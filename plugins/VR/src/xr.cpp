// See xr.h.
#include "xr.h"

#include "mod.h"

#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vr
{
    namespace
    {
        // ---- the loader's entry points ------------------------------------------------------------------------------
        HMODULE g_loader = nullptr;
        PFN_xrGetInstanceProcAddr pGetInstanceProcAddr = nullptr;
#define VR_XR_FUNCTIONS(X)                                                                                                  \
    X(xrDestroyInstance)                                                                                                    \
    X(xrGetInstanceProperties)                                                                                              \
    X(xrGetSystem)                                                                                                          \
    X(xrGetSystemProperties)                                                                                                \
    X(xrEnumerateViewConfigurationViews)                                                                                    \
    X(xrEnumerateEnvironmentBlendModes)                                                                                     \
    X(xrGetD3D11GraphicsRequirementsKHR)                                                                                    \
    X(xrCreateSession)                                                                                                      \
    X(xrDestroySession)                                                                                                     \
    X(xrBeginSession)                                                                                                       \
    X(xrEndSession)                                                                                                         \
    X(xrPollEvent)                                                                                                          \
    X(xrCreateReferenceSpace)                                                                                               \
    X(xrDestroySpace)                                                                                                       \
    X(xrLocateSpace)                                                                                                        \
    X(xrEnumerateSwapchainFormats)                                                                                          \
    X(xrCreateSwapchain)                                                                                                    \
    X(xrDestroySwapchain)                                                                                                   \
    X(xrEnumerateSwapchainImages)                                                                                           \
    X(xrAcquireSwapchainImage)                                                                                              \
    X(xrWaitSwapchainImage)                                                                                                 \
    X(xrReleaseSwapchainImage)                                                                                              \
    X(xrWaitFrame)                                                                                                          \
    X(xrBeginFrame)                                                                                                         \
    X(xrEndFrame)                                                                                                           \
    X(xrResultToString)
#define VR_XR_DECLARE(name) PFN_##name p_##name = nullptr;
        VR_XR_FUNCTIONS(VR_XR_DECLARE)
#undef VR_XR_DECLARE
        PFN_xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties = nullptr;
        PFN_xrCreateInstance p_xrCreateInstance = nullptr;

        // ---- the session --------------------------------------------------------------------------------------------
        ID3D11Device *g_device = nullptr;
        XrInstance g_instance = XR_NULL_HANDLE;
        XrSystemId g_system = XR_NULL_SYSTEM_ID;
        XrSession g_session = XR_NULL_HANDLE;
        XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
        bool g_running = false;
        XrSpace g_viewSpace = XR_NULL_HANDLE, g_localSpace = XR_NULL_HANDLE, g_space = XR_NULL_HANDLE; // g_space: recentred
        bool g_wantRecenter = true;
        XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrFrameState g_frame = {XR_TYPE_FRAME_STATE};
        bool g_frameBegun = false;
        uint32_t g_viewCount = 2;
        std::string g_runtime;
        uint64_t g_submitted = 0;
        int64_t g_formats[4] = {}; // preference order, filled from the runtime's list
        int g_formatCount = 0;
        std::string g_lastError;

        struct Swapchain
        {
            XrSwapchain handle = XR_NULL_HANDLE;
            uint32_t width = 0, height = 0;
            int64_t format = 0;
            std::vector<XrSwapchainImageD3D11KHR> images;
            bool acquired = false;
            uint32_t index = 0;
        };
        Swapchain g_chains[static_cast<int>(Panel::Count)];

        const char *resultText(XrResult r)
        {
            static char text[XR_MAX_RESULT_STRING_SIZE];
            if (p_xrResultToString && g_instance && XR_SUCCEEDED(p_xrResultToString(g_instance, r, text)))
                return text;
            std::snprintf(text, sizeof(text), "XrResult %d", static_cast<int>(r));
            return text;
        }

        bool check(XrResult r, const char *what)
        {
            if (XR_SUCCEEDED(r))
                return true;
            g_lastError = std::string(what) + ": " + resultText(r);
            mod::log("[VR] %s\n", g_lastError.c_str());
            return false;
        }

        std::wstring moduleDir()
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&moduleDir), &self);
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(self, path, MAX_PATH);
            std::wstring s(path);
            const size_t slash = s.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
        }

        bool loadLoader(std::string &why)
        {
            if (g_loader)
                return true;
            // VCS_VR_RUNTIME=<runtime.dll>: that runtime directly, without the loader (tests: the loader ignores
            // XR_RUNTIME_JSON in an elevated process; no API layers then)
            if (const char *rt = std::getenv("VCS_VR_RUNTIME"); rt && *rt)
            {
                g_loader = LoadLibraryA(rt);
                auto negotiate = g_loader ? reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                                                GetProcAddress(g_loader, "xrNegotiateLoaderRuntimeInterface"))
                                          : nullptr;
                XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
                                            sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_RUNTIME_VERSION,
                                            XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION};
                XrNegotiateRuntimeRequest rq = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                                sizeof(XrNegotiateRuntimeRequest)};
                if (!negotiate || XR_FAILED(negotiate(&li, &rq)) || !rq.getInstanceProcAddr)
                {
                    why = std::string("VCS_VR_RUNTIME=") + rt + ": not an OpenXR runtime";
                    return false;
                }
                pGetInstanceProcAddr = rq.getInstanceProcAddr;
                mod::log("[VR] runtime %s loaded directly (VCS_VR_RUNTIME)\n", rt);
            }
            if (pGetInstanceProcAddr)
            {
                pGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                     reinterpret_cast<PFN_xrVoidFunction *>(&p_xrEnumerateInstanceExtensionProperties));
                pGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&p_xrCreateInstance));
                return p_xrEnumerateInstanceExtensionProperties && p_xrCreateInstance;
            }
            const std::wstring here = moduleDir() + L"openxr_loader.dll";
            g_loader = LoadLibraryW(here.c_str());
            if (!g_loader)
                g_loader = LoadLibraryW(L"openxr_loader.dll");
            if (!g_loader)
            {
                why = "openxr_loader.dll not found (put it next to VR.asi in build\\scripts)";
                return false;
            }
            pGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
            if (!pGetInstanceProcAddr)
            {
                why = "openxr_loader.dll has no xrGetInstanceProcAddr";
                return false;
            }
            pGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                 reinterpret_cast<PFN_xrVoidFunction *>(&p_xrEnumerateInstanceExtensionProperties));
            pGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&p_xrCreateInstance));
            if (!p_xrEnumerateInstanceExtensionProperties || !p_xrCreateInstance)
            {
                why = "openxr_loader.dll: no xrCreateInstance";
                return false;
            }
            return true;
        }

        bool createInstance(std::string &why)
        {
            uint32_t count = 0;
            XrResult r = p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
            if (XR_FAILED(r))
            {
                why = r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE
                          ? "no OpenXR runtime (none installed or none active: SteamVR, Meta Quest Link, Virtual Desktop, WMR ...)"
                          : std::string("the OpenXR runtime's extensions: XrResult ") + std::to_string(r);
                return false;
            }
            std::vector<XrExtensionProperties> ext(count, {XR_TYPE_EXTENSION_PROPERTIES});
            p_xrEnumerateInstanceExtensionProperties(nullptr, count, &count, ext.data());
            bool d3d11 = false;
            for (const XrExtensionProperties &e : ext)
                d3d11 = d3d11 || std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
            if (!d3d11)
            {
                why = "the OpenXR runtime has no D3D11 support (XR_KHR_D3D11_enable)";
                return false;
            }
            const char *enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
            XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
            std::snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "GTA VCS native port");
            ci.applicationInfo.applicationVersion = 1;
            std::snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "VCS-Recomp VR.asi");
            ci.applicationInfo.engineVersion = 1;
            ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); // 1.0: every runtime accepts it
            ci.enabledExtensionCount = 1;
            ci.enabledExtensionNames = enabled;
            r = p_xrCreateInstance(&ci, &g_instance);
            if (XR_FAILED(r))
            {
                why = std::string("xrCreateInstance: XrResult ") + std::to_string(r);
                g_instance = XR_NULL_HANDLE;
                return false;
            }
