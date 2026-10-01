// The quests and scripts of the map the game runs, for the UM DLL Connector's Quests tab: read from the
// map files (the .mob scripts, the quest's .mq texts), with the game's state from um.dll (VARS, SCRIPTS).
//
// How the game runs a quest (docs/game-memory.md): QStart("q") then QObj* calls declare its objectives
// (sub-objectives); the game turns them into a chain of scripts, one per objective, each waiting for its
// condition, and keeps the state in global script variables: "q.<q>.<q>" 1 while the quest runs, 2 once
// completed; "q.<q>.<q>.<N>" for objective N: 1 received (active), 2 done. Its conditions, as the game
// writes them:
//   QObjArea(n)          a hero in area n (AddRoundToArea / AddRectToArea)
//   QObjKillGroup(g)     no unit of group g alive (AddObject(g, unit))
//   QObjSeeUnit(u)       the unit is visible;  QObjKillUnit(u)  the unit is dead
//   QObjGetItem(n)       a hero has the item (HaveItem)
//   QObjSeeObject(o)     a hero within 7 units of the object
//   QObjUse(o, state)    the object (a lever) in that state
// The .mq holds the texts: the entry "quest <q>": the title, the description, then "#subobj N" sections
// (a title line, then the description).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "mapedit/mob_file.hpp"
#include "mapedit/quest_file.hpp"
#include "mapedit/text_codec.hpp"
#include "viewer/item_texts.hpp"
#include "viewer/res_archive.hpp"

