/**
 * ============================================================================
 * zip_reader.hpp - Minimal ZIP container reader
 * ============================================================================
 *
 * Reads a .zip (used as the .xlsx container format) well enough to locate
 * and extract named entries by walking the End-Of-Central-Directory record
 * and Central Directory File Headers. Supports store (method 0) and deflate
 * (method 8, via inflate.hpp) compression. No external libraries.
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "inflate.hpp"

namespace ziplib {

inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
inline uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct ZipEntry {
    uint16_t method = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
};

class ZipArchive {
public:
    explicit ZipArchive(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) throw std::runtime_error("zip: cannot open file: " + path);
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        data_.resize(static_cast<size_t>(size));
        if (size > 0) in.read(reinterpret_cast<char*>(data_.data()), size);
        in.close();

        ParseCentralDirectory();
    }

    bool Has(const std::string& name) const { return entries_.count(name) != 0; }

    std::vector<uint8_t> Read(const std::string& name) const {
        auto it = entries_.find(name);
        if (it == entries_.end()) throw std::runtime_error("zip: entry not found: " + name);
        const ZipEntry& e = it->second;

        const uint8_t* lh = data_.data() + e.localHeaderOffset;
        if (ReadU32(lh) != 0x04034b50u) throw std::runtime_error("zip: bad local file header for " + name);
        uint16_t nameLen = ReadU16(lh + 26);
        uint16_t extraLen = ReadU16(lh + 28);
        const uint8_t* fileData = lh + 30 + nameLen + extraLen;

        if (e.method == 0) {
            return std::vector<uint8_t>(fileData, fileData + e.uncompressedSize);
        } else if (e.method == 8) {
            return inflatelib::InflateRaw(fileData, e.compressedSize, e.uncompressedSize);
        }
        throw std::runtime_error("zip: unsupported compression method (" + std::to_string(e.method) + ") for " + name);
    }

    std::string ReadString(const std::string& name) const {
        auto bytes = Read(name);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

private:
    void ParseCentralDirectory() {
        // Locate End-Of-Central-Directory record by scanning backward for its signature.
        if (data_.size() < 22) throw std::runtime_error("zip: file too small");
        size_t maxBack = std::min<size_t>(data_.size(), 22 + 65536);
        size_t eocdPos = std::string::npos;
        for (size_t i = data_.size() - 22; i + 4 <= data_.size(); ) {
            if (ReadU32(&data_[i]) == 0x06054b50u) { eocdPos = i; break; }
            if (i == 0 || data_.size() - i >= maxBack) break;
            --i;
        }
        if (eocdPos == std::string::npos) throw std::runtime_error("zip: end-of-central-directory not found");

        uint16_t entryCount = ReadU16(&data_[eocdPos + 10]);
        uint32_t cdOffset = ReadU32(&data_[eocdPos + 16]);

        size_t pos = cdOffset;
        for (uint16_t i = 0; i < entryCount; ++i) {
            if (pos + 46 > data_.size() || ReadU32(&data_[pos]) != 0x02014b50u) {
                throw std::runtime_error("zip: bad central directory entry");
            }
            uint16_t method = ReadU16(&data_[pos + 10]);
            uint32_t compSize = ReadU32(&data_[pos + 20]);
            uint32_t uncompSize = ReadU32(&data_[pos + 24]);
            uint16_t nameLen = ReadU16(&data_[pos + 28]);
            uint16_t extraLen = ReadU16(&data_[pos + 30]);
            uint16_t commentLen = ReadU16(&data_[pos + 32]);
            uint32_t localOffset = ReadU32(&data_[pos + 42]);

            std::string name(reinterpret_cast<const char*>(&data_[pos + 46]), nameLen);

            ZipEntry entry;
            entry.method = method;
            entry.compressedSize = compSize;
            entry.uncompressedSize = uncompSize;
            entry.localHeaderOffset = localOffset;
            entries_[name] = entry;

            pos += 46 + nameLen + extraLen + commentLen;
        }
    }

    std::vector<uint8_t> data_;
    std::map<std::string, ZipEntry> entries_;
};

} // namespace ziplib
