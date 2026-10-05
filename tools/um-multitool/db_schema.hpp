// The Evil Islands gameplay databases' schema, shared by the compiler (xlsxdb.cpp: spreadsheet -> .res)
// and the exporter (dbexport.cpp: .res -> spreadsheet). See docs/file-formats/database-format.md.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

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

#include "db_schema_generated.hpp"  // DBTYPES (fields of each block) and DBBLOCKS (blocks = sheets)
#include "db_headers_generated.hpp" // DBHEADERS (the columns of each sheet)

// Fixed 10-byte trailer that DBEditor appends after every generated database blob, independent of
// content (confirmed identical across both database.xlsx and databaselmp.xlsx outputs and across many
// edited variants during reverse-engineering; see docs/file-formats/database-format.md).
static const std::vector<uint8_t> TRAILER = {0, 0, 2, 12, 2, 8, 1, 0, 0, 0};

// DBEditor bug (undocumented): these (DbName, BlockID, FieldID) combinations are declared in
// dbtypes.txt but DBEditor never actually serializes their value, regardless of content (verified via
// wine oracle: forcing a non-default value still produces the same output). See
// docs/file-formats/database-format.md "Known Quirks".
// A number typed as text in a spreadsheet cell, as the compiler reads it: atof, except "nan", "inf"
// and "infinity" (any case, optional sign), which are parsed here so that every C runtime gives the
// same bits (glibc's and Windows' atof("nan") differ): nan is the positive quiet NaN (float bits
// 0x7FC00000, what the shipped databases hold), -nan the negative one.
inline double SpreadsheetNumber(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    std::string word;
    for (size_t k = i; k < text.size() && std::isalpha(static_cast<unsigned char>(text[k])); ++k)
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(text[k])));
    // "nan(0xBITS)": a NaN with exactly these float bits (dbexport writes it for the NaNs that are not the
    // plain one, e.g. 0xFFFFFFFF, so that the workbook compiles back to the same bytes).
    if (word == "nan" && text.compare(i + 3, 3, "(0x") == 0) {
        const uint32_t bits = static_cast<uint32_t>(std::strtoul(text.c_str() + i + 6, nullptr, 16));
        float f;
        std::memcpy(&f, &bits, 4);
        if (std::isnan(f)) return static_cast<double>(f);
    }
    if (word == "nan" || word.rfind("nan", 0) == 0)
        return negative ? -std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::quiet_NaN();
    if (word == "inf" || word == "infinity")
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    return std::atof(text.c_str());
}

inline bool IsNeverWritten(const std::string& dbName, int blockId, int fieldId) {
    return dbName == "Items" && blockId == 4 && fieldId == 26; // QuickItems ByteList
}
inline bool IsAlwaysEmpty(const std::string& dbName, int blockId, int fieldId) {
    return dbName == "Items" && blockId == 4 && fieldId == 27; // QuickItems Hex
}

static const std::map<std::string, std::string> DB_OUTPUT_NAME = {
    {"Items", "items.idb"}, {"Levers", "levers.ldb"}, {"Perks", "perks.pdb"},
    {"Prints", "prints.db"}, {"Spells", "spells.sdb"}, {"Units", "units.udb"},
    {"Acks", "acks.db"},
};

// Order matches dbfiles.txt (Items, Levers, Perks, Prints, Spells, Units, Acks).
static const std::vector<std::string> DB_ORDER = {
    "Items", "Levers", "Perks", "Prints", "Spells", "Units", "Acks",
};
