// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Blended terrain tiles: a new tile painted from two others through a soft mask (an edge, a corner), written into
// a free tile of the terrain's own textures (<map>000.mmp .. 007: DXT1, 8 x 8 tiles, mipmaps). The game cannot
// blend tiles itself; this is how the community tools give a natural look between two grounds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../viewer/mmp_texture.hpp"

namespace blend {

// Encodes a 4x4 RGBA patch as a DXT1 block (opaque): the two endpoints are the extremes along the colours'
// main direction (approximated by the longest of the r, g, b ranges), each pixel takes the nearest of the 4.
inline void EncodeDxt1Block(const uint8_t px[16][4], uint8_t out[8]) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) { lo[c] = std::min<int>(lo[c], px[i][c]); hi[c] = std::max<int>(hi[c], px[i][c]); }
    int axis = 0;
    for (int c = 1; c < 3; ++c) if (hi[c] - lo[c] > hi[axis] - lo[axis]) axis = c;
    int a = 0, b = 0;
    for (int i = 1; i < 16; ++i) { if (px[i][axis] < px[a][axis]) a = i; if (px[i][axis] > px[b][axis]) b = i; }
    auto to565 = [](const uint8_t* p) {
        return static_cast<uint16_t>(((p[0] * 31 + 127) / 255) << 11 | ((p[1] * 63 + 127) / 255) << 5 | ((p[2] * 31 + 127) / 255));
    };
    uint16_t c0 = to565(px[b]), c1 = to565(px[a]);
    if (c0 < c1) std::swap(c0, c1);
    uint8_t pal[4][3];
    auto unpack = [](uint16_t c, uint8_t* o) { o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31); o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63); o[2] = static_cast<uint8_t>((c & 31) * 255 / 31); };
    unpack(c0, pal[0]); unpack(c1, pal[1]);
    for (int c = 0; c < 3; ++c) {
        if (c0 > c1) { pal[2][c] = static_cast<uint8_t>((2 * pal[0][c] + pal[1][c]) / 3); pal[3][c] = static_cast<uint8_t>((pal[0][c] + 2 * pal[1][c]) / 3); }
        else { pal[2][c] = static_cast<uint8_t>((pal[0][c] + pal[1][c]) / 2); pal[3][c] = pal[2][c]; } // c0 == c1: one colour
    }
    uint32_t codes = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestD = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
            if (d < bestD) { bestD = d; best = k; }
        }
        codes |= static_cast<uint32_t>(best) << (2 * i);
    }
    out[0] = static_cast<uint8_t>(c0); out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1); out[3] = static_cast<uint8_t>(c1 >> 8);
    std::memcpy(out + 4, &codes, 4);
}

struct Texture { // a terrain texture: a DXT1 .mmp
    int width = 0, height = 0, mips = 0;
    bool ok = false;
    size_t LevelOffset(int level) const {
        size_t at = 76;
        for (int l = 0; l < level; ++l) at += static_cast<size_t>(std::max(1, (width >> l) / 4)) * std::max(1, (height >> l) / 4) * 8;
        return at;
    }
};

inline Texture Inspect(const std::vector<uint8_t>& mmp) {
    Texture t;
    if (mmp.size() < 76 || std::memcmp(mmp.data(), "MMP", 3) != 0 || std::memcmp(mmp.data() + 16, "DXT1", 4) != 0) return t;
    uint32_t v[3];
    std::memcpy(v, mmp.data() + 4, 12);
    t.width = static_cast<int>(v[0]); t.height = static_cast<int>(v[1]); t.mips = static_cast<int>(v[2]);
    t.ok = t.width >= 32 && t.height >= 32 && t.mips >= 1 && mmp.size() >= t.LevelOffset(t.mips);
    return t;
}

// The tile (0..63, 8 x 8 per texture) at full size, as RGBA (size x size, size = width / 8). The game counts
// the tile rows from the BOTTOM of the picture (tile 0 is the bottom-left one; the texture's first block row is
// the top), as the terrain's UVs show (map_app TileUvs).
inline std::vector<uint8_t> ReadTile(const std::vector<uint8_t>& mmp, const Texture& t, int tile) {
    const int size = t.width / 8, x0 = (tile % 8) * size, y0 = (7 - tile / 8) * size, bw = t.width / 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
    for (int by = 0; by < size / 4; ++by)
        for (int bx = 0; bx < size / 4; ++bx) {
            uint8_t patch[4][4][4];
            mmp::DecodeDxt1Block(mmp.data() + t.LevelOffset(0) + (static_cast<size_t>((y0 / 4 + by)) * bw + (x0 / 4 + bx)) * 8, patch);
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
                std::memcpy(&rgba[(static_cast<size_t>(by * 4 + y) * size + bx * 4 + x) * 4], patch[y][x], 4);
        }
    return rgba;
}

