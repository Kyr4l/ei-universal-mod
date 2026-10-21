// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Unit / object shadows that respect the terrain (RE: docs/shadows.md). The game lays a unit's shadow over every terrain triangle inside a prism extruded
// from the unit along the light (CWorldClient::RenderShadowVolume 0x56C750, called from CObject3DClientSpecific::Render at 0x51E08B), so a bush on a plateau
// throws its shadow onto the ground below a cliff that the light never reaches. Here that call goes through UmShadowVolumeCall, which lets the game build the
// receiver mesh and then drops the triangles the sun cannot reach (terrain between the receiver and the caster, see TerrainBlocksSun).
//
// Foliage shadows with the leaves' shape (UmShadowAlphaMap). The game can rasterise a caster's silhouette through its texture (2DintMMX.dll's textured
// triangle fill: a 256 x 256 map of 16-bit pixels, bit 15 = opaque, taken from texture +0x60 when the texture's flag 0x2000 is set), but only the
// original 16-bit textures keep such a copy: HD Lands' DXT textures never do, so a bush casts the shadow of its whole mesh (blocks). The point in
// Render that picks the map (0x51DA50) is redirected to UmShadowAlphaMap, which builds the map from the texture's own surface (DXT1 / DXT3 / DXT5 alpha).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);
extern "C" void UmShadowVolumeCall();
extern "C" { void* UmShadowRet = nullptr; }

extern "C" void UmShadowBegin(const float* dir, const float* pos);
extern "C" void UmShadowEnd();
asm(R"(
.text
.globl _UmShadowVolumeCall
_UmShadowVolumeCall:
    popl %eax
    movl %eax, _UmShadowRet
    pushl %ecx
    pushl 8(%esp)
    pushl 8(%esp)
    call _UmShadowBegin
    addl $8, %esp
    popl %ecx
    pushl $_UmShadowVolumeBack
    movl $0x0056C750, %eax
    jmp *%eax
_UmShadowVolumeBack:
    pushl %ebx
    call _UmShadowFilter
    addl $4, %esp
    call _UmShadowEnd
    jmp *_UmShadowRet
)");

