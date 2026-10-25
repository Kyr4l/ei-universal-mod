// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: BORDERLESS - the windowed game behaves like a borderless full screen window (RE: App_WndProc 0x457D40).
// The game's window procedure treats every WM_ACTIVATE with WA_INACTIVE (an alt-tab, a click on another window) as a loss of the
// screen: it marks itself inactive (the main loop then stops ticking: a multiplayer game desyncs), releases the DirectInput mouse,
// pauses all sound and minimizes its window (ShowWindow SW_MINIMIZE), even in windowed mode. It also never confines the cursor
// to its window, so the camera cannot be scrolled with the mouse at the screen edge. Here the game's window is subclassed:
//   - the deactivation never reaches the game (it stays active, does not minimize and keeps running in the background);
//   - while the window has the focus the cursor is confined to it (ClipCursor), and released when it loses the focus; a timer (4 / s) confines it again when something took the confinement away.
// Needs the game's own setting fullscreen=0 (the window is a plain WS_POPUP at 0,0 of the game's resolution).
// SCALE (um.cfg BORDERLESS_SCALE / um-window.ini Scale=1): a game resolution lower than the monitor's fills the screen. The window gets the size of the game
// picture scaled by the largest factor that fits the monitor (aspect ratio kept), centred, with a black window behind it for the bars. D7VK stretches its frame to
// the window by itself; with any other DirectDraw the primary surface's Blt / BltFast are turned into stretching Blt calls. The game takes its mouse position
// from GetCursorPos / SetCursorPos and from the position in the mouse messages, all as if the window sat at 0,0 at the game's size: those are mapped.
// Self-contained: only LogLine and the import patch of CreateWindowExA are shared with the rest of um.dll. um-launcher builds this file
// on its own (UM_WINDOW_STANDALONE, tools/um-launcher/window/) as um-window.asi for mods that do not use um.dll.
#ifdef UM_WINDOW_STANDALONE
#include "um_window_standalone.hpp"
#else
#include "um_internal.hpp"
#endif
#include <algorithm>
#include "um_cursor_map.hpp"

bool g_enableBorderless = false;
bool g_enableScale = false; // SCALE
static int g_gameW = 0, g_gameH = 0;     // the game's own window size (its resolution)
static int g_winX = 0, g_winY = 0;       // the scaled window's position on the screen
static double g_scale = 1.0;             // the scaled window's size / the game's
static bool g_scaleActive = false;
static CursorMap g_cursor;               // GetCursorPos / SetCursorPos mapping
static CRITICAL_SECTION g_cursorLock;
static bool g_cursorLockReady = false;
static RECT g_monitorRect = {0, 0, 0, 0}, g_gameRect = {0, 0, 0, 0};
static HWND g_bars[4] = {NULL, NULL, NULL, NULL}; // the black bars around the scaled window (they never overlap it)
static bool g_scaleByBlt = false;        // the DirectDraw does not stretch by itself (not D7VK): the primary surface's Blt / BltFast do

static HWND g_gameWindow = NULL;
static WNDPROC g_originalProc = NULL;
static bool g_clipped = false;

static bool WindowClientOnScreen(RECT& screen) {
    RECT client;
    if (!g_gameWindow || !GetClientRect(g_gameWindow, &client)) return false;
    POINT a = {client.left, client.top}, b = {client.right, client.bottom};
    ClientToScreen(g_gameWindow, &a); ClientToScreen(g_gameWindow, &b);
    screen.left = a.x; screen.top = a.y; screen.right = b.x; screen.bottom = b.y;
    return true;
}

static void WindowClip(bool on) {
    if (!g_gameWindow) return;
    if (!on) {
        if (g_clipped) { ClipCursor(NULL); g_clipped = false; }
        return;
    }
    RECT screen;
    if (!WindowClientOnScreen(screen)) return;
    if (ClipCursor(&screen)) g_clipped = true;
}

