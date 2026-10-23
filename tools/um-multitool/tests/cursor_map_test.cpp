// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O1 -I../../resources/universal-mod/um-dll tests/cursor_map_test.cpp -o /tmp/cursor_map_test && /tmp/cursor_map_test
// BORDERLESS_SCALE's cursor mapping (um-dll/um_cursor_map.hpp).
#include <cstdio>
#include <cstdlib>

#include "um_cursor_map.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // 1024x768 on a 1920x1080 monitor: scale 1.40625, window 1440x1080 at 240,0
    CursorMap m; m.scale = 1080.0 / 768.0; m.winX = 240; m.winY = 0; m.gameW = 1024; m.gameH = 768;
    long gx, gy, sx, sy;
    m.ToGame(240, 0, gx, gy); CHECK(gx == 0 && gy == 0);                         // the window's corner
    m.ToGame(240 + 1439, 1079, gx, gy); CHECK(gx == 1023 && gy == 767);           // the far corner stays inside the game
    m.ToGame(0, 500, gx, gy); CHECK(gx == 0);                                     // on a black bar: clamped
    m.ToGame(990, 595, gx, gy); CHECK(gx == 533 && gy == 423);                    // the "OPTIONS" button of the main menu
    // absolute round trip: a warp to (512, 384) puts the pointer on the screen position that maps back to it
    m.ToScreen(512, 384, sx, sy); m.ToGame(sx, sy, gx, gy); CHECK(gx == 512 && gy == 384);
    // the camera look: re-centre every frame, the pointer then moves 1 px per frame to the right; 1000 frames = 1000 / scale game pixels, none lost
    CursorMap c = m; long total = 0;
    c.ToScreen(512, 384, sx, sy);
    for (int i = 0; i < 1000; ++i) {
        c.ToGame(sx + 1, sy, gx, gy);        // the game reads the pointer: it moved by one screen pixel
        total += gx - 512;
        c.ToScreen(512, 384, sx, sy);        // and warps it back to the centre
    }
    const double expect = 1000 / m.scale;     // 711.1
    CHECK(std::labs(total - static_cast<long>(expect)) <= 1);
    // negative direction, two pixels a frame
    long totalNeg = 0;
    for (int i = 0; i < 500; ++i) { c.ToGame(sx - 2, sy, gx, gy); totalNeg += gx - 512; c.ToScreen(512, 384, sx, sy); }
    CHECK(std::labs(totalNeg + static_cast<long>(1000 / m.scale)) <= 1);
    // several reads between two warps do not count the movement twice
    CursorMap d = m; d.ToScreen(512, 384, sx, sy);
    long a, b, e; d.ToGame(sx + 10, sy, a, gy); d.ToGame(sx + 10, sy, b, gy); d.ToGame(sx + 10, sy, e, gy);
    CHECK(a == b && b == e);
    // scale 1 changes nothing
    CursorMap one; one.scale = 1.0; one.winX = 0; one.winY = 0; one.gameW = 1024; one.gameH = 768;
    one.ToGame(333, 222, gx, gy); CHECK(gx == 333 && gy == 222);
    one.ToScreen(100, 50, sx, sy); CHECK(sx == 100 && sy == 50);
    std::printf(g_fail ? "%d failure(s)\n" : "cursor map: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
