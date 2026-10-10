// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The item tabs (Weapons, Armors, Quick Items, Quest Items, Loot Items): one generic
// tab - a filterable list of the category's database rows, and for the selected row
// its figure, material, texture and stats - plus per-category details.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <filesystem>
#include <string>
#include <vector>

#include "item_resolve.hpp"
#include "item_texts.hpp"
#include "png_writer.hpp"
#include "library.hpp"
#include "scene.hpp"
#include "uv_map.hpp"
#include "png_writer.hpp"
#include "ui_common.hpp"

namespace ui {

struct ItemTabState {
    char filter[96] = "";
    std::string typeFilter;          // "" = all types
    int selected = -1;               // row in the category's list, -1 = none
    std::string material;            // picked material name, "" = default
    std::string textureOverride;     // picked texture, "" = the resolved default
    std::string figureOverride;      // picked figure, "" = the resolved one
    bool allTextures = false;        // the texture picker lists every texture, not only the candidates
    char textureFilter[64] = "";
    bool scrollToSelected = false;

    std::string pngMessage;          // result of the last "Export PNG" / "Export UV"
    bool uvWithTexture = true;       // "Export UV": over the texture, or on its own
    bool pngOk = true;

    // Resolution of the selected row, recomputed only when its inputs change.
    resolve::Resolution resolution;
    std::string resolutionKey;
};

inline std::string LowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Which rows pass the filter box and the type combo, in database order.
inline std::vector<int> VisibleRows(const std::vector<items::Item>& list, const ItemTabState& st) {
    std::vector<int> rows;
    std::string needle = LowerCopy(st.filter);
    for (size_t i = 0; i < list.size(); ++i) {
        const items::Item& it = list[i];
        if (!st.typeFilter.empty() && it.type != st.typeFilter) continue;
        if (!needle.empty() && LowerCopy(it.name).find(needle) == std::string::npos &&
            LowerCopy(it.type).find(needle) == std::string::npos) continue;
        rows.push_back(static_cast<int>(i));
    }
    return rows;
}

inline void RefreshResolution(const Library& lib, const items::Item& item, ItemTabState& st, int libraryVersion) {
    std::string key = std::to_string(libraryVersion) + "|" + std::to_string(st.selected) + "|" + st.material;
    if (key == st.resolutionKey) return;
    st.resolutionKey = key;
    st.resolution = resolve::Resolve(lib.db, item, lib.figureIndex, lib.textureIndex,
                                     st.material.empty() ? nullptr : lib.db.FindMaterial(st.material));
}

// The figure and texture this tab wants on screen.
inline std::string ShownFigure(const Library& lib, const ItemTabState& st) {
    if (!st.figureOverride.empty()) return lib.figureIndex.Loadable(st.figureOverride);
    return st.resolution.loadable;
}
inline std::string ShownTexture(const ItemTabState& st) {
    if (!st.textureOverride.empty()) return st.textureOverride;
    const auto& r = st.resolution;
    return r.texture >= 0 ? r.textures[r.texture].name : "";
}

// Puts this tab's selection in the viewport; the camera is re-framed only when the figure changes.
inline void ShowInScene(const Library& lib, Scene& scene, const ItemTabState& st) {
    std::string figure = st.selected >= 0 ? ShownFigure(lib, st) : "";
    if (figure != scene.modelName) scene.LoadModel(lib, figure, true);
    scene.textureName = figure.empty() ? "" : ShownTexture(st);
}

inline void SelectRow(ItemTabState& st, int row) {
    if (row == st.selected) return;
    st.selected = row;
    st.material.clear();
    st.textureOverride.clear();
    st.figureOverride.clear();
    st.scrollToSelected = true;
}

inline void WeaponDetails(const items::Item& it, const items::Material* m) {
    Row("Range", ui::Num(it.range));
    Row("Damage (blueprint)", "min " + ui::Num(it.dMin) + ", max +" + ui::Num(it.dMax));
    if (m) {
        // MinDamage = material damage * dMin, MaxDamage = material damage * (dMin + dMax):
        // checked against tools/dmg-calculator.ods (e.g. stone axe + granite -> 9.54 .. 16.74).
        Row(("Damage in " + m->name).c_str(), ui::Num(m->damage * it.dMin) + " .. " + ui::Num(m->damage * (it.dMin + it.dMax)));
    }
    Row("Attack / defence", ui::Num(it.attack) + " / " + ui::Num(it.defence));
    static const char* const kinds[] = {"piercing", "slashing", "bludgeoning", "thermal", "chemical", "electric", "general"};
    std::string split;
    for (size_t i = 0; i < it.damageProportions.size() && i < 7; ++i) {
        if (it.damageProportions[i] <= 0.0f) continue;
        if (!split.empty()) split += ", ";
        split += ui::Num(it.damageProportions[i] * 100.0f, 0) + "% " + kinds[i];
    }
    Row("Damage type", split.empty() ? "-" : split);
}

inline void ArmorDetails(const items::Item& it) {
    Row("Absorption", it.mainAbsorb.empty() ? "-" : ui::Num(it.mainAbsorb[0]));
}

inline void QuickDetails(const items::Item& it) {
    Row("Item ID / level", std::to_string(it.quickItemId) + " / " + std::to_string(it.graphicsLevel));
    if (it.quickDamage != 0.0f) Row("Damage", ui::Num(it.quickDamage));
    if (!it.spell.empty()) Row("Spell", it.spell);
}

inline void QuestDetails(const items::Item& it) {
    Row("Script ID", std::to_string(it.scriptId));
    std::string zones;
    for (auto& z : it.zones) zones += (zones.empty() ? "" : ", ") + z;
    Row("Zones", zones.empty() ? "-" : zones);
}

// The figure picker: automatic (from the database) or any figure of the item's family.
inline void FigurePicker(const Library& lib, ItemTabState& st) {
    const auto& r = st.resolution;
    std::string automatic = r.figure.empty() ? "Automatic (none)" : "Automatic (" + r.figure + ")";
    std::string current = st.figureOverride.empty() ? automatic : st.figureOverride;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##figure", current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(automatic.c_str(), st.figureOverride.empty())) st.figureOverride.clear();
        for (const std::string& f : lib.figureIndex.Family(r.familyPrefix.empty() ? "init" : r.familyPrefix)) {
            if (ImGui::Selectable(f.c_str(), st.figureOverride == f)) st.figureOverride = f;
        }
        ImGui::EndCombo();
    }
}

