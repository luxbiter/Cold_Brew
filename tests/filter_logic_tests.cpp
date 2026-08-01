#include "filter_logic.h"

int main() {
    using namespace ColdBrew;

    static_assert(SlotBit(0) == 0);
    static_assert(SlotBit(1) == 0x001);
    static_assert(SlotBit(12) == 0x800);
    static_assert(SlotBit(13) == 0);

    static_assert(KeepAtLeastOneVisible(0b001u, 0b011u) == 0b001u);
    static_assert(KeepAtLeastOneVisible(0b011u, 0b011u, 0b010u) == 0b001u);
    static_assert(KeepAtLeastOneVisible(0xFFFFFFFFu, 0b1010u) == 0xFFDu);

    constexpr auto sparseMap = BuildAccountSlotMap(0b11111u, 0b11101u);
    static_assert(sparseMap.visibleCount == 1);
    static_assert(sparseMap.ToPhysical(1) == 2);
    static_assert(sparseMap.ToPhysical(2) == 0);
    static_assert(sparseMap.ToVirtual(2) == 1);
    static_assert(sparseMap.ToVirtual(1) == 0);

    constexpr auto twoAccountMap = BuildAccountSlotMap(0b10101u, 0b00100u);
    static_assert(twoAccountMap.visibleCount == 2);
    static_assert(twoAccountMap.ToPhysical(1) == 1);
    static_assert(twoAccountMap.ToPhysical(2) == 5);

    constexpr auto safetyMap = BuildAccountSlotMap(0b00101u, 0b00101u);
    static_assert(safetyMap.visibleCount == 1);
    static_assert(safetyMap.ToPhysical(1) == 1);

    return 0;
}
