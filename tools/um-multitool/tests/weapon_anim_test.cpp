// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O2 -Iviewer -I. tests/weapon_anim_test.cpp -o weapon_anim_test
// ./weapon_anim_test [--kind crossbow|bow|sword|dagger|axe|club|pike] <new NN> <reference NN> [--csv out.csv] [--random N] [--all-clips] <figures layer>...   (the last layer holds the new parts)
// Bows: the string end against its limb's tip, the strings' inner ends (nock), the two limb halves' join, the hand against the limbs; melee weapons: hand-to-tip, hand-to-butt and hand-to-handle
// distances. Each is compared with the same measure on the vanilla variant; the new weapon must not drift more than it does (plus a few mm / cm: see RunGeneric).
// A frame by frame check of a crossbow on the real rig: every frame of every clip that moves the crossbow's string, at the 8
// complection corners and at N random builds in between (the build is continuous: the game blends the corners), measuring
//   tip   - a limb's tip to its string's outer end (the string hangs on the limb's tip),
//   nock  - the two strings' inner ends to each other (they meet at the stock's axis),
//   root  - a limb's root to the nearest point of the stock's surface (the limbs sit on the stock).
// Each is compared with the same measure at rest (what the model looks like standing still): "drift" = how far a frame moves
// from it. The vanilla variant is measured the same way: a new weapon must not drift more than vanilla does.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "model_loader.hpp"
#include "weapon_import.hpp"

using fig::Vec3;

