#pragma once
#include <d3d9.h>
#include <string>
#include <vector>

#include "ctab.h"

// Original (unhooked) IDirect3DDevice9 methods. Our own code must call these
// instead of going through the device vtable, which points at our hooks.
struct D3D9Methods {
    HRESULT(STDMETHODCALLTYPE* Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    HRESULT(STDMETHODCALLTYPE* Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    HRESULT(STDMETHODCALLTYPE* GetRenderTargetData)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
    HRESULT(STDMETHODCALLTYPE* StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
    HRESULT(STDMETHODCALLTYPE* ColorFill)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
    HRESULT(STDMETHODCALLTYPE* SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    HRESULT(STDMETHODCALLTYPE* GetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
    HRESULT(STDMETHODCALLTYPE* SetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*);
    HRESULT(STDMETHODCALLTYPE* GetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9**);
    HRESULT(STDMETHODCALLTYPE* GetViewport)(IDirect3DDevice9*, D3DVIEWPORT9*);
    HRESULT(STDMETHODCALLTYPE* BeginScene)(IDirect3DDevice9*);
    HRESULT(STDMETHODCALLTYPE* EndScene)(IDirect3DDevice9*);
    HRESULT(STDMETHODCALLTYPE* Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    HRESULT(STDMETHODCALLTYPE* SetTransform)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    HRESULT(STDMETHODCALLTYPE* MultiplyTransform)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    HRESULT(STDMETHODCALLTYPE* SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    HRESULT(STDMETHODCALLTYPE* SetRenderState)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    HRESULT(STDMETHODCALLTYPE* CreateStateBlock)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
    HRESULT(STDMETHODCALLTYPE* BeginStateBlock)(IDirect3DDevice9*);
    HRESULT(STDMETHODCALLTYPE* EndStateBlock)(IDirect3DDevice9*, IDirect3DStateBlock9**);
    HRESULT(STDMETHODCALLTYPE* SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    HRESULT(STDMETHODCALLTYPE* SetScissorRect)(IDirect3DDevice9*, const RECT*);
    HRESULT(STDMETHODCALLTYPE* DrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    HRESULT(STDMETHODCALLTYPE* ProcessVertices)(IDirect3DDevice9*, UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD);
    HRESULT(STDMETHODCALLTYPE* CreateVertexDeclaration)(IDirect3DDevice9*, const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**);
    HRESULT(STDMETHODCALLTYPE* SetVertexDeclaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
    HRESULT(STDMETHODCALLTYPE* SetFVF)(IDirect3DDevice9*, DWORD);
    HRESULT(STDMETHODCALLTYPE* CreateVertexShader)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
    HRESULT(STDMETHODCALLTYPE* SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    HRESULT(STDMETHODCALLTYPE* SetVertexShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
    HRESULT(STDMETHODCALLTYPE* CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
    HRESULT(STDMETHODCALLTYPE* SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    HRESULT(STDMETHODCALLTYPE* SetPixelShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
    HRESULT(STDMETHODCALLTYPE* CreateAdditionalSwapChain)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
    // IDirect3DSwapChain9 / IDirect3DStateBlock9
    HRESULT(STDMETHODCALLTYPE* SwapChainPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
    HRESULT(STDMETHODCALLTYPE* StateBlockApply)(IDirect3DStateBlock9*);
};
const D3D9Methods& D3D9Orig();

struct ShaderInfo {
    int id = 0;
    bool vertex = false;
    std::vector<DWORD> code;
    std::vector<ShaderConstant> constants;
    bool hasCtab = false;
    int projReg = -1;   // c_projMat
    int projClass = 0;  // 2 = rows, 3 = columns
    int worldViewReg = -1;
};
ShaderInfo* FindShader(void* shader);

// Device state as last set by the game.
struct ShadowState {
    IDirect3DSurface9* renderTargets[4] = {};
    IDirect3DSurface9* depthStencil = nullptr;
    D3DVIEWPORT9 viewport = {};
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    DWORD fvf = 0;
    float vsConst[256][4] = {};
    D3DMATRIX world = {}, view = {}, proj = {};
    IDirect3DSurface9* backBuffer = nullptr;   // identity only, not referenced
    IDirect3DSurface9* autoDepth = nullptr;    // identity only, not referenced
};
ShadowState& Shadow();

void Hooks_OnDirect3DCreated(IDirect3D9* d3d);

// The system d3d9.dll this proxy forwards to (main.cpp).
HMODULE RealD3d9();
