// See saanims.h and docs/game-internals.md 12w.
#include "saanims.h"

#include "mod.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vcs::saanims
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        // CAnimManager: *(0x48F048) (made on first use by FUN_0046FE00); its +0xC -> +0x1013C the anim slots (0x28 each:
        // tracks, name[24], track count, duration, size); a track 12 bytes: u16 flags (2: translation), u16 key count,
        // keys pointer, u16 bone, u16 hash; keys half floats x y z w dt (+ x y z with translation) (12i)
        constexpr uint32_t kAnimManager = 0x0048F048u;
        constexpr uint32_t kMaxSlots = 1500u;

        std::atomic<bool> g_on{true};

        struct Level
        {
            float tol = 0;
            std::map<uint16_t, std::vector<std::array<float, 5>>> tracks; // bone -> (t 0..1, x y z w)
        };
        struct Anim
        {
            std::string name;
            float saDur = 0;
            std::vector<Level> levels;
        };
        bool g_loaded = false, g_loadTried = false;
        std::vector<Anim> g_file;

        struct Patched
        {
            uint32_t slot = 0, tracks = 0, count = 0, lo = 0;
            std::vector<uint8_t> origTracks, origKeys;
            int level = -1;
        };
        std::vector<Patched> g_patched; // applied (to restore)
        bool g_applied = false;
        uint32_t g_slots = 0; // the slot array the patch was made in
        std::mutex g_statusMutex;
        std::string g_status = "not yet";

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void setStatus(const std::string &s)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = s;
        }

        uint16_t toHalf(float f)
        {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            int32_t e = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
            uint32_t m = x & 0x7FFFFFu;
            if (e <= 0)
            {
                if (e < -10)
                    return static_cast<uint16_t>(sign);
                m |= 0x800000u;
                const uint32_t shift = static_cast<uint32_t>(14 - e);
                uint32_t h = m >> shift;
                if ((m >> (shift - 1)) & 1u)
                    ++h;
                return static_cast<uint16_t>(sign | h);
            }
            if (e >= 31)
                return static_cast<uint16_t>(sign | 0x7C00u);
            uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
            if (m & 0x1000u)
                ++h; // round to nearest
            return static_cast<uint16_t>(sign | h);
        }

        std::string modsDir()
        {
            if (const char *e = std::getenv("VCS_MODS_DIR"); e && *e)
                return e;
            const char *g = mod::api->game_dir ? mod::api->game_dir() : nullptr;
            return std::string(g ? g : ".") + "/mods";
        }

        bool load()
        {
            g_loadTried = true;
            const std::string path = modsDir() + "/anims/sa_armed.bin";
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
            {
                setStatus("no file " + path);
                return false;
            }
            std::vector<uint8_t> d;
            uint8_t buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
                d.insert(d.end(), buf, buf + n);
            std::fclose(f);
            size_t p = 0;
            auto need = [&](size_t n) { return p + n <= d.size(); };
            auto u32 = [&]() { uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; };
            auto u16 = [&]() { uint16_t v; std::memcpy(&v, &d[p], 2); p += 2; return v; };
            auto f32 = [&]() { float v; std::memcpy(&v, &d[p], 4); p += 4; return v; };
            if (!need(12) || std::memcmp(d.data(), "VSPA", 4) != 0)
            {
                setStatus("bad file " + path);
                return false;
            }
            p = 4;
            if (u32() != 1u)
            {
                setStatus("unknown version " + path);
                return false;
            }
            const uint32_t count = u32();
            for (uint32_t a = 0; a < count && count < 64; ++a)
            {
                if (!need(32))
                    return setStatus("short file"), false;
                Anim an;
                an.name.assign(reinterpret_cast<const char *>(&d[p]), strnlen(reinterpret_cast<const char *>(&d[p]), 24));
                p += 24;
                an.saDur = f32();
                const uint32_t levels = u32();
                for (uint32_t l = 0; l < levels && l < 16; ++l)
                {
                    if (!need(8))
                        return setStatus("short file"), false;
                    Level lv;
                    lv.tol = f32();
                    const uint32_t tracks = u32();
                    for (uint32_t t = 0; t < tracks && t < 64; ++t)
                    {
                        if (!need(4))
                            return setStatus("short file"), false;
                        const uint16_t bone = u16(), keys = u16();
                        if (!need(static_cast<size_t>(keys) * 20u))
                            return setStatus("short file"), false;
                        auto &v = lv.tracks[bone];
                        for (uint16_t k = 0; k < keys; ++k)
                        {
                            std::array<float, 5> key;
                            for (float &x : key)
                                x = f32();
                            v.push_back(key);
                        }
                    }
                    an.levels.push_back(std::move(lv));
                }
                g_file.push_back(std::move(an));
            }
            g_loaded = !g_file.empty();
            setStatus(g_loaded ? "loaded " + path : "empty " + path);
            return g_loaded;
        }

        uint32_t slotArray(const uint8_t *ram)
        {
            const uint32_t mgr = rd32(ram, kAnimManager);
            if (!mgr || mgr > kRamMask)
                return 0;
            const uint32_t p = rd32(ram, mgr + 0xCu);
            if (!p || p > kRamMask)
                return 0;
            const uint32_t s = rd32(ram, p + 0x1013Cu);
            return s && s < kRamMask ? s : 0;
        }

        uint32_t findSlot(const uint8_t *ram, uint32_t slots, const std::string &name)
        {
            for (uint32_t i = 0; i < kMaxSlots; ++i)
            {
                const uint32_t h = slots + i * 0x28u;
                const char *n = reinterpret_cast<const char *>(ram + ((h + 4u) & kRamMask));
                if (strnlen(n, 24) == name.size() && std::equal(name.begin(), name.end(), n,
                                                               [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
                    return h;
            }
            return 0;
        }

        void restore(uint8_t *ram)
        {
            for (const Patched &p : g_patched)
            {
                std::memcpy(ram + (p.tracks & kRamMask), p.origTracks.data(), p.origTracks.size());
                std::memcpy(ram + (p.lo & kRamMask), p.origKeys.data(), p.origKeys.size());
            }
            g_patched.clear();
            g_applied = false;
        }

        // One anim rewritten in place: its tracks' keys re-laid out in the space VCS's took (contiguous, each track's
        // keys on a word), SA's tracks for the bones the file has, VCS's kept for the others.
        bool patchOne(uint8_t *ram, uint32_t slot, const Anim &an, std::string &log)
        {
            const uint32_t tracks = rd32(ram, slot), count = rd32(ram, slot + 0x1Cu);
            const float dur = rdf(ram, slot + 0x20u);
            if (!tracks || count == 0 || count > 64 || !(dur > 0.0f && dur < 60.0f))
                return false;
            uint32_t lo = UINT32_MAX, hi = 0, used = 0;
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t t = tracks + 12u * k;
                const uint16_t flags = rd16(ram, t), n = rd16(ram, t + 2);
                const uint32_t kp = rd32(ram, t + 4), stride = (flags & 2u) ? 16u : 10u;
                lo = std::min(lo, kp);
                hi = std::max(hi, kp + n * stride);
                used += n * stride;
            }
            // the keys must be one block of this anim's (alignment padding only)
            if (lo >= hi || hi - lo > 0x10000u || hi - lo > used + 4u * count)
            {
                log += " " + an.name + ":not-contiguous";
                return false;
            }
            const uint32_t space = hi - lo;
            for (size_t li = 0; li < an.levels.size(); ++li)
            {
                const Level &lv = an.levels[li];
                std::vector<uint8_t> keys;
                std::vector<std::pair<uint16_t, uint32_t>> layout; // count, offset per track
                for (uint32_t k = 0; k < count; ++k)
                {
                    while (keys.size() % 4u)
                        keys.push_back(0);
                    const uint32_t t = tracks + 12u * k;
                    const uint16_t flags = rd16(ram, t), n = rd16(ram, t + 2), bone = rd16(ram, t + 8);
                    const uint32_t kp = rd32(ram, t + 4), stride = (flags & 2u) ? 16u : 10u;
                    const auto sa = lv.tracks.find(bone);
                    layout.push_back({0, static_cast<uint32_t>(keys.size())});
                    if (sa == lv.tracks.end() || (flags & 2u))
                    {
                        keys.insert(keys.end(), ram + (kp & kRamMask), ram + ((kp + n * stride) & kRamMask));
                        layout.back().first = n;
                        continue;
                    }
                    float last = 0.0f;
                    for (const auto &key : sa->second)
                    {
                        const float tt = std::clamp(key[0], 0.0f, 1.0f) * dur;
                        const uint16_t h[5] = {toHalf(key[1]), toHalf(key[2]), toHalf(key[3]), toHalf(key[4]),
                                               toHalf(std::max(tt - last, 0.0f))};
                        last = tt;
                        keys.insert(keys.end(), reinterpret_cast<const uint8_t *>(h), reinterpret_cast<const uint8_t *>(h) + 10);
                    }
                    layout.back().first = static_cast<uint16_t>(sa->second.size());
                }
                if (keys.size() > space)
                    continue;
                Patched p;
                p.slot = slot;
                p.tracks = tracks;
                p.count = count;
                p.lo = lo;
                p.level = static_cast<int>(li);
                p.origTracks.assign(ram + (tracks & kRamMask), ram + ((tracks + 12u * count) & kRamMask));
                p.origKeys.assign(ram + (lo & kRamMask), ram + (hi & kRamMask));
                std::memcpy(ram + (lo & kRamMask), keys.data(), keys.size());
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint32_t t = tracks + 12u * k;
                    const uint16_t n = layout[k].first;
                    const uint32_t kp = lo + layout[k].second;
                    std::memcpy(ram + ((t + 2) & kRamMask), &n, 2);
                    std::memcpy(ram + ((t + 4) & kRamMask), &kp, 4);
                }
                g_patched.push_back(std::move(p));
                char text[96];
                std::snprintf(text, sizeof(text), " %s:%.1fdeg,%u/%uB", an.name.c_str(), lv.tol,
                              static_cast<unsigned>(keys.size()), space);
                log += text;
                return true;
            }
            log += " " + an.name + ":no-fit";
            return false;
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "anims.sa_armed";
        s.page = "Game";
        s.section = "First person";
        s.label = "San Andreas armed anims";
        s.help = "Holding a rifle, shotgun or SMG two-handed, peds stand, walk and run with San Andreas' gun hold (the upper "
                 "body of its IDLE / WALK / run_armed anims), in third and first person. Needs mods\\anims\\sa_armed.bin, "
                 "made from your own GTA San Andreas' anim\\ped.ifp by VCS-mods' plugins/CameraAim/tools/sa_armed_anims.py. "
                 "Off: VCS's own.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_SA_ARMED_ANIMS";
        s.onChange = [](int v) { g_on = v != 0; };
        add(s);
    }

    void install()
    {
        mod::addCommand("saanims", [](const std::string &, mod::Reply reply) {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            reply("ok on=" + std::to_string(g_on ? 1 : 0) + " " + g_status);
        });
    }

    void update(uint8_t *ram)
    {
        const bool want = g_on.load(std::memory_order_relaxed);
        if (!want)
        {
            if (g_applied)
            {
                restore(ram);
                setStatus("restored");
            }
            return;
        }
        if (g_applied)
        {
            // the anims reloaded (a new slot array): our backups are stale
            if (slotArray(ram) != g_slots)
            {
                g_patched.clear();
                g_applied = false;
            }
            return;
        }
        if (!g_loaded && (g_loadTried || !load()))
            return;
        const uint32_t slots = slotArray(ram);
        if (!slots)
            return;
        std::string log = "applied";
        int done = 0;
        for (const Anim &an : g_file)
            if (const uint32_t slot = findSlot(ram, slots, an.name))
                done += patchOne(ram, slot, an, log) ? 1 : 0;
            else
                log += " " + an.name + ":missing";
        if (!done && log.find("missing") != std::string::npos && g_patched.empty())
            return; // the ped anims not loaded yet: again next update
        g_applied = true;
        g_slots = slots;
        setStatus(log);
        mod::log("CameraAim: SA armed anims%s\n", log.c_str() + 7);
    }
}
