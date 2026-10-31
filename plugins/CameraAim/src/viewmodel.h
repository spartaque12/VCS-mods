// The first-person viewmodel (UltimateFP SA's look: the player's own arms and gun held in view; game-internals.md 12t).
//
// No extra model: the player's real arms, posed when the bones are built for the draw (firstperson.cpp's hook before
// the skinned mesh is drawn, 12c). In first person on foot with a gun (not the sniper rifles / rocket launcher, not melee):
// - the right hand (bone 15, the gun drawn in its frame) is put at a fixed place of the view per kind of gun (+ the
//   settings' offsets), its x axis (the barrel) on the crosshair 30 m out, its z (the gun's up) the view's up; the
//   gun sways against the view's turns, bobs with walking speed and kicks back / up when the clip's count drops;
// - two-handed guns: the left hand on a fore-grip point per weapon in the gun's frame, turned half a turn about the
//   barrel (wrapped round it from under) - not where VCS's hip anims put it;
// - both shoulders are carried to fixed places under the eyes, outside the picture, and each arm is solved with two-bone
//   IK (upper arm, forearm, the elbow bent down and out): only forearms and hands show. A grip out of reach draws the gun
//   back along its barrel (at most 12 cm).
// The game's aim flag is kept on (CPed::SetAimFlag's writes, as free aim's) so its own pose and the shots' source follow
// the view; the shots are the game's (free aim's line hook still puts them on the camera's line). Eased in and out over
// 0.15 s (states other than idle / attack / fight / aim: jumping, entering a car). VCS_VIEWMODEL=0|1 overrides the setting.
#pragma once

#include <cstdint>

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
}
