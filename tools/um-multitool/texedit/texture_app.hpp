// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Texture Editor: um-multitool's main tab for the game's textures (see _cpr/PENDING.md #73 / #93).
// For now only the tab itself; the tools come later.
#pragma once

#include <functional>
#include <string>

struct Library;

namespace questmap { struct Input; }

namespace texedit {

// The Map Editor's open map for the quest map generator (terrain pointer valid during the call; false and `err`
// when there is none).
void SetMapSource(std::function<bool(questmap::Input&, std::string& err)> source);

// Opens a picture (.mmp, .dds, .png, .jpg, .bmp, .tga) in the editor (the "--texture <file>" option).
void OpenPath(const std::string& path);

// Inside the ImGui frame, in the tab's content region.
void DrawTab(Library& lib);

} // namespace texedit
