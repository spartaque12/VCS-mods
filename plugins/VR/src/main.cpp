// VR.asi (docs/VR_PLAN.md M0 of the port): the game in an OpenXR headset, mono.
//
// Per host frame, in vcs.exe's present hook (main thread, right before the window's frame is shown):
//   1. the headset's last frame is finished: the picture copied into OpenXR swapchain images and xrEndFrame'd -
//      head-tracked (first person on foot): the 3D picture without the HUD as a projection layer, at the pose it was
//      rendered for and with the game camera's own field of view, the HUD on a quad 1.5 m ahead; anything else (menus,
//      cutscenes, third person, loading screens, vehicles): the window's picture and menus on a "cinema" quad;
//   2. xrWaitFrame paces the game's host frames to the headset, xrBeginFrame starts the next one, and the head's
//      orientation at its predicted display time becomes CameraAim's first-person look (CameraAimSetHeadLook) for the
//      game frame that starts next - the stick / mouse still turn the body.
// The pose each shown frame was rendered with is found by its input time (VcsPresentFrame.input_ns): the runtime then
// reprojects the picture to where the head is when it is shown, so the world stays put even with the game's frame lag.
// Without an OpenXR runtime, headset or D3D11, VR.asi says so in the log once and stays inert: the game runs as usual.
#include "mod.h"
#include "xr.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "dxbc/vr_ps_copy.h"
#include "dxbc/vr_ps_hud.h"
#include "dxbc/vr_vs.h"

namespace
{
    // ---- settings (Graphics > VR) ---------------------------------------------------------------------------------
    std::atomic<bool> g_enabled{false};
    std::atomic<int> g_view{0};             // 0: head-tracked in first person, 1: always the cinema screen
    std::atomic<int> g_cinemaCm{400};       // the cinema screen: distance, width
    std::atomic<int> g_cinemaWidthCm{500};
    std::atomic<int> g_hudCm{150};          // the HUD panel's distance
    std::atomic<int> g_renderScale{100};    // % of the headset's resolution for the picture (vr.render_scale)
    uint32_t g_outW = 0, g_outH = 0;        // the picture's output size VR.asi set (render_set_output_size)
    std::atomic<bool> g_recenter{false};    // the hotkey asked for it

    bool g_hooked = false;                  // the present and render hooks are in
    bool g_failed = false;                  // no runtime / headset / D3D11: inert until VR is switched off and on again
    bool g_split = false;                   // the renderer splits the HUD off
    int64_t g_retryNs = 0;                  // a headset that is off: try again then
    std::string g_mode = "off";             // what the headset shows (status)
    // frames on the cinema screen by why (status): cinema chosen, no layers, no 3D, no head look in the game, no pose for
    // the shown frame, a menu open
    uint64_t g_cinemaWhy[6] = {};
    const char *const kCinemaWhy[6] = {"VR view set to cinema", "no picture layers (loading screen / movie)", "no 3D shown (menu)",
                                       "first person on foot not active (key B; vehicles and cutscenes use the cinema)",
                                       "no head pose for the shown frame yet", "a menu is open"};
    // the F2 readout (vr.status): what VR is doing now
    enum Status
    {
        kOff = 0, kStarting, kNoRuntime, kNoHeadset, kWaiting, kHeadTracked, kCinema, kError, kNeedsD3D11, kEnded
    };
    std::atomic<int> g_status{kOff};
    std::string g_lastShown;              // what the headset showed last frame (logged when it changes)
    uint64_t g_headFrames = 0, g_cinemaFrames = 0;
    int64_t g_summaryNs = 0;

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- CameraAim's head look (its exports) ------------------------------------------------------------------------
    using SetHeadLookFn = void (*)(int, float, float, float);
    using HeadLookAppliedFn = int (*)();
    SetHeadLookFn g_setHeadLook = nullptr;
    using SetHeadPositionFn = void (*)(float, float, float);
    SetHeadPositionFn g_setHeadPosition = nullptr; // newer CameraAim.asi: the head's position too
    HeadLookAppliedFn g_headApplied = nullptr;
    bool g_headSent = false;

    void findCameraAim()
    {
        if (g_setHeadLook)
            return;
        if (HMODULE m = GetModuleHandleW(L"CameraAim.asi"))
        {
            g_setHeadLook = reinterpret_cast<SetHeadLookFn>(GetProcAddress(m, "CameraAimSetHeadLook"));
            g_headApplied = reinterpret_cast<HeadLookAppliedFn>(GetProcAddress(m, "CameraAimHeadLookApplied"));
            g_setHeadPosition = reinterpret_cast<SetHeadPositionFn>(GetProcAddress(m, "CameraAimSetHeadPosition"));
        }
        static bool reported = false;
        if ((!g_setHeadLook || !g_headApplied) && !reported)
        {
            reported = true;
            vr::log("CameraAim.asi with the head look not found: the headset shows the cinema screen only\n");
        }
    }

