// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Imports a new hand-held weapon (a crossbow today) into the human rigs (unhuma / unhufe): reads the model from an
// OBJ file, places it on the rig's hinges (the limbs) and string anchors, builds the 8 complection corners the way
// the vanilla crossbows are built, and writes the new parts into copies of the rig's .mod / .bon.
//
// How the vanilla weapons scale (measured on every vanilla crossbow, docs/file-formats/figure-format.md "Weapons"):
// the parts of a crossbow are ONE mesh at complection corner 5, multiplied by a single number per corner (the
// "corner scales", different for unhuma and unhufe); the parts' .bon pivots follow the same numbers. So a new weapon
// is authored once, in the frame of the rig's crossbow ancestors at corner 5, and its other 7 corners are that mesh
// times the race's corner scales, measured here from a vanilla crossbow of the same race.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "asset_source.hpp"
#include "figure_write.hpp"

namespace wimp {

using fig::Vec2;
using fig::Vec3;

// ---- small vector maths ---------------------------------------------------------------------------------
inline double Dot(const Vec3& a, const Vec3& b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Norm(const Vec3& a) { const double l = Len(a); return l > 1e-12 ? a * static_cast<float>(1.0 / l) : Vec3{}; }
inline Vec3 Mix(const Vec3& a, const Vec3& b, double t) { return a * static_cast<float>(1 - t) + b * static_cast<float>(t); }

// ---- OBJ ------------------------------------------------------------------------------------------------
struct Corner { int v = -1, t = -1, n = -1; }; // indices into the file's pos / uv / nrm arrays (-1 = none)
using Tri = std::array<Corner, 3>;

struct ObjFile {
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uv;
    struct Object { std::string name; std::vector<Tri> tris; };
    std::vector<Object> objects;
};

inline bool ParseObj(const std::string& text, ObjFile& out, std::string& err) {
    out = ObjFile{};
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto resolve = [](int idx, size_t count) -> int { return idx > 0 ? idx - 1 : idx < 0 ? static_cast<int>(count) + idx : -1; };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tag;
        if (!(ls >> tag) || tag[0] == '#') continue;
        if (tag == "v") { Vec3 p; ls >> p.x >> p.y >> p.z; out.pos.push_back(p); }
        else if (tag == "vn") { Vec3 p; ls >> p.x >> p.y >> p.z; out.nrm.push_back(p); }
        else if (tag == "vt") { Vec2 p; ls >> p.x >> p.y; out.uv.push_back(p); }
        else if (tag == "o" || tag == "g") {
            std::string name; std::getline(ls >> std::ws, name);
            if (!out.objects.empty() && out.objects.back().tris.empty()) out.objects.back().name = name;
            else out.objects.push_back({name, {}});
        } else if (tag == "f") {
            if (out.objects.empty()) out.objects.push_back({"object", {}});
            std::vector<Corner> poly;
            std::string tok;
            while (ls >> tok) {
                int a = 0, b = 0, c = 0;
                Corner k;
                const size_t s1 = tok.find('/');
                a = std::atoi(tok.c_str());
                if (s1 != std::string::npos) {
                    const size_t s2 = tok.find('/', s1 + 1);
                    if (s2 == std::string::npos) b = std::atoi(tok.c_str() + s1 + 1);
                    else { if (s2 > s1 + 1) b = std::atoi(tok.c_str() + s1 + 1); c = std::atoi(tok.c_str() + s2 + 1); }
                }
                k.v = resolve(a, out.pos.size()); k.t = b ? resolve(b, out.uv.size()) : -1; k.n = c ? resolve(c, out.nrm.size()) : -1;
                if (k.v < 0 || k.v >= static_cast<int>(out.pos.size())) { err = "line " + std::to_string(lineNo) + ": bad vertex index"; return false; }
                if (k.t >= static_cast<int>(out.uv.size()) || k.n >= static_cast<int>(out.nrm.size())) { err = "line " + std::to_string(lineNo) + ": bad uv / normal index"; return false; }
                poly.push_back(k);
            }
            for (size_t i = 1; i + 1 < poly.size(); ++i) out.objects.back().tris.push_back({poly[0], poly[i], poly[i + 1]});
        }
    }
    if (out.objects.empty()) { err = "no faces in the OBJ"; return false; }
    return true;
}

// ---- pieces: one mesh of the weapon, with its own position / normal / uv arrays ---------------------------
struct Piece {
    std::string name, role;
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uv;
    std::vector<Tri> tris; // indices into the arrays above
};

// The object's triangles as a self-contained piece (the arrays reduced to what the triangles use). A missing normal
// becomes the flat face normal; a missing UV is an error (a weapon needs its texture mapping).
inline bool MakePiece(const ObjFile& f, const std::vector<Tri>& tris, const std::string& name, Piece& out, std::string& err) {
    out = Piece{};
    out.name = name;
    std::map<int, int> pm, nm, um;
    auto add = [](auto& m, int k, auto& dst, const auto& v) { auto it = m.find(k); if (it != m.end()) return it->second; const int i = static_cast<int>(dst.size()); m[k] = i; dst.push_back(v); return i; };
    for (const Tri& t : tris) {
        Tri o;
        Vec3 flat{};
        const Vec3 &a = f.pos[static_cast<size_t>(t[0].v)], &b = f.pos[static_cast<size_t>(t[1].v)], &c = f.pos[static_cast<size_t>(t[2].v)];
        flat = Norm(Cross(b - a, c - a));
        for (int i = 0; i < 3; ++i) {
            if (t[i].t < 0) { err = "object '" + name + "' has a face without UV coordinates"; return false; }
            o[i].v = add(pm, t[i].v, out.pos, f.pos[static_cast<size_t>(t[i].v)]);
            o[i].t = add(um, t[i].t, out.uv, f.uv[static_cast<size_t>(t[i].t)]);
            if (t[i].n >= 0) o[i].n = add(nm, t[i].n, out.nrm, Norm(f.nrm[static_cast<size_t>(t[i].n)]));
            else { o[i].n = static_cast<int>(out.nrm.size()); out.nrm.push_back(flat); } // flat normals are not shared
        }
        out.tris.push_back(o);
    }
    return !out.tris.empty();
}

// Splits an object into its connected islands (triangles sharing a position; positions are compared by value, since
// some exporters repeat a vertex per face), largest first.
inline std::vector<std::vector<Tri>> SplitIslands(const ObjFile& f, const std::vector<Tri>& tris) {
    std::map<std::array<int64_t, 3>, int> idOf;
    std::vector<int> parent;
    auto id = [&](const Vec3& v) {
        const std::array<int64_t, 3> k = {std::llround(v.x * 1e5), std::llround(v.y * 1e5), std::llround(v.z * 1e5)};
        auto it = idOf.find(k);
        if (it == idOf.end()) { it = idOf.emplace(k, static_cast<int>(parent.size())).first; parent.push_back(it->second); }
        return it->second;
    };
    std::function<int(int)> find = [&](int x) { while (parent[static_cast<size_t>(x)] != x) { parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])]; x = parent[static_cast<size_t>(x)]; } return x; };
    std::vector<std::array<int, 3>> tid;
    for (const Tri& t : tris) tid.push_back({id(f.pos[static_cast<size_t>(t[0].v)]), id(f.pos[static_cast<size_t>(t[1].v)]), id(f.pos[static_cast<size_t>(t[2].v)])});
    for (const auto& t : tid) { parent[static_cast<size_t>(find(t[1]))] = find(t[0]); parent[static_cast<size_t>(find(t[2]))] = find(t[0]); }
    std::map<int, std::vector<Tri>> groups;
    for (size_t i = 0; i < tris.size(); ++i) groups[find(tid[i][0])].push_back(tris[i]);
    std::vector<std::vector<Tri>> out;
    for (auto& g : groups) out.push_back(std::move(g.second));
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.size() > y.size(); });
    return out;
}

inline void PieceBounds(const Piece& p, Vec3& lo, Vec3& hi) {
    lo = {1e30f, 1e30f, 1e30f}; hi = {-1e30f, -1e30f, -1e30f};
    for (const Tri& t : p.tris) for (const Corner& c : t) {
        const Vec3& v = p.pos[static_cast<size_t>(c.v)];
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
}
// The positions the triangles use, each once (by value: a vertex repeated per normal or per face counts once, so the
// statistics below do not depend on how the file splits its vertices).
inline std::vector<Vec3> UniquePositions(const std::vector<Vec3>& in) {
    std::set<std::array<int64_t, 3>> seen;
    std::vector<Vec3> out;
    for (const Vec3& v : in) if (seen.insert({std::llround(v.x * 1e5), std::llround(v.y * 1e5), std::llround(v.z * 1e5)}).second) out.push_back(v);
    return out;
}
inline std::vector<Vec3> UsedPositions(const Piece& p) {
    std::vector<Vec3> all;
    for (const Tri& t : p.tris) for (const Corner& c : t) all.push_back(p.pos[static_cast<size_t>(c.v)]);
    return UniquePositions(all);
}
inline Vec3 Centroid(const std::vector<Vec3>& v) {
    Vec3 s{};
    for (const Vec3& p : v) s = s + p;
    return v.empty() ? s : s * (1.0f / static_cast<float>(v.size()));
}

// ---- the rig of one race ----------------------------------------------------------------------------------
struct Rig {
    std::string race; // unhuma / unhufe
    res::Archive mod, bon;
    std::vector<fig::LinkEntry> links;
    std::map<std::string, std::string> parentOf; // lower-case child -> parent (as written)
    uint32_t timestamp = 0;

    bool HasPart(const std::string& n) const { return mod.Contains(n); }
    bool LoadMesh(const std::string& n, fig::FigureMesh& m) const {
        const auto* d = mod.Find(n);
        return d && fig::ParseFig(*d, m);
    }
    // The part's .bon offsets for the 8 corners (zero when the part has no .bon entry).
    void Offsets(const std::string& n, Vec3 out[8]) const {
        for (int c = 0; c < 8; ++c) out[c] = {};
        const auto* d = bon.Find(n);
        if (d) fig::ParseBonOffsets(d->data(), d->size(), out);
    }
};


inline uint32_t FirstTimestamp(const std::vector<uint8_t>& b) {
    if (b.size() < 16) return 0;
    uint32_t count, table;
    std::memcpy(&count, b.data() + 4, 4);
    std::memcpy(&table, b.data() + 8, 4);
    for (uint32_t i = 0; i < count && static_cast<size_t>(table) + (i + 1) * 22 <= b.size(); ++i) {
        uint16_t nameLen;
        std::memcpy(&nameLen, b.data() + table + i * 22 + 16, 2);
        if (nameLen) { uint32_t ts; std::memcpy(&ts, b.data() + table + i * 22 + 12, 4); return ts; }
    }
    return 0;
}

inline bool LoadRigBytes(const std::vector<uint8_t>& modBytes, const std::vector<uint8_t>& bonBytes, const std::string& race, Rig& rig, std::string& err) {
    rig = Rig{};
    rig.race = race;
    if (!res::ParseArchive(modBytes, rig.mod, err)) { err = race + ".mod: " + err; return false; }
    if (!res::ParseArchive(bonBytes, rig.bon, err)) { err = race + ".bon: " + err; return false; }
    const auto* lt = rig.mod.Find(race);
    if (!lt || !fig::ParseLinkTable(lt->data(), lt->size(), rig.links)) { err = race + ".mod has no link table entry '" + race + "'"; return false; }
    for (const auto& l : rig.links) rig.parentOf[res::Archive::ToLower(l.child)] = l.parent;
    rig.timestamp = FirstTimestamp(modBytes);
    return true;
}

// Reads <race>.mod / <race>.bon from the figure layers (the top layer that has them wins, like in the game).
inline bool LoadRig(const LayeredAssetSource& src, const std::string& race, Rig& rig, std::string& err) {
    std::vector<uint8_t> modBytes, bonBytes;
    if (!src.ReadFile(race + ".mod", modBytes)) { err = race + ".mod not found in the figure sources"; return false; }
    if (!src.ReadFile(race + ".bon", bonBytes)) { err = race + ".bon not found in the figure sources"; return false; }
    return LoadRigBytes(modBytes, bonBytes, race, rig, err);
}

// The same from the lowest layer that has them: the base game's rigs, without the weapons a mod added. The corner scales and the male / female ratio are
// measured there, so what a mod put on top of the game does not change how a new weapon scales (a variant of the mod can still be named as the reference).
inline bool LoadBaseRig(const LayeredAssetSource& src, const std::string& race, Rig& rig, std::string& err) {
    for (const auto& layer : src.layers) {
        if (!layer.ok) continue;
        std::vector<uint8_t> m, b;
        if (layer.source.ReadFile(race + ".mod", m) && layer.source.ReadFile(race + ".bon", b)) return LoadRigBytes(m, b, race, rig, err);
    }
    return LoadRig(src, race, rig, err);
}

// The offset of a part's frame from the root ancestor's frame at a corner: its own .bon offset plus its parents'
// (the root ancestor itself is left out: its offset only places the whole weapon in the hand).
inline Vec3 ChainOrigin(const Rig& rig, const std::string& part, const std::string& rootAncestor, int corner) {
    Vec3 sum{};
    std::string cur = part;
    const std::string root = res::Archive::ToLower(rootAncestor);
    for (int guard = 0; guard < 32 && !cur.empty() && res::Archive::ToLower(cur) != root; ++guard) {
        Vec3 off[8];
        rig.Offsets(cur, off);
        sum = sum + off[corner];
        auto it = rig.parentOf.find(res::Archive::ToLower(cur));
        cur = it != rig.parentOf.end() ? it->second : std::string();
    }
    return sum;
}

// ---- the weapon kind ------------------------------------------------------------------------------------
struct RoleDef { const char* role; const char* pattern; const char* ancestor; }; // "{N}" = the two-digit number
enum class Family { Crossbow, Bow, Melee };
struct Kind {
    const char* id;                 // the name on the command line and in the project file
    const char* label;              // what the panel shows
    Family family;
    const char* itemType;           // the database's weapon type that holds it (the preview picks an item of that type)
    const char* rootAncestor;       // the part that carries the animation / the hand's frame; the new parts hang under it
    std::vector<RoleDef> roles;
    const char* defaultReference;   // the vanilla variant the corner scales are measured on; "" = all the variants of the family, averaged
    std::vector<const char*> pool;  // melee: the patterns of the variants that are averaged
    int firstNew;                   // the lowest number picked when none is given
    // melee: where the hand holds the weapon (measured on the vanilla ones, tools in tests/weapon_melee_calib.cpp)
    double gripFromButt = 0;        // the hand is this far from the butt end (rig units)
    double gripFraction = 0;        // or this share of the length (a pike is held at 41%), when gripFromButt is 0
    struct Grip { double x = 0, z = 0, tilt = 0; };   // the handle's centre line at the hand (in the root ancestor's frame) and how many degrees above level the vanilla handles point
    Grip grip[2];                   // [0] unhuma, [1] unhufe (another rig: unhuma's)
    bool headMinusX = false;        // an asymmetric head (an axe's) points to -X, like the vanilla ones
    bool tipByPoint = false;        // the tip is the sharper end (swords, daggers); else the end the cross-sections' weight leans to (an axe's head, a club's, a spear's)
    const char* basePattern = "";   // the tiny "bash" part under the variant
};

inline const std::vector<Kind>& AllKinds() {
    static const std::vector<Kind> k = {
        // A crossbow variant NN: the stock, two limbs and two strings; every part hangs under the "01" crossbow's part,
        // which carries the animation (the limbs' and strings' tracks exist only for the 01 parts).
        {"crossbow", "Crossbow", Family::Crossbow, "crossbow", "rh3.crbow01main",
         {{"main", "rh3.crbow{N}main", "rh3.crbow01main"}, {"part01", "crbow{N}part01", "crbow01part01"}, {"part02", "crbow{N}part02", "crbow01part02"},
          {"tetiva01", "crbow{N}tetiva01", "crbow01tetiva01"}, {"tetiva02", "crbow{N}tetiva02", "crbow01tetiva02"}}, "02", {}, 4, 0, 0, {}, false, false, ""},
        // A bow variant NN: two limb halves (b = the +Y half, which hangs under the hand; a = the -Y half) and a string half on each: the
        // 00 parts carry the animation (every variant hangs under them).
        {"bow", "Bow", Family::Bow, "bow", "lh3.bwpartb00",
         {{"partb", "lh3.bwpartb{N}", "lh3.bwpartb00"}, {"parta", "bwparta{N}", "bwparta00"}, {"tetivaa", "bwtetivaa{N}", "bwtetivaa00"}, {"tetivab", "bwtetivab{N}", "bwtetivab00"}}, "00", {}, 6, 0, 0, {}, false, false, ""},
        // One rigid part per melee weapon, under the 00 part of its family (the axe's for the clubs, the sword's for the daggers).
        {"sword", "Sword", Family::Melee, "sword", "rh3.sword00", {{"main", "rh3.sword{N}", "rh3.sword00"}}, "", {"rh3.sword{N}", "rh3.dagger{N}"}, 6, 0.152, 0, {{0, 0, 0}, {0.001, 0, 0}}, false, true, "basesword{N}"},
        {"dagger", "Dagger", Family::Melee, "dagger", "rh3.sword00", {{"main", "rh3.dagger{N}", "rh3.sword00"}}, "", {"rh3.sword{N}", "rh3.dagger{N}"}, 5, 0.142, 0, {{0.004, 0, 0}, {0.008, -0.002, 0}}, false, true, "basedagger{N}"},
        {"axe", "Axe", Family::Melee, "axe", "rh3.axe00", {{"main", "rh3.axe{N}", "rh3.axe00"}}, "", {"rh3.axe{N}", "rh3.club{N}"}, 6, 0.19, 0, {{-0.048, -0.013, 1.3}, {-0.026, -0.010, 0}}, true, false, "baseaxe{N}"},
        {"club", "Club / hammer", Family::Melee, "hammer", "rh3.axe00", {{"main", "rh3.club{N}", "rh3.axe00"}}, "", {"rh3.axe{N}", "rh3.club{N}"}, 9, 0.153, 0, {{-0.048, -0.012, 1.3}, {-0.024, -0.005, 0}}, true, false, "baseclub{N}"},
        {"pike", "Spear / pike", Family::Melee, "spear", "rh3.pike00", {{"main", "rh3.pike{N}", "rh3.pike00"}}, "", {"rh3.pike{N}"}, 7, 0, 0.41, {{-0.003, 0.008, 0}, {-0.001, 0, 0}}, false, false, "basepike{N}"},
    };
    return k;
}
inline const Kind* FindKind(const std::string& id) {
    std::string l;
    for (char c : id) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const Kind& k : AllKinds()) if (l == k.id) return &k;
    return nullptr;
}
inline const Kind& CrossbowKind() { return *FindKind("crossbow"); }

