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

### JoltPhysics.asi - ragdolls, Euphoria-style hit reactions, dismemberment, pickups on the ground, foot IK, jiggle

San Andreas' "[SA] Jolt Physics" mod re-implemented for VCS on Jolt Physics 5.6.0 (jrouwe/JoltPhysics, MIT; nothing of
the SA mod used):
- **Death ragdolls**: a ped that dies goes limp instead of playing its death animation - an 11-body ragdoll from the
  pose and speed it had, pushed by the shot, blow, vehicle or blast that killed it - and lands on the world's own
  collision (the buildings' and objects' col models near it, vehicles as moving boxes): down stairs and slopes, off
  ledges, against walls. Bullets push bodies already down.
- **Bike falls**: a rider thrown off a bike (Vic in a crash, a ped hit by a car or shot off it) tumbles as a ragdoll and
  gets up where the body comes to rest.
- **Hit reactions** (GTA IV / Euphoria style): a ped shot and not killed reacts with its body where the bullet hits - an
  active ragdoll driven toward its animation -, clutches the wound, stumbles a few steps on a hard hit, and a hit that
  knocks it down drops it as a ragdoll that **catches its fall** with its hands and, armed, **fires back blind** from the
  ground, then gets up.
- **Dismemberment**: a lethal shot, blade or blast can take a limb or the head off (the stump bleeds).
- **Pickups on the ground** (after SA's "GTA IV Pickups On The Ground"): weapons and cash lie at their real size where
  they fall (Jolt props), no spinning or glow.
- **Foot IK**: Vic's and nearby peds' feet stand on slopes and stairs; **jiggle physics**: chest and hip springs.

Settings (F2, Game page, "Physics"; keys in `vcs.ini`): `physics.ragdolls` Ragdolls (off; `VCS_JOLT=0/1`),
`physics.ragdoll_force` Hit force (100 %), `physics.ragdoll_bikes` Bike falls, `physics.ragdoll_hits` Hit reactions,
`physics.ragdoll_catch` Catch falls, `physics.ragdoll_blindfire` Fire back from the ground (these four on, with
Ragdolls), `physics.gore` Dismemberment (off), `physics.pickups` Pickups on the ground (off), `physics.footik` /
`physics.footik_npcs` Foot IK, `physics.jiggle` / `physics.jiggle_intensity` Jiggle physics. For tests:
`VCS_JOLT_HOOKS=0` hooks nothing, `VCS_JOLT_WORLD=0` leaves the world out; control commands `jolt`, `footik`, `jiggle`
(status; the port's `tools/ragdoll_check.py`, `ragdoll_world_check.py`, `hit_reactions_check.py`, `footik_check.py` ...).
How it works: the port's docs/game-internals.md 12f - 12s. Install `JoltPhysics.asi` with `CameraAim.asi` if you use
first person / free aim (it loads after it and chains its hooks to theirs).

### RenderHook.asi - RenderHook-style graphics (ambient occlusion, sun shadows, DLAA)

gtaRenderHook's features (the D3D11 renderer for GTA SA) rebuilt on the port's own renderer, through the mod API's
render hooks - the same passes on D3D11 and Vulkan. The plan and what fits: the port's docs/RENDERHOOK_PLAN.md.
- **Ambient occlusion**: ground-truth AO (GTAO, as in Intel's XeGTAO) from the game's own Z buffer, multiplied into the
  3D picture after the whole scene (world, water, peds, vehicles) and before the game's bloom, trails and motion blur;
  corners, creases, the ground under cars and people darken. Faded by the game's fog and out past ~350 m. Costs about
  0.5-0.7 ms of GPU a frame at 1080p with 4x MSAA on an RTX 4070 Ti SUPER (0.05 ms at the original 640x448).

Settings (F2, Graphics page, "Lighting (RenderHook)"): `renderhook.ao` Ambient occlusion: Off (original) / Subtle /
Normal / Strong (`VCS_RH_AO=0..3`), `renderhook.ao_radius` Occlusion radius, 50-300 cm, default 120
(`VCS_RH_AO_RADIUS`). With the option off the mod adds nothing to the renderer. Tests: `VCS_RH_AO_VIEW=1` shows the
occlusion alone (grey); the control command `renderhook` reports frames, the camera and the depth mapping
(`renderhook view 0|1`, `renderhook ao N`). Needs a `vcs.exe` whose mod API has the render hooks (2026-10-31 on); an
older one refuses the mod with a log line.
- **Sun shadows**: cascaded shadow maps of what the game draws, from the sun of the game's own timecycle (it moves with
  the clock; nothing at night), multiplied into the 3D picture like the AO - buildings shade the streets, people and cars
  cast and receive. About 0.2 ms of GPU at 1080p. Objects off screen cast none yet, and leaves cast their solid quads.
  Settings (Graphics, "Lighting (RenderHook)"): `renderhook.shadows` Off / Soft / Normal / Dark (`VCS_RH_SHADOWS`),
  `renderhook.shadow_distance` 50-400 m, `renderhook.shadow_quality` 1024 / 2048 / 4096; `VCS_RH_SHADOW_VIEW=1` or the
  control command `rhshadow view 1` shows them alone. Needs a `vcs.exe` with `render_shadow_map` (2026-11-01 on).
- **DLAA** (NVIDIA DLSS at native resolution): the picture after the AO, the game's depth and its motion vectors (the
  camera's and the moving objects') and the renderer's jitter go to DLSS through NGX in the game's process; smooth, stable
  edges. Needs an RTX card, D3D11 (the default backend), and a build with the DLSS SDK: configure with
  `-DVCS_NGX_DIR=<folder with include\ and lib\ holding nvsdk_ngx_s.lib and nvngx_dlss.dll>` (NVIDIA's SDK,
  github.com/NVIDIA/DLSS; not in this repository) and copy `nvngx_dlss.dll` with `RenderHook.asi`. Settings (Graphics,
  "Anti-aliasing (RenderHook)"): `renderhook.dlaa` Off / On (`VCS_RH_DLAA`), `renderhook.dlaa_preset`; turn the game's
  MSAA off with it. Control command `rhdlaa`.

### VR.asi - the game in an OpenXR headset (M0: mono, head-tracked first person)

An OpenXR session on the game's D3D11 device, modelled on praydog/REFramework's OpenXR runtime code (MIT). In first
person on foot (CameraAim.asi, key B) the view follows the head (yaw, pitch, roll; the stick / mouse still turn the
body) and the 3D picture is shown at the game camera's field of view, where it was rendered, with the HUD on a panel
below; menus, loading screens, cutscenes, third person and vehicles are on a "cinema" screen. Frames are paced by the
headset. Install `VR.asi` **and `openxr_loader.dll`** (built next to it) into `build\scripts`, with a CameraAim.asi
that exports `CameraAimSetHeadLook`; start the headset's OpenXR runtime (SteamVR, Meta Quest Link, Virtual Desktop,
WMR ...), then F2 > Graphics > VR: `vr.enabled` (off; `VCS_VR=0/1`), `vr.view` (head-tracked / cinema), `vr.cinema_distance`
(4 m), `vr.cinema_size` (5 m wide), `vr.hud_distance` (1.5 m); recentre with F7 (`controls.vr_recenter_key`) or
Back + L3. Without a runtime or headset it logs one line (`[VR] ...: VR stays off`) and does nothing. Turn the
window's vertical sync off (it would pace the loop to the monitor too). Control command `vr` (status; `vr recenter`).
F2 > Graphics > VR > "VR status" shows what VR is doing; `build\run\vr.log` (next to vcs.exe: `<exe folder>\run\vr.log`,
new each run) has the loader, the active runtime, the headset, its views and refresh rate, the swapchain formats,
every session state change, every OpenXR error with its call, and a summary every 30 s - the file to send back.
Needs a `vcs.exe` with the present hook (`add_present_hook`). Tests without a headset: `-DVR_TEST_RUNTIME=ON` builds
`xr_test_runtime.dll` (`plugins/VR/test`), selected per run with `VCS_VR_RUNTIME=<path>`; it simulates a head
(`XRTEST_HEAD`, `XRTEST_YAW_SWEEP`) and writes what each eye sees (`XRTEST_CAPTURE=<dir>`); `XRTEST_HZ`,
`XRTEST_FORMATS` (e.g. `bgra_srgb,bgra`), `XRTEST_TYPELESS=1`, `XRTEST_NO_HMD=<s>` vary the device, and a control file
(`XRTEST_CONTROL=<file>`: sleep, wake, unfocus, focus, loss, exit, instance_loss, recenter, hz N) drives session state
changes; every spec violation it sees is logged as `VALIDATION`. The loader is built from
KhronosGroup/OpenXR-SDK release-1.1.65 (FetchContent; `-DFETCHCONTENT_SOURCE_DIR_OPENXR_SDK=<dir>` offline;
`-DVR_BUILD_OPENXR_LOADER=OFF` to bring your own).

