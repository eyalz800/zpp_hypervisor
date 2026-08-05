#include "zpp/hypervisor/screen.h"

namespace zpp::hypervisor
{
namespace
{
/**
 * The page size, spelled here rather than taken from the hypervisor class,
 * so that this file does not depend on it.
 */
constexpr std::uint64_t page_size = 0x1000;

/**
 * The host virtual address the framebuffer is mapped at, and how much room
 * there is at it.
 *
 * Not the framebuffer's own physical address, which is what everything
 * else in the host page table is mapped at. The reason is the shape of
 * arch::x86_64::page_table: it holds one page directory pointer table
 * shared by every entry of its level four table, and two page directories
 * shared by the halves of that, so the only address bits it actually
 * translates are bit 38 - which selects the half - and bits 29:0. Two
 * aliased one gigabyte windows, in other words, and a mapping is only
 * distinct from another if it differs in those bits.
 *
 * Under UEFI everything else mapped there is identity mapped from physical
 * memory, which on any machine short of two hundred and fifty-six
 * gigabytes leaves bit 38 clear - so this window, which is bit 38 alone,
 * is the one region that cannot collide with the module, the page table
 * itself or the start-up trampoline below one megabyte. Mapping the
 * framebuffer at its own address would collide with any of them that
 * happened to share bits 29:0 with it, and a framebuffer at a gigabyte
 * boundary shares them with the trampoline exactly.
 *
 * Checked as well as reasoned about: initialize refuses to map if anything
 * is already present at these addresses.
 */
constexpr std::uint64_t host_window = 0x4000000000;
constexpr std::uint64_t host_window_size = 0x40000000;

/**
 * What is drawn above the log, so that whoever is looking at the machine
 * knows what the text is and that the machine is not coming back.
 */
constexpr char banner[] =
    "zpp: this processor stopped. its log follows, oldest first.";

} // namespace

std::expected<void, zpp::error>
screen::initialize(const zpp_framebuffer & description,
                   arch::x86_64::page_table & host_page_table)
{
    if (!description.base || !description.size ||
        (zpp_framebuffer_format_none == description.format)) {
        return std::unexpected(zpp::error{error::no_framebuffer});
    }

    // A bit mask says where the channels are but not how wide a pixel is,
    // and a blt-only mode has no linear framebuffer at all - its base
    // address means nothing. Declined rather than guessed at: writing 32
    // bit pixels into a 16 bit mode draws a diagonal mess over whatever
    // was there, and the point of this is to be readable.
    if ((zpp_framebuffer_format_blue_green_red_reserved !=
         description.format) &&
        (zpp_framebuffer_format_red_green_blue_reserved !=
         description.format)) {
        return std::unexpected(
            zpp::error{error::unsupported_pixel_format});
    }

    // The mapping is by whole pages, so a base that is not page aligned
    // would put every pixel at an offset from where it was computed.
    if (description.base & (page_size - 1)) {
        return std::unexpected(zpp::error{error::unaligned_framebuffer});
    }

    if (!description.width || !description.height ||
        (description.pixels_per_scan_line < description.width)) {
        return std::unexpected(zpp::error{error::implausible_geometry});
    }

    // What could be touched at most: every scan line of the visible
    // height, stride included, since the padding at the end of a line is
    // part of the memory the lines are spaced by.
    auto bytes_per_line = static_cast<std::uint64_t>(
        description.pixels_per_scan_line * sizeof(std::uint32_t));
    auto bytes_needed = bytes_per_line * description.height;
    auto bytes = (bytes_needed < description.size) ? bytes_needed
                                                   : description.size;
    if (bytes > host_window_size) {
        return std::unexpected(zpp::error{error::too_large_to_map});
    }

    auto pages = (bytes + (page_size - 1)) / page_size;

    // Refuse rather than overwrite. An entry already present here is
    // either an assumption above having gone wrong or a platform whose
    // host addresses are not what this expects, and in both cases
    // replacing the mapping would silently point something else at the
    // display.
    for (std::uint64_t page{}; page < pages; ++page) {
        if (host_page_table
                .page_table_entry(host_window + (page * page_size))
                .present()) {
            return std::unexpected(zpp::error{error::host_address_in_use});
        }
    }

    for (std::uint64_t page{}; page < pages; ++page) {
        auto address = host_window + (page * page_size);

        // Read-write, and no execute: nothing is ever fetched from a
        // framebuffer, so the one thing that could happen through this
        // mapping by accident is made impossible.
        host_page_table.map_page(
            address,
            description.base + (page * page_size),
            arch::x86_64::page_table::protection::read |
                arch::x86_64::page_table::protection::write);

        // Cache disabled. This is a device aperture, and a write that sits
        // in a cache line is a write that never reaches the screen - there
        // is no later flush to rely on, because the next thing this
        // processor does is halt forever.
        //
        // PCD alone, with PWT and PAT clear, selects PAT entry 2, which is
        // UC- after a reset (SDM Vol. 3A, Table 14-11 and Table 14-12).
        // UC- is the entry that lets an MTRR of write-combining through as
        // write-combining while everything else comes out uncacheable (SDM
        // Vol. 3A, Table 14-7), which is exactly the treatment a
        // framebuffer wants.
        //
        // Note what actually guarantees it, since this VMM does not load
        // an IA32_PAT of its own on VM exit and therefore runs on whatever
        // the guest left in that MSR: it is the MTRR covering the
        // aperture. "If there is an overlap of page-level and MTRR caching
        // controls, the mechanism that prevents caching has precedence"
        // (SDM Vol. 3A, Section 14.5.2), and an aperture is covered by an
        // uncacheable or write-combining MTRR - or by none at all, above
        // the top of memory, where the default type applies. The bit below
        // is the part this code can control.
        host_page_table.page_table_entry(address).page_level_cache_disable(
            true);
    }

    m_address = host_window;
    m_pixels = std::span<volatile std::uint32_t>(
        reinterpret_cast<volatile std::uint32_t *>(host_window),
        static_cast<std::size_t>(bytes / sizeof(std::uint32_t)));
    m_width = description.width;
    m_stride = description.pixels_per_scan_line;
    m_format = description.format;

    // Reduced to what was mapped, if the firmware reported a framebuffer
    // smaller than its own geometry needs. Every write below is computed
    // from this, so nothing can reach past the mapping.
    auto mapped_lines = bytes / bytes_per_line;
    m_height = (mapped_lines < description.height)
                   ? static_cast<std::uint32_t>(mapped_lines)
                   : description.height;
    if (!m_height) {
        m_pixels = {};
        return std::unexpected(zpp::error{error::implausible_geometry});
    }

    return {};
}

void screen::fill_rows(std::uint32_t first_row,
                       std::uint32_t rows,
                       std::uint32_t colour) const
{
    for (std::uint32_t row{}; row < rows; ++row) {
        auto y = first_row + row;
        if (y >= m_height) {
            return;
        }

        auto line = static_cast<std::size_t>(y) * m_stride;
        for (std::uint32_t x{}; x < m_width; ++x) {
            m_pixels[line + x] = colour;
        }
    }
}

void screen::draw_text(std::uint32_t left,
                       std::uint32_t top,
                       std::span<const char> text,
                       std::uint32_t colour) const
{
    for (std::size_t index{}; index < text.size(); ++index) {
        auto x =
            left + static_cast<std::uint32_t>(index * font::cell_width);

        // Stop at the right edge rather than wrapping. A wrapped line
        // would push everything below it down by one row and cost the
        // oldest line shown, and a log line long enough to wrap is long
        // enough for its tail to be the least interesting part of it.
        if ((x + font::cell_width) > m_width) {
            return;
        }

        auto rows = font::rows(text[index]);
        for (std::size_t row{}; row < font::cell_height; ++row) {
            auto y = top + static_cast<std::uint32_t>(row);
            if (y >= m_height) {
                return;
            }

            auto bits = rows[row];
            if (!bits) {
                continue;
            }

            auto line = static_cast<std::size_t>(y) * m_stride;
            for (std::size_t column{}; column < font::cell_width;
                 ++column) {
                if (bits & (0x80u >> column)) {
                    m_pixels[line + x + column] = colour;
                }
            }
        }
    }
}

void screen::draw(const log_storage::line_list & lines) const
{
    if (!usable()) {
        return;
    }

    // Black is zero in both layouts, so it needs no conversion. The banner
    // is amber and the log white, which is also the one place the two
    // layouts differ observably - a screen with the channels the wrong way
    // round shows a blue banner over white text.
    constexpr std::uint32_t black = 0;
    auto banner_colour = pixel(0xff, 0x80, 0x00);
    auto text_colour = pixel(0xff, 0xff, 0xff);

    auto text_rows =
        static_cast<std::uint32_t>(m_height / font::cell_height);
    if (text_rows < 2) {
        return;
    }

    // One row goes to the banner, the rest to the log.
    auto log_rows = text_rows - 1;
    auto available = static_cast<std::size_t>(log_rows);
    auto shown = (lines.size() < available) ? lines.size() : available;

    // Clear only what is about to be written on, across the full width.
    // The screen already has something on it - the firmware's own output,
    // or whatever the guest drew - and text over the top of that is not
    // readable.
    fill_rows(0,
              static_cast<std::uint32_t>((shown + 1) * font::cell_height),
              black);

    draw_text(0,
              0,
              std::span<const char>(banner, sizeof(banner) - 1),
              banner_colour);

    // Wind on to the tail. The newest lines are the ones worth the space:
    // a failure is a sequence, and the end of it is where the machine
    // stopped. std::list has no random access, so this is a walk - bounded
    // by the log's own line limit, and paid once.
    auto line = lines.begin();
    for (auto skipped = lines.size() - shown; skipped; --skipped) {
        ++line;
    }

    for (std::size_t row{}; row < shown; ++row, ++line) {
        draw_text(
            0,
            static_cast<std::uint32_t>((row + 1) * font::cell_height),
            std::span<const char>(line->data(), line->size()),
            text_colour);
    }
}

} // namespace zpp::hypervisor
