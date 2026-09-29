// The Sources tab: where figures, textures and the items database come from.
#pragma once

#include <string>

#include "library.hpp"
#include "scene.hpp"
#include "ui_common.hpp"

namespace ui {

struct SourcesState {
    char figurePath[1024] = "";
    char texturePath[1024] = "";
    char databasePath[1024] = "";
    std::string message;
};

// One layered source list: highest priority on top, with Up/Down/Remove, and an add row.
// Returns true when the list changed.
inline bool LayerList(const char* id, LayeredAssetSource& source, char* path, size_t pathSize, std::string& message,
                      const char* hint) {
    bool changed = false;
    ImGui::PushID(id);
    for (size_t i = source.layers.size(); i-- > 0;) {
        auto& layer = source.layers[i];
        ImGui::PushID(static_cast<int>(i));
        float rowWidth = ImGui::GetContentRegionAvail().x;
        const float buttons = 118.0f;
        StatusDot(layer.ok, layer.source.isArchive ? "RES archive" : "folder", layer.error);
        ImGui::SameLine();
        ImGui::BeginChild("##p", ImVec2(std::max(rowWidth - buttons - ImGui::GetCursorPosX(), 20.0f), ImGui::GetTextLineHeight()),
                          false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(layer.path.c_str());
        ImGui::EndChild();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer.path.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowWidth - buttons);
        ImGui::BeginDisabled(i + 1 == source.layers.size());
        if (ImGui::SmallButton("Up")) { source.MoveLayerUp(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Down")) { source.MoveLayerDown(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) { source.RemoveLayer(i); changed = true; }
        ImGui::PopID();
        if (changed) break;
    }
    if (source.layers.empty()) ImGui::TextDisabled("%s", hint);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##add", "path to a .res archive or a folder", path, pathSize);
    std::string picked;
    if (ImGui::Button("File...") && PickFile(picked)) std::snprintf(path, pathSize, "%s", picked.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Folder...") && PickFolder(picked)) std::snprintf(path, pathSize, "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(path[0] == '\0');
    if (ImGui::Button("Add")) {
        bool ok = source.AddLayer(path);
        message = ok ? std::string("Added ") + path : "Could not add " + std::string(path) + ": " + source.layers.back().error;
        path[0] = '\0';
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

inline void SourcesTab(Library& lib, Scene& scene, SourcesState& st) {
    ImGui::TextWrapped("Sources are searched top to bottom: add the base game first, then mods on top (a mod's file "
                       "overrides the game's). Each source is a .res archive or a folder of loose files.");
    ImGui::Spacing();

    ImGui::SeparatorText("Figures");
    ImGui::TextDisabled("figures.res, or a folder of .fig/.bon/.lnk/.mod files");
    if (LayerList("fig", lib.figures, st.figurePath, sizeof(st.figurePath), st.message, "(none yet)")) {
        lib.RebuildFigureIndex();
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Textures");
    ImGui::TextDisabled("textures.res and redress.res, or a folder of .mmp or .dds files");
    if (LayerList("tex", lib.textures, st.texturePath, sizeof(st.texturePath), st.message, "(none yet)")) {
        lib.RebuildTextureIndex();
        scene.ClearTextures();
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Database");
    ImGui::TextDisabled("database.res or databaselmp.res (whichever holds items.idb)");
    StatusDot(lib.dbLoaded, lib.dbLoaded ? lib.dbPath : "", lib.dbError);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##db", "path to database.res / databaselmp.res", st.databasePath, sizeof(st.databasePath));
    std::string picked;
    if (ImGui::Button("File...##db") && PickFile(picked)) std::snprintf(st.databasePath, sizeof(st.databasePath), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(st.databasePath[0] == '\0');
    if (ImGui::Button("Load##db")) {
        if (lib.LoadDatabase(st.databasePath)) {
            st.message = "Loaded " + lib.db.sourceLabel + ": " + std::to_string(lib.db.materials.size()) + " materials, " +
                         std::to_string(lib.db.List(items::Category::Weapons).size()) + " weapons, " +
                         std::to_string(lib.db.List(items::Category::Armors).size()) + " armors, " +
                         std::to_string(lib.db.List(items::Category::QuickItems).size()) + " quick items, " +
                         std::to_string(lib.db.List(items::Category::QuestItems).size()) + " quest items, " +
                         std::to_string(lib.db.List(items::Category::LootItems).size()) + " loot items";
            lib.SaveConfig();
        } else {
            st.message = "Database not loaded: " + lib.dbError;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(lib.dbPath.empty());
    if (ImGui::Button("Reload")) {
        st.message = lib.LoadDatabase(lib.dbPath) ? "Reloaded " + lib.db.sourceLabel : "Database not loaded: " + lib.dbError;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the same file again, after editing it elsewhere");
    if (!lib.dbLoaded && !lib.dbError.empty()) Note(lib.dbError);

    ImGui::Spacing();
    ImGui::SeparatorText("Found");
    ImGui::Text("%zu figures, %zu textures", lib.figureIndex.baseNames.size(), lib.textureIndex.names.size());
    if (lib.dbLoaded) {
        ImGui::Text("%s: %zu materials", lib.db.sourceLabel.c_str(), lib.db.materials.size());
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c) {
            ImGui::BulletText("%s: %zu", items::CategoryLabel(static_cast<items::Category>(c)),
                              lib.db.List(static_cast<items::Category>(c)).size());
        }
    }
    if (!st.message.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", st.message.c_str());
    }
}

} // namespace ui
