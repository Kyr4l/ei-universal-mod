// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um-engine.dll: re-implementations of game.exe functions (PENDING #80). Loaded by um.dll (um.cfg UM_ENGINE=true),
// which asks for the list of replacements and redirects each game function to its new code with a jump.
//
// Its own config, um-engine.cfg beside the DLL: one line per replaced function, named after the game function,
//   <FunctionName>=true|false        (true by default; a missing file or line counts as true)
// The file is created, and lines added for new functions, when the DLL loads.
//
// Adding a function: write it in its own .cpp (same calling convention and arguments as the original) and add an
// entry to kFunctions below: { "FunctionName", 0x<game.exe address>, reinterpret_cast<void*>(&NewCode), required }.
// `required`: 1 = the mod needs it, it stays on whatever the config says; -1 = experimental: off unless the
// config says true (a missing key is written as false, so an old um-engine.cfg never turns it on).
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "um_engine.h"

static const char* const kEngineVersion = "0.15.0";

extern "C" size_t __cdecl UmStrlen(const char* text); // crt_strings.cpp
extern "C" double __cdecl UmHypot(double x, double y); // crt_math.cpp
extern "C" int __attribute__((thiscall)) UmItemProtoType(unsigned itemId);   // item_proto.cpp (the weapon table lookups)
extern "C" long double __attribute__((fastcall)) UmItemPrice(unsigned itemId);
extern "C" long double __attribute__((fastcall)) UmItemPrice2(unsigned itemId);
extern "C" long double __attribute__((fastcall)) UmItemWeight(unsigned itemId);
extern "C" unsigned __attribute__((fastcall)) UmWeaponSkill(unsigned itemId);
extern "C" int __attribute__((fastcall)) UmWeaponDamageKind(const char* weapon);
extern "C" long double __attribute__((stdcall)) UmWeaponSkillGroupByte(unsigned itemId);
extern "C" void __attribute__((thiscall)) UmCameraTick(void*, float);  // camera_smooth.cpp
extern "C" void __attribute__((thiscall)) UmCameraFrame(void*, float);
extern "C" void __attribute__((thiscall)) UmFpLight(void*, const char*, const char*, unsigned*, const unsigned short*, int, void* const*, int, const float*); // fp_light.cpp
struct sockaddr; struct sockaddr_in; struct GameBuffer; // net_udp.cpp
extern "C" bool __attribute__((thiscall)) UmNetSendTo(unsigned* self, sockaddr* to, GameBuffer* buffer);
extern "C" bool __attribute__((thiscall)) UmNetReceiveFrom(unsigned* self, sockaddr_in* from, GameBuffer* buffer);
struct NetConnection;
extern "C" void __attribute__((thiscall)) UmNetWriteAcks(NetConnection* self, unsigned char** cursor);    // net_update.cpp
extern "C" void __attribute__((thiscall)) UmNetAckOne(NetConnection* self, unsigned short seq);
extern "C" void __attribute__((thiscall)) UmNetReadAcks(NetConnection* self, unsigned char** cursor);
extern "C" bool __attribute__((thiscall)) UmNetAcceptSeq(NetConnection* self, unsigned seq);
extern "C" void __attribute__((thiscall)) UmNetDrainChunks(NetConnection* self);
extern "C" bool __attribute__((thiscall)) UmNetParseUpdate(NetConnection* self, unsigned char** cursor);
extern "C" bool UmShadowResolution(unsigned size); // shadow_res.cpp
extern "C" void UmWorldEarlyInit(int shadowsInstalled); // world_shadow.cpp: the hooks of CWorldClient::Render (guard band, frame recorder, rig commands)
static bool g_sunShadowsInstalled = false;
extern "C" void __attribute__((thiscall)) UmWorldBeginStub(); // world_shadow.cpp (sun shadows: the call sites bracketing the world pass)
extern "C" void __attribute__((thiscall)) UmWorldEndStub();
extern "C" void __attribute__((thiscall)) UmWaterEndStub(); // water_border.cpp (the water's border: foam where the water meets the land; the call site after CTerrain::RenderWater)
extern "C" void __attribute__((thiscall)) UmMpGhostChatLine(void* ui, const char* text, unsigned number);   // mp_ghost.cpp (the other players in the base)
extern "C" void UmMpGhostFrame();
extern "C" int UmMpSpeedLobbyAlive();                                              // mp_speed.cpp (x2 speed in multiplayer)
extern "C" void UmMpSpeedNetTick();
extern "C" void __attribute__((thiscall)) UmMpSpeedNamedValue(void* map, const char* name, float value);
struct NetReader;
extern "C" bool __attribute__((thiscall)) UmNetReadMessage(NetConnection* self, NetReader* reader, unsigned packetSeq);  // net_message.cpp

