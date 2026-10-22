// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The GUI entry point (gui_main.cpp), called by main.cpp.
#pragma once

#include <string>
#include <vector>

struct GuiOptions {
    bool openViewer = false;       // start on the 3D Viewer tab
    std::string viewerCategory;    // with viewerItem: open the viewer on that item
    std::string viewerItem;
    bool viewerNaked = false;      // with "--viewer units <name>": without its equipment (--naked)
    std::string viewerSkin;        // with "--viewer units <name>": a skin texture file to try on it (--skin)
    uint32_t mapFocus = 0;         // with "--map": an object ID to select and look at (--focus)
    std::string mapMode;           // with "--map": the editing mode to start in: object, paint, ground (by material), sculpt, water; or the Script tab as script (text) / nodes (--mode)
    std::string viewerClip;        // with "--viewer units <name>": an animation clip to show (--clip)
    float viewerFrame = -1.0f;     // ... held at that frame (--frame); -1: playing
    std::vector<std::string> viewerWeapons; // with "--viewer units": the weapons to hold instead of its own, "item.material" (--weapon, twice at most)
    float viewerComplection[3] = {0.5f, 0.5f, 0.5f}; // ... and its build (--complection a,b,c)
    bool viewerHasComplection = false;
    float viewerZoom = -1.0f;      // ... the camera distance x this (--zoom) and its target moved by --look dx,dy,dz
    float viewerLook[3] = {0, 0, 0};
    std::string configFile;        // another settings file than the one beside the program (--config)
    float viewerYaw = -1000.0f;    // with "--viewer units": the camera's yaw in degrees (--yaw; 225 = from the front, 45 = the back)
    bool openMap = false;          // start on the Map Editor tab
    std::vector<std::string> mapFiles; // with openMap: open these (.mpr, .mob) instead of the last session's
    bool openSettings = false;     // start on the Settings tab
    std::string dbFile;            // start on File Processing > DB with this database (.res, .xlsx, .ods)
    std::string mpFolder;          // start on File Processing > MP with this characters folder
    std::string iniFile;           // start on File Processing > INI <-> REG with this file (.ini or .reg)
    bool openTexture = false;      // start on the Texture Editor tab
    std::string textureFile;       // with openTexture: open this picture (.mmp, .dds, .png...)
    std::string screenshotPath;    // save the window after a few frames, then quit
};

int RunGui(const GuiOptions& options);
