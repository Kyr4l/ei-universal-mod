// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's network protocol (UDP), as far as it is decoded (_cpr/claude-re/net/README.md): every packet starts
// with a type byte.
//   03 u32                get info        -> 05 u32(echo) u32 key, host name\0, game details
//   04 u32 key u32 cookie u32 0   login   -> 06 u32 our cookie (accepted) / 07 u32 u8 reason (rejected)
//   00 u16 seq u16 ack u32 ackbits u16 id u8 len [len] u8 n [n messages]   updates (the game's messages inside);
//                         an empty one (len 0, n 0) is a keep-alive the server accepts
//   01 u32 client id      disconnect
// Game messages inside updates: a flag byte before every 8, then u8 kind, u16 id, payload (ReadMessages). The kind is the message
// class (0x4142F0's factories): 1 player record (the server's), 5 JOIN (name\0 unit\0 u32 u8 u32): the server
// then lists the player, announces "connected" in the chat and shows its face in the lobby.
#pragma once

#include "mp_file.hpp" // the .mp compression (um-multitool)

#include <cctype>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

struct ServerInfo {
    uint32_t key = 0;
    std::string host;           // the hosting player's name
    std::vector<uint8_t> details; // after the name (players, max ...: not all decoded)
};

class Client {
public:
    enum class State { Idle, AskingInfo, LoggingIn, Accepted, Rejected, Failed };
    State state = State::Idle;
    ServerInfo info;
    uint32_t clientId = 0;
    std::string error;
    std::vector<std::string> log;
    bool joined = false;           // the server lists our player

    // The session as the server's messages describe it (_cpr/claude-re/net/README.md: kinds 1, 2, 3, 8, 9, 11).
    struct Player { std::string name, unit; uint32_t state = 0, unitId = 0, tail = 0; uint8_t flag = 0; };
    std::map<uint16_t, Player> players;          // kind 1, by its object id
    std::map<uint16_t, std::string> quests;      // kind 11: "<quest>.mq"
    std::map<uint16_t, std::string> questStates; // kind 8: "<quest>_1" (available), "_2" (its zone is open)
    std::string chosenQuest;                     // kind 9
    // The host's own record has the flag byte 1 (clients' records 0 and a tail dword: checked with the host and two bots, 2026-10-20); a server without it: any other player.
    bool IsHost(const Player& p) const {
        for (const auto& kv : players) if (kv.second.flag == 1) return p.flag == 1;
        return p.name != name_;
    }
    bool ZoneOpen() const {                      // the chosen quest's zone can be entered (the host is in it)
        for (const auto& kv : questStates) if (kv.second == chosenQuest + "_2") return true;
        if (!chosenQuest.empty()) for (const auto& kv : players) if (IsHost(kv.second) && kv.second.state == 0) return true; // the host is in it
        return false;
    }

