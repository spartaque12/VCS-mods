/* The VCS mod API (docs/MODDING.md): what vcs.exe gives a mod built as a separate DLL - an ".asi" in build\scripts\
 * (or next to vcs.exe) - and what such a mod exports.
 *
 * A plain C ABI: a versioned table of function pointers, no C++ types across the boundary, so a mod is built
 * separately (its own repository: spartaque12/VCS-mods) and still works with a later vcs.exe of the same API version.
 * The one thing it shares with the exe is the guest calling convention: hooks on recompiled game functions use
 * VCS_GUEST_CC (clang's preserve_none on x86-64), so a mod with hooks is built with clang / clang-cl too.
 *
 * A mod exports (extern "C"):
 *   uint32_t VcsModApiVersion(void);             required: VCS_MOD_API_VERSION as the mod was built
 *   int  VcsModRegister(const VcsModApi *api);   required: start-up, before vcs.ini is read - add settings, hotkeys,
 *                                                the look handler, draw callbacks, control commands. 0 = failed (the
 *                                                mod is left out: Install is not called)
 *   int  VcsModInstall(const VcsModApi *api);    optional: when the game's ELF is loaded, before the game runs - its
 *                                                hooks (replace_function), installed before the built-in mods' hooks
 *                                                (those wrap it: e.g. the ragdolls' bone hooks run first)
 *   int  VcsModInstallLate(const VcsModApi *api); optional: right after that, once the built-in mods' hooks are in
 *                                                (this one's wrap theirs: e.g. a line-of-sight hook that changes a
 *                                                bullet's line before the ragdolls see it)
 *   const char *VcsModName(void);                optional: the name in the log (default: the file name)
 *   int  VcsModRegisterSettings(const VcsModApi *api, const char *slot);
 *                                                optional: settings rows registered at a later place of the menu than
 *                                                VcsModRegister's (the Game page after "Window"). Slots: "graphics" -
 *                                                after the Graphics page's renderer rows, before the controls pages
 *                                                (where Project2DFX's rows were when built in). add_setting only.
 *
 * Versioning: VCS_MOD_API_VERSION changes only when something breaks old mods (a field moved or changed meaning);
 * the loader refuses a mod built for another version, with a log line. New functions are appended to VcsModApi
 * without a version change: `size` says how much of the table this exe has (VCS_MOD_API_HAS).
 */
#ifndef VCS_MOD_API_H
#define VCS_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#define VCS_MOD_API_VERSION 1u

/* The calling convention of guest functions (third_party/PS2Recomp/ps2xRuntime/include/ps2_guest_cc.h). */
#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(PS2X_GUEST_CC_DEFAULT)
#define VCS_GUEST_CC __attribute__((preserve_none))
#else
#define VCS_GUEST_CC
#endif

#if defined(_WIN32)
#define VCS_MOD_EXPORT __declspec(dllexport)
#else
#define VCS_MOD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The guest CPU state (the runtime's R5900Context), opaque: its registers are reached through the offsets in
 * VcsModApi (vcs_gpr_u32 and friends below). */
typedef struct VcsGuestContext VcsGuestContext;

/* A recompiled game function: (rdram, ctx, runtime). A hook calls the original it replaced (lookup_function before
 * replace_function) or returns to the caller by setting pc to $ra. Rules: AGENTS.md "Hooks on recompiled functions". */
typedef void(VCS_GUEST_CC *VcsGuestFn)(uint8_t *rdram, VcsGuestContext *ctx, void *runtime);

/* Settings (src/settings/settings.h): a row in the F2 menu and a key in vcs.ini. */
enum
{
    VCS_SETTING_TOGGLE = 0,  /* 0 / 1 */
    VCS_SETTING_CHOICE = 1,  /* one of `choices` */
    VCS_SETTING_INTEGER = 2, /* min..max */
    VCS_SETTING_KEY = 3,     /* a key code (VcsModHotkey) */
    VCS_SETTING_ACTION = 4   /* a button: on_change(0) when pressed; no value */
};
enum
{
    VCS_APPLY_LIVE = 0,   /* on_change at once */
    VCS_APPLY_RESTART = 1 /* takes effect at the next start */
};

typedef struct VcsModChoice
{
    const char *label;
    int value;
} VcsModChoice;

