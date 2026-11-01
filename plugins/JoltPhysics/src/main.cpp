// JoltPhysics.asi: San Andreas' "[SA] Jolt Physics" mod re-implemented for the VCS native port on Jolt Physics 5.6.0 - death
// ragdolls on the world's collision, bike falls, hit reactions (wound clutching, stumbles, knockdowns, catching the fall,
// blind fire from the ground), dismemberment, pickups on the ground (GTA IV), foot IK and jiggle physics (README.md).
// This file: the exports vcs.exe calls (api/vcs_mod_api.h); the API glue is plugins/common/mod.cpp.
#include "mod.h"

#include "footik.h"
#include "jiggle.h"
#include "jolt.h"

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "JoltPhysics"; }

    // For other mods (GetProcAddress on JoltPhysics.asi): 1 while Vic's body is a ragdoll - CameraAim.asi's first person
    // then looks out of the ragdoll's head. Game thread.
    VCS_MOD_EXPORT int JoltPhysicsPlayerRagdoll(void) { return vcs::jolt::playerRagdolled() ? 1 : 0; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, game_dir))
            return 0;
        mod::api = a;

        // the Game page's "Physics" section (where it was in vcs.exe)
        vcs::jolt::registerSettings();
        vcs::footik::registerSettings();
        vcs::jiggle::registerSettings();
        return 1;
    }

    // Before the exe's own hooks and after CameraAim.asi's VcsModInstall (mods install in file name order): the bone
    // hooks wrap first person's; free aim (CameraAim's VcsModInstallLate) wraps the line of sight hook. The jiggle first,
    // so the ragdolls' bone hook runs before it, and foot IK last, so it runs before the ragdolls' (jolt.h, footik.h).
    VCS_MOD_EXPORT int VcsModInstall(const VcsModApi *)
    {
        vcs::jiggle::install();
        vcs::jolt::install();
        vcs::footik::install();
        return 1;
    }

    // Once the exe's own hooks are in: a vcs.exe from before this mod (the ragdolls built in) wrapped ours - this mod
    // steps aside, theirs run.
    VCS_MOD_EXPORT int VcsModInstallLate(const VcsModApi *)
    {
        if (vcs::jolt::builtInFound())
        {
            vcs::g_passive = true;
            mod::log("[JoltPhysics] this vcs.exe has the ragdolls built in: JoltPhysics.asi does nothing (use a vcs.exe "
                     "from 2026-11-01 on, without them)\n");
        }
        return 1;
    }
}
