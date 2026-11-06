// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Options > Renderer: which DirectDraw the game gets, for every mod. D7VK (a DirectDraw / Direct3D 7 on Vulkan) is loaded by its full path, so no
// Wine DLL override and nothing named ddraw.dll is needed:
//   - a mod with um.dll: the environment the game inherits says it (UM_RENDERER = d7vk | native, UM_D7VK = the file); um.dll loads it (um_hooks.cpp);
//   - any other mod: Engine\um-window.ini [Renderer] D7VK=<file>; Engine\um-window.asi sends the game's DirectDraw calls to it.
// The file: the mod's own d7vk.dll (Universal Mod ships the build its um-engine's sun shadows need), else Engine\d7vk.dll (shipped with the launcher).
// Auto: Engine\d7vk-check.exe tries D7VK first (a 32-bit process like the game); the result is kept until d7vk.dll or the check program changes.
// Either way a D7VK that fails to create its DirectDraw falls back to the game's own (um.dll and um-window.asi both do that).
// D7VK's options go into DXVK_CONFIG (merged over the dxvk.conf of Engine\) and DXVK_HUD.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#include "config.hpp"
#include "inifile.hpp"
#include "mods.hpp"
#include "platform.hpp"

namespace launcher {

namespace fs = std::filesystem;

// The d7vk.dll the game would get with this mod: the mod's own, else Engine's; empty when there is none.
inline fs::path FindD7vk(const fs::path& dir, const ModInfo* mod) {
    std::error_code ec;
    if (mod && !mod->path.empty() && fs::is_regular_file(fs::path(mod->path) / "d7vk.dll", ec)) return fs::path(mod->path) / "d7vk.dll";
    if (fs::is_regular_file(dir / "Engine" / "d7vk.dll", ec)) return dir / "Engine" / "d7vk.dll";
    return {};
}

inline fs::path D7vkCheckExe(const fs::path& dir) { return dir / "Engine" / "d7vk-check.exe"; }

// What the check depends on: both files' size and time (a new d7vk.dll or check program runs it again).
inline std::string D7vkCheckKey(const fs::path& dir, const fs::path& d7vk) {
    std::string key;
    for (const fs::path& f : {d7vk, D7vkCheckExe(dir)}) {
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        if (ec) return {};
#ifdef _WIN32 // MinGW's std::filesystem time is a constant under Wine: the file time from Windows
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (!GetFileAttributesExW(f.wstring().c_str(), GetFileExInfoStandard, &a)) return {};
        const long long time = (static_cast<long long>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
#else
        const long long time = static_cast<long long>(fs::last_write_time(f, ec).time_since_epoch().count());
#endif
        key += f.filename().string() + ":" + std::to_string(size) + ":" + std::to_string(time) + ";";
    }
    return key;
}

// Runs the check now (a few seconds: it starts Vulkan) and keeps the result in `r`. Returns whether D7VK works.
inline bool RunD7vkCheck(const fs::path& dir, const fs::path& d7vk, RendererOptions& r) {
    r.checkKey = D7vkCheckKey(dir, d7vk);
    std::error_code ec;
    if (d7vk.empty()) { r.checkCode = 2; r.checkText = "no d7vk.dll"; return false; }
    if (!fs::is_regular_file(D7vkCheckExe(dir), ec)) { r.checkCode = 1; r.checkText = "Engine\\d7vk-check.exe is missing"; return false; }
    const fs::path result = dir / "Engine" / "d7vk-check.txt";
    fs::remove(result, ec);
    std::string error;
    r.checkCode = platform::RunWait(D7vkCheckExe(dir), {d7vk, result}, dir / "Engine", 30000, error);
    std::ifstream f(result.string());
    r.checkText.clear();
    std::getline(f, r.checkText);
    if (r.checkText.empty()) r.checkText = r.checkCode == 0 ? "D7VK works" : (error.empty() ? "the check failed" : "the check failed: " + error);
    fs::remove(result, ec);
    return r.checkCode == 0;
}

// D7VK's options as a DXVK_CONFIG value ("a = b;c = d").
inline std::string D7vkConfig(const RendererOptions& r) {
    std::string s;
    auto add = [&](const std::string& kv) { s += (s.empty() ? "" : ";") + kv; };
    if (r.msaa) add("ddraw.emulateFSAA = forced;ddraw.alternatePixelCenter = true"); // the pixel-center fix: without it a multisampled frame has a 1-pixel line at the left and top
    if (r.anisotropy > 0) add("d3d9.samplerAnisotropy = " + std::to_string(r.anisotropy));
    if (r.vsync == 1) add("d3d9.presentInterval = 1");
    if (r.vsync == 2) add("d3d9.presentInterval = 0");
    if (r.fpsLimit > 0) add("dxvk.maxFrameRate = " + std::to_string(r.fpsLimit));
    if (r.frameLatency > 0) add("d3d9.maxFrameLatency = " + std::to_string(r.frameLatency));
    return s;
}

// The renderer for this Play: sets the environment and Engine\um-window.ini. `log` gets one line per decision. Returns the d7vk.dll used, or empty for the game's own.
template <class Log> fs::path ApplyRenderer(const fs::path& dir, const ModInfo* mod, RendererOptions& r, Log log) {
    static const std::string baseConfig = platform::GetEnv("DXVK_CONFIG"), baseHud = platform::GetEnv("DXVK_HUD"); // the user's own, kept under ours
    const bool umDll = mod && mod->umListed && mod->umFile;
    fs::path d7vk = r.mode == 2 ? fs::path() : FindD7vk(dir, mod);
    if (r.mode != 2 && d7vk.empty()) log("renderer: no d7vk.dll (mod folder or Engine): the game's own DirectDraw");
    if (r.mode == 0 && !d7vk.empty()) {
        if (r.checkKey != D7vkCheckKey(dir, d7vk) || r.checkCode != 0) { // never checked, files changed, or it failed last time (a new driver may have fixed it)
            log("renderer: checking D7VK (" + d7vk.string() + ")");
            RunD7vkCheck(dir, d7vk, r);
            log("renderer: check result " + std::to_string(r.checkCode) + ": " + r.checkText);
        }
        if (r.checkCode != 0) { log("renderer: Auto -> the game's own DirectDraw (" + r.checkText + ")"); d7vk.clear(); }
    }
    const std::string config = D7vkConfig(r);
    if (!d7vk.empty()) {
        platform::SetEnv("DXVK_CONFIG", baseConfig.empty() ? config : config.empty() ? baseConfig : baseConfig + ";" + config);
        platform::SetEnv("DXVK_HUD", r.fpsOverlay ? (baseHud.empty() ? std::string("fps") : baseHud) : baseHud);
    } else { platform::SetEnv("DXVK_CONFIG", baseConfig); platform::SetEnv("DXVK_HUD", baseHud); }
    platform::SetEnv("UM_RENDERER", umDll ? (d7vk.empty() ? "native" : "d7vk") : "");
    platform::SetEnv("UM_D7VK", umDll ? d7vk.string() : "");
    std::error_code ec;
    if (fs::exists(dir / "Engine" / "um-window.asi", ec)) {
        const std::string iniPath = (dir / "Engine" / "um-window.ini").string();
        IniFile ini;
        ini.Load(iniPath);
        ini.Set("Renderer", "D7VK", umDll ? std::string() : d7vk.string()); // um.dll loads it itself
        ini.Save(iniPath);
    } else if (!umDll && !d7vk.empty()) { log("renderer: Engine\\um-window.asi is missing: the game's own DirectDraw"); d7vk.clear(); }
    log(std::string("renderer: ") + (d7vk.empty() ? "the game's own DirectDraw" : "D7VK " + d7vk.string()) + (umDll ? " (through um.dll)" : " (through um-window.asi)")
        + (d7vk.empty() ? std::string() : "; DXVK_CONFIG " + config));
    return d7vk;
}

} // namespace launcher