    // The JOIN message sent once accepted (from the character's .mp): its name (with the clan tag), its unit name
    // and its u32 after the strings. The u32 1 / u8 1 are the host's own record's values (meaning not known yet).
    // The character: its name (with the clan tag), unit name, hero id (the .mp's u32) and the .mp's content (what is
    // uploaded when entering a quest). The JOIN: name\0 unit\0 u32 state, u8 0, u32 hero id.
    void SetCharacter(const std::string& name, const std::string& unit, uint32_t id, const std::vector<uint8_t>& mpRaw = {}) {
        name_ = name; unit_ = unit; heroId_ = id; mpRaw_ = mpRaw;
        phase_ = Phase::Lobby; worldUnit = 0; heroJoined_ = false; reconnect = false; leavingAt_ = 0;
        tx_.clear(); txOffset_ = 0; unacked_.clear(); resend_.clear(); rxChunks_.clear(); rx_.clear(); rxParsed_ = 0; rxBase_ = 0; got_.clear(); seenMessages_.clear();
        SetJoin(1, 0xFFFFFFFFu); // as the game: first without a hero, then with it
    }
    // Orders our hero (stream message 0x30, read by the server's 0x66FFF0, the same path as a click on the ground):
    // u32 n, n x u32 unit id, u32 flags (?), f32 x, y, z.
    void MoveTo(float x, float y, float z = 0, uint32_t flags = 0) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x30};
        Put32(m, 1); Put32(m, worldUnit); Put32(m, flags);
        for (float f : {x, y, z}) { uint32_t v; std::memcpy(&v, &f, 4); Put32(m, v); }
        StreamMessage(m);
    }
    // The players' heroes, from their 0x41 updates (decoded from live walks, 2026-10-03; the rest not decoded yet):
    // while a unit walks, its update holds "00 00 10", f32 x, f32 y (where it walks to), 2 bytes, u16 x, u16 y (the cell
    // it is in), all in half map units. Between updates it is assumed to walk on at kWalkSpeed.
    struct Pos { float x = 0, y = 0, tx = 0, ty = 0; double at = 0; };
    static constexpr float kWalkSpeed = 1.5f; // map units per second (measured roughly)
    std::map<uint32_t, Pos> unitPos;
    bool Where(uint32_t id, float& x, float& y) const { // the estimated position now
        const auto it = unitPos.find(id);
        if (it == unitPos.end()) return false;
        const Pos& p = it->second;
        const float dx = p.tx - p.x, dy = p.ty - p.y, d = std::sqrt(dx * dx + dy * dy);
        const float k = d > 0 ? std::min(1.0f, static_cast<float>(Now() - p.at) * kWalkSpeed / d) : 1.0f;
        x = p.x + dx * k; y = p.y + dy * k;
        return true;
    }
    // The party the server sends: 0x80 (at the start and every 900-1800 ticks) and 0x83 (when the player leaves the world, with the items picked up)
    // + a length (2 bytes = size * 2, or 4 bytes = size * 2 + 1 from 15000) + the character in the .mp's unpacked format (zone name, u32, party, variables).
    // Candidates found by shape; the owner of the file validates them (mp::Parse and write back the same).
    struct PartyBlob { uint8_t op; std::vector<uint8_t> bytes; double at; };
    std::vector<PartyBlob> partyIn;              // not taken yet (takePartyIn)
    size_t partyBlobs = 0;                       // how many were received
    void ReadParty(const std::vector<uint8_t>& m) {
        for (size_t o = 0; o + 6 < m.size(); ++o) {
            if (m[o] != 0x80 && m[o] != 0x83) continue;
            uint32_t len = 0; size_t hdr = 0;
            const uint16_t v16 = static_cast<uint16_t>(m[o + 1] | (m[o + 2] << 8));
            if (!(v16 & 1)) { len = v16 >> 1; hdr = 3; }
            else if (o + 5 < m.size()) { uint32_t v32; std::memcpy(&v32, &m[o + 1], 4); len = v32 >> 1; hdr = 5; }
            if (len < 60 || o + hdr + len > m.size()) continue;
            const uint8_t* b = &m[o + hdr];
            size_t z = 0; while (z < 40 && z < len && b[z]) ++z;           // the zone name: printable, then 0
            if (z == 0 || z >= 40 || b[z] != 0) continue;
            bool text = true; for (size_t k = 0; k < z; ++k) if (b[k] < 0x20 || b[k] > 0x7E) text = false;
            if (!text) continue;
            partyIn.push_back({m[o], std::vector<uint8_t>(b, b + len), Now()});
            if (partyIn.size() > 8) partyIn.erase(partyIn.begin());
            ++partyBlobs;
            o += hdr + len - 1;
        }
    }
    void ReadUnits(const std::vector<uint8_t>& m) {
        ReadParty(m);
        for (size_t o = 0; o + 20 < m.size(); ++o) if (m[o] == 0x40) if (const size_t e = Creation(m, o)) o = e - 1; // creations first: exact
        // Experience shares (server message 0x18 = CPlayerServer::SendMsg18, sent by CWorldServer::GiveExperience 0x5911C0 on a kill in a session): u32 hero id,
        // f32 the hero's share. In a multiplayer session a kill's experience is split among ALL players' first heroes by sqrt(exp total + accumulator), whoever struck
        // the blow: a 500 exp bot next to a 74 million exp host gets 0.26 % (0.0129 of 5.0 per boar).
        for (size_t o = 0; o + 9 <= m.size(); ++o) {
            if (m[o] != 0x18) continue;
            uint32_t id; std::memcpy(&id, &m[o + 1], 4);
            if (id < 0x3B9ACA00u || id > 0x3B9ACAFFu) continue;
            float v; std::memcpy(&v, &m[o + 5], 4);
            const uint8_t nx = o + 9 < m.size() ? m[o + 9] : 0x41;
            if (!(v >= 0 && v < 1e7f) || !(o + 9 >= m.size() || nx == 0x18 || nx == 0x41 || nx == 0x42 || nx == 0x43 || nx == 0x40 || nx == 0x60 || nx == 0x81 || nx == 0x0B)) continue;
            expShare_[id] += v;
            if (id == worldUnit) { expGained += v; Log("experience share +" + std::to_string(v) + " (total " + std::to_string(expGained) + ")"); }
            o += 8;
        }
        // Our hero's record (message 05, right after the snapshot): our name, then "91" + f32 x, y: the arrival point.
        if (worldUnit && !unitPos.count(worldUnit) && !m.empty() && m[0] == 0x05 && !name_.empty()) {
            const auto it = std::search(m.begin(), m.end(), name_.begin(), name_.end());
            for (size_t k = static_cast<size_t>(it - m.begin()) + name_.size(); it != m.end() && k + 9 <= m.size() && k < static_cast<size_t>(it - m.begin()) + 90; ++k) {
                if (m[k] != 0x91 || m[k + 1]) continue;
                float x, y; std::memcpy(&x, &m[k + 1], 4); std::memcpy(&y, &m[k + 5], 4);
                if (x > 0 && y > 0 && x < 2048 && y < 2048) unitPos[worldUnit] = {x, y, x, y, Now()};
                break;
            }
        }
        // Sub-messages of other kinds (their lengths unknown) sit between them: every 0x41 / 0x42 is tried where it
        // decodes cleanly (ends at the message's end or at another known sub-message).
        for (size_t o = 0; o + 5 <= m.size(); ++o) {
            if ((m[o] == 0x42 || m[o] == 0x43) && o + 5 <= m.size() && (o + 5 == m.size() || m[o + 5] == 0x41 || m[o + 5] == 0x42 || m[o + 5] == 0x43 || m[o + 5] == 0x60)) { // 0x42 / 0x43 u32 id: the unit leaves the view / its body is gone
                uint32_t id; std::memcpy(&id, &m[o + 1], 4);
                if (unitPos.count(id)) { unitPos.erase(id); o += 4; }
                continue;
            }
            if (m[o] != 0x41 || o + 7 > m.size()) continue;
            uint32_t id; std::memcpy(&id, &m[o + 1], 4);
            if (!((id >= 0x100 && id < 0x01000000) || (id >= 0x3B9ACA00u && id <= 0x3B9ACAFFu))) continue;
            const size_t e = UnitUpdate(m, o);
            if (e) o = e - 1;
        }
    }
    // A 0x41 unit update (game.exe: client 0x6687B0 -> unit 0x52EF90, its base 0x50EC30; reads 0x47E000): u32 id, u16
    // mask of the fields sent, then each field in order. Every value is delta coded against the unit's previous one:
    // a bit "unchanged", else for each 4-byte chunk a bit "unchanged" or the 4 bytes, then the rest raw. Bits are read
    // from bytes taken from the stream when needed (low bit first), a new byte at each field. Field 10 is the walk:
    // 3 bytes (the step count = 2nd * 256 + 3rd), 3 u16, 4 f32 (the destination x2 in the 2nd and 3rd), then the steps
    // (u16 each: bits 0-1 / 2-3 = x / y -1, 0, +1 from the last, 3 = an absolute u16 follows: cells of half a unit).
    // Returns the offset after it, 0 when it could not be read.
    // A unit's creation (stream message 0x40, client handler 0x667EC0 = WorldClient::OnUnitCreated), decoded 2026-10-19:
    //   u32 id, u32 kind (0x32 a player's hero, 0x34 a monster / NPC), u32 prototype row (the figure's DB row), u32 flags (0x8000 ..),
    //   3 x f32 complexion (the morph), CString (empty), CString figure ("unhuma"), CString texture, CString label, u16 n + n CStrings (the figure's
    //   part names), CString name (a hero's; empty for monsters), u32 (the prototype row again for monsters), then a full 0x41 body from its mask
    //   (CMapObject::UnpackNetState against empty fields): the unit's whole state, position included.
    struct UnitInfo { uint32_t kind = 0, proto = 0, flags = 0; std::string figure, texture, label, name; float morph[3] = {0, 0, 0}; };
    std::map<uint32_t, double> expShare_; // hero id -> the experience shares seen (0x18 messages)
    double expGained = 0;                 // our hero's experience shares so far
    std::map<uint32_t, UnitInfo> unitInfo_;
    std::map<uint32_t, uint32_t> unitEnemies_; // id -> bit mask of the sides it is hostile to
    std::map<uint32_t, int> unitSide_;         // id -> the .mob's player / side number
    // Is this unit hostile to our hero (its enemy mask has our side's bit)? Unknown (not created yet): false.
    bool Hostile(uint32_t id) const {
        const auto e = unitEnemies_.find(id);
        const auto me = unitSide_.find(worldUnit);
        const int mySide = me == unitSide_.end() ? 0 : me->second;
        return e != unitEnemies_.end() && (e->second >> mySide & 1);
    }
    // Returns the offset after the record, 0 when m[o..] does not decode as one (the caller then moves on).
    size_t Creation(const std::vector<uint8_t>& m, size_t o) {
        size_t p = o + 1;
        auto u32 = [&](uint32_t& v) { if (p + 4 > m.size()) return false; std::memcpy(&v, &m[p], 4); p += 4; return true; };
        auto cstr = [&](std::string& s) { s.clear(); while (p < m.size() && m[p]) { if (m[p] < 0x20 || s.size() > 200) return false; s += static_cast<char>(m[p++]); } if (p >= m.size()) return false; ++p; return true; };
        UnitInfo u; uint32_t id = 0, tail = 0; std::string skip, part;
        if (!u32(id) || !u32(u.kind) || !u32(u.proto) || !u32(u.flags)) return 0;
        const bool player = id >= 0x3B9ACA00u && id <= 0x3B9ACAFFu;
        if (u.kind < 0x32 || u.kind > 0x34 || u.proto > 4096 || (!player && (id < 2 || id >= 0x01000000))) return 0;
        if (p + 12 > m.size()) return 0;
        for (float& f : u.morph) { std::memcpy(&f, &m[p], 4); p += 4; if (!(f >= -8 && f <= 8)) return 0; }
        if (!cstr(skip) || !cstr(u.figure) || !cstr(u.texture) || !cstr(u.label)) return 0;
        if (u.figure.empty() || p + 2 > m.size()) return 0;
        const int n = m[p] | m[p + 1] << 8; p += 2;
        if (n > 64) return 0;
        for (int i = 0; i < n; ++i) if (!cstr(part)) return 0;
        if (!cstr(u.name) || !u32(tail)) return 0;
        // the body: UnitUpdate wants "41 id mask ..."
        std::vector<uint8_t> b{0x41}; b.insert(b.end(), m.begin() + o + 1, m.begin() + o + 5); b.insert(b.end(), m.begin() + p, m.end());
        auto plausibleNext = [&](size_t q) { const uint8_t nx = q < m.size() ? m[q] : 0x41; return q >= m.size() || nx == 0x40 || nx == 0x41 || nx == 0x42 || nx == 0x43 || nx == 0x60 || nx == 0x81 || nx == 0x0B || nx == 0x18 || nx == 0x0E || nx == 0x04; };
        dryRun_ = true; const size_t e0 = UnitUpdate(b, 0); dryRun_ = false;
        if (!e0 || !plausibleNext(p + (e0 - 5))) {
            // The body did not decode (a stationary monster's order section, seen 2026-10-19, is shaped differently than the walking ones). Recover what is needed
            // without it: the next record's header marks the end; the tail is section 11 (hostility mask, side) when it is the last section; the position sits right
            // after the mask (section 0 unchanged, section 2's first bits, then f32 x, f32 y).
            size_t q = p + 20; bool found = false;
            auto validId = [&](size_t at) { if (at + 5 > m.size()) return false; uint32_t v; std::memcpy(&v, &m[at + 1], 4); return (v >= 2 && v < 0x01000000u) || (v >= 0x3B9ACA00u && v <= 0x3B9ACAFFu); };
            for (; q <= m.size() && q < p + 400; ++q) { // the first place after which another sub-message starts and before which section 11 sits as the record's tail
                if (m[q - 6] == 0 || (m[q - 6] & 0x0F) != 0x08 || m[q - 1] > 31) continue;
                const bool atEnd = q == m.size();
                const bool next = !atEnd && (((m[q] == 0x40 || m[q] == 0x41 || m[q] == 0x42 || m[q] == 0x43) && validId(q)) || m[q] == 0x60 || m[q] == 0x81);
                if (atEnd || next) { found = true; break; }
            }
            if (!found) return 0;
            if (q < p + 20 || q - p > 400) return 0;
            const int mask = m[p] | m[p + 1] << 8;
            if (!(mask & 0x800) || (mask & 0x3000) || (mask & 1) == 0 || m[q - 6] == 0 || (m[q - 6] & 0x0F) != 0x08) return 0;
            const uint8_t side = m[q - 1]; uint32_t enemies; std::memcpy(&enemies, &m[q - 5], 4);
            if (side > 31) return 0;
            unitInfo_[id] = u; unitSide_[id] = side; unitEnemies_[id] = enemies;
            if (u.flags & 0x80000u) { dead_.insert(id); unitPos.erase(id); }
            else if (m[p + 2] == 0x01 && (mask & 4) && p + 12 < m.size()) { float x, y; std::memcpy(&x, &m[p + 4], 4); std::memcpy(&y, &m[p + 8], 4); if (x > 0 && y > 0 && x < 4096 && y < 4096) unitPos[id] = {x, y, x, y, Now()}; }
            return q;
        }
        const size_t e = UnitUpdate(b, 0);
        if (!e) return 0;
        unitInfo_[id] = u;
        if (u.flags & 0x80000u) { dead_.insert(id); unitPos.erase(id); } // flags bit 0x80000: a body (the game's own record flag)
        return p + (e - 5);
    }
    struct Field { std::vector<uint8_t> b; size_t at = 0; };
    std::map<std::pair<uint32_t, int>, Field> unitFields_;
    std::map<uint32_t, int> unitHp_, unitMana_, unitHpMax_, unitManaMax_;
    std::map<uint32_t, int> unitPace_;   // id -> pace (section 6)
    std::map<uint32_t, uint32_t> unitTarget_; // who attacks whom (field 10 mode 3)
    std::map<uint32_t, int> unitAction_; // field 8's action code: 0xFE idle, 0x07 often in a fight
    double hostHurtAt_ = -99;
    std::set<uint32_t> dead_;
    bool dryRun_ = false;   // UnitUpdate decodes and reports its end without storing anything
    size_t UnitUpdate(const std::vector<uint8_t>& m, size_t o) {
        std::map<int, Field> work;               // the fields as they would become: kept only if the update reads cleanly
        std::vector<std::function<void()>> commit; // and its effects
        size_t p = o + 5;
        int bitMask = 0x80, bitByte = 0;
        bool bad = false;
        auto raw = [&](size_t n) { if (p + n > m.size()) { bad = true; return static_cast<const uint8_t*>(nullptr); } const uint8_t* r = m.data() + p; p += n; return r; };
        auto bit = [&]() { bitMask *= 2; if (bitMask > 0x80) { bitMask = 1; const uint8_t* b = raw(1); bitByte = b ? *b : 0; } return (bitByte & bitMask) != 0; };
        uint32_t id; std::memcpy(&id, m.data() + o + 1, 4);
        const uint8_t* mk = raw(2);
        if (!mk) return 0;
        const int mask = mk[0] | mk[1] << 8;
        auto delta = [&](Field& f, size_t n) -> const uint8_t* {
            if (f.b.size() < f.at + n) f.b.resize(f.at + n);
            uint8_t* v = f.b.data() + f.at;
            f.at += n;
            if (bit()) return v;
            for (size_t k = 0; k < n / 4; ++k) if (!bit()) { const uint8_t* r = raw(4); if (!r) return v; std::memcpy(v + 4 * k, r, 4); }
            if (n % 4) { const uint8_t* r = raw(n % 4); if (r) std::memcpy(v + n - n % 4, r, n % 4); }
            return v;
        };
        static const std::map<int, std::vector<size_t>> kSizes = {{0, {24}}, {1, {12}}, {2, {1, 12, 16}}, {4, {264}}, {5, {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}},
                                                                  {6, {4, 4, 4}}, {7, {1}}, {8, {1, 1, 4, 1}}, {11, {4, 1, 4}}, {12, {20}}, {13, {4}}};
        for (int k = 0; k <= 13 && !bad; ++k) {
            if (!(mask & (1 << k))) continue;
            if (!work.count(k)) { const auto it = unitFields_.find({id, k}); work[k] = it != unitFields_.end() ? it->second : Field{}; }
            Field& f = work[k];
            f.at = 0;
            bitMask = 0x80;
            const auto sz = kSizes.find(k);
            if (sz != kSizes.end()) {
                int elem = -1;
                for (size_t n : sz->second) {
                    ++elem;
                    const uint8_t* v = delta(f, n);
                    // field 11 (CUnit::WriteNetState chunk 0xB): u32 mask of the sides this unit is hostile to (+0x248 & +0x244), u8 side (+0x250), u32 owner id
                    // section 6 (CUnit::ReadNetState): u32 +0x264, u32 +0x268 = the unit's pace / attack mode (SetAttackMode: 0 crawl, 1 sneak, 2 walk, 3 run), u32 +0x26C
                    if (k == 6 && !bad && elem == 1) { uint32_t pc; std::memcpy(&pc, v, 4); commit.push_back([this, id, pc] { unitPace_[id] = static_cast<int>(pc); }); }
                    if (k == 11 && !bad && elem == 0) { uint32_t mask; std::memcpy(&mask, v, 4); commit.push_back([this, id, mask] { unitEnemies_[id] = mask; }); }
                    if (k == 11 && !bad && elem == 1) commit.push_back([this, id, side = v[0]] { unitSide_[id] = side; });
                    // section 0 (24 bytes), as 16-bit elements: [1] HP, [2] HP max, [3] mana, [4] mana max (the test hero: 38 / 38, 57 / 57; the host 1569 / 1569, 1255 / 1255).
                    // (Up to 0.1.2 the HP was read from [2]: the maximum, which never falls.)
                    if (k == 0 && !bad) {
                        auto s16 = [&](int o) { return static_cast<int16_t>(v[o] | v[o + 1] << 8); };
                        commit.push_back([this, id, hp = s16(2), hpMax = s16(4), mana = s16(6), manaMax = s16(8)] {
                            unitMana_[id] = mana; unitManaMax_[id] = manaMax; unitHpMax_[id] = hpMax;
                            auto h = unitHp_.find(id);
                            if (hp <= 0 && h != unitHp_.end() && h->second > 0) { dead_.insert(id); unitPos.erase(id); }
                            if (h != unitHp_.end() && hp < h->second && id == HostUnit()) hostHurtAt_ = Now();
                            if (h != unitHp_.end() && hp < h->second && id == worldUnit) selfHurtAt_ = Now();
                            unitHp_[id] = hp;
                        });
                    }
                    if (k == 8 && n == 1 && sz->second.size() == 4 && !bad && f.at == 2) commit.push_back([this, id, a = v[0]] { // the action code: 0xFE idle, 0x07 fight, 0xFF dead (seen when the host kills a monster)
                        unitAction_[id] = a;
                        if (a == 0xFF && id < 0x01000000u) { dead_.insert(id); unitPos.erase(id); }
                    });
                    if (k == 2 && !bad && n == 12) { float x, y; std::memcpy(&x, v, 4); std::memcpy(&y, v + 4, 4); if (x > 0 && y > 0 && x < 4096 && y < 4096) commit.push_back([this, id, x, y] { unitPos[id] = {x, y, x, y, Now()}; }); }
                    if (k == 1 && !bad) { float x, y; std::memcpy(&x, v, 4); std::memcpy(&y, v + 4, 4); if (x > 0 && y > 0 && x < 2048 && y < 2048) commit.push_back([this, id, x, y] { unitPos[id] = {x, y, x, y, Now()}; }); }
                }
            } else if (k == 3) {
                const int n = *delta(f, 1);
                raw(static_cast<size_t>(n) * 5);
            } else if (k == 9) {
                while (const uint8_t* c = raw(1)) if (!*c) break;
            } else if (k == 10) {
                const int mode = *delta(f, 1); // 0 none, 2 move to a point, 3 attack a unit
                const int hi = *delta(f, 1), lo = *delta(f, 1);
                for (int q = 0; q < 3; ++q) { if (f.b.size() < f.at + 2) f.b.resize(f.at + 2); if (!bit()) { if (const uint8_t* r = raw(2)) std::memcpy(f.b.data() + f.at, r, 2); } f.at += 2; }
                float fl[4];
                for (float& v : fl) std::memcpy(&v, delta(f, 4), 4);
                uint32_t target; std::memcpy(&target, &fl[1], 4);
                commit.push_back([this, id, mode, target] { if (mode == 3) unitTarget_[id] = target; else unitTarget_.erase(id); });
                int cx = -1, cy = -1;
                for (int st = 0; st < hi * 256 + lo && !bad; ++st) {
                    const uint8_t* c = raw(2);
                    if (!c) break;
                    const int code = c[0] | c[1] << 8;
                    auto axis = [&](int bits, int& v) {
                        if (bits == 3) { if (const uint8_t* a = raw(2)) v = a[0] | a[1] << 8; }
                        else if (v >= 0) v += bits - 1;
                    };
                    axis(code & 3, cx);
                    axis((code >> 2) & 3, cy);
                    if (st == 0 && cx >= 0 && cy >= 0) { // where it is now, where it walks to (unknown when only the creation sent it)
                        const float x = (cx + 0.5f) / 2, y = (cy + 0.5f) / 2;
                        const bool dest = fl[1] > 0 && fl[2] > 0;
                        commit.push_back([this, id, x, y, tx = dest ? fl[1] / 2 : x, ty = dest ? fl[2] / 2 : y] { unitPos[id] = {x, y, tx, ty, Now()}; });
                    }
                }
            }
        }
        if (bad) return 0;
        if (p > m.size()) return 0;
        if (dryRun_) return p;
        for (auto& kv : work) unitFields_[{id, kv.first}] = std::move(kv.second);
        for (auto& c : commit) c();
        return p;
    }    // An order on a target object (stream message 0x31, server handler 0x670190 -> 0x5D3C30 with order 4): u32 n,
    // n x u32 unit id, u32 ?, u32 the target's id, u32 ?. On a monster: attack it (to confirm).
    void Attack(uint32_t target, uint32_t a = 0, uint32_t c = 0) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x31};
        Put32(m, 1); Put32(m, worldUnit); Put32(m, a); Put32(m, target); Put32(m, c);
        StreamMessage(m);
    }
    // Cast orders (client senders CPlayerClient::SendMsg33 0x66AC70 / SendMsg32 0x66ACD0 / SendMsg33_UseWeapon 0x66AD40; server handlers 0x670820 / 0x6708D0,
    // AiLogicPlayer::CastOn / CastAt -> CUnitServer::QueueCastOn / QueueCastAt), decoded 2026-10-19:
    //   0x33  u32 spell, u32 caster unit id, u32 target object id, u8 flag (shift / queue)      cast on a target
    //   0x32  u32 spell, u32 caster unit id, f32 x, y, z, u8 flag                                cast at a point
    // The spell is the spell object's id from the party data (the .mp's spell list); a quick item (potion 5 / scroll 8 on the belt) is `slot | 0x80000000`.
    void CastOn(uint32_t spell, uint32_t target, bool queue = false) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x33};
        Put32(m, spell); Put32(m, worldUnit); Put32(m, target); m.push_back(queue ? 1 : 0);
        StreamMessage(m);
    }
    void CastAt(uint32_t spell, float x, float y, float z = 0, bool queue = false) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x32};
        Put32(m, spell); Put32(m, worldUnit);
        for (float f : {x, y, z}) { uint32_t v; std::memcpy(&v, &f, 4); Put32(m, v); }
        m.push_back(queue ? 1 : 0);
        StreamMessage(m);
    }
    void UseQuickSlot(int slot, uint32_t target) { CastOn(0x80000000u | static_cast<uint32_t>(slot), target); }
    // The hero's pace (stream message 0x35, server 0x6709D0 -> 0x555270: the unit's +0x268): u32 unit id, u8 mode
    // 0 crawl, 1 sneak, 2 walk, 3 run (um.dll's names).
    int pace = 3;
    bool matchPace = false;     // follow the host's pace (read from its section 6) instead of always running
    float leash = 25.0f;        // past this distance from the host the fight is dropped and the bot walks back
    int priority = 0;           // 0 heal the player first, 1 focus the player's target, 2 guard the player (only fights what is near the player)
    void SetPace(int mode) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x35};
        Put32(m, worldUnit);
        m.push_back(static_cast<uint8_t>(mode));
        StreamMessage(m);
        paceSent_ = mode;
    }
    // A chat line from another player ("^1Name^7: bot stay"): commands for the bot, after "bot" or its first name.
    // stay | wait | hold: stand where it is; follow | come: follow the host again; passive | defensive | aggressive: the engagement.
    void ChatCommand(const std::string& line) {
        std::string t;
        for (size_t i = 0; i < line.size(); ++i) { if (line[i] == '^' && i + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[i + 1]))) { ++i; continue; } t += line[i]; }
        const size_t colon = t.find(": ");
        if (colon == std::string::npos) return;
        const std::string speaker = t.substr(0, colon);
        if (speaker == name_ || name_.empty()) return;
        std::string msg = t.substr(colon + 2);
        for (char& c : msg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string first = name_.substr(0, name_.find_first_of(" |"));
        for (char& c : first) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& who : {std::string("bot"), first}) {
            if (who.empty() || msg.compare(0, who.size(), who) != 0 || msg.size() <= who.size() || !(msg[who.size()] == ' ' || msg[who.size()] == ',' || msg[who.size()] == ':')) continue;
            std::string cmd = msg.substr(who.size() + 1);
            while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == ',')) cmd.erase(cmd.begin());
            while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '.' || cmd.back() == '!')) cmd.pop_back();
            if (cmd == "stay" || cmd == "wait" || cmd == "hold") { chatStay_ = true; Log("chat command: stay"); }
            else if (cmd == "follow" || cmd == "come") { chatStay_ = false; Log("chat command: follow"); }
            else if (cmd == "passive") { chatEngagement_ = 0; Log("chat command: passive"); }
            else if (cmd == "defensive") { chatEngagement_ = 1; Log("chat command: defensive"); }
            else if (cmd == "aggressive" || cmd == "attack") { chatEngagement_ = 2; Log("chat command: aggressive"); }
            else if (cmd == "reset") { chatStay_ = false; chatEngagement_ = -1; Log("chat command: back to the settings"); }
            else Log("chat command not understood: " + cmd);
            return;
        }
    }
    // What the bot is doing, for the window.
    std::string Doing() const {
        if (!worldUnit) return phase_ == Phase::Lobby ? "waiting in the lobby" : "entering the quest";
        if (fightTarget_) return "fighting unit " + std::to_string(fightTarget_);
        return followHost && !chatStay_ ? "following the host" : "standing";
    }
    bool Alive() const { if (!worldUnit || dead_.count(worldUnit)) return false; const auto it = unitHp_.find(worldUnit); return it == unitHp_.end() || it->second > 0; }
    // Casts the Healing spell (spell items index healSpellSlot) on a target. The first order of a series only switches the hero's stance (checked live: ExecuteCombatOrder
    // drops it); so it is sent again 1.2 s later when the mana has not been spent. The cast costs ~44 mana of 57 (learned from the mana drop: spellCost_).
    void CastHealSpell(uint32_t target) {
        CastOn(static_cast<uint32_t>(healSpellSlot), target);
        spellOrderAt_ = Now(); spellTarget_ = target; spellManaAtCast_ = Mana(worldUnit); spellResends_ = 1;
    }
    void SpellSteps() {
        if (spellOrderAt_ <= 0) return;
        const int mana = Mana(worldUnit);
        if (mana >= 0 && spellManaAtCast_ - mana >= 8) { spellCost_ = std::max(spellCost_, static_cast<int>(spellManaAtCast_) - mana); spellOrderAt_ = 0; return; } // paid: the cast started
        if (spellResends_ > 0 && Now() - spellOrderAt_ > 1.2) { --spellResends_; CastOn(static_cast<uint32_t>(healSpellSlot), spellTarget_); spellOrderAt_ = Now(); holdUntil_ = Now() + 2.5; }
        else if (Now() - spellOrderAt_ > 5) spellOrderAt_ = 0;
    }
    double spellOrderAt_ = 0, spellManaAtCast_ = 0; uint32_t spellTarget_ = 0; int spellResends_ = 0, spellCost_ = 0;
    int Health(uint32_t id) const { const auto it = unitHp_.find(id); return it == unitHp_.end() ? -1 : it->second; }
    int Mana(uint32_t id) const { const auto it = unitMana_.find(id); return it == unitMana_.end() ? -1 : it->second; }
    bool IsHero(uint32_t id) const { for (const auto& p : players) if (p.second.unitId == id) return true; return false; }
    // The nearest unit that is not a player's hero (0: none known).
    uint32_t NearestOther(float& dist) const {
        float mx, my; uint32_t best = 0; dist = 1e9f;
        if (!Where(worldUnit, mx, my)) return 0;
        for (const auto& kv : unitPos) {
            bool hero = false;
            for (const auto& p : players) if (p.second.unitId == kv.first) hero = true;
            float x, y;
            const auto ig = ignored_.find(kv.first);
            if (hero || dead_.count(kv.first) || (ig != ignored_.end() && ig->second > Now()) || !Where(kv.first, x, y)) continue;
            if (!(unitEnemies_.count(kv.first) && Hostile(kv.first))) continue; // only known enemies (#67): NPCs, animals and unknown units are left alone
            { float hx, hy; if (Where(HostUnit(), hx, hy) && std::hypot(x - hx, y - hy) > (priority == 2 ? 10.0f : leash)) continue; } // guard: only what is near the player; else inside the leash
            const float d = std::hypot(x - mx, y - my);
            if (d < dist) { dist = d; best = kv.first; }
        }
        return best;
    }
    static constexpr float kAttackReach = 3.0f; // how close to a unit before ordering the attack (see the fight loop)
    int engageTries_ = 0;
    int healSelfBelow = 35;     // % of its own health (the settings' Heal itself below)
    bool usePotions = true;
    int potionSlot = 0;         // the belt slot (weapon-slot index) of the healing potion
    bool leashed_ = false;
    double holdUntil_ = 0;      // no new orders before this time (a potion is being drunk)
    // The Healing spell's object id from the character's spell list (0: none / no database); the host's heal threshold in % (0: off).
    int healSpellSlot = -1; int healHostBelow = 0;   // the index in the hero's spell items of a spell container holding Healing (-1: none): the spell id of a 0x33 cast order. Verified live: CastOrderSpell and CastSpellMulti run.
    bool drinkSpell_ = false; int spellFails_ = 0, hostHealFails_ = 0, hostHealHp_ = 0; double hostHealAt_ = 0, hostHealCastAt_ = 0, hostHealPause_ = 0;
    bool potionsLeft_ = true; int potionFails_ = 0; double healAt_ = 0, drinkAt_ = 0; int drinkHp_ = 0;
    bool fightInSight = false;  // fighting at all (Engagement not Passive)
    int engagement = 1;         // 0 passive, 1 defensive (the units attacking the host or us, the host's target), 2 aggressive (also anything in sight)
    float sight = 13.0f;        // the character's sight (map units)
    bool chatStay_ = false; int chatEngagement_ = -1; // set by the host's chat commands ("bot stay", "bot follow", "bot passive|defensive|aggressive")
    bool followHost = true;     // walk after the host's hero, staying about followDistance from it
    float followDistance = 4.0f;
    // The host's hero: the first other player in the quest world (the host is listed first).
    uint32_t HostUnit() const {
        for (const auto& kv : players) if (IsHost(kv.second) && kv.second.unitId != worldUnit && kv.second.state == 0 && kv.second.unitId) return kv.second.unitId;
        return 0;
    }
    std::string profileName;    // the player profile sent when entering (empty: the character's name)
    bool leaveWithHost = true;  // leave the quest zone when the host does
    bool autoEnter = true;      // enter the chosen quest when its zone opens
    uint32_t worldUnit = 0;     // our hero's unit in the quest world (0: not in it)

    ~Client() { Close(); }

    // Hands a packet to the client as if the server had sent it (replaying a capture: tests, research).
    void Feed(const uint8_t* b, int n) { Handle(b, n); }

    // Starts: resolves the server, opens a UDP socket, asks for its info.
    bool Connect(const std::string& host, int port) {
        Close();
        error.clear();
#ifdef _WIN32
        static bool wsa = false;
        if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
#endif
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return Fail("cannot resolve " + host);
        std::memcpy(&server_, res->ai_addr, sizeof server_);
        freeaddrinfo(res);
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kBad) return Fail("cannot open a UDP socket");
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sock_, FIONBIO, &nb);
#else
        fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL, 0) | O_NONBLOCK);
