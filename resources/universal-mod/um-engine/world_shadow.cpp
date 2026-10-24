// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Sun shadows (docs/shadows.md, PENDING #141): the game-side driver of D7VK's shadow pass.
//
// The stock game only drapes bitmaps of the casters over the TERRAIN (units, trees and buildings never shadow other objects or units, the terrain never
// shadows objects). With D7VK (d7vk.dll / ddraw.dll exporting UmShadowBegin / UmShadowEnd) every depth-writing draw of the world is also drawn into a
// shadow map seen from the sun, and the colour buffer is darkened where the sun does not reach, so every surface can receive and cast shadows.
//
// Two call sites in CWorldClient::Render (0x569E40) bracket the world pass: the call of CTerrain::RenderOpaque (0x56A06B: begin) and the call of
// CWorldClient::RenderObjects (0x56A080: end, before water, particles and effects). The sun, the ambient colour and the camera / projection / viewport matrices are
// read from the game's own state at the end of the pass. The game's own shadow pass (world flag 0x200) is switched off while this works.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);
extern "C" double UmEngineConfigNumber(const char* key, double fallback); // um_engine.cpp

namespace {

// The same layout as dxvk::DxvkUmShadowParams in d7vk (src/dxvk/dxvk_um_shadow.h).
struct Params {
    uint32_t size = sizeof(Params);
    uint32_t flags = 0;
    float screenToWorld[16] = {};
    float lightDir[3] = {};
    float strength = 1.0f;
    float sunColor[3] = {};
    float bias = 0.0015f;
    float ambient[3] = {};
    float slopeBias = 0.004f;
    float depthK = 0.0f;
    float depthFar = 100.0f;
    float depthNear = 2.0f;
    float cameraPos[3] = {};
    float center[3] = {};
    float radius = 40.0f;
    float height = 60.0f;
    uint32_t mapSize = 2048;
    float extra = 0.0f;
};

// um-engine.cfg numbers, re-read once a second (the file is not parsed every frame; editing it while the game runs works)
struct Cfg { double margin = 8, height = 50, strength = 1, bias = 0.0015, slopeBias = 0.004, mapSize = 2048, extra = 0.3, debug = 0, indoor = 0; };
Cfg g_cfg; DWORD g_cfgAt = 0;
const Cfg& C() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        g_cfg.margin = UmEngineConfigNumber("SunShadows::Margin", 8.0);
        g_cfg.height = UmEngineConfigNumber("SunShadows::Height", 50.0);
        g_cfg.strength = UmEngineConfigNumber("SunShadows::Strength", 1.0);
        g_cfg.bias = UmEngineConfigNumber("SunShadows::Bias", 0.0015);
        g_cfg.slopeBias = UmEngineConfigNumber("SunShadows::SlopeBias", 0.004);
        g_cfg.mapSize = UmEngineConfigNumber("SunShadows::MapSize", 2048.0);
        g_cfg.extra = UmEngineConfigNumber("SunShadows::Extra", 0.3);
        g_cfg.debug = UmEngineConfigNumber("SunShadows::Debug", 0.0);
        g_cfg.indoor = UmEngineConfigNumber("SunShadows::Indoor", 0.0);
    }
    return g_cfg;
}

typedef void (*BeginFn)();
typedef void (*EndFn)(const Params*);

BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
bool g_looked = false, g_enabled = false, g_open = false;
Params g_params;

const uintptr_t kRenderGlobals = 0x7AF068;   // the matrix stacks (g_0x7AF068)
const uintptr_t kCamera = 0x79B2A8;
const uintptr_t kWorld = 0x7AF5B8;           // CWorldClient
const uintptr_t kSunPtr = 0x7AF07C;          // the sun CLight*

template <typename T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }

