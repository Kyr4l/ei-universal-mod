/**
 * ============================================================================
 * xlsx_reader.hpp - Minimal .xlsx (OOXML spreadsheet) cell reader
 * ============================================================================
 *
 * Purpose-built reader for extracting cell values from the specific .xlsx
 * files this project's XLSX databases come as (LibreOffice/DBEditor output):
 * shared strings, numeric literals, and inline strings. Not a general OOXML
 * parser - no styles, formulas, merged cells, or rich-text runs beyond plain
 * concatenated <t> text. No external XML/zip libraries.
 * ============================================================================
 */

#pragma once

#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "zip_reader.hpp"

namespace xlsxlib {

// ----------------------------------------------------------------------------
// Tiny XML text utilities (entity decoding + tag/attribute scanning)
// ----------------------------------------------------------------------------

inline std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out += '&'; i = semi + 1; continue; }
                if (ent == "lt") { out += '<'; i = semi + 1; continue; }
                if (ent == "gt") { out += '>'; i = semi + 1; continue; }
                if (ent == "quot") { out += '"'; i = semi + 1; continue; }
                if (ent == "apos") { out += '\''; i = semi + 1; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    long cp = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        cp = strtol(ent.c_str() + 2, nullptr, 16);
                    } else {
                        cp = strtol(ent.c_str() + 1, nullptr, 10);
                    }
                    // Codepoints here are Windows-1251-mapped text re-encoded as UTF-8 by the XML
                    // writer only for the XML layer itself; our data is plain ASCII/Cyrillic-1251
                    // taken from <t> text directly (not numeric refs) in practice, but support the
                    // common single-byte case defensively.
                    if (cp >= 0 && cp < 256) { out += static_cast<char>(cp); i = semi + 1; continue; }
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// Finds the value of attribute `attr` within a tag's raw attribute text `tag`.
inline std::optional<std::string> FindAttr(const std::string& tag, const std::string& attr) {
    std::string needle = attr + "=\"";
    size_t pos = tag.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return std::nullopt;
    return tag.substr(pos, end - pos);
}

// Extracts the concatenation of all <t>...</t> runs within `xml` (used for <si> shared-string
// entries and <is> inline strings, both of which may contain multiple <r><t>...</t></r> runs).
inline std::string ExtractText(const std::string& xml) {
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t open = xml.find("<t", pos);
        if (open == std::string::npos) break;
        size_t tagEnd = xml.find('>', open);
        if (tagEnd == std::string::npos) break;
        if (xml[tagEnd - 1] == '/') { pos = tagEnd + 1; continue; } // self-closing <t/>
        size_t close = xml.find("</t>", tagEnd);
        if (close == std::string::npos) break;
        out += DecodeEntities(xml.substr(tagEnd + 1, close - tagEnd - 1));
        pos = close + 4;
    }
    return out;
}

// Splits a cell reference like "AB123" into (colIndex 1-based, rowIndex 1-based).
inline void ParseCellRef(const std::string& ref, int& col, int& row) {
    size_t i = 0;
    col = 0;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) {
        col = col * 26 + (std::toupper(static_cast<unsigned char>(ref[i])) - 'A' + 1);
        ++i;
    }
    row = (i < ref.size()) ? std::atoi(ref.c_str() + i) : 0;
}

// ----------------------------------------------------------------------------
// Cell value
// ----------------------------------------------------------------------------

struct CellValue {
    bool isString = false;
    bool present = false;
    std::string strVal;
    double numVal = 0.0;

    bool IsEmpty() const { return !present; }
    std::string AsString() const { return isString ? strVal : (present ? strVal : std::string()); }
    double AsNumber() const { return numVal; }
};

// ----------------------------------------------------------------------------
// Sheet: dense-ish row/col -> value map, parsed once from sheetN.xml
// ----------------------------------------------------------------------------

