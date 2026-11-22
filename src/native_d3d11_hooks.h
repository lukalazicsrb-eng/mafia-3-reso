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
    void (*beforeDispatch)(ID3D11DeviceContext*, const DispatchCall&) noexcept = nullptr;
    void (*afterPresent)(IDXGISwapChain*, UINT syncInterval, UINT flags, HRESULT) noexcept = nullptr;
    void (*beforeResize)(IDXGISwapChain*) noexcept = nullptr;
};

// Call during opt-in loader initialization, outside DllMain and before the
// game's D3D11 device exists. Returns true when an exact factory import is
// connected. Individual observers still require a qualifying device/context.
bool installVerifiedHooks(const Observers& observers) noexcept;
bool getShaderIdentity(ID3D11DeviceChild* shader, ShaderIdentity& identity) noexcept;

}  // namespace mafiareflections::native
