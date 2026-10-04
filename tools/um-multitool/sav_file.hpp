// Single-player saves (<game or mod>/saves/saveNN/, docs/file-formats/sav-format.md): the campaign file
// scenario.sav holds the party exactly as a .mp does (items, spells, backpack, members) and then the quest
// variables, after the merchants' and the camp's stocks, then a tail whose first 8 bytes hold the money as a .mp's
// last 8 do (a ^ b). Only the party, the variables and the money are read; the rest is kept as it is.
#pragma once

#include "mp_file.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace sav {

struct Info { // info.sav (not compressed): u32 0x111, u32 3, f32 (a game time?), the allod, the zone, u32 ?, the title
    std::string allod, zone, title;
    bool ok = false;
};

struct Section { // a .mp-like block: a party (or another stored character), its quest variables, its money
    size_t at = 0, end = 0;            // where it is in the raw file (the money included)
    mp::Character party;               // lists, backpack, members, vars, trailer = the money pair (a ^ b)
};
struct Scenario {
    uint32_t head = 0xDEAD, version = 0x74;
    std::vector<uint8_t> raw;          // the whole file, decompressed: everything not decoded is kept from it
    std::vector<Section> sections;     // [0] the party (the one with the "Hero"); written back in file order
};
inline Info ReadInfo(const std::string& folder) {
    Info info;
    std::ifstream in(folder + "/info.sav", std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    if (d.size() < 14) return info;
    size_t p = 12;
    auto str = [&]() { std::string s; while (p < d.size() && d[p]) s += static_cast<char>(d[p++]); ++p; return s; };
    info.allod = str();
    info.zone = str();
    p += 4;
    if (p < d.size()) info.title = str();
    info.ok = true;
    return info;
}

inline void SerializeSection(const mp::Character& c, std::vector<uint8_t>& o) {
    mp::SerializeParty(c, o);
    for (const mp::QuestVar& v : c.vars) {
        o.insert(o.end(), v.name.begin(), v.name.end());
        o.push_back(0);
        uint32_t u;
        std::memcpy(&u, &v.value, 4);
        for (int k = 0; k < 4; ++k) o.push_back(static_cast<uint8_t>(u >> (8 * k)));
    }
    o.push_back(0);
    o.insert(o.end(), c.trailer.begin(), c.trailer.end());
}

inline std::vector<uint8_t> Serialize(const Scenario& s) {
    std::vector<uint8_t> o;
    std::vector<const Section*> order;
    for (const Section& sec : s.sections) order.push_back(&sec);
    std::sort(order.begin(), order.end(), [](const Section* a, const Section* b) { return a->at < b->at; });
    size_t from = 0;
    for (const Section* p : order) {
        o.insert(o.end(), s.raw.begin() + static_cast<long>(from), s.raw.begin() + static_cast<long>(p->at));
        SerializeSection(p->party, o);
        from = p->end;
    }
    o.insert(o.end(), s.raw.begin() + static_cast<long>(from), s.raw.end());
    return o;
}

// The sections are found where a .mp party section reads (and writes back) exactly, each followed by its quest
// variables (name, f32, until an empty name) and 8 bytes of money, as in a .mp.
inline bool Load(const std::string& path, Scenario& s, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    s = Scenario{};
    if (!mp::Unpack(file, s.raw, err, &s.head, &s.version)) return false;
    const std::vector<uint8_t>& raw = s.raw;
    for (size_t o = 0; o + 8 < raw.size(); ++o) {
        mp::Reader r{raw, o};
        mp::Character c;
        std::string e;
        if (!mp::ParseParty(r, c, e) || c.members.empty() || c.members.size() > 16) continue;
        std::vector<uint8_t> back;
        mp::SerializeParty(c, back);
        if (!std::equal(back.begin(), back.end(), raw.begin() + static_cast<long>(o))) continue;
        while (!r.bad) {
            std::string name = r.Str();
            if (name.empty()) break;
            c.vars.push_back({name, r.F32()});
        }
        if (r.bad || r.p + 8 > raw.size()) continue;
        c.trailer.assign(raw.begin() + static_cast<long>(r.p), raw.begin() + static_cast<long>(r.p + 8));
        s.sections.push_back({o, r.p + 8, std::move(c)});
        o = r.p + 7;
    }
    if (s.sections.empty()) { err = "no party found in " + path; return false; }
    std::stable_partition(s.sections.begin(), s.sections.end(), [](const Section& x) { // the party first
        for (const mp::Member& m : x.party.members) if (m.s1 == "Hero") return true;
        return false;
    });
    if (Serialize(s) != raw) { err = "not understood completely (would not be written back the same)"; return false; }
    return true;
}

inline bool Save(const std::string& path, const Scenario& s, std::string& err) {
    const std::vector<uint8_t> f = mp::Pack(Serialize(s), s.head, s.version);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()))) { err = "cannot write " + tmp; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot replace " + path; return false; }
    }
    return true;
}

} // namespace sav
