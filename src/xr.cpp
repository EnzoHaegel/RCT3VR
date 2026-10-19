#include "xr.h"
#include "config.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

using Microsoft::WRL::ComPtr;

namespace Vr {
namespace {

#define XR_INSTANCE_FUNCTIONS(X)                                                            \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrPollEvent)      \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews)            \
    X(xrEnumerateEnvironmentBlendModes) X(xrGetD3D11GraphicsRequirementsKHR)                \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession)                \
    X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace)                            \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain)               \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)        \
    X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews)       \
    X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean)

struct Functions {
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
    PFN_xrCreateInstance xrCreateInstance;
#define DECLARE_FN(name) PFN_##name name;
    XR_INSTANCE_FUNCTIONS(DECLARE_FN)
#undef DECLARE_FN
};

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    bool everReleased = false;
    bool sizeWarned = false;
};

const XrPosef kIdentityPose = {{0, 0, 0, 1}, {0, 0, 0}};

HMODULE g_runtime; // kept loaded across Shutdown/Init: runtimes may leave threads behind
Functions xr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_baseSpace = XR_NULL_HANDLE;    // LOCAL, never recentered: reference for Recenter
XrSpace g_appSpace = XR_NULL_HANDLE;     // LOCAL with the recenter offset applied
XrSpace g_pendingSpace = XR_NULL_HANDLE; // replaces g_appSpace at the next BeginFrame
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
XrEnvironmentBlendMode g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
bool g_running = false;
bool g_teardown = false;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
bool g_swizzle = false; // swapchain is RGBA: swap B and R while uploading
UINT g_width = 0, g_height = 0;
Eye g_eyes[2];
bool g_frameBegun = false;
bool g_frameRender = false;
XrTime g_displayTime = 0;
XrView g_views[2];
bool g_haveViews = false;
std::vector<DWORD> g_scratch;
int g_errorLogBudget = 20;
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_buttonActions[kButtonCount] = {};
bool g_buttons[kButtonCount] = {};
ComPtr<ID3D11Texture2D> g_shared; // side-by-side eyes written by the game's D3D9 device
UINT g_sharedWidth = 0;

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string ResultName(XrResult r)
{
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g_instance && xr.xrResultToString && XR_SUCCEEDED(xr.xrResultToString(g_instance, r, buf)))
        return buf;
    return "XrResult " + std::to_string((int)r);
}

bool Failed(XrResult r, const char* what, std::string& error)
{
    if (XR_SUCCEEDED(r))
        return false;
    error = std::string(what) + " failed: " + ResultName(r);
    Log("xr: %s", error.c_str());
    return true;
}

// Logs per-frame failures without flooding the log.
void LogFrameError(const char* what, XrResult r)
{
    if (g_errorLogBudget > 0) {
        --g_errorLogBudget;
        Log("xr: %s failed: %s", what, ResultName(r).c_str());
    }
}

// ---------------------------------------------------------------------------
// Runtime discovery (no OpenXR loader: the runtime DLL is negotiated directly)

bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ReadActiveRuntime(REGSAM view)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | view, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG rc = RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(buf, expanded, MAX_PATH * 2))
            return expanded;
    }
    return buf;
}

std::vector<std::wstring> RuntimeManifestCandidates()
{
    std::vector<std::wstring> out;
    if (!Cfg().runtimeJson.empty())
        out.push_back(Cfg().runtimeJson);
    wchar_t env[MAX_PATH * 2];
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, MAX_PATH * 2))
        out.push_back(env);

    std::wstring active32 = ReadActiveRuntime(KEY_WOW64_32KEY);
    if (!active32.empty())
        out.push_back(active32);

    // Only the 64-bit runtime is usually registered: look for its 32-bit sibling
    // (VDXR: virtualdesktop-openxr-32.json, SteamVR: steamxr_win32.json,
    // Meta: oculus_openxr_64.json -> oculus_openxr_32.json).
    std::wstring active64 = ReadActiveRuntime(KEY_WOW64_64KEY);
    if (!active64.empty()) {
        std::wstring lower = active64;
        for (wchar_t& c : lower)
            c = (wchar_t)towlower(c);
        size_t win64 = lower.rfind(L"win64");
        if (win64 != std::wstring::npos)
            out.push_back(active64.substr(0, win64) + L"win32" + active64.substr(win64 + 5));
        size_t name = lower.find_last_of(L"\\/") + 1;
        size_t sixtyFour = lower.rfind(L"64");
        if (sixtyFour != std::wstring::npos && sixtyFour >= name && win64 == std::wstring::npos)
            out.push_back(active64.substr(0, sixtyFour) + L"32" + active64.substr(sixtyFour + 2));
        size_t ext = lower.rfind(L".json");
        if (ext != std::wstring::npos) {
            out.push_back(active64.substr(0, ext) + L"-32" + active64.substr(ext));
            out.push_back(active64.substr(0, ext) + L"_32" + active64.substr(ext));
        }
        out.push_back(active64); // only loads if it is in fact 32-bit
    }
    return out;
}

