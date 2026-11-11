// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Difficulty rating of a quest or a .mob: one score from 0 to 10000, the sum of five factors that only look at the
// enemies and the mission script (never at what the player buys or carries: potions, gear). The scale is anchored to
// the mod's own database: 0 is an empty map; 10000 is the hardest encounter the database can field (8 of its strongest
// unit standing together). A different database therefore moves every score, which is why a rating is stored with the
// database's date and the formula version (ratings_store.hpp).
//
//   Enemy power   the pulls' strength, on a log scale of their total and of the strongest one (so a mid-game and a late quest
//                 stay apart): a pull is units that wake each other (call-for-help radius, per unit), its power
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

constexpr int kFormulaVersion = 5;

// Points each factor can give; they add up to 10000.
enum Factor { Power, Density, Stealth, Duration, Ambush, kFactors };
constexpr std::array<double, kFactors> kMaxPoints = {5000, 1300, 1500, 1000, 1200};
inline const char* FactorName(int f) {
    static const char* const n[] = {"Enemy Power", "Density", "Stealth", "Duration", "Ambushes"};
    return n[f];
}

struct Monster {
    std::string name;
    float health = 0, absorb = 0, toHit = 0, parry = 0, dmgMin = 0, dmgSpread = 0, range = 0, actions = 0, move = 0, sight = 0;
    float hitRandom = 0, weight = 0;          // Tuning To hit Random (the roll's spread), Tuning Weapon Weight (combat.md 4.1-4.2)
    float peripheral = 0, senseLife = 0;      // Skills Peripheral, Senses SenseLife: ranges of the other two ways to notice (unit-ai.md 4.1)
    float visibility = 0, lifeSign = 0;       // Detections Sight, Detections SenseLife: how far others see / sense this unit, as factors
};

// The chance that `a` hits `d` in melee, as game.exe rolls it (ComputeHitChance 0x555BD0, ResolveHitChance 0x553DF0): roll uniform over
// -(S+1), -(S+1)+2 ... S-1 with S = a's to-hit random; a hit when roll + a.toHit + a.weight / (d.weight * 20) >= d.parry.
float HitChance(const Monster& a, const Monster& d);

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
    // The first `baseCount` files are a zone's base maps: their units count like the quest's own, but their scripts are not read for
    // the quest's goals (the walk, groups, spawns come from the quest's own maps). 0: all files are the quest's own (a map rated alone).
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
    int storedStats = 0;                 // enemy units counted with the stats stored in the map (Map Stats & Gear on, as the game plays them), not the database row
    int fights = 0;                      // pulls worth a fight
    float routeLength = 0, exposure = 0; // exposure: the share of the route inside a detection range, 0..1
    int spawnSites = 0, spawnUnits = 0;  // AddUnitToServer calls; of them with a database row
    std::vector<std::pair<std::string, int>> files; // each map read: its file name and how many of its enemies the quest meets
    int objectives = 0;                  // goals found: QObj* calls, or for an older quest the objects its script works with
    double sumNorm = 0, peakNorm = 0;    // the pulls' power as shares of the anchor group's: their sum, and the strongest one
    std::vector<std::array<float, 2>> route;
    std::vector<std::string> unknownNames;
};

Result Rate(const Input& in, const Monsters& db, const Params& p = Params());

// What the strongest fight asks of the party's hero: a fight of `seconds` needs the enemy's total effective HP dealt in that time
// (damage per second) and the enemy's damage per second taken for that long (endurance, effective HP). The tool compares these with
// the same numbers of the allod's other quests (their median): the heroes of an allod have that allod's levels and gear, which the
// database's base Human Hero row does not. The ratios to that row are kept for when nothing else is known. An estimate, like the score.
struct HeroNeed {
    bool ok = false;
    int units = 0;                       // enemies in that fight
    double x = 0, y = 0;                 // where it is
    double damage = 0, endurance = 0;    // needed per second / in total
    double damageRatio = 0, enduranceRatio = 0; // as multiples of the reference hero's
    double seconds = 0;
};
HeroNeed NeedOf(const Result& r, const Monsters& db, const Params& p = Params(), double seconds = 20);

// The tiers shown next to a score.
const char* TierName(double score);
int PartyLevelLow(double score); // the shown range is [low, low + 2]

} // namespace rating
