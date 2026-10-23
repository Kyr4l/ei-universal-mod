// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// See the other players in the base (PENDING #145): every player's base is a world of its own, so a unit of another player never exists in it. Here each machine sends
// its own hero's look and position to the others through the lobby's chat (a hidden line the receiving side swallows) and shows every other player as a "ghost":
// a hero unit that exists only in the local world, made through the game's own unit creation handler (docs/multiplayer-protocol.md "Ghost units").
//  * send (MpGhost, the frame hook): in the base, an appearance line "um!g|A|..." (name, figure, body, equipment) when it changes and every 8 s, and a motion line "um!g|P|..." (position,
//    velocity, animation) only when the hero's walk changes: the receiver walks the ghost on along the velocity (dead reckoning), so a click-walk costs a couple of lines, not 3 a second;
//  * receive (MpGhost, the chat line hook, the call of ChatSystemLine in LobbyClient::OnAddressLine 0x41E656): a line with the marker is taken out and not shown;
//  * the ghosts are made, moved and removed from the frame hook (the second Net_TickAll call of the client loop in main, 0x4573AE).
// A player without this DLL sees the lines as chat text and no ghosts. Everything is skipped outside a multiplayer base.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

extern "C" void UmEngineLog(const char* format, ...);

namespace {
const char kMarker[] = "um!g|";
const unsigned kHeroId = 1000011214u;     // the id of the hero of every .mp in a base world (0x3B9AF5CE)
const unsigned kGhostIdBase = 1000000100u;

char* const kWorld = reinterpret_cast<char*>(0x007AF5B8);   // CWorldClient
typedef void*(__attribute__((thiscall)) *FindFn)(void*, unsigned);   // CWorldClient::FindObjectById 0x55FAE0

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
template <class T> T Get(const void* base, size_t off) { T v{}; if (Readable(static_cast<const char*>(base) + off, sizeof(T))) std::memcpy(&v, static_cast<const char*>(base) + off, sizeof(T)); return v; }

bool Alive(int* lobby) { return lobby && (static_cast<unsigned>(lobby[1]) & 0x80000000u) == 0; }
int* LobbyClient() { return *reinterpret_cast<int* const*>(0x0079B5D8); }

bool InBase() {
    const char* q = *reinterpret_cast<const char* const*>(0x007AFE90);   // the quest name (a game string), empty outside a quest
    return Readable(q, 1) && q[0] == 0;
}

void* FindObject(unsigned id) {
    return reinterpret_cast<FindFn>(0x0055FAE0)(kWorld, id);
}

// ---- the local hero ------------------------------------------------------------------------------------
struct State {
    char name[48] = "";
    char figure[16] = "";
    unsigned proto = 97;
    float x = 0, y = 0, yaw = 0, complexion[3] = {0.5f, 0.5f, 0.5f};
    unsigned cur = 0xFF, que = 0xFE, pack = 0, pack2 = 0;
    int slot = -1;
    unsigned eq[11] = {};   // 4 weapons, 7 armour pieces (item codes: row << 16 | material)
};

bool SampleLocal(State& s) {
    char* hero = static_cast<char*>(FindObject(kHeroId));
    if (!Readable(hero, 0x300) || Get<unsigned>(hero, 0x10) != 0x32) return false;
    char* senses = Get<char*>(hero, 0x240);
    if (!Readable(senses, 0xD0)) return false;
    const char* name = Get<const char*>(hero, 0x238);
    const char* figure = Get<const char*>(hero, 0xA4);
    std::snprintf(s.name, sizeof s.name, "%s", Readable(name, 1) ? name : "?");
    for (char* c = s.name; *c; ++c) if (*c == '|') *c = '/';
    std::snprintf(s.figure, sizeof s.figure, "%s", Readable(figure, 1) ? figure : "unhuma");
    s.proto = Get<unsigned>(hero, 0x18);
    s.x = Get<float>(hero, 0x1C); s.y = Get<float>(hero, 0x20);
    const float qw = Get<float>(hero, 0x34), qz = Get<float>(hero, 0x40);
    s.yaw = 2.0f * std::atan2(qz, qw);
    for (int i = 0; i < 3; ++i) s.complexion[i] = Get<float>(hero, 0x1AC + 4 * i);
    s.cur = Get<uint8_t>(hero, 0x254); s.que = Get<uint8_t>(hero, 0x255);
    s.pack = Get<unsigned>(hero, 0x25C); s.pack2 = Get<unsigned>(hero, 0x260);
    s.slot = Get<int>(hero, 0x26C);
    for (int i = 0; i < 4; ++i) s.eq[i] = Get<unsigned>(senses, 0xBC + 4 * i);
    for (int i = 0; i < 7; ++i) s.eq[4 + i] = Get<unsigned>(senses, 0xA0 + 4 * i);
    return std::isfinite(s.x) && std::isfinite(s.y) && s.x > 0 && s.y > 0;
}

// One chat line to every player (the lobby's chat: the server sends it on to all, as the chat input does).
bool SendLine(int* lobby, const std::string& text) {
    void* msg = reinterpret_cast<void* (*)()>(0x004171A0)();   // ChatMessage::ctor2
    if (!msg) return false;
    const char* cursor = text.c_str();
    reinterpret_cast<void(__attribute__((thiscall)) *)(void*, const char**)>(0x00413B50)(msg, &cursor);   // ChatMessage::Read: sets the text
    reinterpret_cast<void(__attribute__((thiscall)) *)(void*, void*, unsigned)>(0x0041D1A0)(lobby, msg, 0x42700000u);   // LobbyClient::AddKeyedRef2604, as the chat input does
    return true;
}

std::string EncodeAppearance(const State& s, unsigned session) {
    char b[400];
    int n = std::snprintf(b, sizeof b, "%sA|%u|%s|%u|%s|%.3f|%.3f|%.3f|%d", kMarker, session, s.name, s.proto, s.figure, static_cast<double>(s.complexion[0]), static_cast<double>(s.complexion[1]),
                          static_cast<double>(s.complexion[2]), s.slot);
    for (unsigned e : s.eq) n += std::snprintf(b + n, sizeof b - static_cast<size_t>(n), "|%x", e);
    return b;
}
std::string EncodeMotion(const State& s, unsigned session, float vx, float vy) {
    char b[160];
    std::snprintf(b, sizeof b, "%sP|%u|%.2f|%.2f|%.2f|%.2f|%.2f|%u|%u|%u|%u", kMarker, session, static_cast<double>(s.x), static_cast<double>(s.y), static_cast<double>(vx), static_cast<double>(vy),
                  static_cast<double>(s.yaw), s.cur, s.que, s.pack, s.pack2);
    return b;
}

struct Line { char kind = 0; unsigned session = 0; State s; float vx = 0, vy = 0; };   // kind: A appearance, P motion, Q "send your appearance", B bye

bool Decode(const char* text, Line& out) {
    const char* p = std::strstr(text, kMarker);
    if (!p) return false;
    std::vector<std::string> f;
    std::string cur;
    for (p += sizeof(kMarker) - 1; *p; ++p) { if (*p == '|') { f.push_back(cur); cur.clear(); } else cur += *p; }
    f.push_back(cur);
    if (f.size() < 2 || (f[0].size() != 1 && f[0] != "bye")) return false;
    out = Line();
    State& s = out.s;
    out.session = static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
    if (f[0] == "bye") { out.kind = 'B'; return true; }
    out.kind = f[0][0];
    if (out.kind == 'Q') return true;
    if (out.kind == 'A') {
        if (f.size() < 9 + 11) return false;
        std::snprintf(s.name, sizeof s.name, "%s", f[2].c_str());
        s.proto = static_cast<unsigned>(std::strtoul(f[3].c_str(), nullptr, 10));
        std::snprintf(s.figure, sizeof s.figure, "%s", f[4].c_str());
        for (int i = 0; i < 3; ++i) s.complexion[i] = std::strtof(f[5 + static_cast<size_t>(i)].c_str(), nullptr);
        s.slot = static_cast<int>(std::strtol(f[8].c_str(), nullptr, 10));
        for (int i = 0; i < 11; ++i) s.eq[i] = static_cast<unsigned>(std::strtoul(f[9 + static_cast<size_t>(i)].c_str(), nullptr, 16));
        return s.proto < 4096 && s.figure[0] && std::strlen(s.figure) < 12;
    }
    if (out.kind == 'P') {
        if (f.size() < 11) return false;
        s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr);
        out.vx = std::strtof(f[4].c_str(), nullptr); out.vy = std::strtof(f[5].c_str(), nullptr);
        s.yaw = std::strtof(f[6].c_str(), nullptr);
        s.cur = static_cast<unsigned>(std::strtoul(f[7].c_str(), nullptr, 10)); s.que = static_cast<unsigned>(std::strtoul(f[8].c_str(), nullptr, 10));
        s.pack = static_cast<unsigned>(std::strtoul(f[9].c_str(), nullptr, 10)); s.pack2 = static_cast<unsigned>(std::strtoul(f[10].c_str(), nullptr, 10));
        return std::isfinite(s.x) && std::isfinite(s.y) && s.x > 0 && s.y > 0 && s.x < 2000 && s.y < 2000 && std::fabs(out.vx) < 30 && std::fabs(out.vy) < 30;
    }
    return false;
}

