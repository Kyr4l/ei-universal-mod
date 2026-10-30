// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Map Editor's shared state and helpers: what every map_*.cpp file of the editor sees. The App struct
// (everything the tab holds), the edit history types, and the functions one file defines for the others
// (listed by the file that defines them). Not for the rest of the program: map_app.hpp is the public face.
#pragma once

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
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "../viewer/library.hpp"
#include "../viewer/ui_common.hpp"
#include "tile_autoblend.hpp"
#include "tile_paint.hpp"
#include "tile_blend.hpp"
#include "tile_materials.hpp"
#include "script_docs.hpp"
#include "../viewer/png_writer.hpp"
#include "../viewer/ui_sources.hpp"
#include "checks.hpp"
#include "lighting.hpp"
#include "map_scene.hpp"
#include "../viewer/scene.hpp"
#include "../viewer/ui_objects.hpp"
#include "navmesh_gen.hpp"
#include "quest_file.hpp"
#include "text_codec.hpp"
#include "script_highlight.hpp"
#include "script_ast.hpp"
#include "../dllconnect/quests.hpp"
#include "../subtools.hpp"
#include "../log.hpp"
#include "../texedit/quest_map.hpp"

namespace mapedit {

struct MobEntry {
    mob::File file;
    bool visible = true;
    std::vector<uint8_t> savedBytes; // the file as on disk: edits patch file.bytes, so dirty = they differ
    bool Dirty() const { return file.loaded && file.bytes != savedBytes; }
};

// Slow work (the navmesh) runs on a thread while a modal window shows that the editor is working, not hung
// (#91); `apply` runs on the main thread when it is done. Nothing is edited meanwhile: the modal eats the input.
struct Job {
    std::thread thread;
    std::atomic<bool> done{false};
    bool running = false;
    std::string title;
    std::function<void()> apply;
    std::chrono::steady_clock::time_point started;
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
inline std::vector<float> TypedValues(const std::string& typed, int count) {
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

// The Script tab's node view of one map: where it is panned and zoomed (ScriptNodes).
namespace nodegraph { struct View { ImVec2 pan{0, 0}; float zoom = 1.0f; bool placed = false; }; }

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
    bool toolShelf = true, sidebarShown = true; // the Blender-like panels (T / N): the mode's tools in the view, the tabs beside it
    bool materialBrush = false;       // Tile paint by material: the transitions chosen by themselves (tile_materials.hpp)
    int brushMaterialId = -1, materialSeed = 1;
    bool autoBlendSameKind = false;   // "Blend the hard edges": also between two kinds of one ground (grass/dark - grass/common)
    int paintTabWanted = -1;          // Tile paint: select the Grounds (0) or Tiles (1) tab at the next frame
    int materialRadius = 1;           // the ground brush: cells around the mouse (1 = one cell; F + mouse)
    std::vector<std::pair<int, int>> leftCells; // the cells the ground painter left (no tile): tinted in the view (#109)
    double leftCellsUntil = 0;        // ... until this time (ImGui::GetTime)
    // Resting (Settings > Performance "Idle at 25 FPS"): the last drawn 3D view kept in a texture and shown
    // again while nothing in the scene changed (#110), instead of drawing the scene 25 times a second.
    GLuint idleTex = 0;
    int idleTexW = 0, idleTexH = 0;   // the texture's size (powers of two)
    int idleVx = 0, idleVy = 0, idleVw = 0, idleVh = 0; // the viewport it holds
    bool idleValid = false;
    std::vector<unsigned char> idleKey; // what the scene was drawn from (SceneKey)
    int idlePendingModels = 0;        // scene.modelsPending at the last frame
    bool materialRepair = true;       // ... three grounds at a cell with no tile for them: the junction moves a vertex
    bool materialBridge = true;       // ... a ground the brush cannot blend with: a band of a middle ground between them
    char materialSearch[64] = "";     // the grounds' search box
    std::map<std::string, GLuint> borrowThumbs; // "<terrain>/<tile>" -> a thumbnail of another allod's tile (0 = not readable)
    std::string borrowMessage;
    std::set<std::string> sidecarLoaded; // terrains whose borrowed-materials sidecar was merged this session
    std::string materialMessage;
    int quickTiles[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool tilePickerOpen = false;             // Space over the view in Tile paint: TilePicker opens at the mouse
    ImVec2 tilePickerPos{};                  // and stays there
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
    bool dipMarkAsym = true;          // Diplomacy: a corner mark on the cells whose mirror cell differs
    int dipSelected = -1;             // Diplomacy: the selected group (its units are marked in the view)

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
    float scriptViewScrollY = 0.0f;          // the Script tab's scroll in view mode (kept across Edit / Apply / Cancel)
    int scriptEditGotoLine = -1;             // entering edit mode: the cursor goes to this line (0-based), consumed by the callback
    int scriptEditScrollLine = -1;           // then the edit box scrolls so that line is at the top (consumed after the box is drawn)
    int scriptViewGotoLine = -1;             // leaving edit mode: the view scrolls to this line (0-based)
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
    std::string unloadAsk;                   // a file to unload that has unsaved changes: UnloadDialog asks what to do with them
    // The Script tab's Nodes mode (ScriptNodes): on, each map's pan and zoom, the script frame to go to,
    // the line the Checks tab asked for (its node flashes).
    bool scriptVisual = false;
    std::map<std::string, nodegraph::View> nodeViews;
    std::string nodeGoto;
    int nodeFlashLine = 0;
    double nodeFlashUntil = 0;
    bool checkedScripts = true, checkedDatabase = true, checkedAtd = false; // Settings > Checks when the checks last ran (a change runs them again)
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
    std::unique_ptr<struct Job> job;    // the slow work in progress (#91)
    // Merge from another map (#92): the objects of one loaded .mob copied into the active one, in place
    bool mergeOpen = false;
    int mergeSource = -1;
    bool mergeKinds[8] = {true, false, true, true, false, true, true, true}; // by mob::Kind: objects, units, levers, torches, traps, lights, particles, sounds
    char mergeFilter[96] = "";
    float mergeDupDist = 1.0f;   // a source object this close to one of the same figure in the active map is a duplicate
    bool mergeOnlySelectedArea = false;
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
    // the picture's tweaks (#106): levels, gamma, blur, invert, the lowest height
    bool newTerrainInvert = false;
    float newTerrainBlack = 0.0f, newTerrainWhite = 1.0f, newTerrainGamma = 1.0f, newTerrainBase = 0.0f;
    int newTerrainBlur = 0;
    GLuint newTerrainPreviewTex = 0;
    std::string newTerrainPreviewKey;
    char newTerrainImage[512] = "";   // New terrain: heights from a picture (its brightness: black = 0, white = Ground height)
    bool cropOpen = false;            // Tools > Crop terrain
    int cropRect[4] = {0, 0, 4, 4};   // x, y, width, height in sectors
    char cropPath[512] = "";
    bool cropMobs = true;
    std::string cropMessage;
    struct CropDrag { bool on = false; int handle = -1; int start[4] = {0, 0, 0, 0}; fig::Vec3 from; } cropDrag; // the rectangle dragged in the view
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

// Types two files share.
// A quest map's base map and terrain (from its .mq) that are not loaded yet.
struct QuestSuggestion { bool any = false; std::string baseMob, terrain, label; };

// The navmesh built on a thread from the terrain and the open maps' objects (read only meanwhile, the modal sees
// to it): `onDone` gets the payload (empty on failure), the error and the objects left out.
struct NavResult { std::vector<uint8_t> payload; std::string err; int missing = 0; };

// ---- functions defined in one file and used by others (the file that defines them) -------------------

// map_files.cpp
std::string Lower(std::string s);
bool EndsWith(const std::string& s, const char* ext);
void SaveSession(App& app);
void SyncScene(App& app);
bool LoadTerrain(App& app, const std::string& path);
bool AddMob(App& app, const std::string& path);
QuestSuggestion SuggestForQuest(const App& app);
void SetActiveMob(App& app, int index);
bool BlockedByUnsaved(App& app, const MobEntry* only = nullptr);
bool TerrainUnsaved(const App& app);
void UnloadFile(App& app, std::string path);
bool FileUnsaved(const App& app, const std::string& path);
void RequestUnload(App& app, std::string path);
void UnloadLast(App& app);
std::string ResolveMapFile(const App& app, const std::string& fileName, const std::string& questPath);
void FocusRect(App& app, const quest::Rect& r);
bool SameAreas(const std::vector<quest::Exit>& a, const std::vector<quest::Exit>& b);
void PushUndo(App& app, EditStep step, std::string label = std::string());
void PushAreaUndo(App& app, const std::vector<quest::Exit>& before);
MobEntry* FindMob(App& app, const std::string& path);
std::vector<ObjectState> CaptureObjects(const mob::File& f, const std::vector<int>& indices);
void ApplyObjects(mob::File& f, const std::vector<ObjectState>& states);
void UndoRedo(App& app, bool redo);
void CloseQuest(App& app);
void SetOpenQuest(App& app, const quest::QuestSet& set);
void ReloadQuest(App& app, bool keepHistory = true);
void SaveQuestAreas(App& app);
void OpenQuest(App& app, const quest::QuestSet& listed);
void RescanQuests(App& app);
void OpenPaths(App& app, const std::vector<std::string>& paths);
void RefreshScriptModel(App& app);
void RunChecks(App& app);
ImVec4 SeverityColor(char s);

// map_sidebar.cpp
void SelectObject(App& app, int file, int object, bool focus);
void FilesTab(App& app);
bool EditFloats(const char* id, int n, const float* current, float* out, const char* format = "%.3f", float width = -1.0f);
std::string ToCp(const std::string& utf8);
void Label(const char* text);
void CommitLogic(App& app, int objectIndex, int logicIndex, const mob::Logic& g);
std::string ObjectLabel(const mob::Object& o);
void ObjectsTab(App& app);
void ChecksTab(App& app);

// map_script.cpp
void ApplyScriptEdit(App& app);
void PollExternalScript(App& app);
void SetAreaCall(App& app, MobEntry& m, const quests::AreaCall& a);
bool AreaAt(App& app, float x, float y, std::string& file, size_t& index);
void PlaceArea(App& app, float x, float y);
void ScriptTab(App& app);
void ScriptContent(App& app);

// map_quest.cpp
bool AnyMobDirty(const App& app);
bool SaveTerrain(App& app, const std::string& path);
void PollJob(App& app);
void StartNavmeshJob(App& app, std::function<void(const NavResult&)> onDone);
void BuildCompareNavmesh(App& app);
void SaveQuestChanges(App& app);
void UnloadDialog(App& app);
void QuestTab(App& app);

// map_objects.cpp
void DeleteSelection(App& app);
std::filesystem::path ClipboardFile();
void CopySelection(App& app);
bool InsertCopies(App& app, std::vector<App::Copied> items, bool inPlace, mob::Vec3 at);
void Paste(App& app, bool atCursor);
void Duplicate(App& app);
void MissingWindow(App& app);
void RenderPreview(App& app);
void NewObjectWindow(App& app);
void SelectAll(App& app);
void OffsetWindow(App& app);
void RandomizeWindow(App& app);
void FindWindow(App& app);
void DiplomacyTab(App& app);
void IdsTab(App& app);

// map_terrain.cpp
uint16_t PackTile(int tile, int rotation);
bool TileAt(const mpr::Map& m, float x, float y, int& sector, int& row, int& col);
bool TerrainBrushInput(App& app, ImVec2 local);
bool HeightBrushInput(App& app, ImVec2 local);
void HeightBrushKeys(App& app);
void TerrainBrushKeys(App& app);
void ToTilesTab(App& app);
void TurnTileAtMouse(App& app, int dir);
void TerrainBrushOutline(App& app, ImDrawList* draw, ImVec2 min, ImVec2 size);
void TileImage(App& app, int tile, float side, int rotation = 0);
void SculptPanel(App& app, bool inShelf = false);
void TilePicker(App& app);
void TilePanel(App& app, bool inShelf = false);
void Sidebar(App& app, float width, float height);
void RefreshLighting(App& app);
const lighting::Table* ChosenLighting(const App& app);
float MapHour(const App& app);
void ApplyLighting(App& app);
void ResetLogicPaths(App& app);
void SaveAsWindow(App& app);
void HistoryWindow(App& app);
void MobParamsWindow(App& app);

// map_simulation.cpp
void StepSimulation(App& app, float dt);
void SimulationWindow(App& app);
void Toolbar(App& app);
void BottomBar(App& app);
void MoveTarget(App& app, float dx, float dy, float dz);

// map_transform.cpp
void StartTransform(App& app, Transform::Mode mode, ImVec2 mouse, ImVec2 min, ImVec2 size);
void Keys(App& app);
void ToolShelf(App& app, ImVec2 min, ImVec2 size);
void Overlays(App& app, ImVec2 min, ImVec2 size);
bool LogicPointAt(const mob::File& f, const LogicPointRef& r, mob::Vec3& out);
void SetLogicPoint(mob::Logic& g, const LogicPointRef& r, const mob::Vec3& v);
void PruneLogicPoints(App& app);
void DeleteLogicPoints(App& app);
void ViewportInput(App& app, ImVec2 min, ImVec2 size);

// map_minimap.cpp
void CropDialog(App& app);
void NewTerrainDialog(App& app);
void MergeWindow(App& app);
void MinimapDialog(App& app);
void ExportMinimap(App& app, int fbW, int fbH);

// map_app.cpp
std::vector<navgen::Object> NavObjects(const LayeredAssetSource& figures, const std::vector<const mob::File*>& maps, int* missing);

} // namespace mapedit