#endif
        cookie_ = std::random_device{}();
        state = State::AskingInfo;
        Send({0x03, 0, 0, 0, 0});
        sentAt_ = Now();
        Log("asked " + host + ":" + std::to_string(port) + " for its info");
        return true;
    }

    // Every frame: reads what arrived, goes on with the handshake, retries, times out.
    void Update() {
        if (sock_ == kBad) return;
        uint8_t buf[2048];
        while (true) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const int n = static_cast<int>(recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n <= 0) break;
            Handle(buf, n);
        }
        // In the session: an update every 20 ms (as the game): the ack bits, our stream part, the JOIN after a change.
        if (state == State::Accepted) { Enter(); RespawnSteps(); WatchHost(); WatchJoin(); WatchDeath(); LeaveSteps(); }
        // The view point (0x40 + f32 x, y in map units), as the game's client sends its camera's about twice a second:
        // the server may send each client what is around it.
        if (state == State::Accepted && worldUnit && Now() - viewAt_ > 0.5) {
            float x, y;
            if (Where(worldUnit, x, y)) {
                std::vector<uint8_t> m{0x40};
                uint32_t u; std::memcpy(&u, &x, 4); Put32(m, u); std::memcpy(&u, &y, 4); Put32(m, u);
                StreamMessage(m);
            }
            viewAt_ = Now();
        }
        if (matchPace) { const auto hp = unitPace_.find(HostUnit()); if (hp != unitPace_.end() && hp->second >= 0 && hp->second <= 3) pace = hp->second; }
        if (state == State::Accepted && worldUnit && pace != paceSent_) SetPace(pace);
        SpellSteps();
        // Heal itself: drink the belt potion (quick slot potionSlot, CastOn with `slot | 0x80000000` on ourselves) when the health falls below healSelfBelow %.
        // The answer is read from our own health 2.5 s later; two drinks without a rise = no potion left (or none that heals).
        if (state == State::Accepted && worldUnit) { // the health as it changes (research: the effect of a potion over time)
            const int hpNow = Health(worldUnit);
            const int mpNow = Mana(worldUnit);
            if ((hpNow != hpLogged_ && hpNow > 0) || mpNow != mpLogged_) { if (std::getenv("UM_BOT_HPLOG")) Log("health " + std::to_string(hpNow) + " mana " + std::to_string(mpNow)); hpLogged_ = hpNow; mpLogged_ = mpNow; }
        }
        if (state == State::Accepted && worldUnit && (usePotions || healSpellSlot >= 0)) {
            const int hp = Health(worldUnit); const auto mxIt = unitHpMax_.find(worldUnit); const int mx = mxIt == unitHpMax_.end() ? 0 : mxIt->second;
            if (drinkAt_ > 0 && hp > drinkHp_) { // the answer: our health rose (checked live: the potion +6 within 0.2 s)
                Log(std::string(drinkSpell_ ? "the Healing spell" : "the potion") + " healed (" + std::to_string(drinkHp_) + " -> " + std::to_string(hp) + ")");
                if (drinkSpell_) spellFails_ = 0; else potionFails_ = 0;
                drinkAt_ = 0;
            } else if (drinkAt_ > 0 && Now() - drinkAt_ > (drinkSpell_ ? 4.5 : 1.0)) { // the potion works within 0.2 s; a spell has a cast time (~1.6 s, after the stance switch)
                if (drinkSpell_) { if (++spellFails_ >= 4) Log("the Healing spell raised the health 4 times in a row: not casting it on itself any more"); }
                else { potionsLeft_ = false; healAt_ = 0; Log("the potion raised nothing: the belt is empty, not drinking any more"); }
                drinkAt_ = 0;
            }
            const bool canPotion = usePotions && potionsLeft_, canSpell = healSpellSlot >= 0 && spellFails_ < 4 && Mana(worldUnit) >= spellCost_;
            if (drinkAt_ == 0 && mx > 0 && hp > 0 && hp * 100 < healSelfBelow * mx && Now() - healAt_ > 4 && (canPotion || canSpell)) {
                drinkSpell_ = !canPotion;
                Log("health " + std::to_string(hp) + " / " + std::to_string(mx) + (drinkSpell_ ? ": casting Healing on itself" : ": drinking the potion in quick slot " + std::to_string(potionSlot)));
                if (drinkSpell_) CastHealSpell(worldUnit); else UseQuickSlot(potionSlot, worldUnit);
                healAt_ = Now(); drinkAt_ = Now(); drinkHp_ = hp; holdUntil_ = Now() + 2.5; // no other order meanwhile: a move / attack would cancel the queued cast
            }
        }
        // Heal the host: the belt potion (or the Healing spell) used on it when its health falls below healHostBelow % (and it is near enough); the answer is its health 3 s later.
        if (state == State::Accepted && worldUnit && (healSpellSlot >= 0 || (usePotions && potionsLeft_)) && healHostBelow > 0 && Health(worldUnit) > 0) {
            const uint32_t host = HostUnit();
            const int hp = Health(host); const auto mxIt = unitHpMax_.find(host); const int mx = mxIt == unitHpMax_.end() ? 0 : mxIt->second;
            if (hostHealAt_ > 0 && hp > hostHealHp_) { Log("healed the host (" + std::to_string(hostHealHp_) + " -> " + std::to_string(hp) + ")"); hostHealFails_ = 0; hostHealAt_ = 0; }
            else if (hostHealAt_ > 0 && Now() - hostHealAt_ > 3) { if (++hostHealFails_ >= 3) { Log("three casts on the host raised nothing: pausing 30 s"); hostHealPause_ = Now() + 30; hostHealFails_ = 0; } hostHealAt_ = 0; }
            float hx, hy, mx2, my2;
            if (hostHealAt_ == 0 && host && mx > 0 && hp > 0 && hp * 100 < healHostBelow * mx && Now() > hostHealPause_ && Now() - hostHealCastAt_ > 4 &&
                Where(host, hx, hy) && Where(worldUnit, mx2, my2) && std::hypot(hx - mx2, hy - my2) < 15) {
                const bool bySpell = healSpellSlot >= 0;
                Log("the host's health " + std::to_string(hp) + " / " + std::to_string(mx) + (bySpell ? ": casting Healing on it" : ": using the potion in quick slot " + std::to_string(potionSlot) + " on it"));
                if (bySpell) CastHealSpell(host); else UseQuickSlot(potionSlot, host); hostHealAt_ = hostHealCastAt_ = Now(); hostHealHp_ = hp; holdUntil_ = Now() + 1.5;
            }
        }
        // Leash: past `leash` units from the host the fight is dropped and the bot walks back (the follow below) until it is near the host again.
        if (state == State::Accepted && worldUnit && Alive()) {
            float hx, hy, mx, my;
            if (Where(HostUnit(), hx, hy) && Where(worldUnit, mx, my)) {
                const float dh = std::hypot(hx - mx, hy - my);
                if (!leashed_ && dh > leash) { leashed_ = true; fightTarget_ = 0; Log("beyond the leash (" + std::to_string(static_cast<int>(dh)) + " units): back to the host"); }
                else if (leashed_ && dh < followDistance * 2 + 1) { leashed_ = false; Log("back near the host"); }
            }
        }
        // Fight: the nearest unit (not a player's hero) within sight, attacked again every 3 s while it stays the target;
        // nothing in sight: follow the host as usual.
        bool fighting = false;
        if (state == State::Accepted && (chatEngagement_ >= 0 ? chatEngagement_ > 0 : fightInSight) && worldUnit && Alive() && Now() - fightAt_ > 1 && Now() >= holdUntil_ && !leashed_) {
            fightAt_ = Now();
            float d = 0;
            uint32_t t = (chatEngagement_ >= 0 ? chatEngagement_ : engagement) >= 2 ? NearestOther(d) : 0; // aggressive: anything in sight
            if (!(t && d <= sight)) t = 0;
            { // first the units attacking the host or us, then the host's own target
                float mx, my, best = 1e9f;
                const uint32_t host = HostUnit();
                if (Where(worldUnit, mx, my))
                    for (const auto& kv : unitTarget_) {
                        float x, y;
                        const bool onUs = kv.second == host || kv.second == worldUnit;
                        const bool hostsTarget = kv.first == host;
                        const uint32_t u = hostsTarget ? kv.second : kv.first;
                        if (!(onUs || hostsTarget) || IsHero(u) || dead_.count(u) || !Where(u, x, y)) continue;
                        const float e = std::hypot(x - mx, y - my) + (hostsTarget ? (priority == 1 ? -5.0f : 5.0f) : 0.0f); // focus: the player's target first
                        if (e < best && e < sight * 3) { best = e; t = u; d = std::hypot(x - mx, y - my); }
                    }
            }
            { // keep the current target while it lives and stays near: switching (or moving) would cancel the attack
                float x, y, mx, my;
                if (fightTarget_ && !dead_.count(fightTarget_) && Where(fightTarget_, x, y) && Where(worldUnit, mx, my) &&
                    std::hypot(x - mx, y - my) <= sight * 2) { t = fightTarget_; d = std::hypot(x - mx, y - my); }
            }
            const bool sticky = t && t == fightTarget_;
            if (!sticky && !(t && d <= sight) && Now() - selfHurtAt_ < 6) { // we are hurt: the nearest unit, out of sight too
                t = NearestOther(d);
                if (d > sight * 2) t = 0;
            }
            if (!sticky && !(t && d <= sight * 2) && Now() - hostHurtAt_ < 6) { // the host is hurt: the unit nearest to it, even out of our sight
                float hx, hy, mx, my, best = 25;
                t = 0;
                if (Where(HostUnit(), hx, hy) && Where(worldUnit, mx, my))
                    for (const auto& kv : unitPos) {
                        float x, y;
                        const auto ig = ignored_.find(kv.first);
                        if (IsHero(kv.first) || dead_.count(kv.first) || (ig != ignored_.end() && ig->second > Now()) || !Where(kv.first, x, y)) continue;
                        const auto act = unitAction_.find(kv.first);
                        const float e = std::hypot(x - hx, y - hy) - (act != unitAction_.end() && act->second != 0xFE ? 5.0f : 0.0f);
                        if (e < best) { best = e; t = kv.first; d = std::hypot(x - mx, y - my); }
                    }
            }
            if (t) {
                fighting = true;
                if (t != fightTarget_) { fightTarget_ = t; targetSince_ = Now(); engageTries_ = 0; attackedAt_ = 0; Log("attacking unit " + std::to_string(t)); }
                float x, y, mx, my;
                const auto th = unitHp_.find(t);
                if (th != unitHp_.end() && th->second != targetHp_) { targetHp_ = th->second; targetSince_ = Now(); } // it is being hurt: progress
                if (Now() - targetSince_ > 25) { ignored_[t] = Now() + 30; fightTarget_ = 0; fighting = false; Log("no result on unit " + std::to_string(t) + ": leaving it for 30 s"); }
                else if (Where(t, x, y) && Where(worldUnit, mx, my)) {
                    d = std::hypot(x - mx, y - my);
                    // The server refuses an attack order on a unit our hero has not seen (WorldServer::OrderOnTarget 0x5D3C30: AiMemory::IsFriendOrKnown 0x5CB7F0
                    // must find it in the hero's known list): found 2026-10-19 by ordering from 9, 8, 7, 5 units (refused) and ~3 (accepted). So: walk up to
                    // kAttackReach, order the attack, and take "our own update says: attacking this unit" as the answer (unitTarget_, field 10 mode 3).
                    const auto tg = unitTarget_.find(worldUnit);
                    const bool engaged = tg != unitTarget_.end() && tg->second == t;
                    if (engaged) { idleSince_ = Now(); engageTries_ = 0; }
                    else if (d > kAttackReach + 0.7f) {
                        if (Now() - attackedAt_ > 1.2) { MoveTo(x + (mx - x) * kAttackReach / d, y + (my - y) * kAttackReach / d); attackedAt_ = Now(); attackedTarget_ = 0; }
                    } else if (Now() - attackedAt_ > 1.5) {
                        if (++engageTries_ > 6) { ignored_[t] = Now() + 30; fightTarget_ = 0; fighting = false; Log("attack on unit " + std::to_string(t) + " refused " + std::to_string(engageTries_ - 1) + " times: leaving it for 30 s"); }
                        else {
                            Log(std::string("attack ") + std::to_string(t) + " at " + std::to_string(d) + " (try " + std::to_string(engageTries_) + ")");
                            Attack(t); attackedAt_ = Now(); attackedTarget_ = t;
                            if (engageTries_ > 2 && d > 1.5f) MoveTo(x + (mx - x) * 1.0f / d, y + (my - y) * 1.0f / d); // still refused: one step closer
                        }
                    }
                }
            } else {
                fightTarget_ = 0;
            }
        }
        if (fightTarget_ && !unitPos.count(fightTarget_)) fightTarget_ = 0;
        if (fightTarget_) fighting = true;
        if (!fighting && state == State::Accepted && followHost && !chatStay_ && worldUnit && Alive() && Now() - followAt_ > 2 && Now() >= holdUntil_) { // walk to the host when it is away
            followAt_ = Now();
            float hx, hy, mx, my;
            if (Where(HostUnit(), hx, hy)) {
                const bool known = Where(worldUnit, mx, my);
                const float away = known ? std::hypot(hx - mx, hy - my) : 99.0f;
                if (away > followDistance + 1.0f && std::hypot(hx - followX_, hy - followY_) > 1.0f) {
                    followX_ = hx; followY_ = hy;
                    // to the point followDistance short of the host, on our side of it
                    const float k = known && away > 0 ? followDistance / away : 0.0f;
                    MoveTo(hx + (mx - hx) * k, hy + (my - hy) * k);
                    if (std::getenv("UM_BOT_FIGHT")) Log("follow");
                }
            }
        }
        if (state == State::Accepted && Now() - sentAt_ > 0.02) {
            uint32_t bits = 0;
            for (int i = 0; i < 32; ++i) if (got_.count(static_cast<uint16_t>(ackSeq_ - i))) bits |= 1u << i;
            std::vector<uint8_t> p{0x00};
            Put16(p, seq_++);
            Put16(p, ackSeq_);
            Put32(p, bits);
            Chunk c;
            if (!resend_.empty()) { c = std::move(resend_.front()); resend_.pop_front(); } // a lost chunk first, at its old offset
            else {
                const size_t n = std::min<size_t>(204, tx_.size());
                c.offset = txOffset_;
                c.bytes.assign(tx_.begin(), tx_.begin() + static_cast<long>(n));
                tx_.erase(tx_.begin(), tx_.begin() + static_cast<long>(n));
                txOffset_ = static_cast<uint16_t>(txOffset_ + n);
            }
            Put16(p, c.offset);
            p.push_back(static_cast<uint8_t>(c.bytes.size()));
            p.insert(p.end(), c.bytes.begin(), c.bytes.end());
            if (!c.bytes.empty()) unacked_[static_cast<uint16_t>(seq_ - 1)] = std::move(c);
            if (joinSends_ > 0 && !join_.empty()) {
                --joinSends_;
                p.push_back(1);    // one message
                p.push_back(0x01); // its flag byte
                p.push_back(5);    // JOIN
                Put16(p, 1);
                p.insert(p.end(), join_.begin(), join_.end());
            } else {
                p.push_back(0);
            }
            Send(p);
            sentAt_ = Now();
        }
        const double waited = Now() - sentAt_;
        if ((state == State::AskingInfo || state == State::LoggingIn) && waited > 3.0) {
            if (++retries_ > 3) { Fail("no answer from the server"); return; }
            if (state == State::AskingInfo) Send({0x03, 0, 0, 0, 0});
            else SendLogin();
            sentAt_ = Now();
        }
    }

    void Disconnect() {
        if (sock_ != kBad && state == State::Accepted) {
            std::vector<uint8_t> p{0x01};
            Put32(p, clientId);
            Send(p);
            Log("disconnected");
        }
        Close();
        state = State::Idle;
    }
    void SendRaw(const std::vector<uint8_t>& m) { StreamMessage(m); } // research
    // Leaves the world for good (the host left, or we are asked to): the leave message 0x04 + u32 0 makes the server move what we carried into the party and answer with
    // the party (0x83: the final save, taken by the owner of the file); the connection is closed 2 s later and `reconnect` is set: the lobby is entered again with a
    // fresh handshake (a JOIN 3 after the leave message gets no snapshot: checked live 2026-10-20).
    void LeaveWorld() {
        if (state != State::Accepted || !worldUnit || leavingAt_ > 0) return;
        Log("leaving the world (the final save), then reconnecting");
        std::vector<uint8_t> leave{0x04}; Put32(leave, 0); StreamMessage(leave);
        leavingAt_ = Now();
    }
    bool reconnect = false;     // set after LeaveWorld: the owner connects again
    void LeaveSteps() {
        if (leavingAt_ > 0 && Now() - leavingAt_ > 2) { leavingAt_ = 0; Disconnect(); worldUnit = 0; reconnect = true; }
    }
    void GoToLobby() { LeaveZone(); }   // leave the quest zone, stay in the session
    // Stream message 0x06 (server OnMsg_ChangeMapByName 0x670E00): a map name and an entry dword; the hero goes to that map (research: how a real client leaves a quest).
    void ChangeMapByName(const std::string& map, uint32_t entry = 0) {
        std::vector<uint8_t> m{0x06};
        m.insert(m.end(), map.begin(), map.end()); m.push_back(0);
        Put32(m, entry);
        StreamMessage(m);
    }
    void ChangeMapById(uint32_t id) { std::vector<uint8_t> m{0x03}; Put32(m, id); StreamMessage(m); } // stream message 0x03 (OnMsg_ChangeMapById 0x670C20)

