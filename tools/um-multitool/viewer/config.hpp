// Remembers the GUI's settings between sessions in um-multitool.cfg beside the executable: the
// sources (figure/texture/text layers, database) shared by the 3D Viewer and the Map Editor, the
// viewer's rotations and GIF settings, and the map editor's files. "KEY=value" lines, layer keys
// repeated in load order (later = higher priority). Older versions wrote um-multitool-viewer.cfg,
// which is read when um-multitool.cfg does not exist yet.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace config {

inline std::string ExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, len);
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, static_cast<size_t>(len));
#endif
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

inline std::string Path() { return ExeDir() + "/um-multitool.cfg"; }
inline std::string LegacyPath() { return ExeDir() + "/um-multitool-viewer.cfg"; }

// GIF export settings (see the viewer's "Export GIF" dialog).
struct GifSettings {
    int size = 384;                 // square, in pixels
    int fps = 25;
    float degreesPerSecond = 60.0f; // one full turn takes 360 / this seconds
    bool reverse = false;           // turn the other way
    int axis = 2;                   // the axis the model spins about: 0 = X, 1 = Y, 2 = Z (vertical)
    bool transparent = true;        // no background
    unsigned background = 0x28282E; // RRGGBB, used when not transparent
    std::string lastDirectory;      // where the last GIF went
};

// The Map Editor's keys. A key is stored as a GLFW key code, which names a key's POSITION on the
// keyboard (after the US layout), not its letter: the defaults are the keys where W A S D Q E sit on
// a US keyboard, which an AZERTY keyboard labels Z Q S D A E - the same hand position everywhere.
// The Settings tab shows each key by the letter the current layout gives it. Movement keys are held
// (modifiers ignored, so Shift can speed them up); action keys fire once per press and need exactly
// their modifiers (Ctrl+T is not T).
enum MapKey {
    kKeyForward, kKeyBack, kKeyLeft, kKeyRight, kKeyUp, kKeyDown, kKeyFast,   // held
    kKeyLogicMode, kKeySwitchMob, kKeyUnloadLast, kKeyResetCamera, kKeyLighting, kKeySave, kKeyUndo, kKeyRedo,
    kKeyMove, kKeyScale, kKeyFind, kKeySelectAll, kKeyRotate,
    kKeyDelete, kKeyCopy, kKeyPaste, kKeyDuplicate, kKeyResetPaths, kKeyNewObject, // pressed
    kKeyToolShelf, kKeySidebar, // the Blender-like panels: the tool shelf in the view (terrain modes), the sidebar
    kMapKeyCount
};
constexpr int kFirstActionKey = kKeyLogicMode;
// kModLetter: the key is a LETTER, not a position: the key that prints it on the current layout (Ctrl+A
// selects all on any layout; AZERTY's A is not at the US A position).
enum KeyMods { kModCtrl = 1, kModShift = 2, kModAlt = 4, kModLetter = 8 };

struct KeyBind {
    int key = 0;  // GLFW key code
    int mods = 0; // KeyMods, for action keys
    bool operator==(const KeyBind& o) const { return key == o.key && mods == o.mods; }
};

