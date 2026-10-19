#include "keys.h"
#include "log.h"

#include <cwctype>

namespace {

struct Named {
    const wchar_t* name;
    BYTE dik;
};

const Named kNames[] = {
    {L"ctrl", 0x1D}, {L"control", 0x1D}, {L"lctrl", 0x1D}, {L"rctrl", 0x9D}, {L"shift", 0x2A}, {L"lshift", 0x2A},
    {L"rshift", 0x36}, {L"alt", 0x38}, {L"lalt", 0x38}, {L"space", 0x39}, {L"enter", 0x1C}, {L"return", 0x1C},
    {L"esc", 0x01}, {L"escape", 0x01}, {L"tab", 0x0F}, {L"backspace", 0x0E}, {L"up", 0xC8}, {L"down", 0xD0},
    {L"left", 0xCB}, {L"right", 0xCD}, {L"pgup", 0xC9}, {L"pageup", 0xC9}, {L"pgdn", 0xD1}, {L"pagedown", 0xD1},
    {L"home", 0xC7}, {L"end", 0xCF}, {L"ins", 0xD2}, {L"insert", 0xD2}, {L"del", 0xD3}, {L"delete", 0xD3},
    {L"num+", 0x4E}, {L"num-", 0x4A}, {L"f11", 0x57}, {L"f12", 0x58},
};

// DIK codes follow the US keyboard layout positions.
const wchar_t kRowQ[] = L"qwertyuiop";
const wchar_t kRowA[] = L"asdfghjkl";
const wchar_t kRowZ[] = L"zxcvbnm";

bool ParseKey(std::wstring name, BYTE& dik)
{
    for (wchar_t& c : name)
        c = (wchar_t)towlower(c);
    for (const Named& n : kNames) {
        if (name == n.name) {
            dik = n.dik;
            return true;
        }
    }
    if (name.size() == 1) {
        wchar_t c = name[0];
        if (const wchar_t* p = wcschr(kRowQ, c)) {
            dik = (BYTE)(0x10 + (p - kRowQ));
            return true;
        }
        if (const wchar_t* p = wcschr(kRowA, c)) {
            dik = (BYTE)(0x1E + (p - kRowA));
            return true;
        }
        if (const wchar_t* p = wcschr(kRowZ, c)) {
            dik = (BYTE)(0x2C + (p - kRowZ));
            return true;
        }
        if (c >= L'1' && c <= L'9') {
            dik = (BYTE)(0x02 + (c - L'1'));
            return true;
        }
        if (c == L'0') {
            dik = 0x0B;
            return true;
        }
    }
    if (name.size() >= 2 && name[0] == L'f') {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 10) {
            dik = (BYTE)(0x3B + n - 1);
            return true;
        }
    }
    if (name.size() > 2 && name[0] == L'0' && name[1] == L'x') {
        dik = (BYTE)wcstol(name.c_str(), nullptr, 16);
        return dik != 0;
    }
    return false;
}

struct Held {
    std::vector<BYTE> keys;
    int frames = 0;
    DWORD since = 0;
};
std::vector<Held> g_held;

void Send(const std::vector<BYTE>& keys, bool up)
{
    std::vector<INPUT> inputs;
    for (size_t i = 0; i < keys.size(); ++i) {
        BYTE dik = up ? keys[keys.size() - 1 - i] : keys[i]; // release in reverse order
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = dik & 0x7F;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (dik & 0x80 ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        inputs.push_back(in);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

} // namespace

bool ParseKeyCombo(const std::wstring& text, KeyCombo& out)
{
    out = KeyCombo();
    std::wstring trimmed;
    for (wchar_t c : text)
        if (!iswspace(c))
            trimmed += c;
    if (trimmed.empty())
        return true;
    if (_wcsicmp(trimmed.c_str(), L"recenter") == 0) {
        out.recenter = true;
        return true;
    }
    size_t start = 0;
    while (start <= trimmed.size()) {
        // '+' separates keys, except in "Num+"
        size_t plus = trimmed.find(L'+', start);
        if (plus != std::wstring::npos && plus > start && _wcsnicmp(trimmed.c_str() + start, L"num", plus - start) == 0)
            plus = trimmed.find(L'+', plus + 1);
        std::wstring part = trimmed.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
        BYTE dik = 0;
        if (!ParseKey(part, dik))
            return false;
        out.keys.push_back(dik);
        if (plus == std::wstring::npos)
            break;
        start = plus + 1;
    }
    return true;
}

namespace GameKeys {

void Press(const KeyCombo& combo)
{
    if (combo.keys.empty())
        return;
    Send(combo.keys, false);
    g_held.push_back({combo.keys, 0, GetTickCount()});
}

void Update()
{
    for (size_t i = 0; i < g_held.size();) {
        Held& h = g_held[i];
        if (++h.frames >= 3 && GetTickCount() - h.since >= 60) {
            Send(h.keys, true);
            g_held.erase(g_held.begin() + i);
        } else {
            ++i;
        }
    }
}

} // namespace GameKeys
