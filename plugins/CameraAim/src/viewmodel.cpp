// See viewmodel.h and docs/game-internals.md 12t.
#include "viewmodel.h"

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace vcs::viewmodel
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)

        // the ped (game-internals.md 12c / 12d)
        constexpr uint32_t kPedMoveSpeed = 0x140u; // CPhysical's (x 50 = m/s)
        constexpr uint32_t kPedState = 0x8C4u;
        constexpr uint32_t kPedFlags = 0x1C8u; // 64 bits
        constexpr uint64_t kAimingGun = 0x100u, kRestoringGun = 0x200u;
        constexpr uint32_t kPedIkFlags = 0x330u + 0x2Cu, kAimsWithArm = 4u;
        constexpr uint32_t kPedSeekTarget = 0x450u, kPedLookTarget = 0x6B4u, kPedLookDirection = 0x6B8u, kPedLookTimer = 0x6C8u;
        constexpr uint32_t kPedAimPitch = 0xCACu; // down positive
        constexpr uint32_t kSeekFromTask = 0x0048A638u;
        constexpr uint32_t kPedWeapons = 0x578u, kWeaponSize = 0x1Cu, kPedCurrentSlot = 0x789u;
        constexpr uint32_t kWeaponClip = 0x8u; // a slot: +0 the type, +8 the rounds in the clip (VC's CWeapon)
        constexpr uint32_t kWeaponInfos = 0x00712770u, kWeaponInfoSize = 0x70u;
        // the states the arms are ours in: idle (walking, running too), attacking, fighting, aiming (locked on)
        constexpr uint32_t kStates[] = {1u, 0x10u, 0x11u, 0x16u};
        constexpr uint32_t kStateAttack = 0x10u, kStateFight = 0x11u;

        // the arms: VC's bones 7..11 (left clavicle, upper arm, forearm, hand, fingers), 12..16 (right; the hand 15
        // holds the gun, 12c, 12g)

        constexpr float kPi = 3.14159265f, kTwoPi = 6.2831855f;
        constexpr float kConverge = 30.0f;   // m: the barrel aims at the crosshair this far out
        constexpr float kBlendTime = 0.15f;  // s: eased in / out (a state the arms are not ours in: jumping, entering a car)
        constexpr float kSwayRateTime = 0.08f, kSwayGain = 0.035f, kSwayMax = 0.10f; // s, rad per rad/s, rad
        constexpr float kStride = 1.5f, kBob = 0.006f;                              // m per cycle, m at walking pace
        constexpr float kShoulder[3] = {0.19f, 0.08f, -0.30f}; // m: the right shoulder in the view (the left mirrored)
        constexpr float kMaxPull = 0.12f;      // m: the gun drawn back for the left hand's reach, at most
        constexpr float kRecoilBack = 0.04f, kRecoilUp = 0.012f, kRecoilTurn = 0.10f, kRecoilTime = 0.07f; // m, m, rad, s
        constexpr float kPoleOut = 0.5f;       // the elbows' bend: out (right / left) for each 1 down

        // the hand's place in the view per kind of gun (m: right, ahead, up of the eyes)
        struct Place
        {
            float right, ahead, up, recoil;
        };
        constexpr Place kPistol = {0.15f, 0.38f, -0.16f, 1.0f}, kSmg = {0.14f, 0.36f, -0.16f, 0.6f},
                        kLong = {0.14f, 0.24f, -0.19f, 1.3f};

        std::atomic<bool> g_enabled{true};
        std::atomic<int> g_offsetCm[3] = {{0}, {0}, {0}}; // right, ahead, up
        std::atomic<int> g_swayPct{100};

        // game thread
        struct View
        {
            float src[3], r[3], f[3], u[3];
        };
        bool g_active = false;   // this frame's camera update wants the arms in view
        uint32_t g_frame = 0;    // its frame counter
        View g_view = {};
        bool g_posed = false;    // the aim flags are ours
        int g_type = -1;         // the gun the reference is for
        float g_refPos[3] = {};  // the fore-grip (status)
        float g_pull = 0.0f;     // the gun drawn back this draw (m)
        float g_recoil = 0.0f;   // 1 at a shot, decaying
        int32_t g_clip = -1;     // the gun's rounds in the clip, last update
        bool g_twoHanded = false; // the left hand on the gun too (status)
        float g_blend = 0.0f;    // 0 the game's arms .. 1 ours
        float g_lastYaw = 0.0f, g_lastPitch = 0.0f, g_yawRate = 0.0f, g_pitchRate = 0.0f;
        float g_phase = 0.0f, g_speed = 0.0f;
        float g_dt = 0.02f;

        // status (control channel)
        struct Status
        {
            bool active = false;
            int type = -1;
            float blend = 0, ref[3] = {}, hand[3] = {}, barrel = 0, sway[2] = {};
            uint32_t applied = 0;
            float axes[3][3] = {}; // the game's hand axes in the view
        };
        std::mutex g_statusMutex;
        Status g_status;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint64_t rd64(const uint8_t *ram, uint32_t a)
        {
            uint64_t v;
            std::memcpy(&v, ram + (a & kRamMask), 8);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void wr64(uint8_t *ram, uint32_t a, uint64_t v) { std::memcpy(ram + (a & kRamMask), &v, 8); }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        float wrap(float a) { return std::remainder(a, kTwoPi); }
        float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        void normalise(float *v)
        {
            const float l = std::sqrt(dot(v, v));
            if (l > 1e-6f)
                for (int i = 0; i < 3; ++i)
                    v[i] /= l;
        }

        int currentWeapon(const uint8_t *ram, uint32_t ped)
        {
            const int slot = static_cast<int8_t>(ram[(ped + kPedCurrentSlot) & kRamMask]);
            return slot >= 0 && slot < 10 ? static_cast<int>(rd32(ram, ped + kPedWeapons + slot * kWeaponSize)) : -1;
        }

        // The guns held in view: the pistols, shotguns, SMGs, rifles, the flamethrower, M60 and minigun - not the sniper
        // rifles and the rocket launcher (their sights are the game's cameras), not melee or thrown weapons.
        bool held(int type) { return type >= 19 && type <= 35 && type != 30 && type != 31 && type != 32; }

        Place place(int type)
        {
            if (type == 19 || type == 20)
                return kPistol;
            if (type >= 24 && type <= 27)
                return kSmg;
            return kLong;
        }

        // The support hand's place on a two-handed gun, in its frame (m: along the barrel, left, up) - the converted
        // Immersive Weapons Pack's guns (game-internals.md 12g), measured by eye on screenshots; false: one-handed.
        bool foreGrip(int type, float out[3])
        {
            float along = 0.0f, up = -0.045f;
            switch (type)
            {
            case 21: case 22: along = 0.30f; break; // the pump / the SPAS's fore-end
            case 23: along = 0.20f; break;          // the stubby
            case 27: along = 0.19f; break;          // MP5
            case 28: case 29: along = 0.25f; up = -0.05f; break; // M4, Ruger handguards
            case 33: along = 0.28f; break;          // flamethrower
            case 34: along = 0.30f; up = -0.05f; break; // M60
            case 35: along = 0.22f; up = 0.02f; break;  // the minigun's handle
            default: return false;
            }
            out[0] = along;
            out[1] = 0.0f;
            out[2] = up;
            return true;
        }

        bool armsState(uint32_t state) { return std::find(std::begin(kStates), std::end(kStates), state) != std::end(kStates); }

        // CPed::SetAimFlag's writes (as freeaim.cpp's pose): the gun pointed along the view by the game's own IK.
        void pose(uint8_t *ram, uint32_t ped, int type, float yaw, float pitch)
        {
            wr64(ram, ped + kPedFlags, (rd64(ram, ped + kPedFlags) | kAimingGun) & ~kRestoringGun);
            wrf(ram, ped + kPedLookDirection, wrap(yaw - kPi / 2.0f));
            wr32(ram, ped + kPedLookTimer, 0);
            wr32(ram, ped + kPedLookTarget, 0);
            if (rd32(ram, kSeekFromTask) == 0)
                wr32(ram, ped + kPedSeekTarget, 0);
            const uint32_t table = rd32(ram, kWeaponInfos);
            const uint32_t infos = table ? rd32(ram, table + 4) : 0;
            const uint32_t info = infos ? infos + static_cast<uint32_t>(type) * kWeaponInfoSize : 0;
            const uint32_t ik = rd32(ram, ped + kPedIkFlags);
            wr32(ram, ped + kPedIkFlags, info && (ram[info & kRamMask] & 0x80) ? ik | kAimsWithArm : ik & ~kAimsWithArm);
            wrf(ram, ped + kPedAimPitch, -pitch);
        }

        // CPed::ClearAimFlag's (not during an attack: the game's own aim then).
        void unpose(uint8_t *ram, uint32_t ped)
        {
            const uint32_t state = rd32(ram, ped + kPedState);
            const uint64_t flags = rd64(ram, ped + kPedFlags);
            if (state == kStateAttack || state == kStateFight || !(flags & kAimingGun))
                return;
            wr64(ram, ped + kPedFlags, (flags & ~kAimingGun) | kRestoringGun);
            wr32(ram, ped + kPedIkFlags, rd32(ram, ped + kPedIkFlags) & ~kAimsWithArm);
            wr32(ram, ped + kPedLookTimer, 0);
            wrf(ram, ped + kPedAimPitch, 0.0f);
        }

        void lower(uint8_t *ram, uint32_t ped)
        {
            if (g_posed && ped)
                unpose(ram, ped);
            g_posed = false;
        }

        // world <-> the view's frame (x right, y ahead, z up of the eyes)
        void toView(const float *w, bool point, float *l)
        {
            float d[3] = {w[0], w[1], w[2]};
            if (point)
                for (int i = 0; i < 3; ++i)
                    d[i] -= g_view.src[i];
            l[0] = dot(d, g_view.r);
            l[1] = dot(d, g_view.f);
            l[2] = dot(d, g_view.u);
        }
        void toWorld(const float *l, bool point, float *w)
        {
            for (int i = 0; i < 3; ++i)
                w[i] = (point ? g_view.src[i] : 0.0f) + g_view.r[i] * l[0] + g_view.f[i] * l[1] + g_view.u[i] * l[2];
        }

        // The rotation taking unit a onto unit b (3x3, row-major), the shortest.
        void rotationBetween(const float *a, const float *b, float m[9])
        {
            const float v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            const float c = dot(a, b);
            const float k = c > -0.999f ? 1.0f / (1.0f + c) : 0.0f;
            const float vx[9] = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    float sq = 0.0f;
                    for (int n = 0; n < 3; ++n)
                        sq += vx[i * 3 + n] * vx[n * 3 + j];
                    m[i * 3 + j] = (i == j ? 1.0f : 0.0f) + vx[i * 3 + j] + sq * k;
                }
        }
        void mul(const float m[9], const float *v, float *out)
        {
            for (int i = 0; i < 3; ++i)
                out[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
        }

        void publish(const float *hand, float barrel, float swayYaw, float swayPitch, bool applied)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.active = g_active;
            g_status.type = g_type;
            g_status.blend = g_blend;
            std::copy(g_refPos, g_refPos + 3, g_status.ref);
            if (hand)
                std::copy(hand, hand + 3, g_status.hand);
            g_status.barrel = barrel;
            g_status.sway[0] = swayYaw;
            g_status.sway[1] = swayPitch;
            if (applied)
                ++g_status.applied;
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "camera.viewmodel";
        s.page = "Game";
        s.section = "First person";
        s.label = "Arms and gun in view";
        s.help = "First person with a gun: the player's own arms hold it in view, pointed at the middle of the picture, as "
                 "UltimateFP in San Andreas - it sways when you turn and bobs as you walk. Off: the arms are where the "
                 "game's animation puts them (mostly out of view).";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_VIEWMODEL";
        s.onChange = [](int v) { g_enabled = v != 0; };
        s.read = [] { return g_enabled ? 1 : 0; };
        add(s);

        static const char *const kKeys[3] = {"camera.viewmodel_right", "camera.viewmodel_ahead", "camera.viewmodel_up"};
        static const char *const kLabels[3] = {"Gun position: right", "Gun position: ahead", "Gun position: up"};
        for (int axis = 0; axis < 3; ++axis)
        {
            s = {};
            s.key = kKeys[axis];
            s.page = "Game";
            s.section = "First person";
            s.label = kLabels[axis];
            s.help = "Moves the arms and gun in view from where they are held by default (0).";
            s.type = Type::Integer;
            s.min = -20;
            s.max = 20;
            s.format = "%d cm";
            s.defaultValue = 0;
            s.onChange = [axis](int v) { g_offsetCm[axis] = v; };
            add(s);
        }

        s = {};
        s.key = "camera.viewmodel_sway";
        s.page = "Game";
        s.section = "First person";
        s.label = "Gun sway";
        s.help = "How much the gun lags behind the view's turns and bobs while walking (0 = held still).";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 200;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = [](int v) { g_swayPct = v; };
        add(s);
    }

    void install()
    {
        // viewmodel -> ok enabled= active= type= blend= ref=x,y,z hand=x,y,z barrel= sway=y,p applied= (tests)
        mod::addCommand("viewmodel", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[400];
            std::snprintf(text, sizeof(text),
                          "ok enabled=%d active=%d type=%d blend=%.2f ref=%.3f,%.3f,%.3f hand=%.3f,%.3f,%.3f barrel=%.4f pull=%.3f "
                          "sway=%.3f,%.3f applied=%u ax=%.2f,%.2f,%.2f ay=%.2f,%.2f,%.2f az=%.2f,%.2f,%.2f",
                          g_enabled ? 1 : 0, st.active ? 1 : 0, st.type, st.blend, st.ref[0], st.ref[1], st.ref[2], st.hand[0],
                          st.hand[1], st.hand[2], st.barrel, g_pull, st.sway[0], st.sway[1], st.applied, st.axes[0][0], st.axes[0][1], st.axes[0][2],
                          st.axes[1][0], st.axes[1][1], st.axes[1][2], st.axes[2][0], st.axes[2][1], st.axes[2][2]);
            reply(text);
        });
    }

    void frame(uint8_t *ram, uint32_t ped, bool first, bool lockedOn, bool aiming, const float source[3],
               const float front[3], float yaw, float pitch)
    {
        const int type = ped ? currentWeapon(ram, ped) : -1;
        const bool want = g_enabled.load(std::memory_order_relaxed) && first && ped && held(type) &&
                          armsState(rd32(ram, ped + kPedState));
        g_dt = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / 50.0f;
        g_blend = std::clamp(g_blend + (want ? 1.0f : -1.0f) * g_dt / kBlendTime, 0.0f, 1.0f);
        g_active = (want || g_blend > 0.0f) && ped && held(type) && first;
        if (!g_active)
        {
            g_blend = 0.0f;
            if (!aiming)
                lower(ram, ped);
            publish(nullptr, 0.0f, 0.0f, 0.0f, false);
            return;
        }
        if (type != g_type)
        {
            g_type = type;
            g_clip = -1;
        }
        // the view's frame (no roll: up from the pitch)
        std::copy(source, source + 3, g_view.src);
        std::copy(front, front + 3, g_view.f);
        normalise(g_view.f);
        const float h = std::sqrt(g_view.f[0] * g_view.f[0] + g_view.f[1] * g_view.f[1]);
        const float up[3] = {h > 1e-4f ? -g_view.f[0] / h * g_view.f[2] : 0.0f, h > 1e-4f ? -g_view.f[1] / h * g_view.f[2] : 0.0f, h};
        std::copy(up, up + 3, g_view.u);
        g_view.r[0] = g_view.f[1] * up[2] - g_view.f[2] * up[1]; // right = forward x up
        g_view.r[1] = g_view.f[2] * up[0] - g_view.f[0] * up[2];
        g_view.r[2] = g_view.f[0] * up[1] - g_view.f[1] * up[0];
        g_frame = rd32(ram, kFrameCounter);

        // the gun pointed along the view by the game (locked on: at the target, the game's; free aim: its own pose)
        if (want && !lockedOn && !aiming)
        {
            pose(ram, ped, type, yaw, pitch);
            g_posed = true;
        }
        else if (!want)
            lower(ram, ped);

        // sway: the view's turn rates, smoothed; bob: the distance walked
        if (g_dt > 0.0f)
        {
            const float k = 1.0f - std::exp(-g_dt / kSwayRateTime);
            g_yawRate += (wrap(yaw - g_lastYaw) / g_dt - g_yawRate) * k;
            g_pitchRate += ((pitch - g_lastPitch) / g_dt - g_pitchRate) * k;
            const float vx = rdf(ram, ped + kPedMoveSpeed), vy = rdf(ram, ped + kPedMoveSpeed + 4);
            g_speed = std::min(std::sqrt(vx * vx + vy * vy) * 50.0f, 8.0f);
            g_phase = std::fmod(g_phase + g_speed * g_dt / kStride * kTwoPi, kTwoPi);
        }
        g_lastYaw = yaw;
        g_lastPitch = pitch;
        // a shot: the clip's count went down
        const int slot = static_cast<int8_t>(ram[(ped + kPedCurrentSlot) & kRamMask]);
        const int32_t clip = static_cast<int32_t>(rd32(ram, ped + kPedWeapons + static_cast<uint32_t>(slot) * kWeaponSize + kWeaponClip));
        if (g_clip >= 0 && clip < g_clip)
            g_recoil = 1.0f;
        g_clip = clip;
        g_recoil *= std::exp(-g_dt / kRecoilTime);
    }

    void off(uint8_t *ram)
    {
        const uint32_t ped = rd32(ram, 0x004E4910u);
        g_active = false;
        g_blend = 0.0f;
        lower(ram, ped);
        publish(nullptr, 0.0f, 0.0f, 0.0f, false);
    }

    namespace
    {
        // A bone's world matrix (rows: its x, y, z axes, its position).
        struct Bone
        {
            float a[3][3], p[3];
        };
        Bone readBone(const uint8_t *ram, uint32_t bones, uint32_t b)
        {
            Bone o;
            for (int r = 0; r < 3; ++r)
                for (int i = 0; i < 3; ++i)
                    o.a[r][i] = rdf(ram, bones + b * 0x40u + r * 0x10u + 4u * i);
            for (int i = 0; i < 3; ++i)
                o.p[i] = rdf(ram, bones + b * 0x40u + 0x30u + 4u * i);
            return o;
        }
        // eased between the game's (from) and ours (to) by w
        void writeBone(uint8_t *ram, uint32_t bones, uint32_t b, const Bone &from, const Bone &to, float w)
        {
            for (int r = 0; r < 3; ++r)
                for (int i = 0; i < 3; ++i)
                    wrf(ram, bones + b * 0x40u + r * 0x10u + 4u * i, from.a[r][i] + (to.a[r][i] - from.a[r][i]) * w);
            for (int i = 0; i < 3; ++i)
                wrf(ram, bones + b * 0x40u + 0x30u + 4u * i, from.p[i] + (to.p[i] - from.p[i]) * w);
        }
        void sub(const float *a, const float *b, float *o)
        {
            for (int i = 0; i < 3; ++i)
                o[i] = a[i] - b[i];
        }
        float length(const float *v) { return std::sqrt(dot(v, v)); }
        // every axis of `bone` turned by m (world)
        void turn(Bone &bone, const float m[9])
        {
            for (auto &axis : bone.a)
            {
                float o[3];
                mul(m, axis, o);
                std::copy(o, o + 3, axis);
            }
        }
        // `child` carried from `from` to `to` (its place and axes kept in their frame)
        Bone carry(const Bone &child, const Bone &from, const Bone &to)
        {
            Bone o;
            float d[3], l[3];
            sub(child.p, from.p, d);
            for (int k = 0; k < 3; ++k)
                l[k] = dot(d, from.a[k]);
            for (int i = 0; i < 3; ++i)
                o.p[i] = to.p[i] + to.a[0][i] * l[0] + to.a[1][i] * l[1] + to.a[2][i] * l[2];
            for (int r = 0; r < 3; ++r)
            {
                const float c[3] = {dot(child.a[r], from.a[0]), dot(child.a[r], from.a[1]), dot(child.a[r], from.a[2])};
                for (int i = 0; i < 3; ++i)
                    o.a[r][i] = to.a[0][i] * c[0] + to.a[1][i] * c[1] + to.a[2][i] * c[2];
            }
            return o;
        }

        // Two-bone IK: the upper arm (from the shoulder), the forearm, the hand put at `hand` (a pose), the elbow bent
        // toward `pole` (world). Out of reach the clavicle and shoulder slide toward it. The fingers follow the hand.
        void reach(Bone (&arm)[5], const Bone &hand, const float pole[3])
        {
            Bone &clav = arm[0], &upper = arm[1], &fore = arm[2], &h = arm[3], &fingers = arm[4];
            float t[3], d[3];
            sub(fore.p, upper.p, d);
            const float a = length(d);
            sub(h.p, fore.p, d);
            const float b = length(d);
            sub(hand.p, upper.p, t);
            float dist = length(t);
            const float most = (a + b) * 0.995f;
            if (dist > most && dist > 1e-4f)
            {
                const float k = (dist - most) / dist;
                for (int i = 0; i < 3; ++i)
                {
                    clav.p[i] += t[i] * k;
                    upper.p[i] += t[i] * k;
                }
                sub(hand.p, upper.p, t);
                dist = length(t);
            }
            if (dist < 1e-4f || a < 1e-4f || b < 1e-4f)
                return;
            const float n[3] = {t[0] / dist, t[1] / dist, t[2] / dist};
            const float x = std::clamp((a * a + dist * dist - b * b) / (2.0f * dist), -a, a);
            const float hgt = std::sqrt(std::max(a * a - x * x, 0.0f));
            float pp[3];
            const float along = dot(pole, n);
            for (int i = 0; i < 3; ++i)
                pp[i] = pole[i] - n[i] * along;
            normalise(pp);
            float elbow[3];
            for (int i = 0; i < 3; ++i)
                elbow[i] = upper.p[i] + n[i] * x + pp[i] * hgt;
            float m[9], from[3], to[3];
            sub(fore.p, upper.p, from);
            sub(elbow, upper.p, to);
            normalise(from);
            normalise(to);
            rotationBetween(from, to, m);
            turn(upper, m);
            sub(h.p, fore.p, from);
            sub(hand.p, elbow, to);
            normalise(from);
            normalise(to);
            rotationBetween(from, to, m);
            turn(fore, m);
            std::copy(elbow, elbow + 3, fore.p);
            fingers = carry(fingers, h, hand);
            h = hand;
        }
    }

    void apply(uint8_t *ram, uint32_t ped, uint32_t bones)
    {
        (void)ped;
        if (!g_active || !bones || rd32(ram, kFrameCounter) - g_frame > 2u)
            return;
        Bone right[5], left[5];
        for (uint32_t i = 0; i < 5; ++i)
        {
            left[i] = readBone(ram, bones, 7 + i);
            right[i] = readBone(ram, bones, 12 + i);
        }
        const Bone origRight[5] = {right[0], right[1], right[2], right[3], right[4]};
        const Bone origLeft[5] = {left[0], left[1], left[2], left[3], left[4]};
        float hp[3];
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            for (int r = 0; r < 3; ++r)
                toView(right[3].a[r], false, g_status.axes[r]);
        }
        toView(right[3].p, true, hp);
        if (dot(hp, hp) > 4.0f) // not near the eyes: this draw is not the view's (2 m)
            return;

        // where the hand goes (the view's frame): its place (+ the settings, the bob, the recoil), the barrel (its x)
        // on the crosshair kConverge out, its z (the gun's up) the view's up. The anim's own hand (hip holds, fire anims
        // raising the gun 0.3 m) is left out.
        const Place p = place(g_type);
        const float sway = static_cast<float>(g_swayPct.load(std::memory_order_relaxed)) / 100.0f;
        const float walk = std::min(g_speed / 1.5f, 3.0f);
        float pos[3] = {p.right + g_offsetCm[0].load(std::memory_order_relaxed) / 100.0f,
                        p.ahead + g_offsetCm[1].load(std::memory_order_relaxed) / 100.0f,
                        p.up + g_offsetCm[2].load(std::memory_order_relaxed) / 100.0f};
        pos[0] += kBob * walk * sway * std::sin(g_phase);
        pos[2] += kBob * walk * sway * 0.5f * std::cos(2.0f * g_phase);
        float ax[3][3];
        float x0[3] = {-pos[0], kConverge - pos[1], -pos[2]};
        normalise(x0);
        float z0[3] = {0.0f, 0.0f, 1.0f};
        const float zx = dot(z0, x0);
        for (int i = 0; i < 3; ++i)
            z0[i] -= x0[i] * zx;
        normalise(z0);
        // the recoil: back and up, the muzzle raised
        const float kick = g_recoil * p.recoil;
        pos[1] -= kRecoilBack * kick;
        pos[2] += kRecoilUp * kick;
        const float ra = kRecoilTurn * kick, cr = std::cos(ra), sr = std::sin(ra);
        for (float *v : {x0, z0})
        {
            const float y = v[1] * cr - v[2] * sr, z = v[1] * sr + v[2] * cr;
            v[1] = y;
            v[2] = z;
        }
        std::copy(x0, x0 + 3, ax[0]);
        std::copy(z0, z0 + 3, ax[2]);
        ax[1][0] = ax[2][1] * ax[0][2] - ax[2][2] * ax[0][1]; // y = z x x
        ax[1][1] = ax[2][2] * ax[0][0] - ax[2][0] * ax[0][2];
        ax[1][2] = ax[2][0] * ax[0][1] - ax[2][1] * ax[0][0];
        // the lag: turned against the view's turn, about the eyes
        const float sy = std::clamp(-g_yawRate * kSwayGain * sway, -kSwayMax, kSwayMax);
        const float spch = std::clamp(-g_pitchRate * kSwayGain * sway, -kSwayMax, kSwayMax);
        const float cy = std::cos(sy), sny = std::sin(sy), cpch = std::cos(spch), snp = std::sin(spch);
        auto swayed = [&](float *v) {
            const float x = v[0] * cy - v[1] * sny, y = v[0] * sny + v[1] * cy;
            v[0] = x;
            v[1] = y * cpch - v[2] * snp;
            v[2] = y * snp + v[2] * cpch;
        };
        swayed(pos);
        for (auto &axis : ax)
            swayed(axis);
        Bone target;
        toWorld(pos, true, target.p);
        for (int r = 0; r < 3; ++r)
            toWorld(ax[r], false, target.a[r]);

        // the support hand on the gun's fore-grip (not where VCS's anims put it: their hip holds leave it 0.47 m out along
        // a rifle, palm open beside the barrel): a point per weapon in the gun's frame (the right hand's: x the barrel,
        // z up), the hand turned half a turn about the barrel from the gun's axes - fingers along it, wrapped round
        // from under it. One-handed guns (pistols, the small SMGs) leave the left arm to the game.
        float grip[3];
        const bool twoHanded = foreGrip(g_type, grip);
        std::copy(grip, grip + 3, g_refPos);
        Bone leftTarget;
        for (int i = 0; i < 3; ++i)
        {
            leftTarget.p[i] = target.p[i];
            for (int k = 0; k < 3; ++k)
                leftTarget.p[i] += target.a[k][i] * grip[k];
            leftTarget.a[0][i] = target.a[0][i];
            leftTarget.a[1][i] = -target.a[1][i];
            leftTarget.a[2][i] = -target.a[2][i];
        }
        // the shoulders at fixed places of the view, under and a little ahead of the eyes - outside the picture, so
        // only the forearms and hands come into view (the game's shoulders are level with the camera: the upper arms
        // filled the lower corners); the whole arm carried there before the IK
        for (int side = 0; side < 2; ++side)
        {
            Bone(&arm)[5] = side ? left : right;
            if (side && !twoHanded)
                continue;
            const float want[3] = {side ? -kShoulder[0] : kShoulder[0], kShoulder[1], kShoulder[2]};
            float at[3], d[3];
            toWorld(want, true, at);
            sub(at, arm[1].p, d);
            for (Bone &b : arm)
                for (int i = 0; i < 3; ++i)
                    b.p[i] += d[i];
        }
        // a left hand out of the left arm's reach (a rifle's foregrip): the gun drawn back along its barrel, as a stock
        // comes to the shoulder, instead of the shoulder sliding into view
        g_pull = 0.0f;
        if (twoHanded)
        {
            float d[3];
            sub(left[2].p, left[1].p, d);
            const float a = length(d);
            sub(left[3].p, left[2].p, d);
            const float most = (a + length(d)) * 0.97f;
            for (int pass = 0; pass < 3; ++pass)
            {
                sub(leftTarget.p, left[1].p, d);
                const float excess = length(d) - most;
                if (excess <= 0.0f || g_pull >= kMaxPull)
                    break;
                const float back = std::min(excess, kMaxPull - g_pull);
                g_pull += back;
                for (int i = 0; i < 3; ++i)
                {
                    target.p[i] -= target.a[0][i] * back;
                    leftTarget.p[i] -= target.a[0][i] * back;
                }
            }
        }

        // the elbows bent down and out
        float pole[3];
        const float rightPole[3] = {kPoleOut, -0.2f, -1.0f}, leftPole[3] = {-kPoleOut, -0.2f, -1.0f};
        toWorld(rightPole, false, pole);
        reach(right, target, pole);
        if (twoHanded)
        {
            toWorld(leftPole, false, pole);
            reach(left, leftTarget, pole);
        }

        const float w = g_blend * g_blend * (3.0f - 2.0f * g_blend);
        for (uint32_t i = 0; i < 5; ++i)
        {
            writeBone(ram, bones, 12 + i, origRight[i], right[i], w);
            if (twoHanded)
                writeBone(ram, bones, 7 + i, origLeft[i], left[i], w);
        }
        g_twoHanded = twoHanded;
        float handView[3], barrel[3];
        toView(right[3].p, true, handView);
        toView(right[3].a[0], false, barrel);
        normalise(barrel);
        publish(handView, barrel[1], sy, spch, true);
    }
}
