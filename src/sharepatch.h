#pragma once
#include <windows.h>

// d3d9.dll only lets IDirect3DDevice9Ex create or open shared resources. In the
// shared-handle path of resource creation it tests the device mode:
//     cmp dword ptr [eax+X], 4 ; je allowed ; cmp dword ptr [eax+Y], 0 ; jge fail ;
//     mov eax, [eax+Z] ; cmp dword ptr [eax+W], 0 ; jne allowed
// Turning that je into jmp lets the game's plain device share textures with the
// OpenXR (D3D11) device, the same trick OBS uses for game capture. The patch is in
// memory only; the structure offsets are wildcards so other Windows builds match.
// Returns the number of sites changed (0 = pattern not found).
inline int PatchD3d9SharedResourceCheck(HMODULE d3d9, bool enable)
{
    // -1 = any byte
    static const short pattern[] = {0x83, 0xB8, -1, -1, 0x00, 0x00, 0x04, 0x74, -1, 0x83, 0xB8, -1, -1, 0x00, 0x00, 0x00, 0x7D, -1,
        0x8B, 0x80, -1, -1, 0x00, 0x00, 0x83, 0xB8, -1, -1, 0x00, 0x00, 0x00, 0x75};
    const size_t kJcc = 7; // the je to turn into jmp
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)d3d9;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)d3d9 + dos->e_lfanew);
    BYTE* base = (BYTE*)d3d9;
    DWORD size = nt->OptionalHeader.SizeOfImage;
    const size_t n = sizeof(pattern) / sizeof(pattern[0]);
    int changed = 0;
    for (DWORD i = 0; i + n <= size; ++i) {
        BYTE* p = base + i;
        bool match = true;
        for (size_t k = 0; k < n && match; ++k) {
            if (k == kJcc)
                match = p[k] == 0x74 || p[k] == 0xEB;
            else
                match = pattern[k] < 0 || p[k] == pattern[k];
        }
        if (!match)
            continue;
        BYTE want = enable ? 0xEB : 0x74;
        if (p[kJcc] == want)
            continue;
        DWORD old;
        if (VirtualProtect(p + kJcc, 1, PAGE_EXECUTE_READWRITE, &old)) {
            p[kJcc] = want;
            VirtualProtect(p + kJcc, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p + kJcc, 1);
            ++changed;
        }
    }
    return changed;
}
