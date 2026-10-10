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
    struct Player { std::string name, unit; uint32_t state = 0, unitId = 0; };
    std::map<uint16_t, Player> players;          // kind 1, by its object id
    std::map<uint16_t, std::string> quests;      // kind 11: "<quest>.mq"
    std::map<uint16_t, std::string> questStates; // kind 8: "<quest>_1" (available), "_2" (its zone is open)
    std::string chosenQuest;                     // kind 9
    bool ZoneOpen() const {                      // the chosen quest's zone can be entered (the host is in it)
        for (const auto& kv : questStates) if (kv.second == chosenQuest + "_2") return true;
        if (!chosenQuest.empty()) for (const auto& kv : players) if (kv.second.name != name_ && kv.second.state == 0) return true; // the host is in it
        return false;
    }

    // The JOIN message sent once accepted (from the character's .mp): its name (with the clan tag), its unit name
    // and its u32 after the strings. The u32 1 / u8 1 are the host's own record's values (meaning not known yet).
    // The character: its name (with the clan tag), unit name, hero id (the .mp's u32) and the .mp's content (what is
    // uploaded when entering a quest). The JOIN: name\0 unit\0 u32 state, u8 0, u32 hero id.
    void SetCharacter(const std::string& name, const std::string& unit, uint32_t id, const std::vector<uint8_t>& mpRaw = {}) {
        name_ = name; unit_ = unit; heroId_ = id; mpRaw_ = mpRaw;
        phase_ = Phase::Lobby; worldUnit = 0; heroJoined_ = false;
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
    // A unit coming into view: 40, u32 id, u32 0x34, ..., "f5 0d" + 2 bytes, f32 x, y, z (map units).
    void ReadUnits(const std::vector<uint8_t>& m) {
        for (size_t i = 0; i + 9 <= m.size(); ++i) {
            if (m[i] != 0x40) continue; // also inside 0x81 messages; the word after the id is 0x34 or 0
            uint32_t id; std::memcpy(&id, &m[i + 1], 4);
            if (id < 0x100 || id >= 0x01000000 || dead_.count(id)) continue;
            for (size_t k = i + 9; k + 16 <= m.size() && k < i + 220; ++k) {
                if (m[k] != 0xF5 || m[k + 1] != 0x0D) continue;
                float x, y; std::memcpy(&x, &m[k + 4], 4); std::memcpy(&y, &m[k + 8], 4);
                if (x > 0 && y > 0 && x < 2048 && y < 2048) unitPos[id] = {x, y, x, y, Now()};
                break;
            }
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
            if (m[o] == 0x42 && o + 5 <= m.size() && (o + 5 == m.size() || m[o + 5] == 0x41 || m[o + 5] == 0x42)) {
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
    struct Field { std::vector<uint8_t> b; size_t at = 0; };
    std::map<std::pair<uint32_t, int>, Field> unitFields_;
    std::map<uint32_t, int> unitHp_, unitMana_;
    std::map<uint32_t, uint32_t> unitTarget_; // who attacks whom (field 10 mode 3)
    std::map<uint32_t, int> unitAction_; // field 8's action code: 0xFE idle, 0x07 often in a fight
    double hostHurtAt_ = -99;
    std::set<uint32_t> dead_;
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
                for (size_t n : sz->second) {
                    const uint8_t* v = delta(f, n);
                    if (k == 0 && !bad) commit.push_back([this, id, mana = static_cast<int16_t>(v[6] | v[7] << 8)] { unitMana_[id] = mana; }); // [3] = mana
                    if (k == 0 && !bad) commit.push_back([this, id, hp = static_cast<int16_t>(v[4] | v[5] << 8)] { // [2] = health: 0 = dead
                        auto h = unitHp_.find(id);
                        if (hp <= 0 && h != unitHp_.end() && h->second > 0) { dead_.insert(id); unitPos.erase(id); }
                        if (h != unitHp_.end() && hp < h->second && id == HostUnit()) hostHurtAt_ = Now();
                        if (h != unitHp_.end() && hp < h->second && id == worldUnit) selfHurtAt_ = Now();
                        unitHp_[id] = hp;
                    });
                    if (false) {
                        int16_t hp = 0;
                        auto h = unitHp_.find(id);
                        if (hp <= 0 && h != unitHp_.end() && h->second > 0) { dead_.insert(id); unitPos.erase(id); }
                        if (h != unitHp_.end() && hp < h->second && id == HostUnit()) hostHurtAt_ = Now(); // the host takes damage
                        if (h != unitHp_.end() && hp < h->second && id == worldUnit) selfHurtAt_ = Now();
                        unitHp_[id] = hp;
                    }
                    if (k == 8 && n == 1 && sz->second.size() == 4 && !bad && f.at == 2) commit.push_back([this, id, a = v[0]] { unitAction_[id] = a; }); // the action code
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
    // The hero's pace (stream message 0x35, server 0x6709D0 -> 0x555270: the unit's +0x268): u32 unit id, u8 mode
    // 0 crawl, 1 sneak, 2 walk, 3 run (um.dll's names).
    int pace = 3;
    void SetPace(int mode) {
        if (!worldUnit) return;
        std::vector<uint8_t> m{0x35};
        Put32(m, worldUnit);
        m.push_back(static_cast<uint8_t>(mode));
        StreamMessage(m);
        paceSent_ = mode;
    }
    // What the bot is doing, for the window.
    std::string Doing() const {
        if (!worldUnit) return phase_ == Phase::Lobby ? "waiting in the lobby" : "entering the quest";
        if (fightTarget_) return "fighting unit " + std::to_string(fightTarget_);
        return followHost ? "following the host" : "standing";
    }
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
            const float d = std::hypot(x - mx, y - my);
            if (d < dist) { dist = d; best = kv.first; }
        }
        return best;
    }
    bool fightInSight = false;  // fighting at all (Engagement not Passive)
    int engagement = 1;         // 0 passive, 1 defensive (the units attacking the host or us, the host's target), 2 aggressive (also anything in sight)
    float sight = 13.0f;        // the character's sight (map units)
    bool followHost = true;     // walk after the host's hero, staying about followDistance from it
    float followDistance = 4.0f;
    // The host's hero: the first other player in the quest world (the host is listed first).
    uint32_t HostUnit() const {
        for (const auto& kv : players) if (kv.second.unitId != worldUnit && kv.second.state == 0 && kv.second.unitId) return kv.second.unitId;
        return 0;
    }
    std::string profileName;    // the player profile sent when entering (empty: the character's name)
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
        if (state == State::Accepted) { Enter(); RespawnSteps(); }
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
        if (state == State::Accepted && worldUnit && pace != paceSent_) SetPace(pace);
        // Fight: the nearest unit (not a player's hero) within sight, attacked again every 3 s while it stays the target;
        // nothing in sight: follow the host as usual.
        bool fighting = false;
        if (state == State::Accepted && fightInSight && worldUnit && Now() - fightAt_ > 1) {
            fightAt_ = Now();
            float d = 0;
            uint32_t t = engagement >= 2 ? NearestOther(d) : 0; // aggressive: anything in sight
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
                        const float e = std::hypot(x - mx, y - my) + (hostsTarget ? 5.0f : 0.0f);
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
                if (t != fightTarget_) { fightTarget_ = t; targetSince_ = Now(); Log("attacking unit " + std::to_string(t)); }
                float x, y, mx, my;
                const auto th = unitHp_.find(t);
                if (th != unitHp_.end() && th->second != targetHp_) { targetHp_ = th->second; targetSince_ = Now(); } // it is being hurt: progress
                if (Now() - targetSince_ > 20) { ignored_[t] = Now() + 30; fightTarget_ = 0; fighting = false; } // no result: leave it a while
                else if (d > sight * 0.9f && Where(t, x, y) && Where(worldUnit, mx, my)) { // out of our sight: closer first
                    if (Now() - attackedAt_ > 4) { Log("closer to " + std::to_string(t) + " at " + std::to_string(d)); MoveTo(x + (mx - x) * sight * 0.5f / d, y + (my - y) * sight * 0.5f / d); attackedAt_ = Now(); attackedTarget_ = 0; }
                } else {
                    // Once per target; again only when our hero stands idle (action code 0xFE) for 2 s: a new order would
                    // restart the attack.
                    const auto me = unitAction_.find(worldUnit);
                    const bool idle = me == unitAction_.end() || me->second == 0xFE;
                    if (!idle) idleSince_ = Now();
                    if (attackedTarget_ != t || (idle && Now() - idleSince_ > 4 && Now() - targetSince_ > 4)) {
                        Log(std::string(attackedTarget_ != t ? "attack " : "attack again (idle 4 s) ") + std::to_string(t) + " at " + std::to_string(d));
                        Attack(t); attackedAt_ = Now(); attackedTarget_ = t; idleSince_ = Now();
                    }
                }
            } else {
                fightTarget_ = 0;
            }
        }
        if (fightTarget_ && !unitPos.count(fightTarget_)) fightTarget_ = 0;
        if (fightTarget_) fighting = true;
        if (!fighting && state == State::Accepted && followHost && worldUnit && Now() - followAt_ > 2) { // walk to the host when it is away
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
                Player p; p.name = str(); p.unit = str(); p.state = u32(); o += 1; p.unitId = u32(); u32(); players[id] = p;
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
            if (kind == 2 || kind == 3 || kind == 6) Log("server: " + text);
            else if (kind == 9) Log("the host chose the quest " + text);
            else if (kind == 8 && questStates[id] == chosenQuest + "_2") Log("the zone of " + chosenQuest + " is open: the host is in it");
            else if (kind == 1) Log("player " + players[id].name + ": state " + std::to_string(players[id].state));
        }
    }

    void Handle(const uint8_t* b, int n) {
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
