// Animated GIF writer (GIF89a, looping): RGBA frames in, one file out. No GL here.
//
// One palette for the whole animation, so colours do not flicker from frame to frame:
// the opaque pixels of every frame are counted in 15-bit colour bins, and the bins are
// split by median cut into 255 colours (256 when there is no transparency). A pixel with
// alpha < 128 becomes the transparent index 0 - GIF transparency is on/off per pixel.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gif {

struct Frame {
    std::vector<uint8_t> rgba; // width * height * 4, top row first
};

namespace detail {

inline uint16_t Bin(const uint8_t* p) { return static_cast<uint16_t>(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)); }

struct BinStats { uint64_t count = 0, r = 0, g = 0, b = 0; };

struct Box {
    std::vector<uint16_t> bins;
    uint64_t weight = 0;
    int SpanAxis(int& span) const {
        int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
        for (uint16_t b : bins) {
            int c[3] = {(b >> 10) & 31, (b >> 5) & 31, b & 31};
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
        }
        int axis = 0;
        span = -1;
        for (int k = 0; k < 3; ++k) if (hi[k] - lo[k] > span) { span = hi[k] - lo[k]; axis = k; }
        return axis;
    }
};

// Median cut over the used 15-bit bins; returns up to maxColors RGB triples, and the bin -> index map.
inline std::vector<std::array<uint8_t, 3>> BuildPalette(const std::vector<BinStats>& stats, int maxColors,
                                                         std::vector<int16_t>& binToIndex) {
    std::vector<Box> boxes(1);
    for (int b = 0; b < 32768; ++b) {
        if (stats[b].count) { boxes[0].bins.push_back(static_cast<uint16_t>(b)); boxes[0].weight += stats[b].count; }
    }
    std::vector<std::array<uint8_t, 3>> palette;
    binToIndex.assign(32768, -1);
    if (boxes[0].bins.empty()) return palette;
    while (static_cast<int>(boxes.size()) < maxColors) {
        // Split the box with the most pixels that can still be split.
        int best = -1;
        uint64_t bestWeight = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].bins.size() > 1 && boxes[i].weight > bestWeight) { bestWeight = boxes[i].weight; best = static_cast<int>(i); }
        }
        if (best < 0) break;
        Box& box = boxes[best];
        int span;
        int axis = box.SpanAxis(span);
        int shift = axis == 0 ? 10 : axis == 1 ? 5 : 0;
        std::sort(box.bins.begin(), box.bins.end(), [shift](uint16_t a, uint16_t b) { return ((a >> shift) & 31) < ((b >> shift) & 31); });
        uint64_t half = box.weight / 2, running = 0;
        size_t cut = 1;
        for (size_t i = 0; i < box.bins.size() - 1; ++i) {
            running += stats[box.bins[i]].count;
            cut = i + 1;
            if (running >= half) break;
        }
        Box other;
        other.bins.assign(box.bins.begin() + static_cast<long>(cut), box.bins.end());
        box.bins.resize(cut);
        box.weight = 0;
        for (uint16_t b : box.bins) box.weight += stats[b].count;
        for (uint16_t b : other.bins) other.weight += stats[b].count;
        boxes.push_back(std::move(other));
    }
    for (const Box& box : boxes) {
        uint64_t n = 0, r = 0, g = 0, b = 0;
        for (uint16_t bin : box.bins) { n += stats[bin].count; r += stats[bin].r; g += stats[bin].g; b += stats[bin].b; }
        if (!n) continue;
        palette.push_back({static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n), static_cast<uint8_t>(b / n)});
    }
    // Each used bin maps to its nearest palette colour (not simply its box: boxes overlap after averaging).
    for (int bin = 0; bin < 32768; ++bin) {
        if (!stats[bin].count) continue;
        int r = static_cast<int>(stats[bin].r / stats[bin].count), g = static_cast<int>(stats[bin].g / stats[bin].count),
            b = static_cast<int>(stats[bin].b / stats[bin].count);
        int bestIndex = 0, bestDistance = 1 << 30;
        for (size_t i = 0; i < palette.size(); ++i) {
            int dr = r - palette[i][0], dg = g - palette[i][1], db = b - palette[i][2];
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bestDistance) { bestDistance = d; bestIndex = static_cast<int>(i); }
        }
        binToIndex[bin] = static_cast<int16_t>(bestIndex);
    }
    return palette;
}

