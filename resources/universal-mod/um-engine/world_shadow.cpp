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
    float guard[2] = {};
};

// um-engine.cfg numbers, re-read once a second (the file is not parsed every frame; editing it while the game runs works)
struct Cfg { double enabled = 1, margin = 8, height = 50, strength = 1, bias = 0.0015, slopeBias = 0.004, mapSize = 2048, extra = 0.3, debug = 0, indoor = 0, guard = 0.15, guardLog = 0; };
Cfg g_cfg; DWORD g_cfgAt = 0;
const Cfg& C() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        g_cfg.enabled = UmEngineConfigNumber("SunShadows::Enabled", 1.0);
        g_cfg.margin = UmEngineConfigNumber("SunShadows::Margin", 8.0);
        g_cfg.height = UmEngineConfigNumber("SunShadows::Height", 50.0);
        g_cfg.strength = UmEngineConfigNumber("SunShadows::Strength", 1.0);
        g_cfg.bias = UmEngineConfigNumber("SunShadows::Bias", 0.0015);
        g_cfg.slopeBias = UmEngineConfigNumber("SunShadows::SlopeBias", 0.004);
        g_cfg.mapSize = UmEngineConfigNumber("SunShadows::MapSize", 2048.0);
        g_cfg.extra = UmEngineConfigNumber("SunShadows::Extra", 0.3);
        g_cfg.debug = UmEngineConfigNumber("SunShadows::Debug", 0.0);
        g_cfg.indoor = UmEngineConfigNumber("SunShadows::Indoor", 0.0);
        g_cfg.guard = UmEngineConfigNumber("SunShadows::Guard", 0.15);
        g_cfg.guardLog = UmEngineConfigNumber("SunShadows::GuardLog", 0.0);
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


// The game's own shadow switches (AppSettings, the options screen's shadow buttons): master ShadowQuality, per kind UnitShadow (typeId 0x50), FloraShadow (0x52), BuildingShadow (the rest)
const uintptr_t kShadowQuality = 0x79BD0C, kUnitShadow = 0x79BD14, kBuildingShadow = 0x79BD18, kFloraShadow = 0x79BD1C;
bool GameShadowsOn() {
    return Read<int>(kShadowQuality) > 0 && (Read<int>(kUnitShadow) != 0 || Read<int>(kBuildingShadow) != 0 || Read<int>(kFloraShadow) != 0);
}

typedef int (__attribute__((thiscall)) *TypeIdFn)(void*);

} // namespace

// CObject3DClientSpecific::Draw (0x51B7E0) is wrapped: while an object whose kind the game's shadow settings switch off is drawn, its triangles are not recorded as casters.
extern "C" __attribute__((force_align_arg_pointer)) void UmCasterOn(void* specific) {
    if (!g_open || !g_caster || !specific) return;
    void* owner = Read<void*>(reinterpret_cast<uintptr_t>(specific) + 0x1C);
    if (!owner) return;
    void** vt = Read<void**>(reinterpret_cast<uintptr_t>(owner));
    const int type = reinterpret_cast<TypeIdFn>(vt[0x30 / 4])(owner);
    const int on = type == 0x50 ? Read<int>(kUnitShadow) : type == 0x52 ? Read<int>(kFloraShadow) : Read<int>(kBuildingShadow);
    if (on == 0) g_caster(0);
}

extern "C" __attribute__((force_align_arg_pointer)) void UmCasterOff() {
    if (g_open && g_caster) g_caster(1);
}

// The guard band: the camera's matrices (built once, by I3DFigureImpl::InitRenderer: Mat4_Perspective 0x4F0E80 (out, fov, aspect, near, far) and Mat4_Viewport 0x4F1370 (out, rect*), both
// stdcall) and the CPU transform's window (3DfpFPU, vt[1](w, h)) are made (1 + 2 f) times larger with the same pixel scale: the game then transforms, clips and draws a band of f x the screen's size
// around the screen too (its coordinates are shifted by the band's width, D7VK shifts them back and its viewport clips the band off). Objects just off screen cast their shadows.
bool g_guardOn = false;
int g_guardPx = 0, g_guardPy = 0, g_screenW = 0, g_screenH = 0;
int g_rectBig[4] = {};
int g_rectOrig[4] = {};
float g_origPersp[4] = {};   // fov, aspect, near, far as the game asked for them
bool g_bypass = false;       // our own calls of Mat4_Viewport / Mat4_Perspective (building the original matrices) are not changed

