// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: research tracer for the game's Direct3D 7 device (UM_TRACE_D3D=1): which render states and texture stage states each kind of textured draw uses (the unit shadows,
// the footprints, ...), one um.log line per distinct combination. Not for players.
#include "um_internal.hpp"

#include <map>
#include <string>

namespace {
void* g_texture0 = NULL;
typedef HRESULT (WINAPI *SetTextureFn)(void*, DWORD, void*);
typedef HRESULT (WINAPI *DrawPrimitiveFn)(void*, DWORD, DWORD, void*, DWORD, DWORD);
typedef HRESULT (WINAPI *DrawIndexedPrimitiveFn)(void*, DWORD, DWORD, void*, DWORD, void*, DWORD, DWORD);
typedef HRESULT (WINAPI *GetRSFn)(void*, DWORD, DWORD*);
typedef HRESULT (WINAPI *GetTssFn)(void*, DWORD, DWORD, DWORD*);
SetTextureFn g_setTexture = NULL;
DrawPrimitiveFn g_drawPrimitive = NULL;
DrawIndexedPrimitiveFn g_drawIndexed = NULL;
std::map<std::string, long> g_seen;

int g_dumped = 0;
// The first few draws with a 64x64 16-bit video texture (the unit shadows): the texture's bytes (um-shadow-N.raw) and the vertices (um-shadow-N.txt: x y z rhw u v diffuse) with the indices.
void DumpShadowDraw(void* texture, DWORD w, DWORD h, DWORD bits, const void* verts, DWORD nverts, const WORD* idx, DWORD nidx) {
    if (g_dumped >= 6 || w != 64 || h != 64 || bits != 16) return;
    const int n = g_dumped++;
    char path[MAX_PATH]; GetModuleFileNameA(g_dllModule, path, MAX_PATH);
    std::string dir = path; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    BYTE desc[128]; memset(desc, 0, 128); const DWORD size = 124; memcpy(desc, &size, 4);
    if (g_originalDDLock && g_originalDDUnlock && SUCCEEDED(g_originalDDLock(texture, NULL, desc, 0x11, NULL))) {
        BYTE* data = NULL; LONG pitch = 0; memcpy(&data, desc + 36, 4); memcpy(&pitch, desc + 16, 4);
        if (data && pitch > 0) { FILE* f = std::fopen((dir + "um-shadow-" + std::to_string(n) + ".raw").c_str(), "wb"); if (f) { for (int y = 0; y < 64; ++y) std::fwrite(data + y * pitch, 1, 128, f); std::fclose(f); } }
        g_originalDDUnlock(texture, NULL);
    }
    FILE* f = std::fopen((dir + "um-shadow-" + std::to_string(n) + ".txt").c_str(), "w");
    if (f) {
        const float* v = static_cast<const float*>(verts);
        std::fprintf(f, "vertices %lu\n", nverts);
        for (DWORD i = 0; i < nverts && i < 4000; ++i) { DWORD diffuse; std::memcpy(&diffuse, v + i * 8 + 4, 4); std::fprintf(f, "%g %g %g %g %g %g %08lX\n", v[i*8], v[i*8+1], v[i*8+2], v[i*8+3], v[i*8+6], v[i*8+7], diffuse); }
        std::fprintf(f, "indices %lu\n", nidx);
        for (DWORD i = 0; i + 2 < nidx && i < 12000; i += 3) std::fprintf(f, "%u %u %u\n", idx[i], idx[i+1], idx[i+2]);
        std::fclose(f);
    }
    LogLine("INFO", "D3D trace: dumped shadow draw %d (%lu vertices, %lu indices) to um-shadow-%d.raw / .txt", n, nverts, nidx, n);
}
void Trace(void* device, const char* kind, DWORD type, DWORD fvf, DWORD count, const void* verts = NULL, DWORD nverts = 0, const WORD* idx = NULL, DWORD nidx = 0) {
    if (g_seen.size() > 120) return;
    char tex[160] = "none";
    if (g_texture0 && !IsBadReadPtr(g_texture0, sizeof(void*))) {
        BYTE desc[128]; memset(desc, 0, 128); const DWORD size = 124; memcpy(desc, &size, 4);
        void** vt = *reinterpret_cast<void***>(g_texture0);
        if (!IsBadReadPtr(vt + DD_VTABLE_GETSURFACEDESC_SLOT, sizeof(void*)) && SUCCEEDED(reinterpret_cast<DDGetSurfaceDescFunction>(vt[DD_VTABLE_GETSURFACEDESC_SLOT])(g_texture0, desc))) {
            DWORD h, w, bits, caps, flags, mips, ckey;
            memcpy(&flags, desc + 4, 4); memcpy(&h, desc + 8, 4); memcpy(&w, desc + 12, 4); memcpy(&mips, desc + 24, 4); memcpy(&bits, desc + 84, 4); memcpy(&caps, desc + 104, 4); memcpy(&ckey, desc + 56, 4);
            std::snprintf(tex, sizeof tex, "tex %lux%lu %lubpp caps 0x%lX mips %lu", w, h, bits, caps, mips);
            if (verts && idx && fvf == 0x1C4) DumpShadowDraw(g_texture0, w, h, bits, verts, nverts, idx, nidx);
        }
    }
    void** vt = *reinterpret_cast<void***>(device);
    const GetRSFn getRs = reinterpret_cast<GetRSFn>(vt[21]); const GetTssFn getTss = reinterpret_cast<GetTssFn>(vt[36]);
    DWORD rs[10] = {}; const DWORD ids[10] = {7, 14, 15, 24, 25, 27, 19, 20, 22, 136}; // ZENABLE ZWRITE ALPHATEST ALPHAREF ALPHAFUNC ALPHABLEND SRC DEST CULL LIGHTING
    for (int i = 0; i < 10; ++i) getRs(device, ids[i], &rs[i]);
    DWORD ts[8] = {}; const DWORD tids[8] = {1, 2, 4, 5, 16, 17, 18, 12}; // COLOROP COLORARG1 ALPHAOP ALPHAARG1 MAGFILTER MINFILTER MIPFILTER ADDRESS
    for (int i = 0; i < 8; ++i) getTss(device, 0, tids[i], &ts[i]);
    char line[600];
    std::snprintf(line, sizeof line, "%s type %lu fvf 0x%lX | %s | RS z %lu zw %lu atest %lu aref %lu afunc %lu ablend %lu src %lu dst %lu cull %lu light %lu | TSS0 cop %lu carg %lu aop %lu aarg %lu mag %lu min %lu mip %lu addr %lu",
                  kind, type, fvf, tex, rs[0], rs[1], rs[2], rs[3], rs[4], rs[5], rs[6], rs[7], rs[8], rs[9], ts[0], ts[1], ts[2], ts[3], ts[4], ts[5], ts[6], ts[7]);
    long& n = g_seen[line];
    if (n++ == 0) LogLine("INFO", "D3D trace: %s (first draw: %lu primitives)", line, count);
}

// Per-frame draw statistics (UM_TRACE_D3D): draws / primitives / vertices by class, logged every 120 BeginScene calls.
struct FrameStats { long draws = 0, prims = 0, verts = 0; };
FrameStats g_cls[4]; // 0 opaque (ztest+zwrite, no blend), 1 blended, 2 z-write-off opaque, 3 other
long g_frames = 0; DWORD g_fvfMask = 0;
typedef HRESULT (WINAPI *BeginSceneFn)(void*);
BeginSceneFn g_beginScene = NULL;
void Account(void* device, DWORD fvf, DWORD prims, DWORD verts) {
    void** vt = *reinterpret_cast<void***>(device);
    const GetRSFn getRs = reinterpret_cast<GetRSFn>(vt[21]);
    DWORD zw = 0, ab = 0; getRs(device, 14, &zw); getRs(device, 27, &ab);
    (void)ab; FrameStats& c = g_cls[zw ? 0 : 2];
    ++c.draws; c.prims += prims; c.verts += verts; g_fvfMask |= fvf;
}
HRESULT WINAPI HookedBeginScene(void* device) {
    if (++g_frames % 120 == 0) {
        LogLine("INFO", "D3D frame stats (last frame): opaque %ld draws %ld prims %ld verts | blended %ld draws %ld prims %ld verts | zwrite-off %ld draws %ld prims %ld verts | fvf mask 0x%lX",
                g_cls[0].draws, g_cls[0].prims, g_cls[0].verts, g_cls[1].draws, g_cls[1].prims, g_cls[1].verts, g_cls[2].draws, g_cls[2].prims, g_cls[2].verts, g_fvfMask);
    }
    for (FrameStats& c : g_cls) c = FrameStats();
    return g_beginScene(device);
}

HRESULT WINAPI HookedSetTexture(void* device, DWORD stage, void* texture) { if (stage == 0) g_texture0 = texture; return g_setTexture(device, stage, texture); }
HRESULT WINAPI HookedDrawPrimitive(void* device, DWORD type, DWORD fvf, void* verts, DWORD n, DWORD flags) { Account(device, fvf, n / 3, n); Trace(device, "DP", type, fvf, n); return g_drawPrimitive(device, type, fvf, verts, n, flags); }
int g_vdumped = 0;
// Research (UM_TRACE_VERTS=1): the vertices (x y z rhw) of the first big pre-transformed draws, to check the z / rhw relation of the game's projection.
void DumpVerts(const void* verts, DWORD n) {
    if (g_vdumped >= 3 || n < 200 || !std::getenv("UM_TRACE_VERTS")) return;
    char path[MAX_PATH]; GetModuleFileNameA(g_dllModule, path, MAX_PATH);
    std::string dir = path; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    FILE* f = std::fopen((dir + "um-verts-" + std::to_string(g_vdumped++) + ".txt").c_str(), "w");
    if (!f) return;
    const float* v = static_cast<const float*>(verts);
    for (DWORD i = 0; i < n && i < 3000; ++i) std::fprintf(f, "%g %g %g %g\n", v[i*8], v[i*8+1], v[i*8+2], v[i*8+3]);
    std::fclose(f);
}
HRESULT WINAPI HookedDrawIndexed(void* device, DWORD type, DWORD fvf, void* verts, DWORD n, void* idx, DWORD ni, DWORD flags) { if (fvf == 0x1C4) DumpVerts(verts, n); Account(device, fvf, ni / 3, n); Trace(device, "DIP", type, fvf, ni / 3, verts, n, static_cast<const WORD*>(idx), ni); return g_drawIndexed(device, type, fvf, verts, n, idx, ni, flags); }
}

void TraceInstallDevice(void* device) {
    bool already = false;
    if (void* o = PatchVTableSlot(device, 5, reinterpret_cast<void*>(HookedBeginScene), &already)) g_beginScene = reinterpret_cast<BeginSceneFn>(o);
    if (void* o = PatchVTableSlot(device, 35, reinterpret_cast<void*>(HookedSetTexture), &already)) g_setTexture = reinterpret_cast<SetTextureFn>(o);
    if (void* o = PatchVTableSlot(device, 25, reinterpret_cast<void*>(HookedDrawPrimitive), &already)) g_drawPrimitive = reinterpret_cast<DrawPrimitiveFn>(o);
    if (void* o = PatchVTableSlot(device, 26, reinterpret_cast<void*>(HookedDrawIndexed), &already)) g_drawIndexed = reinterpret_cast<DrawIndexedPrimitiveFn>(o);
    LogLine("INFO", "D3D trace: SetTexture / DrawPrimitive / DrawIndexedPrimitive hooked (UM_TRACE_D3D)");
}