static const UmEngineFunction kFunctions[] = {
    // { "FunctionName", 0x00400000, reinterpret_cast<void*>(&NewCode), 0 },
    { "strlen", 0x006ECCB0, reinterpret_cast<void*>(&UmStrlen), 0, {0x8B, 0x4C, 0x24, 0x04, 0xF7, 0xC1, 0x03, 0x00} }, // the Russian game.exe (EIStarter/Engine)
    { "NetSocket::SendTo", 0x00440B90, reinterpret_cast<void*>(&UmNetSendTo), 0, {0x8B, 0x44, 0x24, 0x08, 0x56, 0x57, 0x8B, 0x50} },
    { "NetSocket::ReceiveFrom", 0x00440BE0, reinterpret_cast<void*>(&UmNetReceiveFrom), 0, {0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57, 0x8B} },
    // The CRT's math (crt_math.cpp): the distance function, 22 % of the main thread's CPU time in the lobby
    { "_hypot", 0x006FD026, reinterpret_cast<void*>(&UmHypot), 0, {0xDD, 0x44, 0x24, 0x0C, 0x6A, 0x18, 0x51, 0x51} },
    // The item prototype lookup (item_proto.cpp): an item id past the end of the table returns 0 instead of crashing (closing the multiplayer menu with a mod-row weapon equipped)
    { "ItemId_GetProtoType", 0x006A2B80, reinterpret_cast<void*>(&UmItemProtoType), 0, {0xC1, 0xE9, 0x10, 0x56, 0x57, 0x8B, 0x3D, 0xF8} },
    { "ItemId_GetPrice", 0x006A2A50, reinterpret_cast<void*>(&UmItemPrice), 0, {0x53, 0x8B, 0x1D, 0xF8, 0x86, 0x7C, 0x00, 0x56} },
    { "ItemId_GetPrice2", 0x006A2AE0, reinterpret_cast<void*>(&UmItemPrice2), 0, {0x53, 0x8B, 0x1D, 0xF8, 0x86, 0x7C, 0x00, 0x56} },
    { "ItemId_GetWeight", 0x006A2BD0, reinterpret_cast<void*>(&UmItemWeight), 0, {0x53, 0x8B, 0x1D, 0xF8, 0x86, 0x7C, 0x00, 0x56} },
    { "CItemWeapon::GetSkill", 0x006A2C60, reinterpret_cast<void*>(&UmWeaponSkill), 0, {0xC1, 0xE9, 0x10, 0x56, 0x57, 0x8B, 0x3D, 0xF8} },
    { "CItemWeapon::GetDamageKind", 0x006A07F0, reinterpret_cast<void*>(&UmWeaponDamageKind), 0, {0x56, 0x8B, 0x35, 0xFC, 0x86, 0x7C, 0x00, 0x57} },
    { "Weapon_SkillGroupByte", 0x00526860, reinterpret_cast<void*>(&UmWeaponSkillGroupByte), 0, {0x56, 0x8B, 0x35, 0xFC, 0x86, 0x7C, 0x00, 0x57} },
    // The update packet codec (net_update.cpp): sequence window, acks, stream chunks, the update parser
    { "NetConnection::WriteAcks", 0x00434C20, reinterpret_cast<void*>(&UmNetWriteAcks), -1, {0x51, 0x8B, 0xD1, 0x53, 0x55, 0x56, 0x66, 0x8B} },
    { "NetConnection::AckOne", 0x00434CE0, reinterpret_cast<void*>(&UmNetAckOne), -1, {0x8B, 0x81, 0xF8, 0x10, 0x00, 0x00, 0x56, 0x57} },
    { "NetConnection::ReadAcks", 0x00434D70, reinterpret_cast<void*>(&UmNetReadAcks), -1, {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24} },
    { "NetConnection::AcceptSeq", 0x00434E50, reinterpret_cast<void*>(&UmNetAcceptSeq), -1, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF} },
    { "NetConnection::DrainChunks", 0x00434AE0, reinterpret_cast<void*>(&UmNetDrainChunks), -1, {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1} },
    { "NetConnection::ParseUpdate", 0x00436470, reinterpret_cast<void*>(&UmNetParseUpdate), -1, {0x6A, 0xFF, 0x68, 0xDB, 0x22, 0x71, 0x00, 0x64} },
    { "NetConnection::ReadMessage", 0x00435F30, reinterpret_cast<void*>(&UmNetReadMessage), -1, {0x6A, 0xFF, 0x68, 0xB8, 0x22, 0x71, 0x00, 0x64} },
    { nullptr, 0, nullptr, 0, {} } // end (kept so the table is never empty)
};