private:
#ifdef _WIN32
    static constexpr SOCKET kBad = INVALID_SOCKET;
    SOCKET sock_ = INVALID_SOCKET;
#else
    static constexpr int kBad = -1;
    int sock_ = -1;
#endif
    sockaddr_in server_{};
    uint32_t cookie_ = 0;
    double sentAt_ = 0;
    int retries_ = 0;

    static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    uint16_t seq_ = 1, ackSeq_ = 0;
    int updates_ = 0;
    std::string name_, unit_;
    uint32_t heroId_ = 0;
    std::vector<uint8_t> mpRaw_;
    std::vector<uint8_t> join_;
    std::string worldQuest_;
    int pktDumped_ = 0;
    int hpLogged_ = -1, mpLogged_ = -1;
    double leavingAt_ = 0;
    double hostGoneAt_ = 0, joinWatchAt_ = 0; int joinRetries_ = 0;
    int joinSends_ = 0;                        // the JOIN goes 4 times after each change
    std::set<uint16_t> got_;                   // the server's packets received (the ack bits)
    // The update's stream part (_cpr/claude-re/net/README.md): out = our messages, in = the server's
    // Outgoing: tx_ holds the bytes not cut into a chunk yet (txOffset_ = the stream offset of tx_[0]); a chunk goes in
    // one update and is kept in unacked_ by that update's sequence until the server's ack bits cover it; an update
    // more than 32 behind the server's last ack without its bit was lost: its chunk is queued again (resend_), as the
    // game resends its own (the server waits for the missing offset otherwise: nothing after it is read).
    struct Chunk { uint16_t offset; std::vector<uint8_t> bytes; };
    std::vector<uint8_t> tx_; uint16_t txOffset_ = 0;
    std::map<uint16_t, Chunk> unacked_;
    std::deque<Chunk> resend_;
    // Incoming: rx_ holds the stream from rxBase_ on (the parsed prefix is dropped), rxParsed_ counts from rx_[0].
    std::map<uint16_t, std::vector<uint8_t>> rxChunks_; std::vector<uint8_t> rx_; size_t rxParsed_ = 0; uint32_t rxBase_ = 0;
    enum class Phase { Lobby, Loading, Entering, InWorld } phase_ = Phase::Lobby;
    double phaseAt_ = 0, respawnAt_ = -1, viewAt_ = 0;
    int respawnStep_ = 0;
    bool heroJoined_ = false;
    int dumped_ = 0;
    double followAt_ = 0, fightAt_ = 0, attackedAt_ = 0;
    int paceSent_ = -1;
    uint32_t fightTarget_ = 0;
    double targetSince_ = 0, idleSince_ = 0, selfHurtAt_ = -99;
    uint32_t attackedTarget_ = 0;
    int targetHp_ = 0;
    std::map<uint32_t, double> ignored_; // targets left alone until then
    float followX_ = -99, followY_ = -99;
    void SetJoin(uint32_t st, uint32_t hero) {
        join_.assign(name_.begin(), name_.end()); join_.push_back(0);
        join_.insert(join_.end(), unit_.begin(), unit_.end()); join_.push_back(0);
        Put32(join_, st); join_.push_back(0); Put32(join_, hero);
        joinSends_ = 4;
    }
    // A stream message: u32 4 + compressed length, u32 length, the data (literal-only compression).
    void StreamMessage(const std::vector<uint8_t>& data) {
        const std::vector<uint8_t> packed = mp::Pack(data, 0, 0);
        const std::vector<uint8_t> bits(packed.begin() + 17, packed.end());
        Put32(tx_, static_cast<uint32_t>(4 + bits.size())); Put32(tx_, static_cast<uint32_t>(data.size()));
        tx_.insert(tx_.end(), bits.begin(), bits.end());
    }
    // The server's compression window carries over from one stream message to the next.
    uint8_t win[1024] = {}; unsigned pos = 0;
    std::vector<uint8_t> Unpack(const std::vector<uint8_t>& d, size_t at, uint32_t size) {
        mp::BitReader b{d, at};
        std::vector<uint8_t> out;
        while (out.size() < size && !b.bad) {
            if (!b.Bits(1)) { const uint8_t c = b.Byte(); out.push_back(c); win[pos] = c; pos = (pos + 1) & 0x3FF; }
            else {
                const int len = mp::MatchLength(b), off = mp::MatchOffset(b);
                for (int i = 0; i < len && out.size() < size; ++i) { const uint8_t c = win[(pos - off - 1) & 0x3FF]; out.push_back(c); win[pos] = c; pos = (pos + 1) & 0x3FF; }
            }
        }
        return out;
    }
    // Entering the chosen quest like a real client: JOIN 3, the upload (08 + the .mp content with the quest as its
    // zone + 00 + the 136-byte identity block: version 100, the game's default name and password), then the server's
    // snapshot (our new hero id), "09" (ready: the server spawns the hero), JOIN 0 with that id.
    void Enter() {
        if (phase_ == Phase::Lobby && !heroJoined_ && joinSends_ == 0) { heroJoined_ = true; SetJoin(1, heroId_); return; }
        if (phase_ == Phase::Lobby && autoEnter && joined && ZoneOpen() && !mpRaw_.empty()) {
            phase_ = Phase::Loading; phaseAt_ = Now(); SetJoin(3, heroId_);
            if (worldQuest_ != chosenQuest) ClearWorld(); // another quest = another world
            worldQuest_ = chosenQuest;
            Log("the zone of " + chosenQuest + " is open: entering it");
        } else if (phase_ == Phase::Loading && Now() - phaseAt_ > 0.5) {
            StreamMessage(Upload());
            phase_ = Phase::Entering; phaseAt_ = Now();
        }
    }
    // After a death the game's client enters again (capture 2026-10-04): JOIN 3, the upload (08 + the .mp + the
    // identity), "09" ready, JOIN 0 with the character's id. The new unit is already known (our kind-1 record).
    std::vector<uint8_t> Upload() const { // 08 + the .mp content (the quest as its zone) + 00 + the 136-byte identity
        std::vector<uint8_t> up{0x08};
        up.insert(up.end(), chosenQuest.begin(), chosenQuest.end()); up.push_back(0);
        const size_t zoneEnd = static_cast<size_t>(std::find(mpRaw_.begin(), mpRaw_.end(), 0) - mpRaw_.begin()) + 1;
        up.insert(up.end(), mpRaw_.begin() + static_cast<long>(std::min(zoneEnd, mpRaw_.size())), mpRaw_.end());
        up.push_back(0);
        uint8_t block[136] = {};
        const uint32_t version = 100;
        std::memcpy(block, &version, 4);
        // The player's profile: its own name (the character's, without the clan tag), not the copied "Tango".
        const std::string profile = profileName.empty() ? name_.substr(0, name_.find(" | ")) : profileName;
        std::memcpy(block + 4, profile.data(), std::min<size_t>(profile.size(), 63));
        std::memcpy(block + 68, "Very top secret", 15); // the game's default password
        up.insert(up.end(), block, block + 136);
        return up;
    }
    // Leaves the quest zone but stays in the session (the lobby): JOIN state 1 with the character's id, as a real client does when it goes back (the notes: the
    // players must leave the zone before the host can). The next quest is entered again by Enter() when its zone is open and autoEnter is set.
    void LeaveZone() {
        if (phase_ != Phase::InWorld && phase_ != Phase::Entering) return;
        Log("leaving the quest zone (back to the lobby)");
        phase_ = Phase::Lobby; worldUnit = 0; heroJoined_ = true;
        fightTarget_ = 0; leashed_ = false; paceSent_ = -1; hostGoneAt_ = 0; // the world state stays: re-entering the same quest resends no creations (the host's world is the same); a new quest clears it in Enter()
        SetJoin(1, heroId_);
    }
    void ClearWorld() {
        unitPos.clear(); unitInfo_.clear(); unitFields_.clear(); dead_.clear(); unitTarget_.clear(); unitAction_.clear();
        unitHp_.clear(); unitMana_.clear(); unitHpMax_.clear(); unitManaMax_.clear(); unitSide_.clear(); unitEnemies_.clear(); unitPace_.clear();
    }
    // The host left the quest zone (no other player is in it for a while): follow it back to the lobby.
    void WatchHost() {
        if (phase_ != Phase::InWorld || !leaveWithHost) { hostGoneAt_ = 0; return; }
        bool hostIn = false;
        for (const auto& kv : players) if (IsHost(kv.second) && kv.second.unitId != worldUnit && kv.second.name != name_ && kv.second.state == 0 && kv.second.unitId) hostIn = true;
        if (hostIn) { hostGoneAt_ = 0; return; }
        if (hostGoneAt_ == 0) hostGoneAt_ = Now();
        else if (Now() - hostGoneAt_ > 3) { Log("the host left the zone: leaving it too"); LeaveWorld(); }
    }
    // Join race: the server did not list us (another client left just before, or the first packets were lost): ask again, then give up.
    // Our hero died: the server sent no respawn in 140 s (live, 2026-10-20: the player's record keeps its unit, nothing else changes); entering the world anew (a fresh
    // connection) gives a new, living hero (checked: every run after a death started with full health). After `deadGrace` seconds dead: leave and reconnect.
    double deadGrace = 45, deadAt_ = 0;
    void WatchDeath() {
        if (phase_ != Phase::InWorld || !worldUnit || leavingAt_ > 0) { deadAt_ = 0; return; }
        const auto hp = unitHp_.find(worldUnit);
        const bool dead = dead_.count(worldUnit) || (hp != unitHp_.end() && hp->second <= 0);
        if (!dead) { deadAt_ = 0; return; }
        if (deadAt_ == 0) { deadAt_ = Now(); Log("our hero is dead"); }
        else if (Now() - deadAt_ > deadGrace) { deadAt_ = 0; Log("still dead after " + std::to_string(static_cast<int>(deadGrace)) + " s: entering the world anew"); LeaveWorld(); }
    }
    void WatchJoin() {
        if (joined || state != State::Accepted) { joinWatchAt_ = 0; return; }
        if (joinWatchAt_ == 0) { joinWatchAt_ = Now(); return; }
        if (Now() - joinWatchAt_ > 6) {
            joinWatchAt_ = Now();
            if (++joinRetries_ > 3) { Fail("the server never listed us (join race): reconnect"); return; }
            Log("not listed yet: sending the JOIN again (" + std::to_string(joinRetries_) + ")");
            heroJoined_ = false; SetJoin(1, 0xFFFFFFFFu);
        }
    }
    void Respawned() {
        SetJoin(3, heroId_);
        respawnAt_ = Now();
    }
    void RespawnSteps() {
        if (respawnAt_ < 0) return;
        if (Now() - respawnAt_ > 0.5 && respawnStep_ == 0) { StreamMessage(Upload()); respawnStep_ = 1; }
        else if (Now() - respawnAt_ > 2.5 && respawnStep_ == 1) { StreamMessage({0x09}); SetJoin(0, worldUnit); respawnStep_ = 0; respawnAt_ = -1; }
    }

    // The server's stream: its first big message is the world snapshot, holding our hero's new unit id.
    void ReadStream() {
        while (rxParsed_ + 8 <= rx_.size()) {
            uint32_t clen, ulen;
            std::memcpy(&clen, rx_.data() + rxParsed_, 4); std::memcpy(&ulen, rx_.data() + rxParsed_ + 4, 4);
            if (rxParsed_ + 4 + clen > rx_.size()) { if (std::getenv("UM_BOT_RXLOG")) std::fprintf(stderr, "wait: parsed %zu clen %u ulen %u have %zu\n", rxParsed_, clen, ulen, rx_.size()); return; }
            const std::vector<uint8_t> m = ulen ? Unpack(rx_, rxParsed_ + 8, ulen) : std::vector<uint8_t>();
            if (!m.empty()) ReadUnits(m);
            if (const char* dir = std::getenv("UM_BOT_DUMP"); dir && ulen) { // research: every stream message, decompressed
                char path[512]; std::snprintf(path, sizeof path, "%s/%05d-%.3f.bin", dir, dumped_++, Now());
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(m.data(), 1, m.size(), f); std::fclose(f); }
            }
            if (ulen > 64 && phase_ == Phase::Entering) {
                const std::vector<uint8_t>& snap = m;
                uint32_t mine = 0;
                for (size_t k = 0; k + 4 <= snap.size(); ++k) {
                    uint32_t v; std::memcpy(&v, snap.data() + k, 4);
                    bool other = false;
                    for (const auto& kv : players) if (kv.second.unitId == v) other = true;
                    if (v >= 0x3B9ACA00u && v <= 0x3B9ACAFFu && !other) mine = std::max(mine, v);
                }
                if (mine) {
                    worldUnit = mine; phase_ = Phase::InWorld;
                    StreamMessage({0x09});
                    SetJoin(0, mine); // the world unit: the host links the party portrait to it (the .mp's id dims it)
                    Log("in the quest world: our hero is unit " + std::to_string(mine));
                }
            }
            rxParsed_ += 4 + clen;
        }
        if (rxParsed_ > 65536) { // the parsed bytes are not needed again: drop them (rxBase_ keeps the stream offsets right)
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(rxParsed_));
            rxBase_ += static_cast<uint32_t>(rxParsed_);
            rxParsed_ = 0;
        }
    }
    static void Put16(std::vector<uint8_t>& p, uint16_t v) { p.push_back(static_cast<uint8_t>(v)); p.push_back(static_cast<uint8_t>(v >> 8)); }
    static void Put32(std::vector<uint8_t>& p, uint32_t v) { for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    static uint32_t Get32(const uint8_t* b) { uint32_t v; std::memcpy(&v, b, 4); return v; }
    void Log(const std::string& s) { log.push_back(s); if (log.size() > 2000) log.erase(log.begin(), log.begin() + 500); }
    bool Fail(const std::string& why) { error = why; state = State::Failed; Log("failed: " + why); Close(); return false; }
    void Send(const std::vector<uint8_t>& p) {
        sendto(sock_, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0, reinterpret_cast<const sockaddr*>(&server_), sizeof server_);
    }
    void SendLogin() {
        std::vector<uint8_t> p{0x04};
        Put32(p, info.key);
        Put32(p, cookie_);
        Put32(p, 12500); // as the game sends it (its bandwidth?); 0 makes the server treat the link differently
        p.resize(33, 0);
        Send(p);
    }
    void Close() {
        if (sock_ == kBad) return;
#ifdef _WIN32
        closesocket(sock_);
#else
        close(sock_);
#endif
        sock_ = kBad;
    }
    std::set<std::string> seenMessages_; // reliable messages come again until acknowledged: each handled once

    // The reliable messages of a server update: u8 n, then per message (a flag byte before every 8, LSB first:
    // 1 = with its payload, 0 = the object removed) u8 kind, u16 id, the payload. Kinds holding object references
    // (4, 12, 13, 15, 16, 17) also use flag bits: reading stops there (the rest of the packet is not read).
    void ReadMessages(const uint8_t* b, int n) {
        if (n < 12) return;
        int o = 11;
        const int unreliable = b[o++];
        o += unreliable;
        if (o >= n) return;
        const int count = b[o++];
        uint8_t flags = 0;
        for (int i = 0; i < count && o < n; ++i) {
            if (i % 8 == 0) flags = b[o++];
            const bool payload = (flags >> (i % 8)) & 1;
            if (o + 3 > n) return;
            const int kind = b[o];
            uint16_t id;
            std::memcpy(&id, b + o + 1, 2);
            const int start = o;
            o += 3;
            auto str = [&]() { std::string v; while (o < n && b[o]) v += static_cast<char>(b[o++]); ++o; return v; };
            auto u32 = [&]() { uint32_t v = 0; if (o + 4 <= n) std::memcpy(&v, b + o, 4); o += 4; return v; };
            auto u16 = [&]() { uint16_t v = 0; if (o + 2 <= n) std::memcpy(&v, b + o, 2); o += 2; return v; };
            std::string text;
            if (!payload) { // removed
                if (kind == 1) players.erase(id);
                else if (kind == 8) questStates.erase(id);
                else if (kind == 11) quests.erase(id);
                else if (kind == 9) chosenQuest.clear();
                continue;
            }
            switch (kind) {
            case 1: {
                Player p; p.name = str(); p.unit = str(); p.state = u32(); p.flag = o < n ? b[o] : 0; o += 1; p.unitId = u32(); p.tail = u32(); players[id] = p;
                // Our hero again after a death (respawned): a new unit id in our record.
                if (p.name == name_ && worldUnit && p.state == 0 && p.unitId >= 0x3B9ACA00u && p.unitId <= 0x3B9ACAFFu && p.unitId != worldUnit) {
                    Log("our hero is now unit " + std::to_string(p.unitId) + " (respawned?)");
                    worldUnit = p.unitId; paceSent_ = -1; fightTarget_ = 0; attackedTarget_ = 0;
                    Respawned(); // as the game's client after a death: the entry again (JOIN 3, upload, ready, JOIN 0)
                }
                break;
            }
            case 2: { u32(); text = str(); break; }           // a system line ("Quest accepted: ...")
            case 3: { // a printed pair; "<address>:<port>" + the quest: the session's quest (when kind 9 was missed)
                const std::string a = str(), q = str();
                text = a + " " + q;
                if (chosenQuest.empty() && a.find(':') != std::string::npos && a.find('.') != std::string::npos && !q.empty()) chosenQuest = q;
                break;
            }
            case 5: { str(); str(); u32(); o += 1; u32(); break; }
            case 6: case 9: text = str(); if (kind == 9) chosenQuest = text; break;
            case 7: { u32(); text = str(); break; }
            case 8: questStates[id] = str(); break;
            case 10: { str(); u32(); u32(); break; }          // a quest variable
            case 11: { u16(); u32(); quests[id] = str(); break; }
            case 4: break; // no payload
            case 14: { u32(); u32(); text = str(); break; }
            default:
                { std::string h; char x[4]; for (int k = 0; k < n; ++k) { std::snprintf(x, 4, "%02X ", b[k]); h += x; }
                  Log("server message kind " + std::to_string(kind) + ": not read (the rest skipped): " + h); }
                return;
            }
            const std::string key(reinterpret_cast<const char*>(b + start), static_cast<size_t>(std::min(o, n) - start));
            if (seenMessages_.size() > 4000) seenMessages_.clear(); // a long session: forget the old ones (a repeat is only logged twice)
            if (!seenMessages_.insert(key).second) continue;
            if (kind == 2 || kind == 3 || kind == 6) { Log("server: " + text); if (kind == 2) ChatCommand(text); }
            else if (kind == 9) Log("the host chose the quest " + text);
            else if (kind == 8 && questStates[id] == chosenQuest + "_2") Log("the zone of " + chosenQuest + " is open: the host is in it");
            else if (kind == 1) Log("player " + players[id].name + ": state " + std::to_string(players[id].state));
        }
    }

    void Handle(const uint8_t* b, int n) {
        if (const char* dir = std::getenv("UM_BOT_PKT")) { // research: every received UDP packet as a file
            char path[512]; std::snprintf(path, sizeof path, "%s/%05d.bin", dir, pktDumped_++);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(b, 1, static_cast<size_t>(n), f); std::fclose(f); }
        }
        switch (b[0]) {
        case 0x05: // info
            if (n < 9 || state != State::AskingInfo) return;
            info.key = Get32(b + 5);
            {
                int e = 9;
                while (e < n && b[e]) ++e;
                info.host.assign(reinterpret_cast<const char*>(b + 9), static_cast<size_t>(e - 9));
                info.details.assign(b + std::min(e + 1, n), b + n);
            }
            Log("server \"" + info.host + "\" answered; logging in");
            state = State::LoggingIn;
            retries_ = 0;
            SendLogin();
            sentAt_ = Now();
            break;
        case 0x06: // accepted
            if (n < 5) return;
            clientId = Get32(b + 1);
            state = State::Accepted;
            Log("accepted (the server echoed our cookie)");
            break;
        case 0x07: // rejected
            state = State::Rejected;
            error = "rejected by the server (reason " + std::to_string(n >= 6 ? b[5] : -1) + ")";
            Log(error);
            Close();
            break;
        case 0x00: // the server's update: acknowledged in the next keep-alive (its content is not decoded yet)
            if (n >= 9) { // what the server acknowledged of ours: u16 ack, u32 bits (bit i = ack - 1 - i)
                uint16_t ack; uint32_t bits; std::memcpy(&ack, b + 3, 2); std::memcpy(&bits, b + 5, 4);
                for (auto it = unacked_.begin(); it != unacked_.end();) {
                    const uint16_t back = static_cast<uint16_t>(ack - it->first);
                    const bool acked = back == 0 || (back >= 1 && back <= 32 && (bits & (1u << (back - 1))));
                    if (acked) it = unacked_.erase(it);
                    else if (back > 32 && back < 0x8000) { // too old for the window and never acked: lost
                        if (std::getenv("UM_BOT_RXLOG")) std::fprintf(stderr, "resend chunk at %u (%zu bytes) of update %u\n", it->second.offset, it->second.bytes.size(), it->first);
                        resend_.push_back(std::move(it->second)); it = unacked_.erase(it);
                    } else ++it;
                }
            }
            if (n >= 3) {
                uint16_t sq; std::memcpy(&sq, b + 1, 2);
                if (got_.empty() || static_cast<uint16_t>(sq - ackSeq_) < 0x8000) ackSeq_ = sq;
                got_.insert(sq);
                for (auto it = got_.begin(); it != got_.end();) // keep the last 32 (the ack bits), across the wrap
                    if (static_cast<uint16_t>(ackSeq_ - *it) >= 32 && static_cast<uint16_t>(*it - ackSeq_) >= 0x8000) it = got_.erase(it); else ++it;
            }
            if (n >= 12 && b[11]) { // the server's stream part
                uint16_t at; std::memcpy(&at, b + 9, 2);
                rxChunks_.emplace(at, std::vector<uint8_t>(b + 12, b + 12 + std::min<int>(b[11], n - 12)));
                if (std::getenv("UM_BOT_RXLOG")) std::fprintf(stderr, "%.2f rx seq %u at %u len %u have %zu parsed %zu: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", Now(), *reinterpret_cast<const uint16_t*>(b + 1), at, b[11], rx_.size(), rxParsed_, b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19], b[20], b[21], b[22]);
                // Chunks are resent and can overlap what we have: take the part past our end, drop the old ones.
                for (bool grew = true; grew;) {
                    grew = false;
                    for (auto it = rxChunks_.begin(); it != rxChunks_.end();) {
                        const uint16_t have = static_cast<uint16_t>(static_cast<uint16_t>(rxBase_ + rx_.size()) - it->first);
                        if (have < 0x8000 && have < it->second.size()) {
                            rx_.insert(rx_.end(), it->second.begin() + have, it->second.end());
                            grew = true;
                        }
                        if (have < 0x8000) it = rxChunks_.erase(it); else ++it;
                    }
                }
                ReadStream();
            }
            if (++updates_ == 1) Log("in the session: the server sends its updates (players, quests)");
            if (!joined && !name_.empty() && std::search(b, b + n, name_.begin(), name_.end()) != b + n) {
                joined = true;
                Log("joined: the server lists \"" + name_ + "\" (chat: connected, face in the lobby)");
            }
            ReadMessages(b, n);
            break;
        default:
            Log("received a packet of type " + std::to_string(b[0]) + " (" + std::to_string(n) + " bytes): not handled yet");
        }
    }
};

} // namespace net
