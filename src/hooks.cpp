#include "hooks.h"
#include "capture.h"
#include "config.h"
#include "log.h"
#include "probe.h"
#include "stereo.h"

#include <intrin.h>
#include <mutex>
#include <unordered_map>

namespace {

D3D9Methods g_orig;
ShadowState g_shadow;
std::mutex g_shaderMutex;
std::unordered_map<void*, ShaderInfo> g_shaders;
int g_nextShaderId = 1;
UINT g_maxVsConst = 256;

HRESULT(STDMETHODCALLTYPE* g_origCreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

int g_frame = 0;
int g_frameDraws = 0;
bool g_captureKeyDown = false;
bool g_probeKeyDown = false;
bool g_autoCaptureDone = false;
DWORD g_statsStart = 0;
int g_statsFrames = 0;
int g_statsDraws = 0;

// IDirect3DDevice9 vtable slots.
enum DeviceSlot {
    kReset = 16, kPresent = 17, kGetSwapChain = 14, kCreateAdditionalSwapChain = 13,
    kGetRenderTargetData = 32, kStretchRect = 34, kColorFill = 35,
    kSetRenderTarget = 37, kGetRenderTarget = 38, kSetDepthStencilSurface = 39, kGetDepthStencilSurface = 40,
    kBeginScene = 41, kEndScene = 42, kClear = 43,
    kSetTransform = 44, kMultiplyTransform = 46, kSetViewport = 47, kGetViewport = 48, kSetRenderState = 57,
    kCreateStateBlock = 59, kBeginStateBlock = 60, kEndStateBlock = 61, kSetTexture = 65, kSetScissorRect = 75,
    kDrawPrimitive = 81, kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84,
    kProcessVertices = 85, kCreateVertexDeclaration = 86, kSetVertexDeclaration = 87, kSetFVF = 89,
    kCreateVertexShader = 91, kSetVertexShader = 92, kSetVertexShaderConstantF = 94,
    kCreatePixelShader = 106, kSetPixelShader = 107, kSetPixelShaderConstantF = 109,
};
const int kD3D9CreateDevice = 16;
const int kSwapChainPresent = 3;
const int kStateBlockApply = 5;

template <typename T>
void HookSlot(void* object, int index, T hook, T& original)
{
    void** vtbl = *(void***)object;
    if (vtbl[index] == (void*)hook)
        return; // vtables are shared per class: already hooked
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    original = (T)vtbl[index];
    vtbl[index] = (void*)hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

const char* kTransformNames[] = {"?", "?", "VIEW", "PROJECTION"};

const char* TransformName(D3DTRANSFORMSTATETYPE t)
{
    if (t < 4)
        return kTransformNames[t];
    if (t == D3DTS_WORLD)
        return "WORLD";
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7)
        return "TEXTURE";
    return "WORLDn";
}

void LogPresentParams(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    Log("%s: %ux%u fmt=%d count=%u ms=%d/%lu swap=%d windowed=%d autoDepth=%d(%d) flags=0x%lx refresh=%u interval=0x%x",
        what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType,
        pp->MultiSampleQuality, pp->SwapEffect, pp->Windowed, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags,
        pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

HWND g_window = nullptr;

void ApplyWindowedOverride(D3DPRESENT_PARAMETERS* pp)
{
    if (!Cfg().forceWindowed || !pp || pp->Windowed)
        return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
}

void ApplyBorderlessWindow(const D3DPRESENT_PARAMETERS* pp)
{
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : g_window;
    if (!Cfg().forceWindowed || !wnd)
        return;
    SetWindowLongW(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongW(wnd, GWL_EXSTYLE, GetWindowLongW(wnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
    SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, pp->BackBufferWidth, pp->BackBufferHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void RefreshDeviceSurfaces(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* s = nullptr;
    g_shadow.backBuffer = nullptr;
    g_shadow.autoDepth = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s))) {
        g_shadow.backBuffer = s;
        s->Release();
    }
    if (SUCCEEDED(dev->GetDepthStencilSurface(&s))) {
        g_shadow.autoDepth = s;
        s->Release();
    }
    g_shadow.renderTargets[0] = g_shadow.backBuffer;
    for (int i = 1; i < 4; ++i)
        g_shadow.renderTargets[i] = nullptr;
    g_shadow.depthStencil = g_shadow.autoDepth;
    dev->GetViewport(&g_shadow.viewport);
}

// Re-reads state that a state block may have changed behind our back.
void RefreshShadowFromDevice(IDirect3DDevice9* dev)
{
    IDirect3DVertexShader9* vs = nullptr;
    if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs)
        vs->Release();
    g_shadow.vertexShader = vs;
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps)
        ps->Release();
    g_shadow.pixelShader = ps;
    dev->GetVertexShaderConstantF(0, &g_shadow.vsConst[0][0], g_maxVsConst);
    dev->GetTransform(D3DTS_WORLD, &g_shadow.world);
    dev->GetTransform(D3DTS_VIEW, &g_shadow.view);
    dev->GetTransform(D3DTS_PROJECTION, &g_shadow.proj);
    dev->GetViewport(&g_shadow.viewport);
    dev->GetFVF(&g_shadow.fvf);
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl)
        decl->Release();
    g_shadow.declaration = decl;
}

void RegisterShader(void* shader, const DWORD* code, bool vertex)
{
    ShaderInfo info;
    info.vertex = vertex;
    info.code.assign(code, code + ShaderCodeLength(code));
    info.hasCtab = ParseConstantTable(info.code.data(), info.code.size(), info.constants);
    for (const ShaderConstant& c : info.constants) {
        if (c.registerSet != 2)
            continue;
        if (c.name == "c_projMat") {
            info.projReg = c.registerIndex;
            info.projClass = c.typeClass;
        } else if (c.name == "c_worldViewMat") {
            info.worldViewReg = c.registerIndex;
        }
    }
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    info.id = g_nextShaderId++;
    if (vertex)
        Log("VS%d created: version %08lx, %u dwords, ctab=%d, c_projMat=c%d (class %d), c_worldViewMat=c%d", info.id, info.code[0],
            (unsigned)info.code.size(), (int)info.hasCtab, info.projReg, info.projClass, info.worldViewReg);
    g_shaders[shader] = std::move(info);
}

bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void PollHotkeys(IDirect3DDevice9* dev, bool focused)
{
    bool down = focused && Cfg().captureKey && (GetAsyncKeyState(Cfg().captureKey) & 0x8000);
    if (down && !g_captureKeyDown && !Capture::Active())
        Capture::Start(dev, g_frame);
    g_captureKeyDown = down;

    static DWORD firstFrameTick = GetTickCount();
    bool autoByTime = Cfg().autoCaptureSeconds > 0 && !g_autoCaptureDone
        && GetTickCount() - firstFrameTick >= (DWORD)Cfg().autoCaptureSeconds * 1000;
    bool autoByFrame = Cfg().autoCaptureFrame > 0 && g_frame == Cfg().autoCaptureFrame;
    if ((autoByTime || autoByFrame) && !Capture::Active()) {
        Capture::Start(dev, g_frame);
        g_autoCaptureDone = true;
    }
}

void BeforePresent(IDirect3DDevice9* dev)
{
    Stereo::BeforePresent(dev);
    if (Capture::Active()) {
        Capture::Finish(dev);
        if (g_autoCaptureDone && Cfg().exitAfterAutoCapture) {
            Log("auto capture done, exiting");
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

void AfterPresent(IDirect3DDevice9* dev)
{
    ++g_frame;
    ++g_statsFrames;
    g_statsDraws += g_frameDraws;
    g_frameDraws = 0;
    DWORD now = GetTickCount();
    if (!g_statsStart)
        g_statsStart = now;
    if (now - g_statsStart >= 10000) {
        Log("stats: frame %d, %.1f fps, %d draws/frame", g_frame, g_statsFrames * 1000.0 / (now - g_statsStart),
            g_statsFrames ? g_statsDraws / g_statsFrames : 0);
        g_statsStart = now;
        g_statsFrames = g_statsDraws = 0;
    }
    bool focused = GameFocused();
    PollHotkeys(dev, focused);
    Probe::OnFrameEnd();
    bool probeDown = focused && Cfg().probeKey && (GetAsyncKeyState(Cfg().probeKey) & 0x8000);
    static DWORD firstTick = GetTickCount();
    static bool autoProbeDone = false;
    bool autoProbe = !autoProbeDone && Cfg().autoProbeSeconds > 0 && GetTickCount() - firstTick >= (DWORD)Cfg().autoProbeSeconds * 1000;
    if ((probeDown && !g_probeKeyDown) || autoProbe) {
        autoProbeDone = true;
        Probe::Run();
    }
    g_probeKeyDown = probeDown;
    Stereo::AfterPresent(dev, focused);
}

// Runs a draw call once, or once per eye while VR replaces the back buffer.
template <typename F>
HRESULT StereoDraw(IDirect3DDevice9* dev, F draw)
{
    Probe::OnDraw();
    if (!Stereo::Intercepting())
        return draw();
    Stereo::DrawPlan plan = Stereo::PrepareDraw(dev);
    if (plan.kind == Stereo::kSkip)
        return D3D_OK;
    HRESULT hr = D3D_OK;
    for (int eye = 0; eye < 2; ++eye) {
        Stereo::SetupEye(dev, plan, eye);
        hr = draw();
    }
    Stereo::FinishDraw(dev, plan);
    return hr;
}

void SetFullViewport(IDirect3DSurface9* surface)
{
    D3DSURFACE_DESC d;
    if (surface && SUCCEEDED(surface->GetDesc(&d)))
        g_shadow.viewport = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
}

// ---------------------------------------------------------------------------
// Hooks

HRESULT STDMETHODCALLTYPE Hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LogPresentParams("Reset", pp);
    Stereo::OnBeforeReset(dev);
    ApplyWindowedOverride(pp);
    HRESULT hr = g_orig.Reset(dev, pp);
    Log("Reset -> 0x%08lx", hr);
    if (SUCCEEDED(hr)) {
        ApplyBorderlessWindow(pp);
        RefreshDeviceSurfaces(dev);
    }
    return hr;
}

// Device::Present may route through the swap chain's Present: only the outer call counts.
bool g_inPresent = false;

HRESULT STDMETHODCALLTYPE Hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    if (g_inPresent)
        return g_orig.Present(dev, src, dst, wnd, dirty);
    g_inPresent = true;
    BeforePresent(dev);
    HRESULT hr = g_orig.Present(dev, src, dst, wnd, dirty);
    AfterPresent(dev);
    g_inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SwapChainPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags)
{
    IDirect3DDevice9* dev = nullptr;
    if (g_inPresent || FAILED(sc->GetDevice(&dev)) || !dev)
        return g_orig.SwapChainPresent(sc, src, dst, wnd, dirty, flags);
    dev->Release();
    g_inPresent = true;
    BeforePresent(dev);
    HRESULT hr = g_orig.SwapChainPresent(sc, src, dst, wnd, dirty, flags);
    AfterPresent(dev);
    g_inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateAdditionalSwapChain(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out)
{
    LogPresentParams("CreateAdditionalSwapChain", pp);
    return g_orig.CreateAdditionalSwapChain(dev, pp, out);
}

HRESULT STDMETHODCALLTYPE Hk_GetRenderTargetData(IDirect3DDevice9* dev, IDirect3DSurface9* rt, IDirect3DSurface9* dst)
{
    if (Capture::Active())
        Capture::Line("GetRenderTargetData %s -> %s", Capture::SurfaceLabel(rt).c_str(), Capture::SurfaceLabel(dst).c_str());
    return g_orig.GetRenderTargetData(dev, rt, dst);
}

HRESULT STDMETHODCALLTYPE Hk_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter)
{
    if (Capture::Active()) {
        Capture::Line("StretchRect %s -> %s filter=%d srcRect=%s dstRect=%s", Capture::SurfaceLabel(src).c_str(),
            Capture::SurfaceLabel(dst).c_str(), filter, srcRect ? "yes" : "full", dstRect ? "yes" : "full");
        Capture::OnClear();
    }
    return g_orig.StretchRect(dev, src, srcRect, dst, dstRect, filter);
}

HRESULT STDMETHODCALLTYPE Hk_ColorFill(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color)
{
    if (Capture::Active())
        Capture::Line("ColorFill %s %08lx", Capture::SurfaceLabel(surface).c_str(), color);
    return g_orig.ColorFill(dev, surface, rect, color);
}

HRESULT STDMETHODCALLTYPE Hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface)
{
    if (Capture::Active())
        Capture::BeforeSetRenderTarget(dev, index, surface);
    if (index == 0 && Stereo::Active()) {
        if (!surface)
            return D3DERR_INVALIDCALL;
        g_shadow.renderTargets[0] = surface;
        SetFullViewport(surface); // the runtime resets the viewport on SetRenderTarget
        Stereo::BindTargets(dev, true);
        return D3D_OK;
    }
    HRESULT hr = g_orig.SetRenderTarget(dev, index, surface);
    if (SUCCEEDED(hr) && index < 4) {
        g_shadow.renderTargets[index] = surface;
        if (index == 0)
            SetFullViewport(surface);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_GetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9** out)
{
    // While VR is on the device holds our eye target: report the game's own.
    if (index == 0 && out && Stereo::Active()) {
        *out = g_shadow.renderTargets[0];
        if (*out)
            (*out)->AddRef();
        return *out ? D3D_OK : D3DERR_NOTFOUND;
    }
    return g_orig.GetRenderTarget(dev, index, out);
}

HRESULT STDMETHODCALLTYPE Hk_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* surface)
{
    if (Capture::Active())
        Capture::Line("SetDepthStencilSurface %s", Capture::SurfaceLabel(surface).c_str());
    g_shadow.depthStencil = surface;
    if (Stereo::Active()) {
        Stereo::BindTargets(dev, false);
        return D3D_OK;
    }
    return g_orig.SetDepthStencilSurface(dev, surface);
}

HRESULT STDMETHODCALLTYPE Hk_GetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9** out)
{
    if (out && Stereo::Active()) {
        *out = g_shadow.depthStencil;
        if (*out)
            (*out)->AddRef();
        return *out ? D3D_OK : D3DERR_NOTFOUND;
    }
    return g_orig.GetDepthStencilSurface(dev, out);
}

HRESULT STDMETHODCALLTYPE Hk_GetViewport(IDirect3DDevice9* dev, D3DVIEWPORT9* vp)
{
    if (vp && Stereo::Active()) {
        *vp = g_shadow.viewport;
        return D3D_OK;
    }
    return g_orig.GetViewport(dev, vp);
}

HRESULT STDMETHODCALLTYPE Hk_BeginScene(IDirect3DDevice9* dev)
{
    if (Capture::Active())
        Capture::Line("BeginScene");
    return g_orig.BeginScene(dev);
}

HRESULT STDMETHODCALLTYPE Hk_EndScene(IDirect3DDevice9* dev)
{
    if (Capture::Active())
        Capture::Line("EndScene");
    return g_orig.EndScene(dev);
}

HRESULT STDMETHODCALLTYPE Hk_Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (Capture::Active()) {
        Capture::Line("Clear flags=%lu rects=%lu color=%08lx z=%.3f stencil=%lu rt=%s vp=%lu,%lu,%lux%lu", flags, count, color, z,
            stencil, Capture::SurfaceLabel(g_shadow.renderTargets[0]).c_str(), g_shadow.viewport.X, g_shadow.viewport.Y,
            g_shadow.viewport.Width, g_shadow.viewport.Height);
        Capture::OnClear();
    }
    if (Stereo::Intercepting())
        return Stereo::Clear(dev, flags, color, z, stencil);
    return g_orig.Clear(dev, count, rects, flags, color, z, stencil);
}

