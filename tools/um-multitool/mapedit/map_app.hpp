// The Map Editor: um-multitool's third main tab (read-only for now: an .mpr terrain, several .mob
// files on top of each other, their objects and scripts, and the map checks), and the
// `um-multitool map ...` command-line mode. Everything lives in map_app.cpp.
#pragma once

#include <string>
#include <vector>

struct Library;

namespace mapedit {

struct Context;

Context* Create(Library& lib);     // lib: the GUI's shared sources; reopens the files of the last session
void Destroy(Context* ctx);        // needs the GL context current

// Opens these files instead of the last session's (.mpr = the terrain, the rest .mob, in load order).
void OpenFiles(Context* ctx, const std::vector<std::string>& paths);

// Inside the ImGui frame, in the Map Editor tab's content region.
void DrawTab(Context* ctx);
// After ImGui::Render: the 3D view into the region DrawTab reserved (nothing when it was not drawn).
void RenderGl(Context* ctx, int framebufferWidth, int framebufferHeight, float framebufferScale);

// Still loading figures (the GUI's --screenshot waits for it).
bool Busy(Context* ctx);

// The script editor in a window of its own (an OS window gui_main.cpp makes and draws into): whether
// it is wanted, its content (drawn in that window's ImGui context), what it asks for, and its closing.
bool ScriptWindowWanted(Context* ctx);
void DrawScriptWindow(Context* ctx);
bool TakeScriptWindowFocus(Context* ctx);
void CloseScriptWindow(Context* ctx);

// `um-multitool map <args>` (argv[0] is "map").
int RunCli(int argc, char** argv);
void PrintCliHelp();

} // namespace mapedit