/* Strings are copied; `user` is handed back to the callbacks. Rows of one page + section are shown together; a mod's
 * rows go where the built-in camera mods' rows were (the Game page, after "Window"). */
typedef struct VcsModSetting
{
    uint32_t size;       /* sizeof(VcsModSetting) */
    const char *key;     /* vcs.ini key, e.g. "camera.first_person" */
    const char *page;    /* "Game", "Graphics", ... */
    const char *section; /* heading inside the page */
    const char *label;
    const char *help;    /* one line */
    int type;            /* VCS_SETTING_* */
    int min, max;        /* INTEGER */
    const char *format;  /* INTEGER, as shown: "%d deg"; NULL "%d" */
    int default_value;
    const char *env;     /* an environment variable that overrides it for one run (NULL: none) */
    int apply;           /* VCS_APPLY_* */
    const VcsModChoice *choices; /* CHOICE */
    int choice_count;
    const char *button;  /* ACTION: the button's text */
    void (*on_change)(void *user, int value); /* the new value; also once at start-up with the loaded one */
    int (*read)(void *user);                  /* optional: the live value when something else changes it (a hotkey) */
    void *user;
} VcsModSetting;

/* Gamepad buttons for VcsModHotkey.pad_chord (held together with Back). */
enum
{
    VCS_MOD_PAD_NONE = 0,
    VCS_MOD_PAD_SOUTH = 1,
    VCS_MOD_PAD_EAST = 2,
    VCS_MOD_PAD_WEST = 3,
    VCS_MOD_PAD_NORTH = 4,
    VCS_MOD_PAD_START = 6,
    VCS_MOD_PAD_LEFT_STICK = 7,
    VCS_MOD_PAD_RIGHT_STICK = 8,
    VCS_MOD_PAD_LEFT_SHOULDER = 9,
    VCS_MOD_PAD_RIGHT_SHOULDER = 10,
    VCS_MOD_PAD_DPAD_UP = 11,
    VCS_MOD_PAD_DPAD_DOWN = 12,
    VCS_MOD_PAD_DPAD_LEFT = 13,
    VCS_MOD_PAD_DPAD_RIGHT = 14
};

/* A hotkey: a Key row on the "Keyboard & mouse" page (section "Menus", exclusive with the other menu keys, unbindable
 * with a right click) and, optionally, Back + a gamepad button. `pressed` runs on the main thread when it is pressed
 * outside the menus. Key codes: platform::Key (src/platform/platform.h) - letters and digits are their ASCII upper
 * case ('B' = 66). */
typedef struct VcsModHotkey
{
    uint32_t size; /* sizeof(VcsModHotkey) */
    const char *key;   /* vcs.ini key, e.g. "controls.firstperson_key" */
    const char *label;
    const char *help;
    int default_key;
    int pad_chord; /* VCS_MOD_PAD_* */
    void (*pressed)(void *user);
    void *user;
} VcsModHotkey;

/* The last pad report the controls made (any thread): DualShock 2 buttons held (1 = held: R1 0x0800, Circle 0x2000
 * ...), sticks -1..1 (y down +). */
typedef struct VcsModPad
{
    uint16_t buttons;
    float lx, ly, rx, ry;
} VcsModPad;

/* The mouse / right-stick look (src/game/vcs_input.cpp): while active() says so (game thread, every pad read), the
 * mouse and the right stick turn the mod's view - look(yaw left +, pitch up +, radians) - and the game's camera gets
 * no stick. One handler; the free camera comes first. */
typedef struct VcsModLookHandler
{
    int (*active)(void *user);
    void (*look)(void *user, float yaw, float pitch);
    void *user;
} VcsModLookHandler;

/* A control-channel command (tools/rig.py, tools/gamectl.py): `args` is the rest of the line; the handler writes its
 * one-line answer ("ok ..." / "error ...") into `answer` on the main thread. */
typedef void (*VcsModCommandFn)(void *user, const char *args, char *answer, size_t answer_size);

