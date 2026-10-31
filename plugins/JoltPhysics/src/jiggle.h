// Jiggle physics (Game > Physics > Jiggle physics): after SA's Jolt Physics mod's [JigglePhysics] (docs/game-internals.md
// 12r). VCS's peds have no breast or glute bones (every ped's anim tree is the same 25 nodes, game-internals.md 12h), so
// the jiggle is a spring on the two existing bones whose skinned vertices are those parts: spine1 (bone 3, the chest)
// and the pelvis (bone 1, the hips). The skinned draw takes each bone's world matrix on its own, so offsetting those two
// matrices alone moves only the vertices weighted to them - the chest and the hips bounce while the neck, arms and
// thighs stay. Each bone's spring (frequency, damping, max offset / angle as the SA ini) is driven by that bone's world
// acceleration, per ped, for the nearest few peds within range - female peds only by default (ped +0x550, the ped type:
// 5 = civilian female, VC's PEDTYPE_CIVFEMALE).
//
// The hooks, each before the original:
// - FUN_0024e8d0 (an anim tree's bones built) where it returns to 0x3C3280 in the ped's render (s0 = the ped): its tree.
// - FUN_0020c310 where it returns to 0x200740 in the ped's render (a0 = the ped), between its bones and its skinned
//   draw: the springs stepped and the two bones offset. Installed before jolt::install so that Jolt's hook (a ragdoll's
//   or a hit reaction's pose) runs first and the jiggle rides on the pose that is drawn.
// VCS_JIGGLE=0|1 overrides the setting for one run; VCS_JIGGLE_ALL=1 jiggles every ped (the player too).
#pragma once

namespace vcs::jiggle
{
    // The Game page's "Physics" section: Jiggle physics, Jiggle intensity.
    void registerSettings();

    // VcsModInstall, before jolt::install (see above; main.cpp).
    void install();
}
