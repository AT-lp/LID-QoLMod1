#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace config {
    constexpr std::uintptr_t kGObjectsOffset = 0x2949CE0;
    constexpr std::size_t kProcessEventVtableOffset = 0x218;
    constexpr std::size_t kOuterOffset = 0x40;
    constexpr std::size_t kClassOffset = 0x50;
    constexpr std::size_t kNameIndexOffset = 0x48;
    constexpr std::size_t kUPropertyMemberOffsetField = 0x94;
    constexpr unsigned char kNameArrayPattern[] = { 0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0x0C, 0xD9,
                                                   0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x00, 0x48, 0x8B, 0xC7 };
    constexpr char kNameArrayMask[] = "xxx????xxxxxxxx????xxxx?xxx";
    constexpr std::uint8_t kStorageIcon = 4;
    constexpr std::uint8_t kShopIcon = 5;
    static_assert(sizeof(kNameArrayPattern) == sizeof(kNameArrayMask) - 1);
} // namespace config

using ProcessEvent = void(__fastcall*)(std::uintptr_t* object, std::uintptr_t function, char* parameters);

struct GObjects {
    std::uintptr_t* pArray;
    std::uint32_t Count;
    std::uint32_t Max;
};

struct FStringParam {
    wchar_t* Data;
    std::int32_t Count;
    std::int32_t Max;
};

struct TArrayHeader {
    std::uintptr_t Data;
    std::int32_t Count;
    std::int32_t Max;
};

struct PropertyInfo {
    std::uintptr_t object = 0;
    std::uint32_t offset = 0;
    char name[128]{};
    char className[128]{};
};

struct UnitLayout {
    bool valid = false;
    std::uint32_t iconOffset = 0;
    std::uint32_t nameOffset = 0;
    std::uint32_t boolOffset = 0;
    std::uint8_t hideMask = 0;
    std::uint8_t disableSelectMask = 0;
    std::uint8_t disableChangeNameMask = 0;
    std::uint8_t noSelectSoundMask = 0;
    std::uint8_t newMarkMask = 0;
};

static std::uintptr_t g_moduleBase = 0;
static std::size_t g_moduleSize = 0;
static std::uintptr_t g_nameArrayAddress = 0;
static GObjects* g_objects = nullptr;
static ProcessEvent g_processEvent = nullptr;

static std::uintptr_t g_commonTop2Class = 0;
static std::uintptr_t g_unitStruct = 0;
static std::uintptr_t g_setUnitArrayFunction = 0;
static std::uintptr_t g_startCommonTop2Function = 0;
static std::uintptr_t g_getSelectIndexFunction = 0;
static std::uintptr_t g_checkInIdleFunction = 0;
static std::uintptr_t g_checkCancelFunction = 0;
static std::uintptr_t g_optionCheckEndFunction = 0;
static std::uintptr_t g_optionStartWaitFunction = 0;
static std::uintptr_t g_optionClass = 0;
static std::uintptr_t g_coinLockerFunction = 0;
static std::uintptr_t g_partShopFunction = 0;
static std::uintptr_t g_freeCameraFunction = 0;

static bool g_haveUnitArrayOffset = false;
static std::uint32_t g_unitArrayOffset = 0;
static bool g_haveGetSelectIndexReturnOffset = false;
static std::uint32_t g_getSelectIndexReturnOffset = 0;
static bool g_haveCheckInIdleReturnOffset = false;
static std::uint32_t g_checkInIdleReturnOffset = 0;
static bool g_haveCheckCancelReturnOffset = false;
static std::uint32_t g_checkCancelReturnOffset = 0;
static bool g_haveOptionCheckEndReturnOffset = false;
static std::uint32_t g_optionCheckEndReturnOffset = 0;

static bool g_haveStartWaitLayout = false;
static std::uint32_t g_startWaitFloatOffset = 0;
static std::uint32_t g_startWaitStateOffset = 0;

static bool g_haveExitGameLayout = false;
static std::uint32_t g_exitGameOffset = 0;
static std::uint8_t g_exitGameMask = 0;

static bool g_haveMenuStateOffset = false;
static std::uint32_t g_menuStateOffset = 0;

static UnitLayout g_unitLayout{};
static std::vector<PropertyInfo> g_startCommonTop2Params;
static std::vector<std::uintptr_t> g_commonTop2Candidates;

static std::vector<std::uint8_t> g_mainUnits;
static std::uintptr_t g_activeMenuObject = 0;

static wchar_t g_menuTitle[] = L"MENU";
static wchar_t g_coinLockerText[] = L"Storage";
static wchar_t g_partShopText[] = L"Shop";

static bool g_idleSelectionHandled = false;

static int g_pendingMenuAction = 0;
static int g_pendingMenuPhase = 0;
static ULONGLONG g_pendingMenuStartedAt = 0;
static std::uintptr_t g_liveGameInfo = 0;
static std::uintptr_t g_liveOptionMenu = 0;
static void** g_gameInfoProcessEventSlot = nullptr;
static ProcessEvent g_originalGameInfoProcessEvent = nullptr;
static std::uintptr_t g_gameInfoTickFunction = 0;
static bool g_f5WasDown = false;

static std::int32_t g_lastObservedOptionState = -999;
static bool g_injectedForTopMenuState = false;

static bool SafeReadPointer(const void* address, std::uintptr_t* value) {
    if (!address || !value) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return false;
    }
}

static bool SafeReadUInt32(const void* address, std::uint32_t* value) {
    if (!address || !value) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const std::uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return false;
    }
}

static bool SafeReadInt32(const void* address, std::int32_t* value) {
    if (!address || !value) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const std::int32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return false;
    }
}

static bool SafeReadByte(const void* address, std::uint8_t* value) {
    if (!address || !value) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const std::uint8_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return false;
    }
}

static bool SafeReadArrayHeader(std::uintptr_t address, TArrayHeader* value) {
    if (!address || !value) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const TArrayHeader*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = {};
        return false;
    }
}