typedef void (*GuardFn)(float, float);
GuardFn g_guardFn = nullptr;

const uintptr_t kDeviceRect = 0x24;   // CRenderDevice: the viewport rect (x0, y0, x1, y1) the game was started with

extern "C" __attribute__((force_align_arg_pointer)) void UmPerspectiveFix(int* args) {
    const double f = C().guard;
    if (f <= 0.0) return;
    static int logged = 0;
    float v[4]; std::memcpy(v, &args[1], 16);
    if (C().guardLog > 0.5 && logged < 20) { ++logged; UmEngineLog("um-engine: Mat4_Perspective from 0x%X fov %f aspect %f near %f far %f", static_cast<unsigned>(args[-1]), v[0], v[1], v[2], v[3]); }
    const uintptr_t device = Read<uintptr_t>(0x7AF060 + 0x20);
    const unsigned caller = static_cast<unsigned>(args[-1]);
    if (!device || caller < 0x500B70 || caller > 0x501400) return;   // only the renderer's start-up (this also keeps SetupCamera's calls, if the game makes them, untouched)
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    g_guardFn = module ? reinterpret_cast<GuardFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowGuard"))) : nullptr;
    if (!g_guardFn) { UmEngineLog("um-engine: SunShadows::Guard needs d7vk.dll: not applied"); return; }
    const int* rect = reinterpret_cast<const int*>(device + kDeviceRect);
    const int w = rect[2] - rect[0], h = rect[3] - rect[1];
    std::memcpy(g_origPersp, v, 16); std::memcpy(g_rectOrig, rect, 16);
    if (w < 320 || h < 240) return;
    const double fc = std::fmin(std::fmax(f, 0.0), 0.5);
    g_guardPx = static_cast<int>(std::lround(fc * w)); g_guardPy = static_cast<int>(std::lround(fc * h));
    g_screenW = w; g_screenH = h;
    const double gx = double(w + 2 * g_guardPx) / w, gy = double(h + 2 * g_guardPy) / h;
    // the same pixels per unit as before: tan(fov' / 2) = gx tan(fov / 2); the vertical scale is kept by the aspect
    const float fov2 = static_cast<float>(2.0 * std::atan(gx * std::tan(0.5 * v[0])));
    const float aspect2 = static_cast<float>(v[1] * gx / gy);
    std::memcpy(&args[1], &fov2, 4); std::memcpy(&args[2], &aspect2, 4);
    g_rectBig[0] = rect[0]; g_rectBig[1] = rect[1]; g_rectBig[2] = rect[0] + w + 2 * g_guardPx; g_rectBig[3] = rect[1] + h + 2 * g_guardPy;
    g_guardOn = true;
    UmEngineLog("um-engine: guard band %d x %d pixels around the %d x %d screen", g_guardPx, g_guardPy, w, h);
}

extern "C" __attribute__((force_align_arg_pointer)) void UmViewportFix(int* args) {
    if (!g_guardOn || g_bypass) return;
    const int* rect = reinterpret_cast<const int*>(args[1]);
    if (rect[2] - rect[0] == g_screenW && rect[3] - rect[1] == g_screenH) args[1] = reinterpret_cast<int>(g_rectBig);
}

// CWorldClient::Render (through the wrapper at 0x578910) is bracketed: the game's vertices are shifted by the band only inside it
// The CPU transform's window (3DfpFPU, drawer vt[1](w, h)) is the enlarged one only while the world is drawn: the 2D interface uses the same drawer
void SetDrawerSize(int w, int h) {
    const uintptr_t globals = Read<uintptr_t>(0x7AF068);
    void* drawer = globals ? Read<void*>(globals + 0xC) : nullptr;
    void** vt = drawer ? Read<void**>(reinterpret_cast<uintptr_t>(drawer)) : nullptr;
    if (vt) reinterpret_cast<void (__attribute__((thiscall)) *)(void*, float, float)>(vt[1])(drawer, static_cast<float>(w), static_cast<float>(h));
}

