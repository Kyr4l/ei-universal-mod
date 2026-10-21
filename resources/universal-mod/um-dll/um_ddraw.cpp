// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: DirectDraw: the UI blit cache, the shadow lock cache, the frame counter hooks.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// ---- the UI blit cache (#80) ------------------------------------------------------------------------
// The game draws its interface panels into system-memory surfaces and Blts them onto the 3D back buffer every
// frame. Through D7VK / DXVK a surface that was the source of a GPU transfer makes the game's NEXT Lock of it
// wait for that transfer (1-4 ms each, measured), so the game's surfaces must never be GPU sources: each Blt
// copies the picture on the CPU into one of two staging surfaces of ours (alternating, so the one written was
// last used two frames ago) and the Blt runs from the staging copy.
struct UiStage {
    void* surface[2] = {NULL, NULL};
    int next = 0;
    DWORD width = 0, height = 0, bits = 0, rowBytes = 0;
    bool broken = false; // could not be made or copied: this source is blitted the normal way
};
static std::unordered_map<void*, UiStage> g_uiStages; // game thread only (the DirectDraw hooks)
volatile LONG g_uiStaged = 0;

static bool DdSurfaceDesc(void* surface, BYTE desc[128]) {
    memset(desc, 0, 128);
    const DWORD size = 124;
    memcpy(desc, &size, 4);
    void** vtable = IsBadReadPtr(surface, sizeof(void*)) ? NULL : *reinterpret_cast<void***>(surface);
    if (!vtable || IsBadReadPtr(vtable + DD_VTABLE_GETSURFACEDESC_SLOT, sizeof(void*))) return false;
    return SUCCEEDED(reinterpret_cast<DDGetSurfaceDescFunction>(vtable[DD_VTABLE_GETSURFACEDESC_SLOT])(surface, desc));
}

// A surface like `desc` with caps `caps`, made with the game's own DirectDraw; NULL when it fails.
static void* DdMakeSurfaceLike(const BYTE desc[128], DWORD caps) {
    if (!g_directDrawObject || !g_originalDDCreateSurface) return NULL;
    BYTE make[128];
    memcpy(make, desc, 128);
    const DWORD flags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT
    memcpy(make + 4, &flags, 4);
    memcpy(make + 104, &caps, 4);
    memset(make + 108, 0, 12);
    const DWORD zero = 0;
    memcpy(make + 16, &zero, 4); memcpy(make + 20, &zero, 4); memcpy(make + 24, &zero, 4); // no pitch, back buffers, mips
    void* made = NULL;
    if (FAILED(g_originalDDCreateSurface(g_directDrawObject, make, &made, NULL)) || !made) return NULL;
    return made;
}

// `source`'s pixels copied into `target` on the CPU (the real Lock on both: neither is a GPU source).
static bool DdCopyPixels(void* source, void* target, DWORD height, DWORD rowBytes) {
    BYTE src[128], dst[128];
    const DWORD size = 124;
    memset(src, 0, 128); memset(dst, 0, 128); memcpy(src, &size, 4); memcpy(dst, &size, 4);
    if (FAILED(g_originalDDLock(source, NULL, src, 0x11, NULL))) return false;       // DDLOCK_WAIT | READONLY
    if (FAILED(g_originalDDLock(target, NULL, dst, 0x21, NULL))) { g_originalDDUnlock(source, NULL); return false; } // WAIT | WRITEONLY
    BYTE* from = NULL; BYTE* to = NULL; LONG fromPitch = 0, toPitch = 0;
    memcpy(&from, src + 36, 4); memcpy(&to, dst + 36, 4); memcpy(&fromPitch, src + 16, 4); memcpy(&toPitch, dst + 16, 4);
    const bool ok = from && to && fromPitch > 0 && toPitch > 0;
    if (ok) for (DWORD y = 0; y < height; ++y) memcpy(to + static_cast<size_t>(toPitch) * y, from + static_cast<size_t>(fromPitch) * y, rowBytes);
    g_originalDDUnlock(target, NULL);
    g_originalDDUnlock(source, NULL);
    return ok;
}

static void DdCopyColorKey(void* source, void* target) {
    void** vtable = *reinterpret_cast<void***>(source);
    BYTE key[8] = {};
    if (SUCCEEDED(reinterpret_cast<DDColorKeyFunction>(vtable[DD_VTABLE_GETCOLORKEY_SLOT])(source, 0x8, key))) // DDCKEY_SRCBLT
        reinterpret_cast<DDColorKeyFunction>((*reinterpret_cast<void***>(target))[DD_VTABLE_SETCOLORKEY_SLOT])(target, 0x8, key);
}

