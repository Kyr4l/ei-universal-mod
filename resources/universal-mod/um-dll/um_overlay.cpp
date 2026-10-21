// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the overlay windows: the alpha canvas, the renderer chain, the panels, the window procedure.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// A CPU-side 32bpp ARGB bitmap composited onto its window via
// UpdateLayeredWindow, so each panel can have its own configurable
// background color/opacity while drawn content stays fully opaque/legible.
struct AlphaCanvas {
    HDC dc = NULL;
    HBITMAP bitmap = NULL;
    HBITMAP oldBitmap = NULL;
    void* pixels = NULL;
    int width = 0;
    int height = 0;
};

static AlphaCanvas g_overlayCanvas;
static AlphaCanvas g_overlayLogCanvas;
static AlphaCanvas g_profilerCanvas;

static void DestroyAlphaCanvas(AlphaCanvas& canvas) {
    if (canvas.dc) {
        SelectObject(canvas.dc, canvas.oldBitmap);
        DeleteDC(canvas.dc);
        canvas.dc = NULL;
    }
    if (canvas.bitmap) {
        DeleteObject(canvas.bitmap);
        canvas.bitmap = NULL;
    }
    canvas.pixels = NULL;
    canvas.width = 0;
    canvas.height = 0;
}

// (Re)allocate the canvas's backing DIB only when its size actually changes.
static void EnsureAlphaCanvas(AlphaCanvas& canvas, int width, int height) {
    if (canvas.dc && canvas.width == width && canvas.height == height) {
        return;
    }
    DestroyAlphaCanvas(canvas);

    HDC screenDC = GetDC(NULL);
    canvas.dc = CreateCompatibleDC(screenDC);
    ReleaseDC(NULL, screenDC);
    if (!canvas.dc) {
        LogLine("WARN", "[OVERLAY] CreateCompatibleDC for overlay canvas failed, error=%lu", GetLastError());
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // negative = top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    canvas.bitmap = CreateDIBSection(canvas.dc, &bmi, DIB_RGB_COLORS, &canvas.pixels, NULL, 0);
    if (!canvas.bitmap) {
        LogLine("WARN", "[OVERLAY] CreateDIBSection for overlay canvas failed, error=%lu", GetLastError());
        DeleteDC(canvas.dc);
        canvas.dc = NULL;
        return;
    }
    canvas.oldBitmap = static_cast<HBITMAP>(SelectObject(canvas.dc, canvas.bitmap));
    canvas.width = width;
    canvas.height = height;
}

// Fill every pixel with the panel's background color and alpha, before any
// GDI drawing happens; GDI operations never touch the alpha channel, which is
// what lets FinalizeCanvasAlpha() later tell "untouched background" pixels
// apart from "something was drawn here" pixels.
//
// When ditherStyle is true, alpha is not blended smoothly (per-pixel alpha
// blending is confirmed unsupported by some Wine/Wayland compositor stacks -
// see OVERLAY_TRANSPARENCY_STYLE); instead whole horizontal rows are made
// EITHER fully opaque OR fully transparent (alpha 0 or 255 only, never
// partial), in a repeating scanline band pattern whose density approximates
// the configured opacity. Bands (not a per-pixel checkerboard) are used
// deliberately: a per-pixel pattern was tried first and caused severe
// slowdowns, almost certainly because the compositor rebuilds an "opaque
// region" from the alpha channel on every UpdateLayeredWindow call, and a
// checkerboard turns that into thousands of tiny rectangles instead of a
// handful of full-width bands.
static const int OVERLAY_DITHER_BAND_PERIOD = 5;

// True if scanline row y should be fully opaque for the given opacity%, using
// a repeating OVERLAY_DITHER_BAND_PERIOD-row cycle.
static bool ShouldDitherRowBeOpaque(int y, int opacityPercent) {
    if (opacityPercent <= 0) {
        return false;
    }
    if (opacityPercent >= 100) {
        return true;
    }
    int opaqueRowsPerCycle = (opacityPercent * OVERLAY_DITHER_BAND_PERIOD) / 100;
    if (opaqueRowsPerCycle < 1) {
        opaqueRowsPerCycle = 1; // any nonzero opacity should show at least a faint band
    }
    return (y % OVERLAY_DITHER_BAND_PERIOD) < opaqueRowsPerCycle;
}

static void FillCanvasBackground(const AlphaCanvas& canvas, COLORREF color, BYTE alpha, bool ditherStyle) {
    if (!canvas.pixels) {
        return;
    }
    DWORD rgb = (static_cast<DWORD>(GetRValue(color)) << 16) |
        (static_cast<DWORD>(GetGValue(color)) << 8) | static_cast<DWORD>(GetBValue(color));
    DWORD opaquePacked = 0xFF000000 | rgb;
    // Alpha=0 with RGB still set to the background color (not zeroed) keeps
    // the "still matches background" equality check below working the same
    // way in both dither and smooth-alpha mode.
    DWORD transparentPacked = rgb;
    DWORD blendedPacked = (static_cast<DWORD>(alpha) << 24) | rgb;
    DWORD* pixels = static_cast<DWORD*>(canvas.pixels);
    if (!ditherStyle) {
        int count = canvas.width * canvas.height;
        for (int i = 0; i < count; ++i) {
            pixels[i] = blendedPacked;
        }
        return;
    }
    int opacityPercent = (static_cast<int>(alpha) * 100) / 255;
    for (int y = 0; y < canvas.height; ++y) {
        DWORD rowValue = ShouldDitherRowBeOpaque(y, opacityPercent) ? opaquePacked : transparentPacked;
        DWORD* row = pixels + static_cast<size_t>(y) * canvas.width;
        for (int x = 0; x < canvas.width; ++x) {
            row[x] = rowValue;
        }
    }
}

// Any pixel still exactly matching the background fill was never drawn on,
// so it keeps the alpha FillCanvasBackground already gave it; everything
// else GDI drew becomes fully opaque, since UpdateLayeredWindow requires
// premultiplied alpha (trivial here: alpha is always 0 or 255 in dither mode,
// and the smooth-alpha mode premultiplies explicitly below).
static void FinalizeCanvasAlpha(const AlphaCanvas& canvas, COLORREF color, BYTE alpha, bool ditherStyle) {
    if (!canvas.pixels) {
        return;
    }
    DWORD backgroundRgb = (static_cast<DWORD>(GetRValue(color)) << 16) |
        (static_cast<DWORD>(GetGValue(color)) << 8) | static_cast<DWORD>(GetBValue(color));
    BYTE premulR = static_cast<BYTE>(GetRValue(color) * alpha / 255);
    BYTE premulG = static_cast<BYTE>(GetGValue(color) * alpha / 255);
    BYTE premulB = static_cast<BYTE>(GetBValue(color) * alpha / 255);
    DWORD premulBackground = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(premulR) << 16) |
        (static_cast<DWORD>(premulG) << 8) | static_cast<DWORD>(premulB);

    DWORD* pixels = static_cast<DWORD*>(canvas.pixels);
    int count = canvas.width * canvas.height;
    for (int i = 0; i < count; ++i) {
        if ((pixels[i] & 0x00FFFFFF) == backgroundRgb) {
            if (!ditherStyle) {
                pixels[i] = premulBackground;
            }
            // In dither mode the pixel already holds its final 0x00000000 or
            // 0xFF<rgb> value from FillCanvasBackground; nothing more to do.
        } else {
            pixels[i] |= 0xFF000000;
        }
    }
}

