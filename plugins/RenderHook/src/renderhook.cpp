// See renderhook.h. Everything here runs on the main thread: the settings' callbacks, the control command and the render
// hook (inside the renderer's frame, VCS_RENDER_BEFORE_POST: the 3D scene drawn, the game's post effects next).
//
// Ambient occlusion, three full-screen passes (shaders/gtao.hlsl): the Z buffer to view depth (R32F), GTAO visibility
// (RGBA8, noisy by design: 2 slices per pixel rotated in a 4x4 pattern), then a depth-aware 4x4 blur multiplied into the
// picture. The camera and the Z buffer's mapping come from the renderer (VcsRenderFrame), which takes them from the
// frame's own draws.
#include "renderhook.h"

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhViewDepthPS.h"
#include "dxbc/kRhViewDepthMsPS.h"
#include "dxbc/kRhGtaoPS.h"
#include "dxbc/kRhApplyPS.h"
#include "dxbc/kRhShowPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhViewDepthPS.h"
#include "spirv/kRhViewDepthMsPS.h"
#include "spirv/kRhGtaoPS.h"
#include "spirv/kRhApplyPS.h"
#include "spirv/kRhShowPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        // renderhook.ao: the visibility's strength (0..1) and power per level - 1 subtle, 2 normal, 3 strong
        struct Level
        {
            float strength, power;
        };
        constexpr Level kLevels[4] = {{0.0f, 1.0f}, {0.6f, 1.0f}, {1.0f, 1.5f}, {1.0f, 2.2f}};
        // past these view depths (metres) the occlusion fades out; the game's fog takes over (and lowers it before)
        constexpr float kFadeStart = 120.0f, kFadeEnd = 350.0f;
        constexpr float kFarthest = 100000.0f;  // the Z buffer's range used (beyond: nothing)
        constexpr float kLargestRadius = 0.15f; // on screen, as a share of the picture's height

        std::atomic<int> g_level{0}, g_radiusCm{120}, g_view{0};
        bool g_hooked = false;
        uint64_t g_frames = 0, g_skipped = 0;
        VcsRenderFrame g_last = {}; // the latest frame the hook saw (the status command)
        std::string g_problem;      // why the passes do not run (the status command, the log once)

        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Shaders
        {
            Shader vs, viewDepth, viewDepthMs, gtao, apply, show;
        };

        bool shadersFor(int backend, Shaders &s)
        {
#if defined(VCS_MOD_HAVE_DXBC)
            if (backend == VCS_RENDER_BACKEND_D3D11)
            {
                s = {{kRhFullScreenVS, sizeof(kRhFullScreenVS)}, {kRhViewDepthPS, sizeof(kRhViewDepthPS)},
                     {kRhViewDepthMsPS, sizeof(kRhViewDepthMsPS)}, {kRhGtaoPS, sizeof(kRhGtaoPS)},
                     {kRhApplyPS, sizeof(kRhApplyPS)}, {kRhShowPS, sizeof(kRhShowPS)}};
                return true;
            }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
            if (backend == VCS_RENDER_BACKEND_VULKAN)
            {
                s = {{kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)}, {kRhViewDepthPS_spv, sizeof(kRhViewDepthPS_spv)},
                     {kRhViewDepthMsPS_spv, sizeof(kRhViewDepthMsPS_spv)}, {kRhGtaoPS_spv, sizeof(kRhGtaoPS_spv)},
                     {kRhApplyPS_spv, sizeof(kRhApplyPS_spv)}, {kRhShowPS_spv, sizeof(kRhShowPS_spv)}};
                return true;
            }
