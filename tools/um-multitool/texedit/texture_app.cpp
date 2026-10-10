// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Texture Editor tab: open a game texture (from the texture sources or a file), look at it (zoom, pan, a
// 3 x 3 tiled preview to judge seams), select an area (rectangle or magic wand), change it (recolor, pattern
// fill, flip / rotate / offset) with undo, and save it as PNG / DDS / MMP into an output folder.
// Never writes next to the game's files: Save goes to the output folder only.
#include "texedit/texture_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "i18n.hpp"
#include "imgui.h"
#include <functional>

#include "subtools.hpp"
#include "log.hpp"
#include "texedit/quest_map.hpp"
#include "viewer/dds_texture.hpp"
#include "viewer/library.hpp"
#include "viewer/png_writer.hpp"
#include "viewer/ui_common.hpp"

namespace texedit {
namespace {

namespace fs = std::filesystem;

struct Editor {
    std::string path;           // the file box
    std::string name = "texture"; // base name of what is open (the saved file's default name)
    mmp::Image img;             // the picture being edited
    std::vector<mmp::Image> undo, redo;
    std::vector<uint8_t> mask;  // selection, one byte per pixel (empty: everything)
    GLuint tex = 0, selTex = 0;
    bool texDirty = false, selDirty = false;
    float zoom = 1.0f;
    ImVec2 pan{0, 0};
    bool needFit = false, tiled = false, showSel = true;
    std::string message;
    char filter[96] = "";
    // selection tool
    int selTool = 0; // 0 rectangle, 1 magic wand
    int tolerance = 24;
    bool dragging = false;
    int dragX = 0, dragY = 0, curX = 0, curY = 0;
    // recolor (live preview on adjBase until applied)
    float hue = 0, sat = 0, bright = 0, contrast = 0;
    bool adjActive = false;
    mmp::Image adjBase;
    // pattern
    int pattern = 0; // noise, checker, stripes, gradient
    float colA[3] = {0.25f, 0.45f, 0.15f}, colB[3] = {0.55f, 0.75f, 0.3f};
    int cell = 16, octaves = 3, seed = 1;
    bool tileable = true;
    float opacity = 1.0f;
    // quest map
    int qWidth = 256, qHeight = 256; // the picture (the game's quest maps are 256 x 256)
    questmap::Options quest;
    struct Stamp { std::string name; mmp::Image image; GLuint tex = 0; };
    std::vector<Stamp> stamps;
    int stamp = 0, stampScale = 1; // every stamp pixel is stampScale x stampScale
    char stampName[64] = "icon";
    char stampFilter[64] = "";
    bool stampsLoaded = false;
    // the sprite layer: placed sprites float over the picture until they are flattened into it
    struct Placed { int stamp; float x, y; float angle; int scale; }; // centre in pixels, angle in degrees
    std::vector<Placed> placed;
    int selected = -1;
    bool spriteDrag = false;
    float spriteDragX = 0, spriteDragY = 0;
    int panelTab = 0;
    std::function<bool(questmap::Input&, std::string&)> mapSource;
    // save
    int format = 0; // PNG, DDS, MMP PNT3, MMP 16-bit (quest maps)
    bool opaqueCopy = false; // also <name>m: the same picture with every pixel opaque (the game's quest maps come in pairs)
    char outDir[512] = "texture-output";
    char outName[128] = "texture";
};

Editor g;

bool Loaded() { return g.img.width > 0 && g.img.height > 0 && g.img.rgba.size() == static_cast<size_t>(g.img.width) * g.img.height * 4; }

void FreeTextures() {
    if (g.tex) glDeleteTextures(1, &g.tex);
    if (g.selTex) glDeleteTextures(1, &g.selTex);
    g.tex = g.selTex = 0;
}

void Upload(GLuint& id, uint32_t w, uint32_t h, const uint8_t* rgba) {
    if (!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

size_t Px(int x, int y) { return (static_cast<size_t>(y) * g.img.width + static_cast<size_t>(x)) * 4; }
bool Selected(size_t pixel) { return g.mask.empty() || g.mask[pixel]; }

// ---- undo ----------------------------------------------------------------------------------------
void PushUndo() {
    g.undo.push_back(g.img);
    if (g.undo.size() > 30) g.undo.erase(g.undo.begin());
    g.redo.clear();
}
void Changed() { g.texDirty = true; }

void CommitAdjust() { // the recolor preview becomes the picture
    if (!g.adjActive) return;
    g.adjActive = false;
    g.hue = g.sat = g.bright = g.contrast = 0;
}

void Undo() {
    CommitAdjust();
    if (g.undo.empty()) return;
    g.redo.push_back(g.img);
    g.img = g.undo.back();
    g.undo.pop_back();
    if (!g.mask.empty() && g.mask.size() != static_cast<size_t>(g.img.width) * g.img.height) g.mask.clear();
    Changed();
    g.selDirty = true;
}
void Redo() {
    CommitAdjust();
    if (g.redo.empty()) return;
    g.undo.push_back(g.img);
    g.img = g.redo.back();
    g.redo.pop_back();
    if (!g.mask.empty() && g.mask.size() != static_cast<size_t>(g.img.width) * g.img.height) g.mask.clear();
    Changed();
    g.selDirty = true;
}

// ---- opening -------------------------------------------------------------------------------------
void Adopt(mmp::Image&& image, const std::string& name) {
    FreeTextures();
    g.img = std::move(image);
    g.undo.clear();
    g.redo.clear();
    g.mask.clear();
    g.adjActive = false;
    g.hue = g.sat = g.bright = g.contrast = 0;
    g.name = name;
    std::snprintf(g.outName, sizeof(g.outName), "%s", name.c_str());
    g.texDirty = g.selDirty = true;
    g.needFit = true;
}

void OpenBytes(const std::vector<uint8_t>& bytes, const std::string& name) {
    mmp::Image image;
    std::string err;
    if (!DecodeTextureFile(bytes, image, err)) { g.message = "Cannot open " + name + ": " + err; return; }
    Adopt(std::move(image), name);
    g.message = "Opened " + name + " (" + std::to_string(g.img.width) + " x " + std::to_string(g.img.height) + ")";
}

void OpenPicture(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { g.message = "Cannot read " + path; return; } // a folder's stream throws on read
    std::ifstream f(path, std::ios::binary);
    if (!f) { g.message = "Cannot read " + path; return; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    OpenBytes(bytes, fs::path(path).stem().string());
}

// ---- selection -----------------------------------------------------------------------------------
void SelectRect(int x0, int y0, int x1, int y1, bool add) {
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, w - 1); y1 = std::min(y1, h - 1);
    if (g.mask.empty() || !add) g.mask.assign(static_cast<size_t>(w) * h, 0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) g.mask[static_cast<size_t>(y) * w + x] = 1;
    g.selDirty = true;
}

// Magic wand: the connected pixels whose colour is within `tolerance` of the clicked one.
void SelectWand(int sx, int sy, bool add) {
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return;
    std::vector<uint8_t> found(static_cast<size_t>(w) * h, 0);
    const uint8_t* ref = &g.img.rgba[Px(sx, sy)];
    std::vector<int> stack{sy * w + sx};
    found[static_cast<size_t>(sy) * w + sx] = 1;
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int x = p % w, y = p / w;
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
            const size_t q = static_cast<size_t>(ny[k]) * w + nx[k];
            if (found[q]) continue;
            const uint8_t* c = &g.img.rgba[q * 4];
            int d = 0;
            for (int ch = 0; ch < 4; ++ch) d = std::max(d, std::abs(static_cast<int>(c[ch]) - ref[ch]));
            if (d > g.tolerance) continue;
            found[q] = 1;
            stack.push_back(static_cast<int>(q));
        }
    }
    if (add && !g.mask.empty()) for (size_t i = 0; i < found.size(); ++i) found[i] |= g.mask[i];
    g.mask = std::move(found);
    g.selDirty = true;
}
void SelectNone() { g.mask.clear(); g.selDirty = true; }
void SelectInvert() {
    const size_t n = static_cast<size_t>(g.img.width) * g.img.height;
    if (g.mask.empty()) g.mask.assign(n, 0);
    else for (uint8_t& v : g.mask) v = v ? 0 : 1;
    g.selDirty = true;
}

// ---- tools ---------------------------------------------------------------------------------------
void RgbToHsv(float r, float g_, float b, float& h, float& s, float& v) {
    const float mx = std::max(r, std::max(g_, b)), mn = std::min(r, std::min(g_, b)), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = std::fmod((g_ - b) / d, 6.0f);
    else if (mx == g_) h = (b - r) / d + 2;
    else h = (r - g_) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
}
void HsvToRgb(float h, float s, float v, float& r, float& g_, float& b) {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360;
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = rr + m; g_ = gg + m; b = bb + m;
}
uint8_t To8(float v) { return static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); }

// The recolor preview: adjBase with hue / saturation / brightness / contrast applied inside the selection.
void ApplyAdjust() {
    g.img.rgba = g.adjBase.rgba;
    const size_t n = static_cast<size_t>(g.img.width) * g.img.height;
    for (size_t i = 0; i < n; ++i) {
        if (!Selected(i)) continue;
        uint8_t* p = &g.img.rgba[i * 4];
        float h, s, v;
        RgbToHsv(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, h, s, v);
        h += g.hue;
        s = std::min(std::max(s * (1.0f + g.sat), 0.0f), 1.0f);
        v = v + g.bright;
        v = (v - 0.5f) * (1.0f + g.contrast) + 0.5f;
        float r, gr, b;
        HsvToRgb(h, s, std::min(std::max(v, 0.0f), 1.0f), r, gr, b);
        p[0] = To8(r); p[1] = To8(gr); p[2] = To8(b);
    }
    Changed();
}

// The pattern's 0..1 mix of colour A and B at a pixel.
float PatternAt(int x, int y) {
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    const int cell = std::max(g.cell, 1);
    switch (g.pattern) {
    case 1: return ((x / cell) + (y / cell)) % 2 ? 1.0f : 0.0f;
    case 2: return (x / cell) % 2 ? 1.0f : 0.0f;
    case 3: return h > 1 ? static_cast<float>(y) / (h - 1) : 0.0f;
    default: {
        float sum = 0, amp = 1, norm = 0;
        float scale = 1.0f / cell;
        const int baseCells = std::max(1, static_cast<int>(std::lround(w * scale)));
        int period = g.tileable ? baseCells : 0;
        for (int o = 0; o < g.octaves; ++o) {
            const float fx = static_cast<float>(x) * baseCells / w * (1 << o), fy = static_cast<float>(y) * baseCells / h * (1 << o);
            sum += questmap::ValueNoise(fx, fy, period ? period * (1 << o) : 0, g.seed + o * 101) * amp;
            norm += amp;
            amp *= 0.5f;
        }
        return norm > 0 ? sum / norm : 0.0f;
    }
    }
}

void FillPattern() {
    CommitAdjust();
    PushUndo();
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!Selected(i)) continue;
            const float t = PatternAt(x, y);
            uint8_t* p = &g.img.rgba[i * 4];
            for (int ch = 0; ch < 3; ++ch) {
                const float v = g.colA[ch] + (g.colB[ch] - g.colA[ch]) * t;
                p[ch] = To8(p[ch] / 255.0f + (v - p[ch] / 255.0f) * g.opacity);
            }
            p[3] = std::max<uint8_t>(p[3], static_cast<uint8_t>(g.opacity * 255.0f));
        }
    Changed();
}

