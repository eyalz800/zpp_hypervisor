// A synthetic nested VT-d unit for hvix64, so VBS secure-DMA succeeds with
// kernel DMA protection functional rather than disabled. See
// `nested_vmx::nested_vtd` for the full rationale and the rig measurements
// that ground it, and `.references/nested-vtd-design.md` for the design.
//
// The one lever is a live MMIO read: `HvpComputeIommuFeatureSet` rejects
// QEMU's ECAP (IR=0, the rig's `intremap=off`), so `HvCallAttachDevice`
// (0x82) soft-fails 0x1e at its feature gate before the unit is ever
// programmed. zpp full-traps the DRHD register page in its L1 (hvix64) EPT
// and answers reads from a synthetic unit whose ECAP sets IR - the one bit
// that makes the gate pass. Writes drive a software-shadow global status
// and the invalidation queue; the invalidation-wait descriptor's
// status-write is performed here, which is the completion the securekernel
// polls. QEMU's real unit is never touched (M1): the rig's mtree shows
// every device in `vtd-nodmar` bypass and the disk DMAs today, so leaving
// it alone keeps the passed-through NVMe alive.

#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"

#include "zpp/arch/x86_64/instruction.h"

#include <cstdint>
#include <span>

namespace zpp::hypervisor
{

std::expected<void, zpp::error> hypervisor::setup_nested_vtd()
{
    if constexpr (!nested_vmx::nested_vtd) {
        return {};
    } else {
        // QEMU q35 `intel-iommu` publishes its DRHD register block here,
        // and the rig's DMAR names exactly this base (confirmed by reading
        // VER=0x10 at it directly). One unit on this platform.
        constexpr std::uint64_t drhd_base = 0xfed90000;

        // Full-trap: reads must return the synthetic CAP/ECAP (they differ
        // from QEMU's), so unlike a write-watch the page cannot stay
        // read-mapped. `epte_for` splits the 2 MB identity large page to
        // 4 KB for this one register page.
        auto entry = epte_for(drhd_base);
        if (!entry) {
            return std::unexpected(entry.error());
        }

        auto & epte = **entry;
        epte.read(false);
        epte.write(false);
        epte.execute(false);
        epte.execute_user(false);
        invalidate_ept();

        this->dmar_register_page = drhd_base >> 12;

        log("nested vt-d armed: DRHD register page {} trapped, "
            "synthetic ecap {}",
            this->dmar_register_page,
            this->dmar.extended_capability);

        return {};
    }
}

std::uint64_t hypervisor::dmar_register_read(std::uint64_t offset,
                                             std::size_t size)
{
    // The full register is returned and the emulator truncates it to the
    // access width, so a 32-bit read of a 64-bit register's low dword and a
    // 64-bit read both come out right. High dwords are named by their own
    // offset (e.g. CAP hi at 0x0c) for the aligned 32-bit case.
    (void)size;

    switch (offset) {
    case 0x00: // VER
        return this->dmar.version;
    case 0x08: // CAP (64)
        return this->dmar.capability;
    case 0x0c: // CAP hi
        return this->dmar.capability >> 32;
    case 0x10: // ECAP (64)
        return this->dmar.extended_capability;
    case 0x14: // ECAP hi
        return this->dmar.extended_capability >> 32;
    case 0x18: // GCMD (write-1-to-set; reads the last written shadow)
        return this->dmar.global_command;
    case 0x1c: // GSTS
        return this->dmar.global_status;
    case 0x20: // RTADDR (64)
        return this->dmar.root_table_address;
    case 0x24:
        return this->dmar.root_table_address >> 32;
    case 0x34: // FSTS
        return this->dmar.fault_status;
    case 0x38: // FECTL
        return this->dmar.fault_event_control;
    case 0x3c: // FEDATA
        return this->dmar.fault_event_data;
    case 0x40: // FEADDR (64)
        return this->dmar.fault_event_address;
    case 0x44:
        return this->dmar.fault_event_address >> 32;
    case 0x80: // IQH
        return this->dmar.invalidation_queue_head;
    case 0x88: // IQT
        return this->dmar.invalidation_queue_tail;
    case 0x90: // IQA (64)
        return this->dmar.invalidation_queue_address;
    case 0x94:
        return this->dmar.invalidation_queue_address >> 32;
    case 0xb8: // IRTA (64) - interrupt remap table address, absorbed
        return this->dmar.interrupt_remap_table_address;
    case 0xbc:
        return this->dmar.interrupt_remap_table_address >> 32;
    default:
        // Everything else (reserved, advanced fault logging, etc.) reads
        // zero. hvix64 self-discovers offsets from CAP/ECAP and the
        // synthetic capabilities keep it inside the registers above.
        return 0;
    }
}

void hypervisor::dmar_register_write(std::size_t cpu,
                                     std::uint64_t offset,
                                     std::uint64_t value,
                                     std::size_t size)
{
    auto low = static_cast<std::uint32_t>(value);

    auto latch_low = [&](std::uint64_t & reg) {
        if (size >= 8) {
            reg = value;
        } else {
            reg = (reg & 0xffffffff00000000ull) | low;
        }
    };
    auto latch_high = [&](std::uint64_t & reg) {
        reg = (reg & 0x00000000ffffffffull) | (value << 32);
    };

    switch (offset) {
    case 0x18: { // GCMD - write-1-to-set, one-shot; mirror into GSTS
        this->dmar.global_command = low;

        // Global command bit -> global status bit (Intel VT-d 10.4.4/10.4.5):
        // TE 31 -> TES 31, SRTP 30 -> RTPS 30, QIE 26 -> QIES 26,
        // IRE 25 -> IRES 25, SIRTP 24 -> IRTPS 24. WBF 27 self-clears.
        auto & status = this->dmar.global_status;
        if (0 != (low & (1u << 31))) {
            status |= (1u << 31);
        }
        if (0 != (low & (1u << 30))) {
            status |= (1u << 30);
        }
        if (0 != (low & (1u << 26))) {
            status |= (1u << 26);
        }
        // Interrupt remapping is advertised (ECAP.IR) only to pass the
        // feature gate; enabling it here sets the status hvix64 polls but
        // is never forwarded to QEMU's real unit, which stays intremap=off.
        if (0 != (low & (1u << 25))) {
            status |= (1u << 25);
        }
        if (0 != (low & (1u << 24))) {
            status |= (1u << 24);
        }
        if (0 != (low & (1u << 27))) {
            status &= ~(1u << 27); // WBF completes immediately
        }
        break;
    }
    case 0x1c: // GSTS is read-only
        break;
    case 0x20:
        latch_low(this->dmar.root_table_address);
        break;
    case 0x24:
        latch_high(this->dmar.root_table_address);
        break;
    case 0x34: // FSTS - write-1-to-clear
        this->dmar.fault_status &= ~low;
        break;
    case 0x38:
        this->dmar.fault_event_control = low;
        break;
    case 0x3c:
        this->dmar.fault_event_data = low;
        break;
    case 0x40:
        latch_low(this->dmar.fault_event_address);
        break;
    case 0x44:
        latch_high(this->dmar.fault_event_address);
        break;
    case 0x80: // IQH is read-only status; ignore
        break;
    case 0x88: // IQT doorbell - the one write that does work
        this->dmar.invalidation_queue_tail = low;
        drain_qi_ring(cpu);
        break;
    case 0x90:
        latch_low(this->dmar.invalidation_queue_address);
        break;
    case 0x94:
        latch_high(this->dmar.invalidation_queue_address);
        break;
    case 0xb8:
        latch_low(this->dmar.interrupt_remap_table_address);
        break;
    case 0xbc:
        latch_high(this->dmar.interrupt_remap_table_address);
        break;
    default:
        break;
    }
}

void hypervisor::drain_qi_ring(std::size_t cpu)
{
    // IQA: bits 12-63 queue base (4 KB aligned), bits 0-2 queue size QS.
    // With 128-bit descriptors the queue holds 256 << QS of them.
    auto iqa = this->dmar.invalidation_queue_address;
    auto base = iqa & ~std::uint64_t{0xfff};
    auto queue_size = static_cast<std::uint32_t>(256u << (iqa & 0x7));
    auto queue_bytes = queue_size * 16u;
    if (0 == base || 0 == queue_bytes) {
        return;
    }

    // IQH/IQT hold a byte offset (128-bit aligned) into the queue.
    auto head = this->dmar.invalidation_queue_head & (queue_bytes - 1);
    auto tail = this->dmar.invalidation_queue_tail & (queue_bytes - 1);

    // Bound the walk by the queue length so a malformed tail cannot spin.
    for (std::uint32_t steps{}; (head != tail) && (steps < queue_size);
         ++steps) {
        // The descriptor lives at an hvix64 physical (root-partition GPA),
        // which zpp identity-maps, so it is a plain L1 read.
        std::uint64_t descriptor[2]{};
        if (read_guest_physical(
                base + head,
                std::as_writable_bytes(std::span(descriptor)))) {
            if (cpu < max_cpus) {
                this->dmar_qi_descriptors[cpu] += 1;
            }

            // Invalidation-wait (type 5): the only descriptor that needs
            // action. `sw` (bit 5) asks for a status write of the 32-bit
            // data in bits 32-63 to the dword-aligned address in the
            // upper qword. Performing it is exactly the completion
            // HvpWriteContextEntry polls. Context (1) and IOTLB (2)
            // invalidations are acknowledged by draining, nothing more -
            // the minimal front-end holds no shadow table.
            constexpr std::uint64_t type_mask = 0xf;
            constexpr std::uint64_t status_write = 1ull << 5;
            if (5 == (descriptor[0] & type_mask)) {
                if (0 != (descriptor[0] & status_write)) {
                    auto status_data =
                        static_cast<std::uint32_t>(descriptor[0] >> 32);
                    auto status_address =
                        descriptor[1] & ~std::uint64_t{0x3};
                    if (0 != status_address &&
                        write_guest_physical(
                            status_address,
                            std::as_bytes(std::span(&status_data, 1)))) {
                        if (cpu < max_cpus) {
                            this->dmar_qi_waits_completed[cpu] += 1;
                        }
                    }
                }
            }
        }

        head = (head + 16) & (queue_bytes - 1);
    }

    // The ring is drained: head catches up to tail.
    this->dmar.invalidation_queue_head = tail;
}

bool hypervisor::dmar_mmio(std::size_t cpu,
                           arch::x86_64::context & context,
                           std::uint64_t guest_physical)
{
    // hvix64's first touch of the IOMMU is HvpInitializeIommus reading CAP -
    // the one safe moment to set g_HvFeatureFlags bit 5 (scalable master).
    // At this point HvpInitializeIommus is running and about to allocate the
    // scalable-IOMMU object [0xb1e88], and no earlier runtime consumer has
    // seen bit 5, so the phase-1/early-runtime NULL deref is avoided (agent
    // secure-dma §12). arm_scalable_iommu_force has already located + cached
    // the flag from an earlier exit; here we only flip the bit, once. The
    // compose (0x30a59f) then reads it set and takes the scalable path,
    // building IommuFeatureSet from the ECAP.IR we advertise.
    if constexpr (nested_vmx::nested_vtd) {
        if ((0 != this->hvfeatureflags_gpa) &&
            (0 == this->scalable_force_forced)) {
            constexpr std::uint64_t scalable_master = 0x20;
            std::uint64_t flags{};
            if (read_guest_physical(
                    this->hvfeatureflags_gpa,
                    std::as_writable_bytes(std::span(&flags, 1))) &&
                (scalable_master != (flags & scalable_master))) {
                flags |= scalable_master;
                if (write_guest_physical(
                        this->hvfeatureflags_gpa,
                        std::as_bytes(std::span(&flags, 1)))) {
                    this->scalable_force_forced += 1;
                    this->scalable_force_armed = true;
                    std::uint32_t phase{};
                    if (0 != this->bootphasemode_gpa) {
                        // Best effort, for the dump only - a failure leaves
                        // phase 0, which is the runtime value anyway.
                        (void)read_guest_physical(
                            this->bootphasemode_gpa,
                            std::as_writable_bytes(
                                std::span(&phase, 1)));
                    }
                    this->scalable_force_phase = phase;
                    log("nested vt-d: forced scalable master at hvix64's "
                        "first IOMMU access (HvBootPhaseMode {})",
                        phase);
                }
            }
        }
    }

    auto instruction = decode_guest_instruction(cpu, context);
    if (!instruction) {
        // The decoder could not read this access. Refusing (rather than
        // stepping over it, which would let the write reach QEMU's real
        // unit) stops the processor loudly - a case worth seeing.
        return false;
    }

    // Where in the register page the access landed. The guest-physical
    // address is only page-granular for an EPT violation, so the offset
    // comes from the reported linear address, or from decoding the
    // instruction's own addressing when the exit did not report one - the
    // same order `on_ept_violation` uses for the local APIC page.
    constexpr std::uint64_t page_offset_mask = 0xfff;
    constexpr std::uint64_t linear_address_valid = 1ull << 7;

    std::uint64_t offset{};
    if (0 != (this->vmcs.exit_qualification() & linear_address_valid)) {
        offset = this->vmcs.guest_linear_address() & page_offset_mask;
    } else if (auto decoded = arch::x86_64::effective_address(
                   *instruction, context, context.rip)) {
        offset = *decoded & page_offset_mask;
    } else {
        offset = guest_physical & page_offset_mask;
    }

    using arch::x86_64::memory_operation;
    auto operation = instruction->what;

    // The current register value, needed by a load or a read-modify-write
    // and harmless for a plain store (which ignores it).
    auto old = dmar_register_read(offset, instruction->size);
    auto result = arch::x86_64::apply(*instruction, old);

    // Memory side: a store or a combine replaces the register; a load or
    // an examine leaves it unchanged. Applied to the synthetic unit, never
    // to QEMU's real MMIO - which is the whole point of the interposition.
    auto writes_memory = (memory_operation::load != operation) &&
                         (memory_operation::examine != operation);
    if (writes_memory) {
        dmar_register_write(cpu, offset, result, instruction->size);
    }

    // Register side: a load or read-modify-write may write a destination
    // register with the value read.
    if (instruction->writes_register) {
        auto & slot =
            context.*arch::x86_64::register_of(instruction->destination);
        slot = arch::x86_64::result_for_register(*instruction, old, slot);
    }

    // Flags: everything but a plain move sets them, and the guest branches
    // on them immediately.
    if (memory_operation::store != operation) {
        this->vmcs.guest_rflags(arch::x86_64::flags_after(
            *instruction, this->vmcs.guest_rflags(), old, result));
    }

    if (cpu < max_cpus) {
        if (writes_memory) {
            this->dmar_writes[cpu] += 1;
        } else {
            this->dmar_reads[cpu] += 1;
        }
    }

    // The first accesses, for the state dump - which register, what width,
    // and the value answered (a read) or written (a store). This is how the
    // IOMMU init's CAP/ECAP reads and the attach's programming are told
    // apart from a livelock where the gate never passed.
    if (this->dmar_access_next < dmar_log_entries) {
        auto i = this->dmar_access_next;
        this->dmar_access_offset[i] = static_cast<std::uint32_t>(offset);
        this->dmar_access_value[i] = writes_memory ? result : old;
        this->dmar_access_size[i] =
            static_cast<std::uint8_t>(instruction->size);
        this->dmar_access_write[i] = writes_memory ? 1 : 0;
        this->dmar_access_next += 1;
    }

    // The instruction has been carried out, so resume after it by the
    // length the decoder measured (the VMCS length is undefined for an EPT
    // violation; SDM 30.2.5).
    context.rip = context.rip + instruction->length;
    this->vmcs.guest_rip(context.rip);
    return true;
}

std::uint64_t hypervisor::find_hvix64_base(
    std::size_t cpu, std::uint64_t rip)
{
    if constexpr (!nested_vmx::nested_vtd) {
        return 0;
    } else {
        // hvix64's .text begins at RVA 0x200000 with eight int3 pad bytes
        // then a distinctive prologue (from hvix64.bin, verified against the
        // shipped image): mov r11,rsp / mov [r11+0x20],r9 / sub rsp,0x48 /
        // movzx eax,byte [rdx]. Match it rather than the MZ header, which
        // `image_base_of` scans up to `image_search_pages` for and can miss.
        // The rip is inside .text (this is only called for high kernel VAs),
        // so the base is 64 KB-aligned and 0x200000..~8 MB below it.
        static constexpr std::uint8_t signature[] = {
            0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, // int3 pad
            0x4c, 0x8b, 0xdc,             // mov r11, rsp
            0x4d, 0x89, 0x4b, 0x20,       // mov [r11+0x20], r9
            0x48, 0x83, 0xec, 0x48,       // sub rsp, 0x48
            0x0f, 0xb6, 0x02,             // movzx eax, byte [rdx]
        };
        constexpr std::uint64_t text_rva = 0x200000;
        constexpr std::uint64_t step = 0x10000;    // 64 KB alignment
        constexpr std::size_t max_steps = 0x800000 / step; // 8 MB window

        auto candidate = (rip - text_rva) & ~(step - 1);
        for (std::size_t n{}; n < max_steps; ++n, candidate -= step) {
            // .text (candidate + text_rva) is always mapped - the rip is
            // executing in it - so a translate failure means a wrong
            // candidate, not "not yet". At L1 this yields the L1-physical
            // directly (identity EPT), which read_guest_physical wants.
            if (auto physical =
                    translate_guest_linear(cpu, candidate + text_rva)) {
                std::uint8_t bytes[sizeof(signature)]{};
                if (read_guest_physical(
                        *physical,
                        std::as_writable_bytes(std::span(bytes)))) {
                    bool match = true;
                    for (std::size_t i{}; i < sizeof(signature); ++i) {
                        if (bytes[i] != signature[i]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        return candidate;
                    }
                }
            }
            if (candidate < step) {
                break;
            }
        }
        return 0;
    }
}

void hypervisor::arm_scalable_iommu_force(std::size_t cpu)
{
    if constexpr (!nested_vmx::nested_vtd) {
        return;
    } else {
        // Locate hvix64 once (cheap content-scan) and cache the flag + phase
        // GPAs. The bit-5 force itself is DEFERRED to dmar_mmio, at hvix64's
        // first CAP read - the one safe moment to set the scalable master
        // (agent secure-dma §12). Forcing it on a plain runtime exit resets
        // the guest before HvpInitializeIommus even runs: a runtime consumer
        // NULL-derefs the scalable-IOMMU object [0xb1e88] the same way
        // HvpFinalizeIommuFeatures does in phase 1, and that object is not
        // allocated until the CAP read's own routine (HvpInitializeIommus)
        // builds it. Measured: forcing on the first phase-0 exit set bit 5,
        // but the guest reset with dmar_reads=0 - before the IOMMU init. So:
        // locate here, force there. Once located, stop entirely.
        if ((cpu >= max_cpus) || this->running_l2[cpu] ||
            (0 != this->hvfeatureflags_gpa)) {
            return; // wrong context, or already located
        }

        constexpr std::uint64_t g_hvfeatureflags_rva = 0xaf158;
        constexpr std::uint64_t bootphasemode_rva = 0xa3d34;
        constexpr std::uint64_t snapshot = 0x0040fb2011000002ull;
        constexpr std::uint64_t scalable_master = 0x20; // bit 5 ONLY
        constexpr std::uint64_t kernel_floor = 0xfffff80000000000ull;

        // The first L1 exits run in hvloader (low VA); skip the scan for
        // them and cross-check the flag word against the feature-assembly
        // snapshot before trusting the base.
        auto rip = this->vmcs.guest_rip();
        if (rip < kernel_floor) {
            return; // hvloader/firmware, not hvix64
        }

        auto candidate = find_hvix64_base(cpu, rip);
        if ((0 == candidate) || (candidate < kernel_floor)) {
            this->scalable_force_locate_failed += 1;
            return;
        }

        auto flags_phys = translate_guest_linear(
            cpu, candidate + g_hvfeatureflags_rva);
        auto phase_phys = translate_guest_linear(
            cpu, candidate + bootphasemode_rva);
        if (!flags_phys || !phase_phys) {
            return; // not mapped yet; retry
        }

        std::uint64_t flags{};
        if (!read_guest_physical(
                *flags_phys,
                std::as_writable_bytes(std::span(&flags, 1)))) {
            return;
        }

        if ((flags != snapshot) &&
            (flags != (snapshot | scalable_master))) {
            // Wrong image, or feature assembly has not written it yet.
            this->scalable_force_locate_failed += 1;
            return;
        }

        this->hvix64_base = candidate;
        this->hvfeatureflags_gpa = *flags_phys;
        this->bootphasemode_gpa = *phase_phys;
        log("nested vt-d: hvix64 base {}, g_HvFeatureFlags at {} = {}, "
            "HvBootPhaseMode at {} - scalable master will force at "
            "hvix64's first IOMMU access",
            candidate,
            *flags_phys,
            flags,
            *phase_phys);
    }
}

} // namespace zpp::hypervisor
