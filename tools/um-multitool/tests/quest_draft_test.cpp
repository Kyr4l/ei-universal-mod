// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I. -I.. -I../../../resources/universal-mod/um-dll tests/quest_draft_test.cpp -o /tmp/quest_draft_test && /tmp/quest_draft_test
#include <cstdio>
#include <fstream>

#include "mapedit/quest_draft.hpp"
#include "mapedit/script_ast.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static qdraft::Draft Sample() {
    qdraft::Draft d;
    d.name = "z26qtest";
    d.title = "Cave Cleaning";
    d.summary = "Clear the cave.";
    d.terrain = "zone26";
    d.baseMap = "zone26-lmp";
    d.region = "ingos";
    d.sky = "cave";
    d.exitTitle = "To Town";
    d.exitTarget = "bz1mpg 1";
    d.deploy = {true, 11.5f, 11, 15, 14};
    d.remove = {true, 6, 7, 13, 11};
    d.view = 0.69f;
    d.mapsBlock = "4 6\nzone26map zone26quest";
    d.groups.push_back({"Goblins", "Goblin Archer", "ungobl", 5, 60, 80, 6});
    d.groups.push_back({"Boss", "Goblin Chief", "ungobl", 1, 100, 120, 3});
    qdraft::Objective kill;
    kill.title = "Kill the goblins";
    kill.text = "They are in the cave.";
    kill.group = 0;
    qdraft::Objective area;
    area.kind = qdraft::Objective::Area;
    area.title = "Reach the end";
    area.text = "Walk to the back of the cave.";
    area.x = 100;
    area.y = 118;
    d.objectives = {kill, area};
    d.intro = {{"Hero", "What is it?"}, {"Hero", "I will do it."}};
    d.outro = {{"Hero", "Done."}};
    d.exp = 1000;
    d.money = 2000;
    d.tag = "Medium";
    return d;
}

int main() {
    qdraft::Draft d = Sample();
    CHECK(qdraft::Problems(d).empty());
    CHECK(!qdraft::Problems(d, {"Z26QTEST"}).empty());
    qdraft::Draft bad = d;
    bad.name = "1x";
    CHECK(!qdraft::Problems(bad).empty());

    // the quest map: 6 units from the first ID, in the ID range, with the script that names them
    mob::File f;
    std::string err;
    CHECK(qdraft::BuildMob(d, f, err));
    CHECK(f.loaded && f.objects.size() == 6);
    d.firstId = qdraft::NextIdBlock({});
    CHECK(d.firstId == 1000000);
    CHECK(qdraft::BuildMob(d, f, err) && f.objects.size() == 6);
    CHECK(f.objects[0].id == 1000000 && f.objects[5].id == 1000005);
    CHECK(f.objects[0].prototype == "Goblin Archer" && f.objects[5].prototype == "Goblin Chief");
    CHECK(f.objects[0].needImport && f.objects[0].templ == "ungobl");
    CHECK(f.secRanges.size() == 1 && f.secRanges[0].min == 1000000 && f.secRanges[0].max == 1000099);
    CHECK(std::fabs(f.objects[5].position.x - 100) < 0.01f && std::fabs(f.objects[5].position.y - 120) < 0.01f);
    for (int i = 0; i < 5; ++i) CHECK(std::hypot(f.objects[i].position.x - 60, f.objects[i].position.y - 80) <= 6.01f);
    CHECK(f.hasScript);
    // saved and read back
    mob::File g;
    g.bytes = f.bytes;
    g.loaded = mob::Parse(g);
    CHECK(g.loaded && g.objects.size() == 6 && g.script == f.script);
    CHECK(g.objects[2].logics.size() >= 1 && std::fabs(g.objects[2].logics[0].guardPlace.x - g.objects[2].position.x) < 0.01f);

    // the script reads, and the checker finds nothing wrong
    const scriptast::Program p = scriptast::Parse(f.script);
    CHECK(p.errorLine == 0 && p.scripts.size() == 2);
    const MobScriptReport r = CheckMobScript(f.script);
    if (r.errors) for (const auto& i : r.issues) std::printf("  checker: %s\n", i.message.c_str());
    CHECK(r.errors == 0);
    CHECK(f.script.find("QObjKillGroup( \"Goblins\" )") != std::string::npos && f.script.find("QObjArea( 1 )") != std::string::npos);
    CHECK(f.script.find("AddObject( Boss, GetObject( 1000005 )") != std::string::npos);

    // no units / only a few: the range still holds the IDs
    qdraft::Draft one = d;
    one.groups.resize(1);
    one.groups[0].count = 1;
    CHECK(qdraft::BuildMob(one, f, err) && f.objects.size() == 1);

    // the files: map.txt reads back as the quest it was written from, the .mq archive holds all of them
    quest::Quest q;
    quest::Parse(qdraft::MapTxt(d), q);
    CHECK(q.name == "z26qtest" && q.region == "ingos" && q.terrain == "zone26" && q.baseMap == "zone26-lmp" && q.sky == "cave");
    CHECK(q.exits.size() == 1 && q.exits[0].target == "bz1mpg 1" && q.exits[0].deploy.set && q.exits[0].deploy.x1 == 11.5f && q.exits[0].remove.y2 == 11);
    const std::string ini = qdraft::QuestIni(d);
    CHECK(ini.find("give quests=q.z26qtest.z26qtest.2") != std::string::npos && ini.find("give quests=q.z26qtest.z26qtest.3") == std::string::npos);
    CHECK(ini.find("exp=1000") != std::string::npos && ini.find("money=2000") != std::string::npos);
    CHECK(qdraft::QuestText(d).find("#subobj  2") != std::string::npos);
    CHECK(qdraft::BriefingReceive(d).rfind("[Medium] Cave Cleaning", 0) == 0);
    CHECK(qdraft::BriefingReceive(d).find("#phrase Hero  2") != std::string::npos);

    std::map<std::string, std::vector<uint8_t>> files;
    auto put = [&](const std::string& name, const std::string& text) { files[name] = std::vector<uint8_t>(text.begin(), text.end()); };
    put("quest z26qtest", qdraft::QuestText(d));
    put("z26qtest\\map.txt", qdraft::MapTxt(d));
    const std::vector<uint8_t> arc = res::WriteArchive(files, 1);
    const std::string path = "/tmp/quest_draft_test.mq";
    { std::ofstream o(path, std::ios::binary); o.write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size())); }
    quest::Quest back;
    CHECK(quest::Load(path, back) && back.name == "quest_draft_test" && back.terrain == "zone26");

    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