// The matrix stacks of the render globals hold (at rest, outside CWorldClient::Render) the matrices the interface and the mouse picking use. The guard band's (larger) ones are
// built at start-up (the hooks above); inside Render the stacks hold those, outside the original ones, computed here the way the game does: +0x14 / +0x1c = {V, V W, V W P},
// +0x24 / +0x3c = {W, W P}, +0x44 = {P}, +0x54 = {V} (V viewport, W the stack +0x4c's matrix, P the projection).
struct StackSet { float m[6][3][16]; };
const uintptr_t kSwapStacks[6] = { 0x14, 0x1C, 0x24, 0x3C, 0x44, 0x54 };
const int kSwapLevels[6] = { 3, 3, 2, 2, 1, 1 };
StackSet g_big, g_orig;
int g_swap = 0;   // 0 not tried, 1 working, -1 not possible

float* StackLevel(int stack, int level) {
    const uintptr_t globals = Read<uintptr_t>(kRenderGlobals);
    if (!globals) return nullptr;
    const int depth = Read<int>(globals + kSwapStacks[stack] + 4);
    if (depth < kSwapLevels[stack]) return nullptr;
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

void ReadSet(StackSet& set) {
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l) { const float* m = StackLevel(s, l); if (m) std::memcpy(set.m[s][l], m, 64); }
}

void WriteSet(const StackSet& set) {
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l) { float* m = StackLevel(s, l); if (m) std::memcpy(m, set.m[s][l], 64); }
}

void PrepareSwap() {
    g_swap = -1;
    for (int s = 0; s < 6; ++s) if (!StackLevel(s, 0)) { UmEngineLog("um-engine: guard band: the matrix stacks are not as expected: off"); return; }
    ReadSet(g_big);
    const float* W = StackLevel(2, 0);   // the stack +0x4C's matrix is also the first of +0x24 / +0x3C
    float Vo[16], Po[16];
    g_bypass = true;
    reinterpret_cast<void (__attribute__((stdcall)) *)(float*, int*)>(0x004F1370)(Vo, g_rectOrig);
    reinterpret_cast<void (__attribute__((stdcall)) *)(float*, float, float, float, float)>(0x004F0E80)(Po, g_origPersp[0], g_origPersp[1], g_origPersp[2], g_origPersp[3]);
    g_bypass = false;
    // the formulas must reproduce what the game built for the larger ones (from its P and V)
    StackSet check;
    BuildSet(g_big.m[5][0], W, g_big.m[4][0], check);
    for (int s = 0; s < 6; ++s) for (int l = 0; l < kSwapLevels[s]; ++l)
        if (!Close(check.m[s][l], g_big.m[s][l])) { UmEngineLog("um-engine: guard band: the matrix stacks (%d, level %d) are not what was expected: off", s, l); return; }
    BuildSet(Vo, W, Po, g_orig);
    g_swap = 1;
    UmEngineLog("um-engine: guard band: matrix stacks swapped around the world pass");
}

extern "C" __attribute__((force_align_arg_pointer)) void UmGuardOn() {
    if (!g_guardOn || !g_guardFn) return;
    if (g_swap == 0) PrepareSwap();
    if (g_swap == 1) WriteSet(g_big);
    SetDrawerSize(g_rectBig[2] - g_rectBig[0], g_rectBig[3] - g_rectBig[1]);
    g_guardFn(static_cast<float>(g_guardPx), static_cast<float>(g_guardPy));
}

extern "C" __attribute__((force_align_arg_pointer)) void UmGuardOff() {
    if (!g_guardOn || !g_guardFn) return;
    g_guardFn(0.0f, 0.0f);
    SetDrawerSize(g_screenW, g_screenH);
    if (g_swap == 1) WriteSet(g_orig);
}

