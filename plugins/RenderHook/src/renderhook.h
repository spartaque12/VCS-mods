// RenderHook.asi: RenderHook-style graphics (gtaRenderHook's features for GTA SA, rebuilt in the port's own renderer) through
// the mod API's render hooks (api/vcs_mod_api.h "Rendering"; the port's docs/NATIVE_RENDERER.md "RenderHook-style
// graphics"). First feature: ground-truth ambient occlusion from the game's Z buffer (shaders/gtao.hlsl).
#pragma once

namespace vcs::renderhook
{
    // VcsModRegister: the control-channel command `renderhook` and the debug view's environment switch
    void registerCommands();
    // VcsModRegisterSettings("graphics"): the Graphics page's rows
    void registerSettings();
    // VcsModRegister: temporal AA groundwork (temporal.cpp) - the jitter and the motion-vector debug views
    void registerTemporal();
    // The AO hook added now (dlaa.cpp: before DLAA's own, so DLAA sees the occlusion; it does nothing while AO is off)
    void hookAo();
    bool aoOn(); // ambient occlusion on (its debug view counts)
    // blobs.cpp: the game's blob shadows under peds / cars / bikes hidden with AO or sun shadows (VcsModInstall's hook;
    // the Graphics page's row)
    void installBlobs();
    void registerBlobSettings();
    // dlaa.cpp (dlaa_none.cpp without the DLSS SDK): VcsModRegister's command `rhdlaa`, the Graphics page's DLAA rows
    void registerDlaa();
    // shadows.cpp: sun shadows - VcsModRegister's command `rhshadow`, the Graphics page's rows, the hook (added when first
    // switched on; dlaa.cpp adds it before its own when shadows are on), whether shadows or their debug view are on
    void registerShadows();
    void registerShadowSettings();
    void hookShadows();
    bool shadowsOn();
    void registerDlaaSettings();
}