static bool SafeReadObjectTable(GObjects* table, std::uintptr_t** array, std::uint32_t* count) {
    if (!table || !array || !count) {
        return false;
    }
    __try {
        *array = table->pArray;
        *count = table->Count;
        return *array != nullptr && *count > 0 && *count < 10000000;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *array = nullptr;
        *count = 0;
        return false;
    }
}

static bool SafeReadObjectArrayEntry(const std::uintptr_t* array, std::uint32_t index, std::uintptr_t* value) {
    if (!array || !value) {
        return false;
    }
    __try {
        *value = array[index];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return false;
    }
}

static bool TryGetObjectName(std::uintptr_t object, char* output, std::size_t outputSize) {
    if (!object || !output || outputSize < 2 || !g_nameArrayAddress) {
        return false;
    }
    output[0] = '\0';
    __try {
        const auto nameIndex = *reinterpret_cast<const std::int32_t*>(object + config::kNameIndexOffset);
        if (nameIndex <= 0 || nameIndex > 10000000) {
            return false;
        }
        const auto nameArray = *reinterpret_cast<const std::uintptr_t*>(g_nameArrayAddress);
        if (!nameArray) {
            return false;
        }
        const auto fName = *reinterpret_cast<const std::uintptr_t*>(nameArray + static_cast<std::uintptr_t>(nameIndex) * sizeof(std::uintptr_t));
        if (!fName) {
            return false;
        }
        const char* source = reinterpret_cast<const char*>(fName + 0x14);
        for (std::size_t i = 0; i + 1 < outputSize; ++i) {
            output[i] = source[i];
            if (output[i] == '\0') {
                return i != 0;
            }
        }
        output[outputSize - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

static bool TryGetObjectClassName(std::uintptr_t object, char* output, std::size_t outputSize) {
    std::uintptr_t objectClass = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(object + config::kClassOffset), &objectClass) || !objectClass) {
        return false;
    }
    return TryGetObjectName(objectClass, output, outputSize);
}

static bool FunctionBelongsToClass(std::uintptr_t function, const char* className) {
    if (!function || !className) {
        return false;
    }
    std::uintptr_t current = function;
    for (int depth = 0; current && depth < 10; ++depth) {
        std::uintptr_t outer = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(current + config::kOuterOffset), &outer) || !outer) {
            return false;
        }
        char name[128]{};
        if (TryGetObjectName(outer, name, sizeof(name))) {
            if (std::strcmp(name, className) == 0) {
                return true;
            }
            if (std::strncmp(name, "Default__", 9) == 0 && std::strcmp(name + 9, className) == 0) {
                return true;
            }
        }
        current = outer;
    }
    return false;
}

static bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
        base == PAGE_EXECUTE_WRITECOPY;
}

static bool IsExecutableAddress(std::uintptr_t address) {
    if (!address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static std::uintptr_t FindPattern(std::uintptr_t base, std::size_t imageSize, const unsigned char* pattern, std::size_t patternSize, const char* mask) {
    if (!base || !imageSize || !pattern || !patternSize || !mask || std::strlen(mask) != patternSize) {
        return 0;
    }
    const std::uintptr_t imageEnd = base + imageSize;
    std::uintptr_t cursor = base;
    while (cursor < imageEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
            break;
        }
        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEndUnclamped = regionBase + mbi.RegionSize;
        const auto regionStart = cursor > regionBase ? cursor : regionBase;
        const auto regionEnd = regionEndUnclamped < imageEnd ? regionEndUnclamped : imageEnd;
        if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect) && regionEnd > regionStart && regionEnd - regionStart >= patternSize) {
            const auto last = regionEnd - patternSize;
            for (std::uintptr_t address = regionStart; address <= last; ++address) {
                bool matches = true;
                for (std::size_t i = 0; i < patternSize; ++i) {
                    if (mask[i] == 'x' && *reinterpret_cast<const unsigned char*>(address + i) != pattern[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return address;
                }
            }
        }
        if (regionEndUnclamped <= cursor) {
            break;
        }
        cursor = regionEndUnclamped;
    }
    return 0;
}

static std::uintptr_t ResolveRipRelative(std::uintptr_t instruction) {
    if (!instruction) {
        return 0;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + 3), sizeof(displacement));
    return instruction + 7 + static_cast<std::intptr_t>(displacement);
}

static bool GetMainModuleInfo(std::uintptr_t* base, std::size_t* size) {
    if (!base || !size) {
        return false;
    }
    __try {
        const HMODULE module = GetModuleHandleW(nullptr);
        if (!module) {
            return false;
        }
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + static_cast<std::uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return false;
        }
        *base = moduleBase;
        *size = nt->OptionalHeader.SizeOfImage;
        return *size != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *base = 0;
        *size = 0;
        return false;
    }
}

static bool ResolveAddresses() {
    if (!GetMainModuleInfo(&g_moduleBase, &g_moduleSize)) {
        return false;
    }
    const auto nameArrayInstruction = FindPattern(g_moduleBase, g_moduleSize, config::kNameArrayPattern, sizeof(config::kNameArrayPattern), config::kNameArrayMask);
    if (!nameArrayInstruction) {
        return false;
    }
    g_nameArrayAddress = ResolveRipRelative(nameArrayInstruction);
    g_objects = reinterpret_cast<GObjects*>(g_moduleBase + config::kGObjectsOffset);
    return g_nameArrayAddress && g_objects;
}

static bool ReadPropertyInfo(std::uintptr_t property, PropertyInfo* info) {
    if (!property || !info) {
        return false;
    }
    char name[128]{};
    char className[128]{};
    if (!TryGetObjectName(property, name, sizeof(name)) || !TryGetObjectClassName(property, className, sizeof(className)) || std::strstr(className, "Property") == nullptr) {
        return false;
    }
    std::uint32_t offset = 0;
    if (!SafeReadUInt32(reinterpret_cast<const void*>(property + config::kUPropertyMemberOffsetField), &offset) || offset >= 0x4000) {
        return false;
    }
    info->object = property;
    info->offset = offset;
    std::strncpy(info->name, name, sizeof(info->name) - 1);
    std::strncpy(info->className, className, sizeof(info->className) - 1);
    return true;
}

