// Evil Islands .mmp texture decoder -> raw RGBA8 pixels.
// Header layout and FourCC formats per tools/um-multitool/ddsmmp.cpp / docs/file-formats/mmp-format.md.
// DXT1/DXT3 are software-decompressed here (rather than uploaded as GL compressed
// textures) to avoid depending on GL_EXT_texture_compression_s3tc under Wine/older drivers.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace mmp {

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint32_t width;
    uint32_t height;
    uint32_t mipsOrDataLen;
    char fourcc[4];
    uint32_t bitDepth;
    uint32_t alphaMask, alphaShift, alphaBits;
    uint32_t redMask, redShift, redBits;
    uint32_t greenMask, greenShift, greenBits;
    uint32_t blueMask, blueShift, blueBits;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 76, "mmp::Header must be exactly 76 bytes");

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // width*height*4, RGBA8 top-to-bottom
};

inline uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool FourCcIs(const char fourcc[4], const char* s) {
    return std::memcmp(fourcc, s, 4) == 0;
}

// Decodes one 4x4 DXT1 block (8 bytes) into a 4x4 RGBA8 patch.
inline void DecodeDxt1Block(const uint8_t* block, uint8_t out[4][4][4]) {
    uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    uint32_t codes = ReadU32LE(block + 4);

    auto unpack565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
        r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
        b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
    };
    uint8_t r0, g0, b0, r1, g1, b1;
    unpack565(c0, r0, g0, b0);
    unpack565(c1, r1, g1, b1);

    uint8_t palette[4][4]; // [index][r,g,b,a]
    palette[0][0] = r0; palette[0][1] = g0; palette[0][2] = b0; palette[0][3] = 255;
    palette[1][0] = r1; palette[1][1] = g1; palette[1][2] = b1; palette[1][3] = 255;
    if (c0 > c1) {
        palette[2][0] = static_cast<uint8_t>((2 * r0 + r1) / 3);
        palette[2][1] = static_cast<uint8_t>((2 * g0 + g1) / 3);
        palette[2][2] = static_cast<uint8_t>((2 * b0 + b1) / 3);
        palette[2][3] = 255;
        palette[3][0] = static_cast<uint8_t>((r0 + 2 * r1) / 3);
        palette[3][1] = static_cast<uint8_t>((g0 + 2 * g1) / 3);
        palette[3][2] = static_cast<uint8_t>((b0 + 2 * b1) / 3);
        palette[3][3] = 255;
    } else {
        palette[2][0] = static_cast<uint8_t>((r0 + r1) / 2);
        palette[2][1] = static_cast<uint8_t>((g0 + g1) / 2);
        palette[2][2] = static_cast<uint8_t>((b0 + b1) / 2);
        palette[2][3] = 255;
        palette[3][0] = 0; palette[3][1] = 0; palette[3][2] = 0; palette[3][3] = 0; // transparent black
    }
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            int idx = (codes >> (2 * (py * 4 + px))) & 0x3;
            out[py][px][0] = palette[idx][0];
            out[py][px][1] = palette[idx][1];
            out[py][px][2] = palette[idx][2];
            out[py][px][3] = palette[idx][3];
        }
    }
}

// Decodes one 4x4 DXT3 block (16 bytes: 8 explicit alpha + 8 DXT1-style color) into RGBA8.
inline void DecodeDxt3Block(const uint8_t* block, uint8_t out[4][4][4]) {
    DecodeDxt1Block(block + 8, out);
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            int bitOffset = (py * 4 + px) * 4;
            int byteIdx = bitOffset / 8;
            int nibbleShift = bitOffset % 8;
            uint8_t nibble = (block[byteIdx] >> nibbleShift) & 0xF;
            out[py][px][3] = static_cast<uint8_t>(nibble * 17); // 0..15 -> 0..255
        }
    }
}

