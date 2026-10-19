#pragma once
#include <d3d9.h>
#include <string>

// Writes 32-bit BGRA pixels to a PNG file.
bool SavePixelsPng(const std::wstring& path, UINT width, UINT height, UINT stride, const void* bgra);

// Reads back a D3D9 surface (resolving multisampling if needed) and saves it as PNG.
// Only 32-bit RGB formats are supported; on failure `error` says why.
bool SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const std::wstring& path, std::string& error);
