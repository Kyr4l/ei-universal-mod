// The gameplay databases as spreadsheets in memory: loading, saving, compiling and checking them. See
// db_model.hpp.

#include "db_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include "cp1251.hpp"
#include "db_schema.hpp"

namespace fs = std::filesystem;

namespace dbmodel {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// The shortest text that reads back as this exact double, without an exponent for ordinary magnitudes.
std::string DoubleText(double v) {
    char buf[64];
    const bool plain = std::fabs(v) >= 1e-4 && std::fabs(v) < 1e15; // 140, not 1.4e+02
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (plain && std::strchr(buf, 'e')) continue;
        if (std::strtod(buf, nullptr) == v) return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace

// ---- load, save, compile -----------------------------------------------------------------------------

bool LoadBook(const std::string& path, Book& book, std::string& err) {
    std::string ext = Lower(fs::path(path).extension().string());
    book.clear();
    if (ext == ".xlsx" || ext == ".ods") {
        try {
            xlsxlib::Workbook wb(path);
            for (const std::string& name : wb.SheetNames()) {
                const xlsxlib::Sheet& x = wb.GetSheet(name);
                sheetio::Sheet sh;
                sh.name = name;
                sh.frozenRows = 3;
                for (const auto& [rc, v] : x.Cells()) {
                    if (v.isString) sh.SetText(rc.first, rc.second, v.strVal, rc.first == 2);
                    else sh.SetNumber(rc.first, rc.second, DoubleText(v.numVal));
                }
                // Column widths from DBEditor's headers, by each column's marker.
                std::string db;
                int blockId = 0;
                for (const auto& [dbName, blocks] : DBBLOCKS)
                    for (const auto& [id, sheetName] : blocks)
                        if (sheetName == name) { db = dbName; blockId = id; }
                auto headers = DBHEADERS.find(db);
                if (headers != DBHEADERS.end())
                    for (const auto& [rc, cell] : sh.cells) {
                        if (rc.first != 3) continue;
                        for (const ColumnHeader& h : headers->second)
                            if (h.blockId == blockId && h.width > 0 &&
                                cell.text == "FLD" + std::to_string(h.fieldId) + "-" + std::to_string(h.fieldIndex))
                                sh.widthsPx[rc.second] = h.width;
                    }
                book.push_back(std::move(sh));
            }
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> summary;
    return DecodeToBook(bytes, path, book, summary, err);
}

bool SaveBook(const std::string& path, const Book& book, std::string& err) {
    const std::string ext = Lower(fs::path(path).extension().string());
    if (ext != ".xlsx" && ext != ".ods") { err = "a spreadsheet is saved as .xlsx or .ods"; return false; }
    const std::string data = ext == ".ods" ? sheetio::BuildOds(book) : sheetio::BuildXlsx(book);
    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
    return sheetio::WriteFile(path, data, err);
}

xlsxlib::Workbook ToWorkbook(const Book& book) {
    xlsxlib::Workbook wb;
    for (const sheetio::Sheet& sh : book) {
        xlsxlib::Sheet x;
        for (const auto& [rc, cell] : sh.cells) {
            xlsxlib::CellValue v;
            v.present = true;
            if (cell.isNumber) v.numVal = std::strtod(cell.text.c_str(), nullptr);
            else { v.isString = true; v.strVal = cell.text; }
            x.Set(rc.first, rc.second, v);
        }
        wb.AddSheet(sh.name, std::move(x));
    }
    return wb;
}

bool CompileBook(const Book& book, std::vector<uint8_t>& res, std::vector<GeneratedFile>* filesOut, std::string& err) {
    try {
        xlsxlib::Workbook wb = ToWorkbook(book);
        std::vector<GeneratedFile> files = EncodeWorkbook(wb);
        if (files.empty()) { err = "no database sheet (Materials, Weapons... Answers, Cryes, Others)"; return false; }
        res = PackRes(files);
        if (filesOut) *filesOut = std::move(files);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool ReadAsRes(const std::string& path, std::vector<uint8_t>& res, std::string& err) {
    const std::string ext = Lower(fs::path(path).extension().string());
    if (ext == ".xlsx" || ext == ".ods") {
        Book book;
        return LoadBook(path, book, err) && CompileBook(book, res, nullptr, err);
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    res.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---- checks -------------------------------------------------------------------------------------------

std::string Issue::Where() const {
    if (row <= 0 || col <= 0) return sheet;
    return sheet + "!" + sheetio::ColumnName(col) + std::to_string(row);
}

int PrintIssues(const std::vector<Issue>& issues) {
    int errors = 0, warnings = 0;
    for (const Issue& i : issues) {
        const bool error = i.severity == Issue::Error;
        (error ? std::cerr : std::cout) << (error ? "[ERROR] " : "[WARN] ") << i.Where() << ": " << i.message << "\n";
        (error ? errors : warnings)++;
    }
    if (errors || warnings)
        std::cout << "Checks: " << errors << " error(s), " << warnings << " warning(s)\n";
    else
        std::cout << "Checks: no problem found\n";
    return errors;
}

namespace {

// A sheet as the compiler sees it: its database and block, and the column each FLDx-y marker leads to
// (the last one when a marker is in several columns, as xlsxdb reads it).
struct SheetInfo {
    std::string db;
    int blockId = 0;
    std::map<std::pair<int, int>, std::vector<int>> columns; // (field, index) -> columns, in order
    std::map<int, std::pair<int, int>> markerOf;            // column -> (field, index)
    std::set<int> unmarked;                                  // columns without a marker
    std::string Title(const sheetio::Sheet& sh, int col) const {
        auto it = sh.cells.find({2, col});
        return it != sh.cells.end() ? it->second.text : std::string();
    }
};

SheetInfo Describe(const sheetio::Sheet& sh) {
    SheetInfo info;
    for (const auto& [dbName, blocks] : DBBLOCKS)
        for (const auto& [id, name] : blocks)
            if (name == sh.name) { info.db = dbName; info.blockId = id; }
    for (const auto& [rc, cell] : sh.cells) {
        if (rc.first != 3) continue;
        int f = 0, i = 0;
        if (std::sscanf(cell.text.c_str(), "FLD%d-%d", &f, &i) == 2) {
            info.columns[{f, i}].push_back(rc.second);
            info.markerOf[rc.second] = {f, i};
        }
    }
    return info;
}

const sheetio::Cell* At(const sheetio::Sheet& sh, int row, int col) {
    auto it = sh.cells.find({row, col});
    return it != sh.cells.end() ? &it->second : nullptr;
}

bool Empty(const sheetio::Cell* c) { return !c || (!c->isNumber && c->text.empty()); }

// The number a cell gives the compiler (xlsxdb's CellNum: text goes through atof), and whether the
// text is entirely a number.
double CellNumber(const sheetio::Cell* c, bool& whollyNumber) {
    whollyNumber = true;
    if (Empty(c)) return 0.0;
    if (c->isNumber) return std::strtod(c->text.c_str(), nullptr);
    const std::string t = Trim(c->text);
    char* end = nullptr;
    std::strtod(t.c_str(), &end);
    whollyNumber = !t.empty() && end && *end == '\0';
    return SpreadsheetNumber(c->text); // what the compiler reads
}

// Characters a database string cannot hold (Windows-1251): they become '?'.
std::string Unstorable(const std::string& utf8) {
    std::string bad;
    size_t i = 0;
    while (i < utf8.size()) {
        const size_t start = i;
        const uint32_t cp = cp1251::DecodeUtf8(utf8, i);
        if (cp != '?' && cp1251::CodepointToByte(cp) == '?' && bad.find(utf8.substr(start, i - start)) == std::string::npos)
            bad += utf8.substr(start, i - start);
    }
    return bad;
}

// A comma-separated list's entries, with where each starts in the text (empty entries skipped).
struct Entry {
    size_t pos;
    std::string text;
};
std::vector<Entry> SplitEntries(const std::string& s) {
    std::vector<Entry> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back({a, s.substr(a, b - a)});
        a = b + 1;
    }
    return out;
}

// Names, by their lower case (how the game compares them), as first written.
using NameMap = std::map<std::string, std::string>;

// The names the other sheets refer to: a block's names (the first text field), or a field's values.
NameMap ColumnValues(const Book& book, const std::string& sheetName, int field) {
    NameMap out;
    for (const sheetio::Sheet& sh : book) {
        if (sh.name != sheetName) continue;
        SheetInfo info = Describe(sh);
        auto it = info.columns.find({field, 0});
        if (it == info.columns.end()) continue;
        const int col = it->second.back();
        for (const auto& [rc, cell] : sh.cells)
            if (rc.first >= 4 && rc.second == col && !Empty(&cell)) out.emplace(Lower(Trim(cell.text)), Trim(cell.text));
    }
    return out;
}

bool HasSheet(const Book& book, const char* name) {
    return std::any_of(book.begin(), book.end(), [&](const sheetio::Sheet& s) { return s.name == name; });
}

struct Names {
    bool items = false, spells = false; // whether the sheets they come from are in the book
    NameMap itemNames, materials, spellCodes, modifiers;
    // The names of a sheet's field, read once.
    mutable std::map<std::pair<std::string, int>, NameMap> targets;
    const NameMap& TargetNames(const Book& book, const std::string& sheet, int field) const {
        auto key = std::make_pair(sheet, field);
        auto it = targets.find(key);
        if (it == targets.end()) it = targets.emplace(key, ColumnValues(book, sheet, field)).first;
        return it->second;
    }
    explicit Names(const Book& book) {
        items = HasSheet(book, "Materials") && HasSheet(book, "Weapons");
        spells = HasSheet(book, "SpellPrototypes") && HasSheet(book, "SpellModifiers");
        for (const char* s : {"Weapons", "Armors", "QuickItems", "QuestItems", "LootItems"})
            for (const auto& n : ColumnValues(book, s, 0)) itemNames.insert(n);
        materials = ColumnValues(book, "Materials", 0);
        spellCodes = ColumnValues(book, "SpellPrototypes", 1);
        modifiers = ColumnValues(book, "SpellModifiers", 1);
    }
};

// The edit distance between two lower-case words (insertions, deletions, substitutions and swaps of
// two neighbours each count 1), or limit + 1 when it is more than limit.
size_t Distance(const std::string& a, const std::string& b, size_t limit) {
    if ((a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) > limit) return limit + 1;
    std::vector<size_t> prev2(b.size() + 1), prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > limit) return limit + 1;
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[b.size()];
}

// The known names closest to a mistyped one (at most 3, the closest first): one letter off for short
// words, up to 3 for long ones.
std::vector<std::string> Suggest(const std::string& word, const NameMap& names) {
    const std::string w = Lower(Trim(word));
    if (w.empty()) return {};
    const size_t limit = w.size() <= 4 ? 1 : w.size() <= 10 ? 2 : 3;
    std::vector<std::pair<size_t, std::string>> found;
    for (const auto& [lower, shown] : names) {
        const size_t d = Distance(w, lower, limit);
        if (d <= limit) found.push_back({d, shown});
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (const auto& f : found) {
        if (out.size() == 3 || f.first > found.front().first) break;
        out.push_back(f.second);
    }
    return out;
}

// A problem with a cell's text, and the ways to fix it: each fix replaces len characters at pos (in the
// checked text) with text.
struct Edit {
    size_t pos, len;
    std::string text, label;
};
struct Problem {
    std::string message; // empty: no problem
    std::vector<Edit> fixes;
    explicit operator bool() const { return !message.empty(); }
};

std::string Quoted(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? (i + 1 == v.size() ? " or '" : ", '") : "'") + v[i] + "'";
    return out;
}

// "unknown X 'word'" with the closest names as fixes of the word at pos.
Problem Unknown(const std::string& what, const std::string& word, size_t pos, const NameMap& names, const std::string& context) {
    Problem p;
    p.message = "unknown " + what + " '" + word + "'" + (context.empty() ? "" : " in '" + context + "'");
    const std::vector<std::string> close = Suggest(word, names);
    if (!close.empty()) p.message += " - did you mean " + Quoted(close) + "?";
    for (const std::string& c : close) p.fixes.push_back({pos, word.size(), c, c});
    return p;
}

// Whether every piece inside the { } of this entry, split at ';' AND ',', is a known modifier: the vanilla
// database has 7 entries with a ',' between two modifiers ("lightning {e2;e2;t1,fe}"); whether the game reads
// them is unverified, so they are reported as a warning, not an error (#95).
bool CommaBetweenModifiers(const std::string& text, const Names& n) {
    const size_t open = text.find('{'), close = text.find('}', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) return false;
    size_t a = open + 1;
    bool sawComma = false;
    while (a < close) {
        size_t b = a;
        while (b < close && text[b] != ';' && text[b] != ',') ++b;
        if (b < close && text[b] == ',') sawComma = true;
        const std::string mod = Trim(text.substr(a, b - a));
        if (!mod.empty() && !n.modifiers.count(Lower(mod))) return false;
        a = b + 1;
    }
    return sawComma;
}

// "acid_fog {ee3;ee3;ee2}": a spell code and its modifiers (positions are in text).
Problem CheckSpell(const std::string& text, const Names& n) {
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    const size_t open = text.find('{', start);
    std::string code = text.substr(start, open == std::string::npos ? std::string::npos : open - start);
    while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) code.pop_back();
    if (!n.spellCodes.count(Lower(code))) return Unknown("spell", code, start, n.spellCodes, "");
    if (open == std::string::npos) return {};
    const size_t close = text.find('}', open);
    if (close == std::string::npos || Trim(text.substr(close + 1)) != "") {
        Problem p;
        p.message = "'" + Trim(text) + "': the modifiers' {...} is not closed";
        if (close == std::string::npos) p.fixes.push_back({text.find_last_not_of(" \t") + 1, 0, "}", "add '}'"});
        return p;
    }
    size_t a = open + 1;
    while (a <= close) {
        size_t b = text.find(';', a);
        if (b == std::string::npos || b > close) b = close;
        const std::string raw = text.substr(a, b - a);
        const size_t lead = raw.find_first_not_of(" \t");
        if (lead != std::string::npos) {
            const std::string mod = Trim(raw);
            if (!n.modifiers.count(Lower(mod))) return Unknown("spell modifier", mod, a + lead, n.modifiers, Trim(text));
        }
        a = b + 1;
    }
    return {};
}

// "axe.iron", "material.adamantium [1]", "gipat high helm.tanned thin[healing{ic}]": an item, optionally
// of a material, with a count or an enchantment in brackets (positions are in text).
Problem CheckItem(const std::string& text, const Names& n) {
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    const std::string whole = Trim(text);
    size_t end = text.find_last_not_of(" \t") + 1; // the item part is text[start, end)
    while (end > start && text[end - 1] == ']') {
        const size_t open = text.rfind('[', end - 1);
        if (open == std::string::npos || open < start) return {"'" + whole + "': a ']' without its '['", {}};
        const std::string inner = text.substr(open + 1, end - open - 2);
        const std::string t = Trim(inner);
        const bool count = !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); });
        if (!count && n.spells) {
            Problem p = CheckSpell(inner, n);
            if (p) {
                p.message += " (in '" + whole + "')";
                for (Edit& e : p.fixes) e.pos += open + 1;
                return p;
            }
        }
        end = open;
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    }
    const std::string s = text.substr(start, end - start);
    if (s.find('[') != std::string::npos || s.find(']') != std::string::npos) {
        Problem p{"'" + whole + "': unbalanced [ ]", {}};
        const size_t open = text.rfind('['), close = text.rfind(']');
        if (open != std::string::npos && (close == std::string::npos || close < open))
            p.fixes.push_back({text.find_last_not_of(" \t") + 1, 0, "]", "add ']'"});
        return p;
    }
    const std::string lower = Lower(s);
    if (n.itemNames.count(lower)) return {};
    const size_t dot = s.rfind('.');
    if (dot == std::string::npos) return Unknown("item", s, start, n.itemNames, "");
    const std::string base = s.substr(0, dot), material = s.substr(dot + 1);
    if (Lower(base) != "material" && !n.itemNames.count(Lower(base))) return Unknown("item", base, start, n.itemNames, whole);
    // "rune.e1": a rune of a spell modifier, not of a material (the vanilla database's Monsters drop them)
    if (Lower(base) == "rune" && n.modifiers.count(Lower(Trim(material)))) {
        if (Trim(material) != material) return {"a space after the '.' in '" + whole + "'", {{start + dot + 1, material.find_first_not_of(" \t"), "", "remove the space"}}};
        return {};
    }
    if (!n.materials.count(Lower(material))) {
        const std::string trimmed = Trim(material);
        const size_t spaces = material.find_first_not_of(" \t");
        if (trimmed != material && spaces != std::string::npos && n.materials.count(Lower(trimmed)))
            return {"a space after the '.' in '" + whole + "'", {{start + dot + 1, spaces, "", "remove the space"}}};
        return Unknown("material", material, start + dot + 1, n.materials, whole);
    }
    return {};
}

// Which columns refer to records of other sheets (found by matching the shipped database's values).
struct Reference {
    const char* sheet;
    int field;
    enum Kind { Name, Items, Spells } kind;
    const char* targetSheet; // Name: the sheet whose names (or codes) it holds
    int targetField;
    bool allowNone;          // "none" is accepted
};
const Reference kReferences[] = {
    {"RaceModels", 14, Reference::Name, "HitLocations", 0, false},
    {"RaceModels", 15, Reference::Name, "HitLocations", 0, false},
    {"RaceModels", 16, Reference::Name, "HitLocations", 0, false},
    {"RaceModels", 17, Reference::Name, "HitLocations", 0, false},
    {"Monsters", 1, Reference::Name, "RaceModels", 0, false},
    {"Perks", 10, Reference::Name, "Perks", 1, true},
    {"Perks", 11, Reference::Name, "Skills", 1, true},
    {"Monsters", 29, Reference::Items, nullptr, 0, false},
    {"Monsters", 32, Reference::Items, nullptr, 0, false},
    {"Monsters", 33, Reference::Items, nullptr, 0, false},
    {"Monsters", 42, Reference::Items, nullptr, 0, false},
    {"NPC", 9, Reference::Items, nullptr, 0, false},
    {"NPC", 10, Reference::Items, nullptr, 0, false},
    {"Monsters", 31, Reference::Spells, nullptr, 0, false},
    {"NPC", 11, Reference::Spells, nullptr, 0, false},
    {"QuickItems", 25, Reference::Spells, nullptr, 0, false},
};

const Reference* ReferenceOf(const std::string& sheet, int field) {
    for (const Reference& r : kReferences)
        if (sheet == r.sheet && field == r.field) return &r;
    return nullptr;
}

// The text with every ',' inside { } or [ ] made a ';' (empty when there is none).
std::string CommasInBraces(const std::string& text) {
    std::string out = text;
    int depth = 0;
    bool any = false;
    for (char& ch : out) {
        if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') --depth;
        else if (ch == ',' && depth > 0) { ch = ';'; any = true; }
    }
    return any ? out : std::string();
}

// Two rows with the same name: the columns where they differ ("Level 5 / 7, HP 10 / 12"), or that they
// are identical.
std::string RowDifferences(const sheetio::Sheet& sh, const SheetInfo& info, int rowA, int rowB, int nameCol) {
    auto shown = [](const sheetio::Cell* c) {
        if (Empty(c)) return std::string("(empty)");
        return c->text.size() > 24 ? c->text.substr(0, 21) + "..." : c->text;
    };
    std::vector<std::string> diffs;
    for (const auto& [col, key] : info.markerOf) {
        if (col == nameCol) continue;
        const sheetio::Cell* a = At(sh, rowA, col);
        const sheetio::Cell* b = At(sh, rowB, col);
        bool wa, wb;
        const bool same = Empty(a) || Empty(b) ? Empty(a) == Empty(b)
                        : a->isNumber || b->isNumber ? CellNumber(a, wa) == CellNumber(b, wb) : a->text == b->text;
        if (same) continue;
        std::string title = info.Title(sh, col);
        if (title.empty()) title = sheetio::ColumnName(col);
        diffs.push_back(title + " " + shown(a) + " / " + shown(b));
    }
    if (diffs.empty()) return "the two rows are identical";
    std::string out = "they differ in " + std::to_string(diffs.size()) + " column(s): ";
    for (size_t i = 0; i < diffs.size() && i < 6; ++i) out += (i ? ", " : "") + diffs[i];
    if (diffs.size() > 6) out += ", ...";
    return out;
}

void CheckSheet(const Book& book, const sheetio::Sheet& sh, const Names& names, std::vector<Issue>& out) {
    SheetInfo info = Describe(sh);
    if (info.db.empty()) {
        out.push_back({Issue::Warning, sh.name, 0, 0, "not a database sheet: ignored when compiling", {}});
        return;
    }
    const auto& fieldDefs = DBTYPES.at(info.db).at(info.blockId);
    auto fieldDef = [&](int f) -> const FieldDef* {
        for (const FieldDef& d : fieldDefs) if (d.fieldId == f) return &d;
        return nullptr;
    };
    auto issue = [&](Issue::Severity s, int row, int col, const std::string& msg, std::vector<Issue::Fix> fixes = {}) {
        const std::string title = info.Title(sh, col);
        out.push_back({s, sh.name, row, col, (title.empty() ? "" : "(" + title + ") ") + msg, std::move(fixes)});
    };
    // A problem of one entry of a cell's list (the entry starts at entryPos): its fixes as whole cell texts.
    // Unresolved references (an unknown race, item, spell...) are errors: the game cannot load what is missing.
    auto problem = [&](int row, int col, const std::string& cellText, size_t entryPos, const Problem& p) {
        std::vector<Issue::Fix> fixes;
        for (const Edit& e : p.fixes) {
            std::string fixed = cellText;
            fixed.replace(entryPos + e.pos, e.len, e.text);
            fixes.push_back({e.label, fixed});
        }
        issue(Issue::Error, row, col, p.message, std::move(fixes));
    };
    // Markers the compiler does not know: their columns are ignored.
    for (const auto& [col, key] : info.markerOf) {
        const FieldDef* d = fieldDef(key.first);
        if (!d) issue(Issue::Error, 3, col, "FLD" + std::to_string(key.first) + "-" + std::to_string(key.second) +
                                                ": this block has no field " + std::to_string(key.first) + ", the column is ignored");
    }
    // The data rows.
    std::set<int> rows;
    for (const auto& [rc, cell] : sh.cells)
        if (rc.first >= 4 && !Empty(&cell)) rows.insert(rc.first);
    // Names other sheets (or the game's scripts) refer to: they must be set and unique. Other sheets
    // repeat names on purpose (one row per spell level, for instance).
    static const std::map<std::string, int> kNamed = {
        {"Materials", 0}, {"Weapons", 0}, {"Armors", 0}, {"QuickItems", 0}, {"QuestItems", 0}, {"LootItems", 0},
        {"HitLocations", 0}, {"RaceModels", 0}, {"Monsters", 0}, {"NPC", 0},
        {"SpellPrototypes", 1}, {"SpellModifiers", 1}, {"Perks", 1}, {"Skills", 1}, {"LeverPrototypes", 0},
    };
    const auto named = kNamed.find(sh.name);
    std::map<std::string, int> nameRow;
    std::set<int> flaggedFirst; // first rows of duplicated names, already marked
    // Values in columns without a marker, by row: one pass over the cells (not one per row: a sheet has tens
    // of thousands of cells, and this check was most of a database's check time).
    std::map<int, std::vector<int>> unmarked;
    for (const auto& [rc, cell] : sh.cells)
        if (!Empty(&cell) && !info.markerOf.count(rc.second)) unmarked[rc.first].push_back(rc.second);
    // Monsters: each race's skin textures by its name, once (RaceModels is a big sheet; describing it per
    // monster took most of the rest).
    std::map<std::string, const sheetio::Cell*> raceTextures;
    bool racesKnown = false;
    if (sh.name == "Monsters") {
        const sheetio::Sheet* races = nullptr;
        for (const sheetio::Sheet& other : book) if (other.name == "RaceModels") races = &other;
        if (races) {
            const SheetInfo ri = Describe(*races);
            auto nameCol = ri.columns.find({0, 0}), texCol = ri.columns.find({31, 0});
            if (nameCol != ri.columns.end() && texCol != ri.columns.end()) {
                racesKnown = true;
                for (int r = 4; r <= races->maxRow; ++r) {
                    const sheetio::Cell* n = At(*races, r, nameCol->second.back());
                    if (!Empty(n)) raceTextures.emplace(Lower(Trim(n->text)), At(*races, r, texCol->second.back())); // the first of a name
                }
            }
        }
    }
    for (int row : rows) {
        if (auto u = unmarked.find(row); u != unmarked.end())
            for (int col : u->second) issue(Issue::Error, row, col, "a value in a column without an FLDx-y marker (row 3): ignored");
        // The record's name.
        if (auto it = named == kNamed.end() ? info.columns.end() : info.columns.find({named->second, 0}); it != info.columns.end()) {
            const sheetio::Cell* c = At(sh, row, it->second.back());
            const std::string name = Empty(c) ? "" : Lower(Trim(c->text));
            if (name.empty()) issue(Issue::Warning, row, it->second.back(), "a record without a name");
            else if (auto first = nameRow.find(name); first != nameRow.end()) {
                issue(Issue::Warning, row, it->second.back(), "'" + Trim(c->text) + "' is also the name of row " + std::to_string(first->second) +
                      ": " + RowDifferences(sh, info, first->second, row, it->second.back()) +
                      "; the game uses the last row with this name");
                // The first row too (once), so both are marked. The game keeps the last copy (checked in its memory:
                // a map by name, each copy replacing the one before).
                if (flaggedFirst.insert(first->second).second)
                    issue(Issue::Warning, first->second, it->second.back(), "'" + Trim(c->text) + "' is used again in row " + std::to_string(row) +
                          ": the game ignores this row (it uses the last one)");
            } else nameRow[name] = row;
        }
        // Each field's columns.
        for (const auto& [key, cols] : info.columns) {
            const FieldDef* d = fieldDef(key.first);
            if (!d) continue;
            const int col = cols.back();
            const sheetio::Cell* c = At(sh, row, col);
            // A marker in several columns: only the last is compiled.
            for (size_t k = 0; k + 1 < cols.size(); ++k) {
                const sheetio::Cell* other = At(sh, row, cols[k]);
                bool a, b;
                const bool differ = Empty(other) != Empty(c) ||
                    (!Empty(c) && (c->isNumber || other->isNumber ? CellNumber(c, a) != CellNumber(other, b) : c->text != other->text));
                if (differ)
                    issue(Issue::Error, row, cols[k], "differs from column " + sheetio::ColumnName(col) + " with the same marker FLD" +
                          std::to_string(key.first) + "-" + std::to_string(key.second) + ": only " + sheetio::ColumnName(col) + " is compiled");
            }
            if (Empty(c)) continue;
            FieldType type = d->type;
            if (type == FieldType::TypeList) { // the sub-field's own type
                const FieldDef* sub = nullptr;
                for (const FieldDef& sf : DBTYPES.at(info.db).at(d->typeArg)) if (sf.fieldId == key.second) sub = &sf;
                if (!sub) { issue(Issue::Error, row, col, "no sub-field " + std::to_string(key.second) + ": ignored"); continue; }
                type = sub->type;
            }
            const bool numeric = type == FieldType::SignedLong || type == FieldType::UnsignedLong || type == FieldType::Float ||
                                 type == FieldType::Byte || type == FieldType::BitList || type == FieldType::FloatList ||
                                 type == FieldType::ByteList || type == FieldType::UnsignedLongList;
            if (numeric) {
                bool whole;
                const double v = CellNumber(c, whole);
                const bool floating = type == FieldType::Float || type == FieldType::FloatList;
                const std::string t = Lower(Trim(c->text));
                if (!c->isNumber && !whole && !(floating && (t == "nan" || t == "inf" || t == "-inf"))) {
                    issue(Issue::Error, row, col, "'" + c->text + "' is not a number: compiled as " + DoubleText(v));
                    continue;
                }
                if (!floating && std::isfinite(v) && v != std::floor(v))
                    issue(Issue::Warning, row, col, DoubleText(v) + ": a whole number is expected, compiled as " + DoubleText(std::trunc(v)));
                if ((type == FieldType::Byte || type == FieldType::ByteList) && (v < 0 || v > 255))
                    issue(Issue::Error, row, col, DoubleText(v) + " is outside 0-255 (one byte)");
                if ((type == FieldType::UnsignedLong || type == FieldType::UnsignedLongList) && (v < 0 || v > 4294967295.0))
                    issue(Issue::Error, row, col, DoubleText(v) + " is outside 0-4294967295 (unsigned 32 bits)");
                if (type == FieldType::SignedLong && (v < -2147483648.0 || v > 2147483647.0))
                    issue(Issue::Error, row, col, DoubleText(v) + " is outside the 32-bit range");
                if (floating && std::isfinite(v) && std::fabs(v) > 3.4028234e38)
                    issue(Issue::Error, row, col, DoubleText(v) + " is too large for a float");
                if (type == FieldType::BitList && v != 0 && v != 1)
                    issue(Issue::Warning, row, col, DoubleText(v) + ": a flag is 0 or 1 (any other number counts as 1)");
                continue;
            }
            // Text.
            if (c->isNumber && type != FieldType::String && type != FieldType::StringList) continue;
            const std::string text = c->text;
            if (const std::string bad = Unstorable(text); !bad.empty())
                issue(Issue::Error, row, col, "characters a database cannot hold (Windows-1251), they become '?': " + bad);
            if (type == FieldType::FixedString && static_cast<int>(cp1251::EncodeCp1251(text).size()) > d->typeArg)
                issue(Issue::Error, row, col, "'" + text + "' is longer than " + std::to_string(d->typeArg) + " characters: it is cut");
            if (type == FieldType::Hex && !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch); }))
                issue(Issue::Error, row, col, "'" + text + "' is not hexadecimal");
            if (type == FieldType::StringList || type == FieldType::MinfixedStringList) {
                int depth = 0;
                for (char ch : text) {
                    if (ch == '{' || ch == '[') ++depth;
                    else if (ch == '}' || ch == ']') --depth;
                    else if (ch == ',' && depth > 0) {
                        const bool vanillaForm = names.spells && CommaBetweenModifiers(text, names);
                        issue(vanillaForm ? Issue::Warning : Issue::Error, row, col,
                              vanillaForm ? "a ',' between modifiers inside { }: the vanilla database has this form, whether the game reads it is unverified (';' is the documented separator)"
                                          : "a ',' inside { } or [ ] cuts the entry in two (lists are separated by commas; modifiers use ';')",
                              {{"',' -> ';' inside { } and [ ]", CommasInBraces(text)}});
                        break;
                    }
                }
            }
            if (type == FieldType::AcksUniqueType) {
                // "{1=text##2=5}{...}": text outside the braces is dropped, keys must be known sub-fields.
                const auto& subfields = DBTYPES.at("Acks").at(d->typeArg);
                size_t pos = 0;
                std::string problem;
                while (pos < text.size() && problem.empty()) {
                    const size_t open = text.find('{', pos);
                    if (Trim(text.substr(pos, open == std::string::npos ? std::string::npos : open - pos)) != "")
                        problem = "text outside { } is dropped";
                    if (open == std::string::npos) break;
                    const size_t close = text.find('}', open);
                    if (close == std::string::npos) { problem = "a '{' without its '}'"; break; }
                    const std::string body = text.substr(open + 1, close - open - 1);
                    size_t p = 0;
                    while (p <= body.size() && problem.empty()) {
                        size_t sep = body.find("##", p);
                        if (sep == std::string::npos) sep = body.size();
                        const std::string part = body.substr(p, sep - p);
                        const size_t eq = part.find('=');
                        int key = 0;
                        if (eq == std::string::npos || std::sscanf(part.c_str(), "%d", &key) != 1) problem = "'" + part + "' is not key=value";
                        else if (std::none_of(subfields.begin(), subfields.end(), [&](const FieldDef& sf) { return sf.fieldId == key; }))
                            problem = "unknown key " + std::to_string(key);
                        p = sep + 2;
                    }
                    pos = close + 1;
                }
                if (!problem.empty()) issue(Issue::Error, row, col, "Acks list: " + problem);
            }
        }
        // References to other sheets.
        for (const Reference& ref : kReferences) {
            if (sh.name != std::string(ref.sheet)) continue;
            auto it = info.columns.find({ref.field, 0});
            if (it == info.columns.end()) continue;
            const sheetio::Cell* c = At(sh, row, it->second.back());
            if (Empty(c)) continue;
            if (ref.kind == Reference::Name) {
                if (!HasSheet(book, ref.targetSheet)) continue;
                const NameMap& targets = names.TargetNames(book, ref.targetSheet, ref.targetField);
                for (const Entry& e : SplitEntries(c->text)) {
                    const std::string key = Lower(Trim(e.text));
                    if (key.empty() || (ref.allowNone && key == "none") || targets.count(key)) continue;
                    const std::string word = Trim(e.text);
                    Problem p{"'" + word + "' is not in " + ref.targetSheet, {}};
                    const std::vector<std::string> close = Suggest(word, targets);
                    if (!close.empty()) p.message += " - did you mean " + Quoted(close) + "?";
                    for (const std::string& name : close) p.fixes.push_back({e.text.find_first_not_of(" \t"), word.size(), name, name});
                    problem(row, it->second.back(), c->text, e.pos, p);
                }
            } else if (ref.kind == Reference::Items ? names.items : names.spells) {
                // The game splits these lists at every comma, braces or not.
                int depth = 0;
                bool cut = false;
                for (char ch : c->text) {
                    if (ch == '{' || ch == '[') ++depth;
                    else if (ch == '}' || ch == ']') --depth;
                    else if (ch == ',' && depth > 0) cut = true;
                }
                if (cut) {
                    const bool vanillaForm = ref.kind != Reference::Items && CommaBetweenModifiers(c->text, names);
                    issue(vanillaForm ? Issue::Warning : Issue::Error, row, it->second.back(),
                          "'" + c->text + (vanillaForm ? "': a ',' between modifiers inside { }: the vanilla database has this form, whether the game reads it is unverified (';' is the documented separator)"
                                                       : "': a ',' inside { } or [ ] cuts the entry in two (the list is separated by commas; spell modifiers are separated by ';')"),
                          {{"',' -> ';' inside { } and [ ]", CommasInBraces(c->text)}});
                    continue; // the vanilla form's modifiers were checked by CommaBetweenModifiers; the list splitter would cut it
                }
                for (const Entry& e : SplitEntries(c->text)) {
                    const Problem p = ref.kind == Reference::Items ? CheckItem(e.text, names) : CheckSpell(e.text, names);
                    if (p) problem(row, it->second.back(), c->text, e.pos, p);
                }
            }
        }
        // Values with a known set (the game reads them by name or index).
        auto cellOf = [&](int field) -> const sheetio::Cell* {
            auto it = info.columns.find({field, 0});
            return it == info.columns.end() ? nullptr : At(sh, row, it->second.back());
        };
        auto colOf = [&](int field) { auto it = info.columns.find({field, 0}); return it == info.columns.end() ? 0 : it->second.back(); };
        if (sh.name == "Skills") { // Base attribute: str, dex or int
            const sheetio::Cell* c = cellOf(10);
            const std::string v = Empty(c) ? "" : Lower(Trim(c->text));
            if (!v.empty() && v != "str" && v != "dex" && v != "int")
                issue(Issue::Error, row, colOf(10), "'" + c->text + "': the base attribute is str, dex or int");
        }
        if (sh.name == "Monsters") { // the skin index picks one of its race's primary textures
            const sheetio::Cell* race = cellOf(1);
            const sheetio::Cell* skin = cellOf(3);
            if (!Empty(race) && !Empty(skin) && racesKnown) {
                const auto rt = raceTextures.find(Lower(Trim(race->text)));
                if (rt != raceTextures.end()) {
                    const sheetio::Cell* t = rt->second;
                    const std::vector<Entry> textures = Empty(t) ? std::vector<Entry>() : SplitEntries(t->text);
                    bool whole;
                    const double idx = CellNumber(skin, whole);
                    const bool numbered = !textures.empty() && Lower(Trim(textures[0].text)).rfind("skin", 0) == 0;
                    // Races listing "Skin_NN" textures take any number past the list as <figure>skin_NN (not
                    // checkable here: no texture sources); the others only have their list.
                    if (idx < 0 || (idx >= static_cast<double>(textures.size()) && !numbered))
                        issue(Issue::Error, row, colOf(3), "skin " + DoubleText(idx) + ": the race '" + Trim(race->text) + "' has " +
                              std::to_string(textures.size()) + " skin texture(s) (0-" + std::to_string(static_cast<int>(textures.size()) - 1) + ")");
                }
            }
            const sheetio::Cell* hair = cellOf(4);
            if (!Empty(hair)) {
                bool whole;
                const double h = CellNumber(hair, whole);
                if (h < -1 || h > 99) issue(Issue::Error, row, colOf(4), "hair " + DoubleText(h) + ": -1 (none) or a hr.NN of the figure (0-99)");
            }
        }
    }
}

} // namespace

