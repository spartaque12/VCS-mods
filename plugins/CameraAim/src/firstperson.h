// First person on foot (the user's port of UltimateFP's on-foot camera, docs/CONTROLS.md, game-internals.md 12c): the
// camera at the player's eyes, the mouse / right stick look around, the player's head hidden, a short near plane.
//
// The camera: as vcs.exe's free camera (src/game/vcs_freecam.h), the active CCam's source / front / up / FOV are written in place
// of the height routines CCamera::Process runs after the CCam's update (FUN_002A2ED0), chained after the free camera.
// Only on foot with the game's own follow camera (mode 4) or locked on (11: the view turns with the player, whom the
// game turns to the target), no script camera: every other camera (vehicles, the sniper / rocket views, cutscenes) is
// the game's. Standing still and attacking the player faces the view, so shots and R1's target search go where he looks.
// The head: the player's bone matrices are built while the ped is drawn (FUN_0024e8d0 from FUN_003c3250); before the
// draw goes on (FUN_0020c310 called at 0x200738) the head's two bones are collapsed to their joint.
// In vehicles (modes 18, 22 and the game's own 16; "First person in vehicles"): the eyes from the head joint kept in the
// vehicle's space (a bike's lean matrix), the mouse looking about the vehicle's ahead, eased back while driving, L2 / R2
// still looking aside, the view rolling with it. The game's own cameras' field of view (on foot, vehicles) is scaled
// where CCamera::Process hands it to CDraw::SetFOV (FUN_002653e0), so their own state is left alone.
// Free aim's camera (freeaim.h) is this hook's too: while the player aims in third person, a camera over his
// right shoulder turned by the mouse, eased in from the game's follow camera and back out; in first person the aim keeps
// the eyes' view.
// VCS_FIRSTPERSON=0|1 overrides the setting for one run; VCS_FIRSTPERSON_HOOKS=0 does not hook anything (the reference).
#pragma once

namespace vcs::firstperson
{
    // The Game page's "First person" section (on / off, field of view); the key is on the Keyboard page.
    void registerSettings();

    // VcsModInstall: before the game runs, after vcs.exe's free camera (the camera hook chains to it) and before the
    // ragdolls (their bone hooks wrap these).
    void install();

    // Main thread: the key (vcs.exe's hotkey poll, mod.cpp) turns it on or off.
    void toggle();

    // Game thread (pad reads, through the API's look handler): our on-foot view (first person, free aim's) is the camera this frame - the mouse and the
    // right stick turn it (look), and the game's own camera gets no stick.
    bool looking();

    // Game thread: radians to turn - yaw left (+), pitch up (+).
    void look(float yaw, float pitch);

    // The VR mod's head look (VR.asi through the export CameraAimSetHeadLook, any thread): while on, first person on foot
    // looks along the body's yaw (the mouse / stick) turned by the head's yaw, at the head's pitch (the mouse no longer
    // pitches), rolled by its roll - radians, yaw left +, pitch up +, roll right +.
    void setHeadLook(bool on, float yaw, float pitch, float roll);
    // The last camera update was first person on foot with the head look (any thread).
    bool headLookApplied();
}
