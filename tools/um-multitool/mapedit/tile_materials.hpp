// Painting the terrain by MATERIAL (grass, sand, rock...) with the transitions chosen automatically: the vanilla
// textures hold, for every pair of grounds the artists blended, the transition tiles (a corner, a half, an inner
// corner, under the four .mpr rotations). tile_materials_generated.hpp says what each tile is made of; here the
// other way round: the tile for four corner materials.
// Corners are the tile's four vertices: bits 0 NW, 1 NE, 2 SW, 3 SE (north = +y, the .mpr's rows).
#pragma once

#include <cstdint>
#include <cstring>
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

struct Terrain {
    std::string name;
    std::vector<Material> materials;
    struct Info { int16_t a = -1, b = -1; int8_t pattern = -1; }; // per tile: material ids (indexes into `materials`), -1 unknown
    std::vector<Info> tiles;      // by tile index (texture * 64 + tile)
    std::vector<int> blendTiles;  // the tiles with two materials (pattern >= 0)

    const Info* At(int tile) const { return tile >= 0 && tile < static_cast<int>(tiles.size()) ? &tiles[static_cast<size_t>(tile)] : nullptr; }
    int MaterialId(const std::string& name) {
        for (size_t i = 0; i < materials.size(); ++i) if (materials[i].name == name) return static_cast<int>(i);
        materials.push_back({name, {}});
        return static_cast<int>(materials.size()) - 1;
    }
    // Adds a base tile of `material` (a tile copied from another terrain, or one the user names).
    void AddBase(int tile, const std::string& material) {
        if (tile < 0) return;
        if (tile >= static_cast<int>(tiles.size())) tiles.resize(static_cast<size_t>(tile) + 1);
        const int id = MaterialId(material);
        tiles[static_cast<size_t>(tile)] = {static_cast<int16_t>(id), -1, 0};
        materials[static_cast<size_t>(id)].baseTiles.push_back(tile);
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
        int maxTile = 0;
        for (int e = 0; e < kTables[i].count; ++e) maxTile = std::max(maxTile, static_cast<int>(kTables[i].entries[e].tile));
        t.tiles.resize(static_cast<size_t>(maxTile) + 1);
        for (int e = 0; e < kTables[i].count; ++e) {
            const Entry& en = kTables[i].entries[e];
            Terrain::Info& info = t.tiles[en.tile];
            if (en.pattern < 0) continue; // three or more materials: unknown corners
            info.a = static_cast<int16_t>(t.MaterialId(Family(kMaterials[en.a])));
            info.pattern = en.pattern;
            if (en.b < 0) t.materials[static_cast<size_t>(info.a)].baseTiles.push_back(en.tile);
            else { info.b = static_cast<int16_t>(t.MaterialId(Family(kMaterials[en.b]))); t.blendTiles.push_back(en.tile); }
        }
        break;
    }
    return t.tiles.empty() ? nullptr : &t;
}

// Every terrain the table knows (for borrowing another allod's materials).
inline std::vector<std::string> KnownTerrains() {
    std::vector<std::string> names;
    for (int i = 0; i < kTableCount; ++i) names.push_back(kTables[i].terrain);
    return names;
}

inline int RotateCw(int pattern) { // NW <- SW, NE <- NW, SE <- NE, SW <- SE
    const int nw = pattern & 1, ne = (pattern >> 1) & 1, sw = (pattern >> 2) & 1, se = (pattern >> 3) & 1;
    return sw | (nw << 1) | (se << 2) | (ne << 3);
}

// The materials at the four corners of a placed (packed) tile, -1 where unknown.
inline void Corners(const Terrain& t, uint16_t packed, int out[4]) {
    const int tile = ((packed >> 6) & 0xFF) * 64 + (packed & 63), rotation = (packed >> 14) & 3;
    const Terrain::Info* info = t.At(tile);
    if (!info || info->pattern < 0) { out[0] = out[1] = out[2] = out[3] = -1; return; }
    int p = info->pattern;
    for (int r = 0; r < rotation; ++r) p = RotateCw(p);
    for (int k = 0; k < 4; ++k) out[k] = (p >> k) & 1 ? info->b : info->a;
}

inline uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// The tile (texture * 64 + tile) and rotation whose corners are `corners` (material ids, none -1): a base tile
// when all four are one material, else a transition tile. False when the textures have no such tile.
inline bool Solve(const Terrain& t, const int corners[4], uint32_t hash, int& tile, int& rotation) {
    if (corners[0] < 0 || corners[1] < 0 || corners[2] < 0 || corners[3] < 0) return false;
    const int a = corners[0];
    int b = -1;
    for (int k = 1; k < 4; ++k) if (corners[k] != a) { if (b < 0) b = corners[k]; else if (corners[k] != b) return false; }
    if (b < 0) {
        const std::vector<int>& bases = t.materials[static_cast<size_t>(a)].baseTiles;
        if (bases.empty()) return false;
        tile = bases[hash % bases.size()];
        rotation = static_cast<int>((hash >> 8) % 4);
        return true;
    }
    int wanted = 0; // the corners showing b
    for (int k = 0; k < 4; ++k) if (corners[k] == b) wanted |= 1 << k;
    std::vector<std::pair<int, int>> fits;
    for (int candidate : t.blendTiles) {
        const Terrain::Info& info = t.tiles[static_cast<size_t>(candidate)];
        int p;
        if (info.a == a && info.b == b) p = info.pattern;
        else if (info.a == b && info.b == a) p = ~info.pattern & 15;
        else continue;
        for (int r = 0; r < 4; ++r, p = RotateCw(p)) if (p == wanted) fits.push_back({candidate, r});
    }
    if (fits.empty()) return false;
    const auto& pick = fits[hash % fits.size()];
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

} // namespace tilemat
