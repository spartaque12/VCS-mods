// See mod.h: the API glue every mod links (log, settings, control commands, the exe's pad and free camera).
#include "mod.h"

#include <cstdarg>
#include <cstdio>
#include <memory>

namespace mod
{
    const VcsModApi *api = nullptr;

    void log(const char *format, ...)
    {
        char text[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        api->log(text);
    }

    namespace
    {
        // the Specs live as long as the game (their callbacks are called until it ends)
        std::vector<std::unique_ptr<Spec>> g_specs;

        void specChange(void *user, int v)
        {
            if (const Spec *s = static_cast<const Spec *>(user); s->onChange)
                s->onChange(v);
        }
        int specRead(void *user) { return static_cast<const Spec *>(user)->read(); }

        std::vector<std::unique_ptr<CommandHandler>> g_commands;

        void command(void *user, const char *args, char *answer, size_t size)
        {
            const CommandHandler &h = *static_cast<const CommandHandler *>(user);
            h(args ? args : "", [answer, size](const std::string &text) { std::snprintf(answer, size, "%s", text.c_str()); });
        }
    }

    void add(Spec spec)
    {
        g_specs.push_back(std::make_unique<Spec>(std::move(spec)));
        const Spec &s = *g_specs.back();
        std::vector<VcsModChoice> choices; // copied by the exe
        for (const Choice &c : s.choices)
            choices.push_back({c.label.c_str(), c.value});
        VcsModSetting c = {};
        c.size = sizeof(c);
        c.key = s.key.c_str();
        c.page = s.page.c_str();
        c.section = s.section.c_str();
        c.label = s.label.c_str();
        c.help = s.help.c_str();
        c.type = static_cast<int>(s.type);
        c.min = s.min;
        c.max = s.max;
        c.format = s.format.c_str();
        c.default_value = s.defaultValue;
        c.env = s.env.empty() ? nullptr : s.env.c_str();
        c.apply = VCS_APPLY_LIVE;
        c.choices = choices.empty() ? nullptr : choices.data();
        c.choice_count = static_cast<int>(choices.size());
        c.on_change = &specChange;
        c.read = s.read ? &specRead : nullptr;
        c.user = g_specs.back().get();
        if (!api->add_setting(&c))
            log("[mods] setting %s refused\n", s.key.c_str());
    }

    bool set(const std::string &key, int v) { return api->set_setting(key.c_str(), v) != 0; }

    void addCommand(const std::string &verb, CommandHandler handler)
    {
        g_commands.push_back(std::make_unique<CommandHandler>(std::move(handler)));
        api->add_command(verb.c_str(), &command, g_commands.back().get());
    }

    PadState lastPad()
    {
        VcsModPad p = {};
        api->last_pad(&p);
        return {p.buttons, p.lx, p.ly, p.rx, p.ry};
    }

    bool freecamFlying() { return api->freecam_flying() != 0; }
}
