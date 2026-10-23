// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The mouse position mapping of BORDERLESS_SCALE (um_window.cpp): the game takes its cursor from GetCursorPos and warps it with SetCursorPos as if its window sat at 0,0 at its
// own size; the scaled window is `scale` times bigger at (winX, winY). Without a SetCursorPos the screen position maps absolutely. After one, the position is the warp target
// plus the pointer's travel since, divided by the scale, WITH the fraction carried over: a game that re-centres the pointer every frame (camera look) would otherwise lose every
// movement smaller than `scale` pixels. Plain header (no windows includes) so that it is tested on its own (tests/cursor_map_test.cpp).
#pragma once

#include <cmath>

struct CursorMap {
    double scale = 1.0;
    int winX = 0, winY = 0, gameW = 0, gameH = 0;

    // The game's SetCursorPos(gx, gy): returns the screen position to warp the real pointer to.
    void ToScreen(int gx, int gy, long& sx, long& sy) {
        sx = winX + std::lround(gx * scale); sy = winY + std::lround(gy * scale);
        if (haveSet_) { // the fraction left over from the travel since the previous warp
            carryX_ = lastTotalX_ - std::trunc(lastTotalX_); carryY_ = lastTotalY_ - std::trunc(lastTotalY_);
        }
        haveSet_ = true; setSX_ = sx; setSY_ = sy; setGX_ = gx; setGY_ = gy;
        lastTotalX_ = carryX_; lastTotalY_ = carryY_;
    }

    // The game's GetCursorPos: the real pointer (rawX, rawY) -> the position the game gets.
    void ToGame(long rawX, long rawY, long& gx, long& gy) {
        if (!haveSet_) {
            gx = static_cast<long>((rawX - winX) / scale); gy = static_cast<long>((rawY - winY) / scale);
        } else {
            lastTotalX_ = carryX_ + (rawX - setSX_) / scale; lastTotalY_ = carryY_ + (rawY - setSY_) / scale;
            gx = setGX_ + static_cast<long>(lastTotalX_); gy = setGY_ + static_cast<long>(lastTotalY_);
        }
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx > gameW - 1) gx = gameW - 1;
        if (gy > gameH - 1) gy = gameH - 1;
    }

private:
    bool haveSet_ = false;
    long setSX_ = 0, setSY_ = 0, setGX_ = 0, setGY_ = 0;
    double carryX_ = 0, carryY_ = 0, lastTotalX_ = 0, lastTotalY_ = 0;
};