namespace quests {

struct Call {
    std::string name;
    std::vector<std::string> args; // as written, quotes removed, trimmed
    size_t at = 0;                 // where it starts in the script text
};

struct Objective {
    Call call;              // QObjKillUnit("GetObject(2000583)")...
    std::string title, text; // from the .mq (#subobj N)
    int line = -1;          // its line in the script view
};

struct Quest {
    std::string name;       // z3xq3
    std::string title, text; // from the .mq
    std::string file;       // the .mob that declares it
    size_t sourceIndex = 0; // its file in Model::sources
    std::vector<Objective> objectives;
};

// A map file's script, for the script view: its lines, and where its scripts wait.
struct Source {
    std::string file;
    std::string cp1251;                          // the script as the file holds it
    std::string utf8;                            // shown
    std::vector<std::pair<size_t, size_t>> lines; // [begin, end) of each line in utf8
    struct Block {
        std::string name;
        int header = 0;                          // the "Script <name>" line (0-based)
        int condFirst = -1, condLast = -1;       // its if ( ... ) condition's lines
    };
    std::vector<Block> blocks;
    int worldScript = -1;                        // the WorldScript line
};

struct Area {
    bool round = true;
    float x = 0, y = 0, r = 0, x2 = 0, y2 = 0; // round: centre and radius; else the rectangle x..x2, y..y2
    bool Contains(float px, float py) const {
        if (round) return (px - x) * (px - x) + (py - y) * (py - y) <= r * r;
        return px >= std::min(x, x2) && px <= std::max(x, x2) && py >= std::min(y, y2) && py <= std::max(y, y2);
    }
    float Distance(float px, float py) const { // 0 inside
        if (round) return std::max(0.0f, std::hypot(px - x, py - y) - r);
        const float dx = std::max({std::min(x, x2) - px, 0.0f, px - std::max(x, x2)});
        const float dy = std::max({std::min(y, y2) - py, 0.0f, py - std::max(y, y2)});
        return std::hypot(dx, dy);
    }
};

struct ObjectInfo {
    std::string name, kind;
    float x = 0, y = 0;
};

struct Model {
    std::vector<Quest> quests;
    std::map<int, std::vector<Area>> areas;                  // area number -> its shapes
    std::map<std::string, std::vector<unsigned>> groups;     // AddObject(group, GetObject(id))
    std::vector<std::pair<std::string, std::string>> scripts; // (script name, file)
    std::map<unsigned, ObjectInfo> objects;                  // every object with an ID, from the .mob files
    std::vector<Source> sources;                             // the files' scripts
    void Clear() { *this = Model(); }
};

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string Unquote(std::string s) {
    s = Trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return Trim(s);
}

// Every call in a script text, in order: name( args ) with nested parentheses; strings may hold commas.
inline std::vector<Call> Calls(const std::string& text) {
    std::vector<Call> out;
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        if (!(std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_')) { ++i; continue; }
        size_t j = i;
        while (j < n && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_' || text[j] == '#')) ++j;
        size_t k = j;
        while (k < n && (text[k] == ' ' || text[k] == '\t')) ++k;
        if (k >= n || text[k] != '(') { i = j; continue; }
        Call c;
        c.name = text.substr(i, j - i);
        c.at = i;
        int depth = 0;
        bool quoted = false;
        std::string arg;
        size_t m = k;
        for (; m < n; ++m) {
            const char ch = text[m];
            if (ch == '"') quoted = !quoted;
            if (!quoted && ch == '(') { if (depth++ == 0) continue; }
            if (!quoted && ch == ')') { if (--depth == 0) break; }
            if (!quoted && ch == ',' && depth == 1) { c.args.push_back(Unquote(arg)); arg.clear(); continue; }
            arg += ch;
        }
        if (!Unquote(arg).empty() || !c.args.empty()) c.args.push_back(Unquote(arg));
        out.push_back(c);
        i = j; // nested calls (inside the arguments) are listed too
    }
    return out;
}

// "GetObject(2000583)" -> 2000583 (0: not that form).
inline unsigned ObjectId(const std::string& arg) {
    const std::string a = Trim(arg);
    if (a.compare(0, 10, "GetObject(") != 0) return 0;
    return static_cast<unsigned>(std::strtoul(a.c_str() + 10, nullptr, 10));
}

inline void AddMob(Model& m, const mob::File& f) {
    for (const mob::Object& o : f.objects) {
        if (!o.hasId) continue;
        ObjectInfo& info = m.objects[o.id];
        info.name = o.name;
        info.kind = mob::KindName(o.kind);
        info.x = o.position.x;
        info.y = o.position.y;
    }
    if (!f.hasScript) return;
    const std::vector<Call> calls = Calls(f.script);
    // The script view: the text in UTF-8 (the files hold Windows-1251), its lines, its blocks.
    Source src;
    src.file = f.fileName;
    src.cp1251 = f.script;
    std::vector<size_t> lineOf(f.script.size() + 1, 0); // byte of the cp1251 text -> line
    {
        int line = 0;
        size_t lineStart = 0;
        for (size_t p = 0; p <= f.script.size(); ++p) {
            lineOf[p] = static_cast<size_t>(line);
            if (p == f.script.size() || f.script[p] == '\n') {
                std::string piece(f.script.begin() + static_cast<long>(lineStart), f.script.begin() + static_cast<long>(p));
                if (!piece.empty() && piece.back() == '\r') piece.pop_back();
                const std::string u = codec::ToUtf8(std::vector<uint8_t>(piece.begin(), piece.end()), codec::Encoding::Cp1251);
                src.lines.push_back({src.utf8.size(), src.utf8.size() + u.size()});
                src.utf8 += u;
                src.utf8 += '\n';
                lineStart = p + 1;
                ++line;
            }
        }
    }
    auto lineAt = [&](size_t byte) { return static_cast<int>(lineOf[std::min(byte, f.script.size())]); };
    const size_t sourceIndex = m.sources.size();
    Quest* quest = nullptr;
    // "Script Name" blocks (the call scanner skips them: no parenthesis right after the name).
    for (size_t at = f.script.find("Script "); at != std::string::npos; at = f.script.find("Script ", at + 7)) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(f.script[at - 1])) || f.script[at - 1] == '_')) continue; // DeclareScript, WorldScript
        size_t b = at + 7, e = b;
        while (e < f.script.size() && (std::isalnum(static_cast<unsigned char>(f.script[e])) || f.script[e] == '#' || f.script[e] == '_')) ++e;
        if (e <= b) continue;
        m.scripts.push_back({f.script.substr(b, e - b), f.fileName});
        Source::Block blk;
        blk.name = f.script.substr(b, e - b);
        blk.header = lineAt(at);
        // Its condition: "if" then the parenthesis that follows, to its match.
        size_t ifAt = f.script.find("if", e);
        while (ifAt != std::string::npos && ((ifAt > 0 && std::isalnum(static_cast<unsigned char>(f.script[ifAt - 1]))) ||
                                             (ifAt + 2 < f.script.size() && std::isalnum(static_cast<unsigned char>(f.script[ifAt + 2])))))
            ifAt = f.script.find("if", ifAt + 2);
        const size_t nextScript = f.script.find("Script ", e);
        if (ifAt != std::string::npos && (nextScript == std::string::npos || ifAt < nextScript)) {
            const size_t open = f.script.find('(', ifAt);
            int depth = 0;
            for (size_t p = open; open != std::string::npos && p < f.script.size(); ++p) {
                if (f.script[p] == '(') ++depth;
                else if (f.script[p] == ')' && --depth == 0) { blk.condFirst = lineAt(ifAt); blk.condLast = lineAt(p); break; }
            }
        }
        src.blocks.push_back(blk);
    }
    if (const size_t w = f.script.find("WorldScript"); w != std::string::npos) src.worldScript = lineAt(w);
    for (const Call& c : calls) {
        if (c.name == "QStart" && !c.args.empty()) {
            m.quests.push_back({});
            quest = &m.quests.back();
            quest->name = c.args[0];
            quest->file = f.fileName;
            quest->sourceIndex = sourceIndex;
        } else if (c.name.compare(0, 4, "QObj") == 0 && quest) {
            quest->objectives.push_back({c, "", "", lineAt(c.at)});
        } else if (c.name == "QFinish") {
            quest = nullptr;
        } else if (c.name == "AddRoundToArea" && c.args.size() >= 4) {
            Area a;
            a.x = std::strtof(c.args[1].c_str(), nullptr); a.y = std::strtof(c.args[2].c_str(), nullptr); a.r = std::strtof(c.args[3].c_str(), nullptr);
            m.areas[std::atoi(c.args[0].c_str())].push_back(a);
        } else if (c.name == "AddRectToArea" && c.args.size() >= 5) {
            Area a;
            a.round = false;
            a.x = std::strtof(c.args[1].c_str(), nullptr); a.y = std::strtof(c.args[2].c_str(), nullptr);
            a.x2 = std::strtof(c.args[3].c_str(), nullptr); a.y2 = std::strtof(c.args[4].c_str(), nullptr);
            m.areas[std::atoi(c.args[0].c_str())].push_back(a);
        } else if (c.name == "AddObject" && c.args.size() >= 2) {
            if (const unsigned id = ObjectId(c.args[1])) m.groups[Trim(c.args[0])].push_back(id);
        }
    }
    m.sources.push_back(std::move(src));
}

