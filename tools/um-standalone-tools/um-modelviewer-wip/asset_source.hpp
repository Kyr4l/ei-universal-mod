// Unifies "a loose directory of files" and "a RES archive" behind one
// interface, so the viewer can load figures/textures from either a packed
// .res or an unpacked res-unpacked-style folder without caring which.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "res_archive.hpp"

namespace fs = std::filesystem;

struct AssetSource {
    bool valid = false;
    bool isArchive = false;
    fs::path root; // directory root, when !isArchive
    res::Archive archive; // populated when isArchive
    std::string label; // for UI display

    // Directory sources need this to match a packed .res archive's own
    // case-insensitive lookup (res::Archive keys everything by ToLower): real
    // mod/vanilla asset directories are frequently inconsistent about casing
    // (Windows filesystems don't care), so a straight fs::is_regular_file
    // check with an assumed-lowercase name silently fails to find files that
    // genuinely exist on a case-sensitive filesystem like Linux's. Built once
    // in Load(), keyed by ToLower(filename) -> the real on-disk filename.
    std::map<std::string, std::string> lowerToReal;

    bool Load(const std::string& path, std::string& err) {
        valid = false;
        isArchive = false;
        archive = res::Archive{};
        lowerToReal.clear();
        fs::path p(path);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            root = p;
            isArchive = false;
            valid = true;
            label = p.filename().string();
            for (auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                lowerToReal[res::Archive::ToLower(name)] = name;
            }
            return true;
        }
        if (fs::is_regular_file(p, ec)) {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) { err = "cannot open file: " + path; return false; }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!res::ParseArchive(bytes, archive, err)) return false;
            isArchive = true;
            valid = true;
            label = p.filename().string();
            return true;
        }
        err = "path not found: " + path;
        return false;
    }

    bool Contains(const std::string& name) const {
        if (!valid) return false;
        if (isArchive) return archive.Contains(name);
        return lowerToReal.find(res::Archive::ToLower(name)) != lowerToReal.end();
    }

    bool ReadFile(const std::string& name, std::vector<uint8_t>& out) const {
        if (!valid) return false;
        if (isArchive) {
            const std::vector<uint8_t>* data = archive.Find(name);
            if (!data) return false;
            out = *data;
            return true;
        }
        auto it = lowerToReal.find(res::Archive::ToLower(name));
        if (it == lowerToReal.end()) return false;
        std::ifstream f(root / it->second, std::ios::binary);
        if (!f.is_open()) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    // Lists every file matching one of the given extensions (lowercase, with dot,
    // e.g. ".mmp"), stripped of its extension - used to populate browse lists.
    std::vector<std::string> ListBaseNames(const std::vector<std::string>& extensions) const {
        std::vector<std::string> out;
        auto matches = [&](std::string lowerName) -> bool {
            for (auto& ext : extensions) {
                if (lowerName.size() >= ext.size() &&
                    lowerName.compare(lowerName.size() - ext.size(), ext.size(), ext) == 0) return true;
            }
            return false;
        };
        if (!valid) return out;
        if (isArchive) {
            for (auto& kv : archive.entries) {
                if (matches(kv.first)) {
                    std::string base = kv.second.originalName;
                    auto dot = base.find_last_of('.');
                    if (dot != std::string::npos) base = base.substr(0, dot);
                    out.push_back(base);
                }
            }
        } else {
            std::error_code ec;
            for (auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (matches(lower)) out.push_back(entry.path().stem().string());
            }
        }
        return out;
    }
};

// Multiple sources searched in reverse order (last-added wins), so a base-game
// archive can be loaded first and mod archives layered on top of it, matching
// how the real game's mod chain resolves overrides.
struct LayeredAssetSource {
    struct Layer {
        AssetSource source;
        std::string path;
        bool ok = false;
        std::string error;
    };
    std::vector<Layer> layers;

    bool AddLayer(const std::string& path) {
        Layer layer;
        layer.path = path;
        layer.ok = layer.source.Load(path, layer.error);
        layers.push_back(std::move(layer));
        return layers.back().ok;
    }

    void RemoveLayer(size_t index) {
        if (index < layers.size()) layers.erase(layers.begin() + static_cast<long>(index));
    }

    // "Up" moves a layer toward the END of this vector, since layers.back() is
    // the highest-priority layer and is what the UI displays at the top of the
    // list (see Contains/ReadFile below, which search back-to-front).
    void MoveLayerUp(size_t index) {
        if (index + 1 < layers.size()) std::swap(layers[index], layers[index + 1]);
    }

    void MoveLayerDown(size_t index) {
        if (index > 0 && index < layers.size()) std::swap(layers[index - 1], layers[index]);
    }

    bool Contains(const std::string& name) const {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            if (it->ok && it->source.Contains(name)) return true;
        }
        return false;
    }

    bool ReadFile(const std::string& name, std::vector<uint8_t>& out) const {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            if (it->ok && it->source.ReadFile(name, out)) return true;
        }
        return false;
    }

    // Merges names across all layers (a mod layer's file list still shows up even
    // if a base-game layer doesn't have that particular file).
    std::vector<std::string> ListBaseNames(const std::vector<std::string>& extensions) const {
        std::vector<std::string> merged;
        for (auto& layer : layers) {
            if (!layer.ok) continue;
            auto names = layer.source.ListBaseNames(extensions);
            merged.insert(merged.end(), names.begin(), names.end());
        }
        std::sort(merged.begin(), merged.end());
        merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
        return merged;
    }

    bool AnyLoaded() const {
        for (auto& l : layers) if (l.ok) return true;
        return false;
    }
};
