#include "msaa_reflection.h"

#include <dxgi.h>

#include <utility>

namespace mafia {
namespace {

using Microsoft::WRL::ComPtr;

bool validDescription(const ReflectionMsaaDesc& desc) noexcept {
    const bool validSamples = desc.samples == 1 || desc.samples == 2
        || desc.samples == 4 || desc.samples == 8;
    const bool validDepth = desc.depthFormat == DXGI_FORMAT_D16_UNORM
        || desc.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT
        || desc.depthFormat == DXGI_FORMAT_D32_FLOAT
        || desc.depthFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    return desc.width != 0 && desc.height != 0 && desc.colorFormat != DXGI_FORMAT_UNKNOWN
        && validSamples && validDepth;
}

bool sameDescription(const ReflectionMsaaDesc& a, const ReflectionMsaaDesc& b) noexcept {
    return a.width == b.width && a.height == b.height && a.colorFormat == b.colorFormat
        && a.depthFormat == b.depthFormat && a.samples == b.samples;
}

bool hasFormatSupport(ID3D11Device* device, DXGI_FORMAT format, UINT required) noexcept {
    UINT support = 0;
    return SUCCEEDED(device->CheckFormatSupport(format, &support))
        && (support & required) == required;
}

bool supportsSampleCount(ID3D11Device* device, DXGI_FORMAT format, UINT samples) noexcept {
    UINT levels = 0;
    return SUCCEEDED(device->CheckMultisampleQualityLevels(format, samples, &levels)) && levels != 0;
}

D3D11_TEXTURE2D_DESC textureDesc(const ReflectionMsaaDesc& desc, DXGI_FORMAT format,
                                 UINT samples, UINT bindings) noexcept {
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = desc.width;
    texture.Height = desc.height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = format;
    texture.SampleDesc = {samples, 0};
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = bindings;
    return texture;
}

}  // namespace

HRESULT ReflectionMsaaTarget::configure(ID3D11Device* device, const ReflectionMsaaDesc& requested) noexcept {
    if (!device || !validDescription(requested)) return E_INVALIDARG;
    if (rendering_) return E_UNEXPECTED;
    if (resources_.device.Get() == device && sameDescription(description_, requested)) return S_OK;

    const bool multisampled = requested.samples > 1;
    UINT colorSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET
        | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (multisampled) colorSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET
        | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    constexpr UINT depthSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
    if (!hasFormatSupport(device, requested.colorFormat, colorSupport)
        || !hasFormatSupport(device, requested.depthFormat, depthSupport)
        || (multisampled && (!supportsSampleCount(device, requested.colorFormat, requested.samples)
            || !supportsSampleCount(device, requested.depthFormat, requested.samples)))) {
        return DXGI_ERROR_UNSUPPORTED;
    }

    Resources next;
    next.device = device;
    ComPtr<ID3D11Device1> device1;
    HRESULT hr = next.device.As(&device1);
    if (FAILED(hr)) return hr;
    const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
    hr = device1->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
        __uuidof(ID3D11Device), nullptr, &next.passState);
    if (FAILED(hr)) return hr;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_BACK;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = multisampled;
    hr = device->CreateRasterizerState(&rasterizer, &next.rasterizer);
    if (FAILED(hr)) return hr;

    UINT renderBindings = D3D11_BIND_RENDER_TARGET;
    if (!multisampled) renderBindings |= D3D11_BIND_SHADER_RESOURCE;
    const auto renderDesc = textureDesc(requested, requested.colorFormat, requested.samples, renderBindings);
    hr = device->CreateTexture2D(&renderDesc, nullptr, &next.renderColor);
    if (FAILED(hr)) return hr;
    hr = device->CreateRenderTargetView(next.renderColor.Get(), nullptr, &next.renderView);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11Texture2D> depth;
    const auto depthDesc = textureDesc(requested, requested.depthFormat, requested.samples, D3D11_BIND_DEPTH_STENCIL);
    hr = device->CreateTexture2D(&depthDesc, nullptr, &depth);
    if (FAILED(hr)) return hr;
    hr = device->CreateDepthStencilView(depth.Get(), nullptr, &next.depthView);
    if (FAILED(hr)) return hr;

    if (multisampled) {
        const auto sampleDesc = textureDesc(requested, requested.colorFormat, 1, D3D11_BIND_SHADER_RESOURCE);
        hr = device->CreateTexture2D(&sampleDesc, nullptr, &next.sampleColor);
        if (FAILED(hr)) return hr;
    } else {
        next.sampleColor = next.renderColor;
    }
    hr = device->CreateShaderResourceView(next.sampleColor.Get(), nullptr, &next.sampleView);
    if (FAILED(hr)) return hr;

    resources_ = std::move(next);
    description_ = requested;
    return S_OK;
}

HRESULT ReflectionMsaaTarget::renderAndResolve(ID3D11DeviceContext* context,
    const ReflectionMsaaClear& clear, DrawCallback draw, void* user) noexcept {
    if (!context || !draw || !(clear.depth >= 0.0f && clear.depth <= 1.0f)
        || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return E_INVALIDARG;
    }
    if (rendering_ || !resources_.device) return E_UNEXPECTED;
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != resources_.device.Get()) return E_INVALIDARG;
    ComPtr<ID3D11DeviceContext1> context1;
    const HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&context1));
    if (FAILED(hr)) return hr;

    rendering_ = true;
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(resources_.passState.Get(), &previous);
    auto* renderView = resources_.renderView.Get();
    auto* depthView = resources_.depthView.Get();
    context->OMSetRenderTargets(1, &renderView, depthView);
    context->ClearRenderTargetView(renderView, clear.color);
    UINT depthClearFlags = D3D11_CLEAR_DEPTH;
    if (description_.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT
        || description_.depthFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        depthClearFlags |= D3D11_CLEAR_STENCIL;
    }
    context->ClearDepthStencilView(depthView, depthClearFlags, clear.depth, clear.stencil);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(description_.width),
        static_cast<float>(description_.height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(resources_.rasterizer.Get());

    draw(context, user);

    context->ClearState();
    if (description_.samples > 1) {
        context->ResolveSubresource(resources_.sampleColor.Get(), 0, resources_.renderColor.Get(),
                                    0, description_.colorFormat);
    }
    context1->SwapDeviceContextState(previous.Get(), nullptr);
    rendering_ = false;
    return resources_.device->GetDeviceRemovedReason();
}

}  // namespace mafia
