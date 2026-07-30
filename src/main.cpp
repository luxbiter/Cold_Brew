#include "filter_logic.h"

#include <array>
#include <atomic>
#include <coreinit/debug.h>
#include <coreinit/title.h>
#include <cstdio>
#include <cstring>
#include <exception>
#include <nn/act.h>
#include <string>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>
#include <wups/storage.h>

WUPS_PLUGIN_NAME("Cold Brew");
WUPS_PLUGIN_DESCRIPTION("Hides selected accounts from the Wii U boot user selector");
WUPS_PLUGIN_VERSION("v0.1.1");
WUPS_PLUGIN_AUTHOR("Cold Brew contributors");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_STORAGE("cold_brew");

namespace {

constexpr const char *kHiddenMaskStorageKey = "hidden_account_slots";
constexpr uint32_t kDefaultHiddenMask       = 0;

std::atomic<uint32_t> sHiddenMask{kDefaultHiddenMask};
uint32_t sOccupiedMask = 0;

std::array<std::string, ColdBrew::kLastAccountSlot> sConfigIdentifiers;

class ActScope {
public:
    ActScope() {
        nn::act::Initialize();
    }

    ~ActScope() {
        nn::act::Finalize();
    }

    ActScope(const ActScope &)            = delete;
    ActScope &operator=(const ActScope &) = delete;
};

bool IsWiiUMenu() {
    const uint64_t titleId = OSGetTitleID();
    return titleId == 0x0005001010040000ULL || titleId == 0x0005001010040100ULL || titleId == 0x0005001010040200ULL;
}

uint32_t ReadOriginalOccupiedMask();

} // namespace

DECL_FUNCTION(BOOL, IsSlotOccupied__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    const BOOL occupied = real_IsSlotOccupied__Q2_2nn3actFUc(slot);
    if (!occupied || !ColdBrew::IsValidAccountSlot(slot)) {
        return occupied;
    }

    const uint32_t occupiedMask = ReadOriginalOccupiedMask();
    const uint32_t effectiveHiddenMask =
            ColdBrew::KeepAtLeastOneVisible(sHiddenMask.load(std::memory_order_relaxed), occupiedMask);
    return (effectiveHiddenMask & ColdBrew::SlotBit(slot)) != 0 ? FALSE : occupied;
}

namespace {

uint32_t ReadOriginalOccupiedMask() {
    uint32_t occupiedMask = 0;
    for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
        if (real_IsSlotOccupied__Q2_2nn3actFUc(slot)) {
            occupiedMask |= ColdBrew::SlotBit(slot);
        }
    }
    return occupiedMask;
}

BOOL IsSlotOccupiedUnfiltered(nn::act::SlotNo slot) {
    if (IsWiiUMenu() && real_IsSlotOccupied__Q2_2nn3actFUc != nullptr) {
        return real_IsSlotOccupied__Q2_2nn3actFUc(slot);
    }
    return nn::act::IsSlotOccupied(slot);
}

std::string Utf16ToUtf8(const int16_t *text, size_t capacity) {
    std::string result;
    result.reserve(capacity * 3);

    for (size_t index = 0; index < capacity && text[index] != 0; ++index) {
        uint32_t codePoint = static_cast<uint16_t>(text[index]);

        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < capacity) {
            const uint32_t low = static_cast<uint16_t>(text[index + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10u) + (low - 0xDC00);
                ++index;
            }
        }

        if (codePoint <= 0x7F) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (codePoint >> 6u)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (codePoint >> 12u)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6u) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (codePoint >> 18u)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 12u) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6u) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    return result;
}

std::string GetAccountDisplayName(nn::act::SlotNo slot) {
    int16_t miiName[nn::act::MiiNameSize] = {};
    char accountId[nn::act::AccountIdSize] = {};

    nn::act::GetMiiNameEx(miiName, slot);
    nn::act::GetAccountIdEx(accountId, slot);
    accountId[nn::act::AccountIdSize - 1] = '\0';

    const std::string utf8MiiName = Utf16ToUtf8(miiName, nn::act::MiiNameSize);
    std::string displayName       = utf8MiiName.empty() ? "Unnamed account" : utf8MiiName;

    if (accountId[0] != '\0') {
        displayName += " (";
        displayName += accountId;
        displayName += ")";
    }

    char slotSuffix[16] = {};
    std::snprintf(slotSuffix, sizeof(slotSuffix), " [slot %u]", static_cast<unsigned int>(slot));
    displayName += slotSuffix;
    return displayName;
}

