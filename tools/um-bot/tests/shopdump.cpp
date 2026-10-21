// SPDX-License-Identifier: GPL-3.0-or-later
// What the shopping planner does for a character: g++ -std=c++17 -I.. -I../../um-multitool shopdump.cpp -o shopdump && ./shopdump <character.mp> [repo root]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include "bot_config.hpp"
#include "shop.hpp"
static std::vector<uint8_t> ReadFile(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); }
int main(int argc, char** argv) {
    const std::string root = argc > 2 ? argv[2] : "../..";
    items::Database db; std::string err;
    if (!items::LoadDatabaseRes(ReadFile(root + "/Universal-Mod/res/databaselmp.res"), "databaselmp", db, err)) { std::printf("db: %s\n", err.c_str()); return 1; }
    mp::Character ch; if (!mp::Load(argv[1], ch, err)) { std::printf("mp: %s\n", err.c_str()); return 1; }
    const mp::Member& me = ch.members[0];
    float carried = 0; for (const mp::Object& o : ch.lists[0]) if (o.kind == 0x3004 || o.kind == 0x3005 || o.kind == 0x3006 || o.kind == 0x3007) carried += shop::DF(o, 28);
    std::printf("money %ld, capacity %.1f, carried %.1f, archery ability %d\n", static_cast<long>(mp::Money(ch)), mp::GetF(me.stats, 0x54), carried, static_cast<int>(me.stats[mp::kPerkBase + 5]));
    for (int shopNo : {1, 3}) {
        std::mt19937 rng(1);
        auto stock = shop::Stock(db, shopNo, rng);
        std::printf("shop %d:\n", shopNo);
        for (auto& o : stock) if (o.kind == 0x3004) { const auto& P = db.List(items::Category::Weapons)[o.row]; if (P.typeId >= 5) { const auto& M = db.materials[o.material]; std::printf("   bow: %s (%s) price %.0f weight %.1f\n", P.name.c_str(), M.name.c_str(), o.price, o.weight); } }
        shop::Prefs pf; pf.role = 1; pf.tankiness = 25;
        auto plan = shop::MakePlan(db, ch, pf, stock);
        std::printf("  plan: buy %zu sell %zu money %ld\n", plan.buy.size(), plan.sell.size(), plan.money);
        for (auto& n : plan.notes) std::printf("  note: %s\n", n.c_str());
    }
    return 0;
}