static bool ReplaceVtablePointer(void** slot, void* expected, void* replacement) {
    if (!slot || !expected || !replacement) {
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    bool replaced = false;
    if (*slot == expected) {
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), replacement);
        replaced = true;
    }
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
    return replaced;
}

static bool IsPowerOfTwoByte(std::uint32_t value) { return value != 0 && value <= 0x80 && (value & (value - 1)) == 0; }

static bool ResolveBoolMasks(const std::vector<PropertyInfo>& boolProperties) {
    if (boolProperties.size() < 5) {
        return false;
    }
    for (std::uint32_t fieldOffset = 0x98; fieldOffset <= 0xC0; fieldOffset += 4) {
        std::vector<std::uint32_t> values;
        bool valid = true;
        for (const auto& property : boolProperties) {
            std::uint32_t value = 0;
            if (!SafeReadUInt32(reinterpret_cast<const void*>(property.object + fieldOffset), &value) || !IsPowerOfTwoByte(value)) {
                valid = false;
                break;
            }
            values.push_back(value);
        }
        if (!valid) {
            continue;
        }
        std::sort(values.begin(), values.end());
        if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
            continue;
        }
        for (const auto& property : boolProperties) {
            std::uint32_t value = 0;
            SafeReadUInt32(reinterpret_cast<const void*>(property.object + fieldOffset), &value);
            const auto mask = static_cast<std::uint8_t>(value);
            if (std::strcmp(property.name, "mbHide") == 0) {
                g_unitLayout.hideMask = mask;
            }
            else if (std::strcmp(property.name, "mbDisableSelect") == 0) {
                g_unitLayout.disableSelectMask = mask;
            }
            else if (std::strcmp(property.name, "mbDisableChangeMenuName") == 0) {
                g_unitLayout.disableChangeNameMask = mask;
            }
            else if (std::strcmp(property.name, "mbNoSelectSound") == 0) {
                g_unitLayout.noSelectSoundMask = mask;
            }
            else if (std::strcmp(property.name, "mbNewMark") == 0) {
                g_unitLayout.newMarkMask = mask;
            }
        }
        if (g_unitLayout.hideMask && g_unitLayout.disableSelectMask) {
            return true;
        }
    }
    return false;
}

static FStringParam MakeFString(wchar_t* text) {
    FStringParam result{};
    if (!text) {
        return result;
    }
    result.Data = text;
    result.Count = static_cast<std::int32_t>(std::wcslen(text) + 1);
    result.Max = result.Count;
    return result;
}

static void SetUnitName(std::uint8_t* item, std::uint32_t stride, wchar_t* text) {
    if (!item || g_unitLayout.nameOffset + sizeof(FStringParam) > stride) {
        return;
    }
    const auto value = MakeFString(text);
    std::memcpy(item + g_unitLayout.nameOffset, &value, sizeof(value));
}

static void SetUnitFlags(std::uint8_t* item, std::uint32_t stride, std::uint8_t flags) {
    if (!item || g_unitLayout.boolOffset >= stride) {
        return;
    }
    item[g_unitLayout.boolOffset] = flags;
}

static std::uintptr_t FindLiveOwnerForFunction(std::uintptr_t function) {
    if (!function) {
        return 0;
    }
    std::uintptr_t ownerClass = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(function + config::kOuterOffset), &ownerClass) || !ownerClass) {
        return 0;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return 0;
    }
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object) {
            continue;
        }
        std::uintptr_t objectClass = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(object + config::kClassOffset), &objectClass) || objectClass != ownerClass) {
            continue;
        }
        char name[128]{};
        TryGetObjectName(object, name, sizeof(name));
        if (std::strncmp(name, "Default__", 9) == 0) {
            continue;
        }
        return object;
    }
    return 0;
}

static void ResolveOptionMemberLayout() {
    if (!g_optionClass) {
        return;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return;
    }
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t property = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
            continue;
        }
        PropertyInfo info{};
        if (!ReadPropertyInfo(property, &info)) {
            continue;
        }
        std::uintptr_t outer = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || !outer) {
            continue;
        }
        char outerName[128]{};
        TryGetObjectName(outer, outerName, sizeof(outerName));
        if (!g_haveExitGameLayout && outer == g_optionClass && std::strcmp(info.name, "mExitGame") == 0 && std::strcmp(info.className, "BoolProperty") == 0) {
            std::uint32_t mask = 0;
            if (SafeReadUInt32(reinterpret_cast<const void*>(property + 0xB0), &mask) && IsPowerOfTwoByte(mask)) {
                g_exitGameOffset = info.offset;
                g_exitGameMask = static_cast<std::uint8_t>(mask);
                g_haveExitGameLayout = true;
            }
        }
        if (!g_haveMenuStateOffset && std::strcmp(info.name, "mState") == 0 && std::strcmp(info.className, "IntProperty") == 0 &&
            (std::strcmp(outerName, "BrgUIMenu_Base") == 0 || outer == g_optionClass)) {
            g_menuStateOffset = info.offset;
            g_haveMenuStateOffset = true;
        }
        if (g_haveExitGameLayout && g_haveMenuStateOffset) {
            return;
        }
    }
}

static bool ReadOptionState(std::uintptr_t optionMenu, std::int32_t* stateOut) {
    if (!optionMenu || !stateOut || !g_haveMenuStateOffset) {
        return false;
    }
    return SafeReadInt32(reinterpret_cast<const void*>(optionMenu + g_menuStateOffset), stateOut);
}

