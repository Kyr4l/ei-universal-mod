// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Quests: a .mq archive (RES format) or its unpacked folder (<name>_mq/<name>/map.txt), holding a
// map.txt that tells the game what to load and where the party enters and leaves:
//
//   #zone z8q1 gipat game        the quest, its region (the lights<region>.ini to use) and kind
//   #res                         the next line: the terrain (.mpr) and the base map (.mob)
//   zone8 zone8-lmp
//   #maps / #weather / #sky      the map pictures, the weather, "cave" for the cave lighting
//   ## To Ruins                  one block per exit: its title,
//   #exit 1                      its number and (next line) where it leads,
//   bz1mpg 1
//   #deploy                      the next line: x1 y1 x2 y2 of the rectangle the party is deployed in
//   169 332 179 338
//   #remove                      the next line: x1 y1 x2 y2 of the rectangle that leaves the map
//   165 338 179 349
//   #view                        the next line: the camera angle on arrival
//   192.876
//
// The quest's own map is <name>.mob, next to the base map.
#pragma once

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../viewer/res_archive.hpp"

namespace quest {

struct Rect { bool set = false; float x1 = 0, y1 = 0, x2 = 0, y2 = 0; };

struct Exit {
    std::string title;   // "To Ruins" (from the "## " line before it)
    int number = 0;
    std::string target;  // the line after #exit, e.g. "bz1mpg 1"
    Rect deploy, remove;
    float view = 0;
    bool hasView = false;
};

struct Quest {
    std::string name;            // z8q1
    std::string path;            // the .mq file or the unpacked folder
    bool packed = false;
    std::string region, kind, sky, weather;
    std::string terrain, baseMap; // without extensions
    std::vector<Exit> exits;
    std::string error;

    // lights<region>.ini, or lightscave<region>.ini under a cave sky.
    std::string LightingFile() const {
        if (region.empty()) return std::string();
        return "lights" + std::string(sky == "cave" ? "cave" : "") + region + ".ini";
    }
};

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline void Parse(const std::string& text, Quest& q) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        lines.push_back(start == std::string::npos ? std::string() : line.substr(start));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    auto next = [&](size_t i) { // the next non-empty line after i
        for (size_t j = i + 1; j < lines.size(); ++j) if (!lines[j].empty()) return lines[j];
        return std::string();
    };
    auto rect = [](const std::string& s) {
        Rect r;
        r.set = std::sscanf(s.c_str(), "%f %f %f %f", &r.x1, &r.y1, &r.x2, &r.y2) == 4;
        return r;
    };
    std::string pendingTitle;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.empty()) continue;
        std::string key = Lower(l.substr(0, l.find_first_of(" \t")));
        if (l.rfind("##", 0) == 0) { pendingTitle = l.substr(l.find_first_not_of("# ") == std::string::npos ? l.size() : l.find_first_not_of("# ")); continue; }
        if (key == "#zone") {
            char name[128] = "", region[64] = "", kind[64] = "";
            std::sscanf(l.c_str(), "#%*s %127s %63s %63s", name, region, kind);
            if (q.name.empty()) q.name = name;
            q.region = Lower(region);
            q.kind = kind;
        } else if (key == "#res") {
            char terrain[128] = "", base[128] = "";
            if (std::sscanf(next(i).c_str(), "%127s %127s", terrain, base) >= 1) { q.terrain = terrain; q.baseMap = base; }
        } else if (key == "#sky") q.sky = Lower(next(i));
        else if (key == "#weather") q.weather = Lower(next(i));
        else if (key == "#exit") {
            Exit e;
            e.title = pendingTitle;
            pendingTitle.clear();
            std::sscanf(l.c_str(), "#%*s %d", &e.number);
            e.target = next(i);
            q.exits.push_back(e);
        } else if (key == "#deploy" && !q.exits.empty()) q.exits.back().deploy = rect(next(i));
        else if (key == "#remove" && !q.exits.empty()) q.exits.back().remove = rect(next(i));
        else if (key == "#view" && !q.exits.empty()) {
            q.exits.back().hasView = std::sscanf(next(i).c_str(), "%f", &q.exits.back().view) == 1;
        }
    }
}