// The confinement is also checked on a timer (four times a second): the system drops it by itself (an overlay, another program, a desktop switch, a focus change that sends no
// message to the game) and a ClipCursor made before the window had its final place or its X focus (Wine) can fail or be wrong; none of that sends the game window a message.
// While the game window is the foreground window and the cursor is not confined to it, it is confined again; when the game is not in front, a confinement of ours is released.
static const UINT_PTR kClipWatchTimer = 0x5CA20;
static void WindowClipWatch() {
    if (!g_gameWindow) return;
    if (GetForegroundWindow() != g_gameWindow || IsIconic(g_gameWindow)) {
        if (g_clipped) WindowClip(false);
        return;
    }
    RECT want, now;
    if (!WindowClientOnScreen(want)) return;
    if (GetClipCursor(&now) && EqualRect(&now, &want)) { g_clipped = true; return; }
    g_clipped = false;
    const bool ok = ClipCursor(&want) != 0;
    g_clipped = ok;
    static DWORD lastLog = 0;
    const DWORD tick = GetTickCount();
    if (tick - lastLog > 30000) {
        lastLog = tick;
        LogLine(ok ? "INFO" : "WARN", "BORDERLESS: the cursor was free while the game window has the focus: %s", ok ? "confined to it again" : "ClipCursor failed");
    }
}

