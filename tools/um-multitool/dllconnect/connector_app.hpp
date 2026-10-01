// The UM DLL Connector: um-multitool's tab that connects to um.dll's DLL server inside the running game
// (127.0.0.1, DLL_SERVER_PORT in um.cfg, 18888 by default) and shows what it reports. The protocol is
// described in resources/universal-mod/um-dll/dll_server.hpp. Everything lives in connector_app.cpp.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct Library;

namespace dllconnect {

struct Context;

// What the tab asks of the Map Editor: where the files of the map the game runs are (file names as
// um.dll reports them), and opening them there (it says what it did).
// gamePaths: the files' paths as the game opened them (um.dll reports them), for the names no map folder has.
struct Hooks {
    std::function<bool(const std::string& terrain, const std::string& base, const std::string& quest,
                       const std::vector<std::string>& gamePaths, std::string& terrainPath, std::vector<std::string>& mobPaths,
                       std::string& missing)> resolveMap;
    std::function<std::string(const std::string& terrain, const std::string& base, const std::string& quest,
                              const std::vector<std::string>& gamePaths)> openInMapEditor;
};

Context* Create(Library& lib, Hooks hooks); // lib: the GUI's shared settings (port, auto-connect, sub-tab)
void Destroy(Context* ctx);    // closes the connection

// Every frame, whichever tab is shown: reads what arrived, reconnects when asked to.
void Update(Context* ctx);
// Inside the ImGui frame, in the tab's content region.
void DrawTab(Context* ctx);

// `um-multitool dll <args>` (argv[0] is "dll"): commands to um.dll from a terminal.
int RunCli(int argc, char** argv);
void PrintCliHelp();

} // namespace dllconnect
