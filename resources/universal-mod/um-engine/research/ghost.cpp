// SPDX-License-Identifier: GPL-3.0-or-later
// (research, not built by makemod: copy next to um_engine.cpp to try it; see docs/multiplayer-protocol.md "Ghost units")
// Copyright (C) 2026 Kyr4l
// Experiment for PENDING #145 (see the other players in the base, visual only): a unit created in the LOCAL world with no server object behind it,
// by feeding the game's own client handler of the unit creation (WorldClient::OnUnitCreated 0x667EC0, docs/multiplayer-protocol.md "Unit creation")
// a record built here through a stand-in input stream. Development only: UmGhostSpawn is called by hand (DLL server: CALL <address in um-engine.log>).
#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" void UmEngineLog(const char* format, ...);

namespace {
std::vector<uint8_t> g_payload;
size_t g_at = 0;
size_t g_over = 0;
float g_cellX = 122, g_cellY = 108;   // experiment: the step cell   // bytes the handler asked for beyond the payload

// The embedded stream's vtable slot 1: Read(buffer, count) (the handler and Stream_ReadCString only use this one).
int __attribute__((thiscall)) FakeRead(void*, void* dst, int count) {
    if (count <= 0) return 0;
    if (g_at + static_cast<size_t>(count) > g_payload.size()) { std::memset(dst, 0, static_cast<size_t>(count)); g_over += g_at + static_cast<size_t>(count) - g_payload.size(); g_at = g_payload.size(); return 0; }
    std::memcpy(dst, g_payload.data() + g_at, static_cast<size_t>(count));
    g_at += static_cast<size_t>(count);
    return count;
}

void PutU32(uint32_t v) { for (int i = 0; i < 4; ++i) g_payload.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void PutF32(float f) { uint32_t v; std::memcpy(&v, &f, 4); PutU32(v); }
void PutStr(const char* s) { while (*s) g_payload.push_back(static_cast<uint8_t>(*s++)); g_payload.push_back(0); }

// The game's delta coding (DeltaReader::Read 0x47E000), written so that every chunk is sent as "changed": a bit byte is reserved in the stream
// when a bit is needed and its 8 are used (low bit first), a section starts a new bit byte; the previous state does not matter.
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
        Bit(false);   // not "unchanged"
        for (size_t k = 0; k < n / 4; ++k) { Bit(false); g_payload.insert(g_payload.end(), v + 4 * k, v + 4 * k + 4); }
        if (n % 4) g_payload.insert(g_payload.end(), v + n - n % 4, v + n);
    }
};

// The other sections a hero's creation carries (CUnit::ReadNetState 0x52EF90): equipment 5, stance 6, packed bits 7, animation 8, the name 9, hostility 11.
void PutBodySections(DeltaWriter& w, const char* name, unsigned tick) {
    const uint32_t none = 0xFFFFFFFFu;
    w.Section();                                               // 5: 4 weapon slots + 7 armour pieces, -1 = empty
    for (int i = 0; i < 11; ++i) w.Value(&none, 4);
    w.Section();                                               // 6: stance, stance wanted, weapon slot (2 = walk, no weapon in hand)
    const uint32_t walk = 2;
    w.Value(&walk, 4); w.Value(&walk, 4); w.Value(&none, 4);
    w.Section();                                               // 7: four packed fields
    const uint8_t packed = 0;
    w.Value(&packed, 1);
    w.Section();                                               // 8: animation: current, queued, start tick, flags (0xFE = idle)
    const uint8_t idle = 0xFE, flags = 0;
    w.Value(&idle, 1); w.Value(&idle, 1); w.Value(&tick, 4); w.Value(&flags, 1);
    w.Section();                                               // 9: the player's name (raw)
    PutStr(name);
}
void PutSideSection(DeltaWriter& w) {                          // 11: hostile-to mask, side, owner (-1: none)
    const uint32_t none = 0xFFFFFFFFu, mask = 0x3FFE;
    const uint8_t side = 0;
    w.Section();
    w.Value(&mask, 4); w.Value(&side, 1); w.Value(&none, 4);
}

