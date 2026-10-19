#include "probe.h"
#include "hooks.h"
#include "log.h"

#include <tlhelp32.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace Probe {
namespace {

bool g_haveFrameProj = false;
float g_proj[4][4];      // row-vector convention
float g_lastProj[4][4];  // from the previous complete frame
bool g_haveLast = false;

// Run() arms a one-frame trace of the projection uploads, then breakpoints.
bool g_tracing = false;
struct Upload {
    const void* caller;
    const float* data;
};
std::vector<Upload> g_uploads;

DWORD_PTR g_watch[4] = {};
int g_hits[4] = {};
PVOID g_handler = nullptr;

bool Readable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    return (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool IsGameCode(DWORD v)
{
    return v >= 0x401000 && v < 0x1040000; // RCT3.exe .text
}

bool OnCurrentStack(const void* p)
{
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return p >= tib->StackLimit && p < tib->StackBase;
}

// "module+offset" for an address, for reading the log against a disassembly.
void DescribeAddress(DWORD address, char* out, size_t size)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module)) {
        char path[MAX_PATH];
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        strcpy_s(name, slash ? slash + 1 : path);
        snprintf(out, size, "%08lx(%s+%lx)", address, name, address - (DWORD)module);
    } else {
        snprintf(out, size, "%08lx", address);
    }
}