inline std::string TwoDigits(int n) { char b[16]; std::snprintf(b, sizeof b, "%02d", n < 0 ? 0 : n > 99 ? 99 : n); return b; }
inline std::string PartName(const RoleDef& r, const std::string& nn) {
    std::string s = r.pattern;
    const size_t i = s.find("{N}");
    return i == std::string::npos ? s : s.replace(i, 3, nn);
}
inline std::string PatternName(const char* pattern, const std::string& nn) {
    std::string s = pattern;
    const size_t i = s.find("{N}");
    return i == std::string::npos ? s : s.replace(i, 3, nn);
}

// ---- the corner scales -----------------------------------------------------------------------------------
struct CornerScales {
    double k[8] = {1, 1, 1, 1, 1, 1, 1, 1}; // multiplies a corner-5 vector into corner c
    double worst = 0;                       // the largest distance (rig units) between a reference vector and k * its corner 5
    size_t samples = 0;
    int variants = 1;                       // how many vanilla variants it was measured on (more than 1: averaged)
};

// Measured from a vanilla variant (nn) of the kind: every vertex of its parts and every .bon pivot on the chain, as
// vectors at the 8 corners; k[c] = least squares of v(c) = k * v(5). nn empty (the melee weapons): every variant of the
// kind's family is averaged (their per-build shaping differs by a few cm from one to the other, an average is what none of them is wrong by much).
inline bool MeasureCornerScales(const Rig& rig, const Kind& kind, const std::string& nn, CornerScales& out, std::string& err) {
    std::vector<std::array<Vec3, 8>> samples;
    std::set<std::string> bonDone;
    auto addMesh = [&](const std::string& part) {
        fig::FigureMesh m;
        if (!rig.LoadMesh(part, m)) return false;
        for (size_t i = 0; i < figw::UsedPoints(m); ++i) {
            std::array<Vec3, 8> a;
            for (int c = 0; c < 8; ++c) a[static_cast<size_t>(c)] = m.morphVertex[c][i];
            samples.push_back(a);
        }
        return true;
    };
    if (nn.empty()) {
        int variants = 0;
        for (const char* pat : kind.pool)
            for (int n = 0; n < 100; ++n) {
                const std::string part = PatternName(pat, TwoDigits(n));
                if (rig.HasPart(part) && addMesh(part)) ++variants;
            }
        if (!variants) { err = rig.race + ": no vanilla " + kind.id + " to measure the scales on"; return false; }
        out.variants = variants;
    } else {
        for (const RoleDef& r : kind.roles) {
            const std::string part = PartName(r, nn);
            if (!addMesh(part)) { err = rig.race + ": the reference part " + part + " is missing"; return false; }
            // the part's own offset and its ancestors' (up to the root) are pivots that scale with the same numbers
            std::string cur = part;
            for (int guard = 0; guard < 32 && !cur.empty() && res::Archive::ToLower(cur) != res::Archive::ToLower(kind.rootAncestor); ++guard) {
                if (bonDone.insert(cur).second) {
                    Vec3 off[8];
                    rig.Offsets(cur, off);
                    std::array<Vec3, 8> a;
                    for (int c = 0; c < 8; ++c) a[static_cast<size_t>(c)] = off[c];
                    samples.push_back(a);
                }
                auto it = rig.parentOf.find(res::Archive::ToLower(cur));
                cur = it != rig.parentOf.end() ? it->second : std::string();
            }
        }
        out.variants = 1;
    }
    double den = 0;
    for (const auto& a : samples) den += Dot(a[5], a[5]);
    if (den < 1e-9) { err = "the reference parts have no size"; return false; }
    for (int c = 0; c < 8; ++c) {
        double num = 0;
        for (const auto& a : samples) num += Dot(a[static_cast<size_t>(c)], a[5]);
        out.k[c] = num / den;
    }
    out.worst = 0;
    for (const auto& a : samples)
        for (int c = 0; c < 8; ++c) out.worst = std::max(out.worst, Len(a[static_cast<size_t>(c)] - a[5] * static_cast<float>(out.k[c])));
    out.samples = samples.size();
    return true;
}

// ---- placement ------------------------------------------------------------------------------------------
struct Placement {
    double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double s = 1;
    Vec3 t{};
    bool swappedLimbs = false, swappedStrings = false; // the fit found the named sides the other way round and swapped them
    double gripShift = 0;   // crossbow: how far the stock had to be moved in height to sit in the hand like the reference's
    double heightError = 0; // crossbow: how far the string ends / the hinge end up from the rig's in height (a model turned the wrong way up is 10-20 cm off)
    double bend = 0, bendLength = 1; // bows: the limbs' tips (and the strings) are moved this far in X, the limbs by (|y| / bendLength)^2 of it: see FitBow
    double prodDrop = 0; // the limbs and strings are lowered by this (rig units, corner 5) so the limb roots sit at the hinge's height (FitOptions::prodAtHinge)
    Vec3 Dir(const Vec3& v) const {
        return {static_cast<float>(R[0][0] * v.x + R[0][1] * v.y + R[0][2] * v.z), static_cast<float>(R[1][0] * v.x + R[1][1] * v.y + R[1][2] * v.z),
                static_cast<float>(R[2][0] * v.x + R[2][1] * v.y + R[2][2] * v.z)};
    }
    Vec3 Apply(const Vec3& v) const { return Dir(v) * static_cast<float>(s) + t; }
};

struct Pca { Vec3 axis[3]; double extent[3] = {0, 0, 0}; Vec3 center; }; // axis 0 = the longest spread
inline Pca ComputePca(const std::vector<Vec3>& pts) {
    Pca r;
    r.center = Centroid(pts);
    double c[3][3] = {};
    for (const Vec3& p : pts) {
        const double d[3] = {p.x - r.center.x, p.y - r.center.y, p.z - r.center.z};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) c[i][j] += d[i] * d[j];
    }
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // Jacobi eigen decomposition of the symmetric covariance
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = std::fabs(c[0][1]) + std::fabs(c[0][2]) + std::fabs(c[1][2]);
        if (off < 1e-14) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (std::fabs(c[p][q]) < 1e-30) continue;
            const double th = (c[q][q] - c[p][p]) / (2 * c[p][q]);
            const double t = (th >= 0 ? 1.0 : -1.0) / (std::fabs(th) + std::sqrt(th * th + 1));
            const double co = 1 / std::sqrt(t * t + 1), si = t * co;
            for (int k = 0; k < 3; ++k) { const double a = c[k][p], b = c[k][q]; c[k][p] = co * a - si * b; c[k][q] = si * a + co * b; }
            for (int k = 0; k < 3; ++k) { const double a = c[p][k], b = c[q][k]; c[p][k] = co * a - si * b; c[q][k] = si * a + co * b; }
            for (int k = 0; k < 3; ++k) { const double a = v[k][p], b = v[k][q]; v[k][p] = co * a - si * b; v[k][q] = si * a + co * b; }
        }
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return c[a][a] > c[b][b]; });
    for (int i = 0; i < 3; ++i) {
        const int o = order[i];
        r.axis[i] = Norm({static_cast<float>(v[0][o]), static_cast<float>(v[1][o]), static_cast<float>(v[2][o])});
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : pts) { const double d = Dot(r.axis[i], p); lo = std::min(lo, d); hi = std::max(hi, d); }
        r.extent[i] = hi - lo;
    }
    return r;
}

// ---- roles ----------------------------------------------------------------------------------------------
using RoleMap = std::map<std::string, Piece>;

inline bool NameMatches(const std::string& lowerName, const std::vector<const char*>& tokens) {
    for (const char* t : tokens) if (lowerName.find(t) != std::string::npos) return true;
    return false;
}

// The five pieces of a crossbow: by the objects' names (main / part01 / part02 / tetiva01 / tetiva02, also stock,
// limb1, limb2, string1, string2) when all five are there, else by the shape of the five loose pieces of the model.
inline bool FindCrossbowRoles(const ObjFile& f, RoleMap& roles, std::vector<std::string>& log, std::string& err) {
    roles.clear();
    static const std::vector<std::pair<const char*, std::vector<const char*>>> tokens = {
        {"main", {"main", "stock"}}, {"part01", {"part01", "limb01", "limb1", "limb_1"}}, {"part02", {"part02", "limb02", "limb2", "limb_2"}},
        {"tetiva01", {"tetiva01", "string01", "string1", "string_1"}}, {"tetiva02", {"tetiva02", "string02", "string2", "string_2"}}};
    std::map<std::string, size_t> byName;
    bool named = true;
    for (const auto& tk : tokens) {
        int found = 0;
        for (size_t i = 0; i < f.objects.size(); ++i) {
            std::string n = f.objects[i].name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (NameMatches(n, tk.second)) { byName[tk.first] = i; ++found; }
        }
        if (found != 1) { named = false; break; }
    }
    if (named) {
        for (const auto& kv : byName) {
            Piece p;
            if (!MakePiece(f, f.objects[kv.second].tris, f.objects[kv.second].name, p, err)) return false;
            p.role = kv.first;
            roles[kv.first] = std::move(p);
        }
        log.push_back("pieces: taken from the object names (main, part01/02, tetiva01/02)");
        return true;
    }
    std::vector<Piece> islands;
    for (const auto& o : f.objects) {
        for (const auto& isl : SplitIslands(f, o.tris)) {
            Piece p;
            if (!MakePiece(f, isl, o.name, p, err)) return false;
            islands.push_back(std::move(p));
        }
    }
    if (islands.size() != 5) {
        err = "a crossbow needs 5 pieces (stock, 2 limbs, 2 strings): the model has " + std::to_string(islands.size()) +
              " loose pieces and its objects are not named main / part01 / part02 / tetiva01 / tetiva02";
        return false;
    }
    struct Info { size_t idx; double diag, thin; };
    std::vector<Info> info;
    for (size_t i = 0; i < islands.size(); ++i) {
        const Pca pc = ComputePca(UsedPositions(islands[i]));
        info.push_back({i, pc.extent[0], (pc.extent[1] + pc.extent[2]) / std::max(pc.extent[0], 1e-9)});
    }
    std::sort(info.begin(), info.end(), [](const Info& a, const Info& b) { return a.diag > b.diag; });
    roles["main"] = islands[info[0].idx]; // the longest piece is the stock
    std::vector<Info> rest(info.begin() + 1, info.end());
    std::sort(rest.begin(), rest.end(), [](const Info& a, const Info& b) { return a.thin > b.thin; }); // limbs are broader than strings
    roles["part01"] = islands[rest[0].idx]; roles["part02"] = islands[rest[1].idx];
    roles["tetiva01"] = islands[rest[2].idx]; roles["tetiva02"] = islands[rest[3].idx];
    log.push_back("pieces: told apart by their shape (the longest is the stock, the two thinnest the strings); sides are decided by the fit");
    return true;
}

// ---- the fit ----------------------------------------------------------------------------------------------
// The height of the middle of a mesh at a position along Y: the triangles are cut by planes y = y0 + k * halfBand (k = -1,
// -.5, 0, .5, 1) and the middle of the cut points' heights is averaged. False when no triangle reaches the position.
using TriPos = std::array<Vec3, 3>;
inline bool SliceCentreZ(const std::vector<TriPos>& tris, double y0, double halfBand, double& out) {
    double sum = 0;
    int n = 0;
    for (int k = -2; k <= 2; ++k) {
        const double y = y0 + k * 0.5 * halfBand;
        double lo = 1e30, hi = -1e30;
        for (const TriPos& t : tris)
            for (int e = 0; e < 3; ++e) {
                const Vec3 &a = t[static_cast<size_t>(e)], &b = t[static_cast<size_t>((e + 1) % 3)];
                if ((a.y - y) * (b.y - y) < 0) {
                    const double u = (y - a.y) / (double(b.y) - a.y), z = a.z + u * (double(b.z) - a.z);
                    lo = std::min(lo, z); hi = std::max(hi, z);
                }
            }
        if (lo <= hi) { sum += (lo + hi) / 2; ++n; }
    }
    if (!n) return false;
    out = sum / n;
    return true;
}

struct FitTargets {
    Vec3 hinge[2], tip[2];        // [0] = the part01 side (+X), [1] = part02; M frame, corner 5
    Vec3 forward{0, -1, 0};       // the reference stock's direction (toward the limbs): vanilla stocks are a few degrees off level
    Vec3 hand{};                  // where the character's hand is in this frame (the grip), corner 5
    double refGripZ = 0;          // the height of the reference stock's middle at the hand
    double gripBand = 0.12;       // half the length of the stretch of stock that counts as "at the hand"
    bool haveGrip = false;
};

struct FitOptions {
    bool autoFit = true;      // false: the model is already in the rig's frame (an exported reference)
    double scale = 0;         // > 0: this scale instead of the one that puts the string ends on the rig's anchors
    Vec3 shift{};             // added to the placement, in rig units (M frame)
    bool upIsZ = true;        // the model's up is +Z (Blender); false: -Z
    bool upAuto = false;      // crossbow, experimental: try both turns and keep the one that puts the stock at the reference's height in the hand (a stock that sits low, like crossbow 3's, fools it)
    double hingeWeight = 0.35; // the string ends are the real pivots (they weigh 1 each); the limbs' roots matter less
    bool gripHeight = true;   // height from the hand (the stock at the grip sits as the reference's does); false: from the string anchors
    bool levelStock = false;  // true: the stock exactly along -Y instead of the reference's tilt
    bool keepDepth = false;   // bow: keep the model's own depth (string to grip); else the limbs are bent so the grip sits on the rig's pivot and the string on its anchors
    bool flip = false;        // melee: tip and butt the other way round
    double roll = 0;          // melee: extra turn about the weapon's length, degrees
    double grip = 0;          // melee: the hand's distance from the butt (rig units); 0 = the vanilla rule of the kind
    bool prodAtHinge = true;  // lower the limbs and strings (together) until the limb roots sit at the hinge's height: some animations turn a limb over about the hinge (female deaths), which
                              // throws a root that is higher than the hinge off the stock; vanilla limbs are centred on their hinge. false: keep the model's own height of the prod
};

