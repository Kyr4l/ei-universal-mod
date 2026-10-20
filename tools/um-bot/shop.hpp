// SPDX-License-Identifier: GPL-3.0-or-later
// um-bot's shopping: what the camp's item shop of an allod offers, what its items cost and weigh, and the .mp items the bot ends up with. Everything here is the game's own
// arithmetic (CItemConstructor::ApplyProto 0x6A5D60, CItem::InitFromProto 0x6A0490, CInventory::Write 0x6A0360, CCampScreen::GetItemPrice 0x60FAB0), checked against the live shop's
// items in game memory (2026-10-20), so the bot never buys something the real shop could not sell and prices match the camp screen to the unit.
#pragma once
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "mp_file.hpp"
#include "viewer/item_db.hpp"

namespace shop {

// The camp's shop number of an allod (the database's "Shop N" columns: bit N-1 of an item's mask): Gipath 1 (Babur; 2 is the singleplayer witch), Ingos 3, Suslanger 4, Jigran 5.
inline int ShopOfAllod(const std::string& allod) {
    if (allod == "gipat") return 1;
    if (allod == "ingos") return 3;
    if (allod == "suslanger") return 4;
    if (allod == "jigran") return 5;
    return 0;
}

// An item as the game builds it from a prototype and a material.
struct Built {
    uint32_t kind = 0;     // 0x3004 weapon, 0x3005 armor
    int row = 0, material = 0;
    float weight = 0, price = 0, durability = 0;
    std::vector<uint8_t> detail; // the .mp detail (without the final object id, set by the caller): id, material, row, floats ...
};

inline void PutF(std::vector<uint8_t>& v, float f) { uint8_t b[4]; std::memcpy(b, &f, 4); v.insert(v.end(), b, b + 4); }
inline void PutI(std::vector<uint8_t>& v, int32_t i) { uint8_t b[4]; std::memcpy(b, &i, 4); v.insert(v.end(), b, b + 4); }

// Builds a weapon (kind 0x3004) or an armor piece (0x3005) of prototype row `row` made of material `mat` (an index into db.materials): `count` of them (1).
inline Built Build(const items::Database& db, uint32_t kind, int row, int mat, uint32_t objectId, int count = 1) {
    Built b; b.kind = kind; b.row = row; b.material = mat;
    const auto& list = db.List(kind == 0x3004 ? items::Category::Weapons : items::Category::Armors);
    const items::Item& P = list[static_cast<size_t>(row)];
    const items::Material& M = db.materials[static_cast<size_t>(mat)];
    std::vector<uint8_t>& d = b.detail;
    PutI(d, static_cast<int32_t>(objectId));
    const uint16_t a = static_cast<uint16_t>(mat), r = static_cast<uint16_t>(row);
    d.push_back(static_cast<uint8_t>(a)); d.push_back(static_cast<uint8_t>(a >> 8)); d.push_back(static_cast<uint8_t>(r)); d.push_back(static_cast<uint8_t>(r >> 8));
    const float strength = P.n[12] * M.n[8];                 // +0x20 / +0x24
    const float dur = P.n[10] + M.n[6];                      // +0x28 / +0x2c
    b.weight = P.n[8] * M.n[5];                              // +0x34
    b.price = static_cast<float>(P.ni[13]) * M.n[4] + P.n[7]; // +0x38
    b.durability = dur;
    PutF(d, strength); PutF(d, strength); PutF(d, dur); PutF(d, dur); PutF(d, 0.001f); PutF(d, b.weight); PutF(d, b.price); PutF(d, P.n[9]);
    PutI(d, -1);                                             // no attached spell
    PutI(d, count);                                          // +0x48
    PutI(d, P.ni[11] + M.ni[7]);                             // +0x40
    if (kind == 0x3004) {
        PutF(d, 0.0f); PutF(d, P.n[24] * M.n[10]); PutF(d, P.n[25] * M.n[10]); PutF(d, P.n[23]);
    } else {
        for (int pass = 0; pass < 2; ++pass) {
            const std::vector<float>& L = pass == 0 ? P.list21 : P.list22;
            for (int i = 0; i < 7; ++i) {
                const float s = L.size() > 0 ? L[0] : 0.0f, v = L.size() > static_cast<size_t>(1 + i) ? L[static_cast<size_t>(1 + i)] : 0.0f;
                const float m = M.list11.size() > static_cast<size_t>(i) ? M.list11[static_cast<size_t>(i)] : 0.0f;
                PutF(d, v * m * s);
            }
        }
    }
    return b;
}


// ---- the allod of a quest ---------------------------------------------------------------------------------------------------------------------------------------------------
// Every quest file (.mq, a res archive) holds "<quest>\map.txt" whose "#zone <zone> <allod> game" line names its allod; the lobby tells the bot the session's quest list and the chosen quest.
inline std::string AllodFromMq(const std::string& path, const std::string& quest) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res::Archive a; std::string err;
    if (!res::ParseArchive(bytes, a, err)) return "";
    const std::vector<uint8_t>* txt = a.Find(quest + "\\map.txt");
    if (!txt) return "";
    const std::string t(txt->begin(), txt->end());
    const size_t z = t.find("#zone ");
    if (z == std::string::npos) return "";
    std::string line = t.substr(z, t.find_first_of("\r\n", z) - z);
    std::vector<std::string> w; size_t i = 0;
    while (i < line.size()) { while (i < line.size() && line[i] == ' ') ++i; size_t j = line.find(' ', i); if (j == std::string::npos) j = line.size(); if (j > i) w.push_back(line.substr(i, j - i)); i = j; }
    return w.size() >= 3 ? w[2] : "";
}
// The built-in table (the mod's quests, 2026-10-20) for when the quest files are not at hand.
inline std::string AllodOfQuestBuiltin(const std::string& quest) {
    static const std::map<std::string, std::string> k = {
        {"z0jq1", "jigran"}, {"z11d2q1", "gipat"}, {"z11q1", "ingos"}, {"z11q2", "ingos"}, {"z12q1", "ingos"}, {"z12q2", "ingos"}, {"z12q3", "ingos"}, {"z13q1", "ingos"},
        {"z13q2", "ingos"}, {"z13q3", "ingos"}, {"z15q1", "suslanger"}, {"z17q1", "suslanger"}, {"z17q2", "suslanger"}, {"z17q3", "suslanger"}, {"z18q1", "suslanger"},
        {"z18q2", "suslanger"}, {"z18q3", "suslanger"}, {"z18q4", "suslanger"}, {"z19q1", "suslanger"}, {"z19q2", "suslanger"}, {"z19q3", "suslanger"}, {"z20q1", "final"},
        {"z25q1", "ingos"}, {"z26q1", "ingos"}, {"z31q1", "jigran"}, {"z35xq1", "jigran"}, {"z35xq2", "jigran"}, {"z3xq1", "gipat"}, {"z3xq2", "gipat"}, {"z3xq3", "gipat"},
        {"z5q1", "gipat"}, {"z5q2", "gipat"}, {"z6xq1", "gipat"}, {"z6xq2", "gipat"}, {"z6xq3", "gipat"}, {"z7q1", "gipat"}, {"z7q2", "gipat"}, {"z7q3", "gipat"}, {"z8xq1", "gipat"},
        {"z90q1", "gipat"}, {"z90q2", "gipat"}, {"z90q3", "gipat"}, {"z91q1", "gipat"}, {"z92q1", "gipat"}, {"z92q2", "gipat"}, {"z9xq1", "gipat"}, {"z9xq2", "gipat"}, {"z9xq3", "gipat"}};
    const auto it = k.find(quest);
    return it == k.end() ? "" : it->second;
}
// "z3xq1.mq" -> its allod: from <mapsDir>/z3xq1.mq when readable, else the built-in table.
inline std::string AllodOfQuest(std::string quest, const std::string& mapsDir) {
    if (quest.size() > 3 && quest.compare(quest.size() - 3, 3, ".mq") == 0) quest.erase(quest.size() - 3);
    if (!mapsDir.empty()) { const std::string a = AllodFromMq(mapsDir + "/" + quest + ".mq", quest); if (!a.empty()) return a; }
    return AllodOfQuestBuiltin(quest);
}