void LogStackRefs(const DWORD* sp)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "probe:   game code on stack:");
    for (int k = 0; k < 256 && n < (int)sizeof(buf) - 16; ++k)
        if (IsGameCode(sp[k]))
            n += snprintf(buf + n, sizeof(buf) - n, " [%d]%08lx", k, sp[k]);
    Log("%s", buf);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    DWORD status = (DWORD)c->Dr6;
    bool ours = false;
    for (int i = 0; i < 4; ++i) {
        if (!(status & (1u << i)) || !g_watch[i])
            continue;
        ours = true;
        float value;
        memcpy(&value, (void*)g_watch[i], 4);
        char where[160];
        DescribeAddress(c->Eip, where, sizeof(where));
        Log("probe: write to %08lx (now %g) thread %lu eip=%s eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
            (DWORD)g_watch[i], value, GetCurrentThreadId(), where, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        LogStackRefs((const DWORD*)c->Esp);
        if (++g_hits[i] >= 3)
            c->Dr7 &= ~(3u << (i * 2)); // enough samples from this one
    }
    if (!ours)
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI ArmBreakpoints(LPVOID)
{
    // Debug registers are per thread: arm them on every game thread.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t)
            continue;
        SuspendThread(t);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            DWORD dr7 = 0;
            DWORD* regs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
            for (int i = 0; i < 4; ++i) {
                if (!g_watch[i])
                    continue;
                *regs[i] = (DWORD)g_watch[i];
                dr7 |= 1u << (i * 2);                    // local enable
                dr7 |= (1u | (3u << 2)) << (16 + i * 4); // break on write, 4 bytes
            }
            c.Dr7 = dr7;
            if (SetThreadContext(t, &c))
                ++armed;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("probe: breakpoints armed on %d threads", armed);
    return 0;
}

bool InExeRdata(DWORD v)
{
    return v >= 0x103F000 && v < 0x1294000;
}

// MSVC RTTI: vtable[-1] -> complete object locator -> type descriptor -> ".?AVName@@".
const char* RttiName(DWORD vtable)
{
    __try {
        if (!InExeRdata(vtable))
            return nullptr;
        DWORD col = *(const DWORD*)(vtable - 4);
        if (!InExeRdata(col) || *(const DWORD*)col != 0)
            return nullptr;
        DWORD td = *(const DWORD*)(col + 12);
        const char* name = (const char*)(td + 8);
        if (td < 0x400000 || td >= 0x1300000 || name[0] != '.' || name[1] != '?')
            return nullptr;
        return name;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Finds the polymorphic object that contains `address` (vtable within 0x400 bytes before it).
const char* OwningObject(DWORD_PTR address, DWORD_PTR& object)
{
    for (DWORD k = 0; k <= 0x400; k += 4) {
        DWORD v = 0;
        __try {
            v = *(const DWORD*)(address - k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (const char* name = RttiName(v)) {
            object = address - k;
            return name;
        }
    }
    return nullptr;
}

void AddWatch(DWORD_PTR address, int& count)
{
    for (int i = 0; i < count; ++i)
        if (g_watch[i] == address)
            return;
    if (count < 4)
        g_watch[count++] = address;
}

// Memory copies of the projection outside this DLL and the current stack.
std::vector<DWORD_PTR> FindStoredCopies(float xs, float ys, float a)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Run, &self);
    std::vector<DWORD_PTR> hits;
    MEMORY_BASIC_INFORMATION mbi;
    for (BYTE* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi); p = (BYTE*)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || !Readable(mbi.Protect) || mbi.AllocationBase == self || OnCurrentStack(mbi.BaseAddress))
            continue;
        const BYTE* base = (const BYTE*)mbi.BaseAddress;
        for (size_t off = 0; off + 64 <= mbi.RegionSize; off += 4) {
            const float* f = (const float*)(base + off);
            if (f[0] == xs && f[5] == ys && f[10] == a)
                hits.push_back((DWORD_PTR)f);
        }
        if ((DWORD_PTR)mbi.BaseAddress + mbi.RegionSize >= 0x7FFE0000)
            break;
    }
    return hits;
}

void FinishTrace()
{
    const float xs = g_lastProj[0][0], ys = g_lastProj[1][1], a = g_lastProj[2][2];
    int watches = 0;
    for (int i = 0; i < 4; ++i)
        g_watch[i] = 0, g_hits[i] = 0;

    // 1. Where the game uploads c_projMat from.
    Log("probe: %d projection uploads this frame", (int)g_uploads.size());
    std::vector<const void*> callers;
    for (const Upload& u : g_uploads) {
        bool seen = false;
        for (const void* c : callers)
            seen |= c == u.caller;
        if (seen)
            continue;
        callers.push_back(u.caller);
        char where[160];
        DescribeAddress((DWORD)u.caller, where, sizeof(where));
        Log("probe: upload from %s data=%p%s", where, (void*)u.data, OnCurrentStack(u.data) ? " (stack)" : "");
        if (!OnCurrentStack(u.data))
            AddWatch((DWORD_PTR)u.data, watches);
    }
    g_uploads.clear();

    // 2. Stored copies. Camera objects keep it row-major (m34 = 1 at index 11);
    // the render queue keeps transposed copies, which are only listed.
    std::vector<DWORD_PTR> hits = FindStoredCopies(xs, ys, a);
    Log("probe: %d stored copies of the projection", (int)hits.size());
    int listed = 0;
    for (size_t h = 0; h < hits.size(); ++h) {
        const float* f = (const float*)hits[h];
        bool rowMajor = f[11] == 1.0f && f[15] == 0.0f;
        if (!rowMajor && listed >= 6)
            continue;
        ++listed;
        DWORD_PTR object = 0;
        const char* owner = rowMajor ? OwningObject(hits[h], object) : nullptr;
        char buf[2048];
        int n = snprintf(buf, sizeof(buf), "probe: %s copy at %08lx owner %s @%08lx +%lx:", rowMajor ? "ROW-MAJOR" : "transposed",
            (DWORD)hits[h], owner ? owner : "?", (DWORD)object, (DWORD)(hits[h] - object));
        for (int k = -40; k < 40 && n < (int)sizeof(buf) - 24; ++k)
            n += snprintf(buf + n, sizeof(buf) - n, "%s%g", k == 0 ? " |" : " ", f[k]);
        Log("%s", buf);
        if (owner) {
            // Whole object as floats, to spot the field of view, aspect and clip planes.
            const float* o = (const float*)object;
            for (DWORD row = 0; row < 0x300; row += 0x40) {
                n = snprintf(buf, sizeof(buf), "probe:   +%03lx:", row);
                for (int k = 0; k < 16; ++k)
                    n += snprintf(buf + n, sizeof(buf) - n, " %g", o[row / 4 + k]);
                Log("%s", buf);
            }
        }
        // The render context copy (written by its SetProjection) is known; watch the others.
        if (rowMajor && owner && !strstr(owner, "Render")) {
            AddWatch(hits[h], watches);       // xs: rewritten when the field of view is applied
            AddWatch(hits[h] + 20, watches);  // ys
        }
    }

    // 3. Trap the next writes to them.
    if (!g_handler)
        g_handler = AddVectoredExceptionHandler(1, OnException);
    HANDLE t = CreateThread(nullptr, 0, ArmBreakpoints, nullptr, 0, nullptr);
    if (t) {
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
}

} // namespace

void OnDraw()
{
    if (g_haveFrameProj)
        return;
    const ShadowState& s = Shadow();
    if (s.renderTargets[0] != s.backBuffer || !s.vertexShader)
        return;
    ShaderInfo* info = FindShader(s.vertexShader);
    if (!info || info->projReg < 0)
        return;
    float m[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            m[i][j] = info->projClass == 3 ? s.vsConst[info->projReg + j][i] : s.vsConst[info->projReg + i][j];
    if (m[2][3] != 1.0f || m[3][3] != 0.0f)
        return;
    memcpy(g_proj, m, sizeof(m));
    g_haveFrameProj = true;
}

void OnSetVertexShaderConstantF(UINT start, const float* data, UINT count, const void* caller)
{
    if (!g_tracing || !data || count < 4 || g_uploads.size() >= 64)
        return;
    // Column-major c_projMat: register 0 starts with xs, register 1 holds ys in .y.
    for (UINT r = 0; r + 4 <= count; ++r) {
        const float* m = data + r * 4;
        if (m[0] == g_lastProj[0][0] && m[5] == g_lastProj[1][1] && m[10] == g_lastProj[2][2]) {
            g_uploads.push_back({caller, m});
            return;
        }
    }
    (void)start;
}

bool LastSceneProjection(float out[4][4])
{
    if (!g_haveLast)
        return false;
    memcpy(out, g_lastProj, sizeof(g_lastProj));
    return true;
}

void OnFrameEnd()
{
    // A frame without any perspective scene draw (menus, loading) clears it.
    g_haveLast = g_haveFrameProj;
    if (g_haveFrameProj)
        memcpy(g_lastProj, g_proj, sizeof(g_proj));
    g_haveFrameProj = false;
    if (g_tracing) {
        g_tracing = false;
        FinishTrace();
    }
}

void Run()
{
    if (!g_haveLast) {
        Log("probe: no scene projection seen yet");
        return;
    }
    Log("probe: scene projection xs=%.6f ys=%.6f a=%.6f b=%.6f (hfov %.2f deg), tracing next frame", g_lastProj[0][0], g_lastProj[1][1],
        g_lastProj[2][2], g_lastProj[3][2], 2.0 * atan(1.0 / g_lastProj[0][0]) * 57.29578);
    g_uploads.clear();
    g_tracing = true;
}

} // namespace Probe
