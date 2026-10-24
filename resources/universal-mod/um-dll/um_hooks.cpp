// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the import patches: um-engine functions, exit, file I/O and heap hooks, the log file.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// Patch the executable's import address table instead of replacing kernel32
// exports globally. This limits logging to calls made through game.exe's
// imported file APIs and leaves other processes untouched.
bool PatchImportedFunction(HMODULE module, const char* functionName,
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
// um-engine.dll (beside um.dll): each function it lists replaces the game's own with a jump to the new code.
void InstallEngineFunctions() {
    char path[MAX_PATH] = "";
    if (!g_dllModule || GetModuleFileNameA(g_dllModule, path, sizeof(path)) == 0) return;
    std::string enginePath = path;
    const size_t slash = enginePath.find_last_of("\\/");
    enginePath = (slash == std::string::npos ? std::string() : enginePath.substr(0, slash + 1)) + "um-engine.dll";
    HMODULE engine = LoadLibraryA(enginePath.c_str());
    if (!engine) {
        LogLine("ERROR", "um-engine.dll not found or not loadable (%s); the game's own functions are used", enginePath.c_str());
        return;
    }
    const auto version = reinterpret_cast<UmEngineVersionFn>(reinterpret_cast<void*>(GetProcAddress(engine, "UmEngineVersion")));
    const auto functions = reinterpret_cast<UmEngineFunctionsFn>(reinterpret_cast<void*>(GetProcAddress(engine, "UmEngineFunctions")));
    if (!functions) { LogLine("ERROR", "um-engine.dll has no UmEngineFunctions export"); return; }
    const auto abi = reinterpret_cast<UmEngineAbiFn>(reinterpret_cast<void*>(GetProcAddress(engine, "UmEngineAbi")));
    if (!abi || abi() != UM_ENGINE_ABI) {
        LogLine("ERROR", "um-engine.dll %s is not the build um.dll %s was made for (interface %s != 0x%X): not used, the game's own functions are used", version ? version() : "?", UM_VERSION,
                abi ? "different" : "unknown", UM_ENGINE_ABI);
        return;
    }
    // um-engine's own messages (what it installed, what it skipped, its research counters) are written to this log
    if (const auto setLogger = reinterpret_cast<UmEngineSetLoggerFn>(reinterpret_cast<void*>(GetProcAddress(engine, "UmEngineSetLogger"))))
        setLogger([](const char* level, const char* text) { LogLine(level, "%s", text); });
    int count = 0;
    const UmEngineFunction* list = functions(&count);
    int installed = 0;
    for (int i = 0; i < count && list; ++i) {
        BYTE* at = reinterpret_cast<BYTE*>(list[i].address);
        if (IsBadReadPtr(at, 8) || memcmp(at, list[i].expect, 8) != 0) {
            LogLine("WARN", "um-engine: %s skipped, game.exe has other code at 0x%08lX (another build)", list[i].name, list[i].address);
            continue;
        }
        // one locked write of the first eight bytes: the game's threads are already running while this is done (um_engine.h)
        if (!UmPatchJump(list[i].address, 0xE9, list[i].replacement)) {
            LogLine("ERROR", "um-engine: cannot patch %s at 0x%08lX", list[i].name, list[i].address);
            continue;
        }
        ++installed;
    }
    LogLine("INFO", "um-engine.dll %s: %d of %d function(s) replaced", version ? version() : "?", installed, count);
}


static void LogExit(const char* which, UINT code, void* returnAddress) {
    DWORD callers[8] = {};
    CaptureCallers(callers, 8);
    char text[512];
    int used = snprintf(text, sizeof(text), "%s ExitProcess(%u) from %s", which, code, ProfileNameOf(reinterpret_cast<DWORD>(returnAddress)).c_str());
    for (int i = 0; i < 8 && callers[i] && used > 0 && used < static_cast<int>(sizeof(text)) - 40; ++i)
        used += snprintf(text + used, sizeof(text) - used, " <- %s", ProfileNameOf(callers[i]).c_str());
    LogLine("INFO", "%s", text);
}
static VOID WINAPI HookedExitProcess(UINT code) {
    LogExit("game.exe", code, __builtin_return_address(0));
    g_originalExitProcess(code);
}
static VOID WINAPI HookedExitProcessCrt(UINT code) {
    LogExit("CRT exit():", code, __builtin_return_address(0));
    g_originalExitProcessCrt(code);
}

// DDRAW_DLL: loads that DirectDraw wrapper by its full path and makes the game's DirectDrawCreate / DirectDrawCreateEx calls (already routed through
// um.dll's hooks) go to it. Runs before the game's first DirectDrawCreate; any failure keeps the ddraw.dll of the game folder.
// MSAA: D7VK reads DXVK_CONFIG (semicolon separated options, merged over dxvk.conf) when it creates its device, so this must run before the game's first DirectDrawCreate.
static void ApplyMsaa() {
    if (!g_enableMsaa) return;
    char current[1024] = "";
    const DWORD length = GetEnvironmentVariableA("DXVK_CONFIG", current, sizeof(current));
    if (length >= sizeof(current)) return;
    std::string value = length ? std::string(current) + ";" : std::string();
    value += "ddraw.emulateFSAA = forced;ddraw.alternatePixelCenter = true"; // the pixel-center fix: without it a multisampled frame has a 1-pixel line at the left and top edge
    SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
    LogLine("INFO", "MSAA: DXVK_CONFIG=%s (takes effect when D7VK is the DirectDraw driver)", value.c_str());
}

static void ChainloadDirectDraw() {
    if (!g_ddrawDll[0]) return;
    std::string path = g_ddrawDll;
    bool automatic = false;
    if (_stricmp(g_ddrawDll, "auto") == 0) { // the patched D7VK next to um.dll under Wine; on Windows the game's own DirectDraw until D7VK is tested there
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return;
        path = "d7vk.dll"; automatic = true;
    }
    const bool absolute = path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/');
    if (!absolute) {
        char self[MAX_PATH] = "";
        if (!g_dllModule || GetModuleFileNameA(g_dllModule, self, sizeof(self)) == 0) return;
        std::string dir = self;
        const size_t slash = dir.find_last_of("\\/");
        path = (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1)) + path;
    }
    if (automatic && GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return; // not shipped: silently the game's own
    HMODULE module = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { LogLine("ERROR", "DDRAW_DLL %s could not be loaded (error %lu); the game's ddraw.dll is used", path.c_str(), GetLastError()); return; }
    FARPROC create = GetProcAddress(module, "DirectDrawCreate"), createEx = GetProcAddress(module, "DirectDrawCreateEx");
    if (!create || !createEx) { LogLine("ERROR", "DDRAW_DLL %s lacks DirectDrawCreate / DirectDrawCreateEx; the game's ddraw.dll is used", path.c_str()); FreeLibrary(module); return; }
    g_gameDirectDrawCreate = g_originalDirectDrawCreate; g_gameDirectDrawCreateEx = g_originalDirectDrawCreateEx;
    memcpy(&g_originalDirectDrawCreate, &create, sizeof(create));
    memcpy(&g_originalDirectDrawCreateEx, &createEx, sizeof(createEx));
    g_chainedDdraw = module;
    LogLine("INFO", "DDRAW_DLL: DirectDraw comes from %s", path.c_str());
}

void InstallFileIoHooks() {
    HMODULE process = GetModuleHandleA(NULL);
    if (!process) {
        LogLine("WARN", "Could not locate the game executable for file-I/O hooks");
        return;
    }

    bool hooked = false;
    // Who ends the game: ExitProcess with its code and the callers (a clean exit leaves no crash record otherwise).
    PatchImportedFunction(process, "ExitProcess", reinterpret_cast<ULONG_PTR>(HookedExitProcess), reinterpret_cast<ULONG_PTR*>(&g_originalExitProcess));
    if (HMODULE crt = GetModuleHandleA("ucrtbase.dll"))
        PatchImportedFunction(crt, "ExitProcess", reinterpret_cast<ULONG_PTR>(HookedExitProcessCrt), reinterpret_cast<ULONG_PTR*>(&g_originalExitProcessCrt));
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
    InstallWindowHooks();
    ApplyMsaa();
    ChainloadDirectDraw();
    if (!hooked) {
        // These hooks back file-I/O logging, .mob validation, and the overlay's
        // FPS counter, so a generic success line here would not indicate which
        // feature is active.
        LogLine("WARN", "File-I/O hooks not installed");
    }
}

// Patch game.exe's own imports of HeapFree/HeapReAlloc/HeapDestroy so
// HEAP_OVERRUN_REPORTS can track allocations and hold freed blocks back (see QuarantineHeapFree).
void InstallHeapHooks() {
    if (!g_enableHeapFreeQuarantine && g_heapAllocPadding == 0) {
        return;
    }
    HMODULE process = GetModuleHandleA(NULL);
    if (!process) {
        LogLine("WARN", "[HEAPFIX] Not activated: could not locate the game executable");
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
            "512 bytes, for blocks of 1 KB or more); nothing is tracked, set HEAP_OVERRUN_REPORTS=true for overrun reports",
            g_heapAllocPadding);
        return;
    }

    g_canaryBytes = static_cast<DWORD>(g_heapAllocPadding);
    InitializeCriticalSection(&g_quarantineLock);

    PatchImportedFunction(process, "HeapAlloc",
        reinterpret_cast<ULONG_PTR>(HookedHeapAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapAlloc));
    bool freeHooked = PatchImportedFunction(process, "HeapFree",
        reinterpret_cast<ULONG_PTR>(HookedHeapFree), reinterpret_cast<ULONG_PTR*>(&g_originalHeapFree));
    PatchImportedFunction(process, "HeapReAlloc",
        reinterpret_cast<ULONG_PTR>(HookedHeapReAlloc), reinterpret_cast<ULONG_PTR*>(&g_originalHeapReAlloc));
    PatchImportedFunction(process, "HeapDestroy",
        reinterpret_cast<ULONG_PTR>(HookedHeapDestroy), reinterpret_cast<ULONG_PTR*>(&g_originalHeapDestroy));

    if (!freeHooked || !g_originalHeapFree) {
        LogLine("WARN", "[HEAPFIX] Not activated: game.exe does not import HeapFree directly");
        return;
    }
    if (!g_originalHeapDestroy || !g_originalHeapReAlloc || !g_originalHeapAlloc) {
        // Never expected (game.exe imports both), but without them a destroyed
        // heap's held blocks could not be forgotten and a resize of a freed
        // block could not be redirected - so hold nothing.
        LogLine("WARN", "[HEAPFIX] Not activated: game.exe does not import HeapAlloc/HeapDestroy/HeapReAlloc");
        return;
    }
    g_heapFreeQuarantineInstalled = true;
    HANDLE watch = CreateThread(NULL, 0, OverrunWatchThread, NULL, 0, NULL);
    if (watch) CloseHandle(watch);
    LogLine("DEBUG", "[HEAPFIX] Active: every allocation padded by %d bytes (more for blocks of 1 KB or more) and tracked; "
        "freed blocks are freed at once", g_heapAllocPadding);
    long mode = ReadGameCrtHeapMode();
    if (mode != 1) {
        LogLine("WARN", "[HEAPFIX] game.exe CRT heap mode reads %ld (expected 1 = system heap); "
            "with mode 2/3 its small-block heap frees small blocks WITHOUT calling HeapFree, so those "
            "are not tracked", mode);
    }
}

// Start a numbered exception block in the diagnostic log.
void LogErrorBlockStart() {
    LONG blockNumber = InterlockedIncrement(&g_errorBlockNumber);
    LogLine("FATAL", "============= ERROR %ld LOG =============", blockNumber);
}

// Close the current numbered exception block in the diagnostic log.
void LogErrorBlockEnd() {
    LogLine("FATAL", "===========================================");
}

// Clear or separate the log at process startup, with a cross-instance mutex.
void PrepareLogFile() {
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

