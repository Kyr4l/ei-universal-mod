// SPDX-License-Identifier: GPL-3.0-or-later
// um-bot's spells: what the camp's spell constructor makes of a spell blueprint ("Prototype") and runes, as the .mp objects the character keeps (a spell object, kind 0x3002, held by a spell
// container item, kind 0x3008, in the member's list 4), and whether a character can cast it. The rules are the game's own: CSpellContainer::Finalize 0x68BF60 (the runes' effects), CSpell::Write
// 0x68B920 (the object's layout), CItemLoot price slot 0x6A5470 (blueprint = the proto's field 3, rune = field 2), CCampScreen::CanDropItem 0x6100E0 / CUnitServer::CanCastSpell 0x54B950 (what a
// character may use). Checked against every spell object of the saved characters in the repository.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "mp_file.hpp"
#include "viewer/item_db.hpp"

namespace spells {

struct Built {
    std::vector<uint8_t> object;     // the spell object's detail (66 bytes, kind 0x3002), id filled in
    std::vector<uint8_t> container;  // the spell container item's detail (56 bytes, kind 0x3008), id filled in
    float mana = 0, price = 0;       // the mana the spell costs (before the Intelligence scaling), the sum of the blueprint's and the runes' prices
    int required = 0;                // the magic skill level needed
    int school = 0;                  // 0..2 elemental (fire, lightning, acid), 3..4 sense (illusion, divination), 5..7 astral (enchantments, healing, domination)
    int row = 0;
    std::vector<int> runes;          // rune rows used
};

inline void PutU32(std::vector<uint8_t>& v, uint32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); }
inline void PutF(std::vector<uint8_t>& v, float x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); }

// A rune the spell may take: its kind is one the blueprint allows (field 20's flags: range, targets, area, effect, duration; the other kinds always fit).
inline bool RuneAllowed(const items::Database& db, int spellRow, int runeRow) {
    if (spellRow < 0 || static_cast<size_t>(spellRow) >= db.spells.size() || runeRow < 0 || static_cast<size_t>(runeRow) >= db.runes.size()) return false;
    switch (db.runes[static_cast<size_t>(runeRow)].code[0]) {
    case 'r': return db.spells[static_cast<size_t>(spellRow)].runeKinds[0] != 0;
    case 't': return db.spells[static_cast<size_t>(spellRow)].runeKinds[1] != 0;
    case 'a': return db.spells[static_cast<size_t>(spellRow)].runeKinds[2] != 0;
    case 'e': return db.spells[static_cast<size_t>(spellRow)].runeKinds[3] != 0;
    case 'd': return db.spells[static_cast<size_t>(spellRow)].runeKinds[4] != 0;
    default: return true;
    }
}