// Push the finished canvas to the screen at the given top-left position.
#ifdef UM_TEST_DUMP_CANVAS
// Test builds only (compiled with -DUM_TEST_DUMP_CANVAS, never in the shipped
// DLL): write the panel's pixels to <UM_TEST_DUMP_CANVAS dir>\\overlay-main.bmp
// or overlay-log.bmp so a headless run under Wine can be inspected, since a
// plain popup window can't be captured back out of Wine.
static void DumpCanvasForTest(HWND hwnd, const AlphaCanvas& canvas) {
    char directory[MAX_PATH] = {};
    if (GetEnvironmentVariableA("UM_TEST_DUMP_CANVAS", directory, sizeof(directory)) == 0 || !canvas.pixels) {
        return;
    }
    char path[MAX_PATH + 32] = {};
    snprintf(path, sizeof(path), "%s\\%s", directory, hwnd == g_overlayLogWindow ? "overlay-log.bmp" :
        hwnd == g_profilerWindow ? "overlay-profiler.bmp" : "overlay-main.bmp");
    DIBSECTION section = {};
    if (GetObjectA(canvas.bitmap, sizeof(section), &section) == 0) {
        return;
    }
    BITMAPINFOHEADER header = section.dsBmih;
    DWORD imageBytes = static_cast<DWORD>(canvas.width) * canvas.height * 4;
    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
    FILE* file = fopen(path, "wb");
    if (file) {
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&header, sizeof(header), 1, file);
        fwrite(canvas.pixels, imageBytes, 1, file);
        fclose(file);
    }
}
#endif

static void CompositeCanvasToWindow(HWND hwnd, const AlphaCanvas& canvas, int x, int y) {
    if (!hwnd || !canvas.dc) {
        return;
    }
#ifdef UM_TEST_DUMP_CANVAS
    DumpCanvasForTest(hwnd, canvas);
#endif
    if (!g_overlayWindowsAreLayered) {
        // Fully opaque panel: plain BitBlt onto the window's own DC instead
        // of UpdateLayeredWindow, so the window is never WS_EX_LAYERED at
        // all (see g_overlayWindowsAreLayered for why that matters here).
        SetWindowPos(hwnd, NULL, x, y, canvas.width, canvas.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        HDC windowDC = GetDC(hwnd);
        if (windowDC) {
            BitBlt(windowDC, 0, 0, canvas.width, canvas.height, canvas.dc, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, windowDC);
        }
        return;
    }
    POINT srcPos = {0, 0};
    POINT dstPos = {x, y};
    SIZE size = {canvas.width, canvas.height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (!UpdateLayeredWindow(hwnd, NULL, &dstPos, &size, canvas.dc, &srcPos, 0, &blend, ULW_ALPHA)) {
        // Only the first failure is logged; if per-pixel alpha isn't honored
        // by the current Wine/compositor stack this would otherwise repeat
        // every refresh tick and flood um.log.
        static bool loggedFailure = false;
        if (!loggedFailure) {
            loggedFailure = true;
            LogLine("WARN", "UpdateLayeredWindow failed, error=%lu (background opacity will not apply)",
                GetLastError());
        }
    }
}


// ---------------------------------------------------------------------------
// Renderer chain
//
// "DirectDraw Backend" only reports the name the topmost DirectDraw driver
// gives itself, which hides everything underneath it (DxWrapper forwarding to
// dgVoodoo reads as just "dgVoodoo"). The real chain is worked out from which
// graphics DLLs are loaded, identifying each one by strings inside the file
// instead of its name, since ddraw.dll can be DxWrapper, dgVoodoo, D7VK or
// Wine's own depending on the setup.
// ---------------------------------------------------------------------------
enum ModuleFingerprint : unsigned {
    FP_DGVOODOO = 1, FP_DXWRAPPER = 2, FP_DXVK = 4, FP_D7VK = 8, FP_WINE_BUILTIN = 16
};

struct FingerprintCacheEntry {
    HMODULE module;
    unsigned flags;
};
static FingerprintCacheEntry g_fingerprintCache[64];
static int g_fingerprintCacheCount = 0;

static bool BufferContains(const std::vector<char>& data, const char* needle, size_t needleLength) {
    return std::search(data.begin(), data.end(),
        std::boyer_moore_horspool_searcher(needle, needle + needleLength)) != data.end();
}

// dgVoodoo and DxWrapper store their names as UTF-16 in their resources, the
// others as plain ASCII, so both spellings are searched.
static bool BufferContainsText(const std::vector<char>& data, const char* text) {
    size_t length = strlen(text);
    if (BufferContains(data, text, length)) {
        return true;
    }
    std::vector<char> wide(length * 2, '\0');
    for (size_t i = 0; i < length; ++i) {
        wide[i * 2] = text[i];
    }
    return BufferContains(data, wide.data(), wide.size());
}

static unsigned FingerprintModule(HMODULE module, const char* path) {
    for (int i = 0; i < g_fingerprintCacheCount; ++i) {
        if (g_fingerprintCache[i].module == module) {
            return g_fingerprintCache[i].flags;
        }
    }
    unsigned flags = 0;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(file, NULL);
        if (size != INVALID_FILE_SIZE && size > 0 && size <= 48u * 1024 * 1024) {
            std::vector<char> data(size);
            DWORD read = 0;
            if (ReadFile(file, data.data(), size, &read, NULL) && read == size) {
                if (BufferContainsText(data, "dgVoodoo")) flags |= FP_DGVOODOO;
                if (BufferContainsText(data, "DxWrapper") || BufferContainsText(data, "dxwrapper")) flags |= FP_DXWRAPPER;
                if (BufferContainsText(data, "DXVK") || BufferContainsText(data, "dxvk")) flags |= FP_DXVK;
                if (BufferContainsText(data, "D7VK") || BufferContainsText(data, "d7vk")) flags |= FP_D7VK;
                if (BufferContains(data, "Wine builtin DLL", 16)) flags |= FP_WINE_BUILTIN;
            }
        }
        CloseHandle(file);
    }
    if (g_fingerprintCacheCount < static_cast<int>(sizeof(g_fingerprintCache) / sizeof(g_fingerprintCache[0]))) {
        g_fingerprintCache[g_fingerprintCacheCount++] = FingerprintCacheEntry{ module, flags };
    }
    return flags;
}

// Human-readable chain such as "DxWrapper > dgVoodoo > DXVK > Vulkan", top of
// the stack (closest to the game) first; empty when no graphics DLL is loaded
// yet.
static void DetectRendererChain(char* out, size_t outSize) {
    out[0] = '\0';
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    bool dxWrapper = false, dgVoodoo = false, d7vk = false, wineDdraw = false, nativeDdraw = false;
    bool dxvk = false, wined3d = false, openGl = false, winevulkan = false;
    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32First(snapshot, &entry); ok; ok = Module32Next(snapshot, &entry)) {
        const char* name = entry.szModule;
        HMODULE module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
        if (EqualsIgnoreCase(name, "ddraw.dll")) {
            unsigned flags = FingerprintModule(module, entry.szExePath);
            if (flags & FP_DXWRAPPER) {
                dxWrapper = true;
            } else if (flags & FP_DGVOODOO) {
                dgVoodoo = true;
            } else if (flags & (FP_D7VK | FP_DXVK)) {
                d7vk = true;
            } else if (flags & FP_WINE_BUILTIN) {
                wineDdraw = true;
            } else {
                nativeDdraw = true;
            }
        } else if (EqualsIgnoreCase(name, "dxwrapper.dll")) {
            dxWrapper = true;
        } else if (EqualsIgnoreCase(name, "d3dimm.dll")) {
            if (FingerprintModule(module, entry.szExePath) & FP_DGVOODOO) {
                dgVoodoo = true;
            }
        } else if (EqualsIgnoreCase(name, "d3d9.dll") || EqualsIgnoreCase(name, "d3d11.dll") ||
                   EqualsIgnoreCase(name, "dxgi.dll")) {
            if (FingerprintModule(module, entry.szExePath) & FP_DXVK) {
                dxvk = true;
            }
        } else if (EqualsIgnoreCase(name, "wined3d.dll")) {
            wined3d = true;
        } else if (EqualsIgnoreCase(name, "opengl32.dll")) {
            openGl = true;
        } else if (EqualsIgnoreCase(name, "winevulkan.dll")) {
            winevulkan = true;
        }
    }
    CloseHandle(snapshot);

    // D7VK draws DirectDraw straight to Vulkan. Wine's own ddraw/wined3d/OpenGL can
    // still be loaded next to it (Wine loads them for other reasons) without ever
    // being on the game's rendering path, so listing them would be misleading.
    if (d7vk) {
        wineDdraw = false;
        wined3d = false;
        openGl = false;
    }

    const char* layers[8] = {};
    int layerCount = 0;
    if (dxWrapper) layers[layerCount++] = "DxWrapper";
    if (dgVoodoo) layers[layerCount++] = "dgVoodoo";
    if (d7vk) layers[layerCount++] = "D7VK";
    if (wineDdraw) layers[layerCount++] = "Wine ddraw";
    if (nativeDdraw) layers[layerCount++] = "unknown ddraw.dll";
    if (dxvk) layers[layerCount++] = "DXVK";
    if (wined3d) layers[layerCount++] = "wined3d";
    if (dxvk || (d7vk && winevulkan)) {
        layers[layerCount++] = "Vulkan";
    } else if (wined3d && openGl) {
        layers[layerCount++] = "OpenGL";
    }
    size_t used = 0;
    for (int i = 0; i < layerCount; ++i) {
        int wrote = snprintf(out + used, outSize - used, "%s%s", i ? " > " : "", layers[i]);
        if (wrote < 0 || static_cast<size_t>(wrote) >= outSize - used) {
            break;
        }
        used += static_cast<size_t>(wrote);
    }
}