void Flip(bool horizontal) {
    CommitAdjust();
    PushUndo();
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    mmp::Image out = g.img;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) std::memcpy(&out.rgba[Px(x, y)], &g.img.rgba[Px(horizontal ? w - 1 - x : x, horizontal ? y : h - 1 - y)], 4);
    g.img = std::move(out);
    g.mask.clear();
    Changed(); g.selDirty = true;
}
void Rotate(bool clockwise) {
    CommitAdjust();
    PushUndo();
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    mmp::Image out;
    out.width = static_cast<uint32_t>(h);
    out.height = static_cast<uint32_t>(w);
    out.rgba.resize(g.img.rgba.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int nx = clockwise ? h - 1 - y : y, ny = clockwise ? x : w - 1 - x;
            std::memcpy(&out.rgba[(static_cast<size_t>(ny) * out.width + nx) * 4], &g.img.rgba[Px(x, y)], 4);
        }
    g.img = std::move(out);
    g.mask.clear();
    FreeTextures();
    Changed(); g.selDirty = true; g.needFit = true;
}
void OffsetHalf() { // wraps the picture by half its size: the seams come to the middle
    CommitAdjust();
    PushUndo();
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    mmp::Image out = g.img;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) std::memcpy(&out.rgba[Px((x + w / 2) % w, (y + h / 2) % h)], &g.img.rgba[Px(x, y)], 4);
    g.img = std::move(out);
    g.mask.clear();
    Changed(); g.selDirty = true;
}

