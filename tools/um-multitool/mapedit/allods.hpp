// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Allods: which zone belongs to which allod, read from the mod's config.ini / config.reg [MPZone] section:
//
//   [MPZone]
//   Gipath=zone1          the allod's name = one of its zones, repeated; ";" comments a zone out;
//   Gipath=zone2          an empty "Name=" line ends the allod's block; the file's order is the allods' order.
//   Gipath=
//   Ingos=zone11
//
// A quest's zone is the first word after "#res" in its map.txt (zone12, zone3xobr). The word after "#zone" in
// map.txt (gipat, ingos, suslanger, jigran, final) is only the lighting region (lights<region>.ini): not used here.
// New allods (a mod, SpellAddon) need no code: they are whatever the section lists.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace allods {

struct Allod {
    std::string name;
    std::vector<std::string> zones; // as written
};

struct Table {
    std::vector<Allod> allods;     // in the file's order (the campaign order)
    std::vector<std::string> commented; // zones listed behind a ";" (not part of any allod)
    std::string source;            // the file it was read from (empty: not found)

    bool Empty() const { return allods.empty(); }

    // The allod's index of a zone ("zone12", case does not matter), or -1.
    int Find(const std::string& zone) const {
        const std::string z = Lower(zone);
        for (size_t a = 0; a < allods.size(); ++a)
            for (const std::string& w : allods[a].zones)
                if (Lower(w) == z) return static_cast<int>(a);
        return -1;
    }
    std::string NameOf(const std::string& zone) const {
        const int i = Find(zone);
        return i < 0 ? std::string() : allods[static_cast<size_t>(i)].name;
    }
    static std::string Lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
};

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// The [MPZone] section of an INI text.
inline Table ParseMpZone(const std::string& ini) {
    Table t;
    bool in = false;
    size_t pos = 0;
    while (pos <= ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        const std::string line = Trim(ini.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty()) continue;
        if (line[0] == '[') { in = Table::Lower(line) == "[mpzone]"; continue; }
        if (!in) continue;
        const bool commentedOut = line[0] == ';';
        const std::string body = commentedOut ? Trim(line.substr(1)) : line;
        const size_t eq = body.find('=');
        if (eq == std::string::npos) continue; // a plain comment
        const std::string name = Trim(body.substr(0, eq)), zone = Trim(body.substr(eq + 1));
        if (name.empty() || zone.empty()) continue; // "Name=" closes the block
        if (name.find(' ') != std::string::npos) continue; // ";some words = ..." is prose, not a zone line
        if (commentedOut) { t.commented.push_back(zone); continue; }
        auto it = std::find_if(t.allods.begin(), t.allods.end(), [&](const Allod& a) { return Table::Lower(a.name) == Table::Lower(name); });
        if (it == t.allods.end()) { t.allods.push_back({name, {}}); it = t.allods.end() - 1; }
        it->zones.push_back(zone);
    }
    return t;
}

// The reader of a packed config.reg (the game's binary registry) lives in the INI / REG editor; the caller passes it in.
using RegToText = bool (*)(const std::vector<uint8_t>& reg, std::string& ini, std::string& err);

inline bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// config.ini (preferred: plain text) or config.reg in this folder.
inline bool LoadFromFolder(const std::string& folder, Table& out, RegToText regToText) {
    std::error_code ec;
    const std::filesystem::path dir(folder);
    for (const char* name : {"config.ini", "config.reg"}) {
        const std::filesystem::path p = dir / name;
        if (!std::filesystem::is_regular_file(p, ec)) continue;
        std::vector<uint8_t> bytes;
        if (!ReadFile(p.string(), bytes)) continue;
        std::string text;
        if (std::string(name) == "config.reg") {
            std::string err;
            if (!regToText || !regToText(bytes, text, err)) continue;
        } else text.assign(bytes.begin(), bytes.end());
        Table t = ParseMpZone(text);
        if (t.Empty()) continue;
        t.source = p.string();
        out = std::move(t);
        return true;
    }
    return false;
}

// The mod's folder is not stored anywhere in the tool's settings: look for the config beside the places the
// settings do name (the database, the map folders, the quest folders), going up a few levels from each, and
// in an unpacked mod's "ini" folder.
inline bool FindAndLoad(const std::vector<std::string>& hints, Table& out, RegToText regToText) {
    std::error_code ec;
    for (const std::string& hint : hints) {
        if (hint.empty()) continue;
        std::filesystem::path p(hint);
        if (!std::filesystem::is_directory(p, ec)) p = p.parent_path();
        for (int up = 0; up < 5 && !p.empty(); ++up, p = p.parent_path()) {
            if (LoadFromFolder(p.string(), out, regToText)) return true;
            if (LoadFromFolder((p / "ini").string(), out, regToText)) return true;
            if (p == p.parent_path()) break;
        }
    }
    return false;
}

} // namespace allods
