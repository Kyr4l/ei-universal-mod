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

#include "map_app.hpp"

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h" // the script editor's completion: key ownership, the text box's scroll

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <atomic>
#include <memory>
#include <thread>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "../viewer/library.hpp"
#include "../viewer/ui_common.hpp"
#include "tile_blend.hpp"
#include "../viewer/png_writer.hpp"
#include "../viewer/ui_sources.hpp"
#include "checks.hpp"
#include "lighting.hpp"
#include "map_scene.hpp"
#include "../viewer/scene.hpp"
#include "navmesh_gen.hpp"
#include "quest_file.hpp"
#include "text_codec.hpp"
#include "script_highlight.hpp"
#include "../dllconnect/quests.hpp"
#include "../subtools.hpp"

namespace mapedit {

struct MobEntry {
    mob::File file;
    bool visible = true;
    std::vector<uint8_t> savedBytes; // the file as on disk: edits patch file.bytes, so dirty = they differ
    bool Dirty() const { return file.loaded && file.bytes != savedBytes; }
};

// One undoable edit: the state before it (undo puts it back and keeps the state it replaced for redo).
struct ObjectState { int index; mob::Vec3 position, complection; float rotation[4]; };
// A move or scale in progress: the selection's state before it, and how the mouse started.
struct Transform {
    enum Mode { None, Move, Scale, Rotate } mode = None;
    int axes = 0;                        // bit 0 X, 1 Y, 2 Z: what changes
    ImVec2 startMouse{0, 0};
    fig::Vec3 startGround{};
    bool haveGround = false;
    std::string file;
    std::vector<ObjectState> before;
    mob::Vec3 centre;
    mob::Vec3 delta;                     // the current offset (move) or complection change (scale), for the status
    float angle = 0;                     // rotate: the current angle in degrees
    float startAngle = 0;                // rotate: the mouse's angle around the selection's centre on screen at the start
    std::string typed;                   // a value typed while transforming (replaces the mouse): "5", "-2.5", "1,0,3"
    // Moving logic points instead of objects: the points, the file as it was, and the logic records they are in.
    std::vector<LogicPointRef> points;
    std::vector<uint8_t> bytesBefore;
    std::map<std::pair<int, int>, mob::Logic> logicsBefore;
    std::map<int, std::pair<std::vector<mob::Vec3>, std::vector<mob::Vec3>>> trapsBefore; // trap -> areas, cast points
};

// The typed values, one per axis in use (in X, Y, Z order); a single value goes to each of them.
// Empty when nothing usable is typed.
static std::vector<float> TypedValues(const std::string& typed, int count) {
    std::vector<float> v;
    size_t start = 0;
    while (start <= typed.size()) {
        size_t end = typed.find(',', start);
        if (end == std::string::npos) end = typed.size();
        const std::string part = typed.substr(start, end - start);
        char* stop = nullptr;
        const float f = part.empty() ? 0.0f : std::strtof(part.c_str(), &stop);
        if (!part.empty() && (stop == part.c_str() || *stop)) return {};
        v.push_back(f);
        start = end + 1;
    }
    if (v.size() == 1) v.assign(count, v[0]);
    if (static_cast<int>(v.size()) != count || typed.empty()) return {};
    return v;
}

struct EditStep {
    enum Kind { Areas, Objects, Diplomacy, Bytes, Terrain } kind = Areas;
    // Terrain (file = the .mpr's path): the sectors as they were (index y * sectorsX + x), and when
    // `header`, the materials, tile types and animated tiles.
    std::vector<std::pair<int, mpr::Sector>> sectors;
    bool header = false;
    std::vector<mpr::Material> materials;
    std::vector<int> tileTypes;
    std::vector<std::pair<int, int>> animTiles;
    std::vector<quest::Exit> areas;      // Areas: the open quest's exits
    std::string file;                    // Objects / Diplomacy: the map's path
    std::vector<ObjectState> objects;    // Objects
    std::vector<int32_t> diplomacy;      // Diplomacy: the whole table
    std::vector<uint8_t> bytes;          // Bytes: the whole file (edits that change sizes)
    std::string label;                   // what it was, for the undo history
};

enum class SideTab { Files, Objects, Checks, Script, Quest, Diplomacy, Ids, Terrain, Count };

// The Objects tab's tree (like ei_maper's): kind > group (prototype or template) > objects.
struct ObjectGroup { std::string label; std::vector<int> objects; };
struct ObjectCategory { mob::Kind kind; std::vector<ObjectGroup> groups; int count = 0; };

struct App {
    explicit App(Library& shared) : lib(shared) {}
    Library& lib;
    MapScene scene;

    // Files
    mpr::Map terrain;
    std::string navCompareNote; // "Navmesh differences": what its build left out
    bool terrainLoaded = false;
    // Terrain editing: the brush (a tile: texture * 64 + tile in it, a rotation in quarter turns; on
    // water, a liquid material, -1 removing the water from the tile), eight quick tiles, what is unsaved.
    bool tileBrush = false, brushWater = false;
    bool heightBrush = false;         // Terrain > Height brush: raise, lower, smooth, flatten
    int heightMode = 0;
    float heightRadius = 4.0f, heightStrength = 3.0f, flattenLevel = 0;
    int brushTile = 0, brushRotation = 0, brushMaterial = 0;
    int quickTiles[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::set<int> terrainEditedSectors;
    bool terrainHeaderEdited = false, terrainRebuild = false;
    bool painting = false;
    EditStep stroke;
    std::string terrainMessage;
    bool terrainSaveAsOpen = false;
    char terrainSaveAsPath[1024] = "";
    std::string terrainPath, terrainError;
    bool terrainDirty = false;               // (re)upload to GL in the next RenderGl
    std::vector<std::unique_ptr<MobEntry>> mobs;
    // The loaded maps' scripts: their quests (with their .mq texts) and the areas they declare.
    quests::Model scriptModel;
    std::string scriptModelKey;
    // Script tab -> Areas -> "Place here": the next click on the map moves that area there.
    struct AreaPlace { bool on = false; std::string file; size_t index = 0; } areaPlace;
    // Alt + drag on a script area in the view: the area followed (scene index), from where, by how much.
    struct AreaDrag {
        bool on = false; std::string file; size_t index = 0; int scene = -1; float startX = 0, startY = 0, dx = 0, dy = 0;
        int resize = 0;    // 0 move; 1 a round area's radius; else rect sides: 2 v[0], 4 v[1], 8 v[2], 16 v[3]
        float v[4] = {};   // the area as it is being resized
    } areaDrag;
    int activeMob = 0;                       // the map whose objects can be selected (Ctrl+T)
    std::vector<std::string> loadOrder;      // paths of the loaded files, oldest first (U unloads the last)
    std::vector<Library::MapFile> folderFiles; // the map folders' files (Settings), for mapsVersion
    int listedMapsVersion = -1;
    char folderFilter[128] = "";
    std::vector<quest::QuestSet> quests;     // the quests of the quest folders and language packs (Settings)
    std::vector<std::string> questsFor;      // lib.questFolders + questPacks they were read for
    char questFilter[128] = "";
    std::unique_ptr<quest::QuestSet> openQuest; // the open quest; its areas are drawn and can be resized
    bool questDirty = false;                 // areas changed and not saved
    // Undo / redo (Ctrl+Z / Ctrl+Y) of the quest's areas, objects moved or scaled, and diplomacy.
    std::vector<EditStep> undoSteps, redoSteps;
    std::vector<quest::Exit> savedAreas;     // as on disk, to tell whether undoing reached it
    std::string questMessage;

    // Area handles in the view (drag to resize or move)
    struct RectDrag { bool on = false; int exit = -1; bool remove = false; int handle = -1; quest::Rect start; fig::Vec3 from; };
    RectDrag rectDrag;

    // MQ tab: one file of the quest, as text
    int mqCopy = 0;                          // which copy (language pack)
    std::string mqEntry, mqLoadedFor;        // the file, and "copy path|file" the buffer holds
    std::string mqText;                      // UTF-8
    codec::Encoding mqEncoding = codec::Encoding::Cp1251;
    bool mqIsReg = false, mqTextDirty = false, mqApplyAll = true, mqCrlf = true;
    std::string mqMessage;
    std::vector<std::string> mqEntries;
    std::string mqEntriesFor;
    char terrainInput[1024] = "";
    char mobInput[1024] = "";
    std::string filesMessage;

    // Checks
    checks::DatabaseNames database;
    std::vector<checks::Finding> findings;
    checks::Summary summary;
    int newErrors = 0, newWarnings = 0; // more than the checks before found, not reported yet (TakeNewProblems)
    bool checksDirty = true;
    int checkedVersion = -1;
    bool showErrors = true, showWarnings = true, showInfos = true;

    // Objects
    char filter[128] = "";
    std::vector<ObjectCategory> tree;        // the active map's objects passing the filter
    std::string listedKey;                   // what the tree was built for
    bool revealSelection = false;            // open the tree at the selected object and scroll to it

    // Keys
    bool keyWasDown[config::kMapKeyCount] = {};
    bool switchListShown = false;            // the active-map list, shown from the switch key's press...
    bool switchListLatched = false;          // ...until its modifiers (Ctrl) are released

    // Lighting
    std::vector<std::string> lightingFor;    // lib.lightingFiles the tables were read for
    std::vector<lighting::Table> lightTables;
    float hour = 12.0f;
    bool hourSet = false;                    // the user moved the hour slider

    // Script
    int scriptFile = 0;
    int scriptLine = 0;                      // highlight / scroll to this line (1-based)
    bool scrollToLine = false;

    SideTab requestTab = SideTab::Count;
    bool mouseHeld[5] = {};                   // a drag of that mouse button started over the view
    bool viewHovered = false;                 // the mouse is over the 3D view (this frame)
    ImVec2 viewMin{0, 0}, viewSize{1, 1};     // the 3D view's rectangle this frame
    bool swallowLeftRelease = false;          // the left click that confirmed a move/scale: not a selection

    // Blender-like move (G) and scale (T) of the selection, constrained with X / Y / Z (Shift: all but).
    Transform xf;

    // Find objects (Ctrl+F)
    bool findOpen = false, findFocus = false, findCase = false, findMore = false, findIds = false;
    char findText[256] = "";
    int findIdMin = 0, findIdMax = 999999, findKind = 0;
    std::string findMessage;

    // Diplomacy tab
    bool dipSymmetric = true;

    // Object parameters: apply an edit to every selected object (else the one shown)
    bool editAll = true;
    std::string editMessage;

    // IDs tab
    int idRange = 0;
    uint32_t idStart = 0;
    std::string idMessage;
    struct IdIssue { std::string text; int object; };
    std::vector<IdIssue> idIssues;
    bool idChecked = false;

    // Logic editing: the record shown for the selected unit
    int logicRecord = -1, logicRecordFor = -1;


    // Script editing: the text being edited (UTF-8) and for which map, the line of the cursor; the copy
    // given to an external editor, watched for saves.
    bool scriptEditing = false;
    std::string scriptEdit, scriptEditFor, scriptMessage;
    int scriptCursorLine = 1;
    // Completion in the script editor: the word before the cursor, what it may become, the one picked,
    // the cursor's line and column text (for placing the list), and the signature of the call around it.
    struct Completion {
        std::string prefix;
        std::vector<std::string> items;
        int index = 0, wordStart = 0, line = 0;
        std::string lineBeforeCursor, signature;
        bool dismissed = false, tab = false, accept = false, keepCursor = false;
        int cursor = 0; // where the cursor was at the last callback
        size_t wordsFor = 0;
        std::vector<std::string> words; // names written in the text (original case)
    } completion;
    bool scriptEditFocus = false;
    std::string scriptExtPath, scriptExtMob;
    std::filesystem::file_time_type scriptExtTime{};

    // Orbiting turns the camera around the point under the cursor when the button went down.
    bool orbitPivotSet = false;
    fig::Vec3 orbitPivot{};

    // The patrol simulation (units walking their paths; nothing written)
    struct SimUnit {
        const mob::Object* object = nullptr;
        mob::Logic logic;
        float x = 0, y = 0, yaw = 0, speed = 3.5f, timer = 0;
        int layer = 1, point = 0, dir = 1, look = 0, leg = 0;
        enum Phase { Walk, Look } phase = Walk;
        std::vector<mob::Vec3> route;
    };
    bool simulating = false, simPaused = false;
    float simSpeed = 1.0f;
    std::vector<SimUnit> sim;
    size_t simObjects = 0;

    // Windows: the script in its own window, the undo history, the MOB parameters
    bool scriptWindow = false, historyOpen = false, mobParamsOpen = false;
    bool focusScriptWindow = false;

    // Save active MOB as...
    bool saveAsOpen = false;
    char saveAsPath[1024] = "";
    bool saveAsOverwrite = false;
    std::string saveAsMessage;

    // Randomize: one parameter of each selected object set to (or moved by) a random value in a range
    bool randomOpen = false;
    int randomParam = 9, randomMode = 0;
    float randomMin = 0.8f, randomMax = 1.2f;
    std::string randomMessage;

    // Clipboard of copied objects (whole nodes), and the New object window
    struct Copied { std::vector<uint8_t> node; mob::Vec3 position; };
    std::vector<Copied> clipboard;
    std::vector<MapScene::Missing> missing; // objects whose figure cannot be shown (#72)
    int missingCheck = 0;
    bool missingOpen = false;
    bool newOpen = false;
    char newFigure[128] = "";
    char newName[128] = "";
    char newTexture[128] = "";
    char newFilter[64] = "";
    std::string newMessage;
    int newTab = 0, newCategory = 0;           // Add object / unit: 0 objects, 1 units; the figure category
    std::string newUnit;                       // the database unit chosen
    bool newPreview = true;
    std::unique_ptr<Scene> preview;            // the 3D preview (the 3D Viewer's renderer), drawn in RenderGl
    std::string previewWanted, previewShown;   // what to show / what the texture shows
    GLuint previewTex = 0;
    float previewYaw = 30.0f;

    // Offset: move the selection along one axis by an exact value
    bool offsetOpen = false;
    int offsetAxis = 0;
    double offsetValue = 1.0;
    std::string offsetMessage;

    // Minimap export
    int blendTileB = -1, blendMask = 0, blendSlot = -1; // tile painting: Blend two tiles
    float blendSoftness = 0.35f;
    int blendSeed = 1;
    std::string blendMessage, blendFolder;
    bool newTerrainOpen = false; // Tools > New terrain
    char newTerrainPath[512] = "";
    int newTerrainSize[2] = {4, 4};
    float newTerrainHeight = 10.0f;
    bool newTerrainOwnTextures = false;
    std::string newTerrainMessage;
    bool minimapOpen = false, minimapPending = false, minimapObjects = true, minimapUnits = false;
    bool minimapMmp = true, minimapDds = false, minimapPng = true; // the formats written
    int minimapSize = 1024;
    char minimapPath[1024] = "";
    std::string minimapMessage;
    quest::Rect pendingFocusRect;             // focus the camera on it after the next terrain upload
    uint32_t pendingFocusId = 0;              // gui --map --focus <id>: select that object and look at it once loaded
    float sidebarWidth = 470.0f;
    bool drawnThisFrame = false;
    ImVec2 viewportMin{0, 0}, viewportMax{0, 0};
    bool hoverGround = false;
    fig::Vec3 ground{};
    int seenFigures = -1, seenTextures = -1;
    bool framed = false;
};

struct Context {
    explicit Context(Library& lib) : app(lib) {}
    App app;
};

// ------------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------------

static std::string Lower(std::string s) { return checks::Lower(std::move(s)); }

static bool EndsWith(const std::string& s, const char* ext) {
    std::string l = Lower(s), e = ext;
    return l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0;
}

static void SaveSession(App& app) {
    app.lib.mapTerrain = app.terrainLoaded ? app.terrainPath : std::string();
    app.lib.mapMobs.clear();
    for (auto& m : app.mobs) app.lib.mapMobs.push_back(m->file.path);
    app.lib.SaveConfig();
}

static void SyncScene(App& app) {
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

static bool TerrainUnsaved(const App& app);
static void ForgetTerrainEdits(App& app);
static bool LoadTerrain(App& app, const std::string& path) {
    if (TerrainUnsaved(app)) {
        app.filesMessage = "The terrain has unsaved changes: save them first (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ") or undo them";
        return false;
    }
    std::string err;
    mpr::Map map;
    if (!mpr::Load(path, map, err)) {
        app.terrainError = err;
        app.filesMessage = "Terrain not loaded: " + err;
        return false;
    }
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

static bool AddMob(App& app, const std::string& path) {
    for (auto& m : app.mobs) {
        if (m->file.path == path) { app.filesMessage = path + " is already loaded"; return false; }
    }
    auto entry = std::make_unique<MobEntry>();
    mob::Load(path, entry->file);
    entry->savedBytes = entry->file.bytes;
    if (!entry->file.loaded) app.filesMessage = entry->file.fileName + ": " + entry->file.error;
    app.mobs.push_back(std::move(entry));
    app.loadOrder.push_back(path);
    SyncScene(app);
    if (!app.terrainLoaded) app.framed = false;
    return true;
}

// A quest map's base map and terrain (from its .mq) that are not loaded yet.
struct QuestSuggestion { bool any = false; std::string baseMob, terrain, label; };

static QuestSuggestion SuggestForQuest(const App& app) {
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

static void SetActiveMob(App& app, int index) {
    if (app.mobs.empty()) return;
    app.activeMob = ((index % static_cast<int>(app.mobs.size())) + static_cast<int>(app.mobs.size())) % static_cast<int>(app.mobs.size());
    app.scene.ClearSelection();
    app.listedKey.clear();
    SyncScene(app);
}

// Unloading or replacing a map with unsaved edits would lose them: refuse, and say how to save.
static bool BlockedByUnsaved(App& app, const MobEntry* only = nullptr) {
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

static bool TerrainUnsaved(const App& app) { return app.terrainLoaded && (!app.terrainEditedSectors.empty() || app.terrainHeaderEdited); }

static void ForgetTerrainEdits(App& app) {
    app.terrainEditedSectors.clear();
    app.terrainHeaderEdited = false;
    app.painting = false;
    for (auto* v : {&app.undoSteps, &app.redoSteps})
        v->erase(std::remove_if(v->begin(), v->end(), [](const EditStep& e) { return e.kind == EditStep::Terrain; }), v->end());
}

static void UnloadTerrain(App& app) {
    if (TerrainUnsaved(app)) {
        app.filesMessage = "The terrain has unsaved changes: save them first (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ") or undo them";
        app.questMessage = app.filesMessage;
        return;
    }
    ForgetTerrainEdits(app);
    app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), app.terrainPath), app.loadOrder.end());
    app.terrainLoaded = false;
    app.terrainPath.clear();
    app.terrainDirty = true;
    app.checksDirty = true;
}

static void RemoveMob(App& app, int index) {
    if (index < 0 || index >= static_cast<int>(app.mobs.size())) return;
    if (BlockedByUnsaved(app, app.mobs[index].get())) return;
    const std::string path = app.mobs[index]->file.path;
    app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), path), app.loadOrder.end());
    app.mobs.erase(app.mobs.begin() + index);
    if (app.activeMob > index || app.activeMob >= static_cast<int>(app.mobs.size())) app.activeMob = std::max(0, app.activeMob - 1);
    app.scene.ClearSelection();
    SyncScene(app);
}

// The U key: unloads whichever file was loaded last, terrain or map.
static void UnloadLast(App& app) {
    if (app.loadOrder.empty()) return;
    const std::string path = app.loadOrder.back();
    if (app.terrainLoaded && path == app.terrainPath) {
        UnloadTerrain(app);
        app.filesMessage = "Unloaded the terrain " + path;
    } else {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (app.mobs[i]->file.path == path) {
                if (BlockedByUnsaved(app, app.mobs[i].get())) return;
                RemoveMob(app, static_cast<int>(i));
                break;
            }
        app.filesMessage = "Unloaded " + path;
        app.loadOrder.erase(std::remove(app.loadOrder.begin(), app.loadOrder.end(), path), app.loadOrder.end());
    }
    SaveSession(app);
}

// A map file named in a quest: from the map folders of Settings, else next to the quest.
static std::string ResolveMapFile(const App& app, const std::string& fileName, const std::string& questPath) {
    const std::string lower = Lower(fileName);
    for (const Library::MapFile& f : app.lib.ListMapFiles()) if (Lower(f.name) == lower) return f.path;
    std::error_code ec;
    std::filesystem::path p(questPath);
    std::string dir = std::filesystem::is_directory(p, ec) ? p.parent_path().string() : checks::DirectoryOf(questPath);
    return checks::FindInDirectory(dir, fileName);
}

static void FocusRect(App& app, const quest::Rect& r) {
    if (!r.set) return;
    OrbitCamera& cam = app.scene.camera;
    cam.targetX = (r.x1 + r.x2) * 0.5f;
    cam.targetY = (r.y1 + r.y2) * 0.5f;
    cam.targetZ = app.scene.Ground(cam.targetX, cam.targetY);
    cam.distance = std::max(25.0f, std::max(std::fabs(r.x2 - r.x1), std::fabs(r.y2 - r.y1)) * 2.5f);
}

