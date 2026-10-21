// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Files tab: terrains, maps and quests opened, saved and unloaded; the undo history; the map checks.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------------

std::string Lower(std::string s) { return checks::Lower(std::move(s)); }

bool EndsWith(const std::string& s, const char* ext) {
    std::string l = Lower(s), e = ext;
    return l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0;
}

void SaveSession(App& app) {
    app.lib.mapTerrain = app.terrainLoaded ? app.terrainPath : std::string();
    app.lib.mapMobs.clear();
    for (auto& m : app.mobs) app.lib.mapMobs.push_back(m->file.path);
    app.lib.SaveConfig();
}

void SyncScene(App& app) {
    std::vector<const mob::File*> files;
    std::vector<bool> visible;
    for (auto& m : app.mobs) { files.push_back(&m->file); visible.push_back(m->visible); }
    app.scene.SetMaps(files, visible);
    app.activeMob = app.mobs.empty() ? 0 : std::min(std::max(app.activeMob, 0), static_cast<int>(app.mobs.size()) - 1);
    app.scene.activeFile = app.activeMob;
    if (app.scene.selectedFile != app.activeMob) app.scene.ClearSelection();
    app.checksDirty = true;
    app.listedKey.clear();
}

bool TerrainUnsaved(const App& app);
static void ForgetTerrainEdits(App& app);
bool LoadTerrain(App& app, const std::string& path) {
    if (TerrainUnsaved(app)) {
        app.filesMessage = "The terrain has unsaved changes: save them first (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ") or undo them";
        return false;
    }
    std::string err;
    mpr::Map map;
    if (!mpr::Load(path, map, err)) {
        app.terrainError = err;
        app.filesMessage = "Terrain not loaded: " + err;
        umlog::Write(umlog::Level::Error, "Map Editor: terrain not loaded: " + path + ": " + err);
        return false;
    }
    umlog::Write(umlog::Level::Info, "Map Editor: terrain " + path + ": " + map.name + ", " + std::to_string(map.sectorsX) + " x " + std::to_string(map.sectorsY) + " sectors");
    ForgetTerrainEdits(app);
    app.terrain = std::move(map);
    if (app.terrainLoaded) app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), app.terrainPath), app.loadOrder.end());
    app.loadOrder.push_back(path);
    if (app.terrainPath != path) app.minimapPath[0] = '\0'; // the minimap's default name follows the terrain
    app.terrainLoaded = true;
    app.terrainPath = path;
    app.terrainError.clear();
    app.terrainDirty = true;
    app.checksDirty = true;
    app.framed = false;
    return true;
}

bool AddMob(App& app, const std::string& path) {
    for (auto& m : app.mobs) {
        if (m->file.path == path) { app.filesMessage = path + " is already loaded"; return false; }
    }
    auto entry = std::make_unique<MobEntry>();
    mob::Load(path, entry->file);
    entry->savedBytes = entry->file.bytes;
    if (!entry->file.loaded) app.filesMessage = entry->file.fileName + ": " + entry->file.error;
    if (!entry->file.loaded) umlog::Write(umlog::Level::Error, "Map Editor: map not loaded: " + path + ": " + entry->file.error);
    else umlog::Write(umlog::Level::Info, "Map Editor: map " + path + ": " + std::to_string(entry->file.objects.size()) + " objects" + (entry->file.hasScript ? ", a script" : "") + (entry->file.structure.empty() ? "" : ", " + std::to_string(entry->file.structure.size()) + " structure issue(s)"));
    app.mobs.push_back(std::move(entry));
    app.loadOrder.push_back(path);
    SyncScene(app);
    if (!app.terrainLoaded) app.framed = false;
    return true;
}