// Extracts runtime.library_path from a manifest and resolves it against the manifest folder.
std::wstring ManifestLibraryPath(const std::wstring& manifest)
{
    FILE* f = _wfopen(manifest.c_str(), L"rb");
    if (!f)
        return {};
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);

    size_t key = text.find("\"library_path\"");
    if (key == std::string::npos)
        return {};
    size_t colon = text.find(':', key);
    size_t open = colon == std::string::npos ? colon : text.find('"', colon);
    if (open == std::string::npos)
        return {};
    std::string value;
    for (size_t i = open + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size())
            ++i; // JSON escapes used in paths: \\ and \/
        value += text[i];
    }

    std::wstring lib = Wide(value);
    if (lib.find_first_of(L"\\/") == std::wstring::npos)
        return lib; // bare file name: system search path, per the loader spec
    bool absolute = (lib.size() > 1 && lib[1] == L':') || lib[0] == L'\\' || lib[0] == L'/';
    if (!absolute)
        lib = manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + lib;
    wchar_t full[MAX_PATH * 2];
    if (GetFullPathNameW(lib.c_str(), MAX_PATH * 2, full, nullptr))
        lib = full;
    return lib;
}

bool LoadRuntime(std::string& error)
{
    for (const std::wstring& manifest : RuntimeManifestCandidates()) {
        if (!FileExists(manifest)) {
            Log("xr: manifest %s not found", Utf8(manifest).c_str());
            continue;
        }
        std::wstring lib = ManifestLibraryPath(manifest);
        if (lib.empty()) {
            Log("xr: no library_path in %s", Utf8(manifest).c_str());
            continue;
        }
        Log("xr: trying %s -> %s", Utf8(manifest).c_str(), Utf8(lib).c_str());
        bool hasDir = lib.find_first_of(L"\\/") != std::wstring::npos;
        HMODULE module = LoadLibraryExW(lib.c_str(), nullptr, hasDir ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        if (!module) {
            DWORD err = GetLastError();
            Log("xr: cannot load runtime (error %lu%s)", err, err == ERROR_BAD_EXE_FORMAT ? ", not a 32-bit DLL" : "");
            continue;
        }
        auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface");
        if (!negotiate) {
            Log("xr: runtime exports no xrNegotiateLoaderRuntimeInterface");
            FreeLibrary(module);
            continue;
        }
        XrNegotiateLoaderInfo info = {};
        info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        info.structSize = sizeof(info);
        info.minInterfaceVersion = 1;
        info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest request = {};
        request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
        request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        XrResult r = negotiate(&info, &request);
        if (XR_FAILED(r) || !request.getInstanceProcAddr) {
            Log("xr: runtime negotiation failed (%d)", (int)r);
            FreeLibrary(module);
            continue;
        }
        Log("xr: runtime loaded, interface %u, api %u.%u.%u", request.runtimeInterfaceVersion,
            (unsigned)XR_VERSION_MAJOR(request.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(request.runtimeApiVersion),
            (unsigned)XR_VERSION_PATCH(request.runtimeApiVersion));
        g_runtime = module;
        xr.xrGetInstanceProcAddr = request.getInstanceProcAddr;
        return true;
    }
    error = "no usable 32-bit OpenXR runtime found (set [VR] RuntimeJson in rct3vr.ini)";
    Log("xr: %s", error.c_str());
    return false;
}

template <typename T>
bool Resolve(XrInstance instance, const char* name, T& fn, std::string& error)
{
    fn = nullptr;
    XrResult r = xr.xrGetInstanceProcAddr(instance, name, (PFN_xrVoidFunction*)&fn);
    if (XR_FAILED(r) || !fn) {
        error = std::string("runtime lacks ") + name;
        Log("xr: %s", error.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Setup

bool CreateD3D11Device(const XrGraphicsRequirementsD3D11KHR& req, std::string& error)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "CreateDXGIFactory1 failed";
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc;
        candidate->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
            Log("xr: using adapter %s", Utf8(desc.Description).c_str());
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        error = "the headset's GPU adapter was not found";
        Log("xr: %s", error.c_str());
        return false;
    }

    const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT count = 0;
    while (count < ARRAYSIZE(all) && all[count] >= req.minFeatureLevel)
        ++count;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, all, count,
        D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (FAILED(hr)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "D3D11CreateDevice failed: 0x%08lx", (unsigned long)hr);
        error = buf;
        Log("xr: %s", error.c_str());
        return false;
    }
    return true;
}

bool CreateSwapchains(std::string& error)
{
    uint32_t count = 0;
    if (Failed(xr.xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats", error))
        return false;
    std::vector<int64_t> formats(count);
    if (Failed(xr.xrEnumerateSwapchainFormats(g_session, count, &count, formats.data()), "xrEnumerateSwapchainFormats", error))
        return false;
    if (formats.empty()) {
        error = "runtime offers no swapchain format";
        return false;
    }

    int64_t format = 0;
    for (int64_t preferred : {(int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
        for (int64_t f : formats) {
            if (f == preferred && !format)
                format = f;
        }
    }
    if (!format) {
        format = formats[0];
        Log("xr: WARNING: no 8-bit sRGB swapchain format offered, using DXGI format %lld (colors may be off)", format);
    }
    g_swizzle = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Log("xr: swapchain format %lld%s, %ux%u per eye", format, g_swizzle ? " (BGRA->RGBA swizzle)" : "", g_width, g_height);

    for (Eye& eye : g_eyes) {
        XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = g_width;
        info.height = g_height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (Failed(xr.xrCreateSwapchain(g_session, &info, &eye.swapchain), "xrCreateSwapchain", error))
            return false;
        uint32_t images = 0;
        if (Failed(xr.xrEnumerateSwapchainImages(eye.swapchain, 0, &images, nullptr), "xrEnumerateSwapchainImages", error))
            return false;
        eye.images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (Failed(xr.xrEnumerateSwapchainImages(eye.swapchain, images, &images, (XrSwapchainImageBaseHeader*)eye.images.data()),
                "xrEnumerateSwapchainImages", error))
            return false;
    }
    return true;
}

bool CreateReferenceSpace(XrReferenceSpaceType type, const XrPosef& pose, XrSpace& out, std::string& error)
{
    XrReferenceSpaceCreateInfo info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = type;
    info.poseInReferenceSpace = pose;
    return !Failed(xr.xrCreateReferenceSpace(g_session, &info, &out), "xrCreateReferenceSpace", error);
}

// Controller buttons as boolean actions, bound for Touch (Quest) and Index controllers.
bool CreateActions(std::string& error)
{
    XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "rct3vr");
    strcpy_s(setInfo.localizedActionSetName, "RCT3 VR");
    if (Failed(xr.xrCreateActionSet(g_instance, &setInfo, &g_actionSet), "xrCreateActionSet", error))
        return false;

    static const char* const names[kButtonCount] = {"button_a", "button_b", "button_x", "button_y", "right_stick", "left_stick"};
    for (int i = 0; i < kButtonCount; ++i) {
        XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy_s(info.actionName, names[i]);
        strcpy_s(info.localizedActionName, names[i]);
        if (Failed(xr.xrCreateAction(g_actionSet, &info, &g_buttonActions[i]), "xrCreateAction", error))
            return false;
    }

    // Per profile, the input bound to each VrButton (null = none on that controller).
    // Controllers without A/B buttons map their trackpad/select click to A and menu to B.
    struct Profile {
        const char* path;
        const char* buttons[kButtonCount];
    };
    static const Profile profiles[] = {
        {"/interaction_profiles/oculus/touch_controller",
            {"/user/hand/right/input/a/click", "/user/hand/right/input/b/click", "/user/hand/left/input/x/click",
                "/user/hand/left/input/y/click", "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/thumbstick/click"}},
        {"/interaction_profiles/valve/index_controller",
            {"/user/hand/right/input/a/click", "/user/hand/right/input/b/click", "/user/hand/left/input/a/click",
                "/user/hand/left/input/b/click", "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/thumbstick/click"}},
        {"/interaction_profiles/htc/vive_controller",
            {"/user/hand/right/input/trackpad/click", "/user/hand/right/input/menu/click", "/user/hand/left/input/trackpad/click",
                "/user/hand/left/input/menu/click", nullptr, nullptr}},
        {"/interaction_profiles/microsoft/motion_controller",
            {"/user/hand/right/input/trackpad/click", "/user/hand/right/input/menu/click", "/user/hand/left/input/trackpad/click",
                "/user/hand/left/input/menu/click", "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/thumbstick/click"}},
        {"/interaction_profiles/khr/simple_controller",
            {"/user/hand/right/input/select/click", "/user/hand/right/input/menu/click", "/user/hand/left/input/select/click",
                "/user/hand/left/input/menu/click", nullptr, nullptr}},
    };
    for (const Profile& profile : profiles) {
        XrActionSuggestedBinding bindings[kButtonCount];
        uint32_t count = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            if (!profile.buttons[i])
                continue;
            bindings[count].action = g_buttonActions[i];
            xr.xrStringToPath(g_instance, profile.buttons[i], &bindings[count].binding);
            ++count;
        }
        XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        xr.xrStringToPath(g_instance, profile.path, &suggested.interactionProfile);
        suggested.countSuggestedBindings = count;
        suggested.suggestedBindings = bindings;
        XrResult r = xr.xrSuggestInteractionProfileBindings(g_instance, &suggested);
        if (XR_FAILED(r))
            Log("xr: bindings for %s rejected: %s", profile.path, ResultName(r).c_str());
    }

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    return !Failed(xr.xrAttachSessionActionSets(g_session, &attach), "xrAttachSessionActionSets", error);
}

void SyncButtons()
{
    for (bool& b : g_buttons)
        b = false;
    if (g_state != XR_SESSION_STATE_FOCUSED || !g_actionSet)
        return;
    XrActiveActionSet active = {g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    XrResult r = xr.xrSyncActions(g_session, &sync);
    if (XR_FAILED(r)) {
        LogFrameError("xrSyncActions", r);
        return;
    }
    for (int i = 0; i < kButtonCount; ++i) {
        XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = g_buttonActions[i];
        XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xr.xrGetActionStateBoolean(g_session, &info, &state)))
            g_buttons[i] = state.isActive && state.currentState;
    }
}

const char* StateName(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

void PollEvents()
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xr.xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            g_state = ((XrEventDataSessionStateChanged&)event).state;
            Log("xr: session state %s", StateName(g_state));
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo info = {XR_TYPE_SESSION_BEGIN_INFO};
                info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xr.xrBeginSession(g_session, &info);
                g_running = XR_SUCCEEDED(r);
                if (!g_running)
                    Log("xr: xrBeginSession failed: %s", ResultName(r).c_str());
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xr.xrEndSession(g_session);
                g_running = false;
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_teardown = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            Log("xr: instance loss pending");
            g_teardown = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            Log("xr: reference space change pending");
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

} // namespace

bool Init(std::string& error)
{
    if (g_instance)
        return true;
    error.clear();
    g_teardown = false;
    g_errorLogBudget = 20;
    if (!g_runtime && !LoadRuntime(error))
        return false;

    if (!Resolve(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xr.xrEnumerateInstanceExtensionProperties, error)
        || !Resolve(XR_NULL_HANDLE, "xrCreateInstance", xr.xrCreateInstance, error))
        return false;

    uint32_t count = 0;
    if (Failed(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties", error))
        return false;
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (Failed(xr.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties", error))
        return false;
    bool hasD3D11 = false;
    for (const XrExtensionProperties& e : extensions)
        hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!hasD3D11) {
        error = "the OpenXR runtime does not support Direct3D 11";
        Log("xr: %s", error.c_str());
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "RCT3VR");
    strcpy_s(createInfo.applicationInfo.engineName, "RCT3");
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabled;
    if (Failed(xr.xrCreateInstance(&createInfo, &g_instance), "xrCreateInstance", error))
        return false;

#define RESOLVE_FN(name) if (!Resolve(g_instance, #name, xr.name, error)) { Shutdown(); return false; }
    XR_INSTANCE_FUNCTIONS(RESOLVE_FN)
#undef RESOLVE_FN

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xr.xrGetInstanceProperties(g_instance, &props)))
        Log("xr: runtime \"%s\" %u.%u.%u", props.runtimeName, (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
            (unsigned)XR_VERSION_MINOR(props.runtimeVersion), (unsigned)XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xr.xrGetSystem(g_instance, &systemInfo, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        error = "no headset available (XR_ERROR_FORM_FACTOR_UNAVAILABLE): connect the headset and start Virtual Desktop streaming";
        Log("xr: %s", error.c_str());
        Shutdown();
        return false;
    }
    if (Failed(r, "xrGetSystem", error)) {
        Shutdown();
        return false;
    }

    XrSystemProperties systemProps = {XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xr.xrGetSystemProperties(g_instance, g_system, &systemProps)))
        Log("xr: system \"%s\", max swapchain %ux%u", systemProps.systemName, systemProps.graphicsProperties.maxSwapchainImageWidth,
            systemProps.graphicsProperties.maxSwapchainImageHeight);

    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (Failed(xr.xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views),
            "xrEnumerateViewConfigurationViews", error)) {
        Shutdown();
        return false;
    }
    g_width = (UINT)(views[0].recommendedImageRectWidth * Cfg().renderScale + 0.5f);
    g_height = (UINT)(views[0].recommendedImageRectHeight * Cfg().renderScale + 0.5f);
    if (g_width > views[0].maxImageRectWidth)
        g_width = views[0].maxImageRectWidth;
    if (g_height > views[0].maxImageRectHeight)
        g_height = views[0].maxImageRectHeight;
    Log("xr: recommended eye size %ux%u, using %ux%u (RenderScale %.2f)", views[0].recommendedImageRectWidth,
        views[0].recommendedImageRectHeight, g_width, g_height, Cfg().renderScale);

    XrEnvironmentBlendMode blendModes[8];
    if (XR_SUCCEEDED(xr.xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &count, blendModes))
        && count > 0)
        g_blendMode = blendModes[0];

    XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (Failed(xr.xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &requirements), "xrGetD3D11GraphicsRequirementsKHR", error)
        || !CreateD3D11Device(requirements, error)) {
        Shutdown();
        return false;
    }

    XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_device.Get();
    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    if (Failed(xr.xrCreateSession(g_instance, &sessionInfo, &g_session), "xrCreateSession", error)
        || !CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, kIdentityPose, g_baseSpace, error)
        || !CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, kIdentityPose, g_appSpace, error)
        || !CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, kIdentityPose, g_viewSpace, error)
        || !CreateSwapchains(error)) {
        Shutdown();
        return false;
    }
    if (!CreateActions(error))
        Log("xr: controller buttons unavailable (%s)", error.c_str()); // rendering still works

    Log("xr: initialized");
    return true;
}

