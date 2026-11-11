// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I. -I.. -I../../../resources/universal-mod/um-dll tests/engine_checks_test.cpp -o /tmp/engine_checks_test && /tmp/engine_checks_test [folder with .mob files ...]
// The game.exe script engine checks on small scripts; with folders: what they find in real maps (printed, for review).
#include <cstdio>
#include <filesystem>
#include <map>

#include "mapedit/engine_checks.hpp"
#include "mapedit/mob_file.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool Has(const std::vector<enginecheck::Issue>& v, const char* text) {
    for (const auto& i : v) if (i.message.find(text) != std::string::npos) return true;
    return false;
}

int main(int argc, char** argv) {
    const char* good = "GlobalVars (\n  NULL : object,\n  Grp : group\n)\nDeclareScript Door (  this : object )\nScript Door\n(\n  if\n  (\n  )\n  then\n  (\n    QStart( \"q\" )\n    QFinish(  )\n    CreateParticleSource( 1, 10, 10, 0, 1, \"Portal\" )\n  )\n)\nWorldScript\n(\n  Door( NULL )\n)\n";
    CHECK(enginecheck::Check(good).empty());
    const char* bad = "GlobalVars (\n  NULL : object,\n  SleepCount : object,\n  Grp : group\n)\nDeclareScript Door (  this : object )\nScript Door\n(\n  if\n  (\n    IsInSquare( 1, 1, 50, 50, 10, 10 )\n  )\n  then\n  (\n    QStart( \"q\" )\n    ShowBitmap( 1, \"x\" )\n    CreateParticleSource( 1, 10, 10, 0, 1, \"Rainbow\" )\n    For( SleepCount, Grp )\n    (\n      Attack( SleepCount, this )\n    )\n  )\n)\nWorldScript\n(\n  Door( NULL )\n)\nScript";
    const auto v = enginecheck::Check(bad);
    CHECK(Has(v, "begins with 'Sleep'"));
    CHECK(Has(v, "IsInSquare(): the corners"));
    CHECK(Has(v, "QStart() without QFinish()"));
    CHECK(Has(v, "ShowBitmap() does nothing"));
    CHECK(Has(v, "'Rainbow' is not one of"));
    CHECK(Has(v, "parameter of this script"));
    CHECK(Has(v, "compiler crashes"));
    // real maps: a count per kind of finding
    std::map<std::string, int> kinds;
    int files = 0;
    for (int a = 1; a < argc; ++a) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(argv[a], ec)) {
            if (e.path().extension() != ".mob") continue;
            mob::File f;
            if (!mob::Load(e.path().string(), f) || !f.hasScript) continue;
            ++files;
            for (const auto& i : enginecheck::Check(f.script)) {
                const std::string k = i.message.substr(0, i.message.find(':'));
                if (++kinds[k] <= 3) std::printf("%s:%d %s\n", e.path().filename().string().c_str(), i.line, i.message.c_str());
            }
        }
    }
    if (argc > 1) { std::printf("%d files\n", files); for (const auto& kv : kinds) std::printf("%5d  %s\n", kv.second, kv.first.c_str()); }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