inline const char* MapKeyId(int k) {
    static const char* const ids[kMapKeyCount] = {"FORWARD", "BACK", "LEFT", "RIGHT", "UP", "DOWN", "FAST",
                                                  "LOGIC_MODE", "SWITCH_MOB", "UNLOAD_LAST", "RESET_CAMERA", "LIGHTING", "SAVE",
                                                  "UNDO", "REDO", "MOVE", "SCALE", "FIND", "SELECT_ALL", "ROTATE",
                                                  "DELETE", "COPY", "PASTE", "DUPLICATE", "RESET_PATHS", "NEW_OBJECT", "TOOL_SHELF", "SIDEBAR"};
    return ids[k];
}
inline const char* MapKeyLabel(int k) {
    static const char* const labels[kMapKeyCount] = {"Forward", "Back", "Left", "Right", "Up", "Down", "Faster (hold)",
                                                     "Logic mode on/off", "Next active map (list shown until Ctrl is released)", "Unload the last loaded file",
                                                     "Reset the camera", "Lighting on/off", "Save the quest's changes",
                                                     "Undo", "Redo", "Move the selection (then X/Y/Z)",
                                                     "Scale the selection's complection (then X/Y/Z)", "Find objects",
                                                     "Select everything in the active map", "Rotate the selection (then X/Y/Z)",
                                                     "Delete the selection", "Copy the selection", "Paste (at the mouse)",
                                                     "Duplicate the selection (then move it)",
                                                     "Clear the patrol paths of the selected units",
                                                     "Add an object or a unit (the Add window)",
                                                     "Tool shelf in the view on/off (Tile paint and Sculpt modes)", "Sidebar on/off"};
    return labels[k];
}
inline std::array<KeyBind, kMapKeyCount> DefaultMapKeys() {
    // GLFW_KEY_W, S, A, D, E (up), Q (down), LEFT_SHIFT; Ctrl+Tab, Ctrl+T, U, Ctrl+R, Ctrl+L, Ctrl+S, Ctrl+Z, Ctrl+Y,
    // G (move) as in Blender, T (scale; Blender's S is "back" here), Ctrl+F.
    return {{{87, 0}, {83, 0}, {65, 0}, {68, 0}, {69, 0}, {81, 0}, {340, 0},
             {258, kModCtrl}, {'T', kModCtrl | kModLetter}, {85, 0}, {'R', kModCtrl | kModLetter}, {'L', kModCtrl | kModLetter},
             {'S', kModCtrl | kModLetter}, {'Z', kModCtrl | kModLetter}, {'Y', kModCtrl | kModLetter},
             {71, 0}, {84, 0}, {'F', kModCtrl | kModLetter}, {'A', kModCtrl | kModLetter}, {82, 0},
             {261, 0}, {'C', kModCtrl | kModLetter}, {'V', kModCtrl | kModLetter}, {'D', kModCtrl | kModLetter},
             {'P', kModCtrl | kModLetter}, {'N', kModCtrl | kModLetter},
             {66, 0}, {78, 0}}}; // B (the tool shelf; T is the scale key), N
}

struct Config {
    std::vector<std::string> figureLayers;
    std::vector<std::string> textureLayers;
    std::vector<std::string> textLayers;      // texts.res / textslmp.res / folders of loose texts
    std::map<std::string, std::string> textEncodings; // a text layer's path -> "cp1251" / "cp1250" / "cp949" (missing: auto) (#82)
    bool logVerbose = false;                  // the log also to the console (#88)
    int frameRateLimit = 0;                   // Performance: 0 = no cap, else frames per second at most (25 or more)
    bool idleRedraw = true;                   // Performance: 25 frames per second while nothing happens
    bool vsync = true;                        // Performance: wait for the display (off: the cap alone paces)
    bool uiAntialias = true;                  // Performance: anti-aliased lines and shapes in the UI
    bool showFps = false;                     // Performance: the frame rate in the window's corner
    float mapDrawDistance = 0.0f;             // Performance: the Map Editor draws objects within this many units of the camera's target (0: all)
    bool mapLowDetail = false;                // Performance: the Map Editor without shadows, lighting, dressed and posed units
    bool scriptChecks = true;                 // Checks: the maps' scripts checked (off: games with their own commands)
    bool databaseChecks = true;               // Checks: the database's own checks and the maps' names against it
    std::vector<std::string> mapLayers;       // folders of .mpr / .mob files (the Map Editor's list)
    std::string databasePath;
    // Per tab (key e.g. "WEAPONS"): the shown model's orientation, a unit quaternion (w, x, y, z).
    std::map<std::string, std::array<float, 4>> rotations;
    std::map<std::string, std::array<int, 3>> rotationClicks; // per tab: the degrees clicked about X, Y, Z (the labels)
    GifSettings gif;
    std::string mapTerrain;                   // the map editor's .mpr
    std::vector<std::string> mapMobs;         // and its .mob files, in load order
    std::array<KeyBind, kMapKeyCount> mapKeys = DefaultMapKeys();
    bool viewerSidebarRight = false, mapSidebarRight = false; // the list panel on the right of the view
    bool logicSelectedOnly = true;            // logic mode: only the selected unit's logic
    bool logicAlways = false;                 // the selected units' logic shows outside logic mode too
    std::vector<std::string> lightingFiles;   // lights*.ini files or folders holding them
    std::string lightingChoice;               // the one the Map Editor uses
    std::vector<std::string> questFolders;    // folders of .mq files / unpacked quests (or single ones)
    std::vector<std::string> questPacks;      // language packs: folders holding the same quests in other languages
    // Text language packs: (language, path) - a folder of text files or a texts*.res; one language can
    // have several (texts and textslmp). The Texts editor compares them with the reference language.
    std::vector<std::pair<std::string, std::string>> textPacks;
    std::string textReference;
    std::string mapQuest;                     // the quest the Map Editor has open
    float mapCameraSpeed = 1.0f;              // the Map Editor's key movement speed (multiplier)
    int guiTab = 0, viewerTab = 0, mapSideTab = 0; // the tabs open when the GUI was closed
    std::string language;                     // display language, "en" / "ru"; empty = not chosen yet (first start: the GUI asks)
    std::string themePreset = "charcoal";     // viewer/theme.hpp: the colours ("default" = Dear ImGui's own)
    std::string themeAccent = "236,200,130";  // the accent colour, r,g,b (a preset sets it; Settings can change it by hand)
    // Background pictures behind the menus: one for every tab, and one per main tab (which wins)
    std::string background;
    std::string tabBackground[6];                 // File Processing, 3D Viewer, Map Editor, Settings, UM DLL Connector, Texture Editor
    float backgroundOpacity = 0.35f;
    float markerOpacity = 0.5f;               // the Map Editor's light, particle and sound cubes
    float mapHour = -1.0f;                    // the Map Editor's time of day, -1 = the map's own
    int windowW = 1400, windowH = 860;        // the GUI window as it was closed (size when not maximized)
    int windowX = -100000, windowY = -100000; // its position (-100000: let the desktop place it)
    bool windowMaximized = false;
    int mapMouseOrbit = 2, mapMousePan = 1;   // mouse buttons (ImGui numbering: 1 right, 2 middle, 3/4 side buttons)
    bool lightingOn = false;
    int dllPort = 18888;                      // the UM DLL Connector: um.dll's DLL server port (DLL_SERVER_PORT)
    bool dllAutoConnect = false;              // keep trying to connect while not connected
    int dllTab = 0;                           // its sub-tab open last time
    bool sfxEnabled = false;                  // sounds when errors / warnings are detected (alerts.hpp)
    int sfxVolume = 100;                      // their volume, 0-100 %
    bool alertPopups = true;                  // a popup when errors are detected
    bool dbAutoLoad = false;                  // File Processing > DB opens DATABASE by itself
    bool mapRegenNavmesh = false;             // the Map Editor regenerates a zone's AI_GRAPH when saving it
    std::string mpFolder;                     // File Processing > MP: the folder of multiplayer characters (<game>/mp)
    std::map<std::string, std::string> dbCompileTo; // File Processing > DB: database path -> its last "Compile to"
};

