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

    return 0;
}
