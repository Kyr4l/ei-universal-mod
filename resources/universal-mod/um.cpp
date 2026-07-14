// this file provides a small dll that hooks low-level keyboard events
// and fakes a us qwerty backtick/tilde key press when the physical
// backtick key (vk code 0xc0) is detected. it also checks for a
// required .asi file next to the game executable on dll attach.

#include <windows.h>
#include <string>

// global hook handle for the low-level keyboard hook
static HHOOK g_keyboardHook = NULL;

// synthesize a physical press and release of the us qwerty backtick
// (tilde) key using scancode 0x29. this sends two inputs: keydown
// then keyup, using hardware scancodes so the keyboard layout is
// interpreted as if it were a physical us qwerty keystroke.
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

// low-level keyboard proc called on each keyboard event. when the
// hook reports an action and the virtual-key code matches 0xc0
// (the backtick key), we inject a us qwerty backtick press and
// return 1 to swallow the original event. otherwise we pass the
// event along with callnexthookex.
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb && kb->vkCode == 0xC0) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                SendQwertyBacktickPress();
            }
            return 1; // swallow the original key event
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// thread that installs the low-level keyboard hook and runs a
// simple message loop so the hook stays active. the module handle
// passed in is forwarded to setwindowshookexw.
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

// dll entry point. on process attach we spawn the thread that
// installs the keyboard hook and then check for spelladdonx.asi
// next to the game executable. if the .asi is missing we show an
// error message and terminate the process with exitcode 1.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    CreateThread(NULL, 0, KeyPopupThread, NULL, 0, NULL);

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // compute the directory of the executable by removing the last
    // backslash and filename from the full path
    std::string gameDir(exePath);
    size_t lastSlash = gameDir.find_last_of('\\');
    if (lastSlash != std::string::npos) {
        gameDir.resize(lastSlash);
    }

    // build the expected .asi path and verify it exists
    std::string asiPath = gameDir + "\\SpellAddonX.asi";
    if (GetFileAttributesA(asiPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL,
            "SpellAddonX.asi is missing and required to run the game.",
            "Missing Required File",
            MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }

    return TRUE;
}