// Variable-width LZW as GIF wants it (min code size 8), written as sub-blocks of <= 255 bytes.
inline void WriteLzw(std::ofstream& f, const std::vector<uint8_t>& indices) {
    const int minCodeSize = 8, clearCode = 1 << minCodeSize, endCode = clearCode + 1;
    std::vector<uint8_t> out;
    uint32_t bitBuffer = 0;
    int bitCount = 0, codeSize = minCodeSize + 1, nextCode = endCode + 1;
    auto emit = [&](int code) {
        bitBuffer |= static_cast<uint32_t>(code) << bitCount;
        bitCount += codeSize;
        while (bitCount >= 8) { out.push_back(static_cast<uint8_t>(bitBuffer & 0xFF)); bitBuffer >>= 8; bitCount -= 8; }
    };
    std::unordered_map<uint32_t, int> dictionary;
    dictionary.reserve(8192);
    emit(clearCode);
    if (!indices.empty()) {
        int prefix = indices[0];
        for (size_t i = 1; i < indices.size(); ++i) {
            uint32_t key = (static_cast<uint32_t>(prefix) << 8) | indices[i];
            auto it = dictionary.find(key);
            if (it != dictionary.end()) { prefix = it->second; continue; }
            emit(prefix);
            if (nextCode < 4096) {
                dictionary[key] = nextCode++;
                if (nextCode > (1 << codeSize) && codeSize < 12) ++codeSize;
            } else {
                emit(clearCode);
                dictionary.clear();
                codeSize = minCodeSize + 1;
                nextCode = endCode + 1;
            }
            prefix = indices[i];
        }
        emit(prefix);
    }
    emit(endCode);
    if (bitCount > 0) out.push_back(static_cast<uint8_t>(bitBuffer & 0xFF));
    f.put(static_cast<char>(minCodeSize));
    for (size_t pos = 0; pos < out.size(); pos += 255) {
        size_t n = std::min<size_t>(255, out.size() - pos);
        f.put(static_cast<char>(n));
        f.write(reinterpret_cast<const char*>(out.data() + pos), static_cast<std::streamsize>(n));
    }
    f.put(0);
}

inline void Put16(std::ofstream& f, int v) { f.put(static_cast<char>(v & 0xFF)); f.put(static_cast<char>((v >> 8) & 0xFF)); }

} // namespace detail

// delayCs: time per frame in 1/100 s (GIF's unit).
inline bool Write(const std::string& path, int width, int height, const std::vector<Frame>& frames, int delayCs,
                  bool transparent, std::string& err) {
    if (frames.empty() || width <= 0 || height <= 0) { err = "nothing to write"; return false; }
    std::vector<detail::BinStats> stats(32768);
    for (const Frame& fr : frames) {
        for (size_t p = 0; p + 4 <= fr.rgba.size(); p += 4) {
            const uint8_t* px = &fr.rgba[p];
            if (transparent && px[3] < 128) continue;
            detail::BinStats& s = stats[detail::Bin(px)];
            ++s.count; s.r += px[0]; s.g += px[1]; s.b += px[2];
        }
    }
    std::vector<int16_t> binToIndex;
    auto palette = detail::BuildPalette(stats, transparent ? 255 : 256, binToIndex);
    const int offset = transparent ? 1 : 0; // index 0 is the transparent colour

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) { err = "cannot write " + path; return false; }
    f.write("GIF89a", 6);
    detail::Put16(f, width);
    detail::Put16(f, height);
    f.put(static_cast<char>(0xF7)); // global colour table, 8-bit, 256 entries
    f.put(0);
    f.put(0);
    for (int i = 0; i < 256; ++i) {
        int k = i - offset;
        std::array<uint8_t, 3> c = (k >= 0 && k < static_cast<int>(palette.size())) ? palette[k] : std::array<uint8_t, 3>{0, 0, 0};
        f.put(static_cast<char>(c[0])); f.put(static_cast<char>(c[1])); f.put(static_cast<char>(c[2]));
    }
    // Loop forever.
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    f.write(reinterpret_cast<const char*>(loop), sizeof(loop));

    std::vector<uint8_t> indices(static_cast<size_t>(width) * height);
    for (const Frame& fr : frames) {
        for (size_t i = 0; i < indices.size(); ++i) {
            const uint8_t* px = &fr.rgba[i * 4];
            indices[i] = (transparent && px[3] < 128) ? 0 : static_cast<uint8_t>(binToIndex[detail::Bin(px)] + offset);
        }
        // Graphic control: restore to background after each frame (so transparent parts do not keep
        // the previous frame), delay, transparent index 0.
        f.put(0x21); f.put(static_cast<char>(0xF9)); f.put(4);
        f.put(static_cast<char>((2 << 2) | (transparent ? 1 : 0)));
        detail::Put16(f, delayCs);
        f.put(0); f.put(0);
        f.put(0x2C);
        detail::Put16(f, 0); detail::Put16(f, 0); detail::Put16(f, width); detail::Put16(f, height);
        f.put(0);
        detail::WriteLzw(f, indices);
    }
    f.put(0x3B);
    if (!f.good()) { err = "write error on " + path; return false; }
    return true;
}

} // namespace gif
