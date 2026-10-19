#include "stereo.h"
#include "config.h"
#include "hooks.h"
#include "keys.h"
#include "log.h"
#include "probe.h"
#include "sharepatch.h"
#include "xr.h"

#include <d3dcompiler.h>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>

namespace Stereo {
namespace {

struct Mat4 {
    float m[4][4];
};

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// Constant register used by our screen-space vertex shader (game shaders stay far below it).
const int kScreenConstReg = 250;

struct ScreenDecl {
    IDirect3DVertexDeclaration9* twin = nullptr; // same layout, POSITIONT replaced by POSITION
    IDirect3DVertexShader9* shader = nullptr;
};

// Per-eye view transform (game view space -> eye space, row-vector convention)
// and the off-axis projection terms from the eye's field of view.
struct EyeTransform {
    Mat4 view;
    float sx, sy, ox, oy;
};

bool g_master = true;     // F9 master switch
bool g_active = false;    // eye targets exist and replace the back buffer
bool g_headset = false;   // OpenXR session available (else monitor preview)
bool g_frameBegun = false;
bool g_recenterPending = false; // recenter once tracking has settled after activation
DWORD g_recenterDue = 0;
DWORD g_lastActivateFailure = 0;
bool g_failureLogged = false;
bool g_toggleDown = false, g_recenterDown = false;
KeyCombo g_buttonCombos[kButtonCount];
bool g_buttonWasDown[kButtonCount] = {};

// First-person ride camera detection (its near plane is 5 cm; the park camera's is 1 m).
bool g_rideCam = false;
int g_rideFrames = 0, g_otherFrames = 0;

UINT g_eyeW = 0, g_eyeH = 0, g_bbW = 0, g_bbH = 0;
D3DFORMAT g_format = D3DFMT_A8R8G8B8;
IDirect3DSurface9* g_eyeRT = nullptr;
IDirect3DSurface9* g_eyeDS = nullptr;
IDirect3DSurface9* g_resolveRT = nullptr; // CPU path with MSAA
IDirect3DSurface9* g_sysmem = nullptr;    // CPU path readback
IDirect3DTexture9* g_sharedTex = nullptr; // GPU path: opened from the OpenXR D3D11 device
IDirect3DSurface9* g_sharedSurf = nullptr;
IDirect3DQuery9* g_doneQuery = nullptr;
IDirect3DSurface9* g_boundRT = nullptr;
IDirect3DSurface9* g_boundDS = nullptr;

VrEyePose g_poses[2];
EyeTransform g_eyes[2];
std::unordered_map<void*, ScreenDecl> g_screenDecls;
std::set<void*> g_warned;

// Timing stats, logged every 10 s while active.
DWORD g_statsStart = 0;
int g_statsFrames = 0;
double g_waitMs = 0, g_submitMs = 0;
LARGE_INTEGER g_freq;

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_freq.QuadPart;
}

void WarnOnce(void* key, const char* what)
{
    if (g_warned.insert(key).second)
        Log("stereo: %s (%p)", what, key);
}

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void DefaultPoses()
{
    for (int i = 0; i < 2; ++i) {
        VrEyePose& p = g_poses[i];
        p.pos[0] = i == 0 ? -0.032f : 0.032f;
        p.pos[1] = p.pos[2] = 0.0f;
        p.orient[0] = p.orient[1] = p.orient[2] = 0.0f;
        p.orient[3] = 1.0f;
        p.tanLeft = -1.0f;
        p.tanRight = 1.0f;
        p.tanUp = 1.1f;
        p.tanDown = -1.1f;
    }
}

void ComputeEyeTransforms()
{
    const float s = Cfg().worldScale;
    for (int i = 0; i < 2; ++i) {
        const VrEyePose& p = g_poses[i];
        float x = p.orient[0], y = p.orient[1], z = p.orient[2], w = p.orient[3];
        // Rotation (column-vector convention) of the OpenXR eye in LOCAL space.
        float r[3][3] = {
            {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
        };
        // OpenXR is right-handed with -Z forward, D3D view space left-handed with +Z
        // forward: S = diag(1,1,-1). Row-vector view rotation is S*R*S, translation
        // is -(S*p)*(S*R*S).
        const float sign[3] = {1, 1, -1};
        Mat4& v = g_eyes[i].view;
        v = {};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                v.m[a][b] = sign[a] * r[a][b] * sign[b];
        float sp[3] = {p.pos[0] * s, p.pos[1] * s, -p.pos[2] * s};
        for (int b = 0; b < 3; ++b)
            v.m[3][b] = -(sp[0] * v.m[0][b] + sp[1] * v.m[1][b] + sp[2] * v.m[2][b]);
        v.m[3][3] = 1.0f;

        EyeTransform& e = g_eyes[i];
        e.sx = 2.0f / (p.tanRight - p.tanLeft);
        e.ox = -(p.tanRight + p.tanLeft) / (p.tanRight - p.tanLeft);
        e.sy = 2.0f / (p.tanUp - p.tanDown);
        e.oy = -(p.tanUp + p.tanDown) / (p.tanUp - p.tanDown);
    }
}

// Eye view * eye projection, keeping the game's depth mapping (a, b).
Mat4 EyeProjection(int eye, float a, float b)
{
    const EyeTransform& e = g_eyes[eye];
    Mat4 proj = {};
    proj.m[0][0] = e.sx;
    proj.m[1][1] = e.sy;
    proj.m[2][0] = e.ox;
    proj.m[2][1] = e.oy;
    proj.m[2][2] = a;
    proj.m[2][3] = 1.0f;
    proj.m[3][2] = b;
    return Mul(e.view, proj);
}

D3DVIEWPORT9 EyeViewport(int eye)
{
    const D3DVIEWPORT9& vp = Shadow().viewport;
    D3DVIEWPORT9 out;
    out.X = eye * g_eyeW + (DWORD)((UINT64)vp.X * g_eyeW / g_bbW);
    out.Y = (DWORD)((UINT64)vp.Y * g_eyeH / g_bbH);
    out.Width = (DWORD)((UINT64)vp.Width * g_eyeW / g_bbW);
    out.Height = (DWORD)((UINT64)vp.Height * g_eyeH / g_bbH);
    out.MinZ = vp.MinZ;
    out.MaxZ = vp.MaxZ;
    return out;
}

bool FullViewport()
{
    const D3DVIEWPORT9& vp = Shadow().viewport;
    return vp.X == 0 && vp.Y == 0 && vp.Width == g_bbW && vp.Height == g_bbH;
}

// Reads the projection a shader will use, as a row-vector matrix.
Mat4 ReadShaderProjection(int reg, int typeClass)
{
    const ShadowState& s = Shadow();
    Mat4 m;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            m.m[i][j] = typeClass == 3 ? s.vsConst[reg + j][i] : s.vsConst[reg + i][j];
    return m;
}

void WriteShaderProjection(IDirect3DDevice9* dev, int reg, int typeClass, const Mat4& m)
{
    float regs[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            regs[i][j] = typeClass == 3 ? m.m[j][i] : m.m[i][j];
    D3D9Orig().SetVertexShaderConstantF(dev, reg, &regs[0][0], 4);
}

bool IsPerspective(const Mat4& m)
{
    return fabsf(m.m[2][3] - 1.0f) < 1e-3f && fabsf(m.m[3][3]) < 1e-3f && m.m[0][0] > 0.0f;
}

IDirect3DVertexShader9* CompileScreenShader(IDirect3DDevice9* dev, const D3DVERTEXELEMENT9* elements)
{
    static auto compile = (decltype(&D3DCompile))GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile");
    if (!compile)
        return nullptr;

    std::string inputs, outputs, copies;
    for (const D3DVERTEXELEMENT9* e = elements; e->Stream != 0xFF; ++e) {
        const char* semantic = nullptr;
        if (e->Usage == D3DDECLUSAGE_COLOR && e->UsageIndex < 2)
            semantic = "COLOR";
        else if (e->Usage == D3DDECLUSAGE_TEXCOORD && e->UsageIndex < 8)
            semantic = "TEXCOORD";
        if (!semantic)
            continue;
        std::string name = std::string(semantic == std::string("COLOR") ? "c" : "t") + std::to_string(e->UsageIndex);
        std::string decl = "    float4 " + name + " : " + semantic + std::to_string(e->UsageIndex) + ";\n";
        inputs += decl;
        outputs += decl;
        copies += "    o." + name + " = i." + name + ";\n";
    }
    std::string source =
        "float4 c_map : register(c" + std::to_string(kScreenConstReg) + ");\n"
        "struct I {\n    float4 pos : POSITION;\n" + inputs + "};\n"
        "struct O {\n    float4 pos : POSITION;\n" + outputs + "};\n"
        "O main(I i) {\n    O o;\n"
        "    o.pos = float4(i.pos.xy * c_map.xy + c_map.zw, i.pos.z, 1.0);\n" + copies +
        "    return o;\n}\n";

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source.c_str(), source.size(), "rct3vr_screen", nullptr, nullptr, "main", "vs_2_0", 0, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("stereo: screen shader compile failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
        if (errors)
            errors->Release();
        return nullptr;
    }
    if (errors)
        errors->Release();
    IDirect3DVertexShader9* shader = nullptr;
    // Straight to the original: our CreateVertexShader hook would register it as a game shader.
    D3D9Orig().CreateVertexShader(dev, (const DWORD*)code->GetBufferPointer(), &shader);
    code->Release();
    return shader;
}

// Unlocks D3D9 <-> D3D11 texture sharing for the game's device, and checks that it
// really works: a patch that matched but does not help is reverted.
bool EnableSharing(IDirect3DDevice9* dev)
{
    int sites = PatchD3d9SharedResourceCheck(RealD3d9(), true);
    IDirect3DTexture9* probe = nullptr;
    HANDLE handle = nullptr;
    bool works = SUCCEEDED(dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &probe, &handle)) && handle;
    SafeRelease(probe);
    if (!works && sites)
        PatchD3d9SharedResourceCheck(RealD3d9(), false);
    Log("stereo: GPU texture sharing %s (%d patch site(s))", works ? "available" : "unavailable, using CPU copies (slower)", sites);
    return works;
}

bool CreateResources(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = Shadow().backBuffer;
    if (!bb)
        return false;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    g_bbW = desc.Width;
    g_bbH = desc.Height;
    g_format = desc.Format;

    if (g_headset) {
        g_eyeW = Vr::EyeWidth();
        g_eyeH = Vr::EyeHeight();
    } else {
        g_eyeW = 1440;
        g_eyeH = 1584;
    }

    D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
    if (Cfg().msaa >= 2) {
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp;
        dev->GetCreationParameters(&cp);
        if (SUCCEEDED(dev->GetDirect3D(&d3d))) {
            for (int n = Cfg().msaa > 8 ? 8 : Cfg().msaa; n >= 2; n /= 2) {
                if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, g_format, TRUE, (D3DMULTISAMPLE_TYPE)n, nullptr))
                    && SUCCEEDED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_D24S8, TRUE, (D3DMULTISAMPLE_TYPE)n, nullptr))) {
                    ms = (D3DMULTISAMPLE_TYPE)n;
                    break;
                }
            }
            d3d->Release();
        }
    }

    UINT w = g_eyeW * 2, h = g_eyeH;
    HRESULT hr = dev->CreateRenderTarget(w, h, g_format, ms, 0, FALSE, &g_eyeRT, nullptr);
    if (SUCCEEDED(hr))
        hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, ms, 0, FALSE, &g_eyeDS, nullptr);
    if (FAILED(hr)) {
        Log("stereo: cannot create eye targets %ux%u (0x%08lx)", w, h, hr);
        return false;
    }

    // GPU path: resolve straight into a texture shared with the OpenXR device.
    if (g_headset) {
        static bool sharing = EnableSharing(dev);
        HANDLE handle = sharing ? Vr::CreateSharedEyeTexture() : nullptr;
        if (handle && SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_sharedTex, &handle))
            && SUCCEEDED(g_sharedTex->GetSurfaceLevel(0, &g_sharedSurf))
            && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_doneQuery))) {
            Log("stereo: eye targets %ux%u per eye, msaa %d, GPU transfer", g_eyeW, g_eyeH, ms);
            return true;
        }
        SafeRelease(g_sharedSurf);
        SafeRelease(g_sharedTex);
        SafeRelease(g_doneQuery);
    }

    // CPU path (preview, or sharing unavailable): resolve, read back, upload.
    if (ms != D3DMULTISAMPLE_NONE)
        hr = dev->CreateRenderTarget(w, h, g_format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolveRT, nullptr);
    if (SUCCEEDED(hr) && g_headset)
        hr = dev->CreateOffscreenPlainSurface(w, h, g_format, D3DPOOL_SYSTEMMEM, &g_sysmem, nullptr);
    Log("stereo: eye targets %ux%u per eye, format %d, msaa %d, CPU transfer -> 0x%08lx", g_eyeW, g_eyeH, g_format, ms, hr);
    return SUCCEEDED(hr);
}

