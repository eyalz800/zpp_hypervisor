#pragma once
#include <algorithm>
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
// The only instantiation is hypervisor::module_physical_to_virtual,
// which is never copied, moved or assigned - so those members below
// have never been compiled by anything here.
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
        // The storage is inline, so there is no buffer to hand over and
        // a move is element by element. Each source is destroyed as it
        // is consumed, so zeroing other.m_size below leaks nothing.
        for (size_type i{}; i < m_size; ++i) {
            auto & other_value = other.value(i);

            ::new (std::addressof(m_storage[i]))
                value_type(std::move(other_value));

            other_value.~value_type();
        }

        other.m_size = {};
    }

    /**
     * Copy construct a small map from another one.
     */
    small_map(const small_map & other)
    {
        // The guard destroys exactly what was built if a copy fails
        // part way - inert under -fno-exceptions. Note the loop tests
        // this->m_size, which is zero, so it copies nothing.
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

        for (size_type i{}; i < m_size; ++i) {
            auto & other_value = other.value(i);

            ::new (std::addressof(m_storage[i])) value_type(other_value);

            ++m_size;
        }

        clear_guard.me = {};
    }

    /**
     * Move assign another map to this.
     * The behavior is undefined if other is this object.
     */
    small_map & operator=(small_map && other) noexcept
    {
        // Destroy first, since the storage is raw and assigning into it
        // would run value_type's assignment over bytes holding no
        // object. Also why self-assignment is undefined above - the
        // clear would destroy the elements about to be moved.
        clear();

        m_size = other.m_size;

        for (size_type i{}; i < m_size; ++i) {
            auto & other_value = other.value(i);

            ::new (std::addressof(m_storage[i]))
                value_type(std::move(other_value));

            // `other_value` is a reference, so this does not compile.
            // Never instantiated, which is why the build passes.
            other_value->~value_type();
        }

        other.m_size = {};
        return *this;
    }

    /**
     * Copy assign another map to this.
     */
    small_map & operator=(const small_map & other) noexcept
    {
        // Copy first, so a failure leaves this object untouched.
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
        // Explicit destructor calls, because m_storage is raw bytes and
        // nothing else will ever destroy what was placed in it.
        for (size_type i{}; i < m_size; ++i) {
            this->value(i).~value_type();
        }

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
    constexpr size_type size() const
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
    constexpr bool empty() const
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
        // Nothing here checks m_size against Size - bounding it is the
        // caller's job. The branches differ only in this: a slot below
        // m_size holds a live object and is assigned to, while the slot
        // at m_size is raw bytes and needs placement new.
        if (!m_size) {
            ::new (m_storage)
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            ++m_size;
            return;
        }

        auto [found, index] = find_index(key);

        // An existing key is replaced, unlike std::map::emplace, which
        // leaves the existing value alone.
        if (found) {
            *(begin() + index) =
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            return;
        }

        if (index == m_size) {
            ::new (m_storage + m_size)
                value_type(std::forward<PairKey>(key),
                           std::forward<Arguments>(arguments)...);
            ++m_size;
            return;
        }

        // Opening a hole at `index`. The new last slot is the only one
        // needing construction, so it goes first; everything below it
        // already holds an object. Backwards, so no source is
        // overwritten before it is read.
        ::new (m_storage + m_size) value_type(std::move(back()));

        auto slot = begin() + index;

        for (auto last = (end() - 1); last != slot; --last) {
            *last = std::move(*(last - 1));
        }

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
        // Shifting left overwrites live objects, so every step is an
        // assignment, and it leaves a moved-from duplicate in the last
        // slot. Only that one needs destroying - skipping it would
        // leave an object alive where the next insert placement-news.
        auto first = begin();
        auto to_erase = first + (position - first);
        for (auto last = end() - 1; to_erase != last; ++to_erase) {
            *to_erase = std::move(*(to_erase + 1));
        }

        back().~value_type();
        --m_size;

        return first + (position - first);
    }

    /**
     * Erases values [first, last) from the map.
     */
    iterator erase(const_iterator first, const_iterator last)
    {
        // Back to front, so an erase does not shift the elements still
        // to be erased out from under the iterators naming them.
        for (; last != first; --last) {
            erase(last - 1);
        }

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
        // A miss still reports where the key would belong, which is how
        // emplace avoids searching twice. The empty case is separate
        // because `m_size - 1` below would wrap on an unsigned zero.
        if (!m_size) {
            return {false, 0};
        }

        key_compare compare{};
        size_type first = {};
        size_type last = m_size - 1;

        // An inclusive range, so it ends at first == last with that one
        // element still unexamined - which is what the two comparisons
        // after the loop are for.
        while (first != last) {
            auto middle = (first + last) / 2;
            auto middle_value = value(middle).first;

            // middle, not middle - 1: it is excluded as a match but not
            // as an insertion point. The division rounds down, so
            // middle is below last here and the range still shrinks.
            if (compare(key, middle_value)) {
                last = middle;
                continue;
            }

            if (compare(middle_value, key)) {
                first = middle + 1;
                continue;
            }

            return {true, middle};
        }

        if (compare(key, value(first).first)) {
            return {false, first};
        }

        if (compare(value(first).first, key)) {
            return {false, first + 1};
        }

        return {true, first};
    }

private:
    /**
     * Storage for the values.
     */
    alignas(value_type) std::byte m_storage[Size][sizeof(value_type)];

    /**
     * Size of the map.
     */
    size_type m_size{};
};

} // namespace zpp