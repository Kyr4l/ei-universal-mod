// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// In-memory Evil Islands .res archive reader.
// See docs/file-formats/res-format.md for the on-disk layout this decodes.
#pragma once

#include <cstdint>
#include <cctype>
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
        if (static_cast<size_t>(dataOffset) + dataLength > size) continue; // summed as size_t: two u32 could wrap past the check

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

// Rewrites an archive with some entries' contents replaced (keys: lower-case names), keeping everything
// else as it was: the entry order, the hash chains, the names, the other payloads (in their original
// order, 16-byte aligned) and their timestamps. Replaced payloads go after the kept ones and get
// `timestamp`. Used to save an edited file inside a packed quest (.mq).
// A new RES archive holding `files` (name -> payload), laid out as um-restool packs one: payloads 16-byte
// aligned after the header, then the hash table (bucket = sum of the lower-case name's bytes % count, collisions
// chained into the free slots from the end), then the names.
inline std::vector<uint8_t> WriteArchive(const std::map<std::string, std::vector<uint8_t>>& files, uint32_t timestamp) {
    struct Rec { std::string name; uint32_t length = 0, offset = 0, nameOffset = 0; };
    std::vector<Rec> recs;
    std::vector<uint8_t> data, names;
    for (const auto& f : files) {
        while (data.size() % 16) data.push_back(0);
        Rec r;
        r.name = f.first;
        r.length = static_cast<uint32_t>(f.second.size());
        r.offset = static_cast<uint32_t>(16 + data.size());
        r.nameOffset = static_cast<uint32_t>(names.size());
        data.insert(data.end(), f.second.begin(), f.second.end());
        names.insert(names.end(), f.first.begin(), f.first.end());
        recs.push_back(r);
    }
    while (data.size() % 16) data.push_back(0);
    const uint32_t n = static_cast<uint32_t>(recs.size());
    std::vector<int> slot(n, -1), next(n, -1);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t sum = 0;
        for (unsigned char c : recs[i].name) sum += static_cast<unsigned char>(std::tolower(c));
        const uint32_t bucket = sum % n;
        if (slot[bucket] < 0) { slot[bucket] = static_cast<int>(i); continue; }
        uint32_t cur = bucket;
        while (next[cur] >= 0) cur = static_cast<uint32_t>(next[cur]);
        int free = static_cast<int>(n) - 1;
        while (free >= 0 && slot[static_cast<size_t>(free)] >= 0) --free;
        if (free >= 0) { next[cur] = free; slot[static_cast<size_t>(free)] = static_cast<int>(i); }
    }
    std::vector<uint8_t> out(16);
    auto put = [&](const void* p, size_t k) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + k); };
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t header[4] = {kResMagic, n, static_cast<uint32_t>(16 + data.size()), static_cast<uint32_t>(names.size())};
    std::memcpy(out.data(), header, 16);
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t nx = next[i];
        const Rec empty;
        const Rec& r = slot[i] >= 0 ? recs[static_cast<size_t>(slot[i])] : empty;
        const uint32_t ts = slot[i] >= 0 ? timestamp : 0;
        const uint16_t nl = static_cast<uint16_t>(r.name.size());
        put(&nx, 4); put(&r.length, 4); put(&r.offset, 4); put(&ts, 4); put(&nl, 2); put(&r.nameOffset, 4);
    }
    out.insert(out.end(), names.begin(), names.end());
    return out;
}

inline bool RewriteArchive(const std::vector<uint8_t>& in, const std::map<std::string, std::vector<uint8_t>>& replace,
                           uint32_t timestamp, std::vector<uint8_t>& out, std::string& err) {
    if (in.size() < 16) { err = "too small"; return false; }
    uint32_t magic, count, tableOffset, namesLength;
    std::memcpy(&magic, in.data(), 4);
    std::memcpy(&count, in.data() + 4, 4);
    std::memcpy(&tableOffset, in.data() + 8, 4);
    std::memcpy(&namesLength, in.data() + 12, 4);
    const size_t namesStart = static_cast<size_t>(tableOffset) + static_cast<size_t>(count) * 22;
    if (magic != kResMagic || namesStart + namesLength > in.size()) { err = "not a RES archive"; return false; }
    struct Desc { uint32_t length, offset, time; uint16_t nameLen; uint32_t nameOffset; std::string name; const std::vector<uint8_t>* replacement; };
    std::vector<Desc> descs(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* d = in.data() + tableOffset + static_cast<size_t>(i) * 22;
        Desc& e = descs[i];
        std::memcpy(&e.length, d + 4, 4);
        std::memcpy(&e.offset, d + 8, 4);
        std::memcpy(&e.time, d + 12, 4);
        std::memcpy(&e.nameLen, d + 16, 2);
        std::memcpy(&e.nameOffset, d + 18, 4);
        if (e.nameLen && static_cast<size_t>(e.nameOffset) + e.nameLen <= namesLength)
            e.name = Archive::ToLower(std::string(reinterpret_cast<const char*>(in.data() + namesStart + e.nameOffset), e.nameLen));
        auto it = replace.find(e.name);
        e.replacement = e.nameLen && it != replace.end() ? &it->second : nullptr;
        if (!e.replacement && e.nameLen && static_cast<size_t>(e.offset) + e.length > in.size()) { err = "damaged entry " + e.name; return false; }
    }
    // Kept payloads, once each (identical files may share one), in their original order.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> kept; // (old offset, length) -> new offset
    for (const Desc& e : descs) if (e.nameLen && !e.replacement) kept[{e.offset, e.length}] = 0;
    std::vector<uint8_t> data;
    auto align = [&] { while (data.size() % 16) data.push_back(0); };
    for (auto& kv : kept) {
        align();
        kv.second = static_cast<uint32_t>(16 + data.size());
        data.insert(data.end(), in.begin() + kv.first.first, in.begin() + kv.first.first + kv.first.second);
    }
    std::vector<uint32_t> newOffset(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const Desc& e = descs[i];
        if (!e.nameLen) continue;
        if (e.replacement) {
            align();
            newOffset[i] = static_cast<uint32_t>(16 + data.size());
            data.insert(data.end(), e.replacement->begin(), e.replacement->end());
        } else {
            newOffset[i] = kept[{e.offset, e.length}];
        }
    }
    align();
    out.clear();
    auto put32 = [&](uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(), b, b + 4); };
    put32(kResMagic);
    put32(count);
    put32(static_cast<uint32_t>(16 + data.size()));
    put32(namesLength);
    out.insert(out.end(), data.begin(), data.end());
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t d[22];
        std::memcpy(d, in.data() + tableOffset + static_cast<size_t>(i) * 22, 22); // next index, name: unchanged
        const Desc& e = descs[i];
        if (e.nameLen) {
            uint32_t length = e.replacement ? static_cast<uint32_t>(e.replacement->size()) : e.length;
            uint32_t time = e.replacement ? timestamp : e.time;
            std::memcpy(d + 4, &length, 4);
            std::memcpy(d + 8, &newOffset[i], 4);
            std::memcpy(d + 12, &time, 4);
        }
        out.insert(out.end(), d, d + 22);
    }
    out.insert(out.end(), in.begin() + namesStart, in.begin() + namesStart + namesLength);
    return true;
}

} // namespace res