// ---- making and removing a unit that exists only here --------------------------------------------------
std::vector<uint8_t> g_payload;
size_t g_at = 0;
int __attribute__((thiscall)) FakeRead(void*, void* dst, int count) {
    if (count <= 0) return 0;
    if (g_at + static_cast<size_t>(count) > g_payload.size()) { std::memset(dst, 0, static_cast<size_t>(count)); g_at = g_payload.size(); return 0; }
    std::memcpy(dst, g_payload.data() + g_at, static_cast<size_t>(count));
    g_at += static_cast<size_t>(count);
    return count;
}
void PutU32(uint32_t v) { for (int i = 0; i < 4; ++i) g_payload.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void PutF32(float f) { uint32_t v; std::memcpy(&v, &f, 4); PutU32(v); }
void PutStr(const char* s) { while (*s) g_payload.push_back(static_cast<uint8_t>(*s++)); g_payload.push_back(0); }

// The game's delta coding with every chunk "changed" (DeltaReader::Read 0x47E000): a bit byte is taken from the stream when its 8 bits are used; each section starts a new one.
struct DeltaWriter {
    int mask = 0x80;
    size_t bitByte = 0;
    void Section() { mask = 0x80; }
    void Bit(bool b) {
        mask *= 2;
        if (mask > 0x80) { mask = 1; bitByte = g_payload.size(); g_payload.push_back(0); }
        if (b) g_payload[bitByte] = static_cast<uint8_t>(g_payload[bitByte] | mask);
    }
    void Value(const void* data, size_t n) {
        const uint8_t* v = static_cast<const uint8_t*>(data);
        Bit(false);
        for (size_t k = 0; k < n / 4; ++k) { Bit(false); g_payload.insert(g_payload.end(), v + 4 * k, v + 4 * k + 4); }
        if (n % 4) g_payload.insert(g_payload.end(), v + n - n % 4, v + n);
    }
};

// Runs a client handler of the local player's stream on our own bytes (the handler reads through the stream object's vtable slot 1).
int RunHandler(unsigned handler) {
    char* owner = Get<char*>(kWorld, 0x20C);
    if (!Readable(owner, 0x30)) return -1;
    char* player = Get<char*>(owner, 0x20);
    if (!Readable(player, 0x40) || Get<char*>(player, 0x1C) != owner) return -2;
    void*** stream = reinterpret_cast<void***>(player + 0x34);
    void** real = *stream;
    if (!Readable(real, 128)) return -3;
    void* fake[32];
    std::memcpy(fake, real, sizeof(fake));
    fake[1] = reinterpret_cast<void*>(&FakeRead);
    *stream = fake;
    const char savedFlag = player[0x39];
    player[0x39] = 1;   // the creation handler stops at once while this is 0
    typedef int(__attribute__((fastcall)) * HandlerFn)(void*);
    const int result = reinterpret_cast<HandlerFn>(handler)(player);
    player[0x39] = savedFlag;
    *stream = real;
    return result;
}

bool SpawnGhost(unsigned id, const State& s) {
    g_payload.clear();
    g_at = 0;
    PutU32(id); PutU32(0x32); PutU32(s.proto); PutU32(0x2);
    for (int i = 0; i < 3; ++i) PutF32(s.complexion[i]);
    PutStr("");
    PutStr(s.figure); PutStr(""); PutStr("");
    const char* parts[] = {"hp", "bd", "hd", "hr", "lh1", "lh2", "lh3", "rh1", "rh2", "rh3", "ll1", "ll2", "ll3", "rl1", "rl2", "rl3"};
    g_payload.push_back(static_cast<uint8_t>(sizeof(parts) / sizeof(parts[0]))); g_payload.push_back(0);
    for (const char* p : parts) PutStr(p);
    PutStr(s.name);
    PutU32(s.proto);
    g_payload.push_back(0xE4); g_payload.push_back(0x0F);   // sections 2, 5, 6, 7, 8, 9, 10, 11
    DeltaWriter w;
    {   // 2: position and rotation
        const uint8_t flag = 0;
        const float pos[3] = {s.x, s.y, 0.0f};
        const float q[4] = {std::cos(s.yaw / 2), 0.0f, 0.0f, std::sin(s.yaw / 2)};
        w.Section(); w.Value(&flag, 1); w.Value(pos, 12); w.Value(q, 16);
    }
    {   // 5 equipment, 6 stance, 7 packed, 8 animation, 9 name
        const uint32_t walk = 2;
        const uint32_t slot = static_cast<uint32_t>(s.slot);
        w.Section(); for (int i = 0; i < 11; ++i) w.Value(&s.eq[i], 4);
        w.Section(); w.Value(&walk, 4); w.Value(&walk, 4); w.Value(&slot, 4);
        const uint8_t packed = 0;
        w.Section(); w.Value(&packed, 1);
        const uint8_t idle = 0xFE, flags = 0;
        const unsigned tick = Get<unsigned>(kWorld, 0x64);
        w.Section(); w.Value(&idle, 1); w.Value(&idle, 1); w.Value(&tick, 4); w.Value(&flags, 1);
        w.Section(); PutStr(s.name);
    }
    {   // 10: no order, standing on its first step: without it the unit goes back to the origin every frame (the point v is placed at 2 * v + 48)
        const uint8_t mode = 0, hi = 0, lo = 1;
        w.Section(); w.Value(&mode, 1); w.Value(&hi, 1); w.Value(&lo, 1);
        const float vx = (s.x - 48.0f) / 2, vy = (s.y - 48.0f) / 2;
        const uint16_t head[3] = {static_cast<uint16_t>((vx + 32.0f) / 0.016601816f + 0.5f), static_cast<uint16_t>((vy + 32.0f) / 0.016601816f + 0.5f), 0};
        for (uint16_t v : head) { w.Bit(false); g_payload.push_back(static_cast<uint8_t>(v)); g_payload.push_back(static_cast<uint8_t>(v >> 8)); }
        const uint32_t zero = 0;
        for (int i = 0; i < 4; ++i) w.Value(&zero, 4);
        const uint16_t step[3] = {0x000F, static_cast<uint16_t>(s.x * 2), static_cast<uint16_t>(s.y * 2)};
        for (uint16_t v : step) { g_payload.push_back(static_cast<uint8_t>(v)); g_payload.push_back(static_cast<uint8_t>(v >> 8)); }
    }
    {   // 11: hostile mask, side, owner
        const uint32_t none = 0xFFFFFFFFu, mask = 0x3FFE;
        const uint8_t side = 0;
        w.Section(); w.Value(&mask, 4); w.Value(&side, 1); w.Value(&none, 4);
    }
    const int result = RunHandler(0x00667EC0);   // WorldClient::OnUnitCreated
    return result == 1 && g_at == g_payload.size();
}

void RemoveGhost(unsigned id) {   // CPlayerClient::OnMsg_DetachUnit: out of the player's units, released, out of the world
    g_payload.clear();
    g_at = 0;
    PutU32(id);
    RunHandler(0x00668850);
}

// ---- the others ------------------------------------------------------------------------------------------
struct Remote {
    State state;                      // appearance (A) and the last motion line (P)
    bool haveLook = false, haveMotion = false;
    float vx = 0, vy = 0;             // the walk the sender announced: the ghost goes on along it from where the line said (dead reckoning)
    DWORD motionAt = 0, seen = 0, askedAt = 0;
    float shownX = 0, shownY = 0;     // where the ghost is drawn (it eases to the target)
    unsigned id = 0; bool spawned = false; unsigned look = 0;
};
std::map<unsigned, Remote> g_remote;   // by the sender's session number
unsigned g_nextId = 0;
unsigned g_session = 0;
DWORD g_lastBaseCheck = 0, g_lastAppearance = 0, g_lastMotion = 0, g_lastQuery = 0;
State g_lastSentLook;
bool g_wasBase = false;
bool g_sendAppearanceNow = false;
// the sender's own walk
float g_prevX = 0, g_prevY = 0, g_sentX = 0, g_sentY = 0, g_sentVx = 0, g_sentVy = 0, g_vx = 0, g_vy = 0;
DWORD g_prevAt = 0, g_sentAt = 0;
unsigned g_sentCur = 0xFFFF, g_sentQue = 0xFFFF;

unsigned LookHash(const State& s) {   // what a new ghost has to be made again for: the figure, the body and the equipment
    unsigned h = s.proto * 2654435761u;
    for (unsigned e : s.eq) h = h * 31u + e;
    for (const char* c = s.figure; *c; ++c) h = h * 31u + static_cast<unsigned char>(*c);
    h = h * 31u + static_cast<unsigned>(s.slot + 1);
    return h;
}

void ForgetAll(bool remove) {
    for (auto& kv : g_remote) if (remove && kv.second.spawned && FindObject(kv.second.id)) RemoveGhost(kv.second.id);
    g_remote.clear();
}

// Where a remote hero is now: the position of its last line plus its velocity for the time since (at most 2 seconds: a line lost must not send the ghost far off).
void Target(const Remote& r, DWORD now, float& x, float& y) {
    const float dt = std::min(2.0f, static_cast<float>(now - r.motionAt) / 1000.0f);
    x = r.state.x + r.vx * dt;
    y = r.state.y + r.vy * dt;
}

void UpdateGhost(Remote& r, DWORD now, float seconds) {
    char* obj = static_cast<char*>(FindObject(r.id));
    if (!Readable(obj, 0x300)) { r.spawned = false; return; }
    char* senses = Get<char*>(obj, 0x240);
    if (!Readable(senses, 0x50)) return;
    const State& s = r.state;
    float tx, ty;
    Target(r, now, tx, ty);
    const float dx = tx - r.shownX, dy = ty - r.shownY, d = std::sqrt(dx * dx + dy * dy);
    if (d > 6.0f) { r.shownX = tx; r.shownY = ty; }                          // far: it jumped (a new spot)
    else { const float k = std::min(1.0f, seconds * 8.0f); r.shownX += dx * k; r.shownY += dy * k; }   // eased: a lost or late line does not make it jerk
    *reinterpret_cast<float*>(senses + 0x3C) = 2 * r.shownX;   // the unit's move state holds its place in half units: the object follows within a frame
    *reinterpret_cast<float*>(senses + 0x40) = 2 * r.shownY;
    float yaw = s.yaw;
    if (r.vx * r.vx + r.vy * r.vy > 0.04f) yaw = std::atan2(r.vx, -r.vy);   // facing the way it walks (the unit faces (sin yaw, -cos yaw))
    float* q = reinterpret_cast<float*>(obj + 0x34);
    q[0] = std::cos(yaw / 2); q[1] = 0; q[2] = 0; q[3] = std::sin(yaw / 2);
    if (Get<uint8_t>(obj, 0x254) != s.cur || Get<uint8_t>(obj, 0x255) != s.que) {
        *reinterpret_cast<uint8_t*>(obj + 0x254) = static_cast<uint8_t>(s.cur);
        *reinterpret_cast<uint8_t*>(obj + 0x255) = static_cast<uint8_t>(s.que);
        *reinterpret_cast<unsigned*>(obj + 0x258) = Get<unsigned>(kWorld, 0x64);
        *reinterpret_cast<unsigned*>(obj + 0x25C) = s.pack;
        *reinterpret_cast<unsigned*>(obj + 0x260) = s.pack2;
    }
}
}  // namespace

