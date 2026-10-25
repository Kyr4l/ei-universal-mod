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

#include "um_engine.h"
#include "um_rig.h"

#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);
extern "C" bool UmEngineConfigNumbers(const char* const* keys, double* values, int count);
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
    float guard[2] = {};
};

// um-engine.cfg numbers, re-read once a second (the file is not parsed every frame; editing it while the game runs works)
struct Cfg { double enabled = 1, margin = 8, height = 50, strength = 1, bias = 0.0015, slopeBias = 0.004, mapSize = 4096, extra = 0.3, debug = 0, indoor = 0, guard = 0.3, guardLog = 0; };
Cfg g_cfg; DWORD g_cfgAt = 0;
const Cfg& C() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        // one pass over the file (many small reads on the game's thread are a visible hitch every second under Wine); an unreadable file keeps the values
        static const char* const keys[12] = { "SunShadows::Enabled", "SunShadows::Margin", "SunShadows::Height", "SunShadows::Strength", "SunShadows::Bias", "SunShadows::SlopeBias",
            "SunShadows::MapSize", "SunShadows::Extra", "SunShadows::Debug", "SunShadows::Indoor", "SunShadows::Guard", "SunShadows::GuardLog" };
        double v[12] = { g_cfg.enabled, g_cfg.margin, g_cfg.height, g_cfg.strength, g_cfg.bias, g_cfg.slopeBias, g_cfg.mapSize, g_cfg.extra, g_cfg.debug, g_cfg.indoor, g_cfg.guard, g_cfg.guardLog };
        if (UmEngineConfigNumbers(keys, v, 12)) {
            // a hand-edited file may hold anything: every number is kept inside what the code can use
            auto clamp = [](double x, double lo, double hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; };
            g_cfg.enabled = v[0]; g_cfg.margin = clamp(v[1], 0, 64); g_cfg.height = clamp(v[2], 1, 400); g_cfg.strength = clamp(v[3], 0, 1); g_cfg.bias = clamp(v[4], 0, 0.1);
            g_cfg.slopeBias = clamp(v[5], 0, 0.1); g_cfg.mapSize = clamp(v[6], 1024, 8192); g_cfg.extra = clamp(v[7], 0, 1); g_cfg.debug = clamp(v[8], 0, 99);
            g_cfg.indoor = v[9]; g_cfg.guard = clamp(v[10], 0, 0.6); g_cfg.guardLog = v[11];
        }
    }
    return g_cfg;
}

typedef void (*BeginFn)();
typedef void (*EndFn)(const Params*);
typedef void (*CasterFn)(int);

BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
CasterFn g_caster = nullptr;
bool g_clearedFlag = false;   // the game's own shadow pass (world flag 0x200) was switched off by us
bool g_looked = false, g_enabled = false, g_open = false, g_shadowsInstalled = false;   // g_shadowsInstalled: the call sites of the sun shadows are patched
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


// The game's own shadow switches (AppSettings, the options screen's shadow buttons): master ShadowQuality, per kind UnitShadow (typeId 0x50), FloraShadow (0x52), BuildingShadow (the rest)
const uintptr_t kShadowQuality = 0x79BD0C, kUnitShadow = 0x79BD14, kBuildingShadow = 0x79BD18, kFloraShadow = 0x79BD1C;
bool GameShadowsOn() {
    return Read<int>(kShadowQuality) > 0 && (Read<int>(kUnitShadow) != 0 || Read<int>(kBuildingShadow) != 0 || Read<int>(kFloraShadow) != 0);
}

typedef int (__attribute__((thiscall)) *TypeIdFn)(void*);

} // namespace

// CObject3DClientSpecific::Draw (0x51B7E0) is wrapped: while an object whose kind the game's shadow settings switch off is drawn, its triangles are not recorded as casters.
extern "C" __attribute__((used, force_align_arg_pointer)) void UmCasterOn(void* specific) {
    if (!g_open || !g_caster || !specific) return;
    void* owner = Read<void*>(reinterpret_cast<uintptr_t>(specific) + 0x1C);
    if (!owner) return;
    void** vt = Read<void**>(reinterpret_cast<uintptr_t>(owner));
    const int type = reinterpret_cast<TypeIdFn>(vt[0x30 / 4])(owner);
    const int on = type == 0x50 ? Read<int>(kUnitShadow) : type == 0x52 ? Read<int>(kFloraShadow) : Read<int>(kBuildingShadow);
    if (on == 0) g_caster(0);
}

extern "C" __attribute__((used, force_align_arg_pointer)) void UmCasterOff() {
    if (g_open && g_caster) g_caster(1);
}

namespace { void Lookup(); bool ShadowsWanted(); }   // below

// The vertices the game batches (CRenderDevice::FlushVertexBuffer 0x4E7840 / FlushVertexBuffer2 0x4E7A40, fastcall on the device at 0x7AF080) are drawn when flushed, not when the object is
// drawn: the shifts of the guard band and the end of the shadow capture must not cut through a pending batch. Both return at once when nothing is queued.
void FlushPending() {
    void* device = Read<void*>(0x7AF080);
    if (!device) return;
    reinterpret_cast<void (__attribute__((fastcall)) *)(void*)>(0x004E7840)(device);
    reinterpret_cast<void (__attribute__((fastcall)) *)(void*)>(0x004E7A40)(device);
}

