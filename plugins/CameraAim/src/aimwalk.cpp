// See aimwalk.h and docs/game-internals.md 12w.
#include "aimwalk.h"

#include "mod.h"
#include "viewmodel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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
        // the run-around itself (also from the scopes, below): the stick above while it runs for the aim
        VCS_GUEST_CC void runAroundHook(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4);
            g_inRunAround = ped && ped == rd32(rdram, kPlayerPed) && aiming(rdram);
            if (g_inRunAround)
                g_runAroundFrame = rd32(rdram, 0x004CD124u);
            g_origRunAroundHook(rdram, ctx, runtime);
            g_inRunAround = false;
        }
        // CPhysical::ApplyMoveSpeed (FUN_00348098, a0 the entity: position += speed x step): the run-around's anims
        // move him ahead for a pure left / right stick (no side step at 90 degrees), so while the aim walks the player's
        // horizontal move speed is turned to the stick's direction in the body's frame (the body faces the aim), its size
        // kept - every direction moves where it is pushed.
        constexpr uint32_t kApplyMoveSpeed = 0x00348098u, kPedRotation = 0x8E0u, kPedMoveSpeedX = 0x140u;
        VcsGuestFn g_origApplyMove = nullptr;
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
                const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1]), mag = std::sqrt(x * x + y * y);
                if (speed > 1e-4f && mag > 0.05f)
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
        // a call into the game from a hook's entry (registers and pc put back)
        void callGuest(VcsGuestFn fn, uint32_t address, uint8_t *rdram, VcsGuestContext *ctx, void *runtime, uint32_t a0, uint32_t a1)
        {
            uint8_t gprs[32 * 16];
            float fprs[32];
            uint32_t pc;
            uint8_t *bytes = mod::ctxBytes(ctx);
            std::memcpy(gprs, bytes + mod::api->ctx_gpr_offset, sizeof(gprs));
            std::memcpy(fprs, mod::fpr(ctx), sizeof(fprs));
            std::memcpy(&pc, bytes + mod::api->ctx_pc_offset, 4);
            mod::setGpr32(ctx, 4, a0);
            mod::setGpr32(ctx, 5, a1);
            mod::setGpr32(ctx, 31, pc);
            mod::setPc(ctx, address);
            fn(rdram, ctx, runtime);
            std::memcpy(bytes + mod::api->ctx_gpr_offset, gprs, sizeof(gprs));
            std::memcpy(mod::fpr(ctx), fprs, sizeof(fprs));
            std::memcpy(bytes + mod::api->ctx_pc_offset, &pc, 4);
        }

        void scopeControl(VcsGuestFn orig, uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4), pad = mod::gpr32(ctx, 5);
            if (g_on.load(std::memory_order_relaxed) && g_runAround && ped && pad && ped == rd32(rdram, kPlayerPed) &&
                scoped(rdram, ped))
            {
                ++g_scope;
                callGuest(g_runAround, kRunAround, rdram, ctx, runtime, ped, pad);
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
            std::snprintf(text, sizeof(text), "ok on=%d strafe=%u scope=%u steered=%u kept=%u pose=%d", g_on ? 1 : 0, g_strafe.load(), g_scope.load(), g_steered.load(),
                          g_kept.load(), g_poseValid ? 1 : 0);
            reply(text);
        });
    }
}