// ---- saving --------------------------------------------------------------------------------------
void BakeSprite(mmp::Image& dst, const Editor::Placed& pl);
void Save() {
    CommitAdjust();
    mmp::Image baked = g.img; // the placed sprites go into the file, the picture here keeps them movable
    for (const Editor::Placed& pl : g.placed) if (pl.stamp >= 0 && pl.stamp < static_cast<int>(g.stamps.size())) BakeSprite(baked, pl);
    static const char* const ext[4] = {".png", ".dds", ".mmp", ".mmp"};
    std::error_code ec;
    fs::create_directories(g.outDir, ec);
    if (ec) { g.message = std::string("Cannot create the output folder: ") + ec.message(); return; }
    std::string base = g.outName[0] ? g.outName : "texture";
    std::string err;
    auto write = [&](const fs::path& file, const mmp::Image& img) {
        bool ok = false;
        if (g.format == 0) ok = png::Write(file.string(), static_cast<int>(img.width), static_cast<int>(img.height), img.rgba);
        else {
            std::vector<uint8_t> bytes;
            ok = g.format == 1 ? RgbaToDds(img.width, img.height, img.rgba, bytes)
               : RgbaToMmp(img.width, img.height, img.rgba, bytes, err, g.format == 3 ? MmpFormat::Argb1555 : MmpFormat::Pnt3);
            if (ok) {
                std::ofstream f(file, std::ios::binary);
                ok = static_cast<bool>(f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
            }
        }
        return ok;
    };
    const fs::path file = fs::path(g.outDir) / (base + ext[g.format]);
    bool ok = write(file, baked);
    std::string saved = file.string();
    if (ok && g.opaqueCopy) { // the "m" picture: alpha on everywhere, the transparent pixels black (as the vanilla questm)
        for (size_t i = 0; i + 3 < baked.rgba.size(); i += 4) {
            if (baked.rgba[i + 3] == 0) baked.rgba[i] = baked.rgba[i + 1] = baked.rgba[i + 2] = 0;
            baked.rgba[i + 3] = 255;
        }
        const fs::path fileM = fs::path(g.outDir) / (base + "m" + ext[g.format]);
        ok = write(fileM, baked);
        saved += " + " + fileM.filename().string();
    }
    g.message = ok ? "Saved " + saved : "Could not save " + saved + (err.empty() ? "" : ": " + err);
    umlog::Write(ok ? umlog::Level::Info : umlog::Level::Error, "Texture Editor: " + g.message);
}

// ---- quest maps and stamps -----------------------------------------------------------------------
fs::path StampDir() { return fs::path(g.outDir) / "quest-stamps"; }

void LoadStamps() {
    g.stampsLoaded = true;
    const int keep = g.stamp;
    for (Editor::Stamp& st : g.stamps) if (st.tex) glDeleteTextures(1, &st.tex);
    g.stamps.clear();
    for (const questmap::NamedIcon& icon : questmap::Icons()) { // the base game's own icons
        if (icon.name.rfind("roll-", 0) == 0) continue; // the scroll's ends belong to the generator
        Editor::Stamp st;
        st.name = icon.name;
        st.image = icon.image;
        g.stamps.push_back(std::move(st));
    }
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(StampDir(), ec))
        if (e.is_regular_file() && e.path().extension() == ".png") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Editor::Stamp st;
        std::string err;
        if (!DecodeTextureFile(bytes, st.image, err)) continue;
        st.name = f.stem().string();
        g.stamps.push_back(std::move(st));
    }
    std::sort(g.stamps.begin(), g.stamps.end(), [](const Editor::Stamp& a, const Editor::Stamp& b) { return a.name < b.name; });
    g.stamp = std::min(std::max(keep, 0), static_cast<int>(g.stamps.size()) - 1);
    for (Editor::Placed& pl : g.placed) pl.stamp = std::min(pl.stamp, static_cast<int>(g.stamps.size()) - 1);
}

GLuint StampTexture(Editor::Stamp& st) {
    if (!st.tex) Upload(st.tex, st.image.width, st.image.height, st.image.rgba.data());
    return st.tex;
}

int StampByName(const std::string& name) {
    for (size_t i = 0; i < g.stamps.size(); ++i) if (g.stamps[i].name == name) return static_cast<int>(i);
    return -1;
}

// A placed sprite's corners on the picture (pixels), for drawing and hit tests.
void SpriteCorners(const Editor::Placed& pl, ImVec2 out[4]) {
    const Editor::Stamp& st = g.stamps[static_cast<size_t>(pl.stamp)];
    const float hw = st.image.width * pl.scale * 0.5f, hh = st.image.height * pl.scale * 0.5f;
    const float a = pl.angle * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
    const ImVec2 local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (int i = 0; i < 4; ++i) out[i] = ImVec2(pl.x + local[i].x * c - local[i].y * sn, pl.y + local[i].x * sn + local[i].y * c);
}

bool SpriteHit(const Editor::Placed& pl, float x, float y) {
    const Editor::Stamp& st = g.stamps[static_cast<size_t>(pl.stamp)];
    const float a = -pl.angle * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
    const float dx = x - pl.x, dy = y - pl.y;
    const float u = dx * c - dy * sn, v = dx * sn + dy * c;
    return std::fabs(u) <= st.image.width * pl.scale * 0.5f + 1 && std::fabs(v) <= st.image.height * pl.scale * 0.5f + 1;
}

