#pragma once

namespace zpp::arch::x86_64
{
/**
 * The VM-Exit entry function.
 */
void __attribute__((naked)) vm_exit_entry();

} // namespace zpp::arch::x86_64