// ---- the shop's stock --------------------------------------------------------------------------------------------------------------------------------------------------------
// What the camp's item shop held in the live tests (Babur, three sessions): every weapon and armor prototype of the shop appears once, made of ONE material drawn among the materials
// of its class that the shop sells; weapons and armor in quantity 1 (an armor piece is missing now and then, about 4 %), the potions in a random quantity of 20-50. Each session draws again.
struct Offer {
    uint32_t kind = 0;   // 0x3004 weapon, 0x3005 armor, 0x3006 quick item
    int row = 0, material = 0, count = 1;
    float price = 0, weight = 0;  // per item
};
inline std::vector<Offer> Stock(const items::Database& db, int shopNo, std::mt19937& rng) {
    std::vector<Offer> out;
    if (shopNo < 1 || shopNo > 5) return out;
    const uint32_t bit = 1u << (shopNo - 1);
    for (int cat = 0; cat < 2; ++cat) {
        const uint32_t kind = cat == 0 ? 0x3004u : 0x3005u;
        for (const items::Item& it : db.List(cat == 0 ? items::Category::Weapons : items::Category::Armors)) {
            if (!(it.shopMask & bit)) continue;
            std::vector<int> mats;
            for (size_t m = 0; m < db.materials.size(); ++m) if ((db.materials[m].shopMask & bit) && db.materials[m].type == it.materialType) mats.push_back(static_cast<int>(m));
            if (mats.empty()) continue;                                  // e.g. "cheat dagger" (Metal: the shop sells no metal)
            if (cat == 1 && std::uniform_real_distribution<float>(0, 1)(rng) < 0.04f) continue;
            Offer o; o.kind = kind; o.row = it.row;
            o.material = mats[std::uniform_int_distribution<size_t>(0, mats.size() - 1)(rng)];
            const Built b = Build(db, kind, it.row, o.material, 1);
            o.price = b.price; o.weight = b.weight;
            out.push_back(o);
        }
    }
    for (const items::Item& it : db.List(items::Category::QuickItems)) {
        if (!(it.shopMask & bit) || it.type == "dangersensor" || it.type == "wand") continue; // (wands are flagged for the shop but never showed up in its stock)
        Offer o; o.kind = 0x3006; o.row = it.row; o.material = 0;
        o.count = std::uniform_int_distribution<int>(20, 50)(rng);
        o.price = it.n[7]; o.weight = it.n[8];
        out.push_back(o);
    }
    return out;
}


