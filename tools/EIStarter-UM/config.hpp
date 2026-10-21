// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The launcher's settings, kept in EIStarter-UM.cfg beside the program (no registry): the chosen mod, the language, the starter options of
// EIStarter (ModSkins, UsingINIconfigs, ExeName), the addon.dll options of its Options dialog, and the game's own settings
// ([Game: ...] sections, gamesettings.hpp). A first start imports what EIStarter left: starter.config and the registry.
#pragma once

#include <filesystem>
#include <string>

#include "gamesettings.hpp"
#include "inifile.hpp"
#include "winreg.hpp"

namespace launcher {

namespace fs = std::filesystem;

struct AddonOptions {
    int landscapeDrawRadius = 0;  // 0 = the game's own (the registry value is not written)
    int objectsDrawRadius = 0;
    bool pauseOnMinimize = false;
    bool mpSpeedControl = false;  // "second speed support" in multiplayer
    bool mqClient = false;        // "do not load .MQ (quest) files from the host"
    bool mpConsole = false;
    int logLevel = 1;             // addon.log
    int consoleLogLevel = 1;
};

struct LauncherConfig {
    std::string mod;               // the folder name under mods/
    std::string language = "en";
    bool modSkins = true;
    bool usingIniConfigs = false;
    std::string exeName = "game.exe";
    bool closeOnStart = true;      // leave when the game window is up
    bool debugLog = false;         // more detail in EIStarter-UM.log
    bool borderless = false;       // windowed game without a frame (needs fullscreen off)
    bool largeAddressAware = false; // set the 4 GB flag in Engine/game.exe before starting (off by default: only for a game that runs out of memory)
    AddonOptions addon;
    GameSettings game;
    bool firstRun = false;         // no EIStarter-UM.cfg existed
};

inline std::string ConfigPath(const fs::path& dir) { return (dir / "EIStarter-UM.cfg").string(); }

inline AddonOptions ReadAddonRegistry() {
    AddonOptions a;
    auto num = [&](const char* name, int& out) { winreg::Value v; if (winreg::Get(kAddonKey, name, v) && v.kind == winreg::Value::Kind::Dword) out = static_cast<int>(v.dword); };
    int b = 0;
    num("LandscapeDrawRadius", a.landscapeDrawRadius); num("ObjectsDrawRadius", a.objectsDrawRadius);
    num("LogLevel", a.logLevel); num("ConsoleLogLevel", a.consoleLogLevel);
    b = a.pauseOnMinimize; num("PauseOnMinimize", b); a.pauseOnMinimize = b != 0;
    b = a.mpSpeedControl; num("MP Speed-control", b); a.mpSpeedControl = b != 0;
    b = a.mqClient; num("MQ client", b); a.mqClient = b != 0;
    b = a.mpConsole; num("MP console", b); a.mpConsole = b != 0;
    return a;
}

inline LauncherConfig LoadConfig(const fs::path& dir) {
    LauncherConfig c;
    IniFile ini;
    if (ini.Load(ConfigPath(dir))) {
        c.mod = ini.Get("Launcher", "Mod");
        c.language = ini.Get("Launcher", "Language", "en");
        c.modSkins = ini.GetBool("Launcher", "ModSkins", true);
        c.usingIniConfigs = ini.GetBool("Launcher", "UsingINIconfigs", false);
        c.exeName = ini.Get("Launcher", "ExeName", "game.exe");
        c.closeOnStart = ini.GetBool("Launcher", "CloseOnStart", true);
        c.debugLog = ini.GetBool("Launcher", "DebugLog", false);
        c.borderless = ini.GetBool("Launcher", "Borderless", false);
        c.largeAddressAware = ini.GetBool("Launcher", "LargeAddressAware", false);
        AddonOptions& a = c.addon;
        a.landscapeDrawRadius = ini.GetInt("Addon", "LandscapeDrawRadius", 0);
        a.objectsDrawRadius = ini.GetInt("Addon", "ObjectsDrawRadius", 0);
        a.pauseOnMinimize = ini.GetBool("Addon", "PauseOnMinimize", false);
        a.mpSpeedControl = ini.GetBool("Addon", "MPSpeedControl", false);
        a.mqClient = ini.GetBool("Addon", "MQClient", false);
        a.mpConsole = ini.GetBool("Addon", "MPConsole", false);
        a.logLevel = ini.GetInt("Addon", "LogLevel", 1);
        a.consoleLogLevel = ini.GetInt("Addon", "ConsoleLogLevel", 1);
        c.game.Load(ini);
    } else {
        c.firstRun = true;
        IniFile old; // EIStarter's starter.config: ModSel=Mods\<mod>\config.reg
        if (old.Load((dir / "starter.config").string())) {
            std::string sel = old.Get("Settings", "ModSel");
            for (char& ch : sel) if (ch == '\\') ch = '/';
            const size_t a = sel.find('/');
            if (a != std::string::npos) { const size_t b = sel.find('/', a + 1); c.mod = sel.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1); }
            c.language = old.Get("Settings", "language", "en");
            c.modSkins = old.GetBool("Settings", "ModSkins", true);
            c.usingIniConfigs = old.GetBool("Settings", "UsingINIconfigs", false);
            c.exeName = old.Get("Settings", "ExeName", "game.exe");
        }
        c.addon = ReadAddonRegistry();
    }
    if (c.game.Empty()) { // first run (or an empty file): the clone EIStarter made, else Nival's key, else a fresh installation's values
        c.game.ImportRegistry(kNivalKey);   // Nival's values first, the clone's (EIStarter's own settings) on top
        c.game.ImportRegistry(kCloneKey);
    }
    c.game.FillDefaults();
    return c;
}

inline bool SaveConfig(const fs::path& dir, const LauncherConfig& c) {
    IniFile ini;
    ini.Set("Launcher", "Mod", c.mod);
    ini.Set("Launcher", "Language", c.language);
    ini.SetBool("Launcher", "ModSkins", c.modSkins);
    ini.SetBool("Launcher", "UsingINIconfigs", c.usingIniConfigs);
    ini.Set("Launcher", "ExeName", c.exeName);
    ini.SetBool("Launcher", "CloseOnStart", c.closeOnStart);
    ini.SetBool("Launcher", "DebugLog", c.debugLog);
    ini.SetBool("Launcher", "Borderless", c.borderless);
    ini.SetBool("Launcher", "LargeAddressAware", c.largeAddressAware);
    const AddonOptions& a = c.addon;
    ini.SetInt("Addon", "LandscapeDrawRadius", a.landscapeDrawRadius);
    ini.SetInt("Addon", "ObjectsDrawRadius", a.objectsDrawRadius);
    ini.SetBool("Addon", "PauseOnMinimize", a.pauseOnMinimize);
    ini.SetBool("Addon", "MPSpeedControl", a.mpSpeedControl);
    ini.SetBool("Addon", "MQClient", a.mqClient);
    ini.SetBool("Addon", "MPConsole", a.mpConsole);
    ini.SetInt("Addon", "LogLevel", a.logLevel);
    ini.SetInt("Addon", "ConsoleLogLevel", a.consoleLogLevel);
    c.game.Save(ini);
    return ini.Save(ConfigPath(dir), {"EIStarter-UM settings. The [Game: ...] sections are the game's own options (written to the registry when the game starts)."});
}

} // namespace launcher
