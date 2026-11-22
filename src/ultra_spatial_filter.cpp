#include "ultra_spatial_filter.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace mafiareflections {
namespace {
using Microsoft::WRL::ComPtr;

// Spatial *only*: no temporal accumulation and hence no independent
// subpixel jitter or ghost history. This shader does not change geometry,
// internal reflection resolution, or the original game's TAA.
constexpr const char* shader = R"HLSL(
Texture2D<float4> ReflectionColor : register(t0);

struct VsOut {
    float4 pos : SV_Position;
};
VsOut VS(uint id : SV_VertexID) {
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}

float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
float4 PS(VsOut input) : SV_Target0 {
    uint width, height;
    ReflectionColor.GetDimensions(width, height);
    int2 xy = clamp(int2(input.pos.xy), int2(0, 0), int2(width - 1, height - 1));
    float4 center = ReflectionColor.Load(int3(xy, 0));
    float middleLum = luminance(center.rgb);

    float3 sum = center.rgb;
    float total = 1.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            int2 at = clamp(xy + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            float3 sampleColor = ReflectionColor.Load(int3(at, 0)).rgb;
            float difference = abs(luminance(sampleColor) - middleLum);
            // Bilateral weighting preserves sharp highlight and material edges.
            float radius = max(0.02, 0.25 * max(0.0, middleLum));
            float weight = saturate(1.0 - difference / radius);
            weight = weight * weight;
            if (x != 0 && y != 0) weight *= 0.65;
            sum += sampleColor * weight;
            total += weight;
        }
    }
    float3 filtered = sum / max(total, 1.0);
    // Moderate denoising avoids making vehicle reflections too blurry.
    float3 result = lerp(center.rgb, filtered, 0.60);
    return float4(result, center.a);
}
)HLSL";

bool supported(const D3D11_TEXTURE2D_DESC& d) noexcept {
    return d.Width && d.Height && d.MipLevels == 1 && d.ArraySize == 1 &&
           d.SampleDesc.Count == 1 &&
           d.Usage == D3D11_USAGE_DEFAULT &&
           (d.BindFlags & D3D11_BIND_RENDER_TARGET) &&
           (d.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
            d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
}
bool same(const D3D11_TEXTURE2D_DESC& x, const D3D11_TEXTURE2D_DESC& y) noexcept {
    return x.Width == y.Width && x.Height == y.Height &&
           x.Format == y.Format && x.MipLevels == y.MipLevels &&
           x.ArraySize == y.ArraySize && x.SampleDesc.Count == y.SampleDesc.Count;
}
}  // namespace

bool UltraSpatialFilter::ensure(ID3D11Device* gameDevice, const D3D11_TEXTURE2D_DESC& d) noexcept {
    if (!gameDevice || !supported(d)) return false;
    if (device_.Get() == gameDevice && scratch_ && same(lastDesc_, d) &&
        vertexShader_ && pixelShader_ && isolatedState_ && scratchSrv_ && rasterizer_)
        return true;

    ComPtr<ID3D11Device1> device1;
    if (FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(&device1)))) return false;
    const auto level = gameDevice->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> state;
    if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                __uuidof(ID3D11Device), nullptr, &state)))
        return false;

    ComPtr<ID3DBlob> vertexCode;
    ComPtr<ID3DBlob> pixelCode;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    if (FAILED(D3DCompile(shader, std::strlen(shader), "ultra_spatial_filter",
                          nullptr, nullptr, "VS", "vs_5_0", flags, 0,
                          &vertexCode, nullptr))) return false;
    if (FAILED(D3DCompile(shader, std::strlen(shader), "ultra_spatial_filter",
                          nullptr, nullptr, "PS", "ps_5_0", flags, 0,
                          &pixelCode, nullptr))) return false;

    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    if (FAILED(gameDevice->CreateVertexShader(vertexCode->GetBufferPointer(),
                 vertexCode->GetBufferSize(), nullptr, &vertex))) return false;
    if (FAILED(gameDevice->CreatePixelShader(pixelCode->GetBufferPointer(),
                 pixelCode->GetBufferSize(), nullptr, &pixel))) return false;

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    if (FAILED(gameDevice->CreateRasterizerState(&rasterDesc, &raster))) return false;

    D3D11_TEXTURE2D_DESC copy = d;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> sourceCopy;
    if (FAILED(gameDevice->CreateTexture2D(&copy, nullptr, &sourceCopy))) return false;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    if (FAILED(gameDevice->CreateShaderResourceView(sourceCopy.Get(), nullptr, &sourceView)))
        return false;

    device_ = gameDevice;
    isolatedState_ = std::move(state);
    vertexShader_ = std::move(vertex);
    pixelShader_ = std::move(pixel);
    rasterizer_ = std::move(raster);
    scratch_ = std::move(sourceCopy);
    scratchSrv_ = std::move(sourceView);
    lastDesc_ = d;
    return true;
}

bool UltraSpatialFilter::apply(ID3D11DeviceContext* gameContext,
                               ID3D11RenderTargetView* verifiedOutput) noexcept {
    if (!gameContext || !verifiedOutput || active_) return false;

    D3D11_RENDER_TARGET_VIEW_DESC targetView{};
    verifiedOutput->GetDesc(&targetView);
    if (targetView.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
        targetView.Texture2D.MipSlice != 0) return false;

    ComPtr<ID3D11Resource> targetResource;
    verifiedOutput->GetResource(&targetResource);
    ComPtr<ID3D11Texture2D> target;
    if (!targetResource || FAILED(targetResource.As(&target))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (!supported(desc) || targetView.Format != desc.Format) return false;

    ComPtr<ID3D11Device> gameDevice;
    gameContext->GetDevice(&gameDevice);
    if (!gameDevice || !ensure(gameDevice.Get(), desc)) return false;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(gameContext->QueryInterface(IID_PPV_ARGS(&context1)))) return false;

    // This filter is intentionally render-thread-only and never touches the
    // reflection source before the original pass completed.
    active_ = true;
    gameContext->CopyResource(scratch_.Get(), target.Get());

    ComPtr<ID3DDeviceContextState> previousState;
    context1->SwapDeviceContextState(isolatedState_.Get(), &previousState);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    ID3D11ShaderResourceView* source = scratchSrv_.Get();
    gameContext->OMSetRenderTargets(1, &verifiedOutput, nullptr);
    gameContext->RSSetViewports(1, &viewport);
    gameContext->RSSetState(rasterizer_.Get());
    gameContext->IASetInputLayout(nullptr);
    gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gameContext->VSSetShader(vertexShader_.Get(), nullptr, 0);
    gameContext->PSSetShader(pixelShader_.Get(), nullptr, 0);
    gameContext->PSSetShaderResources(0, 1, &source);
    gameContext->Draw(3, 0);

    // Restore every D3D11 state, including game shaders, input buffers, UAVs,
    // RTV/DSV, blend, and viewport. No stale SRV is left bound to the output.
    context1->SwapDeviceContextState(previousState.Get(), nullptr);
    active_ = false;
    return true;
}
}  // namespace mafiareflections
