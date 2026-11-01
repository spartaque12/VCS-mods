// PedSkills.asi: head shots with any gun. One bullet in a ped's head kills it, with the game's own head-shot death -
// the head taken off (CPed::RemoveBodyPart, with the gore setting), the face-hit death, armour no help - for every
// bullet weapon, not only the ones VCS gives it to.
//
// How the game decides (the port's docs/game-internals.md 12v): CPed::InflictDamage (FUN_001D7940) sends bullet weapons
// (19, 21 - 31, 34, 35, 45) to a block that, for a victim that is not the player and has no "no critical hits" flag
// (ped +0x1D0 bit 9), takes the per-piece branch always for the M4, Ruger, sniper rifles and M60, and otherwise only
// when rand() & 15 (shotguns & 7) is 0. The per-piece branch's head case removes the head and sets the flag that kills
// the ped whatever its health. So the mod answers those two rand() calls with 0 when the piece hit is the head - the
// game's own branch does the rest - and, with "Applies to everyone", answers that block's IsPlayer call with 0 for a
// head hit, so Vic can be one-shot too. Two head hits never reach that block: the Python's (20: its weapon case skips
// it) and any bullet's on a ped already falling or getting up (state 0x2D / 0x2E: InflictDamage skips the weapon cases -
// a shotgun's knock-back puts its victim there before the pellet's damage); those get damage enough to kill. Melee, explosions and fire never reach the block; the game's protections before it
// (invulnerable peds, "only damaged by the player", a ped already dying, bulletproof peds) stay as they are.
#include "mod.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kInflictDamage = 0x001D7940u; // CPed::InflictDamage(ped a0, attacker a1, weapon a2, damage f12, piece a3, direction t0)
    constexpr uint32_t kRand = 0x00408058u;          // rand(): v0
    constexpr uint32_t kIsPlayer = 0x0020C310u;      // CPed::IsPlayer(ped a0): v0 = ped type (+0x550) == 0

    // return addresses inside InflictDamage's bullet block: rand() & 15 / & 7 (the shotguns'), and IsPlayer(victim)
    constexpr uint32_t kRaRandOthers = 0x001D7EB4u;
    constexpr uint32_t kRaRandShotgun = 0x001D7EA4u;
    constexpr uint32_t kRaIsPlayer = 0x001D7E58u;

    constexpr uint32_t kPlayers = 0x004E4910u;     // CWorld::Players, 0x190 each; +0 the ped
    constexpr uint32_t kPlayerIndex = 0x004CD128u; // byte: the local player (FindPlayerPed FUN_0024A430)
    constexpr uint32_t kPedType = 0x550, kPedHealth = 0x4E4, kPedArmour = 0x4E8;
    constexpr uint32_t kPedFlagsD0 = 0x1D0;     // bit 9 (0x200): no critical hits (VC's bNoCriticalHits)
    constexpr uint32_t kEntityFlags = 0x48;     // bit 26 (0x4000000): bullet proof
    constexpr int kHead = 6;                    // VC's PEDPIECE_HEAD
    constexpr int kPython = 20;
    constexpr uint32_t kPedState = 0x8C4, kStateFall = 0x2D, kStateGetUp = 0x2E;

    enum Reg
    {
        V0 = 2, A0 = 4, A1 = 5, A2 = 6, A3 = 7, S1 = 17, S3 = 19, S5 = 21, S6 = 22, RA = 31
    };

    VcsGuestFn g_origInflict = nullptr, g_origRand = nullptr, g_origIsPlayer = nullptr;
    std::atomic<bool> g_on{true}, g_everyone{false};
    bool g_log = false; // VCS_PEDSKILLS_LOG=1: a log line for every hit by the player and every head shot made (tests)
    std::atomic<uint32_t> g_forced{0}, g_playerHits{0}, g_boosted{0};
    struct LastHit
    {
        uint32_t victim = 0, attacker = 0;
        int weapon = -1, piece = -1;
        float damage = 0, health = 0, armour = 0;
        uint32_t count = 0;
    } g_last;

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4);
        return v;
    }
    float rdf(const uint8_t *rdram, uint32_t a)
    {
        float v;
        std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4);
        return v;
    }
    uint32_t playerPed(const uint8_t *rdram)
    {
        return rd32(rdram, kPlayers + 0x190u * (rd32(rdram, kPlayerIndex) & 0xFFu));
    }
    bool bulletWeapon(int w) { return w == 19 || (w >= 21 && w <= 31) || w == 34 || w == 35 || w == 45; }

    // the shooter counts: the player's shots, or anyone's with "Applies to everyone"
    bool shooterCounts(const uint8_t *rdram, uint32_t attacker)
    {
        if (g_everyone.load(std::memory_order_relaxed))
            return true;
        return attacker && attacker == playerPed(rdram);
    }

    void returnV0(VcsGuestContext *ctx, uint32_t v0)
    {
        mod::setGpr32(ctx, V0, v0);
        mod::setPc(ctx, mod::gpr32(ctx, RA));
    }

    // rand() at InflictDamage's "does this bullet take the per-piece branch" draws: 0 for a head hit
    VCS_GUEST_CC void rand_(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
    {
        const uint32_t ra = mod::gpr32(ctx, RA);
        if ((ra == kRaRandOthers || ra == kRaRandShotgun) && g_on.load(std::memory_order_relaxed) &&
            int(mod::gpr32(ctx, S3)) == kHead && shooterCounts(rdram, mod::gpr32(ctx, S5)))
        {
            g_forced.fetch_add(1, std::memory_order_relaxed);
            if (g_log)
                mod::log("[PedSkills] head shot: ped %08X weapon %d\n", mod::gpr32(ctx, S1), int(mod::gpr32(ctx, S6)));
            return returnV0(ctx, 0);
        }
        return g_origRand(rdram, ctx, runtime);
    }

    // IsPlayer(victim) in the bullet block (it keeps the player out of the per-piece branch): "not the player" for a
    // head hit with "Applies to everyone"
    VCS_GUEST_CC void isPlayer(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
    {
        if (mod::gpr32(ctx, RA) == kRaIsPlayer && g_on.load(std::memory_order_relaxed) &&
            g_everyone.load(std::memory_order_relaxed) && int(mod::gpr32(ctx, S3)) == kHead &&
            bulletWeapon(int(mod::gpr32(ctx, S6))))
        {
            g_playerHits.fetch_add(1, std::memory_order_relaxed);
            return returnV0(ctx, 0);
        }
        return g_origIsPlayer(rdram, ctx, runtime);
    }

    // InflictDamage: notes the hit (the "pedskills" command); a head hit that never reaches the bullet block (a Python's, or
    // on a ped falling / getting up) gets damage enough to kill
    VCS_GUEST_CC void inflictDamage(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
    {
        const uint32_t ped = mod::gpr32(ctx, A0), attacker = mod::gpr32(ctx, A1);
        const int weapon = int(mod::gpr32(ctx, A2)), piece = int(mod::gpr32(ctx, A3));
        float *f = mod::fpr(ctx);
        if (ped)
        {
            g_last.victim = ped;
            g_last.attacker = attacker;
            g_last.weapon = weapon;
            g_last.piece = piece;
            g_last.damage = f[12];
            g_last.health = rdf(rdram, ped + kPedHealth);
            g_last.armour = rdf(rdram, ped + kPedArmour);
            ++g_last.count;
            if (g_log && attacker && attacker == playerPed(rdram))
                mod::log("[PedSkills] hit: ped %08X weapon %d piece %d damage %.1f health %.1f armour %.1f\n", ped,
                         weapon, piece, f[12], g_last.health, g_last.armour);
        }
        const uint32_t state = ped ? rd32(rdram, ped + kPedState) : 0;
        if (ped && piece == kHead && (weapon == kPython || (bulletWeapon(weapon) && (state == kStateFall || state == kStateGetUp))) &&
            g_on.load(std::memory_order_relaxed) &&
            shooterCounts(rdram, attacker) && attacker != ped &&
            (rd32(rdram, ped + kPedType) != 0 || g_everyone.load(std::memory_order_relaxed)) &&
            !(rd32(rdram, ped + kPedFlagsD0) & 0x200u) && !(rd32(rdram, ped + kEntityFlags) & 0x4000000u))
        {
            const float kill = rdf(rdram, ped + kPedHealth) + rdf(rdram, ped + kPedArmour) + 100.0f;
            if (f[12] < kill)
            {
                f[12] = kill;
                g_boosted.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return g_origInflict(rdram, ctx, runtime);
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "pedskills.headshots";
        s.page = "Game";
        s.section = "Ped skills";
        s.label = "Headshots with any gun";
        s.help = "One bullet in a ped's head kills it with any gun - pistols, SMGs, rifles, shotgun pellets, the M60 and "
                 "minigun, drive-by shots - as the game's own head shot does with the rifles: the head comes off (with "
                 "gore on) and armour does not help. Melee, explosions and fire are unchanged; peds a mission protects "
                 "stay protected. Off: the game's rule.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_HEADSHOTS";
        s.onChange = [](int v) { g_on = v != 0; };
        add(s);

        s = {};
        s.key = "pedskills.everyone";
        s.page = "Game";
        s.section = "Ped skills";
        s.label = "Applies to everyone";
        s.help = "Head shots by every ped count too, not only yours - and one bullet in Vic's head kills him.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_HEADSHOTS_EVERYONE";
        s.onChange = [](int v) { g_everyone = v != 0; };
        add(s);

        addCommand("pedskills", [](const std::string &, Reply reply) {
            char line[320];
            std::snprintf(line, sizeof line,
                          "ok on=%d everyone=%d hooked=%d forced=%u player=%u boosted=%u hits=%u victim=%08X attacker=%08X "
                          "weapon=%d piece=%d damage=%.2f health=%.2f armour=%.2f",
                          int(g_on.load()), int(g_everyone.load()), int(g_origRand != nullptr), g_forced.load(),
                          g_playerHits.load(), g_boosted.load(), g_last.count, g_last.victim, g_last.attacker,
                          g_last.weapon, g_last.piece, g_last.damage, g_last.health, g_last.armour);
            reply(line);
        });
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_PEDSKILLS_HOOKS"); e && e[0] == '0')
            return;
        if (const char *e = std::getenv("VCS_PEDSKILLS_LOG"); e && e[0] == '1')
            g_log = true;
        g_origInflict = mod::api->lookup_function(kInflictDamage);
        g_origRand = mod::api->lookup_function(kRand);
        g_origIsPlayer = mod::api->lookup_function(kIsPlayer);
        if (!g_origInflict || !g_origRand || !g_origIsPlayer)
        {
            g_origRand = nullptr;
            mod::log("[PedSkills] game functions not found: nothing hooked\n");
            return;
        }
        mod::api->replace_function(kInflictDamage, &inflictDamage);
        mod::api->replace_function(kRand, &rand_);
        mod::api->replace_function(kIsPlayer, &isPlayer);
    }
}

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "PedSkills"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION)
            return 0;
        mod::api = a;
        registerSettings(); // the Game page, after "Window", before "Physics"
        return 1;
    }

    VCS_MOD_EXPORT int VcsModInstall(const VcsModApi *)
    {
        install();
        return 1;
    }
}