inline bool FitCrossbowOnce(RoleMap& roles, const FitTargets& T, const FitOptions& o, Placement& pl, std::vector<std::string>& log, std::string& err) {
    pl = Placement{};
    if (!o.autoFit) { pl.t = o.shift; if (o.scale > 0) pl.s = o.scale; log.push_back("fit: none (the model is taken as it is in the rig's frame)"); return true; }
    const std::vector<Vec3> stock = UsedPositions(roles["main"]);
    const Pca pc = ComputePca(stock);
    const Vec3 a = pc.axis[0];
    // the lateral axis: from one limb to the other, made square to the stock's axis
    const Vec3 c0 = Centroid(UsedPositions(roles["part01"])), c1 = Centroid(UsedPositions(roles["part02"]));
    Vec3 lat = Norm(c0 - c1);
    lat = Norm(lat - a * static_cast<float>(Dot(lat, a)));
    if (Len(lat) < 0.5) { err = "the two limbs lie on the stock's axis: cannot tell the sides"; return false; }
    // the limbs' root ends (nearest the stock's centre line) and the hinge between them
    const double lam0 = Dot(lat, pc.center);
    Vec3 roots[2];
    for (int i = 0; i < 2; ++i) {
        const std::vector<Vec3> pts = UsedPositions(roles[i == 0 ? "part01" : "part02"]);
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : pts) { const double l = Dot(lat, p); lo = std::min(lo, l); hi = std::max(hi, l); }
        const double end = std::fabs(lo - lam0) < std::fabs(hi - lam0) ? lo : hi;
        std::vector<Vec3> nearEnd;
        for (const Vec3& p : pts) if (std::fabs(Dot(lat, p) - end) <= 0.12 * (hi - lo)) nearEnd.push_back(p);
        roots[i] = Centroid(nearEnd);
    }
    const Vec3 hinge = (roots[0] + roots[1]) * 0.5f;
    // forward: along the stock's axis, toward the end the limbs are at
    double lo = 1e30, hi = -1e30;
    for (const Vec3& p : stock) { const double d = Dot(a, p); lo = std::min(lo, d); hi = std::max(hi, d); }
    const double hp = Dot(a, hinge);
    const Vec3 fwd = std::fabs(hp - lo) < std::fabs(hp - hi) ? a * -1.0f : a;
    // up: square to both, on the +Z side (Blender's up) unless told otherwise
    Vec3 up = Norm(Cross(fwd, lat));
    if ((up.z < 0) == o.upIsZ) up = up * -1.0f;
    const Vec3 xm = Cross(up, fwd), ym = fwd * -1.0f; // the model's frame: +X = the part01 side, +Y = toward the shooter, +Z = up
    if (std::fabs(up.z) < 0.5) log.push_back("warning: the model's up is far from +Z (" + std::to_string(up.z) + "): check the preview, or turn the model in Blender");
    // onto the rig's: +X the same, forward = the reference stock's direction (or exactly -Y), up square to both
    Vec3 tf = o.levelStock ? Vec3{0, -1, 0} : Vec3{0, T.forward.y, T.forward.z};
    tf = Norm(tf);
    const Vec3 Xt{1, 0, 0}, Yt = tf * -1.0f, Zt = Cross(Xt, Yt);
    const Vec3 tb[3] = {Xt, Yt, Zt}, mb[3] = {xm, ym, up};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double r = 0;
        for (int k = 0; k < 3; ++k) r += double(i == 0 ? tb[k].x : i == 1 ? tb[k].y : tb[k].z) * double(j == 0 ? mb[k].x : j == 1 ? mb[k].y : mb[k].z);
        pl.R[i][j] = r;
    }
    // sides: the limb / string on +X is part01 / tetiva01 (swap the named ones that came out the other way round)
    auto sideOf = [&](const Piece& p) { return pl.Dir(Centroid(UsedPositions(p))).x; };
    if (sideOf(roles["part01"]) < sideOf(roles["part02"])) { std::swap(roles["part01"], roles["part02"]); pl.swappedLimbs = true; log.push_back("note: the limbs were on the other sides: part01 / part02 swapped"); }
    if (sideOf(roles["tetiva01"]) < sideOf(roles["tetiva02"])) { std::swap(roles["tetiva01"], roles["tetiva02"]); pl.swappedStrings = true; log.push_back("note: the strings were on the other sides: tetiva01 / tetiva02 swapped"); }
    // the strings' outer ends: the anchors the rig rotates them about (the end farthest from the centre line, which is
    // where the limbs' roots meet: the model need not be centred on the origin)
    const double xc = pl.Dir(hinge).x;
    Vec3 tips[2];
    for (int i = 0; i < 2; ++i) {
        const std::vector<Vec3> pts = UsedPositions(roles[i == 0 ? "tetiva01" : "tetiva02"]);
        double mx = 0, mn = 1e30;
        for (const Vec3& p : pts) { const double x = std::fabs(pl.Dir(p).x - xc); mx = std::max(mx, x); mn = std::min(mn, x); }
        std::vector<Vec3> end;
        for (const Vec3& p : pts) if (std::fabs(pl.Dir(p).x - xc) >= mx - 0.15 * (mx - mn)) end.push_back(p);
        tips[i] = Centroid(end);
    }
    const Vec3 hq = pl.Dir(hinge), t0 = pl.Dir(tips[0]), t1 = pl.Dir(tips[1]);
    if (std::getenv("UM_FIT_DEBUG")) {
        char d[400];
        std::snprintf(d, sizeof d, "fit-debug: lat (%.3f %.3f %.3f) roots (%.3f %.3f %.3f) (%.3f %.3f %.3f) tips (%.3f %.3f %.3f) (%.3f %.3f %.3f) stock centre (%.3f %.3f %.3f)",
                      lat.x, lat.y, lat.z, roots[0].x, roots[0].y, roots[0].z, roots[1].x, roots[1].y, roots[1].z, tips[0].x, tips[0].y, tips[0].z, tips[1].x, tips[1].y, tips[1].z, pc.center.x, pc.center.y, pc.center.z);
        log.push_back(d);
    }
    const double span = double(t0.x) - t1.x;
    if (o.scale > 0) pl.s = o.scale;
    else if (span > 1e-6) pl.s = (double(T.tip[0].x) - T.tip[1].x) / span;
    else { err = "the strings' outer ends are not on opposite sides"; return false; }
    const Vec3 th = (T.hinge[0] + T.hinge[1]) * 0.5f;
    const float wh = static_cast<float>(o.hingeWeight), wt = 1.0f;
    pl.t = ((th - hq * static_cast<float>(pl.s)) * wh + (T.tip[0] - t0 * static_cast<float>(pl.s)) * wt + (T.tip[1] - t1 * static_cast<float>(pl.s)) * wt) * (1.0f / (wh + 2 * wt));
    // Height. The limbs and strings swing about the vertical axis (every crossbow shooting clip), so a pivot that is
    // off in height moves nothing; what shows is the stock's height in the hand. So the height comes from the grip: the
    // stretch of stock at the hand's position sits at the height the reference stock has there.
    char b[320];
    if (o.gripHeight && T.haveGrip) {
        std::vector<TriPos> tris;
        const Piece& st = roles["main"];
        for (const Tri& t : st.tris) tris.push_back({pl.Apply(st.pos[static_cast<size_t>(t[0].v)]), pl.Apply(st.pos[static_cast<size_t>(t[1].v)]), pl.Apply(st.pos[static_cast<size_t>(t[2].v)])});
        double zc = 0;
        if (SliceCentreZ(tris, T.hand.y, T.gripBand, zc)) {
            pl.t.z += static_cast<float>(T.refGripZ - zc);
            pl.gripShift = std::fabs(T.refGripZ - zc);
            std::snprintf(b, sizeof b, "fit: height from the grip: the stock at the hand is %.3f high here, the reference's is %.3f: moved by %.3f", zc, T.refGripZ, T.refGripZ - zc);
            log.push_back(b);
        } else log.push_back("warning: the stock does not reach the hand's position: the height was not set from the grip");
    }
    pl.t = pl.t + o.shift;
    if (o.prodAtHinge) {
        const double rootZ = (double(pl.Apply(roots[0]).z) + pl.Apply(roots[1]).z) / 2, hingeZ = (double(T.hinge[0].z) + T.hinge[1].z) / 2;
        pl.prodDrop = rootZ - hingeZ;
        std::snprintf(b, sizeof b, "fit: the limbs and strings are lowered by %.3f so the limb roots sit at the hinge's height (some animations turn a limb over about the hinge)", pl.prodDrop);
        log.push_back(b);
    }
    std::snprintf(b, sizeof b, "fit: model forward (%.3f %.3f %.3f) up (%.3f %.3f %.3f) -> the rig's forward (0 %.3f %.3f); scale %.4f%s", fwd.x, fwd.y, fwd.z, up.x, up.y, up.z, tf.y, tf.z, pl.s, o.scale > 0 ? " (given)" : " (string ends on the rig's anchors)");
    log.push_back(b);
    const Vec3 got[3] = {pl.Apply(hinge), pl.Apply(tips[0]), pl.Apply(tips[1])}, want[3] = {th, T.tip[0], T.tip[1]};
    const char* what[3] = {"hinge", "string end +X", "string end -X"};
    pl.heightError = (std::fabs(double(got[1].z) - want[1].z) + std::fabs(double(got[2].z) - want[2].z)) / 2;
    for (int i = 0; i < 3; ++i) {
        const Vec3 d = got[i] - want[i];
        std::snprintf(b, sizeof b, "fit: %s is off the rig's by %.3f across (x %.3f, y %.3f: these show) and %.3f in height (the swing is about the vertical axis: it does not)", what[i], std::sqrt(double(d.x) * d.x + double(d.y) * d.y), d.x, d.y, d.z);
        log.push_back(b);
    }
    return true;
}

// The fit with the model's up chosen: a model's own "up" is whatever its author had (cb4 stands on its side in Blender), so both turns of 180 degrees about the stock
// are tried and the one that puts the string ends and the hinge at the rig's height wins (a model turned the wrong way is 10-20 cm off in height).
inline bool FitCrossbow(RoleMap& roles, const FitTargets& T, const FitOptions& o, Placement& pl, std::vector<std::string>& log, std::string& err) {
    if (!o.upAuto || !o.autoFit) return FitCrossbowOnce(roles, T, o, pl, log, err);
    RoleMap rolesA = roles, rolesB = roles;
    Placement plA, plB;
    std::vector<std::string> logA, logB;
    FitOptions a = o, b = o;
    a.upIsZ = true; b.upIsZ = false;
    const bool okA = FitCrossbowOnce(rolesA, T, a, plA, logA, err), okB = FitCrossbowOnce(rolesB, T, b, plB, logB, err);
    if (!okA && !okB) return false;
    const double costA = plA.gripShift, costB = plB.gripShift;   // the stock's height in the hand decides (the strings' height is a design choice: crossbow 3's are 13 cm up)
    const bool pickA = okA && (!okB || costA <= costB + (o.upIsZ ? 0.03 : -0.03));   // a near tie keeps the model's +Z (or what was asked for)
    char t[300];
    std::snprintf(t, sizeof t, "up: with the model's +Z up the stock sits %.3f off its place in the hand, turned over %.3f: %s", okA ? costA : -1.0, okB ? costB : -1.0, pickA ? "its +Z is the crossbow's up" : "its -Z is the crossbow's up (turned over about the stock)");
    log.push_back(t);
    for (const std::string& l : pickA ? logA : logB) log.push_back(l);
    roles = pickA ? rolesA : rolesB;
    pl = pickA ? plA : plB;
    return true;
}

// ---- bows ---------------------------------------------------------------------------------------------------------------
// The four pieces of a bow: the limb halves (partb = the +Y half under the hand, parta) and a string half on each (tetivab, tetivaa).
// By the objects' names when all four are there, else by shape (the two thinnest loose pieces are the strings); which side is which is the fit's.
inline bool FindBowRoles(const ObjFile& f, RoleMap& roles, std::vector<std::string>& log, std::string& err) {
    roles.clear();
    static const std::vector<std::pair<const char*, std::vector<const char*>>> tokens = {
        {"partb", {"partb", "limbb", "limb_b", "limb1", "limb_1"}}, {"parta", {"parta", "limba", "limb_a", "limb2", "limb_2"}},
        {"tetivab", {"tetivab", "stringb", "string_b", "string1", "string_1"}}, {"tetivaa", {"tetivaa", "stringa", "string_a", "string2", "string_2"}}};
    std::map<std::string, size_t> byName;
    bool named = true;
    for (const auto& tk : tokens) {
        int found = 0;
        for (size_t i = 0; i < f.objects.size(); ++i) {
            std::string n = f.objects[i].name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (NameMatches(n, tk.second)) { byName[tk.first] = i; ++found; }
        }
        if (found != 1) { named = false; break; }
    }
    if (named) {
        for (const auto& kv : byName) {
            Piece p;
            if (!MakePiece(f, f.objects[kv.second].tris, f.objects[kv.second].name, p, err)) return false;
            p.role = kv.first;
            roles[kv.first] = std::move(p);
        }
        log.push_back("pieces: taken from the object names (partb / parta, tetivab / tetivaa; or limb1 / limb2, string1 / string2)");
        return true;
    }
    std::vector<Piece> islands;
    for (const auto& o : f.objects)
        for (const auto& isl : SplitIslands(f, o.tris)) {
            Piece p;
            if (!MakePiece(f, isl, o.name, p, err)) return false;
            islands.push_back(std::move(p));
        }
    if (islands.size() != 4) {
        err = "a bow needs 4 pieces (2 limb halves, 2 string halves): the model has " + std::to_string(islands.size()) +
              " loose pieces and its objects are not named partb / parta / tetivab / tetivaa (or limb1 / limb2 / string1 / string2)";
        return false;
    }
    struct Info { size_t idx; double thin; };
    std::vector<Info> info;
    for (size_t i = 0; i < islands.size(); ++i) {
        const Pca pc = ComputePca(UsedPositions(islands[i]));
        info.push_back({i, (pc.extent[1] + pc.extent[2]) / std::max(pc.extent[0], 1e-9)});
    }
    std::sort(info.begin(), info.end(), [](const Info& a, const Info& b) { return a.thin > b.thin; });   // limbs are fatter than strings
    roles["partb"] = islands[info[0].idx]; roles["parta"] = islands[info[1].idx];
    roles["tetivab"] = islands[info[2].idx]; roles["tetivaa"] = islands[info[3].idx];
    log.push_back("pieces: told apart by their shape (the two thinnest are the strings); the sides are decided by the fit");
    return true;
}