// The quest's texts (its .mq's entry "quest <name>"): UTF-8, Korean (CP949) or Windows-1251.
inline void ParseTexts(const std::vector<uint8_t>& data, Quest& q) {
    std::string text;
    for (char ch : texts::DecodeToUtf8(data)) if (ch != '\r') text += ch;
    std::vector<std::string> lines;
    for (size_t s = 0; s <= text.size();) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        lines.push_back(text.substr(s, e - s));
        s = e + 1;
    }
    int current = 0; // 0: the quest itself, else objective N
    bool titleTaken = false;
    for (const std::string& raw : lines) {
        const std::string line = Trim(raw);
        if (line.compare(0, 7, "#subobj") == 0) { current = std::atoi(line.c_str() + 7); titleTaken = false; continue; }
        if (line.empty()) continue;
        std::string* title = &q.title;
        std::string* body = &q.text;
        if (current > 0) {
            if (current > static_cast<int>(q.objectives.size())) continue;
            title = &q.objectives[current - 1].title;
            body = &q.objectives[current - 1].text;
        }
        if (!titleTaken) { *title = line; titleTaken = true; }
        else *body += (body->empty() ? "" : " ") + line;
    }
}

// From a .mq file or an unpacked quest folder.
inline bool LoadTexts(const std::string& questPath, Quest& q) {
    quest::Quest file;
    if (!quest::Load(questPath, file)) return false;
    std::vector<uint8_t> data;
    if (!quest::ReadEntry(file, "quest " + q.name, data)) return false;
    ParseTexts(data, q);
    return !q.title.empty();
}

