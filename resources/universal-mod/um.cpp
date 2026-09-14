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
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <algorithm>

// The DLL is injected into the game process, so these flags and hooks are
// process-local. Configuration is loaded once during DLL_PROCESS_ATTACH.
static HHOOK g_keyboardHook = NULL;
// Set once in DllMain; needed by ReloadConfiguration() to re-resolve um.cfg's
// path from a thread other than the one DllMain itself ran on.
static HMODULE g_dllModule = NULL;
static BYTE g_reloadConfigKey = VK_F11;
static bool g_enableAsiCheck = true;
static bool g_enableKeyboardRewrites = true;
static bool g_enableKeyboardRewriteLogging = false;
static bool g_enableCrashLogging = true;
static bool g_enableAntiCrash = true;
static bool g_clearLogOnStart = true;
static bool g_enableCrashDumps = true;
static char g_logPath[MAX_PATH] = {};
static CRITICAL_SECTION g_logLock;
static bool g_logLockInitialized = false;
static PVOID g_vectoredExceptionHandler = NULL;
static volatile LONG g_firstChanceExceptionCount = 0;
static HANDLE g_logClearMutex = NULL;
static volatile LONG g_crashLogInProgress = 0;
static volatile LONG g_errorBlockNumber = 0;
static bool g_keyboardRewriteKeyDown[256] = {};
static bool g_enableFileIoLogging = false;
static char g_fileIoLoggingFilter[256] = {};
static bool g_enableMobValidation = false;
static bool g_enableHeapTermination = false;
static bool g_enableOverlay = true;
static BYTE g_overlayToggleKey = VK_F9;
static volatile LONG g_overlayVisible = 0;
static BYTE g_overlayLogToggleKey = VK_F10;
static volatile LONG g_overlayLogVisible = 0;
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
static int g_overlayRefreshMs = 500;
static const int OVERLAY_FPS_MARKS_MAX = 16;
static double g_overlayFpsMarks[OVERLAY_FPS_MARKS_MAX] = {30.0, 60.0, 75.0, 120.0, 140.0, 165.0, 240.0};
static int g_overlayFpsMarkCount = 7;
static COLORREF g_overlayBackgroundColor = RGB(0, 0, 0);
static int g_overlayBackgroundOpacityPercent = 20;

// Real-time mirror of the last few formatted um.log lines, appended to by
// LogLine() itself; the overlay's log panel reads this instead of the file.
static const int OVERLAY_LOG_CAPACITY = 50;
static char g_overlayLogRing[OVERLAY_LOG_CAPACITY][300] = {};
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

// -- Performance tweaks (opt-in; off by default; independent of each other) --
static bool g_enablePerformancePriority = false;
static bool g_enablePerformanceAffinity = false;
static char g_performancePriorityClass[16] = "high";
static char g_performanceAffinityMaskHex[32] = {};

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
static const DWORD UM_STATUS_HEAP_CORRUPTION = 0xC0000374L;

typedef HANDLE (WINAPI *CreateFileAFunction)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFunction)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFileFunction)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WriteFileFunction)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandleFunction)(HANDLE);
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
static DirectDrawCreateFunction g_originalDirectDrawCreate = NULL;
static DirectDrawCreateExFunction g_originalDirectDrawCreateEx = NULL;

// Forward declaration: defined later, but ApplyPerformanceTweaks() (defined
// earlier, alongside the other config-value Parse* helpers) needs it.
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

