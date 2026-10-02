// A figure's UV layout as a picture: each part's triangles filled in its own colour and outlined, over a texture
// (or on transparency), at the texture's exact size so the lines fall on its texels (painting guides).
// Item figures address a 256x256 atlas (see Scene::Draw): with `atlas`, their UVs are mapped back onto the texture
// the same way the viewer does.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dds_texture.hpp"
#include "figure_format.hpp"

namespace uvmap {

struct Result { int width = 0, height = 0; std::vector<uint8_t> rgba; std::vector<std::string> legend; };

// parts: the names to draw (lower case); empty = all. tex: drawn under the layout when given; its size sets the
// picture's (else `size`, transparent).
inline Result Draw(const fig::Model& model, const std::set<std::string>& parts, const mmp::Image* tex, bool atlas, int size = 512) {
    Result r;
    const bool hasTex = tex && tex->width && tex->height;
    r.width = hasTex ? static_cast<int>(tex->width) : size;
    r.height = hasTex ? static_cast<int>(tex->height) : size;
    const int W = r.width, H = r.height;
    r.rgba.assign(static_cast<size_t>(W) * H * 4, 0);
    if (hasTex) r.rgba = tex->rgba;
    // The atlas: a texture W wide takes a (W/256) square at the atlas' bottom-left: u = u'/s, v = (v' - (1-s))/s.
    const float slot = atlas && hasTex && W < 256 ? W / 256.0f : 1.0f;
    static const uint8_t kColors[][3] = {{255, 60, 60}, {60, 220, 60}, {70, 110, 255}, {255, 220, 40}, {255, 60, 255}, {40, 230, 230},
                                         {255, 150, 40}, {160, 80, 255}, {40, 160, 255}, {255, 80, 150}, {170, 255, 60}, {40, 255, 160}};
    auto put = [&](int x, int y, const uint8_t* c, float a) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* d = &r.rgba[(static_cast<size_t>(y) * W + x) * 4];
        const float da = d[3] / 255.0f, oa = a + da * (1 - a);
        for (int i = 0; i < 3; ++i) d[i] = static_cast<uint8_t>(oa > 0 ? (c[i] * a + d[i] * da * (1 - a)) / oa : 0);
        d[3] = static_cast<uint8_t>(oa * 255);
    };
    auto line = [&](float x0, float y0, float x1, float y1, const uint8_t* c) { // on the texel grid
        int ax = static_cast<int>(std::floor(x0)), ay = static_cast<int>(std::floor(y0));
        const int bx = static_cast<int>(std::floor(x1)), by = static_cast<int>(std::floor(y1));
        const int dx = std::abs(bx - ax), dy = -std::abs(by - ay), sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 100000; ++guard) {
            put(ax, ay, c, 1.0f);
            if (ax == bx && ay == by) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ax += sx; }
            if (e2 <= dx) { err += dx; ay += sy; }
        }
    };
    std::map<std::string, int> color;
    for (const fig::ModelPart& p : model.parts) {
        std::string name = p.name;
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!parts.empty() && !parts.count(name)) continue;
        if (!color.count(name)) {
            color[name] = static_cast<int>(color.size());
            r.legend.push_back(p.name);
        }
        const uint8_t* c = kColors[color[name] % 12];
        const fig::FigureMesh& mesh = p.mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            float u[3], v[3];
            for (int k = 0; k < 3; ++k) {
                const uint16_t idx = mesh.indices[i + k];
                fig::Vec2 uv{0, 0};
                if (idx < mesh.vertexComponents.size() && mesh.vertexComponents[idx].uvIndex < mesh.uvs.size()) uv = mesh.uvs[mesh.vertexComponents[idx].uvIndex];
                u[k] = uv.x / slot * W;
                v[k] = (uv.y - (1.0f - slot)) / slot * H;
            }
            // fill
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({u[0], u[1], u[2]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({v[0], v[1], v[2]}))));
            const float den = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2]);
            if (std::fabs(den) > 1e-9f)
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float l0 = ((v[1] - v[2]) * (px - u[2]) + (u[2] - u[1]) * (py - v[2])) / den;
                        const float l1 = ((v[2] - v[0]) * (px - u[2]) + (u[0] - u[2]) * (py - v[2])) / den;
                        if (l0 >= 0 && l1 >= 0 && l0 + l1 <= 1) put(x, y, c, 0.30f);
                    }
            for (int k = 0; k < 3; ++k) line(u[k], v[k], u[(k + 1) % 3], v[(k + 1) % 3], c);
        }
    }
    return r;
}

} // namespace uvmap
