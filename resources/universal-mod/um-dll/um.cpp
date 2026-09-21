// This DLL rewrites backtick and number-row input as US QWERTY scan codes,
// verifies the required SpellAddonX.asi file, and provides optional diagnostics.
// Logging, crash reporting, keyboard rewrite logging, and anti-crash
// behavior are configured through um.cfg beside the DLL or environment variables.

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <io.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <algorithm>

#include "mob_script_check.hpp"
#include "profile_symbols.hpp"

// The DLL is injected into the game process, so these flags and hooks are
// process-local. Configuration is loaded once during DLL_PROCESS_ATTACH.
static HHOOK g_keyboardHook = NULL;
// Set once in DllMain; needed by ReloadConfiguration() to re-resolve um.cfg's
// path from a thread other than the one DllMain itself ran on.
static HMODULE g_dllModule = NULL;
static BYTE g_reloadConfigKey = VK_F12;
// um.dll's own version, shown in the overlay title and logged at startup.
static const char* const UM_VERSION = "1.1";
static bool g_enableAsiCheck = true;
static bool g_enableKeyboardRewrites = true;
static bool g_enableKeyboardRewriteLogging = false;
static bool g_enableCrashLogging = true;
static bool g_suppressErrorDialogs = true;
static bool g_clearLogOnStart = true;
static bool g_enableCrashDumps = true;
static char g_logPath[MAX_PATH] = {};
static CRITICAL_SECTION g_logLock;
static bool g_logLockInitialized = false;
static HANDLE g_logClearMutex = NULL;
static volatile LONG g_crashLogInProgress = 0;
static volatile LONG g_errorBlockNumber = 0;
static bool g_keyboardRewriteKeyDown[256] = {};
static bool g_enableFileIoLogging = false;
static char g_fileIoLoggingFilter[256] = {};
static bool g_enableMobValidation = false;
static bool g_enableHeapFreeQuarantine = false;
static int g_heapFreeQuarantineMb = 64;
static int g_heapFreeQuarantineObjectsMb = 128;
static int g_heapAllocPadding = 64;
static bool g_heapFreeQuarantinePoison = false;
static bool g_enableOverlay = true;
static BYTE g_overlayToggleKey = VK_F9;
static volatile LONG g_overlayVisible = 0;
static BYTE g_overlayLogToggleKey = VK_F10;
static volatile LONG g_overlayLogVisible = 0;
static BYTE g_profilerKey = VK_F11;
static char g_profilerPosition[16] = "left";
static HWND g_profilerWindow = NULL;
static int g_profilerHz = 250;
static int g_profilerLineCount = 20;
static volatile LONG g_profilerVisible = 0;
static volatile LONG g_profilerRunning = 0;      // the sampler keeps going while this is 1
static volatile DWORD g_profilerFrameThreadId = 0; // the thread that presents frames: the game's main thread
static HWND g_overlayWindow = NULL;
static HWND g_overlayTargetWindow = NULL;
static ULONGLONG g_overlayStartTickMs = 0;
static char g_overlayPosition[16] = "top-left";
static COLORREF g_overlayTextColor = RGB(0, 255, 0);
static bool g_overlayShowFpsGraph = true;
static volatile LONG g_overlayFrameCount = 0;
static volatile LONG g_overlayFrameCounterActive = 0;
static LONG g_overlayFpsLastFrameCount = 0;
static ULONGLONG g_overlayFpsLastTickMs = 0;
static double g_overlayCurrentFps = 0.0;
static double g_overlayCurrentFrameTimeMs = 0.0;
static double g_overlayFpsPeakEver = 0.0;
static const int OVERLAY_HISTORY_CAPACITY = 64;
static double g_overlayFpsHistory[OVERLAY_HISTORY_CAPACITY] = {};
static int g_overlayFpsHistoryNext = 0;
static int g_overlayFpsHistoryCount = 0;
static bool g_overlayCompact = false;
static bool g_overlayShowFrameStats = true;
static bool g_overlayShowLaa = true;
static bool g_overlayShowMap = true;
static bool g_overlayShowWarnings = true;
// Warning/error lines written to um.log so far (counted in LogLine), for the
// overlay's badge.
static volatile LONG g_logWarningCount = 0;
static volatile LONG g_logErrorCount = 0;
// Recent per-frame presentation intervals (written by CountPresentedFrame on the
// game's render thread, read by the overlay thread) for the 1% low / worst
// frametime readout.
static const int OVERLAY_FRAME_TIME_CAPACITY = 4096;
static const double OVERLAY_FRAME_STATS_WINDOW_MS = 10000.0;
static float g_overlayFrameTimesMs[OVERLAY_FRAME_TIME_CAPACITY] = {};
static volatile LONG g_overlayFrameTimeNext = 0;
static volatile LONG g_overlayFrameTimeCount = 0;
// What the game is showing, worked out from the map files it opens (the main menu and the lobby
// are maps too): the terrain (.mpr), the base map (.mob), the quest map (.mob) and the extra
// script-only maps a script loads with AddMob. Kept by NoteMapFile, read by the overlay.
struct MapLoadState {
    char mpr[64];
    char base[64];
    char quest[64];
    char baseWanted[64];        // lower case: the base map the quest's .mq names, if not opened yet
    char lastExtra[64];
    int extraCount;
    ULONGLONG openedTickMs;     // when this map started loading
    char recentMpr[64];         // the last .mpr opened, in case it comes before its map
    ULONGLONG recentMprTickMs;
    bool valid;
};
static MapLoadState g_map = {};
static std::unordered_set<std::string> g_mapExpectedExtras; // lower-case .mob names the loaded scripts AddMob
static volatile LONG g_currentMapLock = 0;
static int g_overlayRefreshMs = 500;
static const int OVERLAY_FPS_MARKS_MAX = 16;
static double g_overlayFpsMarks[OVERLAY_FPS_MARKS_MAX] = {30.0, 60.0, 75.0, 120.0, 140.0, 165.0, 240.0};
static int g_overlayFpsMarkCount = 7;
static COLORREF g_overlayBackgroundColor = RGB(0, 0, 0);
static int g_overlayBackgroundOpacityPercent = 20;

// Real-time mirror of the last few formatted um.log lines, appended to by
// LogLine() itself; the overlay's log panel reads this instead of the file.
static const int OVERLAY_LOG_CAPACITY = 50;
// Wide enough for the long findings the validators write (a double-free or [MOBCHECK]
// line runs 250-400 characters); anything longer is cut when it is stored here, um.log
// on disk always has the full line.
static const size_t OVERLAY_LOG_ENTRY_SIZE = 512;
static char g_overlayLogRing[OVERLAY_LOG_CAPACITY][OVERLAY_LOG_ENTRY_SIZE] = {};
static int g_overlayLogRingNext = 0;
static int g_overlayLogRingCount = 0;
static bool g_overlayLogEnabled = true;
static int g_overlayLogLineCount = 10;
static char g_overlayLogPosition[16] = "bottom-left";
static int g_overlayLogPanelWidth = 900;
static bool g_overlayLogWrapEnabled = true;
static char g_overlayLogLevelFilter[128] = "SYSINFO";
static HWND g_overlayLogWindow = NULL;
static char g_overlayTransparencyStyle[16] = "alpha";
// Set once in OverlayThread before either window is created: true unless
// OVERLAY_BACKGROUND_OPACITY=100, in which case the windows are created
// WITHOUT WS_EX_LAYERED at all (see CompositeCanvasToWindow) - some Wine/
// Wayland compositors force a game out of its direct-scanout present path
// the instant ANY layered/alpha window overlaps it, regardless of the
// actual alpha values used, causing a large GPU/FPS regression; a fully
// opaque panel has no need for a layered window in the first place.
static bool g_overlayWindowsAreLayered = true;

// -- Overlay resource/backend diagnostics --
static bool g_overlayShowResources = true;
static bool g_overlayShowBackend = true;
static bool g_overlayShowThreads = true;
static double g_overlayCpuPercent = 0.0;
static double g_overlayWorkingSetMb = 0.0;
static int g_overlayThreadCount = 0;
static ULONGLONG g_overlayLastCpuSampleTickMs = 0;
static ULONGLONG g_overlayLastCpuTotalTime100ns = 0;
static int g_overlayLogicalProcessorCount = 0;
static char g_directDrawBackendName[128] = "unknown";
static volatile LONG g_directDrawBackendIdentified = 0;

// Per-thread CPU%% breakdown for the overlay ("what's actually using CPU").
// Windows has no per-thread memory/RAM concept (memory belongs to the whole
// process, not individual threads), so only CPU time is tracked per thread.
struct OverlayThreadSample {
    DWORD threadId;
    double cpuPercent;
    char name[32];
};
// Fixed array capacity (OVERLAY_THREAD_COUNT config value is clamped to this).
static const int OVERLAY_THREAD_DISPLAY_MAX = 32;
static int g_overlayThreadDisplayCount = 8;
static OverlayThreadSample g_overlayThreadSamples[OVERLAY_THREAD_DISPLAY_MAX] = {};
static int g_overlayThreadSampleCount = 0;
static ULONGLONG g_overlayLastThreadSampleTickMs = 0;
static std::unordered_map<DWORD, ULONGLONG> g_threadLastCpuTime100ns;

// SetThreadDescription/GetThreadDescription only exist on Windows 10 1607+ and
// are not declared by this project's older MinGW headers, so both are
// resolved dynamically; threads that never named themselves (true for every
// thread this 1990s/2000s-era game engine creates) simply show "(unnamed)".
typedef HRESULT (WINAPI *SetThreadDescriptionFunction)(HANDLE, PCWSTR);
typedef HRESULT (WINAPI *GetThreadDescriptionFunction)(HANDLE, PWSTR*);
static SetThreadDescriptionFunction g_setThreadDescription = NULL;
static GetThreadDescriptionFunction g_getThreadDescription = NULL;
static volatile LONG g_threadDescriptionFunctionsResolved = 0;

// Not defined by MinGW's headers; value is stable across Windows versions.

typedef HANDLE (WINAPI *CreateFileAFunction)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFunction)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFileFunction)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WriteFileFunction)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandleFunction)(HANDLE);
typedef LPVOID (WINAPI *HeapAllocFunction)(HANDLE, DWORD, SIZE_T);
typedef BOOL (WINAPI *HeapFreeFunction)(HANDLE, DWORD, LPVOID);
typedef BOOL (WINAPI *HeapDestroyFunction)(HANDLE);
typedef LPVOID (WINAPI *HeapReAllocFunction)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef HRESULT (WINAPI *DirectDrawCreateFunction)(const GUID*, void**, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawCreateExFunction)(const GUID*, void**, const GUID*, IUnknown*);
typedef BOOL (WINAPI *MiniDumpWriteDumpFunction)(HANDLE, DWORD, HANDLE, DWORD,
    const MINIDUMP_EXCEPTION_INFORMATION*, const void*, const void*);

// Only used to identify which DirectDraw driver is actually serving the game
// (native Wine ddraw, a DDraw-to-D3D/Vulkan wrapper such as DXVK, etc.), via
// the plain DirectDrawEnumerateA export - resolved dynamically below rather
// than linked, since um.dll never links against ddraw.lib.
typedef HRESULT (WINAPI *DDEnumCallbackAFunction)(GUID*, LPSTR, LPSTR, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumerateAFunction)(DDEnumCallbackAFunction, LPVOID);
static const HRESULT UM_DDENUMRET_CANCEL = 1;

// Raw vtable-slot typedefs for the small subset of DirectDraw COM methods
// used to count presented frames. Slot indices and the ddsCaps byte offset
// are stable across IDirectDraw/IDirectDraw2/IDirectDraw7 and DDSURFACEDESC/
// DDSURFACEDESC2 (Microsoft only ever appends new members/methods), so this
// works regardless of which interface version the game actually requests.
typedef HRESULT (WINAPI *DDCreateSurfaceFunction)(void*, void*, void**, IUnknown*);
typedef HRESULT (WINAPI *DDFlipFunction)(void*, void*, DWORD);
typedef HRESULT (WINAPI *DDBltFunction)(void*, LPRECT, void*, LPRECT, DWORD, void*);
typedef HRESULT (WINAPI *DDBltFastFunction)(void*, DWORD, DWORD, void*, LPRECT, DWORD);
static const int DD_VTABLE_CREATESURFACE_SLOT = 6;
static const int DD_VTABLE_BLT_SLOT = 5;
static const int DD_VTABLE_BLTFAST_SLOT = 7;
static const int DD_VTABLE_FLIP_SLOT = 11;
static const size_t DD_SURFACEDESC_DDSCAPS_OFFSET = 0x68;
static const DWORD DD_DDSCAPS_PRIMARYSURFACE = 0x00000200;

static DDCreateSurfaceFunction g_originalDDCreateSurface = NULL;
static DDFlipFunction g_originalDDFlip = NULL;
static DDBltFunction g_originalDDBlt = NULL;
static DDBltFastFunction g_originalDDBltFast = NULL;

static CreateFileAFunction g_originalCreateFileA = NULL;
static CreateFileWFunction g_originalCreateFileW = NULL;
static ReadFileFunction g_originalReadFile = NULL;
static WriteFileFunction g_originalWriteFile = NULL;
static CloseHandleFunction g_originalCloseHandle = NULL;
static HeapAllocFunction g_originalHeapAlloc = NULL;
static HeapFreeFunction g_originalHeapFree = NULL;
static HeapDestroyFunction g_originalHeapDestroy = NULL;
static HeapReAllocFunction g_originalHeapReAlloc = NULL;
static DirectDrawCreateFunction g_originalDirectDrawCreate = NULL;
static DirectDrawCreateExFunction g_originalDirectDrawCreateEx = NULL;

// Forward declaration: defined later, but used by earlier code.
static void LogLine(const char* level, const char* format, ...);

struct TrackedFileHandle {
    HANDLE handle;
    char path[MAX_PATH];
    // Read/write entries are logged once per open handle to avoid flooding um.log.
    bool readLogged;
    bool writeLogged;
};

static CRITICAL_SECTION g_fileHandleLock;
static bool g_fileHandleLockInitialized = false;
static TrackedFileHandle g_fileHandles[256] = {};

// Compare two narrow strings without regard to ASCII letter case.
static bool EqualsIgnoreCase(const char* a, const char* b) {
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
static bool IsTrueString(LPCSTR value) {
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
static BYTE ParseVirtualKeyName(const char* value, BYTE defaultKey = VK_F9) {
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
static COLORREF ParseHexColor(const char* value, COLORREF defaultColor = RGB(0, 255, 0)) {
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
static void ParseOverlayPosition(const char* value, char* out, size_t outSize) {
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
static long ParseClampedLong(const char* value, long min, long max, long defaultValue) {
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
static void ParseFpsMarks(const char* value, double* outMarks, int* outCount) {
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
static void SetQuotedConfigString(char* dest, size_t destSize, const char* value) {
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

// ============================================================================
// Unified Settings Table
// ============================================================================
// Every um.cfg / environment-variable setting used to be hand-maintained in
// three separate places that had to be kept in sync by hand: the default
// um.cfg template writer, the um.cfg parser, and the environment-variable
// override pass in InitializeDllThread. All three are now driven by the one
// declarative table below (kSettings) - adding, renaming, or re-documenting a
// setting is a one-line change here instead of three separate edits.
//
// Note on OVERLAY_THREAD_COUNT: the original default-template writer printed
// the *current live value* of g_overlayThreadDisplayCount for this one
// setting (relevant only if um.cfg is deleted mid-session, after this setting
// was already changed by an earlier load, and then reloaded/regenerated).
// The table below prints the true fixed default (8) instead, which is more
// predictable for a file whose entire purpose is to *show the defaults*, so
// this is a deliberate small behavior simplification, not an oversight.

enum class SettingType { Bool, String, VKey, Color, ClampedInt, Position, FpsMarks };

struct SettingDef {
    const char* key;
    SettingType type;
    void* target;                 // address of the backing global (marks array, for FpsMarks)
    void* target2;                // FpsMarks only: address of the paired count int
    bool hasEnvOverride;          // CRASH_DUMPS and FILE_IO_LOGGING_FILTER intentionally have none (matches original)
    bool boolDefault;             // Bool
    const char* stringDefault;    // String / FpsMarks (template display text)
    size_t bufferSize;            // String / Position: size of the buffer at `target`
    bool quoteInTemplate;         // String only: whether the default template wraps the value in "..."
    BYTE vkeyDefault;             // VKey
    COLORREF colorDefault;        // Color
    long intMin, intMax, intDefault; // ClampedInt
    const char* positionDefault;  // Position
    const char* sectionBanner;    // printed verbatim (own "; " prefixes, own trailing \n) before this entry, or nullptr
    const char* comment;          // printed verbatim (own "; " prefixes, newline-joined, no trailing \n)
};

static SettingDef BoolSetting(const char* key, bool* target, bool def, bool hasEnv,
        const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::Bool, target, nullptr, hasEnv,
        def, nullptr, 0, false, 0, 0, 0, 0, 0, nullptr, section, comment };
}

static SettingDef StringSetting(const char* key, char* target, size_t bufferSize, const char* def,
        bool quoteInTemplate, bool hasEnv, const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::String, target, nullptr, hasEnv,
        false, def, bufferSize, quoteInTemplate, 0, 0, 0, 0, 0, nullptr, section, comment };
}

static SettingDef VKeySetting(const char* key, BYTE* target, BYTE def, bool hasEnv,
        const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::VKey, target, nullptr, hasEnv,
        false, nullptr, 0, false, def, 0, 0, 0, 0, nullptr, section, comment };
}

static SettingDef ColorSetting(const char* key, COLORREF* target, COLORREF def, bool hasEnv,
        const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::Color, target, nullptr, hasEnv,
        false, nullptr, 0, false, 0, def, 0, 0, 0, nullptr, section, comment };
}

static SettingDef IntSetting(const char* key, int* target, long min, long max, long def, bool hasEnv,
        const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::ClampedInt, target, nullptr, hasEnv,
        false, nullptr, 0, false, 0, 0, min, max, def, nullptr, section, comment };
}

static SettingDef PositionSetting(const char* key, char* target, size_t bufferSize, const char* def,
        bool hasEnv, const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::Position, target, nullptr, hasEnv,
        false, nullptr, bufferSize, false, 0, 0, 0, 0, 0, def, section, comment };
}

static SettingDef FpsMarksSetting(const char* key, double* target, int* countTarget, const char* templateText,
        bool hasEnv, const char* comment, const char* section = nullptr) {
    return SettingDef{ key, SettingType::FpsMarks, target, countTarget, hasEnv,
        false, templateText, 0, false, 0, 0, 0, 0, 0, nullptr, section, comment };
}

// Declared ahead of the table since a couple of entries' comment text is
// generated once from a compile-time constant rather than hand-copied.
static const SettingDef kSettings[] = {
    BoolSetting("SPELLADDON_ASI_CHECK", &g_enableAsiCheck, true, true,
        "; Require SpellAddonX.asi beside game.exe; (true/false)"),

    BoolSetting("KEYBOARD_REWRITES", &g_enableKeyboardRewrites, true, true,
        "; Rewrite backtick and number-row input as US-QWERTY keys; (true/false)",
        "; -- Keyboard --"),
    BoolSetting("KEYBOARD_REWRITES_LOGGING", &g_enableKeyboardRewriteLogging, false, true,
        "; Log keyboard rewrite events; (true/false)"),
    VKeySetting("RELOAD_CONFIG_KEY", &g_reloadConfigKey, VK_F12, true,
        "; Key that reloads this file without restarting the game (some settings still\n"
        "; need a restart); F1-F12 or a virtual-key code."),

    BoolSetting("LOGGING", &g_enableCrashLogging, true, true,
        "; Write diagnostic and crash information to um.log; (true/false)",
        "; -- Logging --"),
    BoolSetting("FILE_IO_LOGGING", &g_enableFileIoLogging, false, true,
        "; Log file opens, reads and writes; (true/false)"),
    StringSetting("FILE_IO_LOGGING_FILTER", g_fileIoLoggingFilter, sizeof(g_fileIoLoggingFilter), "", true, false,
        "; File extensions to leave out of file-I/O logging, comma-separated (e.g. mmp,res)."),
    BoolSetting("CLEAR_LOG_ON_START", &g_clearLogOnStart, true, true,
        "; Clear um.log when the game starts; (true/false)"),

    BoolSetting("SUPPRESS_ERROR_DIALOGS", &g_suppressErrorDialogs, true, true,
        "; Suppress Windows critical-error and crash dialogs, so the game just closes on\n"
        "; a crash (um.log and the dump are still written); (true/false)",
        "; -- Crash handling --"),
    BoolSetting("CRASH_DUMPS", &g_enableCrashDumps, true, false,
        "; Write a minidump (.dmp) next to um.log when the game crashes; (true/false)"),
    BoolSetting("MOB_VALIDATION", &g_enableMobValidation, false, true,
        "; Check .mob map files when they are opened and log problems: items and spells\n"
        "; missing from the database, errors in the mission script, and object IDs a quest\n"
        "; map shares with its base map. Only this mod's own map files are checked; (true/false)"),
    IntSetting("HEAP_ALLOC_PADDING", &g_heapAllocPadding, 0, 256, 64, true,
        "; Fixes a game bug that crashes it on Wine: the game writes past the end of some of\n"
        "; its memory blocks and corrupts the heap. This adds spare bytes after every block\n"
        "; to absorb it (larger blocks get more). Needs a restart; 0 = off (0-256 bytes)."),
    BoolSetting("HEAP_FREE_QUARANTINE", &g_enableHeapFreeQuarantine, false, true,
        "; Diagnostics: tracks every allocation to report in um.log which ones the game\n"
        "; overruns, and keeps recently freed memory untouched for a while. Slows the game\n"
        "; down and uses extra memory; needs a restart; (true/false)"),
    IntSetting("HEAP_FREE_QUARANTINE_MB", &g_heapFreeQuarantineMb, 0, 1024, 64, true,
        "; TO BE DEPRECATED. Amount of freed memory held back, in MB (0-1024); 0 = hold\n"
        "; nothing and only track the allocations."),
    IntSetting("HEAP_FREE_QUARANTINE_OBJECTS_MB", &g_heapFreeQuarantineObjectsMb, 0, 512, 128, true,
        "; TO BE DEPRECATED. Extra memory, in MB, kept for small freed objects (blocks that\n"
        "; start with a game vtable); 0 = no extra pool."),
    BoolSetting("HEAP_FREE_QUARANTINE_POISON", &g_heapFreeQuarantinePoison, false, true,
        "; TO BE DEPRECATED. Diagnostic mode: stamp the first 1 KB of every freed block with\n"
        "; 0xDDDDDDDD so a stale use crashes at once, and um.log names the freed block and\n"
        "; who freed it. Switches the quarantine's protection off, so use it for a test\n"
        "; session only; (true/false)"),

    BoolSetting("OVERLAY_ENABLED", &g_enableOverlay, true, true,
        "; Show the diagnostic overlay on top of the game; (true/false)",
        "; -- Overlay --"),
    VKeySetting("OVERLAY_TOGGLE_KEY", &g_overlayToggleKey, VK_F9, true,
        "; Key that shows/hides the main panel; F1-F12 or a virtual-key code."),
    PositionSetting("OVERLAY_POSITION", g_overlayPosition, sizeof(g_overlayPosition), "top-left", true,
        "; Panel position: top-left, top-right, bottom-left, bottom-right, top, bottom,\n"
        "; left, right or center."),
    ColorSetting("OVERLAY_COLOR", &g_overlayTextColor, RGB(0, 255, 0), true,
        "; Text color as hex RRGGBB."),
    IntSetting("OVERLAY_REFRESH_MS", &g_overlayRefreshMs, 100, 5000, 500, true,
        "; Refresh interval in milliseconds (100-5000)."),
    BoolSetting("OVERLAY_COMPACT", &g_overlayCompact, false, true,
        "; Compact mode: only the FPS line (and the warning badge) instead of the full\n"
        "; panel; (true/false)"),
    BoolSetting("OVERLAY_SHOW_FPS_GRAPH", &g_overlayShowFpsGraph, true, true,
        "; Show the FPS graph; (true/false)"),
    FpsMarksSetting("OVERLAY_FPS_MARKS", g_overlayFpsMarks, &g_overlayFpsMarkCount, "30,60,75,120,140,165,240", true,
        "; FPS reference lines on the graph, comma-separated and ascending; the higher\n"
        "; ones appear once the game reaches them."),
    BoolSetting("OVERLAY_SHOW_FRAME_STATS", &g_overlayShowFrameStats, true, true,
        "; Show the 1% low FPS and worst frametime of the last 10 seconds; (true/false)"),
    BoolSetting("OVERLAY_SHOW_RESOURCES", &g_overlayShowResources, true, true,
        "; Show CPU and memory use of game.exe, and how much of its address space is used; (true/false)"),
    BoolSetting("OVERLAY_SHOW_BACKEND", &g_overlayShowBackend, true, true,
        "; Show the DirectDraw driver name and the full renderer chain (DxWrapper, dgVoodoo,\n"
        "; D7VK, DXVK, wined3d...); (true/false)"),
    BoolSetting("OVERLAY_SHOW_LAA", &g_overlayShowLaa, true, true,
        "; Show whether game.exe can use more than 2 GB of address space; (true/false)"),
    BoolSetting("OVERLAY_SHOW_MAP", &g_overlayShowMap, true, true,
        "; Show the current map (terrain, base map, quest map, and how many script maps its\n"
        "; script loaded) and the session time; (true/false)"),
    BoolSetting("OVERLAY_SHOW_WARNINGS", &g_overlayShowWarnings, true, true,
        "; Show a red badge with the number of warnings/errors logged so far; (true/false)"),
    BoolSetting("OVERLAY_SHOW_THREADS", &g_overlayShowThreads, true, true,
        "; Show CPU usage per thread; (true/false)"),
    IntSetting("OVERLAY_THREAD_COUNT", &g_overlayThreadDisplayCount, 1, OVERLAY_THREAD_DISPLAY_MAX, 8, true,
        "; Number of threads to list (1-32)."),
    VKeySetting("OVERLAY_PROFILER_KEY", &g_profilerKey, VK_F11, true,
        "; Key that shows/hides the profiler window: where the game's main thread spends\n"
        "; each frame, as a call tree of game.exe functions. Names are guessed from the\n"
        "; game's classes, strings and API calls (sub_<address> when unknown); you can name\n"
        "; functions in um-names.txt, one \"address name\" per line. Sampling only runs\n"
        "; while it is shown, at a cost of a few percent of one core; F1-F12 or a\n"
        "; virtual-key code."),
    PositionSetting("OVERLAY_PROFILER_POSITION", g_profilerPosition, sizeof(g_profilerPosition), "left", true,
        "; Profiler window position, same choices as OVERLAY_POSITION."),
    IntSetting("OVERLAY_PROFILER_HZ", &g_profilerHz, 50, 1000, 250, true,
        "; How many times per second the main thread is sampled (50-1000); more is more\n"
        "; precise and costs more."),
    IntSetting("OVERLAY_PROFILER_LINES", &g_profilerLineCount, 6, 40, 20, true,
        "; Number of lines of the profiler tree (6-40)."),
    ColorSetting("OVERLAY_BACKGROUND_COLOR", &g_overlayBackgroundColor, RGB(0, 0, 0), true,
        "; Background color as hex RRGGBB."),
    IntSetting("OVERLAY_BACKGROUND_OPACITY", &g_overlayBackgroundOpacityPercent, 0, 100, 20, true,
        "; Background opacity in percent (0-100). Anything below 100 can cost a lot of\n"
        "; FPS on some Wine/Wayland setups; 100 is the safe choice."),
    StringSetting("OVERLAY_TRANSPARENCY_STYLE", g_overlayTransparencyStyle, sizeof(g_overlayTransparencyStyle), "alpha", false, true,
        "; How opacity below 100 is drawn: alpha (smooth, not honored everywhere) or\n"
        "; dither (scanline pattern, works everywhere)."),

    BoolSetting("OVERLAY_LOG_ENABLED", &g_overlayLogEnabled, true, true,
        "; Show a panel with the latest um.log lines; (true/false)",
        "; -- Overlay log panel --"),
    VKeySetting("OVERLAY_LOG_TOGGLE_KEY", &g_overlayLogToggleKey, VK_F10, true,
        "; Key that shows/hides the log panel; F1-F12 or a virtual-key code."),
    IntSetting("OVERLAY_LOG_LINES", &g_overlayLogLineCount, 1, OVERLAY_LOG_CAPACITY, 10, true,
        "; Number of log lines to show, newest at the bottom (1-50)."),
    IntSetting("OVERLAY_LOG_WIDTH", &g_overlayLogPanelWidth, 300, 2000, 900, true,
        "; Log panel width in pixels (300-2000)."),
    BoolSetting("OVERLAY_LOG_WRAP", &g_overlayLogWrapEnabled, true, true,
        "; Wrap long lines onto extra rows instead of clipping them; (true/false)"),
    PositionSetting("OVERLAY_LOG_POSITION", g_overlayLogPosition, sizeof(g_overlayLogPosition), "bottom-left", true,
        "; Log panel position, same choices as OVERLAY_POSITION."),
    StringSetting("OVERLAY_LOG_LEVEL_FILTER", g_overlayLogLevelFilter, sizeof(g_overlayLogLevelFilter), "SYSINFO", true, true,
        "; Log levels to hide in the panel, comma-separated (SYSINFO, INFO, WARN, ERROR,\n"
        "; FATAL, DEBUG); um.log still keeps everything."),
};
static const size_t kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

// Applies one already-extracted "KEY=value" pair to its backing global,
// dispatching on the setting's declared type. Used by both the um.cfg parser
// and (for everything except Bool - see ApplySettingEnvOverride) the
// environment-variable override pass, since both contexts want the same
// "authoritatively set this value" behavior.
static void ApplySettingValue(const SettingDef& def, const char* value) {
    switch (def.type) {
    case SettingType::Bool:
        *static_cast<bool*>(def.target) = IsTrueString(value);
        break;
    case SettingType::String:
        SetQuotedConfigString(static_cast<char*>(def.target), def.bufferSize, value);
        break;
    case SettingType::VKey:
        *static_cast<BYTE*>(def.target) = ParseVirtualKeyName(value, def.vkeyDefault);
        break;
    case SettingType::Color:
        *static_cast<COLORREF*>(def.target) = ParseHexColor(value, def.colorDefault);
        break;
    case SettingType::ClampedInt:
        *static_cast<int*>(def.target) = static_cast<int>(
            ParseClampedLong(value, def.intMin, def.intMax, def.intDefault));
        break;
    case SettingType::Position:
        ParseOverlayPosition(value, static_cast<char*>(def.target), def.bufferSize);
        break;
    case SettingType::FpsMarks:
        ParseFpsMarks(value, static_cast<double*>(def.target), static_cast<int*>(def.target2));
        break;
    }
}

// Applies an environment-variable override for one setting, if both the
// setting allows env overrides and that variable is actually set. Bool
// settings intentionally preserve their original asymmetric semantics here:
// an environment variable can only turn a flag ON (OR-merge with whatever
// um.cfg already set), never force one off, so a stray env var can't
// silently disable a safety feature the user deliberately enabled in their
// config file. Every other type fully overwrites, matching the original
// per-setting env-override code this table replaces.
static void ApplySettingEnvOverride(const SettingDef& def) {
    if (!def.hasEnvOverride) {
        return;
    }
    const char* value = getenv(def.key);
    if (!value) {
        return;
    }
    if (def.type == SettingType::Bool) {
        *static_cast<bool*>(def.target) = *static_cast<bool*>(def.target) || IsTrueString(value);
    } else {
        ApplySettingValue(def, value);
    }
}

// Formats one setting's default value for the generated um.cfg template,
// matching exactly what ApplySettingValue(def, result) would parse back.
static void FormatSettingDefault(const SettingDef& def, char* out, size_t outSize) {
    switch (def.type) {
    case SettingType::Bool:
        snprintf(out, outSize, "%s", def.boolDefault ? "true" : "false");
        break;
    case SettingType::String:
    case SettingType::FpsMarks:
        snprintf(out, outSize, "%s", def.stringDefault ? def.stringDefault : "");
        break;
    case SettingType::VKey:
        // Every setting currently defaults to an F-key; hex is a defensive
        // fallback for any future non-F-key default, not currently reachable.
        if (def.vkeyDefault >= VK_F1 && def.vkeyDefault <= VK_F12) {
            snprintf(out, outSize, "F%d", def.vkeyDefault - VK_F1 + 1);
        } else {
            snprintf(out, outSize, "0x%02X", def.vkeyDefault);
        }
        break;
    case SettingType::Color:
        snprintf(out, outSize, "%02X%02X%02X",
            GetRValue(def.colorDefault), GetGValue(def.colorDefault), GetBValue(def.colorDefault));
        break;
    case SettingType::ClampedInt:
        snprintf(out, outSize, "%ld", def.intDefault);
        break;
    case SettingType::Position:
        snprintf(out, outSize, "%s", def.positionDefault ? def.positionDefault : "");
        break;
    }
}

// Writes a fresh um.cfg populated with every setting's default value and its
// documentation comment, in table order; section banners and the blank line
// separating each section are driven by the table too (a blank line is
// inserted after the last entry of each section, detected by the next
// entry - or end of table - starting a new section banner).
static void WriteDefaultConfigFile(FILE* file) {
    fprintf(file, "; Universal Mod Configuration\n\n");
    for (size_t i = 0; i < kSettingCount; ++i) {
        const SettingDef& def = kSettings[i];
        if (def.sectionBanner) {
            fprintf(file, "%s\n", def.sectionBanner);
        }
        if (def.comment) {
            fprintf(file, "%s\n", def.comment);
        }
        char valueText[512] = {};
        FormatSettingDefault(def, valueText, sizeof(valueText));
        if (def.type == SettingType::String && def.quoteInTemplate) {
            fprintf(file, "%s=\"%s\"\n", def.key, valueText);
        } else {
            fprintf(file, "%s=%s\n", def.key, valueText);
        }
        bool lastInSection = (i + 1 == kSettingCount) || (kSettings[i + 1].sectionBanner != nullptr);
        if (lastInSection) {
            fprintf(file, "\n");
        }
    }
}

// Reads the IMAGE_FILE_LARGE_ADDRESS_AWARE bit straight out of the running
// process's own PE header in memory (no need to reopen game.exe from disk:
// um.dll runs injected inside it, so its own module base IS game.exe's base).
// Result never changes at runtime, so it is cached after the first call.
static bool IsCurrentProcessLargeAddressAware() {
    static int cachedResult = -1; // -1 = not yet queried, 0 = no, 1 = yes
    if (cachedResult != -1) {
        return cachedResult != 0;
    }
    cachedResult = 0;
    BYTE* moduleBase = reinterpret_cast<BYTE*>(GetModuleHandleA(NULL));
    if (moduleBase) {
        IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        if (dosHeader->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS* ntHeaders =
                reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dosHeader->e_lfanew);
            if (ntHeaders->Signature == IMAGE_NT_SIGNATURE &&
                    (ntHeaders->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0) {
                cachedResult = 1;
            }
        }
    }
    return cachedResult != 0;
}

// Resolves SetThreadDescription/GetThreadDescription once (Windows 10 1607+
// only, may not exist under an older Wine build either); leaves both NULL on
// failure, and every caller already tolerates that.
static void EnsureThreadDescriptionFunctionsResolved() {
    if (InterlockedCompareExchange(&g_threadDescriptionFunctionsResolved, 1, 0) != 0) {
        return;
    }
    HMODULE kernel32Module = GetModuleHandleA("kernel32.dll");
    if (!kernel32Module) {
        return;
    }
    FARPROC setAddress = GetProcAddress(kernel32Module, "SetThreadDescription");
    memcpy(&g_setThreadDescription, &setAddress, sizeof(g_setThreadDescription));
    FARPROC getAddress = GetProcAddress(kernel32Module, "GetThreadDescription");
    memcpy(&g_getThreadDescription, &getAddress, sizeof(g_getThreadDescription));
}

// Best-effort: names the calling thread so the overlay's thread breakdown can
// tell um.dll's own threads apart from the game's (which never name theirs -
// SetThreadDescription didn't exist when this engine was written).
static void LabelCurrentThread(const wchar_t* name) {
    if (g_setThreadDescription) {
        g_setThreadDescription(GetCurrentThread(), name);
    }
}

// Snapshot CPU%, working set, and thread count for the overlay's resource
// line, plus a per-thread CPU% breakdown (lowest g_overlayThreadDisplayCount
// thread IDs) so a runaway/busy thread can be spotted. The process-wide CPU% is
// normalized against every logical processor (matching Task Manager's
// convention); per-thread CPU% is NOT divided by core count, since a single
// thread pinned to one core can reach 100% on its own. Windows has no
// per-thread memory/RAM concept (memory belongs to the process, not
// individual threads), so only CPU time is tracked per thread.
static void SampleProcessDiagnostics() {
    if (g_overlayLogicalProcessorCount <= 0) {
        SYSTEM_INFO systemInfo = {};
        GetSystemInfo(&systemInfo);
        g_overlayLogicalProcessorCount = systemInfo.dwNumberOfProcessors > 0 ?
            static_cast<int>(systemInfo.dwNumberOfProcessors) : 1;
    }

    FILETIME creationTime = {}, exitTime = {}, kernelTime = {}, userTime = {};
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER kernel100ns = {};
        kernel100ns.LowPart = kernelTime.dwLowDateTime;
        kernel100ns.HighPart = kernelTime.dwHighDateTime;
        ULARGE_INTEGER user100ns = {};
        user100ns.LowPart = userTime.dwLowDateTime;
        user100ns.HighPart = userTime.dwHighDateTime;
        ULONGLONG totalCpu100ns = kernel100ns.QuadPart + user100ns.QuadPart;

        ULONGLONG nowMs = GetTickCount64();
        if (g_overlayLastCpuSampleTickMs != 0) {
            ULONGLONG wallElapsedMs = nowMs - g_overlayLastCpuSampleTickMs;
            if (wallElapsedMs > 0) {
                ULONGLONG cpuElapsed100ns = totalCpu100ns - g_overlayLastCpuTotalTime100ns;
                double cpuElapsedMs = static_cast<double>(cpuElapsed100ns) / 10000.0;
                g_overlayCpuPercent = (cpuElapsedMs / static_cast<double>(wallElapsedMs)) * 100.0 /
                    g_overlayLogicalProcessorCount;
            }
        }
        g_overlayLastCpuSampleTickMs = nowMs;
        g_overlayLastCpuTotalTime100ns = totalCpu100ns;
    }

    PROCESS_MEMORY_COUNTERS memoryCounters = {};
    memoryCounters.cb = sizeof(memoryCounters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memoryCounters, sizeof(memoryCounters))) {
        g_overlayWorkingSetMb = static_cast<double>(memoryCounters.WorkingSetSize) / (1024.0 * 1024.0);
    }

    ULONGLONG nowMs = GetTickCount64();
    ULONGLONG wallElapsedMs = g_overlayLastThreadSampleTickMs != 0 ? nowMs - g_overlayLastThreadSampleTickMs : 0;
    g_overlayLastThreadSampleTickMs = nowMs;

    int threadCount = 0;
    OverlayThreadSample samples[64] = {};
    int sampleCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        DWORD currentProcessId = GetCurrentProcessId();
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != currentProcessId) {
                    continue;
                }
                ++threadCount;
                if (sampleCount >= static_cast<int>(sizeof(samples) / sizeof(samples[0]))) {
                    continue;
                }
                HANDLE threadHandle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!threadHandle) {
                    continue;
                }
                FILETIME threadCreation = {}, threadExit = {}, threadKernel = {}, threadUser = {};
                if (GetThreadTimes(threadHandle, &threadCreation, &threadExit, &threadKernel, &threadUser)) {
                    ULARGE_INTEGER threadKernel100ns = {};
                    threadKernel100ns.LowPart = threadKernel.dwLowDateTime;
                    threadKernel100ns.HighPart = threadKernel.dwHighDateTime;
                    ULARGE_INTEGER threadUser100ns = {};
                    threadUser100ns.LowPart = threadUser.dwLowDateTime;
                    threadUser100ns.HighPart = threadUser.dwHighDateTime;
                    ULONGLONG threadTotal100ns = threadKernel100ns.QuadPart + threadUser100ns.QuadPart;

                    double threadCpuPercent = 0.0;
                    auto previousSample = g_threadLastCpuTime100ns.find(entry.th32ThreadID);
                    if (previousSample != g_threadLastCpuTime100ns.end() && wallElapsedMs > 0) {
                        ULONGLONG deltaTime100ns = threadTotal100ns - previousSample->second;
                        double deltaMs = static_cast<double>(deltaTime100ns) / 10000.0;
                        threadCpuPercent = (deltaMs / static_cast<double>(wallElapsedMs)) * 100.0;
                    }
                    g_threadLastCpuTime100ns[entry.th32ThreadID] = threadTotal100ns;

                    OverlayThreadSample& sample = samples[sampleCount];
                    sample.threadId = entry.th32ThreadID;
                    sample.cpuPercent = threadCpuPercent;
                    strncpy(sample.name, "(unnamed)", sizeof(sample.name) - 1);
                    if (g_getThreadDescription) {
                        PWSTR description = NULL;
                        if (SUCCEEDED(g_getThreadDescription(threadHandle, &description)) &&
                                description && description[0] != L'\0') {
                            WideCharToMultiByte(CP_ACP, 0, description, -1, sample.name,
                                static_cast<int>(sizeof(sample.name) - 1), NULL, NULL);
                        }
                        if (description) {
                            LocalFree(description);
                        }
                    }
                    ++sampleCount;
                }
                CloseHandle(threadHandle);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    g_overlayThreadCount = threadCount;

    // Sort by ascending thread ID (lowest/oldest first, typically the main
    // game thread) rather than by CPU usage, so the displayed list stays in
    // a stable order instead of reshuffling every sample as CPU%% changes.
    // A selection sort is fine here - sampleCount is small, capped at 64 above.
    int keep = sampleCount < g_overlayThreadDisplayCount ? sampleCount : g_overlayThreadDisplayCount;
    for (int i = 0; i < keep; ++i) {
        int best = i;
        for (int j = i + 1; j < sampleCount; ++j) {
            if (samples[j].threadId < samples[best].threadId) {
                best = j;
            }
        }
        if (best != i) {
            // Not std::swap(): its exported symbol name confuses this
            // toolchain's DLL linker for this particular struct.
            OverlayThreadSample temp = samples[i];
            samples[i] = samples[best];
            samples[best] = temp;
        }
        g_overlayThreadSamples[i] = samples[i];
    }
    g_overlayThreadSampleCount = keep;

    // Periodically drop stale entries (exited threads) so this map doesn't
    // grow without bound over a long play session.
    if (g_threadLastCpuTime100ns.size() > 256) {
        std::unordered_map<DWORD, ULONGLONG> stillAlive;
        for (int i = 0; i < sampleCount; ++i) {
            auto it = g_threadLastCpuTime100ns.find(samples[i].threadId);
            if (it != g_threadLastCpuTime100ns.end()) {
                stillAlive[samples[i].threadId] = it->second;
            }
        }
        g_threadLastCpuTime100ns.swap(stillAlive);
    }
}