// Read a boolean override from the process environment.
static bool GetEnvironmentFlag(LPCSTR name) {
    const char* value = getenv(name);
    if (!value) {
        return false;
    }
    return IsTrueString(value);
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

// Parse OVERLAY_REFRESH_MS, clamped to a sane range; falls back to 500 for
// empty/unrecognized values (100ms floor avoids excessive repaint overhead).
static int ParseOverlayRefreshMs(const char* value) {
    if (!value || value[0] == '\0') {
        return 500;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < 100 || parsed > 5000) {
        return 500;
    }
    return static_cast<int>(parsed);
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

// Parse OVERLAY_BACKGROUND_OPACITY as a percentage, clamped to 0-100.
static int ParseOverlayOpacityPercent(const char* value) {
    if (!value || value[0] == '\0') {
        return 20;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return 20;
    }
    if (parsed < 0) {
        parsed = 0;
    } else if (parsed > 100) {
        parsed = 100;
    }
    return static_cast<int>(parsed);
}

// Parse OVERLAY_LOG_LINES, clamped to [1, OVERLAY_LOG_CAPACITY].
static int ParseOverlayLogLineCount(const char* value) {
    if (!value || value[0] == '\0') {
        return 10;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < 1) {
        return 10;
    }
    if (parsed > OVERLAY_LOG_CAPACITY) {
        parsed = OVERLAY_LOG_CAPACITY;
    }
    return static_cast<int>(parsed);
}

// Parse OVERLAY_LOG_WIDTH (pixels), clamped to a sane range.
static int ParseOverlayLogWidth(const char* value) {
    if (!value || value[0] == '\0') {
        return 900;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < 300) {
        return 900;
    }
    if (parsed > 2000) {
        parsed = 2000;
    }
    return static_cast<int>(parsed);
}

// Parse OVERLAY_THREAD_COUNT, clamped to [1, OVERLAY_THREAD_DISPLAY_MAX].
static int ParseOverlayThreadCount(const char* value) {
    if (!value || value[0] == '\0') {
        return 8;
    }
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < 1) {
        return 8;
    }
    if (parsed > OVERLAY_THREAD_DISPLAY_MAX) {
        parsed = OVERLAY_THREAD_DISPLAY_MAX;
    }
    return static_cast<int>(parsed);
}

// Map a PERFORMANCE_PRIORITY_CLASS config value to a Win32 priority class.
// Falls back to NORMAL_PRIORITY_CLASS for empty or unrecognized values.
static DWORD ParsePriorityClassName(const char* value) {
    if (!value || value[0] == '\0') {
        return NORMAL_PRIORITY_CLASS;
    }
    if (EqualsIgnoreCase(value, "realtime")) {
        return REALTIME_PRIORITY_CLASS;
    }
    if (EqualsIgnoreCase(value, "high")) {
        return HIGH_PRIORITY_CLASS;
    }
    if (EqualsIgnoreCase(value, "abovenormal")) {
        return ABOVE_NORMAL_PRIORITY_CLASS;
    }
    if (EqualsIgnoreCase(value, "belownormal")) {
        return BELOW_NORMAL_PRIORITY_CLASS;
    }
    if (EqualsIgnoreCase(value, "idle")) {
        return IDLE_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;
}

// The inverse of ParsePriorityClassName, used to show the *actual* live
// priority class on the overlay so the user can confirm the tweak really
// took effect rather than trusting the enabled flag alone.
static const char* PriorityClassToName(DWORD priorityClass) {
    switch (priorityClass) {
    case REALTIME_PRIORITY_CLASS: return "realtime";
    case HIGH_PRIORITY_CLASS: return "high";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "abovenormal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case BELOW_NORMAL_PRIORITY_CLASS: return "belownormal";
    case IDLE_PRIORITY_CLASS: return "idle";
    default: return "unknown";
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

// Applies the configured process priority class and/or CPU affinity mask;
// each is independently opt-in and off by default. Forcing high/realtime
// priority on a game that isn't designed for it, or pinning it to too few
// cores, can hurt instead of help - test before leaving either one enabled.
static void ApplyPerformanceTweaks() {
    if (g_enablePerformancePriority) {
        DWORD requestedClass = ParsePriorityClassName(g_performancePriorityClass);
        if (!SetPriorityClass(GetCurrentProcess(), requestedClass)) {
            LogLine("WARN", "SetPriorityClass(%s) failed, error=%lu",
                g_performancePriorityClass, GetLastError());
        } else {
            LogLine("INFO", "Priority class applied: %s", g_performancePriorityClass);
        }
    }
    if (g_enablePerformanceAffinity && g_performanceAffinityMaskHex[0] != '\0') {
        char* end = NULL;
        unsigned long mask = strtoul(g_performanceAffinityMaskHex, &end, 16);
        if (end != g_performanceAffinityMaskHex && mask != 0) {
            if (!SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) {
                LogLine("WARN", "SetProcessAffinityMask(0x%lX) failed, error=%lu", mask, GetLastError());
            } else {
                LogLine("INFO", "CPU affinity mask applied: %s", g_performanceAffinityMaskHex);
            }
        } else {
            LogLine("WARN", "PERFORMANCE_AFFINITY_MASK=\"%s\" is not a valid hex mask; affinity left unchanged",
                g_performanceAffinityMaskHex);
        }
    }
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
    if (!g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    FILE* file = fopen(g_logPath, "a");
// Write file-I/O diagnostics through LogLine when that feature is enabled.
    if (!file) {
        return;
    }

    bool logLockAcquired = !g_logLockInitialized ||
        TryEnterCriticalSection(&g_logLock) != FALSE;

    const char* outputLevel = level;
    const char* category = NULL;
    if (EqualsIgnoreCase(level, "SYSINFO")) {
        outputLevel = "SYSINFO";
    } else if (EqualsIgnoreCase(level, "ANTICRASH")) {
        outputLevel = "DEBUG";
        category = "ANTICRASH";
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

    char message[400] = {};
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

// Normalize and store the comma-separated file-I/O extension filter.
static void SetFileIoLoggingFilter(const char* value) {
    SetQuotedConfigString(g_fileIoLoggingFilter, sizeof(g_fileIoLoggingFilter), value);
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
        LogLine("ANTICRASH", "Open tracked file path=%s handle=%p", paths[i], handles[i]);
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
// databaseadb.res) are themselves .res archives, and their internal record
// layout is undocumented and only partly plain text. Rather than parse that
// container/record format, every printable-ASCII run of 3+ characters is
// collected as a candidate valid name (lowercased, and also with its first
// byte dropped to tolerate a stray length/tag byte from the preceding binary
// field bleeding into the run). This was verified against the mod's real
// database files and the entire map corpus shipped with the mod: it produced
// zero false positives while still catching every deliberately-invalid
// weapon/armor/spell/quest/quick-item name used to test this feature.
static void ExtractDatabaseNames(const BYTE* data, size_t size, std::unordered_set<std::string>& names) {
    size_t i = 0;
    while (i < size) {
        if (data[i] < 0x20 || data[i] > 0x7E) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < size && data[i] >= 0x20 && data[i] <= 0x7E) {
            ++i;
        }
        size_t length = i - start;
        if (length >= 3) {
            std::string run(reinterpret_cast<const char*>(data + start), length);
            for (size_t j = 0; j < run.size(); ++j) {
                run[j] = static_cast<char>(tolower(static_cast<unsigned char>(run[j])));
            }
            names.insert(run);
            names.insert(run.substr(1));
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
static void ValidateMobFile(const char* path) {
    if (!g_enableMobValidation || !path || !HasFileExtension(path, "mob")) {
        return;
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
    if (!objectSectionSeen) {
        LogLine("DEBUG", "[MOBCHECK] %s has no OBJECT_SECTION node (placement-only or menu mob?)", path);
    } else if (!hasError) {
        LogLine("DEBUG", "[MOBCHECK] %s validated OK", path);
    }
    free(buffer);
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
            LogFileIo("File read path=%s bytes=%lu handle=%p", path,
                bytesRead ? *bytesRead : 0, file);
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
            LogFileIo("File written path=%s bytes=%lu handle=%p", path,
                bytesWritten ? *bytesWritten : 0, file);
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
    }
    g_overlayLastCountedFrameTime = now;
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
static void LogSystemInformation() {
    LogLine("SYSINFO", "============= SYSTEM INFORMATION =============");
    LogOperatingSystemInformation();
    LogHardwareInformation();
    LogGraphicsInformation();
    LogLine("SYSINFO", "==============================================");
}

// Classify exception codes that should not be resumed blindly.
static bool IsUnsafeExceptionToResume(const EXCEPTION_RECORD* record) {
    if (!record) {
        return true;
    }

    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_GUARD_PAGE:
    case UM_STATUS_HEAP_CORRUPTION:
        return true;
    default:
        return false;
    }
}

// Return a human-readable explanation for the exception classification above.
static const char* GetUnsafeExceptionReason(const EXCEPTION_RECORD* record) {
    if (!record) {
        return "missing exception record";
    }

    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation: invalid memory read or write";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error: required memory could not be loaded";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction: CPU could not execute the instruction";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "non-continuable exception: Windows forbids resuming execution";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow: the thread has exhausted its stack";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "datatype misalignment: an improperly aligned memory access occurred";
    case EXCEPTION_GUARD_PAGE:
        return "guard-page violation: protected memory was accessed";
    case UM_STATUS_HEAP_CORRUPTION:
        return "heap corruption: the process heap manager detected corrupted allocator metadata";
    default:
        return "exception is classified as unsafe to resume";
    }
}

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
    case UM_STATUS_HEAP_CORRUPTION:
        return "STATUS_HEAP_CORRUPTION";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

// Capture first-chance exceptions before the game or another handler can
// consume them. This handler only logs and always defers; it never edits CPU
// state or attempts unsafe recovery. The cap prevents exception loops from
// flooding the log before the process exits.
static LONG WINAPI VectoredLoggingHandler(EXCEPTION_POINTERS* exceptionInfo) {
    EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : NULL;
    if (!record || !IsUnsafeExceptionToResume(record)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG count = InterlockedIncrement(&g_firstChanceExceptionCount);
    if (count <= 32) {
        LogLine("ANTICRASH", "First-chance exception code=0x%08lX address=%p case=%s count=%ld",
            record->ExceptionCode, record->ExceptionAddress,
            GetExceptionCaseName(record), count);
    } else if (count == 33) {
        LogLine("ANTICRASH", "Further first-chance exceptions suppressed after 32 entries");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Log the exception, register state, faulting module, stack, and tracked files.
// This handler deliberately does not modify the faulting context: generic stack
// surgery cannot safely skip a failed DirectDraw/texture operation.
static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (!g_enableCrashLogging) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_crashLogInProgress, 1, 0) != 0) {
// Load documented settings from um.cfg and ignore unknown/malformed entries.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : NULL;
    CONTEXT* context = exceptionInfo ? exceptionInfo->ContextRecord : NULL;

    LogErrorBlockStart();

    if (!record) {
        LogLine("FATAL", "Unhandled exception had no exception record");
    } else {
        LogLine("FATAL", "Unhandled exception code=0x%08lX flags=0x%08lX address=%p parameters=%lu",
            record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress,
            record->NumberParameters);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            LogLine("FATAL", "Access violation type=%s address=%p",
                record->ExceptionInformation[0] == 0 ? "read" : "write",
                reinterpret_cast<void*>(static_cast<ULONG_PTR>(record->ExceptionInformation[1])));
        }
    }

    if (g_enableAntiCrash && IsUnsafeExceptionToResume(record)) {
        LogLine("ANTICRASH", "Recovery refused: case=%s reason=%s",
            GetExceptionCaseName(record), GetUnsafeExceptionReason(record));
        LogLine("ANTICRASH", "Normal Windows crash handling will continue to prevent silent process corruption");
    }

    if (context) {
#if defined(_M_IX86) || defined(__i386__)
        LogLine("FATAL", "CPU registers eax=0x%08lX ebx=0x%08lX ecx=0x%08lX edx=0x%08lX esi=0x%08lX edi=0x%08lX ebp=0x%08lX esp=0x%08lX eip=0x%08lX",
            context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi,
            context->Edi, context->Ebp, context->Esp, context->Eip);
        if (context->Eip == 0x90909090 || context->Eip == 0xCCCCCCCC || context->Eip == 0xCDCDCDCD) {
            LogLine("ANTICRASH", "Instruction pointer is a debug fill or NOP-sled sentinel (0x%08lX); original control flow cannot be reconstructed safely",
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

    if (g_enableAntiCrash && record && IsUnsafeExceptionToResume(record)) {
        LogLine("ANTICRASH", "Unsafe exception cannot be resumed safely; normal Windows crash handling will continue");
    }
    LogLine("FATAL", "The process will continue with normal Windows crash handling");
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
        configPath[lastSlash] = '\0';
    }
    memcpy(g_logPath, configPath, strlen(configPath) + 1);
    strncat(g_logPath, "\\um.log", sizeof(g_logPath) - strlen(g_logPath) - 1);
    strncat(configPath, "\\um.cfg", sizeof(configPath) - strlen(configPath) - 1);

    FILE* file = fopen(configPath, "r");
    if (!file) {
        // Create um.cfg if it doesn't exist with default settings
        file = fopen(configPath, "w");
        if (file) {
            fprintf(file, "; Universal Mod Configuration\n\n");

            fprintf(file, "; Require SpellAddonX.asi beside game.exe; (true/false)\n");
            fprintf(file, "SPELLADDON_ASI_CHECK=true\n\n");

            fprintf(file, "; -- Keyboard --\n");
            fprintf(file, "; Rewrite backtick and number-row input as US-QWERTY keys; (true/false)\n");
            fprintf(file, "KEYBOARD_REWRITES=true\n");
            fprintf(file, "; Log keyboard rewrite events; (true/false)\n");
            fprintf(file, "KEYBOARD_REWRITES_LOGGING=false\n");
            fprintf(file, "; Key that reloads um.cfg and applies it immediately, without restarting the game;\n");
            fprintf(file, "; some settings (installing hooks, creating the overlay window for the first time)\n");
            fprintf(file, "; still require a restart; F1-F12, or a 0x.. / decimal virtual-key code.\n");
            fprintf(file, "RELOAD_CONFIG_KEY=F11\n\n");

            fprintf(file, "; -- Logging --\n");
            fprintf(file, "; Write diagnostic and crash information to um.log; (true/false)\n");
            fprintf(file, "LOGGING=true\n");
            fprintf(file, "; Log file opens, reads, and writes as INFO entries; (true/false)\n");
            fprintf(file, "FILE_IO_LOGGING=false\n");
            fprintf(file, "; Comma-separated file extensions to exclude from file-I/O logging; empty or like mmp,res.\n");
            fprintf(file, "FILE_IO_LOGGING_FILTER=\"\"\n");
            fprintf(file, "; Clear um.log on the first DLL instance of a launch; (true/false)\n");
            fprintf(file, "CLEAR_LOG_ON_START=true\n\n");

            fprintf(file, "; -- Crash handling --\n");
            fprintf(file, "; Suppress critical-error dialogs; unsafe exceptions still crash normally; (true/false)\n");
            fprintf(file, "ANTICRASH=true\n");
            fprintf(file, "; Write portable Windows minidumps beside um.log; (true/false)\n");
            fprintf(file, "CRASH_DUMPS=true\n");
            fprintf(file, "; Validate .mob file headers when opened and log structural problems, including a\n");
            fprintf(file, "; cross-check of unit weapons/armors/spells/quest/quick items against the item/spell database; (true/false)\n");
            fprintf(file, "MOB_VALIDATION=false\n");
            fprintf(file, "; Fail fast the instant Windows detects heap corruption instead of letting the\n");
            fprintf(file, "; process keep running on corrupted memory until an unrelated later crash; the\n");
            fprintf(file, "; resulting crash log points much closer to the real cause; (true/false)\n");
            fprintf(file, "HEAP_CORRUPTION_TERMINATION=true\n\n");

            fprintf(file, "; -- Performance --\n");
            fprintf(file, "; These two settings are independent of each other - enabling one does not\n");
            fprintf(file, "; enable the other.\n");
            fprintf(file, ";\n");
            fprintf(file, "; Apply a custom process priority class below; off by default, since forcing\n");
            fprintf(file, "; high/realtime priority on a game not designed for it can cause audio/input\n");
            fprintf(file, "; stutter instead of helping; (true/false)\n");
            fprintf(file, "PERFORMANCE_PRIORITY_ENABLED=false\n");
            fprintf(file, "; Priority class to apply when the setting above is enabled; one of idle,\n");
            fprintf(file, "; belownormal, normal, abovenormal, high, realtime.\n");
            fprintf(file, "PERFORMANCE_PRIORITY_CLASS=high\n");
            fprintf(file, "; Apply the CPU affinity mask below; off by default, since restricting the\n");
            fprintf(file, "; game to too few cores can make performance WORSE, not better - test before\n");
            fprintf(file, "; leaving this on; (true/false)\n");
            fprintf(file, "PERFORMANCE_AFFINITY_ENABLED=false\n");
            fprintf(file, "; Restricts which CPU cores the game is allowed to run on when the setting\n");
            fprintf(file, "; above is enabled. This does NOT make the game slower or single-threaded by\n");
            fprintf(file, "; itself - it can help an old, mostly single-threaded game like this one, by\n");
            fprintf(file, "; stopping Windows/Wine from constantly bouncing its one busy thread between\n");
            fprintf(file, "; different cores - but restricting to too few cores can backfire.\n");
            fprintf(file, ";\n");
            fprintf(file, "; You do not need to understand binary/hex to use this - just copy one of these\n");
            fprintf(file, "; common values (check Task Manager/Windows or `nproc`/System Monitor on Linux\n");
            fprintf(file, "; first to see how many cores you actually have, and don't pick too few):\n");
            fprintf(file, ";   0x3  = cores 1-2\n");
            fprintf(file, ";   0xF  = cores 1-4\n");
            fprintf(file, ";   0x3F = cores 1-6\n");
            fprintf(file, ";   0xFF = cores 1-8\n");
            fprintf(file, "PERFORMANCE_AFFINITY_MASK=\"\"\n\n");

            fprintf(file, "; -- Overlay --\n");
            fprintf(file, "; Enable a toggleable diagnostic overlay drawn on top of the game window; (true/false)\n");
            fprintf(file, "OVERLAY_ENABLED=true\n");
            fprintf(file, "; Key that shows/hides the main overlay panel while the game has focus; F1-F12,\n");
            fprintf(file, "; or a 0x.. / decimal virtual-key code.\n");
            fprintf(file, "OVERLAY_TOGGLE_KEY=F9\n");
            fprintf(file, "; Corner/edge of the game window the overlay is anchored to; one of\n");
            fprintf(file, "; top-left, top-right, bottom-left, bottom-right, top, bottom, left, right, center.\n");
            fprintf(file, "OVERLAY_POSITION=top-left\n");
            fprintf(file, "; Overlay text color as a hex RRGGBB value (no # needed).\n");
            fprintf(file, "OVERLAY_COLOR=00FF00\n");
            fprintf(file, "; How often the overlay repaints and samples FPS/resources, in milliseconds (100-5000).\n");
            fprintf(file, "OVERLAY_REFRESH_MS=500\n");
            fprintf(file, "; Show the FPS sparkline graph (frametime stays as text only); (true/false)\n");
            fprintf(file, "OVERLAY_SHOW_FPS_GRAPH=true\n");
            fprintf(file, "; Comma-separated static FPS reference marks for the graph, ascending; the lowest\n");
            fprintf(file, "; two always show, the rest only appear once the game actually reaches them.\n");
            fprintf(file, "OVERLAY_FPS_MARKS=30,60,75,120,140,165,240\n");
            fprintf(file, "; Show CPU%%/memory/thread-count usage of game.exe; (true/false)\n");
            fprintf(file, "OVERLAY_SHOW_RESOURCES=true\n");
            fprintf(file, "; Show which DirectDraw driver is actually rendering (native, dgVoodoo2, DXVK, etc.); (true/false)\n");
            fprintf(file, "OVERLAY_SHOW_BACKEND=true\n");
            fprintf(file, "; Show a per-thread CPU%% breakdown below the FPS graph (lowest %d thread IDs,\n", g_overlayThreadDisplayCount);
            fprintf(file, "; oldest/main thread first and stable across samples, rather than resorted by\n");
            fprintf(file, "; CPU%% each tick); thread names are usually \"(unnamed)\" since this game predates\n");
            fprintf(file, "; thread naming APIs - only um.dll's own threads are named; (true/false)\n");
            fprintf(file, "OVERLAY_SHOW_THREADS=true\n");
            fprintf(file, "; How many threads to list (lowest thread IDs first); max %d.\n", OVERLAY_THREAD_DISPLAY_MAX);
            fprintf(file, "OVERLAY_THREAD_COUNT=%d\n", g_overlayThreadDisplayCount);
            fprintf(file, "; Panel background color as a hex RRGGBB value (no # needed).\n");
            fprintf(file, "OVERLAY_BACKGROUND_COLOR=000000\n");
            fprintf(file, "; Panel background opacity as a percentage (0=fully transparent, 100=solid).\n");
            fprintf(file, "; NOTE: 100 renders the panel WITHOUT a layered window at all (plain BitBlt);\n");
            fprintf(file, "; any value below 100 re-enables a layered window, which on some Wine/Wayland\n");
            fprintf(file, "; setups forces the game out of direct-scanout presentation and causes a large\n");
            fprintf(file, "; GPU/FPS regression - confirmed to happen with BOTH transparency styles below,\n");
            fprintf(file, "; not just smooth alpha blending; 100 is the only performance-safe value there.\n");
            fprintf(file, "OVERLAY_BACKGROUND_OPACITY=100\n");
            fprintf(file, "; How the background opacity above is achieved when below 100; \"alpha\" blends\n");
            fprintf(file, "; smoothly (some Wine/Wayland setups don't honor this and render fully opaque\n");
            fprintf(file, "; instead); \"dither\" approximates it with alternating fully-opaque/fully-\n");
            fprintf(file, "; transparent scanline bands, which still works when smooth per-pixel alpha\n");
            fprintf(file, "; blending does not; one of alpha, dither.\n");
            fprintf(file, "OVERLAY_TRANSPARENCY_STYLE=alpha\n\n");
            fprintf(file, "; -- Overlay log panel --\n");
            fprintf(file, "; Show a separate auto-scrolling panel with the last few um.log lines; (true/false)\n");
            fprintf(file, "OVERLAY_LOG_ENABLED=true\n");
            fprintf(file, "; Key that shows/hides the log panel independently of the main overlay panel;\n");
            fprintf(file, "; F1-F12, or a 0x.. / decimal virtual-key code.\n");
            fprintf(file, "OVERLAY_LOG_TOGGLE_KEY=F10\n");
            fprintf(file, "; Number of most recent log lines to display, newest at the bottom (max %d).\n", OVERLAY_LOG_CAPACITY);
            fprintf(file, "OVERLAY_LOG_LINES=10\n");
            fprintf(file, "; Log panel width in pixels (300-2000); widen this if long lines still get\n");
            fprintf(file, "; wrapped/clipped too aggressively.\n");
            fprintf(file, "OVERLAY_LOG_WIDTH=900\n");
            fprintf(file, "; Wrap log lines that are too long to fit within the panel width onto extra\n");
            fprintf(file, "; visual rows instead of clipping them; counts against OVERLAY_LOG_LINES above; (true/false)\n");
            fprintf(file, "OVERLAY_LOG_WRAP=true\n");
            fprintf(file, "; Corner/edge of the game window the log panel is anchored to; one of\n");
            fprintf(file, "; top-left, top-right, bottom-left, bottom-right, top, bottom, left, right, center.\n");
            fprintf(file, "OVERLAY_LOG_POSITION=bottom-left\n");
            fprintf(file, "; Comma-separated log levels to hide from the live log panel; matches the [LEVEL]\n");
            fprintf(file, "; shown in um.log (SYSINFO, INFO, WARN, ERROR, FATAL, DEBUG); case-insensitive,\n");
            fprintf(file, "; um.log on disk always keeps every level regardless of this filter.\n");
            fprintf(file, "OVERLAY_LOG_LEVEL_FILTER=\"SYSINFO\"\n\n");
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

        // parse the setting
        if (EqualsIgnoreCase(key, "SPELLADDON_ASI_CHECK")) {
            g_enableAsiCheck = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "KEYBOARD_REWRITES")) {
            g_enableKeyboardRewrites = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "KEYBOARD_REWRITES_LOGGING")) {
            g_enableKeyboardRewriteLogging = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "RELOAD_CONFIG_KEY")) {
            g_reloadConfigKey = ParseVirtualKeyName(value, VK_F11);
        } else if (EqualsIgnoreCase(key, "LOGGING")) {
            g_enableCrashLogging = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "FILE_IO_LOGGING")) {
            g_enableFileIoLogging = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "FILE_IO_LOGGING_FILTER")) {
            SetFileIoLoggingFilter(value);
        } else if (EqualsIgnoreCase(key, "ANTICRASH")) {
            g_enableAntiCrash = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "CRASH_DUMPS")) {
            g_enableCrashDumps = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "CLEAR_LOG_ON_START")) {
            g_clearLogOnStart = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "MOB_VALIDATION")) {
            g_enableMobValidation = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "HEAP_CORRUPTION_TERMINATION")) {
            g_enableHeapTermination = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "PERFORMANCE_PRIORITY_ENABLED")) {
            g_enablePerformancePriority = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "PERFORMANCE_PRIORITY_CLASS")) {
            SetQuotedConfigString(g_performancePriorityClass, sizeof(g_performancePriorityClass), value);
        } else if (EqualsIgnoreCase(key, "PERFORMANCE_AFFINITY_ENABLED")) {
            g_enablePerformanceAffinity = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "PERFORMANCE_AFFINITY_MASK")) {
            SetQuotedConfigString(g_performanceAffinityMaskHex, sizeof(g_performanceAffinityMaskHex), value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_ENABLED")) {
            g_enableOverlay = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_TOGGLE_KEY")) {
            g_overlayToggleKey = ParseVirtualKeyName(value, VK_F9);
        } else if (EqualsIgnoreCase(key, "OVERLAY_POSITION")) {
            ParseOverlayPosition(value, g_overlayPosition, sizeof(g_overlayPosition));
        } else if (EqualsIgnoreCase(key, "OVERLAY_COLOR")) {
            g_overlayTextColor = ParseHexColor(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_REFRESH_MS")) {
            g_overlayRefreshMs = ParseOverlayRefreshMs(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_SHOW_FPS_GRAPH")) {
            g_overlayShowFpsGraph = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_FPS_MARKS")) {
            ParseFpsMarks(value, g_overlayFpsMarks, &g_overlayFpsMarkCount);
        } else if (EqualsIgnoreCase(key, "OVERLAY_SHOW_RESOURCES")) {
            g_overlayShowResources = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_SHOW_BACKEND")) {
            g_overlayShowBackend = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_SHOW_THREADS")) {
            g_overlayShowThreads = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_THREAD_COUNT")) {
            g_overlayThreadDisplayCount = ParseOverlayThreadCount(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_BACKGROUND_COLOR")) {
            g_overlayBackgroundColor = ParseHexColor(value, RGB(0, 0, 0));
        } else if (EqualsIgnoreCase(key, "OVERLAY_BACKGROUND_OPACITY")) {
            g_overlayBackgroundOpacityPercent = ParseOverlayOpacityPercent(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_TRANSPARENCY_STYLE")) {
            SetQuotedConfigString(g_overlayTransparencyStyle, sizeof(g_overlayTransparencyStyle), value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_ENABLED")) {
            g_overlayLogEnabled = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_TOGGLE_KEY")) {
            g_overlayLogToggleKey = ParseVirtualKeyName(value, VK_F10);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_LINES")) {
            g_overlayLogLineCount = ParseOverlayLogLineCount(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_WIDTH")) {
            g_overlayLogPanelWidth = ParseOverlayLogWidth(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_WRAP")) {
            g_overlayLogWrapEnabled = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_POSITION")) {
            ParseOverlayPosition(value, g_overlayLogPosition, sizeof(g_overlayLogPosition));
        } else if (EqualsIgnoreCase(key, "OVERLAY_LOG_LEVEL_FILTER")) {
            SetQuotedConfigString(g_overlayLogLevelFilter, sizeof(g_overlayLogLevelFilter), value);
        }
    }

    fclose(file);
}

// Re-reads um.cfg and re-applies whatever can safely take effect while the
// game is already running (most flags, overlay settings, priority/affinity).
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
    ApplyPerformanceTweaks();
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
static void CompositeCanvasToWindow(HWND hwnd, const AlphaCanvas& canvas, int x, int y) {
    if (!hwnd || !canvas.dc) {
        return;
    }
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

// Panel height depends on whether the FPS graph and optional diagnostic lines
// are enabled in um.cfg, so it is computed rather than a fixed constant.
// Layout, top to bottom: title/FPS block, FPS graph, then LAA/priority/
// backend block, then the optional per-thread breakdown.
static int ComputeOverlayTopLineCount() {
    int count = 2; // title, FPS/FrameTime (always shown)
    if (g_overlayShowResources) {
        ++count;
    }
    return count;
}

static int ComputeOverlayBottomLineCount() {
    int count = 2; // LAA, priority/affinity (always shown)
    if (g_overlayShowBackend) {
        ++count;
    }
    return count;
}

// Extra height for the optional per-thread CPU%% breakdown drawn below the
// FPS graph; 0 when disabled or nothing has been sampled yet.
static const int OVERLAY_THREAD_LINE_HEIGHT = 16;
// Room below the graph box for the "-Ns" / "now" time-scale labels.
static const int OVERLAY_GRAPH_TIME_SCALE_HEIGHT = 16;

static int ComputeOverlayThreadSectionHeight() {
    if (!g_overlayShowThreads || g_overlayThreadSampleCount <= 0) {
        return 0;
    }
    return OVERLAY_SECTION_GAP + 20 + g_overlayThreadSampleCount * OVERLAY_THREAD_LINE_HEIGHT;
}

static int ComputeOverlayPanelHeight() {
    // +20 once for the blank line below the title; the pre-graph offset is
    // 20 (was 40) to remove a blank line below the FPS/FrameTime line; +20
    // again for the blank line below the graph's time scale.
    int height = 8 + ComputeOverlayTopLineCount() * 20 + 20 + OVERLAY_SECTION_GAP;
    if (g_overlayShowFpsGraph) {
        height += 20 + OVERLAY_GRAPH_HEIGHT + OVERLAY_GRAPH_TIME_SCALE_HEIGHT + OVERLAY_SECTION_GAP + 20;
    }
    height += ComputeOverlayBottomLineCount() * 20;
    height += ComputeOverlayThreadSectionHeight();
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
    TextOutA(hdc, graphRect.left, graphRect.top - 20, labelText, static_cast<int>(strlen(labelText)));

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
    int panelHeight = ComputeOverlayPanelHeight();

    HFONT font = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    // Top block: title, [resources], FPS/FrameTime.
    char topLines[4][160] = {};
    int topLineCount = 0;
    strncpy(topLines[topLineCount++], "Universal Mod Overlay", sizeof(topLines[0]) - 1);
    if (g_overlayShowResources) {
        snprintf(topLines[topLineCount++], sizeof(topLines[0]), "CPU=%.1f%% Mem=%.1fMB Threads=%d",
            g_overlayCpuPercent, g_overlayWorkingSetMb, g_overlayThreadCount);
    }
    if (g_overlayFrameCounterActive) {
        snprintf(topLines[topLineCount++], sizeof(topLines[0]), "FPS=%.1f FrameTime=%.2fms",
            g_overlayCurrentFps, g_overlayCurrentFrameTimeMs);
    } else {
        strncpy(topLines[topLineCount++], "FPS=pending (waiting for primary surface)", sizeof(topLines[0]) - 1);
    }

    // Bottom block (drawn after the FPS graph): LAA, priority/affinity,
    // [backend].
    char bottomLines[3][160] = {};
    int bottomLineCount = 0;
    // Always shown: confirms whether this specific running game.exe was
    // patched with the LARGE_ADDRESS_AWARE bit (see _cpr/game-exe-laa-patch),
    // read straight from its own in-memory PE header, not assumed.
    strncpy(bottomLines[bottomLineCount++], IsCurrentProcessLargeAddressAware() ?
        "LAA=yes (>2GB address space)" : "LAA=no (capped at 2GB address space)", sizeof(bottomLines[0]) - 1);
    // Always shown (regardless of PERFORMANCE_PRIORITY_ENABLED) so the actual,
    // live process state confirms whether a requested tweak really applied,
    // rather than trusting the enabled flag alone.
    DWORD_PTR processAffinity = 0, systemAffinity = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity);
    snprintf(bottomLines[bottomLineCount++], sizeof(bottomLines[0]), "Priority=%s Affinity=0x%lX",
        PriorityClassToName(GetPriorityClass(GetCurrentProcess())), static_cast<unsigned long>(processAffinity));
    if (g_overlayShowBackend) {
        snprintf(bottomLines[bottomLineCount++], sizeof(bottomLines[0]), "DirectDraw Backend=%s", g_directDrawBackendName);
    }

    // Built separately (drawn below the LAA/priority/backend block, at the
    // very bottom), but still folded into the width measurement below so
    // long thread names/CPU%% don't get clipped either.
    char threadSectionLines[OVERLAY_THREAD_DISPLAY_MAX + 1][64] = {};
    int threadSectionLineCount = 0;
    if (g_overlayShowThreads && g_overlayThreadSampleCount > 0) {
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
    int panelWidth = OVERLAY_PANEL_WIDTH;
    HDC scratchDC = CreateCompatibleDC(NULL);
    if (scratchDC) {
        HFONT oldScratchFont = font ? static_cast<HFONT>(SelectObject(scratchDC, font)) : NULL;
        int maxTextWidth = 0;
        for (int i = 0; i < topLineCount; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, topLines[i], static_cast<int>(strlen(topLines[i])), &textSize) &&
                    textSize.cx > maxTextWidth) {
                maxTextWidth = textSize.cx;
            }
        }
        for (int i = 0; i < bottomLineCount; ++i) {
            SIZE textSize = {};
            if (GetTextExtentPoint32A(scratchDC, bottomLines[i], static_cast<int>(strlen(bottomLines[i])), &textSize) &&
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
    for (int i = 0; i < topLineCount; ++i) {
        TextOutA(hdc, 8, y, topLines[i], static_cast<int>(strlen(topLines[i])));
        y += 20;
        if (i == 0) {
            y += 20; // blank line below the title
        }
    }
    y += OVERLAY_SECTION_GAP;

    if (g_overlayShowFpsGraph) {
        y += 20; // was 40; one blank line removed here below FPS/FrameTime
        RECT fpsGraphRect = {8, y, panelWidth - 8, y + OVERLAY_GRAPH_HEIGHT};
        double timeSpanSeconds = static_cast<double>(g_overlayFpsHistoryCount) * g_overlayRefreshMs / 1000.0;
        DrawSparklineGraph(hdc, fpsGraphRect, "FPS Graph", g_overlayFpsHistory,
            g_overlayFpsHistoryNext, g_overlayFpsHistoryCount, fpsCeiling,
            g_overlayFpsMarks, fpsMarkLabelPtrs, revealedMarks, timeSpanSeconds);
        y += OVERLAY_GRAPH_HEIGHT + OVERLAY_GRAPH_TIME_SCALE_HEIGHT + OVERLAY_SECTION_GAP;
        y += 20; // blank line below the graph's time scale
    }

    for (int i = 0; i < bottomLineCount; ++i) {
        TextOutA(hdc, 8, y, bottomLines[i], static_cast<int>(strlen(bottomLines[i])));
        y += 20;
    }

    if (threadSectionLineCount > 0) {
        y += OVERLAY_SECTION_GAP; // skip a line before the thread breakdown
        for (int i = 0; i < threadSectionLineCount; ++i) {
            TextOutA(hdc, 8, y, threadSectionLines[i], static_cast<int>(strlen(threadSectionLines[i])));
            y += OVERLAY_THREAD_LINE_HEIGHT;
        }
    }

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
    char snapshot[OVERLAY_LOG_CAPACITY][sizeof(g_overlayLogRing[0])] = {};
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
    int maxCharsPerRow = 0;
    if (g_overlayLogWrapEnabled) {
        TEXTMETRICA metrics = {};
        GetTextMetricsA(hdc, &metrics);
        int charWidth = metrics.tmAveCharWidth > 0 ? metrics.tmAveCharWidth : 8;
        maxCharsPerRow = (g_overlayLogPanelWidth - 12) / charWidth;
    }

    static const int OVERLAY_LOG_MAX_VISUAL_ROWS = OVERLAY_LOG_CAPACITY * 4;
    char rows[OVERLAY_LOG_MAX_VISUAL_ROWS][160] = {};
    int rowCount = 0;
    for (int i = 0; i < snapshotCount && rowCount < OVERLAY_LOG_MAX_VISUAL_ROWS; ++i) {
        const char* text = snapshot[i];
        size_t textLen = strlen(text);
        if (maxCharsPerRow <= 0 || static_cast<int>(textLen) <= maxCharsPerRow) {
            snprintf(rows[rowCount++], sizeof(rows[0]), "%s", text);
            continue;
        }
        size_t pos = 0;
        while (pos < textLen && rowCount < OVERLAY_LOG_MAX_VISUAL_ROWS) {
            size_t remaining = textLen - pos;
            size_t chunkLen = remaining < static_cast<size_t>(maxCharsPerRow) ?
                remaining : static_cast<size_t>(maxCharsPerRow);
            size_t breakAt = chunkLen;
            if (chunkLen == static_cast<size_t>(maxCharsPerRow)) {
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
        return 0;
    }
    case WM_PAINT: {
        // Only reached in the non-layered (fully opaque) path; layered
        // windows are painted exclusively through UpdateLayeredWindow and
        // don't normally receive WM_PAINT. Re-blit the last rendered canvas
        // instead of leaving the window blank until the next WM_TIMER tick.
        PAINTSTRUCT paintStruct;
        HDC paintDC = BeginPaint(hwnd, &paintStruct);
        const AlphaCanvas& canvas = (hwnd == g_overlayLogWindow) ? g_overlayLogCanvas : g_overlayCanvas;
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
    int panelHeight = ComputeOverlayPanelHeight();
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

    SetTimer(g_overlayWindow, 1, g_overlayRefreshMs, NULL);
    LogLine("INFO", "Overlay window created; toggle_key=0x%02X log_toggle_key=0x%02X position=%s log_enabled=%s layered=%s",
        g_overlayToggleKey, g_overlayLogToggleKey, g_overlayPosition, g_overlayLogEnabled ? "true" : "false",
        g_overlayWindowsAreLayered ? "true" : "false");

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DestroyAlphaCanvas(g_overlayCanvas);
    DestroyAlphaCanvas(g_overlayLogCanvas);
    UnregisterClassA(windowClass.lpszClassName, module);
    g_overlayWindow = NULL;
    g_overlayLogWindow = NULL;
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

    g_enableAsiCheck = g_enableAsiCheck || GetEnvironmentFlag("SPELLADDON_ASI_CHECK");
    g_enableKeyboardRewrites = g_enableKeyboardRewrites || GetEnvironmentFlag("KEYBOARD_REWRITES");
    g_enableKeyboardRewriteLogging = g_enableKeyboardRewriteLogging || GetEnvironmentFlag("KEYBOARD_REWRITES_LOGGING");
    if (const char* reloadKeyEnv = getenv("RELOAD_CONFIG_KEY")) {
        g_reloadConfigKey = ParseVirtualKeyName(reloadKeyEnv, VK_F11);
    }
    g_enableCrashLogging = g_enableCrashLogging || GetEnvironmentFlag("LOGGING");
    g_enableFileIoLogging = g_enableFileIoLogging || GetEnvironmentFlag("FILE_IO_LOGGING");
    g_enableAntiCrash = g_enableAntiCrash || GetEnvironmentFlag("ANTICRASH");
    g_clearLogOnStart = g_clearLogOnStart || GetEnvironmentFlag("CLEAR_LOG_ON_START");
    g_enableMobValidation = g_enableMobValidation || GetEnvironmentFlag("MOB_VALIDATION");
    g_enableOverlay = g_enableOverlay || GetEnvironmentFlag("OVERLAY_ENABLED");
    if (const char* toggleKeyEnv = getenv("OVERLAY_TOGGLE_KEY")) {
        g_overlayToggleKey = ParseVirtualKeyName(toggleKeyEnv, VK_F9);
    }
    if (const char* positionEnv = getenv("OVERLAY_POSITION")) {
        ParseOverlayPosition(positionEnv, g_overlayPosition, sizeof(g_overlayPosition));
    }
    if (const char* colorEnv = getenv("OVERLAY_COLOR")) {
        g_overlayTextColor = ParseHexColor(colorEnv);
    }
    if (const char* refreshEnv = getenv("OVERLAY_REFRESH_MS")) {
        g_overlayRefreshMs = ParseOverlayRefreshMs(refreshEnv);
    }
    if (const char* showFpsGraphEnv = getenv("OVERLAY_SHOW_FPS_GRAPH")) {
        g_overlayShowFpsGraph = IsTrueString(showFpsGraphEnv);
    }
    if (const char* fpsMarksEnv = getenv("OVERLAY_FPS_MARKS")) {
        ParseFpsMarks(fpsMarksEnv, g_overlayFpsMarks, &g_overlayFpsMarkCount);
    }
    if (const char* showResourcesEnv = getenv("OVERLAY_SHOW_RESOURCES")) {
        g_overlayShowResources = IsTrueString(showResourcesEnv);
    }
    if (const char* showBackendEnv = getenv("OVERLAY_SHOW_BACKEND")) {
        g_overlayShowBackend = IsTrueString(showBackendEnv);
    }
    if (const char* showThreadsEnv = getenv("OVERLAY_SHOW_THREADS")) {
        g_overlayShowThreads = IsTrueString(showThreadsEnv);
    }
    if (const char* threadCountEnv = getenv("OVERLAY_THREAD_COUNT")) {
        g_overlayThreadDisplayCount = ParseOverlayThreadCount(threadCountEnv);
    }
    if (const char* backgroundColorEnv = getenv("OVERLAY_BACKGROUND_COLOR")) {
        g_overlayBackgroundColor = ParseHexColor(backgroundColorEnv, RGB(0, 0, 0));
    }
    if (const char* backgroundOpacityEnv = getenv("OVERLAY_BACKGROUND_OPACITY")) {
        g_overlayBackgroundOpacityPercent = ParseOverlayOpacityPercent(backgroundOpacityEnv);
    }
    if (const char* transparencyStyleEnv = getenv("OVERLAY_TRANSPARENCY_STYLE")) {
        SetQuotedConfigString(g_overlayTransparencyStyle, sizeof(g_overlayTransparencyStyle), transparencyStyleEnv);
    }
    if (const char* logEnabledEnv = getenv("OVERLAY_LOG_ENABLED")) {
        g_overlayLogEnabled = IsTrueString(logEnabledEnv);
    }
    if (const char* logToggleKeyEnv = getenv("OVERLAY_LOG_TOGGLE_KEY")) {
        g_overlayLogToggleKey = ParseVirtualKeyName(logToggleKeyEnv, VK_F10);
    }
    if (const char* logLinesEnv = getenv("OVERLAY_LOG_LINES")) {
        g_overlayLogLineCount = ParseOverlayLogLineCount(logLinesEnv);
    }
    if (const char* logWidthEnv = getenv("OVERLAY_LOG_WIDTH")) {
        g_overlayLogPanelWidth = ParseOverlayLogWidth(logWidthEnv);
    }
    if (const char* logWrapEnv = getenv("OVERLAY_LOG_WRAP")) {
        g_overlayLogWrapEnabled = IsTrueString(logWrapEnv);
    }
    if (const char* logPositionEnv = getenv("OVERLAY_LOG_POSITION")) {
        ParseOverlayPosition(logPositionEnv, g_overlayLogPosition, sizeof(g_overlayLogPosition));
    }
    if (const char* logLevelFilterEnv = getenv("OVERLAY_LOG_LEVEL_FILTER")) {
        SetQuotedConfigString(g_overlayLogLevelFilter, sizeof(g_overlayLogLevelFilter), logLevelFilterEnv);
    }
    g_enablePerformancePriority = g_enablePerformancePriority || GetEnvironmentFlag("PERFORMANCE_PRIORITY_ENABLED");
    if (const char* priorityClassEnv = getenv("PERFORMANCE_PRIORITY_CLASS")) {
        SetQuotedConfigString(g_performancePriorityClass, sizeof(g_performancePriorityClass), priorityClassEnv);
    }
    g_enablePerformanceAffinity = g_enablePerformanceAffinity || GetEnvironmentFlag("PERFORMANCE_AFFINITY_ENABLED");
    if (const char* affinityMaskEnv = getenv("PERFORMANCE_AFFINITY_MASK")) {
        SetQuotedConfigString(g_performanceAffinityMaskHex, sizeof(g_performanceAffinityMaskHex), affinityMaskEnv);
    }
    ApplyPerformanceTweaks();
    if (g_enableAntiCrash) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    g_enableHeapTermination = g_enableHeapTermination || GetEnvironmentFlag("HEAP_CORRUPTION_TERMINATION");
    if (g_enableHeapTermination) {
        // Converts heap corruption that the OS heap manager detects during a later,
        // unrelated alloc/free into an immediate fail-fast crash at that detection
        // point, instead of letting the process silently keep running on corrupted
        // metadata until a much later, harder-to-diagnose crash occurs elsewhere.
        if (!HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0)) {
            LogLine("WARN", "HeapSetInformation(HeapEnableTerminationOnCorruption) failed, error=%lu", GetLastError());
        }
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    g_vectoredExceptionHandler = AddVectoredExceptionHandler(1, VectoredLoggingHandler);
    if (!g_vectoredExceptionHandler) {
        LogLine("WARN", "AddVectoredExceptionHandler failed, error=%lu", GetLastError());
    }
    PrepareLogFile();
    LogLine("INFO", "Universal Mod DLL attached; asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s file_io_logging=%s clear_log_on_start=%s anti_crash=%s mob_validation=%s heap_corruption_termination=%s performance_priority=%s performance_affinity=%s overlay=%s",
        g_enableAsiCheck ? "enabled" : "disabled",
        g_enableKeyboardRewrites ? "enabled" : "disabled",
        g_enableKeyboardRewriteLogging ? "enabled" : "disabled",
        g_enableCrashLogging ? "enabled" : "disabled",
        g_enableFileIoLogging ? "enabled" : "disabled",
        g_clearLogOnStart ? "enabled" : "disabled",
        g_enableAntiCrash ? "enabled" : "disabled",
        g_enableMobValidation ? "enabled" : "disabled",
        g_enableHeapTermination ? "enabled" : "disabled",
        g_enablePerformancePriority ? "enabled" : "disabled",
        g_enablePerformanceAffinity ? "enabled" : "disabled",
        g_enableOverlay ? "enabled" : "disabled");
    if (g_enableAntiCrash) {
        LogLine("ANTICRASH", "Windows critical-error dialogs are suppressed; unsafe exceptions will still use normal crash handling");
    }
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
    if (g_enableFileIoLogging || g_enableMobValidation || g_enableOverlay) {
        InstallFileIoHooks();
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
