// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Item stats of a character from the database. See mp_stats.hpp.

#include "mp_stats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace mpstats {
namespace fs = std::filesystem;

namespace {

// The columns read from each sheet (row 2 headers), in Row::v order.
const char* const kMaterialCols[] = {"Price", "Weight", "Mana", "Slots", "Durability", "Damage",
                                     "A.Piercing", "A.Slashing", "A.Bludg.", "A.Termal", "A.Chemic", "A.Electric", "A.General"};
enum { MPrice, MWeight, MMana, MSlots, MDurability, MDamage, MAbsorb0 };
const char* const kItemCols[] = {"Price", "Weight", "Size", "Mana", "Slots", "Durability", "Components"};
enum { PPrice, PWeight, PSize, PMana, PSlots, PDurability, PComponents, PKind0 };
const char* const kWeaponCols[] = {"Range", "D.Min", "D.Max"};
enum { WRange = PKind0, WMin, WMax };
const char* const kArmorCols[] = {"M. Absorb.", "M. Piercing", "M. Slashing", "M. Bludg.", "M. Termal", "M. Chemic", "M. Electric", "M. General",
                                  "A. Absorb.", "A. Piercing", "A. Slashing", "A. Bludg.", "A. Termal", "A. Chemic", "A. Electric", "A. General"};
enum { AMain = PKind0, AAdd = PKind0 + 8 };

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

const sheetio::Sheet* FindSheet(const dbmodel::Book& book, const char* name) {
    for (const sheetio::Sheet& s : book) if (s.name == name) return &s;
    return nullptr;
}

// The rows of a sheet (row 4 on = index 0 on, like the game's row numbers) with the listed columns as numbers.
bool ReadSheet(const dbmodel::Book& book, const char* sheet, std::vector<const char*> cols, std::vector<Row>& out, std::string& err) {
    const sheetio::Sheet* s = FindSheet(book, sheet);
    if (!s) { err = std::string("no ") + sheet + " sheet"; return false; }
    int nameCol = 0;
    std::vector<int> at(cols.size(), 0);
    for (int c = 1; c <= s->maxCol; ++c) {
        auto h = s->cells.find({2, c});
        if (h == s->cells.end()) continue;
        const std::string t = Trim(h->second.text);
        if (t == "Name" && !nameCol) nameCol = c;
        for (size_t i = 0; i < cols.size(); ++i) if (!at[i] && t == cols[i]) at[i] = c;
    }
    for (size_t i = 0; i < cols.size(); ++i)
        if (!at[i]) { err = std::string(sheet) + ": no \"" + cols[i] + "\" column"; return false; }
    out.clear();
    for (int row = 4; row <= s->maxRow; ++row) {
        Row r;
        auto n = s->cells.find({row, nameCol});
        if (n != s->cells.end()) r.name = n->second.text;
        r.ok = !r.name.empty();
        for (int c : at) {
            auto it = s->cells.find({row, c});
            char* end = nullptr;
            const std::string t = it == s->cells.end() ? std::string() : Trim(it->second.text);
            const double v = std::strtod(t.c_str(), &end);
            if (t.empty() || end == t.c_str()) r.ok = false;
            r.v.push_back(static_cast<float>(v));
        }
        out.push_back(std::move(r));
    }
    return true;
}

std::string Fmt(float v) {
    char b[48];
    if (std::fabs(v) >= 1e9f || v == std::floor(v)) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos) { // 2.50 -> 2.5
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}
std::string FmtList(const float* v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += (i ? " " : "") + Fmt(v[i]);
    return s;
}
bool Same(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); }

float GetF(const std::vector<uint8_t>& b, int at) { return mp::GetF(b, at); }
void SetF(std::vector<uint8_t>& b, int at, float v) { mp::SetF(b, at, v); }
uint32_t GetU(const std::vector<uint8_t>& b, int at) { uint32_t v = 0; if (at + 4 <= static_cast<int>(b.size())) std::memcpy(&v, b.data() + at, 4); return v; }
void SetU(std::vector<uint8_t>& b, int at, uint32_t v) { if (at + 4 <= static_cast<int>(b.size())) std::memcpy(b.data() + at, &v, 4); }