static bool SetOptionExitGame(std::uintptr_t optionMenu, bool value) {
    if (!optionMenu || !g_haveExitGameLayout || !g_exitGameMask) {
        return false;
    }
    __try {
        auto* byte = reinterpret_cast<std::uint8_t*>(optionMenu + g_exitGameOffset);
        if (value) {
            *byte |= g_exitGameMask;
        }
        else {
            *byte &= static_cast<std::uint8_t>(~g_exitGameMask);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ResolveUnitLayout() {
    if (!g_unitStruct || g_unitLayout.valid) {
        return;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return;
    }
    bool haveIcon = false;
    bool haveName = false;
    bool haveBoolOffset = false;
    std::vector<PropertyInfo> boolProperties;
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t property = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
            continue;
        }
        std::uintptr_t outer = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != g_unitStruct) {
            continue;
        }
        PropertyInfo info{};
        if (!ReadPropertyInfo(property, &info)) {
            continue;
        }
        if (std::strcmp(info.name, "mIcon") == 0) {
            g_unitLayout.iconOffset = info.offset;
            haveIcon = true;
        }
        else if (std::strcmp(info.name, "mName") == 0) {
            g_unitLayout.nameOffset = info.offset;
            haveName = true;
        }
        else if (std::strcmp(info.className, "BoolProperty") == 0) {
            if (!haveBoolOffset) {
                g_unitLayout.boolOffset = info.offset;
                haveBoolOffset = true;
            }
            boolProperties.push_back(info);
        }
    }
    const bool haveMasks = ResolveBoolMasks(boolProperties);
    g_unitLayout.valid = haveIcon && haveName && haveBoolOffset && haveMasks;
    if (g_unitLayout.valid) {
    }
}

static void ResolveCommonTop2Metadata() {
    if (!g_commonTop2Class) {
        return;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return;
    }
    if (!g_haveUnitArrayOffset) {
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            std::uintptr_t property = 0;
            if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
                continue;
            }
            std::uintptr_t outer = 0;
            if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != g_commonTop2Class) {
                continue;
            }
            PropertyInfo info{};
            if (!ReadPropertyInfo(property, &info)) {
                continue;
            }
            if (std::strcmp(info.name, "mUnitArray") == 0 && std::strcmp(info.className, "ArrayProperty") == 0) {
                g_unitArrayOffset = info.offset;
                g_haveUnitArrayOffset = true;
                break;
            }
        }
    }
    if (g_getSelectIndexFunction && !g_haveGetSelectIndexReturnOffset) {
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            std::uintptr_t property = 0;
            if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
                continue;
            }
            std::uintptr_t outer = 0;
            if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != g_getSelectIndexFunction) {
                continue;
            }
            PropertyInfo info{};
            if (!ReadPropertyInfo(property, &info)) {
                continue;
            }
            if (std::strcmp(info.name, "ReturnValue") == 0 && std::strcmp(info.className, "IntProperty") == 0) {
                g_getSelectIndexReturnOffset = info.offset;
                g_haveGetSelectIndexReturnOffset = true;
                break;
            }
        }
    }
    auto resolveBoolReturn = [&](std::uintptr_t function, bool* haveOffset, std::uint32_t* offsetOut) {
        if (!function || !haveOffset || !offsetOut || *haveOffset) {
            return;
        }
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            std::uintptr_t property = 0;
            if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
                continue;
            }
            std::uintptr_t outer = 0;
            if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != function) {
                continue;
            }
            PropertyInfo info{};
            if (!ReadPropertyInfo(property, &info)) {
                continue;
            }
            if (std::strcmp(info.name, "ReturnValue") == 0 && std::strcmp(info.className, "BoolProperty") == 0) {
                *offsetOut = info.offset;
                *haveOffset = true;
                return;
            }
        }
        };
    resolveBoolReturn(g_checkInIdleFunction, &g_haveCheckInIdleReturnOffset, &g_checkInIdleReturnOffset);
    resolveBoolReturn(g_checkCancelFunction, &g_haveCheckCancelReturnOffset, &g_checkCancelReturnOffset);
    resolveBoolReturn(g_optionCheckEndFunction, &g_haveOptionCheckEndReturnOffset, &g_optionCheckEndReturnOffset);
    if (g_optionStartWaitFunction && !g_haveStartWaitLayout) {
        bool haveFloat = false;
        bool haveInt = false;
        bool ambiguous = false;
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            std::uintptr_t property = 0;
            if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
                continue;
            }
            std::uintptr_t outer = 0;
            if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != g_optionStartWaitFunction) {
                continue;
            }
            PropertyInfo info{};
            if (!ReadPropertyInfo(property, &info)) {
                continue;
            }
            if (std::strcmp(info.name, "ReturnValue") == 0) {
                continue;
            }
            if (std::strcmp(info.className, "FloatProperty") == 0) {
                if (haveFloat) {
                    ambiguous = true;
                }
                g_startWaitFloatOffset = info.offset;
                haveFloat = true;
            }
            else if (std::strcmp(info.className, "IntProperty") == 0) {
                if (haveInt) {
                    ambiguous = true;
                }
                g_startWaitStateOffset = info.offset;
                haveInt = true;
            }
        }
        g_haveStartWaitLayout = haveFloat && haveInt && !ambiguous;
    }
    if (g_startCommonTop2Function && g_startCommonTop2Params.empty()) {
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            std::uintptr_t property = 0;
            if (!SafeReadObjectArrayEntry(objectArray, i, &property) || !property) {
                continue;
            }
            std::uintptr_t outer = 0;
            if (!SafeReadPointer(reinterpret_cast<const void*>(property + config::kOuterOffset), &outer) || outer != g_startCommonTop2Function) {
                continue;
            }
            PropertyInfo info{};
            if (!ReadPropertyInfo(property, &info)) {
                continue;
            }
            if (std::strcmp(info.name, "ReturnValue") == 0) {
                continue;
            }
            g_startCommonTop2Params.push_back(info);
        }
        std::sort(g_startCommonTop2Params.begin(), g_startCommonTop2Params.end(), [](const PropertyInfo& a, const PropertyInfo& b) { return a.offset < b.offset; });
    }
}

