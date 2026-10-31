// See reload.h and docs/game-internals.md 12g / 12t.
#include "reload.h"

#include "mod.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace vcs::reload
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u;
        constexpr uint32_t kTimeMs = 0x004CD104u; // CTimer::m_snTimeInMilliseconds
        // the ped's weapon slots (12g): ped + 0x578 + slot x 0x1C - +0 type, +4 state (0 ready, 1 firing, 2 reloading),
        // +8 rounds in the clip, +0xC rounds in all, +0x10 the reload's end time (ms)
        constexpr uint32_t kPedWeapons = 0x578u, kWeaponSize = 0x1Cu, kPedCurrentSlot = 0x789u;
        constexpr uint32_t kSlotState = 0x4u, kSlotClip = 0x8u, kSlotTotal = 0xCu, kSlotEnd = 0x10u;
        constexpr uint32_t kStateReady = 0, kStateReloading = 2;
        constexpr uint32_t kPedState = 0x8C4u;
        // the weapon infos (FUN_00407120): +0x10 reload time (ms), +0x14 clip size
        constexpr uint32_t kWeaponInfos = 0x00712770u, kWeaponInfoSize = 0x70u, kInfoReload = 0x10u, kInfoClip = 0x14u;

        std::atomic<bool> g_enabled{true};
        std::atomic<bool> g_pressed{false};
        std::atomic<uint32_t> g_started{0}, g_refused{0}; // status
        std::atomic<int> g_last{0};                        // last answer: 1 started, -1..-5 why not

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        uint32_t slotOf(const uint8_t *ram, uint32_t ped)
        {
            const int slot = static_cast<int8_t>(ram[(ped + kPedCurrentSlot) & kRamMask]);
            return slot >= 0 && slot < 10 ? ped + kPedWeapons + static_cast<uint32_t>(slot) * kWeaponSize : 0u;
        }

        uint32_t infoOf(const uint8_t *ram, int type)
        {
            const uint32_t table = rd32(ram, kWeaponInfos);
            const uint32_t infos = table ? rd32(ram, table + 4) : 0;
            return infos && type >= 0 && type < 64 ? infos + static_cast<uint32_t>(type) * kWeaponInfoSize : 0;
        }

        // The game's empty-clip reload, started now. 1 started; why not: -1 no gun, -2 not a gun with a clip, -3 the
        // clip full or nothing to spare, -4 busy (firing / reloading), -5 the player busy (a state other than idle,
        // attacking, aiming).
        int start(uint8_t *ram)
        {
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t slot = ped ? slotOf(ram, ped) : 0;
            if (!slot)
                return -1;
            const int type = static_cast<int>(rd32(ram, slot));
            const uint32_t info = infoOf(ram, type);
            // guns only (19..35); the single-shot ones (clip 1: the sniper rifles, the rocket launcher) reload themselves
            const int clipSize = info ? static_cast<int>(rd32(ram, info + kInfoClip)) : 0;
            if (type < 19 || type > 35 || clipSize <= 1)
                return -2;
            const int clip = static_cast<int>(rd32(ram, slot + kSlotClip)), total = static_cast<int>(rd32(ram, slot + kSlotTotal));
            if (clip >= clipSize || total <= clip)
                return -3;
            if (rd32(ram, slot + kSlotState) != kStateReady)
                return -4;
            const uint32_t state = rd32(ram, ped + kPedState);
            if (state != 1 && state != 0x10 && state != 0x11 && state != 0x16)
                return -5;
            wr32(ram, slot + kSlotState, kStateReloading);
            wr32(ram, slot + kSlotEnd, rd32(ram, kTimeMs) + rd32(ram, info + kInfoReload));
            return 1;
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "weapons.manual_reload";
        s.page = "Game";
        s.section = "Manual reload";
        s.label = "Manual reload";
        s.help = "The reload key (Keyboard page, R; on a gamepad Back + Square) reloads the gun in hand when its clip is "
                 "not full, as the game does when it runs empty: the reload animation plays and the clip is refilled "
                 "from the spare rounds.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_MANUAL_RELOAD";
        s.onChange = [](int v) { g_enabled = v != 0; };
        s.read = [] { return g_enabled ? 1 : 0; };
        add(s);

        VcsModHotkey key = {};
        key.size = sizeof(key);
        key.key = "controls.reload_key";
        key.label = "Reload";
        key.help = "Reloads the gun in hand (Game page, Manual reload). On a gamepad: Back + Square together.";
        key.default_key = 'R';
        key.pad_chord = VCS_MOD_PAD_WEST;
        key.pressed = [](void *) {
            if (g_enabled.load(std::memory_order_relaxed))
                g_pressed = true;
        };
        mod::api->add_hotkey(&key);

        // reload [press] -> ok enabled= started= refused= last= (tests)
        mod::addCommand("reload", [](const std::string &args, mod::Reply reply) {
            if (args.find("press") != std::string::npos)
                g_pressed = true;
            char text[128];
            std::snprintf(text, sizeof(text), "ok enabled=%d started=%u refused=%u last=%d", g_enabled ? 1 : 0,
                          g_started.load(), g_refused.load(), g_last.load());
            reply(text);
        });
    }

    void update(uint8_t *ram)
    {
        if (!g_pressed.exchange(false))
            return;
        const int r = start(ram);
        g_last = r;
        (r > 0 ? g_started : g_refused).fetch_add(1);
        mod::log("[reload] %s (%d)\n", r > 0 ? "started" : "not now", r);
    }

    bool reloading(const uint8_t *ram, uint32_t ped)
    {
        const uint32_t slot = ped ? slotOf(ram, ped) : 0;
        return slot && rd32(ram, slot + kSlotState) == kStateReloading;
    }
}
