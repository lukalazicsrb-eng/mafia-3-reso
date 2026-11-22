#include "native_d3d11_hooks.h"
#include "reflection_observer.h"

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>

namespace {
using Microsoft::WRL::ComPtr;
using mafiareflections::native::DispatchCall;
using mafiareflections::native::DrawCall;
using mafiareflections::native::DrawKind;
using mafiareflections::native::ShaderIdentity;
using mafiareflections::native::getShaderIdentity;

constexpr GUID resourceIdKey{0xac68883a, 0x68a8, 0x48d7, {0x86,0x01,0x3a,0xd0,0xc8,0xd2,0xb8,0x26}};
constexpr UINT srvLimit = 16;
constexpr UINT rtvLimit = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr UINT uavLimit = D3D11_PS_CS_UAV_REGISTER_COUNT;
constexpr unsigned perPassLimit = 6;
constexpr unsigned relatedEventLimit = 12;
constexpr unsigned dependencyCheckLimit = 128;
constexpr unsigned drawCheckLimit = 1000000;
constexpr unsigned unmatchedLimit = 32;
constexpr unsigned unmatchedSamplingPeriod = 256;

struct Pass {
    std::uint32_t crc;
    const char* name;
    unsigned logged = 0;
};

std::array<Pass, 4> candidates{{
    {0x685d5d10, "SSR candidate 1"},
    {0x7e83aa58, "SSR candidate 2"},
    {0xa2c3d0f2, "Tentative SSR candidate"},
    {0x3117c877, "Reflection probe blend candidate"},
}};

std::mutex state;
std::filesystem::path logPath;
std::filesystem::path settingsPath;
std::atomic<bool> observerStarted{false};
std::atomic<bool> observerAvailable{false};
std::atomic<bool> diagnosticsEnabled{true};
std::atomic<std::uint64_t> presentedFrames{0};
std::atomic<unsigned> drawChecks{0};
std::atomic<unsigned> dependencyChecks{0};
std::uint64_t nextResourceId = 0;
std::uint64_t previousCaptureLastResourceId = 0;
unsigned relatedEvents = 0;
unsigned resizeEvents = 0;
std::set<std::array<std::uint8_t, 32>> unmatchedShaders;
std::atomic<std::uint64_t> untaggedShaderDraws{0};
std::atomic<unsigned> reflectionPassMatches{0};
bool dependencyScanStarted = false;

template<typename View, std::size_t Count>
struct Views {
    std::array<View*, Count> values{};
    ~Views() {
        for (auto* view : values) if (view) view->Release();
    }
};

Pass* candidate(std::uint32_t crc) noexcept {
    for (auto& pass : candidates) if (pass.crc == crc) return &pass;
    return nullptr;
}

bool possibleGraphicsDependency(std::uint32_t crc) noexcept {
    switch (crc) {
        case 0xe4a815cf: // depth linearization
        case 0xddcf04ce: // mip copy
        case 0x0390a051: // known downscale candidate
        case 0x5021911b: // known downscale candidate
        case 0x26ab1e66: // deferred scene-light composition candidate
        case 0x6bc66df6: // deferred scene-light composition candidate
            return true;
        default:
            return false;
    }
}

std::uint64_t resourceId(ID3D11Resource* resource, bool assign) noexcept {
    std::uint64_t id = 0;
    UINT size = sizeof(id);
    if (SUCCEEDED(resource->GetPrivateData(resourceIdKey, &size, &id)) && size == sizeof(id)
        && id > previousCaptureLastResourceId)
        return id;
    if (!assign) return 0;
    id = ++nextResourceId;
    return SUCCEEDED(resource->SetPrivateData(resourceIdKey, sizeof(id), &id)) ? id : 0;
}

template<typename View, std::size_t Count>
bool touchesCandidate(const Views<View, Count>& views) noexcept {
    for (auto* view : views.values) {
        if (!view) continue;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        if (resource && resourceId(resource.Get(), false)) return true;
    }
    return false;
}

void writeResource(std::ostream& log, ID3D11Resource* resource, bool assign) {
    const auto id = resourceId(resource, assign);
    if (id) log << " reflectionResource=" << id;
    D3D11_RESOURCE_DIMENSION kind = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&kind);
    log << " resourceType=" << kind;
    switch (kind) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
            ComPtr<ID3D11Buffer> buffer;
            if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return;
            D3D11_BUFFER_DESC info{};
            buffer->GetDesc(&info);
            log << " bytes=" << info.ByteWidth << " stride=" << info.StructureByteStride
                << " bind=" << info.BindFlags << " misc=" << info.MiscFlags;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
            ComPtr<ID3D11Texture1D> texture;
            if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return;
            D3D11_TEXTURE1D_DESC info{};
            texture->GetDesc(&info);
            log << " width=" << info.Width << " mips=" << info.MipLevels
                << " array=" << info.ArraySize << " resourceFormat=" << info.Format
                << " bind=" << info.BindFlags << " misc=" << info.MiscFlags;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return;
            D3D11_TEXTURE2D_DESC info{};
            texture->GetDesc(&info);
            log << " size=" << info.Width << 'x' << info.Height
                << " samples=" << info.SampleDesc.Count << " quality=" << info.SampleDesc.Quality
                << " mips=" << info.MipLevels << " array=" << info.ArraySize
                << " resourceFormat=" << info.Format << " bind=" << info.BindFlags
                << " misc=" << info.MiscFlags;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
            ComPtr<ID3D11Texture3D> texture;
            if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return;
            D3D11_TEXTURE3D_DESC info{};
            texture->GetDesc(&info);
            log << " size=" << info.Width << 'x' << info.Height << 'x' << info.Depth
                << " mips=" << info.MipLevels << " resourceFormat=" << info.Format
                << " bind=" << info.BindFlags << " misc=" << info.MiscFlags;
            break;
        }
        default:
            break;
    }
}

