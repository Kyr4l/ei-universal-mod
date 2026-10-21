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
#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);
extern "C" void UmShadowVolumeCall();
extern "C" { void* UmShadowRet = nullptr; }

asm(R"(
.text
.globl _UmShadowVolumeCall
_UmShadowVolumeCall:
    popl %eax
    movl %eax, _UmShadowRet
    pushl $_UmShadowVolumeBack
    movl $0x0056C750, %eax
    jmp *%eax
_UmShadowVolumeBack:
    pushl %ebx
    call _UmShadowFilter
    addl $4, %esp
    jmp *_UmShadowRet
)");

// `unit` = the CObject3DClientSpecific whose receiver mesh was just built.
extern "C" void UmShadowFilter(unsigned char*) {}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The alpha map
namespace {
struct AlphaMap { const void* texture = nullptr; unsigned width = 0, height = 0; bool transparent = false; unsigned solid = 0; unsigned fourcc = 0; uint16_t pixels[256 * 256]; };
AlphaMap* g_maps[96]; int g_mapCount = 0;

typedef unsigned (__attribute__((thiscall)) *LockFn)(void*, void*);
typedef void (__attribute__((thiscall)) *UnlockFn)(void*);

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

extern "C" bool UmShadowOcclusion() {
    return true;
}
