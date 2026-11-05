// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Item stats of a character from the database: a weapon or armour keeps the stats it was made with (its detail block in the
// .mp / .sav), so a database change (damage x2, new durability...) reaches only new items. Update recomputes each owned
// weapon and armour the way the game builds them (CItemConstructor::ApplyProto 0x6A5D60, docs/game-exe/items.md section 3):
//   durability max = P.Durability * M.Durability, energy max = P.Mana + M.Mana, weight = P.Weight * M.Weight,
//   price = P.Price + P.Components * M.Price (+ the attached spell's part, kept), size = P.Size, slots = P.Slots + M.Slots,
//   weapon damage min / spread = P.D.Min / P.D.Max * M.Damage, range = P.Range,
//   armour main[k] = P.M[k] * M.A[k] * P.M.Absorb, add[k] = P.A[k] * M.A[k] * P.A.Absorb.
// Wear and spent energy keep their share of the max. The hero's own armour values need no change: the game re-equips the
// worn armour when it loads the hero (CWorldClient::CreateHeroUnits 0x566F70 -> EquipArmor).
//   - LoadDatabase / FromBook: the four sheets' columns it needs;
//   - Update: what would change (apply = false) or change it;
//   - RunMpStats: the "mpstats" command line.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db_model.hpp"
#include "mp_file.hpp"

namespace mpstats {

struct Row { std::string name; std::vector<float> v; bool ok = false; }; // v in the order of the sheet's column list
struct Database {
    std::vector<Row> materials, weapons, armors;
    bool Empty() const { return materials.empty(); }
};
bool FromBook(const dbmodel::Book& book, Database& db, std::string& err);
bool LoadDatabase(const std::string& path, Database& db, std::string& err);

struct Field { std::string name, before, after; };
struct Item {
    uint32_t id = 0;
    std::string name;            // "battle axe (corundum)"
    std::vector<Field> fields;   // what changes (empty: already up to date)
    std::string skipped;         // why it was left alone (row or material not in the database)
};
// Every weapon and armour of the character; apply = true writes the new values into its blocks.
std::vector<Item> Update(mp::Character& c, const Database& db, bool apply);
// Counts of a result: items that change, fields that change, items skipped.
struct Counts { int items = 0, fields = 0, skipped = 0; };
Counts Count(const std::vector<Item>& items);

int RunMpStats(int argc, char* argv[]);

} // namespace mpstats
