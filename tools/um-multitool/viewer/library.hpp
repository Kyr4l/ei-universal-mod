// Everything the viewer and the map editor read from disk, without any GL: the layered figure,
// texture and text sources, the items database, and the name indexes built from them. The GUI
// holds one, shared by its tabs and edited in the Settings tab; the command-line modes make their own.
#pragma once

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
    std::string mapQuest;                  // the quest the Map Editor has open
    float mapCameraSpeed = 1.0f;           // the Map Editor's key movement speed (multiplier)
    int mapMouseOrbit = 2, mapMousePan = 1; // the Map Editor's mouse buttons, see config.hpp
    int guiTab = 0, viewerTab = 0, mapSideTab = 0; // the tabs open last time
    std::string background, tabBackground[5];     // background pictures (config.hpp)
    int dllPort = 18888, dllTab = 0;               // the UM DLL Connector (config.hpp)
    bool dllAutoConnect = false;
    float backgroundOpacity = 0.35f;
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
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            dbError = "cannot open " + path;
            dbLoaded = false;
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string label = path.substr(path.find_last_of("/\\") + 1);
        items::Database loaded;
        if (!items::LoadDatabaseRes(bytes, label, loaded, dbError)) {
            dbLoaded = false;
            return false;
        }
        db = std::move(loaded);
        dbLoaded = true;
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
        config::Config cfg = config::Load(configPath);
        for (auto& p : cfg.figureLayers) figures.AddLayer(p);
        for (auto& p : cfg.textureLayers) textures.AddLayer(p);
        for (auto& p : cfg.textLayers) texts.AddLayer(p);
        for (auto& p : cfg.mapLayers) maps.AddLayer(p);
        rotations = cfg.rotations;
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
        mapQuest = cfg.mapQuest;
        mapCameraSpeed = cfg.mapCameraSpeed;
        mapMouseOrbit = cfg.mapMouseOrbit;
        guiTab = cfg.guiTab; viewerTab = cfg.viewerTab; mapSideTab = cfg.mapSideTab; mapHour = cfg.mapHour;
        background = cfg.background;
        for (int i = 0; i < 5; ++i) tabBackground[i] = cfg.tabBackground[i];
        backgroundOpacity = cfg.backgroundOpacity;
        dllPort = cfg.dllPort; dllTab = cfg.dllTab; dllAutoConnect = cfg.dllAutoConnect;
        windowW = cfg.windowW; windowH = cfg.windowH; windowX = cfg.windowX; windowY = cfg.windowY; windowMaximized = cfg.windowMaximized;
        mapMousePan = cfg.mapMousePan;
        RebuildFigureIndex();
        RebuildTextureIndex();
        if (!cfg.databasePath.empty()) LoadDatabase(cfg.databasePath);
    }

    void SaveConfig() const {
        config::Config cfg;
        for (auto& l : figures.layers) cfg.figureLayers.push_back(l.path);
        for (auto& l : textures.layers) cfg.textureLayers.push_back(l.path);
        for (auto& l : texts.layers) cfg.textLayers.push_back(l.path);
        for (auto& l : maps.layers) cfg.mapLayers.push_back(l.path);
        cfg.databasePath = dbPath;
        cfg.rotations = rotations;
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
        cfg.mapQuest = mapQuest;
        cfg.mapCameraSpeed = mapCameraSpeed;
        cfg.mapMouseOrbit = mapMouseOrbit;
        cfg.guiTab = guiTab; cfg.viewerTab = viewerTab; cfg.mapSideTab = mapSideTab; cfg.mapHour = mapHour;
        cfg.background = background;
        for (int i = 0; i < 5; ++i) cfg.tabBackground[i] = tabBackground[i];
        cfg.backgroundOpacity = backgroundOpacity;
        cfg.dllPort = dllPort; cfg.dllTab = dllTab; cfg.dllAutoConnect = dllAutoConnect;
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