void writeViewRange(std::ostream& log, const D3D11_SHADER_RESOURCE_VIEW_DESC& view) {
    switch (view.ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
            log << " mip=" << view.Texture2D.MostDetailedMip << '+' << view.Texture2D.MipLevels;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            log << " mip=" << view.Texture2DArray.MostDetailedMip << '+' << view.Texture2DArray.MipLevels
                << " slice=" << view.Texture2DArray.FirstArraySlice << '+' << view.Texture2DArray.ArraySize;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            log << " slice=" << view.Texture2DMSArray.FirstArraySlice << '+' << view.Texture2DMSArray.ArraySize;
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            log << " TextureCube mip=" << view.TextureCube.MostDetailedMip << '+' << view.TextureCube.MipLevels;
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            log << " TextureCubeArray mip=" << view.TextureCubeArray.MostDetailedMip
                << '+' << view.TextureCubeArray.MipLevels << " firstFace=" << view.TextureCubeArray.First2DArrayFace
                << " cubes=" << view.TextureCubeArray.NumCubes;
            break;
        default:
            break;
    }
}

void writeViewRange(std::ostream& log, const D3D11_RENDER_TARGET_VIEW_DESC& view) {
    if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
        log << " mip=" << view.Texture2D.MipSlice;
    else if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
        log << " mip=" << view.Texture2DArray.MipSlice
            << " slice=" << view.Texture2DArray.FirstArraySlice << '+' << view.Texture2DArray.ArraySize;
    else if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY)
        log << " slice=" << view.Texture2DMSArray.FirstArraySlice << '+' << view.Texture2DMSArray.ArraySize;
}