// D7VK sets the x87 to 24-bit precision for its thread (Direct3D's default): the doubles of the fit below would round like floats. Extended precision while the mod computes.
struct FpuExtended {
    unsigned short saved;
    FpuExtended() { __asm__ volatile("fnstcw %0" : "=m"(saved)); unsigned short cw = saved | 0x0300; __asm__ volatile("fldcw %0" : : "m"(cw)); }
    ~FpuExtended() { __asm__ volatile("fldcw %0" : : "m"(saved)); }
};

// The guard band. The game's CPU clipper drops every triangle outside the screen, so a tree just beyond an edge would cast no shadow. Inside CWorldClient::Render (called through the wrapper
// at 0x578910) the camera's matrices are replaced by ones for a LARGER window: the screen plus a band (padL / padT / padR / padB pixels) on the sides the sun's shadows come from, with the same
// pixel scale; the game then transforms, culls, clips and draws the band too (its coordinates are shifted by padL / padT: D7VK shifts them back and its own viewport clips the band off).
// The matrix stacks of the render globals hold (at rest, outside Render) the matrices the interface and the mouse picking use, which are put back afterwards: +0x14 / +0x1C = {V, V W, V W P},
// +0x24 / +0x3C = {W, W P}, +0x44 = {P}, +0x54 = {V} (V viewport, W the +0x4C matrix, P the projection), checked against each other before anything is replaced.
// For the larger window: V' is the viewport of the larger window, P' = (V' W)^-1 T V W P with T the shift by (padL, padT), so that V' W P' = T V W P (the same picture, shifted).
bool g_guardOn = false;
int g_padL = 0, g_padT = 0, g_padR = 0, g_padB = 0;   // the pads of the last world pass (the mouse picking compares with rectangles made during it)
int g_screenW = 0, g_screenH = 0;
bool g_applied = false;     // the replaced matrices are in the game's stacks (between UmGuardOn and UmGuardOff)
int g_swapTries = 0;

typedef void (*GuardFn)(float, float);
GuardFn g_guardFn = nullptr;

// The CPU transform's window (3DfpFPU, drawer vt[1](w, h)) is the larger one only while the world is drawn: the 2D interface uses the same drawer
void SetDrawerSize(int w, int h) {
    const uintptr_t globals = Read<uintptr_t>(0x7AF068);
    void* drawer = globals ? Read<void*>(globals + 0xC) : nullptr;
    void** vt = drawer ? Read<void**>(reinterpret_cast<uintptr_t>(drawer)) : nullptr;
    if (vt) reinterpret_cast<void (__attribute__((thiscall)) *)(void*, float, float)>(vt[1])(drawer, static_cast<float>(w), static_cast<float>(h));
}

struct StackSet { float m[6][3][16]; };
const uintptr_t kSwapStacks[6] = { 0x14, 0x1C, 0x24, 0x3C, 0x44, 0x54 };
const int kSwapLevels[6] = { 3, 3, 2, 2, 1, 1 };
StackSet g_big, g_orig;
int g_swap = 0;   // 0 not tried, 1 working, -1 not possible

float* StackLevel(int stack, int level) {
    const uintptr_t globals = Read<uintptr_t>(kRenderGlobals);
    if (!globals) return nullptr;
    const int depth = Read<int>(globals + kSwapStacks[stack] + 4);
    if (depth != kSwapLevels[stack]) return nullptr;   // exactly the rest state: Render multiplies onto the TOP level of each stack
    float* base = Read<float*>(globals + kSwapStacks[stack]);
    return base ? base + level * 16 : nullptr;
}

void MulF(const float* a, const float* b, float* o) { // o = a * b (row-major)
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float v = 0; for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = v; }
    std::memcpy(o, r, sizeof(r));
}

void BuildSet(const float* V, const float* W, const float* P, StackSet& set) {
    float VW[16], VWP[16], WP[16];
    MulF(V, W, VW); MulF(VW, P, VWP); MulF(W, P, WP);
    for (int s = 0; s < 2; ++s) { std::memcpy(set.m[s][0], V, 64); std::memcpy(set.m[s][1], VW, 64); std::memcpy(set.m[s][2], VWP, 64); }
    for (int s = 2; s < 4; ++s) { std::memcpy(set.m[s][0], W, 64); std::memcpy(set.m[s][1], WP, 64); }
    std::memcpy(set.m[4][0], P, 64); std::memcpy(set.m[5][0], V, 64);
}

bool Close(const float* a, const float* b) {
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > 1e-3f * (1.0f + std::fabs(a[i]))) return false;
    return true;
}

bool ReadSet(StackSet& set) {
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l) { const float* m = StackLevel(s, l); if (!m) return false; std::memcpy(set.m[s][l], m, 64); }
    return true;
}

void WriteSet(const StackSet& set) {
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l) { float* m = StackLevel(s, l); if (m) std::memcpy(m, set.m[s][l], 64); }
}