// Re-detected at most every two seconds, from the overlay thread only (so it
// needs no locking), and logged whenever it changes so um.log records which
// renderer stack a session actually ran on.
static char g_rendererChain[192] = "";
static ULONGLONG g_rendererChainCheckedTickMs = 0;

static void RefreshRendererChain() {
    ULONGLONG now = GetTickCount64();
    if (g_rendererChainCheckedTickMs != 0 && now - g_rendererChainCheckedTickMs < 2000) {
        return;
    }
    g_rendererChainCheckedTickMs = now;
    char chain[sizeof(g_rendererChain)] = {};
    DetectRendererChain(chain, sizeof(chain));
    if (chain[0] != '\0' && strcmp(chain, g_rendererChain) != 0) {
        snprintf(g_rendererChain, sizeof(g_rendererChain), "%s", chain);
        LogLine("INFO", "Renderer chain: %s", g_rendererChain);
    }
}

static void BuildOverlayContent(OverlayContent& content) {
    const int topCapacity = static_cast<int>(sizeof(content.top) / sizeof(content.top[0]));
    const int bottomCapacity = static_cast<int>(sizeof(content.bottom) / sizeof(content.bottom[0]));
    content.compact = g_overlayCompact;
    content.hasTitle = !content.compact;
    content.showGraph = !content.compact && g_overlayShowFpsGraph;
    content.showThreads = !content.compact && g_overlayShowThreads && g_overlayThreadSampleCount > 0;

    if (content.hasTitle) {
        AddOverlayLine(content.top, &content.topCount, topCapacity, false, "Universal Mod Library v%s", UM_VERSION);
    }

    LONG warnings = g_logWarningCount;
    LONG errors = g_logErrorCount;
    if (g_overlayShowWarnings && (warnings > 0 || errors > 0)) {
        AddOverlayLine(content.top, &content.topCount, topCapacity, true, "! %ld warning%s, %ld error%s logged",
            warnings, warnings == 1 ? "" : "s", errors, errors == 1 ? "" : "s");
    }

    if (!content.compact && g_overlayShowResources) {
        AddOverlayLine(content.top, &content.topCount, topCapacity, false, "CPU=%.1f%% Mem=%.1fMB",
            g_overlayCpuPercent, g_overlayWorkingSetMb);
    }

    if (g_overlayFrameCounterActive) {
        AddOverlayLine(content.top, &content.topCount, topCapacity, false, "FPS=%.1f FrameTime=%.2fms",
            g_overlayCurrentFps, g_overlayCurrentFrameTimeMs);
    } else {
        AddOverlayLine(content.top, &content.topCount, topCapacity, false, "FPS=pending (waiting for primary surface)");
    }
    double onePercentLow = 0.0, worstFrameMs = 0.0, windowSeconds = 0.0;
    if (!content.compact && g_overlayShowFrameStats && g_overlayFrameCounterActive &&
            ComputeFrameStats(&onePercentLow, &worstFrameMs, &windowSeconds)) {
        AddOverlayLine(content.top, &content.topCount, topCapacity, false,
            "1%% low=%.1f Worst=%.1fms (%.0fs)", onePercentLow, worstFrameMs, windowSeconds);
    }

    if (content.compact) {
        return;
    }

    // Bottom block (drawn after the FPS graph).
    if (g_overlayShowLaa) {
        // Confirms whether this specific running game.exe was patched with the
        // LARGE_ADDRESS_AWARE bit (see _cpr/game-exe-laa-patch), read straight
        // from its own in-memory PE header, not assumed.
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
            IsCurrentProcessLargeAddressAware() ? "LAA=yes (>2GB address space)" : "LAA=no (capped at 2GB address space)");
    }
    if (g_overlayShowResources) {
        // A 32-bit process runs out of address space long before it runs out
        // of RAM, so this is the number to watch for out-of-memory crashes.
        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory) && memory.ullTotalVirtual > 0) {
            double totalGb = memory.ullTotalVirtual / 1073741824.0;
            double usedGb = (memory.ullTotalVirtual - memory.ullAvailVirtual) / 1073741824.0;
            AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity,
                usedGb >= totalGb * OVERLAY_ADDRESS_SPACE_ALERT_FRACTION,
                "AddrSpace=%.2f/%.2fGB used", usedGb, totalGb);
        }
    }
    unsigned long quarantineProblems = 0, quarantineOverruns = 0;
    if (GetQuarantineSnapshot(&quarantineProblems, &quarantineOverruns)) {
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, quarantineProblems > 0,
            "heap-problems=%lu overruns-absorbed=%lu", quarantineProblems, quarantineOverruns);
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false, "%s", ""); // blank line
    }
    if (g_heapPaddingOnlyInstalled) {
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
            "Heap padding=%d bytes (not tracked)", g_heapAllocPadding);
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false, "%s", ""); // blank line
    }
    if (g_overlayShowBackend) {
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
            "DirectDraw Backend=%s", g_directDrawBackendName);
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
            "Renderer=%s", g_rendererChain[0] ? g_rendererChain : "detecting...");
    }
    if (g_overlayShowMap) {
        char sessionText[16] = {};
        FormatDuration(GetProcessUptimeSeconds(), sessionText, sizeof(sessionText));
        MapLoadState map = {};
        if (CopyMapState(&map)) {
            char ageText[16] = {};
            FormatDuration((GetTickCount64() - map.openedTickMs) / 1000, ageText, sizeof(ageText));
            const char* title = map.mpr[0] ? map.mpr : map.quest[0] ? map.quest : map.base;
            AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
                "Map=%s (%s ago) Session=%s", title, ageText, sessionText);
            if (map.base[0] || map.quest[0]) {
                // Only what is not already the title above.
                char parts[160] = {};
                size_t used = 0;
                auto append = [&](const char* label, const char* value) {
                    if (!value[0] || (label[0] == 'B' && !map.quest[0] && !map.mpr[0]) ||
                            (label[0] == 'Q' && !map.mpr[0])) {
                        return;
                    }
                    int wrote = snprintf(parts + used, sizeof(parts) - used, "%s%s=%s", used ? " " : "", label, value);
                    if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(parts) - used) used += static_cast<size_t>(wrote);
                };
                append("Base", map.base);
                append("Quest", map.quest);
                if (used) {
                    AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false, "%s", parts);
                }
            }
            if (map.extraCount > 0) {
                AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
                    "Script maps=%d (last %s)", map.extraCount, map.lastExtra);
            }
        } else {
            AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, false,
                "Map=(none yet) Session=%s", sessionText);
        }
    }
}

