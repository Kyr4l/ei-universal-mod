// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the settings' values (every g_* the other files read), the parsers of their texts.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// The DLL is injected into the game process, so these flags and hooks are
// process-local. Configuration is loaded once during DLL_PROCESS_ATTACH.
HHOOK g_keyboardHook = NULL;
// Set once in DllMain; needed by ReloadConfiguration() to re-resolve um.cfg's
// path from a thread other than the one DllMain itself ran on.
HMODULE g_dllModule = NULL;
BYTE g_reloadConfigKey = VK_F12;
bool g_enableAsiCheck = true;
bool g_enableEngine = true;   // UM_ENGINE: load um-engine.dll and install its re-implemented functions
bool g_enableKeyboardRewrites = true;
bool g_enableKeyboardRewriteLogging = false;
bool g_enableCrashLogging = true;
bool g_suppressErrorDialogs = true;
bool g_clearLogOnStart = true;
bool g_enableCrashDumps = true;
char g_logPath[MAX_PATH] = {};
CRITICAL_SECTION g_logLock;
bool g_logLockInitialized = false;
HANDLE g_logClearMutex = NULL;
volatile LONG g_crashLogInProgress = 0;
volatile LONG g_errorBlockNumber = 0;
bool g_keyboardRewriteKeyDown[256] = {};
bool g_enableFileIoLogging = false;
char g_fileIoLoggingFilter[256] = {};
bool g_enableMsaa = false;       // MSAA: D7VK's ddraw.emulateFSAA = forced (real multisampling, up to 4x)
char g_ddrawDll[MAX_PATH] = {};   // DDRAW_DLL: a DirectDraw wrapper (D7VK) loaded from there instead of the game folder's ddraw.dll
HMODULE g_chainedDdraw = NULL;    // that module once loaded
DirectDrawCreateFunction g_gameDirectDrawCreate = NULL;     // the game folder's own DirectDrawCreate / Ex, kept for when the wrapper cannot create a device
DirectDrawCreateExFunction g_gameDirectDrawCreateEx = NULL;
bool g_enableMobValidation = false;
bool g_enableHeapFreeQuarantine = false;
int g_heapAllocPadding = 64;
bool g_enableOverlay = true;
BYTE g_overlayToggleKey = VK_F9;
volatile LONG g_overlayVisible = 0;
BYTE g_overlayLogToggleKey = VK_F10;
volatile LONG g_overlayLogVisible = 0;
BYTE g_profilerKey = VK_F11;
BYTE g_profilerDumpKey = VK_SCROLL; // OVERLAY_PROFILER_DUMP_KEY: the tree to um-profile.txt
bool g_uiQuadBlit = false;          // UI_QUAD_BLIT (#80): the interface panels drawn as textured quads instead of Blts onto the frame
bool g_uiBlitCache = false;         // UI_BLIT_CACHE (#80): the interface panels uploaded only when they change
bool g_shadowLockCache = false;     // SHADOW_LOCK_CACHE (#80): the units' shadow textures re-uploaded only when they change
// UM_PROFILE_AUTODUMP=<seconds> in the environment (unattended runs, no um.cfg change): the profiler runs from the
// start and its tree goes to um-profile.txt every that many seconds.
int g_profilerAutoDumpSeconds = 0;
char g_profilerPosition[16] = "left";
HWND g_profilerWindow = NULL;
int g_profilerHz = 250;
int g_profilerLineCount = 20;
// The DLL server for um-multitool (dll_server.hpp): off unless DLL_SERVER_ENABLED; needs a restart.
bool g_dllServerEnabled = false;
bool g_dllServerDebug = false;   // DLL_SERVER_DEBUG: commands that change the game (write, breakpoints)
int g_dllServerPort = 18888;
bool g_profilerEnabled = false;  // OVERLAY_PROFILER_ENABLED
volatile LONG g_profilerVisible = 0;
volatile LONG g_profilerRunning = 0;      // the sampler keeps going while this is 1
volatile DWORD g_profilerFrameThreadId = 0; // the thread that presents frames: the game's main thread
void (*volatile g_onMainThreadFrame)() = nullptr; // the DLL server's queued CALLs, run at every frame
HWND g_overlayWindow = NULL;
HWND g_overlayTargetWindow = NULL;
ULONGLONG g_overlayStartTickMs = 0;
char g_overlayPosition[16] = "top-left";
COLORREF g_overlayTextColor = RGB(0, 255, 0);
bool g_overlayShowFpsGraph = true;
volatile LONG g_overlayFrameCount = 0;
volatile LONG g_overlayFrameCounterActive = 0;
LONG g_overlayFpsLastFrameCount = 0;
ULONGLONG g_overlayFpsLastTickMs = 0;
double g_overlayCurrentFps = 0.0;
double g_overlayCurrentFrameTimeMs = 0.0;
double g_overlayFpsPeakEver = 0.0;
double g_overlayFpsHistory[OVERLAY_HISTORY_CAPACITY] = {};
int g_overlayFpsHistoryNext = 0;
int g_overlayFpsHistoryCount = 0;
bool g_overlayCompact = false;
bool g_overlayShowFrameStats = true;
bool g_overlayShowLaa = true;
bool g_overlayShowMap = true;
bool g_overlayShowWarnings = true;
// Warning/error lines written to um.log so far (counted in LogLine), for the
// overlay's badge.
volatile LONG g_logWarningCount = 0;
volatile LONG g_logErrorCount = 0;
float g_overlayFrameTimesMs[OVERLAY_FRAME_TIME_CAPACITY] = {};
volatile LONG g_overlayFrameTimeNext = 0;
volatile LONG g_overlayFrameTimeCount = 0;
MapLoadState g_map = {};
std::unordered_set<std::string> g_mapExpectedExtras; // lower-case .mob names the loaded scripts AddMob
volatile LONG g_currentMapLock = 0;
int g_overlayRefreshMs = 500;
double g_overlayFpsMarks[OVERLAY_FPS_MARKS_MAX] = {30.0, 60.0, 75.0, 120.0, 140.0, 165.0, 240.0};
int g_overlayFpsMarkCount = 7;
COLORREF g_overlayBackgroundColor = RGB(0, 0, 0);
int g_overlayBackgroundOpacityPercent = 20;