// A packed .mq (any entry ending in map.txt) or an unpacked folder (<dir>/map.txt or <dir>/<name>/map.txt).
inline bool Load(const std::string& path, Quest& q) {
    q = Quest{};
    q.path = path;
    std::error_code ec;
    std::filesystem::path p(path);
    std::string text;
    if (std::filesystem::is_directory(p, ec)) {
        std::filesystem::path mapTxt;
        if (std::filesystem::exists(p / "map.txt", ec)) mapTxt = p / "map.txt";
        else
            for (const auto& entry : std::filesystem::directory_iterator(p, ec))
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "map.txt", ec)) { mapTxt = entry.path() / "map.txt"; break; }
        if (mapTxt.empty()) { q.error = "no map.txt"; return false; }
        std::ifstream f(mapTxt, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        q.name = mapTxt.parent_path() == p ? p.filename().string() : mapTxt.parent_path().filename().string();
    } else {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { q.error = "cannot open"; return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res::Archive archive;
        if (!res::ParseArchive(bytes, archive, q.error)) return false;
        for (const auto& e : archive.entries)
            if (e.first.size() >= 7 && e.first.compare(e.first.size() - 7, 7, "map.txt") == 0) { text.assign(e.second.data.begin(), e.second.data.end()); break; }
        if (text.empty()) { q.error = "no map.txt in the archive"; return false; }
        q.packed = true;
        q.name = p.stem().string();
    }
    std::string fileName = q.name;
    Parse(text, q);
    if (q.name.empty()) q.name = fileName;
    return true;
}

// ---- the files inside a quest -------------------------------------------------------------------

// Names of the files a quest holds: archive entries (e.g. "z8q1\\map.txt") or paths relative to the
// unpacked folder (e.g. "z8q1/map.txt", "briefing z8q1_1").
inline std::vector<std::string> ListEntries(const Quest& q) {
    std::vector<std::string> out;
    std::error_code ec;
    if (q.packed) {
        std::ifstream f(q.path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res::Archive a;
        std::string err;
        if (res::ParseArchive(bytes, a, err))
            for (const auto& e : a.entries) out.push_back(e.second.originalName);
    } else {
        std::filesystem::path root(q.path);
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file()) out.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return out;
}

inline bool EndsWithLower(const std::string& s, const char* tail) {
    std::string l = Lower(s), t = tail;
    return l.size() >= t.size() && l.compare(l.size() - t.size(), t.size(), t) == 0;
}

inline bool ReadEntry(const Quest& q, const std::string& name, std::vector<uint8_t>& out) {
    if (q.packed) {
        std::ifstream f(q.path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res::Archive a;
        std::string err;
        if (!res::ParseArchive(bytes, a, err)) return false;
        const std::vector<uint8_t>* d = a.Find(name);
        if (!d) return false;
        out = *d;
        return true;
    }
    std::ifstream f(std::filesystem::path(q.path) / name, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Replaces files of a quest: inside the archive (everything else kept as it was) or on disk.
inline bool WriteEntries(const Quest& q, const std::map<std::string, std::vector<uint8_t>>& files, std::string& err) {
    if (q.packed) {
        std::ifstream f(q.path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::map<std::string, std::vector<uint8_t>> byLower;
        for (const auto& kv : files) byLower[res::Archive::ToLower(kv.first)] = kv.second;
        std::vector<uint8_t> out;
        if (!res::RewriteArchive(bytes, byLower, static_cast<uint32_t>(std::time(nullptr)), out, err)) return false;
        std::string tmp = q.path + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()))) { err = "cannot write " + tmp; return false; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, q.path, ec);
        if (ec) { err = "cannot replace " + q.path + ": " + ec.message(); return false; }
        return true;
    }
    for (const auto& kv : files) {
        std::ofstream o(std::filesystem::path(q.path) / kv.first, std::ios::binary | std::ios::trunc);
        if (!o.write(reinterpret_cast<const char*>(kv.second.data()), static_cast<std::streamsize>(kv.second.size()))) {
            err = "cannot write " + kv.first;
            return false;
        }
    }
    return true;
}

inline std::string MapTxtEntry(const Quest& q) {
    for (const std::string& e : ListEntries(q)) if (EndsWithLower(e, "map.txt")) return e;
    return std::string();
}

inline std::string FormatNumber(float v) {
    char b[32];
    if (v == static_cast<float>(static_cast<long>(v))) std::snprintf(b, sizeof(b), "%ld", static_cast<long>(v));
    else std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

// map.txt with the #deploy / #remove lines of each exit replaced by `exits`' rectangles; every other
// byte (comments, order, line endings) stays as it was.
inline std::string UpdateRects(const std::string& text, const std::vector<Exit>& exits) {
    std::string out;
    size_t at = 0;
    int exitIndex = -1;
    const Rect* pending = nullptr; // the rectangle whose numbers the next non-empty line holds
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        size_t lineEnd = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(at, lineEnd - at);
        std::string eol = end == std::string::npos ? "" : "\n";
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string trimmed = line.substr(std::min(line.size(), line.find_first_not_of(" \t") == std::string::npos ? line.size() : line.find_first_not_of(" \t")));
        std::string key = Lower(trimmed.substr(0, trimmed.find_first_of(" \t")));
        if (pending && !trimmed.empty()) {
            Rect old;
            old.set = std::sscanf(trimmed.c_str(), "%f %f %f %f", &old.x1, &old.y1, &old.x2, &old.y2) == 4;
            const bool changed = !old.set || old.x1 != pending->x1 || old.y1 != pending->y1 || old.x2 != pending->x2 || old.y2 != pending->y2;
            if (pending->set && changed) // untouched lines keep their exact bytes (some end with a space)
                line = FormatNumber(pending->x1) + " " + FormatNumber(pending->y1) + " " + FormatNumber(pending->x2) + " " + FormatNumber(pending->y2);
            pending = nullptr;
        } else if (key == "#exit") {
            ++exitIndex;
        } else if ((key == "#deploy" || key == "#remove") && exitIndex >= 0 && exitIndex < static_cast<int>(exits.size())) {
            pending = key == "#deploy" ? &exits[exitIndex].deploy : &exits[exitIndex].remove;
        }
        out += line + (cr ? "\r" : "") + eol;
        at = end == std::string::npos ? text.size() : end + 1;
    }
    return out;
}

// ---- quest lists ----------------------------------------------------------------------------------

// One quest as the editor offers it. Language packs hold the same quests in different languages: a
// quest found in them lists every pack's copy, and changes that do not depend on the language (the
// deploy and exit areas, map.txt, quest.ini) are written to all of them.
struct QuestSet {
    Quest shown;                     // the copy shown and read (the first language pack's, else the top folder's)
    std::vector<Quest> copies;       // every copy changes go to (just `shown` without language packs)
    std::vector<std::string> labels; // a name per copy (its folder), for the language picker
    bool languagePack = false;
};

inline std::vector<Quest> ScanFolder(const std::string& folder) {
    std::vector<Quest> out;
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_regular_file(folder, ec)) candidates.push_back(folder);
    else if (std::filesystem::exists(std::filesystem::path(folder) / "map.txt", ec)) candidates.push_back(folder);
    else
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            std::string lower = Lower(entry.path().filename().string());
            if (entry.is_regular_file() && lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".mq") == 0) candidates.push_back(entry.path());
            else if (entry.is_directory()) candidates.push_back(entry.path());
        }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& c : candidates) {
        Quest q;
        if (Load(c.string(), q)) out.push_back(std::move(q));
    }
    return out;
}

