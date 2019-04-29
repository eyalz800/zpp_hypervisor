#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zpp::x64
{
/**
 * Represents the GDT layout for SGDT/LGDT instructions.
 */
struct gdt_layout
{
    /**
     * Returns a pointer to the SGDT/SIDT expected instructions operand.
     */
    void * data()
    {
        return reinterpret_cast<char *>(this) +
               offsetof(gdt_layout, limit);
    }

    /**
     * Returns a pointer to the SGDT/SIDT expected instructions operand.
     */
    const void * data() const
    {
        return reinterpret_cast<const char *>(this) +
               offsetof(gdt_layout, limit);
    }

    /**
     * Used to adjust the padding.
     */
    std::uint16_t _[3];

    /**
     * The limit value.
     */
    std::uint16_t limit{};

    /**
     * The base value.
     */
    std::uint64_t base{};
};

/**
 * Represents the GDT layout for SIDT/LIDT instructions.
 */
struct idt_layout
{
    /**
     * Returns a pointer to the SGDT/SIDT expected instructions operand.
     */
    void * data()
    {
        return reinterpret_cast<char *>(this) +
               offsetof(idt_layout, limit);
    }

    /**
     * Returns a pointer to the SGDT/SIDT expected instructions operand.
     */
    const void * data() const
    {
        return reinterpret_cast<const char *>(this) +
               offsetof(idt_layout, limit);
    }

    /**
     * Used to adjust the padding.
     */
    std::uint16_t _[3];

    /**
     * The limit value.
     */
    std::uint16_t limit{};

    /**
     * The base value.
     */
    std::uint64_t base{};
};

/**
 * Check offset values.
 */
static_assert(std::is_standard_layout_v<gdt_layout> &&
                  std::is_standard_layout_v<idt_layout> &&
                  (offsetof(gdt_layout, limit) == 6) &&
                  (offsetof(idt_layout, limit) == 6) &&
                  (offsetof(gdt_layout, base) == 8) &&
                  (offsetof(idt_layout, base) == 8),
              "Offset values for gdt or idt layout failed.");

/**
 * The GDTR register values.
 */
struct gdtr
{
    /**
     * The GDTR base value.
     */
    std::uint64_t base{};

    /**
     * The GDTR limit value, which is one less than the size.
     */
    std::uint16_t limit{};
};

/**
 * The IDTR register values.
 */
struct idtr
{
    /**
     * The IDTR base value.
     */
    std::uint64_t base{};

    /**
     * The IDTR limit value, which is one less than the size.
     */
    std::uint16_t limit{};
};

} // namespace zpp::x64
