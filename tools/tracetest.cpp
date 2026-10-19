// Single-steps IDirect3DDevice9::CreateTexture with and without a shared handle and
// prints where the two executions diverge inside d3d9.dll (the non-Ex sharing check).
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>
#include <cstdio>
#include <vector>

static DWORD g_lo, g_hi;
static volatile bool g_tracing;
static std::vector<DWORD>* g_trace; // reserved up front: no allocation inside the handler

static LONG CALLBACK OnStep(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    DWORD eip = c->Eip;
    if (c->Dr6 & 1) {
        // Back from a stepped-over call: resume single-stepping.
        c->Dr7 = 0;
        c->Dr6 = 0;
    }
    bool inD3d9 = eip >= g_lo && eip < g_hi;
    if (inD3d9 && g_trace->size() < g_trace->capacity())
        g_trace->push_back(eip - g_lo);
    if (!g_tracing)
        return EXCEPTION_CONTINUE_EXECUTION;
    DWORD ret = *(const DWORD*)c->Esp;
    if (!inD3d9 && ret >= g_lo && ret < g_hi) {
        // Entered another module from d3d9 (possibly heading for a WoW64 system call,
        // which cannot be single-stepped): run it at full speed until it returns.
        c->Dr0 = ret;
        c->Dr7 = 1;
        c->EFlags &= ~0x100u;
    } else {
        c->EFlags |= 0x100;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static __declspec(noinline) HRESULT Traced(IDirect3DDevice9* dev, HANDLE* handle, std::vector<DWORD>& trace)
{
    IDirect3DTexture9* tex = nullptr;
    g_trace = &trace;
    g_tracing = true;
    __asm {
        pushfd
        or dword ptr [esp], 0x100
        popfd
    }
    HRESULT hr = dev->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, handle);
    g_tracing = false;
    if (tex)
        tex->Release();
    return hr;
}

int main()
{
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\d3d9.dll");
    HMODULE sys = LoadLibraryA(path);
    MODULEINFO mi;
    GetModuleInformation(GetCurrentProcess(), sys, &mi, sizeof(mi));
    g_lo = (DWORD)mi.lpBaseOfDll;
    g_hi = g_lo + mi.SizeOfImage;
    auto create = (IDirect3D9*(WINAPI*)(UINT))GetProcAddress(sys, "Direct3DCreate9");

    HWND wnd = CreateWindowA("STATIC", "trace", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d9 = create(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    IDirect3DDevice9* dev = nullptr;
    d3d9->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    AddVectoredExceptionHandler(1, OnStep);

    std::vector<DWORD> plain, shared;
    plain.reserve(4000000);
    shared.reserve(4000000);
    HRESULT a = Traced(dev, nullptr, plain);
    HANDLE handle = nullptr;
    HRESULT b = Traced(dev, &handle, shared);
    printf("d3d9 base %08lx; plain 0x%08lx (%u steps), shared 0x%08lx (%u steps)\n", g_lo, a, (unsigned)plain.size(), b,
        (unsigned)shared.size());

    size_t i = 0;
    while (i < plain.size() && i < shared.size() && plain[i] == shared[i])
        ++i;
    printf("diverge at step %u\n", (unsigned)i);
    for (size_t k = i >= 6 ? i - 6 : 0; k < i + 8; ++k)
        printf("  %5u  plain %s%08lx   shared %s%08lx\n", (unsigned)k, k == i ? ">" : " ", k < plain.size() ? plain[k] : 0,
            k == i ? ">" : " ", k < shared.size() ? shared[k] : 0);
    // Shared trace from the divergence to the failing return, for disassembly.
    printf("shared tail:");
    for (size_t k = i; k < shared.size() && k < i + 60; ++k)
        printf(" %lx", shared[k]);
    printf("\n");
    return 0;
}

