// RenderHook.asi: new water (water.cpp, shaders/water.hlsl; the port's docs/RENDERHOOK_PLAN.md "Water"). The game draws its
// sea after the world (game-internals.md 6p); the renderer runs VCS_RENDER_BEFORE_WATER / AFTER_WATER around those draws.
// Before: the picture and the Z buffer without the water are copied (refraction, the seabed's depth). After: the water's
// pixels are found (the Z buffer nearer than before), the other stages' passes run, and the surface is drawn over the
// game's water - waves, depth colour, Fresnel reflections (screen space), sun glitter - before peds and vehicles come.
//
// The other stages plug in here: stage 2 (shore waves, foam, caustics) with addPrepare, stage 3 (wakes) through
// water_sim.h's wakeField, which the surface samples.
#pragma once

#include "vcs_mod_api.h"

namespace vcs::renderhook
{
    namespace water
    {
        struct Frame
        {
            const VcsRenderFrame *f;   // the AFTER_WATER frame (camera, lens, depth with the water, world_to_view)
            VcsRenderTexture *under;   // RGBA8 width x height: the picture before the water (the seabed: refraction source)
            VcsRenderTexture *info;    // RGBA16F: x = 1 where the water is the front surface (else 0), y = its view depth (m),
                                       // z = the view depth behind it before the water was drawn (m; 1e4 = sky / nothing), w 0
            VcsRenderTexture *extras;  // RGBA8 width x height, cleared each frame to (0, 0.5, 0.5, 0): r = foam 0..1,
                                       // g / b = extra slope dh/dx, dh/dy (world axes) as 0.5 + slope, a = caustic light 0..1
            int rect[4];               // the water's bounds (target pixels: x0, y0, x1, y1, exclusive ends)
            float camera[3];           // the camera in the game's world coordinates (x east, y north, z up; metres)
            float worldToView[9];      // = f->world_to_view (view = R world)
            float time;                // seconds of game time (waves)
            float sun[3];              // towards the sun, world (the timecycle's ring, game-internals.md 6n)
            float daylight;            // 0 at night .. 1 with the sun 0.2 above the horizon
            float waterColor[4];       // the game's water tint (f->water_color)
        };
        using Pass = void (*)(const Frame &frame);
        // Stage 2: called inside the water hook after `under`, `info` and `extras` are ready, before the surface is drawn,
        // in the order added (VcsModRegister / any time on the main thread). Draw into `extras` (or `under` for light on
        // the seabed); leave no pass open.
        void addPrepare(Pass pass);
        bool on(); // the new water is drawn (setting or debug view)
    }

    void registerWater();         // VcsModRegister: command `rhwater`, VCS_RH_WATER_VIEW
    void registerWaterSettings(); // the Graphics page's "Water (RenderHook)" rows
}
