// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// A small INI reader / writer for the launcher: launcher.cfg, starter.config (EIStarter's), a mod's mod.config and the lang.ini
// translation files. Reads UTF-8 (with or without BOM), UTF-16 LE / BE (with BOM, as lang.ini is) and plain ANSI bytes (kept as they are).
// Sections and keys are case-insensitive; the file order is kept; `;` and `#` start a comment line.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace launcher {

inline std::string LowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// The text of a file as UTF-8 ("" when it cannot be read). UTF-16 (BOM) is converted, a UTF-8 BOM is dropped.
inline std::string ReadTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u8 = [&](size_t i) { return static_cast<uint8_t>(raw[i]); };
    if (raw.size() >= 2 && ((u8(0) == 0xFF && u8(1) == 0xFE) || (u8(0) == 0xFE && u8(1) == 0xFF))) {
        const bool le = u8(0) == 0xFF;
        std::string out;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            uint32_t cu = le ? (u8(i) | (u8(i + 1) << 8)) : ((u8(i) << 8) | u8(i + 1));
            if (cu >= 0xD800 && cu < 0xDC00 && i + 3 < raw.size()) {
                uint32_t lo = le ? (u8(i + 2) | (u8(i + 3) << 8)) : ((u8(i + 2) << 8) | u8(i + 3));
                if (lo >= 0xDC00 && lo < 0xE000) { cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            AppendUtf8(out, cu);
        }
        return out;
    }
    if (raw.size() >= 3 && u8(0) == 0xEF && u8(1) == 0xBB && u8(2) == 0xBF) raw.erase(0, 3);
    return raw;
}

class IniFile {
public:
    struct Entry { std::string key, value; };
    struct Section { std::string name; std::vector<Entry> entries; };

    bool Load(const std::string& path) {
        sections_.clear();
        const std::string text = ReadTextFile(path);
        if (text.empty()) { std::ifstream probe(path); loaded_ = probe.good(); return loaded_; }
        loaded_ = true;
        Section* cur = nullptr;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line.erase(0, b);
            if (line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                const size_t e = line.find(']');
                if (e == std::string::npos) continue;
                sections_.push_back({line.substr(1, e - 1), {}});
                cur = &sections_.back();
                continue;
            }
            if (!cur) { sections_.push_back({"", {}}); cur = &sections_.back(); }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            cur->entries.push_back({k, v});
        }
        return true;
    }
    bool Loaded() const { return loaded_; }

    bool Has(const std::string& section, const std::string& key) const { return Find(section, key) != nullptr; }
    std::string Get(const std::string& section, const std::string& key, const std::string& def = {}) const {
        const Entry* e = Find(section, key);
        return e ? e->value : def;
    }
    bool GetBool(const std::string& section, const std::string& key, bool def) const {
        const Entry* e = Find(section, key);
        if (!e) return def;
        const std::string v = LowerAscii(e->value);
        if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
        if (v == "0" || v == "false" || v == "no" || v == "off") return false;
        return def;
    }
    int GetInt(const std::string& section, const std::string& key, int def) const {
        const Entry* e = Find(section, key);
        if (!e || e->value.empty()) return def;
        char* end = nullptr;
        const long v = std::strtol(e->value.c_str(), &end, 10);
        return end == e->value.c_str() ? def : static_cast<int>(v);
    }
    float GetFloat(const std::string& section, const std::string& key, float def) const {
        const Entry* e = Find(section, key);
        if (!e || e->value.empty()) return def;
        char* end = nullptr;
        const float v = std::strtof(e->value.c_str(), &end);
        return end == e->value.c_str() ? def : v;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value) {
        Section* s = FindSection(section);
        if (!s) { sections_.push_back({section, {}}); s = &sections_.back(); }
        for (Entry& e : s->entries) if (LowerAscii(e.key) == LowerAscii(key)) { e.value = value; return; }
        s->entries.push_back({key, value});
    }
    void SetBool(const std::string& section, const std::string& key, bool v) { Set(section, key, v ? "true" : "false"); }
    void SetInt(const std::string& section, const std::string& key, int v) { Set(section, key, std::to_string(v)); }
    void SetFloat(const std::string& section, const std::string& key, float v) { char b[48]; std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v)); Set(section, key, b); }

    const std::vector<Section>& Sections() const { return sections_; }

    // Writes UTF-8. `header` lines (without ';') go on top as comments.
    bool Save(const std::string& path, const std::vector<std::string>& header = {}) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const std::string& h : header) out << "; " << h << "\r\n";
        if (!header.empty()) out << "\r\n";
        for (const Section& s : sections_) {
            if (!s.name.empty()) out << "[" << s.name << "]\r\n";
            for (const Entry& e : s.entries) out << e.key << "=" << e.value << "\r\n";
            out << "\r\n";
        }
        return out.good();
    }

private:
    const Section* FindSection(const std::string& name) const {
        const std::string l = LowerAscii(name);
        for (const Section& s : sections_) if (LowerAscii(s.name) == l) return &s;
        return nullptr;
    }
    Section* FindSection(const std::string& name) { return const_cast<Section*>(static_cast<const IniFile*>(this)->FindSection(name)); }
    const Entry* Find(const std::string& section, const std::string& key) const {
        const Section* s = FindSection(section);
        if (!s) return nullptr;
        const std::string l = LowerAscii(key);
        for (const Entry& e : s->entries) if (LowerAscii(e.key) == l) return &e;
        return nullptr;
    }
    std::vector<Section> sections_;
    bool loaded_ = false;
};

// Sets `KEY=value` in a mod's plain cfg file (um.cfg ...) in place, keeping its comments and order; false when the key is not there.
inline bool SetCfgLine(const std::string& path, const std::string& key, const std::string& value) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string lk = LowerAscii(key);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        size_t b = pos;
        while (b < eol && (text[b] == ' ' || text[b] == '\t')) ++b;
        if (b < eol && text[b] != ';' && text[b] != '#') {
            size_t eq = text.find('=', b);
            if (eq != std::string::npos && eq < eol) {
                std::string k = text.substr(b, eq - b);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                if (LowerAscii(k) == lk) {
                    size_t end = eol;
                    if (end > eq && text[end - 1] == '\r') --end;
                    text.replace(eq + 1, end - eq - 1, value);
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out << text;
                    return out.good();
                }
            }
        }
        pos = eol + 1;
    }
    return false;
}

} // namespace launcher
