// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Parser for Evil Islands .fig / .bon / .mod / .anm / .lnk model files.
// See docs/file-formats/figure-format.md for the full byte-level specification
// this implements (recovered from ei_maper's CFigure::readData for .fig, and
// from-scratch byte analysis, verified against real game data, for .anm).
#pragma once

#include <cmath>
#include <algorithm>
#include <cctype>
#include <functional>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "res_archive.hpp"

namespace fig {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float w = 1, x = 0, y = 0, z = 0; };

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
// Quaternions as the .anm uses them (figure-format.md): a part's rotation chains child first, W = q * W(parent).
inline Quat QMul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Vec3 QRotate(const Quat& q, const Vec3& v) { // q v q*
    const Quat p = QMul(QMul(q, Quat{0, v.x, v.y, v.z}), Quat{q.w, -q.x, -q.y, -q.z});
    return {p.x, p.y, p.z};
}
inline Quat QSlerp(Quat a, const Quat& b, float t) { // the short way; nlerp when nearly equal
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) { a = {-a.w, -a.x, -a.y, -a.z}; d = -d; }
    float ka = 1 - t, kb = t;
    if (d < 0.9995f) {
        const float th = std::acos(d), s = std::sin(th);
        ka = std::sin((1 - t) * th) / s;
        kb = std::sin(t * th) / s;
    }
    Quat r{ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z};
    const float n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    return n > 0 ? Quat{r.w / n, r.x / n, r.y / n, r.z / n} : Quat{};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// Hamilton product: composes rotations so that QuatMul(parent, local) applies
