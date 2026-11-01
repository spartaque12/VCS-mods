// RenderHook.asi: interactive water - a height-field simulation of wakes and ripples around the camera (water_sim.cpp,
// shaders/water_sim.hlsl): boats, swimmers and vehicles in the water, vehicles falling in, bullets and explosions on the
// water disturb it; it is damped, and foam builds up where the water churns and fades. The water shader adds the field
// to its own waves (docs/RENDERHOOK_PLAN.md "Water").
#pragma once

#include "vcs_mod_api.h"

namespace vcs::renderhook
{
    // The field of one frame, in the game's world axes (x east, y north, z up; metres). texture: RGBA16F, linear-sampled
    // and clamped - r, g the slope dh/dx, dh/dy (normal = normalize(-r, -g, 1)), b the height (metres), a the foam
    // (0..1); 0 at its border. A water pixel at view point v (VcsRenderFrame's view space) is d = R^T v from the camera
    // (R = world_to_view) and samples the field at uv = 0.5 + (d.x + cam_x, -(d.y + cam_y)) / size (row 0 = the north
    // edge).
    struct WakeField
    {
        VcsRenderTexture *texture; // NULL: no field this frame (off, not ready, an interior)
        float size;                // metres the texture covers
        float cam_x, cam_y;        // the camera relative to the field's centre (metres)
        float water_z;             // the game's water level under the camera, relative to the camera (metres, a hint)
        int valid;
    };

    void registerWakes();        // VcsModRegister: command `rhwake`, VCS_RH_WAKE_VIEW
    void installWakes();         // VcsModInstall: the game-thread hooks (entities each frame, explosions, bullets)
    void registerWakeSettings(); // the "Water (RenderHook)" row "Wakes and ripples"
    bool wakesOn();
    // Inside a VCS_RENDER_BEFORE_POST hook with no pass open, before the water is drawn: steps the simulation (once a
    // frame however often it is called) and returns the field; NULL when off.
    const WakeField *wakeField(const VcsRenderFrame &f);
}
