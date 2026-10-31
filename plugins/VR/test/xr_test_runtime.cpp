// A minimal OpenXR runtime for testing VR.asi without a headset (never installed: a run selects it for itself with
// XR_RUNTIME_JSON=<build>\plugins\VR\xr_test_runtime.json, which the Khronos loader honours per process).
//
// One simulated HMD (two views, 90 Hz), D3D11 only (XR_KHR_D3D11_enable), LOCAL / VIEW / STAGE spaces, swapchains of
// real D3D11 textures on the application's device, the session states up to FOCUSED. The head:
//   XRTEST_HEAD=yaw,pitch,roll   degrees (left +, up +, right +), fixed
//   XRTEST_YAW_SWEEP=deg         plus a yaw swinging +-deg over 8 s
// Captures (what the headset would show): XRTEST_CAPTURE=<dir>, every XRTEST_CAPTURE_EVERY frames (90), at most
// XRTEST_CAPTURE_MAX (20): per capture eye_<n>_left.png / eye_<n>_right.png - each eye's view (110 deg, 900x900)
// composited on the CPU from the submitted layers - the layers' own images layer_<n>_<i>.png, and a line per frame in
// layers.txt (layer kinds, poses, fields of view, sizes).
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace
{
    constexpr float kPi = 3.14159265f;
    constexpr int64_t kPeriodNs = 11111111; // 90 Hz

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- maths ------------------------------------------------------------------------------------------------------
    struct V3
    {
        float x = 0, y = 0, z = 0;
    };
    V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    XrQuaternionf qmul(const XrQuaternionf &a, const XrQuaternionf &b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    XrQuaternionf qconj(const XrQuaternionf &q) { return {-q.x, -q.y, -q.z, q.w}; }
    V3 rot(const XrQuaternionf &q, V3 v)
    {
        const XrQuaternionf p = qmul(qmul(q, {v.x, v.y, v.z, 0.0f}), qconj(q));
        return {p.x, p.y, p.z};
    }
    XrQuaternionf axisAngle(V3 axis, float a) { return {axis.x * std::sin(a / 2), axis.y * std::sin(a / 2), axis.z * std::sin(a / 2), std::cos(a / 2)}; }
    XrPosef compose(const XrPosef &a, const XrPosef &b) // a * b: b in a's frame
    {
        const V3 p = rot(a.orientation, {b.position.x, b.position.y, b.position.z});
        XrPosef r;
        r.orientation = qmul(a.orientation, b.orientation);
        r.position = {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z};
        return r;
    }
    XrPosef inverse(const XrPosef &a)
    {
        XrPosef r;
        r.orientation = qconj(a.orientation);
        const V3 p = rot(r.orientation, {-a.position.x, -a.position.y, -a.position.z});
        r.position = {p.x, p.y, p.z};
        return r;
    }
    XrPosef identity()
    {
        XrPosef p = {};
        p.orientation.w = 1.0f;
        return p;
    }

    // ---- PNG (stored deflate) ---------------------------------------------------------------------------------------
    uint32_t crc32(const uint8_t *d, size_t n, uint32_t c = 0)
    {
        static uint32_t table[256];
        static bool made = false;
        if (!made)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t k = i;
                for (int j = 0; j < 8; ++j)
                    k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
                table[i] = k;
            }
            made = true;
        }
        c = ~c;
        for (size_t i = 0; i < n; ++i)
            c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
        return ~c;
    }
    void writePng(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
        for (int y = 0; y < h; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t off = 0; off < raw.size() || off == 0;)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - off);
            const bool last = off + n >= raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(n & 0xFF), z.push_back(n >> 8);
            z.push_back(~n & 0xFF), z.push_back((~n >> 8) & 0xFF);
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
            off += n;
            if (last)
                break;
        }
        const uint32_t adler = (b << 16) | a;
        for (int i = 3; i >= 0; --i)
            z.push_back((adler >> (8 * i)) & 0xFF);
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return;
        auto be32 = [&](uint32_t v) {
            const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
            std::fwrite(x, 1, 4, f);
        };
        auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
            be32(static_cast<uint32_t>(data.size()));
            std::vector<uint8_t> td(type, type + 4);
            td.insert(td.end(), data.begin(), data.end());
            std::fwrite(td.data(), 1, td.size(), f);
            be32(crc32(td.data(), td.size()));
        };
        const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::fwrite(sig, 1, 8, f);
        std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                     uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
        std::fclose(f);
    }

    // ---- objects ----------------------------------------------------------------------------------------------------
    struct Swapchain
    {
        uint32_t width = 0, height = 0;
        int64_t format = 0;
        std::vector<ID3D11Texture2D *> images;
        uint32_t next = 0, acquired = ~0u, last = 0; // last: the most recently released image
    };
    struct Space
    {
        XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XrPosef offset = identity();
    };
    struct Session
    {
        ID3D11Device *device = nullptr;
        ID3D11DeviceContext *context = nullptr;
        bool running = false;
    };

    std::mutex g_mutex;
    std::deque<XrEventDataSessionStateChanged> g_events;
    Session *g_session = nullptr;
    int64_t g_start = 0, g_nextDisplay = 0;
    uint64_t g_frames = 0;
    int g_captures = 0;
    XrInstance g_instance = reinterpret_cast<XrInstance>(uintptr_t(0x1001));

    void pushState(XrSessionState s)
    {
        XrEventDataSessionStateChanged e = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
        e.session = reinterpret_cast<XrSession>(g_session);
        e.state = s;
        e.time = nowNs();
        g_events.push_back(e);
    }

    float envFloat(const char *name, float fallback)
    {
        const char *e = std::getenv(name);
        return e && *e ? static_cast<float>(std::atof(e)) : fallback;
    }

    // the simulated head in LOCAL at time t (ns)
    XrPosef headAt(int64_t t)
    {
        static float fixed[3] = {0, 0, 0};
        static bool parsed = false;
        if (!parsed)
        {
            parsed = true;
            if (const char *e = std::getenv("XRTEST_HEAD"))
                std::sscanf(e, "%f,%f,%f", &fixed[0], &fixed[1], &fixed[2]);
        }
        const float sweep = envFloat("XRTEST_YAW_SWEEP", 0.0f);
        const double secs = (t - g_start) / 1e9;
        const float yaw = (fixed[0] + sweep * static_cast<float>(std::sin(secs * 2.0 * kPi / 8.0))) * kPi / 180.0f;
        const float pitch = fixed[1] * kPi / 180.0f, roll = fixed[2] * kPi / 180.0f;
        XrPosef p = identity();
        // yaw about +y, then pitch about the head's x, then roll about its -z (right ear down = roll right +)
        p.orientation = qmul(qmul(axisAngle({0, 1, 0}, yaw), axisAngle({1, 0, 0}, pitch)), axisAngle({0, 0, -1}, roll));
        return p;
    }

    XrPosef spaceInLocal(const Space &s, int64_t t)
    {
        if (s.type == XR_REFERENCE_SPACE_TYPE_VIEW)
            return compose(headAt(t), s.offset);
        return s.offset;
    }

    // ---- captures ---------------------------------------------------------------------------------------------------
    struct Pixels
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        bool valid() const { return w > 0; }
        void sample(float u, float v, float out[4]) const
        {
            const int x = std::clamp(static_cast<int>(u * w), 0, w - 1), y = std::clamp(static_cast<int>(v * h), 0, h - 1);
            const uint8_t *p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            for (int i = 0; i < 4; ++i)
                out[i] = p[i] / 255.0f;
        }
    };

    Pixels readImage(Session &s, const Swapchain &sc, uint32_t index)
    {
        Pixels px;
        if (index >= sc.images.size())
            return px;
        D3D11_TEXTURE2D_DESC td = {};
        sc.images[index]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ID3D11Texture2D *staging = nullptr;
        if (FAILED(s.device->CreateTexture2D(&td, nullptr, &staging)))
            return px;
        s.context->CopyResource(staging, sc.images[index]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(s.context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            px.w = static_cast<int>(td.Width), px.h = static_cast<int>(td.Height);
            px.rgba.resize(static_cast<size_t>(px.w) * px.h * 4);
            const bool bgra = sc.format == DXGI_FORMAT_B8G8R8A8_UNORM || sc.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (int y = 0; y < px.h; ++y)
            {
                const uint8_t *row = static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                uint8_t *out = &px.rgba[static_cast<size_t>(y) * px.w * 4];
                std::memcpy(out, row, static_cast<size_t>(px.w) * 4);
                if (bgra)
                    for (int x = 0; x < px.w; ++x)
                        std::swap(out[x * 4], out[x * 4 + 2]);
            }
            s.context->Unmap(staging, 0);
        }
        staging->Release();
        return px;
    }

    void capture(Session &s, const XrFrameEndInfo *info)
    {
        static const char *dir = std::getenv("XRTEST_CAPTURE");
        static const int every = std::max(1, static_cast<int>(envFloat("XRTEST_CAPTURE_EVERY", 90.0f)));
        static const int most = static_cast<int>(envFloat("XRTEST_CAPTURE_MAX", 20.0f));
        if (!dir || !*dir)
            return;
        // a line per frame (layers.txt), every frame
        const std::string base = std::string(dir) + "\\";
        if (FILE *f = std::fopen((base + "layers.txt").c_str(), "a"))
        {
            std::fprintf(f, "frame %llu layers %u:", static_cast<unsigned long long>(g_frames), info->layerCount);
            for (uint32_t i = 0; i < info->layerCount; ++i)
            {
                const XrCompositionLayerBaseHeader *l = info->layers[i];
                if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                {
                    const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(l);
                    const XrCompositionLayerProjectionView &v = p->views[0];
                    std::fprintf(f, " [projection views %u q %.3f %.3f %.3f %.3f fov L%.1f R%.1f U%.1f D%.1f %dx%d]", p->viewCount,
                                 v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
                                 v.fov.angleLeft * 57.3f, v.fov.angleRight * 57.3f, v.fov.angleUp * 57.3f, v.fov.angleDown * 57.3f,
                                 v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height);
                }
                else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                {
                    const auto *q = reinterpret_cast<const XrCompositionLayerQuad *>(l);
                    std::fprintf(f, " [quad at %.2f %.2f %.2f size %.2fx%.2f alpha %d %dx%d]", q->pose.position.x, q->pose.position.y,
                                 q->pose.position.z, q->size.width, q->size.height,
                                 (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1 : 0,
                                 q->subImage.imageRect.extent.width, q->subImage.imageRect.extent.height);
                }
            }
            std::fprintf(f, "\n");
            std::fclose(f);
        }
        if (g_frames % every != 0 || g_captures >= most)
            return;
        const int n = g_captures++;
        // the layers' images
        struct Layer
        {
            const XrCompositionLayerBaseHeader *l;
            Pixels px;
        };
        std::vector<Layer> layers;
        for (uint32_t i = 0; i < info->layerCount; ++i)
        {
            const XrCompositionLayerBaseHeader *l = info->layers[i];
            const XrSwapchainSubImage *sub = l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
                                                 ? &reinterpret_cast<const XrCompositionLayerProjection *>(l)->views[0].subImage
                                                 : l->type == XR_TYPE_COMPOSITION_LAYER_QUAD
                                                       ? &reinterpret_cast<const XrCompositionLayerQuad *>(l)->subImage
                                                       : nullptr;
            if (!sub)
                continue;
            const auto *sc = reinterpret_cast<const Swapchain *>(sub->swapchain);
            Layer layer = {l, readImage(s, *sc, sc->last)};
            if (layer.px.valid())
            {
                std::vector<uint8_t> rgb(static_cast<size_t>(layer.px.w) * layer.px.h * 3);
                for (size_t k = 0; k < rgb.size() / 3; ++k)
                    for (int c = 0; c < 3; ++c)
                        rgb[k * 3 + c] = layer.px.rgba[k * 4 + c];
                writePng(base + "layer_" + std::to_string(n) + "_" + std::to_string(i) + ".png", rgb, layer.px.w, layer.px.h);
            }
            layers.push_back(std::move(layer));
        }
        // each eye: rays through a 110 deg view, the layers composited back to front
        constexpr int kSize = 900;
        const float tanHalf = std::tan(55.0f * kPi / 180.0f);
        const XrPosef head = headAt(info->displayTime);
        for (int eye = 0; eye < 2; ++eye)
        {
            XrPosef eyePose = compose(head, {{0, 0, 0, 1}, {eye ? 0.032f : -0.032f, 0.0f, 0.0f}});
            std::vector<uint8_t> rgb(static_cast<size_t>(kSize) * kSize * 3, 0);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    const V3 dirEye = {((x + 0.5f) / kSize * 2.0f - 1.0f) * tanHalf, (1.0f - (y + 0.5f) / kSize * 2.0f) * tanHalf, -1.0f};
                    float col[3] = {0.08f, 0.08f, 0.1f}; // the runtime's empty space
                    for (const Layer &L : layers)
                    {
                        if (!L.px.valid())
                            continue;
                        float s[4] = {};
                        bool hit = false, alpha = false;
                        if (L.l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                        {
                            // in the layer's space (LOCAL-based spaces: their offsets); rotation-only reprojection
                            const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(L.l);
                            const Space &sp = *reinterpret_cast<const Space *>(p->space);
                            const XrPosef eyeInSpace = compose(inverse(spaceInLocal(sp, info->displayTime)), eyePose);
                            const V3 d = rot(eyeInSpace.orientation, dirEye);
                            const XrCompositionLayerProjectionView &v = p->views[std::min<uint32_t>(eye, p->viewCount - 1)];
                            const V3 dv = rot(qconj(v.pose.orientation), d);
                            if (dv.z < 0.0f)
                            {
                                const float tx = dv.x / -dv.z, ty = dv.y / -dv.z;
                                const float l = std::tan(v.fov.angleLeft), r = std::tan(v.fov.angleRight), u = std::tan(v.fov.angleUp),
                                            dn = std::tan(v.fov.angleDown);
                                const float uu = (tx - l) / (r - l), vv = (u - ty) / (u - dn);
                                if (uu >= 0 && uu <= 1 && vv >= 0 && vv <= 1)
                                {
                                    L.px.sample(uu, vv, s);
                                    hit = true;
                                    alpha = (p->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                                }
                            }
                        }
                        else
                        {
                            const auto *q = reinterpret_cast<const XrCompositionLayerQuad *>(L.l);
                            const Space &sp = *reinterpret_cast<const Space *>(q->space);
                            const XrPosef quadInLocal = compose(spaceInLocal(sp, info->displayTime), q->pose);
                            const XrPosef eyeInQuad = compose(inverse(quadInLocal), eyePose);
                            const V3 o = {eyeInQuad.position.x, eyeInQuad.position.y, eyeInQuad.position.z};
                            const V3 d = rot(eyeInQuad.orientation, dirEye);
                            if (std::fabs(d.z) > 1e-6f)
                            {
                                const float t = -o.z / d.z;
                                const V3 h = o + d * t;
                                if (t > 0 && std::fabs(h.x) <= q->size.width / 2 && std::fabs(h.y) <= q->size.height / 2)
                                {
                                    L.px.sample(h.x / q->size.width + 0.5f, 0.5f - h.y / q->size.height, s);
                                    hit = true;
                                    alpha = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                                }
                            }
                        }
                        if (!hit)
                            continue;
                        for (int c = 0; c < 3; ++c)
                            col[c] = alpha ? s[c] + col[c] * (1.0f - s[3]) : s[c]; // premultiplied
                    }
                    uint8_t *o = &rgb[(static_cast<size_t>(y) * kSize + x) * 3];
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<uint8_t>(std::clamp(col[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            writePng(base + "eye_" + std::to_string(n) + (eye ? "_right.png" : "_left.png"), rgb, kSize, kSize);
        }
    }

    // ---- the functions ----------------------------------------------------------------------------------------------
    XrResult XRAPI_CALL rtEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props)
    {
        if (layer)
            return XR_ERROR_API_LAYER_NOT_PRESENT;
        *count = 1;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 1)
            return XR_ERROR_SIZE_INSUFFICIENT;
        std::snprintf(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        props[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateInstance(const XrInstanceCreateInfo *, XrInstance *out)
    {
        *out = g_instance;
        std::fprintf(stderr, "[xr_test_runtime] instance created\n");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroyInstance(XrInstance) { return XR_SUCCESS; }
    XrResult XRAPI_CALL rtGetInstanceProperties(XrInstance, XrInstanceProperties *p)
    {
        p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
        std::snprintf(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "VCS xr_test_runtime");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetSystem(XrInstance, const XrSystemGetInfo *gi, XrSystemId *id)
    {
        if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        if (std::getenv("XRTEST_NO_HMD"))
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        *id = 1;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties *p)
    {
        p->systemId = 1;
        std::snprintf(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "VCS test HMD");
        p->graphicsProperties = {4096, 4096, 16};
        p->trackingProperties = {XR_TRUE, XR_TRUE};
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t *count,
                                                         XrViewConfigurationView *v)
    {
        *count = 2;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 2)
            return XR_ERROR_SIZE_INSUFFICIENT;
        for (int i = 0; i < 2; ++i)
            v[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, 1440, 2880, 1600, 3200, 1, 1};
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t *count,
                                                        XrEnvironmentBlendMode *m)
    {
        *count = 1;
        if (cap >= 1)
            m[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR *r)
    {
        IDXGIFactory1 *f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f))))
        {
            IDXGIAdapter1 *a = nullptr;
            if (SUCCEEDED(f->EnumAdapters1(0, &a)))
            {
                DXGI_ADAPTER_DESC1 d = {};
                a->GetDesc1(&d);
                r->adapterLuid = d.AdapterLuid;
                a->Release();
            }
            f->Release();
        }
        r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateSession(XrInstance, const XrSessionCreateInfo *ci, XrSession *out)
    {
        const auto *b = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(ci->next);
        if (!b || b->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || !b->device)
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_session = new Session();
        g_session->device = b->device;
        b->device->GetImmediateContext(&g_session->context);
        g_start = nowNs();
        pushState(XR_SESSION_STATE_IDLE);
        pushState(XR_SESSION_STATE_READY);
        *out = reinterpret_cast<XrSession>(g_session);
        std::fprintf(stderr, "[xr_test_runtime] session created on the application's D3D11 device\n");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySession(XrSession s)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto *ss = reinterpret_cast<Session *>(s);
        if (ss->context)
            ss->context->Release();
        delete ss;
        if (g_session == ss)
            g_session = nullptr;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtBeginSession(XrSession s, const XrSessionBeginInfo *)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        reinterpret_cast<Session *>(s)->running = true;
        pushState(XR_SESSION_STATE_SYNCHRONIZED);
        pushState(XR_SESSION_STATE_VISIBLE);
        pushState(XR_SESSION_STATE_FOCUSED);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEndSession(XrSession s)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        reinterpret_cast<Session *>(s)->running = false;
        pushState(XR_SESSION_STATE_IDLE);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtPollEvent(XrInstance, XrEventDataBuffer *e)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_events.empty())
            return XR_EVENT_UNAVAILABLE;
        std::memcpy(e, &g_events.front(), sizeof(XrEventDataSessionStateChanged));
        g_events.pop_front();
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *ci, XrSpace *out)
    {
        auto *s = new Space();
        s->type = ci->referenceSpaceType;
        s->offset = ci->poseInReferenceSpace;
        *out = reinterpret_cast<XrSpace>(s);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySpace(XrSpace s)
    {
        delete reinterpret_cast<Space *>(s);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtLocateSpace(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation *loc)
    {
        const XrPosef a = spaceInLocal(*reinterpret_cast<Space *>(space), t), b = spaceInLocal(*reinterpret_cast<Space *>(base), t);
        loc->pose = compose(inverse(b), a);
        loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                             XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t *count, int64_t *formats)
    {
        *count = 2;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 2)
            return XR_ERROR_SIZE_INSUFFICIENT;
        formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        formats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        auto *sc = new Swapchain();
        sc->width = ci->width;
        sc->height = ci->height;
        sc->format = ci->format;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = ci->width;
        td.Height = ci->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = static_cast<DXGI_FORMAT>(ci->format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 3; ++i)
        {
            ID3D11Texture2D *t = nullptr;
            if (FAILED(ss->device->CreateTexture2D(&td, nullptr, &t)))
            {
                for (ID3D11Texture2D *x : sc->images)
                    x->Release();
                delete sc;
                return XR_ERROR_RUNTIME_FAILURE;
            }
            sc->images.push_back(t);
        }
        *out = reinterpret_cast<XrSwapchain>(sc);
        std::fprintf(stderr, "[xr_test_runtime] swapchain %ux%u format %lld\n", ci->width, ci->height, static_cast<long long>(ci->format));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySwapchain(XrSwapchain s)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        for (ID3D11Texture2D *t : sc->images)
            t->Release();
        delete sc;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t *count, XrSwapchainImageBaseHeader *images)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        *count = static_cast<uint32_t>(sc->images.size());
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < *count)
            return XR_ERROR_SIZE_INSUFFICIENT;
        auto *out = reinterpret_cast<XrSwapchainImageD3D11KHR *>(images);
        for (uint32_t i = 0; i < *count; ++i)
            out[i].texture = sc->images[i];
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtAcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo *, uint32_t *index)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        if (sc->acquired != ~0u)
            return XR_ERROR_CALL_ORDER_INVALID;
        sc->acquired = sc->next;
        sc->next = (sc->next + 1) % sc->images.size();
        *index = sc->acquired;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *) { return XR_SUCCESS; }
    XrResult XRAPI_CALL rtReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo *)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        if (sc->acquired == ~0u)
            return XR_ERROR_CALL_ORDER_INVALID;
        sc->last = sc->acquired;
        sc->acquired = ~0u;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtWaitFrame(XrSession s, const XrFrameWaitInfo *, XrFrameState *st)
    {
        // the display's 90 Hz: block until the next vsync after the last frame's
        const int64_t now = nowNs();
        if (!g_nextDisplay || g_nextDisplay + kPeriodNs < now)
            g_nextDisplay = now; // late (or the first frame): from now on
        else if (g_nextDisplay > now)
            std::this_thread::sleep_for(std::chrono::nanoseconds(g_nextDisplay - now));
        g_nextDisplay += kPeriodNs;
        st->predictedDisplayTime = g_nextDisplay;
        st->predictedDisplayPeriod = kPeriodNs;
        st->shouldRender = reinterpret_cast<Session *>(s)->running ? XR_TRUE : XR_FALSE;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtBeginFrame(XrSession, const XrFrameBeginInfo *) { return XR_SUCCESS; }
    XrResult XRAPI_CALL rtEndFrame(XrSession s, const XrFrameEndInfo *info)
    {
        ++g_frames;
        capture(*reinterpret_cast<Session *>(s), info);
        if (g_frames == 1 || g_frames % 900 == 0)
            std::fprintf(stderr, "[xr_test_runtime] frame %llu: %u layers\n", static_cast<unsigned long long>(g_frames), info->layerCount);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtResultToString(XrInstance, XrResult r, char buffer[XR_MAX_RESULT_STRING_SIZE])
    {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", static_cast<int>(r));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtRequestExitSession(XrSession) { return XR_SUCCESS; }

    XrResult XRAPI_CALL rtGetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *fn)
    {
        struct Entry
        {
            const char *name;
            PFN_xrVoidFunction fn;
        };
#define E(n, f) {n, reinterpret_cast<PFN_xrVoidFunction>(&f)}
        static const Entry table[] = {
            E("xrGetInstanceProcAddr", rtGetInstanceProcAddr),
            E("xrEnumerateInstanceExtensionProperties", rtEnumerateInstanceExtensionProperties),
            E("xrCreateInstance", rtCreateInstance),
            E("xrDestroyInstance", rtDestroyInstance),
            E("xrGetInstanceProperties", rtGetInstanceProperties),
            E("xrGetSystem", rtGetSystem),
            E("xrGetSystemProperties", rtGetSystemProperties),
            E("xrEnumerateViewConfigurationViews", rtEnumerateViewConfigurationViews),
            E("xrEnumerateEnvironmentBlendModes", rtEnumerateEnvironmentBlendModes),
            E("xrGetD3D11GraphicsRequirementsKHR", rtGetD3D11GraphicsRequirementsKHR),
            E("xrCreateSession", rtCreateSession),
            E("xrDestroySession", rtDestroySession),
            E("xrBeginSession", rtBeginSession),
            E("xrEndSession", rtEndSession),
            E("xrRequestExitSession", rtRequestExitSession),
            E("xrPollEvent", rtPollEvent),
            E("xrCreateReferenceSpace", rtCreateReferenceSpace),
            E("xrDestroySpace", rtDestroySpace),
            E("xrLocateSpace", rtLocateSpace),
            E("xrEnumerateSwapchainFormats", rtEnumerateSwapchainFormats),
            E("xrCreateSwapchain", rtCreateSwapchain),
            E("xrDestroySwapchain", rtDestroySwapchain),
            E("xrEnumerateSwapchainImages", rtEnumerateSwapchainImages),
            E("xrAcquireSwapchainImage", rtAcquireSwapchainImage),
            E("xrWaitSwapchainImage", rtWaitSwapchainImage),
            E("xrReleaseSwapchainImage", rtReleaseSwapchainImage),
            E("xrWaitFrame", rtWaitFrame),
            E("xrBeginFrame", rtBeginFrame),
            E("xrEndFrame", rtEndFrame),
            E("xrResultToString", rtResultToString),
        };
#undef E
        for (const Entry &e : table)
            if (std::strcmp(e.name, name) == 0)
            {
                *fn = e.fn;
                return XR_SUCCESS;
            }
        *fn = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *info,
                                                                                       XrNegotiateRuntimeRequest *req)
{
    if (!info || !req || info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    req->getInstanceProcAddr = &rtGetInstanceProcAddr;
    return XR_SUCCESS;
}
