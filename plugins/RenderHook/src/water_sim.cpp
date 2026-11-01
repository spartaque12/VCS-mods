// See water_sim.h. Two halves:
//
// The game thread (a hook before CGame::Process, FUN_003BA8D8, so the frame's entities and camera are the last frame's,
// processed together): the camera, the game's clock, and every vehicle and ped in the water from the pools - position,
// speed, heading, kind - into a snapshot; a vehicle or ped that has just entered the water adds a splash. Hooks before
// CExplosion::AddExplosion (FUN_002E1140) and the shot's water test (FUN_002C3CE8) add splashes for explosions at the
// water's level and bullets that reach the water. Hook rules (AGENTS.md): each reads its arguments, then calls the
// original. The port's docs/game-internals.md 12x.
//
// The render hook (main thread, VCS_RENDER_BEFORE_POST): the field follows the camera in whole texels; the game's time
// since the last frame (CTimer, standing still while paused) is run in fixed 1/60 s steps (at most 4 a frame), each a
// full-screen pass ping-ponging two RGBA16F states with that step's sources - each moving source a capsule from where
// it was to where it is, so fast boats leave a continuous trail. Then the output pass (slopes, height, foam). The game
// thread may run up to three frames ahead of the main thread: entities are placed relative to the camera of their own
// snapshot, which keeps the player's boat under its wake whatever the lag.
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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(VCS_MOD_HAVE_DXBC)
using BYTE = unsigned char;
#include "dxbc/kRhFullScreenVS.h"
#include "dxbc/kRhWakeStepPS.h"
#include "dxbc/kRhWakeOutPS.h"
#include "dxbc/kRhWakeShowPS.h"
#include "dxbc/kRhWakeWaterPS.h"
#include "dxbc/kRhWakeWaterMsPS.h"
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
#include "spirv/kRhFullScreenVS.h"
#include "spirv/kRhWakeStepPS.h"
#include "spirv/kRhWakeOutPS.h"
#include "spirv/kRhWakeShowPS.h"
#include "spirv/kRhWakeWaterPS.h"
#include "spirv/kRhWakeWaterMsPS.h"
#endif

namespace vcs::renderhook
{
    namespace
    {
        // ---- the game (docs/game-internals.md 12, 12e, 12x) --------------------------------------------------------
        constexpr uint32_t kGameProcess = 0x003BA8D8u;  // CGame::Process
        constexpr uint32_t kAddExplosion = 0x002E1140u; // CExplosion::AddExplosion(victim, creator, type a2, &pos a3, ...)
        constexpr uint32_t kShotWater = 0x002C3CE8u;    // the shot's water test (?, &start a1, &end a2, colpoint a3, hit t0)
        constexpr uint32_t kPedPool = 0x00487A94u, kVehiclePool = 0x00487A98u; // CPool *: entries, flags, size
        constexpr uint32_t kPedSize = 0xD20u, kVehicleSize = 0x8C0u;
        constexpr uint32_t kTheCamera = 0x006F44D0u;    // its matrix: right +0, forward +0x10, up +0x20, position +0x30
        constexpr uint32_t kTimerMs = 0x004CD104u;      // CTimer: game milliseconds (stands still while paused)
        constexpr uint32_t kArea = 0x00489F7Cu;         // CGame::currArea (0 outdoors)
        constexpr uint32_t kWaterTable = 0x004874C0u;   // CWaterLevel's: +4 heights (float[]), +0x100C 128 x 128 cells
        // entity fields: +0x00 right, +0x10 forward, +0x30 position, +0xED bit 0 in the water, +0x140 move speed (x 50 =
        // m/s); vehicles +0x354 type (0 car, 1 boat, 2 jetski, 4 heli, 5 plane, 6 bike, 8 BMX, 9 quad); peds +0x480 the
        // vehicle it is in
        constexpr uint32_t kInWater = 0xEDu, kMoveSpeed = 0x140u, kVehicleType = 0x354u, kPedVehicle = 0x480u;

        constexpr int kMaxSources = 32;
        constexpr float kStep = 1.0f / 60.0f;

        inline uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        inline float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }

        // CWaterLevel::GetWaterLevel (FUN_0026EE50) without its waves: 32 m cells from (-2448, -2048), 0x80 = no water;
        // outside the table the sea (6.0)
        bool waterLevel(const uint8_t *ram, float x, float y, float &level)
        {
            level = 6.0f;
            const uint32_t table = rd32(ram, kWaterTable);
            const int xi = static_cast<int>(std::floor((x + 2448.0f) / 32.0f));
            const int yi = static_cast<int>(std::floor((y + 2048.0f) / 32.0f));
            if (!table || xi < 0 || yi < 0 || xi > 127 || yi > 127)
                return true;
            const uint8_t c = ram[(table + 0x100Cu + static_cast<uint32_t>(xi) * 128u + static_cast<uint32_t>(yi)) & 0x01FFFFFFu];
            if (c == 0x80u)
                return false;
            level = rdf(ram, rd32(ram, table + 4u) + c * 4u);
            return std::isfinite(level) && level > -100.0f && level < 200.0f;
        }

