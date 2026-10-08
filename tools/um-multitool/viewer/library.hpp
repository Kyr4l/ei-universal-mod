// Everything the viewer and the map editor read from disk, without any GL: the layered figure,
// texture and text sources, the items database, and the name indexes built from them. The GUI
// holds one, shared by its tabs and edited in the Settings tab; the command-line modes make their own.
#pragma once

#include <chrono>

#include "../log.hpp"

#include "db_model.hpp"

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "asset_source.hpp"
#include "config.hpp"
#include "item_db.hpp"
#include "unit_db.hpp"
#include "item_resolve.hpp"

struct Library {
    LayeredAssetSource figures;
    LayeredAssetSource textures;
    LayeredAssetSource texts;     // item names and descriptions (item_texts.hpp)
    LayeredAssetSource maps;      // folders of .mpr / .mob files, listed by the Map Editor
    resolve::FigureIndex figureIndex;
    resolve::TextureIndex textureIndex;

    items::Database db;
    units::Database unitsDb; // units.udb of the same archive (empty when it has none): dresses map units
    bool dbLoaded = false;
    std::string dbPath;
    std::string dbError;

    std::string configPath = config::Path();
    std::map<std::string, std::array<float, 4>> rotations; // per tab, see config::Config::rotations
    std::map<std::string, std::array<int, 3>> rotationClicks; // per tab: the degrees clicked about X, Y, Z
    config::GifSettings gif;
    std::string mapTerrain;              // the map editor's files, see config::Config
    std::vector<std::string> mapMobs;
    std::array<config::KeyBind, config::kMapKeyCount> mapKeys = config::DefaultMapKeys(); // key positions, see config.hpp
    bool viewerSidebarRight = false, mapSidebarRight = false;
    bool logicSelectedOnly = true;
    bool logicAlways = false;
    std::vector<std::string> lightingFiles; // lights*.ini files, or folders holding them
    std::string lightingChoice;
    bool lightingOn = false;
    std::vector<std::string> questFolders; // .mq files / unpacked quests, or folders of them
    std::vector<std::string> questPacks;   // language packs: the same quests in other languages
    std::vector<std::pair<std::string, std::string>> textPacks; // text language packs: (language, path), config.hpp
    std::string textReference;                                  // the Texts editor's reference language
    int textPacksVersion = 0;                                   // bumped when textPacks change
    std::string mapQuest;                  // the quest the Map Editor has open
    float mapCameraSpeed = 1.0f;           // the Map Editor's key movement speed (multiplier)
    int mapMouseOrbit = 2, mapMousePan = 1; // the Map Editor's mouse buttons, see config.hpp
    int guiTab = 0, viewerTab = 0, mapSideTab = 0; // the tabs open last time
    std::string language;                          // display language "en" / "ru" (i18n.hpp); empty = not chosen yet
    std::string background, tabBackground[6];     // background pictures (config.hpp)
    int dllPort = 18888, dllTab = 0;               // the UM DLL Connector (config.hpp)
    bool dllAutoConnect = false;
    bool sfxEnabled = false, alertPopups = true;   // problem alerts (config.hpp, alerts.hpp)
    int sfxVolume = 100;                           // 0-100 %
    bool dbAutoLoad = false;                       // File Processing > DB opens dbPath by itself
    bool mapRegenNavmesh = false;                  // Map Editor: regenerate the navmesh (AI_GRAPH) on save
    std::string mpFolder;                          // File Processing > MP: the multiplayer characters' folder
    std::map<std::string, std::string> dbCompileTo; // File Processing > DB: database -> its last "Compile to"
    float backgroundOpacity = 0.35f;
    std::map<std::string, std::string> textEncodings; // text layer path -> cp1251 / cp1250 / cp949 (#82)
    bool logVerbose = false, logWindow = false;      // #88
    float markerOpacity = 0.5f;                    // the Map Editor's light, particle and sound cubes
    float mapHour = -1.0f;                         // the Map Editor's time of day (-1: the map's own)
    int windowW = 1400, windowH = 860, windowX = -100000, windowY = -100000; // the GUI window, see config.hpp
    bool windowMaximized = false;
    int version = 0; // bumped whenever a source or the database changes, so derived data is recomputed
    int figuresVersion = 0, texturesVersion = 0; // bumped when that source changes, so GL caches are dropped
    int mapsVersion = 0;                         // bumped when the map folders change