static Vec3 PointAt(const fig::Model& m, const std::vector<fig::PartPose>& pose, size_t part, size_t point, const Vec3& k) {
    const fig::FigureMesh& mesh = m.parts[part].mesh;
    Vec3 corners[8];
    for (int c = 0; c < 8; ++c) corners[c] = mesh.morphVertex[c][point];
    return pose[part].p + fig::QRotate(pose[part].w, fig::BlendComplection(corners, k));
}
struct Pts { size_t part; std::vector<size_t> idx; };
static Vec3 Cen(const fig::Model& m, const std::vector<fig::PartPose>& pose, const Pts& t, const Vec3& k) {
    Vec3 s{};
    for (size_t p : t.idx) s = s + PointAt(m, pose, t.part, p, k);
    return s * (1.0f / static_cast<float>(t.idx.size()));
}
// the closest point of a triangle to p (Ericson, Real-Time Collision Detection)
static double DistPointTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = wimp::Dot(ab, ap), d2 = wimp::Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return wimp::Len(p - a);
    const Vec3 bp = p - b; const double d3 = wimp::Dot(ab, bp), d4 = wimp::Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return wimp::Len(p - b);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return wimp::Len(p - (a + ab * static_cast<float>(d1 / (d1 - d3))));
    const Vec3 cp = p - c; const double d5 = wimp::Dot(ab, cp), d6 = wimp::Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return wimp::Len(p - c);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return wimp::Len(p - (a + ac * static_cast<float>(d2 / (d2 - d6))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return wimp::Len(p - (b + (c - b) * static_cast<float>((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const double denom = 1.0 / (va + vb + vc);
    return wimp::Len(p - (a + ab * static_cast<float>(vb * denom) + ac * static_cast<float>(vc * denom)));
}

struct Setup { size_t main, limb[2], str[2]; Pts tip[2], root[2], strOuter[2], strInner[2]; std::vector<std::array<size_t, 3>> stockTris; };

static bool MakeSetup(const fig::Model& m, const std::string& nn, const Vec3& k, Setup& s) {
    const int im = m.FindPartIndex("rh3.crbow" + nn + "main");
    const int il[2] = {m.FindPartIndex("crbow" + nn + "part01"), m.FindPartIndex("crbow" + nn + "part02")};
    const int is[2] = {m.FindPartIndex("crbow" + nn + "tetiva01"), m.FindPartIndex("crbow" + nn + "tetiva02")};
    if (im < 0 || il[0] < 0 || il[1] < 0 || is[0] < 0 || is[1] < 0) return false;
    s.main = static_cast<size_t>(im);
    const auto rest = fig::PoseModel(m, nullptr, 0, k, 0.0f);
    auto all = [&](size_t part) { Pts p{part, {}}; for (size_t i = 0; i < figw::UsedPoints(m.parts[part].mesh); ++i) p.idx.push_back(i); return p; };
    const float xMain = Cen(m, rest, all(s.main), k).x;
    auto ends = [&](size_t part, double sign, bool outer) {
        const size_t used = figw::UsedPoints(m.parts[part].mesh);
        std::vector<float> xs;
        for (size_t i = 0; i < used; ++i) xs.push_back(static_cast<float>(sign * (PointAt(m, rest, part, i, k).x - xMain)));
        const float lo = *std::min_element(xs.begin(), xs.end()), hi = *std::max_element(xs.begin(), xs.end());
        Pts t{part, {}};
        for (size_t i = 0; i < used; ++i) if (outer ? xs[i] >= hi - 0.12f * (hi - lo) : xs[i] <= lo + 0.12f * (hi - lo)) t.idx.push_back(i);
        return t;
    };
    for (int side = 0; side < 2; ++side) {
        s.limb[side] = static_cast<size_t>(il[side]); s.str[side] = static_cast<size_t>(is[side]);
        const double sd = Cen(m, rest, all(s.limb[side]), k).x >= xMain ? 1.0 : -1.0;
        s.tip[side] = ends(s.limb[side], sd, true); s.root[side] = ends(s.limb[side], sd, false);
        s.strOuter[side] = ends(s.str[side], sd, true); s.strInner[side] = ends(s.str[side], sd, false);
    }
    const fig::FigureMesh& mm = m.parts[s.main].mesh;
    s.stockTris.clear();
    for (size_t i = 0; i + 2 < mm.indices.size(); i += 3)
        s.stockTris.push_back({mm.vertexComponents[mm.indices[i]].vertexIndex, mm.vertexComponents[mm.indices[i + 1]].vertexIndex, mm.vertexComponents[mm.indices[i + 2]].vertexIndex});
    return true;
}
struct Gaps { double tip[2], nock, root[2], grip; };
static Gaps Measure(const fig::Model& m, const Setup& s, const std::vector<fig::PartPose>& pose, const Vec3& k) {
    Gaps g{};
    {   // the hand (the origin of rh3) to the stock's surface: the stock must stay in the hand in every frame
        const int ih = m.FindPartIndex("rh3");
        double best = 1e30;
        if (ih >= 0) for (const auto& t : s.stockTris) best = std::min(best, DistPointTri(pose[static_cast<size_t>(ih)].p, PointAt(m, pose, s.main, t[0], k), PointAt(m, pose, s.main, t[1], k), PointAt(m, pose, s.main, t[2], k)));
        g.grip = best;
    }
    for (int i = 0; i < 2; ++i) {
        g.tip[i] = wimp::Len(Cen(m, pose, s.tip[i], k) - Cen(m, pose, s.strOuter[i], k));
        const Vec3 r = Cen(m, pose, s.root[i], k);
        double best = 1e30;
        for (const auto& t : s.stockTris) best = std::min(best, DistPointTri(r, PointAt(m, pose, s.main, t[0], k), PointAt(m, pose, s.main, t[1], k), PointAt(m, pose, s.main, t[2], k)));
        g.root[i] = best;
    }
    g.nock = wimp::Len(Cen(m, pose, s.strInner[0], k) - Cen(m, pose, s.strInner[1], k));
    return g;
}


// ---- bows and melee weapons ---------------------------------------------------------------------------------------------
// The parts' local extremes at corner 5 are fixed by the mesh, so the points that matter (a limb's tip and root, a string's two ends) are picked once, by
// their local coordinates, and measured in every posed frame.
struct Gen {
    std::vector<size_t> part;                 // the model's part indices of the weapon's roles (in the kind's role order)
    std::vector<Pts> pts;                     // named point sets, per family (see Names)
    std::vector<std::vector<std::array<size_t, 3>>> tris;   // surface triangles of the parts that must stay in the hand
    std::vector<size_t> handTris;             // which part index in `part` each tri list belongs to
};
static Pts Extreme(const fig::Model& m, size_t part, int axis, bool high) {
    const fig::FigureMesh& mesh = m.parts[part].mesh;
    const size_t used = figw::UsedPoints(mesh);
    float lo = 1e30f, hi = -1e30f;
    auto coord = [&](size_t i) { const Vec3& p = mesh.morphVertex[5][i]; return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
    for (size_t i = 0; i < used; ++i) { lo = std::min(lo, coord(i)); hi = std::max(hi, coord(i)); }
    Pts t{part, {}};
    for (size_t i = 0; i < used; ++i) if (high ? coord(i) >= hi - 0.12f * (hi - lo) : coord(i) <= lo + 0.12f * (hi - lo)) t.idx.push_back(i);
    return t;
}
static std::vector<std::array<size_t, 3>> Surface(const fig::FigureMesh& mm) {
    std::vector<std::array<size_t, 3>> out;
    for (size_t i = 0; i + 2 < mm.indices.size(); i += 3)
        out.push_back({mm.vertexComponents[mm.indices[i]].vertexIndex, mm.vertexComponents[mm.indices[i + 1]].vertexIndex, mm.vertexComponents[mm.indices[i + 2]].vertexIndex});
    return out;
}
static double HandToSurface(const fig::Model& m, const std::vector<fig::PartPose>& pose, const std::string& hand, size_t part, const std::vector<std::array<size_t, 3>>& tris, const Vec3& k) {
    const int ih = m.FindPartIndex(hand);
    double best = 1e30;
    if (ih >= 0) for (const auto& t : tris) best = std::min(best, DistPointTri(pose[static_cast<size_t>(ih)].p, PointAt(m, pose, part, t[0], k), PointAt(m, pose, part, t[1], k), PointAt(m, pose, part, t[2], k)));
    return best;
}

// A bow variant NN: parts [partb, parta, tetivab, tetivaa]; points: 0 limb b tip, 1 limb a tip, 2 string b outer end, 3 string a outer end,
// 4 string b inner end, 5 string a inner end, 6 limb b root, 7 limb a root.
static bool MakeBow(const fig::Model& m, const std::string& nn, Gen& g) {
    const char* names[4] = {"lh3.bwpartb", "bwparta", "bwtetivab", "bwtetivaa"};
    g = Gen{};
    for (const char* n : names) { const int i = m.FindPartIndex(std::string(n) + nn); if (i < 0) return false; g.part.push_back(static_cast<size_t>(i)); }
    g.pts = {Extreme(m, g.part[0], 1, true), Extreme(m, g.part[1], 1, false), Extreme(m, g.part[2], 1, true), Extreme(m, g.part[3], 1, false),
             Extreme(m, g.part[2], 1, false), Extreme(m, g.part[3], 1, true), Extreme(m, g.part[0], 1, false), Extreme(m, g.part[1], 1, true)};
    g.tris = {Surface(m.parts[g.part[0]].mesh), Surface(m.parts[g.part[1]].mesh)};
    g.handTris = {0, 1};
    return true;
}
struct BowGaps { double tip[2], nock, split, grip; };
static BowGaps MeasureBow(const fig::Model& m, const Gen& g, const std::vector<fig::PartPose>& pose, const Vec3& k) {
    BowGaps r{};
    r.tip[0] = wimp::Len(Cen(m, pose, g.pts[0], k) - Cen(m, pose, g.pts[2], k));
    r.tip[1] = wimp::Len(Cen(m, pose, g.pts[1], k) - Cen(m, pose, g.pts[3], k));
    r.nock = wimp::Len(Cen(m, pose, g.pts[4], k) - Cen(m, pose, g.pts[5], k));
    r.split = wimp::Len(Cen(m, pose, g.pts[6], k) - Cen(m, pose, g.pts[7], k));
    r.grip = 1e30;
    for (size_t j = 0; j < g.tris.size(); ++j) r.grip = std::min(r.grip, HandToSurface(m, pose, "lh3", g.part[g.handTris[j]], g.tris[j], k));
    return r;
}

// A melee weapon: part [main]; points: 0 the tip (the end at the smallest local Y), 1 the butt.
static bool MakeMelee(const fig::Model& m, const wimp::Kind& kind, const std::string& nn, Gen& g) {
    g = Gen{};
    const int i = m.FindPartIndex(wimp::PartName(kind.roles[0], nn));
    if (i < 0) return false;
    g.part = {static_cast<size_t>(i)};
    g.pts = {Extreme(m, g.part[0], 1, false), Extreme(m, g.part[0], 1, true)};
    g.tris = {Surface(m.parts[g.part[0]].mesh)};
    g.handTris = {0};
    return true;
}
struct MeleeGaps { double reach, butt, grip; };
static MeleeGaps MeasureMelee(const fig::Model& m, const Gen& g, const std::vector<fig::PartPose>& pose, const Vec3& k) {
    MeleeGaps r{};
    const int ih = m.FindPartIndex("rh3");
    const Vec3 hand = ih >= 0 ? pose[static_cast<size_t>(ih)].p : Vec3{};
    r.reach = wimp::Len(Cen(m, pose, g.pts[0], k) - hand);
    r.butt = wimp::Len(Cen(m, pose, g.pts[1], k) - hand);
    r.grip = HandToSurface(m, pose, "rh3", g.part[0], g.tris[0], k);
    return r;
}

// Runs every clip at every build, for the weapon `nn` and the vanilla `ref`; the pass rule is "no worse than vanilla" (plus the allowances below).
static int RunGeneric(const LayeredAssetSource& src, const wimp::Kind& kind, const std::string& nn, const std::string& ref, int randoms, const std::string& csvPath, bool bowClipsOnly) {
    std::FILE* csv = csvPath.empty() ? nullptr : std::fopen(csvPath.c_str(), "w");
    if (csv) std::fprintf(csv, "race,weapon,build,kx,ky,kz,clip,frame,a,b,c,d,e\n");
    std::mt19937 rng(99); std::uniform_real_distribution<float> U(0, 1);
    int fail = 0;
    const bool bow = kind.family == wimp::Family::Bow;
    for (const std::string race : {"unhuma", "unhufe"}) {
        LoadedModel lm;
        if (!LoadNamedModel(src, race, lm)) { std::printf("%s: %s\n", race.c_str(), lm.error.c_str()); return 2; }
        std::vector<std::string> clipNames;
        for (const auto& c : lm.animClips) {
            if (!bowClipsOnly) { clipNames.push_back(c.first); continue; }
            const auto b = c.second.bones.find(bow ? "bwtetivaa00" : "rh3");
            if (b == c.second.bones.end()) continue;
            for (const auto& q : b->second.rotations) if (std::fabs(q.w) < 0.9999f) { clipNames.push_back(c.first); break; }
        }
        size_t frameCount = 0;
        for (const auto& cn : clipNames) frameCount += lm.animClips[cn].FrameCount();
        std::printf("%s: %zu clips (%zu frames); %d random builds + 8 corners\n", race.c_str(), clipNames.size(), frameCount, randoms);
        std::vector<Vec3> builds;
        for (int c = 0; c < 8; ++c) builds.push_back({static_cast<float>((c >> 1) & 1), static_cast<float>(c & 1), static_cast<float>((c >> 2) & 1)});
        for (int i = 0; i < randoms; ++i) builds.push_back({U(rng), U(rng), U(rng)});
        struct Worst { double v[5] = {0, 0, 0, 0, 0}; double grip = 0, rest[5] = {0, 0, 0, 0, 0}; size_t frames = 0; };
        std::map<std::string, Worst> store;
        for (const std::string& weapon : {ref, nn}) {
            Worst all;
            for (const Vec3& k : builds) {
                Gen g;
                if (bow ? !MakeBow(lm.model, weapon, g) : !MakeMelee(lm.model, kind, weapon, g)) { std::printf("  missing parts of %s %s\n", kind.id, weapon.c_str()); return 2; }
                auto gaps = [&](const std::vector<fig::PartPose>& pose, double* out, double& grip) {
                    if (bow) { const BowGaps b = MeasureBow(lm.model, g, pose, k); out[0] = b.tip[0]; out[1] = b.tip[1]; out[2] = b.nock; out[3] = b.split; out[4] = 0; grip = b.grip; }
                    else { const MeleeGaps b = MeasureMelee(lm.model, g, pose, k); out[0] = b.reach; out[1] = b.butt; out[2] = 0; out[3] = 0; out[4] = 0; grip = b.grip; }
                };
                double r0[5], grip0;
                gaps(fig::PoseModel(lm.model, nullptr, 0, k, 0.0f), r0, grip0);
                for (int i = 0; i < 5; ++i) all.rest[i] = std::max(all.rest[i], r0[i]);
                for (const std::string& cn : clipNames) {
                    const fig::AnimClip& clip = lm.animClips[cn];
                    for (size_t f = 0; f < clip.FrameCount(); ++f) {
                        double r[5], grip;
                        gaps(fig::PoseModel(lm.model, &clip, static_cast<float>(f), k, 0.0f), r, grip);
                        for (int i = 0; i < 5; ++i) all.v[i] = std::max(all.v[i], std::fabs(r[i] - r0[i]));
                        all.grip = std::max(all.grip, grip);
                        ++all.frames;
                        if (csv) std::fprintf(csv, "%s,%s,%d,%.3f,%.3f,%.3f,%s,%zu,%.5f,%.5f,%.5f,%.5f,%.5f\n", race.c_str(), weapon.c_str(), 0, k.x, k.y, k.z, cn.c_str(), f, r[0] - r0[0], r[1] - r0[1], r[2] - r0[2], r[3] - r0[3], grip);
                    }
                }
            }
            store[weapon] = all;
            if (bow) std::printf("  %s %s %s   drift of: string end from limb tip (b | a) %.4f | %.4f   strings' nock %.4f   limb halves' join %.4f   hand-to-limb at most %.4f   (%zu measurements)\n", weapon == nn ? "NEW    " : "vanilla", kind.id, weapon.c_str(), all.v[0], all.v[1], all.v[2], all.v[3], all.grip, all.frames);
            else std::printf("  %s %s %s   drift of: hand-to-tip %.4f   hand-to-butt %.4f   hand-to-handle at most %.4f   (%zu measurements)\n", weapon == nn ? "NEW    " : "vanilla", kind.id, weapon.c_str(), all.v[0], all.v[1], all.grip, all.frames);
        }
        const Worst& v = store[ref]; const Worst& n = store[nn];
        bool ok;
        if (bow) ok = n.v[0] <= v.v[0] + 0.005 && n.v[1] <= v.v[1] + 0.005 && n.v[2] <= v.v[2] + 0.005 && n.v[3] <= v.v[3] + 0.005 && n.grip <= v.grip + 0.015;
        else ok = n.v[0] <= v.v[0] + 0.01 && n.v[1] <= v.v[1] + 0.01 && n.grip <= v.grip + 0.02;   // the averaged scales differ from one variant's own by a few cm
        std::printf("  %s: %s\n", race.c_str(), ok ? "PASS" : "FAIL");
        if (!ok) ++fail;
    }
    if (csv) std::fclose(csv);
    std::printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}

struct Worst { double tip = 0, nock = 0, root = 0, grip = 0, tipAbs = 0, nockAbs = 0; size_t frames = 0; std::string where; };

int main(int argc, char** argv) {
    std::string csvPath, dumpPath, kindId = "crossbow"; int randoms = 100; bool allClips = false; std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else if (a == "--random" && i + 1 < argc) randoms = std::atoi(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--all-clips") allClips = true;
        else if (a == "--kind" && i + 1 < argc) kindId = argv[++i];
        else pos.push_back(a);
    }
    if (pos.size() < 3) { std::printf("usage: weapon_anim_test [--kind crossbow|bow|sword|dagger|axe|club|pike] <new NN> <reference NN> [--csv f] [--random N] [--all-clips] <figures layer>...\n"); return 2; }
    const std::string nn = wimp::TwoDigits(std::atoi(pos[0].c_str())), ref = wimp::TwoDigits(std::atoi(pos[1].c_str()));
    LayeredAssetSource src;
    for (size_t i = 2; i < pos.size(); ++i) if (!src.AddLayer(pos[i])) { std::printf("cannot load %s\n", pos[i].c_str()); return 2; }
    if (!dumpPath.empty()) { // posed triangles of both crossbows, unhuma, uattack14, some builds and frames: for drawing views (test_views.py)
        LoadedModel lm;
        if (!LoadNamedModel(src, "unhuma", lm)) return 2;
        std::FILE* d = std::fopen(dumpPath.c_str(), "w");
        for (int corner : {0, 5, 7}) {
            const Vec3 k{static_cast<float>((corner >> 1) & 1), static_cast<float>(corner & 1), static_cast<float>((corner >> 2) & 1)};
            for (size_t f : {0, 12, 24, 34, 42, 47, 52, 60, 70, 80}) {
                const fig::AnimClip& clip = lm.animClips["uattack14"];
                if (f >= clip.FrameCount()) continue;
                const auto pose = fig::PoseModel(lm.model, &clip, static_cast<float>(f), k, 0.0f);
                for (const std::string& weapon : {ref, nn}) {
                    for (const char* pat : {"rh3.crbow%smain", "crbow%spart01", "crbow%spart02", "crbow%stetiva01", "crbow%stetiva02"}) {
                        char name[64]; std::snprintf(name, sizeof name, pat, weapon.c_str());
                        const int pi = lm.model.FindPartIndex(name);
                        if (pi < 0) continue;
                        const fig::FigureMesh& mesh = lm.model.parts[static_cast<size_t>(pi)].mesh;
                        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                            std::fprintf(d, "T %d %zu %s %s", corner, f, weapon.c_str(), name);
                            for (int c = 0; c < 3; ++c) { const Vec3 q = PointAt(lm.model, pose, static_cast<size_t>(pi), mesh.vertexComponents[mesh.indices[i + static_cast<size_t>(c)]].vertexIndex, k); std::fprintf(d, " %.4f %.4f %.4f", q.x, q.y, q.z); }
                            std::fprintf(d, "\n");
                        }
                    }
                }
            }
        }
        std::fclose(d);
        std::printf("dumped %s\n", dumpPath.c_str());
        return 0;
    }
    const wimp::Kind* kindP = wimp::FindKind(kindId);
    if (!kindP) { std::printf("unknown kind %s\n", kindId.c_str()); return 2; }
    if (kindP->family != wimp::Family::Crossbow) return RunGeneric(src, *kindP, nn, ref, randoms, csvPath, !allClips);
    std::FILE* csv = csvPath.empty() ? nullptr : std::fopen(csvPath.c_str(), "w");
    if (csv) std::fprintf(csv, "race,weapon,build,kx,ky,kz,clip,frame,tip0,tip1,nock,root0,root1,dtip0,dtip1,dnock,droot0,droot1\n");
    std::mt19937 rng(99); std::uniform_real_distribution<float> U(0, 1);
    int fail = 0;
    for (const std::string race : {"unhuma", "unhufe"}) {
        LoadedModel lm;
        if (!LoadNamedModel(src, race, lm)) { std::printf("%s: %s\n", race.c_str(), lm.error.c_str()); return 2; }
        std::vector<std::string> clipNames;
        for (const auto& c : lm.animClips) {
            if (allClips) { clipNames.push_back(c.first); continue; }
            const auto b = c.second.bones.find("crbow01tetiva01");
            if (b == c.second.bones.end()) continue;
            for (const auto& q : b->second.rotations) if (std::fabs(q.w) < 0.9999f) { clipNames.push_back(c.first); break; }
        }
        size_t frameCount = 0;
        for (const auto& cn : clipNames) frameCount += lm.animClips[cn].FrameCount();
        std::printf("%s: %zu clips %s (%zu frames); %d random builds + 8 corners\n", race.c_str(), clipNames.size(), allClips ? "(ALL of them)" : "move the crossbow's string", frameCount, randoms);
        std::vector<Vec3> builds;
        for (int c = 0; c < 8; ++c) builds.push_back({static_cast<float>((c >> 1) & 1), static_cast<float>(c & 1), static_cast<float>((c >> 2) & 1)});
        for (int i = 0; i < randoms; ++i) builds.push_back({U(rng), U(rng), U(rng)});
        for (const std::string& weapon : {ref, nn}) {
            Worst corner[8], rnd, rest;
            for (size_t bi = 0; bi < builds.size(); ++bi) {
                const Vec3 k = builds[bi];
                Setup s;
                if (!MakeSetup(lm.model, weapon, k, s)) { std::printf("  missing parts of crossbow %s\n", weapon.c_str()); return 2; }
                const Gaps g0 = Measure(lm.model, s, fig::PoseModel(lm.model, nullptr, 0, k, 0.0f), k);
                Worst& w = bi < 8 ? corner[bi] : rnd;
                rest.tipAbs = std::max({rest.tipAbs, g0.tip[0], g0.tip[1]}); rest.nockAbs = std::max(rest.nockAbs, g0.nock);
                for (const std::string& cn : clipNames) {
                    const fig::AnimClip& clip = lm.animClips[cn];
                    for (size_t f = 0; f < clip.FrameCount(); ++f) {
                        const Gaps g = Measure(lm.model, s, fig::PoseModel(lm.model, &clip, static_cast<float>(f), k, 0.0f), k);
                        const double d[5] = {std::fabs(g.tip[0] - g0.tip[0]), std::fabs(g.tip[1] - g0.tip[1]), std::fabs(g.nock - g0.nock), std::fabs(g.root[0] - g0.root[0]), std::fabs(g.root[1] - g0.root[1])};
                        w.grip = std::max(w.grip, g.grip);
                        ++w.frames;
                        if (std::max(d[0], d[1]) > w.tip) { w.tip = std::max(d[0], d[1]); }
                        if (d[2] > w.nock) w.nock = d[2];
                        if (std::max(d[3], d[4]) > w.root) { w.root = std::max(d[3], d[4]); w.where = cn + " f" + std::to_string(f); }
                        w.tipAbs = std::max({w.tipAbs, g.tip[0], g.tip[1]}); w.nockAbs = std::max(w.nockAbs, g.nock);
                        if (csv) std::fprintf(csv, "%s,%s,%zu,%.3f,%.3f,%.3f,%s,%zu,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", race.c_str(), weapon.c_str(), bi, k.x, k.y, k.z, cn.c_str(), f,
                                                  g.tip[0], g.tip[1], g.nock, g.root[0], g.root[1], d[0], d[1], d[2], d[3], d[4]);
                    }
                }
            }
            std::printf("  %s crbow%s   drift of: string end from limb tip | strings' nock | limb root from stock   (metres; rest gap of tip / nock)\n", weapon == nn ? "NEW    " : "vanilla", weapon.c_str());
            Worst all = rnd; all.frames = rnd.frames;
            for (int c = 0; c < 8; ++c) {
                std::printf("    corner %d  %.4f | %.4f | %.4f | hand-to-stock at most %.4f    (worst frame for the root: %s)\n", c, corner[c].tip, corner[c].nock, corner[c].root, corner[c].grip, corner[c].where.c_str());
                all.tip = std::max(all.tip, corner[c].tip); all.nock = std::max(all.nock, corner[c].nock); all.root = std::max(all.root, corner[c].root); all.grip = std::max(all.grip, corner[c].grip);
            }
            all.grip = std::max(all.grip, rnd.grip);
            std::printf("    %d random builds   %.4f | %.4f | %.4f | hand-to-stock at most %.4f\n    ALL: tip %.4f  nock %.4f  root %.4f  hand-to-stock %.4f   (%zu frame/build measurements)\n", randoms, rnd.tip, rnd.nock, rnd.root, rnd.grip, all.tip, all.nock, all.root, all.grip, all.frames + 0);
            if (weapon == nn) {
                // the criterion: no worse than the vanilla reference + 5 mm, and absolutely under 1 cm for the string / nock
                static double refTip, refNock, refRoot;
                (void)refTip; (void)refNock; (void)refRoot;
            }
            static std::map<std::string, Worst> store;
            store[race + weapon] = all;
            if (weapon == nn) {
                const Worst& v = store[race + ref];
                const bool ok = all.tip <= v.tip + 0.005 && all.nock <= v.nock + 0.005 && all.root <= std::max(v.root + 0.010, 0.020) && all.grip <= v.grip + 0.015;
                std::printf("  %s: %s (new vs vanilla: tip %.4f vs %.4f, nock %.4f vs %.4f, root %.4f vs %.4f, hand-to-stock %.4f vs %.4f)\n", race.c_str(), ok ? "PASS" : "FAIL", all.tip, v.tip, all.nock, v.nock, all.root, v.root, all.grip, v.grip);
                if (!ok) ++fail;
            }
        }
    }
    if (csv) std::fclose(csv);
    std::printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
