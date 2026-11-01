// See water.h. Main thread, inside the renderer's water hook points (VCS_RENDER_BEFORE_WATER / AFTER_WATER).
//
// BEFORE_WATER: the picture (render_current_color) and the Z buffer (as it is, R32F) copied - the scene without the
// water. AFTER_WATER: ps_info finds the water's pixels (the Z buffer nearer than before) and the depths in front and
// behind; the other stages' inputs are cleared and their passes run; ps_water draws the surface over the game's water
// in the water's bounds. Peds, vehicles, particles and the game's post effects come after, as in the original.
#include "renderhook.h"
#include "water.h"
#include "water_sim.h"

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhCopyPS.h"
#include "dxbc/kRhWaterRawDepthPS.h"
#include "dxbc/kRhWaterRawDepthMsPS.h"
#include "dxbc/kRhWaterInfoPS.h"
#include "dxbc/kRhWaterInfoMsPS.h"
#include "dxbc/kRhWaterExtrasClearPS.h"
#include "dxbc/kRhWaterPS.h"
#include "dxbc/kRhWaterShowPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhCopyPS.h"
#include "spirv/kRhWaterRawDepthPS.h"
#include "spirv/kRhWaterRawDepthMsPS.h"
#include "spirv/kRhWaterInfoPS.h"
#include "spirv/kRhWaterInfoMsPS.h"
#include "spirv/kRhWaterExtrasClearPS.h"
#include "spirv/kRhWaterPS.h"
#include "spirv/kRhWaterShowPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        constexpr uint32_t kSunRing = 0x4E7200u, kSunIndex = 0x4EA568u; // CTimeCycle's sun vectors (game-internals.md 6n)
        constexpr uint32_t kCameraPos = 0x6F44D0u + 0x30u;              // TheCamera's matrix position (game-internals.md 6b)
        constexpr uint32_t kTimeMs = 0x4CD104u;                         // CTimer::m_snTimeInMilliseconds
        constexpr float kFarthest = 100000.0f;

        // renderhook.water: 0 off, 1 calm, 2 normal, 3 rough (the waves' strength)
        constexpr float kWaves[4] = {0.0f, 0.45f, 1.0f, 1.7f};
        std::atomic<int> g_level{0}, g_reflections{2}, g_glitter{2}, g_view{0};
        bool g_hooked = false;
        uint64_t g_frames = 0, g_skipped = 0, g_before = 0;
        int g_lastRect[4] = {};
        float g_lastTint[4] = {}, g_lastCam[3] = {};
        std::string g_problem;
        std::vector<water::Pass> g_prepare;

        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Shaders
        {
            Shader vs, copy, raw, rawMs, info, infoMs, clear, water, show;
        };

        bool shadersFor(int backend, Shaders &s)
        {
#if defined(VCS_MOD_HAVE_DXBC)
            if (backend == VCS_RENDER_BACKEND_D3D11)
            {
                s = {{kRhFullScreenVS, sizeof(kRhFullScreenVS)},           {kRhCopyPS, sizeof(kRhCopyPS)},
                     {kRhWaterRawDepthPS, sizeof(kRhWaterRawDepthPS)},     {kRhWaterRawDepthMsPS, sizeof(kRhWaterRawDepthMsPS)},
                     {kRhWaterInfoPS, sizeof(kRhWaterInfoPS)},             {kRhWaterInfoMsPS, sizeof(kRhWaterInfoMsPS)},
                     {kRhWaterExtrasClearPS, sizeof(kRhWaterExtrasClearPS)}, {kRhWaterPS, sizeof(kRhWaterPS)},
                     {kRhWaterShowPS, sizeof(kRhWaterShowPS)}};
                return true;
            }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
            if (backend == VCS_RENDER_BACKEND_VULKAN)
            {
                s = {{kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)},
                     {kRhCopyPS_spv, sizeof(kRhCopyPS_spv)},
                     {kRhWaterRawDepthPS_spv, sizeof(kRhWaterRawDepthPS_spv)},
                     {kRhWaterRawDepthMsPS_spv, sizeof(kRhWaterRawDepthMsPS_spv)},
                     {kRhWaterInfoPS_spv, sizeof(kRhWaterInfoPS_spv)},
                     {kRhWaterInfoMsPS_spv, sizeof(kRhWaterInfoMsPS_spv)},
                     {kRhWaterExtrasClearPS_spv, sizeof(kRhWaterExtrasClearPS_spv)},
                     {kRhWaterPS_spv, sizeof(kRhWaterPS_spv)},
                     {kRhWaterShowPS_spv, sizeof(kRhWaterShowPS_spv)}};
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
            VcsRenderPipeline *copy = nullptr, *raw[2] = {}, *infoPipe[2] = {}, *clear = nullptr, *water = nullptr, *show = nullptr;
            VcsRenderBuffer *constants = nullptr;
            VcsRenderTexture *under = nullptr, *underZ = nullptr, *info = nullptr, *extras = nullptr;
            uint32_t width = 0, height = 0;
            bool failed = false, haveUnder = false;
        };
        Gpu g_gpu;

        struct Constants // shaders/water.hlsl cbuffer Water
        {
            float proj[4], zmap[4], fog[4], rot0[4], rot1[4], rot2[4], cam[4], sun[4], tint[4], params[4], params2[4], wake[4];
            int32_t size[4];
        };

        void fail(const char *why)
        {
            if (g_problem != why)
                mod::log("[renderhook] water off: %s\n", why);
            g_problem = why;
        }

        VcsRenderPipeline *pipeline(const Shader &vs, const Shader &ps, uint32_t mask, uint32_t multisampled, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = vs.code;
            d.vertex_size = vs.size;
            d.pixel_code = ps.code;
            d.pixel_size = ps.size;
            d.blend = VCS_RENDER_BLEND_OPAQUE;
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
                g_gpu.copy = pipeline(s.vs, s.copy, 0xFu, 0u, "renderhook water under");
                g_gpu.raw[0] = pipeline(s.vs, s.raw, 0xFu, 0u, "renderhook water under depth");
                g_gpu.raw[1] = pipeline(s.vs, s.rawMs, 0xFu, 1u, "renderhook water under depth (MSAA)");
                g_gpu.infoPipe[0] = pipeline(s.vs, s.info, 0xFu, 0u, "renderhook water info");
                g_gpu.infoPipe[1] = pipeline(s.vs, s.infoMs, 0xFu, 1u, "renderhook water info (MSAA)");
                g_gpu.clear = pipeline(s.vs, s.clear, 0xFu, 0u, "renderhook water extras clear");
                g_gpu.water = pipeline(s.vs, s.water, 0x7u, 0u, "renderhook water");
                g_gpu.show = pipeline(s.vs, s.show, 0x7u, 0u, "renderhook water view");
                g_gpu.constants = a.render_create_constants(sizeof(Constants), "renderhook water constants");
                if (!g_gpu.copy || !g_gpu.raw[0] || !g_gpu.raw[1] || !g_gpu.infoPipe[0] || !g_gpu.infoPipe[1] || !g_gpu.clear ||
                    !g_gpu.water || !g_gpu.show || !g_gpu.constants)
                {
                    g_gpu.failed = true;
                    fail("the renderer refused its pipelines");
                    return false;
                }
            }
            if (g_gpu.width != f.width || g_gpu.height != f.height || !g_gpu.under)
            {
                for (VcsRenderTexture *t : {g_gpu.under, g_gpu.underZ, g_gpu.info, g_gpu.extras})
                    if (t)
                        a.render_destroy_texture(t);
                g_gpu.under = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, "renderhook water under");
                g_gpu.underZ = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_R32F, "renderhook water under depth");
                g_gpu.info = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA16F, "renderhook water info");
                g_gpu.extras = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, "renderhook water extras");
                g_gpu.width = f.width;
                g_gpu.height = f.height;
                g_gpu.haveUnder = false;
                if (!g_gpu.under || !g_gpu.underZ || !g_gpu.info || !g_gpu.extras)
                {
                    g_gpu.under = nullptr;
                    fail("the renderer refused its textures");
                    return false;
                }
            }
            return true;
        }

        void pass(VcsRenderTexture *target, VcsRenderPipeline *p, const int rect[4], VcsRenderTexture *t0, VcsRenderTexture *t1,
                  VcsRenderTexture *t2 = nullptr)
        {
            const VcsModApi &a = *mod::api;
            a.render_begin_pass(target);
            a.render_set_viewport(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
            a.render_set_pipeline(p);
            a.render_set_constants(0, g_gpu.constants);
            if (t0)
                a.render_set_texture(0, t0, VCS_RENDER_SAMPLER_POINT);
            if (t1)
                a.render_set_texture(1, t1, VCS_RENDER_SAMPLER_POINT);
            if (t2)
                a.render_set_texture(2, t2, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
        }

        const uint8_t *ram() { return mod::api->rdram ? mod::api->rdram() : nullptr; }

        bool sunWorld(float out[3])
        {
            const uint8_t *r = ram();
            if (!r)
                return false;
            uint32_t index;
            std::memcpy(&index, r + kSunIndex, 4);
            if (index > 15u)
                return false;
            std::memcpy(out, r + kSunRing + index * 16u, 12);
            const float len2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
            return len2 > 0.9f && len2 < 1.1f;
        }

        bool frameOk(const VcsRenderFrame *f)
        {
            return f && f->size >= offsetof(VcsRenderFrame, water_color) + sizeof(f->water_color) &&
                   f->backend != VCS_RENDER_BACKEND_NULL && f->depth && f->camera_valid && f->width && f->height;
        }

        void beforeWater(const VcsRenderFrame &f)
        {
            const VcsModApi &a = *mod::api;
            g_gpu.haveUnder = false;
            VcsRenderTexture *picture = a.render_current_color();
            if (!picture)
                return;
            const int full[4] = {0, 0, static_cast<int>(f.width), static_cast<int>(f.height)};
            a.render_push_marker("renderhook water: before");
            pass(g_gpu.under, g_gpu.copy, full, nullptr, nullptr, picture); // temporal.hlsl ps_copy reads t2
            pass(g_gpu.underZ, g_gpu.raw[f.depth_samples > 1u ? 1 : 0], full, f.depth, nullptr);
            a.render_pop_marker();
            g_gpu.haveUnder = true;
            ++g_before;
        }

        void afterWater(const VcsRenderFrame &f)
        {
            const VcsModApi &a = *mod::api;
            if (!g_gpu.haveUnder || !f.view_valid)
            {
                ++g_skipped;
                return;
            }
            g_gpu.haveUnder = false;
            int rect[4] = {f.water_rect[0], f.water_rect[1], f.water_rect[2], f.water_rect[3]};
            if (rect[2] <= rect[0] || rect[3] <= rect[1])
                return;
            std::memcpy(g_lastRect, rect, sizeof(rect));
            water::Frame wf = {};
            wf.f = &f;
            wf.under = g_gpu.under;
            wf.info = g_gpu.info;
            wf.extras = g_gpu.extras;
            std::memcpy(wf.rect, rect, sizeof(rect));
            std::memcpy(wf.worldToView, f.world_to_view, sizeof(wf.worldToView));
            std::memcpy(wf.waterColor, f.water_color, sizeof(wf.waterColor));
            uint32_t ms = 0;
            if (const uint8_t *r = ram())
            {
                std::memcpy(wf.camera, r + kCameraPos, 12);
                std::memcpy(&ms, r + kTimeMs, 4);
            }
            wf.time = static_cast<float>(ms % 3600000u) / 1000.0f;
            if (sunWorld(wf.sun))
            {
                const float t = std::clamp(wf.sun[2] / 0.2f, 0.0f, 1.0f);
                wf.daylight = t * t * (3.0f - 2.0f * t);
            }
            std::memcpy(g_lastTint, f.water_color, sizeof(g_lastTint));
            std::memcpy(g_lastCam, wf.camera, sizeof(g_lastCam));

            const int level = g_level.load(std::memory_order_relaxed), view = g_view.load(std::memory_order_relaxed);
            Constants c = {};
            c.proj[0] = f.fx, c.proj[1] = f.fy, c.proj[2] = f.cx, c.proj[3] = f.cy;
            c.zmap[0] = f.depth_a, c.zmap[1] = f.depth_b, c.zmap[2] = f.depth_scale, c.zmap[3] = kFarthest;
            c.fog[0] = f.fog_bias, c.fog[1] = f.fog_scale;
            for (int k = 0; k < 3; ++k)
            {
                c.rot0[k] = f.world_to_view[k];
                c.rot1[k] = f.world_to_view[3 + k];
                c.rot2[k] = f.world_to_view[6 + k];
                c.cam[k] = wf.camera[k];
                c.sun[k] = wf.sun[k];
                c.tint[k] = f.water_color[k];
            }
            c.cam[3] = wf.time;
            c.sun[3] = wf.daylight;
            c.tint[3] = 1.25f; // the deep colour: the tint times about the water texture's mean
            c.params[0] = kWaves[std::clamp(level, 1, 3)];
            c.params[1] = std::clamp(g_reflections.load(std::memory_order_relaxed), 0, 3) / 2.0f;
            c.params[2] = std::clamp(g_glitter.load(std::memory_order_relaxed), 0, 3) / 2.0f;
            c.params[3] = 0.35f;           // refraction: metres the seabed shifts per unit of slope
            c.params2[0] = 0.12f;          // absorption per metre (scaled per channel in the shader)
            c.params2[2] = static_cast<float>(view);
            c.params2[3] = 0.6f;           // the wind's direction (radians from east)
            c.size[0] = static_cast<int32_t>(f.width);
            c.size[1] = static_cast<int32_t>(f.height);
            const WakeField *wake = wakesOn() ? wakeField(f) : nullptr;
            if (wake && wake->valid && wake->texture && wake->size > 0.0f)
            {
                c.params2[1] = 1.0f;
                c.wake[0] = wake->cam_x, c.wake[1] = wake->cam_y, c.wake[2] = 1.0f / wake->size;
            }
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));

            a.render_push_marker("renderhook water");
            pass(g_gpu.info, g_gpu.infoPipe[f.depth_samples > 1u ? 1 : 0], rect, f.depth, g_gpu.underZ);
            pass(g_gpu.extras, g_gpu.clear, rect, nullptr, nullptr);
            for (water::Pass p : g_prepare)
                p(wf);
            VcsRenderTexture *picture = a.render_current_color(); // with the game's water: the fog's far end
            a.render_update_constants(g_gpu.constants, &c, sizeof(c)); // (a stage may have used the slot)
            a.render_begin_pass(f.color);
            a.render_set_viewport(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
            a.render_set_pipeline(view ? g_gpu.show : g_gpu.water);
            a.render_set_constants(0, g_gpu.constants);
            a.render_set_texture(0, g_gpu.info, VCS_RENDER_SAMPLER_POINT);
            a.render_set_texture(1, g_gpu.under, VCS_RENDER_SAMPLER_LINEAR);
            a.render_set_texture(2, picture, VCS_RENDER_SAMPLER_POINT);
            a.render_set_texture(3, g_gpu.extras, VCS_RENDER_SAMPLER_LINEAR);
            a.render_set_texture(4, c.params2[1] > 0.5f ? wake->texture : g_gpu.extras, VCS_RENDER_SAMPLER_LINEAR);
            a.render_draw(3);
            a.render_end_pass();
            a.render_pop_marker();
            ++g_frames;
            g_problem.clear();
        }

        void hook(void *, const VcsRenderFrame *f)
        {
            if (!water::on() || !frameOk(f) || !ready(*f))
            {
                if (water::on())
                    ++g_skipped;
                return;
            }
            if (f->point == VCS_RENDER_BEFORE_WATER)
                beforeWater(*f);
            else if (f->point == VCS_RENDER_AFTER_WATER)
                afterWater(*f);
        }

        void hookOnce()
        {
            if (g_hooked)
                return;
            const VcsModApi &a = *mod::api;
            g_hooked = a.add_render_hook(VCS_RENDER_BEFORE_WATER, &hook, nullptr, VCS_RENDER_NEEDS_DEPTH) != 0 &&
                       a.add_render_hook(VCS_RENDER_AFTER_WATER, &hook, nullptr, VCS_RENDER_NEEDS_DEPTH) != 0;
            if (!g_hooked)
                fail("this vcs.exe has no water hook points: update vcs.exe");
        }
    }

    namespace water
    {
        void addPrepare(Pass pass)
        {
            if (pass)
                g_prepare.push_back(pass);
        }

        bool on() { return g_level.load(std::memory_order_relaxed) > 0 || g_view.load(std::memory_order_relaxed) != 0; }
    }

    void registerWater()
    {
        // VCS_RH_WATER_VIEW=1 the water's pixels and depth, 2 its normals, 3 the reflections' hits (red: the edge fallback),
        // 4 the picture before the water
        if (const char *e = std::getenv("VCS_RH_WATER_VIEW"); e && std::atoi(e) != 0)
        {
            g_view = std::clamp(std::atoi(e), 0, 4);
            hookOnce();
        }
        // rhwater [view N | level N] -> the settings, frames, the last water's bounds, tint and camera
        mod::addCommand("rhwater", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("view", 0) == 0)
            {
                g_view = std::clamp(std::atoi(args.c_str() + 4), 0, 4);
                if (g_view)
                    hookOnce();
            }
            else if (args.rfind("level", 0) == 0 && !mod::set("renderhook.water", std::atoi(args.c_str() + 5)))
            {
                reply("error refused (an environment override?)");
                return;
            }
            char line[400];
            std::snprintf(line, sizeof(line),
                          "ok level %d view %d hooked %d frames %llu before %llu skipped %llu rect %d %d %d %d tint %.3f %.3f %.3f %.3f "
                          "camera %.2f %.2f %.2f%s%s",
                          g_level.load(), g_view.load(), g_hooked ? 1 : 0, static_cast<unsigned long long>(g_frames),
                          static_cast<unsigned long long>(g_before), static_cast<unsigned long long>(g_skipped), g_lastRect[0],
                          g_lastRect[1], g_lastRect[2], g_lastRect[3], g_lastTint[0], g_lastTint[1], g_lastTint[2], g_lastTint[3],
                          g_lastCam[0], g_lastCam[1], g_lastCam[2], g_problem.empty() ? "" : " problem ", g_problem.c_str());
            reply(line);
        });
    }

    void registerWaterSettings()
    {
        mod::Spec s;
        s.key = "renderhook.water";
        s.page = "Graphics";
        s.section = "Water (RenderHook)";
        s.label = "Water";
        s.help = "A new sea surface: moving waves, the seabed seen through shallow water, reflections of the city and the sky, "
                 "the sun glittering on the waves. Off is the original.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off (original)", 0}, {"Calm", 1}, {"Normal", 2}, {"Rough", 3}};
        s.defaultValue = 0;
        s.env = "VCS_RH_WATER";
        s.onChange = [](int v) {
            g_level = std::clamp(v, 0, 3);
            if (v > 0)
                hookOnce();
        };
        mod::add(s);

        s = {};
        s.key = "renderhook.water_reflections";
        s.page = "Graphics";
        s.section = "Water (RenderHook)";
        s.label = "Water reflections";
        s.help = "How strongly the water mirrors the city and the sky (screen-space reflections: what is off screen falls back "
                 "to the picture's edge).";
        s.type = mod::Type::Choice;
        s.choices = {{"Off", 0}, {"Low", 1}, {"Normal", 2}, {"High", 3}};
        s.defaultValue = 2;
        s.env = "VCS_RH_WATER_REFLECTIONS";
        s.onChange = [](int v) { g_reflections = std::clamp(v, 0, 3); };
        mod::add(s);

        s = {};
        s.key = "renderhook.water_glitter";
        s.page = "Graphics";
        s.section = "Water (RenderHook)";
        s.label = "Sun glitter";
        s.help = "The sun's reflection on the waves: sparkles and a bright path towards the sun, in daylight.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off", 0}, {"Low", 1}, {"Normal", 2}, {"High", 3}};
        s.defaultValue = 2;
        s.env = "VCS_RH_WATER_GLITTER";
        s.onChange = [](int v) { g_glitter = std::clamp(v, 0, 3); };
        mod::add(s);
    }
}
