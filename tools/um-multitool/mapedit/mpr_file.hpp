// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Reads a .mpr terrain (a RES archive) the way ei_maper does (landscape.cpp, sector.cpp, tile.cpp):
//
//   <map>.mp          header: signature 0xCE4AF672, max height, sectors in X and Y, texture count,
//                     texture size, tile count, tile size, material count (u16), animated tile count,
//                     then materials (type, RGBA, self-illumination, wave, warp, 3 reserved), one tile
//                     type (int) per tile, and animated tiles (first tile, phase count; u16 each)
//   <map>0XX0YY.sec   one per sector: signature 0xCF4BF774, a type byte (3 = has water), 33x33 land
//                     vertices (x/y offset: i8 each, height: u16, packed normal: u32), 33x33 water
//                     vertices when it has water, 16x16 land tiles (u16), and with water 16x16 water
//                     tiles and 16x16 water material indices (i16, -1 = no water on that tile)
//
// A sector is 32x32 world units, a tile 2x2, vertices 1 apart. A packed tile is: bits 0-5 the tile
// in its texture (8x8 tiles), bits 6-13 the texture (<map>000.mmp .. <map>007.mmp in textures.res),
// bits 14-15 the rotation in quarter turns. Names inside the archive are often upper-case.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <fstream>
#include <string>
#include <vector>

#include "../viewer/res_archive.hpp"

namespace mpr {

struct Material {
    int type = 0;                 // 1 = terrain, 3 = water (ei_maper's ETerrainType)
    float r = 0, g = 0, b = 0, a = 1;
    float selfIllumination = 0, waveMultiplier = 0, warpSpeed = 0;
    float reserved[3] = {0, 0, 0};
};

struct Vertex {
    int8_t xOffset = 0, yOffset = 0;
    uint16_t z = 0;
    uint32_t packedNormal = 0;
};

struct Sector {
    bool present = false;
    bool water = false;
    uint8_t type = 0;             // as stored: 3 has water, 0 none (no vanilla sector has any other value)
    Vertex land[33][33];          // [row = y][col = x]
    Vertex waterVerts[33][33];
    uint16_t landTiles[16][16] = {};
    uint16_t waterTiles[16][16] = {};
    int16_t waterMaterial[16][16] = {};
};

struct Map {
    std::string name;             // the base name inside the archive, e.g. "zone1"
    float maxZ = 0;
    int sectorsX = 0, sectorsY = 0;
    int textureCount = 0, textureSize = 0, tileCount = 0, tileSize = 0;
    std::vector<Material> materials;
    std::vector<int> tileTypes;   // per tile index (texture * 64 + tile)
    std::vector<std::pair<int, int>> animTiles;
    std::vector<Sector> sectors;  // sectorsX * sectorsY, row-major by y
    std::vector<std::string> warnings;
    std::vector<uint8_t> archiveBytes; // the file as read, rewritten on save with the changed entries
    std::string path;

    const Sector* At(int sx, int sy) const {
        if (sx < 0 || sy < 0 || sx >= sectorsX || sy >= sectorsY) return nullptr;
        const Sector& s = sectors[static_cast<size_t>(sy) * sectorsX + sx];
        return s.present ? &s : nullptr;
    }
    float Width() const { return sectorsX * 32.0f; }
    float Height() const { return sectorsY * 32.0f; }

    // The land height at a world position (bilinear over the vertex grid; 0 outside the map).
    float HeightAt(float x, float y) const {
        if (x < 0 || y < 0 || x > Width() || y > Height()) return 0.0f;
        int sx = std::min(static_cast<int>(x / 32.0f), sectorsX - 1), sy = std::min(static_cast<int>(y / 32.0f), sectorsY - 1);
        const Sector* s = At(sx, sy);
        if (!s) return 0.0f;
        float lx = x - sx * 32.0f, ly = y - sy * 32.0f;
        int c = std::min(static_cast<int>(lx), 31), r = std::min(static_cast<int>(ly), 31);
        float fx = lx - c, fy = ly - r;
        const float k = maxZ / 65535.0f;
        float h00 = s->land[r][c].z * k, h01 = s->land[r][c + 1].z * k, h10 = s->land[r + 1][c].z * k, h11 = s->land[r + 1][c + 1].z * k;
        return (h00 * (1 - fx) + h01 * fx) * (1 - fy) + (h10 * (1 - fx) + h11 * fx) * fy;
    }
};

inline void Normal(uint32_t packed, float out[3]) {
    out[0] = (((packed >> 11) & 0x7FF) - 1000.0f) / 1000.0f;
    out[1] = ((packed & 0x7FF) - 1000.0f) / 1000.0f;
    out[2] = (packed >> 22) / 1000.0f;
}

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T Get() {
        T v{};
        if (pos + sizeof(T) > b.size()) { ok = false; return v; }
        std::memcpy(&v, b.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
};

inline bool ReadSector(const std::vector<uint8_t>& bytes, Sector& s, std::string& err) {
    Reader r{bytes};
    if (r.Get<uint32_t>() != 0xCF4BF774u) { err = "bad sector signature"; return false; }
    uint8_t type = r.Get<uint8_t>();
    s.type = type;
    s.water = type == 3;
    auto readVerts = [&](Vertex (&v)[33][33]) {
        for (int row = 0; row < 33; ++row)
            for (int col = 0; col < 33; ++col) {
                v[row][col].xOffset = r.Get<int8_t>();
                v[row][col].yOffset = r.Get<int8_t>();
                v[row][col].z = r.Get<uint16_t>();
                v[row][col].packedNormal = r.Get<uint32_t>();
            }
    };
    readVerts(s.land);
    if (s.water) readVerts(s.waterVerts);
    for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) s.landTiles[row][col] = r.Get<uint16_t>();
    if (s.water) {
        for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) s.waterTiles[row][col] = r.Get<uint16_t>();
        for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) s.waterMaterial[row][col] = r.Get<int16_t>();
    }
    if (!r.ok) { err = "sector data is truncated"; return false; }
    s.present = true;
    return true;
}