inline Config Load(const std::string& path = Path()) {
    Config cfg;
    std::ifstream f(path);
    if (!f.is_open() && path == Path()) f.open(LegacyPath());
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq + 1 >= line.size()) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "FIGURE_LAYER") cfg.figureLayers.push_back(value);
        else if (key == "TEXTURE_LAYER") cfg.textureLayers.push_back(value);
        else if (key == "TEXT_LAYER") cfg.textLayers.push_back(value);
        else if (key == "TEXT_ENCODING") { const size_t bar = value.find('|'); if (bar != std::string::npos) cfg.textEncodings[value.substr(bar + 1)] = value.substr(0, bar); }
        else if (key == "LOG_VERBOSE") cfg.logVerbose = value == "true";
        else if (key == "FRAME_RATE_LIMIT") cfg.frameRateLimit = std::atoi(value.c_str());
        else if (key == "IDLE_REDRAW") cfg.idleRedraw = value == "true";
        else if (key == "VSYNC") cfg.vsync = value == "true";
        else if (key == "UI_ANTIALIAS") cfg.uiAntialias = value == "true";
        else if (key == "SHOW_FPS") cfg.showFps = value == "true";
        else if (key == "MAP_DRAW_DISTANCE") cfg.mapDrawDistance = static_cast<float>(std::atof(value.c_str()));
        else if (key == "MAP_LOW_DETAIL") cfg.mapLowDetail = value == "true";
        else if (key == "SCRIPT_CHECKS") cfg.scriptChecks = value != "false";
        else if (key == "DATABASE_CHECKS") cfg.databaseChecks = value != "false";
        else if (key == "MAP_LAYER") cfg.mapLayers.push_back(value);
        else if (key == "DATABASE") cfg.databasePath = value;
        else if (key == "LANGUAGE") cfg.language = value;
        else if (key == "THEME") cfg.themePreset = value;
        else if (key == "THEME_ACCENT") cfg.themeAccent = value;
        else if (key == "GIF_SIZE") cfg.gif.size = std::atoi(value.c_str());
        else if (key == "GIF_FPS") cfg.gif.fps = std::atoi(value.c_str());
        else if (key == "GIF_SPEED") cfg.gif.degreesPerSecond = std::max(0.0f, static_cast<float>(std::atof(value.c_str())));
        else if (key == "GIF_REVERSE") cfg.gif.reverse = value == "true";
        else if (key == "GIF_AXIS") cfg.gif.axis = value == "X" ? 0 : value == "Y" ? 1 : 2;
        else if (key == "GIF_BACKGROUND") {
            cfg.gif.transparent = value == "none";
            if (!cfg.gif.transparent) cfg.gif.background = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFF;
        }
        else if (key == "GIF_DIRECTORY") cfg.gif.lastDirectory = value;
        else if (key == "MAP_TERRAIN") cfg.mapTerrain = value;
        else if (key == "MAP_MOB") cfg.mapMobs.push_back(value);
        else if (key.rfind("MAP_KEY_", 0) == 0) {
            for (int k = 0; k < kMapKeyCount; ++k) {
                if (key.substr(8) != MapKeyId(k)) continue;
                KeyBind b;
                if (std::sscanf(value.c_str(), "%d,%d", &b.key, &b.mods) >= 1 && b.key > 0) cfg.mapKeys[k] = b;
            }
        }
        else if (key == "VIEWER_SIDEBAR_RIGHT") cfg.viewerSidebarRight = value == "true";
        else if (key == "MAP_SIDEBAR_RIGHT") cfg.mapSidebarRight = value == "true";
        else if (key == "MAP_LOGIC_SELECTED_ONLY") cfg.logicSelectedOnly = value == "true";
        else if (key == "MAP_LOGIC_ALWAYS") cfg.logicAlways = value == "true";
        else if (key == "LIGHTING_LAYER") cfg.lightingFiles.push_back(value);
        else if (key == "MAP_LIGHTING_INI") cfg.lightingChoice = value;
        else if (key == "MAP_LIGHTING") cfg.lightingOn = value == "true";
        else if (key == "QUEST_LAYER") cfg.questFolders.push_back(value);
        else if (key == "QUEST_PACK") cfg.questPacks.push_back(value);
        else if (key == "TEXT_PACK" && value.find('=') != std::string::npos)
            cfg.textPacks.push_back({value.substr(0, value.find('=')), value.substr(value.find('=') + 1)});
        else if (key == "TEXT_REFERENCE") cfg.textReference = value;
        else if (key == "MAP_QUEST") cfg.mapQuest = value;
        else if (key == "GUI_TAB") cfg.guiTab = std::atoi(value.c_str());
        else if (key == "BACKGROUND") cfg.background = value;
        else if (key == "BACKGROUND_FILES") cfg.tabBackground[0] = value;
        else if (key == "BACKGROUND_VIEWER") cfg.tabBackground[1] = value;
        else if (key == "BACKGROUND_MAP") cfg.tabBackground[2] = value;
        else if (key == "BACKGROUND_SETTINGS") cfg.tabBackground[3] = value;
        else if (key == "BACKGROUND_DLL") cfg.tabBackground[4] = value;
        else if (key == "BACKGROUND_TEXTURE") cfg.tabBackground[5] = value;
        else if (key == "DLL_PORT") cfg.dllPort = std::min(std::max(std::atoi(value.c_str()), 1), 65535);
        else if (key == "DLL_AUTO_CONNECT") cfg.dllAutoConnect = value == "true";
        else if (key == "SFX_ENABLED") cfg.sfxEnabled = value == "true";
        else if (key == "SFX_VOLUME") cfg.sfxVolume = std::max(0, std::min(100, std::atoi(value.c_str())));
        else if (key == "ALERT_POPUPS") cfg.alertPopups = value == "true";
        else if (key == "DB_AUTO_LOAD") cfg.dbAutoLoad = value == "true";
        else if (key == "MAP_REGEN_NAVMESH") cfg.mapRegenNavmesh = value == "true";
        else if (key == "MP_FOLDER") cfg.mpFolder = value;
        else if (key == "DB_COMPILE_TO") { // database|res
            const size_t bar = value.find('|');
            if (bar != std::string::npos) cfg.dbCompileTo[value.substr(0, bar)] = value.substr(bar + 1);
        }
        else if (key == "DLL_TAB") cfg.dllTab = std::atoi(value.c_str());
        else if (key == "BACKGROUND_OPACITY") cfg.backgroundOpacity = static_cast<float>(std::atof(value.c_str()));
        else if (key == "WINDOW_SIZE") std::sscanf(value.c_str(), "%d,%d", &cfg.windowW, &cfg.windowH);
        else if (key == "WINDOW_POSITION") std::sscanf(value.c_str(), "%d,%d", &cfg.windowX, &cfg.windowY);
        else if (key == "WINDOW_MAXIMIZED") cfg.windowMaximized = value == "true";
        else if (key == "VIEWER_TAB") cfg.viewerTab = std::atoi(value.c_str());
        else if (key == "MAP_SIDE_TAB") cfg.mapSideTab = std::atoi(value.c_str());
        else if (key == "MAP_MARKER_OPACITY") cfg.markerOpacity = std::min(std::max(static_cast<float>(std::atof(value.c_str())), 0.0f), 1.0f);
        else if (key == "MAP_HOUR") cfg.mapHour = static_cast<float>(std::atof(value.c_str()));
        else if (key == "MAP_MOUSE_ORBIT") cfg.mapMouseOrbit = std::min(std::max(std::atoi(value.c_str()), 1), 4);
        else if (key == "MAP_MOUSE_PAN") cfg.mapMousePan = std::min(std::max(std::atoi(value.c_str()), 1), 4);
        else if (key == "MAP_CAMERA_SPEED") cfg.mapCameraSpeed = std::min(std::max(static_cast<float>(std::atof(value.c_str())), 0.05f), 20.0f);
        else if (key.rfind("ROTCLICKS_", 0) == 0) {
            int v[3] = {0, 0, 0};
            if (std::sscanf(value.c_str(), "%d,%d,%d", &v[0], &v[1], &v[2]) == 3) cfg.rotationClicks[key.substr(10)] = {v[0], v[1], v[2]};
        }
        else if (key.rfind("ROTATION_", 0) == 0) {
            float v[4] = {0, 0, 0, 0};
            int n = std::sscanf(value.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
            if (n == 4) {                  // w,x,y,z
                float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
                if (len > 1e-6f) cfg.rotations[key.substr(9)] = {v[0] / len, v[1] / len, v[2] / len, v[3] / len};
            } else if (n == 3) {           // older files: degrees about X, then Y, then Z
                std::array<float, 4> q = {1, 0, 0, 0};
                for (int axis = 0; axis < 3; ++axis) {
                    float half = v[axis] * 3.14159265f / 360.0f;
                    std::array<float, 4> r = {std::cos(half), axis == 0 ? std::sin(half) : 0.0f,
                                              axis == 1 ? std::sin(half) : 0.0f, axis == 2 ? std::sin(half) : 0.0f};
                    q = {r[0] * q[0] - r[1] * q[1] - r[2] * q[2] - r[3] * q[3], r[0] * q[1] + r[1] * q[0] + r[2] * q[3] - r[3] * q[2],
                         r[0] * q[2] - r[1] * q[3] + r[2] * q[0] + r[3] * q[1], r[0] * q[3] + r[1] * q[2] - r[2] * q[1] + r[3] * q[0]};
                }
                cfg.rotations[key.substr(9)] = q;
            }
        }
    }
    // Select all follows the letter A (it was the US A position).
    if (cfg.mapKeys[kKeySelectAll] == KeyBind{65, kModCtrl}) cfg.mapKeys[kKeySelectAll] = KeyBind{'A', kModCtrl | kModLetter};
    // Ctrl+letter shortcuts follow the letter (Ctrl+Z is Ctrl+Z on AZERTY too, not the key where QWERTY has Z):
    // the old position-based defaults become letter-based.
    for (const auto& [k, key] : {std::pair<int, int>{kKeySwitchMob, 84}, {kKeyResetCamera, 82}, {kKeyLighting, 76}, {kKeySave, 83},
                                 {kKeyUndo, 90}, {kKeyRedo, 89}, {kKeyFind, 70}})
        if (cfg.mapKeys[k] == KeyBind{key, kModCtrl}) cfg.mapKeys[k] = KeyBind{key, kModCtrl | kModLetter};
    // Scale moved from S (which is "back") to T.
    if (cfg.mapKeys[kKeyScale] == KeyBind{83, 0}) cfg.mapKeys[kKeyScale] = KeyBind{84, 0};
    // The tool shelf moved from T (the scale key) to B.
    if (cfg.mapKeys[kKeyToolShelf] == KeyBind{84, 0}) cfg.mapKeys[kKeyToolShelf] = KeyBind{66, 0};
    // The speed's default went from 1.5 to 1: a file still holding the old default follows.
    if (cfg.mapCameraSpeed == 1.5f) cfg.mapCameraSpeed = 1.0f;
    // Up and Down swapped defaults (E up, Q down): older files that kept the old defaults follow.
    if (cfg.mapKeys[kKeyUp] == KeyBind{81, 0} && cfg.mapKeys[kKeyDown] == KeyBind{69, 0}) std::swap(cfg.mapKeys[kKeyUp], cfg.mapKeys[kKeyDown]);
    return cfg;
}

