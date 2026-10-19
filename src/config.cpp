#include "config.h"
#include "log.h"

#include <windows.h>
#include <cstdlib>

static Config g_config;

const Config& Cfg() { return g_config; }

static int ReadInt(const wchar_t* section, const wchar_t* key, int def, const std::wstring& path)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, path.c_str());
    if (!buf[0])
        return def;
    return (int)wcstol(buf, nullptr, 0); // accepts 0x.. hex
}

static std::wstring ReadString(const wchar_t* section, const wchar_t* key, const std::wstring& path)
{
    wchar_t buf[MAX_PATH * 2];
    GetPrivateProfileStringW(section, key, L"", buf, MAX_PATH * 2, path.c_str());
    return buf;
}

static float ReadFloat(const wchar_t* section, const wchar_t* key, float def, const std::wstring& path)
{
    std::wstring s = ReadString(section, key, path);
    return s.empty() ? def : (float)wcstod(s.c_str(), nullptr);
}

void LoadConfig()
{
    std::wstring path = GameDir() + L"rct3vr.ini";
    Config& c = g_config;
    c.runtimeJson = ReadString(L"VR", L"RuntimeJson", path);
    c.renderScale = ReadFloat(L"VR", L"RenderScale", c.renderScale, path);
    if (c.renderScale < 0.25f || c.renderScale > 4.0f)
        c.renderScale = 1.0f;
    c.toggleKey = ReadInt(L"VR", L"ToggleKey", c.toggleKey, path);
    c.recenterKey = ReadInt(L"VR", L"RecenterKey", c.recenterKey, path);
    c.worldScale = ReadFloat(L"VR", L"WorldScale", c.worldScale, path);
    c.msaa = ReadInt(L"VR", L"MSAA", c.msaa, path);
    c.previewWithoutHeadset = ReadInt(L"VR", L"PreviewWithoutHeadset", 0, path) != 0;
    c.autoEnable = ReadInt(L"VR", L"AutoEnable", 1, path) != 0;
    c.gameFov = ReadFloat(L"VR", L"GameFov", c.gameFov, path);
    if (c.gameFov < 0.0f || c.gameFov > 170.0f)
        c.gameFov = 140.0f;
    static const wchar_t* const buttonKeys[6] = {L"ButtonA", L"ButtonB", L"ButtonX", L"ButtonY", L"RightStick", L"LeftStick"};
    for (int i = 0; i < 6; ++i) {
        wchar_t buf[128];
        GetPrivateProfileStringW(L"Controller", buttonKeys[i], L"\x1", buf, 128, path.c_str());
        if (wcscmp(buf, L"\x1") != 0) // key present (possibly empty = unbound)
            c.buttons[i] = buf;
    }
    c.forceWindowed = ReadInt(L"Display", L"ForceWindowed", 1, path) != 0;
    c.captureKey = ReadInt(L"Debug", L"CaptureKey", c.captureKey, path);
    c.probeKey = ReadInt(L"Debug", L"ProbeKey", c.probeKey, path);
    c.autoCaptureFrame = ReadInt(L"Debug", L"AutoCaptureFrame", c.autoCaptureFrame, path);
    c.autoCaptureSeconds = ReadInt(L"Debug", L"AutoCaptureSeconds", c.autoCaptureSeconds, path);
    c.autoProbeSeconds = ReadInt(L"Debug", L"AutoProbeSeconds", c.autoProbeSeconds, path);
    c.exitAfterAutoCapture =ReadInt(L"Debug", L"ExitAfterAutoCapture", 0, path) != 0;
    c.maxSnapshots = ReadInt(L"Debug", L"MaxSnapshots", c.maxSnapshots, path);
    Log("config: captureKey=0x%x autoCaptureFrame=%d exitAfterAutoCapture=%d",
        c.captureKey, c.autoCaptureFrame, (int)c.exitAfterAutoCapture);
}
