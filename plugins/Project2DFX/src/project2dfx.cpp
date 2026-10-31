// See project2dfx.h and docs/game-internals.md 6l. Ported from ThirteenAG's Project2DFX for the PCSX2 fork
// (source/console/GTAVCS.PCSX2F.Project2DFX and source/console/shared, MIT - THIRD_PARTY_LICENSES.txt): the light
// selection, signal groups, stars and distant traffic follow the plugin line by line; what the plugin asks the game for
// (projection, sprites, render states, Im3D) is native here or a chained guest call.
#include "project2dfx.h"

#include "mod.h"
#include "ps2_float.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcs::p2dfx
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kT0 = 8, kT1 = 9, kS6 = 22, kSp = 29, kRa = 31;

        // Hooks and call sites (game-internals.md 6l)
        constexpr uint32_t kCoronasRender = 0x0027C418u;   // CCoronas::Render()
        constexpr uint32_t kAfterCoronas = 0x0021F270u;    // its return in the frame's 3D render (jal at 0x21F268, FUN_0021EFB8)
        constexpr uint32_t kContinue = 0x0027C41Cu;        // CCoronas::Render + 4: the chains' continuation (no entry there)
        constexpr uint32_t kFlushSprites = 0x0039FDE8u;    // CSprite::FlushSpriteBuffer()
        constexpr uint32_t kAfterStars = 0x0018D458u;      // its return after the stars in CClouds::Render (FUN_0018CE80)
        constexpr uint32_t kCalcScreenCoors = 0x0039FC48u; // CSprite::CalcScreenCoors(in, out, &w, &h, farClip t0)
        constexpr uint32_t kBufferedSprite = 0x0039FF48u;  // CSprite::RenderBufferedOneXLUSprite(f12-f16 x y z w h, r, g, b,
                                                           // intens a3, f17 recipz, a t0)
        constexpr uint32_t kRegisterCorona = 0x0027DD10u;  // CCoronas::RegisterCorona(id, r, g, b, a, pos t1, type t2, ...)
        // its returns in CTrafficLights::DisplayActualLight (FUN_00313480, the signal in $s6)
        constexpr uint32_t kSignalCoronas[] = {0x00313E04u, 0x00313EA4u, 0x003154A8u, 0x00315568u};
        constexpr uint32_t kFlightCeiling = 0x003EA618u;   // the flying controls' height penalty (f12 height, a1 type)
        constexpr uint32_t kSetFarClip = 0x0018F400u;      // RwCameraSetFarClipPlane(camera, f12)
        constexpr uint32_t kAfterFarClip = 0x0021EBB8u;    // its call in the frame's render (FUN_0021E950): 2000 every frame
        constexpr uint32_t kLodDistScale = 0x004887F8u;    // CRenderer::ms_lodDistScale (1.0; read by CCamera::Process only)
        constexpr uint32_t kRenderState = 0x00175198u;     // RwRenderStateSet(state, value)
        constexpr uint32_t kFlush2D = 0x003DFDD8u;         // flushes the temp buffers' 2D batch
        constexpr uint32_t kSetFog = 0x0023E720u;          // FUN_002F6268(0, 0x10, on), around the traffic as in the plugin
        constexpr uint32_t kIm3DTransform = 0x002FBFA8u;   // RwIm3DTransform(vertices, count, matrix)
        constexpr uint32_t kIm3DIndexed = 0x002FC2E8u;     // RwIm3DRenderIndexedPrimitive(type, indices, count)
        constexpr uint32_t kIm3DEnd = 0x002FC1F0u;         // RwIm3DEnd()

        // RwRenderStateSet's states (its jump table at 0x4A50A0)
        constexpr uint32_t kStateRaster = 1, kStateZTest = 4, kStateZWrite = 6, kStateSrcBlend = 8, kStateDestBlend = 9;

        // The game's temporary buffers (TempBufferVertices, 0x3000 bytes, then TempBufferRenderIndexList, 1024 indices):
        // the sprite buffer's quads, the 2D batch and Im3D users (plane trails) all take them
        constexpr uint32_t kTempVertices = 0x00709650u, kTempIndices = 0x0070C650u;
        constexpr unsigned kTempVertexBytes = 0x3000u, kTempIndexCount = 1024u;
        constexpr uint32_t kSpriteCount = 0x00489EC0u, kSpriteZFlag = 0x00489EC8u; // CSprite's buffered quads, its flush flag
        constexpr unsigned kSpriteCapacity = 96u;          // RenderBufferedOneXLUSprite flushes at 96
        constexpr uint32_t k2DVertexCount = 0x004CD624u;   // the 2D batch's vertices (FUN_003DFDD8 flushes them)

        // Camera and projection
        constexpr uint32_t kWorldOffset = 0x006FE980u;     // added to world points before the camera's matrices
        constexpr uint32_t kScreenMatrix = 0x00520A00u;    // world -> x, y in 640x448 pixels (after / w), w = depth; rows
        constexpr uint32_t kDepthMatrix = 0x00520A40u;     // its z and w columns give the GS depth (FUN_002F9F58)
        constexpr uint32_t kNearClip = 0x004CD130u, kFarClip = 0x004CD12Cu;
        constexpr uint32_t kAspect = 0x00487488u;
        constexpr uint32_t kCameraPosition = 0x006F44D0u + 0xA50u; // TheCamera's matrix position

        // Clock, weather, signals
        constexpr uint32_t kClockHours = 0x004CD148u, kClockMinutes = 0x004CD149u; // bytes
        constexpr uint32_t kTimeMs = 0x004CD104u;          // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kWind = 0x004CD2A4u;            // above 1.1 the signals blink amber (LightForCars1 / 2)
        constexpr uint32_t kGreenLights = 0x004883C0u;     // non-zero: every signal green
        constexpr uint32_t kCloudCover = 0x004CD14Cu, kOvercast = 0x004CD2C8u; // the plugin dims stars by the larger
        constexpr uint32_t kCoronaTexture = 0x00487758u;   // gpCoronaTexture[0], its raster at +0
        constexpr uint32_t kPacketBuffer = 0x00520F90u;    // the frame's DMA packet buffer (game-internals.md 6i)
        constexpr uint32_t kPacketHalf = 0xE6670u;

        // Distant traffic: ThePaths' packed road / water graph
        constexpr uint32_t kPaths = 0x00487490u;           // -> nodes at +0, count +16, link count (u16) +26, links +31648
        constexpr uint32_t kTrafficPaused = 0x00489F7Cu;   // non-zero: the plugin keeps no distant traffic

        // The plugin's budgets: each buffered sprite becomes six 48-byte vertices of DMA (plus a header per flush), and
        // 192 KB of the packet buffer stay for the HUD, text and menus after; distant traffic takes at most 256 KB.
        constexpr int64_t kSpriteBytes = 300, kReserve = 192 * 1024;
        constexpr unsigned kTrafficBytes = 256 * 1024;
        constexpr unsigned kVisibleLimit = 2048;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        float floatBits(uint32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        struct Vector3
        {
            float x, y, z;
        };

        Vector3 rdVector(const uint8_t *ram, uint32_t a) { return {rdf(ram, a), rdf(ram, a + 4), rdf(ram, a + 8)}; }

        // The plugin's console::LightData, 32 bytes, as p2dfx/lights.bin holds it (tools/p2dfx.py)
        struct Light
        {
            Vector3 position;
            float size, nativeRange, orientation;
            uint8_t r, g, b, a, blink, noDistance;
            uint16_t pad;
        };
        static_assert(sizeof(Light) == 32, "the table's record");

        float saturate(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }

        float bounded(float v, float low, float high, float fallback)
        {
            if (!std::isfinite(v))
                return fallback;
            return v < low ? low : v > high ? high : v;
        }

        float distanceSquared(const Vector3 &a, const Vector3 &b)
        {
            const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
            return x * x + y * y + z * z;
        }

        float nightIntensity(unsigned hour, unsigned minute)
        {
            const unsigned time = (hour % 24) * 60 + minute % 60;
            if (time >= 19 * 60)
                return float(time - 19 * 60) / 300.0f;
            if (time < 3 * 60)
                return 1.0f;
            return saturate(float(7 * 60 - int(time)) / 240.0f);
        }

        float starIntensity(unsigned hour, unsigned minute)
        {
            const unsigned time = (hour % 24) * 60 + minute % 60;
            if (time >= 23 * 60 || time < 5 * 60)
                return 1.0f;
            if (time >= 22 * 60)
                return float(time - 22 * 60) / 60.0f;
            return saturate(float(6 * 60 - int(time)) / 60.0f);
        }

        // One phase per blink mode a frame (smooth edges; light count and frame rate do not change the duty cycle)
        float blinkIntensity(unsigned mode, uint32_t ms)
        {
            constexpr uint16_t on[] = {0, 500, 1000, 2000, 3000, 4000, 5000, 6000};
            constexpr uint16_t off[] = {0, 500, 1000, 2000, 3000, 4000, 5000, 4000};
            if (!mode || mode >= std::size(on))
                return 1.0f;
            const uint32_t phase = ms % (on[mode] + off[mode]);
            float v = phase < on[mode] ? float(phase) / 250.0f : 1.0f - float(phase - on[mode]) / 250.0f;
            v = saturate(v);
            return v * v * (3.0f - 2.0f * v);
        }

        bool trafficLight(const Light &l) { return l.size == 0.45f; }

        // 0 green, 1 amber, 2 red, 3 none: the values CTrafficLights::LightForCars1 / 2 return
        unsigned trafficColor(const Light &l)
        {
            if (l.r >= 250 && l.g >= 100 && l.b <= 100)
                return 1;
            if (l.r >= 250 && l.g < 100 && l.b == 0)
                return 2;
            return l.r == 0 && l.g >= 250 && l.b == 0 ? 0 : 3;
        }

        // CTrafficLights::LightForCars1 / 2 (FUN_00313390 / FUN_003133F0 -> FUN_003132B0 / FUN_00313318)
        unsigned lightForCars(const uint8_t *ram, bool second)
        {
            const uint32_t ms = rd32(ram, kTimeMs);
            if (floatBits(0x3F8CCCCDu) < rdf(ram, kWind)) // 1.1
                return (ms & 0x400u) ? 3u : 1u;
            if (rd32(ram, kGreenLights) != 0)
                return 0;
            const uint32_t t = (ms & 0x7FFFu) >> 1;
            if (!second)
                return t < 5000 ? 0u : t < 6000 ? 1u : 2u;
            return t < 6000 ? 2u : t < 11000 ? 0u : t < 12000 ? 1u : 2u;
        }

        // CTrafficLights::FindTrafficLightType (FUN_003155C0): the signal's facing from its forward vector
        unsigned findTrafficLightType(float forwardX, float forwardY)
        {
            float angle = std::atan2(forwardY, forwardX);
            if (angle < 0.0f)
                angle += 6.2831855f;
            const float degrees = angle * 57.29578f;
            return (60.0f < degrees && degrees < 150.0f) || (240.0f < degrees && degrees < 330.0f) ? 1u : 2u;
        }

        // ---- the light table and its index ----

        // Cells of 256 units hashed into 1024 buckets; buckets keep the cell coordinates, so collisions never duplicate
        // or omit candidates (the plugin's console::LightGrid)
        class LightGrid
        {
            static constexpr uint16_t kEnd = 65535;
            static constexpr size_t kBuckets = 1024;
            static constexpr float kCell = 256.0f;
            struct Link
            {
                uint16_t next;
                int16_t x, y;
            };
            uint16_t heads_[kBuckets]{};
            std::vector<Link> links_;

            static int cell(float v)
            {
                const float scaled = v / kCell;
                const int truncated = int(scaled);
                return truncated - (scaled < float(truncated));
            }
            static size_t hash(int x, int y) { return (uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u) & (kBuckets - 1); }

        public:
            void build(const std::vector<Light> &lights)
            {
                std::fill(std::begin(heads_), std::end(heads_), kEnd);
                links_.resize(lights.size());
                for (size_t i = 0; i < lights.size(); ++i)
                {
                    const int x = cell(lights[i].position.x), y = cell(lights[i].position.y);
                    const size_t bucket = hash(x, y);
                    links_[i] = {heads_[bucket], int16_t(x), int16_t(y)};
                    heads_[bucket] = uint16_t(i);
                }
            }

            template <class Visitor> void visit(const Vector3 &center, float range, Visitor visitor) const
            {
                // a corrupted camera must not turn this into an unbounded walk or a float-to-int overflow
                if (links_.empty() || !(range >= 0.0f && range <= 8192.0f) ||
                    !(center.x > -100000.0f && center.x < 100000.0f && center.y > -100000.0f && center.y < 100000.0f))
                    return;
                const int minX = cell(center.x - range), maxX = cell(center.x + range);
                const int minY = cell(center.y - range), maxY = cell(center.y + range);
                for (int y = minY; y <= maxY; ++y)
                    for (int x = minX; x <= maxX; ++x)
                        for (uint16_t i = heads_[hash(x, y)]; i != kEnd; i = links_[i].next)
                            if (links_[i].x == x && links_[i].y == y)
                                visitor(i);
            }
        };

        std::vector<Light> g_lights;
        std::vector<uint8_t> g_groups; // per light: the signal group (1 / 2: LightForCars1 / 2)
        LightGrid g_grid;

        std::filesystem::path exeDir()
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (n && n < MAX_PATH)
                return std::filesystem::path(buffer).parent_path();
#else
            std::error_code ec;
            const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec)
                return p.parent_path();