void ReleaseResources()
{
    SafeRelease(g_eyeRT);
    SafeRelease(g_eyeDS);
    SafeRelease(g_resolveRT);
    SafeRelease(g_sysmem);
    SafeRelease(g_sharedSurf);
    SafeRelease(g_sharedTex);
    SafeRelease(g_doneQuery);
    g_boundRT = g_boundDS = nullptr;
}

void OnButtonPressed(int button, bool focused)
{
    const KeyCombo& combo = g_buttonCombos[button];
    if (combo.recenter) {
        Vr::Recenter();
    } else if (!combo.keys.empty()) {
        if (focused)
            GameKeys::Press(combo);
        else
            Log("stereo: controller button ignored, the game window is not in the foreground");
    }
}

// The ride camera code (RideCarInstance, vtable slot 12) stores a hard-coded 90 degree
// field of view in the camera, then the 5 cm near plane:
//     mov dword ptr [edi+190h], 3FC90FDBh   ; C7 87 90 01 00 00 DB 0F C9 3F
// Its immediate is patched so the game culls with a wider frustum; our eye projections
// replace its own. Found by pattern (0x8E2050 in the Complete Edition 3.2.5.13).
BYTE* FindRideCamFovStore()
{
    static const BYTE pattern[] = {0xC7, 0x87, 0x90, 0x01, 0x00, 0x00, 0xDB, 0x0F, 0xC9, 0x3F};
    static const BYTE nearPlane[] = {0xC7, 0x87, 0x88, 0x01, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3D};
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    BYTE* found = nullptr;
    int count = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        BYTE* p = base + section->VirtualAddress;
        DWORD size = section->Misc.VirtualSize;
        for (DWORD i = 0; i + 64 <= size; ++i) {
            if (memcmp(p + i, pattern, sizeof(pattern)) != 0)
                continue;
            // The near plane store follows within a few instructions.
            for (DWORD k = sizeof(pattern); k < 64 - sizeof(nearPlane); ++k) {
                if (memcmp(p + i + k, nearPlane, sizeof(nearPlane)) == 0) {
                    found = p + i;
                    ++count;
                    break;
                }
            }
        }
    }
    return count == 1 ? found : nullptr;
}

