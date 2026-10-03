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
    std::string viewerClip;        // with "--viewer units <name>": an animation clip to show (--clip)
    float viewerFrame = -1.0f;     // ... held at that frame (--frame); -1: playing
    bool openMap = false;          // start on the Map Editor tab
    std::vector<std::string> mapFiles; // with openMap: open these (.mpr, .mob) instead of the last session's
    bool openSettings = false;     // start on the Settings tab
    std::string dbFile;            // start on File Processing > DB with this database (.res, .xlsx, .ods)
    std::string mpFolder;          // start on File Processing > MP with this characters folder
    std::string screenshotPath;    // save the window after a few frames, then quit
};

int RunGui(const GuiOptions& options);
