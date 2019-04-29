#pragma once
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

namespace zpp
{
/**
 * Represents a map
 */
template <typename Key,
          typename Value,
          std::size_t Size,
          typename Compare = std::less<>>
class small_map
{
public:
    /**
     * Type checking.
     * @{
     */
    static_assert(std::is_nothrow_move_constructible_v<Key>,
                  "Must not throw on move construction.");

    static_assert(std::is_nothrow_move_assignable_v<Key>,
                  "Must not throw on move assignment.");

    static_assert(std::is_nothrow_move_constructible_v<Value>,
                  "Must not throw on move construction.");

    static_assert(std::is_nothrow_move_assignable_v<Value>,
                  "Must not throw on move assignment.");
    /**
     * @}
     */

    /**
     * @name Types used in the map.
     * @{
     */
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<key_type, mapped_type>;
    using const_value_type = std::add_const_t<value_type>;
    using reference = std::add_lvalue_reference_t<value_type>;
    using const_reference =
        std::add_lvalue_reference_t<std::add_const_t<value_type>>;
    using pointer = std::add_pointer_t<value_type>;
    using const_pointer = std::add_pointer_t<std::add_const_t<value_type>>;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using key_compare = Compare;
    using difference_type = std::ptrdiff_t;
    using size_type = std::size_t;
    /*
     * @}
     */

    /**
     * Constructs an empty map.
     */
    small_map() = default;

    /**
     * Construct a small map with the values on the range [first, last)
     */
    template <typename InputIterator>
    small_map(InputIterator first, InputIterator last)
    {
        insert(first, last);
    }

    /**
     * Construct a map with initial values.
     */
    small_map(std::initializer_list<value_type> values) :
        small_map(values.begin(), values.end())
    {
    }

    /**
     * Move construct a small map from another one.
     */
    small_map(small_map && other) noexcept : m_size(other.m_size)
    {
        // Move all values from other to this, destroying them along the
        // way.
        for (size_type i{}; i < m_size; ++i) {
            // The other value.
            auto & other_value = other.value(i);

            // Move construct a new value from other value.
            ::new (std::addressof(m_storage[i]))
                value_type(std::move(other_value));

            // Destroy other value.
            other_value.~value_type();
        }

        // Zero other size.
        other.m_size = {};
    }

    /**
     * Copy construct a small map from another one.
     */
    small_map(const small_map & other)
    {
        // Create a guard to clear the map in case of failure.
        struct guard
        {
            ~guard()
            {
                if (me) {
                    me->clear();
                };
            }
            small_map * me;
        } clear_guard{this};

        // Move all values from other to this, destroying them along the
        // way.
        for (size_type i{}; i < m_size; ++i) {
            // The other value.
            auto & other_value = other.value(i);

            // Copy construct a new value from other value.
            ::new (std::addressof(m_storage[i])) value_type(other_value);

            // Increment size.
            ++m_size;
        }

        // Cancel the guard.
        clear_guard.me = {};
    }

    /**
     * Move assign another map to this.
     * The behavior is undefined if other is this object.
     */
    small_map & operator=(small_map && other) noexcept
    {
        // Destroy current values.
        clear();

        // Update current size.
        m_size = other.m_size;

        // Move all values from other to this, destroying them along the
        // way.
        for (size_type i{}; i < m_size; ++i) {
            // The other value.
            auto & other_value = other.value(i);

            // Move construct a new value from other value.
            ::new (std::addressof(m_storage[i]))
                value_type(std::move(other_value));

            // Destroy other value.
            other_value->~value_type();
        }

        // Zero other size.
        other.m_size = {};
        return *this;
    }

    /**
     * Copy assign another map to this.
     */
    small_map & operator=(const small_map & other) noexcept
    {
        // Move assign from a copy.
        *this = small_map(other);
        return *this;
    }

    /**
     * Destroy the map.
     */
    ~small_map()
    {
        clear();
    }

    /**
     * Clears the map.
     */
    void clear()
    {
        // Destroy current values.
        for (size_type i{}; i < m_size; ++i) {
            // Destroy current value.
            this->value(i).~value_type();
        }

        // Zero our size.
        m_size = {};
    }

    /**
     * Returns an iterator to the beginning of the map.
     */
    iterator begin()
    {
        return std::addressof(value(0));
    }

    /**
     * Returns an iterator to the beginning of the map.
     */
    const_iterator begin() const
    {
        return std::addressof(value(0));
    }

    /**
     * Returns an iterator to the beginning of the map.
     */
    const_iterator cbegin() const
    {
        return std::addressof(value(0));
    }

    /**
     * Returns a reverse begin iterator to the map.
     */
    reverse_iterator rbegin()
    {
        return reverse_iterator(end());
    }

    /**
     * Returns a reverse begin iterator to the map.
     */
    const_reverse_iterator rbegin() const
    {
        return const_reverse_iterator(end());
    }

    /**
     * Returns a reverse begin iterator to the map.
     */
    const_reverse_iterator crbegin() const
    {
        return const_reverse_iterator(cend());
    }

    /**
     * Returns an iterator to the end of the map.
     */
    iterator end()
    {
        return std::addressof(value(m_size - 1)) + 1;
    }

    /**
     * Returns an iterator to the end of the map.
     */
    const_iterator end() const
    {
        return std::addressof(value(m_size - 1)) + 1;
    }

    /**
     * Returns an iterator to the end of the map.
     */
    const_iterator cend() const
    {
        return std::addressof(value(m_size - 1)) + 1;
    }