// Places a bow: the string's outer ends (where it is tied to the limbs' tips) on the rig's anchors, the string on the -X side, up = +Z (the model's own).
// anchor[0] = the tetivaa side (-Y), anchor[1] = tetivab (+Y), in the frame of lh3.bwpartb00, corner 5.
inline bool FitBow(RoleMap& roles, const Vec3 anchor[2], const FitOptions& o, Placement& pl, std::vector<std::string>& log, std::string& err) {
    pl = Placement{};
    if (!o.autoFit) { pl.t = o.shift; if (o.scale > 0) pl.s = o.scale; log.push_back("fit: none (the model is taken as it is in the rig's frame)"); return true; }
    std::vector<Vec3> limbs = UsedPositions(roles["partb"]);
    { const auto q = UsedPositions(roles["parta"]); limbs.insert(limbs.end(), q.begin(), q.end()); }
    std::vector<Vec3> strs = UsedPositions(roles["tetivab"]);
    { const auto q = UsedPositions(roles["tetivaa"]); strs.insert(strs.end(), q.begin(), q.end()); }
    const Pca pc = ComputePca(limbs);
    Vec3 a = pc.axis[0];   // the limbs' length: tip to tip
    double half = 0;
    for (const Vec3& p : limbs) half = std::max(half, std::fabs(Dot(a, p - pc.center)));
    std::vector<Vec3> midPts;
    for (const Vec3& p : limbs) if (std::fabs(Dot(a, p - pc.center)) < 0.4 * half) midPts.push_back(p);
    const Vec3 mid = midPts.empty() ? pc.center : Centroid(midPts);
    Vec3 v = Centroid(strs) - mid;
    v = v - a * static_cast<float>(Dot(v, a));
    if (Len(v) < 1e-6) { err = "the strings lie on the limbs' middle line: cannot tell which side the string is on"; return false; }
    const Vec3 xm = Norm(v) * -1.0f;   // the rig's string is on its -X side
    const Vec3 z0{0, 0, o.upIsZ ? 1.0f : -1.0f};
    Vec3 zs = Norm(Cross(xm, a));
    // A bow modelled standing up (its length along the model's vertical) has its upper limb for partb; one lying flat (the rig's own frame, what an
    // exported reference is) keeps the model's up for the rig's up.
    const bool standing = std::fabs(Dot(a, z0)) > 0.7;
    if (standing) { if (Dot(a, z0) < 0) { a = a * -1.0f; zs = zs * -1.0f; } }
    else if (Dot(zs, z0) < 0) { a = a * -1.0f; zs = zs * -1.0f; }
    log.push_back(standing ? "note: the bow stands up in the model: its upper limb is partb" : "note: the bow lies flat in the model: its up is the rig's up");
    if (!standing && std::fabs(zs.z) < 0.5) log.push_back("warning: the model's up is far from +Z (" + std::to_string(zs.z) + "): check the preview, or turn the model in Blender");
    const Vec3 tb[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, mb[3] = {xm, a, zs};   // the model's string side / length / up onto the rig's X / Y / Z
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double r = 0;
        for (int k = 0; k < 3; ++k) r += double(i == 0 ? tb[k].x : i == 1 ? tb[k].y : tb[k].z) * double(j == 0 ? mb[k].x : j == 1 ? mb[k].y : mb[k].z);
        pl.R[i][j] = r;
    }
    auto sideOf = [&](const Piece& p) { return pl.Dir(Centroid(UsedPositions(p))).y; };
    if (sideOf(roles["partb"]) < sideOf(roles["parta"])) { std::swap(roles["partb"], roles["parta"]); pl.swappedLimbs = true; log.push_back("note: the limb halves were on the other sides: partb / parta swapped"); }
    if (sideOf(roles["tetivab"]) < sideOf(roles["tetivaa"])) { std::swap(roles["tetivab"], roles["tetivaa"]); pl.swappedStrings = true; log.push_back("note: the string halves were on the other sides: tetivab / tetivaa swapped"); }
    // each string's outer end: the end farthest from the middle of the bow
    Vec3 tips[2];
    for (int i = 0; i < 2; ++i) {
        const std::vector<Vec3> pts = UsedPositions(roles[i == 0 ? "tetivaa" : "tetivab"]);
        const double sign = i == 0 ? -1.0 : 1.0;   // tetivaa hangs at -Y, tetivab at +Y
        double mx = -1e30, mn = 1e30;
        for (const Vec3& p : pts) { const double y = sign * pl.Dir(p).y; mx = std::max(mx, y); mn = std::min(mn, y); }
        std::vector<Vec3> end;
        for (const Vec3& p : pts) if (sign * pl.Dir(p).y >= mx - 0.15 * (mx - mn)) end.push_back(p);
        tips[i] = Centroid(end);
    }
    const double span = Len(pl.Dir(tips[1]) - pl.Dir(tips[0]));
    if (o.scale > 0) pl.s = o.scale;
    else if (span > 1e-6) pl.s = Len(anchor[1] - anchor[0]) / span;
    else { err = "the strings' outer ends are on the same spot"; return false; }
    pl.t = ((anchor[0] - pl.Dir(tips[0]) * static_cast<float>(pl.s)) + (anchor[1] - pl.Dir(tips[1]) * static_cast<float>(pl.s))) * 0.5f;
    char b[420];
    {   // The depth (string to grip). The limbs turn about the grip's middle (the rig's pivot) and the strings about their anchors, 0.113 further out in X:
        // every vanilla bow has its two limb halves joined on the pivot (within 2.5 cm) AND its string ends on the anchors. A model with another depth cannot
        // have both, and the draw clips then pull the limbs apart at the grip or the strings off the limb tips; so the limbs are bent (the tips moved in X,
        // the grip kept) until both hold.
        std::vector<Vec3> join;
        for (const char* role : {"partb", "parta"})
            for (const Vec3& q : UsedPositions(roles[role])) { const Vec3 w = pl.Apply(q); if (std::fabs(w.y) < 0.05) join.push_back(w); }
        double xs = 0;
        for (int i = 0; i < 2; ++i) xs += pl.Apply(tips[i]).x * 0.5;
        const double xa = (double(anchor[0].x) + anchor[1].x) * 0.5;
        if (!join.empty()) {
            const Vec3 jc = Centroid(join);
            const double depthModel = std::fabs(xs - jc.x), depthRig = std::fabs(xa);   // the rig's join is at x = 0
            if (!o.keepDepth) {
                pl.t.x -= jc.x;   // the grip on the pivot
                pl.bend = xa - (xs - jc.x);
                pl.bendLength = std::max(1e-3, (std::fabs(double(anchor[0].y)) + std::fabs(double(anchor[1].y))) * 0.5);
                std::snprintf(b, sizeof b, "fit: depth (string to grip) %.3f in the model, %.3f in the rig: the limbs are bent by %.3f at the tips so the grip sits on the limbs' pivot and the string on its anchors (keep depth: off)", depthModel, depthRig, pl.bend);
            } else std::snprintf(b, sizeof b, "fit: depth (string to grip) %.3f in the model, %.3f in the rig, kept: the grip is %.3f off the limbs' pivot (the draw clips will pull the limb halves apart by about that much)", depthModel, depthRig, std::fabs(jc.x));
            log.push_back(b);
            std::snprintf(b, sizeof b, "fit: the grip's middle is %.3f along Y and %.3f along Z from the pivot", jc.y, jc.z);
            log.push_back(b);
        }
    }
    pl.t = pl.t + o.shift;
    std::snprintf(b, sizeof b, "fit: scale %.4f%s; the model's string side -> the rig's -X, its length -> Y, up -> Z", pl.s, o.scale > 0 ? " (given)" : " (string ends on the rig's anchors)");
    log.push_back(b);
    const Vec3 got[2] = {pl.Apply(tips[0]) + Vec3{static_cast<float>(pl.bend), 0, 0}, pl.Apply(tips[1]) + Vec3{static_cast<float>(pl.bend), 0, 0}};
    for (int i = 0; i < 2; ++i) {
        const Vec3 d = got[i] - anchor[i];
        std::snprintf(b, sizeof b, "fit: string end %s is off the rig's anchor by %.3f (x %.3f, y %.3f, z %.3f)", i == 0 ? "-Y" : "+Y", Len(d), d.x, d.y, d.z);
        log.push_back(b);
    }
    // where the limb halves meet and how far apart the strings' inner ends are (both belong on the bow's middle)
    auto innerEnd = [&](const Piece& p, double sign) {
        const std::vector<Vec3> pts = UsedPositions(p);
        double lo = 1e30, hi = -1e30;
        for (const Vec3& q : pts) { const double y = sign * pl.Apply(q).y; lo = std::min(lo, y); hi = std::max(hi, y); }
        std::vector<Vec3> end;
        for (const Vec3& q : pts) if (sign * pl.Apply(q).y <= lo + 0.03 * (hi - lo)) end.push_back(pl.Apply(q));
        return Centroid(end);
    };
    const Vec3 limbGap = innerEnd(roles["partb"], 1.0) - innerEnd(roles["parta"], -1.0), nock = innerEnd(roles["tetivab"], 1.0) - innerEnd(roles["tetivaa"], -1.0);
    std::snprintf(b, sizeof b, "fit: the limb halves' inner ends are %.3f apart, the strings' inner ends %.3f apart (about 0 when the bow is cut in the middle)", Len(limbGap), Len(nock));
    log.push_back(b);
    return true;
}

// ---- melee weapons ----------------------------------------------------------------------------------------------------
// One piece: every object of the model (a guard, a pommel, a blade modelled apart) is one weapon.
inline bool FindMeleeRoles(const ObjFile& f, RoleMap& roles, std::vector<std::string>& log, std::string& err) {
    roles.clear();
    std::vector<Tri> all;
    for (const auto& o : f.objects) all.insert(all.end(), o.tris.begin(), o.tris.end());
    if (all.empty()) { err = "the model has no faces"; return false; }
    Piece p;
    if (!MakePiece(f, all, "weapon", p, err)) return false;
    p.role = "main";
    roles["main"] = std::move(p);
    log.push_back("piece: the whole model (" + std::to_string(f.objects.size()) + " object" + (f.objects.size() == 1 ? "" : "s") + ") is one weapon");
    return true;
}

// The cross-section of a piece cut by a plane square to `axis` at distance d along it from `origin`: its middle and its size (the diagonal of the cut
// points' extent in the plane). False when the plane misses the piece.
inline bool CutSection(const Piece& w, const Vec3& axis, const Vec3& origin, double d, Vec3& centre, double& size, const Vec3& hint) {
    // the plane's own axes: `hint` (the shape's second principal axis) made square to the axis: the extent does not depend on how the model is turned
    const Vec3 u = Norm(hint - axis * static_cast<float>(Dot(hint, axis))), v = Cross(axis, u);
    double ulo = 1e30, uhi = -1e30, vlo = 1e30, vhi = -1e30;
    bool any = false;
    for (const Tri& t : w.tris)
        for (int e = 0; e < 3; ++e) {
            const Vec3 a = w.pos[static_cast<size_t>(t[static_cast<size_t>(e)].v)], b = w.pos[static_cast<size_t>(t[static_cast<size_t>((e + 1) % 3)].v)];
            const double da = Dot(axis, a - origin) - d, db = Dot(axis, b - origin) - d;
            if (da * db < 0) {
                const Vec3 q = Mix(a, b, da / (da - db));
                const double qu = Dot(u, q), qv = Dot(v, q);
                ulo = std::min(ulo, qu); uhi = std::max(uhi, qu); vlo = std::min(vlo, qv); vhi = std::max(vhi, qv);
                any = true;
            }
        }
    if (!any) return false;
    centre = axis * static_cast<float>(Dot(axis, origin) + d) + u * static_cast<float>((ulo + uhi) / 2) + v * static_cast<float>((vlo + vhi) / 2);
    size = std::sqrt((uhi - ulo) * (uhi - ulo) + (vhi - vlo) * (vhi - vlo));
    return true;
}
inline bool CutCentre(const Piece& w, const Vec3& axis, const Vec3& origin, double d, Vec3& out, const Vec3& hint) { double s; return CutSection(w, axis, origin, d, out, s, hint); }

// The direction (in the model's frame) from the butt toward the tip, along the long axis. Which end is the tip is not in the shape alone: a sword's
// guard is as heavy as its point, a spear's pole is as long as its shaft. Two rules, each checked on every vanilla weapon of its kinds:
// a blade (sword, dagger) ends in a point, the handle end is blunt: the end with the smaller cross-section 4% in from it is the tip;
// a head (axe, club, spear): the cross-sections' weight (their sizes along the axis, like a mass) lies toward the head.
inline Vec3 MeleeTipDirection(const Piece& w, const Pca& pc, const Kind& kind, bool flip) {
    const Vec3 a = pc.axis[0];
    double lo = 1e30, hi = -1e30;
    for (const Vec3& p : UsedPositions(w)) { const double d = Dot(a, p - pc.center); lo = std::min(lo, d); hi = std::max(hi, d); }
    const double len = hi - lo;
    bool tipAtLo;   // the tip is at the low end of the axis
    if (kind.tipByPoint) {
        Vec3 c; double sLo = 0, sHi = 0;
        if (!CutSection(w, a, pc.center, lo + 0.04 * len, c, sLo, pc.axis[1])) sLo = 0;
        if (!CutSection(w, a, pc.center, hi - 0.04 * len, c, sHi, pc.axis[1])) sHi = 0;
        tipAtLo = sLo < sHi;
    } else {
        double S = 0, Sd = 0;
        const int B = 40;
        for (int i = 0; i < B; ++i) {
            const double d = lo + len * (i + 0.5) / B;
            Vec3 c; double sz = 0;
            if (CutSection(w, a, pc.center, d, c, sz, pc.axis[1])) { S += sz; Sd += sz * d; }
        }
        tipAtLo = S > 0 && Sd / S < (lo + hi) / 2;
    }
    Vec3 tip = tipAtLo ? a * -1.0f : a;
    return flip ? tip * -1.0f : tip;
}

// The handle's centre line: through the middles of the cross-sections at a few stations along the butt-side quarter of the weapon (a curved blade, a heavy
// head do not tilt it the way the whole shape's long axis is tilted). Pointing toward the tip; falls back to `tip` when the stations are not cut.
inline Vec3 MeleeHandleDirection(const Piece& w, const Pca& pc, const Vec3& tip) {
    double lo = 1e30, hi = -1e30;
    for (const Vec3& p : UsedPositions(w)) { const double d = Dot(tip, p - pc.center); lo = std::min(lo, d); hi = std::max(hi, d); }
    const double len = hi - lo;
    std::vector<Vec3> centres;
    for (int k = 0; k < 7; ++k) {
        Vec3 c;
        if (CutCentre(w, tip, pc.center, lo + len * (0.04 + 0.03 * k), c, pc.axis[1])) centres.push_back(c);
    }
    if (centres.size() < 3) return tip;
    const Pca line = ComputePca(centres);
    Vec3 d = line.axis[0];
    if (Dot(d, tip) < 0) d = d * -1.0f;
    return d;
}

// The centre of the weapon's cross-section at height y (the rig's Y; triangles cut by planes around it, the middle of the cut points' x and z).
inline bool SliceCentreXZ(const std::vector<TriPos>& tris, double y0, double halfBand, double& cx, double& cz) {
    double sx = 0, sz = 0;
    int n = 0;
    for (int k = -2; k <= 2; ++k) {
        const double y = y0 + k * 0.5 * halfBand;
        double lox = 1e30, hix = -1e30, loz = 1e30, hiz = -1e30;
        for (const TriPos& t : tris)
            for (int e = 0; e < 3; ++e) {
                const Vec3 &a = t[static_cast<size_t>(e)], &b = t[static_cast<size_t>((e + 1) % 3)];
                if ((a.y - y) * (b.y - y) < 0) {
                    const double u = (y - a.y) / (double(b.y) - a.y), x = a.x + u * (double(b.x) - a.x), z = a.z + u * (double(b.z) - a.z);
                    lox = std::min(lox, x); hix = std::max(hix, x); loz = std::min(loz, z); hiz = std::max(hiz, z);
                }
            }
        if (lox <= hix) { sx += (lox + hix) / 2; sz += (loz + hiz) / 2; ++n; }
    }
    if (!n) return false;
    cx = sx / n; cz = sz / n;
    return true;
}

// Places a melee weapon: its tip toward -Y (end for end with `flip`), its flat sides up (the thinnest direction -> Z; a round one keeps the model's own up),
// an asymmetric head (an axe's) to -X, the hand at its class's distance from the butt with the handle's centre line through the hand.
inline bool FitMelee(RoleMap& roles, const Kind& kind, const Kind::Grip& hand, double raceFactor, const FitOptions& o, Placement& pl, std::vector<std::string>& log, std::string& err) {
    pl = Placement{};
    if (!o.autoFit) { pl.t = o.shift; if (o.scale > 0) pl.s = o.scale; log.push_back("fit: none (the model is taken as it is in the rig's frame)"); return true; }
    const Piece& w = roles["main"];
    const Pca pc = ComputePca(UsedPositions(w));
    if (pc.extent[0] < 1e-6) { err = "the weapon has no length"; return false; }
    const Vec3 tip0 = MeleeTipDirection(w, pc, kind, o.flip);
    const Vec3 tip = MeleeHandleDirection(w, pc, tip0);   // the handle's own direction: the line the hand holds
    const Vec3 z0{0, 0, o.upIsZ ? 1.0f : -1.0f};
    Vec3 up;
    const double e1 = pc.extent[1], e2 = pc.extent[2];
    const bool roundish = std::min(e1, e2) > 0.8 * std::max(e1, e2);
    if (roundish) up = z0;
    else { up = e1 < e2 ? pc.axis[1] : pc.axis[2]; if (Dot(up, z0) < 0) up = up * -1.0f; }
    up = up - tip * static_cast<float>(Dot(up, tip));
    if (Len(up) < 0.3) { up = z0 - tip * static_cast<float>(Dot(z0, tip)); if (Len(up) < 0.3) up = Vec3{0, 1, 0} - tip * tip.y; }
    up = Norm(up);
    const Vec3 ym = tip * -1.0f;   // the rig's +Y = toward the shooter / the butt
    Vec3 xm = Cross(ym, up);
    // an asymmetric head to -X: the tip's side of the weapon against the butt's side
    double headSide = 0;
    if (kind.headMinusX) {
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : UsedPositions(w)) { const double d = Dot(tip, p - pc.center); lo = std::min(lo, d); hi = std::max(hi, d); }
        double wh = 0, wb = 0, mh = 0, mbt = 0;
        for (const Tri& t : w.tris) {
            const Vec3 v0 = w.pos[static_cast<size_t>(t[0].v)], v1 = w.pos[static_cast<size_t>(t[1].v)], v2 = w.pos[static_cast<size_t>(t[2].v)];
            const Vec3 c = (v0 + v1 + v2) * (1.0f / 3.0f);
            const double d = Dot(tip, c - pc.center), ar = 0.5 * Len(Cross(v1 - v0, v2 - v0)), x = Dot(xm, c);
            if (d > lo + 0.55 * (hi - lo)) { wh += ar; mh += ar * x; }
            if (d < lo + 0.25 * (hi - lo)) { wb += ar; mbt += ar * x; }
        }
        if (wh > 0 && wb > 0) headSide = mh / wh - mbt / wb;
        if (headSide > 0.02) { xm = xm * -1.0f; up = up * -1.0f; log.push_back("note: the head was on the +X side: the weapon is turned half a turn about its length so it points to -X like the vanilla ones"); }
    }
    if (std::fabs(o.roll) > 1e-6) {
        const double r = o.roll * 3.14159265358979 / 180.0;
        const Vec3 x2 = xm * static_cast<float>(std::cos(r)) + up * static_cast<float>(std::sin(r)), z2 = up * static_cast<float>(std::cos(r)) - xm * static_cast<float>(std::sin(r));
        xm = x2; up = z2;
    }
    const double tilt = hand.tilt * 3.14159265358979 / 180.0;
    const Vec3 Xt{1, 0, 0}, Yt{0, static_cast<float>(std::cos(tilt)), static_cast<float>(-std::sin(tilt))}, Zt = Cross(Xt, Yt);
    const Vec3 tb[3] = {Xt, Yt, Zt}, mb[3] = {xm, ym, up};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double r = 0;
        for (int k = 0; k < 3; ++k) r += double(i == 0 ? tb[k].x : i == 1 ? tb[k].y : tb[k].z) * double(j == 0 ? mb[k].x : j == 1 ? mb[k].y : mb[k].z);
        pl.R[i][j] = r;
    }
    pl.s = (o.scale > 0 ? o.scale : 1.0) * raceFactor;
    double buttY = -1e30, tipY = 1e30;
    for (const Vec3& p : UsedPositions(w)) { const double y = pl.Apply(p).y; buttY = std::max(buttY, y); tipY = std::min(tipY, y); }
    const double length = buttY - tipY;
    const double baseGrip = o.grip > 0 ? o.grip : kind.gripFromButt;
    const double fromButt = baseGrip > 0 ? baseGrip * raceFactor : kind.gripFraction * length;
    const double gripY = buttY - fromButt;
    std::vector<TriPos> placed;
    for (const Tri& t : w.tris) placed.push_back({pl.Apply(w.pos[static_cast<size_t>(t[0].v)]), pl.Apply(w.pos[static_cast<size_t>(t[1].v)]), pl.Apply(w.pos[static_cast<size_t>(t[2].v)])});
    double cx = 0, cz = 0;
    char b[360];
    if (!SliceCentreXZ(placed, gripY, 0.03, cx, cz)) {
        std::vector<Vec3> closeBy;
        for (const Vec3& p : UsedPositions(w)) { const Vec3 q = pl.Apply(p); if (std::fabs(q.y - gripY) < 0.1) closeBy.push_back(q); }
        if (closeBy.empty()) { err = "the weapon does not reach the place where the hand holds it"; return false; }
        const Vec3 c = Centroid(closeBy); cx = c.x; cz = c.z;
        log.push_back("note: the handle has no faces at the hand: its centre line was taken from the vertices around it");
    }
    pl.t = Vec3{static_cast<float>(hand.x - cx), static_cast<float>(-gripY), static_cast<float>(hand.z - cz)} + o.shift;
    std::snprintf(b, sizeof b, "fit: tip -> -Y, length %.3f (scale %.4f = %s x %.4f for this rig), the hand holds it %.3f from the butt (%s), the handle's centre line through the hand",
                  length, pl.s, o.scale > 0 ? "the one given" : "1, the model's own size", raceFactor, fromButt, o.grip > 0 ? "given" : "the vanilla rule for this kind");
    log.push_back(b);
    if (length < 0.15 || length > 3.2) log.push_back("warning: a vanilla " + std::string(kind.id) + " is between 0.3 and 2.9 long: check the scale of the model (Scale option)");
    return true;
}

