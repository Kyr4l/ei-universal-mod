// The map checks: everything um.dll's MOB_VALIDATION reports when the game opens a map (so a problem
// is found while editing instead of in um.log), plus what only an editor can see.
//
// Same as um.dll (resources/universal-mod/um-dll/um.cpp, "MOBCHECK"):
//   - damaged files: node lengths past the end, corrupted object entries
//   - units' weapon/armor/spell/quick/quest item lists: truncated lists, bad entry lengths, blank
//     entries, names the item/spell database does not know
//   - the mission script, with the same checker (mob_script_check.hpp, shared with um.dll): syntax,
//     argument counts and types, undeclared variables, unknown commands, scripts never called (a base
//     map's scripts count as called when a map loaded after it calls them)...
//   - item/spell names the script gives to commands, against the database
//   - object IDs and names the script uses, against the objects of the loaded maps and of the maps
//     it loads with AddMob
//   - a quest map sharing object IDs with its base map
// Added here:
//   - maps are checked together, in load order: a map sees the variables, scripts and objects of the
//     maps loaded before it (the game loads a quest on top of its zone), and repeated IDs between any
//     two loaded maps are reported
//   - the same object ID used twice within one map
//   - figures and textures that the figure/texture sources do not have
//   - unit prototypes (for units that import their stats) and magic trap spells the database does not know
//   - objects outside the terrain (when an .mpr is loaded)
#pragma once

#include "db_model.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mob_script_check.hpp" // resources/universal-mod/um-dll, shared with um.dll
#include "mob_file.hpp"
#include "mpr_file.hpp"
#include "../viewer/asset_source.hpp"

namespace checks {

struct Finding {
    char severity = 'W';   // 'E' error, 'W' warning, 'I' info
    int file = -1;         // index into the checked maps
    int object = -1;       // index into that map's objects
    int line = 0;          // script line, when about the script
    std::string category;  // "Structure", "Items", "Script", "References", "IDs", "Assets", "Placement"
    std::string message;   // UTF-8
};

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------
// Database names, extracted the way um.dll does (ExtractDatabaseNames): every [tag][length][text\0]
// field of the database files, lower-cased. Loose on purpose: it only has to say whether a name
// exists anywhere in the database.
// ---------------------------------------------------------------------------------------------
inline void ExtractDatabaseNames(const std::vector<uint8_t>& data, std::unordered_set<std::string>& names) {
    const size_t size = data.size();
    for (size_t p = 0; p + 1 < size; ++p) {
        uint8_t lengthByte = data[p + 1];
        size_t payloadSize, payloadStart;
        if (lengthByte % 2 == 0) {
            payloadSize = lengthByte / 2;
            payloadStart = p + 2;
        } else {
            if (p + 5 > size) continue;
            uint32_t wide;
            std::memcpy(&wide, data.data() + p + 1, 4);
            if (wide % 2 == 0) continue;
            payloadSize = (wide - 1) / 2;
            payloadStart = p + 5;
        }
        if (payloadSize == 0 || payloadSize > 4096) continue;
        size_t payloadEnd = payloadStart + payloadSize;
        if (payloadEnd > size || data[payloadEnd - 1] != 0) continue;
        size_t j = payloadStart;
        while (j < payloadEnd - 1 && data[j] >= 0x20 && data[j] <= 0x7E) ++j;
        if (j > payloadStart) names.insert(Lower(std::string(reinterpret_cast<const char*>(data.data() + payloadStart), j - payloadStart)));
    }
}

// The configured database plus database.res / databaselmp.res / databaseadb.res beside it, like
// um.dll, which merges every database the game would load.
struct DatabaseNames {
    std::unordered_set<std::string> names;
    std::vector<std::string> files; // the ones read
    std::string loadedFor;

