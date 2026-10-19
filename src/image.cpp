#include "image.h"
#include "hooks.h"

#include <wincodec.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

bool SavePixelsPng(const std::wstring& path, UINT width, UINT height, UINT stride, const void* bgra)
{
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            && SUCCEEDED(factory->CreateStream(&stream))
            && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
            && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
            && SUCCEEDED(encoder->CreateNewFrame(&frame, &props))
            && SUCCEEDED(frame->Initialize(props.Get()))
            && SUCCEEDED(frame->SetSize(width, height))
            && SUCCEEDED(frame->SetPixelFormat(&format))
            && SUCCEEDED(frame->WritePixels(height, stride, stride * height, (BYTE*)bgra))
            && SUCCEEDED(frame->Commit())
            && SUCCEEDED(encoder->Commit());
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return ok;
}

bool SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const std::wstring& path, std::string& error)
{
    D3DSURFACE_DESC desc;
    if (FAILED(surface->GetDesc(&desc))) {
        error = "GetDesc failed";
        return false;
    }
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        error = "unsupported format " + std::to_string(desc.Format);
        return false;
    }

    // GetRenderTargetData needs a single-sampled render target as source.
    ComPtr<IDirect3DSurface9> resolved;
    IDirect3DSurface9* source = surface;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || !(desc.Usage & D3DUSAGE_RENDERTARGET)) {
        if (FAILED(dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
            || FAILED(D3D9Orig().StretchRect(dev, surface, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE))) {
            error = "resolve failed";
            return false;
        }
        source = resolved.Get();
    }

    ComPtr<IDirect3DSurface9> sysmem;
    if (FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr))
        || FAILED(D3D9Orig().GetRenderTargetData(dev, source, sysmem.Get()))) {
        error = "readback failed";
        return false;
    }

    D3DLOCKED_RECT locked;
    if (FAILED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        error = "lock failed";
        return false;
    }
    std::vector<DWORD> pixels(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        const DWORD* row = (const DWORD*)((const BYTE*)locked.pBits + y * locked.Pitch);
        for (UINT x = 0; x < desc.Width; ++x)
            pixels[y * desc.Width + x] = row[x] | 0xFF000000;
    }
    sysmem->UnlockRect();

    if (!SavePixelsPng(path, desc.Width, desc.Height, desc.Width * 4, pixels.data())) {
        error = "png encode failed";
        return false;
    }
    return true;
}
