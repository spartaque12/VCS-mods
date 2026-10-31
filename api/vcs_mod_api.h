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

    /* -- lit model geometry (appended 2026-10-31, API 1; check with VCS_MOD_API_HAS(api, set_lit_refiner)) -------- */
    /* One refiner (the last set wins), any thread; fn NULL removes it. Called for batches the native lit draws take
     * (not those through the VU's clipper). */
    void (*set_lit_refiner)(VcsLitRefineFn fn, void *user);

    /* -- VR (appended 2026-11-01, API 1; check with VCS_MOD_API_HAS(api, present_window_copy)) -------------------------- */
    /* A present hook (VcsPresentFrame): main thread, any time; it stays until the game ends. 0 if refused. */
    int (*add_present_hook)(VcsPresentHookFn fn, void *user);
    /* The renderer keeps the picture without the HUD and the HUD alone for the present hooks (on = 1), or stops (0; the
     * default: nothing extra is drawn). Any thread. */
    void (*render_set_split_layers)(int on);
    /* Inside a present hook: the window's picture and menus (F2, F5, Dear ImGui) as shown, rendered off-screen at
     * width x height - D3D11: an ID3D11Texture2D * (RGBA8, sRGB-encoded), kept until the next call; NULL if none. */
    void *(*present_window_copy)(uint32_t width, uint32_t height);
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