inline void MaterialPicker(const Library& lib, ItemTabState& st) {
    const auto& r = st.resolution;
    if (r.materials.empty()) return;
    const items::Material* current = r.material >= 0 ? r.materials[r.material] : nullptr;
    ImGui::TextDisabled("Material");
    ImGui::SameLine(90);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##material", current ? current->name.c_str() : "-", ImGuiComboFlags_HeightLarge)) {
        for (const items::Material* m : r.materials) {
            bool hasTexture = std::any_of(r.textures.begin(), r.textures.end(),
                                          [&](const resolve::TextureOption& o) { return o.material == m; });
            // Material loot gets its textures per material only after resolving, so it never shows "(no texture)".
            bool lootMaterial = r.figure.rfind("initlimt", 0) == 0;
            std::string label = m->name + " (" + m->code + ")";
            if (!hasTexture && !lootMaterial) label += "  - no texture";
            if (ImGui::Selectable(label.c_str(), m == current)) {
                st.material = m->name;
                st.textureOverride.clear();
            }
        }
        ImGui::EndCombo();
    }
    (void)lib;
}

// Decodes the texture again (the GL copy cannot be read back portably) and saves it where the user picks.
inline void ExportTexturePng(Library& lib, const std::string& texture, ItemTabState& st) {
    std::string dir = lib.gif.lastDirectory.empty() ? config::ExeDir() : lib.gif.lastDirectory;
    std::string path;
    const std::string base = std::filesystem::path(texture).stem().string(); // a picked file: its own name
    if (!PickSaveFile(dir + "/" + base + ".png", path, "png")) return;
    std::vector<uint8_t> bytes;
    mmp::Image image;
    std::string err;
    if (!Scene::ReadTextureBytes(lib, texture, bytes) || !DecodeTextureFile(bytes, image, err)) {
        st.pngOk = false;
        st.pngMessage = "Could not read " + texture + (err.empty() ? "" : ": " + err);
        return;
    }
    st.pngOk = png::Write(path, static_cast<int>(image.width), static_cast<int>(image.height), image.rgba);
    st.pngMessage = st.pngOk ? "Saved " + path : "Could not write " + path;
    if (st.pngOk) {
        size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) lib.gif.lastDirectory = path.substr(0, slash);
        lib.SaveConfig();
    }
}