// Return whether a log level (e.g. "SYSINFO") appears in the comma-separated
// OVERLAY_LOG_LEVEL_FILTER list, so the live log panel can skip mirroring it.
static bool IsLogLevelFiltered(const char* level) {
    if (!level || g_overlayLogLevelFilter[0] == '\0') {
        return false;
    }
    const char* filter = g_overlayLogLevelFilter;
    while (*filter) {
        while (*filter == ',' || *filter == ' ' || *filter == '\t') {
            ++filter;
        }
        const char* tokenStart = filter;
        while (*filter && *filter != ',') {
            ++filter;
        }
        const char* tokenEnd = filter;
        while (tokenEnd > tokenStart && (tokenEnd[-1] == ' ' || tokenEnd[-1] == '\t')) {
            --tokenEnd;
        }
        char token[32] = {};
        size_t tokenLength = static_cast<size_t>(tokenEnd - tokenStart);
        if (tokenLength >= sizeof(token)) {
            tokenLength = sizeof(token) - 1;
        }
        memcpy(token, tokenStart, tokenLength);
        token[tokenLength] = '\0';
        if (token[0] != '\0' && EqualsIgnoreCase(token, level)) {
            return true;
        }
        if (*filter == ',') {
            ++filter;
        }
    }
    return false;
}

// Append one timestamped, serialized diagnostic line and flush it to disk.
// Also mirrors the exact formatted line into a small in-memory ring buffer so
// the overlay's real-time log panel can display it without re-reading um.log.
static void LogLine(const char* level, const char* format, ...) {
    if (EqualsIgnoreCase(level, "WARN")) {
        InterlockedIncrement(&g_logWarningCount);
    } else if (EqualsIgnoreCase(level, "ERROR") || EqualsIgnoreCase(level, "FATAL")) {
        InterlockedIncrement(&g_logErrorCount);
    }
    if (!g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    FILE* file = fopen(g_logPath, "a");
    if (!file) {
        return;
    }

    bool logLockAcquired = !g_logLockInitialized ||
        TryEnterCriticalSection(&g_logLock) != FALSE;

    const char* outputLevel = level;
    const char* category = NULL;
    if (EqualsIgnoreCase(level, "SYSINFO")) {
        outputLevel = "SYSINFO";
    } else if (EqualsIgnoreCase(level, "CRASH")) {
        outputLevel = "DEBUG";
        category = "CRASH";
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    TIME_ZONE_INFORMATION timeZone = {};
    DWORD timeZoneId = GetTimeZoneInformation(&timeZone);
    LONG offsetMinutes = -timeZone.Bias;
    if (timeZoneId == TIME_ZONE_ID_STANDARD) {
        offsetMinutes -= timeZone.StandardBias;
    } else if (timeZoneId == TIME_ZONE_ID_DAYLIGHT) {
        offsetMinutes -= timeZone.DaylightBias;
    }
    char offsetSign = offsetMinutes < 0 ? '-' : '+';
    int absoluteOffsetMinutes = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;

    char prefix[96] = {};
    if (category) {
        snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02uT%02u:%02u:%02u%c%02d%02d] [%s] [%s] ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            offsetSign, absoluteOffsetMinutes / 60, absoluteOffsetMinutes % 60, outputLevel, category);
    } else {
        snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02uT%02u:%02u:%02u%c%02d%02d] [%s] ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            offsetSign, absoluteOffsetMinutes / 60, absoluteOffsetMinutes % 60, outputLevel);
    }

    char message[800] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    char fullLine[sizeof(prefix) + sizeof(message)] = {};
    snprintf(fullLine, sizeof(fullLine), "%s%s", prefix, message);

    fputs(fullLine, file);
    fputc('\n', file);
    fflush(file);
    intptr_t fileDescriptor = _fileno(file);
    if (fileDescriptor >= 0) {
        intptr_t operatingSystemHandle = _get_osfhandle(static_cast<int>(fileDescriptor));
        if (operatingSystemHandle != -1) {
            FlushFileBuffers(reinterpret_cast<HANDLE>(operatingSystemHandle));
        }
    }
    fclose(file);

    if (logLockAcquired && !IsLogLevelFiltered(outputLevel)) {
        snprintf(g_overlayLogRing[g_overlayLogRingNext], sizeof(g_overlayLogRing[0]), "%s", fullLine);
        g_overlayLogRingNext = (g_overlayLogRingNext + 1) % OVERLAY_LOG_CAPACITY;
        if (g_overlayLogRingCount < OVERLAY_LOG_CAPACITY) {
            ++g_overlayLogRingCount;
        }
    }

    if (logLockAcquired && g_logLockInitialized) {
        LeaveCriticalSection(&g_logLock);
    }
}

// Write a standard Windows minidump. The file is portable: WinDbg/Visual
// Studio open it on Windows, while minidump-aware tools can inspect it on Linux.
static void WriteCrashDump(EXCEPTION_POINTERS* exceptionInfo) {
    if (!g_enableCrashDumps || g_logPath[0] == '\0') {
        return;
    }

    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) {
        LogLine("WARN", "Crash dump unavailable: dbghelp.dll could not be loaded");
        return;
    }
    FARPROC writerAddress = GetProcAddress(dbghelp, "MiniDumpWriteDump");
    MiniDumpWriteDumpFunction writer = NULL;
    memcpy(&writer, &writerAddress, sizeof(writer));
    if (!writer) {
        FreeLibrary(dbghelp);
        LogLine("WARN", "Crash dump unavailable: MiniDumpWriteDump was not found");
        return;
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    // Same directory as um.log, but its own "um-crashdump-" prefix rather
    // than being named after um.log itself.
    char dumpDir[MAX_PATH] = {};
    size_t logPathLen = strlen(g_logPath);
    if (logPathLen >= sizeof(dumpDir)) {
        logPathLen = sizeof(dumpDir) - 1;
    }
    memcpy(dumpDir, g_logPath, logPathLen);
    dumpDir[logPathLen] = '\0';
    size_t lastSlash = strlen(dumpDir);
    while (lastSlash > 0 && dumpDir[lastSlash - 1] != '\\' && dumpDir[lastSlash - 1] != '/') {
        --lastSlash;
    }
    dumpDir[lastSlash] = '\0';

    char dumpPath[MAX_PATH] = {};
    snprintf(dumpPath, sizeof(dumpPath), "%sum-crashdump-%04u%02u%02u-%02u%02u%02u.dmp",
        dumpDir, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dumpFile == INVALID_HANDLE_VALUE) {
        FreeLibrary(dbghelp);
        LogLine("WARN", "Crash dump could not be created: %s error=%lu",
            dumpPath, GetLastError());
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionData = {};
    exceptionData.ThreadId = GetCurrentThreadId();
    exceptionData.ExceptionPointers = exceptionInfo;
    exceptionData.ClientPointers = FALSE;
    BOOL written = writer(GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo,
        exceptionInfo ? &exceptionData : NULL, NULL, NULL);
    CloseHandle(dumpFile);
    FreeLibrary(dbghelp);
    if (written) {
        LogLine("FATAL", "Crash dump written path=%s", dumpPath);
    } else {
        LogLine("WARN", "Crash dump writing failed, error=%lu", GetLastError());
    }
}

// File-I/O diagnostics use the common logger but can be disabled independently.
static void LogFileIo(const char* format, ...) {
    if (!g_enableFileIoLogging || !g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    char message[512] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LogLine("DEBUG", "[FILEIO] %s", message);
}

// Copy a tracked path and atomically mark its read/write direction as logged.
static bool CopyAndMarkTrackedFileIo(HANDLE handle, bool write, char* path, size_t pathSize) {
    // Copy the path while locked and mark the direction before releasing the
    // lock so concurrent reads or writes cannot produce duplicate entries.
    if (!g_fileHandleLockInitialized || !path || pathSize == 0) {
        return false;
    }

    bool found = false;
    path[0] = '\0';
    EnterCriticalSection(&g_fileHandleLock);
    for (size_t i = 0; i < sizeof(g_fileHandles) / sizeof(g_fileHandles[0]); ++i) {
        if (g_fileHandles[i].handle == handle) {
            bool* logged = write ? &g_fileHandles[i].writeLogged : &g_fileHandles[i].readLogged;
            if (*logged) {
                break;
            }
            strncpy(path, g_fileHandles[i].path, pathSize - 1);
            path[pathSize - 1] = '\0';
            *logged = true;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_fileHandleLock);
    return found;
}

// Return whether a path's extension appears in the configured ignore list.
static bool IsIgnoredFileExtension(const char* path) {
    // The filter contains extension tokens without requiring a leading dot,
    // for example "mmp,res". Matching is case-insensitive.
    if (!path || g_fileIoLoggingFilter[0] == '\0') {
        return false;
    }

    const char* fileName = strrchr(path, '\\');
    const char* slash = strrchr(path, '/');
    if (slash && (!fileName || slash > fileName)) {
        fileName = slash;
    }
    fileName = fileName ? fileName + 1 : path;
    const char* dot = strrchr(fileName, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }

    const char* filter = g_fileIoLoggingFilter;
    while (*filter) {
        while (*filter == ',' || *filter == ' ' || *filter == '\t') {
            ++filter;
        }
        const char* tokenStart = filter;
        while (*filter && *filter != ',') {
            ++filter;
        }
        const char* tokenEnd = filter;
        while (tokenEnd > tokenStart &&
            (tokenEnd[-1] == ' ' || tokenEnd[-1] == '\t' || tokenEnd[-1] == '"')) {
            --tokenEnd;
        }
        if (tokenStart < tokenEnd && *tokenStart == '.') {
            ++tokenStart;
        }

        char extension[32] = {};
        size_t extensionLength = static_cast<size_t>(tokenEnd - tokenStart);
        if (extensionLength >= sizeof(extension)) {
            extensionLength = sizeof(extension) - 1;
        }
        memcpy(extension, tokenStart, extensionLength);
        extension[extensionLength] = '\0';
        if (EqualsIgnoreCase(dot + 1, extension)) {
            return true;
        }
        if (*filter == ',') {
            ++filter;
        }
    }
    return false;
}

// Add or reset a file handle in the bounded diagnostic tracking table.
static void TrackFileHandle(HANDLE handle, const char* path) {
    // The fixed table is intentionally small: it is only a diagnostic aid,
    // and untracked handles still continue to work normally in the game.
    if (!g_fileHandleLockInitialized || handle == INVALID_HANDLE_VALUE || !path) {
        return;
    }

    EnterCriticalSection(&g_fileHandleLock);
    size_t freeIndex = sizeof(g_fileHandles) / sizeof(g_fileHandles[0]);
    for (size_t i = 0; i < sizeof(g_fileHandles) / sizeof(g_fileHandles[0]); ++i) {
        if (g_fileHandles[i].handle == handle) {
            freeIndex = i;
            break;
        }
        if (freeIndex == sizeof(g_fileHandles) / sizeof(g_fileHandles[0]) &&
            g_fileHandles[i].handle == NULL) {
            freeIndex = i;
        }
    }
    if (freeIndex < sizeof(g_fileHandles) / sizeof(g_fileHandles[0])) {
        g_fileHandles[freeIndex].handle = handle;
        g_fileHandles[freeIndex].readLogged = false;
        g_fileHandles[freeIndex].writeLogged = false;
        strncpy(g_fileHandles[freeIndex].path, path, sizeof(g_fileHandles[freeIndex].path) - 1);
        g_fileHandles[freeIndex].path[sizeof(g_fileHandles[freeIndex].path) - 1] = '\0';
    }
    LeaveCriticalSection(&g_fileHandleLock);
}

// Remove a closed handle from the diagnostic tracking table.
static void UntrackFileHandle(HANDLE handle) {
    if (!g_fileHandleLockInitialized) {
        return;
    }

    EnterCriticalSection(&g_fileHandleLock);
    for (size_t i = 0; i < sizeof(g_fileHandles) / sizeof(g_fileHandles[0]); ++i) {
        if (g_fileHandles[i].handle == handle) {
            g_fileHandles[i].handle = NULL;
            g_fileHandles[i].path[0] = '\0';
            break;
        }
    }
    LeaveCriticalSection(&g_fileHandleLock);
}

// Snapshot currently tracked paths and log them without holding the table lock.
static void LogTrackedFileHandles() {
    if (!g_fileHandleLockInitialized) {
        return;
    }

    char paths[256][MAX_PATH] = {};
    HANDLE handles[256] = {};
    size_t count = 0;
    EnterCriticalSection(&g_fileHandleLock);
    for (size_t i = 0; i < sizeof(g_fileHandles) / sizeof(g_fileHandles[0]); ++i) {
        if (g_fileHandles[i].handle != NULL && count < 256) {
            strncpy(paths[count], g_fileHandles[i].path, MAX_PATH - 1);
            handles[count] = g_fileHandles[i].handle;
            ++count;
        }
    }
    LeaveCriticalSection(&g_fileHandleLock);

    for (size_t i = 0; i < count; ++i) {
        LogLine("CRASH", "Open tracked file path=%s handle=%p", paths[i], handles[i]);
    }
}

// Convert CreateFile desired-access flags into a compact diagnostic label.
static void DescribeDesiredAccess(DWORD desiredAccess, char* description, size_t descriptionSize) {
    description[0] = '\0';
    if ((desiredAccess & GENERIC_READ) != 0) {
        strncat(description, "read", descriptionSize - strlen(description) - 1);
    }
    if ((desiredAccess & GENERIC_WRITE) != 0) {
        if (description[0] != '\0') {
            strncat(description, "+", descriptionSize - strlen(description) - 1);
        }
        strncat(description, "write", descriptionSize - strlen(description) - 1);
    }
    if (description[0] == '\0') {
        strncpy(description, "none", descriptionSize - 1);
        description[descriptionSize - 1] = '\0';
    }
}

// Track and log a successfully opened file unless its extension is filtered.
static void LogOpenedFile(HANDLE handle, const char* path, DWORD desiredAccess) {
    if (IsIgnoredFileExtension(path)) {
        return;
    }
    char access[32] = {};
    DescribeDesiredAccess(desiredAccess, access, sizeof(access));
    TrackFileHandle(handle, path);
    LogFileIo("File opened path=%s access=%s handle=%p", path, access, handle);
}

// Return whether a path's file name ends with the given extension (no dot), case-insensitively.
static bool HasFileExtension(const char* path, const char* extension) {
    if (!path) {
        return false;
    }
    size_t pathLength = strlen(path);
    size_t extensionLength = strlen(extension);
    if (pathLength < extensionLength + 1) {
        return false;
    }
    const char* candidate = path + pathLength - extensionLength;
    return candidate[-1] == '.' && EqualsIgnoreCase(candidate, extension);
}

// The base game's own "mp\N.mp" per-map counter files (1/2/5/6/7.mp seen so
// far - the game writes these directly, not um.dll) get rewritten in tight
// 4-byte bursts during map loads, exactly the window heap-corruption crashes
// have been reproduced in. Knowing the actual VALUE (not just "4 bytes were
// written somewhere") is what makes this actionable, so dump it as hex and,
// since 4 bytes is also plausibly a float, as a float too.
static bool ShouldDumpFileContents(const char* path) {
    return path && HasFileExtension(path, "mp") && strstr(path, "\\mp\\") != NULL;
}

static void LogFileContents(const char* direction, const char* path, const void* buffer, DWORD length) {
    if (!ShouldDumpFileContents(path) || !buffer || length == 0) {
        return;
    }
    DWORD toShow = length < 32 ? length : 32;
    char hex[3 * 32 + 1] = {};
    for (DWORD i = 0; i < toShow; ++i) {
        snprintf(hex + i * 3, 4, "%02X ", static_cast<const unsigned char*>(buffer)[i]);
    }
    if (length == 4) {
        int32_t asInt = 0;
        float asFloat = 0.0f;
        memcpy(&asInt, buffer, 4);
        memcpy(&asFloat, buffer, 4);
        LogFileIo("%s contents path=%s bytes=%s(as int32=%d as float=%g)", direction, path, hex, asInt, asFloat);
    } else {
        LogFileIo("%s contents path=%s bytes=%s%s", direction, path, hex, length > 32 ? "..." : "");
    }
}

// Node type IDs, taken from ei_maper's own reader (util::CMobParser::initTypes).
// Every node is a flat type(4-byte LE) + length(4-byte LE) header where length
// covers the header itself plus the payload, so any node can be skipped purely
// by its own declared length regardless of what it contains.
static const DWORD kMobTypeRoot = 0;
static const DWORD kMobTypeObjectDbFile = 40960;
static const DWORD kMobTypeObjectSection = 45056;
static const DWORD kMobTypeNid = 45058;
static const DWORD kMobTypeObjName = 45060;
static const DWORD kMobTypeUnit = 3149594624;
static const DWORD kMobTypeUnitQuestItems = 3149594629;
static const DWORD kMobTypeUnitQuickItems = 3149594630;
static const DWORD kMobTypeUnitSpells = 3149594631;
static const DWORD kMobTypeUnitWeapons = 3149594632;
static const DWORD kMobTypeUnitArmors = 3149594633;
static const size_t kMobMaxWalkBytes = 16 * 1024 * 1024;

// Read one node header at offset; returns false if it doesn't fit in [0, size).
static bool ReadMobNodeHeader(const BYTE* data, size_t size, size_t offset, DWORD* type, DWORD* length) {
    if (offset + 8 > size) {
        return false;
    }
    memcpy(type, data + offset, sizeof(*type));
    memcpy(length, data + offset + 4, sizeof(*length));
    return true;
}

// Read a whole file (bounded) into a heap buffer using the real CreateFileA,
// independent of any handle the game itself has open on the same path.
static bool ReadWholeFileForDatabase(const char* path, BYTE** outData, size_t* outSize) {
    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(handle);
        return false;
    }
    DWORD toRead = static_cast<DWORD>(size.QuadPart);
    BYTE* buffer = static_cast<BYTE*>(malloc(toRead));
    DWORD bytesRead = 0;
    BOOL ok = buffer && ReadFile(handle, buffer, toRead, &bytesRead, NULL);
    CloseHandle(handle);
    if (!ok || bytesRead != toRead) {
        free(buffer);
        return false;
    }
    *outData = buffer;
    *outSize = bytesRead;
    return true;
}

// The item/spell/armor database files (database.res, databaselmp.res,
// databaseadb.res) are themselves .res archives holding records in the
// tagged-value format documented in docs/file-formats/database-format.md:
// every leaf string is [tag:1][length][cp1251 bytes + null terminator],
// where `length` is a single even byte giving 2x the payload size, or (when
// that byte is odd) the low byte of a 4-byte little-endian value giving
// 2x+1. This walks every byte position as a candidate [tag,length] header,
// and accepts it only when the payload it implies fits in the buffer and
// ends with the required null terminator - i.e. it follows the real record
// framing instead of guessing from byte values alone. The accepted name is
// the leading printable-ASCII prefix of that payload (most fields are pure
// ASCII identifiers already, but Lever "Lever Text" fields mix an ASCII
// prefix with localized Cyrillic text in the same field, so the prefix is
// taken rather than requiring the whole payload to be ASCII).
//
// This replaces an earlier version that scanned for bare printable-ASCII
// runs (any 3+ character stretch, with a defensive "drop the first
// character" fallback for when a preceding length/tag byte itself happened
// to be printable and bled into the run). That version is retired in favor
// of exact framing now that the format is fully understood; re-verified
// against every name the old heuristic ever produced for both database.res
// and databaselmp.res, confirming this version recovers the same real
// identifiers (usually as a single clean entry rather than several noisy
// off-by-one variants of one), and the only names it does NOT reproduce are
// the RES container's own internal member filenames (e.g. "acks.db") and a
// handful of coincidental byte-value matches inside unrelated binary
// fields - neither of which is a real item/spell/material identifier a
// .mob file could legitimately reference.
static void ExtractDatabaseNames(const BYTE* data, size_t size, std::unordered_set<std::string>& names) {
    static const size_t kMaxPayloadSize = 4096; // no real database field is anywhere near this long
    if (size < 2) {
        return;
    }
    for (size_t p = 0; p + 1 < size; ++p) {
        BYTE lengthByte = data[p + 1];
        size_t rawPayloadSize;
        size_t payloadStart;
        if ((lengthByte % 2) == 0) {
            rawPayloadSize = lengthByte / 2;
            payloadStart = p + 2;
        } else {
            if (p + 5 > size) {
                continue;
            }
            DWORD wideLength = 0;
            memcpy(&wideLength, data + p + 1, sizeof(wideLength));
            if ((wideLength % 2) == 0) {
                continue; // the wide form's low byte must be odd to be plausible at all
            }
            rawPayloadSize = (wideLength - 1) / 2;
            payloadStart = p + 5;
        }
        if (rawPayloadSize == 0 || rawPayloadSize > kMaxPayloadSize) {
            continue;
        }
        size_t payloadEnd = payloadStart + rawPayloadSize;
        if (payloadEnd > size || data[payloadEnd - 1] != 0x00) {
            continue;
        }

        size_t j = payloadStart;
        while (j < payloadEnd - 1 && data[j] >= 0x20 && data[j] <= 0x7E) {
            ++j;
        }
        size_t prefixLength = j - payloadStart;
        if (prefixLength >= 1) {
            std::string run(reinterpret_cast<const char*>(data + payloadStart), prefixLength);
            for (size_t k = 0; k < run.size(); ++k) {
                run[k] = static_cast<char>(tolower(static_cast<unsigned char>(run[k])));
            }
            names.insert(std::move(run));
        }
    }
}

// Load every database.res/databaselmp.res/databaseadb.res found beside the
// game executable and under each installed mod's own "res" folder, since the
// mod that ships a map is not necessarily the one that ships the database
// (the loader merges mod resources).
static void LoadDatabaseFilesFromDirectory(const char* directory, std::unordered_set<std::string>& names) {
    static const char* kDatabaseFileNames[] = { "database.res", "databaselmp.res", "databaseadb.res" };
    for (size_t i = 0; i < sizeof(kDatabaseFileNames) / sizeof(kDatabaseFileNames[0]); ++i) {
        char path[MAX_PATH] = {};
        _snprintf(path, sizeof(path) - 1, "%s\\%s", directory, kDatabaseFileNames[i]);
        BYTE* data = NULL;
        size_t size = 0;
        if (ReadWholeFileForDatabase(path, &data, &size)) {
            ExtractDatabaseNames(data, size, names);
            free(data);
        }
    }
}

// Find a "\Mods\" path component (case-insensitive) in a path and copy the
// directory up to and including "Mods" into outModsRoot. Deriving the mods
// root from the .mob file's own path is more reliable than assuming it is a
// sibling of game.exe, since the executable is not always installed at the
// root of the mod tree.
static bool FindModsRootFromPath(const char* path, char* outModsRoot, size_t outSize) {
    size_t length = strlen(path);
    for (size_t i = 0; i + 5 <= length; ++i) {
        bool isBoundaryBefore = (i == 0) || path[i - 1] == '\\' || path[i - 1] == '/';
        bool isBoundaryAfter = path[i + 4] == '\\' || path[i + 4] == '/';
        if (isBoundaryBefore && isBoundaryAfter &&
                tolower(static_cast<unsigned char>(path[i])) == 'm' &&
                tolower(static_cast<unsigned char>(path[i + 1])) == 'o' &&
                tolower(static_cast<unsigned char>(path[i + 2])) == 'd' &&
                tolower(static_cast<unsigned char>(path[i + 3])) == 's') {
            size_t modsRootLength = i + 4;
            if (modsRootLength >= outSize) {
                return false;
            }
            memcpy(outModsRoot, path, modsRootLength);
            outModsRoot[modsRootLength] = '\0';
            return true;
        }
    }
    return false;
}

// Enumerate every "<modsRoot>\<mod>\res" directory and load its database files.
static void LoadDatabaseFilesFromModsRoot(const char* modsRoot, std::unordered_set<std::string>& names) {
    char modsPattern[MAX_PATH] = {};
    _snprintf(modsPattern, sizeof(modsPattern) - 1, "%s\\*", modsRoot);
    WIN32_FIND_DATAA findData = {};
    HANDLE find = FindFirstFileA(modsPattern, &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            char modResDir[MAX_PATH] = {};
            _snprintf(modResDir, sizeof(modResDir) - 1, "%s\\%s\\res", modsRoot, findData.cFileName);
            LoadDatabaseFilesFromDirectory(modResDir, names);
        }
    } while (FindNextFileA(find, &findData));
    FindClose(find);
}

// Return the cached set of valid item/spell/armor names, loading it from disk
// on first use. Loading is attempted only once per process even if no
// database files are found, so a missing database cannot repeatedly hit disk.
// The mods root is derived from the .mob file's own path (reliable) rather
// than assumed to be beside game.exe (which may live in a different folder).
static std::unordered_set<std::string>& GetMobDatabaseNames(const char* mobPath) {
    static std::unordered_set<std::string> names;
    static bool loaded = false;
    if (loaded) {
        return names;
    }
    loaded = true;

    char modsRoot[MAX_PATH] = {};
    if (mobPath && FindModsRootFromPath(mobPath, modsRoot, sizeof(modsRoot))) {
        LoadDatabaseFilesFromModsRoot(modsRoot, names);
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
        char* slash = strrchr(exePath, '\\');
        if (slash) {
            *slash = '\0';
        }
        char baseResDir[MAX_PATH] = {};
        _snprintf(baseResDir, sizeof(baseResDir) - 1, "%s\\res", exePath);
        LoadDatabaseFilesFromDirectory(baseResDir, names);

        char exeModsRoot[MAX_PATH] = {};
        _snprintf(exeModsRoot, sizeof(exeModsRoot) - 1, "%s\\Mods", exePath);
        LoadDatabaseFilesFromModsRoot(exeModsRoot, names);
    }

    LogLine("DEBUG", "[MOBCHECK] loaded %zu candidate item/spell/armor names for database validation", names.size());
    return names;
}

// Strip a trailing "[...]" annotation (enchantment/count) and surrounding
// whitespace, e.g. "iron [prot_fire {ic; e2; d2}] " -> "iron".
static std::string StripBracketAnnotation(const std::string& value) {
    size_t bracket = value.find('[');
    std::string result = (bracket == std::string::npos) ? value : value.substr(0, bracket);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

static std::string ToLowerString(const std::string& value) {
    std::string result = value;
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

// Check a weapon/armor name ("template.material") against the database,
// tolerating a bracketed annotation on the material part.
static bool IsKnownWeaponOrArmorName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    size_t dot = lower.rfind('.');
    if (dot == std::string::npos) {
        return names.count(lower) != 0;
    }
    std::string templateName = lower.substr(0, dot);
    std::string material = StripBracketAnnotation(lower.substr(dot + 1));
    return names.count(templateName) != 0 && names.count(material) != 0;
}

// Check a spell name, stripping its "{param,...}" argument list first.
static bool IsKnownSpellName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    size_t brace = lower.find('{');
    std::string base = (brace == std::string::npos) ? lower : lower.substr(0, brace);
    return names.count(base) != 0;
}

// Check a quest/quick item name, tolerating a bracketed annotation, or
// falling back to the weapon/armor "template.material" check for items that
// use that same naming convention (e.g. "material.scrab bones [2]").
static bool IsKnownSimpleItemName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    if (names.count(lower) != 0 || names.count(StripBracketAnnotation(lower)) != 0) {
        return true;
    }
    if (lower.find('.') != std::string::npos) {
        return IsKnownWeaponOrArmorName(name, names);
    }
    return false;
}

