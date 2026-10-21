// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: BORDERLESS - the windowed game behaves like a borderless full screen window (RE: App_WndProc 0x457D40).
// The game's window procedure treats every WM_ACTIVATE with WA_INACTIVE (an alt-tab, a click on another window) as a loss of the
// screen: it marks itself inactive (the main loop then stops ticking: a multiplayer game desyncs), releases the DirectInput mouse,
// pauses all sound and minimizes its window (ShowWindow SW_MINIMIZE), even in windowed mode. It also never confines the cursor
// to its window, so the camera cannot be scrolled with the mouse at the screen edge. Here the game's window is subclassed:
//   - the deactivation never reaches the game (it stays active, does not minimize and keeps running in the background);
//   - while the window has the focus the cursor is confined to it (ClipCursor), and released when it loses the focus.
// Needs the game's own setting fullscreen=0 (the window is a plain WS_POPUP at 0,0 of the game's resolution).
// Self-contained: only LogLine and the import patch of CreateWindowExA are shared with the rest of um.dll. um-launcher builds this file
// on its own (UM_WINDOW_STANDALONE, tools/um-launcher/window/) as um-window.asi for mods that do not use um.dll.
#ifdef UM_WINDOW_STANDALONE
#include "um_window_standalone.hpp"
#else
#include "um_internal.hpp"
#endif

bool g_enableBorderless = false;

static HWND g_gameWindow = NULL;
static WNDPROC g_originalProc = NULL;
static bool g_clipped = false;

static void WindowClip(bool on) {
    if (!g_gameWindow) return;
    if (!on) {
        if (g_clipped) { ClipCursor(NULL); g_clipped = false; }
        return;
    }
    RECT client;
    if (!GetClientRect(g_gameWindow, &client)) return;
    POINT a = {client.left, client.top}, b = {client.right, client.bottom};
    ClientToScreen(g_gameWindow, &a); ClientToScreen(g_gameWindow, &b);
    RECT screen = {a.x, a.y, b.x, b.y};
    if (ClipCursor(&screen)) g_clipped = true;
}

static LRESULT CALLBACK BorderlessProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static const bool control = getenv("UM_WINTEST_RAW") != NULL; // research: the hook installed but passing everything (the game's own behaviour)
    if (control) return CallWindowProcA(g_originalProc, window, message, wParam, lParam);
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) { // the game must not see it
            WindowClip(false);
            return 0;
        }
        WindowClip(true);
        break;
    case WM_SETFOCUS:
        WindowClip(true);
        break;
    case WM_KILLFOCUS:
        WindowClip(false);
        break;
    case WM_SIZE:
    case WM_MOVE:
    case WM_DISPLAYCHANGE:
        if (GetForegroundWindow() == window) WindowClip(true);
        break;
    case WM_DESTROY:
        WindowClip(false);
        break;
    }
    return CallWindowProcA(g_originalProc, window, message, wParam, lParam);
}

typedef HWND (WINAPI *CreateWindowExAFunction)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static CreateWindowExAFunction g_originalCreateWindowExA = NULL;

// Research: UM_WINTEST=<seconds> sends the game's window a deactivation after that long and an activation 4 s later (what an alt-tab does).
static DWORD WINAPI WindowTestThread(LPVOID) {
    const char* seconds = getenv("UM_WINTEST");
    if (!seconds) return 0;
    Sleep(static_cast<DWORD>(atof(seconds) * 1000));
    LogLine("INFO", "BORDERLESS test: sending WM_ACTIVATE inactive to the game window");
    SendMessageA(g_gameWindow, WM_ACTIVATE, WA_INACTIVE, 0);
    Sleep(4000);
    LogLine("INFO", "BORDERLESS test: sending WM_ACTIVATE active");
    SendMessageA(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
    return 0;
}

static HWND WINAPI HookedCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                         HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    HWND window = g_originalCreateWindowExA(exStyle, className, title, style, x, y, w, h, parent, menu, instance, param);
    if (window && !g_gameWindow && !parent && (style & WS_POPUP) && w >= 640 && h >= 480) { // the game's main window
        g_gameWindow = window;
        g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongA(window, GWL_WNDPROC, reinterpret_cast<LONG>(BorderlessProc)));
        LogLine("INFO", "BORDERLESS: the game window %dx%d at %d,%d is kept active when it loses the focus, and the cursor is confined to it while it has it", w, h, x, y);
        if (getenv("UM_WINTEST")) CreateThread(NULL, 0, WindowTestThread, NULL, 0, NULL);
    }
    return window;
}

void InstallWindowHooks() {
    if (!g_enableBorderless) return;
    HMODULE process = GetModuleHandleA(NULL);
    if (!process || !PatchImportedFunction(process, "CreateWindowExA", reinterpret_cast<ULONG_PTR>(HookedCreateWindowExA), reinterpret_cast<ULONG_PTR*>(&g_originalCreateWindowExA)))
        LogLine("WARN", "BORDERLESS: could not hook CreateWindowExA");
}
