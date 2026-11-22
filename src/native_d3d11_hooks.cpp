#include "native_d3d11_hooks.h"

#include <bcrypt.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

namespace mafiareflections::native {
namespace {
constexpr char gameSha256[] = "1db8d827c1528c3b80861eeed111a3f11a357fc5683f2b5640dbe119f871abaf";
constexpr GUID shaderIdentityKey{0xb0c4b86f, 0xf040, 0x40bd, {0xa8,0x48,0x51,0x7e,0x21,0x8d,0xb8,0x7a}};
constexpr GUID gameObjectKey{0xd50b1cd8, 0x9a5f, 0x4273, {0xb2,0xc8,0x0d,0xe1,0x7a,0x3e,0x72,0xad}};
constexpr UINT gameObjectMarker = 0xb4f18a20u;
static_assert(sizeof(void*) == 8, "Mafia III interception requires the Win64 ABI");

using DeviceFactory = decltype(&D3D11CreateDevice);
using DeviceAndSwapChainFactory = decltype(&D3D11CreateDeviceAndSwapChain);
using CreatePixelShader = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateComputeShader = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11ComputeShader**);
using ContextDraw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using ContextDrawIndexed = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using ContextDispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateSwapChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

constexpr unsigned createPixelShaderIndex = 15;
constexpr unsigned createComputeShaderIndex = 18;
constexpr unsigned drawIndexedIndex = 12;
constexpr unsigned drawIndex = 13;
constexpr unsigned dispatchIndex = 41;
constexpr unsigned presentIndex = 8;
constexpr unsigned resizeBuffersIndex = 13;
constexpr unsigned createSwapChainIndex = 10;

struct ScopedHash {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~ScopedHash() { if (handle) BCryptDestroyHash(handle); }
    bool start(BCRYPT_ALG_HANDLE algorithm) noexcept {
        return BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    bool append(const void* data, ULONG size) const noexcept {
        return BCryptHashData(handle, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), size, 0) >= 0;
    }
    bool finish(std::array<std::uint8_t, 32>& bytes) const noexcept {
        return BCryptFinishHash(handle, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0;
    }
};

BCRYPT_ALG_HANDLE shaAlgorithm = nullptr;
HMODULE systemD3d11 = nullptr;
HMODULE systemDxgi = nullptr;
Observers observers{};
std::atomic<bool> initialized{false};
std::atomic<bool> attempted{false};
// Real callback counters (unlike the IAT installation result).
std::atomic<std::uint64_t> factoryCalls{0};
std::atomic<std::uint64_t> taggedDevices{0};
std::atomic<std::uint64_t> taggedShaders{0};
std::atomic<std::uint64_t> gameDraws{0};
std::atomic<std::uint64_t> gameDispatches{0};
std::atomic<std::uint64_t> gamePresents{0};
std::mutex installation;
thread_local bool observing = false;

struct ObserverScope {
    ObserverScope() noexcept { observing = true; }
    ~ObserverScope() { observing = false; }
};

bool replacePointer(void** slot, void* expected, void* replacement) noexcept {
    DWORD originalProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &originalProtection)) return false;
    auto* prior = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), originalProtection, &ignored);
    return prior == expected;
}

bool belongsTo(void* address, HMODULE expected) noexcept {
    HMODULE owner = nullptr;
    return address && expected && GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner) && owner == expected;
}

HMODULE loadedSystemLibrary(const wchar_t* filename) noexcept {
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(std::size(path)));
    const auto filenameLength = std::wcslen(filename);
    if (!length || length >= std::size(path) || filenameLength + 2 > std::size(path) - length)
        return nullptr;
    path[length] = L'\\';
    std::memcpy(path + length + 1, filename, sizeof(wchar_t) * (filenameLength + 1));
    return GetModuleHandleW(path);
}

template <typename Function>
struct Hook {
    std::atomic<Function> original{nullptr};

    bool attach(void** slot, Function detour, void* expected) noexcept {
        if (!slot || !expected) return false;
        auto* replacement = reinterpret_cast<void*>(detour);
        if (*slot == replacement) return original.load(std::memory_order_acquire) != nullptr;
        if (*slot != expected) return false;
        const auto prior = reinterpret_cast<Function>(expected);
        const auto seen = original.load(std::memory_order_relaxed);
        // A detour has one unambiguous system implementation to forward to.
        if (seen && seen != prior) return false;
        original.store(prior, std::memory_order_release);
        return replacePointer(slot, expected, replacement);
    }

    template <typename Interface>
    bool attach(Interface* object, unsigned index, Function detour, HMODULE module) noexcept {
        auto* slot = *reinterpret_cast<void***>(object) + index;
        auto* candidate = *slot;
        if (candidate != reinterpret_cast<void*>(detour) && !belongsTo(candidate, module)) return false;
        return attach(slot, detour, candidate);
    }
};

