// See freeaim.h and docs/game-internals.md 12d.
#include "freeaim.h"

#include "mod.h"
#include "viewmodel.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace vcs::freeaim
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u;  // CWorld::Players[0].m_pPed
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter

        // the ped (game-internals.md 12d)
        constexpr uint32_t kPedState = 0x8C4u, kPedStateIdle = 1, kPedStateAttack = 0x10, kPedStateFight = 0x11;
        constexpr uint32_t kPedFlags = 0x1C8u;                         // 64 bits
        constexpr uint64_t kAimingGun = 0x100u, kRestoringGun = 0x200u; // bIsAimingGun, bIsRestoringGun
        constexpr uint32_t kPedIkFlags = 0x330u + 0x2Cu;               // m_pedIK.m_flags
        constexpr uint32_t kAimsWithArm = 4u;                          // CPedIK: the arm points the gun, not the torso
        constexpr uint32_t kPedSeekTarget = 0x450u, kPedLookTarget = 0x6B4u, kPedLookDirection = 0x6B8u,
                           kPedLookTimer = 0x6C8u;
        constexpr uint32_t kPedAimPitch = 0xCACu; // the player's: the pitch AimGun gives a gun with no target
        constexpr uint32_t kSeekFromTask = 0x0048A638u; // non-zero: the peds' seek targets come from their tasks
        constexpr uint32_t kPedWeapons = 0x578u, kWeaponSize = 0x1Cu, kPedCurrentSlot = 0x789u; // a slot: +0 the type
        constexpr uint32_t kPedVehicle = 0x480u; // the vehicle it is in (0 on foot)
        constexpr uint32_t kWeaponInfos = 0x00712770u; // FUN_00407120: *(*(this) + 4) + type * 0x70
        constexpr uint32_t kWeaponInfoSize = 0x70u, kInfoRange = 0x8u; // byte +0 bit 7: aims with the arm

        // the hooks
        constexpr uint32_t kFindLockOnTarget = 0x002D68C8u; // CPlayerPed::FindWeaponLockOnTarget(ped a0) -> found
        constexpr uint32_t kLineOfSight = 0x002D0870u;      // CWeapon::ProcessLineOfSight(start a0, end a1, .., shooter t1)
        // its return addresses in CWeapon::FireInstantHit (FUN_002c3e48) for a ped's shot: at its target, along its hand,
        // the special models' spray, straight ahead
        constexpr uint32_t kInstantHitShots[] = {0x002C47A4u, 0x002C4914u, 0x002C4CA0u, 0x002C4F18u};
        // and its re-trace into a vehicle the shot hit (FireInstantHit calls itself at 0x2C52C0 with a3 = that vehicle
        // when an occupant has +0x1D8 bit 29, "can be shot in a vehicle"): from the caller's muzzle (*a2) to the shot's
        // end, only that vehicle's occupants hittable
        constexpr uint32_t kOccupantRetrace = 0x002C41ACu;
        // and the drive-by's: CWeapon::FireInstantHitFromCar (FUN_002cb7c0, from FireFromCar FUN_002c1bb0) - start a0 =
        // its muzzle (its own sp+0x40, from the driver's hand), end a1 (sp+0x30), t1 the vehicle's driver, s2 the
        // vehicle (ignored by the line: CWorld's ignore entity is set to it around the call)
        constexpr uint32_t kFromCarShot = 0x002CC850u;
        // and in FireShotgun's pellet loop (FUN_002c7c28): start a0 (its copy of the muzzle, shared by the pellets and
        // the tracers), end a1; the pellet's index at sp+0x170, f22 the step between pellets and f23 half the spread
        // (radians; pellet i's heading = the middle + f23 - i f22). sp+0xB0 is free on this path (the game's own
        // camera-aimed shotgun, unused on the PS2, keeps its start there).
        constexpr uint32_t kShotgunPellet = 0x002C8278u;
        constexpr uint32_t kPelletIndex = 0x170u, kPelletStart = 0xB0u;

        constexpr uint16_t kR1 = 0x0800;
        constexpr float kPi = 3.14159265f, kTwoPi = 6.2831855f;
        // AimGun's pitch is VC's m_fFPSMoveHeading: down positive (Find3rdPersonQuickAimPitch)
        constexpr float kAimPitchSign = -1.0f;

        // the crosshair: DotCrosshair (SA) with the user's DotCrosshair.ini - pixels at 1080 lines
        constexpr float kDotRadius = 1.4f, kDotOutline = 1.25f;
        constexpr int kDotAlpha = 255, kDotOutlineAlpha = 192;

        VcsGuestFn g_origLockOn = nullptr, g_origLineOfSight = nullptr, g_origBulletImpact = nullptr, g_origAfterShot = nullptr;
        constexpr uint32_t kBulletImpact = 0x002C6460u; // CWeapon::DoBulletImpact(weapon, shooter, victim, source* a3, end* t0, ..)
        // the first call after FireInstantHit's line of sight that takes its start (a0 = start, a1 = end; 0x2C4F44 and
        // the others) - its gun smoke / flash puff at the start: the muzzle goes back from here on
        constexpr uint32_t kAfterShot = 0x002EB720u;
        // a first-person shot moved onto the view's line: its start (FireInstantHit's stack) and the viewmodel's muzzle,
        // given back to DoBulletImpact - the flash and tracer start at the drawn gun, not at the eyes' line
        struct MuzzleShot
        {
            uint32_t start = 0, frame = 0;
            float muzzle[3] = {};
        } g_muzzleShot;
        uint32_t g_muzzleShots = 0;
        std::atomic<bool> g_installed{false};
        std::atomic<bool> g_enabled{false};
        std::atomic<int> g_fov{60};

        // game thread
        struct Ray
        {
            bool valid = false;
            float source[3] = {}, dir[3] = {};
            uint32_t frame = 0; // m_FrameCounter of the camera update that left it
        } g_ray;
        bool g_posed = false;   // the aim flags are ours (set in the last camera update)
        bool g_driveBy = false; // the ray is a drive-by's (status)
        uint32_t g_shots = 0;   // shots sent along the line (status)
        uint32_t g_retraces = 0; // occupant re-traces put on the line (status)

        // for the crosshair (main thread)
        std::atomic<bool> g_crosshair{false};
        std::atomic<int64_t> g_crosshairStamp{0}; // steady_clock ns of the camera update that showed it

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

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

        int currentWeapon(const uint8_t *ram, uint32_t ped)
        {
            const int slot = static_cast<int8_t>(ram[(ped + kPedCurrentSlot) & kRamMask]);
            return slot >= 0 && slot < 10 ? static_cast<int>(rd32(ram, ped + kPedWeapons + slot * kWeaponSize)) : -1;
        }

        // The guns free aim handles: those R1 locks on with (FindWeaponLockOnTarget's 19..35), less the sniper rifles
        // (30, 31) and the rocket launcher (32), which have sights of their own.
        bool aimsFreely(int type) { return type >= 19 && type <= 35 && type != 30 && type != 31 && type != 32; }

        // The weapons the game's drive-by takes: the SMGs (FUN_002163B8 and the player's FUN_0023AC10 test 24..27).
        bool drivesBy(int type) { return type >= 24 && type <= 27; }

        uint32_t weaponInfo(const uint8_t *ram, int type)
        {
            const uint32_t table = rd32(ram, kWeaponInfos);
            const uint32_t infos = table ? rd32(ram, table + 4) : 0;
            return infos && type >= 0 && type < 64 ? infos + static_cast<uint32_t>(type) * kWeaponInfoSize : 0;
        }

        float wrap(float a) { return std::remainder(a, kTwoPi); }

        // CPed::SetAimFlag(heading) (FUN_001f1298): aiming, not restoring; the look direction; no look or seek target
        // (AimGun would point at it); the arm points the gun if the weapon's info says so. And the pitch.
        void pose(uint8_t *ram, uint32_t ped, int type, float yaw, float pitch)
        {
            wr64(ram, ped + kPedFlags, (rd64(ram, ped + kPedFlags) | kAimingGun) & ~kRestoringGun);
            wrf(ram, ped + kPedLookDirection, wrap(yaw - kPi / 2.0f)); // the heading of a ped facing yaw
            wr32(ram, ped + kPedLookTimer, 0);
            wr32(ram, ped + kPedLookTarget, 0); // as SetAimFlag: no reference to give up (ResolveReferences checks)
            if (rd32(ram, kSeekFromTask) == 0)
                wr32(ram, ped + kPedSeekTarget, 0); // what SetAimFlag's SetSeekTarget(nil) leaves
            const uint32_t info = weaponInfo(ram, type);
            const uint32_t ik = rd32(ram, ped + kPedIkFlags);
            wr32(ram, ped + kPedIkFlags, info && (ram[info & kRamMask] & 0x80) ? ik | kAimsWithArm : ik & ~kAimsWithArm);
            wrf(ram, ped + kPedAimPitch, kAimPitchSign * pitch);
        }

        // CPed::ClearAimFlag (FUN_001f13d0): the gun goes back down. Not during an attack (the game's own aim then).
        void unpose(uint8_t *ram, uint32_t ped)
        {
            const uint32_t state = rd32(ram, ped + kPedState);
            if (state == kPedStateAttack || state == kPedStateFight)
                return;
            const uint64_t flags = rd64(ram, ped + kPedFlags);
            if (!(flags & kAimingGun))
                return;
            wr64(ram, ped + kPedFlags, (flags & ~kAimingGun) | kRestoringGun);
            wr32(ram, ped + kPedIkFlags, rd32(ram, ped + kPedIkFlags) & ~kAimsWithArm);
            wr32(ram, ped + kPedLookTimer, 0);
            wrf(ram, ped + kPedAimPitch, 0.0f);
        }

        // R1 (FindWeaponLockOnTarget, from ProcessPlayerWeapon): with a gun free aim handles, nothing to lock on to.
        VCS_GUEST_CC void findLockOn(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4);
            if (g_enabled.load(std::memory_order_relaxed) && ped && ped == rd32(rdram, kPlayerPed) &&
                aimsFreely(currentWeapon(rdram, ped)))
            {
                mod::setGpr32(ctx, 2, 0);
                mod::setPc(ctx, mod::gpr32(ctx, 31));
                return;
            }
            return g_origLockOn(rdram, ctx, runtime);
        }

        // A shot's line of sight: the player's, from FireInstantHit or a shotgun pellet while the view is ours, goes
        // along the view's line instead - from the point of it nearest the muzzle (the start the game gave), the
        // weapon's range on; a pellet keeps its spread about it. FireInstantHit's start and end are its own (its stack):
        // its tracer and impact follow. A pellet's start goes to a free stack slot (the shared muzzle copy stays the
        // tracers' start).
        VCS_GUEST_CC void lineOfSight(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ra = mod::gpr32(ctx, 31);
            const uint32_t shooter = mod::gpr32(ctx, 9);
            const bool instant = std::find(std::begin(kInstantHitShots), std::end(kInstantHitShots), ra) != std::end(kInstantHitShots);
            const uint32_t player = rd32(rdram, kPlayerPed);
            // a drive-by from the player's vehicle (he drives or rides: the shooter is its driver)
            const bool fromCar = ra == kFromCarShot && player && mod::gpr32(ctx, 18) && rd32(rdram, player + kPedVehicle) == mod::gpr32(ctx, 18);
            if (g_ray.valid && rd32(rdram, kFrameCounter) - g_ray.frame <= 2u &&
                (fromCar || (shooter && shooter == player && (instant || ra == kShotgunPellet || ra == kOccupantRetrace))))
            {
                uint32_t start = mod::gpr32(ctx, 4);
                const uint32_t end = mod::gpr32(ctx, 5);
                float s[3], e[3];
                for (int i = 0; i < 3; ++i)
                {
                    s[i] = rdf(rdram, start + 4u * i);
                    e[i] = rdf(rdram, end + 4u * i);
                }
                if (ra == kOccupantRetrace)
                {
                    // the end is the shot's, already on the line: the start (the muzzle) goes onto it too, so the
                    // re-trace follows the same line
                    const float *c = g_ray.source, *d = g_ray.dir;
                    const float along = std::max(0.0f, (s[0] - c[0]) * d[0] + (s[1] - c[1]) * d[1] + (s[2] - c[2]) * d[2]);
                    for (int i = 0; i < 3; ++i)
                        wrf(rdram, start + 4u * i, c[i] + d[i] * along);
                    ++g_retraces;
                    return g_origLineOfSight(rdram, ctx, runtime);
                }
                const uint32_t info = weaponInfo(rdram, static_cast<int>(mod::gpr32(ctx, 8)));
                float range = info ? rdf(rdram, info + kInfoRange) : 0.0f;
                if (!(range > 1.0f && range < 2000.0f))
                    range = std::sqrt((e[0] - s[0]) * (e[0] - s[0]) + (e[1] - s[1]) * (e[1] - s[1]) + (e[2] - s[2]) * (e[2] - s[2]));
                const float *c = g_ray.source;
                float d[3] = {g_ray.dir[0], g_ray.dir[1], g_ray.dir[2]};
                const float along = std::max(0.0f, (s[0] - c[0]) * d[0] + (s[1] - c[1]) * d[1] + (s[2] - c[2]) * d[2]);
                if (ra == kShotgunPellet)
                {
                    // this pellet's heading off the middle (left +), the view's line turned by it about the vertical
                    const uint32_t sp = mod::gpr32(ctx, 29);
                    const float off = mod::fpr(ctx)[23] - static_cast<float>(rd32(rdram, sp + kPelletIndex)) * mod::fpr(ctx)[22];
                    if (std::fabs(off) < 0.5f)
                    {
                        const float co = std::cos(off), so = std::sin(off), x = d[0] * co - d[1] * so;
                        d[1] = d[0] * so + d[1] * co;
                        d[0] = x;
                    }
                    start = sp + kPelletStart;
                    mod::setGpr32(ctx, 4, start);
                }
                for (int i = 0; i < 3; ++i)
                {
                    const float p = c[i] + g_ray.dir[i] * along;
                    wrf(rdram, start + 4u * i, p);
                    wrf(rdram, end + 4u * i, p + d[i] * range);
                }
                ++g_shots;
                // the trace stays on the view's line (the dot's hit); the effects get the drawn muzzle back below
                g_muzzleShot = {};
                if (instant && viewmodel::muzzle(rdram, g_muzzleShot.muzzle))
                {
                    g_muzzleShot.start = start;
                    g_muzzleShot.frame = rd32(rdram, kFrameCounter);
                }
            }
            return g_origLineOfSight(rdram, ctx, runtime);
        }

        // CWeapon::DoBulletImpact from FireInstantHit (0x2C51FC): the source (a3, FireInstantHit's start - on the view's
        // line since lineOfSight) set to the viewmodel's muzzle for its flash, tracer and smoke. Before the original only.
        void muzzleBack(uint8_t *rdram, uint32_t source)
        {
            if (g_muzzleShot.start && source == g_muzzleShot.start && rd32(rdram, kFrameCounter) - g_muzzleShot.frame <= 1u)
            {
                for (int i = 0; i < 3; ++i)
                    wrf(rdram, source + 4u * i, g_muzzleShot.muzzle[i]);
                ++g_muzzleShots;
            }
            g_muzzleShot.start = 0;
        }
        VCS_GUEST_CC void bulletImpact(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            muzzleBack(rdram, mod::gpr32(ctx, 7));
            return g_origBulletImpact(rdram, ctx, runtime);
        }
        VCS_GUEST_CC void afterShot(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_muzzleShot.start && mod::gpr32(ctx, 4) == g_muzzleShot.start)
                muzzleBack(rdram, g_muzzleShot.start);
            return g_origAfterShot(rdram, ctx, runtime);
        }

        // One disc of DotCrosshair: a 32-segment fan, no antialiasing (its soft edge is the next disc out).
        void disc(ImDrawList *dl, ImVec2 c, float r, int red, int green, int blue, int alpha)
        {
            if (r > 0.0f && alpha > 0)
                dl->AddCircleFilled(c, r, IM_COL32(red, green, blue, std::clamp(alpha, 0, 255)), 32);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "camera.free_aim";
        s.page = "Game";
        s.section = "Free aim";
        s.label = "Free aim (no lock-on)";
        s.help = "Aim by hand as in San Andreas: with a gun R1 / the right mouse button no longer locks on - the camera goes "
                 "over the right shoulder (first person keeps its view), a dot marks the middle and the shots go there; the "
                 "mouse aims. Melee weapons still lock on; the sniper rifles and the rocket launcher keep their sights. "
                 "Drive-bys too: in a vehicle with an SMG, L2 / R2 (look left / right) brings the camera over the vehicle "
                 "to that side, the mouse aims and Circle fires at the dot.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_FREEAIM";
        s.onChange = [](int v) { g_enabled = v != 0; };
        s.read = [] { return g_enabled ? 1 : 0; };
        add(s);

        s = {};
        s.key = "camera.free_aim_fov";
        s.page = "Game";
        s.section = "Free aim";
        s.label = "Aim field of view";
        s.help = "How wide the over-the-shoulder aim is (the game's own camera: 70; narrower zooms in).";
        s.type = Type::Integer;
        s.min = 40;
        s.max = 80;
        s.format = "%d deg";
        s.defaultValue = 60;
        s.onChange = [](int v) { g_fov = v; };
        add(s);
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_FREEAIM_HOOKS"); e && e[0] == '0')
            return;
        g_origLockOn = mod::api->lookup_function(kFindLockOnTarget);
        g_origLineOfSight = mod::api->lookup_function(kLineOfSight);
        if (!g_origLockOn || !g_origLineOfSight)
            return;
        mod::api->replace_function(kFindLockOnTarget, &findLockOn);
        mod::api->replace_function(kLineOfSight, &lineOfSight);
        g_origBulletImpact = mod::api->lookup_function(kBulletImpact);
        if (g_origBulletImpact)
            mod::api->replace_function(kBulletImpact, &bulletImpact);
        g_origAfterShot = mod::api->lookup_function(kAfterShot);
        if (g_origAfterShot)
            mod::api->replace_function(kAfterShot, &afterShot);
        g_installed = true;
        // freeaim -> ok enabled= posed= ray= driveby= shots= retraces= crosshair= (tests)
        mod::addCommand("freeaim", [](const std::string &, mod::Reply reply) {
            char text[192];
            std::snprintf(text, sizeof(text), "ok enabled=%d posed=%d ray=%d driveby=%d shots=%u retraces=%u crosshair=%d muzzle=%u",
                          g_enabled ? 1 : 0, g_posed ? 1 : 0, g_ray.valid ? 1 : 0, g_driveBy ? 1 : 0, g_shots, g_retraces,
                          g_crosshair ? 1 : 0, g_muzzleShots);
            reply(text);
        });
    }

    bool wantsAim(const uint8_t *ram, uint32_t ped)
    {
        if (!g_installed.load(std::memory_order_relaxed) || !g_enabled.load(std::memory_order_relaxed) || !ped ||
            !(mod::lastPad().buttons & kR1) || !aimsFreely(currentWeapon(ram, ped)))
            return false;
        const uint32_t state = rd32(ram, ped + kPedState);
        return state == kPedStateIdle || state == kPedStateAttack || state == kPedStateFight;
    }

    float fov() { return static_cast<float>(g_fov.load(std::memory_order_relaxed)); }

    void frame(uint8_t *ram, uint32_t ped, bool aiming, View view, const float source[3], const float front[3], float yaw,
               float pitch)
    {
        if (!g_installed.load(std::memory_order_relaxed))
            return;
        const int type = ped ? currentWeapon(ram, ped) : -1;
        const bool gun = g_enabled.load(std::memory_order_relaxed) && ped && aimsFreely(type);

        // the shots: along our view's line
        g_ray.valid = gun && view != View::Game;
        g_driveBy = false;
        if (g_ray.valid)
        {
            std::memcpy(g_ray.source, source, sizeof(g_ray.source));
            std::memcpy(g_ray.dir, front, sizeof(g_ray.dir));
            g_ray.frame = rd32(ram, kFrameCounter);
        }

        // the gun's pose: pointed along the aim while it lasts, lowered after
        if (gun && aiming)
        {
            pose(ram, ped, type, yaw, pitch);
            g_posed = true;
        }
        else if (g_posed)
        {
            if (ped)
                unpose(ram, ped);
            g_posed = false;
        }

        // the crosshair: only while he aims (R1 / the right mouse button), first person too - hip fire there still goes
        // along the view, without the dot
        g_crosshair = g_ray.valid && aiming;
        g_crosshairStamp = nowNs();
    }

    bool wantsDriveBy(const uint8_t *ram, uint32_t ped)
    {
        return g_installed.load(std::memory_order_relaxed) && g_enabled.load(std::memory_order_relaxed) && ped &&
               rd32(ram, ped + kPedVehicle) != 0 && drivesBy(currentWeapon(ram, ped));
    }

    void frameDriveBy(uint8_t *ram, uint32_t ped, bool aiming, View view, const float source[3], const float front[3])
    {
        if (!g_installed.load(std::memory_order_relaxed))
            return;
        if (g_posed) // an aim on foot's pose left over (into a vehicle aiming): lowered
        {
            if (ped)
                unpose(ram, ped);
            g_posed = false;
        }
        // the shots: along our view's line while he aims (the game's drive-by fires them: L2 / R2 and Circle)
        g_ray.valid = aiming && view != View::Game && source && front && wantsDriveBy(ram, ped);
        if (g_ray.valid)
        {
            std::memcpy(g_ray.source, source, sizeof(g_ray.source));
            std::memcpy(g_ray.dir, front, sizeof(g_ray.dir));
            g_ray.frame = rd32(ram, kFrameCounter);
        }
        g_driveBy = g_ray.valid;
        g_crosshair = g_ray.valid;
        g_crosshairStamp = nowNs();
    }

    void drawCrosshair()
    {
        // shown by a camera update of the last 0.15 s (none while the game is paused or loading)
        if (!g_installed.load(std::memory_order_relaxed) || !g_crosshair.load(std::memory_order_relaxed) ||
            nowNs() - g_crosshairStamp.load(std::memory_order_relaxed) > 150000000)
            return;
        // the picture is centred in the window (presenter::fitRect); DotCrosshair's discs, sized for 1080 lines
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
        const float scale = vp->Size.y / 1080.0f, r = kDotRadius * scale, o = kDotOutline * scale;
        ImDrawList *dl = ImGui::GetBackgroundDrawList(); // under the menus
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        disc(dl, c, r + o + 0.75f, 0, 0, 0, kDotOutlineAlpha * kDotAlpha / 255 / 3);
        disc(dl, c, r + o, 0, 0, 0, kDotOutlineAlpha * kDotAlpha / 255);
        disc(dl, c, r + 0.6f, 255, 255, 255, kDotAlpha / 2);
        disc(dl, c, r, 255, 255, 255, kDotAlpha);
        dl->Flags = flags;
    }
}