// "Export UV": the shown figure's UV layout as a PNG, over the texture (at its exact size: pixel-perfect) or alone.
// Returns the message to show.
inline std::string ExportUvPng(Library& lib, Scene& scene, const std::string& texture, bool withTexture, bool& ok) {
    ok = false;
    LoadedModel loaded;
    if (scene.modelName.empty() || !LoadNamedModel(lib.figures, scene.modelName, loaded)) return "No figure shown";
    mmp::Image tex;
    std::vector<uint8_t> bytes;
    std::string err;
    const bool haveTex = !texture.empty() && Scene::ReadTextureBytes(lib, texture, bytes) && DecodeTextureFile(bytes, tex, err);
    if (!haveTex && withTexture) return "Could not read the texture " + texture + (err.empty() ? "" : ": " + err);
    if (!withTexture && haveTex) tex.rgba.assign(tex.rgba.size(), 0); // its size, transparent: still on its texels
    std::string dir = lib.gif.lastDirectory.empty() ? config::ExeDir() : lib.gif.lastDirectory;
    std::string path;
    if (!PickSaveFile(dir + "/" + scene.modelName + "_uv.png", path, "png")) return "";
    const uvmap::Result r = uvmap::Draw(loaded.model, scene.shownParts, haveTex ? &tex : nullptr, !scene.unitModel && scene.options.atlasUvs, 256);
    ok = png::Write(path, r.width, r.height, r.rgba);
    if (ok) {
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) lib.gif.lastDirectory = path.substr(0, slash);
        lib.SaveConfig();
    }
    return ok ? "Saved " + path : "Could not write " + path;
}