// A sprite baked into a picture: nearest-neighbour inverse mapping (pixel art stays crisp at quarter turns).
void BakeSprite(mmp::Image& dst, const Editor::Placed& pl) {
    const Editor::Stamp& st = g.stamps[static_cast<size_t>(pl.stamp)];
    const float a = -pl.angle * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
    const float hw = st.image.width * pl.scale * 0.5f, hh = st.image.height * pl.scale * 0.5f, r = std::hypot(hw, hh) + 1;
    for (int y = static_cast<int>(pl.y - r); y <= static_cast<int>(pl.y + r); ++y)
        for (int x = static_cast<int>(pl.x - r); x <= static_cast<int>(pl.x + r); ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(dst.width) || y >= static_cast<int>(dst.height)) continue;
            const float dx = x + 0.5f - pl.x, dy = y + 0.5f - pl.y;
            const float u = (dx * c - dy * sn + hw) / pl.scale, v = (dx * sn + dy * c + hh) / pl.scale;
            const int sx = static_cast<int>(std::floor(u)), sy = static_cast<int>(std::floor(v));
            if (sx < 0 || sy < 0 || sx >= static_cast<int>(st.image.width) || sy >= static_cast<int>(st.image.height)) continue;
            const uint8_t* sp = &st.image.rgba[(static_cast<size_t>(sy) * st.image.width + sx) * 4];
            if (sp[3] == 0) continue;
            uint8_t* p = &dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4];
            if (p[3] == 0) continue; // off the paper
            const float al = sp[3] / 255.0f;
            for (int ch = 0; ch < 3; ++ch) p[ch] = static_cast<uint8_t>(p[ch] + (sp[ch] - p[ch]) * al + 0.5f);
        }
}

void PlaceSprite(float x, float y) {
    if (g.stamps.empty() || g.stamp < 0 || g.stamp >= static_cast<int>(g.stamps.size())) return;
    g.placed.push_back({g.stamp, x, y, 0.0f, g.stampScale});
    g.selected = static_cast<int>(g.placed.size()) - 1;
}

void FlattenSprites() {
    if (g.placed.empty()) return;
    CommitAdjust();
    PushUndo();
    for (const Editor::Placed& pl : g.placed) BakeSprite(g.img, pl);
    g.placed.clear();
    g.selected = -1;
    Changed();
}

// A stamp cut out of the selection (of a vanilla quest map, say): the paper colour (the median of the rectangle's rim)
// becomes transparent, what differs from it (the ink) stays.
void MakeStampFromSelection() {
    if (g.mask.empty()) { g.message = "Select the icon first (rectangle or magic wand)."; return; }
    const int w = static_cast<int>(g.img.width), h = static_cast<int>(g.img.height);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (g.mask[static_cast<size_t>(y) * w + x]) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    if (x1 < x0) { g.message = "The selection is empty."; return; }
    x0 = std::max(x0 - 1, 0); y0 = std::max(y0 - 1, 0); x1 = std::min(x1 + 1, w - 1); y1 = std::min(y1 + 1, h - 1);
    std::vector<uint8_t> rim[3];
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (x == x0 || x == x1 || y == y0 || y == y1)
                for (int ch = 0; ch < 3; ++ch) rim[ch].push_back(g.img.rgba[Px(x, y) + static_cast<size_t>(ch)]);
    float bg[3];
    for (int ch = 0; ch < 3; ++ch) { std::sort(rim[ch].begin(), rim[ch].end()); bg[ch] = rim[ch][rim[ch].size() / 2]; }
    mmp::Image out;
    out.width = static_cast<uint32_t>(x1 - x0 + 1);
    out.height = static_cast<uint32_t>(y1 - y0 + 1);
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    int inked = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const uint8_t* p = &g.img.rgba[Px(x, y)];
            const float dr = p[0] - bg[0], dg = p[1] - bg[1], db = p[2] - bg[2];
            const float dist = std::sqrt(dr * dr + dg * dg + db * db);
            float a = std::min(std::max((dist - 18.0f) / 52.0f, 0.0f), 1.0f);
            if (!g.mask[static_cast<size_t>(y) * w + x]) a = 0.0f; // only inside the selection
            uint8_t* o = &out.rgba[(static_cast<size_t>(y - y0) * out.width + (x - x0)) * 4];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
            if (a > 0.5f) ++inked;
        }
    if (!inked) { g.message = "Nothing differs from the paper in the selection."; return; }
    std::error_code ec;
    fs::create_directories(StampDir(), ec);
    const std::string name = g.stampName[0] ? g.stampName : "icon";
    const fs::path file = StampDir() / (name + ".png");
    if (!png::Write(file.string(), static_cast<int>(out.width), static_cast<int>(out.height), out.rgba)) { g.message = "Could not save " + file.string(); return; }
    LoadStamps();
    g.stamp = std::max(0, StampByName(name));
    g.selTool = 2;
    g.message = "Stamp \"" + name + "\" saved to " + file.string();
}

void NewParchment() {
    mmp::Image paper;
    questmap::Parchment(paper, g.qWidth, g.qHeight, g.quest.seed, g.quest.roughness, g.quest.frame);
    Adopt(std::move(paper), "questmap");
    g.format = 3; g.opaqueCopy = true; // the game's quest map format and its "m" pair
    g.placed.clear();
    g.selected = -1;
    g.message = "A new parchment: " + std::to_string(g.qWidth) + " x " + std::to_string(g.qHeight);
}

void GenerateFromMap() {
    if (!g.mapSource) { g.message = "No Map Editor."; return; }
    if (!g.stampsLoaded) LoadStamps();
    questmap::Input in;
    std::string err;
    if (!g.mapSource(in, err)) { g.message = "Quest map: " + err; return; }
    questmap::Options opt = g.quest;
    opt.width = g.qWidth;
    opt.height = g.qHeight;
    mmp::Image out;
    std::vector<questmap::PlacedIcon> icons;
    if (!questmap::Generate(in, opt, out, err, &icons)) { g.message = "Quest map: " + err; return; }
    Adopt(std::move(out), in.name.empty() ? "questmap" : in.name + "quest");
    g.format = 3; g.opaqueCopy = true; // the game's quest map format and its "m" pair
    g.placed.clear();
    g.selected = -1;
    for (const questmap::PlacedIcon& ic : icons) { // the icons as movable sprites
        const int st = StampByName(ic.name);
        if (st >= 0) g.placed.push_back({st, static_cast<float>(ic.x), static_cast<float>(ic.y), 0.0f, ic.scale});
    }
    g.message = "Quest map made from " + (in.name.empty() ? std::string("the open map") : in.name) + ": " + std::to_string(g.placed.size()) +
                " sprite(s) placed, movable in the Sprites tab";
}

