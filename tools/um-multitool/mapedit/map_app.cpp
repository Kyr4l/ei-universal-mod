// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Map Editor tab: shows an .mpr terrain with the objects of one or more .mob files loaded on top
// of each other (a zone, then a quest or an AddMob map), lists and describes the objects, shows the
// mission scripts, and runs the map checks (checks.hpp) - the ones um.dll logs when the game opens a
// map, and more, so problems are found before the game meets them. Nothing is written back yet.
//
//   mob_file.hpp   .mob reader            mpr_file.hpp  .mpr reader
//   checks.hpp     the map checks         map_scene.hpp the GL view
//
// Like the 3D Viewer, DrawTab lays out a sidebar and a see-through viewport during the ImGui frame,
// and RenderGl draws the 3D view into it after ImGui::Render.

#include "map_internal.hpp"

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Entry points
// ------------------------------------------------------------------------------------------------

Context* Create(Library& lib) {
    Context* ctx = new Context(lib);
    App& app = ctx->app;
    // As last time: the side tab, and the time of day if one was chosen.
    if (lib.mapSideTab >= 0 && lib.mapSideTab < static_cast<int>(SideTab::Count)) app.requestTab = static_cast<SideTab>(lib.mapSideTab);
    if (lib.mapHour >= 0.0f) { app.hour = lib.mapHour; app.hourSet = true; }
    // Walkability and the patrol simulation: the game's tile map, for the shown navmesh layer.
    app.scene.walkBuilder = [&app](MapScene::WalkGrid& w) {
        if (app.terrain.sectorsX <= 0) return;
        std::vector<const mob::File*> all;
        for (auto& m : app.mobs) all.push_back(&m->file);
        navgen::Generator g;
        std::string err;
        if (!navgen::BuildTiles(app.terrain, NavObjects(app.lib.figures, all, nullptr), g, err)) return;
        const int layer = std::clamp(app.scene.options.navLayer, 0, navgen::kLayers - 1);
        w.w = g.tw;
        w.h = g.th;
        w.cell = 0.5f;
        w.value.resize(static_cast<size_t>(g.tw) * g.th);
        for (int y = 0; y < g.th; ++y)
            for (int x = 0; x < g.tw; ++x) w.value[static_cast<size_t>(y) * g.tw + x] = static_cast<uint8_t>(g.Nibble(x, y, layer));
        w.factor.assign(g.factor, g.factor + 16);
        w.factor[0] = 0;
    };
    std::vector<std::string> paths;
    if (!lib.mapTerrain.empty()) paths.push_back(lib.mapTerrain);
    for (const std::string& m : lib.mapMobs) paths.push_back(m);
    OpenPaths(app, paths);
    app.filesMessage.clear();
    if (!lib.mapQuest.empty()) { // reopen the last quest, with its language pack copies
        RescanQuests(app);
        for (const quest::QuestSet& set : app.quests)
            for (const quest::Quest& c : set.copies)
                if (c.path == lib.mapQuest && !app.openQuest) SetOpenQuest(app, set);
        if (!app.openQuest) {
            quest::QuestSet set;
            if (quest::Load(lib.mapQuest, set.shown)) { set.copies.push_back(set.shown); set.labels.push_back(quest::FolderLabel(lib.mapQuest)); SetOpenQuest(app, set); }
        }
    }
    return ctx;
}

void Destroy(Context* ctx) {
    if (!ctx) return;
    ctx->app.scene.DropAll();
    if (ctx->app.idleTex) glDeleteTextures(1, &ctx->app.idleTex);
    delete ctx;
}

void OpenFiles(Context* ctx, const std::vector<std::string>& paths, uint32_t focusId) {
    App& app = ctx->app;
    app.pendingFocusId = focusId;
    CloseQuest(app);
    app.mobs.clear();
    app.loadOrder.clear();
    app.activeMob = 0;
    app.terrainLoaded = false;
    app.terrainPath.clear();
    app.terrainDirty = true;
    OpenPaths(app, paths);
    SyncScene(app);
    SaveSession(app);
}

// The map the game runs (file names as um.dll reports them: "zone3xobr.mpr", "zone3xobr-lmp.mob",
// "z3xq1.mob"): the quest of that name when there is one (found in the quest folders), its own map and
// the base map next to it or in the map folders.
static const quest::QuestSet* GameQuest(App& app, const std::string& quest) {
    if (quest.empty()) return nullptr;
    const std::string stem = Lower(std::filesystem::path(quest).stem().string());
    RescanQuests(app);
    for (const quest::QuestSet& set : app.quests)
        if (Lower(set.shown.name) == stem) return &set;
    return nullptr;
}

// A file the game itself opened (its path as um.dll reports it), when no map folder has that name.
static std::string GamePath(const std::string& name, const std::vector<std::string>& gamePaths) {
    std::error_code ec;
    for (const std::string& p : gamePaths)
        if (Lower(std::filesystem::path(p).filename().string()) == Lower(name) && std::filesystem::is_regular_file(p, ec)) return p;
    return std::string();
}