inline void DecodeDxtGeneric(const uint8_t* payload, size_t payloadLen, uint32_t width, uint32_t height,
                              bool isDxt3, std::vector<uint8_t>& outRgba) {
    outRgba.assign(static_cast<size_t>(width) * height * 4, 0);
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    size_t blockSize = isDxt3 ? 16 : 8;
    size_t offset = 0;
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            if (offset + blockSize > payloadLen) return;
            uint8_t patch[4][4][4];
            if (isDxt3) DecodeDxt3Block(payload + offset, patch);
            else DecodeDxt1Block(payload + offset, patch);
            offset += blockSize;
            for (int py = 0; py < 4; ++py) {
                uint32_t y = by * 4 + py;
                if (y >= height) continue;
                for (int px = 0; px < 4; ++px) {
                    uint32_t x = bx * 4 + px;
                    if (x >= width) continue;
                    uint8_t* dst = &outRgba[(static_cast<size_t>(y) * width + x) * 4];
                    dst[0] = patch[py][px][0];
                    dst[1] = patch[py][px][1];
                    dst[2] = patch[py][px][2];
                    dst[3] = patch[py][px][3];
                }
            }
        }
    }
}

// PNT3: 32-bit BGRA with 16-byte-aligned zero-run compression (see ddsmmp.cpp DecodePnt3).
inline std::vector<uint8_t> DecodePnt3(const uint8_t* payload, size_t payloadLen, size_t expectedByteCount) {
    std::vector<uint8_t> out;
    out.reserve(expectedByteCount);
    size_t pos = 0;
    while (pos < payloadLen && out.size() < expectedByteCount) {
        if (pos + 4 > payloadLen) {
            out.insert(out.end(), payload + pos, payload + payloadLen);
            break;
        }
        uint32_t val = ReadU32LE(payload + pos);
        pos += 4;
        if ((out.size() % 16) == 0 && val > 0 && (val % 16) == 0 && (val >> 24) == 0) {
            out.resize(out.size() + val, 0);
        } else {
            uint8_t px[4] = {
                static_cast<uint8_t>(val & 0xFF), static_cast<uint8_t>((val >> 8) & 0xFF),
                static_cast<uint8_t>((val >> 16) & 0xFF), static_cast<uint8_t>((val >> 24) & 0xFF)};
            out.insert(out.end(), px, px + 4);
        }
    }
    if (out.size() < expectedByteCount) out.resize(expectedByteCount, 0);
    return out;
}

