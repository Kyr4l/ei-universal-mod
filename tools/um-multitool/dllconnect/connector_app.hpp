// The UM DLL Connector: um-multitool's tab that connects to um.dll's DLL server inside the running game
// (127.0.0.1, DLL_SERVER_PORT in um.cfg, 18888 by default) and shows what it reports. The protocol is
// described in resources/universal-mod/um-dll/dll_server.hpp. Everything lives in connector_app.cpp.
#pragma once

struct Library;

namespace dllconnect {

struct Context;

Context* Create(Library& lib); // lib: the GUI's shared settings (port, auto-connect, sub-tab)
void Destroy(Context* ctx);    // closes the connection

// Every frame, whichever tab is shown: reads what arrived, reconnects when asked to.
void Update(Context* ctx);
// Inside the ImGui frame, in the tab's content region.
void DrawTab(Context* ctx);

// `um-multitool dll <args>` (argv[0] is "dll"): commands to um.dll from a terminal.
int RunCli(int argc, char** argv);
void PrintCliHelp();

} // namespace dllconnect
