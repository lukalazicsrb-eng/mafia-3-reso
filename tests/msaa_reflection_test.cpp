#include "msaa_reflection.h"

#include <d3dcompiler.h>
#include <dxgi.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;
using mafia::ReflectionMsaaClear;
using mafia::ReflectionMsaaDesc;
using mafia::ReflectionMsaaTarget;

constexpr char probeShaders[] = R"hlsl(
cbuffer DrawData : register(b0) {
    float depth;
    float3 color;
};

float4 drawTriangle(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3] = { float2(-0.86, 0.79), float2(0.77, -0.45), float2(-0.3, -0.82) };
    return float4(positions[vertex], depth, 1.0);
}

float4 solidColor() : SV_Target {
    return float4(color, 1.0);
}
)hlsl";

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex << static_cast<std::uint32_t>(hr) << ")\n";
        throw std::runtime_error(message);
    }
}

ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> shader, errors;
    const HRESULT hr = D3DCompile(probeShaders, sizeof(probeShaders) - 1, "msaa_probe.hlsl",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(hr) && errors) {
        throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    }
    check(hr, "Compiling raster test shader");
    return shader;
}

struct Drawing {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> data;
    ComPtr<ID3D11DepthStencilState> depthStencil;
    ReflectionMsaaDesc expected;
    bool renderViewsMatch = false;
};

Drawing makeDrawing(ID3D11Device* device) {
    Drawing draw{};
    const auto vs = compile("drawTriangle", "vs_5_0");
    const auto ps = compile("solidColor", "ps_5_0");
    check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &draw.vertex),
          "CreateVertexShader");
    check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &draw.pixel),
          "CreatePixelShader");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 4 * sizeof(float);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&desc, nullptr, &draw.data), "CreateBuffer for draw constants");
    D3D11_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencil.StencilEnable = TRUE;
    depthStencil.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    depthStencil.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
        D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
    depthStencil.BackFace = depthStencil.FrontFace;
    check(device->CreateDepthStencilState(&depthStencil, &draw.depthStencil), "CreateDepthStencilState");
    return draw;
}

D3D11_TEXTURE2D_DESC textureDescription(ID3D11View* view) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    if (view) {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
    }
    return desc;
}