// The game picks with the screen points of the mouse (CWorldClient::PickTerrainPoint 0x56C350 / PickTerrainPoint2 0x56BF30: out, x, y as floats, and PickObjectsAt 0x565790 /
// PickObjectsAt2 0x5659D0: a POINT*): its screen is now the band's width / height further to the top left, so the points get the same shift.
extern "C" __attribute__((force_align_arg_pointer)) void UmPickTerrainFix(int* args) {
    if (!g_guardOn) return;
    float x, y; std::memcpy(&x, &args[1], 4); std::memcpy(&y, &args[2], 4);
    x += static_cast<float>(g_guardPx); y += static_cast<float>(g_guardPy);
    std::memcpy(&args[1], &x, 4); std::memcpy(&args[2], &y, 4);
}

extern "C" __attribute__((force_align_arg_pointer)) void UmPickObjectsFix(int* args) {
    if (!g_guardOn) return;
    static long ring[4][2]; static int next = 0;
    const long* pt = reinterpret_cast<const long*>(args[0]);
    if (!pt) return;
    long* copy = ring[next]; next = (next + 1) & 3;
    copy[0] = pt[0] + g_guardPx; copy[1] = pt[1] + g_guardPy;
    args[0] = reinterpret_cast<int>(copy);
}

extern "C" void UmViewportHook();
extern "C" void UmPerspectiveHook();
extern "C" void UmPickTerrainHook();
extern "C" void UmPickTerrain2Hook();
extern "C" void UmPickObjectsHook();
extern "C" void UmPickObjects2Hook();

extern "C" void UmDrawHook();
extern "C" void UmDrawTrampoline();

