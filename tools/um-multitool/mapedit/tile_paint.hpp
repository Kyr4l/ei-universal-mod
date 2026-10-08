// Painting a ground with the brush: the cells under it become plain tiles of the ground, the cells around get
// the tiles their corners then need (tile_materials.hpp): the transitions, and where three grounds meet the
// artists' three-ground tiles. Where the textures have no tile for a cell's corners, two fallbacks, each an
// option:
//  - `repair`: a cell the textures have no tile for (three grounds meeting, or a shape they lack for two, a
//    concave notch mostly): one of its vertices that is not painted takes whichever ground gives the four
//    cells around it a tile each, the cell's own grounds and the painted one tried first (the artists' notch
//    grounds turn up this way: zone 7 puts a fourth ground at the notches of its grass);
//  - `bridge`: the painted ground against one the textures cannot blend it with (no transition, or not this
//    shape): a one-vertex band of a middle ground that blends with both is laid between them (zone 7's artists
//    ring every grass patch on watered rock with light stone the same way).
// Only vertices around the brush are moved, never a cell farther away. What is still unsolvable is left.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "mpr_file.hpp"
#include "tile_autoblend.hpp"
#include "tile_materials.hpp"

namespace tilemat {

struct PaintResult {
    int changed = 0;        // cells given another tile
    int repaired = 0;       // corners moved where three grounds met
    int bridged = 0;        // corners given a middle ground
    int left = 0;           // cells left as they were: no tile for their corners
    std::string bridges;    // the chains laid ("grass/dark > stone/light > rock/watered")
    std::set<int> sectors;  // the sectors changed
    std::vector<std::pair<int, int>> leftCells; // the cells left (x, y)
    std::string Message() const {
        std::string msg;
        if (repaired) msg += std::to_string(repaired) + " corner(s) moved where three grounds met";
        if (bridged) msg += (msg.empty() ? "" : "; ") + std::to_string(bridged) + " corner(s) bridged (" + bridges + ")";
        if (left) msg += (msg.empty() ? "" : "; ") + std::to_string(left) + " cell(s) left: the textures have no tile for the grounds meeting there";
        return msg;
    }
};

// Paints `brush` (a material id of `t`) in a disc of `radius` cells (1 = the cell alone) around cell (gx, gy).
// `beforeSectorChange(sector)` runs once per sector before its first tile changes (for the undo copy).
inline void PaintGround(const Terrain& t, mpr::Map& m, int gx, int gy, int radius, int brush, uint32_t seed, bool repair, bool bridge,
                        PaintResult& res, const std::function<void(int)>& beforeSectorChange = nullptr) {
    const int W = m.sectorsX * 16, H = m.sectorsY * 16, R = std::clamp(radius, 1, 16) - 1;
    auto painted = [&](int cx, int cy) { const int dx = cx - gx, dy = cy - gy; return dx * dx + dy * dy <= R * R + R; }; // a disc of cells
    auto cellPtr = [&](int cx, int cy) -> uint16_t* {
        if (cx < 0 || cy < 0 || cx >= W || cy >= H) return nullptr;
        mpr::Sector& s = m.sectors[static_cast<size_t>((cy / 16) * m.sectorsX + cx / 16)];
        return s.present ? &s.landTiles[cy % 16][cx % 16] : nullptr;
    };
    // The area: the brush, a ring for the transitions, one for the fallbacks' bands and one for their neighbours.
    const int x0 = gx - R - 3, y0 = gy - R - 3, x1 = gx + R + 3, y1 = gy + R + 3;
    const int AW = x1 - x0 + 1, AH = y1 - y0 + 1, VW = AW + 1, VH = AH + 1;
    auto vIndex = [&](int vx, int vy) { return static_cast<size_t>(vy - y0) * VW + (vx - x0); }; // vertex (vx, vy): the SW corner of cell (vx, vy)
    // 1. The ground at every vertex of the area: what the tiles' corners say (the majority), -1 unknown.
    std::vector<int> votes(static_cast<size_t>(VW) * VH * 4, 0); // up to four different grounds per vertex
    std::vector<int> voteMat(static_cast<size_t>(VW) * VH * 4, -1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
            const uint16_t* p = cellPtr(cx, cy);
            if (!p) continue;
            int c[4];
            Corners(t, *p, c);
            for (int k = 0; k < 4; ++k) {
                if (c[k] < 0) continue;
                const size_t v = vIndex(cx + (k & 1), cy + (k < 2 ? 1 : 0)) * 4;
                for (int i = 0; i < 4; ++i) {
                    if (voteMat[v + i] == c[k] || voteMat[v + i] < 0) { voteMat[v + i] = c[k]; ++votes[v + i]; break; }
                }
            }
        }
    std::vector<int> vert(static_cast<size_t>(VW) * VH, -1);
    std::vector<uint8_t> fixed(vert.size(), 0); // painted vertices: never moved by a fallback
    std::vector<uint8_t> touched(vert.size(), 0); // vertices this paint changed: only the cells around them are re-tiled
    for (size_t v = 0; v < vert.size(); ++v) {
        int best = -1, bestVotes = 0;
        for (int i = 0; i < 4; ++i) if (voteMat[v * 4 + i] >= 0 && votes[v * 4 + i] > bestVotes) { best = voteMat[v * 4 + i]; bestVotes = votes[v * 4 + i]; }
        vert[v] = best;
    }
    // 2. The vertices of the painted cells take the brush's ground.
    for (int cy = gy - R; cy <= gy + R; ++cy)
        for (int cx = gx - R; cx <= gx + R; ++cx) {
            if (!painted(cx, cy) || !cellPtr(cx, cy)) continue;
            for (int k = 0; k < 4; ++k) { const size_t v = vIndex(cx + (k & 1), cy + (k < 2 ? 1 : 0)); vert[v] = brush; fixed[v] = 1; touched[v] = 1; }
        }
    // 3. The fallbacks. First the bridging: for every ground the painted one meets at a cell the textures have
    //    no tile for (no transition at all, or not this shape), a band one vertex wide of a middle ground that
    //    blends with both is laid all round the brush, where that ground stands next to a painted vertex: a rim,
    //    not a patch here and there. Then the repair: a cell by the brush with three grounds and no tile for
    //    them keeps the painted ground and the one holding most of its corners; the third gives way. A few
    //    rounds, as a moved vertex changes the cells around it; painted vertices never move.
    auto wanted = [&](int cx, int cy, int out[4]) { // the cell's corners from the field; the tile's own where unknown
        const uint16_t* p = cellPtr(cx, cy);
        int cur[4];
        Corners(t, *p, cur);
        for (int k = 0; k < 4; ++k) { out[k] = vert[vIndex(cx + (k & 1), cy + (k < 2 ? 1 : 0))]; if (out[k] < 0) out[k] = cur[k]; }
    };
    std::vector<uint8_t> nextToBrush(vert.size(), 0); // vertices next to a painted one (8 neighbours), the band's place
    for (int vy = y0 + 1; vy <= y1; ++vy)
        for (int vx = x0 + 1; vx <= x1; ++vx) {
            if (fixed[vIndex(vx, vy)]) continue;
            for (int dy = -1; dy <= 1 && !nextToBrush[vIndex(vx, vy)]; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (vx + dx >= x0 && vx + dx <= x1 + 1 && vy + dy >= y0 && vy + dy <= y1 + 1 && fixed[vIndex(vx + dx, vy + dy)]) { nextToBrush[vIndex(vx, vy)] = 1; break; }
        }
    auto touchesPainted = [&](int cx, int cy) { // a cell with a painted or band vertex
        for (int k = 0; k < 4; ++k) { const size_t v = vIndex(cx + (k & 1), cy + (k < 2 ? 1 : 0)); if (fixed[v] || nextToBrush[v]) return true; }
        return false;
    };
    int& repaired = res.repaired; int& bridged = res.bridged;
    std::string& bridges = res.bridges;
    if (bridge) {
        std::vector<int> toBridge; // the grounds the brush cannot be laid against here
        for (int cy = y0 + 1; cy <= y1 - 1; ++cy)
            for (int cx = x0 + 1; cx <= x1 - 1; ++cx) {
                if (!cellPtr(cx, cy) || !touchesPainted(cx, cy)) continue;
                int c[4], tile, rot;
                wanted(cx, cy, c);
                if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0 || Solve(t, c, 0, tile, rot)) continue;
                int other = -1; bool two = true;
                for (int k = 0; k < 4; ++k) { if (c[k] == brush) continue; if (other < 0) other = c[k]; else if (c[k] != other) two = false; }
                if (!two || other < 0 || std::find(toBridge.begin(), toBridge.end(), other) != toBridge.end() || Bridge(t, brush, other) < 0) continue;
                toBridge.push_back(other);
            }
        for (int other : toBridge) {
            const int mid = Bridge(t, brush, other);
            for (size_t v = 0; v < vert.size(); ++v)
                if (nextToBrush[v] && vert[v] == other) { vert[v] = mid; touched[v] = 1; ++bridged; }
            const std::string via = t.materials[static_cast<size_t>(brush)].name + " > " + t.materials[static_cast<size_t>(mid)].name + " > " + t.materials[static_cast<size_t>(other)].name;
            if (bridges.find(via) == std::string::npos) bridges += (bridges.empty() ? "" : ", ") + via;
        }
    }
    // The repair: for a cell by the brush the textures have no tile for, one of its vertices that is not
    // painted changes ground, to whichever ground gives the four cells around that vertex a tile each (the
    // cell's own grounds and the painted one tried first, then every other: the artists' notch grounds turn up
    // this way). A few rounds, as a moved vertex changes the cells around it; painted vertices never move.
    auto solvable = [&](int cx, int cy) {
        if (cx < x0 || cy < y0 || cx > x1 || cy > y1 || !cellPtr(cx, cy)) return true; // outside the area: not ours
        int c[4], tile, rot;
        wanted(cx, cy, c);
        if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0) return true; // unknown tiles: left alone
        return Solve(t, c, 0, tile, rot);
    };
    auto aroundSolved = [&](int vx, int vy) { // how many of the four cells around a vertex have a tile
        int n = 0;
        for (int dy = -1; dy <= 0; ++dy) for (int dx = -1; dx <= 0; ++dx) n += solvable(vx + dx, vy + dy) ? 1 : 0;
        return n;
    };
    std::vector<int> byUse; // every ground, the ones with most plain tiles first
    for (size_t i = 0; i < t.materials.size(); ++i) byUse.push_back(static_cast<int>(i));
    std::sort(byUse.begin(), byUse.end(), [&](int a, int b) { return t.materials[static_cast<size_t>(a)].baseTiles.size() > t.materials[static_cast<size_t>(b)].baseTiles.size(); });
    for (int round = 0; round < 6 && repair; ++round) {
        bool moved = false;
        for (int cy = y0 + 1; cy <= y1 - 1; ++cy)
            for (int cx = x0 + 1; cx <= x1 - 1; ++cx) {
                if (!cellPtr(cx, cy) || !touchesPainted(cx, cy) || solvable(cx, cy)) continue;
                int c[4];
                wanted(cx, cy, c);
                int bestV = -1, bestM = -1, bestScore = -1, bestVx = 0, bestVy = 0;
                for (int k = 0; k < 4; ++k) {
                    const int vx = cx + (k & 1), vy = cy + (k < 2 ? 1 : 0);
                    const size_t v = vIndex(vx, vy);
                    if (fixed[v] || vx <= x0 + 1 || vy <= y0 + 1 || vx >= x1 || vy >= y1) continue; // every cell around it inside the area
                    const int was = vert[v];
                    const int before = aroundSolved(vx, vy);
                    std::vector<int> candidates; // the cell's own grounds, the brush, then every other
                    for (int j = 0; j < 4; ++j) if (c[j] != was && std::find(candidates.begin(), candidates.end(), c[j]) == candidates.end()) candidates.push_back(c[j]);
                    if (std::find(candidates.begin(), candidates.end(), brush) == candidates.end()) candidates.push_back(brush);
                    for (int m : byUse) if (m != was && std::find(candidates.begin(), candidates.end(), m) == candidates.end()) candidates.push_back(m);
                    for (int m : candidates) {
                        vert[v] = m;
                        const int score = aroundSolved(vx, vy);
                        if (score > before && score > bestScore) { bestScore = score; bestV = static_cast<int>(v); bestM = m; bestVx = vx; bestVy = vy; }
                        if (score == 4) break;
                    }
                    vert[v] = was;
                    if (bestScore == 4) break;
                }
                if (bestV < 0) continue;
                vert[static_cast<size_t>(bestV)] = bestM;
                touched[static_cast<size_t>(bestV)] = 1;
                if (bestM == brush) fixed[static_cast<size_t>(bestV)] = 1;
                (void)bestVx; (void)bestVy;
                moved = true; ++repaired;
            }
        if (!moved) break;
    }
    // 4. Every cell of the area whose corners changed takes the tile they ask for.
    int& left = res.left;
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
            uint16_t* p = cellPtr(cx, cy);
            if (!p) continue;
            bool mine = false; // a cell around a vertex this paint changed; the others keep their tiles (a vanilla hard edge is not ours)
            for (int k = 0; k < 4 && !mine; ++k) mine = touched[vIndex(cx + (k & 1), cy + (k < 2 ? 1 : 0))] != 0;
            if (!mine) continue;
            int c[4], cur[4];
            wanted(cx, cy, c);
            Corners(t, *p, cur);
            if (c[0] == cur[0] && c[1] == cur[1] && c[2] == cur[2] && c[3] == cur[3] && cur[0] >= 0) continue;
            int tile, rot;
            if (!Solve(t, c, Hash(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), seed), tile, rot)) {
                ++left; res.leftCells.push_back({cx, cy});
                continue;
            }
            const uint16_t packed = PackPlacedTile(tile, rot);
            if (*p == packed) continue;
            const int s2 = (cy / 16) * m.sectorsX + cx / 16;
            if (res.sectors.insert(s2).second && beforeSectorChange) beforeSectorChange(s2);
            *p = packed;
            ++res.changed;
        }
}

} // namespace tilemat
