// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Mutation fuzzer for the file parsers (PENDING #18): takes real files, damages them (bit flips, bytes set to 0 / 0xFF, 32-bit fields set to
// 0 / 0x7FFFFFFF / 0xFFFFFFFF / the file size, truncation, inserted and duplicated pieces) and feeds them to the parser. Build with the sanitizers:
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iviewer -Imapedit -I. -isystem vendor tests/fuzz_parsers.cpp \
//       vendor/stb/stb_image_impl.cpp -o /tmp/fuzz_parsers
//   ASAN_OPTIONS=allocator_may_return_null=1:max_allocation_size_mb=1024:detect_leaks=0 /tmp/fuzz_parsers <target> <seed file> [iterations=2000] [rng seed=1]
// Targets: res (a .res archive: every entry goes to the parser of its extension, nested archives too), entry (one entry, by the seed's extension), mob, mpr, mq, sav, mp, dds, png, mmp, zip, xlsx.
// The input of the call in progress is kept in $FUZZ_DIR/last.bin (default /tmp): after a crash it is the reproducer. std::bad_alloc / length_error
// (a field asking for gigabytes) are counted, not fatal; a call longer than 10 s aborts.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

#include "figure_format.hpp"
#include "item_db.hpp"
#include "unit_db.hpp"
#include "dds_texture.hpp"
#include "png_reader.hpp"
#include "mmp_texture.hpp"
#include "mob_file.hpp"
#include "mpr_file.hpp"
#include "quest_file.hpp"
#include "sav_file.hpp"
#include "mp_file.hpp"
#include "xlsx_reader.hpp"

using Bytes = std::vector<uint8_t>;
static std::string g_dir = "/tmp";

