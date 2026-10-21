// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Unit / object shadows that respect the terrain (RE: docs/shadows.md). The game lays a unit's shadow over every terrain triangle inside a prism extruded
// from the unit along the light (CWorldClient::RenderShadowVolume 0x56C750, called from CObject3DClientSpecific::Render at 0x51E08B), so a bush on a plateau
// throws its shadow onto the ground below a cliff that the light never reaches. The two Clip_TriangleToFrustumA calls that feed the receiver mesh go through
// UmClipTriangle, which leaves out the triangles that lie in the terrain's own shade (a per-vertex ray march towards the sun, computed once and cached).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
extern "C" void UmShadowFilter(unsigned char* unit) {
    static const bool log = std::getenv("UM_SHADOW_LAYOUT") != nullptr; // research: the size of the unit's own shadow texture
    static int logged = 0;
    if (!log || logged >= 4) return;
    unsigned char* own = *reinterpret_cast<unsigned char**>(unit + 0x7C);
    if (!own) return;
    typedef unsigned (__attribute__((thiscall)) *LockFn)(void*, void*);
    typedef void (__attribute__((thiscall)) *UnlockFn)(void*);
    alignas(8) unsigned char desc[128];
    if (!reinterpret_cast<LockFn>(0x00690FC0)(own, desc)) return;
    const unsigned* d = reinterpret_cast<const unsigned*>(desc);
    UmEngineLog("unit shadow texture: surface %ux%u, pitch %u (object says %ux%u)", d[3], d[2], d[4], *reinterpret_cast<const unsigned*>(own + 0x20), *reinterpret_cast<const unsigned*>(own + 0x24));
    reinterpret_cast<UnlockFn>(0x00691030)(own);
    ++logged;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The terrain occlusion: ClipAndAddTriangle (0x6CCF20) feeds every terrain triangle of the prism to Clip_TriangleToFrustumA (0x4D8490, thiscall, 3 vertex
// pointers); the two calls are redirected here. A ground point the terrain itself keeps from the sun is already dark, so a caster's shadow does not belong
// there (a bush on a plateau throws none behind the cliff): the triangles with two or more corners in the terrain's shade are not given to the clipper.
// The sun is a fixed vector, so whether a terrain vertex sees the sun is computed once (a ray march over the terrain heights) and kept.
namespace {
struct Caster { bool active = false; } g_caster;
float g_sun[3] = {0, 0, 1};  // the unit vector towards the sun (set from the first usable light direction)
bool g_sunKnown = false;
typedef const float* (__attribute__((thiscall)) *GetVertexFn)(void* terrain, int x, int y, int layer);
typedef void (__attribute__((thiscall)) *ClipFn)(void* builder, const float* a, const float* b, const float* c);
const GetVertexFn kGetVertex = reinterpret_cast<GetVertexFn>(0x006C85A0);
const ClipFn kClip = reinterpret_cast<ClipFn>(0x004D8490);
long g_triangles = 0, g_dropped = 0;

struct ShadeMap {
    void* terrain = nullptr; int width = 0, height = 0; float zmax = 100;
    std::vector<float> z;        // heights seen so far (NaN: not read yet)
    std::vector<uint8_t> shade;  // 0 unknown, 1 sees the sun, 2 in the terrain's shade
} g_map;

void Prepare(void* terrain) {
    const unsigned sx = *reinterpret_cast<const unsigned*>(static_cast<const unsigned char*>(terrain) + 0x104), sy = *reinterpret_cast<const unsigned*>(static_cast<const unsigned char*>(terrain) + 0x108);
    const float zmax = *reinterpret_cast<const float*>(static_cast<const unsigned char*>(terrain) + 0x114);
    if (g_map.terrain == terrain && g_map.width == static_cast<int>(sx * 32 + 1) && g_map.height == static_cast<int>(sy * 32 + 1)) return;
    g_map.terrain = terrain;
    g_map.width = static_cast<int>(sx * 32 + 1); g_map.height = static_cast<int>(sy * 32 + 1);
    g_map.zmax = (zmax > 1 && zmax < 2000) ? zmax : 200.0f;
    const size_t n = static_cast<size_t>(g_map.width) * static_cast<size_t>(g_map.height);
    g_map.z.assign(n, std::nanf("")); g_map.shade.assign(n, 0);
}

// The height of terrain vertex (ix, iy), read once through the game's own GetVertex; false when that part of the terrain is not loaded.
bool VertexHeight(int ix, int iy, float& z) {
    if (ix < 0 || iy < 0 || ix >= g_map.width || iy >= g_map.height) return false;
    float& slot = g_map.z[static_cast<size_t>(iy) * g_map.width + ix];
    if (std::isnan(slot)) { const float* v = kGetVertex(g_map.terrain, ix, iy, 0); if (!v) return false; slot = v[2]; }
    z = slot;
    return true;
}
bool HeightAt(float x, float y, float& z) {
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    float a, b, c, d;
    if (!VertexHeight(ix, iy, a) || !VertexHeight(ix + 1, iy, b) || !VertexHeight(ix, iy + 1, c) || !VertexHeight(ix + 1, iy + 1, d)) return false;
    z = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    return true;
}

// Is terrain vertex (ix, iy) in the terrain's own shade? (cached)
bool InShade(int ix, int iy) {
    if (ix < 0 || iy < 0 || ix >= g_map.width || iy >= g_map.height) return false;
    uint8_t& st = g_map.shade[static_cast<size_t>(iy) * g_map.width + ix];
    if (st) return st == 2;
    float z0;
    if (!VertexHeight(ix, iy, z0)) return false; // not loaded: undecided, nothing cached
    bool blocked = false;
    const float x0 = static_cast<float>(ix), y0 = static_cast<float>(iy);
    for (float t = 1.0f; t <= 48.0f; t += 1.0f) {
        const float rz = z0 + g_sun[2] * t;
        if (rz > g_map.zmax) break;                       // above all terrain
        float z;
        if (!HeightAt(x0 + g_sun[0] * t, y0 + g_sun[1] * t, z)) break;
        if (z > rz + 0.3f + 0.04f * t) { blocked = true; break; }
    }
    st = blocked ? 2 : 1;
    return blocked;
}
}

extern "C" void UmShadowBegin(const float* dir, const float*) {
    g_caster.active = false;
    if (!dir) return;
    if (!g_sunKnown) {
        float sx = -dir[0], sy = -dir[1], sz = -dir[2];       // the light travels down (z up): the sun is the other way
        if (sz < 0) { sx = dir[0]; sy = dir[1]; sz = dir[2]; }
        const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (len < 1e-4f || sz / len < 0.05f) return;
        g_sun[0] = sx / len; g_sun[1] = sy / len; g_sun[2] = sz / len;
        g_sunKnown = true;
        UmEngineLog("terrain occlusion: the sun is (%g %g %g)", static_cast<double>(g_sun[0]), static_cast<double>(g_sun[1]), static_cast<double>(g_sun[2]));
    }
    if (void* terrain = *reinterpret_cast<void**>(0x007AF738)) { Prepare(terrain); g_caster.active = g_map.terrain == terrain; }
}
extern "C" void UmShadowEnd() { g_caster.active = false; }

extern "C" void __attribute__((thiscall)) UmClipTriangle(void* builder, const float* a, const float* b, const float* c) {
    static const bool timing = std::getenv("UM_SHADOW_TIME") != nullptr; // research: the cost of the occlusion tests, logged every 5 s
    static LARGE_INTEGER freq = {}, lastLog = {}; static long long spent = 0; static long calls = 0;
    LARGE_INTEGER t0 = {};
    if (timing) { if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&lastLog); } QueryPerformanceCounter(&t0); }
    if (g_caster.active && a && b && c) {
        ++g_triangles;
        // two of the three corners in shade (the corner's nearest terrain vertex): the triangle is dropped
        const int n = (InShade(static_cast<int>(a[0] + 0.5f), static_cast<int>(a[1] + 0.5f)) ? 1 : 0) + (InShade(static_cast<int>(b[0] + 0.5f), static_cast<int>(b[1] + 0.5f)) ? 1 : 0) +
                      (InShade(static_cast<int>(c[0] + 0.5f), static_cast<int>(c[1] + 0.5f)) ? 1 : 0);
        if (timing) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); spent += t1.QuadPart - t0.QuadPart; ++calls; if (t1.QuadPart - lastLog.QuadPart > 5 * freq.QuadPart) { UmEngineLog("occlusion cost: %ld triangles tested, %.2f ms in 5 s (%.2f us each), %ld dropped", calls, 1000.0 * static_cast<double>(spent) / static_cast<double>(freq.QuadPart), 1e6 * static_cast<double>(spent) / static_cast<double>(freq.QuadPart) / static_cast<double>(std::max(1L, calls)), g_dropped); spent = 0; calls = 0; lastLog = t1; } }
        if (n >= 2) { if ((++g_dropped & 0xFFFF) == 1) UmEngineLog("shadow receiver: %ld triangles dropped of %ld", g_dropped, g_triangles); return; }
    }
    kClip(builder, a, b, c);
}

extern "C" bool UmShadowOcclusion() {
    return true;
}
