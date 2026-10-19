#pragma once
#include <windows.h>
#include <string>

// Pose of one eye in the (recenterable) LOCAL space plus its field of view as
// tangents of the XrFovf angles (tanLeft and tanDown are negative).
struct VrEyePose {
    float pos[3];
    float orient[4]; // x, y, z, w
    float tanLeft, tanRight, tanUp, tanDown;
};

enum VrButton {
    kButtonA,          // right controller, lower button
    kButtonB,          // right controller, upper button
    kButtonX,          // left controller, lower button
    kButtonY,          // left controller, upper button
    kButtonRightStick, // right thumbstick click
    kButtonLeftStick,  // left thumbstick click
    kButtonCount,
};

// In-process OpenXR output (D3D11 swapchains fed from CPU pixels or a shared texture). The runtime
// DLL is loaded directly, without the OpenXR loader, so a 32-bit runtime works
// even when only the 64-bit one is registered. All calls on one thread.
namespace Vr {

bool Init(std::string& error);
void Shutdown();
bool IsInitialized();
bool IsSessionRunning();
UINT EyeWidth();
UINT EyeHeight();

// Once per game frame: polls events, waits for and begins the next frame, and
// locates the eyes at the predicted display time. Returns false when nothing
// should be rendered or submitted this frame (any begun frame is then ended).
bool BeginFrame(VrEyePose out[2]);
// Copies BGRA pixels into the eye's swapchain; only the overlapping top-left
// region is copied when sizes differ.
void SubmitEyeBGRA(int eye, const void* pixels, UINT pitch, UINT width, UINT height);
// GPU path: a (2 x EyeWidth) x EyeHeight BGRA texture, left eye on the left, that the
// game's D3D9 device opens through the returned handle. Null when unsupported.
HANDLE CreateSharedEyeTexture();
// Copies both halves of the shared texture into the eye swapchains. The caller must
// make sure the D3D9 writes have completed (event query) before calling.
void SubmitSharedTexture();
// Controller button state, refreshed by BeginFrame (false unless the session has focus).
bool ButtonDown(VrButton button);
// Submits a projection layer with the poses returned by BeginFrame.
void EndFrame();
// Makes the current head position and yaw the origin of the LOCAL space.
void Recenter();

} // namespace Vr
