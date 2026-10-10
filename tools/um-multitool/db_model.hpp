// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The gameplay databases as spreadsheets, in memory: what the DB tab edits, and what the xlsxdb / dbexport
// commands convert. A "book" is the workbook's sheets (sheetio::Sheet: DBEditor's layout, titles in row
// 2, FLDx-y markers in row 3, records from row 4, from column B).
//   - LoadBook / SaveBook: from and to .xlsx, .ods, .res or a single database file (units.udb...);
//   - CompileBook: to the .res xlsxdb writes (byte for byte);
//   - CheckBook: what would be lost or changed by compiling, and values that look wrong.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sheet_io.hpp"
#include "xlsx_reader.hpp"

namespace dbmodel {

using Book = std::vector<sheetio::Sheet>;

struct GeneratedFile {
    std::string name;
    std::vector<uint8_t> data;
};

// xlsxdb.cpp: the databases of a workbook, and the .res holding them.
std::vector<GeneratedFile> EncodeWorkbook(xlsxlib::Workbook& wb);
std::vector<uint8_t> PackRes(const std::vector<GeneratedFile>& files);

// dbexport.cpp: the sheets of the databases a .res (or one database file, named like units.udb) holds;
// `summary` gets one line per database ("units.udb: 804 record(s)").
bool DecodeToBook(const std::vector<uint8_t>& bytes, const std::string& fileName, Book& book, std::vector<std::string>& summary,
                  std::string& err);

// db_model.cpp
bool LoadBook(const std::string& path, Book& book, std::string& err);
bool SaveBook(const std::string& path, const Book& book, std::string& err); // .xlsx or .ods
bool CompileBook(const Book& book, std::vector<uint8_t>& res, std::vector<GeneratedFile>* files, std::string& err);
// A database as .res bytes, whatever its form: a .res is read, a spreadsheet (.xlsx, .ods) compiled.
bool ReadAsRes(const std::string& path, std::vector<uint8_t>& res, std::string& err);
xlsxlib::Workbook ToWorkbook(const Book& book);

struct Issue {
    enum Severity { Error, Warning } severity;
    std::string sheet;
    int row = 0, col = 0; // the cell (1-based), 0 when it is about the whole sheet
    std::string message;
    // Quick fixes: the cell's whole new text, and what it does ("adamantium", "remove the space").
    struct Fix {
        std::string label, text;
    };
    std::vector<Fix> fixes;
    std::string Where() const; // "Monsters!AE12"
};
std::vector<Issue> CheckBook(const Book& book);
// Prints the issues (errors to stderr, warnings to stdout) as "[ERROR] Monsters!AE12 (Items): ...";
// returns the number of errors.
int PrintIssues(const std::vector<Issue>& issues);

// What a sheet's column holds, from its row-3 marker: for the editor's headers and typing.
struct ColumnInfo {
    bool known = false;   // a marker of a field this block has
    bool numeric = false; // the field is a number (typed numbers are stored as numbers)
    std::string marker;   // "FLD3-0"
    std::string type;     // "Float", "String"...
    std::string description; // DBEditor's, may be empty
};
ColumnInfo DescribeColumn(const sheetio::Sheet& sheet, int col);
std::string NumberText(double v); // the shortest text that reads back as v

// Autocompletion of what is typed into a cell (the text before the cursor): the names that can come
// next, from the sheets the column refers to (items, materials, spells and their modifiers, names) or,
// for other text columns, the column's own values. Replace typed[tokenStart, end) with a candidate.
struct Completion {
    size_t tokenStart = 0;
    std::vector<std::string> candidates;
};
class Completer {
public:
    explicit Completer(const Book& book); // the book must outlive it (rebuild it after changes)
    ~Completer();
    Completion Complete(const sheetio::Sheet& sheet, int col, const std::string& typed) const;
private:
    struct Data;
    std::unique_ptr<Data> d_;
};

} // namespace dbmodel
