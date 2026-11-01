// See renderhook.h: the game's blob shadows under peds, cars and bikes, hidden while RenderHook's ambient occlusion or sun
// shadows darken the ground there already (the two together gave a dark smudge under every ped). The game stores every
// shadow decal of a frame with CShadows::StoreShadowToBeRendered (0x38A000: a1 the texture, a 48-slot table at 0x6FB980);
// a hook there drops those whose texture is the ped, car or bike one (CShadows::Init keeps them at 0x48916C, 0x489168,
// 0x489174) and passes everything else on: helicopter and RC shadows, the explosions' light, poles' and trees' shadows.
// The port's docs/game-internals.md 6o.
#include "renderhook.h"

#include "mod.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::renderhook
{
    namespace
    {
        constexpr uint32_t kStoreShadow = 0x0038A000u;                              // CShadows::StoreShadowToBeRendered
        constexpr uint32_t kTexCar = 0x00489168u, kTexPed = 0x0048916Cu, kTexBike = 0x00489174u; // gpShadow*Tex

        std::atomic<int> g_mode{0}; // renderhook.blob_shadows: 0 auto (hidden with AO or sun shadows), 1 shown, 2 hidden
        std::atomic<uint64_t> g_dropped{0}, g_passed{0};
        VcsGuestFn g_orig = nullptr;

        bool hidden()
        {
            const int mode = g_mode.load(std::memory_order_relaxed);
            return mode == 2 || (mode == 0 && (aoOn() || shadowsOn()));
        }

        uint32_t ramWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }

        // replaces the function when the shadow is dropped (back to $ra), else runs the original before anything of ours
        void VCS_GUEST_CC storeShadow(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (hidden())
            {
                const uint32_t tex = mod::gpr32(ctx, 5);
                if (tex && (tex == ramWord(rdram, kTexPed) || tex == ramWord(rdram, kTexCar) || tex == ramWord(rdram, kTexBike)))
                {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    mod::setPc(ctx, mod::gpr32(ctx, 31));
                    return;
                }
            }
            if (g_passed.fetch_add(1, std::memory_order_relaxed) < 8u && std::getenv("VCS_RH_BLOB_LOG"))
                mod::log("[renderhook] StoreShadowToBeRendered from 0x%08x tex 0x%08x (ped 0x%08x car 0x%08x)\n", mod::gpr32(ctx, 31),
                         mod::gpr32(ctx, 5), ramWord(rdram, kTexPed), ramWord(rdram, kTexCar));
            g_orig(rdram, ctx, runtime);
        }
    }

    void installBlobs()
    {
        if (const char *e = std::getenv("VCS_RH_BLOB_HOOKS"); e && e[0] == '0')
            return;
        g_orig = mod::api->lookup_function(kStoreShadow);
        if (!g_orig || !mod::api->replace_function(kStoreShadow, &storeShadow))
        {
            g_orig = nullptr;
            mod::log("[renderhook] CShadows::StoreShadowToBeRendered not found: blob shadows stay\n");
            return;
        }
        mod::log("[renderhook] blob shadows: CShadows::StoreShadowToBeRendered hooked\n");
        // rhblobs -> mode, hidden now, shadows dropped / passed on since the start
        mod::addCommand("rhblobs", [](const std::string &, mod::Reply reply) {
            reply("ok mode " + std::to_string(g_mode.load()) + " hidden " + std::to_string(hidden() ? 1 : 0) + " dropped " +
                  std::to_string(g_dropped.load()) + " passed " + std::to_string(g_passed.load()));
        });
    }

    void registerBlobSettings()
    {
        mod::Spec s;
        s.key = "renderhook.blob_shadows";
        s.page = "Graphics";
        s.section = "Lighting (RenderHook)";
        s.label = "Blob shadows of peds and cars";
        s.help = "The game's round shadows under people, cars and bikes. Auto hides them while ambient occlusion or sun "
                 "shadows are on (they darken the ground there already).";
        s.type = mod::Type::Choice;
        s.choices = {{"Auto", 0}, {"Shown (original)", 1}, {"Hidden", 2}};
        s.defaultValue = 0;
        s.env = "VCS_RH_BLOB_SHADOWS";
        s.onChange = [](int value) { g_mode = value < 0 || value > 2 ? 0 : value; };
        mod::add(s);
    }
}
