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

static const char* const kEngineVersion = "0.5.1";

extern "C" size_t __cdecl UmStrlen(const char* text); // crt_strings.cpp
extern "C" double __cdecl UmHypot(double x, double y); // crt_math.cpp
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
struct NetReader;
extern "C" bool __attribute__((thiscall)) UmNetReadMessage(NetConnection* self, NetReader* reader, unsigned packetSeq);  // net_message.cpp

static const UmEngineFunction kFunctions[] = {
    // { "FunctionName", 0x00400000, reinterpret_cast<void*>(&NewCode), 0 },
    { "strlen", 0x006ECCB0, reinterpret_cast<void*>(&UmStrlen), 0, {0x8B, 0x4C, 0x24, 0x04, 0xF7, 0xC1, 0x03, 0x00} }, // the Russian game.exe (EIStarter/Engine)
    { "NetSocket::SendTo", 0x00440B90, reinterpret_cast<void*>(&UmNetSendTo), 0, {0x8B, 0x44, 0x24, 0x08, 0x56, 0x57, 0x8B, 0x50} },
    { "NetSocket::ReceiveFrom", 0x00440BE0, reinterpret_cast<void*>(&UmNetReceiveFrom), 0, {0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57, 0x8B} },
    // The CRT's math (crt_math.cpp): the distance function, 22 % of the main thread's CPU time in the lobby
    { "_hypot", 0x006FD026, reinterpret_cast<void*>(&UmHypot), 0, {0xDD, 0x44, 0x24, 0x0C, 0x6A, 0x18, 0x51, 0x51} },
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
            unsigned char* at = reinterpret_cast<unsigned char*>(other.address);
            DWORD old = 0;
            if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
            const long rel = reinterpret_cast<long>(other.replacement) - reinterpret_cast<long>(at + 5);
            memcpy(at + 1, &rel, 4);
            VirtualProtect(at, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
        }
        UmEngineLog("um-engine: %s installed (call sites in game.exe)", site.name);
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
            DWORD old = 0;
            if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) { UmEngineLog("um-engine: cannot patch %s", fn.name); continue; }
            const long rel = reinterpret_cast<long>(fn.replacement) - reinterpret_cast<long>(at + 5);
            at[0] = 0xE9; memcpy(at + 1, &rel, 4);
            VirtualProtect(at, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
            UmEngineLog("um-engine: %s replaced (%s +0x%lX)", fn.name, fn.module, fn.rva);
        }
        if (remaining > 0) Sleep(2);
    }
    return 0;
}

static std::vector<UmEngineFunction> g_enabled;

// um-engine.log beside the DLL: research logging for the functions under test (cheap when nothing calls it).
extern "C" void UmEngineLog(const char* format, ...) {
    static std::string path;
    if (path.empty()) {
        char buffer[MAX_PATH] = {0};
        GetModuleFileNameA(g_module, buffer, MAX_PATH);
        path = buffer;
        const size_t slash = path.find_last_of("\\/");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "um-engine.log";
    }
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(f, format, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

static std::string ConfigPath() {
    char path[MAX_PATH] = "";
    GetModuleFileNameA(g_module, path, sizeof(path));
    std::string p = path;
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "um-engine.cfg";
}

// Reads um-engine.cfg (name -> on/off) and appends a "=true" line for every function it does not list yet.
static std::map<std::string, bool> ReadConfig() {
    std::map<std::string, bool> values;
    const std::string path = ConfigPath();
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s[0] == ';' || s[0] == '#') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string value = s.substr(eq + 1);
            for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            values[s.substr(0, eq)] = !(value == "false" || value == "0" || value == "off");
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
    for (const UmModuleFunction& fn : kModuleFunctions)
        if (fn.name && !values.count(fn.name)) {
            const bool on = fn.required >= 0;
            missing += std::string(fn.name) + (on ? "=true\n" : "=false\n");
            values[fn.name] = on;
        }
    const bool exists = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists || !missing.empty()) {
        if (FILE* f = std::fopen(path.c_str(), "a")) {
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
