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
static volatile LONG g_crashLogInProgress = 0;
static volatile LONG g_errorBlockNumber = 0;
static volatile LONG g_suppressedErrorCount = 0;
static bool g_hasSuppressedErrorSignature = false;
static DWORD g_suppressedExceptionCode = 0;
static ULONG_PTR g_suppressedExceptionAddress = 0;
static long g_suppressedSummaryOffset = -1;
static bool g_keyboardRewriteKeyDown[256] = {};

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
            fprintf(file, "; Set to true to enable, false to disable\n\n");
            fprintf(file, "SPELLADDON_ASI_CHECK=true\n");
            fprintf(file, "KEYBOARD_REWRITES=true\n");
            fprintf(file, "KEYBOARD_REWRITES_LOGGING=false\n");
            fprintf(file, "LOGGING=false\n");
            fprintf(file, "CLEAR_LOG_ON_START=false\n");
            fprintf(file, "ANTICRASH=false\n");
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
        LogLine("KBRW", "Rewriting virtual key 0xC0 as US-QWERTY backtick scan code 0x29");
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
            if (kb->vkCode == 0xC0) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    bool logRewrite = !g_keyboardRewriteKeyDown[kb->vkCode];
                    g_keyboardRewriteKeyDown[kb->vkCode] = true;
                    SendQwertyBacktickPress(logRewrite);
                } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    g_keyboardRewriteKeyDown[kb->vkCode] = false;
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
    g_enableAntiCrash = g_enableAntiCrash || GetEnvironmentFlag("ANTICRASH");
    g_forceAntiCrash = g_forceAntiCrash || GetEnvironmentFlag("FORCE_UNSAFE_ANTICRASH");
    g_enableCrashLogging = g_enableCrashLogging || g_forceAntiCrash;
    g_clearLogOnStart = g_clearLogOnStart || GetEnvironmentFlag("CLEAR_LOG_ON_START");
    if (g_enableAntiCrash || g_forceAntiCrash) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    PrepareLogFile();
    LogLine("INFO", "Universal Mod DLL attached; asi_check=%s keyboard_rewrites=%s keyboard_rewrite_logging=%s logging=%s clear_log_on_start=%s anti_crash=%s force_anti_crash=%s",
        g_enableAsiCheck ? "enabled" : "disabled",
        g_enableKeyboardRewrites ? "enabled" : "disabled",
        g_enableKeyboardRewriteLogging ? "enabled" : "disabled",
        g_enableCrashLogging ? "enabled" : "disabled",
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