class Sheet {
public:
    void ParseFrom(const std::string& xml, const std::vector<std::string>& sharedStrings) {
        size_t pos = 0;
        while (true) {
            size_t rowOpen = xml.find("<row", pos);
            if (rowOpen == std::string::npos) break;
            size_t rowTagEnd = xml.find('>', rowOpen);
            if (rowTagEnd == std::string::npos) break;
            bool selfClosingRow = xml[rowTagEnd - 1] == '/';
            size_t rowContentStart = rowTagEnd + 1;
            size_t rowClose;
            if (selfClosingRow) {
                rowClose = rowContentStart;
            } else {
                rowClose = xml.find("</row>", rowContentStart);
                if (rowClose == std::string::npos) break;
            }

            std::string rowTag = xml.substr(rowOpen, rowTagEnd - rowOpen);
            auto rAttr = FindAttr(rowTag, "r");
            int rowNum = rAttr ? std::atoi(rAttr->c_str()) : 0;

            if (!selfClosingRow) {
                ParseRowCells(xml.substr(rowContentStart, rowClose - rowContentStart), rowNum, sharedStrings);
                maxRow_ = std::max(maxRow_, rowNum);
            }
            pos = rowClose + (selfClosingRow ? 0 : 6);
        }
    }

    CellValue Get(int row, int col) const {
        auto it = cells_.find({row, col});
        if (it == cells_.end()) return CellValue{};
        return it->second;
    }

    int MaxRow() const { return maxRow_; }
    int MaxColumn() const { return maxCol_; }

private:
    void ParseRowCells(const std::string& rowXml, int rowNum, const std::vector<std::string>& sharedStrings) {
        size_t pos = 0;
        while (true) {
            size_t cOpen = rowXml.find("<c", pos);
            if (cOpen == std::string::npos) break;
            if (cOpen + 2 < rowXml.size() && !std::isspace(static_cast<unsigned char>(rowXml[cOpen + 2])) && rowXml[cOpen + 2] != '>' && rowXml[cOpen + 2] != '/') {
                // e.g. "<color" false match guard; require '<c ' or '<c>' or '<c/>'
                pos = cOpen + 2;
                continue;
            }
            size_t cTagEnd = rowXml.find('>', cOpen);
            if (cTagEnd == std::string::npos) break;
            bool selfClosing = rowXml[cTagEnd - 1] == '/';
            std::string cTag = rowXml.substr(cOpen, cTagEnd - cOpen);

            std::string cellXml;
            size_t next;
            if (selfClosing) {
                next = cTagEnd + 1;
            } else {
                size_t cClose = rowXml.find("</c>", cTagEnd);
                if (cClose == std::string::npos) break;
                cellXml = rowXml.substr(cTagEnd + 1, cClose - cTagEnd - 1);
                next = cClose + 4;
            }

            auto refAttr = FindAttr(cTag, "r");
            if (refAttr) {
                int col, r;
                ParseCellRef(*refAttr, col, r);
                if (r == 0) r = rowNum;
                CellValue cv;
                cv.present = true;

                auto typeAttr = FindAttr(cTag, "t");
                std::string type = typeAttr ? *typeAttr : std::string("n");

                if (type == "s") {
                    size_t vOpen = cellXml.find("<v>");
                    if (vOpen != std::string::npos) {
                        size_t vClose = cellXml.find("</v>", vOpen);
                        int idx = std::atoi(cellXml.substr(vOpen + 3, vClose - vOpen - 3).c_str());
                        cv.isString = true;
                        cv.strVal = (idx >= 0 && idx < static_cast<int>(sharedStrings.size())) ? sharedStrings[idx] : std::string();
                    } else {
                        cv.present = false;
                    }
                } else if (type == "str" || type == "inlineStr") {
                    cv.isString = true;
                    cv.strVal = ExtractText(cellXml);
                } else if (type == "b") {
                    size_t vOpen = cellXml.find("<v>");
                    if (vOpen != std::string::npos) {
                        size_t vClose = cellXml.find("</v>", vOpen);
                        cv.numVal = std::atof(cellXml.substr(vOpen + 3, vClose - vOpen - 3).c_str());
                    }
                } else {
                    // numeric ("n") or untyped
                    size_t vOpen = cellXml.find("<v>");
                    if (vOpen != std::string::npos) {
                        size_t vClose = cellXml.find("</v>", vOpen);
                        cv.numVal = std::atof(cellXml.substr(vOpen + 3, vClose - vOpen - 3).c_str());
                    } else {
                        cv.present = false;
                    }
                }

                if (cv.present) {
                    cells_[{r, col}] = cv;
                    maxCol_ = std::max(maxCol_, col);
                    maxRow_ = std::max(maxRow_, r);
                }
            }
            pos = next;
        }
    }

