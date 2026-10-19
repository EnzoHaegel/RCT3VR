// Standalone check of the OpenXR output: shows a stereo test pattern in the
// headset for 60 seconds (Ctrl+C to stop early). Log: rct3vr.log next to the exe.
#include <windows.h>
#include <cstdio>
#include <vector>

#include "config.h"
#include "log.h"
#include "xr.h"

namespace {

volatile bool g_quit = false;

BOOL WINAPI OnConsoleCtrl(DWORD)
{
    g_quit = true;
    return TRUE;
}

const char* kGlyphL[7] = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
const char* kGlyphR[7] = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};

void FillRect(std::vector<DWORD>& img, UINT w, UINT h, int x0, int y0, int x1, int y1, DWORD color)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < (int)h; ++y)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < (int)w; ++x)
            img[(size_t)y * w + x] = color;
}

std::vector<DWORD> BuildPattern(int eye, const VrEyePose& pose, UINT w, UINT h)
{
    std::vector<DWORD> img((size_t)w * h, 0xFF202020);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            if (x % 64 == 0 || y % 64 == 0)
                img[(size_t)y * w + x] = 0xFF505050;

    // Eye marker: colored block with a big L or R.
    DWORD color = eye == 0 ? 0xFFC03030 : 0xFF3060C0;
    int cell = (int)h / 40;
    int bx = (int)w / 2 - 4 * cell, by = (int)h / 4;
    FillRect(img, w, h, bx, by, bx + 9 * cell, by + 11 * cell, color);
    const char** glyph = eye == 0 ? kGlyphL : kGlyphR;
    for (int gy = 0; gy < 7; ++gy)
        for (int gx = 0; gx < 5; ++gx)
            if (glyph[gy][gx] == '1')
                FillRect(img, w, h, bx + (2 + gx) * cell, by + (2 + gy) * cell, bx + (3 + gx) * cell, by + (3 + gy) * cell, 0xFFFFFFFF);

    // Lines through the projection center (where the eye looks straight ahead).
    int cx = (int)(-pose.tanLeft / (pose.tanRight - pose.tanLeft) * w);
    int cy = (int)(pose.tanUp / (pose.tanUp - pose.tanDown) * h);
    FillRect(img, w, h, 0, cy - 2, (int)w, cy + 2, 0xFFFFD000);
    FillRect(img, w, h, cx - 2, 0, cx + 2, (int)h, 0xFF00D0D0);
    return img;
}

} // namespace

int main()
{
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    LoadConfig();

    std::string error;
    if (!Vr::Init(error)) {
        printf("OpenXR init failed: %s\n", error.c_str());
        return 1;
    }
    UINT w = Vr::EyeWidth(), h = Vr::EyeHeight();
    printf("OpenXR ready, %ux%u per eye. Showing the test pattern for 60 s (Ctrl+C to stop).\n", w, h);

    std::vector<DWORD> images[2];
    DWORD start = GetTickCount(), lastPrint = start;
    int frames = 0;
    bool recentered = false;
    while (!g_quit && GetTickCount() - start < 60000) {
        if (!Vr::IsInitialized()) {
            printf("OpenXR session ended.\n");
            break;
        }
        VrEyePose poses[2];
        if (!Vr::BeginFrame(poses)) {
            if (!Vr::IsSessionRunning())
                Sleep(10);
            continue;
        }
        if (images[0].empty()) {
            for (int eye = 0; eye < 2; ++eye)
                images[eye] = BuildPattern(eye, poses[eye], w, h);
        }
        for (int eye = 0; eye < 2; ++eye)
            Vr::SubmitEyeBGRA(eye, images[eye].data(), w * 4, w, h);
        Vr::EndFrame();
        ++frames;

        DWORD now = GetTickCount();
        if (!recentered && now - start > 5000) {
            Vr::Recenter();
            recentered = true;
        }
        if (now - lastPrint >= 1000) {
            const VrEyePose& p = poses[0];
            printf("%5.1f fps | left eye pos %+.3f %+.3f %+.3f quat %+.3f %+.3f %+.3f %+.3f | fov tan L%.2f R%.2f U%.2f D%.2f\n",
                frames * 1000.0 / (now - lastPrint), p.pos[0], p.pos[1], p.pos[2], p.orient[0], p.orient[1], p.orient[2], p.orient[3],
                p.tanLeft, p.tanRight, p.tanUp, p.tanDown);
            frames = 0;
            lastPrint = now;
        }
    }
    Vr::Shutdown();
    printf("Done.\n");
    return 0;
}