// ---- canvas --------------------------------------------------------------------------------------
void DrawCanvas() {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##texcanvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    dl->AddRectFilled(min, max, IM_COL32(32, 32, 36, 255));
    if (!Loaded()) {
        dl->AddText(ImVec2(min.x + 16, min.y + 16), IM_COL32(190, 190, 190, 255),
                    i18n::Tr("Open a texture, make a parchment (Quest map tab) or generate a quest map from the map open in the Map Editor."));
        return;
    }
    const float w = static_cast<float>(g.img.width), h = static_cast<float>(g.img.height);
    if (g.needFit) {
        g.zoom = std::min(size.x / (w * (g.tiled ? 3.0f : 1.0f)), size.y / (h * (g.tiled ? 3.0f : 1.0f))) * 0.95f;
        g.zoom = std::min(std::max(g.zoom, 0.05f), 32.0f);
        g.pan = ImVec2(0, 0);
        g.needFit = false;
    }
    if (g.texDirty) { Upload(g.tex, g.img.width, g.img.height, g.img.rgba.data()); g.texDirty = false; }
    if (g.selDirty) {
        g.selDirty = false;
        if (g.mask.empty()) { if (g.selTex) { glDeleteTextures(1, &g.selTex); g.selTex = 0; } }
        else {
            std::vector<uint8_t> overlay(g.mask.size() * 4, 0);
            for (size_t i = 0; i < g.mask.size(); ++i)
                if (g.mask[i]) { overlay[i * 4] = 70; overlay[i * 4 + 1] = 140; overlay[i * 4 + 2] = 255; overlay[i * 4 + 3] = 110; }
            Upload(g.selTex, g.img.width, g.img.height, overlay.data());
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 centre(min.x + size.x * 0.5f + g.pan.x, min.y + size.y * 0.5f + g.pan.y);
    // zoom around the mouse, pan with the middle / right button
    if (hovered && io.MouseWheel != 0 && !io.KeyCtrl) {
        const float before = g.zoom;
        g.zoom = std::min(std::max(g.zoom * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 0.05f), 64.0f);
        const ImVec2 m(io.MousePos.x - centre.x, io.MousePos.y - centre.y);
        g.pan.x -= m.x * (g.zoom / before - 1.0f);
        g.pan.y -= m.y * (g.zoom / before - 1.0f);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
        g.pan.x += io.MouseDelta.x;
        g.pan.y += io.MouseDelta.y;
    }
    const ImVec2 origin(min.x + size.x * 0.5f + g.pan.x - w * g.zoom * 0.5f, min.y + size.y * 0.5f + g.pan.y - h * g.zoom * 0.5f);
    auto toScreen = [&](float x, float y) { return ImVec2(origin.x + x * g.zoom, origin.y + y * g.zoom); };
    dl->PushClipRect(min, max, true);
    const int range = g.tiled ? 1 : 0;
    for (int ty = -range; ty <= range; ++ty)
        for (int tx = -range; tx <= range; ++tx) {
            const ImVec2 a(origin.x + tx * w * g.zoom, origin.y + ty * h * g.zoom), b(a.x + w * g.zoom, a.y + h * g.zoom);
            if (b.x < min.x || a.x > max.x || b.y < min.y || a.y > max.y) continue;
            // checkerboard behind transparent pixels
            const float sq = 12.0f;
            const float x0 = std::max(a.x, min.x), x1 = std::min(b.x, max.x), y0 = std::max(a.y, min.y), y1 = std::min(b.y, max.y);
            for (float yy = y0; yy < y1; yy += sq)
                for (float xx = x0; xx < x1; xx += sq) {
                    const int cx = static_cast<int>((xx - a.x) / sq), cy = static_cast<int>((yy - a.y) / sq);
                    dl->AddRectFilled(ImVec2(xx, yy), ImVec2(std::min(xx + sq, x1), std::min(yy + sq, y1)),
                                      (cx + cy) % 2 ? IM_COL32(70, 70, 76, 255) : IM_COL32(100, 100, 106, 255));
                }
            dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(g.tex)), a, b);
            if (tx == 0 && ty == 0 && g.showSel && g.selTex) dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(g.selTex)), a, b);
        }
    // the sprite layer
    for (size_t i = 0; i < g.placed.size(); ++i) {
        const Editor::Placed& pl = g.placed[i];
        if (pl.stamp < 0 || pl.stamp >= static_cast<int>(g.stamps.size())) continue;
        ImVec2 c[4];
        SpriteCorners(pl, c);
        ImVec2 sc[4];
        for (int k = 0; k < 4; ++k) sc[k] = toScreen(c[k].x, c[k].y);
        dl->AddImageQuad(static_cast<ImTextureID>(static_cast<intptr_t>(StampTexture(g.stamps[static_cast<size_t>(pl.stamp)]))), sc[0], sc[1], sc[2], sc[3],
                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
        if (static_cast<int>(i) == g.selected && g.selTool == 2) dl->AddPolyline(sc, 4, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.5f);
    }
    dl->AddRect(origin, ImVec2(origin.x + w * g.zoom, origin.y + h * g.zoom), IM_COL32(255, 255, 255, 90));
    // the pixel under the mouse
    const float fx = (io.MousePos.x - origin.x) / g.zoom, fy = (io.MousePos.y - origin.y) / g.zoom;
    const int px = static_cast<int>(std::floor(fx)), py = static_cast<int>(std::floor(fy));
    const bool inside = px >= 0 && py >= 0 && px < static_cast<int>(g.img.width) && py < static_cast<int>(g.img.height);
    if (hovered && inside) {
        const uint8_t* p = &g.img.rgba[Px(px, py)];
        char info[96];
        std::snprintf(info, sizeof(info), "%d, %d   RGBA %d %d %d %d   zoom %.0f%%", px, py, p[0], p[1], p[2], p[3], g.zoom * 100.0f);
        dl->AddText(ImVec2(min.x + 8, max.y - 22), IM_COL32(230, 230, 230, 255), info);
    }
    if (g.selTool == 2) { // sprites: click places or selects, drag moves, Ctrl+wheel turns
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
            int hit = -1;
            for (int i = static_cast<int>(g.placed.size()) - 1; i >= 0; --i)
                if (g.placed[static_cast<size_t>(i)].stamp >= 0 && SpriteHit(g.placed[static_cast<size_t>(i)], fx, fy)) { hit = i; break; }
            if (hit >= 0) { g.selected = hit; g.spriteDrag = true; g.spriteDragX = fx - g.placed[static_cast<size_t>(hit)].x; g.spriteDragY = fy - g.placed[static_cast<size_t>(hit)].y; }
            else if (inside) PlaceSprite(fx, fy);
            else g.selected = -1;
        }
        if (g.spriteDrag) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g.selected >= 0 && g.selected < static_cast<int>(g.placed.size())) {
                Editor::Placed& pl = g.placed[static_cast<size_t>(g.selected)];
                pl.x = std::round(fx - g.spriteDragX);
                pl.y = std::round(fy - g.spriteDragY);
            } else g.spriteDrag = false;
        }
        if (hovered && io.KeyCtrl && io.MouseWheel != 0 && g.selected >= 0 && g.selected < static_cast<int>(g.placed.size()))
            g.placed[static_cast<size_t>(g.selected)].angle = std::fmod(g.placed[static_cast<size_t>(g.selected)].angle + (io.MouseWheel > 0 ? 15.0f : -15.0f) + 360.0f, 360.0f);
        if (hovered && g.selected < 0 && g.stamp >= 0 && g.stamp < static_cast<int>(g.stamps.size())) { // where the sprite would land
            const Editor::Stamp& st = g.stamps[static_cast<size_t>(g.stamp)];
            const float hw = st.image.width * g.stampScale * g.zoom * 0.5f, hh = st.image.height * g.stampScale * g.zoom * 0.5f;
            dl->AddRect(ImVec2(io.MousePos.x - hw, io.MousePos.y - hh), ImVec2(io.MousePos.x + hw, io.MousePos.y + hh), IM_COL32(255, 255, 255, 140));
        }
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        CommitAdjust();
        if (g.selTool == 1) SelectWand(px, py, io.KeyShift);
        else if (inside) { g.dragging = true; g.dragX = g.curX = px; g.dragY = g.curY = py; }
    }
    if (g.dragging) {
        g.curX = std::min(std::max(px, 0), static_cast<int>(g.img.width) - 1);
        g.curY = std::min(std::max(py, 0), static_cast<int>(g.img.height) - 1);
        const int x0 = std::min(g.dragX, g.curX), x1 = std::max(g.dragX, g.curX), y0 = std::min(g.dragY, g.curY), y1 = std::max(g.dragY, g.curY);
        dl->AddRect(ImVec2(origin.x + x0 * g.zoom, origin.y + y0 * g.zoom), ImVec2(origin.x + (x1 + 1) * g.zoom, origin.y + (y1 + 1) * g.zoom),
                    IM_COL32(120, 190, 255, 255), 0.0f, 0, 2.0f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g.dragging = false;
            if (x0 == x1 && y0 == y1 && !io.KeyShift) SelectNone(); // a plain click clears
            else SelectRect(g.dragX, g.dragY, g.curX, g.curY, io.KeyShift);
        }
    }
    dl->PopClipRect();
}

