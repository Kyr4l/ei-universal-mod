// DirectDraw Surface (.dds) decoder -> RGBA8, for loose texture folders that hold
// DDS files instead of the game's own .mmp. Top mip level only. Supports what the
// game's textures convert to: DXT1, DXT3, DXT5, and uncompressed 16/24/32-bit
// formats described by channel bit masks. The DXT1/DXT3 block decoders are the
// .mmp decoder's own (mmp_texture.hpp).
#pragma once

#include "png_reader.hpp"
#include "../vendor/stb/stb_image.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mmp_texture.hpp"

namespace dds {

// Decodes one 4x4 DXT5 block (16 bytes: 2 alpha endpoints + 3-bit indices, then a DXT1 color block).
inline void DecodeDxt5Block(const uint8_t* block, uint8_t out[4][4][4]) {
    mmp::DecodeDxt1Block(block + 8, out);
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t alpha[8];
    alpha[0] = a0;
    alpha[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) alpha[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i < 5; ++i) alpha[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p) {
        out[p / 4][p % 4][3] = alpha[(bits >> (3 * p)) & 0x7];
    }
    // DXT1's "transparent black" palette entry does not apply inside DXT5: color is always opaque there.
}

// Scales the channel selected by `mask` to 0..255.
inline uint8_t ExtractChannel(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 255;
    int shift = 0;
    while (((mask >> shift) & 1u) == 0) ++shift;
    uint32_t max = mask >> shift;
    uint32_t value = (pixel & mask) >> shift;
    return static_cast<uint8_t>(value * 255u / max);
}

inline bool Decode(const uint8_t* data, size_t size, mmp::Image& out, std::string& err) {
    if (size < 128 || std::memcmp(data, "DDS ", 4) != 0) { err = "not a DDS file"; return false; }
    uint32_t height = mmp::ReadU32LE(data + 12);
    uint32_t width = mmp::ReadU32LE(data + 16);
    uint32_t pfFlags = mmp::ReadU32LE(data + 80);
    const char* fourcc = reinterpret_cast<const char*>(data + 84);
    uint32_t bitCount = mmp::ReadU32LE(data + 88);
    uint32_t rMask = mmp::ReadU32LE(data + 92), gMask = mmp::ReadU32LE(data + 96);
    uint32_t bMask = mmp::ReadU32LE(data + 100), aMask = mmp::ReadU32LE(data + 104);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) { err = "bad DDS size"; return false; }
    const uint8_t* payload = data + 128;
    size_t payloadLen = size - 128;
    out.width = width;
    out.height = height;

    if (pfFlags & 0x4) { // DDPF_FOURCC
        bool dxt1 = std::memcmp(fourcc, "DXT1", 4) == 0;
        bool dxt3 = std::memcmp(fourcc, "DXT3", 4) == 0;
        bool dxt5 = std::memcmp(fourcc, "DXT5", 4) == 0;
        if (!dxt1 && !dxt3 && !dxt5) { err = "unsupported DDS compression " + std::string(fourcc, 4); return false; }
        if (!dxt5) {
            mmp::DecodeDxtGeneric(payload, payloadLen, width, height, dxt3, out.rgba);
            return true;
        }
        out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
        uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        size_t offset = 0;
        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                if (offset + 16 > payloadLen) return true;
                uint8_t patch[4][4][4];
                DecodeDxt5Block(payload + offset, patch);
                offset += 16;
                for (int py = 0; py < 4; ++py) {
                    uint32_t y = by * 4 + py;
                    if (y >= height) continue;
                    for (int px = 0; px < 4; ++px) {
                        uint32_t x = bx * 4 + px;
                        if (x >= width) continue;
                        std::memcpy(&out.rgba[(static_cast<size_t>(y) * width + x) * 4], patch[py][px], 4);
                    }
                }
            }
        }
        return true;
    }

    if (bitCount != 16 && bitCount != 24 && bitCount != 32) { err = "unsupported DDS bit depth"; return false; }
    bool hasAlpha = (pfFlags & 0x1) != 0; // DDPF_ALPHAPIXELS
    size_t bytesPerPixel = bitCount / 8;
    size_t pixelCount = static_cast<size_t>(width) * height;
    if (payloadLen < pixelCount * bytesPerPixel) { err = "truncated DDS data"; return false; }
    out.rgba.assign(pixelCount * 4, 0);
    for (size_t i = 0; i < pixelCount; ++i) {
        uint32_t pixel = 0;
        for (size_t b = 0; b < bytesPerPixel; ++b) pixel |= static_cast<uint32_t>(payload[i * bytesPerPixel + b]) << (8 * b);
        out.rgba[i * 4 + 0] = ExtractChannel(pixel, rMask);
        out.rgba[i * 4 + 1] = ExtractChannel(pixel, gMask);
        out.rgba[i * 4 + 2] = ExtractChannel(pixel, bMask);
        out.rgba[i * 4 + 3] = hasAlpha ? ExtractChannel(pixel, aMask) : 255;
    }
    return true;
}

} // namespace dds

// A texture file of any kind (DDS, MMP, PNG), recognised by its content rather than its name.
inline bool DecodeTextureFile(const std::vector<uint8_t>& bytes, mmp::Image& out, std::string& err) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) return dds::Decode(bytes.data(), bytes.size(), out, err);
    if (pngread::IsPng(bytes)) return pngread::Decode(bytes, out, err); // a picked preview file
    if (!mmp::IsMmp(bytes)) { // any other picture (JPEG, BMP, TGA, GIF...): stb_image
        int w = 0, h = 0, channels = 0;
        unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!px) { err = std::string("not a picture this reads (") + stbi_failure_reason() + ")"; return false; }
        out.width = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
        return true;
    }
    return mmp::Decode(bytes, out, err);
}