    /**
     * Returns a reverse end iterator to the map.
     */
    reverse_iterator rend()
    {
        return reverse_iterator(begin());
    }

    /**
     * Returns a reverse end iterator to the map.
     */
    const_reverse_iterator rend() const
    {
        return const_reverse_iterator(begin());
    }

    /**
     * Returns a reverse end iterator to the map.
     */
    const_reverse_iterator crend() const
    {
        return const_reverse_iterator(cbegin());
    }

    /**
     * Returns the first value in the map.
     */
    reference front()
    {
        return *begin();
    }

    /**
     * Returns the first value in the map.
     */
    const_reference front() const
    {
        return *begin();
    }

    /**
     * Returns the first value in the map.
     */
    reference back()
    {
        return *(end() - 1);
    }

    /**
     * Returns the first value in the map.
     */
    const_reference back() const
    {
        return *(end() - 1);
    }

    /**
     * Returns the size of the map.
     */
    size_type size() const
    {
        return m_size;
    }

    /**
     * Returns the capacity of the map.
     */
    constexpr size_type capacity() const
    {
        return Size;
    }

    /**
     * Returns true if map is empty, else false.
     */
    bool empty() const
    {
        return !m_size;
    }

    /**
     * Inserts the values in the range [first, last) to the map.
     */
    template <typename InputIterator>
    void insert(InputIterator first, InputIterator last)
    {
        std::for_each(first, last, [this](auto && value) {
            insert(std::forward<decltype(value)>(value));
        });
    }

    /**
     * Emplace a value into the map.
     */
    template <typename PairKey, typename... Arguments>
    void emplace(PairKey && key, Arguments &&... arguments)
    {
        // If empty, insert value and increment size.
        if (!m_size) {
            ::new (m_storage)
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            ++m_size;
            return;
        }

        // Find where to insert value before.
        auto [found, index] = find_index(key);

        // If we found the value, just set the new value.
        if (found) {
            *(begin() + index) =
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            return;
        }

        // If index is size, insert at the end.
        if (index == m_size) {
            ::new (m_storage + m_size)
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            ++m_size;
            return;
        }

        // Move construct the last value.
        ::new (m_storage + m_size) value_type(std::move(back()));

        // Where we want to insert our value.
        auto slot = begin() + index;

        // Move all values to the right.
        for (auto last = (end() - 1); last != slot; --last) {
            *last = std::move(*(last - 1));
        }

        // Assign to the slot.
        *slot = value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
        ++m_size;
    }

    /**
     * Insert a value to the map.
     */
    void insert(value_type value)
    {
        return emplace(std::move(value.first), std::move(value.second));
    }

    /**
     * Erases a value from the map.
     * Returns iterator past the removed element.
     */
    iterator erase(const_iterator position)
    {
        // Move all values to the left.
        auto first = begin();
        auto to_erase = first + (position - first);
        for (auto last = end() - 1; to_erase != last; ++to_erase) {
            *to_erase = std::move(*(to_erase + 1));
        }

        // Destroy value at the back.
        back().~value_type();

        // Decrement size.
        --m_size;

        // Return iterator past the last removed element.
        return first + (position - first);
    }

    /**
     * Erases values [first, last) from the map.
     */
    iterator erase(const_iterator first, const_iterator last)
    {
        // Erase [first, last).
        for (; last != first; --last) {
            erase(last - 1);
        }

        // Return last erased.
        return begin() + (first - begin());
    }

    /**
     * Returns an iterator to the found value, else returns end iterator.
     */
    iterator find(const key_type & key)
    {
        auto [found, index] = find_index(key);
        if (found) {
            return begin() + index;
        }
        return end();
    }

    /**
     * Returns an iterator to the found value, else returns end iterator.
     */
    const_iterator find(const key_type & key) const
    {
        auto [found, index] = find_index(key);
        if (found) {
            return begin() + index;
        }
        return end();
    }

private:
    /**
     * Returns the value at the specified index, the behavior is undefined
     * if index is out of range.
     */
    reference value(size_type index)
    {
        return reinterpret_cast<reference>(m_storage[index]);
    }

    /**
     * Returns the value at the specified index, the behavior is undefined
     * if index is out of range.
     */
    const_reference value(size_type index) const
    {
        return reinterpret_cast<const_reference>(m_storage[index]);
    }

    /**
     * Finds the index where key is found/to be inserted before.
     */
    std::tuple<bool, size_type> find_index(const key_type & key) const
    {
        // If empty, return not found.
        if (!m_size) {
            return {false, 0};
        }

        // Initialize first and last indices.
        key_compare compare{};
        size_type first = {};
        size_type last = m_size - 1;

        while (first != last) {
            // Get the middle index and value.
            auto middle = (first + last) / 2;
            auto middle_value = value(middle).first;

            // If key comes before middle.
            if (compare(key, middle_value)) {
                last = middle;
                continue;
            }

            // If key comes after middle.
            if (compare(middle_value, key)) {
                first = middle + 1;
                continue;
            }

            // Return the result.
            return {true, middle};
        }

        // If key comes before first, return the current index.
        if (compare(key, value(first).first)) {
            return {false, first};
        }

        // If key comes after first, return the next index.
        if (compare(value(first).first, key)) {
            return {false, first + 1};
        }

        // Returns the found index.
        return {true, first};
    }

private:
    /**
     * Storage for the values.
     */
    std::aligned_storage_t<sizeof(value_type), alignof(value_type)>
        m_storage[Size];

    /**
     * Size of the map.
     */
    size_type m_size{};
};

} // namespace zpp