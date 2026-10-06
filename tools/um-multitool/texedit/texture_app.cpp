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

#include "imgui.h"
#include "subtools.hpp"
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
    // save
    int format = 0; // PNG, DDS, MMP
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

void OpenPath(const std::string& path) {
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

uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float Lattice(int x, int y, int period, int seed) {
    if (period > 0) { x = ((x % period) + period) % period; y = ((y % period) + period) % period; }
    return (Hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(seed)) & 0xFFFF) / 65535.0f;
}
// Value noise at (x, y) in lattice cells; `period` wraps it so the picture tiles (0: no wrapping).
float ValueNoise(float x, float y, int period, int seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const float a = Lattice(xi, yi, period, seed), b = Lattice(xi + 1, yi, period, seed);
    const float c = Lattice(xi, yi + 1, period, seed), d = Lattice(xi + 1, yi + 1, period, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
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
            sum += ValueNoise(fx, fy, period ? period * (1 << o) : 0, g.seed + o * 101) * amp;
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
void Save() {
    CommitAdjust();
    static const char* const ext[3] = {".png", ".dds", ".mmp"};
    std::error_code ec;
    fs::create_directories(g.outDir, ec);
    if (ec) { g.message = std::string("Cannot create the output folder: ") + ec.message(); return; }
    std::string base = g.outName[0] ? g.outName : "texture";
    const fs::path file = fs::path(g.outDir) / (base + ext[g.format]);
    bool ok = false;
    std::string err;
    if (g.format == 0) ok = png::Write(file.string(), static_cast<int>(g.img.width), static_cast<int>(g.img.height), g.img.rgba);
    else {
        std::vector<uint8_t> bytes;
        ok = g.format == 1 ? RgbaToDds(g.img.width, g.img.height, g.img.rgba, bytes) : RgbaToMmp(g.img.width, g.img.height, g.img.rgba, bytes, err);
        if (ok) {
            std::ofstream f(file, std::ios::binary);
            ok = static_cast<bool>(f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
        }
    }
    g.message = ok ? "Saved " + file.string() : "Could not save " + file.string() + (err.empty() ? "" : ": " + err);
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
        dl->AddText(ImVec2(min.x + 16, min.y + 16), IM_COL32(190, 190, 190, 255), "Open a texture from the list or a file.");
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
    if (hovered && io.MouseWheel != 0) {
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
    dl->AddRect(origin, ImVec2(origin.x + w * g.zoom, origin.y + h * g.zoom), IM_COL32(255, 255, 255, 90));
    // selection tools: the pixel under the mouse
    const int px = static_cast<int>(std::floor((io.MousePos.x - origin.x) / g.zoom));
    const int py = static_cast<int>(std::floor((io.MousePos.y - origin.y) / g.zoom));
    const bool inside = px >= 0 && py >= 0 && px < static_cast<int>(g.img.width) && py < static_cast<int>(g.img.height);
    if (hovered && inside) {
        const uint8_t* p = &g.img.rgba[Px(px, py)];
        char info[96];
        std::snprintf(info, sizeof(info), "%d, %d   RGBA %d %d %d %d   zoom %.0f%%", px, py, p[0], p[1], p[2], p[3], g.zoom * 100.0f);
        dl->AddText(ImVec2(min.x + 8, max.y - 22), IM_COL32(230, 230, 230, 255), info);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
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

// ---- left panel ----------------------------------------------------------------------------------
void DrawOpenPanel(Library& lib) {
    ImGui::SeparatorText("Open");
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", g.path.c_str());
    ImGui::SetNextItemWidth(-150);
    if (ImGui::InputTextWithHint("##texpath", "a .mmp, .dds, .png... file", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) { g.path = buf; OpenPath(g.path); }
    else g.path = buf;
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickFile(picked)) { g.path = picked; OpenPath(picked); }
    ImGui::SameLine();
    if (ImGui::Button("Open")) OpenPath(g.path);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##texfilter", "search the game's textures", g.filter, sizeof(g.filter));
    if (lib.textureIndex.names.empty()) {
        ui::Note("No texture sources: add the game's textures.res in the Settings tab to list its textures here.");
        return;
    }
    std::string needle = g.filter;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ImGui::BeginChild("##texlist", ImVec2(0, 150), true)) {
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
                    if (lib.textures.ReadTexture(n, bytes)) OpenBytes(bytes, n);
                    else g.message = "Cannot read " + n;
                }
            }
    }
    ImGui::EndChild();
}

void DrawToolsPanel() {
    ImGui::SeparatorText("Selection");
    ImGui::RadioButton("Rectangle", &g.selTool, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Magic wand", &g.selTool, 1);
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
    ImGui::TextDisabled("%s", g.mask.empty() ? "Tools change the whole picture." : "Tools change the selected area only.");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag for a rectangle (Shift: add); the wand picks the connected pixels of a similar colour (Shift: add)");

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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wraps the picture by half its size: the seams of a tiling texture come to the middle");
}

void DrawSavePanel() {
    ImGui::SeparatorText("Save");
    static const char* const formats[] = {"PNG", "DDS (32-bit)", "MMP (the game's)"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##fmt", &g.format, formats, 3);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##outname", "file name", g.outName, sizeof(g.outName));
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##outdir", "output folder", g.outDir, sizeof(g.outDir));
    ImGui::SameLine();
    std::string folder;
    if (ImGui::Button("Folder...") && ui::PickFolder(folder)) std::snprintf(g.outDir, sizeof(g.outDir), "%s", folder.c_str());
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    ImGui::TextDisabled("Never into the game's folders: pick a folder of your own.");
}

} // namespace

void DrawTab(Library& lib) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput && io.KeyCtrl && Loaded()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) Redo(); else Undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
    }
    const float panelW = 320.0f;
    ImGui::BeginChild("##texpanel", ImVec2(panelW, 0), false);
    DrawOpenPanel(lib);
    if (Loaded()) {
        DrawToolsPanel();
        DrawSavePanel();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (Loaded()) {
        if (ImGui::Button("Undo")) Undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) Redo();
        ImGui::SameLine();
        if (ImGui::Button("Fit")) g.needFit = true;
        ImGui::SameLine();
        if (ImGui::Button("1:1")) { g.zoom = 1.0f; g.pan = ImVec2(0, 0); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Tiled 3 x 3", &g.tiled)) g.needFit = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%u x %u  %s", g.img.width, g.img.height, g.name.c_str());
    }
    if (!g.message.empty()) {
        if (Loaded()) ImGui::SameLine();
        ImGui::TextUnformatted(g.message.c_str());
    }
    DrawCanvas();
    ImGui::EndGroup();
}

} // namespace texedit
