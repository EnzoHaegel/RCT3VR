#pragma once
#include <string>

struct Config {
    // [VR]
    std::wstring runtimeJson; // OpenXR runtime manifest override (empty = auto-detect)
    float renderScale = 1.0f; // multiplier of the runtime's recommended eye resolution
    int toggleKey = 0x78;     // F9: VR on/off
    int recenterKey = 0x77;   // F8: recenter view
    float worldScale = 1.0f;  // game units per meter of head movement
    int msaa = 4;             // eye render target multisampling (0 = off)
    bool previewWithoutHeadset = false; // show side-by-side on the monitor when no headset is available
    bool autoEnable = true;   // VR only while a first-person ride camera is active
    float gameFov = 140.0f;   // horizontal culling FOV forced on the ride camera (0 = keep 90)

    // [Controller]: game keys (or "Recenter") sent when a controller button is pressed in VR
    std::wstring buttons[6] = {L"N", L"Ctrl+C", L"", L"", L"Recenter", L""}; // A, B, X, Y, right stick, left stick

    // [Display]
    bool forceWindowed = true; // borderless window instead of exclusive fullscreen

    // [Debug]
    int captureKey = 0;    // 0x79 = F10 (off by default)
    int probeKey = 0;      // 0x7A = F11: camera memory probe (off by default)
    int autoCaptureFrame = 0;
    int autoCaptureSeconds = 0;
    int autoProbeSeconds = 0;
    bool exitAfterAutoCapture = false;
    int maxSnapshots = 40;
};

const Config& Cfg();
void LoadConfig();