// CSpellContainer::Finalize: the blueprint's values, then every rune applied in order.
inline Built Build(const items::Database& db, int row, const std::vector<int>& runes, uint32_t spellId, uint32_t containerId) {
    Built b;
    const items::Database::SpellProto& P = db.spells[static_cast<size_t>(row)];
    b.row = row; b.runes = runes;
    uint32_t duration = static_cast<uint32_t>(P.ni[13]);          // S+0x20
    float effect = P.n[10], area = P.n[9];                         // S+0x24, S+0x28
    const float zero = 0.0f;
    float range0 = P.n[7], range = P.n[8];                         // S+0x30, S+0x34
    int targets = P.ni[12];                                        // S+0x38
    uint32_t flags = P.ni[11] == 0x75 ? 0x10000000u : P.ni[11] == 0x74 ? 0x20000000u : 0u; // S+0x3c
    float mana = P.n[5];                                           // S+0x40
    int count = P.ni[6];                                           // S+0x44
    float price = P.n[3];                                          // S+0x48
    const int x4c = P.ni[14];                                      // S+0x4c
    int required = P.ni[21];                                       // S+0x50
    std::vector<uint8_t> perks;
    auto clamp0 = [](float x) { return 0.0f < x ? x : 0.0f; };
    for (int r : runes) {
        const items::Database::Rune& R = db.runes[static_cast<size_t>(r)];
        switch (R.code[0]) {
        case 'a': area = clamp0(R.value * P.n[9] + area); break;
        case 'd': { int64_t d = static_cast<int64_t>(duration) + static_cast<int64_t>(static_cast<double>(static_cast<uint32_t>(P.ni[13])) * R.value); duration = static_cast<uint32_t>(d < 0 ? 0 : d); break; }
        case 'e': effect = clamp0(R.value * P.n[10] + effect); break;
        case 'f': switch (R.code[1]) { case 'e': flags |= 2; break; case 'f': flags |= 1; break; case 'g': flags |= 8; break; case 'h': flags |= 4; break; case 'l': flags |= 0x20; break; case 'o': flags |= 0x10; break; default: break; } break;
        case 'i': if (R.code[1] == 'c') flags |= 0x20000; else if (R.code[1] == 't') flags |= 0x10000; break;
        case 'm': mana = clamp0(R.value * P.n[5] + mana); break;
        case 'r': range = clamp0(R.value * P.n[8] + range); break;
        case 't': targets += static_cast<int>(static_cast<double>(P.ni[12]) * R.value); break;
        default: break;
        }
        ++count;
        required += R.level;
        mana += R.mana;
        price += R.price;
        perks.push_back(static_cast<uint8_t>(r + 1));
    }
    if (0.0f < area) area = std::sqrt(area / 3.1415927f);
    mana = std::max(P.n[5], mana);
    std::sort(perks.begin(), perks.end());
    perks.resize(8, 0);
    b.mana = mana; b.price = price; b.required = required; b.school = P.ni[19];
    (void)zero;
    std::vector<uint8_t>& o = b.object;
    PutU32(o, spellId);
    o.push_back(static_cast<uint8_t>(row)); o.push_back(0x10);
    PutU32(o, duration); PutF(o, effect); PutF(o, area);
    PutF(o, range0); PutF(o, range); PutU32(o, static_cast<uint32_t>(targets)); PutU32(o, flags);
    PutF(o, mana); PutU32(o, static_cast<uint32_t>(count)); PutF(o, price); PutU32(o, static_cast<uint32_t>(x4c));
    o.insert(o.end(), perks.begin(), perks.end());
    PutU32(o, 0); PutU32(o, static_cast<uint32_t>(required));
    std::vector<uint8_t>& c = b.container;
    PutU32(c, containerId);
    c.push_back(0); c.push_back(0); c.push_back(1); c.push_back(0);        // material 0, row 1
    PutF(c, 1000.0f); PutF(c, 1000.0f); PutF(c, 0.0f); PutF(c, 0.0f); PutF(c, 0.001f); PutF(c, 1.0f); PutF(c, price + 2.0f); PutF(c, 1.0f);
    PutU32(c, spellId); PutU32(c, 1); PutU32(c, 0); PutU32(c, 1);
    return b;
}

// What the character may use (UnitStats_MagicSkill: Intelligence - 25 + the school's skill byte + the school ability's bonus; UnitStats_MaxMana; the cost scaled by 25 / Intelligence).
inline float MagicSkill(const items::Database& db, const mp::Member& m, int school) {
    const int skillAt = school <= 2 ? 0x6CE : school <= 4 ? 0x6CF : 0x6D0;
    const float intelligence = mp::GetF(m.stats, 0x20);
    const size_t cat = 7 + static_cast<size_t>(school);   // the ability groups 7..14: fire .. domination
    const int lv = cat < 26 ? m.stats[static_cast<size_t>(mp::kPerkBase) + cat] : 0;
    const size_t at = cat * 3 + static_cast<size_t>(lv) - 1;
    const float perk = lv > 0 && at < db.perks.size() ? static_cast<float>(db.perks[at].bonus) : 0.0f;
    return intelligence - 25.0f + static_cast<float>(m.stats[static_cast<size_t>(skillAt)]) + perk;
}
inline float MaxMana(const items::Database& db, const mp::Member& m) {
    const float exp = mp::GetF(m.stats, mp::kExpTotal), dex = mp::GetF(m.stats, 0x18);
    const double level = exp > 50.0f ? std::log(exp / 50.0) / std::log(1.61) : 0.0;
    const int lv = m.stats[static_cast<size_t>(mp::kPerkBase) + 17];
    const size_t at = 17 * 3 + static_cast<size_t>(lv) - 1;
    const float perk = lv > 0 && at < db.perks.size() ? static_cast<float>(db.perks[at].bonus) : 0.0f;
    return static_cast<float>((dex / 25.0) * 30.0 * std::pow(1.1, level) * (1.0 + perk / 100.0));
}
inline bool CanCast(const items::Database& db, const mp::Member& m, float mana, int required, int school) {
    const float intelligence = mp::GetF(m.stats, 0x20);
    return static_cast<int>(MagicSkill(db, m, school)) >= required && static_cast<int>(mana * 25.0f / (intelligence > 0 ? intelligence : 25.0f)) <= static_cast<int>(MaxMana(db, m));
}

