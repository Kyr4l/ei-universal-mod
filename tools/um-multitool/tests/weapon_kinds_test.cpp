// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O2 -Iviewer -I. tests/weapon_kinds_test.cpp -o weapon_kinds_test && ./weapon_kinds_test [-v] <figures.res> [<more figures.res>...]
// The weapon importer's proof for the bows and the melee weapons (swords, daggers, axes, clubs, spears), the way weapon_import_test.cpp proves the crossbows:
// every vanilla variant of every kind, in both human rigs, is exported as an OBJ in the authoring frame, then
//  F. turned about the vertical axis, moved (and for a melee weapon scaled, which its fit does not guess) and imported again with the automatic fit: the model must land
//     where the vanilla one is (the bows' strings on the anchors; the melee weapon's hand on the rig's hand), the pieces with the same names.
//  A. imported with the fit off: the corner scales the kind measures (a bow's 00 parts; a melee weapon's family averaged) against the variant's own 8 corners: how far the
//     vanilla meshes are from "one mesh times one number per build" (the bows are 1-6 cm off, the melee weapons up to 10 cm on a 2.8 m spear).
//  S. a shape edit does not move the other pieces; a flipped model (the weapon end for end) is put right by `flip`.
//  I. the whole import is the same whatever the model's own orientation about the vertical axis, position and (bows) scale.
// -v lists every variant.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "weapon_import.hpp"

using fig::Vec3;

static int g_fail = 0;
static bool g_verbose = false;

// Positions of a variant's parts in the root ancestor's frame at corner `corner`.
static std::vector<Vec3> World(const wimp::Rig& rig, const wimp::Kind& kind, const fig::FigureMesh& m, const std::string& part, int corner) {
    const Vec3 o = wimp::ChainOrigin(rig, part, kind.rootAncestor, corner);
    std::vector<Vec3> v;
    for (size_t i = 0; i < figw::UsedPoints(m); ++i) v.push_back(m.morphVertex[corner][i] + o);
    return v;
}
static double Hausdorff(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    double worst = 0;
    for (int dir = 0; dir < 2; ++dir) {
        const auto& x = dir ? b : a; const auto& y = dir ? a : b;
        for (const Vec3& p : x) {
            double best = 1e30;
            for (const Vec3& q : y) best = std::min(best, wimp::Len(p - q));
            worst = std::max(worst, best);
        }
    }
    return worst;
}

// An OBJ text turned about the vertical axis, scaled and moved.
static std::string Scrambled(const wimp::ObjFile& obj0, double yaw, double scale, const Vec3& shift, bool lie) {
    wimp::ObjFile o = obj0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    auto rot = [&](Vec3 p) {
        if (lie) p = {p.x, p.z, -p.y};   // the weapon lying down (a quarter turn about X)
        return Vec3{static_cast<float>(c * p.x - s * p.y), static_cast<float>(s * p.x + c * p.y), p.z};
    };
    for (Vec3& p : o.pos) p = rot(p) * static_cast<float>(scale) + shift;
    for (Vec3& n : o.nrm) n = rot(n);
    std::string t;
    char b[200];
    for (const Vec3& p : o.pos) { std::snprintf(b, sizeof b, "v %.7f %.7f %.7f\n", p.x, p.y, p.z); t += b; }
    for (const auto& u : o.uv) { std::snprintf(b, sizeof b, "vt %.7f %.7f\n", u.x, u.y); t += b; }
    for (const Vec3& n : o.nrm) { std::snprintf(b, sizeof b, "vn %.7f %.7f %.7f\n", n.x, n.y, n.z); t += b; }
    for (const auto& ob : o.objects) {
        t += "o " + ob.name + "\n";
        for (const auto& tri : ob.tris) {
            t += "f";
            for (const auto& k : tri) { std::snprintf(b, sizeof b, " %d/%d/%d", k.v + 1, k.t + 1, k.n + 1); t += b; }
            t += "\n";
        }
    }
    return t;
}

