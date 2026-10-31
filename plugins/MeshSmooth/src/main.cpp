// MeshSmooth.asi: smoother peds and vehicles (an adaptation of the idea of Zeneric's "Peds And Vehs Mesh Smoothed" for
// San Andreas; re-implemented, nothing of it is used). This file: the exports vcs.exe calls (api/vcs_mod_api.h).
#include "mod.h"

#include "meshsmooth.h"

#include <cstring>

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "MeshSmooth"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        // the lit refiner was appended to API 1 (2026-10-31): an older vcs.exe has none
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, set_lit_refiner))
        {
            if (a && a->log)
                a->log("[meshsmooth] this vcs.exe's mod API has no lit refiner: update vcs.exe\n");
            return 0;
        }
        mod::api = a;
        vcs::meshsmooth::registerHotkeyAndCommands();
        vcs::meshsmooth::install();
        return 1;
    }

    // The rows on the Graphics page after the renderer's (slot "graphics").
    VCS_MOD_EXPORT int VcsModRegisterSettings(const VcsModApi *, const char *slot)
    {
        if (slot && std::strcmp(slot, "graphics") == 0)
            vcs::meshsmooth::registerSettings();
        return 1;
    }
}
