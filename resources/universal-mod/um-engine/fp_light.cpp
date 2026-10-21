// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// 3DfpFPU.dll's per-vertex lighting (RE: _cpr/claude-re/dll/, RVA 0xD910, thiscall, 8 stack arguments, `ret 0x20`).
// The game's 3D pipeline library lights every vertex of a figure each frame: one directional light, then up to N point lights,
// the strongest of them per colour channel, the ambient light added, the result packed to ARGB. The library's code does it
// in x87 with every intermediate stored to memory; in the multiplayer lobby it was about 40 % of that library's time
// (~1 ms of a 10 ms frame). Same arithmetic, in the same order, in SSE (single-precision, round to nearest: what the x87
// computes under the 24-bit precision the game's DirectDraw setup selects, and the same 8-bit result in all but the ties).
//
// void __thiscall Light(this, const char* positions, const char* normals, uint32_t* out, const uint16_t* indices, int count,
//                       Light** lights, int lightCount, const float* ambient)
//   this + 4  -> the directional light block: +0x108 / 0x10C / 0x110 its direction, +0x114 / 0x118 / 0x11C its colour, +0x120 its alpha
//   this + 0x1C / 0x20 / 0x24: the lowest red / green / blue (the ambient floor)
//   positions, normals: blocks of 4 vertices (SoA): 0x40 bytes per block, x at +0, y at +0x10, z at +0x20, 4 floats each
//   indices: per vertex two u16: its position's index, its normal's index (block * 4 + lane)
//   a point light: +0x28 / 0x2C / 0x30 position, +0x104 its attenuation factor, +0x114 / 0x118 / 0x11C colour
#pragma GCC push_options
#pragma GCC target("sse2,fpmath=sse")
#include <emmintrin.h>

#include <cstdint>
#include <cstring>

namespace {
struct Floats { float x, y, z; };
inline float Max(float a, float b) { return a < b ? b : a; }   // (a < b) ? b : a, as the library's compare-and-mask does (a NaN b keeps a)
inline float Min1(float v) { return v < 1.0f ? v : 1.0f; }
inline int Round(float v) { return _mm_cvtss_si32(_mm_set_ss(v)); } // round to nearest even, like fistp
}

extern "C" void __attribute__((thiscall))
UmFpLight(void* self, const char* positions, const char* normals, uint32_t* out, const uint16_t* indices, int count,
          void* const* lights, int lightCount, const float* ambient) {
    const char* base = static_cast<const char*>(self);
    const char* dirLight = *reinterpret_cast<const char* const*>(base + 4);
    float floorR, floorG, floorB;
    std::memcpy(&floorR, base + 0x1C, 4); std::memcpy(&floorG, base + 0x20, 4); std::memcpy(&floorB, base + 0x24, 4);
    float dirX, dirY, dirZ, colR, colG, colB, alphaF;
    std::memcpy(&dirX, dirLight + 0x108, 4); std::memcpy(&dirY, dirLight + 0x10C, 4); std::memcpy(&dirZ, dirLight + 0x110, 4);
    std::memcpy(&colR, dirLight + 0x114, 4); std::memcpy(&colG, dirLight + 0x118, 4); std::memcpy(&colB, dirLight + 0x11C, 4);
    std::memcpy(&alphaF, dirLight + 0x120, 4);
    const uint32_t alpha = static_cast<uint32_t>(Round(alphaF * 255.0f)) << 24;
    const float ambR = ambient[0], ambG = ambient[1], ambB = ambient[2];

    for (int v = 0; v < count; ++v) {
        const uint32_t pi = indices[2 * v], ni = indices[2 * v + 1];
        const float* nb = reinterpret_cast<const float*>(normals + (ni >> 2) * 0x40);
        const uint32_t nl = ni & 3;
        const float nx = nb[nl], ny = nb[4 + nl], nz = nb[8 + nl];
        const float dot = dirY * ny + dirX * nx + dirZ * nz;          // the library adds y, x, z in this order
        float r = Max(floorR, dot * colR), g = Max(floorG, dot * colG), b = Max(floorB, dot * colB);
        const float* pb = reinterpret_cast<const float*>(positions + (pi >> 2) * 0x40);
        const uint32_t pl = pi & 3;
        const float px = pb[pl], py = pb[4 + pl], pz = pb[8 + pl];
        for (int i = 0; i < lightCount; ++i) {
            const char* light = static_cast<const char*>(lights[i]);
            float lx, ly, lz, atten, lr, lg, lb;
            std::memcpy(&lx, light + 0x28, 4); std::memcpy(&ly, light + 0x2C, 4); std::memcpy(&lz, light + 0x30, 4);
            std::memcpy(&atten, light + 0x104, 4);
            const float dx = lx - px, dy = ly - py, dz = lz - pz;
            const float d2 = dz * dz + dy * dy + dx * dx;
            const float fall = 1.0f - atten * d2;
            if (0.0f <= fall) {
                const float inv = 1.0f / _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(d2)));
                const float t = (dy * inv * ny + dx * inv * nx + inv * dz * nz) * fall;
                std::memcpy(&lr, light + 0x114, 4); std::memcpy(&lg, light + 0x118, 4); std::memcpy(&lb, light + 0x11C, 4);
                r = Max(r, lr * t); g = Max(g, lg * t); b = Max(b, lb * t);
            }
        }
        out[v] = alpha | static_cast<uint32_t>(Round(Min1(r + ambR) * 255.0f)) << 16
                       | static_cast<uint32_t>(Round(Min1(g + ambG) * 255.0f)) << 8
                       | static_cast<uint32_t>(Round(Min1(b + ambB) * 255.0f));
    }
}

#pragma GCC pop_options
