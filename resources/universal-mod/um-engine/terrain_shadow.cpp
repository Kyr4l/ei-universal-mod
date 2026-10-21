// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Softer terrain shadows (RE: docs/shadows.md, CTerrain::TraceSunRay 0x6C92C0, TerrainSector_LightVertexSun 0x6C36B0).
// The terrain's own sun shadow is a yes / no per terrain vertex: the vertex is lit, or it gets the ambient colour only, so the edge of a shadow
// follows the vertex grid. LightVertexSun calls TraceSunRay (the one call site, 0x6C389F) and, when the sun reaches the vertex, lights it with
// the sun scaled by its local intensity at [ebp-4]. Here that call goes through UmTraceSunWrapper, which traces the ray at the vertex height (the argument is the vertex: x, y, z floats) and at
// four heights above it, up to `spread` higher (the penumbra: a ray that only just grazes the relief is partly lit) and scales the sun by the fraction that got through.
#include <windows.h>

#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);
extern "C" void UmTraceSunWrapper();

namespace {
typedef unsigned (__attribute__((thiscall)) *TraceFn)(void* terrain, const float* vertex); // reads only the vertex's height, vertex[2]
const TraceFn kTrace = reinterpret_cast<TraceFn>(0x006C92C0);
const unsigned long kCallSite = 0x006C389F;
float g_spread = 0.5f; // the highest sample above the vertex (game units)
}

// terrain: the CTerrain; vertex: the vertex (x, y, z floats); callerEbp: the frame of LightVertexSun (its sun intensity is the float at [ebp - 4]).
extern "C" unsigned UmSoftTrace(void* terrain, const float* vertex, char* callerEbp) {
    static const float kOffsets[4] = {0.25f, 0.5f, 0.75f, 1.0f}; // of the spread, upwards only: a point sunk below the ground would be shadowed by its own cell
    int lit = kTrace(terrain, vertex) ? 1 : 0;
    for (float k : kOffsets) {
        const float raised[3] = {vertex[0], vertex[1], vertex[2] + k * g_spread};
        lit += kTrace(terrain, raised) ? 1 : 0;
    }
    if (lit == 0) return 0;                      // in the full shadow: the game's own shadow colour
    if (lit < 5) *reinterpret_cast<float*>(callerEbp - 4) *= static_cast<float>(lit) / 5.0f; // a penumbra: the sun partly
    return 1;
}

asm(R"(
.text
.globl _UmTraceSunWrapper
_UmTraceSunWrapper:
    pushl %ebp
    pushl 8(%esp)
    pushl %ecx
    call _UmSoftTrace
    addl $12, %esp
    ret $4
)");

// Installs the softer shadow (spread = the height step in game units, > 0). All or nothing: the call site must be the game's own.
extern "C" bool UmTerrainShadowSoftness(float spread) {
    static const unsigned char expect[5] = {0xE8, 0x1C, 0x5A, 0x00, 0x00};
    if (spread <= 0) return false;
    if (memcmp(reinterpret_cast<void*>(kCallSite), expect, 5) != 0) { UmEngineLog("um-engine: TerrainShadow::Softness skipped, game.exe has other code at the trace call (another build)"); return false; }
    g_spread = spread;
    unsigned char* at = reinterpret_cast<unsigned char*>(kCallSite);
    DWORD old = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const long rel = reinterpret_cast<long>(&UmTraceSunWrapper) - reinterpret_cast<long>(at + 5);
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    UmEngineLog("um-engine: TerrainShadow::Softness installed (spread %g)", static_cast<double>(spread));
    return true;
}
