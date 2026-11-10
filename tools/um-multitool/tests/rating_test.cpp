// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I. -I.. -I../../../resources/universal-mod/um-dll tests/rating_test.cpp mapedit/rating.cpp -o /tmp/rating_test && /tmp/rating_test
#include <cstdio>

#include "mapedit/rating.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// A database with a hero, a goblin and a boss (columns as the real Monsters sheet titles them, titles in row 2, units from row 4).
static dbmodel::Book MakeBook() {
    sheetio::Sheet s;
    s.name = "Monsters";
    const char* cols[] = {"Name", "Stats Health", "Stats Absorbtion", "Tuning Actions", "Tuning Move", "Attack Range", "Stats To Hit", "Stats Parry",
                          "Stats Danage Nub", "Stats Damage Max", "Senses Sight"};
    int c = 2;
    for (const char* t : cols) s.SetText(2, c++, t);
    struct Row { const char* name; double v[10]; };
    const Row rows[] = {
        {"Human Hero", {22, 14, 50, 1, 0, 12, 10, 2, 2, 13}},
        {"Goblin", {40, 0, 50, 1, 0, 12, 10, 4, 4, 8.5}},
        {"Ogre", {250, 5, 50, 1, 0, 14, 10, 12, 6, 10}},
        {"Boss", {400, 10, 50, 1, 0, 20, 10, 20, 10, 12}},
    };
    int row = 4;
    for (const Row& r : rows) {
        s.SetText(row, 2, r.name);
        for (int i = 0; i < 10; ++i) s.SetNumber(row, 3 + i, std::to_string(r.v[i]));
        ++row;
    }
    return {s};
}

static mob::Object Unit(const char* proto, float x, float y, uint32_t id, int player = 1) {
    mob::Object o;
    o.kind = mob::Kind::Unit;
    o.prototype = proto;
    o.position = {x, y, 0};
    o.id = id;
    o.hasId = true;
    o.player = player;
    return o;
}

int main() {
    rating::Params params;
    rating::Monsters db;
    std::string err;
    CHECK(rating::LoadMonsters(MakeBook(), params, db, err));
    CHECK(db.byName.size() == 4 && db.haveHero);
    CHECK(db.anchorName == "Boss" && db.anchorUnit > 0);
    CHECK(db.Find("goblin") != nullptr && db.Find("nobody") == nullptr);
    const rating::UnitPower gob = rating::PowerOf(*db.Find("Goblin"), db, params), boss = rating::PowerOf(*db.Find("Boss"), db, params);
    CHECK(gob.power > 0 && boss.power > gob.power * 4);

    mob::File f;
    f.loaded = true;
    rating::Input in;
    in.files = {&f};
    // nothing: an empty map is 0
    rating::Result r = rating::Rate(in, db, params);
    CHECK(r.ok && r.total == 0 && r.units == 0);

    // two goblins far apart are two pulls; moved together they are one (call-for-help radius 7)
    f.objects = {Unit("Goblin", 10, 10, 1), Unit("Goblin", 60, 60, 2)};
    r = rating::Rate(in, db, params);
    CHECK(r.units == 2 && r.pulls.size() == 2);
    const double apart = r.total;
    f.objects[1].position = {14, 10, 0};
    r = rating::Rate(in, db, params);
    CHECK(r.pulls.size() == 1 && r.pulls[0].units == 2);
    CHECK(r.pts[rating::Density] > 0);

    // more and stronger enemies score higher; the score stays inside 0..10000
    f.objects.push_back(Unit("Boss", 14, 14, 3));
    const rating::Result bossRun = rating::Rate(in, db, params);
    CHECK(bossRun.total > r.total && bossRun.total <= 10000);
    CHECK(apart > 0);

    // the player's group and neutral groups are not enemies
    f.objects = {Unit("Boss", 10, 10, 1, 0)};
    CHECK(rating::Rate(in, db, params).units == 0);
    f.hasDiplomacy = true;
    f.diplomacy.assign(32 * 32, 1);
    f.objects = {Unit("Boss", 10, 10, 1, 5)};
    CHECK(rating::Rate(in, db, params).units == 0); // group 5 is neutral to group 0
    f.diplomacy[5 * 32] = 2;
    CHECK(rating::Rate(in, db, params).units == 1);
    f.hasDiplomacy = false;

    // a zone's base map holds the whole population: a quest meets only the units it names or that are near its walk
    mob::File base, quest;
    base.loaded = quest.loaded = true;
    base.objects = {Unit("Goblin", 10, 10, 11), Unit("Goblin", 200, 200, 12), Unit("Goblin", 250, 20, 13)};
    quest.hasScript = true;
    quest.script = "WorldScript ( AddObject( Chiefs, GetObject( 12 ) ) QStart( \"q\" ) QObjKillGroup( \"Chiefs\" ) )";
    rating::Input q;
    q.files = {&base, &quest};
    q.baseCount = 1;
    q.deploy = {true, 0, 0, 4, 4};
    q.remove = {true, 0, 0, 4, 4};
    r = rating::Rate(q, db, params);
    CHECK(r.objectives == 1);
    CHECK(r.units == 2); // unit 12 (named) and unit 11 (beside the walk), not 13 (far from it)
    q.baseCount = 0;     // rated alone: every unit counts
    CHECK(rating::Rate(q, db, params).units == 3);

    // scripted spawns raise the ambush factor
    quest.script = "WorldScript ( AddUnitToServer( \"a1\", \"Goblin\", \"\", 5, 5, 0 ) AddUnitToServer( \"a2\", \"Boss\", \"\", 5, 5, 0 ) )";
    q.baseCount = 1;
    r = rating::Rate(q, db, params);
    CHECK(r.spawnSites == 2 && r.spawnUnits == 2 && r.pts[rating::Ambush] > 0);

    // the factors add up to the total
    double sum = 0;
    for (double p : r.pts) sum += p;
    CHECK(sum - r.total < 1e-6 && r.total - sum < 1e-6);
    CHECK(std::string(rating::TierName(100)) == "Easy" && std::string(rating::TierName(9000)) == "Brutal");

    // an outlier unit is not the anchor
    dbmodel::Book withJoke = MakeBook();
    withJoke[0].SetText(8, 2, "Joke");
    const double vals[] = {1e6, 0, 50, 1, 0, 100, 0, 100, 100, 8};
    for (int i = 0; i < 10; ++i) withJoke[0].SetNumber(8, 3 + i, std::to_string(vals[i]));
    rating::Monsters db2;
    CHECK(rating::LoadMonsters(withJoke, params, db2, err));
    CHECK(db2.anchorName == "Boss");

    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