// The staging copy to blit from instead of `source` (NULL: blit the source itself).
void* UiStageFor(void* source, void* destination) {
    if (!g_uiBlitCache || !g_directDrawObject || !g_originalDDLock || !g_originalDDUnlock || !source) return NULL;
    BYTE desc[128];
    if (!DdSurfaceDesc(source, desc)) return NULL;
    DWORD srcCaps = 0, dstCaps = 0, width = 0, height = 0, bits = 0;
    memcpy(&srcCaps, desc + 104, 4); memcpy(&height, desc + 8, 4); memcpy(&width, desc + 12, 4); memcpy(&bits, desc + 84, 4);
    if (!(srcCaps & 0x800) || (srcCaps & 0x1000) || width == 0 || height == 0 || bits < 8) return NULL; // only plain system-memory pictures
    BYTE dstDesc[128];
    if (!DdSurfaceDesc(destination, dstDesc)) return NULL;
    memcpy(&dstCaps, dstDesc + 104, 4);
    if (!(dstCaps & 0x4000)) return NULL; // only onto video memory (the frame)
    UiStage& st = g_uiStages[source];
    if (st.broken) return NULL;
    if (!st.surface[0]) LogLine("INFO", "UI blit cache: first blit of a %lux%lu %lubpp picture onto video memory: making its staging surfaces", width, height, bits);
    if (st.surface[0] && (st.width != width || st.height != height || st.bits != bits)) { st.surface[0] = st.surface[1] = NULL; } // the pointer was reused
    if (!st.surface[0]) {
        st.surface[0] = DdMakeSurfaceLike(desc, 0x40 | 0x800); // OFFSCREENPLAIN | SYSTEMMEMORY
        LogLine("INFO", "UI blit cache: staging surface 1 %s", st.surface[0] ? "made" : "FAILED");
        st.surface[1] = DdMakeSurfaceLike(desc, 0x40 | 0x800);
        LogLine("INFO", "UI blit cache: staging surface 2 %s", st.surface[1] ? "made" : "FAILED");
        if (!st.surface[0] || !st.surface[1]) { st.broken = true; LogLine("WARN", "UI blit cache: no staging surfaces for a %lux%lu %lubpp picture, blitting it the normal way", width, height, bits); return NULL; }
        DdCopyColorKey(source, st.surface[0]);
        DdCopyColorKey(source, st.surface[1]);
        st.width = width; st.height = height; st.bits = bits; st.rowBytes = width * (bits / 8);
        LogLine("INFO", "UI blit cache: staging surfaces made for a %lux%lu %lubpp system-memory picture", width, height, bits);
    }
    void* stage = st.surface[st.next];
    st.next ^= 1;
    static int firstCopies = 0;
    const bool say = firstCopies < 4;
    if (say) { ++firstCopies; LogLine("INFO", "UI blit cache: copying the %lux%lu picture into staging surface %d", st.width, st.height, st.next ^ 1); }
    if (!DdCopyPixels(source, stage, st.height, st.rowBytes)) { st.broken = true; LogLine("WARN", "UI blit cache: the copy into the staging surface failed, blitting the source the normal way"); return NULL; }
    if (say) LogLine("INFO", "UI blit cache: copied; blitting from the staging surface");
    InterlockedIncrement(&g_uiStaged);
    return stage;
}

// ---- the shadow lock cache (#80) --------------------------------------------------------------------
// fcn.0051ce60 draws every unit's shadow each frame into a small system-memory texture (Lock, a CPU draw,
// Unlock) and uploads it with IDirect3DDevice7::Load. The upload makes the game's next Lock of that texture
// wait (measured: 1.3-1.6 ms per frame over all units). Same cure as the interface: the Load is served from
// one of two staging textures of ours (the picture copied on the CPU), so the game's texture is never a GPU
// source and its Lock never waits.
struct TexStage {
    void* surface[2] = {NULL, NULL};
    int next = 0;
    DWORD width = 0, height = 0, bits = 0, rowBytes = 0;
    bool broken = false;
};
static std::unordered_map<void*, TexStage> g_texStages; // game thread only
volatile LONG g_shadowStaged = 0;

static HRESULT WINAPI HookedDDUnlock(void* self, LPRECT rect) { return g_originalDDUnlock(self, rect); }

// IDirect3DDevice7::Load(dest, destPoint, source, sourceRect, flags): a whole small system-memory texture
// goes through a staging copy of ours.
static HRESULT WINAPI HookedD3DLoad(void* device, void* dest, void* destPoint, void* source, void* sourceRect, DWORD flags) {
    if (g_shadowLockCache && source && !destPoint && !sourceRect && g_originalDDLock && g_originalDDUnlock) {
        TexStage& st = g_texStages[source];
        if (!st.broken) {
            BYTE desc[128];
            DWORD caps = 0, width = 0, height = 0, bits = 0, ddsdFlags = 0, mips = 0;
            if (DdSurfaceDesc(source, desc)) {
                memcpy(&caps, desc + 104, 4); memcpy(&height, desc + 8, 4); memcpy(&width, desc + 12, 4); memcpy(&bits, desc + 84, 4);
                memcpy(&ddsdFlags, desc + 4, 4); memcpy(&mips, desc + 24, 4);
            }
            const bool fit = (caps & 0x800) && (caps & 0x1000) && width && height && width <= 256 && height <= 256 && bits >= 8 && !((ddsdFlags & 0x20000) && mips > 1);
            if (!fit) st.broken = true;
            else {
                if (st.surface[0] && (st.width != width || st.height != height || st.bits != bits)) st.surface[0] = st.surface[1] = NULL;
                if (!st.surface[0]) {
                    LogLine("INFO", "Shadow lock cache: first Load of a %lux%lu %lubpp texture: making its staging textures", width, height, bits);
                    st.surface[0] = DdMakeSurfaceLike(desc, 0x800 | 0x1000); // SYSTEMMEMORY | TEXTURE
                    st.surface[1] = DdMakeSurfaceLike(desc, 0x800 | 0x1000);
                    if (!st.surface[0] || !st.surface[1]) { st.broken = true; LogLine("WARN", "Shadow lock cache: no staging textures for a %lux%lu %lubpp texture, loading it the normal way", width, height, bits); }
                    else { st.width = width; st.height = height; st.bits = bits; st.rowBytes = width * (bits / 8); LogLine("INFO", "Shadow lock cache: staging textures made for a %lux%lu %lubpp system-memory texture", width, height, bits); }
                }
                if (!st.broken) {
                    void* stage = st.surface[st.next];
                    st.next ^= 1;
                    if (DdCopyPixels(source, stage, st.height, st.rowBytes)) {
                        InterlockedIncrement(&g_shadowStaged);
                        return g_originalD3DLoad(device, dest, destPoint, stage, sourceRect, flags);
                    }
                    st.broken = true;
                    LogLine("WARN", "Shadow lock cache: the copy into the staging texture failed, loading the normal way");
                }
            }
        }
    }
    return g_originalD3DLoad(device, dest, destPoint, source, sourceRect, flags);
}