#define VR_XR_LOAD(name) pGetInstanceProcAddr(g_instance, #name, reinterpret_cast<PFN_xrVoidFunction *>(&p_##name));
            VR_XR_FUNCTIONS(VR_XR_LOAD)
#undef VR_XR_LOAD
#define VR_XR_CHECK(name) || !p_##name
            if (false VR_XR_FUNCTIONS(VR_XR_CHECK))
            {
                why = "the OpenXR runtime lacks a core function";
                return false;
            }
#undef VR_XR_CHECK
            XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &props)))
            {
                char v[64];
                std::snprintf(v, sizeof(v), " %u.%u.%u", XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                              XR_VERSION_PATCH(props.runtimeVersion));
                g_runtime = std::string(props.runtimeName) + v;
            }
            return true;
        }

        // the headset; XR_ERROR_FORM_FACTOR_UNAVAILABLE (off, not connected) is "later"
        enum class SystemResult
        {
            Ok,
            Later,
            Failed
        };
        SystemResult getSystem(std::string &why)
        {
            XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
            gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            const XrResult r = p_xrGetSystem(g_instance, &gi, &g_system);
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            {
                why = "no headset connected (the OpenXR runtime " + g_runtime + " is there): retrying";
                return SystemResult::Later;
            }
            if (XR_FAILED(r))
            {
                why = std::string("xrGetSystem: ") + resultText(r);
                return SystemResult::Failed;
            }
            XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
            if (XR_SUCCEEDED(p_xrGetSystemProperties(g_instance, g_system, &sp)))
                mod::log("[VR] headset: %s (runtime %s)\n", sp.systemName, g_runtime.c_str());
            uint32_t n = 0;
            p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
            std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            if (n && XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n,
                                                                          &n, views.data())))
                mod::log("[VR] %u views, %ux%u per eye recommended\n", n, views[0].recommendedImageRectWidth,
                         views[0].recommendedImageRectHeight);
            g_viewCount = std::max(1u, n);
            uint32_t bn = 0;
            p_xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bn, nullptr);
            std::vector<XrEnvironmentBlendMode> blends(bn);
            if (bn && XR_SUCCEEDED(p_xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bn,
                                                                          &bn, blends.data())))
                g_blend = blends[0];
            return SystemResult::Ok;
        }

        LUID adapterLuid(ID3D11Device *d)
        {
            LUID luid = {};
            IDXGIDevice *dx = nullptr;
            if (SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dx))) && dx)
            {
                IDXGIAdapter *a = nullptr;
                if (SUCCEEDED(dx->GetAdapter(&a)) && a)
                {
                    DXGI_ADAPTER_DESC desc = {};
                    if (SUCCEEDED(a->GetDesc(&desc)))
                        luid = desc.AdapterLuid;
                    a->Release();
                }
                dx->Release();
            }
            return luid;
        }

        bool createSession(std::string &why)
        {
            XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            if (XR_FAILED(p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req)))
            {
                why = "xrGetD3D11GraphicsRequirementsKHR failed";
                return false;
            }
            const LUID mine = adapterLuid(g_device);
            if (mine.LowPart != req.adapterLuid.LowPart || mine.HighPart != req.adapterLuid.HighPart)
                mod::log("[VR] the headset wants another GPU than the game's: the session may fail (connect the headset to the "
                         "GPU the game runs on)\n");
            if (g_device->GetFeatureLevel() < req.minFeatureLevel)
            {
                why = "the game's D3D11 device is below the runtime's minimum feature level";
                return false;
            }
            XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            binding.device = g_device;
            XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
            si.next = &binding;
            si.systemId = g_system;
            XrResult r = p_xrCreateSession(g_instance, &si, &g_session);
            if (XR_FAILED(r))
            {
                why = std::string("xrCreateSession: ") + resultText(r);
                g_session = XR_NULL_HANDLE;
                return false;
            }
            XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            rs.poseInReferenceSpace.orientation.w = 1.0f;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            if (!check(p_xrCreateReferenceSpace(g_session, &rs, &g_viewSpace), "VIEW space"))
                return why = g_lastError, false;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (!check(p_xrCreateReferenceSpace(g_session, &rs, &g_localSpace), "LOCAL space") ||
                !check(p_xrCreateReferenceSpace(g_session, &rs, &g_space), "LOCAL space"))
                return why = g_lastError, false;
            // swapchain formats: sRGB first (the picture is sRGB-encoded: written linear into an sRGB image it shows as
            // on the monitor), RGBA before BGRA
            uint32_t n = 0;
            p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
            std::vector<int64_t> formats(n);
            if (n)
                p_xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
            const int64_t wanted[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      DXGI_FORMAT_B8G8R8A8_UNORM};
            g_formatCount = 0;
            for (int64_t w : wanted)
                if (std::find(formats.begin(), formats.end(), w) != formats.end())
                    g_formats[g_formatCount++] = w;
            if (!g_formatCount)
            {
                why = "the OpenXR runtime offers no 8-bit RGBA swapchain format";
                return false;
            }
            mod::log("[VR] session created (swapchain format %lld)\n", static_cast<long long>(g_formats[0]));
            return true;
        }

        void destroySwapchain(Swapchain &s)
        {
            if (s.handle)
                p_xrDestroySwapchain(s.handle);
            s = Swapchain();
        }

        bool g_pending = false; // instance up, headset not there yet

        void destroySession()
        {
            for (Swapchain &s : g_chains)
                destroySwapchain(s);
            for (XrSpace *sp : {&g_viewSpace, &g_localSpace, &g_space})
                if (*sp)
                {
                    p_xrDestroySpace(*sp);
                    *sp = XR_NULL_HANDLE;
                }
            if (g_session)
                p_xrDestroySession(g_session);
            g_session = XR_NULL_HANDLE;
            g_running = false;
            g_frameBegun = false;
            g_state = XR_SESSION_STATE_UNKNOWN;
        }

        void mul(const float a[4], const float b[4], float out[4]) // quaternion product a * b
        {
            out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
        }
    }

    bool start(ID3D11Device *device, std::string &why)
    {
        if (g_session)
            return true;
        g_device = device;
        if (!g_instance)
        {
            if (!loadLoader(why) || !createInstance(why))
            {
                stop();
                return false;
            }
        }
        if (g_system == XR_NULL_SYSTEM_ID)
        {
            switch (getSystem(why))
            {
            case SystemResult::Later: g_pending = true; return false;
            case SystemResult::Failed: stop(); return false;
            case SystemResult::Ok: break;
            }
        }
        g_pending = false;
        if (!createSession(why))
        {
            destroySession();
            return false;
        }
        g_wantRecenter = true;
        return true;
    }

    void stop()
    {
        if (g_instance)
        {
            if (g_running && g_session)
            {
                if (g_frameBegun)
                    endFrame({}, nullptr, 0);
                p_xrEndSession(g_session);
            }
            destroySession();
            p_xrDestroyInstance(g_instance);
        }
        g_instance = XR_NULL_HANDLE;
        g_system = XR_NULL_SYSTEM_ID;
        g_pending = false;
        g_running = false;
        g_frameBegun = false;
    }

    bool started() { return g_session != XR_NULL_HANDLE; }
    bool pending() { return g_pending; }
    std::string runtimeName() { return g_runtime; }

    void poll()
    {
        if (!g_instance)
            return;
        XrEventDataBuffer e = {XR_TYPE_EVENT_DATA_BUFFER};
        while (p_xrPollEvent(g_instance, &e) == XR_SUCCESS)
        {
            if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto &ev = reinterpret_cast<const XrEventDataSessionStateChanged &>(e);
                g_state = ev.state;
                mod::log("[VR] session state %d\n", static_cast<int>(ev.state));
                if (ev.state == XR_SESSION_STATE_READY && g_session)
                {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    g_running = check(p_xrBeginSession(g_session, &bi), "xrBeginSession");
                    if (g_running)
                        mod::log("[VR] running: frames go to the headset\n");
                }
                else if (ev.state == XR_SESSION_STATE_STOPPING && g_session)
                {
                    p_xrEndSession(g_session);
                    g_running = false;
                    g_frameBegun = false;
                    mod::log("[VR] stopped by the runtime\n");
                }
                else if (ev.state == XR_SESSION_STATE_EXITING || ev.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    mod::log("[VR] the session ended (%s): the headset is released\n",
                             ev.state == XR_SESSION_STATE_EXITING ? "exiting" : "lost");
                    destroySession();
                    g_system = XR_NULL_SYSTEM_ID; // find the headset again on the next start
                    break;
                }
            }
            else if (e.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            {
                mod::log("[VR] the OpenXR runtime is going away\n");
                stop();
                return;
            }
            else if (e.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
                g_wantRecenter = true;
            e = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    bool running() { return g_running && g_session; }
    bool focused() { return g_state == XR_SESSION_STATE_FOCUSED; }

    bool waitAndBegin()
    {
        if (!running() || g_frameBegun)
            return g_frameBegun;
        XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
        g_frame = {XR_TYPE_FRAME_STATE};
        if (!check(p_xrWaitFrame(g_session, &wi, &g_frame), "xrWaitFrame"))
            return false;
        XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult r = p_xrBeginFrame(g_session, &bi);
        g_frameBegun = r == XR_SUCCESS || r == XR_FRAME_DISCARDED;
        if (!g_frameBegun)
            check(r, "xrBeginFrame");
        if (g_frameBegun && g_wantRecenter)
        {
            g_wantRecenter = false;
            recenter();
        }
        return g_frameBegun;
    }

    bool frameBegun() { return g_frameBegun; }
    bool shouldRender() { return g_frameBegun && g_frame.shouldRender; }
    int64_t predictedDisplayTime() { return g_frame.predictedDisplayTime; }

    bool locateHead(Pose &out)
    {
        if (!g_session || !g_frame.predictedDisplayTime)
            return false;
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(p_xrLocateSpace(g_viewSpace, g_space, g_frame.predictedDisplayTime, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            return false;
        out.q[0] = loc.pose.orientation.x, out.q[1] = loc.pose.orientation.y, out.q[2] = loc.pose.orientation.z,
        out.q[3] = loc.pose.orientation.w;
        const bool pos = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        out.p[0] = pos ? loc.pose.position.x : 0.0f, out.p[1] = pos ? loc.pose.position.y : 0.0f, out.p[2] = pos ? loc.pose.position.z : 0.0f;
        return true;
    }

    void recenter()
    {
        if (!g_session || !g_frame.predictedDisplayTime)
        {
            g_wantRecenter = true;
            return;
        }
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(p_xrLocateSpace(g_viewSpace, g_localSpace, g_frame.predictedDisplayTime, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            g_wantRecenter = true;
            return;
        }
        // yaw only: the head's ahead (-z) flattened
        const XrQuaternionf &q = loc.pose.orientation;
        const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float yaw = std::atan2(-fx, -fz); // about +y, left +
        XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            rs.poseInReferenceSpace.position = loc.pose.position;
        XrSpace space = XR_NULL_HANDLE;
        if (XR_SUCCEEDED(p_xrCreateReferenceSpace(g_session, &rs, &space)))
        {
            if (g_space)
                p_xrDestroySpace(g_space);
            g_space = space;
            mod::log("[VR] recentred (yaw %.0f deg)\n", yaw * 57.29578f);
        }
    }

    ID3D11Texture2D *panelImage(Panel panel, uint32_t width, uint32_t height, bool &srgb)
    {
        Swapchain &s = g_chains[static_cast<int>(panel)];
        if (!g_session || !g_frameBegun || !width || !height)
            return nullptr;
        if (s.acquired)
        {
            srgb = s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            return s.images[s.index].texture;
        }
        if (s.handle && (s.width != width || s.height != height))
            destroySwapchain(s);
        if (!s.handle)
        {
            XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = g_formats[0];
            ci.sampleCount = 1;
            ci.width = width;
            ci.height = height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (!check(p_xrCreateSwapchain(g_session, &ci, &s.handle), "xrCreateSwapchain"))
            {
                s = Swapchain();
                return nullptr;
            }
            s.width = width;
            s.height = height;
            s.format = g_formats[0];
            uint32_t n = 0;
            p_xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr);
            s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (!n || XR_FAILED(p_xrEnumerateSwapchainImages(s.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(s.images.data()))))
            {
                destroySwapchain(s);
                return nullptr;
            }
            mod::log("[VR] swapchain %d: %ux%u, %u images\n", static_cast<int>(panel), width, height, n);
        }
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!check(p_xrAcquireSwapchainImage(s.handle, &ai, &s.index), "xrAcquireSwapchainImage"))
            return nullptr;
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000; // 100 ms
        if (!check(p_xrWaitSwapchainImage(s.handle, &wi), "xrWaitSwapchainImage"))
        {
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(s.handle, &ri);
            return nullptr;
        }
        s.acquired = true;
        srgb = s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        return s.images[s.index].texture;
    }

    void endFrame(const ProjectionLayer &projection, const QuadLayer *quads, int quadCount)
    {
        if (!g_frameBegun || !g_session)
            return;
        bool used[static_cast<int>(Panel::Count)] = {};
        for (Swapchain &s : g_chains)
            if (s.acquired)
            {
                XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                p_xrReleaseSwapchainImage(s.handle, &ri);
                s.acquired = false;
                used[&s - g_chains] = true;
            }
        std::vector<const XrCompositionLayerBaseHeader *> layers;
        std::vector<XrCompositionLayerProjectionView> views;
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerQuad> quadLayers;
        if (g_frame.shouldRender)
        {
            const Swapchain &sc = g_chains[static_cast<int>(Panel::Scene)];
            if (projection.on && used[static_cast<int>(Panel::Scene)])
            {
                views.assign(g_viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                for (XrCompositionLayerProjectionView &v : views)
                {
                    // mono: the same image and the same (head) pose for each eye
                    v.pose.orientation = {projection.pose.q[0], projection.pose.q[1], projection.pose.q[2], projection.pose.q[3]};
                    v.pose.position = {projection.pose.p[0], projection.pose.p[1], projection.pose.p[2]};
                    v.fov = {projection.fov.left, projection.fov.right, projection.fov.up, projection.fov.down};
                    v.subImage.swapchain = sc.handle;
                    v.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height)}};
                }
                proj.space = g_space;
                proj.viewCount = static_cast<uint32_t>(views.size());
                proj.views = views.data();
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj));
            }
            quadLayers.reserve(static_cast<size_t>(std::max(quadCount, 0)));
            for (int i = 0; i < quadCount; ++i)
            {
                const QuadLayer &q = quads[i];
                const Swapchain &s = g_chains[static_cast<int>(q.panel)];
                if (!q.on || !used[static_cast<int>(q.panel)])
                    continue;
                XrCompositionLayerQuad l = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                l.layerFlags = q.alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                l.space = g_space;
                l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                l.subImage.swapchain = s.handle;
                l.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)}};
                l.pose.orientation = {q.pose.q[0], q.pose.q[1], q.pose.q[2], q.pose.q[3]};
                l.pose.position = {q.pose.p[0], q.pose.p[1], q.pose.p[2]};
                l.size = {q.width, q.height};
                quadLayers.push_back(l);
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quadLayers.back()));
            }
        }
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_frame.predictedDisplayTime;
        ei.environmentBlendMode = g_blend;
        ei.layerCount = static_cast<uint32_t>(layers.size());
        ei.layers = layers.data();
        g_frameBegun = false;
        if (check(p_xrEndFrame(g_session, &ei), "xrEndFrame"))
            ++g_submitted;
    }

    uint64_t framesSubmitted() { return g_submitted; }

    std::string status()
    {
        char b[256];
        std::snprintf(b, sizeof(b), "runtime=\"%s\" instance=%d session=%d state=%d running=%d frames=%llu", g_runtime.c_str(),
                      g_instance ? 1 : 0, g_session ? 1 : 0, static_cast<int>(g_state), running() ? 1 : 0,
                      static_cast<unsigned long long>(g_submitted));
        return b;
    }
}
