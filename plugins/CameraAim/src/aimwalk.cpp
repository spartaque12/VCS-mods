// See aimwalk.h and docs/game-internals.md 12w.
#include "aimwalk.h"

#include "mod.h"
#include "viewmodel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace vcs::aimwalk
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u;
        constexpr uint32_t kPedState = 0x8C4u, kStateSniper = 0xCu; // PED_SNIPER_MODE: the sniper / rocket views
        constexpr uint32_t kPedWeapons = 0x578u, kWeaponSize = 0x1Cu, kPedCurrentSlot = 0x789u;
        // CCam::Using3rdPersonMouseCam (FUN_002A2E30, a0 the CCam): needs 0x488EB4, never set on the PS2. Answered true
        // at CPlayerPed::ProcessControl's choice of control (ra 0x23141C: VC PC's mouse-aim "1st person run-around",
        // FUN_002339D0 - the body to the camera's heading, the stick strafing) and at the move-anim choice (ra 0x1F21F4
        // in FUN_001F1D38: the walk / run left, right, back anims)
        constexpr uint32_t kUsing3rdPersonMouseCam = 0x002A2E30u, kFromPlayerControl = 0x0023141Cu, kFromMoveAnim = 0x001F21F4u;
        // In the sniper / rocket views (state 0xC) ProcessControl's state switch runs FUN_00233FF0 (sniper rifles) or
        // FUN_00233C80 (the rocket launcher): the scope's look and fire, standing. After them the run-around (FUN_002339D0,
        // a0 ped, a1 pad) is called too, so the stick moves him (strafing) under the scope.
        constexpr uint32_t kSniperControl = 0x00233FF0u, kRocketControl = 0x00233C80u, kRunAround = 0x002339D0u;

        std::atomic<bool> g_on{true};
        VcsGuestFn g_origMouseCam = nullptr, g_origSniper = nullptr, g_origRocket = nullptr, g_runAround = nullptr;
        std::atomic<uint32_t> g_strafe{0}, g_scope{0};

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        int currentWeapon(const uint8_t *ram, uint32_t ped)
        {
            const int slot = static_cast<int8_t>(ram[(ped + kPedCurrentSlot) & kRamMask]);
            return slot >= 0 && slot < 10 ? static_cast<int>(rd32(ram, ped + kPedWeapons + slot * kWeaponSize)) : -1;
        }
        // the player aims a gun: free aim's R1, or the sniper / rocket views
        bool scoped(const uint8_t *ram, uint32_t ped)
        {
            const int type = currentWeapon(ram, ped);
            return rd32(ram, ped + kPedState) == kStateSniper && type >= 30 && type <= 32;
        }
        bool aiming(const uint8_t *ram)
        {
            const uint32_t ped = rd32(ram, kPlayerPed);
            return g_on.load(std::memory_order_relaxed) && ped && (viewmodel::aimingFreely(ram) || scoped(ram, ped));
        }

        VCS_GUEST_CC void mouseCam(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ra = mod::gpr32(ctx, 31);
            if ((ra == kFromPlayerControl || ra == kFromMoveAnim) && aiming(rdram))
            {
                ++g_strafe;
                mod::setGpr32(ctx, 2, 1);
                mod::setPc(ctx, ra);
                return;
            }
            return g_origMouseCam(rdram, ctx, runtime);
        }

        // The run-around's stick (CPad::GetPedWalkLeftRight FUN_00285690 / UpDown FUN_002857E8, -128..128) while it runs for
        // the aim: from the pad as the port sends it, without the game's averaging. Pure left / right (up-down 0) made it
        // run ahead - its anim choice has no 90 degree case - so up-down gets a nudge of 2 (under a degree off). Aiming
        // walks: the stick is held to the Walk key's speed (keys.walk_speed) unless sprint (Cross / Shift) is held.
        constexpr uint32_t kWalkLeftRight = 0x00285690u, kWalkUpDown = 0x002857E8u;
        constexpr uint16_t kCross = 0x4000;
        VcsGuestFn g_origLeftRight = nullptr, g_origUpDown = nullptr, g_origRunAroundHook = nullptr;
        bool g_inRunAround = false;
        uint32_t g_runAroundFrame = 0; // the frame the run-around last ran for the aim

        void aimStick(float &x, float &y)
        {
            const mod::PadState pad = mod::lastPad();
            x = pad.lx;
            y = pad.ly;
            const float mag = std::sqrt(x * x + y * y);
            if (mag < 0.1f)
            {
                x = y = 0.0f;
                return;
            }
            if (!(pad.buttons & kCross))
            {
                const int pct = mod::api->setting_value ? mod::api->setting_value("keys.walk_speed") : 45;
                const float walk = std::clamp(pct > 0 ? pct / 100.0f : 0.45f, 0.2f, 1.0f);
                if (mag > walk)
                {
                    x *= walk / mag;
                    y *= walk / mag;
                }
            }
            if (std::fabs(y) * 128.0f < 2.0f)
                y = 2.0f / 128.0f;
        }
        VCS_GUEST_CC void walkLeftRight(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (!g_inRunAround)
                return g_origLeftRight(rdram, ctx, runtime);
            float x, y;
            aimStick(x, y);
            mod::setGpr32(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 128.0f))));
            mod::setPc(ctx, mod::gpr32(ctx, 31));
        }
        VCS_GUEST_CC void walkUpDown(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (!g_inRunAround)
                return g_origUpDown(rdram, ctx, runtime);
            float x, y;
            aimStick(x, y);
            mod::setGpr32(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(std::lround(y * 128.0f))));
            mod::setPc(ctx, mod::gpr32(ctx, 31));
        }
        // a call into the game from a hook's entry (registers and pc put back); v0
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
            mod::setGpr32(ctx, 31, pc);
            mod::setPc(ctx, address);
            fn(rdram, ctx, runtime);
            const uint32_t v0 = mod::gpr32(ctx, 2);
            std::memcpy(bytes + mod::api->ctx_gpr_offset, gprs, sizeof(gprs));
            std::memcpy(mod::fpr(ctx), fprs, sizeof(fprs));
            std::memcpy(bytes + mod::api->ctx_pc_offset, &pc, 4);
            return v0;
        }

        // The legs: the run-around plays only the forward walk / run (its strafe anims never start in VCS), so the game's
        // own walk / run back, left and right (resident; assoc groups 0x35 / 0x36 / 0x37, anim id 2 walk, 4 run - the
        // templates' +0x30 field) are added (CAnimManager AddAnimation FUN_00215928: BlendAnimation FUN_00215868 blends
        // every other anim out - the player fell) and weighted by the stick's direction: each one's blend amount its share
        // over the forward anim's (at 1; the anim system normalises the sum). An id is shared by every group's walk, so
        // the associations are ours to track (checked by their group / id field, +0x30), and blended out (freed when out)
        // once the stick lets go or the aim ends.
        constexpr uint32_t kAddAnimation = 0x00215928u, kPedClump = 0x50u;
        constexpr uint32_t kAssocFlags = 0x08u, kAssocBlend = 0x10u, kAssocDelta = 0x24u, kAssocGroupId = 0x30u;
        constexpr uint32_t kStrafeGroups[3] = {0x35u, 0x36u, 0x37u}; // back, left, right
        constexpr uint32_t kWalkId = 2u, kRunId = 4u;
        VcsGuestFn g_addAnimation = nullptr;
        uint32_t g_assoc[3][2] = {}; // ours: [direction][walk, run]
        std::atomic<uint32_t> g_legs{0};
        bool g_legsOn = false;

        void wrf32(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        float rdf32(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        void legs(uint8_t *rdram, VcsGuestContext *ctx, void *runtime, uint32_t ped, bool want)
        {
            if (!g_addAnimation)
                return;
            const uint32_t clump = rd32(rdram, ped + kPedClump);
            float x = 0.0f, y = 0.0f;
            if (want && clump)
                aimStick(x, y);
            const float mag = std::sqrt(x * x + y * y);
            const bool sprint = (mod::lastPad().buttons & kCross) != 0;
            float w[3] = {0.0f, 0.0f, 0.0f}, ahead = 1.0f;
            if (mag > 0.05f)
            {
                const float r = x / mag, f = -y / mag;
                ahead = std::max(f, 0.0f);
                w[0] = std::max(-f, 0.0f);
                w[1] = std::max(-r, 0.0f);
                w[2] = std::max(r, 0.0f);
            }
            if (!g_legsOn && w[0] + w[1] + w[2] <= 0.0f)
                return;
            bool any = false;
            for (int i = 0; i < 3; ++i)
                for (int run = 0; run < 2; ++run)
                {
                    const uint32_t id = run ? kRunId : kWalkId, tag = kStrafeGroups[i] << 16 | id;
                    uint32_t &assoc = g_assoc[i][run];
                    if (assoc && (rd32(rdram, assoc + kAssocGroupId) & 0xFFFFFFu) != tag)
                        assoc = 0; // freed by the game since
                    const bool mine = w[i] > 0.01f && (run == 1) == sprint;
                    if (mine)
                    {
                        if (!assoc || rdf32(rdram, assoc + kAssocDelta) < 0.0f)
                        {
                            assoc = callGuest(g_addAnimation, kAddAnimation, rdram, ctx, runtime, {ped, clump, kStrafeGroups[i], id});
                            if (assoc && (rd32(rdram, assoc + kAssocGroupId) & 0xFFFFFFu) != tag)
                                assoc = 0;
                            if (assoc)
                                wrf32(rdram, assoc + kAssocBlend, 0.0f);
                        }
                        if (assoc)
                        {
                            // eased toward its share over the forward anim's (at 1): w / ahead, the sum normalised
                            const float want = std::min(w[i] / std::max(ahead, 0.05f), 20.0f);
                            const float now = rdf32(rdram, assoc + kAssocBlend);
                            wrf32(rdram, assoc + kAssocBlend, now + (want - now) * 0.35f);
                            wrf32(rdram, assoc + kAssocDelta, 0.0f);
                            any = true;
                        }
                    }
                    else if (assoc && rdf32(rdram, assoc + kAssocDelta) >= 0.0f)
                    {
                        const uint32_t flags = rd32(rdram, assoc + kAssocFlags) | 4u; // freed once out
                        std::memcpy(rdram + ((assoc + kAssocFlags) & kRamMask), &flags, 4);
                        wrf32(rdram, assoc + kAssocBlend, std::min(rdf32(rdram, assoc + kAssocBlend), 1.0f));
                        wrf32(rdram, assoc + kAssocDelta, -8.0f);
                        assoc = 0;
                    }
                }
            g_legsOn = any;
            if (any)
                ++g_legs;
        }
        // the run-around itself (also from the scopes, below): the stick above while it runs for the aim, the legs after it
        VCS_GUEST_CC void runAroundHook(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4);
            g_inRunAround = ped && ped == rd32(rdram, kPlayerPed) && aiming(rdram);
            if (g_inRunAround)
                g_runAroundFrame = rd32(rdram, 0x004CD124u);
            const bool forAim = g_inRunAround;
            g_origRunAroundHook(rdram, ctx, runtime);
            g_inRunAround = false;
            if (ped && ped == rd32(rdram, kPlayerPed))
                legs(rdram, ctx, runtime, ped, forAim);
        }
        // CPhysical::ApplyMoveSpeed (FUN_00348098, a0 the entity: position += speed x step): the run-around's anims
        // move him ahead for a pure left / right stick (no side step at 90 degrees), so while the aim walks the player's
        // horizontal move speed is turned to the stick's direction in the body's frame (the body faces the aim), its size
        // kept - every direction moves where it is pushed.
        constexpr uint32_t kApplyMoveSpeed = 0x00348098u, kPedRotation = 0x8E0u, kPedMoveSpeedX = 0x140u;
        VcsGuestFn g_origApplyMove = nullptr;
        constexpr float kAimWalk = 1.6f, kAimRun = 4.5f; // m/s
        std::atomic<uint32_t> g_steered{0};
        VCS_GUEST_CC void applyMoveSpeed(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ent = mod::gpr32(ctx, 4);
            if (ent && ent == rd32(rdram, kPlayerPed) && rd32(rdram, 0x004CD124u) - g_runAroundFrame <= 1u && aiming(rdram))
            {
                float x, y;
                aimStick(x, y);
                float v[2];
                std::memcpy(v, rdram + ((ent + kPedMoveSpeedX) & kRamMask), 8);
                const float animSpeed = std::sqrt(v[0] * v[0] + v[1] * v[1]), mag = std::sqrt(x * x + y * y);
                // the side / back anims carry no root motion the game turns into speed: a walk / run's own pace (m/s / 50 per
                // step), by the stick's share of the walk; the forward anim's when larger (its stride)
                const bool sprint = (mod::lastPad().buttons & kCross) != 0;
                (void)animSpeed;
                const float speed = (sprint ? kAimRun : kAimWalk) / 50.0f * std::min(mag / 0.45f, 1.0f);
                if (mag > 0.05f)
                {
                    float h;
                    std::memcpy(&h, rdram + ((ent + kPedRotation) & kRamMask), 4);
                    const float fwd[2] = {-std::sin(h), std::cos(h)}, right[2] = {std::cos(h), std::sin(h)};
                    const float ax = x / mag, ay = -y / mag; // right, ahead
                    const float d[2] = {right[0] * ax + fwd[0] * ay, right[1] * ax + fwd[1] * ay};
                    v[0] = d[0] * speed;
                    v[1] = d[1] * speed;
                    std::memcpy(rdram + ((ent + kPedMoveSpeedX) & kRamMask), v, 8);
                    ++g_steered;
                }
            }
            return g_origApplyMove(rdram, ctx, runtime);
        }
        void scopeControl(VcsGuestFn orig, uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4), pad = mod::gpr32(ctx, 5);
            if (g_on.load(std::memory_order_relaxed) && g_runAround && ped && pad && ped == rd32(rdram, kPlayerPed) &&
                scoped(rdram, ped))
            {
                ++g_scope;
                callGuest(g_runAround, kRunAround, rdram, ctx, runtime, {ped, pad});
            }
            return orig(rdram, ctx, runtime);
        }
        VCS_GUEST_CC void sniperControl(uint8_t *rdram, VcsGuestContext *ctx, void *runtime) { scopeControl(g_origSniper, rdram, ctx, runtime); }
        VCS_GUEST_CC void rocketControl(uint8_t *rdram, VcsGuestContext *ctx, void *runtime) { scopeControl(g_origRocket, rdram, ctx, runtime); }
    }

    // ---- the aim pose kept over the walk (see aimwalk.h) ----------------------------------------------------------
    namespace
    {
        constexpr uint32_t kFirstUpper = 3, kLastUpper = 16; // spine .. right fingers (the legs hang off bone 2)
        constexpr uint32_t kPedMoveSpeed = 0x140u, kPedAimPitch = 0xCACu; // x 50 = m/s; down positive
        constexpr uint32_t kFrameCounter = 0x004CD124u, kTimeStep = 0x004CD110u;
        constexpr float kStill = 0.3f, kEase = 0.15f; // m/s, s
        struct Pose
        {
            float a[3][3], p[3]; // in the ped's frame (x right, y forward, z up; from its position)
        };
        Pose g_pose[kLastUpper + 1];
        bool g_poseValid = false;
        float g_posePitch = 0.0f, g_keep = 0.0f;
        int g_poseType = -1;
        uint32_t g_lastFrame = 0;
        std::atomic<uint32_t> g_kept{0};

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void pedFrame(const uint8_t *ram, uint32_t ped, float f[4][3])
        {
            for (int r = 0; r < 4; ++r)
                for (int i = 0; i < 3; ++i)
                    f[r][i] = rdf(ram, ped + r * 0x10u + 4u * i);
        }
        // v turned about unit axis k by angle t (Rodrigues)
        void turn(float v[3], const float k[3], float t)
        {
            const float c = std::cos(t), s = std::sin(t), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
            const float x[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
            for (int i = 0; i < 3; ++i)
                v[i] = v[i] * c + x[i] * s + k[i] * d * (1.0f - c);
        }
    }

    void applyBones(uint8_t *ram, uint32_t ped, uint32_t bones)
    {
        const uint32_t frame = rd32(ram, kFrameCounter);
        const float dt = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / 50.0f;
        const bool aiming = g_on.load(std::memory_order_relaxed) && viewmodel::aimingFreely(ram) && viewmodel::raised();
        const int type = currentWeapon(ram, ped);
        const bool firstDraw = frame != g_lastFrame; // the pose noted / the ease stepped once a frame, put back every draw
        g_lastFrame = frame;
        if (!aiming || type != g_poseType)
        {
            g_keep = 0.0f;
            if (type != g_poseType)
                g_poseValid = false;
            g_poseType = type;
            if (!aiming)
                return;
        }
        const float vx = rdf(ram, ped + kPedMoveSpeed), vy = rdf(ram, ped + kPedMoveSpeed + 4);
        // moving: the stick pushed (the walk starts lowering the gun before the speed shows) or still under way; the pose
        // is noted only standing quite still with the stick at rest
        const mod::PadState pad = mod::lastPad();
        const bool pushed = std::fabs(pad.lx) > 0.1f || std::fabs(pad.ly) > 0.1f;
        const bool moving = pushed || std::sqrt(vx * vx + vy * vy) * 50.0f > kStill;
        float f[4][3];
        pedFrame(ram, ped, f);
        const float pitch = rdf(ram, ped + kPedAimPitch);
        if (!moving && g_keep <= 0.0f)
        {
            if (!firstDraw)
                return;
            // standing, aimed: the pose noted in the ped's frame (the game's own aim anim and torso IK)
            for (uint32_t b = kFirstUpper; b <= kLastUpper; ++b)
            {
                const uint32_t m = bones + b * 0x40u;
                float d[3];
                for (int i = 0; i < 3; ++i)
                    d[i] = rdf(ram, m + 0x30u + 4u * i) - f[3][i];
                for (int k = 0; k < 3; ++k)
                    g_pose[b].p[k] = d[0] * f[k][0] + d[1] * f[k][1] + d[2] * f[k][2];
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k)
                        g_pose[b].a[r][k] = rdf(ram, m + r * 0x10u) * f[k][0] + rdf(ram, m + r * 0x10u + 4) * f[k][1] +
                                            rdf(ram, m + r * 0x10u + 8) * f[k][2];
            }
            g_posePitch = pitch;
            g_poseValid = true;
            return;
        }
        if (firstDraw)
            g_keep = std::clamp(g_keep + (moving ? 1.0f : -1.0f) * dt / kEase, 0.0f, 1.0f);
        if (!g_poseValid || g_keep <= 0.0f)
            return;
        // moving: the noted pose put back on the walking body - about the spine's joint where the walk has it now, turned
        // by the aim's pitch since (about the ped's right), eased in / out
        const float w = g_keep * g_keep * (3.0f - 2.0f * g_keep);
        const uint32_t spine = bones + kFirstUpper * 0x40u;
        float pivotNow[3], pivotPose[3];
        for (int i = 0; i < 3; ++i)
        {
            pivotNow[i] = rdf(ram, spine + 0x30u + 4u * i);
            pivotPose[i] = f[3][i] + f[0][i] * g_pose[kFirstUpper].p[0] + f[1][i] * g_pose[kFirstUpper].p[1] +
                           f[2][i] * g_pose[kFirstUpper].p[2];
        }
        const float dPitch = -(pitch - g_posePitch); // down positive -> about +right is up
        for (uint32_t b = kFirstUpper; b <= kLastUpper; ++b)
        {
            const uint32_t m = bones + b * 0x40u;
            float pos[3], ax[3][3];
            for (int i = 0; i < 3; ++i)
                pos[i] = f[3][i] + f[0][i] * g_pose[b].p[0] + f[1][i] * g_pose[b].p[1] + f[2][i] * g_pose[b].p[2] - pivotPose[i];
            for (int r = 0; r < 3; ++r)
                for (int i = 0; i < 3; ++i)
                    ax[r][i] = f[0][i] * g_pose[b].a[r][0] + f[1][i] * g_pose[b].a[r][1] + f[2][i] * g_pose[b].a[r][2];
            turn(pos, f[0], dPitch);
            for (auto &r : ax)
                turn(r, f[0], dPitch);
            for (int i = 0; i < 3; ++i)
                wrf(ram, m + 0x30u + 4u * i, rdf(ram, m + 0x30u + 4u * i) * (1.0f - w) + (pivotNow[i] + pos[i]) * w);
            for (int r = 0; r < 3; ++r)
                for (int i = 0; i < 3; ++i)
                    wrf(ram, m + r * 0x10u + 4u * i, rdf(ram, m + r * 0x10u + 4u * i) * (1.0f - w) + ax[r][i] * w);
        }
        ++g_kept;
    }


    bool enabled() { return g_on.load(std::memory_order_relaxed); }

    void tick(uint8_t *ram, VcsGuestContext *ctx, void *runtime)
    {
        // the aim let go (the run-around no longer runs for it): the strafe anims blended out
        const uint32_t ped = rd32(ram, kPlayerPed);
        if (g_legsOn && ped && rd32(ram, 0x004CD124u) - g_runAroundFrame > 1u)
            legs(ram, ctx, runtime, ped, false);
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "camera.aim_walk";
        s.page = "Game";
        s.section = "First person";
        s.label = "Walk while aiming";
        s.help = "Aiming (free aim's right mouse button / R1, the sniper and rocket views): move with every gun - the rifles, "
                 "shotguns, SMGs, heavy guns and scopes VCS keeps standing still too. You face where you aim and strafe "
                 "with the game's own walk / run left, right and back; the aim pose and the dot stay on target, in third "
                 "and first person. Off: the game's rule.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_AIM_WALK";
        s.onChange = [](int v) { g_on = v != 0; };
        add(s);
    }

    void install()
    {
        g_origMouseCam = mod::api->lookup_function(kUsing3rdPersonMouseCam);
        if (!g_origMouseCam)
        {
            mod::log("CameraAim: the mouse camera tests not found - walk while aiming is off\n");
            return;
        }
        mod::api->replace_function(kUsing3rdPersonMouseCam, &mouseCam);
        g_origSniper = mod::api->lookup_function(kSniperControl);
        g_origRocket = mod::api->lookup_function(kRocketControl);
        g_addAnimation = mod::api->lookup_function(kAddAnimation);
        g_origLeftRight = mod::api->lookup_function(kWalkLeftRight);
        g_origUpDown = mod::api->lookup_function(kWalkUpDown);
        g_origRunAroundHook = mod::api->lookup_function(kRunAround);
        if (g_origLeftRight && g_origUpDown && g_origRunAroundHook)
        {
            mod::api->replace_function(kWalkLeftRight, &walkLeftRight);
            mod::api->replace_function(kWalkUpDown, &walkUpDown);
            mod::api->replace_function(kRunAround, &runAroundHook);
        }
        g_runAround = mod::api->lookup_function(kRunAround); // (the hook above, for the scopes' call)
        g_origApplyMove = mod::api->lookup_function(kApplyMoveSpeed);
        if (g_origApplyMove)
            mod::api->replace_function(kApplyMoveSpeed, &applyMoveSpeed);
        if (g_origSniper && g_origRocket && g_runAround)
        {
            mod::api->replace_function(kSniperControl, &sniperControl);
            mod::api->replace_function(kRocketControl, &rocketControl);
        }
        mod::addCommand("aimwalk", [](const std::string &, mod::Reply reply) {
            char text[160];
            std::snprintf(text, sizeof(text), "ok on=%d strafe=%u scope=%u steered=%u legs=%u kept=%u pose=%d", g_on ? 1 : 0, g_strafe.load(), g_scope.load(), g_steered.load(), g_legs.load(),
                          g_kept.load(), g_poseValid ? 1 : 0);
            std::string s = text;
            reply(s);
        });
    }
}