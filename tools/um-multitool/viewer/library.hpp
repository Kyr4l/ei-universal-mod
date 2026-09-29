// Everything the viewer reads from disk, without any GL: the layered figure and
// texture sources, the items database, and the name indexes built from them.
// Shared by the GUI and the command-line modes.
#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "asset_source.hpp"
#include "config.hpp"
#include "item_db.hpp"
#include "item_resolve.hpp"

struct Library {
    LayeredAssetSource figures;
    LayeredAssetSource textures;
    LayeredAssetSource texts;     // item names and descriptions (item_texts.hpp)
    resolve::FigureIndex figureIndex;
    resolve::TextureIndex textureIndex;

    items::Database db;
    bool dbLoaded = false;
    std::string dbPath;
    std::string dbError;

    std::string configPath = config::Path();
    std::map<std::string, std::array<float, 4>> rotations; // per tab, see config::Config::rotations
    config::GifSettings gif;
    int version = 0; // bumped whenever a source or the database changes, so derived data is recomputed

    void RebuildFigureIndex() { figureIndex.Build(figures); ++version; }
    void RebuildTextureIndex() { textureIndex.Build(textures); ++version; }

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
        ++version;
        return true;
    }

    void LoadConfig() {
        config::Config cfg = config::Load(configPath);
        for (auto& p : cfg.figureLayers) figures.AddLayer(p);
        for (auto& p : cfg.textureLayers) textures.AddLayer(p);
        for (auto& p : cfg.textLayers) texts.AddLayer(p);
        rotations = cfg.rotations;
        gif = cfg.gif;
        RebuildFigureIndex();
        RebuildTextureIndex();
        if (!cfg.databasePath.empty()) LoadDatabase(cfg.databasePath);
    }

    void SaveConfig() const {
        config::Config cfg;
        for (auto& l : figures.layers) cfg.figureLayers.push_back(l.path);
        for (auto& l : textures.layers) cfg.textureLayers.push_back(l.path);
        for (auto& l : texts.layers) cfg.textLayers.push_back(l.path);
        cfg.databasePath = dbPath;
        cfg.rotations = rotations;
        cfg.gif = gif;
        config::Save(cfg, configPath);
    }

    bool Ready() const { return dbLoaded && figures.AnyLoaded() && textures.AnyLoaded(); }
};