// ---- the shop and the bot's choices ------------------------------------------------------------------------------------------------------------------------------------------------

struct StockSpell { int row = 0, count = 0; };   // a blueprint the shop offers, and how many (1..3 in the live shop)

// What the shop of an allod sells of spells: every spell whose "Shop N" bit is set, 1..3 of each (the live Babur shop: exactly the 12 spells of its mask).
inline std::vector<StockSpell> Stock(const items::Database& db, int shopNo, std::mt19937& rng) {
    std::vector<StockSpell> out;
    for (size_t i = 0; i < db.spells.size(); ++i)
        if (db.spells[i].shopMask >> (shopNo - 1) & 1) out.push_back({static_cast<int>(i), 1 + static_cast<int>(rng() % 3)});
    return out;
}

inline int RowOfCode(const items::Database& db, const std::string& code) {
    for (size_t i = 0; i < db.spells.size(); ++i) { std::string c = db.spells[i].code; for (char& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); if (c == code) return static_cast<int>(i); }
    return -1;
}

// Spell rows the character holds (the spell objects of its spell containers, in the order of the member's list 4).
inline std::vector<int> OwnedRows(const mp::Character& ch) {
    std::vector<int> rows;
    if (ch.members.empty()) return rows;
    for (uint32_t id : ch.members[0].lists[3]) {
        for (const mp::Object& c : ch.lists[0]) {
            if (c.id != id || c.kind != 0x3008 || c.detail.size() < 44) continue;
            uint32_t spellId; std::memcpy(&spellId, c.detail.data() + 40, 4);
            for (const mp::Object& sp : ch.lists[1]) if (sp.id == spellId && sp.kind == 0x3002) rows.push_back(static_cast<int>(sp.b));
        }
    }
    return rows;
}

// The skill byte the school needs for a bare blueprint of `row` (no runes) to be usable by the member, or -1 when its mana can never be enough.
inline int SkillByteNeeded(const items::Database& db, const mp::Member& m, int row) {
    const Built b = Build(db, row, {}, 1, 2);
    mp::Member t = m;
    const int skillAt = b.school <= 2 ? 0x6CE : b.school <= 4 ? 0x6CF : 0x6D0;
    for (int byte = m.stats[static_cast<size_t>(skillAt)]; byte <= 100; ++byte) {
        t.stats[static_cast<size_t>(skillAt)] = static_cast<uint8_t>(byte);
        if (CanCast(db, t, b.mana, b.required, b.school)) return byte;
    }
    return -1;
}

// The school's skill byte a built spell needs (-1: its mana can never be enough).
inline int SkillByteForBuilt(const items::Database& db, const mp::Member& m, const Built& b) {
    mp::Member t = m;
    const int skillAt = b.school <= 2 ? 0x6CE : b.school <= 4 ? 0x6CF : 0x6D0;
    for (int byte = m.stats[static_cast<size_t>(skillAt)]; byte <= 100; ++byte) {
        t.stats[static_cast<size_t>(skillAt)] = static_cast<uint8_t>(byte);
        if (CanCast(db, t, b.mana, b.required, b.school)) return byte;
    }
    return -1;
}