// ---- normals ------------------------------------------------------------------------------------------------
// Keep: the model's own normals (a flat-shaded face stays hard-edged: a vertex repeated once per direction). Smooth: one
// normal per position, the angle-weighted mean of its faces' (what ei_figer writes: Blender's vertex normal).
enum class NormalMode { Keep, Smooth };
inline void SmoothNormals(Piece& p) {
    std::map<std::array<int64_t, 3>, int> idOf;
    std::vector<Vec3> acc;
    auto id = [&](const Vec3& v) {
        const std::array<int64_t, 3> k = {std::llround(v.x * 1e5), std::llround(v.y * 1e5), std::llround(v.z * 1e5)};
        auto it = idOf.find(k);
        if (it == idOf.end()) { it = idOf.emplace(k, static_cast<int>(acc.size())).first; acc.push_back({}); }
        return it->second;
    };
    std::vector<std::array<int, 3>> ids;
    for (const Tri& t : p.tris) {
        std::array<int, 3> a;
        for (int i = 0; i < 3; ++i) a[static_cast<size_t>(i)] = id(p.pos[static_cast<size_t>(t[i].v)]);
        ids.push_back(a);
        const Vec3 v0 = p.pos[static_cast<size_t>(t[0].v)], v1 = p.pos[static_cast<size_t>(t[1].v)], v2 = p.pos[static_cast<size_t>(t[2].v)];
        const Vec3 fn = Norm(Cross(v1 - v0, v2 - v0));
        const Vec3 vs[3] = {v0, v1, v2};
        for (int i = 0; i < 3; ++i) {
            const Vec3 e1 = Norm(vs[(i + 1) % 3] - vs[i]), e2 = Norm(vs[(i + 2) % 3] - vs[i]);
            const double ang = std::acos(std::max(-1.0, std::min(1.0, Dot(e1, e2))));
            acc[static_cast<size_t>(a[static_cast<size_t>(i)])] = acc[static_cast<size_t>(a[static_cast<size_t>(i)])] + fn * static_cast<float>(ang);
        }
    }
    p.nrm.clear();
    for (const Vec3& n : acc) p.nrm.push_back(Norm(n));
    for (size_t i = 0; i < p.tris.size(); ++i) for (int k = 0; k < 3; ++k) p.tris[i][static_cast<size_t>(k)].n = ids[i][static_cast<size_t>(k)];
}

// ---- UVs --------------------------------------------------------------------------------------------------
// A hand-held part's UVs are direct texture coordinates: u across the image, v DOWN from its top row (measured on the vanilla
// crossbows through the viewer, 2026-10-22: 97% of the sampled texels). Raw = the model's UVs go to the file as they are.
// Texture / atlas: the UVs of a GROUND or inventory model, which the game maps through a 256 x 256 atlas (a texture W
// wide covers a W/256 square in its corner; what ei_figer converts for `init...` figures): 0..1 over the texture,
// mapped into the atlas. Only for models that really are such a ground model.
enum class UvMode { Raw, Texture };
inline Vec2 MapUv(const Vec2& uv, UvMode mode, int textureWidth) {
    if (mode == UvMode::Raw) return uv;
    const float s = static_cast<float>(textureWidth) / 256.0f;
    return {uv.x * s, (1.0f - s) + uv.y * s};
}

inline Vec2 UnmapUv(const Vec2& uv, UvMode mode, int textureWidth) {
    if (mode == UvMode::Raw) return uv;
    const float s = static_cast<float>(textureWidth) / 256.0f;
    return {uv.x / s, (uv.y - (1.0f - s)) / s};
}

// ---- building a part's FigureMesh ----------------------------------------------------------------------
// The piece (already in its part's frame, corner 5) as a FIG8 mesh: one vertex point per distinct (position, normal)
// (the vanilla files have exactly one normal per point), one component per distinct (point, uv), sorted by point
// then uv, the 8 corners = corner 5 times the corner scales. Padded to blocks of 4 with copies of the last point.
inline bool BuildMesh(const Piece& p, const CornerScales& K, int group, int textureNumber, fig::FigureMesh& m, std::string& err) {
    m = fig::FigureMesh{};
    auto q = [](float v, double step) { return static_cast<int64_t>(std::llround(static_cast<double>(v) / step)); };
    std::map<std::array<int64_t, 6>, int> pointOf;
    std::vector<Vec3> pts, nrms;
    std::map<std::array<int64_t, 2>, int> uvOf;
    std::vector<Vec2> uvs;
    struct Comp { int point, uv; };
    std::vector<std::array<Comp, 3>> tris;
    for (const Tri& t : p.tris) {
        std::array<Comp, 3> tc;
        for (int i = 0; i < 3; ++i) {
            const Vec3& v = p.pos[static_cast<size_t>(t[i].v)];
            const Vec3& n = p.nrm[static_cast<size_t>(t[i].n)];
            const std::array<int64_t, 6> key = {q(v.x, 1e-5), q(v.y, 1e-5), q(v.z, 1e-5), q(n.x, 2e-3), q(n.y, 2e-3), q(n.z, 2e-3)};
            auto it = pointOf.find(key);
            if (it == pointOf.end()) { it = pointOf.emplace(key, static_cast<int>(pts.size())).first; pts.push_back(v); nrms.push_back(n); }
            const Vec2& uv = p.uv[static_cast<size_t>(t[i].t)];
            const std::array<int64_t, 2> uk = {q(uv.x, 1e-6), q(uv.y, 1e-6)};
            auto ut = uvOf.find(uk);
            if (ut == uvOf.end()) { ut = uvOf.emplace(uk, static_cast<int>(uvs.size())).first; uvs.push_back(uv); }
            tc[static_cast<size_t>(i)] = {it->second, ut->second};
        }
        tris.push_back(tc);
    }
    std::set<std::pair<int, int>> compSet;
    for (const auto& t : tris) for (const Comp& c : t) compSet.insert({c.point, c.uv});
    std::vector<std::pair<int, int>> comps(compSet.begin(), compSet.end()); // sorted by point, then uv
    if (pts.size() > 0xFFFF || comps.size() > 0xFFFF || uvs.size() > 0xFFFF) { err = "the part is too big for the format (more than 65535 points / components)"; return false; }
    std::map<std::pair<int, int>, int> compIdx;
    for (size_t i = 0; i < comps.size(); ++i) compIdx[comps[i]] = static_cast<int>(i);
    m.group = group;
    m.textureNumber = textureNumber;
    m.uvs = uvs;
    for (const auto& c : comps) m.vertexComponents.push_back({static_cast<uint16_t>(c.first), static_cast<uint16_t>(c.first), static_cast<uint16_t>(c.second)});
    for (const auto& t : tris) for (const Comp& c : t) m.indices.push_back(static_cast<uint16_t>(compIdx[{c.point, c.uv}]));
    while (pts.size() % 4) { pts.push_back(pts.back()); nrms.push_back(nrms.back()); }
    m.normals = nrms;
    for (int c = 0; c < 8; ++c) {
        m.morphVertex[c].resize(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) m.morphVertex[c][i] = pts[i] * static_cast<float>(K.k[c]);
    }
    figw::RecomputeBounds(m);
    return true;
}


// ---- shape edits ------------------------------------------------------------------------------------------------
// The model's one mesh (the corner-5 shape every other corner and both races are derived from) can be reshaped before the
// corners are built: the edit then reaches every build and both races by itself. An edit thins or thickens a piece's cross-section
// (scales it about the piece's own centre line) over a stretch of its length, with soft ends. `piece`: "main" (the stock), "limbs"
// (both), "strings" (both). t runs along the piece: the stock 0 at the limbs .. 1 at the butt; a limb / string 0 at the stock's
// centre line .. 1 at its tip.
struct ShapeEdit { std::string piece = "main"; double t0 = 0.6, t1 = 0.85, factor = 0.8, soft = 0.06; };

inline double SmoothStep01(double x) { x = std::max(0.0, std::min(1.0, x)); return x * x * (3 - 2 * x); }

// Recomputes a piece's normals after its positions changed: a corner whose normal was its face's stays flat (the new face normal),
// the others become the angle-weighted mean of the faces at their position.
inline void RecomputeNormals(Piece& p, const std::vector<Vec3>& oldPos) {
    std::map<std::array<int64_t, 3>, Vec3> acc;
    auto key = [](const Vec3& v) { return std::array<int64_t, 3>{std::llround(v.x * 1e5), std::llround(v.y * 1e5), std::llround(v.z * 1e5)}; };
    for (const Tri& t : p.tris) {
        const Vec3 v[3] = {p.pos[static_cast<size_t>(t[0].v)], p.pos[static_cast<size_t>(t[1].v)], p.pos[static_cast<size_t>(t[2].v)]};
        const Vec3 fn = Norm(Cross(v[1] - v[0], v[2] - v[0]));
        for (int i = 0; i < 3; ++i) {
            const double ang = std::acos(std::max(-1.0, std::min(1.0, Dot(Norm(v[(i + 1) % 3] - v[i]), Norm(v[(i + 2) % 3] - v[i])))));
            acc[key(v[i])] = acc[key(v[i])] + fn * static_cast<float>(ang);
        }
    }
    std::vector<Vec3> nrm;
    std::map<std::pair<int, int>, int> made;   // (position index, kind) -> normal index; kind 0 = smooth, otherwise flat per triangle
    for (size_t ti = 0; ti < p.tris.size(); ++ti) {
        Tri& t = p.tris[ti];
        const Vec3 ov[3] = {oldPos[static_cast<size_t>(t[0].v)], oldPos[static_cast<size_t>(t[1].v)], oldPos[static_cast<size_t>(t[2].v)]};
        const Vec3 nv[3] = {p.pos[static_cast<size_t>(t[0].v)], p.pos[static_cast<size_t>(t[1].v)], p.pos[static_cast<size_t>(t[2].v)]};
        const Vec3 ofn = Norm(Cross(ov[1] - ov[0], ov[2] - ov[0])), nfn = Norm(Cross(nv[1] - nv[0], nv[2] - nv[0]));
        for (int i = 0; i < 3; ++i) {
            const Vec3 on = p.nrm[static_cast<size_t>(t[i].n)];
            const bool flat = Dot(on, ofn) > 0.98;
            nrm.push_back(flat ? nfn : Norm(acc[key(nv[i])]));
            t[i].n = static_cast<int>(nrm.size()) - 1;
        }
    }
    p.nrm = nrm;
}

// The roles an edit's piece name stands for in a kind ("main" = the whole weapon / the stock, "limbs", "strings").
inline std::vector<std::string> EditTargets(const Kind& kind, const std::string& piece) {
    if (piece == "main") return kind.family == Family::Bow ? std::vector<std::string>{} : std::vector<std::string>{"main"};
    if (piece == "limbs") return kind.family == Family::Crossbow ? std::vector<std::string>{"part01", "part02"} : kind.family == Family::Bow ? std::vector<std::string>{"partb", "parta"} : std::vector<std::string>{};
    if (piece == "strings") return kind.family == Family::Crossbow ? std::vector<std::string>{"tetiva01", "tetiva02"} : kind.family == Family::Bow ? std::vector<std::string>{"tetivab", "tetivaa"} : std::vector<std::string>{};
    return {};
}

// The piece's own axis and where t = 0 is, from the whole model (roles are the pieces of the kind). A stock / a melee weapon: t 0 at the limbs / the tip,
// 1 at the butt; a limb / string: 0 at the middle, 1 at the tip.
struct PieceAxis { Vec3 a, origin, centre; double lo = 0, hi = 1; bool stock = false; Vec3 lat; };
inline bool ComputePieceAxis(const RoleMap& roles, const Kind& kind, const std::string& role, bool flip, PieceAxis& out) {
    auto it = roles.find(role);
    if (it == roles.end()) return false;
    const std::vector<Vec3> pts = UsedPositions(it->second);
    if (pts.size() < 3) return false;
    const Pca pc = ComputePca(pts);
    out.stock = role == "main";
    out.centre = pc.center;
    if (kind.family == Family::Melee) {
        Vec3 a = MeleeTipDirection(it->second, pc, kind, flip) * -1.0f;   // toward the butt
        out.a = a;
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : pts) { const double d = Dot(p - pc.center, a); lo = std::min(lo, d); hi = std::max(hi, d); }
        out.lo = lo; out.hi = hi; out.origin = pc.center;
        return true;
    }
    const char* r0 = kind.family == Family::Bow ? "partb" : "part01";
    const char* r1 = kind.family == Family::Bow ? "parta" : "part02";
    if (!roles.count(r0) || !roles.count(r1)) return false;
    const Vec3 c0 = Centroid(UsedPositions(roles.at(r0))), c1 = Centroid(UsedPositions(roles.at(r1)));
    const Vec3 mid = (c0 + c1) * 0.5f;
    Vec3 a = pc.axis[0];
    if (out.stock) {
        if (Dot(pc.center - mid, a) < 0) a = a * -1.0f;
        out.a = a;
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : pts) { const double d = Dot(p - pc.center, a); lo = std::min(lo, d); hi = std::max(hi, d); }
        out.lo = lo; out.hi = hi; out.origin = pc.center;
    } else {
        Vec3 lat = Norm(c0 - c1);
        out.lat = lat; out.a = lat;
        double lo = 1e30, hi = -1e30;
        for (const Vec3& p : pts) { const double d = std::fabs(Dot(p - mid, lat)); lo = std::min(lo, d); hi = std::max(hi, d); }
        out.lo = lo; out.hi = hi; out.origin = mid;
    }
    return true;
}
inline double PieceT(const PieceAxis& ax, const Vec3& p) {
    const double d = ax.stock ? Dot(p - ax.centre, ax.a) : std::fabs(Dot(p - ax.origin, ax.lat));
    return (d - ax.lo) / std::max(ax.hi - ax.lo, 1e-9);
}

