// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// `um-multitool map --rate`: the difficulty rating (rating.hpp) of the quests of the Settings' quest folders (or of
// the ones named), with the allod each belongs to (allods.hpp). The same numbers the Map Editor shows.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>

#include "allods.hpp"
#include "checks.hpp"
#include "mob_file.hpp"
#include "quest_file.hpp"
#include "rating.hpp"
#include "../subtools.hpp"
#include "../viewer/config.hpp"

namespace mapedit {

namespace {

// A map file by name: the Settings' map folders (the last one wins), else next to the quest.
std::string FindMapFile(const std::vector<std::string>& mapLayers, const std::string& name, const std::string& questPath) {
    for (auto it = mapLayers.rbegin(); it != mapLayers.rend(); ++it) {
        const std::string p = checks::FindInDirectory(*it, name);
        if (!p.empty()) return p;
    }
    std::error_code ec;
    std::filesystem::path p(questPath);
    const std::string dir = std::filesystem::is_directory(p, ec) ? p.parent_path().string() : checks::DirectoryOf(questPath);
    return checks::FindInDirectory(dir, name);
}

} // namespace

int RunRateCli(const std::vector<std::string>& args) {
    std::string configPath = config::Path(), databasePath, allodFile;
    std::vector<std::string> questArgs;
    bool detail = false, units = false;
    rating::Params params;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--rate") continue;
        else if (args[i] == "--config" && i + 1 < args.size()) configPath = args[++i];
        else if (args[i] == "--database" && i + 1 < args.size()) databasePath = args[++i];
        else if (args[i] == "--allods" && i + 1 < args.size()) allodFile = args[++i];
        else if (args[i] == "--detail") detail = true;
        else if (args[i] == "--map-stats") params.storedStats = true;
        else if (args[i] == "--units") units = true;
        else questArgs.push_back(args[i]);
    }
    config::Config cfg = config::Load(configPath);
    if (databasePath.empty()) databasePath = cfg.databasePath;
    if (databasePath.empty()) { std::fprintf(stderr, "no database: set it in Settings or pass --database <file>\n"); return 2; }

    dbmodel::Book book;
    std::string err;
    if (!dbmodel::LoadBook(databasePath, book, err)) { std::fprintf(stderr, "%s: %s\n", databasePath.c_str(), err.c_str()); return 2; }
    rating::Monsters monsters;
    if (!rating::LoadMonsters(book, params, monsters, err)) { std::fprintf(stderr, "%s: %s\n", databasePath.c_str(), err.c_str()); return 2; }
    std::printf("database %s: %zu monsters, strongest %s (power %.0f), reference hero %s\n", databasePath.c_str(), monsters.byName.size(),
                monsters.anchorName.c_str(), monsters.anchorUnit, monsters.haveHero ? "found" : "missing");
    if (units) { // the strongest units, to see what the anchor is made of
        std::vector<std::pair<float, std::string>> v;
        for (const auto& kv : monsters.byName) v.push_back({rating::PowerOf(kv.second, monsters, params).power, kv.second.name});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(12, v.size()); ++i) std::printf("  %8.1f  %s\n", v[i].first, v[i].second.c_str());
    }

    allods::Table table;
    if (!allodFile.empty()) {
        std::string folder = std::filesystem::path(allodFile).parent_path().string();
        if (folder.empty()) folder = ".";
        allods::LoadFromFolder(folder, table, &IniRegRegToText);
    } else {
        std::vector<std::string> hints{databasePath};
        for (const std::string& f : cfg.mapLayers) hints.push_back(f);
        for (const std::string& f : cfg.questFolders) hints.push_back(f);
        allods::FindAndLoad(hints, table, &IniRegRegToText);
    }
    if (table.Empty()) std::printf("allods: no [MPZone] section found (pass --allods <config.ini>)\n");
    else std::printf("allods from %s: %zu\n", table.source.c_str(), table.allods.size());

    std::vector<quest::Quest> quests;
    // The quests are the same in every language pack: the first pack stands in when no quest folder is set.
    std::vector<std::string> sources = questArgs.empty() ? cfg.questFolders : questArgs;
    if (sources.empty() && !cfg.questPacks.empty()) sources.push_back(cfg.questPacks[0]);
    for (const std::string& a : sources)
        for (quest::Quest& q : quest::ScanFolder(a)) quests.push_back(std::move(q));
    std::sort(quests.begin(), quests.end(), [&](const quest::Quest& a, const quest::Quest& b) {
        const int x = table.Find(a.terrain), y = table.Find(b.terrain);
        return x != y ? (x < 0 ? 99 : x) < (y < 0 ? 99 : y) : a.name < b.name;
    });
    std::printf("%-12s %-10s %-10s %6s | %6s %6s %6s %6s %6s | %5s %5s %5s %6s %4s %5s %4s %6s %6s\n", "quest", "zone", "allod", "score", "power", "dens", "stlth", "durat", "ambsh",
                "units", "pulls", "fight", "route", "exp%", "spawn", "obj", "sumN", "peak");
    for (const quest::Quest& q : quests) {
        const std::string terrainPath = q.terrain.empty() ? std::string() : FindMapFile(cfg.mapLayers, q.terrain + ".mpr", q.path);
        (void)terrainPath;
        std::vector<mob::File> files(2);
        rating::Input in;
        const std::string base = q.baseMap.empty() ? std::string() : FindMapFile(cfg.mapLayers, q.baseMap + ".mob", q.path);
        const std::string own = FindMapFile(cfg.mapLayers, q.name + ".mob", q.path);
        size_t n = 0;
        if (!base.empty() && mob::Load(base, files[n])) { in.files.push_back(&files[n++]); in.baseCount = 1; }
        if (!own.empty() && mob::Load(own, files[n])) in.files.push_back(&files[n++]);
        if (!q.exits.empty()) { in.deploy = q.exits[0].deploy; in.remove = q.exits[0].remove; }
        const rating::Result r = rating::Rate(in, monsters, params);
        const std::string allod = table.NameOf(q.terrain);
        if (!r.ok) { std::printf("%-12s %-10s %-10s  not rated: %s\n", q.name.c_str(), q.terrain.c_str(), allod.empty() ? "-" : allod.c_str(), r.note.c_str()); continue; }
        std::printf("%-12s %-10s %-10s %6.0f | %6.0f %6.0f %6.0f %6.0f %6.0f | %5d %5zu %5d %6.0f %3.0f%% %5d %4d %6.3f %6.3f%s\n", q.name.c_str(), q.terrain.c_str(),
                    allod.empty() ? "-" : allod.c_str(), r.total, r.pts[0], r.pts[1], r.pts[2], r.pts[3], r.pts[4], r.units, r.pulls.size(), r.fights, r.routeLength,
                    r.exposure * 100, r.spawnSites, r.objectives, r.sumNorm, r.peakNorm, r.unknown ? ("  (" + std::to_string(r.unknown) + " units without stats)").c_str() : "");
        if (detail) {
            std::printf("      stored stats used by %d of %d units\n", r.storedStats, r.units);
            std::printf("      maps:");
            for (const auto& f : r.files) std::printf(" %s (%d enemies)", f.first.c_str(), f.second);
            std::printf("\n");
            for (const rating::Pull& p : r.pulls) {
                std::printf("      pull at %.0f,%.0f  %d units  norm %.3f  ", p.x, p.y, p.units, p.norm);
                for (const std::string& s : p.names) std::printf("%s ", s.c_str());
                std::printf("\n");
            }
            for (const std::string& u : r.unknownNames) std::printf("      no stats: %s\n", u.c_str());
        }
    }
    return 0;
}

} // namespace mapedit
