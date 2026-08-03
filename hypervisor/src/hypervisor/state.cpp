#include "zpp/hypervisor/state.h"
#include "zpp/x64/asm.h"

namespace zpp::hypervisor
{
alignas(state) static std::byte g_state_storage[sizeof(state)];
state & g_state = *reinterpret_cast<state *>(&g_state_storage);

void state::create_once()
{
    static bool created = false;
    if (!created) {
        // Default initialized rather than value initialized on purpose:
        // the storage lives in BSS which the loader already zeroed, so
        // value initialization would redundantly memset tens of
        // megabytes of heap storage on the boot CPU.
        ::new (&g_state) state;
        created = true;
    }
}

} // namespace zpp::hypervisor
