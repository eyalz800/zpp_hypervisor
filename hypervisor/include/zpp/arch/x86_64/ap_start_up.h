#pragma once
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
     * it is. It cannot find that out for itself: an INIT has reset its
     * local APIC out of x2APIC mode, so its own identifier is no longer
     * readable where this VMM reads identifiers from.
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
};

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
