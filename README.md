# VCS mods

Mods for the native PC port of **GTA: Vice City Stories** (the PS2 game statically recompiled to C++:
[gta-vcs-recomp](https://github.com/spartaque12/gta-vcs-recomp/tree/vcs-mods)), each built as its own DLL - an `.asi`
the game loads at start-up, as an ASI loader does for the PC GTAs.

No game code or data is in this repository, and no binaries: build the mods from source (below), and bring your own
disc for the game.

## Install

1. You need a `vcs.exe` with the mod API: the port's [`vcs-mods` branch](https://github.com/spartaque12/gta-vcs-recomp/tree/vcs-mods)
   (docs/MODDING.md there). A `vcs.exe` without it ignores `.asi` files.
2. Copy the `.asi` into `build\scripts\` beside `vcs.exe` (create the folder; next to `vcs.exe` works too).
3. Start the game. The log says `[mods] CameraAim loaded (CameraAim.asi, API 1)`; the mod's settings are in the F2
   menu where they always were. Remove the file to play without it (`VCS_MODS=0` skips all mods for one run).

A mod built for another API version is refused with a log line - rebuild it against the `api/vcs_mod_api.h` of your
`vcs.exe`.

## The mods

### CameraAim.asi - first person, free aim, drive-by

The camera and aiming mods of the port, together because they share one camera hook:

- **First person on foot** (UltimateFP's camera): the eyes at the head joint, the head hidden, a short near clip, the
  mouse / right stick look around; standing still or attacking the player faces the view, so shots and R1's target
  search go where you look. Cutscenes and the sniper / rocket views keep the game's camera.
- **First person in vehicles**: the view from the seat in the vehicle's space (a bike's lean too), mouse look eased back
  ahead while driving, L2 / R2 still look aside, no camera shake, the body's jolts eased out - and the vehicle's own
  windows not drawn from inside (its glass meshes get alpha 0 at their draw; nothing in the game's memory is changed).
- **Free aim** (San Andreas' manual aim): with a gun, R1 / the right mouse button no longer locks on; the camera goes
  over the right shoulder (first person keeps its view), a DotCrosshair-style dot marks the middle, the gun is posed
  along the aim and the shots (shotgun pellets too) go along the camera's line through the dot. Melee weapons keep the
  game's lock-on, the sniper rifles and the rocket launcher their sights.
- **Manual drive-by** (SA's Manual Driveby Remake): in a vehicle with an SMG, L2 / R2 bring a camera over the vehicle to
  that side, the mouse aims within the window's arc and Circle fires at the dot.
- **Field of view** of the game's own cameras, on foot and in vehicles (the car camera's widening at speed kept).

Settings (F2, Game page; keys in `vcs.ini`):

| Key | Row | Default |
|---|---|---|
| `camera.first_person` | First person | off (`VCS_FIRSTPERSON=0/1` for one run) |
| `camera.first_person_fov` | First person field of view | 90 (60..110) |
| `camera.first_person_near` | First person near clip | 10 cm (2..30) |
| `camera.first_person_eye_right` / `_ahead` / `_up` | Eye position | 0 cm (-50..50) |
| `camera.first_person_vehicle` | First person in vehicles | on |
| `camera.fov_foot` / `camera.fov_vehicle` | Field of view: on foot / vehicles | 70 (50..110) |
| `camera.free_aim` | Free aim (no lock-on) | off (`VCS_FREEAIM=0/1`) |
| `camera.free_aim_fov` | Aim field of view | 60 (40..80) |
| `controls.firstperson_key` | Keyboard page > Menus > First person | B; on a gamepad Back + R3 |

For tests: `VCS_FIRSTPERSON_HOOKS=0`, `VCS_FREEAIM_HOOKS=0`, `VCS_CARGLASS_HOOKS=0` hook nothing; the control channel
(the port's `tools/rig.py`) gets `firstperson`, `freeaim` and `carglass [0|1]` status commands. How the game's camera,
aim and vehicle drawing work: the port's docs/game-internals.md 12b - 12e, 12o.

### Project2DFX.asi - distant lights, stars, distant traffic

The port's re-implementation of ThirteenAG's Project2DFX for VCS (the PCSX2-fork plugin, MIT):
- **Distant lights**: the city's 4,423 street lamps, signs and traffic signals glow far away at night (LOD coronas).
  Needs the plugin's light table, which is game data and not in this repository: the port's
  `python tools/p2dfx.py import <GTAVCS.PCSX2F.Project2DFX.zip>` writes it to `p2dfx\lights.bin` beside `vcs.exe`.
- **More stars** (500 more, fading with the clouds), **distant cars and boats** (simple models on the game's paths far
  beyond its own traffic, lit at night), **draw distance** and **object detail distance** multipliers, and a **higher
  flight ceiling** (800 m instead of 80 m).

Settings (F2, Graphics page "Draw distance" / "Project2DFX", Game page "Flying"): `p2dfx.draw_distance`,
`p2dfx.lod_distance`, `p2dfx.lights` (`VCS_DISTANT_LIGHTS`), `p2dfx.lights_range`, `p2dfx.lights_limit`,
`p2dfx.lights_size`, `p2dfx.stars` (`VCS_MORE_STARS`), `p2dfx.cars` (`VCS_DISTANT_CARS`), `p2dfx.boats`
(`VCS_DISTANT_BOATS`), `p2dfx.traffic_range`, `p2dfx.cars_max`, `p2dfx.boats_max`, `p2dfx.flight_ceiling`
(`VCS_FLIGHT_CEILING`); all off / 1x by default. Tests: `VCS_P2DFX_HOOKS=0` hooks nothing, `VCS_P2DFX_VERIFY=1` checks
the native projection and sprite writer against the game's, the `p2dfx` command reports counts. Needs a `vcs.exe` whose
mod API has `register_function` (the fork's `vcs-mods` branch from 2026-10-31 on); an older one refuses it with a log
line. "Alpha dual pass" (Graphics) is the renderer's, in `vcs.exe`.

### MeshSmooth.asi - smoother peds (and, experimental, vehicles)

The idea of Zeneric's "Peds And Vehs Mesh Smoothed" for San Andreas, re-implemented for the port (nothing of it used):
every lit model batch the port draws natively is refined on the host - PN triangles, each triangle split into 4 (level
1) or 16 (level 2) - and cached per batch; peds are skinned with interpolated bone weights. Crack-free across the
batches a model is cut into: edge curves depend only on the edge's positions and on normals shared by every user of a
position. Settings (F2, Graphics page, "Mesh smoothing (MeshSmooth)"): `meshsmooth.enabled` (on; `VCS_MESHSMOOTH`),
`meshsmooth.peds` (on), `meshsmooth.vehicles` and `meshsmooth.others` (off: layers the game draws over a car's original
mesh - plate, dirt - show through), `meshsmooth.level` 1-2, `meshsmooth.strength` 0-150 %; key Insert
(`controls.meshsmooth_key`). Control command `meshsmooth [on|off|reset]`: batches refined per kind, cache, build time.
Needs a `vcs.exe` whose mod API has the lit refiner (`set_lit_refiner`); the port's check is `tools/meshsmooth_check.py`.

## Building

Windows, with Visual Studio's clang-cl (the C++ Clang tools) and CMake + Ninja, from a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

`build\plugins\<Name>\<Name>.asi` are the mods. With `-DVCS_RECOMP_DIR=<the port's folder>` the configure step
checks `api/vcs_mod_api.h` against the port's `src/modapi/vcs_mod_api.h`, and `-DVCS_INSTALL_SCRIPTS=ON` copies each
built mod into its `build\scripts`. Linux: the same with clang / clang++ (the mods are `.so` files there).

Why clang: hooks on the game's recompiled functions use its calling convention, clang's `preserve_none`
(`VCS_GUEST_CC`). Why the vendored Dear ImGui (`third_party/imgui`, 1.92.7-docking, unmodified): the crosshair is drawn
into the game's own ImGui context, so the mod must have the same version and layout - it checks the sizes the API
reports and leaves the crosshair out if they differ.

## Layout

| Path | What |
|---|---|
| `api/vcs_mod_api.h` | The mod API (a copy of the port's `src/modapi/vcs_mod_api.h`): what `vcs.exe` gives a mod, what a mod exports |
| `plugins/common/` | `mod.h` / `mod.cpp`: the API glue every mod links (guest registers, settings, commands, log) |
| `plugins/CameraAim/` | CameraAim.asi: `main.cpp` (the exports), `firstperson.cpp`, `freeaim.cpp`, `carglass.cpp` |
| `plugins/Project2DFX/` | Project2DFX.asi: `main.cpp` (the exports), `project2dfx.cpp`, `ps2_float.h` (the EE's float maths, copied from the runtime) |
| `plugins/MeshSmooth/` | MeshSmooth.asi: `main.cpp` (the exports), `meshsmooth.cpp` (the refiner, its cache, settings) |
| `third_party/imgui/` | Dear ImGui 1.92.7-docking (MIT), the sources the port builds |

Writing a mod: the port's docs/MODDING.md - the exports (`VcsModApiVersion`, `VcsModRegister`, `VcsModInstall`,
`VcsModInstallLate`), the two hook phases, settings, hotkeys, the look handler, drawing. A new mod is a folder under
`plugins/` with a `vcs_add_mod(Name ...)` CMakeLists and an `add_subdirectory` line in the top-level CMakeLists.

## Next

The Jolt Physics ragdolls and the streamed-file override (the ModLoader-style folder) with the Immersive Weapons Pack
tools are still built into the port; they move here one by one.

## Licence

GPL-3.0, as the port (LICENSE). Dear ImGui is MIT (`third_party/imgui/LICENSE.txt`); Project2DFX's original is MIT
(ThirteenAG/III.VC.SA.IV.Project2DFX).
