// This DLL rewrites backtick and number-row input as US QWERTY scan codes,
// verifies the required SpellAddonX.asi file, and provides optional diagnostics.
// Logging, crash reporting, keyboard rewrite logging, and unsafe anti-crash
// behavior are configured through um.cfg beside the DLL or environment variables.

#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <unordered_set>

// The DLL is injected into the game process, so these flags and hooks are
// process-local. Configuration is loaded once during DLL_PROCESS_ATTACH.
static HHOOK g_keyboardHook = NULL;
static bool g_enableAsiCheck = true;
static bool g_enableKeyboardRewrites = true;
static bool g_enableKeyboardRewriteLogging = false;
static bool g_enableCrashLogging = false;
static bool g_enableAntiCrash = false;
static bool g_clearLogOnStart = false;
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

static CreateFileAFunction g_originalCreateFileA = NULL;
static CreateFileWFunction g_originalCreateFileW = NULL;
static ReadFileFunction g_originalReadFile = NULL;
static WriteFileFunction g_originalWriteFile = NULL;
static CloseHandleFunction g_originalCloseHandle = NULL;
static DirectDrawCreateFunction g_originalDirectDrawCreate = NULL;
static DirectDrawCreateExFunction g_originalDirectDrawCreateEx = NULL;

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

// Append one timestamped, serialized diagnostic line and flush it to disk.
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
    fprintf(file, "[%04u-%02u-%02uT%02u:%02u:%02u%c%02d%02d] [%s] %s",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        offsetSign, absoluteOffsetMinutes / 60, absoluteOffsetMinutes % 60,
        outputLevel, category ? "[" : "");
    if (category) {
        fprintf(file, "%s] ", category);
    }

    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
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
    char dumpPath[MAX_PATH] = {};
    snprintf(dumpPath, sizeof(dumpPath), "%s.%04u%02u%02u-%02u%02u%02u.dmp",
        g_logPath, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
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

// Normalize and store the comma-separated file-I/O extension filter.
static void SetFileIoLoggingFilter(const char* value) {
    g_fileIoLoggingFilter[0] = '\0';
    if (!value) {
        return;
    }

    while (*value == ' ' || *value == '\t' || *value == '"') {
        ++value;
    }
    strncpy(g_fileIoLoggingFilter, value, sizeof(g_fileIoLoggingFilter) - 1);
    g_fileIoLoggingFilter[sizeof(g_fileIoLoggingFilter) - 1] = '\0';
    size_t length = strlen(g_fileIoLoggingFilter);
    while (length > 0 &&
        (g_fileIoLoggingFilter[length - 1] == ' ' ||
         g_fileIoLoggingFilter[length - 1] == '\t' ||
         g_fileIoLoggingFilter[length - 1] == '"')) {
        g_fileIoLoggingFilter[--length] = '\0';
    }
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

// Log DirectDraw initialization results without changing the returned object.
static HRESULT WINAPI HookedDirectDrawCreate(const GUID* guid, void** directDraw,
        IUnknown* outerUnknown) {
    HRESULT result = g_originalDirectDrawCreate(guid, directDraw, outerUnknown);
    LogLine(FAILED(result) ? "ERROR" : "INFO",
        "DirectDrawCreate result=0x%08lX object=%p", result,
        directDraw ? *directDraw : NULL);
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
        // These hooks back both file-I/O logging and .mob validation, so a
        // generic success line here would not indicate which feature is active.
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
    WriteCrashDump(exceptionInfo);
    LogTrackedFileHandles();

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
            fprintf(file, "; Universal Mod Configuration\n");
            fprintf(file, "; Require SpellAddonX.asi beside game.exe; (true/false)\n");
            fprintf(file, "SPELLADDON_ASI_CHECK=true\n\n");
            fprintf(file, "; Rewrite backtick and number-row input as US-QWERTY keys; (true/false)\n");
            fprintf(file, "KEYBOARD_REWRITES=true\n\n");
            fprintf(file, "; Log keyboard rewrite events; (true/false)\n");
            fprintf(file, "KEYBOARD_REWRITES_LOGGING=false\n\n");
            fprintf(file, "; Write diagnostic and crash information to um.log; (true/false)\n");
            fprintf(file, "LOGGING=false\n\n");
            fprintf(file, "; Log file opens, reads, and writes as INFO entries; (true/false)\n");
            fprintf(file, "FILE_IO_LOGGING=false\n\n");
            fprintf(file, "; Comma-separated file extensions to exclude from file-I/O logging; empty or like mmp,res.\n");
            fprintf(file, "FILE_IO_LOGGING_FILTER=\"\"\n\n");
            fprintf(file, "; Clear um.log on the first DLL instance of a launch; (true/false)\n");
            fprintf(file, "CLEAR_LOG_ON_START=false\n\n");
            fprintf(file, "; Suppress critical-error dialogs; unsafe exceptions still crash normally; (true/false)\n");
            fprintf(file, "ANTICRASH=false\n\n");
            fprintf(file, "; Write portable Windows minidumps beside um.log; (true/false)\n");
            fprintf(file, "CRASH_DUMPS=true\n\n");
            fprintf(file, "; Validate .mob file headers when opened and log structural problems, including a\n");
            fprintf(file, "; cross-check of unit weapons/armors/spells/quest/quick items against the item/spell database; (true/false)\n");
            fprintf(file, "MOB_VALIDATION=false\n\n");
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
        }
    }

    fclose(file);
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

// Intercept the backtick and number-row keys and rewrite them as US-QWERTY presses.
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (!g_enableKeyboardRewrites) {
        return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
    }

    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb) {
            // Do not process the synthetic events generated by SendInput above.
            if ((kb->flags & LLKHF_INJECTED) != 0) {
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
            }
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
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// Install the low-level keyboard hook and keep it alive with a message loop.
// WH_KEYBOARD_LL callbacks are delivered to this thread, not to the game thread.
// Synthetic SendInput events are filtered out by LowLevelKeyboardProc.
DWORD WINAPI KeyPopupThread(LPVOID lpParameter) {
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

// Perform configuration, diagnostics, hooks, and ASI validation after the
// loader lock is released. Keeping this work out of DllMain avoids deadlocks.
static DWORD WINAPI InitializeDllThread(LPVOID parameter) {
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
    g_enableCrashLogging = g_enableCrashLogging || GetEnvironmentFlag("LOGGING");
    g_enableFileIoLogging = g_enableFileIoLogging || GetEnvironmentFlag("FILE_IO_LOGGING");
    g_enableAntiCrash = g_enableAntiCrash || GetEnvironmentFlag("ANTICRASH");
    g_clearLogOnStart = g_clearLogOnStart || GetEnvironmentFlag("CLEAR_LOG_ON_START");
    g_enableMobValidation = g_enableMobValidation || GetEnvironmentFlag("MOB_VALIDATION");
    if (g_enableAntiCrash) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    g_vectoredExceptionHandler = AddVectoredExceptionHandler(1, VectoredLoggingHandler);
    if (!g_vectoredExceptionHandler) {
        LogLine("WARN", "AddVectoredExceptionHandler failed, error=%lu", GetLastError());
    }
    PrepareLogFile();
    LogLine("INFO", "Universal Mod DLL attached; asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s file_io_logging=%s clear_log_on_start=%s anti_crash=%s mob_validation=%s",
        g_enableAsiCheck ? "enabled" : "disabled",
        g_enableKeyboardRewrites ? "enabled" : "disabled",
        g_enableKeyboardRewriteLogging ? "enabled" : "disabled",
        g_enableCrashLogging ? "enabled" : "disabled",
        g_enableFileIoLogging ? "enabled" : "disabled",
        g_clearLogOnStart ? "enabled" : "disabled",
        g_enableAntiCrash ? "enabled" : "disabled",
        g_enableMobValidation ? "enabled" : "disabled");
    if (g_enableAntiCrash) {
        LogLine("ANTICRASH", "Windows critical-error dialogs are suppressed; unsafe exceptions will still use normal crash handling");
    }
    LogSystemInformation();

    HANDLE threadHandle = CreateThread(NULL, 0, KeyPopupThread, hModule, 0, NULL);
    if (threadHandle) {
        CloseHandle(threadHandle);
    }

    InitializeCriticalSection(&g_fileHandleLock);
    g_fileHandleLockInitialized = true;
    if (g_enableFileIoLogging || g_enableMobValidation) {
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

    DisableThreadLibraryCalls(hModule);
    HANDLE threadHandle = CreateThread(NULL, 0, InitializeDllThread, hModule, 0, NULL);
    if (threadHandle) {
        CloseHandle(threadHandle);
    }
    return TRUE;
}
