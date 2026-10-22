// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -O2 -Iviewer tests/figwrite_test.cpp -o $TMPBIN/figwrite_test && /tmp/figwrite_test <figures.res>
// Every FIG8 mesh, .bon and link table of a figures.res must survive parse -> write byte for byte, and the derived
// fields (bounds, radius) must equal what RecomputeBounds gives.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "figure_write.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool Read(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: figwrite_test <figures.res>\n"); return 2; }
    std::vector<uint8_t> bytes;
    if (!Read(argv[1], bytes)) { std::printf("cannot read %s\n", argv[1]); return 2; }
    res::Archive top; std::string err;
    if (!res::ParseArchive(bytes, top, err)) { std::printf("%s\n", err.c_str()); return 2; }
    int oddLinks = 0, meshes = 0, bons = 0, links = 0, empty = 0, boundsOff = 0;
    double worst = 0;
    auto testMesh = [&](const std::string& name, const std::vector<uint8_t>& data) {
        if (data.size() < 4 || std::memcmp(data.data(), "FIG8", 4)) return;
        fig::FigureMesh m;
        if (!fig::ParseFig(data, m)) { std::printf("parse failed: %s\n", name.c_str()); ++g_fail; return; }
        if (m.vertexComponents.empty()) { ++empty; return; }
        ++meshes;
        std::vector<uint8_t> out; std::string e;
        if (!figw::WriteFig(m, out, e)) { std::printf("write failed %s: %s\n", name.c_str(), e.c_str()); ++g_fail; return; }
        if (out != data) { std::printf("not byte-identical: %s (%zu vs %zu bytes)\n", name.c_str(), out.size(), data.size()); ++g_fail; }
        fig::FigureMesh r = m;
        figw::RecomputeBounds(r);
        for (int c = 0; c < 8; ++c) {
            auto d = [&](const fig::Vec3& a, const fig::Vec3& b) -> double { return std::max<double>({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}); };
            double w = std::max({d(r.morphCenter[c], m.morphCenter[c]), d(r.morphMin[c], m.morphMin[c]), d(r.morphMax[c], m.morphMax[c]), (double)std::fabs(r.morphRadius[c] - m.morphRadius[c])});
            worst = std::max(worst, w);
            if (w > 3e-5) { ++boundsOff; break; }
        }
    };
    for (const auto& kv : top.entries) {
        const std::string& n = kv.first;
        const auto& d = kv.second.data;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".fig") == 0) testMesh(n, d);
        else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".mod") == 0) {
            res::Archive sub;
            if (!res::ParseArchive(d, sub, err)) continue;
            for (const auto& s : sub.entries) {
                testMesh(n + ":" + s.first, s.second.data);
                if (s.first + ".mod" == n) { // the link table
                    std::vector<fig::LinkEntry> l;
                    if (fig::ParseLinkTable(s.second.data.data(), s.second.data.size(), l)) {
                        ++links;
                        const auto w = figw::WriteLinkTable(l);
                        if (w != s.second.data) { // nawa1 / nawa2 store their one name without the NUL: same table, other bytes
                            std::vector<fig::LinkEntry> l2;
                            const bool same = fig::ParseLinkTable(w.data(), w.size(), l2) && l2.size() == l.size() && l2[0].child == l[0].child && l2[0].parent == l[0].parent;
                            if (same) ++oddLinks; else { std::printf("link table differs: %s\n", n.c_str()); ++g_fail; }
                        }
                    }
                }
            }
        } else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bon") == 0) {
            res::Archive sub;
            if (res::ParseArchive(d, sub, err)) {
                for (const auto& s : sub.entries) if (s.second.data.size() == 96) {
                    fig::Vec3 off[8]; fig::ParseBonOffsets(s.second.data.data(), 96, off);
                    ++bons; if (figw::WriteBon(off) != s.second.data) { std::printf("bon not identical %s:%s\n", n.c_str(), s.first.c_str()); ++g_fail; }
                }
            } else if (d.size() == 96) {
                fig::Vec3 off[8]; fig::ParseBonOffsets(d.data(), 96, off);
                ++bons; if (figw::WriteBon(off) != d) { std::printf("bon not identical %s\n", n.c_str()); ++g_fail; }
            }
        }
    }
    std::printf("meshes %d (empty skipped %d), bons %d, link tables %d (%d stored without the NUL), bounds off by > 3e-5: %d (worst %.2g)\n", meshes, empty, bons, links, oddLinks, boundsOff, worst);
    CHECK(meshes > 3000);
    CHECK(boundsOff == 0);
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