    void sendHeadLook(bool on, float yaw = 0.0f, float pitch = 0.0f, float roll = 0.0f)
    {
        if (!on && !g_headSent)
            return;
        findCameraAim();
        if (g_setHeadLook)
            g_setHeadLook(on ? 1 : 0, yaw, pitch, roll);
        g_headSent = on;
    }

    // the head's yaw (left +), pitch (up +), roll (right +) from its orientation (OpenXR: x right, y up, -z ahead)
    void headAngles(const vr::Pose &p, float &yaw, float &pitch, float &roll)
    {
        const float x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
        const float f[3] = {-2.0f * (x * z + w * y), -2.0f * (y * z - w * x), -(1.0f - 2.0f * (x * x + y * y))};
        const float u[3] = {2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)};
        yaw = std::atan2(-f[0], -f[2]);
        pitch = std::asin(std::clamp(f[1], -1.0f, 1.0f));
        float r[3] = {-f[2], 0.0f, f[0]}; // f x up
        const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (rl < 1e-4f)
        {
            roll = 0.0f;
            return;
        }
        r[0] /= rl, r[2] /= rl;
        const float u0[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]}; // r x f
        roll = std::atan2(u[0] * r[0] + u[1] * r[1] + u[2] * r[2], u[0] * u0[0] + u[1] * u0[1] + u[2] * u0[2]);
    }

    // the poses sent to the game, by the time they were sent: a shown frame was rendered with the last one before its
    // input snapshot
    struct PoseRecord
    {
        int64_t ns = 0;
        vr::Pose pose;
    };
    PoseRecord g_poses[128];
    uint32_t g_poseNext = 0;

    bool poseFor(int64_t inputNs, vr::Pose &out)
    {
        const PoseRecord *best = nullptr;
        for (const PoseRecord &r : g_poses)
            if (r.ns && r.ns <= inputNs && (!best || r.ns > best->ns))
                best = &r;
        if (!best)
            return false;
        out = best->pose;
        return true;
    }

    // ---- the 3D picture's lens (a render hook: the frame's camera) ----------------------------------------------------
    struct Lens
    {
        bool valid = false;
        float fx = 0, fy = 0, cx = 0, cy = 0, width = 0, height = 0;
    };
    Lens g_lens;

    void renderHook(void *, const VcsRenderFrame *f)
    {
        if (!f || !f->camera_valid || f->fx <= 0.0f || f->fy <= 0.0f || !f->width || !f->height)
            return;
        g_lens = {true, f->fx, f->fy, f->cx, f->cy, static_cast<float>(f->width), static_cast<float>(f->height)};
    }

    vr::Fov lensFov(const Lens &l)
    {
        vr::Fov v;
        v.left = -std::atan(l.cx / l.fx);
        v.right = std::atan((l.width - l.cx) / l.fx);
        v.up = std::atan(l.cy / l.fy);
        v.down = -std::atan((l.height - l.cy) / l.fy);
        return v;
    }

    // ---- Dear ImGui: a menu of the exe's open (F2, F5) -----------------------------------------------------------------
    bool g_imgui = false;
    bool imguiMatches(const VcsModApi *a)
    {
        return a->imgui_version_num == IMGUI_VERSION_NUM && a->imgui_sizeof_io == sizeof(ImGuiIO) &&
               a->imgui_sizeof_style == sizeof(ImGuiStyle) && a->imgui_sizeof_vec2 == sizeof(ImVec2) &&
               a->imgui_sizeof_vec4 == sizeof(ImVec4) && a->imgui_sizeof_drawvert == sizeof(ImDrawVert) &&
               a->imgui_sizeof_drawidx == sizeof(ImDrawIdx) && a->imgui_sizeof_context == sizeof(ImGuiContext);
    }
    bool menuOpen()
    {
        if (!g_imgui)
            return false;
        auto *ctx = static_cast<ImGuiContext *>(mod::api->imgui_context());
        if (!ctx)
            return false;
        // a focused window of this frame (the settings / trainer menus take focus; the F3 overlay does not)
        const ImGuiWindow *w = ctx->NavWindow;
        return (w && (w->Active || w->WasActive) && !w->Hidden) || ctx->IO.WantTextInput;
    }

    // ---- D3D11: the copies into the swapchain images ------------------------------------------------------------------
    struct Gpu
    {
        ID3D11Device *device = nullptr;
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *copy = nullptr, *hud = nullptr;
        ID3D11SamplerState *sampler = nullptr;
        ID3D11Buffer *params = nullptr;
        ID3D11RasterizerState *raster = nullptr;
        ID3D11BlendState *blend = nullptr;
        ID3D11DepthStencilState *depth = nullptr;
    } g_gpu;

    template <class T> void release(T *&p)
    {
        if (p)
            p->Release();
        p = nullptr;
    }

    void gpuRelease()
    {
        release(g_gpu.vs);
        release(g_gpu.copy);
        release(g_gpu.hud);
        release(g_gpu.sampler);
        release(g_gpu.params);
        release(g_gpu.raster);
        release(g_gpu.blend);
        release(g_gpu.depth);
        g_gpu.device = nullptr;
    }

    bool gpuInit(ID3D11Device *d)
    {
        if (g_gpu.device == d)
            return true;
        gpuRelease();
        g_gpu.device = d;
        bool ok = SUCCEEDED(d->CreateVertexShader(vr_vs, sizeof(vr_vs), nullptr, &g_gpu.vs)) &&
                  SUCCEEDED(d->CreatePixelShader(vr_ps_copy, sizeof(vr_ps_copy), nullptr, &g_gpu.copy)) &&
                  SUCCEEDED(d->CreatePixelShader(vr_ps_hud, sizeof(vr_ps_hud), nullptr, &g_gpu.hud));
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(d->CreateSamplerState(&sd, &g_gpu.sampler));
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_gpu.params));
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        ok = ok && SUCCEEDED(d->CreateRasterizerState(&rd, &g_gpu.raster));
        D3D11_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(d->CreateBlendState(&blend, &g_gpu.blend));
        D3D11_DEPTH_STENCIL_DESC dd = {};
        ok = ok && SUCCEEDED(d->CreateDepthStencilState(&dd, &g_gpu.depth));
        if (!ok)
        {
            vr::log("could not create the copy shaders\n");
            gpuRelease();
        }
        return ok;
    }

    ID3D11ShaderResourceView *srvOf(ID3D11Texture2D *t)
    {
        if (!t)
            return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // the renderer's RGBA8 (typeless or not)
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView *v = nullptr;
        g_gpu.device->CreateShaderResourceView(t, &sd, &v);
        return v;
    }

    // src (and src2 for the HUD) into the swapchain image `dst` of width x height
    void blit(ID3D11DeviceContext *c, ID3D11Texture2D *dst, bool srgb, ID3D11Texture2D *src, ID3D11Texture2D *src2, uint32_t width,
              uint32_t height)
    {
        if (!dst || !src || !g_gpu.vs)
            return;
        D3D11_TEXTURE2D_DESC td = {};
        dst->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        // a typeless image: the format the swapchain was made with is the typed one OpenXR named (sRGB preferred)
        if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            rd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            rd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = td.ArraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (td.ArraySize > 1)
            rd.Texture2DArray.ArraySize = 1;
        ID3D11RenderTargetView *rtv = nullptr;
        if (FAILED(g_gpu.device->CreateRenderTargetView(dst, &rd, &rtv)) || !rtv)
            return;
        ID3D11ShaderResourceView *srv[2] = {srvOf(src), srvOf(src2)};
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(c->Map(g_gpu.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            const float p[8] = {0.0f, 0.0f, 1.0f, 1.0f, srgb ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
            std::memcpy(m.pData, p, sizeof(p));
            c->Unmap(g_gpu.params, 0);
        }
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->OMSetBlendState(g_gpu.blend, nullptr, 0xFFFFFFFFu);
        c->OMSetDepthStencilState(g_gpu.depth, 0);
        c->RSSetState(g_gpu.raster);
        const D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        c->RSSetViewports(1, &vp);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(g_gpu.vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &g_gpu.params);
        c->PSSetShader(src2 ? g_gpu.hud : g_gpu.copy, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &g_gpu.params);
        c->PSSetShaderResources(0, 2, srv);
        c->PSSetSamplers(0, 1, &g_gpu.sampler);
        c->Draw(3, 0);
        ID3D11ShaderResourceView *none[2] = {};
        c->PSSetShaderResources(0, 2, none);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        release(srv[0]);
        release(srv[1]);
        rtv->Release();
    }

    vr::Pose ahead(float metres, float down = 0.0f)
    {
        vr::Pose p;
        p.p[1] = -down;
        p.p[2] = -metres;
        return p;
    }

    // ---- per host frame -----------------------------------------------------------------------------------------------
    // ---- performance: per host frame while VR runs (the `vr perf` command, vr.log every 30 s) ---------------------------
    struct Perf
    {
        uint64_t frames = 0, newFrames = 0, gpuSamples = 0;
        double hostMs = 0, hostMax = 0, submitMs = 0, waitMs = 0, waitMax = 0, gpuMs = 0;
    } g_perf;
    int64_t g_lastHostNs = 0, g_perfStartNs = 0;
    uint64_t g_lastSerial = 0;
    // GPU time of VR.asi's own copies: timestamp queries read back a few frames later, never waited for
    struct GpuQuery
    {
        ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
        bool pending = false;
    };
    GpuQuery g_queries[4];
    uint32_t g_queryNext = 0;

    void gpuTimeBegin(ID3D11DeviceContext *c)
    {
        GpuQuery &q = g_queries[g_queryNext % 4u];
        if (!q.disjoint && g_gpu.device)
        {
            D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            g_gpu.device->CreateQuery(&d, &q.disjoint);
            d.Query = D3D11_QUERY_TIMESTAMP;
            g_gpu.device->CreateQuery(&d, &q.begin);
            g_gpu.device->CreateQuery(&d, &q.end);
        }
        if (!q.disjoint || !q.begin || !q.end)
            return;
        if (q.pending) // the oldest: read it if the GPU is done with it, else drop the sample (never stall)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            UINT64 b = 0, e = 0;
            if (c->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                c->GetData(q.begin, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                c->GetData(q.end, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency && e >= b)
            {
                g_perf.gpuMs += (e - b) * 1000.0 / static_cast<double>(dj.Frequency);
                ++g_perf.gpuSamples;
            }
        }
        c->Begin(q.disjoint);
        c->End(q.begin);
    }

    void gpuTimeEnd(ID3D11DeviceContext *c)
    {
        GpuQuery &q = g_queries[g_queryNext % 4u];
        if (!q.disjoint || !q.begin || !q.end)
            return;
        c->End(q.end);
        c->End(q.disjoint);
        q.pending = true;
        ++g_queryNext;
    }

    std::string perfLine(bool reset)
    {
        const double n = std::max<double>(1.0, static_cast<double>(g_perf.frames));
        const double secs = g_perfStartNs ? (nowNs() - g_perfStartNs) / 1e9 : 0.0;
        char b[400];
        std::snprintf(b, sizeof(b),
                      "host %.1f fps (frame %.2f ms avg, %.2f max), new game frames %.1f /s (%.0f%% of host frames), "
                      "VR.asi CPU %.2f ms, GPU %.3f ms, waiting for the headset %.2f ms avg (%.2f max), picture %ux%u",
                      secs > 0 ? g_perf.frames / secs : 0.0, g_perf.hostMs / n, g_perf.hostMax,
                      secs > 0 ? g_perf.newFrames / secs : 0.0, 100.0 * g_perf.newFrames / n, g_perf.submitMs / n,
                      g_perf.gpuSamples ? g_perf.gpuMs / g_perf.gpuSamples : 0.0, g_perf.waitMs / n, g_perf.waitMax, g_outW, g_outH);
        if (reset)
        {
            g_perf = {};
            g_perfStartNs = nowNs();
        }
        return b;
    }

    void releaseQueries()
    {
        for (GpuQuery &q : g_queries)
        {
            release(q.disjoint);
            release(q.begin);
            release(q.end);
            q.pending = false;
        }
    }

    // ---- pacing and render size while the session runs -------------------------------------------------------------
    float g_pacedHz = 0.0f;

    void updatePacing(const VcsPresentFrame *f)
    {
        // the game at the headset's rate, the window's limiter / vsync out of the way (xrWaitFrame paces)
        static const bool off = [] { const char *e = std::getenv("VCS_VR_NO_PACING"); return e && *e == '1'; }(); // A/B tests
        if (off)
            return;
        const float hz = vr::refreshHz();
        if (hz > 0.0f && std::fabs(hz - g_pacedHz) > 0.5f && VCS_MOD_API_HAS(mod::api, vr_set_pacing))
        {
            mod::api->vr_set_pacing(hz);
            g_pacedHz = hz;
            vr::log("pacing: the game runs at the headset's %.0f Hz (the frame rate cap and vertical sync are off while VR "
                    "runs)\n", hz);
            g_perf = {};
            g_perfStartNs = nowNs();
        }
        // the picture at the headset's resolution for the angle it covers, x the render scale
        uint32_t rec = 0;
        vr::Fov eye;
        if (!VCS_MOD_API_HAS(mod::api, render_set_output_size) || !g_lens.valid || !vr::eyeView(rec, eye))
            return;
        const double gameSpan = (g_lens.width - g_lens.cx) / g_lens.fx + g_lens.cx / g_lens.fx; // tan right + tan left
        const double eyeSpan = std::tan(eye.right) - std::tan(eye.left);
        const double ww = std::max(1u, f->window_width), wh = std::max(1u, f->window_height);
        double w = rec * (gameSpan / std::max(0.1, eyeSpan)) * g_renderScale.load() / 100.0;
        w = std::clamp(w, 480.0, 4096.0);
        double h = w * wh / ww;
        if (h > 4096.0)
            w *= 4096.0 / h, h = 4096.0;
        const uint32_t ow = static_cast<uint32_t>(std::lround(w)) & ~1u, oh = static_cast<uint32_t>(std::lround(h)) & ~1u;
        if (std::abs(static_cast<int>(ow) - static_cast<int>(g_outW)) > static_cast<int>(g_outW / 50u) ||
            std::abs(static_cast<int>(oh) - static_cast<int>(g_outH)) > static_cast<int>(g_outH / 50u))
        {
            mod::api->render_set_output_size(ow, oh);
            vr::log("render size: %ux%u for the game's %.0f deg of the headset's %.0f deg at %ux per eye, scale %d%% (the "
                    "window is %ux%u)\n", ow, oh, 2.0 * std::atan(gameSpan / 2.0) * 57.2958, (eye.right - eye.left) * 57.2958,
                    rec, g_renderScale.load(), f->window_width, f->window_height);
            g_outW = ow, g_outH = oh;
        }
    }

    void unpace()
    {
        if (g_pacedHz > 0.0f && VCS_MOD_API_HAS(mod::api, vr_set_pacing))
            mod::api->vr_set_pacing(0.0f);
        if (g_outW && VCS_MOD_API_HAS(mod::api, render_set_output_size))
            mod::api->render_set_output_size(0, 0);
        g_pacedHz = 0.0f;
        g_outW = g_outH = 0;
    }

    void setSplit(bool on)
    {
        if (on != g_split && VCS_MOD_API_HAS(mod::api, render_set_split_layers))
            mod::api->render_set_split_layers(on ? 1 : 0);
        g_split = on;
    }

    void stopVr(const char *why)
    {
        sendHeadLook(false);
        setSplit(false);
        unpace();
        if (vr::started() || vr::pending())
        {
            vr::stop();
            vr::log("off (%s)\n", why);
        }
        g_mode = "off";
    }

    void submit(const VcsPresentFrame *f, ID3D11DeviceContext *c)
    {
        vr::ProjectionLayer proj;
        vr::QuadLayer quads[2];
        int nq = 0;
        const char *cinemaWhy = "";
        if (vr::shouldRender())
        {
            vr::Pose renderPose;
            // first person dropping out for a few frames (a frame between camera modes) keeps the head-tracked view: the
            // cinema only after about a third of a second without it
            static int missed = 1000;
            missed = g_headApplied && g_headApplied() != 0 ? 0 : missed + 1;
            const bool headApplied = missed <= 30;
            const bool reasons[6] = {g_view.load() == 0, f->scene != nullptr, f->has_3d != 0, headApplied,
                                     poseFor(f->input_ns, renderPose), !menuOpen()};
            bool head = g_lens.valid;
            int why = g_lens.valid ? -1 : 2;
            for (int i = 0; i < 6; ++i)
            {
                head = head && reasons[i];
                if (!reasons[i])
                {
                    ++g_cinemaWhy[i];
                    if (why < 0)
                        why = i;
                }
            }
            cinemaWhy = why < 0 ? "" : why == 2 && !g_lens.valid ? "no 3D camera seen yet" : kCinemaWhy[why];
            if (head)
            {
                bool srgb = false;
                if (ID3D11Texture2D *img = vr::panelImage(vr::Panel::Scene, f->layer_width, f->layer_height, srgb))
                {
                    blit(c, img, srgb, static_cast<ID3D11Texture2D *>(f->scene), nullptr, f->layer_width, f->layer_height);
                    proj.on = true;
                    proj.pose = renderPose;
                    proj.fov = lensFov(g_lens);
                }
                if (f->has_hud && f->hud_black && f->hud_white)
                    if (ID3D11Texture2D *img = vr::panelImage(vr::Panel::Hud, f->layer_width, f->layer_height, srgb))
                    {
                        blit(c, img, srgb, static_cast<ID3D11Texture2D *>(f->hud_black), static_cast<ID3D11Texture2D *>(f->hud_white),
                             f->layer_width, f->layer_height);
                        const float d = g_hudCm.load() / 100.0f;
                        vr::QuadLayer &q = quads[nq++];
                        q.on = true;
                        q.panel = vr::Panel::Hud;
                        q.pose = ahead(d, d * 0.08f);
                        q.width = d * 1.1f;
                        q.height = q.width * f->layer_height / std::max(1u, f->layer_width);
                        q.alpha = true;
                    }
                g_mode = "head-tracked";
            }
            if (!proj.on)
            {
                // the cinema screen: the window's picture and menus, at the window's shape
                const uint32_t ww = std::max(1u, f->window_width), wh = std::max(1u, f->window_height);
                uint32_t cw = 1600, ch = static_cast<uint32_t>(std::lround(1600.0 * wh / ww));
                if (ch > 1600u)
                {
                    cw = static_cast<uint32_t>(std::lround(1600.0 * ww / wh));
                    ch = 1600u;
                }
                ID3D11Texture2D *src = static_cast<ID3D11Texture2D *>(mod::api->present_window_copy(cw, ch));
                bool srgb = false;
                if (src)
                    if (ID3D11Texture2D *img = vr::panelImage(vr::Panel::Cinema, cw, ch, srgb))
                    {
                        blit(c, img, srgb, src, nullptr, cw, ch);
                        vr::QuadLayer &q = quads[nq++];
                        q.on = true;
                        q.panel = vr::Panel::Cinema;
                        q.pose = ahead(g_cinemaCm.load() / 100.0f);
                        q.width = g_cinemaWidthCm.load() / 100.0f;
                        q.height = q.width * ch / cw;
                    }
                g_mode = "cinema";
            }
        }
        const bool rendered = vr::shouldRender();
        vr::endFrame(proj, quads, nq);
        if (rendered)
        {
            const std::string shown = proj.on ? "head-tracked" : nq ? "cinema" : "nothing";
            (proj.on ? g_headFrames : g_cinemaFrames)++;
            g_status = proj.on ? kHeadTracked : kCinema;
            if (shown != g_lastShown)
            {
                if (proj.on)
                    vr::log("showing: head-tracked picture %ux%u (fov L%.0f R%.0f U%.0f D%.0f deg)%s\n", f->layer_width,
                            f->layer_height, proj.fov.left * 57.3f, proj.fov.right * 57.3f, proj.fov.up * 57.3f,
                            proj.fov.down * 57.3f, nq ? " + HUD panel" : "");
                else
                    vr::log("showing: %s%s%s\n", shown.c_str(), *cinemaWhy ? " - " : "", cinemaWhy);
                g_lastShown = shown;
            }
        }
    }

    void present(void *, const VcsPresentFrame *f)
    {
        if (!g_enabled.load())
        {
            if (vr::started() || vr::pending() || g_split || g_headSent || vr::exited())
                stopVr("switched off");
            vr::clearExited();
            g_failed = false;
            g_retryNs = 0;
            g_status = kOff;
            return;
        }
        if (g_failed)
            return;
        if (f->backend != VCS_RENDER_BACKEND_D3D11 || !f->device || !f->context)
        {
            vr::log("VR needs the D3D11 renderer (Graphics > Renderer): VR stays off, the game runs as usual\n");
            g_failed = true;
            g_status = kNeedsD3D11;
            return;
        }
        if (vr::exited())
        {
            g_status = kEnded;
            sendHeadLook(false);
            setSplit(false);
            return;
        }
        auto *device = static_cast<ID3D11Device *>(f->device);
        auto *context = static_cast<ID3D11DeviceContext *>(f->context);
        if (!vr::started())
        {
            if (nowNs() < g_retryNs)
                return;
            std::string why;
            const bool wasPending = vr::pending();
            if (g_status == kOff)
            {
                g_status = kStarting;
                vr::log("VR switched on: window %ux%u, picture layers %ux%u\n", f->window_width, f->window_height, f->layer_width,
                        f->layer_height);
            }
            if (!vr::start(device, why))
            {
                if (vr::pending())
                {
                    if (!wasPending)
                        vr::log("%s\n", why.c_str());
                    g_retryNs = nowNs() + 3000000000ll;
                    g_status = kNoHeadset;
                }
                else
                {
                    vr::log("%s: VR stays off, the game runs as usual\n", why.c_str());
                    g_failed = true;
                    g_status = why.find("no OpenXR runtime") != std::string::npos || why.find("openxr_loader") != std::string::npos
                                   ? kNoRuntime
                                   : kError;
                }
                return;
            }
            if (!gpuInit(device))
            {
                stopVr("no shaders");
                g_failed = true;
                g_status = kError;
                return;
            }
            vr::log("OpenXR session on the game's D3D11 device (%s)\n", vr::runtimeName().c_str());
        }
        vr::poll();
        if (!vr::running())
        {
            sendHeadLook(false);
            setSplit(false);
            unpace(); // the headset asleep: the game on the monitor at its own settings
            g_lastShown.clear();
            g_mode = vr::started() ? "waiting for the headset" : "off";
            g_status = vr::exited() ? kEnded : vr::started() ? kWaiting : kNoHeadset;
            if (!vr::started())
                g_retryNs = nowNs() + 3000000000ll; // the session ended: start again later
            return;
        }
        setSplit(true);
        updatePacing(f);
        const int64_t t0 = nowNs();
        if (g_lastHostNs)
            g_perf.hostMs += (t0 - g_lastHostNs) / 1e6, g_perf.hostMax = std::max(g_perf.hostMax, (t0 - g_lastHostNs) / 1e6);
        g_lastHostNs = t0;
        ++g_perf.frames;
        if (f->layer_serial != g_lastSerial)
        {
            g_lastSerial = f->layer_serial;
            ++g_perf.newFrames;
        }
        if (vr::frameBegun())
        {
            gpuTimeBegin(context);
            submit(f, context);
            gpuTimeEnd(context);
            context->ClearState(); // the renderer sets everything again for its next pass
        }
        const int64_t t1 = nowNs();
        g_perf.submitMs += (t1 - t0) / 1e6;
        if (g_recenter.exchange(false))
            vr::recenter();
        // the next headset frame: wait for it (paces the host frame), begin it, and turn the game's first-person view
        // to where the head will be when it is shown
        if (vr::waitAndBegin())
        {
            vr::Pose head;
            if (g_view.load() == 0 && vr::locateHead(head))
            {
                float yaw = 0, pitch = 0, roll = 0;
                headAngles(head, yaw, pitch, roll);
                sendHeadLook(true, yaw, pitch, roll);
                if (g_setHeadPosition) // OpenXR: x right, y up, -z ahead (the recentred space: the body's ahead)
                    g_setHeadPosition(head.p[0], -head.p[2], head.p[1]);
                g_poses[g_poseNext++ % 128u] = {nowNs(), head};
            }
            else
                sendHeadLook(false);
        }
        const int64_t t2 = nowNs();
        g_perf.waitMs += (t2 - t1) / 1e6;
        g_perf.waitMax = std::max(g_perf.waitMax, (t2 - t1) / 1e6);
        // every 30 s: what went to the headset
        const int64_t now = t2;
        if (!g_summaryNs)
            g_summaryNs = now;
        else if (now - g_summaryNs > 30000000000ll)
        {
            g_summaryNs = now;
            vr::log("last 30 s: %llu frames head-tracked, %llu cinema (session %s, %.0f Hz); %s\n",
                    static_cast<unsigned long long>(g_headFrames), static_cast<unsigned long long>(g_cinemaFrames),
                    vr::sessionStateName(), vr::refreshHz(), vr::status().c_str());
            vr::log("performance: %s\n", perfLine(true).c_str());
            g_headFrames = g_cinemaFrames = 0;
        }
    }
    void enable(bool on)
    {
        g_enabled = on;
        if (on && !g_hooked && VCS_MOD_API_HAS(mod::api, present_window_copy))
        {
            g_hooked = mod::api->add_present_hook(&present, nullptr) != 0;
            mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &renderHook, nullptr, 0u);
        }
    }

    void recenterKey(void *) { g_recenter = true; }

    mod::Choice cm(const char *label, int value) { return {label, value}; }
}

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "VR"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION)
            return 0;
        mod::api = a;
        if (!VCS_MOD_API_HAS(a, present_window_copy))
        {
            a->log("[VR] this vcs.exe has no present hook (an older build): VR.asi stays inert\n");
            return 1;
        }
        g_imgui = imguiMatches(a);
        if (g_imgui)
        {
            void *allocFn = nullptr, *freeFn = nullptr, *user = nullptr;
            a->imgui_allocators(&allocFn, &freeFn, &user);
            if (allocFn && freeFn)
                ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFn), reinterpret_cast<ImGuiMemFreeFunc>(freeFn),
                                             user);
        }

        VcsModHotkey key = {};
        key.size = sizeof(key);
        key.key = "controls.vr_recenter_key";
        key.label = "VR: recentre";
        key.help = "Sets the headset's forward to where you look now (Graphics > VR). On a gamepad: Back + L3.";
        key.default_key = 296; // F7 (platform::KEY_F7)
        key.pad_chord = VCS_MOD_PAD_LEFT_STICK;
        key.pressed = &recenterKey;
        a->add_hotkey(&key);

        mod::addCommand("vr", [](const std::string &args, mod::Reply reply) {
            if (args == "perf")
            {
                reply("ok " + perfLine(false));
                return;
            }
            if (args == "perf reset")
            {
                perfLine(true);
                reply("ok reset");
                return;
            }
            if (args == "recenter")
            {
                g_recenter = true;
                reply("ok recentre");
                return;
            }
            char b[512];
            std::snprintf(b, sizeof(b),
                          "ok status=%d enabled=%d failed=%d mode=%s split=%d head=%d lens=%d cinema_by=view:%llu,layers:%llu,no3d:%llu,"
                          "nohead:%llu,nopose:%llu,menu:%llu %s",
                          g_status.load(), g_enabled.load() ? 1 : 0, g_failed ? 1 : 0, g_mode.c_str(), g_split ? 1 : 0, g_headSent ? 1 : 0,
                          g_lens.valid ? 1 : 0, (unsigned long long)g_cinemaWhy[0], (unsigned long long)g_cinemaWhy[1],
                          (unsigned long long)g_cinemaWhy[2], (unsigned long long)g_cinemaWhy[3], (unsigned long long)g_cinemaWhy[4],
                          (unsigned long long)g_cinemaWhy[5], vr::status().c_str());
            reply(b);
        });
        vr::log("VR.asi loaded (built %s %s; Graphics > VR, off by default)\n", __DATE__, __TIME__);
        return 1;
    }

    // Graphics > VR (after the renderer's rows)
    VCS_MOD_EXPORT int VcsModRegisterSettings(const VcsModApi *a, const char *slot)
    {
        if (!a || !slot || std::string(slot) != "graphics" || !VCS_MOD_API_HAS(a, present_window_copy))
            return 1;
        mod::Spec on;
        on.key = "vr.enabled";
        on.page = "Graphics";
        on.section = "VR";
        on.label = "VR (OpenXR headset)";
        on.help = "Shows the game in an OpenXR headset (SteamVR, Quest Link, Virtual Desktop, WMR ...): first person on foot "
                  "follows your head, everything else is on a screen in front of you. Mono for now. VCS_VR=0|1.";
        on.type = mod::Type::Toggle;
        on.env = "VCS_VR";
        on.onChange = [](int v) { enable(v != 0); };
        on.read = [] { return g_enabled.load() ? 1 : 0; };
        mod::add(on);

        // a readout, not a choice: read() gives what VR is doing now (changing it does nothing)
        mod::Spec status;
        status.key = "vr.status";
        status.page = "Graphics";
        status.section = "VR";
        status.label = "VR status";
        status.help = "What VR is doing now (a readout). Details: build\\run\\vr.log - send that file when VR does not work.";
        status.type = mod::Type::Choice;
        status.choices = {cm("Off", kOff),
                          cm("Starting...", kStarting),
                          cm("No OpenXR runtime (start SteamVR / Quest Link / Virtual Desktop)", kNoRuntime),
                          cm("No headset found - retrying", kNoHeadset),
                          cm("Waiting for the headset (asleep / not worn?)", kWaiting),
                          cm("Running: head-tracked", kHeadTracked),
                          cm("Running: cinema screen", kCinema),
                          cm("Error - see build\\run\\vr.log", kError),
                          cm("Needs the D3D11 renderer", kNeedsD3D11),
                          cm("Ended by the runtime - switch VR off and on", kEnded)};
        status.onChange = [](int) {};
        status.read = [] { return g_status.load(); };
        mod::add(status);

        mod::Spec view;
        view.key = "vr.view";
        view.page = "Graphics";
        view.section = "VR";
        view.label = "VR view";
        view.help = "Head-tracked: first person on foot (CameraAim, key B) looks where your head does, the HUD on a panel "
                    "below. Cinema: always the flat screen.";
        view.type = mod::Type::Choice;
        view.choices = {cm("Head-tracked", 0), cm("Cinema screen", 1)};
        view.onChange = [](int v) { g_view = v; };
        mod::add(view);

        mod::Spec scale;
        scale.key = "vr.render_scale";
        scale.page = "Graphics";
        scale.section = "VR";
        scale.label = "VR render scale";
        scale.help = "The picture's resolution against the headset's own (100%: as sharp as the headset shows it). Lower is "
                     "faster. The 3D itself follows it with Internal resolution = Match window; otherwise only the HUD and "
                     "the picture's scaling do.";
        scale.type = mod::Type::Choice;
        scale.choices = {cm("50%", 50), cm("60%", 60), cm("70%", 70), cm("80%", 80), cm("90%", 90), cm("100%", 100),
                         cm("120%", 120), cm("150%", 150)};
        scale.defaultValue = 100;
        scale.onChange = [](int v) { g_renderScale = v; };
        mod::add(scale);

        mod::Spec dist;
        dist.key = "vr.cinema_distance";
        dist.page = "Graphics";
        dist.section = "VR";
        dist.label = "VR cinema distance";
        dist.help = "How far the cinema screen (menus, cutscenes, third person, vehicles) stands in front of you.";
        dist.type = mod::Type::Choice;
        dist.choices = {cm("2 m", 200), cm("3 m", 300), cm("4 m", 400), cm("6 m", 600), cm("8 m", 800), cm("12 m", 1200)};
        dist.defaultValue = 400;
        dist.onChange = [](int v) { g_cinemaCm = v; };
        mod::add(dist);

        mod::Spec size;
        size.key = "vr.cinema_size";
        size.page = "Graphics";
        size.section = "VR";
        size.label = "VR cinema width";
        size.help = "The cinema screen's width.";
        size.type = mod::Type::Choice;
        size.choices = {cm("2 m", 200), cm("3 m", 300), cm("4 m", 400), cm("5 m", 500), cm("6 m", 600), cm("8 m", 800),
                        cm("10 m", 1000), cm("15 m", 1500)};
        size.defaultValue = 500;
        size.onChange = [](int v) { g_cinemaWidthCm = v; };
        mod::add(size);

        mod::Spec hud;
        hud.key = "vr.hud_distance";
        hud.page = "Graphics";
        hud.section = "VR";
        hud.label = "VR HUD distance";
        hud.help = "How far the HUD panel stands in front of you in the head-tracked view (its size follows: the same angle).";
        hud.type = mod::Type::Choice;
        hud.choices = {cm("0.75 m", 75), cm("1 m", 100), cm("1.5 m", 150), cm("2 m", 200), cm("3 m", 300)};
        hud.defaultValue = 150;
        hud.onChange = [](int v) { g_hudCm = v; };
        mod::add(hud);
        return 1;
    }
}