// Writes `rgba` (size x size) as the tile, at every mipmap level (box-filtered down), re-encoding only that
// tile's blocks: the other tiles keep their bytes.
inline void WriteTile(std::vector<uint8_t>& mmp, const Texture& t, int tile, std::vector<uint8_t> rgba) {
    int size = t.width / 8;
    for (int level = 0; level < t.mips && size >= 1; ++level) {
        const int lw = std::max(1, t.width >> level), bw = std::max(1, lw / 4);
        const int x0 = (tile % 8) * size, y0 = (7 - tile / 8) * size; // rows from the bottom, as ReadTile
        for (int by = 0; by < std::max(1, size / 4); ++by)
            for (int bx = 0; bx < std::max(1, size / 4); ++bx) {
                uint8_t px[16][4];
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    const int sx = std::min(bx * 4 + x, size - 1), sy = std::min(by * 4 + y, size - 1);
                    std::memcpy(px[y * 4 + x], &rgba[(static_cast<size_t>(sy) * size + sx) * 4], 4);
                }
                uint8_t block[8];
                EncodeDxt1Block(px, block);
                const size_t at = t.LevelOffset(level) + (static_cast<size_t>(y0 / 4 + by) * bw + (x0 / 4 + bx)) * 8;
                if (at + 8 <= mmp.size()) std::memcpy(mmp.data() + at, block, 8);
            }
        if (size <= 4) break; // a block is the smallest unit: deeper levels would mix the neighbours
        const int half = size / 2;
        std::vector<uint8_t> next(static_cast<size_t>(half) * half * 4);
        for (int y = 0; y < half; ++y) for (int x = 0; x < half; ++x) for (int c = 0; c < 4; ++c) {
            int sum = 0;
            for (int k = 0; k < 4; ++k) sum += rgba[(static_cast<size_t>(2 * y + k / 2) * size + 2 * x + k % 2) * 4 + c];
            next[(static_cast<size_t>(y) * half + x) * 4 + c] = static_cast<uint8_t>(sum / 4);
        }
        rgba.swap(next);
        size = half;
    }
}

enum class Mask { Edge, Corner, InnerCorner };

// How much of tile B shows at (u, v) in [0, 1]: Edge = B on the right; Corner = B in the bottom-right corner;
// InnerCorner = B everywhere but the top-left corner. `softness` widens the transition; a little value noise
// breaks the straight line.
inline float Weight(Mask mask, float u, float v, float softness, uint32_t seed) {
    auto noise = [seed](float x, float y) {
        auto h = [seed](int i, int j) { uint32_t n = static_cast<uint32_t>(i) * 374761393u + static_cast<uint32_t>(j) * 668265263u + seed * 2246822519u; n = (n ^ (n >> 13)) * 1274126177u; return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0f; };
        const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y));
        const float fx = x - i, fy = y - j, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        return (h(i, j) * (1 - sx) + h(i + 1, j) * sx) * (1 - sy) + (h(i, j + 1) * (1 - sx) + h(i + 1, j + 1) * sx) * sy;
    };
    float d = 0;
    switch (mask) {
    case Mask::Edge: d = u - 0.5f; break;
    case Mask::Corner: d = std::min(u, v) - 0.5f; break;
    case Mask::InnerCorner: d = std::max(u, v) - 0.5f; break;
    }
    d += (noise(u * 6, v * 6) - 0.5f) * 0.25f + (noise(u * 17 + 5, v * 17 + 3) - 0.5f) * 0.08f;
    const float w = std::max(softness, 0.01f);
    const float k = std::clamp(d / w + 0.5f, 0.0f, 1.0f);
    return k * k * (3 - 2 * k);
}

// The blended tile: A where the weight is 0, B where it is 1.
inline std::vector<uint8_t> Mix(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int size, Mask mask, float softness, uint32_t seed) {
    std::vector<uint8_t> out(a.size());
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float w = Weight(mask, (x + 0.5f) / size, (y + 0.5f) / size, softness, seed);
            for (int c = 0; c < 4; ++c) {
                const size_t i = (static_cast<size_t>(y) * size + x) * 4 + c;
                out[i] = static_cast<uint8_t>(std::lround(a[i] * (1 - w) + b[i] * w));
            }
        }
    return out;
}

} // namespace blend