bool ResolveGameMap(Context* ctx, const std::string& terrain, const std::string& base, const std::string& quest,
                    const std::vector<std::string>& gamePaths, std::string& terrainPath, std::vector<std::string>& mobPaths,
                    std::string& missing) {
    App& app = ctx->app;
    const quest::QuestSet* set = GameQuest(app, quest);
    const std::string hint = set ? set->shown.path : std::string();
    auto resolve = [&](const std::string& name) {
        std::string path = ResolveMapFile(app, name, hint);
        return path.empty() ? GamePath(name, gamePaths) : path;
    };
    mobPaths.clear();
    missing.clear();
    terrainPath = terrain.empty() ? std::string() : resolve(terrain);
    if (!terrain.empty() && terrainPath.empty()) missing += " " + terrain;
    for (const std::string& name : {base, quest}) {
        if (name.empty()) continue;
        const std::string path = resolve(name);
        if (path.empty()) missing += " " + name;
        else mobPaths.push_back(path);
    }
    return missing.empty();
}

std::string OpenGameMap(Context* ctx, const std::string& terrain, const std::string& base, const std::string& quest,
                        const std::vector<std::string>& gamePaths) {
    App& app = ctx->app;
    if (BlockedByUnsaved(app)) return app.filesMessage;
    std::string terrainPath, missing;
    std::vector<std::string> mobs;
    ResolveGameMap(ctx, terrain, base, quest, gamePaths, terrainPath, mobs, missing);
    // A quest found in the quest folders opens as a quest (with its areas), when its files are all there.
    if (const quest::QuestSet* set = GameQuest(app, quest)) {
        if (missing.empty()) {
            OpenQuest(app, *set);
            return app.filesMessage;
        }
    }
    std::vector<std::string> paths = mobs;
    if (!terrainPath.empty()) paths.insert(paths.begin(), terrainPath);
    if (paths.empty()) return "Not found in the map folders:" + missing;
    OpenFiles(ctx, paths);
    app.filesMessage = missing.empty() ? "Opened the game's map" : "Opened the game's map; not found in the map folders:" + missing;
    return app.filesMessage;
}

bool ScriptWindowWanted(Context* ctx) { return ctx->app.scriptWindow; }
bool TakeScriptWindowFocus(Context* ctx) { const bool f = ctx->app.focusScriptWindow; ctx->app.focusScriptWindow = false; return f; }
void CloseScriptWindow(Context* ctx) { ctx->app.scriptWindow = false; }

// The whole script window: the script, and its own Ctrl+S (the main window's keys do not reach it).
void DrawScriptWindow(Context* ctx) {
    App& app = ctx->app;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##scriptwindow", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus);
    ScriptContent(app);
    ImGui::End();
    const config::KeyBind save = app.lib.mapKeys[config::kKeySave];
    if ((save.mods & config::kModCtrl) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        ApplyScriptEdit(app);
        SaveQuestChanges(app);
    }
}

bool TakeNewProblems(Context* ctx, int& errors, int& warnings, int& totalErrors, int& totalWarnings) {
    App& app = ctx->app;
    errors = app.newErrors;
    warnings = app.newWarnings;
    totalErrors = app.summary.errors;
    totalWarnings = app.summary.warnings;
    app.newErrors = app.newWarnings = 0;
    return errors > 0 || warnings > 0;
}

void ShowChecks(Context* ctx) { ctx->app.requestTab = SideTab::Checks; }

void SetMode(Context* ctx, const std::string& mode) {
    App& app = ctx->app;
    if (mode == "script" || mode == "nodes") { // the Script tab, as text or as nodes
        app.requestTab = SideTab::Script;
        app.scriptVisual = mode == "nodes";
        return;
    }
    app.tileBrush = mode == "paint" || mode == "ground" || mode == "water";
    app.heightBrush = mode == "sculpt";
    app.brushWater = mode == "water";
    app.materialBrush = mode == "ground";
    app.paintTabWanted = mode == "ground" ? 0 : mode == "paint" ? 1 : -1;
    if (app.materialBrush && app.brushMaterialId < 0)
        if (tilemat::Terrain* tm = tilemat::Lookup(app.terrain.name))
            for (size_t i = 0; i < tm->materials.size(); ++i) if (!tm->materials[i].baseTiles.empty()) { app.brushMaterialId = static_cast<int>(i); break; }
}

bool Busy(Context* ctx) { return ctx->app.scene.modelsPending > 0 || ctx->app.terrainDirty; }

