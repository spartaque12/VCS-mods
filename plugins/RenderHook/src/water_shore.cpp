// See water_shore.h. Main thread, inside the new water's AFTER_WATER hook (water.h addPrepare): the sea is drawn, the
// water's pixels are known (Frame.info: the surface's and the seabed's view depths), the surface is not drawn yet, peds
// and vehicles come later.
//
// Two full-screen passes (shaders/water_shore.hlsl):
// - ps_shore_extras into Frame.extras over the water's bounds: foam (r), the shore waves' slope (g / b) and the caustic
//   light on the seabed (a), which the surface shader applies. From the seabed's depth below the water level - the game's
//   water is one plane at z 6.0 (game-internals.md 6q) - so it needs no knowledge of where the beaches are.
// - ps_shore_swash into the picture: the waves' run-up on the sand above the waterline (land pixels: Frame.info.x 0),
//   blended over the ground before peds and vehicles are drawn.
#include "renderhook.h"
#include "water.h"
#include "water_shore.h"

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
#include "dxbc/kRhShoreExtrasPS.h"
#include "dxbc/kRhShoreShowPS.h"
#include "dxbc/kRhShoreShowMsPS.h"
#include "dxbc/kRhShoreSwashPS.h"
#include "dxbc/kRhShoreSwashMsPS.h"
#include "dxbc/kRhPoolCausticsPS.h"
#include "dxbc/kRhPoolCausticsMsPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhCopyPS.h"
#include "spirv/kRhShoreExtrasPS.h"
#include "spirv/kRhShoreShowPS.h"
#include "spirv/kRhShoreShowMsPS.h"
#include "spirv/kRhShoreSwashPS.h"
#include "spirv/kRhShoreSwashMsPS.h"
#include "spirv/kRhPoolCausticsPS.h"
#include "spirv/kRhPoolCausticsMsPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        constexpr float kWaterLevel = 6.0f; // CWaterLevel's one level (game-internals.md 6q)
        constexpr float kFarthest = 100000.0f;
        // the shore waves: period (s), crest spacing (in sqrt(m) of seabed depth: shaders/water_shore.hlsl wavePhase), the
        // depth they break at (m), the run-up's height above the waterline (m; 2.5 m up a 1:4 beach, 9 m up a 1:15 one)
        constexpr float kPeriod = 7.0f, kSpacing = 0.5f, kBreaking = 1.0f, kRunUp = 0.6f;
        constexpr float kFadeStart = 150.0f, kFadeEnd = 320.0f; // view depth (m) over which the shore fades out
        // renderhook.shore_foam / renderhook.caustics: Off, Subtle, Normal, Strong
        constexpr float kAmounts[4] = {0.0f, 0.5f, 0.8f, 1.0f};

        std::atomic<int> g_foam{0}, g_waves{0}, g_caustics{0}, g_view{0}, g_poolView{0};
        uint64_t g_frames = 0, g_skipped = 0;
        VcsRenderTexture *g_viewInfo = nullptr; // the water's info for the debug view (this frame's, from prepare)

        int g_lastRect[4] = {};
        float g_lastCam[3] = {}, g_lastDay = 0.0f;
        std::string g_problem;

        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Shaders
        {
            Shader vs, extras, show, swash, swashMs, pool, poolMs, copy, showMs;
        };

        bool shadersFor(int backend, Shaders &s)
        {
#if defined(VCS_MOD_HAVE_DXBC)
            if (backend == VCS_RENDER_BACKEND_D3D11)
            {
                s = {{kRhFullScreenVS, sizeof(kRhFullScreenVS)}, {kRhShoreExtrasPS, sizeof(kRhShoreExtrasPS)},
                     {kRhShoreShowPS, sizeof(kRhShoreShowPS)}, {kRhShoreSwashPS, sizeof(kRhShoreSwashPS)},
                     {kRhShoreSwashMsPS, sizeof(kRhShoreSwashMsPS)}, {kRhPoolCausticsPS, sizeof(kRhPoolCausticsPS)},
                     {kRhPoolCausticsMsPS, sizeof(kRhPoolCausticsMsPS)}, {kRhCopyPS, sizeof(kRhCopyPS)},
                     {kRhShoreShowMsPS, sizeof(kRhShoreShowMsPS)}};
                return true;
            }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
            if (backend == VCS_RENDER_BACKEND_VULKAN)
            {
                s = {{kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)}, {kRhShoreExtrasPS_spv, sizeof(kRhShoreExtrasPS_spv)},
                     {kRhShoreShowPS_spv, sizeof(kRhShoreShowPS_spv)}, {kRhShoreSwashPS_spv, sizeof(kRhShoreSwashPS_spv)},
                     {kRhShoreSwashMsPS_spv, sizeof(kRhShoreSwashMsPS_spv)}, {kRhPoolCausticsPS_spv, sizeof(kRhPoolCausticsPS_spv)},
                     {kRhPoolCausticsMsPS_spv, sizeof(kRhPoolCausticsMsPS_spv)}, {kRhCopyPS_spv, sizeof(kRhCopyPS_spv)},
                     {kRhShoreShowMsPS_spv, sizeof(kRhShoreShowMsPS_spv)}};
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
            VcsRenderPipeline *extras = nullptr, *show[2] = {}, *swash[2] = {}, *pool[2] = {}; // [2]: single / multisampled Z
            VcsRenderBuffer *poolConstants = nullptr;
            VcsRenderPipeline *copy = nullptr;    // temporal.hlsl ps_copy (t2 -> target)
            VcsRenderTexture *picture = nullptr;  // the pools' copy of the picture (it may be the target itself)
            uint32_t width = 0, height = 0;
            VcsRenderBuffer *constants = nullptr;
            bool failed = false;
        };
        Gpu g_gpu;

        struct Constants // shaders/water_shore.hlsl cbuffer Shore
        {
            float proj[4], zmap[4], rowX[4], rowY[4], rowZ[4], camera[4], sun[4], anim[4], amount[4], fade[4];
            int32_t size[4];
        };

        void fail(const char *why)
        {
            if (g_problem != why)
                mod::log("[renderhook] shore off: %s\n", why);
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
            if (g_gpu.failed)
                return false;
            if (g_gpu.backend == f.backend)
                return true;
            g_gpu.backend = f.backend;
            Shaders s;
            if (!shadersFor(f.backend, s))
            {
                g_gpu.failed = true;
                fail("this build of RenderHook.asi has no shaders for the graphics API in use");
                return false;
            }
            g_gpu.extras = pipeline(s.vs, s.extras, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook shore extras");
            g_gpu.show[0] = pipeline(s.vs, s.show, VCS_RENDER_BLEND_OPAQUE, 0x7u, 0u, "renderhook shore view");
            g_gpu.show[1] = pipeline(s.vs, s.showMs, VCS_RENDER_BLEND_OPAQUE, 0x7u, 1u << 1, "renderhook shore view (MSAA)");
            g_gpu.swash[0] = pipeline(s.vs, s.swash, VCS_RENDER_BLEND_ALPHA, 0x7u, 0u, "renderhook shore swash");
            g_gpu.swash[1] = pipeline(s.vs, s.swashMs, VCS_RENDER_BLEND_ALPHA, 0x7u, 1u << 1, "renderhook shore swash (MSAA)");
            g_gpu.pool[0] = pipeline(s.vs, s.pool, VCS_RENDER_BLEND_ADD, 0x7u, 0u, "renderhook pool caustics");
            g_gpu.pool[1] = pipeline(s.vs, s.poolMs, VCS_RENDER_BLEND_ADD, 0x7u, 1u << 1, "renderhook pool caustics (MSAA)");
            g_gpu.constants = mod::api->render_create_constants(sizeof(Constants), "renderhook shore constants");
            g_gpu.poolConstants = mod::api->render_create_constants(sizeof(Constants), "renderhook pool constants");
            g_gpu.copy = pipeline(s.vs, s.copy, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook pool picture copy");
            if (!g_gpu.extras || !g_gpu.show[0] || !g_gpu.show[1] || !g_gpu.swash[0] || !g_gpu.swash[1] || !g_gpu.pool[0] || !g_gpu.pool[1] ||
                !g_gpu.constants || !g_gpu.poolConstants || !g_gpu.copy)
            {
                g_gpu.failed = true;
                fail("the renderer refused its pipelines");
                return false;
            }
            return true;
        }

        void prepare(const water::Frame &w)
        {
            const float foam = kAmounts[std::clamp(g_foam.load(std::memory_order_relaxed), 0, 3)];
            const float waves = g_waves.load(std::memory_order_relaxed) > 0 ? 1.0f : 0.0f;
            const float caustics = kAmounts[std::clamp(g_caustics.load(std::memory_order_relaxed), 0, 3)];
            const bool view = g_view.load(std::memory_order_relaxed) != 0;
            if (foam <= 0.0f && waves <= 0.0f && caustics <= 0.0f && !view)
                return;
            const VcsRenderFrame &f = *w.f;
            if (f.backend == VCS_RENDER_BACKEND_NULL || !f.depth || !f.camera_valid || !f.view_valid || !w.info || !w.extras ||
                !ready(f))
            {
                ++g_skipped;
                return;
            }
            Constants c = {};
            c.proj[0] = f.fx, c.proj[1] = f.fy, c.proj[2] = f.cx, c.proj[3] = f.cy;
            c.zmap[0] = f.depth_a, c.zmap[1] = f.depth_b, c.zmap[2] = f.depth_scale, c.zmap[3] = kFarthest;
            // R's rows: the shader turns view space back into the world as R^T v = v.x row 0 + v.y row 1 + v.z row 2
            for (int k = 0; k < 3; ++k)
            {
                c.rowX[k] = w.worldToView[k];
                c.rowY[k] = w.worldToView[3 + k];
                c.rowZ[k] = w.worldToView[6 + k];
                c.camera[k] = w.camera[k];
                c.sun[k] = w.sun[k];
            }
            c.camera[3] = kWaterLevel;
            c.sun[3] = w.daylight;
            c.anim[0] = std::fmod(w.time, kPeriod * 600.0f); // whole periods: the waves do not jump when it wraps
            c.anim[1] = kPeriod, c.anim[2] = kSpacing, c.anim[3] = kBreaking;
            c.amount[0] = foam, c.amount[1] = waves, c.amount[2] = caustics, c.amount[3] = kRunUp;
            if (view && foam <= 0.0f && waves <= 0.0f && caustics <= 0.0f)
                c.amount[0] = c.amount[1] = c.amount[2] = 1.0f; // the debug view on its own shows everything
            c.fade[0] = f.fog_bias, c.fade[1] = f.fog_scale, c.fade[2] = kFadeStart, c.fade[3] = kFadeEnd;
            c.size[0] = static_cast<int32_t>(f.width);
            c.size[1] = static_cast<int32_t>(f.height);
            c.size[2] = g_view.load(std::memory_order_relaxed); // the debug view's mode
            const VcsModApi &a = *mod::api;
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));
            std::copy(w.rect, w.rect + 4, g_lastRect);
            std::copy(w.camera, w.camera + 3, g_lastCam);
            g_lastDay = w.daylight;

            a.render_push_marker("renderhook shore");
            // the water's extras (the surface shader applies them; the debug view is drawn at BEFORE_POST, over it)
            g_viewInfo = view ? w.info : nullptr;
            a.render_begin_pass(w.extras);
            a.render_set_viewport(w.rect[0], w.rect[1], w.rect[2] - w.rect[0], w.rect[3] - w.rect[1]);
            a.render_set_pipeline(g_gpu.extras);
            a.render_set_constants(0, g_gpu.constants);
            a.render_set_texture(0, w.info, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
            // the run-up on the sand (whole picture: the beach can reach past the water's bounds)
            if (waves > 0.0f || foam > 0.0f)
            {
                a.render_begin_pass(f.color);
                a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
                a.render_set_pipeline(g_gpu.swash[f.depth_samples > 1u ? 1 : 0]);
                a.render_set_constants(0, g_gpu.constants);
                a.render_set_texture(0, w.info, VCS_RENDER_SAMPLER_POINT);
                a.render_set_texture(1, f.depth, VCS_RENDER_SAMPLER_POINT);
                a.render_draw(3);
                a.render_end_pass();
            }
            a.render_pop_marker();
            ++g_frames;
            g_problem.clear();
        }

        // ---- caustics in pools: a BEFORE_POST hook of its own (pools are no water to the game: the water hook does not
        // run for them, frames without the sea included) --------------------------------------------------------------
        constexpr uint32_t kSunRing = 0x4E7200u, kSunIndex = 0x4EA568u; // CTimeCycle's sun vectors (game-internals.md 6n)
        constexpr uint32_t kCameraPos = 0x6F44D0u + 0x30u;              // TheCamera's position (game-internals.md 6q)
        constexpr uint32_t kTimeMs = 0x4CD104u;                         // CTimer::m_snTimeInMilliseconds
        uint64_t g_poolFrames = 0;
        bool g_poolHooked = false;

        void poolHook(void *, const VcsRenderFrame *fp)
        {
            const float caustics = kAmounts[std::clamp(g_caustics.load(std::memory_order_relaxed), 0, 3)];
            if (caustics <= 0.0f || !fp || fp->size < offsetof(VcsRenderFrame, world_to_view) + sizeof(fp->world_to_view))
                return;
            const VcsRenderFrame &f = *fp;
            const uint8_t *ram = mod::api->rdram ? mod::api->rdram() : nullptr;
            if (f.backend == VCS_RENDER_BACKEND_NULL || !ram || !f.depth || !f.camera_valid || !f.view_valid || !ready(f))
                return;
            uint32_t index = 0, ms = 0;
            float sun[3] = {}, camera[3] = {};
            std::memcpy(&index, ram + kSunIndex, 4);
            std::memcpy(&ms, ram + kTimeMs, 4);
            std::memcpy(camera, ram + kCameraPos, 12);
            if (index > 15u)
                return;
            std::memcpy(sun, ram + kSunRing + index * 16u, 12);
            const float t = std::clamp(sun[2] / 0.2f, 0.0f, 1.0f), day = t * t * (3.0f - 2.0f * t);
            if (day <= 0.0f)
                return;
            Constants c = {};
            c.proj[0] = f.fx, c.proj[1] = f.fy, c.proj[2] = f.cx, c.proj[3] = f.cy;
            c.zmap[0] = f.depth_a, c.zmap[1] = f.depth_b, c.zmap[2] = f.depth_scale, c.zmap[3] = kFarthest;
            for (int k = 0; k < 3; ++k)
            {
                c.rowX[k] = f.world_to_view[k];
                c.rowY[k] = f.world_to_view[3 + k];
                c.rowZ[k] = f.world_to_view[6 + k];
                c.camera[k] = camera[k];
                c.sun[k] = sun[k];
            }
            c.camera[3] = kWaterLevel;
            c.sun[3] = day;
            c.anim[0] = std::fmod(static_cast<float>(ms % 3600000u) / 1000.0f, kPeriod * 600.0f);
            c.anim[1] = kPeriod, c.anim[2] = kSpacing, c.anim[3] = kBreaking;
            c.amount[2] = caustics;
            c.amount[3] = static_cast<float>(g_poolView.load(std::memory_order_relaxed)); // the debug view
            c.fade[0] = f.fog_bias, c.fade[1] = f.fog_scale, c.fade[2] = 60.0f, c.fade[3] = 120.0f;
            c.size[0] = static_cast<int32_t>(f.width);
            c.size[1] = static_cast<int32_t>(f.height);
            const VcsModApi &a = *mod::api;
            // the picture as it is now, copied: without MSAA render_current_color is the target itself (read as black
            // while drawing into it)
            VcsRenderTexture *current = a.render_current_color();
            if (!current)
                return;
            if (g_gpu.width != f.width || g_gpu.height != f.height || !g_gpu.picture)
            {
                if (g_gpu.picture)
                    a.render_destroy_texture(g_gpu.picture);
                g_gpu.picture = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, "renderhook pool picture");
                g_gpu.width = f.width, g_gpu.height = f.height;
                if (!g_gpu.picture)
                    return;
            }
            VcsRenderTexture *picture = g_gpu.picture;
            a.render_update_constants(g_gpu.poolConstants, &c, sizeof(c));
            a.render_push_marker("renderhook pool caustics");
            a.render_begin_pass(picture);
            a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
            a.render_set_pipeline(g_gpu.copy);
            a.render_set_texture(2, current, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
            a.render_begin_pass(f.color);
            a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
            a.render_set_pipeline(g_gpu.pool[f.depth_samples > 1u ? 1 : 0]);
            a.render_set_constants(0, g_gpu.poolConstants);
            a.render_set_texture(0, picture, VCS_RENDER_SAMPLER_POINT); // the unused info slot, bound for Vulkan
            a.render_set_texture(1, f.depth, VCS_RENDER_SAMPLER_POINT);
            a.render_set_texture(2, picture, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
            a.render_pop_marker();
            ++g_poolFrames;
        }

        void hookPools()
        {
            if (g_poolHooked)
                return;
            g_poolHooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &poolHook, nullptr,
                                                     VCS_RENDER_NEEDS_DEPTH) != 0;
            if (!g_poolHooked)
                mod::log("[renderhook] the renderer refused the pool caustics' render hook\n");
        }

        // the debug view: after the surface (BEFORE_POST), with this frame's constants and the water's info
        bool g_viewHooked = false;
        void viewHook(void *, const VcsRenderFrame *f)
        {
            if (!g_view.load(std::memory_order_relaxed) || !f || !g_viewInfo || !f->depth ||
                g_gpu.failed || g_gpu.backend != f->backend)
                return;
            const VcsModApi &a = *mod::api;
            a.render_push_marker("renderhook shore view");
            a.render_begin_pass(f->color);
            a.render_set_viewport(0, 0, static_cast<int>(f->width), static_cast<int>(f->height));
            a.render_set_pipeline(g_gpu.show[f->depth_samples > 1u ? 1 : 0]);
            a.render_set_constants(0, g_gpu.constants);
            a.render_set_texture(0, g_viewInfo, VCS_RENDER_SAMPLER_POINT);
            a.render_set_texture(1, f->depth, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
            a.render_pop_marker();
            g_viewInfo = nullptr; // once: a frame without the water has none
        }

        void hookView()
        {
            if (!g_viewHooked)
                g_viewHooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &viewHook, nullptr, VCS_RENDER_NEEDS_DEPTH) != 0;
        }

        bool g_added = false;
        void addOnce()
        {
            if (!g_added)
                water::addPrepare(&prepare);
            g_added = true;
        }

        void choiceRow(const char *key, const char *label, const char *help, const char *env, std::atomic<int> &value,
                       std::vector<mod::Choice> choices)
        {
            mod::Spec s;
            s.key = key;
            s.page = "Graphics";
            s.section = "Water (RenderHook)";
            s.label = label;
            s.help = help;
            s.type = mod::Type::Choice;
            s.choices = std::move(choices);
            s.defaultValue = 0;
            s.env = env;
            s.onChange = [&value](int v) {
                value = std::clamp(v, 0, 3);
                if (&value == &g_caustics && v > 0)
                    hookPools(); // added when first needed: with caustics off nothing in the renderer changes
            };
            mod::add(s);
        }
    }

    void registerShore()
    {
        addOnce();
        // VCS_RH_SHORE_VIEW=1: the debug view over the water (depth bands, crests red, foam white, caustics yellow)
        if (const char *e = std::getenv("VCS_RH_SHORE_VIEW"); e && std::atoi(e) != 0)
        {
            g_view = 1;
            hookView();
        }
        // rhshore [view 0|1|2 | poolview 0|1|2] -> the settings, frames drawn / skipped, the last water's bounds, camera and daylight
        mod::addCommand("rhshore", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("view", 0) == 0)
            {
                g_view = std::clamp(std::atoi(args.c_str() + 4), 0, 2);
                if (g_view)
                    hookView();
            }
            else if (args.rfind("poolview", 0) == 0)
                g_poolView = std::clamp(std::atoi(args.c_str() + 8), 0, 2);
            char line[400];
            std::snprintf(line, sizeof(line),
                          "ok foam %d waves %d caustics %d view %d water %d frames %llu skipped %llu pool_frames %llu rect %d %d %d %d camera %.2f %.2f "
                          "%.2f day %.2f%s%s",
                          g_foam.load(), g_waves.load(), g_caustics.load(), g_view.load(), water::on() ? 1 : 0,
                          static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_skipped),
                          static_cast<unsigned long long>(g_poolFrames), g_lastRect[0],
                          g_lastRect[1], g_lastRect[2], g_lastRect[3], g_lastCam[0], g_lastCam[1], g_lastCam[2], g_lastDay,
                          g_problem.empty() ? "" : " problem ", g_problem.c_str());
            reply(line);
        });
    }

    void registerShoreSettings()
    {
        choiceRow("renderhook.shore_foam", "Shore foam",
                  "White foam where the sea meets the sand and behind breaking waves (with the new water on). Off is the original.",
                  "VCS_RH_SHORE_FOAM", g_foam, {{"Off (original)", 0}, {"Subtle", 1}, {"Normal", 2}, {"Heavy", 3}});
        choiceRow("renderhook.shore_waves", "Shore waves",
                  "Waves that rise and break near beaches and run up the sand, leaving it wet (with the new water on). Off is "
                  "the original.",
                  "VCS_RH_SHORE_WAVES", g_waves, {{"Off (original)", 0}, {"On", 1}});
        choiceRow("renderhook.caustics", "Caustics",
                  "Rippling sunlight on the sea floor under shallow water, in daylight (with the new water on). Off is the "
                  "original.",
                  "VCS_RH_CAUSTICS", g_caustics, {{"Off (original)", 0}, {"Subtle", 1}, {"Normal", 2}, {"Strong", 3}});
    }
}
