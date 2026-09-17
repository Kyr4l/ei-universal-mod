// Reads the "Units" gameplay database (RaceModels + Monsters records) from
// either a database RES archive (databaselmp.res's "units.udb" tagged-value
// blob) or a databaselmp.xlsx spreadsheet, giving the model viewer enough to
// list units, find their figure/texture names, and their complection.
//
// Binary format per docs/file-formats/database-format.md; verified against
// the real Universal-Mod/res/databaselmp.res during development (RaceModels
// records decoded byte-exact, e.g. "Human Male" -> MaskName "unhuma").
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "res_archive.hpp"
#include "xlsx_reader.hpp"
#include "cp1251.hpp"

namespace db {

// cp1251.hpp only provides UTF-8 -> CP1251 (the encoder direction um-xlsxdb needs);
// the viewer needs the reverse to display strings read out of a binary database.
inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline std::string Cp1251ToUtf8(const std::string& raw) {
    static const std::array<uint32_t, 256> reverseTable = [] {
        std::array<uint32_t, 256> table{};
        for (int i = 0; i < 128; ++i) table[i] = static_cast<uint32_t>(i);
        for (int i = 128; i < 256; ++i) table[i] = static_cast<uint32_t>(i); // fallback: Latin-1
        for (const auto& pair : cp1251::NonAsciiTable()) table[pair.byte] = pair.codepoint;
        return table;
    }();
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) AppendUtf8(out, reverseTable[c]);
    return out;
}

struct RaceModel {
    std::string name;             // e.g. "Human Male"
    std::string maskName;         // .mod/.bon/.fig basename, e.g. "unhuma"
    std::vector<std::string> primaryTextures;
    std::vector<std::string> secondaryTextures;
};

struct Monster {
    std::string name;
    std::string baseRace;         // references RaceModel::name
    float complectionX = 0.5f, complectionY = 0.5f, complectionZ = 0.5f;
};

struct UnitDatabase {
    std::vector<RaceModel> raceModels;
    std::vector<Monster> monsters;

    const RaceModel* FindRaceModel(const std::string& name) const {
        for (auto& r : raceModels) if (r.name == name) return &r;
        return nullptr;
    }
};

// ----------------------------------------------------------------------------
// Generic tagged-value reader (see database-format.md "Universal Tagged-Value
// Primitive"): a length byte with an even value is a 1-byte length (n = v/2);
// an odd value means the true length is a following 4-byte LE word.
// ----------------------------------------------------------------------------

struct TagSpan { uint8_t tag; size_t contentStart; size_t contentLen; };

inline bool ReadTag(const uint8_t* data, size_t size, size_t& off, TagSpan& out) {
    if (off + 2 > size) return false;
    uint8_t tag = data[off];
    uint8_t lenByte = data[off + 1];
    size_t contentLen;
    size_t contentStart;
    if ((lenByte % 2) == 0) {
        contentLen = lenByte / 2;
        contentStart = off + 2;
    } else {
        if (off + 5 > size) return false;
        uint32_t wide;
        std::memcpy(&wide, data + off + 1, 4);
        contentLen = (wide - 1) / 2;
        contentStart = off + 5;
    }
    if (contentStart + contentLen > size) return false;
    out.tag = tag;
    out.contentStart = contentStart;
    out.contentLen = contentLen;
    off = contentStart + contentLen;
    return true;
}

// Reads every sibling tagged value in [start, start+len), keyed by tag (last one wins
// for repeated tags, which doesn't happen for Field IDs within one Record).
inline std::map<uint8_t, TagSpan> ReadFields(const uint8_t* data, size_t start, size_t len) {
    std::map<uint8_t, TagSpan> fields;
    size_t off = start;
    size_t end = start + len;
    while (off < end) {
        TagSpan span;
        if (!ReadTag(data, end, off, span)) break;
        fields[span.tag] = span;
    }
    return fields;
}

inline std::string ReadCp1251String(const uint8_t* data, const TagSpan& span) {
    // Fields are CP1251 bytes + trailing NUL; strip the NUL before converting.
    size_t len = span.contentLen;
    if (len > 0 && data[span.contentStart + len - 1] == 0x00) --len;
    std::string raw(reinterpret_cast<const char*>(data + span.contentStart), len);
    return Cp1251ToUtf8(raw);
}

inline float ReadFloatField(const uint8_t* data, const TagSpan& span) {
    if (span.contentLen < 4) return 0.0f;
    float v;
    std::memcpy(&v, data + span.contentStart, 4);
    return v;
}

// StringList: a Field whose content is a concatenation of tag=1 Items, each a
// plain CP1251 string + NUL (see database-format.md "Tagged-Item List Types").
inline std::vector<std::string> ReadStringList(const uint8_t* data, const TagSpan& span) {
    std::vector<std::string> out;
    size_t off = span.contentStart;
    size_t end = span.contentStart + span.contentLen;
    while (off < end) {
        TagSpan item;
        if (!ReadTag(data, end, off, item)) break;
        out.push_back(ReadCp1251String(data, item));
    }
    return out;
}

