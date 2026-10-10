// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Map Editor: um-multitool's third main tab (read-only for now: an .mpr terrain, several .mob
// files on top of each other, their objects and scripts, and the map checks), and the
// `um-multitool map ...` command-line mode. Everything lives in map_app.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Library;

namespace questmap { struct Input; }

namespace mapedit {

struct Context;

Context* Create(Library& lib);     // lib: the GUI's shared sources; reopens the files of the last session
void Destroy(Context* ctx);        // needs the GL context current

// Opens these files instead of the last session's (.mpr = the terrain, the rest .mob, in load order).
void OpenFiles(Context* ctx, const std::vector<std::string>& paths, uint32_t focusId = 0); // focusId: gui --map --focus
void SetMode(Context* ctx, const std::string& mode); // "object", "paint", "ground" (paint by material), "sculpt", "water": gui --map --mode

// For the Texture Editor's quest map generator: the open terrain and the objects that become icons / forests.
bool FillQuestMapInput(Context* ctx, questmap::Input& in, std::string& err);

// Inside the ImGui frame, in the Map Editor tab's content region.
void DrawTab(Context* ctx);
// After ImGui::Render: the 3D view into the region DrawTab reserved (nothing when it was not drawn).
void RenderGl(Context* ctx, int framebufferWidth, int framebufferHeight, float framebufferScale);

// The map the game runs (um.dll's MAP: file names): its files' paths (from the map folders, the quest
// folders and next to the quest), and what was not found; OpenGameMap opens them (the quest when there
// is one) and says what it did.
// gamePaths: the files' paths as the game opened them (um.dll's MAP), used for the names no map folder has.
bool ResolveGameMap(Context* ctx, const std::string& terrain, const std::string& base, const std::string& quest,
                    const std::vector<std::string>& gamePaths, std::string& terrainPath, std::vector<std::string>& mobPaths,
                    std::string& missing);
std::string OpenGameMap(Context* ctx, const std::string& terrain, const std::string& base, const std::string& quest,
                        const std::vector<std::string>& gamePaths);

// The map checks (including the scripts') found more errors or warnings than the time before: how many
// more, and how many in all. Reported once (for the alerts, alerts.hpp).
bool TakeNewProblems(Context* ctx, int& errors, int& warnings, int& totalErrors, int& totalWarnings);
void ShowChecks(Context* ctx); // opens the Checks side tab
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
