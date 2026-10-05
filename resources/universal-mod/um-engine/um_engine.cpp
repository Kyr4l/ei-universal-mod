// um-engine.dll: re-implementations of game.exe functions (PENDING #80). Loaded by um.dll (um.cfg UM_ENGINE=true),
// which asks for the list of replacements and redirects each game function to its new code with a jump.
//
// Its own config, um-engine.cfg beside the DLL: one line per replaced function, named after the game function,
//   <FunctionName>=true|false        (true by default; a missing file or line counts as true)
// The file is created, and lines added for new functions, when the DLL loads.
//
// Adding a function: write it in its own .cpp (same calling convention and arguments as the original) and add an
// entry to kFunctions below: { "FunctionName", 0x<game.exe address>, reinterpret_cast<void*>(&NewCode), required }.
// `required`: the mod needs it; it stays on whatever the config says.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "um_engine.h"

static const char* const kEngineVersion = "0.1.2a";

extern "C" size_t __cdecl UmStrlen(const char* text); // crt_strings.cpp

static const UmEngineFunction kFunctions[] = {
    // { "FunctionName", 0x00400000, reinterpret_cast<void*>(&NewCode), 0 },
    { "strlen", 0x006ECCB0, reinterpret_cast<void*>(&UmStrlen), 0, {0x8B, 0x4C, 0x24, 0x04, 0xF7, 0xC1, 0x03, 0x00} }, // the Russian game.exe (EIStarter/Engine)
    { nullptr, 0, nullptr, 0, {} } // end (kept so the table is never empty)
};

static HMODULE g_module = NULL;
static std::vector<UmEngineFunction> g_enabled;

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
        if (fn.name && !values.count(fn.name)) { missing += std::string(fn.name) + "=true\n"; values[fn.name] = true; }
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
        if (fn.required || it == config.end() || it->second) g_enabled.push_back(fn);
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