bool SetIsConsistent(const StackSet& set, const float* W) {
    StackSet check;
    BuildSet(set.m[5][0], W, set.m[4][0], check);
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l) if (!Close(check.m[s][l], set.m[s][l])) return false;
    return true;
}

// The sets for the larger window (pad in pixels on each side) from the original one.
bool BuildBig(const StackSet& o, int padL, int padT, int padR, int padB, StackSet& big) {
    const float* V = o.m[5][0]; const float* W = o.m[2][0]; const float* P = o.m[4][0];
    const double hw = V[0], hh = -V[5];
    float V2[16]; std::memcpy(V2, V, 64);
    const double w2 = 2 * hw + padL + padR, h2 = 2 * hh + padT + padB;
    V2[0] = static_cast<float>(w2 / 2); V2[3] = static_cast<float>(w2 / 2); V2[5] = static_cast<float>(-h2 / 2); V2[7] = static_cast<float>(h2 / 2);
    float V2W[16], VW[16], VWP[16], T[16] = { 1, 0, 0, static_cast<float>(padL), 0, 1, 0, static_cast<float>(padT), 0, 0, 1, 0, 0, 0, 0, 1 }, TVWP[16];
    MulF(V2, W, V2W); MulF(V, W, VW); MulF(VW, P, VWP); MulF(T, VWP, TVWP);
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = V2W[i];
    if (!Invert(a, inv)) return false;
    float invf[16], P2[16];
    for (int i = 0; i < 16; ++i) invf[i] = static_cast<float>(inv[i]);
    MulF(invf, TVWP, P2);
    BuildSet(V2, W, P2, big);
    return true;
}

// How far beyond each screen edge a caster can stand and still shadow the screen: the sun's shadows fall along the light's horizontal direction, about 14 units tall casters; the edge's
// ground points shifted towards the sun (their feet and tops) are projected back to the screen.
const double kCasterHeight = 9.0;   // the casters that matter at the screen's edge (trees, huts, rocks; the tallest are cut off)

void ChoosePads(const StackSet& o, double fraction, int& padL, int& padT, int& padR, int& padB) {
    const double W = 2.0 * o.m[5][0][0], H = -2.0 * o.m[5][0][5];
    const double minPadX = std::ceil(0.04 * W / 8) * 8, minPadY = std::ceil(0.04 * H / 8) * 8;
    const double maxPadX = std::ceil(fraction * W / 8) * 8, maxPadY = std::ceil(fraction * H / 8) * 8;
    double need[4] = { minPadX, minPadY, minPadX, minPadY };   // left, top, right, bottom
    double view[16], m[16], inv[16], proj[16];
    const uintptr_t sun = Read<uintptr_t>(kSunPtr);
    if (sun && StackTop(0x5C, view)) {
        for (int i = 0; i < 16; ++i) proj[i] = o.m[0][2][i];
        Mul(proj, view, m);
        if (Invert(m, inv)) {
            double dx = Read<float>(sun + 0xD4), dy = Read<float>(sun + 0xD8), dz = Read<float>(sun + 0xDC);
            const double hl = std::hypot(dx, dy);
            if (hl > 1e-6) {
                dx /= hl; dy /= hl;
                const double tanE = std::fabs(dz) / hl;
                const double L = std::fmin(std::fmax(kCasterHeight / std::fmax(tanE, 0.25), 4.0), 24.0);   // shadow length of a caster of that height
                const double m10 = proj[10], m11 = proj[11], m14 = proj[14];
                if (std::fabs(m14) > 1e-9) {
                    const double K = -m14 * m11, gz = Read<float>(kCamera + 0xE8);
                    const double sxs[8] = { 0, W, 0, W, W / 2, W / 2, 0, W }, sys[8] = { 0, 0, H, H, 0, H, H / 2, H / 2 };
                    for (int i = 0; i < 8; ++i) {
                        // the edge point on the ground plane at the target's height
                        double a0[3], b0[3];
                        const double z0 = 0.05, z1 = 0.999;
                        const double w0 = K / (m10 - z0 * m14), w1 = K / (m10 - z1 * m14);
                        Transform(inv, sxs[i] * w0, sys[i] * w0, z0 * w0, w0, a0);
                        Transform(inv, sxs[i] * w1, sys[i] * w1, z1 * w1, w1, b0);
                        const double dzr = b0[2] - a0[2];
                        if (std::fabs(dzr) < 1e-9) continue;
                        double t = (gz - a0[2]) / dzr; if (t < 0 || t > 1) continue;
                        const double gx = a0[0] + (b0[0] - a0[0]) * t, gy = a0[1] + (b0[1] - a0[1]) * t;
                        for (int k = 0; k < 2; ++k) {   // the caster's foot and top, towards the sun (opposite to the light's travel)
                            double q[4] = { gx - dx * L, gy - dy * L, gz + (k ? kCasterHeight : 0.0), 1.0 }, r[4];
                            for (int row = 0; row < 4; ++row) r[row] = m[row * 4] * q[0] + m[row * 4 + 1] * q[1] + m[row * 4 + 2] * q[2] + m[row * 4 + 3] * q[3];
                            if (r[3] < 1e-6) continue;
                            const double px = r[0] / r[3], py = r[1] / r[3];
                            need[0] = std::fmax(need[0], -px + 8); need[2] = std::fmax(need[2], px - W + 8);
                            need[1] = std::fmax(need[1], -py + 8); need[3] = std::fmax(need[3], py - H + 8);
                        }
                    }
                }
            }
        }
    }
    static double kept[4] = { 0, 0, 0, 0 };
    int out[4];
    for (int i = 0; i < 4; ++i) {
        const double cap = (i & 1) ? maxPadY : maxPadX;
        double v = std::fmin(need[i], cap);
        // grows at once, shrinks slowly (the shadows do not change with the pads, but the game's matrices do)
        kept[i] = v > kept[i] ? v : std::fmax(v, kept[i] * 0.98);
        out[i] = static_cast<int>(std::ceil(kept[i] / 8.0) * 8.0);
    }
    padL = out[0]; padT = out[1]; padR = out[2]; padB = out[3];
}

