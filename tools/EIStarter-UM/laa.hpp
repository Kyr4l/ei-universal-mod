// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// "Large address aware": sets bit 0x0020 of the PE file header's Characteristics in Engine\game.exe (the launcher's own copy of the game's
// exe, never the one of the installation) so the 32-bit game may use up to 4 GB of address space on a 64-bit system. An option, off by default:
// it is not needed in general. Seen once (Wine 11.19, Universal-Mod with D7VK, fullscreen 1920x1080): the stock exe exited after ~2 s with
// D7VK "InitTexture: map failed" (2 of 2 runs), the same exe patched ran (2 of 2); windowed 1024x768 ran with the stock exe, and so did the
// vanilla game (no um.dll) at the same fullscreen setup. The cause (address space) is an inference, not verified.
// The unpatched file is kept once as <name>.orig.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace launcher {

// 1 = patched now, 0 = already aware, -1 = not a PE file / cannot write.
inline int EnsureLargeAddressAware(const std::filesystem::path& exe) {
    namespace fs = std::filesystem;
    std::fstream f(exe, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return -1;
    uint8_t head[0x40];
    f.read(reinterpret_cast<char*>(head), sizeof head);
    uint32_t pe = 0;
    if (!f || head[0] != 'M' || head[1] != 'Z') return -1;
    std::memcpy(&pe, head + 0x3C, 4);
    uint8_t sig[4];
    f.seekg(pe); f.read(reinterpret_cast<char*>(sig), 4);
    if (!f || std::memcmp(sig, "PE\0\0", 4) != 0) return -1;
    const std::streamoff at = static_cast<std::streamoff>(pe) + 4 + 18;
    uint16_t chars = 0;
    f.seekg(at); f.read(reinterpret_cast<char*>(&chars), 2);
    if (!f) return -1;
    if (chars & 0x0020) return 0;
    f.close();
    std::error_code ec;
    const fs::path backup = fs::path(exe).concat(".orig");
    if (!fs::exists(backup, ec)) fs::copy_file(exe, backup, ec);
    std::fstream w(exe, std::ios::in | std::ios::out | std::ios::binary);
    if (!w) return -1;
    chars |= 0x0020;
    w.seekp(at); w.write(reinterpret_cast<const char*>(&chars), 2);
    return w.good() ? 1 : -1;
}

} // namespace launcher