char g_overlayLogRing[OVERLAY_LOG_CAPACITY][OVERLAY_LOG_ENTRY_SIZE] = {};
int g_overlayLogRingNext = 0;
int g_overlayLogRingCount = 0;
bool g_overlayLogEnabled = true;
int g_overlayLogLineCount = 10;
char g_overlayLogPosition[16] = "bottom-left";
int g_overlayLogPanelWidth = 900;
bool g_overlayLogWrapEnabled = true;
char g_overlayLogLevelFilter[128] = "SYSINFO";
HWND g_overlayLogWindow = NULL;
char g_overlayTransparencyStyle[16] = "alpha";
// Set once in OverlayThread before either window is created: true unless
// OVERLAY_BACKGROUND_OPACITY=100, in which case the windows are created
// WITHOUT WS_EX_LAYERED at all (see CompositeCanvasToWindow) - some Wine/
// Wayland compositors force a game out of its direct-scanout present path
// the instant ANY layered/alpha window overlaps it, regardless of the
// actual alpha values used, causing a large GPU/FPS regression; a fully
// opaque panel has no need for a layered window in the first place.
bool g_overlayWindowsAreLayered = true;

// -- Overlay resource/backend diagnostics --
bool g_overlayShowResources = true;
bool g_overlayShowBackend = true;
bool g_overlayShowThreads = true;
double g_overlayCpuPercent = 0.0;
double g_overlayWorkingSetMb = 0.0;
int g_overlayThreadCount = 0;
ULONGLONG g_overlayLastCpuSampleTickMs = 0;
ULONGLONG g_overlayLastCpuTotalTime100ns = 0;
int g_overlayLogicalProcessorCount = 0;
char g_directDrawBackendName[128] = "unknown";
volatile LONG g_directDrawBackendIdentified = 0;

int g_overlayThreadDisplayCount = 8;
OverlayThreadSample g_overlayThreadSamples[OVERLAY_THREAD_DISPLAY_MAX] = {};
int g_overlayThreadSampleCount = 0;
ULONGLONG g_overlayLastThreadSampleTickMs = 0;
std::unordered_map<DWORD, ULONGLONG> g_threadLastCpuTime100ns;

SetThreadDescriptionFunction g_setThreadDescription = NULL;
GetThreadDescriptionFunction g_getThreadDescription = NULL;
volatile LONG g_threadDescriptionFunctionsResolved = 0;

// Not defined by MinGW's headers; value is stable across Windows versions.




