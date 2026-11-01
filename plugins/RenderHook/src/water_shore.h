// RenderHook.asi: the shore (water stage 2) - foam where the sea meets the sand, shore waves that break and run up the
// beach, caustics on the seabed (water_shore.cpp, shaders/water_shore.hlsl; the port's docs/RENDERHOOK_PLAN.md "Water").
// It draws inside the new water's hook (water.h addPrepare), so it shows while the new water is on.
#pragma once

namespace vcs::renderhook
{
    void registerShore();         // VcsModRegister: command `rhshore`, VCS_RH_SHORE_VIEW, the water's prepare pass
    void registerShoreSettings(); // the "Water (RenderHook)" rows: Shore foam, Shore waves, Caustics
}