Hook<DeviceFactory> deviceFactory;
Hook<DeviceAndSwapChainFactory> deviceAndSwapChainFactory;
Hook<CreatePixelShader> pixelShader;
Hook<CreateComputeShader> computeShader;
Hook<ContextDraw> draw;
Hook<ContextDrawIndexed> drawIndexed;
Hook<ContextDispatch> dispatch;
Hook<Present> present;
Hook<ResizeBuffers> resizeBuffers;
Hook<CreateSwapChain> createSwapChain;

template <typename Object>
bool tagGameObject(Object* object) noexcept {
    return SUCCEEDED(object->SetPrivateData(gameObjectKey, sizeof(gameObjectMarker), &gameObjectMarker));
}

template <typename Object>
bool gameObject(Object* object) noexcept {
    if (!object) return false;
    UINT marker = 0;
    UINT size = sizeof(marker);
    return SUCCEEDED(object->GetPrivateData(gameObjectKey, &size, &marker))
        && size == sizeof(marker) && marker == gameObjectMarker;
}

bool sameProcessWindow(HWND window) noexcept {
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process)
        && process == GetCurrentProcessId();
}

IDXGIAdapter* deviceAdapter(ID3D11Device* device) noexcept {
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return nullptr;
    IDXGIAdapter* adapter = nullptr;
    const HRESULT queried = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    return SUCCEEDED(queried) ? adapter : nullptr;
}

std::uint32_t crc32(const void* data, SIZE_T size) noexcept {
    auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t result = 0xffffffffu;
    while (size--) {
        result ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ (0xedb88320u & (0u - (result & 1u)));
    }
    return result ^ 0xffffffffu;
}

void markShader(ID3D11Device* device, ID3D11DeviceChild* shader,
    const void* bytecode, SIZE_T size) noexcept {
    if (!shader || !bytecode || size < 4 || size > (std::numeric_limits<ULONG>::max)()
        || std::memcmp(bytecode, "DXBC", 4) != 0 || !gameObject(device)) return;
    ShaderIdentity identity{};
    ScopedHash hash;
    if (hash.start(shaAlgorithm) && hash.append(bytecode, static_cast<ULONG>(size))
        && hash.finish(identity.sha256)) {
        identity.crc32 = crc32(bytecode, size);
        if (SUCCEEDED(shader->SetPrivateData(shaderIdentityKey, sizeof(identity), &identity)))
            taggedShaders.fetch_add(1, std::memory_order_relaxed);
    }
}

HRESULT STDMETHODCALLTYPE onCreatePixelShader(ID3D11Device* device, const void* bytecode, SIZE_T size,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** result) {
    const auto forward = pixelShader.original.load(std::memory_order_acquire);
    const HRESULT hr = forward(device, bytecode, size, linkage, result);
    if (SUCCEEDED(hr) && result) markShader(device, *result, bytecode, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE onCreateComputeShader(ID3D11Device* device, const void* bytecode, SIZE_T size,
    ID3D11ClassLinkage* linkage, ID3D11ComputeShader** result) {
    const auto forward = computeShader.original.load(std::memory_order_acquire);
    const HRESULT hr = forward(device, bytecode, size, linkage, result);
    if (SUCCEEDED(hr) && result) markShader(device, *result, bytecode, size);
    return hr;
}

void observeDraw(ID3D11DeviceContext* context, const DrawCall& call) noexcept {
    if (!observing && observers.beforeDraw && gameObject(context)) {
        ObserverScope active;
        gameDraws.fetch_add(1, std::memory_order_relaxed);
        observers.beforeDraw(context, call);
    }
}

void STDMETHODCALLTYPE onDraw(ID3D11DeviceContext* context, UINT count, UINT first) {
    observeDraw(context, {DrawKind::Draw, count, first, 0});
    draw.original.load(std::memory_order_acquire)(context, count, first);
}

void STDMETHODCALLTYPE onDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT first, INT baseVertex) {
    observeDraw(context, {DrawKind::DrawIndexed, count, first, baseVertex});
    drawIndexed.original.load(std::memory_order_acquire)(context, count, first, baseVertex);
}

void STDMETHODCALLTYPE onDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) {
    if (!observing && observers.beforeDispatch && gameObject(context)) {
        ObserverScope active;
        gameDispatches.fetch_add(1, std::memory_order_relaxed);
        observers.beforeDispatch(context, {x, y, z});
    }
    dispatch.original.load(std::memory_order_acquire)(context, x, y, z);
}

