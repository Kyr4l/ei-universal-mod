// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The mods the launcher offers: every folder of mods\ that holds a config.reg (the game's binary config, [MOD] / [starter] sections), or a
// mod.config (INI) when the starter option UsingINIconfigs is on. The folder mods\.0 is the "no mods" entry (the original game).
#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "eireg.hpp"
#include "inifile.hpp"

namespace launcher {

namespace fs = std::filesystem;

struct ModInfo {
    std::string folder;        // the folder name under mods\ (what launcher.cfg keeps)
    std::string path;          // the full folder path
    std::string title = "Unkn_Title";
    std::string version, author = "Unknown_Author", email, site, date;
    bool single = true, multi = true;
    std::string pluginPath = "none", pluginText = "Plugin"; // [starter] pluginpath / plugintext: a script the Info window offers to run
};

inline bool ReadMod(const fs::path& folder, bool iniConfigs, ModInfo& m) {
    m.folder = folder.filename().string();
    m.path = folder.string();
    std::error_code ec;
    if (iniConfigs && fs::exists(folder / "mod.config", ec)) {
        IniFile ini;
        if (!ini.Load((folder / "mod.config").string())) return false;
        m.title = ini.Get("MOD", "Title", "Unkn_Title");
        m.version = ini.Get("MOD", "Version"); m.author = ini.Get("MOD", "Author", "Unknown_Author");
        m.email = ini.Get("MOD", "AuthorEmail"); m.site = ini.Get("MOD", "URL"); m.date = ini.Get("MOD", "date_DMY");
        m.pluginPath = ini.Get("Starter", "pluginpath", "none"); m.pluginText = ini.Get("Starter", "plugintext", "Plugin");
        if (!ini.Get("MOD", "Single").empty()) m.single = ini.GetInt("MOD", "Single", 1) != 0;
        if (!ini.Get("MOD", "Multi").empty()) m.multi = ini.GetInt("MOD", "Multi", 1) != 0;
        return true;
    }
    if (fs::exists(folder / "config.reg", ec)) {
        EiRegFile reg;
        if (!reg.Load((folder / "config.reg").string())) return false;
        m.title = reg.GetString("MOD", "Title", "Unkn_Title");
        m.version = reg.GetString("MOD", "Version"); m.author = reg.GetString("MOD", "Author", "Unknown_Author");
        m.email = reg.GetString("MOD", "AuthorEmail"); m.site = reg.GetString("MOD", "URL"); m.date = reg.GetString("MOD", "date_DMY");
        m.pluginPath = reg.GetString("starter", "pluginpath", "none"); m.pluginText = reg.GetString("starter", "plugintext", "Plugin");
        m.single = reg.GetDword("MOD", "Single", 1) != 0;
        m.multi = reg.GetDword("MOD", "Multi", 1) != 0;
        return true;
    }
    return false;
}

inline std::vector<ModInfo> ScanMods(const fs::path& launcherDir, bool iniConfigs) {
    std::vector<ModInfo> mods;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(launcherDir / "mods", ec)) {
        if (!e.is_directory(ec)) continue;
        ModInfo m;
        if (ReadMod(e.path(), iniConfigs, m)) mods.push_back(std::move(m));
    }
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) { return LowerAscii(a.folder) < LowerAscii(b.folder); });
    return mods;
}

} // namespace launcher