void Shutdown()
{
    if (!g_instance)
        return;
    for (Eye& eye : g_eyes) {
        if (eye.swapchain)
            xr.xrDestroySwapchain(eye.swapchain);
        eye = Eye();
    }
    for (XrSpace* space : {&g_pendingSpace, &g_appSpace, &g_baseSpace, &g_viewSpace}) {
        if (*space)
            xr.xrDestroySpace(*space);
        *space = XR_NULL_HANDLE;
    }
    if (g_session)
        xr.xrDestroySession(g_session);
    if (xr.xrDestroyInstance)
        xr.xrDestroyInstance(g_instance); // also destroys the action set and actions
    g_actionSet = XR_NULL_HANDLE;
    for (int i = 0; i < kButtonCount; ++i) {
        g_buttonActions[i] = XR_NULL_HANDLE;
        g_buttons[i] = false;
    }
    g_shared.Reset();
    g_sharedWidth = 0;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_running = g_teardown = g_frameBegun = g_frameRender = g_haveViews = false;
    g_displayTime = 0;
    g_context.Reset();
    g_device.Reset();
    Log("xr: shut down");
}

bool IsInitialized() { return g_instance != XR_NULL_HANDLE; }
bool IsSessionRunning() { return g_running; }
UINT EyeWidth() { return g_width; }
UINT EyeHeight() { return g_height; }