void PatchRideCamFov(bool wide)
{
    static BYTE* store = FindRideCamFovStore();
    static bool logged = false;
    if (!logged) {
        Log(store ? "stereo: ride camera FOV code at %p" : "stereo: ride camera FOV code not found (other game version?), FOV left at 90",
            (void*)store);
        logged = true;
    }
    if (!store)
        return;
    float value = wide && Cfg().gameFov > 0.0f ? Cfg().gameFov * 3.14159265f / 180.0f : 1.57079637f;
    BYTE* imm = store + 6;
    DWORD old;
    VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(imm, &value, 4);
    VirtualProtect(imm, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), store, 10);
}

bool RideCameraVisible()
{
    float m[4][4];
    if (!Probe::LastSceneProjection(m) || m[2][2] == 0.0f)
        return false;
    float zNear = -m[3][2] / m[2][2];
    return zNear > 0.0f && zNear < 0.2f;
}

bool Activate(IDirect3DDevice9* dev)
{
    g_headset = Vr::IsInitialized();
    if (!g_headset) {
        std::string error;
        g_headset = Vr::Init(error);
        if (!g_headset && !g_failureLogged)
            Log("stereo: no headset (%s)%s", error.c_str(), Cfg().previewWithoutHeadset ? ", monitor preview" : "");
        g_failureLogged = !g_headset;
    }
    if (!g_headset && !Cfg().previewWithoutHeadset)
        return false;
    if (!CreateResources(dev)) {
        ReleaseResources();
        return false;
    }
    DefaultPoses();
    ComputeEyeTransforms();
    g_active = true;
    BindTargets(dev, true);
    PatchRideCamFov(true);
    g_recenterPending = g_headset;
    g_recenterDue = 0;
    for (bool& b : g_buttonWasDown)
        b = true; // a button still held from before must be released first
    Log("stereo: VR on (%s)", g_headset ? "headset" : "preview");
    return true;
}