// Per-thread CPU%% breakdown rows drawn below the FPS graph, and the graph box
// and its time scale, use these heights.
static const int OVERLAY_THREAD_LINE_HEIGHT = 16;
// Room below the graph box for the "-Ns" / "now" time-scale labels.
static const int OVERLAY_GRAPH_TIME_SCALE_HEIGHT = 16;
// Half a text line between the "FPS Graph (0-N)" title and the graph box, so
// the top FPS mark's label (drawn just above its line, which can sit at the
// very top edge) doesn't collide with the title.
static const int OVERLAY_GRAPH_TITLE_GAP = 10;

// Panel height depends on which rows and sections are enabled in um.cfg, so it
// is computed from the content rather than being a fixed constant. Layout, top
// to bottom: title/warnings/FPS block, FPS graph, then the LAA/address space/
// quarantine/backend/map block, then the optional per-thread breakdown.
static int ComputeOverlayPanelHeight(const OverlayContent& content) {
    // +20 once for the blank line below the title.
    int height = 8 + content.topCount * 20 + (content.hasTitle ? 20 : 0);
    if (content.compact) {
        return height + 10;
    }
    height += OVERLAY_SECTION_GAP;
    if (content.showGraph) {
        // +20 for the graph title row, +20 again for the blank line below the
        // graph's time scale.
        height += 20 + OVERLAY_GRAPH_TITLE_GAP + OVERLAY_GRAPH_HEIGHT + OVERLAY_GRAPH_TIME_SCALE_HEIGHT +
            OVERLAY_SECTION_GAP + 20;
    }
    height += content.bottomCount * 20;
    if (content.showThreads) {
        height += OVERLAY_SECTION_GAP + 20 + g_overlayThreadSampleCount * OVERLAY_THREAD_LINE_HEIGHT;
    }
    return height + 10;
}

// Static FPS reference marks, configurable via OVERLAY_FPS_MARKS; the lowest
// two are always shown, the rest only appear once the game actually reaches
// them (see ComputeRevealedMarkCount).

// How many of the leading (lowest) marks are relevant given the highest FPS
// ever observed this session: always at least the first two, more once
// actually reached.
static int ComputeRevealedMarkCount(double bestFpsEver) {
    int revealed = g_overlayFpsMarkCount > 1 ? 2 : g_overlayFpsMarkCount;
    for (int i = 2; i < g_overlayFpsMarkCount; ++i) {
        if (bestFpsEver >= g_overlayFpsMarks[i]) {
            revealed = i + 1;
        }
    }
    return revealed;
}

// Smallest FPS mark that is >= bestFpsEver, floored at the second-lowest mark
// and capped at the highest defined mark so the FPS graph's scale grows with
// real headroom.
static double ComputeFpsCeiling(double bestFpsEver) {
    double floorMark = g_overlayFpsMarkCount > 1 ? g_overlayFpsMarks[1] :
        (g_overlayFpsMarkCount > 0 ? g_overlayFpsMarks[0] : 60.0);
    if (bestFpsEver <= floorMark) {
        return floorMark;
    }
    for (int i = 0; i < g_overlayFpsMarkCount; ++i) {
        if (g_overlayFpsMarks[i] >= bestFpsEver) {
            return g_overlayFpsMarks[i];
        }
    }
    return g_overlayFpsMarkCount > 0 ? g_overlayFpsMarks[g_overlayFpsMarkCount - 1] : floorMark;
}

// Anchor a fixed-size panel to a corner/edge/center of the target window based
// on an OVERLAY_POSITION value such as "top-left", "bottom", or "right".
static void ComputeOverlayRect(const RECT& targetRect, const char* position,
        int panelWidth, int panelHeight, RECT& outRect) {
    bool top = strstr(position, "top") != NULL;
    bool bottom = strstr(position, "bottom") != NULL;
    bool left = strstr(position, "left") != NULL;
    bool right = strstr(position, "right") != NULL;

    int targetWidth = targetRect.right - targetRect.left;
    int targetHeight = targetRect.bottom - targetRect.top;

    int x;
    if (left) {
        x = targetRect.left + OVERLAY_PANEL_MARGIN;
    } else if (right) {
        x = targetRect.right - OVERLAY_PANEL_MARGIN - panelWidth;
    } else {
        x = targetRect.left + (targetWidth - panelWidth) / 2;
    }

    int y;
    if (top) {
        y = targetRect.top + OVERLAY_PANEL_MARGIN;
    } else if (bottom) {
        y = targetRect.bottom - OVERLAY_PANEL_MARGIN - panelHeight;
    } else {
        y = targetRect.top + (targetHeight - panelHeight) / 2;
    }

    outRect.left = x;
    outRect.top = y;
    outRect.right = x + panelWidth;
    outRect.bottom = y + panelHeight;
}