// `unit` = the CObject3DClientSpecific whose receiver mesh was just built.
typedef unsigned (__attribute__((thiscall)) *LockFn)(void*, void*);
typedef void (__attribute__((thiscall)) *UnlockFn)(void*);
extern "C" const uint16_t* UmShadowAlphaMap(unsigned char* unit);
extern "C" void UmShadowFilter(unsigned char* unit) {
    // research (UM_SHADOW_DUMP=<dir>): the unit's own shadow texture (64 x 64, 16 bit) and the alpha map of its texture, for the first casters of kind 0x54 (foliage)
    static const char* layoutLog = std::getenv("UM_SHADOW_LAYOUT");
    static int layouts = 0;
    if (layoutLog && layouts < 6 && *reinterpret_cast<const unsigned*>(unit + 0x50) > 20) { // research: the receiver mesh arrays of a unit
        ++layouts;
        const unsigned* d = reinterpret_cast<const unsigned*>(unit + 0x48);
        UmEngineLog("receiver of %p: +48.. = %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", static_cast<void*>(unit), d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13]);
        if (const unsigned char* ow = *reinterpret_cast<unsigned char* const*>(unit + 0x1C)) UmEngineLog("  caster position (+1C,+20,+24 of its owner) %g %g %g", static_cast<double>(*reinterpret_cast<const float*>(ow + 0x1C)), static_cast<double>(*reinterpret_cast<const float*>(ow + 0x20)), static_cast<double>(*reinterpret_cast<const float*>(ow + 0x24)));
        if (const float* v = *reinterpret_cast<const float* const*>(unit + 0x4C)) { const unsigned n = *reinterpret_cast<const unsigned*>(unit + 0x50); float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f}; for (unsigned i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) { const float c = v[(i / 4) * 12 + k * 4 + i % 4]; mn[k] = std::min(mn[k], c); mx[k] = std::max(mx[k], c); } UmEngineLog("  receiver bounds (SoA x,y,z): %g..%g  %g..%g  %g..%g over %u vertices", static_cast<double>(mn[0]), static_cast<double>(mx[0]), static_cast<double>(mn[1]), static_cast<double>(mx[1]), static_cast<double>(mn[2]), static_cast<double>(mx[2]), n); }
        for (int off : {0x4C, 0x5C, 0x6C}) {
            const unsigned* arr = *reinterpret_cast<const unsigned* const*>(unit + off);
            if (!arr || reinterpret_cast<uintptr_t>(arr) < 0x10000) continue;
            UmEngineLog("  array at +%X -> %p: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X", off, static_cast<const void*>(arr), arr[0], arr[1], arr[2], arr[3], arr[4], arr[5], arr[6], arr[7], arr[8], arr[9], arr[10], arr[11]);
        }
    }
    static const char* dir = std::getenv("UM_SHADOW_DUMP");
    static int dumped = 0;
    if (!dir || dumped >= 14) return;
    const unsigned char* owner = *reinterpret_cast<unsigned char* const*>(unit + 0x1C);
    unsigned kind = 0;
    if (owner) { typedef unsigned (__attribute__((thiscall)) *KindFn)(const void*); kind = (*reinterpret_cast<KindFn* const*>(owner))[0x30 / 4](owner); }
    if (kind != 0x54) return;
    unsigned char* own = *reinterpret_cast<unsigned char**>(unit + 0x7C);
    if (!own) return;
    alignas(8) unsigned char desc[128];
    if (!reinterpret_cast<LockFn>(0x00690FC0)(own, desc)) return;
    const unsigned* d = reinterpret_cast<const unsigned*>(desc);
    const unsigned w = d[3], h = d[2], pitch = d[4];
    const uint8_t* data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(d[0x24 / 4]));
    char path[512]; std::snprintf(path, sizeof path, "%s/shadow-%02d-%ux%u.raw", dir, dumped, w, h);
    if (data) if (FILE* f = std::fopen(path, "wb")) { for (unsigned y = 0; y < h; ++y) std::fwrite(data + static_cast<size_t>(y) * pitch, 1, w * 2, f); std::fclose(f); }
    reinterpret_cast<UnlockFn>(0x00691030)(own);
    const uint16_t* map = UmShadowAlphaMap(unit);
    std::snprintf(path, sizeof path, "%s/shadow-%02d-map.raw", dir, dumped);
    if (FILE* f = std::fopen(path, "wb")) { if (map) std::fwrite(map, 2, 256 * 256, f); std::fclose(f); }
    ++dumped;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The alpha map
