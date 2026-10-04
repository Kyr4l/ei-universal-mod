// Grouped texts: a texts.res / textslmp.res folder kept as a few <TYPE>.umtexts files instead of one file per entry.
// The type is the entry name's first word in capitals (ARMOR, WEAPON, QUESTITEM, string, pers...), the kind of string the game
// reads. A .umtexts file:
//
//   # um-texts 1
//   === ARMOR Gipat_Brigand_Boots Hyena_Hide
//   Hyena hide boots
//   Walk confidently with these durable boots.
//   === ARMOR Gipat_Brigand_Boots Wolf_Hide
//   ...
//
// An entry is its "=== <name>" line, then its text byte for byte (any encoding, CRLF kept): the text is what lies
// between that line and the next "=== " line, less the one newline the format adds after it ("\n", or "\r\n" when an
// editor converted the line ends). A text line starting
// with "===" or "\" is written with a "\" before it. Loose files in the same folder still count (an entry in both:
// the loose file wins), so old folders keep working.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace textgroups {

namespace fs = std::filesystem;
inline const char* kExt = ".umtexts";

inline std::string GroupOf(const std::string& name) {
    std::string g = name.substr(0, name.find(' '));
    for (char& c : g) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); // one file per type, whatever the case (Windows)
    for (char& c : g) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    return g.empty() ? "misc" : g;
}

using Entries = std::map<std::string, std::vector<uint8_t>>; // name -> text bytes

inline void Parse(const std::vector<uint8_t>& d, Entries& out) {
    std::string name;
    std::vector<uint8_t> body;
    bool in = false;
    auto flush = [&]() {
        if (!in) return;
        // The newline the format added after the text: "\n", or "\r\n" once an editor converted the file's line ends
        // (a text never ends with a lone "\r": none of the vanilla or mod texts do).
        if (body.size() >= 2 && body[body.size() - 2] == '\r' && body.back() == '\n') body.resize(body.size() - 2);
        else if (!body.empty() && body.back() == '\n') body.pop_back();
        out[name] = body;
        body.clear();
    };
    size_t p = 0;
    while (p < d.size()) {
        size_t e = p;
        while (e < d.size() && d[e] != '\n') ++e;
        const bool hasNl = e < d.size();
        const std::string line(d.begin() + static_cast<long>(p), d.begin() + static_cast<long>(e));
        if (line.rfind("=== ", 0) == 0) {
            flush();
            name = line.substr(4);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            in = true;
        } else if (in) {
            size_t from = p;
            if (!line.empty() && line[0] == '\\') ++from; // an escaped line
            body.insert(body.end(), d.begin() + static_cast<long>(from), d.begin() + static_cast<long>(e));
            if (hasNl) body.push_back('\n');
        }
        p = hasNl ? e + 1 : e;
    }
    flush();
}

inline std::vector<uint8_t> Write(const Entries& entries) {
    std::vector<uint8_t> o;
    auto put = [&](const std::string& s) { o.insert(o.end(), s.begin(), s.end()); };
    put("# um-texts 1\n");
    for (const auto& [name, body] : entries) {
        put("=== " + name + "\n");
        size_t p = 0;
        while (true) {
            size_t e = p;
            while (e < body.size() && body[e] != '\n') ++e;
            if (e > p && (body[p] == '\\' || (e - p >= 3 && body[p] == '=' && body[p + 1] == '=' && body[p + 2] == '='))) o.push_back('\\');
            o.insert(o.end(), body.begin() + static_cast<long>(p), body.begin() + static_cast<long>(e));
            if (e >= body.size()) break;
            o.push_back('\n');
            p = e + 1;
        }
        o.push_back('\n');
    }
    return o;
}

inline bool ReadAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
inline bool WriteAll(const fs::path& p, const std::vector<uint8_t>& d) {
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()))) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

inline bool IsGrouped(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) if (e.is_regular_file() && e.path().extension() == kExt) return true;
    return false;
}

// Every entry of a folder: the .umtexts files, then the loose files (which win). `skip`: loose names left out.
inline Entries LoadFolder(const fs::path& dir, const std::vector<std::string>& skip = {}) {
    Entries out;
    std::error_code ec;
    std::vector<fs::path> groups, loose;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        (e.path().extension() == kExt ? groups : loose).push_back(e.path());
    }
    std::sort(groups.begin(), groups.end());
    for (const fs::path& g : groups) { std::vector<uint8_t> d; if (ReadAll(g, d)) Parse(d, out); }
    for (const fs::path& f : loose) {
        const std::string n = f.filename().string();
        if (std::find(skip.begin(), skip.end(), n) != skip.end() || (n.size() > 4 && n.substr(n.size() - 4) == ".tmp")) continue;
        std::vector<uint8_t> d;
        if (ReadAll(f, d)) out[n] = d;
    }
    return out;
}

// Writes the entries as <group>.umtexts files into `dir` (the groups not given are left alone).
inline bool SaveGroups(const fs::path& dir, const Entries& entries, std::string& err) {
    std::map<std::string, Entries> by;
    for (const auto& kv : entries) by[GroupOf(kv.first)][kv.first] = kv.second;
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& [g, es] : by)
        if (!WriteAll(dir / (g + kExt), Write(es))) { err = "cannot write " + (dir / (g + kExt)).string(); return false; }
    return true;
}

// One entry set (or replaced) in a folder: into its group file when the folder is grouped, else a loose file.
inline bool SetEntry(const fs::path& dir, const std::string& name, const std::vector<uint8_t>& text, std::string& err) {
    if (!IsGrouped(dir)) {
        if (!WriteAll(dir / name, text)) { err = "cannot write " + (dir / name).string(); return false; }
        return true;
    }
    const fs::path g = dir / (GroupOf(name) + kExt);
    Entries es;
    std::vector<uint8_t> d;
    if (ReadAll(g, d)) Parse(d, es);
    es[name] = text;
    std::error_code ec;
    if (fs::exists(dir / name, ec)) fs::remove(dir / name, ec); // a loose copy would shadow it
    if (!WriteAll(g, Write(es))) { err = "cannot write " + g.string(); return false; }
    return true;
}

// A folder of loose files turned into group files (the loose files removed once written). `skip`: kept loose.
inline bool GroupFolder(const fs::path& dir, const std::vector<std::string>& skip, int& count, std::string& err) {
    const Entries es = LoadFolder(dir, skip);
    count = static_cast<int>(es.size());
    if (!SaveGroups(dir, es, err)) return false;
    std::error_code ec;
    std::vector<fs::path> remove;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() == kExt) continue;
        const std::string n = e.path().filename().string();
        if (std::find(skip.begin(), skip.end(), n) == skip.end()) remove.push_back(e.path());
    }
    for (const fs::path& p : remove) fs::remove(p, ec);
    return true;
}

} // namespace textgroups
