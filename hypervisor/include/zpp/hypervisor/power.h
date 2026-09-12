#pragma once
#include <cstddef>
#include <cstdint>

/**
 * What a guest power transition costs this VMM, and what it would take to
 * survive one.
 *
 * The guest asks for a sleep state by writing the ACPI PM1 control
 * register, which the loader locates and hands over. That write is the
 * only warning there is: after it the platform removes power from the
 * processors, and nothing about VMX operation survives that.
 *
 * There are three distinct transitions behind one register, and they need
 * different things of us:
 *
 * - **S3, suspend to RAM.** Memory is preserved; processor state is not.
 *   Every processor leaves VMX operation the hard way, the VMCS pointer
 *   and CR4.VMXE are gone, and IA32_FEATURE_CONTROL is back to zero -
 *   "This MSR is cleared to zero when a logical processor is reset" (SDM
 *   26.7). Everything this VMM keeps in memory is still there and still
 *   correct, because it describes physical memory that has not moved: the
 *   host page table, the host GDT and IDT, the extended page tables, the
 *   module itself. So an S3 resume is not a re-launch; it is a
 *   re-establishment of processor state around memory that is already
 *   right.
 *
 * - **S4, hibernate.** The guest writes memory to disk and the platform
 *   powers off. A resume is a full firmware boot followed by the boot
 *   manager restoring that image, so from this VMM's point of view S4 is
 *   S5 plus an ordinary cold boot - there is no state to carry across and
 *   nothing to re-establish, because the loader runs again from the
 *   beginning. What matters for S4 is only that the image does not contain
 *   or expect our memory, which is why the module is allocated as
 *   EfiReservedMemoryType (see the comment on allocate_rwx in
 *   uefi_loader/src/main.cpp) rather than anything carrying
 *   EFI_MEMORY_RUNTIME.
 *
 * - **S5, power off, and a warm reset.** Stop cleanly. Nothing comes back.
 *
 * The register cannot tell them apart. SLP_TYPx is a platform-specific
 * value produced by evaluating the \\_Sx objects in the DSDT, and this VMM
 * has no AML interpreter and will not be getting one - so a write with
 * SLP_EN set means "some sleep state", and the type is recorded for a
 * human to read rather than acted on.
 *
 * That turns out not to matter, because the *entry* side is identical for
 * all three: flush anything staged in the diagnostic channel, leave the
 * storage controller in a state firmware can re-initialise, VMCLEAR every
 * VMCS this processor holds and then VMXOFF. Only the resume differs, and
 * S4 and S5 have no resume path of their own to write.
 *
 * The VMCLEAR is not tidiness. SDM 27.11.1: "If a logical processor leaves
 * VMX operation, any VMCSs active on that logical processor may be
 * corrupted... To prevent such corruption of a VMCS that may be used
 * either after a return to VMX operation or on another logical processor,
 * software should execute VMCLEAR for that VMCS before executing the
 * VMXOFF instruction or removing power from the processor (e.g., as part
 * of a transition to the S3 and S4 power states)." That is the SDM naming
 * this exact case.
 */