std::string NumberText(double v) { return DoubleText(v); }

ColumnInfo DescribeColumn(const sheetio::Sheet& sh, int col) {
    static const char* const kTypeNames[] = {"String", "SignedLong", "UnsignedLong", "Float", "Byte", "Hex", "FixedString",
                                             "BitList", "ByteList", "FloatList", "UnsignedLongList", "StringList",
                                             "MinfixedStringList", "AcksUniqueType", "TypeList"};
    ColumnInfo out;
    const sheetio::Cell* marker = At(sh, 3, col);
    if (!marker) return out;
    out.marker = marker->text;
    int f = 0, i = 0;
    if (std::sscanf(marker->text.c_str(), "FLD%d-%d", &f, &i) != 2) return out;
    SheetInfo info = Describe(sh);
    if (info.db.empty()) return out;
    const FieldDef* def = nullptr;
    for (const FieldDef& d : DBTYPES.at(info.db).at(info.blockId)) if (d.fieldId == f) def = &d;
    if (!def) return out;
    FieldType type = def->type;
    if (type == FieldType::TypeList) { // the sub-field's own type
        const FieldDef* sub = nullptr;
        for (const FieldDef& d : DBTYPES.at(info.db).at(def->typeArg)) if (d.fieldId == i) sub = &d;
        if (!sub) return out;
        type = sub->type;
    }
    out.known = true;
    out.type = kTypeNames[static_cast<int>(type)];
    out.numeric = type == FieldType::SignedLong || type == FieldType::UnsignedLong || type == FieldType::Float ||
                  type == FieldType::Byte || type == FieldType::BitList || type == FieldType::FloatList ||
                  type == FieldType::ByteList || type == FieldType::UnsignedLongList;
    if (auto headers = DBHEADERS.find(info.db); headers != DBHEADERS.end())
        for (const ColumnHeader& h : headers->second)
            if (h.blockId == info.blockId && h.fieldId == f && h.fieldIndex == i && h.description) out.description = h.description;
    return out;
}