// ---- the panel -----------------------------------------------------------------------------------
void DrawOpenSection(Library& lib) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", g.path.c_str());
    ImGui::SetNextItemWidth(-116);
    if (ImGui::InputTextWithHint("##texpath", "a .mmp, .dds, .png... file", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) { g.path = buf; OpenPicture(g.path); }
    else g.path = buf;
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickFile(picked)) { g.path = picked; OpenPicture(picked); }
    ImGui::SameLine();
    if (ImGui::Button("Open")) OpenPicture(g.path);
    if (ImGui::CollapsingHeader("Game textures")) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##texfilter", "search the game's textures", g.filter, sizeof(g.filter));
        if (lib.textureIndex.names.empty()) {
            ui::Note("No texture sources: add the game's textures.res in the Settings tab to list its textures here.");
            return;
        }
        std::string needle = g.filter;
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ImGui::BeginChild("##texlist", ImVec2(0, 140), true)) {
            ImGuiListClipper clipper;
            std::vector<const std::string*> shown;
            for (const std::string& n : lib.textureIndex.names)
                if (needle.empty() || n.find(needle) != std::string::npos) shown.push_back(&n);
            clipper.Begin(static_cast<int>(shown.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const std::string& n = *shown[static_cast<size_t>(i)];
                    if (ImGui::Selectable(n.c_str(), n == g.name)) {
                        std::vector<uint8_t> bytes;
                        if (lib.textures.ReadTexture(n, bytes)) { OpenBytes(bytes, n); g.placed.clear(); g.selected = -1; }
                        else g.message = "Cannot read " + n;
                    }
                }
        }
        ImGui::EndChild();
    }
}

