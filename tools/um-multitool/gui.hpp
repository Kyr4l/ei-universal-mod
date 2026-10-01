// The GUI entry point (gui_main.cpp), called by main.cpp.
#pragma once

#include <string>
#include <vector>

struct GuiOptions {
    bool openViewer = false;       // start on the 3D Viewer tab
    std::string viewerCategory;    // with viewerItem: open the viewer on that item
    std::string viewerItem;
    bool openMap = false;          // start on the Map Editor tab
    std::vector<std::string> mapFiles; // with openMap: open these (.mpr, .mob) instead of the last session's
    bool openSettings = false;     // start on the Settings tab
    std::string dbFile;            // start on File Processing > DB with this database (.res, .xlsx, .ods)
    std::string screenshotPath;    // save the window after a few frames, then quit
};

int RunGui(const GuiOptions& options);
