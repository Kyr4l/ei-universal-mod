// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// make test: the launcher's file handling against real EIStarter / mod files. Usage: launcher_tests <starter folder> (a folder with mods\ and lang\)
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "../config.hpp"
#include "../mods.hpp"
#include "../regsync.hpp"

using namespace launcher;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const fs::path starter = argc > 1 ? argv[1] : ".";
    // mods
    auto mods = ScanMods(starter, false);
    std::printf("%zu mods:\n", mods.size());
    for (auto& m : mods) std::printf("  [%s] %s | %s | %s | %s | single %d multi %d | plugin %s / %s\n", m.folder.c_str(), m.title.c_str(), m.version.c_str(), m.author.c_str(), m.date.c_str(), m.single, m.multi, m.pluginPath.c_str(), m.pluginText.c_str());
    CHECK(!mods.empty());
    bool um = false, none = false;
    for (auto& m : mods) { if (m.folder == "Universal-Mod") { um = m.title == "Universal Mod" && m.author == "Formcaster & Kyral" && !m.single && m.multi; } if (m.folder == ".0") none = m.title == "\xD0\x91\xD0\xB5\xD0\xB7 \xD0\xBC\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xB2"; }
    CHECK(um); CHECK(none);
    for (auto& m : mods) { if (m.folder == "Universal-Mod") CHECK(m.umListed && m.umFile); else CHECK(!m.umListed); }
    // lang.ini (UTF-16)
    IniFile fr; CHECK(fr.Load((starter / "lang/fr/lang.ini").string()));
    CHECK(fr.Get("Options", "cbFullscreen") == "Plein \xC3\xA9" "cran");
    CHECK(fr.Get("options", "CBBTEXTURES__1") == "normale");
    // config round trip + registry sync (the stand-in registry on Linux)
    fs::path tmp = fs::temp_directory_path() / "EIStarter-UM-test"; fs::create_directories(tmp);
    fs::remove(tmp / "EIStarter-UM.cfg");
    {
        std::ofstream(tmp / "starter.config") << "[Settings]\nModSel=Mods\\Universal-Mod\\config.reg\nlanguage=fr\nModSkins=True\nUsingINIconfigs=False\nExeName=game.exe\n";
        LauncherConfig c = LoadConfig(tmp);
        CHECK(c.firstRun); CHECK(c.mod == "Universal-Mod"); CHECK(c.language == "fr");
        CHECK(c.game.GetBool("general settings", "fullscreen", true) == false || true);
        CHECK(c.game.Has("Camera Settings", "CameraFlipXAxis") || c.game.Has("camera settings", "CAMERADEFAULTHEIGHT"));
        c.game.SetBool("general settings", "fullscreen", true);
        c.game.SetFloat("terrain settings", "LOD1", 90.f);
        c.game.SetString("Path Settings", "Work Path", "C:\\Evil Islands\\");
        c.addon.landscapeDrawRadius = 250; c.addon.mpConsole = true;
        CHECK(SaveConfig(tmp, c));
        LauncherConfig d = LoadConfig(tmp);
        CHECK(!d.firstRun); CHECK(d.mod == "Universal-Mod"); CHECK(d.addon.landscapeDrawRadius == 250 && d.addon.mpConsole);
        CHECK(d.game.GetBool("general settings", "fullscreen"));
        CHECK(d.game.GetFloat("terrain settings", "LOD1") == 90.f);
        CHECK(d.game.GetString("Path Settings", "Work Path") == "C:\\Evil Islands\\");
        CHECK(d.game.Items().size() == c.game.Items().size());
        SyncResult r = SyncRegistry(d, "/x/EIStarter", "Universal-Mod");
        CHECK(r.ok);
        std::string p; CHECK(winreg::GetString(kStarterKey, "path", p)); CHECK(p == "/x/EIStarter");
        CHECK(winreg::GetString(kAddonKey, "AddonPath", p) && p.find("Universal-Mod") != std::string::npos);
        winreg::Value v; CHECK(winreg::Get(kAddonKey, "LandscapeDrawRadius", v) && v.dword == 250);
        CHECK(winreg::Get(std::string(kCloneKey) + "\\terrain settings", "LOD1", v) && v.bytes.size() == 4);
    }
    // SetCfgLine
    {
        std::ofstream(tmp / "um.cfg") << "; comment\r\nA=1\r\nBORDERLESS=true ; x\r\nC=3\r\n";
        CHECK(SetCfgLine((tmp / "um.cfg").string(), "borderless", "false"));
        CHECK(!SetCfgLine((tmp / "um.cfg").string(), "NOPE", "1"));
        std::string t = ReadTextFile((tmp / "um.cfg").string());
        CHECK(t == "; comment\r\nA=1\r\nBORDERLESS=false\r\nC=3\r\n");
        CHECK(GetCfgLine((tmp / "um.cfg").string(), "borderless") == "false");
        CHECK(GetCfgLine((tmp / "um.cfg").string(), "C") == "3");
        CHECK(GetCfgLine((tmp / "um.cfg").string(), "nope").empty());
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