static void ScanMetadata() {
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return;
    }
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object) {
            continue;
        }
        char name[128]{};
        if (!TryGetObjectName(object, name, sizeof(name))) {
            continue;
        }
        if (!g_optionClass && std::strcmp(name, "BrgUIMenu_Option") == 0) {
            char className[128]{};
            if (TryGetObjectClassName(object, className, sizeof(className)) && std::strcmp(className, "Class") == 0) {
                g_optionClass = object;
            }
        }
        if (!g_commonTop2Class && std::strcmp(name, "BrgUIMenu_CommonTop2") == 0) {
            char className[128]{};
            if (TryGetObjectClassName(object, className, sizeof(className)) && std::strcmp(className, "Class") == 0) {
                g_commonTop2Class = object;
            }
        }
        if (!g_unitStruct && std::strcmp(name, "BrgUIMenu_CommonTop2Unit") == 0) {
            char className[128]{};
            if (TryGetObjectClassName(object, className, sizeof(className)) && std::strstr(className, "Struct")) {
                g_unitStruct = object;
            }
        }
        if (!g_setUnitArrayFunction && std::strcmp(name, "SetUnitArray") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_CommonTop2")) {
            g_setUnitArrayFunction = object;
        }
        if (!g_startCommonTop2Function && std::strcmp(name, "StartCommonTop2") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_CommonTop2")) {
            g_startCommonTop2Function = object;
        }
        if (!g_getSelectIndexFunction && std::strcmp(name, "GetSelectIndex") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_CommonTop2")) {
            g_getSelectIndexFunction = object;
        }
        if (!g_checkInIdleFunction && std::strcmp(name, "CheckInIdle") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_CommonTop2")) {
            g_checkInIdleFunction = object;
        }
        if (!g_checkCancelFunction && std::strcmp(name, "CheckCancel") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_CommonTop2")) {
            g_checkCancelFunction = object;
        }
        if (!g_optionCheckEndFunction && std::strcmp(name, "CheckEnd") == 0 && FunctionBelongsToClass(object, "BrgUIMenu_Option")) {
            g_optionCheckEndFunction = object;
        }
        if (!g_optionStartWaitFunction && std::strcmp(name, "StartWait") == 0 &&
            (FunctionBelongsToClass(object, "BrgUIMenu_Base") || FunctionBelongsToClass(object, "BrgUIMenu_Option"))) {
            g_optionStartWaitFunction = object;
        }
        if (!g_coinLockerFunction && std::strcmp(name, "DebugStartCoinLocker") == 0 && FunctionBelongsToClass(object, "BrgGameInfo")) {
            g_coinLockerFunction = object;
        }
        if (!g_partShopFunction && std::strcmp(name, "DebugStartPartShop") == 0 && FunctionBelongsToClass(object, "BrgGameInfo")) {
            g_partShopFunction = object;
        }
        if (!g_freeCameraFunction && std::strcmp(name, "FreeCamera") == 0) {
            g_freeCameraFunction = object;
        }
    }
    ResolveUnitLayout();
    ResolveCommonTop2Metadata();
    ResolveOptionMemberLayout();
}

static bool ResolveProcessEvent() {
    if (g_processEvent) {
        return true;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return false;
    }
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object) {
            continue;
        }
        char name[128]{};
        if (!TryGetObjectName(object, name, sizeof(name))) {
            continue;
        }
        if (std::strcmp(name, "Default__BrgUIMenu_CommonTop2") != 0) {
            continue;
        }
        std::uintptr_t vtable = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(object), &vtable) || !vtable) {
            continue;
        }
        std::uintptr_t address = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(vtable + config::kProcessEventVtableOffset), &address) || !IsExecutableAddress(address)) {
            continue;
        }
        g_processEvent = reinterpret_cast<ProcessEvent>(address);
        return true;
    }
    return false;
}

static void RefreshCommonTop2Candidates() {
    if (!g_commonTop2Class) {
        return;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return;
    }
    g_commonTop2Candidates.clear();
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object || object == g_commonTop2Class) {
            continue;
        }
        std::uintptr_t objectClass = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(object + config::kClassOffset), &objectClass) || objectClass != g_commonTop2Class) {
            continue;
        }
        char name[128]{};
        TryGetObjectName(object, name, sizeof(name));
        if (std::strncmp(name, "Default__", 9) == 0) {
            continue;
        }
        g_commonTop2Candidates.push_back(object);
    }
}

