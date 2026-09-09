// This DLL rewrites backtick and number-row input as US QWERTY scan codes,
// verifies the required SpellAddonX.asi file, and provides optional diagnostics.
// Logging, crash reporting, keyboard rewrite logging, and unsafe anti-crash
// behavior are configured through um.cfg beside the DLL or environment variables.

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Global state
static HHOOK g_keyboardHook = NULL;
static bool g_enableAsiCheck = true;
static bool g_enableKeyboardRewrites = true;
static bool g_enableKeyboardRewriteLogging = false;
static bool g_enableCrashLogging = false;
static bool g_enableAntiCrash = false;
static bool g_forceAntiCrash = false;
static bool g_clearLogOnStart = false;
static char g_logPath[MAX_PATH] = {};
static HANDLE g_logClearMutex = NULL;
static volatile LONG g_crashLogInProgress = 0;
static volatile LONG g_errorBlockNumber = 0;
static volatile LONG g_suppressedErrorCount = 0;
static bool g_hasSuppressedErrorSignature = false;
static DWORD g_suppressedExceptionCode = 0;
static ULONG_PTR g_suppressedExceptionAddress = 0;
static long g_suppressedSummaryOffset = -1;
static bool g_keyboardRewriteKeyDown[256] = {};
static bool g_enableFileIoLogging = false;
static char g_fileIoLoggingFilter[256] = {};

typedef HANDLE (WINAPI *CreateFileAFunction)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFunction)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFileFunction)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WriteFileFunction)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandleFunction)(HANDLE);

static CreateFileAFunction g_originalCreateFileA = NULL;
static CreateFileWFunction g_originalCreateFileW = NULL;
static ReadFileFunction g_originalReadFile = NULL;
static WriteFileFunction g_originalWriteFile = NULL;
static CloseHandleFunction g_originalCloseHandle = NULL;

struct TrackedFileHandle {
    HANDLE handle;
    char path[MAX_PATH];
    bool readLogged;
    bool writeLogged;
};

static CRITICAL_SECTION g_fileHandleLock;
static bool g_fileHandleLockInitialized = false;
static TrackedFileHandle g_fileHandles[256] = {};

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

static bool GetEnvironmentFlag(LPCSTR name) {
    const char* value = getenv(name);
    if (!value) {
        return false;
    }
    return IsTrueString(value);
}

static void LogLine(const char* level, const char* format, ...) {
    if (!g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    FILE* file = fopen(g_logPath, "a");
    if (!file) {
        return;
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] pid=%lu tid=%lu ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, level, GetCurrentProcessId(), GetCurrentThreadId());

    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fclose(file);
}

static void LogFileIo(const char* format, ...) {
    if (!g_enableFileIoLogging || !g_enableCrashLogging || g_logPath[0] == '\0') {
        return;
    }

    char message[512] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LogLine("INFO", "%s", message);
}

static bool CopyAndMarkTrackedFileIo(HANDLE handle, bool write, char* path, size_t pathSize) {
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

static bool IsIgnoredFileExtension(const char* path) {
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

static void TrackFileHandle(HANDLE handle, const char* path) {
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

static void LogOpenedFile(HANDLE handle, const char* path, DWORD desiredAccess) {
    if (IsIgnoredFileExtension(path)) {
        return;
    }
    char access[32] = {};
    DescribeDesiredAccess(desiredAccess, access, sizeof(access));
    TrackFileHandle(handle, path);
    LogFileIo("File opened path=%s access=%s handle=%p", path, access, handle);
}

static void ConvertWidePath(LPCWSTR widePath, char* path, size_t pathSize) {
    if (!widePath || pathSize == 0) {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, widePath, -1, path, static_cast<int>(pathSize), NULL, NULL);
    path[pathSize - 1] = '\0';
}

static HANDLE WINAPI HookedCreateFileA(LPCSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileA(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        LogOpenedFile(handle, fileName, desiredAccess);
    }
    return handle;
}

static HANDLE WINAPI HookedCreateFileW(LPCWSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileW(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        char path[MAX_PATH] = {};
        ConvertWidePath(fileName, path, sizeof(path));
        LogOpenedFile(handle, path, desiredAccess);
    }
    return handle;
}

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

static BOOL WINAPI HookedCloseHandle(HANDLE handle) {
    BOOL result = g_originalCloseHandle(handle);
    if (result) {
        UntrackFileHandle(handle);
    }
    return result;
}

static bool PatchImportedFunction(HMODULE module, const char* functionName,
        PROC replacement, PROC* original) {
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
            *original = reinterpret_cast<PROC>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                oldProtection, &oldProtection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                sizeof(addresses->u1.Function));
            return true;
        }
    }
    return false;
}

