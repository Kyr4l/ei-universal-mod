// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Writes what the loader chain reads (HKCU\Software\Gipat.Ru\EI_Starter, see docs/launcher.md) from the launcher's config file, so nobody has to
// run "Init Starter Registry.bat" or add the `path` value by hand:
//   EI_Starter\path            the launcher's folder (Engine\dinput.dll looks for addon.dll there)
//   EI_Starter\settings        AddonPath (the chosen mod's folder) and the addon.dll options
//   EI_Starter\EvilIslands     the game's settings (the clone of Nival's key that addon.dll points game.exe at)
#pragma once

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

struct SyncResult { bool ok = true; std::string error; };

inline SyncResult SyncRegistry(const LauncherConfig& cfg, const fs::path& launcherDir, const std::string& modFolder) {
    SyncResult r;
    const std::string dir = WinPath(launcherDir.string());
    if (!winreg::Set(kStarterKey, "path", winreg::Value::String(dir))) { r.ok = false; r.error = "cannot write the registry (HKCU\\" + std::string(kStarterKey) + ")"; return r; }

    // The addon options: AddonPath always, the others only when set (a missing value keeps the game's behaviour).
    winreg::Set(kAddonKey, "AddonPath", winreg::Value::String(WinPath((launcherDir / "mods" / modFolder).string())));
    const AddonOptions& a = cfg.addon;
    auto dword = [&](const char* name, int v, bool write) { if (write) winreg::Set(kAddonKey, name, winreg::Value::Dword(static_cast<uint32_t>(v))); };
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

    // The game's own settings, over the clone (created from Nival's key when it does not exist yet).
    if (!winreg::KeyExists(kCloneKey) && winreg::KeyExists(kNivalKey)) winreg::CopyTree(kNivalKey, kCloneKey);
    cfg.game.ApplyToRegistry(kCloneKey);
    return r;
}

} // namespace launcher