QuestSuggestion SuggestForQuest(const App& app) {
    QuestSuggestion s;
    for (auto& m : app.mobs) {
        checks::QuestInfo q = checks::ReadQuestArchive(m->file.path);
        if (!q.found) continue;
        std::string dir = checks::DirectoryOf(m->file.path);
        std::string base = checks::FindInDirectory(dir, checks::WithMobExtension(q.baseMap));
        bool baseLoaded = false;
        for (auto& other : app.mobs) if (Lower(other->file.fileName) == Lower(checks::WithMobExtension(q.baseMap))) baseLoaded = true;
        std::string mprName = q.terrain;
        if (!EndsWith(mprName, ".mpr")) mprName += ".mpr";
        std::string terrain = app.terrainLoaded ? std::string() : checks::FindInDirectory(dir, mprName);
        if ((!baseLoaded && !base.empty()) || !terrain.empty()) {
            s.any = true;
            s.baseMob = baseLoaded ? std::string() : base;
            s.terrain = terrain;
            s.label = m->file.fileName + " is a quest for " + q.baseMap + (q.terrain.empty() ? "" : " on " + q.terrain);
            return s;
        }
    }
    return s;
}

void SetActiveMob(App& app, int index) {
    if (app.mobs.empty()) return;
    app.activeMob = ((index % static_cast<int>(app.mobs.size())) + static_cast<int>(app.mobs.size())) % static_cast<int>(app.mobs.size());
    app.scene.ClearSelection();
    app.listedKey.clear();
    SyncScene(app);
}

// Unloading or replacing a map with unsaved edits would lose them: refuse, and say how to save.
bool BlockedByUnsaved(App& app, const MobEntry* only) {
    for (auto& m : app.mobs) {
        if (only && m.get() != only) continue;
        if (!m->Dirty()) continue;
        app.filesMessage = m->file.fileName + " has unsaved changes: save them first (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) +
                           ") or undo them (" + ui::BindName(app.lib.mapKeys[config::kKeyUndo]) + ")";
        app.questMessage = app.filesMessage;
        return true;
    }
    return false;
}

bool TerrainUnsaved(const App& app) { return app.terrainLoaded && (!app.terrainEditedSectors.empty() || app.terrainHeaderEdited); }

static void ForgetTerrainEdits(App& app) {
    app.terrainEditedSectors.clear();
    app.terrainHeaderEdited = false;
    app.painting = false;
    for (auto* v : {&app.undoSteps, &app.redoSteps})
        v->erase(std::remove_if(v->begin(), v->end(), [](const EditStep& e) { return e.kind == EditStep::Terrain; }), v->end());
}

// Unloads the terrain, its unsaved edits dropped (RequestUnload asks first).
static void UnloadTerrain(App& app) {
    ForgetTerrainEdits(app);
    app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), app.terrainPath), app.loadOrder.end());
    app.terrainLoaded = false;
    app.terrainPath.clear();
    app.terrainDirty = true;
    app.checksDirty = true;
}

// Unloads a map, its unsaved edits and their undo steps dropped (RequestUnload asks first).
static void RemoveMob(App& app, int index) {
    if (index < 0 || index >= static_cast<int>(app.mobs.size())) return;
    const std::string path = app.mobs[index]->file.path;
    for (auto* v : {&app.undoSteps, &app.redoSteps})
        v->erase(std::remove_if(v->begin(), v->end(), [&](const EditStep& e) { return e.kind != EditStep::Terrain && e.kind != EditStep::Areas && e.file == path; }), v->end());
    if (app.scriptEditFor == path) app.scriptEditing = false;
    if (app.scriptExtMob == path) app.scriptExtPath.clear();
    app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), path), app.loadOrder.end());
    app.mobs.erase(app.mobs.begin() + index);
    if (app.activeMob > index || app.activeMob >= static_cast<int>(app.mobs.size())) app.activeMob = std::max(0, app.activeMob - 1);
    app.scene.ClearSelection();
    SyncScene(app);
}

// Unloads a file, the terrain or a map (its path): at once when it has no unsaved changes, else after
// UnloadDialog asks whether to save them, drop them, or keep the file.
// The path is taken BY VALUE: callers pass the map's own path string (or the terrain path, or a load-order entry), which this very function destroys; using the
// reference afterwards (the message, the load order, the session) was a use-after-free that crashed the editor when a map was unloaded.
void UnloadFile(App& app, std::string path) {
    if (app.terrainLoaded && path == app.terrainPath) {
        UnloadTerrain(app);
        app.filesMessage = "Unloaded the terrain " + path;
    } else {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (app.mobs[i]->file.path == path) { RemoveMob(app, static_cast<int>(i)); break; }
        app.filesMessage = "Unloaded " + path;
    }
    app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), path), app.loadOrder.end());
    SaveSession(app);
}

