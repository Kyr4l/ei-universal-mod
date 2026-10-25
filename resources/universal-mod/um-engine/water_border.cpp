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

bool g_dirty = false;   // the map changed (or a setting that shapes it): upload it again
struct Cfg { double width = 0.25, strength = 0.5, wet = 0.3, speed = 1, style = 1, minEdge = 24; };
Cfg g_cfg; DWORD g_cfgAt = 0;
const Cfg& C() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        static const char* const keys[6] = { "WaterBorder::Width", "WaterBorder::Strength", "WaterBorder::Wet", "WaterBorder::Speed", "WaterBorder::Style", "WaterBorder::MinEdge" };
        double v[6] = { g_cfg.width, g_cfg.strength, g_cfg.wet, g_cfg.speed, g_cfg.style, g_cfg.minEdge };
        if (UmEngineConfigNumbers(keys, v, 6)) {
            auto clamp = [](double x, double lo, double hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; };
            g_cfg.width = clamp(v[0], 0.05, 5); g_cfg.strength = clamp(v[1], 0, 1); g_cfg.wet = clamp(v[2], 0.01, 2); g_cfg.speed = clamp(v[3], 0, 10); g_cfg.style = clamp(v[4], 0, 2); { const double e = clamp(v[5], 0, 400); if (e != g_cfg.minEdge) g_dirty = true; g_cfg.minEdge = e; }
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

std::vector<float> g_raw;              // per terrain vertex ((sectors * 32 + 1) squared), four floats: the water's depth over the ground (-100: no water there), the water's level, the liquid's colour (r g b as 8 bits each in one float), 0
std::vector<float> g_values;           // the same after the short shores are taken out (what D7VK gets)
int g_w = 0, g_h = 0;
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
    if (w != g_w || h != g_h) { g_w = w; g_h = h; g_raw.assign(static_cast<size_t>(w) * h * 4, 0.0f); for (size_t i = 0; i < g_raw.size(); i += 4) g_raw[i] = -100.0f; g_dirty = true; }
    const uintptr_t t = reinterpret_cast<uintptr_t>(terrain);
    const int rows = Read<int>(t + 0xAAC), first = Read<int>(t + 0xAA4);
    const int* minCol = Read<const int*>(t + 0xAA8);
    const int* maxCol = Read<const int*>(t + 0xAB8);
    if (!minCol || !maxCol || rows < 1 || rows > 64) return;
    const uintptr_t materials = Read<uintptr_t>(t + 0x12C);   // the water materials (0x38 bytes: +0 type, +4 / +8 / +0xC the liquid's colour r g b)
    auto liquid = [&](unsigned material, float& packed) {         // the liquid's colour packed as 0xRRGGBB; false for lava and the like (a red liquid gets no foam)
        packed = 0.0f;
        if (!materials || material > 63) return false;
        const float r = Read<float>(materials + material * 0x38 + 4), g = Read<float>(materials + material * 0x38 + 8), b = Read<float>(materials + material * 0x38 + 0xC);
        if (r > std::fmax(g, b)) return false;
        auto byte = [](float v) { return static_cast<int>(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
        packed = static_cast<float>((byte(r) << 16) | (byte(g) << 8) | byte(b));
        return true;
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
                float packed = 0.0f;
                if (material == 0xFFFF || !liquid(material, packed) || !(depth > -90.0f && depth < 90.0f) || !(level > -1000.0f && level < 1000.0f)) { depth = -100.0f; level = -100.0f; packed = 0.0f; }
                const int gx = col * 32 + i % 33, gy = row * 32 + i / 33;
                float* slot = &g_raw[(static_cast<size_t>(gy) * w + gx) * 4];
                if (slot[0] != depth || slot[1] != level || slot[2] != packed) { slot[0] = depth; slot[1] = level; slot[2] = packed; g_dirty = true; }
            }
        }
    }
}