inline bool Load(const std::string& path, Map& m, std::string& err) {
    m = Map{};
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    m.archiveBytes = bytes;
    m.path = path;
    res::Archive archive;
    if (!res::ParseArchive(bytes, archive, err)) { err = "not a RES archive: " + err; return false; }

    const std::vector<uint8_t>* mp = nullptr;
    for (const auto& e : archive.entries) { // keys are lower-case
        const std::string& lower = e.first;
        if (lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".mp") == 0) {
            m.name = lower.substr(0, lower.size() - 3);
            mp = &e.second.data;
            break;
        }
    }
    if (!mp) { err = "no .mp header in the archive"; return false; }
    Reader r{*mp};
    if (r.Get<uint32_t>() != 0xCE4AF672u) { err = "bad map header signature"; return false; }
    m.maxZ = r.Get<float>();
    m.sectorsX = static_cast<int>(r.Get<uint32_t>());
    m.sectorsY = static_cast<int>(r.Get<uint32_t>());
    m.textureCount = static_cast<int>(r.Get<uint32_t>());
    m.textureSize = static_cast<int>(r.Get<uint32_t>());
    m.tileCount = static_cast<int>(r.Get<uint32_t>());
    m.tileSize = static_cast<int>(r.Get<uint32_t>());
    int materials = r.Get<uint16_t>();
    int anims = static_cast<int>(r.Get<uint32_t>());
    if (!r.ok || m.sectorsX <= 0 || m.sectorsY <= 0 || m.sectorsX > 256 || m.sectorsY > 256 || m.tileCount < 0 || m.tileCount > 65536) {
        err = "implausible map header";
        return false;
    }
    for (int i = 0; i < materials; ++i) {
        Material mat;
        mat.type = r.Get<int32_t>();
        mat.r = r.Get<float>(); mat.g = r.Get<float>(); mat.b = r.Get<float>(); mat.a = r.Get<float>();
        mat.selfIllumination = r.Get<float>(); mat.waveMultiplier = r.Get<float>(); mat.warpSpeed = r.Get<float>();
        for (float& v : mat.reserved) v = r.Get<float>();
        m.materials.push_back(mat);
    }
    for (int i = 0; i < m.tileCount; ++i) m.tileTypes.push_back(r.Get<int32_t>());
    for (int i = 0; i < anims; ++i) {
        int first = r.Get<uint16_t>();
        m.animTiles.push_back({first, static_cast<int>(r.Get<uint16_t>())});
    }
    if (!r.ok) { err = "the map header is truncated"; return false; }

    m.sectors.resize(static_cast<size_t>(m.sectorsX) * m.sectorsY);
    int missing = 0;
    for (int y = 0; y < m.sectorsY; ++y)
        for (int x = 0; x < m.sectorsX; ++x) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "%03d%03d.sec", x, y);
            const std::vector<uint8_t>* sec = archive.Find(m.name + suffix);
            if (!sec) { ++missing; continue; }
            std::string secErr;
            if (!ReadSector(*sec, m.sectors[static_cast<size_t>(y) * m.sectorsX + x], secErr))
                m.warnings.push_back(m.name + suffix + ": " + secErr);
        }
    if (missing) m.warnings.push_back(std::to_string(missing) + " sector(s) missing from the archive");
    return true;
}

// ---- writing (the same layouts as read) ------------------------------------------------------------

