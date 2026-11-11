// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The stored difficulty ratings of the quests (rating.hpp), and the allod ranges a user locked. A rating is kept with a
// "stamp": the formula version, the database's size and date, and the size and date of each file the rating read (the
// quest's map.txt or .mq, its base map and its own map). When any of them differs the rating is "changed": its score is
// not shown and a rescan is suggested. Rewards and texts are not part of the stamp: they do not change the difficulty.
//
// File (UserDir()/um-multitool-ratings.tsv), one record per line, fields separated by tabs:
//   R  quest  score  p0 p1 p2 p3 p4  units  fights  spawns  objectives  route  exposure  allod  stamp  rated
//   L  allod  min  max                       a locked allod range
//   O  storedstats  0|1                      the option "use the stats stored in the map" (default 0: the database's rows)
#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ratingstore {

struct Entry {
    double score = 0;
    std::array<double, 5> pts{};
    int units = 0, fights = 0, spawns = 0, objectives = 0;
    float route = 0, exposure = 0;
    std::string allod, stamp, rated;
};

struct Lock { double lo = 0, hi = 0; };

inline std::string Key(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct Store {
    std::map<std::string, Entry> entries; // by Key(quest name)
    std::map<std::string, Lock> locks;    // by Key(allod name)
    bool storedStats = false;             // O storedstats: units with "Stats imported" off are rated with the stats stored in the map (default: database rows)

    bool Load(const std::string& path) {
        entries.clear();
        locks.clear();
        storedStats = false;
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '\t')) f.push_back(part);
            if (f.size() >= 3 && f[0] == "O" && f[1] == "storedstats") { storedStats = f[2] == "1"; continue; }
            if (f.size() >= 4 && f[0] == "L") { locks[Key(f[1])] = {std::atof(f[2].c_str()), std::atof(f[3].c_str())}; continue; }
            if (f.size() < 17 || f[0] != "R") continue;
            Entry e;
            e.score = std::atof(f[2].c_str());
            for (size_t i = 0; i < 5; ++i) e.pts[i] = std::atof(f[3 + i].c_str());
            e.units = std::atoi(f[8].c_str()); e.fights = std::atoi(f[9].c_str()); e.spawns = std::atoi(f[10].c_str()); e.objectives = std::atoi(f[11].c_str());
            e.route = static_cast<float>(std::atof(f[12].c_str())); e.exposure = static_cast<float>(std::atof(f[13].c_str()));
            e.allod = f[14]; e.stamp = f[15]; e.rated = f[16];
            entries[Key(f[1])] = e;
        }
        return true;
    }

    bool Save(const std::string& path) const {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return false;
        out << "# um-multitool quest ratings (Map Editor > Files). Safe to delete: ratings are calculated again.\n";
        char b[64];
        for (const auto& kv : entries) {
            const Entry& e = kv.second;
            out << "R\t" << kv.first << '\t';
            std::snprintf(b, sizeof b, "%.1f", e.score); out << b;
            for (double p : e.pts) { std::snprintf(b, sizeof b, "\t%.1f", p); out << b; }
            out << '\t' << e.units << '\t' << e.fights << '\t' << e.spawns << '\t' << e.objectives;
            std::snprintf(b, sizeof b, "\t%.1f\t%.4f", e.route, e.exposure); out << b;
            out << '\t' << e.allod << '\t' << e.stamp << '\t' << e.rated << '\n';
        }
        out << "O\tstoredstats\t" << (storedStats ? 1 : 0) << '\n';
        for (const auto& kv : locks) out << "L\t" << kv.first << '\t' << kv.second.lo << '\t' << kv.second.hi << '\n';
        return static_cast<bool>(out);
    }
};

} // namespace ratingstore