static void* g_quadDevice = NULL; // the game's IDirect3DDevice7 (the UI quad blit draws on it)
static ULONG (WINAPI *g_originalDeviceRelease)(void*) = NULL;
// The device's last Release destroys it: the quad blit must not use its pointer after that.
static ULONG WINAPI HookedDeviceRelease(void* self) {
    const ULONG left = g_originalDeviceRelease(self);
    if (left == 0 && self == g_quadDevice) { g_quadDevice = NULL; LogLine("INFO", "UI quad blit: the game's D3D device was released"); }
    return left;
}
static HRESULT WINAPI HookedD3DCreateDevice(void* d3d, const IID* iid, void* target, void** device) {
    const HRESULT result = g_originalD3DCreateDevice(d3d, iid, target, device);
    if (SUCCEEDED(result) && device && *device && g_uiQuadBlit) {
        g_quadDevice = *device; // the UI quad blit draws on it
        bool already = false;
        void* original = PatchVTableSlot(*device, 2 /* IUnknown::Release */, reinterpret_cast<void*>(HookedDeviceRelease), &already);
        if (original) g_originalDeviceRelease = reinterpret_cast<ULONG (WINAPI*)(void*)>(original);
    }
    if (SUCCEEDED(result) && device && *device && !g_originalD3DLoad) {
        void* original = PatchVTableSlot(*device, D3DDEVICE7_VTABLE_LOAD_SLOT, reinterpret_cast<void*>(HookedD3DLoad));
        if (original) { g_originalD3DLoad = reinterpret_cast<D3DLoadFunction>(original); LogLine("INFO", "IDirect3DDevice7::Load hooked (the shadow lock cache)"); }
    }
    return result;
}


// ---- the UI quad blit (#80) -------------------------------------------------------------------------
// A Blt of a system-memory interface picture onto the 3D frame (the render target) makes D7VK read the whole frame
// back from the GPU (the CPU waits until the GPU has finished it), do the blit on the CPU in Wine's own surface and
// upload the whole frame again: 16 MB of copies and a stall per frame (measured: the copies are 2.5 ms of the main
// thread's ~11 ms per frame, the stall is the rest of the 42 ms the one Blt takes in the profiler). Here the picture
// becomes a texture (kept, and rewritten only when its pixels change) and is drawn with the game's own D3D7 device as
// one pre-transformed quad, with the colour key as texel alpha: nothing is read back.
// Only the plain case is handled: a copy (or a source colour key override) of the same size onto the device's render
// target, from a 16 or 32 bit RGB system-memory surface. Everything else is blitted the normal way.
volatile LONG g_uiQuadDrawn = 0;
volatile LONG g_uiQuadFallback = 0;

struct QuadSource {
    void* texture = NULL;
    DWORD width = 0, height = 0, texWidth = 0, texHeight = 0, bits = 0;
    DWORD rMask = 0, gMask = 0, bMask = 0;
    bool keyed = false;
    DWORD key = 0;
    bool broken = false;
    std::vector<BYTE> shadow; // the source's pixels as last converted
};
static std::unordered_map<void*, QuadSource> g_quadSources; // game thread only

typedef HRESULT (WINAPI *DevNoArgFn)(void*);
typedef HRESULT (WINAPI *DevGetPtrFn)(void*, void**);
typedef HRESULT (WINAPI *DevViewportFn)(void*, void*);
typedef HRESULT (WINAPI *DevSetRSFn)(void*, DWORD, DWORD);
typedef HRESULT (WINAPI *DevGetRSFn)(void*, DWORD, DWORD*);
typedef HRESULT (WINAPI *DevDrawFn)(void*, DWORD, DWORD, void*, DWORD, DWORD);
typedef HRESULT (WINAPI *DevGetTexFn)(void*, DWORD, void**);
typedef HRESULT (WINAPI *DevSetTexFn)(void*, DWORD, void*);
typedef HRESULT (WINAPI *DevSetTssFn)(void*, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *DevGetTssFn)(void*, DWORD, DWORD, DWORD*);
typedef ULONG (WINAPI *ReleaseFn)(void*);
enum { kDevBeginScene = 5, kDevEndScene = 6, kDevGetRenderTarget = 9, kDevSetViewport = 13, kDevGetViewport = 15, kDevSetRS = 20, kDevGetRS = 21,
       kDevDrawPrimitive = 25, kDevGetTexture = 34, kDevSetTexture = 35, kDevGetTss = 36, kDevSetTss = 37 };

static DWORD NextPow2(DWORD v) { DWORD p = 1; while (p < v) p <<= 1; return p; }

static void QuadNoteFallback(const char* why) {
    InterlockedIncrement(&g_uiQuadFallback);
    static int said = 0;
    if (said < 12) { ++said; LogLine("INFO", "UI quad blit: a Blt went the normal way (%s)", why); }
}

