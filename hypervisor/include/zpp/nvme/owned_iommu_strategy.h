#pragma once
#include "zpp/nvme/dma_reachability.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace zpp::nvme
{
/**
 * One DMA remapping hardware unit, as the firmware described it.
 */
struct remapping_unit
{
    std::uint64_t registers{};
    std::uint16_t segment{};
    bool include_pci_all{};
};

} // namespace zpp::nvme
