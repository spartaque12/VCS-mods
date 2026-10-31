// RenderHook.asi: RenderHook-style graphics in the port's renderer - ambient occlusion first (README.md). This file: the
// exports vcs.exe calls (api/vcs_mod_api.h); the API glue is plugins/common/mod.cpp.
#include "mod.h"

#include "renderhook.h"

#include <cstring>

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "RenderHook"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        // the render hooks were appended to API 1 (2026-10-31): an older vcs.exe has none
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, render_pop_marker))
        {
            if (a && a->log)
                a->log("[renderhook] this vcs.exe's mod API has no render hooks: update vcs.exe\n");
            return 0;
        }
        mod::api = a;
        vcs::renderhook::registerCommands();
        vcs::renderhook::registerTemporal();
        vcs::renderhook::registerShadows();
        vcs::renderhook::registerDlaa();
        return 1;
    }

    // The rows on the Graphics page after the renderer's (slot "graphics").
    VCS_MOD_EXPORT int VcsModRegisterSettings(const VcsModApi *, const char *slot)
    {
        if (slot && std::strcmp(slot, "graphics") == 0)
        {
            vcs::renderhook::registerSettings();
            vcs::renderhook::registerShadowSettings();
            vcs::renderhook::registerDlaaSettings();
        }
        return 1;
    }
}
