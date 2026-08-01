#include "filter_logic.h"

#include <array>
#include <atomic>
#include <coreinit/debug.h>
#include <coreinit/launch.h>
#include <coreinit/thread.h>
#include <coreinit/title.h>
#include <cstdint>
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

#if defined(COLD_BREW_TRACE)
#define COLD_BREW_BUILD_VERSION "v0.1.2-trace16"
#else
#define COLD_BREW_BUILD_VERSION "v0.1.2"
#endif

WUPS_PLUGIN_NAME("Cold Brew");
WUPS_PLUGIN_DESCRIPTION("Hides selected accounts from the Wii U boot user selector");
WUPS_PLUGIN_VERSION(COLD_BREW_BUILD_VERSION);
WUPS_PLUGIN_AUTHOR("Cold Brew contributors");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_STORAGE("cold_brew");

namespace {

constexpr const char *kHiddenMaskStorageKey = "hidden_account_slots";
constexpr uint32_t kDefaultHiddenMask       = 0;

std::atomic<uint32_t> sHiddenMask{kDefaultHiddenMask};
std::atomic<uint32_t> sPendingHiddenMask{kDefaultHiddenMask};
std::atomic<bool> sSlotMapReady{false};
std::atomic<uint32_t> sVisibleAccountMask{0};
// ACT stores its active account as a physical slot.  The user selector,
// however, only knows the compact virtual slots exposed by this plugin.  Keep
// a mapping for the account selected through our translated loader call, and
// only use it when the menu reads the active slot afterwards.
std::atomic<uint32_t> sSelectedPhysicalSlot{0};
std::atomic<uint32_t> sSelectedVirtualSlot{0};
uint32_t sOccupiedMask = 0;

std::array<std::string, ColdBrew::kLastAccountSlot> sConfigIdentifiers;

// Function hooks can execute simultaneously on several Wii U Menu threads.
// WUPS does not provide usable ELF TLS for plugins, so retain bypass depth by
// OSThread instead of using a process-wide flag that would make other threads
// unexpectedly bypass the virtual-slot mapping.
constexpr size_t kBypassEntryCount = 128;

struct ThreadBypassEntry {
    std::atomic<uintptr_t> thread{0};
    std::atomic<uint32_t> depth{0};
};

std::array<ThreadBypassEntry, kBypassEntryCount> sThreadBypassEntries;

#if defined(COLD_BREW_TRACE)
// WHB's UDP logger can outlive the network service during OSRestartGame and
// then dereference a torn-down socket.  OSReport is forwarded by Aroma's TCP
// syslog and does not make a network call from the plugin itself.
void InitializeTraceLogging() {
    OSReport("[Cold Brew trace] logging through OSReport\\n");
}

void FinalizeTraceLogging() {
}

#define COLD_BREW_TRACE_LOG(...) OSReport("[Cold Brew trace] " __VA_ARGS__)
#else
void InitializeTraceLogging() {
}

void FinalizeTraceLogging() {
}

#define COLD_BREW_TRACE_LOG(...) ((void) 0)
#endif

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

ThreadBypassEntry *FindThreadBypassEntry(bool claimEmptyEntry) {
    const uintptr_t currentThread = reinterpret_cast<uintptr_t>(OSGetCurrentThread());
    if (currentThread == 0) {
        return nullptr;
    }

    const size_t firstIndex = (currentThread >> 4u) % kBypassEntryCount;
    for (size_t offset = 0; offset < kBypassEntryCount; ++offset) {
        ThreadBypassEntry &entry = sThreadBypassEntries[(firstIndex + offset) % kBypassEntryCount];
        const uintptr_t owner    = entry.thread.load(std::memory_order_acquire);
        if (owner == currentThread) {
            return &entry;
        }
        if (owner != 0 || !claimEmptyEntry) {
            continue;
        }

        uintptr_t expected = 0;
        if (entry.thread.compare_exchange_strong(expected, currentThread, std::memory_order_acq_rel,
                                                 std::memory_order_acquire) ||
            expected == currentThread) {
            return &entry;
        }
    }

    return nullptr;
}

class SlotMappingBypassScope {
public:
    SlotMappingBypassScope() {
        mEntry = FindThreadBypassEntry(true);
        if (mEntry != nullptr) {
            mEntry->depth.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ~SlotMappingBypassScope() {
        if (mEntry != nullptr) {
            mEntry->depth.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    SlotMappingBypassScope(const SlotMappingBypassScope &)            = delete;
    SlotMappingBypassScope &operator=(const SlotMappingBypassScope &) = delete;

private:
    ThreadBypassEntry *mEntry = nullptr;
};

bool IsSlotMappingBypassed() {
    ThreadBypassEntry *entry = FindThreadBypassEntry(false);
    return entry != nullptr && entry->depth.load(std::memory_order_relaxed) != 0;
}

ColdBrew::AccountSlotMap BuildCurrentSlotMap();
ColdBrew::AccountSlotMap GetSlotMapSnapshot();
ColdBrew::AccountSlotMap RefreshSlotMap();
nn::act::SlotNo ResolveVirtualSlot(nn::act::SlotNo virtualSlot);
nn::act::SlotNo TranslateSelectedPhysicalSlot(nn::act::SlotNo physicalSlot);
void EnsureSlotMapReady();
void RememberSelectedVirtualSlot(nn::act::SlotNo virtualSlot, nn::act::SlotNo physicalSlot, int32_t result);

} // namespace

DECL_FUNCTION(uint8_t, GetNumOfAccounts__Q2_2nn3actFv) {
    if (IsSlotMappingBypassed()) {
        return real_GetNumOfAccounts__Q2_2nn3actFv();
    }

    const ColdBrew::AccountSlotMap slotMap = RefreshSlotMap();
    COLD_BREW_TRACE_LOG("GetNumOfAccounts -> %u [v1=%u v2=%u v3=%u v4=%u v5=%u v6=%u]\\n", slotMap.visibleCount,
                         slotMap.ToPhysical(1), slotMap.ToPhysical(2), slotMap.ToPhysical(3), slotMap.ToPhysical(4),
                         slotMap.ToPhysical(5), slotMap.ToPhysical(6));
    return slotMap.visibleCount;
}

DECL_FUNCTION(BOOL, IsSlotOccupied__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsSlotOccupied__Q2_2nn3actFUc(slot);
    }

    // The Wii U Menu probes every slot from 1 through 12 even after it has
    // obtained the (filtered) account count.  A virtual slot that is not in
    // the compact visible map must therefore be reported as empty; falling
    // back to the same physical number leaks a hidden account back into that
    // probe sequence.
    EnsureSlotMapReady();
    const nn::act::SlotNo physicalSlot = GetSlotMapSnapshot().ToPhysical(slot);
    COLD_BREW_TRACE_LOG("IsSlotOccupied input=%u physical=%u\\n", slot, physicalSlot);
    if (!ColdBrew::IsValidAccountSlot(physicalSlot)) {
        return FALSE;
    }

    const SlotMappingBypassScope bypass;
    return real_IsSlotOccupied__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(nn::Result, GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc, FFLStoreData *mii, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc(mii, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc(mii, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc,
              size_t *outImageSize, void *buffer, size_t bufferSize, int imageType, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc(outImageSize, buffer, bufferSize, imageType, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc(outImageSize, buffer, bufferSize, imageType, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetMiiNameEx__Q2_2nn3actFPwUc, int16_t *name, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetMiiNameEx__Q2_2nn3actFPwUc(name, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetMiiNameEx__Q2_2nn3actFPwUc(name, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetAccountIdEx__Q2_2nn3actFPcUc, char *accountId, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetAccountIdEx__Q2_2nn3actFPcUc(accountId, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetAccountIdEx__Q2_2nn3actFPcUc(accountId, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetAccountInfoEx__Q2_2nn3actFP14ACTAccountInfoUc, void *accountInfo,
              nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetAccountInfoEx__Q2_2nn3actFP14ACTAccountInfoUc(accountInfo, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetAccountInfoEx__Q2_2nn3actFP14ACTAccountInfoUc(accountInfo, physicalSlot);
}

DECL_FUNCTION(int32_t, GetLastAuthenticationResultEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetLastAuthenticationResultEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetLastAuthenticationResultEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(nn::Result, GetBirthdayEx__Q2_2nn3actFPUsPUcT2Uc,
              uint16_t *year, uint8_t *month, uint8_t *day, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetBirthdayEx__Q2_2nn3actFPUsPUcT2Uc(year, month, day, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetBirthdayEx__Q2_2nn3actFPUsPUcT2Uc(year, month, day, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetNfsPasswordEx__Q2_2nn3actFPcUc, char *password, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetNfsPasswordEx__Q2_2nn3actFPcUc(password, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetNfsPasswordEx__Q2_2nn3actFPcUc(password, physicalSlot);
}

DECL_FUNCTION(nn::act::PersistentId, GetPersistentIdEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetPersistentIdEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetPersistentIdEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(nn::Result, GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc,
              nn::act::SlotNo *outSlot, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc(outSlot, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc(outSlot, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetPrincipalIdEx__Q2_2nn3actFPUiUc,
              nn::act::PrincipalId *outId, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetPrincipalIdEx__Q2_2nn3actFPUiUc(outId, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetPrincipalIdEx__Q2_2nn3actFPUiUc(outId, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetSimpleAddressIdEx__Q2_2nn3actFPUiUc,
              nn::act::SimpleAddressId *outId, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetSimpleAddressIdEx__Q2_2nn3actFPUiUc(outId, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetSimpleAddressIdEx__Q2_2nn3actFPUiUc(outId, physicalSlot);
}

DECL_FUNCTION(BOOL, IsNetworkAccountEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsNetworkAccountEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_IsNetworkAccountEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(BOOL, IsPasswordCacheEnabledEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsPasswordCacheEnabledEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_IsPasswordCacheEnabledEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(BOOL, IsCommittedEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsCommittedEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_IsCommittedEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(BOOL, IsServerAccountActiveEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsServerAccountActiveEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_IsServerAccountActiveEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(BOOL, IsServerAccountDeletedEx__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_IsServerAccountDeletedEx__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_IsServerAccountDeletedEx__Q2_2nn3actFUc(physicalSlot);
}

DECL_FUNCTION(nn::Result, GetTransferableIdEx__Q2_2nn3actFPULUiUc,
              nn::act::TransferrableId *id, uint32_t unknown, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetTransferableIdEx__Q2_2nn3actFPULUiUc(id, unknown, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    return real_GetTransferableIdEx__Q2_2nn3actFPULUiUc(id, unknown, physicalSlot);
}

DECL_FUNCTION(nn::Result, GetUuidEx__Q2_2nn3actFP7ACTUuidUc, char *uuid, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_GetUuidEx__Q2_2nn3actFP7ACTUuidUc(uuid, slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    const nn::Result result = real_GetUuidEx__Q2_2nn3actFP7ACTUuidUc(uuid, physicalSlot);
    COLD_BREW_TRACE_LOG("GetUuidEx(uuid) input=%u physical=%u\\n", slot, physicalSlot);
    return result;
}

DECL_FUNCTION(nn::Result, GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi, char *uuid, nn::act::SlotNo slot, int32_t unknown) {
    if (IsSlotMappingBypassed()) {
        return real_GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi(uuid, slot, unknown);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    const nn::Result result = real_GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi(uuid, physicalSlot, unknown);
    COLD_BREW_TRACE_LOG("GetUuidEx(uuid,mode) input=%u physical=%u mode=%d\\n", slot, physicalSlot, unknown);
    return result;
}

// The active ACT account is physical, while the selector is rendered from the
// compact virtual slots.  Convert only the physical slot recorded after a
// successful translated selection.  Calls before selection deliberately pass
// through unchanged: the selector can still have a different active account
// while it is being constructed.
DECL_FUNCTION(nn::act::SlotNo, GetSlotNo__Q2_2nn3actFv) {
    if (IsSlotMappingBypassed()) {
        return real_GetSlotNo__Q2_2nn3actFv();
    }

    const SlotMappingBypassScope bypass;
    const nn::act::SlotNo physicalSlot = real_GetSlotNo__Q2_2nn3actFv();
    const nn::act::SlotNo virtualSlot   = TranslateSelectedPhysicalSlot(physicalSlot);
    COLD_BREW_TRACE_LOG("GetSlotNo physical=%u virtual=%u selected-physical=%u\\n", physicalSlot, virtualSlot,
                         sSelectedPhysicalSlot.load(std::memory_order_relaxed));
    return virtualSlot;
}

DECL_FUNCTION(nn::act::SlotNo, GetDefaultAccount__Q2_2nn3actFv) {
    if (IsSlotMappingBypassed()) {
        return real_GetDefaultAccount__Q2_2nn3actFv();
    }

    const SlotMappingBypassScope bypass;
    const nn::act::SlotNo physicalSlot = real_GetDefaultAccount__Q2_2nn3actFv();
    const nn::act::SlotNo virtualSlot  = TranslateSelectedPhysicalSlot(physicalSlot);
    COLD_BREW_TRACE_LOG("GetDefaultAccount physical=%u virtual=%u\\n", physicalSlot, virtualSlot);
    return virtualSlot;
}

DECL_FUNCTION(int32_t, LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb,
              nn::act::SlotNo slot, nn::act::ACTLoadOption option, const char *password, bool useCache) {
    if (IsSlotMappingBypassed()) {
        return real_LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb(slot, option, password, useCache);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    const int32_t result = real_LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb(physicalSlot, option, password, useCache);
    RememberSelectedVirtualSlot(slot, physicalSlot, result);
    COLD_BREW_TRACE_LOG("LoadConsoleAccount input=%u physical=%u option=%u result=%08X\\n", slot, physicalSlot, option,
                         static_cast<uint32_t>(result));
    return result;
}

DECL_FUNCTION(int32_t, SetDefaultAccount__Q2_2nn3actFUc, nn::act::SlotNo slot) {
    if (IsSlotMappingBypassed()) {
        return real_SetDefaultAccount__Q2_2nn3actFUc(slot);
    }

    const nn::act::SlotNo physicalSlot = ResolveVirtualSlot(slot);
    const SlotMappingBypassScope bypass;
    const int32_t result = real_SetDefaultAccount__Q2_2nn3actFUc(physicalSlot);
    RememberSelectedVirtualSlot(slot, physicalSlot, result);
    COLD_BREW_TRACE_LOG("SetDefaultAccount input=%u physical=%u result=%08X\\n", slot, physicalSlot,
                         static_cast<uint32_t>(result));
    return result;
}

namespace {

BOOL IsSlotOccupiedUnfiltered(nn::act::SlotNo slot) {
    if (IsWiiUMenu() && real_IsSlotOccupied__Q2_2nn3actFUc != nullptr) {
        return real_IsSlotOccupied__Q2_2nn3actFUc(slot);
    }
    return nn::act::IsSlotOccupied(slot);
}

ColdBrew::AccountSlotMap BuildCurrentSlotMap() {
    uint32_t occupiedMask = 0;
    const SlotMappingBypassScope bypass;
    for (uint8_t slot = ColdBrew::kFirstAccountSlot; slot <= ColdBrew::kLastAccountSlot; ++slot) {
        if (real_IsSlotOccupied__Q2_2nn3actFUc(slot)) {
            occupiedMask |= ColdBrew::SlotBit(slot);
        }
    }
    return ColdBrew::BuildAccountSlotMap(occupiedMask, sHiddenMask.load(std::memory_order_relaxed));
}

uint32_t VisibleMaskFromSlotMap(const ColdBrew::AccountSlotMap &slotMap) {
    uint32_t visibleMask = 0;
    for (uint8_t physicalSlot = ColdBrew::kFirstAccountSlot; physicalSlot <= ColdBrew::kLastAccountSlot; ++physicalSlot) {
        if (slotMap.ToVirtual(physicalSlot) != 0) {
            visibleMask |= ColdBrew::SlotBit(physicalSlot);
        }
    }
    return visibleMask;
}

void PublishSlotMap(const ColdBrew::AccountSlotMap &slotMap) {
    sVisibleAccountMask.store(VisibleMaskFromSlotMap(slotMap), std::memory_order_release);
    sSlotMapReady.store(true, std::memory_order_release);
}

ColdBrew::AccountSlotMap GetSlotMapSnapshot() {
    const uint32_t visibleMask = sVisibleAccountMask.load(std::memory_order_acquire);
    return ColdBrew::BuildAccountSlotMap(visibleMask, 0);
}

ColdBrew::AccountSlotMap RefreshSlotMap() {
    const ColdBrew::AccountSlotMap slotMap = BuildCurrentSlotMap();
    PublishSlotMap(slotMap);
    return slotMap;
}

nn::act::SlotNo ResolveVirtualSlot(nn::act::SlotNo virtualSlot) {
    if (!ColdBrew::IsValidAccountSlot(virtualSlot)) {
        return virtualSlot;
    }

    EnsureSlotMapReady();
    if (!sSlotMapReady.load(std::memory_order_acquire)) {
        return virtualSlot;
    }

    const nn::act::SlotNo physicalSlot = GetSlotMapSnapshot().ToPhysical(virtualSlot);
    return physicalSlot != 0 ? physicalSlot : virtualSlot;
}

void EnsureSlotMapReady() {
    if (sSlotMapReady.load(std::memory_order_acquire)) {
        return;
    }

    PublishSlotMap(BuildCurrentSlotMap());
}

nn::act::SlotNo TranslateSelectedPhysicalSlot(nn::act::SlotNo physicalSlot) {
    const uint32_t selectedPhysicalSlot = sSelectedPhysicalSlot.load(std::memory_order_acquire);
    const uint32_t selectedVirtualSlot  = sSelectedVirtualSlot.load(std::memory_order_relaxed);
    return physicalSlot == selectedPhysicalSlot && ColdBrew::IsValidAccountSlot(selectedVirtualSlot)
                   ? static_cast<nn::act::SlotNo>(selectedVirtualSlot)
                   : physicalSlot;
}

void RememberSelectedVirtualSlot(nn::act::SlotNo virtualSlot, nn::act::SlotNo physicalSlot, int32_t result) {
    if (result != 0) {
        return;
    }

    if (virtualSlot != physicalSlot && ColdBrew::IsValidAccountSlot(virtualSlot) &&
        ColdBrew::IsValidAccountSlot(physicalSlot)) {
        // Publish the virtual value first.  An acquiring reader of the
        // physical slot can then safely use its matching virtual slot.
        sSelectedVirtualSlot.store(virtualSlot, std::memory_order_relaxed);
        sSelectedPhysicalSlot.store(physicalSlot, std::memory_order_release);
        return;
    }

    // A native (not translated) selection must not inherit a previous mapping.
    sSelectedPhysicalSlot.store(0, std::memory_order_release);
    sSelectedVirtualSlot.store(0, std::memory_order_relaxed);
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

    if (IsWiiUMenu() && real_GetMiiNameEx__Q2_2nn3actFPwUc != nullptr &&
        real_GetAccountIdEx__Q2_2nn3actFPcUc != nullptr) {
        real_GetMiiNameEx__Q2_2nn3actFPwUc(miiName, slot);
        real_GetAccountIdEx__Q2_2nn3actFPcUc(accountId, slot);
    } else {
        nn::act::GetMiiNameEx(miiName, slot);
        nn::act::GetAccountIdEx(accountId, slot);
    }
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
    const SlotMappingBypassScope bypass;
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
    uint32_t newMask           = sPendingHiddenMask.load(std::memory_order_relaxed);
    if (hidden) {
        newMask |= selectedBit;
    } else {
        newMask &= ~selectedBit;
    }

    newMask = ColdBrew::KeepAtLeastOneVisible(newMask, sOccupiedMask, selectedBit);
    sPendingHiddenMask.store(newMask, std::memory_order_relaxed);
    WUPSStorageAPI::Store(kHiddenMaskStorageKey, newMask);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);
        root.add(WUPSConfigItemStub::Create("Choose which accounts are shown in the boot user selector."));

        sOccupiedMask      = ReadOccupiedMask();
        uint32_t hiddenMask = ColdBrew::KeepAtLeastOneVisible(sPendingHiddenMask.load(std::memory_order_relaxed), sOccupiedMask);
        sPendingHiddenMask.store(hiddenMask, std::memory_order_relaxed);
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

    const uint32_t activeMask  = sHiddenMask.load(std::memory_order_relaxed);
    const uint32_t pendingMask = sPendingHiddenMask.load(std::memory_order_relaxed);
    if (pendingMask != activeMask) {
        sHiddenMask.store(pendingMask, std::memory_order_relaxed);
        sSlotMapReady.store(false, std::memory_order_release);
        OSReport("Cold Brew: settings changed; restarting Wii U Menu to apply them\n");
        OSRestartGame(0, nullptr);
    }
}

} // namespace

INITIALIZE_PLUGIN() {
    InitializeTraceLogging();
    COLD_BREW_TRACE_LOG("Cold Brew trace build initialized\\n");

    sSlotMapReady.store(false, std::memory_order_relaxed);
    sVisibleAccountMask.store(0, std::memory_order_relaxed);
    sSelectedPhysicalSlot.store(0, std::memory_order_relaxed);
    sSelectedVirtualSlot.store(0, std::memory_order_relaxed);

    uint32_t storedMask = kDefaultHiddenMask;
    const WUPSStorageError storageResult =
            WUPSStorageAPI::GetOrStoreDefault(kHiddenMaskStorageKey, storedMask, kDefaultHiddenMask);
    if (storageResult == WUPS_STORAGE_ERROR_SUCCESS) {
        sHiddenMask.store(storedMask, std::memory_order_relaxed);
        sPendingHiddenMask.store(storedMask, std::memory_order_relaxed);
        WUPSStorageAPI::SaveStorage();
    } else {
        OSReport("Cold Brew: failed to load settings: %s\n", WUPSStorageAPI_GetStatusStr(storageResult));
    }

    const WUPSConfigAPIOptionsV1 configOptions = {.name = "Cold Brew"};
    if (WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) != WUPSCONFIG_API_RESULT_SUCCESS) {
        OSReport("Cold Brew: failed to initialize the config menu\n");
    }
}

DEINITIALIZE_PLUGIN() {
    COLD_BREW_TRACE_LOG("Cold Brew trace build deinitialized\\n");
    FinalizeTraceLogging();
}

WUPS_MUST_REPLACE_FOR_PROCESS(IsSlotOccupied__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT, IsSlotOccupied__Q2_2nn3actFUc,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetNumOfAccounts__Q2_2nn3actFv, WUPS_LOADER_LIBRARY_NN_ACT, GetNumOfAccounts__Q2_2nn3actFv,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetMiiNameEx__Q2_2nn3actFPwUc, WUPS_LOADER_LIBRARY_NN_ACT, GetMiiNameEx__Q2_2nn3actFPwUc,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetAccountIdEx__Q2_2nn3actFPcUc, WUPS_LOADER_LIBRARY_NN_ACT, GetAccountIdEx__Q2_2nn3actFPcUc,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetAccountInfoEx__Q2_2nn3actFP14ACTAccountInfoUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetAccountInfoEx__Q2_2nn3actFP14ACTAccountInfoUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetLastAuthenticationResultEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetLastAuthenticationResultEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetBirthdayEx__Q2_2nn3actFPUsPUcT2Uc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetBirthdayEx__Q2_2nn3actFPUsPUcT2Uc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetNfsPasswordEx__Q2_2nn3actFPcUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetNfsPasswordEx__Q2_2nn3actFPcUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetPersistentIdEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetPersistentIdEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetPrincipalIdEx__Q2_2nn3actFPUiUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetPrincipalIdEx__Q2_2nn3actFPUiUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetSimpleAddressIdEx__Q2_2nn3actFPUiUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetSimpleAddressIdEx__Q2_2nn3actFPUiUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(IsNetworkAccountEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              IsNetworkAccountEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(IsPasswordCacheEnabledEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              IsPasswordCacheEnabledEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(IsCommittedEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              IsCommittedEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(IsServerAccountActiveEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              IsServerAccountActiveEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(IsServerAccountDeletedEx__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              IsServerAccountDeletedEx__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetTransferableIdEx__Q2_2nn3actFPULUiUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetTransferableIdEx__Q2_2nn3actFPULUiUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetUuidEx__Q2_2nn3actFP7ACTUuidUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetUuidEx__Q2_2nn3actFP7ACTUuidUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetSlotNo__Q2_2nn3actFv, WUPS_LOADER_LIBRARY_NN_ACT, GetSlotNo__Q2_2nn3actFv,
                              WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(GetDefaultAccount__Q2_2nn3actFv, WUPS_LOADER_LIBRARY_NN_ACT,
                              GetDefaultAccount__Q2_2nn3actFv, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb, WUPS_LOADER_LIBRARY_NN_ACT,
                              LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
WUPS_MUST_REPLACE_FOR_PROCESS(SetDefaultAccount__Q2_2nn3actFUc, WUPS_LOADER_LIBRARY_NN_ACT,
                              SetDefaultAccount__Q2_2nn3actFUc, WUPS_FP_TARGET_PROCESS_WII_U_MENU);