void LogMatrix(const char* what, const D3DMATRIX* m)
{
    Capture::Line("%s [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", what,
        m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
}

HRESULT STDMETHODCALLTYPE Hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m)
{
    if (m) {
        if (type == D3DTS_WORLD)
            g_shadow.world = *m;
        else if (type == D3DTS_VIEW)
            g_shadow.view = *m;
        else if (type == D3DTS_PROJECTION)
            g_shadow.proj = *m;
        if (Capture::Active()) {
            char what[64];
            snprintf(what, sizeof(what), "SetTransform %s(%d)", TransformName(type), type);
            LogMatrix(what, m);
        }
    }
    return g_orig.SetTransform(dev, type, m);
}

HRESULT STDMETHODCALLTYPE Hk_MultiplyTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m)
{
    if (Capture::Active())
        Capture::Line("MultiplyTransform %d (unhandled)", type);
    HRESULT hr = g_orig.MultiplyTransform(dev, type, m);
    if (type == D3DTS_WORLD)
        dev->GetTransform(type, &g_shadow.world);
    else if (type == D3DTS_VIEW)
        dev->GetTransform(type, &g_shadow.view);
    else if (type == D3DTS_PROJECTION)
        dev->GetTransform(type, &g_shadow.proj);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
    if (vp) {
        g_shadow.viewport = *vp;
        if (Capture::Active())
            Capture::Line("SetViewport %lu,%lu %lux%lu z=%.2f-%.2f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
        if (Stereo::Intercepting())
            return D3D_OK; // eye viewports are set per draw
    }
    return g_orig.SetViewport(dev, vp);
}

HRESULT STDMETHODCALLTYPE Hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if (Capture::Active())
        Capture::Line("RS %d=0x%lx", state, value);
    return g_orig.SetRenderState(dev, state, value);
}

