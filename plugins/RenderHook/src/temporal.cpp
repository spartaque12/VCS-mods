// See renderhook.h: temporal AA groundwork for DLAA - the renderer's jitter (render_set_jitter) and camera motion vectors
// from the Z buffer and the frame's reprojection (VcsRenderFrame.reproject). Debug views for now (shaders/temporal.hlsl):
// VCS_RH_MV_VIEW / `rhtaa mv N` = 1 motion vectors as colour, 2 the previous picture warped by them minus the current one
// (black where right), 3 the same unwarped; VCS_RH_JITTER / `rhtaa jitter N` = an N-point jitter sequence.
#include "renderhook.h"

#include "mod.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhMotionViewPS.h"
#include "dxbc/kRhMotionViewMsPS.h"
#include "dxbc/kRhReprojectPS.h"
#include "dxbc/kRhReprojectMsPS.h"
#include "dxbc/kRhCopyPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhMotionViewPS.h"
#include "spirv/kRhMotionViewMsPS.h"
#include "spirv/kRhReprojectPS.h"
#include "spirv/kRhReprojectMsPS.h"
#include "spirv/kRhCopyPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        constexpr float kFarthest = 100000.0f;
        std::atomic<int> g_mode{0}, g_objects{1};
        uint32_t g_jitter = 0;
        bool g_hooked = false;
        uint64_t g_frames = 0, g_noReprojection = 0;
        VcsRenderFrame g_last = {};
        bool g_haveReproject = false; // this exe's frames carry the temporal fields
        std::string g_problem;

        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Gpu
        {
            int backend = -1;
            VcsRenderPipeline *motion[2] = {}, *reproject[2] = {}, *copy = nullptr;
            VcsRenderBuffer *constants = nullptr;
            VcsRenderTexture *previous = nullptr;
            uint32_t width = 0, height = 0;
            bool havePrevious = false, failed = false;
        };
        Gpu g_gpu;

        struct Constants // shaders/temporal.hlsl cbuffer Temporal
        {
            float q[16], zmap[4], jitter[4];
            int32_t size[4];
        };

        VcsRenderPipeline *pipeline(const Shader &vs, const Shader &ps, uint32_t multisampled, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = vs.code;
            d.vertex_size = vs.size;
            d.pixel_code = ps.code;
            d.pixel_size = ps.size;
            d.blend = VCS_RENDER_BLEND_OPAQUE;
            d.write_mask = multisampled == ~0u ? 0xFu : 0x7u;
            d.multisampled_textures = multisampled == ~0u ? 0u : multisampled;
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
                Shader vs, mv, mvMs, rp, rpMs, cp;
#if defined(VCS_MOD_HAVE_DXBC)
                if (f.backend == VCS_RENDER_BACKEND_D3D11)
                {
                    vs = {kRhFullScreenVS, sizeof(kRhFullScreenVS)};
                    mv = {kRhMotionViewPS, sizeof(kRhMotionViewPS)};
                    mvMs = {kRhMotionViewMsPS, sizeof(kRhMotionViewMsPS)};
                    rp = {kRhReprojectPS, sizeof(kRhReprojectPS)};
                    rpMs = {kRhReprojectMsPS, sizeof(kRhReprojectMsPS)};
                    cp = {kRhCopyPS, sizeof(kRhCopyPS)};
                }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
                if (f.backend == VCS_RENDER_BACKEND_VULKAN)
                {
                    vs = {kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)};
                    mv = {kRhMotionViewPS_spv, sizeof(kRhMotionViewPS_spv)};
                    mvMs = {kRhMotionViewMsPS_spv, sizeof(kRhMotionViewMsPS_spv)};
                    rp = {kRhReprojectPS_spv, sizeof(kRhReprojectPS_spv)};
                    rpMs = {kRhReprojectMsPS_spv, sizeof(kRhReprojectMsPS_spv)};
                    cp = {kRhCopyPS_spv, sizeof(kRhCopyPS_spv)};
                }