struct Writer {
    std::vector<uint8_t> b;
    template <typename T> void Put(T v) {
        const size_t at = b.size();
        b.resize(at + sizeof(T));
        std::memcpy(b.data() + at, &v, sizeof(T));
    }
};

inline std::vector<uint8_t> WriteHeader(const Map& m) {
    Writer w;
    w.Put<uint32_t>(0xCE4AF672u);
    w.Put<float>(m.maxZ);
    w.Put<uint32_t>(static_cast<uint32_t>(m.sectorsX));
    w.Put<uint32_t>(static_cast<uint32_t>(m.sectorsY));
    w.Put<uint32_t>(static_cast<uint32_t>(m.textureCount));
    w.Put<uint32_t>(static_cast<uint32_t>(m.textureSize));
    w.Put<uint32_t>(static_cast<uint32_t>(m.tileTypes.size()));
    w.Put<uint32_t>(static_cast<uint32_t>(m.tileSize));
    w.Put<uint16_t>(static_cast<uint16_t>(m.materials.size()));
    w.Put<uint32_t>(static_cast<uint32_t>(m.animTiles.size()));
    for (const Material& mat : m.materials) {
        w.Put<int32_t>(mat.type);
        for (float v : {mat.r, mat.g, mat.b, mat.a, mat.selfIllumination, mat.waveMultiplier, mat.warpSpeed}) w.Put<float>(v);
        for (float v : mat.reserved) w.Put<float>(v);
    }
    for (int t : m.tileTypes) w.Put<int32_t>(t);
    for (const auto& a : m.animTiles) { w.Put<uint16_t>(static_cast<uint16_t>(a.first)); w.Put<uint16_t>(static_cast<uint16_t>(a.second)); }
    return w.b;
}

inline std::vector<uint8_t> WriteSector(const Sector& s) {
    Writer w;
    w.Put<uint32_t>(0xCF4BF774u);
    w.Put<uint8_t>(s.water ? 3 : 0);
    auto verts = [&](const Vertex (&v)[33][33]) {
        for (int row = 0; row < 33; ++row)
            for (int col = 0; col < 33; ++col) {
                w.Put<int8_t>(v[row][col].xOffset);
                w.Put<int8_t>(v[row][col].yOffset);
                w.Put<uint16_t>(v[row][col].z);
                w.Put<uint32_t>(v[row][col].packedNormal);
            }
    };
    verts(s.land);
    if (s.water) verts(s.waterVerts);
    for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) w.Put<uint16_t>(s.landTiles[row][col]);
    if (s.water) {
        for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) w.Put<uint16_t>(s.waterTiles[row][col]);
        for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) w.Put<int16_t>(s.waterMaterial[row][col]);
    }
    return w.b;
}

inline std::string SectorEntryName(const Map& m, int x, int y) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "%03d%03d.sec", x, y);
    return m.name + suffix;
}

// Writes the terrain to `path`: the archive as it was read, with the header and the given sectors
// (index y * sectorsX + x) written again; everything else keeps its bytes. Written to a temporary file,
// then moved over the target.
inline bool Save(Map& m, const std::string& path, const std::vector<int>& sectors, bool header, std::string& err) {
    std::map<std::string, std::vector<uint8_t>> replace;
    if (header) replace[m.name + ".mp"] = WriteHeader(m);
    for (int i : sectors) {
        if (i < 0 || i >= static_cast<int>(m.sectors.size()) || !m.sectors[static_cast<size_t>(i)].present) continue;
        replace[SectorEntryName(m, i % m.sectorsX, i / m.sectorsX)] = WriteSector(m.sectors[static_cast<size_t>(i)]);
    }
    std::vector<uint8_t> out;
    if (!res::RewriteArchive(m.archiveBytes, replace, static_cast<uint32_t>(std::time(nullptr)), out, err)) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) { err = "cannot write " + tmp; return false; }
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f) { err = "cannot write " + tmp; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot replace " + path; return false; }
    }
    m.archiveBytes = std::move(out);
    m.path = path;
    return true;
}

