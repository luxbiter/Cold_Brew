#pragma once

#include <array>
#include <cstdint>

namespace ColdBrew {

constexpr uint8_t kFirstAccountSlot = 1;
constexpr uint8_t kLastAccountSlot  = 12;

constexpr bool IsValidAccountSlot(uint8_t slot) {
    return slot >= kFirstAccountSlot && slot <= kLastAccountSlot;
}

constexpr uint32_t SlotBit(uint8_t slot) {
    return IsValidAccountSlot(slot) ? (1u << (slot - kFirstAccountSlot)) : 0u;
}

constexpr uint32_t KeepAtLeastOneVisible(uint32_t hiddenMask, uint32_t occupiedMask, uint32_t preferredVisibleBit = 0) {
    constexpr uint32_t validSlotMask = (1u << kLastAccountSlot) - 1u;
    hiddenMask &= validSlotMask;
    occupiedMask &= validSlotMask;

    if (occupiedMask == 0 || (hiddenMask & occupiedMask) != occupiedMask) {
        return hiddenMask;
    }

    uint32_t bitToShow = preferredVisibleBit & occupiedMask;
    if (bitToShow == 0) {
        bitToShow = occupiedMask & (~occupiedMask + 1u);
    }
    return hiddenMask & ~bitToShow;
}

struct AccountSlotMap {
    uint8_t visibleCount = 0;
    std::array<uint8_t, kLastAccountSlot + 1> virtualToPhysical = {};
    std::array<uint8_t, kLastAccountSlot + 1> physicalToVirtual = {};

    constexpr uint8_t ToPhysical(uint8_t virtualSlot) const {
        return IsValidAccountSlot(virtualSlot) ? virtualToPhysical[virtualSlot] : 0;
    }

    constexpr uint8_t ToVirtual(uint8_t physicalSlot) const {
        return IsValidAccountSlot(physicalSlot) ? physicalToVirtual[physicalSlot] : 0;
    }
};

constexpr AccountSlotMap BuildAccountSlotMap(uint32_t occupiedMask, uint32_t hiddenMask) {
    AccountSlotMap result;
    const uint32_t effectiveHiddenMask = KeepAtLeastOneVisible(hiddenMask, occupiedMask);

    for (uint8_t physicalSlot = kFirstAccountSlot; physicalSlot <= kLastAccountSlot; ++physicalSlot) {
        const uint32_t bit = SlotBit(physicalSlot);
        if ((occupiedMask & bit) == 0 || (effectiveHiddenMask & bit) != 0) {
            continue;
        }

        ++result.visibleCount;
        result.virtualToPhysical[result.visibleCount] = physicalSlot;
        result.physicalToVirtual[physicalSlot]         = result.visibleCount;
    }

    return result;
}

} // namespace ColdBrew
