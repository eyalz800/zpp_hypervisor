#pragma once

namespace zpp::crt
{
/**
 * Runs the preinit and init arrays, that is, the constructors of objects
 * with static storage duration whose initializers are not constant, plus
 * anything marked with the constructor attribute.
 *
 * Only the first call has an effect, so every CPU may call this freely.
 * The heap must already be initialized before calling, because a
 * constructor is allowed to allocate.
 */
void construct_static_objects();

/**
 * Runs destructors registered through __cxa_atexit, followed by the fini
 * array, both in reverse order of registration.
 *
 * Only the first call has an effect. Call this only when the module is
 * not staying resident - once the hypervisor is live its globals must
 * outlive every guest, so the successful path deliberately never runs
 * destructors.
 */
void destroy_static_objects();

} // namespace zpp::crt
