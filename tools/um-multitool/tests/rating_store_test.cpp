// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I.. tests/rating_store_test.cpp -o /tmp/rating_store_test && /tmp/rating_store_test
#include <cstdio>
#include <cstdlib>

#include "mapedit/rating_store.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    ratingstore::Store a;
    ratingstore::Entry e;
    e.score = 5641.3; e.pts = {2391, 856, 994, 1400, 0}; e.units = 68; e.fights = 11; e.spawns = 2; e.objectives = 2; e.route = 451.5f; e.exposure = 0.2345f;
    e.allod = "Ingos"; e.stamp = "v1|db:123:456|map.txt:10:20"; e.rated = "2026-11-11";
    a.entries[ratingstore::Key("Z12Q2")] = e;
    a.locks[ratingstore::Key("Ingos")] = {3200, 5600};
    const std::string path = "/tmp/claude-1000/rating_store_test.tsv";
    CHECK(a.Save(path));
    ratingstore::Store b;
    CHECK(b.Load(path));
    CHECK(b.entries.size() == 1 && b.entries.count("z12q2"));
    const ratingstore::Entry& r = b.entries["z12q2"];
    CHECK(r.units == 68 && r.fights == 11 && r.objectives == 2 && r.allod == "Ingos" && r.stamp == e.stamp && r.rated == "2026-11-11");
    CHECK(std::abs(r.score - 5641.3) < 0.06 && std::abs(r.pts[1] - 856) < 0.06 && std::abs(r.exposure - 0.2345f) < 1e-3f);
    CHECK(b.locks.size() == 1 && b.locks["ingos"].lo == 3200 && b.locks["ingos"].hi == 5600);
    ratingstore::Store c;
    CHECK(!c.Load("/nonexistent/none.tsv") && c.entries.empty());
    std::remove(path.c_str());
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
