// See jiggle.h and the port's docs/game-internals.md 12r.
#include "jiggle.h"
#include "jolt.h" // g_passive

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs::jiggle
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kTreeUpdate = 0x0024E8D0u;            // an anim tree's bone matrices (tree a0)
        constexpr uint32_t kTreeUpdateInPedRender = 0x003C3280u; // its return address in the ped's render (s0 = the ped)
        constexpr uint32_t kAfterBones = 0x0020C310u;            // CPed::IsPlayer, called by the ped's render next
        constexpr uint32_t kAfterBonesInPedRender = 0x00200740u; // its return address there (a0 = the ped)
        constexpr uint32_t kPedPool = 0x00487A94u, kPedSize = 0xD20u; // CPool: +0 entries, +4 flag bytes, +8 size
        constexpr uint32_t kPlayerPed = 0x004E4910u;             // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedPosition = 0x30u;
        constexpr uint32_t kPedType = 0x550u;                    // 0 the player, 4 civilian male, 5 civilian female, 6 cop
        constexpr uint32_t kPedTypeFemale = 5u;
        constexpr uint32_t kTimeMs = 0x004CD104u;                // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kFrameCounter = 0x004CD124u;          // CTimer::m_FrameCounter
        constexpr uint32_t kTreeNodes = 0x04u, kTreeMatrices = 0x28u; // RslTAnimTree (game-internals.md 12h)
        constexpr int kBones = 25;
        constexpr int kA0 = 4, kS0 = 16, kRa = 31;

        // SA's JoltPhysics.ini [JigglePhysics]: MaxJigglePeds, JiggleDistance, GravityScale, InwardLimit and the two
        // springs; Intensity 1.8 is the setting's 100 %.
        constexpr size_t kMaxPeds = 8;
        constexpr float kRange = 30.0f;
        constexpr float kGravityScale = 0.35f;
        constexpr float kInwardLimit = 0.35f; // of the max offset, into the body (backward along the bone's forward)
        constexpr float kIntensity = 1.8f;
        constexpr float kMaxAccel = 80.0f;    // m/s^2: more is a teleport or a respawn, not motion
        constexpr float kLever = 0.15f;       // m: an offset's share turned into a tilt (angle = offset / lever)
        constexpr uint32_t kForgetMs = 2000;  // a ped not drawn this long loses its state

        struct SpringDef
        {
            int bone;       // the bone whose skinned vertices bounce
            float hz, damping, maxOffset, maxAngleDeg, gain;
        };
        // spine1 (the chest): SA's Breast*; the pelvis (the hips): its Glute*
        constexpr SpringDef kChest{3, 5.2f, 0.28f, 0.06f, 22.0f, 1.0f};
        constexpr SpringDef kHips{1, 3.8f, 0.30f, 0.055f, 16.0f, 1.0f};

        struct Spring
        {
            float x[3] = {}, v[3] = {}; // the offset of the soft part from its bone (m, world) and its speed
            float last[3] = {}, lastVel[3] = {};
            int primed = 0;             // positions seen (2: an acceleration can be taken)
        };

        struct Body
        {
            int generation = -1;
            uint32_t tree = 0;
            uint32_t lastMs = 0;
            Spring chest, hips;
        };

        std::atomic<bool> g_enabled{false};
        std::atomic<int> g_intensity{100}; // percent
        bool g_everyone = false;           // VCS_JIGGLE_ALL=1: every ped, the player too
        VcsGuestFn g_origTree = nullptr, g_origAfterBones = nullptr;

        // game thread
        std::unordered_map<uint32_t, Body> g_bodies;         // ped -> its springs
        std::unordered_map<uint32_t, uint32_t> g_trees;      // ped -> its anim tree (its last draw)
        std::vector<std::pair<float, uint32_t>> g_seen;      // (distance, ped) of the eligible peds drawn this frame
        std::vector<uint32_t> g_chosen;                      // the nearest kMaxPeds of last frame's
        uint32_t g_frame = 0;

        struct Status
        {
            int peds = 0;
            uint32_t ped = 0;
            float chest = 0.0f, hips = 0.0f;       // the last jiggled ped's offsets now (m)
            float chestMax = 0.0f, hipsMax = 0.0f; // the largest since the last status read
            unsigned steps = 0;
        };
        std::mutex g_statusMutex;
        Status g_status;
        std::unordered_map<uint32_t, Status> g_pedStatus; // per ped, for "jiggle <ped>" (its max since the last read)

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        float length(const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        int generation(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t pool = rd32(ram, kPedPool);
            if (!pool)
                return -1;
            const uint32_t base = rd32(ram, pool), flags = rd32(ram, pool + 4), size = rd32(ram, pool + 8);
            if (ped < base || (ped - base) % kPedSize || (ped - base) / kPedSize >= size)
                return -1;
            const uint8_t f = ram[(flags + (ped - base) / kPedSize) & kRamMask];
            return f & 0x80 ? -1 : f & 0x7F;
        }

        // A new game frame: last frame's nearest eligible peds are this one's (the draws of a frame come in no order),
        // and peds not drawn for a while are forgotten.
        void newFrame(const uint8_t *ram, uint32_t frame)
        {
            g_frame = frame;
            std::sort(g_seen.begin(), g_seen.end());
            g_chosen.clear();
            for (const auto &[distance, ped] : g_seen)
                if (g_chosen.size() < kMaxPeds && std::find(g_chosen.begin(), g_chosen.end(), ped) == g_chosen.end())
                    g_chosen.push_back(ped);
            g_seen.clear();
            g_trees.clear(); // each ped notes its tree again just before its own draw
            const uint32_t ms = rd32(ram, kTimeMs);
            for (auto it = g_bodies.begin(); it != g_bodies.end();)
                if (ms - it->second.lastMs > kForgetMs || generation(ram, it->first) != it->second.generation)
                    it = g_bodies.erase(it);
                else
                    ++it;
        }

        // One spring over dt: the soft part lags its bone's acceleration and springs back (semi-implicit Euler in steps
        // of at most 1/240 s), kept within its max offset - less of it into the body.
        void stepSpring(Spring &s, const SpringDef &def, const float *bonePos, const float *boneForward, float dt,
                        float gain)
        {
            float accel[3] = {};
            if (s.primed >= 1 && dt > 0.0f)
            {
                float vel[3];
                for (int k = 0; k < 3; ++k)
                    vel[k] = (bonePos[k] - s.last[k]) / dt;
                if (s.primed >= 2)
                    for (int k = 0; k < 3; ++k)
                        accel[k] = (vel[k] - s.lastVel[k]) / dt;
                std::copy(vel, vel + 3, s.lastVel);
                s.primed = std::min(s.primed + 1, 2);
            }
            else if (s.primed == 0)
                s.primed = 1;
            std::copy(bonePos, bonePos + 3, s.last);
            const float a = length(accel);
            if (a > kMaxAccel) // a teleport: start again
            {
                std::fill(std::begin(s.x), std::end(s.x), 0.0f);
                std::fill(std::begin(s.v), std::end(s.v), 0.0f);
                s.primed = 1;
                return;
            }
            const float w = 2.0f * 3.14159265f * def.hz, w2 = w * w, c = 2.0f * def.damping * w;
            const float drive = def.gain * gain;
            const int steps = std::clamp(int(std::ceil(dt * 240.0f)), 1, 12);
            const float h = dt / float(steps);
            for (int i = 0; i < steps; ++i)
                for (int k = 0; k < 3; ++k)
                {
                    const float force = -w2 * s.x[k] - c * s.v[k] - accel[k] * drive - (k == 2 ? 9.81f * kGravityScale : 0.0f);
                    s.v[k] += force * h;
                    s.x[k] += s.v[k] * h;
                }
            // the limits: the max offset; into the body (against the bone's forward) only kInwardLimit of it
            const float in = -(s.x[0] * boneForward[0] + s.x[1] * boneForward[1] + s.x[2] * boneForward[2]);
            if (in > def.maxOffset * kInwardLimit)
            {
                const float back = in - def.maxOffset * kInwardLimit;
                for (int k = 0; k < 3; ++k)
                    s.x[k] += boneForward[k] * back;
            }
            const float l = length(s.x);
            if (l > def.maxOffset)
                for (int k = 0; k < 3; ++k)
                {
                    s.x[k] *= def.maxOffset / l;
                    s.v[k] *= 0.5f;
                }
        }

        // The bone's drawn matrix moved by its spring: offset, and tilted about its position toward the offset (up to
        // the max angle) - the vertices weighted to it lag and swing; its children's matrices are their own.
        void apply(uint8_t *ram, uint32_t matrix, const Spring &s, const SpringDef &def, float scale)
        {
            float rows[3][3], pos[3];
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t k = 0; k < 3; ++k)
                    rows[r][k] = rdf(ram, matrix + r * 0x10u + k * 4);
            for (uint32_t k = 0; k < 3; ++k)
                pos[k] = rdf(ram, matrix + 0x30u + k * 4);
            const float x[3] = {s.x[0] * scale, s.x[1] * scale, s.x[2] * scale};
            const float l = length(x);
            if (l < 1e-5f)
                return;
            // the tilt: about the axis across the bone's length (its X) and the offset
            const float *along = rows[0];
            float axis[3] = {along[1] * x[2] - along[2] * x[1], along[2] * x[0] - along[0] * x[2], along[0] * x[1] - along[1] * x[0]};
            const float al = length(axis);
            const float angle = std::min(l / kLever, def.maxAngleDeg * 3.14159265f / 180.0f);
            if (al > 1e-6f)
            {
                for (float &v : axis)
                    v /= al;
                const float cs = std::cos(angle), sn = std::sin(angle);
                for (auto &row : rows)
                {
                    const float d = axis[0] * row[0] + axis[1] * row[1] + axis[2] * row[2];
                    const float cr[3] = {axis[1] * row[2] - axis[2] * row[1], axis[2] * row[0] - axis[0] * row[2],
                                         axis[0] * row[1] - axis[1] * row[0]};
                    for (int k = 0; k < 3; ++k)
                        row[k] = row[k] * cs + cr[k] * sn + axis[k] * d * (1.0f - cs);
                }
            }
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t k = 0; k < 3; ++k)
                    wrf(ram, matrix + r * 0x10u + k * 4, rows[r][k]);
            for (uint32_t k = 0; k < 3; ++k)
                wrf(ram, matrix + 0x30u + k * 4, pos[k] + x[k]);
        }

        // A ped about to be drawn, its bones built (and any ragdoll pose written by Jolt's hook, which runs before this
        // one): noted for the choice of the nearest; one of last frame's nearest gets its springs stepped and applied.
        void jiggle(uint8_t *ram, uint32_t ped, uint32_t tree)
        {
            const uint32_t frame = rd32(ram, kFrameCounter);
            if (frame != g_frame)
                newFrame(ram, frame);
            const uint32_t player = rd32(ram, kPlayerPed);
            if (!g_everyone && (ped == player || rd32(ram, ped + kPedType) != kPedTypeFemale))
                return;
            const uint32_t matrices = rd32(ram, tree + kTreeMatrices);
            if (!matrices || rd32(ram, tree + kTreeNodes) != uint32_t(kBones) || !player)
                return;
            float d[3];
            for (uint32_t k = 0; k < 3; ++k)
                d[k] = rdf(ram, ped + kPedPosition + 4 * k) - rdf(ram, player + kPedPosition + 4 * k);
            const float distance = length(d);
            if (!(distance < kRange))
                return;
            g_seen.emplace_back(distance, ped);
            if (std::find(g_chosen.begin(), g_chosen.end(), ped) == g_chosen.end())
                return;
            const int gen = generation(ram, ped);
            Body &body = g_bodies[ped];
            const uint32_t ms = rd32(ram, kTimeMs);
            if (body.generation != gen || body.tree != tree)
            {
                body = Body{};
                body.generation = gen;
                body.tree = tree;
                body.lastMs = ms;
            }
            const float dt = std::clamp(float(ms - body.lastMs) / 1000.0f, 0.0f, 0.05f);
            body.lastMs = ms;
            const float gain = kIntensity * static_cast<float>(g_intensity.load(std::memory_order_relaxed)) / 100.0f;
            for (const auto &[spring, def] : {std::pair<Spring *, const SpringDef *>{&body.chest, &kChest},
                                              std::pair<Spring *, const SpringDef *>{&body.hips, &kHips}})
            {
                const uint32_t m = matrices + uint32_t(def->bone) * 0x40u;
                float pos[3], forward[3];
                for (uint32_t k = 0; k < 3; ++k)
                {
                    pos[k] = rdf(ram, m + 0x30u + k * 4);
                    forward[k] = rdf(ram, m + 0x10u + k * 4); // the torso bones' Y: forward (game-internals.md 12h)
                }
                if (!std::isfinite(pos[0] + pos[1] + pos[2]) || !std::isfinite(forward[0] + forward[1] + forward[2]))
                    continue;
                if (dt > 0.0f || spring->primed == 0)
                    stepSpring(*spring, *def, pos, forward, dt, gain);
                apply(ram, m, *spring, *def, 1.0f);
            }
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.peds = int(g_chosen.size());
            g_status.ped = ped;
            g_status.chest = length(body.chest.x);
            g_status.hips = length(body.hips.x);
            g_status.chestMax = std::max(g_status.chestMax, g_status.chest);
            g_status.hipsMax = std::max(g_status.hipsMax, g_status.hips);
            ++g_status.steps;
            Status &one = g_pedStatus[ped];
            one.ped = ped;
            one.chest = g_status.chest;
            one.hips = g_status.hips;
            one.chestMax = std::max(one.chestMax, one.chest);
            one.hipsMax = std::max(one.hipsMax, one.hips);
            ++one.steps;
            if (g_pedStatus.size() > 64)
                g_pedStatus.clear();
        }

        VCS_GUEST_CC void treeUpdate(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origTree(rdram, ctx, runtime);
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kRa) == kTreeUpdateInPedRender)
                g_trees[mod::gpr32(ctx, kS0)] = mod::gpr32(ctx, kA0);
            return g_origTree(rdram, ctx, runtime);
        }

        VCS_GUEST_CC void afterBones(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origAfterBones(rdram, ctx, runtime);
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kRa) == kAfterBonesInPedRender)
            {
                const uint32_t ped = mod::gpr32(ctx, kA0);
                if (const auto tree = g_trees.find(ped); tree != g_trees.end())
                    jiggle(rdram, ped, tree->second);
            }
            return g_origAfterBones(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "physics.jiggle";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Jiggle physics";
        s.help = "Female peds' chests and hips bounce with their movement and settle, as with San Andreas' Jolt Physics "
                 "mod (VCS's peds have no extra bones for it: the chest and pelvis bones' own skin moves). The nearest 8 "
                 "within 30 m.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_JIGGLE";
        s.onChange = [](int v) { g_enabled = v != 0; };
        add(s);

        s = {};
        s.key = "physics.jiggle_intensity";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Jiggle intensity";
        s.help = "How much the jiggle follows the body's movement (100 %: the SA mod's 1.8).";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 300;
        s.format = "%d %%";
        s.defaultValue = 100;
        s.onChange = [](int v) { g_intensity = v; };
        add(s);
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_JIGGLE_ALL"); e && e[0] == '1')
            g_everyone = true;
        g_origTree = mod::api->lookup_function(kTreeUpdate);
        g_origAfterBones = mod::api->lookup_function(kAfterBones);
        if (!g_origTree || !g_origAfterBones)
            return;
        mod::api->replace_function(kTreeUpdate, &treeUpdate);
        mod::api->replace_function(kAfterBones, &afterBones);
        // jiggle [ped] -> ok enabled= peds= ped= chest= hips= chestmax= hipsmax= steps= (the last jiggled ped's, or that
        // ped's; the max since the last read)
        mod::addCommand("jiggle", [](const std::string &args, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                const uint32_t ped = args.empty() ? 0u : uint32_t(std::strtoul(args.c_str(), nullptr, 16));
                if (ped)
                {
                    Status &one = g_pedStatus[ped];
                    st = one;
                    st.peds = g_status.peds;
                    one.chestMax = one.hipsMax = 0.0f;
                }
                else
                {
                    st = g_status;
                    g_status.chestMax = g_status.hipsMax = 0.0f;
                }
            }
            char text[256];
            std::snprintf(text, sizeof(text), "ok enabled=%d peds=%d ped=%08x chest=%.4f hips=%.4f chestmax=%.4f hipsmax=%.4f steps=%u",
                          g_enabled ? 1 : 0, st.peds, st.ped, st.chest, st.hips, st.chestMax, st.hipsMax, st.steps);
            reply(text);
        });
    }
}
