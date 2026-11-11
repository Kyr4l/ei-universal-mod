// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Quest Builder's quest, before it is a quest: what the wizard collects (map, story, enemy groups, objectives, rewards)
// and the files it is written as:
//
//   <name>.mq          a RES archive: "quest <name>" (title, summary, one "#subobj N" block per objective), "briefing <name>_1/_2/_3"
//                      (the talk on receiving, rejecting and completing), "<name>\map.txt" and "<name>\quest.reg" (rewards, the quest
//                      variables the objectives set)
//   <name>.mob         the quest's own map: its units, and the mission script that starts the quest (QStart, QObj*, QFinish)
//
// Nothing here draws anything: the wizard (quest_builder.cpp) and tests/quest_draft_test.cpp both call it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mob_file.hpp"
#include "quest_file.hpp"
#include "quest_skeleton.hpp"
#include "text_codec.hpp"

namespace qdraft {

struct Group {
    std::string name;     // the script's group: letters and digits
    std::string monster;  // the database unit (its prototype name)
    std::string templ;    // its figure (the race's mask), filled by the wizard from the database
    int count = 4;
    float x = 0, y = 0, radius = 4;
};

struct Objective {
    enum Kind { KillGroup, Area };
    Kind kind = KillGroup;
    std::string title, text;
    int group = 0;                 // KillGroup: index in Draft::groups
    float x = 0, y = 0, size = 8;  // Area: the square's centre and side
};

struct Line { std::string speaker, text; };

struct Draft {
    std::string name, title, summary;
    std::string terrain, baseMap;                       // "zone26", "zone26-lmp"
    std::string region, sky = "normal", weather = "none";
    std::string mapsBlock;                              // the lines after #maps (the picture's grid, its textures), copied from a quest of the zone
    std::string exitTitle = "Leave", exitTarget;        // where the exit leads: "bz1mpg 1"
    quest::Rect deploy, remove;
    float view = 0;
    std::vector<Group> groups;
    std::vector<Objective> objectives;
    std::string npc = "Hero";                           // the one who gives the quest besides the hero
    std::vector<Line> intro, reject, outro;
    int exp = 0, money = 0;
    uint32_t firstId = 1000000;                         // the quest map's object IDs run from here
    std::string tag;                                    // "Easy", "Medium", "Hard": the briefing's first line
};

// ---- names ------------------------------------------------------------------------------------------------------

inline bool ValidName(const std::string& n) {
    if (n.size() < 3 || n.size() > 24 || !std::isalpha(static_cast<unsigned char>(n[0]))) return false;
    for (char c : n) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

// A script identifier from free text: letters and digits, a letter first.
inline std::string Identifier(const std::string& s, const std::string& fallback) {
    std::string out;
    for (char c : s) if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80) out += c;
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0]))) out = fallback + out;
    return out;
}

// The groups' script names, each unique.
inline std::vector<std::string> GroupNames(const Draft& d) {
    std::vector<std::string> out;
    std::set<std::string> used;
    for (size_t i = 0; i < d.groups.size(); ++i) {
        std::string n = Identifier(d.groups[i].name.empty() ? d.groups[i].monster : d.groups[i].name, "Group");
        std::string unique = n;
        for (int k = 2; used.count(quest::Lower(unique)); ++k) unique = n + std::to_string(k);
        used.insert(quest::Lower(unique));
        out.push_back(unique);
    }
    return out;
}

// ---- where the units stand --------------------------------------------------------------------------------------

struct Placed { size_t group; int index; float x, y; };

// Every unit of every group: on a spiral around the group's centre (even spacing, no two on one spot).
inline std::vector<Placed> Layout(const Draft& d) {
    std::vector<Placed> out;
    for (size_t g = 0; g < d.groups.size(); ++g) {
        const Group& gr = d.groups[g];
        const int n = std::max(1, gr.count);
        for (int i = 0; i < n; ++i) {
            const float t = std::sqrt((i + 0.5f) / n), a = 2.399963f * i; // golden angle
            out.push_back({g, i, gr.x + (n == 1 ? 0.0f : gr.radius * t * std::cos(a)), gr.y + (n == 1 ? 0.0f : gr.radius * t * std::sin(a))});
        }
    }
    return out;
}

inline int UnitCount(const Draft& d) {
    int n = 0;
    for (const Group& g : d.groups) n += std::max(1, g.count);
    return n;
}

// ---- the script ---------------------------------------------------------------------------------------------------

inline std::string Num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