// One-time part: D7VK's guard export. The stacks themselves are checked on every frame (a cutscene camera, a resolution change or a 3D interface screen may leave others).
void PrepareSwap() {
    g_swap = -1;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    g_guardFn = module ? reinterpret_cast<GuardFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowGuard"))) : nullptr;
    if (!g_guardFn) { UmEngineLog("um-engine: SunShadows::Guard needs d7vk.dll: not applied"); return; }
    g_swap = 1;
    UmEngineLog("um-engine: guard band ready: the world is drawn with a band on the sides the sun's shadows come from (at most %.0f %% of the screen)", C().guard * 100);
}

extern "C" __attribute__((used, force_align_arg_pointer)) void UmGuardOn() {
    FpuExtended fpu;
    if (g_applied) {   // a world pass that did not come back through UmGuardOff (an exception unwound through it): put the original matrices back first
        g_applied = false;
        g_guardFn(0.0f, 0.0f); SetDrawerSize(g_screenW, g_screenH); WriteSet(g_orig);
    }
    g_padL = g_padT = g_padR = g_padB = 0;
    g_umFrame.guard = 0;
    if (g_swap == 0) PrepareSwap();
    if (g_swap != 1 || C().guard <= 0.0 || !ShadowsWanted()) return;
    StackSet now;
    static unsigned bad = 0;
    if (!ReadSet(now) || !SetIsConsistent(now, now.m[2][0])) {   // not the rest state of the game's own set-up: this frame is drawn as the game does
        g_umFrame.guard = 2;
        if (C().guardLog > 0.5 && (bad++ & 255) == 0) UmEngineLog("um-engine: guard band: the matrix stacks are not as expected in this frame (%u so far): no band", bad);
        return;
    }
    g_orig = now;
    g_screenW = static_cast<int>(std::lround(2.0 * g_orig.m[5][0][0])); g_screenH = static_cast<int>(std::lround(-2.0 * g_orig.m[5][0][5]));
    if (g_screenW < 320 || g_screenH < 240) return;
    int l, t, r, b;
    ChoosePads(g_orig, std::fmin(std::fmax(C().guard, 0.0), 0.6), l, t, r, b);
    if (!BuildBig(g_orig, l, t, r, b, g_big)) return;
    g_padL = l; g_padT = t; g_padR = r; g_padB = b;
    g_umFrame.guard = 1; g_umFrame.pad[0] = static_cast<int16_t>(l); g_umFrame.pad[1] = static_cast<int16_t>(t); g_umFrame.pad[2] = static_cast<int16_t>(r); g_umFrame.pad[3] = static_cast<int16_t>(b);
    { static int lp[4] = {-1,-1,-1,-1}; if (C().guardLog > 1.5 && (lp[0] != g_padL || lp[1] != g_padT || lp[2] != g_padR || lp[3] != g_padB)) { lp[0] = g_padL; lp[1] = g_padT; lp[2] = g_padR; lp[3] = g_padB; UmEngineLog("um-engine: dbg pads L %d T %d R %d B %d (screen %d x %d)", g_padL, g_padT, g_padR, g_padB, g_screenW, g_screenH); } }
    FlushPending();   // what the interface left queued is drawn without the shift
    WriteSet(g_big);
    SetDrawerSize(g_screenW + g_padL + g_padR, g_screenH + g_padT + g_padB);
    g_guardFn(static_cast<float>(g_padL), static_cast<float>(g_padT));
    g_applied = true;
}

extern "C" __attribute__((used, force_align_arg_pointer)) void UmGuardOff() {
    if (!g_applied) return;
    g_applied = false;
    FlushPending();   // the world's last batch is drawn with the shift
    g_guardFn(0.0f, 0.0f);
    SetDrawerSize(g_screenW, g_screenH);
    WriteSet(g_orig);
}

