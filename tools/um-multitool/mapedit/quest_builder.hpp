// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Quest Builder tab's state (quest_builder.cpp draws it): the draft being built, the map shown, the placing tool.
#pragma once

#include <GLFW/glfw3.h>

#include <string>
#include <vector>

#include "mob_file.hpp"
#include "mpr_file.hpp"
#include "quest_draft.hpp"
#include "rating.hpp"

namespace mapedit {

struct BuilderUi {
    ~BuilderUi() { if (tex) glDeleteTextures(1, &tex); }
    qdraft::Draft d;
    int step = 0;
    int goStep = -1;                // a step asked for by a button: the tab bar selects it next frame
    bool started = false;           // the draft's defaults were filled in
    // the map shown
    std::string zone;               // the terrain's name ("zone26") the picture and the base map belong to
    mpr::Map terrain;
    bool terrainOk = false;
    GLuint tex = 0;
    float mapW = 0, mapH = 0;       // world size
    mob::File base;                 // the zone's base map (units are rated from it)
    bool baseOk = false;
    std::string baseError, templateName;
    // the placing tool: a click on the picture sets ...
    enum Tool { None, Deploy, Exit, GroupAt, AreaAt };
    Tool tool = None;
    int selGroup = -1, selObj = -1;
    char monsterFilter[64] = "";
    char outFolder[512] = "";
    std::string message;
    bool idsReady = false;
    std::vector<std::string> problems;
    // the draft as files, and its rating (made again when the draft changes)
    std::string builtKey, builtError;
    mob::File built;
    bool builtOk = false;
    rating::Result result;
    bool resultOk = false;
    std::string resultWait;
    std::string savedQuest, savedFolder; // the quest just written, and where
    bool overwrite = false;
    int tagMode = 0;                // 0 from the rating, 1 Easy, 2 Medium, 3 Hard
    bool wantMapEditor = false;     // asked: show the Map Editor tab
    int allodIndex = -1;            // -1 all zones
};

} // namespace mapedit