/* Rendering (appended 2026-10-31, docs/MODDING.md "Rendering"): a mod's GPU passes inside the renderer. At a point of the
 * frame the renderer calls the mod's render hook on the main thread with the frame's targets and camera; the hook draws
 * full-screen passes with the render_* functions of VcsModApi - the port's RHI (src/rhi/rhi.h) as opaque handles, the
 * same calls on D3D11 and Vulkan. Shaders are bytecode for the device's backend (VcsRenderFrame.backend): DXBC from fxc
 * (vs_5_0 / ps_5_0) for D3D11, SPIR-V for Vulkan with the D3D registers as bindings (bN -> N, tN -> 16 + N, sN -> 32 + N,
 * set 0: the port's tools/hlsl_spirv.py, glslang --shift-texture-binding 16 --shift-sampler-binding 32). */
typedef struct VcsRenderTexture VcsRenderTexture; /* opaque */
typedef struct VcsRenderBuffer VcsRenderBuffer;
typedef struct VcsRenderPipeline VcsRenderPipeline;

enum
{
    VCS_RENDER_BACKEND_D3D11 = 0,
    VCS_RENDER_BACKEND_VULKAN = 1,
    VCS_RENDER_BACKEND_NULL = 2 /* no GPU work (tests): resources may be NULL */
};
enum
{
    VCS_RENDER_FORMAT_RGBA8 = 1, /* 8-bit unorm */
    VCS_RENDER_FORMAT_R32F = 2,  /* point sampling only (Vulkan does not promise linear filtering of it) */
    VCS_RENDER_FORMAT_RGBA16F = 3 /* half floats (motion vectors); appended 2026-10-31 */
};
enum
{
    VCS_RENDER_SAMPLER_POINT = 0, /* clamped */
    VCS_RENDER_SAMPLER_LINEAR = 1
};
enum
{
    VCS_RENDER_BLEND_OPAQUE = 0,   /* the output replaces the target */
    VCS_RENDER_BLEND_MULTIPLY = 1, /* rgb = target.rgb * output.a (darkening: ambient occlusion, shadows) */
    VCS_RENDER_BLEND_ALPHA = 2,    /* rgb = lerp(target.rgb, output.rgb, output.a) */
    VCS_RENDER_BLEND_ADD = 3       /* rgb = target.rgb + output.rgb */
};
/* Where in the frame a hook runs. */
enum
{
    /* the 3D scene is drawn - sky, world, water, peds, vehicles, blob shadows, particles - and the game's post effects
     * (bloom, trails, motion blur) and the HUD come next */
    VCS_RENDER_BEFORE_POST = 1,
    /* -- appended 2026-11-01 (an older vcs.exe refuses these points: add_render_hook returns 0) -- around the game's
     * water (game-internals.md 6p), drawn after the world and before the sky's self-copy, the reflection map, peds and
     * vehicles. BEFORE_WATER: right before
     * the first water draw - the picture and Z buffer hold the world without the water (copy what you need: the seabed
     * for refraction, its depth). AFTER_WATER: right after the last - the water drawn and in the Z buffer (where the Z
     * buffer is nearer than before, water is in front); water_rect bounds it. A frame without water runs neither; a frame
     * may run both more than once (each run of water draws). No velocity, reprojection or shadow maps at these points. */
    VCS_RENDER_BEFORE_WATER = 2,
    VCS_RENDER_AFTER_WATER = 3
};
/* What a hook reads (add_render_hook's flags). */
enum
{
    VCS_RENDER_NEEDS_DEPTH = 1u,         /* VcsRenderFrame.depth: the Z buffer made sampleable */
    VCS_RENDER_NEEDS_RESOLVED_COLOR = 2u, /* VcsRenderFrame.color_resolved: the picture resolved (MSAA) to read */
    VCS_RENDER_NEEDS_VELOCITY = 4u,       /* VcsRenderFrame.velocity: per-object motion (appended 2026-10-31) */
    VCS_RENDER_NEEDS_SHADOW = 8u          /* render_shadow_map: the scene's casters kept (appended 2026-11-01) */
};