## Building

Windows, with Visual Studio's clang-cl (the C++ Clang tools) and CMake + Ninja, from a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

`build\plugins\<Name>\<Name>.asi` are the mods. With `-DVCS_RECOMP_DIR=<the port's folder>` the configure step
checks `api/vcs_mod_api.h` against the port's `src/modapi/vcs_mod_api.h`, and `-DVCS_INSTALL_SCRIPTS=ON` copies each
built mod into its `build\scripts`. Linux: the same with clang / clang++ (the mods are `.so` files there).

Render mods' shaders (RenderHook) are built from HLSL by `vcs_mod_shader()`: DXBC with fxc from the Windows SDK (for
D3D11) and SPIR-V with glslang (`glslangValidator` from the Vulkan SDK, or the glslang package on Linux; for Vulkan).
Either may be missing: the mod then has no shaders for that graphics API and says so in the log.

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
| `plugins/JoltPhysics/` | JoltPhysics.asi: `main.cpp` (the exports), `jolt.cpp` (ragdolls, bike falls, hit reactions, gore, pickups), `footik.cpp`, `jiggle.cpp`, `physics/ragdoll.cpp` (the only code that sees Jolt) |
| `plugins/Project2DFX/` | Project2DFX.asi: `main.cpp` (the exports), `project2dfx.cpp`, `ps2_float.h` (the EE's float maths, copied from the runtime) |
| `plugins/RenderHook/` | RenderHook.asi: `main.cpp` (the exports), `renderhook.cpp` (AO settings and passes), `temporal.cpp` (jitter, motion-vector views), `dlaa.cpp` (DLAA through NGX), `shadows.cpp` (sun shadows), `shaders/` |
| `plugins/MeshSmooth/` | MeshSmooth.asi: `main.cpp` (the exports), `meshsmooth.cpp` (the refiner, its cache, settings) |
| `plugins/VR/` | VR.asi: `main.cpp` (the exports, settings, the present hook: copies, layers, head look), `xr.cpp` (OpenXR: loader, session, spaces, swapchains, frames), `shaders/vr_blit.hlsl`, `test/` (xr_test_runtime) |
| `third_party/imgui/` | Dear ImGui 1.92.7-docking (MIT), the sources the port builds |
| `third_party/JoltPhysics/` | Jolt Physics 5.6.0 (MIT): `Jolt/` and `Build/` of the v5.6.0 tag, unmodified |
| `third_party/openxr/` | The OpenXR headers (KhronosGroup/OpenXR-SDK release-1.1.65, Apache-2.0 OR MIT) |

Writing a mod: the port's docs/MODDING.md - the exports (`VcsModApiVersion`, `VcsModRegister`, `VcsModInstall`,
`VcsModInstallLate`), the two hook phases, settings, hotkeys, the look handler, drawing. A new mod is a folder under
`plugins/` with a `vcs_add_mod(Name ...)` CMakeLists and an `add_subdirectory` line in the top-level CMakeLists.

## Next

The streamed-file override (the ModLoader-style folder) with the Immersive Weapons Pack tools is still built into
the port.

## Licence

GPL-3.0, as the port (LICENSE). Dear ImGui is MIT (`third_party/imgui/LICENSE.txt`), Jolt Physics MIT (`third_party/JoltPhysics/LICENSE`); Project2DFX's original is MIT
(ThirteenAG/III.VC.SA.IV.Project2DFX).