// A short name for a folder: its own name, or its parent's when that is a generic "maps" folder
// (Universal-Mod/lang-packs/fra/maps -> "fra").
inline std::string FolderLabel(const std::string& folder) {
    std::filesystem::path p(folder);
    if (p.filename().empty()) p = p.parent_path();
    std::string name = p.filename().string();
    if (Lower(name) == "maps" && !p.parent_path().filename().empty()) name = p.parent_path().filename().string();
    return name;
}

// Quests of the quest folders (a later folder's quest replaces an earlier one with the same name), then
// of the language packs (each one the same quests in another language; they come on top).
inline std::vector<QuestSet> Scan(const std::vector<std::string>& folders, const std::vector<std::string>& packs) {
    std::map<std::string, QuestSet> byName;
    for (const std::string& folder : folders)
        for (Quest& q : ScanFolder(folder)) {
            QuestSet& s = byName[Lower(q.name)];
            s = QuestSet{};
            s.shown = q;
            s.copies.push_back(q);
            s.labels.push_back(FolderLabel(folder));
        }
    std::map<std::string, bool> fromPacks;
    for (const std::string& pack : packs)
        for (Quest& q : ScanFolder(pack)) {
            QuestSet& s = byName[Lower(q.name)];
            if (!fromPacks[Lower(q.name)]) { s = QuestSet{}; s.shown = q; fromPacks[Lower(q.name)] = true; }
            s.languagePack = true;
            s.copies.push_back(q);
            s.labels.push_back(FolderLabel(pack));
        }
    std::vector<QuestSet> out;
    for (auto& kv : byName) out.push_back(std::move(kv.second));
    return out;
}

} // namespace quest