static void CreateBars(const RECT& monitor, const RECT& game);
static bool g_topmost = false;
// The scaled picture covers the whole monitor height or width; the taskbar is topmost and would cover its edge: the game window and the bars are topmost while the game is active only.
static void SetScaleTopmost(bool on) {
    if (!g_scaleActive || g_topmost == on) return;
    g_topmost = on;
    LogLine("INFO", "SCALE: the window is %s", on ? "topmost (the game is active)" : "no longer topmost (the game lost the focus)");
    HWND after = on ? HWND_TOPMOST : HWND_NOTOPMOST;
    for (HWND bar : g_bars) if (bar) SetWindowPos(bar, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_gameWindow) SetWindowPos(g_gameWindow, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void DestroyBars() {
    for (HWND& bar : g_bars) if (bar) { DestroyWindow(bar); bar = NULL; }
}

// Screen -> game coordinates (the game's window is taken to sit at 0,0 at its own size), for positions that are not the pointer's (the wheel message's).
static void ScaleToGame(LONG& x, LONG& y) {
    x = static_cast<LONG>((x - g_winX) / g_scale); y = static_cast<LONG>((y - g_winY) / g_scale);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_gameW - 1) x = g_gameW - 1;
    if (y > g_gameH - 1) y = g_gameH - 1;
}

static BOOL (WINAPI *g_originalGetCursorPos)(LPPOINT) = NULL;
static BOOL (WINAPI *g_originalSetCursorPos)(int, int) = NULL;
static BOOL WINAPI HookedGetCursorPos(LPPOINT p) {
    BOOL ok = g_originalGetCursorPos(p);
    if (ok && p && g_scaleActive && g_cursorLockReady) {
        long gx, gy;
        EnterCriticalSection(&g_cursorLock);
        g_cursor.ToGame(p->x, p->y, gx, gy);
        LeaveCriticalSection(&g_cursorLock);
        p->x = gx; p->y = gy;
    }
    return ok;
}
static BOOL WINAPI HookedSetCursorPos(int x, int y) {
    if (g_scaleActive && g_cursorLockReady) {
        long sx, sy;
        EnterCriticalSection(&g_cursorLock);
        g_cursor.ToScreen(x, y, sx, sy);
        LeaveCriticalSection(&g_cursorLock);
        x = static_cast<int>(sx); y = static_cast<int>(sy);
    }
    return g_originalSetCursorPos(x, y);
}

// Maps the position of a mouse message (client coordinates of the scaled window, or screen coordinates for the wheel) to the game's coordinates.
static void MapMouseMessage(UINT message, LPARAM& lParam) {
    if ((message >= WM_MOUSEMOVE && message <= WM_MBUTTONDBLCLK && message != WM_MOUSEWHEEL) || (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK)) {
        LONG gx = static_cast<LONG>(static_cast<short>(LOWORD(lParam)) / g_scale), gy = static_cast<LONG>(static_cast<short>(HIWORD(lParam)) / g_scale);
        gx = (std::max)(0L, (std::min)(gx, static_cast<LONG>(g_gameW - 1)));
        gy = (std::max)(0L, (std::min)(gy, static_cast<LONG>(g_gameH - 1)));
        lParam = MAKELPARAM(static_cast<WORD>(gx), static_cast<WORD>(gy));
    } else if (message == WM_MOUSEWHEEL) {
        LONG gx = static_cast<short>(LOWORD(lParam)), gy = static_cast<short>(HIWORD(lParam));
        ScaleToGame(gx, gy);
        lParam = MAKELPARAM(static_cast<WORD>(gx), static_cast<WORD>(gy));
    }
}

// The game's loop takes its messages from GetMessageA / PeekMessageA and passes them to its own input handler without dispatching them to the
// window procedure: the positions are mapped here.
static BOOL (WINAPI *g_originalGetMessageA)(LPMSG, HWND, UINT, UINT) = NULL;
static BOOL (WINAPI *g_originalPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT) = NULL;
static BOOL WINAPI HookedGetMessageA(LPMSG msg, HWND window, UINT min, UINT max) {
    BOOL r = g_originalGetMessageA(msg, window, min, max);
    if (r > 0 && g_scaleActive && msg) MapMouseMessage(msg->message, msg->lParam);
    return r;
}
static BOOL WINAPI HookedPeekMessageA(LPMSG msg, HWND window, UINT min, UINT max, UINT remove) {
    BOOL r = g_originalPeekMessageA(msg, window, min, max, remove);
    if (r && (remove & PM_REMOVE) && g_scaleActive && msg) MapMouseMessage(msg->message, msg->lParam); // a peek without PM_REMOVE leaves the message queued: it is mapped when taken
    return r;
}

static LRESULT CALLBACK BorderlessProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static const bool control = getenv("UM_WINTEST_RAW") != NULL; // research: the hook installed but passing everything (the game's own behaviour)
    if (control) return CallWindowProcA(g_originalProc, window, message, wParam, lParam);
    if (g_scaleActive) { // (the mouse messages are mapped where the game takes them from its queue, GetMessageA / PeekMessageA: mapping them again here would scale them twice)
        if (message == WM_ERASEBKGND) { // black, not whatever the class brush is
            RECT r; GetClientRect(window, &r); FillRect(reinterpret_cast<HDC>(wParam), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
    }
    if (message == WM_TIMER && wParam == kClipWatchTimer) { WindowClipWatch(); return 0; }
    if (message == WM_TIMER && wParam == 0x5CA1E) { KillTimer(window, 0x5CA1E); if (g_scaleActive) CreateBars(g_monitorRect, g_gameRect); return 0; }
    switch (message) {
    case WM_ACTIVATE:
        SetScaleTopmost(LOWORD(wParam) != WA_INACTIVE);
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
        KillTimer(window, kClipWatchTimer);
        WindowClip(false);
        DestroyBars();
        g_scaleActive = false;
        g_topmost = false;
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

// The black bars: up to four plain black windows around the scaled game window.
static void CreateBars(const RECT& monitor, const RECT& game) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "UmScaleBar";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassA(&wc); registered = true;
    }
    const RECT rects[4] = {{monitor.left, monitor.top, game.left, monitor.bottom}, {game.right, monitor.top, monitor.right, monitor.bottom},
                           {game.left, monitor.top, game.right, game.top}, {game.left, game.bottom, game.right, monitor.bottom}};
    for (int i = 0; i < 4; ++i) {
        const RECT& r = rects[i];
        if (r.right - r.left <= 0 || r.bottom - r.top <= 0) continue;
        g_bars[i] = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "UmScaleBar", "", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                    NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (g_bars[i]) { ShowWindow(g_bars[i], SW_SHOWNOACTIVATE); if (g_topmost) SetWindowPos(g_bars[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    }
}

static HWND WINAPI HookedCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                         HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    const bool gameWindow = !g_gameWindow && !parent && (style & WS_POPUP) && w >= 640 && h >= 480; // the game's main window
    if (gameWindow && g_enableScale) {
        POINT origin = {x, y};
        MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        RECT monitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) monitor = mi.rcMonitor;
        const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
        const double scale = (std::min)(static_cast<double>(mw) / w, static_cast<double>(mh) / h);
        if (scale > 1.001) {
            g_gameW = w; g_gameH = h; g_scale = scale;
            int nw = static_cast<int>(w * scale + 0.5), nh = static_cast<int>(h * scale + 0.5);
            if (nw > mw) nw = mw;
            if (nh > mh) nh = mh;
            g_winX = monitor.left + (mw - nw) / 2; g_winY = monitor.top + (mh - nh) / 2;
            LogLine("INFO", "SCALE: the game window %dx%d becomes %dx%d (x%.3f) at %d,%d on the %dx%d monitor", w, h, nw, nh, scale, g_winX, g_winY, mw, mh);
            g_monitorRect = monitor; g_gameRect.left = g_winX; g_gameRect.top = g_winY; g_gameRect.right = g_winX + nw; g_gameRect.bottom = g_winY + nh;
            x = g_winX; y = g_winY; w = nw; h = nh;
            g_cursor.scale = scale; g_cursor.winX = g_winX; g_cursor.winY = g_winY; g_cursor.gameW = g_gameW; g_cursor.gameH = g_gameH;
            if (!g_cursorLockReady) { InitializeCriticalSection(&g_cursorLock); g_cursorLockReady = true; }
            g_scaleActive = true;
        } else LogLine("INFO", "SCALE: the game window %dx%d is not smaller than the %dx%d monitor: nothing to scale", w, h, mw, mh);
    }
    HWND window = g_originalCreateWindowExA(exStyle, className, title, style, x, y, w, h, parent, menu, instance, param);
    if (window && gameWindow) {
        g_gameWindow = window;
        g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongA(window, GWL_WNDPROC, reinterpret_cast<LONG>(BorderlessProc)));
        LogLine("INFO", "BORDERLESS: the game window %dx%d at %d,%d is kept active when it loses the focus, and the cursor is confined to it while it has it", w, h, x, y);
        SetTimer(window, kClipWatchTimer, 250, NULL);
        if (g_scaleActive) SetTimer(window, 0x5CA1E, 1500, NULL); // the bars after the game window has been shown
        if (getenv("UM_WINTEST")) CreateThread(NULL, 0, WindowTestThread, NULL, 0, NULL);
    }
    return window;
}

// ---- DirectDraw, for the DirectDraw that does not stretch by itself ----
// IDirectDraw7 slots: CreateSurface 6, SetCooperativeLevel 20. IDirectDrawSurface7 slots: Blt 5, BltFast 7, GetSurfaceDesc 22.
typedef HRESULT (WINAPI *CreateSurfaceFn)(void*, void*, void**, IUnknown*);
typedef HRESULT (WINAPI *SetCoopFn)(void*, HWND, DWORD);
typedef HRESULT (WINAPI *BltFn)(void*, RECT*, void*, RECT*, DWORD, void*);
typedef HRESULT (WINAPI *BltFastFn)(void*, DWORD, DWORD, void*, RECT*, DWORD);
typedef HRESULT (WINAPI *GetSurfaceDescFn)(void*, void*);
static CreateSurfaceFn g_originalCreateSurface = NULL;
static SetCoopFn g_originalSetCoop = NULL;
static BltFn g_originalBlt = NULL;
static BltFastFn g_originalBltFast = NULL;
static void* g_primary = NULL;

// Writes `replacement` into a vtable slot of the object's class and returns what was there (the previous hook's, if any).
static void* WindowPatchSlot(void* object, int slot, void* replacement) {
    if (!object || IsBadReadPtr(object, sizeof(void*))) return NULL;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!vtable || IsBadReadPtr(vtable + slot, sizeof(void*))) return NULL;
    DWORD old = 0;
    if (!VirtualProtect(vtable + slot, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
    void* original = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(vtable + slot, sizeof(void*), old, &old);
    return original;
}

static void ScaleRectToScreen(const RECT& in, RECT& out) {
    out.left = g_winX + static_cast<LONG>(in.left * g_scale + 0.5); out.top = g_winY + static_cast<LONG>(in.top * g_scale + 0.5);
    out.right = g_winX + static_cast<LONG>(in.right * g_scale + 0.5); out.bottom = g_winY + static_cast<LONG>(in.bottom * g_scale + 0.5);
}

static HRESULT WINAPI HookedPrimaryBlt(void* self, RECT* dest, void* src, RECT* srcRect, DWORD flags, void* fx) {
    if (self != g_primary || !g_scaleActive || !g_scaleByBlt) return g_originalBlt(self, dest, src, srcRect, flags, fx);
    RECT full = {0, 0, g_gameW, g_gameH}, scaled;
    ScaleRectToScreen(dest ? *dest : full, scaled);
    return g_originalBlt(self, &scaled, src, srcRect, flags, fx);
}

static HRESULT WINAPI HookedPrimaryBltFast(void* self, DWORD x, DWORD y, void* src, RECT* srcRect, DWORD trans) {
    if (self != g_primary || !g_scaleActive || !g_scaleByBlt || !src) return g_originalBltFast(self, x, y, src, srcRect, trans);
    RECT whole = {0, 0, 0, 0};
    if (!srcRect) { // the whole source: its size from the surface description (DDSURFACEDESC2: height at +8, width at +12)
        BYTE desc[124]; memset(desc, 0, sizeof desc); *reinterpret_cast<DWORD*>(desc) = sizeof desc;
        void** vtable = *reinterpret_cast<void***>(src);
        if (FAILED(reinterpret_cast<GetSurfaceDescFn>(vtable[22])(src, desc))) return g_originalBltFast(self, x, y, src, srcRect, trans);
        whole.right = static_cast<LONG>(*reinterpret_cast<DWORD*>(desc + 12)); whole.bottom = static_cast<LONG>(*reinterpret_cast<DWORD*>(desc + 8));
        srcRect = &whole;
    }
    RECT dest = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + (srcRect->right - srcRect->left), static_cast<LONG>(y) + (srcRect->bottom - srcRect->top)}, scaled;
    ScaleRectToScreen(dest, scaled);
    DWORD flags = 0x01000000 /* DDBLT_WAIT */;
    if (trans & 1) flags |= 0x8000;  // DDBLTFAST_SRCCOLORKEY -> DDBLT_KEYSRC
    if (trans & 2) flags |= 0x2000;  // DDBLTFAST_DESTCOLORKEY -> DDBLT_KEYDEST
    return g_originalBlt(self, &scaled, src, srcRect, flags, NULL);
}

static HRESULT WINAPI HookedCreateSurfaceScale(void* self, void* desc, void** surface, IUnknown* outer) {
    HRESULT result = g_originalCreateSurface(self, desc, surface, outer);
    if (SUCCEEDED(result) && surface && *surface && desc && g_scaleByBlt) {
        const DWORD flags = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(desc) + 4), caps = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(desc) + 0x68); // DDSURFACEDESC2: ddsCaps at 0x68
        if ((flags & 0x1 /* DDSD_CAPS */) && (caps & 0x200 /* DDSCAPS_PRIMARYSURFACE */)) {
            g_primary = *surface; // a primary surface the game creates again (a lost surface, a mode change) replaces the old one
            if (!g_originalBlt) { // the vtable (shared by the class's surfaces) is patched once
                g_originalBlt = reinterpret_cast<BltFn>(WindowPatchSlot(g_primary, 5, reinterpret_cast<void*>(HookedPrimaryBlt)));
                g_originalBltFast = reinterpret_cast<BltFastFn>(WindowPatchSlot(g_primary, 7, reinterpret_cast<void*>(HookedPrimaryBltFast)));
                LogLine("INFO", "SCALE: the primary surface's Blt / BltFast stretch the frame to the scaled window");
            }
        }
    }
    return result;
}