// The mission script: WorldScript puts the units in their groups and starts the quest script, which names the quest, shows the
// objectives in order and closes the list (the game finishes the quest when every objective is done).
inline std::string Script(const Draft& d, const std::vector<uint32_t>& ids) {
    const std::vector<std::string> names = GroupNames(d);
    std::string s = "GlobalVars (\n  NULL : object,\n  VSS#i#val : object,\n  i : object";
    for (const std::string& n : names) s += ",\n  " + n + " : group";
    s += "\n)\n\nDeclareScript QuestStart (  this : object )\n\nScript QuestStart\n(\n  if\n  (\n  )\n  then\n  (\n    KillScript(  ) \n";
    s += "    QStart( \"" + d.name + "\" ) \n";
    int area = 0;
    for (const Objective& o : d.objectives) {
        if (o.kind == Objective::Area) {
            ++area;
            const float h = o.size * 0.5f;
            s += "    AddRectToArea( " + std::to_string(area) + ", " + Num(o.x - h) + ", " + Num(o.y - h) + ", " + Num(o.x + h) + ", " + Num(o.y + h) + " ) \n";
            s += "    QObjArea( " + std::to_string(area) + " ) \n";
        } else if (o.group >= 0 && o.group < static_cast<int>(names.size())) {
            s += "    QObjKillGroup( \"" + names[static_cast<size_t>(o.group)] + "\" ) \n";
        }
    }
    s += "    QFinish(  ) \n  )\n)\n\nWorldScript\n(\n  Sleep( 2 ) \n";
    const std::vector<Placed> placed = Layout(d);
    for (size_t i = 0; i < placed.size() && i < ids.size(); ++i)
        s += "  AddObject( " + names[placed[i].group] + ", GetObject( " + std::to_string(ids[i]) + " )  ) \n";
    s += "  QuestStart( NULL ) \n)\n";
    return s;
}

// ---- the quest's files ----------------------------------------------------------------------------------------------

inline std::string Crlf(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '\n') out += "\r\n"; else if (c != '\r') out += c; }
    return out;
}

inline std::string Cp(const std::string& utf8) {
    std::vector<uint8_t> cp;
    std::string err;
    codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err);
    return std::string(cp.begin(), cp.end());
}