// Parses one "units.udb" blob (already extracted from a database RES archive).
inline bool ParseUnitsResBlob(const std::vector<uint8_t>& udb, UnitDatabase& out) {
    const uint8_t* data = udb.data();
    size_t size = udb.size();
    size_t off = 0;
    TagSpan file;
    if (!ReadTag(data, size, off, file)) return false;

    size_t blockOff = file.contentStart;
    size_t blockEnd = file.contentStart + file.contentLen;
    while (blockOff < blockEnd) {
        TagSpan block;
        if (!ReadTag(data, blockEnd, blockOff, block)) break;

        // block.tag: 1=HitLocations, 2=RaceModels, 3=Monsters, 4=NPC
        if (block.tag != 2 && block.tag != 3) continue;

        size_t recOff = block.contentStart;
        size_t recEnd = block.contentStart + block.contentLen;
        while (recOff < recEnd) {
            TagSpan record;
            if (!ReadTag(data, recEnd, recOff, record)) break;
            auto fields = ReadFields(data, record.contentStart, record.contentLen);

            if (block.tag == 2) { // RaceModels
                RaceModel rm;
                if (auto it = fields.find(0); it != fields.end()) rm.name = ReadCp1251String(data, it->second);
                if (auto it = fields.find(30); it != fields.end()) rm.maskName = ReadCp1251String(data, it->second);
                if (auto it = fields.find(31); it != fields.end()) rm.primaryTextures = ReadStringList(data, it->second);
                if (auto it = fields.find(32); it != fields.end()) rm.secondaryTextures = ReadStringList(data, it->second);
                if (!rm.name.empty()) out.raceModels.push_back(std::move(rm));
            } else { // Monsters
                Monster m;
                if (auto it = fields.find(0); it != fields.end()) m.name = ReadCp1251String(data, it->second);
                if (auto it = fields.find(1); it != fields.end()) m.baseRace = ReadCp1251String(data, it->second);
                if (auto it = fields.find(5); it != fields.end()) m.complectionX = ReadFloatField(data, it->second);
                if (auto it = fields.find(6); it != fields.end()) m.complectionY = ReadFloatField(data, it->second);
                if (auto it = fields.find(7); it != fields.end()) m.complectionZ = ReadFloatField(data, it->second);
                if (!m.name.empty()) out.monsters.push_back(std::move(m));
            }
        }
    }
    return true;
}

// Loads from a databaselmp.res file's bytes (looks up the "units.udb" entry inside).
inline bool LoadUnitsFromRes(const std::vector<uint8_t>& resBytes, UnitDatabase& out, std::string& err) {
    res::Archive archive;
    if (!res::ParseArchive(resBytes, archive, err)) return false;
    const std::vector<uint8_t>* udb = archive.Find("units.udb");
    if (!udb) { err = "units.udb not found in archive"; return false; }
    if (!ParseUnitsResBlob(*udb, out)) { err = "failed to parse units.udb"; return false; }
    return true;
}

// ----------------------------------------------------------------------------
// XLSX loading (databaselmp.xlsx: sheets "RaceModels" / "Monsters"), using the
// same row-3 "FLDx-y" column markers xlsxdb.cpp's encoder reads.
// ----------------------------------------------------------------------------

using ColMap = std::map<int, std::map<int, int>>;

inline ColMap BuildColumnMap(const xlsxlib::Sheet& ws) {
    ColMap cols;
    static const std::regex fldRe(R"(FLD(\d+)-(\d+)$)");
    for (int c = 1; c <= ws.MaxColumn(); ++c) {
        auto v = ws.Get(3, c);
        if (v.IsEmpty() || !v.isString) continue;
        std::smatch m;
        if (std::regex_search(v.strVal, m, fldRe)) {
            int fid = std::stoi(m[1].str());
            int fidx = std::stoi(m[2].str());
            cols[fid][fidx] = c;
        }
    }
    return cols;
}

inline std::optional<int> ColFor(const ColMap& cols, int fieldId, int idx = 0) {
    auto it = cols.find(fieldId);
    if (it == cols.end()) return std::nullopt;
    auto it2 = it->second.find(idx);
    if (it2 == it->second.end()) return std::nullopt;
    return it2->second;
}

inline std::vector<std::string> SplitCommaList(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) out.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

inline bool LoadUnitsFromXlsx(const std::string& xlsxPath, UnitDatabase& out, std::string& err) {
    try {
        xlsxlib::Workbook wb(xlsxPath);
        if (wb.HasSheet("RaceModels")) {
            const auto& ws = wb.GetSheet("RaceModels");
            ColMap cols = BuildColumnMap(ws);
            for (int row = 4; row <= ws.MaxRow(); ++row) {
                RaceModel rm;
                if (auto c = ColFor(cols, 0)) rm.name = ws.Get(row, *c).AsString();
                if (rm.name.empty()) continue;
                if (auto c = ColFor(cols, 30)) rm.maskName = ws.Get(row, *c).AsString();
                if (auto c = ColFor(cols, 31)) rm.primaryTextures = SplitCommaList(ws.Get(row, *c).AsString());
                if (auto c = ColFor(cols, 32)) rm.secondaryTextures = SplitCommaList(ws.Get(row, *c).AsString());
                out.raceModels.push_back(std::move(rm));
            }
        }
        if (wb.HasSheet("Monsters")) {
            const auto& ws = wb.GetSheet("Monsters");
            ColMap cols = BuildColumnMap(ws);
            for (int row = 4; row <= ws.MaxRow(); ++row) {
                Monster m;
                if (auto c = ColFor(cols, 0)) m.name = ws.Get(row, *c).AsString();
                if (m.name.empty()) continue;
                if (auto c = ColFor(cols, 1)) m.baseRace = ws.Get(row, *c).AsString();
                if (auto c = ColFor(cols, 5)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) m.complectionX = std::stof(s); }
                if (auto c = ColFor(cols, 6)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) m.complectionY = std::stof(s); }
                if (auto c = ColFor(cols, 7)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) m.complectionZ = std::stof(s); }
                out.monsters.push_back(std::move(m));
            }
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

} // namespace db