// A unit's own equipment/spell/quest-item lists are stored as: a 4-byte count,
// then that many [type(4), length(4), name bytes] entries. A blank entry name
// is not a legitimate item and is a reliable sign of a corrupted item list;
// this was confirmed by comparing a known-good and a known-bad copy of the
// same map (the bad copy had one blank armor slot appended) and by scanning
// every real .mob file shipped with the mod, none of which has a blank entry.
// When enabled, non-blank entries are additionally checked against the
// item/spell database so a typo'd or made-up name is reported before the
// game can crash on it, instead of only catching blanks; this is always on
// alongside structural validation, since checking structure without also
// checking item/spell/armor names against the database misses most real bugs.
static void ValidateMobStringArray(const char* path, const BYTE* data, size_t payloadStart,
        size_t payloadEnd, const char* fieldName, int fieldKind, const char* unitLabel, bool* hasError) {
    if (payloadStart + 4 > payloadEnd) {
        return;
    }
    DWORD count = 0;
    memcpy(&count, data + payloadStart, sizeof(count));
    size_t pos = payloadStart + 4;
    for (DWORD i = 0; i < count; ++i) {
        DWORD entryType = 0, entryLength = 0;
        if (pos + 8 > payloadEnd) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a truncated %s list (expected %lu entries)",
                path, unitLabel, fieldName, count);
            *hasError = true;
            return;
        }
        memcpy(&entryType, data + pos, sizeof(entryType));
        memcpy(&entryLength, data + pos + 4, sizeof(entryLength));
        if (entryLength < 8 || pos + entryLength > payloadEnd) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a %s entry with an invalid length %lu",
                path, unitLabel, fieldName, entryLength);
            *hasError = true;
            return;
        }
        if (entryLength == 8) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a blank entry (#%lu of %lu) in its %s list",
                path, unitLabel, i + 1, count, fieldName, fieldName);
            *hasError = true;
        } else {
            std::string entryName(reinterpret_cast<const char*>(data + pos + 8), entryLength - 8);
            const std::unordered_set<std::string>& databaseNames = GetMobDatabaseNames(path);
            bool known;
            if (fieldKind == 0) known = IsKnownWeaponOrArmorName(entryName, databaseNames);
            else if (fieldKind == 1) known = IsKnownSpellName(entryName, databaseNames);
            else known = IsKnownSimpleItemName(entryName, databaseNames);
            if (!known) {
                LogLine("ERROR", "[MOBCHECK] %s unit %s has a %s entry '%s' that does not exist in the item/spell database",
                    path, unitLabel, fieldName, entryName.c_str());
                *hasError = true;
            }
        }
        pos += entryLength;
    }
}

// Walk a single UNIT record's fields, checking its equipment/spell/quest lists.
// Fields are not stored in a fixed order (NID/OBJ_NAME can appear before or
// after the item lists), so the unit's name/NID are collected in a first pass
// before the item lists are validated in a second pass, so errors can always
// name the unit instead of only reporting a raw file offset.
static void ValidateMobUnit(const char* path, const BYTE* data, size_t unitStart, size_t unitEnd, bool* hasError) {
    char unitLabel[160] = {};
    strcpy(unitLabel, "(unnamed)");
    DWORD nid = 0;
    bool haveNid = false;
    size_t pos = unitStart + 8;
    while (pos + 8 <= unitEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, unitEnd, pos, &type, &length) || length < 8 || pos + length > unitEnd) {
            break;
        }
        if (type == kMobTypeNid && length == 12) {
            memcpy(&nid, data + pos + 8, sizeof(nid));
            haveNid = true;
        } else if (type == kMobTypeObjName) {
            size_t nameLength = length - 8;
            if (nameLength >= sizeof(unitLabel)) {
                nameLength = sizeof(unitLabel) - 1;
            }
            memcpy(unitLabel, data + pos + 8, nameLength);
            unitLabel[nameLength] = '\0';
        }
        pos += length;
    }

    char labelWithNid[192] = {};
    if (haveNid) {
        _snprintf(labelWithNid, sizeof(labelWithNid) - 1, "'%s' (NID %lu)", unitLabel, nid);
    } else {
        _snprintf(labelWithNid, sizeof(labelWithNid) - 1, "'%s'", unitLabel);
    }

    pos = unitStart + 8;
    while (pos + 8 <= unitEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, unitEnd, pos, &type, &length) || length < 8 || pos + length > unitEnd) {
            return;
        }
        const char* fieldName = NULL;
        int fieldKind = 2; // 0=weapon/armor (template.material), 1=spell ({params}), 2=quest/quick (simple)
        if (type == kMobTypeUnitQuestItems) fieldName = "quest item";
        else if (type == kMobTypeUnitQuickItems) fieldName = "quick item";
        else if (type == kMobTypeUnitSpells) { fieldName = "spell"; fieldKind = 1; }
        else if (type == kMobTypeUnitWeapons) { fieldName = "weapon"; fieldKind = 0; }
        else if (type == kMobTypeUnitArmors) { fieldName = "armor"; fieldKind = 0; }
        if (fieldName) {
            ValidateMobStringArray(path, data, pos + 8, pos + length, fieldName, fieldKind, labelWithNid, hasError);
        }
        pos += length;
    }
}

// Walk the OBJECT_SECTION node's direct children and validate every UNIT found.
static void ValidateMobObjectSection(const char* path, const BYTE* data, size_t sectionStart, size_t sectionEnd, bool* hasError) {
    size_t pos = sectionStart + 8;
    while (pos + 8 <= sectionEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, sectionEnd, pos, &type, &length) || length < 8 || pos + length > sectionEnd) {
            LogLine("ERROR", "[MOBCHECK] %s has a corrupted object entry inside OBJECT_SECTION", path);
            *hasError = true;
            return;
        }
        if (type == kMobTypeUnit) {
            ValidateMobUnit(path, data, pos, pos + length, hasError);
        }
        pos += length;
    }
}

// ---------------------------------------------------------------------------
// Mission script checking
//
// A .mob carries its mission script in an SS_TEXT node (encrypted; see
// docs/file-formats/mob-format.md). The game loads a QUEST map (zNqM.mob) on top
// of its zone's base map (zoneN-lmp.mob) and the quest's script freely uses the
// base map's variables, so a quest is checked together with its base: the quest's
// own zNqM.mq archive holds a map.txt whose "#res" line names the zone's base map.
// Object IDs must not repeat between the two - the quest's copy silently replaces
// the base map's object - which is reported too.
// ---------------------------------------------------------------------------
static const DWORD kMobTypeSsTextOld = 2899242186u; // plain-text script (older files)
static const DWORD kMobTypeSsText = 2899242187u;    // encrypted script
static const DWORD kMqMagic = 0x019CE23Cu;
static const size_t kMqMaxBytes = 8u * 1024 * 1024;

// MSVC LCG XOR cipher over everything after the 4-byte key; NUL bytes are padding.
static std::string DecryptMobScript(const BYTE* payload, size_t length) {
    std::string text;
    if (length < 4) {
        return text;
    }
    DWORD key = 0;
    memcpy(&key, payload, sizeof(key));
    text.reserve(length - 4);
    for (size_t i = 4; i < length; ++i) {
        key = key * 214013u + 2531011u;
        BYTE plain = static_cast<BYTE>(payload[i] ^ ((key >> 16) & 0xFF));
        if (plain != 0) {
            text.push_back(static_cast<char>(plain));
        }
    }
    return text;
}

// Pull the mission script text and every object's NID out of a .mob image.
static void ExtractMobScriptAndIds(const BYTE* data, size_t size, std::string* scriptText,
        std::unordered_set<DWORD>* objectIds, std::unordered_set<std::string>* objectNames = nullptr) {
    size_t pos = 16; // past the root node and the marker node, like ValidateMobFile
    while (pos + 8 <= size) {
        DWORD type = 0, length = 0;
        memcpy(&type, data + pos, sizeof(type));
        memcpy(&length, data + pos + 4, sizeof(length));
        if (type == kMobTypeRoot || length < 8 || pos + length > size) {
            break;
        }
        if (type == kMobTypeSsText) {
            *scriptText = DecryptMobScript(data + pos + 8, length - 8);
        } else if (type == kMobTypeSsTextOld) {
            scriptText->clear();
            for (size_t i = 0; i < length - 8; ++i) {
                if (data[pos + 8 + i] != 0) scriptText->push_back(static_cast<char>(data[pos + 8 + i]));
            }
        } else if (type == kMobTypeObjectSection) {
            size_t sectionEnd = pos + length;
            size_t child = pos + 8;
            while (child + 8 <= sectionEnd) {
                DWORD childType = 0, childLength = 0;
                memcpy(&childType, data + child, sizeof(childType));
                memcpy(&childLength, data + child + 4, sizeof(childLength));
                if (childLength < 8 || child + childLength > sectionEnd) {
                    break;
                }
                size_t field = child + 8;
                size_t childEnd = child + childLength;
                while (field + 8 <= childEnd) {
                    DWORD fieldType = 0, fieldLength = 0;
                    memcpy(&fieldType, data + field, sizeof(fieldType));
                    memcpy(&fieldLength, data + field + 4, sizeof(fieldLength));
                    if (fieldLength < 8 || field + fieldLength > childEnd) {
                        break;
                    }
                    if (fieldType == kMobTypeNid && fieldLength == 12) {
                        DWORD nid = 0;
                        memcpy(&nid, data + field + 8, sizeof(nid));
                        objectIds->insert(nid);
                    } else if (fieldType == kMobTypeObjName && objectNames) {
                        std::string objectName(reinterpret_cast<const char*>(data + field + 8), fieldLength - 8);
                        objectName.resize(strlen(objectName.c_str())); // stop at the terminating NUL
                        for (size_t i = 0; i < objectName.size(); ++i) {
                            objectName[i] = static_cast<char>(tolower(static_cast<unsigned char>(objectName[i])));
                        }
                        objectNames->insert(objectName);
                    }
                    field += fieldLength;
                }
                child = childEnd;
            }
        }
        pos += length;
    }
}

static bool ReadWholeFile(const std::string& path, size_t limitBytes, std::vector<BYTE>* out) {
    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
        static_cast<ULONGLONG>(size.QuadPart) <= limitBytes;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, out->data(), static_cast<DWORD>(out->size()), &read, NULL) && read == out->size();
    }
    CloseHandle(handle);
    return ok;
}

// The base map's name from a quest archive's map.txt ("#res <mpr> <base mob>"), without extension.
static bool ReadMqBaseMapName(const std::string& mqPath, std::string* baseName, std::string* mprName = nullptr) {
    std::vector<BYTE> file;
    if (!ReadWholeFile(mqPath, kMqMaxBytes, &file) || file.size() < 16) {
        return false;
    }
    DWORD magic = 0, count = 0, tableOffset = 0, namesLength = 0;
    memcpy(&magic, file.data(), 4);
    memcpy(&count, file.data() + 4, 4);
    memcpy(&tableOffset, file.data() + 8, 4);
    memcpy(&namesLength, file.data() + 12, 4);
    if (magic != kMqMagic || count == 0 || count > 65536) {
        return false;
    }
    // Each descriptor is 22 bytes: next(4) length(4) offset(4) timestamp(4) nameLength(2) nameOffset(4).
    unsigned long long namesOffset = static_cast<unsigned long long>(tableOffset) + static_cast<unsigned long long>(count) * 22;
    if (namesOffset + namesLength > file.size()) {
        return false;
    }
    for (DWORD i = 0; i < count; ++i) {
        const BYTE* descriptor = file.data() + tableOffset + static_cast<size_t>(i) * 22;
        DWORD dataLength = 0, dataOffset = 0, nameOffset = 0;
        WORD nameLength = 0;
        memcpy(&dataLength, descriptor + 4, 4);
        memcpy(&dataOffset, descriptor + 8, 4);
        memcpy(&nameLength, descriptor + 16, 2);
        memcpy(&nameOffset, descriptor + 18, 4);
        if (nameLength < 7 || static_cast<unsigned long long>(nameOffset) + nameLength > namesLength) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(file.data() + namesOffset + nameOffset), nameLength);
        if (!EqualsIgnoreCase(name.substr(name.size() - 7).c_str(), "map.txt")) {
            continue;
        }
        if (static_cast<unsigned long long>(dataOffset) + dataLength > file.size()) {
            return false;
        }
        std::string text(reinterpret_cast<const char*>(file.data() + dataOffset), dataLength);
        size_t at = 0;
        bool afterRes = false;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            if (afterRes) {
                size_t space = line.find_first_of(" \t");
                if (space == std::string::npos) return false;
                if (mprName) *mprName = line.substr(0, space);
                size_t second = line.find_first_not_of(" \t", space);
                if (second == std::string::npos) return false;
                size_t secondEnd = line.find_first_of(" \t", second);
                *baseName = line.substr(second, secondEnd == std::string::npos ? std::string::npos : secondEnd - second);
                return !baseName->empty();
            }
            if (line.size() >= 4 && EqualsIgnoreCase(line.substr(0, 4).c_str(), "#res")) {
                afterRes = true;
            }
        }
        return false;
    }
    return false;
}

struct MobScriptContext {
    MobScriptDeclarations declarations;
    std::unordered_set<DWORD> objectIds;
    std::unordered_set<std::string> objectNames; // lower-case OBJNAMEs of the map's objects
    std::vector<std::string> addMobs;            // lower-case file names its script loads with AddMob
};

// Where the game itself opened each .mob (lower-case file name -> full path). A quest's base
// map is opened by the game just before the quest, often from a different folder (a mod's
// quests sit in the mod's maps folder, the zone's base map in the game's own), so this is
// how it is found when it is not next to the quest.
static volatile LONG g_seenMobPathsLock = 0;
static std::unordered_map<std::string, std::string> g_seenMobPaths;

static std::string LowerCaseCopy(const std::string& text) {
    std::string result = text;
    for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    return result;
}

static void RememberSeenMobPath(const char* path) {
    std::string full = path;
    size_t slash = full.find_last_of("\\/");
    std::string name = LowerCaseCopy(slash == std::string::npos ? full : full.substr(slash + 1));
    while (InterlockedCompareExchange(&g_seenMobPathsLock, 1, 0) != 0) Sleep(0);
    g_seenMobPaths[name] = full;
    InterlockedExchange(&g_seenMobPathsLock, 0);
}

static std::string FindSeenMobPath(const std::string& mobFileName) {
    std::string name = LowerCaseCopy(mobFileName);
    while (InterlockedCompareExchange(&g_seenMobPathsLock, 1, 0) != 0) Sleep(0);
    auto it = g_seenMobPaths.find(name);
    std::string result = it != g_seenMobPaths.end() ? it->second : std::string();
    InterlockedExchange(&g_seenMobPathsLock, 0);
    return result;
}

// Base maps are read on demand (whether or not the game opened them yet) and kept.
static volatile LONG g_mobContextLock = 0;
static std::unordered_map<std::string, std::shared_ptr<MobScriptContext>> g_mobContextCache;

static std::shared_ptr<MobScriptContext> LoadMobContext(const std::string& mobPath) {
    std::string key = mobPath;
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
    while (InterlockedCompareExchange(&g_mobContextLock, 1, 0) != 0) Sleep(0);
    auto cached = g_mobContextCache.find(key);
    std::shared_ptr<MobScriptContext> result = cached != g_mobContextCache.end() ? cached->second : nullptr;
    InterlockedExchange(&g_mobContextLock, 0);
    if (result) {
        return result;
    }
    std::vector<BYTE> file;
    if (!ReadWholeFile(mobPath, kMobMaxWalkBytes, &file) || file.size() < 16) {
        return nullptr;
    }
    DWORD rootType = 0;
    memcpy(&rootType, file.data(), sizeof(rootType));
    if (rootType != kMobTypeObjectDbFile) {
        return nullptr;
    }
    result = std::make_shared<MobScriptContext>();
    std::string scriptText;
    ExtractMobScriptAndIds(file.data(), file.size(), &scriptText, &result->objectIds, &result->objectNames);
    if (!scriptText.empty()) {
        MobScriptReport scriptReport = CheckMobScript(scriptText);
        result->declarations = scriptReport.declarations;
        for (const std::string& target : scriptReport.addMobs) {
            std::string name = LowerCaseCopy(target);
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".mob") != 0) name += ".mob";
            result->addMobs.push_back(name);
        }
    }
    while (InterlockedCompareExchange(&g_mobContextLock, 1, 0) != 0) Sleep(0);
    g_mobContextCache[key] = result;
    InterlockedExchange(&g_mobContextLock, 0);
    return result;
}

// Quest maps are named z<zone>q<n> (z12q2, z3xq1, z11d2q1, z0jq1); base maps are zone<N>-lmp
// and the like. A quest whose archive (and so its base map) cannot be found must not be
// held to "every object it mentions is in this file".
static bool LooksLikeQuestMapName(const std::string& fileName) {
    std::string name = LowerCaseCopy(fileName);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.size() < 4 || name[0] != 'z' || name[1] < '0' || name[1] > '9') {
        return false;
    }
    size_t q = name.find_last_of('q');
    if (q == std::string::npos || q < 2 || q + 1 >= name.size()) {
        return false;
    }
    for (size_t i = q + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static std::string MobFileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Is this text argument an item/spell the database knows? Same shape rule as the
// unit weapon/armor check: drop a spell's "{parameters}" and an item's "[count]",
// then every dot-separated part must be a database name ("material.iron[1]").
static bool ScriptDatabaseNameKnown(const std::string& raw, const std::unordered_set<std::string>& names) {
    std::string text = LowerCaseCopy(raw);
    if (names.count(text) != 0) {
        return true;
    }
    size_t brace = text.find('{');
    if (brace != std::string::npos) text.resize(brace);
    size_t bracket = text.find('[');
    if (bracket != std::string::npos) text.resize(bracket);
    size_t start = 0;
    bool any = false;
    while (start <= text.size()) {
        size_t dot = text.find('.', start);
        std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) {
            any = true;
            if (names.count(part) == 0) return false;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return any;
}

// What a script refers to inside the map, checked against the map's own objects: the IDs
// and names of this map, its base map (for a quest) and every map the script loads with
// AddMob(). Findings are summarised per script, since a stale reference usually repeats.
// Skipped whenever one of those maps could not be read, so a map that simply is not
// available never produces a false warning.
static void ValidateScriptAgainstMap(const char* path, const std::string& fileName, const MobScriptReport& report,
        const std::unordered_set<DWORD>& ownIds, const std::unordered_set<std::string>& ownNames,
        const std::shared_ptr<MobScriptContext>& base, bool baseMissing, bool* hasError) {
    (void)hasError;
    // --- item / spell names against the database
    const std::unordered_set<std::string>& databaseNames = GetMobDatabaseNames(path);
    if (!databaseNames.empty()) {
        std::unordered_set<std::string> reported;
        for (const MobScriptDatabaseName& ref : report.databaseNames) {
            if (ScriptDatabaseNameKnown(ref.text, databaseNames) || !reported.insert(LowerCaseCopy(ref.text)).second) {
                continue;
            }
            LogLine("WARN", "[MOBCHECK] %s script line %d: %s(): '%s' is not %s in the database", fileName.c_str(),
                ref.line, ref.command.c_str(), ref.text.c_str(), ref.kind == 's' ? "a spell" : "an item");
        }
    }

    // --- object IDs and names against the map's objects
    if (baseMissing || (report.objectIds.empty() && report.objectNames.empty())) {
        return;
    }
    std::unordered_set<DWORD> ids = ownIds;
    std::unordered_set<std::string> names = ownNames;
    if (base) {
        ids.insert(base->objectIds.begin(), base->objectIds.end());
        names.insert(base->objectNames.begin(), base->objectNames.end());
    }
    std::string directory;
    size_t slash = std::string(path).find_last_of("\\/");
    if (slash != std::string::npos) directory = std::string(path).substr(0, slash + 1);
    for (const std::string& target : report.addMobs) {
        std::string name = target;
        if (name.size() < 4 || !EqualsIgnoreCase(name.substr(name.size() - 4).c_str(), ".mob")) name += ".mob";
        std::shared_ptr<MobScriptContext> added = LoadMobContext(directory + name);
        if (!added) {
            std::string seen = FindSeenMobPath(name);
            if (!seen.empty()) added = LoadMobContext(seen);
        }
        if (!added) {
            LogLine("DEBUG", "[MOBCHECK] %s loads '%s' with AddMob, which was not found; object IDs and names "
                "in its script are not checked", fileName.c_str(), target.c_str());
            return;
        }
        ids.insert(added->objectIds.begin(), added->objectIds.end());
        names.insert(added->objectNames.begin(), added->objectNames.end());
    }

    char examples[200] = {};
    size_t used = 0;
    int firstLine = 0;
    int missingIds = 0;
    std::unordered_set<std::string> seenIds;
    for (const MobScriptReference& ref : report.objectIds) {
        unsigned long id = strtoul(ref.text.c_str(), NULL, 10);
        if (ref.text.size() > 10 || ids.count(static_cast<DWORD>(id)) != 0 || !seenIds.insert(ref.text).second) {
            continue;
        }
        if (missingIds == 0) firstLine = ref.line;
        if (missingIds < 5) {
            int wrote = snprintf(examples + used, sizeof(examples) - used, "%s%s", missingIds ? ", " : "", ref.text.c_str());
            if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(examples) - used) used += static_cast<size_t>(wrote);
        }
        ++missingIds;
    }
    if (missingIds > 0) {
        LogLine("WARN", "[MOBCHECK] %s script line %d: refers to %d object ID(s) that no object has in this map, its base "
            "map or the maps it loads with AddMob (e.g. %s); commands using them get no object", fileName.c_str(),
            firstLine, missingIds, examples);
    }

    char nameExamples[200] = {};
    used = 0;
    int missingNames = 0;
    firstLine = 0;
    for (const MobScriptReference& ref : report.objectNames) {
        if (names.count(LowerCaseCopy(ref.text)) != 0) {
            continue;
        }
        if (missingNames == 0) firstLine = ref.line;
        if (missingNames < 5) {
            int wrote = snprintf(nameExamples + used, sizeof(nameExamples) - used, "%s'%s'", missingNames ? ", " : "", ref.text.c_str());
            if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(nameExamples) - used) used += static_cast<size_t>(wrote);
        }
        ++missingNames;
    }
    if (missingNames > 0) {
        LogLine("WARN", "[MOBCHECK] %s script line %d: uses %d object name(s) that no object is called in this map, its "
            "base map or the maps it loads with AddMob: %s", fileName.c_str(), firstLine, missingNames, nameExamples);
    }
}

