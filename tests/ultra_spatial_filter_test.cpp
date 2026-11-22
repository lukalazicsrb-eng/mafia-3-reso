#include "ultra_spatial_filter.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace {
struct Pixel { unsigned char r,g,b,a; };
bool ok(HRESULT hr,const char* what) {
    if(SUCCEEDED(hr))return true;
    std::cerr<<what<<" HRESULT="<<std::hex<<static_cast<unsigned>(hr)<<"\n";
    return false;
}
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    if(!ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
                            nullptr,0,D3D11_SDK_VERSION,
                            &device,&level,&context),"Create WARP"))return 1;

    constexpr UINT width=16,height=16;
    std::array<Pixel,width*height> pixels{};
    for(auto& p:pixels)p={120,120,120,200};
    pixels[7*width+7]={128,128,128,200};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height;
    desc.MipLevels=1; desc.ArraySize=1;
    desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem=pixels.data(); initial.SysMemPitch=width*sizeof(Pixel);
    ComPtr<ID3D11Texture2D> target;
    if(!ok(device->CreateTexture2D(&desc,&initial,&target),"Create reflection target"))return 2;
    ComPtr<ID3D11RenderTargetView> rtv;
    if(!ok(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"Create RTV"))return 3;

    context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
    mafiareflections::UltraSpatialFilter filter;
    if(!filter.apply(context.Get(),rtv.Get()) || !filter.initialized()){
        std::cerr<<"Spatial Ultra filtering did not render on a WARP device\n";
        return 4;
    }
    ComPtr<ID3D11RenderTargetView> afterRtv;
    context->OMGetRenderTargets(1,&afterRtv,nullptr);
    if(afterRtv.Get()!=rtv.Get()){
        std::cerr<<"The filter did not preserve the original RTV binding.\n";
        return 5;
    }

    D3D11_TEXTURE2D_DESC stagingDesc=desc;
    stagingDesc.Usage=D3D11_USAGE_STAGING;
    stagingDesc.BindFlags=0;
    stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if(!ok(device->CreateTexture2D(&stagingDesc,nullptr,&staging),"Create staging"))return 6;
    context->CopyResource(staging.Get(),target.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if(!ok(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Map result"))return 7;
    const auto* row=reinterpret_cast<const Pixel*>(
        static_cast<const unsigned char*>(map.pData)+7*map.RowPitch);
    const Pixel filtered=row[7];
    context->Unmap(staging.Get(),0);
    if(filtered.r>=128 || filtered.r<119 || filtered.a!=200){
        std::cerr<<"Unexpected filter output "<<static_cast<int>(filtered.r)
                 <<" (expected 119..127), alpha "<<static_cast<int>(filtered.a)<<"\n";
        return 8;
    }

    // A texture with multiple mips could lose important game history.
    // Refuse it without changing its content.
    D3D11_TEXTURE2D_DESC nonIsolated=desc;
    nonIsolated.MipLevels=2;
    ComPtr<ID3D11Texture2D> unsafe;
    if(!ok(device->CreateTexture2D(&nonIsolated,nullptr,&unsafe),"Create mipped target"))return 9;
    ComPtr<ID3D11RenderTargetView> unsafeRtv;
    if(!ok(device->CreateRenderTargetView(unsafe.Get(),nullptr,&unsafeRtv),"Create mipped RTV"))return 10;
    if(filter.apply(context.Get(),unsafeRtv.Get())){
        std::cerr<<"Unsafe mipmapped render target was accepted.\n";
        return 11;
    }

    if(filter.apply(nullptr,rtv.Get()) || filter.apply(context.Get(),nullptr)){
        std::cerr<<"Filter accepted null graphics resource.\n";
        return 12;
    }
    std::cout<<"WARP SSR spatial filter changed pixels "<<128<<" -> "
             <<static_cast<unsigned>(filtered.r)
             <<", preserved alpha and RTV state, rejected unsafe targets.\n";
    return 0;
}
