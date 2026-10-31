// See renderhook.h: sun shadows. The sun from the game's timecycle (the port's docs/game-internals.md "Sun direction": a
// ring of 16 vectors towards the sun at 0x4E7200, world space, the newest at the index in 0x4EA568), turned into view space
// with the camera's rotation the renderer finds (VcsRenderFrame.world_to_view); cascades fitted around the view frustum's
// depth slices (bounding spheres, a light basis tied to the world's axes and centres snapped to whole texels, so the maps
// do not shimmer while the camera turns); the renderer draws the scene's opaque draws into them (render_shadow_map); one
// full-screen pass multiplies the shadows into the picture (shaders/shadows.hlsl). Main thread, inside the render hook.
// Limits: only what the game drew this frame casts (nothing off screen), and leaves cast as their solid quads.
#include "renderhook.h"

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

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhShadowPS.h"
#include "dxbc/kRhShadowMsPS.h"
#include "dxbc/kRhShadowShowPS.h"
#include "dxbc/kRhShadowShowMsPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhShadowPS.h"
#include "spirv/kRhShadowMsPS.h"
#include "spirv/kRhShadowShowPS.h"
#include "spirv/kRhShadowShowMsPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        constexpr uint32_t kSunRing = 0x4E7200u, kSunIndex = 0x4EA568u; // CTimeCycle's sun vectors and the newest one's index
        constexpr float kStrength[4] = {0.0f, 0.3f, 0.45f, 0.6f};       // renderhook.shadows: soft / normal / dark
        constexpr uint32_t kSizes[3] = {1024u, 2048u, 4096u};            // renderhook.shadow_quality
        constexpr uint32_t kCascades = 3;
        constexpr float kNear = 0.5f, kSplitLambda = 0.75f; // practical split: log / uniform blend
        constexpr float kCasterReach = 250.0f;               // casters this far beyond a cascade towards the sun still count
        constexpr float kBlendBand = 0.1f;                    // the share of a cascade's depth blended into the next
        constexpr float kFarthest = 100000.0f;

        std::atomic<int> g_level{0}, g_distance{150}, g_quality{1}, g_view{0};
        bool g_hooked = false;
        uint64_t g_frames = 0, g_skipped = 0;
        uint32_t g_drawn = 0;
        float g_sunWorld[3] = {}, g_sunView[3] = {}, g_day = 0.0f;
        int g_viewValid = 0;
        std::string g_problem;

        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Shaders
        {
            Shader vs, apply, applyMs, show, showMs;
        };

        bool shadersFor(int backend, Shaders &s)
        {
#if defined(VCS_MOD_HAVE_DXBC)
            if (backend == VCS_RENDER_BACKEND_D3D11)
            {
                s = {{kRhFullScreenVS, sizeof(kRhFullScreenVS)}, {kRhShadowPS, sizeof(kRhShadowPS)}, {kRhShadowMsPS, sizeof(kRhShadowMsPS)},
                     {kRhShadowShowPS, sizeof(kRhShadowShowPS)}, {kRhShadowShowMsPS, sizeof(kRhShadowShowMsPS)}};
                return true;
            }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
            if (backend == VCS_RENDER_BACKEND_VULKAN)
            {
                s = {{kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)}, {kRhShadowPS_spv, sizeof(kRhShadowPS_spv)},
                     {kRhShadowMsPS_spv, sizeof(kRhShadowMsPS_spv)}, {kRhShadowShowPS_spv, sizeof(kRhShadowShowPS_spv)},
                     {kRhShadowShowMsPS_spv, sizeof(kRhShadowShowMsPS_spv)}};
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
            VcsRenderPipeline *apply[2] = {}, *show[2] = {}; // single-sampled / multisampled Z buffer
            VcsRenderBuffer *constants = nullptr;
            bool failed = false;
        };
        Gpu g_gpu;

        struct Constants // shaders/shadows.hlsl cbuffer Shadows
        {
            float proj[4], zmap[4], sun[4], splits[4], texel[4], zbias[4], fade[4], info[4];
            float light[16][4];
            int32_t size[4];
        };

        void fail(const char *why)
        {
            if (g_problem != why)
                mod::log("[renderhook] shadows off: %s\n", why);
            g_problem = why;
        }

        VcsRenderPipeline *pipeline(const Shader &vs, const Shader &ps, int blend, uint32_t multisampled, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = vs.code;
            d.vertex_size = vs.size;
            d.pixel_code = ps.code;
            d.pixel_size = ps.size;
            d.blend = blend;
            d.write_mask = 0x7u;
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
            g_gpu.apply[0] = pipeline(s.vs, s.apply, VCS_RENDER_BLEND_MULTIPLY, 0u, "renderhook shadows");
            g_gpu.apply[1] = pipeline(s.vs, s.applyMs, VCS_RENDER_BLEND_MULTIPLY, 1u, "renderhook shadows (MSAA)");
            g_gpu.show[0] = pipeline(s.vs, s.show, VCS_RENDER_BLEND_OPAQUE, 0u, "renderhook shadow view");
            g_gpu.show[1] = pipeline(s.vs, s.showMs, VCS_RENDER_BLEND_OPAQUE, 1u, "renderhook shadow view (MSAA)");
            g_gpu.constants = mod::api->render_create_constants(sizeof(Constants), "renderhook shadow constants");
            if (!g_gpu.apply[0] || !g_gpu.apply[1] || !g_gpu.show[0] || !g_gpu.show[1] || !g_gpu.constants)
            {
                g_gpu.failed = true;
                fail("the renderer refused its pipelines");
                return false;
            }
            return true;
        }

        struct V3
        {
            float x, y, z;
        };
        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        V3 normalise(V3 a)
        {
            const float l = std::sqrt(dot(a, a));
            return l > 0.0f ? a * (1.0f / l) : a;
        }
        V3 rotate(const float m[9], V3 v) // row-major
        {
            return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
        }

        // the timecycle's newest sun vector (world: x east, y north, z up), or false if the game's RAM says otherwise
        bool sunWorld(V3 &out)
        {
            const uint8_t *ram = mod::api->rdram ? mod::api->rdram() : nullptr;
            if (!ram)
                return false;
            uint32_t index;
            std::memcpy(&index, ram + kSunIndex, 4);
            if (index > 15u)
                return false;
            float v[3];
            std::memcpy(v, ram + kSunRing + index * 16u, 12);
            const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (!(len2 > 0.9f && len2 < 1.1f))
                return false;
            out = {v[0], v[1], v[2]};
            return true;
        }

        void hook(void *, const VcsRenderFrame *f)
        {
            const int level = g_level.load(std::memory_order_relaxed);
            const bool view = g_view.load(std::memory_order_relaxed) != 0;
            if ((!level && !view) || !f || f->size < offsetof(VcsRenderFrame, world_to_view) + sizeof(f->world_to_view))
                return;
            g_viewValid = f->view_valid;
            V3 sw;
            if (f->backend == VCS_RENDER_BACKEND_NULL || !f->depth || !f->camera_valid || !f->view_valid || !f->width ||
                !f->height || !sunWorld(sw) || !ready(*f))
            {
                ++g_skipped;
                return;
            }
            const V3 s = normalise(rotate(f->world_to_view, sw)); // towards the sun, view space
            g_sunWorld[0] = sw.x, g_sunWorld[1] = sw.y, g_sunWorld[2] = sw.z;
            g_sunView[0] = s.x, g_sunView[1] = s.y, g_sunView[2] = s.z;
            // daylight: the sun above the horizon (below it the vector is still there; the night has no sun shadows)
            const float t = std::clamp(sw.z / 0.2f, 0.0f, 1.0f);
            g_day = t * t * (3.0f - 2.0f * t);
            if (g_day <= 0.0f && !view)
                return;
            const uint32_t n = kSizes[std::clamp(g_quality.load(std::memory_order_relaxed), 0, 2)];
            const float distance = static_cast<float>(std::clamp(g_distance.load(std::memory_order_relaxed), 50, 400));
            // the light basis tied to the world: u from the world's east axis (north if the sun stands along it)
            V3 ref = rotate(f->world_to_view, {1.0f, 0.0f, 0.0f});
            if (std::fabs(dot(ref, s)) > 0.9f)
                ref = rotate(f->world_to_view, {0.0f, 1.0f, 0.0f});
            const V3 u = normalise(ref - s * dot(ref, s)), v = cross(s, u);
            Constants c = {};
            float splits[kCascades + 1];
            splits[0] = kNear;
            for (uint32_t i = 1; i <= kCascades; ++i)
            {
                const float k = static_cast<float>(i) / kCascades;
                splits[i] = kSplitLambda * kNear * std::pow(distance / kNear, k) + (1.0f - kSplitLambda) * (kNear + (distance - kNear) * k);
            }
            float matrices[kCascades][16] = {};
            const float x0 = (0.0f - f->cx) / f->fx, x1 = (static_cast<float>(f->width) - f->cx) / f->fx;
            const float y0 = (0.0f - f->cy) / f->fy, y1 = (static_cast<float>(f->height) - f->cy) / f->fy;
            for (uint32_t i = 0; i < kCascades; ++i)
            {
                // the slice's bounding sphere: centre on the view axis where the near and far corners are equally far
                const float zn = splits[i], zf = splits[i + 1];
                const float rx = std::max(std::fabs(x0), std::fabs(x1)), ry = std::max(std::fabs(y0), std::fabs(y1));
                const float k2 = rx * rx + ry * ry;
                float zc = std::clamp(0.5f * (zn + zf) * (1.0f + k2), zn, zf);
                const float axisX = 0.5f * (x0 + x1), axisY = 0.5f * (y0 + y1);
                float r = 0.0f;
                for (float z : {zn, zf})
                    for (float xs : {x0, x1})
                        for (float ys : {y0, y1})
                        {
                            const V3 corner = {xs * z, ys * z, z}, centre = {axisX * zc, axisY * zc, zc};
                            r = std::max(r, std::sqrt(dot(corner - centre, corner - centre)));
                        }
                r = std::ceil(r * 4.0f) / 4.0f; // steady across small lens changes (the car camera's widening)
                const V3 centre = {axisX * zc, axisY * zc, zc};
                const float texelSize = 2.0f * r / static_cast<float>(n);
                // snapped to whole texels in the light's plane, measured from the camera: steady while the camera turns
                const float cu = std::floor(dot(u, centre) / texelSize) * texelSize;
                const float cv = std::floor(dot(v, centre) / texelSize) * texelSize;
                const float cs = dot(s, centre);
                const float back = r, range = r + r + kCasterReach;
                float *m = matrices[i];
                m[0] = u.x / r, m[1] = u.y / r, m[2] = u.z / r, m[3] = -cu / r;
                m[4] = v.x / r, m[5] = v.y / r, m[6] = v.z / r, m[7] = -cv / r;
                m[8] = s.x / range, m[9] = s.y / range, m[10] = s.z / range, m[11] = (back - cs) / range;
                m[15] = 1.0f;
                for (int row = 0; row < 4; ++row)
                    for (int k = 0; k < 4; ++k)
                        c.light[i * 4 + row][k] = m[row * 4 + k];
                c.splits[i] = zf;
                c.texel[i] = texelSize;
                c.zbias[i] = 0.5f * texelSize / range;
            }
            const VcsModApi &a = *mod::api;
            a.render_push_marker("renderhook shadows");
            uint32_t drawn = 0;
            VcsRenderTexture *map = a.render_shadow_map(&matrices[0][0], kCascades, n, &drawn);
            g_drawn = drawn;
            if (!map)
            {
                a.render_pop_marker();
                ++g_skipped;
                fail("the renderer drew no shadow map");
                return;
            }
            c.proj[0] = f->fx, c.proj[1] = f->fy, c.proj[2] = f->cx, c.proj[3] = f->cy;
            c.zmap[0] = f->depth_a, c.zmap[1] = f->depth_b, c.zmap[2] = f->depth_scale, c.zmap[3] = kFarthest;
            c.sun[0] = s.x, c.sun[1] = s.y, c.sun[2] = s.z;
            c.sun[3] = kStrength[std::clamp(level, 0, 3)] * g_day;
            c.fade[0] = f->fog_bias, c.fade[1] = f->fog_scale, c.fade[2] = 0.8f * distance, c.fade[3] = distance;
            c.info[0] = static_cast<float>(kCascades), c.info[1] = static_cast<float>(n), c.info[2] = kBlendBand;
            c.size[0] = static_cast<int32_t>(f->width);
            c.size[1] = static_cast<int32_t>(f->height);
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));
            const int ms = f->depth_samples > 1u ? 1 : 0;
            a.render_begin_pass(f->color);
            a.render_set_viewport(0, 0, static_cast<int>(f->width), static_cast<int>(f->height));
            a.render_set_pipeline(view ? g_gpu.show[ms] : g_gpu.apply[ms]);
            a.render_set_constants(0, g_gpu.constants);
            a.render_set_texture(0, f->depth, VCS_RENDER_SAMPLER_POINT);
            a.render_set_texture(1, map, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
            a.render_pop_marker();
            ++g_frames;
            g_problem.clear();
        }
    }

    void hookShadows()
    {
        if (g_hooked || !VCS_MOD_API_HAS(mod::api, render_shadow_map))
            return;
        g_hooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &hook, nullptr, VCS_RENDER_NEEDS_DEPTH | VCS_RENDER_NEEDS_SHADOW) != 0;
        if (!g_hooked)
            mod::log("[renderhook] the renderer refused the shadows' render hook\n");
    }

    bool shadowsOn() { return g_level.load(std::memory_order_relaxed) > 0 || g_view.load(std::memory_order_relaxed) != 0; }

    void registerShadows()
    {
        // VCS_RH_SHADOW_VIEW=1: the debug view (lit / shadowed as grey, a tint per cascade) from the start
        if (const char *e = std::getenv("VCS_RH_SHADOW_VIEW"); e && std::atoi(e) != 0)
        {
            g_view = 1;
            hookShadows();
        }
        // rhshadow [view 0|1] -> the settings, frames, the sun (world, view), daylight, casters drawn
        mod::addCommand("rhshadow", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("view", 0) == 0)
            {
                g_view = std::atoi(args.c_str() + 4) != 0 ? 1 : 0;
                if (g_view)
                    hookShadows();
            }
            char line[400];
            std::snprintf(line, sizeof(line),
                          "ok level %d view %d hooked %d frames %llu skipped %llu view_valid %d sun_world %.3f %.3f %.3f sun_view %.3f "
                          "%.3f %.3f day %.2f casters %u%s%s",
                          g_level.load(), g_view.load(), g_hooked ? 1 : 0, static_cast<unsigned long long>(g_frames),
                          static_cast<unsigned long long>(g_skipped), g_viewValid, g_sunWorld[0], g_sunWorld[1], g_sunWorld[2], g_sunView[0],
                          g_sunView[1], g_sunView[2], g_day, g_drawn, g_problem.empty() ? "" : " problem ", g_problem.c_str());
            reply(line);
        });
    }

    void registerShadowSettings()
    {
        if (!VCS_MOD_API_HAS(mod::api, render_shadow_map))
            return; // this vcs.exe cannot draw shadow maps
        mod::Spec s;
        s.key = "renderhook.shadows";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Sun shadows";
        s.help = "Shadows of buildings, trees, cars and people from the sun, in daylight (shadow maps of what the game draws). "
                 "Objects off screen cast none. Off is the original.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off (original)", 0}, {"Soft", 1}, {"Normal", 2}, {"Dark", 3}};
        s.defaultValue = 0;
        s.env = "VCS_RH_SHADOWS";
        s.onChange = [](int value) {
            g_level = std::clamp(value, 0, 3);
            if (value > 0)
                hookShadows();
        };
        mod::add(s);

        s = {};
        s.key = "renderhook.shadow_distance";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Shadow distance";
        s.help = "How far from the camera shadows are drawn. Farther spreads the shadow maps' detail thinner.";
        s.type = mod::Type::Integer;
        s.min = 50;
        s.max = 400;
        s.format = "%d m";
        s.defaultValue = 150;
        s.env = "VCS_RH_SHADOW_DISTANCE";
        s.onChange = [](int value) { g_distance = std::clamp(value, 50, 400); };
        mod::add(s);

        s = {};
        s.key = "renderhook.shadow_quality";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Shadow quality";
        s.help = "The shadow maps' resolution (three of them): sharper edges cost GPU time and memory.";
        s.type = mod::Type::Choice;
        s.choices = {{"Low (1024)", 0}, {"Medium (2048)", 1}, {"High (4096)", 2}};
        s.defaultValue = 1;
        s.env = "VCS_RH_SHADOW_QUALITY";
        s.onChange = [](int value) { g_quality = std::clamp(value, 0, 2); };
        mod::add(s);
    }
}