// The block offsets (docs/file-formats/mp-format.md): +8 durability, +12 its max, +16 energy, +20 its max, +28 weight,
// +32 price, +36 size, +40 spell id, +48 slots (u32); weapon +56 min, +60 spread, +64 range; armour +52 main[7], +80 add[7].
enum { kDur = 8, kDurMax = 12, kMana = 16, kManaMax = 20, kWeight = 28, kPrice = 32, kSize = 36, kSpell = 40, kSlots = 48,
       kMin = 56, kSpread = 60, kRange = 64, kMain = 52, kAdd = 80 };

} // namespace

bool FromBook(const dbmodel::Book& book, Database& db, std::string& err) {
    std::vector<const char*> item(std::begin(kItemCols), std::end(kItemCols));
    std::vector<const char*> weapon = item, armor = item;
    weapon.insert(weapon.end(), std::begin(kWeaponCols), std::end(kWeaponCols));
    armor.insert(armor.end(), std::begin(kArmorCols), std::end(kArmorCols));
    return ReadSheet(book, "Materials", {std::begin(kMaterialCols), std::end(kMaterialCols)}, db.materials, err) &&
           ReadSheet(book, "Weapons", weapon, db.weapons, err) && ReadSheet(book, "Armors", armor, db.armors, err);
}

bool LoadDatabase(const std::string& path, Database& db, std::string& err) {
    dbmodel::Book book;
    if (!dbmodel::LoadBook(path, book, err)) return false;
    return FromBook(book, db, err);
}

std::vector<Item> Update(mp::Character& c, const Database& db, bool apply) {
    std::vector<Item> out;
    for (mp::Object& o : c.lists[0]) {
        const bool weapon = o.kind == 0x3004, armor = o.kind == 0x3005;
        if (!weapon && !armor) continue;
        const std::vector<Row>& protos = weapon ? db.weapons : db.armors;
        Item it;
        it.id = o.id;
        const bool protoOk = o.b < protos.size() && protos[o.b].ok, matOk = o.a < db.materials.size() && db.materials[o.a].ok;
        it.name = (o.b < protos.size() ? protos[o.b].name : "row " + std::to_string(o.b)) + " (" +
                  (o.a < db.materials.size() ? db.materials[o.a].name : "material " + std::to_string(o.a)) + ")";
        if (o.detail.size() < static_cast<size_t>(mp::DetailSize(o.kind))) { it.skipped = "short block"; out.push_back(it); continue; }
        if (!protoOk) { it.skipped = std::string("not in ") + (weapon ? "Weapons" : "Armors"); out.push_back(it); continue; }
        if (!matOk) { it.skipped = "material not in Materials"; out.push_back(it); continue; }
        const std::vector<float>& P = protos[o.b].v;
        const std::vector<float>& M = db.materials[o.a].v;
        std::vector<uint8_t> nb = o.detail;
        auto setF = [&](const char* name, int at, float v) {
            const float old = GetF(o.detail, at);
            if (Same(old, v)) return;
            it.fields.push_back({name, Fmt(old), Fmt(v)});
            SetF(nb, at, v);
        };
        // a max with its current value: the current keeps its share (worn / spent stays worn / spent)
        auto setPair = [&](const char* name, int cur, int max, float v) {
            const float oldMax = GetF(o.detail, max), oldCur = GetF(o.detail, cur);
            if (Same(oldMax, v)) return;
            const float nc = oldMax > 0 ? std::min(v, oldCur * (v / oldMax)) : v;
            it.fields.push_back({name, Fmt(oldCur) + " / " + Fmt(oldMax), Fmt(nc) + " / " + Fmt(v)});
            SetF(nb, cur, nc);
            SetF(nb, max, v);
        };
        setPair("Durability", kDur, kDurMax, P[PDurability] * M[MDurability]);
        setPair("Energy", kMana, kManaMax, P[PMana] + M[MMana]);
        setF("Weight", kWeight, P[PWeight] * M[MWeight]);
        setF("Size", kSize, P[PSize]);
        if (GetU(o.detail, kSpell) == 0xFFFFFFFFu) setF("Price", kPrice, P[PPrice] + P[PComponents] * M[MPrice]); // with a spell: kept (it holds the spell's price)
        const uint32_t slots = static_cast<uint32_t>(static_cast<int>(P[PSlots]) + static_cast<int>(M[MSlots]));
        if (GetU(o.detail, kSlots) != slots) {
            it.fields.push_back({"Slots", std::to_string(GetU(o.detail, kSlots)), std::to_string(slots)});
            SetU(nb, kSlots, slots);
        }
        if (weapon) {
            setF("Damage min", kMin, P[WMin] * M[MDamage]);
            setF("Damage spread", kSpread, P[WMax] * M[MDamage]);
            setF("Range", kRange, P[WRange]);
        } else {
            for (int set = 0; set < 2; ++set) {
                const int base = set ? AAdd : AMain, at = set ? kAdd : kMain;
                float now[7], want[7];
                bool same = true;
                for (int k = 0; k < 7; ++k) {
                    now[k] = GetF(o.detail, at + 4 * k);
                    want[k] = P[base + 1 + k] * M[MAbsorb0 + k] * P[base];
                    same = same && Same(now[k], want[k]);
                }
                if (same) continue;
                it.fields.push_back({set ? "Protection (extra)" : "Protection", FmtList(now, 7), FmtList(want, 7)});
                for (int k = 0; k < 7; ++k) SetF(nb, at + 4 * k, want[k]);
            }
        }
        if (apply) o.detail = std::move(nb);
        out.push_back(std::move(it));
    }
    return out;
}