// Functions of the libraries game.exe loads (RE: _cpr/claude-re/dll/): patched once the library is in the process, at module + rva.
struct UmModuleFunction {
    const char* name;        // the key in um-engine.cfg
    const char* module;      // the library's file name
    unsigned long rva;       // where the function starts in it
    void* replacement;       // the new code (same calling convention and arguments)
    int required;            // as in UmEngineFunction
    unsigned char expect[8]; // the original's first 8 bytes
};
static const UmModuleFunction kModuleFunctions[] = {
    // 3DfpFPU.dll, the 3D pipeline: the per-vertex lighting (fp_light.cpp), ~40 % of that library's time
    { "3DfpFPU::LightVertices", "3DfpFPU.dll", 0xD910, reinterpret_cast<void*>(&UmFpLight), 0, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4} },
    { nullptr, nullptr, 0, nullptr, 0, {} }
};

extern "C" void UmEngineLog(const char* format, ...);

// Call sites inside game.exe (a `call rel32` is rewritten to call the replacement): all sites of one name are patched together, or none.
struct UmCallSite {
    const char* name;        // the key in um-engine.cfg
    unsigned long address;   // the call instruction
    void* replacement;
    int required;            // as in UmEngineFunction
    unsigned char expect[5]; // the original instruction
};
static const UmCallSite kCallSites[] = {
    // camera_smooth.cpp: the camera's fixed-tick lump (18 Hz) spread over the frames, the frame time measured precisely
    { "CCamera::SmoothUpdate", 0x004573A9, reinterpret_cast<void*>(&UmCameraTick),  0, {0xE8, 0x82, 0x87, 0xFB, 0xFF} },
    { "CCamera::SmoothUpdate", 0x0045756D, reinterpret_cast<void*>(&UmCameraFrame), 0, {0xE8, 0xBE, 0x85, 0xFB, 0xFF} },
    { "CCamera::SmoothUpdate", 0x00457595, reinterpret_cast<void*>(&UmCameraFrame), 0, {0xE8, 0x96, 0x85, 0xFB, 0xFF} },
    // mp_speed.cpp: game speed x2 in multiplayer (the host's buttons work, the players follow the host's speed)
    { "MpSpeed::HostButtons", 0x006490B6, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0x85, 0x63, 0xF3, 0xFF} },
    { "MpSpeed::HostButtons", 0x00649116, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0x25, 0x63, 0xF3, 0xFF} },
    { "MpSpeed::HostButtons", 0x00648DF7, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0x44, 0x66, 0xF3, 0xFF} },
    { "MpSpeed::HostButtons", 0x00648E5E, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0xDD, 0x65, 0xF3, 0xFF} },
    { "MpSpeed::HostButtons", 0x0063A6CD, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0x6E, 0x4D, 0xF4, 0xFF} },
    { "MpSpeed::HostButtons", 0x0063A733, reinterpret_cast<void*>(&UmMpSpeedLobbyAlive), 0, {0xE8, 0x08, 0x4D, 0xF4, 0xFF} },
    { "MpSpeed::Sync", 0x00457252, reinterpret_cast<void*>(&UmMpSpeedNetTick), 0, {0xE8, 0x79, 0xB0, 0xFB, 0xFF} },
    { "MpSpeed::Sync", 0x006698DF, reinterpret_cast<void*>(&UmMpSpeedNamedValue), 0, {0xE8, 0xDC, 0x72, 0xDF, 0xFF} },
    // mp_ghost.cpp: the other players shown in the base (their state travels as a hidden chat line)
    { "MpGhost", 0x0041E656, reinterpret_cast<void*>(&UmMpGhostChatLine), -1, {0xE8, 0x95, 0x9C, 0x1D, 0x00} },
    { "MpGhost", 0x004573AE, reinterpret_cast<void*>(&UmMpGhostFrame), -1, {0xE8, 0x1D, 0xAF, 0xFB, 0xFF} },
    // world_shadow.cpp: real sun shadows with D7VK (shadow map + screen-space resolve); the game's own projected-bitmap shadows are switched off while it works
    { "SunShadows", 0x0056A06B, reinterpret_cast<void*>(&UmWorldBeginStub), -1, {0xE8, 0xC0, 0xC1, 0x15, 0x00} },
    { "SunShadows", 0x0056A080, reinterpret_cast<void*>(&UmWorldEndStub),   -1, {0xE8, 0x5B, 0x0F, 0x00, 0x00} },
    // water_border.cpp: foam where the water meets the land (needs D7VK): right after the game has drawn the water layer
    { "WaterBorder", 0x0056A120, reinterpret_cast<void*>(&UmWaterEndStub), -1, {0xE8, 0x1B, 0xD9, 0xF7, 0xFF} },
    { nullptr, 0, nullptr, 0, {} }
};