// The mouse picking of objects compares the mouse with the screen rectangles made during the last world pass (PickObjectsAt 0x565790 / PickObjectsAt2 0x5659D0: a POINT*): they carry the
// shift of that pass. (The terrain picking inverts the original matrices at rest and needs nothing.)
extern "C" __attribute__((used, force_align_arg_pointer)) void UmPickObjectsFix(int* args) {
    if (g_padL == 0 && g_padT == 0) return;
    static long ring[4][2]; static int next = 0;
    const long* pt = reinterpret_cast<const long*>(args[0]);
    if (!pt) return;
    long* copy = ring[next]; next = (next + 1) & 3;
    copy[0] = pt[0] + g_padL; copy[1] = pt[1] + g_padT;
    args[0] = reinterpret_cast<int>(copy);
}

// The selection frame (CWorldClient::CollectVisibleRecordObjects 0x5656A0, thiscall(this, RECT*, list*), called from the group selection of the UI screens) is intersected with the same
// rectangles: its RECT gets the shift too.
extern "C" __attribute__((used, force_align_arg_pointer)) void UmRectFix(int* args) {
    if (g_padL == 0 && g_padT == 0) return;
    static RECT copy;
    const RECT* r = reinterpret_cast<const RECT*>(args[0]);
    if (!r) return;
    copy.left = r->left + g_padL; copy.right = r->right + g_padL; copy.top = r->top + g_padT; copy.bottom = r->bottom + g_padT;
    args[0] = reinterpret_cast<int>(&copy);
}

extern "C" void UmPickObjectsHook();
extern "C" void UmCollectHook();
extern "C" void UmPickObjects2Hook();

extern "C" void UmDrawHook();
extern "C" void UmDrawTrampoline();