// Net-state section 10 (ReadUnitOrder 0x5C7FC0): "no order" (mode 0) with one step, the cell the unit stands on: the client unit's move replay (CUnitClientSpecific::AdvanceTime
// -> 0x535850) puts the unit back on the first step of its order every frame, so a unit without it snaps to the origin.
void PutStandingOrder(DeltaWriter& w, float x, float y, float cellX, float cellY) {
    const uint8_t mode = 0, hi = 0, lo = 1;
    w.Section();
    w.Value(&mode, 1); w.Value(&hi, 1); w.Value(&lo, 1);
    // the unit stands at 2 * v + 48 for the order's point v (measured: v = 10 gives 68.0, (20, 30) gives (88, 108); the step cells do not matter), v = u16 * 0.016601816 - 32
    const float vx = (x - 48.0f) / 2, vy = (y - 48.0f) / 2;
    const uint16_t head[3] = {static_cast<uint16_t>((vx + 32.0f) / 0.016601816f + 0.5f), static_cast<uint16_t>((vy + 32.0f) / 0.016601816f + 0.5f), 0};   // range 0
    for (uint16_t v : head) { w.Bit(false); g_payload.push_back(static_cast<uint8_t>(v)); g_payload.push_back(static_cast<uint8_t>(v >> 8)); }
    const uint32_t zero = 0;
    for (int i = 0; i < 4; ++i) w.Value(&zero, 4);
    const uint16_t step[3] = {0x000F, static_cast<uint16_t>(cellX), static_cast<uint16_t>(cellY)};   // both axes absolute; cells of half a unit
    for (uint16_t v : step) { g_payload.push_back(static_cast<uint8_t>(v)); g_payload.push_back(static_cast<uint8_t>(v >> 8)); }
}

// Net-state section 2 (CMapObject::ReadNetState): the flag byte, the position (x, y, z), the rotation quaternion about the vertical axis (w, x, y, z).
void PutPositionSection(DeltaWriter& w, float x, float y, float yaw) {
    const uint8_t flag = 0;
    const float pos[3] = {x, y, 0.0f};
    const float q[4] = {std::cos(yaw / 2), 0.0f, 0.0f, std::sin(yaw / 2)};
    w.Section();
    w.Value(&flag, 1);
    w.Value(pos, 12);
    w.Value(q, 16);
}

}  // namespace



// Creates a hero-like unit with this id in the local world (the unit starts at the world's origin: move it by writing its position, record +0x1C).
// Returns what the handler returned (1 = created).
extern "C" __declspec(dllexport) int UmGhostSpawn(unsigned id, unsigned proto, float x, float y, float yaw) {
    char* world = reinterpret_cast<char*>(0x007AF5B8);              // CWorldClient
    char* owner = *reinterpret_cast<char**>(world + 0x20C);         // the local player's object
    if (!owner) return -1;
    char* player = *reinterpret_cast<char**>(owner + 0x20);         // its CPlayerClient (the handler's `this`; its +0x1C is the owner again)
    if (!player || *reinterpret_cast<char**>(player + 0x1C) != owner) return -2;
    g_payload.clear();
    g_at = 0;
    g_over = 0;
    g_over = 0;
    PutU32(id); PutU32(0x32); PutU32(proto); PutU32(0x2);           // id, kind (a player's hero), prototype row, object flags
    PutF32(0.5f); PutF32(0.5f); PutF32(0.5f);                       // complexion
    PutStr("");                                                      // empty
    PutStr("unhuma"); PutStr(""); PutStr("");                        // figure, texture, label
    const char* parts[] = {"hp", "bd", "hd", "hr", "lh1", "lh2", "lh3", "rh1", "rh2", "rh3", "ll1", "ll2", "ll3", "rl1", "rl2", "rl3"};
    g_payload.push_back(static_cast<uint8_t>(sizeof(parts) / sizeof(parts[0]))); g_payload.push_back(0);
    for (const char* p : parts) PutStr(p);
    PutStr("Ghost");                                                 // name
    PutU32(proto);
    g_payload.push_back(0xE4); g_payload.push_back(0x0F);            // net state: the section mask (u16): sections 2, 5, 6, 7, 8, 9, 10, 11
    DeltaWriter writer;
    PutPositionSection(writer, x, y, yaw);
    PutBodySections(writer, "Ghost", *reinterpret_cast<unsigned*>(world + 0x64));
    PutStandingOrder(writer, x, y, g_cellX, g_cellY);
    PutSideSection(writer);
    void*** stream = reinterpret_cast<void***>(player + 0x34);       // the embedded input stream: its first word is its vtable
    void** real = *stream;
    void* fake[32];
    std::memcpy(fake, real, sizeof(fake));
    fake[1] = reinterpret_cast<void*>(&FakeRead);
    *stream = fake;
    UmEngineLog("um-engine: ghost experiment: x %g y %g yaw %g", static_cast<double>(x), static_cast<double>(y), static_cast<double>(yaw));
    UmEngineLog("um-engine: ghost experiment: player %p, flag +0x39 = %d, stream vtable %p", static_cast<void*>(player), player[0x39], static_cast<void*>(real));
    typedef int(__attribute__((fastcall)) * HandlerFn)(void*);
    const char savedFlag = player[0x39];
    player[0x39] = 1;   // the handler stops at once while this is 0 (what it means is not known yet)
    const int result = reinterpret_cast<HandlerFn>(0x00667EC0)(player);
    player[0x39] = savedFlag;
    *stream = real;
    {
        typedef void*(__attribute__((thiscall)) * FindFn)(void*, unsigned);
        void* obj = reinterpret_cast<FindFn>(0x0055FAE0)(world, id);   // CWorldClient::FindObjectById
        if (obj) {
            const float* p = reinterpret_cast<const float*>(static_cast<char*>(obj) + 0x1C);
            UmEngineLog("um-engine: ghost experiment: object %p at (%g, %g, %g) after the creation", obj, static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]));
        } else UmEngineLog("um-engine: ghost experiment: the object is not found by id");
    }
    UmEngineLog("um-engine: ghost experiment: OnUnitCreated(id %u, proto %u) returned %d, %u of %u payload bytes read, %u asked beyond the end", id, proto, result,
                static_cast<unsigned>(g_at), static_cast<unsigned>(g_payload.size()), static_cast<unsigned>(g_over));
    return result;
}

