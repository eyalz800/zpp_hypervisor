#include "zpp/hypervisor/state.h"
#include "zpp/x64/asm.h"
#include <type_traits>

namespace zpp::hypervisor
{
static std::aligned_storage_t<sizeof(state), alignof(state)>
    g_state_storage;
state & g_state = *reinterpret_cast<state *>(&g_state_storage);

void state::create_once()
{
    static bool created = false;
    if (!created) {
        ::new (&g_state) state{};
        created = true;
    }
}

} // namespace zpp::hypervisor
