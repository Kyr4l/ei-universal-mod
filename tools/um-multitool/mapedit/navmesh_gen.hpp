// The game's navmesh (AI_GRAPH) generator, as game.exe builds it (reverse-engineered from the game:
// CAIMap::Load 0x5B43C0, the per-tile passability 0x5B8FA0 / 0x5B8BA0, objects 0x5B6A80, the graph
// 0x5AFDA0 .. 0x5B0050; EI_Plugin's GraphGen only makes the game build it and saves the result).
//
//   tiles   0.5 x 0.5 world units (64 per sector side): a height (the terrain at 511 / maxZ steps), a water
//           depth, a ground material (aiinfo.res tileDesc.reg) and the volumes of the objects over it
//           (the boxes of their figure parts). Per AI layer (8 unit classes), a value 0..15: how easy it is
//           to walk there (0: not at all), eroded by the class's footprint, cut at cliffs.
//   graph   cells of 8 x 8 tiles (4 x 4 units). Per layer and cell: a representative tile (B), the cost to
//           the 8 neighbour cells' representative tiles (A, a shortest path over the tiles, 0xFFFF: none)
//           and the connected component (C).
//   file    W, H (cells), then per layer and cell row: A (W x 8 u16), B (W bytes), C (W u16).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "mpr_file.hpp"
#include "../viewer/figure_format.hpp"

namespace navgen {

constexpr int kLayers = 8;
constexpr int kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1}; // the graph's directions (as mob::kAiDx / kAiDy)
constexpr int kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// Ground materials' CostMul (aiinfo.res, tileDesc.reg): GRASS GROUND STONE SAND ROCK FIELD WATER ROAD ASTRAL SNOW
// ICE DRYGRASS SNOWBALLS Lava Swamp, then the game's default (1).
constexpr int kCostMul[16] = {2, 2, 2, 2, 2, 2, 8, 1, 2, 2, 2, 2, 2, 1, 50, 1};

// A figure part as the generator sees it: its box in the figure (bounds blended by the complection, plus the
// part's offset), and what the part is: an obstacle, a floor units walk on ("base..." parts) or nothing
// ("crown..." parts: tree tops; "empty..." parts are left out).
struct PartBox {
    enum Kind { Obstacle, Floor, Ignored };
    fig::Vec3 min, max;
    Kind kind = Obstacle;
};
struct Object {
    fig::Vec3 position;  // as in the map: z above the ground
    fig::Quat rotation;
    std::vector<PartBox> parts;
};

inline PartBox::Kind PartKind(const std::string& name, bool& skip) {
    auto starts = [&](const char* p) {
        const size_t n = std::strlen(p);
        if (name.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::toupper(static_cast<unsigned char>(name[i])) != p[i]) return false;
        return true;
    };
    skip = starts("EMPTY");
    if (starts("BASE")) return PartBox::Floor;
    if (starts("CROWN")) return PartBox::Ignored;
    return PartBox::Obstacle;
}

struct Generator {
    // ---- the tile map ------------------------------------------------------------------------------
    int tw = 0, th = 0;             // tiles
    float scale = 1;                // tile height units per world unit (511 / maxZ)
    std::vector<uint16_t> height;   // per tile
    std::vector<uint16_t> water;    // bits 0-5 depth, 6 on a floor object, 7 water surface
    std::vector<uint8_t> material;
    struct Volume { int lo, hi, type; };
    std::unordered_map<int, std::vector<Volume>> volumes; // per tile index
    std::vector<uint32_t> value;    // per tile: 8 nibbles, one per layer
    int factor[16] = {};            // a tile value's step cost factor (1024 = 1)
    int slope[2][1023] = {};        // [layer 0 / others][dh + 511]: the step factor for a height change, -1 too steep
    int climb40 = 0, climb60 = 0;   // the height steps the slope tables allow
    // ---- the graph ---------------------------------------------------------------------------------
    int gw = 0, gh = 0;
    std::vector<uint16_t> A[kLayers]; // gw * gh * 8
    std::vector<uint8_t> B[kLayers];
    std::vector<uint16_t> C[kLayers];

