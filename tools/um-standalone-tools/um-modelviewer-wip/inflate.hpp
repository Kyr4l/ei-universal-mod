/**
 * ============================================================================
 * inflate.hpp - Minimal raw DEFLATE (RFC 1951) decoder
 * ============================================================================
 *
 * Self-contained, dependency-free DEFLATE decompressor used to read the
 * compressed entries inside .xlsx files (which are ZIP archives). Handles
 * stored, fixed-Huffman, and dynamic-Huffman blocks. No external libraries
 * (matches the zero-dependency style of the other um-* tools).
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <array>

namespace inflatelib {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Reads `count` bits (0-16), LSB-first, as DEFLATE requires.
    uint32_t GetBits(int count) {
        while (bitCount_ < count) {
            if (pos_ >= size_) {
                throw std::runtime_error("inflate: unexpected end of input");
            }
            bitBuf_ |= static_cast<uint32_t>(data_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        uint32_t result = bitBuf_ & ((1u << count) - 1u);
        bitBuf_ >>= count;
        bitCount_ -= count;
        return result;
    }

    void AlignToByte() {
        bitBuf_ = 0;
        bitCount_ = 0;
    }

    uint8_t ReadByte() {
        if (pos_ >= size_) throw std::runtime_error("inflate: unexpected end of input");
        return data_[pos_++];
    }

    size_t Position() const { return pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
};

// Canonical Huffman decode table built from a list of code lengths (one per symbol).
struct HuffTable {
    std::vector<uint16_t> counts;  // number of codes of each length
    std::vector<uint16_t> symbols; // symbols sorted by (length, symbol)

    void Build(const std::vector<uint8_t>& lengths) {
        int maxLen = 0;
        for (uint8_t l : lengths) maxLen = std::max<int>(maxLen, l);
        counts.assign(maxLen + 1, 0);
        for (uint8_t l : lengths) if (l > 0) counts[l]++;

        std::vector<uint16_t> offsets(maxLen + 2, 0);
        for (int i = 1; i <= maxLen; ++i) offsets[i + 1] = offsets[i] + counts[i];

        symbols.assign(lengths.size(), 0);
        for (size_t sym = 0; sym < lengths.size(); ++sym) {
            if (lengths[sym] > 0) {
                symbols[offsets[lengths[sym]]++] = static_cast<uint16_t>(sym);
            }
        }
    }

    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (size_t len = 1; len < counts.size(); ++len) {
            code |= static_cast<int>(br.GetBits(1));
            int count = counts[len];
            if (code - first < count) {
                return symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: invalid Huffman code");
    }
};

inline void BuildFixedTables(HuffTable& litTable, HuffTable& distTable) {
    std::vector<uint8_t> litLens(288);
    for (int i = 0; i < 144; ++i) litLens[i] = 8;
    for (int i = 144; i < 256; ++i) litLens[i] = 9;
    for (int i = 256; i < 280; ++i) litLens[i] = 7;
    for (int i = 280; i < 288; ++i) litLens[i] = 8;
    litTable.Build(litLens);

    std::vector<uint8_t> distLens(30, 5);
    distTable.Build(distLens);
}

static const uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

inline void ReadDynamicTables(BitReader& br, HuffTable& litTable, HuffTable& distTable) {
    int hlit = br.GetBits(5) + 257;
    int hdist = br.GetBits(5) + 1;
    int hclen = br.GetBits(4) + 4;

    std::vector<uint8_t> clLens(19, 0);
    for (int i = 0; i < hclen; ++i) {
        clLens[kCodeLengthOrder[i]] = static_cast<uint8_t>(br.GetBits(3));
    }
    HuffTable clTable;
    clTable.Build(clLens);

    std::vector<uint8_t> lens;
    lens.reserve(hlit + hdist);
    while (static_cast<int>(lens.size()) < hlit + hdist) {
        int sym = clTable.Decode(br);
        if (sym < 16) {
            lens.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 16) {
            if (lens.empty()) throw std::runtime_error("inflate: repeat with no previous length");
            uint8_t prev = lens.back();
            int rep = 3 + br.GetBits(2);
            for (int i = 0; i < rep; ++i) lens.push_back(prev);
        } else if (sym == 17) {
            int rep = 3 + br.GetBits(3);
            for (int i = 0; i < rep; ++i) lens.push_back(0);
        } else {
            int rep = 11 + br.GetBits(7);
            for (int i = 0; i < rep; ++i) lens.push_back(0);
        }
    }
    if (static_cast<int>(lens.size()) != hlit + hdist) {
        throw std::runtime_error("inflate: bad dynamic Huffman code length count");
    }

    std::vector<uint8_t> litLens(lens.begin(), lens.begin() + hlit);
    std::vector<uint8_t> distLens(lens.begin() + hlit, lens.end());
    litTable.Build(litLens);
    distTable.Build(distLens);
}

// Decompresses a raw DEFLATE stream (no zlib/gzip wrapper) of `inSize` bytes at `in`.
inline std::vector<uint8_t> InflateRaw(const uint8_t* in, size_t inSize, size_t expectedOutSize = 0) {
    BitReader br(in, inSize);
    std::vector<uint8_t> out;
    if (expectedOutSize > 0) out.reserve(expectedOutSize);

    bool final = false;
    while (!final) {
        final = br.GetBits(1) != 0;
        uint32_t type = br.GetBits(2);

        if (type == 0) {
            // Stored block: skip to byte boundary, then LEN/NLEN + raw bytes.
            br.AlignToByte();
            uint8_t lenLo = br.ReadByte();
            uint8_t lenHi = br.ReadByte();
            br.ReadByte(); // NLEN low (ignored)
            br.ReadByte(); // NLEN high (ignored)
            uint16_t len = static_cast<uint16_t>(lenLo | (lenHi << 8));
            for (uint16_t i = 0; i < len; ++i) out.push_back(br.ReadByte());
            continue;
        }

        HuffTable litTable, distTable;
        if (type == 1) {
            BuildFixedTables(litTable, distTable);
        } else if (type == 2) {
            ReadDynamicTables(br, litTable, distTable);
        } else {
            throw std::runtime_error("inflate: invalid block type");
        }

        while (true) {
            int sym = litTable.Decode(br);
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break; // end of block
            } else {
                int idx = sym - 257;
                if (idx >= 29) throw std::runtime_error("inflate: invalid length symbol");
                int length = kLengthBase[idx] + static_cast<int>(br.GetBits(kLengthExtra[idx]));
                int distSym = distTable.Decode(br);
                if (distSym >= 30) throw std::runtime_error("inflate: invalid distance symbol");
                int distance = kDistBase[distSym] + static_cast<int>(br.GetBits(kDistExtra[distSym]));
                if (static_cast<size_t>(distance) > out.size()) {
                    throw std::runtime_error("inflate: distance too far back");
                }
                size_t start = out.size() - distance;
                for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
            }
        }
    }
    return out;
}

} // namespace inflatelib