bool FileUnsaved(const App& app, const std::string& path) {
    if (app.terrainLoaded && path == app.terrainPath) return TerrainUnsaved(app);
    for (const auto& m : app.mobs) if (m->file.path == path) return m->Dirty();
    return false;
}

void RequestUnload(App& app, std::string path) {
    if (FileUnsaved(app, path)) app.unloadAsk = path;
    else UnloadFile(app, path);
}

// The U key: unloads whichever file was loaded last, terrain or map.
void UnloadLast(App& app) {
    if (!app.loadOrder.empty()) RequestUnload(app, app.loadOrder.back());
}

// A map file named in a quest: from the map folders of Settings, else next to the quest.
std::string ResolveMapFile(const App& app, const std::string& fileName, const std::string& questPath) {
    const std::string lower = Lower(fileName);
    for (const Library::MapFile& f : app.lib.ListMapFiles()) if (Lower(f.name) == lower) return f.path;
    std::error_code ec;
    std::filesystem::path p(questPath);
    std::string dir = std::filesystem::is_directory(p, ec) ? p.parent_path().string() : checks::DirectoryOf(questPath);
    return checks::FindInDirectory(dir, fileName);
}

void FocusRect(App& app, const quest::Rect& r) {
    if (!r.set) return;
    OrbitCamera& cam = app.scene.camera;
    cam.targetX = (r.x1 + r.x2) * 0.5f;
    cam.targetY = (r.y1 + r.y2) * 0.5f;
    cam.targetZ = app.scene.Ground(cam.targetX, cam.targetY);
    cam.distance = std::max(25.0f, std::max(std::fabs(r.x2 - r.x1), std::fabs(r.y2 - r.y1)) * 2.5f);
}

bool SameAreas(const std::vector<quest::Exit>& a, const std::vector<quest::Exit>& b) {
    if (a.size() != b.size()) return false;
    auto same = [](const quest::Rect& x, const quest::Rect& y) {
        return x.set == y.set && x.x1 == y.x1 && x.y1 == y.y1 && x.x2 == y.x2 && x.y2 == y.y2;
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (!same(a[i].deploy, b[i].deploy) || !same(a[i].remove, b[i].remove)) return false;
    return true;
}

void PushUndo(App& app, EditStep step, std::string label) {
    if (label.empty()) {
        static const char* const kinds[] = {"Quest areas", "Objects", "Diplomacy", "Edit", "Terrain"};
        label = kinds[static_cast<int>(step.kind)];
    }
    step.label = std::move(label);
    app.undoSteps.push_back(std::move(step));
    if (app.undoSteps.size() > 300) app.undoSteps.erase(app.undoSteps.begin());
    app.redoSteps.clear();
}

// Before a change of the areas: remembers them for Ctrl+Z (a new change drops what Ctrl+Y could redo).
void PushAreaUndo(App& app, const std::vector<quest::Exit>& before) {
    EditStep step;
    step.kind = EditStep::Areas;
    step.areas = before;
    PushUndo(app, std::move(step), "Resize a quest area");
}

// Area steps belong to the quest that was open: they go when another opens or it is reverted.
static void DropAreaSteps(App& app) {
    for (auto* v : {&app.undoSteps, &app.redoSteps})
        v->erase(std::remove_if(v->begin(), v->end(), [](const EditStep& e) { return e.kind == EditStep::Areas; }), v->end());
}

MobEntry* FindMob(App& app, const std::string& path) {
    for (auto& m : app.mobs) if (m->file.path == path) return m.get();
    return nullptr;
}

std::vector<ObjectState> CaptureObjects(const mob::File& f, const std::vector<int>& indices) {
    std::vector<ObjectState> out;
    for (int i : indices)
        if (i >= 0 && i < static_cast<int>(f.objects.size())) {
            const mob::Object& o = f.objects[i];
            out.push_back({i, o.position, o.complection, {o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]}});
        }
    return out;
}