// Exclusive full screen has no window to scale: leave the game at its own size.
static HRESULT WINAPI HookedSetCoopScale(void* self, HWND window, DWORD flags) {
    if ((flags & 0x10 /* DDSCL_EXCLUSIVE */) && g_scaleActive) {
        g_scaleActive = false;
        LogLine("WARN", "SCALE: the game asked for exclusive full screen (fullscreen=1 in its settings): scaling is off");
        if (g_gameWindow) SetWindowPos(g_gameWindow, NULL, 0, 0, g_gameW, g_gameH, SWP_NOZORDER | SWP_NOACTIVATE);
        DestroyBars();
    }
    return g_originalSetCoop(self, window, flags);
}

// Called with every new DirectDraw object the game creates (by um.dll's hook, or by the asi's own below).
void WindowScaleDirectDrawCreated(void* directDraw, bool stretchesByItself) {
    if (!g_scaleActive || !directDraw || g_originalCreateSurface) return;
    g_scaleByBlt = !stretchesByItself;
    g_originalSetCoop = reinterpret_cast<SetCoopFn>(WindowPatchSlot(directDraw, 20, reinterpret_cast<void*>(HookedSetCoopScale)));
    if (g_scaleByBlt) g_originalCreateSurface = reinterpret_cast<CreateSurfaceFn>(WindowPatchSlot(directDraw, 6, reinterpret_cast<void*>(HookedCreateSurfaceScale)));
    else g_originalCreateSurface = reinterpret_cast<CreateSurfaceFn>(-1); // "installed": nothing else to hook
    LogLine("INFO", "SCALE: %s", g_scaleByBlt ? "this DirectDraw does not stretch by itself: the present is stretched by the plugin" : "D7VK stretches the frame to the window by itself");
}