    void Load(const std::string& databasePath) {
        if (databasePath == loadedFor) return;
        loadedFor = databasePath;
        names.clear();
        files.clear();
        if (databasePath.empty()) return;
        // The other databases beside it, in the same form (.res, or spreadsheets compiled in memory).
        std::set<std::string> paths{databasePath};
        size_t slash = databasePath.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "" : databasePath.substr(0, slash + 1);
        size_t dot = databasePath.find_last_of('.');
        std::string ext = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? ".res" : Lower(databasePath.substr(dot));
        for (const char* n : {"database", "databaselmp", "databaseadb"}) paths.insert(dir + n + ext);
        for (const std::string& p : paths) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec)) continue;
            std::vector<uint8_t> bytes;
            std::string err;
            if (!dbmodel::ReadAsRes(p, bytes, err)) continue;
            ExtractDatabaseNames(bytes, names);
            files.push_back(p);
        }
    }
    bool Empty() const { return names.empty(); }
    bool Has(const std::string& lower) const { return names.count(lower) != 0; }
};

inline std::string StripBracket(const std::string& value) {
    size_t bracket = value.find('[');
    std::string r = bracket == std::string::npos ? value : value.substr(0, bracket);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
    return r;
}

// "template.material [annotation]"
inline bool KnownWeaponOrArmor(const std::string& name, const DatabaseNames& db) {
    std::string lower = Lower(name);
    size_t dot = lower.rfind('.');
    if (dot == std::string::npos) return db.Has(lower);
    return db.Has(lower.substr(0, dot)) && db.Has(StripBracket(lower.substr(dot + 1)));
}

// "spell{parameters}"
inline bool KnownSpell(const std::string& name, const DatabaseNames& db) {
    std::string lower = Lower(name);
    size_t brace = lower.find('{');
    return db.Has(brace == std::string::npos ? lower : lower.substr(0, brace));
}

inline bool KnownSimpleItem(const std::string& name, const DatabaseNames& db) {
    std::string lower = Lower(name);
    if (db.Has(lower) || db.Has(StripBracket(lower))) return true;
    return lower.find('.') != std::string::npos && KnownWeaponOrArmor(name, db);
}

// A text given to a script command: drop "{...}" and "[...]", then every dot-separated part must exist.
inline bool KnownScriptName(const std::string& raw, const DatabaseNames& db) {
    std::string text = Lower(raw);
    if (db.Has(text)) return true;
    size_t brace = text.find('{');
    if (brace != std::string::npos) text.resize(brace);
    size_t bracket = text.find('[');
    if (bracket != std::string::npos) text.resize(bracket);
    bool any = false;
    for (size_t start = 0; start <= text.size();) {
        size_t dot = text.find('.', start);
        std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) {
            any = true;
            if (!db.Has(part)) return false;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// Quest maps: z<zone>q<n>.mob with a z<zone>q<n>.mq beside it whose map.txt says, after "#res",
// "<terrain .mpr> <base map>".
// ---------------------------------------------------------------------------------------------
inline bool LooksLikeQuestMapName(const std::string& fileName) {
    std::string name = Lower(fileName);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.size() < 4 || name[0] != 'z' || name[1] < '0' || name[1] > '9') return false;
    size_t q = name.find_last_of('q');
    if (q == std::string::npos || q < 2 || q + 1 >= name.size()) return false;
    for (size_t i = q + 1; i < name.size(); ++i) if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

inline std::string DirectoryOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// A file named inside a map (a base map, an AddMob target) in `dir`, whatever its case: the game runs on
// Windows, where "zone17-LMP.mob" is zone17-lmp.mob. Empty when there is none.
inline std::string FindInDirectory(const std::string& dir, const std::string& name) {
    std::error_code ec;
    std::filesystem::path base = dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir);
    if (std::filesystem::exists(base / name, ec)) return (base / name).string();
    const std::string lower = Lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (Lower(entry.path().filename().string()) == lower) return entry.path().string();
    }
    return std::string();
}

struct QuestInfo { bool found = false; std::string terrain, baseMap; };