static bool SameAreas(const std::vector<quest::Exit>& a, const std::vector<quest::Exit>& b) {
    if (a.size() != b.size()) return false;
    auto same = [](const quest::Rect& x, const quest::Rect& y) {
        return x.set == y.set && x.x1 == y.x1 && x.y1 == y.y1 && x.x2 == y.x2 && x.y2 == y.y2;
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (!same(a[i].deploy, b[i].deploy) || !same(a[i].remove, b[i].remove)) return false;
    return true;
}

static void PushUndo(App& app, EditStep step, std::string label = std::string()) {
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
static void PushAreaUndo(App& app, const std::vector<quest::Exit>& before) {
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

static MobEntry* FindMob(App& app, const std::string& path) {
    for (auto& m : app.mobs) if (m->file.path == path) return m.get();
    return nullptr;
}

static std::vector<ObjectState> CaptureObjects(const mob::File& f, const std::vector<int>& indices) {
    std::vector<ObjectState> out;
    for (int i : indices)
        if (i >= 0 && i < static_cast<int>(f.objects.size())) {
            const mob::Object& o = f.objects[i];
            out.push_back({i, o.position, o.complection, {o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]}});
        }
    return out;
}

static void ApplyObjects(mob::File& f, const std::vector<ObjectState>& states) {
    for (const ObjectState& st : states) {
        if (st.index < 0 || st.index >= static_cast<int>(f.objects.size())) continue;
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

static void UndoRedo(App& app, bool redo) {
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

static void CloseQuest(App& app) {
    app.openQuest.reset();
    app.scene.quest = nullptr;
    app.lib.mapQuest.clear();
    app.questDirty = false;
    DropAreaSteps(app);
    app.rectDrag = App::RectDrag{};
    app.mqLoadedFor.clear();
    app.mqEntriesFor.clear();
}

static void SetOpenQuest(App& app, const quest::QuestSet& set) {
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
static void ReloadQuest(App& app, bool keepHistory = true) {
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
static void SaveQuestAreas(App& app) {
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
static void OpenQuest(App& app, const quest::QuestSet& listed) {
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

static void RescanQuests(App& app) {
    std::vector<std::string> key = app.lib.questFolders;
    key.push_back("|packs|");
    key.insert(key.end(), app.lib.questPacks.begin(), app.lib.questPacks.end());
    if (app.questsFor == key) return;
    app.questsFor = key;
    app.quests = quest::Scan(app.lib.questFolders, app.lib.questPacks);
}

static void OpenPaths(App& app, const std::vector<std::string>& paths) {
    for (const std::string& p : paths) {
        if (EndsWith(p, ".mpr")) LoadTerrain(app, p);
        else AddMob(app, p);
    }
}

// ------------------------------------------------------------------------------------------------
// Checks
// ------------------------------------------------------------------------------------------------

// The scripts' areas and quests, built again when the loaded maps change.
static void RefreshScriptModel(App& app) {
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

static void RunChecks(App& app) {
    app.database.Load(app.lib.dbPath);
    checks::Inputs in;
    for (auto& m : app.mobs) in.maps.push_back(&m->file);
    in.terrain = app.terrainLoaded ? &app.terrain : nullptr;
    in.figures = &app.lib.figures;
    in.textures = &app.lib.textures;
    in.database = &app.database;
    const checks::Summary before = app.summary;
    app.findings = checks::Run(in, &app.summary);
    if (app.summary.errors > before.errors) app.newErrors = app.summary.errors - before.errors;
    if (app.summary.warnings > before.warnings) app.newWarnings = app.summary.warnings - before.warnings;
    app.checksDirty = false;
    app.checkedVersion = app.lib.version;
}

static ImVec4 SeverityColor(char s) {
    return s == 'E' ? ImVec4(0.95f, 0.42f, 0.38f, 1) : s == 'W' ? ImVec4(0.95f, 0.78f, 0.35f, 1) : ImVec4(0.6f, 0.75f, 0.95f, 1);
}

// ------------------------------------------------------------------------------------------------
// Sidebar tabs
// ------------------------------------------------------------------------------------------------

static void SelectObject(App& app, int file, int object, bool focus) {
    if (file >= 0 && file != app.activeMob) SetActiveMob(app, file); // e.g. a finding in another map
    app.scene.Select(file, object);
    if (focus && file >= 0 && file < static_cast<int>(app.mobs.size()) && object >= 0 &&
        object < static_cast<int>(app.mobs[file]->file.objects.size()))
        app.scene.FocusOn(app.mobs[file]->file.objects[object]);
}

static bool IsLoaded(const App& app, const std::string& path) {
    if (app.terrainLoaded && app.terrainPath == path) return true;
    for (auto& m : app.mobs) if (m->file.path == path) return true;
    return false;
}

// The .mpr and .mob files of the map folders set in Settings: a click loads one.
static void FolderList(App& app) {
    if (app.listedMapsVersion != app.lib.mapsVersion) {
        app.folderFiles = app.lib.ListMapFiles();
        app.listedMapsVersion = app.lib.mapsVersion;
    }
    ImGui::SeparatorText("In the map folders");
    if (app.lib.maps.layers.empty()) {
        ImGui::TextDisabled("Add the game's (and a mod's) maps folder in the Settings tab to list their files here.");
        return;
    }
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##folderfilter", "filter, e.g. zone12", app.folderFilter, sizeof(app.folderFilter));
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        for (auto& layer : app.lib.maps.layers) {
            std::string err;
            layer.ok = layer.source.Load(layer.path, err);
            layer.error = err;
        }
        ++app.lib.mapsVersion;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for new or removed files in the folders");
    const std::string filter = Lower(app.folderFilter);
    if (ImGui::BeginTable("##folderfiles", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 190))) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthFixed, 150);
        for (size_t i = 0; i < app.folderFiles.size(); ++i) {
            const Library::MapFile& f = app.folderFiles[i];
            if (!filter.empty() && Lower(f.name).find(filter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            const bool loaded = IsLoaded(app, f.path);
            if (loaded) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.5f, 1));
            if (ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !loaded) {
                if (f.terrain ? LoadTerrain(app, f.path) : AddMob(app, f.path)) SaveSession(app);
            }
            if (loaded) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", f.path.c_str(), loaded ? "(loaded)" : f.terrain ? "Click: use as the terrain" : "Click: load on top of the loaded maps");
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string folder = f.folder;
            size_t cut = folder.find_last_of("/\\", folder.size() > 1 ? folder.size() - 2 : std::string::npos);
            ImGui::TextDisabled("%s", cut == std::string::npos ? folder.c_str() : folder.c_str() + cut + 1);
        }
        ImGui::EndTable();
    }
}

// The quests of the quest folders and language packs set in Settings: a click opens one.
static void QuestList(App& app) {
    RescanQuests(app);
    if (app.lib.questFolders.empty() && app.lib.questPacks.empty()) return;
    ImGui::SeparatorText("Quests");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##questfilter", "filter, e.g. z8q", app.questFilter, sizeof(app.questFilter));
    ImGui::SameLine();
    if (ImGui::Button("Rescan##quests")) app.questsFor.clear();
    const std::string filter = Lower(app.questFilter);
    if (ImGui::BeginTable("##quests", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 150))) {
        ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 110);
        for (size_t i = 0; i < app.quests.size(); ++i) {
            const quest::QuestSet& set = app.quests[i];
            const quest::Quest& q = set.shown;
            if (!filter.empty() && Lower(q.name + " " + q.terrain + " " + q.region).find(filter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            bool open = app.openQuest && app.openQuest->shown.path == q.path;
            std::string label = q.name + (set.languagePack ? "  (" + std::to_string(set.copies.size()) + " languages)" : "");
            if (ImGui::Selectable(label.c_str(), open, ImGuiSelectableFlags_SpanAllColumns)) {
                if (app.questDirty) app.questMessage = "Save or revert the changes of " + app.openQuest->shown.name + " first";
                else OpenQuest(app, set);
            }
            if (ImGui::IsItemHovered()) {
                std::string copies;
                for (size_t k = 0; k < set.copies.size(); ++k) copies += "\n  " + set.labels[k] + ": " + set.copies[k].path;
                ImGui::SetTooltip("%s, %zu exit(s)%s\nClick: open its terrain, base map and quest map", q.packed ? "packed .mq" : "unpacked",
                                  q.exits.size(), copies.c_str());
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", q.terrain.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s%s", q.region.c_str(), q.sky == "cave" ? " (cave)" : "");
        }
        ImGui::EndTable();
    }
}

// The area as numbers; when the user starts changing them, `before` goes on the undo stack.
static bool RectFields(App& app, const char* id, quest::Rect& r, const std::vector<quest::Exit>& before) {
    if (!r.set) return false;
    float v[4] = {r.x1, r.y1, r.x2, r.y2};
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::DragFloat4(id, v, 0.25f, 0.0f, 0.0f, "%.1f");
    if (ImGui::IsItemActivated()) PushAreaUndo(app, before);
    if (!changed) return false;
    r.x1 = v[0]; r.y1 = v[1]; r.x2 = v[2]; r.y2 = v[3];
    return true;
}

// The open quest: its exits, with their deploy and exit areas as numbers (drag or type; the view's
// handles do the same), and saving them to every copy.
static void QuestPanel(App& app) {
    if (!app.openQuest) return;
    quest::QuestSet& set = *app.openQuest;
    quest::Quest& q = set.shown;
    ImGui::SeparatorText(("Quest " + q.name + (app.questDirty ? " (changed)" : "")).c_str());
    ImGui::TextDisabled("%s%s, terrain %s, base map %s, lighting %s", q.region.c_str(), q.sky == "cave" ? " cave" : "", q.terrain.c_str(),
                        q.baseMap.c_str(), q.LightingFile().c_str());
    if (set.languagePack) {
        std::string langs;
        for (const std::string& l : set.labels) langs += (langs.empty() ? "" : ", ") + l;
        ImGui::TextDisabled("language packs: %s (area changes go to all of them)", langs.c_str());
    }
    const std::vector<quest::Exit> before = q.exits; // for the undo stack, when a field starts changing
    for (size_t i = 0; i < q.exits.size(); ++i) {
        quest::Exit& e = q.exits[i];
        ImGui::PushID(static_cast<int>(i));
        std::string label = "#" + std::to_string(e.number) + " " + (e.title.empty() ? std::string("exit") : e.title) + "  -> " + e.target;
        if (ImGui::Selectable(label.c_str())) FocusRect(app, e.deploy.set ? e.deploy : e.remove);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: go there");
        if (e.deploy.set) {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "deploy");
            ImGui::SameLine(70);
            if (RectFields(app, "##deploy", e.deploy, before)) app.questDirty = !SameAreas(q.exits, app.savedAreas);
        }
        if (e.remove.set) {
            ImGui::TextColored(ImVec4(0.98f, 0.5f, 0.42f, 1), "exit");
            ImGui::SameLine(70);
            if (RectFields(app, "##remove", e.remove, before)) app.questDirty = !SameAreas(q.exits, app.savedAreas);
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(!app.questDirty);
    if (ImGui::Button(set.copies.size() > 1 ? ("Save areas to all " + std::to_string(set.copies.size()) + " copies").c_str() : "Save areas"))
        SaveQuestAreas(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.undoSteps.empty());
    if (ImGui::Button("Undo")) UndoRedo(app, false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.redoSteps.empty());
    if (ImGui::Button("Redo")) UndoRedo(app, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", ui::BindName(app.lib.mapKeys[config::kKeyRedo]).c_str());
    ImGui::BeginDisabled(!app.questDirty);
    ImGui::SameLine();
    if (ImGui::Button("Revert")) { ReloadQuest(app, false); app.questMessage = "Reverted to the files on disk"; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Close quest")) {
        if (app.questDirty) app.questMessage = "Save or revert the changes first";
        else CloseQuest(app);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop showing its areas (the files stay loaded)");
    if (!app.questMessage.empty()) ImGui::TextWrapped("%s", app.questMessage.c_str());
    ImGui::TextDisabled("In the view: drag an area's corner, side or centre handle to resize or move it (Shift: no snapping). "
                        "Save with the button above, the toolbar's Save quest or %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
}

static void FilesTab(App& app) {
    QuestList(app);
    QuestPanel(app);
    FolderList(app);
    if (!app.missing.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1), "%zu object(s) whose figure cannot be shown", app.missing.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(app.missingOpen ? "Hide##missing" : "Show##missing")) app.missingOpen = !app.missingOpen;
        ImGui::SetItemTooltip("Their figure is not in the figure sources (Settings) or cannot be read: the game would not show them either\n"
                              "if it lacks them too. Marked with a red ? in the view.");
    }
    ImGui::SeparatorText("Terrain (.mpr)");
    if (app.terrainLoaded) {
        const mpr::Map& m = app.terrain;
        ui::StatusDot(true, app.terrainPath, "");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", app.terrainPath.c_str());
        ImGui::TextDisabled("%s: %d x %d sectors (%.0f x %.0f), max height %.1f, %d/%d textures found", m.name.c_str(), m.sectorsX,
                            m.sectorsY, m.Width(), m.Height(), m.maxZ, app.scene.TerrainTexturesFound(), m.textureCount);
        if (app.scene.TerrainTexturesFound() < m.textureCount)
            ui::Note("Terrain textures " + m.name + "000.mmp... are missing from the texture sources (Settings): add the textures.res that "
                     "has them (a mod's may be textures-zones.res).");
        for (const std::string& w : m.warnings) ui::Note(w);
        if (ImGui::SmallButton("Unload##mpr")) {
            UnloadTerrain(app);
            SaveSession(app);
        }
    } else {
        ImGui::TextDisabled("(none: objects stand at height 0)");
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mpr", "any other .mpr: its path", app.terrainInput, sizeof(app.terrainInput));
    std::string picked;
    if (ImGui::Button("File...##mpr") && ui::PickFile(picked)) std::snprintf(app.terrainInput, sizeof(app.terrainInput), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.terrainInput[0] == '\0');
    if (ImGui::Button("Load##mpr") && LoadTerrain(app, app.terrainInput)) {
        app.terrainInput[0] = '\0';
        app.filesMessage.clear();
        SaveSession(app);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Maps (.mob), in load order");
    ImGui::TextDisabled("Each map is loaded on top of the ones above it, like a quest on its zone.");
    int removeAt = -1, moveUp = -1;
    for (size_t i = 0; i < app.mobs.size(); ++i) {
        MobEntry& e = *app.mobs[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Checkbox("##vis", &e.visible)) SyncScene(app);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show its objects");
        ImGui::SameLine();
        if (ImGui::RadioButton("##active", app.activeMob == static_cast<int>(i))) SetActiveMob(app, static_cast<int>(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The active map: the only one whose objects can be selected (%s: next)",
                                                      ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
        ImGui::SameLine();
        ui::StatusDot(e.file.loaded, e.file.path, e.file.error);
        ImGui::SameLine();
        if (e.Dirty()) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "%s *", e.file.fileName.c_str());
        else ImGui::TextUnformatted(e.file.fileName.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", e.file.path.c_str(), e.Dirty() ? "\n(unsaved changes)" : "");
        ImGui::SameLine();
        float right = ImGui::GetContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - 88.0f));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up")) moveUp = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
        if (e.file.loaded) {
            int counts[8] = {};
            for (const mob::Object& o : e.file.objects) counts[static_cast<int>(o.kind)]++;
            ImGui::TextDisabled("   %d objects, %d units, %d levers, %d torches, %d traps, %d lights, %d particles, %d sounds%s",
                                counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
                                e.file.hasScript ? ", script" : "");
        }
        ImGui::PopID();
    }
    if (moveUp > 0) {
        std::swap(app.mobs[moveUp], app.mobs[moveUp - 1]);
        if (app.activeMob == moveUp) app.activeMob = moveUp - 1;
        else if (app.activeMob == moveUp - 1) app.activeMob = moveUp;
        SyncScene(app);
        SaveSession(app);
    }
    if (removeAt >= 0) {
        RemoveMob(app, removeAt);
        SaveSession(app);
    }
    if (app.mobs.empty()) ImGui::TextDisabled("(none yet)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mob", "any other .mob: its path", app.mobInput, sizeof(app.mobInput));
    if (ImGui::Button("File...##mob") && ui::PickFile(picked)) std::snprintf(app.mobInput, sizeof(app.mobInput), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.mobInput[0] == '\0');
    if (ImGui::Button("Add##mob") && AddMob(app, app.mobInput)) {
        app.mobInput[0] = '\0';
        SaveSession(app);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.mobs.empty() && !app.terrainLoaded);
    if (ImGui::Button("Reload all") && !BlockedByUnsaved(app)) {
        std::vector<std::string> paths;
        if (app.terrainLoaded) paths.push_back(app.terrainPath);
        for (auto& m : app.mobs) paths.push_back(m->file.path);
        app.mobs.clear();
        app.loadOrder.clear();
        app.terrainLoaded = false;
        app.scene.ClearSelection();
        OpenPaths(app, paths);
        SyncScene(app);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read every file again, after changing them elsewhere");

    QuestSuggestion q = SuggestForQuest(app);
    if (q.any) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s.", q.label.c_str());
        if (ImGui::Button(q.baseMob.empty() ? "Load its terrain" : q.terrain.empty() ? "Load its base map" : "Load its base map and terrain")) {
            if (!q.terrain.empty()) LoadTerrain(app, q.terrain);
            if (!q.baseMob.empty()) {
                AddMob(app, q.baseMob);
                std::rotate(app.mobs.begin(), app.mobs.end() - 1, app.mobs.end()); // the base goes first
                SyncScene(app);
            }
            SaveSession(app);
        }
    }
    if (!app.filesMessage.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", app.filesMessage.c_str());
    }
}

// ---- editing an object's parameters --------------------------------------------------------------
// The fields keep what is typed while they are being edited and commit when left (Enter, Tab or a
// click elsewhere). A commit is one undo step (the file as it was) and goes to every selected object
// that has the field when "apply to all" is on. Texts are stored in CP1251, the game's encoding.

static int ResizeCallbackStr(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = static_cast<std::string*>(d->UserData);
        str->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

static bool EditText(const char* id, const std::string& current, std::string& out) {
    static std::map<ImGuiID, std::string> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::string v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(id, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallbackStr, &v);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) { out = v; pending.erase(key); return out != current; }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

// The names the object fields complete from (rebuilt when the sources change).
struct NameLists {
    unsigned figuresFor = ~0u, texturesFor = ~0u, dbFor = ~0u;
    std::vector<std::string> figures, textures, prototypes, items, materials;
};
static NameLists& Names(const Library& lib) {
    static NameLists n;
    auto sorted = [](std::vector<std::string> v) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); return v; };
    if (n.figuresFor != static_cast<unsigned>(lib.figuresVersion)) {
        n.figuresFor = static_cast<unsigned>(lib.figuresVersion);
        std::vector<std::string> f;
        for (const std::string& b : lib.figures.ListBaseNames({".mod", ".lnk"})) f.push_back(Lower(b)); // whole figures, not parts
        n.figures = sorted(f);
    }
    if (n.texturesFor != static_cast<unsigned>(lib.texturesVersion)) {
        n.texturesFor = static_cast<unsigned>(lib.texturesVersion);
        std::vector<std::string> t;
        for (const std::string& b : lib.textures.ListBaseNames({".mmp", ".dds"})) t.push_back(Lower(b));
        n.textures = sorted(t);
    }
    if (n.dbFor != static_cast<unsigned>(lib.version)) {
        n.dbFor = static_cast<unsigned>(lib.version);
        n.prototypes.clear(); n.items.clear(); n.materials.clear();
        for (const units::Monster& m : lib.unitsDb.monsters) n.prototypes.push_back(m.name);
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c)
            for (const items::Item& it : lib.db.List(static_cast<items::Category>(c))) n.items.push_back(it.name);
        for (const items::Material& m : lib.db.materials) n.materials.push_back(m.name);
        n.prototypes = sorted(n.prototypes); n.items = sorted(n.items); n.materials = sorted(n.materials);
    }
    return n;
}

// EditText with suggestions while typing: names starting with the text first, then containing it. Up / Down
// pick one, Tab (or Enter on a picked one) or a click takes it; Escape or typing on ignores them. `prefix`: kept
// before the completed part (e.g. "axe." while completing the material of "axe.bro").
static bool EditTextSuggest(const char* id, const std::string& current, std::string& out, const std::vector<std::string>& names,
                            const std::string& prefix = "", float width = -1.0f) {
    static std::map<ImGuiID, std::string> pending;
    static ImGuiID openFor = 0;
    static int highlight = -1;
    static ImVec2 boxMin, boxMax; // last frame's suggestion box
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::string v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallbackStr, &v);
    const ImVec2 fieldMin = ImGui::GetItemRectMin(), fieldMax = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    // The suggestions for what is typed (after the prefix).
    std::vector<const std::string*> found;
    const std::string typed = v.size() >= prefix.size() ? Lower(v.substr(prefix.size())) : "";
    if ((openFor == key || active) && !typed.empty()) {
        for (int pass = 0; pass < 2 && found.size() < 12; ++pass)
            for (const std::string& n : names) {
                const std::string ln = Lower(n);
                const size_t at = ln.find(typed);
                if (at == std::string::npos || (pass == 0) != (at == 0) || ln == typed) continue;
                found.push_back(&n);
                if (found.size() >= 12) break;
            }
    }
    if (active) {
        pending[key] = v;
        if (openFor != key) { openFor = key; highlight = -1; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !found.empty()) highlight = std::min(highlight + 1, static_cast<int>(found.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) highlight = std::max(highlight - 1, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { openFor = 0; found.clear(); }
    }
    bool picked = false;
    std::string choice;
    if (openFor == key && !found.empty() && !typed.empty()) { // the box under the field
        ImGui::SetNextWindowPos(ImVec2(fieldMin.x, fieldMax.y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(fieldMax.x - fieldMin.x, 0), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("##suggest", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        for (int i = 0; i < static_cast<int>(found.size()); ++i)
            if (ImGui::Selectable(found[i]->c_str(), i == highlight)) { picked = true; choice = *found[i]; }
        boxMin = ImGui::GetWindowPos();
        boxMax = ImVec2(boxMin.x + ImGui::GetWindowWidth(), boxMin.y + ImGui::GetWindowHeight());
        ImGui::End();
    } else if (openFor == key) {
        boxMin = boxMax = ImVec2(0, 0);
    }
    if (picked) { openFor = 0; pending.erase(key); out = prefix + choice; return out != current; }
    // The box left open by a click that was released elsewhere: the typed text is taken.
    if (!active && openFor == key && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(boxMin, boxMax, false)) {
        openFor = 0; pending.erase(key); out = v; return out != current;
    }
    if (ImGui::IsItemDeactivated()) {
        // A click in the box: the box takes it (next frame), not the half-typed text.
        if (ImGui::IsMouseHoveringRect(boxMin, boxMax, false) && openFor == key) return false;
        const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false), enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        openFor = 0;
        if (!found.empty() && (tab || (enter && highlight >= 0))) v = prefix + *found[highlight >= 0 ? highlight : 0];
        pending.erase(key);
        out = v;
        return out != current;
    }
    if (!active && openFor != key) pending.erase(key);
    return false;
}

// A texture field: suggestions while typing, and a button opening the full list, each texture previewed on hover.
static bool EditTexture(App& app, const char* id, const std::string& current, std::string& out) {
    const std::vector<std::string>& names = Names(app.lib).textures;
    ImGui::PushID(id);
    const float button = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    bool changed = EditTextSuggest("##t", current, out, names, "", ImGui::GetContentRegionAvail().x - button - gap);
    ImGui::SameLine(0, gap);
    if (ImGui::ArrowButton("##list", ImGuiDir_Down)) ImGui::OpenPopup("##textures");
    ImGui::SetItemTooltip("Pick a texture (previewed when hovered)");
    if (ImGui::BeginPopup("##textures")) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##filter", "search", filter, sizeof filter);
        const std::string f = Lower(filter);
        ImGui::BeginChild("##list", ImVec2(300, 320));
        for (const std::string& n : names) {
            if (!f.empty() && n.find(f) == std::string::npos) continue;
            if (ImGui::Selectable(n.c_str(), Lower(current) == n)) { out = n; changed = out != current; ImGui::CloseCurrentPopup(); }
            if (ImGui::IsItemHovered()) {
                const GLuint tex = app.scene.TexturePreview(app.lib, n);
                if (tex && ImGui::BeginTooltip()) {
                    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(160, 160));
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

static bool EditFloats(const char* id, int n, const float* current, float* out, const char* format = "%.3f", float width = -1.0f) {
    static std::map<ImGuiID, std::array<float, 4>> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::array<float, 4> v{};
    for (int i = 0; i < n; ++i) v[i] = it != pending.end() ? it->second[i] : current[i];
    ImGui::SetNextItemWidth(width);
    ImGui::InputScalarN(id, ImGuiDataType_Float, v.data(), n, nullptr, nullptr, format);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        pending.erase(key);
        bool changed = false;
        for (int i = 0; i < n; ++i) { changed |= out[i] != v[i] || current[i] != v[i]; out[i] = v[i]; }
        return changed;
    }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

static bool EditU32(const char* id, uint32_t current, uint32_t& out) {
    static std::map<ImGuiID, uint32_t> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    uint32_t v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputScalar(id, ImGuiDataType_U32, &v);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) { pending.erase(key); out = v; return v != current; }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

static std::vector<uint8_t> F32Payload(const float* v, int n) {
    std::vector<uint8_t> p(static_cast<size_t>(n) * 4);
    std::memcpy(p.data(), v, p.size());
    return p;
}

// Writes one field into the shown object, or every selected object that has it. One undo step.
static void CommitField(App& app, int shown, uint32_t type, const std::vector<uint8_t>& payload, bool allowAll = true) {
    if (app.mobs.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::vector<int> targets;
    if (allowAll && app.editAll && app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1) targets = app.scene.selection;
    else targets = {shown};
    int done = 0;
    for (int oi : targets) done += mob::ReplaceField(f, oi, type, payload) ? 1 : 0;
    if (!done) { app.editMessage = "No selected object has that field"; return; }
    PushUndo(app, std::move(step), "Edit " + checks::Label(f.objects[shown]));
    app.checksDirty = true;
    app.listedKey.clear();
    app.editMessage = done > 1 ? "Changed " + std::to_string(done) + " objects" : std::string();
}

static void CommitText(App& app, int shown, uint32_t type, const std::string& utf8, bool allowAll = true) {
    std::vector<uint8_t> cp;
    std::string err;
    if (!codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err)) { app.editMessage = "Not saved: the text has " + err; return; }
    CommitField(app, shown, type, cp, allowAll);
}

static std::string ToCp(const std::string& utf8) {
    std::vector<uint8_t> cp;
    std::string err;
    codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err);
    return std::string(cp.begin(), cp.end());
}

static void Label(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableNextColumn();
}

// ---- unit stats (UNIT_STATS, ei_maper's SUnitStat) ----------------------------------------------------

struct StatField { const char* group; const char* name; int offset; char type; const char* tip; }; // type: i int, f float, b byte

static const StatField kStatFields[] = {
    {"Health and mana", "HP", 0, 'i', nullptr}, {"Health and mana", "Max HP", 4, 'i', nullptr},
    {"Health and mana", "MP", 8, 'i', nullptr}, {"Health and mana", "Max MP", 12, 'i', nullptr},
    {"Movement", "Move", 16, 'f', nullptr}, {"Movement", "Actions", 20, 'f', nullptr},
    {"Movement", "Run speed", 24, 'f', nullptr}, {"Movement", "Walk speed", 28, 'f', nullptr},
    {"Movement", "Crouch speed", 32, 'f', nullptr}, {"Movement", "Crawl speed", 36, 'f', nullptr},
    {"Vision", "Vision arc", 40, 'f', "radians"}, {"Vision", "Peripheral skill", 44, 'f', nullptr},
    {"Vision", "Peripheral arc", 48, 'f', "radians"},
    {"Combat", "Attack distance", 52, 'f', nullptr}, {"Combat", "AI class (standing)", 56, 'b', nullptr},
    {"Combat", "AI class (lying)", 57, 'b', nullptr}, {"Combat", "Range", 60, 'f', nullptr},
    {"Combat", "Attack", 64, 'f', nullptr}, {"Combat", "Defence", 68, 'f', nullptr}, {"Combat", "Weight", 72, 'f', nullptr},
    {"Combat", "Damage min", 76, 'f', nullptr}, {"Combat", "Damage range", 80, 'f', "added to the minimum at random"},
    {"Armour", "Impaling", 84, 'f', nullptr}, {"Armour", "Slashing", 88, 'f', nullptr}, {"Armour", "Crushing", 92, 'f', nullptr},
    {"Armour", "Thermal", 96, 'f', nullptr}, {"Armour", "Chemical", 100, 'f', nullptr}, {"Armour", "Electrical", 104, 'f', nullptr},
    {"Armour", "General", 108, 'f', nullptr}, {"Armour", "Absorption", 112, 'i', nullptr},
    {"Senses", "Sight", 116, 'f', nullptr}, {"Senses", "Night sight", 120, 'f', nullptr}, {"Senses", "Sense life", 124, 'f', nullptr},
    {"Senses", "Hearing", 128, 'f', nullptr}, {"Senses", "Smell", 132, 'f', nullptr}, {"Senses", "Tracking", 136, 'f', nullptr},
    {"Senses", "Sight (p)", 140, 'f', "the p values of ei_maper"}, {"Senses", "Night sight (p)", 144, 'f', nullptr},
    {"Senses", "Sense life (p)", 148, 'f', nullptr}, {"Senses", "Hearing (p)", 152, 'f', nullptr},
    {"Senses", "Smell (p)", 156, 'f', nullptr}, {"Senses", "Tracking (p)", 160, 'f', nullptr},
    {"Skills", "Science", 164, 'b', nullptr}, {"Skills", "Stealing", 165, 'b', nullptr}, {"Skills", "Taming", 166, 'b', nullptr},
    {"Skills", "Magic school 1", 167, 'b', nullptr}, {"Skills", "Magic school 2", 168, 'b', nullptr}, {"Skills", "Magic school 3", 169, 'b', nullptr},
};

// Writes one stat into the shown unit, or into every selected unit (each keeps its other stats). One undo step.
static void CommitStat(App& app, int shown, const StatField& sf, const uint8_t* value) {
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::vector<int> targets;
    if (app.editAll && app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1) targets = app.scene.selection;
    else targets = {shown};
    const size_t size = sf.type == 'b' ? 1 : 4;
    int done = 0;
    for (int oi : targets) {
        std::vector<uint8_t> stats = f.objects[oi].stats;
        if (f.objects[oi].kind != mob::Kind::Unit || stats.size() < 172) continue;
        std::memcpy(stats.data() + sf.offset, value, size);
        done += mob::ReplaceField(f, oi, mob::kUnitStats, stats) ? 1 : 0;
    }
    if (!done) return;
    PushUndo(app, std::move(step), std::string("Stat ") + sf.name);
    app.listedKey.clear();
    app.editMessage = done > 1 ? "Changed " + std::to_string(done) + " units" : std::string();
}

static void StatsEditor(App& app, const mob::Object& o, int objectIndex) {
    if (o.stats.size() < 172) return;
    if (!ImGui::CollapsingHeader("Stats")) return;
    if (o.needImport) ImGui::TextWrapped("\"Stats imported\" is on: the game takes this unit's stats from its prototype, not these.");
    const char* group = nullptr;
    bool open = false;
    for (const StatField& sf : kStatFields) {
        if (!group || std::strcmp(group, sf.group) != 0) {
            if (open) ImGui::EndTable();
            group = sf.group;
            ImGui::SeparatorText(group);
            open = ImGui::BeginTable(group, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg);
            if (open) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            }
        }
        if (!open) continue;
        Label(sf.name);
        if (sf.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sf.tip);
        ImGui::PushID(sf.offset);
        const uint8_t* at = o.stats.data() + sf.offset;
        if (sf.type == 'f') {
            float cur[1], v[1];
            std::memcpy(cur, at, 4);
            if (EditFloats("##v", 1, cur, v, "%.4g")) CommitStat(app, objectIndex, sf, reinterpret_cast<const uint8_t*>(v));
        } else {
            uint32_t cur = 0, v = 0;
            if (sf.type == 'i') std::memcpy(&cur, at, 4);
            else cur = at[0];
            if (EditU32("##v", cur, v)) {
                if (sf.type == 'b') { const uint8_t b = static_cast<uint8_t>(std::min<uint32_t>(v, 255)); CommitStat(app, objectIndex, sf, &b); }
                else CommitStat(app, objectIndex, sf, reinterpret_cast<const uint8_t*>(&v));
            }
        }
        ImGui::PopID();
    }
    if (open) ImGui::EndTable();
}

// Body parts: the figure's parts to show (OBJ_BODYPARTS); none ticked shows them all.
static void BodyPartsEditor(App& app, const mob::Object& o, int objectIndex) {
    const std::vector<std::string>* names = app.scene.PartNames(o.templ);
    if (!names && o.bodyParts.empty()) return;
    const std::string title = "Body parts (" + (o.bodyParts.empty() ? std::string("all") : std::to_string(o.bodyParts.size())) + ")###bodyparts";
    if (!ImGui::CollapsingHeader(title.c_str())) return;
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::vector<std::string> all = names ? *names : std::vector<std::string>{};
    for (const std::string& p : o.bodyParts) { // parts the figure lacks stay listed, so they can be removed
        bool found = false;
        for (const std::string& n : all) found |= lower(n) == lower(p);
        if (!found) all.push_back(p);
    }
    ImGui::TextDisabled("None ticked: the whole figure shows.");
    std::vector<std::string> now = o.bodyParts;
    bool changed = false;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 125.0f));
    if (ImGui::BeginTable("##parts", columns)) {
        for (size_t i = 0; i < all.size(); ++i) {
            ImGui::TableNextColumn();
            auto it = std::find_if(now.begin(), now.end(), [&](const std::string& p) { return lower(p) == lower(all[i]); });
            bool on = it != now.end();
            if (ImGui::Checkbox((all[i] + "##bp" + std::to_string(i)).c_str(), &on)) {
                if (on) now.push_back(all[i]);
                else now.erase(it);
                changed = true;
            }
        }
        ImGui::EndTable();
    }
    if (!now.empty() && ImGui::SmallButton("Show all (clear)")) { now.clear(); changed = true; }
    if (changed) CommitField(app, objectIndex, mob::kObjBodyParts, mob::StringArrayPayload(mob::kObjBodyParts, now), false);
}

// ---- logic (UNIT_LOGIC) -----------------------------------------------------------------------------

static void CommitLogic(App& app, int objectIndex, int logicIndex, const mob::Logic& g) {
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    if (!mob::SetLogic(f, objectIndex, logicIndex, g)) { app.editMessage = "This unit's logic could not be written"; return; }
    PushUndo(app, std::move(step), "Logic of " + checks::Label(f.objects[objectIndex]));
    app.checksDirty = true;
    app.listedKey.clear();
}

// The record shown for this unit: the one picked before, else the first in use.
static int LogicRecord(App& app, const mob::Object& o, int objectIndex) {
    if (app.logicRecordFor != objectIndex || app.logicRecord < 0 || app.logicRecord >= static_cast<int>(o.logics.size())) {
        app.logicRecordFor = objectIndex;
        app.logicRecord = 0;
        for (size_t i = 0; i < o.logics.size(); ++i) if (o.logics[i].use) { app.logicRecord = static_cast<int>(i); break; }
    }
    return app.logicRecord;
}

static bool EditByte(const char* id, int current, int& out) {
    uint32_t v = 0;
    if (!EditU32(id, static_cast<uint32_t>(std::max(current, 0)), v)) return false;
    out = static_cast<int>(std::min<uint32_t>(v, 255));
    return true;
}

static void LogicEditor(App& app, const mob::Object& o, int objectIndex) {
    if (o.logics.empty()) return;
    const int gi = LogicRecord(app, o, objectIndex);
    mob::Logic g = o.logics[gi];
    bool changed = false;
    ImGui::SeparatorText("Logic");
    if (o.logics.size() > 1) {
        std::string preview = "Record " + std::to_string(gi + 1) + (g.use ? " (in use)" : "");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##record", preview.c_str())) {
            for (size_t i = 0; i < o.logics.size(); ++i) {
                std::string label = "Record " + std::to_string(i + 1) + ": " + mob::LogicModelName(o.logics[i].model) + (o.logics[i].use ? " (in use)" : "");
                if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == gi)) app.logicRecord = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A unit carries several logic records; the ones in use apply");
    }
    float v4[4];
    if (ImGui::BeginTable("##logic", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        Label("In use");
        changed |= ImGui::Checkbox("##use", &g.use);
        Label("Behaviour");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##model", mob::LogicModelName(g.model))) {
            for (int m = 0; m <= 5; ++m)
                if (ImGui::Selectable(mob::LogicModelName(m), m == g.model)) { g.model = m; changed = true; }
            ImGui::EndCombo();
        }
        if (g.model == 1) {
            Label("Guard radius");
            const float cur[1] = {g.guardRadius};
            if (EditFloats("##gradius", 1, cur, v4, "%.2f")) { g.guardRadius = v4[0]; changed = true; }
        }
        if (g.model == 1 || g.model == 3) {
            Label(g.model == 1 ? "Guard place" : "Sentry place");
            const float cur[3] = {g.guardPlace.x, g.guardPlace.y, g.guardPlace.z};
            if (EditFloats("##gplace", 3, cur, v4, "%.3f", -50)) { g.guardPlace = {v4[0], v4[1], v4[2]}; changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Here")) { g.guardPlace = {o.position.x, o.position.y, 0.0f}; changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The unit's own position (or drag the point in logic mode)");
        }
        if (g.model == 2) {
            Label("Cyclic");
            changed |= ImGui::Checkbox("##cyclic", &g.cyclic);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the first point after the last");
        }
        Label("Calls help within");
        {
            const float cur[1] = {g.help};
            if (EditFloats("##help", 1, cur, v4, "%.1f")) { g.help = v4[0]; changed = true; }
        }
        Label("Wait (s)");
        {
            const float cur[1] = {g.wait < 0 ? -1.0f : g.wait / 15.0f};
            if (EditFloats("##wait", 1, cur, v4, "%.2f")) { g.wait = v4[0] < 0 ? -1.0f : v4[0] * 15.0f; changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stored as 15 per second; -1: none");
        }
        Label("Alarm condition");
        changed |= EditByte("##alarmcond", g.alarmCondition, g.alarmCondition);
        Label("Alarm count");
        changed |= EditByte("##alarmcount", g.alarmCount, g.alarmCount);
        Label("Aggression");
        changed |= EditByte("##aggr", g.aggression, g.aggression);
        Label("Always active");
        {
            bool on = g.alwaysActive != 0;
            if (ImGui::Checkbox("##always", &on)) { g.alwaysActive = on ? 1 : 0; changed = true; }
        }
        ImGui::EndTable();
    }
    // Patrol points, each with its look points (where the unit turns to look, how long, how fast).
    if (g.model == 2 || !g.patrol.empty()) {
        ImGui::TextDisabled("Patrol points (logic mode: drag them; Ctrl+click the ground adds one)");
        int removePoint = -1;
        for (size_t i = 0; i < g.patrol.size(); ++i) {
            mob::PatrolPoint& pt = g.patrol[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu", i + 1);
            ImGui::SameLine(30);
            const float cur[3] = {pt.position.x, pt.position.y, pt.position.z};
            if (EditFloats("##pp", 3, cur, v4, "%.3f", -80)) { pt.position = {v4[0], v4[1], v4[2]}; changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ look")) {
                pt.looks.push_back({{pt.position.x + 2.0f, pt.position.y, pt.position.z}, 15, 0.0f, 0});
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removePoint = static_cast<int>(i);
            int removeLook = -1;
            for (size_t k = 0; k < pt.looks.size(); ++k) {
                mob::LookPoint& l = pt.looks[k];
                ImGui::PushID(static_cast<int>(k));
                ImGui::Indent(30);
                ImGui::TextDisabled("look %zu", k + 1);
                ImGui::SameLine(110);
                const float lc[3] = {l.position.x, l.position.y, l.position.z};
                if (EditFloats("##lp", 3, lc, v4, "%.3f", -30)) { l.position = {v4[0], v4[1], v4[2]}; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeLook = static_cast<int>(k);
                ImGui::TextDisabled("wait s, turn speed");
                ImGui::SameLine(160);
                {
                    const float wc[2] = {l.wait / 15.0f, l.turnSpeed};
                    ImGui::PushItemWidth(-1);
                    if (EditFloats("##wt", 2, wc, v4, "%.2f")) { l.wait = static_cast<uint32_t>(std::max(0.0f, v4[0]) * 15.0f + 0.5f); l.turnSpeed = v4[1]; changed = true; }
                    ImGui::PopItemWidth();
                }
                ImGui::Unindent(30);
                ImGui::PopID();
            }
            if (removeLook >= 0) { pt.looks.erase(pt.looks.begin() + removeLook); changed = true; }
            ImGui::PopID();
        }
        if (removePoint >= 0) { g.patrol.erase(g.patrol.begin() + removePoint); changed = true; }
        if (ImGui::SmallButton("+ point")) {
            mob::PatrolPoint pt;
            const mob::Vec3 from = g.patrol.empty() ? o.position : g.patrol.back().position;
            pt.position = {from.x + 3.0f, from.y, app.scene.Ground(from.x + 3.0f, from.y)};
            g.patrol.push_back(pt);
            changed = true;
        }
    }
    if (changed) CommitLogic(app, objectIndex, gi, g);
}

// ---- the other fields of each kind (as ei_maper shows them) ---------------------------------------------

// A field's payload as stored in the object (empty when it has none).
static std::vector<uint8_t> FieldBytes(const mob::File& f, int objectIndex, uint32_t type) {
    const size_t at = mob::FindField(f, objectIndex, type);
    if (!at) return {};
    const uint32_t l = mob::U32(f.bytes.data() + at + 4);
    return std::vector<uint8_t>(f.bytes.begin() + at + 8, f.bytes.begin() + at + l);
}

// One row per field the object has: a box for flags, a number, a float, a point or a text.
// kind: 'b' flag byte, 'B' number byte, 'u' dword, 'f' float, 'p' three floats, 's' text.
static void FieldRow(App& app, int objectIndex, const char* label, uint32_t type, char kind, const char* tip = nullptr) {
    const mob::File& f = app.mobs[app.activeMob]->file;
    const std::vector<uint8_t> v = FieldBytes(f, objectIndex, type);
    const size_t need = kind == 'b' || kind == 'B' ? 1 : kind == 'u' || kind == 'f' ? 4 : kind == 'p' ? 12 : 0;
    if ((need && v.size() != need) || (kind == 's' && mob::FindField(f, objectIndex, type) == 0)) return;
    Label(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PushID(static_cast<int>(type));
    float fv[4];
    uint32_t u = 0;
    std::string text;
    switch (kind) {
    case 'b': {
        bool on = v[0] != 0;
        if (ImGui::Checkbox("##v", &on)) CommitField(app, objectIndex, type, std::vector<uint8_t>{static_cast<uint8_t>(on)});
        break;
    }
    case 'B':
        if (EditU32("##v", v[0], u)) CommitField(app, objectIndex, type, std::vector<uint8_t>{static_cast<uint8_t>(std::min<uint32_t>(u, 255))});
        break;
    case 'u':
        if (EditU32("##v", mob::U32(v.data()), u)) CommitField(app, objectIndex, type, mob::U32Payload(u));
        break;
    case 'f': {
        float cur[1];
        std::memcpy(cur, v.data(), 4);
        if (EditFloats("##v", 1, cur, fv, "%.3f")) CommitField(app, objectIndex, type, F32Payload(fv, 1));
        break;
    }
    case 'p': {
        float cur[3];
        std::memcpy(cur, v.data(), 12);
        if (EditFloats("##v", 3, cur, fv)) CommitField(app, objectIndex, type, F32Payload(fv, 3));
        break;
    }
    default:
        if (EditText("##v", mob::Utf8(std::string(v.begin(), v.end())), text)) CommitText(app, objectIndex, type, text);
        break;
    }
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
}

enum : uint32_t {
    kObjIsShadow = 45076,
    kLeverCurState = 3148611586u, kLeverTotalState = 3148611587u, kLeverIsCycled = 3148611588u, kLeverCastOnce = 3148611589u,
    kLeverStats = 3148611590u, kLeverIsDoor = 3148611591u, kLeverRecalcGraph = 3148611592u,
    kTrapDiplomacy = 3148546049u, kTrapAreas = 3148546051u, kTrapTargets = 3148546052u, kTrapCastInterval = 3148546053u,
    kTorchStrength = 3149856769u, kTorchPoint = 3149856770u, kTorchSound = 3149856771u,
    kLightShadow = 43526,
    kSoundMin = 52230, kSoundMax = 52231, kSoundRange2 = 52235, kSoundAmbient = 52237, kSoundMusic = 52238,
};

// The rows of the details table for what each kind stores beyond the common fields.
static void KindFieldRows(App& app, const mob::Object& o, int objectIndex) {
    if (mob::HasFigure(o.kind)) {
        FieldRow(app, objectIndex, "Parent ID", mob::kObjParentId, 'u', "The object this one belongs to (0: none)");
        FieldRow(app, objectIndex, "Used in script", mob::kObjUseInScript, 'b');
        FieldRow(app, objectIndex, "Shadow", kObjIsShadow, 'b');
    }
    const mob::File& f = app.mobs[app.activeMob]->file;
    switch (o.kind) {
    case mob::Kind::Lever: {
        FieldRow(app, objectIndex, "State", kLeverCurState, 'B');
        FieldRow(app, objectIndex, "Number of states", kLeverTotalState, 'B');
        FieldRow(app, objectIndex, "Cycled", kLeverIsCycled, 'b');
        FieldRow(app, objectIndex, "Door", kLeverIsDoor, 'b');
        FieldRow(app, objectIndex, "Recalculate graph", kLeverRecalcGraph, 'b', "Paths are worked out again when it changes (doors, gates)");
        // LEVER_SCIENCE_STATS_NEW: how it opens, the key, the sleight of hand needed
        std::vector<uint8_t> st = FieldBytes(f, objectIndex, kLeverStats);
        if (st.size() == 12) {
            uint32_t v[3];
            std::memcpy(v, st.data(), 12);
            static const uint32_t types[] = {0, 1, 5, 8};
            static const char* const names[] = {"0: disabled", "1: enabled", "5: needs sleight of hand", "8: needs a key"};
            int cur = -1;
            for (int i = 0; i < 4; ++i) if (types[i] == v[0]) cur = i;
            Label("Opening");
            ImGui::SetNextItemWidth(-1);
            const std::string preview = cur >= 0 ? names[cur] : std::to_string(v[0]);
            if (ImGui::BeginCombo("##open", preview.c_str())) {
                for (int i = 0; i < 4; ++i)
                    if (ImGui::Selectable(names[i], i == cur)) { v[0] = types[i]; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
                ImGui::EndCombo();
            }
            uint32_t u = 0;
            Label("Key ID");
            if (EditU32("##key", v[1], u)) { v[1] = u; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
            Label("Sleight of hand");
            if (EditU32("##sleight", v[2], u)) { v[2] = u; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
        }
        break;
    }
    case mob::Kind::MagicTrap:
        FieldRow(app, objectIndex, "Diplomacy group", kTrapDiplomacy, 'u', "The player group the trap belongs to");
        FieldRow(app, objectIndex, "Cast interval", kTrapCastInterval, 'u');
        FieldRow(app, objectIndex, "Cast once", kLeverCastOnce, 'b');
        break;
    case mob::Kind::Torch:
        FieldRow(app, objectIndex, "Strength", kTorchStrength, 'f');
        FieldRow(app, objectIndex, "Point link", kTorchPoint, 'p', "Where the flame is (TORCH_PTLINK)");
        FieldRow(app, objectIndex, "Sound", kTorchSound, 's');
        break;
    case mob::Kind::Light:
        FieldRow(app, objectIndex, "Shadow", kLightShadow, 'b');
        break;
    case mob::Kind::Particle:
        FieldRow(app, objectIndex, "Type", mob::kParticleType, 'u');
        break;
    case mob::Kind::Sound:
        FieldRow(app, objectIndex, "Range 2", kSoundRange2, 'u');
        FieldRow(app, objectIndex, "Min distance", kSoundMin, 'u');
        FieldRow(app, objectIndex, "Max distance", kSoundMax, 'u');
        FieldRow(app, objectIndex, "Ambient", kSoundAmbient, 'b');
        FieldRow(app, objectIndex, "Music", kSoundMusic, 'b');
        break;
    default: break;
    }
}

// A trap's activation areas (x, y, radius) and cast points (x, y), and a sound's files: editable lists.
static void KindLists(App& app, const mob::Object& o, int objectIndex) {
    const mob::File& f = app.mobs[app.activeMob]->file;
    float v4[4];
    if (o.kind == mob::Kind::MagicTrap) {
        for (int which = 0; which < 2; ++which) {
            const uint32_t type = which == 0 ? kTrapAreas : kTrapTargets;
            const int per = which == 0 ? 3 : 2;
            std::vector<uint8_t> b = FieldBytes(f, objectIndex, type);
            if (b.size() < 4) continue;
            const uint32_t n = mob::U32(b.data());
            if (b.size() != 4 + static_cast<size_t>(n) * per * 4) continue;
            std::vector<float> vals(static_cast<size_t>(n) * per);
            std::memcpy(vals.data(), b.data() + 4, vals.size() * 4);
            ImGui::PushID(static_cast<int>(type));
            ImGui::SeparatorText(which == 0 ? "Activation areas (x, y, radius)" : "Cast points (x, y)");
            bool changed = false;
            int remove = -1;
            for (uint32_t i = 0; i < n; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (EditFloats("##e", per, vals.data() + i * per, v4, "%.2f", -30)) { std::memcpy(vals.data() + i * per, v4, per * 4); changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            if (remove >= 0) { vals.erase(vals.begin() + remove * per, vals.begin() + (remove + 1) * per); changed = true; }
            if (ImGui::SmallButton("Add")) {
                vals.push_back(o.position.x);
                vals.push_back(o.position.y);
                if (per == 3) vals.push_back(5.0f);
                changed = true;
            }
            if (changed) {
                std::vector<uint8_t> out = mob::U32Payload(static_cast<uint32_t>(vals.size() / per));
                out.resize(4 + vals.size() * 4);
                std::memcpy(out.data() + 4, vals.data(), vals.size() * 4);
                CommitField(app, objectIndex, type, out, false);
            }
            ImGui::PopID();
        }
    }
    if (o.kind == mob::Kind::Sound && mob::FindField(f, objectIndex, mob::kSoundResName)) {
        ImGui::SeparatorText("Sound files");
        std::vector<std::string> entries = o.soundFiles;
        bool changed = false;
        int remove = -1;
        for (size_t i = 0; i < entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(-40);
            std::string v;
            if (EditText("##file", mob::Utf8(entries[i]), v)) { entries[i] = ToCp(v); changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) { entries.erase(entries.begin() + remove); changed = true; }
        if (ImGui::SmallButton("Add##sound")) { entries.push_back("nature\\sound.wav"); changed = true; }
        if (changed) CommitField(app, objectIndex, mob::kSoundResName, mob::StringArrayPayload(mob::kSoundResName, entries), false);
    }
}

static void ObjectDetails(App& app, const mob::File& fileIn, int fileIndex, const mob::Object& oIn, int objectIndex) {
    (void)fileIn; (void)oIn;
    // The object is read afresh after each commit (a commit re-reads the file).
    auto obj = [&]() -> const mob::Object& { return app.mobs[app.activeMob]->file.objects[objectIndex]; };
    const mob::File& f = app.mobs[app.activeMob]->file;
    const mob::Object o = obj(); // a copy: the file may change under it this frame
    const bool multi = app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1;
    ImGui::SeparatorText("Selected");
    ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "%s", checks::Label(o).c_str());
    if (multi) {
        ImGui::Checkbox(("Apply edits to all " + std::to_string(app.scene.selection.size()) + " selected").c_str(), &app.editAll);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: only this object. Objects without the field are left alone. The ID is never shared.");
    }
    const bool figure = mob::HasFigure(o.kind);
    std::string text;
    float v4[4];
    uint32_t u;
    if (ImGui::BeginTable("##obj", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Map");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(f.fileName.c_str());

        // Name (lights, sounds and particles have their own name fields)
        const uint32_t nameType = o.kind == mob::Kind::Light ? mob::kLightName : o.kind == mob::Kind::Sound ? mob::kSoundName
                                : o.kind == mob::Kind::Particle ? mob::kParticleName : mob::kObjName;
        Label(o.kind == mob::Kind::Particle ? "Effect" : "Name");
        if (EditText("##name", mob::Utf8(o.kind == mob::Kind::Particle ? o.particleName : o.name), text)) CommitText(app, objectIndex, nameType, text);
        // ID: one object only (IDs must not repeat)
        if (o.hasId) {
            const uint32_t idType = o.kind == mob::Kind::Light ? mob::kLightId : o.kind == mob::Kind::Sound ? mob::kSoundId
                                  : o.kind == mob::Kind::Particle ? mob::kParticleId : mob::kNid;
            Label("ID");
            if (EditU32("##id", o.id, u)) CommitField(app, objectIndex, idType, mob::U32Payload(u), false);
        }
        if (figure) {
            Label("Figure");
            if (EditTextSuggest("##templ", o.templ, text, Names(app.lib).figures)) CommitText(app, objectIndex, mob::kObjTemplate, text);
            Label("Texture");
            if (EditTexture(app, "##tex", o.primTexture, text)) CommitText(app, objectIndex, mob::kObjPrimTexture, text);
            Label("2nd texture");
            if (EditTexture(app, "##tex2", o.secTexture, text)) CommitText(app, objectIndex, mob::kObjSecTexture, text);
            Label("Parent template");
            if (EditTextSuggest("##parent", mob::Utf8(o.parentTemplate), text, Names(app.lib).prototypes)) CommitText(app, objectIndex, mob::kParentTemplate, text);
        }
        // Position
        const uint32_t posType = o.kind == mob::Kind::Light ? mob::kLightPosition : o.kind == mob::Kind::Sound ? mob::kSoundPosition
                               : o.kind == mob::Kind::Particle ? mob::kParticlePosition : mob::kObjPosition;
        Label(figure ? "Position (z: above ground)" : "Position");
        {
            const float cur[3] = {o.position.x, o.position.y, o.position.z};
            if (EditFloats("##pos", 3, cur, v4)) CommitField(app, objectIndex, posType, F32Payload(v4, 3));
        }
        if (figure) {
            Label("Turn about Z (deg)");
            {
                const float yaw = 2.0f * std::atan2(o.rotation[3], o.rotation[0]) * 180.0f / 3.14159265f;
                const float cur[1] = {yaw};
                if (EditFloats("##yaw", 1, cur, v4, "%.1f")) {
                    const float h = v4[0] * 3.14159265f / 360.0f;
                    const float q[4] = {std::cos(h), 0.0f, 0.0f, std::sin(h)};
                    CommitField(app, objectIndex, mob::kObjRotation, F32Payload(q, 4));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets a turn about the vertical only (any tilt goes); the quaternion below keeps it");
            }
            Label("Rotation w x y z");
            if (EditFloats("##rot", 4, o.rotation, v4, "%.4f")) {
                float len = std::sqrt(v4[0] * v4[0] + v4[1] * v4[1] + v4[2] * v4[2] + v4[3] * v4[3]);
                if (len > 1e-6f) for (float& c : v4) c /= len; // kept a unit quaternion
                CommitField(app, objectIndex, mob::kObjRotation, F32Payload(v4, 4));
            }
            Label("Complection");
            {
                const float cur[3] = {o.complection.x, o.complection.y, o.complection.z};
                if (EditFloats("##comp", 3, cur, v4)) CommitField(app, objectIndex, mob::kObjComplection, F32Payload(v4, 3));
            }
            Label("Group (diplomacy)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The player group it belongs to (ei_maper's \"Player\"): one of the 32 groups of the\n"
                                  "Diplomacy tab, which says who is friend, neutral or enemy to whom");
            if (EditU32("##player", static_cast<uint32_t>(std::max(o.player, 0)), u))
                CommitField(app, objectIndex, mob::kObjPlayer, std::vector<uint8_t>{static_cast<uint8_t>(std::min<uint32_t>(u, 255))});
            Label("Engine type");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How the engine handles the object (ei_maper's \"Type\"): 50-52 units, 53 plants and scenery, 54 structures, 55 other objects, 58 torches, 59 magic traps, 60 levers, 8193+ particles. Change it only to fix a wrong one.");
            if (EditU32("##type", static_cast<uint32_t>(std::max(o.type, 0)), u)) CommitField(app, objectIndex, mob::kObjType, mob::U32Payload(u));
        }
        if (o.kind == mob::Kind::Unit) {
            Label("Prototype");
            if (EditTextSuggest("##proto", mob::Utf8(o.prototype), text, Names(app.lib).prototypes)) CommitText(app, objectIndex, mob::kUnitPrototype, text);
            Label("Stats imported");
            bool imported = o.needImport;
            if (ImGui::Checkbox("##needimport", &imported)) CommitField(app, objectIndex, mob::kUnitNeedImport, std::vector<uint8_t>{static_cast<uint8_t>(imported)});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: the game takes the stats from the prototype (the Stats below are ignored)");
        }
        if (o.kind == mob::Kind::MagicTrap) {
            Label("Spell");
            if (EditText("##spell", mob::Utf8(o.spell), text)) CommitText(app, objectIndex, mob::kMagicTrapSpell, text);
        }
        if (o.kind == mob::Kind::Light) {
            Label("Colour");
            {
                const float cur[3] = {o.color.x, o.color.y, o.color.z};
                if (EditFloats("##colour", 3, cur, v4)) CommitField(app, objectIndex, mob::kLightColor, F32Payload(v4, 3));
            }
            Label("Range");
            {
                const float cur[1] = {o.range};
                if (EditFloats("##range", 1, cur, v4, "%.2f")) CommitField(app, objectIndex, mob::kLightRange, F32Payload(v4, 1));
            }
        }
        if (o.kind == mob::Kind::Particle) {
            Label("Scale");
            const float cur[1] = {o.scale};
            if (EditFloats("##scale", 1, cur, v4, "%.2f")) CommitField(app, objectIndex, mob::kParticleScale, F32Payload(v4, 1));
        }
        if (o.kind == mob::Kind::Sound) {
            Label("Range");
            if (EditU32("##srange", static_cast<uint32_t>(o.range), u)) CommitField(app, objectIndex, mob::kSoundRange, mob::U32Payload(u));
        }
        if (figure) {
            Label("Comments");
            if (EditText("##comments", mob::Utf8(o.comments), text)) CommitText(app, objectIndex, mob::kObjComments, text);
            Label("Quest info");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ei_maper's \"Quest\": the quest (e.g. z3xq1) or quest objective this object belongs to;\n"
                    "the game marks it on the map while it is active.");
            if (EditText("##quest", mob::Utf8(o.questInfo), text)) CommitText(app, objectIndex, mob::kObjQuestInfo, text);
        }
        KindFieldRows(app, o, objectIndex);
        const MapModel* model = app.scene.ModelFor(o);
        if (figure && model && !model->ok) { Label("Figure problem"); ImGui::TextWrapped("%s", model->error.c_str()); }
        if (model && !model->dressed.empty()) {
            Label("Shown");
            ImGui::TextWrapped("%s", model->dressed.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Units on the default0 placeholder are dressed from the database like the game does: skin, hair and equipment (the map's own lists first).");
        }
        ImGui::EndTable();
    }
    KindLists(app, o, objectIndex);
    if (figure) BodyPartsEditor(app, o, objectIndex);
    if (o.kind == mob::Kind::Unit) StatsEditor(app, o, objectIndex);
    // A unit's item lists: each entry editable, X removes it, + adds one. They go to this unit only.
    if (o.kind == mob::Kind::Unit) {
        for (const mob::ItemList& l : o.lists) {
            ImGui::PushID(static_cast<int>(l.type));
            ImGui::SeparatorText((std::string(l.label) + "s").c_str());
            std::vector<std::string> entries = l.entries;
            bool changed = false;
            int removeAt = -1;
            for (size_t i = 0; i < entries.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(-40);
                std::string v;
                // item, then ".material": the part after the last dot completes from the materials
                const std::string cur = mob::Utf8(entries[i]);
                const size_t dot = cur.rfind('.');
                const bool spells = l.type == mob::kUnitSpells;
                const bool material = !spells && dot != std::string::npos;
                if (EditTextSuggest("##entry", cur, v, material ? Names(app.lib).materials : spells ? std::vector<std::string>() : Names(app.lib).items,
                                    material ? cur.substr(0, dot + 1) : std::string()))
                    { entries[i] = ToCp(v); changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeAt >= 0) { entries.erase(entries.begin() + removeAt); changed = true; }
            if (ImGui::SmallButton("Add")) { entries.push_back(l.type == mob::kUnitSpells ? "spell" : "item"); changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds an entry to type over (template.material for weapons and armors)");
            if (changed) CommitField(app, objectIndex, l.type, mob::StringArrayPayload(l.type, entries), false);
            ImGui::PopID();
        }
        LogicEditor(app, o, objectIndex);
    }
    if (!app.editMessage.empty()) ImGui::TextDisabled("%s", app.editMessage.c_str());
    for (const checks::Finding& fd : app.findings) {
        if (fd.file != fileIndex || fd.object != objectIndex) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(fd.severity));
        ImGui::TextWrapped("%s", fd.message.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Focus")) app.scene.FocusOn(obj());
}

static const char* KindPlural(mob::Kind k) {
    switch (k) {
    case mob::Kind::Object: return "World objects";
    case mob::Kind::Unit: return "Units";
    case mob::Kind::Lever: return "Levers";
    case mob::Kind::Torch: return "Torches";
    case mob::Kind::MagicTrap: return "Magic traps";
    case mob::Kind::Light: return "Lights";
    case mob::Kind::Particle: return "Particles";
    case mob::Kind::Sound: return "Sounds";
    }
    return "?";
}

// What an object is grouped under: a unit's prototype, else its parent template, figure or name.
static std::string GroupLabel(const mob::Object& o) {
    for (const std::string* s : {&o.prototype, &o.parentTemplate, &o.templ, &o.particleName, &o.name})
        if (!s->empty() && Lower(*s) != "none") return mob::Utf8(*s);
    return "(unnamed)";
}

static std::string ObjectLabel(const mob::Object& o) {
    const std::string& n = o.name.empty() ? o.particleName : o.name;
    return n.empty() ? "(unnamed)" : mob::Utf8(n);
}

static void BuildTree(App& app) {
    app.tree.clear();
    if (app.mobs.empty()) return;
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    const std::string f = Lower(app.filter);
    std::map<mob::Kind, std::map<std::string, std::vector<int>>> byKind;
    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const mob::Object& o = objects[oi];
        if (!f.empty()) {
            std::string hay = Lower(o.name + " " + o.templ + " " + o.primTexture + " " + o.prototype + " " + o.parentTemplate + " " +
                                    o.particleName) + (o.hasId ? " " + std::to_string(o.id) : "");
            if (hay.find(f) == std::string::npos) continue;
        }
        byKind[o.kind][GroupLabel(o)].push_back(static_cast<int>(oi));
    }
    for (auto& kv : byKind) {
        ObjectCategory c;
        c.kind = kv.first;
        for (auto& g : kv.second) {
            c.count += static_cast<int>(g.second.size());
            c.groups.push_back({g.first, std::move(g.second)});
        }
        app.tree.push_back(std::move(c));
    }
}

static void DeleteSelection(App& app);
static std::filesystem::path ClipboardFile();
static void CopySelection(App& app);
static void Paste(App& app);
static void Duplicate(App& app);

static void ObjectsTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Active map");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##activemob", app.mobs[app.activeMob]->file.fileName.c_str())) {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (ImGui::Selectable(app.mobs[i]->file.fileName.c_str(), static_cast<int>(i) == app.activeMob)) SetActiveMob(app, static_cast<int>(i));
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the active map's objects are listed and selectable (%s: next map)",
                                                  ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter: name, ID, prototype, figure or texture", app.filter, sizeof(app.filter));
    { // editing the objects: the same as the keys
        const bool sel = app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
        if (ImGui::SmallButton("Add...")) { app.newOpen = true; app.newMessage.clear(); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        if (ImGui::SmallButton("Duplicate")) Duplicate(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) CopySelection(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        std::error_code ec;
        ImGui::BeginDisabled(app.clipboard.empty() && !std::filesystem::exists(ClipboardFile(), ec));
        if (ImGui::SmallButton("Paste")) Paste(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        if (ImGui::SmallButton("Delete")) DeleteSelection(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Keys: %s, %s, %s, %s", ui::BindName(app.lib.mapKeys[config::kKeyDelete]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyCopy]).c_str(),
                              ui::BindName(app.lib.mapKeys[config::kKeyPaste]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyDuplicate]).c_str());
    }

    std::string key = std::string(app.filter) + "|" + std::to_string(app.activeMob) + "|" + app.mobs[app.activeMob]->file.path;
    if (key != app.listedKey) {
        app.listedKey = key;
        BuildTree(app);
    }
    const bool filtering = app.filter[0] != '\0';
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    const bool hasSelection = app.scene.selectedFile == app.activeMob && app.scene.selectedObject >= 0 &&
                              app.scene.selectedObject < static_cast<int>(objects.size());
    float listHeight = hasSelection ? ImGui::GetContentRegionAvail().y * 0.5f : 0.0f;
    if (ImGui::BeginTable("##objects", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Count / ID", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        const mob::Object* sel = hasSelection ? &objects[app.scene.selectedObject] : nullptr;
        for (const ObjectCategory& c : app.tree) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool revealHere = app.revealSelection && sel && sel->kind == c.kind;
            if (filtering || revealHere) ImGui::SetNextItemOpen(true, filtering ? ImGuiCond_Appearing : ImGuiCond_Always);
            bool open = ImGui::TreeNodeEx(KindPlural(c.kind), ImGuiTreeNodeFlags_SpanAllColumns);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d", c.count);
            if (!open) continue;
            for (const ObjectGroup& g : c.groups) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool groupHasSel = revealHere && std::find(g.objects.begin(), g.objects.end(), app.scene.selectedObject) != g.objects.end();
                if (groupHasSel) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                bool gopen = ImGui::TreeNodeEx(g.label.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu", g.objects.size());
                if (!gopen) continue;
                for (int oi : g.objects) {
                    const mob::Object& o = objects[oi];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(oi);
                    bool selected = app.scene.IsSelected(app.activeMob, oi);
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns |
                                               (selected ? ImGuiTreeNodeFlags_Selected : 0);
                    ImGui::TreeNodeEx("##o", flags, "%s", ObjectLabel(o).c_str());
                    if (ImGui::IsItemClicked()) {
                        if (ImGui::GetIO().KeyShift) app.scene.Toggle(app.activeMob, oi);
                        else SelectObject(app, app.activeMob, oi, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %s\n(double-click: go to it; Shift+click: add or remove)", mob::KindName(o.kind), o.templ.c_str());
                    if (selected && app.revealSelection) { ImGui::SetScrollHereY(0.3f); app.revealSelection = false; }
                    ImGui::TableNextColumn();
                    if (o.hasId) ImGui::TextDisabled("%u", o.id);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
    if (hasSelection) {
        ImGui::BeginChild("##details");
        if (app.scene.selection.size() > 1) {
            ImGui::TextDisabled("%zu objects selected; details of the last one picked", app.scene.selection.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) app.scene.ClearSelection();
        }
        const mob::File& f = app.mobs[app.activeMob]->file;
        if (app.scene.selectedObject >= 0) ObjectDetails(app, f, app.activeMob, f.objects[app.scene.selectedObject], app.scene.selectedObject);
        ImGui::EndChild();
    }
}

static void ChecksTab(App& app) {
    if (app.mobs.empty()) {
        ImGui::TextWrapped("Load one or more .mob files in the Files tab to check them.");
        return;
    }
    if (ImGui::Button("Check again")) app.checksDirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The checks run by themselves when the files or sources change");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('E'));
    ImGui::Checkbox(("Errors (" + std::to_string(app.summary.errors) + ")").c_str(), &app.showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('W'));
    ImGui::Checkbox(("Warnings (" + std::to_string(app.summary.warnings) + ")").c_str(), &app.showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('I'));
    ImGui::Checkbox(("Info (" + std::to_string(app.summary.infos) + ")").c_str(), &app.showInfos);
    ImGui::PopStyleColor();
    if (app.database.Empty())
        ui::Note("No items database in the Settings tab: item, spell and prototype names are not checked.");
    else
        ImGui::TextDisabled("Names checked against %zu database file(s) beside %s", app.database.files.size(),
                            app.lib.dbPath.substr(app.lib.dbPath.find_last_of("/\\") + 1).c_str());
    if (app.findings.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "No problems found.");
        return;
    }
    if (ImGui::BeginTable("##findings", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 14);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, app.mobs.size() > 1 ? 150.0f : 70.0f);
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < app.findings.size(); ++i) {
            const checks::Finding& f = app.findings[i];
            if ((f.severity == 'E' && !app.showErrors) || (f.severity == 'W' && !app.showWarnings) || (f.severity == 'I' && !app.showInfos)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(f.severity));
            char sev[2] = {f.severity, 0};
            bool clicked = ImGui::Selectable(sev, false, ImGuiSelectableFlags_SpanAllColumns);
            ImGui::PopStyleColor();
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s", f.category.c_str(),
                                  f.object >= 0 ? "\nClick: select the object and go to it" : f.line > 0 ? "\nClick: show the script line" : "");
            }
            ImGui::TableNextColumn();
            // The file only when several are loaded; the script line when there is one.
            std::string where = app.mobs.size() > 1 && f.file >= 0 && f.file < static_cast<int>(app.mobs.size()) ? app.mobs[f.file]->file.fileName : "";
            if (f.line > 0) where += (where.empty() ? "line " : ":") + std::to_string(f.line);
            ImGui::TextUnformatted(where.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", f.message.c_str());
            if (clicked) {
                if (f.object >= 0) {
                    SelectObject(app, f.file, f.object, true);
                    app.requestTab = SideTab::Objects;
                } else if (f.line > 0) {
                    app.scriptFile = f.file;
                    app.scriptLine = f.line;
                    app.scrollToLine = true;
                    app.requestTab = SideTab::Script;
                }
            }
        }
        ImGui::EndTable();
    }
}

// Script highlighting: mapedit/script_highlight.hpp (shared with the UM DLL Connector).
using scripthl::ScriptNames;
using scripthl::EachScriptToken;
using scripthl::HighlightedLine;
using scripthl::NamesOf;

// ---- script editing ----------------------------------------------------------------------------------

// Puts new text (UTF-8) in a map's script, as one undo step. False (with a message) when it cannot.
static bool CommitScript(App& app, MobEntry& m, const std::string& utf8) {
    std::vector<uint8_t> cp;
    std::string err;
    if (!codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err)) { app.scriptMessage = "Not applied: the text has " + err; return false; }
    const std::string text(cp.begin(), cp.end());
    if (text == m.file.script) { app.scriptMessage = "No change"; return true; }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = m.file.path;
    step.bytes = m.file.bytes;
    if (!mob::SetScript(m.file, text)) { app.scriptMessage = "This map has no script node to write to"; return false; }
    PushUndo(app, std::move(step), "Script of " + m.file.fileName);
    app.checksDirty = true;
    app.scriptMessage = "Script changed (save with " + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ")";
    return true;
}

static void ApplyScriptEdit(App& app) {
    if (!app.scriptEditing) return;
    MobEntry* m = FindMob(app, app.scriptEditFor);
    if (m && CommitScript(app, *m, app.scriptEdit)) app.scriptEditing = false;
}

// The script opened in the system's editor for .eis files: saves there come back as an undoable change.
static void OpenScriptExternally(App& app, const MobEntry& m) {
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::temp_directory_path(ec) / ("um-multitool-" + std::filesystem::path(m.file.fileName).stem().string() + ".eis");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { app.scriptMessage = "Cannot write " + path.string(); return; }
        const std::string text = mob::Utf8(m.file.script);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    app.scriptExtPath = path.string();
    app.scriptExtMob = m.file.path;
    app.scriptExtTime = std::filesystem::last_write_time(path, ec);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const bool opened = pio.Platform_OpenInShellFn && pio.Platform_OpenInShellFn(ImGui::GetCurrentContext(), app.scriptExtPath.c_str());
    app.scriptMessage = opened ? "Opened in the external editor; its saves come back here" : "Open this file in an editor; its saves come back here";
}

// Called every frame: picks up saves of the externally edited copy (checked twice a second).
static void PollExternalScript(App& app) {
    if (app.scriptExtPath.empty()) return;
    static double last = 0.0;
    const double now = ImGui::GetTime();
    if (now - last < 0.5) return;
    last = now;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(app.scriptExtPath, ec);
    if (ec || t == app.scriptExtTime) return;
    app.scriptExtTime = t;
    MobEntry* m = FindMob(app, app.scriptExtMob);
    if (!m) { app.scriptExtPath.clear(); return; }
    std::ifstream in(app.scriptExtPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF)
        text.erase(0, 3); // a BOM some editors add
    if (CommitScript(app, *m, text)) app.scriptMessage = "Script updated from the external editor (" + m->file.fileName + ")";
    if (app.scriptEditing && app.scriptEditFor == m->file.path) app.scriptEdit = mob::Utf8(m->file.script);
}

// ---- script completion ------------------------------------------------------------------------------

static bool IsNameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '#' || u >= 0x80;
}

static std::string Signature(const MobScriptFunction& fn) {
    auto type = [](char t) -> const char* {
        switch (t) { case 'f': return "float"; case 's': return "string"; case 'o': return "object"; case 'g': return "group"; case 'v': return "nothing"; default: return "any"; }
    };
    std::string out = std::string(fn.name) + "(";
    for (const char* p = fn.params; *p; ++p) out += std::string(p == fn.params ? "" : ", ") + type(*p);
    return out + ") -> " + type(fn.returns);
}

// What the word before the cursor may become: script commands, the language's words, and the names
// written in the text (globals, scripts...), those starting with it first.
static void UpdateCompletion(App& app, const char* buf, int len, int cursor) {
    App::Completion& c = app.completion;
    int start = cursor;
    while (start > 0 && IsNameChar(buf[start - 1])) --start;
    const std::string prefix(buf + start, buf + cursor);
    const bool afterWord = cursor < len && IsNameChar(buf[cursor]);
    if (prefix != c.prefix || start != c.wordStart) c.dismissed = false;
    c.prefix = prefix;
    c.wordStart = start;
    int lineStart = cursor;
    while (lineStart > 0 && buf[lineStart - 1] != '\n') --lineStart;
    c.lineBeforeCursor.assign(buf + lineStart, buf + cursor);
    c.line = 0;
    for (int i = 0; i < cursor; ++i) if (buf[i] == '\n') ++c.line;
    // The call around the cursor: the name before the nearest open parenthesis.
    c.signature.clear();
    int depth = 0;
    for (int i = cursor - 1; i >= 0 && i > cursor - 400; --i) {
        if (buf[i] == ')') ++depth;
        else if (buf[i] == '(') {
            if (depth-- > 0) continue;
            int e = i;
            while (e > 0 && buf[e - 1] == ' ') --e;
            int b = e;
            while (b > 0 && IsNameChar(buf[b - 1])) --b;
            auto it = mobscript::FunctionTable().find(mobscript::LowerCase(std::string(buf + b, buf + e)));
            if (it != mobscript::FunctionTable().end()) c.signature = Signature(*it->second);
            break;
        }
    }
    // The names written in the text, gathered again when it changes.
    const size_t hash = std::hash<std::string_view>{}(std::string_view(buf, static_cast<size_t>(len)));
    if (hash != c.wordsFor) {
        c.wordsFor = hash;
        c.words.clear();
        std::unordered_set<std::string> seen;
        for (int i = 0; i < len;) {
            if (buf[i] == '/' && i + 1 < len && buf[i + 1] == '/') { while (i < len && buf[i] != '\n') ++i; continue; }
            if (buf[i] == '"') { ++i; while (i < len && buf[i] != '"' && buf[i] != '\n') ++i; ++i; continue; }
            if (!IsNameChar(buf[i])) { ++i; continue; }
            int b = i;
            while (i < len && IsNameChar(buf[i])) ++i;
            std::string w(buf + b, buf + i);
            if (w.size() >= 3 && !(w[0] >= '0' && w[0] <= '9') && seen.insert(mobscript::LowerCase(w)).second) c.words.push_back(w);
        }
    }
    c.items.clear();
    if (prefix.size() < 2 || afterWord || c.dismissed || (prefix[0] >= '0' && prefix[0] <= '9')) { c.index = 0; return; }
    const std::string lp = mobscript::LowerCase(prefix);
    std::vector<std::string> starts, contains;
    std::unordered_set<std::string> added;
    auto consider = [&](const std::string& w) {
        const std::string lw = mobscript::LowerCase(w);
        if (lw == lp || !added.insert(lw).second) return;
        const size_t at = lw.find(lp);
        if (at == 0) starts.push_back(w);
        else if (at != std::string::npos && lp.size() >= 3) contains.push_back(w);
    };
    static const char* const language[] = {"GlobalVars", "DeclareScript", "Script", "WorldScript", "if", "then", "else", "object", "group", "float", "string"};
    for (const char* w : language) consider(w);
    for (const MobScriptFunction& fn : kMobScriptFunctions) consider(fn.name);
    for (const std::string& w : c.words) if (mobscript::LowerCase(w) != lp) consider(w);
    auto byLength = [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; };
    std::sort(starts.begin(), starts.end(), byLength);
    std::sort(contains.begin(), contains.end(), byLength);
    c.items = starts;
    c.items.insert(c.items.end(), contains.begin(), contains.end());
    if (c.items.size() > 12) c.items.resize(12);
    if (c.index >= static_cast<int>(c.items.size())) c.index = 0;
}

static int ScriptEditCallback(ImGuiInputTextCallbackData* d) {
    App& app = *static_cast<App*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        app.scriptEdit.resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = app.scriptEdit.data();
        return 0;
    }
    App::Completion& c = app.completion;
    if (c.keepCursor) { // Up / Down went to the list
        d->CursorPos = d->SelectionStart = d->SelectionEnd = std::min(c.cursor, d->BufTextLen);
        c.keepCursor = false;
    }
    // Tab / Enter with the list up: the picked word replaces the one being typed (a command gets its "(").
    if ((c.accept || c.tab) && !c.items.empty() && c.wordStart <= d->CursorPos) {
        std::string w = c.items[c.index];
        if (mobscript::FunctionTable().count(mobscript::LowerCase(w)) && (d->CursorPos >= d->BufTextLen || d->Buf[d->CursorPos] != '(')) w += "(";
        d->DeleteChars(c.wordStart, d->CursorPos - c.wordStart);
        d->InsertChars(c.wordStart, w.c_str());
        c.items.clear();
        c.dismissed = true;
    } else if (c.tab) {
        d->InsertChars(d->CursorPos, "  "); // no list: Tab indents
    }
    c.accept = c.tab = false;
    c.cursor = d->CursorPos;
    int line = 1;
    for (int i = 0; i < d->CursorPos && i < d->BufTextLen; ++i) if (d->Buf[i] == '\n') ++line;
    app.scriptCursorLine = line;
    if (!d->HasSelection()) UpdateCompletion(app, d->Buf, d->BufTextLen, d->CursorPos);
    else c.items.clear();
    return 0;
}

static void ScriptContent(App& app);

// The Script tab: the script, or a note while it is in its own window.
// One area call of a map's script rewritten with new numbers (an undo step; the script is saved with the map).
static void SetAreaCall(App& app, MobEntry& m, const quests::AreaCall& a) {
    std::string text = m.file.script;
    if (a.end > text.size() || a.begin >= a.end) return;
    text.replace(a.begin, a.end - a.begin, quests::FormatAreaCall(a));
    if (CommitScript(app, m, codec::ToUtf8(std::vector<uint8_t>(text.begin(), text.end()), codec::Encoding::Cp1251)))
        app.scriptModelKey.clear(); // the areas are read again
}

// The script area (its file and call) under a ground point: a circle containing it, else a rectangle.
static bool AreaAt(App& app, float x, float y, std::string& file, size_t& index) {
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            const quests::AreaCall& a = calls[i];
            const float e = std::max(0.4f, app.scene.camera.distance * 0.012f); // the edge can be grabbed from just outside
            const bool in = a.round ? std::hypot(x - a.v[0], y - a.v[1]) <= a.v[2] + e
                                    : x >= std::min(a.v[0], a.v[2]) - e && x <= std::max(a.v[0], a.v[2]) + e && y >= std::min(a.v[1], a.v[3]) - e &&
                                          y <= std::max(a.v[1], a.v[3]) + e;
            if (in) { file = m->file.path; index = i; return true; }
        }
    }
    return false;
}

static void PlaceArea(App& app, float x, float y) {
    MobEntry* m = FindMob(app, app.areaPlace.file);
    if (!m) return;
    std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
    if (app.areaPlace.index >= calls.size()) return;
    quests::AreaCall a = calls[app.areaPlace.index];
    if (a.round) { a.v[0] = x; a.v[1] = y; }
    else {
        const float w = a.v[2] - a.v[0], h = a.v[3] - a.v[1];
        a.v[0] = x - w * 0.5f; a.v[1] = y - h * 0.5f; a.v[2] = a.v[0] + w; a.v[3] = a.v[1] + h;
    }
    SetAreaCall(app, *m, a);
}

// The areas the loaded maps' scripts declare, editable: their numbers (Enter applies), or "Place here".
static void AreasPanel(App& app) {
    int count = 0;
    for (auto& m : app.mobs) count += static_cast<int>(quests::AreaCalls(m->file.script).size());
    if (!ImGui::CollapsingHeader(("Areas (" + std::to_string(count) + ")###areas").c_str())) return;
    if (count == 0) { ImGui::TextDisabled("No AddRoundToArea / AddRectToArea in the loaded maps' scripts."); return; }
    ImGui::TextDisabled("The scripts' areas (Layers -> Script areas). Change a number and press Enter, or Place here then click the map:");
    ImGui::TextDisabled("the call in the script is rewritten (undo with %s; saved with the map).", ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            quests::AreaCall a = calls[i];
            ImGui::PushID((m->file.path + "#" + std::to_string(i)).c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Area %d %s", a.id, a.round ? "(circle)" : "(rectangle)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m->file.fileName.c_str());
            ImGui::SameLine(150);
            ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 110));
            const bool changed = a.round ? ImGui::InputFloat3("##v", a.v, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)
                                         : ImGui::InputFloat4("##v", a.v, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(a.round ? "x, y, radius" : "x1, y1, x2, y2 (two corners)");
            if (changed) SetAreaCall(app, *m, a);
            ImGui::SameLine();
            const bool placing = app.areaPlace.on && app.areaPlace.file == m->file.path && app.areaPlace.index == i;
            if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.35f, 0.75f, 1));
            if (ImGui::Button(placing ? "Click the map" : "Place here")) {
                if (placing) app.areaPlace.on = false;
                else { app.areaPlace.on = true; app.areaPlace.file = m->file.path; app.areaPlace.index = i; app.scene.options.scriptAreas = true; }
            }
            if (placing) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Then click the map: the area's centre moves there (Escape cancels)");
            ImGui::PopID();
        }
    }
    ImGui::Separator();
}

static void ScriptTab(App& app) {
    AreasPanel(app);
    if (app.scriptWindow) {
        ImGui::TextWrapped("The script is shown in its own window.");
        if (ImGui::Button("Show it here again")) app.scriptWindow = false;
        return;
    }
    ScriptContent(app);
}


static void ScriptContent(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    app.scriptFile = std::min(std::max(app.scriptFile, 0), static_cast<int>(app.mobs.size()) - 1);
    const char* detach = app.scriptWindow ? "Back to the tab" : "Open in a window";
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(detach).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##scriptfile", app.mobs[app.scriptFile]->file.fileName.c_str())) {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (ImGui::Selectable(app.mobs[i]->file.fileName.c_str(), static_cast<int>(i) == app.scriptFile)) {
                app.scriptFile = static_cast<int>(i);
                app.scriptLine = 0;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(detach)) app.scriptWindow = !app.scriptWindow;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The script in a window of its own, beside the program's (moved, resized and placed by the desktop)");
    MobEntry& entry = *app.mobs[app.scriptFile];
    const mob::File& f = entry.file;
    if (!f.hasScript) { ImGui::TextDisabled("This map has no mission script."); return; }
    // Editing: the whole text, applied as one undoable change (Ctrl+S applies and saves).
    if (app.scriptEditing && app.scriptEditFor != f.path) app.scriptEditing = false;
    if (app.scriptEditing) {
        if (ImGui::Button("Apply")) ApplyScriptEdit(app);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the text in the map (undoable); %s also applies it and saves", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { app.scriptEditing = false; app.scriptMessage.clear(); }
        ImGui::SameLine();
        const bool changed = mob::Utf8(f.script) != app.scriptEdit;
        ImGui::TextDisabled("line %d%s", app.scriptCursorLine, changed ? "  (not applied)" : "");
        if (!app.scriptMessage.empty()) ImGui::TextDisabled("%s", app.scriptMessage.c_str());
        App::Completion& c = app.completion;
        ImGui::TextDisabled("%s", c.signature.empty() ? "Tab: complete (or indent), Up/Down: pick, Esc: hide the list" : c.signature.c_str());
        if (app.scriptEditFocus) { ImGui::SetKeyboardFocusHere(); app.scriptEditFocus = false; }
        const ImGuiID editId = ImGui::GetID("##scriptedit");
        const bool editing = ImGui::GetActiveID() == editId;
        // The list takes Up / Down / Enter / Esc from the text while it shows; Tab always (complete or indent).
        const ImGuiID owner = ImGui::GetID("##scriptcompletion");
        if (editing) {
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) c.tab = true;
            ImGui::SetKeyOwner(ImGuiKey_Tab, owner, ImGuiInputFlags_LockThisFrame);
            // Esc would undo everything typed since the text box took the focus: it only hides the list.
            if (c.items.empty()) ImGui::SetKeyOwner(ImGuiKey_Escape, owner, ImGuiInputFlags_LockThisFrame);
            if (!c.items.empty()) {
                const int n = static_cast<int>(c.items.size());
                // The text box owns Up / Down: it moves its cursor, which the callback puts back.
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, editId)) { c.index = (c.index + 1) % n; c.keepCursor = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, editId)) { c.index = (c.index + n - 1) % n; c.keepCursor = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) c.accept = true;
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { c.dismissed = true; c.items.clear(); }
                for (ImGuiKey k : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
                    ImGui::SetKeyOwner(k, owner, ImGuiInputFlags_LockThisFrame);
            }
        }
        const ImGuiWindow* parent = ImGui::GetCurrentWindow();
        // The text box draws its text invisibly on a dark neutral ground; the coloured text is painted
        // over it below, at the same places (only the lines in view).
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.30f, 0.45f, 0.75f, 0.45f));
        ImGui::InputTextMultiline("##scriptedit", app.scriptEdit.data(), app.scriptEdit.capacity() + 1, ImVec2(-1, -1),
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways, ScriptEditCallback, &app);
        ImGui::PopStyleColor(6);
        char childName[512];
        std::snprintf(childName, sizeof(childName), "%s/%s_%08X", parent->Name, "##scriptedit", editId);
        ImGuiWindow* box = ImGui::FindWindowByName(childName);
        if (!box) return;
        const ImGuiStyle& style = ImGui::GetStyle();
        {
            // The names the text declares, found again at most twice a second while it changes.
            static ScriptNames names;
            static size_t namesFor = 0;
            static double namesAt = -10.0;
            const size_t hash = std::hash<std::string_view>{}(std::string_view(app.scriptEdit.data(), std::strlen(app.scriptEdit.c_str())));
            if (hash != namesFor && ImGui::GetTime() - namesAt > 0.5) {
                std::vector<uint8_t> cp;
                std::string err;
                codec::FromUtf8(app.scriptEdit.c_str(), codec::Encoding::Cp1251, cp, err);
                names = NamesOf(std::string(cp.begin(), cp.end()));
                namesFor = hash;
                namesAt = ImGui::GetTime();
            }
            const float lineH = ImGui::GetFontSize();
            const ImVec2 origin(box->Pos.x + style.FramePadding.x - box->Scroll.x, box->Pos.y + style.FramePadding.y - box->Scroll.y);
            const int firstLine = std::max(0, static_cast<int>((box->Scroll.y - style.FramePadding.y) / lineH) - 1);
            const int lastLine = firstLine + static_cast<int>(box->Size.y / lineH) + 3;
            ImDrawList* dl = box->DrawList;
            dl->PushClipRect(box->InnerClipRect.Min, box->InnerClipRect.Max, true);
            const char* text = app.scriptEdit.c_str();
            const char* p = text;
            for (int line = 0; *p && line <= lastLine; ++line) {
                const char* e = p;
                while (*e && *e != '\n') ++e;
                if (line >= firstLine) {
                    float x = origin.x;
                    const float y = origin.y + line * lineH;
                    EachScriptToken(p, (e > p && e[-1] == '\r') ? e - 1 : e, names, [&](const char* b, const char* t, const ImVec4& color) {
                        dl->AddText(ImVec2(x, y), ImGui::GetColorU32(color), b, t);
                        x += ImGui::CalcTextSize(b, t, false).x;
                    });
                }
                p = *e ? e + 1 : e;
            }
            dl->PopClipRect();
        }
        if (!editing || c.items.empty()) return;
        // The list under the cursor.
        const float x = ImGui::CalcTextSize(c.lineBeforeCursor.c_str()).x - ImGui::CalcTextSize(c.prefix.c_str()).x;
        ImVec2 at(box->Pos.x + style.FramePadding.x - box->Scroll.x + x, box->Pos.y + style.FramePadding.y - box->Scroll.y + (c.line + 1) * ImGui::GetFontSize() + 2);
        ImGui::SetNextWindowPos(at);
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::Begin("##completion", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_Tooltip);
        for (size_t i = 0; i < c.items.size(); ++i) {
            const std::string& w = c.items[i];
            auto fn = mobscript::FunctionTable().find(mobscript::LowerCase(w));
            const bool picked = static_cast<int>(i) == c.index;
            const std::string text = fn != mobscript::FunctionTable().end() ? Signature(*fn->second) : w;
            if (picked) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "> %s", text.c_str());
            else ImGui::Text("  %s", text.c_str());
        }
        ImGui::End();
        return;
    }
    if (ImGui::Button("Edit")) {
        app.scriptEditing = true;
        app.scriptEditFor = f.path;
        app.scriptEdit = mob::Utf8(f.script);
        app.scriptEditFocus = true;
        app.scriptMessage.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in external editor")) OpenScriptExternally(app, entry);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens a copy (.eis, UTF-8) in the system's editor; each save there comes back into\n"
            "the map as an undoable change. The map itself is written with %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    if (!app.scriptExtPath.empty() && app.scriptExtMob == f.path) {
        ImGui::SameLine();
        ImGui::TextDisabled("watching %s", app.scriptExtPath.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy the path");
        if (ImGui::IsItemClicked()) ImGui::SetClipboardText(app.scriptExtPath.c_str());
    }
    if (!app.scriptMessage.empty()) ImGui::TextDisabled("%s", app.scriptMessage.c_str());
    if (f.script.empty()) { ImGui::TextDisabled("The script is empty."); return; }
    // Lines as the checker counts them (every '\n').
    static const mob::File* cachedFor = nullptr;
    static std::vector<std::pair<size_t, size_t>> lines;
    static std::string text;
    static size_t cachedHash = 0;
    static ScriptNames names;
    const size_t hash = std::hash<std::string>{}(f.script);
    if (cachedFor != &f || cachedHash != hash) {
        cachedFor = &f;
        cachedHash = hash;
        text = mob::Utf8(f.script);
        names = NamesOf(f.script);
        lines.clear();
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                size_t end = i;
                if (end > start && text[end - 1] == '\r') --end;
                lines.push_back({start, end});
                start = i + 1;
            }
        }
    }
    ImGui::TextDisabled("%zu lines - the same numbering as the checks and um-multitool mobdump's .eis", lines.size());
    ImGui::BeginChild("##script", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    if (app.scrollToLine && app.scriptLine > 0) {
        ImGui::SetScrollY(std::max(0.0f, (app.scriptLine - 6) * lineHeight));
        app.scrollToLine = false;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines.size()), lineHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            bool marked = i + 1 == app.scriptLine;
            bool flagged = false;
            char severity = 0;
            for (const checks::Finding& fd : app.findings)
                if (fd.file == app.scriptFile && fd.line == i + 1) { flagged = true; if (!severity || fd.severity == 'E') severity = fd.severity; }
            // The line the checks jumped to gets a yellow band, lines with findings a tinted band and a bar
            // in the finding's colour; the text keeps its highlighting.
            ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x + ImGui::GetScrollMaxX() + 4000.0f, rowMin.y + ImGui::GetTextLineHeight());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (marked) draw->AddRectFilled(rowMin, rowMax, IM_COL32(255, 220, 120, 40));
            if (flagged) {
                ImVec4 c = SeverityColor(severity);
                draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.12f)));
                draw->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), ImGui::GetColorU32(c));
            }
            ImGui::TextDisabled("%5d", i + 1);
            ImGui::SameLine();
            ImGui::BeginGroup();
            HighlightedLine(text.data() + lines[i].first, text.data() + lines[i].second, names);
            ImGui::EndGroup();
            if (flagged && ImGui::IsItemHovered()) {
                std::string tip;
                for (const checks::Finding& fd : app.findings)
                    if (fd.file == app.scriptFile && fd.line == i + 1) tip += std::string(1, fd.severity) + ": " + fd.message + "\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------------------------------------
// Quest tab: the files of the open quest as text. Packed quests hold quest.reg (the game's binary
// registry), shown and saved as INI text; unpacked ones hold quest.ini. Texts keep their encoding.
// ------------------------------------------------------------------------------------------------

static bool IsLanguageFree(const std::string& entry) {
    return quest::EndsWithLower(entry, "map.txt") || quest::EndsWithLower(entry, "quest.ini") || quest::EndsWithLower(entry, "quest.reg");
}

// The same file in another copy of the quest: same name with either slash, or quest.ini <-> quest.reg.
static std::string MatchEntry(const quest::Quest& copy, const std::string& entry) {
    auto norm = [](std::string n) {
        n = Lower(n);
        std::replace(n.begin(), n.end(), '\\', '/');
        if (quest::EndsWithLower(n, "quest.reg") || quest::EndsWithLower(n, "quest.ini")) n = n.substr(0, n.size() - 4) + ".cfg";
        return n;
    };
    const std::string want = norm(entry);
    for (const std::string& e : quest::ListEntries(copy)) if (norm(e) == want) return e;
    return std::string();
}

// The file as UTF-8 text with \n lines (quest.reg converted to INI).
static bool ReadEntryText(const quest::Quest& q, const std::string& entry, std::string& text, codec::Encoding& enc, bool& crlf, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!quest::ReadEntry(q, entry, bytes)) { err = "cannot read " + entry; return false; }
    if (quest::EndsWithLower(entry, ".reg")) {
        std::string ini;
        if (!IniRegRegToText(bytes, ini, err)) return false;
        bytes.assign(ini.begin(), ini.end());
    }
    enc = codec::Detect(bytes);
    std::string t = codec::ToUtf8(bytes, enc);
    crlf = t.find("\r\n") != std::string::npos;
    text.clear();
    for (char c : t) if (c != '\r') text += c;
    return true;
}

static bool EncodeEntryText(const std::string& entry, const std::string& text, codec::Encoding enc, bool crlf, std::vector<uint8_t>& out, std::string& err) {
    std::string t;
    for (char c : text) { if (c == '\n' && crlf) t += '\r'; t += c; }
    std::vector<uint8_t> bytes;
    if (!codec::FromUtf8(t, enc, bytes, err)) return false;
    if (quest::EndsWithLower(entry, ".reg")) {
        std::string ini(bytes.begin(), bytes.end());
        if (!IniRegTextToReg(ini, out)) { err = "not valid INI text"; return false; }
        return true;
    }
    out = std::move(bytes);
    return true;
}

static int ResizeStringCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = static_cast<std::string*>(d->UserData);
        str->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

// Saves the Quest tab's text (to this copy, or every copy when "Save to every language" is on).
static void SaveQuestText(App& app) {
    if (!app.openQuest || !app.mqTextDirty) return;
    quest::QuestSet& set = *app.openQuest;
    const bool mapTxt = quest::EndsWithLower(app.mqEntry, "map.txt");
    if (mapTxt && app.questDirty) {
        app.mqMessage = "Save or revert the moved areas first: they would overwrite this text";
        return;
    }
    std::string errors;
    int saved = 0;
    for (size_t i = 0; i < set.copies.size(); ++i) {
        if (!app.mqApplyAll && static_cast<int>(i) != app.mqCopy) continue;
        const quest::Quest& c = set.copies[i];
        std::string entry = static_cast<int>(i) == app.mqCopy ? app.mqEntry : MatchEntry(c, app.mqEntry);
        if (entry.empty()) { errors += " " + set.labels[i] + " (no such file)"; continue; }
        std::vector<uint8_t> bytes;
        std::string err;
        if (!EncodeEntryText(entry, app.mqText, app.mqEncoding, app.mqCrlf, bytes, err) || !quest::WriteEntries(c, {{entry, bytes}}, err)) {
            errors += " " + set.labels[i] + ": " + err;
            continue;
        }
        ++saved;
    }
    app.mqMessage = "Saved " + app.mqEntry + " to " + std::to_string(saved) + " cop" + (saved == 1 ? "y" : "ies") +
                    (errors.empty() ? "" : "; failed:" + errors);
    app.mqTextDirty = false;
    if (mapTxt) ReloadQuest(app); // the areas and exits come from map.txt
}

// Ctrl+S / the toolbar's Save: whatever the open quest has unsaved (its areas, the Quest tab's text).
static bool AnyMobDirty(const App& app) {
    for (auto& m : app.mobs) if (m->Dirty()) return true;
    return TerrainUnsaved(app);
}

// Writes the terrain (to its own file, or `path`): the header and the edited sectors, the rest kept.
static bool SaveTerrain(App& app, const std::string& path) {
    std::vector<int> sectors(app.terrainEditedSectors.begin(), app.terrainEditedSectors.end());
    std::string err;
    const std::string old = app.terrainPath;
    if (!mpr::Save(app.terrain, path, sectors, app.terrainHeaderEdited || path != old, err)) {
        app.terrainMessage = "Terrain not saved: " + err;
        app.questMessage = app.terrainMessage;
        return false;
    }
    if (path != old) { // the terrain is now that file: its undo steps and load order follow it
        for (auto* v : {&app.undoSteps, &app.redoSteps}) for (EditStep& st : *v) if (st.file == old) st.file = path;
        std::replace(app.loadOrder.begin(), app.loadOrder.end(), old, path);
        app.terrainPath = path;
    }
    app.terrainEditedSectors.clear();
    app.terrainHeaderEdited = false;
    app.terrainMessage = "Saved " + path;
    return true;
}

std::vector<navgen::Object> NavObjects(const LayeredAssetSource& figures, const std::vector<const mob::File*>& maps, int* missing);

// Save with "Navmesh" on: the open maps that have a navmesh (AI_GRAPH, the zone's main map) get it built again
// from the terrain and every open map's objects, as the game builds it (navmesh_gen.hpp). Returns what to say.
static std::string RegenerateNavmeshes(App& app) {
    std::vector<mob::File*> targets;
    for (auto& m : app.mobs) if (m->file.aiGraphBytes) targets.push_back(&m->file);
    if (targets.empty()) return "";
    if (app.terrain.sectorsX <= 0) return "navmesh not rebuilt: no terrain open";
    std::vector<const mob::File*> all;
    for (auto& m : app.mobs) all.push_back(&m->file);
    int missing = 0;
    const std::vector<navgen::Object> objects = NavObjects(app.lib.figures, all, &missing);
    std::vector<uint8_t> payload;
    std::string err;
    if (!navgen::Generate(app.terrain, objects, payload, err)) return "navmesh not rebuilt: " + err;
    std::string done;
    for (mob::File* f : targets) {
        mob::SetAiGraph(*f, payload);
        done += (done.empty() ? "" : ", ") + f->fileName;
    }
    return "navmesh rebuilt in " + done + (missing ? " (" + std::to_string(missing) + " objects without a figure left out)" : "");
}

// "Navmesh differences": the navmesh built from the open terrain and maps, for the view to compare.
static void BuildCompareNavmesh(App& app) {
    app.scene.builtNav.clear();
    ++app.scene.builtNavStamp;
    if (app.terrain.sectorsX <= 0) { app.navCompareNote = "No terrain open."; return; }
    std::vector<const mob::File*> all;
    for (auto& m : app.mobs) all.push_back(&m->file);
    int missing = 0;
    const std::vector<navgen::Object> objects = NavObjects(app.lib.figures, all, &missing);
    std::string err;
    if (!navgen::Generate(app.terrain, objects, app.scene.builtNav, err)) { app.navCompareNote = err; return; }
    app.navCompareNote = missing ? std::to_string(missing) + " objects without a figure left out." : "";
}

// Ctrl+S / the toolbar's Save: every unsaved change (edited maps, the quest's areas, the Quest tab's text).
static void SaveQuestChanges(App& app) {
    std::string saved, failed, navmesh;
    if (app.lib.mapRegenNavmesh && (AnyMobDirty(app))) navmesh = RegenerateNavmeshes(app);
    for (auto& m : app.mobs) {
        if (!m->Dirty()) continue;
        std::string err;
        if (mob::Save(m->file, err)) { m->savedBytes = m->file.bytes; saved += (saved.empty() ? "" : ", ") + m->file.fileName; }
        else failed += " " + m->file.fileName + ": " + err;
    }
    if (TerrainUnsaved(app)) {
        if (SaveTerrain(app, app.terrainPath)) saved += (saved.empty() ? "" : ", ") + std::filesystem::path(app.terrainPath).filename().string();
        else failed += " terrain: " + app.terrainMessage;
    }
    if (!saved.empty() || !failed.empty()) app.filesMessage = (saved.empty() ? "" : "Saved " + saved) + (failed.empty() ? "" : "; could not save" + failed);
    if (!navmesh.empty()) app.filesMessage += (app.filesMessage.empty() ? "" : "; ") + navmesh;
    if (!saved.empty()) app.questMessage = "Saved " + saved;
    if (app.questDirty) SaveQuestAreas(app);
    if (app.mqTextDirty) SaveQuestText(app);
}

static void QuestTab(App& app) {
    if (!app.openQuest) {
        ImGui::TextWrapped("Open a quest in the Files tab (quests come from Settings > Quests) to see and edit its files here.");
        return;
    }
    quest::QuestSet& set = *app.openQuest;
    app.mqCopy = std::min(std::max(app.mqCopy, 0), static_cast<int>(set.copies.size()) - 1);
    const quest::Quest& copy = set.copies[app.mqCopy];
    ImGui::Text("%s", set.shown.name.c_str());
    if (set.copies.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##mqcopy", set.labels[app.mqCopy].c_str())) {
            for (size_t i = 0; i < set.copies.size(); ++i)
                if (ImGui::Selectable(set.labels[i].c_str(), static_cast<int>(i) == app.mqCopy) && !app.mqTextDirty) {
                    app.mqCopy = static_cast<int>(i);
                    app.mqLoadedFor.clear();
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The language pack whose copy is shown and edited");
    }
    ImGui::TextDisabled("%s (%s)", copy.path.c_str(), copy.packed ? "packed" : "unpacked");
    if (app.mqEntriesFor != copy.path) {
        app.mqEntries = quest::ListEntries(copy);
        app.mqEntriesFor = copy.path;
        if (std::find(app.mqEntries.begin(), app.mqEntries.end(), app.mqEntry) == app.mqEntries.end()) {
            std::string match = app.mqEntry.empty() ? std::string() : MatchEntry(copy, app.mqEntry);
            app.mqEntry = !match.empty() ? match : app.mqEntries.empty() ? std::string() : app.mqEntries.front();
            for (const std::string& e : app.mqEntries) if (match.empty() && quest::EndsWithLower(e, "map.txt")) app.mqEntry = e;
        }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##mqentry", app.mqEntry.c_str())) {
        for (const std::string& e : app.mqEntries)
            if (ImGui::Selectable(e.c_str(), e == app.mqEntry) && !app.mqTextDirty) { app.mqEntry = e; app.mqLoadedFor.clear(); }
        ImGui::EndCombo();
    }
    if (app.mqEntry.empty()) return;
    const std::string key = copy.path + "|" + app.mqEntry;
    if (app.mqLoadedFor != key) {
        std::string err;
        app.mqIsReg = quest::EndsWithLower(app.mqEntry, ".reg");
        if (!ReadEntryText(copy, app.mqEntry, app.mqText, app.mqEncoding, app.mqCrlf, err)) { app.mqText.clear(); app.mqMessage = err; }
        else app.mqMessage.clear();
        app.mqLoadedFor = key;
        app.mqTextDirty = false;
        app.mqApplyAll = IsLanguageFree(app.mqEntry);
    }
    if (app.mqIsReg) ImGui::TextDisabled("binary quest.reg, shown as INI text (saved back as .reg)");
    ImGui::SetNextItemWidth(170);
    int enc = static_cast<int>(app.mqEncoding);
    const char* encs[] = {codec::EncodingName(codec::Encoding::Cp1251), codec::EncodingName(codec::Encoding::Utf8), codec::EncodingName(codec::Encoding::Cp949)};
    if (ImGui::Combo("encoding", &enc, encs, 3)) { app.mqEncoding = static_cast<codec::Encoding>(enc); app.mqTextDirty = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The file's encoding (detected); it is saved in this encoding");
    if (set.copies.size() > 1) {
        ImGui::SameLine();
        ImGui::Checkbox("Save to every language", &app.mqApplyAll);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: every language pack's copy of this file gets this text (map.txt and quest.ini/.reg do not depend on\n"
                              "the language). Off: only the %s copy (for briefings and texts).", set.labels[app.mqCopy].c_str());
    }
    ImGui::BeginDisabled(!app.mqTextDirty);
    if (ImGui::Button("Save")) SaveQuestText(app);
    ImGui::SameLine();
    if (ImGui::Button("Revert##mq")) app.mqLoadedFor.clear();
    ImGui::EndDisabled();
    if (!app.mqMessage.empty()) { ImGui::SameLine(); ImGui::TextWrapped("%s", app.mqMessage.c_str()); }
    if (ImGui::InputTextMultiline("##mqtext", app.mqText.data(), app.mqText.capacity() + 1, ImVec2(-1, -1),
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeStringCallback, &app.mqText))
        app.mqTextDirty = true;
}

// ------------------------------------------------------------------------------------------------
// Find (Ctrl+F): the active map's objects by name (wildcards * and ?, case-sensitive or not), kind and
// ID range; select the matches.
// ------------------------------------------------------------------------------------------------

static bool WildcardMatch(const char* p, const char* t) {
    // * any run, ? any one character (iterative, with backtracking to the last *)
    const char *star = nullptr, *resume = nullptr;
    while (*t) {
        if (*p == '?' || *p == *t) { ++p; ++t; }
        else if (*p == '*') { star = p++; resume = t; }
        else if (star) { p = star + 1; t = ++resume; }
        else return false;
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

static bool FindMatches(const App& app, const mob::Object& o) {
    static const mob::Kind kinds[] = {mob::Kind::Object, mob::Kind::Unit, mob::Kind::Lever, mob::Kind::Torch, mob::Kind::MagicTrap,
                                      mob::Kind::Light, mob::Kind::Particle, mob::Kind::Sound};
    if (app.findKind > 0 && o.kind != kinds[app.findKind - 1]) return false;
    if (app.findIds && (!o.hasId || static_cast<int64_t>(o.id) < app.findIdMin || static_cast<int64_t>(o.id) > app.findIdMax)) return false;
    std::string pattern = app.findText;
    if (pattern.empty()) return true;
    auto fold = [&](std::string t) { return app.findCase ? t : Lower(t); };
    const bool wild = pattern.find_first_of("*?") != std::string::npos;
    pattern = fold(pattern);
    std::vector<std::string> fields = {mob::Utf8(o.name.empty() ? o.particleName : o.name)};
    if (app.findMore) { fields.push_back(o.templ); fields.push_back(mob::Utf8(o.prototype)); fields.push_back(mob::Utf8(o.parentTemplate)); }
    for (const std::string& f : fields) {
        const std::string t = fold(f);
        if (wild ? WildcardMatch(pattern.c_str(), t.c_str()) : t.find(pattern) != std::string::npos) return true;
    }
    return false;
}

// ---- delete, copy, paste, duplicate, new ---------------------------------------------------------------

static void DeleteSelection(App& app) {
    if (app.simulating) return;
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    PushUndo(app, std::move(step), "Delete " + std::to_string(app.scene.selection.size()) + " object(s)");
    const size_t n = app.scene.selection.size();
    mob::RemoveObjects(f, app.scene.selection);
    app.scene.ClearSelection();
    app.checksDirty = true;
    app.listedKey.clear();
    app.questMessage = "Deleted " + std::to_string(n) + " object(s)";
}

// The clipboard is also written to a file in the temporary folder, so another running um-multitool can
// paste it (like ei_maper's copy_paste_buffer.json): "UMCB", count, then per object its position and node.
static std::filesystem::path ClipboardFile() {
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "um-multitool-clipboard.bin";
}

static void WriteClipboardFile(const std::vector<App::Copied>& items) {
    std::ofstream out(ClipboardFile(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    const uint32_t n = static_cast<uint32_t>(items.size());
    out.write("UMCB", 4);
    out.write(reinterpret_cast<const char*>(&n), 4);
    for (const App::Copied& c : items) {
        const uint32_t size = static_cast<uint32_t>(c.node.size());
        out.write(reinterpret_cast<const char*>(&c.position), 12);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(c.node.data()), size);
    }
}

static bool ReadClipboardFile(std::vector<App::Copied>& items) {
    std::ifstream in(ClipboardFile(), std::ios::binary);
    char magic[4] = {};
    uint32_t n = 0;
    if (!in.read(magic, 4) || std::memcmp(magic, "UMCB", 4) != 0 || !in.read(reinterpret_cast<char*>(&n), 4) || n > 100000) return false;
    std::vector<App::Copied> out(n);
    for (App::Copied& c : out) {
        uint32_t size = 0;
        if (!in.read(reinterpret_cast<char*>(&c.position), 12) || !in.read(reinterpret_cast<char*>(&size), 4) || size < 8 || size > (64u << 20)) return false;
        c.node.resize(size);
        if (!in.read(reinterpret_cast<char*>(c.node.data()), size) || mob::U32(c.node.data() + 4) != size) return false;
    }
    items = std::move(out);
    return true;
}

static void CopySelection(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    app.clipboard.clear();
    for (int oi : app.scene.selection) app.clipboard.push_back({mob::ObjectNode(f, oi), f.objects[oi].position});
    WriteClipboardFile(app.clipboard);
    app.questMessage = "Copied " + std::to_string(app.clipboard.size()) + " object(s)";
}

// Free IDs for new objects: from the active map's first ID range, skipping every loaded map's IDs.
static std::vector<uint32_t> FreeIds(App& app, size_t count) {
    std::set<uint32_t> used;
    for (auto& m : app.mobs) for (const mob::Object& o : m->file.objects) if (o.hasId) used.insert(o.id);
    const mob::File& f = app.mobs[app.activeMob]->file;
    uint64_t next = f.mainRanges.empty() ? 1 : std::max<uint32_t>(f.mainRanges[0].min, 1);
    const uint64_t hi = f.mainRanges.empty() ? 0xFFFFFFFFu : f.mainRanges[0].max;
    std::vector<uint32_t> ids;
    while (ids.size() < count && next <= hi) { if (!used.count(static_cast<uint32_t>(next))) ids.push_back(static_cast<uint32_t>(next)); ++next; }
    return ids;
}

// Where pasted or new objects go: the ground under the mouse when it is over the view, else the view's centre.
static mob::Vec3 PlacePoint(App& app) {
    if (app.hoverGround) return {app.ground.x, app.ground.y, 0};
    return {app.scene.camera.targetX, app.scene.camera.targetY, 0};
}

// Inserts copies of these nodes into the active map with new IDs, moved so that their centre lands at
// `at` (or where they were, when `inPlace`), and selects them. One undo step.
static bool InsertCopies(App& app, std::vector<App::Copied> items, bool inPlace, mob::Vec3 at) {
    if (app.simulating) return false;
    if (app.mobs.empty() || items.empty()) return false;
    mob::File& f = app.mobs[app.activeMob]->file;
    if (!f.objectSectionAt) { app.questMessage = "This map has no object section to add to"; return false; }
    std::vector<uint32_t> ids = FreeIds(app, items.size());
    if (ids.size() < items.size()) { app.questMessage = "Not enough free IDs in the map's ID range"; return false; }
    mob::Vec3 c{0, 0, 0};
    for (auto& it : items) { c.x += it.position.x; c.y += it.position.y; }
    c.x /= items.size();
    c.y /= items.size();
    std::vector<std::vector<uint8_t>> nodes;
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<uint8_t> n = items[i].node;
        mob::NodeSetId(n, ids[i]);
        if (!inPlace) mob::NodeMove(n, at.x - c.x, at.y - c.y, 0.0f);
        nodes.push_back(std::move(n));
    }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    PushUndo(app, std::move(step), "Add " + std::to_string(items.size()) + " object(s)");
    std::vector<int> added = mob::InsertObjects(f, nodes);
    app.scene.ClearSelection();
    for (int oi : added) {
        if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
        else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
    }
    app.checksDirty = true;
    app.listedKey.clear();
    app.revealSelection = true;
    return true;
}

static void Paste(App& app) {
    ReadClipboardFile(app.clipboard); // the last copy, from this um-multitool or another one
    if (app.clipboard.empty()) { app.questMessage = "Nothing copied"; return; }
    if (InsertCopies(app, app.clipboard, false, PlacePoint(app))) app.questMessage = "Pasted " + std::to_string(app.clipboard.size()) + " object(s)";
}

// Ctrl+D: copies of the selection where it is, then moving them (like Blender's Shift+D).
static void StartTransform(App& app, Transform::Mode mode, ImVec2 mouse, ImVec2 min, ImVec2 size);
static void PruneLogicPoints(App& app);
static bool LogicPointAt(const mob::File& f, const LogicPointRef& r, mob::Vec3& out);
static void SetLogicPoint(mob::Logic& g, const LogicPointRef& r, const mob::Vec3& v);
static void DeleteLogicPoints(App& app);
static void Duplicate(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    std::vector<App::Copied> items;
    for (int oi : app.scene.selection) items.push_back({mob::ObjectNode(f, oi), f.objects[oi].position});
    if (InsertCopies(app, items, true, {}) && app.viewHovered)
        StartTransform(app, Transform::Move, ImGui::GetIO().MousePos, app.viewMin, app.viewSize);
}

// The objects whose figure the editor cannot show: listed; a click selects and shows one.
static void MissingWindow(App& app) {
    if (++app.missingCheck % 60 == 1 && app.scene.modelsPending == 0) app.missing = app.scene.MissingFigures();
    if (!app.missingOpen) return;
    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_Appearing);
    if (!ImGui::Begin("Figures that cannot be shown", &app.missingOpen)) { ImGui::End(); return; }
    if (app.missing.empty()) ImGui::TextDisabled("None.");
    if (ImGui::BeginTable("##miss", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Map"); ImGui::TableSetupColumn("Object"); ImGui::TableSetupColumn("Figure"); ImGui::TableSetupColumn("Why");
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < app.missing.size(); ++k) {
            const MapScene::Missing& mi = app.missing[k];
            if (mi.file < 0 || mi.file >= static_cast<int>(app.mobs.size())) continue;
            const mob::File& f = app.mobs[static_cast<size_t>(mi.file)]->file;
            if (mi.object >= static_cast<int>(f.objects.size())) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((f.fileName + "##mi" + std::to_string(k)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) SelectObject(app, mi.file, mi.object, true);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(checks::Label(f.objects[static_cast<size_t>(mi.object)]).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mi.figure.empty() ? "-" : mi.figure.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mi.error.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Figure categories by name prefix (the vanilla naming: stbuho1 = a building, naflli1 = a plant, initqi... = an item).
static const char* FigureCategory(const std::string& lowerName) {
    static const std::pair<const char*, const char*> kPrefixes[] = {
        {"stbu", "Buildings"}, {"stwa", "Walls & fences"}, {"stbr", "Bridges"}, {"st", "Structures"}, {"jst", "Statues & ruins"},
        {"j", "Ruins & props"}, {"nafl", "Plants"}, {"natr", "Trees"}, {"nast", "Stones"}, {"na", "Nature"},
        {"in", "Items & props"}, {"co", "Containers"}, {"un", "Creature figures"}, {"ef", "Effects & markers"}};
    for (const auto& pr : kPrefixes) if (lowerName.rfind(pr.first, 0) == 0) return pr.second;
    return "Other";
}

// What the preview shows: "o:<figure>|<texture>" or "u:<unit>".
static std::string PreviewKey(const App& app) {
    if (!app.newPreview) return "";
    if (app.newTab == 1) return app.newUnit.empty() ? "" : "u:" + app.newUnit;
    return app.newFigure[0] ? std::string("o:") + app.newFigure + "|" + app.newTexture : "";
}

// Draws the preview into the back buffer's corner (RenderGl, before the map paints over it) and keeps it as a texture.
static void RenderPreview(App& app) {
    if (!app.newOpen || app.previewWanted.empty()) return;
    const int size = 256;
    if (!app.preview) app.preview = std::make_unique<Scene>();
    Scene& sc = *app.preview;
    if (app.previewShown != app.previewWanted) {
        if (app.previewWanted.rfind("u:", 0) == 0) {
            mob::Object o;
            o.kind = mob::Kind::Unit;
            o.prototype = ToCp(app.previewWanted.substr(2));
            o.primTexture = "default0";
            const units::Monster* m = app.lib.unitsDb.FindMonster(app.previewWanted.substr(2));
            const units::Race* race = m ? app.lib.unitsDb.FindRace(m->race) : nullptr;
            o.templ = race ? race->mask : "";
            dress::Dress d = dress::Resolve(app.lib, o);
            if (!d.on) sc.LoadModel(app.lib, o.templ, true);
            else {
                sc.LoadUnit(app.lib, o.templ, fig::Vec3{0.5f, 0.5f, 0.5f}, true,
                            [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartShown(d, model, part); },
                            [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartTexture(d, model, part); });
                sc.textureName = d.skin;
            }
        } else {
            const std::string v = app.previewWanted.substr(2);
            const size_t bar = v.find('|');
            sc.LoadModel(app.lib, v.substr(0, bar), true);
            sc.textureName = bar == std::string::npos ? "" : v.substr(bar + 1);
            if (sc.textureName.empty() && !app.mobs.empty()) { // the pattern's texture, else the figure's own name
                sc.textureName = v.substr(0, bar);
            }
        }
        app.previewShown = app.previewWanted;
    }
    sc.options.grid = false;
    sc.camera.yawDeg = app.previewYaw;
    glDisable(GL_SCISSOR_TEST);
    sc.Draw(app.lib, 0, 0, size, size, 0.0f);
    glFinish();
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4), flipped(rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (int y = 0; y < size; ++y) std::memcpy(&flipped[static_cast<size_t>(y) * size * 4], &rgba[static_cast<size_t>(size - 1 - y) * size * 4], static_cast<size_t>(size) * 4);
    if (!app.previewTex) glGenTextures(1, &app.previewTex);
    glBindTexture(GL_TEXTURE_2D, app.previewTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
}

// The Add object / unit window: a figure (by category, searchable) or a database unit (dressed as the game does),
// a 3D preview, then a copy of a pattern of that kind (the selected object, else the active map's first one) with
// the choice applied, placed under the mouse (else the view's centre).
static void NewObjectWindow(App& app) {
    if (!app.newOpen) return;
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 60, app.viewportMin.y + 60), ImGuiCond_Appearing);
    if (!ImGui::Begin("Add object / unit", &app.newOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("Load a map first."); ImGui::End(); return; }
    const mob::File& f = app.mobs[app.activeMob]->file;
    if (ImGui::BeginTabBar("##newtabs")) {
        if (ImGui::BeginTabItem("Objects")) { app.newTab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Units")) { app.newTab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    const mob::Kind want = app.newTab == 1 ? mob::Kind::Unit : mob::Kind::Object;
    int pattern = -1;
    if (app.scene.selectedFile == app.activeMob && app.scene.selectedObject >= 0 && f.objects[app.scene.selectedObject].kind == want)
        pattern = app.scene.selectedObject;
    for (size_t i = 0; pattern < 0 && i < f.objects.size(); ++i) if (f.objects[i].kind == want) pattern = static_cast<int>(i);

    ImGui::BeginChild("##pick", ImVec2(330, -ImGui::GetFrameHeightWithSpacing() * 2), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##figfilter", app.newTab == 1 ? "search the units" : "search the figures", app.newFilter, sizeof(app.newFilter));
    const std::string filter = Lower(app.newFilter);
    if (app.newTab == 0) {
        static const char* const kCats[] = {"All", "Buildings", "Walls & fences", "Bridges", "Structures", "Statues & ruins", "Ruins & props", "Plants",
                                            "Trees", "Stones", "Nature", "Items & props", "Containers", "Creature figures", "Effects & markers", "Other"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##cat", &app.newCategory, kCats, IM_ARRAYSIZE(kCats));
        if (ImGui::BeginListBox("##figures", ImVec2(-1, -1))) {
            int shown = 0;
            for (const std::string& name : app.lib.figureIndex.baseNames) {
                const std::string low = Lower(name);
                if (!filter.empty() && low.find(filter) == std::string::npos) continue;
                if (app.newCategory > 0 && std::string(FigureCategory(low)) != kCats[app.newCategory]) continue;
                if (++shown > 600) { ImGui::TextDisabled("(more: search to narrow)"); break; }
                if (ImGui::Selectable(name.c_str(), name == app.newFigure)) { std::snprintf(app.newFigure, sizeof(app.newFigure), "%s", name.c_str()); app.newTexture[0] = '\0'; }
            }
            ImGui::EndListBox();
        }
    } else {
        if (app.lib.unitsDb.monsters.empty()) ImGui::TextDisabled("No database set in Settings: no units to pick.");
        if (ImGui::BeginListBox("##units", ImVec2(-1, -1))) {
            int shown = 0;
            for (const units::Monster& m : app.lib.unitsDb.monsters) {
                if (!filter.empty() && Lower(m.name).find(filter) == std::string::npos) continue;
                if (++shown > 800) { ImGui::TextDisabled("(more: search to narrow)"); break; }
                if (ImGui::Selectable(m.name.c_str(), m.name == app.newUnit)) app.newUnit = m.name;
            }
            ImGui::EndListBox();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Checkbox("3D preview", &app.newPreview);
    app.previewWanted = PreviewKey(app);
    if (app.newPreview && app.previewTex && !app.previewWanted.empty()) {
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(app.previewTex)), ImVec2(256, 256));
        ImGui::SetNextItemWidth(256);
        ImGui::SliderFloat("##yaw", &app.previewYaw, -180.0f, 180.0f, "turn %.0f");
    } else {
        ImGui::Dummy(ImVec2(256, 256));
    }
    if (app.newTab == 0) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("Texture", pattern >= 0 ? f.objects[pattern].primTexture.c_str() : "", app.newTexture, sizeof(app.newTexture));
        ImGui::SetItemTooltip("Empty: the pattern's. Most objects use the texture named like their figure.");
    }
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Name", pattern >= 0 ? mob::Utf8(f.objects[pattern].name).c_str() : "", app.newName, sizeof(app.newName));
    ImGui::EndGroup();

    if (pattern < 0) ImGui::TextDisabled(app.newTab == 1 ? "The active map has no unit to copy the other fields from." : "The active map has no object to copy the other fields from.");
    else ImGui::TextDisabled("Other fields from: %s (select another %s to use it)", checks::Label(f.objects[pattern]).c_str(), app.newTab == 1 ? "unit" : "object");
    const bool ready = pattern >= 0 && (app.newTab == 1 ? !app.newUnit.empty() : app.newFigure[0] != '\0');
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Add", ImVec2(120, 0))) {
        const mob::Object& p = f.objects[pattern];
        std::vector<uint8_t> node = mob::ObjectNode(f, pattern);
        std::string what;
        if (app.newTab == 1) {
            const units::Monster* m = app.lib.unitsDb.FindMonster(app.newUnit);
            const units::Race* race = m ? app.lib.unitsDb.FindRace(m->race) : nullptr;
            mob::NodeReplaceField(node, mob::kUnitPrototype, mob::TextPayload(ToCp(app.newUnit)));
            if (race && !race->mask.empty()) mob::NodeReplaceField(node, mob::kObjTemplate, mob::TextPayload(race->mask));
            mob::NodeReplaceField(node, mob::kObjPrimTexture, mob::TextPayload("default0")); // dressed from the database
            what = app.newUnit;
        } else {
            mob::NodeReplaceField(node, mob::kObjTemplate, mob::TextPayload(app.newFigure));
            if (app.newTexture[0]) mob::NodeReplaceField(node, mob::kObjPrimTexture, mob::TextPayload(app.newTexture));
            what = app.newFigure;
        }
        if (app.newName[0]) mob::NodeReplaceField(node, mob::kObjName, mob::TextPayload(ToCp(app.newName)));
        if (InsertCopies(app, {{node, p.position}}, false, PlacePoint(app))) app.newMessage = "Added " + what + " (G to move it)";
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("At the ground under the mouse, else the view's centre; press G to move it");
    if (!app.newMessage.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", app.newMessage.c_str()); }
    ImGui::End();
}

// Ctrl+A: everything in the active map that can be selected (only units in logic mode).
static void SelectAll(App& app) {
    if (app.mobs.empty()) return;
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    app.scene.ClearSelection();
    for (size_t i = 0; i < objects.size(); ++i) {
        if (app.scene.logicMode && objects[i].kind != mob::Kind::Unit) continue;
        if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, static_cast<int>(i));
        else { app.scene.selection.push_back(static_cast<int>(i)); app.scene.selectedObject = static_cast<int>(i); }
    }
}

// Moves every selected object along one axis by exactly the typed value (positive or negative). A
// figure's z is its height above the ground, a light's, particle's or sound's its absolute height.
static void OffsetWindow(App& app) {
    if (!app.offsetOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_Appearing);
    if (!ImGui::Begin("Offset the selection", &app.offsetOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    const bool ok = !app.mobs.empty() && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    if (ok) ImGui::Text("%zu object(s) selected in %s", app.scene.selection.size(), app.mobs[app.activeMob]->file.fileName.c_str());
    else ImGui::TextDisabled("Select objects in the active map first.");
    ImGui::TextUnformatted("Axis");
    for (int a = 0; a < 3; ++a) {
        ImGui::SameLine();
        ImGui::RadioButton(a == 0 ? "X" : a == 1 ? "Y" : "Z", &app.offsetAxis, a);
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputDouble("by", &app.offsetValue, 1.0, 10.0, "%.4f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negative to move the other way. Added to each object's own position.");
    ImGui::BeginDisabled(!ok || app.offsetValue == 0.0);
    if (ImGui::Button("Apply", ImVec2(120, 0))) {
        mob::File& f = app.mobs[app.activeMob]->file;
        EditStep step;
        step.kind = EditStep::Objects;
        step.file = f.path;
        step.objects = CaptureObjects(f, app.scene.selection);
        std::vector<ObjectState> now = step.objects;
        const double v = app.offsetValue;
        for (ObjectState& st : now) { // in double, then stored as the file's float
            float* c = app.offsetAxis == 0 ? &st.position.x : app.offsetAxis == 1 ? &st.position.y : &st.position.z;
            *c = static_cast<float>(static_cast<double>(*c) + v);
        }
        PushUndo(app, std::move(step), "Offset " + std::to_string(now.size()) + " object(s)");
        ApplyObjects(f, now);
        app.checksDirty = true;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "Moved %zu object(s) by %+.4f on %c (undo: %s)", now.size(), v, "XYZ"[app.offsetAxis],
                      ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
        app.offsetMessage = msg;
    }
    ImGui::EndDisabled();
    if (!app.offsetMessage.empty()) ImGui::TextDisabled("%s", app.offsetMessage.c_str());
    ImGui::End();
}

// ---- randomize ------------------------------------------------------------------------------------------
// Like ei_maper's "Randomize parameter": one parameter of each selected object gets its own random value
// in a range, or is moved by one. Rotations in degrees; one undo step per click.

static const char* const kRandomParams[] = {"Position X", "Position Y", "Position Z (above ground)", "Rotation about X", "Rotation about Y",
                                             "Rotation about Z (turn)", "Complection X", "Complection Y", "Complection Z", "Complection X Y Z (together)"};

// Euler angles (radians, about X then Y then Z) of a quaternion w x y z, and back.
static void QuatToEuler(const float q[4], float& rx, float& ry, float& rz) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    rx = std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y));
    ry = std::asin(std::max(-1.0f, std::min(1.0f, 2.0f * (w * y - z * x))));
    rz = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
}
static void EulerToQuat(float rx, float ry, float rz, float q[4]) {
    const float cr = std::cos(rx / 2), sr = std::sin(rx / 2), cp = std::cos(ry / 2), sp = std::sin(ry / 2), cy = std::cos(rz / 2), sy = std::sin(rz / 2);
    q[0] = cr * cp * cy + sr * sp * sy;
    q[1] = sr * cp * cy - cr * sp * sy;
    q[2] = cr * sp * cy + sr * cp * sy;
    q[3] = cr * cp * sy - sr * sp * cy;
}

static void RandomizeWindow(App& app) {
    if (!app.randomOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 50, app.viewportMin.y + 70), ImGuiCond_Appearing);
    if (!ImGui::Begin("Randomize", &app.randomOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    const bool ok = !app.mobs.empty() && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    if (ok) ImGui::Text("%zu object(s) selected in %s", app.scene.selection.size(), app.mobs[app.activeMob]->file.fileName.c_str());
    else ImGui::TextDisabled("Select objects in the active map first.");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Parameter", &app.randomParam, kRandomParams, IM_ARRAYSIZE(kRandomParams));
    ImGui::RadioButton("Set to a random value", &app.randomMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Add a random value", &app.randomMode, 1);
    const bool rotation = app.randomParam >= 3 && app.randomParam <= 5;
    ImGui::SetNextItemWidth(120);
    ImGui::InputFloat("min", &app.randomMin, 0, 0, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputFloat("max", &app.randomMax, 0, 0, "%.3f");
    if (rotation) { ImGui::SameLine(); ImGui::TextDisabled("degrees"); }
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Randomize", ImVec2(120, 0))) {
        static std::mt19937 rng{std::random_device{}()};
        const float lo = std::min(app.randomMin, app.randomMax), hi = std::max(app.randomMin, app.randomMax);
        std::uniform_real_distribution<float> dist(lo, hi);
        mob::File& f = app.mobs[app.activeMob]->file;
        EditStep step;
        step.kind = EditStep::Objects;
        step.file = f.path;
        step.objects = CaptureObjects(f, app.scene.selection);
        std::vector<ObjectState> now = step.objects;
        const bool add = app.randomMode == 1;
        int done = 0;
        for (ObjectState& st : now) {
            const mob::Object& o = f.objects[st.index];
            const bool figure = mob::HasFigure(o.kind);
            const float v = dist(rng);
            auto apply = [&](float& field) { field = add ? field + v : v; };
            switch (app.randomParam) {
            case 0: apply(st.position.x); break;
            case 1: apply(st.position.y); break;
            case 2: apply(st.position.z); break;
            case 3: case 4: case 5: {
                if (!figure) continue;
                const int axis = app.randomParam - 3;
                const float rad = v * 3.14159265f / 180.0f;
                if (add) { // turned about the world axis, like R
                    const float h = rad / 2;
                    const fig::Quat q{std::cos(h), axis == 0 ? std::sin(h) : 0.0f, axis == 1 ? std::sin(h) : 0.0f, axis == 2 ? std::sin(h) : 0.0f};
                    fig::Quat r = fig::QuatNormalize(fig::QuatMul(q, fig::Quat{st.rotation[0], st.rotation[1], st.rotation[2], st.rotation[3]}));
                    st.rotation[0] = r.w; st.rotation[1] = r.x; st.rotation[2] = r.y; st.rotation[3] = r.z;
                } else { // that angle set, the other two kept
                    float e[3];
                    QuatToEuler(st.rotation, e[0], e[1], e[2]);
                    e[axis] = rad;
                    EulerToQuat(e[0], e[1], e[2], st.rotation);
                }
                break;
            }
            case 6: if (!figure) continue; apply(st.complection.x); break;
            case 7: if (!figure) continue; apply(st.complection.y); break;
            case 8: if (!figure) continue; apply(st.complection.z); break;
            default: if (!figure) continue; apply(st.complection.x); apply(st.complection.y); apply(st.complection.z); break;
            }
            ++done;
        }
        PushUndo(app, std::move(step), std::string("Randomize ") + kRandomParams[app.randomParam]);
        ApplyObjects(f, now);
        app.checksDirty = true;
        app.randomMessage = "Randomized " + std::string(kRandomParams[app.randomParam]) + " of " + std::to_string(done) + " object(s) (undo: " +
                            ui::BindName(app.lib.mapKeys[config::kKeyUndo]) + ")";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Each object gets its own value between min and max. Rotation and complection apply to figures only.");
    if (!app.randomMessage.empty()) ImGui::TextDisabled("%s", app.randomMessage.c_str());
    ImGui::End();
}

static void FindWindow(App& app) {
    if (!app.findOpen) return;
    ImGui::SetNextWindowSize(ImVec2(440, 460), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 30, app.viewportMin.y + 50), ImGuiCond_Appearing);
    if (!ImGui::Begin("Find objects", &app.findOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    ImGui::TextDisabled("In the active map: %s", app.mobs[app.activeMob]->file.fileName.c_str());
    if (app.findFocus) { ImGui::SetKeyboardFocusHere(); app.findFocus = false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##findtext", "name; wildcards: * any, ? one character (e.g. Orc*-1?7*)", app.findText, sizeof(app.findText));
    ImGui::Checkbox("Case sensitive", &app.findCase);
    ImGui::SameLine();
    ImGui::Checkbox("Also figure, prototype, template", &app.findMore);
    static const char* kinds[] = {"All kinds", "World objects", "Units", "Levers", "Torches", "Magic traps", "Lights", "Particles", "Sounds"};
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##findkind", &app.findKind, kinds, IM_ARRAYSIZE(kinds));
    ImGui::SameLine();
    ImGui::Checkbox("ID from", &app.findIds);
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.findIds);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##idmin", &app.findIdMin, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##idmax", &app.findIdMax, 0);
    ImGui::EndDisabled();

    const auto& objects = app.mobs[app.activeMob]->file.objects;
    std::vector<int> found;
    for (size_t i = 0; i < objects.size(); ++i) {
        const mob::Object& o = objects[i];
        if (app.scene.logicMode && o.kind != mob::Kind::Unit) continue; // logic mode selects units only
        if (FindMatches(app, o)) found.push_back(static_cast<int>(i));
    }
    ImGui::Text("%zu found", found.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(found.empty());
    if (ImGui::Button("Select all matching")) {
        app.scene.ClearSelection();
        for (int oi : found) {
            if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
            else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
        }
        app.findMessage = std::to_string(found.size()) + " selected";
        app.requestTab = SideTab::Objects;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to selection")) {
        int added = 0;
        for (int oi : found) {
            if (app.scene.IsSelected(app.activeMob, oi)) continue;
            if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
            else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
            ++added;
        }
        app.findMessage = std::to_string(added) + " added";
    }
    ImGui::EndDisabled();
    if (!app.findMessage.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", app.findMessage.c_str()); }
    if (ImGui::BeginTable("##found", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(found.size()));
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const int oi = found[r];
                const mob::Object& o = objects[oi];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(oi);
                if (ImGui::Selectable(ObjectLabel(o).c_str(), app.scene.IsSelected(app.activeMob, oi),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::GetIO().KeyShift) app.scene.Toggle(app.activeMob, oi);
                    else SelectObject(app, app.activeMob, oi, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", mob::KindName(o.kind));
                ImGui::TableNextColumn();
                if (o.hasId) ImGui::TextDisabled("%u", o.id);
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------------
// Diplomacy: the active map's 32 x 32 table between player groups (DIPLOMATION), as ei_maper edits it:
// 0 friend (green), 1 neutral (yellow), 2 enemy (red); a click cycles the value, both ways by default.
// ------------------------------------------------------------------------------------------------

static void DiplomacyTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    mob::File& f = m.file;
    ImGui::Text("%s", f.fileName.c_str());
    if (!f.hasDiplomacy) { ImGui::TextDisabled("This map has no diplomacy table (quest maps use their base map's)."); return; }
    ImGui::Checkbox("Both ways", &app.dipSymmetric);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A change applies to row -> column and column -> row");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.35f, 1), "friend");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.25f, 1), "neutral");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.25f, 1), "enemy");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click a cell: next value; right-click: previous. A row is a group, its cells its attitude towards the column's group.");
    ImGui::PopStyleColor();
    const float cell = std::max(10.0f, std::min(22.0f, (ImGui::GetContentRegionAvail().x - 30.0f) / 32.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    const float head = 26.0f;
    ImGui::InvisibleButton("##dipgrid", ImVec2(head + cell * 32, head + cell * 32), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hr = -1, hc = -1;
    if (hovered) {
        int c = static_cast<int>((mouse.x - origin.x - head) / cell), r = static_cast<int>((mouse.y - origin.y - head) / cell);
        if (c >= 0 && c < 32 && r >= 0 && r < 32 && mouse.x >= origin.x + head && mouse.y >= origin.y + head) { hr = r; hc = c; }
    }
    auto name = [&](int i) {
        return i < static_cast<int>(f.diplomacyNames.size()) && !f.diplomacyNames[i].empty() ? mob::Utf8(f.diplomacyNames[i]) : "group " + std::to_string(i);
    };
    for (int i = 0; i < 32; ++i) {
        char n[8];
        std::snprintf(n, sizeof(n), "%d", i);
        ImU32 col = (i == hr || i == hc) ? IM_COL32(255, 225, 130, 255) : IM_COL32(170, 170, 175, 255);
        // Column numbers: every other one when two digits do not fit a cell.
        if (i < 10 || ImGui::CalcTextSize(n).x + 2 <= cell || i % 2 == 0) draw->AddText(ImVec2(origin.x + head + i * cell + 1, origin.y + 6), col, n);
        draw->AddText(ImVec2(origin.x + 2, origin.y + head + i * cell + (cell - ImGui::GetFontSize()) * 0.5f), col, n);
    }
    for (int r = 0; r < 32; ++r)
        for (int c = 0; c < 32; ++c) {
            const int v = f.diplomacy[static_cast<size_t>(r) * 32 + c];
            ImU32 col = r == c ? IM_COL32(60, 60, 64, 255) : v == 0 ? IM_COL32(40, 130, 50, 255) : v == 1 ? IM_COL32(170, 150, 40, 255)
                                                           : v == 2 ? IM_COL32(160, 45, 40, 255) : IM_COL32(90, 90, 200, 255);
            ImVec2 a(origin.x + head + c * cell, origin.y + head + r * cell);
            draw->AddRectFilled(a, ImVec2(a.x + cell - 1, a.y + cell - 1), col);
            if (r == hr && c == hc) draw->AddRect(a, ImVec2(a.x + cell - 1, a.y + cell - 1), IM_COL32(255, 255, 255, 255));
        }
    if (hr >= 0) {
        static const char* values[] = {"friend", "neutral", "enemy"};
        const int v = f.diplomacy[static_cast<size_t>(hr) * 32 + hc];
        ImGui::SetTooltip("%s -> %s: %s%s", name(hr).c_str(), name(hc).c_str(), v >= 0 && v <= 2 ? values[v] : std::to_string(v).c_str(),
                          hr == hc ? " (a group with itself: not editable)" : "");
        const bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left), right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if ((left || right) && hr != hc) {
            EditStep step;
            step.kind = EditStep::Diplomacy;
            step.file = f.path;
            step.diplomacy = f.diplomacy;
            PushUndo(app, std::move(step), "Diplomacy");
            const int next = ((v + (left ? 1 : 2)) % 3 + 3) % 3; // 0 -> 1 -> 2 -> 0 (right: backwards)
            mob::SetDiplomacy(f, hr, hc, next);
            if (app.dipSymmetric) mob::SetDiplomacy(f, hc, hr, next);
        }
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Hover a cell for the groups' names (DIPLOMATION_PL_NAMES). Save with %s, undo with %s.",
                       ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------------------------------------
// IDs: the active map's ID ranges (MAIN_RANGE / SEC_RANGE), giving the selection new IDs, copying them,
// and checking the map's IDs (repeated, shared with the other loaded maps, outside the ranges).
// ------------------------------------------------------------------------------------------------

static std::set<uint32_t> UsedIds(const App& app) {
    std::set<uint32_t> used;
    for (auto& m : app.mobs)
        for (const mob::Object& o : m->file.objects) if (o.hasId) used.insert(o.id);
    return used;
}

static void CheckIds(App& app) {
    app.idIssues.clear();
    app.idChecked = true;
    if (app.mobs.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    std::map<uint32_t, std::vector<int>> byId;
    int noId = 0;
    for (size_t i = 0; i < f.objects.size(); ++i) {
        if (f.objects[i].hasId) byId[f.objects[i].id].push_back(static_cast<int>(i));
        else ++noId;
    }
    for (auto& kv : byId)
        if (kv.second.size() > 1)
            for (int oi : kv.second) app.idIssues.push_back({"ID " + std::to_string(kv.first) + " is used " + std::to_string(kv.second.size()) + " times in this map: " + checks::Label(f.objects[oi]), oi});
    for (size_t k = 0; k < app.mobs.size(); ++k) {
        if (static_cast<int>(k) == app.activeMob) continue;
        for (const mob::Object& other : app.mobs[k]->file.objects) {
            if (!other.hasId) continue;
            auto it = byId.find(other.id);
            if (it != byId.end())
                app.idIssues.push_back({"ID " + std::to_string(other.id) + " is also used in " + app.mobs[k]->file.fileName + ": " + checks::Label(f.objects[it->second[0]]), it->second[0]});
        }
    }
    if (!f.mainRanges.empty()) {
        for (size_t i = 0; i < f.objects.size(); ++i) {
            const mob::Object& o = f.objects[i];
            if (!o.hasId) continue;
            bool inside = false;
            for (const mob::Range& r : f.mainRanges) inside |= o.id >= r.min && o.id <= r.max;
            if (!inside) app.idIssues.push_back({"ID " + std::to_string(o.id) + " is outside the map's ID ranges: " + checks::Label(o), static_cast<int>(i)});
        }
    }
    if (noId) app.idIssues.push_back({std::to_string(noId) + " object(s) without an ID field", -1});
}

static void IdsTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    mob::File& f = m.file;
    ImGui::Text("%s", f.fileName.c_str());
    {
        const uint32_t kind = f.bytes.size() >= 16 ? mob::U32(f.bytes.data() + 8) : 0;
        bool quest = mob::IsQuestMob(f);
        ImGui::BeginDisabled(kind != mob::kBaseMob && kind != mob::kQuestMob);
        if (ImGui::Checkbox("Quest MOB", &quest)) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::SetQuestMob(f, quest)) { PushUndo(app, std::move(step), quest ? "Quest MOB on" : "Quest MOB off"); app.checksDirty = true; }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("PR_OBJECT_DB_FILE: a quest map, loaded over a zone (ei_maper's \"is Quest Mob?\"): no WORLD_SET, uses SEC_RANGE.\n"
                "Off, SC_OBJECT_DB_FILE: a zone's own map, with WORLD_SET (time, ambient, sun, wind), usually MAIN_RANGE.");
        ImGui::SameLine();
        ImGui::TextDisabled(f.hasWorld ? "(has WORLD_SET)" : "(no WORLD_SET)");
    }

    ImGui::SeparatorText("ID ranges");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The IDs this map's objects may use (MAIN_RANGE), and SEC_RANGE. New IDs are taken from them.");
    ImGui::PopStyleColor();
    auto rangeRows = [&](std::vector<mob::Range>& ranges, const char* label) {
        for (size_t i = 0; i < ranges.size(); ++i) {
            ImGui::PushID(label);
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s %zu", label, i + 1);
            ImGui::SameLine(110);
            uint32_t v[2] = {ranges[i].min, ranges[i].max};
            ImGui::SetNextItemWidth(220);
            static std::map<ImGuiID, std::array<uint32_t, 2>> pending;
            const ImGuiID key = ImGui::GetID("##r");
            auto it = pending.find(key);
            if (it != pending.end()) { v[0] = it->second[0]; v[1] = it->second[1]; }
            ImGui::InputScalarN("##r", ImGuiDataType_U32, v, 2);
            if (ImGui::IsItemActive()) pending[key] = {v[0], v[1]};
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                pending.erase(key);
                if (v[0] <= v[1] && (v[0] != ranges[i].min || v[1] != ranges[i].max)) {
                    EditStep step;
                    step.kind = EditStep::Bytes;
                    step.file = f.path;
                    step.bytes = f.bytes;
                    PushUndo(app, std::move(step), "ID range");
                    mob::SetRange(f, ranges[i], v[0], v[1]);
                    app.checksDirty = true;
                } else if (v[0] > v[1]) {
                    app.idMessage = "A range's start must not be past its end";
                }
            } else if (!ImGui::IsItemActive()) {
                pending.erase(key);
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    };
    rangeRows(f.mainRanges, "Main");
    rangeRows(f.secRanges, "Sec");
    if (f.mainRanges.empty() && f.secRanges.empty()) ImGui::TextDisabled("(this map has no ID ranges)");

    ImGui::SeparatorText("The selection");
    const bool sel = app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Copy IDs")) {
        std::string ids;
        for (int oi : app.scene.selection)
            if (f.objects[oi].hasId) ids += (ids.empty() ? "" : ", ") + std::to_string(f.objects[oi].id);
        ImGui::SetClipboardText(ids.c_str());
        app.idMessage = "Copied the IDs of the selection";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("To the clipboard, separated by commas");
    ImGui::EndDisabled();
    // New IDs: the lowest ones from the chosen start that no loaded map uses, within the range.
    std::vector<std::string> rangeNames;
    for (const mob::Range& r : f.mainRanges) rangeNames.push_back(std::to_string(r.min) + " - " + std::to_string(r.max));
    if (rangeNames.empty()) rangeNames.push_back("1 - 4294967295 (no ranges)");
    app.idRange = std::min(std::max(app.idRange, 0), static_cast<int>(rangeNames.size()) - 1);
    const uint32_t lo = f.mainRanges.empty() ? 1 : f.mainRanges[app.idRange].min;
    const uint32_t hi = f.mainRanges.empty() ? 0xFFFFFFFFu : f.mainRanges[app.idRange].max;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("New IDs in");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##idrange", rangeNames[app.idRange].c_str())) {
        for (size_t i = 0; i < rangeNames.size(); ++i)
            if (ImGui::Selectable(rangeNames[i].c_str(), static_cast<int>(i) == app.idRange)) { app.idRange = static_cast<int>(i); app.idStart = 0; }
        ImGui::EndCombo();
    }
    if (app.idStart < lo || app.idStart > hi) app.idStart = lo;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("from");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputScalar("##idstart", ImGuiDataType_U32, &app.idStart);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Reset selected IDs")) {
        std::set<uint32_t> used = UsedIds(app);
        for (int oi : app.scene.selection) if (f.objects[oi].hasId) used.erase(f.objects[oi].id); // their own IDs may be reused
        std::vector<int> targets;
        for (int oi : app.scene.selection) if (f.objects[oi].hasId && f.objects[oi].idAt) targets.push_back(oi);
        std::sort(targets.begin(), targets.end());
        std::vector<uint32_t> ids;
        uint64_t next = std::max(app.idStart, lo);
        while (ids.size() < targets.size() && next <= hi) {
            if (!used.count(static_cast<uint32_t>(next))) ids.push_back(static_cast<uint32_t>(next));
            ++next;
        }
        if (ids.size() < targets.size()) {
            app.idMessage = "Not enough free IDs in that range from " + std::to_string(app.idStart);
        } else {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            PushUndo(app, std::move(step), "New IDs");
            for (size_t k = 0; k < targets.size(); ++k) mob::SetId(f, f.objects[targets[k]], ids[k]);
            app.checksDirty = true;
            app.listedKey.clear();
            app.idMessage = "Gave " + std::to_string(targets.size()) + " object(s) the IDs " + std::to_string(ids.front()) +
                            (ids.size() > 1 ? " to " + std::to_string(ids.back()) : "");
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Gives each selected object the next ID no loaded map uses (scripts naming the old IDs\n"
            "won't find them: the Checks tab lists those).");

    ImGui::SeparatorText("Check IDs");
    if (ImGui::Button("Check IDs")) CheckIds(app);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("IDs used twice in this map, also used by another loaded map, or outside the map's ranges");
    if (!app.idMessage.empty()) { ImGui::SameLine(); ImGui::TextWrapped("%s", app.idMessage.c_str()); }
    if (app.idChecked) {
        if (app.idIssues.empty()) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "No ID problems.");
        for (size_t i = 0; i < app.idIssues.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto& issue = app.idIssues[i];
            if (ImGui::Selectable(issue.text.c_str()) && issue.object >= 0 && issue.object < static_cast<int>(f.objects.size()))
                SelectObject(app, app.activeMob, issue.object, true);
            ImGui::PopID();
        }
    }
}

// ---- terrain editing (ei_maper's tile brush and tile parameters) --------------------------------------
// A packed tile (mpr_file.hpp): bits 0-5 the tile in its texture (8 x 8), 6-13 the texture, 14-15 the
// rotation. The brush's tile is texture * 64 + tile, the same index the tile types use.

static const char* const kTileTypeNames[] = {"grass", "ground", "stone", "sand", "rock", "field", "water", "road",
                                             "undefined", "snow", "ice", "dry grass", "snowballs", "lava", "swamp", "high rock"};

static uint16_t PackTile(int tile, int rotation) {
    return static_cast<uint16_t>(((tile / 64) << 6) | (tile % 64) | ((rotation & 3) << 14));
}

// The texture and the part of it (ImGui UVs: row 0 of the tiles is at the bottom of the picture) of a tile.
static void TileUvs(int tile, ImVec2& uv0, ImVec2& uv1) {
    const int t = tile % 64, cx = t % 8, cy = 7 - t / 8;
    uv0 = ImVec2(cx / 8.0f, cy / 8.0f);
    uv1 = ImVec2((cx + 1) / 8.0f, (cy + 1) / 8.0f);
}

// The land or water tile at a world point: its sector index and place; false outside the map.
static bool TileAt(const mpr::Map& m, float x, float y, int& sector, int& row, int& col) {
    if (x < 0 || y < 0 || x >= m.Width() || y >= m.Height()) return false;
    const int sx = static_cast<int>(x / 32.0f), sy = static_cast<int>(y / 32.0f);
    if (!m.At(sx, sy)) return false;
    sector = sy * m.sectorsX + sx;
    col = std::min(static_cast<int>((x - sx * 32.0f) / 2.0f), 15);
    row = std::min(static_cast<int>((y - sy * 32.0f) / 2.0f), 15);
    return true;
}

static void PushTerrainHeaderUndo(App& app) {
    EditStep step;
    step.kind = EditStep::Terrain;
    step.file = app.terrainPath;
    step.header = true;
    step.materials = app.terrain.materials;
    step.tileTypes = app.terrain.tileTypes;
    step.animTiles = app.terrain.animTiles;
    PushUndo(app, std::move(step), "Terrain parameters");
}

// Paints the brush's tile on the tile at (x, y), keeping the sector as it was in the stroke's undo step.
static void PaintAt(App& app, float x, float y) {
    mpr::Map& m = app.terrain;
    int si, row, col;
    if (!TileAt(m, x, y, si, row, col)) return;
    mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
    if (app.brushWater && !s.water) { app.terrainMessage = "This sector has no water layer to paint on"; return; }
    const uint16_t packed = PackTile(app.brushTile, app.brushRotation);
    const bool same = app.brushWater ? s.waterTiles[row][col] == packed && s.waterMaterial[row][col] == app.brushMaterial
                                     : s.landTiles[row][col] == packed;
    if (same) return;
    bool kept = false;
    for (const auto& p : app.stroke.sectors) kept |= p.first == si;
    if (!kept) app.stroke.sectors.push_back({si, s});
    if (app.brushWater) {
        s.waterTiles[row][col] = packed;
        s.waterMaterial[row][col] = static_cast<int16_t>(app.brushMaterial);
    } else {
        s.landTiles[row][col] = packed;
    }
    app.terrainEditedSectors.insert(si);
    app.terrainRebuild = true;
}

// The brush in the view: a left drag paints (one undo step per stroke), Alt+click takes the tile under
// the mouse. True when it took the mouse.
static bool TerrainBrushInput(App& app, ImVec2 local) {
    if (!app.tileBrush || !app.terrainLoaded) { app.painting = false; return false; }
    ImGuiIO& io = ImGui::GetIO();
    fig::Vec3 g;
    const bool onGround = app.scene.GroundAt(local.x, local.y, g);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) { // the eyedropper
            int si, row, col;
            if (onGround && TileAt(app.terrain, g.x, g.y, si, row, col)) {
                const mpr::Sector& s = app.terrain.sectors[static_cast<size_t>(si)];
                const bool water = app.brushWater && s.water && s.waterMaterial[row][col] >= 0;
                const uint16_t packed = water ? s.waterTiles[row][col] : s.landTiles[row][col];
                app.brushTile = ((packed >> 6) & 255) * 64 + (packed & 63);
                app.brushRotation = (packed >> 14) & 3;
                if (water) app.brushMaterial = s.waterMaterial[row][col];
            }
            app.swallowLeftRelease = true;
            return true;
        }
        app.painting = true;
        app.stroke = EditStep{};
        app.stroke.kind = EditStep::Terrain;
        app.stroke.file = app.terrainPath;
    }
    if (!app.painting) return false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (onGround) PaintAt(app, g.x, g.y);
    } else {
        app.painting = false;
        if (!app.stroke.sectors.empty()) PushUndo(app, std::move(app.stroke), "Paint terrain tiles");
        app.stroke = EditStep{};
    }
    return true;
}

// The height brush: every land vertex within the radius moves (raise / lower by `heightStrength` units a second,
// smooth toward its neighbours, flatten toward the height under the stroke's start), with a soft falloff. A vertex on
// a sector's edge exists in both sectors: each copy moves the same. Normals are computed again around the change.
static void SculptAt(App& app, float gx, float gy, float dt) {
    mpr::Map& m = app.terrain;
    if (m.maxZ <= 0) return;
    const float R = std::max(app.heightRadius, 0.5f), k = 65535.0f / m.maxZ;
    std::vector<int> touched;
    for (int sy = std::max(0, static_cast<int>((gy - R) / 32)); sy <= std::min(m.sectorsY - 1, static_cast<int>((gy + R) / 32)); ++sy)
        for (int sx = std::max(0, static_cast<int>((gx - R) / 32)); sx <= std::min(m.sectorsX - 1, static_cast<int>((gx + R) / 32)); ++sx) {
            const int si = sy * m.sectorsX + sx;
            mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
            if (!s.present) continue;
            bool changed = false, kept = false;
            for (const auto& pr : app.stroke.sectors) kept |= pr.first == si;
            const mpr::Sector before = s;
            for (int r = 0; r <= 32; ++r)
                for (int c = 0; c <= 32; ++c) {
                    const float vx = sx * 32.0f + c, vy = sy * 32.0f + r, d = std::hypot(vx - gx, vy - gy);
                    if (d > R) continue;
                    const float t = 1 - d / R, w = t * t * (3 - 2 * t);
                    const float z = s.land[r][c].z / k;
                    float target = z;
                    switch (app.heightMode) {
                    case 0: target = z + app.heightStrength * dt * w; break;
                    case 1: target = z - app.heightStrength * dt * w; break;
                    case 2: { // the average around it (from the whole map: across sector edges)
                        const float avg = (m.HeightAt(vx - 1, vy) + m.HeightAt(vx + 1, vy) + m.HeightAt(vx, vy - 1) + m.HeightAt(vx, vy + 1)) / 4;
                        target = z + (avg - z) * std::min(1.0f, app.heightStrength * dt * w);
                        break;
                    }
                    default: target = z + (app.flattenLevel - z) * std::min(1.0f, app.heightStrength * dt * w); break;
                    }
                    const uint16_t nz = static_cast<uint16_t>(std::lround(std::clamp(target, 0.0f, m.maxZ) * k));
                    if (nz != s.land[r][c].z) { s.land[r][c].z = nz; changed = true; }
                }
            if (!changed) continue;
            if (!kept) app.stroke.sectors.push_back({si, before});
            touched.push_back(si);
        }
    for (int si : touched) { // normals from the heights around each vertex
        mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
        const int sx = si % m.sectorsX, sy = si / m.sectorsX;
        for (int r = 0; r <= 32; ++r)
            for (int c = 0; c <= 32; ++c) {
                const float vx = sx * 32.0f + c, vy = sy * 32.0f + r;
                const float dx = (m.HeightAt(vx + 1, vy) - m.HeightAt(vx - 1, vy)) / 2, dy = (m.HeightAt(vx, vy + 1) - m.HeightAt(vx, vy - 1)) / 2;
                const float len = std::sqrt(dx * dx + dy * dy + 1);
                const float nx = -dx / len, ny = -dy / len, nz = 1 / len;
                s.land[r][c].packedNormal = (static_cast<uint32_t>(std::lround(nz * 1000)) << 22) |
                                            (static_cast<uint32_t>(std::lround(nx * 1000 + 1000)) & 0x7FF) << 11 |
                                            (static_cast<uint32_t>(std::lround(ny * 1000 + 1000)) & 0x7FF);
            }
        app.terrainEditedSectors.insert(si);
    }
    if (!touched.empty()) app.terrainRebuild = true;
}

static bool HeightBrushInput(App& app, ImVec2 local) {
    if (!app.heightBrush || !app.terrainLoaded) { if (app.painting && !app.tileBrush) app.painting = false; return false; }
    fig::Vec3 g;
    const bool onGround = app.scene.GroundAt(local.x, local.y, g);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app.painting = true;
        app.stroke = EditStep{};
        app.stroke.kind = EditStep::Terrain;
        app.stroke.file = app.terrainPath;
        if (onGround) app.flattenLevel = app.terrain.HeightAt(g.x, g.y);
    }
    if (!app.painting) return false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (onGround) SculptAt(app, g.x, g.y, std::min(ImGui::GetIO().DeltaTime, 0.1f));
    } else {
        app.painting = false;
        static const char* const kNames[] = {"Raise terrain", "Lower terrain", "Smooth terrain", "Flatten terrain"};
        if (!app.stroke.sectors.empty()) PushUndo(app, std::move(app.stroke), kNames[std::clamp(app.heightMode, 0, 3)]);
        app.stroke = EditStep{};
    }
    return true;
}

// Keys while the brush is on: 1-8 take a quick tile, comma and period turn the tile.
static void HeightBrushKeys(App& app) { // F: radius, Shift+F: strength (while held, moving the mouse sideways)
    if (!app.heightBrush || ImGui::GetIO().WantTextInput || !ImGui::IsKeyDown(ImGuiKey_F)) return;
    const float dx = ImGui::GetIO().MouseDelta.x;
    if (ImGui::GetIO().KeyShift) app.heightStrength = std::clamp(app.heightStrength * std::pow(1.01f, dx), 0.2f, 20.0f);
    else app.heightRadius = std::clamp(app.heightRadius * std::pow(1.01f, dx), 1.0f, 32.0f);
}

static void TerrainBrushKeys(App& app) {
    if (!app.tileBrush || ImGui::GetIO().WantTextInput) return;
    for (int i = 0; i < 8; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false) && app.quickTiles[i] >= 0) app.brushTile = app.quickTiles[i];
    if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) app.brushRotation = (app.brushRotation + 3) % 4;
    if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) app.brushRotation = (app.brushRotation + 1) % 4;
}

// The outline of the tile the brush would paint.
static void TerrainBrushOutline(App& app, ImDrawList* draw, ImVec2 min, ImVec2 size) {
    if (!app.tileBrush || !app.terrainLoaded || !app.hoverGround) return;
    int si, row, col;
    if (!TileAt(app.terrain, app.ground.x, app.ground.y, si, row, col)) return;
    const float x0 = (si % app.terrain.sectorsX) * 32.0f + col * 2.0f, y0 = (si / app.terrain.sectorsX) * 32.0f + row * 2.0f;
    ImVec2 pts[4];
    const float cx[4] = {0, 2, 2, 0}, cy[4] = {0, 0, 2, 2};
    for (int i = 0; i < 4; ++i) {
        const float x = x0 + cx[i], y = y0 + cy[i];
        float fx, fy;
        if (!app.scene.Project({x, y, app.scene.Ground(x, y) + 0.05f}, fx, fy)) return;
        pts[i] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
    }
    draw->AddPolyline(pts, 4, app.brushWater ? IM_COL32(120, 200, 255, 255) : IM_COL32(255, 230, 120, 255), ImDrawFlags_Closed, 2.0f);
}

// One tile's picture (rotated as it will be painted when `rotation` is given).
static void TileImage(App& app, int tile, float side, int rotation = 0) {
    const GLuint tex = app.scene.TerrainTexture(tile / 64);
    ImVec2 uv0, uv1;
    TileUvs(tile, uv0, uv1);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    if (!tex) { ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + side, p.y + side), IM_COL32(120, 120, 120, 255)); return; }
    // The four corners' UVs turned by quarter turns (the same way the terrain turns them).
    ImVec2 c[4] = {ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y), ImVec2(uv0.x, uv1.y)};
    ImVec2 r[4];
    for (int i = 0; i < 4; ++i) r[i] = c[(i + rotation) % 4];
    ImGui::GetWindowDrawList()->AddImageQuad(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), p, ImVec2(p.x + side, p.y),
                                             ImVec2(p.x + side, p.y + side), ImVec2(p.x, p.y + side), r[0], r[1], r[2], r[3]);
}

// The terrain's tiles no sector uses (land or water), highest first: where a blended tile can go.
static void MarkUsedTiles(const mpr::Map& m, std::vector<bool>& used);
// The tiles no terrain using these textures uses: the open one and every .mpr of the map folders with the same
// name inside (zone3xobr.mpr is zone3obr inside: they share zone3obr000.mmp...), highest first.
static std::vector<int> FreeTiles(App& app) {
    const mpr::Map& m = app.terrain;
    std::vector<bool> used(static_cast<size_t>(std::max(m.textureCount, 0)) * 64, false);
    MarkUsedTiles(m, used);
    static std::map<std::string, std::vector<bool>> cache; // per .mpr path: its used tiles (read once)
    for (const Library::MapFile& f : app.lib.ListMapFiles()) {
        if (!f.terrain || f.path == app.terrainPath) continue;
        auto it = cache.find(f.path);
        if (it == cache.end()) {
            mpr::Map other;
            std::string err;
            std::vector<bool> u;
            if (mpr::Load(f.path, other, err) && other.name == m.name) { u.assign(used.size(), false); MarkUsedTiles(other, u); }
            it = cache.emplace(f.path, u).first;
        }
        for (size_t i = 0; i < it->second.size() && i < used.size(); ++i) if (it->second[i]) used[i] = true;
    }
    std::vector<int> free;
    for (int t = static_cast<int>(used.size()) - 1; t >= 0; --t) if (!used[static_cast<size_t>(t)]) free.push_back(t);
    return free;
}

static void MarkUsedTiles(const mpr::Map& m, std::vector<bool>& used) {
    for (const mpr::Sector& s : m.sectors) {
        if (!s.present) continue;
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 16; ++c) {
            const int lt = ((s.landTiles[r][c] >> 6) & 0xFF) * 64 + (s.landTiles[r][c] & 63);
            if (lt < static_cast<int>(used.size())) used[static_cast<size_t>(lt)] = true;
            if (s.water && s.waterMaterial[r][c] >= 0) {
                const int wt = ((s.waterTiles[r][c] >> 6) & 0xFF) * 64 + (s.waterTiles[r][c] & 63);
                if (wt < static_cast<int>(used.size())) used[static_cast<size_t>(wt)] = true;
            }
        }
    }
    for (const auto& a : m.animTiles) for (int k = 0; k < a.second; ++k) if (a.first + k < static_cast<int>(used.size())) used[static_cast<size_t>(a.first + k)] = true;
}

// Tile painting > Blend two tiles: the brush's tile (A) fading into tile B through a soft, slightly noisy mask,
// written into a free tile of the terrain's textures (in the texture source they come from), then painted with.
static void BlendPanel(App& app) {
    if (!ImGui::CollapsingHeader("Blend two tiles")) return;
    const mpr::Map& m = app.terrain;
    ImGui::TextWrapped("A soft transition from the brush's tile (A) to tile B, made into a new tile of the terrain's textures. "
                       "Paint it between the two grounds, turning it with %s / %s.", ",", ".");
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    TileImage(app, std::max(app.brushTile, 0), 48, 0);
    ImGui::SameLine();
    if (app.blendTileB >= 0) TileImage(app, app.blendTileB, 48, 0); else ImGui::Dummy(ImVec2(48, 48));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 2, p0.y + 2), IM_COL32(255, 255, 255, 230), "A");
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 58, p0.y + 2), IM_COL32(255, 255, 255, 230), "B");
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button("Brush tile -> B")) app.blendTileB = app.brushTile;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick tile B with the brush first, then this, then pick tile A");
    static const char* const kMasks[] = {"Edge (B on the right)", "Corner (B bottom right)", "Inner corner (A top left)"};
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("Shape", &app.blendMask, kMasks, 3);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Softness", &app.blendSoftness, 0.05f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Variation", &app.blendSeed);
    ImGui::EndGroup();
    const std::vector<int> free = FreeTiles(app);
    if (app.blendSlot < 0 || std::find(free.begin(), free.end(), app.blendSlot) == free.end()) app.blendSlot = free.empty() ? -1 : free.front();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Into tile", &app.blendSlot) && std::find(free.begin(), free.end(), app.blendSlot) == free.end()) app.blendSlot = free.empty() ? -1 : free.front();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A tile no sector of this terrain uses (%zu free). Careful: other terrains sharing these textures\n"
                          "(zone6x uses zone6's) may use it.", free.size());
    ImGui::SameLine();
    ImGui::TextDisabled("texture %d, tile %d", app.blendSlot / 64, app.blendSlot % 64);
    if (app.blendFolder.empty() && !app.terrainPath.empty())
        app.blendFolder = (std::filesystem::path(app.terrainPath).parent_path() / "blended-textures").string();
    char folder[512];
    std::snprintf(folder, sizeof folder, "%s", app.blendFolder.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##blendfolder", folder, sizeof folder)) app.blendFolder = folder;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The output folder of the blended textures (loose .mmp files)");
    ImGui::BeginDisabled(app.blendTileB < 0 || app.brushTile < 0 || app.blendSlot < 0 || app.blendFolder.empty());
    if (ImGui::Button("Make the tile")) {
        app.blendMessage.clear();
        auto texName = [&](int tile) { return m.name + "00" + std::to_string(tile / 64); };
        std::vector<uint8_t> ta, tb, tt;
        std::string found;
        if (!app.lib.textures.ReadTexture(texName(app.brushTile), ta) || !app.lib.textures.ReadTexture(texName(app.blendTileB), tb) ||
            !app.lib.textures.ReadTexture(texName(app.blendSlot), tt, &found))
            app.blendMessage = "The terrain's textures were not found in the texture sources";
        const blend::Texture ia = blend::Inspect(ta), ib = blend::Inspect(tb), it = blend::Inspect(tt);
        if (app.blendMessage.empty() && (!ia.ok || !ib.ok || !it.ok || ia.width != ib.width || ia.width != it.width))
            app.blendMessage = "Only DXT1 terrain textures of one size can be blended";
        int layer = app.lib.textures.LayerWith(found);
        if (app.blendMessage.empty() && app.blendFolder.empty()) app.blendMessage = "Choose the output folder first";
        if (app.blendMessage.empty()) {
            const int size = it.width / 8;
            const std::vector<uint8_t> mixed = blend::Mix(blend::ReadTile(ta, ia, app.brushTile % 64), blend::ReadTile(tb, ib, app.blendTileB % 64), size,
                                                          static_cast<blend::Mask>(app.blendMask), app.blendSoftness, static_cast<uint32_t>(app.blendSeed));
            blend::WriteTile(tt, it, app.blendSlot % 64, mixed);
            // Never into an archive (the game's own textures.res!): a loose .mmp in the output folder, which becomes the
            // top texture source so the editor shows it; pack it into the mod's textures archive for the game.
            std::error_code ec;
            std::filesystem::create_directories(app.blendFolder, ec);
            const std::string out = (std::filesystem::path(app.blendFolder) / found).string();
            {
                std::ofstream f(out, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(tt.data()), static_cast<std::streamsize>(tt.size()));
                if (!f) app.blendMessage = "Cannot write " + out;
            }
            bool listed = false;
            for (size_t li = 0; li < app.lib.textures.layers.size(); ++li) if (app.lib.textures.layers[li].path == app.blendFolder) { listed = true; layer = static_cast<int>(li); }
            if (!listed) { app.lib.textures.AddLayer(app.blendFolder); layer = static_cast<int>(app.lib.textures.layers.size()) - 1; }
            if (app.blendMessage.empty()) {
                app.lib.textures.ReloadLayer(static_cast<size_t>(layer));
                app.lib.RebuildTextureIndex();
                PushTerrainHeaderUndo(app);
                if (app.blendSlot < static_cast<int>(app.terrain.tileTypes.size()) && app.brushTile < static_cast<int>(app.terrain.tileTypes.size()))
                    app.terrain.tileTypes[static_cast<size_t>(app.blendSlot)] = app.terrain.tileTypes[static_cast<size_t>(app.brushTile)];
                app.terrainHeaderEdited = true;
                app.terrainDirty = true; // the textures again
                app.blendMessage = "Tile " + std::to_string(app.blendSlot) + " written into " + out + " (a texture source for this session; "
                                   "pack it into the mod's textures archive for the game): the brush paints with it now";
                app.brushTile = app.blendSlot;
                app.brushRotation = 0;
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the changed texture as a loose .mmp into the output folder (never into an archive), shown\n"
                          "here at once; pack it into the mod's textures archive for the game. The tile's type (footsteps)\n"
                          "is A's; save the terrain to keep it.");
    if (!app.blendMessage.empty()) ImGui::TextWrapped("%s", app.blendMessage.c_str());
}

static void TerrainTab(App& app) {
    if (!app.terrainLoaded) { ImGui::TextWrapped("Load a terrain (.mpr) in the Files tab to edit its tiles here."); return; }
    mpr::Map& m = app.terrain;
    ImGui::Text("%s", std::filesystem::path(app.terrainPath).filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d sectors, %d textures", m.sectorsX, m.sectorsY, m.textureCount);
    ImGui::BeginDisabled(!TerrainUnsaved(app));
    if (ImGui::Button("Save terrain")) SaveTerrain(app, app.terrainPath);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the edited sectors and the header into the .mpr (%s saves it with the maps)", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save terrain as...")) { app.terrainSaveAsOpen = !app.terrainSaveAsOpen; std::snprintf(app.terrainSaveAsPath, sizeof(app.terrainSaveAsPath), "%s", app.terrainPath.c_str()); }
    if (app.terrainSaveAsOpen) {
        ImGui::SetNextItemWidth(-70);
        ImGui::InputText("##mprsaveas", app.terrainSaveAsPath, sizeof(app.terrainSaveAsPath));
        ImGui::SameLine();
        std::error_code ec;
        const bool exists = std::filesystem::exists(app.terrainSaveAsPath, ec) && !std::filesystem::equivalent(app.terrainSaveAsPath, app.terrainPath, ec);
        ImGui::BeginDisabled(app.terrainSaveAsPath[0] == '\0');
        if (ImGui::Button(exists ? "Replace" : "Save")) { if (SaveTerrain(app, app.terrainSaveAsPath)) app.terrainSaveAsOpen = false; }
        ImGui::EndDisabled();
    }
    if (!app.terrainMessage.empty()) ImGui::TextDisabled("%s", app.terrainMessage.c_str());

    ImGui::SeparatorText("Height brush");
    if (ImGui::Checkbox("Shape the ground in the view", &app.heightBrush) && app.heightBrush) app.tileBrush = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Left drag on the ground (one undo step per stroke). Objects can't be selected meanwhile.\n"
                                                  "After shaping, rebuild the navmesh (Tools) so units walk the new ground.");
    if (app.heightBrush) {
        static const char* const kModes[] = {"Raise", "Lower", "Smooth", "Flatten"};
        for (int i = 0; i < 4; ++i) { if (i) ImGui::SameLine(); ImGui::RadioButton(kModes[i], &app.heightMode, i); }
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Radius", &app.heightRadius, 1.0f, 32.0f, "%.1f units");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Strength", &app.heightStrength, 0.2f, 20.0f, "%.1f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Raise / lower: units per second at the centre; smooth / flatten: how fast it gets there");
        ImGui::TextDisabled("Height range of this terrain: 0 to %.1f", app.terrain.maxZ);
    }

    ImGui::SeparatorText("Tile brush");
    if (ImGui::Checkbox("Paint tiles in the view", &app.tileBrush) && app.tileBrush) app.heightBrush = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Left drag: paint (one undo step per stroke); Alt+click: pick the tile under the mouse.\n"
            "Keys 1-8: quick tiles; comma / period: turn the tile. Objects can't be selected meanwhile.");
    ImGui::SameLine();
    int layer = app.brushWater ? 1 : 0;
    ImGui::RadioButton("Land", &layer, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Water", &layer, 1);
    app.brushWater = layer == 1;
    if (app.brushWater) {
        ImGui::SetNextItemWidth(-1);
        const std::string label = app.brushMaterial < 0 ? std::string("no water (removes it from the tile)") : "material " + std::to_string(app.brushMaterial);
        if (ImGui::BeginCombo("##brushmat", label.c_str())) {
            if (ImGui::Selectable("no water (removes it from the tile)", app.brushMaterial < 0)) app.brushMaterial = -1;
            for (size_t i = 0; i < m.materials.size(); ++i) {
                const mpr::Material& mt = m.materials[i];
                const std::string l = "material " + std::to_string(i) + (mt.type == 3 ? " (water)" : " (terrain)");
                if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == app.brushMaterial)) app.brushMaterial = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
    }
    TileImage(app, app.brushTile, 48, app.brushRotation);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("tile %d of texture %d", app.brushTile % 64, app.brushTile / 64);
    for (int r = 0; r < 4; ++r) {
        if (r) ImGui::SameLine();
        ImGui::RadioButton((std::to_string(r * 90) + "##rot").c_str(), &app.brushRotation, r);
    }
    if (app.brushTile < static_cast<int>(m.tileTypes.size())) {
        int type = m.tileTypes[static_cast<size_t>(app.brushTile)];
        ImGui::SetNextItemWidth(140);
        const char* preview = type >= 0 && type < 16 ? kTileTypeNames[type] : "?";
        if (ImGui::BeginCombo("type##tiletype", preview)) {
            for (int t = 0; t < 16; ++t)
                if (ImGui::Selectable(kTileTypeNames[t], t == type)) {
                    PushTerrainHeaderUndo(app);
                    m.tileTypes[static_cast<size_t>(app.brushTile)] = t;
                    app.terrainHeaderEdited = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the game makes of this tile (footsteps, walking): saved in the terrain's header");
    }
    ImGui::EndGroup();
    // Quick tiles: click takes one, right-click keeps the brush's tile in it.
    ImGui::TextDisabled("Quick tiles (keys 1-8; right-click keeps the brush's tile):");
    for (int i = 0; i < 8; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (app.quickTiles[i] >= 0) TileImage(app, app.quickTiles[i], 32);
        else ImGui::Dummy(ImVec2(32, 32));
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + 32, p.y + 32), app.quickTiles[i] == app.brushTile && app.quickTiles[i] >= 0 ? IM_COL32(255, 220, 120, 255) : IM_COL32(90, 90, 90, 255));
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("##q", ImVec2(32, 32), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && app.quickTiles[i] >= 0) app.brushTile = app.quickTiles[i];
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.quickTiles[i] = app.brushTile;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d", i + 1);
        ImGui::PopID();
    }
    BlendPanel(app);
    // The terrain's textures, 8 x 8 tiles each: click a tile to paint with it.
    for (int t = 0; t < m.textureCount; ++t) {
        const GLuint tex = app.scene.TerrainTexture(t);
        if (!ImGui::TreeNodeEx(("Texture " + std::to_string(t) + "##tex" + std::to_string(t)).c_str(), t == app.brushTile / 64 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            continue;
        if (!tex) { ImGui::TextDisabled("(not found in the texture sources)"); ImGui::TreePop(); continue; }
        const float side = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(side, side));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cell = side / 8.0f;
        for (int i = 1; i < 8; ++i) {
            dl->AddLine(ImVec2(p.x + i * cell, p.y), ImVec2(p.x + i * cell, p.y + side), IM_COL32(0, 0, 0, 90));
            dl->AddLine(ImVec2(p.x, p.y + i * cell), ImVec2(p.x + side, p.y + i * cell), IM_COL32(0, 0, 0, 90));
        }
        if (app.brushTile / 64 == t) {
            const int bt = app.brushTile % 64, cx = bt % 8, cy = 7 - bt / 8;
            dl->AddRect(ImVec2(p.x + cx * cell, p.y + cy * cell), ImVec2(p.x + (cx + 1) * cell, p.y + (cy + 1) * cell), IM_COL32(255, 220, 120, 255), 0, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const int cx = std::min(7, static_cast<int>((mp.x - p.x) / cell)), cy = std::min(7, static_cast<int>((mp.y - p.y) / cell));
            const int tile = t * 64 + (7 - cy) * 8 + cx;
            const int type = tile < static_cast<int>(m.tileTypes.size()) ? m.tileTypes[static_cast<size_t>(tile)] : -1;
            ImGui::SetTooltip("tile %d (%s)", tile % 64, type >= 0 && type < 16 ? kTileTypeNames[type] : "?");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { app.brushTile = tile; app.tileBrush = true; }
        }
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Materials");
    ImGui::TextDisabled("Liquid materials (water tiles use them) and their look; changes are one undo step each.");
    int removeMat = -1;
    for (size_t i = 0; i < m.materials.size(); ++i) {
        mpr::Material& mt = m.materials[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%zu", i);
        ImGui::SameLine(30);
        int type = mt.type == 3 ? 1 : 0;
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("##type", &type, "terrain\0water\0")) { PushTerrainHeaderUndo(app); mt.type = type ? 3 : 1; app.terrainHeaderEdited = true; }
        ImGui::SameLine();
        float col[4] = {mt.r, mt.g, mt.b, mt.a};
        ImGui::SetNextItemWidth(-30);
        if (ImGui::ColorEdit4("##rgba", col, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
            mt.r = col[0]; mt.g = col[1]; mt.b = col[2]; mt.a = col[3];
            app.terrainHeaderEdited = true;
            app.terrainRebuild = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeMat = static_cast<int>(i);
        float v3[3] = {mt.selfIllumination, mt.waveMultiplier, mt.warpSpeed};
        ImGui::Indent(30);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##params", v3, 0.01f, 0.0f, 0.0f, "%.3f")) {
            mt.selfIllumination = v3[0]; mt.waveMultiplier = v3[1]; mt.warpSpeed = v3[2];
            app.terrainHeaderEdited = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Self-illumination, wave (wind) multiplier, warp speed");
        ImGui::Unindent(30);
        ImGui::PopID();
    }
    if (removeMat >= 0) {
        // Water tiles using it lose their water, those after it follow the renumbering.
        PushTerrainHeaderUndo(app);
        EditStep& header = app.undoSteps.back();
        for (size_t si = 0; si < m.sectors.size(); ++si) {
            mpr::Sector& s = m.sectors[si];
            if (!s.water) continue;
            bool touched = false;
            for (auto& rowv : s.waterMaterial) for (int16_t& v : rowv) if (v >= removeMat) touched = true;
            if (!touched) continue;
            header.sectors.push_back({static_cast<int>(si), s});
            for (auto& rowv : s.waterMaterial)
                for (int16_t& v : rowv) { if (v == removeMat) v = -1; else if (v > removeMat) --v; }
            app.terrainEditedSectors.insert(static_cast<int>(si));
        }
        m.materials.erase(m.materials.begin() + removeMat);
        app.terrainHeaderEdited = true;
        app.terrainRebuild = true;
        if (app.brushMaterial >= static_cast<int>(m.materials.size())) app.brushMaterial = static_cast<int>(m.materials.size()) - 1;
    }
    if (ImGui::SmallButton("+ material")) {
        PushTerrainHeaderUndo(app);
        mpr::Material mt;
        mt.type = 3;
        mt.r = 0.2f; mt.g = 0.35f; mt.b = 0.5f; mt.a = 0.7f;
        m.materials.push_back(mt);
        app.terrainHeaderEdited = true;
    }

    ImGui::SeparatorText("Animated tiles");
    ImGui::TextDisabled("A first tile and how many tiles after it the game cycles through.");
    int removeAnim = -1;
    for (size_t i = 0; i < m.animTiles.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        int v[2] = {m.animTiles[i].first, m.animTiles[i].second};
        ImGui::SetNextItemWidth(-30);
        if (ImGui::InputInt2("##anim", v)) {
            m.animTiles[i] = {std::max(0, v[0]), std::max(1, v[1])};
            app.terrainHeaderEdited = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAnim = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAnim >= 0) { PushTerrainHeaderUndo(app); m.animTiles.erase(m.animTiles.begin() + removeAnim); app.terrainHeaderEdited = true; }
    if (ImGui::SmallButton("+ animated tile")) { PushTerrainHeaderUndo(app); m.animTiles.push_back({app.brushTile, 4}); app.terrainHeaderEdited = true; }
}

static void Sidebar(App& app, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PanelBg());
    ImGui::BeginChild("##mapSidebar", ImVec2(width, height), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##maptabs")) {
        const SideTab requested = app.requestTab;
        app.requestTab = SideTab::Count;
        auto tab = [&](SideTab t, const char* label, void (*draw)(App&)) {
            if (ImGui::BeginTabItem(label, nullptr, requested == t ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (app.lib.mapSideTab != static_cast<int>(t) && requested == SideTab::Count) { // remembered for the next start
                    app.lib.mapSideTab = static_cast<int>(t);
                    app.lib.SaveConfig();
                }
                ImGui::BeginChild("##tab", ImVec2(0, 0));
                draw(app);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        };
        tab(SideTab::Files, "Files", FilesTab);
        tab(SideTab::Objects, "Objects", ObjectsTab);
        std::string checksLabel = "Checks";
        if (app.summary.errors || app.summary.warnings)
            checksLabel += " (" + std::to_string(app.summary.errors) + "/" + std::to_string(app.summary.warnings) + ")";
        checksLabel += "###checks";
        tab(SideTab::Checks, checksLabel.c_str(), ChecksTab);
        tab(SideTab::Script, "Script", ScriptTab);
        tab(SideTab::Quest, "Quest", QuestTab);
        tab(SideTab::Diplomacy, "Diplomacy", DiplomacyTab);
        tab(SideTab::Ids, "IDs", IdsTab);
        tab(SideTab::Terrain, TerrainUnsaved(app) ? "Terrain*###terrain" : "Terrain###terrain", TerrainTab);
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The lighting files of the Settings tab, read again when that list changes.
static void RefreshLighting(App& app) {
    if (app.lightingFor == app.lib.lightingFiles) return;
    app.lightingFor = app.lib.lightingFiles;
    app.lightTables.clear();
    for (const std::string& path : lighting::Expand(app.lib.lightingFiles)) {
        lighting::Table t;
        if (lighting::Load(path, t)) app.lightTables.push_back(std::move(t));
    }
}

static const lighting::Table* ChosenLighting(const App& app) {
    for (const lighting::Table& t : app.lightTables) if (t.path == app.lib.lightingChoice) return &t;
    return app.lightTables.empty() ? nullptr : &app.lightTables.front();
}

// The time of day: the hour slider once moved, else the first loaded map's WORLD_SET time.
static float MapHour(const App& app) {
    if (app.hourSet) return app.hour;
    for (auto& m : app.mobs) if (m->file.hasWorld) return m->file.worldTime;
    return 12.0f;
}

static void ApplyLighting(App& app) {
    SceneLight& l = app.scene.light;
    const lighting::Table* t = ChosenLighting(app);
    l = SceneLight{};
    if (!app.lib.lightingOn || !t) return;
    float h = MapHour(app);
    lighting::Color sun = lighting::Table::At(t->sun, h), amb = lighting::Table::At(t->ambient, h), sky = lighting::Table::At(t->sky, h);
    l.on = true;
    // The file's colours scaled like a light and an ambient term; the fixed-function sum stays near white at noon.
    const float sunScale = 0.8f, ambScale = 0.55f;
    l.sun[0] = sun.r * sunScale; l.sun[1] = sun.g * sunScale; l.sun[2] = sun.b * sunScale;
    l.ambient[0] = amb.r * ambScale; l.ambient[1] = amb.g * ambScale; l.ambient[2] = amb.b * ambScale;
    l.sky[0] = sky.r; l.sky[1] = sky.g; l.sky[2] = sky.b;
    // Where the sun is at that hour: rising in the east at 6, over the south at noon (70 degrees up),
    // setting in the west at 18. At night it stays low (the light files' night colours are dim) and casts
    // no shadows.
    const float day = std::fmod(h + 24.0f, 24.0f);
    const float across = std::min(std::max((day - 6.0f) / 12.0f, 0.0f), 1.0f); // 0 sunrise .. 1 sunset
    const float pi = 3.14159265f;
    const float elevation = std::max(std::sin(pi * across) * 70.0f, 8.0f) * pi / 180.0f;
    const float azimuth = pi * across;
    l.sunDir[0] = std::cos(azimuth) * std::cos(elevation);
    l.sunDir[1] = -std::sin(azimuth) * std::cos(elevation);
    l.sunDir[2] = std::sin(elevation);
    l.sunUp = day > 6.3f && day < 17.7f;
}

// Clears the patrol points of the selected units' logic in use (ei_maper's "Reset logic paths"). One undo step.
static void ResetLogicPaths(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    int units = 0;
    for (int oi : app.scene.selection) {
        if (oi < 0 || oi >= static_cast<int>(f.objects.size()) || f.objects[oi].kind != mob::Kind::Unit) continue;
        bool cleared = false;
        for (size_t gi = 0; gi < f.objects[oi].logics.size(); ++gi) {
            mob::Logic g = f.objects[oi].logics[gi];
            if (!g.use || g.patrol.empty()) continue;
            g.patrol.clear();
            cleared |= mob::SetLogic(f, oi, static_cast<int>(gi), g);
        }
        units += cleared;
    }
    if (!units) { app.questMessage = "No selected unit has a patrol path"; return; }
    PushUndo(app, std::move(step), "Clear patrol paths");
    app.scene.logicPoints.clear();
    app.checksDirty = true;
    app.listedKey.clear();
    app.questMessage = "Cleared the patrol paths of " + std::to_string(units) + " unit(s)";
}

// Writes the active map to another file, which it then is (its undo steps follow it).
static void SaveAsWindow(App& app) {
    if (!app.saveAsOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Save active MOB as", &app.saveAsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    ImGui::Text("%s", m.file.path.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##saveas", app.saveAsPath, sizeof(app.saveAsPath));
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path to(app.saveAsPath);
    const bool same = fs::equivalent(to, m.file.path, ec);
    const bool exists = !same && fs::exists(to, ec);
    if (exists) ImGui::Checkbox("Replace the existing file", &app.saveAsOverwrite);
    ImGui::BeginDisabled(app.saveAsPath[0] == '\0' || same || (exists && !app.saveAsOverwrite) || to.extension().string().empty());
    if (ImGui::Button("Save", ImVec2(120, 0))) {
        const std::string oldPath = m.file.path;
        mob::File copy = m.file;
        copy.path = to.string();
        std::string err;
        if (!mob::Save(copy, err)) {
            app.saveAsMessage = "Not saved: " + err;
        } else {
            m.file.path = copy.path;
            m.file.fileName = to.filename().string();
            m.savedBytes = m.file.bytes;
            // Everything that names the map by its path follows it.
            for (auto* stack : {&app.undoSteps, &app.redoSteps})
                for (EditStep& st : *stack) if (st.file == oldPath) st.file = m.file.path;
            if (app.xf.file == oldPath) app.xf.file = m.file.path;
            if (app.scriptEditFor == oldPath) app.scriptEditFor = m.file.path;
            if (app.scriptExtMob == oldPath) app.scriptExtMob = m.file.path;
            app.saveAsMessage = "Saved as " + m.file.path;
            app.saveAsOverwrite = false;
        }
    }
    ImGui::EndDisabled();
    if (same) ImGui::TextDisabled("That is the map's own file: save it with %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    if (!app.saveAsMessage.empty()) ImGui::TextWrapped("%s", app.saveAsMessage.c_str());
    ImGui::End();
}

// The undo history: every step Ctrl+Z can take back (oldest first) and those Ctrl+Y can do again (grey).
// A click goes back or forward to just after that step.
static void HistoryWindow(App& app) {
    if (!app.historyOpen) return;
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMax.x - 340, app.viewportMin.y + 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo history", &app.historyOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::TextDisabled("Click a step to go back or forward to it.");
    int target = -2; // steps to keep done: -1 none
    if (ImGui::Selectable("(start)", app.undoSteps.empty())) target = 0;
    for (size_t i = 0; i < app.undoSteps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool current = i + 1 == app.undoSteps.size();
        if (ImGui::Selectable(app.undoSteps[i].label.c_str(), current)) target = static_cast<int>(i) + 1;
        if (current) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (size_t j = app.redoSteps.size(); j-- > 0;) { // the next to redo first
        ImGui::PushID(1000000 + static_cast<int>(j));
        if (ImGui::Selectable(app.redoSteps[j].label.c_str(), false))
            target = static_cast<int>(app.undoSteps.size() + (app.redoSteps.size() - j));
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    ImGui::End();
    if (target < 0) return;
    for (int guard = 0; guard < 700; ++guard) {
        const size_t done = app.undoSteps.size(), undone = app.redoSteps.size();
        if (static_cast<int>(done) > target) UndoRedo(app, false);
        else if (static_cast<int>(done) < target && undone) UndoRedo(app, true);
        else break;
        if (app.undoSteps.size() == done && app.redoSteps.size() == undone) break; // nothing could be applied
    }
}

// Writes one WORLD_SET value in place (fixed sizes). One undo step.
static void CommitWorld(App& app, uint32_t type, const float* v, int n, const char* what) {
    mob::File& f = app.mobs[app.activeMob]->file;
    const size_t at = mob::WorldSetField(f, type);
    if (!at || at + static_cast<size_t>(n) * 4 > f.bytes.size()) return;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::memcpy(f.bytes.data() + at, v, static_cast<size_t>(n) * 4);
    mob::Reparse(f);
    PushUndo(app, std::move(step), what);
}

// ei_maper's MOB parameters: the map's kind and its WORLD_SET (wind, time of day, ambient and sun light),
// with the ID ranges, diplomacy and script a click away.
static void MobParamsWindow(App& app) {
    if (!app.mobParamsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MOB parameters", &app.mobParamsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    mob::File& f = app.mobs[app.activeMob]->file;
    ImGui::Text("%s", f.fileName.c_str());
    {
        bool quest = mob::IsQuestMob(f);
        if (ImGui::Checkbox("Quest MOB", &quest)) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::SetQuestMob(f, quest)) { PushUndo(app, std::move(step), quest ? "Quest MOB on" : "Quest MOB off"); app.checksDirty = true; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: a quest's map, loaded over a zone (PR_OBJECT_DB_FILE). Off: a zone's own map (SC_OBJECT_DB_FILE)");
    }
    ImGui::SeparatorText("World (WORLD_SET)");
    if (!f.hasWorld) {
        ImGui::TextWrapped("This map has no WORLD_SET (quest maps take the zone's). A zone's own map needs one.");
        if (ImGui::Button("Add a WORLD_SET")) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::AddWorldSet(f)) PushUndo(app, std::move(step), "Add a WORLD_SET");
        }
    } else if (ImGui::BeginTable("##world", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        float v[3];
        Label("Wind direction");
        { const float cur[3] = {f.worldWindDir.x, f.worldWindDir.y, f.worldWindDir.z}; if (EditFloats("##wdir", 3, cur, v)) CommitWorld(app, mob::kWorldWindDir, v, 3, "Wind direction"); }
        Label("Wind strength");
        { const float cur[1] = {f.worldWindStr}; if (EditFloats("##wstr", 1, cur, v)) CommitWorld(app, mob::kWorldWindStr, v, 1, "Wind strength"); }
        Label("Time (hours)");
        { const float cur[1] = {f.worldTime}; if (EditFloats("##wtime", 1, cur, v, "%.2f")) CommitWorld(app, mob::kWorldTime, v, 1, "Time of day"); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The map's time of day, 0 to 24 (the lighting follows it)");
        Label("Ambient");
        { const float cur[1] = {f.worldAmbient}; if (EditFloats("##wamb", 1, cur, v)) CommitWorld(app, mob::kWorldAmbient, v, 1, "Ambient"); }
        Label("Sun light");
        { const float cur[1] = {f.worldSunLight}; if (EditFloats("##wsun", 1, cur, v)) CommitWorld(app, mob::kWorldSunLight, v, 1, "Sun light"); }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Also");
    if (ImGui::Button("ID ranges")) app.requestTab = SideTab::Ids;
    ImGui::SameLine();
    if (ImGui::Button("Diplomacy")) app.requestTab = SideTab::Diplomacy;
    ImGui::SameLine();
    if (ImGui::Button("Script")) { if (app.scriptWindow) app.focusScriptWindow = true; else app.requestTab = SideTab::Script; }
    ImGui::End();
}

// ---- patrol simulation --------------------------------------------------------------------------------
// Units with a patrol path walk it: at their walking speed (stats, per tick, 15 ticks a second), along the
// shortest route around what blocks them (the scene's walkability grid) smoothed where the way is clear, turning to each look point of a patrol point and waiting its time there; then on to
// the next point (back to the first when cyclic, else back along the path). Nothing is written to the map.

static const char* kSimLimits =
    "Units walk their patrol paths like in the game:\n"
        "- routes: the shortest way on the game's walkable tiles (Layers > Walkability); units do not avoid\n"
        "  one another; without a terrain they walk straight\n"
        "- speed: the unit's walking speed (15 ticks a second); no running\n"
        "- no fighting, calls for help, alarms or scripts; guards and sentries stay put\n"
        "- nothing is written to the map; editing waits until the simulation stops";

// A* over the walkability grid (8 directions, diagonals only between free sides), then the corners that
// are needed: a straight line between two points is kept when every cell it crosses is free. A start or
// target in a blocked cell (a unit against a wall) searches from the nearest free cell.
static std::vector<mob::Vec3> FindRoute(const MapScene::WalkGrid& g, mob::Vec3 from, mob::Vec3 to) {
    const std::vector<mob::Vec3> straight{to};
    if (g.w == 0) return straight;
    auto cellOf = [&](float v, int n) { return std::min(std::max(static_cast<int>(v / g.cell), 0), n - 1); };
    auto nearestFree = [&](int& x, int& y) {
        if (!g.Blocked(x, y)) return true;
        for (int r = 1; r <= 32; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if ((std::abs(dx) == r || std::abs(dy) == r) && !g.Blocked(x + dx, y + dy)) { x += dx; y += dy; return true; }
        return false;
    };
    int sx = cellOf(from.x, g.w), sy = cellOf(from.y, g.h), tx = cellOf(to.x, g.w), ty = cellOf(to.y, g.h);
    if (!nearestFree(sx, sy) || !nearestFree(tx, ty) || (sx == tx && sy == ty)) return straight;
    const int n = g.w * g.h;
    std::vector<float> cost(static_cast<size_t>(n), 1e30f);
    std::vector<int> back(static_cast<size_t>(n), -1);
    std::vector<char> closed(static_cast<size_t>(n), 0);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto h = [&](int x, int y) { const int dx = std::abs(x - tx), dy = std::abs(y - ty); return static_cast<float>(std::max(dx, dy)) + 0.4142f * std::min(dx, dy); };
    const int start = sy * g.w + sx, goal = ty * g.w + tx;
    cost[static_cast<size_t>(start)] = 0;
    open.push({h(sx, sy), start});
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int visited = 0;
    while (!open.empty() && visited < 3000000) {
        const int cur = open.top().second;
        open.pop();
        if (closed[static_cast<size_t>(cur)]) continue;
        closed[static_cast<size_t>(cur)] = 1;
        ++visited;
        if (cur == goal) break;
        const int cx = cur % g.w, cy = cur / g.w;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + dxs[d], ny = cy + dys[d];
            if (g.Blocked(nx, ny)) continue;
            if (d >= 4 && (g.Blocked(cx + dxs[d], cy) || g.Blocked(cx, cy + dys[d]))) continue; // no cutting corners
            const int ni = ny * g.w + nx;
            const float nc = cost[static_cast<size_t>(cur)] + (d >= 4 ? 1.4142f : 1.0f) * g.Cost(nx, ny); // the game's step cost
            if (nc < cost[static_cast<size_t>(ni)]) {
                cost[static_cast<size_t>(ni)] = nc;
                back[static_cast<size_t>(ni)] = cur;
                open.push({nc + h(nx, ny), ni});
            }
        }
    }
    if (back[static_cast<size_t>(goal)] < 0 && goal != start) return straight; // no way there: straight
    std::vector<mob::Vec3> cells;
    for (int c = goal; c != start && c >= 0; c = back[static_cast<size_t>(c)])
        cells.push_back({(c % g.w + 0.5f) * g.cell, (c / g.w + 0.5f) * g.cell, 0});
    std::reverse(cells.begin(), cells.end());
    cells.push_back(to);
    auto clear = [&](mob::Vec3 a, mob::Vec3 b) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        const int steps = std::max(1, static_cast<int>(len / (g.cell * 0.25f)));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            if (g.Blocked(cellOf(a.x + (b.x - a.x) * t, g.w), cellOf(a.y + (b.y - a.y) * t, g.h))) return false;
        }
        return true;
    };
    std::vector<mob::Vec3> route;
    mob::Vec3 at = from;
    size_t i = 0;
    while (i < cells.size()) {
        size_t reach = i; // "far" is a macro in the Windows headers
        for (size_t j = cells.size(); j-- > i + 1;) if (clear(at, cells[j])) { reach = j; break; }
        route.push_back(cells[reach]);
        at = cells[reach];
        i = reach + 1;
    }
    return route;
}

static float AngleTo(float from, float to) { // the shortest turn from one heading to another
    float d = std::fmod(to - from + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return d - 3.14159265f;
}

// The figures face along their local -Y (a heading of 0 looks along the world's -Y).
static float HeadingTo(float dx, float dy) { return std::atan2(dx, -dy); }

static void StartSimulation(App& app) {
    app.sim.clear();
    app.scene.simPoses.clear();
    app.scene.BuildWalkGrid(); // the ground as it is now
    for (auto& m : app.mobs) {
        if (!m->visible) continue;
        const mob::File& f = m->file;
        for (size_t oi = 0; oi < f.objects.size(); ++oi) {
            const mob::Object& o = f.objects[oi];
            if (o.kind != mob::Kind::Unit) continue;
            for (const mob::Logic& g : o.logics) {
                if (!g.use || g.model != 2 || g.patrol.empty()) continue;
                App::SimUnit u;
                u.object = &o;
                u.logic = g;
                u.x = o.position.x;
                u.y = o.position.y;
                u.yaw = 2.0f * std::atan2(o.rotation[3], o.rotation[0]);
                float walk = 0.0f;
                if (o.stats.size() >= 32) std::memcpy(&walk, o.stats.data() + 28, 4);
                u.speed = walk > 0.01f && walk < 3.0f ? walk * 15.0f : 3.5f; // units a second
                int ai = o.stats.size() >= 57 ? o.stats[56] : 1;
                u.layer = std::min(std::max(ai, 0), mob::kAiLayers - 1);
                app.sim.push_back(std::move(u));
                break;
            }
        }
    }
    app.simulating = true;
    app.simPaused = false;
}

static void StopSimulation(App& app) {
    app.simulating = false;
    app.sim.clear();
    app.scene.simPoses.clear();
}

static void StepSimulation(App& app, float dt) {
    if (!app.simulating) return;
    // A map was unloaded or changed under it: the units it knew may be gone.
    size_t objects = 0;
    for (auto& m : app.mobs) objects += m->file.objects.size();
    if (objects != app.simObjects) { if (app.simObjects) { StopSimulation(app); return; } app.simObjects = objects; }
    if (app.simPaused) dt = 0;
    const float turnDefault = 3.14159265f; // radians a second when a look point says 0
    for (App::SimUnit& u : app.sim) {
        float left = dt;
        for (int guard = 0; guard < 16 && left > 0; ++guard) {
            const auto& pts = u.logic.patrol;
            const mob::PatrolPoint& target = pts[static_cast<size_t>(u.point)];
            if (u.phase == App::SimUnit::Walk) {
                if (u.route.empty()) {
                    u.route = FindRoute(app.scene.walk, {u.x, u.y, 0}, target.position);
                    u.leg = 0;
                }
                const mob::Vec3 next = u.route[static_cast<size_t>(u.leg)];
                const float dx = next.x - u.x, dy = next.y - u.y, dist = std::hypot(dx, dy);
                if (dist > 0.01f) {
                    // Turn towards the way first (quickly), then walk.
                    const float want = HeadingTo(dx, dy), turn = AngleTo(u.yaw, want), maxTurn = 2.0f * turnDefault * left;
                    u.yaw += std::max(-maxTurn, std::min(maxTurn, turn));
                }
                const float step = u.speed * left;
                if (step < dist) { u.x += dx / dist * step; u.y += dy / dist * step; left = 0; break; }
                u.x = next.x;
                u.y = next.y;
                left -= u.speed > 0 ? dist / u.speed : left;
                if (++u.leg >= static_cast<int>(u.route.size())) { u.route.clear(); u.phase = App::SimUnit::Look; u.look = 0; u.timer = 0; }
            } else { // at a patrol point: each look point in turn, turning to it then waiting its time
                if (u.look >= static_cast<int>(target.looks.size())) {
                    const int n = static_cast<int>(pts.size());
                    if (n > 1) {
                        if (u.logic.cyclic) u.point = (u.point + 1) % n;
                        else {
                            if (u.point + u.dir < 0 || u.point + u.dir >= n) u.dir = -u.dir;
                            u.point += u.dir;
                        }
                    }
                    u.phase = App::SimUnit::Walk;
                    if (n == 1) { u.look = 0; } // one point: look around it again and again
                    continue;
                }
                const mob::LookPoint& l = target.looks[static_cast<size_t>(u.look)];
                const float want = HeadingTo(l.position.x - u.x, l.position.y - u.y), turn = AngleTo(u.yaw, want);
                const float speed = l.turnSpeed > 0.001f ? l.turnSpeed * 15.0f : turnDefault;
                if (std::fabs(turn) > 0.01f && u.timer == 0) {
                    const float can = speed * left;
                    if (std::fabs(turn) > can) { u.yaw += turn > 0 ? can : -can; left = 0; break; }
                    u.yaw = want;
                    left -= std::fabs(turn) / speed;
                }
                const float wait = l.wait / 15.0f;
                if (u.timer + left < wait) { u.timer += left; left = 0; break; }
                left -= wait - u.timer;
                u.timer = 0;
                ++u.look;
            }
        }
        { // moving: it changed place since the last step (its walk animation plays; else its idle one)
            auto prev = app.scene.simPoses.find(u.object);
            const bool moving = prev != app.scene.simPoses.end() && std::hypot(prev->second.x - u.x, prev->second.y - u.y) > 1e-4f;
            app.scene.simPoses[u.object] = {u.x, u.y, u.yaw, moving};
        }
    }
}

// The small window of a running simulation: pause, speed, stop.
static void SimulationWindow(App& app) {
    if (!app.simulating) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMax.y - 110), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Patrol simulation", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%zu unit(s) on patrol%s", app.sim.size(), app.scene.walk.w ? "" : " (no terrain loaded: straight lines)");
        if (ImGui::Button(app.simPaused ? "Play" : "Pause", ImVec2(70, 0))) app.simPaused = !app.simPaused;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##simspeed", &app.simSpeed, 0.25f, 8.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) open = false;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSimLimits);
    }
    ImGui::End();
    if (!open) StopSimulation(app);
}

// The Tools menu: exact offsets, random values, the minimap, saving the active map elsewhere, patrol paths.
static void ToolsMenu(App& app) {
    const bool selection = !app.scene.selection.empty() && app.scene.selectedFile == app.activeMob;
    ImGui::SetNextItemWidth(90);
    if (!ImGui::BeginCombo("##tools", "Tools", ImGuiComboFlags_HeightLarge)) return;
    if (ImGui::Selectable("Offset...", false, selection ? 0 : ImGuiSelectableFlags_Disabled)) app.offsetOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move the selection along one axis by an exact value");
    if (ImGui::Selectable("Randomize...", false, selection ? 0 : ImGuiSelectableFlags_Disabled)) app.randomOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Give each selected object a random position, rotation or complection in a range (like ei_maper)");
    if (ImGui::Selectable("Minimap...", false, app.terrainLoaded ? 0 : ImGuiSelectableFlags_Disabled)) {
        app.minimapOpen = true;
        if (app.minimapPath[0] == '\0') {
            std::string dir = app.lib.gif.lastDirectory.empty() ? config::ExeDir() : app.lib.gif.lastDirectory;
            // Named after the .mpr file (zone6x.mpr -> zone6xmap), not the name inside it (which may be zone6).
            std::string stem = std::filesystem::path(app.terrainPath).stem().string();
            for (char& ch : stem) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::snprintf(app.minimapPath, sizeof(app.minimapPath), "%s/%smap.mmp", dir.c_str(), stem.c_str());
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Export a minimap texture of the terrain, like ZoneView's <zone>map");
    if (ImGui::Selectable("New terrain...", false, app.terrainLoaded ? 0 : ImGuiSelectableFlags_Disabled)) {
        app.newTerrainOpen = true;
        if (app.newTerrainPath[0] == '\0') {
            const std::string dir = std::filesystem::path(app.terrainPath).parent_path().string();
            std::snprintf(app.newTerrainPath, sizeof(app.newTerrainPath), "%s/newzone.mpr", dir.c_str());
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new flat terrain (.mpr) from scratch, with the open terrain's textures and materials");
    ImGui::Separator();
    if (ImGui::Selectable("MOB parameters...", false, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) app.mobParamsOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The active map's kind and WORLD_SET (wind, time, ambient, sun), like ei_maper's");
    if (ImGui::Selectable("Undo history", app.historyOpen, ImGuiSelectableFlags_DontClosePopups)) app.historyOpen = !app.historyOpen;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A window listing the steps undo and redo can take; a click goes to one");
    if (ImGui::Selectable(app.simulating ? "Stop the patrol simulation" : "Simulate patrols", app.simulating, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
        if (app.simulating) StopSimulation(app);
        else { app.simObjects = 0; StartSimulation(app); }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kSimLimits);
    // The navmesh again now, even without an edit (Save then writes it).
    bool hasGraph = false;
    for (auto& m : app.mobs) hasGraph |= m->file.aiGraphBytes != 0;
    if (ImGui::Selectable("Rebuild navmesh", false, hasGraph && app.terrain.sectorsX > 0 ? 0 : ImGuiSelectableFlags_Disabled)) {
        std::vector<std::vector<uint8_t>> before;
        for (auto& m : app.mobs) before.push_back(m->file.bytes);
        std::string msg = RegenerateNavmeshes(app);
        bool changed = false;
        for (size_t i = 0; i < app.mobs.size(); ++i) changed |= app.mobs[i]->file.bytes != before[i];
        app.filesMessage = msg + (changed ? ": save to write it" : " (it was already up to date)");
        if (app.scene.options.navCompare) BuildCompareNavmesh(app);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Builds the navmesh (AI_GRAPH) of the open zone map again from the terrain and the open maps' objects,\n"
                          "as the game does, without an edit first. Save writes it. Needs a terrain and a map that has a navmesh.");
    ImGui::Separator();
    const std::string resetLabel = "Clear patrol paths (" + ui::BindName(app.lib.mapKeys[config::kKeyResetPaths]) + ")";
    if (ImGui::Selectable(resetLabel.c_str(), false, selection ? 0 : ImGuiSelectableFlags_Disabled)) ResetLogicPaths(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Removes every patrol point of the selected units (ei_maper's Reset logic paths)");
    if (ImGui::Selectable("Save active MOB as...", false, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
        app.saveAsOpen = true;
        app.saveAsMessage.clear();
        std::snprintf(app.saveAsPath, sizeof(app.saveAsPath), "%s", app.mobs[app.activeMob]->file.path.c_str());
    }
    ImGui::EndCombo();
}

static void Toolbar(App& app) {
    MapViewOptions& o = app.scene.options;
    // What is drawn (the layers), in a dropdown to keep the bar short; its label says what is hidden.
    {
        const int hidden = !o.terrain + !o.water + !o.objects + !o.units + !o.markers;
        const std::string label = hidden ? "Layers (" + std::to_string(5 - hidden) + "/5)" : std::string("Layers");
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##show", label.c_str(), ImGuiComboFlags_HeightLarge)) {
            ImGui::Checkbox("Terrain", &o.terrain);
            ImGui::Checkbox("Water", &o.water);
            ImGui::Checkbox("Objects", &o.objects);
            ImGui::Checkbox("Units", &o.units);
            if (ImGui::Checkbox("Units: idle pose", &o.poseUnits)) app.scene.DropModels();
            ImGui::SameLine();
            ImGui::BeginDisabled(!o.poseUnits);
            ImGui::Checkbox("animated", &o.animateUnits);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("They play their animations at the game's 15 frames a second: idle; walking when they move in the patrol simulation");
            ImGui::SetItemTooltip("Units (and other animated figures) stand in their idle animation's first frame, as in the game; off: the T-pose");
            ImGui::Checkbox("Markers", &o.markers);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lights (yellow), particles (magenta), sounds (cyan) and objects\nwhose figure is missing (red)");
            if (ImGui::Checkbox("Walkability", &o.walkability) && o.walkability) app.scene.BuildWalkGrid();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Where units can walk, as the game computes it, for the navmesh layer below. Red: blocked; orange: hard ground (water, slopes, obstacles). Patrols route on it.");
            ImGui::Checkbox("Game navmesh (AI_GRAPH)", &o.navmesh);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The zone map's stored navmesh: a node per 4 x 4 units, lines to the neighbours a unit can reach (green cheap, red costly), red squares where it can go nowhere.");
            ImGui::SetNextItemWidth(110);
            if (ImGui::SliderInt("Navmesh layer", &o.navLayer, 0, mob::kAiLayers - 1) && o.walkability) app.scene.BuildWalkGrid();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The graph has 8 layers: units use the one of their AI class");
            if (ImGui::Checkbox("Navmesh differences", &o.navCompare) && o.navCompare) BuildCompareNavmesh(app);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The map's navmesh against the one the game would build now, per 4 x 4 node: orange: only the map's walks there; blue: only the rebuilt one; yellow: both, other costs. Anything shown: out of date.\n"
                    "Now: %d orange, %d blue, %d yellow.%s",
                                  app.scene.navCompareGame, app.scene.navCompareEditor, app.scene.navCompareCost,
                                  app.navCompareNote.empty() ? "" : ("\n" + app.navCompareNote).c_str());
            if (o.navCompare) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Rebuild")) BuildCompareNavmesh(app);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build it again from the maps as they are now (after edits)");
            }
            ImGui::Checkbox("Script areas", &o.scriptAreas);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The areas the scripts declare (magenta), with their number and the quest objectives that use them. %zu loaded.\n"
                    "Alt + drag inside one moves it; on its edge, resizes it.",
                                  app.scene.scriptAreas.size());
            ImGui::Checkbox("Shadows", &o.shadows);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the lighting on: the sun's shadows of the terrain and the figures, on the terrain");
            if (ImGui::Checkbox("Selected unit's logic", &app.lib.logicAlways)) app.lib.SaveConfig();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The selected units' paths, points and radii also show outside logic mode\n(their points are selected and moved in logic mode)");
            if (ImGui::Checkbox("Logic: selected only", &app.lib.logicSelectedOnly)) app.lib.SaveConfig();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("In logic mode, only the selected units show their paths, patrol points and radii.\nOff: every unit of the active map shows them.");
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ToolsMenu(app);
    ImGui::SameLine();
    ImGui::Checkbox("Textured", &o.textured);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &o.wireframe);
    if (app.openQuest) {
        ImGui::SameLine();
        ImGui::Checkbox("Exits", &o.exits);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The quest's deploy areas (green) and exit areas (red), from its map.txt");
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) app.scene.FrameAll();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole map (%s)", ui::BindName(app.lib.mapKeys[config::kKeyResetCamera]).c_str());

    // Second row: movement speed, modes, and the status on the right.
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##speed", &app.lib.mapCameraSpeed, 0.1f, 5.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.lib.SaveConfig();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera speed of the movement keys (Ctrl+click to type a value)");
    ImGui::SameLine();
    bool logic = app.scene.logicMode;
    if (ImGui::Checkbox("Logic", &logic)) app.scene.logicMode = logic;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Units' behaviours: patrol paths and points (yellow), look directions (blue), guard radius (orange),\n"
                          "sentry places (green), help radius (purple). Only units can be selected. %s",
                          ui::BindName(app.lib.mapKeys[config::kKeyLogicMode]).c_str());
    // Blender-like modes: Object (select and edit objects), Tile paint, Sculpt; the active tool's settings follow.
    ImGui::SameLine();
    {
        static const char* const kModes[] = {"Object mode", "Tile paint", "Sculpt"};
        int mode = app.heightBrush ? 2 : app.tileBrush ? 1 : 0;
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##mode", kModes[mode])) {
            for (int i = 0; i < 3; ++i) {
                const bool needsTerrain = i > 0 && !app.terrainLoaded;
                if (ImGui::Selectable(kModes[i], i == mode, needsTerrain ? ImGuiSelectableFlags_Disabled : 0)) {
                    app.tileBrush = i == 1;
                    app.heightBrush = i == 2;
                }
                if (needsTerrain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Load a terrain (.mpr) first");
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The editing mode, as in Blender: objects, painting the terrain's tiles, or shaping its ground");
        if (mode == 1) {
            ImGui::SameLine();
            TileImage(app, std::max(app.brushTile, 0), ImGui::GetFrameHeight(), app.brushRotation);
            ImGui::SameLine();
            ImGui::TextDisabled("tile %d, turn %d (, .)  %s", app.brushTile, app.brushRotation * 90, app.brushWater ? "water" : "land");
        } else if (mode == 2) {
            static const char* const kTools[] = {"Raise", "Lower", "Smooth", "Flatten"};
            for (int i = 0; i < 4; ++i) { ImGui::SameLine(); ImGui::RadioButton(kTools[i], &app.heightMode, i); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##radius", &app.heightRadius, 1.0f, 32.0f, "radius %.1f");
            ImGui::SetItemTooltip("F + move the mouse left/right");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##strength", &app.heightStrength, 0.2f, 20.0f, "strength %.1f");
            ImGui::SetItemTooltip("Shift+F + move the mouse left/right");
        }
    }
    if (app.xf.mode != Transform::None) { // a move or scale in progress: how it works
        ImGui::SameLine();
        const char* axis[8] = {"free", "X", "Y", "X Y", "Z", "X Z", "Y Z", "X Y Z"};
        if (!app.xf.typed.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 1.0f, 1.0f), "typed: %s%s", app.xf.typed.c_str(), app.xf.mode == Transform::Rotate ? " deg" : "");
            ImGui::SameLine();
        }
        if (app.xf.mode == Transform::Rotate)
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "Rotate about %s  %.1f deg   X/Y/Z axis, type degrees, Ctrl free angle, click/Enter apply, right-click/Esc cancel",
                               axis[app.xf.axes & 7], app.xf.angle);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "%s %s  %.2f %.2f %.2f   X/Y/Z axis, Shift+axis all but, type a value (5 or 1,-2), click/Enter apply, right-click/Esc cancel",
                               app.xf.mode == Transform::Move ? "Move" : "Scale", axis[app.xf.axes & 7], app.xf.delta.x, app.xf.delta.y, app.xf.delta.z);
    } else if (app.questDirty || app.mqTextDirty || AnyMobDirty(app)) { // unsaved: say so where it is seen, with the way to save
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.1f, 1.0f));
        const std::string label = "Save (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ")";
        if (ImGui::Button(label.c_str())) SaveQuestChanges(app);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            std::string what;
            for (auto& m : app.mobs) if (m->Dirty()) what += "\n  " + m->file.fileName;
            if (TerrainUnsaved(app)) what += "\n  the terrain " + std::filesystem::path(app.terrainPath).filename().string();
            if (app.questDirty) what += "\n  the quest's areas";
            if (app.mqTextDirty) what += "\n  " + app.mqEntry;
            ImGui::SetTooltip("Unsaved:%s", what.c_str());
        }
        // Regenerate the zone's navmesh (AI_GRAPH) when saving, as the game builds it (navmesh_gen.hpp).
        ImGui::SameLine();
        if (ImGui::Checkbox("Navmesh", &app.lib.mapRegenNavmesh)) app.lib.SaveConfig();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rebuild the navmesh on save: the open zone map's AI_GRAPH is built again from the terrain and the open maps' objects, as the game does. A few seconds on big maps.");
    }
    // Always one item after SameLine, even empty: otherwise the viewport below would start on this line.
    ImGui::SameLine();
    ImGui::TextUnformatted("");
}

// The bar under the view: lighting (on/off, file, hour), the active map, what is loading and where the
// mouse points.
static void BottomBar(App& app) {
    if (app.heightBrush || app.tileBrush) { // the mode's mouse and keys, as Blender's status bar
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.4f, 1), "%s", app.heightBrush
            ? "Sculpt: left drag shapes the ground (one undo per stroke)  F radius  Shift+F strength  (Tools > Rebuild navmesh after)"
            : "Tile paint: left drag paints  Alt+click picks a tile  1-8 quick tiles  , . turn the tile");
        ImGui::SameLine();
    }
    if (ImGui::Checkbox("Lighting", &app.lib.lightingOn)) app.lib.SaveConfig();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light the map with a lighting file of the Settings tab at the map's time of day. %s",
                          ui::BindName(app.lib.mapKeys[config::kKeyLighting]).c_str());
    if (app.lib.lightingOn) {
        const lighting::Table* t = ChosenLighting(app);
        ImGui::SameLine();
        if (!t) {
            ImGui::TextDisabled("(add lights*.ini files in Settings > Lighting)");
        } else {
            ImGui::SetNextItemWidth(170);
            if (ImGui::BeginCombo("##lightini", t->name.c_str())) {
                for (const lighting::Table& other : app.lightTables)
                    if (ImGui::Selectable(other.name.c_str(), &other == t)) { app.lib.lightingChoice = other.path; app.lib.SaveConfig(); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->path.c_str());
            ImGui::SameLine();
            float h = MapHour(app);
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderFloat("##hour", &h, 0.0f, 24.0f, "%.1f h")) { app.hour = h; app.hourSet = true; }
            if (ImGui::IsItemDeactivatedAfterEdit()) { app.lib.mapHour = app.hour; app.lib.SaveConfig(); } // remembered
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time of day (starts at the map's own time; right-click: back to it)");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { app.hourSet = false; app.lib.mapHour = -1.0f; app.lib.SaveConfig(); }
        }
    }
    if (!app.mobs.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  active: %s", app.mobs[app.activeMob]->file.fileName.c_str());
    }
    char status[96] = "";
    if (app.scene.modelsPending > 0) std::snprintf(status, sizeof(status), "loading %d figures...", app.scene.modelsPending);
    else if (app.hoverGround) std::snprintf(status, sizeof(status), "x %.1f  y %.1f  z %.2f", app.ground.x, app.ground.y, app.ground.z);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", status);
    if (!app.questMessage.empty()) { // the last action's result (saved, deleted, copied...)
        ImGui::SameLine();
        ImGui::TextDisabled("   %s", app.questMessage.c_str());
    }
}

// Moves the camera's target freely: its height only changes with up/down, whatever the ground does.
static void MoveTarget(App& app, float dx, float dy, float dz) {
    OrbitCamera& cam = app.scene.camera;
    cam.targetX += dx;
    cam.targetY += dy;
    cam.targetZ += dz;
}

// ---- move (G) and scale (T) ------------------------------------------------------------------------
// Blender's way: the key starts it, the mouse drives it, X / Y / Z keep it to one axis (Shift+axis: all
// but that one), a left click or Enter applies it, a right click or Esc puts everything back. Moving
// follows the ground under the mouse (Z: the mouse's height on screen); scaling changes the complection
// (like ei_maper's scale tool) with the mouse's horizontal movement.

// The world position of the selection's centre as drawn (figures stand on the ground).
static mob::Vec3 DrawnCentre(App& app, const mob::Vec3& c) {
    return {c.x, c.y, app.scene.Ground(c.x, c.y) + std::max(c.z, 0.0f)};
}

// The mouse's angle (radians) around the selection's centre on screen, for rotating.
static float ScreenAngle(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    mob::Vec3 c = DrawnCentre(app, app.xf.centre);
    float fx, fy;
    if (!app.scene.Project({c.x, c.y, c.z}, fx, fy)) return 0.0f;
    return std::atan2(mouse.y - (min.y + fy * size.y), mouse.x - (min.x + fx * size.x));
}

static void StartTransform(App& app, Transform::Mode mode, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    if (app.simulating) return; // editing waits for the simulation to stop
    if (app.scene.selectedFile != app.activeMob || app.scene.selection.empty() || app.mobs.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    // Logic points selected (logic mode): G moves them rather than the units.
    PruneLogicPoints(app);
    const bool points = !app.scene.logicPoints.empty();
    if (points && mode != Transform::Move) return;
    Transform& x = app.xf;
    x = Transform{};
    x.mode = mode;
    x.axes = mode == Transform::Move ? 1 | 2 : mode == Transform::Rotate ? 4 : 1 | 2 | 4;
    x.startMouse = mouse;
    x.file = f.path;
    if (points) {
        x.points = app.scene.logicPoints;
        x.bytesBefore = f.bytes;
        for (const LogicPointRef& r : x.points) {
            if (r.trap) x.trapsBefore.emplace(r.object, std::make_pair(f.objects[r.object].trapAreas, f.objects[r.object].trapTargets));
            else x.logicsBefore.emplace(std::make_pair(r.object, r.logic), f.objects[r.object].logics[r.logic]);
            mob::Vec3 p;
            LogicPointAt(f, r, p);
            x.centre.x += p.x; x.centre.y += p.y; x.centre.z += p.z;
        }
    } else {
        x.before = CaptureObjects(f, app.scene.selection);
    }
    for (const ObjectState& st : x.before) { x.centre.x += st.position.x; x.centre.y += st.position.y; x.centre.z += st.position.z; }
    const float n = static_cast<float>(std::max<size_t>(points ? x.points.size() : x.before.size(), 1));
    x.centre = {x.centre.x / n, x.centre.y / n, x.centre.z / n};
    ImVec2 at((mouse.x - min.x) / std::max(size.x, 1.0f), (mouse.y - min.y) / std::max(size.y, 1.0f));
    x.haveGround = app.scene.GroundAt(at.x, at.y, x.startGround);
    x.startAngle = ScreenAngle(app, mouse, min, size);
}

static void EndTransform(App& app, bool apply) {
    Transform& x = app.xf;
    MobEntry* m = FindMob(app, x.file);
    if (m && !x.points.empty()) { // logic points
        if (apply) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = x.file;
            step.bytes = std::move(x.bytesBefore);
            PushUndo(app, std::move(step), "Move logic points");
            app.checksDirty = true;
            app.listedKey.clear();
        } else {
            m->file.bytes = std::move(x.bytesBefore);
            mob::Reparse(m->file);
        }
    } else if (m) {
        if (apply) {
            EditStep step;
            step.kind = EditStep::Objects;
            step.file = x.file;
            step.objects = x.before;
            PushUndo(app, std::move(step), std::string(x.mode == Transform::Move ? "Move " : x.mode == Transform::Rotate ? "Rotate " : "Scale ") + std::to_string(x.before.size()) + " object(s)");
            app.checksDirty = true;
        } else {
            ApplyObjects(m->file, x.before);
        }
    }
    x = Transform{};
    app.scene.guides.clear();
}

// The world position of an object as drawn (for the guide lines): figures stand on the ground.
static mob::Vec3 GuideCentre(App& app) {
    mob::Vec3 c = app.xf.centre;
    return {c.x, c.y, app.scene.Ground(c.x, c.y) + std::max(c.z, 0.0f) + 0.3f};
}

static void UpdateTransform(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    Transform& x = app.xf;
    MobEntry* m = FindMob(app, x.file);
    if (!m) { x = Transform{}; return; }
    ImGuiIO& io = ImGui::GetIO();
    // Axis keys (by the letter the layout prints: X, Y and Z mean the axes).
    for (int a = 0; a < 3; ++a) {
        if (!ImGui::IsKeyPressed(a == 0 ? ImGuiKey_X : a == 1 ? ImGuiKey_Y : ImGuiKey_Z, false)) continue;
        if (x.mode == Transform::Rotate) { x.axes = x.axes == (1 << a) ? 4 : (1 << a); continue; } // one axis; again: back to Z
        int want = io.KeyShift ? (7 & ~(1 << a)) : (1 << a);
        const int free = x.mode == Transform::Move ? 3 : 7;
        x.axes = x.axes == want ? free : want; // the same key again: back to free
    }
    // Typed values (like Blender): digits, '.', '-' and ',' between the axes' values; Backspace erases.
    for (ImWchar c : io.InputQueueCharacters)
        if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-') x.typed += static_cast<char>(c);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !x.typed.empty()) x.typed.pop_back();
    const int axisCount = x.mode == Transform::Rotate ? 1 : ((x.axes & 1) != 0) + ((x.axes & 2) != 0) + ((x.axes & 4) != 0);
    const std::vector<float> typed = TypedValues(x.typed, axisCount);
    auto typedFor = [&](int axis) { // the typed value of this axis (0..2), by its place among the axes in use
        int i = 0;
        for (int a = 0; a < axis; ++a) if (x.axes & (1 << a)) ++i;
        return typed[i];
    };
    std::vector<ObjectState> now = x.before;
    if (x.mode == Transform::Move) {
        mob::Vec3 d{0, 0, 0};
        if (x.axes & 3) {
            ImVec2 at((mouse.x - min.x) / std::max(size.x, 1.0f), (mouse.y - min.y) / std::max(size.y, 1.0f));
            fig::Vec3 g;
            if (x.haveGround && app.scene.GroundAt(at.x, at.y, g)) { d.x = g.x - x.startGround.x; d.y = g.y - x.startGround.y; }
        }
        if (x.axes & 4) d.z = (x.startMouse.y - mouse.y) * app.scene.camera.distance / 600.0f;
        if (!(x.axes & 1)) d.x = 0;
        if (!(x.axes & 2)) d.y = 0;
        if (!(x.axes & 4)) d.z = 0;
        if (!io.KeyCtrl) { d.x = std::round(d.x * 10) / 10; d.y = std::round(d.y * 10) / 10; d.z = std::round(d.z * 10) / 10; }
        if (!typed.empty()) d = {(x.axes & 1) ? typedFor(0) : 0.0f, (x.axes & 2) ? typedFor(1) : 0.0f, (x.axes & 4) ? typedFor(2) : 0.0f};
        if (!x.points.empty()) { // logic points: each keeps to the ground (plus any Z offset)
            std::map<std::pair<int, int>, mob::Logic> now = x.logicsBefore;
            auto traps = x.trapsBefore;
            for (const LogicPointRef& r : x.points) {
                if (r.trap) { // a trap's area or cast point: x and y only (an area's z is its radius)
                    auto& list = r.trap == 1 ? traps[r.object].first : traps[r.object].second;
                    list[static_cast<size_t>(r.point)].x += d.x;
                    list[static_cast<size_t>(r.point)].y += d.y;
                    continue;
                }
                mob::Logic& g = now[std::make_pair(r.object, r.logic)];
                const mob::Logic& was = x.logicsBefore[std::make_pair(r.object, r.logic)];
                mob::Vec3 p = r.place ? was.guardPlace : r.look >= 0 ? was.patrol[r.point].looks[r.look].position : was.patrol[r.point].position;
                p.x += d.x;
                p.y += d.y;
                p.z = r.place ? p.z + d.z : app.scene.Ground(p.x, p.y) + d.z;
                SetLogicPoint(g, r, p);
            }
            m->file.bytes = x.bytesBefore;
            mob::Reparse(m->file);
            for (auto& [key, g] : now) mob::SetLogic(m->file, key.first, key.second, g);
            for (auto& [oi, lists] : traps) {
                mob::ReplaceField(m->file, oi, mob::kMagicTrapAreas, mob::TrapListPayload(true, lists.first));
                mob::ReplaceField(m->file, oi, mob::kMagicTrapTargets, mob::TrapListPayload(false, lists.second));
            }
            x.delta = d;
            app.scene.guides.clear();
            if (x.axes != 3) {
                mob::Vec3 c = GuideCentre(app);
                const float L = 400.0f;
                if (x.axes & 1) app.scene.guides.push_back({{c.x - L, c.y, c.z}, {c.x + L, c.y, c.z}, 0.95f, 0.25f, 0.25f});
                if (x.axes & 2) app.scene.guides.push_back({{c.x, c.y - L, c.z}, {c.x, c.y + L, c.z}, 0.35f, 0.9f, 0.3f});
                if (x.axes & 4) app.scene.guides.push_back({{c.x, c.y, c.z - L}, {c.x, c.y, c.z + L}, 0.3f, 0.5f, 1.0f});
            }
            return;
        }
        for (ObjectState& st : now) { st.position.x += d.x; st.position.y += d.y; st.position.z += d.z; }
        x.delta = d;
    } else if (x.mode == Transform::Rotate) {
        // The angle the mouse turned around the centre on screen; turning counterclockwise on screen turns
        // the objects counterclockwise seen from above (about Z). 1 degree steps unless Ctrl is held.
        float turned = -(ScreenAngle(app, mouse, min, size) - x.startAngle) * 180.0f / 3.14159265f;
        while (turned > 180.0f) turned -= 360.0f;
        while (turned < -180.0f) turned += 360.0f;
        if (!io.KeyCtrl) turned = std::round(turned);
        if (!typed.empty()) turned = typed[0];
        x.angle = turned;
        const int axis = x.axes == 1 ? 0 : x.axes == 2 ? 1 : 2;
        const float half = turned * 3.14159265f / 360.0f;
        const fig::Quat q{std::cos(half), axis == 0 ? std::sin(half) : 0.0f, axis == 1 ? std::sin(half) : 0.0f, axis == 2 ? std::sin(half) : 0.0f};
        for (ObjectState& st : now) {
            // Each object turns in place, and around the selection's centre with the others (world axes).
            fig::Quat r = fig::QuatNormalize(fig::QuatMul(q, fig::Quat{st.rotation[0], st.rotation[1], st.rotation[2], st.rotation[3]}));
            st.rotation[0] = r.w; st.rotation[1] = r.x; st.rotation[2] = r.y; st.rotation[3] = r.z;
            fig::Vec3 off = fig::QuatRotate(q, {st.position.x - x.centre.x, st.position.y - x.centre.y, st.position.z - x.centre.z});
            st.position = {x.centre.x + off.x, x.centre.y + off.y, x.centre.z + off.z};
        }
        x.delta = {0, 0, 0};
    } else {
        float k = (mouse.x - x.startMouse.x) / 200.0f;
        if (!io.KeyCtrl) k = std::round(k * 100) / 100;
        mob::Vec3 d{(x.axes & 1) ? k : 0.0f, (x.axes & 2) ? k : 0.0f, (x.axes & 4) ? k : 0.0f};
        if (!typed.empty()) d = {(x.axes & 1) ? typedFor(0) : 0.0f, (x.axes & 2) ? typedFor(1) : 0.0f, (x.axes & 4) ? typedFor(2) : 0.0f};
        for (ObjectState& st : now) { st.complection.x += d.x; st.complection.y += d.y; st.complection.z += d.z; }
        x.delta = d;
    }
    ApplyObjects(m->file, now);
    // Guide lines for a constrained axis: X red, Y green, Z blue.
    app.scene.guides.clear();
    const bool constrained = x.mode == Transform::Rotate || x.axes != (x.mode == Transform::Move ? 3 : 7);
    if (constrained) {
        mob::Vec3 c = GuideCentre(app);
        const float L = 400.0f;
        if (x.axes & 1) app.scene.guides.push_back({{c.x - L, c.y, c.z}, {c.x + L, c.y, c.z}, 0.95f, 0.25f, 0.25f});
        if (x.axes & 2) app.scene.guides.push_back({{c.x, c.y - L, c.z}, {c.x, c.y + L, c.z}, 0.35f, 0.9f, 0.3f});
        if (x.axes & 4) app.scene.guides.push_back({{c.x, c.y, c.z - L}, {c.x, c.y, c.z + L}, 0.3f, 0.5f, 1.0f});
    }
}

// The keys of the Settings tab (by key position). Held: move forward/back/left/right relative to where
// the camera looks, up/down, faster. Pressed once: logic mode, next active map (its list shows while
// held), unload the last file, reset the camera, lighting. Not while a text field is being edited.
static void Keys(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    GLFWwindow* window = glfwGetCurrentContext();
    app.switchListShown = false;
    if (!window) return;
    const int mods = ui::HeldMods(window);
    bool down[config::kMapKeyCount];
    for (int k = 0; k < config::kMapKeyCount; ++k) {
        config::KeyBind b = app.lib.mapKeys[k];
        if (b.mods & config::kModLetter) { b.key = ui::KeyForLetter(b.key); b.mods &= ~config::kModLetter; } // the key printing that letter
        // While a text field is being typed in, only the save key (Ctrl+S) works: the rest would type or move.
        const bool allowed = !io.WantTextInput || (k == config::kKeySave && b.mods != 0);
        down[k] = allowed && b.key > 0 && glfwGetKey(window, b.key) == GLFW_PRESS && (k < config::kFirstActionKey || mods == b.mods);
    }
    auto pressed = [&](int k) { return down[k] && !app.keyWasDown[k]; };
    if (pressed(config::kKeyLogicMode)) {
        app.scene.logicMode = !app.scene.logicMode;
        if (app.scene.logicMode) app.scene.FilterSelection([](const mob::Object& o) { return o.kind == mob::Kind::Unit; }); // only units stay selected
    }
    // The map list shows from the press until its modifiers (Ctrl by default) are let go, like Alt+Tab,
    // so it can be read; without modifiers, while the key is held.
    const config::KeyBind& switchBind = app.lib.mapKeys[config::kKeySwitchMob];
    if (pressed(config::kKeySwitchMob) && !app.mobs.empty()) {
        SetActiveMob(app, app.activeMob + 1);
        app.switchListLatched = true;
    }
    const int switchMods = switchBind.mods & ~config::kModLetter; // the letter flag is not a key that is held
    if (switchMods ? (mods & switchMods) != switchMods : !down[config::kKeySwitchMob]) app.switchListLatched = false;
    app.switchListShown = app.switchListLatched && app.mobs.size() > 1;
    if (pressed(config::kKeyUnloadLast)) UnloadLast(app);
    if (pressed(config::kKeyResetCamera)) app.scene.FrameAll();
    if (pressed(config::kKeyLighting)) { app.lib.lightingOn = !app.lib.lightingOn; app.lib.SaveConfig(); }
    if (pressed(config::kKeySave) && app.xf.mode == Transform::None) {
        ApplyScriptEdit(app); // a script being edited goes in first
        SaveQuestChanges(app);
    }
    // G / T with a selection and the mouse over the view.
    bool transformStarted = false;
    if (app.xf.mode == Transform::None && app.viewHovered && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty()) {
        if (pressed(config::kKeyMove)) { StartTransform(app, Transform::Move, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
        else if (pressed(config::kKeyScale)) { StartTransform(app, Transform::Scale, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
        else if (pressed(config::kKeyRotate)) { StartTransform(app, Transform::Rotate, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
    }
    if (pressed(config::kKeyFind)) { app.findOpen = true; app.findFocus = true; }
    if (app.xf.mode == Transform::None) { TerrainBrushKeys(app); HeightBrushKeys(app); }
    if (pressed(config::kKeySelectAll) && app.xf.mode == Transform::None) { SelectAll(app); app.requestTab = SideTab::Objects; }
    if (app.xf.mode == Transform::None) {
        if (pressed(config::kKeyDelete)) {
            if (!app.scene.logicPoints.empty()) DeleteLogicPoints(app); // the selected points, not their unit or trap
            else DeleteSelection(app);
        }
        if (pressed(config::kKeyCopy)) CopySelection(app);
        if (pressed(config::kKeyPaste)) Paste(app);
        if (pressed(config::kKeyDuplicate)) Duplicate(app);
        if (pressed(config::kKeyResetPaths)) ResetLogicPaths(app);
    }
    if (pressed(config::kKeyUndo) && app.xf.mode == Transform::None) UndoRedo(app, false);
    if (pressed(config::kKeyRedo) && app.xf.mode == Transform::None) UndoRedo(app, true);
    for (int k = 0; k < config::kMapKeyCount; ++k) app.keyWasDown[k] = down[k];
    if (app.xf.mode != Transform::None || transformStarted) return; // the camera stays still while transforming

    // Movement ignores modifiers (Shift is the default "faster"), but not with Ctrl held: Ctrl+... are actions.
    if (mods & config::kModCtrl) return;
    float forward = (down[config::kKeyForward] ? 1.0f : 0.0f) - (down[config::kKeyBack] ? 1.0f : 0.0f);
    float right = (down[config::kKeyRight] ? 1.0f : 0.0f) - (down[config::kKeyLeft] ? 1.0f : 0.0f);
    float up = (down[config::kKeyUp] ? 1.0f : 0.0f) - (down[config::kKeyDown] ? 1.0f : 0.0f);
    if (forward == 0 && right == 0 && up == 0) return;
    const OrbitCamera& cam = app.scene.camera;
    float speed = std::max(cam.distance, 8.0f) * 0.9f * app.lib.mapCameraSpeed * io.DeltaTime * (down[config::kKeyFast] ? 3.0f : 1.0f);
    float yaw = cam.yawDeg * 3.14159265f / 180.0f;
    // The camera sits at +(cos yaw, sin yaw) from its target, so it looks along -(cos yaw, sin yaw).
    float fx = -std::cos(yaw), fy = -std::sin(yaw), rx = -std::sin(yaw), ry = std::cos(yaw); // right = forward x up
    MoveTarget(app, (fx * forward + rx * right) * speed, (fy * forward + ry * right) * speed, up * speed * 0.6f);
}

// An area's handles: 0-3 corners (x1 y1, x2 y1, x2 y2, x1 y2), 4-7 sides (x1, x2, y1, y2), 8 the centre.
// The script areas' drag squares: rect corners, sides and centre; a round area's centre and rim (east point).
struct ScriptHandle { std::string file; size_t index = 0; float x = 0, y = 0; int resize = 0; };
static std::vector<ScriptHandle> ScriptHandles(App& app) {
    std::vector<ScriptHandle> out;
    if (!app.scene.options.scriptAreas) return out;
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            const float* v = calls[i].v;
            auto add = [&](float x, float y, int resize) { out.push_back({m->file.path, i, x, y, resize}); };
            if (calls[i].round) { add(v[0], v[1], 0); add(v[0] + v[2], v[1], 1); continue; }
            const float mx = (v[0] + v[2]) * 0.5f, my = (v[1] + v[3]) * 0.5f;
            add(v[0], v[1], 2 | 4); add(v[2], v[1], 8 | 4); add(v[2], v[3], 8 | 16); add(v[0], v[3], 2 | 16);
            add(v[0], my, 2); add(v[2], my, 8); add(mx, v[1], 4); add(mx, v[3], 16); add(mx, my, 0);
        }
    }
    return out;
}

static mob::Vec3 HandlePoint(const quest::Rect& r, int h) {
    const float mx = (r.x1 + r.x2) * 0.5f, my = (r.y1 + r.y2) * 0.5f;
    switch (h) {
    case 0: return {r.x1, r.y1, 0};
    case 1: return {r.x2, r.y1, 0};
    case 2: return {r.x2, r.y2, 0};
    case 3: return {r.x1, r.y2, 0};
    case 4: return {r.x1, my, 0};
    case 5: return {r.x2, my, 0};
    case 6: return {mx, r.y1, 0};
    case 7: return {mx, r.y2, 0};
    default: return {mx, my, 0};
    }
}

// Starts dragging the area handle under the mouse (within a few pixels), if any.
static bool GrabAreaHandle(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size, fig::Vec3 ground) {
    if (!app.openQuest || !app.scene.options.exits) return false;
    float best = 9.0f * 9.0f;
    App::RectDrag found;
    for (size_t ei = 0; ei < app.openQuest->shown.exits.size(); ++ei) {
        const quest::Exit& e = app.openQuest->shown.exits[ei];
        for (int which = 0; which < 2; ++which) {
            const quest::Rect& r = which ? e.remove : e.deploy;
            if (!r.set) continue;
            for (int h = 0; h < 9; ++h) {
                mob::Vec3 hp = HandlePoint(r, h);
                float fx, fy;
                if (!app.scene.Project({hp.x, hp.y, app.scene.Ground(hp.x, hp.y) + 0.12f}, fx, fy)) continue;
                float dx = min.x + fx * size.x - mouse.x, dy = min.y + fy * size.y - mouse.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    found = App::RectDrag{true, static_cast<int>(ei), which == 1, h, r, ground};
                }
            }
        }
    }
    if (!found.on) return false;
    PushAreaUndo(app, app.openQuest->shown.exits); // the areas before this drag
    app.rectDrag = found;
    return true;
}

// Moves the dragged handle to the ground point under the mouse (whole units unless Shift is held).
static void DragAreaHandle(App& app, fig::Vec3 ground, bool snap) {
    App::RectDrag& d = app.rectDrag;
    quest::Exit& e = app.openQuest->shown.exits[d.exit];
    quest::Rect& r = d.remove ? e.remove : e.deploy;
    auto round = [&](float v) { return snap ? std::round(v) : std::round(v * 10.0f) / 10.0f; };
    const float gx = round(ground.x), gy = round(ground.y);
    r = d.start;
    switch (d.handle) {
    case 0: r.x1 = gx; r.y1 = gy; break;
    case 1: r.x2 = gx; r.y1 = gy; break;
    case 2: r.x2 = gx; r.y2 = gy; break;
    case 3: r.x1 = gx; r.y2 = gy; break;
    case 4: r.x1 = gx; break;
    case 5: r.x2 = gx; break;
    case 6: r.y1 = gy; break;
    case 7: r.y2 = gy; break;
    default: {
        float dx = round(ground.x - d.from.x), dy = round(ground.y - d.from.y);
        r.x1 += dx; r.x2 += dx; r.y1 += dy; r.y2 += dy;
        break;
    }
    }
    app.questDirty = !SameAreas(app.openQuest->shown.exits, app.savedAreas);
}

// Drawn over the viewport: logic labels (behaviour, point numbers, waits) and, while the switch key is
// held, the list of maps with the active one marked.
static void Overlays(App& app, ImVec2 min, ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), true); // nothing over the bars around the view
    auto label = [&](const mob::Vec3& p, float lift, ImU32 color, const std::string& text) {
        float fx, fy;
        if (!app.scene.Project({p.x, p.y, app.scene.Ground(p.x, p.y) + lift}, fx, fy) || fx < 0 || fx > 1 || fy < 0 || fy > 1) return;
        ImVec2 at(min.x + fx * size.x + 6, min.y + fy * size.y - 7);
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        draw->AddRectFilled(ImVec2(at.x - 2, at.y - 1), ImVec2(at.x + ts.x + 2, at.y + ts.y + 1), IM_COL32(0, 0, 0, 150), 3.0f);
        draw->AddText(at, color, text.c_str());
    };
    for (size_t k = 0; k < app.missing.size() && k < 400; ++k) { // objects without a figure shown
        const MapScene::Missing& mi = app.missing[k];
        if (mi.file < 0 || mi.file >= static_cast<int>(app.mobs.size())) continue;
        const auto& objs = app.mobs[static_cast<size_t>(mi.file)]->file.objects;
        if (mi.object < static_cast<int>(objs.size())) label(objs[static_cast<size_t>(mi.object)].position, 0.5f, IM_COL32(255, 90, 70, 255), "? " + (mi.figure.empty() ? std::string("(no figure)") : mi.figure));
    }
    // The scripts' areas: their number and the objectives that use them.
    if (app.scene.options.scriptAreas) {
        for (const MapScene::ScriptArea& a : app.scene.scriptAreas) {
            std::string text = "Area " + std::to_string(a.id);
            for (const quests::Quest& q : app.scriptModel.quests)
                for (size_t k = 0; k < q.objectives.size(); ++k) {
                    const quests::Call& c = q.objectives[k].call;
                    if (c.name == "QObjArea" && !c.args.empty() && std::atoi(c.args[0].c_str()) == a.id)
                        text += "\n" + q.name + " #" + std::to_string(k + 1) + (q.objectives[k].title.empty() ? "" : ": " + q.objectives[k].title);
                }
            const float cx = a.round ? a.x : (a.x + a.x2) * 0.5f, cy = a.round ? a.y : (a.y + a.y2) * 0.5f;
            label({cx, cy, 0}, 0.5f, IM_COL32(245, 120, 245, 255), text);
        }
    }
    // A look point's wait, beside its eye (drawn in the scene).
    auto eye = [&](const mob::Vec3& p, float wait) { label(p, 0.0f, IM_COL32(150, 195, 255, 255), ui::Num(wait / 15.0f, 1) + " s"); };
    if (app.scene.logicMode || app.scene.logicAlways) {
        app.scene.ForEachLogicUnit([&](const mob::Object& o, bool selected) {
            std::string head;
            for (const mob::Logic& g : o.logics) {
                if (!g.use) continue;
                if (!head.empty()) head += " + ";
                head += mob::LogicModelName(g.model);
                if (g.wait >= 0) head += ", waits " + ui::Num(g.wait / 15.0f, 1) + " s";
            }
            if (head.empty()) return;
            label(o.position, 2.2f, selected ? IM_COL32(255, 225, 120, 255) : IM_COL32(230, 230, 230, 220), ObjectLabel(o) + ": " + head);
            for (const mob::Logic& g : o.logics) {
                if (!g.use) continue;
                if (g.model == 1) label(g.guardPlace, 0.4f, IM_COL32(245, 150, 60, 255), "radius " + ui::Num(g.guardRadius, 1));
                for (size_t i = 0; i < g.patrol.size(); ++i) {
                    const mob::PatrolPoint& p = g.patrol[i];
                    label(p.position, 1.5f, IM_COL32(235, 225, 90, 255), std::to_string(i + 1));
                    for (size_t k = 0; k < p.looks.size(); ++k) eye(p.looks[k].position, static_cast<float>(p.looks[k].wait));
                }
            }
        });
    }
    // The selected traps' areas and cast points.
    if (!app.mobs.empty() && app.scene.selectedFile == app.activeMob) {
        const mob::File& f = app.mobs[app.activeMob]->file;
        for (int oi : app.scene.selection) {
            if (oi < 0 || oi >= static_cast<int>(f.objects.size()) || f.objects[oi].kind != mob::Kind::MagicTrap) continue;
            const mob::Object& o = f.objects[oi];
            for (size_t i = 0; i < o.trapAreas.size(); ++i)
                label({o.trapAreas[i].x, o.trapAreas[i].y, 0}, 0.3f, IM_COL32(245, 150, 80, 255), "area " + std::to_string(i + 1) + ", radius " + ui::Num(o.trapAreas[i].z, 1));
            for (size_t i = 0; i < o.trapTargets.size(); ++i)
                label({o.trapTargets[i].x, o.trapTargets[i].y, 0}, 0.8f, IM_COL32(235, 130, 235, 255), "cast point " + std::to_string(i + 1));
        }
    }
    for (const ScriptHandle& h : ScriptHandles(app)) { // the script areas' drag squares
        float fx, fy;
        if (!app.scene.Project({h.x, h.y, app.scene.Ground(h.x, h.y) + 0.12f}, fx, fy)) continue;
        const ImVec2 c(min.x + fx * size.x, min.y + fy * size.y);
        const bool active = app.areaDrag.on && app.areaDrag.file == h.file && app.areaDrag.index == h.index && app.areaDrag.resize == h.resize;
        const float hs = h.resize == 0 ? 5.0f : 4.0f;
        draw->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), active ? IM_COL32(255, 230, 120, 255) : IM_COL32(120, 200, 255, 255));
        draw->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(0, 0, 0, 200));
    }
    if (app.openQuest && app.scene.options.exits) {
        for (size_t ei = 0; ei < app.openQuest->shown.exits.size(); ++ei) {
            const quest::Exit& e = app.openQuest->shown.exits[ei];
            // Handles: corners, sides and centre of each area.
            for (int which = 0; which < 2; ++which) {
                const quest::Rect& r = which ? e.remove : e.deploy;
                if (!r.set) continue;
                for (int h = 0; h < 9; ++h) {
                    mob::Vec3 hp = HandlePoint(r, h);
                    float fx, fy;
                    if (!app.scene.Project({hp.x, hp.y, app.scene.Ground(hp.x, hp.y) + 0.12f}, fx, fy)) continue;
                    ImVec2 c(min.x + fx * size.x, min.y + fy * size.y);
                    bool active = app.rectDrag.on && app.rectDrag.exit == static_cast<int>(ei) && app.rectDrag.remove == (which == 1) && app.rectDrag.handle == h;
                    ImU32 col = which ? IM_COL32(250, 120, 100, 255) : IM_COL32(110, 240, 130, 255);
                    float hs = h == 8 ? 5.0f : 4.0f;
                    draw->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), active ? IM_COL32(255, 230, 120, 255) : col);
                    draw->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(0, 0, 0, 200));
                }
            }
            std::string title = e.title.empty() ? "exit " + std::to_string(e.number) : e.title;
            auto centre = [](const quest::Rect& r) { return mob::Vec3{(r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f, 0}; };
            if (e.deploy.set) label(centre(e.deploy), 0.5f, IM_COL32(120, 245, 140, 255), title + ": deploy");
            if (e.remove.set) label(centre(e.remove), 1.2f, IM_COL32(250, 130, 110, 255), title + ": exit");
        }
    }
    TerrainBrushOutline(app, draw, min, size);
    if (app.heightBrush && app.terrainLoaded && app.hoverGround) { // the height brush's circle on the ground
        ImVec2 pts[48];
        int n = 0;
        for (int i = 0; i < 48; ++i) {
            const float a2 = i * 6.2831853f / 48, x = app.ground.x + std::cos(a2) * app.heightRadius, y = app.ground.y + std::sin(a2) * app.heightRadius;
            float fx, fy;
            if (app.scene.Project({x, y, app.terrain.HeightAt(x, y) + 0.1f}, fx, fy)) pts[n++] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
        }
        if (n > 2) draw->AddPolyline(pts, n, IM_COL32(255, 210, 90, 220), ImDrawFlags_Closed, 2.0f);
    }
    draw->PopClipRect();
    if (app.switchListShown) {
        ImGui::SetNextWindowPos(ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.35f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::Begin("##switchmob", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextDisabled("Active map (%s: next)", ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
        for (size_t i = 0; i < app.mobs.size(); ++i) {
            bool active = static_cast<int>(i) == app.activeMob;
            if (active) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "> %s", app.mobs[i]->file.fileName.c_str());
            else ImGui::Text("  %s", app.mobs[i]->file.fileName.c_str());
        }
        ImGui::End();
    }
}

// ---- logic points (logic mode) ------------------------------------------------------------------------
// The selected units' patrol points (flags), look points (eyes) and guard places can be clicked to select
// them (Shift: add or remove), taken in a rectangle, dragged, moved with G and deleted.

// Calls fn(ref, position, lift) for each point of the selected units' logic in use in the active map;
// lift is the height above the ground where it is grabbed.
template <typename F> static void EachLogicPoint(App& app, F fn) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    for (int oi : app.scene.selection) {
        if (oi < 0 || oi >= static_cast<int>(f.objects.size())) continue;
        const mob::Object& o = f.objects[oi];
        if (o.kind == mob::Kind::MagicTrap) { // a selected trap's areas and cast points
            for (size_t i = 0; i < o.trapAreas.size(); ++i)
                fn(LogicPointRef{oi, -1, static_cast<int>(i), -1, false, 1}, mob::Vec3{o.trapAreas[i].x, o.trapAreas[i].y, 0}, 'a');
            for (size_t i = 0; i < o.trapTargets.size(); ++i)
                fn(LogicPointRef{oi, -1, static_cast<int>(i), -1, false, 2}, mob::Vec3{o.trapTargets[i].x, o.trapTargets[i].y, 0}, 'c');
            continue;
        }
        if (o.kind != mob::Kind::Unit || !app.scene.logicMode) continue; // a unit's logic: in logic mode
        for (size_t gi = 0; gi < o.logics.size(); ++gi) {
            const mob::Logic& g = o.logics[gi];
            if (!g.use) continue;
            const int l = static_cast<int>(gi);
            if (g.model == 1 || g.model == 3) fn(LogicPointRef{oi, l, -1, -1, true}, g.guardPlace, 'g');
            if (g.model != 2) continue;
            for (size_t i = 0; i < g.patrol.size(); ++i) {
                fn(LogicPointRef{oi, l, static_cast<int>(i), -1, false}, g.patrol[i].position, 'f');
                for (size_t k = 0; k < g.patrol[i].looks.size(); ++k)
                    fn(LogicPointRef{oi, l, static_cast<int>(i), static_cast<int>(k), false}, g.patrol[i].looks[k].position, 'e');
            }
        }
    }
}

// The screen rectangle a logic point's marker covers, as the scene draws it (map_scene Flag / Eye /
// GroundPoint): the flag's base, pole and cloth; the eye and its base; the guard place's cube.
static bool MarkerScreenRect(App& app, const mob::Vec3& p, char kind, ImVec2 min, ImVec2 size, ImVec2& lo, ImVec2& hi) {
    const float z = app.scene.Ground(p.x, p.y);
    const Mat4 view = app.scene.camera.ViewMatrix();
    const fig::Vec3 right{view.m[0], view.m[4], view.m[8]}, up{view.m[1], view.m[5], view.m[9]};
    std::vector<fig::Vec3> pts;
    auto ring = [&](float radius, float h) {
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * i / 8;
            pts.push_back({p.x + std::cos(a) * radius, p.y + std::sin(a) * radius, z + h});
        }
    };
    if (kind == 'f') {
        const float yaw = app.scene.camera.yawDeg * 3.14159265f / 180.0f;
        ring(0.45f, 0.04f);
        pts.push_back({p.x, p.y, z + 1.4f});
        pts.push_back({p.x - std::sin(yaw) * 0.75f, p.y + std::cos(yaw) * 0.75f, z + 1.15f});
    } else if (kind == 'a') { // a trap area's centre handle
        ring(0.5f, 0.04f);
    } else if (kind == 'c') { // a trap's cast point
        ring(0.35f, 0.0f);
        ring(0.35f, 0.6f);
    } else if (kind == 'e') {
        ring(0.3f, 0.04f);
        const fig::Vec3 c{p.x, p.y, z + 0.85f};
        for (float sx : {-0.6f, 0.6f})
            for (float sy : {-0.42f, 0.45f}) pts.push_back(c + right * sx + up * sy);
    } else {
        ring(0.28f, 0.0f);
        ring(0.28f, 0.4f);
    }
    bool any = false;
    for (const fig::Vec3& q : pts) {
        float fx, fy;
        if (!app.scene.Project(q, fx, fy)) continue;
        const ImVec2 s(min.x + fx * size.x, min.y + fy * size.y);
        if (!any) { lo = hi = s; any = true; }
        lo.x = std::min(lo.x, s.x); lo.y = std::min(lo.y, s.y);
        hi.x = std::max(hi.x, s.x); hi.y = std::max(hi.y, s.y);
    }
    return any;
}

// Where a logic point is (the file's current values); false when the reference no longer fits.
static bool LogicPointAt(const mob::File& f, const LogicPointRef& r, mob::Vec3& out) {
    if (r.object < 0 || r.object >= static_cast<int>(f.objects.size())) return false;
    if (r.trap) {
        const auto& list = r.trap == 1 ? f.objects[r.object].trapAreas : f.objects[r.object].trapTargets;
        if (r.point < 0 || r.point >= static_cast<int>(list.size())) return false;
        out = {list[static_cast<size_t>(r.point)].x, list[static_cast<size_t>(r.point)].y, 0.0f};
        return true;
    }
    const auto& logics = f.objects[r.object].logics;
    if (r.logic < 0 || r.logic >= static_cast<int>(logics.size())) return false;
    const mob::Logic& g = logics[r.logic];
    if (r.place) { out = g.guardPlace; return true; }
    if (r.point < 0 || r.point >= static_cast<int>(g.patrol.size())) return false;
    const mob::PatrolPoint& p = g.patrol[r.point];
    if (r.look < 0) { out = p.position; return true; }
    if (r.look >= static_cast<int>(p.looks.size())) return false;
    out = p.looks[r.look].position;
    return true;
}

static void SetLogicPoint(mob::Logic& g, const LogicPointRef& r, const mob::Vec3& v) {
    if (r.place) g.guardPlace = v;
    else if (r.look >= 0) g.patrol[r.point].looks[r.look].position = v;
    else g.patrol[r.point].position = v;
}

// Drops selected points that no longer exist (after an undo, a delete...).
static void PruneLogicPoints(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) { app.scene.logicPoints.clear(); return; }
    const mob::File& f = app.mobs[app.activeMob]->file;
    auto& v = app.scene.logicPoints;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const LogicPointRef& r) { mob::Vec3 p; return !LogicPointAt(f, r, p); }), v.end());
}

// Deletes the selected patrol and look points (guard places stay: every guard has one). One undo step.
static void DeleteLogicPoints(App& app) {
    PruneLogicPoints(app);
    if (app.scene.logicPoints.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    std::map<std::pair<int, int>, mob::Logic> changed;
    std::map<int, std::pair<std::vector<mob::Vec3>, std::vector<mob::Vec3>>> traps;
    for (const LogicPointRef& r : app.scene.logicPoints) {
        if (r.trap) traps.emplace(r.object, std::make_pair(f.objects[r.object].trapAreas, f.objects[r.object].trapTargets));
        else if (!r.place) changed.emplace(std::make_pair(r.object, r.logic), f.objects[r.object].logics[r.logic]);
    }
    if (changed.empty() && traps.empty()) return;
    for (auto& [oi, lists] : traps) { // from the back, so the other indices stay right
        std::vector<LogicPointRef> mine;
        for (const LogicPointRef& r : app.scene.logicPoints) if (r.trap && r.object == oi) mine.push_back(r);
        std::sort(mine.begin(), mine.end(), [](const LogicPointRef& a, const LogicPointRef& b) { return a.point > b.point; });
        for (const LogicPointRef& r : mine) {
            auto& list = r.trap == 1 ? lists.first : lists.second;
            list.erase(list.begin() + r.point);
        }
    }
    for (auto& [key, g] : changed) {
        // From the back, so the indices of the rest stay right.
        std::vector<LogicPointRef> mine;
        for (const LogicPointRef& r : app.scene.logicPoints) if (!r.place && r.object == key.first && r.logic == key.second) mine.push_back(r);
        std::sort(mine.begin(), mine.end(), [](const LogicPointRef& a, const LogicPointRef& b) { return a.point != b.point ? a.point > b.point : a.look > b.look; });
        for (const LogicPointRef& r : mine) {
            if (r.look >= 0) {
                // A look point of a patrol point deleted too goes with it.
                if (std::find(mine.begin(), mine.end(), LogicPointRef{r.object, r.logic, r.point, -1, false}) != mine.end()) continue;
                g.patrol[r.point].looks.erase(g.patrol[r.point].looks.begin() + r.look);
            } else {
                g.patrol.erase(g.patrol.begin() + r.point);
            }
        }
    }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    for (auto& [key, g] : changed) mob::SetLogic(f, key.first, key.second, g);
    for (auto& [oi, lists] : traps) {
        mob::ReplaceField(f, oi, mob::kMagicTrapAreas, mob::TrapListPayload(true, lists.first));
        mob::ReplaceField(f, oi, mob::kMagicTrapTargets, mob::TrapListPayload(false, lists.second));
    }
    PushUndo(app, std::move(step), "Delete points");
    app.checksDirty = true;
    app.listedKey.clear();
    app.scene.logicPoints.clear();
}

// The mouse on logic points: a click selects (Shift: adds or removes; G then moves them), Ctrl+click on
// the ground adds a patrol point. True when it took the click.
static bool LogicHandles(App& app, ImVec2 min, ImVec2 size, ImVec2 local) {
    ImGuiIO& io = ImGui::GetIO();
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) return false;
    if (!(ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return false;
    mob::File& f = app.mobs[app.activeMob]->file;
    // The marker under the mouse (its drawn outline, a few pixels wider); of several, the smallest on
    // screen, which is the one in front or the more precise one.
    float best = 1e30f;
    LogicPointRef hit;
    bool found = false;
    EachLogicPoint(app, [&](const LogicPointRef& r, const mob::Vec3& p, char kind) {
        ImVec2 lo, hi;
        if (!MarkerScreenRect(app, p, kind, min, size, lo, hi)) return;
        const float pad = 4.0f;
        if (io.MousePos.x < lo.x - pad || io.MousePos.x > hi.x + pad || io.MousePos.y < lo.y - pad || io.MousePos.y > hi.y + pad) return;
        const float area = (hi.x - lo.x + 1) * (hi.y - lo.y + 1);
        if (area >= best) return;
        best = area;
        hit = r;
        found = true;
    });
    if (found) {
        auto& sel = app.scene.logicPoints;
        auto it = std::find(sel.begin(), sel.end(), hit);
        if (io.KeyShift) { if (it != sel.end()) sel.erase(it); else sel.push_back(hit); }
        else sel.assign(1, hit);
        app.logicRecord = hit.logic;
        app.logicRecordFor = hit.object;
        app.swallowLeftRelease = true; // not a click on the unit behind it
        return true;
    }
    fig::Vec3 g;
    const int oi = app.scene.selectedObject;
    // A selected trap: Ctrl+click adds a cast point there, Ctrl+Shift+click an activation area.
    if (io.KeyCtrl && oi >= 0 && oi < static_cast<int>(f.objects.size()) && f.objects[oi].kind == mob::Kind::MagicTrap &&
        app.scene.GroundAt(local.x, local.y, g)) {
        const bool area = io.KeyShift;
        std::vector<mob::Vec3> list = area ? f.objects[oi].trapAreas : f.objects[oi].trapTargets;
        list.push_back({std::round(g.x * 100) / 100, std::round(g.y * 100) / 100, area ? 5.0f : 0.0f});
        EditStep step;
        step.kind = EditStep::Bytes;
        step.file = f.path;
        step.bytes = f.bytes;
        if (mob::ReplaceField(f, oi, area ? mob::kMagicTrapAreas : mob::kMagicTrapTargets, mob::TrapListPayload(area, list))) {
            PushUndo(app, std::move(step), "Add a trap point");
            app.checksDirty = true;
            app.listedKey.clear();
            app.scene.logicPoints.assign(1, LogicPointRef{oi, -1, static_cast<int>(list.size()) - 1, -1, false, area ? 1 : 2});
        } else {
            app.questMessage = "This trap has no such list in the file to add to";
        }
        app.swallowLeftRelease = true;
        return true;
    }
    if (io.KeyCtrl && app.scene.logicMode && oi >= 0 && oi < static_cast<int>(f.objects.size()) && app.scene.GroundAt(local.x, local.y, g)) {
        const mob::Object& o = f.objects[oi];
        for (size_t gi = 0; gi < o.logics.size(); ++gi) {
            if (!o.logics[gi].use || o.logics[gi].model != 2) continue;
            mob::Logic l = o.logics[gi];
            mob::PatrolPoint pt;
            pt.position = {std::round(g.x * 100) / 100, std::round(g.y * 100) / 100, g.z};
            l.patrol.push_back(pt);
            CommitLogic(app, oi, static_cast<int>(gi), l);
            app.scene.logicPoints.assign(1, LogicPointRef{oi, static_cast<int>(gi), static_cast<int>(l.patrol.size()) - 1, -1, false});
            app.swallowLeftRelease = true; // not a click that selects
            return true;
        }
    }
    return false;
}

// The logic points inside a rectangle of the view (fractions), for a rectangle selection in logic mode.
static std::vector<LogicPointRef> LogicPointsInRect(App& app, ImVec2 a, ImVec2 b) {
    std::vector<LogicPointRef> out;
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    EachLogicPoint(app, [&](const LogicPointRef& r, const mob::Vec3& p, char) {
        float fx, fy;
        if (!app.scene.Project({p.x, p.y, app.scene.Ground(p.x, p.y) + 0.05f}, fx, fy)) return;
        if (fx < x0 || fx > x1 || fy < y0 || fy > y1) return;
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    });
    return out;
}

// Left click selects (Shift: adds or removes), left drag draws a selection rectangle (Shift: adds).
// The orbit button (the wheel click by default, like ei_maper) turns the camera, the drag button (right
// by default) moves the ground, the wheel zooms; both buttons are set in Settings.
static void ViewportInput(App& app, ImVec2 min, ImVec2 size) {
    ImGui::InvisibleButton("##mapview", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)), ImGuiButtonFlags_MouseButtonLeft);
    ImGuiIO& io = ImGui::GetIO();
    OrbitCamera& cam = app.scene.camera;
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // The other buttons are followed by hand (the side buttons have no ImGui button flag): a drag that
    // starts over the view keeps going until that button is let go.
    for (int b = 1; b < 5; ++b) {
        if (hovered && ImGui::IsMouseClicked(b)) {
            app.mouseHeld[b] = true;
            // Orbiting turns around what the cursor points at (an object, else the ground), not around a
            // point that may be far below or behind it. The view is not re-aimed at it: the whole camera
            // turns around it as the mouse moves.
            if (b == app.lib.mapMouseOrbit) {
                ImVec2 at((io.MousePos.x - min.x) / std::max(size.x, 1.0f), (io.MousePos.y - min.y) / std::max(size.y, 1.0f));
                app.orbitPivotSet = app.scene.PickAny(at.x, at.y, app.orbitPivot) || app.scene.GroundAt(at.x, at.y, app.orbitPivot);
            }
        }
        if (!ImGui::IsMouseDown(b)) app.mouseHeld[b] = false;
    }
    {
        if (app.mouseHeld[app.lib.mapMouseOrbit]) {
            const float yaw0 = cam.yawDeg, pitch0 = cam.pitchDeg;
            cam.Orbit(-io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
            if (app.orbitPivotSet) {
                // The target turns around the pivot with the camera: first the pitch about the view's
                // horizontal right axis, then the yaw about the vertical.
                const float d2r = 3.14159265f / 180.0f;
                const float dp = (cam.pitchDeg - pitch0) * d2r, dy = (cam.yawDeg - yaw0) * d2r;
                float v[3] = {cam.targetX - app.orbitPivot.x, cam.targetY - app.orbitPivot.y, cam.targetZ - app.orbitPivot.z};
                const float ax = -std::sin(yaw0 * d2r), ay = std::cos(yaw0 * d2r); // right axis (horizontal)
                const float th = -dp, c = std::cos(th), s = std::sin(th);
                const float dot = ax * v[0] + ay * v[1];
                const float cx = ay * v[2], cy = -ax * v[2], cz = ax * v[1] - ay * v[0]; // axis x v
                float r[3] = {v[0] * c + cx * s + ax * dot * (1 - c), v[1] * c + cy * s + ay * dot * (1 - c), v[2] * c + cz * s};
                const float cyw = std::cos(dy), syw = std::sin(dy);
                v[0] = r[0] * cyw - r[1] * syw;
                v[1] = r[0] * syw + r[1] * cyw;
                v[2] = r[2];
                cam.targetX = app.orbitPivot.x + v[0];
                cam.targetY = app.orbitPivot.y + v[1];
                cam.targetZ = app.orbitPivot.z + v[2];
            }
        }
        else if (app.mouseHeld[app.lib.mapMousePan]) {
            float k = cam.distance * 0.0016f;
            float yaw = cam.yawDeg * 3.14159265f / 180.0f;
            // The ground follows the mouse: the view's right is (-sin yaw, cos yaw), its forward -(cos yaw, sin yaw),
            // so dragging right moves the target left and dragging down moves it forward.
            float mx = io.MouseDelta.x * k, my = io.MouseDelta.y * k;
            MoveTarget(app, std::sin(yaw) * mx - std::cos(yaw) * my, -std::cos(yaw) * mx - std::sin(yaw) * my, 0.0f);
        }
    }
    if (hovered && io.MouseWheel != 0.0f) {
        cam.distance *= std::pow(0.85f, io.MouseWheel);
        cam.distance = std::max(1.5f, std::min(cam.distance, 3000.0f));
    }
    auto frac = [&](ImVec2 p) { return ImVec2((p.x - min.x) / std::max(size.x, 1.0f), (p.y - min.y) / std::max(size.y, 1.0f)); };
    ImVec2 local = frac(io.MousePos);
    app.viewHovered = hovered;
    app.viewMin = min;
    app.viewSize = size;

    // A move or scale in progress takes the mouse: it follows it, a left click applies, a right click cancels.
    if (app.xf.mode != Transform::None) {
        UpdateTransform(app, io.MousePos, min, size);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            EndTransform(app, true);
            app.swallowLeftRelease = true;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            EndTransform(app, false);
            for (bool& h : app.mouseHeld) h = false; // that right click is not a drag
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (app.swallowLeftRelease) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) app.swallowLeftRelease = false;
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    if (HeightBrushInput(app, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (TerrainBrushInput(app, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (LogicHandles(app, min, size, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    // Alt + drag on a script area: it follows the mouse; the script is rewritten once, on release.
    if (app.areaDrag.on) {
        fig::Vec3 g;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (app.areaDrag.resize && app.scene.GroundAt(local.x, local.y, g)) { // resizing: the grabbed edge follows
                float* v = app.areaDrag.v;
                const int r = app.areaDrag.resize;
                if (r == 1) v[2] = std::max(0.5f, std::hypot(g.x - v[0], g.y - v[1]));
                if (r & 2) v[0] = g.x;
                if (r & 4) v[1] = g.y;
                if (r & 8) v[2] = g.x;
                if (r & 16) v[3] = g.y;
                if (app.areaDrag.scene >= 0 && app.areaDrag.scene < static_cast<int>(app.scene.scriptAreas.size())) {
                    MapScene::ScriptArea& sa = app.scene.scriptAreas[app.areaDrag.scene];
                    if (r == 1) sa.r = v[2];
                    else { sa.x = v[0]; sa.y = v[1]; sa.x2 = v[2]; sa.y2 = v[3]; }
                }
            } else if (app.scene.GroundAt(local.x, local.y, g)) {
                const float ddx = g.x - app.areaDrag.startX - app.areaDrag.dx, ddy = g.y - app.areaDrag.startY - app.areaDrag.dy;
                app.areaDrag.dx += ddx; app.areaDrag.dy += ddy;
                if (app.areaDrag.scene >= 0 && app.areaDrag.scene < static_cast<int>(app.scene.scriptAreas.size())) {
                    MapScene::ScriptArea& s = app.scene.scriptAreas[app.areaDrag.scene];
                    s.x += ddx; s.y += ddy; s.x2 += ddx; s.y2 += ddy;
                }
            }
        } else {
            app.areaDrag.on = false;
            if (MobEntry* m = FindMob(app, app.areaDrag.file)) {
                std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
                if (app.areaDrag.resize && app.areaDrag.index < calls.size()) {
                    quests::AreaCall a = calls[app.areaDrag.index];
                    const int n = a.round ? 3 : 4;
                    bool changed = false;
                    for (int k = 0; k < n; ++k) {
                        const float v = std::round(app.areaDrag.v[k] * 10.0f) / 10.0f; // tenths, like the panel
                        changed |= v != a.v[k];
                        a.v[k] = v;
                    }
                    if (changed) SetAreaCall(app, *m, a);
                } else if (app.areaDrag.index < calls.size() && (app.areaDrag.dx != 0 || app.areaDrag.dy != 0)) {
                    quests::AreaCall a = calls[app.areaDrag.index];
                    a.v[0] += app.areaDrag.dx; a.v[1] += app.areaDrag.dy;
                    if (!a.round) { a.v[2] += app.areaDrag.dx; a.v[3] += app.areaDrag.dy; }
                    SetAreaCall(app, *m, a);
                }
            }
            app.scriptModelKey.clear(); // the drawn areas come back from the script
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    // A click on a script area's square: move it (the centre) or resize it (a corner, a side, the rim).
    if (!io.KeyAlt && !io.KeyCtrl && app.scene.options.scriptAreas && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ScriptHandle* best = nullptr;
        float bestD = 9.0f * 9.0f;
        const std::vector<ScriptHandle> handles = ScriptHandles(app);
        for (const ScriptHandle& h : handles) {
            float fx, fy;
            if (!app.scene.Project({h.x, h.y, app.scene.Ground(h.x, h.y) + 0.12f}, fx, fy)) continue;
            const float dx = min.x + fx * size.x - io.MousePos.x, dy = min.y + fy * size.y - io.MousePos.y;
            if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = &h; }
        }
        fig::Vec3 g;
        if (best && app.scene.GroundAt(local.x, local.y, g)) {
            app.areaDrag = App::AreaDrag{};
            app.areaDrag.on = true;
            app.areaDrag.file = best->file;
            app.areaDrag.index = best->index;
            app.areaDrag.startX = g.x;
            app.areaDrag.startY = g.y;
            app.areaDrag.resize = best->resize;
            const std::vector<quests::AreaCall> calls = quests::AreaCalls(FindMob(app, best->file)->file.script);
            const quests::AreaCall& a = calls[best->index];
            for (int k = 0; k < 4; ++k) app.areaDrag.v[k] = a.v[k];
            for (size_t i = 0; i < app.scene.scriptAreas.size(); ++i) {
                const MapScene::ScriptArea& sa = app.scene.scriptAreas[i];
                if (sa.round == a.round && sa.id == a.id && std::fabs(sa.x - a.v[0]) < 1e-3f && std::fabs(sa.y - a.v[1]) < 1e-3f) { app.areaDrag.scene = static_cast<int>(i); break; }
            }
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    if (io.KeyAlt && app.scene.options.scriptAreas && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        fig::Vec3 g;
        std::string file;
        size_t index = 0;
        if (app.scene.GroundAt(local.x, local.y, g) && AreaAt(app, g.x, g.y, file, index)) {
            app.areaDrag = App::AreaDrag{};
            app.areaDrag.on = true;
            app.areaDrag.file = file;
            app.areaDrag.index = index;
            app.areaDrag.startX = g.x;
            app.areaDrag.startY = g.y;
            // Grabbed near its edge: a resize (the rim of a round area; a rect's side or corner).
            {
                const quests::AreaCall a = quests::AreaCalls(FindMob(app, file)->file.script)[index];
                const float tol = std::max(0.4f, app.scene.camera.distance * 0.012f);
                for (int k = 0; k < 4; ++k) app.areaDrag.v[k] = a.v[k];
                if (a.round) {
                    if (std::fabs(std::hypot(g.x - a.v[0], g.y - a.v[1]) - a.v[2]) < tol) app.areaDrag.resize = 1;
                } else {
                    if (std::fabs(g.x - a.v[0]) < tol) app.areaDrag.resize |= 2;
                    if (std::fabs(g.y - a.v[1]) < tol) app.areaDrag.resize |= 4;
                    if (std::fabs(g.x - a.v[2]) < tol) app.areaDrag.resize |= 8;
                    if (std::fabs(g.y - a.v[3]) < tol) app.areaDrag.resize |= 16;
                }
            }
            // The drawn shape of this call (same id and place).
            for (size_t i = 0; i < app.scene.scriptAreas.size(); ++i) {
                const MapScene::ScriptArea& s = app.scene.scriptAreas[i];
                const std::vector<quests::AreaCall> calls = quests::AreaCalls(FindMob(app, file)->file.script);
                const quests::AreaCall& a = calls[index];
                if (s.round == a.round && s.id == a.id && std::fabs(s.x - a.v[0]) < 1e-3f && std::fabs(s.y - a.v[1]) < 1e-3f) { app.areaDrag.scene = static_cast<int>(i); break; }
            }
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    // Script tab -> Areas -> Place here: the click puts the area's centre there.
    if (app.areaPlace.on) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) app.areaPlace.on = false;
        else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            fig::Vec3 g;
            if (app.scene.GroundAt(local.x, local.y, g)) PlaceArea(app, g.x, g.y);
            app.areaPlace.on = false;
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    // Left button on an area handle of the open quest: resize or move that area.
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        fig::Vec3 g;
        if (app.scene.GroundAt(local.x, local.y, g)) GrabAreaHandle(app, io.MousePos, min, size, g);
    }
    if (app.rectDrag.on) {
        fig::Vec3 g;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (app.scene.GroundAt(local.x, local.y, g)) DragAreaHandle(app, g, !io.KeyShift);
        } else {
            app.rectDrag.on = false;
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    // Left button: a rectangle while dragging, a pick on a click.
    const ImVec2 start = io.MouseClickedPos[ImGuiMouseButton_Left];
    const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                          ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);
    if (dragging) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start, io.MousePos, IM_COL32(255, 220, 120, 40));
        draw->AddRect(start, io.MousePos, IM_COL32(255, 220, 120, 220), 0.0f, 0, 1.5f);
    }
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        const bool additive = io.KeyShift;
        std::vector<LogicPointRef> pts;
        if (drag.x * drag.x + drag.y * drag.y >= 16.0f) pts = LogicPointsInRect(app, frac(start), local);
        if (!pts.empty()) { // logic mode: a rectangle over the selected units' points selects those
            if (!additive) app.scene.logicPoints.clear();
            for (const LogicPointRef& r : pts)
                if (!app.scene.IsLogicPointSelected(r)) app.scene.logicPoints.push_back(r);
        } else if (drag.x * drag.x + drag.y * drag.y >= 16.0f) { // a rectangle
            ImVec2 a = frac(start), b = local;
            std::vector<int> inside = app.scene.ObjectsInRect(a.x, a.y, b.x, b.y);
            if (!additive) app.scene.ClearSelection();
            for (int oi : inside) {
                if (app.scene.IsSelected(app.activeMob, oi)) continue;
                if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
                else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
            }
            if (!inside.empty()) { app.requestTab = SideTab::Objects; app.revealSelection = true; }
        } else { // a click
            int fi = -1, oi = -1;
            if (app.scene.Pick(local.x, local.y, fi, oi)) {
                if (additive) app.scene.Toggle(fi, oi);
                else SelectObject(app, fi, oi, false);
                app.requestTab = SideTab::Objects;
                app.revealSelection = true;
            } else if (!additive) {
                app.scene.ClearSelection();
            }
        }
    }
    app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
}

// ------------------------------------------------------------------------------------------------
// Minimap export
// ------------------------------------------------------------------------------------------------

// An uncompressed 32-bit BGRA .dds (A8R8G8B8, one mip level), which texture tools and um-multitool
// ddsmmp read.
static bool WriteDds32(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    uint32_t header[32] = {};
    header[0] = 0x20534444u;                        // "DDS "
    header[1] = 124;                                // header size
    header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;     // caps, height, width, pitch, pixel format
    header[3] = static_cast<uint32_t>(h);
    header[4] = static_cast<uint32_t>(w);
    header[5] = static_cast<uint32_t>(w) * 4;       // pitch
    header[19] = 32;                                // pixel format size
    header[20] = 0x40 | 0x1;                        // RGB, alpha pixels
    header[22] = 32;                                // bits per pixel
    header[23] = 0x00FF0000u; header[24] = 0x0000FF00u; header[25] = 0x000000FFu; header[26] = 0xFF000000u;
    header[27] = 0x1000;                            // texture
    f.write(reinterpret_cast<const char*>(header), sizeof(header));
    std::vector<uint8_t> bgra(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = rgba[i + 3];
    }
    f.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    return f.good();
}

// A 16-bit RGBA5551 ("QU") .mmp, the format of the game's own quest map textures (e.g. zone8quest.mmp).
static bool WriteMmp5551(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    const uint32_t header[19] = {0x00504D4Du, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1, 0x00005551u, 16,
                                 0x8000, 15, 1, 0x7C00, 10, 5, 0x03E0, 5, 5, 0x001F, 0, 5, 0};
    f.write(reinterpret_cast<const char*>(header), sizeof(header));
    f.seekp(16);
    f.write("QU\0\0", 4); // the fourcc bytes as the game writes them
    f.seekp(0, std::ios::end);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 2);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t* p = &rgba[i * 4];
        uint16_t v = static_cast<uint16_t>(((p[3] >= 128) << 15) | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
        px[i * 2] = static_cast<uint8_t>(v & 0xFF);
        px[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
    return f.good();
}

static void NewTerrainDialog(App& app) {
    if (!app.newTerrainOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMin.y + 40), ImGuiCond_Appearing);
    if (!ImGui::Begin("New terrain", &app.newTerrainOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    ImGui::TextWrapped("A flat terrain, every tile the same, with the textures, materials and tile types of the open terrain (%s). "
                       "Then paint it, raise it, add water as usual.", app.terrain.name.c_str());
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2("Sectors (X, Y)", app.newTerrainSize);
    for (int& v : app.newTerrainSize) v = std::clamp(v, 1, 64);
    ImGui::SameLine();
    ImGui::TextDisabled("= %d x %d units", app.newTerrainSize[0] * 32, app.newTerrainSize[1] * 32);
    ImGui::SetNextItemWidth(160);
    ImGui::DragFloat("Ground height", &app.newTerrainHeight, 0.1f, 0.0f, 200.0f, "%.1f");
    const uint16_t tile = PackTile(std::max(app.brushTile, 0), app.brushRotation);
    ImGui::Text("Tile: texture %d, tile %d (the tile brush's)", (tile >> 6) & 0xFF, tile & 63);
    ImGui::Checkbox("Its own textures", &app.newTerrainOwnTextures);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the new terrain keeps the name inside the file (%s), so it uses that terrain's textures.\n"
                          "On: it is named after the file, and copies of the textures are written next to it\n"
                          "(<name>000.mmp...): add that folder to the texture sources, or pack them into textures.res.", app.terrain.name.c_str());
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##newterrain", app.newTerrainPath, sizeof(app.newTerrainPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.newTerrainPath, picked, "mpr")) std::snprintf(app.newTerrainPath, sizeof(app.newTerrainPath), "%s", picked.c_str());
    ImGui::BeginDisabled(!app.terrainLoaded || app.newTerrainPath[0] == '\0');
    if (ImGui::Button("Create and open", ImVec2(160, 0))) {
        std::string path = app.newTerrainPath, err;
        if (!EndsWith(path, ".mpr")) path += ".mpr";
        std::string stem = std::filesystem::path(path).stem().string();
        for (char& ch : stem) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::string name = app.newTerrainOwnTextures ? stem : app.terrain.name;
        mpr::Map made;
        if (std::filesystem::exists(path)) app.newTerrainMessage = path + " exists already: pick another name";
        else if (!mpr::Create(app.terrain, name, app.newTerrainSize[0], app.newTerrainSize[1], app.newTerrainHeight, tile, path, made, err))
            app.newTerrainMessage = "Not created: " + err;
        else {
            std::string copied;
            if (app.newTerrainOwnTextures)
                for (int i = 0; i < app.terrain.textureCount; ++i) {
                    std::vector<uint8_t> bytes;
                    std::string found;
                    if (!app.lib.textures.ReadTexture(app.terrain.name + "00" + std::to_string(i), bytes, &found)) continue;
                    const std::string ext = std::filesystem::path(found).extension().string();
                    const std::string out = (std::filesystem::path(path).parent_path() / (name + "00" + std::to_string(i) + (ext.empty() ? ".mmp" : ext))).string();
                    std::ofstream f(out, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (f) copied += (copied.empty() ? "" : ", ") + std::filesystem::path(out).filename().string();
                }
            app.newTerrainMessage = "Created " + path + (copied.empty() ? "" : "; textures written: " + copied);
            LoadTerrain(app, path);
        }
    }
    ImGui::EndDisabled();
    if (!app.newTerrainMessage.empty()) ImGui::TextWrapped("%s", app.newTerrainMessage.c_str());
    ImGui::End();
}

static void MinimapDialog(App& app) {
    if (!app.minimapOpen) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMin.y + 40), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export minimap", &app.minimapOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    ImGui::TextWrapped("The terrain seen straight from above, like the game's <zone>map textures: a square image, the map in its "
                       "top-left corner with its longer side filling it, y = 0 at the top.");
    static const int sizes[] = {256, 512, 1024, 2048};
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("Size", (std::to_string(app.minimapSize) + " px").c_str())) {
        for (int sz : sizes) if (ImGui::Selectable((std::to_string(sz) + " px").c_str(), sz == app.minimapSize)) app.minimapSize = sz;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Objects", &app.minimapObjects);
    ImGui::SameLine();
    ImGui::Checkbox("Units", &app.minimapUnits);
    ImGui::TextDisabled("Lighting follows the view (Lighting on: the map's lighting file and hour).");
    ImGui::TextUnformatted("Save as");
    ImGui::SameLine();
    ImGui::Checkbox(".mmp", &app.minimapMmp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("16-bit RGBA5551, the format of the game's own quest map textures");
    ImGui::SameLine();
    ImGui::Checkbox(".dds", &app.minimapDds);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uncompressed 32-bit (A8R8G8B8): for texture tools, or um-multitool ddsmmp");
    ImGui::SameLine();
    ImGui::Checkbox(".png", &app.minimapPng);
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##minipath", app.minimapPath, sizeof(app.minimapPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.minimapPath, picked, "mmp")) std::snprintf(app.minimapPath, sizeof(app.minimapPath), "%s", picked.c_str());
    ImGui::BeginDisabled(!app.terrainLoaded || app.minimapPath[0] == '\0' || app.scene.modelsPending > 0 ||
                         !(app.minimapMmp || app.minimapDds || app.minimapPng));
    if (ImGui::Button("Export", ImVec2(120, 0))) { app.minimapPending = true; app.minimapMessage = "Exporting..."; }
    ImGui::EndDisabled();
    if (app.scene.modelsPending > 0) { ImGui::SameLine(); ImGui::TextDisabled("waiting for figures to load"); }
    if (!app.minimapMessage.empty()) ImGui::TextWrapped("%s", app.minimapMessage.c_str());
    ImGui::End();
}

static void ExportMinimap(App& app, int fbW, int fbH) {
    std::vector<uint8_t> rgba;
    if (!app.scene.RenderMinimap(app.lib, app.minimapSize, app.minimapObjects, app.minimapUnits, fbW, fbH, rgba)) {
        app.minimapMessage = "Nothing to export: load a terrain first";
        return;
    }
    // The path's extension, if one of the formats, is dropped: each format gets its own.
    std::string base = app.minimapPath;
    for (const char* ext : {".mmp", ".dds", ".png"}) if (EndsWith(base, ext)) { base.resize(base.size() - 4); break; }
    const int n = app.minimapSize;
    std::vector<std::string> saved, failed;
    auto one = [&](bool want, const char* ext, bool (*write)(const std::string&, int, int, const std::vector<uint8_t>&)) {
        if (want) (write(base + ext, n, n, rgba) ? saved : failed).push_back(base + ext);
    };
    one(app.minimapMmp, ".mmp", WriteMmp5551);
    one(app.minimapDds, ".dds", WriteDds32);
    one(app.minimapPng, ".png", [](const std::string& p, int w, int h, const std::vector<uint8_t>& px) { return png::Write(p, w, h, px); });
    auto join = [](const std::vector<std::string>& v) { std::string r; for (const auto& x : v) r += (r.empty() ? "" : ", ") + x; return r; };
    app.minimapMessage.clear();
    if (!saved.empty()) app.minimapMessage = "Saved " + join(saved);
    if (!failed.empty()) app.minimapMessage += (saved.empty() ? "" : "; ") + std::string("could not write ") + join(failed);
}

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

bool Busy(Context* ctx) { return ctx->app.scene.modelsPending > 0 || ctx->app.terrainDirty; }

void DrawTab(Context* ctx) {
    App& app = ctx->app;
    if (app.checksDirty || app.checkedVersion != app.lib.version) RunChecks(app);
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
        if (app.mobs.empty() && !app.terrainLoaded) {
            ImGui::SetCursorScreenPos(ImVec2(min.x + 20, min.y + 20));
            ImGui::TextUnformatted("Open a terrain (.mpr) and one or more maps (.mob) in the Files tab.\n"
                                   "Figures and textures come from the sources in the Settings tab.");
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + size.y + ImGui::GetStyle().ItemSpacing.y));
        BottomBar(app);
        ImGui::EndChild();
    };
    if (split.panelRight) { view(); split.Bar(); Sidebar(app, panelWidth, split.height); }
    else { Sidebar(app, panelWidth, split.height); split.Bar(); view(); }
    app.viewportMin = min;
    app.viewportMax = ImVec2(min.x + size.x, min.y + size.y);
    app.drawnThisFrame = true;
    MinimapDialog(app);
    MissingWindow(app);
    NewTerrainDialog(app);
    FindWindow(app);
    OffsetWindow(app);
    RandomizeWindow(app);
    SaveAsWindow(app);
    HistoryWindow(app);
    MobParamsWindow(app);
    SimulationWindow(app);
    NewObjectWindow(app);
    PollExternalScript(app);
}

void RenderGl(Context* ctx, int fbW, int fbH, float scale) {
    App& app = ctx->app;
    if (!app.drawnThisFrame) return;
    app.drawnThisFrame = false;
    if (scale <= 0) scale = 1.0f;

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
    app.scene.AnimateModels(app.lib, ImGui::GetTime()); // units play their idle (or walk) animation
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
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);
    app.scene.Draw(app.lib, vx, vy, vw, vh);
    glDisable(GL_SCISSOR_TEST);
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
            if (!mob::HasFigure(o.kind) || o.kind == mob::Kind::Unit) continue;
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
    if (db.Empty()) std::printf("note: no items database set in the GUI's Settings tab: item, spell and prototype names are not checked\n");
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