namespace {
void InstallDrawHook() {
    static const unsigned char kExpect[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    unsigned char* at = reinterpret_cast<unsigned char*>(0x0051B7E0);
    if (std::memcmp(at, kExpect, 5) != 0) { UmEngineLog("um-engine: sun shadows: CObject3DClientSpecific::Draw has other code (another build): per-kind switches not applied"); return; }
    DWORD old = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    const long rel = reinterpret_cast<long>(&UmDrawHook) - reinterpret_cast<long>(at + 5);
    at[0] = 0xE9; std::memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
}

// the first bytes of a function are replaced by a jump (the rest of the replaced instructions is NOPs); the hook runs them again
void HookPrologue(uintptr_t address, void* hook, int length = 6, int b0 = 0x55, int b1 = 0x8B, int b2 = 0xEC, int b3 = 0x83, int b4 = 0xEC, int b5 = 0x08) {
    const unsigned char expect[6] = { static_cast<unsigned char>(b0), static_cast<unsigned char>(b1), static_cast<unsigned char>(b2), static_cast<unsigned char>(b3), static_cast<unsigned char>(b4), static_cast<unsigned char>(b5) };
    unsigned char* at = reinterpret_cast<unsigned char*>(address);
    if (std::memcmp(at, expect, length) != 0) { UmEngineLog("um-engine: sun shadows: 0x%X has other code (another build): no guard band", static_cast<unsigned>(address)); return; }
    DWORD old = 0;
    if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) return;
    const long rel = reinterpret_cast<long>(hook) - reinterpret_cast<long>(at + 5);
    at[0] = 0xE9; std::memcpy(at + 1, &rel, 4);
    for (int i = 5; i < length; ++i) at[i] = 0x90;
    VirtualProtect(at, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, length);
}

extern "C" void UmRenderWrap();

void InstallSetupHook() {
    // the wrapper of CWorldClient::Render: mov ecx, 0x7AF5B8 / jmp 0x569E40 -> the jump goes through UmRenderWrap
    static const unsigned char kJump[5] = { 0xE9, 0x26, 0x15, 0xFF, 0xFF };
    unsigned char* wrap = reinterpret_cast<unsigned char*>(0x00578915);
    if (std::memcmp(wrap, kJump, 5) != 0) { UmEngineLog("um-engine: sun shadows: the world render wrapper has other code (another build): no guard band"); return; }
    HookPrologue(0x00565790, reinterpret_cast<void*>(&UmPickObjectsHook), 5, 0x83, 0xEC, 0x1C, 0x53, 0x55);
    HookPrologue(0x005659D0, reinterpret_cast<void*>(&UmPickObjects2Hook), 5, 0x83, 0xEC, 0x1C, 0x53, 0x55);
    HookPrologue(0x004F1370, reinterpret_cast<void*>(&UmViewportHook));
    HookPrologue(0x004F0E80, reinterpret_cast<void*>(&UmPerspectiveHook));
    DWORD old = 0;
    if (!VirtualProtect(wrap, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    const long rel = reinterpret_cast<long>(&UmRenderWrap) - reinterpret_cast<long>(wrap + 5);
    std::memcpy(wrap + 1, &rel, 4);
    VirtualProtect(wrap, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), wrap, 5);
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
    g_caster = reinterpret_cast<CasterFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowCaster")));
    g_enabled = g_begin && g_end;
    if (g_enabled && g_caster) InstallDrawHook();
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
    // with the guard band the screen is the middle of the (larger) window the game draws: its coordinates are shifted by the band's width
    const double gpx = g_guardOn ? g_guardPx : 0, gpy = g_guardOn ? g_guardPy : 0;
    const double width = 2 * hw - 2 * gpx, height = 2 * hh - 2 * gpy;
    p.guard[0] = static_cast<float>(gpx); p.guard[1] = static_cast<float>(gpy);
    const double groundZ = Read<float>(kCamera + 0xE8);
    double cx = 0, cy = 0, cz = groundZ;
    double hits[4][3];
    int n = 0;
    const double xs[4] = { gpx, gpx + width, gpx, gpx + width }, ys[4] = { gpy, gpy, gpy + height, gpy + height };
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
    // the radius moves in steps of 4 units: a radius that follows the camera continuously changes the texel size every frame and the shadows shimmer
    p.radius = static_cast<float>(std::fmin(std::fmax(std::ceil((radius + cfg.margin) / 4.0) * 4.0, 16.0), 120.0));
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

// Called once from um_engine.cpp when the call sites are installed (the camera is set up before the first world frame)
extern "C" void UmWorldEarlyInit() {
    if (C().guard > 0.0 || C().guardLog > 0.5) InstallSetupHook();
}

extern "C" __attribute__((force_align_arg_pointer)) void UmWorldBegin() {
    Lookup();
    g_open = false;
    if (!g_enabled) return;
    // Off: SunShadows::Enabled=0 (checked once a second), the game's own shadow switches all off (the options screen), a cave. The game's own pass is given back then.
    const bool indoor = Read<int>(kWorld + 0x6E0) == 1 && C().indoor < 0.5;
    if (C().enabled < 0.5 || !GameShadowsOn() || indoor) {
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
.globl _UmViewportHook
_UmViewportHook:
    leal 4(%esp), %eax
    pushl %eax
    call _UmViewportFix
    addl $4, %esp
    pushl %ebp
    movl %esp, %ebp
    subl $8, %esp
    pushl $0x004F1376
    ret
.globl _UmPerspectiveHook
_UmPerspectiveHook:
    leal 4(%esp), %eax
    pushl %eax
    call _UmPerspectiveFix
    addl $4, %esp
    pushl %ebp
    movl %esp, %ebp
    subl $8, %esp
    pushl $0x004F0E86
    ret
)");
asm(R"(
.text
.globl _UmRenderWrap
_UmRenderWrap:
    pushl %ecx
    call _UmGuardOn
    popl %ecx
    movl $0x00569E40, %eax
    call *%eax
    pushl %eax
    call _UmGuardOff
    popl %eax
    ret
)");
asm(R"(
.text
.globl _UmPickTerrainHook
_UmPickTerrainHook:
    pushl %ecx
    leal 8(%esp), %eax
    pushl %eax
    call _UmPickTerrainFix
    addl $4, %esp
    popl %ecx
    pushl %ebp
    movl %esp, %ebp
    subl $0x30, %esp
    pushl $0x0056C356
    ret
.globl _UmPickTerrain2Hook
_UmPickTerrain2Hook:
    pushl %ecx
    leal 8(%esp), %eax
    pushl %eax
    call _UmPickTerrainFix
    addl $4, %esp
    popl %ecx
    pushl %ebp
    movl %esp, %ebp
    subl $0x30, %esp
    pushl $0x0056BF36
    ret
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
