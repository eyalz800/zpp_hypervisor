#pragma once
#include <type_traits>

namespace zpp
{
/**
 * Accepts a function object and calls it during destruction, unless
 * operation is canceled or an explicit call is made.
 */
template <typename Function>
class scope_guard
{
public:
    /**
     * The value type to be stored.
     */
    using value_type = std::conditional_t<std::is_function_v<Function>,
                                          std::add_pointer_t<Function>,
                                          Function>;

    /**
     * Construct a scope guard from function, lvalues are being stored
     * as reference, rvalues are being moved from.
     */
    scope_guard(Function && function) :
        m_function(std::forward<Function>(function)),
        m_active(true)
    {
    }

    /**
     * Disable copy/move construction/assignment.
     * @{
     */
    scope_guard(scope_guard &&) = delete;
    scope_guard(const scope_guard &) = delete;
    scope_guard & operator=(scope_guard &&) = delete;
    scope_guard & operator=(const scope_guard &) = delete;
    /**
     * }
     */

    /**
     * Calls the function object, after disabling it.
     */
    decltype(auto) operator()()
    {
        m_active = false;
        return m_function();
    }

    /**
     * Cancels the scope guard.
     */
    void cancel()
    {
        m_active = false;
    }

    /**
     * Calls the function object if active.
     */
    ~scope_guard()
    {
        if (m_active) {
            m_function();
        }
    }

private:
    /**
     * The function object to be called.
     */
    value_type m_function;

    /**
     * Whether the scope guard is active.
     */
    bool m_active{};
};

} // namespace zpp