void Deactivate(IDirect3DDevice9* dev)
{
    g_active = false;
    PatchRideCamFov(false);
    BindTargets(dev, true);
    D3D9Orig().SetViewport(dev, &Shadow().viewport);
    ReleaseResources();
    if (Vr::IsInitialized())
        Vr::Shutdown();
    g_headset = false;
    g_frameBegun = false;
    Log("stereo: VR off");
}

bool KeyPressed(int key, bool focused, bool& wasDown)
{
    bool down = focused && key && (GetAsyncKeyState(key) & 0x8000);
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

} // namespace

bool Active() { return g_active; }

bool Intercepting()
{
    return g_active && Shadow().renderTargets[0] == Shadow().backBuffer;
}

void OnDeclarationCreated(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl, const D3DVERTEXELEMENT9* elements)
{
    D3DVERTEXELEMENT9 twin[MAXD3DDECLLENGTH + 1];
    int n = 0;
    bool pretransformed = false;
    for (const D3DVERTEXELEMENT9* e = elements; e->Stream != 0xFF && n < MAXD3DDECLLENGTH; ++e, ++n) {
        twin[n] = *e;
        if (e->Usage == D3DDECLUSAGE_POSITIONT) {
            twin[n].Usage = D3DDECLUSAGE_POSITION;
            pretransformed = true;
        }
    }
    if (!pretransformed)
        return;
    twin[n] = D3DDECL_END();

    ScreenDecl sd;
    if (FAILED(D3D9Orig().CreateVertexDeclaration(dev, twin, &sd.twin)))
        return;
    sd.shader = CompileScreenShader(dev, elements);
    if (!sd.shader) {
        sd.twin->Release();
        return;
    }
    g_screenDecls[decl] = sd; // kept for the lifetime of the game (declarations are few)
}

