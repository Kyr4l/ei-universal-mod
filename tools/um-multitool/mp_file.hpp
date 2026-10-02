// Multiplayer character files (<game>/mp/N.mp), reverse-engineered from game.exe (load 0x662DD0 -> reader
// 0x579830 -> decompressor 0x430960; save 0x662AC0 -> writer 0x661C10, members 0x65FDE0, stats 0x528800).
//
//   file   u32 0x114, u32 version (0x74), u32 checksum of the data, u8 1, u32 data size, compressed data
//          (LZ-style: bit 0 a literal byte, bit 1 a match in a 1 KB window; bits LSB first, interleaved with
//          the literal bytes). Written back with literals only, which the game reads (and compresses again
//          when it saves).
//   data   zone\0, u32
//          2 object lists (items, spells): u32 n, n x {u32 id, u32 kind, u16 a, u16 b}
//          their detail blocks, list by list: per object a fixed-size block starting with its id, then
//            u32 0xFFFFFADF, u8 0
//          the backpack: u32 n, n x u32 object id (-1: empty)
//          u32 members, per member: 5 strings (name, figure, "", "", unit name), u32, the unit parameters
//            (0x704 bytes straight from memory), 3 floats (complection), 4 id lists (weapons, belt, the worn
//            armour, spell items), string, u32, u32, u8, u32, u32, string
//          the quest variables: {name\0, f32} ..., "\0", then the money: 2 u32 whose XOR is the amount
// Item kinds are the database's sheets: 0x3002 spell (SpellPrototypes), 0x3004 weapon (Weapons, a = material),
// 0x3005 armour (Armors, a = material), 0x3006 quick item (QuickItems), 0x3008 spell container (QuickItems; the
// spell's object id at +40 of its block, like a weapon's attached spell), 0x3007 quest item (QuestItems).
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace mp {

struct Object { uint32_t id = 0, kind = 0; uint16_t a = 0, b = 0; std::vector<uint8_t> detail; };
struct Member {
    std::string strings[5];       // name, figure, ?, ?, unit name
    uint32_t u0 = 0;
    std::vector<uint8_t> stats;   // 0x704 bytes, see StatField
    float complection[3] = {};
    std::vector<uint32_t> lists[4]; // weapons, belt, worn armour, spell items
    std::string s1, s6;           // "Hero", the prototype name
    uint32_t a = 0, b = 0, d = 0, e = 0;
    uint8_t c = 0;
};
struct QuestVar { std::string name; float value = 0; };
struct Character {
    uint32_t head = 0x114, version = 0x74;
    std::string zone;
    uint32_t v = 0;
    std::vector<Object> lists[2]; // items, spells
    uint8_t listEnd[2] = {};
    std::vector<uint32_t> backpack;
    std::vector<Member> members;
    std::vector<QuestVar> vars;
    std::vector<uint8_t> trailer; // after the variables' end: 8 bytes, changed by the game on each save
};

inline int DetailSize(uint32_t kind) {
    switch (kind) {
    case 0x3002: return 66;
    case 0x3004: return 68;
    case 0x3005: return 108;
    case 0x3006: return 56;
    case 0x3007: return 64;
    case 0x3008: return 56;
    }
    return -1;
}
inline const char* KindName(uint32_t kind) {
    switch (kind) {
    case 0x3002: return "Spell";
    case 0x3004: return "Weapon";
    case 0x3005: return "Armour";
    case 0x3006: return "Quick item";
    case 0x3007: return "Quest item";
    case 0x3008: return "Spell container";
    }
    return "?";
}

// ---- compression ------------------------------------------------------------------------------------
struct BitReader {
    const std::vector<uint8_t>& d;
    size_t p;
    uint32_t buf = 0;
    int n = 0;
    bool bad = false;
    uint32_t Bits(int k) {
        while (n < k) {
            if (p >= d.size()) { bad = true; return 0; }
            buf |= static_cast<uint32_t>(d[p++]) << n;
            n += 8;
        }
        const uint32_t v = buf & ((1u << k) - 1);
        buf >>= k;
        n -= k;
        return v;
    }
    uint8_t Byte() { if (p >= d.size()) { bad = true; return 0; } return d[p++]; }
};
inline int MatchLength(BitReader& b) {
    if (!b.Bits(1)) return b.Bits(1) ? 4 : 2;
    if (!b.Bits(1)) {
        if (b.Bits(1)) return static_cast<int>(b.Bits(2)) + 7;
        return b.Bits(1) ? static_cast<int>(b.Bits(3)) + 0xB : 3;
    }
    if (!b.Bits(1)) return b.Bits(1) ? static_cast<int>(b.Bits(6)) + 0x23 : 5;
    if (!b.Bits(1)) return 6;
    return b.Bits(1) ? static_cast<int>(b.Bits(7)) + 0x63 : static_cast<int>(b.Bits(4)) + 0x13;
}
inline int MatchOffset(BitReader& b) {
    if (b.Bits(1)) return b.Bits(1) ? static_cast<int>(b.Bits(10)) + 0x162 : static_cast<int>(b.Bits(12)) + 0x562;
    if (b.Bits(1)) return b.Bits(1) ? static_cast<int>(b.Bits(8)) + 0x62 : static_cast<int>(b.Bits(6)) + 0x22;
    if (b.Bits(1)) {
        if (b.Bits(1)) return b.Bits(1) ? static_cast<int>(b.Bits(1)) : static_cast<int>(b.Bits(4)) + 0x12;
        return static_cast<int>(b.Bits(2)) + 6;
    }
    return b.Bits(1) ? static_cast<int>(b.Bits(2)) + 2 : static_cast<int>(b.Bits(3)) + 0xA;
}
inline uint32_t Checksum(const std::vector<uint8_t>& raw) {
    uint32_t u = 0;
    for (size_t i = 0; i + 4 <= raw.size(); i += 4) {
        uint32_t w;
        std::memcpy(&w, raw.data() + i, 4);
        const uint32_t x = u ^ w;
        u = x * ((x & 0xFC) | 3);
    }
    return u;
}
// The file's data, decompressed; false (with err) when it is not an .mp file or is damaged.
inline bool Unpack(const std::vector<uint8_t>& file, std::vector<uint8_t>& raw, std::string& err, uint32_t* head = nullptr, uint32_t* version = nullptr) {
    if (file.size() < 17) { err = "too small"; return false; }
    uint32_t h, ver, sum, size;
    std::memcpy(&h, file.data(), 4);
    std::memcpy(&ver, file.data() + 4, 4);
    std::memcpy(&sum, file.data() + 8, 4);
    std::memcpy(&size, file.data() + 13, 4);
    if (file[12] != 1 || size > (1u << 24)) { err = "not a character file"; return false; }
    BitReader b{file, 17};
    raw.clear();
    raw.reserve(size);
    uint8_t win[1024] = {};
    unsigned pos = 0;
    while (raw.size() < size && !b.bad) {
        if (!b.Bits(1)) {
            const uint8_t c = b.Byte();
            raw.push_back(c);
            win[pos] = c;
            pos = (pos + 1) & 0x3FF;
        } else {
            const int len = MatchLength(b);
            const int off = MatchOffset(b);
            for (int i = 0; i < len && raw.size() < size; ++i) {
                const uint8_t c = win[(pos - off - 1) & 0x3FF];
                raw.push_back(c);
                win[pos] = c;
                pos = (pos + 1) & 0x3FF;
            }
        }
    }
    if (b.bad || raw.size() != size) { err = "damaged (data ends early)"; return false; }
    if (Checksum(raw) != sum) { err = "damaged (checksum)"; return false; }
    if (head) *head = h;
    if (version) *version = ver;
    return true;
}
inline std::vector<uint8_t> Pack(const std::vector<uint8_t>& raw, uint32_t head, uint32_t version) {
    std::vector<uint8_t> f(17);
    const uint32_t sum = Checksum(raw), size = static_cast<uint32_t>(raw.size());
    std::memcpy(f.data(), &head, 4);
    std::memcpy(f.data() + 4, &version, 4);
    std::memcpy(f.data() + 8, &sum, 4);
    f[12] = 1;
    std::memcpy(f.data() + 13, &size, 4);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i % 8 == 0) f.push_back(0); // 8 flag bits of 0: literals
        f.push_back(raw[i]);
    }
    return f;
}