bool FillQuestMapInput(Context* ctx, questmap::Input& in, std::string& err) {
    const App& app = ctx->app;
    if (!app.terrainLoaded) { err = "open a terrain (.mpr) in the Map Editor first"; return false; }
    in.terrain = &app.terrain;
    in.name = app.terrain.name;
    for (const auto& m : app.mobs) {
        if (!m->visible) continue;
        for (const mob::Object& o : m->file.objects) {
            if (o.kind != mob::Kind::Object) continue;
            questmap::Kind k;
            if (!questmap::Classify(Lower(o.templ), k)) continue;
            const float w = o.rotation[0], x = o.rotation[1], y = o.rotation[2], z = o.rotation[3]; // quaternion, w first
            in.markers.push_back({k, o.position.x, o.position.y, std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))});
        }
    }
    if (app.openQuest) // the exits: where the party leaves (#remove), else where it arrives (#deploy)
        for (const quest::Exit& e : app.openQuest->shown.exits) {
            const quest::Rect& r = e.remove.set ? e.remove : e.deploy;
            if (!r.set) continue;
            in.markers.push_back({questmap::Kind::Exit, (r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f, 0.0f});
        }
    return true;
}

void DrawTab(Context* ctx) {
    App& app = ctx->app;
    PollJob(app);
    if (app.checksDirty || app.checkedVersion != app.lib.version || app.checkedScripts != app.lib.scriptChecks ||
        app.checkedDatabase != app.lib.databaseChecks || app.checkedAtd != app.lib.atdScripts)
        RunChecks(app);
    RefreshScriptModel(app);
    RefreshLighting(app);
    ApplyLighting(app);
    app.scene.logicSelectedOnly = app.lib.logicSelectedOnly;
    app.scene.logicAlways = app.lib.logicAlways;
    StepSimulation(app, ImGui::GetIO().DeltaTime * app.simSpeed);

    // The panel and the view, side by side (the panel on the right when set in Settings).
    ui::SplitLayout split{&app.sidebarWidth, app.lib.mapSidebarRight};
    float panelWidth, viewWidth;
    split.Begin(panelWidth, viewWidth);
    ImVec2 min, size;
    auto view = [&] {
        ImGui::BeginChild("##mapView", ImVec2(viewWidth, split.height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Toolbar(app);
        min = ImGui::GetCursorScreenPos();
        size = ImGui::GetContentRegionAvail();
        size.y = std::max(1.0f, size.y - ImGui::GetFrameHeightWithSpacing()); // the bottom bar's row
        ViewportInput(app, min, size);
        Keys(app);
        Overlays(app, min, size);
        ToolShelf(app, min, size);
        if (app.mobs.empty() && !app.terrainLoaded) {
            ImGui::SetCursorScreenPos(ImVec2(min.x + 20, min.y + 20));
            ImGui::TextUnformatted("Open a terrain (.mpr) and one or more maps (.mob) in the Files tab.\n"
                                   "Figures and textures come from the sources in the Settings tab.");
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + size.y + ImGui::GetStyle().ItemSpacing.y));
        BottomBar(app);
        ImGui::EndChild();
    };
    if (!app.sidebarShown) { viewWidth = 0; view(); } // N: the view alone
    else if (split.panelRight) { view(); split.Bar(); Sidebar(app, panelWidth, split.height); }
    else { Sidebar(app, panelWidth, split.height); split.Bar(); view(); }
    app.viewportMin = min;
    app.viewportMax = ImVec2(min.x + size.x, min.y + size.y);
    app.drawnThisFrame = true;
    MinimapDialog(app);
    MissingWindow(app);
    MergeWindow(app);
    NewTerrainDialog(app);
    CropDialog(app);
    FindWindow(app);
    OffsetWindow(app);
    RandomizeWindow(app);
    SaveAsWindow(app);
    UnloadDialog(app);
    TilePicker(app);
    HistoryWindow(app);
    MobParamsWindow(app);
    SimulationWindow(app);
    NewObjectWindow(app);
    PollExternalScript(app);
}

// What the 3D view is drawn from, as bytes: while resting, the view is drawn again only when this changes
// (or something is still loading / animating / simulating: RenderGl). Input (mouse, keys) ends resting by
// itself, so this only has to catch what changes without input.
static std::vector<unsigned char> SceneKey(const App& app, int vx, int vy, int vw, int vh) {
    std::vector<unsigned char> key;
    auto put = [&](const void* p, size_t n) { const unsigned char* b = static_cast<const unsigned char*>(p); key.insert(key.end(), b, b + n); };
    auto putInt = [&](long long v) { put(&v, sizeof v); };
    put(&app.scene.camera, sizeof app.scene.camera);
    put(&app.scene.options, sizeof app.scene.options);
    put(&app.scene.light, sizeof app.scene.light);
    putInt(vx); putInt(vy); putInt(vw); putInt(vh);
    putInt(app.scene.selectedFile); putInt(app.scene.selectedObject);
    for (int o : app.scene.selection) putInt(o);
    for (const LogicPointRef& r : app.scene.logicPoints) { putInt(r.object); putInt(r.logic); putInt(r.point); putInt(r.look); putInt(r.place); putInt(r.trap); }
    putInt(app.scene.logicMode); putInt(app.scene.logicAlways); putInt(app.scene.logicSelectedOnly);
    putInt(static_cast<long long>(app.scene.guides.size()));
    putInt(static_cast<long long>(app.undoSteps.size())); putInt(static_cast<long long>(app.redoSteps.size()));
    putInt(static_cast<long long>(app.mobs.size())); putInt(app.terrainLoaded);
    for (size_t fi = 0; fi < app.mobs.size(); ++fi) putInt(app.scene.FileVisible(static_cast<int>(fi)));
    putInt(app.lib.version); putInt(app.lib.figuresVersion); putInt(app.lib.texturesVersion);
    put(&app.lib.markerOpacity, sizeof app.lib.markerOpacity);
    putInt(app.lib.mapLowDetail); put(&app.lib.mapDrawDistance, sizeof app.lib.mapDrawDistance);
    putInt(app.dipSelected);
    return key;
}

// Shows the kept view again (idleTex over the viewport).
static void DrawIdleFrame(const App& app, int fbW, int fbH) {
    glViewport(app.idleVx, app.idleVy, app.idleVw, app.idleVh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, app.idleTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    const float u = static_cast<float>(app.idleVw) / app.idleTexW, v = static_cast<float>(app.idleVh) / app.idleTexH;
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(u, 0); glVertex2f(1, 0);
    glTexCoord2f(u, v); glVertex2f(1, 1);
    glTexCoord2f(0, v); glVertex2f(0, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F // OpenGL 1.2; MinGW's gl.h stops at 1.1 (the texture's edge is what the quad shows)
#endif

// Keeps the viewport just drawn in idleTex (a copy from the back buffer: no framebuffer object needed).
static void KeepIdleFrame(App& app, int vx, int vy, int vw, int vh) {
    int w = 1, h = 1;
    while (w < vw) w *= 2;
    while (h < vh) h *= 2;
    if (!app.idleTex) glGenTextures(1, &app.idleTex);
    glBindTexture(GL_TEXTURE_2D, app.idleTex);
    if (w != app.idleTexW || h != app.idleTexH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        app.idleTexW = w; app.idleTexH = h;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vx, vy, vw, vh);
    app.idleValid = glGetError() == GL_NO_ERROR; // a driver refusing the size or the copy: drawn every frame
    app.idleVx = vx; app.idleVy = vy; app.idleVw = vw; app.idleVh = vh;
}

void RenderGl(Context* ctx, int fbW, int fbH, float scale, bool resting) {
    App& app = ctx->app;
    if (!app.drawnThisFrame) return;
    app.drawnThisFrame = false;
    if (scale <= 0) scale = 1.0f;
    // Still loading or moving: the view is drawn whatever the resting state (what follows may set more).
    bool busy = !resting || app.terrainRebuild || app.terrainDirty || app.minimapPending || !app.framed || app.pendingFocusId ||
                (app.newOpen && !app.previewWanted.empty()) || app.idlePendingModels > 0 || (app.simulating && !app.simPaused) ||
                app.seenFigures != app.lib.figuresVersion || app.seenTextures != app.lib.texturesVersion;

    if (app.seenFigures != app.lib.figuresVersion || app.seenTextures != app.lib.texturesVersion) {
        app.scene.DropModels();
        if (app.seenTextures != app.lib.texturesVersion) app.terrainDirty = true;
        app.seenFigures = app.lib.figuresVersion;
        app.seenTextures = app.lib.texturesVersion;
    }
    if (app.terrainRebuild && !app.terrainDirty) { app.scene.RebuildTerrainGeometry(); app.terrainRebuild = false; }
    if (app.terrainDirty) {
        app.terrainRebuild = false;
        app.scene.SetTerrain(app.lib, app.terrainLoaded ? &app.terrain : nullptr);
        app.terrainDirty = false;
        if (app.pendingFocusRect.set) {
            FocusRect(app, app.pendingFocusRect);
            app.scene.camera.pitchDeg = 50.0f;
            app.pendingFocusRect = quest::Rect{};
        }
    }
    RenderPreview(app); // the Add window's 3D preview, in the back buffer before the map
    app.scene.BuildSomeModels(app.lib, 12);
    if (app.scene.AnimateModels(app.lib, ImGui::GetTime())) busy = true; // units play their idle (or walk) animation
    app.idlePendingModels = app.scene.modelsPending;
    if (app.idlePendingModels > 0) busy = true;
    if (app.minimapPending) { // drawn in the back buffer, which this frame then paints over
        app.minimapPending = false;
        ExportMinimap(app, fbW, fbH);
    }
    if (!app.framed && (app.terrainLoaded || !app.mobs.empty())) {
        app.scene.FrameAll();
        app.framed = true;
    }
    if (app.pendingFocusId && app.framed) { // --focus: the object of that ID, close up and selected
        for (size_t fi = 0; fi < app.mobs.size() && app.pendingFocusId; ++fi)
            for (size_t oi = 0; oi < app.mobs[fi]->file.objects.size(); ++oi)
                if (app.mobs[fi]->file.objects[oi].id == app.pendingFocusId) {
                    app.scene.FocusOn(app.mobs[fi]->file.objects[oi]);
                    app.scene.camera.distance = 6.0f;
                    app.scene.camera.pitchDeg = 15.0f; // low: the unit side on
                    app.scene.Select(static_cast<int>(fi), static_cast<int>(oi));
                    app.pendingFocusId = 0;
                    break;
                }
        if (app.pendingFocusId) { app.filesMessage = "--focus: no object " + std::to_string(app.pendingFocusId) + " in the open maps"; app.pendingFocusId = 0; }
    }

    int vx = static_cast<int>(app.viewportMin.x * scale);
    int vw = std::max(1, static_cast<int>((app.viewportMax.x - app.viewportMin.x) * scale));
    int vh = std::max(1, static_cast<int>((app.viewportMax.y - app.viewportMin.y) * scale));
    int vy = std::max(0, fbH - static_cast<int>(app.viewportMax.y * scale));
    std::vector<unsigned char> key = SceneKey(app, vx, vy, vw, vh);
    if (!busy && app.idleValid && key == app.idleKey) { // resting and nothing changed: the kept frame
        DrawIdleFrame(app, fbW, fbH);
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);
    app.scene.Draw(app.lib, vx, vy, vw, vh);
    glDisable(GL_SCISSOR_TEST);
    if (resting) { KeepIdleFrame(app, vx, vy, vw, vh); app.idleKey = std::move(key); }
    else app.idleValid = false; // active: kept again at the first resting frame
    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// ------------------------------------------------------------------------------------------------
// Command line
// ------------------------------------------------------------------------------------------------

// The maps' objects as the navmesh generator sees them (navmesh_gen.hpp): every object with a figure but
// units, each with the boxes of its parts (OBJ_BODYPARTS, or all of them). `missing`: objects whose figure
// was not found (left out).
std::vector<navgen::Object> NavObjects(const LayeredAssetSource& figures, const std::vector<const mob::File*>& maps, int* missing) {
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::map<std::string, std::unique_ptr<LoadedModel>> models;
    std::vector<navgen::Object> out;
    if (missing) *missing = 0;
    for (const mob::File* f : maps)
        for (const mob::Object& o : f->objects) {
            if (!mob::HasModel(o.kind) || o.kind == mob::Kind::Unit) continue;
            std::unique_ptr<LoadedModel>& m = models[lower(o.templ)];
            if (!m) {
                m = std::make_unique<LoadedModel>();
                if (!LoadNamedModel(figures, o.templ, *m)) m->ok = false;
                else m->ok = true;
            }
            if (!m->ok) {
                if (missing) ++*missing;
                if (std::getenv("NAVGEN_OBJ")) std::printf("no figure: %s at %.1f %.1f\n", o.templ.c_str(), o.position.x, o.position.y);
                continue;
            }
            std::vector<std::string> shown;
            for (const std::string& p : o.bodyParts) shown.push_back(lower(p));
            const fig::Vec3 k{o.complection.x, o.complection.y, o.complection.z}; // not clamped: the game extrapolates
            navgen::Object n;
            n.position = {o.position.x, o.position.y, o.position.z};
            n.rotation = fig::QuatNormalize(fig::Quat{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]});
            for (const fig::ModelPart& part : m->model.parts) {
                if (!shown.empty() && std::find(shown.begin(), shown.end(), lower(part.name)) == shown.end()) continue;
                if (part.mesh.morphMin.size() < 8 || part.mesh.morphMax.size() < 8) continue;
                bool skip = false;
                navgen::PartBox b;
                b.kind = navgen::PartKind(part.name, skip);
                if (skip) continue;
                // The game places the box at the part's offset plus the figure's centre (0x5B6A80).
                const fig::Vec3 off = fig::BlendComplection(part.accumulatedOffset, k) + fig::BlendComplection(part.mesh.morphCenter.data(), k);
                const fig::Vec3 lo = fig::BlendComplection(part.mesh.morphMin.data(), k) + off, hi = fig::BlendComplection(part.mesh.morphMax.data(), k) + off;
                b.min = {std::min(lo.x, hi.x), std::min(lo.y, hi.y), std::min(lo.z, hi.z)};
                b.max = {std::max(lo.x, hi.x), std::max(lo.y, hi.y), std::max(lo.z, hi.z)};
                n.parts.push_back(b);
            }
            if (const char* dbg = std::getenv("NAVGEN_OBJ")) { // "x0,y0,x1,y1": the objects there (development)
                float a = 0, b = 0, c = 0, d = 0;
                if (std::sscanf(dbg, "%f,%f,%f,%f", &a, &b, &c, &d) == 4 && o.position.x >= a && o.position.y >= b && o.position.x <= c && o.position.y <= d) {
                    std::printf("%s at %.3f %.3f %.3f rot %.3f %.3f %.3f %.3f compl %.2f %.2f %.2f\n", o.templ.c_str(), o.position.x, o.position.y, o.position.z,
                                o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3], o.complection.x, o.complection.y, o.complection.z);
                    for (const navgen::PartBox& b2 : n.parts)
                        std::printf("   part %d  %.3f %.3f %.3f .. %.3f %.3f %.3f\n", static_cast<int>(b2.kind), b2.min.x, b2.min.y, b2.min.z, b2.max.x, b2.max.y, b2.max.z);
                }
            }
            if (!n.parts.empty()) out.push_back(std::move(n));
        }
    return out;
}

// Compares two AI_GRAPH payloads (the game's and ours), per layer: representative tiles, costs, walkable
// nodes, components.
void CompareNavmesh(const std::vector<uint8_t>& game, const std::vector<uint8_t>& ours) {
    auto u32 = [](const std::vector<uint8_t>& b, size_t at) { return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24); };
    if (game.size() < 8 || ours.size() < 8) { std::printf("nothing to compare\n"); return; }
    const uint32_t w = u32(game, 0), h = u32(game, 4);
    if (w != u32(ours, 0) || h != u32(ours, 4) || game.size() != ours.size()) {
        std::printf("different grids: game %ux%u (%zu bytes), ours %ux%u (%zu bytes)\n", w, h, game.size(), u32(ours, 0), u32(ours, 4), ours.size());
        return;
    }
    const size_t row = static_cast<size_t>(w) * 19;
    for (int layer = 0; layer < 8; ++layer) {
        size_t sameB = 0, sameA = 0, within = 0, walkG = 0, walkO = 0, walkBoth = 0, edges = 0;
        uint32_t compG = 0, compO = 0;
        for (uint32_t y = 0; y < h; ++y) {
            const size_t base = 8 + (static_cast<size_t>(layer) * h + y) * row;
            for (uint32_t x = 0; x < w; ++x) {
                sameB += game[base + w * 16 + x] == ours[base + w * 16 + x];
                compG = std::max<uint32_t>(compG, game[base + w * 17 + x * 2] | (game[base + w * 17 + x * 2 + 1] << 8));
                compO = std::max<uint32_t>(compO, ours[base + w * 17 + x * 2] | (ours[base + w * 17 + x * 2 + 1] << 8));
                bool wg = false, wo = false;
                for (int k = 0; k < 8; ++k) {
                    const size_t at = base + x * 16 + k * 2;
                    const int a = game[at] | (game[at + 1] << 8), b = ours[at] | (ours[at + 1] << 8);
                    ++edges;
                    sameA += a == b;
                    within += a == b || (a != 0xFFFF && b != 0xFFFF && std::abs(a - b) <= std::max(2, a / 20));
                    wg |= a != 0xFFFF;
                    wo |= b != 0xFFFF;
                }
                walkG += wg; walkO += wo; walkBoth += wg && wo;
            }
        }
        const double n = static_cast<double>(w) * h;
        std::printf("layer %d: rep tiles %5.1f%%  costs exact %5.1f%% / within 5%% %5.1f%%  walkable nodes game %zu ours %zu both %zu  components game %u ours %u\n",
                    layer, 100.0 * sameB / n, 100.0 * sameA / edges, 100.0 * within / edges, walkG, walkO, walkBoth, compG, compO);
    }
}

void PrintCliHelp() {
    std::printf(
        "um-multitool map - the Map Editor's command-line mode\n"
        "\n"
        "Usage:\n"
        "  um-multitool map --check <map.mob> [more.mob ...] [--mpr <terrain.mpr>] [--config <file>]\n"
        "        Check maps like the Map Editor's Checks tab: the checks um.dll runs when the game opens a\n"
        "        map, and more. Several maps are checked together in the given order, each on top of the\n"
        "        ones before it (a zone, then its quest). Figures, textures and the items database are the\n"
        "        sources of the GUI's Settings tab (um-multitool.cfg). Exit code 1 when errors are found.\n"
        "  um-multitool map --navmesh <map.mob> [more.mob ...] --mpr <terrain.mpr> [--write <out.mob> [--force]] [--config <file>]\n"
        "        Build the navmesh (AI_GRAPH) the way the game does, from the terrain and the maps' objects (their\n"
        "        figures from the Settings tab's sources), compare it with the first map's own, and with --write\n"
        "        save the first map with it (refused when it has no navmesh, e.g. a quest map, unless --force).\n"
        "  um-multitool map --navmesh-all <folder> [--write-all] [--terrains <folder>]... [--config <file>]\n"
        "        Every map of the folder that has a navmesh, with the terrain of its name (zone3xobr-lmp.mob ->\n"
        "        zone3xobr.mpr, beside it or in the Settings' map folders): says which are out of date; --write-all rebuilds those. Quest maps are skipped (their\n"
        "        objects are not counted: rebuild a zone with its quests by --navmesh when they add obstacles).\n"
        "  um-multitool gui --map <file.mpr|file.mob> [...] [--focus <object id>]\n"
        "        Open the GUI's Map Editor on these files.\n");
}

int RunCli(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") { PrintCliHelp(); return args.empty() ? 1 : 0; }
    Library lib;
    std::string terrainPath;
    std::vector<std::string> mobs;
    bool check = false, navmesh = false, force = false, writeAll = false;
    std::string navFolder;
    std::vector<std::string> terrainDirs;
    std::string writePath;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--check") check = true;
        else if (args[i] == "--navmesh") navmesh = true;
        else if (args[i] == "--force") force = true;
        else if (args[i] == "--navmesh-all" && i + 1 < args.size()) navFolder = args[++i];
        else if (args[i] == "--write-all") writeAll = true;
        else if (args[i] == "--terrains" && i + 1 < args.size()) terrainDirs.push_back(args[++i]);
        else if (args[i] == "--write" && i + 1 < args.size()) writePath = args[++i];
        else if (args[i] == "--mpr" && i + 1 < args.size()) terrainPath = args[++i];
        else if (args[i] == "--config" && i + 1 < args.size()) lib.configPath = args[++i];
        else if (EndsWith(args[i], ".mpr")) terrainPath = args[i];
        else mobs.push_back(args[i]);
    }
    if (!navFolder.empty()) { // every map of a folder that has a navmesh, with the terrain of the same name
        lib.LoadConfig();
        int done = 0, failed = 0;
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(navFolder, ec))
            if (e.is_regular_file() && Lower(e.path().extension().string()) == ".mob") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        // 1. One by one (the figure sources are not thread-safe): each map, its terrain and its objects.
        struct Job {
            std::filesystem::path path;
            std::unique_ptr<mob::File> map;
            mpr::Map terrain;
            std::vector<navgen::Object> objects;
            int missing = 0;
            std::string note;                // a problem found before generating
            std::vector<uint8_t> payload;
            std::string err;
            bool ok = false;
        };
        std::vector<Job> jobs;
        for (const auto& path : files) {
            auto f = std::make_unique<mob::File>();
            if (!mob::Load(path.string(), *f) || !f->aiGraphBytes) continue; // quest maps have no navmesh
            std::string base = path.stem().string();
            const size_t dash = base.find('-');
            if (dash != std::string::npos) base = base.substr(0, dash); // zone3xobr-lmp -> zone3xobr
            std::filesystem::path terrainPath = path.parent_path() / (base + ".mpr");
            if (!std::filesystem::exists(terrainPath, ec)) // else the Settings' map folders (the base game's ...)
                for (const Library::MapFile& mf : lib.ListMapFiles())
                    if (Lower(mf.name) == Lower(base + ".mpr")) { terrainPath = mf.path; break; }
            for (const std::string& dir : terrainDirs) { // and the folders given by --terrains
                if (std::filesystem::exists(terrainPath, ec)) break;
                const std::string found = checks::FindInDirectory(dir, base + ".mpr");
                if (!found.empty()) terrainPath = found;
            }
            Job j;
            j.path = path;
            std::string err;
            if (!mpr::Load(terrainPath.string(), j.terrain, err)) j.note = "no terrain (" + terrainPath.filename().string() + ")";
            else j.objects = NavObjects(lib.figures, {f.get()}, &j.missing);
            j.map = std::move(f);
            jobs.push_back(std::move(j));
        }
        // 2. The navmeshes on every core (navgen::Generate only reads its inputs).
        std::atomic<size_t> next{0};
        auto work = [&]() {
            for (size_t i; (i = next++) < jobs.size();) {
                Job& j = jobs[i];
                if (j.note.empty()) j.ok = navgen::Generate(j.terrain, j.objects, j.payload, j.err);
            }
        };
        const unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(jobs.size())));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
        // 3. In order: what each map needs, and the writes.
        for (Job& j : jobs) {
            const std::string name = j.path.filename().string();
            if (!j.note.empty()) { std::printf("%-28s %s\n", name.c_str(), j.note.c_str()); ++failed; continue; }
            if (!j.ok) { std::printf("%-28s %s\n", name.c_str(), j.err.c_str()); ++failed; continue; }
            mob::File& f = *j.map;
            const std::vector<uint8_t> old(f.bytes.begin() + f.aiGraphAt, f.bytes.begin() + f.aiGraphAt + f.aiGraphBytes - 8);
            const bool same = old == j.payload;
            std::printf("%-28s %s%s\n", name.c_str(), same ? "up to date" : "out of date",
                        j.missing ? (" (" + std::to_string(j.missing) + " objects without a figure)").c_str() : "");
            if (!same && writeAll) {
                std::string err;
                mob::SetAiGraph(f, j.payload);
                if (mob::Save(f, err)) std::printf("%-28s written\n", "");
                else { std::printf("%-28s not written: %s\n", "", err.c_str()); ++failed; }
            }
            ++done;
        }
        std::printf("%d map(s) with a navmesh, %d problem(s)%s\n", done, failed, writeAll ? "" : " (--write-all to rebuild the out-of-date ones)");
        return failed ? 1 : 0;
    }
    if ((!check && !navmesh) || mobs.empty()) { PrintCliHelp(); return 1; }
    lib.LoadConfig();
    if (navmesh) {
        std::vector<std::unique_ptr<mob::File>> maps;
        std::vector<const mob::File*> list;
        for (const std::string& p : mobs) {
            maps.push_back(std::make_unique<mob::File>());
            if (!mob::Load(p, *maps.back())) { std::fprintf(stderr, "cannot read %s\n", p.c_str()); return 1; }
            list.push_back(maps.back().get());
        }
        mpr::Map terrain;
        std::string err;
        if (terrainPath.empty() || !mpr::Load(terrainPath, terrain, err)) { std::fprintf(stderr, "terrain needed (--mpr): %s\n", err.c_str()); return 1; }
        if (!lib.figures.AnyLoaded()) std::printf("note: no figure sources set: objects are left out\n");
        int missing = 0;
        const std::vector<navgen::Object> objects = NavObjects(lib.figures, list, &missing);
        std::printf("%zu objects (%d without a figure)\n", objects.size(), missing);
        std::vector<uint8_t> payload;
        if (!navgen::Generate(terrain, objects, payload, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        const mob::File& first = *maps[0];
        if (first.aiGraphAt) {
            const size_t len = static_cast<size_t>(first.aiGraphBytes) - 8;
            CompareNavmesh(std::vector<uint8_t>(first.bytes.begin() + first.aiGraphAt, first.bytes.begin() + first.aiGraphAt + len), payload);
        } else {
            std::printf("%s has no navmesh to compare with\n", first.fileName.c_str());
        }
        if (!writePath.empty()) {
            if (!first.aiGraphBytes && !force) {
                std::fprintf(stderr, "not written: %s has no navmesh (a quest map?). The zone's main map comes first; --force writes it anyway.\n",
                             first.fileName.c_str());
                return 1;
            }
            mob::File out = first;
            mob::SetAiGraph(out, payload);
            out.path = writePath;
            if (!mob::Save(out, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
            std::printf("written: %s\n", writePath.c_str());
        }
        return 0;
    }

    std::vector<std::unique_ptr<mob::File>> files;
    checks::Inputs in;
    for (const std::string& p : mobs) {
        files.push_back(std::make_unique<mob::File>());
        mob::Load(p, *files.back());
        in.maps.push_back(files.back().get());
    }
    mpr::Map terrain;
    if (!terrainPath.empty()) {
        std::string err;
        if (mpr::Load(terrainPath, terrain, err)) in.terrain = &terrain;
        else std::fprintf(stderr, "terrain not loaded: %s\n", err.c_str());
    }
    checks::DatabaseNames db;
    db.Load(lib.dbPath);
    in.figures = &lib.figures;
    in.textures = &lib.textures;
    in.database = &db;
    in.scriptChecks = lib.scriptChecks;
    in.databaseChecks = lib.databaseChecks;
    if (!lib.scriptChecks) std::printf("note: script checks are off (Settings > Checks)\n");
    if (!lib.databaseChecks) std::printf("note: database checks are off (Settings > Checks)\n");
    else if (db.Empty()) std::printf("note: no items database set in the GUI's Settings tab: item, spell and prototype names are not checked\n");
    if (!lib.figures.AnyLoaded()) std::printf("note: no figure sources set: figures are not checked\n");
    if (!lib.textures.AnyLoaded()) std::printf("note: no texture sources set: textures are not checked\n");

    checks::Summary summary;
    std::vector<checks::Finding> findings = checks::Run(in, &summary);
    for (const checks::Finding& f : findings) {
        const mob::File& file = *in.maps[f.file];
        std::string where = file.fileName;
        if (f.line > 0) where += ":" + std::to_string(f.line);
        std::printf("%s %s [%s] %s\n", f.severity == 'E' ? "ERROR" : f.severity == 'W' ? "WARN " : "INFO ", where.c_str(), f.category.c_str(),
                    f.message.c_str());
    }
    std::printf("%d error(s), %d warning(s), %d note(s) in %zu map(s)\n", summary.errors, summary.warnings, summary.infos, mobs.size());
    return summary.errors > 0 ? 1 : 0;
}



} // namespace mapedit
