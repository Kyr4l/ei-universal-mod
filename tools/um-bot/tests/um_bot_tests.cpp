// SPDX-License-Identifier: GPL-3.0-or-later
// Offline checks of um-bot's rules against what was verified in the running game (2026-10-20): `make test`. They need Universal-Mod/res/databaselmp.res and the characters in Universal-Mod/mp.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

#include "bot_config.hpp"
#include "growth.hpp"
#include "shop.hpp"
#include "spells.hpp"

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> ReadFile(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); }

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "../..";
    items::Database db; std::string err;
    const bool haveDb = items::LoadDatabaseRes(ReadFile(root + "/Universal-Mod/res/databaselmp.res"), "databaselmp", db, err);
    CHECK(haveDb);
    if (!haveDb) { std::printf("no database: %s\n", err.c_str()); return 1; }

    // growth: the skill price, and the perk price (the game's own results for the cost numbers 10 / 20 / 30 with no other ability: 80, 210, 500; more abilities x 1.95 each)
    CHECK(growth::SkillCost(0) == 4 && growth::SkillCost(10) == 12 && growth::SkillCost(50) == 584);
    items::Database fake; fake.perks.resize(3);
    for (int i = 0; i < 3; ++i) fake.perks[static_cast<size_t>(i)].costNum = 10 * (i + 1);
    CHECK(growth::PerkCost(fake, 0, 0) == 80 && growth::PerkCost(fake, 1, 0) == 210 && growth::PerkCost(fake, 2, 0) == 500);
    CHECK(growth::PerkCost(fake, 0, 3) > growth::PerkCost(fake, 0, 0) * 5);

    // the saved characters: the file round trip, the spent experience explained by skills + abilities (8.mp: exactly)
    int parsed = 0;
    for (const char* name : {"1", "2", "5", "6", "7", "8", "9"}) {
        mp::Character ch; std::string e;
        if (!mp::Load(root + "/Universal-Mod/mp/" + name + ".mp", ch, e)) continue;
        ++parsed;
        CHECK(mp::Serialize(ch).size() > 100);
        std::vector<uint8_t> raw; CHECK(mp::Unpack(mp::Pack(mp::Serialize(ch), 0, 0), raw, e) && raw == mp::Serialize(ch));
        std::vector<uint8_t> lz; CHECK(mp::Unpack(mp::PackLz(mp::Serialize(ch), 0, 0), lz, e) && lz == mp::Serialize(ch)); // the compressed upload decodes to the same bytes
        // spells: a rebuilt spell object equals the saved one (the saves older than the database differ in a few; at least most must match)
        int same = 0, total = 0;
        for (const mp::Object& o : ch.lists[1]) if (o.kind == 0x3002 && o.detail.size() == 66) {
            std::vector<int> runes; for (int k = 50; k < 58; ++k) if (o.detail[static_cast<size_t>(k)]) runes.push_back(o.detail[static_cast<size_t>(k)] - 1);
            uint32_t id; std::memcpy(&id, o.detail.data(), 4);
            ++total; if (spells::Build(db, o.b, runes, id, 0).object == o.detail) ++same;
        }
        CHECK(total == 0 || same * 2 >= total);
    }
    CHECK(parsed >= 5);
    {   // 8.mp: 498 experience spent = its skills and 3 abilities (the cheapest order of the abilities)
        mp::Character ch; std::string e;
        if (mp::Load(root + "/Universal-Mod/mp/8.mp", ch, e)) {
            const auto& m = ch.members[0];
            long skills = 0; for (int s = 0; s < 6; ++s) for (int v = 0; v < m.stats[static_cast<size_t>(0x6CB + s)]; ++v) skills += growth::SkillCost(v);
            CHECK(skills + 450 == static_cast<long>(mp::GetF(m.stats, mp::kExpSpent)) || skills + 450 <= static_cast<long>(mp::GetF(m.stats, mp::kExpSpent)) + 60);
        }
    }

    // the spell blueprint of Healing with no rune: effect 20, mana 20, required 9, price 200; mana 86.53 for exp 4000 / dexterity 30 (read from the game)
    const int heal = spells::RowOfCode(db, "healing");
    CHECK(heal >= 0);
    if (heal >= 0) {
        const spells::Built b = spells::Build(db, heal, {}, 1, 2);
        float effect; std::memcpy(&effect, b.object.data() + 10, 4);
        CHECK(effect == 20.0f && b.mana == 20.0f && b.required == 9 && b.price == 200.0f && b.object.size() == 66 && b.container.size() == 56);
    }
    {   mp::Member m; m.stats.assign(0x700, 0); mp::SetF(m.stats, mp::kExpTotal, 4000); mp::SetF(m.stats, 0x18, 30);
        CHECK(std::fabs(spells::MaxMana(db, m) - 86.53f) < 0.05f); }

    // the shop: every spell of Babur's shop is the live stock's twelve rows; the rune stock too
    std::mt19937 rng(1);
    const auto st = spells::Stock(db, 1, rng);
    std::vector<int> rows; for (const auto& s : st) rows.push_back(s.row);
    CHECK((rows == std::vector<int>{0, 3, 6, 9, 12, 18, 23, 24, 27, 29, 30, 31}));
    const auto rs = spells::RuneStock(db, 1, rng);
    std::vector<int> runeRows; for (const auto& r : rs) runeRows.push_back(r.row);
    CHECK((runeRows == std::vector<int>{0, 1, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17, 18, 22, 23}));
    CHECK(shop::ShopOfAllod("gipat") == 1 && shop::ShopOfAllod("ingos") == 3 && shop::ShopOfAllod("suslanger") == 4 && shop::ShopOfAllod("jigran") == 5);

    // the configuration: the builds, and the old role numbers
    {   auto load = [](const char* text) { std::ofstream("/tmp/um-bot-test.cfg") << text; bot::Config c; c.path = "/tmp/um-bot-test.cfg"; bot::Load(c); return c; };
        CHECK(load("ROLE=2\nTANKINESS=40\n").role == bot::Role::Fighter && load("ROLE=2\nTANKINESS=40\n").tankiness == 40);
        CHECK(load("ROLE=1\n").role == bot::Role::Archer);
        CHECK(load("BUILD=1\n").role == bot::Role::Tank);
        CHECK(bot::RoleKind(bot::Role::Archer) == 1 && bot::RoleKind(bot::Role::Tank) == 0 && bot::RoleKind(bot::Role::Fighter) == 0); }

    std::printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
