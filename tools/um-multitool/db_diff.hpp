// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Comparing two gameplay databases (any mix of .res, .xlsx, .ods: both are read as dbmodel::Book).
// Sheets are matched by name, records by their Name column (else Code, else column B; a repeated key gets
// "#2", "#3"...), columns by their row-2 title (a repeated title gets "#2"...). So rows inserted, deleted
// or moved in one file do not shift the comparison. Numbers are compared as numbers.
//   - Compare: the differences, sheet by sheet;
//   - Summary: one text line per sheet and per changed column (cells, the factor range of number cells);
//   - WriteCsv: every changed cell, one line each;
//   - RunDbDiff: the "dbdiff" command line.
#pragma once

#include <string>
#include <vector>

#include "db_model.hpp"

namespace dbdiff {

struct CellChange {
    std::string key;           // the record ("LMP Skeleton 7", "healing", "row 12")
    std::string column;        // the column's title ("Stats Health")
    int rowA = 0, colA = 0;    // the cell in the first book (1-based; 0 when the record or column is only in B)
    int rowB = 0, colB = 0;    // the cell in the second book
    std::string before, after; // the cell texts ("" = empty)
    bool numeric = false;      // both are numbers
    double ratio = 0;          // after / before, numbers only (0 when before is 0)
};

struct SheetDiff {
    std::string name;
    bool onlyInA = false, onlyInB = false;
    std::vector<std::string> rowsAdded, rowsRemoved;     // records only in B / only in A (keys)
    std::vector<int> rowsRemovedAt;                      // their rows in A
    std::vector<std::string> columnsAdded, columnsRemoved;
    std::vector<CellChange> changes;                     // records and columns in both
    int rowsChanged = 0;                                 // records with at least one changed cell
    bool Same() const {
        return !onlyInA && !onlyInB && rowsAdded.empty() && rowsRemoved.empty() && columnsAdded.empty() && columnsRemoved.empty() && changes.empty();
    }
};

struct BookDiff {
    std::vector<SheetDiff> sheets; // the sheets of A in their order, then those only in B
    size_t ChangedCells() const;
    bool Same() const;
};

BookDiff Compare(const dbmodel::Book& a, const dbmodel::Book& b);
std::vector<std::string> Summary(const BookDiff& d);
bool WriteCsv(const BookDiff& d, const std::string& path, std::string& err);

} // namespace dbdiff

int RunDbDiff(int argc, char* argv[]);
