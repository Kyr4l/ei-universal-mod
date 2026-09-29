// Remembers the viewer's sources (figure/texture layers, database) between sessions
// in um-multitool-viewer.cfg beside the executable: "KEY=value" lines, layer keys repeated
// in load order (later = higher priority).
#pragma once

#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace config {

inline std::string ExeDir() {
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

inline std::string Path() { return ExeDir() + "/um-multitool-viewer.cfg"; }

// GIF export settings (see the viewer's "Export GIF" dialog).
struct GifSettings {
    int size = 384;                 // square, in pixels
    int fps = 25;
    float degreesPerSecond = 60.0f; // one full turn takes 360 / this seconds
    bool reverse = false;           // turn the other way
    int axis = 2;                   // the axis the model spins about: 0 = X, 1 = Y, 2 = Z (vertical)
    bool transparent = true;        // no background
    unsigned background = 0x28282E; // RRGGBB, used when not transparent
    std::string lastDirectory;      // where the last GIF went
};

struct Config {
    std::vector<std::string> figureLayers;
    std::vector<std::string> textureLayers;
    std::vector<std::string> textLayers;      // texts.res / textslmp.res / folders of loose texts
    std::string databasePath;
    // Per tab (key e.g. "WEAPONS"): the shown model's orientation, a unit quaternion (w, x, y, z).
    std::map<std::string, std::array<float, 4>> rotations;
    GifSettings gif;
};

inline Config Load(const std::string& path = Path()) {
    Config cfg;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq + 1 >= line.size()) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "FIGURE_LAYER") cfg.figureLayers.push_back(value);
        else if (key == "TEXTURE_LAYER") cfg.textureLayers.push_back(value);
        else if (key == "TEXT_LAYER") cfg.textLayers.push_back(value);
        else if (key == "DATABASE") cfg.databasePath = value;
        else if (key == "GIF_SIZE") cfg.gif.size = std::atoi(value.c_str());
        else if (key == "GIF_FPS") cfg.gif.fps = std::atoi(value.c_str());
        else if (key == "GIF_SPEED") cfg.gif.degreesPerSecond = static_cast<float>(std::atof(value.c_str()));
        else if (key == "GIF_REVERSE") cfg.gif.reverse = value == "true";
        else if (key == "GIF_AXIS") cfg.gif.axis = value == "X" ? 0 : value == "Y" ? 1 : 2;
        else if (key == "GIF_BACKGROUND") {
            cfg.gif.transparent = value == "none";
            if (!cfg.gif.transparent) cfg.gif.background = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFF;
        }
        else if (key == "GIF_DIRECTORY") cfg.gif.lastDirectory = value;
        else if (key.rfind("ROTATION_", 0) == 0) {
            float v[4] = {0, 0, 0, 0};
            int n = std::sscanf(value.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
            if (n == 4) {                  // w,x,y,z
                float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
                if (len > 1e-6f) cfg.rotations[key.substr(9)] = {v[0] / len, v[1] / len, v[2] / len, v[3] / len};
            } else if (n == 3) {           // older files: degrees about X, then Y, then Z
                std::array<float, 4> q = {1, 0, 0, 0};
                for (int axis = 0; axis < 3; ++axis) {
                    float half = v[axis] * 3.14159265f / 360.0f;
                    std::array<float, 4> r = {std::cos(half), axis == 0 ? std::sin(half) : 0.0f,
                                              axis == 1 ? std::sin(half) : 0.0f, axis == 2 ? std::sin(half) : 0.0f};
                    q = {r[0] * q[0] - r[1] * q[1] - r[2] * q[2] - r[3] * q[3], r[0] * q[1] + r[1] * q[0] + r[2] * q[3] - r[3] * q[2],
                         r[0] * q[2] - r[1] * q[3] + r[2] * q[0] + r[3] * q[1], r[0] * q[3] + r[1] * q[2] - r[2] * q[1] + r[3] * q[0]};
                }
                cfg.rotations[key.substr(9)] = q;
            }
        }
    }
    return cfg;
}

inline void Save(const Config& cfg, const std::string& path = Path()) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "; um-multitool 3D Viewer settings - written by the viewer, safe to delete.\n";
    for (auto& p : cfg.figureLayers) f << "FIGURE_LAYER=" << p << "\n";
    for (auto& p : cfg.textureLayers) f << "TEXTURE_LAYER=" << p << "\n";
    for (auto& p : cfg.textLayers) f << "TEXT_LAYER=" << p << "\n";
    if (!cfg.databasePath.empty()) f << "DATABASE=" << cfg.databasePath << "\n";
    char background[16];
    std::snprintf(background, sizeof(background), "%06X", cfg.gif.background & 0xFFFFFF);
    f << "GIF_SIZE=" << cfg.gif.size << "\nGIF_FPS=" << cfg.gif.fps << "\nGIF_SPEED=" << cfg.gif.degreesPerSecond
      << "\nGIF_REVERSE=" << (cfg.gif.reverse ? "true" : "false") << "\nGIF_AXIS=" << "XYZ"[cfg.gif.axis % 3] << "\nGIF_BACKGROUND=" << (cfg.gif.transparent ? "none" : background) << "\n";
    if (!cfg.gif.lastDirectory.empty()) f << "GIF_DIRECTORY=" << cfg.gif.lastDirectory << "\n";
    for (auto& kv : cfg.rotations) {
        const auto& q = kv.second;
        if (std::fabs(std::fabs(q[0]) - 1.0f) < 1e-6f) continue; // no rotation
        char line[160];
        std::snprintf(line, sizeof(line), "ROTATION_%s=%.6f,%.6f,%.6f,%.6f\n", kv.first.c_str(), q[0], q[1], q[2], q[3]);
        f << line;
    }
}

} // namespace config
