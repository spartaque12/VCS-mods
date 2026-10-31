// MeshSmooth.asi - smoother peds and vehicles.
//
// vcs.exe's native lit draws (game/vcs_hle_world.cpp) hand every lit model batch - a triangle strip of up to 256
// vertices out of a model's VU1 packets - to this refiner (VcsModApi::set_lit_refiner) before drawing it. A batch is
// refined once and cached; the exe draws the refined strip instead (peds skinned there with interpolated bone weights,
// the vehicles' environment pass built from it).
//
// The surface: curved PN triangles (Vlachos et al. 2001), each triangle split into n x n (n = 2^level). Models reach the
// renderer cut into batches, and materials / hard edges duplicate vertices, so a triangle's own normals would give the
// two sides of a shared edge different curves (cracks). Every edge curve here depends only on the edge's two positions
// and on normals shared by all users of a position: the normals seen at a position (over all batches of all frames, as
// integers, summed exactly) are averaged; a position whose normals spread more than the crease angle is a crease - an
// edge between two creases stays straight, and at a crease the normal of its set nearest to the other end's is taken.
// Edge points are evaluated in one orientation (by position), so both sides get the same bits. Corners keep their
// original data exactly. A batch is refined at its third sighting, once its neighbours have shown their normals.
// Shading normals: the original normals interpolated, blended a little (blur) toward the shared ones.
#include "meshsmooth.h"

