// San Andreas' armed holds on VCS's own anims (game-internals.md 12w; the user's request, 2026-11-01: first person holds
// the gun as third person does, with SA's animations).
//
// VCS plays IDLE_armed / WALK_armed / run_armed / WALK_start_armed for a ped with a two-handed gun - the same names as
// SA's ped.ifp. They are resident (the ped anims loaded at boot, not a streamed block the override could replace), so
// this rewrites their keys in place: the upper body's tracks (spine1, neck, head, arms) from SA's stock ped.ifp,
// converted by tools/sa_armed_anims.py to <mods>/anims/sa_armed.bin (the user's own data, never shipped), stretched
// to VCS's durations; the root, pelvis, spine and legs stay VCS's (steps and speed unchanged). The anims have no room
// to grow, so the finest of the file's thinning levels that fits VCS's key space is used. Off (or no file) puts VCS's
// keys back. Third and first person alike (and every ped holding such a gun).
#pragma once

#include <cstdint>

namespace vcs::saanims
{
    void registerSettings(); // Game > First person: "San Andreas armed anims"
    void install();          // the "saanims" status command
    void update(uint8_t *ram); // game thread, every camera update: applied / restored as the setting says
}
