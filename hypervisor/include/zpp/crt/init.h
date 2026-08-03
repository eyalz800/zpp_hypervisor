#pragma once

namespace zpp::crt
{
/**
 * Initializes the C runtime: runs the preinit and init arrays, that is the
 * constructors of objects with static storage duration whose initializers
 * are not constant, plus anything marked with the constructor attribute.
 *
 * Only the first call has an effect, so every CPU may call this freely.
 * The global heap self initializes, so nothing needs to be ordered ahead
 * of this - a constructor is free to allocate.
 */
void init();

/**
 * Tears the C runtime down: runs destructors registered through
 * __cxa_atexit, followed by the fini array, both in reverse order of
 * registration.
 *
 * Only the first call has an effect. Call this only when the module is not
 * staying resident - once the hypervisor is live its globals must outlive
 * every guest, so the successful path deliberately never runs destructors.
 */
void fini();

} // namespace zpp::crt
