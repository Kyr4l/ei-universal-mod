// Persists the viewer's loaded source paths (figure/texture layers, database)
// across sessions in a small text config file kept beside the executable -
// same convention as the game's own um.cfg beside game.exe.
#pragma once

#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace config {

inline std::string GetExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, len);
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, static_cast<size_t>(len));
#endif
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

inline std::string ConfigPath() {
    return GetExeDir() + "/um-modelviewer.cfg";
}

struct Config {
    std::vector<std::string> figureLayers;   // in load order, later = higher priority
    std::vector<std::string> textureLayers;
    std::string databasePath;
};

// Simple line-based "KEY=value" format, repeatable keys for layer lists -
// no need for anything more structured than what um.cfg itself uses.
inline Config Load() {
    Config cfg;
    std::ifstream f(ConfigPath());
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        if (value.empty()) continue;
        if (key == "FIGURE_LAYER") cfg.figureLayers.push_back(value);
        else if (key == "TEXTURE_LAYER") cfg.textureLayers.push_back(value);
        else if (key == "DATABASE") cfg.databasePath = value;
    }
    return cfg;
}

inline void Save(const Config& cfg) {
    std::ofstream f(ConfigPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << "; um-modelviewer remembered source paths - auto-generated, safe to delete.\n";
    for (auto& p : cfg.figureLayers) f << "FIGURE_LAYER=" << p << "\n";
    for (auto& p : cfg.textureLayers) f << "TEXTURE_LAYER=" << p << "\n";
    if (!cfg.databasePath.empty()) f << "DATABASE=" << cfg.databasePath << "\n";
}

} // namespace config