inline void TexturePicker(Library& lib, Scene& scene, ItemTabState& st) {
    const auto& r = st.resolution;
    std::string shown = ShownTexture(st);
    ImGui::TextDisabled("Texture");
    ImGui::SameLine(90);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("File...").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##texture", shown.empty() ? "(none)" : shown.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (!st.allTextures) {
            for (size_t i = 0; i < r.textures.size(); ++i) {
                const auto& o = r.textures[i];
                std::string label = o.name + "   " + o.note;
                bool selected = o.name == shown;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    st.textureOverride = static_cast<int>(i) == r.texture ? "" : o.name;
                    // A texture of another material also switches the material the details are computed for.
                    if (o.material && !st.textureOverride.empty()) st.material = o.material->name;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            if (r.textures.empty()) ImGui::TextDisabled("no candidate texture - tick \"all textures\" below");
        } else {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##texfilter", "filter", st.textureFilter, sizeof(st.textureFilter));
            std::string needle = LowerCopy(st.textureFilter);
            int listed = 0;
            for (const std::string& n : lib.textureIndex.names) {
                if (!needle.empty() && n.find(needle) == std::string::npos) continue;
                if (++listed > 400) { ImGui::TextDisabled("... type more of the name to narrow the list"); break; }
                if (ImGui::Selectable(n.c_str(), n == shown)) st.textureOverride = n;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickFile(picked)) st.textureOverride = picked;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview a texture file on this figure (.dds, .mmp, or any picture: .png, .jpg, .bmp, .tga...)");
    ImGui::Checkbox("all textures", &st.allTextures);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.textureOverride.empty());
    if (ImGui::SmallButton("Reset")) st.textureOverride.clear();
    ImGui::EndDisabled();

    if (!shown.empty()) {
        const GlTexture& t = scene.Texture(lib, shown);
        if (t.id) {
            float side = std::min(ImGui::GetContentRegionAvail().x, 160.0f);
            float h = t.width > 0 ? side * t.height / t.width : side;
            // On a checkerboard-grey background so the texture's transparent parts show as such.
            ImGui::ImageWithBg(static_cast<ImTextureID>(static_cast<intptr_t>(t.id)), ImVec2(side, h), ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", t.file.c_str());
            ImGui::TextDisabled("%d x %d", t.width, t.height);
            if (ImGui::Button("Export PNG")) ExportTexturePng(lib, shown, st);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save this texture as a PNG image, transparency included");
            if (ImGui::Button("Export UV")) st.pngMessage = ExportUvPng(lib, scene, shown, st.uvWithTexture, st.pngOk);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the figure's UV layout as a PNG, at the texture's size (each line on its texels)");
            ImGui::SameLine();
            ImGui::Checkbox("with texture", &st.uvWithTexture);
            if (!st.pngMessage.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, st.pngOk ? ImVec4(0.5f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
                ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
                ImGui::TextUnformatted(st.pngMessage.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();
        } else {
            Note(shown + ": " + t.error);
        }
    }
}

inline void ItemTab(Library& lib, Scene& scene, items::Category category, ItemTabState& st) {
    if (!lib.dbLoaded) {
        ImGui::TextWrapped("No items database loaded yet: set it in the Settings tab (database.res or databaselmp.res).");
        return;
    }
    const std::vector<items::Item>& list = lib.db.List(category);
    if (st.selected >= static_cast<int>(list.size())) SelectRow(st, -1);

    // --- filters
    std::set<std::string> types;
    for (auto& it : list) if (!it.type.empty()) types.insert(it.type);
    ImGui::SetNextItemWidth(types.size() > 1 ? ImGui::GetContentRegionAvail().x * 0.6f : -1);
    ImGui::InputTextWithHint("##filter", "search by name", st.filter, sizeof(st.filter));
    if (types.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##type", st.typeFilter.empty() ? "all types" : st.typeFilter.c_str())) {
            if (ImGui::Selectable("all types", st.typeFilter.empty())) st.typeFilter.clear();
            for (auto& t : types) if (ImGui::Selectable(t.c_str(), st.typeFilter == t)) st.typeFilter = t;
            ImGui::EndCombo();
        }
    }

    // --- the list (Up/Down move the selection while it has focus)
    std::vector<int> rows = VisibleRows(list, st);
    float listHeight = std::max(ImGui::GetContentRegionAvail().y * 0.38f, 120.0f);
    ImGui::BeginChild("##list", ImVec2(-1, listHeight), ImGuiChildFlags_Borders);
    if (ImGui::IsWindowFocused() && !rows.empty()) {
        auto pos = std::find(rows.begin(), rows.end(), st.selected);
        int index = pos == rows.end() ? -1 : static_cast<int>(pos - rows.begin());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) SelectRow(st, rows[std::min(index + 1, static_cast<int>(rows.size()) - 1)]);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) SelectRow(st, rows[std::max(index - 1, 0)]);
    }
    for (int row : rows) {
        const items::Item& it = list[row];
        ImGui::PushID(row);
        bool selected = row == st.selected;
        if (ImGui::Selectable(it.name.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) SelectRow(st, row);
        if (selected && st.scrollToSelected) {
            ImGui::SetScrollHereY(0.5f);
            st.scrollToSelected = false;
        }
        std::string tag = it.type.empty() ? "#" + std::to_string(it.tti) : it.type;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(tag.c_str()).x -
                        ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextDisabled("%s", tag.c_str());
        ImGui::PopID();
    }
    if (rows.empty()) ImGui::TextDisabled("nothing matches");
    ImGui::EndChild();
    ImGui::TextDisabled("%zu of %zu", rows.size(), list.size());

    if (st.selected < 0) {
        scene.LoadModel(lib, "", false);
        ImGui::TextWrapped("Pick an item to see its model.");
        return;
    }

    // --- the selected item
    const items::Item& item = list[st.selected];
    RefreshResolution(lib, item, st, lib.version);
    ShowInScene(lib, scene, st);
    const auto& r = st.resolution;
    const items::Material* material = r.material >= 0 ? r.materials[r.material] : nullptr;

    ImGui::SeparatorText(item.name.c_str());
    std::string figure = ShownFigure(lib, st);
    StatusDot(scene.hasModel, figure + ": " + std::to_string(scene.vertexCount) + " vertices, " +
                                  std::to_string(scene.triangleCount) + " triangles",
              scene.modelError.empty() ? "no figure" : scene.modelError);
    ImGui::SameLine();
    ImGui::TextDisabled("Figure");
    ImGui::SameLine(90);
    FigurePicker(lib, st);
    MaterialPicker(lib, st);
    TexturePicker(lib, scene, st);
    for (auto& note : r.notes) Note(note);

    // The item's in-game name and description, above its stats (item_texts.hpp).
    texts::ItemText text;
    if (lib.texts.AnyLoaded()) {
        text = texts::Lookup(lib.texts, item, material, &lib.textEncodings);
        ImGui::Spacing();
        if (text.found) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.55f, 1.0f));
            ImGui::TextWrapped("%s", text.name.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.key.c_str());
            if (!text.description.empty()) ImGui::TextWrapped("%s", text.description.c_str());
            ImGui::Spacing();
        } else if (!text.key.empty()) {
            ImGui::TextDisabled("No text \"%s\" in the text sources", text.key.c_str());
        }
    }
    if (ImGui::BeginTable("##details", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        if (!item.type.empty()) Row("Type", item.type);
        if (!item.materialType.empty() && item.materialType != "None") Row("Material class", item.materialType);
        Row("TTI / TTI2", std::to_string(item.tti) + " / " + std::to_string(item.tti2));
        Row("Price / weight", ui::Num(item.price, 0) + " / " + ui::Num(item.weight));
        if (material && category != items::Category::LootItems) Row("Material factor", ui::Num(material->damage) + " (" + material->name + ")");
        switch (category) {
        case items::Category::Weapons: WeaponDetails(item, material); break;
        case items::Category::Armors: ArmorDetails(item); break;
        case items::Category::QuickItems: QuickDetails(item); break;
        case items::Category::QuestItems: QuestDetails(item); break;
        default: break;
        }
        ImGui::EndTable();
    }
}

} // namespace ui
