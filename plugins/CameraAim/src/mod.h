// The plugin's side of the VCS mod API (api/vcs_mod_api.h): the API table, the guest context's registers, and C++
// helpers that turn the API's C callbacks into the std::function style the camera code was written in (nothing of
// the STL crosses into vcs.exe).
#pragma once

#include "vcs_mod_api.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace mod
{
    extern const VcsModApi *api; // set by VcsModRegister

    // ---- the guest context (the runtime's R5900Context, through the API's offsets) ------------------------------
    inline uint8_t *ctxBytes(VcsGuestContext *ctx) { return reinterpret_cast<uint8_t *>(ctx); }
    inline uint32_t gpr32(VcsGuestContext *ctx, int reg)
    {
        if (reg == 0)
            return 0;
        uint32_t v;
        std::memcpy(&v, ctxBytes(ctx) + api->ctx_gpr_offset + 16u * reg, 4);
        return v;
    }
    // the low 64 bits, sign-extended from 32 (the runtime's SET_GPR_U32 / SET_GPR_S32)
    inline void setGpr32(VcsGuestContext *ctx, int reg, uint32_t value)
    {
        if (reg == 0)
            return;
        const int64_t v = static_cast<int32_t>(value);
        std::memcpy(ctxBytes(ctx) + api->ctx_gpr_offset + 16u * reg, &v, 8);
    }
    inline void setPc(VcsGuestContext *ctx, uint32_t pc) { std::memcpy(ctxBytes(ctx) + api->ctx_pc_offset, &pc, 4); }
    inline float *fpr(VcsGuestContext *ctx) { return reinterpret_cast<float *>(ctxBytes(ctx) + api->ctx_fpr_offset); }

    // ---- the game's log ----------------------------------------------------------------------------------------
    void log(const char *format, ...)
#if defined(__clang__) || defined(__GNUC__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;

    // ---- settings (as src/settings/settings.h's Spec) ------------------------------------------------------------
    enum class Type
    {
        Toggle = VCS_SETTING_TOGGLE,
        Choice = VCS_SETTING_CHOICE,
        Integer = VCS_SETTING_INTEGER,
        Key = VCS_SETTING_KEY,
        Action = VCS_SETTING_ACTION,
    };
    struct Spec
    {
        std::string key, page, section, label, help;
        Type type = Type::Toggle;
        int defaultValue = 0;
        int min = 0, max = 1;
        std::string format = "%d";
        std::string env;
        std::function<void(int)> onChange;
        std::function<int()> read;
    };
    void add(Spec spec);                     // VcsModRegister only
    bool set(const std::string &key, int v); // false if refused (overridden by its environment variable)

    // ---- control-channel commands (as ps2control::addCommand; answered at once) ---------------------------------
    using Reply = std::function<void(const std::string &answer)>;
    using CommandHandler = std::function<void(const std::string &args, Reply reply)>;
    void addCommand(const std::string &verb, CommandHandler handler);

    // ---- the exe's state the camera reads -----------------------------------------------------------------------
    struct PadState // DualShock 2 buttons held (1 = held), sticks -1..1 (y down +)
    {
        uint16_t buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0;
    };
    PadState lastPad();
    bool freecamFlying();
}
