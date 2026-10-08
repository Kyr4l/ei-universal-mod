// Auto-blend of a whole terrain: finds the HARD EDGES (a vertex where plain tiles of two different grounds meet
// although the textures hold transition tiles for that pair: hand-painted maps have them) and gives the cells
// around each of them the vanilla transition tiles their corners need (tile_materials.hpp). Two grounds with no
// transition between them are meant to touch and are left alone; so are, unless `sameKind`, two kinds of one
// ground (grass/dark beside grass/common: the vanilla maps lay those side by side everywhere, zone7 has 487 such
// sides and 3 real ones), and every cell away from a hard edge: a vanilla terrain comes out unchanged.
// At a hard edge the ground that is RARER on the terrain keeps its painted cells; the transition band is laid
// into the commoner ground (a few rock cells painted on grass stay rock, the grass around them fades into it).
// A feature one cell wide of the commoner ground is swallowed by the band: the tiles cannot show it.
// Laying a band turns plain cells into transitions, which can move the edge next to them: the pass repeats
// until nothing changes (a few rounds), so running it again changes nothing.
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "mpr_file.hpp"
#include "tile_materials.hpp"

namespace tilemat {

struct AutoBlendResult {
    int rounds = 0;
    int hardVertices = 0;   // vertices where two plain grounds met (the first round)
    int cells = 0;          // cells touching one of them
    int changed = 0;        // cells given another tile
    int left = 0;           // cells left as they were: no transition for those grounds, or an unknown tile
    std::set<int> sectors;  // the sectors changed
};

inline bool SameKind(const Terrain& t, int a, int b) { // "grass/dark" and "grass/common": one ground, two kinds
    const std::string& na = t.materials[static_cast<size_t>(a)].name;
    const std::string& nb = t.materials[static_cast<size_t>(b)].name;
    const size_t sa = na.find('/'), sb = nb.find('/');
    return na.compare(0, sa, nb, 0, sb) == 0;
}

inline uint16_t PackPlacedTile(int tile, int rotation) {
    return static_cast<uint16_t>(((tile / 64) << 6) | (tile % 64) | ((rotation & 3) << 14));
}

// `beforeSectorChange(sector)` runs once per sector before its first tile changes (for the undo copy).
inline void BlendHardEdgesOnce(const Terrain& t, mpr::Map& m, uint32_t seed, bool sameKind, AutoBlendResult& res,
                               const std::function<void(int)>& beforeSectorChange) {
    const int W = m.sectorsX * 16, H = m.sectorsY * 16;
    const bool first = res.rounds++ == 0;
    auto cellAt = [&](int cx, int cy) -> uint16_t* {
        if (cx < 0 || cy < 0 || cx >= W || cy >= H) return nullptr;
        mpr::Sector& s = m.sectors[static_cast<size_t>((cy / 16) * m.sectorsX + cx / 16)];
        return s.present ? &s.landTiles[cy % 16][cx % 16] : nullptr;
    };
    // 1. The ground of every plain cell (-1: a transition tile, an unknown tile, no sector).
    std::vector<int> cellMat(static_cast<size_t>(W) * H, -1);
    std::vector<int> count(t.materials.size(), 0);
    for (int cy = 0; cy < H; ++cy)
        for (int cx = 0; cx < W; ++cx) {
            const uint16_t* p = cellAt(cx, cy);
            if (!p) continue;
            const Terrain::Info* info = t.At(((*p >> 6) & 0xFF) * 64 + (*p & 63));
            if (!info || info->pattern < 0 || info->b >= 0) continue;
            cellMat[static_cast<size_t>(cy) * W + cx] = info->a;
            ++count[static_cast<size_t>(info->a)];
        }
    // 2. The ground at every vertex: what the plain cells touching it say. Several grounds = a hard edge: the
    //    majority wins, a tie goes to the rarer ground of the terrain, then the lower id (deterministic).
    const int VW = W + 1, VH = H + 1;
    std::vector<int> vert(static_cast<size_t>(VW) * VH, -1);
    std::vector<uint8_t> hard(static_cast<size_t>(VW) * VH, 0);
    for (int vy = 0; vy < VH; ++vy)
        for (int vx = 0; vx < VW; ++vx) {
            int mats[4], votes[4], n = 0;
            for (int dy = -1; dy <= 0; ++dy)
                for (int dx = -1; dx <= 0; ++dx) {
                    const int cx = vx + dx, cy = vy + dy;
                    if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
                    const int mat = cellMat[static_cast<size_t>(cy) * W + cx];
                    if (mat < 0) continue;
                    int k = 0;
                    while (k < n && mats[k] != mat) ++k;
                    if (k == n) { mats[n] = mat; votes[n] = 0; ++n; }
                    ++votes[k];
                }
            if (n == 0) continue;
            bool blendable = false; // a hard edge only where the textures can show the transition
            for (int i = 0; i < n && !blendable; ++i)
                for (int j = i + 1; j < n; ++j)
                    if (CanBlend(t, mats[i], mats[j]) && (sameKind || !SameKind(t, mats[i], mats[j]))) { blendable = true; break; }
            if (n > 1 && !blendable) continue;
            int best = 0;
            for (int k = 1; k < n; ++k) {
                const bool better = votes[k] > votes[best] ||
                                    (votes[k] == votes[best] && (count[static_cast<size_t>(mats[k])] < count[static_cast<size_t>(mats[best])] ||
                                                                 (count[static_cast<size_t>(mats[k])] == count[static_cast<size_t>(mats[best])] && mats[k] < mats[best])));
                if (better) best = k;
            }
            vert[static_cast<size_t>(vy) * VW + vx] = mats[best];
            if (n > 1) { hard[static_cast<size_t>(vy) * VW + vx] = 1; if (first) ++res.hardVertices; }
        }
    // 3. Every cell touching a hard vertex takes the tile its four vertices now ask for.
    for (int cy = 0; cy < H; ++cy)
        for (int cx = 0; cx < W; ++cx) {
            uint16_t* p = cellAt(cx, cy);
            if (!p) continue;
            // corner k of the cell is the vertex (cx + (k & 1), cy + (k < 2)): bits 0 NW, 1 NE, 2 SW, 3 SE
            int wanted[4], current[4];
            bool touches = false;
            for (int k = 0; k < 4; ++k) {
                const size_t v = static_cast<size_t>(cy + (k < 2 ? 1 : 0)) * VW + (cx + (k & 1));
                wanted[k] = vert[v];
                touches |= hard[v] != 0;
            }
            if (!touches) continue;
            if (first) ++res.cells;
            Corners(t, *p, current);
            for (int k = 0; k < 4; ++k) if (wanted[k] < 0) wanted[k] = current[k]; // no plain cell there: keep what the tile shows
            if (wanted[0] == current[0] && wanted[1] == current[1] && wanted[2] == current[2] && wanted[3] == current[3]) continue;
            int tile, rotation;
            if (!Solve(t, wanted, Hash(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), seed), tile, rotation)) { if (first) ++res.left; continue; }
            const uint16_t packed = PackPlacedTile(tile, rotation);
            if (packed == *p) continue;
            const int si = (cy / 16) * m.sectorsX + cx / 16;
            if (res.sectors.insert(si).second && beforeSectorChange) beforeSectorChange(si);
            *p = packed;
            ++res.changed;
        }
}

// `beforeSectorChange(sector)` runs once per sector before its first tile changes (for the undo copy).
inline AutoBlendResult BlendHardEdges(const Terrain& t, mpr::Map& m, uint32_t seed, bool sameKind = false,
                                      const std::function<void(int)>& beforeSectorChange = nullptr) {
    AutoBlendResult res;
    if (m.sectorsX <= 0 || m.sectorsY <= 0 || m.sectors.size() < static_cast<size_t>(m.sectorsX * m.sectorsY)) return res;
    for (int round = 0; round < 8; ++round) {
        const int before = res.changed;
        BlendHardEdgesOnce(t, m, seed, sameKind, res, beforeSectorChange);
        if (res.changed == before) break;
    }
    return res;
}

} // namespace tilemat
