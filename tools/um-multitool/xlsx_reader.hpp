/**
 * ============================================================================
 * xlsx_reader.hpp - Minimal .xlsx (OOXML) and .ods (OpenDocument) spreadsheet cell reader
 * ============================================================================
 *
 * Purpose-built reader for extracting cell values from the specific .xlsx
 * files this project's XLSX databases come as (LibreOffice/DBEditor output):
 * shared strings, numeric literals, and inline strings. Not a general OOXML
 * parser - no styles, formulas, merged cells, or rich-text runs beyond plain
 * concatenated <t> text. No external XML/zip libraries.
 *
 * An OpenDocument spreadsheet (.ods, e.g. saved by LibreOffice) is read into the same cells: its
 * content.xml tables, with repeated rows and cells expanded, numbers from office:value and text from
 * the <text:p> paragraphs (joined by line breaks; <text:s/>, <text:tab/> and <text:line-break/> restored).
 * ============================================================================
 */

#pragma once

#include <cctype>
#include <map>
#include <memory>
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

    const std::map<std::pair<int, int>, CellValue>& Cells() const { return cells_; } // (row, col) -> value

    void Set(int row, int col, const CellValue& v) {
        cells_[{row, col}] = v;
        maxRow_ = std::max(maxRow_, row);
        maxCol_ = std::max(maxCol_, col);
    }

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
    explicit Workbook(const std::string& path) : zip_(std::make_unique<ziplib::ZipArchive>(path)) {
        if (zip_->Has("content.xml") && zip_->Has("mimetype") &&
            zip_->ReadString("mimetype").find("opendocument.spreadsheet") != std::string::npos) {
            LoadOds();
            return;
        }
        LoadSharedStrings();
        LoadSheetList();
    }

    // A workbook built in memory (the DB editor's sheets), with AddSheet.
    Workbook() = default;
    void AddSheet(const std::string& name, Sheet sheet) {
        if (!loadedSheets_.count(name)) order_.push_back(name);
        loadedSheets_[name] = std::move(sheet);
    }

    bool HasSheet(const std::string& name) const { return sheetPaths_.count(name) != 0 || loadedSheets_.count(name) != 0; }
    // The sheets' names, in the workbook's order.
    const std::vector<std::string>& SheetNames() const { return order_; }

    const Sheet& GetSheet(const std::string& name) {
        auto it = loadedSheets_.find(name);
        if (it != loadedSheets_.end()) return it->second;

        auto pathIt = sheetPaths_.find(name);
        if (pathIt == sheetPaths_.end()) throw std::runtime_error("xlsx: sheet not found: " + name);

        std::string xml = zip_->ReadString("xl/" + pathIt->second);
        Sheet sheet;
        sheet.ParseFrom(xml, sharedStrings_);
        auto [ins, ok] = loadedSheets_.emplace(name, std::move(sheet));
        return ins->second;
    }

