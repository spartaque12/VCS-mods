// CameraAim.asi: first person (on foot and in vehicles), free aim, manual drive-by, the vehicle's glass hidden from
// inside, the game cameras' field of view - the camera / aim mods of the VCS native port as one mod (README.md).
// This file: the exports vcs.exe calls (api/vcs_mod_api.h); the API glue is plugins/common/mod.cpp.
#include "mod.h"

#include "carglass.h"
#include "firstperson.h"
#include "freeaim.h"
#include "aimwalk.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace
{
    // Dear ImGui: the exe's context and allocator, when its ImGui is the one this mod was built with
    bool imguiMatches(const VcsModApi *a)
    {
        return a->imgui_version_num == IMGUI_VERSION_NUM && a->imgui_sizeof_io == sizeof(ImGuiIO) &&
               a->imgui_sizeof_style == sizeof(ImGuiStyle) && a->imgui_sizeof_vec2 == sizeof(ImVec2) &&
               a->imgui_sizeof_vec4 == sizeof(ImVec4) && a->imgui_sizeof_drawvert == sizeof(ImDrawVert) &&
               a->imgui_sizeof_drawidx == sizeof(ImDrawIdx) && a->imgui_sizeof_context == sizeof(ImGuiContext);
    }

    void drawFrame(void *)
    {
        auto *ctx = static_cast<ImGuiContext *>(mod::api->imgui_context());
        if (!ctx)
            return;
        ImGui::SetCurrentContext(ctx);
        vcs::freeaim::drawCrosshair();
    }

    int lookActive(void *) { return vcs::firstperson::looking() ? 1 : 0; }
    void look(void *, float yaw, float pitch) { vcs::firstperson::look(yaw, pitch); }
    void toggle(void *) { vcs::firstperson::toggle(); }
}

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "CameraAim"; }

    // For other mods (GetProcAddress on CameraAim.asi): VR.asi's head look (firstperson.h setHeadLook); 1 = the last
    // camera update applied it (first person on foot).
    VCS_MOD_EXPORT void CameraAimSetHeadLook(int on, float yaw, float pitch, float roll)
    {
        vcs::firstperson::setHeadLook(on != 0, yaw, pitch, roll);
    }
    VCS_MOD_EXPORT void CameraAimSetHeadPosition(float right, float ahead, float up)
    {
        vcs::firstperson::setHeadPosition(right, ahead, up);
    }
    VCS_MOD_EXPORT int CameraAimHeadLookApplied(void) { return vcs::firstperson::headLookApplied() ? 1 : 0; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, game_dir))
            return 0;
        mod::api = a;

        // the Game page: "First person", "Field of view", "Free aim" (where they were in vcs.exe)
        vcs::firstperson::registerSettings();
        vcs::freeaim::registerSettings();
    vcs::aimwalk::registerSettings();

        // the mouse / right stick turn our view (game/vcs_input.cpp asks every pad read)
        const VcsModLookHandler lookHandler = {&lookActive, &look, nullptr};
        a->set_look_handler(&lookHandler);

        // first person's key (Keyboard page > Menus, B) and Back + R3
        VcsModHotkey key = {};
        key.size = sizeof(key);
        key.key = "controls.firstperson_key";
        key.label = "First person";
        key.help = "Switches first person on foot on and off (Game page). On a gamepad: press Back + R3 together.";
        key.default_key = 'B';
        key.pad_chord = VCS_MOD_PAD_RIGHT_STICK;
        key.pressed = &toggle;
        a->add_hotkey(&key);

        // the crosshair: the exe's Dear ImGui context, drawn into its background list
        if (imguiMatches(a))
        {
            void *allocFn = nullptr, *freeFn = nullptr, *user = nullptr;
            a->imgui_allocators(&allocFn, &freeFn, &user);
            if (allocFn && freeFn)
                ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
                                             reinterpret_cast<ImGuiMemFreeFunc>(freeFn), user);
            a->add_draw_callback(&drawFrame, nullptr);
        }
        else
            mod::log("[CameraAim] vcs.exe's Dear ImGui (%u) is not this mod's (%d): no crosshair\n", a->imgui_version_num,
                     IMGUI_VERSION_NUM);
        return 1;
    }

    // Before the built-in mods' hooks: the camera, the bones (the ragdolls' bone hooks wrap these), the field of view.
    VCS_MOD_EXPORT int VcsModInstall(const VcsModApi *)
    {
        vcs::firstperson::install();
        return 1;
    }

    // After them: free aim's line of sight wraps the ragdolls' (they see the camera's line), the glass.
    VCS_MOD_EXPORT int VcsModInstallLate(const VcsModApi *)
    {
        vcs::freeaim::install();
    vcs::aimwalk::install();
        vcs::carglass::install();
        return 1;
    }
}