static void InstallCallSites(const std::map<std::string, bool>& config) {
    std::map<std::string, bool> done;
    for (const UmCallSite& site : kCallSites) {
        if (!site.name || done.count(site.name)) continue;
        done[site.name] = true;
        const auto it = config.find(site.name);
        if (!(site.required > 0 || (it != config.end() ? it->second : site.required >= 0))) continue;
        bool match = true;
        for (const UmCallSite& other : kCallSites)
            if (other.name && std::string(other.name) == site.name && memcmp(reinterpret_cast<void*>(other.address), other.expect, 5) != 0) match = false;
        if (!match) { UmEngineLog("um-engine: %s skipped, game.exe has other code at its call sites (another build)", site.name); continue; }
        for (const UmCallSite& other : kCallSites) {
            if (!other.name || std::string(other.name) != site.name) continue;
            UmPatchJump(other.address, 0xE8, other.replacement);
        }
        if (std::string(site.name) == "SunShadows") g_sunShadowsInstalled = true;
        UmEngineLog("um-engine: %s installed (call sites in game.exe)", site.name);
    }
}

// Inlined copies of a function inside other game.exe functions: the instruction is replaced by NOPs / a shorter one (len bytes, all sites of one name together or none).
struct UmBytePatch {
    const char* name;        // the key in um-engine.cfg
    unsigned long address;
    int length;              // 3
    unsigned char expect[4]; // the original bytes
    unsigned char replace[4];
    int required;            // as in UmEngineFunction
};
static const UmBytePatch kBytePatches[] = {
    // item_proto.cpp: three inlined copies of the weapon prototype lookup that read the kind (+0xC) of a NULL record when the index is outside the table;
    // the register holding the null pointer is zero already (xor), so no instruction is the same as "kind 0".
    { "ItemTable::InlineLookups", 0x005354FB, 3, {0x8B, 0x49, 0x0C, 0}, {0x90, 0x90, 0x90, 0}, 0 },   // ApplyWeaponAnimMask: mov ecx, [ecx+0xC]
    // Hero_BuildEquipmentEntries (0x5683D0), the null record of the WEAPON table: after `xor eax, eax` at 0x5685A6 only this path reaches the load (the valid path returns at 0x5685A1),
    // so the load goes and eax stays 0, the field stored is 0. (The armour lookups at 0x56849C / 0x568529 are shared with their valid paths: not patched; an earlier version broke them.)
    { "ItemTable::InlineLookups", 0x005685A8, 3, {0x8B, 0x40, 0x0C, 0}, {0x90, 0x90, 0x90, 0}, 0 },
    { nullptr, 0, 0, {0}, {0}, 0 }
};

