#pragma once
#include <d3d9.h>

// Stereo rendering: while VR is on, everything the game draws to its back buffer
// is drawn twice into a side-by-side eye render target, once per eye, with the
// game's projection replaced by the headset eye projection (head rotation and eye
// offset included). The result is read back and submitted to OpenXR at Present.
namespace Stereo {

enum DrawKind {
    kPassthrough, // not drawing to the back buffer, or VR off
    kSkip,        // unsupported, dropped while in VR
    kStereo,      // vertex shader with c_projMat: per-eye projection constant
    kStereoFF,    // fixed function: per-eye D3DTS_PROJECTION
    kScreen,      // pre-transformed (POSITIONT) quad, remapped into each eye
    kMono,        // anything else: same matrices, drawn into each eye viewport
};

struct DrawPlan {
    DrawKind kind = kPassthrough;
    int projReg = -1;
    int projClass = 0;
    float a = 0, b = 0; // z row of the game's projection (keeps its depth range and bias)
};

// True when draws must go through PrepareDraw/SetupEye/FinishDraw.
bool Intercepting();
bool Active();

void OnDeclarationCreated(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl, const D3DVERTEXELEMENT9* elements);
void BindTargets(IDirect3DDevice9* dev, bool force);
HRESULT Clear(IDirect3DDevice9* dev, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

DrawPlan PrepareDraw(IDirect3DDevice9* dev);
void SetupEye(IDirect3DDevice9* dev, const DrawPlan& plan, int eye);
void FinishDraw(IDirect3DDevice9* dev, const DrawPlan& plan);

void BeforePresent(IDirect3DDevice9* dev);
void AfterPresent(IDirect3DDevice9* dev, bool focused);
void OnBeforeReset(IDirect3DDevice9* dev);

} // namespace Stereo
