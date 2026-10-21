// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll's shared declarations: what every um_*.cpp file sees. The includes, the types and constants used by
// more than one file, the settings' values (defined in um_core.cpp) and the functions one file defines for the
// others, listed by the file that defines them. Made from the one-file um.cpp of 1.4.9 (#56).
#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// This DLL rewrites backtick and number-row input as US QWERTY scan codes,
// verifies the required SpellAddonX.asi file, and provides optional diagnostics.
// Logging, crash reporting, keyboard rewrite logging, and anti-crash
// behavior are configured through um.cfg beside the DLL or environment variables.

#include "../um-engine/um_engine.h"
#include <winsock2.h> // before windows.h (the DLL server, dll_server.hpp)
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


// ---- types, macros and constants shared by the files (in their original order) --------------------------

// um.dll's own version, shown in the overlay title and logged at startup.
static const char* const UM_VERSION = "1.5.3";

static const int OVERLAY_HISTORY_CAPACITY = 64;

// Recent per-frame presentation intervals (written by CountPresentedFrame on the
// game's render thread, read by the overlay thread) for the 1% low / worst
// frametime readout.
static const int OVERLAY_FRAME_TIME_CAPACITY = 4096;

static const double OVERLAY_FRAME_STATS_WINDOW_MS = 10000.0;

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

static const int OVERLAY_FPS_MARKS_MAX = 16;

// Real-time mirror of the last few formatted um.log lines, appended to by
// LogLine() itself; the overlay's log panel reads this instead of the file.
static const int OVERLAY_LOG_CAPACITY = 50;

// Wide enough for the long findings the validators write (a double-free or [MOBCHECK]
// line runs 250-400 characters); anything longer is cut when it is stored here, um.log
// on disk always has the full line.
static const size_t OVERLAY_LOG_ENTRY_SIZE = 512;

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

// SetThreadDescription/GetThreadDescription only exist on Windows 10 1607+ and
// are not declared by this project's older MinGW headers, so both are
// resolved dynamically; threads that never named themselves (true for every
// thread this 1990s/2000s-era game engine creates) simply show "(unnamed)".
typedef HRESULT (WINAPI *SetThreadDescriptionFunction)(HANDLE, PCWSTR);

typedef HRESULT (WINAPI *GetThreadDescriptionFunction)(HANDLE, PWSTR*);

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

typedef HRESULT (WINAPI *DDLockFunction)(void*, LPRECT, void*, DWORD, HANDLE);

typedef HRESULT (WINAPI *DDGetDCFunction)(void*, HDC*);

typedef HRESULT (WINAPI *DDGetSurfaceDescFunction)(void*, void*);

typedef HRESULT (WINAPI *DDColorKeyFunction)(void*, DWORD, void*);

typedef HRESULT (WINAPI *DDUnlockFunction)(void*, LPRECT);

typedef HRESULT (WINAPI *DDQueryInterfaceFunction)(void*, const IID*, void**);

typedef HRESULT (WINAPI *D3DCreateDeviceFunction)(void*, const IID*, void*, void**);


static const int DD_VTABLE_CREATESURFACE_SLOT = 6;

static const int DD_VTABLE_BLT_SLOT = 5;

static const int DD_VTABLE_BLTFAST_SLOT = 7;

static const int DD_VTABLE_FLIP_SLOT = 11;

static const int DD_VTABLE_LOCK_SLOT = 25; // IDirectDrawSurface*::Lock (the game locks a surface every frame: which one?)

static const int DD_VTABLE_GETDC_SLOT = 17;

static const int DD_VTABLE_GETSURFACEDESC_SLOT = 22;

static const int DD_VTABLE_GETCOLORKEY_SLOT = 16;

static const int DD_VTABLE_SETCOLORKEY_SLOT = 29;

static const int DD_VTABLE_UNLOCK_SLOT = 32;

static const int D3D7_VTABLE_CREATEDEVICE_SLOT = 4;   // IDirect3D7::CreateDevice


