// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: um.log, the crash dump, the file I/O logging and the tracked handles.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// Reads the IMAGE_FILE_LARGE_ADDRESS_AWARE bit straight out of the running
// process's own PE header in memory (no need to reopen game.exe from disk:
// um.dll runs injected inside it, so its own module base IS game.exe's base).
// Result never changes at runtime, so it is cached after the first call.
bool IsCurrentProcessLargeAddressAware() {
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
void EnsureThreadDescriptionFunctionsResolved() {
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
void LabelCurrentThread(const wchar_t* name) {
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
void SampleProcessDiagnostics() {
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
void LogLine(const char* level, const char* format, ...) {
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
    DllServerNoteLog(fullLine);

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
void WriteCrashDump(EXCEPTION_POINTERS* exceptionInfo) {
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
void LogFileIo(const char* format, ...) {
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
bool CopyAndMarkTrackedFileIo(HANDLE handle, bool write, char* path, size_t pathSize) {
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
void UntrackFileHandle(HANDLE handle) {
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
void LogTrackedFileHandles() {
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
void LogOpenedFile(HANDLE handle, const char* path, DWORD desiredAccess) {
    if (IsIgnoredFileExtension(path)) {
        return;
    }
    char access[32] = {};
    DescribeDesiredAccess(desiredAccess, access, sizeof(access));
    TrackFileHandle(handle, path);
    LogFileIo("File opened path=%s access=%s handle=%p", path, access, handle);
}

// Return whether a path's file name ends with the given extension (no dot), case-insensitively.
bool HasFileExtension(const char* path, const char* extension) {
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

void LogFileContents(const char* direction, const char* path, const void* buffer, DWORD length) {
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