// Check the mission script of one .mob image (already validated structurally) and, for a
// quest map, its object IDs against its base map. Findings are logged; errors set *hasError.
static void ValidateMobScript(const char* path, const BYTE* data, size_t size, bool* hasError) {
    try {
        std::string mobPath = path;
        std::string fileName = MobFileNameOf(mobPath);
        std::string scriptText;
        std::unordered_set<DWORD> objectIds;
        std::unordered_set<std::string> objectNamesOfThisMap;
        ExtractMobScriptAndIds(data, size, &scriptText, &objectIds, &objectNamesOfThisMap);

        // A quest map has a sibling .mq archive naming its base map.
        std::shared_ptr<MobScriptContext> base;
        std::string baseName;
        bool baseMissing = false;
        size_t dot = mobPath.find_last_of('.');
        if (dot != std::string::npos) {
            std::string mqPath = mobPath.substr(0, dot) + ".mq";
            if (GetFileAttributesA(mqPath.c_str()) != INVALID_FILE_ATTRIBUTES && ReadMqBaseMapName(mqPath, &baseName)) {
                size_t slash = mobPath.find_last_of("\\/");
                std::string directory = slash == std::string::npos ? std::string() : mobPath.substr(0, slash + 1);
                base = LoadMobContext(directory + baseName + ".mob");
                if (!base) {
                    std::string seen = FindSeenMobPath(baseName + ".mob");
                    if (!seen.empty()) {
                        base = LoadMobContext(seen);
                    }
                }
                if (!base) {
                    baseMissing = true;
                    LogLine("DEBUG", "[MOBCHECK] %s is a quest map for base map '%s', which was not found next to it or opened by the "
                        "game before it; variables it takes from the base map cannot be resolved", fileName.c_str(), baseName.c_str());
                }
            }
        }

        if (!base && !baseMissing && LooksLikeQuestMapName(fileName)) {
            baseMissing = true; // a quest map whose archive (and base map) is not available
        }

        if (scriptText.empty()) {
            LogLine("DEBUG", "[MOBCHECK] %s has no mission script", fileName.c_str());
        } else {
            MobScriptReport report = CheckMobScript(scriptText, base ? &base->declarations : nullptr, baseMissing);
            if (report.errors > 0) {
                *hasError = true;
            }
            std::stable_sort(report.issues.begin(), report.issues.end(),
                [](const MobScriptIssue& x, const MobScriptIssue& y) { return x.line < y.line; });
            for (const MobScriptIssue& issue : report.issues) {
                const char* level = issue.severity == 'E' ? "ERROR" : issue.severity == 'W' ? "WARN" : "DEBUG";
                LogLine(level, "[MOBCHECK] %s script line %d: %s", fileName.c_str(), issue.line, issue.message.c_str());
            }
            if (report.suppressed > 0) {
                LogLine("WARN", "[MOBCHECK] %s script: %d more finding(s) not shown", fileName.c_str(), report.suppressed);
            }
            ValidateScriptAgainstMap(path, fileName, report, objectIds, objectNamesOfThisMap, base,
                baseMissing, hasError);
            if (report.errors > 0 || report.warnings > 0) {
                LogLine("INFO", "[MOBCHECK] %s script: %d error(s), %d warning(s) in %d script(s); line numbers count "
                    "lines of the map's script text (um-multitool mobdump writes it as %s.eis)",
                    fileName.c_str(), report.errors, report.warnings, report.scriptCount,
                    fileName.substr(0, fileName.find_last_of('.')).c_str());
            } else {
                LogLine("DEBUG", "[MOBCHECK] %s script checked OK (%d script(s)%s%s)", fileName.c_str(), report.scriptCount,
                    base ? ", with base map " : "", base ? baseName.c_str() : "");
            }
        }

        if (base && !objectIds.empty()) {
            std::vector<DWORD> shared;
            for (DWORD id : objectIds) {
                if (base->objectIds.count(id) != 0) shared.push_back(id);
            }
            if (!shared.empty()) {
                std::sort(shared.begin(), shared.end());
                char examples[160] = {};
                size_t used = 0;
                for (size_t i = 0; i < shared.size() && i < 5; ++i) {
                    int wrote = snprintf(examples + used, sizeof(examples) - used, "%s%lu", i ? ", " : "",
                        static_cast<unsigned long>(shared[i]));
                    if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(examples) - used) break;
                    used += static_cast<size_t>(wrote);
                }
                LogLine("WARN", "[MOBCHECK] %s shares %lu object ID(s) with its base map %s.mob (e.g. %s); the quest map's "
                    "object replaces the base map's object with the same ID", fileName.c_str(),
                    static_cast<unsigned long>(shared.size()), baseName.c_str(), examples);
            }
        }
    } catch (...) {
        LogLine("WARN", "[MOBCHECK] script check of %s failed unexpectedly and was skipped", path);
    }
}

// Return whether this exact path has already been through ValidateMobFile
// during this process's lifetime, recording it if not.
static bool HasMobFileAlreadyBeenValidated(const char* path) {
    static std::unordered_set<std::string> validatedPaths;
    std::string key(path);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
    }
    if (validatedPaths.count(key) != 0) {
        return true;
    }
    validatedPaths.insert(key);
    return false;
}

// Read and sanity-check a .mob file without disturbing the game's own file
// handle/position; a second, independent handle is opened for this purpose.
// The node format (type:4, length:4, length includes the 8-byte header) and
// the type IDs used below come directly from ei_maper's own reader
// (util::CMobParser / CMob::deserialize), not from guesswork. Only clearly
// invalid data is flagged as an error: a root length that doesn't match the
// actual file size is common (the file can be extended, or resaved by the
// navmesh generator, after the root length was written) and is only DEBUG.
// Lower-case, backslash-separated, absolute form of a path, for comparing folders.
static std::string NormalizedFullPath(const char* path) {
    char full[MAX_PATH] = {};
    DWORD length = GetFullPathNameA(path, sizeof(full), full, NULL);
    std::string result = (length > 0 && length < sizeof(full)) ? std::string(full) : std::string(path);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = result[i] == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

// Is this file inside the folder of the mod that owns this um.dll ("...\Mods\Universal-Mod\")?
// The game opens other mods' maps and the base game's too; only this mod's are validated.
static bool IsInsideOwnModFolder(const char* path) {
    static std::string modFolder;
    if (modFolder.empty()) {
        char dllPath[MAX_PATH] = {};
        if (g_dllModule && GetModuleFileNameA(g_dllModule, dllPath, sizeof(dllPath)) != 0) {
            char* slash = strrchr(dllPath, '\\');
            if (slash) *slash = '\0';
            modFolder = NormalizedFullPath(dllPath) + "\\";
        }
    }
    if (modFolder.empty()) {
        return true; // cannot tell where this DLL lives: better to validate than to silently skip
    }
    return NormalizedFullPath(path).compare(0, modFolder.size(), modFolder) == 0;
}

static void ValidateMobFile(const char* path) {
    if (!g_enableMobValidation || !path || !HasFileExtension(path, "mob")) {
        return;
    }
    if (!IsInsideOwnModFolder(path)) {
        return; // another mod's map (or the base game's) that the game also opens: not this mod's business
    }
    if (HasMobFileAlreadyBeenValidated(path)) {
        // The game routinely opens the same .mob file twice in a row (a size
        // probe followed by the real read); validating it again would only
        // duplicate identical log entries since the file cannot have changed.
        return;
    }

    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return; // cannot inspect the file; not itself a validation failure
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(handle, &fileSize)) {
        CloseHandle(handle);
        return;
    }

    ULONGLONG actualSize = static_cast<ULONGLONG>(fileSize.QuadPart);
    DWORD bytesToRead = static_cast<DWORD>(actualSize < kMobMaxWalkBytes ? actualSize : kMobMaxWalkBytes);
    BYTE* buffer = static_cast<BYTE*>(malloc(bytesToRead > 0 ? bytesToRead : 1));
    DWORD bytesRead = 0;
    BOOL readOk = buffer && ReadFile(handle, buffer, bytesToRead, &bytesRead, NULL);
    CloseHandle(handle);
    if (!buffer) {
        return;
    }

    if (!readOk || bytesRead < 16) {
        LogLine("ERROR", "[MOBCHECK] %s is only %llu bytes, too small to contain a valid .mob header",
            path, actualSize);
        free(buffer);
        return;
    }

    DWORD rootType = 0;
    memcpy(&rootType, buffer + 0, sizeof(rootType));
    if (rootType != kMobTypeObjectDbFile) {
        LogLine("ERROR", "[MOBCHECK] %s has an unexpected root node type %lu (expected %lu); file may be corrupted or is not a .mob file",
            path, rootType, kMobTypeObjectDbFile);
        free(buffer);
        return;
    }
    // The root node's declared length routinely differs from the actual file
    // size (maps get resaved/extended after it was last written), so it is
    // not a useful corruption signal and is intentionally not logged here.

    // Force the (once-per-process) database load to happen right here rather
    // than lazily on the first field that needs it: otherwise its "loaded N
    // names" log line lands wherever that first lookup happens to occur,
    // which could be partway through this file, after this one, or never at
    // all if this particular file has no items/spells/armors to check.
    GetMobDatabaseNames(path);

    if (actualSize > kMobMaxWalkBytes) {
        free(buffer);
        return;
    }

    // Skip the root node and the SC_/PR_OBJECT_DB_FILE marker (fixed 8 bytes
    // each; ei_maper's own reader does not use their declared lengths either)
    // then walk sibling top-level nodes purely by declared length until the
    // terminating ROOT node or a length that would run past the file.
    size_t pos = 16;
    bool objectSectionSeen = false;
    bool hasError = false;
    while (pos + 8 <= bytesRead) {
        DWORD type = 0, length = 0;
        memcpy(&type, buffer + pos, sizeof(type));
        memcpy(&length, buffer + pos + 4, sizeof(length));
        if (type == kMobTypeRoot) {
            break;
        }
        if (length < 8 || pos + length > bytesRead) {
            LogLine("ERROR", "[MOBCHECK] %s has a node declaring a length of %lu bytes that extends past the end of the file; the file is likely truncated or corrupted",
                path, length);
            free(buffer);
            return;
        }
        if (type == kMobTypeObjectSection) {
            objectSectionSeen = true;
            ValidateMobObjectSection(path, buffer, pos, pos + length, &hasError);
        }
        pos += length;
    }
    ValidateMobScript(path, buffer, bytesRead, &hasError);
    if (!objectSectionSeen) {
        LogLine("DEBUG", "[MOBCHECK] %s has no OBJECT_SECTION node (placement-only or menu mob?)", path);
    } else if (!hasError) {
        LogLine("DEBUG", "[MOBCHECK] %s validated OK", path);
    }
    free(buffer);
}

// ---------------------------------------------------------------------------
// HEAP_FREE_QUARANTINE
//
// Crash dumps from Wine show game.exe destroying a UI screen (0x5f6620) whose
// child-widget list still holds a pointer to a widget that was already freed:
// the virtual call through it (0x5ec010) reads a vtable pointer out of freed
// memory. Windows' heap tends to leave a freed block's contents intact, so
// the stale pointer still lands on a valid vtable and the bug goes unnoticed;
// Wine's heap overwrites/reuses the block almost immediately and the process
// dies. Holding recently freed blocks back - never touching them, never
// handing them out again - gives Wine the same forgiving behavior, and any
// write through a stale pointer then corrupts nothing else either.
//
// game.exe's static CRT (VC7.1) selects the plain system heap on any NT 5+
// OS (__heap_select at 0x6f539e), so every malloc/free/new/delete reaches
// HeapAlloc/HeapFree on its private _crtheap through game.exe's own import
// table, which is exactly what PatchImportedFunction hooks. (On an OS that
// reports as Windows 9x it would use its own small-block heap instead, which
// bypasses HeapFree - ReadGameCrtHeapMode() below is logged so that's visible.)
// ---------------------------------------------------------------------------
static const int kQuarantineCallerSlots = 6;

struct QuarantinedBlock {
    HANDLE heap;
    LPVOID pointer;
    SIZE_T size;
    DWORD flags;
    DWORD freedTickMs;                      // GetTickCount() when the game freed it
    DWORD vtable;                           // its first dword if that pointed into game.exe (an object's vtable), else 0
    DWORD callers[kQuarantineCallerSlots];  // return-address candidates of the free, innermost first (0 = unused)
    char label[32];                         // a name the freed object mentions (model, mesh, texture...), "" if none found
};

// Bounds the bookkeeping (a std::deque + std::unordered_set entry per block)
// when the game frees very many tiny blocks: the oldest are really freed once
// either the block count or the byte limit is exceeded. The count limit scales
// with the MB setting, assuming 256-byte blocks on average (measured: ~480 B
// in this game's map loads), so it only binds when blocks are unusually small.
static const size_t kQuarantineBlocksPerMb = 4096;
static const unsigned long kQuarantineDoubleFreeLogLimit = 10;
static const unsigned long kQuarantineReallocLogLimit = 5;

static CRITICAL_SECTION g_quarantineLock;
// Set only once the hooks are live, and deliberately never cleared by a config
// reload: blocks already held must keep being recognized as held.
static bool g_heapFreeQuarantineInstalled = false;
// False when both windows are 0 MB: the hooks then only track allocations and free everything at once.
static bool g_quarantineHolds = true;
// Two pools, each a FIFO with its own limits: 0 = data buffers and everything else (the
// HEAP_FREE_QUARANTINE_MB window), 1 = small OBJECTS - blocks that start with a game vtable -
// with a separate window, so a long-lived widget's dangling child stays held for far longer than
// the megabytes of pixel data freed around it.
struct QuarantinePool {
    std::deque<QuarantinedBlock> queue;
    unsigned long long frontSequence = 0; // sequence number of queue.front()
    unsigned long long nextSequence = 0;
    SIZE_T bytes = 0;
    SIZE_T byteLimit = 0;
    size_t maxBlocks = 0;
};
static QuarantinePool g_quarantinePools[2];
static const int kObjectPool = 1;
static const SIZE_T kObjectPoolMaxBlockBytes = 16384;
// HeapSize above this is not a size: the block's heap header was overwritten.
static const SIZE_T kMaxSaneBlockBytes = 0x20000000;
static unsigned long g_quarantineCorruptHeaders = 0;

// Every live allocation game.exe made through its own HeapAlloc since this hook went in (start ->
// requested size). A HeapFree for a pointer that is not a block's start but lies inside a live block
// is a free of the MIDDLE of an allocation (an array element): on Wine that corrupts the heap, and
// the crashes seen so far (stale children, zeroed list nodes, damaged headers) fit it.
static const int kAllocCallerSlots = 3;

// Open-addressing hash table from a 32-bit address to V (linear probing, backward-shift deletion so
// there are no tombstones), in memory of its own from VirtualAlloc. The hooks run for every game
// allocation; a std::map costs a tree walk with cache misses and a malloc per operation, which measured
// at ~3 us per free+alloc with 300,000 live blocks. Callers hold g_quarantineLock.
template <typename V>
class PtrTable {
public:
    struct Slot { DWORD key; V value; };   // key 0 = empty
    size_t Size() const { return count_; }
    size_t Capacity() const { return capacity_; }
    Slot& At(size_t index) { return slots_[index]; }
    V* Find(DWORD key) {
        if (!slots_) return nullptr;
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return &slots_[i].value;
            if (slots_[i].key == 0) return nullptr;
        }
    }
    bool Put(DWORD key, const V& value) {
        if (!slots_ || (count_ + 1) * 8 > capacity_ * 5) {
            if (!Grow() && (!slots_ || (count_ + 1) * 10 > capacity_ * 9)) return false;
        }
        size_t i = Home(key);
        while (slots_[i].key != 0 && slots_[i].key != key) i = (i + 1) & mask_;
        if (slots_[i].key == 0) { ++count_; slots_[i].key = key; }
        slots_[i].value = value;
        return true;
    }
    bool Erase(DWORD key) {
        if (!slots_) return false;
        size_t i = Home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        EraseAt(i);
        return true;
    }
    void EraseAt(size_t hole) {
        size_t i = hole, j = hole;
        for (;;) {
            slots_[i].key = 0;
            for (;;) {
                j = (j + 1) & mask_;
                if (slots_[j].key == 0) { --count_; return; }
                const size_t home = Home(slots_[j].key);
                // The entry at j may move back into the hole at i unless its home lies in (i, j].
                if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) break;
            }
            slots_[i] = slots_[j];
            i = j;
        }
    }
    void Clear() {
        if (slots_) memset(slots_, 0, capacity_ * sizeof(Slot));
        count_ = 0;
    }
private:
    size_t Home(DWORD key) const { return static_cast<size_t>(((key >> 3) * 2654435761u) >> shift_); }
    bool Grow() {
        const size_t newCapacity = capacity_ ? capacity_ * 2 : (static_cast<size_t>(1) << 16);
        Slot* fresh = static_cast<Slot*>(VirtualAlloc(NULL, newCapacity * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fresh) return false;
        Slot* old = slots_;
        const size_t oldCapacity = capacity_;
        slots_ = fresh;
        capacity_ = newCapacity;
        mask_ = newCapacity - 1;
        shift_ = 32;
        for (size_t c = newCapacity; c > 1; c >>= 1) --shift_;
        for (size_t k = 0; k < oldCapacity; ++k) {
            if (old[k].key == 0) continue;
            size_t i = Home(old[k].key);
            while (slots_[i].key != 0) i = (i + 1) & mask_;
            slots_[i] = old[k];
        }
        if (old) VirtualFree(old, 0, MEM_RELEASE);
        return true;
    }
    Slot* slots_ = nullptr;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    unsigned shift_ = 32;
    size_t count_ = 0;
};

struct LiveBlock {
    DWORD size;                          // the size the game asked for (the real block is g_canaryBytes larger)
    HANDLE heap;
    DWORD tickMs;                        // GetTickCount() at the allocation
    DWORD callers[kAllocCallerSlots];    // who allocated it (return-address candidates, innermost first)
};
// Every allocation is made g_canaryBytes larger and the extra bytes are filled with kCanaryFill; the
// pointer returned to the game is unchanged. A buffer overrun lands in those bytes first, so a change
// there means "this block was written past its end" - and the block's allocation site is known.
static const DWORD kMaxCanaryBytes = 512;
static DWORD g_canaryBytes = 64;         // HEAP_ALLOC_PADDING, for the statistics only: see PaddingFor
// The padding of a block of this size. The game's figure objects (656 bytes) overrun by 12, but the
// arrays of pointers of 1 KB and more overrun by 130+ bytes (measured with a 160-byte guard), so those
// get a proportionally larger one: a quarter of their size, at most 512 bytes.
static DWORD PaddingFor(SIZE_T size) {
    if (g_heapAllocPadding <= 0) return 0;
    DWORD padding = static_cast<DWORD>(g_heapAllocPadding);
    if (size >= 1024) {
        const DWORD scaled = size / 4 > 512 ? 512 : static_cast<DWORD>(size / 4);
        if (scaled > padding) padding = scaled;
    }
    return padding;
}
static const BYTE kCanaryFill = 0xA5;
static const unsigned long kOverrunLogLimit = 25;
static unsigned long g_quarantineOverruns = 0;
static unsigned long g_quarantineOverlaps = 0;
static std::unordered_set<DWORD> g_reportedOverruns;
static std::map<DWORD, unsigned long> g_overrunSites;   // allocation site (innermost caller) -> blocks it overran
static void FormatOverrunSites(char* out, size_t outSize);
static PtrTable<LiveBlock> g_liveBlocks;
static unsigned long g_quarantineInvalidFrees = 0;
static unsigned long g_quarantineWritesAfterFree = 0;
static const unsigned long kWriteAfterFreeLogLimit = 25;
static const unsigned long kInvalidFreeLogLimit = 20;
// The last few blocks freed with a trusted (tracked) size, to catch an array's elements being
// freed after the array's own block was.
struct RecentBlock {
    DWORD start;
    DWORD size;
};
static const int kRecentBlocks = 64;
static RecentBlock g_recentFreed[kRecentBlocks];
static unsigned g_recentFreedNext = 0;
// pointer -> (pool, sequence number). A pool's entries leave strictly in order, so an entry is
// pool.queue[sequence - pool.frontSequence].
struct QuarantineIndexEntry { int pool; unsigned long long sequence; };
static PtrTable<QuarantineIndexEntry> g_quarantineIndex;
static unsigned long g_quarantineFreesHeld = 0;
static unsigned long g_quarantineEvictions = 0;
static unsigned long g_quarantineDoubleFrees = 0;
static unsigned long g_quarantineReallocRedirects = 0;
static bool g_quarantineFullLogged = false;

// game.exe's CRT heap mode: 1 = system heap, 2 = V5 small-block heap, 3 = V6
// small-block heap (__active_heap, 0x7caf0c in the one known OBT-1 game.exe
// build, which is loaded at its preferred base). Returns -1 when this is not
// that build or the address isn't readable - the number means nothing then.
static long ReadGameCrtHeapMode() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleA(NULL));
    if (base != reinterpret_cast<const BYTE*>(0x400000)) {
        return -1;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage <= 0x7caf10 - 0x400000) {
        return -1;
    }
    const DWORD* mode = reinterpret_cast<const DWORD*>(0x7caf0c);
    if (IsBadReadPtr(mode, sizeof(DWORD))) {
        return -1;
    }
    return static_cast<long>(*mode);
}

// Does the value found on the stack look like a return address, i.e. does a
// call instruction end right before it? Recognizes the call encodings MSVC
// emits (E8 rel32, FF /2 with register/[reg]/[reg+disp8]/[reg+disp32]/[imm32]).
// On the direct form, the callee is written to *target.
static bool LooksLikeReturnAddress(const BYTE* address, DWORD* target, bool knownReadable = false) {
    *target = 0;
    if (!knownReadable && IsBadReadPtr(address - 6, 6)) {
        return false;
    }
    if (address[-5] == 0xE8) {
        *target = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(address) +
            *reinterpret_cast<const LONG*>(address - 4));
        return true;
    }
    if (address[-2] == 0xFF && ((address[-1] & 0xF8) == 0xD0 || (address[-1] & 0xF8) == 0x10)) {
        return true;
    }
    if (address[-3] == 0xFF && (address[-2] & 0xF8) == 0x50) {
        return true;
    }
    if (address[-6] == 0xFF && (address[-5] == 0x15 || (address[-5] & 0xF8) == 0x90)) {
        return true;
    }
    return false;
}

// Log the return-address-looking words in game.exe's code found in the next
// 2 KB of this thread's stack, like a hand-rolled backtrace that copes with
// the FPO frames game.exe is built with (which RtlCaptureStackBackTrace's
// frame-pointer walk cannot follow). Innermost first; needs no symbols.
static void LogCallerCandidates(const char* what) {
    const BYTE* exe = reinterpret_cast<const BYTE*>(GetModuleHandleA(NULL));
    if (!exe) {
        return;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    ULONG_PTR low = reinterpret_cast<ULONG_PTR>(exe);
    ULONG_PTR high = low + nt->OptionalHeader.SizeOfImage;

    const DWORD* sp = reinterpret_cast<const DWORD*>(__builtin_frame_address(0));
    const DWORD* stackBase = reinterpret_cast<const DWORD*>(__readfsdword(4));
    const DWORD* end = sp + 1024 < stackBase ? sp + 1024 : stackBase;

    // LogLine truncates a message at 400 characters, so the chain is written
    // as several lines of a few candidates each rather than one long one.
    const int perLine = 5;
    char text[320] = {};
    size_t used = 0;
    int inLine = 0;
    int found = 0;
    int lineNumber = 0;
    for (const DWORD* word = sp; word < end && found < 12; ++word) {
        ULONG_PTR value = *word;
        DWORD target = 0;
        if (value <= low + 6 || value >= high ||
            !LooksLikeReturnAddress(reinterpret_cast<const BYTE*>(value), &target)) {
            continue;
        }
        int wrote = target
            ? snprintf(text + used, sizeof(text) - used, " 0x%08lX(call 0x%08lX)",
                static_cast<unsigned long>(value), static_cast<unsigned long>(target))
            : snprintf(text + used, sizeof(text) - used, " 0x%08lX(indirect call)",
                static_cast<unsigned long>(value));
        if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(text) - used) {
            break;
        }
        used += static_cast<size_t>(wrote);
        ++found;
        if (++inLine == perLine) {
            LogLine("WARN", "[QUARANTINE] %s - caller candidates %d-%d, innermost first:%s",
                what, lineNumber * perLine + 1, found, text);
            ++lineNumber;
            used = 0;
            inLine = 0;
            text[0] = '\0';
        }
    }
    if (inLine > 0) {
        LogLine("WARN", "[QUARANTINE] %s - caller candidates %d-%d, innermost first:%s",
            what, lineNumber * perLine + 1, found, text);
    } else if (found == 0) {
        LogLine("WARN", "[QUARANTINE] %s - no caller candidates found in game.exe", what);
    }
}

// game.exe's own address ranges, for recognising vtable pointers and return addresses without
// probing memory. Set up when the hooks are installed.
static DWORD g_imageLow = 0, g_imageHigh = 0, g_codeLow = 0, g_codeHigh = 0;
// The one known OBT-1 build: its static CRT (its own free/delete frames) lives at 0x6E0000 and
// above, which is noise in a "who freed this" list.
static bool g_knownGameBuild = false;
static const DWORD kKnownBuildCrtStart = 0x6E0000;
static const DWORD kPoisonMarker = 0xDDDDDDDD;
static const SIZE_T kPoisonBytes = 1024;

static void InitGameImageRanges() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleA(NULL));
    if (!base) return;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_imageLow = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(base));
    g_imageHigh = g_imageLow + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_CNT_CODE) {
            DWORD low = g_imageLow + section->VirtualAddress;
            DWORD high = low + section->Misc.VirtualSize;
            if (g_codeLow == 0 || low < g_codeLow) g_codeLow = low;
            if (high > g_codeHigh) g_codeHigh = high;
        }
    }
    g_knownGameBuild = ReadGameCrtHeapMode() == 1;
}

// Where the game freed a block: return-address-looking words in game.exe's code found in the
// next 768 bytes of this thread's stack, innermost first (the static CRT's own frames left out
// on the known build). No symbols and no memory probing - a scan this cheap runs on every free.
static void CaptureCallers(DWORD* out, int slots) {
    const DWORD* sp = reinterpret_cast<const DWORD*>(__builtin_frame_address(0));
    const DWORD* stackBase = reinterpret_cast<const DWORD*>(__readfsdword(4));
    const DWORD* end = sp + 192 < stackBase ? sp + 192 : stackBase;
    int found = 0;
    for (const DWORD* word = sp; word < end && found < slots; ++word) {
        DWORD value = *word;
        if (value < g_codeLow + 6 || value >= g_codeHigh) continue;
        if (g_knownGameBuild && value >= kKnownBuildCrtStart) continue;
        DWORD target = 0;
        if (!LooksLikeReturnAddress(reinterpret_cast<const BYTE*>(value), &target, true)) continue;
        out[found++] = value;
    }
}

static void CaptureFreeCallers(DWORD* out) {
    CaptureCallers(out, 3); // the slots after those stay 0: a free only needs to say who freed it
}

static void FormatCallerList(const DWORD* callers, int count, char* out, size_t outSize) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count && callers[i]; ++i) {
        int wrote = snprintf(out + used, outSize - used, "%s0x%08lX", i ? " <- " : "", static_cast<unsigned long>(callers[i]));
        if (wrote < 0 || static_cast<size_t>(wrote) >= outSize - used) break;
        used += static_cast<size_t>(wrote);
    }
    if (out[0] == '\0') snprintf(out, outSize, "(not recorded)");
}

static void FormatCallers(const DWORD* callers, char* out, size_t outSize) {
    FormatCallerList(callers, kQuarantineCallerSlots, out, outSize);
}

// Is this a plausible asset name: 4-31 printable characters ending at a NUL?
static bool LooksLikeName(const BYTE* text, size_t available, char* out, size_t outSize) {
    size_t length = 0;
    while (length < available && length < outSize - 1 && text[length] >= 0x20 && text[length] < 0x7F) ++length;
    if (length < 4 || length >= available || text[length] != 0) return false;
    memcpy(out, text, length);
    out[length] = '\0';
    return true;
}

// The first name a freed object mentions: an inline string in its first 256 bytes, or a string one
// pointer away in game.exe's read-only data. Only memory the DLL already knows is readable is
// touched - no probing. Only done in poison mode.
static void HarvestLabelLocked(const BYTE* block, SIZE_T size, char* out, size_t outSize) {
    out[0] = '\0';
    SIZE_T scan = size < 256 ? size : 256;
    for (SIZE_T i = 0; i + 5 < scan; ++i) {
        if (block[i] >= 0x41 && block[i] < 0x7F && LooksLikeName(block + i, scan - i, out, outSize)) return; // starts with a letter
    }
    for (SIZE_T i = 0; i + 4 <= scan; i += 4) {
        DWORD value = 0;
        memcpy(&value, block + i, 4);
        if (value < 0x10000) continue;
        if (value >= g_imageLow && value < g_imageHigh) {
            if (LooksLikeName(reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(value)), g_imageHigh - value, out, outSize)) return;
            continue;
        }
    }
}

// The last freed objects that carried a name, for the crash report ("what was being torn down").
struct RecentLabel {
    DWORD tickMs;
    DWORD vtable;
    char label[32];
};
static const int kRecentLabels = 24;
static RecentLabel g_recentLabels[kRecentLabels];
static unsigned g_recentLabelNext = 0;

static const QuarantinedBlock* FindHeldBlockLocked(LPVOID pointer) {
    const QuarantineIndexEntry* entry = g_quarantineIndex.Find(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)));
    if (!entry) return nullptr;
    const QuarantinePool& pool = g_quarantinePools[entry->pool];
    unsigned long long position = entry->sequence - pool.frontSequence;
    return position < pool.queue.size() ? &pool.queue[static_cast<size_t>(position)] : nullptr;
}

static unsigned long OldestHeldAgeSecondsLocked() {
    DWORD oldest = 0;
    DWORD now = GetTickCount();
    for (const QuarantinePool& pool : g_quarantinePools) {
        if (!pool.queue.empty()) {
            DWORD age = now - pool.queue.front().freedTickMs;
            if (age > oldest) oldest = age;
        }
    }
    return oldest / 1000;
}

static size_t HeldBlockCountLocked() {
    return g_quarantinePools[0].queue.size() + g_quarantinePools[1].queue.size();
}