// The picture of `source` as an ARGB texture (made on the first use, rewritten when its pixels changed); NULL when it cannot be.
static QuadSource* QuadSourceFor(void* source, bool keyed, DWORD key) {
    QuadSource& q = g_quadSources[source];
    if (q.broken) return NULL;
    BYTE desc[128];
    if (!DdSurfaceDesc(source, desc)) { q.broken = true; return NULL; }
    DWORD caps = 0, width = 0, height = 0, bits = 0, pfFlags = 0, rMask = 0, gMask = 0, bMask = 0;
    memcpy(&caps, desc + 104, 4); memcpy(&height, desc + 8, 4); memcpy(&width, desc + 12, 4); memcpy(&bits, desc + 84, 4);
    memcpy(&pfFlags, desc + 76, 4); memcpy(&rMask, desc + 88, 4); memcpy(&gMask, desc + 92, 4); memcpy(&bMask, desc + 96, 4);
    if (!(caps & 0x800) || (caps & 0x1000) || !(pfFlags & 0x40) || (bits != 16 && bits != 32) || width == 0 || height == 0 || width > 4096 || height > 4096) {
        q.broken = true;
        LogLine("INFO", "UI quad blit: a %lux%lu %lubpp picture (caps 0x%lX, pixel flags 0x%lX) is not handled: its Blts go the normal way", width, height, bits, caps, pfFlags);
        return NULL;
    }
    if (q.texture && (q.width != width || q.height != height || q.bits != bits || q.rMask != rMask || q.gMask != gMask || q.bMask != bMask)) {
        reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(q.texture))[2])(q.texture); // the pointer was reused by another picture
        q.texture = NULL;
        q.shadow.clear();
    }
    bool everything = false;
    if (!q.texture) {
        if (!g_directDrawObject || !g_originalDDCreateSurface) return NULL;
        q.width = width; q.height = height; q.bits = bits; q.rMask = rMask; q.gMask = gMask; q.bMask = bMask;
        q.texWidth = NextPow2(width); q.texHeight = NextPow2(height);
        BYTE make[128];
        memset(make, 0, 128);
        const DWORD size = 124, flags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT
        memcpy(make, &size, 4); memcpy(make + 4, &flags, 4);
        memcpy(make + 8, &q.texHeight, 4); memcpy(make + 12, &q.texWidth, 4);
        const DWORD pfSize = 32, pf = 0x40 | 0x1, bpp = 32, rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF, am = 0xFF000000; // DDPF_RGB | ALPHAPIXELS: ARGB8888
        memcpy(make + 72, &pfSize, 4); memcpy(make + 76, &pf, 4); memcpy(make + 84, &bpp, 4);
        memcpy(make + 88, &rm, 4); memcpy(make + 92, &gm, 4); memcpy(make + 96, &bm, 4); memcpy(make + 100, &am, 4);
        const DWORD texCaps = 0x1000, texCaps2 = 0x10; // DDSCAPS_TEXTURE, DDSCAPS2_TEXTUREMANAGE
        memcpy(make + 104, &texCaps, 4); memcpy(make + 108, &texCaps2, 4);
        void* made = NULL;
        if (FAILED(g_originalDDCreateSurface(g_directDrawObject, make, &made, NULL)) || !made) {
            q.broken = true;
            LogLine("WARN", "UI quad blit: no %lux%lu texture for a %lux%lu picture: its Blts go the normal way", q.texWidth, q.texHeight, width, height);
            return NULL;
        }
        q.texture = made;
        q.shadow.clear();
        LogLine("INFO", "UI quad blit: a %lux%lu %lubpp picture gets a %lux%lu texture", width, height, bits, q.texWidth, q.texHeight);
        everything = true;
    }
    if (q.keyed != keyed || q.key != key) { q.keyed = keyed; q.key = key; everything = true; }
    // the source's pixels, compared with the last ones converted
    BYTE src[128];
    memset(src, 0, 128);
    const DWORD size = 124;
    memcpy(src, &size, 4);
    if (FAILED(g_originalDDLock(source, NULL, src, 0x11, NULL))) return NULL; // DDLOCK_WAIT | READONLY
    BYTE* pixels = NULL; LONG pitch = 0;
    memcpy(&pixels, src + 36, 4); memcpy(&pitch, src + 16, 4);
    const DWORD bytes = bits / 8, rowBytes = width * bytes;
    bool ok = pixels && pitch > 0 && static_cast<DWORD>(pitch) >= rowBytes;
    if (ok) {
        if (q.shadow.size() != static_cast<size_t>(rowBytes) * height) { q.shadow.assign(static_cast<size_t>(rowBytes) * height, 0); everything = true; }
        long first = -1, last = -1;
        for (DWORD y = 0; y < height; ++y) {
            BYTE* mine = &q.shadow[static_cast<size_t>(y) * rowBytes];
            if (everything || memcmp(mine, pixels + static_cast<size_t>(pitch) * y, rowBytes) != 0) {
                memcpy(mine, pixels + static_cast<size_t>(pitch) * y, rowBytes);
                if (first < 0) first = static_cast<long>(y);
                last = static_cast<long>(y);
            }
        }
        if (first >= 0) { // the changed rows (between the first and the last that changed) go into the texture
            BYTE dst[128];
            memset(dst, 0, 128);
            memcpy(dst, &size, 4);
            if (FAILED(g_originalDDLock(q.texture, NULL, dst, 0x21, NULL))) ok = false; // DDLOCK_WAIT | WRITEONLY
            else {
                BYTE* out = NULL; LONG outPitch = 0;
                memcpy(&out, dst + 36, 4); memcpy(&outPitch, dst + 16, 4);
                if (!out || outPitch <= 0) ok = false;
                else for (long y = first; y <= last; ++y) {
                    const BYTE* in = &q.shadow[static_cast<size_t>(y) * rowBytes];
                    DWORD* row = reinterpret_cast<DWORD*>(out + static_cast<size_t>(outPitch) * y);
                    if (bits == 32) {
                        const DWORD keyRgb = key & 0x00FFFFFF;
                        for (DWORD x = 0; x < width; ++x) {
                            DWORD v; memcpy(&v, in + 4 * x, 4);
                            row[x] = (v & 0x00FFFFFF) | ((keyed && (v & 0x00FFFFFF) == keyRgb) ? 0u : 0xFF000000u);
                        }
                    } else { // 16 bits: widen each channel to 8 bits through the masks
                        auto widen = [](DWORD v, DWORD mask) -> DWORD {
                            if (!mask) return 0;
                            int shift = 0; DWORD m = mask; while (!(m & 1)) { m >>= 1; ++shift; }
                            const DWORD field = (v & mask) >> shift;
                            return m == 0xFF ? field : (field * 255 + m / 2) / m;
                        };
                        for (DWORD x = 0; x < width; ++x) {
                            WORD v; memcpy(&v, in + 2 * x, 2);
                            const bool transparent = keyed && v == static_cast<WORD>(key);
                            row[x] = transparent ? 0u : (0xFF000000u | (widen(v, rMask) << 16) | (widen(v, gMask) << 8) | widen(v, bMask));
                        }
                    }
                }
                g_originalDDUnlock(q.texture, NULL);
            }
        }
    }
    g_originalDDUnlock(source, NULL);
    if (!ok) { QuadNoteFallback("the picture could not be converted"); return NULL; }
    return &q;
}


// ---- the software cursor's background (part of the UI quad blit) -----------------------------------------
// The game draws its mouse cursor in software (CSoftCursor::Draw 0x5E54A0): each frame it Blts the pixels under the
// cursor from the frame into a small system-memory surface (a read-back of the whole frame for D7VK: a GPU stall and
// 8 MB of copies), mixes the cursor into them with an AND mask (background & mask | cursor pixels, so the cursor's
// transparent pixels keep the background) and Blts that composite back onto the frame. Here the "read" fills the
// small surface with a marker colour instead (no frame access), the composite then holds the marker where the cursor
// is transparent, and its quad is drawn with the marker as the colour key: the same picture. The composite is the
// next plain Blt of the same size onto the frame on the same thread (CSoftCursor::Draw does nothing in between).
// The rectangle shrinks at the edges of the screen (a 32 x 20 cursor at the bottom), so any size up to the cursor's.
static const DWORD kCursorMarker = 0x00FA01FB;     // R 0xFA, G 0x01, B 0xFB
static DWORD g_cursorPendingThread = 0;            // set by a saved background, cleared by the composite that follows it
static LONG g_cursorPendingW = 0, g_cursorPendingH = 0;