// ---- completion ----------------------------------------------------------------------------------------

struct Completer::Data {
    const Book& book;
    Names names;
    explicit Data(const Book& b) : book(b), names(b) {}
};

Completer::Completer(const Book& book) : d_(std::make_unique<Data>(book)) {}
Completer::~Completer() = default;

namespace {

constexpr size_t kMaxCandidates = 40;

// The names matching a word (case ignored): those starting with it first, then those containing it,
// each as prefix + name. Nothing for an empty word or when the word is already a full name only.
void Match(const NameMap& names, const std::string& word, const std::string& prefix, std::vector<std::string>& out) {
    const std::string w = Lower(word);
    if (w.empty()) return;
    std::vector<std::string> starts, contains;
    for (const auto& [lower, shown] : names) {
        if (lower == w) continue;
        if (lower.compare(0, w.size(), w) == 0) starts.push_back(prefix + shown);
        else if (lower.find(w) != std::string::npos) contains.push_back(prefix + shown);
    }
    for (auto* list : {&starts, &contains})
        for (const std::string& s : *list)
            if (out.size() < kMaxCandidates) out.push_back(s);
}

size_t SkipSpaces(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// A spell being typed ("heal", "healing {e", "healing {e2;d"): its code or the modifier being typed.
void CompleteSpell(const Names& n, const std::string& typed, size_t start, Completion& out) {
    const size_t brace = typed.rfind('{');
    if (brace != std::string::npos && brace >= start) {
        const size_t semi = typed.rfind(';');
        size_t seg = (semi != std::string::npos && semi > brace ? semi : brace) + 1;
        seg = SkipSpaces(typed, seg);
        out.tokenStart = seg;
        Match(n.modifiers, typed.substr(seg), "", out.candidates);
        return;
    }
    out.tokenStart = start;
    Match(n.spellCodes, typed.substr(start), "", out.candidates);
}

} // namespace

Completion Completer::Complete(const sheetio::Sheet& sh, int col, const std::string& typed) const {
    Completion out;
    const sheetio::Cell* marker = At(sh, 3, col);
    int f = -1, i = -1;
    if (!marker || std::sscanf(marker->text.c_str(), "FLD%d-%d", &f, &i) != 2) return out;
    const Names& n = d_->names;
    const Reference* ref = i == 0 ? ReferenceOf(sh.name, f) : nullptr;
    if (ref) {
        // The entry being typed: after the last comma.
        const size_t comma = typed.rfind(',');
        const size_t start = SkipSpaces(typed, comma == std::string::npos ? 0 : comma + 1);
        const std::string token = typed.substr(start);
        out.tokenStart = start;
        if (ref->kind == Reference::Name) {
            NameMap names = n.TargetNames(d_->book, ref->targetSheet, ref->targetField);
            if (ref->allowNone) names.emplace("none", "none");
            Match(names, token, "", out.candidates);
        } else if (ref->kind == Reference::Spells) {
            CompleteSpell(n, typed, start, out);
        } else {
            const size_t open = token.rfind('[');
            if (open != std::string::npos && token.find(']', open) == std::string::npos) {
                CompleteSpell(n, typed, SkipSpaces(typed, start + open + 1), out); // an enchantment: item[spell{...}]
            } else if (open == std::string::npos) {
                const size_t dot = token.rfind('.');
                if (dot != std::string::npos) {
                    Match(n.materials, token.substr(dot + 1), token.substr(0, dot + 1), out.candidates); // item.material
                    if (Lower(token.substr(0, dot)) == "rune") Match(n.modifiers, token.substr(dot + 1), token.substr(0, dot + 1), out.candidates); // rune.<modifier>
                } else {
                    NameMap items = n.itemNames;
                    items.emplace("material", "material");
                    Match(items, token, "", out.candidates);
                }
            }
        }
        return out;
    }
    // Other text columns: the values the column already has.
    if (DescribeColumn(sh, col).numeric) return out;
    NameMap values;
    for (const auto& [rc, cell] : sh.cells)
        if (rc.first >= 4 && rc.second == col && !cell.isNumber && !cell.text.empty()) values.emplace(Lower(cell.text), cell.text);
    out.tokenStart = 0;
    Match(values, typed, "", out.candidates);
    return out;
}

std::vector<Issue> CheckBook(const Book& book) {
    const Names names(book);
    std::vector<Issue> out;
    for (const sheetio::Sheet& sh : book) CheckSheet(book, sh, names, out);
    return out;
}

} // namespace dbmodel