bool BeginFrame(VrEyePose out[2])
{
    if (!g_instance)
        return false;
    if (g_frameBegun)
        EndFrame(); // the previous frame was never ended

    PollEvents();
    if (g_teardown) {
        Log("xr: session exiting or lost, shutting down (Init again to retry)");
        Shutdown();
        return false;
    }
    if (!g_running)
        return false;

    if (g_pendingSpace) {
        xr.xrDestroySpace(g_appSpace);
        g_appSpace = g_pendingSpace;
        g_pendingSpace = XR_NULL_HANDLE;
    }

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    XrResult r = xr.xrWaitFrame(g_session, &waitInfo, &frameState);
    if (XR_FAILED(r)) {
        LogFrameError("xrWaitFrame", r);
        return false;
    }
    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    r = xr.xrBeginFrame(g_session, &beginInfo);
    if (XR_FAILED(r)) {
        LogFrameError("xrBeginFrame", r);
        return false;
    }
    g_frameBegun = true;
    g_frameRender = frameState.shouldRender == XR_TRUE;
    g_displayTime = frameState.predictedDisplayTime;
    SyncButtons();
    if (!g_frameRender) {
        EndFrame();
        return false;
    }

    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = g_displayTime;
    locateInfo.space = g_appSpace;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    r = xr.xrLocateViews(g_session, &locateInfo, &viewState, 2, &count, views);
    if (XR_FAILED(r) || count != 2) {
        LogFrameError("xrLocateViews", r);
        EndFrame();
        return false;
    }
    if (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) {
        g_views[0] = views[0];
        g_views[1] = views[1];
        g_haveViews = true;
    } else if (!g_haveViews) {
        // No tracking yet: keep the field of view, look straight ahead.
        for (int i = 0; i < 2; ++i) {
            g_views[i] = views[i];
            g_views[i].pose = kIdentityPose;
        }
    } else {
        g_views[0].fov = views[0].fov; // hold the last tracked pose
        g_views[1].fov = views[1].fov;
    }

    for (int i = 0; i < 2; ++i) {
        const XrPosef& p = g_views[i].pose;
        const XrFovf& f = g_views[i].fov;
        out[i].pos[0] = p.position.x;
        out[i].pos[1] = p.position.y;
        out[i].pos[2] = p.position.z;
        out[i].orient[0] = p.orientation.x;
        out[i].orient[1] = p.orientation.y;
        out[i].orient[2] = p.orientation.z;
        out[i].orient[3] = p.orientation.w;
        out[i].tanLeft = tanf(f.angleLeft);
        out[i].tanRight = tanf(f.angleRight);
        out[i].tanUp = tanf(f.angleUp);
        out[i].tanDown = tanf(f.angleDown);
    }
    return true;
}