// A shore that is short is no shore: a piece of dry ground surrounded by water (an islet, a stone in the shallows) or a puddle of water in the ground whose edge is shorter than `minEdge`
// world units (about four times the square root of its area) gets none: its cells are turned into the other kind. Regions that reach cells nothing is known about (the map's edge, sectors not loaded
// yet) may continue there and are kept.
void RemoveShortShores(double minEdge) {
    const size_t cells = static_cast<size_t>(g_w) * g_h;
    g_values = g_raw;
    if (minEdge <= 0 || cells == 0) return;
    const int maxArea = static_cast<int>(std::pow(minEdge / 4.0, 2.0));
    std::vector<unsigned char> seen(cells, 0);
    std::vector<size_t> stack, region;
    auto depthAt = [&](size_t i) { return g_values[i * 4]; };
    for (size_t start = 0; start < cells; ++start) {
        if (seen[start] || depthAt(start) < -50.0f) continue;
        const bool wet = depthAt(start) > 0.0f;
        bool open = false;
        stack.clear(); region.clear();
        stack.push_back(start); seen[start] = 1;
        while (!stack.empty()) {
            const size_t i = stack.back(); stack.pop_back();
            region.push_back(i);
            const int x = static_cast<int>(i % g_w), y = static_cast<int>(i / g_w);
            const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= g_w || ny[k] >= g_h) { open = true; continue; }
                const size_t j = static_cast<size_t>(ny[k]) * g_w + nx[k];
                const float d = depthAt(j);
                if (d < -50.0f) { open = true; continue; }
                if ((d > 0.0f) != wet || seen[j]) continue;
                seen[j] = 1; stack.push_back(j);
            }
        }
        if (open || static_cast<int>(region.size()) >= maxArea) continue;
        for (size_t i : region) g_values[i * 4] = wet ? -0.4f : 0.4f;   // the small water becomes ground, the small land becomes water: no zero crossing
    }
}

// The shores of the whole terrain (the rig's ENGINE WATERSPOT): loads every sector (it is a research tool) and returns, per sector, the number of lattice cells where the water's edge runs
// (a water vertex deeper than 0 next to one that is not) and their centre. The best `max` sectors, most shoreline first. Returns the number written (x, y, count per spot).
int Spots(float* out, int max) {
    void* terrain = Read<void*>(kWorld + 0x180);
    if (!terrain || Read<int>(reinterpret_cast<uintptr_t>(terrain) + 0x100) == 0) return 0;
    const uintptr_t t = reinterpret_cast<uintptr_t>(terrain);
    const int sectorsX = Read<int>(t + 0x104), sectorsY = Read<int>(t + 0x108);
    if (sectorsX < 1 || sectorsY < 1 || sectorsX > 64 || sectorsY > 64) return 0;
    struct Spot { float x, y; int count; };
    std::vector<Spot> spots;
    typedef void* (__attribute__((thiscall)) *LoadFn)(void*, int, int, int, int);
    for (int row = 0; row < sectorsY; ++row) for (int col = 0; col < sectorsX; ++col) {
        const uintptr_t sector = reinterpret_cast<uintptr_t>(reinterpret_cast<LoadFn>(0x006C8140)(terrain, col, row, 1, 1));
        if (!sector || Read<int>(sector + 0x10) == 0) continue;
        const uintptr_t records = Read<uintptr_t>(sector + 0x194);
        if (!records || Read<int>(sector + 0x198) < 0x882) continue;
        float depth[0x441];
        for (int i = 0; i < 0x441; ++i) {
            const unsigned short material = Read<unsigned short>(records + (0x441 + i) * 0x54 + 0x25);
            const float d = Read<float>(records + i * 0x54 + 0x27);
            depth[i] = (material == 0xFFFF || !(d > -90.0f && d < 90.0f)) ? -100.0f : d;
        }
        double sx = 0, sy = 0; int n = 0;
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const float a = depth[y * 33 + x], b = depth[y * 33 + x + 1], c = depth[(y + 1) * 33 + x];
            if (a < -50.0f || b < -50.0f || c < -50.0f) continue;
            if ((a > 0) != (b > 0) || (a > 0) != (c > 0)) { sx += col * 32 + x; sy += row * 32 + y; ++n; }
        }
        if (n >= 6) spots.push_back({ static_cast<float>(sx / n), static_cast<float>(sy / n), n });
    }
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.count > b.count; });
    int written = 0;
    for (size_t i = 0; i < spots.size() && written < max; ++i, ++written) { out[written * 3] = spots[i].x; out[written * 3 + 1] = spots[i].y; out[written * 3 + 2] = static_cast<float>(spots[i].count); }
    return written;
}

} // namespace

extern "C" int UmWaterSpots(float* out, int max) { return Spots(out, max); }

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
        if (g_dirty && g_w > 1) { RemoveShortShores(C().minEdge); g_map(g_values.data(), g_w, g_h); g_dirty = false; ++g_uploads; }
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
    p.strength = static_cast<float>(c.strength); p.width = static_cast<float>(c.width); p.wet = static_cast<float>(c.wet); p.speed = static_cast<float>(c.speed); p.flags = static_cast<uint32_t>(c.style + 0.5);
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