void ApplyObjects(mob::File& f, const std::vector<ObjectState>& states) {
    for (const ObjectState& st : states) {
        if (st.index < 0 || st.index >= static_cast<int>(f.objects.size())) continue;
        { // a unit's AI places (guard place: the guard and help circles; patrol and look points) move with it
            const mob::Object& o = f.objects[st.index];
            const float dx = st.position.x - o.position.x, dy = st.position.y - o.position.y;
            const std::vector<mob::Logic> logics = o.logics; // (SetLogic rewrites the object)
            if (o.kind == mob::Kind::Unit && (dx != 0 || dy != 0))
                for (size_t gi = 0; gi < logics.size(); ++gi) {
                    mob::Logic g = logics[gi];
                    if (g.guardPlace.x != 0 || g.guardPlace.y != 0) { g.guardPlace.x += dx; g.guardPlace.y += dy; }
                    for (mob::PatrolPoint& pt : g.patrol) {
                        pt.position.x += dx; pt.position.y += dy;
                        for (mob::LookPoint& l : pt.looks) { l.position.x += dx; l.position.y += dy; }
                    }
                    mob::SetLogic(f, st.index, static_cast<int>(gi), g);
                }
        }
        mob::SetPosition(f, f.objects[st.index], st.position);
        mob::SetComplection(f, f.objects[st.index], st.complection);
        mob::SetRotation(f, f.objects[st.index], st.rotation);
    }
}

// Puts a step's state back, returning the state it replaced (for the other stack).
static bool ApplyStep(App& app, const EditStep& step, EditStep& replaced) {
    replaced = EditStep{};
    replaced.kind = step.kind;
    replaced.file = step.file;
    if (step.kind == EditStep::Terrain) {
        if (!app.terrainLoaded || step.file != app.terrainPath) return false;
        mpr::Map& m = app.terrain;
        for (const auto& [i, sector] : step.sectors) {
            if (i < 0 || i >= static_cast<int>(m.sectors.size())) continue;
            replaced.sectors.push_back({i, m.sectors[static_cast<size_t>(i)]});
            m.sectors[static_cast<size_t>(i)] = sector;
            app.terrainEditedSectors.insert(i);
        }
        if (step.header) {
            replaced.header = true;
            replaced.materials = m.materials;
            replaced.tileTypes = m.tileTypes;
            replaced.animTiles = m.animTiles;
            m.materials = step.materials;
            m.tileTypes = step.tileTypes;
            m.animTiles = step.animTiles;
            app.terrainHeaderEdited = true;
        }
        app.terrainRebuild = true;
        return true;
    }
    if (step.kind == EditStep::Areas) {
        if (!app.openQuest) return false;
        replaced.areas = app.openQuest->shown.exits;
        app.openQuest->shown.exits = step.areas;
        app.rectDrag = App::RectDrag{};
        app.questDirty = !SameAreas(app.openQuest->shown.exits, app.savedAreas);
        return true;
    }
    MobEntry* m = FindMob(app, step.file);
    if (!m) return false;
    if (step.kind == EditStep::Bytes) {
        replaced.bytes = m->file.bytes;
        m->file.bytes = step.bytes;
        mob::Reparse(m->file);
        app.listedKey.clear();
    } else if (step.kind == EditStep::Objects) {
        std::vector<int> indices;
        for (const ObjectState& st : step.objects) indices.push_back(st.index);
        replaced.objects = CaptureObjects(m->file, indices);
        ApplyObjects(m->file, step.objects);
    } else {
        replaced.diplomacy = m->file.diplomacy;
        for (int i = 0; i < 1024 && i < static_cast<int>(step.diplomacy.size()); ++i) mob::SetDiplomacy(m->file, i / 32, i % 32, step.diplomacy[i]);
    }
    app.checksDirty = true;
    return true;
}

void UndoRedo(App& app, bool redo) {
    auto& from = redo ? app.redoSteps : app.undoSteps;
    auto& to = redo ? app.undoSteps : app.redoSteps;
    while (!from.empty()) {
        EditStep step = std::move(from.back());
        from.pop_back();
        EditStep replaced;
        if (!ApplyStep(app, step, replaced)) continue; // its map or quest is no longer open
        replaced.label = step.label; // the same step, the other way
        to.push_back(std::move(replaced));
        app.questMessage = redo ? "Redone" : "Undone";
        return;
    }
}

void CloseQuest(App& app) {
    app.openQuest.reset();
    app.scene.quest = nullptr;
    app.lib.mapQuest.clear();
    app.questDirty = false;
    DropAreaSteps(app);
    app.rectDrag = App::RectDrag{};
    app.mqLoadedFor.clear();
    app.mqEntriesFor.clear();
}