static void LogQuarantineStats(const char* reason) {
    EnterCriticalSection(&g_quarantineLock);
    size_t blocks = HeldBlockCountLocked();
    SIZE_T dataBytes = g_quarantinePools[0].bytes, objectBytes = g_quarantinePools[kObjectPool].bytes;
    size_t objectBlocks = g_quarantinePools[kObjectPool].queue.size();
    unsigned long held = g_quarantineFreesHeld;
    unsigned long evicted = g_quarantineEvictions;
    unsigned long doubleFrees = g_quarantineDoubleFrees;
    unsigned long redirects = g_quarantineReallocRedirects;
    unsigned long corrupt = g_quarantineCorruptHeaders;
    unsigned long invalid = g_quarantineInvalidFrees;
    unsigned long writes = g_quarantineWritesAfterFree;
    unsigned long overruns = g_quarantineOverruns;
    unsigned long overlaps = g_quarantineOverlaps;
    unsigned long oldest = OldestHeldAgeSecondsLocked();
    unsigned long liveBlocks = static_cast<unsigned long>(g_liveBlocks.Size());
    char overrunSites[160];
    FormatOverrunSites(overrunSites, sizeof(overrunSites));
    LeaveCriticalSection(&g_quarantineLock);
    LogLine("DEBUG", "[QUARANTINE] %s: holding %lu freed blocks (data %.1f of %.0f MB, %lu objects %.1f of %.0f MB), the oldest freed %lu s ago; "
        "frees held so far=%lu, really freed after eviction=%lu, double frees ignored=%lu, reallocs of freed blocks redirected=%lu, "
        "damaged heap headers seen=%lu, invalid frees ignored=%lu, writes to freed blocks=%lu, buffer overruns=%lu (sites: %s), overlapping allocations=%lu, live blocks tracked=%lu (padding %.1f MB)", reason, static_cast<unsigned long>(blocks), dataBytes / 1048576.0,
        g_quarantinePools[0].byteLimit / 1048576.0, static_cast<unsigned long>(objectBlocks), objectBytes / 1048576.0,
        g_quarantinePools[kObjectPool].byteLimit / 1048576.0, oldest, held, evicted, doubleFrees, redirects, corrupt, invalid, writes, overruns, overrunSites, overlaps, liveBlocks,
        liveBlocks * static_cast<double>(g_canaryBytes) / 1048576.0);
}

// Point-in-time numbers for the overlay; false when the quarantine isn't active.
static bool GetQuarantineSnapshot(double* heldMb, double* limitMb, double* objectMb, double* objectLimitMb,
        unsigned long* blocks, unsigned long* doubleFrees, unsigned long* oldestSeconds, unsigned long* invalidFrees,
        unsigned long* problems, unsigned long* overruns) {
    if (!g_heapFreeQuarantineInstalled) {
        return false;
    }
    EnterCriticalSection(&g_quarantineLock);
    *heldMb = g_quarantinePools[0].bytes / 1048576.0;
    *objectMb = g_quarantinePools[kObjectPool].bytes / 1048576.0;
    *blocks = static_cast<unsigned long>(HeldBlockCountLocked());
    *doubleFrees = g_quarantineDoubleFrees;
    *invalidFrees = g_quarantineInvalidFrees;
    // Overruns are absorbed by the padding, so they are counted apart from the real problems.
    *overruns = g_quarantineOverruns;
    *problems = g_quarantineOverlaps + g_quarantineWritesAfterFree + g_quarantineCorruptHeaders;
    *oldestSeconds = OldestHeldAgeSecondsLocked();
    LeaveCriticalSection(&g_quarantineLock);
    *limitMb = g_quarantinePools[0].byteLimit / 1048576.0;
    *objectLimitMb = g_quarantinePools[kObjectPool].byteLimit / 1048576.0;
    return true;
}

// Poison mode stamps a held block's first kPoisonBytes with 0xDDDDDDDD. If any of it has changed,
// something wrote into the block AFTER the game freed it: a stale pointer used for writing, or an
// allocation handed out on top of live memory. Returns the number of changed dwords.
static unsigned PoisonChangedDwords(const QuarantinedBlock& block, SIZE_T* firstChange) {
    SIZE_T bytes = (block.size < kPoisonBytes ? block.size : kPoisonBytes) & ~static_cast<SIZE_T>(3);
    const DWORD* words = static_cast<const DWORD*>(block.pointer);
    unsigned changed = 0;
    *firstChange = 0;
    for (SIZE_T i = 0; i < bytes / sizeof(DWORD); ++i) {
        if (words[i] != kPoisonMarker) {
            if (changed == 0) *firstChange = i * sizeof(DWORD);
            ++changed;
        }
    }
    return changed;
}

// What was written: readable text if it looks like text (script text, names), else the first bytes in hex.
static void DescribeWrittenBytes(const BYTE* bytes, SIZE_T available, char* out, size_t outSize) {
    size_t length = available < 48 ? available : 48;
    size_t printable = 0;
    for (size_t i = 0; i < length; ++i) if (bytes[i] >= 0x20 && bytes[i] < 0x7F) ++printable;
    if (length >= 8 && printable * 10 >= length * 7) {
        char text[64];
        for (size_t i = 0; i < length; ++i) text[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<char>(bytes[i]) : '.';
        text[length] = '\0';
        snprintf(out, outSize, "text \"%s\"", text);
    } else {
        size_t shown = length < 16 ? length : 16;
        size_t used = snprintf(out, outSize, "bytes");
        for (size_t i = 0; i < shown && used + 4 < outSize; ++i) used += snprintf(out + used, outSize - used, " %02X", bytes[i]);
    }
}

static void ReportWriteAfterFree(const QuarantinedBlock& block, unsigned changed, SIZE_T firstChange, const char* when) {
    unsigned long number = ++g_quarantineWritesAfterFree;
    if (number > kWriteAfterFreeLogLimit) return;
    char callers[160], written[140];
    FormatCallers(block.callers, callers, sizeof(callers));
    SIZE_T avail = (block.size < kPoisonBytes ? block.size : kPoisonBytes);
    DescribeWrittenBytes(static_cast<const BYTE*>(block.pointer) + firstChange, avail - firstChange, written, sizeof(written));
    LogLine("WARN", "[QUARANTINE] WRITE AFTER FREE #%lu (%s): block %p (%lu bytes, vtable when freed 0x%08lX, freed %.1f s ago by %s) "
        "was written to while free - %u dwords changed, the first at +0x%lX: %s%s%s", number, when, block.pointer,
        static_cast<unsigned long>(block.size), static_cast<unsigned long>(block.vtable),
        (GetTickCount() - block.freedTickMs) / 1000.0, callers, changed, static_cast<unsigned long>(firstChange), written,
        block.label[0] ? "; the freed object mentioned: " : "", block.label);
}

// True when the guard bytes after a live block were changed (the 16 bytes found are copied out).
// Unreadable memory counts as intact: a block freed behind the hooks' back must not crash the scan.
static bool CanaryDamaged(DWORD start, DWORD size, BYTE* found, bool probe = true) {
    const DWORD padding = PaddingFor(size);
    if (padding == 0) return false;
    const BYTE* tail = reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(start)) + size;
    if (probe && IsBadReadPtr(tail, padding)) return false;
    bool damaged = false;
    for (DWORD i = 0; i < padding; ++i) {
        found[i] = tail[i];
        if (tail[i] != kCanaryFill) damaged = true;
    }
    return damaged;
}

// The most-overrunning allocation sites, "0x005AA92E x40, ...", for the stats and the crash report.
static void FormatOverrunSites(char* out, size_t outSize) {
    std::vector<std::pair<unsigned long, DWORD>> sites;
    for (const auto& entry : g_overrunSites) sites.push_back({entry.second, entry.first});
    std::sort(sites.begin(), sites.end(), [](const std::pair<unsigned long, DWORD>& x, const std::pair<unsigned long, DWORD>& y) {
        return x.first > y.first;
    });
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sites.size() && i < 6; ++i) {
        int wrote = snprintf(out + used, outSize - used, "%s0x%08lX x%lu", i ? ", " : "", static_cast<unsigned long>(sites[i].second), sites[i].first);
        if (wrote < 0 || static_cast<size_t>(wrote) >= outSize - used) break;
        used += static_cast<size_t>(wrote);
    }
    if (out[0] == '\0') snprintf(out, outSize, "none");
}

// Once per block; logged for the first few blocks of each allocation site (a site that overruns
// does it for every object it makes). Lock held.
static void ReportOverrun(DWORD start, const LiveBlock& block, const BYTE* found, const char* when) {
    if (!g_reportedOverruns.insert(start).second) return;
    ++g_quarantineOverruns;
    static bool explained = false;
    if (!explained) {
        explained = true;
        LogLine("INFO", "[HEAPFIX] game.exe writes a few bytes past the end of some of its own allocations (a bug of the game, "
            "on any setup). The padding absorbs it and nothing is damaged; each allocation site is described at DEBUG level.");
    }
    const DWORD padding = PaddingFor(block.size);
    int first = -1, last = -1;
    for (DWORD i = 0; i < padding; ++i) {
        if (found[i] != kCanaryFill) { if (first < 0) first = static_cast<int>(i); last = static_cast<int>(i); }
    }
    if (first < 0) return;
    const unsigned long siteCount = ++g_overrunSites[block.callers[0]];
    const bool usedUp = static_cast<DWORD>(last + 1) >= padding;   // the write may have gone on beyond the padding
    const bool powerOfTen = siteCount == 10 || siteCount == 100 || siteCount == 1000 || siteCount == 10000;
    if (siteCount > 3 && !powerOfTen && !usedUp) return;
    char callers[100], written[140];
    FormatCallerList(block.callers, kAllocCallerSlots, callers, sizeof(callers));
    DWORD shown = padding - static_cast<DWORD>(first) < 24 ? padding - static_cast<DWORD>(first) : 24;
    DescribeWrittenBytes(found + first, shown, written, sizeof(written));
    LogLine(usedUp ? "WARN" : "DEBUG", "[HEAPFIX] BUFFER OVERRUN (%s): block 0x%08lX (%lu bytes, allocated %.1f s ago by %s) was written past its end, "
        "up to %d of its %lu padding bytes (first at +%d: %s); this allocation site has now overrun %lu block(s)%s%s",
        when, static_cast<unsigned long>(start), static_cast<unsigned long>(block.size), (GetTickCount() - block.tickMs) / 1000.0,
        callers, last + 1, static_cast<unsigned long>(padding), first, written, siteCount,
        usedUp ? "; THE WHOLE PADDING WAS USED, the write may have gone further: raise HEAP_ALLOC_PADDING" : "",
        siteCount > 3 && !usedUp ? " (not logging each one any more)" : "");
}

// The heap returned memory that overlaps a block the game still has: something is wrong with the
// allocator's bookkeeping (a stale free, or damaged headers). Lock held.
static void ReportOverlap(DWORD start, DWORD size, const DWORD* callers, DWORD otherStart, const LiveBlock& other) {
    unsigned long number = ++g_quarantineOverlaps;
    if (number > kOverrunLogLimit) return;
    char byCallers[100], otherCallers[100];
    FormatCallerList(callers, kAllocCallerSlots, byCallers, sizeof(byCallers));
    FormatCallerList(other.callers, kAllocCallerSlots, otherCallers, sizeof(otherCallers));
    LogLine("WARN", "[QUARANTINE] OVERLAPPING ALLOCATION #%lu: HeapAlloc returned 0x%08lX (%lu bytes, requested by %s) which overlaps the LIVE "
        "block 0x%08lX (%lu bytes, allocated %.1f s ago by %s) - the heap handed out memory that was still in use", number,
        static_cast<unsigned long>(start), static_cast<unsigned long>(size), byCallers, static_cast<unsigned long>(otherStart),
        static_cast<unsigned long>(other.size), (GetTickCount() - other.tickMs) / 1000.0, otherCallers);
}

// Checks every live block's guard bytes, a chunk at a time so the game is never held up for long.
// A block the heap no longer knows (freed behind the hooks' back) is dropped instead of reported.
static DWORD WINAPI OverrunWatchThread(LPVOID) {
    for (;;) {
        Sleep(500);
        size_t resumeAt = 0;
        for (;;) {
            EnterCriticalSection(&g_quarantineLock);
            const size_t capacity = g_liveBlocks.Capacity();
            const size_t end = resumeAt + 4096 < capacity ? resumeAt + 4096 : capacity;
            for (size_t i = resumeAt; i < end;) {
                auto& slot = g_liveBlocks.At(i);
                if (slot.key == 0) { ++i; continue; }
                BYTE found[kMaxCanaryBytes];
                if (CanaryDamaged(slot.key, slot.value.size, found)) {
                    const void* pointer = reinterpret_cast<const void*>(static_cast<ULONG_PTR>(slot.key));
                    if (HeapSize(slot.value.heap, 0, pointer) == static_cast<SIZE_T>(-1)) {
                        g_liveBlocks.EraseAt(i); // the entry shifted into this slot has to be looked at too
                        continue;
                    }
                    ReportOverrun(slot.key, slot.value, found, "found by the background scan");
                }
                ++i;
            }
            resumeAt = end;
            const bool done = resumeAt >= capacity;
            LeaveCriticalSection(&g_quarantineLock);
            if (done) break;
            Sleep(1);
        }
    }
    return 0;
}

// Really free the oldest blocks of a pool until it is back within its limits. Lock held.
static bool TrimPoolLocked(QuarantinePool& pool) {
    bool evictedAny = false;
    while (!pool.queue.empty() && (pool.bytes > pool.byteLimit || pool.queue.size() > pool.maxBlocks)) {
        QuarantinedBlock oldest = pool.queue.front();
        pool.queue.pop_front();
        ++pool.frontSequence;
        if (g_heapFreeQuarantinePoison && oldest.size >= sizeof(DWORD)) {
            SIZE_T firstChange = 0;
            unsigned changed = PoisonChangedDwords(oldest, &firstChange);
            if (changed) ReportWriteAfterFree(oldest, changed, firstChange, "found when the block left the quarantine");
        }
        g_quarantineIndex.Erase(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(oldest.pointer)));
        pool.bytes -= oldest.size;
        ++g_quarantineEvictions;
        g_originalHeapFree(oldest.heap, oldest.flags, oldest.pointer);
        evictedAny = true;
    }
    return evictedAny;
}

// Called for every HeapFree game.exe makes directly. Returns true when the
// free was fully handled here (block held back, or a double free swallowed),
// false when the caller should just forward it to the real HeapFree.
static bool QuarantineHeapFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    if (!g_quarantineHolds) {
        // Tracking only: look at the block's guard bytes, forget it, and let the real HeapFree free it.
        EnterCriticalSection(&g_quarantineLock);
        const DWORD address = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer));
        if (LiveBlock* live = g_liveBlocks.Find(address)) {
            BYTE found[kMaxCanaryBytes];
            if (CanaryDamaged(address, live->size, found, false)) {
                ReportOverrun(address, *live, found, "found when the game freed the block");
            }
            g_liveBlocks.Erase(address);
        }
        LeaveCriticalSection(&g_quarantineLock);
        return false;
    }
    bool doubleFree = false;
    unsigned long doubleFreeNumber = 0;
    bool justFilled = false;
    bool damagedHeader = false;
    unsigned long damagedNumber = 0;
    SIZE_T damagedSize = 0;
    DWORD around[8] = {}; // the 16 bytes just before the block and its first 16 bytes
    QuarantinedBlock first = {};
    bool invalidFree = false;
    unsigned long invalidNumber = 0;
    DWORD ownerStart = 0, ownerSize = 0;
    DWORD trackedSize = 0;
    bool trackedStart = false;

    // Gathered before taking the lock: it only reads this thread's stack.
    DWORD callers[kQuarantineCallerSlots] = {};
    CaptureFreeCallers(callers);

    EnterCriticalSection(&g_quarantineLock);
    const QuarantinedBlock* earlier = FindHeldBlockLocked(pointer);
    if (earlier) {
        doubleFree = true;
        doubleFreeNumber = ++g_quarantineDoubleFrees;
        first = *earlier;
    } else {
        // Is this a block the game allocated since the hooks went in? (Freeing the middle of a block is
        // no longer looked for: Wine rejects such a free itself, and the search needed an ordered table.)
        const DWORD address = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer));
        LiveBlock* live = g_liveBlocks.Find(address);
        if (live) {
            trackedStart = true;
            trackedSize = live->size;
            BYTE found[kMaxCanaryBytes];
            if (CanaryDamaged(address, live->size, found, false)) { // a live block: readable, no probing
                ReportOverrun(address, *live, found, "found when the game freed the block");
            }
            g_liveBlocks.Erase(address);
        }
    }
    if (!invalidFree && !doubleFree) {
        // HeapSize doubles as a validity check: -1 means this is not a live
        // block of this heap, so let the real HeapFree deal with (and report)
        // whatever it is.
        SIZE_T size = trackedStart ? static_cast<SIZE_T>(trackedSize) + PaddingFor(trackedSize) : HeapSize(heap, 0, pointer);
        if (size == static_cast<SIZE_T>(-1)) {
            LeaveCriticalSection(&g_quarantineLock);
            return false;
        }
        if (size > kMaxSaneBlockBytes) {
            // The heap header in front of this block was overwritten (it reads as a negative
            // or absurd size). Freeing it for real could corrupt the heap further, so it is
            // kept forever instead - a small leak - and reported: this is where a buffer
            // overrun from the block before it becomes visible.
            damagedHeader = true;
            damagedNumber = ++g_quarantineCorruptHeaders;
            damagedSize = size;
            const BYTE* before = static_cast<const BYTE*>(pointer) - 16;
            if (!IsBadReadPtr(before, 16)) memcpy(around, before, 16);
            if (!IsBadReadPtr(pointer, 16)) memcpy(around + 4, pointer, 16);
        } else {
            QuarantinedBlock block = {};
            block.heap = heap;
            block.pointer = pointer;
            block.size = size;
            block.flags = flags;
            block.freedTickMs = GetTickCount();
            memcpy(block.callers, callers, sizeof(callers));
            if (size >= sizeof(DWORD)) {
                // The block is still a live allocation here, so its first dword is readable.
                DWORD firstDword = 0;
                memcpy(&firstDword, pointer, sizeof(firstDword));
                if (firstDword >= g_imageLow && firstDword < g_imageHigh && (firstDword & 3) == 0) {
                    block.vtable = firstDword; // an object of a game class: the vtable says which one
                }
            }
            if (g_heapFreeQuarantinePoison && block.vtable && size <= kObjectPoolMaxBlockBytes) {
                HarvestLabelLocked(static_cast<const BYTE*>(pointer), size, block.label, sizeof(block.label));
                if (block.label[0]) {
                    RecentLabel& recent = g_recentLabels[g_recentLabelNext++ % kRecentLabels];
                    recent.tickMs = block.freedTickMs;
                    recent.vtable = block.vtable;
                    memcpy(recent.label, block.label, sizeof(recent.label));
                }
            }
            if (g_heapFreeQuarantinePoison) {
                // Any stale read - a vtable, a list node's next pointer, a child pointer - now
                // returns garbage at once, while this free is still fresh in the window.
                SIZE_T poisonBytes = (size < kPoisonBytes ? size : kPoisonBytes) & ~static_cast<SIZE_T>(3);
                DWORD* words = static_cast<DWORD*>(pointer);
                for (SIZE_T i = 0; i < poisonBytes / sizeof(DWORD); ++i) words[i] = kPoisonMarker;
            }
            int poolIndex = (block.vtable && size <= kObjectPoolMaxBlockBytes &&
                g_quarantinePools[kObjectPool].byteLimit > 0) ? kObjectPool : 0;
            QuarantinePool& pool = g_quarantinePools[poolIndex];
            if (trackedStart) {
                g_recentFreed[g_recentFreedNext++ % kRecentBlocks] =
                    RecentBlock{ static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)), trackedSize };
            }
            pool.queue.push_back(block);
            g_quarantineIndex.Put(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)), QuarantineIndexEntry{poolIndex, pool.nextSequence++});
            pool.bytes += size;
            ++g_quarantineFreesHeld;
            if (TrimPoolLocked(g_quarantinePools[0]) | TrimPoolLocked(g_quarantinePools[kObjectPool])) {
                if (!g_quarantineFullLogged) {
                    g_quarantineFullLogged = true;
                    justFilled = true;
                }
            }
        }
    }
    LeaveCriticalSection(&g_quarantineLock);

    if (invalidFree) {
        if (invalidNumber <= kInvalidFreeLogLimit) {
            char freedBy[160];
            FormatCallers(callers, freedBy, sizeof(freedBy));
            LogLine("WARN", "[QUARANTINE] INVALID FREE #%lu ignored: %p is INSIDE the block at 0x%08lX (%lu bytes, offset +0x%lX), not the start of "
                "an allocation - the game freed the middle of a block (an array element?). Freed by: %s",
                invalidNumber, pointer, static_cast<unsigned long>(ownerStart), static_cast<unsigned long>(ownerSize),
                static_cast<unsigned long>(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)) - ownerStart), freedBy);
            if (invalidNumber <= 3) {
                LogCallerCandidates("invalid free");
            }
        }
        return true;
    }
    if (damagedHeader && damagedNumber <= kQuarantineDoubleFreeLogLimit) {
        char freedBy[160];
        FormatCallers(callers, freedBy, sizeof(freedBy));
        LogLine("WARN", "[QUARANTINE] DAMAGED HEAP HEADER #%lu: block %p reports a size of %lu bytes (a wrapped negative), so the 16 bytes "
            "in front of it were overwritten - most likely by a buffer overrun in the block before it. Not freed (kept). "
            "Bytes before the block: %08lX %08lX %08lX %08lX; its first bytes: %08lX %08lX %08lX %08lX; freed by: %s",
            damagedNumber, pointer, static_cast<unsigned long>(damagedSize), around[0], around[1], around[2], around[3],
            around[4], around[5], around[6], around[7], freedBy);
        return true;
    }
    if (damagedHeader) {
        return true;
    }
    if (doubleFree && doubleFreeNumber <= kQuarantineDoubleFreeLogLimit) {
        char firstCallers[160];
        FormatCallers(first.callers, firstCallers, sizeof(firstCallers));
        LogLine("WARN", "[QUARANTINE] Ignored double free #%lu of block %p (heap=%p size=%lu, vtable when first freed=0x%08lX) - "
            "it was first freed %.1f s ago by: %s", doubleFreeNumber, pointer, heap, static_cast<unsigned long>(first.size),
            static_cast<unsigned long>(first.vtable), (GetTickCount() - first.freedTickMs) / 1000.0, firstCallers);
        LogCallerCandidates("second free");
    }
    if (justFilled) {
        LogQuarantineStats("Quarantine is full, from now on the oldest freed blocks are really freed");
    }
    return true;
}

static bool QuarantineHolds(LPVOID pointer) {
    EnterCriticalSection(&g_quarantineLock);
    bool held = FindHeldBlockLocked(pointer) != nullptr;
    LeaveCriticalSection(&g_quarantineLock);
    return held;
}

// Forget every held block of a heap that is about to be destroyed: really
// freeing into a destroyed heap later would corrupt memory.
static void QuarantineForgetHeap(HANDLE heap) {
    EnterCriticalSection(&g_quarantineLock);
    {
        std::vector<DWORD> doomed;
        for (size_t i = 0; i < g_liveBlocks.Capacity(); ++i) {
            if (g_liveBlocks.At(i).key != 0 && g_liveBlocks.At(i).value.heap == heap) doomed.push_back(g_liveBlocks.At(i).key);
        }
        for (DWORD key : doomed) g_liveBlocks.Erase(key);
    }
    g_quarantineIndex.Clear();
    for (int poolIndex = 0; poolIndex < 2; ++poolIndex) {
        QuarantinePool& pool = g_quarantinePools[poolIndex];
        std::deque<QuarantinedBlock> kept;
        for (const QuarantinedBlock& block : pool.queue) {
            if (block.heap == heap) pool.bytes -= block.size;
            else kept.push_back(block);
        }
        pool.queue.swap(kept);
        // Re-number what is left so the index stays consistent with the queue.
        pool.frontSequence = 0;
        pool.nextSequence = 0;
        for (const QuarantinedBlock& block : pool.queue) {
            g_quarantineIndex.Put(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(block.pointer)), QuarantineIndexEntry{poolIndex, pool.nextSequence++});
        }
    }
    LeaveCriticalSection(&g_quarantineLock);
}

// For a register that looks like a pointer to a heap object: what is at that address, and what
// does the game's heap say about it? "FREE" means the game's own heap no longer has an allocated
// block starting there - the pointer is stale - while "allocated" means something (possibly a
// different object that reused the address) lives there now. The first dwords show the vtable
// (which class it is) or zero/garbage if the memory was cleared or reused.
static void LogHeapPointerProbe(const char* label, DWORD value) {
    if (value < 0x10000 || value >= 0x7FFF0000 || (value & 7) != 0) return;
    if (value >= g_imageLow && value < g_imageHigh) return;
    const void* pointer = reinterpret_cast<const void*>(static_cast<ULONG_PTR>(value));
    if (IsBadReadPtr(pointer, 16)) return;
    DWORD dwords[4] = {};
    memcpy(dwords, pointer, sizeof(dwords));
    HANDLE heap = NULL;
    if (g_knownGameBuild) {
        const HANDLE* crtHeap = reinterpret_cast<const HANDLE*>(0x7caf08); // game.exe's _crtheap
        if (!IsBadReadPtr(crtHeap, sizeof(HANDLE))) heap = *crtHeap;
    }
    const char* state = "game heap not identified";
    char stateBuffer[80];
    if (heap) {
        SIZE_T size = HeapSize(heap, 0, pointer);
        if (size == static_cast<SIZE_T>(-1)) {
            state = "NOT an allocated block of the game heap: freed (stale pointer) or not a block start";
        } else {
            snprintf(stateBuffer, sizeof(stateBuffer), "an ALLOCATED block of %lu bytes in the game heap", static_cast<unsigned long>(size));
            state = stateBuffer;
        }
    }
    LogLine("FATAL", "[QUARANTINE] crash: %s = 0x%08lX is %s; first dwords %08lX %08lX %08lX %08lX", label,
        static_cast<unsigned long>(value), state, dwords[0], dwords[1], dwords[2], dwords[3]);
}

// Crash report: which freed block, if any, do the crashing thread's registers and stack point
// into? With HEAP_FREE_QUARANTINE_POISON this names the object whose stale use crashed the game
// and the code that freed it; without it, "none" says the object is older than the window (or is
// not a heap block at all). Non-blocking on the lock: the crash may have happened inside it.
// Only the words right at the stack pointer are looked at: a stack slot deeper down is very
// often a stale leftover, and with hundreds of MB of freed blocks held one would "hit" one by
// coincidence. Registers and the fault address are the strong evidence.
static const DWORD kCrashStackWords = 32;

static void LogQuarantineCrashAnalysis(const CONTEXT* context, const EXCEPTION_RECORD* record) {
#if defined(_M_IX86) || defined(__i386__)
    if (!g_heapFreeQuarantineInstalled || !context) {
        return;
    }
    struct Value { const char* label; DWORD value; DWORD stackOffset; };
    std::vector<Value> values;
    values.push_back({"EAX", context->Eax, 0});
    values.push_back({"EBX", context->Ebx, 0});
    values.push_back({"ECX", context->Ecx, 0});
    values.push_back({"EDX", context->Edx, 0});
    values.push_back({"ESI", context->Esi, 0});
    values.push_back({"EDI", context->Edi, 0});
    values.push_back({"EBP", context->Ebp, 0});
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        values.push_back({"fault address", static_cast<DWORD>(record->ExceptionInformation[1]), 0});
    }
    const DWORD stackLimit = __readfsdword(8);
    const DWORD stackBase = __readfsdword(4);
    if (context->Esp >= stackLimit && context->Esp < stackBase && (context->Esp & 3) == 0) {
        const DWORD* words = reinterpret_cast<const DWORD*>(context->Esp);
        for (DWORD i = 0; i < kCrashStackWords && context->Esp + (i + 1) * 4 <= stackBase; ++i) {
            values.push_back({"stack", words[i], i * 4});
        }
    }

    // Registers first: what each heap-looking one points at. Skipped when the crash is not in
    // game.exe itself (inside ntdll the heap may be damaged and a HeapSize call is unsafe).
    if (record && reinterpret_cast<DWORD>(record->ExceptionAddress) >= g_imageLow &&
            reinterpret_cast<DWORD>(record->ExceptionAddress) < g_imageHigh) {
        for (size_t i = 0; i < 7; ++i) {
            LogHeapPointerProbe(values[i].label, values[i].value);
        }
    }

    bool poisonSeen = false;
    for (const Value& v : values) {
        if (v.value >= kPoisonMarker && v.value < kPoisonMarker + 0x400) poisonSeen = true;
    }
    if (poisonSeen) {
        LogLine("FATAL", "[QUARANTINE] crash: a register or the fault address holds the poison marker 0xDDDDDDDD - "
            "something read memory that was already freed (a stale pointer: object, list node or child)");
    }
    if (!TryEnterCriticalSection(&g_quarantineLock)) {
        LogLine("FATAL", "[QUARANTINE] crash: could not inspect the held blocks (another thread holds the quarantine lock)");
        return;
    }
    int hits = 0;
    DWORD now = GetTickCount();
    std::unordered_set<LPVOID> reportedFromStack;
    for (const QuarantinePool& pool : g_quarantinePools) for (const QuarantinedBlock& block : pool.queue) {
        DWORD start = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(block.pointer));
        for (const Value& v : values) {
            if (v.value < start || v.value - start >= block.size) continue;
            // A stack word is weak evidence, and one landing in a huge freed buffer (a screen
            // surface, a texture) is a leftover, not a lead: only small blocks, once each.
            if (v.stackOffset || strcmp(v.label, "stack") == 0) {
                if (block.size > 16384 || !reportedFromStack.insert(block.pointer).second) continue;
            }
            char callers[160];
            FormatCallers(block.callers, callers, sizeof(callers));
            char where[96];
            if (v.stackOffset || strcmp(v.label, "stack") == 0) snprintf(where, sizeof(where), "stack word ESP+0x%lX (weaker evidence: may be a stale leftover)", static_cast<unsigned long>(v.stackOffset));
            else snprintf(where, sizeof(where), "%s", v.label);
            LogLine("FATAL", "[QUARANTINE] crash: %s = 0x%08lX is inside a FREED block that is being held: block %p, %lu bytes, "
                "offset +0x%lX, vtable when freed 0x%08lX, freed %.1f s ago by: %s%s%s", where, static_cast<unsigned long>(v.value),
                block.pointer, static_cast<unsigned long>(block.size), static_cast<unsigned long>(v.value - start),
                static_cast<unsigned long>(block.vtable), (now - block.freedTickMs) / 1000.0, callers,
                block.label[0] ? "; it mentions the name: " : "", block.label);
            if (++hits >= 8) break;
        }
        if (hits >= 8) break;
    }
    size_t held = HeldBlockCountLocked();
    unsigned long oldest = OldestHeldAgeSecondsLocked();
    {
        unsigned long overrunBlocks = 0;
        for (size_t i = 0; i < g_liveBlocks.Capacity(); ++i) {
            const auto& entry = g_liveBlocks.At(i);
            if (entry.key == 0) continue;
            BYTE found[kMaxCanaryBytes];
            if (CanaryDamaged(entry.key, entry.value.size, found)) {
                ++overrunBlocks;
                ReportOverrun(entry.key, entry.value, found, "found by the crash report");
            }
        }
        char sites[160];
        FormatOverrunSites(sites, sizeof(sites));
        LogLine("FATAL", "[QUARANTINE] crash: %lu live block(s) have overwritten guard bytes (a buffer was written past its end); "
            "allocation sites that overran so far: %s; %lu overlapping allocation(s) were seen this session", overrunBlocks, sites,
            g_quarantineOverlaps);
    }
    if (g_heapFreeQuarantinePoison) {
        // Poison stamped every held block; any that changed since was written to while free.
        unsigned long found = 0;
        for (int poolIndex = 0; poolIndex < 2; ++poolIndex) {
            const std::deque<QuarantinedBlock>& queue = g_quarantinePools[poolIndex].queue;
            size_t limit = poolIndex == kObjectPool ? queue.size() : (queue.size() < 20000 ? queue.size() : 20000);
            for (size_t k = 0; k < limit; ++k) {
                const QuarantinedBlock& block = queue[queue.size() - 1 - k];
                if (block.size < sizeof(DWORD)) continue;
                SIZE_T firstChange = 0;
                unsigned changed = PoisonChangedDwords(block, &firstChange);
                if (changed && found < 6) ReportWriteAfterFree(block, changed, firstChange, "found by the crash report");
                if (changed) ++found;
            }
        }
        LogLine("FATAL", "[QUARANTINE] crash: %lu held block(s) among the most recently freed were written to after being freed", found);
    }
    {
        // The named objects freed most recently, newest first: what the game was tearing down.
        char names[600] = {};
        size_t used = 0;
        int listed = 0;
        for (int i = 0; i < kRecentLabels && listed < 12; ++i) {
            const RecentLabel& recent = g_recentLabels[(g_recentLabelNext + kRecentLabels - 1 - i) % kRecentLabels];
            if (!recent.label[0]) continue;
            int wrote = snprintf(names + used, sizeof(names) - used, "%s'%s' (vtable 0x%08lX, %.1f s ago)", listed ? ", " : "",
                recent.label, static_cast<unsigned long>(recent.vtable), (now - recent.tickMs) / 1000.0);
            if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(names) - used) break;
            used += static_cast<size_t>(wrote);
            ++listed;
        }
        if (listed) {
            LogLine("FATAL", "[QUARANTINE] crash: named objects freed most recently, newest first: %s", names);
        }
    }
    LeaveCriticalSection(&g_quarantineLock);
    if (hits == 0) {
        LogLine("FATAL", "[QUARANTINE] crash: no register and none of the top %lu stack words point into a block the quarantine "
            "holds (%lu blocks, the oldest freed %lu s ago). The object involved is older than the window, was never a "
            "heap block, or this is memory corruption rather than a use-after-free.",
            static_cast<unsigned long>(kCrashStackWords), static_cast<unsigned long>(held), oldest);
    }
