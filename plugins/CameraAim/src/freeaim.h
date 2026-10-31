// Free aim (SA's manual aim; docs/CONTROLS.md, game-internals.md 12d): with a gun R1 (the right mouse button) no longer
// locks on. The camera goes over the player's right shoulder (first person keeps its own view), a dot crosshair marks the
// middle of the picture, the mouse / right stick aim, the player points his gun there and his shots go along the
// camera's line through the crosshair. Melee weapons keep the game's lock-on (as SA's fists), the sniper rifles and the
// rocket launcher their own sights.
//
// The game's parts (game-internals.md 12d):
// - lock-on: CPlayerPed::FindWeaponLockOnTarget (FUN_002d68c8, ProcessPlayerWeapon on R1) finds nothing for a gun.
// - the pose: CPed::AimGun (FUN_001f1438) points the gun at m_fLookDirection (+0x6B8), pitched by the player's +0xCAC,
//   while bIsAimingGun (+0x1C8 0x100) is set and he has no seek target (+0x450): CPed::SetAimFlag's writes
//   (FUN_001f1298) are made in every camera update of the aim, ClearAimFlag's (FUN_001f13d0) when it ends.
// - the shots: CWeapon::ProcessLineOfSight (FUN_002d0870) from FireInstantHit for the player gets the camera's line:
//   from the point of it nearest the muzzle, the weapon's range along it (as SA's Find3rdPersonCamTargetVector).
// - the camera: firstperson.cpp (its hook; look() turns the aim too).
// - drive-bys (SA's Manual Driveby Remake): the game's own (L2 / R2 + Circle with an SMG: CWeapon::FireFromCar
//   FUN_002c1bb0 -> FireInstantHitFromCar FUN_002cb7c0) - the camera over the vehicle (or first person's) aims and its
//   line carries the shots through the same hook (its line-of-sight call returning to 0x2CC850).
// VCS_FREEAIM=0|1 overrides the setting for one run.
#pragma once

#include <cstdint>

namespace vcs::freeaim
{
    // The Game page's "Free aim" section (on / off, the aim's field of view).
    void registerSettings();

    // VcsModInstallLate: after the ragdolls' hooks (this line-of-sight hook wraps theirs: they see the camera's line).
    void install();

    // Game thread, the camera update: free aim is on and the player (`ped`, on foot) holds R1 with a gun it handles.
    bool wantsAim(const uint8_t *ram, uint32_t ped);

    // The aim camera's field of view (degrees).
    float fov();

    // Whose view the camera update left: the game's, first person's, the aim camera's.
    enum class View
    {
        Game,
        FirstPerson,
        Aim
    };

    // Game thread, every update of the active camera: whether the player aims, whose view it is and, ours, its line
    // (source, front: through the middle of the picture) and its yaw / pitch (radians; yaw from +x, pitch up +). The gun
    // points along yaw / pitch while he aims, his shots follow our view's line, the crosshair shows in it.
    void frame(uint8_t *ram, uint32_t ped, bool aiming, View view, const float source[3], const float front[3], float yaw,
               float pitch);

    // Game thread, the camera update in a vehicle: free aim is on and the player sits in one with a weapon the game's
    // drive-by takes (the SMGs, 24..27). The drive-by itself is the game's: L2 / R2 (its camera looking aside) and Circle.
    bool wantsDriveBy(const uint8_t *ram, uint32_t ped);

    // Game thread, every camera update in a vehicle (instead of frame()): whether he aims a drive-by and our view's line.
    // While he does, his shots follow the line and the crosshair shows; nothing of the ped is posed (the game's drive-by
    // animation holds the gun).
    void frameDriveBy(uint8_t *ram, uint32_t ped, bool aiming, View view, const float source[3], const float front[3]);

    // Main thread, each host frame (the API's draw callback, the exe's Dear ImGui context): the crosshair.
    void drawCrosshair();
}
