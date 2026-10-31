// Project2DFX.asi: distant lights, more stars, distant cars and boats, draw / detail distance and a higher flight
// ceiling - the port's Project2DFX for VCS (ThirteenAG's GTAVCS.PCSX2F.Project2DFX, MIT) as a mod (README.md).
// This file: the exports vcs.exe calls (api/vcs_mod_api.h); the API glue is plugins/common/mod.cpp.
#include "mod.h"

#include "project2dfx.h"

#include <cstring>

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "Project2DFX"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        // its guest tail-call chains return to a continuation of its own: register_function (appended to API 1)
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, has_function))
        {
            if (a && a->log)
                a->log("[p2dfx] this vcs.exe's mod API has no register_function: update vcs.exe\n");
            return 0;
        }
        mod::api = a;
        return 1;
    }

    // The Graphics page's rows after the renderer's, the flight ceiling on the Game page: where they were.
    VCS_MOD_EXPORT int VcsModRegisterSettings(const VcsModApi *, const char *slot)
    {
        if (slot && std::strcmp(slot, "graphics") == 0)
            vcs::p2dfx::registerSettings();
        return 1;
    }

    // After the built-in mods' hooks (nothing else hooks its functions; where it was installed when built in).
    VCS_MOD_EXPORT int VcsModInstallLate(const VcsModApi *)
    {
        vcs::p2dfx::install();
        return 1;
    }
}
