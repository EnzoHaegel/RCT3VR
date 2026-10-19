#include "capture.h"
#include "config.h"
#include "hooks.h"
#include "image.h"
#include "log.h"

#include <d3dcompiler.h>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>

namespace Capture {
namespace {

bool g_active = false;
FILE* g_file = nullptr;
std::wstring g_dir;
int g_seq = 0;
int g_draws = 0;
int g_snapshots = 0;
int g_drawsOnTarget = 0; // draws/clears since render target 0 was last changed
std::map<void*, int> g_surfaceIds;
std::map<void*, int> g_textureIds;
std::set<ShaderInfo*> g_usedShaders;

std::string Format(const char* fmt, va_list args)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    return buf;
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w)
        s += (char)c; // paths here are ASCII
    return s;
}

void Snapshot(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const char* reason)
{
    if (!surface || g_snapshots >= Cfg().maxSnapshots)
        return;
    char name[128];
    snprintf(name, sizeof(name), "snap_%02d_%s_%s.png", g_snapshots, SurfaceLabel(surface).c_str(), reason);
    std::string error;
    std::wstring wname(name, name + strlen(name));
    if (SaveSurfacePng(dev, surface, g_dir + wname, error)) {
        Line("  -> saved %s", name);
        ++g_snapshots;
    } else {
        Line("  -> snapshot of %s failed: %s", SurfaceLabel(surface).c_str(), error.c_str());
    }
}

void DumpShader(ShaderInfo* info)
{
    char base[64];
    snprintf(base, sizeof(base), "%s_%03d", info->vertex ? "vs" : "ps", info->id);
    std::wstring wbase(base, base + strlen(base));

    if (FILE* f = _wfopen((g_dir + wbase + L".bin").c_str(), L"wb")) {
        fwrite(info->code.data(), 4, info->code.size(), f);
        fclose(f);
    }

    FILE* f = _wfopen((g_dir + wbase + L".txt").c_str(), L"w");
    if (!f)
        return;
    fprintf(f, "// %s, %u dwords, version %08lx\n", base, (unsigned)info->code.size(), info->code[0]);
    for (const ShaderConstant& c : info->constants)
        fprintf(f, "// %-28s set=%d reg=%d count=%d class=%d %dx%d elems=%d\n", c.name.c_str(), c.registerSet,
            c.registerIndex, c.registerCount, c.typeClass, c.rows, c.columns, c.elements);

    static auto disassemble = (decltype(&D3DDisassemble))GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DDisassemble");
    ID3DBlob* text = nullptr;
    if (disassemble && SUCCEEDED(disassemble(info->code.data(), info->code.size() * 4, 0, nullptr, &text))) {
        fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, f);
        text->Release();
    } else {
        fprintf(f, "// disassembly failed\n");
    }
    fclose(f);
}

// Projection matrix (row-vector convention) actually used by the next draw.
bool CurrentProjection(D3DMATRIX& m, const char*& source)
{
    ShadowState& s = Shadow();
    if (!s.vertexShader) {
        m = s.proj;
        source = "ff";
        return true;
    }
    ShaderInfo* info = FindShader(s.vertexShader);
    if (!info || info->projReg < 0)
        return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            m.m[i][j] = info->projClass == 3 ? s.vsConst[info->projReg + j][i] : s.vsConst[info->projReg + i][j];
    source = "vs";
    return true;
}

} // namespace

bool Active() { return g_active; }

void Line(const char* fmt, ...)
{
    if (!g_file)
        return;
    va_list args;
    va_start(args, fmt);
    std::string s = Format(fmt, args);
    va_end(args);
    fprintf(g_file, "%05d %s\n", g_seq++, s.c_str());
}

std::string SurfaceLabel(IDirect3DSurface9* surface)
{
    if (!surface)
        return "null";
    if (surface == Shadow().backBuffer)
        return "BB";
    if (surface == Shadow().autoDepth)
        return "DS0";
    auto it = g_surfaceIds.find(surface);
    if (it != g_surfaceIds.end())
        return "S" + std::to_string(it->second);

    int id = (int)g_surfaceIds.size() + 1;
    g_surfaceIds[surface] = id;
    D3DSURFACE_DESC d = {};
    surface->GetDesc(&d);
    IDirect3DBaseTexture9* container = nullptr;
    bool isTexture = SUCCEEDED(surface->GetContainer(IID_IDirect3DBaseTexture9, (void**)&container));
    if (container)
        container->Release();
    Line("  [S%d = %ux%u fmt=%d ms=%d usage=0x%lx pool=%d %s]", id, d.Width, d.Height, d.Format, d.MultiSampleType,
        d.Usage, d.Pool, isTexture ? "texture" : "surface");
    return "S" + std::to_string(id);
}

