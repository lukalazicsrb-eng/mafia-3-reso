#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace mafia {

struct ReflectionMsaaDesc {
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    UINT samples = 1;
};

struct ReflectionMsaaClear {
    float color[4]{};
    float depth = 1.0f;
    UINT8 stencil = 0;
};

// Isolated, single-mip 2D raster pass. A caller must identify a reflection
// pass that owns its color and depth contents. The output is always a
// single-sample shader resource. The renderer calls this on its immediate
// context thread and binds its draw shaders, resources and any required
// depth/stencil state in the callback.
class ReflectionMsaaTarget final {
public:
    using DrawCallback = void (*)(ID3D11DeviceContext* context, void* user) noexcept;

    ReflectionMsaaTarget() = default;
    ReflectionMsaaTarget(const ReflectionMsaaTarget&) = delete;
    ReflectionMsaaTarget& operator=(const ReflectionMsaaTarget&) = delete;

    HRESULT configure(ID3D11Device* device, const ReflectionMsaaDesc& description) noexcept;
    HRESULT renderAndResolve(ID3D11DeviceContext* context, const ReflectionMsaaClear& clear,
                             DrawCallback draw, void* user) noexcept;

    // Borrowed views remain valid until a successful reconfiguration.
    ID3D11ShaderResourceView* resolvedView() const noexcept { return resources_.sampleView.Get(); }
    ID3D11Texture2D* resolvedTexture() const noexcept { return resources_.sampleColor.Get(); }
    const ReflectionMsaaDesc& description() const noexcept { return description_; }

private:
    struct Resources {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> passState;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> renderColor;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderView;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sampleColor;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sampleView;
    };
    Resources resources_;
    ReflectionMsaaDesc description_;
    bool rendering_ = false;
};

}  // namespace mafia