struct StockRune { int row = 0, count = 0; };   // a rune the shop offers (20..50 of each in the live shop)
inline std::vector<StockRune> RuneStock(const items::Database& db, int shopNo, std::mt19937& rng) {
    std::vector<StockRune> out;
    for (size_t i = 0; i < db.runes.size(); ++i)
        if (db.runes[i].shopMask >> (shopNo - 1) & 1) out.push_back({static_cast<int>(i), 20 + static_cast<int>(rng() % 31)});
    return out;
}

// The worn spell of a row: its container, its spell object, its effect and mana (read from the object).
struct Equipped { uint32_t container = 0, spell = 0; float effect = 0, mana = 0; int required = 0; };
inline Equipped EquippedSpell(const mp::Character& ch, int row) {
    Equipped e;
    if (ch.members.empty()) return e;
    for (uint32_t id : ch.members[0].lists[3])
        for (const mp::Object& c : ch.lists[0]) {
            if (c.id != id || c.kind != 0x3008 || c.detail.size() < 44) continue;
            uint32_t spellId; std::memcpy(&spellId, c.detail.data() + 40, 4);
            for (const mp::Object& sp : ch.lists[1]) {
                if (sp.id != spellId || sp.kind != 0x3002 || static_cast<int>(sp.b) != row || sp.detail.size() < 66) continue;
                uint32_t need; std::memcpy(&e.effect, sp.detail.data() + 10, 4); std::memcpy(&e.mana, sp.detail.data() + 34, 4); std::memcpy(&need, sp.detail.data() + 62, 4);
                e.container = id; e.spell = spellId; e.required = static_cast<int>(need);
                return e;
            }
        }
    return e;
}

// The strongest version of a spell (more of an "effect" rune: the Healing spell's heal grows with each) the character can use, within a price: tries every mix of up to six runes (effect and mana kinds the blueprint allows). It must be castable (skill, mana) at least twice from a full pool; `ignoreSkill` asks what it could be with more skill (what the skill is raised for).
struct Upgrade { std::vector<int> runes; Built built; float price = 0; bool ok = false; };
inline Upgrade BestVersion(const items::Database& db, const mp::Member& me, int row, const std::vector<StockRune>& runes, float budget, bool ignoreSkill) {
    Upgrade best;
    std::vector<StockRune> eff;
    for (const StockRune& r : runes) { const char k = db.runes[static_cast<size_t>(r.row)].code[0]; if (r.count > 0 && (k == 'e' || k == 'm') && RuneAllowed(db, row, r.row)) eff.push_back(r); } // effect runes, and mana runes (they take mana off what the effect runes add; the cost never drops below the blueprint's)
    const float intelligence = mp::GetF(me.stats, 0x20), pool = MaxMana(db, me);
    std::vector<int> pick;
    std::function<void(size_t, int)> rec = [&](size_t from, int left) {
        const Built b = Build(db, row, pick, 1, 2);
        const float scaled = b.mana * 25.0f / (intelligence > 0 ? intelligence : 25.0f);
        float price = b.price;
        const bool fits = price <= budget && scaled * 2 <= pool && (ignoreSkill || CanCast(db, me, b.mana, b.required, b.school));
        if (fits) {
            float effect, bestEffect = 0;
            std::memcpy(&effect, b.object.data() + 10, 4);
            if (best.ok) std::memcpy(&bestEffect, best.built.object.data() + 10, 4);
            float bestMana = 0; if (best.ok) bestMana = best.built.mana;
            const bool same = std::fabs(effect - bestEffect) <= 0.01f;
            if (!best.ok || effect > bestEffect + 0.01f || (same && b.mana < bestMana - 0.01f) || (same && std::fabs(b.mana - bestMana) <= 0.01f && price < best.price)) { best.ok = true; best.runes = pick; best.built = b; best.price = price; }
        }
        if (left == 0) return;
        for (size_t i = from; i < eff.size(); ++i) {
            const int used = static_cast<int>(std::count(pick.begin(), pick.end(), eff[i].row));
            if (used >= eff[i].count) continue;
            pick.push_back(eff[i].row); rec(i, left - 1); pick.pop_back();
        }
    };
    rec(0, 6);
    return best;
}