#include "mod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vcs::meshsmooth
{
    namespace
    {
        // ---- settings (main thread writes, the render worker reads) ---------------------------------------------
        std::atomic<int> g_enabled{1}, g_peds{1}, g_vehicles{0}, g_others{0}, g_level{1}, g_strength{100};
        std::atomic<uint32_t> g_generation{1}; // part of every cache key: a settings change refines again

        void bump() { g_generation.fetch_add(1, std::memory_order_relaxed); }

        // per class, as the San Andreas plugin's defaults: peds PN 1.0 with "de-grow" 0.35, vehicles ShapeBlend 1.0
        constexpr float kPedShape = 0.65f, kVehicleShape = 1.0f;
        constexpr float kPedCreaseCos = -0.5f;     // PNCreaseAngle 120
        constexpr float kVehicleCreaseCos = 0.94f;   // normals split over ~20 degrees: an authored hard edge
        constexpr float kNormalBlur = 0.15f;       // VanillaNormalsBlur
        constexpr int kSightingsBeforeRefine = 3;
        constexpr size_t kCacheBudget = size_t(256) << 20;

        // ---- small vector maths ---------------------------------------------------------------------------------
        struct V3
        {
            float x, y, z;
        };
        inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline float len(V3 a) { return std::sqrt(dot(a, a)); }
        inline V3 norm(V3 a)
        {
            const float l = len(a);
            return l > 1e-20f ? a * (1.0f / l) : V3{0.0f, 0.0f, 1.0f};
        }

        // ---- positions: the normals seen there ------------------------------------------------------------------
        struct NormalSet
        {
            int16_t n[6][3];
            uint8_t count = 0;
        };
        struct PosInfo // derived, per refine
        {
            V3 canon;           // unit
            bool crease = false;
            const NormalSet *set = nullptr;
        };

        inline uint64_t mix(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return h * 0xFF51AFD7ED558CCDull;
        }

        inline uint64_t edgeKey(uint64_t a, uint64_t b)
        {
            return a < b ? mix(a, b) : mix(b, a);
        }

        struct State
        {
            std::mutex lock;
            std::unordered_map<uint64_t, NormalSet> positions;
            struct EdgeSeen
            {
                uint64_t opposite[2];
                uint8_t count = 0;
            };
            std::unordered_map<uint64_t, EdgeSeen> edges; // an edge with one opposite corner: the model's open boundary
            std::unordered_map<uint64_t, uint8_t> sightings;
            struct Entry
            {
                std::vector<float> pos, nrm, uv;
                std::vector<uint32_t> w;
                std::vector<uint16_t> idx;
                uint64_t lastUse = 0;
                size_t bytes = 0;
            };
            std::unordered_map<uint64_t, std::unique_ptr<Entry>> cache;
            size_t cacheBytes = 0;
            uint64_t clock = 0;
            VcsLitRefined out{};
            // stats
            uint64_t calls[3] = {}, refined[3] = {}, built = 0, trianglesIn = 0, trianglesOut = 0;
            double buildMs = 0.0, lookupMs = 0.0;
        };
        State &st()
        {
            static State s;
            return s;
        }

        // ---- the refinement of one batch ------------------------------------------------------------------------
        struct Refiner
        {
            const VcsLitBatch &b;
            int level;
            float shape, creaseCos;
            uint64_t space; // the position key's model part (kind, scale, offset)
            State &s;

            std::vector<V3> P, N;    // real space, unit normals (originals)
            std::vector<float> nLen; // the input normals' lengths (their units)
            std::vector<uint64_t> key;
            std::vector<PosInfo> info;

            std::vector<float> opos, onrm, ouv;
            std::vector<uint32_t> ow;
            std::vector<uint16_t> idx;
            std::unordered_map<uint64_t, uint16_t> edgeVerts;

            V3 realPos(const int32_t *p) const
            {
                return {p[0] * b.pos_scale[0] + b.pos_offset[0], p[1] * b.pos_scale[1] + b.pos_offset[1],
                        p[2] * b.pos_scale[2] + b.pos_offset[2]};
            }

            // the normal of position i used for its edge towards position j (see the file comment): the shared one, or
            // at a crease the one of its set the edge lies flattest in (a flat panel's edges stay flat, an edge along the
            // crease straight) - from positions and the position's set only, so both sides of the edge agree
            V3 edgeNormal(uint32_t i, uint32_t j) const
            {
                const PosInfo &a = info[i];
                if (!a.crease || !a.set)
                    return a.canon;
                const V3 e = P[j] - P[i];
                V3 best = a.canon;
                float bestW = 3.0e38f;
                for (int k = 0; k < a.set->count; ++k)
                {
                    const V3 n = norm(V3{float(a.set->n[k][0]), float(a.set->n[k][1]), float(a.set->n[k][2])});
                    const float w = std::fabs(dot(e, n));
                    if (w < bestW)
                        bestW = w, best = n;
                }
                return best;
            }

            // the edge's two inner control points, from i towards j (b_ij near i, b_ji near j)
            bool boundary(uint32_t i, uint32_t j) const
            {
                auto it = s.edges.find(edgeKey(key[i], key[j]));
                return it != s.edges.end() && it->second.count == 1;
            }

            void edgeControl(uint32_t i, uint32_t j, V3 &bij, V3 &bji) const
            {
                const V3 a = P[i], c = P[j];
                const float sh = boundary(i, j) ? 0.0f : shape; // open boundaries stay straight (parts meeting there)
                const V3 ni = edgeNormal(i, j), nj = edgeNormal(j, i);
                const float wij = dot(c - a, ni), wji = dot(a - c, nj);
                bij = (a * 2.0f + c - ni * (sh * wij)) * (1.0f / 3.0f);
                bji = (c * 2.0f + a - nj * (sh * wji)) * (1.0f / 3.0f);
            }

            // a point of edge (i, j) at t from i: evaluated from the end with the smaller position key (same bits from
            // either side)
            V3 edgePoint(uint32_t i, uint32_t j, float t) const
            {
                if (key[j] < key[i] || (key[j] == key[i] && j < i))
                {
                    std::swap(i, j);
                    t = 1.0f - t;
                }
                V3 bij, bji;
                edgeControl(i, j, bij, bji);
                const float u = 1.0f - t;
                return P[i] * (u * u * u) + bij * (3.0f * u * u * t) + bji * (3.0f * u * t * t) + P[j] * (t * t * t);
            }

            V3 toBatch(V3 p) const
            {
                return {(p.x - b.pos_offset[0]) / b.pos_scale[0], (p.y - b.pos_offset[1]) / b.pos_scale[1],
                        (p.z - b.pos_offset[2]) / b.pos_scale[2]};
            }

            // bone weights of a blend (w0 * a + w1 * b + w2 * c): the four heaviest bones, renormalised, in the batch's
            // encoding (float bits with the bone in the low bits)
            void blendWeights(const uint32_t *ids, const float *f, int n, uint32_t out[4]) const
            {
                uint32_t bone[12];
                float wt[12];
                int m = 0;
                for (int v = 0; v < n; ++v)
                    for (int k = 0; k < 4; ++k)
                    {
                        const uint32_t bits = b.weights[ids[v]][k];
                        float w;
                        std::memcpy(&w, &bits, 4);
                        w *= f[v];
                        if (!(w > 0.0f))
                            continue;
                        const uint32_t id = bits & b.bone_mask;
                        int at = 0;
                        while (at < m && bone[at] != id)
                            ++at;
                        if (at == m)
                            bone[m] = id, wt[m++] = 0.0f;
                        wt[at] += w;
                    }
                // the four heaviest
                for (int a = 0; a < m; ++a)
                    for (int c = a + 1; c < m; ++c)
                        if (wt[c] > wt[a])
                            std::swap(wt[a], wt[c]), std::swap(bone[a], bone[c]);
                m = std::min(m, 4);
                float sum = 0.0f;
                for (int a = 0; a < m; ++a)
                    sum += wt[a];
                const uint32_t first = m ? bone[0] : (b.weights[ids[0]][0] & b.bone_mask);
                for (int k = 0; k < 4; ++k)
                {
                    const float w = (k < m && sum > 0.0f) ? wt[k] / sum : 0.0f;
                    uint32_t bits;
                    std::memcpy(&bits, &w, 4);
                    out[k] = (bits & ~b.bone_mask) | ((k < m ? bone[k] : first) & b.bone_mask);
                }
            }

            uint16_t addVertex(V3 p, V3 shadeUnit, float nl, const float uv[3], const uint32_t *w)
            {
                const V3 q = toBatch(p);
                opos.insert(opos.end(), {q.x, q.y, q.z});
                const V3 n = shadeUnit * nl;
                onrm.insert(onrm.end(), {n.x, n.y, n.z});
                ouv.insert(ouv.end(), {uv[0], uv[1], uv[2]});
                if (b.weights)
                    ow.insert(ow.end(), {w[0], w[1], w[2], w[3]});
                return static_cast<uint16_t>(opos.size() / 3u - 1u);
            }

            // shading normal at a blend of vertices: their normals interpolated, blurred toward the shared ones
            V3 shade(const uint32_t *ids, const float *f, int n) const
            {
                V3 v{0, 0, 0}, c{0, 0, 0};
                for (int k = 0; k < n; ++k)
                {
                    v = v + N[ids[k]] * f[k];
                    c = c + info[ids[k]].canon * f[k];
                }
                return norm(norm(v) * (1.0f - kNormalBlur) + norm(c) * kNormalBlur);
            }

            uint16_t vertexOnEdge(uint32_t i, uint32_t j, int k, int n)
            {
                if (k == 0)
                    return static_cast<uint16_t>(i);
                if (k == n)
                    return static_cast<uint16_t>(j);
                if (j < i) // index-oriented sharing inside the batch
                {
                    std::swap(i, j);
                    k = n - k;
                }
                const uint64_t ek = (uint64_t(i) << 40) | (uint64_t(j) << 16) | uint64_t(k);
                auto it = edgeVerts.find(ek);
                if (it != edgeVerts.end())
                    return it->second;
                const float t = float(k) / float(n);
                const uint32_t ids[2] = {i, j};
                const float f[2] = {1.0f - t, t};
                float uv[3];
                for (int c = 0; c < 3; ++c)
                    uv[c] = float(b.uv[i][c]) * f[0] + float(b.uv[j][c]) * f[1];
                uint32_t w[4] = {};
                if (b.weights)
                    blendWeights(ids, f, 2, w);
                const uint16_t v = addVertex(edgePoint(i, j, t), shade(ids, f, 2), nLen[i] * f[0] + nLen[j] * f[1], uv, w);
                edgeVerts.emplace(ek, v);
                return v;
            }

            void emit(uint16_t v, bool &first, bool &stitch)
            {
                if (stitch)
                {
                    idx.push_back(idx.back());
                    idx.push_back(v);
                    stitch = false;
                }
                idx.push_back(v);
                first = false;
            }

            void triangle(uint32_t a, uint32_t bb, uint32_t c, int n)
            {
                // control points of the patch (edges as edgePoint has them) for interior points
                V3 b210, b120, b021, b012, b102, b201;
                edgeControl(a, bb, b210, b120);
                edgeControl(bb, c, b021, b012);
                edgeControl(c, a, b102, b201);
                const V3 E = (b210 + b120 + b021 + b012 + b102 + b201) * (1.0f / 6.0f);
                const V3 V = (P[a] + P[bb] + P[c]) * (1.0f / 3.0f);
                const V3 b111 = E + (E - V) * 0.5f;
                // grid (i along a->b, j along a->c)
                uint16_t grid[5][5];
                for (int j = 0; j <= n; ++j)
                    for (int i = 0; i + j <= n; ++i)
                    {
                        if (j == 0)
                            grid[i][j] = vertexOnEdge(a, bb, i, n);
                        else if (i == 0)
                            grid[i][j] = vertexOnEdge(a, c, j, n);
                        else if (i + j == n)
                            grid[i][j] = vertexOnEdge(bb, c, j, n);
                        else
                        {
                            const float u = float(i) / n, v = float(j) / n, w = 1.0f - u - v;
                            const V3 p = P[a] * (w * w * w) + P[bb] * (u * u * u) + P[c] * (v * v * v) +
                                         b210 * (3 * w * w * u) + b120 * (3 * w * u * u) + b201 * (3 * w * w * v) +
                                         b021 * (3 * u * u * v) + b102 * (3 * w * v * v) + b012 * (3 * u * v * v) +
                                         b111 * (6 * w * u * v);
                            const uint32_t ids[3] = {a, bb, c};
                            const float f[3] = {w, u, v};
                            float uv[3];
                            for (int k = 0; k < 3; ++k)
                                uv[k] = float(b.uv[a][k]) * w + float(b.uv[bb][k]) * u + float(b.uv[c][k]) * v;
                            uint32_t wt[4] = {};
                            if (b.weights)
                                blendWeights(ids, f, 3, wt);
                            grid[i][j] = addVertex(p, shade(ids, f, 3), nLen[a] * w + nLen[bb] * u + nLen[c] * v, uv, wt);
                        }
                    }
                // rows as strips, joined by degenerate triangles
                bool first = idx.empty(), stitch = !idx.empty();
                for (int j = 0; j < n; ++j)
                {
                    for (int i = 0; i < n - j; ++i)
                    {
                        emit(grid[i][j], first, stitch);
                        emit(grid[i][j + 1], first, stitch);
                    }
                    emit(grid[n - j][j], first, stitch);
                    stitch = true;
                }
            }

            // level 1 over a run of T strip triangles (vertices s .. s+T+1): every triangle splits into its corners' three
            // and a centre one, which form two strips along the run - one on each side of the line through the midpoints
            // of the rungs (k, k+1): ~4 strip vertices per original triangle instead of 12 for triangle-by-triangle rows
            void band(uint32_t s0, uint32_t T)
            {
                auto V = [s0](uint32_t k) { return static_cast<uint16_t>(s0 + k); };
                auto M = [this, s0](uint32_t i, uint32_t j) { return vertexOnEdge(s0 + i, s0 + j, 1, 2); };
                for (uint32_t p = 0; p < 2u; ++p)
                {
                    bool first = idx.empty(), stitch = !idx.empty();
                    if (p == 0)
                    {
                        emit(V(0), first, stitch);
                        emit(M(0, 1), first, stitch);
                    }
                    else
                    {
                        emit(M(0, 1), first, stitch);
                        emit(V(1), first, stitch);
                    }
                    for (uint32_t k = 0; k < T; ++k)
                    {
                        if ((k & 1u) == p)
                        {
                            emit(M(k, k + 2u), first, stitch); // the side edge's midpoint
                            emit(M(k + 1u, k + 2u), first, stitch);
                            emit(V(k + 2u), first, stitch);
                        }
                        else
                            emit(M(k + 1u, k + 2u), first, stitch);
                    }
                }
            }

            bool run(State::Entry &e)
            {
                const uint32_t count = b.count;
                P.resize(count);
                N.resize(count);
                nLen.resize(count);
                key.resize(count);
                info.resize(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    P[i] = realPos(b.pos[i]);
                    const V3 n{float(b.nrm[i][0]), float(b.nrm[i][1]), float(b.nrm[i][2])};
                    nLen[i] = len(n);
                    N[i] = norm(n);
                    key[i] = mix(mix(mix(space, uint32_t(b.pos[i][0])), uint32_t(b.pos[i][1])), uint32_t(b.pos[i][2]));
                    PosInfo &pi = info[i];
                    auto it = s.positions.find(key[i]);
                    if (it == s.positions.end() || it->second.count == 0)
                    {
                        pi.canon = N[i];
                        continue;
                    }
                    const NormalSet &ns = it->second;
                    pi.set = &ns;
                    int32_t sum[3] = {0, 0, 0};
                    V3 units[6];
                    for (int k = 0; k < ns.count; ++k)
                    {
                        units[k] = norm(V3{float(ns.n[k][0]), float(ns.n[k][1]), float(ns.n[k][2])});
                        for (int c = 0; c < 3; ++c)
                            sum[c] += ns.n[k][c];
                    }
                    pi.canon = norm(V3{float(sum[0]), float(sum[1]), float(sum[2])});
                    for (int k = 0; k < ns.count && !pi.crease; ++k)
                        for (int m = k + 1; m < ns.count; ++m)
                            if (dot(units[k], units[m]) < creaseCos)
                            {
                                pi.crease = true;
                                break;
                            }
                }
                // the originals first, exactly as they came
                opos.clear(), onrm.clear(), ouv.clear(), ow.clear(), idx.clear();
                for (uint32_t i = 0; i < count; ++i)
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        opos.push_back(float(b.pos[i][c]));
                        onrm.push_back(float(b.nrm[i][c]));
                        ouv.push_back(float(b.uv[i][c]));
                    }
                    if (b.weights)
                        ow.insert(ow.end(), {b.weights[i][0], b.weights[i][1], b.weights[i][2], b.weights[i][3]});
                }
                const int n = 1 << level;
                uint32_t tris = 0;
                auto degenerate = [this](uint32_t t) {
                    return key[t] == key[t + 1u] || key[t + 1u] == key[t + 2u] || key[t] == key[t + 2u];
                };
                for (uint32_t t = 0; t + 2u < b.draw_count;)
                {
                    if (degenerate(t)) // a join of the strip's runs
                    {
                        ++t;
                        continue;
                    }
                    const uint32_t start = t;
                    while (t + 2u < b.draw_count && !degenerate(t))
                        ++t;
                    if (n == 2)
                        band(start, t - start); // the run as two strips
                    else
                        for (uint32_t k = start; k < t; ++k)
                            triangle(k, k + 1u, k + 2u, n);
                    tris += t - start;
                    if (opos.size() / 3u > 60000u || idx.size() > 200000u)
                        return false;
                }
                if (idx.size() < 3u)
                    return false;
                s.trianglesIn += tris;
                s.trianglesOut += uint64_t(tris) * n * n;
                e.pos = std::move(opos);
                e.nrm = std::move(onrm);
                e.uv = std::move(ouv);
                e.w = std::move(ow);
                e.idx = std::move(idx);
                e.bytes = (e.pos.size() + e.nrm.size() + e.uv.size() + e.w.size()) * 4u + e.idx.size() * 2u + 128u;
                return true;
            }
        };

        uint64_t batchKey(const VcsLitBatch &b, uint32_t generation, int level, int strength)
        {
            uint64_t h = mix(mix(mix(0x4D534D4F4F5448ull, uint64_t(b.kind)), b.count | (uint64_t(b.draw_count) << 32)),
                             generation | (uint64_t(level) << 32) | (uint64_t(uint32_t(strength)) << 40));
            uint32_t sc[8];
            std::memcpy(sc, b.pos_scale, 16);
            std::memcpy(sc + 4, b.pos_offset, 16);
            for (uint32_t v : sc)
                h = mix(h, v);
            auto words = [&h](const void *p, size_t bytes) {
                const auto *q = static_cast<const uint64_t *>(p);
                for (size_t i = 0; i < bytes / 8u; ++i)
                    h = mix(h, q[i]);
            };
            words(b.pos, size_t(b.count) * 16u);
            words(b.nrm, size_t(b.count) * 16u);
            words(b.uv, size_t(b.count) * 16u);
            if (b.weights)
                words(b.weights, size_t(b.count) * 16u);
            return h;
        }

        uint64_t spaceKey(const VcsLitBatch &b)
        {
            uint32_t sc[8];
            std::memcpy(sc, b.pos_scale, 16);
            std::memcpy(sc + 4, b.pos_offset, 16);
            uint64_t h = mix(0x504F53ull, b.kind == VCS_LIT_SKINNED ? 1u : 2u);
            for (int i = 0; i < 3; ++i)
                h = mix(mix(h, sc[i]), sc[4 + i]);
            return h;
        }

        // the batch's normals into the position table (until it is refined)
        void notePositions(State &s, const VcsLitBatch &b, uint64_t space)
        {
            if (s.positions.size() > 1000000u)
                s.positions.clear();
            if (s.edges.size() > 2000000u)
                s.edges.clear();
            auto pk = [&](uint32_t i) {
                return mix(mix(mix(space, uint32_t(b.pos[i][0])), uint32_t(b.pos[i][1])), uint32_t(b.pos[i][2]));
            };
            for (uint32_t t = 0; t + 2u < b.draw_count; ++t)
            {
                const uint64_t k[3] = {pk(t), pk(t + 1u), pk(t + 2u)};
                if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2])
                    continue;
                for (int e = 0; e < 3; ++e)
                {
                    State::EdgeSeen &es = s.edges[edgeKey(k[e], k[(e + 1) % 3])];
                    const uint64_t opp = k[(e + 2) % 3];
                    if (es.count >= 2 || (es.count == 1 && es.opposite[0] == opp))
                        continue;
                    es.opposite[es.count++] = opp;
                }
            }
            for (uint32_t i = 0; i < b.count; ++i)
            {
                const uint64_t k =
                    mix(mix(mix(space, uint32_t(b.pos[i][0])), uint32_t(b.pos[i][1])), uint32_t(b.pos[i][2]));
                NormalSet &ns = s.positions[k];
                const int16_t n[3] = {int16_t(b.nrm[i][0]), int16_t(b.nrm[i][1]), int16_t(b.nrm[i][2])};
                bool have = false;
                for (int m = 0; m < ns.count && !have; ++m)
                    have = ns.n[m][0] == n[0] && ns.n[m][1] == n[1] && ns.n[m][2] == n[2];
                if (!have && ns.count < 6)
                {
                    std::memcpy(ns.n[ns.count], n, sizeof(n));
                    ++ns.count;
                }
            }
        }

        void evict(State &s)
        {
            std::vector<std::pair<uint64_t, uint64_t>> byAge;
            byAge.reserve(s.cache.size());
            for (auto &kv : s.cache)
                byAge.emplace_back(kv.second->lastUse, kv.first);
            std::sort(byAge.begin(), byAge.end());
            for (size_t i = 0; i < byAge.size() / 4u + 1u && i < byAge.size(); ++i)
            {
                auto it = s.cache.find(byAge[i].second);
                s.cacheBytes -= it->second->bytes;
                s.cache.erase(it);
            }
        }

        const VcsLitRefined *refine(void *, const VcsLitBatch *bp)
        {
            if (!bp || bp->size < sizeof(VcsLitBatch) || !g_enabled.load(std::memory_order_relaxed))
                return nullptr;
            const VcsLitBatch &b = *bp;
            const int kind = b.kind;
            if (kind < 0 || kind > 2)
                return nullptr;
            if (kind == VCS_LIT_SKINNED ? !g_peds.load(std::memory_order_relaxed)
                : kind == VCS_LIT_ENV   ? !g_vehicles.load(std::memory_order_relaxed)
                                        : !g_others.load(std::memory_order_relaxed))
                return nullptr;
            if (b.draw_count < 3u || (kind == VCS_LIT_SKINNED && !b.weights))
                return nullptr;
            for (int k = 0; k < 3; ++k)
                if (!(std::fabs(b.pos_scale[k]) > 1e-12f))
                    return nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            const int level = std::clamp(g_level.load(std::memory_order_relaxed), 1, 2);
            const int strength = std::clamp(g_strength.load(std::memory_order_relaxed), 0, 150);
            const uint64_t key = batchKey(b, g_generation.load(std::memory_order_relaxed), level, strength);
            State &s = st();
            std::lock_guard<std::mutex> lock(s.lock);
            ++s.calls[kind];
            ++s.clock;
            auto it = s.cache.find(key);
            State::Entry *e = it != s.cache.end() ? it->second.get() : nullptr;
            if (!e)
            {
                const uint64_t space = spaceKey(b);
                uint8_t &seen = s.sightings[key];
                if (seen < 255)
                    ++seen;
                if (seen < kSightingsBeforeRefine)
                {
                    notePositions(s, b, space);
                    if (s.sightings.size() > 1000000u)
                        s.sightings.clear();
                    s.lookupMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    return nullptr;
                }
                if (seen == 255) // refused before (too big): leave it
                    return nullptr;
                auto fresh = std::make_unique<State::Entry>();
                const float shape = (kind == VCS_LIT_SKINNED ? kPedShape : kVehicleShape) * float(strength) / 100.0f;
                Refiner r{b, level, shape, kind == VCS_LIT_SKINNED ? kPedCreaseCos : kVehicleCreaseCos, space, s};
                const bool ok = r.run(*fresh);
                s.buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (!ok)
                {
                    seen = 255;
                    return nullptr;
                }
                ++s.built;
                s.sightings.erase(key);
                s.cacheBytes += fresh->bytes;
                e = fresh.get();
                s.cache.emplace(key, std::move(fresh));
                if (s.cacheBytes > kCacheBudget)
                {
                    e->lastUse = s.clock;
                    evict(s);
                    auto again = s.cache.find(key);
                    if (again == s.cache.end())
                        return nullptr;
                    e = again->second.get();
                }
            }
            else
                s.lookupMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            e->lastUse = s.clock;
            ++s.refined[kind];
            VcsLitRefined &o = s.out;
            o.key = key;
            o.vertex_count = static_cast<uint32_t>(e->pos.size() / 3u);
            o.index_count = static_cast<uint32_t>(e->idx.size());
            o.pos = reinterpret_cast<const float (*)[3]>(e->pos.data());
            o.nrm = reinterpret_cast<const float (*)[3]>(e->nrm.data());
            o.uv = reinterpret_cast<const float (*)[3]>(e->uv.data());
            o.weights = e->w.empty() ? nullptr : reinterpret_cast<const uint32_t (*)[4]>(e->w.data());
            o.indices = e->idx.data();
            return &o;
        }

        void clearCache()
        {
            State &s = st();
            std::lock_guard<std::mutex> lock(s.lock);
            s.cache.clear();
            s.cacheBytes = 0;
            s.sightings.clear();
        }

        void changed()
        {
            bump();
            clearCache();
        }

        void toggleKey(void *)
        {
            mod::set("meshsmooth.enabled", g_enabled.load() ? 0 : 1);
            mod::log("[meshsmooth] %s\n", g_enabled.load() ? "on" : "off");
        }
    }

    void registerHotkeyAndCommands()
    {
        VcsModHotkey key = {};
        key.size = sizeof(key);
        key.key = "controls.meshsmooth_key";
        key.label = "Mesh smoothing";
        key.help = "Switches the smoothing of peds and vehicles on and off (Graphics page, MeshSmooth).";
        key.default_key = 260; // Insert, as the San Andreas plugin
        key.pad_chord = VCS_MOD_PAD_NONE;
        key.pressed = &toggleKey;
        mod::api->add_hotkey(&key);

        // rig / gamectl: "meshsmooth" = stats, "meshsmooth on|off", "meshsmooth reset" (stats)
        mod::addCommand("meshsmooth", [](const std::string &args, mod::Reply reply) {
            if (args == "on" || args == "off")
            {
                if (!mod::set("meshsmooth.enabled", args == "on" ? 1 : 0))
                {
                    reply("error refused (an environment override?)");
                    return;
                }
            }
            State &s = st();
            std::lock_guard<std::mutex> lock(s.lock);
            char line[512];
            std::snprintf(line, sizeof(line),
                          "ok enabled=%d level=%d strength=%d calls=%llu/%llu/%llu refined=%llu/%llu/%llu built=%llu "
                          "tris_in=%llu tris_out=%llu cache=%zu entries %.1f MB positions=%zu build_ms=%.1f lookup_ms=%.1f",
                          g_enabled.load(), g_level.load(), g_strength.load(), (unsigned long long)s.calls[0],
                          (unsigned long long)s.calls[1], (unsigned long long)s.calls[2], (unsigned long long)s.refined[0],
                          (unsigned long long)s.refined[1], (unsigned long long)s.refined[2], (unsigned long long)s.built,
                          (unsigned long long)s.trianglesIn, (unsigned long long)s.trianglesOut, s.cache.size(),
                          s.cacheBytes / 1048576.0, s.positions.size(), s.buildMs, s.lookupMs);
            if (args == "reset")
            {
                std::memset(s.calls, 0, sizeof(s.calls));
                std::memset(s.refined, 0, sizeof(s.refined));
                s.buildMs = s.lookupMs = 0.0;
            }
            reply(line);
        });
    }

    void install() { mod::api->set_lit_refiner(&refine, nullptr); }

    void registerSettings()
    {
        const char *section = "Mesh smoothing (MeshSmooth)";
        mod::Spec s;
        s.key = "meshsmooth.enabled";
        s.page = "Graphics";
        s.section = section;
        s.label = "Smooth models";
        s.help = "Rounds the low-poly peds and vehicles: their triangles split and curved (PN triangles), cached per model. "
                 "Off is the original. Key: Insert.";
        s.type = mod::Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_MESHSMOOTH";
        s.onChange = [](int v) {
            g_enabled = v ? 1 : 0;
        };
        s.read = [] { return g_enabled.load(); };
        mod::add(s);

        s = {};
        s.key = "meshsmooth.peds";
        s.page = "Graphics";
        s.section = section;
        s.label = "Peds";
        s.help = "Smooth the people (skinned models): Vic, peds, cutscene characters.";
        s.type = mod::Type::Toggle;
        s.defaultValue = 1;
        s.onChange = [](int v) {
            g_peds = v ? 1 : 0;
        };
        mod::add(s);

        s = {};
        s.key = "meshsmooth.vehicles";
        s.page = "Graphics";
        s.section = section;
        s.label = "Vehicles";
        s.help = "Experimental, off by default: smooth vehicle bodies (the parts with the shiny environment map). The "
                 "game draws some layers of a car (licence plate, dirt) over the original mesh, which then shows through.";
        s.type = mod::Type::Toggle;
        s.defaultValue = 0;
        s.onChange = [](int v) {
            g_vehicles = v ? 1 : 0;
        };
        mod::add(s);

        s = {};
        s.key = "meshsmooth.others";
        s.page = "Graphics";
        s.section = section;
        s.label = "Other lit models";
        s.help = "Smooth the other lit models too: vehicle parts without the shine (bumpers, wheels), weapons, props. Off by "
                 "default: decals on them (licence plates) sink into the curved parts.";
        s.type = mod::Type::Toggle;
        s.defaultValue = 0;
        s.onChange = [](int v) {
            g_others = v ? 1 : 0;
        };
        mod::add(s);

        s = {};
        s.key = "meshsmooth.level";
        s.page = "Graphics";
        s.section = section;
        s.label = "Level";
        s.help = "How finely each triangle is split: 1 = into 4 (as the San Andreas plugin's default), 2 = into 16.";
        s.type = mod::Type::Integer;
        s.min = 1;
        s.max = 2;
        s.defaultValue = 1;
        s.onChange = [](int v) {
            g_level = std::clamp(v, 1, 2);
            changed();
        };
        mod::add(s);

        s = {};
        s.key = "meshsmooth.strength";
        s.page = "Graphics";
        s.section = section;
        s.label = "Strength";
        s.help = "How much the surface curves between the original corners (0 = only split, 100 = full curvature).";
        s.type = mod::Type::Integer;
        s.min = 0;
        s.max = 150;
        s.format = "%d %%";
        s.defaultValue = 100;
        s.onChange = [](int v) {
            g_strength = std::clamp(v, 0, 150);
            changed();
        };
        mod::add(s);
    }
}
