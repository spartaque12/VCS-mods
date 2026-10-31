// Manual reload (SA's manual reload mods; game-internals.md 12t): a key (R; Back + Square on a gamepad) reloads the
// gun in hand when its clip is not full and there are rounds to spare - the way the game does on an empty clip
// (CWeapon::Fire 0x2C1998: the slot's state 2 and its end time = now + the weapon info's reload time), so the game
// plays the reload anim and sound and CWeapon::Update (FUN_002CE018) refills the clip (CWeapon::Reload FUN_002CDFB0)
// when the time is up. Game > Manual reload switches it off.
#pragma once

#include <cstdint>

namespace vcs::reload
{
    // Game page row and the key (VcsModRegister).
    void registerSettings();

    // Game thread, every camera update (any camera): a pressed key is carried out here.
    void update(uint8_t *ram);

    // Game thread: the player's gun is reloading (the slot's state 2).
    bool reloading(const uint8_t *ram, uint32_t ped);
}