// ---- the planner -------------------------------------------------------------------------------------------------------------------------------------------------------------
// What the bot wants to wear: its class (the settings' role and Damage <-> Defence slider) or, when the user names gear, exactly that (priority), kept while possible.
struct GearWish { std::string name, material; };   // "bone long bow" / "bone long bow:dragon red bones" (empty material: any the shop offers)
struct Prefs {
    int role = 1;             // 0 melee, 1 ranged, 2 mage, 3 hybrid (the settings' Role)
    int tankiness = 30;       // 0 all damage .. 100 all defence: the share of the money that goes into armor
    std::vector<GearWish> wishes;
    float weightLimit = 0.95f; // never carry more than this part of the lift capacity
    int potionsWanted = 3;    // belt potions kept topped up (only of a kind it already has: the potion's spell object is made by the game)
};
// "bone long bow:dragon red bones; gipat highest plate" -> wishes.
inline std::vector<GearWish> ParseWishes(const std::string& text) {
    std::vector<GearWish> out; size_t i = 0;
    auto trim = [](std::string v) { while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back(); size_t k = 0; while (k < v.size() && (v[k] == ' ' || v[k] == '\t')) ++k; return v.substr(k); };
    while (i <= text.size()) {
        size_t j = text.find(';', i); if (j == std::string::npos) j = text.size();
        const std::string part = trim(text.substr(i, j - i));
        if (!part.empty()) { const size_t c = part.find(':'); GearWish w; w.name = trim(c == std::string::npos ? part : part.substr(0, c)); if (c != std::string::npos) w.material = trim(part.substr(c + 1)); out.push_back(w); }
        i = j + 1;
    }
    return out;
}

