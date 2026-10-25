// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The water's border (docs/water.md): foam where the water meets the land.
//
// The game's water is a second vertex layer of every terrain sector, a sheet at the water's level that the depth buffer cuts where the ground rises above it: the shore is a hard intersection
// line, no border. The ground records (TerrainSector +0x194, 0x54 bytes per vertex, 0x441 per layer) hold, per ground vertex, the water's depth over it (+0x27: water surface minus ground,
// negative on dry land) and the water layer's records say where the sheet exists (+0x25 != 0xFFFF). um-engine collects that depth for the terrain in view into one map (a float per
// vertex, -100 = no water there) and hands it to D7VK (UmWaterMap); right after the game has drawn the water layer D7VK draws the foam (UmWaterEnd): every pixel's world position comes from
// the depth buffer, the map says how deep the water is above it and how steep the ground is there, so the band is as wide as asked in world units whatever the slope.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "um_engine.h"

extern "C" void UmEngineLog(const char* format, ...);
extern "C" bool UmEngineConfigNumbers(const char* const* keys, double* values, int count);
extern "C" bool UmViewParams(float* screenToWorld, float* depth, float* guard);   // world_shadow.cpp

namespace {

// The same layout as dxvk::DxvkUmWaterParams in d7vk (src/dxvk/dxvk_um_shadow.h).
struct Params {
    uint32_t size = sizeof(Params);
    uint32_t flags = 0;
    float screenToWorld[16] = {};
    float depthK = 0, depthFar = 100, depthNear = 2;
    float guard[2] = {};
    float tint[3] = {};
    float strength = 1;
    float width = 0.6f, wet = 0.15f, speed = 1, time = 0;
};

struct Cfg { double width = 0.35, strength = 0.55, wet = 0.3, speed = 1; };
Cfg g_cfg; DWORD g_cfgAt = 0;
const Cfg& C() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        static const char* const keys[4] = { "WaterBorder::Width", "WaterBorder::Strength", "WaterBorder::Wet", "WaterBorder::Speed" };
        double v[4] = { g_cfg.width, g_cfg.strength, g_cfg.wet, g_cfg.speed };
        if (UmEngineConfigNumbers(keys, v, 4)) {
            auto clamp = [](double x, double lo, double hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; };
            g_cfg.width = clamp(v[0], 0.05, 5); g_cfg.strength = clamp(v[1], 0, 1); g_cfg.wet = clamp(v[2], 0.01, 2); g_cfg.speed = clamp(v[3], 0, 10);
        }
    }
    return g_cfg;
}

typedef void (*MapFn)(const float*, int, int);
typedef void (*EndFn)(const Params*);
MapFn g_map = nullptr; EndFn g_end = nullptr; bool g_looked = false;

void Lookup() {
    if (g_looked) return;
    g_looked = true;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    if (module) {
        g_map = reinterpret_cast<MapFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmWaterMap")));
        g_end = reinterpret_cast<EndFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmWaterEnd")));
    }
    UmEngineLog("um-engine: water border %s", g_map && g_end ? "on" : "off (UmWaterMap / UmWaterEnd not found in the DirectDraw library: an older d7vk.dll)");
}

const uintptr_t kWorld = 0x7AF5B8;     // CWorldClient (+0x180: the terrain)
const uintptr_t kSunPtr = 0x7AF07C;    // the sun CLight*

template <typename T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }

std::vector<float> g_values;           // per terrain vertex ((sectors * 32 + 1) squared), two floats: the water's depth over the ground (-100: no water there), the water's level
int g_w = 0, g_h = 0;
bool g_dirty = false;
DWORD g_builtAt = 0;
unsigned g_uploads = 0;

// CTerrain::GetSector (0x6C8140, thiscall: sx, sy, wanted flags, load): the loaded sector with those flags, 0 when it is not loaded (load 0: never loads one)
void* GetSector(void* terrain, int sx, int sy, int flags) {
    typedef void* (__attribute__((thiscall)) *Fn)(void*, int, int, int, int);
    return reinterpret_cast<Fn>(0x006C8140)(terrain, sx, sy, flags, 0);
}

