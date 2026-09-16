/**
 * ============================================================================
 * um-xlsxdb - Evil Islands XLSX Database Compiler
 * ============================================================================
 *
 * Description:
 *   Converts an Evil Islands gameplay database spreadsheet (.xlsx, in the
 *   layout produced/consumed by the legacy EIDBEditor tool) directly into a
 *   packed .res archive, replacing `wine DBEditor.exe file.xlsx`.
 *
 *   Handles both database families used by this project:
 *     - Acks dialogue databases (database.xlsx): Answers/Cryes/Others sheets
 *       -> acks.db
 *     - Gameplay stat databases (databaselmp.xlsx): Items/Levers/Perks/
 *       Prints/Spells/Units sheets -> items.idb, levers.ldb, perks.pdb,
 *       prints.db, spells.sdb, units.udb
 *   A database is only emitted if at least one of its sheets is present in
 *   the input workbook, so the same tool handles either input unmodified.
 *
 *   ADB files (per-unit override blobs) are intentionally out of scope; they
 *   are managed as static binary resources packed by um-restool instead.
 *
 * Binary format:
 *   See docs/file-formats/database-format.md for the full reverse-engineered
 *   specification (verified byte-for-byte against the shipped release .res
 *   files). Briefly: a single recursive "tagged value" primitive is used at
 *   every nesting level (file/block/record/field/item/leaf):
 *     encode_tagged(tag, raw) = [tag:u8][length][raw]
 *     length = 2*len(raw), stored as 1 byte if <=255, else as a 4-byte
 *              little-endian (2*len(raw)+1) if larger.
 *
 * Usage:
 *   um-xlsxdb [options] <path/to/database.xlsx>
 *
 * Version:
 *   1.0
 * ============================================================================
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "cp1251.hpp"
#include "xlsx_reader.hpp"

namespace fs = std::filesystem;

static constexpr const char* PROGRAM_VERSION = "1.0";
static constexpr const char* PROGRAM_NAME = "um-xlsxdb";

// ============================================================================
// Database Schema (field type table)
// ============================================================================

enum class FieldType {
    String, SignedLong, UnsignedLong, Float, Byte, Hex, FixedString,
    BitList, ByteList, FloatList, UnsignedLongList, StringList,
    MinfixedStringList, AcksUniqueType, TypeList,
};

struct FieldDef {
    int fieldId;
    FieldType type;
    int typeArg;
};

#include "db_schema_generated.hpp"

// ============================================================================
// Universal Tagged-Value Encoding Primitive
// ============================================================================

static void WriteU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// encode_tagged(tag, raw) = [tag][length: 1 or 4 bytes][raw]
// length = 2*len(raw); stored in 1 byte if that fits (<=255), otherwise as a
// 4-byte little-endian (2*len(raw)+1). The reader can tell the two apart
// unambiguously because a "doubled" length is always even, while the 4-byte
// form's low byte is always odd (the "+1"). Verified byte-exact against the
// shipped .res files; see docs/file-formats/database-format.md.
static std::vector<uint8_t> EncodeTagged(uint8_t tag, const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    size_t n = raw.size();
    size_t doubled = 2 * n;
    out.push_back(tag);
    if (doubled <= 255) {
        out.push_back(static_cast<uint8_t>(doubled));
    } else {
        WriteU32LE(out, static_cast<uint32_t>(doubled + 1));
    }
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

static void Append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

static std::vector<uint8_t> EncodeCp1251String(const std::string& utf8) {
    std::vector<uint8_t> raw = cp1251::EncodeCp1251(utf8);
    raw.push_back(0x00);
    return raw;
}

static std::vector<uint8_t> PackFloat(float v) {
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
}
static std::vector<uint8_t> PackI32(int32_t v) {
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
}
static std::vector<uint8_t> PackU32(uint32_t v) {
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
}

// Fixed 10-byte trailer that DBEditor appends after every generated database
// blob, independent of content (confirmed identical across both database.xlsx
// and databaselmp.xlsx outputs and across many edited variants during
// reverse-engineering; see docs/file-formats/database-format.md).
static const std::vector<uint8_t> TRAILER = {0, 0, 2, 12, 2, 8, 1, 0, 0, 0};

// ============================================================================
// XLSX Column Mapping ("FLDx-y" marker row helpers)
// ============================================================================

// Map fieldID -> {fieldIndex: column}, read from the sheet's row-3 "FLDx-y" markers.
using ColMap = std::map<int, std::map<int, int>>;

static ColMap BuildColumnMap(const xlsxlib::Sheet& ws) {
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

static std::optional<int> ColFor(const ColMap& cols, int fieldId, int idx = 0) {
    auto it = cols.find(fieldId);
    if (it == cols.end()) return std::nullopt;
    auto it2 = it->second.find(idx);
    if (it2 == it->second.end()) return std::nullopt;
    return it2->second;
}

// ============================================================================
// Acks Family Encoder (database.xlsx: Answers / Cryes / Others)
// ============================================================================
//
// Hierarchy: File(tag=1) > Block(tag=BlockID) > Record(tag=1) >
//            Field(tag=FieldID, a list of Items) > Item(tag=1) > Subfield(tag=SubfieldID)
// Every level uses EncodeTagged(); an empty Field is written as a bare
// [tag,0x00] (zero-length raw), never omitted.

struct AckItem {
    std::map<int, std::string> values; // subfieldID -> raw text value (still needs typing)
};

static std::vector<AckItem> ParseAckCell(const std::string& text) {
    std::vector<AckItem> items;
    size_t pos = 0;
    while (true) {
        size_t open = text.find('{', pos);
        if (open == std::string::npos) break;
        size_t close = text.find('}', open);
        if (close == std::string::npos) break;
        std::string body = text.substr(open + 1, close - open - 1);
        AckItem item;
        size_t p = 0;
        while (p < body.size()) {
            size_t sep = body.find("##", p);
            std::string part = (sep == std::string::npos) ? body.substr(p) : body.substr(p, sep - p);
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                int key = std::stoi(part.substr(0, eq));
                item.values[key] = part.substr(eq + 1);
            }
            if (sep == std::string::npos) break;
            p = sep + 2;
        }
        items.push_back(std::move(item));
        pos = close + 1;
    }
    return items;
}

static std::vector<uint8_t> EncodeAckSubstructItem(const AckItem& item, const std::vector<FieldDef>& subfields) {
    std::vector<uint8_t> raw;
    for (const auto& sf : subfields) {
        auto it = item.values.find(sf.fieldId);
        if (it == item.values.end()) continue;
        const std::string& v = it->second;
        switch (sf.type) {
            case FieldType::String:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), EncodeCp1251String(v)));
                break;
            case FieldType::SignedLong:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), PackI32(std::stoi(v))));
                break;
            case FieldType::UnsignedLong:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), PackU32(static_cast<uint32_t>(std::stoul(v)))));
                break;
            default:
                throw std::runtime_error("Acks substructure: unsupported subfield type");
        }
    }
    return EncodeTagged(1, raw);
}

static std::vector<uint8_t> EncodeAckField(const xlsxlib::Sheet& ws, int row, const ColMap& cols,
                                            int fieldId, const std::vector<FieldDef>& subfields) {
    auto col = ColFor(cols, fieldId, 0);
    std::vector<uint8_t> raw;
    if (col) {
        auto cv = ws.Get(row, *col);
        if (!cv.IsEmpty() && cv.isString && !cv.strVal.empty()) {
            for (const auto& item : ParseAckCell(cv.strVal)) {
                Append(raw, EncodeAckSubstructItem(item, subfields));
            }
        }
    }
    return EncodeTagged(static_cast<uint8_t>(fieldId), raw);
}

static std::vector<uint8_t> EncodeAcksBlock(xlsxlib::Workbook& wb, int blockId, const std::string& sheetName,
                                             const std::vector<FieldDef>& fieldDefs) {
    const auto& ws = wb.GetSheet(sheetName);
    ColMap cols = BuildColumnMap(ws);
    auto nameCol = ColFor(cols, 1, 0);
    if (!nameCol) throw std::runtime_error("Acks sheet missing Name column: " + sheetName);

    std::vector<uint8_t> records;
    for (int r = 4; r <= ws.MaxRow(); ++r) {
        auto nameVal = ws.Get(r, *nameCol);
        if (nameVal.IsEmpty()) continue;

        std::vector<uint8_t> content = EncodeTagged(1, EncodeCp1251String(nameVal.strVal));
        for (const auto& fd : fieldDefs) {
            if (fd.fieldId == 1) continue; // Name already written
            const auto& subfields = DBTYPES.at("Acks").at(fd.typeArg);
            Append(content, EncodeAckField(ws, r, cols, fd.fieldId, subfields));
        }
        Append(records, EncodeTagged(1, content));
    }
    return EncodeTagged(static_cast<uint8_t>(blockId), records);
}

// ============================================================================
// General Family Encoder (databaselmp.xlsx: Items/Levers/Perks/Prints/Spells/Units)
// ============================================================================

static std::vector<uint8_t> HexDecode(const std::string& s) {
    std::vector<uint8_t> out;
    std::string t = s;
    if (t.size() % 2) t = "0" + t;
    out.reserve(t.size() / 2);
    for (size_t i = 0; i + 1 < t.size() + 1 && i + 1 < t.size() + 1; i += 2) {
        if (i + 1 >= t.size()) break;
        out.push_back(static_cast<uint8_t>(std::stoul(t.substr(i, 2), nullptr, 16)));
    }
    return out;
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

struct FieldResult {
    std::vector<uint8_t> raw;
    bool isDefault = false;
};

static double CellNum(const xlsxlib::CellValue& v) {
    if (v.IsEmpty()) return 0.0;
    return v.isString ? std::atof(v.strVal.c_str()) : v.numVal;
}
static std::string CellStr(const xlsxlib::CellValue& v) {
    if (v.IsEmpty()) return std::string();
    return v.isString ? v.strVal : std::to_string(v.numVal);
}

// Builds ONE fixed struct instance directly from columns (subfieldID == FieldIndex),
// used by TypeList(N): a single instance, not a repeatable curly-brace list, and with
// no extra "item" tag=1 wrapper around it (unlike AcksUniqueType).
static std::vector<uint8_t> EncodeStructFromColumns(const std::string& dbName, int outerFieldId, int subBlockId,
                                                      const xlsxlib::Sheet& ws, int row, const ColMap& cols) {
    const auto& subfields = DBTYPES.at(dbName).at(subBlockId);
    std::vector<uint8_t> raw;
    for (const auto& sf : subfields) {
        auto col = ColFor(cols, outerFieldId, sf.fieldId);
        if (!col) continue;
        auto cv = ws.Get(row, *col);
        switch (sf.type) {
            case FieldType::String:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), EncodeCp1251String(CellStr(cv))));
                break;
            case FieldType::SignedLong:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), PackI32(static_cast<int32_t>(CellNum(cv)))));
                break;
            case FieldType::UnsignedLong:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), PackU32(static_cast<uint32_t>(CellNum(cv)))));
                break;
            case FieldType::Float:
                Append(raw, EncodeTagged(static_cast<uint8_t>(sf.fieldId), PackFloat(static_cast<float>(CellNum(cv)))));
                break;
            default:
                throw std::runtime_error("TypeList substructure: unsupported subfield type");
        }
    }
    return raw;
}

// Returns (rawContent, isStringType, isDefault) for one top-level field.
static FieldResult EncodeGeneralField(const std::string& dbName, const FieldDef& fd,
                                       const xlsxlib::Sheet& ws, int row, const ColMap& cols) {
    FieldResult res;
    auto colMapIt = cols.find(fd.fieldId);
    const std::map<int, int> empty;
    const std::map<int, int>& idxMap = (colMapIt != cols.end()) ? colMapIt->second : empty;

    switch (fd.type) {
        case FieldType::String: {
            auto col = ColFor(cols, fd.fieldId, 0);
            std::string v = col ? CellStr(ws.Get(row, *col)) : std::string();
            res.raw = EncodeCp1251String(v);
            res.isDefault = v.empty();
            return res;
        }
        case FieldType::SignedLong: {
            auto col = ColFor(cols, fd.fieldId, 0);
            double v = col ? CellNum(ws.Get(row, *col)) : 0.0;
            res.raw = PackI32(static_cast<int32_t>(v));
            res.isDefault = (v == 0.0);
            return res;
        }
        case FieldType::UnsignedLong: {
            auto col = ColFor(cols, fd.fieldId, 0);
            double v = col ? CellNum(ws.Get(row, *col)) : 0.0;
            res.raw = PackU32(static_cast<uint32_t>(v));
            res.isDefault = (v == 0.0);
            return res;
        }
        case FieldType::Float: {
            auto col = ColFor(cols, fd.fieldId, 0);
            double v = col ? CellNum(ws.Get(row, *col)) : 0.0;
            res.raw = PackFloat(static_cast<float>(v));
            res.isDefault = (v == 0.0);
            return res;
        }
        case FieldType::Byte: {
            auto col = ColFor(cols, fd.fieldId, 0);
            double v = col ? CellNum(ws.Get(row, *col)) : 0.0;
            res.raw = {static_cast<uint8_t>(static_cast<int>(v) & 0xFF)};
            res.isDefault = (v == 0.0);
            return res;
        }
        case FieldType::Hex: {
            auto col = ColFor(cols, fd.fieldId, 0);
            std::string v = col ? CellStr(ws.Get(row, *col)) : std::string();
            res.raw = HexDecode(v);
            res.isDefault = res.raw.empty() || std::all_of(res.raw.begin(), res.raw.end(), [](uint8_t b) { return b == 0; });
            return res;
        }
        case FieldType::FixedString: {
            auto col = ColFor(cols, fd.fieldId, 0);
            std::string v = col ? CellStr(ws.Get(row, *col)) : std::string();
            std::vector<uint8_t> b = cp1251::EncodeCp1251(v);
            int n = fd.typeArg > 0 ? fd.typeArg : static_cast<int>(b.size());
            b.resize(std::max<size_t>(b.size(), static_cast<size_t>(n)), 0);
            b.resize(n);
            res.raw = b;
            res.isDefault = v.empty();
            return res;
        }
        default: break;
    }

    if (fd.type == FieldType::BitList) {
        int n = fd.typeArg;
        uint32_t mask = 0;
        for (int i = 0; i < n; ++i) {
            auto col = ColFor(cols, fd.fieldId, i);
            double v = col ? CellNum(ws.Get(row, *col)) : 0.0;
            if (v != 0.0) mask |= (1u << i);
        }
        res.raw = PackU32(mask);
        res.isDefault = (mask == 0);
        return res;
    }

    if (fd.type == FieldType::FloatList || fd.type == FieldType::ByteList || fd.type == FieldType::UnsignedLongList) {
        std::vector<double> vals;
        for (const auto& [idx, col] : idxMap) {
            (void)idx;
            vals.push_back(CellNum(ws.Get(row, col)));
        }
        bool allZero = true;
        for (double v : vals) {
            if (v != 0.0) allZero = false;
            if (fd.type == FieldType::FloatList) Append(res.raw, PackFloat(static_cast<float>(v)));
            else if (fd.type == FieldType::ByteList) res.raw.push_back(static_cast<uint8_t>(static_cast<int>(v) & 0xFF));
            else Append(res.raw, PackU32(static_cast<uint32_t>(v)));
        }
        res.isDefault = allZero;
        return res;
    }

    if (fd.type == FieldType::StringList) {
        auto col = ColFor(cols, fd.fieldId, 0);
        std::string v = col ? CellStr(ws.Get(row, *col)) : std::string();
        auto parts = SplitComma(v);
        for (const auto& p : parts) Append(res.raw, EncodeTagged(1, EncodeCp1251String(p)));
        res.isDefault = parts.empty();
        return res;
    }

    if (fd.type == FieldType::MinfixedStringList) {
        auto col = ColFor(cols, fd.fieldId, 0);
        std::string v = col ? CellStr(ws.Get(row, *col)) : std::string();
        auto parts = SplitComma(v);
        int minlen = fd.typeArg;
        for (const auto& p : parts) {
            std::vector<uint8_t> b = cp1251::EncodeCp1251(p);
            if (static_cast<int>(b.size()) < minlen) b.resize(minlen, 0);
            Append(res.raw, EncodeTagged(1, b));
        }
        res.isDefault = parts.empty();
        return res;
    }

    if (fd.type == FieldType::TypeList) {
        auto col = ColFor(cols, fd.fieldId, 0);
        res.raw = EncodeStructFromColumns(dbName, fd.fieldId, fd.typeArg, ws, row, cols);
        res.isDefault = res.raw.empty();
        (void)col;
        return res;
    }

    throw std::runtime_error("EncodeGeneralField: unhandled field type");
}

// DBEditor bug (undocumented): these (DbName, BlockID, FieldID) combinations are declared
// in dbtypes.txt but DBEditor never actually serializes their value, regardless of content
// (verified via wine oracle: forcing a non-default value still produces the same output).
// See docs/file-formats/database-format.md "Known Quirks".
static bool IsNeverWritten(const std::string& dbName, int blockId, int fieldId) {
    return dbName == "Items" && blockId == 4 && fieldId == 26; // QuickItems ByteList
}
static bool IsAlwaysEmpty(const std::string& dbName, int blockId, int fieldId) {
    return dbName == "Items" && blockId == 4 && fieldId == 27; // QuickItems Hex
}

static std::vector<uint8_t> EncodeGeneralBlock(const std::string& dbName, int blockId, const std::string& sheetName,
                                                xlsxlib::Workbook& wb) {
    const auto& ws = wb.GetSheet(sheetName);
    ColMap cols = BuildColumnMap(ws);
    const auto& fieldDefs = DBTYPES.at(dbName).at(blockId);

    std::vector<uint8_t> records;
    for (int r = 4; r <= ws.MaxRow(); ++r) {
        bool rowEmpty = true;
        for (int c = 1; c <= ws.MaxColumn(); ++c) {
            if (!ws.Get(r, c).IsEmpty()) { rowEmpty = false; break; }
        }
        if (rowEmpty) continue;

        std::vector<uint8_t> content;
        for (const auto& fd : fieldDefs) {
            if (IsNeverWritten(dbName, blockId, fd.fieldId)) continue;
            FieldResult fr = EncodeGeneralField(dbName, fd, ws, r, cols);
            if (IsAlwaysEmpty(dbName, blockId, fd.fieldId)) fr.raw.clear();
            Append(content, EncodeTagged(static_cast<uint8_t>(fd.fieldId), fr.raw));
        }
        Append(records, EncodeTagged(1, content));
    }
    return EncodeTagged(static_cast<uint8_t>(blockId), records);
}

// ============================================================================
// Top-Level Database Dispatch
// ============================================================================

struct GeneratedFile {
    std::string name;
    std::vector<uint8_t> data;
};

static const std::map<std::string, std::string> DB_OUTPUT_NAME = {
    {"Items", "items.idb"}, {"Levers", "levers.ldb"}, {"Perks", "perks.pdb"},
    {"Prints", "prints.db"}, {"Spells", "spells.sdb"}, {"Units", "units.udb"},
    {"Acks", "acks.db"},
};

// Order matches dbfiles.txt (Items, Levers, Perks, Prints, Spells, Units, Acks).
static const std::vector<std::string> DB_ORDER = {
    "Items", "Levers", "Perks", "Prints", "Spells", "Units", "Acks",
};

static bool AnyBlockSheetPresent(xlsxlib::Workbook& wb, const std::string& dbName) {
    for (const auto& [blockId, sheetName] : DBBLOCKS.at(dbName)) {
        (void)blockId;
        if (wb.HasSheet(sheetName)) return true;
    }
    return false;
}

static std::vector<GeneratedFile> EncodeAllDatabases(xlsxlib::Workbook& wb) {
    std::vector<GeneratedFile> files;
    for (const auto& dbName : DB_ORDER) {
        if (!AnyBlockSheetPresent(wb, dbName)) continue;

        std::vector<uint8_t> blocks;
        for (const auto& [blockId, sheetName] : DBBLOCKS.at(dbName)) {
            if (!wb.HasSheet(sheetName)) continue;
            if (dbName == "Acks") {
                Append(blocks, EncodeAcksBlock(wb, blockId, sheetName, DBTYPES.at(dbName).at(blockId)));
            } else {
                Append(blocks, EncodeGeneralBlock(dbName, blockId, sheetName, wb));
            }
        }

        std::vector<uint8_t> fileContent = EncodeTagged(1, blocks);
        Append(fileContent, TRAILER);

        files.push_back({DB_OUTPUT_NAME.at(dbName), std::move(fileContent)});
    }
    return files;
}

// ============================================================================
// RES Archive Packing
// ============================================================================
//
// Same container format as um-restool (see docs/file-formats/res-format.md),
// but WITHOUT 16-byte inter-file padding: DBEditor places payloads back to
// back with zero padding, and files are inserted in ALPHABETICAL filename
// order. Both details were confirmed by diffing against the shipped release
// .res files (see docs/file-formats/database-format.md "Container Packing").

static constexpr uint32_t RES_MAGIC = 0x019CE23Cu;

static uint32_t CalculateResHash(const std::string& name, uint32_t bucketCount) {
    if (bucketCount == 0) return 0;
    uint32_t sum = 0;
    for (unsigned char c : name) sum += static_cast<unsigned char>(std::tolower(c));
    return sum % bucketCount;
}

static std::vector<uint8_t> PackResArchive(std::vector<GeneratedFile> files) {
    std::sort(files.begin(), files.end(), [](const GeneratedFile& a, const GeneratedFile& b) {
        return a.name < b.name;
    });

    struct Rec { std::string name; uint32_t offset; uint32_t length; };
    std::vector<Rec> records;
    std::vector<uint8_t> dataBlock;
    for (const auto& f : files) {
        uint32_t off = static_cast<uint32_t>(16 + dataBlock.size());
        Append(dataBlock, f.data);
        records.push_back({f.name, off, static_cast<uint32_t>(f.data.size())});
    }

    std::vector<uint8_t> namesBlock;
    std::vector<uint32_t> nameOffsets(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        nameOffsets[i] = static_cast<uint32_t>(namesBlock.size());
        namesBlock.insert(namesBlock.end(), records[i].name.begin(), records[i].name.end());
    }

    uint32_t n = static_cast<uint32_t>(records.size());
    std::vector<int> table(n, -1);
    std::vector<int32_t> nextIndex(n, -1);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bucket = CalculateResHash(records[i].name, n);
        if (table[bucket] == -1) {
            table[bucket] = static_cast<int>(i);
        } else {
            int cur = static_cast<int>(bucket);
            while (nextIndex[cur] != -1) cur = nextIndex[cur];
            int freeIdx = static_cast<int>(n) - 1;
            while (freeIdx >= 0 && table[freeIdx] != -1) --freeIdx;
            if (freeIdx >= 0) {
                nextIndex[cur] = freeIdx;
                table[freeIdx] = static_cast<int>(i);
            }
        }
    }

    std::vector<uint8_t> descBlock;
    descBlock.reserve(static_cast<size_t>(n) * 22);
    for (uint32_t slot = 0; slot < n; ++slot) {
        int i = table[slot];
        std::vector<uint8_t> d;
        if (i >= 0) {
            WriteU32LE(d, static_cast<uint32_t>(nextIndex[slot]));
            WriteU32LE(d, records[i].length);
            WriteU32LE(d, records[i].offset);
            WriteU32LE(d, 0); // timestamp: always 0 for DBEditor-generated archives
            d.push_back(static_cast<uint8_t>(records[i].name.size() & 0xFF));
            d.push_back(static_cast<uint8_t>((records[i].name.size() >> 8) & 0xFF));
            WriteU32LE(d, nameOffsets[i]);
        } else {
            d.assign(22, 0);
            d[0] = d[1] = d[2] = d[3] = 0xFF; // nextIndex = -1
        }
        descBlock.insert(descBlock.end(), d.begin(), d.end());
    }

    std::vector<uint8_t> out;
    WriteU32LE(out, RES_MAGIC);
    WriteU32LE(out, n);
    WriteU32LE(out, static_cast<uint32_t>(16 + dataBlock.size()));
    WriteU32LE(out, static_cast<uint32_t>(namesBlock.size()));
    out.insert(out.end(), dataBlock.begin(), dataBlock.end());
    out.insert(out.end(), descBlock.begin(), descBlock.end());
    out.insert(out.end(), namesBlock.begin(), namesBlock.end());
    return out;
}

// ============================================================================
// CLI
// ============================================================================

static void PrintVersion() {
    std::cout << PROGRAM_NAME << " version " << PROGRAM_VERSION << "\n";
}

static void PrintHelp() {
    std::cout << "um-xlsxdb - Evil Islands XLSX Database Compiler\n\n"
              << "Usage:\n"
              << "  um-xlsxdb [options] <path/to/database.xlsx>\n\n"
              << "Options:\n"
              << "  -o, --output <path>   Set output .res archive path (default: same directory,\n"
              << "                        same base name, .res extension)\n"
              << "  --version             Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help            Print this help message\n\n"
              << "Description:\n"
              << "  Converts an Evil Islands gameplay database spreadsheet directly into a\n"
              << "  packed .res archive. Detects which database family(ies) the workbook\n"
              << "  contains sheets for (Acks dialogue, or Items/Levers/Perks/Prints/Spells/\n"
              << "  Units gameplay stats) and only emits the corresponding files. See\n"
              << "  docs/file-formats/database-format.md for the on-disk format.\n\n"
              << "Examples:\n"
              << "  um-xlsxdb database.xlsx        # Writes database.res (acks.db)\n"
              << "  um-xlsxdb databaselmp.xlsx     # Writes databaselmp.res (6 db files)\n";
}

struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    fs::path inputPath;
    fs::path outputPath;
};

static bool ParseCommandLine(int argc, char* argv[], CliOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { opt.showHelp = true; return true; }
        if (arg == "--version") { opt.showVersion = true; return true; }
        if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) { opt.outputPath = argv[++i]; }
            else { std::cerr << "Error: " << arg << " requires a path argument.\n"; return false; }
        } else if (arg.rfind("--output=", 0) == 0) {
            opt.outputPath = arg.substr(9);
        } else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option '" << arg << "'. Use -h / --help for usage.\n";
            return false;
        } else {
            if (opt.inputPath.empty()) opt.inputPath = arg;
            else { std::cerr << "Error: Unexpected additional positional argument '" << arg << "'.\n"; return false; }
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);

    CliOptions opt;
    if (!ParseCommandLine(argc, argv, opt)) return 1;
    if (opt.showHelp) { PrintHelp(); return 0; }
    if (opt.showVersion) { PrintVersion(); return 0; }

    if (opt.inputPath.empty()) {
        std::cerr << "Error: No input .xlsx file specified.\n"
                  << "Try '" << PROGRAM_NAME << " --help' for more information.\n";
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(opt.inputPath, ec)) {
        std::cerr << "Error: Input file does not exist: " << opt.inputPath.string() << "\n";
        return 1;
    }

    fs::path outputPath = opt.outputPath.empty()
        ? fs::path(opt.inputPath).replace_extension(".res")
        : opt.outputPath;

    try {
        xlsxlib::Workbook wb(opt.inputPath.string());
        std::vector<GeneratedFile> files = EncodeAllDatabases(wb);

        if (files.empty()) {
            std::cerr << "Error: No recognized database sheets found in " << opt.inputPath.string() << "\n";
            return 1;
        }

        std::vector<uint8_t> res = PackResArchive(files);

        if (outputPath.has_parent_path()) fs::create_directories(outputPath.parent_path(), ec);
        std::ofstream out(outputPath, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "Error: Failed to open output archive for writing: " << outputPath.string() << "\n";
            return 1;
        }
        out.write(reinterpret_cast<const char*>(res.data()), static_cast<std::streamsize>(res.size()));
        out.close();

        std::cout << "[SUCCESS] " << opt.inputPath.filename().string() << " -> " << outputPath.filename().string()
                  << " (" << files.size() << " file(s), " << res.size() << " bytes)\n";
        for (const auto& f : files) {
            std::cout << "  " << f.name << " (" << f.data.size() << " bytes)\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << opt.inputPath.string() << ": " << e.what() << "\n";
        return 1;
    }

    return 0;
}