// Draw one bordered, dynamically-scaled rolling sparkline graph (oldest sample
// on the left, newest on the right) from a circular history buffer, with
// horizontal reference lines at markValues[0..markCount-1] (already expressed
// in the graph's own unit - FPS or ms).
static void DrawSparklineGraph(HDC hdc, const RECT& graphRect, const char* label,
        const double* history, int historyNext, int historyCount, double maxScale,
        const double* markValues, const char* const* markLabels, int markCount,
        double timeSpanSeconds) {
    char labelText[48] = {};
    snprintf(labelText, sizeof(labelText), "%s (0-%.0f)", label, maxScale);
    TextOutA(hdc, graphRect.left, graphRect.top - 20 - OVERLAY_GRAPH_TITLE_GAP, labelText, static_cast<int>(strlen(labelText)));

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_overlayTextColor);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    HBRUSH nullBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, nullBrush));
    Rectangle(hdc, graphRect.left, graphRect.top, graphRect.right, graphRect.bottom);

    // One dotted reference line + label per revealed static mark (e.g. 30/60 FPS).
    HPEN gridPen = CreatePen(PS_DOT, 1, g_overlayTextColor);
    HPEN oldGridPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
    int graphHeight = graphRect.bottom - graphRect.top;
    for (int i = 0; i < markCount; ++i) {
        double markValue = markValues[i];
        if (markValue <= 0 || markValue > maxScale) {
            continue;
        }
        int lineY = graphRect.bottom - static_cast<int>((markValue / maxScale) * graphHeight);
        MoveToEx(hdc, graphRect.left, lineY, NULL);
        LineTo(hdc, graphRect.right, lineY);
        TextOutA(hdc, graphRect.left + 2, lineY - 13, markLabels[i], static_cast<int>(strlen(markLabels[i])));
    }
    SelectObject(hdc, oldGridPen);
    DeleteObject(gridPen);
    TextOutA(hdc, graphRect.left + 2, graphRect.bottom - 15, "0", 1);

    if (historyCount > 1) {
        int graphWidth = graphRect.right - graphRect.left;
        POINT points[OVERLAY_HISTORY_CAPACITY];
        for (int i = 0; i < historyCount; ++i) {
            int historyIndex = (historyNext - historyCount + i + OVERLAY_HISTORY_CAPACITY * 2) %
                OVERLAY_HISTORY_CAPACITY;
            double sample = history[historyIndex];
            if (sample > maxScale) {
                sample = maxScale;
            } else if (sample < 0) {
                sample = 0;
            }
            points[i].x = graphRect.left + (graphWidth * i) / (historyCount - 1);
            points[i].y = graphRect.bottom - static_cast<int>((sample / maxScale) * graphHeight);
        }
        HPEN linePen = CreatePen(PS_SOLID, 2, g_overlayTextColor);
        HPEN oldLinePen = static_cast<HPEN>(SelectObject(hdc, linePen));
        Polyline(hdc, points, historyCount);
        SelectObject(hdc, oldLinePen);
        DeleteObject(linePen);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    // Time scale under the graph: oldest sample on the left, newest ("now")
    // on the right, matching the left-to-right sample order drawn above.
    char oldestLabel[16] = {};
    snprintf(oldestLabel, sizeof(oldestLabel), "-%.0fs", timeSpanSeconds);
    TextOutA(hdc, graphRect.left, graphRect.bottom + 4, oldestLabel, static_cast<int>(strlen(oldestLabel)));
    TextOutA(hdc, graphRect.right - 24, graphRect.bottom + 4, "now", 3);
}

// Render the main diagnostic panel (config status, FPS graph) into its alpha
// canvas and push it to the screen at its configured anchor position.
static void RenderOverlayPanel(const RECT& targetRect) {
    OverlayContent content;
    BuildOverlayContent(content);
    int panelHeight = ComputeOverlayPanelHeight(content);

    HFONT font = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    // Built separately (drawn below the bottom block, at the very bottom), but
    // still folded into the width measurement below so long thread names/CPU%%
    // don't get clipped either.
    char threadSectionLines[OVERLAY_THREAD_DISPLAY_MAX + 1][64] = {};
    int threadSectionLineCount = 0;
    if (content.showThreads) {
        snprintf(threadSectionLines[threadSectionLineCount++], sizeof(threadSectionLines[0]),
            "Threads (lowest %d TIDs, %d total)", g_overlayThreadSampleCount, g_overlayThreadCount);
        for (int i = 0; i < g_overlayThreadSampleCount; ++i) {
            snprintf(threadSectionLines[threadSectionLineCount++], sizeof(threadSectionLines[0]),
                "TID %lu %s: CPU=%.1f%%", static_cast<unsigned long>(g_overlayThreadSamples[i].threadId),
                g_overlayThreadSamples[i].name, g_overlayThreadSamples[i].cpuPercent);
        }
    }

    // Measure with a scratch DC (independent of the real canvas, which isn't
    // sized yet) so the panel is always wide enough to avoid clipping text.
    int panelWidth = content.compact ? 0 : OVERLAY_PANEL_WIDTH;
    HDC scratchDC = CreateCompatibleDC(NULL);
    if (scratchDC) {
        HFONT oldScratchFont = font ? static_cast<HFONT>(SelectObject(scratchDC, font)) : NULL;
        int maxTextWidth = 0;
        for (int i = 0; i < content.topCount; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, content.top[i].text,
                    static_cast<int>(strlen(content.top[i].text)), &textSize) &&
                    textSize.cx > maxTextWidth) {
                maxTextWidth = textSize.cx;
            }
        }
        for (int i = 0; i < content.bottomCount; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, content.bottom[i].text,
                    static_cast<int>(strlen(content.bottom[i].text)), &textSize) &&
                    textSize.cx > maxTextWidth) {
                maxTextWidth = textSize.cx;
            }
        }
        for (int i = 0; i < threadSectionLineCount; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, threadSectionLines[i],
                    static_cast<int>(strlen(threadSectionLines[i])), &textSize) &&
                    textSize.cx > maxTextWidth) {
                maxTextWidth = textSize.cx;
            }
        }
        if (oldScratchFont) {
            SelectObject(scratchDC, oldScratchFont);
        }
        DeleteDC(scratchDC);
        int neededWidth = maxTextWidth + 16;
        if (neededWidth > panelWidth) {
            panelWidth = neededWidth;
        }
    }

    RECT overlayRect;
    ComputeOverlayRect(targetRect, g_overlayPosition, panelWidth, panelHeight, overlayRect);

    EnsureAlphaCanvas(g_overlayCanvas, panelWidth, panelHeight);
    BYTE bgAlpha = static_cast<BYTE>((g_overlayBackgroundOpacityPercent * 255) / 100);
    bool ditherStyle = EqualsIgnoreCase(g_overlayTransparencyStyle, "dither");
    FillCanvasBackground(g_overlayCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);

    HDC hdc = g_overlayCanvas.dc;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_overlayTextColor);
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(hdc, font)) : NULL;

    // Scale and which static marks are shown both grow with the highest FPS
    // ever reached this session (e.g. a 60 FPS game never reveals the 120 FPS mark).
    double fpsCeiling = ComputeFpsCeiling(g_overlayFpsPeakEver);
    int revealedMarks = ComputeRevealedMarkCount(g_overlayFpsPeakEver);

    char fpsMarkLabels[OVERLAY_FPS_MARKS_MAX][8] = {};
    const char* fpsMarkLabelPtrs[OVERLAY_FPS_MARKS_MAX] = {};
    for (int i = 0; i < revealedMarks; ++i) {
        snprintf(fpsMarkLabels[i], sizeof(fpsMarkLabels[i]), "%.0f", g_overlayFpsMarks[i]);
        fpsMarkLabelPtrs[i] = fpsMarkLabels[i];
    }

    int y = 8;
    for (int i = 0; i < content.topCount; ++i) {
        SetTextColor(hdc, content.top[i].alert ? OVERLAY_ALERT_COLOR : g_overlayTextColor);
        TextOutA(hdc, 8, y, content.top[i].text, static_cast<int>(strlen(content.top[i].text)));
        y += 20;
        if (i == 0 && content.hasTitle) {
            y += 20; // blank line below the title
        }
    }
    SetTextColor(hdc, g_overlayTextColor);

    if (!content.compact) {
        y += OVERLAY_SECTION_GAP;

        if (content.showGraph) {
            y += 20 + OVERLAY_GRAPH_TITLE_GAP; // was 40; one blank line removed here below FPS/FrameTime
            RECT fpsGraphRect = {8, y, panelWidth - 8, y + OVERLAY_GRAPH_HEIGHT};
            double timeSpanSeconds = static_cast<double>(g_overlayFpsHistoryCount) * g_overlayRefreshMs / 1000.0;
            DrawSparklineGraph(hdc, fpsGraphRect, "FPS Graph", g_overlayFpsHistory,
                g_overlayFpsHistoryNext, g_overlayFpsHistoryCount, fpsCeiling,
                g_overlayFpsMarks, fpsMarkLabelPtrs, revealedMarks, timeSpanSeconds);
            y += OVERLAY_GRAPH_HEIGHT + OVERLAY_GRAPH_TIME_SCALE_HEIGHT + OVERLAY_SECTION_GAP;
            y += 20; // blank line below the graph's time scale
        }

        for (int i = 0; i < content.bottomCount; ++i) {
            SetTextColor(hdc, content.bottom[i].alert ? OVERLAY_ALERT_COLOR : g_overlayTextColor);
            TextOutA(hdc, 8, y, content.bottom[i].text, static_cast<int>(strlen(content.bottom[i].text)));
            y += 20;
        }
        SetTextColor(hdc, g_overlayTextColor);

        if (threadSectionLineCount > 0) {
            y += OVERLAY_SECTION_GAP; // skip a line before the thread breakdown
            for (int i = 0; i < threadSectionLineCount; ++i) {
                TextOutA(hdc, 8, y, threadSectionLines[i], static_cast<int>(strlen(threadSectionLines[i])));
                y += OVERLAY_THREAD_LINE_HEIGHT;
            }
        }
    } // !content.compact

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    if (font) {
        DeleteObject(font);
    }

    FinalizeCanvasAlpha(g_overlayCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);
    CompositeCanvasToWindow(g_overlayWindow, g_overlayCanvas, overlayRect.left, overlayRect.top);
}