// Moves a unit made by UmGhostSpawn the way the game's own position update of a unit (section 2 of the net state, CMapObject::ReadNetState 0x50EC30) does:
// the world's slot 41 (position + the object grid), then the object's slot 0xCC.
extern "C" __declspec(dllexport) int UmGhostMove(void* object, float x, float y, float yaw) {
    char* obj = static_cast<char*>(object);
    char* world = reinterpret_cast<char*>(0x007AF5B8);
    float pos[3] = {x, y, *reinterpret_cast<float*>(obj + 0x24)};
    // the rotation quaternion about the vertical axis (w, x, y, z) at +0x34
    float* q = reinterpret_cast<float*>(obj + 0x34);
    q[0] = std::cos(yaw / 2); q[1] = 0; q[2] = 0; q[3] = std::sin(yaw / 2);
    typedef void(__attribute__((thiscall)) * WorldMoveFn)(void*, void*, float*);
    typedef void(__attribute__((thiscall)) * ObjectFn)(void*, float);
    void** worldVtable = *reinterpret_cast<void***>(world);
    reinterpret_cast<WorldMoveFn>(worldVtable[0xA4 / 4])(world, obj, pos);
    void** objVtable = *reinterpret_cast<void***>(obj);
    reinterpret_cast<ObjectFn>(objVtable[0xCC / 4])(obj, *reinterpret_cast<float*>(obj + 0x24));
    return 1;
}

extern "C" __declspec(dllexport) void UmGhostCell(float cx, float cy) { g_cellX = cx; g_cellY = cy; }

// An update of a ghost (the client handler of message 0x41, WorldClient::OnUnitUpdate 0x6687B0): the position and the standing order, plus the animation byte (0xFE idle; others untested).
extern "C" __declspec(dllexport) int UmGhostUpdate(unsigned id, float x, float y, float yaw, unsigned anim) {
    char* world = reinterpret_cast<char*>(0x007AF5B8);
    char* owner = *reinterpret_cast<char**>(world + 0x20C);
    if (!owner) return -1;
    char* player = *reinterpret_cast<char**>(owner + 0x20);
    if (!player) return -2;
    g_payload.clear();
    g_at = 0;
    g_over = 0;
    PutU32(id);
    g_payload.push_back(0x04); g_payload.push_back(0x05);   // mask: sections 2, 8 (0x100), 10 (0x400) -> 0x0504
    DeltaWriter writer;
    PutPositionSection(writer, x, y, yaw);
    {   // 8: animation
        const uint8_t a = static_cast<uint8_t>(anim), flags = 0;
        const unsigned tick = *reinterpret_cast<unsigned*>(world + 0x64);
        writer.Section();
        writer.Value(&a, 1); writer.Value(&a, 1); writer.Value(&tick, 4); writer.Value(&flags, 1);
    }
    PutStandingOrder(writer, x, y, g_cellX, g_cellY);
    void*** stream = reinterpret_cast<void***>(player + 0x34);
    void** real = *stream;
    void* fake[32];
    std::memcpy(fake, real, sizeof(fake));
    fake[1] = reinterpret_cast<void*>(&FakeRead);
    *stream = fake;
    typedef int(__attribute__((fastcall)) * HandlerFn)(void*);
    const int result = reinterpret_cast<HandlerFn>(0x006687B0)(player);
    *stream = real;
    UmEngineLog("um-engine: ghost update: id %u (%g, %g) returned %d, %u of %u payload bytes read, %u beyond", id, static_cast<double>(x), static_cast<double>(y), result,
                static_cast<unsigned>(g_at), static_cast<unsigned>(g_payload.size()), static_cast<unsigned>(g_over));
    return result;
}