Counts Count(const std::vector<Item>& items) {
    Counts n;
    for (const Item& i : items) {
        if (!i.skipped.empty()) ++n.skipped;
        else if (!i.fields.empty()) { ++n.items; n.fields += static_cast<int>(i.fields.size()); }
    }
    return n;
}

int RunMpStats(int argc, char* argv[]) {
    std::string dbPath;
    bool write = false, quiet = false;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "mpstats") continue;
        if (a == "--db" && i + 1 < argc) dbPath = argv[++i];
        else if (a == "--write") write = true;
        else if (a == "--quiet" || a == "-q") quiet = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: um-multitool mpstats <file.mp | mp folder>... --db <database.xlsx|.res> [--write] [--quiet]\n"
                         "Recomputes the stats of every weapon and armour a multiplayer character owns from the database\n"
                         "(durability, energy, weight, price, slots, damage, protection), the way the game builds them.\n"
                         "Without --write it only lists the changes. --write saves the files (the first time keeps the original as .bak).\n";
            return 0;
        } else inputs.push_back(a);
    }
    if (dbPath.empty() || inputs.empty()) { std::cerr << "mpstats: give the .mp files (or a folder) and --db (see --help)\n"; return 2; }
    Database db;
    std::string err;
    if (!LoadDatabase(dbPath, db, err)) { std::cerr << "mpstats: " << dbPath << ": " << err << "\n"; return 1; }
    std::vector<std::string> files;
    for (const std::string& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (const auto& e : fs::directory_iterator(in, ec))
                if (e.is_regular_file() && e.path().extension() == ".mp") files.push_back(e.path().string());
        } else files.push_back(in);
    }
    std::sort(files.begin(), files.end());
    int failed = 0, changedFiles = 0;
    for (const std::string& f : files) {
        mp::Character c;
        if (!mp::Load(f, c, err)) { std::cerr << f << ": " << err << "\n"; ++failed; continue; }
        const std::vector<Item> items = Update(c, db, write);
        const Counts n = Count(items);
        std::cout << f << ": " << n.items << " item(s) to update (" << n.fields << " field(s))";
        if (n.skipped) std::cout << ", " << n.skipped << " skipped";
        std::cout << "\n";
        if (!quiet)
            for (const Item& i : items) {
                if (!i.skipped.empty()) { std::cout << "  " << i.name << ": skipped (" << i.skipped << ")\n"; continue; }
                for (const Field& fd : i.fields) std::cout << "  " << i.name << "  " << fd.name << ": " << fd.before << " -> " << fd.after << "\n";
            }
        if (!write || !n.items) continue;
        std::error_code ec;
        if (!fs::exists(f + ".bak", ec)) fs::copy_file(f, f + ".bak", ec);
        if (!mp::Save(f, c, err)) { std::cerr << f << ": not saved: " << err << "\n"; ++failed; continue; }
        ++changedFiles;
    }
    if (write) std::cout << changedFiles << " file(s) written\n";
    return failed ? 1 : 0;
}

} // namespace mpstats
