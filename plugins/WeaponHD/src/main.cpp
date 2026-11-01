// WeaponHD.asi: the converted weapons (the port's Immersive Weapons Pack conversion, tools/weapon_convert.py --hd) drawn
// with the pack's full meshes. The game streams and holds a small model of each weapon (the streaming heap has no room
// for more); the full mesh lives here, on the host, and replaces the small model's batches as the native lit draws take
// them (the mod API's lit refiner chain, add_lit_refiner). Each batch of a small model is found by a hash of its
// positions as the VU gets them; its share of the full mesh (the triangles nearest to it) is drawn in its place, with
// the same atlas (and its full-resolution PNG through the texture replacements). Batches the VU's clipper takes are not
// offered: those draw the small model's triangles. Data: <mods>/weaponhd/<model id>.whd (VCS_MODS_DIR or vcs.exe's
// folder's "mods"); VCS_WEAPONHD=0 turns it off.
#include "mod.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Batch
    {
        // positions in the batch's units (the small model's int16 space), unit normals, uv in the game's V2-8 units
        std::vector<float> pos, nrm, uv;
        std::vector<uint16_t> idx;
        std::vector<float> unrm, uuv; // as the refiner hands them over (normals of length 127, uv with a z)
        VcsLitRefined out{};
        uint64_t key = 0;
    };

    std::unordered_map<uint64_t, Batch> g_batches;
    std::mutex g_lock;
    unsigned g_models = 0;
    uint64_t g_drawn = 0;

    // FNV-1a 64 over the count and the int32 x, y, z of each position (tools/weapon_hd.py batch_hash)
    uint64_t hashBatch(const VcsLitBatch *b)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        auto feed = [&h](const void *p, size_t n) {
            const auto *c = static_cast<const uint8_t *>(p);
            for (size_t i = 0; i < n; ++i)
                h = (h ^ c[i]) * 0x100000001B3ull;
        };
        const uint32_t count = b->count;
        feed(&count, 4);
        for (uint32_t i = 0; i < count; ++i)
            feed(b->pos[i], 12);
        return h;
    }

    bool loadFile(const std::filesystem::path &path)
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t o = 0;
        auto take = [&](void *dst, size_t n) {
            if (o + n > d.size())
                return false;
            std::memcpy(dst, d.data() + o, n);
            o += n;
            return true;
        };
        char magic[4];
        uint32_t model = 0, count = 0;
        if (!take(magic, 4) || std::memcmp(magic, "WHD1", 4) != 0 || !take(&model, 4) || !take(&count, 4))
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t hash;
            uint32_t nv, ni;
            if (!take(&hash, 8) || !take(&nv, 4) || !take(&ni, 4) || nv > 65535u)
                return false;
            Batch b;
            b.pos.resize(nv * 3u);
            b.nrm.resize(nv * 3u);
            b.uv.resize(nv * 2u);
            b.idx.resize(ni);
            if (!take(b.pos.data(), nv * 12u) || !take(b.nrm.data(), nv * 12u) || !take(b.uv.data(), nv * 8u) ||
                !take(b.idx.data(), ni * 2u))
                return false;
            o += (4 - (o & 3)) & 3;
            if (nv == 0u || ni < 3u)
            { // a batch with no share of the full mesh: drawn as nothing (one degenerate triangle)
                b.pos.assign(3, 0.0f);
                b.nrm.assign({0.0f, 0.0f, 1.0f});
                b.uv.assign(2, 0.0f);
                b.idx.assign(3, 0);
            }
            b.key = hash ^ 0x5745415048440000ull;
            g_batches[hash] = std::move(b);
        }
        ++g_models;
        return true;
    }

    void loadAll()
    {
        const char *env = std::getenv("VCS_MODS_DIR");
        std::filesystem::path dir = env && *env ? std::filesystem::path(reinterpret_cast<const char8_t *>(env))
                                                : std::filesystem::path(reinterpret_cast<const char8_t *>(mod::api->game_dir())) / "mods";
        dir /= "weaponhd";
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".whd" && !loadFile(e.path()))
                mod::log("[weaponhd] %s: not a WeaponHD file\n", e.path().filename().string().c_str());
        mod::log("[weaponhd] %u models, %zu batches from %s\n", g_models, g_batches.size(), dir.string().c_str());
    }

    const VcsLitRefined *refine(void *, const VcsLitBatch *b)
    {
        if (b->kind != VCS_LIT_RIGID || b->count == 0u)
            return nullptr;
        const uint64_t h = hashBatch(b);
        std::lock_guard<std::mutex> guard(g_lock);
        const auto it = g_batches.find(h);
        if (it == g_batches.end())
            return nullptr;
        Batch &e = it->second;
        if (e.unrm.empty())
        {
            const size_t n = e.pos.size() / 3;
            e.unrm.resize(n * 3);
            e.uuv.resize(n * 3);
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k)
                {
                    e.unrm[i * 3 + k] = e.nrm[i * 3 + k] * 127.0f;
                    // the third uv lane as the batch has it (the packets' STROW fills it: 255, read by the shader)
                    e.uuv[i * 3 + k] = k < 2 ? e.uv[i * 2 + k] : static_cast<float>(b->uv[0][2]);
                }
            e.out.key = e.key;
            e.out.vertex_count = static_cast<uint32_t>(n);
            e.out.index_count = static_cast<uint32_t>(e.idx.size());
            e.out.pos = reinterpret_cast<const float(*)[3]>(e.pos.data());
            e.out.nrm = reinterpret_cast<const float(*)[3]>(e.unrm.data());
            e.out.uv = reinterpret_cast<const float(*)[3]>(e.uuv.data());
            e.out.weights = nullptr;
            e.out.indices = e.idx.data();
        }
        ++g_drawn;
        return &e.out;
    }
}

extern "C"
{
    VCS_MOD_EXPORT uint32_t VcsModApiVersion(void) { return VCS_MOD_API_VERSION; }

    VCS_MOD_EXPORT const char *VcsModName(void) { return "WeaponHD"; }

    VCS_MOD_EXPORT int VcsModRegister(const VcsModApi *a)
    {
        if (!a || a->version != VCS_MOD_API_VERSION || !VCS_MOD_API_HAS(a, add_lit_refiner))
        {
            if (a && a->log)
                a->log("[weaponhd] this vcs.exe's mod API cannot add a lit refiner: update vcs.exe\n");
            return 0;
        }
        mod::api = a;
        if (const char *e = std::getenv("VCS_WEAPONHD"); e && e[0] == '0')
            return 1;
        loadAll();
        if (!g_batches.empty() && !a->add_lit_refiner(&refine, nullptr))
            mod::log("[weaponhd] the exe took no more lit refiners\n");
        // weaponhd -> ok models= batches= drawn= (batches drawn from the full meshes so far)
        mod::addCommand("weaponhd", [](const std::string &, mod::Reply reply) {
            char text[128];
            std::lock_guard<std::mutex> guard(g_lock);
            std::snprintf(text, sizeof(text), "ok models=%u batches=%zu drawn=%llu", g_models, g_batches.size(),
                          static_cast<unsigned long long>(g_drawn));
            reply(text);
        });
        return 1;
    }
}