// Render the auto-scrolling log tail panel: newest line pinned to the bottom,
// older lines pushed upward as new ones arrive, oldest falling off the top.

// Skip the leading "[YYYY-MM-DDTHH:MM:SS+ZZZZ] " timestamp so the overlay's
// narrow panel has room for the actually useful level/category/message text;
// um.log on disk keeps the full timestamp regardless.
static const char* SkipLogTimestamp(const char* line) {
    if (line[0] != '[') {
        return line;
    }
    const char* closeBracket = strchr(line, ']');
    if (!closeBracket) {
        return line;
    }
    const char* rest = closeBracket + 1;
    while (*rest == ' ') {
        ++rest;
    }
    return rest;
}

static void RenderLogPanel(const RECT& targetRect) {
    int lineCount = g_overlayLogLineCount;
    int panelHeight = 8 + lineCount * OVERLAY_LOG_LINE_HEIGHT + 6;
    RECT logRect;
    ComputeOverlayRect(targetRect, g_overlayLogPosition, g_overlayLogPanelWidth, panelHeight, logRect);

    EnsureAlphaCanvas(g_overlayLogCanvas, g_overlayLogPanelWidth, panelHeight);
    BYTE bgAlpha = static_cast<BYTE>((g_overlayBackgroundOpacityPercent * 255) / 100);
    bool ditherStyle = EqualsIgnoreCase(g_overlayTransparencyStyle, "dither");
    FillCanvasBackground(g_overlayLogCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);

    HDC hdc = g_overlayLogCanvas.dc;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_overlayTextColor);
    HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(hdc, font)) : NULL;

    // Copy every buffered entry (not just `lineCount` of them) so wrapping a
    // long entry into extra rows still leaves enough source material to
    // fill the panel; the tail of the resulting row list is taken below.
    // Static (only the overlay thread renders the log panel) so these large buffers stay off its stack.
    static char snapshot[OVERLAY_LOG_CAPACITY][OVERLAY_LOG_ENTRY_SIZE];
    int snapshotCount = 0;
    if (g_logLockInitialized) {
        EnterCriticalSection(&g_logLock);
    }
    int available = g_overlayLogRingCount;
    for (int i = 0; i < available; ++i) {
        int idx = (g_overlayLogRingNext - available + i + OVERLAY_LOG_CAPACITY * 2) % OVERLAY_LOG_CAPACITY;
        snprintf(snapshot[snapshotCount], sizeof(snapshot[0]), "%s", SkipLogTimestamp(g_overlayLogRing[idx]));
        ++snapshotCount;
    }
    if (g_logLockInitialized) {
        LeaveCriticalSection(&g_logLock);
    }

    // Word-wrap (falling back to a hard break) any entry too wide for the
    // panel into extra visual rows, oldest first; disabled entries are just
    // left as a single (possibly clipped) row instead.
    //
    // This used to estimate capacity as (panelWidth / tmAveCharWidth) - looks
    // reasonable, but tmAveCharWidth is a generic-English-prose average, and
    // "Consolas" isn't actually installed under Wine: GDI silently
    // substitutes a proportional font (Liberation Sans, measured here), whose
    // average glyph is noticeably WIDER than the digits/brackets/path
    // separators that dominate real log lines. That mismatch made every row
    // wrap ~15-20% earlier than the panel could actually fit, which is
    // exactly the large black margin in the screenshot. Measuring the real
    // pixel width via GetTextExtentExPointA (which exists specifically to
    // answer "how many characters of this string fit in N pixels") is
    // correct regardless of which font ends up selected.
    int maxRowWidthPx = g_overlayLogWrapEnabled ? (g_overlayLogPanelWidth - 12) : 0;

    static const int OVERLAY_LOG_MAX_VISUAL_ROWS = OVERLAY_LOG_CAPACITY * 4;
    // A wrapped row holds however many characters fit the panel width (up to ~400 for the
    // widest panel with a narrow font), so it must be as large as a stored log entry -
    // 160 here cut every long row short and silently dropped the text after the cut.
    static char rows[OVERLAY_LOG_MAX_VISUAL_ROWS][OVERLAY_LOG_ENTRY_SIZE];
    int rowCount = 0;
    for (int i = 0; i < snapshotCount && rowCount < OVERLAY_LOG_MAX_VISUAL_ROWS; ++i) {
        const char* text = snapshot[i];
        size_t textLen = strlen(text);
        SIZE fullExtent = {};
        if (maxRowWidthPx > 0) {
            GetTextExtentPoint32A(hdc, text, static_cast<int>(textLen), &fullExtent);
        }
        if (maxRowWidthPx <= 0 || fullExtent.cx <= maxRowWidthPx) {
            snprintf(rows[rowCount++], sizeof(rows[0]), "%s", text);
            continue;
        }
        size_t pos = 0;
        while (pos < textLen && rowCount < OVERLAY_LOG_MAX_VISUAL_ROWS) {
            size_t remaining = textLen - pos;
            int fitChars = 0;
            SIZE chunkExtent = {};
            GetTextExtentExPointA(hdc, text + pos, static_cast<int>(remaining), maxRowWidthPx,
                &fitChars, NULL, &chunkExtent);
            // Always make progress, even if a single glyph is wider than the row.
            size_t chunkLen = fitChars > 0 ? static_cast<size_t>(fitChars) : 1;
            if (chunkLen > remaining) {
                chunkLen = remaining;
            }
            size_t breakAt = chunkLen;
            if (chunkLen < remaining) {
                // Prefer breaking on the last space in this chunk (if any,
                // and not absurdly early) over splitting a word in half.
                for (size_t k = chunkLen; k > chunkLen / 3; --k) {
                    if (text[pos + k - 1] == ' ') {
                        breakAt = k;
                        break;
                    }
                }
            }
            size_t copyLen = breakAt < sizeof(rows[0]) - 1 ? breakAt : sizeof(rows[0]) - 1;
            memcpy(rows[rowCount], text + pos, copyLen);
            rows[rowCount][copyLen] = '\0';
            ++rowCount;
            pos += breakAt;
            while (pos < textLen && text[pos] == ' ') {
                ++pos; // skip the space that was broken on
            }
        }
    }

    int toShow = rowCount < lineCount ? rowCount : lineCount;
    int startIndex = rowCount - toShow;
    int y = 4 + (lineCount - toShow) * OVERLAY_LOG_LINE_HEIGHT;
    for (int i = 0; i < toShow; ++i) {
        const char* row = rows[startIndex + i];
        TextOutA(hdc, 6, y, row, static_cast<int>(strlen(row)));
        y += OVERLAY_LOG_LINE_HEIGHT;
    }

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    if (font) {
        DeleteObject(font);
    }

    FinalizeCanvasAlpha(g_overlayLogCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);
    CompositeCanvasToWindow(g_overlayLogWindow, g_overlayLogCanvas, logRect.left, logRect.top);
}