void SubmitEyeBGRA(int eye, const void* pixels, UINT pitch, UINT width, UINT height)
{
    if (!g_frameBegun || !g_frameRender || eye < 0 || eye > 1 || !pixels)
        return;
    Eye& e = g_eyes[eye];

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = xr.xrAcquireSwapchainImage(e.swapchain, &acquireInfo, &index);
    if (XR_FAILED(r)) {
        LogFrameError("xrAcquireSwapchainImage", r);
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    r = xr.xrWaitSwapchainImage(e.swapchain, &waitInfo);
    if (XR_FAILED(r)) {
        LogFrameError("xrWaitSwapchainImage", r);
        return;
    }

    UINT w = width < g_width ? width : g_width;
    UINT h = height < g_height ? height : g_height;
    if ((width != g_width || height != g_height) && !e.sizeWarned) {
        Log("xr: eye %d image is %ux%u but the swapchain is %ux%u, copying %ux%u", eye, width, height, g_width, g_height, w, h);
        e.sizeWarned = true;
    }

    const void* source = pixels;
    UINT sourcePitch = pitch;
    if (g_swizzle) {
        g_scratch.resize((size_t)w * h);
        for (UINT y = 0; y < h; ++y) {
            const DWORD* row = (const DWORD*)((const BYTE*)pixels + (size_t)y * pitch);
            DWORD* dst = &g_scratch[(size_t)y * w];
            for (UINT x = 0; x < w; ++x) {
                DWORD c = row[x];
                dst[x] = (c & 0xFF00FF00) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
            }
        }
        source = g_scratch.data();
        sourcePitch = w * 4;
    }
    D3D11_BOX box = {0, 0, 0, w, h, 1};
    g_context->UpdateSubresource(e.images[index].texture, 0, &box, source, sourcePitch, 0);

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r = xr.xrReleaseSwapchainImage(e.swapchain, &releaseInfo);
    if (XR_FAILED(r))
        LogFrameError("xrReleaseSwapchainImage", r);
    else
        e.everReleased = true;
}

HANDLE CreateSharedEyeTexture()
{
    if (!g_device || g_swizzle)
        return nullptr; // RGBA swapchains would need a shader pass: callers fall back to CPU uploads
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = g_width * 2;
    desc.Height = g_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    g_shared.Reset();
    ComPtr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_shared)) || FAILED(g_shared.As(&resource))
        || FAILED(resource->GetSharedHandle(&handle))) {
        Log("xr: shared eye texture unavailable");
        g_shared.Reset();
        return nullptr;
    }
    g_sharedWidth = g_width;
    return handle;
}

