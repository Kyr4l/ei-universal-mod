// In-memory Evil Islands .res archive reader.
// See docs/file-formats/res-format.md for the on-disk layout this decodes.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace res {

static constexpr uint32_t kResMagic = 0x019CE23Cu;

struct Archive {
    // Keyed by lowercase name for case-insensitive lookup; original-case name kept alongside.
    struct Entry {
        std::string originalName;
        std::vector<uint8_t> data;
    };
    std::map<std::string, Entry> entries;

    static std::string ToLower(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    const std::vector<uint8_t>* Find(const std::string& name) const {
        auto it = entries.find(ToLower(name));
        return it != entries.end() ? &it->second.data : nullptr;
    }

    bool Contains(const std::string& name) const {
        return entries.find(ToLower(name)) != entries.end();
    }
};

inline bool ParseArchive(const uint8_t* data, size_t size, Archive& out, std::string& err) {
    if (size < 16) { err = "too small for RES header"; return false; }
    uint32_t magic, numFiles, tableOffset, namesLength;
    std::memcpy(&magic, data + 0, 4);
    std::memcpy(&numFiles, data + 4, 4);
    std::memcpy(&tableOffset, data + 8, 4);
    std::memcpy(&namesLength, data + 12, 4);
    if (magic != kResMagic) { err = "bad RES magic"; return false; }
    if (static_cast<size_t>(tableOffset) > size || static_cast<size_t>(namesLength) > size ||
        tableOffset + static_cast<size_t>(numFiles) * 22 > size) {
        err = "corrupted RES directory bounds";
        return false;
    }

    size_t namesStart = size - namesLength;
    for (uint32_t i = 0; i < numFiles; ++i) {
        size_t pos = tableOffset + static_cast<size_t>(i) * 22;
        uint32_t dataLength, dataOffset, timestamp, nameOffset;
        uint16_t nameLen;
        std::memcpy(&dataLength, data + pos + 4, 4);
        std::memcpy(&dataOffset, data + pos + 8, 4);
        std::memcpy(&timestamp, data + pos + 12, 4);
        (void)timestamp;
        std::memcpy(&nameLen, data + pos + 16, 2);
        std::memcpy(&nameOffset, data + pos + 18, 4);
        if (nameLen == 0) continue;
        if (namesStart + nameOffset + nameLen > size) continue;
        if (dataOffset + dataLength > size) continue;

        std::string name(reinterpret_cast<const char*>(data + namesStart + nameOffset), nameLen);
        Archive::Entry entry;
        entry.originalName = name;
        entry.data.assign(data + dataOffset, data + dataOffset + dataLength);
        out.entries[Archive::ToLower(name)] = std::move(entry);
    }
    return true;
}

inline bool ParseArchive(const std::vector<uint8_t>& bytes, Archive& out, std::string& err) {
    return ParseArchive(bytes.data(), bytes.size(), out, err);
}

} // namespace res