// The profiler window: the call tree of the main thread, on its own so it can sit beside the main panel.
static void RenderProfilerPanel(const RECT& targetRect) {
    static char lines[kProfileMaxLines + 1][kProfileTextWidth];
    int count = 0;
    snprintf(lines[count++], kProfileTextWidth, "Main thread profile (ms per frame)");
    while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
    for (int i = 0; i < g_profileLineCount && count <= kProfileMaxLines; ++i) {
        snprintf(lines[count++], kProfileTextWidth, "%s", g_profileLines[i]);
    }
    InterlockedExchange(&g_profileLock, 0);
    if (count == 1) {
        snprintf(lines[count++], kProfileTextWidth, "collecting samples...");
    }

    HFONT font = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    int panelWidth = 320;
    HDC scratchDC = CreateCompatibleDC(NULL);
    if (scratchDC) {
        HFONT oldScratchFont = font ? static_cast<HFONT>(SelectObject(scratchDC, font)) : NULL;
        for (int i = 0; i < count; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, lines[i], static_cast<int>(strlen(lines[i])), &textSize) &&
                    textSize.cx + 16 > panelWidth) {
                panelWidth = textSize.cx + 16;
            }
        }
        if (oldScratchFont) SelectObject(scratchDC, oldScratchFont);
        DeleteDC(scratchDC);
    }
    const int panelHeight = 8 + count * OVERLAY_THREAD_LINE_HEIGHT + 8;
    RECT rect;
    ComputeOverlayRect(targetRect, g_profilerPosition, panelWidth, panelHeight, rect);

    EnsureAlphaCanvas(g_profilerCanvas, panelWidth, panelHeight);
    BYTE bgAlpha = static_cast<BYTE>((g_overlayBackgroundOpacityPercent * 255) / 100);
    bool ditherStyle = EqualsIgnoreCase(g_overlayTransparencyStyle, "dither");
    FillCanvasBackground(g_profilerCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);
    HDC hdc = g_profilerCanvas.dc;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_overlayTextColor);
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(hdc, font)) : NULL;
    int y = 8;
    for (int i = 0; i < count; ++i) {
        TextOutA(hdc, 8, y, lines[i], static_cast<int>(strlen(lines[i])));
        y += OVERLAY_THREAD_LINE_HEIGHT;
    }
    if (oldFont) SelectObject(hdc, oldFont);
    if (font) DeleteObject(font);
    FinalizeCanvasAlpha(g_profilerCanvas, g_overlayBackgroundColor, bgAlpha, ditherStyle);
    CompositeCanvasToWindow(g_profilerWindow, g_profilerCanvas, rect.left, rect.top);
}

// Follow the game window, handle input, and drive both panels' rendering.
static LRESULT CALLBACK OverlayWindowProcBody(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        return OverlayWindowProcBody(hwnd, message, wParam, lParam);
    } catch (...) {
        return DefWindowProcA(hwnd, message, wParam, lParam); // an exception must not unwind through user32
    }
}