inline QuestInfo ReadQuestArchive(const std::string& mobPath) {
    QuestInfo info;
    size_t dot = mobPath.find_last_of('.');
    if (dot == std::string::npos) return info;
    std::string stem = mobPath.substr(0, dot);
    std::string mq = FindInDirectory(DirectoryOf(mobPath), stem.substr(DirectoryOf(mobPath).size()) + ".mq");
    if (mq.empty()) return info;
    std::ifstream f(mq, std::ios::binary);
    if (!f.is_open()) return info;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    res::Archive archive;
    std::string err;
    if (!res::ParseArchive(bytes, archive, err)) return info;
    for (const auto& e : archive.entries) {
        if (e.first.size() < 7 || e.first.compare(e.first.size() - 7, 7, "map.txt") != 0) continue;
        std::string text(e.second.data.begin(), e.second.data.end());
        bool afterRes = false;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            if (afterRes) {
                size_t space = line.find_first_of(" \t");
                if (space == std::string::npos) return info;
                info.terrain = line.substr(0, space);
                size_t second = line.find_first_not_of(" \t", space);
                if (second == std::string::npos) return info;
                size_t secondEnd = line.find_first_of(" \t", second);
                info.baseMap = line.substr(second, secondEnd == std::string::npos ? std::string::npos : secondEnd - second);
                info.found = !info.baseMap.empty();
                return info;
            }
            if (line.size() >= 4 && Lower(line.substr(0, 4)) == "#res") afterRes = true;
        }
    }
    return info;
}

inline std::string WithMobExtension(std::string name) {
    if (name.size() < 4 || Lower(name.substr(name.size() - 4)) != ".mob") name += ".mob";
    return name;
}

// What a map offers the maps checked after it (or a map read from disk for the check only).
struct MapContext {
    MobScriptDeclarations declarations;
    std::unordered_set<uint32_t> ids;
    std::unordered_set<std::string> names; // lower-case
    std::vector<std::string> addMobs;
};

inline void AddObjects(const mob::File& f, MapContext& ctx) {
    for (const mob::Object& o : f.objects) {
        if (o.kind == mob::Kind::Light || o.kind == mob::Kind::Particle || o.kind == mob::Kind::Sound) continue; // not objects to scripts
        if (o.hasId) ctx.ids.insert(o.id);
        if (!o.name.empty()) ctx.names.insert(Lower(o.name));
    }
}

inline std::shared_ptr<MapContext> ReadContextFromDisk(const std::string& dir, const std::string& name) {
    std::string path = FindInDirectory(dir, name);
    mob::File f;
    if (path.empty() || !mob::Load(path, f)) return nullptr;
    auto ctx = std::make_shared<MapContext>();
    AddObjects(f, *ctx);
    if (!f.script.empty()) {
        MobScriptReport r = CheckMobScript(f.script);
        ctx->declarations = r.declarations;
        for (const std::string& a : r.addMobs) ctx->addMobs.push_back(Lower(WithMobExtension(a)));
    }
    return ctx;
}

inline void MergeDeclarations(MobScriptDeclarations& into, const MobScriptDeclarations& from) {
    into.globals.insert(from.globals.begin(), from.globals.end());
    into.scripts.insert(from.scripts.begin(), from.scripts.end());
    into.defined.insert(from.defined.begin(), from.defined.end());
}

struct Inputs {
    std::vector<const mob::File*> maps;   // in load order
    const mpr::Map* terrain = nullptr;    // may be null
    const LayeredAssetSource* figures = nullptr;
    const LayeredAssetSource* textures = nullptr;
    const DatabaseNames* database = nullptr;
    // Settings > Checks: off, the script's findings (and the objects it names) are not reported, or nothing
    // is checked against the database - for games with their own commands or databases.
    bool scriptChecks = true;
    bool databaseChecks = true;
};

struct Summary { int errors = 0, warnings = 0, infos = 0; };

inline std::string Label(const mob::Object& o) {
    std::string s = std::string(mob::KindName(o.kind));
    if (!o.name.empty()) s += " '" + mob::Utf8(o.name) + "'";
    if (o.hasId) s += " (ID " + std::to_string(o.id) + ")";
    return s;
}