#else
    (void)context; (void)record;
#endif
}

static std::string FileNameOfPath(const char* path) {
    const char* name = strrchr(path, '\\');
    const char* slash = strrchr(path, '/');
    if (slash && (!name || slash > name)) {
        name = slash;
    }
    return name ? name + 1 : path;
}

static void CopyMapText(char* dest, size_t destSize, const std::string& text) {
    snprintf(dest, destSize, "%s", text.c_str());
}

// A map is several files. The quest map (z35q1.mob, with a z35q1.mq archive whose map.txt names the
// terrain and the base map) is opened next to its base map (zone35x-lmp.mob), and its script can load
// more script-only maps (z35q1_strongwarriors.mob) with AddMob, so "the last .mob opened" names the
// wrong thing. A .mob is an extra map when a script already loaded says it AddMobs it; otherwise it
// starts a new map (or completes the current one, when it is the base map the quest asked for).
static const ULONGLONG kMapSameLoadMs = 30000;

static void NoteMapFile(const char* path, bool isMob) {
    const std::string name = FileNameOfPath(path);
    const std::string lower = LowerCaseCopy(name);
    const ULONGLONG now = GetTickCount64();

    if (!isMob) {
        while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
        CopyMapText(g_map.recentMpr, sizeof(g_map.recentMpr), name);
        g_map.recentMprTickMs = now;
        if (g_map.valid && g_map.mpr[0] == '\0' && now - g_map.openedTickMs < kMapSameLoadMs) {
            CopyMapText(g_map.mpr, sizeof(g_map.mpr), name);
        }
        InterlockedExchange(&g_currentMapLock, 0);
        return;
    }

    // Read outside the lock: this parses the map's script (cached for the next time).
    std::shared_ptr<MobScriptContext> context = LoadMobContext(path);
    std::string mqBase, mqMpr;
    bool isQuest = false;
    size_t dot = std::string(path).find_last_of('.');
    if (dot != std::string::npos) {
        std::string mqPath = std::string(path).substr(0, dot) + ".mq";
        if (GetFileAttributesA(mqPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            isQuest = true;
            ReadMqBaseMapName(mqPath, &mqBase, &mqMpr);
        }
    }
    if (!mqMpr.empty() && mqMpr.find('.') == std::string::npos) mqMpr += ".mpr";
    const std::string mqBaseFile = mqBase.empty() ? std::string() : mqBase + ".mob";

    while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
    const bool fresh = g_map.valid && now - g_map.openedTickMs < kMapSameLoadMs;
    bool startNew = false;
    if (g_map.valid && g_mapExpectedExtras.count(lower) != 0 &&
            !EqualsIgnoreCase(name.c_str(), g_map.quest) && !EqualsIgnoreCase(name.c_str(), g_map.base)) {
        ++g_map.extraCount;
        CopyMapText(g_map.lastExtra, sizeof(g_map.lastExtra), name);
    } else if (fresh && (EqualsIgnoreCase(name.c_str(), g_map.quest) || EqualsIgnoreCase(name.c_str(), g_map.base))) {
        // The same map file read again while it loads: nothing new.
    } else if (fresh && !isQuest && g_map.baseWanted[0] && lower == g_map.baseWanted) {
        CopyMapText(g_map.base, sizeof(g_map.base), name); // the quest opened first, its base map follows
    } else if (fresh && isQuest && g_map.quest[0] == '\0' && g_map.base[0] &&
            EqualsIgnoreCase(g_map.base, mqBaseFile.c_str())) {
        CopyMapText(g_map.quest, sizeof(g_map.quest), name); // the base map opened first, its quest follows
        if (g_map.mpr[0] == '\0' && !mqMpr.empty()) CopyMapText(g_map.mpr, sizeof(g_map.mpr), mqMpr);
    } else {
        startNew = true;
    }
    if (startNew) {
        memset(g_map.mpr, 0, sizeof(g_map.mpr));
        memset(g_map.base, 0, sizeof(g_map.base));
        memset(g_map.quest, 0, sizeof(g_map.quest));
        memset(g_map.baseWanted, 0, sizeof(g_map.baseWanted));
        memset(g_map.lastExtra, 0, sizeof(g_map.lastExtra));
        g_map.extraCount = 0;
        g_map.valid = true;
        g_map.openedTickMs = now;
        g_mapExpectedExtras.clear();
        if (isQuest) {
            CopyMapText(g_map.quest, sizeof(g_map.quest), name);
            if (!mqMpr.empty()) CopyMapText(g_map.mpr, sizeof(g_map.mpr), mqMpr);
            if (!mqBaseFile.empty()) CopyMapText(g_map.baseWanted, sizeof(g_map.baseWanted), LowerCaseCopy(mqBaseFile));
        } else {
            CopyMapText(g_map.base, sizeof(g_map.base), name);
            if (g_map.recentMpr[0] && now - g_map.recentMprTickMs < kMapSameLoadMs) {
                CopyMapText(g_map.mpr, sizeof(g_map.mpr), g_map.recentMpr);
            }
        }
    }
    if (context) {
        for (const std::string& added : context->addMobs) g_mapExpectedExtras.insert(added);
    }
    InterlockedExchange(&g_currentMapLock, 0);
}

static bool CopyMapState(MapLoadState* out) {
    while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) {
        Sleep(0);
    }
    *out = g_map;
    InterlockedExchange(&g_currentMapLock, 0);
    return out->valid;
}

static void NoteMapLoad(const char* path) {
    const bool isMob = HasFileExtension(path, "mob");
    if (!isMob && !HasFileExtension(path, "mpr")) {
        return;
    }
    if (!g_enableOverlay || !g_overlayShowMap) {
        // The overlay is the only user of the map identity; the rest still runs for .mob files.
        if (isMob) {
            RememberSeenMobPath(path);
            if (g_heapFreeQuarantineInstalled) {
                LogQuarantineStats("Map load");
            }
        }
        return;
    }
    if (isMob) {
        RememberSeenMobPath(path); // before NoteMapFile: it reads the file and the quest's base map
    }
    NoteMapFile(path, isMob);
    if (isMob && g_heapFreeQuarantineInstalled) {
        LogQuarantineStats("Map load");
    }
}

// Convert a Windows wide path to the log's narrow system-code-page format.
static void ConvertWidePath(LPCWSTR widePath, char* path, size_t pathSize) {
    if (!widePath || pathSize == 0) {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, widePath, -1, path, static_cast<int>(pathSize), NULL, NULL);
    path[pathSize - 1] = '\0';
}

// Forward CreateFileA and record the resulting handle/path for diagnostics.
static HANDLE WINAPI HookedCreateFileA(LPCSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileA(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        LogOpenedFile(handle, fileName, desiredAccess);
        if ((desiredAccess & GENERIC_READ) != 0) {
            ValidateMobFile(fileName);
            NoteMapLoad(fileName);
        }
    }
    return handle;
}

// Forward CreateFileW, converting its path before recording it.
static HANDLE WINAPI HookedCreateFileW(LPCWSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileW(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        char path[MAX_PATH] = {};
        ConvertWidePath(fileName, path, sizeof(path));
        LogOpenedFile(handle, path, desiredAccess);
        if ((desiredAccess & GENERIC_READ) != 0) {
            ValidateMobFile(path);
            NoteMapLoad(path);
        }
    }
    return handle;
}

// Forward ReadFile and log the first successful read for a tracked handle.
static BOOL WINAPI HookedReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead,
        LPDWORD bytesRead, LPOVERLAPPED overlapped) {
    BOOL result = g_originalReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
    if (result) {
        char path[MAX_PATH] = {};
        if (CopyAndMarkTrackedFileIo(file, false, path, sizeof(path))) {
            DWORD length = bytesRead ? *bytesRead : 0;
            LogFileIo("File read path=%s bytes=%lu handle=%p", path, length, file);
            LogFileContents("File read", path, buffer, length);
        }
    }
    return result;
}

// Forward WriteFile and log the first successful write for a tracked handle.
static BOOL WINAPI HookedWriteFile(HANDLE file, LPCVOID buffer, DWORD bytesToWrite,
        LPDWORD bytesWritten, LPOVERLAPPED overlapped) {
    BOOL result = g_originalWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);
    if (result) {
        char path[MAX_PATH] = {};
        if (CopyAndMarkTrackedFileIo(file, true, path, sizeof(path))) {
            DWORD length = bytesWritten ? *bytesWritten : 0;
            LogFileIo("File written path=%s bytes=%lu handle=%p", path, length, file);
            LogFileContents("File written", path, buffer, length);
        }
    }
    return result;
}

// Forward CloseHandle and discard any diagnostic state for the closed handle.
static BOOL WINAPI HookedCloseHandle(HANDLE handle) {
    BOOL result = g_originalCloseHandle(handle);
    if (result) {
        UntrackFileHandle(handle);
    }
    return result;
}

// These hooks only see calls game.exe makes directly through its own import
// table - which is all of them: its static CRT sends every malloc/free/new/
// delete to HeapAlloc/HeapFree/HeapReAlloc on its private heap.
// The default mode: only the padding, nothing tracked. Costs an addition per allocation.
static bool g_heapPaddingOnlyInstalled = false;

// Time spent inside the heap hooks, measured only while the profiler window is shown so the
// profiler can say what the hooks themselves cost the game's main thread.
static volatile LONG g_heapHookTicks = 0;   // QueryPerformanceCounter ticks
static volatile LONG g_heapHookCalls = 0;
struct HeapHookTimer {
    LONGLONG start = 0;
    HeapHookTimer() {
        if (g_profilerRunning) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            start = now.QuadPart;
        }
    }
    ~HeapHookTimer() {
        if (start) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            InterlockedExchangeAdd(&g_heapHookTicks, static_cast<LONG>(now.QuadPart - start));
            InterlockedIncrement(&g_heapHookCalls);
        }
    }
};

static LPVOID WINAPI PaddedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    HeapHookTimer timer;
    return g_originalHeapAlloc(heap, flags, size > 0x7FFFFF00 ? size : size + PaddingFor(size));
}

static LPVOID WINAPI PaddedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size) {
    HeapHookTimer timer;
    return g_originalHeapReAlloc(heap, flags, pointer, size > 0x7FFFFF00 ? size : size + PaddingFor(size));
}

static BOOL WINAPI HookedHeapFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    HeapHookTimer timer;
    if (g_heapFreeQuarantineInstalled && pointer && QuarantineHeapFree(heap, flags, pointer)) {
        return TRUE;
    }
    return g_originalHeapFree(heap, flags, pointer);
}

// Every allocation is recorded (start -> size) so a free of the middle of a block can be told from
// a free of a block's start; see g_liveBlocks.
static LPVOID WINAPI HookedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    HeapHookTimer timer;
    if (!g_heapFreeQuarantineInstalled || size > 0x7FFFFF00) {
        return g_originalHeapAlloc(heap, flags, size);
    }
    // Asked for a little more, so an overrun of the block lands in guard bytes we can check.
    const DWORD padding = PaddingFor(size);
    LPVOID result = g_originalHeapAlloc(heap, flags, size + padding);
    if (!result) return NULL;
    DWORD callers[kAllocCallerSlots] = {};
    CaptureCallers(callers, kAllocCallerSlots);
    memset(static_cast<BYTE*>(result) + size, kCanaryFill, padding);
    const DWORD start = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(result));
    EnterCriticalSection(&g_quarantineLock);
    // The heap must never return memory that is still in use (only an exact repeat of a live address is
    // caught; overlaps in the middle needed an ordered table).
    if (LiveBlock* same = g_liveBlocks.Find(start)) {
        ReportOverlap(start, static_cast<DWORD>(size), callers, start, *same);
    }
    LiveBlock block = {};
    block.size = static_cast<DWORD>(size);
    block.heap = heap;
    block.tickMs = GetTickCount();
    memcpy(block.callers, callers, sizeof(callers));
    g_liveBlocks.Put(start, block);
    LeaveCriticalSection(&g_quarantineLock);
    return result;
}

static LPVOID WINAPI HookedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size) {
    HeapHookTimer timer;
    if (g_heapFreeQuarantineInstalled && g_quarantineHolds && pointer && QuarantineHolds(pointer)) {
        // The game is resizing a block it already freed. Let the real
        // HeapReAlloc move/free it and the block would be freed a second time
        // when the quarantine evicts it, so hand back a fresh copy instead and
        // leave the held block alone.
        if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
            return NULL;
        }
        SIZE_T oldSize = HeapSize(heap, 0, pointer);
        LPVOID fresh = HeapAlloc(heap,
            flags & (HEAP_ZERO_MEMORY | HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS), size);
        if (fresh && oldSize != static_cast<SIZE_T>(-1)) {
            memcpy(fresh, pointer, oldSize < size ? oldSize : size);
        }
        EnterCriticalSection(&g_quarantineLock);
        unsigned long number = ++g_quarantineReallocRedirects;
        LeaveCriticalSection(&g_quarantineLock);
        if (number <= kQuarantineReallocLogLimit) {
            LogLine("WARN", "[QUARANTINE] HeapReAlloc of an already-freed block %p (redirect #%lu)", pointer, number);
            LogCallerCandidates("realloc of a freed block");
        }
        return fresh;
    }
    if (!g_heapFreeQuarantineInstalled || size > 0x7FFFFF00) {
        return g_originalHeapReAlloc(heap, flags, pointer, size);
    }
    if (pointer) {
        EnterCriticalSection(&g_quarantineLock);
        const DWORD oldStart = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer));
        LiveBlock* old = g_liveBlocks.Find(oldStart);
        BYTE found[kMaxCanaryBytes];
        if (old && CanaryDamaged(oldStart, old->size, found, false)) {
            ReportOverrun(oldStart, *old, found, "found when the game resized the block");
        }
        LeaveCriticalSection(&g_quarantineLock);
    }
    const DWORD padding = PaddingFor(size);
    LPVOID result = g_originalHeapReAlloc(heap, flags, pointer, size + padding);
    if (result) {
        DWORD callers[kAllocCallerSlots] = {};
        CaptureCallers(callers, kAllocCallerSlots);
        memset(static_cast<BYTE*>(result) + size, kCanaryFill, padding);
        EnterCriticalSection(&g_quarantineLock);
        if (pointer) g_liveBlocks.Erase(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)));
        LiveBlock block = {};
        block.size = static_cast<DWORD>(size);
        block.heap = heap;
        block.tickMs = GetTickCount();
        memcpy(block.callers, callers, sizeof(callers));
        g_liveBlocks.Put(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(result)), block);
        LeaveCriticalSection(&g_quarantineLock);
    }
    return result;
}

static BOOL WINAPI HookedHeapDestroy(HANDLE heap) {
    if (g_heapFreeQuarantineInstalled) {
        QuarantineForgetHeap(heap);
    }
    return g_originalHeapDestroy(heap);
}

// Overwrite one COM vtable slot process-wide (vtables are normally shared by
// every instance of a class) and return the previous function pointer, or
// NULL if the slot already holds newFunction. When alreadyPatched is given,
// it distinguishes "already patched by an earlier call" (not an error, e.g. a
// second surface sharing the same vtable) from a real VirtualProtect failure.
static void* PatchVTableSlot(void* comObject, int slotIndex, void* newFunction,
        bool* alreadyPatched = NULL) {
    if (alreadyPatched) {
        *alreadyPatched = false;
    }
    if (!comObject) {
        return NULL;
    }
    void*** selfPtr = reinterpret_cast<void***>(comObject);
    void** vtable = *selfPtr;
    if (vtable[slotIndex] == newFunction) {
        if (alreadyPatched) {
            *alreadyPatched = true;
        }
        return NULL;
    }
    void* original = vtable[slotIndex];
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slotIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return NULL;
    }
    vtable[slotIndex] = newFunction;
    VirtualProtect(&vtable[slotIndex], sizeof(void*), oldProtect, &oldProtect);
    return original;
}

// Largest Blt destination area seen on the primary surface so far; used to
// tell a full-frame present apart from small partial updates (HUD, cursor).
static volatile LONG g_overlayFrameMaxBltArea = 0;

// Some game states (menus, alt-tab) can trigger more than one present-style
// call (Flip/Blt/BltFast) for what is visually the same single frame, which
// would otherwise double-count FPS. Real distinct frames can never arrive
// faster than the display's max refresh; treat anything within 2ms of the
// last counted frame as a duplicate signal for that same frame, not a new one.
static LARGE_INTEGER g_overlayPerfFrequency = {};
static LARGE_INTEGER g_overlayLastCountedFrameTime = {};
static const double OVERLAY_MIN_FRAME_INTERVAL_MS = 2.0;

static void CountPresentedFrame() {
    if (g_overlayPerfFrequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_overlayPerfFrequency);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_overlayLastCountedFrameTime.QuadPart != 0 && g_overlayPerfFrequency.QuadPart != 0) {
        double elapsedMs = static_cast<double>(now.QuadPart - g_overlayLastCountedFrameTime.QuadPart) *
            1000.0 / static_cast<double>(g_overlayPerfFrequency.QuadPart);
        if (elapsedMs < OVERLAY_MIN_FRAME_INTERVAL_MS) {
            return;
        }
        LONG slot = g_overlayFrameTimeNext;
        g_overlayFrameTimesMs[slot] = static_cast<float>(elapsedMs);
        g_overlayFrameTimeNext = (slot + 1) % OVERLAY_FRAME_TIME_CAPACITY;
        if (g_overlayFrameTimeCount < OVERLAY_FRAME_TIME_CAPACITY) {
            InterlockedIncrement(&g_overlayFrameTimeCount);
        }
    }
    g_overlayLastCountedFrameTime = now;
    g_profilerFrameThreadId = GetCurrentThreadId();
    InterlockedIncrement(&g_overlayFrameCount);
}

// Count one presented frame for the overlay's FPS/frametime readout.
static HRESULT WINAPI HookedDDFlip(void* self, void* targetOverride, DWORD flags) {
    CountPresentedFrame();
    return g_originalDDFlip(self, targetOverride, flags);
}

// A NULL destRect conventionally means "blit the entire surface", which is
// only true for a full-frame present; otherwise only count a Blt as a frame
// once its area is close to the largest one seen (partial/HUD/cursor blits
// are typically much smaller than a full-frame redraw).
static HRESULT WINAPI HookedDDBlt(void* self, LPRECT destRect, void* srcSurface,
        LPRECT srcRect, DWORD flags, void* bltFx) {
    if (!destRect) {
        CountPresentedFrame();
    } else {
        LONG area = (destRect->right - destRect->left) * (destRect->bottom - destRect->top);
        LONG maxArea = g_overlayFrameMaxBltArea;
        if (area > maxArea) {
            InterlockedExchange(&g_overlayFrameMaxBltArea, area);
            maxArea = area;
        }
        if (maxArea > 0 && area * 10 >= maxArea * 9) {
            CountPresentedFrame();
        }
    }
    return g_originalDDBlt(self, destRect, srcSurface, srcRect, flags, bltFx);
}

// BltFast has no destination rect, only an origin; a full-frame present
// conventionally targets (0,0), while sprite/HUD/cursor blits usually don't.
static HRESULT WINAPI HookedDDBltFast(void* self, DWORD x, DWORD y, void* srcSurface,
        LPRECT srcRect, DWORD trans) {
    if (x == 0 && y == 0) {
        CountPresentedFrame();
    }
    return g_originalDDBltFast(self, x, y, srcSurface, srcRect, trans);
}

// When the game creates its primary (front-buffer) surface, patch that
// surface's own Flip/Blt/BltFast vtable slots so every present is counted,
// without touching any other surface (textures, sprites, etc. would over-count).
static HRESULT WINAPI HookedDDCreateSurface(void* self, void* surfaceDesc,
        void** lplpDDSurface, IUnknown* outer) {
    HRESULT result = g_originalDDCreateSurface(self, surfaceDesc, lplpDDSurface, outer);
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
            InterlockedExchange(&g_overlayFrameCounterActive, 1);
            LogLine("INFO", "Primary surface Flip/Blt/BltFast hooked for the overlay's FPS counter");
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
        LogLine("INFO", "DirectDraw CreateSurface hooked for the overlay's FPS counter");
    } else if (!alreadyPatched) {
        LogLine("WARN", "DirectDraw CreateSurface was not hooked; FPS counter will read n/a");
    }
}

// Log DirectDraw initialization results without changing the returned object.
static HRESULT WINAPI HookedDirectDrawCreate(const GUID* guid, void** directDraw,
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

// Log DirectDrawEx initialization results without changing the returned object.
static HRESULT WINAPI HookedDirectDrawCreateEx(const GUID* guid, void** directDraw,
        const GUID* interfaceId, IUnknown* outerUnknown) {
    HRESULT result = g_originalDirectDrawCreateEx(guid, directDraw, interfaceId,
        outerUnknown);
    LogLine(FAILED(result) ? "ERROR" : "INFO",
        "DirectDrawCreateEx result=0x%08lX object=%p", result,
        directDraw ? *directDraw : NULL);
    if (g_enableOverlay && SUCCEEDED(result) && directDraw && *directDraw) {
        IdentifyDirectDrawBackend();
        InstallDirectDrawFrameCounterHooks(*directDraw);
    }
    return result;
}


// Patch the executable's import address table instead of replacing kernel32
// exports globally. This limits logging to calls made through game.exe's
// imported file APIs and leaves other processes untouched.
static bool PatchImportedFunction(HMODULE module, const char* functionName,
    ULONG_PTR replacement, ULONG_PTR* original) {
    if (!module || !functionName || !replacement || !original) {
        return false;
    }

    BYTE* base = reinterpret_cast<BYTE*>(module);
    IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    IMAGE_NT_HEADERS* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_DATA_DIRECTORY importDirectory = ntHeaders->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0) {
        return false;
    }

    IMAGE_IMPORT_DESCRIPTOR* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDirectory.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        IMAGE_THUNK_DATA* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + imports->FirstThunk);
        IMAGE_THUNK_DATA* names = imports->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk)
            : addresses;
        if (!names) {
            continue;
        }

        for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            IMAGE_IMPORT_BY_NAME* importedName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importedName->Name), functionName) != 0) {
                continue;
            }

            DWORD oldProtection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                    PAGE_READWRITE, &oldProtection)) {
                return false;
            }
            *original = addresses->u1.Function;
            addresses->u1.Function = replacement;
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                oldProtection, &oldProtection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                sizeof(addresses->u1.Function));
            return true;
        }
    }
    return false;
}

// Patch game.exe imports for the file APIs used by the file-I/O logger.
static void InstallFileIoHooks() {
    HMODULE process = GetModuleHandleA(NULL);
    if (!process) {
        LogLine("WARN", "Could not locate the game executable for file-I/O hooks");
        return;
    }

    bool hooked = false;
    hooked = PatchImportedFunction(process, "CreateFileA",
        reinterpret_cast<ULONG_PTR>(HookedCreateFileA), reinterpret_cast<ULONG_PTR*>(&g_originalCreateFileA)) || hooked;
    hooked = PatchImportedFunction(process, "CreateFileW",
        reinterpret_cast<ULONG_PTR>(HookedCreateFileW), reinterpret_cast<ULONG_PTR*>(&g_originalCreateFileW)) || hooked;
    hooked = PatchImportedFunction(process, "ReadFile",
        reinterpret_cast<ULONG_PTR>(HookedReadFile), reinterpret_cast<ULONG_PTR*>(&g_originalReadFile)) || hooked;
    hooked = PatchImportedFunction(process, "WriteFile",
        reinterpret_cast<ULONG_PTR>(HookedWriteFile), reinterpret_cast<ULONG_PTR*>(&g_originalWriteFile)) || hooked;
    hooked = PatchImportedFunction(process, "CloseHandle",
        reinterpret_cast<ULONG_PTR>(HookedCloseHandle), reinterpret_cast<ULONG_PTR*>(&g_originalCloseHandle)) || hooked;
    hooked = PatchImportedFunction(process, "DirectDrawCreate",
        reinterpret_cast<ULONG_PTR>(HookedDirectDrawCreate), reinterpret_cast<ULONG_PTR*>(&g_originalDirectDrawCreate)) || hooked;
    hooked = PatchImportedFunction(process, "DirectDrawCreateEx",
        reinterpret_cast<ULONG_PTR>(HookedDirectDrawCreateEx), reinterpret_cast<ULONG_PTR*>(&g_originalDirectDrawCreateEx)) || hooked;
    if (!hooked) {
        // These hooks back file-I/O logging, .mob validation, and the overlay's
        // FPS counter, so a generic success line here would not indicate which
        // feature is active.
        LogLine("WARN", "File-I/O hooks not installed");
    }
}

// Patch game.exe's own imports of HeapFree/HeapReAlloc/HeapDestroy so
// HEAP_FREE_QUARANTINE can hold freed blocks back (see QuarantineHeapFree).
static void InstallHeapHooks() {
    if (!g_enableHeapFreeQuarantine && g_heapAllocPadding == 0) {
        return;
    }
    HMODULE process = GetModuleHandleA(NULL);
    if (!process) {
        LogLine("WARN", "[QUARANTINE] Not activated: could not locate the game executable");
        return;
    }

    InitGameImageRanges(); // also what the profiler finds game.exe's code by
    if (!g_enableHeapFreeQuarantine) {
        // Only the padding: the pointer the game gets is the block's start, so it frees it as always.
        PatchImportedFunction(process, "HeapAlloc",
            reinterpret_cast<ULONG_PTR>(PaddedHeapAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapAlloc));
        PatchImportedFunction(process, "HeapReAlloc",
            reinterpret_cast<ULONG_PTR>(PaddedHeapReAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapReAlloc));
        if (!g_originalHeapAlloc || !g_originalHeapReAlloc) {
            LogLine("WARN", "[HEAPFIX] Not activated: game.exe does not import HeapAlloc/HeapReAlloc directly");
            return;
        }
        g_heapPaddingOnlyInstalled = true;
        LogLine("INFO", "[HEAPFIX] Active: every allocation the game makes is padded by %d bytes (a quarter of its size, up to "
            "512 bytes, for blocks of 1 KB or more); nothing is tracked, set HEAP_FREE_QUARANTINE=true for overrun reports",
            g_heapAllocPadding);
        return;
    }

    g_canaryBytes = static_cast<DWORD>(g_heapAllocPadding);
    g_quarantineHolds = g_heapFreeQuarantineMb > 0 || g_heapFreeQuarantineObjectsMb > 0;
    InitializeCriticalSection(&g_quarantineLock);
    g_quarantinePools[0].byteLimit = static_cast<SIZE_T>(g_heapFreeQuarantineMb) * 1048576;
    g_quarantinePools[0].maxBlocks = static_cast<size_t>(g_heapFreeQuarantineMb) * kQuarantineBlocksPerMb;
    g_quarantinePools[kObjectPool].byteLimit = static_cast<SIZE_T>(g_heapFreeQuarantineObjectsMb) * 1048576;
    g_quarantinePools[kObjectPool].maxBlocks = static_cast<size_t>(g_heapFreeQuarantineObjectsMb) * kQuarantineBlocksPerMb;

    PatchImportedFunction(process, "HeapAlloc",
        reinterpret_cast<ULONG_PTR>(HookedHeapAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapAlloc));
    bool freeHooked = PatchImportedFunction(process, "HeapFree",
        reinterpret_cast<ULONG_PTR>(HookedHeapFree), reinterpret_cast<ULONG_PTR*>(&g_originalHeapFree));
    PatchImportedFunction(process, "HeapReAlloc",
        reinterpret_cast<ULONG_PTR>(HookedHeapReAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapReAlloc));
    PatchImportedFunction(process, "HeapDestroy",
        reinterpret_cast<ULONG_PTR>(HookedHeapDestroy), reinterpret_cast<ULONG_PTR*>(&g_originalHeapDestroy));

    if (!freeHooked || !g_originalHeapFree) {
        LogLine("WARN", "[QUARANTINE] Not activated: game.exe does not import HeapFree directly");
        return;
    }
    if (!g_originalHeapDestroy || !g_originalHeapReAlloc || !g_originalHeapAlloc) {
        // Never expected (game.exe imports both), but without them a destroyed
        // heap's held blocks could not be forgotten and a resize of a freed
        // block could not be redirected - so hold nothing.
        LogLine("WARN", "[QUARANTINE] Not activated: game.exe does not import HeapAlloc/HeapDestroy/HeapReAlloc");
        return;
    }
    g_heapFreeQuarantineInstalled = true;
    HANDLE watch = CreateThread(NULL, 0, OverrunWatchThread, NULL, 0, NULL);
    if (watch) CloseHandle(watch);
    char holding[120];
    if (g_quarantineHolds) {
        snprintf(holding, sizeof(holding), "freed blocks are held back (data window %d MB, plus %d MB for small objects) before "
            "being really freed", g_heapFreeQuarantineMb, g_heapFreeQuarantineObjectsMb);
    } else {
        snprintf(holding, sizeof(holding), "freed blocks are freed at once (windows set to 0 MB)");
    }
    LogLine("DEBUG", "[QUARANTINE] Active: every allocation padded by %d bytes (more for blocks of 1 KB or more) and tracked; %s%s",
        g_heapAllocPadding, holding,
        g_heapFreeQuarantinePoison ? "; POISON MODE: freed objects get 0xDDDDDDDD as their vtable, the workaround is off for them" : "");
    long mode = ReadGameCrtHeapMode();
    if (mode != 1) {
        LogLine("WARN", "[QUARANTINE] game.exe CRT heap mode reads %ld (expected 1 = system heap); "
            "with mode 2/3 its small-block heap frees small blocks WITHOUT calling HeapFree, so those "
            "are not held back", mode);
    }
}

