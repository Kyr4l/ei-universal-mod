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

static const char* const kEngineVersion = "0.3.2";

extern "C" size_t __cdecl UmStrlen(const char* text); // crt_strings.cpp
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

static HMODULE g_module = NULL;
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
