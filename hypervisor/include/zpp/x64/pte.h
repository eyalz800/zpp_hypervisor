#pragma once
#include <cstdint>

namespace zpp::x64
{
/**
 * Represents a page table entry structure.
 */
class pte
{
public:
    /**
     * Constructs an empty page table entry.
     */
    constexpr pte() = default;

    /**
     * Constructs a page table entry structure from a given value.
     */
    constexpr pte(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the present bit.
     */
    constexpr bool present() const
    {
        return m_value & 1;
    }

    /**
     * Sets the present bit to the specified value.
     */
    constexpr void present(bool value)
    {
        m_value = (m_value & ~0x1ull) | std::uint64_t{value};
    }

    /**
     * Returns the write bit.
     */
    constexpr bool write() const
    {
        return m_value & (1 << 1);
    }

    /**
     * Sets the write bit to the specified value.
     */
    constexpr void write(bool value)
    {
        m_value = (m_value & ~(0x1ull << 1)) | (std::uint64_t{value} << 1);
    }

    /**
     * Returns the user bit.
     */
    constexpr bool user() const
    {
        return m_value & (1 << 2);
    }

    /**
     * Sets the user bit to the specified value.
     */
    constexpr void user(bool value)
    {
        m_value = (m_value & ~(0x1ull << 2)) | (std::uint64_t{value} << 2);
    }

    /**
     * Returns the write through bit.
     */
    constexpr bool write_through() const
    {
        return m_value & (1 << 3);
    }

    /**
     * Sets the write through bit to the specified value.
     */
    constexpr void write_through(bool value)
    {
        m_value = (m_value & ~(0x1ull << 3)) | (std::uint64_t{value} << 3);
    }

    /**
     * Returns the page level cache disable bit.
     */
    constexpr bool page_level_cache_disable() const
    {
        return m_value & (1 << 4);
    }

    /**
     * Sets the page level cache disable bit to the specified value.
     */
    constexpr void page_level_cache_disable(bool value)
    {
        m_value = (m_value & ~(0x1ull << 4)) | (std::uint64_t{value} << 4);
    }

    /**
     * Returns the access bit.
     */
    constexpr bool access() const
    {
        return m_value & (1 << 5);
    }

    /**
     * Sets the access bit to the specified value.
     */
    constexpr void access(bool value)
    {
        m_value = (m_value & ~(0x1ull << 5)) | (std::uint64_t{value} << 5);
    }

    /**
     * Returns the dirty bit.
     */
    constexpr bool dirty() const
    {
        return m_value & (1 << 6);
    }

    /**
     * Sets the dirty bit to the specified value.
     */
    constexpr void dirty(bool value)
    {
        m_value = (m_value & ~(0x1ull << 6)) | (std::uint64_t{value} << 6);
    }

    /**
     * Returns the PAT bit.
     */
    constexpr bool pat() const
    {
        return m_value & (1 << 7);
    }

    /**
     * Sets the PAT bit to the specified value.
     */
    constexpr void pat(bool value)
    {
        m_value = (m_value & ~(0x1ull << 7)) | (std::uint64_t{value} << 7);
    }

    /**
     * Returns the large bit.
     */
    constexpr bool large() const
    {
        return m_value & (1 << 7);
    }

    /**
     * Sets the large bit to the specified value.
     */
    constexpr void large(bool value)
    {
        m_value = (m_value & ~(0x1ull << 7)) | (std::uint64_t{value} << 7);
    }

    /**
     * Returns the global bit.
     */
    constexpr bool global() const
    {
        return m_value & (1 << 8);
    }

    /**
     * Sets the global bit to the specified value.
     */
    constexpr void global(bool value)
    {
        m_value = (m_value & ~(0x1ull << 8)) | (std::uint64_t{value} << 8);
    }

    /**
     * Returns the page number field.
     */
    constexpr std::uint64_t page_number() const
    {
        return ((m_value >> 12) & 0xffffffffffull);
    }

    /**
     * Sets the page number to the specified value.
     */
    constexpr void page_number(std::uint64_t value)
    {
        m_value = ((m_value & ~0xffffffffff000ull) |
                   ((value & 0xffffffffffull) << 12));
    }

    /**
     * Returns the protection key field.
     */
    constexpr std::uint64_t protection_key() const
    {
        return ((m_value >> 58) & 0xf);
    }

    /**
     * Sets the protection key field to the specified value.
     */
    constexpr void protection_key(std::uint64_t value)
    {
        m_value =
            ((m_value & ~0x3c00000000000000ull) | ((value & 0xf) << 58));
    }

    /**
     * Returns the execute disable bit.
     */
    constexpr bool execute_disable() const
    {
        return m_value & (1ull << 63);
    }

    /**
     * Sets the execute disable bit to the specified value.
     */
    constexpr void execute_disable(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 63)) | (std::uint64_t{value} << 63);
    }

    /**
     * Convert the entry to an integral representation.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Converts the entry to an integral representation.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The internal representation of the entry.
     */
    std::uint64_t m_value{};
};

} // namespace zpp::x64
