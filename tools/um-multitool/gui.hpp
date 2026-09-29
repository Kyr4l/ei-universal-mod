// The GUI entry point (gui_main.cpp), called by main.cpp.
#pragma once

#include <string>

struct GuiOptions {
    bool openViewer = false;       // start on the 3D Viewer tab
    std::string viewerCategory;    // with viewerItem: open the viewer on that item
    std::string viewerItem;
    std::string screenshotPath;    // save the window after a few frames, then quit
};

int RunGui(const GuiOptions& options);