static bool CursorSizeKnown(DWORD* width, DWORD* height) {
    const DWORD* w = reinterpret_cast<const DWORD*>(0x7C7C90);
    const DWORD* h = reinterpret_cast<const DWORD*>(0x7C7C94);
    if (!g_knownGameBuild || IsBadReadPtr(w, 4) || IsBadReadPtr(h, 4)) return false;
    if (*w < 8 || *w > 64 || *h < 8 || *h > 64) return false;
    *width = *w; *height = *h;
    return true;
}

static bool IsRenderTargetSurface(void* surface) {
    void** device = *reinterpret_cast<void***>(g_quadDevice);
    void* target = NULL;
    if (FAILED(reinterpret_cast<DevGetPtrFn>(device[kDevGetRenderTarget])(g_quadDevice, &target)) || !target) return false;
    reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(target))[2])(target);
    return target == surface;
}

static DWORD PackMarker(DWORD bits, DWORD rMask, DWORD gMask, DWORD bMask) {
    if (bits == 32) return kCursorMarker;
    auto pack = [](DWORD channel8, DWORD mask) -> DWORD {
        if (!mask) return 0;
        int shift = 0; DWORD m = mask; while (!(m & 1)) { m >>= 1; ++shift; }
        return ((channel8 * m + 127) / 255) << shift;
    };
    return pack(0xFA, rMask) | pack(0x01, gMask) | pack(0xFB, bMask);
}

// `Blt(destination small system surface <- the frame)`: fills it with the marker instead of reading the frame.
bool UiQuadCursorSave(void* destination, LPRECT destRect, void* source, LPRECT srcRect) {
    if (!g_uiQuadBlit || !g_quadDevice || !destination || !source || !srcRect || !g_originalDDLock || !g_originalDDUnlock) return false;
    DWORD cw = 0, ch = 0;
    if (!CursorSizeKnown(&cw, &ch)) return false;
    const LONG rw = srcRect->right - srcRect->left, rh = srcRect->bottom - srcRect->top;
    if (rw < 1 || rh < 1 || rw > static_cast<LONG>(cw) || rh > static_cast<LONG>(ch)) return false;
    if (destRect && (destRect->right - destRect->left != rw || destRect->bottom - destRect->top != rh)) return false;
    BYTE dd[128];
    if (!DdSurfaceDesc(destination, dd)) return false;
    DWORD caps = 0, w = 0, h = 0, bits = 0, pfFlags = 0, rMask = 0, gMask = 0, bMask = 0;
    memcpy(&caps, dd + 104, 4); memcpy(&h, dd + 8, 4); memcpy(&w, dd + 12, 4); memcpy(&bits, dd + 84, 4);
    memcpy(&pfFlags, dd + 76, 4); memcpy(&rMask, dd + 88, 4); memcpy(&gMask, dd + 92, 4); memcpy(&bMask, dd + 96, 4);
    // a small plain system-memory surface (the surface may be larger than the cursor: 32 x 32 for a 16 x 16 one)
    if (!(caps & 0x800) || (caps & 0x1000) || w < static_cast<DWORD>(rw) || h < static_cast<DWORD>(rh) || w > 128 || h > 128 || !(pfFlags & 0x40) || (bits != 16 && bits != 32)) return false;
    if (!IsRenderTargetSurface(source)) return false;
    const DWORD marker = PackMarker(bits, rMask, gMask, bMask);
    BYTE desc[128];
    memset(desc, 0, 128);
    const DWORD size = 124;
    memcpy(desc, &size, 4);
    if (FAILED(g_originalDDLock(destination, NULL, desc, 0x21, NULL))) return false;
    BYTE* pixels = NULL; LONG pitch = 0;
    memcpy(&pixels, desc + 36, 4); memcpy(&pitch, desc + 16, 4);
    const bool ok = pixels && pitch > 0;
    if (ok) for (DWORD y = 0; y < h; ++y) {
        BYTE* row = pixels + static_cast<size_t>(pitch) * y;
        if (bits == 32) for (DWORD x = 0; x < w; ++x) memcpy(row + 4 * x, &marker, 4);
        else for (DWORD x = 0; x < w; ++x) { const WORD m16 = static_cast<WORD>(marker); memcpy(row + 2 * x, &m16, 2); }
    }
    g_originalDDUnlock(destination, NULL);
    if (!ok) return false;
    g_cursorPendingThread = GetCurrentThreadId();
    g_cursorPendingW = rw; g_cursorPendingH = rh;
    InterlockedIncrement(&g_uiQuadDrawn);
    return true;
}