// A new flat terrain of sx x sy sectors (32 x 32 units each) at `height`, every tile `tile` (packed as stored),
// with the materials, tile types and texture settings of `model` (an open terrain). `name` is the name inside the
// archive, which the textures go by (<name>000.mmp...): keep the model's to use its textures. Written to `path`.
inline bool Create(const Map& model, const std::string& name, int sx, int sy, float height, uint16_t tile,
                   const std::string& path, Map& out, std::string& err) {
    if (sx < 1 || sy < 1 || sx > 64 || sy > 64) { err = "1 to 64 sectors each way"; return false; }
    Map m;
    m.name = name;
    m.maxZ = std::max({model.maxZ, height * 2.0f, 1.0f});
    m.sectorsX = sx; m.sectorsY = sy;
    m.textureCount = model.textureCount; m.textureSize = model.textureSize;
    m.tileCount = model.tileCount; m.tileSize = model.tileSize;
    m.materials = model.materials; m.tileTypes = model.tileTypes; m.animTiles = model.animTiles;
    m.sectors.resize(static_cast<size_t>(sx) * sy);
    const uint16_t z = static_cast<uint16_t>(std::lround(std::clamp(height / m.maxZ, 0.0f, 1.0f) * 65535.0f));
    const uint32_t up = (1000u << 22) | (1000u << 11) | 1000u; // a normal straight up (see Normal)
    std::map<std::string, std::vector<uint8_t>> files;
    for (int y = 0; y < sy; ++y)
        for (int x = 0; x < sx; ++x) {
            Sector& s = m.sectors[static_cast<size_t>(y) * sx + x];
            s.present = true; s.water = false; s.type = 0;
            for (auto& row : s.land) for (Vertex& v : row) { v.z = z; v.packedNormal = up; }
            for (auto& row : s.landTiles) for (uint16_t& t : row) t = tile;
            for (auto& row : s.waterMaterial) for (int16_t& w : row) w = -1;
            files[SectorEntryName(m, x, y)] = WriteSector(s);
        }
    files[m.name + ".mp"] = WriteHeader(m);
    m.archiveBytes = res::WriteArchive(files, static_cast<uint32_t>(std::time(nullptr)));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { err = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(m.archiveBytes.data()), static_cast<std::streamsize>(m.archiveBytes.size()));
    if (!f) { err = "cannot write " + path; return false; }
    m.path = path;
    out = std::move(m);
    return true;
}

// The normals of a sector's land vertices from the heights around them (across sector edges).
inline void ComputeNormals(Map& m, int si) {
    if (si < 0 || si >= static_cast<int>(m.sectors.size())) return;
    Sector& s = m.sectors[static_cast<size_t>(si)];
    const int sx = si % m.sectorsX, sy = si / m.sectorsX;
    for (int r = 0; r <= 32; ++r)
        for (int c = 0; c <= 32; ++c) {
            const float vx = sx * 32.0f + c, vy = sy * 32.0f + r;
            const float dx = (m.HeightAt(vx + 1, vy) - m.HeightAt(vx - 1, vy)) / 2, dy = (m.HeightAt(vx, vy + 1) - m.HeightAt(vx, vy - 1)) / 2;
            const float len = std::sqrt(dx * dx + dy * dy + 1);
            const float nx = -dx / len, ny = -dy / len, nz = 1 / len;
            s.land[r][c].packedNormal = (static_cast<uint32_t>(std::lround(nz * 1000)) << 22) |
                                        (static_cast<uint32_t>(std::lround(nx * 1000 + 1000)) & 0x7FF) << 11 |
                                        (static_cast<uint32_t>(std::lround(ny * 1000 + 1000)) & 0x7FF);
        }
}

// A smaller copy of a terrain: the sectors of the rectangle (x0, y0, w, h) only, written as a new .mpr at `path`
// with the same name inside (it keeps using the same textures). The caller shifts the objects by (-x0 * 32, -y0 * 32).
inline bool Crop(const Map& src, int x0, int y0, int w, int h, const std::string& path, Map& out, std::string& err) {
    if (x0 < 0 || y0 < 0 || w < 1 || h < 1 || x0 + w > src.sectorsX || y0 + h > src.sectorsY) { err = "the rectangle must lie inside the terrain"; return false; }
    Map m;
    m.name = src.name;
    m.maxZ = src.maxZ;
    m.sectorsX = w; m.sectorsY = h;
    m.textureCount = src.textureCount; m.textureSize = src.textureSize;
    m.tileCount = src.tileCount; m.tileSize = src.tileSize;
    m.materials = src.materials; m.tileTypes = src.tileTypes; m.animTiles = src.animTiles;
    m.sectors.resize(static_cast<size_t>(w) * h);
    std::map<std::string, std::vector<uint8_t>> files;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Sector& s = m.sectors[static_cast<size_t>(y) * w + x];
            s = src.sectors[static_cast<size_t>(y0 + y) * src.sectorsX + (x0 + x)];
            if (s.present) files[SectorEntryName(m, x, y)] = WriteSector(s);
        }
    files[m.name + ".mp"] = WriteHeader(m);
    m.archiveBytes = res::WriteArchive(files, static_cast<uint32_t>(std::time(nullptr)));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open() || !f.write(reinterpret_cast<const char*>(m.archiveBytes.data()), static_cast<std::streamsize>(m.archiveBytes.size()))) { err = "cannot write " + path; return false; }
    m.path = path;
    out = std::move(m);
    return true;
}

} // namespace mpr
