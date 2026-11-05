// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Comparing two gameplay databases. See db_diff.hpp.

#include "db_diff.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include "version.hpp"

namespace dbdiff {

namespace {

constexpr int kTitleRow = 2, kMarkerRow = 3, kFirstRow = 4, kFirstCol = 2; // DBEditor's layout (db_model.hpp)

const sheetio::Cell* At(const sheetio::Sheet& s, int row, int col) {
    auto it = s.cells.find({row, col});
    return it == s.cells.end() ? nullptr : &it->second;
}

std::string Text(const sheetio::Sheet& s, int row, int col) {
    const sheetio::Cell* c = At(s, row, col);
    return c ? c->text : std::string();
}

bool Number(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}

// The columns of a sheet by title: title (with "#n" for repeats) -> column.
std::map<std::string, int> ColumnKeys(const sheetio::Sheet& s, std::vector<std::string>& order) {
    std::map<std::string, int> out, seen;
    for (int c = kFirstCol; c <= s.maxCol; ++c) {
        std::string t = Text(s, kTitleRow, c);
        if (t.empty()) t = Text(s, kMarkerRow, c);
        if (t.empty()) t = sheetio::ColumnName(c);
        const int n = ++seen[t];
        if (n > 1) t += "#" + std::to_string(n);
        out[t] = c;
        order.push_back(t);
    }
    return out;
}

// The column that names the records: "Name", else "Code", else B.
int KeyColumn(const sheetio::Sheet& s) {
    int code = 0;
    for (int c = kFirstCol; c <= s.maxCol; ++c) {
        const std::string t = Text(s, kTitleRow, c);
        if (t == "Name") return c;
        if (t == "Code" && !code) code = c;
    }
    return code ? code : kFirstCol;
}

// The records of a sheet: key (with "#n" for repeats) -> row. Empty rows are skipped.
std::map<std::string, int> RowKeys(const sheetio::Sheet& s, std::vector<std::string>& order) {
    std::map<std::string, int> out, seen;
    const int kc = KeyColumn(s);
    std::set<int> rows;
    for (const auto& kv : s.cells)
        if (kv.first.first >= kFirstRow && !kv.second.text.empty()) rows.insert(kv.first.first);
    for (int r : rows) {
        std::string k = Text(s, r, kc);
        if (k.empty()) k = "row " + std::to_string(r);
        const int n = ++seen[k];
        if (n > 1) k += "#" + std::to_string(n);
        out[k] = r;
        order.push_back(k);
    }
    return out;
}

bool SameCell(const std::string& a, const std::string& b, bool& numeric, double& ratio) {
    double x = 0, y = 0;
    numeric = Number(a, x) && Number(b, y);
    if (numeric) {
        ratio = x != 0 ? y / x : 0;
        return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
    }
    return a == b;
}

SheetDiff CompareSheet(const sheetio::Sheet& a, const sheetio::Sheet& b) {
    SheetDiff d;
    d.name = a.name;
    std::vector<std::string> colOrderA, colOrderB, rowOrderA, rowOrderB;
    const auto colsA = ColumnKeys(a, colOrderA), colsB = ColumnKeys(b, colOrderB);
    const auto rowsA = RowKeys(a, rowOrderA), rowsB = RowKeys(b, rowOrderB);
    for (const std::string& c : colOrderA) if (!colsB.count(c)) d.columnsRemoved.push_back(c);
    for (const std::string& c : colOrderB) if (!colsA.count(c)) d.columnsAdded.push_back(c);
    for (const std::string& r : rowOrderA) if (!rowsB.count(r)) { d.rowsRemoved.push_back(r); d.rowsRemovedAt.push_back(rowsA.at(r)); }
    for (const std::string& r : rowOrderB) if (!rowsA.count(r)) d.rowsAdded.push_back(r);
    for (const std::string& rk : rowOrderA) {
        auto rb = rowsB.find(rk);
        if (rb == rowsB.end()) continue;
        const int ra = rowsA.at(rk);
        bool any = false;
        for (const std::string& ck : colOrderA) {
            auto cb = colsB.find(ck);
            if (cb == colsB.end()) continue;
            const int ca = colsA.at(ck);
            const std::string ta = Text(a, ra, ca), tb = Text(b, rb->second, cb->second);
            bool numeric = false;
            double ratio = 0;
            if (SameCell(ta, tb, numeric, ratio)) continue;
            CellChange ch;
            ch.key = rk; ch.column = ck;
            ch.rowA = ra; ch.colA = ca; ch.rowB = rb->second; ch.colB = cb->second;
            ch.before = ta; ch.after = tb; ch.numeric = numeric; ch.ratio = ratio;
            d.changes.push_back(std::move(ch));
            any = true;
        }
        if (any) ++d.rowsChanged;
    }
    return d;
}

std::string Csv(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

std::string Factor(double lo, double hi) {
    char buf[64];
    if (std::fabs(hi - lo) < 1e-6) std::snprintf(buf, sizeof(buf), "x%.4g", lo);
    else std::snprintf(buf, sizeof(buf), "x%.4g..x%.4g", lo, hi);
    return buf;
}

} // namespace

size_t BookDiff::ChangedCells() const {
    size_t n = 0;
    for (const SheetDiff& s : sheets) n += s.changes.size();
    return n;
}

bool BookDiff::Same() const {
    return std::all_of(sheets.begin(), sheets.end(), [](const SheetDiff& s) { return s.Same(); });
}

BookDiff Compare(const dbmodel::Book& a, const dbmodel::Book& b) {
    BookDiff d;
    std::map<std::string, const sheetio::Sheet*> byName;
    for (const sheetio::Sheet& s : b) byName[s.name] = &s;
    std::set<std::string> inA;
    for (const sheetio::Sheet& s : a) {
        inA.insert(s.name);
        auto it = byName.find(s.name);
        if (it == byName.end()) { SheetDiff x; x.name = s.name; x.onlyInA = true; d.sheets.push_back(std::move(x)); continue; }
        d.sheets.push_back(CompareSheet(s, *it->second));
    }
    for (const sheetio::Sheet& s : b)
        if (!inA.count(s.name)) { SheetDiff x; x.name = s.name; x.onlyInB = true; d.sheets.push_back(std::move(x)); }
    return d;
}

std::vector<std::string> Summary(const BookDiff& d) {
    std::vector<std::string> out;
    for (const SheetDiff& s : d.sheets) {
        if (s.onlyInA) { out.push_back(s.name + ": only in the first file"); continue; }
        if (s.onlyInB) { out.push_back(s.name + ": only in the second file"); continue; }
        if (s.Same()) continue;
        std::string line = s.name + ": " + std::to_string(s.changes.size()) + " cell(s) in " + std::to_string(s.rowsChanged) + " row(s)";
        if (!s.rowsAdded.empty()) line += ", " + std::to_string(s.rowsAdded.size()) + " row(s) added";
        if (!s.rowsRemoved.empty()) line += ", " + std::to_string(s.rowsRemoved.size()) + " row(s) removed";
        if (!s.columnsAdded.empty()) line += ", " + std::to_string(s.columnsAdded.size()) + " column(s) added";
        if (!s.columnsRemoved.empty()) line += ", " + std::to_string(s.columnsRemoved.size()) + " column(s) removed";
        out.push_back(line);
        // per column: cells, and the factor range when every changed cell is a non-zero number
        std::vector<std::string> order;
        std::map<std::string, std::vector<const CellChange*>> byCol;
        for (const CellChange& c : s.changes) {
            if (!byCol.count(c.column)) order.push_back(c.column);
            byCol[c.column].push_back(&c);
        }
        for (const std::string& col : order) {
            const auto& v = byCol[col];
            double lo = 1e300, hi = -1e300;
            bool allNum = true;
            for (const CellChange* c : v) {
                if (!c->numeric || c->ratio == 0) { allNum = false; break; }
                lo = std::min(lo, c->ratio); hi = std::max(hi, c->ratio);
            }
            out.push_back("    " + col + ": " + std::to_string(v.size()) + " cell(s)" + (allNum ? ", " + Factor(lo, hi) : std::string()));
        }
        for (const std::string& r : s.rowsAdded) out.push_back("    + " + r);
        for (const std::string& r : s.rowsRemoved) out.push_back("    - " + r);
        for (const std::string& c : s.columnsAdded) out.push_back("    + column " + c);
        for (const std::string& c : s.columnsRemoved) out.push_back("    - column " + c);
    }
    return out;
}

bool WriteCsv(const BookDiff& d, const std::string& path, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write " + path; return false; }
    f << "sheet,record,column,first,second,factor\n";
    for (const SheetDiff& s : d.sheets) {
        for (const CellChange& c : s.changes) {
            char factor[32] = "";
            if (c.numeric && c.ratio != 0) std::snprintf(factor, sizeof(factor), "%.6g", c.ratio);
            f << Csv(s.name) << ',' << Csv(c.key) << ',' << Csv(c.column) << ',' << Csv(c.before) << ',' << Csv(c.after) << ',' << factor << '\n';
        }
        for (const std::string& r : s.rowsAdded) f << Csv(s.name) << ',' << Csv(r) << ",(row added),,,\n";
        for (const std::string& r : s.rowsRemoved) f << Csv(s.name) << ',' << Csv(r) << ",(row removed),,,\n";
    }
    return true;
}

} // namespace dbdiff

static void PrintDbDiffUsage() {
    std::cout << "um-multitool dbdiff - compare two gameplay databases\n\n"
              << "Usage:\n"
              << "  um-multitool dbdiff [options] <first> <second>\n\n"
              << "  <first>, <second>  .res, .xlsx or .ods (any mix: a compiled database against its spreadsheet works)\n\n"
              << "Options:\n"
              << "  --cells            List every changed cell (record, column, first -> second)\n"
              << "  --sheet <name>     Only this sheet (Monsters, Materials...)\n"
              << "  --csv <file>       Write every changed cell to a .csv file\n"
              << "  -h, --help         Show this help\n\n"
              << "Records are matched by their Name column (else Code), columns by their title, so inserted or moved\n"
              << "rows do not shift the comparison. Numbers are compared as numbers; the summary gives the factor\n"
              << "(second / first) of each changed column.\n"
              << "Exit code: 0 the same, 1 different, 2 error.\n\n"
              << "Example:\n"
              << "  um-multitool dbdiff databaselmp.xlsx databaselmp-x3.xlsx --csv changes.csv\n";
}

int RunDbDiff(int argc, char* argv[]) {
    std::vector<std::string> files;
    std::string csv, onlySheet;
    bool cells = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { PrintDbDiffUsage(); return 0; }
        if (a == "--cells") { cells = true; continue; }
        if (a == "--csv" && i + 1 < argc) { csv = argv[++i]; continue; }
        if (a == "--sheet" && i + 1 < argc) { onlySheet = argv[++i]; continue; }
        if (!a.empty() && a[0] == '-') { std::cerr << "unknown option " << a << "\n"; return 2; }
        files.push_back(a);
    }
    if (files.size() != 2) { PrintDbDiffUsage(); return 2; }
    dbmodel::Book a, b;
    std::string err;
    if (!dbmodel::LoadBook(files[0], a, err)) { std::cerr << files[0] << ": " << err << "\n"; return 2; }
    if (!dbmodel::LoadBook(files[1], b, err)) { std::cerr << files[1] << ": " << err << "\n"; return 2; }
    if (!onlySheet.empty()) {
        auto keep = [&](dbmodel::Book& book) {
            book.erase(std::remove_if(book.begin(), book.end(), [&](const sheetio::Sheet& s) { return s.name != onlySheet; }), book.end());
        };
        keep(a); keep(b);
        if (a.empty() && b.empty()) { std::cerr << "no sheet named " << onlySheet << "\n"; return 2; }
    }
    const dbdiff::BookDiff d = dbdiff::Compare(a, b);
    std::cout << "first:  " << files[0] << "\nsecond: " << files[1] << "\n";
    if (d.Same()) { std::cout << "The databases are the same.\n"; return 0; }
    std::cout << d.ChangedCells() << " changed cell(s)\n";
    for (const std::string& line : dbdiff::Summary(d)) std::cout << line << "\n";
    if (cells) {
        for (const dbdiff::SheetDiff& s : d.sheets)
            for (const dbdiff::CellChange& c : s.changes)
                std::cout << s.name << " | " << c.key << " | " << c.column << " | " << c.before << " -> " << c.after << "\n";
    }
    if (!csv.empty()) {
        if (!dbdiff::WriteCsv(d, csv, err)) { std::cerr << err << "\n"; return 2; }
        std::cout << "written " << csv << "\n";
    }
    return 1;
}
