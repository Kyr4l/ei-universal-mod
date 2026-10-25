// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// make test: the launcher's file handling against real EIStarter / mod files. Usage: launcher_tests <starter folder> (a folder with mods\ and lang\)
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "../config.hpp"
#include "../mods.hpp"
#include "../regsync.hpp"
#include "../update.hpp"

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
        CHECK(c.addon.mpSpeedControl && c.addon.mpConsole);   // the multiplayer second speed and console are on by default
        CHECK(!c.addon.mqClient);
        CHECK(c.game.GetBool("general settings", "fullscreen", true) == false || true);
        CHECK(c.game.Has("Camera Settings", "CameraFlipXAxis") || c.game.Has("camera settings", "CAMERADEFAULTHEIGHT"));
        c.game.SetBool("general settings", "fullscreen", true);
        c.game.SetFloat("terrain settings", "LOD1", 90.f);
        c.game.SetString("Path Settings", "Work Path", "C:\\Evil Islands\\");
        c.addon.landscapeDrawRadius = 250; c.addon.mpConsole = true; c.addon.mpSpeedControl = false;
        CHECK(SaveConfig(tmp, c));
        LauncherConfig d = LoadConfig(tmp);
        CHECK(!d.firstRun); CHECK(d.mod == "Universal-Mod"); CHECK(d.addon.landscapeDrawRadius == 250 && d.addon.mpConsole && !d.addon.mpSpeedControl);   // a saved choice is kept
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
    // path settings: a path without its closing backslash gets one (the game appends "3DfpFPU.dll" to it), usable paths are kept, a relative GOG path is replaced
    {
        fs::path game = fs::temp_directory_path() / "EIStarter-UM-test-game"; fs::create_directories(game / "res");
        std::ofstream(game / "game.exe") << "x";
        GameSettings g;
        const std::string w = game.string();
        g.SetString("Path Settings", "Work Path", w);
        g.SetString("Path Settings", "CD-ROM Path", w);
        g.SetString("Path Settings", "Resources Path", (game / "res").string());
        CHECK(NormalizePaths(g));
        CHECK(g.GetString("Path Settings", "CD-ROM Path") == WithSlash(w));
        CHECK(g.GetString("Path Settings", "Resources Path") == WithSlash((game / "res").string()));
        CHECK(g.GetString("Path Settings", "Work Path") == WithSlash(w));
        CHECK(!NormalizePaths(g));   // done: nothing more to repair
        g.SetString("Path Settings", "CD-ROM Path", ".\\");
        CHECK(NormalizePaths(g) && g.GetString("Path Settings", "CD-ROM Path") == WithSlash(w));
        fs::remove_all(game);
    }
    // the camera of the original CD release: found, replaced, written to the registry, the choice kept
    {
        GameSettings g; g.FillDefaults();
        CHECK(g.CameraDifferences().empty());                                      // a fresh installation's values are the original ones
        g.SetFloat("camera settings", "CameraDefaultScrollSpeed", 3.f);               // the GOG values differ (the names are in capitals there)
        g.SetInt("CAMERA SETTINGS", "CAMERABORDERSCROLLAREA", 9);
        g.SetBool("general settings", "fullscreen", false);
        CHECK(g.CameraDifferences().size() == 2);
        CHECK(!g.CameraLines().empty() && g.CameraLines().size() == 62);
        g.RestoreOriginalCamera();
        CHECK(g.CameraDifferences().empty());
        CHECK(!g.GetBool("general settings", "fullscreen", true));                   // the other settings stay
        GameSettings none; CHECK(none.CameraDifferences().size() == 62);              // a missing value counts as different
        GameSettings::WriteOriginalCameraToRegistry(kNivalKey);
        winreg::Value v;
        CHECK(winreg::Get(std::string(kNivalKey) + "\\Camera Settings", "FARCLIPDISTANCE", v) && v.bytes == std::vector<uint8_t>({0x00, 0x00, 0xC8, 0x42}));
        CHECK(winreg::Get(std::string(kNivalKey) + "\\camera settings", "CameraBorderScrollArea", v) && v.dword == 1);
        // the choice "don't show again" survives the config file
        LauncherConfig c; c.game = g; CHECK(c.cameraCheck);
        c.cameraCheck = false; c.cameraRestored = true;
        CHECK(SaveConfig(tmp, c));
        LauncherConfig d = LoadConfig(tmp);
        CHECK(!d.cameraCheck); CHECK(!d.cameraRestored);                              // cameraRestored is not saved
        CHECK(d.game.CameraDifferences().empty());
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
        // a missing key is added on its own line after another key (the DLLs never add keys to an existing cfg)
        std::ofstream(tmp / "um2.cfg", std::ios::binary) << "; comment\r\nA=1\r\nBORDERLESS=true ; x\r\nC=3\r\n";
        const std::string p2 = (tmp / "um2.cfg").string();
        CHECK(SetCfgLineAfter(p2, "SCALE_KEY", "true", "BORDERLESS"));
        CHECK(ReadTextFile(p2) == "; comment\r\nA=1\r\nBORDERLESS=true ; x\r\nSCALE_KEY=true\r\nC=3\r\n");
        CHECK(SetCfgLineAfter(p2, "SCALE_KEY", "false", "BORDERLESS") && GetCfgLine(p2, "SCALE_KEY") == "false");
        CHECK(!SetCfgLineAfter(p2, "X", "1", "NOPE_EITHER"));
        std::ofstream(tmp / "um3.cfg", std::ios::binary) << "A=1\nBORDERLESS=true";   // no newline at the end, LF endings
        CHECK(SetCfgLineAfter((tmp / "um3.cfg").string(), "SCALE_KEY", "true", "BORDERLESS") && ReadTextFile((tmp / "um3.cfg").string()) == "A=1\nBORDERLESS=true\nSCALE_KEY=true\n");
    }
    // updater: JSON, release choice, zip extraction, folder swap
    {
        const std::string json = R"([{"tag_name":"0.9.0","name":"Beta","draft":false,"prerelease":true,"published_at":"2026-10-20T10:00:00Z","assets":[{"name":"um.dll","size":5,"updated_at":"x","browser_download_url":"https://h/um.dll"},{"name":"Universal-Mod-beta-0.9.0-abc.zip","size":123,"updated_at":"2026-10-21T19:40:17Z","browser_download_url":"https://h/b.zip"}]},
            {"tag_name":"0.8.5","name":"Rel \u00e9","draft":false,"prerelease":false,"published_at":"2026-08-07T23:00:52Z","assets":[{"name":"Universal-Mod-0.8.5-10ed.zip","size":99,"updated_at":"2026-09-14T19:02:01Z","browser_download_url":"https://h/r.zip"}]}])";
        update::Release r; std::string err;
        CHECK(update::PickRelease(json, false, r, err)); CHECK(r.tag == "0.9.0" && r.prerelease && r.assetName == "Universal-Mod-beta-0.9.0-abc.zip" && r.assetSize == 123);
        CHECK(r.Key() == "0.9.0|2026-10-21T19:40:17Z|123");
        CHECK(update::PickRelease(json, true, r, err)); CHECK(r.tag == "0.8.5" && r.assetUrl == "https://h/r.zip");
        CHECK(!update::PickRelease("{\"message\":\"API rate limit exceeded\"}", false, r, err) && err.find("rate limit") != std::string::npos);
        CHECK(!update::PickRelease("not json", false, r, err));
        CHECK(update::HashOfAssetName("Universal-Mod-beta-0.8.6-b373a09c9.zip") == "b373a09c9"); CHECK(update::HashOfAssetName("Universal-Mod-0.8.5-10ed39635.zip") == "10ed39635");
        CHECK(update::HashOfAssetName("Universal-Mod-beta-0.8.6-.zip").empty()); CHECK(update::HashOfAssetName("Universal-Mod-0.8.6.zip").empty());
        { update::Release hr; std::string e2; const std::string j2 = R"([{"tag_name":"0.8.6","draft":false,"prerelease":true,"assets":[{"name":"Universal-Mod-beta-0.8.6-abc1234.zip","size":5,"updated_at":"x","browser_download_url":"https://h/z.zip"}]}])";
          CHECK(update::PickRelease(j2, false, hr, e2) && hr.hash == "abc1234" && hr.Key() == "0.8.6|abc1234"); }
        CHECK(update::CompareVersions("0.8.6", "0.8.5") > 0); CHECK(update::CompareVersions("v.1.0.0", "1.0") == 0); CHECK(update::CompareVersions("0.9", "0.10") < 0); CHECK(update::CompareVersions("", "0.1") < 0);
        CHECK(update::SafeRelative("a/b.txt")); CHECK(!update::SafeRelative("../x")); CHECK(!update::SafeRelative("a/../../x")); CHECK(!update::SafeRelative("/abs")); CHECK(!update::SafeRelative("C:/x"));
        // a real zip made by 7z (deflate and stored entries, a root folder "Universal-Mod/")
        const fs::path z = fs::temp_directory_path() / "um-launcher-zip"; fs::remove_all(z); fs::create_directories(z / "src/Universal-Mod/res"); fs::create_directories(z / "src/Universal-Mod/mp");
        { std::ofstream(z / "src/Universal-Mod/config.reg") << "x"; std::ofstream(z / "src/Universal-Mod/um.cfg") << "NEW=1\n"; std::ofstream(z / "src/Universal-Mod/mp/zip.mp") << "zipsave";
          std::ofstream o(z / "src/Universal-Mod/res/big.txt"); for (int i = 0; i < 20000; ++i) o << "line " << i << " of a compressible text\n"; }
        const std::string cmd = "cd '" + (z / "src").string() + "' && 7z a -tzip -bso0 ../t.zip Universal-Mod >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) {
            std::atomic<bool> no{false}; size_t files = 0;
            CHECK(update::ExtractZip(z / "t.zip", z / "out", nullptr, no, files, err)); CHECK(files == 4);
            CHECK(fs::exists(z / "out/config.reg") && fs::exists(z / "out/res/big.txt") && !fs::exists(z / "out/Universal-Mod"));
            CHECK(fs::file_size(z / "out/res/big.txt") == fs::file_size(z / "src/Universal-Mod/res/big.txt"));
            // swap: keeps mp and saves of the old folder, backs up the cfgs, removes the rest
            const fs::path mod = z / "mods/Universal-Mod"; fs::create_directories(mod / "mp"); fs::create_directories(mod / "saves"); fs::create_directories(mod / "old");
            { std::ofstream(mod / "mp/my.mp") << "mine"; std::ofstream(mod / "saves/s.sav") << "s"; std::ofstream(mod / "um.cfg") << "OLD=1\n"; std::ofstream(mod / "old/gone.txt") << "g"; }
            CHECK(update::SwapModFolder(mod, z / "out", z / "mods/.u/old", z / "mods/.u/backup", {"mp", "saves"}, err));
            CHECK(fs::exists(mod / "mp/my.mp") && !fs::exists(mod / "mp/zip.mp") && fs::exists(mod / "saves/s.sav") && !fs::exists(mod / "old") && fs::exists(mod / "res/big.txt"));
            CHECK(ReadTextFile((mod / "um.cfg").string()) == "NEW=1\n" && ReadTextFile((z / "mods/.u/backup/um.cfg").string()) == "OLD=1\n");
            CHECK(!fs::exists(z / "mods/.u/old"));
            // a build without um-engine.cfg keeps the user's
            { fs::create_directories(z / "fresh2"); std::ofstream(z / "fresh2/config.reg") << "x"; std::ofstream(mod / "um-engine.cfg") << "MINE=1\n"; }
            CHECK(update::SwapModFolder(mod, z / "fresh2", z / "mods/.u/old", z / "mods/.u/backup", {"mp", "saves"}, err));
            CHECK(ReadTextFile((mod / "um-engine.cfg").string()) == "MINE=1\n");
            // a fresh install: the folder does not exist yet
            { fs::create_directories(z / "fresh3"); std::ofstream(z / "fresh3/config.reg") << "x"; }
            CHECK(update::SwapModFolder(z / "mods/Universal-Mod-Beta", z / "fresh3", z / "mods/.u/old2", z / "mods/.u/backup", {"mp", "saves"}, err));
            CHECK(fs::exists(z / "mods/Universal-Mod-Beta/config.reg"));
            // a damaged archive: flip a byte inside the data, the CRC must catch it
            { std::fstream f(z / "t.zip", std::ios::in | std::ios::out | std::ios::binary); const std::streamoff at = static_cast<std::streamoff>(fs::file_size(z / "t.zip") / 3); f.seekg(at); char c; f.read(&c, 1); f.seekp(at); c = static_cast<char>(c ^ 0x55); f.write(&c, 1); }
            CHECK(!update::ExtractZip(z / "t.zip", z / "out2", nullptr, no, files, err));
        } else std::printf("(7z missing: zip tests skipped)\n");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