HRESULT STDMETHODCALLTYPE onPresent(IDXGISwapChain* chain, UINT syncInterval, UINT flags) {
    const HRESULT hr = present.original.load(std::memory_order_acquire)(chain, syncInterval, flags);
    if (!observing && observers.afterPresent && gameObject(chain)) {
        ObserverScope active;
        gamePresents.fetch_add(1, std::memory_order_relaxed);
        observers.afterPresent(chain, syncInterval, flags, hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE onResizeBuffers(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
    if (!observing && observers.beforeResize && gameObject(chain)) {
        ObserverScope active;
        observers.beforeResize(chain);
    }
    return resizeBuffers.original.load(std::memory_order_acquire)(chain, count, width, height, format, flags);
}

void attachSwapChain(IDXGISwapChain* chain) noexcept {
    DXGI_SWAP_CHAIN_DESC description{};
    if (!chain || FAILED(chain->GetDesc(&description)) || !sameProcessWindow(description.OutputWindow))
        return;
    ID3D11Device* device = nullptr;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    const bool tagged = gameObject(device);
    device->Release();
    if (!tagged) return;

    std::lock_guard<std::mutex> guard(installation);
    if (present.attach(chain, presentIndex, &onPresent, systemDxgi)
        && resizeBuffers.attach(chain, resizeBuffersIndex, &onResizeBuffers, systemDxgi))
        tagGameObject(chain);
}

HRESULT STDMETHODCALLTYPE onCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** result) {
    const HRESULT hr = createSwapChain.original.load(std::memory_order_acquire)(factory, device, desc, result);
    if (SUCCEEDED(hr) && result) attachSwapChain(*result);
    return hr;
}

void attachDevice(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    if (!device) return;
    IDXGIAdapter* adapter = deviceAdapter(device);
    if (!adapter) return;
    IDXGIFactory* factory = nullptr;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    adapter->Release();
    if (context) context->AddRef();
    else device->GetImmediateContext(&context);
    if (context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        std::lock_guard<std::mutex> guard(installation);
        if (pixelShader.attach(device, createPixelShaderIndex, &onCreatePixelShader, systemD3d11)
            && draw.attach(context, drawIndex, &onDraw, systemD3d11)
            && drawIndexed.attach(context, drawIndexedIndex, &onDrawIndexed, systemD3d11)
            && tagGameObject(device)) {
            tagGameObject(context);
            taggedDevices.fetch_add(1, std::memory_order_relaxed);
            computeShader.attach(device, createComputeShaderIndex, &onCreateComputeShader, systemD3d11);
            dispatch.attach(context, dispatchIndex, &onDispatch, systemD3d11);
            if (factory) createSwapChain.attach(factory, createSwapChainIndex, &onCreateSwapChain, systemDxgi);
        }
    }
    if (context) context->Release();
    if (factory) factory->Release();
}

void attachResults(ID3D11Device** device, ID3D11DeviceContext** context, IDXGISwapChain** chain) noexcept {
    ID3D11Device* gameDevice = device ? *device : nullptr;
    if (gameDevice) gameDevice->AddRef();
    else if (context && *context) (*context)->GetDevice(&gameDevice);
    else if (chain && *chain) (*chain)->GetDevice(IID_PPV_ARGS(&gameDevice));
    if (!gameDevice) return;
    attachDevice(gameDevice, context ? *context : nullptr);
    gameDevice->Release();
    if (chain) attachSwapChain(*chain);
}

HRESULT WINAPI onCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE kind, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion, ID3D11Device** device,
    D3D_FEATURE_LEVEL* chosenLevel, ID3D11DeviceContext** context) {
    factoryCalls.fetch_add(1, std::memory_order_relaxed);
    const auto forward = deviceFactory.original.load(std::memory_order_acquire);
    const HRESULT hr = forward(adapter, kind, software, flags, levels, numLevels, sdkVersion,
        device, chosenLevel, context);
    if (SUCCEEDED(hr) && initialized.load(std::memory_order_acquire))
        attachResults(device, context, nullptr);
    return hr;
}

HRESULT WINAPI onCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE kind, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** chain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* chosenLevel, ID3D11DeviceContext** context) {
    factoryCalls.fetch_add(1, std::memory_order_relaxed);
    const auto forward = deviceAndSwapChainFactory.original.load(std::memory_order_acquire);
    const HRESULT hr = forward(adapter, kind, software, flags, levels, numLevels, sdkVersion,
        description, chain, device, chosenLevel, context);
    if (SUCCEEDED(hr) && initialized.load(std::memory_order_acquire))
        attachResults(device, context, chain);
    return hr;
}

