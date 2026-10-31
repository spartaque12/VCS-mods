// CameraAim.asi: first person (on foot and in vehicles), free aim, manual drive-by, the vehicle's glass hidden from
// inside, the game cameras' field of view - the camera / aim mods of the VCS native port as one mod (README.md).
// This file: the exports vcs.exe calls (api/vcs_mod_api.h) and the glue in mod.h.
#include "mod.h"

#include "carglass.h"
#include "firstperson.h"
#include "freeaim.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdarg>
#include <cstdio>
#include <memory>

namespace mod
{
    const VcsModApi *api = nullptr;

    void log(const char *format, ...)
    {
        char text[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        api->log(text);
    }

    namespace
    {
        // the Specs live as long as the game (their callbacks are called until it ends)
        std::vector<std::unique_ptr<Spec>> g_specs;

        void specChange(void *user, int v)
        {
            if (const Spec *s = static_cast<const Spec *>(user); s->onChange)
                s->onChange(v);
        }
        int specRead(void *user) { return static_cast<const Spec *>(user)->read(); }

        std::vector<std::unique_ptr<CommandHandler>> g_commands;

        void command(void *user, const char *args, char *answer, size_t size)
        {
            const CommandHandler &h = *static_cast<const CommandHandler *>(user);
            h(args ? args : "", [answer, size](const std::string &text) { std::snprintf(answer, size, "%s", text.c_str()); });
        }
    }

    void add(Spec spec)
    {
        g_specs.push_back(std::make_unique<Spec>(std::move(spec)));
        const Spec &s = *g_specs.back();
        VcsModSetting c = {};
        c.size = sizeof(c);
        c.key = s.key.c_str();
        c.page = s.page.c_str();
        c.section = s.section.c_str();
        c.label = s.label.c_str();
        c.help = s.help.c_str();
        c.type = static_cast<int>(s.type);
        c.min = s.min;
        c.max = s.max;
        c.format = s.format.c_str();
        c.default_value = s.defaultValue;
        c.env = s.env.empty() ? nullptr : s.env.c_str();
        c.apply = VCS_APPLY_LIVE;
        c.on_change = &specChange;
        c.read = s.read ? &specRead : nullptr;
        c.user = g_specs.back().get();
        if (!api->add_setting(&c))
            log("[CameraAim] setting %s refused\n", s.key.c_str());
    }

    bool set(const std::string &key, int v) { return api->set_setting(key.c_str(), v) != 0; }

    void addCommand(const std::string &verb, CommandHandler handler)
    {
        g_commands.push_back(std::make_unique<CommandHandler>(std::move(handler)));
        api->add_command(verb.c_str(), &command, g_commands.back().get());
    }

    PadState lastPad()
    {
        VcsModPad p = {};
        api->last_pad(&p);
        return {p.buttons, p.lx, p.ly, p.rx, p.ry};
    }

    bool freecamFlying() { return api->freecam_flying() != 0; }
}

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

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION || a->size < sizeof(VcsModApi))
            return 0;
        mod::api = a;

        // the Game page: "First person", "Field of view", "Free aim" (where they were in vcs.exe)
        vcs::firstperson::registerSettings();
        vcs::freeaim::registerSettings();

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
        vcs::carglass::install();
        return 1;
    }
}