// The chat line hook: the call of ChatSystemLine (0x5F82F0) in LobbyClient::OnAddressLine (0x41E656). thiscall(the interface, the text, the line's number).
extern "C" void __attribute__((thiscall)) UmMpGhostChatLine(void* ui, const char* text, unsigned number) {
    if (Readable(text, 6) && std::strstr(text, kMarker)) {
        Line l;
        if (Decode(text, l) && l.session != g_session) {
            const DWORD now = GetTickCount();
            if (l.kind == 'Q') { g_sendAppearanceNow = true; return; }   // somebody lacks the looks of the players: everybody answers
            if (l.kind == 'B') { auto it = g_remote.find(l.session); if (it != g_remote.end()) it->second.seen = 0; return; }
            Remote& r = g_remote[l.session];
            r.seen = now;
            if (l.kind == 'A') {
                const State keep = r.state;
                r.state = l.s;
                if (r.haveMotion) { r.state.x = keep.x; r.state.y = keep.y; r.state.yaw = keep.yaw; r.state.cur = keep.cur; r.state.que = keep.que; r.state.pack = keep.pack; r.state.pack2 = keep.pack2; }
                r.haveLook = true;
            } else if (l.kind == 'P') {
                const State look = r.state;
                r.state = l.s;
                if (r.haveLook) { std::memcpy(r.state.name, look.name, sizeof look.name); std::memcpy(r.state.figure, look.figure, sizeof look.figure); r.state.proto = look.proto; r.state.slot = look.slot;
                                  std::memcpy(r.state.eq, look.eq, sizeof look.eq); std::memcpy(r.state.complexion, look.complexion, sizeof look.complexion); }
                r.vx = l.vx; r.vy = l.vy; r.motionAt = now;
                if (!r.haveMotion) { r.shownX = l.s.x; r.shownY = l.s.y; }
                r.haveMotion = true;
            }
        }
        return;
    }
    reinterpret_cast<void(__attribute__((thiscall)) *)(void*, const char*, unsigned)>(0x005F82F0)(ui, text, number);
}