void BindTargets(IDirect3DDevice9* dev, bool force)
{
    const ShadowState& s = Shadow();
    bool eyes = g_active && s.renderTargets[0] == s.backBuffer;
    IDirect3DSurface9* rt = eyes ? g_eyeRT : s.renderTargets[0];
    IDirect3DSurface9* ds = eyes ? g_eyeDS : s.depthStencil;
    if (force || rt != g_boundRT) {
        D3D9Orig().SetRenderTarget(dev, 0, rt);
        g_boundRT = rt;
    }
    if (force || ds != g_boundDS) {
        D3D9Orig().SetDepthStencilSurface(dev, ds);
        g_boundDS = ds;
    }
}

HRESULT Clear(IDirect3DDevice9* dev, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    HRESULT hr = D3D_OK;
    for (int eye = 0; eye < 2; ++eye) {
        D3DVIEWPORT9 vp = EyeViewport(eye);
        D3D9Orig().SetViewport(dev, &vp);
        hr = D3D9Orig().Clear(dev, 0, nullptr, flags, color, z, stencil);
    }
    return hr;
}

DrawPlan PrepareDraw(IDirect3DDevice9* dev)
{
    const ShadowState& s = Shadow();
    DrawPlan plan;
    plan.kind = kMono;

    if (s.declaration) {
        auto it = g_screenDecls.find(s.declaration);
        if (it != g_screenDecls.end()) {
            if (s.vertexShader) {
                WarnOnce(s.declaration, "pre-transformed declaration with a vertex shader, drawn as is");
                return plan;
            }
            plan.kind = kScreen;
            D3D9Orig().SetVertexDeclaration(dev, it->second.twin);
            D3D9Orig().SetVertexShader(dev, it->second.shader);
            // Back buffer pixels -> clip space; the eye viewport then places it.
            float map[4] = {2.0f / g_bbW, -2.0f / g_bbH, -1.0f, 1.0f};
            D3D9Orig().SetVertexShaderConstantF(dev, kScreenConstReg, map, 1);
            return plan;
        }
    } else if ((s.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        WarnOnce((void*)(UINT_PTR)s.fvf, "pre-transformed FVF draw skipped");
        plan.kind = kSkip;
        return plan;
    }

    if (!FullViewport())
        return plan; // HUD-style 3D inset: keep the game's matrices

    if (s.vertexShader) {
        ShaderInfo* info = FindShader(s.vertexShader);
        if (!info || info->projReg < 0) {
            WarnOnce(s.vertexShader, "vertex shader without c_projMat, drawn mono");
            return plan;
        }
        Mat4 p = ReadShaderProjection(info->projReg, info->projClass);
        if (!IsPerspective(p))
            return plan;
        plan.kind = kStereo;
        plan.projReg = info->projReg;
        plan.projClass = info->projClass;
        plan.a = p.m[2][2];
        plan.b = p.m[3][2];
    } else {
        Mat4 p;
        memcpy(&p, &s.proj, sizeof(p));
        if (!IsPerspective(p))
            return plan;
        plan.kind = kStereoFF;
        plan.a = p.m[2][2];
        plan.b = p.m[3][2];
    }
    return plan;
}

void SetupEye(IDirect3DDevice9* dev, const DrawPlan& plan, int eye)
{
    D3DVIEWPORT9 vp = EyeViewport(eye);
    D3D9Orig().SetViewport(dev, &vp);
    if (plan.kind == kStereo) {
        WriteShaderProjection(dev, plan.projReg, plan.projClass, EyeProjection(eye, plan.a, plan.b));
    } else if (plan.kind == kStereoFF) {
        Mat4 m = EyeProjection(eye, plan.a, plan.b);
        D3D9Orig().SetTransform(dev, D3DTS_PROJECTION, (const D3DMATRIX*)&m);
    }
}

void FinishDraw(IDirect3DDevice9* dev, const DrawPlan& plan)
{
    const ShadowState& s = Shadow();
    if (plan.kind == kStereo) {
        D3D9Orig().SetVertexShaderConstantF(dev, plan.projReg, s.vsConst[plan.projReg], 4);
    } else if (plan.kind == kStereoFF) {
        D3D9Orig().SetTransform(dev, D3DTS_PROJECTION, &s.proj);
    } else if (plan.kind == kScreen) {
        D3D9Orig().SetVertexDeclaration(dev, s.declaration);
        D3D9Orig().SetVertexShader(dev, s.vertexShader);
        D3D9Orig().SetVertexShaderConstantF(dev, kScreenConstReg, s.vsConst[kScreenConstReg], 1);
    }
}

void BeforePresent(IDirect3DDevice9* dev)
{
    if (!g_active)
        return;
    const D3D9Methods& d3d = D3D9Orig();
    IDirect3DSurface9* image = g_eyeRT;
    IDirect3DSurface9* resolved = g_sharedSurf ? g_sharedSurf : g_resolveRT;
    if (resolved) {
        d3d.StretchRect(dev, g_eyeRT, nullptr, resolved, nullptr, D3DTEXF_NONE);
        image = resolved;
    }

    // Monitor: the left eye cropped to the window's aspect, or both eyes in preview.
    IDirect3DSurface9* bb = Shadow().backBuffer;
    if (g_headset) {
        RECT src;
        UINT w = g_eyeW, h = (UINT)((UINT64)g_eyeW * g_bbH / g_bbW);
        if (h > g_eyeH) {
            h = g_eyeH;
            w = (UINT)((UINT64)g_eyeH * g_bbW / g_bbH);
        }
        src.left = (g_eyeW - w) / 2;
        src.top = (g_eyeH - h) / 2;
        src.right = src.left + w;
        src.bottom = src.top + h;
        d3d.StretchRect(dev, image, &src, bb, nullptr, D3DTEXF_LINEAR);
    } else {
        RECT dst = {0, 0, (LONG)g_bbW, (LONG)g_bbH};
        UINT h = (UINT)((UINT64)g_bbW * g_eyeH / (g_eyeW * 2));
        dst.top = (g_bbH - h) / 2;
        dst.bottom = dst.top + h;
        d3d.ColorFill(dev, bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
        d3d.StretchRect(dev, image, nullptr, bb, &dst, D3DTEXF_LINEAR);
    }

    if (g_headset && g_frameBegun) {
        double t0 = NowMs();
        if (g_sharedSurf) {
            // The OpenXR device reads the shared texture: wait until the GPU has written it.
            g_doneQuery->Issue(D3DISSUE_END);
            while (g_doneQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                SwitchToThread();
            Vr::SubmitSharedTexture();
        } else {
            D3DLOCKED_RECT locked;
            if (SUCCEEDED(d3d.GetRenderTargetData(dev, image, g_sysmem))
                && SUCCEEDED(g_sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                Vr::SubmitEyeBGRA(0, locked.pBits, locked.Pitch, g_eyeW, g_eyeH);
                Vr::SubmitEyeBGRA(1, (const BYTE*)locked.pBits + g_eyeW * 4, locked.Pitch, g_eyeW, g_eyeH);
                g_sysmem->UnlockRect();
            }
        }
        Vr::EndFrame();
        g_frameBegun = false;
        g_submitMs += NowMs() - t0;
    }
}

void AfterPresent(IDirect3DDevice9* dev, bool focused)
{
    if (!g_freq.QuadPart) {
        QueryPerformanceFrequency(&g_freq);
        static const char* const names[kButtonCount] = {"A", "B", "X", "Y", "RightStick", "LeftStick"};
        for (int i = 0; i < kButtonCount; ++i) {
            KeyCombo& c = g_buttonCombos[i];
            if (!ParseKeyCombo(Cfg().buttons[i], c))
                Log("stereo: cannot read controller binding %s=\"%ls\"", names[i], Cfg().buttons[i].c_str());
            else if (!c.Empty())
                Log("stereo: button %s -> %ls (%u key(s)%s)", names[i], Cfg().buttons[i].c_str(), (unsigned)c.keys.size(),
                    c.recenter ? ", recenter" : "");
        }
    }
    GameKeys::Update(); // also after VR turned off, to release keys a button just pressed

    if (KeyPressed(Cfg().toggleKey, focused, g_toggleDown)) {
        g_master = !g_master;
        Log("stereo: mod %s", g_master ? "on" : "off");
    }

    // Hysteresis so camera cuts and loading frames do not flip VR on and off.
    if (RideCameraVisible()) {
        g_otherFrames = 0;
        if (++g_rideFrames >= 5)
            g_rideCam = true;
    } else {
        g_rideFrames = 0;
        if (++g_otherFrames >= 15)
            g_rideCam = false;
    }

    bool want = g_master && (!Cfg().autoEnable || g_rideCam);
    if (want && !g_active && GetTickCount() - g_lastActivateFailure > 3000) {
        if (!Activate(dev))
            g_lastActivateFailure = GetTickCount();
    } else if (!want && g_active) {
        Deactivate(dev);
    }
    if (!g_active)
        return;

    if (KeyPressed(Cfg().recenterKey, focused, g_recenterDown) && g_headset)
        Vr::Recenter();

    if (g_headset) {
        double t0 = NowMs();
        VrEyePose poses[2];
        g_frameBegun = Vr::BeginFrame(poses);
        g_waitMs += NowMs() - t0;
        if (g_frameBegun) {
            g_poses[0] = poses[0];
            g_poses[1] = poses[1];
            // The first tracked poses can be bogus: let tracking settle before recentering.
            if (g_recenterPending && !g_recenterDue)
                g_recenterDue = GetTickCount() + 500;
            if (g_recenterPending && GetTickCount() >= g_recenterDue) {
                Vr::Recenter(); // straight ahead in the headset = the ride camera's forward
                g_recenterPending = false;
            }
        }
        for (int i = 0; i < kButtonCount; ++i) {
            bool down = Vr::ButtonDown((VrButton)i);
            if (down && !g_buttonWasDown[i])
                OnButtonPressed(i, focused);
            g_buttonWasDown[i] = down;
        }
        if (!Vr::IsInitialized()) {
            // Session lost (headset disconnected or runtime exit): back to flat, retry later.
            Deactivate(dev);
            g_lastActivateFailure = GetTickCount();
            return;
        }
        ComputeEyeTransforms();
    }

    ++g_statsFrames;
    DWORD now = GetTickCount();
    if (!g_statsStart)
        g_statsStart = now;
    if (now - g_statsStart >= 10000) {
        Log("stereo: %.1f fps, wait %.2f ms/frame, transfer+submit %.2f ms/frame", g_statsFrames * 1000.0 / (now - g_statsStart),
            g_waitMs / g_statsFrames, g_submitMs / g_statsFrames);
        g_statsStart = now;
        g_statsFrames = 0;
        g_waitMs = g_submitMs = 0;
    }
}

void OnBeforeReset(IDirect3DDevice9* dev)
{
    // Default-pool surfaces must be gone before Reset; AfterPresent reactivates.
    if (g_active)
        Deactivate(dev);
}

} // namespace Stereo