typedef struct VcsRenderFrame
{
    uint32_t size;    /* sizeof(VcsRenderFrame) of this exe */
    int backend;      /* VCS_RENDER_BACKEND_* */
    uint64_t frame;   /* frames the hooks ran in */
    VcsRenderTexture *color;          /* the picture: RGBA8, multisampled when color_samples > 1 - draw into it */
    VcsRenderTexture *color_resolved; /* single-sampled copy of it to read (VCS_RENDER_NEEDS_RESOLVED_COLOR), or NULL */
    VcsRenderTexture *depth;          /* its Z buffer (VCS_RENDER_NEEDS_DEPTH), or NULL in the first frames: a float d =
                                         GS z / 2^24 (0 = nothing drawn, far; larger = nearer); a Texture2DMS<float> when
                                         depth_samples > 1 (declare it in VcsRenderPipelineDesc.multisampled_textures) */
    uint32_t color_samples, depth_samples;
    uint32_t width, height; /* the picture in target pixels, from (0, 0): the textures can be larger */
    /* The camera, from the frame's 3D (camera_valid): the target pixel centre (x + 0.5, y + 0.5) at view depth w (metres)
     * is the view-space point ((x + 0.5 - cx) / fx * w, (y + 0.5 - cy) / fy * w, w): x right, y down, w forward. */
    int camera_valid;
    float fx, fy, cx, cy;
    /* The Z buffer: GS z = d / depth_scale = depth_a + depth_b / w, so w = depth_b / (d / depth_scale - depth_a). */
    float depth_a, depth_b, depth_scale;
    /* The game's distance fog on the world: visibility clamp((fog_bias + fog_scale * w) / 255, 0, 1) (1 = no fog). */
    float fog_bias, fog_scale;
    /* -- appended 2026-10-31 (check size >= offsetof(VcsRenderFrame, reproject) + sizeof(reproject)) -- temporal AA:
     * the sub-pixel offset this frame's 3D was drawn with and the last frame's (target pixels; render_set_jitter), and
     * the camera's reprojection (reproject_valid), row-major Q: (u' w', v' w', ., w') = Q (u w, v w, z w, w) with (u, v)
     * this frame's unjittered target position (pixel centre - jitter), z the GS z, w the view depth; (u', v') is where
     * that point was in the last frame, unjittered. Camera motion only (from static world draws): moving objects have no
     * vectors of their own yet. */
    float jitter_x, jitter_y, prev_jitter_x, prev_jitter_y;
    int reproject_valid;
    float reproject[16];
    /* -- appended 2026-10-31 (check size >= offsetof(VcsRenderFrame, velocity) + sizeof(velocity)) -- per-object motion
     * (VCS_RENDER_NEEDS_VELOCITY): RGBA16F, width x height: xy = the pixels the surface moved since the last frame
     * (unjittered, now - then), z = its view depth, w = 1 where a native draw of a matched object covers the pixel, 0 where
     * none does (use `reproject`). Compare z with the depth from `depth`: draws through the GS path are not in it. */
    VcsRenderTexture *velocity;
    /* -- appended 2026-11-01 (check size >= offsetof(VcsRenderFrame, world_to_view) + sizeof(world_to_view)) -- the
     * camera's rotation in the world (view_valid), from the frame's static world draws: row-major, view = R world, world
     * x east, y north, z up (metres), view as above (x right, y down, w forward). */
    int view_valid;
    float world_to_view[9];
    /* -- appended 2026-11-01 (check size >= offsetof(VcsRenderFrame, camera_position) + sizeof(camera_position)) -- the
     * camera's position (position_valid; hooks with VCS_RENDER_NEEDS_SHADOW) in a frame of reference the renderer keeps
     * from the static world's draws: world axes, metres, an origin of the renderer's own (renewed after a teleport) - for
     * differences between frames, e.g. snapping shadow maps to the world. */
    int position_valid;
    float camera_position[3];
    /* -- appended 2026-11-01 (check size >= offsetof(VcsRenderFrame, water_color) + sizeof(water_color)) -- the hook point
     * this call is for (VCS_RENDER_*), and at VCS_RENDER_AFTER_WATER the water draws' bounds in target pixels (x0, y0,
     * x1, y1; exclusive ends, clamped to width x height), and at both water points the first water draw's vertex colour
     * (RGBA, 1.0 = the PS2's 0x80): the timecycle's water tint the game multiplies its water texture by. */
    int point;
    int water_rect[4];
    float water_color[4];
} VcsRenderFrame;

typedef void (*VcsRenderHookFn)(void *user, const VcsRenderFrame *frame);

