// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's CRT string functions (MSVC 6, statically linked into game.exe).
#include <cstddef>

// strlen at 0x6ECC70 (cdecl): the length of a zero-terminated string.
extern "C" size_t __cdecl UmStrlen(const char* text) {
    const char* p = text;
    while (*p) ++p;
    return static_cast<size_t>(p - text);
}
