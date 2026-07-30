#pragma once

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

} // namespace ColdBrew