void SubmitSharedTexture()
{
    if (!g_frameBegun || !g_frameRender || !g_shared)
        return;
    for (int eye = 0; eye < 2; ++eye) {
        Eye& e = g_eyes[eye];
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult r = xr.xrAcquireSwapchainImage(e.swapchain, &acquireInfo, &index);
        if (XR_FAILED(r)) {
            LogFrameError("xrAcquireSwapchainImage", r);
            return;
        }
        XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        r = xr.xrWaitSwapchainImage(e.swapchain, &waitInfo);
        if (XR_FAILED(r)) {
            LogFrameError("xrWaitSwapchainImage", r);
            return;
        }
        // BGRA UNORM -> BGRA UNORM_SRGB: same format family, copied as raw bytes.
        D3D11_BOX box = {eye * g_sharedWidth, 0, 0, (eye + 1) * g_sharedWidth, g_height, 1};
        g_context->CopySubresourceRegion(e.images[index].texture, 0, 0, 0, 0, g_shared.Get(), 0, &box);
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        r = xr.xrReleaseSwapchainImage(e.swapchain, &releaseInfo);
        if (XR_FAILED(r))
            LogFrameError("xrReleaseSwapchainImage", r);
        else
            e.everReleased = true;
    }
}

bool ButtonDown(VrButton button)
{
    return button >= 0 && button < kButtonCount && g_buttons[button];
}

