// SPDX-License-Identifier: GPL-3.0-or-later
// The zone exits the server sends in its first big message (the zone state: CWorldServer::SendZoneState 0x58C890, read by CWorldClient::LoadZone 0x567AB0):
//   u32 tick - 1, the zone name and the map name (zero-terminated, no length), u32 exit count, then per exit (CEffectKey::Write 0x4E58A0, really the exit record):
//   9 x u32 (+0 .. +0x20; +0x14 / +0x18 / +0x1C / +0x20 are the rectangle: x min, y min, x max, y max, in world units), the name (zero-terminated; "none" = unused),
//   u32 entrance, u32, u8, u32, u32 (the last one a float: the rectangle's rotation, 0 = none), then more state.
// A hero standing inside an exit (CRect2F::Contains 0x4E59F0) can leave the zone through it.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace zone {

struct Exit { std::string name; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; uint32_t entrance = 0; float angle = 0; };

// The game's own test: inside the rectangle (rotated about its centre by `angle` when that is not 0).
inline bool Contains(const Exit& e, float x, float y) {
    if (e.angle != 0.0f) {
        const float cx = (e.x1 + e.x0) * 0.5f, cy = (e.y1 + e.y0) * 0.5f, c = std::cos(e.angle), s = std::sin(e.angle);
        const float dx = x - cx, dy = y - cy;
        const float rx = dy * s + dx * c + cx, ry = dy * c + (cy - dx * s);
        x = rx; y = ry;
    }
    return !(x < e.x0 || e.x1 < x || y < e.y0 || e.y1 < y);
}

namespace detail {
inline bool Name(const uint8_t* m, size_t n, size_t& p, std::string& out, size_t maxLen) { // a zero-terminated printable name
    const size_t start = p;
    while (p < n && m[p] != 0) { if (m[p] < 0x20 || m[p] > 0x7E) return false; ++p; }
    if (p >= n || p - start > maxLen) return false;
    out.assign(reinterpret_cast<const char*>(m) + start, p - start);
    ++p;
    return true;
}
inline bool U32(const uint8_t* m, size_t n, size_t& p, uint32_t& v) { if (p + 4 > n) return false; std::memcpy(&v, m + p, 4); p += 4; return true; }
inline bool F32(const uint8_t* m, size_t n, size_t& p, float& v) { if (p + 4 > n) return false; std::memcpy(&v, m + p, 4); p += 4; return std::isfinite(v); }
}

// Looks for the zone state in a decompressed stream message; true when a plausible one was found (the first match is taken).
inline bool ParseExits(const std::vector<uint8_t>& m, std::vector<Exit>& out, std::string* zoneName = nullptr, std::string* mapName = nullptr) {
    using namespace detail;
    const size_t n = m.size();
    for (size_t start = 0; start + 16 <= n; ++start) {
        size_t p = start + 4; // the tick
        std::string zoneN, mapN;
        if (!Name(m.data(), n, p, zoneN, 64) || zoneN.empty() || !Name(m.data(), n, p, mapN, 64)) continue;
        uint32_t count;
        if (!U32(m.data(), n, p, count) || count > 64) continue;
        std::vector<Exit> list;
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; ++i) {
            uint32_t w[9];
            for (int k = 0; k < 9 && ok; ++k) ok = U32(m.data(), n, p, w[k]);
            if (!ok) break;
            Exit e;
            std::memcpy(&e.x0, &w[5], 4); std::memcpy(&e.y0, &w[6], 4); std::memcpy(&e.x1, &w[7], 4); std::memcpy(&e.y1, &w[8], 4);
            if (!std::isfinite(e.x0) || !std::isfinite(e.y0) || !std::isfinite(e.x1) || !std::isfinite(e.y1) ||
                std::fabs(e.x0) > 20000 || std::fabs(e.y0) > 20000 || std::fabs(e.x1) > 20000 || std::fabs(e.y1) > 20000 || e.x1 < e.x0 || e.y1 < e.y0) { ok = false; break; }
            uint32_t a, b, last, entrance; float angle;
            if (!Name(m.data(), n, p, e.name, 48) || !U32(m.data(), n, p, entrance) || !U32(m.data(), n, p, a) || p + 1 > n) { ok = false; break; }
            ++p; // the byte
            if (!U32(m.data(), n, p, b) || !F32(m.data(), n, p, angle)) { ok = false; break; }
            (void)last;
            e.entrance = entrance; e.angle = angle;
            list.push_back(e);
        }
        if (!ok || list.empty()) continue;
        out = list;
        if (zoneName) *zoneName = zoneN;
        if (mapName) *mapName = mapN;
        return true;
    }
    return false;
}

} // namespace zone