std::string TextureLabel(IDirect3DBaseTexture9* texture)
{
    if (!texture)
        return "null";
    auto it = g_textureIds.find(texture);
    if (it != g_textureIds.end())
        return "T" + std::to_string(it->second);
    int id = (int)g_textureIds.size() + 1;
    g_textureIds[texture] = id;

    std::string extra;
    if (texture->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* t = (IDirect3DTexture9*)texture;
        D3DSURFACE_DESC d = {};
        t->GetLevelDesc(0, &d);
        char buf[160];
        snprintf(buf, sizeof(buf), "%ux%u fmt=%d levels=%lu usage=0x%lx", d.Width, d.Height, d.Format, t->GetLevelCount(), d.Usage);
        extra = buf;
        if (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
            IDirect3DSurface9* level0 = nullptr;
            if (SUCCEEDED(t->GetSurfaceLevel(0, &level0))) {
                extra += " level0=" + SurfaceLabel(level0);
                level0->Release();
            }
        }
    } else {
        extra = "type=" + std::to_string(texture->GetType());
    }
    Line("  [T%d = %s]", id, extra.c_str());
    return "T" + std::to_string(id);
}

std::string ShaderLabel(void* shader)
{
    if (!shader)
        return "ff";
    ShaderInfo* info = FindShader(shader);
    if (!info)
        return "unknown";
    if (g_active)
        g_usedShaders.insert(info);
    return std::string(info->vertex ? "vs" : "ps") + std::to_string(info->id);
}

void Start(IDirect3DDevice9* dev, int frame)
{
    for (int n = 1;; ++n) {
        std::wstring dir = GameDir() + L"rct3vr_capture_" + std::to_wstring(n) + L"\\";
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            g_dir = dir;
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS || n > 999)
            return;
    }
    g_file = _wfopen((g_dir + L"calls.log").c_str(), L"w");
    if (!g_file)
        return;
    g_active = true;
    g_seq = g_draws = g_snapshots = 0;
    g_drawsOnTarget = 1; // whatever is bound may hold earlier content
    g_surfaceIds.clear();
    g_textureIds.clear();
    g_usedShaders.clear();
    Log("capture started at frame %d -> %s", frame, Narrow(g_dir).c_str());

    ShadowState& s = Shadow();
    Line("frame %d; RT0=%s DS=%s vs=%s ps=%s fvf=0x%lx vp=%lu,%lu %lux%lu", frame, SurfaceLabel(s.renderTargets[0]).c_str(),
        SurfaceLabel(s.depthStencil).c_str(), ShaderLabel(s.vertexShader).c_str(), ShaderLabel(s.pixelShader).c_str(), s.fvf,
        s.viewport.X, s.viewport.Y, s.viewport.Width, s.viewport.Height);
    SurfaceLabel(s.backBuffer);
    (void)dev;
}

void Finish(IDirect3DDevice9* dev)
{
    if (!g_active)
        return;
    ShadowState& s = Shadow();
    if (g_drawsOnTarget > 0 && s.renderTargets[0] != s.backBuffer)
        Snapshot(dev, s.renderTargets[0], "end");
    Line("Present (%d draws)", g_draws);
    Snapshot(dev, s.backBuffer, "present");

    for (ShaderInfo* info : g_usedShaders)
        DumpShader(info);
    fclose(g_file);
    g_file = nullptr;
    g_active = false;
    Log("capture finished: %d draws, %d shaders, %d snapshots", g_draws, (int)g_usedShaders.size(), g_snapshots);
}

void BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface)
{
    ShadowState& s = Shadow();
    if (index == 0 && surface != s.renderTargets[0]) {
        if (g_drawsOnTarget > 0)
            Snapshot(dev, s.renderTargets[0], "before_switch");
        g_drawsOnTarget = 0;
    }
    Line("SetRenderTarget %lu %s", index, SurfaceLabel(surface).c_str());
}

void OnClear() { ++g_drawsOnTarget; }

void OnDraw(IDirect3DDevice9* dev, const char* call)
{
    (void)dev;
    ++g_draws;
    ++g_drawsOnTarget;
    ShadowState& s = Shadow();

    char proj[160] = "P=?";
    D3DMATRIX m;
    const char* source = "";
    if (CurrentProjection(m, source))
        snprintf(proj, sizeof(proj), "P(%s)=[%.4f %.4f | %.5f %.5f | %.3f %.3f | %.3f %.3f]", source, m._11, m._22, m._33, m._43,
            m._34, m._44, m._31, m._32);

    Line("%s | %s %s decl=%p fvf=0x%lx rt=%s ds=%s vp=%lu,%lu,%lux%lu z=%.2f-%.2f | %s", call, ShaderLabel(s.vertexShader).c_str(),
        ShaderLabel(s.pixelShader).c_str(), (void*)s.declaration, s.fvf, SurfaceLabel(s.renderTargets[0]).c_str(),
        SurfaceLabel(s.depthStencil).c_str(), s.viewport.X, s.viewport.Y, s.viewport.Width, s.viewport.Height, s.viewport.MinZ,
        s.viewport.MaxZ, proj);
}

} // namespace Capture