// The frame hook: the call of Net_TickAll in the client loop of main (0x4573AE); the game's own function is called first.
extern "C" void UmMpGhostFrame() {
    reinterpret_cast<void (*)()>(0x004122D0)();
    const DWORD now = GetTickCount();
    if (now - g_lastBaseCheck < 100) return;
    const float seconds = std::min(0.5f, static_cast<float>(now - g_lastBaseCheck) / 1000.0f);
    g_lastBaseCheck = now;
    int* lobby = LobbyClient();
    const bool base = Alive(lobby) && InBase() && Readable(Get<char*>(kWorld, 0x20C), 0x30) && FindObject(kHeroId);
    if (!base) {
        if (g_wasBase) {
            ForgetAll(true);
            g_wasBase = false;
            if (Alive(lobby)) SendLine(lobby, std::string(kMarker) + "bye|" + std::to_string(g_session));   // the others take our ghost out at once
            UmEngineLog("um-engine: MpGhost: left the base");
        }
        return;
    }
    if (!g_wasBase) {
        g_wasBase = true;
        if (!g_session) g_session = (now ^ GetCurrentProcessId() * 2654435761u) | 1;
        g_remote.clear();
        g_lastAppearance = 0; g_lastMotion = 0; g_sentCur = 0xFFFF; g_prevAt = 0; g_sentVx = g_sentVy = 0;
        g_sendAppearanceNow = true;
        UmEngineLog("um-engine: MpGhost: in a multiplayer base (session %u)", g_session);
    }

    // send our own state
    State me;
    if (SampleLocal(me)) {
        if (LookHash(me) != LookHash(g_lastSentLook)) g_sendAppearanceNow = true;
        if (g_sendAppearanceNow || now - g_lastAppearance >= 8000) {
            if (SendLine(lobby, EncodeAppearance(me, g_session))) { g_lastSentLook = me; g_lastAppearance = now; g_sendAppearanceNow = false; }
        }
        // our velocity from the last two samples (smoothed), then the walk is announced when it differs from the one the others are following
        if (g_prevAt && now > g_prevAt) {
            const float dt = static_cast<float>(now - g_prevAt) / 1000.0f;
            const float vx = (me.x - g_prevX) / dt, vy = (me.y - g_prevY) / dt;
            g_vx += (vx - g_vx) * 0.6f; g_vy += (vy - g_vy) * 0.6f;
            if (std::fabs(g_vx) < 0.05f) g_vx = 0;
            if (std::fabs(g_vy) < 0.05f) g_vy = 0;
        }
        g_prevX = me.x; g_prevY = me.y; g_prevAt = now;
        const float age = static_cast<float>(now - g_sentAt) / 1000.0f;
        const float predictedX = g_sentX + g_sentVx * age, predictedY = g_sentY + g_sentVy * age;
        const float off = std::fabs(me.x - predictedX) + std::fabs(me.y - predictedY);
        const float dv = std::fabs(g_vx - g_sentVx) + std::fabs(g_vy - g_sentVy);
        const bool changed = off > 0.6f || dv > 0.9f || me.cur != g_sentCur || me.que != g_sentQue;
        if ((changed && now - g_lastMotion >= 200) || now - g_lastMotion >= 5000) {
            if (SendLine(lobby, EncodeMotion(me, g_session, g_vx, g_vy))) {
                g_sentX = me.x; g_sentY = me.y; g_sentVx = g_vx; g_sentVy = g_vy; g_sentAt = now; g_sentCur = me.cur; g_sentQue = me.que; g_lastMotion = now;
            }
        }
    }

    // make, move and remove the others'
    bool lacksLook = false;
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        Remote& r = it->second;
        if (now - r.seen > 15000 || r.seen == 0) {
            if (r.spawned && FindObject(r.id)) RemoveGhost(r.id);
            it = g_remote.erase(it);
            continue;
        }
        if (r.haveMotion && !r.haveLook) lacksLook = true;
        if (!(r.haveLook && r.haveMotion)) { ++it; continue; }
        if (r.spawned && (!FindObject(r.id) || r.look != LookHash(r.state))) {   // gone with its world, or it changed its look: made again
            UmEngineLog("um-engine: MpGhost: the ghost of %s is made again (%s)", r.state.name, FindObject(r.id) ? "its look changed" : "it was gone");
            if (FindObject(r.id)) RemoveGhost(r.id);
            r.spawned = false;
        }
        if (!r.spawned) {
            if (!r.id) r.id = kGhostIdBase + (g_nextId++ % 200);
            if (FindObject(r.id)) RemoveGhost(r.id);
            float tx, ty;
            Target(r, now, tx, ty);
            r.shownX = tx; r.shownY = ty;
            State at = r.state; at.x = tx; at.y = ty;
            r.spawned = SpawnGhost(r.id, at);
            r.look = LookHash(r.state);
            UmEngineLog("um-engine: MpGhost: %s %s at (%.1f, %.1f) as unit %u", r.spawned ? "made the ghost of" : "could not make the ghost of", r.state.name, static_cast<double>(tx), static_cast<double>(ty), r.id);
        }
        if (r.spawned) UpdateGhost(r, now, seconds);
        ++it;
    }
    if (lacksLook && now - g_lastQuery >= 1500) { g_lastQuery = now; SendLine(lobby, std::string(kMarker) + "Q|" + std::to_string(g_session)); }   // somebody's motion came without its looks: ask for them
}
