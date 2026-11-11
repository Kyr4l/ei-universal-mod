// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I. tests/briefing_test.cpp -o /tmp/briefing_test && /tmp/briefing_test [briefing file ...]
// The briefing reader (mapedit/briefing.hpp) on a small text; with files: their phrases and findings, printed.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "mapedit/briefing.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const std::set<std::string> units{"huber", "guard"};
    const briefing::Parsed ok = briefing::Parse("[Hard] Mines\r\n#show Hero\r\n#show Huber\r\n#CAMERA 2\r\n#phrase Hero  1\r\nHello.\r\n#phrase Huber  2\r\nHi.\r\nTwo lines.\r\n", units);
    CHECK(ok.title == "[Hard] Mines" && ok.phrases.size() == 2 && ok.phrases[1].text == "Hi. Two lines." && ok.phrases[0].voice == 1);
    CHECK(ok.phrases[0].camera == "shot 2" && ok.phrases[1].camera == "shot 3 (the speaker)");
    CHECK(ok.issues.empty());
    const briefing::Parsed bad = briefing::Parse("Title\n#show Hero\n#show Nobody\n#camera 14\n#phrase Stranger\n\n", units);
    int errors = 0, warnings = 0;
    for (const auto& i : bad.issues) { errors += i.severity == 'E'; warnings += i.severity == 'W'; }
    CHECK(errors == 1);    // nobody to talk to: the game does not open it
    CHECK(warnings >= 4);  // Nobody unknown without index, camera 14, Stranger not shown, no text
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const briefing::Parsed p = briefing::Parse(text, {});
        std::printf("%s: %zu phrases, title '%s'\n", argv[a], p.phrases.size(), p.title.c_str());
        for (const auto& i : p.issues) std::printf("  %c line %d: %s\n", i.severity, i.line, i.message.c_str());
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