// Cuts a piece by the plane f(p) = level (loop cuts: a mesh with a long bare span, like a stock's grip, has no vertices there to reshape).
// Triangles crossing the plane are split; positions, UVs and normals are interpolated, so the texture stays where it was.
template <class F>
inline void SliceAtLevel(Piece& p, const F& f, double level, size_t& added) {
    struct Cn { Vec3 pos, nrm; Vec2 uv; double d; };
    Piece out;
    out.name = p.name; out.role = p.role;
    auto emit = [&](const Cn& a, const Cn& b, const Cn& c) {
        Tri t;
        const Cn* cs[3] = {&a, &b, &c};
        for (int i = 0; i < 3; ++i) {
            out.pos.push_back(cs[i]->pos); out.nrm.push_back(cs[i]->nrm); out.uv.push_back(cs[i]->uv);
            const int k = static_cast<int>(out.pos.size()) - 1;
            t[static_cast<size_t>(i)] = {k, k, k};
        }
        out.tris.push_back(t);
    };
    const double eps = 1e-6;
    for (const Tri& t : p.tris) {
        Cn c[3];
        for (int i = 0; i < 3; ++i) {
            c[i].pos = p.pos[static_cast<size_t>(t[static_cast<size_t>(i)].v)]; c[i].nrm = p.nrm[static_cast<size_t>(t[static_cast<size_t>(i)].n)];
            c[i].uv = p.uv[static_cast<size_t>(t[static_cast<size_t>(i)].t)]; c[i].d = f(c[i].pos) - level;
        }
        const bool anyPos = c[0].d > eps || c[1].d > eps || c[2].d > eps, anyNeg = c[0].d < -eps || c[1].d < -eps || c[2].d < -eps;
        if (!(anyPos && anyNeg)) { emit(c[0], c[1], c[2]); continue; }
        // Sutherland-Hodgman against the plane: the polygons on each side, fanned into triangles
        auto lerp = [&](const Cn& a, const Cn& b) {
            const double u = a.d / (a.d - b.d);
            Cn r;
            r.pos = Mix(a.pos, b.pos, u); r.nrm = Norm(Mix(a.nrm, b.nrm, u)); r.uv = {static_cast<float>(a.uv.x + (b.uv.x - a.uv.x) * u), static_cast<float>(a.uv.y + (b.uv.y - a.uv.y) * u)}; r.d = 0;
            return r;
        };
        for (int side = 0; side < 2; ++side) {
            std::vector<Cn> poly;
            for (int i = 0; i < 3; ++i) {
                const Cn& a = c[i]; const Cn& b = c[(i + 1) % 3];
                const bool ain = side == 0 ? a.d >= -eps : a.d <= eps;
                if (ain) poly.push_back(a);
                if ((a.d > eps && b.d < -eps) || (a.d < -eps && b.d > eps)) poly.push_back(lerp(a, b));
            }
            for (size_t i = 1; i + 1 < poly.size(); ++i) emit(poly[0], poly[i], poly[i + 1]);
            (void)side;
        }
    }
    added += out.tris.size() - p.tris.size();
    p = std::move(out);
}

// Applies one edit to the pieces it names (before the fit; positions in the model's own frame).
inline void ApplyShapeEdit(RoleMap& roles, const Kind& kind, bool flip, const ShapeEdit& e, std::vector<std::string>& log) {
    if (std::fabs(e.factor - 1.0) < 1e-6) return;   // nothing to change: leave the mesh as it is (no cuts either)
    const std::vector<std::string> targets = EditTargets(kind, e.piece);
    if (targets.empty()) { log.push_back("edit: a " + std::string(kind.id) + " has no '" + e.piece + "' piece: skipped"); return; }
    // every piece's axis from the model as it was: editing the first limb must not move the second one's axis (it is measured against both)
    std::map<std::string, PieceAxis> axes;
    for (const std::string& role : targets) { PieceAxis a; if (ComputePieceAxis(roles, kind, role, flip, a)) axes[role] = a; }
    for (const std::string& role : targets) {
        if (!axes.count(role)) continue;
        const PieceAxis ax = axes[role];
        Piece& p = roles[role];
        {   // loop cuts across the edited stretch (and its soft ends): at most 0.07 of the piece's length apart (8 cuts at most)
            const double a0 = std::max(0.0, e.t0 - e.soft), a1 = std::min(1.0, e.t1 + e.soft);
            const int n = std::max(1, std::min(8, static_cast<int>(std::ceil((a1 - a0) / 0.07))));
            double sgn = 1;
            if (!ax.stock) sgn = Dot(Centroid(UsedPositions(p)) - ax.origin, ax.lat) >= 0 ? 1.0 : -1.0;
            auto f = [&](const Vec3& v) { return ax.stock ? Dot(v - ax.centre, ax.a) : sgn * Dot(v - ax.origin, ax.lat); };
            size_t added = 0;
            for (int i = 0; i <= n; ++i) {
                const double t = a0 + (a1 - a0) * i / n;
                SliceAtLevel(p, f, ax.lo + t * (ax.hi - ax.lo), added);
            }
            if (added) { char b[160]; std::snprintf(b, sizeof b, "edit: %s cut %d times across t %.2f-%.2f (+%zu triangles) so there are vertices to reshape", role.c_str(), n + 1, a0, a1, added); log.push_back(b); }
        }
        const std::vector<Vec3> oldPos = p.pos;
        // the piece's centre line: the mean of the points near each t (a bent piece has a bent line)
        const int B = 48;
        std::vector<Vec3> sum(B); std::vector<int> cnt(B, 0);
        for (const Vec3& v : UsedPositions(p)) {
            const int b = std::max(0, std::min(B - 1, static_cast<int>(PieceT(ax, v) * B)));
            sum[static_cast<size_t>(b)] = sum[static_cast<size_t>(b)] + v; ++cnt[static_cast<size_t>(b)];
        }
        std::vector<Vec3> line(B); std::vector<char> have(B, 0);
        for (int b = 0; b < B; ++b) if (cnt[static_cast<size_t>(b)]) { line[static_cast<size_t>(b)] = sum[static_cast<size_t>(b)] * (1.0f / cnt[static_cast<size_t>(b)]); have[static_cast<size_t>(b)] = 1; }
        auto centreAt = [&](double t) {   // linear between the nearest bins that have points
            const double x = t * B - 0.5;
            int b0 = static_cast<int>(std::floor(x)), b1 = b0 + 1;
            double w = x - b0;
            auto nearest = [&](int b, int dir) { for (int k = 0; k < B; ++k) { const int c = b + dir * k; if (c < 0 || c >= B) break; if (have[static_cast<size_t>(c)]) return c; } return -1; };
            int i0 = nearest(std::max(0, std::min(B - 1, b0)), -1), i1 = nearest(std::max(0, std::min(B - 1, b1)), +1);
            if (i0 < 0) i0 = i1;
            if (i1 < 0) i1 = i0;
            if (i0 < 0) return ax.centre;
            return Mix(line[static_cast<size_t>(i0)], line[static_cast<size_t>(i1)], i0 == i1 ? 0.0 : std::max(0.0, std::min(1.0, w)));
        };
        const double soft = std::max(e.soft, 1e-3);
        for (Vec3& v : p.pos) {
            const double t = PieceT(ax, v);
            const double win = SmoothStep01((t - (e.t0 - soft)) / (2 * soft)) * SmoothStep01(((e.t1 + soft) - t) / (2 * soft));
            const double f = 1.0 + (e.factor - 1.0) * win;
            const Vec3 c = centreAt(std::max(0.0, std::min(1.0, t)));
            v = c + (v - c) * static_cast<float>(f);
        }
        RecomputeNormals(p, oldPos);
        char b[200];
        std::snprintf(b, sizeof b, "edit: %s thickness x%.2f over t %.2f-%.2f (soft %.2f)", role.c_str(), e.factor, e.t0, e.t1, e.soft);
        log.push_back(b);
    }
}

// ---- the import -----------------------------------------------------------------------------------------
struct ImportOptions {
    std::string kind = "crossbow";   // crossbow, bow, sword, dagger, axe, club, pike
    int number = -1;                 // crbow04...; -1 = the first number nothing uses yet (kind's firstNew and up)
    std::string referenceNumber;     // the vanilla variant the corner scales are measured on; "" = the kind's own (crossbow 02, bow 00, melee: all variants averaged)
    int group = 0;                   // the mesh's render-flags word (18 or 19 on the melee weapons); 0 = the reference's / the family's most common
    FitOptions fit;
    UvMode uvMode = UvMode::Raw;     // hand-held parts: direct texture coordinates
    int uvTextureWidth = 128;        // for UvMode::Texture (atlas)
    NormalMode normals = NormalMode::Keep;
    bool replace = false;            // overwrite parts of that number when the rig has them
    std::vector<std::string> races = {"unhuma", "unhufe"};
    std::vector<ShapeEdit> edits;    // applied to the model's mesh before anything is built
    bool ground = true;              // crossbow: also the ground / inventory figure (initwecbNNweapon.fig, .bon, .lnk)
};

// A part to add to a rig: its mesh, its parent in the link table and its .bon offsets (zero: the mesh is in the parent's frame).
struct NewPart {
    std::string name, parent;
    fig::FigureMesh mesh;
    Vec3 offset[8] = {};
};

struct RaceOutput {
    std::string race, kind;
    int number = 0;
    std::vector<uint8_t> mod, bon;
    std::vector<std::string> parts;
    std::map<std::string, fig::FigureMesh> meshes; // the new parts, for the preview / checks
    CornerScales scales;
    Placement placement;
    std::map<std::string, std::vector<uint8_t>> extra;   // other files of the import (the ground figure), on the first race's output
};

inline std::vector<uint8_t> LinkTableFor(const std::vector<fig::LinkEntry>& l) { return figw::WriteLinkTable(l); }

// Adds parts to a copy of the rig's archives.
inline bool AssembleArchives(const Rig& rig, const std::vector<NewPart>& parts, bool replace,
                             std::vector<uint8_t>& modOut, std::vector<uint8_t>& bonOut, std::string& err) {
    std::map<std::string, std::vector<uint8_t>> mod, bon;
    for (const auto& kv : rig.mod.entries) mod[kv.second.originalName] = kv.second.data;
    for (const auto& kv : rig.bon.entries) bon[kv.second.originalName] = kv.second.data;
    std::vector<fig::LinkEntry> links = rig.links;
    auto findKey = [](std::map<std::string, std::vector<uint8_t>>& m, const std::string& n) {
        for (auto it = m.begin(); it != m.end(); ++it) if (res::Archive::ToLower(it->first) == res::Archive::ToLower(n)) return it;
        return m.end();
    };
    auto known = [&](const std::string& n) {   // in the rig, or added by an earlier part of this call
        if (rig.mod.Contains(n)) return true;
        for (const auto& q : parts) if (res::Archive::ToLower(q.name) == res::Archive::ToLower(n)) return true;
        return false;
    };
    for (const NewPart& pt : parts) {
        const std::string& name = pt.name;
        if (!known(pt.parent)) { err = rig.race + ": the parent part " + pt.parent + " does not exist"; return false; }
        const bool exists = rig.mod.Contains(name);
        if (exists && !replace) { err = rig.race + ": the part " + name + " already exists (use replace to overwrite it)"; return false; }
        std::vector<uint8_t> bytes;
        if (!figw::WriteFig(pt.mesh, bytes, err)) { err = name + ": " + err; return false; }
        if (auto it = findKey(mod, name); it != mod.end()) it->second = bytes; else mod[name] = bytes;
        if (auto it = findKey(bon, name); it != bon.end()) it->second = figw::WriteBon(pt.offset); else bon[name] = figw::WriteBon(pt.offset);
        bool linked = false;
        for (auto& l : links) if (res::Archive::ToLower(l.child) == res::Archive::ToLower(name)) { l.parent = pt.parent; linked = true; }
        if (!linked) links.push_back({name, pt.parent});
    }
    if (auto it = findKey(mod, rig.race); it != mod.end()) it->second = figw::WriteLinkTable(links);
    modOut = res::WriteArchive(mod, rig.timestamp);
    bonOut = res::WriteArchive(bon, rig.timestamp);
    return true;
}

// ---- project file -----------------------------------------------------------------------------------------------------
// A weapon import as a small text file (.umweapon): the model, the options and the shape edits. Changing an edit and importing again
// rebuilds every part of both races: the file is the one place a weapon's shape is described.
inline std::string ProjectText(const std::string& model, const ImportOptions& o) {
    char b[600];
    std::string t = "# um-multitool weapon import project (viewer --weapon-project <file> --out <folder>)\n";
    t += "model=" + model + "\n";
    std::snprintf(b, sizeof b, "kind=%s\nnumber=%d\nreference=%s\ngroup=%d\nuv=%s\nwidth=%d\nnormals=%s\nfit=%s\nscale=%g\nshift=%g,%g,%g\nlevel=%d\nheight=%s\nprod=%s\nup=%s\nground=%d\ndepth=%s\nflip=%d\nroll=%g\ngrip=%g\nreplace=%d\n",
                  o.kind.c_str(), o.number, o.referenceNumber.c_str(), o.group, o.uvMode == UvMode::Texture ? "atlas" : "raw", o.uvTextureWidth, o.normals == NormalMode::Smooth ? "smooth" : "keep",
                  o.fit.autoFit ? "auto" : "off", o.fit.scale, o.fit.shift.x, o.fit.shift.y, o.fit.shift.z, o.fit.levelStock ? 1 : 0, o.fit.gripHeight ? "grip" : "anchors",
                  o.fit.prodAtHinge ? "hinge" : "keep", o.fit.upAuto ? "auto" : o.fit.upIsZ ? "z" : "down", o.ground ? 1 : 0, o.fit.keepDepth ? "keep" : "match", o.fit.flip ? 1 : 0, o.fit.roll, o.fit.grip, o.replace ? 1 : 0);
    t += b;
    for (const ShapeEdit& e : o.edits) { std::snprintf(b, sizeof b, "edit=%s,%g,%g,%g,%g\n", e.piece.c_str(), e.t0, e.t1, e.factor, e.soft); t += b; }
    return t;
}
inline bool ParseProject(const std::string& text, const std::string& baseDir, std::string& model, ImportOptions& o, std::string& err) {
    o = ImportOptions{};
    o.edits.clear();
    model.clear();
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { err = "line " + std::to_string(n) + ": no '='"; return false; }
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "model") model = v;
        else if (k == "kind") { if (!FindKind(v)) { err = "line " + std::to_string(n) + ": the kind is crossbow, bow, sword, dagger, axe, club or pike"; return false; } o.kind = FindKind(v)->id; }
        else if (k == "number") o.number = std::atoi(v.c_str());
        else if (k == "reference") o.referenceNumber = v.empty() ? std::string() : TwoDigits(std::atoi(v.c_str()));
        else if (k == "group") o.group = std::atoi(v.c_str());
        else if (k == "uv") o.uvMode = v == "atlas" ? UvMode::Texture : UvMode::Raw;
        else if (k == "width") o.uvTextureWidth = std::atoi(v.c_str());
        else if (k == "normals") o.normals = v == "smooth" ? NormalMode::Smooth : NormalMode::Keep;
        else if (k == "fit") o.fit.autoFit = v != "off";
        else if (k == "scale") o.fit.scale = std::atof(v.c_str());
        else if (k == "shift") { float x = 0, y = 0, z = 0; std::sscanf(v.c_str(), "%f,%f,%f", &x, &y, &z); o.fit.shift = {x, y, z}; }
        else if (k == "level") o.fit.levelStock = std::atoi(v.c_str()) != 0;
        else if (k == "height") o.fit.gripHeight = v != "anchors";
        else if (k == "prod") o.fit.prodAtHinge = v != "keep";
        else if (k == "up") { o.fit.upAuto = v != "z" && v != "down"; o.fit.upIsZ = v != "down"; }
        else if (k == "ground") o.ground = std::atoi(v.c_str()) != 0;
        else if (k == "depth") o.fit.keepDepth = v == "keep";
        else if (k == "flip") o.fit.flip = std::atoi(v.c_str()) != 0;
        else if (k == "roll") o.fit.roll = std::atof(v.c_str());
        else if (k == "grip") o.fit.grip = std::atof(v.c_str());
        else if (k == "replace") o.replace = std::atoi(v.c_str()) != 0;
        else if (k == "edit") {
            ShapeEdit e; char piece[32] = "";
            if (std::sscanf(v.c_str(), "%31[^,],%lf,%lf,%lf,%lf", piece, &e.t0, &e.t1, &e.factor, &e.soft) < 4) { err = "line " + std::to_string(n) + ": edit=<main|limbs|strings>,t0,t1,factor[,soft]"; return false; }
            e.piece = piece;
            if (e.piece != "main" && e.piece != "limbs" && e.piece != "strings") { err = "line " + std::to_string(n) + ": the edit's piece is main, limbs or strings"; return false; }
            o.edits.push_back(e);
        } else { err = "line " + std::to_string(n) + ": unknown key '" + k + "'"; return false; }
    }
    if (model.empty()) { err = "the project names no model"; return false; }
    const bool absolute = !model.empty() && (model[0] == '/' || model[0] == '\\' || (model.size() > 1 && model[1] == ':'));
    if (!absolute && !baseDir.empty()) model = baseDir + "/" + model;
    return true;
}

