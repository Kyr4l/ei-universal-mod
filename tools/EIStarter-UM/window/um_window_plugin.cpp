// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um-window.asi: the BORDERLESS feature of um.dll (um_window.cpp) as a plugin of its own, for mods that do not use um.dll. addon.dll loads
// the *.asi files of Engine\. EIStarter-UM writes Engine\um-window.ini ([Window] Borderless=1, Scale=1) when "Borderless window" is chosen in its Options,
// and [Renderer] D7VK=<path> when its renderer choice is D7VK for such a mod. When um.dll is loaded it does the same jobs (um.cfg BORDERLESS,
// the launcher's renderer through the environment), so this one stays out of the way.
#ifndef UM_WINDOW_STANDALONE
#define UM_WINDOW_STANDALONE
#endif
#include "um_window_standalone.hpp"

static char g_dir[MAX_PATH];

static void PluginLog(const char* level, const char* text) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sum-window.log", g_dir);
    if (FILE* f = fopen(path, "a")) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, level, text);
        fclose(f);
    }
}

void LogLine(const char* level, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    PluginLog(level, text);
}

// The import patch (um_hooks.cpp's): replaces `functionName` in the import tables of `module` and returns the original.
bool PatchImportedFunction(HMODULE module, const char* functionName, ULONG_PTR replacement, ULONG_PTR* original) {
    if (!module || !functionName || !replacement || !original) return false;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return false;
    bool patched = false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
        IMAGE_THUNK_DATA* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        IMAGE_THUNK_DATA* names = imp->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : addresses;
        for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(name->Name), functionName) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) continue;
            if (!patched) *original = static_cast<ULONG_PTR>(addresses->u1.Function);
            addresses->u1.Function = replacement;
            VirtualProtect(&addresses->u1.Function, sizeof(ULONG_PTR), old, &old);
            patched = true;
        }
    }
    return patched;
}

// The renderer EIStarter-UM chose for a mod without um.dll ([Renderer] D7VK=<path of d7vk.dll> in um-window.ini; empty = the game's own DirectDraw):
// the game's DirectDraw calls go to that file, loaded at the first call (not inside DllMain). If it cannot be loaded or cannot create its
// DirectDraw (no Vulkan), the call goes to the game's own ddraw.dll. For a mod with um.dll the launcher leaves the key empty: um.dll loads D7VK.
typedef HRESULT (WINAPI *DirectDrawCreateFn)(GUID*, void**, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawCreateExFn2)(GUID*, void**, REFIID, IUnknown*);
static char g_d7vkPath[MAX_PATH];
static DirectDrawCreateFn g_gameCreate, g_d7vkCreate;
static DirectDrawCreateExFn2 g_gameCreateEx, g_d7vkCreateEx;

static bool LoadD7vk() {
    static int state = 0; // 0 not tried, 1 loaded, 2 failed
    if (state) return state == 1;
    state = 2;
    HMODULE module = LoadLibraryExA(g_d7vkPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { LogLine("ERROR", "renderer: %s could not be loaded (error %lu); the game's own DirectDraw is used", g_d7vkPath, GetLastError()); return false; }
    FARPROC create = GetProcAddress(module, "DirectDrawCreate"), createEx = GetProcAddress(module, "DirectDrawCreateEx");
    if (!create || !createEx) { LogLine("ERROR", "renderer: %s lacks DirectDrawCreate / DirectDrawCreateEx; the game's own DirectDraw is used", g_d7vkPath); return false; }
    g_d7vkCreate = reinterpret_cast<DirectDrawCreateFn>(reinterpret_cast<void*>(create));
    g_d7vkCreateEx = reinterpret_cast<DirectDrawCreateExFn2>(reinterpret_cast<void*>(createEx));
    LogLine("INFO", "renderer: DirectDraw comes from %s", g_d7vkPath);
    state = 1;
    return true;
}

static HRESULT WINAPI RendererDirectDrawCreate(GUID* guid, void** directDraw, IUnknown* outer) {
    if (LoadD7vk()) {
        const HRESULT r = g_d7vkCreate(guid, directDraw, outer);
        if (SUCCEEDED(r)) return r;
        LogLine("ERROR", "renderer: D7VK's DirectDrawCreate failed (0x%08lX); the game's own DirectDraw is used", static_cast<unsigned long>(r));
    }
    return g_gameCreate(guid, directDraw, outer);
}

static HRESULT WINAPI RendererDirectDrawCreateEx(GUID* guid, void** directDraw, REFIID iid, IUnknown* outer) {
    if (LoadD7vk()) {
        const HRESULT r = g_d7vkCreateEx(guid, directDraw, iid, outer);
        if (SUCCEEDED(r)) return r;
        LogLine("ERROR", "renderer: D7VK's DirectDrawCreateEx failed (0x%08lX); the game's own DirectDraw is used", static_cast<unsigned long>(r));
    }
    return g_gameCreateEx(guid, directDraw, iid, outer);
}

static void InstallRenderer(const char* ini) {
    GetPrivateProfileStringA("Renderer", "D7VK", "", g_d7vkPath, sizeof g_d7vkPath, ini);
    if (!g_d7vkPath[0]) return;
    HMODULE process = GetModuleHandleA(NULL);
    ULONG_PTR create = 0, createEx = 0;
    const bool a = PatchImportedFunction(process, "DirectDrawCreate", reinterpret_cast<ULONG_PTR>(RendererDirectDrawCreate), &create);
    const bool b = PatchImportedFunction(process, "DirectDrawCreateEx", reinterpret_cast<ULONG_PTR>(RendererDirectDrawCreateEx), &createEx);
    g_gameCreate = reinterpret_cast<DirectDrawCreateFn>(create);
    g_gameCreateEx = reinterpret_cast<DirectDrawCreateExFn2>(createEx);
    if (a || b) PluginLog("INFO", "um-window.asi: the game's DirectDraw calls go to D7VK"); // game.exe imports only one of the two
    else PluginLog("WARN", "um-window.asi: could not hook DirectDrawCreate / DirectDrawCreateEx: the game's own DirectDraw is used");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameA(instance, g_dir, sizeof g_dir);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = '\0'; else g_dir[0] = '\0';
    if (GetModuleHandleA("um.dll")) return TRUE; // um.dll's BORDERLESS and DDRAW_DLL do it
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sum-window.ini", g_dir);
    InstallRenderer(ini); // before the window hooks: the scale hook then wraps this one
    if (GetPrivateProfileIntA("Window", "Borderless", 0, ini) == 0) return TRUE;
    g_enableBorderless = true;
    g_enableScale = GetPrivateProfileIntA("Window", "Scale", 0, ini) != 0; // the monitor-sized, letterboxed window (um_window.cpp)
    InstallWindowHooks();
    PluginLog("INFO", "um-window.asi loaded: borderless window on");
    return TRUE;
}
