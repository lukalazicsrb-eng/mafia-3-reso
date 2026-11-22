#include "reflection_menu.h"
#include "reflection_observer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace mafiareflections {
namespace {
constexpr std::uintptr_t callRva = 0x5eee3;
constexpr std::uintptr_t comboRva = 0x10e3b0;
constexpr LONG originalDisplacement = 0xaf4c8;
constexpr std::array<unsigned char, 5> originalCall{0xe8, 0xc8, 0xf4, 0x0a, 0x00};
static_assert(sizeof(void*) == 8 && sizeof(LONG) == 4);
static_assert((callRva + 1) % alignof(LONG) == 0);
static_assert(callRva + originalCall.size() + originalDisplacement == comboRva);

using Combo = bool (__cdecl*)(const char*, int*, const char* const*, int, int);
std::atomic<Combo> originalCombo{nullptr};
std::mutex installMutex;

bool __cdecl chooseReflectionDiagnostics(const char* label, int* current,
    const char* const* items, int itemCount, int popupHeight) {
    const auto combo = originalCombo.load(std::memory_order_acquire);
    if (!combo) return false;

    if (current && items && itemCount == 3) {
        constexpr char reflectionLabel[] = "Reflection Diagnostics";
        if (reflectionDiagnosticsAvailable()) {
            constexpr const char* choices[] = {"Off", "On"};
            int enabled = reflectionDiagnosticsEnabled() ? 1 : 0;
            if (combo(reflectionLabel, &enabled, choices, 2, -1) && (enabled == 0 || enabled == 1))
                setReflectionDiagnosticsEnabled(enabled != 0);
        } else {
            constexpr const char* unavailable[] = {"Unavailable"};
            int option = 0;
            combo(reflectionLabel, &option, unavailable, 1, -1);
        }
    }
    // Preserve the original combo as ImGui's last item for its adjacent tooltip.
    return combo(label, current, items, itemCount, popupHeight);
}

bool imageRange(HMODULE module, const void* address, std::size_t bytes, bool executable) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(address, &information, sizeof(information))
        || information.AllocationBase != module || information.State != MEM_COMMIT
        || information.Type != MEM_IMAGE
        || (information.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;

    const DWORD access = information.Protect & 0xff;
    const bool executableRead = access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE
        || access == PAGE_EXECUTE_WRITECOPY;
    const bool dataRead = access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY;
    if (!executableRead && (executable || !dataRead)) return false;

    const auto base = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    if (pointer < base) return false;
    const auto offset = pointer - base;
    return offset <= information.RegionSize && bytes <= information.RegionSize - offset;
}

const IMAGE_NT_HEADERS64* imageHeaders(HMODULE module) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!imageRange(module, dos, sizeof(*dos), false) || dos->e_magic != IMAGE_DOS_SIGNATURE
        || dos->e_lfanew < static_cast<LONG>(sizeof(*dos))) return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<DWORD>(dos->e_lfanew));
    if (!imageRange(module, nt, sizeof(*nt), false) || nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return nt;
}

void* allocateInRange(std::uintptr_t from, std::uintptr_t to, const SYSTEM_INFO& system) noexcept {
    // Visit mapped regions instead of trying every allocation-granularity slot.
    constexpr unsigned regionLimit = 2048;
    for (unsigned inspected = 0; from < to && inspected < regionLimit; ++inspected) {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQuery(reinterpret_cast<const void*>(from), &information, sizeof(information)))
            break;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress)
            + information.RegionSize;
        if (regionEnd <= from) break;

        if (information.State == MEM_FREE) {
            const auto granularity = system.dwAllocationGranularity;
            const auto remainder = from % granularity;
            const auto aligned = remainder ? from + granularity - remainder : from;
            const auto end = (std::min)(to, regionEnd);
            if (aligned < end && system.dwPageSize <= end - aligned) {
                if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(aligned), system.dwPageSize,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return memory;
            }
        }
        from = regionEnd;
    }
    return nullptr;
}