#ifdef UM_WINDOW_STANDALONE
// Without um.dll the plugin hooks the game's DirectDrawCreateEx itself, and finds out whether the DirectDraw is D7VK by its strings.
typedef HRESULT (WINAPI *DirectDrawCreateExFn)(const GUID*, void**, const GUID&, IUnknown*);
static DirectDrawCreateExFn g_originalDirectDrawCreateEx = NULL;

static bool ModuleHasText(HMODULE module, const char* text) {
    const BYTE* p = reinterpret_cast<const BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(p + dos->e_lfanew);
    const size_t n = nt->OptionalHeader.SizeOfImage, k = strlen(text);
    for (size_t i = 0; i + k < n; ++i) {
        if ((i & 0xFFF) == 0 && IsBadReadPtr(p + i, 4096 + k)) { i |= 0xFFF; continue; } // an unreadable page (a discardable section): skip it
        if (p[i] == static_cast<BYTE>(text[0]) && memcmp(p + i, text, k) == 0) return true;
    }
    return false;
}

static HRESULT WINAPI HookedDirectDrawCreateExScale(const GUID* guid, void** directDraw, const GUID& iid, IUnknown* outer) {
    HRESULT result = g_originalDirectDrawCreateEx(guid, directDraw, iid, outer);
    if (SUCCEEDED(result) && directDraw && *directDraw && g_scaleActive) {
        HMODULE ddraw = GetModuleHandleA("ddraw.dll");
        WindowScaleDirectDrawCreated(*directDraw, ddraw && (ModuleHasText(ddraw, "D7VK") || ModuleHasText(ddraw, "DXVK")));
    }
    return result;
}
#endif

