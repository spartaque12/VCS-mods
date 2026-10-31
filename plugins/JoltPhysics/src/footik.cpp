// See footik.h and the port's docs/game-internals.md 12q.
#include "footik.h"
#include "jolt.h" // g_passive

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs::footik
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kTreeUpdate = 0x0024E8D0u;            // an anim tree's bone matrices (tree a0)
        constexpr uint32_t kTreeUpdateInPedRender = 0x003C3280u; // its return address in FUN_003c3250 (s0 = the ped)
        constexpr uint32_t kAfterBones = 0x0020C310u;            // CPed::IsPlayer, called by the ped's render next
        constexpr uint32_t kAfterBonesInPedRender = 0x00200740u; // its return address there (a0 = the ped)
        constexpr int kA0 = 4, kS0 = 16, kRa = 31;
        // RslTAnimTree: +0x04 node count, +0x28 the bones' world matrices (0x40 each: rows X, Y, Z, position)
        constexpr uint32_t kTreeNodes = 0x04u, kTreeMatrices = 0x28u;
        constexpr int kBones = 25;
        constexpr uint32_t kPedPosition = 0x30u, kPedState = 0x8C4u, kPedFlags = 0x1C8u; // +0x1C8 bit 0: standing
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        constexpr uint32_t kTimeMs = 0x004CD104u;    // CTimer::m_snTimeInMilliseconds
        // states the ragdolls or a vehicle have the ped in: getting into a jump 0x2C, fall 0x2D, get-up 0x2E, driving 0x37,
        // dying / dead 0x39 / 0x3A
        constexpr uint32_t kSkipStates[] = {0x2Cu, 0x2Du, 0x2Eu, 0x37u, 0x38u, 0x39u, 0x3Au};

        // The world's collision (game-internals.md 12k): CWorld's sectors, their building and object lists, CColModel.
        constexpr uint32_t kSectors = 0x00486464u, kSectorBytes = 0x38u;
        constexpr int kSectorCount = 50;
        constexpr float kSectorSize = 80.0f, kSectorX0 = 30.0f, kSectorY0 = 25.0f;
        constexpr uint32_t kLists[] = {0x00u, 0x04u, 0x14u, 0x18u, 0x0Cu, 0x10u}; // buildings, objects
        constexpr uint32_t kListLimit = 4096;
        constexpr uint32_t kEntityFlags = 0x48u, kUsesCollision = 1u << 9, kEntityModel = 0x56u;
        constexpr uint32_t kModelInfos = 0x00486518u, kModelColModel = 0x14u;
        constexpr uint32_t kColBoxCount = 0x30u, kColTriangleCount = 0x32u, kColSphereCount = 0x34u,
                           kColSphereArray = 0x38u, kColBoxArray = 0x40u, kColVertexArray = 0x48u,
                           kColTriangleArray = 0x4Cu;

        // after SA's mod's [FootIK]
        constexpr float kNpcRange = 25.0f, kMaxOffset = 0.45f, kSmoothing = 0.4f, kAlignStrength = 0.7f;
        constexpr float kSoles = 1.0f;           // a standing ped's position is 1 m over its soles (12h)
        constexpr uint32_t kForgetMs = 2000;     // a ped not drawn this long loses its state
        constexpr uint32_t kCacheMs = 10000;     // an entity's triangles kept this long unused

        constexpr int kLegs[2][4] = {{17, 18, 19, 20}, {21, 22, 23, 24}}; // thigh, calf, foot, toe

        std::atomic<bool> g_enabled{false}, g_npcs{true};
        VcsGuestFn g_origTree = nullptr, g_origAfterBones = nullptr;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint32_t rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        struct V3
        {
            float x = 0, y = 0, z = 0;
        };
        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float len(V3 a) { return std::sqrt(dot(a, a)); }
        V3 unit(V3 a)
        {
            const float l = len(a);
            return l > 1e-6f ? a * (1.0f / l) : V3{};
        }
        // v turned by `angle` about the unit axis k (Rodrigues)
        V3 turn(V3 v, V3 k, float angle)
        {
            const float c = std::cos(angle), s = std::sin(angle);
            return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0f - c));
        }

        // ---- the ground: the col triangles of the entities around, cached per entity in world space ----

        struct Tri
        {
            V3 a, b, c, n;      // corners; the face's normal, z up
            float lo[2], hi[2]; // its x / y extent
        };
        struct Entity
        {
            uint64_t key = 0;
            V3 at;
            float lo[2] = {}, hi[2] = {};
            std::vector<Tri> tris;
            uint32_t used = 0;
        };
        std::unordered_map<uint32_t, Entity> g_cache;

        uint64_t colKey(const uint8_t *ram, uint32_t col)
        {
            const uint32_t counts = rd32(ram, col + kColBoxCount), spheres = ram[(col + kColSphereCount) & kRamMask];
            if (!(counts & 0xFFFF0000u) && !(counts & 0xFFFFu))
                return 0; // no triangles, no boxes: nothing to stand on
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
            mix(counts);
            mix(spheres);
            for (uint32_t array : {kColSphereArray, kColBoxArray, kColVertexArray, kColTriangleArray})
                mix(rd32(ram, col + array));
            return h | 1;
        }

        void addTri(Entity &e, V3 a, V3 b, V3 c)
        {
            Tri t{a, b, c, {}, {}, {}};
            // the game's front face is clockwise: its normal (c - a) x (b - a); upward faces only (ground)
            V3 n = unit(cross(c - a, b - a));
            if (n.z < 0.0f)
                n = n * -1.0f;
            if (n.z < 0.35f)
                return; // a wall
            t.n = n;
            t.lo[0] = std::min({a.x, b.x, c.x});
            t.hi[0] = std::max({a.x, b.x, c.x});
            t.lo[1] = std::min({a.y, b.y, c.y});
            t.hi[1] = std::max({a.y, b.y, c.y});
            for (int k = 0; k < 2; ++k)
            {
                e.lo[k] = e.tris.empty() ? t.lo[k] : std::min(e.lo[k], t.lo[k]);
                e.hi[k] = e.tris.empty() ? t.hi[k] : std::max(e.hi[k], t.hi[k]);
            }
            e.tris.push_back(t);
        }

        // An entity's ground faces in the world: its col triangles (and its boxes' tops) through its matrix.
        void build(const uint8_t *ram, uint32_t entity, uint32_t col, Entity &e)
        {
            e.tris.clear();
            float m[16];
            std::memcpy(m, ram + (entity & kRamMask), 64);
            auto world = [&m](float x, float y, float z) {
                return V3{m[12] + m[0] * x + m[4] * y + m[8] * z, m[13] + m[1] * x + m[5] * y + m[9] * z,
                          m[14] + m[2] * x + m[6] * y + m[10] * z};
            };
            const uint32_t triangles = rd16(ram, col + kColTriangleCount), vertices = rd32(ram, col + kColVertexArray),
                           array = rd32(ram, col + kColTriangleArray);
            for (uint32_t i = 0; vertices && array && i < triangles; ++i)
            {
                V3 p[3];
                bool good = true;
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t off = rd16(ram, array + i * 8u + 2u * k);
                    good = good && off % 6 == 0;
                    p[k] = world(float(int16_t(rd16(ram, vertices + off))) / 128.0f,
                                 float(int16_t(rd16(ram, vertices + off + 2))) / 128.0f,
                                 float(int16_t(rd16(ram, vertices + off + 4))) / 128.0f);
                }
                if (good)
                    addTri(e, p[0], p[1], p[2]);
            }
            const uint32_t boxes = rd16(ram, col + kColBoxCount), boxArray = rd32(ram, col + kColBoxArray);
            for (uint32_t i = 0; boxArray && i < boxes; ++i)
            {
                const uint32_t b = boxArray + i * 0x30u;
                const float x0 = rdf(ram, b), y0 = rdf(ram, b + 4), x1 = rdf(ram, b + 0x10), y1 = rdf(ram, b + 0x14),
                            z1 = rdf(ram, b + 0x18);
                if (!(x1 > x0) || !(y1 > y0) || !std::isfinite(z1))
                    continue;
                const V3 c00 = world(x0, y0, z1), c10 = world(x1, y0, z1), c11 = world(x1, y1, z1), c01 = world(x0, y1, z1);
                addTri(e, c00, c11, c10); // either winding: addTri keeps the upward normal
                addTri(e, c00, c01, c11);
            }
            e.at = {m[12], m[13], m[14]};
        }

        int sectorIndex(float coordinate, float origin)
        {
            return std::clamp(int(std::floor(coordinate / kSectorSize + origin)), 0, kSectorCount - 1);
        }

        // The highest ground face over (x, y) between zLow and zHigh: its height and normal; false when none.
        bool ground(const uint8_t *ram, float x, float y, float zHigh, float zLow, uint32_t ms, float &z, V3 &n)
        {
            const uint32_t sectors = rd32(ram, kSectors), infos = rd32(ram, kModelInfos);
            if (!sectors || !infos)
                return false;
            const uint32_t sector = sectors + uint32_t(sectorIndex(y, kSectorY0) * kSectorCount + sectorIndex(x, kSectorX0)) * kSectorBytes;
            bool found = false;
            for (uint32_t list : kLists)
            {
                uint32_t node = rd32(ram, sector + list);
                for (uint32_t i = 0; node && i < kListLimit; ++i, node = rd32(ram, node + 8))
                {
                    const uint32_t entity = rd32(ram, node);
                    if (!entity || !(rd32(ram, entity + kEntityFlags) & kUsesCollision))
                        continue;
                    const int model = int(int16_t(rd16(ram, entity + kEntityModel)));
                    const uint32_t info = model >= 0 ? rd32(ram, infos + uint32_t(model) * 4) : 0;
                    const uint32_t col = info ? rd32(ram, info + kModelColModel) : 0;
                    if (!col)
                        continue;
                    const uint64_t key = colKey(ram, col);
                    if (!key)
                        continue;
                    Entity &e = g_cache[entity];
                    const V3 at{rdf(ram, entity + 0x30), rdf(ram, entity + 0x34), rdf(ram, entity + 0x38)};
                    if (e.key != key || len(at - e.at) > 0.01f)
                    {
                        build(ram, entity, col, e);
                        e.key = key;
                    }
                    e.used = ms;
                    if (e.tris.empty() || x < e.lo[0] || x > e.hi[0] || y < e.lo[1] || y > e.hi[1])
                        continue;
                    for (const Tri &t : e.tris)
                    {
                        if (x < t.lo[0] || x > t.hi[0] || y < t.lo[1] || y > t.hi[1])
                            continue;
                        const float d = (t.b.y - t.c.y) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.y - t.c.y);
                        if (std::fabs(d) < 1e-9f)
                            continue;
                        const float l1 = ((t.b.y - t.c.y) * (x - t.c.x) + (t.c.x - t.b.x) * (y - t.c.y)) / d;
                        const float l2 = ((t.c.y - t.a.y) * (x - t.c.x) + (t.a.x - t.c.x) * (y - t.c.y)) / d;
                        const float l3 = 1.0f - l1 - l2;
                        if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f)
                            continue;
                        const float h = l1 * t.a.z + l2 * t.b.z + l3 * t.c.z;
                        if (h > zHigh || h < zLow || (found && h <= z))
                            continue;
                        z = h;
                        n = t.n;
                        found = true;
                    }
                }
            }
            return found;
        }

        // ---- the legs ----

        struct Bone
        {
            V3 row[3], pos;
        };

        // Bones `first`..`last` of the leg turned by `angle` about the unit axis k through `pivot`.
        void turnBones(Bone *b, const int *leg, int first, int last, V3 pivot, V3 k, float angle)
        {
            if (std::fabs(angle) < 1e-6f)
                return;
            for (int i = first; i <= last; ++i)
            {
                Bone &bone = b[leg[i]];
                for (V3 &r : bone.row)
                    r = turn(r, k, angle);
                bone.pos = pivot + turn(bone.pos - pivot, k, angle);
            }
        }

        // The rotation taking direction u to v: its axis and angle.
        bool fromTo(V3 u, V3 v, V3 &axis, float &angle)
        {
            u = unit(u);
            v = unit(v);
            const V3 c = cross(u, v);
            const float s = len(c);
            if (s < 1e-6f)
                return false;
            axis = c * (1.0f / s);
            angle = std::atan2(s, dot(u, v));
            return true;
        }

        // Two-bone IK: the leg's foot to `target` (the knee in the plane it bends in), then the foot tilted toward `normal`
        // by `align`.
        void reach(Bone *b, const int *leg, V3 target, V3 normal, float align)
        {
            const V3 hip = b[leg[0]].pos, knee = b[leg[1]].pos, ankle = b[leg[2]].pos;
            const float upper = len(knee - hip), lower = len(ankle - knee);
            V3 to = target - hip;
            const float dist = len(to);
            if (upper < 0.05f || lower < 0.05f || dist < 1e-3f)
                return;
            const V3 dir = to * (1.0f / dist);
            const float d = std::clamp(dist, std::fabs(upper - lower) + 1e-3f, upper + lower - 1e-3f);
            V3 pole = (knee - hip) - dir * dot(knee - hip, dir);
            if (len(pole) < 1e-4f)
                return;
            pole = unit(pole);
            const float cosA = std::clamp((upper * upper + d * d - lower * lower) / (2.0f * upper * d), -1.0f, 1.0f);
            const V3 kneeTo = hip + dir * (upper * cosA) + pole * (upper * std::sqrt(1.0f - cosA * cosA));
            V3 axis;
            float angle;
            if (fromTo(knee - hip, kneeTo - hip, axis, angle))
                turnBones(b, leg, 0, 3, hip, axis, angle);
            const V3 k2 = b[leg[1]].pos;
            if (fromTo(b[leg[2]].pos - k2, target - k2, axis, angle))
                turnBones(b, leg, 1, 3, k2, axis, angle);
            if (align > 0.0f && fromTo(V3{0, 0, 1}, normal, axis, angle))
                turnBones(b, leg, 2, 3, b[leg[2]].pos, axis, angle * align);
        }

        struct Ped
        {
            uint32_t tree = 0, seen = 0, drawn = 0;
            float pelvis = 0.0f, leg[2] = {}; // eased offsets (m)
            V3 normal[2] = {V3{0, 0, 1}, V3{0, 0, 1}};
        };
        std::unordered_map<uint32_t, Ped> g_peds;

        // for the control channel's "footik" (another thread): the player's last adjustment
        struct Status
        {
            unsigned peds = 0;           // adjusted in the last frame
            float ms = 0.0f;             // their cost then
            bool vic = false;            // the player adjusted
            float ground[2] = {}, sole[2] = {}, animSole[2] = {}, pelvis = 0.0f;
            bool found[2] = {};
        };
        std::mutex g_statusMutex;
        Status g_status, g_building;
        std::atomic<unsigned> g_adjusted{0}; // peds' poses adjusted, in all
        uint32_t g_frameMs = 0;

        bool skipState(uint32_t state)
        {
            return std::find(std::begin(kSkipStates), std::end(kSkipStates), state) != std::end(kSkipStates);
        }

        // A ped's drawn bones: each foot onto the ground under it.
        void adjust(uint8_t *ram, uint32_t ped, Ped &p, uint32_t ms)
        {
            const uint32_t matrices = rd32(ram, p.tree + kTreeMatrices);
            if (!matrices || rd32(ram, p.tree + kTreeNodes) != uint32_t(kBones))
                return;
            const bool player = rd32(ram, kPlayerPed) == ped;
            const float dt = p.drawn && ms > p.drawn ? std::min(float(ms - p.drawn) / 1000.0f, 0.25f) : 1.0f / 30.0f;
            p.drawn = ms;
            const float ease = 1.0f - std::pow(1.0f - kSmoothing, dt * 30.0f); // SA's per-frame ease at 30 fps

            // standing on foot (else everything eases back to the animation)
            const uint32_t state = rd32(ram, ped + kPedState);
            const bool active = (rd32(ram, ped + kPedFlags) & 1u) && !skipState(state);

            Bone b[kBones];
            for (int i = 0; i < kBones; ++i)
            {
                float m[16];
                std::memcpy(m, ram + ((matrices + i * 0x40u) & kRamMask), 64);
                for (int r = 0; r < 3; ++r)
                    b[i].row[r] = {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]};
                b[i].pos = {m[12], m[13], m[14]};
            }
            const float floor = rdf(ram, ped + kPedPosition + 8) - kSoles; // where the animation has the ground
            float need[2] = {}, groundZ[2] = {}, animSole[2] = {}, lift[2] = {};
            V3 normal[2] = {V3{0, 0, 1}, V3{0, 0, 1}};
            bool found[2] = {};
            for (int l = 0; l < 2 && active; ++l)
            {
                const V3 ankle = b[kLegs[l][2]].pos;
                lift[l] = ankle.z - floor; // the ankle over the animation's ground (a step lifts it)
                float z = 0.0f;
                V3 n;
                if (ground(ram, ankle.x, ankle.y, floor + kMaxOffset + 0.05f, floor - kMaxOffset - 0.1f, ms, z, n))
                {
                    found[l] = true;
                    groundZ[l] = z;
                    normal[l] = n;
                    need[l] = std::clamp(z - floor, -kMaxOffset, kMaxOffset);
                    animSole[l] = floor;
                }
            }
            // the pelvis down by the lower foot's need; each leg the rest
            const float pelvisTarget = active ? std::min(0.0f, std::min(found[0] ? need[0] : 0.0f, found[1] ? need[1] : 0.0f)) : 0.0f;
            p.pelvis += (pelvisTarget - p.pelvis) * ease;
            for (int l = 0; l < 2; ++l)
            {
                p.leg[l] += ((found[l] ? need[l] : 0.0f) - p.leg[l]) * ease;
                p.normal[l] = unit(p.normal[l] + (normal[l] - p.normal[l]) * ease);
            }
            if (std::fabs(p.pelvis) < 1e-4f && std::fabs(p.leg[0]) < 1e-4f && std::fabs(p.leg[1]) < 1e-4f && !active)
                return;
            for (Bone &bone : b)
                bone.pos.z += p.pelvis;
            for (int l = 0; l < 2; ++l)
            {
                const V3 target = b[kLegs[l][2]].pos + V3{0, 0, p.leg[l] - p.pelvis};
                reach(b, kLegs[l], target, p.normal[l], kAlignStrength);
            }
            for (int i = 0; i < kBones; ++i)
            {
                const uint32_t m = matrices + i * 0x40u;
                const float rows[4][3] = {{b[i].row[0].x, b[i].row[0].y, b[i].row[0].z},
                                          {b[i].row[1].x, b[i].row[1].y, b[i].row[1].z},
                                          {b[i].row[2].x, b[i].row[2].y, b[i].row[2].z},
                                          {b[i].pos.x, b[i].pos.y, b[i].pos.z}};
                for (uint32_t r = 0; r < 4; ++r)
                    std::memcpy(ram + ((m + r * 0x10u) & kRamMask), rows[r], 12); // the w lanes are the game's
            }
            ++g_building.peds;
            ++g_adjusted;
            if (player)
            {
                g_building.vic = active;
                g_building.pelvis = p.pelvis;
                for (int l = 0; l < 2; ++l)
                {
                    g_building.found[l] = found[l];
                    g_building.ground[l] = groundZ[l];
                    g_building.animSole[l] = animSole[l];
                    // the sole as drawn: the ankle's lift over the animation's ground kept
                    g_building.sole[l] = b[kLegs[l][2]].pos.z - lift[l];
                }
            }
        }

        // An anim tree's bones built in a ped's render (FUN_003c3250, s0 = the ped): its tree.
        VCS_GUEST_CC void treeUpdate(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origTree(rdram, ctx, runtime);
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kRa) == kTreeUpdateInPedRender)
            {
                Ped &p = g_peds[mod::gpr32(ctx, kS0)];
                p.tree = mod::gpr32(ctx, kA0);
                p.seen = rd32(rdram, kTimeMs);
            }
            return g_origTree(rdram, ctx, runtime);
        }

        // The ped's render, its bones built, before the skinned draw: its feet on the ground.
        VCS_GUEST_CC void afterBones(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origAfterBones(rdram, ctx, runtime);
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kRa) == kAfterBonesInPedRender)
            {
                const auto start = std::chrono::steady_clock::now();
                const uint32_t ped = mod::gpr32(ctx, kA0), ms = rd32(rdram, kTimeMs);
                if (ms != g_frameMs)
                {
                    // a new frame: the last one's numbers published, stale peds and triangles forgotten
                    {
                        std::lock_guard<std::mutex> lock(g_statusMutex);
                        g_status = g_building;
                    }
                    g_building = Status{};
                    g_frameMs = ms;
                    for (auto it = g_peds.begin(); it != g_peds.end();)
                        it = ms - it->second.seen > kForgetMs ? g_peds.erase(it) : std::next(it);
                    for (auto it = g_cache.begin(); it != g_cache.end();)
                        it = ms - it->second.used > kCacheMs ? g_cache.erase(it) : std::next(it);
                }
                const auto it = g_peds.find(ped);
                const uint32_t player = rd32(rdram, kPlayerPed);
                bool inRange = ped == player;
                if (!inRange && player && g_npcs.load(std::memory_order_relaxed))
                {
                    const float dx = rdf(rdram, ped + kPedPosition) - rdf(rdram, player + kPedPosition),
                                dy = rdf(rdram, ped + kPedPosition + 4) - rdf(rdram, player + kPedPosition + 4);
                    inRange = dx * dx + dy * dy < kNpcRange * kNpcRange;
                }
                if (it != g_peds.end() && it->second.tree && it->second.seen == ms && inRange)
                    adjust(rdram, ped, it->second, ms);
                g_building.ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
            return g_origAfterBones(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "physics.footik";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Foot IK";
        s.help = "Feet stand on the ground on slopes and stairs, as with San Andreas' Jolt Physics mod: each foot reaches "
                 "the ground under it and tilts with it, the hips lowered to the lower foot.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_FOOTIK";
        s.onChange = [](int v) { g_enabled = v != 0; };
        add(s);

        s = {};
        s.key = "physics.footik_npcs";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Foot IK for peds";
        s.help = "With Foot IK on: peds within 25 m too, not only Vic.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = [](int v) { g_npcs = v != 0; };
        add(s);
    }

    void install()
    {
        g_origTree = mod::api->lookup_function(kTreeUpdate);       // the ragdolls' (and first person's) hooks, when installed
        g_origAfterBones = mod::api->lookup_function(kAfterBones);
        if (!g_origTree || !g_origAfterBones)
            return;
        mod::api->replace_function(kTreeUpdate, &treeUpdate);
        mod::api->replace_function(kAfterBones, &afterBones);
        // footik -> ok enabled= peds= ms= vic= ground=l,r found=l,r sole=l,r animsole=l,r pelvis= (tools/footik_check.py)
        mod::addCommand("footik", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[400];
            std::snprintf(text, sizeof(text),
                          "ok enabled=%d adjusted=%u peds=%u ms=%.3f vic=%d ground=%.3f,%.3f found=%d,%d sole=%.3f,%.3f "
                          "animsole=%.3f,%.3f pelvis=%.3f",
                          g_enabled ? 1 : 0, g_adjusted.load(), st.peds, st.ms, st.vic ? 1 : 0, st.ground[0], st.ground[1], st.found[0] ? 1 : 0,
                          st.found[1] ? 1 : 0, st.sole[0], st.sole[1], st.animSole[0], st.animSole[1], st.pelvis);
            reply(text);
        });
    }
}