enum
{
    VCS_RENDER_NATIVE_DEVICE = 0,  /* ID3D11Device * */
    VCS_RENDER_NATIVE_CONTEXT = 1, /* ID3D11DeviceContext * (the immediate context) */
    VCS_RENDER_NATIVE_TEXTURE = 2  /* ID3D11Texture2D * of the texture */
};
enum
{
    VCS_RENDER_TEXTURE_STORAGE = 1u /* unordered access (D3D11) / storage (Vulkan) */
};

/* A full-screen pass's pipeline: vertices from SV_VertexID (no vertex buffer), a triangle list, no depth test. */
typedef struct VcsRenderPipelineDesc
{
    uint32_t size; /* sizeof(VcsRenderPipelineDesc) */
    const void *vertex_code;
    size_t vertex_size;
    const void *pixel_code;
    size_t pixel_size;
    int blend;                      /* VCS_RENDER_BLEND_* */
    uint32_t write_mask;            /* RGBA bits (0xF: all; 0x7 keeps the target's alpha) */
    uint32_t multisampled_textures; /* texture slots (bit per slot) the pixel shader declares Texture2DMS */
    const char *name;               /* debugger / GPU timing name */
} VcsRenderPipelineDesc;

/* Lit model geometry (appended 2026-10-31, docs/MODDING.md "Model geometry"): one VU1 batch of a lit model - a ped
 * (skinned), a vehicle body (rigid, environment-mapped) or another lit model (rigid: wheels, props, weapons) - as the
 * native draw (game/vcs_hle_world.cpp) gets it, before it is drawn: the uploaded VU data as integers, a triangle strip
 * of draw_count vertices (degenerate triangles join its runs). The real-space position is pos * pos_scale + pos_offset;
 * normals are ints of length ~127 (rigid) / ~128 (skinned); uv are the V2-8 texture coordinates (the game's uv matrix
 * applies); weights (skinned) are float bits whose low bits (bone_mask) pick the bone. */
enum
{
    VCS_LIT_SKINNED = 0,
    VCS_LIT_RIGID = 1,
    VCS_LIT_ENV = 2 /* rigid with an environment map (vehicle paint) */
};
typedef struct VcsLitBatch
{
    uint32_t size; /* sizeof(VcsLitBatch) */
    int kind;      /* VCS_LIT_* */
    uint32_t count, draw_count;
    const int32_t (*pos)[4];
    const int32_t (*nrm)[4];
    const int32_t (*uv)[4];
    const uint32_t (*weights)[4]; /* skinned only, else NULL */
    uint32_t bone_mask;
    float pos_scale[4], pos_offset[4];
} VcsLitBatch;
/* A replacement for the batch: vertex_count vertices in the batch's units (floats), drawn as the triangle strip
 * `indices` (index_count; degenerate triangles allowed). Valid until the refiner's next call; `key` identifies its
 * content (the GPU's mesh cache). */
typedef struct VcsLitRefined
{
    uint64_t key;
    uint32_t vertex_count, index_count;
    const float (*pos)[3];
    const float (*nrm)[3];
    const float (*uv)[3];
    const uint32_t (*weights)[4]; /* skinned: the batch's encoding */
    const uint16_t *indices;
} VcsLitRefined;
/* The render worker's thread, once per batch drawn natively; NULL = draw the batch as it is. */
typedef const VcsLitRefined *(*VcsLitRefineFn)(void *user, const VcsLitBatch *batch);

/* VR (appended 2026-11-01, docs/MODDING.md "VR"; VR.asi, docs/VR_PLAN.md M0): a present hook runs on the main thread once
 * per host frame, after the window's picture and menus are drawn and right before the window's frame is presented - where
 * an OpenXR mod submits its frame and waits for the next (xrEndFrame / xrWaitFrame / xrBeginFrame). With
 * render_set_split_layers on, the frame carries the game's picture without the HUD and the HUD alone. Textures are the
 * graphics API's own (D3D11: ID3D11Texture2D *, DXGI_FORMAT_R8G8B8A8_UNORM, holding sRGB-encoded colour), valid inside the
 * hook; NULL on other backends for now. A hook may use the immediate context; the renderer sets its state again. */