static void InstallBytePatches(const std::map<std::string, bool>& config) {
    std::map<std::string, bool> done;
    for (const UmBytePatch& patch : kBytePatches) {
        if (!patch.name || done.count(patch.name)) continue;
        done[patch.name] = true;
        const auto it = config.find(patch.name);
        if (!(patch.required > 0 || (it != config.end() ? it->second : patch.required >= 0))) continue;
        bool match = true;
        for (const UmBytePatch& other : kBytePatches)
            if (other.name && std::string(other.name) == patch.name && memcmp(reinterpret_cast<void*>(other.address), other.expect, static_cast<size_t>(other.length)) != 0) match = false;
        if (!match) { UmEngineLog("um-engine: %s skipped, game.exe has other code at its patch sites (another build)", patch.name); continue; }
        for (const UmBytePatch& other : kBytePatches) {
            if (!other.name || std::string(other.name) != patch.name) continue;
            UmPatchCode(reinterpret_cast<void*>(other.address), other.replace, static_cast<size_t>(other.length));
        }
        UmEngineLog("um-engine: %s installed (patched in game.exe)", patch.name);
    }
}

static HMODULE g_module = NULL;
static std::vector<UmModuleFunction> g_enabledModule;
static HANDLE g_moduleThread = NULL;

// Waits for each library to be loaded by the game, then redirects its functions with a jump (the library's code is not running yet:
// the game loads it when it starts the renderer, long before the first frame).
static DWORD WINAPI ModulePatchThread(LPVOID) {
    std::vector<bool> done(g_enabledModule.size(), false);
    size_t remaining = g_enabledModule.size();
    for (int waited = 0; remaining > 0 && waited < 60000; waited += 2) {
        for (size_t i = 0; i < g_enabledModule.size(); ++i) {
            if (done[i]) continue;
            HMODULE lib = GetModuleHandleA(g_enabledModule[i].module);
            if (!lib) continue;
            const UmModuleFunction& fn = g_enabledModule[i];
            unsigned char* at = reinterpret_cast<unsigned char*>(lib) + fn.rva;
            done[i] = true; --remaining;
            if (memcmp(at, fn.expect, 8) != 0) { UmEngineLog("um-engine: %s skipped, %s has other code at +0x%lX (another build)", fn.name, fn.module, fn.rva); continue; }
            if (!UmPatchJump(reinterpret_cast<unsigned long>(at), 0xE9, fn.replacement)) { UmEngineLog("um-engine: cannot patch %s", fn.name); continue; }
            UmEngineLog("um-engine: %s replaced (%s +0x%lX)", fn.name, fn.module, fn.rva);
        }
        if (remaining > 0) Sleep(2);
    }
    return 0;
}

static std::vector<UmEngineFunction> g_enabled;

// um-engine's log lines go to um.log through the function um.dll hands over (UmEngineSetLogger, before anything is installed): one log file for the mod. Without it (an um.dll that
// does not know the function) they go to um-engine.log beside the DLL as before.
static void (*g_logger)(const char* level, const char* text) = nullptr;

extern "C" __declspec(dllexport) void UmEngineSetLogger(void (*log)(const char* level, const char* text)) { g_logger = log; }

