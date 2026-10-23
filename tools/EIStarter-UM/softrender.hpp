// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// A renderer for Dear ImGui that needs no graphics driver: the draw lists are rasterised on the CPU into a 32-bit image, which the Windows build
// copies to the window with GDI. The launcher uses it when no OpenGL window can be opened (a virtual machine without 3D acceleration, a broken driver,
// a remote desktop) or when started with --software. The picture is 640 x 360, so a plain triangle filler is plenty fast.
//  * textures: the font atlas ImGui builds and the skin images (registered with soft::AddImage); nearest-neighbour sampling, clamped;
//  * triangles: filled with the top-left rule (a quad cut into two triangles covers every pixel once, so a half-transparent quad has no seam);
//  * colour: the vertex colours interpolated (flat when the three are equal) times the texel, blended over the target with straight alpha.
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace launcher::soft {

struct Image {
    const uint8_t* pixels = nullptr; // RGBA8 (Alpha8: one byte a texel)
    std::vector<uint8_t> own;        // the pixels of a skin image (ImGui's textures keep their own)
    int w = 0, h = 0;
    bool alpha8 = false;
};

inline std::vector<Image>& Images() { static std::vector<Image> v(1); return v; } // slot 0 is "no texture"

// A skin image (RGBA8, copied); the returned id is the ImTextureID to draw it with.
inline uint32_t AddImage(const unsigned char* rgba, int w, int h) {
    Image im;
    im.own.assign(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    im.pixels = im.own.data(); im.w = w; im.h = h;
    Images().push_back(std::move(im));
    return static_cast<uint32_t>(Images().size() - 1);
}

struct Target {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // 0x00RRGGBB: what a 32-bit DIB wants
    void Resize(int nw, int nh) { w = nw; h = nh; px.assign(static_cast<size_t>(nw) * static_cast<size_t>(nh), 0); }
};

// The backend's side of ImGui's texture protocol (1.92): the atlas is created / updated / destroyed through the draw data's texture list.
inline void UpdateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        Image im;
        im.pixels = static_cast<const uint8_t*>(tex->GetPixels()); // ImGui keeps the pixels and updates them in place
        im.w = tex->Width; im.h = tex->Height; im.alpha8 = tex->Format == ImTextureFormat_Alpha8;
        Images().push_back(std::move(im));
        tex->SetTexID(static_cast<ImTextureID>(Images().size() - 1));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        const size_t id = static_cast<size_t>(tex->TexID);
        if (id < Images().size()) Images()[id].pixels = static_cast<const uint8_t*>(tex->GetPixels());
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        const size_t id = static_cast<size_t>(tex->TexID);
        if (id < Images().size()) Images()[id].pixels = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

namespace detail {
struct V { float x, y, u, v; float r, g, b, a; };
inline V Load(const ImDrawVert& d) {
    return {d.pos.x, d.pos.y, d.uv.x, d.uv.y, static_cast<float>(d.col & 0xFF), static_cast<float>((d.col >> 8) & 0xFF), static_cast<float>((d.col >> 16) & 0xFF), static_cast<float>(d.col >> 24)};
}
inline float Edge(const V& a, const V& b, float px, float py) { return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x); }
// Top-left rule for a triangle wound so that Edge() is positive inside (screen y points down): an edge owns the pixels exactly on it when it is a
// "top" edge (horizontal, going left) or a "left" edge (going down).
inline bool Owns(const V& a, const V& b) { return (b.y > a.y) || (b.y == a.y && b.x < a.x); }

inline void Triangle(Target& t, const Image* tex, V a, V b, V c, int cx0, int cy0, int cx1, int cy1) {
    float area = Edge(a, b, c.x, c.y);
    if (std::fabs(area) < 1e-6f) return;
    if (area < 0) { std::swap(b, c); area = -area; }
    const int x0 = std::max(cx0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(cx1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(cy0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(cy1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    const bool own0 = Owns(b, c), own1 = Owns(c, a), own2 = Owns(a, b); // w0 is the edge opposite a, ...
    const bool flat = a.r == b.r && b.r == c.r && a.g == b.g && b.g == c.g && a.b == b.b && b.b == c.b && a.a == b.a && b.a == c.a;
    const float inv = 1.0f / area;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float w0 = Edge(b, c, px, py), w1 = Edge(c, a, px, py), w2 = Edge(a, b, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2)) continue;
            float r, g, bl, al;
            if (flat) { r = a.r; g = a.g; bl = a.b; al = a.a; }
            else { const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv; r = a.r * l0 + b.r * l1 + c.r * l2; g = a.g * l0 + b.g * l1 + c.g * l2; bl = a.b * l0 + b.b * l1 + c.b * l2; al = a.a * l0 + b.a * l1 + c.a * l2; }
            if (tex && tex->pixels && tex->w > 0 && tex->h > 0) {
                const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
                const float u = a.u * l0 + b.u * l1 + c.u * l2, v = a.v * l0 + b.v * l1 + c.v * l2;
                const int tx = std::min(tex->w - 1, std::max(0, static_cast<int>(u * static_cast<float>(tex->w)))), ty = std::min(tex->h - 1, std::max(0, static_cast<int>(v * static_cast<float>(tex->h))));
                if (tex->alpha8) al *= static_cast<float>(tex->pixels[static_cast<size_t>(ty) * static_cast<size_t>(tex->w) + static_cast<size_t>(tx)]) * (1.0f / 255.0f);
                else {
                    const uint8_t* p = tex->pixels + (static_cast<size_t>(ty) * static_cast<size_t>(tex->w) + static_cast<size_t>(tx)) * 4;
                    const float k = 1.0f / 255.0f;
                    r *= p[0] * k; g *= p[1] * k; bl *= p[2] * k; al *= p[3] * k;
                }
            }
            const float alpha = al * (1.0f / 255.0f);
            if (alpha <= 0.0f) continue;
            uint32_t& d = t.px[static_cast<size_t>(y) * static_cast<size_t>(t.w) + static_cast<size_t>(x)];
            if (alpha >= 0.999f) { d = (static_cast<uint32_t>(r + 0.5f) << 16) | (static_cast<uint32_t>(g + 0.5f) << 8) | static_cast<uint32_t>(bl + 0.5f); continue; }
            const float dr = static_cast<float>((d >> 16) & 0xFF), dg = static_cast<float>((d >> 8) & 0xFF), db = static_cast<float>(d & 0xFF);
            const uint32_t or_ = static_cast<uint32_t>(r * alpha + dr * (1 - alpha) + 0.5f), og = static_cast<uint32_t>(g * alpha + dg * (1 - alpha) + 0.5f), ob = static_cast<uint32_t>(bl * alpha + db * (1 - alpha) + 0.5f);
            d = (std::min(or_, 255u) << 16) | (std::min(og, 255u) << 8) | std::min(ob, 255u);
        }
    }
}
} // namespace detail

// Draws ImGui's draw data over `t` (cleared to `clear` first, 0x00RRGGBB).
inline void Render(ImDrawData* dd, Target& t, uint32_t clear = 0x2B2B2B) {
    if (!dd) return;
    if (dd->Textures) for (ImTextureData* tex : *dd->Textures) if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
    std::fill(t.px.begin(), t.px.end(), clear);
    for (int n = 0; n < dd->CmdLists.Size; ++n) {
        const ImDrawList* list = dd->CmdLists[n];
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        for (int ci = 0; ci < list->CmdBuffer.Size; ++ci) {
            const ImDrawCmd& cmd = list->CmdBuffer[ci];
            if (cmd.UserCallback) continue; // the launcher uses none
            const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x - dd->DisplayPos.x)), cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y - dd->DisplayPos.y));
            const int cx1 = std::min(t.w, static_cast<int>(std::ceil(cmd.ClipRect.z - dd->DisplayPos.x))), cy1 = std::min(t.h, static_cast<int>(std::ceil(cmd.ClipRect.w - dd->DisplayPos.y)));
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            const size_t id = static_cast<size_t>(cmd.GetTexID());
            const Image* tex = id > 0 && id < Images().size() ? &Images()[id] : nullptr;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert* p = vtx + cmd.VtxOffset;
                detail::Triangle(t, tex, detail::Load(p[idx[cmd.IdxOffset + i]]), detail::Load(p[idx[cmd.IdxOffset + i + 1]]), detail::Load(p[idx[cmd.IdxOffset + i + 2]]), cx0, cy0, cx1, cy1);
            }
        }
    }
}

#ifdef _WIN32
// Copies the image to the window (GDI; a 32-bit top-down DIB).
inline void Present(HWND hwnd, const Target& t) {
    if (!hwnd || t.px.empty()) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = t.w; bi.bmiHeader.biHeight = -t.h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    if (HDC dc = GetDC(hwnd)) {
        SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(t.w), static_cast<DWORD>(t.h), 0, 0, 0, static_cast<UINT>(t.h), t.px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(hwnd, dc);
    }
}
#endif

} // namespace launcher::soft
