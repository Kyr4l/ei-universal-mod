// The "Units" gameplay database (units.udb), read from the same database RES archive as items.idb:
// the race models (which figure, which skin textures) and the monsters (a unit prototype's race, skin,
// hair and default equipment). The map editor dresses units with it, as the game does.
//
// Same tagged-value tree as items.idb (item_db.hpp): file -> block -> record -> field. Blocks: 1 hit
// locations, 2 race models, 3 monsters, 4 NPC. Field ids per EIDBEditor's dbheaders.txt ("[Units]"),
// checked against Universal-Mod's databaselmp.res ("Human Male" -> mask "unhuma").
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "item_db.hpp"
#include "res_archive.hpp"

namespace units {

struct Race {
    std::string name;                    // field 0, e.g. "Human Male"
    std::string mask;                    // field 30: the figure, e.g. "unhuma"
    std::vector<std::string> primary;    // field 31: texture choices ("Skin_00" = <mask>skin_00)
    std::vector<std::string> secondary;  // field 32
};

struct Monster {
    std::string name;                    // field 0: what a map unit's prototype names
    std::string race;                    // field 1: a Race's name
    int skin = 0;                        // field 3: <mask>skin_NN
    int hair = -1;                       // field 4: hr.NN, -1 none
    std::vector<std::string> wears;      // field 32: armors, "blueprint.material"
    std::string weapon1, weapon2;        // fields 33, 42
};

struct Database {
    std::vector<Race> races;
    std::vector<Monster> monsters;
    std::unordered_map<std::string, size_t> raceByName, monsterByName; // lower-case names

    const Race* FindRace(const std::string& name) const {
        auto it = raceByName.find(res::Archive::ToLower(name));
        return it == raceByName.end() ? nullptr : &races[it->second];
    }
    const Monster* FindMonster(const std::string& name) const {
        auto it = monsterByName.find(res::Archive::ToLower(name));
        return it == monsterByName.end() ? nullptr : &monsters[it->second];
    }
};

inline bool ParseUnitsUdb(const std::vector<uint8_t>& udb, Database& out) {
    const uint8_t* data = udb.data();
    size_t off = 0;
    items::Span file;
    if (!items::ReadTag(data, udb.size(), off, file)) return false;
    for (const items::Span& block : items::Children(data, file)) {
        if (block.tag != 2 && block.tag != 3) continue;
        for (const items::Span& record : items::Children(data, block)) {
            items::Fields f;
            f.data = data;
            for (const items::Span& field : items::Children(data, record)) f.byId[field.tag] = field;
            if (block.tag == 2) {
                Race r;
                r.name = f.String(0);
                r.mask = f.String(30);
                r.primary = f.Strings(31);
                r.secondary = f.Strings(32);
                if (r.name.empty()) continue;
                out.raceByName[res::Archive::ToLower(r.name)] = out.races.size();
                out.races.push_back(std::move(r));
            } else {
                Monster m;
                m.name = f.String(0);
                m.race = f.String(1);
                m.skin = f.Int(3);
                m.hair = f.Has(4) ? f.Int(4) : -1;
                m.wears = f.Strings(32);
                m.weapon1 = f.String(33);
                m.weapon2 = f.String(42);
                if (m.name.empty()) continue;
                out.monsterByName[res::Archive::ToLower(m.name)] = out.monsters.size();
                out.monsters.push_back(std::move(m));
            }
        }
    }
    return true;
}

} // namespace units
