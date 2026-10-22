// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Writers for Evil Islands .fig (FIG8) / .bon / .mod link table: the inverse of figure_format.hpp's parsers.
// Every rule below was checked against all 3,563 non-empty FIG8 meshes of the vanilla figures.res
// (docs/file-formats/figure-format.md "Writing"): a parsed mesh written back is byte-identical, and the derived
// fields (bounds, radius, trailer, normal w) are what the vanilla files hold.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "figure_format.hpp"

namespace figw {

using fig::Vec2;
using fig::Vec3;

inline void PutU32(std::vector<uint8_t>& o, uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); o.insert(o.end(), b, b + 4); }
inline void PutI32(std::vector<uint8_t>& o, int32_t v) { PutU32(o, static_cast<uint32_t>(v)); }
inline void PutU16(std::vector<uint8_t>& o, uint16_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); o.insert(o.end(), b, b + 2); }
inline void PutF32(std::vector<uint8_t>& o, float v) { uint32_t u; std::memcpy(&u, &v, 4); PutU32(o, u); }

// The mesh's vertex points actually used by a vertex component (the vertex array is padded to blocks of 4 with copies
// of existing points, which no component refers to).
inline size_t UsedPoints(const fig::FigureMesh& m) {
    size_t n = 0;
    for (const auto& vc : m.vertexComponents) n = std::max<size_t>(n, static_cast<size_t>(vc.vertexIndex) + 1);
    return n;
}

// Bounds per corner as the vanilla files hold them: centre = the bounding box's centre, min / max = minus / plus its
// half extents, radius = the half diagonal.
inline void RecomputeBounds(fig::FigureMesh& m) {
    m.morphCenter.assign(8, {}); m.morphMin.assign(8, {}); m.morphMax.assign(8, {}); m.morphRadius.assign(8, 0.0f);
    for (int c = 0; c < 8; ++c) {
        const auto& v = m.morphVertex[c];
        if (v.empty()) continue;
        Vec3 lo = v[0], hi = v[0];
        for (const Vec3& p : v) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const Vec3 half{(hi.x - lo.x) / 2, (hi.y - lo.y) / 2, (hi.z - lo.z) / 2};
        m.morphCenter[c] = {(hi.x + lo.x) / 2, (hi.y + lo.y) / 2, (hi.z + lo.z) / 2};
        m.morphMax[c] = half;
        m.morphMin[c] = {-half.x, -half.y, -half.z};
        m.morphRadius[c] = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    }
}

// FIG8 bytes of a mesh (the stored bounds are written as they are: call RecomputeBounds after changing vertices).
// morphVertex[c] must hold a multiple of 4 points, normals a multiple of 4 entries.
inline bool WriteFig(const fig::FigureMesh& m, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    const size_t points = m.morphVertex[0].size();
    for (int c = 1; c < 8; ++c) if (m.morphVertex[c].size() != points) { err = "the 8 corners hold different vertex counts"; return false; }
    if (points % 4 || m.normals.size() % 4) { err = "vertex / normal arrays are not padded to blocks of 4"; return false; }
    if (m.morphCenter.size() != 8 || m.morphMin.size() != 8 || m.morphMax.size() != 8 || m.morphRadius.size() != 8) { err = "bounds missing"; return false; }
    const size_t used = UsedPoints(m);
    if (used > points) { err = "a vertex component points past the vertex array"; return false; }
    for (const auto& vc : m.vertexComponents) {
        if (vc.normalIndex >= m.normals.size() || vc.uvIndex >= m.uvs.size()) { err = "a vertex component points past the normals / UVs"; return false; }
    }
    for (uint16_t i : m.indices) if (i >= m.vertexComponents.size()) { err = "an index points past the vertex components"; return false; }
    if (m.indices.size() % 3) { err = "the index count is not a multiple of 3"; return false; }
    if (m.vertexComponents.size() > 0xFFFF || m.uvs.size() > 0xFFFF || points > 0xFFFF) { err = "a count does not fit 16 bits"; return false; }

    PutU32(out, 0x38474946u);
    PutI32(out, static_cast<int32_t>(points / 4));
    PutI32(out, static_cast<int32_t>(m.normals.size() / 4));
    PutI32(out, static_cast<int32_t>(m.uvs.size()));
    PutI32(out, static_cast<int32_t>(m.indices.size()));
    PutI32(out, static_cast<int32_t>(m.vertexComponents.size()));
    PutI32(out, static_cast<int32_t>(points)); // morphingComponentCount = 4 * vertex blocks
    PutI32(out, 0);
    PutI32(out, m.group);
    PutI32(out, m.textureNumber);
    for (const auto* v : {&m.morphCenter, &m.morphMin, &m.morphMax}) for (const Vec3& p : *v) { PutF32(out, p.x); PutF32(out, p.y); PutF32(out, p.z); }
    for (float r : m.morphRadius) PutF32(out, r);
    for (size_t block = 0; block < points / 4; ++block)
        for (int axis = 0; axis < 3; ++axis)
            for (int corner = 0; corner < 8; ++corner)
                for (size_t pt = 0; pt < 4; ++pt) {
                    const Vec3& p = m.morphVertex[corner][block * 4 + pt];
                    PutF32(out, axis == 0 ? p.x : axis == 1 ? p.y : p.z);
                }
    for (size_t block = 0; block < m.normals.size() / 4; ++block)
        for (int comp = 0; comp < 4; ++comp)
            for (size_t pt = 0; pt < 4; ++pt) {
                const Vec3& n = m.normals[block * 4 + pt];
                PutF32(out, comp == 0 ? n.x : comp == 1 ? n.y : comp == 2 ? n.z : 1.0f);
            }
    for (const Vec2& uv : m.uvs) { PutF32(out, uv.x); PutF32(out, uv.y); }
    for (uint16_t i : m.indices) PutU16(out, i);
    for (const auto& vc : m.vertexComponents) { PutU16(out, vc.normalIndex); PutU16(out, vc.vertexIndex); PutU16(out, vc.uvIndex); }
    // The trailer: per vertex point p, p in the high half, min(p, last used point) in the low half.
    for (size_t p = 0; p < points; ++p) PutU32(out, static_cast<uint32_t>((p << 16) | std::min(p, used ? used - 1 : 0)));
    return true;
}

inline std::vector<uint8_t> WriteBon(const Vec3 offset[8]) {
    std::vector<uint8_t> o;
    for (int c = 0; c < 8; ++c) { PutF32(o, offset[c].x); PutF32(o, offset[c].y); PutF32(o, offset[c].z); }
    return o;
}

// A .mod's link table: count, then {child length (with the NUL) + child, parent length + parent (0 = root)}.
inline std::vector<uint8_t> WriteLinkTable(const std::vector<fig::LinkEntry>& links) {
    std::vector<uint8_t> o;
    PutI32(o, static_cast<int32_t>(links.size()));
    for (const auto& l : links) {
        PutI32(o, static_cast<int32_t>(l.child.size() + 1));
        o.insert(o.end(), l.child.begin(), l.child.end()); o.push_back(0);
        if (l.parent.empty()) PutI32(o, 0);
        else { PutI32(o, static_cast<int32_t>(l.parent.size() + 1)); o.insert(o.end(), l.parent.begin(), l.parent.end()); o.push_back(0); }
    }
    return o;
}

} // namespace figw