struct Buy { int row = 0; std::vector<int> runes; float cost = 0; };
struct Plan { std::vector<Buy> buys; std::vector<uint32_t> equips; std::vector<std::pair<uint32_t, uint32_t>> replaced; long money = 0; std::vector<std::string> notes; }; // equips: spell containers carried but not worn; replaced: (container, spell) of a spell the new one replaces

// The spell containers the character carries without wearing them (in its items, not in the member's list 4), with the spell row each holds.
inline std::vector<std::pair<uint32_t, int>> SpareContainers(const mp::Character& ch) {
    std::vector<std::pair<uint32_t, int>> out;
    if (ch.members.empty()) return out;
    const auto& worn = ch.members[0].lists[3];
    for (const mp::Object& c : ch.lists[0]) {
        if (c.kind != 0x3008 || c.detail.size() < 44 || std::find(worn.begin(), worn.end(), c.id) != worn.end()) continue;
        uint32_t spellId; std::memcpy(&spellId, c.detail.data() + 40, 4);
        for (const mp::Object& sp : ch.lists[1]) if (sp.id == spellId && sp.kind == 0x3002) out.push_back({c.id, static_cast<int>(sp.b)});
    }
    return out;
}

// Which spells the bot makes: the Healing spell when it has none and can use it (its companion job), worn from its items or from the blueprint the shop offers; then, for a mage or hybrid,
// an attack spell (PickAttack), within the money (the blueprint's price).
inline Plan MakePlan(const items::Database& db, const mp::Character& ch, const std::vector<StockSpell>& stock, const std::vector<StockRune>& runes = {}) {
    Plan plan;
    if (ch.members.empty()) return plan;
    const mp::Member& me = ch.members[0];
    long money = static_cast<long>(mp::Money(ch));
    const int heal = RowOfCode(db, "healing");
    const std::vector<int> owned = OwnedRows(ch);
    plan.money = money;
    auto has = [&](int row) { return std::find(owned.begin(), owned.end(), row) != owned.end(); };
    auto spare = [&](int row) -> uint32_t { // a carried container with a usable spell of `row`
        for (const auto& sp : SpareContainers(ch)) {
            if (sp.second != row) continue;
            for (const mp::Object& c : ch.lists[0]) {
                if (c.id != sp.first) continue;
                uint32_t spellId; std::memcpy(&spellId, c.detail.data() + 40, 4);
                for (const mp::Object& o : ch.lists[1]) {
                    if (o.id != spellId || o.detail.size() < 66) continue;
                    float mana; uint32_t need; std::memcpy(&mana, o.detail.data() + 34, 4); std::memcpy(&need, o.detail.data() + 62, 4);
                    if (CanCast(db, me, mana, static_cast<int>(need), db.spells[static_cast<size_t>(row)].ni[19])) return sp.first;
                }
            }
        }
        return 0;
    };
    auto make = [&](int row, const char* what, long budget) {
        if (const uint32_t id = spare(row)) { plan.equips.push_back(id); return; }
        const auto it = std::find_if(stock.begin(), stock.end(), [&](const StockSpell& s) { return s.row == row && s.count > 0; });
        if (it == stock.end()) { plan.notes.push_back(std::string("the shop has no ") + what + " blueprint"); return; }
        const Built b = Build(db, row, {}, 1, 2);
        if (!CanCast(db, me, b.mana, b.required, b.school)) { plan.notes.push_back(std::string(what) + ": not usable yet (magic skill " + std::to_string(static_cast<int>(MagicSkill(db, me, b.school))) + " of " + std::to_string(b.required) + ", mana " + std::to_string(static_cast<int>(MaxMana(db, me))) + ")"); return; }
        if (static_cast<long>(b.price) > budget) { plan.notes.push_back(std::string(what) + ": " + std::to_string(static_cast<long>(b.price)) + " needed, " + std::to_string(budget) + " to spend"); return; }
        plan.buys.push_back({row, {}, b.price});
        plan.money -= static_cast<long>(b.price);
    };
    if (heal >= 0 && !has(heal)) make(heal, "Healing", plan.money);
    // A stronger Healing spell: the blueprint again and runes, when the worn one can be beaten by a quarter within half of what is left (the old one is replaced).
    if (heal >= 0 && has(heal) && !runes.empty() && std::any_of(stock.begin(), stock.end(), [&](const StockSpell& s) { return s.row == heal && s.count > 0; })) {
        const Equipped cur = EquippedSpell(ch, heal);
        const Built base = Build(db, heal, {}, 1, 2);
        const Upgrade up = BestVersion(db, me, heal, runes, std::max(0.0f, static_cast<float>(plan.money) * 0.5f), false);
        if (cur.container && up.ok && !up.runes.empty()) {
            float effect; std::memcpy(&effect, up.built.object.data() + 10, 4);
            if (effect >= cur.effect * 1.25f && static_cast<long>(up.price) <= plan.money) {
                plan.buys.push_back({heal, up.runes, up.price});
                plan.replaced.push_back({cur.container, cur.spell});
                plan.money -= static_cast<long>(up.price);
                plan.notes.push_back("Healing: effect " + std::to_string(static_cast<int>(cur.effect)) + " -> " + std::to_string(static_cast<int>(effect)) + " with " + std::to_string(up.runes.size()) + " rune(s)");
            }
        }
        (void)base;
    }
    return plan;
}