    int Index(int x, int y) const { return y * tw + x; }
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < tw && y < th; }
    int Nibble(int x, int y, int layer) const { return (value[Index(x, y)] >> (layer * 4)) & 15; }

    static int Round(double v) { return static_cast<int>(std::nearbyint(v)); }

    // ---- terrain (CAIMap::Load) ---------------------------------------------------------------------
    bool LoadTerrain(const mpr::Map& m, std::string& err) {
        if (m.sectorsX <= 0 || m.sectorsY <= 0 || m.maxZ <= 0) { err = "no terrain"; return false; }
        tw = m.sectorsX * 64;
        th = m.sectorsY * 64;
        scale = static_cast<float>(511.0 / m.maxZ);
        height.assign(static_cast<size_t>(tw) * th, 0);
        water.assign(height.size(), 0);
        material.assign(height.size(), 0);
        value.assign(height.size(), 0);
        volumes.clear();
        const float k = static_cast<float>(m.maxZ * (1.0 / 65535.0));
        const int tiles = std::max(m.textureCount, 0) * 64;
        auto typeOf = [&](uint16_t tile) -> int {
            const int i = tile & 0x3FFF;
            return i < tiles && i < static_cast<int>(m.tileTypes.size()) ? (m.tileTypes[i] & 0xFF) : 15;
        };
        for (int sy = 0; sy < m.sectorsY; ++sy)
            for (int sx = 0; sx < m.sectorsX; ++sx) {
                const mpr::Sector* s = m.At(sx, sy);
                if (!s) continue;
                const bool hasWaterVerts = (s->type & 1) != 0, hasWaterTiles = (s->type & 3) != 0;
                for (int r = 0; r < 32; ++r)
                    for (int c = 0; c < 32; ++c) {
                        auto quad = [&](const mpr::Vertex (&v)[33][33], float t[4]) {
                            const float h00 = static_cast<float>(v[r][c].z * static_cast<double>(k)), h10 = static_cast<float>(v[r][c + 1].z * static_cast<double>(k));
                            const float h01 = static_cast<float>(v[r + 1][c].z * static_cast<double>(k)), h11 = static_cast<float>(v[r + 1][c + 1].z * static_cast<double>(k));
                            const float a = static_cast<float>((static_cast<double>(h01) + h10) * 0.25), b = static_cast<float>((static_cast<double>(h00) + h11) * 0.25);
                            t[0] = static_cast<float>(h00 * 0.5 + a); // (2r, 2c)
                            t[1] = static_cast<float>(h10 * 0.5 + b); // (2r, 2c + 1)
                            t[2] = static_cast<float>(h01 * 0.5 + b); // (2r + 1, 2c)
                            t[3] = static_cast<float>(h11 * 0.5 + a); // (2r + 1, 2c + 1)
                        };
                        float land[4];
                        quad(s->land, land);
                        float wat[4];
                        if (hasWaterVerts) quad(s->waterVerts, wat);
                        for (int q = 0; q < 4; ++q) {
                            const int x = sx * 64 + c * 2 + (q & 1), y = sy * 64 + r * 2 + (q >> 1);
                            const int i = Index(x, y);
                            height[i] = static_cast<uint16_t>(Round(static_cast<float>(land[q] * static_cast<double>(scale))));
                            if (hasWaterVerts) {
                                const int d = Round((static_cast<double>(wat[q]) - land[q]) * scale);
                                water[i] = static_cast<uint16_t>(d < 63 ? std::max(d, 0) : 63);
                            }
                        }
                    }
                for (int r = 0; r < 16; ++r)
                    for (int c = 0; c < 16; ++c) {
                        const int mat = typeOf(s->landTiles[r][c]);
                        bool surface = false;
                        if (hasWaterVerts) {
                            const int wm = s->waterMaterial[r][c];
                            surface = wm >= 0 && wm < static_cast<int>(m.materials.size()) && (m.materials[wm].type == 2 || m.materials[wm].type == 3);
                        }
                        const int wmat = hasWaterTiles && s->waterTiles[r][c] != 0xFFFF ? typeOf(s->waterTiles[r][c]) : -1;
                        for (int dy = 0; dy < 4; ++dy)
                            for (int dx = 0; dx < 4; ++dx) {
                                const int i = Index(sx * 64 + c * 4 + dx, sy * 64 + r * 4 + dy);
                                material[i] = static_cast<uint8_t>(mat);
                                if (surface && (water[i] & 0x3F)) water[i] |= 0x80;
                                if (wmat >= 0 && (water[i] & 0x80) && (water[i] & 0x3F)) material[i] = static_cast<uint8_t>(wmat);
                            }
                    }
            }
        BuildTables();
        return true;
    }

    void BuildTables() {
        static const int kFactor[16] = {-1, 0x6400, 0x77EC, 0x8BD8, 0x2FF8, 0x37F0, 0x3FFC, 0xDFC, 0xFFF, 0x11FD, 0x1400, 0x666, 0x732, 0x800, 0x400, 0x4CC};
        std::memcpy(factor, kFactor, sizeof factor);
        // In float: the game runs with the FPU at single precision (Direct3D's default).
        const float pi = 3.14159265358979f;
        for (int dh = -511; dh <= 511; ++dh) {
            const float dz = std::fabs(static_cast<float>(dh) / scale);
            const float deg = std::fabs(std::atan2(dz, 0.5f) * (180.0f / pi));
            const float s = std::sin(pi * (1.0f / 180.0f) * deg);
            const float g = (s * s + 1.0f) * (s * s + 1.0f);
            const float f = dh >= 0 ? g : 1.0f / std::sqrt(g);
            slope[0][dh + 511] = deg <= 60.0f ? 0x400 : -1;
            slope[1][dh + 511] = deg <= 40.0f ? Round(f * 1024.0f) : -1;
        }
        climb40 = climb60 = 0;
        while (climb60 <= 511 && slope[0][511 + climb60] >= 0) ++climb60;
        while (climb40 <= 511 && slope[1][511 + climb40] >= 0) ++climb40;
    }

    // ---- objects (0x5B6A80): each part's box, its faces sampled about 3 times per tile ---------------
    struct Rec { int lo = 0x7FFFFFFF, hi = 0, type = 0; };
    void AddObject(const Object& o) {
        std::unordered_map<int, Rec> recs[2]; // obstacles, floors
        std::vector<int> order[2];
        // The object stands on the ground at its tile (the terrain's, under a floor object too).
        const int ox = Round(o.position.x * 2.0f - 0.5f), oy = Round(o.position.y * 2.0f - 0.5f);
        const float ground = In(ox, oy) ? groundAt(ox, oy) / scale : 0.0f;
        const fig::Vec3 at{o.position.x, o.position.y, o.position.z + ground};
        static const fig::Vec3 kNormals[6] = {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, -1, 0}};
        for (const PartBox& p : o.parts) {
            const int list = p.kind == PartBox::Floor ? 1 : 0;
            const int type = p.kind == PartBox::Ignored ? 2 : 0;
            const fig::Vec3 n = p.min, x = p.max;
            const fig::Vec3 local[8] = {{x.x, n.y, n.z}, {n.x, n.y, n.z}, {n.x, x.y, n.z}, {n.x, x.y, x.z},
                                        {x.x, x.y, x.z}, {x.x, n.y, x.z}, {x.x, n.y, n.z}, {n.x, n.y, n.z}};
            fig::Vec3 c[8];
            for (int i = 0; i < 8; ++i) c[i] = fig::QuatRotate(o.rotation, local[i]) + at;
            for (int f = 0; f < 6; ++f) {
                const fig::Vec3 org = c[f + 1], e1 = c[f] - c[f + 1], e2 = c[f + 2] - c[f + 1];
                const bool top = fig::QuatRotate(o.rotation, kNormals[f]).z > 0.0f;
                const int n1 = Round(std::sqrt(e1.x * e1.x + e1.y * e1.y) * 2.0f * 1.5f) + 2;
                const int n2 = Round(std::sqrt(e2.x * e2.x + e2.y * e2.y) * 2.0f * 1.5f) + 2;
                for (int i = 0; i < n1; ++i)
                    for (int j = 0; j < n2; ++j) {
                        const float a = (i + 0.5f) / n1, b = (j + 0.5f) / n2;
                        const fig::Vec3 s{org.x + e1.x * a + e2.x * b, org.y + e1.y * a + e2.y * b, org.z + e1.z * a + e2.z * b};
                        const int tx = Round(s.x * 2.0f - 0.5f), ty = Round(s.y * 2.0f - 0.5f);
                        if (!In(tx, ty)) continue;
                        int z = Round(s.z * scale);
                        z = z < 0x3FD ? std::max(z, 0) : 0x3FC;
                        const int key = Index(tx, ty);
                        auto it = recs[list].find(key);
                        if (it == recs[list].end()) {
                            Rec r;
                            if (top) r.hi = z; else r.lo = z;
                            r.type = type;
                            recs[list].emplace(key, r);
                            order[list].push_back(key);
                        } else if (top) {
                            it->second.hi = std::max(it->second.hi, z);
                            it->second.type = std::min(it->second.type, type);
                        } else {
                            it->second.lo = std::min(it->second.lo, z);
                        }
                    }
            }
        }
        // Floors first: the ground rises to their top, dry, a road.
        for (int key : order[1]) {
            const Rec& r = recs[1][key];
            if (r.lo >= r.hi) continue;
            if (!(water[key] & 0x40)) baseHeight_[key] = height[key];
            water[key] = static_cast<uint16_t>((water[key] & 0xFF40) | 0x40);
            height[key] = static_cast<uint16_t>(std::max<int>(height[key], r.hi));
            material[key] = 7;
            volumes[key].push_back({r.lo, r.hi, 4});
        }
        for (int key : order[0]) {
            const Rec& r = recs[0][key];
            if (r.lo < r.hi) volumes[key].push_back({r.lo, r.hi, r.type});
        }
    }
    std::unordered_map<int, uint16_t> baseHeight_; // the terrain's height under floor objects
    float groundAt(int x, int y) const {
        auto it = baseHeight_.find(Index(x, y));
        return it != baseHeight_.end() ? it->second : height[Index(x, y)];
    }

    // ---- per tile values (0x5B8FA0) -------------------------------------------------------------------
    int TileValue(int x, int y, int layer, int thr) const {
        static const int kLut[5][16] = {{0, 0, 1, 1, 1, 1, 4, 7, 11, 12, 14, 14, 15, 15, 15, 15},
                                        {0, 0, 1, 1, 1, 1, 4, 7, 11, 12, 13, 13, 13, 13, 13, 13},
                                        {0, 0, 1, 1, 1, 1, 4, 7, 8, 9, 10, 10, 10, 10, 10, 10},
                                        {0, 0, 1, 1, 1, 1, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6},
                                        {0, 0, 1, 1, 1, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3}};
        auto row = [](int cost) { return cost < 10 ? (cost < 3 ? (cost > 1 ? 1 : 0) : 2) : (cost > 34 ? 4 : 3); };
        const int i = Index(x, y);
        const int mat = material[i] & 31;
        const int costMul = mat < 16 ? kCostMul[mat] : 1;
        const int depth = water[i] & 0x3F;
        auto vit = volumes.find(i);
        const bool none = vit == volumes.end() || vit->second.empty();
        if (none && depth == 0) {
            const int cost = layer == 0 ? 1 : costMul;
            return cost >= 0x47 ? 0 : kLut[row(cost)][10];
        }
        int lo = height[i], speed = 0x400, cost;
        bool dbl = false;
        if (layer == 0) {
            if (depth) lo += depth;
            cost = 1;
        } else {
            cost = costMul;
            const int lim = layer != 1 ? thr : 0;
            if (depth) {
                if (depth == 0x3F || lim - 1 <= depth) speed = -1;
                else if (lim < 0x400) {
                    if (lim / 2 < depth) { speed = 800; cost += 30; }
                    else speed = 0x370;
                }
            }
        }
        const int hi = lo + thr;
        if (!none)
            for (const Volume& v : vit->second) {
                if (v.type == 2 || v.type == 4) continue;
                if (v.type == 3) { dbl = true; continue; }
                if (speed < 0 || v.lo > hi || lo > v.hi) continue;
                static const int kA[2] = {290, 20}, kB[2] = {7, 2};
                const float f = static_cast<float>(std::min(v.hi, hi) - std::max(v.lo, lo)) / static_cast<float>(hi - lo);
                const int t = v.type < 2 ? v.type : 0;
                cost += Round(static_cast<float>(kA[t]) * f);
                speed = Round(static_cast<float>(std::pow(0.6, kB[t] * static_cast<double>(f)) * speed));
            }
        if (speed < 0 || cost > 70) return 0;
        const int s = std::min(15, (speed + 50) / 102);
        if (dbl) cost *= 2;
        return kLut[row(cost)][s];
    }

    void ComputeValues() {
        static const float kStep[8] = {2.0f, 0.4f, 1.0f, 1.6f, 2.2f, 1.7f, 3.0f, 6.0f};
        static const int kFoot1[][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        static const int kFoot2[][2] = {{-2, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -2}, {0, -1}, {0, 0}, {0, 1}, {0, 2}, {1, -1}, {1, 0}, {1, 1}, {2, 0}};
        static const int kFoot3[][2] = {{-3, 0}, {-2, -2}, {-2, -1}, {-2, 0}, {-2, 1}, {-2, 2}, {-1, -2}, {-1, -1}, {-1, 0}, {-1, 1},
                                        {-1, 2}, {0, -3}, {0, -2}, {0, -1}, {0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, -2}, {1, -1},
                                        {1, 0}, {1, 1}, {1, 2}, {2, -2}, {2, -1}, {2, 0}, {2, 1}, {2, 2}, {3, 0}};
        std::fill(value.begin(), value.end(), 0u);
        std::vector<uint8_t> temp(value.size());
        for (int layer = 0; layer < kLayers; ++layer) {
            const int thr = Round(static_cast<float>(kStep[layer] * scale));
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) temp[Index(x, y)] = static_cast<uint8_t>(TileValue(x, y, layer, thr));
            const int (*foot)[2] = layer < 5 ? kFoot1 : layer < 7 ? kFoot2 : kFoot3;
            const int feet = layer < 5 ? 5 : layer < 7 ? 13 : 29;
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) {
                    int v = temp[Index(x, y)];
                    for (int k = 0; k < feet && v; ++k) {
                        const int nx = x + foot[k][0], ny = y + foot[k][1];
                        if (In(nx, ny) && temp[Index(nx, ny)] == 0) v = 0;
                    }
                    value[Index(x, y)] |= static_cast<uint32_t>(v) << (layer * 4);
                }
        }
        // Cliffs (0x5B8710): too steep for anything, or for all but layer 0; the big classes keep away. As the
        // game does it: layers 5-6 are cleared on one tile only, the neighbour that found the cliff (or, for the
        // lesser one, the tile 2 to the east, the table's next entry), 4 times over.
        static const int kAround[5][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, 0}};
        for (int y = 2; y <= th - 3; ++y)
            for (int x = 2; x <= tw - 3; ++x) {
                int m = 0, k = 0;
                bool all = false;
                for (; k < 4; ++k) {
                    const int d = std::abs(height[Index(x + kAround[k][0], y + kAround[k][1])] - height[Index(x, y)]);
                    if (d > m) {
                        m = d;
                        if (m > climb60 - 1) { all = true; break; }
                    }
                }
                if (!all && m <= climb40 - 1) continue;
                value[Index(x, y)] &= all ? 0u : 0xFu;
                value[Index(x + kAround[k][0], y + kAround[k][1])] &= 0xF00FFFFFu;
                for (const auto& d : kFoot2) value[Index(x + d[0], y + d[1])] &= 0x0FFFFFFFu;
            }
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x)
                if (x < 3 || y < 3 || x >= tw - 3 || y >= th - 3) value[Index(x, y)] = 0;
    }

    // ---- shortest paths over the tiles (0x5C2B10 .. 0x5C2D40) ----------------------------------------
    // From (sx, sy) inside the window [x0, x1] x [y0, y1]; dist in the window's row-major order, INT_MAX: not
    // reached.
    void Search(int layer, int x0, int y0, int x1, int y1, int sx, int sy, std::vector<int>& dist) const {
        const int ww = x1 - x0 + 1, wh = y1 - y0 + 1;
        dist.assign(static_cast<size_t>(ww) * wh, 0x7FFFFFFF);
        using Item = std::pair<int, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        dist[(sy - y0) * ww + (sx - x0)] = 0;
        q.push({0, (sy - y0) * ww + (sx - x0)});
        const int* sl = slope[layer == 0 ? 0 : 1];
        while (!q.empty()) {
            const auto [d, at] = q.top();
            q.pop();
            if (d != dist[at]) continue;
            const int cx = x0 + at % ww, cy = y0 + at / ww;
            for (int k = 0; k < 8; ++k) {
                const int nx = cx + kDx[k], ny = cy + kDy[k];
                if (nx < x0 || ny < y0 || nx > x1 || ny > y1 || !In(nx, ny)) continue;
                const int ni = (ny - y0) * ww + (nx - x0);
                if (dist[ni] <= d) continue;
                const int f1 = factor[Nibble(nx, ny, layer)];
                if (f1 < 0) continue;
                const int dh = height[Index(nx, ny)] - height[Index(cx, cy)];
                if (dh < -511 || dh > 511) continue;
                const int f2 = sl[dh + 511];
                if (f2 < 0) continue;
                const int step = ((f2 * f1) >> 10) * ((kDx[k] && kDy[k]) ? 0x5A8 : 0x400) >> 10;
                if (d + step < dist[ni]) { dist[ni] = d + step; q.push({d + step, ni}); }
            }
        }
    }

    // ---- the graph (0x5AFDA0) -------------------------------------------------------------------------
    void BuildGraph() {
        gw = tw / 8;
        gh = th / 8;
        std::vector<int> dist;
        for (int layer = 0; layer < kLayers; ++layer) {
            A[layer].assign(static_cast<size_t>(gw) * gh * 8, 0xFFFF);
            B[layer].assign(static_cast<size_t>(gw) * gh, 0x33);
            C[layer].assign(static_cast<size_t>(gw) * gh, 0);
            // Representative tiles: the best from the centre out, among those that cross their cell.
            for (int cy = 0; cy < gh; ++cy)
                for (int cx = 0; cx < gw; ++cx) {
                    bool found = false;
                    int best = 0;
                    for (int ring = 4; ring >= 0; --ring) {
                        const int lo = ring, hi = 8 - ring;
                        for (int ly = lo; ly < hi; ++ly)
                            for (int lx = lo; lx < hi; ++lx) {
                                const int x = cx * 8 + lx, y = cy * 8 + ly;
                                if (!In(x, y)) continue;
                                const int v = Nibble(x, y, layer);
                                if (!v || (found && v <= best)) continue;
                                const bool ok = Crosses(layer, cx, cy, x, y, dist);
                                if (!ok && found) continue;
                                found = ok;
                                B[layer][cy * gw + cx] = static_cast<uint8_t>(lx | (ly << 4));
                                best = v;
                            }
                    }
                }
            // Costs to the neighbours' representative tiles.
            for (int cy = 0; cy < gh; ++cy)
                for (int cx = 0; cx < gw; ++cx) {
                    const uint8_t b = B[layer][cy * gw + cx];
                    const int x0 = cx * 8 - 8, y0 = cy * 8 - 8;
                    Search(layer, x0, y0, cx * 8 + 16, cy * 8 + 16, cx * 8 + (b & 15), cy * 8 + (b >> 4), dist);
                    for (int k = 0; k < 8; ++k) {
                        const int nx = cx + kDx[k], ny = cy + kDy[k];
                        uint16_t cost = 0xFFFF;
                        if (nx >= 0 && ny >= 0 && nx < gw && ny < gh) {
                            const uint8_t nb = B[layer][ny * gw + nx];
                            const int tx = nx * 8 + (nb & 15), ty = ny * 8 + (nb >> 4);
                            const int d = dist[(ty - y0) * 25 + (tx - x0)];
                            if (d < 0x7FFFFFFF) cost = static_cast<uint16_t>(std::min(0x7FFF, d >> 7));
                        }
                        A[layer][(static_cast<size_t>(cy) * gw + cx) * 8 + k] = cost;
                    }
                }
            // Both ways the same (the lower, "none" winning), none off the map.
            auto a = [&](int x, int y, int k) -> uint16_t& { return A[layer][(static_cast<size_t>(y) * gw + x) * 8 + k]; };
            for (int y = 0; y < gh; ++y)
                for (int k : {1, 2, 3}) { a(0, y, k) = 0xFFFF; a(gw - 1, y, k + 4) = 0xFFFF; }
            for (int x = 0; x < gw; ++x)
                for (int k : {7, 0, 1}) { a(x, 0, k) = 0xFFFF; a(x, gh - 1, (k + 4) & 7) = 0xFFFF; }
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x)
                    for (int k = 0; k < 8; ++k) {
                        const int nx = x + kDx[k], ny = y + kDy[k];
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                        const int16_t m = std::min(static_cast<int16_t>(a(x, y, k)), static_cast<int16_t>(a(nx, ny, (k + 4) & 7)));
                        a(x, y, k) = a(nx, ny, (k + 4) & 7) = static_cast<uint16_t>(m);
                    }
            // Connected components, numbered from 1 in row order.
            uint16_t next = 1;
            std::vector<int> stack;
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    if (C[layer][y * gw + x]) continue;
                    C[layer][y * gw + x] = next;
                    stack.assign(1, y * gw + x);
                    while (!stack.empty()) {
                        const int at = stack.back();
                        stack.pop_back();
                        for (int k = 0; k < 8; ++k) {
                            if (static_cast<int16_t>(A[layer][static_cast<size_t>(at) * 8 + k]) < 0) continue;
                            const int nx = at % gw + kDx[k], ny = at / gw + kDy[k];
                            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || C[layer][ny * gw + nx]) continue;
                            C[layer][ny * gw + nx] = next;
                            stack.push_back(ny * gw + nx);
                        }
                    }
                    ++next;
                }
        }
    }

    // Whether a candidate representative tile reaches across its cell (0x5AF7B0): both the left and right
    // edges, or both the top and bottom ones.
    bool Crosses(int layer, int cx, int cy, int sx, int sy, std::vector<int>& dist) const {
        const int x0 = cx * 8, y0 = cy * 8;
        Search(layer, x0, y0, x0 + 8, y0 + 8, sx, sy, dist);
        auto reached = [&](int x, int y) { return dist[(y - y0) * 9 + (x - x0)] < 0x7FFFFFFF; };
        bool left = false, right = false, top = false, bottom = false;
        for (int i = 0; i <= 8; ++i) {
            left |= reached(x0, y0 + i);
            right |= reached(x0 + 8, y0 + i);
            top |= reached(x0 + i, y0);
            bottom |= reached(x0 + i, y0 + 8);
        }
        return (left && right) || (top && bottom);
    }

    // The AI_GRAPH node's payload.
    std::vector<uint8_t> Payload() const {
        std::vector<uint8_t> out;
        out.reserve(8 + static_cast<size_t>(kLayers) * gw * gh * 19);
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8))); };
        auto u16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
        u32(static_cast<uint32_t>(gw));
        u32(static_cast<uint32_t>(gh));
        for (int layer = 0; layer < kLayers; ++layer)
            for (int y = 0; y < gh; ++y) {
                for (int x = 0; x < gw; ++x)
                    for (int k = 0; k < 8; ++k) u16(A[layer][(static_cast<size_t>(y) * gw + x) * 8 + k]);
                for (int x = 0; x < gw; ++x) out.push_back(B[layer][y * gw + x]);
                for (int x = 0; x < gw; ++x) u16(C[layer][y * gw + x]);
            }
        return out;
    }
};