static const size_t DD_SURFACEDESC_DDSCAPS_OFFSET = 0x68;

static const DWORD DD_DDSCAPS_PRIMARYSURFACE = 0x00000200;

typedef VOID (WINAPI *ExitProcessFunction)(UINT);

struct TrackedFileHandle {
    HANDLE handle;
    char path[MAX_PATH];
    // Read/write entries are logged once per open handle to avoid flooding um.log.
    bool readLogged;
    bool writeLogged;
};

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

struct MobScriptContext {
    MobScriptDeclarations declarations;
    std::unordered_set<DWORD> objectIds;
    std::unordered_set<std::string> objectNames; // lower-case OBJNAMEs of the map's objects
    std::vector<std::string> addMobs;            // lower-case file names its script loads with AddMob
};

// Checks every live block's guard bytes, a chunk at a time so the game is never held up for long.
// A block the heap no longer knows (freed behind the hooks' back) is dropped instead of reported.
// An exception that leaves a thread of this DLL ends the whole game process, so every thread of the DLL
// runs inside this guard: the thread just ends.
#define UM_GUARDED_THREAD(name) \
    static DWORD WINAPI name##Body(LPVOID); \
    DWORD WINAPI name(LPVOID parameter) { try { return name##Body(parameter); } catch (...) { return 0; } } \
    static DWORD WINAPI name##Body(LPVOID parameter)

// Time spent inside the game's DirectDraw Flip/Blt/BltFast calls, measured only while the profiler window
// is shown, so the profiler can say what getting the frame to the screen costs the main thread.
enum { kDdFlip = 0, kDdBlt = 1, kDdBltFast = 2, kDdLock = 3, kDdKinds = 4 };

struct DdPairStat { LONGLONG ticks; LONG calls; LONGLONG pixels; };

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

// ---- the settings' values and the other variables the files share (defined where they were) ------------

extern HHOOK g_keyboardHook;
extern HMODULE g_dllModule;
extern BYTE g_reloadConfigKey;
extern bool g_enableAsiCheck;
extern bool g_enableEngine;
extern bool g_enableKeyboardRewrites;
extern bool g_enableKeyboardRewriteLogging;
extern bool g_enableCrashLogging;
extern bool g_suppressErrorDialogs;
extern bool g_clearLogOnStart;
extern bool g_enableCrashDumps;
extern char g_logPath[MAX_PATH];
extern CRITICAL_SECTION g_logLock;
extern bool g_logLockInitialized;
extern HANDLE g_logClearMutex;
extern volatile LONG g_crashLogInProgress;
extern volatile LONG g_errorBlockNumber;
extern bool g_keyboardRewriteKeyDown[256];
extern bool g_enableFileIoLogging;
extern char g_fileIoLoggingFilter[256];
extern bool g_enableMsaa;
bool IsD7vkDirectDraw(); // the DirectDraw driver in use (the chained DDRAW_DLL, else the loaded ddraw.dll) is D7VK (found by strings in its file)
extern char g_ddrawDll[MAX_PATH];
extern HMODULE g_chainedDdraw;
extern DirectDrawCreateFunction g_gameDirectDrawCreate;
extern DirectDrawCreateExFunction g_gameDirectDrawCreateEx;
extern bool g_enableMobValidation;
extern bool g_enableHeapFreeQuarantine;
extern int g_heapAllocPadding;
extern bool g_enableOverlay;
extern BYTE g_overlayToggleKey;
extern volatile LONG g_overlayVisible;
extern BYTE g_overlayLogToggleKey;
extern volatile LONG g_overlayLogVisible;
extern BYTE g_profilerKey;
extern BYTE g_profilerDumpKey;
extern bool g_uiQuadBlit;
extern int g_profilerAutoDumpSeconds;
extern char g_profilerPosition[16];
extern HWND g_profilerWindow;
extern int g_profilerHz;
extern int g_profilerLineCount;
extern bool g_dllServerEnabled;
extern bool g_dllServerDebug;
extern int g_dllServerPort;
extern bool g_profilerEnabled;
extern volatile LONG g_profilerVisible;
extern volatile LONG g_profilerRunning;
extern volatile DWORD g_profilerFrameThreadId;
extern void (*volatile g_onMainThreadFrame)();
extern HWND g_overlayWindow;
extern HWND g_overlayTargetWindow;
extern ULONGLONG g_overlayStartTickMs;
extern char g_overlayPosition[16];
extern COLORREF g_overlayTextColor;
extern bool g_overlayShowFpsGraph;
extern volatile LONG g_overlayFrameCount;
extern volatile LONG g_overlayFrameCounterActive;
extern LONG g_overlayFpsLastFrameCount;
extern ULONGLONG g_overlayFpsLastTickMs;
extern double g_overlayCurrentFps;
extern double g_overlayCurrentFrameTimeMs;
extern double g_overlayFpsPeakEver;
extern double g_overlayFpsHistory[OVERLAY_HISTORY_CAPACITY];
extern int g_overlayFpsHistoryNext;
extern int g_overlayFpsHistoryCount;
extern bool g_overlayCompact;
extern bool g_overlayShowFrameStats;
extern bool g_overlayShowLaa;
extern bool g_overlayShowMap;
extern bool g_overlayShowWarnings;
extern volatile LONG g_logWarningCount;
extern volatile LONG g_logErrorCount;
extern float g_overlayFrameTimesMs[OVERLAY_FRAME_TIME_CAPACITY];
extern volatile LONG g_overlayFrameTimeNext;
extern volatile LONG g_overlayFrameTimeCount;
extern MapLoadState g_map;
extern std::unordered_set<std::string> g_mapExpectedExtras;
extern volatile LONG g_currentMapLock;
extern int g_overlayRefreshMs;
extern double g_overlayFpsMarks[OVERLAY_FPS_MARKS_MAX];
extern int g_overlayFpsMarkCount;
extern COLORREF g_overlayBackgroundColor;
extern int g_overlayBackgroundOpacityPercent;
extern char g_overlayLogRing[OVERLAY_LOG_CAPACITY][OVERLAY_LOG_ENTRY_SIZE];
extern int g_overlayLogRingNext;
extern int g_overlayLogRingCount;
extern bool g_overlayLogEnabled;
extern int g_overlayLogLineCount;
extern char g_overlayLogPosition[16];
extern int g_overlayLogPanelWidth;
extern bool g_overlayLogWrapEnabled;
extern char g_overlayLogLevelFilter[128];
extern HWND g_overlayLogWindow;
extern char g_overlayTransparencyStyle[16];
extern bool g_overlayWindowsAreLayered;
extern bool g_overlayShowResources;
extern bool g_overlayShowBackend;
extern bool g_overlayShowThreads;
extern double g_overlayCpuPercent;
extern double g_overlayWorkingSetMb;
extern int g_overlayThreadCount;
extern ULONGLONG g_overlayLastCpuSampleTickMs;
extern ULONGLONG g_overlayLastCpuTotalTime100ns;
extern int g_overlayLogicalProcessorCount;
extern char g_directDrawBackendName[128];
extern volatile LONG g_directDrawBackendIdentified;
extern int g_overlayThreadDisplayCount;
extern OverlayThreadSample g_overlayThreadSamples[OVERLAY_THREAD_DISPLAY_MAX];
extern int g_overlayThreadSampleCount;
extern ULONGLONG g_overlayLastThreadSampleTickMs;
extern std::unordered_map<DWORD, ULONGLONG> g_threadLastCpuTime100ns;
extern SetThreadDescriptionFunction g_setThreadDescription;
extern GetThreadDescriptionFunction g_getThreadDescription;
extern volatile LONG g_threadDescriptionFunctionsResolved;
extern DDCreateSurfaceFunction g_originalDDCreateSurface;
extern DDFlipFunction g_originalDDFlip;
extern DDBltFunction g_originalDDBlt;
extern DDBltFastFunction g_originalDDBltFast;
extern DDLockFunction g_originalDDLock;
extern DDGetDCFunction g_originalDDGetDC;
extern DDUnlockFunction g_originalDDUnlock;
extern DDQueryInterfaceFunction g_originalDDQueryInterface;
extern D3DCreateDeviceFunction g_originalD3DCreateDevice;
extern void* g_directDrawObject;
extern ExitProcessFunction g_originalExitProcess;
extern ExitProcessFunction g_originalExitProcessCrt;
extern CreateFileAFunction g_originalCreateFileA;
extern CreateFileWFunction g_originalCreateFileW;
extern ReadFileFunction g_originalReadFile;
extern WriteFileFunction g_originalWriteFile;
extern CloseHandleFunction g_originalCloseHandle;
extern HeapAllocFunction g_originalHeapAlloc;
extern HeapFreeFunction g_originalHeapFree;
extern HeapDestroyFunction g_originalHeapDestroy;
extern HeapReAllocFunction g_originalHeapReAlloc;
extern DirectDrawCreateFunction g_originalDirectDrawCreate;
extern DirectDrawCreateExFunction g_originalDirectDrawCreateEx;
extern CRITICAL_SECTION g_fileHandleLock;
extern bool g_fileHandleLockInitialized;
extern TrackedFileHandle g_fileHandles[256];
extern const SettingDef kSettings[];
extern const size_t kSettingCount;
extern CRITICAL_SECTION g_quarantineLock;
extern bool g_heapFreeQuarantineInstalled;
extern DWORD g_canaryBytes;
extern DWORD g_imageLow, g_imageHigh, g_codeLow, g_codeHigh;
extern bool g_knownGameBuild;
extern bool g_heapPaddingOnlyInstalled;
extern volatile LONG g_heapHookTicks;
extern volatile LONG g_heapHookCalls;
extern volatile LONG g_ddCalls[kDdKinds];
extern volatile LONG g_ddTicks[kDdKinds];
extern volatile LONG g_ddPixels[kDdKinds];
extern volatile LONG g_ddWholeSurface[kDdKinds];
extern volatile LONG g_ddInfoLock;
extern std::map<std::string, DdPairStat> g_ddPairs;
extern volatile LONG g_uiQuadDrawn;
extern volatile LONG g_uiQuadFallback;
bool UiQuadCursorSave(void* destination, LPRECT destRect, void* source, LPRECT srcRect);
bool UiQuadBlt(void* destination, LPRECT destRect, void* source, LPRECT srcRect, DWORD flags, void* bltFx);
extern char g_profileLines[kProfileMaxLines][kProfileTextWidth];
extern int g_profileLineCount;
extern volatile LONG g_profileLock;

// ---- functions defined in one file and used by others --------------------------------------------------

// um_core.cpp
bool EqualsIgnoreCase(const char* a, const char* b);
bool IsTrueString(LPCSTR value);
BYTE ParseVirtualKeyName(const char* value, BYTE defaultKey = VK_F9);
COLORREF ParseHexColor(const char* value, COLORREF defaultColor = RGB(0, 255, 0));
void ParseOverlayPosition(const char* value, char* out, size_t outSize);
long ParseClampedLong(const char* value, long min, long max, long defaultValue);
void ParseFpsMarks(const char* value, double* outMarks, int* outCount);
void SetQuotedConfigString(char* dest, size_t destSize, const char* value);

// um_settings.cpp
SettingDef BoolSetting(const char* key, bool* target, bool def, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef StringSetting(const char* key, char* target, size_t bufferSize, const char* def, bool quoteInTemplate, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef VKeySetting(const char* key, BYTE* target, BYTE def, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef ColorSetting(const char* key, COLORREF* target, COLORREF def, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef IntSetting(const char* key, int* target, long min, long max, long def, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef PositionSetting(const char* key, char* target, size_t bufferSize, const char* def, bool hasEnv, const char* comment, const char* section = nullptr);
SettingDef FpsMarksSetting(const char* key, double* target, int* countTarget, const char* templateText, bool hasEnv, const char* comment, const char* section = nullptr);
void ApplySettingEnvOverride(const SettingDef& def);
void LoadConfigFile(const char* dllPath);
void ReloadConfiguration();

// um_log.cpp
bool IsCurrentProcessLargeAddressAware();
void EnsureThreadDescriptionFunctionsResolved();
void LabelCurrentThread(const wchar_t* name);
void SampleProcessDiagnostics();
void LogLine(const char* level, const char* format, ...);
void WriteCrashDump(EXCEPTION_POINTERS* exceptionInfo);
void LogFileIo(const char* format, ...);
bool CopyAndMarkTrackedFileIo(HANDLE handle, bool write, char* path, size_t pathSize);
void UntrackFileHandle(HANDLE handle);
void LogTrackedFileHandles();
void LogOpenedFile(HANDLE handle, const char* path, DWORD desiredAccess);
bool HasFileExtension(const char* path, const char* extension);
void LogFileContents(const char* direction, const char* path, const void* buffer, DWORD length);

// um_mobcheck.cpp
bool ReadMqBaseMapName(const std::string& mqPath, std::string* baseName, std::string* mprName = nullptr);
std::string LowerCaseCopy(const std::string& text);
void RememberSeenMobPath(const char* path);
std::shared_ptr<MobScriptContext> LoadMobContext(const std::string& mobPath);
void ValidateMobFile(const char* path);

// um_heap.cpp
long ReadGameCrtHeapMode();
bool LooksLikeReturnAddress(const BYTE* address, DWORD* target, bool knownReadable = false);
void InitGameImageRanges();
void CaptureCallers(DWORD* out, int slots);
bool GetQuarantineSnapshot(unsigned long* problems, unsigned long* overruns);
DWORD WINAPI OverrunWatchThread(LPVOID);
void LogQuarantineCrashAnalysis(const CONTEXT* context, const EXCEPTION_RECORD* record);
bool CopyMapState(MapLoadState* out);
std::string MapFilePath(const char* name);
HANDLE WINAPI HookedCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile);
HANDLE WINAPI HookedCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile);
BOOL WINAPI HookedReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead, LPOVERLAPPED overlapped);
BOOL WINAPI HookedWriteFile(HANDLE file, LPCVOID buffer, DWORD bytesToWrite, LPDWORD bytesWritten, LPOVERLAPPED overlapped);
BOOL WINAPI HookedCloseHandle(HANDLE handle);
LPVOID WINAPI PaddedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
LPVOID WINAPI PaddedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size);
BOOL WINAPI HookedHeapFree(HANDLE heap, DWORD flags, LPVOID pointer);
LPVOID WINAPI HookedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
LPVOID WINAPI HookedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size);
BOOL WINAPI HookedHeapDestroy(HANDLE heap);
void* PatchVTableSlot(void* comObject, int slotIndex, void* newFunction, bool* alreadyPatched = NULL);
std::string DescribeDdSurface(void* surface);
HRESULT WINAPI HookedDDFlip(void* self, void* targetOverride, DWORD flags);
HRESULT WINAPI HookedDDBlt(void* self, LPRECT destRect, void* srcSurface, LPRECT srcRect, DWORD flags, void* bltFx);
HRESULT WINAPI HookedDDBltFast(void* self, DWORD x, DWORD y, void* srcSurface, LPRECT srcRect, DWORD trans);

// um_ddraw.cpp
HRESULT WINAPI HookedDirectDrawCreate(const GUID* guid, void** directDraw, IUnknown* outerUnknown);
HRESULT WINAPI HookedDirectDrawCreateEx(const GUID* guid, void** directDraw, const GUID* interfaceId, IUnknown* outerUnknown);

// um_hooks.cpp
void InstallEngineFunctions();
void InstallFileIoHooks();
void InstallHeapHooks();
void LogErrorBlockStart();
void LogErrorBlockEnd();
void PrepareLogFile();

// um_diag.cpp
void LogSystemInformation();
LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo);

// um_input.cpp
DWORD WINAPI KeyPopupThread(LPVOID lpParameter);
HWND FindGameWindow();

// um_overlay.cpp
DWORD WINAPI OverlayThread(LPVOID);

// um_profiler.cpp
void EnsureProfileSymbols();
DWORD ProfileFunctionOf(DWORD address);
std::string ProfileNameOf(DWORD function);
void DumpProfile();
void UpdateProfilerState();
void AddOverlayLine(OverlayLine* lines, int* count, int capacity, bool alert, const char* format, ...);
bool ComputeFrameStats(double* onePercentLowFps, double* worstFrameMs, double* windowSeconds);
ULONGLONG GetProcessUptimeSeconds();
void FormatDuration(ULONGLONG seconds, char* out, size_t outSize);

// um.cpp
void DllServerNoteLog(const char* line);

// ---- shared types whose member functions use the variables and functions above --------------------------

struct DdTimer {
    int kind;
    void* destination;
    void* source;
    void* caller;        // the game's call site (the return address into game.exe), named in the per-pair line
    DWORD lockFlags = 0; // Lock: its DDLOCK_* flags, shown in the per-pair line
    LONGLONG start = 0;
    LONGLONG pixels = 0;
    DdTimer(int kindOfCall, const RECT* rect, void* destinationSurface, void* sourceSurface, void* callSite = NULL)
            : kind(kindOfCall), destination(destinationSurface), source(sourceSurface), caller(callSite) {
        if (g_profilerRunning) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            start = now.QuadPart;
            if (rect) pixels = static_cast<LONGLONG>(rect->right - rect->left) * (rect->bottom - rect->top);
            if (rect) InterlockedExchangeAdd(&g_ddPixels[kind], static_cast<LONG>(pixels));
            else InterlockedIncrement(&g_ddWholeSurface[kind]);
        }
    }
    ~DdTimer() {
        if (!start) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG ticks = now.QuadPart - start;
        InterlockedExchangeAdd(&g_ddTicks[kind], static_cast<LONG>(ticks));
        InterlockedIncrement(&g_ddCalls[kind]);
        bool locked = false;
        try {
            static const char* const kinds[kDdKinds] = { "Flip", "Blt", "BltFast", "Lock" };
            std::string key = kind == kDdLock ? std::string("Lock ") + DescribeDdSurface(destination)
                                              : std::string(kinds[kind]) + " " + DescribeDdSurface(source) + " -> " + DescribeDdSurface(destination);
            if (kind == kDdLock) {
                char fl[32];
                snprintf(fl, sizeof(fl), " flags 0x%lX", static_cast<unsigned long>(lockFlags));
                key += fl;
            }
            if (caller) { // who blits it: the game function the call returns into (RE starts there)
                char site[48];
                snprintf(site, sizeof(site), " @%s", ProfileNameOf(reinterpret_cast<DWORD>(caller)).c_str());
                key += site;
            }
            while (InterlockedCompareExchange(&g_ddInfoLock, 1, 0) != 0) Sleep(0);
            locked = true;
            if (g_ddPairs.size() < 200 || g_ddPairs.count(key)) {
                DdPairStat& stat = g_ddPairs[key];
                stat.ticks += ticks;
                stat.calls += 1;
                stat.pixels += pixels;
            }
        } catch (...) {
            // A destructor must not throw; the per-surface list is only a diagnostic.
        }
        if (locked) InterlockedExchange(&g_ddInfoLock, 0);
    }
};