bool UiQuadBlt(void* destination, LPRECT destRect, void* source, LPRECT srcRect, DWORD flags, void* bltFx) {
    if (!g_quadDevice || !destination || !destRect || !source || !g_originalDDLock || !g_originalDDUnlock) return false;
    const DWORD kWait = 0x01000000, kAsync = 0x200, kKeyOverride = 0x10000;
    if (flags & ~(kWait | kAsync | kKeyOverride)) return false;
    bool keyed = (flags & kKeyOverride) != 0;
    if (keyed && !bltFx) return false;
    void** device = *reinterpret_cast<void***>(g_quadDevice);
    // only onto the device's current render target
    void* target = NULL;
    if (FAILED(reinterpret_cast<DevGetPtrFn>(device[kDevGetRenderTarget])(g_quadDevice, &target)) || !target) return false;
    reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(target))[2])(target);
    if (target != destination) return false;
    BYTE dstDesc[128];
    if (!DdSurfaceDesc(destination, dstDesc)) return false;
    DWORD frameW = 0, frameH = 0;
    memcpy(&frameH, dstDesc + 8, 4); memcpy(&frameW, dstDesc + 12, 4);
    if (!frameW || !frameH) return false;
    BYTE srcDesc[128];
    if (!DdSurfaceDesc(source, srcDesc)) return false;
    DWORD srcW = 0, srcH = 0;
    memcpy(&srcH, srcDesc + 8, 4); memcpy(&srcW, srcDesc + 12, 4);
    RECT from = {0, 0, static_cast<LONG>(srcW), static_cast<LONG>(srcH)};
    if (srcRect) from = *srcRect;
    if (from.left < 0 || from.top < 0 || from.right > static_cast<LONG>(srcW) || from.bottom > static_cast<LONG>(srcH) || from.right <= from.left || from.bottom <= from.top) return false;
    if (destRect->right - destRect->left != from.right - from.left || destRect->bottom - destRect->top != from.bottom - from.top) { QuadNoteFallback("stretched"); return false; }
    DWORD key = 0;
    if (keyed) memcpy(&key, static_cast<BYTE*>(bltFx) + 92, 4); // DDBLTFX.ddckSrcColorKey.dwColorSpaceLowValue
    // the software cursor's composite: the first plain Blt of the saved rectangle's size after its background was "read"
    if (g_cursorPendingThread) {
        if (g_cursorPendingThread == GetCurrentThreadId() && !keyed && destRect->right - destRect->left == g_cursorPendingW && destRect->bottom - destRect->top == g_cursorPendingH) {
            BYTE pf[128];
            DWORD bitsHere = 0, rM = 0, gM = 0, bM = 0;
            memcpy(&bitsHere, srcDesc + 84, 4); memcpy(&rM, srcDesc + 88, 4); memcpy(&gM, srcDesc + 92, 4); memcpy(&bM, srcDesc + 96, 4);
            (void)pf;
            keyed = true;
            key = PackMarker(bitsHere, rM, gM, bM);
            g_cursorPendingThread = 0;
        } else if (g_cursorPendingThread == GetCurrentThreadId()) {
            g_cursorPendingThread = 0; // something else came first: the marker would stay visible, but nothing was kept to undo it
        }
    }
    QuadSource* q = QuadSourceFor(source, keyed, key);
    if (!q) { InterlockedIncrement(&g_uiQuadFallback); return false; }

    // the device's states we change, kept to be put back
    static const DWORD kRenderStates[] = {7, 14, 15, 19, 20, 22, 24, 25, 27, 28, 29, 41, 52, 136, 137}; // ZENABLE ZWRITE ALPHATEST SRC DEST CULL ALPHAREF ALPHAFUNC ALPHABLEND FOG SPECULAR COLORKEY STENCIL CLIPPING LIGHTING
    static const DWORD kRenderValues[] = {0, 0, 0, 2, 1, 1, 0, 5, 0, 0, 0, 0, 0, 0, 0};          // off, SRCBLEND ONE not used (blend is off), CULL_NONE, ALPHAFUNC GREATER
    static const DWORD kStage0[] = {1, 2, 4, 5, 11, 12, 16, 17, 18, 24};                           // COLOROP COLORARG1 ALPHAOP ALPHAARG1 TEXCOORDINDEX ADDRESS MAGFILTER MINFILTER MIPFILTER TEXTURETRANSFORMFLAGS
    static const DWORD kStage0Values[] = {2, 2, 2, 2, 0, 3, 1, 1, 1, 0};                           // SELECTARG1 TEXTURE, SELECTARG1 TEXTURE, index 0, CLAMP, POINT, POINT, NONE, no transform
    DWORD savedRs[sizeof(kRenderStates) / sizeof(DWORD)], savedStage0[sizeof(kStage0) / sizeof(DWORD)], savedStage1ColorOp = 1, savedStage1AlphaOp = 1;
    BYTE savedViewport[28];
    void* savedTexture = NULL;
    const DevGetRSFn getRs = reinterpret_cast<DevGetRSFn>(device[kDevGetRS]);
    const DevSetRSFn setRs = reinterpret_cast<DevSetRSFn>(device[kDevSetRS]);
    const DevGetTssFn getTss = reinterpret_cast<DevGetTssFn>(device[kDevGetTss]);
    const DevSetTssFn setTss = reinterpret_cast<DevSetTssFn>(device[kDevSetTss]);
    bool saved = true;
    for (size_t i = 0; i < sizeof(kRenderStates) / sizeof(DWORD); ++i) if (FAILED(getRs(g_quadDevice, kRenderStates[i], &savedRs[i]))) saved = false;
    for (size_t i = 0; i < sizeof(kStage0) / sizeof(DWORD); ++i) if (FAILED(getTss(g_quadDevice, 0, kStage0[i], &savedStage0[i]))) saved = false;
    if (FAILED(getTss(g_quadDevice, 1, 1, &savedStage1ColorOp)) || FAILED(getTss(g_quadDevice, 1, 4, &savedStage1AlphaOp))) saved = false;
    if (FAILED(reinterpret_cast<DevViewportFn>(device[kDevGetViewport])(g_quadDevice, savedViewport))) saved = false;
    reinterpret_cast<DevGetTexFn>(device[kDevGetTexture])(g_quadDevice, 0, &savedTexture);
    if (!saved) { if (savedTexture) reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(savedTexture))[2])(savedTexture); QuadNoteFallback("the device's state could not be read"); return false; }

    const bool beganScene = SUCCEEDED(reinterpret_cast<DevNoArgFn>(device[kDevBeginScene])(g_quadDevice));
    BYTE viewport[28];
    memset(viewport, 0, 28);
    const DWORD vpX = 0, vpY = 0; const float zMin = 0.0f, zMax = 1.0f;
    memcpy(viewport, &vpX, 4); memcpy(viewport + 4, &vpY, 4); memcpy(viewport + 8, &frameW, 4); memcpy(viewport + 12, &frameH, 4);
    memcpy(viewport + 16, &zMin, 4); memcpy(viewport + 20, &zMax, 4);
    reinterpret_cast<DevViewportFn>(device[kDevSetViewport])(g_quadDevice, viewport);
    for (size_t i = 0; i < sizeof(kRenderStates) / sizeof(DWORD); ++i) setRs(g_quadDevice, kRenderStates[i], kRenderValues[i]);
    setRs(g_quadDevice, 15, keyed ? 1 : 0); // ALPHATESTENABLE: the colour key is the texels with alpha 0
    for (size_t i = 0; i < sizeof(kStage0) / sizeof(DWORD); ++i) setTss(g_quadDevice, 0, kStage0[i], kStage0Values[i]);
    setTss(g_quadDevice, 1, 1, 1); // stage 1 off (COLOROP DISABLE)
    setTss(g_quadDevice, 1, 4, 1); // ALPHAOP DISABLE
    reinterpret_cast<DevSetTexFn>(device[kDevSetTexture])(g_quadDevice, 0, q->texture);

    struct Vertex { float x, y, z, rhw, u, v; } quad[4];
    const float shift = 0.5f; // pre-transformed positions are pixel centres: the corners sit half a pixel before them
    const float l = static_cast<float>(destRect->left) - shift, t = static_cast<float>(destRect->top) - shift;
    const float r = static_cast<float>(destRect->right) - shift, b = static_cast<float>(destRect->bottom) - shift;
    const float u0 = static_cast<float>(from.left) / q->texWidth, u1 = static_cast<float>(from.right) / q->texWidth;
    const float v0 = static_cast<float>(from.top) / q->texHeight, v1 = static_cast<float>(from.bottom) / q->texHeight;
    quad[0] = {l, t, 0.0f, 1.0f, u0, v0};
    quad[1] = {r, t, 0.0f, 1.0f, u1, v0};
    quad[2] = {l, b, 0.0f, 1.0f, u0, v1};
    quad[3] = {r, b, 0.0f, 1.0f, u1, v1};
    const HRESULT drawn = reinterpret_cast<DevDrawFn>(device[kDevDrawPrimitive])(g_quadDevice, 5 /* D3DPT_TRIANGLESTRIP */, 0x4 | 0x100 /* D3DFVF_XYZRHW | D3DFVF_TEX1 */, quad, 4, 0);

    // put everything back
    reinterpret_cast<DevSetTexFn>(device[kDevSetTexture])(g_quadDevice, 0, savedTexture);
    if (savedTexture) reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(savedTexture))[2])(savedTexture);
    setTss(g_quadDevice, 1, 1, savedStage1ColorOp);
    setTss(g_quadDevice, 1, 4, savedStage1AlphaOp);
    for (size_t i = 0; i < sizeof(kStage0) / sizeof(DWORD); ++i) setTss(g_quadDevice, 0, kStage0[i], savedStage0[i]);
    for (size_t i = 0; i < sizeof(kRenderStates) / sizeof(DWORD); ++i) setRs(g_quadDevice, kRenderStates[i], savedRs[i]);
    reinterpret_cast<DevViewportFn>(device[kDevSetViewport])(g_quadDevice, savedViewport);
    if (beganScene) reinterpret_cast<DevNoArgFn>(device[kDevEndScene])(g_quadDevice);
    if (FAILED(drawn)) { QuadNoteFallback("the draw failed"); return false; }
    InterlockedIncrement(&g_uiQuadDrawn);
    return true;
}