extern "C" void UmEngineLog(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_logger) {
        // problems are warnings (the counts um.log keeps), the research counters ("dbg") are debug lines, the rest information
        const char* level = std::strstr(text, " skipped") || std::strstr(text, "other code") || std::strstr(text, ": off") || std::strstr(text, "not applied") || std::strstr(text, "not as expected")
            ? "WARN" : std::strstr(text, "dbg ") ? "DEBUG" : "INFO";
        g_logger(level, text);
        return;
    }
    static std::string path;
    if (path.empty()) {
        char buffer[MAX_PATH] = {0};
        GetModuleFileNameA(g_module, buffer, MAX_PATH);
        path = buffer;
        const size_t slash = path.find_last_of("\\/");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "um-engine.log";
    }
    if (FILE* f = std::fopen(path.c_str(), "a")) {
        std::fputs(text, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

static const char* const kShadowResolutionKey = "UnitShadow::Resolution"; // a number (64 = the game's own), not on / off

// Numeric keys of um-engine.cfg ("Name=1.5"): default values, written to the file when missing.
static const struct { const char* key; const char* value; const char* comment; } kNumberKeys[] = {
    { "SunShadows::Enabled",   "1",      "SunShadows: 0 switches the sun shadows off while playing (the game's own come back); checked once a second" },
    { "SunShadows::MapSize",   "4096",   "SunShadows: the largest shadow map, in texels (1024 / 2048 / 4096; the map follows the visible area at about 0.04 world units a texel)" },
    { "SunShadows::Strength",  "1",      "SunShadows: 0..1, how much of the sun's light a shadow takes (1 = all of it: as dark as the ambient light)" },
    { "SunShadows::Bias",      "0.0015", "SunShadows: depth bias against shadow acne (light depth units)" },
    { "SunShadows::SlopeBias", "0.004",  "SunShadows: extra bias on surfaces edge-on to the sun" },
    { "SunShadows::Margin",    "8",      "SunShadows: world units added around the visible ground (casters just off screen)" },
    { "SunShadows::Height",    "50",     "SunShadows: half of the casters' height range along the light (world units)" },
    { "SunShadows::Extra",     "0.3",    "SunShadows: 0..1, extra darkness inside shadows on top of the game's own sun / ambient ratio (0 = none, 0.3 reads clearly)" },
    { "SunShadows::Indoor",    "0",      "SunShadows: 1 also shadows caves (the game gives them a fixed direction instead of a sun), 0 keeps the game's own shadows there" },
    { "SunShadows::Guard",     "0.3",   "SunShadows: 0..0.6, at most this much of the screen's size is also drawn beyond an edge, on the sides the sun's shadows come from, so casters just off screen shadow too (0 = off; needs a restart)" },
    { "SunShadows::GuardLog",  "0",      "SunShadows: 1 logs the guard band's problems, 2 also every change of its pads / the shadowed area and a frame count every 5 s (research)" },
    { "WaterBorder::Width",    "0.35",   "WaterBorder: width of the foam line inside the water, in world units (a step is about 1)" },
    { "WaterBorder::Strength", "0.55",   "WaterBorder: 0..1, how visible the shore is (foam, paler shallows, wet ground); 0 = off" },
    { "WaterBorder::Wet",      "0.3",    "WaterBorder: how far the ground above the waterline looks wet (darker), in world units" },
    { "WaterBorder::Speed",    "1",      "WaterBorder: speed of the foam's movement (0 = still)" },
    { "Recorder::Enabled",     "1",      "Recorder: 0 switches the frame recorder and its hotkey off (needs a restart)" },
    { "Recorder::Key",         "19",      "Recorder: this key (a virtual-key code: 19 = Pause, 119 = F8, 0 = none) writes the last seconds as a report + picture into um-reports\\ beside the DLL" },
    { "Recorder::Ctrl",        "0",      "Recorder: 1 needs Ctrl held with the key" },
    { "Recorder::Burst",       "0",      "Recorder: 1 keeps the last frames of the world as pictures and writes them with each report (a flicker of one or two frames; costs a few FPS)" },
    { "Recorder::BurstFrames", "240",    "Recorder: frames kept by the burst (30..1200; 240 is 1.5 s at 160 FPS, about 0.4 MB each)" },
    { "Recorder::BurstScale",  "4",      "Recorder: the burst pictures are 1/this of the screen in each direction (1..8)" },
    { "Recorder::Frames",      "600",    "Recorder: how many world frames the report holds (60..7200; about ten seconds at 60 FPS)" },
    { "Recorder::Screenshot",  "1",      "Recorder: 1 also saves a PNG of the world (no interface) with each report, 2 at half the size (a quarter of the bytes), 0 none" },
    { "Recorder::MaxReports",  "20",     "Recorder: at most this many reports are kept in um-reports\\ (the oldest goes first)" },
    { "SunShadows::Debug",     "0",      "SunShadows: research views (1 shadow term only, 2 lit fraction / sun angle, 4 normals, 5 world position)" },
};

extern "C" double UmEngineConfigNumber(const char* key, double fallback);

static std::string ConfigPath() {
    char path[MAX_PATH] = "";
    GetModuleFileNameA(g_module, path, sizeof(path));
    std::string p = path;
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "um-engine.cfg";
}

// One line of um-engine.cfg: "key = value ; comment". Spaces / tabs around the key and the value, a BOM, CR LF and an inline ';' / '#' comment are all accepted.
static bool ParseCfgLine(const char* line, std::string& key, std::string& value) {
    std::string s = line;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) s.erase(0, 3);
    auto trim = [](std::string& t) {
        size_t b = 0; while (b < t.size() && (t[b] == ' ' || t[b] == '\t' || t[b] == '\r' || t[b] == '\n')) ++b;
        size_t e = t.size(); while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\t' || t[e - 1] == '\r' || t[e - 1] == '\n')) --e;
        t = t.substr(b, e - b);
    };
    trim(s);
    if (s.empty() || s[0] == ';' || s[0] == '#') return false;
    const size_t eq = s.find('=');
    if (eq == std::string::npos) return false;
    key = s.substr(0, eq); value = s.substr(eq + 1);
    const size_t comment = value.find_first_of(";#");
    if (comment != std::string::npos) value.erase(comment);
    trim(key); trim(value);
    return !key.empty();
}

// Reads um-engine.cfg (name -> on/off) and appends a "=true" line for every function it does not list yet.
static std::map<std::string, bool> ReadConfig() {
    std::map<std::string, bool> values;
    const std::string path = ConfigPath();
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::string key, value;
            if (!ParseCfgLine(line, key, value)) continue;
            for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            values[key] = !(value == "false" || value == "0" || value == "off" || value == "no");
        }
        std::fclose(f);
    }
    std::string missing;
    for (const UmEngineFunction& fn : kFunctions)
        if (fn.name && !values.count(fn.name)) {
            const bool on = fn.required >= 0;
            missing += std::string(fn.name) + (on ? "=true\n" : "=false\n");
            values[fn.name] = on;
        }
    for (const UmCallSite& fn : kCallSites)
        if (fn.name && !values.count(fn.name)) {
            const bool on = fn.required >= 0;
            missing += std::string(fn.name) + (on ? "=true\n" : "=false\n");
            values[fn.name] = on;
        }
    for (const UmBytePatch& fn : kBytePatches)
        if (fn.name && !values.count(fn.name)) {
            const bool on = fn.required >= 0;
            missing += std::string(fn.name) + (on ? "=true\n" : "=false\n");
            values[fn.name] = on;
        }
    for (const UmModuleFunction& fn : kModuleFunctions)
        if (fn.name && !values.count(fn.name)) {
            const bool on = fn.required >= 0;
            missing += std::string(fn.name) + (on ? "=true\n" : "=false\n");
            values[fn.name] = on;
        }
    if (!values.count(kShadowResolutionKey)) missing += std::string(kShadowResolutionKey) + "=64\n";
    for (const auto& number : kNumberKeys)
        if (!values.count(number.key)) missing += std::string("; ") + number.comment + "\n" + number.key + "=" + number.value + "\n";
    const bool exists = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists || !missing.empty()) {
        bool endsInNewline = true;   // the appended lines must start on a line of their own
        if (exists) if (FILE* r = std::fopen(path.c_str(), "rb")) { if (std::fseek(r, -1, SEEK_END) == 0) endsInNewline = std::fgetc(r) == '\n'; std::fclose(r); }
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            if (!endsInNewline) std::fputc('\n', f);
            if (!exists)
                std::fputs("; um-engine.cfg: re-implemented game.exe functions, named after the game function.\n"
                           "; <FunctionName>=true uses the new code, false keeps the game's own. Missing = true.\n"
                           "; The whole DLL is switched with UM_ENGINE in um.cfg.\n", f);
            std::fputs(missing.c_str(), f);
            std::fclose(f);
        }
    }
    return values;
}