#endif
            (void)backend;
            (void)s;
            return false;
        }

        struct Gpu
        {
            int backend = -1;
            VcsRenderPipeline *viewDepth[2] = {}; // single-sampled / multisampled Z buffer
            VcsRenderPipeline *gtao = nullptr, *apply = nullptr, *show = nullptr;
            VcsRenderBuffer *constants = nullptr;
            VcsRenderTexture *depth = nullptr, *raw = nullptr; // view depth, the noisy visibility
            uint32_t width = 0, height = 0;
            bool failed = false;
        };
        Gpu g_gpu;

        struct Constants // shaders/gtao.hlsl cbuffer Ao
        {
            float proj[4], zmap[4], params[4], fade[4];
            int32_t size[4];
        };

        void fail(const char *why)
        {
            if (g_problem.empty())
                mod::log("[renderhook] ambient occlusion off: %s\n", why);
            g_problem = why;
        }

        VcsRenderPipeline *pipeline(const Shader &vs, const Shader &ps, int blend, uint32_t mask, uint32_t multisampled, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = vs.code;
            d.vertex_size = vs.size;
            d.pixel_code = ps.code;
            d.pixel_size = ps.size;
            d.blend = blend;
            d.write_mask = mask;
            d.multisampled_textures = multisampled;
            d.name = name;
            return mod::api->render_create_pipeline(&d);
        }

        bool ready(const VcsRenderFrame &f)
        {
            const VcsModApi &a = *mod::api;
            if (g_gpu.failed)
                return false;
            if (g_gpu.backend != f.backend)
            {
                g_gpu.backend = f.backend;
                Shaders s;
                if (!shadersFor(f.backend, s))
                {
                    g_gpu.failed = true;
                    fail("this build of RenderHook.asi has no shaders for the graphics API in use");
                    return false;
                }
                g_gpu.viewDepth[0] = pipeline(s.vs, s.viewDepth, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook view depth");
                g_gpu.viewDepth[1] = pipeline(s.vs, s.viewDepthMs, VCS_RENDER_BLEND_OPAQUE, 0xFu, 1u, "renderhook view depth (MSAA)");
                g_gpu.gtao = pipeline(s.vs, s.gtao, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook gtao");
                g_gpu.apply = pipeline(s.vs, s.apply, VCS_RENDER_BLEND_MULTIPLY, 0x7u, 0u, "renderhook ao apply");
                g_gpu.show = pipeline(s.vs, s.show, VCS_RENDER_BLEND_OPAQUE, 0x7u, 0u, "renderhook ao view");
                g_gpu.constants = a.render_create_constants(sizeof(Constants), "renderhook constants");
                if (!g_gpu.viewDepth[0] || !g_gpu.viewDepth[1] || !g_gpu.gtao || !g_gpu.apply || !g_gpu.show || !g_gpu.constants)
                {
                    g_gpu.failed = true;
                    fail("the renderer refused its pipelines");
                    return false;
                }
            }
            if (g_gpu.width != f.width || g_gpu.height != f.height || !g_gpu.depth || !g_gpu.raw)
            {
                if (g_gpu.depth)
                    a.render_destroy_texture(g_gpu.depth);
                if (g_gpu.raw)
                    a.render_destroy_texture(g_gpu.raw);
                g_gpu.depth = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_R32F, "renderhook view depth");
                g_gpu.raw = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, "renderhook ao");
                g_gpu.width = f.width;
                g_gpu.height = f.height;
                if (!g_gpu.depth || !g_gpu.raw)
                {
                    fail("the renderer refused its textures");
                    return false;
                }
            }
            return true;
        }

        // one full-screen pass over the picture's area of `target`, reading t0 (and t1)
        void pass(VcsRenderTexture *target, VcsRenderPipeline *p, const VcsRenderFrame &f, VcsRenderTexture *t0, VcsRenderTexture *t1)
        {
            const VcsModApi &a = *mod::api;
            a.render_begin_pass(target);
            a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
            a.render_set_pipeline(p);
            a.render_set_constants(0, g_gpu.constants);
            a.render_set_texture(0, t0, VCS_RENDER_SAMPLER_POINT);
            if (t1)
                a.render_set_texture(1, t1, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
        }

        void hook(void *, const VcsRenderFrame *f)
        {
            const int level = g_level.load(std::memory_order_relaxed);
            const bool view = g_view.load(std::memory_order_relaxed) != 0;
            if ((!level && !view) || !f || f->size < offsetof(VcsRenderFrame, fog_scale) + sizeof(f->fog_scale))
                return;
            g_last = *f;
            if (f->backend == VCS_RENDER_BACKEND_NULL || !f->depth || !f->camera_valid || !f->width || !f->height || !ready(*f))
            {
                ++g_skipped;
                return;
            }
            const Level &lv = kLevels[std::clamp(level, 1, 3)]; // the debug view on its own shows "normal"
            Constants c = {};
            c.proj[0] = f->fx;
            c.proj[1] = f->fy;
            c.proj[2] = f->cx;
            c.proj[3] = f->cy;
            c.zmap[0] = f->depth_a;
            c.zmap[1] = f->depth_b;
            c.zmap[2] = f->depth_scale;
            c.zmap[3] = kFarthest;
            c.params[0] = static_cast<float>(g_radiusCm.load(std::memory_order_relaxed)) / 100.0f;
            c.params[1] = lv.strength;
            c.params[2] = lv.power;
            c.params[3] = kLargestRadius * static_cast<float>(f->height);
            c.fade[0] = f->fog_bias;
            c.fade[1] = f->fog_scale;
            c.fade[2] = kFadeStart;
            c.fade[3] = kFadeEnd;
            c.size[0] = static_cast<int32_t>(f->width);
            c.size[1] = static_cast<int32_t>(f->height);
            const VcsModApi &a = *mod::api;
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));
            a.render_push_marker("renderhook ao");
            pass(g_gpu.depth, g_gpu.viewDepth[f->depth_samples > 1u ? 1 : 0], *f, f->depth, nullptr);
            pass(g_gpu.raw, g_gpu.gtao, *f, g_gpu.depth, nullptr);
            pass(f->color, view ? g_gpu.show : g_gpu.apply, *f, g_gpu.raw, g_gpu.depth);
            a.render_pop_marker();
            ++g_frames;
        }

        // the render hook is added the first time it is needed: with ambient occlusion off nothing in the renderer changes
        // (its Z buffer stays as it was)
        void hookOnce()
        {
            if (g_hooked)
                return;
            g_hooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &hook, nullptr, VCS_RENDER_NEEDS_DEPTH) != 0;
            if (!g_hooked)
                mod::log("[renderhook] the renderer refused the render hook\n");
        }
    }

    void hookAo() { hookOnce(); }

    bool aoOn() { return g_level.load(std::memory_order_relaxed) > 0 || g_view.load(std::memory_order_relaxed) != 0; }

    void registerCommands()
    {
        // VCS_RH_AO_VIEW=1: the debug view (the occlusion as grey over the 3D picture) from the start
        if (const char *e = std::getenv("VCS_RH_AO_VIEW"); e && e[0] == '1')
        {
            g_view = 1;
            hookOnce();
        }
        // renderhook [view 0|1 | ao 0..3] -> the status line: the settings, frames drawn / skipped, the last frame's camera
        mod::addCommand("renderhook", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("view", 0) == 0)
            {
                g_view = std::atoi(args.c_str() + 4) != 0 ? 1 : 0;
                if (g_view)
                    hookOnce();
            }
            else if (args.rfind("ao", 0) == 0 && !mod::set("renderhook.ao", std::atoi(args.c_str() + 2)))
            {
                reply("error refused (an environment override?)");
                return;
            }
            const VcsRenderFrame &f = g_last;
            char line[600];
            std::snprintf(line, sizeof(line),
                          "ok ao %d radius %d view %d hooked %d frames %llu skipped %llu backend %d size %ux%u samples %u/%u "
                          "depth %d camera %d f %.2f %.2f c %.2f %.2f z %.4f %.2f %g fog %.2f %.5f%s%s",
                          g_level.load(), g_radiusCm.load(), g_view.load(), g_hooked ? 1 : 0, static_cast<unsigned long long>(g_frames),
                          static_cast<unsigned long long>(g_skipped), f.backend, f.width, f.height, f.color_samples, f.depth_samples,
                          f.depth ? 1 : 0, f.camera_valid, f.fx, f.fy, f.cx, f.cy, f.depth_a, f.depth_b, f.depth_scale, f.fog_bias,
                          f.fog_scale, g_problem.empty() ? "" : " problem: ", g_problem.c_str());
            reply(line);
        });
    }

    void registerSettings()
    {
        mod::Spec s;
        s.key = "renderhook.ao";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Ambient occlusion";
        s.help = "Darkens corners, creases and the ground under cars and people (ground-truth AO from the game's depth, as "
                 "RenderHook does in San Andreas). Off is the original. Costs some GPU time at high resolutions.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off (original)", 0}, {"Subtle", 1}, {"Normal", 2}, {"Strong", 3}};
        s.defaultValue = 0;
        s.env = "VCS_RH_AO";
        s.onChange = [](int v) {
            g_level = std::clamp(v, 0, 3);
            if (v > 0)
                hookOnce();
        };
        mod::add(s);

        s = {};
        s.key = "renderhook.ao_radius";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Occlusion radius";
        s.help = "How far around an object its occlusion reaches. Larger darkens more of the surroundings.";
        s.type = mod::Type::Integer;
        s.min = 50;
        s.max = 300;
        s.format = "%d cm";
        s.defaultValue = 120;
        s.env = "VCS_RH_AO_RADIUS";
        s.onChange = [](int v) { g_radiusCm = std::clamp(v, 50, 300); };
        mod::add(s);
    }
}