namespace {
void InstallDrawHook() {
    static const unsigned char kExpect[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    unsigned char* at = reinterpret_cast<unsigned char*>(0x0051B7E0);
    if (std::memcmp(at, kExpect, 5) != 0) { UmEngineLog("um-engine: sun shadows: CObject3DClientSpecific::Draw has other code (another build): per-kind switches not applied"); return; }
    UmPatchJump(0x0051B7E0, 0xE9, reinterpret_cast<void*>(&UmDrawHook));
}

// the first bytes of a function (whole instructions, no relative operands) are replaced by a jump, the rest of them by NOPs; the hook runs them again
bool HookPrologue(uintptr_t address, void* hook, const unsigned char* expect, int length) {
    unsigned char* at = reinterpret_cast<unsigned char*>(address);
    if (length < 5 || std::memcmp(at, expect, length) != 0) { UmEngineLog("um-engine: sun shadows: 0x%X has other code (another build): no guard band", static_cast<unsigned>(address)); return false; }
    // the jump over the first five bytes (one locked write), then the rest of the replaced instructions as NOPs (they are not reached any more)
    if (!UmPatchJump(address, 0xE9, hook)) return false;
    if (length > 5) { static const unsigned char nops[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; UmPatchCode(at + 5, nops, static_cast<size_t>(length - 5)); }
    return true;
}

extern "C" void UmRenderWrap();

void InstallSetupHook() {
    // the wrapper of CWorldClient::Render: mov ecx, 0x7AF5B8 / jmp 0x569E40 -> the jump goes through UmRenderWrap
    static const unsigned char kJump[5] = { 0xE9, 0x26, 0x15, 0xFF, 0xFF };
    unsigned char* wrap = reinterpret_cast<unsigned char*>(0x00578915);
    if (std::memcmp(wrap, kJump, 5) != 0) { UmEngineLog("um-engine: sun shadows: the world render wrapper has other code (another build): no guard band"); return; }
    static const unsigned char kPick[5] = { 0x83, 0xEC, 0x1C, 0x53, 0x55 };
    static const unsigned char kCollect[10] = { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0xF0, 0x00, 0x00, 0x00 };
    // all or none: every site is checked before the first is patched
    if (std::memcmp(reinterpret_cast<void*>(0x00565790), kPick, 5) || std::memcmp(reinterpret_cast<void*>(0x005659D0), kPick, 5) || std::memcmp(reinterpret_cast<void*>(0x005656A0), kCollect, 10)) {
        UmEngineLog("um-engine: sun shadows: the picking functions have other code (another build): no guard band"); return;
    }
    HookPrologue(0x00565790, reinterpret_cast<void*>(&UmPickObjectsHook), kPick, 5);
    HookPrologue(0x005659D0, reinterpret_cast<void*>(&UmPickObjects2Hook), kPick, 5);
    HookPrologue(0x005656A0, reinterpret_cast<void*>(&UmCollectHook), kCollect, 10);
    UmPatchJump(0x00578915, 0xE9, reinterpret_cast<void*>(&UmRenderWrap));
}

void Lookup() {
    if (g_looked) return;
    g_looked = true;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    if (!module) return;
    g_begin = reinterpret_cast<BeginFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowBegin")));
    g_end = reinterpret_cast<EndFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowEnd")));
    g_caster = reinterpret_cast<CasterFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowCaster")));
    g_enabled = g_begin && g_end;
    if (g_enabled && g_caster) InstallDrawHook();
    UmEngineLog("um-engine: sun shadows %s (UmShadowBegin / UmShadowEnd %sfound in the DirectDraw library)", g_enabled ? "on" : "off", g_enabled ? "" : "not ");
}

// The sun shadows (and the guard band) work: D7VK has them, Enabled is on, the game's own shadow settings allow them, no cave (the game gives it a fixed direction instead of a sun).
bool ShadowsWanted() {
    if (!g_shadowsInstalled) return false;
    Lookup();
    if (!g_enabled) return false;
    const bool indoor = Read<int>(kWorld + 0x6E0) == 1 && C().indoor < 0.5;
    return C().enabled >= 0.5 && GameShadowsOn() && !indoor;
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
    // with the guard band the screen is the middle of the (larger) window the game draws: its coordinates are shifted by the band's width
    const double gpx = g_padL, gpy = g_padT;
    const double width = 2 * hw - g_padL - g_padR, height = 2 * hh - g_padT - g_padB;
    p.guard[0] = static_cast<float>(gpx); p.guard[1] = static_cast<float>(gpy);
    const double groundZ = Read<float>(kCamera + 0xE8);
    // The shadowed area, fitted in the sun's own axes (the map is a square in R / U around its centre, light depth runs along D): the visible ground is found by sending nine rays through the
    // screen (corners, edge middles, centre) down to the game's own terrain (CTerrain::GetHeightAt), the nearest hit within 70 units of the camera; their bounds in R / U / D give the
    // map's centre and half size, the depth range reaches 40 units upstream (casters standing on the area) and 6 downstream. Without a terrain the rays meet three planes instead.
    const Cfg& cfg = C();
    double D[3] = { p.lightDir[0], p.lightDir[1], p.lightDir[2] };
    const double dlen = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
    if (dlen < 1e-6) return false;
    for (double& v : D) v /= dlen;
    double upv[3] = { 0, 0, 1 };
    if (std::fabs(D[2]) > 0.99) { upv[1] = 1; upv[2] = 0; }
    auto cross3 = [](const double* x, const double* y, double* o) { o[0] = x[1] * y[2] - x[2] * y[1]; o[1] = x[2] * y[0] - x[0] * y[2]; o[2] = x[0] * y[1] - x[1] * y[0]; };
    auto norm3 = [](double* v) { const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9) { v[0] /= l; v[1] /= l; v[2] /= l; } };
    double Rv[3], Uv[3];
    cross3(D, upv, Rv); norm3(Rv); cross3(Rv, D, Uv); norm3(Uv);
    auto dot3 = [](const double* x, const double* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };

    void* terrain = Read<void*>(kWorld + 0x180);
    if (terrain && Read<int>(reinterpret_cast<uintptr_t>(terrain) + 0x100) == 0) terrain = nullptr;   // no sector data (the game's own RenderObjects needs it too)
    typedef float (__attribute__((thiscall)) *HeightFn)(void*, float, float, int);
    const HeightFn heightAt = reinterpret_cast<HeightFn>(0x006C8680);
    typedef float* (__attribute__((thiscall)) *VertexFn)(void*, int, int, int);   // CTerrain::GetVertex 0x6C85A0: the vertex of a cell, 0 outside the map
    const VertexFn vertexAt = reinterpret_cast<VertexFn>(0x006C85A0);
    const double camX = p.cameraPos[0], camY = p.cameraPos[1];
    const double sx[9] = { gpx, gpx + width, gpx, gpx + width, gpx + 0.5 * width, gpx + 0.5 * width, gpx, gpx + width, gpx + 0.5 * width };
    const double sy[9] = { gpy, gpy, gpy + height, gpy + height, gpy, gpy + height, gpy + 0.5 * height, gpy + 0.5 * height, gpy + 0.5 * height };
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
    auto add = [&](double x, double y, double z) {
        const double pt[3] = { x, y, z };
        const double c[3] = { dot3(Rv, pt), dot3(Uv, pt), dot3(D, pt) };
        for (int k = 0; k < 3; ++k) { lo[k] = std::fmin(lo[k], c[k]); hi[k] = std::fmax(hi[k], c[k]); }
    };
    for (int i = 0; i < 9; ++i) {
        const double z0 = 0.05, z1 = 0.999;
        const double w0 = p.depthK / (p.depthFar - z0 * (p.depthFar - p.depthNear)), w1 = p.depthK / (p.depthFar - z1 * (p.depthFar - p.depthNear));
        double a0[3], b0[3];
        Transform(inv, sx[i] * w0, sy[i] * w0, z0 * w0, w0, a0);
        Transform(inv, sx[i] * w1, sy[i] * w1, z1 * w1, w1, b0);
        auto at = [&](double t, double* o) { for (int k = 0; k < 3; ++k) o[k] = a0[k] + (b0[k] - a0[k]) * t; };
        double hit[3]; bool found = false;
        if (terrain) {
            // march until the ray is under the ground (96 steps), then bisect; stop at 70 units from the camera
            double t0 = 0;
            for (int s = 1; s <= 96 && !found; ++s) {
                const double t1 = double(s) / 96.0; double q[3]; at(t1, q);
                if ((q[0] - camX) * (q[0] - camX) + (q[1] - camY) * (q[1] - camY) > 4900.0) { hit[0] = q[0]; hit[1] = q[1]; hit[2] = q[2]; found = true; break; }
                if (q[2] <= heightAt(terrain, static_cast<float>(q[0]), static_cast<float>(q[1]), 0)) {
                    double ta = t0, tb = t1;
                    for (int it = 0; it < 12; ++it) { const double tm = 0.5 * (ta + tb); double m[3]; at(tm, m); if (m[2] <= heightAt(terrain, static_cast<float>(m[0]), static_cast<float>(m[1]), 0)) tb = tm; else ta = tm; }
                    at(tb, hit); found = true;
                }
                t0 = t1;
            }
            if (!found) at(1.0, hit);
            // outside the map the terrain answers 0: a hit there is dropped (a hit AT height 0 inside the map is fine: asked with the vertex, not the height)
            if (found && !vertexAt(terrain, static_cast<int>(std::lround(hit[0])), static_cast<int>(std::lround(hit[1])), 0)) found = false;
            if (!found) { at(1.0, hit); }
            add(hit[0], hit[1], hit[2]);
        } else {
            const double planeZ[3] = { groundZ + 8.0, groundZ, groundZ - 16.0 };
            for (double pz : planeZ) {
                const double dz = b0[2] - a0[2]; double t = std::fabs(dz) > 1e-9 ? (pz - a0[2]) / dz : 1.0; t = std::fmin(std::fmax(t, 0.0), 1.0);
                double q[3]; at(t, q); add(q[0], q[1], q[2]);
            }
        }
    }
    // the camera's own ground point and the target (the shadows around the hero) always belong to the area
    add(camX, camY, groundZ);
    const double ext = 0.5 * std::fmax(hi[0] - lo[0], hi[1] - lo[1]);
    const double half = ext * 1.08 + cfg.margin;
    const double mid[3] = { 0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2]) };
    const double dLow = lo[2] - 40.0, dHigh = hi[2] + 6.0;
    const double midD = 0.5 * (dLow + dHigh), hD = 0.5 * (dHigh - dLow);
    for (int k = 0; k < 3; ++k) p.center[k] = static_cast<float>(Rv[k] * mid[0] + Uv[k] * mid[1] + D[k] * midD);
    {   // The radius keeps its value until the area needs more or is much smaller (steps of 8 units, 30 % slack): a radius that follows the camera changes the texel size and the shadows shimmer
        static double kept = 0;
        const double need = std::fmin(std::fmax(std::ceil(half / 8.0) * 8.0, 16.0), 120.0);
        if (need > kept || need < kept * 0.7) kept = need;
        p.radius = static_cast<float>(kept);
    }
    {
        const double h = std::fmax(hD, 20.0);
        p.height = static_cast<float>(h);
        p.bias = static_cast<float>(cfg.bias * 100.0 / (2.0 * h));
        p.slopeBias = static_cast<float>(cfg.slopeBias * 100.0 / (2.0 * h));
    }
    {   // the map's size follows the area: about 0.04 world units a texel (a power of two between 1024 and SunShadows::MapSize)
        uint32_t size = 1024;
        while (size < 8192 && 2.0 * p.radius / size > 0.04) size *= 2;
        p.mapSize = std::min<uint32_t>(size, std::max<uint32_t>(1024u, static_cast<uint32_t>(cfg.mapSize)));
    }
    { static float lr = 0, lh = 0; static DWORD lt = 0;
      if (cfg.guardLog > 1.5 && (lr != p.radius || std::fabs(lh - p.height) > 3.0f)) { const DWORD n2 = GetTickCount(); UmEngineLog("um-engine: dbg +%lu ms area half %.1f -> radius %.0f, depth half %.1f, map %u, terrain %s", static_cast<unsigned long>(lt ? n2 - lt : 0), half, p.radius, p.height, p.mapSize, terrain ? "yes" : "no"); lt = n2; lr = p.radius; lh = p.height; } }
    p.strength = static_cast<float>(cfg.strength);
    p.extra = static_cast<float>(cfg.extra);
    p.flags = static_cast<uint32_t>(cfg.debug);
    return true;
}

} // namespace