inline std::string OneLine(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

inline std::string QuestText(const Draft& d) {
    std::string s = OneLine(d.title) + "\n" + OneLine(d.summary) + "\n";
    int n = 0;
    for (const Objective& o : d.objectives) {
        ++n;
        s += "#subobj  " + std::to_string(n) + "\n" + OneLine(o.title) + "\n" + OneLine(o.text) + "\n";
    }
    return Cp(Crlf(s));
}

inline std::string Briefing(const std::vector<Line>& lines, const std::string& head) {
    std::set<std::string> speakers{"Hero"};
    for (const Line& l : lines) speakers.insert(l.speaker.empty() ? "Hero" : l.speaker);
    std::string s = OneLine(head) + "\n";
    s += "#show Hero\n";
    for (const std::string& sp : speakers) if (sp != "Hero") s += "#show " + sp + "\n";
    s += "#CAMERA 2\n";
    int n = 0;
    for (const Line& l : lines) s += "#phrase " + (l.speaker.empty() ? std::string("Hero") : l.speaker) + "  " + std::to_string(++n) + "\n" + OneLine(l.text) + "\n";
    return Cp(Crlf(s));
}

inline std::string BriefingReceive(const Draft& d) { return Briefing(d.intro, (d.tag.empty() ? std::string() : "[" + d.tag + "] ") + d.title); }
inline std::string BriefingReject(const Draft& d) { return Briefing(d.reject, "Not now"); }
inline std::string BriefingComplete(const Draft& d) { return Briefing(d.outro, "Done"); }

inline std::string MapTxt(const Draft& d) {
    std::string s = "## " + OneLine(d.title) + "\n#zone " + d.name + " " + (d.region.empty() ? "gipat" : d.region) + " game\n#res\n" + d.terrain + " " + d.baseMap + "\n";
    if (!d.mapsBlock.empty()) s += "#maps\n" + d.mapsBlock + (d.mapsBlock.back() == '\n' ? "" : "\n");
    s += "#weather\n" + d.weather + "\n#sky\n" + d.sky + "\n\n";
    s += "## " + OneLine(d.exitTitle) + "\n#exit 1\n" + (d.exitTarget.empty() ? std::string("bz1mpg 1") : d.exitTarget) + "\n";
    auto rect = [&](const char* key, const quest::Rect& r) {
        if (r.set) s += std::string("#") + key + "\n" + quest::FormatNumber(r.x1) + " " + quest::FormatNumber(r.y1) + " " + quest::FormatNumber(r.x2) + " " + quest::FormatNumber(r.y2) + "\n";
    };
    rect("deploy", d.deploy);
    rect("remove", d.remove);
    s += "#view\n" + quest::FormatNumber(d.view) + "\n";
    return Cp(Crlf(s));
}

// quest.reg as an .ini text (IniRegTextToReg makes the binary): the quest, the variables the objectives set, the rewards.
inline std::string QuestIni(const Draft& d) {
    std::string s = "[quest]\nname=" + d.name + "\nexp=0\nmoney=0\n[briefing receive]\nname=" + d.name + "_1\ngive items=\n";
    s += "give quests=q." + d.name + "." + d.name + "\n";
    for (size_t i = 1; i <= d.objectives.size(); ++i) s += "give quests=q." + d.name + "." + d.name + "." + std::to_string(i) + "\n";
    s += "[briefing reject]\nname=" + d.name + "_2\n[briefing complete]\nname=" + d.name + "_3\nexp=" + std::to_string(d.exp) + "\nmoney=" + std::to_string(d.money) + "\ngive items=\n";
    return s;
}

// ---- the quest map --------------------------------------------------------------------------------------------------

// The IDs of the units, in Layout's order.
inline std::vector<uint32_t> UnitIds(const Draft& d) {
    std::vector<uint32_t> ids;
    for (int i = 0; i < UnitCount(d); ++i) ids.push_back(d.firstId + static_cast<uint32_t>(i));
    return ids;
}

// The quest's .mob: the skeleton without its unit, a copy of that unit for every placed one, the script, the ID range. false
// (and `err`) when the skeleton cannot be read.
inline bool BuildMob(const Draft& d, mob::File& f, std::string& err) {
    f = mob::File();
    f.bytes.assign(qdraft::kSkeletonMob, qdraft::kSkeletonMob + qdraft::kSkeletonMobSize);
    f.fileName = d.name + ".mob";
    f.loaded = mob::Parse(f);
    if (!f.loaded || f.objects.empty() || f.objects[0].kind != mob::Kind::Unit || f.secRanges.empty()) { err = "the quest map frame cannot be read"; return false; }
    const std::vector<uint8_t> proto = mob::ObjectNode(f, 0);
    const mob::Vec3 origin = f.objects[0].position;
    std::vector<int> all;
    for (size_t i = 0; i < f.objects.size(); ++i) all.push_back(static_cast<int>(i));
    mob::RemoveObjects(f, all);
    const std::vector<Placed> placed = Layout(d);
    const std::vector<uint32_t> ids = UnitIds(d);
    std::vector<std::vector<uint8_t>> nodes;
    for (size_t i = 0; i < placed.size(); ++i) {
        const Group& g = d.groups[placed[i].group];
        std::vector<uint8_t> node = proto;
        mob::NodeSetId(node, ids[i]);
        mob::NodeMove(node, placed[i].x - origin.x, placed[i].y - origin.y, 0.0f);
        mob::NodeReplaceField(node, mob::kUnitPrototype, mob::TextPayload(Cp(g.monster)));
        mob::NodeReplaceField(node, mob::kParentTemplate, mob::TextPayload(Cp(g.monster)));
        mob::NodeReplaceField(node, mob::kUnitNeedImport, std::vector<uint8_t>{1}); // the game takes the stats from the database
        if (!g.templ.empty()) mob::NodeReplaceField(node, mob::kObjTemplate, mob::TextPayload(Cp(g.templ)));
        mob::NodeReplaceField(node, mob::kObjPrimTexture, mob::TextPayload("default0"));
        mob::NodeReplaceField(node, mob::kObjName, mob::TextPayload(Cp(g.monster + "-" + std::to_string(placed[i].index + 1))));
        nodes.push_back(std::move(node));
    }
    if (!nodes.empty()) mob::InsertObjects(f, nodes);
    // each unit guards the place it stands on (the frame's unit guarded a place of its own)
    for (size_t i = 0; i < f.objects.size(); ++i)
        for (size_t k = 0; k < f.objects[i].logics.size(); ++k) {
            mob::Logic lg = f.objects[i].logics[k];
            lg.guardPlace = f.objects[i].position;
            mob::SetLogic(f, static_cast<int>(i), static_cast<int>(k), lg);
        }
    const uint32_t last = d.firstId + static_cast<uint32_t>(std::max(UnitCount(d), 100)) - 1;
    mob::SetRange(f, f.secRanges[0], d.firstId, last); // a quest map's own IDs are its section range (the zone's map holds 0-999999)
    if (!mob::SetScript(f, Cp(Script(d, ids)))) { err = "the quest map frame has no script"; return false; }
    return true;
}

// ---- checks before saving ----------------------------------------------------------------------------------------------

inline std::vector<std::string> Problems(const Draft& d, const std::vector<std::string>& takenNames = {}) {
    std::vector<std::string> out;
    if (d.terrain.empty() || d.baseMap.empty()) out.push_back("Pick a map");
    if (!ValidName(d.name)) out.push_back("The quest ID needs 3-24 letters, digits or _ , a letter first");
    for (const std::string& t : takenNames) if (quest::Lower(t) == quest::Lower(d.name)) out.push_back("A quest with this ID exists");
    if (d.title.empty()) out.push_back("Give the quest a title");
    if (!d.deploy.set) out.push_back("Place the party's start");
    if (d.groups.empty()) out.push_back("Add an enemy group");
    if (d.objectives.empty()) out.push_back("Add an objective");
    for (const Objective& o : d.objectives)
        if (o.kind == Objective::KillGroup && (o.group < 0 || o.group >= static_cast<int>(d.groups.size()))) { out.push_back("An objective kills a group that is gone"); break; }
    for (const Group& g : d.groups) if (g.monster.empty()) { out.push_back("A group has no unit"); break; }
    return out;
}

// What an unused ID block starts at: after the highest ID block the quests of this zone use (100 IDs each, from 1000000).
inline uint32_t NextIdBlock(const std::vector<uint32_t>& usedMax) {
    uint32_t top = 999999;
    for (uint32_t m : usedMax) top = std::max(top, m);
    return (top / 100 + 1) * 100;
}

} // namespace qdraft
