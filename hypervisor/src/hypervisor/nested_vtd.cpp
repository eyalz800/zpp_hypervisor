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
    // The scalable-master enable happens in arm_scalable_iommu_force (the
    // dummy-then-force one-shot), not here - by the time hvix64 reads the CAP
    // register bit 5 is long set. dmar_mmio only answers the register access.
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
        // The COHERENT-crash-safe enable (secure-dma §15/§16): break the
        // circular dependency (bit 5 must be set for HvpInitializeIommus to
        // allocate [0xb1e88], but earlier bit-5 consumers NULL-deref it) by
        // first pointing [0xb1e88] at hvix64_base - a non-NULL page whose
        // [+0x2c] is 0 - so the deref reads a harmless zero, then forcing
        // bit 5+6. HvpInitializeIommus later overwrites [0xb1e88] with the
        // real unit and composes IommuFeatureSet. One-shot: locate, then set
        // the dummy and force in a single exit, then arm and stop - no
        // per-exit overhead (which reset earlier heavy-scan builds).
        if ((cpu >= max_cpus) || this->running_l2[cpu] ||
            this->scalable_force_armed) {
            return; // wrong context, or already done
        }

        constexpr std::uint64_t g_hvfeatureflags_rva = 0xaf158;
        constexpr std::uint64_t bootphasemode_rva = 0xa3d34;
        constexpr std::uint64_t scalable_obj_rva = 0xb1e88;
        constexpr std::uint64_t kernel_floor = 0xfffff80000000000ull;

        // Step 1: locate hvix64's base (once), FAST. The content scan and the
        // snapshot cross-check took ~2805 exits - far too late: HvpInitializeIommus's
        // 0x30a59f bit-5 check runs very early, so the force must land before
        // it (§18 Q4). hvix64 is 2 MB-aligned with .text at +0x200000, so for
        // a rip in the first 2 MB of .text the base is O(1): mask to the 2 MB
        // page and subtract 0x200000. Verify the 'MZ' header to reject a wrong
        // 2 MB (a higher-.text rip) - retry then, the early exits are low-RVA.
        // No snapshot requirement: every writer is RMW and never clears bit
        // 5/6, so forcing before feature assembly is safe and survives (§18 Q3).
        if (0 == this->hvfeatureflags_gpa) {
            auto rip = this->vmcs.guest_rip();
            if (rip < kernel_floor) {
                return; // hvloader/firmware, not hvix64
            }

            auto candidate = (rip & ~0x1fffffull) - 0x200000ull;
            if (candidate < kernel_floor) {
                return;
            }

            auto base_phys = translate_guest_linear(cpu, candidate);
            std::uint16_t mz{};
            if (!base_phys ||
                !read_guest_physical(
                    *base_phys,
                    std::as_writable_bytes(std::span(&mz, 1))) ||
                (0x5a4d != mz)) {
                this->scalable_force_locate_failed += 1;
                return; // wrong 2 MB or not mapped yet; retry
            }

            auto flags_phys = translate_guest_linear(
                cpu, candidate + g_hvfeatureflags_rva);
            auto phase_phys = translate_guest_linear(
                cpu, candidate + bootphasemode_rva);
            auto obj_phys = translate_guest_linear(
                cpu, candidate + scalable_obj_rva);
            if (!flags_phys || !phase_phys || !obj_phys) {
                return; // hvix64 .data not mapped yet; retry
            }

            this->hvix64_base = candidate;
            this->hvfeatureflags_gpa = *flags_phys;
            this->bootphasemode_gpa = *phase_phys;
            this->scalable_obj_gpa = *obj_phys;
            log("nested vt-d: hvix64 base {} (rip-located), "
                "g_HvFeatureFlags at {}, scalable obj [0xb1e88] at {}",
                candidate,
                *flags_phys,
                *obj_phys);
            return; // poke the legacy feature gate on the next exit
        }

        // Step 2 (once): the LEGACY-path enable (§14/§15, KVM agent). The
        // rig's intremap=off leaves the loader block empty, so hvix64 can
        // never build the scalable object [0xb1e88] - but the no-PASID NVMe
        // needs only LEGACY VT-d second-level remapping, which hvix64 has via
        // HvpProgramDeviceContext (0x303bc8): it uses the SLPT at
        // partition+0x4550 and the unit (already initialized, M1 answers its
        // register/QI interface) - no scalable object, no bit 5. The only
        // obstacle is the attach's feature gate reading IommuFeatureSet
        // [0xb1f24] (0x303bc8:600 needs bit 13; :642-645 needs one of
        // {2,4,6,7,16}) and the present flag [0xb1f99] (else 0x303bc8 returns
        // 8). Poke both; do NOT set bit 5 (so the worker stays legacy) or
        // build the scalable object. Both share 0xb1e88's page, so the GPAs
        // come from scalable_obj_gpa + the RVA delta. IommuFeatureSet is
        // written only by the scalable compose (which never runs here), so a
        // single poke persists to the 0x82 attach.
        constexpr std::uint64_t featureset_delta = 0xb1f24 - 0xb1e88;
        constexpr std::uint64_t present_delta = 0xb1f99 - 0xb1e88;
        // bit 6 passes the bit-0-set gate (0x303bc8:642); bit 13 passes the
        // bit-0-clear gate (:600) that the routing steer below enters. Both,
        // so the gate passes on either branch.
        constexpr std::uint32_t featureset_bits = 0x2040; // bit 13 + bit 6

        if (0 == this->scalable_force_forced) {
            auto featureset_gpa = this->scalable_obj_gpa + featureset_delta;
            std::uint32_t featureset{};
            if (!read_guest_physical(
                    featureset_gpa,
                    std::as_writable_bytes(std::span(&featureset, 1)))) {
                return;
            }
            featureset |= featureset_bits;
            if (!write_guest_physical(
                    featureset_gpa,
                    std::as_bytes(std::span(&featureset, 1)))) {
                return;
            }

            auto present_gpa = this->scalable_obj_gpa + present_delta;
            std::uint8_t present{};
            if (read_guest_physical(
                    present_gpa,
                    std::as_writable_bytes(std::span(&present, 1)))) {
                present |= 1;
                (void)write_guest_physical(
                    present_gpa, std::as_bytes(std::span(&present, 1)));
            }
            this->scalable_force_forced = 1;
        }

        // Step 3: steer the 0x82 attach off the scalable route (§16/§22). The
        // worker sends a plain-PCI NVMe to the scalable context 0x318508
        // (whose object never allocates on this rig) unless the root
        // partition's DMA-cap [partition+0x1a0] bit 0 is clear. Locate the
        // partition the way HvpRefPartition does - the partition VA is at
        // %gs:0x360 - but ONLY from an hvix64-KERNEL exit: GS is hvix64's
        // per-LP block only while hvix64's kernel runs; at most L1 exits the
        // context is Windows-under-hvix64 and GS/CR3 are Windows' (§22, why
        // the earlier ungated *(GS+0x360) read garbage 521k times). Gate on
        // rip in hvix64 .text so guest_gs_base and translate_guest_linear's
        // CR3 are hvix64's. Reader-proof [partition+0x1a0] == 0x40e9000221
        // before writing, so a wrong base cannot corrupt the guest; then
        // clear bit 0, routing the attach onto the object-free 0x318bfc path.
        constexpr std::uint64_t text_start = 0x200000;
        constexpr std::uint64_t text_end = 0x3b3894;
        constexpr std::uint64_t partition_from_gs = 0x360;
        constexpr std::uint64_t dma_cap_off = 0x1a0;
        constexpr std::uint64_t dma_cap_expected = 0x40e9000221ull;
        // The partition lives in a lower canonical-kernel range than hvix64's
        // own image (measured *(GS+0x360) = 0xffffe80000001000), so accept any
        // canonical high-half address, not hvix64's 0xfffff8.. floor.
        constexpr std::uint64_t partition_floor = 0xffff800000000000ull;

        auto rip = this->vmcs.guest_rip();
        if ((rip < this->hvix64_base + text_start) ||
            (rip >= this->hvix64_base + text_end)) {
            return; // not an hvix64-kernel exit; GS/CR3 not hvix64's, retry
        }
        this->steer_rip_ok += 1;

        auto gs_base = this->vmcs.guest_gs_base();
        this->steer_gs_base = gs_base; // diagnostic
        if (gs_base < kernel_floor) {
            return; // retry
        }

        auto part_ptr_phys =
            translate_guest_linear(cpu, gs_base + partition_from_gs);
        if (!part_ptr_phys) {
            return;
        }
        std::uint64_t partition{};
        if (!read_guest_physical(
                *part_ptr_phys,
                std::as_writable_bytes(std::span(&partition, 1)))) {
            return;
        }
        this->steer_gs360_raw = partition; // diagnostic, raw pre-check
        if (partition < partition_floor) {
            return; // GS+0x360 did not hold a kernel pointer; retry
        }
        this->partition_va = partition;

        auto dma_cap_phys =
            translate_guest_linear(cpu, partition + dma_cap_off);
        if (!dma_cap_phys) {
            return;
        }
        std::uint64_t dma_cap{};
        if (!read_guest_physical(
                *dma_cap_phys,
                std::as_writable_bytes(std::span(&dma_cap, 1)))) {
            return;
        }
        this->partition_dma_cap = dma_cap; // diagnostic, pre-check
        if (dma_cap_expected != dma_cap) {
            return; // wrong base, or partition not fully set up; retry
        }

        // Located + verified. Read the default domain + SLPT root (§17): the
        // attach's scalable context 0x318508 needs NO scalable object, only
        // these partition structures - if either is null, THAT is the real
        // precondition to provide. Do NOT clear bit 0 (§17 CORRECTION A: both
        // bit-0 states fall through to 0x318508, so clearing it is pointless).
        constexpr std::uint64_t default_domain_off = 0x4540;
        constexpr std::uint64_t slpt_off = 0x4550;
        if (auto dd_phys = translate_guest_linear(
                cpu, partition + default_domain_off)) {
            std::uint64_t dd{};
            if (read_guest_physical(
                    *dd_phys, std::as_writable_bytes(std::span(&dd, 1)))) {
                this->partition_default_domain = dd;
            }
        }
        if (auto slpt_phys =
                translate_guest_linear(cpu, partition + slpt_off)) {
            std::uint64_t slpt{};
            if (read_guest_physical(
                    *slpt_phys,
                    std::as_writable_bytes(std::span(&slpt, 1)))) {
                this->partition_slpt = slpt;
            }
        }

        this->partition_steer_done += 1;
        this->scalable_force_armed = true;
        log("nested vt-d: partition {} located, DMA-cap {}, default domain "
            "[+0x4540] {}, SLPT [+0x4550] {}",
            partition,
            this->partition_dma_cap,
            this->partition_default_domain,
            this->partition_slpt);
    }
}

} // namespace zpp::hypervisor
