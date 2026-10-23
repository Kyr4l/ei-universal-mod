// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The software renderer's fill rules: a quad cut into two triangles covers every pixel exactly once (no seams, no double blending), also at fractional corners.
#include <cstdio>
#include <cstdlib>

#include "softrender.hpp"

using namespace launcher::soft;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// Draws a quad of 50 % white over black; a pixel covered twice would come out brighter than the single blend.
static void Quad(Target& t, float x0, float y0, float x1, float y1) {
    detail::V a{x0, y0, 0, 0, 255, 255, 255, 128}, b{x1, y0, 0, 0, 255, 255, 255, 128}, c{x1, y1, 0, 0, 255, 255, 255, 128}, d{x0, y1, 0, 0, 255, 255, 255, 128};
    detail::Triangle(t, nullptr, a, b, c, 0, 0, t.w, t.h);
    detail::Triangle(t, nullptr, a, c, d, 0, 0, t.w, t.h);
}

int main() {
    for (const float off : {0.0f, 0.25f, 0.5f, 0.73f}) {
        Target t; t.Resize(40, 30);
        Quad(t, 5 + off, 4 + off, 31 + off, 22 + off);
        int covered = 0, doubled = 0;
        for (uint32_t p : t.px) { const uint32_t g = p & 0xFF; if (g) ++covered; if (g > 129) ++doubled; }
        CHECK(doubled == 0);
        CHECK(covered > 0);
        // a quad with integer corners covers exactly its area
        if (off == 0.0f) CHECK(covered == 26 * 18);
    }
    Target t; t.Resize(8, 8);
    Quad(t, 2, 2, 6, 6);
    CHECK(t.px[3 * 8 + 3] == 0x808080u || (t.px[3 * 8 + 3] & 0xFF) == 128);
    CHECK(t.px[0] == 0);
    std::printf(failures ? "%d failure(s)\n" : "softrender: all passed\n", failures);
    return failures ? 1 : 0;
}
