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
    std::atomic<bool> g_recenter{false};    // the hotkey asked for it

    bool g_hooked = false;                  // the present and render hooks are in
    bool g_failed = false;                  // no runtime / headset / D3D11: inert until VR is switched off and on again
    bool g_split = false;                   // the renderer splits the HUD off
    int64_t g_retryNs = 0;                  // a headset that is off: try again then
    std::string g_mode = "off";             // what the headset shows (status)
    // frames on the cinema screen by why (status): cinema chosen, no layers, no 3D, no head look in the game, no pose for
    // the shown frame, a menu open
    uint64_t g_cinemaWhy[6] = {};

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- CameraAim's head look (its exports) ------------------------------------------------------------------------
    using SetHeadLookFn = void (*)(int, float, float, float);
    using HeadLookAppliedFn = int (*)();
    SetHeadLookFn g_setHeadLook = nullptr;
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
        }
        static bool reported = false;
        if ((!g_setHeadLook || !g_headApplied) && !reported)
        {
            reported = true;
            mod::log("[VR] CameraAim.asi with the head look not found: the headset shows the cinema screen only\n");
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
            mod::log("[VR] could not create the copy shaders\n");
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
        if (vr::started() || vr::pending())
        {
            vr::stop();
            mod::log("[VR] off (%s)\n", why);
        }
        g_mode = "off";
    }

    void submit(const VcsPresentFrame *f, ID3D11DeviceContext *c)
    {
        vr::ProjectionLayer proj;
        vr::QuadLayer quads[2];
        int nq = 0;
        if (vr::shouldRender())
        {
            vr::Pose renderPose;
            const bool headApplied = g_headApplied && g_headApplied() != 0;
            const bool reasons[6] = {g_view.load() == 0, f->scene != nullptr, f->has_3d != 0, headApplied,
                                     poseFor(f->input_ns, renderPose), !menuOpen()};
            bool head = g_lens.valid;
            for (int i = 0; i < 6; ++i)
            {
                head = head && reasons[i];
                if (!reasons[i])
                    ++g_cinemaWhy[i];
            }
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
        vr::endFrame(proj, quads, nq);
    }

    void present(void *, const VcsPresentFrame *f)
    {
        if (!g_enabled.load())
        {
            if (vr::started() || vr::pending() || g_split || g_headSent)
                stopVr("switched off");
            g_failed = false;
            return;
        }
        if (g_failed)
            return;
        if (f->backend != VCS_RENDER_BACKEND_D3D11 || !f->device || !f->context)
        {
            mod::log("[VR] VR needs the D3D11 renderer (Graphics > Renderer): VR stays off, the game runs as usual\n");
            g_failed = true;
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
            if (!vr::start(device, why))
            {
                if (vr::pending())
                {
                    if (!wasPending)
                        mod::log("[VR] %s\n", why.c_str());
                    g_retryNs = nowNs() + 3000000000ll;
                }
                else
                {
                    mod::log("[VR] %s: VR stays off, the game runs as usual\n", why.c_str());
                    g_failed = true;
                }
                return;
            }
            if (!gpuInit(device))
            {
                stopVr("no shaders");
                g_failed = true;
                return;
            }
            mod::log("[VR] OpenXR session on the game's D3D11 device (%s)\n", vr::runtimeName().c_str());
        }
        vr::poll();
        if (!vr::running())
        {
            sendHeadLook(false);
            setSplit(false);
            g_mode = vr::started() ? "waiting for the headset" : "off";
            if (!vr::started())
                g_retryNs = nowNs() + 3000000000ll; // the session ended: start again later
            return;
        }
        setSplit(true);
        if (vr::frameBegun())
        {
            submit(f, context);
            context->ClearState(); // the renderer sets everything again for its next pass
        }
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
                g_poses[g_poseNext++ % 128u] = {nowNs(), head};
            }
            else
                sendHeadLook(false);
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
            if (args == "recenter")
            {
                g_recenter = true;
                reply("ok recentre");
                return;
            }
            char b[512];
            std::snprintf(b, sizeof(b),
                          "ok enabled=%d failed=%d mode=%s split=%d head=%d lens=%d cinema_by=view:%llu,layers:%llu,no3d:%llu,"
                          "nohead:%llu,nopose:%llu,menu:%llu %s",
                          g_enabled.load() ? 1 : 0, g_failed ? 1 : 0, g_mode.c_str(), g_split ? 1 : 0, g_headSent ? 1 : 0,
                          g_lens.valid ? 1 : 0, (unsigned long long)g_cinemaWhy[0], (unsigned long long)g_cinemaWhy[1],
                          (unsigned long long)g_cinemaWhy[2], (unsigned long long)g_cinemaWhy[3], (unsigned long long)g_cinemaWhy[4],
                          (unsigned long long)g_cinemaWhy[5], vr::status().c_str());
            reply(b);
        });
        mod::log("[VR] VR.asi loaded (Graphics > VR; off by default)\n");
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