inline bool Decode(const uint8_t* data, size_t size, Image& out, std::string& err) {
    if (size < sizeof(Header)) { err = "file too small for MMP header"; return false; }
    Header hdr;
    std::memcpy(&hdr, data, sizeof(Header));
    if (std::memcmp(hdr.magic, "MMP\0", 4) != 0) { err = "bad MMP magic"; return false; }

    out.width = hdr.width;
    out.height = hdr.height;
    const uint8_t* payload = data + sizeof(Header);
    size_t payloadLen = size - sizeof(Header);

    if (FourCcIs(hdr.fourcc, "DXT1")) {
        DecodeDxtGeneric(payload, payloadLen, hdr.width, hdr.height, false, out.rgba);
        return true;
    }
    if (FourCcIs(hdr.fourcc, "DXT3")) {
        DecodeDxtGeneric(payload, payloadLen, hdr.width, hdr.height, true, out.rgba);
        return true;
    }
    if (FourCcIs(hdr.fourcc, "PNT3")) {
        size_t expected = static_cast<size_t>(hdr.width) * hdr.height * 4;
        std::vector<uint8_t> bgra = DecodePnt3(payload, payloadLen, expected);
        out.rgba.resize(expected);
        for (size_t i = 0; i + 4 <= expected; i += 4) {
            out.rgba[i + 0] = bgra[i + 2];
            out.rgba[i + 1] = bgra[i + 1];
            out.rgba[i + 2] = bgra[i + 0];
            out.rgba[i + 3] = bgra[i + 3];
        }
        return true;
    }
    if (hdr.fourcc[0] == 'Q' && hdr.fourcc[1] == 'U') { // RGBA5551
        size_t pixelCount = static_cast<size_t>(hdr.width) * hdr.height;
        out.rgba.assign(pixelCount * 4, 0);
        for (size_t i = 0; i < pixelCount && (i * 2 + 2) <= payloadLen; ++i) {
            uint16_t p = static_cast<uint16_t>(payload[i * 2] | (payload[i * 2 + 1] << 8));
            uint8_t r = static_cast<uint8_t>(((p >> 10) & 0x1F) * 255 / 31);
            uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x1F) * 255 / 31);
            uint8_t b = static_cast<uint8_t>((p & 0x1F) * 255 / 31);
            uint8_t a = static_cast<uint8_t>(((p >> 15) & 0x1) ? 255 : 0);
            out.rgba[i * 4 + 0] = r; out.rgba[i * 4 + 1] = g;
            out.rgba[i * 4 + 2] = b; out.rgba[i * 4 + 3] = a;
        }
        return true;
    }
    if (static_cast<uint8_t>(hdr.fourcc[0]) == 0x88 && static_cast<uint8_t>(hdr.fourcc[1]) == 0x88) {
        // Uncompressed 32-bit BGRA, no run-length compression (used by UI bitmaps
        // like cursors/logos rather than model textures) - same byte layout as
        // PNT3 minus the RLE, so a plain byte-order swap is all that's needed.
        size_t expected = static_cast<size_t>(hdr.width) * hdr.height * 4;
        out.rgba.assign(expected, 0);
        for (size_t i = 0; i + 4 <= expected && i + 4 <= payloadLen; i += 4) {
            out.rgba[i + 0] = payload[i + 2];
            out.rgba[i + 1] = payload[i + 1];
            out.rgba[i + 2] = payload[i + 0];
            out.rgba[i + 3] = payload[i + 3];
        }
        return true;
    }
    if (hdr.fourcc[0] == 'D' && hdr.fourcc[1] == 'D' && hdr.fourcc[2] == '\0') { // ARGB4444
        // Not currently handled by tools/um-multitool/ddsmmp.cpp either - a small
        // set of vanilla effect/particle textures (e.g. antimagic.mmp, astral.mmp)
        // use this 16-bit uncompressed format instead of QU (5551) or PV (565).
        size_t pixelCount = static_cast<size_t>(hdr.width) * hdr.height;
        out.rgba.assign(pixelCount * 4, 0);
        for (size_t i = 0; i < pixelCount && (i * 2 + 2) <= payloadLen; ++i) {
            uint16_t p = static_cast<uint16_t>(payload[i * 2] | (payload[i * 2 + 1] << 8));
            uint8_t a = static_cast<uint8_t>(((p >> 12) & 0xF) * 17);
            uint8_t r = static_cast<uint8_t>(((p >> 8) & 0xF) * 17);
            uint8_t g = static_cast<uint8_t>(((p >> 4) & 0xF) * 17);
            uint8_t b = static_cast<uint8_t>((p & 0xF) * 17);
            out.rgba[i * 4 + 0] = r; out.rgba[i * 4 + 1] = g;
            out.rgba[i * 4 + 2] = b; out.rgba[i * 4 + 3] = a;
        }
        return true;
    }
    if (hdr.fourcc[0] == 'P' && hdr.fourcc[1] == 'V') { // RGB565
        size_t pixelCount = static_cast<size_t>(hdr.width) * hdr.height;
        out.rgba.assign(pixelCount * 4, 0);
        for (size_t i = 0; i < pixelCount && (i * 2 + 2) <= payloadLen; ++i) {
            uint16_t p = static_cast<uint16_t>(payload[i * 2] | (payload[i * 2 + 1] << 8));
            uint8_t r = static_cast<uint8_t>(((p >> 11) & 0x1F) * 255 / 31);
            uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x3F) * 255 / 63);
            uint8_t b = static_cast<uint8_t>((p & 0x1F) * 255 / 31);
            out.rgba[i * 4 + 0] = r; out.rgba[i * 4 + 1] = g;
            out.rgba[i * 4 + 2] = b; out.rgba[i * 4 + 3] = 255;
        }
        return true;
    }

    err = "unsupported MMP fourcc";
    return false;
}

inline bool Decode(const std::vector<uint8_t>& bytes, Image& out, std::string& err) {
    return Decode(bytes.data(), bytes.size(), out, err);
}

} // namespace mmp