    void RebuildFigureIndex() { figureIndex.Build(figures); ++version; ++figuresVersion; }
    void RebuildTextureIndex() { textureIndex.Build(textures); ++version; ++texturesVersion; }

    bool LoadDatabase(const std::string& path) {
        dbPath = path;
        dbError.clear();
        std::vector<uint8_t> bytes; // a .res, or a spreadsheet compiled in memory
        if (!dbmodel::ReadAsRes(path, bytes, dbError)) {
            dbLoaded = false;
            umlog::Write(umlog::Level::Error, "Database " + path + ": " + dbError);
            return false;
        }
        std::string label = path.substr(path.find_last_of("/\\") + 1);
        items::Database loaded;
        if (!items::LoadDatabaseRes(bytes, label, loaded, dbError)) {
            dbLoaded = false;
            return false;
        }
        db = std::move(loaded);
        dbLoaded = true;
        umlog::Write(umlog::Level::Info, "Database " + path + ": " + std::to_string(db.List(items::Category::Weapons).size()) + " weapons, " + std::to_string(db.List(items::Category::Armors).size()) + " armors");
        unitsDb = units::Database{};
        {
            res::Archive archive;
            std::string err;
            if (res::ParseArchive(bytes, archive, err))
                if (const std::vector<uint8_t>* udb = archive.Find("units.udb")) units::ParseUnitsUdb(*udb, unitsDb);
        }
        ++version;
        return true;
    }