static bool Import(const LayeredAssetSource& src, const std::string& text, const wimp::ImportOptions& opt, std::vector<wimp::RaceOutput>& out, std::vector<std::string>& log) {
    wimp::ObjFile obj;
    std::string err;
    if (!wimp::ParseObj(text, obj, err)) { std::printf("  parse failed: %s\n", err.c_str()); return false; }
    log.clear();
    if (!wimp::ImportWeapon(src, obj, opt, out, log, err)) { std::printf("  import failed: %s\n", err.c_str()); for (const auto& l : log) std::printf("      %s\n", l.c_str()); return false; }
    return true;
}

struct Spread { double hd = 0, hdMain = 0; Vec3 shift{}; };

// The new weapon's parts against the vanilla variant's, at corner 5 (the frame is the root ancestor's).
static Spread Against(const wimp::Rig& vanilla, const wimp::Rig& built, const wimp::Kind& kind, const std::string& nnVanilla, const std::string& nnNew, int corner) {
    Spread sp;
    std::vector<Vec3> allA, allB;
    for (const wimp::RoleDef& r : kind.roles) {
        const std::string pa = wimp::PartName(r, nnVanilla), pb = wimp::PartName(r, nnNew);
        fig::FigureMesh a, b;
        if (!vanilla.LoadMesh(pa, a) || !built.LoadMesh(pb, b)) { std::printf("  part missing: %s / %s\n", pa.c_str(), pb.c_str()); ++g_fail; continue; }
        const auto wa = World(vanilla, kind, a, pa, corner), wb = World(built, kind, b, pb, corner);
        sp.hd = std::max(sp.hd, Hausdorff(wa, wb));
        if (g_verbose && Hausdorff(wa, wb) > 0.05) {
            Vec3 la = wa[0], ha = wa[0], lb = wb[0], hb = wb[0];
            for (const Vec3& q : wa) { la = {std::min(la.x, q.x), std::min(la.y, q.y), std::min(la.z, q.z)}; ha = {std::max(ha.x, q.x), std::max(ha.y, q.y), std::max(ha.z, q.z)}; }
            for (const Vec3& q : wb) { lb = {std::min(lb.x, q.x), std::min(lb.y, q.y), std::min(lb.z, q.z)}; hb = {std::max(hb.x, q.x), std::max(hb.y, q.y), std::max(hb.z, q.z)}; }
            std::printf("        %s bb (%.3f %.3f %.3f)-(%.3f %.3f %.3f)   %s bb (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n", pa.c_str(), la.x, la.y, la.z, ha.x, ha.y, ha.z, pb.c_str(), lb.x, lb.y, lb.z, hb.x, hb.y, hb.z);
        }
        allA.insert(allA.end(), wa.begin(), wa.end()); allB.insert(allB.end(), wb.begin(), wb.end());
    }
    if (!allA.empty() && !allB.empty()) sp.shift = wimp::Centroid(allB) - wimp::Centroid(allA);
    return sp;
}

static void Check(bool ok, const char* what, const std::string& where) {
    if (!ok) { std::printf("  FAIL %s: %s\n", what, where.c_str()); ++g_fail; }
}