// Start a numbered exception block in the diagnostic log.
static void LogErrorBlockStart() {
    LONG blockNumber = InterlockedIncrement(&g_errorBlockNumber);
    LogLine("FATAL", "============= ERROR %ld LOG =============", blockNumber);
}

// Close the current numbered exception block in the diagnostic log.
static void LogErrorBlockEnd() {
    LogLine("FATAL", "===========================================");
}

// Clear or separate the log at process startup, with a cross-instance mutex.
static void PrepareLogFile() {
    if (!g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    if (g_clearLogOnStart) {
        g_logClearMutex = CreateMutexA(NULL, TRUE, "Local\\UniversalModLogClear");
        if (!g_logClearMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_logClearMutex) {
                CloseHandle(g_logClearMutex);
                g_logClearMutex = NULL;
            }
            return;
        }

        FILE* file = fopen(g_logPath, "w");
        if (file) {
            fclose(file);
        }
        return;
    }

    FILE* file = fopen(g_logPath, "a");
    if (!file) {
        return;
    }
    fputs("\n\n", file);
    fclose(file);
}

// Read one registry string value, accepting normal and expandable strings.
static bool ReadRegistryString(HKEY root, const char* subKey, const char* valueName,
        char* value, DWORD valueSize) {
    HKEY key = NULL;
    DWORD type = 0;
    DWORD size = valueSize;
    LONG result = RegOpenKeyExA(root, subKey, 0, KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS) {
        return false;
    }

    result = RegQueryValueExA(key, valueName, NULL, &type,
        reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || valueSize == 0) {
        return false;
    }

    value[valueSize - 1] = '\0';
    return value[0] != '\0';
}

// Try several registry locations/value names because Wine and Windows expose
// adapter metadata under different layouts.
static bool ReadFirstRegistryString(HKEY root, const char* const* subKeys,
        size_t subKeyCount, const char* const* valueNames, size_t valueNameCount,
        char* value, DWORD valueSize) {
    for (size_t keyIndex = 0; keyIndex < subKeyCount; ++keyIndex) {
        for (size_t valueIndex = 0; valueIndex < valueNameCount; ++valueIndex) {
            if (ReadRegistryString(root, subKeys[keyIndex], valueNames[valueIndex],
                    value, valueSize)) {
                return true;
            }
        }
    }
    return false;
}

// Infer a vendor name from an adapter description when registry data omits it.
static const char* InferGraphicsProvider(const char* description, bool isWine) {
    if (description) {
        if (strstr(description, "NVIDIA") || strstr(description, "GeForce")) {
            return "NVIDIA";
        }
        if (strstr(description, "AMD") || strstr(description, "Radeon") ||
                strstr(description, "ATI")) {
            return "AMD";
        }
        if (strstr(description, "Intel")) {
            return "Intel";
        }
    }
    return isWine ? "Wine" : "unknown";
}

typedef unsigned int VulkanUint32;
typedef int VulkanResult;
typedef void* VulkanInstance;
typedef void* VulkanPhysicalDevice;
typedef VulkanResult (WINAPI *VulkanCreateInstanceFunction)(const void*, const void*, VulkanInstance*);
typedef VulkanResult (WINAPI *VulkanEnumeratePhysicalDevicesFunction)(VulkanInstance,
    VulkanUint32*, VulkanPhysicalDevice*);
typedef void (WINAPI *VulkanGetPhysicalDevicePropertiesFunction)(VulkanPhysicalDevice, void*);
typedef void (WINAPI *VulkanDestroyInstanceFunction)(VulkanInstance, const void*);

// Map a Vulkan PCI vendor ID to a readable vendor name.
static const char* GetVulkanProvider(VulkanUint32 vendorId) {
    switch (vendorId) {
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
        return "AMD";
    case 0x8086:
        return "Intel";
    default:
        return "unknown";
    }
}

// Enumerate Vulkan physical devices and log their driver metadata.
static bool LogVulkanGraphicsInformation() {
    // Vulkan is queried dynamically so the DLL remains loadable on systems
    // without Vulkan. Its physical-device driverVersion is preferred over
    // Wine's synthetic Windows registry version.
    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) {
        LogLine("SYSINFO", "Vulkan GPU driver information is unavailable");
        return false;
    }

    FARPROC createAddress = GetProcAddress(vulkan, "vkCreateInstance");
    FARPROC enumerateAddress = GetProcAddress(vulkan, "vkEnumeratePhysicalDevices");
    FARPROC propertiesAddress = GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties");
    FARPROC destroyAddress = GetProcAddress(vulkan, "vkDestroyInstance");
    VulkanCreateInstanceFunction createInstance = NULL;
    VulkanEnumeratePhysicalDevicesFunction enumeratePhysicalDevices = NULL;
    VulkanGetPhysicalDevicePropertiesFunction getPhysicalDeviceProperties = NULL;
    VulkanDestroyInstanceFunction destroyInstance = NULL;
    memcpy(&createInstance, &createAddress, sizeof(createInstance));
    memcpy(&enumeratePhysicalDevices, &enumerateAddress, sizeof(enumeratePhysicalDevices));
    memcpy(&getPhysicalDeviceProperties, &propertiesAddress, sizeof(getPhysicalDeviceProperties));
    memcpy(&destroyInstance, &destroyAddress, sizeof(destroyInstance));
    if (!createInstance || !enumeratePhysicalDevices || !getPhysicalDeviceProperties ||
            !destroyInstance) {
        FreeLibrary(vulkan);
        LogLine("SYSINFO", "Vulkan GPU driver information functions are unavailable");
        return false;
    }

    struct VulkanInstanceCreateInfo {
        VulkanUint32 structureType;
        const void* next;
        VulkanUint32 flags;
        const void* applicationInfo;
        VulkanUint32 enabledLayerCount;
        const char* const* enabledLayerNames;
        VulkanUint32 enabledExtensionCount;
        const char* const* enabledExtensionNames;
    } createInfo = {1, NULL, 0, NULL, 0, NULL, 0, NULL};
    VulkanInstance instance = NULL;
    if (createInstance(&createInfo, NULL, &instance) != 0 || !instance) {
        FreeLibrary(vulkan);
        LogLine("SYSINFO", "Vulkan instance creation failed");
        return false;
    }

    VulkanUint32 deviceCount = 0;
    if (enumeratePhysicalDevices(instance, &deviceCount, NULL) != 0 || deviceCount == 0) {
        destroyInstance(instance, NULL);
        FreeLibrary(vulkan);
        LogLine("SYSINFO", "Vulkan returned no GPU adapters");
        return false;
    }
    if (deviceCount > 16) {
        deviceCount = 16;
    }

    VulkanPhysicalDevice devices[16] = {};
    if (enumeratePhysicalDevices(instance, &deviceCount, devices) != 0) {
        destroyInstance(instance, NULL);
        FreeLibrary(vulkan);
        LogLine("SYSINFO", "Vulkan GPU enumeration failed");
        return false;
    }

    for (VulkanUint32 index = 0; index < deviceCount; ++index) {
        unsigned char properties[4096] = {};
        getPhysicalDeviceProperties(devices[index], properties);
        VulkanUint32 driverVersion = 0;
        VulkanUint32 vendorId = 0;
        char deviceName[256] = {};
        memcpy(&driverVersion, properties + 4, sizeof(driverVersion));
        memcpy(&vendorId, properties + 8, sizeof(vendorId));
        memcpy(deviceName, properties + 20, sizeof(deviceName) - 1);
        if (vendorId == 0x10DE) {
            LogLine("SYSINFO", "Vulkan GPU index=%lu name=%s vendor=%s driver_version=%lu.%lu.%lu raw=0x%08lX source=Vulkan",
                index, deviceName[0] != '\0' ? deviceName : "unknown",
                GetVulkanProvider(vendorId), driverVersion >> 22,
                (driverVersion >> 14) & 0xFF, driverVersion & 0x3FFF,
                driverVersion);
        } else {
            LogLine("SYSINFO", "Vulkan GPU index=%lu name=%s vendor=%s driver_version=%lu.%lu.%lu raw=0x%08lX source=Vulkan",
                index, deviceName[0] != '\0' ? deviceName : "unknown",
                GetVulkanProvider(vendorId), driverVersion >> 22,
                (driverVersion >> 12) & 0x3FF, driverVersion & 0xFFF,
                driverVersion);
        }
    }

    destroyInstance(instance, NULL);
    FreeLibrary(vulkan);
    return deviceCount > 0;
}

// Log Windows/Wine OS registry metadata for the running process.
static void LogOperatingSystemInformation() {
    char productName[128] = {};
    char displayVersion[64] = {};
    char buildNumber[32] = {};
    ReadRegistryString(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName",
        productName, sizeof(productName));
    ReadRegistryString(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion",
        displayVersion, sizeof(displayVersion));
    ReadRegistryString(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber",
        buildNumber, sizeof(buildNumber));

    OSVERSIONINFOEXA version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef LONG (WINAPI *RtlGetVersionFunction)(OSVERSIONINFOEXA*);
    RtlGetVersionFunction rtlGetVersion = NULL;
    FARPROC rtlGetVersionAddress = ntdll
        ? GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    memcpy(&rtlGetVersion, &rtlGetVersionAddress, sizeof(rtlGetVersion));
    if (rtlGetVersion && rtlGetVersion(&version) == 0) {
        LogLine("SYSINFO", "OS version=%lu.%lu build=%lu platform=%lu",
            version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
            version.dwPlatformId);
    } else {
        LogLine("SYSINFO", "OS version could not be queried");
    }

    if (productName[0] != '\0') {
        LogLine("SYSINFO", "OS name=%s version=%s build_number=%s", productName,
            displayVersion[0] != '\0' ? displayVersion : "unknown",
            buildNumber[0] != '\0' ? buildNumber : "unknown");
    }

    typedef const char* (__cdecl *WineGetVersionFunction)();
    WineGetVersionFunction wineGetVersion = NULL;
    FARPROC wineGetVersionAddress = ntdll
        ? GetProcAddress(ntdll, "wine_get_version") : NULL;
    memcpy(&wineGetVersion, &wineGetVersionAddress, sizeof(wineGetVersion));
    if (wineGetVersion) {
        const char* wineVersion = wineGetVersion();
        if (wineVersion && wineVersion[0] != '\0') {
            LogLine("SYSINFO", "Wine version=%s", wineVersion);
        }
    }
}

// Log CPU architecture, processor identity, and memory availability.
static void LogHardwareInformation() {
    SYSTEM_INFO systemInfo = {};
    GetNativeSystemInfo(&systemInfo);

    const char* architecture = "unknown";
    if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        architecture = "x86";
    } else if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        architecture = "x64";
    } else if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        architecture = "arm64";
    } else if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM) {
        architecture = "arm";
    }

    LogLine("SYSINFO", "CPU architecture=%s logical_processors=%lu page_size=%lu allocation_granularity=%lu",
        architecture, systemInfo.dwNumberOfProcessors, systemInfo.dwPageSize,
        systemInfo.dwAllocationGranularity);

    char processorName[256] = {};
    if (ReadRegistryString(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "ProcessorNameString", processorName, sizeof(processorName))) {
        LogLine("SYSINFO", "CPU model=%s", processorName);
    } else {
        LogLine("SYSINFO", "CPU model could not be queried");
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        LogLine("SYSINFO", "RAM total_mb=%llu available_mb=%llu",
            static_cast<unsigned long long>(memory.ullTotalPhys / (1024 * 1024)),
            static_cast<unsigned long long>(memory.ullAvailPhys / (1024 * 1024)));
    } else {
        LogLine("SYSINFO", "RAM information could not be queried, error=%lu", GetLastError());
    }
}

// Prefer Vulkan GPU metadata and fall back to registry adapter metadata.
static void LogGraphicsInformation() {
    // Vulkan provides the authoritative report when available. The registry
    // path below is retained as a fallback for systems without Vulkan.
    bool hasVulkanGpu = LogVulkanGraphicsInformation();
    HKEY videoKey = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Video", 0,
            KEY_ENUMERATE_SUB_KEYS, &videoKey) != ERROR_SUCCESS) {
        LogLine("SYSINFO", "GPU information could not be queried");
        return;
    }

    DWORD index = 0;
    DWORD loggedAdapters = 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef const char* (__cdecl *WineGetVersionFunction)();
    WineGetVersionFunction wineGetVersion = NULL;
    FARPROC wineGetVersionAddress = ntdll
        ? GetProcAddress(ntdll, "wine_get_version") : NULL;
    memcpy(&wineGetVersion, &wineGetVersionAddress, sizeof(wineGetVersion));
    bool isWine = wineGetVersion != NULL;
    char adapterKeyName[128] = {};
    while (index < 32) {
        DWORD adapterKeyNameSize = sizeof(adapterKeyName);
        LONG result = RegEnumKeyExA(videoKey, index++, adapterKeyName,
            &adapterKeyNameSize, NULL, NULL, NULL, NULL);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            continue;
        }

        char adapterRootKey[192] = {};
        char settingsKey[192] = {};
        snprintf(adapterRootKey, sizeof(adapterRootKey),
            "SYSTEM\\CurrentControlSet\\Control\\Video\\%s", adapterKeyName);
        snprintf(settingsKey, sizeof(settingsKey), "%s\\0000", adapterRootKey);
        const char* adapterKeys[] = {settingsKey, adapterRootKey};
        const char* descriptionNames[] = {"DriverDesc", "Description"};
        const char* versionNames[] = {"DriverVersion", "DriverVer"};
        const char* providerNames[] = {"ProviderName", "Provider", "DriverProvider"};
        char description[256] = {};
        char driverVersion[128] = {};
        char provider[128] = {};
        bool hasDescription = ReadFirstRegistryString(HKEY_LOCAL_MACHINE, adapterKeys,
            sizeof(adapterKeys) / sizeof(adapterKeys[0]), descriptionNames,
            sizeof(descriptionNames) / sizeof(descriptionNames[0]), description,
            sizeof(description));
        bool hasDriverVersion = ReadFirstRegistryString(HKEY_LOCAL_MACHINE, adapterKeys,
            sizeof(adapterKeys) / sizeof(adapterKeys[0]), versionNames,
            sizeof(versionNames) / sizeof(versionNames[0]), driverVersion,
            sizeof(driverVersion));
        bool hasProvider = ReadFirstRegistryString(HKEY_LOCAL_MACHINE, adapterKeys,
            sizeof(adapterKeys) / sizeof(adapterKeys[0]), providerNames,
            sizeof(providerNames) / sizeof(providerNames[0]), provider,
            sizeof(provider));
        if (!hasVulkanGpu && (hasDescription || hasDriverVersion || hasProvider)) {
            if (!hasProvider) {
                strncpy(provider, InferGraphicsProvider(description, isWine),
                    sizeof(provider) - 1);
            }
            LogLine("SYSINFO", "GPU index=%lu name=%s driver_version=%s provider=%s driver_source=%s",
                loggedAdapters++, hasDescription ? description : "unknown",
                hasDriverVersion ? driverVersion : "unknown",
                provider, isWine ? "Wine registry" : "Windows registry");
        }
    }

    RegCloseKey(videoKey);
    if (loggedAdapters == 0 && !hasVulkanGpu) {
        LogLine("SYSINFO", "No GPU information was found");
    }
}

// Emit the complete startup system-information section.
// Reports whether heap debug flags from Image File Execution Options (e.g.
// GlobalFlag=0x20, "free checking") actually reached this process, so a value
// set in the Wine prefix can be confirmed from inside the running game instead
// of inferred from behavior. NtGlobalFlag is at PEB+0x68 (32-bit layout, and
// um.dll is always built 32-bit); the flags the process heap is actually
// running with are at heap+0x40, where 0x40 = free checking and 0x20 = tail
// checking (both verified against Wine 11 with a test program).
static void LogHeapDebugFlags() {
    const BYTE* peb = reinterpret_cast<const BYTE*>(__readfsdword(0x30));
    DWORD ntGlobalFlag = peb ? *reinterpret_cast<const DWORD*>(peb + 0x68) : 0;

    DWORD heapFlags = 0;
    bool haveHeapFlags = false;
    const BYTE* heap = static_cast<const BYTE*>(GetProcessHeap());
    if (heap && !IsBadReadPtr(heap + 0x40, sizeof(DWORD))) {
        heapFlags = *reinterpret_cast<const DWORD*>(heap + 0x40);
        haveHeapFlags = true;
    }
    LogLine("SYSINFO", "Heap debug flags: NtGlobalFlag=0x%08lX (free-check requested=%s, tail-check requested=%s) "
        "process_heap_flags=0x%08lX (free-checking active=%s, tail-checking active=%s)",
        ntGlobalFlag, (ntGlobalFlag & 0x20) ? "yes" : "no", (ntGlobalFlag & 0x10) ? "yes" : "no",
        heapFlags, !haveHeapFlags ? "unknown" : (heapFlags & 0x40) ? "yes" : "no",
        !haveHeapFlags ? "unknown" : (heapFlags & 0x20) ? "yes" : "no");
}

static void LogGameCrtHeapMode() {
    long mode = ReadGameCrtHeapMode();
    if (mode < 0) {
        LogLine("SYSINFO", "game.exe CRT heap mode: unknown (not the known game.exe build)");
        return;
    }
    LogLine("SYSINFO", "game.exe CRT heap mode: %ld (%s)", mode,
        mode == 1 ? "system heap - every malloc/free goes through HeapAlloc/HeapFree" :
        mode == 2 ? "V5 small-block heap" : mode == 3 ? "V6 small-block heap" :
        "not initialised yet or unrecognized");
}

static void LogSystemInformation() {
    LogLine("SYSINFO", "============= SYSTEM INFORMATION =============");
    LogOperatingSystemInformation();
    LogHardwareInformation();
    LogGraphicsInformation();
    LogHeapDebugFlags();
    LogGameCrtHeapMode();
    LogLine("SYSINFO", "==============================================");
}

// Classify exception codes that should not be resumed blindly.
// Keep exception names stable in logs instead of exposing only numeric codes.
static const char* GetExceptionCaseName(const EXCEPTION_RECORD* record) {
    if (!record) {
        return "UNKNOWN_EXCEPTION";
    }

    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_GUARD_PAGE:
        return "EXCEPTION_GUARD_PAGE";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

// Log the exception, register state, faulting module, stack, and tracked files.
// This handler deliberately does not modify the faulting context: generic stack
// surgery cannot safely skip a failed DirectDraw/texture operation.
static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (!g_enableCrashLogging) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_crashLogInProgress, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : NULL;
    CONTEXT* context = exceptionInfo ? exceptionInfo->ContextRecord : NULL;

    LogErrorBlockStart();

    if (!record) {
        LogLine("FATAL", "Unhandled exception had no exception record");
    } else {
        LogLine("FATAL", "Unhandled exception code=0x%08lX flags=0x%08lX address=%p parameters=%lu case=%s",
            record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress,
            record->NumberParameters, GetExceptionCaseName(record));
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            LogLine("FATAL", "Access violation type=%s address=%p",
                record->ExceptionInformation[0] == 0 ? "read" : "write",
                reinterpret_cast<void*>(static_cast<ULONG_PTR>(record->ExceptionInformation[1])));
        }
    }

    if (context) {
#if defined(_M_IX86) || defined(__i386__)
        LogLine("FATAL", "CPU registers eax=0x%08lX ebx=0x%08lX ecx=0x%08lX edx=0x%08lX esi=0x%08lX edi=0x%08lX ebp=0x%08lX esp=0x%08lX eip=0x%08lX",
            context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi,
            context->Edi, context->Ebp, context->Esp, context->Eip);
        if (context->Eip == 0x90909090 || context->Eip == 0xCCCCCCCC || context->Eip == 0xCDCDCDCD) {
            LogLine("CRASH", "Instruction pointer is a debug fill or NOP-sled sentinel (0x%08lX); original control flow cannot be reconstructed safely",
                context->Eip);
        }
#elif defined(_M_X64) || defined(__x86_64__)
        LogLine("FATAL", "CPU registers rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rbp=%p rsp=%p rip=%p",
            reinterpret_cast<void*>(context->Rax), reinterpret_cast<void*>(context->Rbx),
            reinterpret_cast<void*>(context->Rcx), reinterpret_cast<void*>(context->Rdx),
            reinterpret_cast<void*>(context->Rsi), reinterpret_cast<void*>(context->Rdi),
            reinterpret_cast<void*>(context->Rbp), reinterpret_cast<void*>(context->Rsp),
            reinterpret_cast<void*>(context->Rip));
#endif
    }

    LogQuarantineCrashAnalysis(context, record);

    if (record && record->ExceptionAddress) {
        HMODULE module = NULL;
        char modulePath[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module) &&
            GetModuleFileNameA(module, modulePath, sizeof(modulePath)) != 0) {
            char* moduleName = strrchr(modulePath, '\\');
            if (!moduleName) {
                moduleName = strrchr(modulePath, '/');
            }
            LogLine("FATAL", "Faulting module=%s", moduleName ? moduleName + 1 : modulePath);
        }
    }

    void* stack[32] = {};
    USHORT frameCount = CaptureStackBackTrace(0, sizeof(stack) / sizeof(stack[0]), stack, NULL);
    for (USHORT i = 0; i < frameCount; ++i) {
        LogLine("FATAL", "Stack frame=%u address=%p", i, stack[i]);
    }

    // The crash location above is the critical fact to preserve, so it is
    // logged before anything riskier runs. Writing a minidump loads
    // dbghelp.dll and walks the whole process's memory/threads, which can
    // itself hang or crash when the process is already in a bad state (e.g.
    // heap corruption) - if that ran first, a failure here would silently
    // mask the real crash location by preventing the log lines above from
    // ever being written.
    WriteCrashDump(exceptionInfo);
    LogTrackedFileHandles();

    LogErrorBlockEnd();
    InterlockedExchange(&g_crashLogInProgress, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Load optional configuration overrides from um.cfg. Missing files are created
// with documented defaults; malformed or unknown lines are ignored.
static void LoadConfigFile(const char* dllPath) {
    char configPath[MAX_PATH] = {};
    size_t dllPathLen = strlen(dllPath);
    if (dllPathLen >= sizeof(configPath)) {
        dllPathLen = sizeof(configPath) - 1;
    }
    memcpy(configPath, dllPath, dllPathLen);
    configPath[dllPathLen] = '\0';
    size_t lastSlash = strlen(configPath);
    while (lastSlash > 0 && configPath[lastSlash - 1] != '\\' && configPath[lastSlash - 1] != '/') {
        --lastSlash;
    }
    if (lastSlash > 0) {
        // Strip the separator itself, not just what follows it - the "\\um.log"/
        // "\\um.cfg" appended below already supply their own leading separator,
        // so keeping this one too produced a harmless but confusing doubled
        // backslash in every logged path (e.g. "Universal-Mod\\um-crashdump-...").
        configPath[lastSlash - 1] = '\0';
    }
    memcpy(g_logPath, configPath, strlen(configPath) + 1);
    strncat(g_logPath, "\\um.log", sizeof(g_logPath) - strlen(g_logPath) - 1);
    strncat(configPath, "\\um.cfg", sizeof(configPath) - strlen(configPath) - 1);

    FILE* file = fopen(configPath, "r");
    if (!file) {
        // Create um.cfg if it doesn't exist, populated with every setting's
        // default value and documentation from the table above.
        file = fopen(configPath, "w");
        if (file) {
            WriteDefaultConfigFile(file);
            fclose(file);
        }
        return;
    }

    char line[256] = {};
    while (fgets(line, sizeof(line), file)) {
        // trim leading whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) {
            start++;
        }

        // skip empty lines and comments
        if (*start == '\0' || *start == ';' || *start == '#' || *start == '\n') {
            continue;
        }

        // find the '=' delimiter
        char* equals = strchr(start, '=');
        if (!equals) {
            continue;
        }

        // extract key
        char key[128] = {};
        size_t keyLen = equals - start;
        if (keyLen >= sizeof(key)) {
            continue;
        }
        strncpy(key, start, keyLen);
        key[keyLen] = '\0';

        // trim trailing whitespace from key
        while (keyLen > 0 && (key[keyLen - 1] == ' ' || key[keyLen - 1] == '\t')) {
            key[--keyLen] = '\0';
        }

        // extract value
        char* value = equals + 1;
        while (*value && (*value == ' ' || *value == '\t')) {
            value++;
        }

        // trim trailing whitespace and newline from value
        char* valueEnd = value + strlen(value) - 1;
        while (valueEnd > value && (*valueEnd == ' ' || *valueEnd == '\t' || *valueEnd == '\n' || *valueEnd == '\r')) {
            *valueEnd-- = '\0';
        }

        bool matched = false;
        for (size_t i = 0; i < kSettingCount; ++i) {
            if (EqualsIgnoreCase(key, kSettings[i].key)) {
                ApplySettingValue(kSettings[i], value);
                matched = true;
                break;
            }
        }
        if (!matched) {
            LogLine("WARN", "Unknown um.cfg setting '%s' ignored (typo?)", key);
        }
    }

    fclose(file);
}

// Re-reads um.cfg and re-applies whatever can safely take effect while the
// game is already running (most flags, overlay settings).
// A few things - installing IAT/DirectDraw hooks and creating the overlay
// window - only ever happen once at DLL attach, so toggling those settings
// on for the first time via reload still needs a game restart to take effect.
static void ReloadConfiguration() {
    if (!g_dllModule) {
        return;
    }
    char dllPath[MAX_PATH] = {};
    if (GetModuleFileNameA(g_dllModule, dllPath, MAX_PATH) == 0) {
        LogLine("WARN", "Config reload failed: could not resolve um.dll's own path");
        return;
    }
    LoadConfigFile(dllPath);
    LogLine("INFO", "um.cfg reloaded");
}

// Synthesize one US-QWERTY backtick press using scan code 0x29.
static void SendQwertyBacktickPress(bool logRewrite) {
    if (g_enableKeyboardRewriteLogging && logRewrite) {
        LogLine("KBRW", "Rewriting physical scan code 0x29 as US-QWERTY backtick");
    }

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = 0x29; // us qwerty backtick/tilde key scan code
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = 0x29;
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        LogLine("WARN", "SendInput failed for US-QWERTY backtick, error=%lu", GetLastError());
    }
}

// Synthesize one US-QWERTY number-row press from a virtual-key code.
static void SendQwertyNumberKeyPress(BYTE vkCode, bool logRewrite) {
    // map vk codes 48-57 (0-9) to scan codes 0x02-0x0b (1-0)
    BYTE scanCodeMap[] = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    if (vkCode < 0x30 || vkCode > 0x39) {
        return;
    }
    BYTE scanCode = scanCodeMap[vkCode - 0x30];
    if (g_enableKeyboardRewriteLogging && logRewrite) {
        LogLine("KBRW", "Rewriting virtual key 0x%02X as US-QWERTY scan code 0x%02X", vkCode, scanCode);
    }

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = scanCode;
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = scanCode;
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        LogLine("WARN", "SendInput failed for number key vk=0x%02X, error=%lu", vkCode, GetLastError());
    }
}

// Intercept the backtick and number-row keys and rewrite them as US-QWERTY presses;
// also watch for the overlay toggle key and the config-reload key, both
// independently of the rewrite feature.
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // Do not process the synthetic events generated by SendInput below.
        if (kb && (kb->flags & LLKHF_INJECTED) == 0) {
            if (kb->vkCode == g_reloadConfigKey) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    if (!g_keyboardRewriteKeyDown[g_reloadConfigKey]) {
                        g_keyboardRewriteKeyDown[g_reloadConfigKey] = true;
                        ReloadConfiguration();
                    }
                } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    g_keyboardRewriteKeyDown[g_reloadConfigKey] = false;
                }
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam); // let the key still reach the game
            }
            if (g_enableOverlay && kb->vkCode == g_overlayToggleKey) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    if (!g_keyboardRewriteKeyDown[g_overlayToggleKey]) {
                        g_keyboardRewriteKeyDown[g_overlayToggleKey] = true;
                        g_overlayVisible = g_overlayVisible ? 0 : 1;
                        if (g_overlayWindow) {
                            ShowWindow(g_overlayWindow, g_overlayVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
                        }
                        LogLine("INFO", "Overlay toggled %s", g_overlayVisible ? "visible" : "hidden");
                    }
                } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    g_keyboardRewriteKeyDown[g_overlayToggleKey] = false;
                }
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam); // let the key still reach the game
            }
            if (g_enableOverlay && kb->vkCode == g_profilerKey) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    if (!g_keyboardRewriteKeyDown[g_profilerKey]) {
                        g_keyboardRewriteKeyDown[g_profilerKey] = true;
                        g_profilerVisible = g_profilerVisible ? 0 : 1;
                        if (g_profilerWindow) {
                            ShowWindow(g_profilerWindow, g_profilerVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
                        }
                        LogLine("INFO", "Profiler toggled %s", g_profilerVisible ? "on" : "off");
                    }
                } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    g_keyboardRewriteKeyDown[g_profilerKey] = false;
                }
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam); // let the key still reach the game
            }
            if (g_enableOverlay && g_overlayLogEnabled && kb->vkCode == g_overlayLogToggleKey) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    if (!g_keyboardRewriteKeyDown[g_overlayLogToggleKey]) {
                        g_keyboardRewriteKeyDown[g_overlayLogToggleKey] = true;
                        g_overlayLogVisible = g_overlayLogVisible ? 0 : 1;
                        if (g_overlayLogWindow) {
                            ShowWindow(g_overlayLogWindow, g_overlayLogVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
                        }
                        LogLine("INFO", "Overlay log panel toggled %s", g_overlayLogVisible ? "visible" : "hidden");
                    }
                } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    g_keyboardRewriteKeyDown[g_overlayLogToggleKey] = false;
                }
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam); // let the key still reach the game
            }
            if (g_enableKeyboardRewrites) {
                if (kb->vkCode == 0xC0 || kb->scanCode == 0x29) {
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        bool logRewrite = !g_keyboardRewriteKeyDown[0xC0];
                        g_keyboardRewriteKeyDown[0xC0] = true;
                        SendQwertyBacktickPress(logRewrite);
                    } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        g_keyboardRewriteKeyDown[0xC0] = false;
                    }
                    return 1; // swallow the original key event
                }
                if (kb->vkCode >= 0x30 && kb->vkCode <= 0x39) {
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        bool logRewrite = !g_keyboardRewriteKeyDown[kb->vkCode];
                        g_keyboardRewriteKeyDown[kb->vkCode] = true;
                        SendQwertyNumberKeyPress(kb->vkCode, logRewrite);
                    } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        g_keyboardRewriteKeyDown[kb->vkCode] = false;
                    }
                    return 1; // swallow the original key event
                }
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// Install the low-level keyboard hook and keep it alive with a message loop.
// WH_KEYBOARD_LL callbacks are delivered to this thread, not to the game thread.
// Synthetic SendInput events are filtered out by LowLevelKeyboardProc.
DWORD WINAPI KeyPopupThread(LPVOID lpParameter) {
    LabelCurrentThread(L"um.dll: KeyPopup");
    HMODULE module = reinterpret_cast<HMODULE>(lpParameter);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, module, 0);
    if (!g_keyboardHook) {
        LogLine("ERROR", "SetWindowsHookExW failed, error=%lu", GetLastError());
        return 0;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = NULL;
    return 0;
}