// Top matrix of a stack of the render globals (offset of {buffer*, depth}): 16 floats, row-major, column-vector convention.
bool StackTop(uintptr_t offset, double out[16]) {
    const uintptr_t globals = Read<uintptr_t>(kRenderGlobals);
    if (!globals) return false;
    const float* base = Read<const float*>(globals + offset);
    const int depth = Read<int>(globals + offset + 4);
    if (!base || depth < 1 || depth > 64) return false;
    const float* top = base + (depth - 1) * 16;
    for (int i = 0; i < 16; ++i) out[i] = top[i];
    return true;
}

void Mul(const double* a, const double* b, double* o) { // o = a * b (row-major)
    double r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = v;
        }
    std::memcpy(o, r, sizeof(r));
}

bool Invert(const double* m, double* inv) { // Gauss-Jordan
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) { a[i][j] = m[i * 4 + j]; a[i][4 + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-18) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) inv[i * 4 + j] = a[i][4 + j];
    return true;
}

void Transform(const double* inv, double x, double y, double z, double w, double out[3]) { // inv * (x, y, z, w) -> xyz / w
    double v[4] = { x, y, z, w }, r[4];
    for (int i = 0; i < 4; ++i) r[i] = inv[i * 4] * v[0] + inv[i * 4 + 1] * v[1] + inv[i * 4 + 2] * v[2] + inv[i * 4 + 3] * v[3];
    const double iw = std::fabs(r[3]) > 1e-12 ? 1.0 / r[3] : 1.0;
    out[0] = r[0] * iw; out[1] = r[1] * iw; out[2] = r[2] * iw;
}

void Lookup() {
    if (g_looked) return;
    g_looked = true;
    if (UmEngineConfigNumber("SunShadows::Enabled", 1.0) < 0.5) return;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    if (!module) return;
    g_begin = reinterpret_cast<BeginFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowBegin")));
    g_end = reinterpret_cast<EndFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowEnd")));
    g_enabled = g_begin && g_end;
    UmEngineLog("um-engine: sun shadows %s (UmShadowBegin / UmShadowEnd %sfound in the DirectDraw library)", g_enabled ? "on" : "off", g_enabled ? "" : "not ");
}