void EndFrame()
{
    if (!g_frameBegun)
        return;
    g_frameBegun = false;

    XrCompositionLayerProjectionView projViews[2];
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_displayTime;
    endInfo.environmentBlendMode = g_blendMode;

    // A swapchain may only be referenced once an image has been released; until
    // then (or when the frame must not be rendered) submit no layer at all.
    if (g_frameRender && g_eyes[0].everReleased && g_eyes[1].everReleased) {
        for (int i = 0; i < 2; ++i) {
            projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projViews[i].pose = g_views[i].pose;
            projViews[i].fov = g_views[i].fov;
            projViews[i].subImage.swapchain = g_eyes[i].swapchain;
            projViews[i].subImage.imageRect = {{0, 0}, {(int32_t)g_width, (int32_t)g_height}};
            projViews[i].subImage.imageArrayIndex = 0;
        }
        layer.space = g_appSpace;
        layer.viewCount = 2;
        layer.views = projViews;
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    }
    XrResult r = xr.xrEndFrame(g_session, &endInfo);
    if (XR_FAILED(r))
        LogFrameError("xrEndFrame", r);
}

void Recenter()
{
    if (!g_session || !g_displayTime) {
        Log("xr: recenter ignored, no frame yet");
        return;
    }
    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    XrResult r = xr.xrLocateSpace(g_viewSpace, g_baseSpace, g_displayTime, &location);
    if (XR_FAILED(r) || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        Log("xr: recenter failed, head not tracked");
        return;
    }
    // Keep only the yaw of the head so the horizon stays level.
    const XrQuaternionf& q = location.pose.orientation;
    float yaw = atan2f(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    XrPosef origin = kIdentityPose;
    origin.orientation = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        origin.position = location.pose.position;

    std::string error;
    XrSpace space = XR_NULL_HANDLE;
    if (!CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, origin, space, error))
        return;
    if (g_pendingSpace)
        xr.xrDestroySpace(g_pendingSpace);
    g_pendingSpace = space; // swapped in at the next BeginFrame, so this frame's poses stay consistent
    Log("xr: recentered (yaw %.1f deg, head at %.2f %.2f %.2f)", yaw * 57.2958f, origin.position.x, origin.position.y,
        origin.position.z);
}

} // namespace Vr