    std::map<std::pair<int, int>, CellValue> cells_;
    int maxRow_ = 0;
    int maxCol_ = 0;
};

// ----------------------------------------------------------------------------
// Workbook: opens the .xlsx zip, resolves sheet name -> sheetN.xml via
// workbook.xml + workbook.xml.rels, loads shared strings once.
// ----------------------------------------------------------------------------

class Workbook {
public:
    explicit Workbook(const std::string& path) : zip_(path) {
        LoadSharedStrings();
        LoadSheetList();
    }

    bool HasSheet(const std::string& name) const { return sheetPaths_.count(name) != 0; }

    const Sheet& GetSheet(const std::string& name) {
        auto it = loadedSheets_.find(name);
        if (it != loadedSheets_.end()) return it->second;

        auto pathIt = sheetPaths_.find(name);
        if (pathIt == sheetPaths_.end()) throw std::runtime_error("xlsx: sheet not found: " + name);

        std::string xml = zip_.ReadString("xl/" + pathIt->second);
        Sheet sheet;
        sheet.ParseFrom(xml, sharedStrings_);
        auto [ins, ok] = loadedSheets_.emplace(name, std::move(sheet));
        return ins->second;
    }

private:
    void LoadSharedStrings() {
        if (!zip_.Has("xl/sharedStrings.xml")) return;
        std::string xml = zip_.ReadString("xl/sharedStrings.xml");
        size_t pos = 0;
        while (true) {
            size_t open = xml.find("<si>", pos);
            size_t openSelf = xml.find("<si/>", pos);
            if (open == std::string::npos && openSelf == std::string::npos) break;
            if (openSelf != std::string::npos && (open == std::string::npos || openSelf < open)) {
                sharedStrings_.push_back("");
                pos = openSelf + 5;
                continue;
            }
            size_t close = xml.find("</si>", open);
            if (close == std::string::npos) break;
            sharedStrings_.push_back(ExtractText(xml.substr(open + 4, close - open - 4)));
            pos = close + 5;
        }
    }

    void LoadSheetList() {
        std::string relsXml = zip_.Has("xl/_rels/workbook.xml.rels") ? zip_.ReadString("xl/_rels/workbook.xml.rels") : "";
        std::map<std::string, std::string> ridToTarget;
        {
            size_t pos = 0;
            while (true) {
                size_t open = relsXml.find("<Relationship", pos);
                if (open == std::string::npos) break;
                size_t tagEnd = relsXml.find('>', open);
                std::string tag = relsXml.substr(open, tagEnd - open);
                auto id = FindAttr(tag, "Id");
                auto target = FindAttr(tag, "Target");
                if (id && target) ridToTarget[*id] = *target;
                pos = tagEnd + 1;
            }
        }

        std::string wbXml = zip_.ReadString("xl/workbook.xml");
        size_t pos = 0;
        while (true) {
            size_t open = wbXml.find("<sheet ", pos);
            if (open == std::string::npos) break;
            size_t tagEnd = wbXml.find('>', open);
            std::string tag = wbXml.substr(open, tagEnd - open);
            auto name = FindAttr(tag, "name");
            auto rid = FindAttr(tag, "r:id");
            if (name && rid) {
                auto targetIt = ridToTarget.find(*rid);
                if (targetIt != ridToTarget.end()) {
                    std::string target = targetIt->second;
                    if (target.rfind("/xl/", 0) == 0) target = target.substr(4);
                    sheetPaths_[*name] = target;
                }
            }
            pos = tagEnd + 1;
        }
    }

    ziplib::ZipArchive zip_;
    std::vector<std::string> sharedStrings_;
    std::map<std::string, std::string> sheetPaths_;
    std::map<std::string, Sheet> loadedSheets_;
};

} // namespace xlsxlib