inline const mp::Object* FindObject(const mp::Character& ch, uint32_t id) {
    for (const auto& list : ch.lists) for (const mp::Object& o : list) if (o.id == id) return &o;
    return nullptr;
}
inline float DF(const mp::Object& o, size_t at) { float f = 0; if (at + 4 <= o.detail.size()) std::memcpy(&f, o.detail.data() + at, 4); return f; }
inline uint32_t DU(const mp::Object& o, size_t at) { uint32_t u = 0; if (at + 4 <= o.detail.size()) std::memcpy(&u, o.detail.data() + at, 4); return u; }
inline std::string Lower(std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; }

// The camp's price coefficients of shop 1 (GSPlayers_CreateDefaults: buy 1.0, sell 0.5, repair 0.2).
constexpr float kBuy = 1.0f, kSell = 0.5f, kRepair = 0.2f;

inline float WeaponScore(float lo, float hi) { return lo + hi; }
inline float ArmorScore(const std::vector<uint8_t>& d) { // two blocks of 7 floats from byte 52 and 80
    float s = 0;
    for (int i = 0; i < 7; ++i) { float a, b; std::memcpy(&a, &d[52 + 4 * i], 4); std::memcpy(&b, &d[80 + 4 * i], 4); s += a + 0.5f * b; }
    return s;
}
inline int WeaponGroup(int typeId) { return typeId; } // sword 0, axe 1, dagger 2, spear 3, hammer 4 (bludgeon), bow 5: the ability groups' order

struct Step { std::string text; };
struct Plan {
    std::vector<Offer> buy;                 // what to buy (the item bought replaces `replaces`, when set, in the same slot)
    std::vector<uint32_t> replaces;         // object id of the item each purchase replaces (0: none), parallel to buy
    std::vector<uint32_t> sell;             // object ids sold (the replaced gear and the loot)
    int potionRow = -1, potionAdd = 0;      // more of a potion the character already carries
    long money = 0;                         // money after the plan
    std::vector<std::string> notes;
};

