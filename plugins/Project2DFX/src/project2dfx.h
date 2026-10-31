// Project2DFX for VCS (the user's port of ThirteenAG's PCSX2-fork plugin GTAVCS.PCSX2F.Project2DFX, MIT;
// docs/game-internals.md 6l): distant lights (LOD coronas for the city's 4,423 lamps and traffic signals), more stars,
// distant traffic (low-polygon cars and boats moving along the game's path graph far beyond its own traffic) and a
// higher flight ceiling. Each is a setting, off by default (Graphics page, "Project2DFX"; the ceiling on the Game page).
//
// The plugin calls game functions in loops (projection, the sprite buffer, render states); here the per-light work is
// host code - CSprite::CalcScreenCoors and the sprite buffer's vertex writer ported bit-exactly - and the game functions
// that must run (RwRenderStateSet, CSprite::FlushSpriteBuffer, Im3D) run as a chain of guest tail calls: each returns to
// a continuation address of ours (0x27C41C, CCoronas::Render + 4: neither an entry nor a resume point), whose host
// function sets up the next call. The chains start where the plugin hooks: after CCoronas::Render in the frame's render
// (lights, traffic) and at the flush after the game's own stars in CClouds::Render (stars).
//
// The light table is game data (taken from the map by Project2DFX): never in this repository. tools/p2dfx.py imports it
// from the plugin the player has into p2dfx/lights.bin next to vcs.exe (VCS_P2DFX_LIGHTS=<file> for another one).
//
// VCS_P2DFX_HOOKS=0: no hooks (the reference). VCS_P2DFX_VERIFY=1: the native projection and sprite writer checked
// against every call of the game's own (corona, star, particle sprites), counts in the log and the "p2dfx" command.
#pragma once

namespace vcs::p2dfx
{
    // The Graphics page's "Project2DFX" section and the Game page's flight ceiling (VcsModRegisterSettings, slot
    // "graphics": where they were when built in).
    void registerSettings();

    // VcsModInstallLate: the hooks and the "p2dfx" control-channel command (status for tests).
    void install();
}
