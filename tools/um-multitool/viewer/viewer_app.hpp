// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The 3D Viewer: um-multitool's second main tab (formerly um-modelviewer2), and its
// `um-multitool viewer ...` command-line modes. Everything else lives in viewer_app.cpp.
#pragma once

#include <string>

struct Library;

namespace viewer {

struct Context;

Context* Create(Library& lib);     // lib: the GUI's shared sources (outlives the context)
void Destroy(Context* ctx);        // likewise

// Inside the ImGui frame, in the 3D Viewer tab's content region.
void DrawTab(Context* ctx);
// After ImGui::Render and before drawing ImGui's data: the 3D view into the region DrawTab
// reserved, and any pending GIF export. Does nothing on frames where DrawTab was not called.
void RenderGl(Context* ctx, int framebufferWidth, int framebufferHeight, float framebufferScale, float dt);

// Opens the viewer on one item (e.g. "weapons", "axe"); false with a message if not found.
// category "units": a unit (Monsters) by name, with `skin` (a texture file or name) tried on it when given.
bool OpenItem(Context* ctx, const std::string& category, const std::string& item, std::string& error, const std::string& skin = "", bool naked = false,
              const std::string& clip = "", float frame = -1.0f, float yaw = -1000.0f);

// `um-multitool viewer <args>`: --list, --resolve, --render, --gif (argv[0] is "viewer").
int RunCli(int argc, char** argv);
void PrintCliHelp();

} // namespace viewer
