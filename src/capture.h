#pragma once
#include <d3d9.h>
#include <string>

struct ShaderInfo;

// Single-frame trace of the game's D3D9 usage, written to rct3vr_capture_N\ in the
// game folder: calls.log (every intercepted call), shader disassembly and PNG
// snapshots of each render target at the moment the game switches away from it.
namespace Capture {

bool Active();
void Start(IDirect3DDevice9* dev, int frame);
void Finish(IDirect3DDevice9* dev); // call before the game's Present
void Line(const char* fmt, ...);

std::string SurfaceLabel(IDirect3DSurface9* surface);
std::string TextureLabel(IDirect3DBaseTexture9* texture);
std::string ShaderLabel(void* shader);

void BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface);
void OnDraw(IDirect3DDevice9* dev, const char* call);
void OnClear();

} // namespace Capture