inline std::vector<Finding> Run(const Inputs& in, Summary* summary = nullptr) {
    std::vector<Finding> out;
    auto add = [&](char sev, int file, int object, int line, const char* cat, const std::string& msg) {
        if (!in.scriptChecks && (std::strcmp(cat, "Script") == 0 || std::strcmp(cat, "References") == 0)) return;
        out.push_back({sev, file, object, line, cat, msg});
    };
    const DatabaseNames* db = in.databaseChecks && in.database && !in.database->Empty() ? in.database : nullptr;
    struct Uncalled { int file; std::string name; int line; };
    std::vector<Uncalled> baseUncalled;                       // a base map's scripts nothing in it calls: a later map may
    std::vector<std::unordered_set<std::string>> calledBy(in.maps.size()); // per file: the scripts it calls (lower-case)
    std::vector<bool> hasScriptFile(in.maps.size(), false);

    MapContext loadedBefore; // everything the maps before the current one offer
    std::map<uint32_t, std::pair<int, int>> idOwner; // ID -> (file, object) of its first use across loaded maps

    for (int fi = 0; fi < static_cast<int>(in.maps.size()); ++fi) {
        const mob::File& f = *in.maps[fi];
        if (!f.loaded) { add('E', fi, -1, 0, "Structure", f.error); continue; }
        for (const mob::Issue& issue : f.structure) add(issue.severity, fi, -1, 0, "Structure", issue.message);

        // --- units' item lists
        for (int oi = 0; oi < static_cast<int>(f.objects.size()); ++oi) {
            const mob::Object& o = f.objects[oi];
            for (const mob::ItemList& list : o.lists) {
                if (list.truncated)
                    add('E', fi, oi, 0, "Items", Label(o) + " has a truncated " + list.label + " list (expected " + std::to_string(list.declaredCount) + " entries)");
                if (list.badLength)
                    add('E', fi, oi, 0, "Items", Label(o) + " has a " + list.label + " entry with an invalid length " + std::to_string(list.badLengthValue));
                for (size_t i = 0; i < list.entries.size(); ++i) {
                    const std::string& e = list.entries[i];
                    if (e.empty()) {
                        add('E', fi, oi, 0, "Items", Label(o) + " has a blank entry (#" + std::to_string(i + 1) + " of " +
                                                          std::to_string(list.declaredCount) + ") in its " + list.label + " list");
                        continue;
                    }
                    if (!db) continue;
                    bool known = list.type == mob::kUnitWeapons || list.type == mob::kUnitArmors ? KnownWeaponOrArmor(e, *db)
                               : list.type == mob::kUnitSpells ? KnownSpell(e, *db) : KnownSimpleItem(e, *db);
                    if (!known) add('E', fi, oi, 0, "Items", Label(o) + " has a " + list.label + " '" + mob::Utf8(e) + "' that the database does not have");
                }
            }
            // Only when the unit takes its stats from the prototype (UNIT_NEED_IMPORT): most units carry their
            // own, and thousands of units in the shipped maps name prototypes no database has.
            if (db && o.kind == mob::Kind::Unit && o.needImport && !o.prototype.empty() && !db->Has(Lower(o.prototype)))
                add('W', fi, oi, 0, "Items", Label(o) + " imports its stats from the unit prototype '" + mob::Utf8(o.prototype) +
                                                  "', which the database does not have");
            if (db && o.kind == mob::Kind::MagicTrap && !o.spell.empty() && !KnownSpell(o.spell, *db))
                add('E', fi, oi, 0, "Items", Label(o) + " casts '" + mob::Utf8(o.spell) + "', which is not a spell in the database");
        }

        // --- IDs: repeated within this map, and between loaded maps
        MapContext own;
        AddObjects(f, own);
        {
            std::unordered_map<uint32_t, int> firstInFile;
            std::map<int, std::vector<uint32_t>> sharedWith; // earlier file -> IDs
            for (int oi = 0; oi < static_cast<int>(f.objects.size()); ++oi) {
                const mob::Object& o = f.objects[oi];
                if (!o.hasId || !mob::HasFigure(o.kind)) continue;
                auto first = firstInFile.find(o.id);
                if (first != firstInFile.end()) {
                    add('E', fi, oi, 0, "IDs", Label(o) + " has the same ID as " + Label(f.objects[first->second]) +
                                                   " in this map; scripts and the game see only one of them");
                    continue;
                }
                firstInFile[o.id] = oi;
                auto owner = idOwner.find(o.id);
                if (owner != idOwner.end() && owner->second.first != fi) sharedWith[owner->second.first].push_back(o.id);
                else idOwner[o.id] = {fi, oi};
            }
            for (auto& kv : sharedWith) {
                std::string examples;
                for (size_t i = 0; i < kv.second.size() && i < 5; ++i) examples += (i ? ", " : "") + std::to_string(kv.second[i]);
                add('W', fi, -1, 0, "IDs", "shares " + std::to_string(kv.second.size()) + " object ID(s) with " + in.maps[kv.first]->fileName +
                                               " (e.g. " + examples + "); loaded on top of it, this map's object replaces the other one");
            }
        }

        // --- figures and textures
        for (int oi = 0; oi < static_cast<int>(f.objects.size()); ++oi) {
            const mob::Object& o = f.objects[oi];
            if (!mob::HasModel(o.kind)) continue;
            if (in.figures && in.figures->AnyLoaded() && !o.templ.empty()) {
                const std::string t = o.templ;
                if (!in.figures->Contains(t + ".mod") && !in.figures->Contains(t + ".fig") && !in.figures->Contains(t + ".lnk"))
                    add('E', fi, oi, 0, "Assets", Label(o) + " uses the figure '" + mob::Utf8(t) + "', which the figure sources do not have");
            }
            if (in.textures && in.textures->AnyLoaded() && !o.primTexture.empty()) {
                const std::string t = o.primTexture;
                if (!in.textures->Contains(t + ".mmp") && !in.textures->Contains(t + ".dds"))
                    add('W', fi, oi, 0, "Assets", Label(o) + " uses the texture '" + mob::Utf8(t) + "', which the texture sources do not have");
            }
        }

        // --- placement
        if (in.terrain) {
            const float w = in.terrain->Width(), h = in.terrain->Height();
            for (int oi = 0; oi < static_cast<int>(f.objects.size()); ++oi) {
                const mob::Object& o = f.objects[oi];
                if (o.position.x < 0 || o.position.y < 0 || o.position.x > w || o.position.y > h) {
                    char where[96];
                    std::snprintf(where, sizeof(where), "(%.1f, %.1f)", o.position.x, o.position.y);
                    add('W', fi, oi, 0, "Placement", Label(o) + " is outside the terrain " + where + "; the terrain is " +
                                                         std::to_string(static_cast<int>(w)) + " x " + std::to_string(static_cast<int>(h)));
                }
            }
        }

        // --- the script
        std::shared_ptr<MapContext> base;       // a quest's base map named by its .mq, when not loaded here
        bool baseMissing = false;
        QuestInfo quest = ReadQuestArchive(f.path);
        if (quest.found) {
            bool loaded = false;
            for (int k = 0; k < fi; ++k) if (Lower(in.maps[k]->fileName) == Lower(WithMobExtension(quest.baseMap))) loaded = true;
            if (!loaded) {
                base = ReadContextFromDisk(DirectoryOf(f.path), WithMobExtension(quest.baseMap));
                if (!base) {
                    baseMissing = fi == 0;
                    if (fi == 0) add('I', fi, -1, 0, "Script", "a quest map for the base map '" + quest.baseMap +
                                                             "', which is neither loaded nor next to it: variables it takes from there are not checked");
                }
            }
        } else if (fi == 0 && LooksLikeQuestMapName(f.fileName)) {
            baseMissing = true;
            add('I', fi, -1, 0, "Script", "looks like a quest map, but no base map is loaded before it: load the zone's map first to check what it uses from there");
        }

        MapContext visible = loadedBefore; // what this map's script can see besides its own
        if (base) {
            MergeDeclarations(visible.declarations, base->declarations);
            visible.ids.insert(base->ids.begin(), base->ids.end());
            visible.names.insert(base->names.begin(), base->names.end());
        }
        MobScriptReport report;
        if (f.hasScript && !f.script.empty()) {
            const bool haveBase = fi > 0 || base;
            report = CheckMobScript(f.script, haveBase ? &visible.declarations : nullptr, baseMissing);
            for (const MobScriptIssue& issue : report.issues) add(issue.severity, fi, -1, issue.line, "Script", issue.message);
            if (report.suppressed > 0) add('W', fi, -1, 0, "Script", std::to_string(report.suppressed) + " more script finding(s) not shown");
            calledBy[static_cast<size_t>(fi)] = report.called;
            hasScriptFile[static_cast<size_t>(fi)] = true;
            const bool isQuest = quest.found || LooksLikeQuestMapName(f.fileName);
            for (const auto& u : report.uncalled) {
                if (isQuest) add('W', fi, -1, u.second, "Script", "script '" + u.first + "' is declared but never called");
                else baseUncalled.push_back({fi, u.first, u.second}); // decided once the maps on top are read
            }

            if (db) {
                std::unordered_set<std::string> reported;
                for (const MobScriptDatabaseName& ref : report.databaseNames) {
                    if (KnownScriptName(ref.text, *db) || !reported.insert(Lower(ref.text)).second) continue;
                    add('W', fi, -1, ref.line, "Script", ref.command + "(): '" + mob::Utf8(ref.text) + "' is not " +
                                                             (ref.kind == 's' ? "a spell" : "an item") + " in the database");
                }
            }

            // Object IDs and names, against every loaded map, the base and the AddMob maps.
            bool resolvable = !baseMissing;
            MapContext all = visible;
            all.ids.insert(own.ids.begin(), own.ids.end());
            all.names.insert(own.names.begin(), own.names.end());
            for (int k = fi + 1; k < static_cast<int>(in.maps.size()); ++k) AddObjects(*in.maps[k], all); // loaded on top too
            for (const std::string& target : report.addMobs) {
                std::string name = Lower(WithMobExtension(target));
                bool loaded = false;
                for (const mob::File* m : in.maps) if (Lower(m->fileName) == name) loaded = true;
                if (loaded) continue;
                auto added = ReadContextFromDisk(DirectoryOf(f.path), WithMobExtension(target));
                if (!added) {
                    add('I', fi, -1, 0, "References", "loads '" + target + "' with AddMob, which is neither loaded nor next to it: "
                                                        "the object IDs and names of its script are not checked");
                    resolvable = false;
                    break;
                }
                all.ids.insert(added->ids.begin(), added->ids.end());
                all.names.insert(added->names.begin(), added->names.end());
            }
            if (resolvable) {
                std::set<std::string> seen;
                for (const MobScriptReference& ref : report.objectIds) {
                    unsigned long id = std::strtoul(ref.text.c_str(), nullptr, 10);
                    if (ref.text.size() > 10 || all.ids.count(static_cast<uint32_t>(id)) || !seen.insert(ref.text).second) continue;
                    add('W', fi, -1, ref.line, "References", "the script uses the object ID " + ref.text + ", which no loaded object has");
                }
                seen.clear();
                for (const MobScriptReference& ref : report.objectNames) {
                    if (all.names.count(Lower(ref.text)) || !seen.insert(Lower(ref.text)).second) continue;
                    add('W', fi, -1, ref.line, "References", "the script uses the object name '" + mob::Utf8(ref.text) + "', which no loaded object has");
                }
            }
        }

        // What the maps after this one see.
        MergeDeclarations(loadedBefore.declarations, report.declarations);
        if (base) MergeDeclarations(loadedBefore.declarations, base->declarations);
        loadedBefore.ids.insert(own.ids.begin(), own.ids.end());
        loadedBefore.names.insert(own.names.begin(), own.names.end());
    }

    for (const Uncalled& u : baseUncalled) {
        const std::string lowered = Lower(u.name);
        bool called = false, later = false;
        std::string names;
        for (size_t k = static_cast<size_t>(u.file) + 1; k < in.maps.size(); ++k) {
            if (!hasScriptFile[k]) continue;
            later = true;
            names += (names.empty() ? "" : ", ") + in.maps[k]->fileName;
            called |= calledBy[k].count(lowered) != 0;
        }
        if (called) continue;
        if (later) add('W', u.file, -1, u.line, "Script", "script '" + u.name + "' is declared but never called, not here nor by " + names);
        else add('I', u.file, -1, u.line, "Script", "script '" + u.name + "' is never called in this file (load the zone's quest maps on top of it to see whether one calls it)");
    }

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        if (a.file != b.file) return a.file < b.file;
        auto rank = [](char s) { return s == 'E' ? 0 : s == 'W' ? 1 : 2; };
        if (rank(a.severity) != rank(b.severity)) return rank(a.severity) < rank(b.severity);
        return a.line < b.line;
    });
    if (summary) {
        *summary = {};
        for (const Finding& f : out) (f.severity == 'E' ? summary->errors : f.severity == 'W' ? summary->warnings : summary->infos)++;
    }
    return out;
}

} // namespace checks