#endif
                if (!vs.code)
                {
                    g_gpu.failed = true;
                    g_problem = "no shaders for this graphics API";
                    return false;
                }
                g_gpu.motion[0] = pipeline(vs, mv, 0u, "renderhook motion view");
                g_gpu.motion[1] = pipeline(vs, mvMs, 1u, "renderhook motion view (MSAA)");
                g_gpu.reproject[0] = pipeline(vs, rp, 0u, "renderhook reprojection check");
                g_gpu.reproject[1] = pipeline(vs, rpMs, 1u, "renderhook reprojection check (MSAA)");
                g_gpu.copy = pipeline(vs, cp, ~0u, "renderhook previous picture");
                g_gpu.constants = a.render_create_constants(sizeof(Constants), "renderhook temporal constants");
                if (!g_gpu.motion[0] || !g_gpu.motion[1] || !g_gpu.reproject[0] || !g_gpu.reproject[1] || !g_gpu.copy || !g_gpu.constants)
                {
                    g_gpu.failed = true;
                    g_problem = "pipelines refused";
                    return false;
                }
            }
            if (g_gpu.width != f.width || g_gpu.height != f.height || !g_gpu.previous)
            {
                if (g_gpu.previous)
                    a.render_destroy_texture(g_gpu.previous);
                g_gpu.previous = a.render_create_texture(f.width, f.height, VCS_RENDER_FORMAT_RGBA8, "renderhook previous picture");
                g_gpu.width = f.width;
                g_gpu.height = f.height;
                g_gpu.havePrevious = false;
            }
            return g_gpu.previous != nullptr;
        }

        void draw(VcsRenderTexture *target, VcsRenderPipeline *p, const VcsRenderFrame &f, VcsRenderTexture *t0, VcsRenderTexture *t1,
                  VcsRenderTexture *t2)
        {
            const VcsModApi &a = *mod::api;
            a.render_begin_pass(target);
            a.render_set_viewport(0, 0, static_cast<int>(f.width), static_cast<int>(f.height));
            if (f.size >= offsetof(VcsRenderFrame, velocity) + sizeof(f.velocity) && f.velocity)
                a.render_set_texture(3, f.velocity, VCS_RENDER_SAMPLER_POINT);
            a.render_set_pipeline(p);
            a.render_set_constants(0, g_gpu.constants);
            if (t0)
                a.render_set_texture(0, t0, VCS_RENDER_SAMPLER_POINT);
            if (t1)
                a.render_set_texture(1, t1, VCS_RENDER_SAMPLER_LINEAR);
            if (t2)
                a.render_set_texture(2, t2, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
        }

        void hook(void *, const VcsRenderFrame *f)
        {
            const int mode = g_mode.load(std::memory_order_relaxed);
            if (!mode || !f || f->size < offsetof(VcsRenderFrame, reproject) + sizeof(f->reproject))
                return;
            g_last = *f;
            if (!f->depth || !f->color_resolved || !f->camera_valid || f->backend == VCS_RENDER_BACKEND_NULL || !ready(*f))
                return;
            if (!f->reproject_valid)
                ++g_noReprojection;
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
            c.size[2] = mode;
            const bool haveVelocity = f->size >= offsetof(VcsRenderFrame, velocity) + sizeof(f->velocity) && f->velocity;
            c.size[3] = haveVelocity && g_objects.load(std::memory_order_relaxed) ? 1 : 0;            const VcsModApi &a = *mod::api;
            a.render_update_constants(g_gpu.constants, &c, sizeof(c));
            a.render_push_marker("renderhook temporal");
            const int ms = f->depth_samples > 1u ? 1 : 0;
            if (mode == 1 || mode == 4)
                draw(f->color, g_gpu.motion[ms], *f, f->depth, nullptr, nullptr);
            else if (g_gpu.havePrevious)
                draw(f->color, g_gpu.reproject[ms], *f, f->depth, g_gpu.previous, f->color_resolved);
            // this frame's picture (as the 3D left it) for the next frame's check
            draw(g_gpu.previous, g_gpu.copy, *f, nullptr, nullptr, f->color_resolved);
            g_gpu.havePrevious = true;
            a.render_pop_marker();
            ++g_frames;
        }

        void setMode(int mode)
        {
            g_mode = mode;
            if (mode && !g_hooked && g_haveReproject)
                g_hooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &hook, nullptr,
                                                     VCS_RENDER_NEEDS_DEPTH | VCS_RENDER_NEEDS_RESOLVED_COLOR | VCS_RENDER_NEEDS_VELOCITY) != 0;
        }

        void setJitter(uint32_t phases)
        {
            g_jitter = phases;
            if (VCS_MOD_API_HAS(mod::api, render_set_jitter))
                mod::api->render_set_jitter(phases);
        }
    }

    void registerTemporal()
    {
        g_haveReproject = VCS_MOD_API_HAS(mod::api, render_set_jitter);
        if (const char *e = std::getenv("VCS_RH_JITTER"))
            setJitter(static_cast<uint32_t>(std::atoi(e)));
        if (const char *e = std::getenv("VCS_RH_MV_VIEW"))
            setMode(std::atoi(e));
        // rhtaa [mv 0..3 | jitter N] -> status: the debug view, the jitter, frames with / without a reprojection
        mod::addCommand("rhtaa", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("mv", 0) == 0)
                setMode(std::atoi(args.c_str() + 2));
            else if (args.rfind("objects", 0) == 0)
                g_objects = std::atoi(args.c_str() + 7) != 0 ? 1 : 0;
            else if (args.rfind("jitter", 0) == 0)
                setJitter(static_cast<uint32_t>(std::atoi(args.c_str() + 6)));
            const VcsRenderFrame &f = g_last;
            char line[400];
            std::snprintf(line, sizeof(line), "ok mv %d objects %d jitter %u hooked %d api %d frames %llu noreproject %llu jit %.3f %.3f prev %.3f %.3f valid %d%s%s",
                          g_mode.load(), g_objects.load(), g_jitter, g_hooked ? 1 : 0, g_haveReproject ? 1 : 0, static_cast<unsigned long long>(g_frames),
                          static_cast<unsigned long long>(g_noReprojection), f.jitter_x, f.jitter_y, f.prev_jitter_x, f.prev_jitter_y,
                          f.reproject_valid, g_problem.empty() ? "" : " problem: ", g_problem.c_str());
            reply(line);
        });
    }
}
