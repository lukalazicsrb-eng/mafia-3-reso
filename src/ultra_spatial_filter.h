#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace mafiareflections {

// Native DX11 spatial denoiser intended for a single, VERIFIED reflection
// output texture. No ReShade, no history/jitter, no modifications to the
// author's original Modern Tweaks DLL. A shader/producer integration must
// authorize the target before calling this class.
class UltraSpatialFilter final {
public:
    // Works with R11G11B10_FLOAT, R16G16B16A16_FLOAT or RGBA8.
    // Unsupported views/configurations return false without changing pixels.
    bool apply(ID3D11DeviceContext* gameContext,
               ID3D11RenderTargetView* verifiedReflectionTarget) noexcept;
    bool initialized() const noexcept { return vertexShader_ && pixelShader_; }
private:
    bool ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> isolatedState_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scratch_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scratchSrv_;
    D3D11_TEXTURE2D_DESC lastDesc_{};
    bool active_ = false;
};
} // namespace mafiareflections