// The tile map only (no graph): what units walk on, per layer (Generator::Nibble, factor).
inline bool BuildTiles(const mpr::Map& terrain, const std::vector<Object>& objects, Generator& g, std::string& err) {
    if (!g.LoadTerrain(terrain, err)) return false;
    for (const Object& o : objects) g.AddObject(o);
    g.ComputeValues();
    return true;
}

// The whole generation: terrain, then the objects, then the tile values and the graph.
inline bool Generate(const mpr::Map& terrain, const std::vector<Object>& objects, std::vector<uint8_t>& payload, std::string& err) {
    Generator g;
    if (!g.LoadTerrain(terrain, err)) return false;
    for (const Object& o : objects) g.AddObject(o);
    g.ComputeValues();
    g.BuildGraph();
    payload = g.Payload();
    if (const char* dbg = std::getenv("NAVGEN_DEBUG")) { // "layer,cellX,cellY": the cell's tiles (development)
        int layer = 0, cx = 0, cy = 0;
        if (std::sscanf(dbg, "%d,%d,%d", &layer, &cx, &cy) == 3 && cx >= 0 && cy >= 0 && cx < g.gw && cy < g.gh) {
            std::printf("cell %d,%d layer %d: B=%02X\n", cx, cy, layer, g.B[layer][cy * g.gw + cx]);
            for (int ly = 7; ly >= 0; --ly) {
                for (int lx = 0; lx < 8; ++lx) {
                    const int x = cx * 8 + lx, y = cy * 8 + ly, i = g.Index(x, y);
                    auto it = g.volumes.find(i);
                    std::printf(" %2d/%3d/%c%c", g.Nibble(x, y, layer), g.height[i], it != g.volumes.end() ? 'V' : '-', (g.water[i] & 0x3F) ? 'W' : '-');
                }
                std::printf("\n");
            }
        }
    }
    return true;
}

} // namespace navgen
