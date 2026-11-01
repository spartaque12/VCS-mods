// See viewmodel.h and docs/game-internals.md 12t.
#include "viewmodel.h"

#include "mod.h"
#include "aimwalk.h"
#include "reload.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
        // the barrel parallel to the view: on screen its line runs into the crosshair's dot (the view's vanishing point)
        // at every pitch, yaw and distance, wherever the hand is; a distance > 0 aims it at the view's line that far out
        // instead (it crosses the dot there and runs past it beyond - the earlier 30 m / SA's 2 m)
        constexpr float kConverge = 0.0f;
        constexpr float kBlendTime = 0.15f;  // s: eased in / out (a state the arms are not ours in: jumping, entering a car)
        constexpr float kSwayRateTime = 0.08f, kSwayGain = 0.035f, kSwayMax = 0.10f; // s, rad per rad/s, rad
        constexpr float kStride = 1.5f, kBob = 0.006f;                              // m per cycle, m at walking pace
        constexpr float kShoulder[3] = {0.19f, 0.08f, -0.30f}; // m: the right shoulder in the view (the left mirrored)
        constexpr float kMaxPull = 0.12f;      // m: the gun drawn back for the left hand's reach, at most
        // at rest (not aiming): the hand lower and back, the barrel still on the dot; raised while R1 / the right mouse
        // button aims or the gun fires, eased like UltimateFP's ADS (200 ms up, 180 ms down)
        constexpr float kRestOffset[3] = {0.0f, -0.03f, -0.04f}; // m
        constexpr float kReloadOffset[3] = {-0.04f, -0.02f, -0.025f}, kReloadPitch = -0.12f, kReloadRoll = 0.7f; // m, rad
        constexpr float kReloadEase = 0.15f; // s
        constexpr float kAdsUp = 0.20f, kAdsDown = 0.18f, kAdsUpFiring = 0.08f;                          // s
        constexpr float kRecoilBack = 0.04f, kRecoilUp = 0.012f, kRecoilTurn = 0.10f, kRecoilTime = 0.07f; // m, m, rad, s
        constexpr float kPoleOut = 0.5f;       // the elbows' bend: out (right / left) for each 1 down

        // the hand's place in the view per kind of gun (m: right, ahead, up of the eyes)
        struct Place
        {
            float right, ahead, up, recoil;
        };
        constexpr Place kPistol = {0.15f, 0.38f, -0.16f, 1.0f}, kSmg = {0.14f, 0.36f, -0.16f, 0.6f},
                        kLong = {0.14f, 0.24f, -0.19f, 1.3f};

        // camera.fp_hold: how first person holds a gun
        enum Hold : int
        {
            kHoldBody = 0,      // as in third person: the body's anim, raised only by the aim (animate)
            kHoldViewmodel = 1, // UltimateFP's viewmodel (this file's IK)
            kHoldGame = 2,      // the game's arms, nothing moved
        };
        std::atomic<int> g_hold{kHoldViewmodel};
        std::atomic<bool> g_enabled{true}; // the viewmodel (g_hold == kHoldViewmodel)
        std::atomic<bool> g_aimRaise{true}; // camera.aim_raise

        // the aim raise (game-internals.md 12d / 12w): the game's anim calls
        constexpr uint32_t kBlendAnimation = 0x00215868u; // (ped, clump, group, anim id, f12 blend) -> association
        constexpr uint32_t kGetAssociation = 0x002159A8u; // (ped, clump, anim id) -> the clump's association or 0
        constexpr uint32_t kPointGunAt = 0x001D06A0u;     // (ped): the fire anim held at its aim point (state 0x16's)
        constexpr uint32_t kAnimFire = 0xD6u;             // ANIM_WEAPON_FIRE (0xD7 the crouched one)
        constexpr uint32_t kPedClump = 0x50u, kWeaponInfoGroup = 0x40u;
        constexpr uint32_t kAssocFlags = 0x08u, kAssocBlendDelta = 0x24u; // flags 4: deleted once blended out
        constexpr uint32_t kPedVehicle = 0x480u;
        constexpr uint64_t kDuckingBits = 0xC000ull << 30; // ped +0x1C8: crouched (SetPointGunAt's test)
        VcsGuestFn g_blendAnimation = nullptr, g_getAssociation = nullptr, g_pointGunAt = nullptr;
        // frame() -> animate(), the same camera update
        struct AimWant
        {
            uint32_t frame = 0, ped = 0;
            bool aiming = false, lockedOn = false;
        } g_aimWant;
        std::atomic<bool> g_fpAimEye{true}; // camera.fp_aim_eye: as in third person, aiming brings the gun to the eye
        bool g_body = false;        // this update: the body hold with the eye raise (frame -> apply)
        bool g_raised = false;      // the fire anim is ours (held at its aim point)
        uint32_t g_raisedPed = 0;
        uint32_t g_raiseCount = 0;  // status: anims started
        uint32_t g_assocDbg = 0;    // status: the held fire anim's association
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
        float g_reload = 0.0f;   // 0 .. 1 the reload pose's share (the gun lower still, tipped in)
        float g_ads = 0.0f;      // 0 the gun at rest (low ready) .. 1 raised to the crosshair (aiming, firing)
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
            float elbows[2][3] = {}, leftHand[3] = {}; // ours, in the view (right, left)
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

        // San Andreas' own two-handed holds (game-internals.md 12w): from the Immersive Weapons Pack's fire anims at the
        // fire point by forward kinematics (bone offsets from its RIFLE_fire, which carries them) - the left hand's place
        // (m) and rotation (x y z w) in the right hand's frame (VC's / SA's bone 15 / 24: x the barrel, z the gun's up),
        // and both finger bones' local rotations (SA's 35 / 25 -> VCS's 11 / 16). The IWP models keep SA's attachment
        // to the hand, so these put the left hand on the handguard (not the magazine, as the old by-eye points below did).
        struct SaHold
        {
            float pos[3], q[4], leftFingers[4], rightFingers[4];
        };
        constexpr SaHold kSaRifle = {{0.2999f, 0.0339f, 0.0596f}, {-0.9143f, 0.2159f, 0.0974f, -0.3286f},
                                     {-0.4454f, -0.2676f, 0.3699f, 0.7702f}, {0.2703f, 0.0850f, 0.3260f, 0.9019f}}; // RIFLE_fire
        constexpr SaHold kSaShotgun = {{0.3338f, 0.0188f, 0.0116f}, {-0.8955f, 0.2420f, 0.0339f, -0.3721f},
                                       {-0.5003f, -0.3189f, 0.2964f, 0.7484f}, {0.5348f, 0.2774f, 0.4527f, 0.6574f}}; // newshot_fire
        constexpr SaHold kSaStubby = {{0.3026f, 0.0445f, 0.0310f}, {-0.8164f, 0.4050f, -0.0888f, -0.4021f},
                                      {-0.5365f, -0.3636f, 0.1458f, 0.7475f}, {0.3702f, 0.2679f, 0.5787f, 0.6755f}}; // buddy_fire
        constexpr SaHold kSaSmg = {{0.2357f, 0.0510f, 0.0610f}, {-0.8747f, 0.3133f, 0.1142f, -0.3517f},
                                   {-0.4264f, -0.2315f, 0.2662f, 0.8329f}, {0.1136f, -0.0608f, 0.2847f, 0.9499f}}; // UZI_fire
        constexpr SaHold kSaTec = {{0.0333f, 0.0838f, -0.0046f}, {0.9891f, -0.0697f, 0.1261f, 0.0300f},
                                   {-0.1235f, -0.1394f, 0.2691f, 0.9449f}, {0.1856f, -0.0925f, 0.1323f, 0.9693f}}; // tecani_fire_2hands
        constexpr SaHold kSaColt = {{0.0203f, 0.1127f, -0.0180f}, {-0.9609f, 0.2130f, -0.0281f, -0.1746f},
                                    {-0.0884f, -0.1880f, 0.3201f, 0.9243f}, {0.1685f, 0.0422f, 0.3751f, 0.9106f}}; // colt45_fire_2hands
        constexpr SaHold kSaPython = {{0.0243f, 0.0571f, 0.0339f}, {0.9176f, -0.0669f, -0.0378f, 0.3899f},
                                      {-0.1560f, -0.0904f, -0.2535f, -0.9504f}, {0.1138f, -0.2854f, 0.1333f, 0.9422f}}; // python_fire
        const SaHold *saHold(int type)
        {
            switch (type)
            {
            case 19: return &kSaColt;
            case 20: return &kSaPython;
            case 21: case 22: return &kSaShotgun;
            case 23: return &kSaStubby;
            case 24: return &kSaTec;
            case 25: case 26: case 27: return &kSaSmg;
            case 28: case 29: case 34: return &kSaRifle;
            default: return nullptr; // the flamethrower, the minigun: the by-eye points below
            }
        }
        // a unit quaternion's matrix, m[row][col] (column i = the rotated axis i)
        void quatMatrix(const float q[4], float m[3][3])
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
            m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
            m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
        }
        // axes (rows, world) of a child turned by local rotation q from `parent`'s axes
        void turnedAxes(const float parent[3][3], const float q[4], float out[3][3])
        {
            float m[3][3];
            quatMatrix(q, m);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    out[i][j] = m[0][i] * parent[0][j] + m[1][i] * parent[1][j] + m[2][i] * parent[2][j];
        }
        // the right hand's place (m: right, ahead, up of the eyes), the barrel parallel to the view (kConverge), and both
        // shoulders (x right) - under the picture, so only forearms and hands show, as SA's first person: the right
        // forearm from the lower right corner to the grip, the left from the bottom middle up to the handguard
        struct SaPlace
        {
            float right, ahead, up, converge, left[3], rightShoulder[3];
        };
        constexpr SaPlace kSaLong = {0.17f, 0.36f, -0.22f, kConverge, {0.04f, 0.42f, -0.62f}, {0.24f, 0.05f, -0.40f}},
                          kSaSmgPlace = {0.17f, 0.36f, -0.22f, kConverge, {0.04f, 0.42f, -0.62f}, {0.24f, 0.05f, -0.40f}},
                          kSaPistolPlace = {0.17f, 0.36f, -0.22f, kConverge, {0.04f, 0.42f, -0.62f}, {0.24f, 0.05f, -0.40f}},
                          kSaHeavyPlace = {0.30f, 0.40f, -0.24f, kConverge, {0.08f, 0.45f, -0.65f}, {0.30f, 0.08f, -0.40f}};
        const SaPlace &saPlace(int type)
        {
            if (type == 19 || type == 20)
                return kSaPistolPlace;
            if (type >= 24 && type <= 27)
                return kSaSmgPlace;
            if (type >= 33)
                return kSaHeavyPlace;
            return kSaLong;
        }
        // The bore's direction in the hand's frame (x the hand's barrel axis, y left, z up: the tangents of its turn left
        // and up) - the converted SA models are not drawn with the bore exactly along the hand's x (SA's anims point them):
        // measured on screenshots per gun, so the drawn barrel's line runs into the dot (game-internals.md 12t)
        struct Bore
        {
            float left, up;
        };
        Bore bore(int type)
        {
            switch (type) // (the vmbore command, hidden runs at FOV 87: the barrel's line through the dot, 2026-11-02)
            {
            case 19: return {-0.070f, 0.075f};  // Colt45
            case 20: return {-0.025f, -0.074f}; // Python (Desert Eagle)
            case 21: return {-0.110f, 0.130f};  // Shotgun (chromegun)
            case 22: return {-0.300f, 0.000f};  // SPAS-12
            case 23: return {0.000f, 0.160f};   // stubby (sawn-off)
            case 24: return {-0.067f, 0.155f};  // Tec9
            case 25: return {-0.110f, 0.030f};  // Uzi
            case 26: return {0.000f, 0.080f};   // silenced Ingram (VCS's model)
            case 27: return {-0.060f, 0.035f};  // MP5
            case 28: return {-0.086f, 0.050f};  // M4
            case 29: return {-0.064f, 0.042f};  // Ruger (AK-47)
            case 33: return {0.000f, 0.650f};   // flamethrower (held by its handle, the nozzle 33 degrees up)
            case 34: return {0.000f, 0.300f};   // M60 (VCS's model, by its carry handle)
            case 35: return {-0.060f, 0.450f};  // minigun (by its top handle)
            default: return {0.0f, 0.0f};
            }
        }
        std::atomic<bool> g_boreTune{false};
        std::atomic<float> g_boreTuned[2] = {{0.0f}, {0.0f}};
        std::atomic<bool> g_saHolds{true}; // camera.fp_sa_hands
        std::atomic<bool> g_placeTune{false};
        std::atomic<float> g_tune[10] = {{0.0f}, {0.0f}, {0.0f}, {0.0f}, {-0.19f}, {0.08f}, {-0.30f}, {0.19f}, {0.08f}, {-0.30f}}; // + converge, left, right shoulder

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
        s.key = "camera.fp_hold";
        s.page = "Game";
        s.section = "First person";
        s.label = "Gun hold";
        s.help = "How first person holds a gun. Viewmodel (the default): both arms hold the gun in view at the lower right as "
                 "in San Andreas' first person - right hand on the grip, left hand under the handguard, forearms from the "
                 "bottom of the picture - its barrel always running into the crosshair's dot, lowered a little until you "
                 "aim, swaying and bobbing (the gun position / sway rows). As in third person: the body's own animation "
                 "holds it - exactly what third person shows (look down to see it) - and it comes up only while you aim "
                 "(right mouse button / R1). Game's arms: nothing moved.";
        s.type = Type::Choice;
        s.choices = {{"As in third person", kHoldBody}, {"Viewmodel (SA style)", kHoldViewmodel}, {"Game's arms", kHoldGame}};
        s.defaultValue = kHoldViewmodel;
        s.env = "VCS_FP_HOLD";
        s.onChange = [](int v) {
            g_hold = std::clamp(v, 0, 2);
            g_enabled = g_hold == kHoldViewmodel;
        };
        s.read = [] { return g_hold.load(); };
        add(s);

        s = {};
        s.key = "camera.aim_raise";
        s.page = "Game";
        s.section = "First person";
        s.label = "Raise the gun while aiming";
        s.help = "Free aim (right mouse button / R1 without a lock-on): rifles, shotguns, SMGs and the heavy guns come up "
                 "to the shoulder in the game's own aim pose - what locking on shows - in third and first person, and go "
                 "back down when you let go. Off: they stay at the hip until fired (the pistols come up either way).";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_AIM_RAISE";
        s.onChange = [](int v) { g_aimRaise = v != 0; };
        add(s);

        s = {};
        s.key = "camera.fp_aim_eye";
        s.page = "Game";
        s.section = "First person";
        s.label = "Aiming brings the gun to the eye";
        s.help = "With the gun hold \"As in third person\": while you aim in first person the arms bring the gun up in front "
                 "of you, its barrel on the dot (San Andreas' look), easing back to the body's hold when you let go. Off: "
                 "first person shows the game's own aim pose, as third person does (the gun at the shoulder, mostly below "
                 "the picture).";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = [](int v) { g_fpAimEye = v != 0; };
        add(s);

        s = {};
        s.key = "camera.fp_sa_hands";
        s.page = "Game";
        s.section = "First person";
        s.label = "San Andreas hands on the gun";
        s.help = "In first person the hands hold the gun as San Andreas' Immersive Weapons Pack does: the left hand on the "
                 "handguard of rifles, shotguns and SMGs (wrapped round the right one on pistols), SA's finger pose, the "
                 "arms placed as SA's first person. Off: the earlier hand places.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = [](int v) { g_saHolds = v != 0; };
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
            s.help = "UltimateFP viewmodel: moves the arms and gun in view from where they are held by default (0).";
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
        s.help = "UltimateFP viewmodel: how much the gun lags behind the view's turns and bobs while walking (0 = held still).";
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
        // vmplace right ahead up [converge [left shoulder x y z [right shoulder x y z]]] | vmplace off (tests: the hand's
        // place in view, every gun)
        mod::addCommand("vmplace", [](const std::string &args, mod::Reply reply) {
            float v[10] = {0.0f, 0.0f, 0.0f, kConverge, -kShoulder[0], kShoulder[1], kShoulder[2], kShoulder[0], kShoulder[1], kShoulder[2]};
            if (std::sscanf(args.c_str(), "%f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                            &v[8], &v[9]) >= 3)
            {
                for (int i = 0; i < 10; ++i)
                    g_tune[i] = v[i];
                g_placeTune = true;
            }
            else
                g_placeTune = false;
            reply(g_placeTune ? "ok tuned" : "ok off");
        });
        // vmbore left up | vmbore off (tests: the bore's direction in the hand's frame, tangents)
        mod::addCommand("vmbore", [](const std::string &args, mod::Reply reply) {
            float v[2] = {0.0f, 0.0f};
            if (std::sscanf(args.c_str(), "%f %f", &v[0], &v[1]) == 2)
            {
                g_boreTuned[0] = v[0];
                g_boreTuned[1] = v[1];
                g_boreTune = true;
            }
            else
                g_boreTune = false;
            reply(g_boreTune ? "ok tuned" : "ok off");
        });
        mod::addCommand("viewmodel", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[800];
            std::snprintf(text, sizeof(text),
                          "ok hold=%d aimraise=%d raised=%d raises=%u assoc=%08x enabled=%d active=%d type=%d blend=%.2f ref=%.3f,%.3f,%.3f hand=%.3f,%.3f,%.3f barrel=%.4f pull=%.3f "
                          "ads=%.2f reload=%.2f sway=%.3f,%.3f applied=%u ax=%.2f,%.2f,%.2f ay=%.2f,%.2f,%.2f az=%.2f,%.2f,%.2f",
                          g_hold.load(), g_aimRaise ? 1 : 0, g_raised ? 1 : 0, g_raiseCount, g_assocDbg, g_enabled ? 1 : 0, st.active ? 1 : 0, st.type, st.blend, st.ref[0], st.ref[1], st.ref[2], st.hand[0],
                          st.hand[1], st.hand[2], st.barrel, g_pull, g_ads, g_reload, st.sway[0], st.sway[1], st.applied, st.axes[0][0], st.axes[0][1], st.axes[0][2],
                          st.axes[1][0], st.axes[1][1], st.axes[1][2], st.axes[2][0], st.axes[2][1], st.axes[2][2]);
            const size_t n = std::strlen(text);
            std::snprintf(text + n, sizeof(text) - n, " elbowR=%.3f,%.3f,%.3f elbowL=%.3f,%.3f,%.3f handL=%.3f,%.3f,%.3f",
                          st.elbows[0][0], st.elbows[0][1], st.elbows[0][2], st.elbows[1][0], st.elbows[1][1], st.elbows[1][2],
                          st.leftHand[0], st.leftHand[1], st.leftHand[2]);
            reply(text);
        });
    }

    void frame(uint8_t *ram, uint32_t ped, bool first, bool lockedOn, bool aiming, const float source[3],
               const float front[3], float yaw, float pitch)
    {
        g_aimWant = {rd32(ram, kFrameCounter), ped, aiming, lockedOn};
        const int type = ped ? currentWeapon(ram, ped) : -1;
        // as in third person: the viewmodel's raised pose only while aiming (its share the ADS ease), the body's own
        // hold otherwise
        g_body = g_hold.load(std::memory_order_relaxed) == kHoldBody && g_fpAimEye.load(std::memory_order_relaxed);
        const bool armed = first && ped && held(type) && armsState(rd32(ram, ped + kPedState));
        const bool want = armed && (g_enabled.load(std::memory_order_relaxed) ||
                                    (g_body && (aiming || lockedOn || g_ads > 0.0f)));
        g_dt = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / 50.0f;
        g_blend = std::clamp(g_blend + (want ? 1.0f : -1.0f) * g_dt / kBlendTime, 0.0f, 1.0f);
        g_active = (want || g_blend > 0.0f) && ped && held(type) && first;
        if (!g_active)
        {
            g_blend = 0.0f;
            if (g_body)
                g_ads = 0.0f;
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
        if (want && !lockedOn && !aiming && !g_body)
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
        // raised while aiming (free aim's R1, locked on) or attacking (firing from the hip: up fast)
        {
            const uint32_t state = rd32(ram, ped + kPedState);
            const bool firing = state == kStateAttack || state == kStateFight || g_recoil > 0.05f;
            // reloading (the slot's state 2): down, whatever else
            const bool reloadingNow = reload::reloading(ram, ped);
            g_reload = std::clamp(g_reload + (reloadingNow ? 1.0f : -1.0f) * g_dt / kReloadEase, 0.0f, 1.0f);
            // (as in third person: aiming only - hip fire is the body's fire anim, as third person's)
            const bool raise = (aiming || lockedOn || (firing && !g_body)) && !reloadingNow;
            const float t = raise ? (firing && !aiming && !lockedOn ? kAdsUpFiring : kAdsUp) : kAdsDown;
            g_ads = std::clamp(g_ads + (raise ? 1.0f : -1.0f) * g_dt / t, 0.0f, 1.0f);
        }
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
        // San Andreas' raised hold (its FP shots, matched side by side): lower right, the barrel angled in to the middle a few
        // metres out, the left shoulder low in the middle so the forearm rises from the bottom centre to the handguard
        const SaHold *sa = g_saHolds.load(std::memory_order_relaxed) ? saHold(g_type) : nullptr;
        Place p = place(g_type);
        float converge = kConverge;
        float leftShoulder[3] = {-kShoulder[0], kShoulder[1], kShoulder[2]};
        float rightShoulder[3] = {kShoulder[0], kShoulder[1], kShoulder[2]};
        if (g_saHolds.load(std::memory_order_relaxed)) // (the flamethrower / minigun too: the by-eye left hand below)
        {
            const SaPlace &s = saPlace(g_type);
            p = {s.right, s.ahead, s.up, p.recoil};
            converge = s.converge;
            std::copy(s.left, s.left + 3, leftShoulder);
            std::copy(s.rightShoulder, s.rightShoulder + 3, rightShoulder);
        }
        if (g_placeTune.load(std::memory_order_relaxed)) // tests: "vmplace right ahead up ..."
        {
            p = {g_tune[0].load(), g_tune[1].load(), g_tune[2].load(), p.recoil};
            converge = g_tune[3].load();
            for (int i = 0; i < 3; ++i)
            {
                leftShoulder[i] = g_tune[4 + i].load();
                rightShoulder[i] = g_tune[7 + i].load();
            }
        }
        const float sway = static_cast<float>(g_swayPct.load(std::memory_order_relaxed)) / 100.0f;
        const float walk = std::min(g_speed / 1.5f, 3.0f);
        float pos[3] = {p.right + g_offsetCm[0].load(std::memory_order_relaxed) / 100.0f,
                        p.ahead + g_offsetCm[1].load(std::memory_order_relaxed) / 100.0f,
                        p.up + g_offsetCm[2].load(std::memory_order_relaxed) / 100.0f};
        pos[0] += kBob * walk * sway * std::sin(g_phase);
        pos[2] += kBob * walk * sway * 0.5f * std::cos(2.0f * g_phase);
        float ax[3][3];
        float x0[3] = {0.0f, 1.0f, 0.0f};
        if (converge > 0.0f)
        {
            x0[0] = -pos[0], x0[1] = converge - pos[1], x0[2] = -pos[2];
            normalise(x0);
        }
        float z0[3] = {0.0f, 0.0f, 1.0f};
        const float zx = dot(z0, x0);
        for (int i = 0; i < 3; ++i)
            z0[i] -= x0[i] * zx;
        normalise(z0);
        // at rest: lowered and turned down / in (smoothstep of the aim's share)
        {
            // (as in third person the raised pose itself, eased in from the body's hold by the weight below)
            const float a = g_body ? 1.0f : g_ads * g_ads * (3.0f - 2.0f * g_ads), rest = 1.0f - a;
            for (int i = 0; i < 3; ++i)
                pos[i] += kRestOffset[i] * rest;
            const float rl = g_reload * g_reload * (3.0f - 2.0f * g_reload);
            for (int i = 0; i < 3; ++i)
                pos[i] += kReloadOffset[i] * rl;
            // reloading: rolled over toward the left hand (the gun's up turned about the barrel)
            if (rl > 0.0f)
            {
                const float rr = kReloadRoll * rl, crr = std::cos(rr), srr = std::sin(rr);
                const float side[3] = {x0[1] * z0[2] - x0[2] * z0[1], x0[2] * z0[0] - x0[0] * z0[2], x0[0] * z0[1] - x0[1] * z0[0]};
                for (int i = 0; i < 3; ++i)
                    z0[i] = z0[i] * crr - side[i] * srr;
            }
            // (the barrel stays on the dot at rest: lowered, not turned away - only reloading tips it)
            const float yw = 0.0f, pt = kReloadPitch * rl;
            const float cyw = std::cos(yw), syw = std::sin(yw), cpt = std::cos(pt), spt = std::sin(pt);
            for (float *v : {x0, z0})
            {
                const float y1 = v[1] * cpt - v[2] * spt, z1 = v[1] * spt + v[2] * cpt; // pitch about the view's right
                v[1] = y1;
                v[2] = z1;
                const float x2 = v[0] * cyw - v[1] * syw, y2 = v[0] * syw + v[1] * cyw; // yaw about its up (left +)
                v[0] = x2;
                v[1] = y2;
            }
        }
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
        // so far the bore's frame (x the bore, z the gun's up): the hand turned so that the model's bore lies along it
        {
            Bore b = bore(g_type);
            if (g_boreTune.load(std::memory_order_relaxed))
                b = {g_boreTuned[0].load(), g_boreTuned[1].load()};
            if (b.left != 0.0f || b.up != 0.0f)
            {
                // the bore's frame in the hand's: b1 the bore, b3 the hand's z made square to it, b2 = b3 x b1
                float b1[3] = {1.0f, b.left, b.up};
                normalise(b1);
                float b3[3] = {-b1[2] * b1[0], -b1[2] * b1[1], 1.0f - b1[2] * b1[2]};
                normalise(b3);
                const float b2[3] = {b3[1] * b1[2] - b3[2] * b1[1], b3[2] * b1[0] - b3[0] * b1[2], b3[0] * b1[1] - b3[1] * b1[0]};
                // the hand's axis k in the view = sum over i of (the bore frame's axis i in the view) x (b_i)_k
                float hand[3][3];
                for (int k = 0; k < 3; ++k)
                    for (int i = 0; i < 3; ++i)
                        hand[k][i] = ax[0][i] * b1[k] + ax[1][i] * b2[k] + ax[2][i] * b3[k];
                std::copy(&hand[0][0], &hand[0][0] + 9, &ax[0][0]);
            }
        }
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
        // (the place only: the barrel stays parallel to the view, its line on the dot while turning)
        swayed(pos);
        Bone target;
        toWorld(pos, true, target.p);
        for (int r = 0; r < 3; ++r)
            toWorld(ax[r], false, target.a[r]);

        // the support hand on the gun's fore-grip (not where VCS's anims put it: their hip holds leave it 0.47 m out along
        // a rifle, palm open beside the barrel): a point per weapon in the gun's frame (the right hand's: x the barrel,
        // z up), the hand turned half a turn about the barrel from the gun's axes - fingers along it, wrapped round
        // from under it. One-handed guns (pistols, the small SMGs) leave the left arm to the game.
        // San Andreas' holds (saHold): its left hand's place and turn in the right hand's frame, every gun it has one for,
        // pistols too (the pack holds them two-handed)
        float grip[3] = {0.0f, 0.0f, 0.0f};
        const bool twoHanded = sa ? true : foreGrip(g_type, grip);
        if (sa)
            std::copy(sa->pos, sa->pos + 3, grip);
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
        if (sa)
            turnedAxes(target.a, sa->q, leftTarget.a);
        // the shoulders at fixed places of the view, under and a little ahead of the eyes - outside the picture, so
        // only the forearms and hands come into view (the game's shoulders are level with the camera: the upper arms
        // filled the lower corners); the whole arm carried there before the IK
        for (int side = 0; side < 2; ++side)
        {
            Bone(&arm)[5] = side ? left : right;
            if (side && !twoHanded)
                continue;
            float want[3] = {rightShoulder[0], rightShoulder[1], rightShoulder[2]};
            if (side)
                std::copy(leftShoulder, leftShoulder + 3, want);
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
        // SA's fingers: each finger bone turned from its hand as SA's anim turns it (kept at its place on the hand)
        if (sa)
        {
            turnedAxes(right[3].a, sa->rightFingers, right[4].a);
            turnedAxes(left[3].a, sa->leftFingers, left[4].a);
        }

        float w =g_blend * g_blend * (3.0f - 2.0f * g_blend);
        if (g_body)
            w *= g_ads * g_ads * (3.0f - 2.0f * g_ads);
        for (uint32_t i = 0; i < 5; ++i)
        {
            writeBone(ram, bones, 12 + i, origRight[i], right[i], w);
            if (twoHanded)
                writeBone(ram, bones, 7 + i, origLeft[i], left[i], w);
        }
        g_twoHanded = twoHanded;
        float handView[3], barrel[3];
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            toView(right[2].p, true, g_status.elbows[0]);
            toView(left[2].p, true, g_status.elbows[1]);
            toView(left[3].p, true, g_status.leftHand);
        }
        toView(right[3].p, true, handView);
        toView(right[3].a[0], false, barrel);
        normalise(barrel);
        publish(handView, barrel[1], sy, spch, true);
    }

    namespace
    {
        // A call into the game from a hook's context (at a function's entry: its stack free below sp): the arguments in
        // a0..a3 / f12, the guest's registers and pc put back afterwards. Returns v0.
        uint32_t callGuest(VcsGuestFn fn, uint32_t address, uint8_t *rdram, VcsGuestContext *ctx, void *runtime,
                           std::initializer_list<uint32_t> args, float f12 = 0.0f)
        {
            uint8_t gprs[32 * 16];
            float fprs[32];
            uint32_t pc;
            uint8_t *bytes = mod::ctxBytes(ctx);
            std::memcpy(gprs, bytes + mod::api->ctx_gpr_offset, sizeof(gprs));
            std::memcpy(fprs, mod::fpr(ctx), sizeof(fprs));
            std::memcpy(&pc, bytes + mod::api->ctx_pc_offset, 4);
            int reg = 4;
            for (uint32_t a : args)
                mod::setGpr32(ctx, reg++, a);
            mod::fpr(ctx)[12] = f12;
            mod::setGpr32(ctx, 31, pc); // back to here (pc is restored below anyway)
            mod::setPc(ctx, address);
            fn(rdram, ctx, runtime);
            const uint32_t v0 = mod::gpr32(ctx, 2);
            std::memcpy(bytes + mod::api->ctx_gpr_offset, gprs, sizeof(gprs));
            std::memcpy(mod::fpr(ctx), fprs, sizeof(fprs));
            std::memcpy(bytes + mod::api->ctx_pc_offset, &pc, 4);
            return v0;
        }

        uint32_t weaponInfo(const uint8_t *ram, int type)
        {
            const uint32_t table = rd32(ram, kWeaponInfos);
            const uint32_t infos = table ? rd32(ram, table + 4) : 0;
            return infos && type >= 0 ? infos + static_cast<uint32_t>(type) * kWeaponInfoSize : 0;
        }

        // ClearPointGunAt's blend-out of the fire anim (flags 4: freed once out, blend delta -4)
        void blendOut(uint8_t *ram, uint32_t assoc)
        {
            if (!assoc || rdf(ram, assoc + kAssocBlendDelta) < 0.0f)
                return;
            wr32(ram, assoc + kAssocFlags, rd32(ram, assoc + kAssocFlags) | 4u);
            wrf(ram, assoc + kAssocBlendDelta, -4.0f);
        }
    }

    void animate(uint8_t *ram, VcsGuestContext *ctx, void *runtime)
    {
        if (!g_blendAnimation || !g_getAssociation || !g_pointGunAt)
            return;
        const uint32_t frameNow = rd32(ram, kFrameCounter);
        const AimWant w = g_aimWant;
        const uint32_t ped = w.ped;
        const bool fresh = ped && w.frame == frameNow;
        const int type = fresh ? currentWeapon(ram, ped) : -1;
        const uint32_t info = weaponInfo(ram, type);
        const uint32_t state = fresh ? rd32(ram, ped + kPedState) : 0;
        // free aim's R1 (not a lock-on: the game's own then), standing (not crouched, not in a vehicle), a gun the torso
        // aims (the arm-aimed pistols come up by themselves), not reloading
        bool want = g_aimRaise.load(std::memory_order_relaxed) && fresh && w.aiming && !w.lockedOn && held(type) && info &&
                    !(ram[info & kRamMask] & 0x80) && rd32(ram, ped + kPedVehicle) == 0 &&
                    (rd64(ram, ped + kPedFlags) & kDuckingBits) != kDuckingBits && !reload::reloading(ram, ped);
        const uint32_t clump = ped ? rd32(ram, ped + kPedClump) : 0;
        if (!clump)
            want = false;
        if (want)
        {
            // firing (attack 0x10): the game's own attack drives the same anim; it is held again once back to idle
            if (state == 1u)
            {
                uint32_t assoc = callGuest(g_getAssociation, kGetAssociation, ram, ctx, runtime, {ped, clump, kAnimFire});
                if (!assoc || rdf(ram, assoc + kAssocBlendDelta) < 0.0f)
                {
                    // SetPointGunAt's: the weapon's group, blended in at 4, then its delta 8 and +0x10 cleared
                    assoc = callGuest(g_blendAnimation, kBlendAnimation, ram, ctx, runtime,
                                      {ped, clump, rd32(ram, info + kWeaponInfoGroup), kAnimFire}, 4.0f);
                    if (assoc)
                    {
                        wr32(ram, assoc + 0x10u, 0);
                        wrf(ram, assoc + kAssocBlendDelta, 8.0f);
                        ++g_raiseCount;
                    }
                }
                g_assocDbg = assoc;
                // walking while aiming (aimwalk.h): its flag 0x20 lets the walk / run anims move him under it (without it the
                // held fire anim keeps him standing - VCS's rule for the torso-aimed guns)
                if (assoc && aimwalk::enabled())
                    wr32(ram, assoc + kAssocFlags, rd32(ram, assoc + kAssocFlags) | 0x20u);
                if (assoc)
                    callGuest(g_pointGunAt, kPointGunAt, ram, ctx, runtime, {ped}); // held at the aim point
            }
            g_raised = true;
            g_raisedPed = ped;
            return;
        }
        if (!g_raised)
            return;
        g_raised = false;
        // let go: blended out as ClearPointGunAt does - unless firing (the attack's own) or the ped is gone
        const uint32_t p = rd32(ram, 0x004E4910u);
        if (p && p == g_raisedPed && rd32(ram, p + kPedState) == 1u)
            if (const uint32_t c = rd32(ram, p + kPedClump))
                blendOut(ram, callGuest(g_getAssociation, kGetAssociation, ram, ctx, runtime, {p, c, kAnimFire}));
        g_raisedPed = 0;
    }

    bool raised() { return g_raised; }

    bool aimingFreely(const uint8_t *ram)
    {
        const AimWant w = g_aimWant;
        return w.ped && w.aiming && !w.lockedOn && rd32(ram, kFrameCounter) - w.frame <= 2u;
    }

    bool installAnims()
    {
        g_blendAnimation = mod::api->lookup_function(kBlendAnimation);
        g_getAssociation = mod::api->lookup_function(kGetAssociation);
        g_pointGunAt = mod::api->lookup_function(kPointGunAt);
        if (g_blendAnimation && g_getAssociation && g_pointGunAt)
            return true;
        g_blendAnimation = g_getAssociation = g_pointGunAt = nullptr;
        mod::log("CameraAim: the game's anim functions not found - the aim raise is off\n");
        return false;
    }
}