#endif
            return std::filesystem::current_path();
        }

        // The plugin's TrafficGroup: the table keeps the IPL orientation, not a heading, so the signal's direction comes
        // from its red-to-green lamp pair; opposite faces share a phase. The native FindTrafficLightType sectors.
        unsigned trafficGroup(float redToGreenX, float redToGreenY)
        {
            float x = redToGreenY, y = -redToGreenX;
            if (y < 0.0f)
            {
                x = -x;
                y = -y;
            }
            return 0.5f * y - 0.8660254f * x > 0.0f && -0.8660254f * y - 0.5f * x < 0.0f ? 1u : 2u;
        }

        bool loadLights()
        {
            const char *env = std::getenv("VCS_P2DFX_LIGHTS");
            const std::filesystem::path file = env && *env ? std::filesystem::path(env) : exeDir() / "p2dfx" / "lights.bin";
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                mod::log("[p2dfx] no light table at %s: distant lights need python tools/p2dfx.py import "
                                     "<GTAVCS.PCSX2F.Project2DFX.zip>\n", file.string().c_str());
                return false;
            }
            const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            uint32_t version = 0, count = 0;
            if (data.size() >= 16)
            {
                std::memcpy(&version, data.data() + 8, 4);
                std::memcpy(&count, data.data() + 12, 4);
            }
            if (data.size() < 16 || std::memcmp(data.data(), "VCSP2DFX", 8) != 0 || version != 1 || !count || count >= 65535 ||
                data.size() != 16 + size_t(count) * sizeof(Light))
            {
                mod::log("[p2dfx] %s is not a light table of this version: import it again (tools/p2dfx.py)\n",
                             file.string().c_str());
                return false;
            }
            g_lights.resize(count);
            std::memcpy(g_lights.data(), data.data() + 16, size_t(count) * sizeof(Light));
            g_grid.build(g_lights);
            // each signal lamp's group from its pair of the opposite colour in its small cluster
            g_groups.assign(count, 0);
            for (size_t i = 0; i < count; ++i)
            {
                const Light &light = g_lights[i];
                if (!trafficLight(light))
                    continue;
                float nearest = 1.0f;
                size_t pair = i;
                const unsigned own = trafficColor(light);
                g_grid.visit(light.position, 1.0f, [&](uint16_t c) {
                    const Light &other = g_lights[c];
                    if (!trafficLight(other) || other.orientation != light.orientation)
                        return;
                    const unsigned color = trafficColor(other);
                    if ((own == 0 && color != 2) || (own != 0 && color != 0))
                        return;
                    const float d = distanceSquared(light.position, other.position);
                    if (d < nearest)
                    {
                        nearest = d;
                        pair = c;
                    }
                });
                g_groups[i] = uint8_t(trafficGroup(g_lights[pair].position.x - light.position.x,
                                                   g_lights[pair].position.y - light.position.y));
            }
            mod::log("[p2dfx] %u lights from %s\n", count, file.string().c_str());
            return true;
        }

        // ---- settings (atomics: the menu writes, the game thread reads) ----

        std::atomic<bool> g_lightsOn{false}, g_starsOn{false}, g_carsOn{false}, g_boatsOn{false}, g_ceilingOn{false};
        std::atomic<int> g_range{1000}, g_limit{900}, g_sizePercent{150};
        std::atomic<int> g_trafficRange{800}, g_maxCars{40}, g_maxBoats{8};
        std::atomic<int> g_drawPercent{100}, g_lodPercent{100}; // draw distance, LOD distance (100: the original)

        // ---- the game's projection and sprite writer, natively ----

        __m128 rdVec4(const uint8_t *ram, uint32_t a)
        {
            float v[4];
            std::memcpy(v, ram + (a & kRamMask), 16);
            return _mm_loadu_ps(v);
        }

        // CSprite::CalcScreenCoors (FUN_0039FC48) in the game's arithmetic: the point plus RSL's world offset through the
        // screen matrix (FUN_002F9E70: FUN_001AAA38's VU0 multiply-add, then x and y scaled by 1 / w), out.z = w; rejected
        // nearer than the near plane (and past the far plane with farClip); h = 448 / w, w = h x 1.4285715 / aspect.
        bool project(const uint8_t *ram, const Vector3 &in, Vector3 &out, float &width, float &height, bool farClip = false)
        {
            const float x = FPU_ADD_S(in.x, rdf(ram, kWorldOffset));
            const float y = FPU_ADD_S(in.y, rdf(ram, kWorldOffset + 4));
            const float z = FPU_ADD_S(in.z, rdf(ram, kWorldOffset + 8));
            __m128 acc = PS2_VMUL(rdVec4(ram, kScreenMatrix), _mm_set1_ps(x));
            acc = PS2_VADD(acc, PS2_VMUL(rdVec4(ram, kScreenMatrix + 0x10), _mm_set1_ps(y)));
            acc = PS2_VADD(acc, PS2_VMUL(rdVec4(ram, kScreenMatrix + 0x20), _mm_set1_ps(z)));
            const __m128 v = PS2_VADD(acc, PS2_VMUL(rdVec4(ram, kScreenMatrix + 0x30), _mm_set1_ps(1.0f)));
            float p[4];
            _mm_storeu_ps(p, v);
            if (p[3] <= 0.0f)
            {
                out = {0.0f, 0.0f, 0.0f};
                return false;
            }
            const float recip = FPU_DIV_S(1.0f, p[3]);
            out = {FPU_MUL_S(p[0], recip), FPU_MUL_S(p[1], recip), p[3]};
            if (out.z < rdf(ram, kNearClip))
                return false;
            if (!(out.z < rdf(ram, kFarClip)) && farClip)
                return false;
            height = FPU_MUL_S(FPU_DIV_S(1.0f, out.z), 448.0f);
            width = FPU_DIV_S(FPU_MUL_S(height, floatBits(0x3FB6DB6Eu)), rdf(ram, kAspect));
            return true;
        }

        // One buffered sprite as RenderBufferedOneXLUSprite (FUN_0039FF48) writes it: four Im2D vertices (u, v u16 | RGBA |
        // x, y s16 | z u32) - corners clamped to the 640x448 screen with their u / v moved to match, the colour scaled by
        // intens / 256, z through FUN_0039FBF0 - and six indices.
        struct Quad
        {
            uint8_t bytes[64];
        };

        uint32_t spriteDepth(const uint8_t *ram, float z) // FUN_0039FBF0 -> FUN_002F9F58 -> FUN_0030C984
        {
            const float depth = FPU_DIV_S(FPU_ADD_S(FPU_MUL_S(rdf(ram, kDepthMatrix + 0x28), z), rdf(ram, kDepthMatrix + 0x38)),
                                          FPU_MUL_S(rdf(ram, kDepthMatrix + 0x2C), z));
            const uint32_t shifted = static_cast<uint32_t>(FPU_CVT_W_S(depth)) >> 4;
            return static_cast<uint32_t>(FPU_CVT_W_S(FPU_CVT_S_W(static_cast<int32_t>(shifted))));
        }

        void spriteQuad(const uint8_t *ram, float x, float y, float z, float w, float h, uint32_t r, uint32_t g, uint32_t b,
                        int16_t intens, uint32_t a, Quad &q)
        {
            const float right = FPU_ADD_S(x, w), top = FPU_ADD_S(y, h), left = FPU_SUB_S(x, w), bottom = FPU_SUB_S(y, h);
            float xs[4] = {left, left, right, right}, ys[4] = {bottom, top, top, bottom};
            float us[4] = {0.0f, 0.0f, 1.0f, 1.0f}, vs[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            for (int i = 0; i < 4; ++i)
            {
                if (xs[i] < 0.0f)
                {
                    us[i] = FPU_MUL_S(FPU_DIV_S(xs[i], w), -0.5f);
                    xs[i] = 0.0f;
                }
                if (640.0f < xs[i])
                {
                    us[i] = FPU_SUB_S(1.0f, FPU_DIV_S(FPU_MUL_S(FPU_SUB_S(xs[i], 640.0f), 0.5f), w));
                    xs[i] = 640.0f;
                }
                if (ys[i] < 0.0f)
                {
                    vs[i] = FPU_MUL_S(FPU_DIV_S(ys[i], h), -0.5f);
                    ys[i] = 0.0f;
                }
                if (448.0f < ys[i])
                {
                    vs[i] = FPU_SUB_S(1.0f, FPU_DIV_S(FPU_MUL_S(FPU_SUB_S(ys[i], 448.0f), 0.5f), h));
                    ys[i] = 448.0f;
                }
            }
            const uint32_t depth = spriteDepth(ram, z);
            const int32_t s = intens;
            const uint8_t rgba[4] = {uint8_t((int32_t(r & 0xFFu) * s) >> 8), uint8_t((int32_t(g & 0xFFu) * s) >> 8),
                                     uint8_t((int32_t(b & 0xFFu) * s) >> 8), uint8_t(a)};
            for (int i = 0; i < 4; ++i)
            {
                uint8_t *v = q.bytes + i * 16;
                const uint16_t u16 = uint16_t(FPU_CVT_W_S(FPU_ADD_S(FPU_MUL_S(us[i], 32768.0f), 0.5f)));
                const uint16_t v16 = uint16_t(FPU_CVT_W_S(FPU_ADD_S(FPU_MUL_S(vs[i], 32768.0f), 0.5f)));
                const int16_t px = int16_t(FPU_CVT_W_S(xs[i])), py = int16_t(FPU_CVT_W_S(ys[i]));
                std::memcpy(v, &u16, 2);
                std::memcpy(v + 2, &v16, 2);
                std::memcpy(v + 4, rgba, 4);
                std::memcpy(v + 8, &px, 2);
                std::memcpy(v + 10, &py, 2);
                std::memcpy(v + 12, &depth, 4);
            }
        }

        void spriteIndices(uint32_t slot, uint16_t out[6])
        {
            const uint16_t base = uint16_t(slot * 4u);
            const uint16_t v[6] = {base, uint16_t(base + 1), uint16_t(base + 2), uint16_t(base + 3), base, uint16_t(base + 2)};
            std::memcpy(out, v, sizeof(v));
        }

        // FUN_00478628: the packet buffer's free bytes (0xE6670 less FUN_00476770's use of both ends)
        int64_t packetFree(const uint8_t *ram)
        {
            const uint32_t start = kPacketBuffer + rd32(ram, kPacketBuffer + 0x14u) * kPacketHalf + 0x20u;
            const int32_t up = (int32_t(rd32(ram, kPacketBuffer + 0x18u) - (start - 0x10u)) >> 4) << 4;
            const int32_t down = int32_t(start + kPacketHalf - rd32(ram, kPacketBuffer + 0x1Cu));
            return int64_t(kPacketHalf) - int64_t(up + down);
        }

        unsigned spriteBudget(const uint8_t *ram)
        {
            const int64_t free = packetFree(ram);
            return free > kReserve ? unsigned((free - kReserve) / kSpriteBytes) : 0u;
        }

        uint32_t coronaRaster(const uint8_t *ram)
        {
            const uint32_t texture = rd32(ram, kCoronaTexture);
            return texture ? rd32(ram, texture) : 0u;
        }

        Vector3 cameraPosition(const uint8_t *ram) { return rdVector(ram, kCameraPosition); }
        unsigned hour(const uint8_t *ram) { return ram[kClockHours]; }
        unsigned minute(const uint8_t *ram) { return ram[kClockMinutes]; }

        // ---- distant traffic (the plugin's console::traffic: Paths, Population, FarRenderer) ----

        struct Node // ThePaths' packed node, 10 bytes
        {
            int16_t x, y;
            int8_t z;
            uint8_t width;
            int16_t firstLink;
            uint16_t flags;
            Vector3 position() const { return {x / 8.0f, y / 8.0f, float(z < 0 ? int(z) - 100 : int(z))}; }
            bool water() const { return flags & 0x100; }
            bool disabled() const { return flags & 0x20; }
            unsigned degree() const { return flags & 15; }
        };
        static_assert(sizeof(Node) == 10, "ThePaths' node");

        struct Paths // a read-only view of the game's graph
        {
            const uint8_t *ram = nullptr;
            uint32_t nodes = 0, links = 0;
            unsigned count = 0, linkCount = 0;

            Node node(unsigned n) const
            {
                Node v;
                std::memcpy(&v, ram + ((nodes + n * 10u) & kRamMask), sizeof(v));
                return v;
            }
            unsigned link(unsigned i) const
            {
                uint16_t v;
                std::memcpy(&v, ram + ((links + i * 2u) & kRamMask), 2);
                return v;
            }
            bool valid(unsigned n, bool water) const
            {
                if (!nodes || !links || n >= count)
                    return false;
                const Node p = node(n);
                return !p.disabled() && p.water() == water && p.degree() && p.firstLink >= 0 &&
                       unsigned(p.firstLink) <= linkCount && p.degree() <= linkCount - unsigned(p.firstLink);
            }
            unsigned next(unsigned from, unsigned previous, bool water, uint32_t seed) const
            {
                if (!valid(from, water))
                    return count;
                const Node p = node(from);
                for (unsigned i = 0; i < p.degree(); ++i)
                {
                    const unsigned to = link(unsigned(p.firstLink) + (seed + i) % p.degree()) & 0x3FFFu;
                    if (to != previous && to != from && valid(to, water))
                        return to;
                }
                return valid(previous, water) ? previous : count;
            }
        };

        struct Vehicle
        {
            unsigned from = 0, to = 0, previous = 0;
            uint32_t seed = 0;
            float along = 0, age = 0;
            Vector3 position{}, forward{};
            float alpha = 0;
            bool active = false, water = false;
        };

        class Population
        {
            uint32_t seed_ = 0x56435332, stamp_ = 0;
            unsigned cursor_ = 0;
            uint32_t graph_ = 0;
            uint32_t random()
            {
                seed_ ^= seed_ << 13;
                seed_ ^= seed_ >> 17;
                seed_ ^= seed_ << 5;
                return seed_;
            }

        public:
            static constexpr unsigned capacity = 64;
            Vehicle vehicles[capacity]{};
            float range = 800;
            unsigned carLimit = 40, boatLimit = 8;

            void clear()
            {
                for (Vehicle &v : vehicles)
                    v.active = false;
                graph_ = 0;
                stamp_ = 0;
                cursor_ = 0;
            }

            void update(const Paths &paths, const Vector3 &camera, uint32_t now, bool enabled)
            {
                if (!enabled || !paths.nodes || !paths.links || !paths.count || paths.count > 65535)
                {
                    clear();
                    return;
                }
                if (graph_ != paths.nodes)
                {
                    clear();
                    graph_ = paths.nodes;
                    stamp_ = now;
                }
                const unsigned elapsed = now - stamp_;
                stamp_ = now;
                if (!elapsed)
                    return;
                const float dt = float(elapsed > 100 ? 100 : elapsed) * 0.001f;
                unsigned cars = 0, boats = 0;
                for (Vehicle &v : vehicles)
                {
                    if (!v.active)
                        continue;
                    if (!paths.valid(v.from, v.water) || !paths.valid(v.to, v.water))
                    {
                        v.active = false;
                        continue;
                    }
                    v.age += dt;
                    v.along += dt * (v.water ? 7.0f : 13.0f);
                    for (unsigned hops = 0; hops < 4; ++hops) // a frame cannot cross an unbounded number of short edges
                    {
                        const Vector3 a = paths.node(v.from).position(), b = paths.node(v.to).position();
                        const float dx = b.x - a.x, dy = b.y - a.y;
                        const float length = std::sqrt(dx * dx + dy * dy);
                        if (!(length > 1.0f))
                        {
                            v.active = false;
                            break;
                        }
                        if (v.along < length)
                        {
                            const float t = v.along / length;
                            v.forward = {dx / length, dy / length, 0};
                            const float lane = v.water ? 0.0f : 1.5f;
                            v.position = {a.x + dx * t + v.forward.y * lane, a.y + dy * t - v.forward.x * lane, a.z + (b.z - a.z) * t};
                            break;
                        }
                        v.along -= length;
                        const unsigned next = paths.next(v.to, v.from, v.water, random());
                        v.previous = v.from;
                        v.from = v.to;
                        v.to = next;
                        if (next >= paths.count || hops == 3)
                        {
                            v.active = false;
                            break;
                        }
                    }
                    if (!v.active)
                        continue;
                    const float dx = v.position.x - camera.x, dy = v.position.y - camera.y;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance < 300 || distance > range + 100)
                    {
                        v.active = false;
                        continue;
                    }
                    v.alpha = saturate(v.age / 1.2f) * saturate((distance - 300) / 100) * saturate((range - distance) / 100);
                    v.water ? ++boats : ++cars;
                }
                unsigned spawned = 0;
                for (unsigned scan = 0; scan < 256 && spawned < 4; ++scan) // a bounded slice, even far from any node
                {
                    const unsigned n = cursor_++ % paths.count;
                    const bool water = paths.node(n).water();
                    if ((water ? boats >= boatLimit : cars >= carLimit) || !paths.valid(n, water))
                        continue;
                    const Vector3 p = paths.node(n).position();
                    const float dx = p.x - camera.x, dy = p.y - camera.y, d2 = dx * dx + dy * dy;
                    if (d2 < 400 * 400 || d2 > (range - 50) * (range - 50))
                        continue;
                    const unsigned next = paths.next(n, paths.count, water, random());
                    if (next >= paths.count)
                        continue;
                    bool occupied = false;
                    for (const Vehicle &v : vehicles)
                        if (v.active && v.water == water)
                        {
                            const float x = v.position.x - p.x, y = v.position.y - p.y;
                            if (x * x + y * y < 60 * 60)
                            {
                                occupied = true;
                                break;
                            }
                        }
                    if (occupied)
                        continue;
                    for (Vehicle &v : vehicles)
                        if (!v.active)
                        {
                            v = {};
                            v.active = true;
                            v.water = water;
                            v.from = n;
                            v.to = next;
                            v.previous = paths.count;
                            v.seed = random();
                            v.position = p;
                            water ? ++boats : ++cars;
                            ++spawned;
                            break;
                        }
                }
            }
        };

        enum Material : uint8_t { Paint, Glass, Rubber, Chrome, Headlight, Taillight, BoatHull };

        struct FarFace
        {
            Vector3 normal;
            uint8_t first, count; // polygon corners (3..5)
            Material material;
            uint8_t level;        // 0 always, 1 cabin (nearer vehicles), 2 lamp (night)
        };

        // The plugin's far_mesh: metres, +Y forward, +Z up. A car is a body box without a bottom, a cabin and head / tail
        // lamp quads just outside the front and back; a boat a pointed hull prism and a cabin box.
        constexpr Vector3 kCarCorners[] = {
            {-0.90f, -2.30f, 0.30f}, {0.90f, -2.30f, 0.30f}, {0.90f, 2.30f, 0.30f}, {-0.90f, 2.30f, 0.30f},
            {-0.90f, -2.30f, 0.85f}, {0.90f, -2.30f, 0.85f}, {0.90f, 2.30f, 0.85f}, {-0.90f, 2.30f, 0.85f},
            {-0.80f, -1.20f, 0.85f}, {0.80f, -1.20f, 0.85f}, {0.80f, 0.90f, 0.85f}, {-0.80f, 0.90f, 0.85f},
            {-0.70f, -0.95f, 1.32f}, {0.70f, -0.95f, 1.32f}, {0.70f, 0.45f, 1.32f}, {-0.70f, 0.45f, 1.32f},
            {-0.80f, 2.32f, 0.50f}, {0.80f, 2.32f, 0.50f}, {0.80f, 2.32f, 0.72f}, {-0.80f, 2.32f, 0.72f},
            {0.80f, -2.32f, 0.52f}, {-0.80f, -2.32f, 0.52f}, {-0.80f, -2.32f, 0.74f}, {0.80f, -2.32f, 0.74f},
        };
        constexpr uint8_t kCarPolygons[] = {
            0, 3, 7, 4, 1, 5, 6, 2, 3, 2, 6, 7, 0, 4, 5, 1, 4, 7, 6, 5,
            8, 11, 15, 12, 9, 13, 14, 10, 11, 10, 14, 15, 8, 12, 13, 9, 12, 15, 14, 13,
            16, 17, 18, 19, 20, 21, 22, 23,
        };
        constexpr FarFace kCarFaces[] = {
            {{-1, 0, 0}, 0, 4, Paint, 0},          {{1, 0, 0}, 4, 4, Paint, 0},           {{0, 1, 0}, 8, 4, Paint, 0},
            {{0, -1, 0}, 12, 4, Paint, 0},         {{0, 0, 1}, 16, 4, Paint, 0},          {{-0.98f, 0, 0.2f}, 20, 4, Glass, 1},
            {{0.98f, 0, 0.2f}, 24, 4, Glass, 1},   {{0, 0.75f, 0.66f}, 28, 4, Glass, 1},  {{0, -0.7f, 0.71f}, 32, 4, Glass, 1},
            {{0, 0, 1}, 36, 4, Paint, 1},          {{0, 1, 0}, 40, 4, Headlight, 2},      {{0, -1, 0}, 44, 4, Taillight, 2},
        };
        constexpr Vector3 kBoatCorners[] = {
            {-1.00f, -3.00f, 0.00f}, {1.00f, -3.00f, 0.00f}, {1.00f, 1.20f, 0.00f}, {0.00f, 3.60f, 0.10f}, {-1.00f, 1.20f, 0.00f},
            {-1.25f, -3.00f, 0.90f}, {1.25f, -3.00f, 0.90f}, {1.25f, 1.20f, 0.95f}, {0.00f, 4.20f, 1.10f}, {-1.25f, 1.20f, 0.95f},
            {-0.80f, -1.80f, 0.92f}, {0.80f, -1.80f, 0.92f}, {0.80f, 0.60f, 0.95f}, {-0.80f, 0.60f, 0.95f},
            {-0.75f, -1.70f, 1.85f}, {0.75f, -1.70f, 1.85f}, {0.75f, 0.30f, 1.85f}, {-0.75f, 0.30f, 1.85f},
        };
        constexpr uint8_t kBoatPolygons[] = {
            0, 4, 9, 5, 4, 3, 8, 9, 3, 2, 7, 8, 2, 1, 6, 7, 1, 0, 5, 6, 5, 9, 8, 7, 6,
            10, 13, 17, 14, 11, 15, 16, 12, 13, 12, 16, 17, 10, 14, 15, 11, 14, 17, 16, 15,
        };
        constexpr FarFace kBoatFaces[] = {
            {{-1, 0, 0}, 0, 4, BoatHull, 0},  {{-0.92f, 0.38f, 0}, 4, 4, BoatHull, 0}, {{0.92f, 0.38f, 0}, 8, 4, BoatHull, 0},
            {{1, 0, 0}, 12, 4, BoatHull, 0},  {{0, -1, 0}, 16, 4, BoatHull, 0},        {{0, 0, 1}, 20, 5, Chrome, 0},
            {{-1, 0, 0}, 25, 4, Chrome, 1},   {{1, 0, 0}, 29, 4, Chrome, 1},           {{0, 1, 0}, 33, 4, Glass, 1},
            {{0, -1, 0}, 37, 4, Chrome, 1},   {{0, 0, 1}, 41, 4, Chrome, 1},
        };

        // Im3D's vertex (24 bytes): position, u, v, RGBA
        struct Vertex3D
        {
            Vector3 position;
            float u, v;
            uint32_t color;
        };
        static_assert(sizeof(Vertex3D) == 24, "Im3D's vertex");

        // The plugin's traffic::vertex: a corner placed on the vehicle (scale 1.2 cars, 0.8 boats), coloured by material
        // and lit by the face normal (emissive lamps at night), the vehicle's fade in alpha
        Vertex3D vehicleVertex(const Vector3 &p, const Vector3 &normal, Material material, const Vehicle &vehicle, float night)
        {
            const float scale = vehicle.water ? 0.8f : 1.2f;
            const Vector3 position{vehicle.position.x + scale * (vehicle.forward.y * p.x + vehicle.forward.x * p.y),
                                   vehicle.position.y + scale * (-vehicle.forward.x * p.x + vehicle.forward.y * p.y),
                                   vehicle.position.z + scale * p.z};
            static constexpr uint32_t paints[] = {0x304C70, 0x647E88, 0xA4A4A0, 0x38383C, 0x807058, 0x305048, 0x782C30, 0x907028};
            uint32_t color = 0xFFFFFF;
            bool emissive = false;
            switch (material)
            {
            case Paint: color = paints[vehicle.seed % 8]; break;
            case Glass: color = 0x303848; break;
            case Rubber: color = 0x181818; break;
            case Chrome: color = 0xB0B0B0; break;
            case Headlight:
                color = night > 0.2f ? 0xD8E8FF : 0x989898;
                emissive = night > 0.2f;
                break;
            case Taillight:
                color = 0x2020B0;
                emissive = true;
                break;
            default: break;
            }
            const float lighting = emissive ? 1.0f : (0.45f + 0.4f * saturate(normal.z * 0.6f + 0.4f)) * (1.0f - 0.65f * night);
            const uint32_t r = unsigned((color & 255) * lighting), g = unsigned(((color >> 8) & 255) * lighting),
                           b = unsigned(((color >> 16) & 255) * lighting);
            return {position, 0.0f, 0.0f, r | (g << 8) | (b << 16) | (unsigned(vehicle.alpha * 255) << 24)};
        }

        // ---- the chains of guest calls ----

        enum class Op : uint8_t
        {
            Flush2D,      // FUN_003DFDD8 when the 2D batch holds anything (it shares the temp buffers)
            FlushSprites, // CSprite::FlushSpriteBuffer when it holds the game's own sprites
            State,        // RwRenderStateSet(a, b)
            Sprites,      // our quads [a, a + b) into the sprite buffer, then FlushSpriteBuffer
            Fog,          // FUN_0023E720(a)
            Transform,    // traffic batch a into the temp buffers, then RwIm3DTransform (refused: on to the next Fog)
            Indexed,      // RwIm3DRenderIndexedPrimitive(3, indices + a, b)
            End,          // RwIm3DEnd()
        };

        struct Step
        {
            Op op;
            uint32_t a = 0, b = 0;
        };

        struct Batch
        {
            uint32_t firstVertex, vertexCount, firstIndex, indexCount;
        };

        enum class Kind : uint8_t { Lights, Stars };

        struct Chain
        {
            bool active = false;
            bool prepared = false;
            Kind kind = Kind::Lights;
            uint32_t returnTo = 0; // where the game's call returns to at the end
            std::vector<Step> steps;
            size_t next = 0;
        };

        Chain g_chain; // game thread
        std::vector<Quad> g_quads;
        std::vector<Vertex3D> g_trafficVertices;
        std::vector<int16_t> g_trafficIndices;
        std::vector<Batch> g_batches;
        unsigned g_trafficRemaining = 0;
        Population g_population;

        struct VisibleLight
        {
            float distance;
            Vector3 screen;
            float width, height;
            uint16_t index;
            uint8_t intensity;
        };

        // The nearest `limit` candidates (a max-heap on distance; the plugin's console::NearestLights)
        class NearestLights
        {
            std::vector<VisibleLight> items_;
            size_t limit_ = 0;
            void descend(size_t parent)
            {
                const VisibleLight value = items_[parent];
                for (size_t child = parent * 2 + 1; child < items_.size(); child = parent * 2 + 1)
                {
                    if (child + 1 < items_.size() && items_[child + 1].distance > items_[child].distance)
                        ++child;
                    if (items_[child].distance <= value.distance)
                        break;
                    items_[parent] = items_[child];
                    parent = child;
                }
                items_[parent] = value;
            }

        public:
            void clear(size_t limit)
            {
                items_.clear();
                limit_ = std::min<size_t>(limit, kVisibleLimit);
                items_.reserve(limit_);
            }
            void insert(const VisibleLight &item)
            {
                if (!(item.distance >= 0.0f) || !limit_)
                    return;
                if (items_.size() == limit_)
                {
                    if (item.distance >= items_[0].distance)
                        return;
                    items_[0] = item;
                    descend(0);
                    return;
                }
                size_t child = items_.size();
                items_.push_back(item);
                while (child && items_[(child - 1) / 2].distance < item.distance)
                {
                    items_[child] = items_[(child - 1) / 2];
                    child = (child - 1) / 2;
                }
                items_[child] = item;
            }
            const std::vector<VisibleLight> &items() const { return items_; }
        };
        NearestLights g_visible;

        // for the "p2dfx" command (another thread)
        struct Status
        {
            unsigned frames = 0, candidates = 0, visible = 0, stars = 0, vehicles = 0, batches = 0, learned = 0, steps = 0;
            unsigned long long verifiedProjections = 0, projectionMismatches = 0, verifiedSprites = 0, spriteMismatches = 0;
        };
        std::mutex g_statusMutex;
        Status g_status;

        void addSteps(std::vector<Step> &steps, std::initializer_list<Step> list) { steps.insert(steps.end(), list); }

        // Batches of at most the sprite buffer's 96 quads from g_quads[first, end)
        void addSpriteSteps(std::vector<Step> &steps, size_t first, size_t end)
        {
            for (size_t i = first; i < end; i += kSpriteCapacity)
                steps.push_back({Op::Sprites, uint32_t(i), uint32_t(std::min<size_t>(kSpriteCapacity, end - i))});
        }

        // The plugin's RenderTraffic: the population moved, then the nearest visible vehicles' faces in batches the temp
        // buffers hold (512 vertices, 1020 indices), farthest first; states and fog as the plugin sets them.
        void prepareTraffic(const uint8_t *ram, std::vector<Step> &steps)
        {
            g_trafficVertices.clear();
            g_trafficIndices.clear();
            g_batches.clear();
            const bool cars = g_carsOn.load(std::memory_order_relaxed), boats = g_boatsOn.load(std::memory_order_relaxed);
            const uint32_t pathsAt = rd32(ram, kPaths);
            if ((!cars && !boats) || !pathsAt)
            {
                g_population.clear();
                return;
            }
            g_population.range = float(g_trafficRange.load(std::memory_order_relaxed));
            g_population.carLimit = cars ? unsigned(std::clamp(g_maxCars.load(std::memory_order_relaxed), 0, 40)) : 0u;
            g_population.boatLimit = boats ? unsigned(std::clamp(g_maxBoats.load(std::memory_order_relaxed), 0, 8)) : 0u;
            Paths paths;
            paths.ram = ram;
            paths.nodes = rd32(ram, pathsAt);
            paths.links = rd32(ram, pathsAt + 31648u);
            paths.count = rd32(ram, pathsAt + 16u);
            paths.linkCount = uint16_t(rd32(ram, pathsAt + 24u) >> 16); // the u16 at +26
            const Vector3 camera = cameraPosition(ram);
            g_population.update(paths, camera, rd32(ram, kTimeMs), rd32(ram, kTrafficPaused) == 0);
            // remove vehicles of a kind switched off since
            for (Vehicle &v : g_population.vehicles)
                if (v.active && (v.water ? !boats : !cars))
                    v.active = false;

            const float night = nightIntensity(hour(ram), minute(ram));
            struct Item
            {
                float distance;
                uint8_t index;
            };
            Item items[Population::capacity];
            unsigned count = 0;
            for (unsigned i = 0; i < Population::capacity; ++i)
            {
                const Vehicle &v = g_population.vehicles[i];
                if (!v.active || !(v.alpha > 0))
                    continue;
                const float dx = v.position.x - camera.x, dy = v.position.y - camera.y;
                const Item item{dx * dx + dy * dy, uint8_t(i)};
                unsigned at = count++;
                while (at && items[at - 1].distance < item.distance) // farthest first: nearer ones draw over them
                {
                    items[at] = items[at - 1];
                    --at;
                }
                items[at] = item;
            }
            constexpr unsigned kMaxVertices = kTempVertexBytes / sizeof(Vertex3D), kMaxIndices = 1020;
            static_assert(kMaxIndices <= kTempIndexCount && kMaxIndices % 3 == 0, "whole triangles in the index buffer");
            Batch batch{0, 0, 0, 0};
            const auto close = [&] {
                if (batch.indexCount)
                    g_batches.push_back(batch);
                batch = {uint32_t(g_trafficVertices.size()), 0, uint32_t(g_trafficIndices.size()), 0};
            };
            unsigned drawn = 0;
            for (unsigned k = 0; k < count; ++k)
            {
                const Vehicle &v = g_population.vehicles[items[k].index];
                Vector3 screen;
                float w, h;
                // the whole traffic range projects on the PS2 (no pulling in), as the plugin found
                if (!project(ram, v.position, screen, w, h) || !(screen.z > 0) || !(screen.x > -30 && screen.x < 670 &&
                                                                                     screen.y > -30 && screen.y < 478))
                    continue;
                const Vector3 *corners = v.water ? kBoatCorners : kCarCorners;
                const uint8_t *polygons = v.water ? kBoatPolygons : kCarPolygons;
                const FarFace *faces = v.water ? kBoatFaces : kCarFaces;
                const unsigned faceCount = v.water ? unsigned(std::size(kBoatFaces)) : unsigned(std::size(kCarFaces));
                // the camera in vehicle space (rotation only; the 1.2 / 0.8 scale is uniform)
                const float cx = camera.x - v.position.x, cy = camera.y - v.position.y, cz = camera.z - v.position.z;
                const float lx = v.forward.y * cx - v.forward.x * cy, ly = v.forward.x * cx + v.forward.y * cy;
                const bool cabin = items[k].distance < 500.0f * 500.0f, lamps = night > 0.2f;
                bool facing[16]{};
                unsigned needVertices = 0, needIndices = 0;
                for (unsigned f = 0; f < faceCount && f < 16; ++f)
                {
                    const FarFace &face = faces[f];
                    if ((face.level == 1 && !cabin) || (face.level == 2 && !lamps))
                        continue;
                    const Vector3 &c = corners[polygons[face.first]];
                    const float sx = v.water ? 0.8f : 1.2f;
                    const float d = face.normal.x * (lx - c.x * sx) + face.normal.y * (ly - c.y * sx) + face.normal.z * (cz - c.z * sx);
                    facing[f] = d > 0;
                    if (!facing[f])
                        continue;
                    needVertices += face.count;
                    needIndices += 3 * (face.count - 2);
                }
                if (!needIndices)
                    continue;
                if (batch.vertexCount + needVertices > kMaxVertices || batch.indexCount + needIndices > kMaxIndices)
                    close();
                for (unsigned f = 0; f < faceCount && f < 16; ++f)
                {
                    if (!facing[f])
                        continue;
                    const FarFace &face = faces[f];
                    const unsigned base = batch.vertexCount;
                    for (unsigned c = 0; c < face.count; ++c)
                        g_trafficVertices.push_back(vehicleVertex(corners[polygons[face.first + c]], face.normal, face.material, v, night));
                    batch.vertexCount += face.count;
                    for (unsigned c = 2; c < face.count; ++c) // corner lists run clockwise: the order the detailed meshes use
                    {
                        g_trafficIndices.push_back(int16_t(base));
                        g_trafficIndices.push_back(int16_t(base + c));
                        g_trafficIndices.push_back(int16_t(base + c - 1));
                    }
                    batch.indexCount += 3 * (face.count - 2);
                }
                ++drawn;
            }
            close();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status.vehicles = drawn;
                g_status.batches = unsigned(g_batches.size());
            }
            if (g_batches.empty())
                return;
            g_trafficRemaining = kTrafficBytes;
            addSteps(steps, {{Op::Flush2D}, {Op::FlushSprites}, {Op::State, kStateRaster, 0}, {Op::State, kStateZTest, 1},
                             {Op::State, kStateZWrite, 0}, {Op::State, kStateSrcBlend, 5}, {Op::State, kStateDestBlend, 6},
                             {Op::Fog, 0}});
            for (size_t i = 0; i < g_batches.size(); ++i)
            {
                steps.push_back({Op::Transform, uint32_t(i)});
                for (uint32_t first = 0; first < g_batches[i].indexCount; first += 480) // the plugin's draw size
                    steps.push_back({Op::Indexed, first, std::min<uint32_t>(480, g_batches[i].indexCount - first)});
                steps.push_back({Op::End});
            }
            // CCoronas::Render's postconditions at this site, as the plugin restores them
            addSteps(steps, {{Op::Fog, 1}, {Op::State, kStateRaster, 0}, {Op::State, kStateZTest, 1},
                             {Op::State, kStateZWrite, 1}, {Op::State, kStateSrcBlend, 5}, {Op::State, kStateDestBlend, 6}});
        }

        // The plugin's RenderLights: the nearest `limit` lamps lit now within the range (a horizontal one: altitude keeps
        // them) and beyond a third of their own corona range (the game's coronas cover the rest), as sprites with the
        // corona texture, depth-tested, drawn in front of the lamp a little.
        void prepareLights(const uint8_t *ram, std::vector<Step> &steps)
        {
            if (!g_lightsOn.load(std::memory_order_relaxed) || g_lights.empty())
                return;
            const Vector3 camera = cameraPosition(ram);
            const float night = nightIntensity(hour(ram), minute(ram));
            const uint32_t ms = rd32(ram, kTimeMs);
            float phases[8];
            for (unsigned mode = 0; mode < 8; ++mode)
                phases[mode] = blinkIntensity(mode, ms);
            const unsigned signals[] = {3, lightForCars(ram, false), lightForCars(ram, true)};
            const float range = float(g_range.load(std::memory_order_relaxed));
            const float maximumSquared = range * range;
            const float radiusScale = float(g_sizePercent.load(std::memory_order_relaxed)) / 100.0f;
            const unsigned limit = unsigned(std::clamp(g_limit.load(std::memory_order_relaxed), 1, int(kVisibleLimit)));
            const unsigned budget = spriteBudget(ram);
            g_visible.clear(std::min(limit, budget));
            unsigned candidates = 0;
            if (budget)
                g_grid.visit(camera, range, [&](uint16_t i) {
                    const Light &light = g_lights[i];
                    const float d2 = distanceSquared(camera, light.position);
                    const float hx = light.position.x - camera.x, hy = light.position.y - camera.y;
                    const float horizontal = std::sqrt(hx * hx + hy * hy);
                    if (!(horizontal * horizontal < maximumSquared))
                        return;
                    ++candidates;
                    float intensity = trafficLight(light) ? (signals[g_groups[i] < 3 ? g_groups[i] : 0] == trafficColor(light) ? 1.0f : 0.0f)
                                                          : night * phases[light.blink < 8 ? light.blink : 0];
                    if (intensity <= 0.0f || !light.a)
                        return;
                    const float nearRange = light.nativeRange / 3.0f;
                    if (!light.noDistance && d2 <= nearRange * nearRange)
                        return;
                    Vector3 screen;
                    float width, height;
                    if (!project(ram, light.position, screen, width, height)) // independent of the streamed far clip
                        return;
                    const float distance = std::sqrt(d2);
                    float radius = light.noDistance ? 3.5f : 3.5f * saturate((distance - nearRange) / (light.nativeRange - nearRange));
                    radius *= light.size * radiusScale * bounded(0.0025f * distance + 0.25f, 0.0f, 4.0f, 1.0f);
                    width *= radius;
                    height *= radius;
                    if (!(width > 0.0f && height > 0.0f && screen.z > 0.0f) || screen.x + width < 0.0f ||
                        screen.x - width > 640.0f || screen.y + height < 0.0f || screen.y - height > 448.0f)
                        return;
                    intensity *= saturate((range - horizontal) / (range * 0.1f)); // the last tenth fades
                    const unsigned alpha = unsigned(intensity * light.a);
                    if (alpha)
                        g_visible.insert({d2, screen, width, height, i, uint8_t(alpha)});
                });
            const uint32_t raster = coronaRaster(ram);
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status.candidates = candidates;
                g_status.visible = raster ? unsigned(g_visible.items().size()) : 0u;
            }
            if (!raster || g_visible.items().empty())
                return;
            const size_t first = g_quads.size();
            for (const VisibleLight &item : g_visible.items())
            {
                const Light &light = g_lights[item.index];
                // flat sprites at the lamp's depth were cut by the facade or ground behind it seen from above: a little in
                // front, scaled with depth (as native coronas)
                const float bias = 2.0f + 0.04f * item.screen.z;
                const float z = bias < item.screen.z * 0.5f ? item.screen.z - bias : item.screen.z * 0.5f;
                Quad q;
                spriteQuad(ram, item.screen.x, item.screen.y, z, item.width, item.height, light.r, light.g, light.b,
                           int16_t(item.intensity), item.intensity, q);
                g_quads.push_back(q);
            }
            addSteps(steps, {{Op::Flush2D}, {Op::FlushSprites}, {Op::State, kStateZWrite, 0}, {Op::State, kStateZTest, 1},
                             {Op::State, kStateSrcBlend, 2}, {Op::State, kStateDestBlend, 2}, {Op::State, kStateRaster, raster}});
            addSpriteSteps(steps, first, g_quads.size());
            // CCoronas::Render's postconditions before the next world pass, as the plugin restores them
            addSteps(steps, {{Op::State, kStateRaster, 0}, {Op::State, kStateZWrite, 1}, {Op::State, kStateZTest, 1}});
        }

        // The plugin's 500 stars: five faces of a box around the camera (100 units out), sizes from its defaults
        struct Star
        {
            Vector3 offset;
            float size;
        };
        std::vector<Star> g_stars;

        void makeStars()
        {
            uint32_t state = 0x56435332;
            const auto random = [&](float low, float high) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                return low + (high - low) * float(state & 0xFFFFFFu) / 16777216.0f;
            };
            constexpr float smallStars = 0.15f, mediumStars = 0.6f, largeStars = 1.2f, largeChance = 0.2f;
            g_stars.clear();
            for (unsigned side = 0; side < 5; ++side)
                for (unsigned i = 0; i < 100; ++i)
                {
                    const float x = random(-95.0f, 95.0f);
                    const float y = side == 4 ? random(-95.0f, 95.0f) : random(-33.25f, 95.0f);
                    Vector3 offset;
                    switch (side)
                    {
                    case 0: offset = {100.0f, -x, 10.0f + y}; break;
                    case 1: offset = {-100.0f, -x, 10.0f + y}; break;
                    case 2: offset = {-x, 100.0f, 10.0f + y}; break;
                    case 3: offset = {-x, -100.0f, 10.0f + y}; break;
                    default: offset = {x, y, 95.0f}; break;
                    }
                    const float maximum = random(0.0f, 1.0f) < largeChance ? largeStars : mediumStars;
                    g_stars.push_back({offset, 0.8f * random(smallStars, maximum)});
                }
        }

        // The plugin's RenderStars, after the game's own stars' flush in CClouds::Render (before the moon and clouds):
        // dimmed by the clouds, in the state the game's stars left
        void prepareStars(const uint8_t *ram, std::vector<Step> &steps)
        {
            float coverage = rdf(ram, kCloudCover);
            const float overcast = rdf(ram, kOvercast);
            if (overcast > coverage)
                coverage = overcast;
            const unsigned brightness = unsigned(255.0f * starIntensity(hour(ram), minute(ram)) * (1.0f - saturate(coverage)));
            const uint32_t raster = coronaRaster(ram);
            unsigned budget = brightness && raster ? spriteBudget(ram) : 0u;
            if (!budget)
                return;
            const Vector3 camera = cameraPosition(ram);
            const size_t first = g_quads.size();
            for (const Star &star : g_stars)
            {
                const Vector3 position{camera.x + star.offset.x, camera.y + star.offset.y, camera.z + star.offset.z};
                Vector3 screen;
                float width, height;
                if (!project(ram, position, screen, width, height) || !(screen.z > 0.0f))
                    continue;
                width *= star.size;
                height *= star.size;
                if (screen.x + width < 0.0f || screen.x - width > 640.0f || screen.y + height < 0.0f || screen.y - height > 448.0f)
                    continue;
                Quad q;
                spriteQuad(ram, screen.x, screen.y, screen.z, width, height, brightness, brightness, brightness, 255, 255, q);
                g_quads.push_back(q);
                if (!--budget)
                    break;
            }
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status.stars = unsigned(g_quads.size() - first);
            }
            if (g_quads.size() == first)
                return;
            addSteps(steps, {{Op::Flush2D}, {Op::FlushSprites}, {Op::State, kStateRaster, raster}});
            addSpriteSteps(steps, first, g_quads.size());
        }

        void call(VcsGuestContext *ctx, uint32_t function, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
        {
            mod::setGpr32(ctx, kA0, a0);
            mod::setGpr32(ctx, kA1, a1);
            mod::setGpr32(ctx, kA2, a2);
            mod::setGpr32(ctx, kA3, a3);
            mod::setGpr32(ctx, kRa, kContinue);
            mod::setPc(ctx, function);
        }

        // The next step's guest call (true), or false when the step needs none
        bool startStep(uint8_t *ram, VcsGuestContext *ctx, const Step &s)
        {
            switch (s.op)
            {
            case Op::Flush2D:
                if (int32_t(rd32(ram, k2DVertexCount)) <= 0)
                    return false;
                call(ctx, kFlush2D);
                return true;
            case Op::FlushSprites:
                if (int32_t(rd32(ram, kSpriteCount)) <= 0)
                    return false;
                call(ctx, kFlushSprites);
                return true;
            case Op::State:
                call(ctx, kRenderState, s.a, s.b);
                return true;
            case Op::Sprites:
                for (uint32_t i = 0; i < s.b; ++i)
                {
                    std::memcpy(ram + ((kTempVertices + i * 64u) & kRamMask), g_quads[s.a + i].bytes, 64);
                    uint16_t indices[6];
                    spriteIndices(i, indices);
                    std::memcpy(ram + ((kTempIndices + i * 12u) & kRamMask), indices, sizeof(indices));
                }
                wr32(ram, kSpriteCount, s.b);
                wr32(ram, kSpriteZFlag, 0);
                call(ctx, kFlushSprites);
                return true;
            case Op::Fog:
                call(ctx, kSetFog, s.a);
                return true;
            case Op::Transform:
            {
                const Batch &b = g_batches[s.a];
                // the plugin's AdmitPS2Mesh: whole batches only, room kept for the native coronas / HUD after
                const unsigned bytes = (b.indexCount / 3) * 5 * 16 * 3 + 16 * 1024;
                const int64_t free = packetFree(ram);
                if (bytes > g_trafficRemaining || free < 0 || uint64_t(free) < uint64_t(bytes) + 192 * 1024)
                {
                    while (g_chain.next < g_chain.steps.size() && g_chain.steps[g_chain.next].op != Op::Fog)
                        ++g_chain.next;
                    return false;
                }
                g_trafficRemaining -= bytes;
                std::memcpy(ram + (kTempVertices & kRamMask), g_trafficVertices.data() + b.firstVertex, b.vertexCount * sizeof(Vertex3D));
                std::memcpy(ram + (kTempIndices & kRamMask), g_trafficIndices.data() + b.firstIndex, b.indexCount * sizeof(int16_t));
                call(ctx, kIm3DTransform, kTempVertices, b.vertexCount, 0);
                return true;
            }
            case Op::Indexed:
                call(ctx, kIm3DIndexed, 3, kTempIndices + s.a * 2u, s.b);
                return true;
            case Op::End:
                call(ctx, kIm3DEnd);
                return true;
            }
            return false;
        }

        void prepare(const uint8_t *ram)
        {
            g_chain.steps.clear();
            g_chain.next = 0;
            g_quads.clear();
            if (g_chain.kind == Kind::Lights)
            {
                prepareTraffic(ram, g_chain.steps);
                prepareLights(ram, g_chain.steps);
            }
            else
                prepareStars(ram, g_chain.steps);
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.frames += g_chain.kind == Kind::Lights ? 1u : 0u;
            g_status.steps = unsigned(g_chain.steps.size());
        }

        // Every chained call returns here: the next call, or back to where the game's own call returns
        VCS_GUEST_CC void chainContinue(uint8_t *rdram, VcsGuestContext *ctx, void *)
        {
            if (!g_chain.active)
            {
                mod::log("[p2dfx] continuation 0x%08x reached outside a chain (ra=0x%08x): returning to 0x%08x\n",
                             kContinue, mod::gpr32(ctx, kRa), g_chain.returnTo);
                mod::setPc(ctx, g_chain.returnTo);
                return;
            }
            if (!g_chain.prepared)
            {
                prepare(rdram);
                g_chain.prepared = true;
            }
            while (g_chain.next < g_chain.steps.size())
                if (startStep(rdram, ctx, g_chain.steps[g_chain.next++]))
                    return;
            g_chain.active = false;
            mod::setPc(ctx, g_chain.returnTo);
        }

        void startChain(VcsGuestContext *ctx, Kind kind)
        {
            g_chain.active = true;
            g_chain.prepared = false;
            g_chain.kind = kind;
            g_chain.returnTo = mod::gpr32(ctx, kRa);
            g_chain.steps.clear();
            g_chain.next = 0;
            mod::setGpr32(ctx, kRa, kContinue); // the original returns into the chain
        }

        // ---- VCS_P2DFX_VERIFY: the native projection and sprite writer against the game's own calls ----

        bool g_verify = false;
        struct PendingProjection
        {
            bool pending = false, ok = false;
            uint32_t out = 0, w = 0, h = 0, sp = 0;
            Vector3 screen{};
            float width = 0, height = 0;
        } g_pendingProjection;
        struct PendingSprite
        {
            bool pending = false;
            uint32_t slot = 0;
            Quad quad{};
        } g_pendingSprite;

        // The game's call before this one has written its results by now (its caller's frame still holds them while
        // the stack pointer is the same)
        void checkPending(const uint8_t *ram, const VcsGuestContext *ctx)
        {
            if (g_pendingProjection.pending && mod::gpr32(ctx, kSp) == g_pendingProjection.sp)
            {
                const PendingProjection &p = g_pendingProjection;
                bool same = std::memcmp(ram + (p.out & kRamMask), &p.screen, sizeof(Vector3)) == 0;
                if (p.ok)
                    same = same && std::memcmp(ram + (p.w & kRamMask), &p.width, 4) == 0 &&
                           std::memcmp(ram + (p.h & kRamMask), &p.height, 4) == 0;
                std::lock_guard<std::mutex> lock(g_statusMutex);
                ++g_status.verifiedProjections;
                if (!same && g_status.projectionMismatches++ < 8)
                    mod::log("[p2dfx] projection differs: native (%.6f %.6f %.6f) w %.6f h %.6f, game (%.6f %.6f %.6f) "
                                         "w %.6f h %.6f\n", p.screen.x, p.screen.y, p.screen.z, p.width, p.height, rdf(ram, p.out),
                                 rdf(ram, p.out + 4), rdf(ram, p.out + 8), rdf(ram, p.w), rdf(ram, p.h));
            }
            g_pendingProjection.pending = false;
            if (g_pendingSprite.pending)
            {
                const PendingSprite &p = g_pendingSprite;
                uint16_t indices[6];
                spriteIndices(p.slot, indices);
                const bool same = std::memcmp(ram + ((kTempVertices + p.slot * 64u) & kRamMask), p.quad.bytes, 64) == 0 &&
                                  std::memcmp(ram + ((kTempIndices + p.slot * 12u) & kRamMask), indices, sizeof(indices)) == 0;
                std::lock_guard<std::mutex> lock(g_statusMutex);
                ++g_status.verifiedSprites;
                if (!same && g_status.spriteMismatches++ < 8)
                {
                    uint32_t game[4], ours[4];
                    std::memcpy(game, ram + ((kTempVertices + p.slot * 64u) & kRamMask), 16);
                    std::memcpy(ours, p.quad.bytes, 16);
                    mod::log("[p2dfx] sprite %u differs: native %08x %08x %08x %08x, game %08x %08x %08x %08x\n", p.slot,
                                 ours[0], ours[1], ours[2], ours[3], game[0], game[1], game[2], game[3]);
                }
            }
            g_pendingSprite.pending = false;
        }

        VcsGuestFn g_origCoronas = nullptr, g_origFlush = nullptr, g_origRegister = nullptr,
                                       g_origCeiling = nullptr, g_origProject = nullptr, g_origSprite = nullptr,
                                       g_origFarClip = nullptr;

        VCS_GUEST_CC void verifyProject(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            checkPending(rdram, ctx);
            PendingProjection &p = g_pendingProjection;
            p.out = mod::gpr32(ctx, kA1);
            p.w = mod::gpr32(ctx, kA2);
            p.h = mod::gpr32(ctx, kA3);
            p.sp = mod::gpr32(ctx, kSp);
            p.ok = project(rdram, rdVector(rdram, mod::gpr32(ctx, kA0)), p.screen, p.width, p.height, mod::gpr32(ctx, kT0) != 0);
            p.pending = true;
            return g_origProject(rdram, ctx, runtime);
        }

        VCS_GUEST_CC void verifySprite(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            checkPending(rdram, ctx);
            const uint32_t slot = rd32(rdram, kSpriteCount);
            if (slot < kSpriteCapacity)
            {
                spriteQuad(rdram, mod::fpr(ctx)[12], mod::fpr(ctx)[13], mod::fpr(ctx)[14], mod::fpr(ctx)[15], mod::fpr(ctx)[16], mod::gpr32(ctx, kA0), mod::gpr32(ctx, kA1),
                           mod::gpr32(ctx, kA2), int16_t(mod::gpr32(ctx, kA3)), mod::gpr32(ctx, kT0), g_pendingSprite.quad);
                g_pendingSprite.slot = slot;
                g_pendingSprite.pending = true;
            }
            return g_origSprite(rdram, ctx, runtime);
        }

        // ---- the hooks ----

        // CCoronas::Render from the frame's render: the original returns into the chain (traffic, then the lights)
        VCS_GUEST_CC void coronasRender(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (mod::gpr32(ctx, kRa) == kAfterCoronas && !g_chain.active &&
                ((g_lightsOn.load(std::memory_order_relaxed) && !g_lights.empty()) || g_carsOn.load(std::memory_order_relaxed) ||
                 g_boatsOn.load(std::memory_order_relaxed)))
                startChain(ctx, Kind::Lights);
            return g_origCoronas(rdram, ctx, runtime);
        }

        // CSprite::FlushSpriteBuffer after CClouds::Render's stars: the original returns into the stars' chain
        VCS_GUEST_CC void flushSprites(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_verify)
                checkPending(rdram, ctx);
            if (mod::gpr32(ctx, kRa) == kAfterStars && !g_chain.active && g_starsOn.load(std::memory_order_relaxed))
                startChain(ctx, Kind::Stars);
            return g_origFlush(rdram, ctx, runtime);
        }

        // CCoronas::RegisterCorona from DisplayActualLight: the streamed signal's lamps say which group (FindTrafficLightType
        // of the signal) the table's lamps there follow - no guessed timing, no extra coronas
        VCS_GUEST_CC void registerCorona(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ra = mod::gpr32(ctx, kRa);
            if (g_lightsOn.load(std::memory_order_relaxed) && !g_lights.empty() &&
                std::find(std::begin(kSignalCoronas), std::end(kSignalCoronas), ra) != std::end(kSignalCoronas))
            {
                const uint32_t signal = mod::gpr32(ctx, kS6), position = mod::gpr32(ctx, kT1);
                if (signal && position)
                {
                    const uint8_t group = uint8_t(findTrafficLightType(rdf(rdram, signal + 0x10u), rdf(rdram, signal + 0x14u)));
                    const Vector3 at = rdVector(rdram, position);
                    unsigned learned = 0;
                    g_grid.visit(at, 0.5f, [&](uint16_t c) {
                        if (trafficLight(g_lights[c]) && distanceSquared(at, g_lights[c].position) < 0.25f && g_groups[c] != group)
                        {
                            g_groups[c] = group;
                            ++learned;
                        }
                    });
                    if (learned)
                    {
                        std::lock_guard<std::mutex> lock(g_statusMutex);
                        g_status.learned += learned;
                    }
                }
            }
            return g_origRegister(rdram, ctx, runtime);
        }

        // The flying controls' height penalty (FUN_003EA618: f12 height, a1 the flying type), with the 80 m of both
        // branches at 800 m as the plugin patches them (types 1 / 2 keep their soft start at 70 m)
        VCS_GUEST_CC void flightCeiling(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (!g_ceilingOn.load(std::memory_order_relaxed))
                return g_origCeiling(rdram, ctx, runtime);
            constexpr float kCeiling = 800.0f;
            const float h = mod::fpr(ctx)[12];
            const uint32_t type = mod::gpr32(ctx, kA1) - 1u;
            float result = -1.0f;
            if (type < 2u)
            {
                if (!(h < 70.0f))
                    result = h < kCeiling ? FPU_SUB_S(h, 70.0f) : FPU_ADD_S(h, 70.0f);
            }
            else if (!(h < kCeiling))
                result = FPU_SUB_S(h, kCeiling);
            mod::fpr(ctx)[0] = result;
            mod::setPc(ctx, mod::gpr32(ctx, kRa));
        }

        // RwCameraSetFarClipPlane from the frame's render: the world's far plane (2000 every frame; CCamera::Process copies
        // it into the entity culling's 0x4CD12C, the sector culling builds its frustum from it, the fog ends there) times
        // Draw distance. The LOD distance goes into CRenderer::ms_lodDistScale here too, on the game thread once a frame:
        // CCamera::Process multiplies TheCamera's LOD multiplier (+0x7DC: 70 / FOV, raised with height, capped at 2) by it.
        VCS_GUEST_CC void setFarClip(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (mod::gpr32(ctx, kRa) == kAfterFarClip)
            {
                const int draw = g_drawPercent.load(std::memory_order_relaxed);
                if (draw != 100)
                    mod::fpr(ctx)[12] = mod::fpr(ctx)[12] * float(draw) / 100.0f;
                const float lod = float(g_lodPercent.load(std::memory_order_relaxed)) / 100.0f;
                std::memcpy(rdram + kLodDistScale, &lod, 4);
            }
            return g_origFarClip(rdram, ctx, runtime);
        }

        void addChoice(const char *key, const char *label, const char *help, std::vector<mod::Choice> choices, int value,
                       std::atomic<int> &target, const char *section = "Project2DFX")
        {
            mod::Spec s;
            s.key = key;
            s.page = "Graphics";
            s.section = section;
            s.label = label;
            s.help = help;
            s.type = mod::Type::Choice;
            s.choices = std::move(choices);
            s.defaultValue = value;
            s.onChange = [&target](int v) { target = v; };
            mod::add(s);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        addChoice("p2dfx.draw_distance", "Draw distance",
                  "How far the world is drawn: the camera's far plane, 2000 m in the original (the fog ends there too, so "
                  "the distance is clearer). Every island is drawn from anywhere at 3x. Costs GPU time.",
                  {{"1x (original)", 100}, {"1.5x", 150}, {"2x", 200}, {"3x", 300}}, 100, g_drawPercent, "Draw distance");
        addChoice("p2dfx.lod_distance", "Object detail distance",
                  "How far objects (lamp posts, trees, signs, small buildings) keep their detailed models before they switch "
                  "to a simpler one or disappear - the game's LOD distance scale. Costs CPU and GPU time.",
                  {{"1x (original)", 100}, {"1.5x", 150}, {"2x", 200}, {"3x", 300}, {"4x", 400}, {"5x", 500}}, 100,
                  g_lodPercent, "Draw distance");

        Spec s;
        s.key = "p2dfx.lights";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "Distant lights";
        s.help = "Street lamps, signs and traffic signals glow far across the city at night (Project2DFX's LOD lights). "
                 "Needs the plugin's light table: python tools/p2dfx.py import <GTAVCS.PCSX2F.Project2DFX.zip>.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_DISTANT_LIGHTS";
        s.onChange = [](int v) { g_lightsOn = v != 0; };
        add(s);

        addChoice("p2dfx.lights_range", "Light draw distance", "How far away distant lights are drawn. Lower is faster.",
                  {{"300 m", 300}, {"500 m", 500}, {"750 m", 750}, {"1000 m", 1000}, {"1500 m", 1500}, {"2000 m", 2000},
                   {"3000 m", 3000}, {"4000 m", 4000}},
                  1000, g_range);
        addChoice("p2dfx.lights_limit", "Light limit", "The most distant lights shown at once (the nearest are kept).",
                  {{"300", 300}, {"600", 600}, {"900", 900}, {"1200", 1200}, {"1600", 1600}, {"2048", 2048}}, 900, g_limit);
        addChoice("p2dfx.lights_size", "Light size", "Size of the distant lights.",
                  {{"0.75x", 75}, {"1x", 100}, {"1.5x", 150}, {"2x", 200}, {"3x", 300}}, 150, g_sizePercent);

        s = {};
        s.key = "p2dfx.stars";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "More stars";
        s.help = "500 more stars in the night sky, fading with the clouds (the game draws 11).";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_MORE_STARS";
        s.onChange = [](int v) { g_starsOn = v != 0; };
        add(s);

        s = {};
        s.key = "p2dfx.cars";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "Distant cars";
        s.help = "Simple cars driving the game's roads far beyond its own traffic (300 m out to the traffic distance), "
                 "head and tail lights at night.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_DISTANT_CARS";
        s.onChange = [](int v) { g_carsOn = v != 0; };
        add(s);

        s = {};
        s.key = "p2dfx.boats";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "Distant boats";
        s.help = "Simple boats on the game's water paths far away.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_DISTANT_BOATS";
        s.onChange = [](int v) { g_boatsOn = v != 0; };
        add(s);

        addChoice("p2dfx.traffic_range", "Traffic draw distance", "Distant cars and boats fade out at this distance.",
                  {{"450 m", 450}, {"600 m", 600}, {"800 m", 800}, {"1000 m", 1000}, {"1500 m", 1500}, {"2000 m", 2000}}, 800,
                  g_trafficRange);

        s = {};
        s.key = "p2dfx.cars_max";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "Distant car limit";
        s.help = "The most distant cars at once.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 40;
        s.defaultValue = 40;
        s.onChange = [](int v) { g_maxCars = v; };
        add(s);

        s = {};
        s.key = "p2dfx.boats_max";
        s.page = "Graphics";
        s.section = "Project2DFX";
        s.label = "Distant boat limit";
        s.help = "The most distant boats at once.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 8;
        s.defaultValue = 8;
        s.onChange = [](int v) { g_maxBoats = v; };
        add(s);

        s = {};
        s.key = "p2dfx.flight_ceiling";
        s.page = "Game";
        s.section = "Flying";
        s.label = "Higher flight ceiling";
        s.help = "Helicopters and planes lose lift from 800 m instead of the game's 80 m (as Project2DFX sets it, to see "
                 "the distant lights from above).";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_FLIGHT_CEILING";
        s.onChange = [](int v) { g_ceilingOn = v != 0; };
        add(s);
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_P2DFX_HOOKS"); e && e[0] == '0')
            return;
        if (mod::api->has_function(kContinue))
        {
            mod::log("[p2dfx] 0x%08x is a function or resume point: no Project2DFX\n", kContinue);
            return;
        }
        g_origCoronas = mod::api->lookup_function(kCoronasRender);
        g_origFlush = mod::api->lookup_function(kFlushSprites);
        g_origRegister = mod::api->lookup_function(kRegisterCorona);
        g_origCeiling = mod::api->lookup_function(kFlightCeiling);
        g_origFarClip = mod::api->lookup_function(kSetFarClip);
        if (!g_origCoronas || !g_origFlush || !g_origRegister || !g_origCeiling || !g_origFarClip)
            return;
        loadLights();
        makeStars();
        mod::api->register_function(kContinue, &chainContinue);
        mod::api->replace_function(kCoronasRender, &coronasRender);
        mod::api->replace_function(kFlushSprites, &flushSprites);
        mod::api->replace_function(kRegisterCorona, &registerCorona);
        mod::api->replace_function(kFlightCeiling, &flightCeiling);
        mod::api->replace_function(kSetFarClip, &setFarClip);
        if (const char *e = std::getenv("VCS_P2DFX_VERIFY"); e && e[0] == '1')
        {
            g_origProject = mod::api->lookup_function(kCalcScreenCoors);
            g_origSprite = mod::api->lookup_function(kBufferedSprite);
            if (g_origProject && g_origSprite)
            {
                g_verify = true;
                mod::api->replace_function(kCalcScreenCoors, &verifyProject);
                mod::api->replace_function(kBufferedSprite, &verifySprite);
            }
        }
        // p2dfx -> ok table= frames= candidates= visible= stars= vehicles= batches= steps= learned= [verify counts] (tests)
        mod::addCommand("p2dfx", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[400];
            std::snprintf(text, sizeof(text),
                          "ok table=%zu lights=%d stars_on=%d cars=%d boats=%d frames=%u candidates=%u visible=%u stars=%u "
                          "vehicles=%u batches=%u steps=%u learned=%u projections=%llu/%llu sprites=%llu/%llu",
                          g_lights.size(), g_lightsOn ? 1 : 0, g_starsOn ? 1 : 0, g_carsOn ? 1 : 0, g_boatsOn ? 1 : 0, st.frames,
                          st.candidates, st.visible, st.stars, st.vehicles, st.batches, st.steps, st.learned,
                          st.verifiedProjections - st.projectionMismatches, st.verifiedProjections,
                          st.verifiedSprites - st.spriteMismatches, st.verifiedSprites);
            reply(text);
        });
    }
}