// Fills the parameters from the game's state; false when something is not ready (no frame is shadowed then).
bool Gather(Params& p) {
    // The pipeline's own matrices (found by fitting the terrain sectors' world vertices against their transformed records, 2026-10-24): the stack at +0x14 holds, on top,
    // the projection with the viewport folded in (and the widescreen x scale of 0.75): screen * w = Mc * view-space; +0x5C is the camera's view matrix.
    // (The projection stack at +0x44 is NOT what is used: it still holds the 4:3 matrix.)
    double view[16], proj[16], m[16], inv[16];
    if (!StackTop(0x14, proj) || !StackTop(0x5C, view)) return false;
    Mul(proj, view, m);
    if (!Invert(m, inv)) return false;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p.screenToWorld[c * 4 + r] = static_cast<float>(inv[r * 4 + c]); // column-major for the GPU

    // z -> clip w: cw = K / (Fp - z * Fq), K = -m14 * m11, Fp = m10, Fq = m14 (rows 2 and 3 of the combined matrix)
    const double m10 = proj[10], m11 = proj[11], m14 = proj[14];
    if (std::fabs(m14) < 1e-9) return false;
    p.depthK = static_cast<float>(-m14 * m11);
    p.depthFar = static_cast<float>(m10);
    p.depthNear = static_cast<float>(m10 - m14);

    // the sun: direction the light travels, colour; the ambient colour of the day / night tables
    const uintptr_t sun = Read<uintptr_t>(kSunPtr);
    if (!sun) return false;
    p.lightDir[0] = Read<float>(sun + 0xD4); p.lightDir[1] = Read<float>(sun + 0xD8); p.lightDir[2] = Read<float>(sun + 0xDC);
    p.sunColor[0] = Read<float>(sun + 0x98); p.sunColor[1] = Read<float>(sun + 0x9C); p.sunColor[2] = Read<float>(sun + 0xA0);
    p.ambient[0] = Read<float>(kWorld + 0x6AC); p.ambient[1] = Read<float>(kWorld + 0x6B0); p.ambient[2] = Read<float>(kWorld + 0x6B4);

    for (int i = 0; i < 3; ++i) p.cameraPos[i] = Read<float>(kCamera + 0x1C + 4 * i);

    // The shadowed area: the four screen corners seen on the ground plane at the camera's target height
    double vp[16];
    if (!StackTop(0x54, vp)) return false;
    const double hw = vp[0], hh = -vp[5];
    const double width = 2 * hw, height = 2 * hh;
    const double groundZ = Read<float>(kCamera + 0xE8);
    double cx = 0, cy = 0, cz = groundZ;
    double hits[4][3];
    int n = 0;
    const double xs[4] = { 0, width, 0, width }, ys[4] = { 0, 0, height, height };
    for (int i = 0; i < 4; ++i) {
        const double z0 = 0.05, z1 = 0.9;
        const double w0 = p.depthK / (p.depthFar - z0 * (p.depthFar - p.depthNear)), w1 = p.depthK / (p.depthFar - z1 * (p.depthFar - p.depthNear));
        double a[3], b[3];
        Transform(inv, xs[i] * w0, ys[i] * w0, z0 * w0, w0, a);
        Transform(inv, xs[i] * w1, ys[i] * w1, z1 * w1, w1, b);
        const double dz = b[2] - a[2];
        double t = std::fabs(dz) > 1e-9 ? (groundZ - a[2]) / dz : 1.0;
        if (t < 0 || t > 3) t = 1.0;   // not hitting the ground plane: use the far point
        for (int k = 0; k < 3; ++k) hits[n][k] = a[k] + (b[k] - a[k]) * t;
        ++n;
    }
    for (int i = 0; i < n; ++i) { cx += hits[i][0]; cy += hits[i][1]; }
    cx /= n; cy /= n;
    double radius = 0;
    for (int i = 0; i < n; ++i) radius = std::fmax(radius, std::hypot(hits[i][0] - cx, hits[i][1] - cy));
    p.center[0] = static_cast<float>(cx); p.center[1] = static_cast<float>(cy); p.center[2] = static_cast<float>(cz);
    const Cfg& cfg = C();
    p.radius = static_cast<float>(std::fmin(std::fmax(radius + cfg.margin, 15.0), 120.0));
    p.height = static_cast<float>(cfg.height);
    p.strength = static_cast<float>(cfg.strength);
    p.bias = static_cast<float>(cfg.bias);
    p.slopeBias = static_cast<float>(cfg.slopeBias);
    p.mapSize = static_cast<uint32_t>(cfg.mapSize);
    p.extra = static_cast<float>(cfg.extra);
    p.flags = static_cast<uint32_t>(cfg.debug);
    return true;
}

} // namespace

extern "C" __attribute__((force_align_arg_pointer)) void UmWorldBegin() {
    Lookup();
    g_open = false;
    if (!g_enabled) return;
    if (Read<int>(kWorld + 0x6E0) == 1 && C().indoor < 0.5) return; // caves have no sun (the game fixes a direction there): its own shadows stay
    // the game's own projected-bitmap shadows are off while the new ones work (world flag 0x200 = the shadow pass of RenderObjects)
    *reinterpret_cast<uint32_t*>(kWorld + 8) &= ~0x200u;
    g_begin();
    g_open = true;
}

extern "C" __attribute__((force_align_arg_pointer)) void UmWorldEnd() {
    if (!g_open) return;
    g_open = false;
    if (Gather(g_params)) g_end(&g_params);
    else g_end(nullptr);
}

// The stubs: begin = the call of CTerrain::RenderOpaque (thiscall, one stack argument), end = the call of CWorldClient::RenderObjects (fastcall).
asm(R"(
.text
.globl _UmWorldBeginStub
_UmWorldBeginStub:
    pushal
    pushfl
    call _UmWorldBegin
    popfl
    popal
    pushl $0x006C6230
    ret
.globl _UmWorldEndStub
_UmWorldEndStub:
    movl $0x0056AFE0, %eax
    call *%eax
    pushal
    pushfl
    call _UmWorldEnd
    popfl
    popal
    ret
)");