namespace {
struct AlphaMap { const void* texture = nullptr; unsigned width = 0, height = 0; bool transparent = false; unsigned solid = 0; unsigned fourcc = 0; uint16_t pixels[256 * 256]; };
AlphaMap* g_maps[96]; int g_mapCount = 0;

// The alpha (0..255) of the block's texel (x, y of 0..3).
inline int Dxt1Alpha(const uint8_t* b, int x, int y) {
    const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    if (c0 > c1) return 255;
    const unsigned idx = (b[4 + y] >> (x * 2)) & 3;
    return idx == 3 ? 0 : 255;
}
inline int Dxt3Alpha(const uint8_t* b, int x, int y) { const unsigned v = (b[y * 2 + x / 2] >> ((x & 1) * 4)) & 15; return static_cast<int>(v * 17); }
inline int Dxt5Alpha(const uint8_t* b, int x, int y) {
    const int a0 = b[0], a1 = b[1];
    uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    const unsigned idx = static_cast<unsigned>(bits >> (3 * (y * 4 + x))) & 7;
    if (idx == 0) return a0;
    if (idx == 1) return a1;
    if (a0 > a1) return ((8 - idx) * a0 + (idx - 1) * a1) / 7;
    if (idx == 6) return 0;
    if (idx == 7) return 255;
    return ((6 - idx) * a0 + (idx - 1) * a1) / 5;
}

// Builds the map of a texture from its surface; null when the texture has no transparency (or is not a format read here).
AlphaMap* BuildMap(void* tex) {
    alignas(8) unsigned char desc[128];
    if (!reinterpret_cast<LockFn>(0x00690FC0)(tex, desc)) return nullptr; // the game's own lock (it zeroes the descriptor and sets its size)
    const unsigned* d = reinterpret_cast<const unsigned*>(desc);
    const unsigned height = d[2], width = d[3], flags = d[0x4C / 4], fourcc = d[0x50 / 4];
    const uint8_t* data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(d[0x24 / 4]));
    AlphaMap* map = nullptr;
    const bool dxt = (flags & 4) && (fourcc == 0x31545844u || fourcc == 0x33545844u || fourcc == 0x35545844u);
    if (dxt && data && width >= 4 && height >= 4 && width <= 4096 && height <= 4096) {
        const unsigned blockBytes = fourcc == 0x31545844u ? 8 : 16, blocksX = width / 4;
        map = new AlphaMap();
        map->texture = tex; map->width = width; map->height = height;
        unsigned opaque = 0;
        for (unsigned my = 0; my < 256; ++my)
            for (unsigned mx = 0; mx < 256; ++mx) {
                // the source texels this map texel covers (at least one), their mean alpha decides
                const unsigned x0 = mx * width / 256, x1 = std::max(x0 + 1, (mx + 1) * width / 256), y0 = my * height / 256, y1 = std::max(y0 + 1, (my + 1) * height / 256);
                unsigned sum = 0, n = 0;
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = x0; x < x1; ++x) {
                        const uint8_t* block = data + (static_cast<size_t>(y / 4) * blocksX + x / 4) * blockBytes;
                        const int a = fourcc == 0x31545844u ? Dxt1Alpha(block, x & 3, y & 3) : fourcc == 0x33545844u ? Dxt3Alpha(block, x & 3, y & 3) : Dxt5Alpha(block + 0, x & 3, y & 3);
                        sum += static_cast<unsigned>(a); ++n;
                    }
                const bool solid = sum * 2 >= n * 255; // more than half covered
                map->pixels[my * 256 + mx] = solid ? 0x8000 : 0;
                opaque += solid ? 1 : 0;
            }
        map->solid = opaque; map->fourcc = fourcc;
        map->transparent = opaque < 256u * 256u - 8 && opaque >= 256u * 256u / 10; // real holes, but not a texture whose alpha channel is unused (all zero: seen on a DXT3 unit texture)
    }
    reinterpret_cast<UnlockFn>(0x00691030)(tex);
    return map;
}
}

// The map of the caster `unit`'s first texture (the one the game's rasteriser reads): the game's own buffer when it has one, else one built from the surface, else 0 (the flat silhouette).
extern "C" const uint16_t* UmShadowAlphaMap(unsigned char* unit) {
    const unsigned char* holder = *reinterpret_cast<unsigned char* const*>(unit + 0x20);
    unsigned char* tex = holder ? *reinterpret_cast<unsigned char* const*>(const_cast<unsigned char*>(holder)) : nullptr;
    if (!tex) return nullptr;
    if (*reinterpret_cast<const unsigned*>(tex + 8) & 0x2000) return *reinterpret_cast<const uint16_t* const*>(tex + 0x60); // the game's own (original textures)
    for (int i = 0; i < g_mapCount; ++i)
        if (g_maps[i]->texture == tex && g_maps[i]->width == *reinterpret_cast<const unsigned*>(tex + 0x20) && g_maps[i]->height == *reinterpret_cast<const unsigned*>(tex + 0x24)) return g_maps[i]->transparent ? g_maps[i]->pixels : nullptr;
    AlphaMap* m = BuildMap(tex);
    if (g_mapCount < 96) {
        if (!m) { m = new AlphaMap(); m->texture = tex; m->width = *reinterpret_cast<const unsigned*>(tex + 0x20); m->height = *reinterpret_cast<const unsigned*>(tex + 0x24); m->transparent = false; }
        else { m->width = *reinterpret_cast<const unsigned*>(tex + 0x20); m->height = *reinterpret_cast<const unsigned*>(tex + 0x24); }
        g_maps[g_mapCount++] = m;
        const unsigned char* owner = *reinterpret_cast<unsigned char* const*>(unit + 0x1C);
        unsigned kind = 0;
        if (owner) { typedef unsigned (__attribute__((thiscall)) *KindFn)(const void*); kind = (*reinterpret_cast<KindFn* const*>(owner))[0x30 / 4](owner); }
        UmEngineLog("shadow alpha map for texture %p (%ux%u) of kind 0x%X: %s (fourcc %08X, solid texels %u of 65536)", static_cast<void*>(tex), m->width, m->height, kind, m->transparent ? "built" : "none (opaque or not a DXT format)", m->fourcc, m->solid);
        return m->transparent ? m->pixels : nullptr;
    }
    delete m;
    return nullptr;
}