void SetOpenQuest(App& app, const quest::QuestSet& set) {
    app.openQuest = std::make_unique<quest::QuestSet>(set);
    app.scene.quest = &app.openQuest->shown;
    app.questDirty = false;
    app.savedAreas = app.openQuest->shown.exits;
    DropAreaSteps(app);
    app.mqCopy = 0;
    app.mqLoadedFor.clear();
    app.mqEntriesFor.clear();
}

// Reads the open quest's copies from disk again: after saving (the undo history stays, so a save can
// be undone too), or to drop unsaved changes (Revert: the history goes).
void ReloadQuest(App& app, bool keepHistory) {
    if (!app.openQuest) return;
    quest::QuestSet& s = *app.openQuest;
    for (quest::Quest& c : s.copies) {
        quest::Quest fresh;
        if (quest::Load(c.path, fresh)) c = fresh;
    }
    quest::Quest fresh;
    if (quest::Load(s.shown.path, fresh)) s.shown = fresh;
    app.scene.quest = &s.shown;
    app.questDirty = false;
    app.savedAreas = s.shown.exits;
    if (!keepHistory) DropAreaSteps(app);
}

// Writes the resized areas into map.txt of every copy of the quest (all its language packs).
void SaveQuestAreas(App& app) {
    if (!app.openQuest) return;
    quest::QuestSet& s = *app.openQuest;
    int saved = 0;
    std::string errors;
    for (const quest::Quest& c : s.copies) {
        std::string entry = quest::MapTxtEntry(c);
        std::vector<uint8_t> bytes;
        if (entry.empty() || !quest::ReadEntry(c, entry, bytes)) { errors += " " + c.path + " (no map.txt)"; continue; }
        std::string text(bytes.begin(), bytes.end());
        std::string updated = quest::UpdateRects(text, s.shown.exits);
        std::string err;
        if (updated == text) { ++saved; continue; }
        if (!quest::WriteEntries(c, {{entry, std::vector<uint8_t>(updated.begin(), updated.end())}}, err)) { errors += " " + c.path + ": " + err; continue; }
        ++saved;
    }
    app.questMessage = "Saved the areas in " + std::to_string(saved) + " of " + std::to_string(s.copies.size()) + " cop" +
                       (s.copies.size() == 1 ? "y" : "ies") + (errors.empty() ? "" : "; failed:" + errors);
    ReloadQuest(app);
}

// Opens a quest instead of the loaded files: its terrain, its zone's base map and its own map (made
// active), with its lighting file picked, and the camera on where the party is deployed.
void OpenQuest(App& app, const quest::QuestSet& listed) {
    if (BlockedByUnsaved(app)) return;
    // Read from disk again: the quest list keeps what it read when it was scanned (a saved change of the areas,
    // or one made outside, would come back otherwise).
    quest::QuestSet set = listed;
    for (quest::Quest& c : set.copies) {
        quest::Quest fresh;
        if (quest::Load(c.path, fresh)) c = fresh;
    }
    {
        quest::Quest fresh;
        if (quest::Load(set.shown.path, fresh)) set.shown = fresh;
    }
    const quest::Quest& q = set.shown;
    std::string missing;
    std::string terrain = q.terrain.empty() ? std::string() : ResolveMapFile(app, q.terrain + ".mpr", q.path);
    std::string base = q.baseMap.empty() ? std::string() : ResolveMapFile(app, q.baseMap + ".mob", q.path);
    std::string own = ResolveMapFile(app, q.name + ".mob", q.path);
    if (terrain.empty()) missing += " " + q.terrain + ".mpr";
    if (base.empty()) missing += " " + q.baseMap + ".mob";
    if (own.empty()) missing += " " + q.name + ".mob";
    // The same terrain as now (the quest again, or another quest of the zone): the camera stays where it is.
    const bool sameTerrain = app.terrainLoaded && !terrain.empty() && Lower(app.terrainPath) == Lower(terrain);
    app.mobs.clear();
    app.loadOrder.clear();
    app.terrainLoaded = false;
    app.terrainPath.clear();
    app.terrainDirty = true;
    app.scene.ClearSelection();
    if (!terrain.empty()) LoadTerrain(app, terrain);
    if (!base.empty()) AddMob(app, base);
    if (!own.empty()) AddMob(app, own);
    app.activeMob = std::max(0, static_cast<int>(app.mobs.size()) - 1);
    SyncScene(app);
    SetOpenQuest(app, set);
    app.lib.mapQuest = q.path;
    for (const lighting::Table& t : app.lightTables)
        if (Lower(t.name) == Lower(q.LightingFile())) app.lib.lightingChoice = t.path;
    app.filesMessage = missing.empty() ? "Opened the quest " + q.name
                                       : "Opened the quest " + q.name + "; not found in the map folders or next to it:" + missing;
    SaveSession(app);
    app.terrainDirty = true;
    if (sameTerrain) app.framed = true;
    else if (!q.exits.empty() && q.exits[0].deploy.set) { app.pendingFocusRect = q.exits[0].deploy; app.framed = true; }
    else app.framed = false;
}

