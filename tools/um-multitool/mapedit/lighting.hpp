// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's lighting files (config/lights<region>.ini, e.g. lightsgipat.ini, lightscavegipat.ini):
// the sun, ambient and sky colours for each hour of the day,
//
//   [sunlight]        [ambient]        [sky]
//   time00 = r, g, b  ...              ...
//
// The Map Editor lights the map with them at the map's time of day (WORLD_SET's WS_TIME, in hours).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace lighting {

struct Color { float r = 1, g = 1, b = 1; };

struct Table {
    std::string path, name;
    bool ok = false;
    std::array<Color, 24> sun{}, ambient{}, sky{};

    // Colours at a time in hours (blended between the hours around it).
    static Color At(const std::array<Color, 24>& t, float hour) {
        hour = std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f);
        int h0 = static_cast<int>(hour) % 24, h1 = (h0 + 1) % 24;
        float f = hour - std::floor(hour);
        return {t[h0].r + (t[h1].r - t[h0].r) * f, t[h0].g + (t[h1].g - t[h0].g) * f, t[h0].b + (t[h1].b - t[h0].b) * f};
    }
};

inline bool Load(const std::string& path, Table& t) {
    t = Table{};
    t.path = path;
    t.name = std::filesystem::path(path).filename().string();
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::array<Color, 24>* section = nullptr;
    int found = 0;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '[') {
            std::string s = line;
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            section = s == "[sunlight]" ? &t.sun : s == "[ambient]" ? &t.ambient : s == "[sky]" ? &t.sky : nullptr;
            continue;
        }
        int hour, r, g, b;
        if (section && std::sscanf(line.c_str(), " time%d = %d , %d , %d", &hour, &r, &g, &b) == 4 && hour >= 0 && hour < 24) {
            (*section)[hour] = {r / 255.0f, g / 255.0f, b / 255.0f};
            ++found;
        }
    }
    t.ok = found > 0;
    return t.ok;
}

// The lighting files among these entries (lowest priority first): files as they are, folders searched
// for lights*.ini. A file name found in a later entry replaces the same name from an earlier one, so a
// mod's lightsgipat.ini listed above the game's config folder is the one used. Sorted by name.
inline std::vector<std::string> Expand(const std::vector<std::string>& entries) {
    std::map<std::string, std::string> byName; // lower-case file name -> path
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    for (const std::string& e : entries) {
        std::error_code ec;
        if (std::filesystem::is_directory(e, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(e, ec)) {
                std::string name = lower(entry.path().filename().string());
                if (name.rfind("lights", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0)
                    byName[name] = entry.path().string();
            }
        } else {
            byName[lower(std::filesystem::path(e).filename().string())] = e;
        }
    }
    std::vector<std::string> out;
    for (auto& kv : byName) out.push_back(kv.second);
    return out;
}

} // namespace lighting