static Bytes ReadAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void WriteAll(const std::string& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

static void Mutate(Bytes& b, std::mt19937_64& rng) {
    if (b.empty()) { b.push_back(static_cast<uint8_t>(rng())); return; }
    auto pick = [&]() -> size_t { // half of the hits in the first 256 bytes (headers, counts), the rest anywhere
        const size_t lim = (rng() & 1) && b.size() > 256 ? 256 : b.size();
        return static_cast<size_t>(rng() % lim);
    };
    const int n = 1 + static_cast<int>(rng() % 4);
    for (int k = 0; k < n; ++k) {
        switch (rng() % 9) {
        case 0: b[pick()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
        case 1: b[pick()] = 0; break;
        case 2: b[pick()] = 0xFF; break;
        case 3: b[pick()] = static_cast<uint8_t>(rng()); break;
        case 4: case 5: { // a 32-bit field
            if (b.size() < 4) break;
            const size_t o = pick() % (b.size() - 3);
            static const uint32_t kv[] = {0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x10000u, 0xFFFFu, 0x7Fu, 0x80u};
            uint32_t v = (rng() % 6 == 0) ? static_cast<uint32_t>(b.size()) : kv[rng() % (sizeof kv / sizeof *kv)];
            if (rng() % 8 == 0) v = static_cast<uint32_t>(b.size() + (rng() % 5) - 2);
            std::memcpy(&b[o], &v, 4);
            break;
        }
        case 6: b.resize(static_cast<size_t>(rng() % (b.size() + 1))); if (b.empty()) b.push_back(0); break; // truncate
        case 7: { // insert some bytes
            const size_t o = static_cast<size_t>(rng() % (b.size() + 1)), c = 1 + static_cast<size_t>(rng() % 16);
            Bytes ins(c); for (auto& x : ins) x = static_cast<uint8_t>(rng());
            b.insert(b.begin() + static_cast<long>(o), ins.begin(), ins.end());
            break;
        }
        default: { // duplicate a piece
            const size_t o = pick(), c = std::min<size_t>(1 + static_cast<size_t>(rng() % 64), b.size() - o);
            Bytes piece(b.begin() + static_cast<long>(o), b.begin() + static_cast<long>(o + c));
            b.insert(b.begin() + static_cast<long>(pick()), piece.begin(), piece.end());
        }
        }
    }
}

// Every entry of an archive to the parser of its extension (nested archives recursively, depth limited).
static void FeedEntry(const std::string& lname, const Bytes& d, int depth);
static void FeedArchive(const Bytes& b, int depth) {
    res::Archive a; std::string err;
    if (!res::ParseArchive(b, a, err) || depth > 3) return;
    for (const auto& e : a.entries) FeedEntry(e.first, e.second.data, depth + 1);
}
static bool Ends(const std::string& s, const char* t) { const size_t n = std::strlen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; }
static void FeedEntry(const std::string& n, const Bytes& d, int depth) {
    if (Ends(n, ".fig")) { fig::FigureMesh m; fig::ParseFig(d, m); }
    else if (Ends(n, ".bon")) { fig::Vec3 o[8]; fig::ParseBonOffsets(d.data(), d.size(), o); }
    else if (Ends(n, ".lnk")) { std::string s; fig::ParseLnk(d.data(), d.size(), s); std::vector<fig::LinkEntry> v; fig::ParseLinkTable(d.data(), d.size(), v); }
    else if (Ends(n, ".anm")) { std::map<std::string, fig::AnimClip> c; std::string e; fig::ParseAnm(d, c, e); }
    else if (Ends(n, ".mmp")) { mmp::Image im; std::string e; mmp::Decode(d, im, e); }
    else if (Ends(n, ".dds") || Ends(n, ".png") || Ends(n, ".bmp") || Ends(n, ".tga")) { mmp::Image im; std::string e; DecodeTextureFile(d, im, e); }
    else if (Ends(n, ".udb")) { units::Database u; units::ParseUnitsUdb(d, u); }
    else if (Ends(n, ".idb")) { items::Database db; std::string e; items::ParseItemsIdb(d, db, e); }
    else if (Ends(n, ".sdb")) { items::Database db; items::ParseSpellsSdb(d, db); }
    else if (Ends(n, ".pdb")) { items::Database db; items::ParsePerksPdb(d, db); }
    else if (Ends(n, ".mp") && !d.empty()) { mp::Character c; std::string e; mp::Parse(d, c, e); }
    else if (Ends(n, ".res") || Ends(n, ".mod") || Ends(n, ".mq") || Ends(n, ".mpr")) FeedArchive(d, depth);
}

static const char* g_stage = "";
static void OnAlarm(int) {
    std::fprintf(stderr, "TIMEOUT (> 10 s) in %s: input kept in %s/last.bin\n", g_stage, g_dir.c_str());
    std::_Exit(124);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: fuzz_parsers <target> <seed file> [iterations] [rng seed]\n"); return 2; }
    const std::string target = argv[1];
    std::string seedName = argv[2]; for (char& c : seedName) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const Bytes seed = ReadAll(argv[2]);
    const int iterations = argc > 3 ? std::atoi(argv[3]) : 2000;
    std::mt19937_64 rng(argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1);
    if (const char* d = std::getenv("FUZZ_DIR")) g_dir = d;
    if (seed.empty()) { std::fprintf(stderr, "cannot read the seed %s\n", argv[2]); return 2; }
    std::signal(SIGALRM, OnAlarm);
    const std::string last = g_dir + "/last.bin", tmp = g_dir + "/fuzz_in." + target;

    int oom = 0, ok = 0;
    for (int it = 0; it <= iterations; ++it) {
        Bytes b = seed;
        if (it > 0) Mutate(b, rng); // iteration 0 = the intact seed (must not report anything)
        WriteAll(last, b);
        g_stage = target.c_str();
        alarm(10);
        try {
            if (target == "res") FeedArchive(b, 0);
            else if (target == "entry") FeedEntry(seedName, b, 1); // one archive entry: the seed file's extension picks the parser (.fig .bon .lnk .anm .mmp .udb .idb .sdb .pdb .mod)
            else if (target == "mp") { mp::Character c; std::string e; mp::Parse(b, c, e); }
            else if (target == "dds" || target == "png") { mmp::Image im; std::string e; DecodeTextureFile(b, im, e); pngread::Decode(b, im, e); }
            else if (target == "mmp") { mmp::Image im; std::string e; mmp::Decode(b, im, e); }
            else {
                WriteAll(tmp, b); // the parsers that read a path
                if (target == "mob") { mob::File f; mob::Load(tmp, f); }
                else if (target == "mpr") { mpr::Map m; std::string e; mpr::Load(tmp, m, e); }
                else if (target == "mq") { quest::Quest q; if (quest::Load(tmp, q)) { for (const auto& n : quest::ListEntries(q)) { Bytes o; quest::ReadEntry(q, n, o); } } }
                else if (target == "sav") { sav::Scenario s; std::string e; sav::Load(tmp, s, e); }
                else if (target == "zip") { ziplib::ZipArchive z(tmp); }
                else if (target == "xlsx") { xlsxlib::Workbook w(tmp); for (const auto& n : w.SheetNames()) w.GetSheet(n); }
                else { std::fprintf(stderr, "unknown target %s\n", target.c_str()); return 2; }
            }
            ++ok;
        } catch (const std::bad_alloc&) { ++oom; }
        catch (const std::length_error&) { ++oom; }
        catch (const std::exception& e) { ++ok; (void)e; } // a parser that reports by exception is fine
        alarm(0);
    }
    std::printf("%s: %d inputs, %d returned or threw an error, %d asked for impossible memory\n", target.c_str(), iterations + 1, ok, oom);
    return 0;
}