extern "C" double UmEngineConfigNumber(const char* key, double fallback) {
    double result = fallback;
    if (FILE* f = std::fopen(ConfigPath().c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::string k, v;
            if (ParseCfgLine(line, k, v) && k == key) result = std::atof(v.c_str());
        }
        std::fclose(f);
    }
    return result;
}

// Several numbers with one pass over the file; false (the values stay as they are) when the file cannot be read
extern "C" bool UmEngineConfigNumbers(const char* const* keys, double* values, int count) {
    FILE* f = std::fopen(ConfigPath().c_str(), "r");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string k, v;
        if (!ParseCfgLine(line, k, v)) continue;
        for (int i = 0; i < count; ++i) if (k == keys[i]) values[i] = std::atof(v.c_str());
    }
    std::fclose(f);
    return true;
}

// The folder of um-engine.dll (with a trailing backslash): the recorder's reports go beside it
extern "C" bool UmEngineDirectory(char* out, int size) {
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA(g_module, path, sizeof(path))) return false;
    char* slash = std::strrchr(path, '\\');
    if (!slash) return false;
    slash[1] = 0;
    if (static_cast<int>(std::strlen(path)) >= size) return false;
    std::strcpy(out, path);
    return true;
}

extern "C" __declspec(dllexport) unsigned UmEngineAbi() { return UM_ENGINE_ABI; }

