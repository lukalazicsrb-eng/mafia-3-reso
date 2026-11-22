#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>

namespace mafiareflections::native {

struct ShaderIdentity {
    std::uint32_t crc32;
    std::array<std::uint8_t, 32> sha256;
};

enum class DrawKind { Draw, DrawIndexed };

struct DrawCall {
    DrawKind kind;
    UINT count;
    UINT first;
    INT baseVertex;
};

struct DispatchCall {
    UINT groupsX;
    UINT groupsY;
    UINT groupsZ;
};

struct Observers {
    void (*beforeDraw)(ID3D11DeviceContext*, const DrawCall&) noexcept = nullptr;
    // Optional post-pass callback. Recursion guard remains active while it runs.
    void (*afterDraw)(ID3D11DeviceContext*, const DrawCall&) noexcept = nullptr;
    void (*beforeDispatch)(ID3D11DeviceContext*, const DispatchCall&) noexcept = nullptr;
    void (*afterPresent)(IDXGISwapChain*, UINT syncInterval, UINT flags, HRESULT) noexcept = nullptr;
    void (*beforeResize)(IDXGISwapChain*) noexcept = nullptr;
};

// Non-invasive counters. An executable IAT patch does not prove that a
// rendering device or a draw callback was actually observed.
struct HookHealth {
    std::uint64_t deviceFactoriesCalled = 0;
    std::uint64_t gameDevicesTagged = 0;
    std::uint64_t shadersTagged = 0;
    std::uint64_t gameDrawsObserved = 0;
    std::uint64_t gameDispatchesObserved = 0;
    std::uint64_t gamePresentsObserved = 0;
    // Calls observed in the *already verified* Modern Tweaks 2.5.1 DX11 Draw
    // forwarding slot. Independent of the game's D3D11 factory/vtable hooks.
    std::uint64_t modernTweaksDrawCalls = 0;
    bool modernTweaksSlotInstalled = false;
};

HookHealth hookHealth() noexcept;

// Uses original Modern Tweaks 2.5.1's version-checked RIP-indirect Draw
// callsite, rather than patching fragile graphics COM vtables. The original
// user's DLL must be SHA-256 validated by the caller before this is invoked.
// Patches one 8-byte pointer slot atomically; never alters bytes on disk.
// Safe to retry while Modern Tweaks has not yet initialized the Draw slot.
bool installVerifiedModernTweaksDrawBridge(HMODULE verifiedMod) noexcept;

// Call during opt-in loader initialization, outside DllMain and before the
// game's D3D11 device exists. Returns true when an exact factory import is
// connected. Individual observers still require a qualifying device/context.
bool installVerifiedHooks(const Observers& observers) noexcept;
bool getShaderIdentity(ID3D11DeviceChild* shader, ShaderIdentity& identity) noexcept;

}  // namespace mafiareflections::native