// A key position's US-layout name, for the comments of the saved file (the GUI shows each key by
// what the user's own layout prints on it).
inline std::string KeyPositionName(int key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    switch (key) {
    case 32: return "Space";
    case 256: return "Escape";
    case 257: return "Enter";
    case 258: return "Tab";
    case 340: return "Left Shift";
    case 341: return "Left Ctrl";
    case 342: return "Left Alt";
    case 344: return "Right Shift";
    case 345: return "Right Ctrl";
    case 346: return "Right Alt";
    default: break;
    }
    if (key >= 290 && key <= 314) return "F" + std::to_string(key - 289);
    return "key " + std::to_string(key);
}

// Writes the settings in sections, each setting under a comment saying what it is and what it takes,
// like the mod's um.cfg. The keys are the same as ever, so older files still read.
inline void Save(const Config& cfg, const std::string& path = Path()) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    auto section = [&](const char* title) { f << "\n; -- " << title << " --\n"; };
    auto list = [&](const char* key, const std::vector<std::string>& values) { for (const auto& v : values) f << key << "=" << v << "\n"; };
    auto flag = [](bool b) { return b ? "true" : "false"; };

    f << "; um-multitool Configuration\n"
         "; Written by the GUI (Settings tab and the tabs' own options); edit it here or there.\n"
         "; Lists repeat their key, one line per entry, in order: a later entry wins.\n";

    section("Sources (Settings tab)");
    f << "; Figures: figures.res or folders of .fig/.bon/.lnk/.mod files, base game first, mods after.\n";
    list("FIGURE_LAYER", cfg.figureLayers);
    f << "; Textures: textures.res, redress.res, a mod's textures-zones.res, or folders of .mmp/.dds files.\n";
    list("TEXTURE_LAYER", cfg.textureLayers);
    f << "; Texts: texts.res, textslmp.res or folders of loose text files (item names and descriptions).\n";
    list("TEXT_LAYER", cfg.textLayers);
    f << "; A text source read as one encoding (cp1251 / cp1250 / cp949) instead of guessing: <encoding>|<path>\n";
    for (auto& [path, enc] : cfg.textEncodings) if (!enc.empty() && enc != "auto") f << "TEXT_ENCODING=" << enc << "|" << path << "\n";
    f << "; The log (um-multitool.log beside the program) also printed to the console; (true/false)\n";
    f << "LOG_VERBOSE=" << flag(cfg.logVerbose) << "\n";
    f << "; -- Performance (Settings > Performance) --\n";
    f << "; At most this many frames per second (0: no cap; 25 or more). Lower = less CPU and GPU work.\n";
    f << "FRAME_RATE_LIMIT=" << cfg.frameRateLimit << "\n";
    f << "; 25 frames per second while the mouse and keyboard are quiet (true/false)\n";
    f << "IDLE_REDRAW=" << flag(cfg.idleRedraw) << "\n";
    f << "; Wait for the display's refresh (true/false; off: the cap alone paces the frames)\n";
    f << "VSYNC=" << flag(cfg.vsync) << "\n";
    f << "; Show the frame rate in the window's top-right corner (true/false)\n";
    f << "SHOW_FPS=" << flag(cfg.showFps) << "\n";
    f << "; Anti-aliased lines and shapes in the UI (true/false; off: fewer vertices to draw)\n";
    f << "UI_ANTIALIAS=" << flag(cfg.uiAntialias) << "\n";
    f << "; Map Editor: draw the objects within this many units of the camera's target (0: all of them)\n";
    f << "MAP_DRAW_DISTANCE=" << cfg.mapDrawDistance << "\n";
    f << "; Map Editor: no shadows, lighting, dressed or posed units (true/false)\n";
    f << "MAP_LOW_DETAIL=" << flag(cfg.mapLowDetail) << "\n";
    f << "; -- Checks (Settings > Checks) --\n";
    f << "; The maps' scripts are checked: syntax, commands, arguments, the objects they name (true/false;\n";
    f << "; false for a game with its own commands, see also script_commands.txt beside the program)\n";
    f << "SCRIPT_CHECKS=" << flag(cfg.scriptChecks) << "\n";
    f << "; The database is checked, and the maps' item, spell and prototype names against it (true/false)\n";
    f << "DATABASE_CHECKS=" << flag(cfg.databaseChecks) << "\n";
    f << "; Maps: folders of .mpr and .mob files, listed by the Map Editor.\n";
    list("MAP_LAYER", cfg.mapLayers);
    f << "; Quests: folders of .mq files or unpacked quests, each holding different quests.\n";
    list("QUEST_LAYER", cfg.questFolders);
    f << "; Quest language packs: folders holding the same quests in different languages; changes that\n"
         "; do not depend on the language go to all of them.\n";
    list("QUEST_PACK", cfg.questPacks);
    for (const auto& [language, path] : cfg.textPacks) f << "TEXT_PACK=" << language << "=" << path << "\n";
    if (!cfg.textReference.empty()) f << "TEXT_REFERENCE=" << cfg.textReference << "\n";
    f << "; Lighting: the game's config/lights*.ini files, or folders holding them.\n";
    list("LIGHTING_LAYER", cfg.lightingFiles);
    f << "; Items database: database.res or databaselmp.res (whichever holds items.idb).\n";
    if (!cfg.databasePath.empty()) f << "DATABASE=" << cfg.databasePath << "\n";

    section("Layout");
    f << "; Display language of the GUI: en (English) or ru (Russian). Empty or missing: the GUI asks at start.\n";
    if (!cfg.language.empty()) f << "LANGUAGE=" << cfg.language << "\n";
    f << "; The GUI's colours: a preset of viewer/theme.hpp (default, charcoal, navy, slate, burgundy, bronze, teal) and the accent as r,g,b\n";
    f << "THEME=" << cfg.themePreset << "\nTHEME_ACCENT=" << cfg.themeAccent << "\n";
    f << "; The tabs open when the GUI was closed: main tab (0 File Processing, 1 3D Viewer, 2 Map Editor, 3 Settings),\n"
         "; the 3D Viewer's item tab and the Map Editor's side tab.\n";
    f << "GUI_TAB=" << cfg.guiTab << "\nVIEWER_TAB=" << cfg.viewerTab << "\nMAP_SIDE_TAB=" << cfg.mapSideTab << "\n";
    f << "; The window as it was closed: its size (when not maximized), position (not on Wayland), maximized; (true/false)\n";
    f << "WINDOW_SIZE=" << cfg.windowW << "," << cfg.windowH << "\n";
    if (cfg.windowX != -100000) f << "WINDOW_POSITION=" << cfg.windowX << "," << cfg.windowY << "\n";
    f << "WINDOW_MAXIMIZED=" << flag(cfg.windowMaximized) << "\n";
    f << "; Background pictures (.jpg, .png, .bmp, .tga, .gif, .dds, .mmp...) behind the menus: one for every tab, one per main tab (it wins over the\n"
         "; first), and how much they show (0 to 1).\n";
    if (!cfg.background.empty()) f << "BACKGROUND=" << cfg.background << "\n";
    static const char* const tabKeys[6] = {"BACKGROUND_FILES", "BACKGROUND_VIEWER", "BACKGROUND_MAP", "BACKGROUND_SETTINGS", "BACKGROUND_DLL", "BACKGROUND_TEXTURE"};
    for (int i = 0; i < 6; ++i) if (!cfg.tabBackground[i].empty()) f << tabKeys[i] << "=" << cfg.tabBackground[i] << "\n";
    f << "BACKGROUND_OPACITY=" << cfg.backgroundOpacity << "\n";
    f << "; The 3D Viewer's item list on the right of the view; (true/false)\n";
    f << "VIEWER_SIDEBAR_RIGHT=" << flag(cfg.viewerSidebarRight) << "\n";
    f << "; The Map Editor's panel on the right of the view; (true/false)\n";
    f << "MAP_SIDEBAR_RIGHT=" << flag(cfg.mapSidebarRight) << "\n";

    section("3D Viewer");
    f << "; Each item tab's rotation, a quaternion w,x,y,z (only rotated tabs are written).\n";
    for (auto& kv : cfg.rotations) {
        const auto& q = kv.second;
        if (std::fabs(std::fabs(q[0]) - 1.0f) < 1e-6f) continue; // no rotation
        char line[160];
        std::snprintf(line, sizeof(line), "ROTATION_%s=%.6f,%.6f,%.6f,%.6f\n", kv.first.c_str(), q[0], q[1], q[2], q[3]);
        f << line;
        auto clicks = cfg.rotationClicks.find(kv.first);
        if (clicks != cfg.rotationClicks.end())
            f << "ROTCLICKS_" << kv.first << "=" << clicks->second[0] << "," << clicks->second[1] << "," << clicks->second[2] << "\n";
    }
    char background[16];
    std::snprintf(background, sizeof(background), "%06X", cfg.gif.background & 0xFFFFFF);
    f << "; GIF export: size in pixels (square), frames per second, turning speed in degrees per second.\n";
    f << "GIF_SIZE=" << cfg.gif.size << "\nGIF_FPS=" << cfg.gif.fps << "\nGIF_SPEED=" << cfg.gif.degreesPerSecond << "\n";
    f << "; Turn the other way; (true/false)\n";
    f << "GIF_REVERSE=" << flag(cfg.gif.reverse) << "\n";
    f << "; The axis the model spins about; (X/Y/Z)\n";
    f << "GIF_AXIS=" << "XYZ"[cfg.gif.axis % 3] << "\n";
    f << "; Background colour RRGGBB, or none for transparent.\n";
    f << "GIF_BACKGROUND=" << (cfg.gif.transparent ? "none" : background) << "\n";
    f << "; Where the last GIF was saved.\n";
    if (!cfg.gif.lastDirectory.empty()) f << "GIF_DIRECTORY=" << cfg.gif.lastDirectory << "\n";

    section("Map Editor");
    f << "; The files open at the last session: the terrain, the maps in load order, and the open quest.\n";
    if (!cfg.mapTerrain.empty()) f << "MAP_TERRAIN=" << cfg.mapTerrain << "\n";
    list("MAP_MOB", cfg.mapMobs);
    if (!cfg.mapQuest.empty()) f << "MAP_QUEST=" << cfg.mapQuest << "\n";
    f << "; Logic mode shows only the selected unit's logic (false: every unit of the active map); (true/false)\n";
    f << "MAP_LOGIC_SELECTED_ONLY=" << flag(cfg.logicSelectedOnly) << "\n";
    f << "; The selected units' logic also shows outside logic mode (Layers); (true/false)\n";
    f << "MAP_LOGIC_ALWAYS=" << flag(cfg.logicAlways) << "\n";
    f << "; Lighting on, and the lighting file it uses; (true/false)\n";
    f << "MAP_LIGHTING=" << flag(cfg.lightingOn) << "\n";
    if (!cfg.lightingChoice.empty()) f << "MAP_LIGHTING_INI=" << cfg.lightingChoice << "\n";
    f << "; How opaque the Map Editor's light, particle and sound cubes are (0 to 1)\n";
    f << "MAP_MARKER_OPACITY=" << cfg.markerOpacity << "\n";
    f << "; Time of day for lighting, in hours (-1: the map's own time).\n";
    f << "MAP_HOUR=" << cfg.mapHour << "\n";
    f << "; Speed of the movement keys (0.1-5, 1 = normal).\n";
    f << "MAP_CAMERA_SPEED=" << cfg.mapCameraSpeed << "\n";
    f << "; Mouse buttons that orbit the camera and drag the ground (the left button selects);\n"
         "; 1 right, 2 middle (wheel click), 3 and 4 side buttons.\n";
    f << "MAP_MOUSE_ORBIT=" << cfg.mapMouseOrbit << "\nMAP_MOUSE_PAN=" << cfg.mapMousePan << "\n";

    section("UM DLL Connector");
    f << "; um.dll's DLL server port (DLL_SERVER_PORT in um.cfg), and whether to keep trying to connect; (true/false)\n";
    f << "DLL_PORT=" << cfg.dllPort << "\nDLL_AUTO_CONNECT=" << flag(cfg.dllAutoConnect) << "\nDLL_TAB=" << cfg.dllTab << "\n";
    f << "SFX_ENABLED=" << flag(cfg.sfxEnabled) << "\nSFX_VOLUME=" << cfg.sfxVolume << "\nALERT_POPUPS=" << flag(cfg.alertPopups)
      << "\nDB_AUTO_LOAD=" << flag(cfg.dbAutoLoad) << "\nMAP_REGEN_NAVMESH=" << flag(cfg.mapRegenNavmesh) << "\nMP_FOLDER=" << cfg.mpFolder << "\n";
    for (const auto& [db, res] : cfg.dbCompileTo) f << "DB_COMPILE_TO=" << db << "|" << res << "\n";

    section("Map Editor keys");
    f << "; key,modifiers: the key is a key POSITION (GLFW key code, named after the US layout: the key at the\n"
         "; US W position is Z on AZERTY); modifiers add up: 1 Ctrl, 2 Shift, 4 Alt, 8 the key is a letter (the key\n"
         "; printing it on your layout, e.g. 65,9 = Ctrl+A on any layout). Movement keys ignore modifiers.\n";
    for (int k = 0; k < kMapKeyCount; ++k) {
        const KeyBind& b = cfg.mapKeys[k];
        std::string name = std::string(b.mods & kModCtrl ? "Ctrl+" : "") + (b.mods & kModShift ? "Shift+" : "") +
                           (b.mods & kModAlt ? "Alt+" : "") + KeyPositionName(b.key) + (b.mods & kModLetter ? ", by letter" : "");
        f << "; " << MapKeyLabel(k) << " (" << name << ")\n";
        f << "MAP_KEY_" << MapKeyId(k) << "=" << b.key << "," << b.mods << "\n";
    }
}

} // namespace config