// ---- the data -----------------------------------------------------------------------------------------
struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool bad = false;
    uint32_t U32() { uint32_t v = 0; if (p + 4 > d.size()) { bad = true; return 0; } std::memcpy(&v, d.data() + p, 4); p += 4; return v; }
    uint16_t U16() { uint16_t v = 0; if (p + 2 > d.size()) { bad = true; return 0; } std::memcpy(&v, d.data() + p, 2); p += 2; return v; }
    uint8_t U8() { if (p >= d.size()) { bad = true; return 0; } return d[p++]; }
    float F32() { const uint32_t u = U32(); float f; std::memcpy(&f, &u, 4); return f; }
    std::string Str() {
        size_t e = p;
        while (e < d.size() && d[e]) ++e;
        if (e >= d.size()) { bad = true; return ""; }
        std::string s(reinterpret_cast<const char*>(d.data() + p), e - p);
        p = e + 1;
        return s;
    }
    std::vector<uint8_t> Bytes(size_t n) {
        if (p + n > d.size()) { bad = true; return {}; }
        std::vector<uint8_t> v(d.begin() + p, d.begin() + p + n);
        p += n;
        return v;
    }
};
inline bool Parse(const std::vector<uint8_t>& raw, Character& c, std::string& err) {
    Reader r{raw};
    c.zone = r.Str();
    c.v = r.U32();
    for (auto& list : c.lists) {
        const uint32_t n = r.U32();
        if (n > 10000) { err = "bad object count"; return false; }
        list.resize(n);
        for (Object& o : list) { o.id = r.U32(); o.kind = r.U32(); o.a = r.U16(); o.b = r.U16(); }
    }
    for (int l = 0; l < 2; ++l) {
        for (Object& o : c.lists[l]) {
            const int size = DetailSize(o.kind);
            if (size < 0) { err = "unknown object kind " + std::to_string(o.kind); return false; }
            uint32_t id;
            if (r.p + 4 > raw.size()) { err = "ends early"; return false; }
            std::memcpy(&id, raw.data() + r.p, 4);
            if (id != o.id) { err = "object details out of order"; return false; }
            o.detail = r.Bytes(static_cast<size_t>(size));
        }
        if (r.U32() != 0xFFFFFADFu) { err = "missing object list end"; return false; }
        c.listEnd[l] = r.U8();
    }
    const uint32_t nb = r.U32();
    if (nb > 10000) { err = "bad backpack count"; return false; }
    for (uint32_t i = 0; i < nb; ++i) c.backpack.push_back(r.U32());
    const uint32_t nm = r.U32();
    if (nm > 16) { err = "bad member count"; return false; }
    c.members.resize(nm);
    for (Member& m : c.members) {
        for (std::string& s : m.strings) s = r.Str();
        m.u0 = r.U32();
        m.stats = r.Bytes(0x704);
        for (float& f : m.complection) f = r.F32();
        for (auto& l : m.lists) {
            const uint32_t n = r.U32();
            if (n > 1000) { err = "bad member list"; return false; }
            for (uint32_t i = 0; i < n; ++i) l.push_back(r.U32());
        }
        m.s1 = r.Str();
        m.a = r.U32();
        m.b = r.U32();
        m.c = r.U8();
        m.d = r.U32();
        m.e = r.U32();
        m.s6 = r.Str();
    }
    while (!r.bad) {
        std::string name = r.Str();
        if (name.empty()) break;
        c.vars.push_back({name, r.F32()});
    }
    if (r.bad) { err = "ends early"; return false; }
    c.trailer.assign(raw.begin() + r.p, raw.end());
    return true;
}
inline std::vector<uint8_t> Serialize(const Character& c) {
    std::vector<uint8_t> o;
    auto u32 = [&](uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); o.insert(o.end(), b, b + 4); };
    auto u16 = [&](uint16_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); };
    auto f32 = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); u32(u); };
    auto str = [&](const std::string& s) { o.insert(o.end(), s.begin(), s.end()); o.push_back(0); };
    str(c.zone);
    u32(c.v);
    for (const auto& list : c.lists) {
        u32(static_cast<uint32_t>(list.size()));
        for (const Object& x : list) { u32(x.id); u32(x.kind); u16(x.a); u16(x.b); }
    }
    for (int l = 0; l < 2; ++l) {
        for (const Object& x : c.lists[l]) o.insert(o.end(), x.detail.begin(), x.detail.end());
        u32(0xFFFFFADFu);
        o.push_back(c.listEnd[l]);
    }
    u32(static_cast<uint32_t>(c.backpack.size()));
    for (uint32_t id : c.backpack) u32(id);
    u32(static_cast<uint32_t>(c.members.size()));
    for (const Member& m : c.members) {
        for (const std::string& s : m.strings) str(s);
        u32(m.u0);
        o.insert(o.end(), m.stats.begin(), m.stats.end());
        for (float f : m.complection) f32(f);
        for (const auto& l : m.lists) { u32(static_cast<uint32_t>(l.size())); for (uint32_t id : l) u32(id); }
        str(m.s1);
        u32(m.a);
        u32(m.b);
        o.push_back(m.c);
        u32(m.d);
        u32(m.e);
        str(m.s6);
    }
    for (const QuestVar& v : c.vars) { str(v.name); f32(v.value); }
    o.push_back(0);
    o.insert(o.end(), c.trailer.begin(), c.trailer.end());
    return o;
}

