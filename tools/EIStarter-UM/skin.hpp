// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The launcher's looks, read the way EIStarter reads them (so its skins work): the background back.png|bmp and the button images
// <mask>.png|bmp (+ _h hovered, _d pressed) are looked for in design\<language>\, design\, and first of all in the mod's own design\ folders
// when the option ModSkins is on; font.otf|ttf and click.wav likewise. Masks: pl op re we un ex (the six buttons), lang, info.
#pragma once

#include <GLFW/glfw3.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "stb/stb_image.h"

namespace launcher {

namespace fs = std::filesystem;

struct Texture {
    unsigned id = 0;
    int w = 0, h = 0;
    explicit operator bool() const { return id != 0; }
};

inline Texture LoadTexture(const std::string& path) {
    Texture t;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return t;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    t.id = id; t.w = w; t.h = h;
    return t;
}

class Skin {
public:
    // `launcherDir`: where design\ and lang\ live; `modDesign`: the chosen mod's design\ folder ("" when the mod has none or ModSkins is off).
    void Configure(const fs::path& launcherDir, const std::string& modDesign, const std::string& lang) {
        dirs_.clear();
        const fs::path base = launcherDir / "design";
        if (!modDesign.empty()) { dirs_.push_back(fs::path(modDesign) / lang); dirs_.push_back(modDesign); }
        dirs_.push_back(base / lang);
        dirs_.push_back(base);
        modDesign_ = modDesign;
    }

    // The first existing "<mask>.png|bmp" over the design folders ("" when none).
    std::string FindImage(const std::string& mask) const { return FindFile(mask, {".png", ".bmp"}); }
    std::string FindFile(const std::string& name, const std::vector<std::string>& exts) const {
        std::error_code ec;
        for (const fs::path& d : dirs_)
            for (const std::string& e : exts) {
                const fs::path p = d / (name + e);
                if (fs::is_regular_file(p, ec)) return p.string();
            }
        return {};
    }
    std::string FindExact(const std::string& file) const {
        std::error_code ec;
        for (const fs::path& d : dirs_) if (fs::is_regular_file(d / file, ec)) return (d / file).string();
        return {};
    }

    // The texture of an image mask ("pl", "pl_h" ...), loaded once.
    const Texture& Image(const std::string& mask) {
        const std::string path = FindImage(mask);
        if (path.empty()) return none_;
        auto it = cache_.find(path);
        if (it == cache_.end()) it = cache_.emplace(path, LoadTexture(path)).first;
        return it->second;
    }
    const Texture& Background() { return Image("back"); }

    // font.otf / font.ttf: the first one of the design folders ("" when none).
    std::string FontPath() const {
        std::string p = FindFile("font", {".otf", ".ttf"});
        return p;
    }
    std::string ClickSound() const { return FindExact("click.wav"); }

    void Clear() { for (auto& kv : cache_) if (kv.second.id) glDeleteTextures(1, &kv.second.id); cache_.clear(); }

private:
    std::vector<fs::path> dirs_;
    std::string modDesign_;
    std::map<std::string, Texture> cache_;
    Texture none_;
};

// The languages the skin offers: every design\<code>\ folder that holds an image.
inline std::vector<std::string> EnumerateLanguages(const fs::path& launcherDir) {
    std::vector<std::string> langs;
    std::error_code ec;
    for (const fs::directory_entry& d : fs::directory_iterator(launcherDir / "design", ec)) {
        if (!d.is_directory(ec)) continue;
        for (const fs::directory_entry& f : fs::directory_iterator(d.path(), ec)) {
            std::string ext = f.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (f.is_regular_file(ec) && (ext == ".png" || ext == ".bmp")) { langs.push_back(d.path().filename().string()); break; }
        }
    }
    std::sort(langs.begin(), langs.end());
    return langs;
}

} // namespace launcher