        // ---- the snapshot ----------------------------------------------------------------------------------------
        enum Kind : uint8_t
        {
            kBoat,
            kJetski,
            kCar,
            kBike,
            kSwimmer,
            kOther
        };
        struct Entity
        {
            uint32_t id; // its address
            Kind kind;
            float pos[3], vel[3], fwd[2];
        };
        struct Splash
        {
            float pos[3];
            float radius, depth, foam;
        };
        struct Snapshot
        {
            uint64_t serial = 0;
            uint32_t ms = 0;
            bool outdoors = false;
            float cam[3] = {}, right[3] = {}, fwd[3] = {}, up[3] = {};
            float level = 6.0f;
            bool hasLevel = false;
            std::vector<Entity> entities;
        };

        std::atomic<int> g_quality{0}; // renderhook.wakes: 0 off, 1 low (256 x 0.5 m), 2 high (512 x 0.375 m)
        std::atomic<int> g_view{0};    // 1 the field in a corner, 2 the stand-in water shading, 3 both
        std::mutex g_lock;
        Snapshot g_snap;                  // the latest (g_lock)
        std::vector<Splash> g_splashes;   // since the render side last took them (g_lock)
        struct Seen // the game thread's memory of an entity from the last frame
        {
            bool wet;      // touching the water (see scanPool)
            float pos[3];
            uint32_t ms, nextStroke;
            float fall;    // how fast it came down (m/s) while out of the water, easing off
        };
        std::unordered_map<uint32_t, Seen> g_seen; // game thread only
        uint64_t g_bullets = 0, g_explosions = 0, g_entered = 0;
        VcsGuestFn g_origProcess = nullptr, g_origExplosion = nullptr, g_origShot = nullptr;

