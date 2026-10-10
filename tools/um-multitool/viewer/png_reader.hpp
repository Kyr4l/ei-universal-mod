// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Reads a PNG into RGBA8 (for previewing a picked texture file): 8-bit grey, grey+alpha, RGB, RGBA and
// palette images (with tRNS transparency), 16-bit ones reduced to 8 bits, not interlaced. The pixel
// data is zlib: its 2-byte header is skipped and the rest inflated with inflate.hpp.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../inflate.hpp"
#include "mmp_texture.hpp"

namespace pngread {

inline bool IsPng(const std::vector<uint8_t>& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return b.size() >= 8 && std::memcmp(b.data(), sig, 8) == 0;
}

inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

inline bool Decode(const std::vector<uint8_t>& b, mmp::Image& out, std::string& err) {
    if (!IsPng(b)) { err = "not a PNG"; return false; }
    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (size_t pos = 8; pos + 12 <= b.size();) {
        const uint32_t len = Be32(b.data() + pos);
        const std::string type(reinterpret_cast<const char*>(b.data() + pos + 4), 4);
        if (pos + 12 + len > b.size()) { err = "PNG chunk runs past the end"; return false; }
        const uint8_t* d = b.data() + pos + 8;
        if (type == "IHDR" && len >= 13) { w = Be32(d); h = Be32(d + 4); depth = d[8]; color = d[9]; interlace = d[12]; }
        else if (type == "PLTE") palette.assign(d, d + len);
        else if (type == "tRNS") trns.assign(d, d + len);
        else if (type == "IDAT") idat.insert(idat.end(), d, d + len);
        else if (type == "IEND") break;
        pos += 12 + len;
    }
    if (!w || !h || w > 16384 || h > 16384) { err = "PNG size missing or too large"; return false; }
    if (interlace) { err = "interlaced PNG (save it without interlacing)"; return false; }
    if (depth != 8 && depth != 16) { err = "PNG of " + std::to_string(depth) + "-bit channels (8 or 16 are read)"; return false; }
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!channels) { err = "unknown PNG colour type"; return false; }
    if (color == 3 && depth != 8) { err = "16-bit palette PNG"; return false; }
    if (idat.size() < 2) { err = "PNG without pixel data"; return false; }
    std::vector<uint8_t> raw;
    const size_t bpp = static_cast<size_t>(channels) * (depth / 8), stride = bpp * w;
    try {
        raw = inflatelib::InflateRaw(idat.data() + 2, idat.size() - 2, (stride + 1) * h);
    } catch (const std::exception& e) {
        err = std::string("PNG data: ") + e.what();
        return false;
    }
    if (raw.size() < (stride + 1) * h) { err = "PNG data is short"; return false; }
    // Undo the per-row filters (none, sub, up, average, Paeth).
    std::vector<uint8_t> px(stride * h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t f = raw[y * (stride + 1)];
        const uint8_t* src = raw.data() + y * (stride + 1) + 1;
        uint8_t* row = px.data() + y * stride;
        const uint8_t* prev = y ? row - stride : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0, up = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = src[i];
            switch (f) {
            case 1: v += a; break;
            case 2: v += up; break;
            case 3: v += (a + up) / 2; break;
            case 4: {
                const int p = a + up - c, pa = std::abs(p - a), pb = std::abs(p - up), pc = std::abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c);
                break;
            }
            default: break;
            }
            row[i] = static_cast<uint8_t>(v);
        }
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    const size_t step = depth / 8; // 16-bit: the high byte
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t* s = px.data() + i * bpp;
        uint8_t* o = out.rgba.data() + i * 4;
        auto ch = [&](int k) { return s[k * step]; };
        switch (color) {
        case 0: o[0] = o[1] = o[2] = ch(0); break;
        case 4: o[0] = o[1] = o[2] = ch(0); o[3] = ch(1); break;
        case 2: o[0] = ch(0); o[1] = ch(1); o[2] = ch(2); break;
        case 6: o[0] = ch(0); o[1] = ch(1); o[2] = ch(2); o[3] = ch(3); break;
        case 3: {
            const size_t idx = s[0];
            if (idx * 3 + 2 < palette.size()) { o[0] = palette[idx * 3]; o[1] = palette[idx * 3 + 1]; o[2] = palette[idx * 3 + 2]; }
            if (idx < trns.size()) o[3] = trns[idx];
            break;
        }
        }
    }
    return true;
}

} // namespace pngread
