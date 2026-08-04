#pragma once
extern "C" {
#include <Protocol/MpService.h>
#include <Uefi.h>
}
#include "zpp/loader.h"
#include "zpp/trace.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace zpp
{
/**
 * The hypervisor self check. Same struct-instead-of-free-functions reason
 * as trace above.
 *
 * What it checks, and why it is shaped the way it is: the loader now
 * virtualizes the boot processor alone, so asking the firmware to run a
 * CPUID on every processor - which is what this used to do - can only ever
 * ask the boot processor, and answers nothing. The processors that matter
 * are the ones the hypervisor has to adopt *itself*, when it sees a guest
 * send the INIT-SIPI-SIPI that starts them.
 *
 * So this check stops being a passenger and becomes the operating system:
 * it enables x2APIC, finds the other processors, sends each one a real
 * INIT-SIPI-SIPI through the interrupt command register, and has the code
 * they start in answer CPUID leaf 0x40000000 for itself. A processor that
 * comes up under the hypervisor spells ZppZppZppZpp; one that came up on
 * bare metal beside it does not, and that difference is the whole test.
 *
 * Each IPI names one processor. That is a requirement rather than a style
 * choice: this VMM redirects a start-up IPI by substituting its own
 * trampoline's vector for the one the guest asked for, which it can only
 * do for a command it can attribute to a processor - so a broadcast is
 * passed through untouched and the processors it starts are never adopted.
 *
 * It is destructive, deliberately. Every application processor the
 * firmware had parked is taken away from it and left halted in real mode,
 * and the boot processor's local APIC is left in x2APIC mode, which the
 * firmware's own xAPIC accesses cannot see. Nothing here is meant to run
 * on a machine that is going to keep booting - hence ZPP_VERIFY_HYPERVISOR
 * and hence this file compiling to nothing without it.
 */
struct verify
{
    /**
     * Whether this build carries the self check. Off by default: it exists
     * a test facility, whether run locally or in CI, and has no place in a
     * loader that is actually being deployed.
     */
    static constexpr bool enabled = ZPP_VERIFY_HYPERVISOR;

    /**
     * cpuid and the MSR accessors written with register constraints rather
     * than reused from zpp/arch/x86_64/asm.h. Those are naked functions
     * that read their arguments from the System V registers, while this
     * loader is built for the Microsoft ABI, where the third argument
     * arrives in r8 rather than rdx - reusing them here would store the
     * results through the second argument's value instead of a pointer.
     * Letting the compiler allocate registers avoids the question, and it
     * is why the MSR accessors below are also spelled out here.
     * @{
     */
    static void query_cpuid(std::uint32_t leaf, std::uint32_t (&out)[4])
    {
        std::uint32_t a{};
        std::uint32_t b{};
        std::uint32_t c{};
        std::uint32_t d{};

        asm volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(0u));

        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }

    /**
     * The same, for a leaf that takes a subleaf in ecx. The diagnostic
     * leaf uses it to select which processor is being asked about, so this
     * cannot go through the version above, which forces ecx to zero.
     */
    static void query_cpuid_ecx(std::uint32_t leaf,
                                std::uint32_t subleaf,
                                std::uint32_t (&out)[4])
    {
        std::uint32_t a{};
        std::uint32_t b{};
        std::uint32_t c{};
        std::uint32_t d{};

        asm volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));

        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }

    static std::uint64_t read_msr(std::uint32_t index)
    {
        std::uint32_t low{};
        std::uint32_t high{};

        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));

        return (static_cast<std::uint64_t>(high) << 32) | low;
    }

    /**
     * The memory clobber is not decoration. The interrupt command register
     * is written through this, and the processor it starts reads a
     * trampoline this one stored moments earlier - so the stores must not
     * be sunk past the write that wakes the reader.
     */
    static void write_msr(std::uint32_t index, std::uint64_t value)
    {
        asm volatile("wrmsr"
                     :
                     : "a"(static_cast<std::uint32_t>(value)),
                       "d"(static_cast<std::uint32_t>(value >> 32)),
                       "c"(index)
                     : "memory");
    }

    /**
     * @}
     */

    /**
     * What the CPUID checks saw, so the caller can print the answer
     * instead of only whether it matched.
     */
    struct hypervisor_signature
    {
        std::uint32_t leaf_1_ecx;
        std::uint32_t signature[4];
    };

    /**
     * The signature bytes a processor under this hypervisor answers with,
     * in ebx, ecx and edx of leaf 0x40000000 - ZppZppZppZpp.
     * @{
     */
    /**
     * The leaf this VMM answers with a processor's most recent exit,
     * selected by ecx. Inside the range it already owns, so nothing
     * underneath can answer it instead.
     */
    static constexpr std::uint32_t diagnostic_leaf = 0x40000001;

    static constexpr std::uint32_t signature_ebx = 0x5a70705a;
    static constexpr std::uint32_t signature_ecx = 0x705a7070;
    static constexpr std::uint32_t signature_edx = 0x70705a70;
    /**
     * @}
     */

    /**
     * Asks the hypervisor to identify itself, from whichever CPU this runs
     * on.
     *
     * The vmexit handler answers CPUID leaf 0x40000000 with the signature
     * ZppZppZppZpp and sets the hypervisor present bit in leaf 1. Neither
     * can happen unless vmxon, the VMCS setup, vmlaunch, the exit handler
     * and vmresume all worked on this CPU, so one check covers the whole
     * path end to end.
     */
    static int read_signature(hypervisor_signature & result)
    {
        std::uint32_t registers[4]{};

        // Leaf 1, bit 31 of ecx: a hypervisor is present.
        query_cpuid(1, registers);
        result.leaf_1_ecx = registers[2];

        // Leaf 0x40000000: the vendor signature, in ebx, ecx then edx.
        // Read before the present bit is judged, so a failure still has
        // something to show.
        query_cpuid(1u << 30, registers);
        result.signature[0] = registers[0];
        result.signature[1] = registers[1];
        result.signature[2] = registers[2];
        result.signature[3] = registers[3];

        // The present bit is recorded and reported, but deliberately not
        // required. This VMM leaves it clear so its guest does not know it
        // is virtualized - see the CPUID leaf 1 handling for why - so
        // demanding it here would fail a working hypervisor. The signature
        // below is the real evidence anyway: nothing but this VMM answers
        // leaf 0x40000000 with it, and answering at all proves vmxon, the
        // VMCS, vmlaunch, the exit handler and vmresume all worked.
        if (!signature_matches(result.signature)) {
            return -2;
        }

        return 0;
    }

    /**
     * Whether the three words are the signature this hypervisor answers
     * with. Takes the four register words as read, so the indices match
     * everywhere the signature is handled.
     */
    static constexpr bool signature_matches(const std::uint32_t * words)
    {
        return (signature_ebx == words[1]) &&
               (signature_ecx == words[2]) && (signature_edx == words[3]);
    }

    /**
     * Appends the twelve signature bytes as the text they are meant to
     * spell, which for this hypervisor is ZppZppZppZpp.
     *
     * Non printable bytes are replaced. The bytes come from whatever is
     * answering CPUID, which on a machine running somebody else's
     * hypervisor is arbitrary data, and it must not be able to put control
     * characters into the log.
     */
    static constexpr char *
    append_signature_text(char * out, const std::uint32_t * signature)
    {
        // ebx, then ecx, then edx - the order a CPUID vendor string is
        // assembled in - each little endian.
        for (std::size_t word = 1; word <= 3; ++word) {
            for (std::size_t byte{}; byte < 4; ++byte) {
                auto character = static_cast<char>(
                    (signature[word] >> (byte * 8)) & 0xff);
                *out++ = (character >= 0x20 && character < 0x7f)
                             ? character
                             : '.';
            }
        }
        return out;
    }

    /**
     * IA32_APIC_BASE and the two bits of it this needs. SDM 13.12.1,
     * "Detecting and Enabling x2APIC Mode".
     * @{
     */
    static constexpr std::uint32_t ia32_apic_base = 0x1b;
    static constexpr std::uint64_t apic_base_enabled = 1ull << 11;
    static constexpr std::uint64_t apic_base_extended = 1ull << 10;
    /**
     * @}
     */

    /**
     * The x2APIC MSRs this check uses. The interrupt command register is
     * the reason the check switches modes at all - see enable_x2apic.
     * @{
     */
    static constexpr std::uint32_t ia32_x2apic_apic_id = 0x802;
    static constexpr std::uint32_t ia32_x2apic_icr = 0x830;
    /**
     * @}
     */

    /**
     * Puts this processor's local APIC into x2APIC mode, returning whether
     * it is in that mode afterwards.
     *
     * The test needs this, and the reason is the hypervisor rather than
     * convenience. The only interception it has on the interrupt command
     * register is the MSR one, on 0x830: the firmware boots in xAPIC mode,
     * where that register is a location on the APIC page at base + 0x300
     * and no MSR bitmap can see it, so an INIT-SIPI-SIPI sent in xAPIC
     * mode reaches the hardware without the hypervisor ever being told.
     * That would test the emulator, not the code under test.
     *
     * The x2APIC form is also simpler to send: the destination APIC id
     * lives in bits 63:32 of the same register, so there is no separate
     * destination register to write first, and with the delivery status
     * bit gone there is nothing to poll between IPIs (SDM 13.12.9).
     */
    static bool enable_x2apic()
    {
        auto base = read_msr(ia32_apic_base);

        // Bit 11 clear means the local APIC is globally disabled, and the
        // transition to EXTD=1 with EN=0 is the one the SDM says raises a
        // general protection fault (13.12.5.1). Nothing here is worth
        // taking the boot down for, so leave it alone and report.
        if (!(base & apic_base_enabled)) {
            return false;
        }

        if (base & apic_base_extended) {
            return true;
        }

        write_msr(ia32_apic_base, base | apic_base_extended);

        // Read back rather than assume. This is the one place the check
        // changes machine state before it has anything to report with.
        return 0 != (read_msr(ia32_apic_base) & apic_base_extended);
    }

    /**
     * Sends one interrupt command in the x2APIC form: the whole command in
     * a single MSR write, destination in the upper half.
     *
     * The fence is required rather than cautious. SDM 13.12.3, "MSR Access
     * in x2APIC Mode": "A WRMSR to an APIC register may complete before
     * all preceding stores are globally visible; software can prevent this
     * by inserting a serializing instruction or the sequence
     * MFENCE;LFENCE before the WRMSR." The preceding stores here are the
     * trampoline the woken processor is about to execute.
     */
    static void send_interrupt_command(std::uint32_t destination,
                                       std::uint32_t command)
    {
        asm volatile("mfence; lfence" : : : "memory");
        auto destination_field = static_cast<std::uint64_t>(destination)
                                 << 32;
        write_msr(ia32_x2apic_icr, destination_field | command);
    }

    /**
     * The interrupt command fields this check builds by hand. SDM Figure
     * 13-28, "Interrupt Command Register (ICR) in x2APIC Mode": vector in
     * bits 7:0, delivery mode in 10:8, level in 14.
     *
     * The destination shorthand in bits 19:18 is deliberately absent. It
     * is left zero on every command sent here, which is what makes the
     * destination field in the upper half name the target - and a command
     * whose target can be named is the only kind this VMM can redirect.
     *
     * Level is set for both, which is what firmware and every operating
     * system send - an INIT level de-assert is a delivery mode of its own
     * and is not used here. Bit 12, the delivery status bit of the xAPIC
     * form, is reserved in this one and left clear: WRMSR raises a general
     * protection fault for a reserved bit set to one in an x2APIC register
     * (SDM Table 13-6, note 2).
     * @{
     */
    static constexpr std::uint32_t delivery_init = 5u << 8;
    static constexpr std::uint32_t delivery_start_up = 6u << 8;
    static constexpr std::uint32_t level_assert = 1u << 14;
    /**
     * @}
     */

    /**
     * The trampoline page's layout.
     *
     * One page holds the code and one result slot per processor, each slot
     * selected by the low four bits of the processor's own APIC id. A slot
     * per processor rather than one shared area because the whole point is
     * to hear from each processor separately, and because the fallback
     * path below starts them all at once.
     * @{
     */
    static constexpr std::size_t slot_area_offset = 0x200;
    static constexpr std::size_t slot_size = 0x20;
    static constexpr std::size_t slot_count = 16;
    static constexpr std::size_t slot_apic_id_offset = 0x00;
    static constexpr std::size_t slot_ebx_offset = 0x04;
    static constexpr std::size_t slot_ecx_offset = 0x08;
    static constexpr std::size_t slot_edx_offset = 0x0c;
    static constexpr std::size_t slot_marker_offset = 0x10;

    /**
     * Written last, so a slot whose marker is present has its three
     * signature words already in it. x86 stores retire in order, so no
     * fence is needed on the writing side for that to hold.
     */
    static constexpr std::uint32_t completion_marker = 0xc0debabe;
    /**
     * @}
     */

    /**
     * The code a start-up IPI actually starts: sixteen bit real mode, at
     * offset zero of its own page, with cs based there and every other
     * segment based at zero.
     *
     * A byte array with the assembly beside it rather than a real assembly
     * file, because this project cross compiles a single x86-64 target and
     * teaching the build to also emit a sixteen bit object is not worth
     * what it would cost. Everything here is deliberately short enough to
     * check by hand.
     *
     * ds is loaded from cs because the stores below are into this same
     * page, and a processor coming out of a start-up IPI has ds based at
     * zero while cs is based at the page - the vector is a page number.
     * Nothing touches the stack, so ss and sp are left as found.
     *
     * The slot is chosen from the processor's own initial APIC id, out of
     * leaf 1 ebx bits 31:24, so several processors started at once cannot
     * write over each other. The full id is stored as well: the slot index
     * is only its low four bits, so the stored value is what says which
     * processor a slot belongs to.
     */
    static constexpr std::uint8_t trampoline_code[]{
        0xfa,                               // cli
        0x8c, 0xc8,                         // mov ax, cs
        0x8e, 0xd8,                         // mov ds, ax
        0x66, 0xb8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0x0f, 0xa2,                         // cpuid
        0x66, 0xc1, 0xeb, 0x18,             // shr ebx, 24
        0x66, 0x89, 0xde,                   // mov esi, ebx
        0x83, 0xe3, 0x0f,                   // and bx, 0xf
        0xc1, 0xe3, 0x05,                   // shl bx, 5
        0x89, 0xdf,                         // mov di, bx
        0x66, 0x89, 0xb5, 0x00, 0x02,       // mov [di+0x200], esi
        0x66, 0xb8, 0x00, 0x00, 0x00, 0x40, // mov eax, 0x40000000
        0x66, 0x31, 0xc9,                   // xor ecx, ecx
        0x0f, 0xa2,                         // cpuid
        0x66, 0x89, 0x9d, 0x04, 0x02,       // mov [di+0x204], ebx
        0x66, 0x89, 0x8d, 0x08, 0x02,       // mov [di+0x208], ecx
        0x66, 0x89, 0x95, 0x0c, 0x02,       // mov [di+0x20c], edx
        0x66, 0xc7, 0x85, 0x10, 0x02,       // mov dword [di+0x210],
        0xbe, 0xba, 0xde, 0xc0,             //     0xc0debabe
        0xfa,                               // cli          <- halted:
        0xf4,                               // hlt
        0xeb, 0xfc,                         // jmp halted
    };

    /**
     * The code must not reach the slots it writes into, and the marker in
     * it must be the one the poll loop looks for - it is the last
     * immediate in the array, four bytes before the closing cli, hlt and
     * jmp. Both are checked here rather than trusted, because a byte array
     * has no compiler to tell it otherwise.
     * @{
     */
    static_assert(sizeof(trampoline_code) <= slot_area_offset);
    static constexpr std::size_t marker_immediate_offset =
        sizeof(trampoline_code) - 8;
    static_assert(completion_marker ==
                  (static_cast<std::uint32_t>(
                       trampoline_code[marker_immediate_offset]) |
                   (static_cast<std::uint32_t>(
                        trampoline_code[marker_immediate_offset + 1])
                    << 8) |
                   (static_cast<std::uint32_t>(
                        trampoline_code[marker_immediate_offset + 2])
                    << 16) |
                   (static_cast<std::uint32_t>(
                        trampoline_code[marker_immediate_offset + 3])
                    << 24)));
    /**
     * @}
     */

    /**
     * One 32-bit word out of the trampoline page.
     *
     * Volatile because the page is written by another processor while this
     * one polls it in a loop with nothing else in the body - exactly the
     * shape an optimizer is entitled to hoist out.
     */
    static std::uint32_t page_word(std::span<std::byte> page,
                                   std::size_t offset)
    {
        return *reinterpret_cast<volatile std::uint32_t *>(page.data() +
                                                           offset);
    }

    /**
     * Where the given processor's result slot begins.
     */
    static constexpr std::size_t slot_offset(std::uint32_t apic_id)
    {
        return slot_area_offset + ((apic_id % slot_count) * slot_size);
    }

    /**
     * Which of the two enumeration sources named the processors that were
     * started. Reported, because a run that fell back is a run whose
     * premise is worth seeing rather than inferring.
     */
    enum class processor_source
    {
        none,
        madt,
        mp_services,
    };

    /**
     * The processors an enumeration listed, and where the list came from.
     * The source is a separate answer from the count: no table means
     * nothing was asked, an empty list means the machine says it has one
     * processor.
     */
    struct discovery
    {
        processor_source source{processor_source::none};
        std::size_t count{};
    };

    /**
     * Fills in the local APIC ids of every enabled processor the ACPI
     * multiple APIC description table lists.
     *
     * The configuration table walk and the XSDT walk are the same shape as
     * acpi_timer_advancing in the loader's main - locate the ACPI 2.0 root
     * pointer among the configuration tables, take the XSDT out of it at
     * offset 24, then walk its 64-bit entries looking for a signature.
     * Written here rather than shared because the loader's main is being
     * changed in parallel and this file is the one that owns the check.
     *
     * MP services is deliberately not used. It is the firmware's own idea
     * of the machine's processors, delivered by the firmware starting them
     * for us, and what is under test is precisely whether *we* can start
     * them and be intercepted while doing it.
     */
    static discovery discover_processors(EFI_SYSTEM_TABLE * system_table,
                                         std::span<std::uint32_t> apic_ids)
    {
        constexpr std::uint32_t acpi_20_guid_data1 = 0x8868e871;
        constexpr std::size_t table_header_size = 36;
        constexpr std::size_t xsdt_entry_offset = 36;
        constexpr std::size_t madt_entries_offset = 44;

        // The interrupt controller structure types this cares about. ACPI
        // 6.5, Table 5-21: type 0 is a processor local APIC, type 9 a
        // processor local x2APIC. Both are taken, because a machine with
        // more than 255 processors describes the low ones with either.
        constexpr std::uint8_t local_apic_type = 0;
        constexpr std::uint8_t local_x2apic_type = 9;
        constexpr std::uint8_t local_apic_length = 8;
        constexpr std::uint8_t local_x2apic_length = 16;
        constexpr std::uint32_t processor_enabled = 1u << 0;

        discovery result{};

        auto read_word = [](const unsigned char * from) {
            std::uint32_t value{};
            for (std::size_t byte{}; byte < 4; ++byte) {
                value |= static_cast<std::uint32_t>(from[byte])
                         << (byte * 8);
            }
            return value;
        };

        auto read_quad = [](const unsigned char * from) {
            std::uint64_t value{};
            for (std::size_t byte{}; byte < 8; ++byte) {
                value |= static_cast<std::uint64_t>(from[byte])
                         << (byte * 8);
            }
            return value;
        };

        const unsigned char * rsdp{};
        auto tables = system_table->NumberOfTableEntries;
        for (std::size_t i{}; i < tables; ++i) {
            auto & entry = system_table->ConfigurationTable[i];
            if (entry.VendorGuid.Data1 == acpi_20_guid_data1) {
                rsdp =
                    static_cast<const unsigned char *>(entry.VendorTable);
                break;
            }
        }
        if (!rsdp) {
            return result;
        }

        auto xsdt_address = read_quad(rsdp + 24);
        if (!xsdt_address) {
            return result;
        }

        auto * xsdt =
            reinterpret_cast<const unsigned char *>(xsdt_address);
        auto xsdt_length = read_word(xsdt + 4);
        if (xsdt_length <= table_header_size) {
            return result;
        }

        auto entries =
            (xsdt_length - xsdt_entry_offset) / sizeof(std::uint64_t);
        for (std::size_t i{}; i < entries; ++i) {
            auto table_address = read_quad(xsdt + xsdt_entry_offset +
                                           (i * sizeof(std::uint64_t)));
            if (!table_address) {
                continue;
            }

            auto * madt =
                reinterpret_cast<const unsigned char *>(table_address);
            if ((madt[0] != 'A') || (madt[1] != 'P') || (madt[2] != 'I') ||
                (madt[3] != 'C')) {
                continue;
            }

            // Walked by each structure's own length field, bounded by the
            // table's own length. A zero length would loop forever and a
            // long one would walk off the table, so both stop the walk
            // rather than being trusted - this is firmware supplied data.
            auto madt_length = read_word(madt + 4);
            for (auto offset = madt_entries_offset;
                 (offset + 2) <= madt_length;) {
                auto type = madt[offset];
                auto length = madt[offset + 1];
                if ((length < 2) || ((offset + length) > madt_length)) {
                    break;
                }

                if ((local_apic_type == type) &&
                    (length >= local_apic_length) &&
                    (read_word(madt + offset + 4) & processor_enabled)) {
                    if (result.count < apic_ids.size()) {
                        apic_ids[result.count++] = madt[offset + 3];
                    }
                } else if ((local_x2apic_type == type) &&
                           (length >= local_x2apic_length) &&
                           (read_word(madt + offset + 8) &
                            processor_enabled)) {
                    if (result.count < apic_ids.size()) {
                        apic_ids[result.count++] =
                            read_word(madt + offset + 4);
                    }
                }

                offset += length;
            }

            if (result.count) {
                result.source = processor_source::madt;
            }
            return result;
        }

        return result;
    }

    /**
     * The same list, from the firmware's own enumeration, for machines
     * whose firmware publishes no ACPI tables at all - which the CI
     * machine is, and its loader probe reports as "no rsdp".
     *
     * This is a change of *source* only, and that is the whole reason it
     * is acceptable here. What is under test is whether this hypervisor
     * sees a guest start a processor and adopts it on the way through, so
     * the INIT-SIPI-SIPI below is still ours; only the question "which
     * processors exist, and by what APIC id" is answered by the firmware.
     * EFI_MP_SERVICES_PROTOCOL is not asked to start anything, which is
     * the part that would have tested the firmware instead of this code.
     *
     * What it replaced was a start-up broadcast, and that could never have
     * passed: a broadcast names no destination, so this VMM cannot
     * redirect it one processor at a time and passes it through - handing
     * every processor to the guest unvirtualized. A fallback that is
     * guaranteed to fail is worse than none, because it reports a working
     * hypervisor as broken.
     *
     * UEFI 2.10, EFI_MP_SERVICES_PROTOCOL.GetProcessorInfo():
     * "For IA32 and X64, the processor ID is the same as the Local APIC
     * ID", and PROCESSOR_ENABLED_BIT in StatusFlag says the processor is
     * usable on this boot.
     */
    static discovery discover_processors_from_mp_services(
        EFI_SYSTEM_TABLE * system_table, std::span<std::uint32_t> apic_ids)
    {
        discovery result{};

        EFI_GUID guid = EFI_MP_SERVICES_PROTOCOL_GUID;
        EFI_MP_SERVICES_PROTOCOL * mp_services{};
        if (EFI_ERROR(system_table->BootServices->LocateProtocol(
                &guid,
                nullptr,
                reinterpret_cast<void **>(&mp_services)))) {
            return result;
        }

        UINTN processors{};
        UINTN enabled{};
        if (EFI_ERROR(mp_services->GetNumberOfProcessors(
                mp_services, &processors, &enabled))) {
            return result;
        }

        for (UINTN i{};
             (i < processors) && (result.count < apic_ids.size());
             ++i) {
            EFI_PROCESSOR_INFORMATION information{};
            if (EFI_ERROR(mp_services->GetProcessorInfo(
                    mp_services, i, &information))) {
                continue;
            }
            if (!(information.StatusFlag & PROCESSOR_ENABLED_BIT)) {
                continue;
            }
            apic_ids[result.count++] =
                static_cast<std::uint32_t>(information.ProcessorId);
        }

        if (result.count) {
            result.source = processor_source::mp_services;
        }
        return result;
    }

    /**
     * Runs the check and reports it, over serial and through the UEFI
     * console when there is one.
     *
     * The loader parameters are no longer looked at. They carried
     * call_on_cpu and number_of_cpus, which is how this used to reach
     * another processor - and both of those are the firmware's answer
     * rather than the hypervisor's. Kept in the signature so the caller
     * does not have to change.
     */
    static bool present(EFI_SYSTEM_TABLE * system_table,
                        const zpp_loader_parameters &)
    {
        // Shared with trace, so a line built here and a line built there
        // cannot disagree about how much room there is.
        constexpr std::size_t line_capacity = trace::line_capacity;

        auto * boot_services = system_table->BootServices;

        // char16_t and u"" literals rather than EDK2's CHAR16 and L"":
        // char16_t is the standard type that actually means what this
        // needs, and it is what the string literals below are. CHAR16 is a
        // typedef for unsigned short, a distinct type of the same size and
        // alignment, so the reinterpret_cast is confined to the one call
        // that crosses into the firmware.
        auto report = [&](const char * ascii, const char16_t * wide) {
            trace::raw(ascii);
            if (system_table->ConOut) {
                system_table->ConOut->OutputString(
                    system_table->ConOut,
                    reinterpret_cast<CHAR16 *>(
                        const_cast<char16_t *>(wide)));
            }
        };

        // Same two channels, for a line built at runtime. Widening byte by
        // byte is enough because everything printed here is ASCII by
        // construction.
        auto report_line = [&](const char * ascii) {
            trace::raw(ascii);
            if (!system_table->ConOut) {
                return;
            }
            char16_t wide[line_capacity]{};
            std::size_t i{};
            for (; ascii[i] && (i < (line_capacity - 1)); ++i) {
                wide[i] = static_cast<char16_t>(
                    static_cast<unsigned char>(ascii[i]));
            }
            system_table->ConOut->OutputString(
                system_table->ConOut, reinterpret_cast<CHAR16 *>(wide));
        };

        // What CPUID answered for one processor, pass or fail. The point
        // is to be able to read the signature rather than trust a compare,
        // and a mismatch is exactly the case where the bytes matter most.
        auto report_signature = [&](std::size_t index,
                                    std::uint32_t apic_id,
                                    const hypervisor_signature & found) {
            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, index);
            end = trace::append_text(end, " apic_id=");
            end = trace::append_hex(end, apic_id, 8);
            end = trace::append_text(end, " leaf 1 ecx=");
            end = trace::append_hex(end, found.leaf_1_ecx, 8);
            end = trace::append_text(end, " hypervisor_bit=");
            end = trace::append_text(
                end, (found.leaf_1_ecx & (1u << 31)) ? "1" : "0");
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);

            end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, index);
            end = trace::append_text(end, " leaf 0x40000000 ebx=");
            end = trace::append_hex(end, found.signature[1], 8);
            end = trace::append_text(end, " ecx=");
            end = trace::append_hex(end, found.signature[2], 8);
            end = trace::append_text(end, " edx=");
            end = trace::append_hex(end, found.signature[3], 8);
            end = trace::append_text(end, " text=\"");
            end = append_signature_text(end, found.signature);
            end = trace::append_text(end, "\"\r\n");
            *end = 0;
            report_line(line);
        };

        auto fail = [&](const char * ascii, const char16_t * wide) {
            report(ascii, wide);
            return false;
        };

        // Step one, unchanged: the boot processor is a guest of ours, and
        // if it is not then nothing below can mean anything.
        hypervisor_signature boot{};
        auto boot_result = read_signature(boot);
        std::uint32_t boot_identity[4]{};
        query_cpuid(1, boot_identity);
        auto boot_apic_id = boot_identity[1] >> 24;
        report_signature(0, boot_apic_id, boot);
        if (0 != boot_result) {
            return fail("zpp: ZPP_HYPERVISOR_FAILED on the boot cpu\r\n",
                        u"zpp: ZPP_HYPERVISOR_FAILED on the boot cpu\r\n");
        }

        // Step two: become able to send an INIT-SIPI-SIPI the hypervisor
        // can see. See enable_x2apic for why this is not optional.
        if (!enable_x2apic()) {
            return fail(
                "zpp: ZPP_HYPERVISOR_FAILED no x2apic to send with\r\n",
                u"zpp: ZPP_HYPERVISOR_FAILED no x2apic to send with\r\n");
        }

        // In x2APIC mode the local APIC id is a 32-bit MSR rather than the
        // eight bits leaf 1 reports, and it is what the other processors
        // are matched against, so take it again from there.
        boot_apic_id =
            static_cast<std::uint32_t>(read_msr(ia32_x2apic_apic_id));

        // Step three: a page for the code the started processors run. It
        // has to be below 1 MiB and page aligned, because a start-up IPI
        // carries a page number in eight bits and the processor begins
        // executing at cs = vector << 8, ip = 0.
        //
        // Reserved memory rather than loader data: nothing in the firmware
        // or in whatever boots next is allowed to hand out a reserved
        // range, and the processors left halted in this page keep
        // executing out of it long after this function has returned.
        EFI_PHYSICAL_ADDRESS trampoline_address = 0x100000;
        if (EFI_ERROR(boot_services->AllocatePages(AllocateMaxAddress,
                                                   EfiReservedMemoryType,
                                                   1,
                                                   &trampoline_address))) {
            return fail(
                "zpp: ZPP_HYPERVISOR_FAILED no page below 1mb\r\n",
                u"zpp: ZPP_HYPERVISOR_FAILED no page below 1mb\r\n");
        }

        std::span<std::byte> page{
            reinterpret_cast<std::byte *>(trampoline_address),
            EFI_PAGE_SIZE};
        for (auto & byte : page) {
            byte = std::byte{};
        }
        for (std::size_t i{}; i < sizeof(trampoline_code); ++i) {
            page[i] = static_cast<std::byte>(trampoline_code[i]);
        }

        auto vector = static_cast<std::uint32_t>(trampoline_address >> 12);
        {
            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: trampoline at ");
            end = trace::append_hex(end, trampoline_address, 16);
            end = trace::append_text(end, " vector=");
            end = trace::append_hex(end, vector, 2);
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);
        }

        // Step four: find the other processors. The MADT is the operating
        // system's own source for this, and it is what a guest would use,
        // so it is asked first. Where there is no ACPI table at all the
        // firmware's own enumeration answers the same question - see
        // discover_processors_from_mp_services for why substituting it
        // leaves what is under test alone.
        std::uint32_t discovered[slot_count]{};
        auto listed = discover_processors(system_table, discovered);
        if (!listed.count) {
            listed = discover_processors_from_mp_services(system_table,
                                                          discovered);
        }

        std::uint32_t targets[slot_count]{};
        std::size_t target_count{};
        for (std::size_t i{}; i < listed.count; ++i) {
            if (discovered[i] == boot_apic_id) {
                continue;
            }
            targets[target_count++] = discovered[i];
        }

        {
            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: processors from ");
            switch (listed.source) {
            case processor_source::madt:
                end = trace::append_text(end, "the acpi madt");
                break;
            case processor_source::mp_services:
                end = trace::append_text(end, "mp services");
                break;
            case processor_source::none:
                end = trace::append_text(end, "nowhere");
                break;
            }
            end = trace::append_text(end, ", listed=");
            end = trace::append_decimal(end, listed.count);
            end = trace::append_text(end, " targets=");
            end = trace::append_decimal(end, target_count);
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);
        }

        // Nothing to start means nothing to test, and it is a failure of
        // the check rather than of the hypervisor - so it says which,
        // instead of reporting a hypervisor that was never asked anything.
        if (!target_count) {
            return fail("zpp: ZPP_HYPERVISOR_FAILED no cpu to start\r\n",
                        u"zpp: ZPP_HYPERVISOR_FAILED no cpu to start\r\n");
        }

        // Both delays are the ones an operating system uses, and both come
        // from the same place: the MP initialization sequence wants 10
        // milliseconds after the INIT and 200 microseconds after the first
        // start-up IPI. Stall is the firmware's microsecond delay, and it
        // is usable here because it is built on the ACPI power management
        // timer rather than on the local APIC this has just reconfigured.
        constexpr std::size_t init_delay_microseconds = 10000;
        constexpr std::size_t start_up_delay_microseconds = 200;

        // One processor at a time, by name. A destination shorthand is
        // deliberately not used even where it would be shorter: this VMM
        // can only redirect a start-up IPI it can attribute to a
        // processor, so a broadcast is passed through and the processors
        // it starts are never adopted. Naming each one is also what an
        // operating system does.
        auto send_sequence = [&](std::uint32_t destination) {
            send_interrupt_command(destination,
                                   level_assert | delivery_init);
            boot_services->Stall(init_delay_microseconds);
            send_interrupt_command(
                destination, level_assert | delivery_start_up | vector);
            boot_services->Stall(start_up_delay_microseconds);
            send_interrupt_command(
                destination, level_assert | delivery_start_up | vector);
        };

        // The hypervisor's own account of the sequence, sampled either
        // side of it with nothing in between on the second one.
        //
        // The diagnostic leaf reports this processor's exit count and its
        // most recent exit, and the exit that matters is the write that
        // sent the last start-up IPI: reason 0x20, wrmsr. Seeing it there
        // is the only evidence from inside the guest that the interrupt
        // command register write was intercepted at all rather than going
        // straight to the hardware - and in xAPIC mode it would have gone
        // straight to the hardware, which is what enable_x2apic is for.
        //
        // Sampled before as well, because the count alone says nothing:
        // firmware code keeps running on this processor between the IPIs,
        // out of its own timer interrupt, and exits while it does.
        std::uint32_t before[4]{};
        std::uint32_t after[4]{};
        query_cpuid_ecx(diagnostic_leaf, 0, before);

        for (std::size_t i{}; i < target_count; ++i) {
            send_sequence(targets[i]);
        }

        query_cpuid_ecx(diagnostic_leaf, 0, after);

        // Step five: wait for them, bounded, the same shape as the wait
        // in the loader's call_on_cpu. Generous, because this is not a
        // latency budget: a processor that has not answered in a second is
        // not late, it is not coming.
        constexpr std::size_t answer_poll_microseconds = 1000;
        constexpr std::size_t answer_polls = 1000;
        for (std::size_t poll{}; poll < answer_polls; ++poll) {
            std::size_t answered{};
            for (std::size_t i{}; i < target_count; ++i) {
                if (completion_marker ==
                    page_word(page,
                              slot_offset(targets[i]) +
                                  slot_marker_offset)) {
                    ++answered;
                }
            }
            if (answered == target_count) {
                break;
            }
            boot_services->Stall(answer_poll_microseconds);
        }

        // Both samples, printed here rather than where they were taken:
        // reporting goes through the firmware's console, which runs enough
        // code on this processor to bury the exit being looked at.
        {
            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: cpu 0 exits ");
            end = trace::append_hex(end, before[0], 8);
            end = trace::append_text(end, " -> ");
            end = trace::append_hex(end, after[0], 8);
            end = trace::append_text(end, " last_reason ");
            end = trace::append_hex(end, before[1], 8);
            end = trace::append_text(end, " -> ");
            end = trace::append_hex(end, after[1], 8);
            end = trace::append_text(end, " qual=");
            end = trace::append_hex(end, after[2], 8);
            end = trace::append_text(end, " flags=");
            end = trace::append_hex(end, after[3], 8);
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);
        }

        // Step six: report each of them, and judge only after every one
        // has been printed. A run that stops at the first failure hides
        // the difference between one broken processor and all of them.
        auto all_answered = true;
        for (std::size_t i{}; i < target_count; ++i) {
            auto base = slot_offset(targets[i]);
            auto marker = page_word(page, base + slot_marker_offset);

            hypervisor_signature answer{};
            answer.signature[1] = page_word(page, base + slot_ebx_offset);
            answer.signature[2] = page_word(page, base + slot_ecx_offset);
            answer.signature[3] = page_word(page, base + slot_edx_offset);

            // The slot's own idea of who wrote it. Only the low four bits
            // of an APIC id pick the slot, so two processors 16 apart
            // share one - and then the second one to arrive would answer
            // for the first. Reported, and required to match below, so a
            // collision fails loudly instead of passing twice.
            auto owner = page_word(page, base + slot_apic_id_offset);

            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, i + 1);
            end = trace::append_text(end, " apic_id=");
            end = trace::append_hex(end, targets[i], 8);
            end = trace::append_text(end, " started=");
            end = trace::append_text(
                end, (completion_marker == marker) ? "1" : "0");
            end = trace::append_text(end, " slot_apic_id=");
            end = trace::append_hex(end, owner, 8);
            end = trace::append_text(end, " leaf 0x40000000 ebx=");
            end = trace::append_hex(end, answer.signature[1], 8);
            end = trace::append_text(end, " ecx=");
            end = trace::append_hex(end, answer.signature[2], 8);
            end = trace::append_text(end, " edx=");
            end = trace::append_hex(end, answer.signature[3], 8);
            end = trace::append_text(end, " text=\"");
            end = append_signature_text(end, answer.signature);
            end = trace::append_text(end, "\"\r\n");
            *end = 0;
            report_line(line);

            // And what the hypervisor itself recorded under that index,
            // which is a second and independent question: the signature
            // above says a processor is virtualized, this says the VMM's
            // own per-processor state for it is where the check thinks it
            // is. Those are not the same index by construction - the VMM
            // indexes by its virtual processor id less one, while this
            // check counts the order it started them in - so they agree
            // only if allocation followed the same order, and that is
            // worth measuring rather than assuming.
            //
            // A nonzero exit count with the started-by-start-up-IPI flag
            // set is what agreement looks like: bit 2 of the flags word is
            // that flag, and it is per processor. Bits 0 and 1 are not -
            // they say *some* processor stopped on an unhandled exit or a
            // failed VM entry - which is still a failure of this run, so
            // they are judged here rather than only printed.
            std::uint32_t recorded[4]{};
            query_cpuid_ecx(diagnostic_leaf,
                            static_cast<std::uint32_t>(i + 1),
                            recorded);

            end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, i + 1);
            end = trace::append_text(end, " vmm exits ");
            end = trace::append_hex(end, recorded[0], 8);
            end = trace::append_text(end, " last_reason ");
            end = trace::append_hex(end, recorded[1], 8);
            end = trace::append_text(end, " qual=");
            end = trace::append_hex(end, recorded[2], 8);
            end = trace::append_text(end, " flags=");
            end = trace::append_hex(end, recorded[3], 8);
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);

            constexpr std::uint32_t stopped_flags = 0x3;
            constexpr std::uint32_t started_by_start_up_ipi = (1u << 2);
            if (!recorded[0] || !(recorded[3] & started_by_start_up_ipi) ||
                (recorded[3] & stopped_flags)) {
                all_answered = false;
            }

            if ((completion_marker != marker) || (owner != targets[i]) ||
                !signature_matches(answer.signature)) {
                all_answered = false;
            }
        }

        if (!all_answered) {
            return fail(
                "zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n",
                u"zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n");
        }

        report("zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n",
               u"zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n");
        return true;
    }
};

} // namespace zpp