// ---- the ground / inventory figure ------------------------------------------------------------------------------------
// initwecb<TTI>weapon.fig is the item lying on the ground and in the inventory: ONE mesh of all the parts, the 8 corners identical. Measured on the vanilla
// crossbows 1-3 (the hand parts at corner 5 against the ground figure, matched by their UVs): an exact similarity, the same for all three,
// ground = 1.8642 * M * hand + t, with M turning the weapon 45 degrees about Z (the stock lies on a diagonal) and upside down (x, y, z) -> ((x + y) / sqrt 2, (x - y) / sqrt 2, -z).
// t is not a constant: the z of the box is centred, so is the box across the diagonal; along it the figure's origin sits at the hand-frame y of about
// the mean of the vertices (-0.52 .. -0.55 measured, the vertex mean is -0.54 .. -0.57). The UVs go through the 256 atlas (a 128 px texture covers
// u 0..0.5, v 0.5..1), `group` and `textureNumber` are the reference's. The bone / .lnk are the vanilla ones (zero offsets; the socket redirect "weapon").
// Only the crossbow: the other kinds turn by other angles (bow 04 and sword 02 differ), not measured on enough items to derive.
inline bool BuildGroundFigure(const LayeredAssetSource& src, const Rig& built, const Kind& kind, int number, const std::string& refNN, int textureWidth,
                              std::map<std::string, std::vector<uint8_t>>& files, std::vector<std::string>& log, std::string& err) {
    if (kind.family != Family::Crossbow) { log.push_back(std::string("ground figure: not derived for ") + kind.id + " yet (only the crossbow's is)"); return true; }
    const std::string nn = TwoDigits(number);
    const double sc = 1.8642, r2 = 1.0 / std::sqrt(2.0);
    Piece P;
    P.name = "ground";
    for (const RoleDef& r : kind.roles) {
        const std::string part = PartName(r, nn);
        fig::FigureMesh m;
        if (!built.LoadMesh(part, m)) { err = "ground figure: " + part + " is missing"; return false; }
        const Vec3 origin = ChainOrigin(built, part, kind.rootAncestor, 5);
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            Tri t;
            for (int k = 0; k < 3; ++k) {
                const auto& vc = m.vertexComponents[m.indices[i + static_cast<size_t>(k)]];
                P.pos.push_back(m.morphVertex[5][vc.vertexIndex] + origin);
                P.nrm.push_back(m.normals[vc.normalIndex]);
                P.uv.push_back(MapUv(m.uvs[vc.uvIndex], UvMode::Texture, textureWidth));
                const int idx = static_cast<int>(P.pos.size()) - 1;
                t[static_cast<size_t>(k)] = {idx, idx, idx};
            }
            P.tris.push_back(t);
        }
    }
    if (P.tris.empty()) { err = "ground figure: no triangles"; return false; }
    auto turn = [&](const Vec3& v) { return Vec3{static_cast<float>((v.x + v.y) * r2), static_cast<float>((v.x - v.y) * r2), -v.z}; };
    double meanY = 0;
    for (const Vec3& v : UsedPositions(P)) meanY += v.y;
    meanY /= static_cast<double>(std::max<size_t>(1, UsedPositions(P).size()));
    for (Vec3& v : P.pos) v = turn(v) * static_cast<float>(sc);
    for (Vec3& n : P.nrm) n = Norm(turn(n));
    // t: the box centred in z and across the diagonal (b = (1, 1) / sqrt 2); along it (a = (-1, 1) / sqrt 2) the hand-frame y `meanY` goes to 0
    double zlo = 1e30, zhi = -1e30, blo = 1e30, bhi = -1e30;
    for (const Vec3& v : P.pos) { zlo = std::min<double>(zlo, v.z); zhi = std::max<double>(zhi, v.z); const double b = (v.x + v.y) * r2; blo = std::min(blo, b); bhi = std::max(bhi, b); }
    const double da = sc * meanY, db = -(blo + bhi) / 2, dz = -(zlo + zhi) / 2;
    const Vec3 t{static_cast<float>(-da * r2 + db * r2), static_cast<float>(da * r2 + db * r2), static_cast<float>(dz)};
    for (Vec3& v : P.pos) v = v + t;
    // the reference's words and files
    const std::string ref = "initwecb" + std::to_string(std::atoi(refNN.c_str())), self = "initwecb" + std::to_string(number);
    int group = 18, tex = 2;
    std::vector<uint8_t> refFig, refLnk;
    if (src.ReadFile(ref + "weapon.fig", refFig)) { fig::FigureMesh rm; if (fig::ParseFig(refFig, rm)) { group = rm.group; tex = rm.textureNumber; } }
    if (!src.ReadFile(ref + ".lnk", refLnk) && !src.ReadFile("initwecb1.lnk", refLnk)) { err = "ground figure: no vanilla initwecb<N>.lnk to copy"; return false; }
    CornerScales one;
    fig::FigureMesh gm;
    if (!BuildMesh(P, one, group, tex, gm, err)) { err = "ground figure: " + err; return false; }
    std::vector<uint8_t> bytes;
    if (!figw::WriteFig(gm, bytes, err)) { err = "ground figure: " + err; return false; }
    const fig::Vec3 zero[8] = {};
    files[self + "weapon.fig"] = bytes;
    files[self + "weapon.bon"] = figw::WriteBon(zero);
    files[self + ".lnk"] = refLnk;
    char b[300];
    std::snprintf(b, sizeof b, "ground figure %sweapon.fig: %zu points, %zu triangles, scale %.4f, turned 45 degrees and upside down, centred; group %d texture %d (the reference's)", self.c_str(), gm.morphVertex[0].size(), gm.indices.size() / 3, sc, group, tex);
    log.push_back(b);
    return true;
}

// The placed piece's extent along the rig's Y (the tip is at the smallest, the butt at the largest).
struct PieceRange { double tipY = 0, buttY = 0; };
inline PieceRange RangeOf(const Piece& p, const Placement& pl) {
    PieceRange r{1e30, -1e30};
    for (const Vec3& v : UsedPositions(p)) { const double y = pl.Apply(v).y; r.tipY = std::min(r.tipY, y); r.buttY = std::max(r.buttY, y); }
    return r;
}

// A rig's weapons against unhuma's: the vanilla weapons of the female rig are 4-16% longer at corner 5 than the male's (its builds scale down further), so a
// new weapon is made that much longer there: the average, over the variants both rigs have, of the length of the same weapon.
inline double MeleeRaceFactor(const Rig& base, const Rig& rig, const Kind& kind, int* variants = nullptr) {
    if (rig.race == base.race) return 1;
    auto length = [&](const Rig& r, const std::string& part, double& out) {
        fig::FigureMesh m;
        if (!r.LoadMesh(part, m) || !figw::UsedPoints(m)) return false;
        double lo = 1e30, hi = -1e30;
        for (size_t i = 0; i < figw::UsedPoints(m); ++i) { lo = std::min<double>(lo, m.morphVertex[5][i].y); hi = std::max<double>(hi, m.morphVertex[5][i].y); }
        out = hi - lo;
        return true;
    };
    double sum = 0;
    int n = 0;
    for (int i = 0; i < 100; ++i) {
        const std::string part = PartName(kind.roles[0], TwoDigits(i));
        double a, b;
        if (length(base, part, a) && length(rig, part, b) && a > 1e-6) { sum += b / a; ++n; }
    }
    if (variants) *variants = n;
    return n ? sum / n : 1.0;
}
inline Kind::Grip HandFor(const Kind& kind, const std::string& race) { return kind.grip[race == "unhufe" ? 1 : 0]; }

// The first number from the kind's firstNew up that none of the rigs has a part of.
inline int FirstFreeNumber(const std::vector<Rig>& rigs, const Kind& kind) {
    for (int n = kind.firstNew; n < 100; ++n) {
        bool used = false;
        for (const Rig& rig : rigs) for (const RoleDef& r : kind.roles) if (rig.HasPart(PartName(r, TwoDigits(n)))) used = true;
        if (!used) return n;
    }
    return -1;
}

// Imports the weapon of an OBJ (of the options' kind) into every race of the options. `src` holds the rigs' figures.
inline bool ImportWeapon(const LayeredAssetSource& src, const ObjFile& obj, const ImportOptions& opt, std::vector<RaceOutput>& out,
                         std::vector<std::string>& log, std::string& err) {
    out.clear();
    const Kind* kp = FindKind(opt.kind);
    if (!kp) { err = "unknown weapon kind '" + opt.kind + "'"; return false; }
    const Kind& kind = *kp;
    RoleMap rolesAll;
    if (!(kind.family == Family::Crossbow ? FindCrossbowRoles(obj, rolesAll, log, err) : kind.family == Family::Bow ? FindBowRoles(obj, rolesAll, log, err) : FindMeleeRoles(obj, rolesAll, log, err))) return false;
    std::vector<Rig> rigs;
    for (const std::string& race : opt.races) {
        Rig rig;
        if (!LoadRig(src, race, rig, err)) return false;
        rigs.push_back(std::move(rig));
    }
    std::vector<Rig> gameRigs;   // the base game's rigs, to measure on
    for (const Rig& r : rigs) { Rig g; std::string e2; if (!LoadBaseRig(src, r.race, g, e2)) g = r; gameRigs.push_back(std::move(g)); }
    Rig baseRig;   // the game's unhuma: what the other rigs' melee weapons are measured against
    { std::string e2; if (!LoadBaseRig(src, "unhuma", baseRig, e2)) baseRig = Rig{}; }
    const int number = opt.number >= 0 ? opt.number : FirstFreeNumber(rigs, kind);
    if (number < 0 || number > 99) { err = std::string("no free number left for a ") + kind.id; return false; }
    const std::string nn = TwoDigits(number);
    const std::string refNN = opt.referenceNumber.empty() ? std::string(kind.defaultReference) : opt.referenceNumber;
    char b[640];
    std::snprintf(b, sizeof b, "%s number %s%s", kind.label, nn.c_str(), opt.number < 0 ? " (the first free one)" : "");
    log.push_back(b);
    for (size_t ri = 0; ri < rigs.size(); ++ri) {
        const Rig& rig = rigs[ri];
        log.push_back("== " + rig.race);
        RaceOutput ro;
        ro.race = rig.race; ro.kind = kind.id; ro.number = number;
        const Rig& measureRig = opt.referenceNumber.empty() ? gameRigs[ri] : rig;   // the default reference is the game's; a named one is looked for in the rig as it is
        if (!MeasureCornerScales(measureRig, kind, refNN, ro.scales, err)) return false;
        std::snprintf(b, sizeof b, "corner scales (measured on %s): %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f; the reference fits them to %.6f over %zu vectors",
                      refNN.empty() ? (std::to_string(ro.scales.variants) + " vanilla variants, averaged").c_str() : (std::string(kind.id) + " " + refNN).c_str(),
                      ro.scales.k[0], ro.scales.k[1], ro.scales.k[2], ro.scales.k[3], ro.scales.k[4], ro.scales.k[5], ro.scales.k[6], ro.scales.k[7], ro.scales.worst, ro.scales.samples);
        log.push_back(b);
        RoleMap roles = rolesAll;
        FitTargets T;
        Vec3 anchors[2];
        if (kind.family == Family::Crossbow) {
            // where the rig has its hinge and string anchors (corner 5, in the main's frame)
            T.hinge[0] = ChainOrigin(rig, "crbow01part01", kind.rootAncestor, 5);
            T.hinge[1] = ChainOrigin(rig, "crbow01part02", kind.rootAncestor, 5);
            T.tip[0] = ChainOrigin(rig, "crbow01tetiva01", kind.rootAncestor, 5);
            T.tip[1] = ChainOrigin(rig, "crbow01tetiva02", kind.rootAncestor, 5);
            {   // the reference stock's direction: the weapon sits in the hand the way the vanilla crossbows do
                fig::FigureMesh refMain;
                if (rig.LoadMesh(PartName(kind.roles[0], refNN), refMain)) {
                    std::vector<Vec3> rp;
                    for (size_t i = 0; i < figw::UsedPoints(refMain); ++i) rp.push_back(refMain.morphVertex[5][i]);
                    Vec3 ax = ComputePca(UniquePositions(rp)).axis[0];
                    if (ax.y > 0) ax = ax * -1.0f;
                    T.forward = Norm(Vec3{0, ax.y, ax.z});
                    // the hand (the rig's origin) in the main's frame, and the height of the reference stock there
                    Vec3 rootOff[8];
                    rig.Offsets(kind.rootAncestor, rootOff);
                    T.hand = rootOff[5] * -1.0f;
                    const double len = ComputePca(UniquePositions(rp)).extent[0];
                    T.gripBand = 0.1 * len;
                    std::vector<TriPos> rt;
                    for (size_t i = 0; i + 2 < refMain.indices.size(); i += 3) {
                        TriPos tp;
                        for (int k = 0; k < 3; ++k) tp[static_cast<size_t>(k)] = refMain.morphVertex[5][refMain.vertexComponents[refMain.indices[i + static_cast<size_t>(k)]].vertexIndex];
                        rt.push_back(tp);
                    }
                    double zc = 0;
                    if (SliceCentreZ(rt, T.hand.y, T.gripBand, zc)) { T.refGripZ = zc; T.haveGrip = true; }
                    std::snprintf(b, sizeof b, "the reference stock is tilted %.2f degrees (forward 0 %.3f %.3f)", std::asin(std::max(-1.0f, std::min(1.0f, T.forward.z))) * 180.0 / 3.14159265358979, T.forward.y, T.forward.z);
                    log.push_back(b);
                }
            }
            if (!FitCrossbow(roles, T, opt.fit, ro.placement, log, err)) return false;   // the placement comes from the model as modelled: an edit never moves it
        } else if (kind.family == Family::Bow) {
            anchors[0] = ChainOrigin(rig, "bwtetivaa00", kind.rootAncestor, 5);
            anchors[1] = ChainOrigin(rig, "bwtetivab00", kind.rootAncestor, 5);
            if (!FitBow(roles, anchors, opt.fit, ro.placement, log, err)) return false;
        } else {
            int nv = 0;
            const double factor = baseRig.race.empty() ? 1.0 : MeleeRaceFactor(baseRig, gameRigs[ri], kind, &nv);
            if (factor != 1.0) { std::snprintf(b, sizeof b, "this rig's %s weapons are %.3f times unhuma's (average over %d variants): the weapon is made that much bigger here", kind.id, factor, nv); log.push_back(b); }
            if (rig.race != "unhuma" && rig.race != "unhufe") log.push_back("warning: untested rig: unhuma's hand constants are used");
            if (!FitMelee(roles, kind, HandFor(kind, rig.race), factor, opt.fit, ro.placement, log, err)) return false;
        }
        if (!opt.edits.empty()) {
            RoleMap edited = rolesAll;
            if (kind.family == Family::Crossbow) {
                if (ro.placement.swappedLimbs) std::swap(edited["part01"], edited["part02"]);
                if (ro.placement.swappedStrings) std::swap(edited["tetiva01"], edited["tetiva02"]);
            } else if (kind.family == Family::Bow) {
                if (ro.placement.swappedLimbs) std::swap(edited["partb"], edited["parta"]);
                if (ro.placement.swappedStrings) std::swap(edited["tetivab"], edited["tetivaa"]);
            }
            std::vector<std::string> editLog;
            for (const ShapeEdit& e : opt.edits) ApplyShapeEdit(edited, kind, opt.fit.flip, e, editLog);
            for (const std::string& l : editLog) log.push_back(l);
            roles = edited;
        }
        if (kind.family != Family::Bow) {   // where along the stock / the weapon the hand is: the range a handle edit wants
            PieceAxis ax;
            if (opt.fit.autoFit && ComputePieceAxis(roles, kind, "main", opt.fit.flip, ax)) {
                Vec3 q;
                if (kind.family == Family::Crossbow) q = (T.hand - ro.placement.t) * static_cast<float>(1.0 / ro.placement.s);
                else q = (Vec3{static_cast<float>(HandFor(kind, rig.race).x), 0, static_cast<float>(HandFor(kind, rig.race).z)} - ro.placement.t) * static_cast<float>(1.0 / ro.placement.s);
                const Vec3 m{static_cast<float>(ro.placement.R[0][0] * q.x + ro.placement.R[1][0] * q.y + ro.placement.R[2][0] * q.z),
                             static_cast<float>(ro.placement.R[0][1] * q.x + ro.placement.R[1][1] * q.y + ro.placement.R[2][1] * q.z),
                             static_cast<float>(ro.placement.R[0][2] * q.x + ro.placement.R[1][2] * q.y + ro.placement.R[2][2] * q.z)};
                std::snprintf(b, sizeof b, "the hand holds the %s at t = %.2f (t runs 0 at the %s to 1 at the butt): a handle edit wants about %.2f-%.2f", kind.family == Family::Crossbow ? "stock" : "weapon",
                              PieceT(ax, m), kind.family == Family::Crossbow ? "limbs" : "tip", PieceT(ax, m) - 0.12, PieceT(ax, m) + 0.12);
                log.push_back(b);
            }
        }
        // the group / texture words of the new meshes: the reference variant's, or the most common among the family's variants
        int group = opt.group;
        std::map<int, int> groupVotes, textureVotes;
        if (!refNN.empty()) {
            fig::FigureMesh ref;
            if (rig.LoadMesh(PartName(kind.roles[0], refNN), ref)) { if (!group) group = ref.group; }
        } else {
            for (const char* pat : kind.pool)
                for (int n = 0; n < 100; ++n) {
                    fig::FigureMesh m;
                    if (gameRigs[ri].HasPart(PatternName(pat, TwoDigits(n))) && gameRigs[ri].LoadMesh(PatternName(pat, TwoDigits(n)), m)) { ++groupVotes[m.group]; ++textureVotes[m.textureNumber]; }
                }
            if (!group) { int best = 0; for (const auto& kv : groupVotes) if (kv.second > best) { best = kv.second; group = kv.first; } }
        }
        std::vector<NewPart> parts;
        for (const RoleDef& r : kind.roles) {
            const std::string name = PartName(r, nn);
            Piece p = roles[r.role];
            if (opt.normals == NormalMode::Smooth) SmoothNormals(p);
            const Vec3 origin = ChainOrigin(rig, r.ancestor, kind.rootAncestor, 5);
            const bool prod = kind.family == Family::Crossbow && std::string(r.role) != "main";
            const bool limb = kind.family == Family::Bow && (std::string(r.role) == "partb" || std::string(r.role) == "parta");
            const bool bowString = kind.family == Family::Bow && !limb;
            std::vector<Vec3> placed;   // before a bow's bend (the normals are recomputed from the change)
            for (Vec3& v : p.pos) { v = ro.placement.Apply(v); placed.push_back(v); }
            for (Vec3& n : p.nrm) n = Norm(ro.placement.Dir(n));
            if (std::fabs(ro.placement.bend) > 1e-9) {
                for (Vec3& v : p.pos) {
                    if (limb) { const double t = std::fabs(double(v.y)) / ro.placement.bendLength; v.x += static_cast<float>(ro.placement.bend * t * t); }
                    else if (bowString) v.x += static_cast<float>(ro.placement.bend);
                }
                if (limb) RecomputeNormals(p, placed);
            }
            for (Vec3& v : p.pos) { v = v - origin; if (prod) v.z -= static_cast<float>(ro.placement.prodDrop); }
            for (Vec2& uv : p.uv) uv = MapUv(uv, opt.uvMode, opt.uvTextureWidth);
            int grp = group, tex = 2;
            if (!refNN.empty()) {
                fig::FigureMesh ref;
                const std::string refName = PartName(r, refNN);
                if (!rig.LoadMesh(refName, ref)) { err = rig.race + ": reference part " + refName + " is missing"; return false; }
                if (!opt.group) grp = ref.group;
                tex = ref.textureNumber;
            } else if (!textureVotes.empty()) { int best = 0; for (const auto& kv : textureVotes) if (kv.second > best) { best = kv.second; tex = kv.first; } }
            fig::FigureMesh m;
            if (!BuildMesh(p, ro.scales, grp, tex, m, err)) { err = name + ": " + err; return false; }
            std::snprintf(b, sizeof b, "%s: %zu points, %zu components, %zu triangles, group %d texture %d", name.c_str(), m.morphVertex[0].size(), m.vertexComponents.size(), m.indices.size() / 3, m.group, m.textureNumber);
            log.push_back(b);
            ro.meshes[name] = m;
            NewPart np;
            np.name = name; np.parent = r.ancestor; np.mesh = std::move(m);
            parts.push_back(std::move(np));
            ro.parts.push_back(name);
        }
        if (kind.family == Family::Melee) {   // the tiny "bash" part under the variant: the vanilla one's mesh, a few cm short of the tip
            const std::string baseName = PatternName(kind.basePattern, nn);
            const Rig& baseSrc = opt.referenceNumber.empty() ? gameRigs[ri] : rig;
            std::string refBase;
            for (int n = -1; n < 100 && refBase.empty(); ++n) {
                const std::string cand = PatternName(kind.basePattern, TwoDigits(n < 0 ? std::atoi(refNN.c_str()) : n));
                if ((n >= 0 || !refNN.empty()) && baseSrc.HasPart(cand)) refBase = cand;
            }
            if (!refBase.empty()) {
                NewPart np;
                np.name = baseName; np.parent = parts[0].name;
                baseSrc.LoadMesh(refBase, np.mesh);
                const PieceRange pr = RangeOf(roles["main"], ro.placement);
                const Vec3 off5{static_cast<float>(HandFor(kind, rig.race).x), static_cast<float>(pr.tipY + 0.12), static_cast<float>(HandFor(kind, rig.race).z)};
                for (int c = 0; c < 8; ++c) np.offset[c] = off5 * static_cast<float>(ro.scales.k[c]);
                parts.push_back(std::move(np));
                ro.parts.push_back(baseName);
                ro.meshes[baseName] = parts.back().mesh;
                log.push_back(baseName + ": the vanilla " + refBase + "'s mesh, offset (" + std::to_string(off5.x) + ", " + std::to_string(off5.y) + ", " + std::to_string(off5.z) + ")");
            } else log.push_back("note: no vanilla base part to copy: " + baseName + " not made");
        }
        if (!AssembleArchives(rig, parts, opt.replace, ro.mod, ro.bon, err)) return false;
        out.push_back(std::move(ro));
    }
    if (opt.ground && kind.family == Family::Crossbow && !out.empty()) {   // one ground figure for both races (the weapon in unhuma's frame)
        Rig built;
        if (!LoadRigBytes(out[0].mod, out[0].bon, out[0].race, built, err)) return false;
        if (!BuildGroundFigure(src, built, kind, number, refNN.empty() ? std::string(kind.defaultReference) : refNN, opt.uvMode == UvMode::Texture ? opt.uvTextureWidth : 128, out[0].extra, log, err)) return false;
    }
    return true;
}
inline bool ImportCrossbow(const LayeredAssetSource& src, const ObjFile& obj, const ImportOptions& opt, std::vector<RaceOutput>& out,
                           std::vector<std::string>& log, std::string& err) {
    ImportOptions o = opt;
    o.kind = "crossbow";
    return ImportWeapon(src, obj, o, out, log, err);
}