DDCreateSurfaceFunction g_originalDDCreateSurface = NULL;
DDFlipFunction g_originalDDFlip = NULL;
DDBltFunction g_originalDDBlt = NULL;
DDBltFastFunction g_originalDDBltFast = NULL;
DDLockFunction g_originalDDLock = NULL;
DDGetDCFunction g_originalDDGetDC = NULL;
DDUnlockFunction g_originalDDUnlock = NULL;
DDQueryInterfaceFunction g_originalDDQueryInterface = NULL;
D3DCreateDeviceFunction g_originalD3DCreateDevice = NULL;
D3DLoadFunction g_originalD3DLoad = NULL;
void* g_directDrawObject = NULL; // the game's IDirectDraw (its CreateSurface makes the shadow surfaces)

ExitProcessFunction g_originalExitProcess = NULL;       // game.exe's import
ExitProcessFunction g_originalExitProcessCrt = NULL;    // ucrtbase.dll's import (exit() goes through it)
CreateFileAFunction g_originalCreateFileA = NULL;
CreateFileWFunction g_originalCreateFileW = NULL;
ReadFileFunction g_originalReadFile = NULL;
WriteFileFunction g_originalWriteFile = NULL;
CloseHandleFunction g_originalCloseHandle = NULL;
HeapAllocFunction g_originalHeapAlloc = NULL;
HeapFreeFunction g_originalHeapFree = NULL;
HeapDestroyFunction g_originalHeapDestroy = NULL;
HeapReAllocFunction g_originalHeapReAlloc = NULL;
DirectDrawCreateFunction g_originalDirectDrawCreate = NULL;
DirectDrawCreateExFunction g_originalDirectDrawCreateEx = NULL;

// Forward declaration: defined later, but used by earlier code.
void LogLine(const char* level, const char* format, ...);
std::string ProfileNameOf(DWORD function); // profiler names, also for the DirectDraw call sites
void* UiStageFor(void* source, void* destination); // the UI blit cache (#80): the staging copy to blit from, NULL for the source itself
void DumpProfile(); // the profiler tree to um-profile.txt (OVERLAY_PROFILER_DUMP_KEY)
void DllServerNoteLog(const char* line); // dll_server.hpp: um.log lines for its clients


CRITICAL_SECTION g_fileHandleLock;
bool g_fileHandleLockInitialized = false;
TrackedFileHandle g_fileHandles[256] = {};

