// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
/**
 * sheet_io.hpp - Writing simple spreadsheets as .xlsx (Office Open XML) or .ods (OpenDocument)
 *
 * Purpose-built for the gameplay databases (dbexport.cpp): sheets of plain text and number cells, a
 * bold title row, column widths, and the rows above the data frozen. No formulas, merged cells or
 * styles beyond that. The files are zip archives whose entries are stored uncompressed (both formats
 * allow it), written without any external library. Reading them back is xlsx_reader.hpp's job (it
 * reads both formats).
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sheetio {

// ----------------------------------------------------------------------------
// The spreadsheet model
// ----------------------------------------------------------------------------

struct Cell {
    bool isNumber = false;
    std::string text; // a number's text as written (e.g. "1.5"), or the string
    bool bold = false;
};

struct Sheet {
    std::string name;
    std::map<std::pair<int, int>, Cell> cells; // (row, column), both 1-based
    std::map<int, int> widthsPx;               // column -> width in pixels (absent: default)
    int frozenRows = 0;                        // rows kept in view at the top
    int maxRow = 0, maxCol = 0;

    void Set(int row, int col, Cell c) {
        cells[{row, col}] = std::move(c);
        if (row > maxRow) maxRow = row;
        if (col > maxCol) maxCol = col;
    }
    void SetText(int row, int col, const std::string& s, bool bold = false) {
        Cell c;
        c.text = s;
        c.bold = bold;
        Set(row, col, std::move(c));
    }
    void SetNumber(int row, int col, const std::string& numberText) {
        Cell c;
        c.isNumber = true;
        c.text = numberText;
        Set(row, col, std::move(c));
    }
};

// ----------------------------------------------------------------------------
// Zip writer (stored entries)
// ----------------------------------------------------------------------------

inline uint32_t Crc32(const std::string& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

class ZipWriter {
public:
    void Add(const std::string& name, const std::string& data) { entries_.push_back({name, data}); }

    std::string Build() const {
        std::string out, central;
        // A fixed date (1 January 2020, 00:00): the same content gives the same file.
        const uint16_t dosTime = 0, dosDate = static_cast<uint16_t>(((2020 - 1980) << 9) | (1 << 5) | 1);
        for (const auto& e : entries_) {
            const uint32_t offset = static_cast<uint32_t>(out.size());
            const uint32_t crc = Crc32(e.second), size = static_cast<uint32_t>(e.second.size());
            Put32(out, 0x04034B50); Put16(out, 20); Put16(out, 0); Put16(out, 0); // stored
            Put16(out, dosTime); Put16(out, dosDate); Put32(out, crc); Put32(out, size); Put32(out, size);
            Put16(out, static_cast<uint16_t>(e.first.size())); Put16(out, 0);
            out += e.first;
            out += e.second;
            Put32(central, 0x02014B50); Put16(central, 20); Put16(central, 20); Put16(central, 0); Put16(central, 0);
            Put16(central, dosTime); Put16(central, dosDate); Put32(central, crc); Put32(central, size); Put32(central, size);
            Put16(central, static_cast<uint16_t>(e.first.size())); Put16(central, 0); Put16(central, 0); Put16(central, 0);
            Put16(central, 0); Put32(central, 0); Put32(central, offset);
            central += e.first;
        }
        const uint32_t centralOffset = static_cast<uint32_t>(out.size());
        out += central;
        Put32(out, 0x06054B50); Put16(out, 0); Put16(out, 0);
        Put16(out, static_cast<uint16_t>(entries_.size())); Put16(out, static_cast<uint16_t>(entries_.size()));
        Put32(out, static_cast<uint32_t>(central.size())); Put32(out, centralOffset); Put16(out, 0);
        return out;
    }

private:
    static void Put16(std::string& s, uint16_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>(v >> 8); }
    static void Put32(std::string& s, uint32_t v) { Put16(s, static_cast<uint16_t>(v & 0xFFFF)); Put16(s, static_cast<uint16_t>(v >> 16)); }
    std::vector<std::pair<std::string, std::string>> entries_;
};

inline bool WriteFile(const std::string& path, const std::string& data, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write " + path; return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) { err = "writing " + path + " failed"; return false; }
    return true;
}

// ----------------------------------------------------------------------------
// XML helpers
// ----------------------------------------------------------------------------

inline std::string XmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch;
        }
    }
    return out;
}

// "A", "B", ... "Z", "AA"...
inline std::string ColumnName(int col) {
    std::string s;
    while (col > 0) {
        const int r = (col - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + r));
        col = (col - 1) / 26;
    }
    return s;
}

// ----------------------------------------------------------------------------
// .xlsx
// ----------------------------------------------------------------------------

inline std::string BuildXlsx(const std::vector<Sheet>& sheets) {
    const char* header = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    ZipWriter zip;
    std::string types = std::string(header) +
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
    for (size_t i = 0; i < sheets.size(); ++i)
        types += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) +
                 ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    types += "</Types>";
    zip.Add("[Content_Types].xml", types);
    zip.Add("_rels/.rels", std::string(header) +
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>");

    std::string workbook = std::string(header) +
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    std::string rels = std::string(header) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        workbook += "<sheet name=\"" + XmlEscape(sheets[i].name) + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
        rels += "<Relationship Id=\"rId" + n + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
                "Target=\"worksheets/sheet" + n + ".xml\"/>";
    }
    workbook += "</sheets></workbook>";
    rels += "<Relationship Id=\"rId" + std::to_string(sheets.size() + 1) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";
    zip.Add("xl/workbook.xml", workbook);
    zip.Add("xl/_rels/workbook.xml.rels", rels);
    // Style 0: normal; style 1: bold (the titles).
    zip.Add("xl/styles.xml", std::string(header) +
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"2\"><font><sz val=\"10\"/><name val=\"Arial\"/></font><font><b/><sz val=\"10\"/><name val=\"Arial\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>");

    for (size_t i = 0; i < sheets.size(); ++i) {
        const Sheet& sh = sheets[i];
        std::string x = std::string(header) + "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
        if (sh.frozenRows > 0)
            x += "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"" + std::to_string(sh.frozenRows) + "\" topLeftCell=\"A" +
                 std::to_string(sh.frozenRows + 1) + "\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>";
        if (!sh.widthsPx.empty()) {
            x += "<cols>";
            for (const auto& [col, px] : sh.widthsPx) {
                char w[32];
                std::snprintf(w, sizeof(w), "%.2f", px / 7.0 + 0.71); // pixels -> Excel's character widths
                x += "<col min=\"" + std::to_string(col) + "\" max=\"" + std::to_string(col) + "\" width=\"" + w + "\" customWidth=\"1\"/>";
            }
            x += "</cols>";
        }
        x += "<sheetData>";
        int row = 0;
        for (const auto& [rc, cell] : sh.cells) {
            if (rc.first != row) {
                if (row) x += "</row>";
                row = rc.first;
                x += "<row r=\"" + std::to_string(row) + "\">";
            }
            const std::string ref = ColumnName(rc.second) + std::to_string(rc.first);
            const std::string style = cell.bold ? " s=\"1\"" : "";
            if (cell.isNumber) {
                x += "<c r=\"" + ref + "\"" + style + "><v>" + cell.text + "</v></c>";
            } else {
                const bool keepSpaces = !cell.text.empty() && (cell.text.front() == ' ' || cell.text.back() == ' ');
                x += "<c r=\"" + ref + "\"" + style + " t=\"inlineStr\"><is><t" + (keepSpaces ? " xml:space=\"preserve\"" : "") + ">" +
                     XmlEscape(cell.text) + "</t></is></c>";
            }
        }
        if (row) x += "</row>";
        x += "</sheetData></worksheet>";
        zip.Add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", x);
    }
    return zip.Build();
}

// ----------------------------------------------------------------------------
// .ods
// ----------------------------------------------------------------------------

// Text for a <text:p>: runs of spaces, tabs and line breaks as OpenDocument writes them (a plain
// space run would be collapsed by readers).
inline std::string OdsText(const std::string& s) {
    std::string out = "<text:p>";
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ' ') {
            size_t n = 0;
            while (i + n < s.size() && s[i + n] == ' ') ++n;
            // One space between words stays a space; leading, trailing and repeated ones are <text:s/>.
            if (n == 1 && i > 0 && i + 1 < s.size()) out += ' ';
            else out += n == 1 ? "<text:s/>" : "<text:s text:c=\"" + std::to_string(n) + "\"/>";
            i += n;
        } else if (s[i] == '\t') {
            out += "<text:tab/>";
            ++i;
        } else if (s[i] == '\n') {
            out += "</text:p><text:p>";
            ++i;
        } else if (s[i] == '\r') {
            ++i; // CR LF -> one paragraph break
        } else {
            const size_t start = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') ++i;
            out += XmlEscape(s.substr(start, i - start));
        }
    }
    return out + "</text:p>";
}

inline std::string BuildOds(const std::vector<Sheet>& sheets) {
    ZipWriter zip;
    zip.Add("mimetype", "application/vnd.oasis.opendocument.spreadsheet"); // first, stored: the format requires it
    zip.Add("META-INF/manifest.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"settings.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>");

    // Column styles: one per distinct width.
    std::map<int, std::string> widthStyle;
    for (const Sheet& sh : sheets)
        for (const auto& [col, px] : sh.widthsPx) widthStyle.emplace(px, "");
    std::string styles;
    int n = 0;
    for (auto& [px, name] : widthStyle) {
        name = "co" + std::to_string(++n);
        char cm[32];
        std::snprintf(cm, sizeof(cm), "%.3fcm", px * 2.54 / 96.0);
        styles += "<style:style style:name=\"" + name + "\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"" +
                  cm + "\"/></style:style>";
    }
    styles += "<style:style style:name=\"ceBold\" style:family=\"table-cell\"><style:text-properties fo:font-weight=\"bold\"/></style:style>";

    std::string x =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
        "office:version=\"1.2\"><office:automatic-styles>" + styles + "</office:automatic-styles><office:body><office:spreadsheet>";
    for (const Sheet& sh : sheets) {
        x += "<table:table table:name=\"" + XmlEscape(sh.name) + "\">";
        for (int c = 1; c <= sh.maxCol; ++c) {
            auto w = sh.widthsPx.find(c);
            x += w != sh.widthsPx.end() ? "<table:table-column table:style-name=\"" + widthStyle[w->second] + "\"/>" : "<table:table-column/>";
        }
        auto it = sh.cells.begin();
        for (int r = 1; r <= sh.maxRow; ++r) {
            x += "<table:table-row>";
            int col = 1;
            for (; it != sh.cells.end() && it->first.first == r; ++it) {
                const int c = it->first.second;
                if (c > col) {
                    x += c - col == 1 ? "<table:table-cell/>" : "<table:table-cell table:number-columns-repeated=\"" + std::to_string(c - col) + "\"/>";
                }
                const Cell& cell = it->second;
                const std::string style = cell.bold ? " table:style-name=\"ceBold\"" : "";
                if (cell.isNumber)
                    x += "<table:table-cell" + style + " office:value-type=\"float\" office:value=\"" + cell.text + "\"><text:p>" + cell.text +
                         "</text:p></table:table-cell>";
                else
                    x += "<table:table-cell" + style + " office:value-type=\"string\">" + OdsText(cell.text) + "</table:table-cell>";
                col = c + 1;
            }
            if (col == 1) x += "<table:table-cell/>"; // an empty row still needs a cell
            x += "</table:table-row>";
        }
        x += "</table:table>";
    }
    x += "</office:spreadsheet></office:body></office:document-content>";
    zip.Add("content.xml", x);

    // The rows above the data stay in view (LibreOffice's frozen panes, per sheet).
    std::string settings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-settings xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\" office:version=\"1.2\"><office:settings>"
        "<config:config-item-set config:name=\"ooo:view-settings\"><config:config-item-map-indexed config:name=\"Views\">"
        "<config:config-item-map-entry><config:config-item-map-named config:name=\"Tables\">";
    for (const Sheet& sh : sheets) {
        if (sh.frozenRows <= 0) continue;
        const std::string f = std::to_string(sh.frozenRows);
        settings += "<config:config-item-map-entry config:name=\"" + XmlEscape(sh.name) + "\">"
                    "<config:config-item config:name=\"VerticalSplitMode\" config:type=\"short\">2</config:config-item>"
                    "<config:config-item config:name=\"VerticalSplitPosition\" config:type=\"int\">" + f + "</config:config-item>"
                    "<config:config-item config:name=\"ActiveSplitRange\" config:type=\"short\">2</config:config-item>"
                    "<config:config-item config:name=\"PositionBottom\" config:type=\"int\">" + f + "</config:config-item>"
                    "</config:config-item-map-entry>";
    }
    settings += "</config:config-item-map-named></config:config-item-map-entry></config:config-item-map-indexed></config:config-item-set>"
                "</office:settings></office:document-settings>";
    zip.Add("settings.xml", settings);
    return zip.Build();
}

} // namespace sheetio