static LRESULT CALLBACK OverlayWindowProcBody(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER: {
        if (g_overlayTargetWindow && !IsWindow(g_overlayTargetWindow)) {
            g_overlayTargetWindow = NULL;
        }
        if (!g_overlayTargetWindow) {
            g_overlayTargetWindow = FindGameWindow();
        }

        ULONGLONG nowMs = GetTickCount64();
        ULONGLONG elapsedMs = nowMs - g_overlayFpsLastTickMs;
        if (g_overlayFrameCounterActive && elapsedMs >= static_cast<ULONGLONG>(g_overlayRefreshMs)) {
            LONG currentFrames = g_overlayFrameCount;
            LONG deltaFrames = currentFrames - g_overlayFpsLastFrameCount;
            g_overlayCurrentFps = deltaFrames * 1000.0 / static_cast<double>(elapsedMs);
            g_overlayCurrentFrameTimeMs = g_overlayCurrentFps > 0.0 ? 1000.0 / g_overlayCurrentFps : 0.0;
            g_overlayFpsLastFrameCount = currentFrames;
            g_overlayFpsLastTickMs = nowMs;
            if (g_overlayCurrentFps > g_overlayFpsPeakEver) {
                g_overlayFpsPeakEver = g_overlayCurrentFps;
            }

            size_t historyCapacity = static_cast<size_t>(OVERLAY_HISTORY_CAPACITY);
            g_overlayFpsHistory[g_overlayFpsHistoryNext] = g_overlayCurrentFps;
            g_overlayFpsHistoryNext = static_cast<int>((g_overlayFpsHistoryNext + 1) % historyCapacity);
            if (g_overlayFpsHistoryCount < static_cast<int>(historyCapacity)) {
                ++g_overlayFpsHistoryCount;
            }
        }

        if (g_overlayShowResources && (g_overlayVisible || g_overlayLogVisible)) {
            SampleProcessDiagnostics();
        }
        RefreshRendererChain();
        UpdateProfilerState();

        RECT targetRect;
        bool haveTargetRect = g_overlayTargetWindow && GetWindowRect(g_overlayTargetWindow, &targetRect);
        if (g_overlayVisible) {
            if (haveTargetRect) {
                RenderOverlayPanel(targetRect);
            }
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (g_overlayLogEnabled && g_overlayLogVisible && g_overlayLogWindow) {
            if (haveTargetRect) {
                RenderLogPanel(targetRect);
            }
            SetWindowPos(g_overlayLogWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (g_profilerEnabled && g_profilerVisible && g_profilerWindow) {
            if (haveTargetRect) {
                RenderProfilerPanel(targetRect);
            }
            SetWindowPos(g_profilerWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_PAINT: {
        // Only reached in the non-layered (fully opaque) path; layered
        // windows are painted exclusively through UpdateLayeredWindow and
        // don't normally receive WM_PAINT. Re-blit the last rendered canvas
        // instead of leaving the window blank until the next WM_TIMER tick.
        PAINTSTRUCT paintStruct;
        HDC paintDC = BeginPaint(hwnd, &paintStruct);
        const AlphaCanvas& canvas = (hwnd == g_overlayLogWindow) ? g_overlayLogCanvas :
            (hwnd == g_profilerWindow) ? g_profilerCanvas : g_overlayCanvas;
        if (paintDC && canvas.dc) {
            BitBlt(paintDC, 0, 0, canvas.width, canvas.height, canvas.dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &paintStruct);
        return 0;
    }
    case WM_ERASEBKGND:
        // WM_PAINT above (or UpdateLayeredWindow) always repaints every
        // pixel, so a separate erase would only cause flicker.
        return 1;
    case WM_MOUSEACTIVATE:
        // Belt-and-suspenders alongside WS_EX_NOACTIVATE: never let a click
        // on the overlay activate this window, which would steal foreground
        // focus from the fullscreen game and cause it to minimize.
        return MA_NOACTIVATE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wParam, lParam);
    }
}

// Create the click-through, always-on-top diagnostic overlay (plus its
// optional scrolling log panel) and run their message loop. Each panel is a
// composited alpha-blended bitmap rather than a color-keyed window, so its
// own configurable background color/opacity shows behind fully-opaque text;
// this works regardless of the active render backend (native DDraw,
// dgVoodoo2, a DDraw-to-D3D9 wrapper, DXVK, etc.) since none of those are
// touched by this approach. A separate, narrowly-scoped vtable hook on the
// primary surface's Flip/Blt is used only for the FPS/frametime counter.
// Both windows start hidden; toggled together by LowLevelKeyboardProc via
// the configurable OVERLAY_TOGGLE_KEY.
UM_GUARDED_THREAD(OverlayThread) {
    LabelCurrentThread(L"um.dll: Overlay");
    HMODULE module = reinterpret_cast<HMODULE>(parameter);
    g_overlayStartTickMs = GetTickCount64();
    g_overlayFpsLastTickMs = g_overlayStartTickMs;

    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = "UMOverlayWindowClass";
    if (!RegisterClassA(&windowClass)) {
        LogLine("ERROR", "[OVERLAY] RegisterClassA for overlay failed, error=%lu", GetLastError());
        return 0;
    }

    g_overlayTargetWindow = FindGameWindow();
    OverlayContent initialContent;
    BuildOverlayContent(initialContent);
    int panelHeight = ComputeOverlayPanelHeight(initialContent);
    RECT targetRect = {100, 100, 100 + OVERLAY_PANEL_WIDTH + 200, 100 + panelHeight + 200};
    if (g_overlayTargetWindow) {
        GetWindowRect(g_overlayTargetWindow, &targetRect);
    }
    RECT initialRect;
    ComputeOverlayRect(targetRect, g_overlayPosition, OVERLAY_PANEL_WIDTH, panelHeight, initialRect);

    g_overlayWindowsAreLayered = g_overlayBackgroundOpacityPercent < 100;
    // WS_EX_NOACTIVATE is the actual fix for "clicking the overlay minimizes
    // the game": without it, a click can still activate/focus this window
    // (even though it's a plain WS_POPUP with no visible titlebar), which
    // steals foreground focus from the fullscreen game and makes it minimize.
    // WS_EX_TRANSPARENT alone is a mouse-passthrough hint, not a guarantee.
    DWORD extendedStyle = WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    if (g_overlayWindowsAreLayered) {
        extendedStyle |= WS_EX_LAYERED;
    }
    g_overlayWindow = CreateWindowExA(extendedStyle,
        windowClass.lpszClassName, "Universal Mod Overlay", WS_POPUP,
        initialRect.left, initialRect.top, OVERLAY_PANEL_WIDTH, panelHeight,
        NULL, NULL, module, NULL);
    if (!g_overlayWindow) {
        LogLine("ERROR", "[OVERLAY] CreateWindowExA for overlay failed, error=%lu", GetLastError());
        return 0;
    }
    ShowWindow(g_overlayWindow, SW_HIDE);

    if (g_overlayLogEnabled) {
        RECT logRect;
        int logPanelHeight = 8 + g_overlayLogLineCount * OVERLAY_LOG_LINE_HEIGHT + 6;
        ComputeOverlayRect(targetRect, g_overlayLogPosition, g_overlayLogPanelWidth, logPanelHeight, logRect);
        g_overlayLogWindow = CreateWindowExA(extendedStyle,
            windowClass.lpszClassName, "Universal Mod Overlay Log", WS_POPUP,
            logRect.left, logRect.top, g_overlayLogPanelWidth, logPanelHeight,
            NULL, NULL, module, NULL);
        if (g_overlayLogWindow) {
            ShowWindow(g_overlayLogWindow, SW_HIDE);
        } else {
            LogLine("WARN", "[OVERLAY] CreateWindowExA for overlay log panel failed, error=%lu", GetLastError());
        }
    }

    if (g_profilerEnabled) {
        RECT profilerRect;
        ComputeOverlayRect(targetRect, g_profilerPosition, 320, 60, profilerRect);
        g_profilerWindow = CreateWindowExA(extendedStyle,
            windowClass.lpszClassName, "Universal Mod Profiler", WS_POPUP,
            profilerRect.left, profilerRect.top, 320, 60,
            NULL, NULL, module, NULL);
        if (g_profilerWindow) {
            ShowWindow(g_profilerWindow, SW_HIDE);
        } else {
            LogLine("WARN", "CreateWindowExA for the profiler window failed, error=%lu", GetLastError());
        }
    }

    SetTimer(g_overlayWindow, 1, g_overlayRefreshMs, NULL);
    LogLine("INFO", "[OVERLAY] Overlay window created; toggle_key=0x%02X log_toggle_key=0x%02X position=%s log_enabled=%s layered=%s",
        g_overlayToggleKey, g_overlayLogToggleKey, g_overlayPosition, g_overlayLogEnabled ? "true" : "false",
        g_overlayWindowsAreLayered ? "true" : "false");
#ifdef UM_TEST_DUMP_CANVAS
    // Test builds only: injected keystrokes are ignored by the toggle hook (by
    // design), so start visible instead.
    g_overlayVisible = 1;
    // Injected key presses never reach the keyboard hook either: UM_TEST_PROFILER starts the profiler on.
    if (GetEnvironmentVariableA("UM_TEST_PROFILER", NULL, 0) != 0) {
        g_profilerVisible = 1;
        if (g_profilerWindow) ShowWindow(g_profilerWindow, SW_SHOWNOACTIVATE);
    }
    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    if (g_overlayLogWindow) {
        g_overlayLogVisible = 1;
        ShowWindow(g_overlayLogWindow, SW_SHOWNOACTIVATE);
    }
#endif

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DestroyAlphaCanvas(g_overlayCanvas);
    DestroyAlphaCanvas(g_overlayLogCanvas);
    DestroyAlphaCanvas(g_profilerCanvas);
    UnregisterClassA(windowClass.lpszClassName, module);
    g_overlayWindow = NULL;
    g_overlayLogWindow = NULL;
    g_profilerWindow = NULL;
    return 0;
}


// Whether D7VK is the DirectDraw driver (only then does MSAA's ddraw.alternatePixelCenter apply): found once, by the text inside the driver's file.
// Has its own result (not the overlay thread's fingerprint cache), so the game's thread may call it.
bool IsD7vkDirectDraw() {
    static volatile LONG state = 0; // 0 unknown, 1 yes, 2 no
    if (state) return state == 1;
    HMODULE module = g_chainedDdraw ? g_chainedDdraw : GetModuleHandleA("ddraw.dll");
    char path[MAX_PATH] = "";
    if (!module || !GetModuleFileNameA(module, path, sizeof(path))) return false;
    bool d7vk = false;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        const DWORD size = GetFileSize(file, NULL);
        if (size != INVALID_FILE_SIZE && size > 0 && size <= 48u * 1024 * 1024) {
            std::vector<char> data(size);
            DWORD read = 0;
            if (ReadFile(file, data.data(), size, &read, NULL) && read == size) d7vk = BufferContainsText(data, "D7VK") || BufferContainsText(data, "d7vk");
        }
        CloseHandle(file);
    }
    InterlockedExchange(&state, d7vk ? 1 : 2);
    return d7vk;
}