// Compare two narrow strings without regard to ASCII letter case.
bool EqualsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) {
        return a == b;
    }

    while (*a && *b) {
        unsigned char ca = static_cast<unsigned char>(*a);
        unsigned char cb = static_cast<unsigned char>(*b);
        if (tolower(ca) != tolower(cb)) {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == *b;
}

// Accept only the literal boolean value "true", case-insensitively.
bool IsTrueString(LPCSTR value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    char lowerValue[16] = {};
    size_t len = strlen(value);
    if (len >= sizeof(lowerValue)) {
        len = sizeof(lowerValue) - 1;
    }

    for (size_t i = 0; i < len; ++i) {
        lowerValue[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
    }
    lowerValue[len] = '\0';

    return strcmp(lowerValue, "true") == 0;
}

// Parse a key name (e.g. "F7", "0x76", "118") into a virtual-key code.
// Falls back to defaultKey for empty or unrecognized values.
BYTE ParseVirtualKeyName(const char* value, BYTE defaultKey) {
    if (!value || value[0] == '\0') {
        return defaultKey;
    }

    if ((value[0] == 'F' || value[0] == 'f') && isdigit(static_cast<unsigned char>(value[1]))) {
        int number = atoi(value + 1);
        if (number >= 1 && number <= 12) {
            return static_cast<BYTE>(VK_F1 + (number - 1));
        }
    }

    char* end = NULL;
    long parsed = strtol(value, &end, 0);
    if (end != value && *end == '\0' && parsed >= 0 && parsed <= 255) {
        return static_cast<BYTE>(parsed);
    }

    return defaultKey;
}

// Parse a "RRGGBB" (optionally prefixed with '#' or "0x") hex color into a
// COLORREF. Falls back to defaultColor for empty or unrecognized values.
COLORREF ParseHexColor(const char* value, COLORREF defaultColor) {
    if (!value || value[0] == '\0') {
        return defaultColor;
    }

    if (value[0] == '#') {
        ++value;
    } else if ((value[0] == '0') && (value[1] == 'x' || value[1] == 'X')) {
        value += 2;
    }

    if (strlen(value) != 6) {
        return defaultColor;
    }
    for (int i = 0; i < 6; ++i) {
        if (!isxdigit(static_cast<unsigned char>(value[i]))) {
            return defaultColor;
        }
    }

    unsigned long rgb = strtoul(value, NULL, 16);
    BYTE r = static_cast<BYTE>((rgb >> 16) & 0xFF);
    BYTE g = static_cast<BYTE>((rgb >> 8) & 0xFF);
    BYTE b = static_cast<BYTE>(rgb & 0xFF);
    return RGB(r, g, b);
}

// Normalize an OVERLAY_POSITION value (e.g. "Top-Right") to lowercase and
// fall back to "top-left" for empty or unrecognized values.
void ParseOverlayPosition(const char* value, char* out, size_t outSize) {
    static const char* validPositions[] = {
        "top-left", "top-right", "bottom-left", "bottom-right",
        "top", "bottom", "left", "right", "center"
    };

    char lowerValue[32] = {};
    if (value) {
        size_t len = strlen(value);
        if (len >= sizeof(lowerValue)) {
            len = sizeof(lowerValue) - 1;
        }
        for (size_t i = 0; i < len; ++i) {
            lowerValue[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
        }
        lowerValue[len] = '\0';
    }

    for (const char* candidate : validPositions) {
        if (strcmp(lowerValue, candidate) == 0) {
            strncpy(out, candidate, outSize - 1);
            out[outSize - 1] = '\0';
            return;
        }
    }

    strncpy(out, "top-left", outSize - 1);
    out[outSize - 1] = '\0';
}

// Parse an integer clamped to [min, max], falling back to defaultValue only
// when the text itself is not a number at all; an in-range-but-out-of-bounds
// value is clamped rather than discarded. Replaces five near-identical
// clamped-int parsers (refresh interval, opacity, log lines, log width,
// thread count) that used to each hand-roll this with slightly different
// (almost certainly unintentional) edge-case behavior - e.g. some of them
// fell back to defaultValue instead of clamping when a value was merely too
// small. One consistent rule now applies to all of them.
long ParseClampedLong(const char* value, long min, long max, long defaultValue) {
    if (!value || value[0] == '\0') {
        return defaultValue;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }
    if (parsed < min) {
        parsed = min;
    } else if (parsed > max) {
        parsed = max;
    }
    return parsed;
}

// Parse a comma-separated list of positive FPS values (e.g. "30,60,75,120")
// into a sorted-ascending array, capped at OVERLAY_FPS_MARKS_MAX entries.
// Leaves outMarks/outCount untouched (caller keeps its defaults) on any
// empty/fully-invalid input.
void ParseFpsMarks(const char* value, double* outMarks, int* outCount) {
    if (!value || value[0] == '\0') {
        return;
    }

    double parsedMarks[OVERLAY_FPS_MARKS_MAX] = {};
    int parsedCount = 0;

    char buffer[256] = {};
    strncpy(buffer, value, sizeof(buffer) - 1);
    char* token = strtok(buffer, ",");
    while (token && parsedCount < OVERLAY_FPS_MARKS_MAX) {
        char* end = NULL;
        double parsedValue = strtod(token, &end);
        if (end != token && parsedValue > 0.0) {
            parsedMarks[parsedCount++] = parsedValue;
        }
        token = strtok(NULL, ",");
    }

    if (parsedCount == 0) {
        return;
    }

    std::sort(parsedMarks, parsedMarks + parsedCount);
    for (int i = 0; i < parsedCount; ++i) {
        outMarks[i] = parsedMarks[i];
    }
    *outCount = parsedCount;
}


// Strip optional surrounding double quotes/whitespace and store a bounded,
// comma-separated config string (used for FILE_IO_LOGGING_FILTER and
// OVERLAY_LOG_LEVEL_FILTER, both documented/written as a quoted value).
void SetQuotedConfigString(char* dest, size_t destSize, const char* value) {
    dest[0] = '\0';
    if (!value) {
        return;
    }

    while (*value == ' ' || *value == '\t' || *value == '"') {
        ++value;
    }
    strncpy(dest, value, destSize - 1);
    dest[destSize - 1] = '\0';
    size_t length = strlen(dest);
    while (length > 0 &&
        (dest[length - 1] == ' ' || dest[length - 1] == '\t' || dest[length - 1] == '"')) {
        dest[--length] = '\0';
    }
}