void RescanQuests(App& app) {
    std::vector<std::string> key = app.lib.questFolders;
    key.push_back("|packs|");
    key.insert(key.end(), app.lib.questPacks.begin(), app.lib.questPacks.end());
    if (app.questsFor == key) return;
    app.questsFor = key;
    app.quests = quest::Scan(app.lib.questFolders, app.lib.questPacks);
}

void OpenPaths(App& app, const std::vector<std::string>& paths) {
    for (const std::string& p : paths) {
        if (EndsWith(p, ".mpr")) LoadTerrain(app, p);
        else AddMob(app, p);
    }
}

// ------------------------------------------------------------------------------------------------
// Checks
// ------------------------------------------------------------------------------------------------

// The scripts' areas and quests, built again when the loaded maps change.
void RefreshScriptModel(App& app) {
    std::string key;
    for (auto& m : app.mobs) key += m->file.path + "|" + std::to_string(m->file.bytes.size()) + ";";
    if (key == app.scriptModelKey) return;
    app.scriptModelKey = key;
    app.scriptModel.Clear();
    for (auto& m : app.mobs) {
        const size_t before = app.scriptModel.quests.size();
        quests::AddMob(app.scriptModel, m->file);
        const size_t dot = m->file.path.find_last_of('.');
        for (size_t i = before; i < app.scriptModel.quests.size(); ++i) {
            quests::Quest& q = app.scriptModel.quests[i];
            if (dot == std::string::npos || !quests::LoadTexts(m->file.path.substr(0, dot) + ".mq", q))
                quests::LoadTexts(app.lib.questFolders, app.lib.questPacks, q);
        }
    }
    app.scene.scriptAreas.clear();
    for (const auto& [id, shapes] : app.scriptModel.areas)
        for (const quests::Area& a : shapes) {
            MapScene::ScriptArea s;
            s.id = id; s.round = a.round; s.x = a.x; s.y = a.y; s.r = a.r; s.x2 = a.x2; s.y2 = a.y2;
            app.scene.scriptAreas.push_back(s);
        }
}

void RunChecks(App& app) {
    app.database.Load(app.lib.dbPath);
    checks::Inputs in;
    for (auto& m : app.mobs) in.maps.push_back(&m->file);
    in.terrain = app.terrainLoaded ? &app.terrain : nullptr;
    in.figures = &app.lib.figures;
    in.textures = &app.lib.textures;
    in.database = &app.database;
    in.scriptChecks = app.checkedScripts = app.lib.scriptChecks;
    in.databaseChecks = app.checkedDatabase = app.lib.databaseChecks;
    app.checkedAtd = app.lib.atdScripts;
    const checks::Summary before = app.summary;
    app.findings = checks::Run(in, &app.summary);
    if (app.summary.errors > before.errors) app.newErrors = app.summary.errors - before.errors;
    if (app.summary.warnings > before.warnings) app.newWarnings = app.summary.warnings - before.warnings;
    app.checksDirty = false;
    app.checkedVersion = app.lib.version;
}

ImVec4 SeverityColor(char s) {
    return s == 'E' ? ImVec4(0.95f, 0.42f, 0.38f, 1) : s == 'W' ? ImVec4(0.95f, 0.78f, 0.35f, 1) : ImVec4(0.6f, 0.75f, 0.95f, 1);
}


} // namespace mapedit
