#include "reflection_menu.h"
#include "native_d3d11_hooks.h"
#include "reflection_observer.h"

#include <windows.h>
#include <bcrypt.h>
#include <unknwn.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string_view>

namespace {
using InputFactory = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

constexpr auto originalName = L"ModernTweaks_reflections_original.dll";
constexpr auto expectedHash = "529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900";
HMODULE proxyModule = nullptr;
INIT_ONCE init = INIT_ONCE_STATIC_INIT;
InputFactory originalFactory = nullptr;

struct FileHandle {
    HANDLE value;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool isOriginal(HANDLE file) {
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Algorithm {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~Hash() { if (value) BCryptDestroyHash(value); }
    } hash;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0) < 0) return false;

    std::array<UCHAR, 64 * 1024> chunk{};
    DWORD count = 0;
    do {
        if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr)) return false;
        if (count && BCryptHashData(hash.value, chunk.data(), count, 0) < 0) return false;
    } while (count);
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;

    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (hex[digest[i] >> 4] != expectedHash[2 * i] || hex[digest[i] & 15] != expectedHash[2 * i + 1])
            return false;
    }
    return true;
}

HMODULE loadLocal(const std::filesystem::path& path) {
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

InputFactory loadSystemInput() noexcept {
    wchar_t systemDll[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDll, MAX_PATH);
    constexpr wchar_t suffix[] = L"\\dinput8.dll";
    if (!length || length + std::size(suffix) > std::size(systemDll)) return nullptr;
    for (std::size_t i = 0; i < std::size(suffix); ++i) systemDll[length + i] = suffix[i];
    const auto module = LoadLibraryExW(systemDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<InputFactory>(GetProcAddress(module, "DirectInput8Create")) : nullptr;
}

void report(const std::filesystem::path& folder, std::string_view message) {
    std::ofstream log(folder / L"MafiaReflections-loader.log", std::ios::app);
    if (log) log << message << '\n';
}

struct ModernDrawWorker {
    std::filesystem::path folder;
    HMODULE originalMod=nullptr;
};

DWORD WINAPI delayedRendererHealth(LPVOID parameter) {
    std::unique_ptr<ModernDrawWorker> worker(static_cast<ModernDrawWorker*>(parameter));
    // Original Modern Tweaks initializes its D3D11 Draw forwarding slot
    // after this dinput8 proxy is loaded. Retry safely on a separate thread;
    // never block the game or DirectInput8Create during startup.
    bool bridged=false;
    for(unsigned attempt=0;attempt<120;++attempt) {
        if(mafiareflections::native::installVerifiedModernTweaksDrawBridge(worker->originalMod)) {
            bridged=true;
            break;
        }
        Sleep(250);
    }
    report(worker->folder,bridged
        ? "Verified original Modern Tweaks Draw slot connected atomically."
        : "Modern Tweaks Draw slot not ready/validated; original rendering unchanged.");
    Sleep(12000);
    const auto status=mafiareflections::native::hookHealth();
    std::ostringstream message;
    message << "Renderer hook health: factories=" << status.deviceFactoriesCalled
        << " taggedDevices=" << status.gameDevicesTagged
        << " taggedShaders=" << status.shadersTagged
        << " draws=" << status.gameDrawsObserved
        << " modernTweaksDraws=" << status.modernTweaksDrawCalls
        << " modernTweaksSlotInstalled=" << (status.modernTweaksSlotInstalled?1:0)
        << " dispatches=" << status.gameDispatchesObserved
        << " presents=" << status.gamePresentsObserved;
    report(worker->folder,message.str());
    if(!status.gameDrawsObserved && !status.modernTweaksDrawCalls)
        report(worker->folder,
            "WARNING: no real renderer callbacks reached our observer; Ultra filter remains disabled.");
    return 0;
}

void loadForGame(const std::filesystem::path& folder) {
    const auto originalPath = folder / originalName;
    const FileHandle lockedOriginal{CreateFileW(originalPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!isOriginal(lockedOriginal.value)) {
        report(folder, "Preserved Modern Tweaks DLL missing or SHA-256 mismatch.");
        return;
    }

    // Load the verified original first, then install our optional observer.
    // Its native Draw forward pointer does not exist until the game initializes.
    const HMODULE original=loadLocal(originalPath);
    if(!original) {
        report(folder,"Original Modern Tweaks DLL could not be loaded.");
        return;
    }

    // Renderer and F8 callbacks can outlive calls to DirectInput8Create.
    HMODULE pinned = nullptr;
    const bool pinVerified = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(proxyModule), &pinned) != FALSE;
    const bool observerInstalled = pinVerified && mafiareflections::installReflectionObserver(folder.c_str());
    report(folder, observerInstalled ? "Reflection observer installed (import attached; waiting for real renderer callbacks)."
        : "Reflection observer could not attach to this renderer.");
    if (observerInstalled) {
        auto* copy = new (std::nothrow) ModernDrawWorker{folder,original};
        if (copy) {
            auto handle = CreateThread(nullptr, 0, delayedRendererHealth, copy, 0, nullptr);
            if (handle) CloseHandle(handle);
            else delete copy;
        }
    }

    originalFactory = reinterpret_cast<InputFactory>(GetProcAddress(original, "DirectInput8Create"));
    report(folder, originalFactory ? "Forwarding to verified Modern Tweaks 2.5.1."
                                  : "Modern Tweaks input export unavailable.");
    if (originalFactory && pinVerified) {
        report(folder, mafiareflections::installReflectionMenu(original)
            ? "Reflection Diagnostics added to Modern Tweaks F8 / Visuals."
            : "Reflection Diagnostics F8 hook refused; the Modern Tweaks menu remains available.");
    }
}

BOOL CALLBACK initialize(PINIT_ONCE, PVOID, PVOID*) noexcept {
    try {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(proxyModule, path, static_cast<DWORD>(std::size(path)));
        if (length && length < std::size(path)) loadForGame(std::filesystem::path(path).parent_path());
    } catch (...) {
        // Leave DirectInput available even if observer initialization throws.
    }
    if (!originalFactory) originalFactory = loadSystemInput();
    return TRUE;
}

DWORD WINAPI startInitialization(LPVOID) {
    InitOnceExecuteOnce(&init, initialize, nullptr, nullptr);
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    InitOnceExecuteOnce(&init, initialize, nullptr, nullptr);
    return originalFactory ? originalFactory(instance, version, iid, output, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        proxyModule = module;
        if (const auto thread = CreateThread(nullptr, 0, startInitialization, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