extern "C" __declspec(dllexport) const char* UmEngineVersion() { return kEngineVersion; }

// The replacements to install (those switched on, and the required ones). `count` receives their number.
extern "C" __declspec(dllexport) const UmEngineFunction* UmEngineFunctions(int* count) {
    const std::map<std::string, bool> config = ReadConfig();
    g_enabled.clear();
    for (const UmEngineFunction& fn : kFunctions) {
        if (!fn.name) continue;
        const auto it = config.find(fn.name);
        if (fn.required > 0 || (it != config.end() ? it->second : fn.required >= 0)) g_enabled.push_back(fn);
    }
    g_enabledModule.clear();
    for (const UmModuleFunction& fn : kModuleFunctions) {
        if (!fn.name) continue;
        const auto it = config.find(fn.name);
        if (fn.required > 0 || (it != config.end() ? it->second : fn.required >= 0)) g_enabledModule.push_back(fn);
    }
    InstallCallSites(config);
    UmWorldEarlyInit(g_sunShadowsInstalled ? 1 : 0);
    InstallBytePatches(config);
    { // the unit shadow size: a number
        double resolution = 64;
        if (FILE* f = std::fopen(ConfigPath().c_str(), "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                std::string k, v;
                if (ParseCfgLine(line, k, v) && k == kShadowResolutionKey) resolution = std::atof(v.c_str());
            }
            std::fclose(f);
        }
        if (resolution > 64) UmShadowResolution(static_cast<unsigned>(resolution));
    }
    if (!g_enabledModule.empty() && !g_moduleThread) g_moduleThread = CreateThread(NULL, 0, ModulePatchThread, NULL, 0, NULL);
    if (count) *count = static_cast<int>(g_enabled.size());
    return g_enabled.empty() ? nullptr : g_enabled.data();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
