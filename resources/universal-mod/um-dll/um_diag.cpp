// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: system information logged at start, and the unhandled exception handler.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

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

void LogSystemInformation() {
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
LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
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

