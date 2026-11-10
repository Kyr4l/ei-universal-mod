// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I.. tests/allods_test.cpp -o /tmp/allods_test && /tmp/allods_test [path/to/config.ini]
#include <cstdio>

#include "mapedit/allods.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const std::string ini =
        "[MOD]\r\nname=x\r\n\r\n[MPZone]\r\nGipath=zone1\r\nGipath=zone2\r\n;Gipath=zone3obr\r\nGipath=zone3xobr\r\nGipath=\r\n"
        "Ingos=zone11\r\nIngos=Zone12\r\nIngos=\r\n; some prose = not a zone\r\nSuslanger=zone15\r\nJigran=zone0j\r\n[CONFIG]\r\nGipath=zone99\r\n";
    const allods::Table t = allods::ParseMpZone(ini);
    CHECK(t.allods.size() == 4);
    CHECK(t.allods[0].name == "Gipath" && t.allods[0].zones.size() == 3);
    CHECK(t.allods[1].name == "Ingos" && t.allods[3].name == "Jigran");
    CHECK(t.Find("zone12") == 1 && t.Find("ZONE12") == 1);
    CHECK(t.Find("zone3xobr") == 0);
    CHECK(t.Find("zone3obr") == -1);   // commented out
    CHECK(t.Find("zone99") == -1);     // another section
    CHECK(t.NameOf("zone0j") == "Jigran" && t.NameOf("nope").empty());
    CHECK(t.commented.size() == 1 && t.commented[0] == "zone3obr");
    CHECK(allods::ParseMpZone("[MPZone]\n").Empty());
    CHECK(allods::ParseMpZone("").Empty());
    if (argc > 1) { // a real file: print what was read
        std::vector<uint8_t> bytes;
        if (!allods::ReadFile(argv[1], bytes)) { std::printf("cannot read %s\n", argv[1]); return 1; }
        const allods::Table real = allods::ParseMpZone(std::string(bytes.begin(), bytes.end()));
        for (const allods::Allod& a : real.allods) std::printf("%s: %zu zones\n", a.name.c_str(), a.zones.size());
        CHECK(!real.Empty());
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