extern "C" void UmShadowTexStub();
asm(R"(
.text
.globl _UmShadowTexStub
_UmShadowTexStub:
    pushl %ebx
    call _UmShadowAlphaMap
    addl $4, %esp
    movl %eax, -0x110(%ebp)
    movl $0x0051DA76, %eax
    jmp *%eax
)");

// Installs the redirect of Render's texture pick (0x51DA50). All or nothing: the bytes must be the game's own.
extern "C" bool UmShadowAlphaInstall() {
    static const unsigned char expect[12] = {0x8B, 0x43, 0x20, 0x3B, 0xC6, 0x74, 0x19, 0x8B, 0x00, 0x3B, 0xC6, 0x74};
    unsigned char* at = reinterpret_cast<unsigned char*>(0x0051DA50);
    if (memcmp(at, expect, 12) != 0) { UmEngineLog("um-engine: ShadowAlpha::Textured skipped, game.exe has other code in Render (another build)"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const long rel = reinterpret_cast<long>(&UmShadowTexStub) - reinterpret_cast<long>(at + 5);
    at[0] = 0xE9; memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    UmEngineLog("um-engine: ShadowAlpha::Textured installed");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The terrain occlusion: ClipAndAddTriangle (0x6CCF20) feeds every terrain triangle of the prism to Clip_TriangleToFrustumA (0x4D8490, thiscall, 3 vertex
// pointers); the two calls are redirected here and a triangle whose sun-ward ray runs into terrain before it reaches the caster is not given to it.
namespace {
struct Caster { bool active = false; float pos[3] = {0, 0, 0}; float sun[3] = {0, 0, 1}; } g_caster; // sun = the unit vector towards the sun
typedef const float* (__attribute__((thiscall)) *GetVertexFn)(void* terrain, int x, int y, int layer);
typedef void (__attribute__((thiscall)) *ClipFn)(void* builder, const float* a, const float* b, const float* c);
const GetVertexFn kGetVertex = reinterpret_cast<GetVertexFn>(0x006C85A0);
const ClipFn kClip = reinterpret_cast<ClipFn>(0x004D8490);
long g_triangles = 0, g_dropped = 0;

// The terrain height at (x, y): the game's own vertices, bilinear; false outside the loaded terrain.
bool HeightAt(void* terrain, float x, float y, float& z) {
    if (x < 0 || y < 0) return false;
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    const float* v00 = kGetVertex(terrain, ix, iy, 0);
    const float* v10 = kGetVertex(terrain, ix + 1, iy, 0);
    const float* v01 = kGetVertex(terrain, ix, iy + 1, 0);
    const float* v11 = kGetVertex(terrain, ix + 1, iy + 1, 0);
    if (!v00 || !v10 || !v01 || !v11) return false;
    z = (v00[2] * (1 - fx) + v10[2] * fx) * (1 - fy) + (v01[2] * (1 - fx) + v11[2] * fx) * fy;
    return true;
}

// Does terrain stand between the receiver point and the caster, on the way to the sun?
bool Blocked(void* terrain, const float* p) {
    const float* s = g_caster.sun;
    const float hx = g_caster.pos[0] - p[0], hy = g_caster.pos[1] - p[1];
    const float sh2 = s[0] * s[0] + s[1] * s[1];
    float reach; // how far along the ray the caster is
    if (sh2 < 1e-4f) reach = (g_caster.pos[2] - p[2]) / std::max(s[2], 0.05f);
    else reach = (hx * s[0] + hy * s[1]) / sh2;
    if (reach <= 0.6f) return false;                 // the caster is not sun-ward of the receiver
    reach = std::min(reach, 60.0f);
    const float step = std::max(0.5f, reach / 48.0f);
    for (float t = step; t < reach; t += step) {
        float z;
        if (!HeightAt(terrain, p[0] + s[0] * t, p[1] + s[1] * t, z)) break;
        if (z > p[2] + s[2] * t + 0.3f + 0.04f * t) return true;
    }
    return false;
}
}

extern "C" void UmShadowBegin(const float* dir, const float* pos) {
    g_caster.active = false;
    if (!dir || !pos) return;
    float sx = -dir[0], sy = -dir[1], sz = -dir[2];       // the light travels down (z up): the sun is the other way
    if (sz < 0) { sx = dir[0]; sy = dir[1]; sz = dir[2]; }
    const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len < 1e-4f || sz / len < 0.05f) return;           // no usable sun
    g_caster.sun[0] = sx / len; g_caster.sun[1] = sy / len; g_caster.sun[2] = sz / len;
    for (int i = 0; i < 3; ++i) g_caster.pos[i] = pos[i];
    g_caster.active = true;
}
extern "C" void UmShadowEnd() {
    static long lastDropped = 0, lastTriangles = 0; static int lines = 0;
    if (g_caster.active && g_dropped > lastDropped && lines < 60 && std::getenv("UM_SHADOW_LAYOUT")) {
        ++lines;
        UmEngineLog("occlusion: caster at (%.1f %.1f %.1f): %ld of %ld receiver triangles dropped", static_cast<double>(g_caster.pos[0]), static_cast<double>(g_caster.pos[1]), static_cast<double>(g_caster.pos[2]), g_dropped - lastDropped, g_triangles - lastTriangles);
    }
    lastDropped = g_dropped; lastTriangles = g_triangles;
    g_caster.active = false;
}

extern "C" void __attribute__((thiscall)) UmClipTriangle(void* builder, const float* a, const float* b, const float* c) {
    if (g_caster.active) {
        void* terrain = *reinterpret_cast<void**>(0x007AF738);
        if (terrain && a && b && c) {
            ++g_triangles;
            const float centre[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
            int blocked = Blocked(terrain, centre) ? 1 : 0;
            blocked += Blocked(terrain, a) ? 1 : 0;
            blocked += Blocked(terrain, b) ? 1 : 0;
            blocked += Blocked(terrain, c) ? 1 : 0;
            static int logged = 0;
            if (logged < 8 && std::getenv("UM_SHADOW_LAYOUT")) { ++logged; UmEngineLog("clip triangle a (%g %g %g) b (%g %g %g) c (%g %g %g), caster (%g %g %g), sun (%g %g %g), blocked %d of 4", static_cast<double>(a[0]), static_cast<double>(a[1]), static_cast<double>(a[2]), static_cast<double>(b[0]), static_cast<double>(b[1]), static_cast<double>(b[2]), static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2]), static_cast<double>(g_caster.pos[0]), static_cast<double>(g_caster.pos[1]), static_cast<double>(g_caster.pos[2]), static_cast<double>(g_caster.sun[0]), static_cast<double>(g_caster.sun[1]), static_cast<double>(g_caster.sun[2]), blocked); }
            if (blocked >= 3) { ++g_dropped; if ((g_dropped & 0x3FFF) == 1) UmEngineLog("shadow receiver: %ld triangles dropped of %ld", g_dropped, g_triangles); return; }
        }
    }
    kClip(builder, a, b, c);
}

extern "C" bool UmShadowOcclusion() {
    return true;
}
