// MeshSmooth.asi (README.md): lit model batches - peds (skinned), vehicle bodies (environment-mapped), other lit models
// (wheels, props, weapons) - refined on the render worker with curved PN triangles and drawn in the original's place.
#pragma once

namespace vcs::meshsmooth
{
    void registerHotkeyAndCommands(); // VcsModRegister: the toggle key (Insert) and the "meshsmooth" command
    void install();                   // VcsModRegister: the lit refiner
    void registerSettings();          // slot "graphics": the rows
}