static void InstallFileIoHooks() {
    HMODULE process = GetModuleHandleA(NULL);
    if (!process) {
        LogLine("WARN", "Could not locate the game executable for file-I/O hooks");
        return;
    }

    bool hooked = false;
    hooked = PatchImportedFunction(process, "CreateFileA",
        reinterpret_cast<PROC>(HookedCreateFileA), reinterpret_cast<PROC*>(&g_originalCreateFileA)) || hooked;
    hooked = PatchImportedFunction(process, "CreateFileW",
        reinterpret_cast<PROC>(HookedCreateFileW), reinterpret_cast<PROC*>(&g_originalCreateFileW)) || hooked;
    hooked = PatchImportedFunction(process, "ReadFile",
        reinterpret_cast<PROC>(HookedReadFile), reinterpret_cast<PROC*>(&g_originalReadFile)) || hooked;
    hooked = PatchImportedFunction(process, "WriteFile",
        reinterpret_cast<PROC>(HookedWriteFile), reinterpret_cast<PROC*>(&g_originalWriteFile)) || hooked;
    hooked = PatchImportedFunction(process, "CloseHandle",
        reinterpret_cast<PROC>(HookedCloseHandle), reinterpret_cast<PROC*>(&g_originalCloseHandle)) || hooked;
    LogLine(hooked ? "INFO" : "WARN", "File-I/O hooks %s", hooked ? "installed" : "not installed");
}

static void LogErrorBlockStart() {
    LONG blockNumber = InterlockedIncrement(&g_errorBlockNumber);
    LogLine("ERROR", "============= ERROR %ld LOG =============", blockNumber);
}

static void LogErrorBlockEnd() {
    LogLine("ERROR", "===========================================");
}

static bool IsSameSuppressedError(const EXCEPTION_RECORD* record, const CONTEXT* context) {
    (void)context;
    if (!record || !g_hasSuppressedErrorSignature) {
        return false;
    }

    return record->ExceptionCode == g_suppressedExceptionCode &&
        reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) == g_suppressedExceptionAddress;
}

static void UpdateSuppressedErrorSummary() {
    if (g_suppressedSummaryOffset < 0 || g_logPath[0] == '\0') {
        return;
    }

    FILE* file = fopen(g_logPath, "r+b");
    if (!file || fseek(file, g_suppressedSummaryOffset, SEEK_SET) != 0) {
        if (file) {
            fclose(file);
        }
        return;
    }

    char summary[256] = {};
    snprintf(summary, sizeof(summary),
        "[ANTICRASH] unsafe anticrash was triggered and %ld errors were suppressed to avoid duplication in the logs",
        g_suppressedErrorCount);
    char paddedSummary[257] = {};
    snprintf(paddedSummary, sizeof(paddedSummary), "%-255s\n", summary);
    fwrite(paddedSummary, 1, 256, file);
    fclose(file);
}

static void RecordSuppressedError(const EXCEPTION_RECORD* record, const CONTEXT* context) {
    if (!IsSameSuppressedError(record, context)) {
        return;
    }

    InterlockedIncrement(&g_suppressedErrorCount);
    UpdateSuppressedErrorSummary();
}