// The camera / projection of the world pass as the water border needs it (the same matrices Gather reads: valid between the guard band's set-up and its end, i.e. inside CWorldClient::Render):
// screenToWorld column-major (see DxvkUmShadowParams), depth = { K, far, near }, guard = the band's shift in pixels. false when the stacks cannot be read.
extern "C" bool UmViewParams(float* screenToWorld, float* depth, float* guard) {
    FpuExtended fpu;
    double view[16], proj[16], m[16], inv[16];
    if (!StackTop(0x14, proj) || !StackTop(0x5C, view)) return false;
    Mul(proj, view, m);
    if (!Invert(m, inv)) return false;
    const double m10 = proj[10], m11 = proj[11], m14 = proj[14];
    if (std::fabs(m14) < 1e-9) return false;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) screenToWorld[c * 4 + r] = static_cast<float>(inv[r * 4 + c]);
    depth[0] = static_cast<float>(-m14 * m11); depth[1] = static_cast<float>(m10); depth[2] = static_cast<float>(m10 - m14);
    guard[0] = static_cast<float>(g_padL); guard[1] = static_cast<float>(g_padT);
    return true;
}

// Called once from um_engine.cpp when the call sites are installed (the camera is set up before the first world frame)
extern "C" void UmWorldEarlyInit(int shadowsInstalled) {
    g_shadowsInstalled = shadowsInstalled != 0;
    if (C().guard > 0.0 || C().guardLog > 0.5 || UmEngineConfigNumber("Recorder::Enabled", 1) >= 0.5) InstallSetupHook();
}

