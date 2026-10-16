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

static HRESULT WINAPI HookedD3DCreateDevice(void* d3d, const IID* iid, void* target, void** device) {
    const HRESULT result = g_originalD3DCreateDevice(d3d, iid, target, device);
    if (SUCCEEDED(result) && device && *device && !g_originalD3DLoad) {
        void* original = PatchVTableSlot(*device, D3DDEVICE7_VTABLE_LOAD_SLOT, reinterpret_cast<void*>(HookedD3DLoad));
        if (original) { g_originalD3DLoad = reinterpret_cast<D3DLoadFunction>(original); LogLine("INFO", "IDirect3DDevice7::Load hooked (the shadow lock cache)"); }
    }
    return result;
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
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
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

// Patch CreateSurface on a newly created DirectDraw object so the overlay can
// find and hook the primary surface once the game creates it.
static void InstallDirectDrawFrameCounterHooks(void* directDrawObject) {
    bool alreadyPatched = false;
    void* original = PatchVTableSlot(directDrawObject, DD_VTABLE_CREATESURFACE_SLOT,
        reinterpret_cast<void*>(HookedDDCreateSurface), &alreadyPatched);
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
    if (g_enableOverlay && SUCCEEDED(result) && directDraw && *directDraw) {
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
    if (g_enableOverlay && SUCCEEDED(result) && directDraw && *directDraw) {
        IdentifyDirectDrawBackend();
        InstallDirectDrawFrameCounterHooks(*directDraw);
    }
    return result;
}