typedef struct VcsPresentFrame
{
    uint32_t size;  /* sizeof(VcsPresentFrame) of this exe */
    int backend;    /* VCS_RENDER_BACKEND_* */
    void *device;   /* ID3D11Device * (D3D11), else NULL */
    void *context;  /* ID3D11DeviceContext * (the immediate context) */
    uint32_t window_width, window_height; /* the window's back buffer */
    /* render_set_split_layers: the shown picture (post effects in) without the HUD, and the native HUD layer drawn over
     * black and over white - coverage = 1 - (white - black), premultiplied colour = black - all layer_width x layer_height
     * (the display's output size). NULL when the shown frame has none: a picture from CPU memory (loading screens), a
     * black frame, splitting off. */
    void *scene, *hud_black, *hud_white;
    uint32_t layer_width, layer_height;
    int has_3d;            /* the picture shows 3D (the recent frames drew it: not a menu / loading screen) */
    int has_hud;           /* the HUD layer had draws */
    uint64_t layer_serial; /* changes with every composed game frame that had layers */
    int64_t input_ns;      /* std::chrono::steady_clock ns of the input snapshot the shown game frame was built from */
} VcsPresentFrame;
typedef void (*VcsPresentHookFn)(void *user, const VcsPresentFrame *frame);

typedef struct VcsModApi
{
    uint32_t version; /* VCS_MOD_API_VERSION */
    uint32_t size;    /* sizeof(VcsModApi) of this exe */

    /* -- the game ---------------------------------------------------------------------------------------------- */
    uint8_t *(*rdram)(void); /* the guest's 32 MB of RAM (NULL before Install); addresses masked with 0x01FFFFFF */
    int (*replace_function)(uint32_t address, VcsGuestFn fn); /* Install / InstallLate only; 0 = no such function */
    VcsGuestFn (*lookup_function)(uint32_t address);          /* what runs there now (a hook's original) */
    /* the guest context's layout: 32 GPRs of 16 bytes (the low 64 bits are the register), pc (uint32_t), the FPU's 32
     * floats */
    uint32_t ctx_gpr_offset, ctx_pc_offset, ctx_fpr_offset;

    /* -- settings (VcsModRegister only for add_*; value / set any time on the main thread) ----------------------- */
    int (*add_setting)(const VcsModSetting *setting);
    int (*setting_value)(const char *key);       /* the default for unknown keys */
    int (*set_setting)(const char *key, int value); /* saves vcs.ini, calls on_change; 0 if refused (env override) */
    int (*add_hotkey)(const VcsModHotkey *hotkey);

    /* -- the control channel and the log ------------------------------------------------------------------------ */
    void (*add_command)(const char *verb, VcsModCommandFn fn, void *user); /* VcsModRegister / Install */
    void (*log)(const char *text); /* the game's log (stderr), as is: end the line with "\n" */

    /* -- Dear ImGui (the exe's context: same version and configuration in the mod, docs/MODDING.md) -------------- */
    uint32_t imgui_version_num; /* IMGUI_VERSION_NUM */
    uint32_t imgui_sizeof_io, imgui_sizeof_style, imgui_sizeof_vec2, imgui_sizeof_vec4, imgui_sizeof_drawvert,
        imgui_sizeof_drawidx, imgui_sizeof_context; /* to compare with the mod's own headers */
    void *(*imgui_context)(void); /* ImGuiContext* - ImGui::SetCurrentContext before drawing */
    void (*imgui_allocators)(void **alloc_fn, void **free_fn, void **user); /* for ImGui::SetAllocatorFunctions */
    /* Main thread, every host frame before the menus: the callback draws into ImGui::GetBackgroundDrawList() (under
     * the menus) - a crosshair, a HUD element. */
    void (*add_draw_callback)(void (*fn)(void *user), void *user);

    /* -- input and the game's other parts the camera needs ------------------------------------------------------ */
    void (*set_look_handler)(const VcsModLookHandler *handler);
    void (*last_pad)(VcsModPad *out);
    int (*freecam_flying)(void); /* the trainer's free camera flies: leave the camera to it */
    const char *(*game_dir)(void); /* the folder of vcs.exe (UTF-8) */

    /* -- appended 2026-10-31 (API 1; check with VCS_MOD_API_HAS) ----------------------------------------------- */
    /* A host function at an address that has none - a continuation a guest tail call returns to (Install /
     * InstallLate only). 0 if refused. */
    int (*register_function)(uint32_t address, VcsGuestFn fn);
    int (*has_function)(uint32_t address); /* a function (or a resume point) is there already */

    /* -- rendering (appended 2026-10-31, API 1; check with VCS_MOD_API_HAS(api, render_pop_marker)) ------------- */
    /* A render hook at a point of the frame (VCS_RENDER_BEFORE_POST), flags VCS_RENDER_NEEDS_*: the main thread, any time
     * (a setting's on_change: until a hook is added nothing in the renderer changes); it stays until the game ends. 0 if
     * refused. Hooks run in the order they were added. */
    int (*add_render_hook)(int point, VcsRenderHookFn fn, void *user, uint32_t flags);
    /* Inside a render hook only (the device's thread). Resources live until destroyed (or the game ends); a texture is a
     * render target that can be sampled. NULL when refused. */
    VcsRenderTexture *(*render_create_texture)(uint32_t width, uint32_t height, int format, const char *name);
    void (*render_destroy_texture)(VcsRenderTexture *texture);
    VcsRenderBuffer *(*render_create_constants)(uint32_t size, const char *name); /* a cbuffer, rewritten whole */
    void (*render_update_constants)(VcsRenderBuffer *buffer, const void *data, uint32_t size);
    void (*render_destroy_constants)(VcsRenderBuffer *buffer);
    VcsRenderPipeline *(*render_create_pipeline)(const VcsRenderPipelineDesc *desc);
    void (*render_destroy_pipeline)(VcsRenderPipeline *pipeline);
    /* A pass into one texture (viewport and scissor: the whole texture); the pass a hook leaves open is ended for it. */
    void (*render_begin_pass)(VcsRenderTexture *target);
    void (*render_set_viewport)(int x, int y, int width, int height); /* the scissor too */
    void (*render_set_pipeline)(VcsRenderPipeline *pipeline);
    void (*render_set_texture)(uint32_t slot, VcsRenderTexture *texture, int sampler); /* VCS_RENDER_SAMPLER_*; slots 0..7 */
    void (*render_set_constants)(uint32_t slot, VcsRenderBuffer *buffer);
    void (*render_draw)(uint32_t vertex_count);
    void (*render_end_pass)(void);
    void (*render_push_marker)(const char *name); /* GPU time per marker with PS2X_GPU_TIMING=1 (D3D11) */
    void (*render_pop_marker)(void);

    /* -- lit model geometry (appended 2026-10-31, API 1; check with VCS_MOD_API_HAS(api, set_lit_refiner)) -------- */
    /* One refiner (the last set wins), any thread; fn NULL removes it. Called for batches the native lit draws take
     * (not those through the VU's clipper). */
    void (*set_lit_refiner)(VcsLitRefineFn fn, void *user);

    /* -- temporal AA (appended 2026-10-31, API 1; check with VCS_MOD_API_HAS(api, render_set_jitter)) ------------- */
    /* The 3D picture's draws jittered by a Halton (2, 3) sequence of `phases` points, one per frame (0: off, the default);
     * the offsets reach the render hooks in VcsRenderFrame. Main thread, any time. */
    void (*render_set_jitter)(uint32_t phases);
    /* Inside a hook, with no pass open: the picture as it is now, single-sampled (what earlier hooks drew included). */
    VcsRenderTexture *(*render_current_color)(void);
    /* The graphics API's own objects for an SDK that takes them (NVIDIA NGX): VCS_RENDER_NATIVE_*; NULL where the backend
     * has none (Vulkan for now). Inside a hook; leave the device's state as found where possible. */
    void *(*render_native)(int what, VcsRenderTexture *texture);
    /* render_create_texture with VCS_RENDER_TEXTURE_* flags (STORAGE: written by such an SDK, e.g. DLSS's output). */
    VcsRenderTexture *(*render_create_texture_ex)(uint32_t width, uint32_t height, int format, uint32_t flags, const char *name);

    /* -- VR (appended 2026-11-01, API 1; check with VCS_MOD_API_HAS(api, present_window_copy)) -------------------------- */
    /* A present hook (VcsPresentFrame): main thread, any time; it stays until the game ends. 0 if refused. */
    int (*add_present_hook)(VcsPresentHookFn fn, void *user);
    /* The renderer keeps the picture without the HUD and the HUD alone for the present hooks (on = 1), or stops (0; the
     * default: nothing extra is drawn). Any thread. */
    void (*render_set_split_layers)(int on);
    /* Inside a present hook: the window's picture and menus (F2, F5, Dear ImGui) as shown, rendered off-screen at
     * width x height - D3D11: an ID3D11Texture2D * (RGBA8, sRGB-encoded), kept until the next call; NULL if none. */
    void *(*present_window_copy)(uint32_t width, uint32_t height);

    /* -- shadow maps (appended 2026-11-01, API 1; check with VCS_MOD_API_HAS(api, render_shadow_map)) ----------------- */
    /* Inside a hook added with VCS_RENDER_NEEDS_SHADOW, no pass open: the scene's native draws that write Z (alpha-tested
     * ones solid) drawn depth only into `count` (1..4) square viewports of `size` (64..4096) pixels
     * side by side - a depth texture of size * count x size, NULL if refused. view_to_clip: count row-major 4x4 matrices
     * from view space (VcsRenderFrame: x right, y down, w forward, metres) to clip space (x, y -1..1 with y up, z 0..1 with
     * larger = nearer the light; cleared to 0). Draws through the GS path (geometry crossing the screen's guard band) are
     * not in it; static world meshes seen before and now off screen are drawn too, where they were (peds and vehicles off
     * screen are not). `drawn` (may be NULL): draws drawn. */
    VcsRenderTexture *(*render_shadow_map)(const float *view_to_clip, uint32_t count, uint32_t size, uint32_t *drawn);

    /* -- several lit refiners (appended 2026-11-01, API 1; check with VCS_MOD_API_HAS(api, add_lit_refiner)) ---------- */
    /* Another refiner beside the one set_lit_refiner sets (a chain, e.g. WeaponHD.asi): the added ones are asked
     * first, in the order added; the first to hand back geometry draws the batch, the others are not asked for it.
     * At most 8; returns 0 when refused. Not removable. */
    int (*add_lit_refiner)(VcsLitRefineFn fn, void *user);

    /* -- VR pacing and render size (appended 2026-11-01, API 1; check with VCS_MOD_API_HAS(api, render_set_output_size)) */
    /* A headset paces the frames: hz > 0 runs the game at that rate (frame-rate independent timing) with the window's
     * own frame limiter and vertical sync out of the way (the present hook's xrWaitFrame paces the loop); 0 gives the
     * settings back. Main thread. */
    void (*vr_set_pacing)(float hz);
    /* The picture's output size (the scene / HUD layers; with "Match window" the render targets too) in place of the
     * window's: the headset's resolution x a render scale. 0 x 0 = the window's again. Any thread. */
    void (*render_set_output_size)(uint32_t width, uint32_t height);

    /* -- vegetation wind (appended 2026-11-02, API 1; check with VCS_MOD_API_HAS(api, render_set_wind_list)) ---------- */
    /* Native world draws whose texture a wind list names sway (gs_draw.hlsl vs_world): x, y the noise field's position
     * (metres; drift it for motion), strength in metres (0 = still, the default). Any thread, every frame. */
    void (*render_set_wind)(float x, float y, float strength);
    /* The wind list: lines "<PCSX2 texture name> mx my mz 0 reach" (metres per position unit, the reach in position
     * units; the port's tools/veg_convert.py writes them). VcsModRegister only - the renderer reads it when it starts;
     * 0 if too late or unreadable. */
    int (*render_set_wind_list)(const char *path);
} VcsModApi;

/* Whether this exe's table has `field` (functions appended after a mod was built are NULL-checked this way). */
#define VCS_MOD_API_HAS(api, field) ((api)->size >= offsetof(VcsModApi, field) + sizeof((api)->field))

/* The exported functions' types (the loader's GetProcAddress). */
typedef uint32_t (*VcsModApiVersionFn)(void);
typedef int (*VcsModRegisterFn)(const VcsModApi *api);
typedef int (*VcsModInstallFn)(const VcsModApi *api);
typedef const char *(*VcsModNameFn)(void);
typedef int (*VcsModRegisterSettingsFn)(const VcsModApi *api, const char *slot);

#ifdef __cplusplus
}
#endif

#endif /* VCS_MOD_API_H */
