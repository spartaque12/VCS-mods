// Vegetation.asi: the converted palms and trees' leaves sway in the game's wind (SkyGfx Extended's vegetation wind for
// San Andreas, on the port's native renderer).
//
// The models themselves are data: the port's tools/veg_convert.py turns your copy of BSOR Vegetation (SA) or 80's VC
// True Vegetation into the game's own formats, under mods\stream beside vcs.exe (the port's streamed-file override), and
// writes mods\wind.txt - the PCSX2 names of their leaf textures. This mod hands that list to the renderer
// (render_set_wind_list), whose world shader moves those textures' vertices by a value-noise field, by their distance
// from the top of the trunk (so the bark stays still and the leaves stay on it); every frame it moves the field with the
// game's wind (CWeather::Wind, 0.25 sunny - 2 hurricane) on the game's clock (still while paused) - render_set_wind.
// The port's docs/game-internals.md 12j has the facts, docs/DECISIONS.md why it is made this way.
#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
    constexpr uint32_t kFrameKick = 0x002723E8u; // kicks the frame's display list: once per rendered frame (game thread)
    constexpr uint32_t kTimeMs = 0x004CD104u;    // CTimer::m_snTimeInMilliseconds (stops while paused)
    constexpr uint32_t kWind = 0x004CD2A4u;      // CWeather::Wind: 0.25 sunny, 0.7 cloudy, 1 rain, 2 hurricane

    VcsGuestFn g_origKick = nullptr;
    std::atomic<bool> g_enabled{false};
    std::atomic<int> g_strength{100};
    bool g_listed = false;       // the renderer took the wind list
    uint32_t g_lastMs = 0;
    double g_x = 0.0, g_y = 0.0; // the field's position (metres), kept in a 4 km period
    float g_wind = 0.0f;         // the game's wind, smoothed
    bool g_on = false;

    template <typename T> T rd(const uint8_t *rdram, uint32_t a)
    {
        T v;
        std::memcpy(&v, rdram + (a & 0x1FFFFFFu), sizeof v);
        return v;
    }

    void registerSettings()
    {
        mod::Spec s;
        s.key = "graphics.wind_sway";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Vegetation wind";
        s.help = "The converted palms' and trees' leaves (the port's tools/veg_convert.py) sway in the wind, as with "
                 "SkyGfx Extended's wind for San Andreas; stronger in storms. The PS2's own models stay still.";
        s.type = mod::Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_WIND";
        s.onChange = [](int v) { g_enabled = v != 0; };
        mod::add(s);

        s = {};
        s.key = "graphics.wind_strength";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Vegetation wind strength";
        s.help = "How far the leaves move (100 %: 6 m from the top of the trunk, about 15 cm in a calm, 45 cm in the "
                 "rain, 75 cm in a hurricane; longer fronds more). Trunks stay still.";
        s.type = mod::Type::Integer;
        s.min = 0;
        s.max = 300;
        s.format = "%d %%";
        s.defaultValue = 100;
        s.env = "VCS_WIND_STRENGTH";
        s.onChange = [](int v) { g_strength = v; };
        mod::add(s);
    }

    std::filesystem::path utf8Path(const char *s)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(s)));
    }

    // mods\wind.txt beside vcs.exe (VCS_MODS_DIR: another mods folder; VCS_WIND_LIST: the file itself)
    void handOverList()
    {
        std::filesystem::path path;
        if (const char *e = std::getenv("VCS_WIND_LIST"); e && *e)
            path = utf8Path(e);
        else if (const char *m = std::getenv("VCS_MODS_DIR"); m && *m)
            path = utf8Path(m) / "wind.txt";
        else
            path = utf8Path(mod::api->game_dir()) / "mods" / "wind.txt";
        const auto utf8 = path.u8string();
        const std::string p(utf8.begin(), utf8.end());
        g_listed = mod::api->render_set_wind_list(p.c_str()) != 0;
        mod::log(g_listed ? "[Vegetation] wind list %s\n" : "[Vegetation] no wind list at %s (tools/veg_convert.py --hd "
                                                            "writes it): nothing sways\n", p.c_str());
    }

    void updateWind(const uint8_t *rdram)
    {
        if (!g_enabled.load(std::memory_order_relaxed) || !g_listed)
        {
            if (g_on)
                mod::api->render_set_wind(0.0f, 0.0f, 0.0f);
            g_on = false;
            return;
        }
        const uint32_t ms = rd<uint32_t>(rdram, kTimeMs);
        float wind = rd<float>(rdram, kWind);
        if (!std::isfinite(wind))
            wind = 0.0f;
        wind = std::clamp(wind, 0.0f, 2.0f);
        // the game's clock: still while paused; a load or a jump back starts the field from where it is
        const uint32_t step = ms - g_lastMs;
        g_lastMs = ms;
        const double dt = (g_on && step < 1000u) ? step / 1000.0 : 0.0;
        g_wind = g_on ? g_wind + (wind - g_wind) * float(std::min(dt * 0.5, 1.0)) : wind;
        g_on = true;
        // the field drifts with the wind (a slowly turning direction); its 1 m cells pass at 0.6 - 3 m/s
        const double t = ms / 1000.0, angle = 0.7 + 0.35 * std::sin(t * 0.013);
        const double speed = 0.6 + 1.2 * g_wind;
        g_x = std::fmod(g_x + std::cos(angle) * speed * dt, 4096.0);
        g_y = std::fmod(g_y + std::sin(angle) * speed * dt, 4096.0);
        const float strength = (0.3f + 0.6f * g_wind) * g_strength.load(std::memory_order_relaxed) / 100.0f;
        mod::api->render_set_wind(float(g_x), float(g_y), strength);
    }

    // The frame kick: the wind for this frame, then whatever ran there (the port's frame-boundary work and the game's
    // kick) - work before the original only (the port's AGENTS.md, hooks on recompiled functions)
    VCS_GUEST_CC void frameKick(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
    {
        updateWind(rdram);
        g_origKick(rdram, ctx, runtime);
    }
}

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "Vegetation"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION)
            return 0;
        mod::api = a;
        if (!VCS_MOD_API_HAS(a, render_set_wind_list))
        {
            mod::log("[Vegetation] this vcs.exe has no render_set_wind_list (the fork's vcs-mods branch from 2026-11-02 "
                     "on): not loaded\n");
            return 0;
        }
        handOverList(); // before the renderer starts, which reads it then
        return 1;
    }

    // Graphics page, after the renderer's rows (where Vegetation wind was when built in)
    VCS_MOD_EXPORT int VcsModRegisterSettings(const VcsModApi *, const char *slot)
    {
        if (slot && std::strcmp(slot, "graphics") == 0)
            registerSettings();
        return 1;
    }

    // After the built-in hooks: the port's own frame-kick hook (its per-frame work) is what this one calls
    VCS_MOD_EXPORT int VcsModInstallLate(const VcsModApi *a)
    {
        g_origKick = a->lookup_function(kFrameKick);
        if (!g_origKick || !a->replace_function(kFrameKick, &frameKick))
        {
            mod::log("[Vegetation] frame kick 0x%06X not hooked: the wind stays still\n", kFrameKick);
            return 1;
        }
        return 1;
    }
}