// From the quest folders and language packs of the Settings (the English pack first, when there is one).
inline bool LoadTexts(const std::vector<std::string>& folders, const std::vector<std::string>& packs, Quest& q) {
    for (const quest::QuestSet& s : quest::Scan(folders, packs)) {
        if (quest::Lower(s.shown.name) != quest::Lower(q.name)) continue;
        std::vector<const quest::Quest*> order;
        for (size_t i = 0; i < s.copies.size(); ++i)
            if (quest::Lower(s.labels[i]).find("eng") != std::string::npos) order.push_back(&s.copies[i]);
        order.push_back(&s.shown);
        for (const quest::Quest& c : s.copies) order.push_back(&c);
        for (const quest::Quest* c : order) {
            std::vector<uint8_t> data;
            if (quest::ReadEntry(*c, "quest " + q.name, data)) { ParseTexts(data, q); if (!q.title.empty()) return true; }
        }
    }
    return false;
}

// The area calls of a script, with where each one is in the text (to rewrite it: the Map Editor's Areas).
struct AreaCall {
    size_t begin = 0, end = 0; // [begin, end) in the script text: "AddRectToArea( ... )"
    bool round = true;         // AddRoundToArea(id, x, y, r); else AddRectToArea(id, x1, y1, x2, y2)
    int id = 0;
    float v[4] = {0, 0, 0, 0};
};

inline std::vector<AreaCall> AreaCalls(const std::string& script) {
    std::vector<AreaCall> out;
    for (const Call& c : Calls(script)) {
        const bool round = c.name == "AddRoundToArea";
        if (!round && c.name != "AddRectToArea") continue;
        if (c.args.size() < (round ? 4u : 5u)) continue;
        const size_t open = script.find('(', c.at);
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t p = open; open != std::string::npos && p < script.size(); ++p) {
            if (script[p] == '(') ++depth;
            else if (script[p] == ')' && --depth == 0) { close = p; break; }
        }
        if (close == std::string::npos) continue;
        AreaCall a;
        a.begin = c.at;
        a.end = close + 1;
        a.round = round;
        a.id = std::atoi(c.args[0].c_str());
        for (int i = 0; i < (round ? 3 : 4); ++i) a.v[i] = std::strtof(c.args[i + 1].c_str(), nullptr);
        out.push_back(a);
    }
    return out;
}

inline std::string FormatAreaCall(const AreaCall& a) {
    std::string s = std::string(a.round ? "AddRoundToArea" : "AddRectToArea") + "( " + std::to_string(a.id);
    for (int i = 0; i < (a.round ? 3 : 4); ++i) {
        char b[32];
        std::snprintf(b, sizeof(b), ", %g", static_cast<double>(std::round(a.v[i] * 100.0f) / 100.0f));
        s += b;
    }
    return s + " )";
}

// What an objective asks, in words: "Kill unit 2000583 (Seer)".
inline std::string Describe(const Model& m, const Call& c) {
    auto object = [&](const std::string& arg) {
        const unsigned id = ObjectId(arg);
        if (!id) return arg;
        auto it = m.objects.find(id);
        return std::to_string(id) + (it != m.objects.end() && !it->second.name.empty() ? " (" + it->second.name + ")" : "");
    };
    const std::string a0 = c.args.empty() ? "" : c.args[0];
    if (c.name == "QObjArea") return "Reach area " + a0;
    if (c.name == "QObjKillGroup") return "Kill group " + a0;
    if (c.name == "QObjSeeUnit") return "See unit " + object(a0);
    if (c.name == "QObjKillUnit") return "Kill unit " + object(a0);
    if (c.name == "QObjGetItem") return "Get item " + a0;
    if (c.name == "QObjSeeObject") return "Come within 7 of object " + object(a0);
    if (c.name == "QObjUse") return "Use object " + object(a0);
    std::string s = c.name + "(";
    for (size_t i = 0; i < c.args.size(); ++i) s += (i ? ", " : "") + c.args[i];
    return s + ")";
}

} // namespace quests
