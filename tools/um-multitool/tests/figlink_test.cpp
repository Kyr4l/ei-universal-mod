// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O1 -fsanitize=address,undefined -Iviewer tests/figlink_test.cpp -o /tmp/figlink_test && /tmp/figlink_test
// ParseLinkTable: a damaged entry count or length (negative, far larger than the data) is refused, it must not reserve gigabytes or read past the end.
#include <cstdio>
#include <cstring>
#include <vector>

#include "figure_format.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void Put32(std::vector<uint8_t>& b, int32_t v) { uint8_t t[4]; std::memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); }

int main() {
    std::vector<fig::LinkEntry> out;
    { std::vector<uint8_t> b; Put32(b, 1); Put32(b, 3); b.insert(b.end(), {'a', 'b', 0}); Put32(b, 0); // one link "ab" without a parent
      CHECK(fig::ParseLinkTable(b.data(), b.size(), out) && out.size() == 1 && out[0].child == "ab" && out[0].parent.empty()); }
    { std::vector<uint8_t> b; Put32(b, -1); CHECK(!fig::ParseLinkTable(b.data(), b.size(), out)); }
    { std::vector<uint8_t> b; Put32(b, 0x7FFFFFFF); Put32(b, 0); Put32(b, 0); CHECK(!fig::ParseLinkTable(b.data(), b.size(), out)); }
    { std::vector<uint8_t> b; Put32(b, 1); Put32(b, -1); Put32(b, 0); CHECK(!fig::ParseLinkTable(b.data(), b.size(), out)); }                       // negative child length
    { std::vector<uint8_t> b; Put32(b, 1); Put32(b, 0); Put32(b, -5); CHECK(!fig::ParseLinkTable(b.data(), b.size(), out)); }                       // negative parent length
    { std::vector<uint8_t> b; Put32(b, 1); Put32(b, 0x7FFFFFFF); Put32(b, 0); CHECK(!fig::ParseLinkTable(b.data(), b.size(), out)); }                // child longer than the data
    std::printf(g_fail ? "%d failure(s)\n" : "figlink: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