static unsigned g_dbgFrames = 0, g_dbgClosed = 0, g_dbgGatherFail = 0; static DWORD g_dbgAt = 0;
extern "C" __attribute__((used, force_align_arg_pointer)) void UmWorldBegin() {
    Lookup();
    g_open = false;
    ++g_dbgFrames;
    { const DWORD n = GetTickCount(); if (!g_dbgAt) g_dbgAt = n; if (n - g_dbgAt > 5000) { g_dbgAt = n; if (C().guardLog > 1.5) UmEngineLog("um-engine: dbg frames %u closed %u gatherfail %u", g_dbgFrames, g_dbgClosed, g_dbgGatherFail); } }
    if (!g_enabled) return;
    // Off: SunShadows::Enabled=0 (checked once a second), the game's own shadow switches all off (the options screen), a cave. The game's own pass is given back then.
    if (!ShadowsWanted()) {
        ++g_dbgClosed;
        g_umFrame.mode = kModeOff;
        if (g_clearedFlag && Read<int>(kShadowQuality) > 0) *reinterpret_cast<uint32_t*>(kWorld + 8) |= 0x200u;
        g_clearedFlag = false;
        return;
    }
    // the game's own projected-bitmap shadows are off while the new ones work (world flag 0x200 = the shadow pass of RenderObjects)
    *reinterpret_cast<uint32_t*>(kWorld + 8) &= ~0x200u;
    g_clearedFlag = true;
    g_begin();
    g_open = true;
}

extern "C" __attribute__((used, force_align_arg_pointer)) void UmWorldEnd() {
    if (!g_open) return;
    g_open = false;
    FpuExtended fpu;
    FlushPending();   // the last batch of RenderObjects belongs to the capture and to the depth the resolve reads
    // a frame whose matrices cannot be read keeps the last good ones (no frame without shadows)
    static Params last; static bool haveLast = false;
    Params now;
    if (Gather(now)) { last = now; haveLast = true; g_end(&last); g_umFrame.mode = kModeDrawn; }
    else { ++g_dbgGatherFail; if (haveLast) { g_end(&last); g_umFrame.mode = kModeKept; } else { g_end(nullptr); g_umFrame.mode = kModeNone; } }
    if (haveLast) {
        UmFrameRec& r = g_umFrame;
        for (int i = 0; i < 3; ++i) { r.center[i] = last.center[i]; r.light[i] = last.lightDir[i]; }
        r.radius = last.radius; r.height = last.height; r.mapSize = last.mapSize;
    }
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
asm(R"(
.text
.globl _UmDrawHook
_UmDrawHook:
    pushl %ecx
    pushl %ecx
    call _UmCasterOn
    addl $4, %esp
    popl %ecx
    pushl 8(%esp)
    pushl 8(%esp)
    call _UmDrawTrampoline
    pushl %eax
    call _UmCasterOff
    popl %eax
    ret $8
.globl _UmDrawTrampoline
_UmDrawTrampoline:
    pushl %ebp
    movl %esp, %ebp
    pushl $-1
    pushl $0x0051B7E5
    ret
)");
asm(R"(
.text
)");
asm(R"(
.text
.globl _UmRenderWrap
_UmRenderWrap:
    pushl %ecx
    call _UmRigFrameBegin
    call _UmGuardOn
    popl %ecx
    movl $0x00569E40, %eax
    call *%eax
    pushl %eax
    call _UmGuardOff
    call _UmRigFrameEnd
    popl %eax
    ret
)");
asm(R"(
.text
.globl _UmPickObjectsHook
_UmPickObjectsHook:
    pushl %ecx
    leal 8(%esp), %eax
    pushl %eax
    call _UmPickObjectsFix
    addl $4, %esp
    popl %ecx
    subl $0x1C, %esp
    pushl %ebx
    pushl %ebp
    pushl $0x00565795
    ret
.globl _UmPickObjects2Hook
_UmPickObjects2Hook:
    pushl %ecx
    leal 8(%esp), %eax
    pushl %eax
    call _UmPickObjectsFix
    addl $4, %esp
    popl %ecx
    subl $0x1C, %esp
    pushl %ebx
    pushl %ebp
    pushl $0x005659D5
    ret
)");
asm(R"(
.text
.globl _UmCollectHook
_UmCollectHook:
    pushl %ecx
    leal 8(%esp), %eax
    pushl %eax
    call _UmRectFix
    addl $4, %esp
    popl %ecx
    pushl %esi
    pushl %edi
    movl %ecx, %edi
    movl 0xF0(%edi), %eax
    pushl $0x005656AA
    ret
)");