static std::uint32_t DetectOptionStride(std::uintptr_t data) {
    if (!data || !g_unitLayout.valid) {
        return 0;
    }
    constexpr std::uint8_t expectedIcons[6] = { 21, 23, 13, 14, 25, 26 };
    for (std::uint32_t stride = 0x15; stride <= 0x60; ++stride) {
        bool matches = true;
        for (std::uint32_t i = 0; i < 6; ++i) {
            std::uint8_t icon = 0;
            if (!SafeReadByte(reinterpret_cast<const void*>(data + static_cast<std::uintptr_t>(i) * stride + g_unitLayout.iconOffset), &icon) || icon != expectedIcons[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return stride;
        }
    }
    return 0;
}

static std::uintptr_t FindActiveOptionCommonTop2(TArrayHeader* array, std::uint32_t* stride) {
    if (!array || !stride || !g_haveUnitArrayOffset) {
        return 0;
    }
    auto findCurrent = [&]() -> std::uintptr_t {
        for (const auto object : g_commonTop2Candidates) {
            TArrayHeader current{};
            if (!SafeReadArrayHeader(object + g_unitArrayOffset, &current) || !current.Data || current.Count != 6 || current.Max < current.Count || current.Max > 256) {
                continue;
            }
            const auto detected = DetectOptionStride(current.Data);
            if (!detected) {
                continue;
            }
            *array = current;
            *stride = detected;
            return object;
        }
        return 0;
        };
    if (const auto object = findCurrent()) {
        return object;
    }
    RefreshCommonTop2Candidates();
    return findCurrent();
}

static bool CallSetUnitArray(std::uintptr_t object, void* data, std::int32_t count) {
    if (!object || !data || count <= 0 || !g_processEvent || !g_setUnitArrayFunction) {
        return false;
    }
    alignas(8) unsigned char params[0x40]{};
    auto* array = reinterpret_cast<TArrayHeader*>(params);
    array->Data = reinterpret_cast<std::uintptr_t>(data);
    array->Count = count;
    array->Max = count;
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(object), g_setUnitArrayFunction, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool CallStartCommonTop2(std::uintptr_t object, wchar_t* titleText, std::int32_t firstSelectIndex) {
    if (!object || !g_processEvent || !g_startCommonTop2Function || g_startCommonTop2Params.empty()) {
        return false;
    }
    alignas(8) unsigned char params[0x100]{};
    for (const auto& p : g_startCommonTop2Params) {
        if (p.offset >= sizeof(params)) {
            return false;
        }
        if (std::strcmp(p.name, "inTopMenu") == 0) {
            params[p.offset] = 7;
        }
        else if (std::strcmp(p.name, "inMenuName") == 0) {
            const auto title = MakeFString(titleText);
            if (p.offset + sizeof(title) > sizeof(params)) {
                return false;
            }
            std::memcpy(params + p.offset, &title, sizeof(title));
        }
        else if (std::strcmp(p.name, "inFirstSelectIndex") == 0) {
            if (p.offset + sizeof(firstSelectIndex) > sizeof(params)) {
                return false;
            }
            std::memcpy(params + p.offset, &firstSelectIndex, sizeof(firstSelectIndex));
        }
        else if (std::strcmp(p.name, "inHideMenuImage") == 0 || std::strcmp(p.name, "inSelectEndStart") == 0) {
            params[p.offset] = 0;
        }
    }
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(object), g_startCommonTop2Function, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool GetSelectIndex(std::uintptr_t object, std::int32_t* indexOut) {
    if (!object || !indexOut || !g_processEvent || !g_getSelectIndexFunction || !g_haveGetSelectIndexReturnOffset) {
        return false;
    }
    alignas(8) unsigned char params[0x20]{};
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(object), g_getSelectIndexFunction, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (g_getSelectIndexReturnOffset + sizeof(std::int32_t) > sizeof(params)) {
        return false;
    }
    std::memcpy(indexOut, params + g_getSelectIndexReturnOffset, sizeof(std::int32_t));
    return true;
}

static bool CallNoArgBool(std::uintptr_t object, std::uintptr_t function, std::uint32_t returnOffset, bool* valueOut) {
    if (!object || !function || !valueOut || !g_processEvent || returnOffset >= 0x20) {
        return false;
    }
    alignas(8) unsigned char params[0x20]{};
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(object), function, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *valueOut = params[returnOffset] != 0;
    return true;
}

static bool CheckInIdle(std::uintptr_t object, bool* valueOut) {
    if (!g_haveCheckInIdleReturnOffset) {
        return false;
    }
    return CallNoArgBool(object, g_checkInIdleFunction, g_checkInIdleReturnOffset, valueOut);
}

static bool CheckCancel(std::uintptr_t object, bool* valueOut) {
    if (!g_haveCheckCancelReturnOffset) {
        return false;
    }
    return CallNoArgBool(object, g_checkCancelFunction, g_checkCancelReturnOffset, valueOut);
}

static std::uintptr_t FindLiveInstanceOfClass(std::uintptr_t wantedClass) {
    if (!wantedClass) {
        return 0;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return 0;
    }
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object) {
            continue;
        }
        std::uintptr_t objectClass = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(object + config::kClassOffset), &objectClass) || objectClass != wantedClass) {
            continue;
        }
        char name[128]{};
        TryGetObjectName(object, name, sizeof(name));
        if (std::strncmp(name, "Default__", 9) == 0) {
            continue;
        }
        return object;
    }
    return 0;
}

static std::uintptr_t FindActiveOptionMenuInstance() {
    if (!g_optionClass || !g_haveMenuStateOffset) {
        return 0;
    }
    std::uintptr_t* objectArray = nullptr;
    std::uint32_t objectCount = 0;
    if (!SafeReadObjectTable(g_objects, &objectArray, &objectCount)) {
        return 0;
    }
    std::uintptr_t bestObject = 0;
    int bestScore = -1;
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        std::uintptr_t object = 0;
        if (!SafeReadObjectArrayEntry(objectArray, i, &object) || !object) {
            continue;
        }
        std::uintptr_t objectClass = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(object + config::kClassOffset), &objectClass) || objectClass != g_optionClass) {
            continue;
        }
        char name[128]{};
        TryGetObjectName(object, name, sizeof(name));
        if (std::strncmp(name, "Default__", 9) == 0) {
            continue;
        }
        std::int32_t state = -1;
        if (!ReadOptionState(object, &state)) {
            continue;
        }
        if (state == 0 || state == 13) {
            continue;
        }
        int score = 10;
        if (state == 2) {
            score = 100;
        }
        else if (state == 1) {
            score = 90;
        }
        else if (state == 12) {
            score = 80;
        }
        else if (state >= 3 && state <= 14) {
            score = 70;
        }
        if (score > bestScore) {
            bestScore = score;
            bestObject = object;
        }
    }
    return bestObject;
}

