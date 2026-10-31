// Foot IK (Game > Physics > Foot IK), after SA's Jolt Physics mod's [FootIK] (docs/game-internals.md 12q): the feet of
// Vic and of peds within 25 m stand on the ground under them on slopes and stairs. Per leg the ground under the ankle is
// found in the game's own collision (the col triangles of the buildings and objects in CWorld's sectors, 12k, cached per
// entity); the pelvis goes down by the lower foot's need (0.45 m at most), each leg reaches its ground with a two-bone
// IK on thigh / calf (bones 17-19, 21-23, 12h), the foot tilts toward the slope (0.7 of the way), all eased over frames.
// Only peds standing on foot (+0x1C8 bit 0; not dying, dead, down or getting up - the ragdolls' states).
//
// The hooks, each before the original: FUN_0024e8d0 at return address 0x3C3280 (a ped's anim tree, s0 the ped) and
// FUN_0020c310 at 0x200740 (the ped's render between its bones and its skinned draw, a0 the ped): the drawn bones are
// adjusted there. Installed after jolt::install, so the ragdolls' hooks on the same functions run after these (a reaction
// takes the adjusted pose as its target, a ragdoll's pose replaces it).
// VCS_FOOTIK=0|1 overrides the setting for one run.
#pragma once

namespace vcs::footik
{
    // The Game page's Physics rows "Foot IK" and "Foot IK for peds".
    void registerSettings();

    // VcsModInstall, after jolt::install (main.cpp).
    void install();
}