// IDirectDraw::QueryInterface: the IDirect3D7 interface it hands out gets its CreateDevice hooked.
static const IID kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
static HRESULT WINAPI HookedDDQueryInterface(void* self, const IID* iid, void** out) {
    const HRESULT result = g_originalDDQueryInterface(self, iid, out);
    if (SUCCEEDED(result) && iid && out && *out && IsEqualGUID(*iid, kIID_IDirect3D7) && !g_originalD3DCreateDevice) {
        void* original = PatchVTableSlot(*out, D3D7_VTABLE_CREATEDEVICE_SLOT, reinterpret_cast<void*>(HookedD3DCreateDevice));
        if (original) g_originalD3DCreateDevice = reinterpret_cast<D3DCreateDeviceFunction>(original);
    }
    return result;
}

static HRESULT WINAPI HookedDDGetDC(void* self, HDC* dc) { return g_originalDDGetDC(self, dc); }

// Lock: a CPU write into a surface. On a video-memory surface the wrapper has to wait for the GPU first,
// which is where a whole frame can go; the per-pair line names the surface, its flags and the caller.
static HRESULT WINAPI HookedDDLock(void* self, LPRECT rect, void* desc, DWORD flags, HANDLE event) {
    DdTimer timer(kDdLock, rect, self, NULL, __builtin_return_address(0));
    timer.lockFlags = flags;
    return g_originalDDLock(self, rect, desc, flags, event);
}

// When the game creates its primary (front-buffer) surface, patch that
// surface's own Flip/Blt/BltFast vtable slots so every present is counted,
// without touching any other surface (textures, sprites, etc. would over-count).
static HRESULT WINAPI HookedDDCreateSurface(void* self, void* surfaceDesc,
        void** lplpDDSurface, IUnknown* outer) {
    HRESULT result = g_originalDDCreateSurface(self, surfaceDesc, lplpDDSurface, outer);
    if (!g_directDrawObject) {
        g_directDrawObject = self;
        void* originalQi = PatchVTableSlot(self, 0, reinterpret_cast<void*>(HookedDDQueryInterface)); // IUnknown::QueryInterface
        if (originalQi) g_originalDDQueryInterface = reinterpret_cast<DDQueryInterfaceFunction>(originalQi);
    }
    if (SUCCEEDED(result) && lplpDDSurface && *lplpDDSurface && surfaceDesc) {
        DWORD caps = *reinterpret_cast<DWORD*>(
            reinterpret_cast<uint8_t*>(surfaceDesc) + DD_SURFACEDESC_DDSCAPS_OFFSET);
        if ((caps & DD_DDSCAPS_PRIMARYSURFACE) != 0) {
            void* surface = *lplpDDSurface;
            void* originalFlip = PatchVTableSlot(surface, DD_VTABLE_FLIP_SLOT,
                reinterpret_cast<void*>(HookedDDFlip));
            if (originalFlip) {
                g_originalDDFlip = reinterpret_cast<DDFlipFunction>(originalFlip);
            }
            void* originalBlt = PatchVTableSlot(surface, DD_VTABLE_BLT_SLOT,
                reinterpret_cast<void*>(HookedDDBlt));
            if (originalBlt) {
                g_originalDDBlt = reinterpret_cast<DDBltFunction>(originalBlt);
            }
            void* originalBltFast = PatchVTableSlot(surface, DD_VTABLE_BLTFAST_SLOT,
                reinterpret_cast<void*>(HookedDDBltFast));
            if (originalBltFast) {
                g_originalDDBltFast = reinterpret_cast<DDBltFastFunction>(originalBltFast);
            }
            void* originalLock = PatchVTableSlot(surface, DD_VTABLE_LOCK_SLOT,
                reinterpret_cast<void*>(HookedDDLock));
            if (originalLock) {
                g_originalDDLock = reinterpret_cast<DDLockFunction>(originalLock);
            }
            void* originalGetDC = PatchVTableSlot(surface, DD_VTABLE_GETDC_SLOT,
                reinterpret_cast<void*>(HookedDDGetDC));
            if (originalGetDC) {
                g_originalDDGetDC = reinterpret_cast<DDGetDCFunction>(originalGetDC);
            }
            void* originalUnlock = PatchVTableSlot(surface, DD_VTABLE_UNLOCK_SLOT,
                reinterpret_cast<void*>(HookedDDUnlock));
            if (originalUnlock) {
                g_originalDDUnlock = reinterpret_cast<DDUnlockFunction>(originalUnlock);
            }
            InterlockedExchange(&g_overlayFrameCounterActive, 1);
            LogLine("INFO", "[OVERLAY] Primary surface Flip/Blt/BltFast hooked for the overlay's FPS counter");
        }
    }
    return result;
}

