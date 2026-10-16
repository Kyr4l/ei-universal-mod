// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the import patches: um-engine functions, exit, file I/O and heap hooks, the log file.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

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
    int count = 0;
    const UmEngineFunction* list = functions(&count);
    int installed = 0;
    for (int i = 0; i < count && list; ++i) {
        BYTE* at = reinterpret_cast<BYTE*>(list[i].address);
        if (IsBadReadPtr(at, 8) || memcmp(at, list[i].expect, 8) != 0) {
            LogLine("WARN", "um-engine: %s skipped, game.exe has other code at 0x%08lX (another build)", list[i].name, list[i].address);
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) {
            LogLine("ERROR", "um-engine: cannot patch %s at 0x%08lX", list[i].name, list[i].address);
            continue;
        }
        const DWORD relative = reinterpret_cast<DWORD>(list[i].replacement) - (list[i].address + 5);
        at[0] = 0xE9; // jmp rel32
        memcpy(at + 1, &relative, 4);
        VirtualProtect(at, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
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

