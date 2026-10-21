// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The C runtime's math, re-implemented from game.exe (RE: _cpr/claude-re/full, docs/game-exe-analysis.md).
//
// 0x6FD026 _hypot(double x, double y), cdecl, the result in ST0. The game's statically linked MSVC 6 runtime computes
// it by switching the FPU into extended precision (FLDCW 0x133F, and back before returning), scaling by the larger
// argument (m * sqrt((x/m)^2 + (y/m)^2)), splitting both factors with frexp, and re-joining them with ldexp, on top of
// the error dispatch. 11 places call it: the camera, the footprint / blood decals marked near every unit
// (Records_MarkWithinRadius, one call per decal per unit per frame), line quads, the scene welding. In the base of the
// multiplayer lobby it was 22 % of the main thread's CPU time (fnstcw / fldcw stall the FPU pipeline each time).
//
// Here: for finite arguments whose larger one lies in [1e-150, 1e150] (every distance in the game) sqrt(x*x + y*y) in SSE2
// doubles: no control word change, no calls. It is not bit-identical to the original (that one rounds through extended
// precision and a double product: its last bit can differ), both are within 1 ulp of the exact value. Anything else
// (zero, infinity, NaN, huge or tiny) goes to the C library's hypot, which handles them as the standard says.
#include <emmintrin.h>

#include <cmath>
#include <cstring>

#ifndef _WIN32
#define __cdecl // the host test (tests/hypot_test.cpp) builds this file on Linux
#endif

extern "C" __attribute__((target("sse2"))) double __cdecl UmHypot(double x, double y) {
    double ax, ay;
    // |x|, |y| without a branch: clear the sign bit through the integer representation
    unsigned long long bx, by;
    std::memcpy(&bx, &x, 8);
    std::memcpy(&by, &y, 8);
    bx &= 0x7FFFFFFFFFFFFFFFULL;
    by &= 0x7FFFFFFFFFFFFFFFULL;
    std::memcpy(&ax, &bx, 8);
    std::memcpy(&ay, &by, 8);
    const double larger = ax > ay ? ax : ay;
    if (larger >= 1e-150 && larger <= 1e150) {                       // NaN fails both tests
        const __m128d sum = _mm_set_sd(ax * ax + ay * ay);
        double out;
        _mm_store_sd(&out, _mm_sqrt_sd(sum, sum));
        return out;
    }
    return std::hypot(x, y);
}
