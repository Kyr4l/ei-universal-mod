// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Difficulty rating of a quest or a .mob: one score from 0 to 10000, the sum of five factors that only look at the
// enemies and the mission script (never at what the player buys or carries: potions, gear). The scale is anchored to
// the mod's own database: 0 is an empty map; 10000 is the hardest encounter the database can field (8 of its strongest
// unit standing together). A different database therefore moves every score, which is why a rating is stored with the
// database's date and the formula version (ratings_store.hpp).
//
//   Enemy power   the pulls' strength: a pull is units that wake each other (call-for-help radius, per unit), its power
//                 sqrt(sum effective HP * sum damage per second); effective HP counts absorption, damage per second the
//                 hit chance (To Hit against the player reference's Parry, the game's own HUD formula) and the attack rate.
//   Density       how many units a pull holds on average.
//   Stealth       how much of the walk (deploy -> objectives -> exit) lies inside a unit's detection: its sight, a guard's
//                 radius, a patrol's path.
//   Duration      the walk's length, counted only when there are fights on it.
//   Ambushes      units the script spawns (AddUnitToServer) and how strong they are.
//
// Units come from the loaded .mob files in order (a zone, then its quest); their stats from the database's Monsters
// sheet by prototype name. Nothing here touches the GUI: the Map Editor and `um-multitool map --rate` both call Rate().
#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "mob_file.hpp"
#include "quest_file.hpp"
#include "../db_model.hpp"

namespace rating {

constexpr int kFormulaVersion = 1;

// Points each factor can give; they add up to 10000.
enum Factor { Power, Density, Stealth, Duration, Ambush, kFactors };
constexpr std::array<double, kFactors> kMaxPoints = {3200, 2100, 1800, 1400, 1500};
inline const char* FactorName(int f) {
    static const char* const n[] = {"Enemy Power", "Density", "Stealth", "Duration", "Ambushes"};
    return n[f];
}

struct Monster {
    std::string name;
    float health = 0, absorb = 0, toHit = 0, parry = 0, dmgMin = 0, dmgSpread = 0, range = 0, actions = 0, move = 0, sight = 0;
};

struct Params {
    float defaultHelp = 7.0f;      // call-for-help radius when a unit stores none (the game's AiInfo default)
    float defaultSight = 8.5f;     // a monster's sight when the database row has none
    float absorbScale = 20.0f;     // effective HP = HP * (1 + absorption / this)
    float refParry = 10.0f;        // the player's defence when the database has no "Human Hero" row
    float refActions = 50.0f;      // attack rate reference (the same row's Tuning Actions)
    float rangedFrom = 4.0f;       // Attack Range from which a unit counts as ranged
    float anchorPullUnits = 8.0f;  // the anchor encounter: this many of the strongest unit in one pull
};

struct Monsters {
    std::unordered_map<std::string, Monster> byName; // lower-case name
    Monster hero;                                     // the player's reference row ("Human Hero")
    bool haveHero = false;
    float anchorUnit = 0;                             // power of the database's strongest unit
    std::string anchorName;
    std::string source;
    bool Empty() const { return byName.empty(); }
    const Monster* Find(const std::string& prototype) const;
};

// The Monsters sheet of a database book. Fills the table and the anchor.
bool LoadMonsters(const dbmodel::Book& book, const Params& p, Monsters& out, std::string& err);

// Per-unit numbers from a database row.
struct UnitPower {
    float effHp = 0, dps = 0, power = 0, sight = 0;
    bool ranged = false;
};
UnitPower PowerOf(const Monster& m, const Monsters& db, const Params& p);

struct Pull {
    float x = 0, y = 0, radius = 0;
    int units = 0;
    float effHp = 0, dps = 0, power = 0, norm = 0; // norm: power / the anchor pull's
    int ranged = 0;
    std::vector<std::string> names;               // "4x Goblin"
};

struct Input {
    std::vector<const mob::File*> files; // in load order: a zone's base map, then the quest's own map
    // The first `baseCount` files are a zone's base maps: they hold the whole zone's population, of which a quest meets only the
    // units its script names (QObjKill*, AddObject groups) and those near its walk. 0: every unit counts (a map rated alone).
    size_t baseCount = 0;
    quest::Rect deploy, remove;          // the first exit's areas (not set: the route has no such end)
};

struct Result {
    bool ok = false;
    std::string note;                    // what could not be rated (no units, no database)
    double total = 0;
    std::array<double, kFactors> pts{};
    std::vector<Pull> pulls;
    int units = 0, unknown = 0;          // enemy units; of them without a database row
    int fights = 0;                      // pulls worth a fight
    float routeLength = 0, exposure = 0; // exposure: the share of the route inside a detection range, 0..1
    int spawnSites = 0, spawnUnits = 0;  // AddUnitToServer calls; of them with a database row
    int objectives = 0;                  // QObj* calls found (the walk's waypoints)
    std::vector<std::array<float, 2>> route;
    std::vector<std::string> unknownNames;
};

Result Rate(const Input& in, const Monsters& db, const Params& p = Params());

// The tiers shown next to a score.
const char* TierName(double score);
int PartyLevelLow(double score); // the shown range is [low, low + 2]

} // namespace rating