namespace zpp::hypervisor::power
{
/**
 * The ACPI PM1 control register, as far as a sleep request is concerned.
 *
 * SLP_TYPx in bits 12:10 and SLP_EN in bit 13, from the PM1 control
 * register description in the ACPI specification; the field positions are
 * the same in every revision that has the register. Cross-checked against
 * the FACS and FADT layouts EDK2 declares in
 * MdePkg/Include/IndustryStandard/Acpi65.h, which this build already
 * fetches, since those are what the loader's finder is written against.
 * @{
 */
inline constexpr std::uint32_t sleep_type_shift = 10;
inline constexpr std::uint32_t sleep_type_mask = 7u << sleep_type_shift;
inline constexpr std::uint32_t sleep_enable = 1u << 13;
/**
 * @}
 */

/**
 * The firmware ACPI control structure, as far as a waking vector is
 * concerned.
 *
 * It is the one ACPI table with no revision, checksum or OEM identifier,
 * so only the signature and the length are where they are in every other
 * table. Checked against the FACS structure EDK2 declares in
 * MdePkg/Include/IndustryStandard/Acpi65.h, which this build fetches:
 * signature, length, hardware signature, then the thirty two bit waking
 * vector, then the global lock, the flags, and the sixty four bit vector.
 *
 * `minimum_length` is what has to be there before either vector field may
 * be read *or written*, and it is the end of the extended field rather
 * than the end of the thirty two bit one. The difference matters because
 * the two sides of this are not symmetric: a read past a short table
 * returns a number nobody wrote, which is merely wrong, while a write past
 * one lands in whatever the firmware put next to the table.
 * @{
 */
inline constexpr std::uint32_t facs_signature = 0x53434146; // 'FACS'
inline constexpr std::size_t facs_length_offset = 0x04;
inline constexpr std::size_t facs_waking_vector_offset = 0x0c;
inline constexpr std::size_t facs_extended_waking_vector_offset = 0x18;
inline constexpr std::size_t facs_minimum_length =
    facs_extended_waking_vector_offset + sizeof(std::uint64_t);
/**
 * @}
 */

/**
 * Whether a value written to the PM1 control register asks for a sleep
 * state, rather than being one of the many writes that touch the other
 * bits in the same register and enter nothing.
 *
 * The register also carries SCI_EN, BM_RLD and GBL_RLS, and an operating
 * system writes it during ordinary running. Only SLP_EN starts a
 * transition.
 */
constexpr bool asks_for_sleep(std::uint32_t value)
{
    return 0 != (value & sleep_enable);
}

/**
 * The SLP_TYPx field, which names *which* sleep state on this platform and
 * nothing at all on any other. Recorded, never compared against a
 * constant - see the note above about AML.
 */
constexpr std::uint32_t sleep_type(std::uint32_t value)
{
    return (value & sleep_type_mask) >> sleep_type_shift;
}

/**
 * Whether this VMM quiesces itself before the platform removes power,
 * instead of passing the guest's write straight through and being taken
 * away mid-flight.
 *
 * Off, and the reason is that switching it on changes who executes the
 * OUT. Passing the write through costs nothing and cannot disagree with
 * what the guest meant: the port interception is released and the guest
 * re-executes its own instruction. Quiescing means this VMM performs the
 * write itself, from root mode, after taking itself apart - and if any
 * step of that hangs, the machine neither sleeps nor wakes and somebody
 * has to be at it to hold the power button. On a shared machine that is
 * the expensive failure.
 *
 * What would turn it on: one run on hardware that reaches the
 * "quiesced for sleep" line in the channel and then actually suspends and
 * resumes. Until that has happened once, the honest default is the
 * behaviour that has been observed to boot.
 */
inline constexpr bool quiesce_on_sleep = false;

/**
 * Whether this VMM reads the firmware waking vector out of the FACS on the
 * way into a sleep state.
 *
 * Read only: it writes nothing, changes no behaviour, and cannot make a
 * suspend or a resume go differently. It exists because everything a
 * resume path could do rests on one unverified fact - that the guest
 * leaves its own resume entry point in that table, where this VMM can find
 * it - and that fact is checkable on its own, with a log line, before
 * anything is built on it.
 *
 * Off anyway, because nothing arrives on by default here before it has
 * been run once, and this one does touch guest memory through the mapping
 * window from inside a VM exit. What would turn it on: a hardware run that
 * suspends and reports a non-zero "guest waking vector" line. That result
 * is also what would justify writing to the field, and its absence is what
 * would stop the whole approach - a guest that leaves no vector cannot be
 * resumed into, and the answer would have to be somewhere else entirely.
 */
inline constexpr bool observe_waking_vector = false;

/**
 * Whether this VMM takes over the ACPI firmware waking vector, so that an
 * S3 resume comes back through it rather than straight into the guest's
 * own resume trampoline on bare hardware.
 *
 * The path behind it exists now - hypervisor::arm_resume_from_sleep on the
 * way down and zpp_resume_from_sleep_main on the way up - and this switch
 * is off because it has never been run, not because there is nothing to
 * run. It is the last of the three for a reason: the failure modes are not
 * comparable. A machine that resumes unvirtualized has lost this VMM and
 * kept its guest. A machine pointed at a trampoline that does not finish
 * has lost both, and looks exactly like a dead motherboard from the
 * outside - so this one needs somebody at the machine, and the two
 * switches above answer their own questions first and safely.
 *
 * What would turn it on: a hardware run that suspends and comes back with
 * the guest still virtualized, with resume_request.stage reaching
 * `launched` in the channel. See BACKLOG.md item 7 for the experiments in
 * order.
 */
inline constexpr bool resume_from_waking_vector = false;

/**
 * The resume needs the quiesce, and the dependency is asserted rather than
 * left to be discovered.
 *
 * Not because the resume path could not cope with a processor that was
 * taken away mid-flight - it VMCLEARs and rebuilds every region it uses,
 * so it could - but because the *guest's* write is what sleeps the
 * machine, and on the pass-through path that write is re-executed by the
 * guest after the port interception has been released. Releasing it is
 * what makes the suspend seen once, and a resume that re-arms nothing
 * would then miss the next one. Arming a resume is therefore only
 * meaningful on the path where this VMM performs the write itself and
 * keeps the port.
 */
static_assert(!resume_from_waking_vector || quiesce_on_sleep,
              "resume_from_waking_vector requires quiesce_on_sleep");

/**
 * Reading the vector is a precondition of taking it over, and this keeps
 * the two switches from disagreeing about whether it was read.
 *
 * The resume path reads the FACS itself rather than relying on this - it
 * has to, since it also writes to it and must re-check the table it is
 * about to modify - but observe_waking_vector is what puts the value on
 * the wire, and a resume attempt with no log line saying what the guest's
 * own vector was is one whose failure cannot be read afterwards.
 */
static_assert(!resume_from_waking_vector || observe_waking_vector,
              "resume_from_waking_vector requires observe_waking_vector");

} // namespace zpp::hypervisor::power