bool hashMatchesGame(HMODULE executable) {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(executable, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return false;
    ScopedHash hash;
    if (!hash.start(shaAlgorithm)) return false;
    std::array<char, 65536> block{};
    while (file.read(block.data(), static_cast<std::streamsize>(block.size())) || file.gcount()) {
        if (!hash.append(block.data(), static_cast<ULONG>(file.gcount()))) return false;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!file.eof() || file.bad() || !hash.finish(digest)) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (hex[digest[i] >> 4] != gameSha256[2 * i]
            || hex[digest[i] & 15] != gameSha256[2 * i + 1]) return false;
    }
    return true;
}

template <typename Value>
Value* imageValue(std::uint8_t* base, DWORD size, ULONGLONG offset, std::size_t count = 1) noexcept {
    if (offset >= size || count > (size - offset) / sizeof(Value)) return nullptr;
    return reinterpret_cast<Value*>(base + offset);
}

bool equalImport(std::uint8_t* base, DWORD size, ULONGLONG offset,
    const char* expected, bool insensitive = false) noexcept {
    const auto count = std::strlen(expected) + 1;
    const auto* name = imageValue<char>(base, size, offset, count);
    return name && (insensitive ? _strnicmp(name, expected, count) == 0
                               : std::memcmp(name, expected, count) == 0);
}

bool attachImports(HMODULE executable) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(executable);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(*dos))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return false;
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto count = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const auto* descriptors = imageValue<IMAGE_IMPORT_DESCRIPTOR>(base, size, imports.VirtualAddress, count);
    if (!descriptors || !imports.VirtualAddress) return false;

    bool attached = false;
    for (std::size_t i = 0; i < count && descriptors[i].Name; ++i) {
        const auto& descriptor = descriptors[i];
        if (!equalImport(base, size, descriptor.Name, "d3d11.dll", true)
            || !descriptor.OriginalFirstThunk || !descriptor.FirstThunk) continue;
        for (std::size_t n = 0;; ++n) {
            const auto offset = n * sizeof(IMAGE_THUNK_DATA64);
            const auto* opt = imageValue<IMAGE_THUNK_DATA64>(base, size, descriptor.OriginalFirstThunk + offset);
            auto* address = imageValue<IMAGE_THUNK_DATA64>(base, size, descriptor.FirstThunk + offset);
            if (!opt || !address || !opt->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL64(opt->u1.Ordinal)) continue;
            const auto name = opt->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
            auto** slot = reinterpret_cast<void**>(&address->u1.Function);
            if (equalImport(base, size, name, "D3D11CreateDevice")) {
                auto* expected = reinterpret_cast<void*>(GetProcAddress(systemD3d11, "D3D11CreateDevice"));
                if (deviceFactory.attach(slot, &onCreateDevice, expected)) attached = true;
            } else if (equalImport(base, size, name, "D3D11CreateDeviceAndSwapChain")) {
                auto* expected = reinterpret_cast<void*>(GetProcAddress(systemD3d11, "D3D11CreateDeviceAndSwapChain"));
                if (deviceAndSwapChainFactory.attach(slot, &onCreateDeviceAndSwapChain, expected)) attached = true;
            }
        }
    }
    return attached;
}
}  // namespace

bool installVerifiedHooks(const Observers& callbacks) noexcept {
    if (attempted.exchange(true, std::memory_order_acq_rel)) return false;
    try {
        const HMODULE executable = GetModuleHandleW(nullptr);
        systemD3d11 = loadedSystemLibrary(L"d3d11.dll");
        systemDxgi = loadedSystemLibrary(L"dxgi.dll");
        if (!executable || !systemD3d11
            || BCryptOpenAlgorithmProvider(&shaAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return false;
        if (hashMatchesGame(executable)) {
            observers = callbacks;
            initialized.store(true, std::memory_order_release);
            if (attachImports(executable)) return true;
        }
    } catch (...) {
    }
    initialized.store(false, std::memory_order_release);
    if (shaAlgorithm) BCryptCloseAlgorithmProvider(shaAlgorithm, 0);
    return false;
}

HookHealth hookHealth() noexcept {
    return {
        factoryCalls.load(std::memory_order_relaxed),
        taggedDevices.load(std::memory_order_relaxed),
        taggedShaders.load(std::memory_order_relaxed),
        gameDraws.load(std::memory_order_relaxed),
        gameDispatches.load(std::memory_order_relaxed),
        gamePresents.load(std::memory_order_relaxed)
    };
}

bool getShaderIdentity(ID3D11DeviceChild* shader, ShaderIdentity& identity) noexcept {
    if (!shader) return false;
    UINT size = sizeof(identity);
    return SUCCEEDED(shader->GetPrivateData(shaderIdentityKey, &size, &identity))
        && size == sizeof(identity);
}
}  // namespace mafiareflections::native
