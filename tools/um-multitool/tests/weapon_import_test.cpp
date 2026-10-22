// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O2 -Iviewer tests/weapon_import_test.cpp -o weapon_import_test && ./weapon_import_test <figures.res> [<mod figures.res>]
// The weapon importer's proof, in both human rigs, on the vanilla crossbows 01, 02 and 03 exported as OBJs (the
// authoring frame) and imported as new weapons through the bytes of the generated .mod / .bon:
//  A. fit off, the corner scales measured on ANOTHER variant: all 8 corners must equal the vanilla part's.
//  B. auto fit with the variant's own tilt: the model must land (nearly) where vanilla has it.
//  C. the same model turned, scaled, moved, with the pieces unnamed, or with the sides' names swapped: the same result.
#include <cmath>
#include <cstdio>
#include <random>

#include "weapon_import.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static wimp::Placement RandomSimilarity(std::mt19937& rng) {
    std::uniform_real_distribution<double> ang(0, 6.2831853), tilt(-0.14, 0.14), sc(0.3, 3.0), sh(-2, 2);
    const double a = ang(rng), t = tilt(rng);
    const double ca = std::cos(a), sa = std::sin(a), ct = std::cos(t), st = std::sin(t);
    wimp::Placement p;
    const double Rz[3][3] = {{ca, -sa, 0}, {sa, ca, 0}, {0, 0, 1}}, Rx[3][3] = {{1, 0, 0}, {0, ct, -st}, {0, st, ct}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { p.R[i][j] = 0; for (int k = 0; k < 3; ++k) p.R[i][j] += Rz[i][k] * Rx[k][j]; }
    p.s = sc(rng);
    p.t = {static_cast<float>(sh(rng)), static_cast<float>(sh(rng)), static_cast<float>(sh(rng))};
    return p;
}

