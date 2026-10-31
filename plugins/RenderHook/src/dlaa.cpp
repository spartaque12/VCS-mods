// See renderhook.h: DLAA - NVIDIA's DLSS at native resolution, through NGX in the game's own process (vcs.exe is 64-bit;
// the user's RHDLAA for SA needed a 64-bit helper). Inputs at the render hook (after the 3D, before the game's post
// effects): the picture as the earlier hooks left it (ambient occlusion included), a reversed depth and the motion vectors
// (shaders/temporal.hlsl: the renderer's per-object motion, the camera's reprojection elsewhere), the frame's jitter
// (render_set_jitter, an 8-point Halton sequence while DLAA is on). The result is drawn back into the picture. D3D11 only
// (render_native gives no Vulkan objects yet); with MSAA on it still works (the picture resolved, depth sample 0) but MSAA
// is then wasted work.
#include "renderhook.h"

#include "mod.h"

#include <windows.h>
#include <d3d11.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_helpers.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhDlaaDepthPS.h"
#include "dxbc/kRhDlaaDepthMsPS.h"
#include "dxbc/kRhDlaaMotionPS.h"
#include "dxbc/kRhDlaaMotionMsPS.h"
#include "dxbc/kRhCopyPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        constexpr uint32_t kJitterPhases = 8; // DLSS: 8 x (output / render)^2 at native resolution
        constexpr float kFarthest = 100000.0f;

        int g_mode = 0;         // renderhook.dlaa: 0 off, 1 on
        int g_preset = 0;       // renderhook.dlaa_preset: DLSS's render preset for DLAA (0 = its default)
        float g_jitterSign = 1.0f, g_mvSign = 1.0f; // VCS_RH_DLAA_JITTER_SIGN / _MV_SIGN = -1 (diagnostics)
        bool g_hooked = false;
        std::string g_status = "off";
        uint64_t g_frames = 0, g_failed = 0;

        struct Gpu
        {
            VcsRenderPipeline *depth[2] = {}, *motion[2] = {}, *back = nullptr;
            VcsRenderBuffer *constants = nullptr;
            VcsRenderTexture *depthTex = nullptr, *motionTex = nullptr, *output = nullptr;
            uint32_t width = 0, height = 0;
            bool ready = false, failed = false;
        };
        Gpu g_gpu;

        struct Ngx
        {
            bool tried = false, ok = false;
            ID3D11Device *device = nullptr;
            NVSDK_NGX_Parameter *params = nullptr;
            NVSDK_NGX_Handle *feature = nullptr;
            uint32_t width = 0, height = 0;
            int preset = -1;
            bool reset = true;
        };
        Ngx g_ngx;

        struct Constants // shaders/temporal.hlsl cbuffer Temporal
        {
            float q[16], zmap[4], jitter[4];
            int32_t size[4];
        };

        void fail(const char *what, unsigned code = 0)
        {
            char text[200];
            std::snprintf(text, sizeof(text), code ? "%s (0x%08X)" : "%s", what, code);
            if (g_status != text)
                mod::log("[renderhook] DLAA: %s\n", text);
            g_status = text;
        }

        std::wstring moduleFolder()
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&moduleFolder), &self);
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(self, path, MAX_PATH);
            if (wchar_t *slash = wcsrchr(path, L'\\'))
                *slash = 0;
            return path;
        }

        bool ngxInit(ID3D11Device *device)
        {
            if (g_ngx.tried)
                return g_ngx.ok && g_ngx.device == device;
            g_ngx.tried = true;
            g_ngx.device = device;
            static std::wstring folder = moduleFolder(); // nvngx_dlss.dll lives beside RenderHook.asi
            static const wchar_t *paths[] = {folder.c_str()};
            NVSDK_NGX_FeatureCommonInfo info = {};
            info.PathListInfo.Path = paths;
            info.PathListInfo.Length = 1;
            NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("0f6bd2a4-6c1e-4a53-9a9e-5b7c3d2e8f10", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                     "1.0", folder.c_str(), device, &info);
            if (NVSDK_NGX_FAILED(r))
            {
                fail("NGX could not start - an NVIDIA RTX card and a current driver are needed", r);
                return false;
            }
            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&g_ngx.params)) || !g_ngx.params)
            {
                fail("NGX capability parameters failed");
                return false;
            }
            int available = 0, needsDriver = 0;
            NVSDK_NGX_Parameter_GetI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            if (needsDriver)
            {
                fail("DLSS needs a newer NVIDIA driver");
                return false;
            }
            if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
                !available)
            {
                int code = 0;
                NVSDK_NGX_Parameter_GetI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &code);
                fail("DLSS not available (an RTX card and nvngx_dlss.dll beside RenderHook.asi are needed)", static_cast<unsigned>(code));
                return false;
            }
            g_ngx.ok = true;
            mod::log("[renderhook] DLAA: NGX ready\n");
            return true;
        }

        bool ngxFeature(ID3D11DeviceContext *context, uint32_t width, uint32_t height)
        {
            if (g_ngx.feature && g_ngx.width == width && g_ngx.height == height && g_ngx.preset == g_preset)
                return true;
            if (g_ngx.feature)
            {
                NVSDK_NGX_D3D11_ReleaseFeature(g_ngx.feature);
                g_ngx.feature = nullptr;
            }
            NVSDK_NGX_Parameter_SetUI(g_ngx.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<unsigned>(g_preset));
            NVSDK_NGX_DLSS_Create_Params create = {};
            create.Feature.InWidth = create.Feature.InTargetWidth = width;
            create.Feature.InHeight = create.Feature.InTargetHeight = height;
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &g_ngx.feature, g_ngx.params, &create);
            if (NVSDK_NGX_FAILED(r))
            {
                g_ngx.feature = nullptr;
                fail("creating the DLSS feature failed", r);
                return false;
            }
            g_ngx.width = width;
            g_ngx.height = height;
            g_ngx.preset = g_preset;
            g_ngx.reset = true;
            mod::log("[renderhook] DLAA: %ux%u, preset %d\n", width, height, g_preset);
            return true;
        }

        VcsRenderPipeline *pipeline(const void *ps, size_t psSize, uint32_t mask, uint32_t multisampled, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = kRhFullScreenVS;
            d.vertex_size = sizeof(kRhFullScreenVS);
            d.pixel_code = ps;
            d.pixel_size = psSize;
            d.blend = VCS_RENDER_BLEND_OPAQUE;
            d.write_mask = mask;
            d.multisampled_textures = multisampled;
            d.name = name;
            return mod::api->render_create_pipeline(&d);
        }

        bool gpuReady(const VcsRenderFrame &f)
        {
            const VcsModApi &a = *mod::api;
            if (g_gpu.failed)
                return false;
            if (!g_gpu.ready)
            {
                g_gpu.depth[0] = pipeline(kRhDlaaDepthPS, sizeof(kRhDlaaDepthPS), 0xFu, 0u, "renderhook dlaa depth");
                g_gpu.depth[1] = pipeline(kRhDlaaDepthMsPS, sizeof(kRhDlaaDepthMsPS), 0xFu, 1u, "renderhook dlaa depth (MSAA)");
                g_gpu.motion[0] = pipeline(kRhDlaaMotionPS, sizeof(kRhDlaaMotionPS), 0xFu, 0u, "renderhook dlaa motion");
                g_gpu.motion[1] = pipeline(kRhDlaaMotionMsPS, sizeof(kRhDlaaMotionMsPS), 0xFu, 1u, "renderhook dlaa motion (MSAA)");
                g_gpu.back = pipeline(kRhCopyPS, sizeof(kRhCopyPS), 0x7u, 0u, "renderhook dlaa result");
                g_gpu.constants = a.render_create_constants(sizeof(Constants), "renderhook dlaa constants");
                g_gpu.ready = g_gpu.depth[0] && g_gpu.depth[1] && g_gpu.motion[0] && g_gpu.motion[1] && g_gpu.back && g_gpu.constants;
                if (!g_gpu.ready)
                {
                    g_gpu.failed = true;
                    fail("the renderer refused DLAA's pipelines");
                    return false;
                }
            }
            if (g_gpu.width != f.width || g_gpu.height != f.height || !g_gpu.output)
            {
                for (VcsRenderTexture *t : {g_gpu.depthTex, g_gpu.motionTex, g_gpu.output})
                    if (t)
                        a.render_destroy_texture(t);
                g_gpu.depthTex = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_R32F, "renderhook dlaa depth");
                g_gpu.motionTex = a.render_create_texture_ex(f.width, f.height, VCS_RENDER_FORMAT_RGBA16F, 0u, "renderhook dlaa motion");
                g_gpu.output = a.render_create_texture_ex(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, VCS_RENDER_TEXTURE_STORAGE,
                                                          "renderhook dlaa output");
                g_gpu.width = f.width;
                g_gpu.height = f.height;
                g_ngx.reset = true;
            }
            return g_gpu.depthTex && g_gpu.motionTex && g_gpu.output;
        }

        void pass(VcsRenderTexture *target, VcsRenderPipeline *p, const VcsRenderFrame &f, VcsRenderTexture *t0, VcsRenderTexture *t2,
                  VcsRenderTexture *t3)
        {
            const VcsModApi &a = *mod::api;
            a.render_begin_pass(target);
            a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
            a.render_set_pipeline(p);
            a.render_set_constants(0, g_gpu.constants);
            if (t0)
                a.render_set_texture(0, t0, VCS_RENDER_SAMPLER_POINT);
            if (t2)
                a.render_set_texture(2, t2, VCS_RENDER_SAMPLER_POINT);
            if (t3)
                a.render_set_texture(3, t3, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
        }

        void hook(void *, const VcsRenderFrame *f)
        {
            if (!g_mode || !f || f->size < offsetof(VcsRenderFrame, velocity) + sizeof(f->velocity))
                return;
            if (f->backend != VCS_RENDER_BACKEND_D3D11)
            {
                fail("D3D11 only for now");
                return;
            }
            if (!f->depth || !f->camera_valid || !f->width || !f->height || !gpuReady(*f))
                return;
            const VcsModApi &a = *mod::api;
            auto *device = static_cast<ID3D11Device *>(a.render_native(VCS_RENDER_NATIVE_DEVICE, nullptr));
            auto *context = static_cast<ID3D11DeviceContext *>(a.render_native(VCS_RENDER_NATIVE_CONTEXT, nullptr));
            if (!device || !context || !ngxInit(device) || !ngxFeature(context, f->width, f->height))
            {
                ++g_failed;
                return;
            }
            if (!f->reproject_valid)
                g_ngx.reset = true; // no motion known: start the history again
            Constants c = {};
            std::memcpy(c.q, f->reproject, sizeof(c.q));
            c.zmap[0] = f->depth_a;
            c.zmap[1] = f->depth_b;
            c.zmap[2] = f->depth_scale;
            c.zmap[3] = kFarthest;
            c.jitter[0] = f->jitter_x;
            c.jitter[1] = f->jitter_y;
            c.jitter[2] = f->prev_jitter_x;
            c.jitter[3] = f->prev_jitter_y;
            c.size[0] = static_cast<int32_t>(f->width);
            c.size[1] = static_cast<int32_t>(f->height);
            c.size[3] = f->velocity ? 1 : 0;
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));
            a.render_push_marker("renderhook dlaa");
            const int ms = f->depth_samples > 1u ? 1 : 0;
            pass(g_gpu.depthTex, g_gpu.depth[ms], *f, f->depth, nullptr, nullptr);
            pass(g_gpu.motionTex, g_gpu.motion[ms], *f, f->depth, nullptr, f->velocity);
            VcsRenderTexture *color = a.render_current_color(); // with the ambient occlusion drawn before
            auto *inColor = static_cast<ID3D11Resource *>(a.render_native(VCS_RENDER_NATIVE_TEXTURE, color));
            auto *inDepth = static_cast<ID3D11Resource *>(a.render_native(VCS_RENDER_NATIVE_TEXTURE, g_gpu.depthTex));
            auto *inMotion = static_cast<ID3D11Resource *>(a.render_native(VCS_RENDER_NATIVE_TEXTURE, g_gpu.motionTex));
            auto *out = static_cast<ID3D11Resource *>(a.render_native(VCS_RENDER_NATIVE_TEXTURE, g_gpu.output));
            if (!inColor || !inDepth || !inMotion || !out)
            {
                a.render_pop_marker();
                fail("no native textures");
                return;
            }
            NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
            eval.Feature.pInColor = inColor;
            eval.Feature.pInOutput = out;
            eval.pInDepth = inDepth;
            eval.pInMotionVectors = inMotion;
            eval.InJitterOffsetX = g_jitterSign * f->jitter_x;
            eval.InJitterOffsetY = g_jitterSign * f->jitter_y;
            eval.InRenderSubrectDimensions = {f->width, f->height};
            eval.InReset = g_ngx.reset ? 1 : 0;
            eval.InMVScaleX = g_mvSign;
            eval.InMVScaleY = g_mvSign;
            eval.InPreExposure = 1.0f;
            eval.InExposureScale = 1.0f;
            const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(context, g_ngx.feature, g_ngx.params, &eval);
            if (NVSDK_NGX_FAILED(r))
            {
                a.render_pop_marker();
                ++g_failed;
                fail("DLSS evaluation failed", r);
                return;
            }
            g_ngx.reset = false;
            pass(f->color, g_gpu.back, *f, nullptr, g_gpu.output, nullptr);
            a.render_pop_marker();
            ++g_frames;
            g_status = "on";
        }

        void setMode(int mode)
        {
            if (!VCS_MOD_API_HAS(mod::api, render_create_texture_ex) || mode == g_mode)
                return;
            g_mode = mode;
            mod::api->render_set_jitter(mode ? kJitterPhases : 0u); // off: no jitter (VCS_RH_JITTER's own is set at start)
            if (mode && !g_hooked)
            {
                hookAo();
                if (shadowsOn())
                    hookShadows();
                g_hooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &hook, nullptr,
                                                     VCS_RENDER_NEEDS_DEPTH | VCS_RENDER_NEEDS_VELOCITY) != 0;
            }
            g_ngx.reset = true;
            if (!mode)
                g_status = "off";
        }
    }

    void registerDlaa()
    {
        if (const char *e = std::getenv("VCS_RH_DLAA_JITTER_SIGN"); e && std::atof(e) < 0.0)
            g_jitterSign = -1.0f;
        if (const char *e = std::getenv("VCS_RH_DLAA_MV_SIGN"); e && std::atof(e) < 0.0)
            g_mvSign = -1.0f;
        // rhdlaa -> status: on / off / why not, frames, failures
        mod::addCommand("rhdlaa", [](const std::string &, mod::Reply reply) {
            char line[300];
            std::snprintf(line, sizeof(line), "ok mode %d preset %d hooked %d frames %llu failed %llu status %s", g_mode, g_preset,
                          g_hooked ? 1 : 0, static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_failed),
                          g_status.c_str());
            reply(line);
        });
    }

    void registerDlaaSettings()
    {
        if (!VCS_MOD_API_HAS(mod::api, render_create_texture_ex))
            return; // this vcs.exe cannot give NGX its device
        mod::Spec s;
        s.key = "renderhook.dlaa";
        s.page = "Graphics";
        s.section = "Anti-aliasing (RenderHook)";
        s.label = "DLAA (NVIDIA)";
        s.help = "NVIDIA's DLSS at native resolution: smooth, stable edges through motion, using the game's depth and motion "
                 "vectors. Needs an RTX card. Turn MSAA off with it (MSAA is then wasted work). D3D11.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off", 0}, {"On", 1}};
        s.defaultValue = 0;
        s.env = "VCS_RH_DLAA";
        s.onChange = [](int v) { setMode(v != 0 ? 1 : 0); };
        mod::add(s);

        s = {};
        s.key = "renderhook.dlaa_preset";
        s.page = "Graphics";
        s.section = "Anti-aliasing (RenderHook)";
        s.label = "DLAA preset";
        s.help = "DLSS's render preset (the model): Default lets the DLSS version choose; J and K are the transformer models.";
        s.type = mod::Type::Choice;
        s.choices = {{"Default", 0}, {"F", 6}, {"J", 10}, {"K", 11}};
        s.defaultValue = 0;
        s.onChange = [](int v) { g_preset = v; };
        mod::add(s);
    }
}