// "local" first, then "parent" - i.e. the standard child-to-world order used
// when walking a bone hierarchy root-down.
inline Quat QuatMul(const Quat& a, const Quat& b) {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

inline Vec3 QuatRotate(const Quat& q, const Vec3& v) {
    // v' = q * (0,v) * conj(q), expanded without building the intermediate quaternions.
    Vec3 qv{q.x, q.y, q.z};
    Vec3 t = (Vec3{qv.y * v.z - qv.z * v.y, qv.z * v.x - qv.x * v.z, qv.x * v.y - qv.y * v.x}) * 2.0f;
    Vec3 cross{qv.y * t.z - qv.z * t.y, qv.z * t.x - qv.x * t.z, qv.x * t.y - qv.y * t.x};
    return v + t * q.w + cross;
}

inline Quat QuatNormalize(const Quat& q) {
    float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (len < 1e-8f) return {1, 0, 0, 0};
    return {q.w / len, q.x / len, q.y / len, q.z / len};
}

// Shortest-path spherical linear interpolation (flips sign if that shortens
// the path, standard for animation keyframes stored as raw quaternions).
inline Quat QuatSlerp(Quat a, Quat b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) { b = {-b.w, -b.x, -b.y, -b.z}; dot = -dot; }
    if (dot > 0.9995f) {
        Quat r{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        return QuatNormalize(r);
    }
    float theta0 = std::acos(dot);
    float theta = theta0 * t;
    float sinTheta0 = std::sin(theta0);
    float s0 = std::cos(theta) - dot * std::sin(theta) / sinTheta0;
    float s1 = std::sin(theta) / sinTheta0;
    return {a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1};
}

// Trilinear blend across the 8 complection corners.
// Corner order: 0=str0,dex0,tall0  1=str1,dex0,tall0  2=str0,dex1,tall0  3=str1,dex1,tall0
//               4=str0,dex0,tall1  5=str1,dex0,tall1  6=str0,dex1,tall1  7=str1,dex1,tall1
// (matches ei_maper's calcComplection / CFigure::calculateConstitution)
inline Vec3 BlendComplection(const Vec3 corners[8], const Vec3& constitution) {
    Vec3 res0 = Lerp(corners[0], corners[1], constitution.y);
    Vec3 res1 = Lerp(corners[2], corners[3], constitution.y);
    Vec3 low = Lerp(res0, res1, constitution.x);
    res0 = Lerp(corners[4], corners[5], constitution.y);
    res1 = Lerp(corners[6], corners[7], constitution.y);
    Vec3 high = Lerp(res0, res1, constitution.x);
    return Lerp(low, high, constitution.z);
}

struct VertComponent { uint16_t normalIndex = 0, vertexIndex = 0, uvIndex = 0; };

// Raw parsed .fig mesh: everything still in "8 morph corners" form.
struct FigureMesh {
    bool valid = false;
    int32_t group = 0;
    int32_t textureNumber = 0;

    // vertex[corner][vertexIndex]
    std::vector<Vec3> morphVertex[8];
    std::vector<Vec3> morphCenter, morphMin, morphMax; // size 8 each
    std::vector<float> morphRadius;                     // size 8

    std::vector<Vec3> normals;   // one per vertex (not morph-varying); w component dropped
    std::vector<Vec2> uvs;
    std::vector<uint16_t> indices;             // flat triangle list into vertexComponents
    std::vector<VertComponent> vertexComponents;

    // Resolves final blended position/normal/uv for one vertex-component entry.
    Vec3 BlendedPosition(size_t vcIndex, const Vec3& constitution) const {
        uint16_t vIdx = vertexComponents[vcIndex].vertexIndex;
        Vec3 corners[8];
        for (int c = 0; c < 8; ++c) corners[c] = morphVertex[c][vIdx];
        return BlendComplection(corners, constitution);
    }
};

inline bool ReadFloat(const uint8_t* d, size_t size, size_t& off, float& out) {
    if (off + 4 > size) return false;
    std::memcpy(&out, d + off, 4);
    off += 4;
    return true;
}
inline bool ReadU32(const uint8_t* d, size_t size, size_t& off, uint32_t& out) {
    if (off + 4 > size) return false;
    std::memcpy(&out, d + off, 4);
    off += 4;
    return true;
}
inline bool ReadI32(const uint8_t* d, size_t size, size_t& off, int32_t& out) {
    if (off + 4 > size) return false;
    std::memcpy(&out, d + off, 4);
    off += 4;
    return true;
}
inline bool ReadU16(const uint8_t* d, size_t size, size_t& off, uint16_t& out) {
    if (off + 2 > size) return false;
    std::memcpy(&out, d + off, 2);
    off += 2;
    return true;
}

inline bool ParseFig(const uint8_t* data, size_t size, FigureMesh& out) {
    size_t off = 0;
    uint32_t sig;
    if (!ReadU32(data, size, off, sig) || sig != 0x38474946u /* 'FIG8' */) return false;

    int32_t vertBlocks, normalBlocks, uvCount, indexCount, vertexComponentCount, morphingComponentCount, unknown;
    if (!ReadI32(data, size, off, vertBlocks)) return false;
    if (!ReadI32(data, size, off, normalBlocks)) return false;
    if (!ReadI32(data, size, off, uvCount)) return false;
    if (!ReadI32(data, size, off, indexCount)) return false;
    if (!ReadI32(data, size, off, vertexComponentCount)) return false;
    if (!ReadI32(data, size, off, morphingComponentCount)) return false;
    if (!ReadI32(data, size, off, unknown)) return false;
    if (!ReadI32(data, size, off, out.group)) return false;
    if (!ReadI32(data, size, off, out.textureNumber)) return false;

    auto readVec3x8 = [&](std::vector<Vec3>& v) -> bool {
        v.resize(8);
        for (int i = 0; i < 8; ++i) {
            if (!ReadFloat(data, size, off, v[i].x)) return false;
            if (!ReadFloat(data, size, off, v[i].y)) return false;
            if (!ReadFloat(data, size, off, v[i].z)) return false;
        }
        return true;
    };
    if (!readVec3x8(out.morphCenter)) return false;
    if (!readVec3x8(out.morphMin)) return false;
    if (!readVec3x8(out.morphMax)) return false;
    out.morphRadius.resize(8);
    for (int i = 0; i < 8; ++i) {
        if (!ReadFloat(data, size, off, out.morphRadius[i])) return false;
    }

    // The counts come from the file: refuse what the bytes left cannot hold before allocating for them (a damaged
    // header must not ask for gigabytes). Vertex block 384 B, normal block 64 B, uv 8 B, index 2 B, component 6 B.
    if (vertBlocks < 0 || normalBlocks < 0 || uvCount < 0 || indexCount < 0 || vertexComponentCount < 0 || morphingComponentCount < 0) return false;
    const size_t left = size - off;
    if (static_cast<size_t>(vertBlocks) * 384 + static_cast<size_t>(normalBlocks) * 64 + static_cast<size_t>(uvCount) * 8 +
        static_cast<size_t>(indexCount) * 2 + static_cast<size_t>(vertexComponentCount) * 6 > left) return false;
    // Vertices: for block, for axis{x,y,z}, for morph[8], for point[4]: float
    size_t vertCount = static_cast<size_t>(vertBlocks) * 4;
    for (int c = 0; c < 8; ++c) out.morphVertex[c].assign(vertCount, Vec3{});
    for (int32_t block = 0; block < vertBlocks; ++block) {
        for (int axis = 0; axis < 3; ++axis) {
            for (int morph = 0; morph < 8; ++morph) {
                for (int point = 0; point < 4; ++point) {
                    float v;
                    if (!ReadFloat(data, size, off, v)) return false;
                    Vec3& target = out.morphVertex[morph][static_cast<size_t>(block) * 4 + point];
                    if (axis == 0) target.x = v;
                    else if (axis == 1) target.y = v;
                    else target.z = v;
                }
            }
        }
    }

    // Normals: for block, for component{x,y,z,w}, for point[4]: float (w dropped)
    size_t normCount = static_cast<size_t>(normalBlocks) * 4;
    out.normals.assign(normCount, Vec3{});
    for (int32_t block = 0; block < normalBlocks; ++block) {
        for (int comp = 0; comp < 4; ++comp) {
            for (int point = 0; point < 4; ++point) {
                float v;
                if (!ReadFloat(data, size, off, v)) return false;
                if (comp == 3) continue; // w unused
                Vec3& target = out.normals[static_cast<size_t>(block) * 4 + point];
                if (comp == 0) target.x = v;
                else if (comp == 1) target.y = v;
                else target.z = v;
            }
        }
    }

    out.uvs.resize(static_cast<size_t>(uvCount));
    for (auto& uv : out.uvs) {
        if (!ReadFloat(data, size, off, uv.x)) return false;
        if (!ReadFloat(data, size, off, uv.y)) return false;
    }

    out.indices.resize(static_cast<size_t>(indexCount));
    for (auto& idx : out.indices) {
        if (!ReadU16(data, size, off, idx)) return false;
    }

    out.vertexComponents.resize(static_cast<size_t>(vertexComponentCount));
    for (auto& vc : out.vertexComponents) {
        if (!ReadU16(data, size, off, vc.normalIndex)) return false;
        if (!ReadU16(data, size, off, vc.vertexIndex)) return false;
        if (!ReadU16(data, size, off, vc.uvIndex)) return false;
    }

    // Trailing block (morphingComponentCount uint32s) is intentionally skipped -
    // see figure-format.md "Unread Trailer"; purpose unconfirmed, not needed to render.
    out.valid = true;
    return true;
}

inline bool ParseFig(const std::vector<uint8_t>& bytes, FigureMesh& out) {
    return ParseFig(bytes.data(), bytes.size(), out);
}

// ----------------------------------------------------------------------------
// .bon - assembly offsets
// ----------------------------------------------------------------------------

// 8 x vec3, one per complection corner.
inline bool ParseBonOffsets(const uint8_t* data, size_t size, Vec3 out[8]) {
    if (size < 96) return false;
    size_t off = 0;
    for (int i = 0; i < 8; ++i) {
        if (!ReadFloat(data, size, off, out[i].x)) return false;
        if (!ReadFloat(data, size, off, out[i].y)) return false;
        if (!ReadFloat(data, size, off, out[i].z)) return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// .mod - composite hierarchy (nested RES archive)
// ----------------------------------------------------------------------------

struct ModelPart {
    std::string name;
    std::string parentName; // empty if root
    FigureMesh mesh;
    Vec3 offset[8];           // from sibling .bon, own contribution only
    Vec3 accumulatedOffset[8]; // offset + all ancestors' offsets (filled by BuildHierarchy)
};

struct Model {
    std::vector<ModelPart> parts; // index-addressable; hierarchy resolved via parentName
    bool isComposite = false;     // false => single-part simple figure (parts[0] only)

    int FindPartIndex(const std::string& name) const {
        std::string lower = res::Archive::ToLower(name);
        for (size_t i = 0; i < parts.size(); ++i) {
            if (res::Archive::ToLower(parts[i].name) == lower) return static_cast<int>(i);
        }
        return -1;
    }
};

// Parses the link-table payload: nLink, then repeated {childLen+child, parentLen(+parent)}.
struct LinkEntry { std::string child, parent; };
inline bool ParseLinkTable(const uint8_t* data, size_t size, std::vector<LinkEntry>& out) {
    size_t off = 0;
    int32_t nLink;
    if (!ReadI32(data, size, off, nLink)) return false;
    out.clear();
    out.reserve(static_cast<size_t>(nLink));
    for (int32_t i = 0; i < nLink; ++i) {
        int32_t childLen;
        if (!ReadI32(data, size, off, childLen)) return false;
        if (off + static_cast<size_t>(childLen) > size) return false;
        std::string child(reinterpret_cast<const char*>(data + off), static_cast<size_t>(childLen));
        off += static_cast<size_t>(childLen);
        // strip trailing NUL(s)
        while (!child.empty() && child.back() == '\0') child.pop_back();

        int32_t parentLen;
        if (!ReadI32(data, size, off, parentLen)) return false;
        std::string parent;
        if (parentLen != 0) {
            if (off + static_cast<size_t>(parentLen) > size) return false;
            parent.assign(reinterpret_cast<const char*>(data + off), static_cast<size_t>(parentLen));
            off += static_cast<size_t>(parentLen);
            while (!parent.empty() && parent.back() == '\0') parent.pop_back();
        }
        out.push_back({child, parent});
    }
    return true;
}

// Loads a simple (non-composite) figure: modName is the base name without extension;
// figData/bonData are that base name's sibling .fig/.bon file contents (bonData may be empty
// if no .bon file exists alongside).
inline bool LoadSimpleFigure(const std::vector<uint8_t>& figData, const std::vector<uint8_t>* bonData,
                              const std::string& name, Model& out) {
    ModelPart part;
    part.name = name;
    if (!ParseFig(figData, part.mesh)) return false;
    if (bonData && !bonData->empty()) {
        ParseBonOffsets(bonData->data(), bonData->size(), part.offset);
    }
    for (int i = 0; i < 8; ++i) part.accumulatedOffset[i] = part.offset[i];
    out.parts.clear();
    out.parts.push_back(std::move(part));
    out.isComposite = false;
    return true;
}

// Loads a composite model: modArchive is the parsed .mod RES, bonArchive is the parsed
// sibling .bon RES (may have zero entries if missing), rootEntryName is the .mod's own
// base name (the link-table entry to use).
inline bool LoadCompositeModel(const res::Archive& modArchive, const res::Archive& bonArchive,
                                const std::string& rootEntryName, Model& out) {
    const std::vector<uint8_t>* linkData = modArchive.Find(rootEntryName);
    if (!linkData) return false;
    std::vector<LinkEntry> links;
    if (!ParseLinkTable(linkData->data(), linkData->size(), links)) return false;

    out.parts.clear();
    out.isComposite = true;
    std::map<std::string, int> nameToIndex;

    for (const auto& link : links) {
        const std::vector<uint8_t>* meshData = modArchive.Find(link.child);
        if (!meshData) continue; // referenced part missing; skip defensively
        ModelPart part;
        part.name = link.child;
        part.parentName = link.parent;
        if (!ParseFig(*meshData, part.mesh)) continue;
        const std::vector<uint8_t>* bonData = bonArchive.Find(link.child);
        if (bonData) ParseBonOffsets(bonData->data(), bonData->size(), part.offset);
        nameToIndex[res::Archive::ToLower(link.child)] = static_cast<int>(out.parts.size());
        out.parts.push_back(std::move(part));
    }

    // Accumulate offsets down the hierarchy (root parts first; parents are always
    // link-table-declared before use since the vanilla data does, but we don't rely
    // on ordering - resolve iteratively).
    std::vector<bool> resolved(out.parts.size(), false);
    bool progress = true;
    size_t resolvedCount = 0;
    while (progress && resolvedCount < out.parts.size()) {
        progress = false;
        for (size_t i = 0; i < out.parts.size(); ++i) {
            if (resolved[i]) continue;
            ModelPart& p = out.parts[i];
            if (p.parentName.empty()) {
                for (int c = 0; c < 8; ++c) p.accumulatedOffset[c] = p.offset[c];
                resolved[i] = true;
                ++resolvedCount;
                progress = true;
                continue;
            }
            auto it = nameToIndex.find(res::Archive::ToLower(p.parentName));
            if (it == nameToIndex.end()) {
                // Unknown parent (shouldn't happen in valid data) - treat as root.
                for (int c = 0; c < 8; ++c) p.accumulatedOffset[c] = p.offset[c];
                resolved[i] = true;
                ++resolvedCount;
                progress = true;
                continue;
            }
            if (!resolved[static_cast<size_t>(it->second)]) continue;
            const ModelPart& parent = out.parts[static_cast<size_t>(it->second)];
            for (int c = 0; c < 8; ++c) p.accumulatedOffset[c] = p.offset[c] + parent.accumulatedOffset[c];
            resolved[i] = true;
            ++resolvedCount;
            progress = true;
        }
    }
    return !out.parts.empty();
}

// ----------------------------------------------------------------------------
// .anm - skeletal animation
// ----------------------------------------------------------------------------

struct BoneTrack {
    std::vector<Quat> rotations;   // per frame, (w, x, y, z)
    std::vector<Vec3> positions;   // per frame: the root's position; others: W(parent) * rest offset (docs)
    uint32_t vertexFrames = 0, vertexCount = 0;
    std::vector<Vec3> vertices;    // per-vertex animation, vertexFrames * vertexCount (bowstrings); usually empty
};

struct AnimClip {
    std::map<std::string, BoneTrack> bones; // key: lowercase part name
    size_t FrameCount() const {
        size_t n = 0;
        for (auto& kv : bones) n = std::max(n, kv.second.rotations.size());
        return n;
    }
};

// The pose of every part of a model at a frame of a clip (docs/file-formats/figure-format.md): its rotation W and
// position P, so a vertex v is at P + W * v. W(part) = q(part) * W(parent); a root's P is its track's position (when
// groundScale > 0: its height scaled by it (GroundScale), on the ground; else relative to the clip's first frame, so
// the model stays where its rest pose is), any other part's P(parent) + W(parent) * its .bon offset for complection k.
// Without a clip (or for a part it has no track for): no rotation, the rest pose. `frame` is fractional and wraps.
struct PartPose { Quat w; Vec3 p; };
struct Model;
inline std::vector<PartPose> PoseModel(const Model& m, const AnimClip* clip, float frame, const Vec3& k, float groundScale);
// How far below the root the model reaches at rest (the hips to the soles), for complection k.
inline float RestDepth(const Model& m, const Vec3& k);
// The clips' root height is right for the build they were made on (complection corner 5: their offsets are that
// corner's, and their planted feet stay at the ground); for build k it scales by the legs: RestDepth(k) /
// RestDepth(corner 5). PoseModel's groundScale: this to stand on the ground; 0: the root relative to the clip's
// first frame (the model stays where its rest pose is).
inline float GroundScale(const Model& m, const Vec3& k) {
    const float d5 = RestDepth(m, {0, 1, 1});
    return d5 > 0.01f ? RestDepth(m, k) / d5 : 1.0f;
}

// u32 N, N quats; u32 N, N vec3; u32 F, u32 V, F*V vec3 (figure-format.md).
inline bool ParseBoneTrack(const uint8_t* data, size_t size, BoneTrack& out) {
    size_t off = 0;
    uint32_t frameCount, positionCount;
    if (!ReadU32(data, size, off, frameCount) || frameCount > 100000) return false;
    out.rotations.resize(frameCount);
    for (auto& q : out.rotations) {
        if (!ReadFloat(data, size, off, q.w)) return false;
        if (!ReadFloat(data, size, off, q.x)) return false;
        if (!ReadFloat(data, size, off, q.y)) return false;
        if (!ReadFloat(data, size, off, q.z)) return false;
    }
    if (!ReadU32(data, size, off, positionCount) || positionCount > 100000) return false;
    out.positions.resize(positionCount);
    for (auto& p : out.positions) {
        if (!ReadFloat(data, size, off, p.x)) return false;
        if (!ReadFloat(data, size, off, p.y)) return false;
        if (!ReadFloat(data, size, off, p.z)) return false;
    }
    if (!ReadU32(data, size, off, out.vertexFrames) || !ReadU32(data, size, off, out.vertexCount)) return true; // older files: none
    const uint64_t n = static_cast<uint64_t>(out.vertexFrames) * out.vertexCount;
    if (n * 12 > size - off) { out.vertexFrames = out.vertexCount = 0; return true; }
    out.vertices.resize(static_cast<size_t>(n));
    for (auto& v : out.vertices) {
        ReadFloat(data, size, off, v.x);
        ReadFloat(data, size, off, v.y);
        ReadFloat(data, size, off, v.z);
    }
    return true;
}

// Parses a full .anm file (outer RES of clips, each a RES of bone tracks).
inline bool ParseAnm(const std::vector<uint8_t>& bytes, std::map<std::string, AnimClip>& outClips, std::string& err) {
    res::Archive outer;
    if (!res::ParseArchive(bytes, outer, err)) return false;
    for (auto& kv : outer.entries) {
        res::Archive clipArchive;
        std::string clipErr;
        if (!res::ParseArchive(kv.second.data, clipArchive, clipErr)) continue; // skip malformed
        AnimClip clip;
        for (auto& boneKv : clipArchive.entries) {
            BoneTrack track;
            if (ParseBoneTrack(boneKv.second.data.data(), boneKv.second.data.size(), track)) {
                clip.bones[boneKv.first] = std::move(track);
            }
        }
        if (!clip.bones.empty()) outClips[kv.second.originalName] = std::move(clip);
    }
    return true;
}

// ----------------------------------------------------------------------------
// .lnk - attachment socket redirect
// ----------------------------------------------------------------------------

// Returns the redirect suffix string (e.g. "item", "weapon"), or empty on parse failure.
inline bool ParseLnk(const uint8_t* data, size_t size, std::string& outSuffix) {
    size_t off = 0;
    uint32_t tag, suffixLen;
    if (!ReadU32(data, size, off, tag)) return false;
    if (!ReadU32(data, size, off, suffixLen)) return false;
    if (off + suffixLen > size) return false;
    std::string s(reinterpret_cast<const char*>(data + off), suffixLen);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    outSuffix = s;
    return true;
}

// (declared above, next to AnimClip)
inline std::vector<PartPose> PoseModel(const Model& m, const AnimClip* clip, float frame, const Vec3& k, float groundScale) {
    const size_t n = m.parts.size();
    std::vector<PartPose> pose(n);
    std::vector<int> state(n, 0); // 0 to do, 1 doing, 2 done
    std::map<std::string, int> byName;
    auto lower = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
    for (size_t i = 0; i < n; ++i) byName[lower(m.parts[i].name)] = static_cast<int>(i);
    std::function<void(size_t)> solve = [&](size_t i) {
        if (state[i] == 2) return;
        state[i] = 1;
        const ModelPart& part = m.parts[i];
        Quat q{};
        Vec3 trackPos{}, firstPos{};
        bool hasPos = false;
        if (clip) {
            auto t = clip->bones.find(lower(part.name));
            if (t != clip->bones.end() && !t->second.rotations.empty()) {
                const BoneTrack& tr = t->second;
                const size_t frames = tr.rotations.size();
                const float f = std::fmod(std::max(frame, 0.0f), static_cast<float>(frames));
                const size_t a = static_cast<size_t>(f) % frames, b = (a + 1) % frames;
                const float u = f - std::floor(f);
                q = QSlerp(tr.rotations[a], tr.rotations[b], u);
                if (a < tr.positions.size() && b < tr.positions.size()) {
                    hasPos = true;
                    trackPos = Lerp(tr.positions[a], tr.positions[b], u);
                    firstPos = tr.positions[0];
                }
            }
        }
        const Vec3 offset = BlendComplection(part.offset, k);
        auto parent = part.parentName.empty() ? byName.end() : byName.find(lower(part.parentName));
        if (parent == byName.end() || state[static_cast<size_t>(parent->second)] == 1) { // a root
            pose[i].w = q;
            if (!hasPos) pose[i].p = offset;
            else if (groundScale <= 0) pose[i].p = offset + (trackPos - firstPos);
            else pose[i].p = {trackPos.x, trackPos.y, trackPos.z * groundScale}; // on the ground (GroundScale)
        } else {
            const size_t pi = static_cast<size_t>(parent->second);
            solve(pi);
            pose[i].w = QMul(q, pose[pi].w);
            pose[i].p = pose[pi].p + QRotate(pose[pi].w, offset);
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < n; ++i) solve(i);
    return pose;
}

inline float RestDepth(const Model& m, const Vec3& k) {
    float low = 0;
    for (const ModelPart& part : m.parts) {
        const Vec3 offset = BlendComplection(part.accumulatedOffset, k);
        for (size_t v = 0; v < part.mesh.vertexComponents.size(); ++v) low = std::min(low, part.mesh.BlendedPosition(v, k).z + offset.z);
    }
    return -low;
}

// The idle clip of a figure's animations, for showing a unit at rest the way the game does: "cidle01", else the
// first "cidle...", else none.
inline const AnimClip* IdleClip(const std::map<std::string, AnimClip>& clips) {
    auto it = clips.find("cidle01");
    if (it != clips.end()) return &it->second;
    for (const auto& kv : clips)
        if (kv.first.compare(0, 5, "cidle") == 0) return &kv.second;
    return nullptr;
}

} // namespace fig