// Decides what to buy and sell. `stock` is this session's draw (Stock()). Never exceeds the money or the lift capacity, never swaps wished gear away.
inline Plan MakePlan(const items::Database& db, const mp::Character& ch, const Prefs& pf, const std::vector<Offer>& stock) {
    Plan plan;
    if (ch.members.empty()) return plan;
    const mp::Member& me = ch.members[0];
    long money = static_cast<long>(mp::Money(ch));
    const float capacity = mp::GetF(me.stats, 0x54) > 1 ? mp::GetF(me.stats, 0x54) : 1e9f;
    float weight = 0;                                    // everything the character carries
    for (const mp::Object& o : ch.lists[0]) if (o.kind == 0x3004 || o.kind == 0x3005 || o.kind == 0x3006 || o.kind == 0x3007) weight += DF(o, 28);
    const float limit = capacity * pf.weightLimit;
    auto matName = [&](int m) { return m >= 0 && static_cast<size_t>(m) < db.materials.size() ? Lower(db.materials[static_cast<size_t>(m)].name) : std::string(); };
    auto itemName = [&](uint32_t kind, int row) { const auto& L = db.List(kind == 0x3004 ? items::Category::Weapons : items::Category::Armors); return static_cast<size_t>(row) < L.size() ? Lower(L[static_cast<size_t>(row)].name) : std::string(); };
    auto wished = [&](uint32_t kind, int row, int mat) {
        for (const GearWish& w : pf.wishes) if (Lower(w.name) == itemName(kind, row) && (w.material.empty() || Lower(w.material) == matName(mat))) return true;
        return false;
    };
    auto sellValue = [&](const mp::Object& o) { // the camp: price x 0.5, minus the repair price of the wear (price x wear x 0.2)
        const float price = DF(o, 32), cur = DF(o, 16), max = DF(o, 20);
        const float wear = max > 0 ? 1.0f - cur / max : 0.0f;
        const uint32_t count = o.kind == 0x3007 || o.kind == 0x3006 ? std::max<uint32_t>(1, DU(o, 44)) : 1;
        return std::max(0.0f, price * kSell - price * wear * kRepair) * static_cast<float>(count);
    };
    // 1. The loot: sold, all of it (money 0 is not an item; quest items never leave).
    for (const mp::Object& o : ch.lists[0]) {
        if (o.kind != 0x3007 || o.b == 0) continue;     // loot row 0 is money
        const float v = sellValue(o);
        if (v < 1) continue;
        plan.sell.push_back(o.id); money += static_cast<long>(v); weight -= DF(o, 28);
        plan.notes.push_back("sell loot id " + std::to_string(o.id) + " for " + std::to_string(static_cast<long>(v)));
    }
    // 2. What it wears now.
    std::map<int, const mp::Object*> armorBy;          // armor type -> worn piece
    for (uint32_t id : me.lists[2]) if (const mp::Object* o = FindObject(ch, id)) if (o->kind == 0x3005 && static_cast<size_t>(o->b) < db.List(items::Category::Armors).size()) armorBy[db.List(items::Category::Armors)[o->b].typeId] = o;
    const mp::Object* weapon = nullptr;
    for (uint32_t id : me.lists[0]) if (const mp::Object* o = FindObject(ch, id)) if (o->kind == 0x3004) { weapon = o; break; }
    // 3. The wishes first (priority): each is bought when the shop offers it now and the money / lift allow; worn wished gear is kept.
    std::map<int, bool> armorDone; bool weaponDone = false;
    long keep = 0;                                         // money the step must leave (set after the wishes)
    auto trySlot = [&](const Offer& o, const mp::Object* current, const std::string& why) {
        const float newW = o.weight - (current ? DF(*current, 28) : 0.0f);
        const float gain = current ? sellValue(*current) : 0.0f;
        if (static_cast<float>(money) + gain - static_cast<float>(keep) < o.price * kBuy || weight + newW > limit) return false;
        money += static_cast<long>(gain - o.price * kBuy); weight += newW;
        plan.buy.push_back(o); plan.replaces.push_back(current ? current->id : 0);
        if (current) plan.sell.push_back(current->id);
        plan.notes.push_back(why + ": " + itemName(o.kind, o.row) + " (" + matName(o.material) + ") for " + std::to_string(static_cast<long>(o.price * kBuy)));
        return true;
    };
    for (const GearWish& w : pf.wishes) {
        bool haveIt = false;
        for (const mp::Object* o : {weapon}) if (o && wished(o->kind, o->b, o->a) && Lower(itemName(o->kind, o->b)) == Lower(w.name)) { haveIt = true; weaponDone = true; }
        for (auto& kv : armorBy) { const mp::Object* o = kv.second; if (wished(o->kind, o->b, o->a) && Lower(itemName(o->kind, o->b)) == Lower(w.name)) { haveIt = true; armorDone[kv.first] = true; } }
        if (haveIt) continue;
        for (const Offer& o : stock) {
            if ((o.kind != 0x3004 && o.kind != 0x3005) || !wished(o.kind, o.row, o.material)) continue;
            if (o.kind == 0x3004) { if (trySlot(o, weapon, "wished weapon")) weaponDone = true; }
            else { const int t = db.List(items::Category::Armors)[static_cast<size_t>(o.row)].typeId; const auto it = armorBy.find(t); if (trySlot(o, it == armorBy.end() ? nullptr : it->second, "wished armor")) armorDone[t] = true; }
            break;
        }
    }
    // Money kept back: for wished gear not owned yet (the shop draws its materials anew each session, so the wished one may turn up later: its cheapest price is saved) and a few potions.
    long reserve = 0;
    for (const GearWish& w : pf.wishes) {
        bool owned = false;
        for (const mp::Object* o : {weapon}) if (o && Lower(itemName(o->kind, o->b)) == Lower(w.name) && (w.material.empty() || Lower(w.material) == matName(o->a))) owned = true;
        for (auto& kv : armorBy) if (Lower(itemName(kv.second->kind, kv.second->b)) == Lower(w.name) && (w.material.empty() || Lower(w.material) == matName(kv.second->a))) owned = true;
        if (owned) continue;
        float cheapest = 0;
        for (int cat = 0; cat < 2; ++cat) for (const items::Item& P : db.List(cat == 0 ? items::Category::Weapons : items::Category::Armors)) {
            if (Lower(P.name) != Lower(w.name)) continue;
            for (size_t m = 0; m < db.materials.size(); ++m) {
                if (db.materials[m].type != P.materialType || (!w.material.empty() && Lower(db.materials[m].name) != Lower(w.material))) continue;
                const float price = Build(db, cat == 0 ? 0x3004 : 0x3005, P.row, static_cast<int>(m), 1).price;
                if (cheapest == 0 || price < cheapest) cheapest = price;
            }
        }
        reserve += static_cast<long>(cheapest);
    }
    reserve += std::min<long>(money / 10, 200);
    if (reserve > money) reserve = money;
    keep = reserve;
    // 4. The rest by class: the weapon of the role's kind with the best damage x the character's ability, then armor by the Damage <-> Defence share.
    const long armorBudget = money * pf.tankiness / 100, weaponBudget = money - armorBudget;
    (void)armorBudget; (void)weaponBudget;
    auto ability = [&](int group) { const size_t at = mp::kPerkBase + static_cast<size_t>(group); return at < me.stats.size() ? static_cast<int>(me.stats[at]) : 0; };
    auto allowed = [&](int typeId) {
        switch (pf.role) { case 0: return typeId <= 4; case 1: return typeId == 5; case 2: return typeId == 2 || typeId == 5; default: return true; }
    };
    if (!weaponDone) {
        float bestScore = weapon ? WeaponScore(DF(*weapon, 56), DF(*weapon, 60)) * (1 + 0.35f * ability(WeaponGroup(db.List(items::Category::Weapons)[weapon->b].typeId))) : 0.0f;
        const Offer* best = nullptr;
        for (const Offer& o : stock) {
            if (o.kind != 0x3004) continue;
            const items::Item& P = db.List(items::Category::Weapons)[static_cast<size_t>(o.row)];
            if (!allowed(P.typeId)) continue;
            const items::Material& M = db.materials[static_cast<size_t>(o.material)];
            const float score = WeaponScore(P.n[24] * M.n[10], P.n[25] * M.n[10]) * (1 + 0.35f * ability(WeaponGroup(P.typeId)));
            const float cost = o.price * kBuy - (weapon ? sellValue(*weapon) : 0.0f);
            if (score > bestScore * 1.10f && static_cast<float>(money - keep) >= cost && weight + o.weight - (weapon ? DF(*weapon, 28) : 0.0f) <= limit && static_cast<long>(cost) <= money * (100 - pf.tankiness) / 100 + 1) { best = &o; bestScore = score; }
        }
        if (best) trySlot(*best, weapon, "weapon");
    }
    for (int t = 0; t <= 6; ++t) {
        if (armorDone[t]) continue;
        const auto cur = armorBy.find(t);
        float bestScore = cur != armorBy.end() ? ArmorScore(cur->second->detail) : 0.0f;
        const Offer* best = nullptr;
        for (const Offer& o : stock) {
            if (o.kind != 0x3005 || db.List(items::Category::Armors)[static_cast<size_t>(o.row)].typeId != t) continue;
            const Built b = Build(db, 0x3005, o.row, o.material, 1);
            const float score = ArmorScore(b.detail);
            const float cost = o.price * kBuy - (cur != armorBy.end() ? sellValue(*cur->second) : 0.0f);
            if (score > bestScore * 1.08f && static_cast<float>(money - keep) >= cost && weight + o.weight - (cur != armorBy.end() ? DF(*cur->second, 28) : 0.0f) <= limit && (pf.tankiness > 0)) { best = &o; bestScore = score; }
        }
        if (best) trySlot(*best, cur != armorBy.end() ? cur->second : nullptr, "armor");
    }
    // 5. Potions of a kind it carries: topped up to potionsWanted.
    for (const mp::Object& o : ch.lists[0]) {
        if (o.kind != 0x3006) continue;
        const int have = static_cast<int>(std::max<uint32_t>(1, DU(o, 44)));
        for (const Offer& st : stock) if (st.kind == 0x3006 && st.row == o.b && have < pf.potionsWanted) {
            const int add = std::min(pf.potionsWanted - have, st.count);
            if (static_cast<float>(money) >= st.price * kBuy * static_cast<float>(add) && weight + st.weight * static_cast<float>(add) <= limit) {
                plan.potionRow = st.row; plan.potionAdd = add; money -= static_cast<long>(st.price * kBuy * static_cast<float>(add)); weight += st.weight * static_cast<float>(add);
                plan.notes.push_back("potion row " + std::to_string(st.row) + " +" + std::to_string(add));
            }
        }
    }
    plan.money = money;
    return plan;
}

