// Painting the terrain by MATERIAL (grass, sand, rock...) with the transitions chosen automatically: every tile
// of the vanilla textures is known by the ground at each of its four corners (tile_materials_generated.hpp): a
// plain tile has one ground, the transitions the artists blended two (a corner, a half, an inner corner, under
// the four .mpr rotations), and the tiles they laid where three grounds meet have three. Here the other way
// round: the tile for four corner materials.
// Corners are the tile's four vertices: bits 0 NW, 1 NE, 2 SW, 3 SE (north = +y, the .mpr's rows).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "tile_materials_generated.hpp"

namespace tilemat {

// A material to paint: a family/kind ("grass/dark"); its variants ("grass/dark/a", "/b"...) are base tiles.
struct Material {
    std::string name;
    std::vector<int> baseTiles;   // the plain tiles of it (any variant), to paint a full cell with
};

inline int RotateCw(int pattern) { // NW <- SW, NE <- NW, SE <- NE, SW <- SE
    const int nw = pattern & 1, ne = (pattern >> 1) & 1, sw = (pattern >> 2) & 1, se = (pattern >> 3) & 1;
    return sw | (nw << 1) | (se << 2) | (ne << 3);
}
inline std::array<int, 4> RotateCornersCw(const std::array<int, 4>& c) { return {c[2], c[0], c[3], c[1]}; }

struct Terrain {
    std::string name;
    std::vector<Material> materials;
    // Per tile: the material id (index into `materials`) at each corner, -1 unknown; for a tile of one or two
    // materials also the older form a / b / pattern (the corners showing b), which the blending tools use.
    struct Info { int16_t c[4] = {-1, -1, -1, -1}; int16_t a = -1, b = -1; int8_t pattern = -1; bool known = false; };
    std::vector<Info> tiles;      // by tile index (texture * 64 + tile)
    std::vector<int> blendTiles;  // the tiles with exactly two materials
    std::vector<int> multiTiles;  // the tiles with three or more
    std::map<std::array<int, 4>, std::vector<std::pair<int, int>>> bySignature; // corners -> the (tile, rotation) showing them

    const Info* At(int tile) const { return tile >= 0 && tile < static_cast<int>(tiles.size()) ? &tiles[static_cast<size_t>(tile)] : nullptr; }
    int MaterialId(const std::string& name) {
        for (size_t i = 0; i < materials.size(); ++i) if (materials[i].name == name) return static_cast<int>(i);
        materials.push_back({name, {}});
        return static_cast<int>(materials.size()) - 1;
    }
    int FindMaterial(const std::string& name) const {
        for (size_t i = 0; i < materials.size(); ++i) if (materials[i].name == name) return static_cast<int>(i);
        return -1;
    }
    // Registers a tile by its four corner materials (ids), at rotation 0.
    void SetCorners(int tile, const std::array<int, 4>& c) {
        if (tile < 0 || c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0) return;
        if (tile >= static_cast<int>(tiles.size())) tiles.resize(static_cast<size_t>(tile) + 1);
        Info& info = tiles[static_cast<size_t>(tile)];
        if (info.known) return; // the vanilla data first; a sidecar cannot redefine a tile
        info.known = true;
        for (int k = 0; k < 4; ++k) info.c[k] = static_cast<int16_t>(c[k]);
        int distinct[4], n = 0;
        for (int k = 0; k < 4; ++k) {
            int i = 0;
            while (i < n && distinct[i] != c[k]) ++i;
            if (i == n) distinct[n++] = c[k];
        }
        if (n == 1) {
            info.a = static_cast<int16_t>(c[0]); info.pattern = 0;
            materials[static_cast<size_t>(c[0])].baseTiles.push_back(tile);
        } else if (n == 2) {
            info.a = static_cast<int16_t>(c[0]); info.b = static_cast<int16_t>(distinct[1]);
            int p = 0;
            for (int k = 0; k < 4; ++k) if (c[k] == info.b) p |= 1 << k;
            info.pattern = static_cast<int8_t>(p);
            blendTiles.push_back(tile);
        } else {
            multiTiles.push_back(tile);
        }
        std::array<int, 4> r = c;
        for (int rot = 0; rot < 4; ++rot, r = RotateCornersCw(r)) {
            std::vector<std::pair<int, int>>& fits = bySignature[r];
            bool dup = false;
            for (const auto& f : fits) dup |= f.first == tile; // a symmetric tile fits one signature at two rotations: once
            if (!dup) fits.push_back({tile, rot});
        }
    }
    // Adds a base tile of `material` (a tile copied from another terrain, or one the user names).
    void AddBase(int tile, const std::string& material) {
        const int id = MaterialId(material);
        SetCorners(tile, {id, id, id, id});
    }
    // Adds a transition tile from `a` to `b`: `pattern` = the corners showing b at rotation 0 (bits 0 NW, 1 NE, 2 SW, 3 SE).
    void AddBlend(int tile, const std::string& a, const std::string& b, int pattern) {
        if (tile < 0 || pattern < 0) return;
        const int ia = MaterialId(a), ib = MaterialId(b);
        std::array<int, 4> c;
        for (int k = 0; k < 4; ++k) c[k] = (pattern >> k) & 1 ? ib : ia;
        SetCorners(tile, c);
    }
};

inline std::string Family(const char* material) { // "grass/dark/a" -> "grass/dark"
    std::string s = material;
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(0, slash);
}

// The table of a terrain by the name inside its .mpr (zone3xobr's is zone3obr: they share the textures), built
// once; nullptr when the vanilla data has no entry for it (a custom terrain: paint by tile instead).
inline Terrain* Lookup(const std::string& terrainName) {
    static std::map<std::string, Terrain> cache;
    std::string key = terrainName;
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.tiles.empty() ? nullptr : &it->second;
    Terrain& t = cache[key];
    t.name = key;
    for (int i = 0; i < kTableCount; ++i) {
        if (key != kTables[i].terrain) continue;
        for (int e = 0; e < kTables[i].count; ++e) {
            const Entry& en = kTables[i].entries[e];
            std::array<int, 4> c;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
                if (en.c[k] < 0 || en.c[k] >= kMaterialCount) { ok = false; break; }
                c[k] = t.MaterialId(Family(kMaterials[en.c[k]]));
            }
            if (ok) t.SetCorners(en.tile, c);
        }
        break;
    }
    return t.tiles.empty() ? nullptr : &t;
}