HRESULT STDMETHODCALLTYPE Hk_StateBlockApply(IDirect3DStateBlock9* sb)
{
    HRESULT hr = g_orig.StateBlockApply(sb);
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(sb->GetDevice(&dev)) && dev) {
        RefreshShadowFromDevice(dev);
        dev->Release();
    }
    if (Capture::Active())
        Capture::Line("StateBlock %p Apply", (void*)sb);
    return hr;
}

void HookStateBlock(IDirect3DStateBlock9* sb)
{
    if (sb)
        HookSlot(sb, kStateBlockApply, &Hk_StateBlockApply, g_orig.StateBlockApply);
}

HRESULT STDMETHODCALLTYPE Hk_CreateStateBlock(IDirect3DDevice9* dev, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out)
{
    HRESULT hr = g_orig.CreateStateBlock(dev, type, out);
    if (SUCCEEDED(hr))
        HookStateBlock(*out);
    Log("CreateStateBlock type=%d -> %p", type, out ? (void*)*out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_BeginStateBlock(IDirect3DDevice9* dev)
{
    if (Capture::Active())
        Capture::Line("BeginStateBlock");
    return g_orig.BeginStateBlock(dev);
}

HRESULT STDMETHODCALLTYPE Hk_EndStateBlock(IDirect3DDevice9* dev, IDirect3DStateBlock9** out)
{
    HRESULT hr = g_orig.EndStateBlock(dev, out);
    if (SUCCEEDED(hr))
        HookStateBlock(*out);
    if (Capture::Active())
        Capture::Line("EndStateBlock -> %p", out ? (void*)*out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
    if (Capture::Active())
        Capture::Line("SetTexture %lu %s", stage, Capture::TextureLabel(tex).c_str());
    return g_orig.SetTexture(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE Hk_SetScissorRect(IDirect3DDevice9* dev, const RECT* r)
{
    if (Capture::Active() && r)
        Capture::Line("SetScissorRect %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
    return g_orig.SetScissorRect(dev, r);
}

HRESULT STDMETHODCALLTYPE Hk_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    ++g_frameDraws;
    if (Capture::Active()) {
        char call[96];
        snprintf(call, sizeof(call), "DrawPrimitive t=%d start=%u prims=%u", type, start, count);
        Capture::OnDraw(dev, call);
    }
    return StereoDraw(dev, [&] { return g_orig.DrawPrimitive(dev, type, start, count); });
}

HRESULT STDMETHODCALLTYPE Hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start, UINT count)
{
    ++g_frameDraws;
    if (Capture::Active()) {
        char call[128];
        snprintf(call, sizeof(call), "DrawIndexedPrimitive t=%d base=%d nv=%u start=%u prims=%u", type, base, numVertices, start, count);
        Capture::OnDraw(dev, call);
    }
    return StereoDraw(dev, [&] { return g_orig.DrawIndexedPrimitive(dev, type, base, minIndex, numVertices, start, count); });
}

HRESULT STDMETHODCALLTYPE Hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    ++g_frameDraws;
    if (Capture::Active()) {
        char call[96];
        snprintf(call, sizeof(call), "DrawPrimitiveUP t=%d prims=%u stride=%u", type, count, stride);
        Capture::OnDraw(dev, call);
    }
    return StereoDraw(dev, [&] { return g_orig.DrawPrimitiveUP(dev, type, count, data, stride); });
}

HRESULT STDMETHODCALLTYPE Hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count, const void* indices, D3DFORMAT fmt, const void* data, UINT stride)
{
    ++g_frameDraws;
    if (Capture::Active()) {
        char call[128];
        snprintf(call, sizeof(call), "DrawIndexedPrimitiveUP t=%d nv=%u prims=%u stride=%u", type, numVertices, count, stride);
        Capture::OnDraw(dev, call);
    }
    return StereoDraw(dev, [&] { return g_orig.DrawIndexedPrimitiveUP(dev, type, minIndex, numVertices, count, indices, fmt, data, stride); });
}

HRESULT STDMETHODCALLTYPE Hk_ProcessVertices(IDirect3DDevice9* dev, UINT src, UINT dst, UINT count, IDirect3DVertexBuffer9* vb, IDirect3DVertexDeclaration9* decl, DWORD flags)
{
    if (Capture::Active())
        Capture::Line("ProcessVertices count=%u", count);
    return g_orig.ProcessVertices(dev, src, dst, count, vb, decl, flags);
}

HRESULT STDMETHODCALLTYPE Hk_CreateVertexDeclaration(IDirect3DDevice9* dev, const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out)
{
    HRESULT hr = g_orig.CreateVertexDeclaration(dev, elements, out);
    if (SUCCEEDED(hr) && elements) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "decl %p:", (void*)*out);
        for (const D3DVERTEXELEMENT9* e = elements; e->Stream != 0xFF && n < (int)sizeof(buf) - 32; ++e)
            n += snprintf(buf + n, sizeof(buf) - n, " s%u+%u t%u u%u.%u", e->Stream, e->Offset, e->Type, e->Usage, e->UsageIndex);
        Log("%s", buf);
        Stereo::OnDeclarationCreated(dev, *out, elements);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetVertexDeclaration(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl)
{
    g_shadow.declaration = decl;
    if (decl)
        g_shadow.fvf = 0;
    return g_orig.SetVertexDeclaration(dev, decl);
}

HRESULT STDMETHODCALLTYPE Hk_SetFVF(IDirect3DDevice9* dev, DWORD fvf)
{
    g_shadow.fvf = fvf;
    if (fvf)
        g_shadow.declaration = nullptr;
    return g_orig.SetFVF(dev, fvf);
}

HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out)
{
    HRESULT hr = g_orig.CreateVertexShader(dev, code, out);
    if (SUCCEEDED(hr) && code)
        RegisterShader(*out, code, true);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
    g_shadow.vertexShader = vs;
    if (Capture::Active())
        Capture::Line("SetVertexShader %s", Capture::ShaderLabel(vs).c_str());
    return g_orig.SetVertexShader(dev, vs);
}

HRESULT STDMETHODCALLTYPE Hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{
    Probe::OnSetVertexShaderConstantF(start, data, count, _ReturnAddress());
    if (data && start < 256) {
        UINT n = start + count > 256 ? 256 - start : count;
        memcpy(g_shadow.vsConst[start], data, n * 16);
        if (Capture::Active()) {
            for (UINT i = 0; i < count; ++i)
                Capture::Line("VSConst c%u = %.5f %.5f %.5f %.5f", start + i, data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
        }
    }
    return g_orig.SetVertexShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out)
{
    HRESULT hr = g_orig.CreatePixelShader(dev, code, out);
    if (SUCCEEDED(hr) && code)
        RegisterShader(*out, code, false);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
    g_shadow.pixelShader = ps;
    if (Capture::Active())
        Capture::Line("SetPixelShader %s", Capture::ShaderLabel(ps).c_str());
    return g_orig.SetPixelShader(dev, ps);
}

HRESULT STDMETHODCALLTYPE Hk_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{
    if (Capture::Active() && data)
        Capture::Line("PSConst c%u x%u = %.4f %.4f %.4f %.4f ...", start, count, data[0], data[1], data[2], data[3]);
    return g_orig.SetPixelShaderConstantF(dev, start, data, count);
}

void InstallDeviceHooks(IDirect3DDevice9* dev)
{
#define HOOK(slot, name) HookSlot(dev, k##slot, &Hk_##name, g_orig.name)
    HOOK(Reset, Reset);
    HOOK(Present, Present);
    HOOK(CreateAdditionalSwapChain, CreateAdditionalSwapChain);
    HOOK(GetRenderTargetData, GetRenderTargetData);
    HOOK(StretchRect, StretchRect);
    HOOK(ColorFill, ColorFill);
    HOOK(SetRenderTarget, SetRenderTarget);
    HOOK(GetRenderTarget, GetRenderTarget);
    HOOK(SetDepthStencilSurface, SetDepthStencilSurface);
    HOOK(GetDepthStencilSurface, GetDepthStencilSurface);
    HOOK(GetViewport, GetViewport);
    HOOK(BeginScene, BeginScene);
    HOOK(EndScene, EndScene);
    HOOK(Clear, Clear);
    HOOK(SetTransform, SetTransform);
    HOOK(MultiplyTransform, MultiplyTransform);
    HOOK(SetViewport, SetViewport);
    HOOK(SetRenderState, SetRenderState);
    HOOK(CreateStateBlock, CreateStateBlock);
    HOOK(BeginStateBlock, BeginStateBlock);
    HOOK(EndStateBlock, EndStateBlock);
    HOOK(SetTexture, SetTexture);
    HOOK(SetScissorRect, SetScissorRect);
    HOOK(DrawPrimitive, DrawPrimitive);
    HOOK(DrawIndexedPrimitive, DrawIndexedPrimitive);
    HOOK(DrawPrimitiveUP, DrawPrimitiveUP);
    HOOK(DrawIndexedPrimitiveUP, DrawIndexedPrimitiveUP);
    HOOK(ProcessVertices, ProcessVertices);
    HOOK(CreateVertexDeclaration, CreateVertexDeclaration);
    HOOK(SetVertexDeclaration, SetVertexDeclaration);
    HOOK(SetFVF, SetFVF);
    HOOK(CreateVertexShader, CreateVertexShader);
    HOOK(SetVertexShader, SetVertexShader);
    HOOK(SetVertexShaderConstantF, SetVertexShaderConstantF);
    HOOK(CreatePixelShader, CreatePixelShader);
    HOOK(SetPixelShader, SetPixelShader);
    HOOK(SetPixelShaderConstantF, SetPixelShaderConstantF);
#undef HOOK

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        HookSlot(sc, kSwapChainPresent, &Hk_SwapChainPresent, g_orig.SwapChainPresent);
        sc->Release();
    }
}

HRESULT STDMETHODCALLTYPE Hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    Log("CreateDevice adapter=%u type=%d hwnd=%p flags=0x%lx", adapter, type, (void*)wnd, flags);
    LogPresentParams("CreateDevice", pp);
    g_window = wnd;
    ApplyWindowedOverride(pp);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice -> 0x%08lx dev=%p", hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DDevice9* dev = *out;
        ApplyBorderlessWindow(pp);
        D3DCAPS9 caps;
        if (SUCCEEDED(dev->GetDeviceCaps(&caps))) {
            g_maxVsConst = caps.MaxVertexShaderConst < 256 ? caps.MaxVertexShaderConst : 256;
            Log("caps: VS %08lx PS %08lx maxVSConst=%lu maxRT=%lu maxTex=%lux%lu", caps.VertexShaderVersion, caps.PixelShaderVersion,
                caps.MaxVertexShaderConst, caps.NumSimultaneousRTs, caps.MaxTextureWidth, caps.MaxTextureHeight);
        }
        InstallDeviceHooks(dev);
        RefreshDeviceSurfaces(dev);
    }
    return hr;
}

} // namespace

const D3D9Methods& D3D9Orig() { return g_orig; }
ShadowState& Shadow() { return g_shadow; }

ShaderInfo* FindShader(void* shader)
{
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    auto it = g_shaders.find(shader);
    return it == g_shaders.end() ? nullptr : &it->second;
}

void Hooks_OnDirect3DCreated(IDirect3D9* d3d)
{
    HookSlot(d3d, kD3D9CreateDevice, &Hk_CreateDevice, g_origCreateDevice);
}