void DrawQuestTab() {
    static const int sizes[] = {256, 512, 1024, 2048};
    static const char* const sizeNames[] = {"256", "512", "1024", "2048"};
    auto sizeCombo = [&](const char* id, int& value) {
        int idx = 0;
        for (int i = 0; i < 4; ++i) if (sizes[i] == value) idx = i;
        ImGui::SetNextItemWidth(70);
        if (ImGui::Combo(id, &idx, sizeNames, 4)) value = sizes[idx];
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Picture");
    ImGui::SameLine();
    sizeCombo("##qw", g.qWidth);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    sizeCombo("##qh", g.qHeight);
    {
        int frame = static_cast<int>(g.quest.frame);
        const char* names[questmap::kFrameCount];
        for (int i = 0; i < questmap::kFrameCount; ++i) names[i] = questmap::FrameName(static_cast<questmap::Frame>(i));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##frame", &frame, names, questmap::kFrameCount)) g.quest.frame = static_cast<questmap::Frame>(frame);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("The game's three frames: a torn sheet (zones 1-9), rolled ends top and bottom (10-14), rolled ends left and right (15-20)"));
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##rough", &g.quest.roughness, 0.0f, 1.0f, "Torn edge %.2f");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##qseed", &g.quest.seed, 1, 999, "Paper seed %d");
    ImGui::Checkbox("Cliffs", &g.quest.relief);
    ImGui::SameLine();
    ImGui::Checkbox("Water", &g.quest.water);
    ImGui::SameLine();
    ImGui::Checkbox("Bridges", &g.quest.bridges);
    ImGui::SameLine();
    ImGui::Checkbox("Icons", &g.quest.icons);
    ImGui::Checkbox("Forests", &g.quest.forests);
    ImGui::SameLine();
    ImGui::Checkbox("Exits", &g.quest.exits);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("A marker on each exit area of the quest open in the Map Editor (the #remove rectangle of map.txt, else the #deploy one)"));
    ImGui::Spacing();
    ImGui::BeginDisabled(!g.mapSource);
    if (ImGui::Button("Generate from the open map", ImVec2(-1, 0))) GenerateFromMap();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", i18n::Tr("Paints the terrain (cliff lines, water) of the terrain open in the Map Editor, its bridges over water as the game draws them, and the game's icons for its houses and goblin houses"));
    if (ImGui::Button("New parchment", ImVec2(-1, 0))) NewParchment();
}

void DrawSpritesTab() {
    if (!g.stampsLoaded) LoadStamps();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##stfilter", "search sprites", g.stampFilter, sizeof(g.stampFilter));
    std::string needle = g.stampFilter;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const float cellSize = 44.0f;
    if (ImGui::BeginChild("##stamps", ImVec2(0, 230), true)) {
        const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cellSize + 4)));
        int shown = 0;
        for (size_t i = 0; i < g.stamps.size(); ++i) {
            Editor::Stamp& st = g.stamps[i];
            if (!needle.empty() && st.name.find(needle) == std::string::npos) continue;
            if (shown % perRow) ImGui::SameLine();
            ++shown;
            ImGui::PushID(static_cast<int>(i));
            const bool sel = static_cast<int>(i) == g.stamp;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            // the sprite fitted in the cell, pixel art scaled by whole numbers when it fits
            const float k = std::min(cellSize - 8, std::max(1.0f, std::floor((cellSize - 8) / std::max(st.image.width, st.image.height)))) ;
            const float sw = st.image.width * k, sh = st.image.height * k;
            const ImVec2 pad((cellSize - sw) * 0.5f, (cellSize - sh) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
            if (ImGui::ImageButton("##st", static_cast<ImTextureID>(static_cast<intptr_t>(StampTexture(st))), ImVec2(sw, sh))) { g.stamp = static_cast<int>(i); g.selTool = 2; }
            ImGui::PopStyleVar();
            if (sel) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %u x %u", st.name.c_str(), st.image.width, st.image.height);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (g.stamp >= 0 && g.stamp < static_cast<int>(g.stamps.size())) ImGui::TextDisabled("%s: %s", i18n::Tr("Selected"), g.stamps[static_cast<size_t>(g.stamp)].name.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##stsize", &g.stampScale, 1, 8, "Scale x%d");
    ImGui::TextDisabled("%d sprite(s) placed", static_cast<int>(g.placed.size()));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("Click: place the chosen sprite; click a placed one: select it; drag: move; Q / E: turn 15 degrees, R: a quarter turn; [ ]: scale; Delete: remove. Sprites float over the picture until Flatten or Save."));
    if (g.selected >= 0 && g.selected < static_cast<int>(g.placed.size())) {
        Editor::Placed& pl = g.placed[static_cast<size_t>(g.selected)];
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##angle", &pl.angle, 0.0f, 360.0f, "Angle %.0f");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##plscale", &pl.scale, 1, 8, "Scale x%d");
        if (ImGui::Button("Remove")) { g.placed.erase(g.placed.begin() + g.selected); g.selected = -1; }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(g.placed.empty());
    if (ImGui::Button("Flatten sprites")) FlattenSprites();
    ImGui::SameLine();
    if (ImGui::Button("Remove all")) { g.placed.clear(); g.selected = -1; }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::SetNextItemWidth(-170);
    ImGui::InputTextWithHint("##stname", "stamp name", g.stampName, sizeof(g.stampName));
    ImGui::SameLine();
    if (ImGui::Button("Stamp from the selection")) MakeStampFromSelection();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("Select an icon on a vanilla quest map (open it from the list), then cut it out: the paper becomes transparent. Saved in the output folder's quest-stamps"));
    if (ImGui::Button("Reload stamps")) LoadStamps();
}

