// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um-window.asi: the BORDERLESS feature of um.dll (um_window.cpp) as a plugin of its own, for mods that do not use um.dll. addon.dll loads
// the *.asi files of Engine\. EIStarter-UM writes Engine\um-window.ini ([Window] Borderless=1) when "Borderless window" is chosen in its Options.
// When um.dll is loaded it does the same job (um.cfg BORDERLESS), so this one stays out of the way.
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameA(instance, g_dir, sizeof g_dir);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = '\0'; else g_dir[0] = '\0';
    if (GetModuleHandleA("um.dll")) return TRUE; // um.dll's BORDERLESS does it
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sum-window.ini", g_dir);
    if (GetPrivateProfileIntA("Window", "Borderless", 0, ini) == 0) return TRUE;
    g_enableBorderless = true;
    InstallWindowHooks();
    PluginLog("INFO", "um-window.asi loaded: borderless window on");
    return TRUE;
}
