// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// This DLL rewrites backtick and number-row input as US QWERTY scan codes,
// verifies the required SpellAddonX.asi file, and provides optional diagnostics.
// Logging, crash reporting, keyboard rewrite logging, and anti-crash
// behavior are configured through um.cfg beside the DLL or environment variables.

#include "um_internal.hpp"

#include "dll_server.hpp"

void DllServerNoteLog(const char* line) { dllserver::NoteLog(line); }

// A Windows path as the host system sees it: under Wine, the Unix path (Wine's own conversion);
// on Windows, unchanged.
static std::string HostPath(const std::string& path) {
    if (path.empty()) return path;
    typedef char*(CDECL * UnixNameFunction)(LPCWSTR);
    static UnixNameFunction unixName = reinterpret_cast<UnixNameFunction>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")));
    if (!unixName) return path;
    wchar_t wide[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide, MAX_PATH) == 0) return path;
    char* converted = unixName(wide);
    if (!converted) return path;
    std::string out = converted;
    HeapFree(GetProcessHeap(), 0, converted);
    return out;
}

// The map the game shows, for the DLL server's MAP command: the files' names, and their full paths (as
// the host sees them) when the game opened them.
static std::string DllServerMapInfo() {
    MapLoadState state;
    CopyMapState(&state);
    // The names from the game itself; what the opened files say when they cannot be read (the game
    // does not always open the files again when it goes back to a map).
    std::string terrain = state.mpr, base = state.base, quest = state.quest;
    std::string gameTerrain, gameBase, gameQuest;
    if (dllgame::GameMapNames(gameTerrain, gameBase, gameQuest)) {
        terrain = gameTerrain;
        base = gameBase;
        quest = gameQuest;
    }
    char text[512];
    snprintf(text, sizeof(text), "terrain=%s base=%s quest=%s script_maps=%d last_script_map=%s",
        dlldebug::Quote(terrain).c_str(), dlldebug::Quote(base).c_str(), dlldebug::Quote(quest).c_str(),
        state.extraCount, dlldebug::Quote(state.lastExtra).c_str());
    std::string out = text;
    const char* keys[] = {"terrain_path", "base_path", "quest_path"};
    const char* names[] = {terrain.c_str(), base.c_str(), quest.c_str()};
    for (int i = 0; i < 3; ++i) {
        if (!names[i][0]) continue;
        const std::string path = MapFilePath(names[i]);
        if (!path.empty()) out += std::string(" ") + keys[i] + "=" + dlldebug::Quote(HostPath(path));
    }
    return out;
}

// "sub_4A1F20+0x12 <hint>" (or a known name) for an address in game.exe's code, "" elsewhere.
static std::string DllServerGameFunction(uint32_t address) {
    EnsureProfileSymbols();
    if (address < g_codeLow || address >= g_codeHigh) return "";
    const DWORD function = ProfileFunctionOf(address);
    if (!function) return "";
    std::string name = ProfileNameOf(function);
    if (address != function) {
        char offset[16];
        snprintf(offset, sizeof(offset), "+0x%lX", static_cast<unsigned long>(address - function));
        const size_t space = name.find(' ');
        name.insert(space == std::string::npos ? name.size() : space, offset);
    }
    return name;
}

UM_GUARDED_THREAD(DllServerThread) {
    LabelCurrentThread(L"um.dll: DLL server");
    static dllserver::Host host;
    host.version = UM_VERSION;
    host.debugAllowed = g_dllServerDebug;
    host.password = g_dllServerPassword;
    host.mapInfo = DllServerMapInfo;
    host.gameFunction = DllServerGameFunction;
    host.mainThread = []() -> uint32_t { return g_profilerFrameThreadId; };
    if (g_dllServerDebug) {
        g_onMainThreadFrame = dllserver::RunQueuedCalls; // CALL (development only)
        if (host.password.empty())
            LogLine("WARN", "[DLL SERVER] DLL_SERVER_DEBUG is on but DLL_SERVER_PASSWORD is empty: the debug commands (WRITE, POKE, CALL, BP set, CONSOLE send) are refused until a password is set in um.cfg.");
        else
            LogLine("WARN", "[DLL SERVER] DLL_SERVER_DEBUG is on: a program on this computer that knows DLL_SERVER_PASSWORD can write the game's memory and run "
                            "code in it (CALL). Development only: turn it off in um.cfg outside a controlled development setup.");
    }
    host.log = [](const char* level, const char* message) { LogLine(level, "%s", message); };
    dllserver::Run(g_dllServerPort, host);
    return 0;
}

// Perform configuration, diagnostics, hooks, and ASI validation after the
// loader lock is released. Keeping this work out of DllMain avoids deadlocks.
UM_GUARDED_THREAD(InitializeDllThread) {
    EnsureThreadDescriptionFunctionsResolved();
    LabelCurrentThread(L"um.dll: Init");
    HMODULE hModule = reinterpret_cast<HMODULE>(parameter);
    InitializeCriticalSection(&g_logLock);
    g_logLockInitialized = true;

    char dllPath[MAX_PATH] = {};
    if (GetModuleFileNameA(hModule, dllPath, MAX_PATH) != 0) {
        LoadConfigFile(dllPath);
        if (const char* autoDump = getenv("UM_PROFILE_AUTODUMP")) { // unattended profiling runs (#80)
            g_profilerAutoDumpSeconds = atoi(autoDump);
            if (g_profilerAutoDumpSeconds > 0) { g_profilerEnabled = true; g_profilerVisible = 1; LogLine("INFO", "UM_PROFILE_AUTODUMP=%d: the profiler runs from the start and dumps every %d s", g_profilerAutoDumpSeconds, g_profilerAutoDumpSeconds); }
        }
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
    LogLine("INFO", "Universal Mod DLL attached; version=%s asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s file_io_logging=%s clear_log_on_start=%s suppress_error_dialogs=%s mob_validation=%s heap_overrun_reports=%s heap_alloc_padding=%d overlay=%s dll_server=%s dll_server_debug=%s",
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
        g_enableOverlay ? "enabled" : "disabled",
        g_dllServerEnabled ? "enabled" : "disabled",
        g_dllServerDebug ? "enabled" : "disabled");

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

    if (g_dllServerEnabled) {
        HANDLE serverThreadHandle = CreateThread(NULL, 0, DllServerThread, NULL, 0, NULL);
        if (serverThreadHandle) {
            CloseHandle(serverThreadHandle);
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
    if (g_enableEngine) {
        InstallEngineFunctions();
    }
    // after the hooks: the system information (a Vulkan instance and device enumeration) takes about a second, which the game must not win against the hooks
    LogSystemInformation();

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

