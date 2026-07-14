// This DLL hooks low-level keyboard events to synthesize US QWERTY key presses
// for keys that may be affected by non-US keyboard layouts (backtick and number row).
// It also verifies the presence of a required .asi file on DLL attach.
//
// Configuration is managed via um.cfg in the DLL directory and environment variables.

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global state
static HHOOK g_keyboardHook = NULL;
static bool g_disableLayoutPopup = true;
static bool g_disableAsiCheck = false;
static bool g_disableKeyboardRewrites = false;

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
    strncat(configPath, "\\um.cfg", sizeof(configPath) - strlen(configPath) - 1);

    FILE* file = fopen(configPath, "r");
    if (!file) {
        // Create um.cfg if it doesn't exist with default settings
        file = fopen(configPath, "w");
        if (file) {
            fprintf(file, "; Universal Mod Configuration\n");
            fprintf(file, "; Set to true to enable, false to disable\n\n");
            fprintf(file, "UM_DISABLE_KEYBOARD_LAYOUT_POPUP=true\n");
            fprintf(file, "UM_DISABLE_SPELLADDON_ASI_CHECK=false\n");
            fprintf(file, "UM_DISABLE_KEYBOARD_REWRITES=false\n");
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
        if (EqualsIgnoreCase(key, "UM_DISABLE_KEYBOARD_LAYOUT_POPUP")) {
            g_disableLayoutPopup = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "UM_DISABLE_SPELLADDONX_ASI_CHECK") || EqualsIgnoreCase(key, "UM_DISABLE_SPELLADDON_ASI_CHECK")) {
            g_disableAsiCheck = IsTrueString(value);
        } else if (EqualsIgnoreCase(key, "UM_DISABLE_KEYBOARD_REWRITES")) {
            g_disableKeyboardRewrites = IsTrueString(value);
        }
    }

    fclose(file);
}

// Synthesize a US-QWERTY backtick press.
static void SendQwertyBacktickPress() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = 0x29; // us qwerty backtick/tilde key scan code
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = 0x29;
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

// Synthesize a US-QWERTY number-row press.
static void SendQwertyNumberKeyPress(BYTE vkCode) {
    // map vk codes 48-57 (0-9) to scan codes 0x02-0x0b (1-0)
    BYTE scanCodeMap[] = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    if (vkCode < 0x30 || vkCode > 0x39) {
        return;
    }
    BYTE scanCode = scanCodeMap[vkCode - 0x30];

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = scanCode;
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = scanCode;
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

// Intercept the backtick and number-row keys and rewrite them as US-QWERTY presses.
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_disableKeyboardRewrites) {
        return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
    }

    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb) {
            if (kb->vkCode == 0xC0) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    SendQwertyBacktickPress();
                }
                return 1; // swallow the original key event
            }
            if (kb->vkCode >= 0x30 && kb->vkCode <= 0x39) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    SendQwertyNumberKeyPress(kb->vkCode);
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

    g_disableLayoutPopup = g_disableLayoutPopup || GetEnvironmentFlag("UM_DISABLE_KEYBOARD_LAYOUT_POPUP");
    g_disableAsiCheck = g_disableAsiCheck || GetEnvironmentFlag("UM_DISABLE_SPELLADDONX_ASI_CHECK")
        || GetEnvironmentFlag("UM_DISABLE_SPELLADDON_ASI_CHECK");
    g_disableKeyboardRewrites = g_disableKeyboardRewrites || GetEnvironmentFlag("UM_DISABLE_KEYBOARD_REWRITES");

    HANDLE threadHandle = CreateThread(NULL, 0, KeyPopupThread, hModule, 0, NULL);
    if (threadHandle) {
        CloseHandle(threadHandle);
    }

    if (!g_disableLayoutPopup) {
        char keyboardLayoutName[KL_NAMELENGTH] = {};
        if (GetKeyboardLayoutNameA(keyboardLayoutName) != 0) {
            size_t layoutLen = strlen(keyboardLayoutName);
            if (layoutLen >= 3 && strcmp(keyboardLayoutName + layoutLen - 3, "40C") == 0) {
                char layoutMessage[256] = {};
                snprintf(layoutMessage, sizeof(layoutMessage),
                    "Detected keyboard layout: %s.\nThe keyboard input hook will synthesize a US QWERTY backtick on the physical backtick key. This warning can be disabled in um.cfg (UM_DISABLE_KEYBOARD_LAYOUT_POPUP).",
                    keyboardLayoutName);
                MessageBoxA(NULL,
                    layoutMessage,
                    "Keyboard Layout Warning",
                    MB_ICONWARNING | MB_OK);
            }
        }
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

    if (!g_disableAsiCheck) {
        char asiPath[MAX_PATH] = {};
        snprintf(asiPath, sizeof(asiPath), "%s\\SpellAddonX.asi", gameDir);
        if (GetFileAttributesA(asiPath) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxA(NULL,
                "SpellAddonX.asi is missing and required to run the game.",
                "Missing Required File",
                MB_ICONERROR | MB_OK);
            ExitProcess(1);
        }
    }

    return TRUE;
}
