// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The INI / REG editor's model (inireg_editor.cpp): a text file as lines, grouped in sections, with the key = value rules inireg.cpp applies when it makes a .reg
// (a key repeated in a section becomes an array; an integer, a float or a string by the shape of its value). Header only, tested by tests/inireg_model_test.cpp.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace inimodel {

enum class LineKind { Blank, Section, KeyValue, Other };

inline std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The text as lines (a trailing newline does not make an empty last line; \r\n and \n both end a line).
inline std::vector<std::string> Split(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        size_t stop = end;
        if (stop > start && text[stop - 1] == '\r') --stop;
        lines.push_back(text.substr(start, stop - start));
        start = end + 1;
    }
    return lines;
}
inline std::string Join(const std::vector<std::string>& lines, bool crlf) {
    std::string out;
    for (const std::string& l : lines) { out += l; out += crlf ? "\r\n" : "\n"; }
    return out;
}
inline bool UsesCrlf(const std::string& text) { return text.find("\r\n") != std::string::npos; }

inline LineKind Kind(const std::string& line) {
    const std::string t = Trim(line);
    if (t.empty()) return LineKind::Blank;
    if (t.front() == '[' && t.back() == ']') return LineKind::Section;
    if (t[0] == '#') return LineKind::Other;                 // inireg.cpp skips these lines: comments
    if (t.find('=') != std::string::npos) return LineKind::KeyValue; // also "; Key=value": a key of the .reg whose name starts with ';'
    return LineKind::Other;
}
inline std::string SectionName(const std::string& line) { const std::string t = Trim(line); return Trim(t.substr(1, t.size() - 2)); }
inline void SplitKeyValue(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    key = Trim(line.substr(0, eq));
    value = eq == std::string::npos ? "" : Trim(line.substr(eq + 1));
}
inline std::string MakeKeyValue(const std::string& key, const std::string& value) { return key + "=" + value; }

// A section: the lines [first, last) of the file; `header` is the line of its [name] (-1 for what comes before the first one).
struct Section { std::string name; int header = -1; int first = 0, last = 0; int keys = 0; };

inline std::vector<Section> Sections(const std::vector<std::string>& lines) {
    std::vector<Section> out;
    Section cur; cur.name = ""; cur.header = -1; cur.first = 0;
    bool any = false;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const LineKind k = Kind(lines[static_cast<size_t>(i)]);
        if (k == LineKind::Section) {
            if (any || cur.header >= 0 || i > 0) { cur.last = i; if (cur.header >= 0 || any) out.push_back(cur); }
            cur = Section(); cur.name = SectionName(lines[static_cast<size_t>(i)]); cur.header = i; cur.first = i + 1; any = true;
        } else if (k == LineKind::KeyValue) ++cur.keys;
    }
    cur.last = static_cast<int>(lines.size());
    if (any || cur.keys > 0) out.push_back(cur);
    // the lines before the first header: they only count as a section when something in them is a key
    if (!out.empty() && out[0].header < 0 && out[0].keys == 0) out.erase(out.begin());
    return out;
}

// What the value becomes in a .reg (inireg.cpp's rules): int, float or text.
enum class ValueType { Int, Float, Text };
inline ValueType Classify(const std::string& v) {
    if (v.empty()) return ValueType::Text;
    size_t i = (v[0] == '+' || v[0] == '-') ? 1 : 0;
    if (i < v.size() && std::all_of(v.begin() + static_cast<long>(i), v.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return ValueType::Int;
    if (v.find('.') != std::string::npos || v.find('e') != std::string::npos || v.find('E') != std::string::npos) {
        char* end = nullptr;
        std::strtof(v.c_str(), &end);
        if (end && *end == '\0' && end != v.c_str()) return ValueType::Float;
    }
    return ValueType::Text;
}
inline const char* TypeName(ValueType t) { return t == ValueType::Int ? "int" : t == ValueType::Float ? "float" : "text"; }

// The lines of a section holding `key` (case as written), for the "repeated key = array" note.
inline int CountKey(const std::vector<std::string>& lines, const Section& s, const std::string& key) {
    int n = 0;
    for (int i = s.first; i < s.last; ++i) {
        if (Kind(lines[static_cast<size_t>(i)]) != LineKind::KeyValue) continue;
        std::string k, v; SplitKeyValue(lines[static_cast<size_t>(i)], k, v);
        if (k == key) ++n;
    }
    return n;
}

// New lines: a key at the end of a section (before its trailing blank lines), a section at the end of the file.
inline int InsertKey(std::vector<std::string>& lines, const Section& s, const std::string& key, const std::string& value) {
    int at = s.last;
    while (at > s.first && Kind(lines[static_cast<size_t>(at - 1)]) == LineKind::Blank) --at;
    lines.insert(lines.begin() + at, MakeKeyValue(key, value));
    return at;
}
inline void AddSection(std::vector<std::string>& lines, const std::string& name) {
    if (!lines.empty() && Kind(lines.back()) != LineKind::Blank) lines.push_back("");
    lines.push_back("[" + name + "]");
}

} // namespace inimodel