static void StartSuppressedErrorTracking(const EXCEPTION_RECORD* record, const CONTEXT* context) {
    (void)context;
    if (!record || g_hasSuppressedErrorSignature) {
        return;
    }
    g_suppressedExceptionCode = record->ExceptionCode;
    g_suppressedExceptionAddress = reinterpret_cast<ULONG_PTR>(record->ExceptionAddress);
    g_hasSuppressedErrorSignature = true;

    FILE* file = fopen(g_logPath, "ab");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    g_suppressedSummaryOffset = ftell(file);
    if (g_suppressedSummaryOffset < 0) {
        fclose(file);
        return;
    }
    char summary[256] = {};
    snprintf(summary, sizeof(summary),
        "[ANTICRASH] unsafe anticrash was triggered and 0 errors were suppressed to avoid duplication in the logs");
    char paddedSummary[257] = {};
    snprintf(paddedSummary, sizeof(paddedSummary), "%-255s\n", summary);
    fwrite(paddedSummary, 1, 256, file);
    fclose(file);
}

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

static void LogGraphicsInformation() {
    HKEY videoKey = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Video", 0,
            KEY_ENUMERATE_SUB_KEYS, &videoKey) != ERROR_SUCCESS) {
        LogLine("SYSINFO", "GPU information could not be queried");
        return;
    }

    DWORD index = 0;
    DWORD loggedAdapters = 0;
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

        char settingsKey[192] = {};
        snprintf(settingsKey, sizeof(settingsKey),
            "SYSTEM\\CurrentControlSet\\Control\\Video\\%s\\0000", adapterKeyName);
        char description[256] = {};
        char driverVersion[128] = {};
        char provider[128] = {};
        bool hasDescription = ReadRegistryString(HKEY_LOCAL_MACHINE, settingsKey,
            "DriverDesc", description, sizeof(description));
        bool hasDriverVersion = ReadRegistryString(HKEY_LOCAL_MACHINE, settingsKey,
            "DriverVersion", driverVersion, sizeof(driverVersion));
        bool hasProvider = ReadRegistryString(HKEY_LOCAL_MACHINE, settingsKey,
            "ProviderName", provider, sizeof(provider));
        if (hasDescription || hasDriverVersion || hasProvider) {
            LogLine("SYSINFO", "GPU index=%lu name=%s driver_version=%s provider=%s",
                loggedAdapters++, hasDescription ? description : "unknown",
                hasDriverVersion ? driverVersion : "unknown",
                hasProvider ? provider : "unknown");
        }
    }

    RegCloseKey(videoKey);
    if (loggedAdapters == 0) {
        LogLine("SYSINFO", "No GPU information was found");
    }
}

static void LogSystemInformation() {
    LogLine("SYSINFO", "============= SYSTEM INFORMATION =============");
    LogOperatingSystemInformation();
    LogHardwareInformation();
    LogGraphicsInformation();
    LogLine("SYSINFO", "==============================================");
}

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

