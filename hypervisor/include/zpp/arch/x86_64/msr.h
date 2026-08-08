#pragma once
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * MSR addresses.
 */
namespace msr
{
enum type : std::size_t
{
    ia32_extended_feature_enable = 0xc0000080,
    ia32_feature_control = 0x3a,
    ia32_mtrr_capability = 0xfe,
    ia32_debug_control = 0x1d9,

    /**
     * x2APIC registers, addressed as MSRs rather than through the APIC
     * page. The interrupt command register is what a processor writes to
     * send an inter-processor interrupt, including the INIT and start-up
     * IPIs that start an application processor.
     */
    /**
     * The APIC base, whose EXTD bit says whether the local APIC is in
     * x2APIC mode - and therefore whether the interrupt command register
     * is an MSR at all rather than a location on the APIC page.
     */
    ia32_apic_base = 0x1b,

    ia32_x2apic_apic_id = 0x802,
    ia32_x2apic_icr = 0x830,

    /**
     * The two ways a guest arms its next local timer interrupt, and the
     * only periodic thing a settled guest does that this VMM can see
     * without asking the processor for a control it may not be given.
     *
     * Which of the two is used is the guest's choice: with the deadline
     * mode advertised in CPUID leaf 1 it writes a time stamp counter
     * value to the deadline register, and otherwise it counts down from
     * the initial count register. Both are written afresh for every tick,
     * which is what makes either one a clock.
     *
     * The initial count is an x2APIC register, so it is an MSR only while
     * the local APIC is in x2APIC mode. In xAPIC mode the same register
     * lives on the APIC page and a write to it is a memory access this
     * VMM does not see.
     */
    ia32_tsc_deadline = 0x6e0,
    ia32_x2apic_init_count = 0x838,
    ia32_fs_base = 0xC0000100,
    ia32_gs_base = 0xC0000101,
};
} // namespace msr

/**
 * MTRR msr addresses.
 */
namespace msr::mtrr
{
enum type : std::size_t
{
    physbase_0 = 0x200,
    physmask_0,
    physbase_1,
    physmask_1,
    physbase_2,
    physmask_2,
    physbase_3,
    physmask_3,
    physbase_4,
    physmask_4,
    physbase_5,
    physmask_5,
    physbase_6,
    physmask_6,
    physbase_7,
    physmask_7,

    /**
     * The fixed-range MTRRs, which govern the first 1 MB and are the only
     * MTRRs with sub-page-directory granularity - 64 KB, 16 KB and 4 KB.
     * There are eleven of them, each holding eight memory types.
     *
     * The addresses are not contiguous with the variable pairs above and
     * not contiguous with each other across the three groups, which is why
     * they are named rather than derived.
     *
     * SDM Vol. 4, Table 2-2: "Register Address: 250H, 592
     * IA32_MTRR_FIX64K_00000", "258H, 600 IA32_MTRR_FIX16K_80000", "259H,
     * 601 IA32_MTRR_FIX16K_A0000", "268H, 616 IA32_MTRR_FIX4K_C0000"
     * through "26FH, 623 IA32_MTRR_FIX4K_F8000".
     */
    fix64k_00000 = 0x250,
    fix16k_80000 = 0x258,
    fix16k_a0000 = 0x259,
    fix4k_c0000 = 0x268,
    fix4k_c8000 = 0x269,
    fix4k_d0000 = 0x26a,
    fix4k_d8000 = 0x26b,
    fix4k_e0000 = 0x26c,
    fix4k_e8000 = 0x26d,
    fix4k_f0000 = 0x26e,
    fix4k_f8000 = 0x26f,

    /**
     * IA32_MTRR_DEF_TYPE, which carries the memory type used for every
     * range no MTRR covers, the fixed-range enable and the global MTRR
     * enable.
     *
     * SDM Vol. 4, Table 2-2: "Register Address: 2FFH, 767
     * IA32_MTRR_DEF_TYPE".
     */
    default_type = 0x2ff,
};
} // namespace msr::mtrr

} // namespace zpp::arch::x86_64