// ---- the reference as an OBJ: a crossbow variant in the authoring frame (main's frame, corner 5) ----------------
// uvMode Texture: the UVs are written over the texture (0..1) as an artist paints them, the importer's default reads them back.
inline std::string ExportReferenceObj(const Rig& rig, const Kind& kind, const std::string& nn, std::string& err, UvMode uvMode = UvMode::Raw, int uvTextureWidth = 128) {
    const char* frame = kind.family == Family::Crossbow ? "+X = the part01 side, -Y = forward, +Z = up" : kind.family == Family::Bow ? "+X = away from the archer (the string is at -X), +Y = the partb half, +Z = up" : "-Y = toward the tip, +Z = up, the hand at the origin";
    std::string out = std::string("# ") + kind.id + " " + nn + " of " + rig.race + ": the rig's frame at complection corner 5 (" + frame + ")\n";
    size_t vBase = 1, tBase = 1, nBase = 1;
    for (const RoleDef& r : kind.roles) {
        const std::string part = PartName(r, nn);
        fig::FigureMesh m;
        if (!rig.LoadMesh(part, m)) { err = part + " is missing in " + rig.race; return {}; }
        const Vec3 origin = ChainOrigin(rig, part, kind.rootAncestor, 5);
        char b[200];
        out += "o " + part + "\n";
        const size_t used = figw::UsedPoints(m);
        for (size_t i = 0; i < used; ++i) { const Vec3 v = m.morphVertex[5][i] + origin; std::snprintf(b, sizeof b, "v %.6f %.6f %.6f\n", v.x, v.y, v.z); out += b; }
        for (const auto& uv0 : m.uvs) { const Vec2 uv = UnmapUv(uv0, uvMode, uvTextureWidth); std::snprintf(b, sizeof b, "vt %.7f %.7f\n", uv.x, uv.y); out += b; }
        for (size_t i = 0; i < used; ++i) { const Vec3& n = m.normals[i]; std::snprintf(b, sizeof b, "vn %.6f %.6f %.6f\n", n.x, n.y, n.z); out += b; }
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            out += "f";
            for (int k = 0; k < 3; ++k) {
                const auto& vc = m.vertexComponents[m.indices[i + static_cast<size_t>(k)]];
                std::snprintf(b, sizeof b, " %zu/%zu/%zu", vBase + vc.vertexIndex, tBase + vc.uvIndex, nBase + vc.normalIndex);
                out += b;
            }
            out += "\n";
        }
        vBase += used; tBase += m.uvs.size(); nBase += used;
    }
    return out;
}
inline std::string ExportReferenceObj(const Rig& rig, const std::string& nn, std::string& err, UvMode uvMode = UvMode::Raw, int uvTextureWidth = 128) {
    return ExportReferenceObj(rig, CrossbowKind(), nn, err, uvMode, uvTextureWidth);
}

// ---- comparing a built part with a vanilla one (in the world frame of the root ancestor, all 8 corners) ------
struct MeshDiff { double position[8] = {}, uv = 0, normal = 0; size_t compsA = 0, compsB = 0; bool sameSize = true; };

inline MeshDiff CompareParts(const Rig& rigA, const std::string& partA, const fig::FigureMesh& a, const Rig& rigB, const std::string& partB,
                             const fig::FigureMesh& b, const std::string& root) {
    MeshDiff d;
    d.compsA = a.vertexComponents.size(); d.compsB = b.vertexComponents.size();
    if (d.compsA != d.compsB) { d.sameSize = false; return d; }
    struct Row { std::array<Vec3, 8> pos; Vec2 uv; Vec3 n; };
    auto rows = [&](const Rig& rig, const std::string& part, const fig::FigureMesh& m) {
        std::vector<Row> r;
        Vec3 org[8];
        for (int c = 0; c < 8; ++c) org[c] = ChainOrigin(rig, part, root, c);
        for (const auto& vc : m.vertexComponents) {
            Row w;
            for (int c = 0; c < 8; ++c) w.pos[static_cast<size_t>(c)] = m.morphVertex[c][vc.vertexIndex] + org[c];
            w.uv = m.uvs[vc.uvIndex];
            w.n = m.normals[vc.normalIndex];
            r.push_back(w);
        }
        return r;
    };
    const auto ra = rows(rigA, partA, a), rb = rows(rigB, partB, b);
    // every component of A is matched to the component of B with the same UV (then the nearest at corner 5); each B once
    std::vector<char> used(rb.size(), 0);
    for (const Row& x : ra) {
        size_t best = rb.size();
        double bestCost = 1e30;
        for (size_t j = 0; j < rb.size(); ++j) {
            if (used[j]) continue;
            const double cost = 1000.0 * (std::fabs(x.uv.x - rb[j].uv.x) + std::fabs(x.uv.y - rb[j].uv.y)) + Len(x.pos[5] - rb[j].pos[5]);
            if (cost < bestCost) { bestCost = cost; best = j; }
        }
        if (best == rb.size()) { d.sameSize = false; return d; }
        used[best] = 1;
        for (int c = 0; c < 8; ++c) d.position[c] = std::max(d.position[c], Len(x.pos[static_cast<size_t>(c)] - rb[best].pos[static_cast<size_t>(c)]));
        d.uv = std::max(d.uv, std::max<double>(std::fabs(x.uv.x - rb[best].uv.x), std::fabs(x.uv.y - rb[best].uv.y)));
        d.normal = std::max(d.normal, Len(x.n - rb[best].n));
    }
    return d;
}

// ---- files ------------------------------------------------------------------------------------------------
inline bool ReadWholeFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}
inline bool WriteWholeFile(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

// Writes an import into a folder: <race>.mod / <race>.bon, placed-<race>.obj (the result in the authoring frame, to look
// at beside the reference) and import-report.txt; with packDest also a copy of the figures archive the rigs came from,
// holding the new .mod / .bon. Never into what was read: a folder that is a figure source, or the archive itself.
inline bool SaveImport(const LayeredAssetSource& src, const std::vector<RaceOutput>& out, const std::vector<std::string>& log,
                       const std::string& outDir, const std::string& packDest, std::string& err, UvMode uvMode = UvMode::Raw, int uvWidth = 128) {
    std::error_code ec;
    const std::filesystem::path outAbs = std::filesystem::weakly_canonical(outDir, ec);
    for (const auto& l : src.layers) {
        if (l.path.empty()) continue;
        const std::filesystem::path p = std::filesystem::weakly_canonical(l.path, ec);
        if (p == outAbs || (std::filesystem::is_regular_file(p, ec) && p.parent_path() == outAbs)) {
            err = outDir + " is one of the figure sources: pick another folder (the game's and the mod's files are never written)";
            return false;
        }
    }
    std::filesystem::create_directories(outAbs, ec);
    std::string report;
    for (const std::string& l : log) report += l + "\n";
    for (const RaceOutput& ro : out) {
        if (!WriteWholeFile((outAbs / (ro.race + ".mod")).string(), ro.mod.data(), ro.mod.size()) || !WriteWholeFile((outAbs / (ro.race + ".bon")).string(), ro.bon.data(), ro.bon.size())) {
            err = "cannot write into " + outDir;
            return false;
        }
        Rig made;
        std::string e2;
        if (LoadRigBytes(ro.mod, ro.bon, ro.race, made, e2)) {
            const std::string placed = ExportReferenceObj(made, *FindKind(ro.kind), TwoDigits(ro.number), e2, uvMode, uvWidth);
            WriteWholeFile((outAbs / ("placed-" + ro.race + ".obj")).string(), placed.data(), placed.size());
        }
    }
    for (const RaceOutput& ro : out)
        for (const auto& kv : ro.extra)
            if (!WriteWholeFile((outAbs / kv.first).string(), kv.second.data(), kv.second.size())) { err = "cannot write " + (outAbs / kv.first).string(); return false; }
    WriteWholeFile((outAbs / "import-report.txt").string(), report.data(), report.size());
    if (!packDest.empty()) {
        const int layer = src.LayerWith(out[0].race + ".mod");
        if (layer < 0 || !src.layers[static_cast<size_t>(layer)].source.isArchive) { err = "the rigs do not come from a .res archive: nothing to pack"; return false; }
        const std::string srcPath = src.layers[static_cast<size_t>(layer)].path;
        const std::filesystem::path destAbs = std::filesystem::weakly_canonical(packDest, ec);
        for (const auto& l : src.layers) if (!l.path.empty() && std::filesystem::weakly_canonical(l.path, ec) == destAbs) { err = packDest + " is one of the figure sources: refusing to overwrite it"; return false; }
        std::string raw;
        if (!ReadWholeFile(srcPath, raw)) { err = "cannot read " + srcPath; return false; }
        std::map<std::string, std::vector<uint8_t>> replace;
        for (const RaceOutput& ro : out) {
            replace[ro.race + ".mod"] = ro.mod; replace[ro.race + ".bon"] = ro.bon;
            for (const auto& kv : ro.extra) replace[res::Archive::ToLower(kv.first)] = kv.second;
        }
        std::vector<uint8_t> packed;
        const std::vector<uint8_t> rawBytes(raw.begin(), raw.end());
        res::Archive arc;
        bool allThere = res::ParseArchive(rawBytes, arc, err);
        if (!allThere) return false;
        for (const auto& kv : replace) if (!arc.Contains(kv.first)) allThere = false;
        if (allThere) { if (!res::RewriteArchive(rawBytes, replace, static_cast<uint32_t>(std::time(nullptr)), packed, err)) return false; }
        else {   // a file the archive does not have yet (a new ground figure): the entries again, with the new ones in
            std::map<std::string, std::vector<uint8_t>> entries;
            for (const auto& kv : arc.entries) entries[kv.second.originalName] = kv.second.data;
            for (const auto& kv : replace) {
                for (auto it = entries.begin(); it != entries.end(); ++it) if (res::Archive::ToLower(it->first) == kv.first) { entries.erase(it); break; }
                entries[kv.first] = kv.second;
            }
            packed = res::WriteArchive(entries, static_cast<uint32_t>(std::time(nullptr)));
        }
        if (!WriteWholeFile(destAbs.string(), packed.data(), packed.size())) { err = "cannot write " + packDest; return false; }
    }
    return true;
}

} // namespace wimp