// Applies a plan to the character: sells, buys (new objects built from the database), equips, tops up the potions, sets the money.
inline std::string Apply(const items::Database& db, mp::Character& ch, const Plan& plan) {
    if (ch.members.empty()) return "";
    mp::Member& me = ch.members[0];
    auto erase = [&](std::vector<uint32_t>& v, uint32_t id) { v.erase(std::remove(v.begin(), v.end(), id), v.end()); };
    uint32_t next = 1;
    for (const auto& list : ch.lists) for (const mp::Object& o : list) next = std::max(next, o.id + 1);
    for (uint32_t id : plan.sell) {
        for (int k = 0; k < 4; ++k) erase(me.lists[k], id);
        erase(ch.backpack, id);
        ch.lists[0].erase(std::remove_if(ch.lists[0].begin(), ch.lists[0].end(), [&](const mp::Object& o) { return o.id == id; }), ch.lists[0].end());
    }
    for (const Offer& o : plan.buy) {
        const Built b = Build(db, o.kind, o.row, o.material, next);
        mp::Object obj; obj.id = next++; obj.kind = o.kind; obj.a = static_cast<uint16_t>(o.material); obj.b = static_cast<uint16_t>(o.row); obj.detail = b.detail;
        ch.lists[0].push_back(obj);
        me.lists[o.kind == 0x3004 ? 0 : 2].push_back(obj.id); // worn: the weapon slot / the armor slots
    }
    if (plan.potionRow >= 0) for (mp::Object& o : ch.lists[0]) if (o.kind == 0x3006 && o.b == plan.potionRow && o.detail.size() >= 48) {
        const uint32_t have = std::max<uint32_t>(1, DU(o, 44)), now = have + static_cast<uint32_t>(plan.potionAdd);
        std::memcpy(o.detail.data() + 44, &now, 4);
        const float unit = DF(o, 28) / static_cast<float>(have), w = unit * static_cast<float>(now);
        std::memcpy(o.detail.data() + 28, &w, 4);
    }
    mp::SetMoney(ch, static_cast<uint32_t>(std::max(0L, plan.money)));
    std::string text = std::to_string(plan.buy.size()) + " bought, " + std::to_string(plan.sell.size()) + " sold, money " + std::to_string(plan.money);
    return text;
}

} // namespace shop