// Makes the spells: the spell object (list 2 of the file), its container item (list 1, worn in the member's list 4), the blueprint paid.
inline std::string Apply(const items::Database& db, mp::Character& ch, const Plan& plan) {
    if (ch.members.empty() || (plan.buys.empty() && plan.equips.empty())) return "";
    mp::Member& me = ch.members[0];
    for (const auto& old : plan.replaced) { // the spell a stronger one replaces: its container and spell object go
        me.lists[3].erase(std::remove(me.lists[3].begin(), me.lists[3].end(), old.first), me.lists[3].end());
        ch.lists[0].erase(std::remove_if(ch.lists[0].begin(), ch.lists[0].end(), [&](const mp::Object& o) { return o.id == old.first; }), ch.lists[0].end());
        ch.lists[1].erase(std::remove_if(ch.lists[1].begin(), ch.lists[1].end(), [&](const mp::Object& o) { return o.id == old.second; }), ch.lists[1].end());
    }
    for (uint32_t id : plan.equips) {
        ch.backpack.erase(std::remove(ch.backpack.begin(), ch.backpack.end(), id), ch.backpack.end());
        me.lists[3].push_back(id);
    }
    if (plan.buys.empty()) return "spell worn: " + std::to_string(plan.equips.size()) + " from the items";
    uint32_t next = 1;
    for (const auto& list : ch.lists) for (const mp::Object& o : list) next = std::max(next, o.id + 1);
    std::string text;
    for (const Buy& buy : plan.buys) {
        const uint32_t spellId = next++, containerId = next++;
        const Built b = Build(db, buy.row, buy.runes, spellId, containerId);
        mp::Object sp; sp.id = spellId; sp.kind = 0x3002; sp.a = 0; sp.b = static_cast<uint16_t>(buy.row); sp.detail = b.object;
        mp::Object ct; ct.id = containerId; ct.kind = 0x3008; ct.a = 0; ct.b = 1; ct.detail = b.container;
        ch.lists[1].push_back(sp); ch.lists[0].push_back(ct);
        me.lists[3].push_back(containerId);
        text += (text.empty() ? "" : ", ") + db.spells[static_cast<size_t>(buy.row)].name + " (" + std::to_string(static_cast<long>(buy.cost)) + ")";
    }
    mp::SetMoney(ch, static_cast<uint32_t>(std::max(0L, plan.money)));
    return "spells made: " + text;
}

} // namespace spells
