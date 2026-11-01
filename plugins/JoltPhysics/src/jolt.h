// Death ragdolls and bike falls (Game > Physics): milestones 1 and 2 of the port of San Andreas' "[SA] Jolt Physics"
// mod and its first knockdowns (docs/game-internals.md 12f, 12h, 12k, 12l). A ped that dies goes limp - a Jolt Physics
// ragdoll (physics/ragdoll.h) made from the pose and velocity it had - instead of playing its death animation, and
// lands on the world's own collision; a rider thrown off a bike tumbles the same way and gets up where it lands.
//
// The hooks, each before the original (the bone ones chained after first person's, CameraAim.asi's firstperson.h):
// - CPed::SetDie (FUN_0020a3b8): the ped is noted with its velocity (+0x140 x 50 = m/s).
// - The ped's render: FUN_0024e8d0 builds a tree's bones (return address 0x3C3280, s0 = the ped: its tree is noted),
//   then FUN_0020c310 is called between the bones and the skinned draw (return address 0x200740, a0 = the ped): a noted
//   ped gets its ragdoll from the bones just built, a ragdolled ped's 25 bone matrices are written from its ragdoll.
// - FUN_00408c30, called only right after CWorld::Process: the physics step (CTimer::ms_fTimeStep / 50 s); peds that were
//   removed or got up (the player after Wasted) lose their ragdoll; a ped's position follows its body.
// - CPhysical::ApplyMoveSpeed (FUN_00348098, through the vtable): not run for ragdolled peds (the body moves them).
// - CPed::InflictDamage (FUN_001d7940; it calls SetDie): the blow is noted - attacker, weapon, the piece hit. The one that
//   kills pushes the ragdoll: a shot or melee hit from the attacker's side on that part (by weapon, after SA's mod; a
//   shotgun's harder close up), in place of the game's own knock-back; a vehicle gives the legs its speed and the trunk
//   less, so the body folds onto the bonnet (Game > Physics > Hit force scales both).
// - CWeapon::ProcessLineOfSight (FUN_002d0870; every bullet's line, a shotgun's pellets each): a line that meets a
//   ragdolled body, cast against the ragdolls in the physics, pushes it at that point (the game's own collision for a
//   dead ped is not where its ragdoll lies). Free aim's hook on it, installed after, wraps this one.
// - CWorld::TriggerExplosion (FUN_00192760; position, radius f12, power f13): the ragdolls in reach are thrown part by
//   part, the nearest parts fastest; peds the blast kills get the same throw when their ragdoll is made.
// - CBike::KnockOffRider (FUN_003f0a58; bike a0, weapon a1, rider a3): a rider thrown off a bike (Game > Physics > Bike
//   falls) gets a knockdown ragdoll at its next draw in the game's fall state (0x2D), thrown after SA's (throwOff: a
//   rider rammed by a vehicle swept over its bonnet, legs first; a crash over the handlebars; never more than 4.5 m/s
//   up - the game's own throw sends one rigid block 5 m high); the
//   game's get-up is held back while the body moves (+0x6C4 at -1), then the ped is put on the body and let up, the
//   body's last pose eased into the get-up animation (docs/game-internals.md 12l).
// - Hit reactions (Game > Physics > Hit reactions): a ped hit and not killed (InflictDamage's note) gets, at its next
//   draw, a short active ragdoll - hips on its animation, joint motors pulling toward the animated pose -, the blow
//   pushing the part hit; drawn eased back into the animation within 0.9 s. A hit that knocks it down is a knockdown
//   ragdoll as above; a free hand reaches the wound (IK on the drawn pose) and a hard hit stumbles the ped a few
//   steps along the push (docs/game-internals.md 12n).
// - CPickups::DoPickUpEffects (FUN_001df618) / DoMoneyEffects (FUN_001e0e08), from the render (Game > Physics > Pickups
//   on the ground): a gun's or cash's object is a Jolt prop instead - dropped from where the game put it, lying on a
//   side at its real size, no spin or glow; the pickup's position follows it (docs/game-internals.md 12m).
// Vehicles near a ragdoll are moving boxes in the physics (their col model's bounding box; a car's as lower body and
// cabin), so they throw, carry and roll over the bodies. The world near a ragdoll is read from CWorld's sectors each
// step: the buildings' and objects' col models (triangles, boxes, spheres; one cached shape each) as static bodies and
// as kinematic ones moved with the game's; a ragdoll gets a ground of its own only where none of it is streamed in.
// VCS_JOLT=0|1 overrides the setting for one run; VCS_JOLT_HOOKS=0 hooks nothing (the reference); VCS_JOLT_WORLD=0
// leaves the world out (each ragdoll on a ground of its own at its feet); VCS_JOLT_BIKES=0|1 overrides Bike falls,
// VCS_JOLT_PICKUPS=0|1 Pickups on the ground, VCS_JOLT_HITS=0|1 Hit reactions.
#pragma once

namespace vcs
{
    // Every hook of this mod (jolt, footik, jiggle) passes straight to the original: set in VcsModInstallLate when a
    // vcs.exe that still has the ragdolls built in wrapped them (main.cpp) - one set of ragdolls, the exe's.
    inline bool g_passive = false;
}

namespace vcs::jolt
{
    // The Game page's "Physics" section.
    void registerSettings();

    // VcsModInstall (main.cpp): after CameraAim.asi's VcsModInstall - its first person (the bone hooks chain to first
    // person's) - and before its VcsModInstallLate - free aim (its line of sight wraps this one's, so the bullets' lines
    // seen here are the camera's).
    void install();

    // VcsModInstallLate: whether the game's death hook is no longer this mod's - an older vcs.exe's own ragdolls
    // installed theirs around it.
    bool builtInFound();

    // Vic's body is a ragdoll now (knocked down, thrown off a bike, dead), not a hit reaction: game thread
    // (JoltPhysicsPlayerRagdoll, for CameraAim.asi's first person).
    bool playerRagdolled();
}