void* createRelay(std::uintptr_t nextInstruction) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto minimum = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    if (nextInstruction < minimum || nextInstruction >= maximum || !system.dwAllocationGranularity
        || !system.dwPageSize) return nullptr;

    constexpr auto reach = static_cast<std::uintptr_t>((std::numeric_limits<LONG>::max)());
    const auto lower = nextInstruction - (std::min)(reach, nextInstruction - minimum);
    const auto upper = nextInstruction + (std::min)(reach, maximum - nextInstruction);
    auto* relay = allocateInRange(nextInstruction, upper, system);
    if (!relay) relay = allocateInRange(lower, nextInstruction, system);
    if (!relay) return nullptr;

    std::array<unsigned char, 12> instructions{0x48, 0xb8}; // mov rax, imm64; jmp rax
    const auto callback = reinterpret_cast<std::uintptr_t>(&chooseReflectionDiagnostics);
    std::memcpy(instructions.data() + 2, &callback, sizeof(callback));
    instructions[10] = 0xff;
    instructions[11] = 0xe0;
    std::memcpy(relay, instructions.data(), instructions.size());

    DWORD formerProtection = 0;
    if (!VirtualProtect(relay, instructions.size(), PAGE_EXECUTE_READ, &formerProtection)
        || !FlushInstructionCache(GetCurrentProcess(), relay, instructions.size())) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return nullptr;
    }
    return relay;
}

bool patchCall(unsigned char* site, Combo combo, void* relay) noexcept {
    const auto next = reinterpret_cast<std::intptr_t>(site + originalCall.size());
    const auto difference = reinterpret_cast<std::intptr_t>(relay) - next;
    if (difference < (std::numeric_limits<LONG>::min)() || difference > (std::numeric_limits<LONG>::max)()) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    const auto displacement = static_cast<LONG>(difference);

    auto* address = reinterpret_cast<volatile LONG*>(site + 1);
    DWORD protection = 0;
    if (!VirtualProtect(site + 1, sizeof(LONG), PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    originalCombo.store(combo, std::memory_order_release);
    const bool swapped = *site == originalCall[0]
        && InterlockedCompareExchange(address, displacement, originalDisplacement) == originalDisplacement;
    const bool flushed = !swapped || FlushInstructionCache(GetCurrentProcess(), site, originalCall.size());
    DWORD ignored = 0;
    const bool restored = VirtualProtect(site + 1, sizeof(LONG), protection, &ignored);

    if (!swapped) {
        originalCombo.store(nullptr, std::memory_order_release);
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    if (flushed && restored) return true;

    // Calls that already reached the relay need its code until process exit.
    if (VirtualProtect(site + 1, sizeof(LONG), PAGE_EXECUTE_READWRITE, &ignored)) {
        InterlockedCompareExchange(address, originalDisplacement, displacement);
        FlushInstructionCache(GetCurrentProcess(), site, originalCall.size());
        VirtualProtect(site + 1, sizeof(LONG), protection, &ignored);
    }
    return false;
}
}  // namespace

bool installReflectionMenu(HMODULE verifiedMod) noexcept {
    if (!verifiedMod) return false;
    try {
        std::lock_guard<std::mutex> lock(installMutex);
        if (originalCombo.load(std::memory_order_acquire)) return false;

        const auto* headers = imageHeaders(verifiedMod);
        if (!headers || headers->OptionalHeader.SizeOfImage <= comboRva) return false;

        auto* base = reinterpret_cast<unsigned char*>(verifiedMod);
        auto* site = base + callRva;
        auto* target = base + comboRva;
        if (!imageRange(verifiedMod, site, originalCall.size(), true)
            || !imageRange(verifiedMod, target, 1, true)
            || std::memcmp(site, originalCall.data(), originalCall.size()) != 0) return false;

        auto* relay = createRelay(reinterpret_cast<std::uintptr_t>(site + originalCall.size()));
        return relay && patchCall(site, reinterpret_cast<Combo>(target), relay);
    } catch (...) {
        return false;
    }
}
}  // namespace mafiareflections