// Match the game's own top-level window so the overlay can track its position.
static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId != GetCurrentProcessId() || !IsWindowVisible(hwnd) ||
        GetWindow(hwnd, GW_OWNER) != NULL) {
        return TRUE;
    }
    char title[256] = {};
    if (GetWindowTextA(hwnd, title, sizeof(title)) == 0) {
        return TRUE;
    }
    *reinterpret_cast<HWND*>(lParam) = hwnd;
    return FALSE;
}

static HWND FindGameWindow() {
    HWND result = NULL;
    EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

// Initial/minimum panel width; RenderOverlayPanel widens the actual canvas to
// fit whatever text it measures, so long lines are never clipped.
static const int OVERLAY_PANEL_WIDTH = 340;
static const int OVERLAY_PANEL_MARGIN = 12;
static const int OVERLAY_GRAPH_HEIGHT = 150;
static const int OVERLAY_SECTION_GAP = 10;
// Matches the main panel's font size/line spacing (16px font, 20px pitch) so
// the log panel isn't noticeably smaller/harder to read than the rest of the
// overlay.
static const int OVERLAY_LOG_LINE_HEIGHT = 20;

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
        LogLine("WARN", "CreateCompatibleDC for overlay canvas failed, error=%lu", GetLastError());
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
        LogLine("WARN", "CreateDIBSection for overlay canvas failed, error=%lu", GetLastError());
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

// Text rows of the main panel, built once per repaint and shared by the height
// computation, the width measurement and the drawing so the three can never
// disagree about which rows exist.
struct OverlayLine {
    char text[160];
    bool alert; // drawn in the warning color
};

// ---------------------------------------------------------------------------------------------
// Main-thread profiler. While the profiler window is shown, a sampler thread stops the
// game's frame thread ~250 times a second, copies its registers and the top of its stack, lets it
// go and adds what it was doing to a call tree. game.exe has no symbols and is built with frame
// pointer omission, so functions are found from the code itself (every target of a direct call and
// every code pointer in the data sections, when preceded by the padding the compiler leaves between
// functions) and a call chain is the return-address-looking words on the stack. It is statistical:
// a function on 20% of the samples costs about 20% of a frame.
// ---------------------------------------------------------------------------------------------
static const int kProfileTextWidth = 96;
static const int kProfileMaxLines = 56;
static char g_profileLines[kProfileMaxLines][kProfileTextWidth];
static int g_profileLineCount = 0;
static volatile LONG g_profileLock = 0;
static volatile LONG g_profilerThreadAlive = 0;
static HANDLE g_profilerStopEvent = NULL;
static std::vector<uint32_t> g_profileFunctions;   // sorted entry addresses of game.exe's functions
static std::unordered_map<DWORD, std::string> g_profileHints; // what an unnamed function refers to
static std::unordered_map<DWORD, std::string> g_profileNames;

struct ProfileNode {
    DWORD function;
    DWORD count;     // samples with this function on the stack below its parents
    DWORD self;      // samples where it was the innermost game function
    int firstChild;
    int nextSibling;
};
static const size_t kProfileMaxNodes = 20000;
static const DWORD kProfileStackBytes = 8192;

struct ProfileWindow {
    std::vector<ProfileNode> nodes; // [0] is the root
    DWORD samples = 0;
    std::vector<std::pair<std::string, DWORD>> categories; // where the innermost frame was: game.exe or a module
    std::vector<std::pair<std::string, DWORD>> externals;  // "module!export" of the innermost frame outside game.exe
    void Reset() {
        nodes.assign(1, ProfileNode{0, 0, 0, -1, -1});
        samples = 0;
        categories.clear();
        externals.clear();
    }
};

// Function entries and the names worked out from the code (see profile_symbols.hpp): classes' virtual
// methods, constructors, and hints from the strings and Windows APIs a function uses.
static void BuildProfileFunctionTable() {
    g_profileFunctions.clear();
    g_profileNames.clear();
    g_profileHints.clear();
    profsym::Image image;
    image.data = reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(g_imageLow));
    image.base = g_imageLow;
    image.size = g_imageHigh - g_imageLow;
    if (!image.Parse()) return;
    g_profileFunctions = profsym::FindFunctions(image);
    profsym::Symbols symbols = profsym::Analyze(image, g_profileFunctions);
    for (const auto& entry : symbols.names) g_profileNames[entry.first] = entry.second;
    for (const auto& entry : symbols.hints) g_profileHints[entry.first] = entry.second;
    LogLine("DEBUG", "[PERF] found %lu functions in game.exe: %lu named from %lu class vtables, %lu with a string or API hint",
        static_cast<unsigned long>(g_profileFunctions.size()), static_cast<unsigned long>(g_profileNames.size()),
        static_cast<unsigned long>(symbols.namedVtables), static_cast<unsigned long>(g_profileHints.size()));
}

// The optional um-names.txt next to um.dll: one "address name" pair per line, '#' starts a comment.
static void LoadProfileNames() {
    if (g_knownGameBuild) {
        g_profileNames[0x457970] = "MainMessageHandler";
    }
    char path[MAX_PATH] = {};
    if (!g_dllModule || GetModuleFileNameA(g_dllModule, path, sizeof(path)) == 0) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, sizeof(path) - (slash + 1 - path), "um-names.txt");
    FILE* file = fopen(path, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char* text = line;
        while (*text == ' ' || *text == '\t') ++text;
        if (*text == '#' || *text == '\r' || *text == '\n' || *text == '\0') continue;
        char* end = NULL;
        unsigned long address = strtoul(text, &end, 16);
        if (end == text || address == 0) continue;
        while (*end == ' ' || *end == '\t') ++end;
        std::string name = end;
        size_t comment = name.find('#');
        if (comment != std::string::npos) name.resize(comment);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) name.pop_back();
        if (!name.empty()) g_profileNames[static_cast<DWORD>(address)] = name;
    }
    fclose(file);
}

// The function a code address belongs to (its entry), 0 when there is none close enough.
static DWORD ProfileFunctionOf(DWORD address) {
    auto after = std::upper_bound(g_profileFunctions.begin(), g_profileFunctions.end(), address);
    if (after == g_profileFunctions.begin()) return 0;
    DWORD entry = *(after - 1);
    return address - entry < 0x10000 ? entry : 0;
}

static std::string ProfileNameOf(DWORD function) {
    auto known = g_profileNames.find(function);
    if (known != g_profileNames.end()) return known->second;
    char text[24];
    snprintf(text, sizeof(text), "sub_%lX", static_cast<unsigned long>(function));
    auto hint = g_profileHints.find(function);
    return hint != g_profileHints.end() ? std::string(text) + " " + hint->second : std::string(text);
}

// Which module an address is in: "game.exe" or the DLL's file name, and the name of the export it is
// in or just after (both cached per module).
struct ProfileModuleInfo {
    std::string name;
    profsym::ExportSymbols exports;
};
static std::string ProfileModuleOf(DWORD address, std::string* symbol) {
    static std::unordered_map<DWORD, ProfileModuleInfo> cache; // only the sampler thread calls this
    symbol->clear();
    if (address >= g_imageLow && address < g_imageHigh) return "game.exe";
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0 || !info.AllocationBase) return "other";
    const DWORD base = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(info.AllocationBase));
    auto found = cache.find(base);
    if (found == cache.end()) {
        ProfileModuleInfo module;
        module.name = "other";
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(info.AllocationBase), path, sizeof(path)) != 0) {
            const char* slash = strrchr(path, '\\');
            module.name = slash ? slash + 1 : path;
            const BYTE* header = reinterpret_cast<const BYTE*>(info.AllocationBase);
            if (!IsBadReadPtr(header, 0x200) && header[0] == 'M' && header[1] == 'Z') {
                DWORD lfanew = 0;
                memcpy(&lfanew, header + 0x3c, 4);
                DWORD imageSize = 0;
                if (lfanew < 0x1000 && !IsBadReadPtr(header + lfanew, 0x100)) memcpy(&imageSize, header + lfanew + 24 + 56, 4);
                if (imageSize >= 0x1000 && imageSize < 0x10000000) {
                    profsym::Image image;
                    image.data = header;
                    image.base = base;
                    image.size = imageSize;
                    module.exports.Load(image);
                }
            }
        }
        found = cache.emplace(base, std::move(module)).first;
    }
    // "!name+0x10" for an address in or just after an export, else "+0xRVA" (a DLL without exports, or an
    // internal function of one).
    const std::string described = found->second.exports.Describe(address - base);
    if (!described.empty()) {
        *symbol = "!" + described;
    } else {
        char offset[24];
        snprintf(offset, sizeof(offset), "+0x%lX", static_cast<unsigned long>(address - base));
        *symbol = offset;
    }
    return found->second.name;
}

// An import thunk or tail jump: what a direct call into another module goes through.
static bool ProfileIsThunk(DWORD address) {
    if (address < g_codeLow || address + 2 > g_codeHigh) return false;
    const BYTE* p = reinterpret_cast<const BYTE*>(address);
    return p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25);
}

// The call chain, innermost function first. A word on the stack that looks like a return address may
// be a leftover in a frame's uninitialised space, so a direct call is only believed when its target is at or
// just before the address of the frame found so far (the function it must have called), or is a thunk;
// indirect calls (virtual, through the import table) cannot be checked.
static int ProfileBuildPath(DWORD eip, const DWORD* stack, DWORD words, DWORD* path, int capacity) {
    int length = 0;
    DWORD inner = 0; // where the frame found so far is executing; 0 = the leaf is outside the game
    if (eip >= g_codeLow && eip < g_codeHigh) {
        DWORD function = ProfileFunctionOf(eip);
        if (function) { path[length++] = function; inner = eip; }
    }
    for (DWORD i = 0; i < words && length < capacity; ++i) {
        DWORD value = stack[i];
        if (value < g_codeLow + 6 || value >= g_codeHigh) continue;
        DWORD target = 0;
        if (!LooksLikeReturnAddress(reinterpret_cast<const BYTE*>(value), &target, true)) continue;
        if (target != 0 && !(target <= inner && inner - target < 0x8000) && !ProfileIsThunk(target)) continue; // stale
        DWORD function = ProfileFunctionOf(value - 1);
        if (!function) continue;
        if (length == 0 || path[length - 1] != function) path[length++] = function;
        inner = value;
    }
    return length;
}

static void ProfileAddSample(ProfileWindow& window, const DWORD* path, int length, const std::string& category,
        const std::string& external) {
    ++window.samples;
    if (!external.empty() && window.externals.size() < 400) {
        bool seen = false;
        for (auto& entry : window.externals) {
            if (entry.first == external) { ++entry.second; seen = true; break; }
        }
        if (!seen) window.externals.push_back(std::make_pair(external, 1u));
    }
    bool counted = false;
    for (auto& entry : window.categories) {
        if (entry.first == category) { ++entry.second; counted = true; break; }
    }
    if (!counted) window.categories.push_back(std::make_pair(category, 1u));
    int current = 0;
    ++window.nodes[0].count;
    for (int i = length - 1; i >= 0; --i) { // outermost first
        int child = window.nodes[current].firstChild;
        while (child >= 0 && window.nodes[child].function != path[i]) child = window.nodes[child].nextSibling;
        if (child < 0) {
            if (window.nodes.size() >= kProfileMaxNodes) break;
            window.nodes.push_back(ProfileNode{path[i], 0, 0, -1, window.nodes[current].firstChild});
            child = static_cast<int>(window.nodes.size()) - 1;
            window.nodes[current].firstChild = child;
        }
        ++window.nodes[child].count;
        current = child;
    }
    ++window.nodes[current].self;
}

static void ProfileEmit(const ProfileWindow& window, int parent, int depth, double frameMs,
        std::vector<std::string>& lines, size_t maxLines, DWORD minCount) {
    std::vector<int> children;
    for (int child = window.nodes[parent].firstChild; child >= 0; child = window.nodes[child].nextSibling) {
        if (window.nodes[child].count >= minCount) children.push_back(child);
    }
    std::sort(children.begin(), children.end(),
        [&](int a, int b) { return window.nodes[a].count > window.nodes[b].count; });
    for (int index : children) {
        if (lines.size() >= maxLines) return;
        const ProfileNode& node = window.nodes[index];
        // A frame that is on nearly every sample and does nothing itself (the loop that calls the
        // real work) is left out; its children are shown at the same depth.
        const bool wrapper = node.count * 100 >= window.samples * 97 && node.self * 100 <= window.samples * 3;
        if (!wrapper) {
            char text[kProfileTextWidth];
            const double share = static_cast<double>(node.count) / window.samples;
            std::string indent(static_cast<size_t>(depth > 10 ? 10 : depth) * 2, ' ');
            std::string name = ProfileNameOf(node.function);
            if (name.size() > 46) name.resize(46);
            if (frameMs > 0.0) {
                snprintf(text, sizeof(text), "%6.2f %s%s", share * frameMs, indent.c_str(), name.c_str());
            } else {
                snprintf(text, sizeof(text), "%5.1f%% %s%s", share * 100.0, indent.c_str(), name.c_str());
            }
            if (node.self * 50 >= window.samples) { // 2% or more spent in the function's own code
                size_t used = strlen(text);
                snprintf(text + used, sizeof(text) - used, frameMs > 0.0 ? "  (own %.2f)" : "  (own %.1f%%)",
                    frameMs > 0.0 ? static_cast<double>(node.self) / window.samples * frameMs
                                  : static_cast<double>(node.self) / window.samples * 100.0);
            }
            lines.push_back(text);
        }
        ProfileEmit(window, index, wrapper ? depth : depth + 1, frameMs, lines, maxLines, minCount);
    }
}

static void PublishProfile(const std::vector<std::string>& lines) {
    while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
    g_profileLineCount = 0;
    for (const std::string& line : lines) {
        if (g_profileLineCount >= kProfileMaxLines) break;
        snprintf(g_profileLines[g_profileLineCount++], kProfileTextWidth, "%s", line.c_str());
    }
    InterlockedExchange(&g_profileLock, 0);
}

static DWORD WINAPI ProfilerThread(LPVOID) {
    LabelCurrentThread(L"um.dll: Profiler");
    if (g_codeLow == 0) InitGameImageRanges(); // not done when the heap hooks are off
    if (g_profileFunctions.empty()) {
        BuildProfileFunctionTable();
        LoadProfileNames();
    }
    static BYTE stackCopy[kProfileStackBytes];
    ProfileWindow window;
    window.Reset();
    HANDLE thread = NULL;
    DWORD threadId = 0;
    ULONGLONG windowStart = GetTickCount64();
    LONG windowFrames = g_overlayFrameCount;
    LARGE_INTEGER counterFrequency = {};
    QueryPerformanceFrequency(&counterFrequency);
    InterlockedExchange(&g_heapHookTicks, 0);
    InterlockedExchange(&g_heapHookCalls, 0);
    int snapshots = 0;
    const DWORD periodMs = g_profilerHz >= 1000 ? 1 : static_cast<DWORD>(1000 / (g_profilerHz > 0 ? g_profilerHz : 250));

    while (g_profilerRunning) {
        DWORD wanted = g_profilerFrameThreadId;
        if (wanted != 0 && wanted != threadId) {
            if (thread) CloseHandle(thread);
            thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, wanted);
            threadId = thread ? wanted : 0;
            window.Reset(); // measure from the moment there is a thread to look at
            windowStart = GetTickCount64();
            windowFrames = g_overlayFrameCount;
            InterlockedExchange(&g_heapHookTicks, 0);
            InterlockedExchange(&g_heapHookCalls, 0);
        }
        if (thread) {
            // Nothing between SuspendThread and ResumeThread may take a lock the game thread could hold.
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            SIZE_T copied = 0;
            bool have = false;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(thread, &context)) {
                    SIZE_T got = 0;
                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Esp), stackCopy, kProfileStackBytes, &got);
                    copied = got;
                    have = true;
                }
                ResumeThread(thread);
            }
            if (have) {
                DWORD path[64];
                int length = ProfileBuildPath(context.Eip, reinterpret_cast<const DWORD*>(stackCopy),
                    static_cast<DWORD>(copied / 4), path, 64);
                std::string symbol;
                std::string module = ProfileModuleOf(context.Eip, &symbol);
                // Outside the game, also who called in: the innermost game function on the stack.
                std::string external;
                if (module != "game.exe") {
                    external = module + symbol;
                    if (length > 0) external += " <- " + ProfileNameOf(path[0]);
                }
                ProfileAddSample(window, path, length, module, external);
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - windowStart >= 2000 && window.samples > 0) {
            const LONG frames = g_overlayFrameCount - windowFrames;
            const double frameMs = frames > 0 ? static_cast<double>(now - windowStart) / frames : 0.0;
            std::vector<std::string> lines;
            char text[kProfileTextWidth];
            if (frameMs > 0.0) {
                snprintf(text, sizeof(text), "%.1f ms/frame (%.0f FPS), %lu samples", frameMs, 1000.0 / frameMs,
                    static_cast<unsigned long>(window.samples));
            } else {
                snprintf(text, sizeof(text), "no frames presented, %lu samples", static_cast<unsigned long>(window.samples));
            }
            lines.push_back(text);
            std::sort(window.categories.begin(), window.categories.end(),
                [](const std::pair<std::string, DWORD>& a, const std::pair<std::string, DWORD>& b) { return a.second > b.second; });
            std::string where = "in:";
            for (size_t i = 0; i < window.categories.size() && i < 4; ++i) {
                char part[64];
                snprintf(part, sizeof(part), " %s %.0f%%", window.categories[i].first.c_str(),
                    window.categories[i].second * 100.0 / window.samples);
                where += part;
            }
            lines.push_back(where);
            const LONG hookTicks = InterlockedExchange(&g_heapHookTicks, 0);
            const LONG hookCalls = InterlockedExchange(&g_heapHookCalls, 0);
            if (hookCalls > 0 && frames > 0 && counterFrequency.QuadPart > 0) {
                const double hookMs = static_cast<double>(hookTicks) * 1000.0 / counterFrequency.QuadPart / frames;
                snprintf(text, sizeof(text), "um.dll heap hooks: %.0f calls, %.2f ms per frame (%.0f%%)",
                    static_cast<double>(hookCalls) / frames, hookMs, frameMs > 0.0 ? hookMs * 100.0 / frameMs : 0.0);
                lines.push_back(text);
            }
            ProfileEmit(window, 0, 0, frameMs, lines, static_cast<size_t>(g_profilerLineCount) + lines.size(),
                window.samples / 100 > 0 ? window.samples / 100 : 1);
            if (!window.externals.empty() && frameMs > 0.0) {
                std::sort(window.externals.begin(), window.externals.end(),
                    [](const std::pair<std::string, DWORD>& a, const std::pair<std::string, DWORD>& b) { return a.second > b.second; });
                lines.push_back("outside game.exe (ms per frame):");
                for (size_t i = 0; i < window.externals.size() && i < 6; ++i) {
                    if (window.externals[i].second * 100 < window.samples) break; // under 1%
                    std::string name = window.externals[i].first;
                    if (name.size() > 70) name.resize(70);
                    snprintf(text, sizeof(text), "%6.2f  %s", static_cast<double>(window.externals[i].second) / window.samples * frameMs, name.c_str());
                    lines.push_back(text);
                }
            }
            PublishProfile(lines);
            if (++snapshots % 15 == 1) {
                for (const std::string& line : lines) LogLine("DEBUG", "[PERF] %s", line.c_str());
            }
            window.Reset();
            windowStart = now;
            windowFrames = g_overlayFrameCount;
            InterlockedExchange(&g_heapHookTicks, 0);
            InterlockedExchange(&g_heapHookCalls, 0);
        }
        WaitForSingleObject(g_profilerStopEvent, periodMs);
    }
    if (thread) CloseHandle(thread);
    InterlockedExchange(&g_profilerThreadAlive, 0);
    return 0;
}

// Called from the overlay's timer: the sampler runs only while its window is on screen.
static void UpdateProfilerState() {
    const bool wanted = g_profilerVisible;
    if (wanted && !g_profilerRunning) {
        if (InterlockedCompareExchange(&g_profilerThreadAlive, 1, 0) != 0) return; // the last one is still ending
        if (!g_profilerStopEvent) g_profilerStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        ResetEvent(g_profilerStopEvent);
        g_profilerRunning = 1;
        HANDLE handle = CreateThread(NULL, 0, ProfilerThread, NULL, 0, NULL);
        if (handle) {
            CloseHandle(handle);
        } else {
            g_profilerRunning = 0;
            InterlockedExchange(&g_profilerThreadAlive, 0);
        }
    } else if (!wanted && g_profilerRunning) {
        g_profilerRunning = 0;
        if (g_profilerStopEvent) SetEvent(g_profilerStopEvent);
        while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
        g_profileLineCount = 0;
        InterlockedExchange(&g_profileLock, 0);
    }
}

struct OverlayContent {
    OverlayLine top[8];
    int topCount = 0;
    OverlayLine bottom[10];
    int bottomCount = 0;
    bool compact = false;
    bool hasTitle = false;
    bool showGraph = false;
    bool showThreads = false;
};

static const COLORREF OVERLAY_ALERT_COLOR = RGB(255, 90, 90);
// Address-space use above this fraction is drawn as an alert: a 32-bit process
// dies from running out of address space long before it runs out of RAM.
static const double OVERLAY_ADDRESS_SPACE_ALERT_FRACTION = 0.85;

static void AddOverlayLine(OverlayLine* lines, int* count, int capacity, bool alert,
        const char* format, ...) {
    if (*count >= capacity) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(lines[*count].text, sizeof(lines[*count].text), format, arguments);
    va_end(arguments);
    lines[*count].alert = alert;
    ++*count;
}

// 1% low FPS (average of the slowest 1% of frames) and worst frame time over
// the last OVERLAY_FRAME_STATS_WINDOW_MS; false until enough frames exist.
static bool ComputeFrameStats(double* onePercentLowFps, double* worstFrameMs, double* windowSeconds) {
    static float window[OVERLAY_FRAME_TIME_CAPACITY]; // only the overlay thread calls this
    LONG count = g_overlayFrameTimeCount;
    LONG next = g_overlayFrameTimeNext;
    int used = 0;
    double totalMs = 0.0;
    double worst = 0.0;
    for (LONG i = 0; i < count && totalMs < OVERLAY_FRAME_STATS_WINDOW_MS; ++i) {
        float frameMs = g_overlayFrameTimesMs[(next - 1 - i + OVERLAY_FRAME_TIME_CAPACITY) % OVERLAY_FRAME_TIME_CAPACITY];
        window[used++] = frameMs;
        totalMs += frameMs;
        if (frameMs > worst) {
            worst = frameMs;
        }
    }
    if (used < 10) {
        return false;
    }
    int slowest = used / 100 > 1 ? used / 100 : 1;
    std::nth_element(window, window + (slowest - 1), window + used,
        [](float a, float b) { return a > b; });
    double sumMs = 0.0;
    for (int i = 0; i < slowest; ++i) {
        sumMs += window[i];
    }
    *onePercentLowFps = sumMs > 0.0 ? 1000.0 * slowest / sumMs : 0.0;
    *worstFrameMs = worst;
    *windowSeconds = totalMs / 1000.0;
    return true;
}

static ULONGLONG GetProcessUptimeSeconds() {
    FILETIME creation = {}, exitTime = {}, kernel = {}, user = {}, now = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        return 0;
    }
    GetSystemTimeAsFileTime(&now);
    ULONGLONG created = (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    ULONGLONG current = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return current > created ? (current - created) / 10000000ULL : 0;
}

// "2m14s" under an hour, "1h05m" beyond.
static void FormatDuration(ULONGLONG seconds, char* out, size_t outSize) {
    if (seconds >= 3600) {
        snprintf(out, outSize, "%luh%02lum", static_cast<unsigned long>(seconds / 3600),
            static_cast<unsigned long>((seconds % 3600) / 60));
    } else {
        snprintf(out, outSize, "%lum%02lus", static_cast<unsigned long>(seconds / 60),
            static_cast<unsigned long>(seconds % 60));
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
    double quarantineMb = 0.0, quarantineLimitMb = 0.0, objectMb = 0.0, objectLimitMb = 0.0;
    unsigned long quarantineBlocks = 0, quarantineDoubleFrees = 0, quarantineOldest = 0, quarantineInvalid = 0, quarantineProblems = 0, quarantineOverruns = 0;
    if (GetQuarantineSnapshot(&quarantineMb, &quarantineLimitMb, &objectMb, &objectLimitMb, &quarantineBlocks,
            &quarantineDoubleFrees, &quarantineOldest, &quarantineInvalid, &quarantineProblems, &quarantineOverruns)) {
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity, g_heapFreeQuarantinePoison,
            "Quarantine=%.0f/%.0fMB objects=%.0f/%.0fMB held=%luk oldest=%lus%s",
            quarantineMb, quarantineLimitMb, objectMb, objectLimitMb, quarantineBlocks / 1000, quarantineOldest,
            g_heapFreeQuarantinePoison ? " POISON" : "");
        AddOverlayLine(content.bottom, &content.bottomCount, bottomCapacity,
            quarantineDoubleFrees > 0 || quarantineInvalid > 0 || quarantineProblems > 0,
            "double-frees=%lu invalid-frees=%lu heap-problems=%lu overruns-absorbed=%lu",
            quarantineDoubleFrees, quarantineInvalid, quarantineProblems, quarantineOverruns);
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
static LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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
        if (g_profilerVisible && g_profilerWindow) {
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
static DWORD WINAPI OverlayThread(LPVOID parameter) {
    LabelCurrentThread(L"um.dll: Overlay");
    HMODULE module = reinterpret_cast<HMODULE>(parameter);
    g_overlayStartTickMs = GetTickCount64();
    g_overlayFpsLastTickMs = g_overlayStartTickMs;

    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = "UMOverlayWindowClass";
    if (!RegisterClassA(&windowClass)) {
        LogLine("ERROR", "RegisterClassA for overlay failed, error=%lu", GetLastError());
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
        LogLine("ERROR", "CreateWindowExA for overlay failed, error=%lu", GetLastError());
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
            LogLine("WARN", "CreateWindowExA for overlay log panel failed, error=%lu", GetLastError());
        }
    }

    {
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
    LogLine("INFO", "Overlay window created; toggle_key=0x%02X log_toggle_key=0x%02X position=%s log_enabled=%s layered=%s",
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

// Perform configuration, diagnostics, hooks, and ASI validation after the
// loader lock is released. Keeping this work out of DllMain avoids deadlocks.
static DWORD WINAPI InitializeDllThread(LPVOID parameter) {
    EnsureThreadDescriptionFunctionsResolved();
    LabelCurrentThread(L"um.dll: Init");
    HMODULE hModule = reinterpret_cast<HMODULE>(parameter);
    InitializeCriticalSection(&g_logLock);
    g_logLockInitialized = true;

    char dllPath[MAX_PATH] = {};
    if (GetModuleFileNameA(hModule, dllPath, MAX_PATH) != 0) {
        LoadConfigFile(dllPath);
    }

    // Environment variables override whatever um.cfg set, using each
    // setting's declared type and (for Bool only) the OR-merge semantics
    // documented on ApplySettingEnvOverride.
    for (size_t i = 0; i < kSettingCount; ++i) {
        ApplySettingEnvOverride(kSettings[i]);
    }

    if (g_suppressErrorDialogs) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    PrepareLogFile();
    LogLine("INFO", "Universal Mod DLL attached; version=%s asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s file_io_logging=%s clear_log_on_start=%s suppress_error_dialogs=%s mob_validation=%s heap_free_quarantine=%s heap_alloc_padding=%d overlay=%s",
        UM_VERSION,
        g_enableAsiCheck ? "enabled" : "disabled",
        g_enableKeyboardRewrites ? "enabled" : "disabled",
        g_enableKeyboardRewriteLogging ? "enabled" : "disabled",
        g_enableCrashLogging ? "enabled" : "disabled",
        g_enableFileIoLogging ? "enabled" : "disabled",
        g_clearLogOnStart ? "enabled" : "disabled",
        g_suppressErrorDialogs ? "enabled" : "disabled",
        g_enableMobValidation ? "enabled" : "disabled",
        g_enableHeapFreeQuarantine ? "enabled" : "disabled",
        g_heapAllocPadding,
        g_enableOverlay ? "enabled" : "disabled");
    LogSystemInformation();

    HANDLE threadHandle = CreateThread(NULL, 0, KeyPopupThread, hModule, 0, NULL);
    if (threadHandle) {
        CloseHandle(threadHandle);
    }

    if (g_enableOverlay) {
        HANDLE overlayThreadHandle = CreateThread(NULL, 0, OverlayThread, hModule, 0, NULL);
        if (overlayThreadHandle) {
            CloseHandle(overlayThreadHandle);
        }
    }

    InitializeCriticalSection(&g_fileHandleLock);
    g_fileHandleLockInitialized = true;
    const bool heapHooksWanted = g_enableHeapFreeQuarantine || g_heapAllocPadding > 0;
    if (g_enableFileIoLogging || g_enableMobValidation || g_enableOverlay || heapHooksWanted) {
        InstallFileIoHooks();
    }
    if (heapHooksWanted) {
        InstallHeapHooks();
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
        return TRUE;
    }

    // compute the directory of the executable by removing the last
    // backslash and filename from the full path
    char gameDir[MAX_PATH] = {};
    size_t exePathLen = strlen(exePath);
    if (exePathLen >= sizeof(gameDir)) {
        exePathLen = sizeof(gameDir) - 1;
    }
    memcpy(gameDir, exePath, exePathLen);
    gameDir[exePathLen] = '\0';
    size_t lastSlash = strlen(gameDir);
    while (lastSlash > 0 && gameDir[lastSlash - 1] != '\\' && gameDir[lastSlash - 1] != '/') {
        --lastSlash;
    }
    if (lastSlash > 0) {
        gameDir[lastSlash] = '\0';
    }

    if (g_enableAsiCheck) {
        char asiPath[MAX_PATH] = {};
        snprintf(asiPath, sizeof(asiPath), "%s\\SpellAddonX.asi", gameDir);
        if (GetFileAttributesA(asiPath) == INVALID_FILE_ATTRIBUTES) {
            LogLine("ERROR", "Required file missing: %s, error=%lu", asiPath, GetLastError());
            MessageBoxA(NULL,
                "SpellAddonX.asi is missing and required to run the game.",
                "Missing Required File",
                MB_ICONERROR | MB_OK);
            ExitProcess(1);
        }
    }

    return 0;
}

// DLL entry point: perform only loader-safe setup and defer real initialization.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    g_dllModule = hModule;
    DisableThreadLibraryCalls(hModule);
    HANDLE threadHandle = CreateThread(NULL, 0, InitializeDllThread, hModule, 0, NULL);
    if (threadHandle) {
        CloseHandle(threadHandle);
    }
    return TRUE;
}
