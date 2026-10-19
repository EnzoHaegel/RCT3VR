// d3d9.dll proxy: forwards every export to the system d3d9.dll and hooks the
// IDirect3D9 object handed to the game.
#include <windows.h>
#include <d3d9.h>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "version.h"

#define FORWARDED_EXPORTS(X)                \
    X(Direct3DCreate9Ex)                     \
    X(Direct3DShaderValidatorCreate9)        \
    X(D3DPERF_BeginEvent)                    \
    X(D3DPERF_EndEvent)                      \
    X(D3DPERF_GetStatus)                     \
    X(D3DPERF_QueryRepeatFrame)              \
    X(D3DPERF_SetMarker)                     \
    X(D3DPERF_SetOptions)                    \
    X(D3DPERF_SetRegion)                     \
    X(DebugSetLevel)                         \
    X(DebugSetMute)                          \
    X(Direct3D9EnableMaximizedWindowedModeShim) \
    X(PSGPError)                             \
    X(PSGPSampleTexture)

static HMODULE g_real;
static IDirect3D9*(WINAPI* g_realCreate9)(UINT);

#define DECLARE_PTR(name) extern "C" FARPROC g_real_##name = nullptr;
FORWARDED_EXPORTS(DECLARE_PTR)

static void LoadReal()
{
    if (g_real)
        return;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for this 32-bit process
    wcscat_s(path, L"\\d3d9.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) {
        Log("FATAL: cannot load system d3d9.dll (%lu)", GetLastError());
        return;
    }
    g_realCreate9 = (decltype(g_realCreate9))GetProcAddress(g_real, "Direct3DCreate9");
#define RESOLVE(name) g_real_##name = GetProcAddress(g_real, #name);
    FORWARDED_EXPORTS(RESOLVE)
}

// Signature-agnostic forwarders: resolve lazily, then tail-jump into the real export.
#define DEFINE_STUB(name)                                          \
    extern "C" __declspec(naked) void Stub_##name()                \
    {                                                              \
        __asm { pushad }                                           \
        __asm { call LoadReal }                                    \
        __asm { popad }                                            \
        __asm { jmp dword ptr [g_real_##name] }                    \
    }                                                              \
    __pragma(comment(linker, "/EXPORT:" #name "=_Stub_" #name))
FORWARDED_EXPORTS(DEFINE_STUB)

HMODULE RealD3d9()
{
    LoadReal();
    return g_real;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
{
    LoadReal();
    if (!g_realCreate9)
        return nullptr;
    IDirect3D9* d3d = g_realCreate9(sdkVersion);
    Log("Direct3DCreate9(%u) -> %p", sdkVersion, d3d);
    if (d3d)
        Hooks_OnDirect3DCreated(d3d);
    return d3d;
}
#pragma comment(linker, "/EXPORT:Direct3DCreate9=_Proxy_Direct3DCreate9@4")

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Log("rct3vr %s loaded into pid %lu", RCT3VR_VERSION_STRING, GetCurrentProcessId());
        LoadConfig();
    }
    return TRUE;
}
