// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I.. tests/textenc_test.cpp -o /tmp/textenc_test && /tmp/textenc_test
#include <cstdio>

#include "textenc.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    using namespace textenc;
    // "СТЕПЕНЬ" in CP866 (the bytes of the vanilla ai.ini's comment)
    const std::vector<uint8_t> cp866 = {0x91, 0x92, 0x85, 0x8F, 0x85, 0x8D, 0x9C};
    CHECK(Decode(Enc::Cp866, cp866) == "\xD0\xA1\xD0\xA2\xD0\x95\xD0\x9F\xD0\x95\xD0\x9D\xD0\xAC");
    CHECK(Encode(Enc::Cp866, Decode(Enc::Cp866, cp866)) == cp866);
    // every byte round-trips in every single-byte page that defines it
    for (int e = 1; e < kCount; ++e) {
        std::vector<uint8_t> all;
        for (int b = 0x20; b < 0x100; ++b) all.push_back(static_cast<uint8_t>(b));
        const Enc en = static_cast<Enc>(e);
        if (en == Enc::Cp1250 || en == Enc::Cp1251) continue; // the codepages with undefined bytes (0x98, 0x81...) are covered by their own tests
        CHECK(Encode(en, Decode(en, all)) == all);
    }
    CHECK(Encode(Enc::Cp1251, Decode(Enc::Cp1251, {0xC0, 0xE0, 0xDF})) == (std::vector<uint8_t>{0xC0, 0xE0, 0xDF}));
    CHECK(Encode(Enc::Koi8r, "\xD0\xB0")[0] == 0xC1); // а
    CHECK(Encode(Enc::Cp866, "\xE2\x82\xAC")[0] == '?'); // euro: not in CP866
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
