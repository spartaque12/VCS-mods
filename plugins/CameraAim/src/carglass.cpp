// See carglass.h.
//
// How VCS draws a vehicle (2026-10-31, read from the recompiled code; the player's car - model 211 - read with the rig):
// - CRenderer::RenderOneNonRoad (FUN_0035D610), for a vehicle: SetupVehicleVariables (FUN_0036DAA8, the clump at vehicle
//   +0x50), InitAlphaAtomicList (FUN_0036ED70), the driver and passengers, the vehicle's Render (vtable at +0x5C, slot
//   +0x44 -> RslElementGroupRender FUN_002C0270: each element of the clump's list at +0x08, link at element +0x1C, by
//   its callback at +0x24), then RenderAlphaAtomics (FUN_0036EED0, the sorted list at 0x6F4030).
// - The callbacks (CVisibilityPlugins) are set by CVehicleModelInfo::SetAtomicRenderCallbacks (FUN_00394A90) by vehicle
//   type: FUN_003942B0 for cars - "_hi" elements with a material whose alpha is not 255 (HasAlphaMaterialCB,
//   FUN_00394260) or named "windscreen" get RenderVehicleHiDetailAlphaCB (FUN_0036DF88: into the alpha list), the rest
//   RenderVehicleHiDetailCB (FUN_0036DE58); FUN_00394438 / 00394620 / 003947C8 / 00394960 for the other types. Every
//   callback ends in the default element render FUN_0039EC18 (geometry pipeline 0x47: FUN_0039E828).
// - RslElement: +0x04 its frame (name at frame +0xA8), +0x14 geometry (materials at +0x0C, count +0x10), +0x18 clump,
//   +0x2A atomic id. RslMaterial: +0x00 texture, +0x04 colour RGBA (alpha at +0x07), +0x0C MatFX (flags at +0x0C).
// - The glass is meshes, not elements: model 211's windows are a material of colour 0x32C2B666 (alpha 0x32) in
//   "chassis_hi" (rear and side windows), the windscreen and the front doors (their windows). The element render draws
//   mesh by mesh (native data at geometry +0x20: count = +0x10 >> 20, 0x30-byte meshes from +0x40, material index at
//   +0x22) and, before each mesh's texture bind and draw (FUN_002F7D58), hands its material colour to FUN_0039E7F0(rgba),
//   which halves the alpha to the PS2's 0..0x80 and sets the VU colour (FUN_002F5EA0 -> 0x742AE0 + 0x20, dirty bit 8).
//   FUN_0039E7F0 is called from those two renders only: 0x39F89C / 0x39F9C0 (FUN_0039EC18, the element in $s7) and
//   0x39E920 / 0x39EB24 (FUN_0039E828, the element at 0($sp)).
//
// The hook (FUN_0039E7F0, before the original): for a mesh of the hidden vehicle's elements whose material is glass -
// alpha not 255, or any mesh of a "windscreen" element (the game's own test; it takes the damaged windscreen, whose
// material is opaque, too) - the colour goes on with alpha 0. Model draws are alpha-tested (GREATER 8): nothing of the
// mesh reaches the picture. Checked 2026-10-31 by setting the glass materials' alpha to 0 in memory (what the hook
// passes on): the rear window and windscreen gone, the seats, the driver and the street beyond seen through them, the
// body unchanged. Bikes have no glass; boats' and helicopters' glass is the same kind of material (their alpha
// materials - a rotor's too, if it has one - go with it).
#include "carglass.h"

#include "mod.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::carglass
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedVehicle = 0x480u;     // the vehicle it is in (0 on foot)
        constexpr uint32_t kVehicleClump = 0x50u;    // the entity's RslElementGroup
        constexpr uint32_t kElementFrame = 0x04u, kElementClump = 0x18u; // RslElement
        constexpr uint32_t kFrameName = 0xA8u;       // RslNode: its name (GetFrameNodeName, FUN_00329C48)

        constexpr uint32_t kMaterialColour = 0x0039E7F0u;           // FUN_0039E7F0(rgba): a mesh's colour -> the VU
        constexpr uint32_t kRenderA = 0x0039F8A4u, kRenderB = 0x0039F9C8u; // its return addresses in FUN_0039EC18 ($s7)
        constexpr uint32_t kRender47A = 0x0039E928u, kRender47B = 0x0039EB2Cu; // in FUN_0039E828 (element at 0($sp))
        constexpr int kS7 = 23, kSp = 29, kA0 = 4, kRa = 31;

        VcsGuestFn g_origColour = nullptr;
        std::atomic<uint32_t> g_hidden{0};  // hide()'s vehicle
        std::atomic<bool> g_forced{false};  // control channel "carglass 1": the player's vehicle (tests)

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        // The vehicle whose glass is hidden now: hide()'s (or the forced one), only while the player is in it.
        uint32_t hiddenVehicle(const uint8_t *ram)
        {
            uint32_t v = g_hidden.load(std::memory_order_relaxed);
            const bool forced = g_forced.load(std::memory_order_relaxed);
            if (!v && !forced)
                return 0;
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t mine = ped ? rd32(ram, ped + kPedVehicle) : 0u;
            if (!v)
                v = mine;
            return v == mine ? v : 0u;
        }

        bool isGlass(const uint8_t *ram, uint32_t element, uint32_t rgba)
        {
            if ((rgba >> 24) != 0xFFu) // HasAlphaMaterialCB's test (FUN_00394260)
                return true;
            const uint32_t frame = rd32(ram, element + kElementFrame);
            const uint32_t name = frame ? rd32(ram, frame + kFrameName) & kRamMask : 0u;
            return name && name <= kRamMask - 10u && std::memcmp(ram + name, "windscreen", 10) == 0;
        }

        // FUN_0039E7F0(rgba a0), called by the element render for each mesh before its draw.
        VCS_GUEST_CC void materialColour(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t vehicle = hiddenVehicle(rdram);
            if (vehicle)
            {
                const uint32_t ra = mod::gpr32(ctx, kRa);
                const uint32_t element = (ra == kRenderA || ra == kRenderB)       ? mod::gpr32(ctx, kS7)
                                         : (ra == kRender47A || ra == kRender47B) ? rd32(rdram, mod::gpr32(ctx, kSp))
                                                                                  : 0u;
                const uint32_t rgba = mod::gpr32(ctx, kA0);
                if (element && rd32(rdram, element + kElementClump) == rd32(rdram, vehicle + kVehicleClump) &&
                    isGlass(rdram, element, rgba))
                    mod::setGpr32(ctx, kA0, rgba & 0x00FFFFFFu); // alpha 0: the alpha test drops the mesh
            }
            return g_origColour(rdram, ctx, runtime);
        }
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_CARGLASS_HOOKS"); e && e[0] == '0')
            return;
        g_origColour = mod::api->lookup_function(kMaterialColour);
        if (!g_origColour)
            return;
        mod::api->replace_function(kMaterialColour, &materialColour);
        // carglass [0|1] -> ok forced= hidden=: 1 hides the player's vehicle's glass whatever hide() says (tests)
        mod::addCommand("carglass", [](const std::string &args, mod::Reply reply) {
            if (!args.empty())
                g_forced = args[0] == '1';
            char text[64];
            std::snprintf(text, sizeof(text), "ok forced=%d hidden=%08x", g_forced ? 1 : 0, g_hidden.load());
            reply(text);
        });
    }

    void hide(uint32_t vehicle)
    {
        if (g_hidden.exchange(vehicle, std::memory_order_relaxed) != vehicle)
            mod::log("[carglass] %s %08x\n", vehicle ? "hide" : "show", vehicle);
    }
}
