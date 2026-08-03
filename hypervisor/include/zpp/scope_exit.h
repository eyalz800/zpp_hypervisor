#pragma once
#include <type_traits>
#include <utility>

namespace zpp
{
// Matches the std::scope_exit API (P0052 / C++29 P3610).
template <typename Function>
class scope_exit
{
public:
    using value_type = std::conditional_t<std::is_function_v<Function>,
                                          std::add_pointer_t<Function>,
                                          Function>;

    constexpr explicit scope_exit(Function && function) noexcept(
        std::is_nothrow_constructible_v<value_type, Function>) :
        m_function(std::forward<Function>(function)), m_active(true)
    {
    }

    constexpr scope_exit(scope_exit && other) noexcept(
        std::is_nothrow_move_constructible_v<value_type>) :
        m_function(std::move(other.m_function)), m_active(other.m_active)
    {
        other.release();
    }

    scope_exit(const scope_exit &) = delete;
    scope_exit & operator=(const scope_exit &) = delete;
    scope_exit & operator=(scope_exit &&) = delete;

    constexpr ~scope_exit()
    {
        if (m_active) {
            m_function();
        }
    }

    constexpr void release() noexcept
    {
        m_active = false;
    }

private:
    value_type m_function;
    bool m_active{};
};

template <typename Function>
scope_exit(Function) -> scope_exit<Function>;

} // namespace zpp
