// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// A smooth camera (RE: docs/game-exe, game.exe main loop 0x456440, CCamera::Update 0x40FB30).
// The game moves the camera from TWO places: once per fixed client tick (55 ms, ~18 Hz) with a whole tick's worth of time (dt = 1.0), and
// once per rendered frame with the frame's time, measured by timeGetTime() in whole milliseconds. The tick call adds a lump of motion every
// 55 ms (7 frames' worth at 140 FPS: a stutter), and the frame call's time is rounded to the millisecond (7 ms, 7 ms, 8 ms ...).
// Here the three call sites in the main loop are redirected:
//   - the tick call does not move the camera; its time is kept as "pending";
//   - a frame call moves it with the frame's time measured by the high resolution counter, plus as much of the pending tick time as the frame's
//     own time (so the tick's motion is spread over the following ~55 ms): the total motion is the same as the game's, the speed does not change,
//     and nothing moves in a lump. When the game does not tick (paused) nothing is pending and the camera moves as the game's own.
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace {
typedef void (__attribute__((thiscall)) *CameraUpdateFn)(void* camera, float dt);
const CameraUpdateFn kOriginalUpdate = reinterpret_cast<CameraUpdateFn>(0x0040FB30);
const unsigned long kReturnAfterTickCall = 0x004573AE;  // the fixed-tick call
const unsigned long kReturnAfterLoopCall = 0x00457572;  // a whole tick of a frame that lasted more than one tick (slow frames)
const unsigned long kReturnAfterFrameCall = 0x0045759A; // the frame's remainder: every frame

float g_pending = 0;                  // tick time (game units: 1.0 = one tick) not yet applied
LARGE_INTEGER g_lastFrame = {};       // the counter at the previous frame call
bool g_haveLast = false;
double g_frequency = 0;
}

// The fixed-tick call site: keeps the tick's time for the next frames.
extern "C" void __attribute__((thiscall)) UmCameraTick(void* /*camera*/, float dt) {
    g_pending = std::min(g_pending + dt, 8.0f);
}

// The two frame call sites (the loop of whole ticks, then the remainder of the frame).
extern "C" void __attribute__((thiscall)) UmCameraFrame(void* camera, float dt) {
    const unsigned long caller = reinterpret_cast<unsigned long>(__builtin_return_address(0));
    float useDt = dt;
    if (caller == kReturnAfterFrameCall) { // once per frame: the precise time
        if (g_frequency == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_frequency = static_cast<double>(f.QuadPart); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (g_haveLast) {
            const double ms = static_cast<double>(now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / g_frequency;
            const double gameMs = static_cast<double>(dt) * 55.0; // the game's whole-millisecond version (a tick is 55 ms = dt 1.0; 27 ms in fast mode: the ratio is the same)
            if (std::fabs(ms - gameMs) <= 2.5 && ms > 0) useDt = static_cast<float>(dt * (ms / (gameMs > 0 ? gameMs : ms)));
        }
        g_lastFrame = now; g_haveLast = true;
    }
    // spread the pending tick time: at most the frame's own time per frame
    const float release = std::min(g_pending, useDt);
    g_pending -= release;
    kOriginalUpdate(camera, useDt + release);
}
