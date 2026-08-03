#pragma once
#include <atomic>

namespace zpp
{
/**
 * Hints to the processor that the caller is in a spin-wait loop.
 *
 * This matters for three reasons: it stops the core from speculating far
 * ahead into iterations that will be invalidated the moment the awaited
 * value changes, which would otherwise cost a memory order violation
 * penalty on loop exit; it yields to the sibling logical core, which may
 * be the one holding whatever is being waited for; and it lowers power
 * draw while spinning.
 */
inline void spin_hint()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}

/**
 * A lock that spins rather than blocking. There is no scheduler to yield
 * to in this environment, so this is the only lock shape available.
 *
 * Not recursive: acquiring it twice on the same CPU deadlocks.
 */
class spin_lock
{
public:
    constexpr spin_lock() = default;

    spin_lock(const spin_lock &) = delete;
    spin_lock & operator=(const spin_lock &) = delete;

    void lock()
    {
        while (m_flag.test_and_set(std::memory_order_acquire)) {
            spin_hint();
        }
    }

    void unlock()
    {
        m_flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag m_flag{};
};

} // namespace zpp
