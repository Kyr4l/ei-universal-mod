// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Writes what the loader chain reads (HKCU\Software\Gipat.Ru\EI_Starter, see docs/EIStarter-UM.md) from the launcher's config file, so nobody has to
// run "Init Starter Registry.bat" or add the `path` value by hand:
//   EI_Starter\path            the launcher's folder (Engine\dinput.dll looks for addon.dll there)
//   EI_Starter\settings        AddonPath (the chosen mod's folder) and the addon.dll options
//   EI_Starter\EvilIslands     the game's settings (the clone of Nival's key that addon.dll points game.exe at)
#pragma once

#include <cstring>
#include <filesystem>
#include <string>

#include "config.hpp"
#include "mods.hpp"

namespace launcher {

// A path the way Windows writes it (backslashes, the registry's strings).
inline std::string WinPath(std::string p) {
#ifdef _WIN32
    for (char& c : p) if (c == '/') c = '\\';
#endif
    return p;
}
inline std::string WithSlash(std::string p) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += sep;
    return p;
}

// Whether `dir` looks like an Evil Islands installation (the original game's files).
inline bool LooksLikeGameFolder(const std::string& dir) {
    std::error_code ec;
    const fs::path d(dir);
    if (dir.empty() || !fs::is_directory(d, ec)) return false;
    auto has = [&](const char* name) {
        for (const fs::directory_entry& e : fs::directory_iterator(d, ec)) if (LowerAscii(e.path().filename().string()) == name) return true;
        return false;
    };
    return has("game.exe") && has("res");
}

// Whether the program runs under Wine (it has to ask Wine to use Engine\\dinput.dll, see SyncRegistry).
inline bool RunningUnderWine() {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

// Makes the path settings usable: GOG's installer writes a relative CD-ROM Path (".\\"), which the game resolves against Engine\\ and then asks for a disc;
// a missing path becomes the game folder (the original starter's convention: CD = game folder, resources = <game folder>\\res\\). A path that exists
// (the CD drive of the CD edition, D:\\) is kept. Returns true when something changed.
inline bool NormalizePaths(GameSettings& g) {
    std::error_code ec;
    bool changed = false;
    // The zone and the music the game starts with: a key imported as "none" (or emptied) leaves the game in an empty zone without music, and the launcher wrote it back at every start.
    for (const DefaultValue& d : kGameDefaults) {
        if (std::strcmp(d.key, "Game Specific Settings") != 0 || (LowerAscii(d.name) != "defaultmapname" && LowerAscii(d.name) != "defaultstreamingsound")) continue;
        const winreg::Value* v = g.Find(d.key, d.name);
        if (v && v->kind == winreg::Value::Kind::String && (v->text.empty() || LowerAscii(v->text) == "none")) { g.SetString(d.key, d.name, d.text); changed = true; }
    }
    const std::string work = g.GetString("Path Settings", "Work Path");
    if (!LooksLikeGameFolder(work)) return changed;
    auto usable = [&](const std::string& p) { return p.size() >= 2 && (p[1] == ':' || p[0] == '/') && fs::is_directory(fs::path(p), ec); };
    if (!usable(g.GetString("Path Settings", "CD-ROM Path"))) { g.SetString("Path Settings", "CD-ROM Path", WithSlash(work)); changed = true; }
    if (!usable(g.GetString("Path Settings", "Resources Path"))) { g.SetString("Path Settings", "Resources Path", WithSlash(WithSlash(work) + "res")); changed = true; }
    // The game builds its file names by appending to these paths ("<CD-ROM Path>3DfpFPU.dll", "<..>textures\\"): one without its closing backslash (typed by hand in
    // Options, or imported from another starter) makes "D:\Evil Islands3DfpFPU.dll", the game cannot load its 3D library and crashes on its missing renderer.
    for (const char* key : {"CD-ROM Path", "Resources Path", "Work Path"}) {
        const std::string v = g.GetString("Path Settings", key);
        if (!v.empty() && v != WithSlash(v)) { g.SetString("Path Settings", key, WithSlash(v)); changed = true; }
    }
    return changed;
}

struct SyncResult { bool ok = true; std::string error; };

inline SyncResult SyncRegistry(const LauncherConfig& cfg, const fs::path& launcherDir, const std::string& modFolder, void (*log)(const std::string&) = nullptr) {
    SyncResult r;
    const std::string dir = WinPath(launcherDir.string());
    if (!winreg::Set(kStarterKey, "path", winreg::Value::String(dir))) { r.ok = false; r.error = "cannot write the registry (HKCU\\" + std::string(kStarterKey) + ")"; return r; }

    // The addon options: AddonPath always, the others only when set (a missing value keeps the game's behaviour).
    winreg::Set(kAddonKey, "AddonPath", winreg::Value::String(WinPath((launcherDir / "mods" / modFolder).string())));
    if (log) log(std::string("HKCU\\") + kStarterKey + "\\path = " + dir + "; AddonPath = " + WinPath((launcherDir / "mods" / modFolder).string()));
    const AddonOptions& a = cfg.addon;
    auto dword = [&](const char* name, int v, bool write) {
        if (!write) return;
        winreg::Set(kAddonKey, name, winreg::Value::Dword(static_cast<uint32_t>(v)));
        if (log) log(std::string("HKCU\\") + kAddonKey + "\\" + name + " = dword:" + std::to_string(v));
    };
    dword("LandscapeDrawRadius", a.landscapeDrawRadius, a.landscapeDrawRadius > 0);
    dword("ObjectsDrawRadius", a.objectsDrawRadius, a.objectsDrawRadius > 0);
    dword("PauseOnMinimize", a.pauseOnMinimize, true);
    dword("MP Speed-control", a.mpSpeedControl, true);
    dword("MQ client", a.mqClient, true);
    dword("MP console", a.mpConsole, true);
    dword("LogLevel", a.logLevel, true);
    dword("ConsoleLogLevel", a.consoleLogLevel, true);
    dword("Xoptions Active", 1, true);   // what EIStarter's Options dialog always wrote
    dword("ModesFiltration", 0, true);

    // Wine uses its own dinput / ddraw unless told otherwise: Engine\\dinput.dll is the loader that brings addon.dll in (EIStarter needed this set by hand,
    // in winecfg or WINEDLLOVERRIDES). A Windows system has no use for the key.
    if (RunningUnderWine()) {
        std::error_code ec;
        winreg::Set("Software\\Wine\\DllOverrides", "dinput", winreg::Value::String("native,builtin"));
        if (fs::exists(launcherDir / "Engine" / "ddraw.dll", ec)) winreg::Set("Software\\Wine\\DllOverrides", "ddraw", winreg::Value::String("native,builtin"));
    }

    // The game's own settings, over the clone (created from Nival's key when it does not exist yet).
    if (!winreg::KeyExists(kCloneKey) && winreg::KeyExists(kNivalKey)) winreg::CopyTree(kNivalKey, kCloneKey);
    cfg.game.ApplyToRegistry(kCloneKey, log);
    return r;
}

} // namespace launcher
