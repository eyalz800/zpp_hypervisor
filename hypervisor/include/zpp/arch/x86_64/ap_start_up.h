#pragma once
#include "zpp/arch/x86_64/generic.h"

#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * The blob an application processor begins executing when this VMM starts
 * it, defined in ap_start_up.S. Copied into a page below one megabyte,
 * because a start-up IPI vector is a page number and cannot name anything
 * higher, and position independent so that copying it is enough.
 * @{
 */
extern "C" const unsigned char zpp_ap_start_up_begin[];
extern "C" const unsigned char zpp_ap_start_up_end[];
/**
 * @}
 */

/**
 * The data area at the end of the blob, which is everything the trampoline
 * cannot work out for itself. Written before the start-up IPI is sent.
 *
 * This mirrors a layout the assembler pins to a fixed offset, so the two
 * declarations have to agree. They are checked against each other at the
 * bottom of this header rather than trusted.
 */
struct ap_start_up_area
{
    /**
     * The host page table root, loaded once the processor is in long mode
     * and a full width write to CR3 is available. The page the blob lives
     * in has to be mapped here too: the instruction after that write is
     * fetched through it.
     */
    std::uint64_t host_cr3;

    /**
     * The host CR0 and CR4, so that the code entered below runs in the
     * same environment every other processor's host side does.
     * @{
     */
    std::uint64_t host_cr0;
    std::uint64_t host_cr4;
    /**
     * @}
     */

    /**
     * Where to go once long mode is reached. Takes the value of argument
     * below as its only parameter.
     */
    std::uint64_t entry;

    /**
     * The stack the entry point is called with. The trampoline itself
     * never pushes, so this is the first stack this processor has.
     */
    std::uint64_t stack_top;

    /**
     * Passed to the entry point, and used to tell this processor which one
     * it is, rather than leaving it to work that out from its own local
     * APIC. Its identifier is readable from CPUID in any APIC mode, but
     * being told costs nothing and keeps the trampoline free of any
     * assumption about what mode the APIC is in.
     */
    std::uint64_t argument;

    /**
     * A page table root that identity maps enough of low memory to get
     * from the moment paging is enabled to the first sixty-four bit
     * instruction, and is used for nothing else.
     *
     * It exists because host_cr3 cannot be loaded yet. That load happens
     * in thirty-two bit code, where CR3 can only be written thirty-two
     * bits wide, and the module may be mapped above four gigabytes - so
     * the real root is not always expressible there. This one is placed
     * deliberately low.
     */
    std::uint64_t temporary_cr3;

    /**
     * The assembly's own working area: the descriptor table pointer it
     * relocates, the three far pointers, and the small GDT it climbs with.
     * Nothing here writes to any of it.
     */
    std::uint8_t assembly_owned[0x80 - 0x38];

    /**
     * The host descriptor tables, loaded before the entry point above is
     * reached.
     *
     * Both are needed and in this order. The IDT's gates name the host
     * code segment, which does not exist in the GDT the trampoline climbs
     * with, so an IDT loaded without the GDT would make a fault worse
     * rather than better - the delivery would fault too. With both loaded
     * a fault reaches the ordinary host handler, which records it; before
     * them it escalated to a triple fault and reset the machine.
     *
     * idt_layout rather than a type of this header's own, because it
     * already solves the awkward part: an lgdt or lidt operand is a two
     * byte limit immediately followed by an eight byte base, which no
     * naturally aligned struct lays out by itself. It places six bytes of
     * padding first, so that the limit lands six bytes in and the base
     * eight, adjacent and each on its own alignment - and data() hands
     * back the ten bytes starting at the limit. The assembly addresses
     * those ten bytes directly, so its offsets are six greater than these
     * members'.
     * @{
     */
    idt_layout host_gdtr;
    idt_layout host_idtr;
    /**
     * @}
     */

    /**
     * The host code segment selector, which CS is reloaded to out of the
     * host GDT once it is live.
     */
    std::uint16_t host_cs;

    /**
     * How far the climb got, one of ap_start_up_stage. Written as each
     * step completes, and the only account of a failure that happens
     * before the tables above are loaded.
     */
    std::uint8_t stage;
};

/**
 * The steps the trampoline records in stage, in the order it reaches them.
 * Mirrors the ap_stage_ constants in ap_start_up.S.
 */
enum class ap_start_up_stage : std::uint8_t
{
    not_started = 0,
    real_mode = 1,
    protected_mode = 2,
    long_mode = 3,
    host_page_table = 4,
    host_tables = 5,
    entering_cpp = 6,
};

/**
 * The layout above describes memory the assembly also addresses, by fixed
 * offsets it declares for itself, so the two are pinned against each other
 * rather than trusted to stay in step.
 * @{
 */
static_assert(offsetof(ap_start_up_area, host_cr3) == 0x00);
static_assert(offsetof(ap_start_up_area, host_cr0) == 0x08);
static_assert(offsetof(ap_start_up_area, host_cr4) == 0x10);
static_assert(offsetof(ap_start_up_area, entry) == 0x18);
static_assert(offsetof(ap_start_up_area, stack_top) == 0x20);
static_assert(offsetof(ap_start_up_area, argument) == 0x28);
static_assert(offsetof(ap_start_up_area, temporary_cr3) == 0x30);
static_assert(offsetof(ap_start_up_area, host_gdtr) == 0x80);
static_assert(offsetof(ap_start_up_area, host_idtr) == 0x90);

// The assembly's lgdt and lidt operands are the ten bytes starting at each
// limit, which is six into the layout above - so it names 0x86 and 0x96.
static_assert(offsetof(idt_layout, limit) == 6);
static_assert(offsetof(idt_layout, base) == 8);
static_assert(sizeof(idt_layout) == 0x10);
static_assert(offsetof(ap_start_up_area, host_cs) == 0xa0);
static_assert(offsetof(ap_start_up_area, stage) == 0xa2);
/**
 * @}
 */

/**
 * Where that area sits inside the blob, matching the offset ap_start_up.S
 * declares and pads to.
 */
inline constexpr std::size_t ap_start_up_area_offset = 0xf00;

/**
 * How much memory the blob needs reserved below one megabyte: its own page
 * plus one for each level of the temporary page table above.
 *
 * Three levels, because that table maps with two megabyte pages and so
 * stops at the page directory. Each level has to start on a page boundary,
 * so none of them can share a page with another.
 */
inline constexpr std::size_t ap_start_up_pages = 4;

} // namespace zpp::arch::x86_64

/**
 * What the trampoline jumps to once the processor it is running on has
 * reached long mode on the host page table, taking the value the area's
 * argument field was left holding.
 *
 * Declared at global scope, and with C linkage, because the assembly that
 * reaches it stores its address as a plain word and knows nothing about
 * namespaces or overloads.
 */
extern "C" void zpp_ap_start_up_main(std::uint64_t processor);
