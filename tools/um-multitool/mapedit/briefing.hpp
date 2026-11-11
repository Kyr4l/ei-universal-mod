// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// A quest briefing ("briefing <topic>" of a .mq) read the way game.exe's dialog screen reads it (DialogScreen::ParseDialogScript
// 0x604C50, SelectAnswer 0x6064E0; docs/game-exe/ui-screens.md 3): the phrases as the game shows them, and what goes wrong.
//   #show <name> [index] [weapon]   a live unit named so joins the talk; any other name is an NPC portrait and needs an index
//   #hide <name>, #nolips, #animation <n>, #camera <shot 1..12> [yaw distance height]
//   #phrase <name> [voice]          the text is the following lines up to the next '#'
// The first line (before any '#') is the title. The game opens the talk only when a #show names a live unit other than the hero.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace briefing {

struct Phrase { std::string speaker, text, camera; int voice = 0; bool nolips = false; int line = 0; };
struct Issue { char severity; int line; std::string message; };
struct Parsed {
    std::string title;
    std::vector<std::string> shown;      // the #show names, in order
    std::vector<Phrase> phrases;
    std::vector<Issue> issues;
};

inline std::string Low(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

// unitNames: the names of the units of the loaded maps (lower-case); empty: the "a live unit" test is skipped.
inline Parsed Parse(const std::string& text, const std::set<std::string>& unitNames) {
    Parsed p;
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        std::string l = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        lines.push_back(l);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::string camera;
    bool nolips = false, firstPhrase = true, anyUnit = false, titled = false;
    std::set<std::string> speakers{"hero"};
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        const int ln = static_cast<int>(i) + 1;
        if (l.empty()) continue;
        if (l[0] != '#') { if (!titled && p.phrases.empty()) { p.title = l; titled = true; } continue; }
        std::vector<std::string> w;
        { size_t s = 0; while (s < l.size()) { while (s < l.size() && (l[s] == ' ' || l[s] == '\t')) ++s; size_t e = s; while (e < l.size() && l[e] != ' ' && l[e] != '\t') ++e; if (e > s) w.push_back(l.substr(s, e - s)); s = e; } }
        const std::string key = Low(w[0]);
        if (key == "#show") {
            if (w.size() < 2) { p.issues.push_back({'W', ln, "#show without a name"}); continue; }
            p.shown.push_back(w[1]);
            speakers.insert(Low(w[1]));
            const bool unit = unitNames.count(Low(w[1])) > 0;
            if (unit && Low(w[1]) != "hero") anyUnit = true;
            if (!unit && Low(w[1]) != "hero" && !unitNames.empty() && w.size() < 3)
                p.issues.push_back({'W', ln, "#show " + w[1] + ": no unit of the loaded maps has this name, and as an NPC portrait it needs an index (#show " + w[1] + " 1): the game ignores it"});
            if (w.size() >= 4) p.issues.push_back({'I', ln, "#show's third word (" + w[3] + ") is never read by the game"});
        } else if (key == "#camera") {
            const int shot = w.size() >= 2 ? std::atoi(w[1].c_str()) : 0;
            if (w.size() >= 5) camera = "shot " + w[1] + ", yaw " + w[2] + ", distance " + w[3] + ", height " + w[4];
            else {
                camera = "shot " + (w.size() >= 2 ? w[1] : std::string("?"));
                if (shot < 1 || shot > 12) p.issues.push_back({'W', ln, "#camera " + (w.size() >= 2 ? w[1] : std::string()) + ": the shots are 1 to 12 (other values keep the default shot)"});
                if (w.size() == 3 || w.size() == 4) p.issues.push_back({'W', ln, "#camera takes a shot alone, or a shot with yaw, distance and height: the extra numbers are ignored"});
            }
        } else if (key == "#nolips") nolips = true;
        else if (key == "#hide" || key == "#animation") {}
        else if (key == "#phrase") {
            Phrase ph;
            ph.line = ln;
            ph.speaker = w.size() >= 2 ? w[1] : std::string();
            ph.voice = w.size() >= 3 ? std::atoi(w[2].c_str()) : 0;
            ph.nolips = nolips;
            ph.camera = camera.empty() ? (firstPhrase ? "shot 1 (both)" : Low(ph.speaker) == "hero" ? "shot 2 (the hero)" : "shot 3 (the speaker)") : camera;
            if (ph.speaker.empty()) p.issues.push_back({'W', ln, "#phrase without a speaker"});
            else if (!speakers.count(Low(ph.speaker))) p.issues.push_back({'W', ln, "#phrase " + ph.speaker + ": not shown by a #show before it (the label is the raw name and nobody moves)"});
            size_t j = i + 1;
            for (; j < lines.size() && (lines[j].empty() || lines[j][0] != '#'); ++j) { if (!lines[j].empty()) ph.text += (ph.text.empty() ? "" : " ") + lines[j]; }
            if (ph.text.empty()) p.issues.push_back({'W', ln, "#phrase " + ph.speaker + " has no text"});
            p.phrases.push_back(ph);
            i = j - 1;
            camera.clear();
            nolips = false;
            firstPhrase = false;
        } else p.issues.push_back({'I', ln, w[0] + " is not a directive of the game's dialog screen (ignored)"});
    }
    if (!unitNames.empty() && !anyUnit)
        p.issues.push_back({'E', 1, "no #show names a unit of the loaded maps besides the hero: the game does not open this briefing at all"});
    if (p.phrases.empty()) p.issues.push_back({'W', 1, "no #phrase: the briefing shows no text"});
    return p;
}

} // namespace briefing
