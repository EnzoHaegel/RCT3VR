// Checks D3D9 (non-Ex) -> D3D11 texture sharing with the d3d9.dll patch used by the mod.
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#include "../src/sharepatch.h"

int main()
{
    // The build folder holds our proxy's d3d9.lib: load the system DLL explicitly.
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\d3d9.dll");
    HMODULE sys = LoadLibraryA(path);
    auto Direct3DCreate9 = (IDirect3D9*(WINAPI*)(UINT))GetProcAddress(sys, "Direct3DCreate9");

    HWND wnd = CreateWindowA("STATIC", "sharetest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    IDirect3DDevice9* dev9 = nullptr;
    HRESULT hr = d3d9->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev9);
    printf("d3d9 CreateDevice: 0x%08lx\n", hr);
    printf("patch: %d site(s)\n", PatchD3d9SharedResourceCheck(sys, true));

    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev11, nullptr, &ctx);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 256;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* shared = nullptr;
    dev11->CreateTexture2D(&desc, nullptr, &shared);
    IDXGIResource* res = nullptr;
    HANDLE handle = nullptr;
    shared->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    res->GetSharedHandle(&handle);

    IDirect3DTexture9* tex9 = nullptr;
    hr = dev9->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex9, &handle);
    printf("d3d9 open shared: 0x%08lx\n", hr);
    if (FAILED(hr))
        return 1;
    IDirect3DSurface9* surf = nullptr;
    tex9->GetSurfaceLevel(0, &surf);
    dev9->ColorFill(surf, nullptr, D3DCOLOR_ARGB(255, 200, 30, 60));
    IDirect3DQuery9* query = nullptr;
    dev9->CreateQuery(D3DQUERYTYPE_EVENT, &query);
    query->Issue(D3DISSUE_END);
    while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
    }

    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.BindFlags = 0;
    sdesc.MiscFlags = 0;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev11->CreateTexture2D(&sdesc, nullptr, &staging);
    ctx->CopyResource(staging, shared);
    D3D11_MAPPED_SUBRESOURCE map;
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    DWORD pixel = *(const DWORD*)map.pData;
    ctx->Unmap(staging, 0);
    printf("d3d11 reads %08lx (expect ffc81e3c)\n", pixel);
    return pixel == 0xffc81e3c ? 0 : 2;
}
