// A vehicle's glass not drawn (the in-car first-person view, firstperson.h): its windscreen, side and rear
// windows are left out of its draw while every other vehicle - and this one once hide(0) is called - is drawn as the
// game draws it.
//
// VCS has no glass atomics: a car's windows are meshes with a glass material (colour alpha below 255) inside its
// "chassis_hi", windscreen and door elements, and the materials are shared by every vehicle of the model (writing them
// would clear the glass of the parked twin too). Each mesh's material colour is handed to the VU by FUN_0039E7F0 from
// the element render (FUN_0039EC18 / FUN_0039E828) just before its draw; for the hidden vehicle's glass meshes the hook
// passes it on with alpha 0, so the alpha test leaves nothing of them. Nothing is written to the game's memory.
// Details and addresses: carglass.cpp. VCS_CARGLASS_HOOKS=0 hooks nothing (the reference).
#pragma once

#include <cstdint>

namespace vcs::carglass
{
    // VcsModInstallLate (no order with the other hooks: nothing else hooks FUN_0039E7F0).
    void install();

    // Game thread: from the next draw on, `vehicle`'s glass is not drawn (0: every vehicle's glass is). Cheap and
    // idempotent - call it every camera update. Only acts while `vehicle` is the player's (ped +0x480), so a vehicle that
    // was destroyed or left is never touched.
    void hide(uint32_t vehicle);
}
