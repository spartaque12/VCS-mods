// Walking while aiming (the user's request, 2026-11-01; game-internals.md 12w). Game > Walk while aiming
// (camera.aim_walk, on; VCS_AIM_WALK).
//
// - Free aim with a gun: CCam::Using3rdPersonMouseCam (FUN_002A2E30; needs 0x488EB4, never set on the PS2) answers true
//   at CPlayerPed::ProcessControl's choice of control and at the move-anim choice, so VC PC's mouse-aim control runs:
//   the body faces the camera's heading and the stick strafes with the game's own walk / run left, right and back.
// - The aim raise's held fire anim kept him standing (its association without flag 0x20): viewmodel.cpp sets the flag
//   while this is on, and the walk / run anims move him under it.
// - Those anims also bend the upper body: the aim pose is noted standing still (bones 3 - 16 in the ped's frame) and put
//   back on the walking body each draw (about the spine joint where the walk has it, turned by the aim's pitch since),
//   eased in / out over 0.15 s - the gun stays on the dot.
// - The sniper / rocket views (state 0xC): after the scope's own control (FUN_00233FF0 / FUN_00233C80) the run-around
//   (FUN_002339D0) runs too, so he walks under the scope.
// - Every direction (2026-11-02): inside the run-around the stick (CPad::GetPedWalkLeftRight / UpDown) comes straight
//   from the pad, held to the Walk key's speed unless sprint (Cross / Shift) is held; its anims have no side step at 90
//   degrees (A / D ran ahead), so CPhysical::ApplyMoveSpeed gets the player's move speed turned to the stick's direction.
#pragma once

#include <cstdint>

namespace vcs::aimwalk
{
    void registerSettings();
    void install(); // the hooks and the "aimwalk" status command

    // Game thread, the player's bones built for the draw (before first person's viewmodel): while moving and aiming, the
    // upper body held in the aim pose noted standing (the walk's legs and hips kept).
    void applyBones(uint8_t *ram, uint32_t ped, uint32_t bones);

    // The setting (camera.aim_walk).
    bool enabled();
}