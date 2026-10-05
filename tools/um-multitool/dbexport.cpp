/**
 * ============================================================================
 * um-dbexport - Evil Islands gameplay database exporter (.res -> .xlsx / .ods)
 * ============================================================================
 *
 * The reverse of xlsxdb.cpp: reads the databases out of a .res archive (databaselmp.res: items.idb,
 * levers.ldb, perks.pdb, prints.db, spells.sdb, units.udb; database.res: acks.db) or a single database
 * file, and writes the spreadsheet xlsxdb compiles them from, as .xlsx or OpenDocument .ods:
 *   - one sheet per block (Materials, Weapons, ... NPC; Answers, Cryes, Others), in DBEditor's order;
 *   - from column B: row 2 the column titles, row 3 the "FLDx-y" markers xlsxdb reads, the records
 *     from row 4 (column A and row 1 empty, as DBEditor lays them out);
 *   - the columns of DBEditor's dbheaders.txt (db_headers_generated.hpp), plus one at the end for any
 *     value the database holds that dbheaders.txt has no column for ("Unk<field>-<index>").
 * Every value is written so that xlsxdb gives back the same bytes: floats as the shortest decimal that
 * reads back as the same float, lists over their columns, Acks lists as "{1=text##2=5}{...}".
 *
 * Usage (via um-multitool):
 *   um-multitool dbexport [-o out.xlsx|out.ods] <databaselmp.res | units.udb | ...>
 * ============================================================================
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "subtools.hpp"

#include "cp1251.hpp"
#include "db_model.hpp"
#include "db_schema.hpp"
#include "sheet_io.hpp"
#include "viewer/res_archive.hpp"

namespace fs = std::filesystem;

namespace {

// ---- reading the tagged values ----------------------------------------------------------------------

// One tagged value: [tag][length][raw] (see xlsxdb.cpp EncodeTagged).
struct Tagged {
    int tag = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// The tagged values one after another in [p, p + n).
std::vector<Tagged> ReadTaggedList(const uint8_t* p, size_t n) {
    std::vector<Tagged> out;
    size_t i = 0;
    while (i < n) {
        if (i + 2 > n) throw std::runtime_error("truncated tagged value");
        Tagged t;
        t.tag = p[i];
        size_t len;
        if ((p[i + 1] & 1) == 0) { // 1-byte length: twice the size
            len = p[i + 1] / 2;
            i += 2;
        } else {                   // 4-byte length: twice the size + 1
            if (i + 5 > n) throw std::runtime_error("truncated tagged length");
            uint32_t v;
            std::memcpy(&v, p + i + 1, 4);
            len = (v - 1) / 2;
            i += 5;
        }
        if (i + len > n) throw std::runtime_error("tagged value runs past its parent");
        t.data = p + i;
        t.size = len;
        out.push_back(t);
        i += len;
    }
    return out;
}

// ---- values as cell text ----------------------------------------------------------------------------

std::string CString(const uint8_t* p, size_t n) {
    size_t end = 0;
    while (end < n && p[end]) ++end;
    return cp1251::DecodeCp1251(p, end);
}

template <typename T>
T Read(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// The shortest decimal text that reads back as this exact float (finite floats only), without an
// exponent for ordinary magnitudes (140, not 1.4e+02).
std::string FloatText(float f) {
    char buf[64];
    const double a = std::fabs(static_cast<double>(f));
    const bool plain = a >= 1e-4 && a < 1e15;
    for (int precision = 1; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, static_cast<double>(f));
        if (plain && std::strchr(buf, 'e')) continue;
        if (static_cast<float>(std::strtod(buf, nullptr)) == f) return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return buf;
}

std::string HexText(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef"; // lower case, as the workbooks have it
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

std::string Join(const std::vector<std::string>& parts, const char* sep) {
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? sep : "") + parts[i];
    return s;
}

// A record as cells: (fieldId, fieldIndex) -> cell. fieldIndex: the element of a list, the subfield of a
// TypeList, else 0 (DBEditor's FLD<field>-<index> columns).
using RowCells = std::map<std::pair<int, int>, sheetio::Cell>;

sheetio::Cell Number(const std::string& text) {
    sheetio::Cell c;
    c.isNumber = true;
    c.text = text;
    return c;
}
sheetio::Cell Text(const std::string& text) {
    sheetio::Cell c;
    c.text = text;
    return c;
}

// A float's cell. A spreadsheet number cannot be infinite or NaN: those are the text "inf", "-inf" or
// "nan", as the databases' workbooks have them (LeverPrototypes), which xlsxdb reads back with
// SpreadsheetNumber - so only the exact bits it gives are written (another NaN would not come back the same).
sheetio::Cell FloatCell(float f) {
    if (std::isfinite(f)) return Number(FloatText(f));
    std::string text = std::isnan(f) ? "nan" : f > 0 ? "inf" : "-inf";
    float back = static_cast<float>(SpreadsheetNumber(text));
    if (std::memcmp(&back, &f, 4) != 0) {
        // Not the plain NaN (the Windows compiler gives 0xFFFFFFFF): the bits spelled out.
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "nan(0x%08X)", bits);
        text = buf;
        back = static_cast<float>(SpreadsheetNumber(text));
        if (!std::isnan(f) || std::memcmp(&back, &f, 4) != 0)
            throw std::runtime_error("a float (bits 0x" + std::string(buf + 6, 8) + ") that no spreadsheet text gives back");
    }
    return Text(text);
}

// The value of a plain-typed (sub)field.
sheetio::Cell ScalarCell(FieldType type, int typeArg, const uint8_t* p, size_t n, bool& empty) {
    empty = false;
    auto need = [&](size_t k) { if (n < k) throw std::runtime_error("a field is shorter than its type"); };
    switch (type) {
    case FieldType::String: {
        std::string s = CString(p, n);
        empty = s.empty();
        return Text(s);
    }
    case FieldType::SignedLong: need(4); return Number(std::to_string(Read<int32_t>(p)));
    case FieldType::UnsignedLong: need(4); return Number(std::to_string(Read<uint32_t>(p)));
    case FieldType::Float: need(4); return FloatCell(Read<float>(p));
    case FieldType::Byte: need(1); return Number(std::to_string(p[0]));
    case FieldType::Hex: empty = n == 0; return Text(HexText(p, n));
    case FieldType::FixedString: {
        (void)typeArg;
        std::string s = CString(p, n);
        empty = s.empty();
        return Text(s);
    }
    default: throw std::runtime_error("unexpected scalar type");
    }
}

// One field of a general (non-Acks) record into its cells.
void DecodeGeneralField(const std::string& dbName, const FieldDef& fd, const Tagged& t, RowCells& cells) {
    const uint8_t* p = t.data;
    const size_t n = t.size;
    bool empty;
    switch (fd.type) {
    case FieldType::String: case FieldType::SignedLong: case FieldType::UnsignedLong: case FieldType::Float:
    case FieldType::Byte: case FieldType::Hex: case FieldType::FixedString: {
        sheetio::Cell c = ScalarCell(fd.type, fd.typeArg, p, n, empty);
        if (!empty) cells[{fd.fieldId, 0}] = c;
        return;
    }
    case FieldType::BitList: {
        if (n < 4) throw std::runtime_error("a bit list is shorter than 4 bytes");
        const uint32_t mask = Read<uint32_t>(p);
        for (int i = 0; i < fd.typeArg; ++i) cells[{fd.fieldId, i}] = Number((mask >> i) & 1 ? "1" : "0");
        return;
    }
    case FieldType::FloatList: case FieldType::UnsignedLongList: case FieldType::ByteList: {
        const size_t step = fd.type == FieldType::ByteList ? 1 : 4;
        for (size_t i = 0; i * step + step <= n; ++i) {
            const uint8_t* e = p + i * step;
            cells[{fd.fieldId, static_cast<int>(i)}] = fd.type == FieldType::FloatList ? FloatCell(Read<float>(e))
                                                       : fd.type == FieldType::ByteList ? Number(std::to_string(e[0]))
                                                                                        : Number(std::to_string(Read<uint32_t>(e)));
        }
        return;
    }
    case FieldType::StringList: case FieldType::MinfixedStringList: {
        std::vector<std::string> parts;
        for (const Tagged& item : ReadTaggedList(p, n)) parts.push_back(CString(item.data, item.size));
        if (!parts.empty()) cells[{fd.fieldId, 0}] = Text(Join(parts, ","));
        return;
    }
    case FieldType::TypeList: {
        const auto& subfields = DBTYPES.at(dbName).at(fd.typeArg);
        for (const Tagged& sub : ReadTaggedList(p, n)) {
            for (const FieldDef& sf : subfields) {
                if (sf.fieldId != sub.tag) continue;
                sheetio::Cell c = ScalarCell(sf.type, sf.typeArg, sub.data, sub.size, empty);
                // A TypeList writes every subfield that has a column, even empty: keep its column.
                cells[{fd.fieldId, sf.fieldId}] = empty ? Text("") : c;
            }
        }
        return;
    }
    default: throw std::runtime_error("unexpected field type");
    }
}

// An Acks field: its items as "{1=text##2=5}{...}".
std::string AcksCellText(const Tagged& t, const std::vector<FieldDef>& subfields) {
    std::string out;
    for (const Tagged& item : ReadTaggedList(t.data, t.size)) {
        std::vector<std::string> parts;
        for (const Tagged& sub : ReadTaggedList(item.data, item.size)) {
            std::string value;
            for (const FieldDef& sf : subfields) {
                if (sf.fieldId != sub.tag) continue;
                bool empty;
                value = ScalarCell(sf.type, sf.typeArg, sub.data, sub.size, empty).text;
            }
            parts.push_back(std::to_string(sub.tag) + "=" + value);
        }
        out += "{" + Join(parts, "##") + "}";
    }
    return out;
}

// ---- a database as sheets ---------------------------------------------------------------------------

struct BlockRows {
    int blockId;
    std::string sheetName;
    std::vector<RowCells> rows;
};

std::vector<BlockRows> DecodeDatabase(const std::string& dbName, const std::vector<uint8_t>& file) {
    // [tag 1: the blocks] + DBEditor's fixed trailer.
    if (file.size() < TRAILER.size() + 2) throw std::runtime_error("too short");
    std::vector<Tagged> top = ReadTaggedList(file.data(), file.size() - TRAILER.size());
    if (top.size() != 1 || top[0].tag != 1) throw std::runtime_error("not a database (no root value)");
    if (!std::equal(TRAILER.begin(), TRAILER.end(), file.end() - static_cast<long>(TRAILER.size())))
        std::cerr << "  warning: " << dbName << ": unusual trailer (DBEditor writes a fixed one)\n";

    std::vector<BlockRows> out;
    for (const Tagged& block : ReadTaggedList(top[0].data, top[0].size)) {
        std::string sheetName;
        for (const auto& [id, name] : DBBLOCKS.at(dbName))
            if (id == block.tag) sheetName = name;
        if (sheetName.empty()) throw std::runtime_error("unknown block " + std::to_string(block.tag));
        const auto& fieldDefs = DBTYPES.at(dbName).at(block.tag);
        BlockRows br{block.tag, sheetName, {}};
        for (const Tagged& record : ReadTaggedList(block.data, block.size)) {
            RowCells cells;
            for (const Tagged& field : ReadTaggedList(record.data, record.size)) {
                const FieldDef* fd = nullptr;
                for (const FieldDef& f : fieldDefs)
                    if (f.fieldId == field.tag) fd = &f;
                if (!fd) throw std::runtime_error(sheetName + ": unknown field " + std::to_string(field.tag));
                if (dbName == "Acks") {
                    if (fd->type == FieldType::AcksUniqueType) {
                        const std::string text = AcksCellText(field, DBTYPES.at("Acks").at(fd->typeArg));
                        if (!text.empty()) cells[{fd->fieldId, 0}] = Text(text);
                    } else {
                        bool empty;
                        sheetio::Cell c = ScalarCell(fd->type, fd->typeArg, field.data, field.size, empty);
                        if (!empty) cells[{fd->fieldId, 0}] = c;
                    }
                } else {
                    try {
                        DecodeGeneralField(dbName, *fd, field, cells);
                    } catch (const std::exception& e) {
                        throw std::runtime_error(sheetName + " record " + std::to_string(br.rows.size() + 1) + " field " +
                                                 std::to_string(fd->fieldId) + ": " + e.what());
                    }
                }
            }
            // The fields DBEditor never writes (QuickItems' ByteList) or always writes empty (QuickItems'
            // Hex) have no value in the file: DBEditor's workbooks show 0 / 0000000000000000 there (xlsxdb
            // ignores them).
            for (const FieldDef& f : fieldDefs) {
                if (IsAlwaysEmpty(dbName, block.tag, f.fieldId) && f.type == FieldType::Hex) cells[{f.fieldId, 0}] = Text("0000000000000000");
                if (!IsNeverWritten(dbName, block.tag, f.fieldId)) continue;
                auto headers = DBHEADERS.find(dbName);
                if (headers == DBHEADERS.end()) continue;
                for (const ColumnHeader& h : headers->second)
                    if (h.blockId == block.tag && h.fieldId == f.fieldId) cells[{h.fieldId, h.fieldIndex}] = Number("0");
            }
            br.rows.push_back(std::move(cells));
        }
        out.push_back(std::move(br));
    }
    return out;
}

// The sheet: dbheaders.txt's columns (then any extra the data has), titles, markers, records.
sheetio::Sheet BuildSheet(const std::string& dbName, const BlockRows& block) {
    struct Column { int fieldId, fieldIndex, width; std::string title; };
    std::vector<Column> columns;
    std::set<std::pair<int, int>> known;
    auto headers = DBHEADERS.find(dbName);
    if (headers != DBHEADERS.end())
        for (const ColumnHeader& h : headers->second) {
            if (h.blockId != block.blockId) continue;
            columns.push_back({h.fieldId, h.fieldIndex, h.width, h.title});
            known.insert({h.fieldId, h.fieldIndex});
        }
    std::set<std::pair<int, int>> extra;
    for (const RowCells& row : block.rows)
        for (const auto& [key, cell] : row)
            if (!known.count(key)) extra.insert(key);
    for (const auto& [f, i] : extra)
        columns.push_back({f, i, 0, "Unk" + std::to_string(f) + "-" + std::to_string(i)});

    // DBEditor's layout starts at B2: column A and row 1 stay empty.
    const int firstColumn = 2;
    // A marker can be in two columns (dbheaders.txt: Prints FLD3-2, Monsters FLD38-0): xlsxdb, like
    // DBEditor, reads only the last of them. The value goes in each, so that both show what the game uses.
    std::map<std::pair<int, int>, std::vector<int>> columnsOf;
    for (size_t c = 0; c < columns.size(); ++c)
        columnsOf[{columns[c].fieldId, columns[c].fieldIndex}].push_back(static_cast<int>(c) + firstColumn);

    sheetio::Sheet sh;
    sh.name = block.sheetName;
    sh.frozenRows = 3;
    for (size_t c = 0; c < columns.size(); ++c) {
        const int col = static_cast<int>(c) + firstColumn;
        sh.SetText(2, col, columns[c].title, true);
        sh.SetText(3, col, "FLD" + std::to_string(columns[c].fieldId) + "-" + std::to_string(columns[c].fieldIndex));
        if (columns[c].width > 0) sh.widthsPx[col] = columns[c].width;
    }
    int row = 4;
    for (const RowCells& cells : block.rows) {
        bool any = false;
        for (const auto& [key, cell] : cells) {
            if (!cell.isNumber && cell.text.empty()) continue;
            for (int col : columnsOf.at(key)) sh.Set(row, col, cell);
            any = true;
        }
        // xlsxdb skips empty rows: a record with nothing in it must still show (DBEditor wrote it).
        if (!any) throw std::runtime_error(block.sheetName + ": a record with no value at all cannot be written as a row");
        ++row;
    }
    return sh;
}

// ---- CLI --------------------------------------------------------------------------------------------

void PrintHelp() {
    std::cout << "um-multitool dbexport - Evil Islands database exporter (.res -> .xlsx / .ods)\n\n"
              << "Usage:\n"
              << "  um-multitool dbexport [-o <out.xlsx|out.ods>] <database.res | units.udb | items.idb | ...>\n\n"
              << "Options:\n"
              << "  -o, --output <path>   Output spreadsheet; its extension picks the format (.xlsx or .ods).\n"
              << "                        Default: the input's name with .xlsx\n"
              << "  -h, --help            Print this help message\n\n"
              << "Description:\n"
              << "  The reverse of xlsxdb: writes the spreadsheet a database is compiled from, one sheet per\n"
              << "  block with DBEditor's columns (row 2 titles, row 3 FLDx-y markers, records from row 4).\n"
              << "  xlsxdb compiles it (.xlsx or .ods) back to the same bytes.\n\n"
              << "Examples:\n"
              << "  um-multitool dbexport databaselmp.res                 # databaselmp.xlsx\n"
              << "  um-multitool dbexport database.res -o database.ods\n";
}

} // namespace

bool dbmodel::DecodeToBook(const std::vector<uint8_t>& bytes, const std::string& fileName, Book& book,
                           std::vector<std::string>& summary, std::string& err) {
    // The databases: the ones a .res holds, or the file itself (by its name: units.udb...).
    std::vector<std::pair<std::string, std::vector<uint8_t>>> databases;
    res::Archive archive;
    std::string resErr;
    if (res::ParseArchive(bytes.data(), bytes.size(), archive, resErr)) {
        for (const std::string& db : DB_ORDER)
            if (const auto* data = archive.Find(DB_OUTPUT_NAME.at(db))) databases.push_back({db, *data});
    } else {
        std::string name = fs::path(fileName).filename().string();
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& [db, file] : DB_OUTPUT_NAME)
            if (file == name) databases.push_back({db, bytes});
    }
    if (databases.empty()) {
        err = fs::path(fileName).filename().string() + " holds none of the databases (items.idb, levers.ldb, perks.pdb, prints.db, "
              "spells.sdb, units.udb, acks.db)";
        return false;
    }
    try {
        book.clear();
        for (const auto& [db, data] : databases) {
            size_t records = 0;
            for (const BlockRows& block : DecodeDatabase(db, data)) {
                book.push_back(BuildSheet(db, block));
                records += block.rows.size();
            }
            summary.push_back(DB_OUTPUT_NAME.at(db) + ": " + std::to_string(records) + " record(s)");
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

int RunDbExport(int argc, char* argv[]) {
    fs::path input, output;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { PrintHelp(); return 0; }
        if ((a == "-o" || a == "--output") && i + 1 < argc) output = argv[++i];
        else if (a.rfind("--output=", 0) == 0) output = a.substr(9);
        else if (!a.empty() && a[0] == '-') { std::cerr << "Error: unknown option '" << a << "'. Use -h for usage.\n"; return 1; }
        else if (input.empty()) input = a;
        else { std::cerr << "Error: unexpected argument '" << a << "'.\n"; return 1; }
    }
    if (input.empty()) { PrintHelp(); return 1; }
    if (output.empty()) output = fs::path(input).replace_extension(".xlsx");
    std::string ext = output.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".xlsx" && ext != ".ods") { std::cerr << "Error: the output must be .xlsx or .ods\n"; return 1; }

    std::ifstream f(input, std::ios::binary);
    if (!f) { std::cerr << "Error: cannot read " << input.string() << "\n"; return 1; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    dbmodel::Book book;
    std::vector<std::string> summary;
    std::string err;
    if (!dbmodel::DecodeToBook(bytes, input.string(), book, summary, err)) {
        std::cerr << "[ERROR] " << input.string() << ": " << err << "\n";
        return 1;
    }
    for (const std::string& line : summary) std::cout << "  " << line << "\n";
    const int errors = PrintIssues(dbmodel::CheckBook(book));
    (void)errors; // the database is what it is: the spreadsheet shows it, problems included
    if (!dbmodel::SaveBook(output.string(), book, err)) { std::cerr << "Error: " << err << "\n"; return 1; }
    std::cout << "[SUCCESS] " << input.filename().string() << " -> " << output.filename().string() << " (" << book.size()
              << " sheet(s))\n";
    return 0;
}