void writeViewRange(std::ostream& log, const D3D11_UNORDERED_ACCESS_VIEW_DESC& view) {
    if (view.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D)
        log << " mip=" << view.Texture2D.MipSlice;
    else if (view.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
        log << " mip=" << view.Texture2DArray.MipSlice
            << " slice=" << view.Texture2DArray.FirstArraySlice << '+' << view.Texture2DArray.ArraySize;
    else if (view.ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
        log << " elements=" << view.Buffer.FirstElement << '+' << view.Buffer.NumElements
            << " bufferFlags=" << view.Buffer.Flags;
}

void writeViewRange(std::ostream& log, const D3D11_DEPTH_STENCIL_VIEW_DESC& view) {
    log << " depthViewFlags=" << view.Flags;
    if (view.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
        log << " mip=" << view.Texture2D.MipSlice;
    else if (view.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
        log << " mip=" << view.Texture2DArray.MipSlice
            << " slice=" << view.Texture2DArray.FirstArraySlice << '+' << view.Texture2DArray.ArraySize;
    else if (view.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
        log << " slice=" << view.Texture2DMSArray.FirstArraySlice << '+' << view.Texture2DMSArray.ArraySize;
}

template<typename Descriptor, typename View>
void writeView(std::ostream& log, const char* slot, unsigned index, View* view, bool assign) {
    if (!view) return;
    Descriptor description{};
    view->GetDesc(&description);
    log << "  " << slot << index << " viewType=" << description.ViewDimension
        << " viewFormat=" << description.Format;
    writeViewRange(log, description);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (resource) writeResource(log, resource.Get(), assign);
    log << '\n';
}

template<typename Descriptor, typename View, std::size_t Count>
void writeViews(std::ostream& log, const char* slot, const Views<View, Count>& views, bool assign) {
    for (unsigned i = 0; i < views.values.size(); ++i)
        writeView<Descriptor>(log, slot, i, views.values[i], assign);
}

void writeIdentity(std::ostream& log, const ShaderIdentity& shader, const char* pass) {
    log << "\nPresentEpoch=" << presentedFrames.load(std::memory_order_relaxed)
        << " " << pass << " CRC32=" << std::hex << std::setfill('0') << std::setw(8) << shader.crc32
        << " SHA256=";
    for (auto byte : shader.sha256) log << std::setw(2) << static_cast<unsigned>(byte);
    log << std::dec << std::setfill(' ');
}

bool takeDependencyCheck() noexcept {
    auto remaining = dependencyChecks.load(std::memory_order_relaxed);
    while (remaining) {
        if (dependencyChecks.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

void startDependencyChecks() noexcept {
    if (dependencyScanStarted) return;
    dependencyScanStarted = true;
    dependencyChecks.store(dependencyCheckLimit, std::memory_order_relaxed);
}

void observeDraw(ID3D11DeviceContext* context, const DrawCall& call) noexcept {
    if (!diagnosticsEnabled.load(std::memory_order_relaxed)) return;
    const unsigned checked = drawChecks.fetch_add(1, std::memory_order_relaxed);
    if (checked >= drawCheckLimit) return;
    try {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(&shader, nullptr, nullptr);
        ShaderIdentity identity{};
        if (!getShaderIdentity(shader.Get(), identity)) {
            untaggedShaderDraws.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto* pass = candidate(identity.crc32);
        const bool dependency = !pass && possibleGraphicsDependency(identity.crc32);
        // Candidate IDs are discovery hints from another project, not guaranteed
        // to equal the standard bytecode CRC32 computed by this hook.
        // Capture a small, reproducible sample of unmatched shaders so an
        // incorrect CRC algorithm does not silently yield an empty log.
        const bool sampleUnmatched = !pass && !dependency && checked >= 2048
            && (checked % unmatchedSamplingPeriod == 0);
        if (!pass && !sampleUnmatched && (!dependency || !takeDependencyCheck())) return;

        std::lock_guard<std::mutex> guard(state);
        if (!diagnosticsEnabled.load(std::memory_order_relaxed)) return;
        if (pass ? pass->logged >= perPassLimit
                 : dependency ? relatedEvents >= relatedEventLimit
                              : (!sampleUnmatched || unmatchedShaders.size() >= unmatchedLimit
                                  || !unmatchedShaders.insert(identity.sha256).second)) return;
        Views<ID3D11ShaderResourceView, srvLimit> inputs;
        Views<ID3D11RenderTargetView, rtvLimit> outputs;
        Views<ID3D11UnorderedAccessView, uavLimit> uavs;
        ComPtr<ID3D11DepthStencilView> depth;
        context->PSGetShaderResources(0, srvLimit, inputs.values.data());
        context->OMGetRenderTargets(rtvLimit, outputs.values.data(), &depth);
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavLimit, uavs.values.data());
        if (dependency && !touchesCandidate(inputs)
            && !touchesCandidate(outputs) && !touchesCandidate(uavs)) return;

        std::ofstream log(logPath, std::ios::app);
        if (!log) return;
        if (pass) {
            ++pass->logged;
            reflectionPassMatches.fetch_add(1, std::memory_order_relaxed);
            startDependencyChecks();
        } else if (dependency) {
            ++relatedEvents;
        }
        writeIdentity(log, identity, pass ? pass->name
            : dependency ? "Graphics pass sharing reflection resources"
                         : "Unmatched shader sample (NOT confirmed reflection)");
        log << " PS " << (call.kind == DrawKind::Draw ? "Draw" : "DrawIndexed")
            << " count=" << call.count << " first=" << call.first << " baseVertex=" << call.baseVertex;
        UINT viewportCount = 1;
        D3D11_VIEWPORT viewport{};
        context->RSGetViewports(&viewportCount, &viewport);
        if (viewportCount)
            log << " viewport=" << viewport.Width << 'x' << viewport.Height
                << " offset=" << viewport.TopLeftX << ',' << viewport.TopLeftY;
        log << '\n';
        writeViews<D3D11_SHADER_RESOURCE_VIEW_DESC>(log, "t", inputs, pass != nullptr);
        writeViews<D3D11_RENDER_TARGET_VIEW_DESC>(log, "rtv", outputs, pass != nullptr);
        writeViews<D3D11_UNORDERED_ACCESS_VIEW_DESC>(log, "omUav", uavs, pass != nullptr);
        writeView<D3D11_DEPTH_STENCIL_VIEW_DESC>(log, "dsv", 0, depth.Get(), pass != nullptr);
    } catch (...) {
        // Optional observations must never interrupt rendering.
    }
}

void observeDispatch(ID3D11DeviceContext* context, const DispatchCall& call) noexcept {
    if (!diagnosticsEnabled.load(std::memory_order_relaxed) || !takeDependencyCheck()) return;
    try {
        ComPtr<ID3D11ComputeShader> shader;
        context->CSGetShader(&shader, nullptr, nullptr);
        ShaderIdentity identity{};
        if (!getShaderIdentity(shader.Get(), identity)) return;
        std::lock_guard<std::mutex> guard(state);
        if (!diagnosticsEnabled.load(std::memory_order_relaxed) || relatedEvents >= relatedEventLimit) return;

        Views<ID3D11ShaderResourceView, srvLimit> inputs;
        Views<ID3D11UnorderedAccessView, uavLimit> outputs;
        context->CSGetShaderResources(0, srvLimit, inputs.values.data());
        context->CSGetUnorderedAccessViews(0, uavLimit, outputs.values.data());
        if (!touchesCandidate(inputs) && !touchesCandidate(outputs)) return;
        std::ofstream log(logPath, std::ios::app);
        if (!log) return;
        ++relatedEvents;
        writeIdentity(log, identity, "Compute pass sharing reflection resources");
        log << " CS Dispatch groups=" << call.groupsX << 'x' << call.groupsY << 'x' << call.groupsZ << '\n';
        writeViews<D3D11_SHADER_RESOURCE_VIEW_DESC>(log, "t", inputs, false);
        writeViews<D3D11_UNORDERED_ACCESS_VIEW_DESC>(log, "uav", outputs, false);
    } catch (...) {
        // Optional observations must never interrupt rendering.
    }
}

void observePresent(IDXGISwapChain*, UINT, UINT flags, HRESULT result) noexcept {
    if (result != S_OK || (flags & DXGI_PRESENT_TEST)) return;
    const auto frame = presentedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!diagnosticsEnabled.load(std::memory_order_relaxed)
        || (frame != 1 && frame != 60 && frame != 180)) return;
    try {
        const auto health = mafiareflections::native::hookHealth();
        std::lock_guard<std::mutex> guard(state);
        std::ofstream log(logPath, std::ios::app);
        if (log)
            log << "HOOK_HEALTH frame=" << frame
                << " factories=" << health.deviceFactoriesCalled
                << " gameDevices=" << health.gameDevicesTagged
                << " taggedShaders=" << health.shadersTagged
                << " callbacksDraw=" << health.gameDrawsObserved
                << " callbacksDispatch=" << health.gameDispatchesObserved
                << " callbacksPresent=" << health.gamePresentsObserved
                << " shaderIdentityMisses=" << untaggedShaderDraws.load()
                << " reflectionMatches=" << reflectionPassMatches.load()
                << " unmatchedSamples=" << unmatchedShaders.size() << '\n';
    } catch (...) {
        // Logging is strictly optional and cannot affect the game.
    }
}

void observeResize(IDXGISwapChain*) noexcept {
    if (!diagnosticsEnabled.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard<std::mutex> guard(state);
        if (!diagnosticsEnabled.load(std::memory_order_relaxed) || resizeEvents >= 8) return;
        ++resizeEvents;
        std::ofstream log(logPath, std::ios::app);
        if (log) log << "\nResizeBuffers attempted after PresentEpoch="
                     << presentedFrames.load(std::memory_order_relaxed) << '\n';
    } catch (...) {
        // Observation must not interfere with resize.
    }
}
}  // namespace

bool mafiareflections::installReflectionObserver(const wchar_t* gameFolder) noexcept {
    if (!gameFolder || !*gameFolder) return false;
    try {
        const std::filesystem::path folder(gameFolder);
        if (!folder.is_absolute() || observerStarted.exchange(true, std::memory_order_acq_rel)) return false;
        logPath = folder / L"MafiaReflections.log";
        settingsPath = folder / L"MafiaReflections.ini";
        diagnosticsEnabled.store(GetPrivateProfileIntW(L"Diagnostics", L"Enabled", 1,
            settingsPath.c_str()) == 1, std::memory_order_relaxed);
        {
            std::ofstream log(logPath, std::ios::trunc);
            if (!log) return false;
            log << "Mafia III reflections resource observation\n"
                << "Candidate CRCs guide inspection; full DXBC SHA-256 and runtime bindings are logged.\n"
                << "Renderer settings, resources, draws and dispatches are preserved.\n"
                << "Each candidate logs up to six events, with twelve related events total.\n"
                << "Each event inspects at most sixteen SRVs and eight UAVs. Draw inspection ends after "
                << drawCheckLimit << " calls.\n"
                << "Unknown shader discovery: at most " << unmatchedLimit
                << " unique SHA-256 signatures; these are NOT validated SSR passes.\n"
                << "HOOK_HEALTH distinguishes a patched IAT from real rendering callbacks.\n";
        }
        mafiareflections::native::Observers hooks{};
        hooks.beforeDraw = &observeDraw;
        hooks.beforeDispatch = &observeDispatch;
        hooks.afterPresent = &observePresent;
        hooks.beforeResize = &observeResize;
        const bool installed = mafiareflections::native::installVerifiedHooks(hooks);
        observerAvailable.store(installed, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(state);
            std::ofstream log(logPath, std::ios::app);
            if (log) log << (installed ? "Verified executable D3D11 import connected.\n"
                                      : "No supported executable D3D11 import was connected.\n");
        }
        return installed;
    } catch (...) {
        return false;
    }
}

bool mafiareflections::reflectionDiagnosticsAvailable() noexcept {
    return observerAvailable.load(std::memory_order_acquire);
}

bool mafiareflections::reflectionDiagnosticsEnabled() noexcept {
    return diagnosticsEnabled.load(std::memory_order_relaxed);
}

bool mafiareflections::setReflectionDiagnosticsEnabled(bool enabled) noexcept {
    if (!reflectionDiagnosticsAvailable()) return false;
    try {
        std::lock_guard<std::mutex> guard(state);
        if (reflectionDiagnosticsEnabled() == enabled) return true;
        if (!WritePrivateProfileStringW(L"Diagnostics", L"Enabled", enabled ? L"1" : L"0",
            settingsPath.c_str())) {
            std::ofstream log(logPath, std::ios::app);
            if (log) log << "\nCould not save the F8 choice to MafiaReflections.ini.\n";
            return false;
        }
        if (enabled) {
            for (auto& pass : candidates) pass.logged = 0;
            unmatchedShaders.clear();
            untaggedShaderDraws.store(0, std::memory_order_relaxed);
            reflectionPassMatches.store(0, std::memory_order_relaxed);
            previousCaptureLastResourceId = nextResourceId;
            relatedEvents = 0;
            resizeEvents = 0;
            dependencyScanStarted = false;
            dependencyChecks.store(0, std::memory_order_relaxed);
            drawChecks.store(0, std::memory_order_relaxed);
        }
        diagnosticsEnabled.store(enabled, std::memory_order_relaxed);
        std::ofstream log(logPath, std::ios::app);
        if (log) log << (enabled ? "\nReflection diagnostics enabled from F8.\n"
                                : "\nReflection diagnostics disabled from F8.\n");
        return true;
    } catch (...) {
        return false;
    }
}
