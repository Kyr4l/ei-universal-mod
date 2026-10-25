// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The frame recorder and the rig commands of um-engine (um_rig.cpp): what world_shadow.cpp reports about each world frame.
#pragma once
#include <cstdint>

// One world frame (CWorldClient::Render) as the sun shadows saw it. world_shadow.cpp fills g_umFrame during the frame; UmRigFrameEnd stores it in the ring.
struct UmFrameRec {
    uint32_t tick = 0;        // GetTickCount at the start of the frame
    uint32_t frame = 0;       // world frames since the game started
    float dtMs = 0;           // time since the previous frame started
    float renderMs = 0;       // the world pass itself
    float hour = 0;           // game hour of the world clock
    uint8_t mode = 0;         // see UmFrameMode
    uint8_t guard = 0;        // 0 band off, 1 band drawn, 2 matrix stacks not as expected (no band)
    uint8_t reserved[2] = {};
    int16_t pad[4] = {};      // guard band: left, top, right, bottom (pixels)
    float cam[3] = {};        // camera position
    float target[2] = {};     // camera target
    float center[3] = {};     // centre of the shadowed area
    float radius = 0, height = 0;
    uint32_t mapSize = 0;
    float light[3] = {};      // direction the light travels
    uint32_t dvk[3] = {};     // D7VK: shadow frames closed so far, caster draws of the last one, its result (0 drawn)
};

enum UmFrameMode : uint8_t {
    kModeNoPass = 0,    // the shadow pass did not run (no world pass between the call sites)
    kModeDrawn = 1,     // shadows drawn with this frame's parameters
    kModeKept = 2,      // this frame's matrices could not be read: the last good parameters were used
    kModeNone = 3,      // no parameters at all: no shadows this frame
    kModeOff = 4,       // not wanted (Enabled=0, the game's shadow settings, a cave)
};

extern UmFrameRec g_umFrame;

extern "C" __attribute__((used, force_align_arg_pointer)) void UmRigFrameBegin();   // the start of CWorldClient::Render (game thread)
extern "C" __attribute__((used, force_align_arg_pointer)) void UmRigFrameEnd();     // after it: the world is drawn, the interface is not