uint32_t ReadOccupiedMask() {
    uint32_t occupiedMask = 0;

    const ActScope actScope;
    for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
        if (IsSlotOccupiedUnfiltered(slot)) {
            occupiedMask |= ColdBrew::SlotBit(slot);
        }
    }

    return occupiedMask;
}

void HiddenAccountChanged(ConfigItemBoolean *item, bool hidden) {
    if (item == nullptr || item->identifier == nullptr) {
        return;
    }

    uint8_t selectedSlot = 0;
    for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
        if (sConfigIdentifiers[slot - 1] == item->identifier) {
            selectedSlot = slot;
            break;
        }
    }
    if (!ColdBrew::IsValidAccountSlot(selectedSlot)) {
        return;
    }

    const uint32_t selectedBit = ColdBrew::SlotBit(selectedSlot);
    uint32_t newMask           = sHiddenMask.load(std::memory_order_relaxed);
    if (hidden) {
        newMask |= selectedBit;
    } else {
        newMask &= ~selectedBit;
    }

    newMask = ColdBrew::KeepAtLeastOneVisible(newMask, sOccupiedMask, selectedBit);
    sHiddenMask.store(newMask, std::memory_order_relaxed);
    WUPSStorageAPI::Store(kHiddenMaskStorageKey, newMask);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);
        root.add(WUPSConfigItemStub::Create("Choose which accounts are shown in the boot user selector."));

        sOccupiedMask      = ReadOccupiedMask();
        uint32_t hiddenMask = ColdBrew::KeepAtLeastOneVisible(sHiddenMask.load(std::memory_order_relaxed), sOccupiedMask);
        sHiddenMask.store(hiddenMask, std::memory_order_relaxed);
        WUPSStorageAPI::Store(kHiddenMaskStorageKey, hiddenMask);

        if (sOccupiedMask == 0) {
            root.add(WUPSConfigItemStub::Create("No Wii U accounts were found."));
            return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
        }

        std::array<std::string, ColdBrew::kLastAccountSlot> accountDisplayNames;
        {
            const ActScope actScope;
            for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
                const uint32_t slotBit = ColdBrew::SlotBit(slot);
                if ((sOccupiedMask & slotBit) != 0) {
                    accountDisplayNames[slot - 1] = GetAccountDisplayName(slot);
                }
            }
        }

        for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
            const uint32_t slotBit = ColdBrew::SlotBit(slot);
            if ((sOccupiedMask & slotBit) == 0) {
                continue;
            }
            sConfigIdentifiers[slot - 1] = "hide_slot_" + std::to_string(slot);
            root.add(WUPSConfigItemBoolean::CreateEx(sConfigIdentifiers[slot - 1], accountDisplayNames[slot - 1], false,
                                                     (hiddenMask & slotBit) != 0, HiddenAccountChanged, "Hidden", "Visible"));
        }
    } catch (const std::exception &exception) {
        OSReport("Cold Brew: failed to create config menu: %s\n", exception.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    WUPSStorageAPI::SaveStorage();
}

} // namespace

INITIALIZE_PLUGIN() {
    uint32_t storedMask = kDefaultHiddenMask;
    const WUPSStorageError storageResult =
            WUPSStorageAPI::GetOrStoreDefault(kHiddenMaskStorageKey, storedMask, kDefaultHiddenMask);
    if (storageResult == WUPS_STORAGE_ERROR_SUCCESS) {
        sHiddenMask.store(storedMask, std::memory_order_relaxed);
        WUPSStorageAPI::SaveStorage();
    } else {
        OSReport("Cold Brew: failed to load settings: %s\n", WUPSStorageAPI_GetStatusStr(storageResult));
    }

    const WUPSConfigAPIOptionsV1 configOptions = {.name = "Cold Brew"};
    if (WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) != WUPSCONFIG_API_RESULT_SUCCESS) {
        OSReport("Cold Brew: failed to initialize the config menu\n");
    }
}

WUPS_MUST_REPLACE_FOR_PROCESS(IsSlotOccupied__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT, IsSlotOccupied__Q2_2nn3actFUc,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
