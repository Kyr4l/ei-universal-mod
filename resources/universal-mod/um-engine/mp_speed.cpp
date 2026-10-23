// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Game speed x2 in multiplayer (RE and rig tests 2026-10-23, _cpr/PENDING.md #86).
//
// game.exe's main loop is a fixed time step: the server world ticks once per g_0x78FF34 ms of real time (55, or 27 at x2) and every
// client's own world ticks the same way. The speed buttons / keys only write that value when Lobby_IsAlive() is false, which is
// never the case in multiplayer. Setting it on the host alone doubles the host's server ticks but the clients keep 18 ticks a second
// and do not follow (tested with two real game.exe: client hero 1.87 units/s); set on the host AND every client, the tick counters stay
// in step (418 / 417 in 11 s) and the client's hero walks at 3.79 units/s = x2.
//
// So, with this DLL on every machine:
//  * MpSpeed::HostButtons: the six Lobby_IsAlive() calls in the speed buttons / keys answer "no lobby" for the HOST, so its buttons work;
//  * MpSpeed::Sync, host side: once a frame (the first Net_TickAll call of the main loop) the host looks at g_0x78FF34 and, when it changed
//    (and once a second while x2, for players who join later), sends every player the named value "um.speed" (2 = x2, 0 = normal) with the
//    game's own message 0x81 (CPlayerServer::SendMsg81_NamedValue, the one that carries "gtime"); the host's copy of the value is removed
//    again so it never reaches a saved character;
//  * client side: the receiving handler (WorldClient::OnQuestString) stores named values in the player's variables; here "um.speed" is taken
//    out before that (not stored) and kept in g_hostSpeed; the client then keeps g_0x78FF34 at the host's value every frame (the game
//    resets it to 55 when a screen closes or a zone loads) and sets it back to 55 when the host says normal or the game is left.
// A player without this DLL does not follow the host's x2 (he lags behind the host's world): all players need it before the host uses x2.
#include <cstring>
#include <windows.h>

extern "C" void UmEngineLog(const char* format, ...);

namespace {
volatile unsigned* const kStep = reinterpret_cast<volatile unsigned*>(0x0078FF34);       // ms per tick: 55 normal, 27 x2
const unsigned kStepNormal = 55, kStepFast = 27;
const char kName[] = "um.speed";

// A lobby object is alive when it exists and bit 31 of its second word is clear (Lobby_IsAlive 0x57F440).
bool Alive(int* lobby) { return lobby && (static_cast<unsigned>(lobby[1]) & 0x80000000u) == 0; }
bool InMultiplayer() { return Alive(*reinterpret_cast<int* const*>(0x0079B5D8)); }  // g_LobbyClient
bool IsHost() { return Alive(*reinterpret_cast<int* const*>(0x0079B5DC)); }         // the lobby server: only the host has one

int g_hostSpeed = 0;      // client: what the host last said: 0 nothing yet, 1 normal, 2 x2
bool g_enforced = false;  // client: g_0x78FF34 was changed by this DLL
int g_sent = 1;           // host: the last value sent (1 normal, 2 x2)
DWORD g_lastSend = 0;

typedef void* (__attribute__((thiscall)) *IterBeginFn)(void* iterator, void* manager);   // ManagerIter::Begin 0x404920
typedef void* (__attribute__((thiscall)) *IterNextFn)(void* iterator);                    // ManagerIter::Next 0x404960
typedef void (__attribute__((thiscall)) *SendNamedFn)(void* playerServer, const char* name, float value);  // CPlayerServer::SendMsg81_NamedValue 0x66EE10
typedef void (__attribute__((thiscall)) *MapRemoveFn)(void* map, const char* name);       // StringFloatMap_Remove 0x460DB0
typedef void (__attribute__((thiscall)) *MapSetFn)(void* map, const char* name, float value);  // StringFloatMap_Set 0x460BC0
typedef void* (__attribute__((thiscall)) *ManagerFn)(void* world);                         // CWorldServer vt[20]: the player manager

// Sends the value to every player of the world server (the loop of CWorldServer::BroadcastGameTime, 0x58D280).
void Broadcast(float value) {
    void* world = reinterpret_cast<void*>(0x007AFF60);   // CWorldServer
    void** vtable = *reinterpret_cast<void***>(world);
    void* manager = reinterpret_cast<ManagerFn>(vtable[0x50 / 4])(world);
    if (!manager) return;
    struct { void* manager; void* next; } iterator = { nullptr, nullptr };
    void* player = reinterpret_cast<IterBeginFn>(0x00404920)(&iterator, manager);
    while (player) {
        void* server = *reinterpret_cast<void**>(static_cast<char*>(player) + 0x20);   // its CPlayerServer
        if (server) {
            reinterpret_cast<SendNamedFn>(0x0066EE10)(server, kName, value);
            // SendMsg81_NamedValue also stores the value in the player's variables (owner +0x1C, table +0xC4): not in a saved character
            void* owner = *reinterpret_cast<void**>(static_cast<char*>(server) + 0x1C);
            if (owner) reinterpret_cast<MapRemoveFn>(0x00460DB0)(static_cast<char*>(owner) + 0xC4, kName);
        }
        player = reinterpret_cast<IterNextFn>(0x00404960)(&iterator);
    }
    if (iterator.manager) --*reinterpret_cast<int*>(static_cast<char*>(iterator.manager) + 4);   // the iterator's reference
}

void Frame() {
    if (!InMultiplayer()) {
        if (g_enforced) { *kStep = kStepNormal; g_enforced = false; UmEngineLog("um-engine: MpSpeed: left the game, speed back to normal"); }
        g_hostSpeed = 0;
        g_sent = 1;
        return;
    }
    if (IsHost()) {
        const int now = *kStep < 40 ? 2 : 1;
        const DWORD t = GetTickCount();
        if (now != g_sent || (now == 2 && t - g_lastSend >= 1000)) {
            if (now != g_sent) UmEngineLog("um-engine: MpSpeed: the host runs at %s, telling the players", now == 2 ? "x2" : "normal speed");
            Broadcast(now == 2 ? 2.0f : 0.0f);
            g_sent = now;
            g_lastSend = t;
        }
        return;
    }
    if (g_hostSpeed == 2) {
        if (!g_enforced) UmEngineLog("um-engine: MpSpeed: the host runs at x2");
        *kStep = kStepFast;
        g_enforced = true;
    } else if (g_hostSpeed == 1 && g_enforced) {
        *kStep = kStepNormal;
        g_enforced = false;
        UmEngineLog("um-engine: MpSpeed: the host is back to normal speed");
    }
}
}  // namespace

// The six Lobby_IsAlive() calls of the speed buttons / keys (0x57F440, no arguments, 1 = a multiplayer lobby is open): the host is allowed.
extern "C" int UmMpSpeedLobbyAlive() {
    return !IsHost() && InMultiplayer() ? 1 : 0;
}

// The first Net_TickAll() call of every frame in main (0x4122D0 is the game's function).
extern "C" void UmMpSpeedNetTick() {
    reinterpret_cast<void (*)()>(0x004122D0)();
    Frame();
}

// The call of StringFloatMap_Set inside the client's 0x81 handler (WorldClient::OnQuestString 0x669850): "um.speed" is not a variable.
extern "C" void __attribute__((thiscall)) UmMpSpeedNamedValue(void* map, const char* name, float value) {
    if (name && std::strcmp(name, kName) == 0) {
        g_hostSpeed = value > 1.5f ? 2 : 1;
        return;
    }
    reinterpret_cast<MapSetFn>(0x00460BC0)(map, name, value);
}
