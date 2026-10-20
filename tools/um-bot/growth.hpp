// SPDX-License-Identifier: GPL-3.0-or-later
// um-bot's growth: what the camp's skills window lets a character buy with its free experience (skills +1, abilities / "perks"), at the game's own prices, and which of it the bot
// buys for its role. The rules are the game's (CCampSkillsWin::OnLButtonUp 0x626920 and RefreshPerks 0x625880, the skill cost 0x527FF0, Perk_ComputeCost 0x528050,
// UnitStats_CanLearnPerk 0x5260A0, UnitStats_Recompute 0x524CD0); the perk cost was checked against the running game for every perk of the table at several ability counts (468 cases).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mp_file.hpp"
#include "viewer/item_db.hpp"

namespace growth {

// Skill bytes of the unit parameters: melee 0x6CB, archery +1, backstab +2, elemental +3, sense +4, astral +5 (the database's perk "skill index" 0..5).
constexpr int kSkillBase = 0x6CB, kSkills = 6, kSkillCap = 100;
constexpr int kMelee = 0, kArchery = 1, kElemental = 3, kSense = 4, kAstral = 5;
inline const char* SkillName(int s) {
    static const char* const k[kSkills] = {"Melee", "Archery", "Backstab", "Elemental Magic", "Sensory Magic", "Astral Magic"};
    return s >= 0 && s < kSkills ? k[s] : "?";
}

// What raising a skill from `value` to value + 1 costs: 50 * (1.61^((value+1)/5) - 1.61^(value/5)), cut to an integer (floats as the game computes them).
inline int SkillCost(int value) { // (the game subtracts in the FPU's extended precision, hence long double)
    return static_cast<int>(50.0L * (std::pow(1.61L, (value + 1) / 5.0L) - std::pow(1.61L, value / 5.0L)));
}

// The experience cost of a perk: the difference of two exponentials of the table's cost numbers (this level and the one before it), times 1.95 for every ability level the
// character already has, then rounded to one significant digit (two when the leading digit is below 4).
inline int PerkCost(const items::Database& db, size_t index, int abilitiesOwned) {
    if (index >= db.perks.size()) return 0;
    const long double p = db.perks[index].costNum, prev = index % 3 && index > 0 ? db.perks[index - 1].costNum : 0.0L;
    const long double base = std::trunc(50.0L * (std::pow(1.61L, p / 5.0L) - std::pow(1.61L, prev / 5.0L)));
    const double T = static_cast<float>(base * std::pow(1.9500000476837158L, abilitiesOwned)); // 1.95 is a float constant in the game
    if (T <= 0) return 0;
    double m = std::pow(10.0, std::floor(std::log10(T)));
    double q = T / m;
    if (q < 4.0) {
        if (m <= 10.0) return static_cast<int>(std::ceil(T));
        m = static_cast<double>(static_cast<float>(m * 0.1));
        q = T / m;
    }
    return static_cast<int>(std::floor(q + 0.5) * m);
}

struct Prefs {
    int role = 1;           // 0 melee, 1 ranged, 2 mage, 3 hybrid
    int tankiness = 30;     // 0 all damage .. 100 all defence
};

struct Buy { bool perk = false; int index = 0; int to = 0; int cost = 0; }; // a skill raised to `to` (cost: the whole raise) / a perk bought (index: the table's)
struct Plan {
    std::vector<Buy> buys;
    long cost = 0, freeBefore = 0;
    std::vector<std::string> lines;
};

inline int Level(const mp::Member& m, int at) { return at >= 0 && at < static_cast<int>(m.stats.size()) ? m.stats[static_cast<size_t>(at)] : 0; }
inline int PerkLevel(const mp::Member& m, size_t category) { return Level(m, mp::kPerkBase + static_cast<int>(category)); }
inline int AbilitiesOwned(const mp::Member& m) { int n = 0; for (int g = 0; g < mp::kPerkGroups; ++g) n += PerkLevel(m, static_cast<size_t>(g)); return n; }

// UnitStats_CanLearnPerk: enabled, attribute minima, the skill it needs, the previous level owned (exactly), and no other ability of the same exclusion group.
inline bool CanLearn(const items::Database& db, const mp::Member& m, size_t index, const std::vector<int>* skillOverride = nullptr) {
    if (index >= db.perks.size()) return false;
    const items::Database::Perk& p = db.perks[index];
    if (!p.enabled) return false;
    const float str = mp::GetF(m.stats, 0x10), dex = mp::GetF(m.stats, 0x18), in = mp::GetF(m.stats, 0x20);
    if (p.attrMin[0] > str || p.attrMin[1] > dex || p.attrMin[2] > in) return false;
    if (!p.skill.empty() && p.skill != "none") {
        if (p.skillIndex < 0 || p.skillIndex >= kSkills) return false;
        const int have = skillOverride ? (*skillOverride)[static_cast<size_t>(p.skillIndex)] : Level(m, kSkillBase + p.skillIndex);
        if (p.skillLevel > have) return false;
    }
    const size_t cat = index / 3, level = index % 3;
    if (!p.prereq.empty() && p.prereq != "none") {
        size_t pre = db.perks.size();
        for (size_t i = 0; i < db.perks.size(); ++i) if (db.perks[i].key == p.prereq) { pre = i; break; }
        if (pre >= db.perks.size() || PerkLevel(m, pre / 3) != static_cast<int>(pre % 3) + 1) return false;
    }
    for (size_t c = 0; c < static_cast<size_t>(mp::kPerkGroups) && c * 3 < db.perks.size(); ++c) {
        const int have = PerkLevel(m, c);
        if (c == cat || have <= 0) continue;
        const size_t other = c * 3 + static_cast<size_t>(have) - 1;
        if (other < db.perks.size() && db.perks[other].group == p.group) return false;
    }
    return PerkLevel(m, cat) < static_cast<int>(level) + 1 && PerkLevel(m, cat) >= static_cast<int>(level); // exactly the next level (the previous one owned)
}

// The ability group of the weapon the character wields (sword 0, axe 1, dagger 2, spear 3, bludgeon 4, bow 5, crossbow 6), or the role's usual one.
inline int WeaponGroupOf(const items::Database& db, const mp::Character& ch, int role) {
    if (!ch.members.empty() && !ch.members[0].lists[0].empty()) {
        const uint32_t id = ch.members[0].lists[0][0];
        for (const mp::Object& o : ch.lists[0]) if (o.id == id && o.kind == 0x3004) {
            const auto& L = db.List(items::Category::Weapons);
            if (o.b < L.size()) {
                const int g = L[o.b].typeId;
                const bool ranged = g >= 5;
                if (ranged == (role == 1) || role == 3) return g;
            }
        }
    }
    return role == 1 ? 5 : 0;
}

// What the bot buys: greedy by (what it is worth to the role) / (what it costs). A skill point is worth less the higher the skill is; an ability by its bonus (percent, or points for
// the attributes). Every number is a judgment, the costs and the rules are the game's.
inline Plan MakePlan(const items::Database& db, const mp::Character& ch, const Prefs& pf) {
    Plan plan;
    if (ch.members.empty() || db.perks.empty()) return plan;
    mp::Member me = ch.members[0]; // worked on a copy: the stats bytes follow the plan as it grows
    const long free = static_cast<long>(mp::GetF(me.stats, mp::kExpTotal) - mp::GetF(me.stats, mp::kExpSpent));
    plan.freeBefore = free;
    long left = free;
    const int group = WeaponGroupOf(db, ch, pf.role);
    const int weaponSkill = group >= 5 ? kArchery : kMelee;
    const float tank = 0.5f + pf.tankiness / 100.0f; // health matters more for a defensive build
    const bool mage = pf.role == 2, hybrid = pf.role == 3;

    // Worth of a skill point at 0, by skill (primary skills of the role; the magic school the Healing spell belongs to).
    float skillWeight[kSkills] = {};
    if (pf.role == 0 || hybrid) skillWeight[kMelee] = hybrid ? 0.8f : 1.0f;
    if (pf.role == 1) skillWeight[kArchery] = 1.0f;
    if (mage || hybrid) skillWeight[kAstral] = mage ? 1.0f : 0.5f;
    if (mage) skillWeight[kElemental] = 0.4f;
    if (pf.role == 0) skillWeight[weaponSkill == kMelee ? kMelee : kArchery] = 1.0f;
    if (pf.role == 1) skillWeight[kMelee] = 0.0f;

    // Worth per unit of ability bonus, by ability group (0..25: sword .. intelligence).
    float abilityWeight[26] = {};
    abilityWeight[group] = mage ? 0.15f : 1.0f;                   // the wielded weapon's (damage bonus in percent)
    abilityWeight[16] = 0.45f * tank;                             // health (percent of max health)
    abilityWeight[17] = mage || hybrid ? 0.4f : 0.05f;            // mana
    abilityWeight[18] = 0.2f * tank;                              // vitality: health regeneration
    abilityWeight[19] = mage || hybrid ? 0.25f : 0.03f;           // spirit: mana regeneration
    abilityWeight[20] = 0.5f;                                     // quickness: actions
    abilityWeight[21] = 0.25f;                                    // lift: carrying capacity (heavier armor)
    abilityWeight[15] = 0.04f;                                    // night vision
    abilityWeight[23] = 6.0f;                                     // strength points: health and capacity
    abilityWeight[24] = pf.role == 1 ? 5.0f : 3.0f;               // dexterity points: speed, ranged accuracy
    abilityWeight[25] = mage ? 6.0f : 1.0f;                       // intelligence points: magic skill
    if (mage || hybrid) for (int g = 7; g <= 14; ++g) abilityWeight[g] = g == 13 ? 0.9f : 0.15f; // the magic schools' abilities (Healing), where the database enables them

    std::vector<int> skill(kSkills);
    for (int s = 0; s < kSkills; ++s) skill[static_cast<size_t>(s)] = Level(me, kSkillBase + s);
    std::vector<int> skillFrom = skill;
    int owned = AbilitiesOwned(me);
    for (int guard = 0; guard < 2000; ++guard) {
        float bestRatio = 0; int bestSkill = -1; long bestCost = 0; size_t bestPerk = db.perks.size();
        for (int s = 0; s < kSkills; ++s) {
            const int v = skill[static_cast<size_t>(s)];
            if (skillWeight[s] <= 0 || v >= kSkillCap) continue;
            const long cost = SkillCost(v);
            if (cost <= 0 || cost > left) continue;
            const float ratio = skillWeight[s] * (1.0f - v / 125.0f) / static_cast<float>(cost);
            if (ratio > bestRatio) { bestRatio = ratio; bestSkill = s; bestCost = cost; bestPerk = db.perks.size(); }
        }
        for (size_t i = 0; i < db.perks.size(); ++i) {
            const size_t cat = i / 3;
            if (cat >= 26 || abilityWeight[cat] <= 0 || !CanLearn(db, me, i, &skill)) continue;
            const long cost = PerkCost(db, i, owned);
            if (cost <= 0 || cost > left) continue;
            const int before = i % 3 ? db.perks[i - 1].bonus : 0;
            const float gain = abilityWeight[cat] * static_cast<float>(std::max(1, db.perks[i].bonus - before));
            const float ratio = gain / static_cast<float>(cost);
            if (ratio > bestRatio) { bestRatio = ratio; bestSkill = -1; bestCost = cost; bestPerk = i; }
        }
        if (bestRatio <= 0) break;
        left -= bestCost;
        if (bestSkill >= 0) {
            ++skill[static_cast<size_t>(bestSkill)];
            me.stats[static_cast<size_t>(kSkillBase + bestSkill)] = static_cast<uint8_t>(skill[static_cast<size_t>(bestSkill)]);
        } else {
            me.stats[static_cast<size_t>(mp::kPerkBase) + bestPerk / 3] = static_cast<uint8_t>(bestPerk % 3 + 1);
            plan.buys.push_back({true, static_cast<int>(bestPerk), 0, static_cast<int>(bestCost)});
            plan.lines.push_back(db.perks[bestPerk].name + " (" + std::to_string(bestCost) + ")");
            ++owned;
        }
        plan.cost += bestCost;
        // the skill raises of the same skill are one buy: a skill's cost entry is its whole raise
        if (bestSkill >= 0) {
            auto it = std::find_if(plan.buys.begin(), plan.buys.end(), [&](const Buy& b) { return !b.perk && b.index == bestSkill; });
            if (it == plan.buys.end()) plan.buys.push_back({false, bestSkill, skill[static_cast<size_t>(bestSkill)], static_cast<int>(bestCost)});
            else { it->to = skill[static_cast<size_t>(bestSkill)]; it->cost += static_cast<int>(bestCost); }
        }
    }
    for (int s = 0; s < kSkills; ++s)
        if (skill[static_cast<size_t>(s)] != skillFrom[static_cast<size_t>(s)])
            plan.lines.insert(plan.lines.begin(), std::string(SkillName(s)) + " " + std::to_string(skillFrom[static_cast<size_t>(s)]) + " -> " + std::to_string(skill[static_cast<size_t>(s)]));
    return plan;
}

// UnitStats_Recompute's cached values that the camp and the shop planner read: the attributes with their ability bonus, the carrying capacity and the actions.
inline void Refresh(const items::Database& db, mp::Member& m) {
    auto bonus = [&](size_t cat) { const int lv = PerkLevel(m, cat); return lv > 0 && cat * 3 + static_cast<size_t>(lv) - 1 < db.perks.size() ? static_cast<float>(db.perks[cat * 3 + static_cast<size_t>(lv) - 1].bonus) : 0.0f; };
    const float str = mp::GetF(m.stats, 0x0C) + bonus(23), dex = mp::GetF(m.stats, 0x14) + bonus(24), in = mp::GetF(m.stats, 0x1C) + bonus(25);
    mp::SetF(m.stats, 0x10, str); mp::SetF(m.stats, 0x18, dex); mp::SetF(m.stats, 0x20, in);
    mp::SetF(m.stats, 0x54, (bonus(21) * 0.01f + 1.0f) * str * 12.0f);
    const float legs = mp::GetF(m.stats, 0x5C) > 0 ? mp::GetF(m.stats, 0x5C) : 1.0f;
    mp::SetF(m.stats, 0x2C, dex * 0.2f + 10.0f);
    mp::SetF(m.stats, 0x30, (bonus(20) * 0.01f + 1.0f) * legs * (dex * 0.2f + 10.0f));
}

// Spends the plan on the character: the skill bytes, the ability bytes, the experience spent, the cached values.
inline std::string Apply(const items::Database& db, mp::Character& ch, const Plan& plan) {
    if (ch.members.empty() || plan.buys.empty()) return "";
    mp::Member& me = ch.members[0];
    for (const Buy& b : plan.buys) {
        if (b.perk) me.stats[static_cast<size_t>(mp::kPerkBase) + static_cast<size_t>(b.index) / 3] = static_cast<uint8_t>(b.index % 3 + 1);
        else me.stats[static_cast<size_t>(kSkillBase + b.index)] = static_cast<uint8_t>(b.to);
    }
    mp::SetF(me.stats, mp::kExpSpent, mp::GetF(me.stats, mp::kExpSpent) + static_cast<float>(plan.cost));
    Refresh(db, me);
    std::string text = "experience: " + std::to_string(plan.cost) + " of " + std::to_string(plan.freeBefore) + " spent";
    for (const std::string& l : plan.lines) text += "\n  " + l;
    return text;
}

} // namespace growth
