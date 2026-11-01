// The first-person viewmodel (UltimateFP SA's look: the player's own arms and gun held in view; game-internals.md 12t).
//
// No extra model: the player's real arms, posed when the bones are built for the draw (firstperson.cpp's hook before
// the skinned mesh is drawn, 12c). In first person on foot with a gun (not the sniper rifles / rocket launcher, not melee):
// - the right hand (bone 15, the gun drawn in its frame) is put at a fixed place of the view per kind of gun (+ the
//   settings' offsets), turned so the model's bore (measured per gun: the converted SA models' bores are a few degrees
//   off the hand's x) is parallel to the view - on screen the barrel's line runs into the crosshair's dot at every
//   pitch, yaw and turn - its z (the gun's up) the view's up; the gun sways (its place only) against the view's turns,
//   bobs with walking speed, kicks back / up when the clip's count drops, and sits a little lower until aimed;
// - two-handed guns: the left hand on the handguard as San Andreas holds it (camera.fp_sa_hands; the earlier by-eye
//   fore-grip points without it);
// - both shoulders are carried to fixed places under the eyes, outside the picture, and each arm is solved with two-bone
//   IK (upper arm, forearm, the elbow bent down and out): only forearms and hands show, from the bottom of the picture
//   as in SA's first person. A grip out of reach draws the gun back along its barrel (at most 12 cm).
// The game's aim flag is kept on (CPed::SetAimFlag's writes, as free aim's) so its own pose and the shots' source follow
// the view; the shots are the game's (free aim's line hook still puts them on the camera's line). Eased in and out over
// 0.15 s (states other than idle / attack / fight / aim: jumping, entering a car).
//
// That viewmodel is one of three first-person holds (camera.fp_hold, VCS_FP_HOLD): the viewmodel above (the default again
// since 2026-11-02, the user's choice), "as in third person" (the body's own anim holds the gun - the same pose third
// person shows, nothing moved - and the gun comes up only while aiming, through the game's own aim below), or the game's
// arms untouched.
//
// The aim raise (camera.aim_raise, third and first person): free aim's R1 with a gun the torso aims (not the arm-aimed
// Colt45 / Tec9 / silenced Ingram, which come up by themselves) plays the gun's fire anim held at its aim point - what
// CPed::SetPointGunAt (FUN_001D0198) / PointGunAt (FUN_001D06A0) do when R1 locks on - and blends it out on release,
// as ClearPointGunAt (FUN_001D0820). VCS has no aim stance without a target, so before this free aim kept the rifle
// at the hip (game-internals.md 12d, 12t).
#pragma once

#include <cstdint>

#include "vcs_mod_api.h"

namespace vcs::viewmodel
{
    // Rows in the Game page's "First person" section (called by firstperson::registerSettings).
    void registerSettings();

    // The control-channel command "viewmodel" (tests).
    void install();

    // Game thread, every on-foot update of first person's camera: `first` = the view is first person's this frame (mode
    // 4 or 11), `aiming` = free aim poses the gun itself, the view's line and yaw / pitch (radians, yaw from +x, up +).
    void frame(uint8_t *ram, uint32_t ped, bool first, bool lockedOn, bool aiming, const float source[3],
               const float front[3], float yaw, float pitch);

    // Game thread: first person's camera given back (the pose lowered).
    void off(uint8_t *ram);

    // Game thread, the player's bones built for the draw (before the head is collapsed): the arms moved into view.
    void apply(uint8_t *ram, uint32_t ped, uint32_t bones);

    // Game thread, after frame() in the camera's hook (a guest context at a function's entry): the aim raise's anim
    // calls into the game (see above).
    void animate(uint8_t *ram, VcsGuestContext *ctx, void *runtime);

    // Game thread: free aim's R1 aimed in one of the last two camera updates (the player, not locked on) - aimwalk.cpp.
    bool aimingFreely(const uint8_t *ram);

    // Game thread: the aim raise holds the gun's fire anim (free aim, a torso-aimed gun).
    bool raised();

    // Install (CameraAim's): the game's anim functions the aim raise calls; false leaves it off.
    bool installAnims();
}