inline bool Load(const std::string& path, Character& c, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {}), raw;
    c = Character{};
    if (!Unpack(file, raw, err, &c.head, &c.version)) return false;
    if (!Parse(raw, c, err)) return false;
    if (Serialize(c) != raw) { err = "not understood completely (would not be written back the same)"; return false; }
    return true;
}
inline bool Save(const std::string& path, const Character& c, std::string& err) {
    const std::vector<uint8_t> f = Pack(Serialize(c), c.head, c.version);
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

// ---- named fields of the unit parameters (Member::stats) -------------------------------------------------
// Checked against the game's character screens (2026-10-02). Health and stamina are computed by the game.
struct StatField { int offset; const char* name; const char* tip; };
inline const StatField* StatFields(int& count) {
    static const StatField k[] = {
        {0x0C, "Strength (base)", "Before abilities"},
        {0x10, "Strength", "As the game shows it"},
        {0x14, "Dexterity (base)", "Before abilities"},
        {0x18, "Dexterity", "As the game shows it"},
        {0x1C, "Intelligence (base)", "Before abilities"},
        {0x20, "Intelligence", "As the game shows it"},
        {0x2C, "Actions (base)", "Before abilities"},
        {0x30, "Actions", "As the game shows it"},
        {0x50, "Encumbrance (current)", "The weight carried, as last saved"},
        {0x54, "Encumbrance (max)", ""},
        {0xB4, "Sight", "In units; spells such as Eagle Sight raise it"},
    };
    count = static_cast<int>(sizeof k / sizeof k[0]);
    return k;
}
constexpr int kExpTotal = 0x04, kExpSpent = 0x08; // floats; the free experience is total - spent
// Skills: one byte each. Use/Steal at 0x6C8; the others at 0x6CB + the skill's id (database Skills: melee 0,
// archery 1, 2 unused, elemental 3, sense 4, astral 5).
struct SkillByte { int offset; const char* name; };
inline const SkillByte* Skills(int& count) {
    static const SkillByte k[] = {{0x6CB, "Melee"}, {0x6CC, "Archery"}, {0x6C8, "Use/Steal"}, {0x6CE, "Elemental Magic"},
                                  {0x6CF, "Sensory Magic"}, {0x6D0, "Astral Magic"}};
    count = static_cast<int>(sizeof k / sizeof k[0]);
    return k;
}
// Abilities ("perks"): one byte each, the level 0..3, at 0x6D3 + group; a group is 3 rows of the database's Perks
// sheet (Specialist, Expert, Master): 0 swords .. 6 crossbows, 7-14 the magic schools, 15 night vision,
// 16 health, 17 mana (shown as Stamina), 18 vitality (Regeneration), 19 spirit (Recovery), 20 quickness
// (Actions), 21 lift (Encumbrance), 22 backstab, 23-25 strength, dexterity, intelligence.
constexpr int kPerkBase = 0x6D3, kPerkGroups = 26;
inline const char* PerkGroupName(int g) {
    static const char* const k[kPerkGroups] = {"Sword", "Axe", "Dagger", "Spear", "Bludgeon", "Bow", "Crossbow", "Fire", "Lightning",
                                               "Acid", "Illusion", "Divination", "Enchantments", "Healing", "Domination",
                                               "Night Vision", "Health", "Stamina (mana)", "Regeneration (vitality)",
                                               "Recovery (spirit)", "Actions (quickness)", "Encumbrance (lift)", "Backstab",
                                               "Strength", "Dexterity", "Intelligence"};
    return g >= 0 && g < kPerkGroups ? k[g] : "?";
}
// Money: not in the parameters but in the file's last 8 bytes, two u32 whose XOR is the amount (the game picks a
// new first one on every save).
inline uint32_t Money(const Character& c) {
    if (c.trailer.size() < 8) return 0;
    uint32_t a, b;
    std::memcpy(&a, c.trailer.data(), 4);
    std::memcpy(&b, c.trailer.data() + 4, 4);
    return a ^ b;
}
inline void SetMoney(Character& c, uint32_t money) {
    if (c.trailer.size() < 8) c.trailer.resize(8, 0);
    uint32_t a;
    std::memcpy(&a, c.trailer.data(), 4);
    const uint32_t b = a ^ money;
    std::memcpy(c.trailer.data() + 4, &b, 4);
}
inline float GetF(const std::vector<uint8_t>& b, int at) { float f = 0; if (at + 4 <= static_cast<int>(b.size())) std::memcpy(&f, b.data() + at, 4); return f; }
inline void SetF(std::vector<uint8_t>& b, int at, float f) { if (at + 4 <= static_cast<int>(b.size())) std::memcpy(b.data() + at, &f, 4); }

} // namespace mp
