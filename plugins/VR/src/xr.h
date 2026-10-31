// VR.asi's OpenXR side (modelled on praydog/REFramework's src/mods/vr/runtimes/OpenXR.cpp, MIT): the loader
// (openxr_loader.dll next to VR.asi, loaded at run time), an instance with XR_KHR_D3D11_enable, the head-mounted
// display's system, a session on vcs.exe's D3D11 device, the reference spaces (LOCAL, recentred), swapchains and the
// frame loop (xrWaitFrame / xrBeginFrame / xrEndFrame). Main thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace vr
{
    // A line for the game's log ("[VR] " in front) and for <vcs.exe's folder>\run\vr.log (time-stamped, started afresh
    // each run): what a player sends back when VR does not work.
    void log(const char *format, ...)
#if defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;
    std::string logPath();

    struct Pose
    {
        float q[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
        float p[3] = {0.0f, 0.0f, 0.0f};       // metres; OpenXR axes: x right, y up, -z ahead
    };
    struct Fov
    {
        float left = 0.0f, right = 0.0f, up = 0.0f, down = 0.0f; // radians (left, down negative)
    };

    enum class Panel
    {
        Scene = 0,  // the 3D picture (projection layer)
        Hud = 1,    // the HUD (quad, premultiplied alpha)
        Cinema = 2, // the window's picture and menus (quad)
        Count = 3
    };

    // A projection layer showing one image to both eyes (mono), rendered for `pose` with field of view `fov`.
    struct ProjectionLayer
    {
        bool on = false;
        Pose pose;
        Fov fov;
    };
    // A quad in the recentred space: centre `pose`, width x height metres.
    struct QuadLayer
    {
        bool on = false;
        Panel panel = Panel::Hud;
        Pose pose;
        float width = 1.0f, height = 1.0f;
        bool alpha = false; // blended by the texture's (premultiplied) alpha
    };

    // Start: false with `why` (a line for the log) when there is no loader, runtime, headset or D3D11 support; the mod
    // then stays inert. Waiting for a headset that is off is a start that succeeds later (pending()).
    bool start(ID3D11Device *device, std::string &why);
    void stop();
    bool started();
    bool pending(); // the instance is there, the headset not (yet): start again later
    bool exited();  // the runtime ended the session (XR_SESSION_STATE_EXITING): not restarted until VR is switched on again
    void clearExited();
    const char *sessionStateName();
    float refreshHz(); // from the predicted display period (0 before the first frame)
    std::string runtimeName();

    // Events (session state); call once per frame before frame().
    void poll();
    bool running(); // the session runs: frames go to the headset
    bool focused(); // the application has the headset's input focus

    // xrWaitFrame (paces the caller to the headset) + xrBeginFrame. False when the session does not run.
    bool waitAndBegin();
    bool frameBegun();
    bool shouldRender();
    int64_t predictedDisplayTime();
    // The head (the VIEW space) in the recentred space at the predicted display time.
    bool locateHead(Pose &out);
    // The recentred space: its origin at the head's position, facing where the head faces (yaw only), from now on.
    void recenter();

    // The image to draw this frame's panel into (acquired; released by endFrame), made or remade at width x height.
    // `srgb`: the image's format is an *_SRGB one (write linear colour).
    ID3D11Texture2D *panelImage(Panel panel, uint32_t width, uint32_t height, bool &srgb);
    // xrEndFrame with the layers (projection first, quads in order); none when shouldRender is false.
    void endFrame(const ProjectionLayer &projection, const QuadLayer *quads, int quadCount);

    // statistics for the control channel
    uint64_t framesSubmitted();
    std::string status();
}