    void LoadConfig() {
        const auto t0 = std::chrono::steady_clock::now();
        config::Config cfg = config::Load(configPath);
        umlog::Write(umlog::Level::Info, "Config " + configPath + ": " + std::to_string(cfg.figureLayers.size()) + " figure, " + std::to_string(cfg.textureLayers.size()) +
                                         " texture, " + std::to_string(cfg.textLayers.size()) + " text, " + std::to_string(cfg.mapLayers.size()) + " map source(s)");
        auto add = [&](const char* kind, LayeredAssetSource& src, const std::string& p) {
            const auto t = std::chrono::steady_clock::now();
            const bool ok = src.AddLayer(p);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            umlog::Write(ok ? umlog::Level::Info : umlog::Level::Warning, std::string(kind) + " source " + p + (ok ? ": " + std::to_string(src.layers.back().source.isArchive ? src.layers.back().source.archive.entries.size() : src.layers.back().source.lowerToReal.size()) + " file(s), " + std::to_string(static_cast<int>(ms)) + " ms" : ": NOT loaded: " + src.layers.back().error));
        };
        for (auto& p : cfg.figureLayers) add("Figure", figures, p);
        for (auto& p : cfg.textureLayers) add("Texture", textures, p);
        for (auto& p : cfg.textLayers) add("Text", texts, p);
        for (auto& p : cfg.mapLayers) add("Map", maps, p);
        rotations = cfg.rotations;
        rotationClicks = cfg.rotationClicks;
        gif = cfg.gif;
        mapTerrain = cfg.mapTerrain;
        mapMobs = cfg.mapMobs;
        mapKeys = cfg.mapKeys;
        viewerSidebarRight = cfg.viewerSidebarRight;
        mapSidebarRight = cfg.mapSidebarRight;
        logicSelectedOnly = cfg.logicSelectedOnly;
        logicAlways = cfg.logicAlways;
        lightingFiles = cfg.lightingFiles;
        lightingChoice = cfg.lightingChoice;
        lightingOn = cfg.lightingOn;
        questFolders = cfg.questFolders;
        questPacks = cfg.questPacks;
        textPacks = cfg.textPacks;
        textReference = cfg.textReference;
        mapQuest = cfg.mapQuest;
        mapCameraSpeed = cfg.mapCameraSpeed;
        mapMouseOrbit = cfg.mapMouseOrbit;
        language = cfg.language;
        guiTab = cfg.guiTab; viewerTab = cfg.viewerTab; mapSideTab = cfg.mapSideTab; mapHour = cfg.mapHour; markerOpacity = cfg.markerOpacity; textEncodings = cfg.textEncodings; logVerbose = cfg.logVerbose;
        background = cfg.background;
        for (int i = 0; i < 6; ++i) tabBackground[i] = cfg.tabBackground[i];
        backgroundOpacity = cfg.backgroundOpacity;
        dllPort = cfg.dllPort; dllTab = cfg.dllTab; dllAutoConnect = cfg.dllAutoConnect;
        sfxEnabled = cfg.sfxEnabled; sfxVolume = cfg.sfxVolume; alertPopups = cfg.alertPopups; dbAutoLoad = cfg.dbAutoLoad; mapRegenNavmesh = cfg.mapRegenNavmesh; mpFolder = cfg.mpFolder; dbCompileTo = cfg.dbCompileTo;
        windowW = cfg.windowW; windowH = cfg.windowH; windowX = cfg.windowX; windowY = cfg.windowY; windowMaximized = cfg.windowMaximized;
        mapMousePan = cfg.mapMousePan;
        RebuildFigureIndex();
        RebuildTextureIndex();
        if (!cfg.databasePath.empty()) LoadDatabase(cfg.databasePath);
        umlog::Write(umlog::Level::Info, "Sources ready in " + std::to_string(static_cast<int>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count())) + " ms");
    }

    void SaveConfig() const {
        config::Config cfg;
        for (auto& l : figures.layers) cfg.figureLayers.push_back(l.path);
        for (auto& l : textures.layers) cfg.textureLayers.push_back(l.path);
        for (auto& l : texts.layers) cfg.textLayers.push_back(l.path);
        for (auto& l : maps.layers) cfg.mapLayers.push_back(l.path);
        cfg.databasePath = dbPath;
        cfg.rotations = rotations;
        cfg.rotationClicks = rotationClicks;
        cfg.gif = gif;
        cfg.mapTerrain = mapTerrain;
        cfg.mapMobs = mapMobs;
        cfg.mapKeys = mapKeys;
        cfg.viewerSidebarRight = viewerSidebarRight;
        cfg.mapSidebarRight = mapSidebarRight;
        cfg.logicSelectedOnly = logicSelectedOnly;
        cfg.logicAlways = logicAlways;
        cfg.lightingFiles = lightingFiles;
        cfg.lightingChoice = lightingChoice;
        cfg.lightingOn = lightingOn;
        cfg.questFolders = questFolders;
        cfg.questPacks = questPacks;
        cfg.textPacks = textPacks;
        cfg.textReference = textReference;
        cfg.mapQuest = mapQuest;
        cfg.mapCameraSpeed = mapCameraSpeed;
        cfg.mapMouseOrbit = mapMouseOrbit;
        cfg.language = language;
        cfg.guiTab = guiTab; cfg.viewerTab = viewerTab; cfg.mapSideTab = mapSideTab; cfg.mapHour = mapHour; cfg.markerOpacity = markerOpacity; cfg.textEncodings = textEncodings; cfg.logVerbose = logVerbose;
        cfg.background = background;
        for (int i = 0; i < 6; ++i) cfg.tabBackground[i] = tabBackground[i];
        cfg.backgroundOpacity = backgroundOpacity;
        cfg.dllPort = dllPort; cfg.dllTab = dllTab; cfg.dllAutoConnect = dllAutoConnect;
        cfg.sfxEnabled = sfxEnabled; cfg.sfxVolume = sfxVolume; cfg.alertPopups = alertPopups; cfg.dbAutoLoad = dbAutoLoad; cfg.mapRegenNavmesh = mapRegenNavmesh; cfg.mpFolder = mpFolder; cfg.dbCompileTo = dbCompileTo;
        cfg.windowW = windowW; cfg.windowH = windowH; cfg.windowX = windowX; cfg.windowY = windowY; cfg.windowMaximized = windowMaximized;
        cfg.mapMousePan = mapMousePan;
        config::Save(cfg, configPath);
    }

    // The .mpr and .mob files of the map folders; a file in a higher folder hides the same name below.
    struct MapFile { std::string name, path, folder; bool terrain = false; };
    std::vector<MapFile> ListMapFiles() const {
        std::map<std::string, MapFile> byName;
        for (const auto& layer : maps.layers) { // bottom to top: later ones replace
            if (!layer.ok || layer.source.isArchive) continue;
            for (const auto& kv : layer.source.lowerToReal) {
                const std::string& lower = kv.first;
                bool terrain = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mpr") == 0;
                bool mob = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mob") == 0;
                if (!terrain && !mob) continue;
                byName[lower] = {kv.second, (layer.source.root / kv.second).string(), layer.path, terrain};
            }
        }
        std::vector<MapFile> out;
        for (auto& kv : byName) out.push_back(kv.second);
        return out;
    }

    bool Ready() const { return dbLoaded && figures.AnyLoaded() && textures.AnyLoaded(); }
};