        void addSplash(const Splash &s)
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_splashes.size() < 256u)
                g_splashes.push_back(s);
        }

        void scanPool(const uint8_t *ram, uint32_t poolPtr, uint32_t stride, bool peds, Snapshot &s,
                      std::unordered_map<uint32_t, Seen> &seen)
        {
            const uint32_t pool = rd32(ram, poolPtr);
            if (!pool)
                return;
            const uint32_t entries = rd32(ram, pool), flags = rd32(ram, pool + 4u);
            const uint32_t count = std::min(rd32(ram, pool + 8u), 512u);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (ram[(flags + i) & 0x01FFFFFFu] & 0x80u)
                    continue;
                const uint32_t e = entries + i * stride;
                const bool inWater = (ram[(e + kInWater) & 0x01FFFFFFu] & 1u) != 0;
                if (peds && rd32(ram, e + kPedVehicle))
                    continue; // in a vehicle: the vehicle makes the wake
                Entity en = {};
                en.id = e;
                for (int k = 0; k < 3; ++k)
                {
                    en.pos[k] = rdf(ram, e + 0x30u + 4u * k);
                    en.vel[k] = rdf(ram, e + kMoveSpeed + 4u * k) * 50.0f;
                }
                if (!std::isfinite(en.pos[0]) || !std::isfinite(en.pos[1]) || !std::isfinite(en.pos[2]) || !std::isfinite(en.vel[2]))
                    continue;
                en.fwd[0] = rdf(ram, e + 0x10u);
                en.fwd[1] = rdf(ram, e + 0x14u);
                if (peds)
                    en.kind = kSwimmer;
                else
                    switch (rd32(ram, e + kVehicleType))
                    {
                    case 1: en.kind = kBoat; break;
                    case 2: en.kind = kJetski; break;
                    case 0: case 9: en.kind = kCar; break;
                    case 6: case 8: en.kind = kBike; break;
                    default: en.kind = kOther; break;
                    }
                // peds' move speed stays 0 while they swim: their speed from where they were the last frame
                auto it = g_seen.find(e);
                const bool known = it != g_seen.end() && s.ms > it->second.ms && s.ms - it->second.ms < 500u;
                if (peds)
                {
                    for (int k = 0; k < 3; ++k)
                        en.vel[k] = known ? (en.pos[k] - it->second.pos[k]) * 1000.0f / static_cast<float>(s.ms - it->second.ms) : 0.0f;
                    if (std::hypot(en.vel[0], en.vel[1]) > 20.0f)
                        en.vel[0] = en.vel[1] = en.vel[2] = 0.0f; // a teleport
                }
                Seen now = {false, {en.pos[0], en.pos[1], en.pos[2]}, s.ms, it != g_seen.end() ? it->second.nextStroke : 0u, 0.0f};
                // a swimmer's strokes: a ring every 0.75 s while moving, a small one every 1.8 s treading water
                if (peds && inWater && s.ms >= now.nextStroke)
                {
                    const bool moving = std::hypot(en.vel[0], en.vel[1]) > 0.3f;
                    if (now.nextStroke)
                        addSplash({{en.pos[0], en.pos[1], en.pos[2]}, moving ? 0.8f : 0.6f, moving ? 0.1f : 0.035f, moving ? 0.3f : 0.05f});
                    now.nextStroke = s.ms + (moving ? 750u : 1800u);
                }
                // the flag is set over water already (a car dropped from 12 m had it in its first frame): what touches the
                // water is near the level
                float level = 6.0f;
                const bool wet = inWater && waterLevel(ram, en.pos[0], en.pos[1], level) && en.pos[2] < level + 1.5f &&
                                 en.pos[2] > level - 6.0f;
                // entering the water: the game sets its flag a few frames after the fall has met the water (and slowed
                // it), so the speed it came down with is kept from while it was out of the water
                now.wet = wet;
                const float downNow = std::max(-en.vel[2], 0.0f) + 0.15f * std::hypot(en.vel[0], en.vel[1]);
                const float fallBefore = known ? it->second.fall : 0.0f;
                now.fall = wet ? 0.0f : std::max(downNow, fallBefore * 0.85f);
                if (wet && known && !it->second.wet)
                {
                    const float down = std::max(downNow, fallBefore);
                    const bool small = peds || en.kind == kBike || en.kind == kJetski;
                    if (down > 1.5f)
                    {
                        addSplash({{en.pos[0], en.pos[1], level}, small ? 1.0f : 2.4f,
                                   std::clamp(down * (small ? 0.04f : 0.07f), 0.08f, small ? 0.4f : 1.2f), small ? 0.6f : 1.0f});
                        ++g_entered;
                    }
                }
                seen[e] = now;
                if (wet)
                    s.entities.push_back(en);
            }
        }

        void snapshot(const uint8_t *ram)
        {
            Snapshot s;
            s.ms = rd32(ram, kTimerMs);
            s.outdoors = rd32(ram, kArea) == 0u;
            for (int k = 0; k < 3; ++k)
            {
                s.right[k] = rdf(ram, kTheCamera + 4u * k);
                s.fwd[k] = rdf(ram, kTheCamera + 0x10u + 4u * k);
                s.up[k] = rdf(ram, kTheCamera + 0x20u + 4u * k);
                s.cam[k] = rdf(ram, kTheCamera + 0x30u + 4u * k);
            }
            s.hasLevel = waterLevel(ram, s.cam[0], s.cam[1], s.level);
            std::unordered_map<uint32_t, Seen> seen;
            if (s.outdoors)
            {
                scanPool(ram, kVehiclePool, kVehicleSize, false, s, seen);
                scanPool(ram, kPedPool, kPedSize, true, s, seen);
            }
            g_seen.swap(seen);
            std::lock_guard<std::mutex> lk(g_lock);
            s.serial = g_snap.serial + 1u;
            g_snap = std::move(s);
        }

        bool simWanted() { return g_quality.load(std::memory_order_relaxed) > 0 || g_view.load(std::memory_order_relaxed) != 0; }

        void VCS_GUEST_CC gameProcess(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (simWanted())
                snapshot(rdram);
            g_origProcess(rdram, ctx, runtime);
        }

        void VCS_GUEST_CC addExplosion(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (simWanted())
            {
                const uint32_t p = mod::gpr32(ctx, 7);
                const float x = rdf(rdram, p), y = rdf(rdram, p + 4u), z = rdf(rdram, p + 8u);
                float level;
                if (p && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && waterLevel(rdram, x, y, level) &&
                    std::fabs(z - level) < 4.0f)
                {
                    const float near = 1.0f - std::fabs(z - level) / 4.0f;
                    addSplash({{x, y, level}, 3.5f, 0.9f * near + 0.2f, 1.0f});
                    ++g_explosions;
                }
            }
            g_origExplosion(rdram, ctx, runtime);
        }

        // the shot's water test (FUN_002C3CE8), done again on our side: the line from a1 to a2 crosses z 6.2 above water
        // (a cell with water), and an earlier hit (t0 non-zero: the colpoint at a3) is under the water's level
        void VCS_GUEST_CC shotWater(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (simWanted())
            {
                const uint32_t a = mod::gpr32(ctx, 5), b = mod::gpr32(ctx, 6), hit = mod::gpr32(ctx, 7);
                const bool hadHit = mod::gpr32(ctx, 8) != 0;
                const float z0 = 6.2f;
                const float sz = rdf(rdram, a + 8u), ez = rdf(rdram, b + 8u);
                if (a && b && sz > z0 && ez <= z0 && (!hadHit || rdf(rdram, hit + 8u) <= z0))
                {
                    const float t = (z0 - sz) / (ez - sz);
                    const float x = rdf(rdram, a) + (rdf(rdram, b) - rdf(rdram, a)) * t;
                    const float y = rdf(rdram, a + 4u) + (rdf(rdram, b + 4u) - rdf(rdram, a + 4u)) * t;
                    float level;
                    if (std::isfinite(x) && std::isfinite(y) && waterLevel(rdram, x, y, level) && (!hadHit || rdf(rdram, hit + 8u) < level))
                    {
                        addSplash({{x, y, level}, 0.35f, 0.12f, 0.5f});
                        ++g_bullets;
                    }
                }
            }
            g_origShot(rdram, ctx, runtime);
        }

        // ---- the GPU ---------------------------------------------------------------------------------------------
        struct Shader
        {
            const void *code = nullptr;
            size_t size = 0;
        };
        struct Shaders
        {
            Shader vs, step, out, show, water, waterMs;
        };
        bool shadersFor(int backend, Shaders &s)
        {
#if defined(VCS_MOD_HAVE_DXBC)
            if (backend == VCS_RENDER_BACKEND_D3D11)
            {
                s = {{kRhFullScreenVS, sizeof(kRhFullScreenVS)}, {kRhWakeStepPS, sizeof(kRhWakeStepPS)},
                     {kRhWakeOutPS, sizeof(kRhWakeOutPS)},       {kRhWakeShowPS, sizeof(kRhWakeShowPS)},
                     {kRhWakeWaterPS, sizeof(kRhWakeWaterPS)},   {kRhWakeWaterMsPS, sizeof(kRhWakeWaterMsPS)}};
                return true;
            }
#endif
#if defined(VCS_MOD_HAVE_SPIRV)
            if (backend == VCS_RENDER_BACKEND_VULKAN)
            {
                s = {{kRhFullScreenVS_spv, sizeof(kRhFullScreenVS_spv)}, {kRhWakeStepPS_spv, sizeof(kRhWakeStepPS_spv)},
                     {kRhWakeOutPS_spv, sizeof(kRhWakeOutPS_spv)},       {kRhWakeShowPS_spv, sizeof(kRhWakeShowPS_spv)},
                     {kRhWakeWaterPS_spv, sizeof(kRhWakeWaterPS_spv)},   {kRhWakeWaterMsPS_spv, sizeof(kRhWakeWaterMsPS_spv)}};
                return true;
            }
#endif
            (void)backend;
            (void)s;
            return false;
        }

        struct StepConstants // shaders/water_sim.hlsl cbuffer WakeStep
        {
            float grid[4], steps[4], more[4];
            int32_t shift[4];
            float seg[kMaxSources][4], shape[kMaxSources][4];
        };
        struct ViewConstants // cbuffer WakeView
        {
            float proj[4], zmap[4], r0[4], r1[4], r2[4], field[4], rect[4], look[4];
        };

        struct Gpu
        {
            int backend = -1;
            bool failed = false;
            VcsRenderPipeline *step = nullptr, *out = nullptr, *show = nullptr, *water[2] = {};
            VcsRenderBuffer *stepC[5] = {}, *viewC = nullptr; // [4]: the output pass / reset
            VcsRenderTexture *state[2] = {}, *field = nullptr;
            int n = 0, cur = 0;
        };
        Gpu g_gpu;

        // the render side's state
        struct Track
        {
            float x, y;
            uint64_t seen;
        };
        struct Sim
        {
            bool started = false;
            int64_t cx = 0, cy = 0; // the field's centre in texels of the world
            float dx = 0.5f;
            uint32_t lastMs = 0;
            float acc = 0.0f;
            uint64_t lastSerial = 0, lastFrame = ~0ull, frames = 0, steps = 0, resets = 0;
            std::unordered_map<uint32_t, Track> tracks;
            WakeField result = {};
            int lastSources = 0;
            std::string problem;
        };
        Sim g_sim;

        void fail(const char *why)
        {
            if (g_sim.problem.empty())
                mod::log("[renderhook] wakes off: %s\n", why);
            g_sim.problem = why;
            g_gpu.failed = true;
        }

        VcsRenderPipeline *pipeline(const Shader &vs, const Shader &ps, int blend, uint32_t mask, uint32_t ms, const char *name)
        {
            VcsRenderPipelineDesc d = {};
            d.size = sizeof(d);
            d.vertex_code = vs.code;
            d.vertex_size = vs.size;
            d.pixel_code = ps.code;
            d.pixel_size = ps.size;
            d.blend = blend;
            d.write_mask = mask;
            d.multisampled_textures = ms;
            d.name = name;
            return mod::api->render_create_pipeline(&d);
        }

        bool ready(const VcsRenderFrame &f, int n)
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
                    fail("this build of RenderHook.asi has no shaders for the graphics API in use");
                    return false;
                }
                g_gpu.step = pipeline(s.vs, s.step, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook wake step");
                g_gpu.out = pipeline(s.vs, s.out, VCS_RENDER_BLEND_OPAQUE, 0xFu, 0u, "renderhook wake out");
                g_gpu.show = pipeline(s.vs, s.show, VCS_RENDER_BLEND_OPAQUE, 0x7u, 0u, "renderhook wake view");
                g_gpu.water[0] = pipeline(s.vs, s.water, VCS_RENDER_BLEND_ALPHA, 0x7u, 0u, "renderhook wake water");
                g_gpu.water[1] = pipeline(s.vs, s.waterMs, VCS_RENDER_BLEND_ALPHA, 0x7u, 1u << 1, "renderhook wake water (MSAA)");
                for (auto &c : g_gpu.stepC)
                    c = a.render_create_constants(sizeof(StepConstants), "renderhook wake step");
                g_gpu.viewC = a.render_create_constants(sizeof(ViewConstants), "renderhook wake view");
                bool ok = g_gpu.step && g_gpu.out && g_gpu.show && g_gpu.water[0] && g_gpu.water[1] && g_gpu.viewC;
                for (auto *c : g_gpu.stepC)
                    ok = ok && c;
                if (!ok)
                {
                    fail("the renderer refused its pipelines");
                    return false;
                }
            }
            if (g_gpu.n != n || !g_gpu.field)
            {
                for (auto *&t : g_gpu.state)
                    if (t)
                        a.render_destroy_texture(t), t = nullptr;
                if (g_gpu.field)
                    a.render_destroy_texture(g_gpu.field), g_gpu.field = nullptr;
                const uint32_t un = static_cast<uint32_t>(n);
                g_gpu.state[0] = a.render_create_texture(un, un, VCS_RENDER_FORMAT_RGBA16F, "renderhook wake state 0");
                g_gpu.state[1] = a.render_create_texture(un, un, VCS_RENDER_FORMAT_RGBA16F, "renderhook wake state 1");
                g_gpu.field = a.render_create_texture(un, un, VCS_RENDER_FORMAT_RGBA16F, "renderhook wake field");
                g_gpu.n = n;
                g_sim.started = false;
                if (!g_gpu.state[0] || !g_gpu.state[1] || !g_gpu.field)
                {
                    fail("the renderer refused its textures");
                    return false;
                }
            }
            return true;
        }

        void fullPass(VcsRenderTexture *target, VcsRenderPipeline *p, VcsRenderBuffer *c, VcsRenderTexture *src)
        {
            const VcsModApi &a = *mod::api;
            a.render_begin_pass(target);
            a.render_set_viewport(0, 0, g_gpu.n, g_gpu.n);
            a.render_set_pipeline(p);
            a.render_set_constants(0, c);
            a.render_set_texture(0, src, VCS_RENDER_SAMPLER_POINT);
            a.render_draw(3);
            a.render_end_pass();
        }

        // per kind: hull length and half-beam (m), depth per m/s and at most, foam at speed
        struct Hull
        {
            float length, radius, depthPerSpeed, depthMax, idleDepth, foamSpeed;
        };
        constexpr Hull kHulls[] = {
            {7.0f, 1.3f, 0.035f, 0.45f, 0.03f, 12.0f}, // boat
            {2.4f, 0.55f, 0.03f, 0.3f, 0.02f, 8.0f},   // jetski
            {4.2f, 1.0f, 0.05f, 0.5f, 0.04f, 4.0f},    // car
            {2.0f, 0.45f, 0.05f, 0.35f, 0.03f, 4.0f},  // bike
            {0.6f, 0.6f, 0.06f, 0.16f, 0.04f, 2.0f},   // swimmer
            {6.0f, 1.5f, 0.05f, 0.5f, 0.04f, 6.0f},    // other (a heli in the sea)
        };

        struct Source
        {
            float seg[4], shape[4];
            float dist;
        };

        // the step's hull capsule of an entity at (x, y)
        Source hullAt(const Entity &e, float x, float y, float fieldX, float fieldY)
        {
            const Hull &h = kHulls[e.kind];
            const float speed = std::hypot(e.vel[0], e.vel[1]);
            float fx = e.fwd[0], fy = e.fwd[1];
            const float fl = std::hypot(fx, fy);
            if (e.kind == kSwimmer || fl < 0.1f)
                fx = fy = 0.0f;
            else
                fx /= fl, fy /= fl;
            const float bow = h.length * 0.5f - h.radius, stern = h.length * 0.5f - h.radius;
            Source s = {};
            s.seg[0] = x - fieldX + fx * std::max(bow, 0.0f);
            s.seg[1] = y - fieldY + fy * std::max(bow, 0.0f);
            s.seg[2] = x - fieldX - fx * std::max(stern, 0.0f);
            s.seg[3] = y - fieldY - fy * std::max(stern, 0.0f);
            s.shape[0] = h.radius;
            s.shape[1] = std::min(h.idleDepth + h.depthPerSpeed * speed, h.depthMax);
            s.shape[2] = std::clamp((speed - 0.5f) / h.foamSpeed, 0.0f, 1.0f) * (e.kind == kSwimmer ? 0.35f : 0.7f);
            s.shape[3] = 0.0f;
            return s;
        }

        void viewConstants(const VcsRenderFrame &f, const float R[9], const WakeField &w, ViewConstants &v)
        {
            v = {};
            v.proj[0] = f.fx, v.proj[1] = f.fy, v.proj[2] = f.cx, v.proj[3] = f.cy;
            v.zmap[0] = f.depth_a, v.zmap[1] = f.depth_b, v.zmap[2] = f.depth_scale;
            for (int k = 0; k < 3; ++k)
                v.r0[k] = R[k], v.r1[k] = R[3 + k], v.r2[k] = R[6 + k];
            v.field[0] = w.cam_x, v.field[1] = w.cam_y, v.field[2] = w.size, v.field[3] = w.water_z;
            const float side = std::floor(std::min(f.width, f.height) * 0.45f);
            v.rect[0] = 16.0f, v.rect[1] = 16.0f, v.rect[2] = side, v.rect[3] = side;
            v.look[0] = 1.0f;
        }

        VcsRenderFrame g_last = {};
        float g_lastR[9] = {};
        bool g_viewFromGame = false;

        const WakeField *simulate(const VcsRenderFrame &f)
        {
            const VcsModApi &a = *mod::api;
            if (f.frame == g_sim.lastFrame)
                return g_sim.result.valid ? &g_sim.result : nullptr;
            g_sim.lastFrame = f.frame;
            g_sim.result = {};
            if (!simWanted() || f.backend == VCS_RENDER_BACKEND_NULL)
                return nullptr;
            const int quality = std::max(g_quality.load(std::memory_order_relaxed), 1);
            const int n = quality >= 2 ? 512 : 256;
            const float dx = quality >= 2 ? 0.375f : 0.5f;
            if (!ready(f, n))
                return nullptr;

            Snapshot s;
            std::vector<Splash> splashes;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                s = g_snap;
                splashes.swap(g_splashes);
            }
            if (!s.serial || !s.outdoors)
                return nullptr;

            // the field's centre follows the camera in whole texels; a jump of half the field starts it over
            const int64_t ncx = static_cast<int64_t>(std::floor(s.cam[0] / dx)), ncy = static_cast<int64_t>(std::floor(s.cam[1] / dx));
            bool reset = !g_sim.started || dx != g_sim.dx || std::llabs(ncx - g_sim.cx) > n / 2 || std::llabs(ncy - g_sim.cy) > n / 2;
            const int sx = reset ? 0 : static_cast<int>(ncx - g_sim.cx), sy = reset ? 0 : static_cast<int>(ncy - g_sim.cy);
            g_sim.cx = ncx, g_sim.cy = ncy, g_sim.dx = dx;
            const float fieldX = static_cast<float>(ncx) * dx, fieldY = static_cast<float>(ncy) * dx;

            // the game's time since the last frame, in fixed steps
            int steps = 0;
            if (s.serial != g_sim.lastSerial)
            {
                const uint32_t dms = reset ? 0u : s.ms - g_sim.lastMs;
                g_sim.acc += std::min(static_cast<float>(dms) / 1000.0f, 0.25f);
                g_sim.lastMs = s.ms;
                g_sim.lastSerial = s.serial;
                steps = static_cast<int>(g_sim.acc / kStep);
                g_sim.acc -= steps * kStep;
                if (steps > 4)
                    steps = 4, g_sim.acc = 0.0f;
            }
            if ((sx || sy) && !steps)
                steps = 1; // the scroll needs a pass
            if (reset)
            {
                g_sim.started = true;
                g_sim.tracks.clear();
                ++g_sim.resets;
            }

            const float c = 7.0f; // wave speed, m/s
            const float c2 = (c * kStep / dx) * (c * kStep / dx);
            StepConstants base = {};
            base.grid[0] = static_cast<float>(n), base.grid[1] = dx, base.grid[2] = c2;
            base.grid[3] = std::exp(-kStep * 0.9f); // velocity damping (e-folding ~1.1 s)
            base.steps[0] = kStep;
            base.steps[1] = std::exp(-kStep / 3.0f); // foam life ~3 s
            base.steps[2] = 0.25f;
            base.more[0] = 1.0f;
            base.more[1] = 0.08f;
            base.more[2] = std::exp(-kStep * 0.35f); // the height relaxes to the level (e-folding ~3 s)

            a.render_push_marker("renderhook wakes");
            int cur = g_gpu.cur;
            if (reset)
            {
                StepConstants z = base;
                z.steps[3] = 1.0f;
                a.render_update_constants(g_gpu.stepC[4], &z, sizeof(z));
                fullPass(g_gpu.state[0], g_gpu.step, g_gpu.stepC[4], g_gpu.state[1]);
                fullPass(g_gpu.state[1], g_gpu.step, g_gpu.stepC[4], g_gpu.state[0]);
                cur = 0;
            }

            // where each entity was when last used, to sweep its capsule from there
            const float half = n * dx * 0.5f;
            int used = 0;
            for (int k = 0; k < steps; ++k)
            {
                StepConstants sc = base;
                if (k == 0)
                    sc.shift[0] = sx, sc.shift[1] = -sy;
                std::vector<Source> list;
                const float t = static_cast<float>(k + 1) / static_cast<float>(steps);
                for (const Entity &e : s.entities)
                {
                    // placed by its offset from its own snapshot's camera (the lag between the game and the frame drawn)
                    const float ex = e.pos[0], ey = e.pos[1];
                    if (std::fabs(ex - fieldX) > half + 8.0f || std::fabs(ey - fieldY) > half + 8.0f)
                        continue;
                    auto it = g_sim.tracks.find(e.id);
                    float px = ex, py = ey;
                    if (it != g_sim.tracks.end() && std::hypot(ex - it->second.x, ey - it->second.y) < 12.0f)
                        px = it->second.x, py = it->second.y;
                    Source src = hullAt(e, px + (ex - px) * t, py + (ey - py) * t, fieldX, fieldY);
                    // the step's sweep: from the last step's place to this one
                    const float tp = static_cast<float>(k) / static_cast<float>(steps);
                    const float qx = px + (ex - px) * tp - fieldX, qy = py + (ey - py) * tp - fieldY;
                    if (std::hypot(src.seg[2] - qx, src.seg[3] - qy) > 0.05f && kHulls[e.kind].length < 1.0f)
                        src.seg[0] = qx, src.seg[1] = qy;
                    src.dist = std::hypot(ex - s.cam[0], ey - s.cam[1]);
                    list.push_back(src);
                }
                if (k == 0)
                    for (const Splash &sp : splashes)
                    {
                        if (std::fabs(sp.pos[0] - fieldX) > half || std::fabs(sp.pos[1] - fieldY) > half)
                            continue;
                        Source src = {};
                        src.seg[0] = src.seg[2] = sp.pos[0] - fieldX;
                        src.seg[1] = src.seg[3] = sp.pos[1] - fieldY;
                        src.shape[0] = sp.radius, src.shape[1] = sp.depth, src.shape[2] = sp.foam, src.shape[3] = 1.0f;
                        src.dist = std::hypot(sp.pos[0] - s.cam[0], sp.pos[1] - s.cam[1]) - 1000.0f; // splashes first
                        list.push_back(src);
                    }
                std::sort(list.begin(), list.end(), [](const Source &l, const Source &r) { return l.dist < r.dist; });
                const int count = std::min(static_cast<int>(list.size()), kMaxSources);
                for (int i = 0; i < count; ++i)
                {
                    std::memcpy(sc.seg[i], list[i].seg, sizeof(sc.seg[i]));
                    std::memcpy(sc.shape[i], list[i].shape, sizeof(sc.shape[i]));
                }
                sc.shift[2] = count;
                used = std::max(used, count);
                a.render_update_constants(g_gpu.stepC[k], &sc, sizeof(sc));
                fullPass(g_gpu.state[cur ^ 1], g_gpu.step, g_gpu.stepC[k], g_gpu.state[cur]);
                cur ^= 1;
                ++g_sim.steps;
            }
            g_gpu.cur = cur;
            if (steps)
            {
                for (auto it = g_sim.tracks.begin(); it != g_sim.tracks.end();)
                    it = (g_sim.frames - it->second.seen > 120u) ? g_sim.tracks.erase(it) : std::next(it);
                for (const Entity &e : s.entities)
                    g_sim.tracks[e.id] = {e.pos[0], e.pos[1], g_sim.frames};
            }
            g_sim.lastSources = used;

            // the field
            a.render_update_constants(g_gpu.stepC[4], &base, sizeof(base));
            fullPass(g_gpu.field, g_gpu.out, g_gpu.stepC[4], g_gpu.state[cur]);
            a.render_pop_marker();
            ++g_sim.frames;

            WakeField &w = g_sim.result;
            w.texture = g_gpu.field;
            w.size = n * dx;
            w.cam_x = s.cam[0] - fieldX;
            w.cam_y = s.cam[1] - fieldY;
            w.water_z = s.hasLevel ? s.level - s.cam[2] : -1000.0f;
            w.valid = 1;

            // the rotation for the debug views: the frame's own, else the game camera's (view x right, y down, w ahead)
            if (f.size >= offsetof(VcsRenderFrame, world_to_view) + sizeof(f.world_to_view) && f.view_valid)
                std::memcpy(g_lastR, f.world_to_view, sizeof(g_lastR)), g_viewFromGame = false;
            else
            {
                for (int k = 0; k < 3; ++k)
                    g_lastR[k] = s.right[k], g_lastR[3 + k] = -s.up[k], g_lastR[6 + k] = s.fwd[k];
                g_viewFromGame = true;
            }
            return &w;
        }

        // the debug views, in a hook of our own
        bool g_hooked = false;
        void hook(void *, const VcsRenderFrame *f)
        {
            if (!f || f->size < offsetof(VcsRenderFrame, fog_scale) + sizeof(f->fog_scale))
                return;
            g_last = *f;
            const WakeField *w = simulate(*f);
            const int view = g_view.load(std::memory_order_relaxed);
            if (!w || !view || !f->camera_valid)
                return;
            const VcsModApi &a = *mod::api;
            ViewConstants v;
            viewConstants(*f, g_lastR, *w, v);
            a.render_update_constants(g_gpu.viewC, &v, sizeof(v));
            a.render_push_marker("renderhook wake view");
            if ((view & 2) && f->depth)
            {
                a.render_begin_pass(f->color);
                a.render_set_viewport(0, 0, static_cast<int>(f->width), static_cast<int>(f->height));
                a.render_set_pipeline(g_gpu.water[f->depth_samples > 1u ? 1 : 0]);
                a.render_set_constants(1, g_gpu.viewC);
                a.render_set_texture(1, f->depth, VCS_RENDER_SAMPLER_POINT);
                a.render_set_texture(2, w->texture, VCS_RENDER_SAMPLER_LINEAR);
                a.render_draw(3);
                a.render_end_pass();
            }
            if (view & 1)
            {
                a.render_begin_pass(f->color);
                a.render_set_viewport(static_cast<int>(v.rect[0]), static_cast<int>(v.rect[1]), static_cast<int>(v.rect[2]),
                                      static_cast<int>(v.rect[3]));
                a.render_set_pipeline(g_gpu.show);
                a.render_set_constants(1, g_gpu.viewC);
                a.render_set_texture(2, w->texture, VCS_RENDER_SAMPLER_LINEAR);
                a.render_draw(3);
                a.render_end_pass();
            }
            a.render_pop_marker();
        }

        void hookOnce()
        {
            if (g_hooked)
                return;
            g_hooked = mod::api->add_render_hook(VCS_RENDER_BEFORE_POST, &hook, nullptr, VCS_RENDER_NEEDS_DEPTH) != 0;
            if (!g_hooked)
                mod::log("[renderhook] wakes: the renderer refused the render hook\n");
        }

        void setView(int v)
        {
            g_view = std::clamp(v, 0, 3);
            if (g_view)
                hookOnce();
        }

        void hookGame(uint32_t address, VcsGuestFn fn, VcsGuestFn &orig, const char *name)
        {
            orig = mod::api->lookup_function(address);
            if (!orig || !mod::api->replace_function(address, fn))
            {
                orig = nullptr;
                mod::log("[renderhook] wakes: %s (0x%08x) not found\n", name, address);
            }
        }
    }

    bool wakesOn() { return g_quality.load(std::memory_order_relaxed) > 0; }

    const WakeField *wakeField(const VcsRenderFrame &f)
    {
        if (!wakesOn())
            return nullptr;
        return simulate(f);
    }

    void installWakes()
    {
        if (const char *e = std::getenv("VCS_RH_WAKE_HOOKS"); e && e[0] == '0')
            return;
        hookGame(kGameProcess, &gameProcess, g_origProcess, "CGame::Process");
        hookGame(kAddExplosion, &addExplosion, g_origExplosion, "CExplosion::AddExplosion");
        hookGame(kShotWater, &shotWater, g_origShot, "the shot's water test");
        if (g_origProcess)
            mod::log("[renderhook] wakes: game hooks in\n");
    }

    void registerWakes()
    {
        // VCS_RH_WAKE_VIEW=1 the field in a corner, 2 a stand-in shading of the water with it, 3 both
        if (const char *e = std::getenv("VCS_RH_WAKE_VIEW"); e && e[0] >= '1' && e[0] <= '3')
            setView(e[0] - '0');
        // rhwake [view N | wakes 0..2 | splash X Y R D] -> the status line
        mod::addCommand("rhwake", [](const std::string &args, mod::Reply reply) {
            if (args.rfind("view", 0) == 0)
                setView(std::atoi(args.c_str() + 4));
            else if (args.rfind("wakes", 0) == 0 && !mod::set("renderhook.wakes", std::atoi(args.c_str() + 5)))
                return reply("error refused (an environment override?)");
            else if (args.rfind("splash", 0) == 0)
            {
                float x = 0, y = 0, r = 2.0f, d = 0.5f;
                if (std::sscanf(args.c_str() + 6, "%f %f %f %f", &x, &y, &r, &d) < 2)
                    return reply("error usage: rhwake splash X Y [radius depth]");
                addSplash({{x, y, 6.0f}, r, d, 1.0f});
            }
            Snapshot s;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                s = g_snap;
            }
            int boats = 0, swimmers = 0, others = 0;
            for (const Entity &e : s.entities)
                (e.kind == kBoat || e.kind == kJetski ? boats : e.kind == kSwimmer ? swimmers : others)++;
            char line[640];
            std::snprintf(line, sizeof(line),
                          "ok wakes %d view %d hooked %d frames %llu steps %llu resets %llu sources %d snapshot %llu ms %u "
                          "outdoors %d cam %.2f %.2f %.2f level %.2f(%d) in-water boats %d swimmers %d other %d entered %llu "
                          "bullets %llu explosions %llu field %.1f m cam %.2f %.2f rot %s%s%s",
                          g_quality.load(), g_view.load(), g_hooked ? 1 : 0, static_cast<unsigned long long>(g_sim.frames),
                          static_cast<unsigned long long>(g_sim.steps), static_cast<unsigned long long>(g_sim.resets),
                          g_sim.lastSources, static_cast<unsigned long long>(s.serial), s.ms, s.outdoors ? 1 : 0, s.cam[0],
                          s.cam[1], s.cam[2], s.level, s.hasLevel ? 1 : 0, boats, swimmers, others,
                          static_cast<unsigned long long>(g_entered), static_cast<unsigned long long>(g_bullets),
                          static_cast<unsigned long long>(g_explosions), g_sim.result.size, g_sim.result.cam_x,
                          g_sim.result.cam_y, g_viewFromGame ? "game" : "frame", g_sim.problem.empty() ? "" : " problem: ",
                          g_sim.problem.c_str());
            reply(line);
        });
    }

    void registerWakeSettings()
    {
        mod::Spec s;
        s.key = "renderhook.wakes";
        s.page = "Graphics";
        s.section = "Water (RenderHook)";
        s.label = "Wakes and ripples";
        s.help = "Boats, swimmers, cars falling in, bullets and explosions make waves and foam on the water that spread, "
                 "fade and follow you (a wave simulation around the camera). Off is the original.";
        s.type = mod::Type::Choice;
        s.choices = {{"Off (original)", 0}, {"On", 1}, {"High detail", 2}};
        s.defaultValue = 0;
        s.env = "VCS_RH_WAKES";
        s.onChange = [](int v) { g_quality = std::clamp(v, 0, 2); };
        mod::add(s);
    }
}
