// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I.. tests/allod_range_test.cpp -o /tmp/allod_range_test && /tmp/allod_range_test
#include <cstdio>

#include "mapedit/allod_range.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    using namespace allodrange;
    // the mockup's sample: Gipath 7 quests with one far outlier, Ingos 5, Suslanger 4, Jigran 5, a fifth allod without quests
    const std::vector<std::vector<double>> scores = {
        {1180, 1420, 1710, 2380, 2620, 2850, 5420}, {3120, 3490, 4410, 4760, 5090}, {5230, 5750, 6320, 6860}, {6990, 7410, 4380, 8120, 8500}, {}};
    const std::vector<Range> r = Compute(scores, std::vector<Lock>(5));
    CHECK(r.size() == 5);
    CHECK(r[0].lo == 1200 && r[0].hi == 3550);       // the 5420 outlier does not stretch the range
    CHECK(r[1].lo == 3200 && r[1].hi == 5600);
    CHECK(!r[0].estimated && !r[3].estimated && r[4].estimated);
    CHECK(r[4].lo >= r[3].lo && r[4].hi <= 10000);   // beyond the last allod: one step up, capped
    double d = 0;
    CHECK(Judge(5420, r[0], &d) == Verdict::Above && d > 1800);
    CHECK(Judge(4380, r[3], &d) == Verdict::Below && d > 1700);
    CHECK(Judge(1180, r[0]) == Verdict::InRange);    // 20 points outside: within the tolerance
    CHECK(Judge(3100, r[1]) == Verdict::InRange && Judge(3000, r[1]) == Verdict::Below);

    // a locked range wins over the estimate
    std::vector<Lock> locks(5);
    locks[1] = {true, 4000, 6000};
    const std::vector<Range> l = Compute(scores, locks);
    CHECK(l[1].locked && l[1].lo == 4000 && l[1].hi == 6000);
    CHECK(!l[0].locked);

    // an allod with two rated quests is estimated from its neighbours
    const std::vector<Range> few = Compute({{2000, 2200, 1900, 2100, 2050}, {4000, 4100}, {6000, 6100, 5900, 6200, 6050}}, std::vector<Lock>(3));
    CHECK(few[1].estimated && few[1].lo < 4100 && few[1].hi > 3900);
    CHECK(Compute({}, {}).empty());
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