// The table of a terrain the vanilla data does not know (a custom one), empty until a sidecar fills it.
inline Terrain& Create(const std::string& terrainName) {
    static std::map<std::string, Terrain> own;
    std::string key = terrainName;
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    Terrain& t = own[key];
    t.name = key;
    return t;
}

// The sidecar of a terrain's BORROWED tiles (<folder>/<terrain>-materials.tsv, written by the Map Editor when it
// copies another allod's material into free tiles): one line per tile, "tile<TAB>a<TAB>b<TAB>pattern" (b empty
// for a plain tile). Merged into the table so the next session still knows what those tiles are made of.
inline int LoadSidecar(Terrain& t, const std::string& path) {
    std::ifstream in(path);
    int n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t at = 0;
        while (true) { const size_t tab = line.find('\t', at); f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at)); if (tab == std::string::npos) break; at = tab + 1; }
        if (f.size() < 4) continue;
        const int tile = std::atoi(f[0].c_str());
        if (f[2].empty()) t.AddBase(tile, f[1]); else t.AddBlend(tile, f[1], f[2], std::atoi(f[3].c_str()));
        ++n;
    }
    return n;
}
inline void AppendSidecar(const std::string& path, int tile, const std::string& a, const std::string& b, int pattern) {
    std::ofstream out(path, std::ios::app);
    out << tile << '\t' << a << '\t' << b << '\t' << pattern << '\n';
}

// Every terrain the table knows (for borrowing another allod's materials).
inline std::vector<std::string> KnownTerrains() {
    std::vector<std::string> names;
    for (int i = 0; i < kTableCount; ++i) names.push_back(kTables[i].terrain);
    return names;
}

// The materials at the four corners of a placed (packed) tile, -1 where unknown.
inline void Corners(const Terrain& t, uint16_t packed, int out[4]) {
    const int tile = ((packed >> 6) & 0xFF) * 64 + (packed & 63), rotation = (packed >> 14) & 3;
    const Terrain::Info* info = t.At(tile);
    if (!info || !info->known) { out[0] = out[1] = out[2] = out[3] = -1; return; }
    std::array<int, 4> c = {info->c[0], info->c[1], info->c[2], info->c[3]};
    for (int r = 0; r < rotation; ++r) c = RotateCornersCw(c);
    for (int k = 0; k < 4; ++k) out[k] = c[k];
}

inline uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// The tile (texture * 64 + tile) and rotation whose corners are `corners` (material ids, none -1): a base tile
// when all four are one material, else the transition (or three-ground tile) showing exactly those corners.
// False when the textures have no such tile.
inline bool Solve(const Terrain& t, const int corners[4], uint32_t hash, int& tile, int& rotation) {
    if (corners[0] < 0 || corners[1] < 0 || corners[2] < 0 || corners[3] < 0) return false;
    if (corners[1] == corners[0] && corners[2] == corners[0] && corners[3] == corners[0]) {
        const std::vector<int>& bases = t.materials[static_cast<size_t>(corners[0])].baseTiles;
        if (bases.empty()) return false;
        tile = bases[hash % bases.size()];
        rotation = static_cast<int>((hash >> 8) % 4);
        return true;
    }
    const auto it = t.bySignature.find({corners[0], corners[1], corners[2], corners[3]});
    if (it == t.bySignature.end() || it->second.empty()) return false;
    const auto& pick = it->second[hash % it->second.size()];
    tile = pick.first;
    rotation = pick.second;
    return true;
}

// Whether the textures hold any transition between the two materials (to warn before painting).
inline bool CanBlend(const Terrain& t, int a, int b) {
    if (a == b) return true;
    for (int candidate : t.blendTiles) {
        const Terrain::Info& info = t.tiles[static_cast<size_t>(candidate)];
        if ((info.a == a && info.b == b) || (info.a == b && info.b == a)) return true;
    }
    return false;
}

// A ground that blends with both `a` and `b` (a band of it can stand between them where they have no
// transition of their own, or not the shape needed), the one with most plain tiles; -1 when none.
inline int Bridge(const Terrain& t, int a, int b) {
    int best = -1;
    for (size_t m = 0; m < t.materials.size(); ++m) {
        const int id = static_cast<int>(m);
        if (id == a || id == b || t.materials[m].baseTiles.empty() || !CanBlend(t, a, id) || !CanBlend(t, id, b)) continue;
        if (best < 0 || t.materials[m].baseTiles.size() > t.materials[static_cast<size_t>(best)].baseTiles.size()) best = id;
    }
    return best;
}

} // namespace tilemat