// A DPI-unaware process gets scaled-down monitor sizes and a blurry bitmap-stretched window on a scaled display: the scaling needs the real pixels.
static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    FARPROC aware = user32 ? GetProcAddress(user32, "SetProcessDPIAware") : NULL; // Vista and newer
    if (aware) reinterpret_cast<BOOL (WINAPI*)()>(aware)();
}

void InstallWindowHooks() {
    if (!g_enableBorderless) return;
    if (g_enableScale) EnableDpiAwareness();
    HMODULE process = GetModuleHandleA(NULL);
    if (!process || !PatchImportedFunction(process, "CreateWindowExA", reinterpret_cast<ULONG_PTR>(HookedCreateWindowExA), reinterpret_cast<ULONG_PTR*>(&g_originalCreateWindowExA)))
        LogLine("WARN", "BORDERLESS: could not hook CreateWindowExA");
    if (g_enableScale && process) {
        PatchImportedFunction(process, "GetCursorPos", reinterpret_cast<ULONG_PTR>(HookedGetCursorPos), reinterpret_cast<ULONG_PTR*>(&g_originalGetCursorPos));
        PatchImportedFunction(process, "GetMessageA", reinterpret_cast<ULONG_PTR>(HookedGetMessageA), reinterpret_cast<ULONG_PTR*>(&g_originalGetMessageA));
        PatchImportedFunction(process, "PeekMessageA", reinterpret_cast<ULONG_PTR>(HookedPeekMessageA), reinterpret_cast<ULONG_PTR*>(&g_originalPeekMessageA));
        PatchImportedFunction(process, "SetCursorPos", reinterpret_cast<ULONG_PTR>(HookedSetCursorPos), reinterpret_cast<ULONG_PTR*>(&g_originalSetCursorPos));
#ifdef UM_WINDOW_STANDALONE
        PatchImportedFunction(process, "DirectDrawCreateEx", reinterpret_cast<ULONG_PTR>(HookedDirectDrawCreateExScale), reinterpret_cast<ULONG_PTR*>(&g_originalDirectDrawCreateEx));
#endif
    }
}
