// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The system calls of the launcher (Windows, which includes Wine; a few stand-ins elsewhere so the program also builds and its screens can be
// looked at on Linux: starting the game needs the registry setup of Windows / Wine, so there it only reports that).
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace launcher::platform {

namespace fs = std::filesystem;

inline fs::path ExeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return fs::path(buf).parent_path();
#else
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? fs::path(std::string(buf, static_cast<size_t>(n))).parent_path() : fs::current_path();
#endif
}

// Opens a file or a web address with the default program.
// A message the user sees when the launcher cannot even open its window (a GUI program has no console, so stderr is lost). Windows only; elsewhere stderr.
inline void ShowFatal(const std::string& text) {
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), "EIStarter-UM", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", text.c_str());
#endif
}

inline void OpenWithDefault(const std::string& target) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, &w[0], n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '" + target + "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str())) {}
#endif
}

inline void PlayWav(const std::string& path) {
#ifdef _WIN32
    PlaySoundA(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#else
    (void)path;
#endif
}

// A folder chooser; `path` is the start folder and receives the choice. False when cancelled or unavailable.
inline bool PickFolder(std::string& path) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the new-style dialog needs COM
    struct Cb { static int CALLBACK Init(HWND h, UINT msg, LPARAM, LPARAM data) { if (msg == BFFM_INITIALIZED && data) SendMessageA(h, BFFM_SETSELECTIONA, TRUE, data); return 0; } };
    std::string start = path;
    while (start.size() > 3 && (start.back() == '\\' || start.back() == '/')) start.pop_back(); // the dialog wants no trailing slash
    BROWSEINFOA bi = {};
    bi.lpfn = Cb::Init;
    bi.lParam = reinterpret_cast<LPARAM>(start.c_str());
    char name[MAX_PATH] = {};
    bi.pszDisplayName = name;
    bi.lpszTitle = "Evil Islands folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    if (!id) return false;
    char out[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListA(id, out) != 0;
    CoTaskMemFree(id);
    if (ok) path = out;
    return ok;
#else
    FILE* p = popen("zenity --file-selection --directory 2>/dev/null || kdialog --getexistingdirectory 2>/dev/null", "r");
    if (!p) return false;
    char buf[4096] = {};
    const bool got = std::fgets(buf, sizeof buf, p) != nullptr;
    pclose(p);
    if (!got) return false;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty()) return false;
    path = s;
    return true;
#endif
}

// Display adapters and the resolutions the desktop's driver offers ("1920x1080").
inline std::vector<std::string> Adapters() {
    std::vector<std::string> out;
#ifdef _WIN32
    DISPLAY_DEVICEA dd = {};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i, dd.cb = sizeof dd) {
        bool have = false;
        for (const std::string& s : out) have = have || s == dd.DeviceString;
        if (!have) out.push_back(dd.DeviceString);
    }
#endif
    return out;
}
inline std::vector<std::string> Resolutions() {
    std::vector<std::string> out;
#ifdef _WIN32
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); ++i) {
        const std::string s = std::to_string(dm.dmPelsWidth) + "x" + std::to_string(dm.dmPelsHeight);
        bool have = false;
        for (const std::string& o : out) have = have || o == s;
        if (!have) out.push_back(s);
    }
#else
    out = {"800x600", "1024x768", "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440"};
#endif
    return out;
}

// The game process.
struct Process {
#ifdef _WIN32
    HANDLE handle = nullptr;
    DWORD pid = 0;
#endif
    bool started = false;
};

inline bool Start(const fs::path& exe, const fs::path& workDir, Process& p, std::string& error) {
#ifdef _WIN32
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::wstring dir = workDir.wstring();
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        error = "CreateProcess failed, error " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    p.handle = pi.hProcess; p.pid = pi.dwProcessId; p.started = true;
    return true;
#else
    (void)exe; (void)workDir; (void)p;
    error = "Starting the game needs Windows or Wine: run EIStarter-UM.exe there.";
    return false;
#endif
}

inline bool Running(Process& p, int& exitCode) {
#ifdef _WIN32
    if (!p.handle) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(p.handle, &code) || code == STILL_ACTIVE) return true;
    exitCode = static_cast<int>(code);
    return false;
#else
    (void)p; exitCode = 0; return false;
#endif
}

// Whether the game process has a visible window yet (it is up: the launcher can leave).
inline bool HasWindow(const Process& p) {
#ifdef _WIN32
    struct Ctx { DWORD pid; bool found; } ctx = {p.pid, false};
    struct Cb {
        static BOOL CALLBACK Each(HWND h, LPARAM l) {
            Ctx* c = reinterpret_cast<Ctx*>(l);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && IsWindowVisible(h)) { c->found = true; return FALSE; }
            return TRUE;
        }
    };
    EnumWindows(Cb::Each, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
#else
    (void)p; return false;
#endif
}

// Runs a mod's script (the Info window's plugin button) through the command interpreter.
inline void RunScript(const fs::path& script) {
#ifdef _WIN32
    const std::wstring params = L"/C \"" + script.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), script.parent_path().wstring().c_str(), SW_SHOWNORMAL);
#else
    OpenWithDefault(script.string());
#endif
}

} // namespace launcher::platform