static bool SamePositions(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// S: a shape edit thins the stretch it names by its factor, leaves every vertex outside the stretch where it was, and moves no other piece.
static void EditTest(const LayeredAssetSource& src, const wimp::Kind& kind, const std::string& nn) {
    wimp::Rig rig; std::string err;
    if (!wimp::LoadRig(src, "unhuma", rig, err)) { ++g_fail; return; }
    wimp::ObjFile obj;
    if (!wimp::ParseObj(wimp::ExportReferenceObj(rig, kind, nn, err), obj, err)) { ++g_fail; return; }
    wimp::RoleMap plain; std::vector<std::string> log;
    const bool ok = kind.family == wimp::Family::Bow ? wimp::FindBowRoles(obj, plain, log, err) : wimp::FindMeleeRoles(obj, plain, log, err);
    if (!ok) { std::printf("  roles: %s\n", err.c_str()); ++g_fail; return; }
    // rig frame exports: parta is -Y, partb +Y, the roles are already named by the objects; the sides are right
    const std::string target = kind.family == wimp::Family::Bow ? "limbs" : "main";
    wimp::ShapeEdit e;
    e.piece = target; e.t0 = 0.3; e.t1 = 0.7; e.factor = 0.6; e.soft = 0.05;
    wimp::RoleMap edited = plain;
    std::vector<std::string> elog;
    wimp::ApplyShapeEdit(edited, kind, false, e, elog);
    const std::string where = std::string(kind.id) + " " + nn;
    for (const auto& kv : plain) {
        const bool targeted = target == "limbs" ? (kv.first == "partb" || kv.first == "parta") : kv.first == "main";
        const wimp::Piece& a = kv.second; const wimp::Piece& b = edited[kv.first];
        if (!targeted) { Check(SamePositions(a.pos, b.pos) && a.tris.size() == b.tris.size(), "an edit moved a piece it was not about", where + " " + kv.first); continue; }
        wimp::PieceAxis ax;
        if (!wimp::ComputePieceAxis(plain, kind, kv.first, false, ax)) { ++g_fail; continue; }
        // untouched vertices: every vertex of the plain piece whose t is outside the stretch (and its soft ends) has a twin in the edited one
        size_t checked = 0, missing = 0;
        const auto pb = wimp::UsedPositions(b);
        for (const Vec3& p : wimp::UsedPositions(a)) {
            const double t = wimp::PieceT(ax, p);
            if (t > e.t0 - e.soft - 0.02 && t < e.t1 + e.soft + 0.02) continue;
            ++checked;
            double best = 1e30;
            for (const Vec3& q : pb) best = std::min(best, wimp::Len(p - q));
            if (best > 1e-5) ++missing;
        }
        Check(missing == 0, "an edit moved vertices outside its stretch", where + " " + kv.first + " (" + std::to_string(missing) + " of " + std::to_string(checked) + ")");
        // the stretch's middle is thinner by the factor: the cross-section's size there before and after
        const double tm = (e.t0 + e.t1) / 2;
        const double sgn = ax.stock ? 1.0 : (wimp::Dot(wimp::Centroid(wimp::UsedPositions(a)) - ax.origin, ax.lat) >= 0 ? 1.0 : -1.0);
        const wimp::Vec3 axis = ax.stock ? ax.a : ax.lat * static_cast<float>(sgn);
        const wimp::Vec3 origin = ax.stock ? ax.centre : ax.origin;
        const double d = (ax.stock ? ax.lo : ax.lo) + tm * (ax.hi - ax.lo);
        wimp::Vec3 c0, c1; double s0 = 0, s1 = 0;
        const wimp::Vec3 hint = std::fabs(axis.z) < 0.9 ? wimp::Vec3{0, 0, 1} : wimp::Vec3{1, 0, 0};
        if (wimp::CutSection(a, axis, origin, d, c0, s0, hint) && wimp::CutSection(b, axis, origin, d, c1, s1, hint)) {
            if (g_verbose) std::printf("  %-14s S %s cross-section %.4f -> %.4f (x%.3f; asked x%.2f)\n", where.c_str(), kv.first.c_str(), s0, s1, s1 / std::max(s0, 1e-9), e.factor);
            Check(std::fabs(s1 / s0 - e.factor) < 0.03, "an edit did not thin the stretch by its factor", where + " " + kv.first);
        } else Check(false, "no cross-section to measure", where + " " + kv.first);
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) { if (std::string(argv[i]) == "-v") g_verbose = true; else files.push_back(argv[i]); }
    if (files.empty()) { std::printf("usage: weapon_kinds_test [-v] <figures.res>...\n"); return 2; }
    LayeredAssetSource src;
    for (const auto& f : files) if (!src.AddLayer(f)) { std::printf("cannot load %s\n", f.c_str()); return 2; }
    std::mt19937 rng(777);
    std::uniform_real_distribution<double> U(0, 1);
    for (const wimp::Kind& kind : wimp::AllKinds()) {
        if (kind.family == wimp::Family::Crossbow) continue;   // weapon_import_test.cpp
        for (const std::string race : {"unhuma", "unhufe"}) {
            wimp::Rig rig; std::string err;
            if (!wimp::LoadRig(src, race, rig, err)) { std::printf("%s\n", err.c_str()); return 2; }
            wimp::Rig baseRig, gameRig;   // gameRig: this race's rig of the base game (a mod's weapons on top, which are not the vanilla ones, are left out of the tests)
            if (!wimp::LoadBaseRig(src, "unhuma", baseRig, err) || !wimp::LoadBaseRig(src, race, gameRig, err)) { std::printf("%s\n", err.c_str()); return 2; }
            const double raceFactor = kind.family == wimp::Family::Melee ? wimp::MeleeRaceFactor(baseRig, gameRig, kind) : 1.0;   // a melee weapon is made this much bigger in the other rig: the exported one already is
            double worstF = 0, worstFshift = 0, worstA = 0, worstI = 0, worstT = 0, worstD = 0;
            std::string worstWhere;
            int tested = 0;
            for (int n = 0; n < 100; ++n) {
                const std::string nn = wimp::TwoDigits(n);
                bool all = true;
                for (const wimp::RoleDef& r : kind.roles) all &= rig.HasPart(wimp::PartName(r, nn)) && gameRig.HasPart(wimp::PartName(r, nn));
                if (!all) continue;
                // a melee family's own 00 root is the ancestor of the others: still a valid vanilla weapon to put back
                const std::string objText = wimp::ExportReferenceObj(rig, kind, nn, err);
                if (objText.empty()) { std::printf("%s\n", err.c_str()); return 2; }
                wimp::ObjFile obj;
                if (!wimp::ParseObj(objText, obj, err)) { std::printf("%s\n", err.c_str()); return 2; }
                ++tested;
                const std::string where = std::string(kind.id) + " " + nn + " " + race;
                // F: scrambled, auto fit
                {
                    const double yaw = (U(rng) * 2 - 1) * 3.14159, scale = kind.family == wimp::Family::Bow ? 0.4 + U(rng) * 2 : 0.6 + U(rng) * 1.2;
                    const Vec3 shift{static_cast<float>(U(rng) * 4 - 2), static_cast<float>(U(rng) * 4 - 2), static_cast<float>(U(rng) * 4 - 2)};
                    wimp::ImportOptions opt;
                    opt.kind = kind.id; opt.number = 90; opt.races = {race};
                    if (kind.family == wimp::Family::Melee) opt.fit.scale = 1.0 / (scale * raceFactor);
                    std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                    if (!Import(src, Scrambled(obj, yaw, scale, shift, false), opt, out, log)) { ++g_fail; continue; }
                    wimp::Rig built;
                    if (!wimp::LoadRigBytes(out[0].mod, out[0].bon, race, built, err)) { std::printf("  %s\n", err.c_str()); ++g_fail; continue; }
                    const Spread sp = Against(rig, built, kind, nn, "90", 5);
                    if (g_verbose) std::printf("  %-14s F fit: Hausdorff %.4f  centroid shift (%+.4f %+.4f %+.4f)\n", where.c_str(), sp.hd, sp.shift.x, sp.shift.y, sp.shift.z);
                    if (g_verbose && sp.hd > 0.05) for (const auto& l : log) std::printf("        %s\n", l.c_str());
                    if (sp.hd > worstF) { worstF = sp.hd; worstWhere = where; }
                    worstFshift = std::max(worstFshift, wimp::Len(sp.shift));
                    // I: another yaw / position / scale: the same bytes up to rounding
                    const double yaw2 = yaw + 1.1, scale2 = kind.family == wimp::Family::Bow ? scale * 1.7 : scale * 0.8;
                    wimp::ImportOptions opt2 = opt;
                    if (kind.family == wimp::Family::Melee) opt2.fit.scale = 1.0 / (scale2 * raceFactor);
                    std::vector<wimp::RaceOutput> out2; std::vector<std::string> log2;
                    if (Import(src, Scrambled(obj, yaw2, scale2, shift * 0.3f, false), opt2, out2, log2)) {
                        wimp::Rig b2;
                        if (wimp::LoadRigBytes(out2[0].mod, out2[0].bon, race, b2, err)) { const Spread s2 = Against(built, b2, kind, "90", "90", 5); worstI = std::max(worstI, s2.hd); }
                    }
                }
                // T: the model lying down, or turned end for end: the melee weapon's tip must still go to -Y (the bows are authored upright)
                if (kind.family == wimp::Family::Melee) {
                    for (int mode = 0; mode < 2; ++mode) {
                        wimp::ObjFile o2 = obj;
                        for (Vec3& p : o2.pos) p = mode == 0 ? Vec3{p.x, p.z, -p.y} : Vec3{-p.x, -p.y, p.z};   // a quarter turn about X; a half turn about Z (end for end)
                        for (Vec3& q : o2.nrm) q = mode == 0 ? Vec3{q.x, q.z, -q.y} : Vec3{-q.x, -q.y, q.z};
                        const std::string t2 = Scrambled(o2, 0.3, 1.0, Vec3{0, 0, 0}, false);
                        wimp::ImportOptions opt;
                        opt.kind = kind.id; opt.number = 90; opt.races = {race}; opt.fit.scale = 1.0 / raceFactor;
                        std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                        if (!Import(src, t2, opt, out, log)) { ++g_fail; continue; }
                        wimp::Rig built;
                        if (!wimp::LoadRigBytes(out[0].mod, out[0].bon, race, built, err)) { ++g_fail; continue; }
                        const Spread sp = Against(rig, built, kind, nn, "90", 5);
                        const double dy = sp.shift.y;
                        if (g_verbose) std::printf("  %-14s T %s: centroid shift y %+.4f\n", where.c_str(), mode == 0 ? "lying" : "end for end", dy);
                        worstT = std::max(worstT, std::fabs(dy));
                    }
                }
                // T: a bow standing up in the model (its length along Z, the upper limb partb): the same bow
                if (kind.family == wimp::Family::Bow) {
                    wimp::ObjFile o2 = obj;
                    for (Vec3& p : o2.pos) p = Vec3{p.x, -p.z, p.y};   // a quarter turn about X: +Y (partb) becomes +Z
                    for (Vec3& q : o2.nrm) q = Vec3{q.x, -q.z, q.y};
                    wimp::ImportOptions opt;
                    opt.kind = kind.id; opt.number = 90; opt.races = {race};
                    std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                    if (Import(src, Scrambled(o2, 0.4, 1.3, Vec3{1, 2, 3}, false), opt, out, log)) {
                        wimp::Rig built;
                        if (wimp::LoadRigBytes(out[0].mod, out[0].bon, race, built, err)) {
                            const Spread sp = Against(rig, built, kind, nn, "90", 5);
                            if (g_verbose) std::printf("  %-14s T standing: Hausdorff %.4f\n", where.c_str(), sp.hd);
                            worstT = std::max(worstT, sp.hd);
                        }
                    }
                }
                // D: a bow with another depth (the limbs' tips and the strings moved 6 cm in X, the limbs by (y / tip)^2 of it): the fit bends it back, the grip on the
                // pivot and the strings on the anchors, so the draw clips keep the halves joined
                if (kind.family == wimp::Family::Bow) {
                    wimp::ObjFile o2 = obj;
                    const double tipY = std::fabs(static_cast<double>(wimp::ChainOrigin(rig, "bwtetivaa00", kind.rootAncestor, 5).y));   // where the strings are tied (0.8 in unhuma)
                    std::vector<char> done(o2.pos.size(), 0);
                    for (const auto& ob : o2.objects) {
                        const bool str = ob.name.find("tetiva") != std::string::npos;
                        for (const auto& t : ob.tris) for (const auto& k : t) {
                            if (done[static_cast<size_t>(k.v)]) continue;
                            done[static_cast<size_t>(k.v)] = 1;
                            Vec3& p = o2.pos[static_cast<size_t>(k.v)];
                            const double w = str ? 1.0 : (p.y / tipY) * (p.y / tipY);
                            p.x += static_cast<float>(0.06 * w);
                        }
                    }
                    wimp::ImportOptions opt;
                    opt.kind = kind.id; opt.number = 90; opt.races = {race};
                    std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                    if (Import(src, Scrambled(o2, 0.7, 1.1, Vec3{0.5f, -1, 2}, false), opt, out, log)) {
                        wimp::Rig built;
                        if (wimp::LoadRigBytes(out[0].mod, out[0].bon, race, built, err)) {
                            const Spread sp = Against(rig, built, kind, nn, "90", 5);
                            if (g_verbose) std::printf("  %-14s D deeper bow: Hausdorff %.4f\n", where.c_str(), sp.hd);
                            worstD = std::max(worstD, sp.hd);
                        }
                    }
                }
                // A: fit off, the kind's corner scales against the variant's own corners
                {
                    wimp::ImportOptions opt;
                    opt.kind = kind.id; opt.number = 90; opt.races = {race}; opt.fit.autoFit = false;
                    std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                    if (!Import(src, objText, opt, out, log)) { ++g_fail; continue; }
                    wimp::Rig built;
                    if (!wimp::LoadRigBytes(out[0].mod, out[0].bon, race, built, err)) { ++g_fail; continue; }
                    double w = 0;
                    for (int c = 0; c < 8; ++c) w = std::max(w, Against(rig, built, kind, nn, "90", c).hd);
                    if (g_verbose) std::printf("  %-14s A corners: worst %.4f\n", where.c_str(), w);
                    worstA = std::max(worstA, w);
                }
            }
            std::printf("%-8s %s  %2d variants:  F fit (corner 5) Hausdorff worst %.4f (%s), centroid %.4f   A the kind's scales vs the variant's own corners %.4f   I invariance %.1e   T tip direction when lying / end for end %.3f   D deeper bow %.4f\n",
                        kind.id, race.c_str(), tested, worstF, worstWhere.c_str(), worstFshift, worstA, worstI, worstT, worstD);
            const double limitF = kind.family == wimp::Family::Bow ? 0.03 : std::string(kind.id) == "pike" ? 0.16 : 0.07;   // a vanilla hand holds the same kind a few cm differently from one variant to the next (a spear 10 cm); bow 02 has its grip 2.4 cm off the rig pivot, which the fit puts on it
            Check(tested > 0, "no vanilla variant found", std::string(kind.id) + " " + race);
            Check(worstF <= limitF, "the fit puts a vanilla weapon back further away than allowed", std::string(kind.id) + " " + race + " " + worstWhere);
            Check(worstT < limitF, "a melee weapon lying down or turned end for end got its tip the wrong way", std::string(kind.id) + " " + race);
            Check(worstD < 0.03, "a bow of another depth was not bent back onto the pivot and the anchors", std::string(kind.id) + " " + race);
            Check(worstI < 2e-4, "the import depends on the model's own turn / place / scale", std::string(kind.id) + " " + race);
        }
    }
    for (const wimp::Kind& kind : wimp::AllKinds()) {
        if (kind.family == wimp::Family::Crossbow) continue;
        for (int n = 0; n < 100; ++n) {
            const std::string nn = wimp::TwoDigits(n);
            bool all = true;
            wimp::Rig rig; std::string err;
            if (!wimp::LoadRig(src, "unhuma", rig, err)) return 2;
            for (const wimp::RoleDef& r : kind.roles) all &= rig.HasPart(wimp::PartName(r, nn));
            if (all && (n == 2 || n == 3)) { EditTest(src, kind, nn); break; }
        }
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
    return g_fail ? 1 : 0;
}