static double CompareAll(const wimp::Rig& ra, const wimp::Rig& rb, const std::string& nnA, const std::string& nnB, double* mainOnly = nullptr) {
    const wimp::Kind& kind = wimp::CrossbowKind();
    double worst = 0;
    if (mainOnly) *mainOnly = 0;
    for (const wimp::RoleDef& r : kind.roles) {
        const std::string pa = wimp::PartName(r, nnA), pb = wimp::PartName(r, nnB);
        fig::FigureMesh a, b;
        if (!ra.LoadMesh(pa, a) || !rb.LoadMesh(pb, b)) { std::printf("  part missing: %s / %s\n", pa.c_str(), pb.c_str()); ++g_fail; continue; }
        const wimp::MeshDiff d = wimp::CompareParts(ra, pa, a, rb, pb, b, kind.rootAncestor);
        if (!d.sameSize) { std::printf("  %s: %zu vs %zu components\n", pb.c_str(), d.compsA, d.compsB); ++g_fail; continue; }
        double w = 0;
        for (int c = 0; c < 8; ++c) w = std::max(w, d.position[c]);
        if (d.uv > 1e-5) { std::printf("  %s: uv differs by %.2e\n", pb.c_str(), d.uv); ++g_fail; }
        if (std::string(r.role) == "main" && mainOnly) *mainOnly = w; else worst = std::max(worst, w);
        if (!mainOnly) worst = std::max(worst, w);
    }
    return worst;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: weapon_import_test <figures.res> [<more figures.res>...]\n"); return 2; }
    LayeredAssetSource src;
    for (int i = 1; i < argc; ++i) if (!src.AddLayer(argv[i])) { std::printf("cannot load %s\n", argv[i]); return 2; }
    std::mt19937 rng(12345);
    for (const std::string race : {"unhuma", "unhufe"}) {
        wimp::Rig rig; std::string err;
        if (!wimp::LoadRig(src, race, rig, err)) { std::printf("%s\n", err.c_str()); return 2; }
        for (const std::string test : {"01", "02", "03"}) {
            const std::string measure = test == "02" ? "03" : "02";
            const std::string objText = wimp::ExportReferenceObj(rig, test, err);
            if (objText.empty()) { std::printf("%s\n", err.c_str()); return 2; }
            wimp::ObjFile obj;
            CHECK(wimp::ParseObj(objText, obj, err));
            wimp::Placement lastPlacement;
            auto run = [&](const wimp::ObjFile& o, wimp::ImportOptions opt, wimp::Rig& made, std::vector<std::string>* logOut = nullptr) {
                std::vector<wimp::RaceOutput> out; std::vector<std::string> log;
                opt.races = {race};
                if (!wimp::ImportCrossbow(src, o, opt, out, log, err)) { std::printf("%s crbow%s: import failed: %s\n", race.c_str(), test.c_str(), err.c_str()); ++g_fail; return false; }
                if (logOut) *logOut = log;
                lastPlacement = out[0].placement;
                if (!wimp::LoadRigBytes(out[0].mod, out[0].bon, race, made, err)) { std::printf("generated rig unreadable: %s\n", err.c_str()); ++g_fail; return false; }
                return true;
            };
            // A: fit off
            {
                wimp::ImportOptions opt; opt.uvMode = wimp::UvMode::Raw; opt.number = 90 + std::atoi(test.c_str()); opt.referenceNumber = measure; opt.fit.autoFit = false;
                wimp::Rig made;
                if (run(obj, opt, made)) {
                    double mainErr; const double w = CompareAll(rig, made, test, wimp::TwoDigits(opt.number), &mainErr);
                    const bool hufeMain = race == "unhufe"; // the stock's rear end is lifted by hand at the big builds (up to 2.4 cm)
                    std::printf("%s crbow%s  A fit off (scales from %s): limbs+strings %.2e, stock %.2e\n", race.c_str(), test.c_str(), measure.c_str(), w, mainErr);
                    CHECK(w < 5e-4); CHECK(mainErr < (hufeMain ? 3e-2 : 5e-4));
                }
            }
            // B: auto fit, tilt and scales of the variant itself
            wimp::Rig baseMade;
            {
                wimp::ImportOptions opt; opt.uvMode = wimp::UvMode::Raw; opt.number = 90 + std::atoi(test.c_str()); opt.referenceNumber = test;
                std::vector<std::string> log;
                if (run(obj, opt, baseMade, &log)) {
                    // how far the fit moved the vanilla model from where vanilla has it
                    double w = 0;
                    for (const auto& v : obj.pos) w = std::max(w, wimp::Len(lastPlacement.Apply(v) - v));
                    std::printf("%s crbow%s  B auto fit (own tilt and scales): moves the vanilla model by at most %.3f (scale %.4f)\n", race.c_str(), test.c_str(), w, lastPlacement.s);
                    for (const auto& l : log) if (l.rfind("fit: hinge", 0) == 0 || l.rfind("fit: string", 0) == 0) std::printf("      %s\n", l.c_str());
                    CHECK(w < 0.02);
                    CHECK(std::fabs(lastPlacement.s - 1.0) < 0.01);
                }
            }
            // D: the reference exported in atlas space and imported in atlas mode: the same UVs back
            {
                const std::string t2 = wimp::ExportReferenceObj(rig, test, err, wimp::UvMode::Texture, 128);
                wimp::ObjFile o3;
                CHECK(wimp::ParseObj(t2, o3, err));
                wimp::ImportOptions opt; opt.uvMode = wimp::UvMode::Texture; opt.uvTextureWidth = 128; opt.number = 90 + std::atoi(test.c_str()); opt.referenceNumber = measure; opt.fit.autoFit = false;
                wimp::Rig made;
                if (run(o3, opt, made)) {
                    double mainErr; const double w = CompareAll(rig, made, test, wimp::TwoDigits(opt.number), &mainErr);
                    std::printf("%s crbow%s  D texture-space UV round trip: limbs+strings %.2e (UVs within 1e-5 or reported above)\n", race.c_str(), test.c_str(), w);
                    CHECK(w < 5e-4);
                }
            }
            // E: shape edits: factor 1 changes nothing; thinning the stock changes only the stock, and reaches all 8 corners (they are derived from it)
            {
                wimp::ImportOptions base; base.uvMode = wimp::UvMode::Raw; base.number = 90 + std::atoi(test.c_str()); base.referenceNumber = measure; base.fit.autoFit = false; base.races = {race};
                std::vector<wimp::RaceOutput> o0, o1, o2, o3; std::vector<std::string> l0, l1, l2, l3;
                wimp::ImportOptions cut = base; cut.edits.push_back({"main", 0.2, 0.9, 0.9999, 0.05});   // cut like the thinned one, but not reshaped: the fair baseline
                wimp::ImportOptions one = base; one.edits.push_back({"main", 0.2, 0.9, 1.0, 0.05});
                wimp::ImportOptions thin = base; thin.edits.push_back({"main", 0.2, 0.9, 0.5, 0.05});
                const bool ok = wimp::ImportCrossbow(src, obj, base, o0, l0, err) && wimp::ImportCrossbow(src, obj, one, o1, l1, err) && wimp::ImportCrossbow(src, obj, thin, o2, l2, err) && wimp::ImportCrossbow(src, obj, cut, o3, l3, err);
                CHECK(ok);
                if (ok) {
                    CHECK(o0[0].mod == o1[0].mod && o0[0].bon == o1[0].bon);   // factor 1: byte for byte the same
                    const wimp::Kind& kind = wimp::CrossbowKind();
                    wimp::Rig r0, r2, r3;
                    CHECK(wimp::LoadRigBytes(o0[0].mod, o0[0].bon, race, r0, err) && wimp::LoadRigBytes(o2[0].mod, o2[0].bon, race, r2, err) && wimp::LoadRigBytes(o3[0].mod, o3[0].bon, race, r3, err));
                    const std::string nn = wimp::TwoDigits(base.number);
                    double worstOther = 0;
                    for (const wimp::RoleDef& r : kind.roles) {
                        fig::FigureMesh a, b;
                        r0.LoadMesh(wimp::PartName(r, nn), a); r2.LoadMesh(wimp::PartName(r, nn), b);
                        if (std::string(r.role) == "main") continue;
                        const wimp::MeshDiff d = wimp::CompareParts(r0, wimp::PartName(r, nn), a, r2, wimp::PartName(r, nn), b, kind.rootAncestor);
                        for (int c = 0; c < 8; ++c) worstOther = std::max(worstOther, d.position[c]);
                    }
                    // the stock: its mean distance from its own axis, before and after, at corner 5 and at corner 0 (it must follow the same factor)
                    fig::FigureMesh a, b;
                    r3.LoadMesh(wimp::PartName(kind.roles[0], nn), a); r2.LoadMesh(wimp::PartName(kind.roles[0], nn), b);
                    // the stock's cross-section (its extent across the unedited axis, in the middle of its length), before and after, at two corners
                    auto across = [&](const fig::FigureMesh& m, int corner, const wimp::Pca& ref) {
                        const size_t used = figw::UsedPoints(m); double lo1 = 1e30, hi1 = -1e30, lo2 = 1e30, hi2 = -1e30;
                        for (size_t i = 0; i < used; ++i) {
                            const wimp::Vec3 d = m.morphVertex[corner][i] - ref.center;
                            if (std::fabs(wimp::Dot(d, ref.axis[0])) > 0.2 * ref.extent[0]) continue;
                            const double u = wimp::Dot(d, ref.axis[1]), w = wimp::Dot(d, ref.axis[2]);
                            lo1 = std::min(lo1, u); hi1 = std::max(hi1, u); lo2 = std::min(lo2, w); hi2 = std::max(hi2, w);
                        }
                        return std::sqrt((hi1 - lo1) * (hi2 - lo2));
                    };
                    auto refAt = [&](const fig::FigureMesh& m, int corner) { std::vector<wimp::Vec3> pts; for (size_t i = 0; i < figw::UsedPoints(m); ++i) pts.push_back(m.morphVertex[corner][i]); return wimp::ComputePca(wimp::UniquePositions(pts)); };
                    const wimp::Pca ref5 = refAt(a, 5), ref0 = refAt(a, 0);
                    const double q5 = across(b, 5, ref5) / across(a, 5, ref5), q0 = across(b, 0, ref0) / across(a, 0, ref0);
                    std::printf("%s crbow%s  E thin the stock to x0.5: other pieces moved by %.1e; the stock's cross-section is x%.2f at corner 5 and x%.2f at corner 0\n", race.c_str(), test.c_str(), worstOther, q5, q0);
                    CHECK(worstOther < 1e-6);
                    CHECK(q5 < 0.65 && q5 > 0.4);   // x0.5 in the window; its soft ends and the sparse rings of the vanilla stocks blur it a little
                    CHECK(std::fabs(q5 - q0) < 0.05);
                }
            }
            // C: invariance
            if (baseMade.mod.entries.empty()) continue;
            for (int variant = 0; variant < 3; ++variant) {
                wimp::ObjFile o2 = obj;
                const wimp::Placement sim = RandomSimilarity(rng);
                for (auto& v : o2.pos) v = sim.Apply(v);
                for (auto& n : o2.nrm) n = wimp::Norm(sim.Dir(n));
                const char* what = "turned + scaled + moved";
                if (variant == 1) { // one object, no names: the five loose pieces are told apart by shape (only for models of exactly 5 islands)
                    size_t islands = 0;
                    for (auto& ob : o2.objects) islands += wimp::SplitIslands(o2, ob.tris).size();
                    if (islands != 5) continue;
                    wimp::ObjFile::Object all; all.name = "weapon";
                    for (auto& ob : o2.objects) all.tris.insert(all.tris.end(), ob.tris.begin(), ob.tris.end());
                    o2.objects = {all};
                    what = "turned + one unnamed object";
                } else if (variant == 2) { // the sides' names swapped
                    for (auto& ob : o2.objects) {
                        for (const auto& pr : std::vector<std::pair<std::string, std::string>>{{"part01", "part02"}, {"tetiva01", "tetiva02"}}) {
                            const size_t i = ob.name.find(pr.first), j = ob.name.find(pr.second);
                            if (i != std::string::npos) ob.name.replace(i, pr.first.size(), pr.second);
                            else if (j != std::string::npos) ob.name.replace(j, pr.second.size(), pr.first);
                        }
                    }
                    what = "turned + sides' names swapped";
                }
                wimp::ImportOptions opt; opt.uvMode = wimp::UvMode::Raw; opt.number = 90 + std::atoi(test.c_str()); opt.referenceNumber = test;
                wimp::Rig made;
                if (!run(o2, opt, made)) continue;
                const double w = CompareAll(baseMade, made, wimp::TwoDigits(opt.number), wimp::TwoDigits(opt.number));
                std::printf("%s crbow%s  C %-30s: differs from the untouched import by %.2e\n", race.c_str(), test.c_str(), what, w);
                CHECK(w < 2e-3);
            }
        }
    }
    // G: the ground / inventory figure rebuilt from the vanilla crossbows' hand parts against the vanilla initwecbNweapon.fig: the same shape, scale, turn and UVs
    // (exact up to the one free number: where the origin sits along the stock, a few cm), the same words.
    {
        wimp::Rig rig; std::string err;
        CHECK(wimp::LoadRig(src, "unhuma", rig, err));
        for (int n = 1; n <= 3; ++n) {
            std::map<std::string, std::vector<uint8_t>> files;
            std::vector<std::string> log;
            const bool ok = wimp::BuildGroundFigure(src, rig, wimp::CrossbowKind(), n, n == 2 ? "03" : "02", 128, files, log, err);
            CHECK(ok);
            if (!ok) { std::printf("  ground: %s\n", err.c_str()); continue; }
            std::vector<uint8_t> vb;
            CHECK(src.ReadFile("initwecb" + std::to_string(n) + "weapon.fig", vb));
            fig::FigureMesh vm, nm;
            CHECK(fig::ParseFig(vb, vm) && fig::ParseFig(files["initwecb" + std::to_string(n) + "weapon.fig"], nm));
            auto pts = [](const fig::FigureMesh& m) { std::vector<fig::Vec3> v; for (size_t i = 0; i < figw::UsedPoints(m); ++i) v.push_back(m.morphVertex[5][i]); return v; };
            const auto a = pts(vm), b = pts(nm);
            // the best shift along the diagonal (the share of the new points that sit on a vanilla point is what counts: the vanilla ground figures of 2 and 3 were touched up by hand, so only cb1 matches point for point;
            // cb3's strings and limbs differ from its hand parts), then the distances that are left
            double bestShare = -1, bestShift = 0, bestMax = 0;
            for (double sh = -0.3; sh <= 0.3; sh += 0.002) {
                size_t on = 0;
                double w = 0;
                for (const auto& p : b) { double d = 1e30; for (const auto& q : a) { const double dx = p.x - 0.70710678 * -sh - q.x, dy = p.y - 0.70710678 * sh - q.y, dz = p.z - q.z; d = std::min(d, std::sqrt(dx * dx + dy * dy + dz * dz)); } if (d < 0.01) ++on; w = std::max(w, d); }
                const double share = static_cast<double>(on) / static_cast<double>(b.size());
                if (share > bestShare) { bestShare = share; bestShift = sh; bestMax = w; }
            }
            double uvDiff = 0;   // the vanilla UVs are all in the new figure's (and the other way round)
            for (const auto& u : vm.uvs) { double d = 1e30; for (const auto& v : nm.uvs) d = std::min(d, std::fabs(double(u.x) - v.x) + std::fabs(double(u.y) - v.y)); uvDiff = std::max(uvDiff, d); }
            std::printf("%s G ground figure of crossbow %d: %zu / %zu points (vanilla / new), %.0f%% of the new points are on a vanilla one (within 1 cm) after sliding %.3f along the stock (the largest distance %.3f), UVs off by %.1e, group %d / %d texture %d / %d\n",
                        "unhuma", n, a.size(), b.size(), 100 * bestShare, bestShift, bestMax, uvDiff, vm.group, nm.group, vm.textureNumber, nm.textureNumber);
            CHECK(bestShare >= (n == 1 ? 0.99 : 0.3) && uvDiff < 1e-4 && vm.group == nm.group && vm.textureNumber == nm.textureNumber);
            CHECK(std::fabs(bestShift) < 0.2);
        }
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