private:
    void LoadSharedStrings() {
        if (!zip_->Has("xl/sharedStrings.xml")) return;
        std::string xml = zip_->ReadString("xl/sharedStrings.xml");
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
        std::string relsXml = zip_->Has("xl/_rels/workbook.xml.rels") ? zip_->ReadString("xl/_rels/workbook.xml.rels") : "";
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

        std::string wbXml = zip_->ReadString("xl/workbook.xml");
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
                    if (!sheetPaths_.count(*name)) order_.push_back(*name);
                    sheetPaths_[*name] = target;
                }
            }
            pos = tagEnd + 1;
        }
    }

    // ---- OpenDocument ----

    // The text of a cell: its <text:p> paragraphs joined by line breaks.
    static std::string OdsCellText(const std::string& x) {
        std::string out;
        bool firstParagraph = true;
        size_t pos = 0;
        while (true) {
            size_t p = x.find("<text:p", pos);
            if (p == std::string::npos) break;
            size_t tagEnd = x.find('>', p);
            if (tagEnd == std::string::npos) break;
            if (!firstParagraph) out += '\n';
            firstParagraph = false;
            if (x[tagEnd - 1] == '/') { pos = tagEnd + 1; continue; } // <text:p/>
            size_t close = x.find("</text:p>", tagEnd);
            if (close == std::string::npos) break;
            const std::string body = x.substr(tagEnd + 1, close - tagEnd - 1);
            for (size_t i = 0; i < body.size();) {
                if (body[i] != '<') {
                    size_t next = body.find('<', i);
                    if (next == std::string::npos) next = body.size();
                    out += DecodeEntities(body.substr(i, next - i));
                    i = next;
                    continue;
                }
                size_t end = body.find('>', i);
                if (end == std::string::npos) break;
                const std::string tag = body.substr(i, end - i + 1);
                if (tag.rfind("<text:s", 0) == 0 && (tag.size() == 8 || tag[7] == ' ' || tag[7] == '/')) {
                    auto c = FindAttr(tag, "text:c");
                    out.append(c ? static_cast<size_t>(std::max(1, std::atoi(c->c_str()))) : 1, ' ');
                } else if (tag.rfind("<text:tab", 0) == 0) {
                    out += '\t';
                } else if (tag.rfind("<text:line-break", 0) == 0) {
                    out += '\n';
                }
                i = end + 1; // other tags (spans, links...): their text is kept, the tags dropped
            }
            pos = close + 9;
        }
        return out;
    }

    // Every table of content.xml becomes a sheet (cells keep their row and column, from 1).
    void LoadOds() {
        const std::string xml = zip_->ReadString("content.xml");
        size_t pos = 0;
        while (true) {
            size_t t = xml.find("<table:table ", pos);
            if (t == std::string::npos) break;
            size_t tEnd = xml.find('>', t);
            size_t tClose = xml.find("</table:table>", tEnd);
            if (tEnd == std::string::npos || tClose == std::string::npos) break;
            auto name = FindAttr(xml.substr(t, tEnd - t), "table:name");
            Sheet sheet;
            int row = 1;
            size_t r = tEnd;
            while (true) {
                size_t rOpen = xml.find("<table:table-row", r);
                if (rOpen == std::string::npos || rOpen > tClose) break;
                size_t rTagEnd = xml.find('>', rOpen);
                const std::string rTag = xml.substr(rOpen, rTagEnd - rOpen);
                auto rRep = FindAttr(rTag, "table:number-rows-repeated");
                const int rowsRepeated = rRep ? std::max(1, std::atoi(rRep->c_str())) : 1;
                size_t rClose = xml[rTagEnd - 1] == '/' ? rTagEnd + 1 : xml.find("</table:table-row>", rTagEnd);
                const std::string rowXml = xml[rTagEnd - 1] == '/' ? std::string() : xml.substr(rTagEnd + 1, rClose - rTagEnd - 1);
                // The row's cells.
                std::vector<std::pair<int, CellValue>> cells; // column -> value
                int col = 1;
                size_t c = 0;
                while (true) {
                    size_t c1 = rowXml.find("<table:table-cell", c), c2 = rowXml.find("<table:covered-table-cell", c);
                    size_t cOpen = std::min(c1, c2);
                    if (cOpen == std::string::npos) break;
                    size_t cTagEnd = rowXml.find('>', cOpen);
                    const std::string cTag = rowXml.substr(cOpen, cTagEnd - cOpen);
                    const bool self = rowXml[cTagEnd - 1] == '/';
                    const std::string closeTag = cOpen == c2 ? "</table:covered-table-cell>" : "</table:table-cell>";
                    size_t cClose = self ? cTagEnd + 1 : rowXml.find(closeTag, cTagEnd);
                    const std::string inner = self ? std::string() : rowXml.substr(cTagEnd + 1, cClose - cTagEnd - 1);
                    auto cRep = FindAttr(cTag, "table:number-columns-repeated");
                    const int colsRepeated = cRep ? std::max(1, std::atoi(cRep->c_str())) : 1;
                    auto type = FindAttr(cTag, "office:value-type");
                    CellValue v;
                    if (type && (*type == "float" || *type == "percentage" || *type == "currency")) {
                        auto val = FindAttr(cTag, "office:value");
                        v.present = true;
                        v.numVal = val ? std::atof(val->c_str()) : 0.0;
                    } else if (type && *type == "boolean") {
                        auto val = FindAttr(cTag, "office:boolean-value");
                        v.present = true;
                        v.numVal = (val && *val == "true") ? 1.0 : 0.0;
                    } else if (type) { // string, date, time: their text
                        v.present = true;
                        v.isString = true;
                        auto sv = FindAttr(cTag, "office:string-value");
                        v.strVal = sv ? DecodeEntities(*sv) : OdsCellText(inner);
                    }
                    if (v.present) for (int k = 0; k < colsRepeated; ++k) cells.push_back({col + k, v});
                    col += colsRepeated;
                    c = self ? cTagEnd + 1 : cClose + closeTag.size();
                }
                if (!cells.empty())
                    for (int k = 0; k < rowsRepeated; ++k)
                        for (const auto& [cc, v] : cells) sheet.Set(row + k, cc, v);
                row += rowsRepeated;
                r = xml[rTagEnd - 1] == '/' ? rTagEnd + 1 : rClose + 18;
            }
            if (name) AddSheet(DecodeEntities(*name), std::move(sheet));
            pos = tClose + 14;
        }
    }

    std::unique_ptr<ziplib::ZipArchive> zip_;
    std::vector<std::string> sharedStrings_;
    std::vector<std::string> order_;
    std::map<std::string, std::string> sheetPaths_;
    std::map<std::string, Sheet> loadedSheets_;
};

} // namespace xlsxlib