// The terrain in view: its sectors' records into the map (what the game holds for a sector that has been out of view stays in the map: the water does not move)
void Collect(void* terrain) {
    const int sectorsX = Read<int>(reinterpret_cast<uintptr_t>(terrain) + 0x104), sectorsY = Read<int>(reinterpret_cast<uintptr_t>(terrain) + 0x108);
    if (sectorsX < 1 || sectorsY < 1 || sectorsX > 64 || sectorsY > 64) return;
    const int w = sectorsX * 32 + 1, h = sectorsY * 32 + 1;
    if (w != g_w || h != g_h) { g_w = w; g_h = h; g_values.assign(static_cast<size_t>(w) * h * 2, -100.0f); g_dirty = true; }
    const uintptr_t t = reinterpret_cast<uintptr_t>(terrain);
    const int rows = Read<int>(t + 0xAAC), first = Read<int>(t + 0xAA4);
    const int* minCol = Read<const int*>(t + 0xAA8);
    const int* maxCol = Read<const int*>(t + 0xAB8);
    if (!minCol || !maxCol || rows < 1 || rows > 64) return;
    const uintptr_t materials = Read<uintptr_t>(t + 0x12C);   // the water materials (0x38 bytes: +0 type, +4 / +8 / +0xC the liquid's colour r g b)
    auto isWater = [&](unsigned material) {                      // lava and the like (a red liquid) get no foam
        if (!materials || material > 63) return true;
        const float r = Read<float>(materials + material * 0x38 + 4), g = Read<float>(materials + material * 0x38 + 8), b = Read<float>(materials + material * 0x38 + 0xC);
        return !(r > std::fmax(g, b));
    };
    for (int r = 0; r < rows; ++r) {
        const int row = first + r;
        if (row < 0 || row >= sectorsY) continue;
        for (int col = std::max(minCol[r], 0); col <= std::min(maxCol[r], sectorsX - 1); ++col) {
            const uintptr_t sector = reinterpret_cast<uintptr_t>(GetSector(terrain, col, row, 1));
            if (!sector || Read<int>(sector + 0x10) == 0) continue;   // not loaded / no water layer
            const uintptr_t records = Read<uintptr_t>(sector + 0x194);
            if (!records || Read<int>(sector + 0x198) < 0x882) continue;
            for (int i = 0; i < 0x441; ++i) {
                const unsigned short material = Read<unsigned short>(records + (0x441 + i) * 0x54 + 0x25);   // the water layer's vertex: 0xFFFF = no water
                float depth = Read<float>(records + i * 0x54 + 0x27);                                      // the ground vertex: water surface minus ground
                float level = Read<float>(records + (0x441 + i) * 0x54 + 0x21);                      // the water layer's vertex: the surface's base height
                if (material == 0xFFFF || !isWater(material) || !(depth > -90.0f && depth < 90.0f) || !(level > -1000.0f && level < 1000.0f)) { depth = -100.0f; level = -100.0f; }
                const int gx = col * 32 + i % 33, gy = row * 32 + i / 33;
                float* slot = &g_values[(static_cast<size_t>(gy) * w + gx) * 2];
                if (slot[0] != depth || slot[1] != level) { slot[0] = depth; slot[1] = level; g_dirty = true; }
            }
        }
    }
}

} // namespace

// Called after the game has drawn the water layer (the call site after CTerrain::RenderWater in CWorldClient::Render).
extern "C" __attribute__((used, force_align_arg_pointer)) void UmWaterEnd() {
    Lookup();
    if (!g_map || !g_end) return;
    void* terrain = Read<void*>(kWorld + 0x180);
    if (!terrain || Read<int>(reinterpret_cast<uintptr_t>(terrain) + 0x100) == 0) return;   // no terrain / no sector data

    const DWORD now = GetTickCount();
    if (g_builtAt == 0 || now - g_builtAt > 400) {
        g_builtAt = now ? now : 1;
        Collect(terrain);
        if (g_dirty && g_w > 1) { g_map(g_values.data(), g_w, g_h); g_dirty = false; ++g_uploads; }
    }
    if (g_w < 2) return;

    Params p;
    float depth[3];
    if (!UmViewParams(p.screenToWorld, depth, p.guard)) return;
    p.depthK = depth[0]; p.depthFar = depth[1]; p.depthNear = depth[2];

    // the foam is lit like the world: the ambient colour plus what the sun gives a surface facing up
    const uintptr_t sun = Read<uintptr_t>(kSunPtr);
    float sunColor[3] = {}, sunUp = 0;
    if (sun) { for (int i = 0; i < 3; ++i) sunColor[i] = Read<float>(sun + 0x98 + 4 * i); sunUp = std::fmax(-Read<float>(sun + 0xDC), 0.15f); }
    for (int i = 0; i < 3; ++i) {
        const float light = Read<float>(kWorld + 0x6AC + 4 * i) + sunColor[i] * sunUp;
        p.tint[i] = std::fmin(std::fmax(light, 0.06f), 1.0f);
    }
    const Cfg& c = C();
    p.strength = static_cast<float>(c.strength); p.width = static_cast<float>(c.width); p.wet = static_cast<float>(c.wet); p.speed = static_cast<float>(c.speed);
    p.time = static_cast<float>(std::fmod(now * 0.001, 3600.0));
    g_end(&p);
}

// The call site after CTerrain::RenderWater (0x56A120: call CRenderDevice::FlushVertexBuffer2 0x4E7A40 on the device in ecx): the original call, then the foam.
asm(R"(
.text
.globl _UmWaterEndStub
_UmWaterEndStub:
    movl $0x004E7A40, %eax
    call *%eax
    pushal
    pushfl
    call _UmWaterEnd
    popfl
    popal
    ret
)");