static bool CallNoArgVoidOnGameThread(std::uintptr_t object, std::uintptr_t function) {
    if (!object || !function || !g_processEvent) {
        return false;
    }
    ProcessEvent dispatcher = g_processEvent;
    if (object == g_liveGameInfo && g_originalGameInfoProcessEvent) {
        dispatcher = g_originalGameInfoProcessEvent;
    }
    __try {
        dispatcher(reinterpret_cast<std::uintptr_t*>(object), function, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool CallOptionStartWaitOnGameThread(std::int32_t nextState) {
    if (!g_liveOptionMenu || !g_optionStartWaitFunction || !g_haveStartWaitLayout || !g_processEvent) {
        return false;
    }
    alignas(8) unsigned char params[0x40]{};
    if (g_startWaitFloatOffset + sizeof(float) > sizeof(params) || g_startWaitStateOffset + sizeof(std::int32_t) > sizeof(params)) {
        return false;
    }
    const float waitTime = 0.0f;
    std::memcpy(params + g_startWaitFloatOffset, &waitTime, sizeof(waitTime));
    std::memcpy(params + g_startWaitStateOffset, &nextState, sizeof(nextState));
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(g_liveOptionMenu), g_optionStartWaitFunction, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool OptionMenuHasEndedOnGameThread() {
    if (!g_liveOptionMenu || !g_optionCheckEndFunction || !g_haveOptionCheckEndReturnOffset || !g_processEvent) {
        return false;
    }
    alignas(8) unsigned char params[0x20]{};
    __try {
        g_processEvent(reinterpret_cast<std::uintptr_t*>(g_liveOptionMenu), g_optionCheckEndFunction, reinterpret_cast<char*>(params));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (g_optionCheckEndReturnOffset >= sizeof(params)) {
        return false;
    }
    return params[g_optionCheckEndReturnOffset] != 0;
}

static void DispatchPendingMenuActionOnGameThread() {
    const int action = g_pendingMenuAction;
    if (action == 0) {
        return;
    }
    const int phase = g_pendingMenuPhase;
    const ULONGLONG now = GetTickCount64();
    if (now - g_pendingMenuStartedAt > 5000) {
        g_pendingMenuAction = 0;
        g_pendingMenuPhase = 0;
        g_idleSelectionHandled = false;
        g_liveOptionMenu = 0;
        return;
    }
    if (phase == 1) {
        if (!g_liveOptionMenu) {
            g_liveOptionMenu = FindLiveInstanceOfClass(g_optionClass);
        }
        if (!g_liveOptionMenu) {
            return;
        }
        if (CallOptionStartWaitOnGameThread(12)) {
            g_pendingMenuPhase = 2;
        }
        else {
            g_pendingMenuAction = 0;
            g_pendingMenuPhase = 0;
            g_idleSelectionHandled = false;
        }
        return;
    }
    if (phase == 2) {
        if (!OptionMenuHasEndedOnGameThread()) {
            return;
        }
        const auto function = action == 1 ? g_coinLockerFunction : g_partShopFunction;
        const auto owner = g_liveGameInfo ? g_liveGameInfo : FindLiveOwnerForFunction(function);
        if (owner) {
            CallNoArgVoidOnGameThread(owner, function);
        }
        g_pendingMenuAction = 0;
        g_pendingMenuPhase = 0;
        g_liveOptionMenu = 0;
        g_activeMenuObject = 0;
        g_injectedForTopMenuState = false;
        g_idleSelectionHandled = false;
        g_lastObservedOptionState = -999;
    }
}

static bool InstallGameInfoProcessEventHook();

static void QueueCustomMenuAfterPauseClose(std::uintptr_t optionMenu, int action) {
    if (!optionMenu || (action != 1 && action != 2)) {
        return;
    }
    if (!g_gameInfoProcessEventSlot || !g_originalGameInfoProcessEvent) {
        InstallGameInfoProcessEventHook();
        return;
    }
    g_liveOptionMenu = optionMenu;
    g_pendingMenuStartedAt = GetTickCount64();
    g_pendingMenuAction = action;
    if (CallOptionStartWaitOnGameThread(12)) {
        g_pendingMenuPhase = 2;
    }
    else {
        g_pendingMenuAction = 0;
        g_pendingMenuPhase = 0;
    }
}

static void MonitorOptionMenuOnGameThread();

static void FreeCamera(std::uintptr_t* gameInfo) {
    if (!gameInfo || !g_freeCameraFunction || !g_originalGameInfoProcessEvent) {
        return;
    }
    __try {
        g_originalGameInfoProcessEvent(gameInfo, g_freeCameraFunction, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void __fastcall HookedGameInfoProcessEvent(std::uintptr_t* object, std::uintptr_t function, char* parameters) {
    g_originalGameInfoProcessEvent(object, function, parameters);
    if (!function) {
        return;
    }
    if (!g_gameInfoTickFunction) {
        char functionName[32]{};
        if (!TryGetObjectName(function, functionName, sizeof(functionName)) || std::strcmp(functionName, "Tick") != 0) {
            return;
        }
        g_gameInfoTickFunction = function;
    }
    else if (function != g_gameInfoTickFunction) {
        return;
    }
    MonitorOptionMenuOnGameThread();
    if (g_pendingMenuAction != 0) {
        DispatchPendingMenuActionOnGameThread();
    }
    const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5Down && !g_f5WasDown && g_pendingMenuAction == 0) {
        FreeCamera(object);
    }
    g_f5WasDown = f5Down;
}

static bool InstallGameInfoProcessEventHook() {
    if (!g_coinLockerFunction || !g_processEvent) {
        return false;
    }
    const auto gameInfo = FindLiveOwnerForFunction(g_coinLockerFunction);
    if (!gameInfo) {
        return false;
    }
    g_liveGameInfo = gameInfo;
    std::uintptr_t vtable = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(gameInfo), &vtable) || !vtable) {
        return false;
    }
    auto slot = reinterpret_cast<void**>(vtable + config::kProcessEventVtableOffset);
    if (g_gameInfoProcessEventSlot == slot && *slot == reinterpret_cast<void*>(&HookedGameInfoProcessEvent)) {
        return true;
    }
    std::uintptr_t current = 0;
    if (!SafeReadPointer(slot, &current) || !current) {
        return false;
    }
    if (current == reinterpret_cast<std::uintptr_t>(&HookedGameInfoProcessEvent)) {
        g_gameInfoProcessEventSlot = slot;
        if (!g_originalGameInfoProcessEvent) {
            g_originalGameInfoProcessEvent = g_processEvent;
        }
        return true;
    }
    if (!IsExecutableAddress(current)) {
        return false;
    }
    g_originalGameInfoProcessEvent = reinterpret_cast<ProcessEvent>(current);
    if (!ReplaceVtablePointer(slot, reinterpret_cast<void*>(g_originalGameInfoProcessEvent), reinterpret_cast<void*>(&HookedGameInfoProcessEvent))) {
        return false;
    }
    g_gameInfoProcessEventSlot = slot;
    return true;
}

static bool InjectTwoButtons() {
    if (g_injectedForTopMenuState) {
        return false;
    }
    if (g_pendingMenuAction != 0) {
        return false;
    }
    if (!g_processEvent || !g_setUnitArrayFunction || !g_startCommonTop2Function || !g_unitLayout.valid || !g_haveUnitArrayOffset) {
        return false;
    }
    TArrayHeader original{};
    std::uint32_t stride = 0;
    const auto object = FindActiveOptionCommonTop2(&original, &stride);
    if (!object) {
        return false;
    }
    if (g_unitLayout.nameOffset + sizeof(FStringParam) > stride) {
        return false;
    }
    g_mainUnits.assign(static_cast<std::size_t>(stride) * 8, 0);
    __try {
        std::memcpy(g_mainUnits.data() + static_cast<std::size_t>(stride) * 0, reinterpret_cast<const void*>(original.Data + static_cast<std::uintptr_t>(stride) * 0), stride);
        for (std::uint32_t sourceIndex = 1; sourceIndex < 6; ++sourceIndex) {
            std::memcpy(g_mainUnits.data() + static_cast<std::size_t>(stride) * (sourceIndex + 2),
                reinterpret_cast<const void*>(original.Data + static_cast<std::uintptr_t>(stride) * sourceIndex), stride);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    auto configureItem = [&](std::uint32_t index, std::uint8_t icon, wchar_t* name) {
        auto* item = g_mainUnits.data() + static_cast<std::size_t>(stride) * index;
        std::memset(item, 0, stride);
        item[g_unitLayout.iconOffset] = icon;
        SetUnitName(item, stride, name);
        SetUnitFlags(item, stride, 0);
        };
    configureItem(1, config::kStorageIcon, g_coinLockerText);
    configureItem(2, config::kShopIcon, g_partShopText);
    if (!CallSetUnitArray(object, g_mainUnits.data(), 8)) {
        return false;
    }
    TArrayHeader after{};
    SafeReadArrayHeader(object + g_unitArrayOffset, &after);
    if (after.Count != 8) {
        return false;
    }
    if (!CallStartCommonTop2(object, g_menuTitle, 0)) {
    }
    else {
    }
    g_activeMenuObject = object;
    g_idleSelectionHandled = false;
    return true;
}

static void MonitorOptionMenuOnGameThread() {
    if (!g_optionClass || !g_haveMenuStateOffset || !g_processEvent) {
        return;
    }
    if (!g_liveOptionMenu) {
        g_liveOptionMenu = FindActiveOptionMenuInstance();
        if (!g_liveOptionMenu) {
            return;
        }
    }
    std::int32_t state = -1;
    if (!ReadOptionState(g_liveOptionMenu, &state)) {
        g_liveOptionMenu = 0;
        g_lastObservedOptionState = -999;
        return;
    }
    if ((state == 0 || state == 13) && g_pendingMenuAction == 0) {
        if (state != g_lastObservedOptionState) {
        }
        g_activeMenuObject = 0;
        g_injectedForTopMenuState = false;
        g_idleSelectionHandled = false;
        g_liveOptionMenu = 0;
        g_lastObservedOptionState = -999;
        return;
    }
    if (state != g_lastObservedOptionState) {
        if (state == 1) {
            g_activeMenuObject = 0;
            g_injectedForTopMenuState = false;
            g_idleSelectionHandled = false;
            RefreshCommonTop2Candidates();
        }
        if (state == 2) {
            g_injectedForTopMenuState = false;
            g_idleSelectionHandled = false;
        }
        g_lastObservedOptionState = state;
    }
    if (state == 2 && !g_injectedForTopMenuState) {
        if (InjectTwoButtons()) {
            g_injectedForTopMenuState = true;
        }
        return;
    }
    if (!g_injectedForTopMenuState || !g_activeMenuObject) {
        return;
    }
    std::int32_t selected = -1;
    if (!GetSelectIndex(g_activeMenuObject, &selected)) {
        return;
    }
    if (state == 5 && selected == 1) {
        QueueCustomMenuAfterPauseClose(g_liveOptionMenu, 1);
        return;
    }
    if (state == 6 && selected == 2) {
        QueueCustomMenuAfterPauseClose(g_liveOptionMenu, 2);
        return;
    }
    if (state == 14 && selected == 3) {
        CallOptionStartWaitOnGameThread(5);
        return;
    }
    if (state == 7 && selected == 4) {
        SetOptionExitGame(g_liveOptionMenu, false);
        CallOptionStartWaitOnGameThread(6);
        return;
    }
    if (state == 7 && selected == 5) {
        SetOptionExitGame(g_liveOptionMenu, false);
        CallOptionStartWaitOnGameThread(14);
        return;
    }
    if (state != 2 || (selected != 6 && selected != 7)) {
        return;
    }
    bool inIdle = false;
    if (!CheckInIdle(g_activeMenuObject, &inIdle) || !inIdle) {
        g_idleSelectionHandled = false;
        return;
    }
    if (g_idleSelectionHandled) {
        return;
    }
    bool cancelled = false;
    if (CheckCancel(g_activeMenuObject, &cancelled) && cancelled) {
        g_idleSelectionHandled = true;
        return;
    }
    g_idleSelectionHandled = true;
    const bool exitGame = selected == 7;
    SetOptionExitGame(g_liveOptionMenu, exitGame);
    CallOptionStartWaitOnGameThread(7);
}

static bool RuntimeMetadataReady() {
    return g_commonTop2Class && g_unitStruct && g_setUnitArrayFunction && g_startCommonTop2Function && g_getSelectIndexFunction && g_checkInIdleFunction && g_checkCancelFunction &&
        g_optionClass && g_optionCheckEndFunction && g_optionStartWaitFunction && g_coinLockerFunction && g_partShopFunction && g_freeCameraFunction && g_unitLayout.valid &&
        g_haveUnitArrayOffset && g_haveGetSelectIndexReturnOffset && g_haveCheckInIdleReturnOffset && g_haveCheckCancelReturnOffset && g_haveOptionCheckEndReturnOffset &&
        g_haveStartWaitLayout && g_haveMenuStateOffset && g_processEvent;
}

static DWORD WINAPI MainThread(void*) {
    while (!ResolveAddresses()) {
        Sleep(100);
    }
    while (!RuntimeMetadataReady()) {
        ScanMetadata();
        ResolveProcessEvent();
        Sleep(100);
    }
    RefreshCommonTop2Candidates();
    while (!InstallGameInfoProcessEventHook()) {
        Sleep(100);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (!thread) {
            return FALSE;
        }
        CloseHandle(thread);
    }
    return TRUE;
}