void DrawAdjustTab() {
    if (!Loaded()) { ImGui::TextDisabled("%s", i18n::Tr("Open a texture, make a parchment (Quest map tab) or generate a quest map from the map open in the Map Editor.")); return; }
    ImGui::SeparatorText("Selection");
    if (g.selTool == 1) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##tol", &g.tolerance, 0, 128, "Tolerance %d");
    }
    if (ImGui::Button("Select all")) { g.mask.assign(static_cast<size_t>(g.img.width) * g.img.height, 1); g.selDirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("None")) SelectNone();
    ImGui::SameLine();
    if (ImGui::Button("Invert")) SelectInvert();
    ImGui::SameLine();
    ImGui::Checkbox("Show", &g.showSel);
    ImGui::TextDisabled("%s", i18n::Tr(g.mask.empty() ? "Tools change the whole picture." : "Tools change the selected area only."));

    ImGui::SeparatorText("Recolor");
    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderFloat("##hue", &g.hue, -180.0f, 180.0f, "Hue %.0f");
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderFloat("##sat", &g.sat, -1.0f, 1.0f, "Saturation %.2f");
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderFloat("##bri", &g.bright, -1.0f, 1.0f, "Brightness %.2f");
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderFloat("##con", &g.contrast, -1.0f, 1.0f, "Contrast %.2f");
    if (changed) {
        if (!g.adjActive) { PushUndo(); g.adjBase = g.img; g.adjActive = true; }
        ApplyAdjust();
    }
    ImGui::BeginDisabled(!g.adjActive);
    if (ImGui::Button("Apply##adj")) CommitAdjust();
    ImGui::SameLine();
    if (ImGui::Button("Cancel##adj")) { g.img = g.adjBase; if (!g.undo.empty()) g.undo.pop_back(); CommitAdjust(); Changed(); }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Pattern");
    static const char* const names[] = {"Noise", "Checker", "Stripes", "Gradient"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##pat", &g.pattern, names, 4);
    ImGui::ColorEdit3("Color A", g.colA, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("Color B", g.colB, ImGuiColorEditFlags_NoInputs);
    if (g.pattern != 3) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##cell", &g.cell, 2, 128, g.pattern == 0 ? "Size %d px" : "Cell %d px");
    }
    if (g.pattern == 0) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##oct", &g.octaves, 1, 5, "Detail %d");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##seed", &g.seed, 1, 999, "Seed %d");
        ImGui::Checkbox("Seamless", &g.tileable);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##opa", &g.opacity, 0.0f, 1.0f, "Strength %.2f");
    if (ImGui::Button("Fill with the pattern")) FillPattern();

    ImGui::SeparatorText("Transform");
    if (ImGui::Button("Flip H")) Flip(true);
    ImGui::SameLine();
    if (ImGui::Button("Flip V")) Flip(false);
    ImGui::SameLine();
    if (ImGui::Button("Turn left")) Rotate(false);
    ImGui::SameLine();
    if (ImGui::Button("Turn right")) Rotate(true);
    if (ImGui::Button("Offset by half")) OffsetHalf();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("Wraps the picture by half its size: the seams of a tiling texture come to the middle"));
}

void DrawSaveTab() {
    if (!Loaded()) { ImGui::TextDisabled("%s", i18n::Tr("Open a texture, make a parchment (Quest map tab) or generate a quest map from the map open in the Map Editor.")); return; }
    static const char* const formats[] = {"PNG", "DDS (32-bit)", "MMP 32-bit (terrain textures)", "MMP 16-bit (quest maps)"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##fmt", &g.format, formats, 4);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("The game's quest maps are 16-bit (A1R5G5B5, 'QU'): one bit of transparency, as the torn edge needs"));
    ImGui::Checkbox("Also the opaque copy (<name>m)", &g.opaqueCopy);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("The game's quest maps come in pairs: zone7quest (transparent outside the sheet) and zone7questm (the same picture, every pixel opaque, black outside). Saves both."));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##outname", "file name", g.outName, sizeof(g.outName));
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##outdir", "output folder", g.outDir, sizeof(g.outDir));
    ImGui::SameLine();
    std::string folder;
    if (ImGui::Button("Folder...") && ui::PickFolder(folder)) std::snprintf(g.outDir, sizeof(g.outDir), "%s", folder.c_str());
    if (ImGui::Button("Save", ImVec2(-1, 0))) Save();
    if (!g.placed.empty()) ImGui::TextDisabled("%s", i18n::Tr("The placed sprites are baked into the saved file (the picture here keeps them movable)."));
    ImGui::TextDisabled("%s", i18n::Tr("Never into the game's folders: pick a folder of your own."));
}

} // namespace

void OpenPath(const std::string& path) { OpenPicture(path); }

void SetMapSource(std::function<bool(questmap::Input&, std::string&)> source) { g.mapSource = std::move(source); }

void DrawTab(Library& lib) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput && Loaded()) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) Redo(); else Undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        if (g.selTool == 2 && g.selected >= 0 && g.selected < static_cast<int>(g.placed.size())) { // the selected sprite's keys
            Editor::Placed& pl = g.placed[static_cast<size_t>(g.selected)];
            if (ImGui::IsKeyPressed(ImGuiKey_Q)) pl.angle = std::fmod(pl.angle - 15.0f + 360.0f, 360.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_E)) pl.angle = std::fmod(pl.angle + 15.0f, 360.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_R)) pl.angle = std::fmod(pl.angle + 90.0f, 360.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) pl.scale = std::max(1, pl.scale - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) pl.scale = std::min(8, pl.scale + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) { g.placed.erase(g.placed.begin() + g.selected); g.selected = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g.selected = -1;
        }
    }
    const float panelW = 330.0f;
    ImGui::BeginChild("##texpanel", ImVec2(panelW, 0), false);
    DrawOpenSection(lib);
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##textabs")) {
        if (ImGui::BeginTabItem("Quest map")) { DrawQuestTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sprites")) { DrawSpritesTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Adjust")) { DrawAdjustTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Save")) { DrawSaveTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    { // the mode bar: what a click on the picture does
        auto mode = [&](const char* label, int which, const char* tip) {
            const bool on = g.selTool == which;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label)) g.selTool = which;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr(tip));
            ImGui::SameLine();
        };
        mode("Select", 0, "Drag a rectangle (Shift: add to the selection); a click clears it");
        mode("Wand", 1, "Click picks the connected pixels of a similar colour (Shift: add)");
        mode("Sprites", 2, "Click: place the chosen sprite; click a placed one: select it; drag: move; Q / E: turn 15 degrees, R: a quarter turn; [ ]: scale; Delete: remove. Sprites float over the picture until Flatten or Save.");
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::BeginDisabled(!Loaded());
        if (ImGui::Button("Undo")) Undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) Redo();
        ImGui::SameLine();
        if (ImGui::Button("Fit")) g.needFit = true;
        ImGui::SameLine();
        if (ImGui::Button("1:1")) { g.zoom = 1.0f; g.pan = ImVec2(0, 0); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Tiled 3 x 3", &g.tiled)) g.needFit = true;
        ImGui::EndDisabled();
        if (Loaded()) { ImGui::SameLine(); ImGui::TextDisabled("%u x %u  %s", g.img.width, g.img.height, g.name.c_str()); }
    }
    if (!g.message.empty()) ImGui::TextUnformatted(g.message.c_str());
    DrawCanvas();
    ImGui::EndGroup();
}

} // namespace texedit
