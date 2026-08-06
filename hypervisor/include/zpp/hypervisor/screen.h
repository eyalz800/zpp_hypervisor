#pragma once
#include "zpp/arch/x86_64/page_table.h"
#include "zpp/error.h"
#include "zpp/hypervisor/font.h"
#include "zpp/hypervisor/log.h"
#include "zpp/loader.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace zpp::hypervisor
{
/**
 * The screen, as somewhere for a processor that is stopping to say why.
 *
 * Every other channel is gone by the time it matters. There is no console,
 * the serial port belongs to the guest - and the machine this was written
 * for has none at all - the loader's own log was written before the
 * hand-over and says nothing about anything after it, and the halt records
 * in `hypervisor` need a debugger attached to the very processor that
 * stopped, on a machine that has to be powered off to escape the hang.
 * What is left is the display, which the user is already looking at.
 *
 * Why writing to it works here, since it is not a thing that works in
 * general: nothing has drawn on that screen since the loader handed over.
 * That is the whole shape of the failure being chased - the boot manager
 * takes over and the machine stops with the loader's own output still the
 * last thing visible - so the guest has neither reprogrammed the display
 * controller nor changed the mode, and the framebuffer the loader found is
 * still where it was and still live. The situation this technique is
 * fragile in, a guest that has taken the display over, is not the
 * situation it exists for.
 *
 * And if that is wrong, the cost is bounded: the writes land in whatever
 * memory the old base address now names, which produces garbage pixels or
 * nothing visible at all. It cannot fault - the range is mapped read-write
 * in the host page table before anything is written to it - and it cannot
 * corrupt the guest's own memory, because the address came from the
 * firmware's description of a device aperture rather than from anything
 * the guest allocated.
 *
 * Nothing here allocates and nothing here takes a lock. It runs on a
 * processor that is about to stop, with the heap in any state at all, and
 * the log's own lock could be held by a processor that is not coming back.
 *
 * Only the halt paths draw. Drawing from the ordinary exit path would take
 * the display away from the guest for good - a boot manager that is about
 * to paint its own screen would be fighting this for it - and the guest is
 * meant to be unaware of any of this while it is working. The failure this
 * does not cover is therefore a hang with no halt, which shows nothing at
 * all; making that visible needs something that says "still running" rather
 * than "stopped here", and a counter drawn in one corner from the exit path
 * would do it. Not built, because it has the same cost as the thing above:
 * it writes to the screen while the guest still wants it.
 */
class screen
{
public:
    /**
     * Why a screen could not be used.
     */
    enum class error
    {
        success = 0,
        no_framebuffer = 1,
        unsupported_pixel_format = 2,
        unaligned_framebuffer = 3,
        implausible_geometry = 4,
        too_large_to_map = 5,
        host_address_in_use = 6,
    };

    /**
     * Constructs an unusable screen, which is what every platform without
     * a framebuffer keeps.
     */
    constexpr screen() = default;

    /**
     * Adopts what the loader found and maps it into the host page table.
     *
     * The mapping is read-write and no-execute, and it has to exist before
     * anything is drawn: a write from a VM exit handler through an
     * unmapped address is a host page fault, and there is nowhere for the
     * host to unwind to once the guest is running.
     */
    std::expected<void, zpp::error>
    initialize(const zpp_framebuffer & description,
               arch::x86_64::page_table & host_page_table);

    /**
     * Whether anything may be drawn.
     */
    constexpr bool usable() const
    {
        return !m_pixels.empty();
    }

    /**
     * The host virtual address the framebuffer was mapped at, or zero. For
     * a debugger, and for the log line that records the mapping.
     */
    constexpr std::uint64_t address() const
    {
        return m_address;
    }

    /**
     * Draws the log over the top of the screen, newest lines last.
     *
     * The tail rather than the head: the interesting part of a log that
     * does not fit is its end, and truncating the front is what makes the
     * last thing that happened visible. A banner goes above it so that
     * whoever is looking at the machine knows what they are reading.
     */
    void draw(const log_storage::line_list & lines) const;

private:
    /**
     * Builds a pixel of the given colour in this screen's format.
     */
    constexpr std::uint32_t pixel(std::uint8_t red,
                                  std::uint8_t green,
                                  std::uint8_t blue) const
    {
        if (zpp_framebuffer_format_red_green_blue_reserved == m_format) {
            return (static_cast<std::uint32_t>(blue) << 16) |
                   (static_cast<std::uint32_t>(green) << 8) |
                   static_cast<std::uint32_t>(red);
        }

        return (static_cast<std::uint32_t>(red) << 16) |
               (static_cast<std::uint32_t>(green) << 8) |
               static_cast<std::uint32_t>(blue);
    }

    /**
     * Fills the given rows across the whole width.
     */
    void fill_rows(std::uint32_t first_row,
                   std::uint32_t rows,
                   std::uint32_t colour) const;

    /**
     * Draws one line of text with its top left corner at the given pixel,
     * stopping at the right edge rather than wrapping.
     */
    void draw_text(std::uint32_t left,
                   std::uint32_t top,
                   std::span<const char> text,
                   std::uint32_t colour) const;

    /**
     * The framebuffer as pixels, at the host virtual address it was mapped
     * at, empty until one has been. Volatile because these writes exist
     * only to be seen on a screen: nothing in this program ever reads them
     * back, which is exactly the shape of a store a compiler is entitled
     * to discard.
     */
    std::span<volatile std::uint32_t> m_pixels{};

    /**
     * The host virtual address of the first pixel.
     */
    std::uint64_t m_address{};

    /**
     * The visible width and height in pixels. The height is reduced to
     * what was actually mapped, if the firmware reported a framebuffer
     * smaller than its own geometry implies.
     */
    std::uint32_t m_width{};
    std::uint32_t m_height{};

    /**
     * How many pixels one line of video memory holds. Not the width: real
     * adapters pad the line, and the difference skews the image by a few
     * pixels on every row, which compounds down the screen.
     */
    std::uint32_t m_stride{};

    /**
     * Which of the two layouts a pixel has, as a zpp_framebuffer_format.
     */
    std::uint32_t m_format{};
};

/**
 * The screen error category.
 */
inline const zpp::error_category & category(screen::error)
{
    static constexpr auto error_category = zpp::make_error_category(
        "screen::error",
        screen::error::success,
        [](auto code) -> std::string_view {
            switch (code) {
            case screen::error::success:
                return zpp::error::no_error;
            case screen::error::no_framebuffer:
                return "The loader reported no framebuffer";
            case screen::error::unsupported_pixel_format:
                return "Pixel format not one this can draw on";
            case screen::error::unaligned_framebuffer:
                return "Framebuffer base is not page aligned";
            case screen::error::implausible_geometry:
                return "Framebuffer geometry does not describe a screen";
            case screen::error::too_large_to_map:
                return "Framebuffer does not fit the host mapping window";
            case screen::error::host_address_in_use:
                return "Host mapping window is already occupied";
            }
        });
    return error_category;
}

} // namespace zpp::hypervisor