static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (!g_enableCrashLogging) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_crashLogInProgress, 1, 0) != 0) {
        if (g_forceAntiCrash) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : NULL;
    CONTEXT* context = exceptionInfo ? exceptionInfo->ContextRecord : NULL;
    if (g_forceAntiCrash && IsSameSuppressedError(record, context)) {
        RecordSuppressedError(record, context);
        InterlockedExchange(&g_crashLogInProgress, 0);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    LogErrorBlockStart();

    if (!record) {
        LogLine("ERROR", "Unhandled exception had no exception record");
    } else {
        LogLine("ERROR", "Unhandled exception code=0x%08lX flags=0x%08lX address=%p parameters=%lu",
            record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress,
            record->NumberParameters);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            LogLine("ERROR", "Access violation type=%s address=%p",
                record->ExceptionInformation[0] == 0 ? "read" : "write",
                reinterpret_cast<void*>(static_cast<ULONG_PTR>(record->ExceptionInformation[1])));
        }
    }

    if (g_enableAntiCrash && !g_forceAntiCrash && IsUnsafeExceptionToResume(record)) {
        LogLine("ANTICRASH", "Recovery refused: case=%s reason=%s",
            GetExceptionCaseName(record), GetUnsafeExceptionReason(record));
        LogLine("ANTICRASH", "Normal Windows crash handling will continue to prevent silent process corruption");
    }

    if (context) {
#if defined(_M_IX86) || defined(__i386__)
        LogLine("ERROR", "CPU registers eax=0x%08lX ebx=0x%08lX ecx=0x%08lX edx=0x%08lX esi=0x%08lX edi=0x%08lX ebp=0x%08lX esp=0x%08lX eip=0x%08lX",
            context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi,
            context->Edi, context->Ebp, context->Esp, context->Eip);
        if (context->Eip == 0x90909090 || context->Eip == 0xCCCCCCCC || context->Eip == 0xCDCDCDCD) {
            LogLine("ANTICRASH", "Instruction pointer is a debug fill or NOP-sled sentinel (0x%08lX); original control flow cannot be reconstructed safely",
                context->Eip);
        }
#elif defined(_M_X64) || defined(__x86_64__)
        LogLine("ERROR", "CPU registers rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rbp=%p rsp=%p rip=%p",
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
            LogLine("ERROR", "Faulting module=%s", moduleName ? moduleName + 1 : modulePath);
        }
    }

    void* stack[32] = {};
    USHORT frameCount = CaptureStackBackTrace(0, sizeof(stack) / sizeof(stack[0]), stack, NULL);
    for (USHORT i = 0; i < frameCount; ++i) {
        LogLine("ERROR", "Stack frame=%u address=%p", i, stack[i]);
    }

    if (g_forceAntiCrash) {
        LogLine("ANTICRASH", "Exception case=%s", GetExceptionCaseName(record));
        if (IsUnsafeExceptionToResume(record)) {
            LogLine("ANTICRASH", "FORCE_UNSAFE_ANTICRASH is forcing continuation after an unsafe exception; the faulting context will be retried");
        } else {
            LogLine("ANTICRASH", "FORCE_UNSAFE_ANTICRASH resumed execution for a continuable exception");
        }
        LogErrorBlockEnd();
        StartSuppressedErrorTracking(record, context);
        InterlockedExchange(&g_crashLogInProgress, 0);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    LogLine("ERROR", "The process will continue with normal Windows crash handling");
    LogErrorBlockEnd();
    InterlockedExchange(&g_crashLogInProgress, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Load optional configuration overrides from um.cfg.
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
            fprintf(file, "; Resume even after unsafe exceptions; accepts true or false (not recommended).\n");
            fprintf(file, "FORCE_UNSAFE_ANTICRASH=false\n");
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
        } else if (EqualsIgnoreCase(key, "FORCE_UNSAFE_ANTICRASH")) {
            g_forceAntiCrash = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "CLEAR_LOG_ON_START")) {
            g_clearLogOnStart = IsTrueString(value);
        }
    }

    fclose(file);
}

// Synthesize a US-QWERTY backtick press.
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

// Synthesize a US-QWERTY number-row press.
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

// DLL entry point: initialize the hook and validate the required ASI file.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DisableThreadLibraryCalls(hModule);

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
    g_forceAntiCrash = g_forceAntiCrash || GetEnvironmentFlag("FORCE_UNSAFE_ANTICRASH");
    g_enableCrashLogging = g_enableCrashLogging || g_forceAntiCrash;
    g_clearLogOnStart = g_clearLogOnStart || GetEnvironmentFlag("CLEAR_LOG_ON_START");
    if (g_enableAntiCrash || g_forceAntiCrash) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    PrepareLogFile();
    LogLine("INFO", "Universal Mod DLL attached; asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s file_io_logging=%s clear_log_on_start=%s anti_crash=%s force_anti_crash=%s",
        g_enableAsiCheck ? "enabled" : "disabled",
        g_enableKeyboardRewrites ? "enabled" : "disabled",
        g_enableKeyboardRewriteLogging ? "enabled" : "disabled",
        g_enableCrashLogging ? "enabled" : "disabled",
        g_enableFileIoLogging ? "enabled" : "disabled",
        g_clearLogOnStart ? "enabled" : "disabled",
        g_enableAntiCrash ? "enabled" : "disabled",
        g_forceAntiCrash ? "enabled" : "disabled");
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
    if (g_enableFileIoLogging) {
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

    return TRUE;
}