// Only the first (primary) enumerated driver is wanted, so cancel right away.
static HRESULT WINAPI CaptureDirectDrawBackendName(GUID* guid, LPSTR driverDescription,
        LPSTR driverName, LPVOID context) {
    (void)guid;
    (void)context;
    if (driverDescription && driverDescription[0] != '\0') {
        strncpy(g_directDrawBackendName, driverDescription, sizeof(g_directDrawBackendName) - 1);
    } else if (driverName && driverName[0] != '\0') {
        strncpy(g_directDrawBackendName, driverName, sizeof(g_directDrawBackendName) - 1);
    }
    return UM_DDENUMRET_CANCEL;
}

// Identify which DirectDraw driver is actually rendering (native Wine ddraw,
// a DDraw-to-D3D/Vulkan wrapper such as DXVK, etc.) via the driver description
// string DirectDrawEnumerateA reports, resolved dynamically since um.dll never
// links against ddraw.lib. Runs once per process; the result never changes.
static void IdentifyDirectDrawBackend() {
    if (InterlockedCompareExchange(&g_directDrawBackendIdentified, 1, 0) != 0) {
        return;
    }
    HMODULE ddrawModule = g_chainedDdraw ? g_chainedDdraw : GetModuleHandleA("ddraw.dll");
    DirectDrawEnumerateAFunction enumerate = NULL;
    if (ddrawModule) {
        FARPROC enumerateAddress = GetProcAddress(ddrawModule, "DirectDrawEnumerateA");
        memcpy(&enumerate, &enumerateAddress, sizeof(enumerate));
    }
    if (!enumerate) {
        LogLine("WARN", "DirectDrawEnumerateA unavailable; backend name will read as unknown");
        return;
    }
    enumerate(CaptureDirectDrawBackendName, NULL);
    LogLine("INFO", "DirectDraw backend identified as \"%s\"", g_directDrawBackendName);
}

// IDirectDraw::SetCooperativeLevel (slot 20): logged, so the profile can say whether the game asked for exclusive full screen.
static HRESULT (WINAPI *g_originalSetCooperativeLevel)(void*, HWND, DWORD) = NULL;
static HRESULT WINAPI HookedSetCooperativeLevel(void* self, HWND window, DWORD flags) {
    LogLine("INFO", "DirectDraw SetCooperativeLevel(flags 0x%lX%s%s)", flags, (flags & 0x10) ? " EXCLUSIVE" : "", (flags & 0x1) ? " FULLSCREEN" : "");
    return g_originalSetCooperativeLevel(self, window, flags);
}

// Patch CreateSurface on a newly created DirectDraw object so the overlay can
// find and hook the primary surface once the game creates it.
static void InstallDirectDrawFrameCounterHooks(void* directDrawObject) {
    bool alreadyPatched = false;
    void* original = PatchVTableSlot(directDrawObject, DD_VTABLE_CREATESURFACE_SLOT,
        reinterpret_cast<void*>(HookedDDCreateSurface), &alreadyPatched);
    {
        void* originalLevel = PatchVTableSlot(directDrawObject, 20, reinterpret_cast<void*>(HookedSetCooperativeLevel));
        if (originalLevel) g_originalSetCooperativeLevel = reinterpret_cast<HRESULT (WINAPI*)(void*, HWND, DWORD)>(originalLevel);
    }
    if (original) {
        g_originalDDCreateSurface = reinterpret_cast<DDCreateSurfaceFunction>(original);
        LogLine("INFO", "[OVERLAY] DirectDraw CreateSurface hooked for the overlay's FPS counter");
    } else if (!alreadyPatched) {
        LogLine("WARN", "DirectDraw CreateSurface was not hooked; FPS counter will read n/a");
    }
}

// Log DirectDraw initialization results without changing the returned object.
HRESULT WINAPI HookedDirectDrawCreate(const GUID* guid, void** directDraw,
        IUnknown* outerUnknown) {
    HRESULT result = g_originalDirectDrawCreate(guid, directDraw, outerUnknown);
    LogLine(FAILED(result) ? "ERROR" : "INFO",
        "DirectDrawCreate result=0x%08lX object=%p", result,
        directDraw ? *directDraw : NULL);
    if ((g_enableOverlay || g_uiQuadBlit) && SUCCEEDED(result) && directDraw && *directDraw) {
        IdentifyDirectDrawBackend();
        InstallDirectDrawFrameCounterHooks(*directDraw);
    }
    return result;
}

// Hooks the overlay's FPS counter into each DirectDraw object; only a failure is logged.
HRESULT WINAPI HookedDirectDrawCreateEx(const GUID* guid, void** directDraw,
        const GUID* interfaceId, IUnknown* outerUnknown) {
    HRESULT result = g_originalDirectDrawCreateEx(guid, directDraw, interfaceId,
        outerUnknown);
    if (FAILED(result)) LogLine("ERROR", "DirectDrawCreateEx failed: result=0x%08lX", result);
    if ((g_enableOverlay || g_uiQuadBlit) && SUCCEEDED(result) && directDraw && *directDraw) {
        IdentifyDirectDrawBackend();
        InstallDirectDrawFrameCounterHooks(*directDraw);
    }
    return result;
}