void drawReflection(ID3D11DeviceContext* context, void* user) noexcept {
    auto& drawing = *static_cast<Drawing*>(user);
    ComPtr<ID3D11RenderTargetView> color;
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, &color, &depth);
    const auto colorDesc = textureDescription(color.Get());
    const auto depthDesc = textureDescription(depth.Get());
    D3D11_RENDER_TARGET_VIEW_DESC renderView{};
    D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
    if (color) color->GetDesc(&renderView);
    if (depth) depth->GetDesc(&depthView);
    const auto& expected = drawing.expected;
    drawing.renderViewsMatch = colorDesc.SampleDesc.Count == expected.samples
        && depthDesc.SampleDesc.Count == expected.samples
        && colorDesc.SampleDesc.Quality == depthDesc.SampleDesc.Quality
        && colorDesc.Width == expected.width && colorDesc.Height == expected.height
        && depthDesc.Width == expected.width && depthDesc.Height == expected.height
        && colorDesc.Format == expected.colorFormat && depthDesc.Format == expected.depthFormat
        && renderView.ViewDimension == (expected.samples == 1 ? D3D11_RTV_DIMENSION_TEXTURE2D : D3D11_RTV_DIMENSION_TEXTURE2DMS)
        && depthView.ViewDimension == (expected.samples == 1 ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS)
        && (colorDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0;

    ID3D11Buffer* constants = drawing.data.Get();
    context->VSSetConstantBuffers(0, 1, &constants);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->VSSetShader(drawing.vertex.Get(), nullptr, 0);
    context->PSSetShader(drawing.pixel.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetDepthStencilState(drawing.depthStencil.Get(), 5);

    const float nearWhite[] = {0.2f, 1.0f, 1.0f, 1.0f};
    context->UpdateSubresource(constants, 0, nullptr, nearWhite, 0, 0);
    context->Draw(3, 0);
    const float farRed[] = {0.8f, 1.0f, 0.0f, 0.0f};
    context->UpdateSubresource(constants, 0, nullptr, farRed, 0, 0);
    context->Draw(3, 0);
    const float wrongStencilRed[] = {0.1f, 1.0f, 0.0f, 0.0f};
    context->OMSetDepthStencilState(drawing.depthStencil.Get(), 3);
    context->UpdateSubresource(constants, 0, nullptr, wrongStencilRed, 0, 0);
    context->Draw(3, 0);
}

void inspectPixels(ID3D11Device* device, ID3D11DeviceContext* context,
                   ID3D11Texture2D* resolved, UINT samples) {
    D3D11_TEXTURE2D_DESC desc{};
    resolved->GetDesc(&desc);
    expect(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0,
           "The resolved reflection is not single-sample");
    expect((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0, "Reflection output is not sampleable");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    check(device->CreateTexture2D(&desc, nullptr, &readback), "Creating resolve readback");
    context->CopyResource(readback.Get(), resolved);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Reading resolved pixels");
    UINT white = 0, black = 0, mixed = 0, redLeak = 0, badCoverage = 0;
    std::array<UINT, 9> coverageLevels{};
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch * y;
        for (UINT x = 0; x < desc.Width; ++x) {
            const auto* pixel = row + x * 4;
            if (pixel[0] != pixel[1] || pixel[1] != pixel[2]) ++redLeak;
            const UINT value = pixel[0];
            const UINT coveredSamples = (value * samples + 127) / 255;
            const UINT expected = (coveredSamples * 255 + samples / 2) / samples;
            if (value + 1 < expected || value > expected + 1) ++badCoverage;
            ++coverageLevels[coveredSamples];
            if (pixel[0] == 255) ++white;
            else if (pixel[0] == 0) ++black;
            else ++mixed;
        }
    }
    context->Unmap(readback.Get(), 0);
    expect(white != 0 && black != 0, "The coverage probe did not draw inside the target");
    expect(redLeak == 0, "Geometry failed the depth or stencil test and still leaked into the reflection");
    expect((samples == 1) == (mixed == 0), "Resolved pixels do not match the requested sample count");
    expect(badCoverage == 0, "The resolve returned a color outside its sample coverage levels");
    std::cout << samples << " sample(s), " << mixed << " partially covered pixels, depth and stencil respected\n";
    for (UINT covered = 1; covered < samples; ++covered) {
        std::cout << "  " << covered << '/' << samples << " coverage in " << coverageLevels[covered] << " pixels\n";
    }
}

bool supportsSamples(ID3D11Device* device, const ReflectionMsaaDesc& desc) {
    constexpr UINT required = D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET
        | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    UINT support = 0, colorLevels = 0, depthLevels = 0;
    check(device->CheckFormatSupport(desc.colorFormat, &support), "Checking test color support");
    check(device->CheckMultisampleQualityLevels(desc.colorFormat, desc.samples, &colorLevels),
          "Checking test color sample count");
    check(device->CheckMultisampleQualityLevels(desc.depthFormat, desc.samples, &depthLevels),
          "Checking test depth sample count");
    return (support & required) == required && colorLevels && depthLevels;
}

void run() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL minimum = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &minimum, 1,
        D3D11_SDK_VERSION, &device, nullptr, &context), "Creating D3D11 WARP device");
    auto draw = makeDrawing(device.Get());

    ReflectionMsaaTarget target;
    ReflectionMsaaDesc desc{32, 24, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, 1};
    const D3D11_VIEWPORT originalViewport{2, 3, 14, 13, 0, 1};
    context->RSSetViewports(1, &originalViewport);
    ReflectionMsaaClear clear{};
    clear.stencil = 5;
    bool testedMultisampling = false;
    for (const UINT samples : {1u, 2u, 4u, 8u}) {
        desc.samples = samples;
        const HRESULT configured = target.configure(device.Get(), desc);
        if (samples > 1 && !supportsSamples(device.Get(), desc)) {
            expect(configured == DXGI_ERROR_UNSUPPORTED, "An unadvertised MSAA sample count was accepted");
            std::cout << samples << " samples unsupported for this color/depth format pair\n";
            continue;
        }
        check(configured, "Configuring reflection targets");
        expect(target.description().samples == samples, "The selected sample count changed");
        testedMultisampling = testedMultisampling || samples > 1;
        auto* resolved = target.resolvedTexture();
        check(target.configure(device.Get(), desc), "Reconfiguring an unchanged reflection target");
        expect(target.resolvedTexture() == resolved, "An unchanged reflection texture was recreated");

        ReflectionMsaaDesc invalid = desc;
        invalid.samples = 3;
        expect(target.configure(device.Get(), invalid) == E_INVALIDARG && target.resolvedTexture() == resolved,
               "An invalid setting replaced the existing render target");

        draw.expected = desc;
        draw.renderViewsMatch = false;
        auto* shaderView = target.resolvedView();
        D3D11_SHADER_RESOURCE_VIEW_DESC sampleView{};
        shaderView->GetDesc(&sampleView);
        expect(sampleView.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sampleView.Format == desc.colorFormat,
               "The resolved view does not expose the expected sampleable color format");
        context->PSSetShaderResources(7, 1, &shaderView);
        check(target.renderAndResolve(context.Get(), clear, drawReflection, &draw),
              "Rendering and resolving reflection test geometry");
        expect(draw.renderViewsMatch, "Reflection RTV and DSV dimensions, formats or samples differ");

        UINT viewportCount = 1;
        D3D11_VIEWPORT after{};
        context->RSGetViewports(&viewportCount, &after);
        expect(viewportCount == 1 && std::memcmp(&after, &originalViewport, sizeof(after)) == 0,
               "The original viewport was not restored");
        ComPtr<ID3D11ShaderResourceView> restored;
        context->PSGetShaderResources(7, 1, &restored);
        expect(restored.Get() == shaderView, "The caller's shader-resource binding was not restored");
        ID3D11ShaderResourceView* unbound = nullptr;
        context->PSSetShaderResources(7, 1, &unbound);
        inspectPixels(device.Get(), context.Get(), resolved, samples);
    }
    expect(testedMultisampling, "The D3D11 device did not execute any multisampled raster pass");

    desc.samples = 1;
    desc.width *= 2;
    desc.height *= 2;
    check(target.configure(device.Get(), desc), "Recreating the reflection target at a new resolution");
    D3D11_TEXTURE2D_DESC resized{};
    target.resolvedTexture()->GetDesc(&resized);
    expect(resized.Width == desc.width && resized.Height == desc.height, "Reflection resize did not update the output");
    check(device->GetDeviceRemovedReason(), "Final D3D11 device status");
}

}  // namespace

int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
