// 3D Viewer > Units: a unit of the database (Monsters), dressed as the game and the Map Editor dress it
// (mapedit/dress.hpp: its race's figure, skin, hair, weapons and armour), with every part of that open to
// change: the skin (by the race's list, or any texture file, e.g. a new skin being painted), the hair, the
// complection, the weapons and the armour with their materials.
//
// Animations: the figure's clips (its .anm) played in place, at the game's 15 frames a second
// (docs/file-formats/figure-format.md for how a clip poses the parts).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "library.hpp"
#include "model_loader.hpp"
#include "scene.hpp"
#include "ui_items.hpp"
#include "ui_common.hpp"
#include "../mapedit/dress.hpp"

namespace ui {

struct UnitsTabState {
    char filter[96] = "";
    int selected = -1;            // in lib.unitsDb.monsters
    bool dirty = true;            // rebuild the scene's model
    bool frame = true;            // and frame it
    // Overrides of the monster's own values
    int skin = -1;                // index in the race's primary textures; -1: the monster's
    std::string customSkin;       // a texture name or a file: wins over `skin`
    int hair = -2;                // hr.NN; -1 none; -2: the monster's
    float complection[3] = {0.5f, 0.5f, 0.5f};
    struct Worn { std::string item, material; };
    std::vector<Worn> weapons, armour; // "item.material" entries, as a map unit lists them
    std::string summary;          // what the dressing did
    bool uvWithTexture = true;    // Export UV: over the skin, or alone
    std::string uvMessage;
    std::vector<int> hairs;       // the hr.NN the figure has
    std::string hairsFor;         // ... for this figure
    // Animation: the clip shown ("" = at rest), its frame (fractional), playing or not
    std::string clip;
    float animSpeed = 1.0f, animTime = 0.0f;
    bool animPlaying = false;
};

namespace units_detail {

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// "blueprint.material" split at the last dot.
inline void Split(const std::string& full, std::string& item, std::string& material) {
    const size_t dot = full.find_last_of('.');
    item = dot == std::string::npos ? full : full.substr(0, dot);
    material = dot == std::string::npos ? "" : full.substr(dot + 1);
}
inline void ResetFromMonster(const Library& lib, UnitsTabState& st) {
    st.skin = -1;
    st.customSkin.clear();
    st.hair = -2;
    st.weapons.clear();
    st.armour.clear();
    if (st.selected < 0 || st.selected >= static_cast<int>(lib.unitsDb.monsters.size())) return;
    const units::Monster& m = lib.unitsDb.monsters[static_cast<size_t>(st.selected)];
    for (const std::string& w : {m.weapon1, m.weapon2}) {
        if (w.empty()) continue;
        UnitsTabState::Worn x;
        Split(w, x.item, x.material);
        st.weapons.push_back(x);
    }
    for (const std::string& a : m.wears) {
        UnitsTabState::Worn x;
        Split(a, x.item, x.material);
        st.armour.push_back(x);
    }
}
// The hr.NN parts a figure has (hair choices).
inline const std::vector<int>& Hairs(const Library& lib, const std::string& figure, UnitsTabState& st) {
    if (st.hairsFor == figure) return st.hairs;
    st.hairsFor = figure;
    st.hairs.clear();
    LoadedModel loaded;
    if (!LoadNamedModel(lib.figures, figure, loaded)) return st.hairs;
    for (const fig::ModelPart& p : loaded.model.parts) {
        const std::string n = Lower(p.name);
        if (n.rfind("hr.", 0) == 0 && n.size() >= 5 && std::isdigit(static_cast<unsigned char>(n[3]))) st.hairs.push_back(std::atoi(n.c_str() + 3));
    }
    std::sort(st.hairs.begin(), st.hairs.end());
    st.hairs.erase(std::unique(st.hairs.begin(), st.hairs.end()), st.hairs.end());
    return st.hairs;
}
// An item picker over a database category (searchable); "" = none.
inline bool ItemCombo(const Library& lib, const char* id, items::Category c, std::string& value, const char* filterType = nullptr) {
    bool changed = false;
    ImGui::SetNextItemWidth(-90);
    if (ImGui::BeginCombo(id, value.empty() ? "(none)" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::InputTextWithHint("##f", "search", filter, sizeof filter);
        const std::string f = Lower(filter);
        for (const items::Item& it : lib.db.List(c)) {
            if (filterType && Lower(it.type) == filterType) continue;
            if (!f.empty() && Lower(it.name).find(f) == std::string::npos) continue;
            if (ImGui::Selectable((it.name + "  (" + it.type + ")").c_str(), Lower(it.name) == Lower(value))) { value = it.name; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}
inline bool MaterialCombo(const Library& lib, const char* id, items::Category c, const std::string& item, std::string& value) {
    std::string type;
    for (const items::Item& it : lib.db.List(c)) if (Lower(it.name) == Lower(item)) { type = it.materialType; break; }
    bool changed = false;
    ImGui::SetNextItemWidth(85);
    if (ImGui::BeginCombo(id, value.empty() ? "-" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const items::Material* m : lib.db.MaterialsOfType(type))
            if (ImGui::Selectable(m->name.c_str(), Lower(m->name) == Lower(value))) { value = m->name; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

// Builds the scene's model for the current choices.
inline void Rebuild(const Library& lib, Scene& scene, UnitsTabState& st) {
    st.dirty = false;
    const bool frame = st.frame;
    st.frame = false;
    if (st.selected < 0 || st.selected >= static_cast<int>(lib.unitsDb.monsters.size())) {
        scene.LoadModel(lib, "", false);
        return;
    }
    const units::Monster& m = lib.unitsDb.monsters[static_cast<size_t>(st.selected)];
    const units::Race* race = lib.unitsDb.FindRace(m.race);
    // A map unit standing in for it: dress::Resolve then applies the game's rules.
    mob::Object o;
    o.kind = mob::Kind::Unit;
    o.primTexture = "default0";
    o.prototype = m.name;
    o.templ = race && !race->mask.empty() ? race->mask : "";
    mob::ItemList weapons, armour;
    weapons.type = mob::kUnitWeapons;
    armour.type = mob::kUnitArmors;
    for (const auto& w : st.weapons) if (!w.item.empty()) weapons.entries.push_back(w.item + "." + w.material);
    for (const auto& a : st.armour) if (!a.item.empty()) armour.entries.push_back(a.item + "." + a.material);
    // An empty list would fall back to the monster's defaults: a "none" entry keeps it bare.
    if (weapons.entries.empty()) weapons.entries.push_back("none.none");
    if (armour.entries.empty()) armour.entries.push_back("none.none");
    o.lists.push_back(weapons);
    o.lists.push_back(armour);
    dress::Dress d = dress::Resolve(lib, o);
    if (!d.on) { scene.LoadModel(lib, o.templ, frame); st.summary = "not dressed (no race or figure)"; return; }
    // Skin: another of the race's textures, or a texture of one's own.
    std::string skin = d.skin;
    if (!st.customSkin.empty()) skin = st.customSkin;
    else if (st.skin >= 0 && race && st.skin < static_cast<int>(race->primary.size())) skin = dress::detail::RaceTexture(race->primary[static_cast<size_t>(st.skin)], Lower(race->mask));
    if (skin != d.skin) {
        const size_t bar = d.body.find('|');
        d.body = "compose:" + skin + (bar == std::string::npos ? "" : d.body.substr(bar));
        d.skin = skin;
    }
    if (st.hair == -1) d.selected.erase("hr");
    else if (st.hair >= 0) { char num[16]; std::snprintf(num, sizeof num, "%02d", st.hair); d.selected["hr"] = std::string("hr.") + num; }
    st.summary = d.summary;
    const fig::Vec3 k{st.complection[0], st.complection[1], st.complection[2]};
    scene.LoadUnit(lib, o.templ, k, frame,
                   [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartShown(d, model, part); },
                   [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartTexture(d, model, part); });
    scene.textureName = skin;
}

} // namespace units_detail

inline void UnitsTab(Library& lib, Scene& scene, UnitsTabState& st) {
    using namespace units_detail;
    if (lib.unitsDb.monsters.empty()) {
        ImGui::TextWrapped("No units: set a database (database.res / databaselmp.res, with units.udb) in Settings.");
        return;
    }
    // The list
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##unitfilter", "filter, e.g. human, orc, kania", st.filter, sizeof st.filter);
    const std::string f = Lower(st.filter);
    ImGui::BeginChild("##units", ImVec2(0, 220), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < lib.unitsDb.monsters.size(); ++i) {
        const units::Monster& m = lib.unitsDb.monsters[i];
        if (!f.empty() && Lower(m.name + " " + m.race).find(f) == std::string::npos) continue;
        ImGui::PushID(static_cast<int>(i)); // names repeat in the database
        const bool clicked = ImGui::Selectable(m.name.c_str(), st.selected == static_cast<int>(i)) || NavMovedHere(); // Up/Down select too
        ImGui::PopID();
        if (clicked) {
            st.selected = static_cast<int>(i);
            ResetFromMonster(lib, st);
            st.dirty = st.frame = true;
        }
    }
    ImGui::EndChild();
    if (st.selected < 0 || st.selected >= static_cast<int>(lib.unitsDb.monsters.size())) {
        ImGui::TextDisabled("Pick a unit.");
        if (st.dirty) Rebuild(lib, scene, st);
        return;
    }
    const units::Monster& m = lib.unitsDb.monsters[static_cast<size_t>(st.selected)];
    const units::Race* race = lib.unitsDb.FindRace(m.race);
    ImGui::Text("%s: %s, figure %s", m.name.c_str(), m.race.c_str(), race ? race->mask.c_str() : "?");
    if (!st.summary.empty()) ImGui::TextDisabled("%s", st.summary.c_str());
    if (ImGui::Button("Reset to the database's")) { ResetFromMonster(lib, st); st.dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Remove all")) { st.weapons.clear(); st.armour.clear(); st.dirty = true; }
    ImGui::SetItemTooltip("No weapon nor armour: the bare skin (to look at a skin)");

    // Skin
    ImGui::SeparatorText("Skin");
    if (race && !race->primary.empty()) {
        const int current = st.skin >= 0 ? st.skin : m.skin;
        const std::string label = current >= 0 && current < static_cast<int>(race->primary.size()) ? race->primary[static_cast<size_t>(current)] : "?";
        ImGui::SetNextItemWidth(-90);
        if (ImGui::BeginCombo("Skin", label.c_str())) {
            for (size_t i = 0; i < race->primary.size(); ++i)
                if (ImGui::Selectable((std::to_string(i) + "  " + race->primary[i]).c_str(), static_cast<int>(i) == current)) { st.skin = static_cast<int>(i); st.dirty = true; }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The race's textures; the database's skin index picks one");
    }
    char custom[512];
    std::snprintf(custom, sizeof custom, "%s", st.customSkin.c_str());
    ImGui::SetNextItemWidth(-160);
    if (ImGui::InputTextWithHint("##custom", "custom skin: texture name or file", custom, sizeof custom, ImGuiInputTextFlags_EnterReturnsTrue)) { st.customSkin = custom; st.dirty = true; }
    ImGui::SetItemTooltip("A texture of the sources (e.g. unhumaskin_05) or a .png / .dds / .mmp file: shown instead of the skin,\n"
        "with the worn items painted over it like in the game. Enter applies it.");
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && PickFile(picked)) { st.customSkin = picked; st.dirty = true; scene.ClearTextures(); }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { scene.ClearTextures(); st.dirty = true; }
    ImGui::SetItemTooltip("Read the textures again (after saving the skin being painted)");
    if (!st.customSkin.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("X##clearskin")) { st.customSkin.clear(); st.dirty = true; }
    }
    if (ImGui::Button("Export UV")) { bool ok; st.uvMessage = ExportUvPng(lib, scene, scene.textureName, st.uvWithTexture, ok); }
    ImGui::SetItemTooltip("Save the shown parts' UV layout as a PNG, at the skin's size (each line on its texels): a painting guide");
    ImGui::SameLine();
    ImGui::Checkbox("with the skin", &st.uvWithTexture);
    if (!st.uvMessage.empty()) ImGui::TextWrapped("%s", st.uvMessage.c_str());

    // Hair and complection
    ImGui::SeparatorText("Body");
    if (race) {
        const std::vector<int>& hairs = Hairs(lib, race->mask, st);
        const int current = st.hair == -2 ? m.hair : st.hair;
        char label[32];
        std::snprintf(label, sizeof label, current < 0 ? "none" : "hr.%02d", current);
        ImGui::SetNextItemWidth(-90);
        if (ImGui::BeginCombo("Hair", label)) {
            if (ImGui::Selectable("none", current < 0)) { st.hair = -1; st.dirty = true; }
            for (int h : hairs) {
                std::snprintf(label, sizeof label, "hr.%02d", h);
                if (ImGui::Selectable(label, h == current)) { st.hair = h; st.dirty = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A helm hides the hair, as in the game");
    }
    ImGui::SetNextItemWidth(-150);
    if (ImGui::SliderFloat3("##complection", st.complection, 0.0f, 1.0f, "%.2f")) st.dirty = true;
    ImGui::SetItemTooltip("Body proportions (the figure's morphs); Ctrl+click a value to type it");
    ImGui::SameLine();
    ImGui::BeginDisabled(st.complection[0] == 0.5f && st.complection[1] == 0.5f && st.complection[2] == 0.5f);
    if (ImGui::SmallButton("Reset##complection")) {
        st.complection[0] = st.complection[1] = st.complection[2] = 0.5f;
        st.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back to 0.5 / 0.5 / 0.5: the middle of every morph");
    ImGui::SameLine();
    ImGui::TextUnformatted("Complection");

    // Equipment
    ImGui::SeparatorText("Weapons");
    for (size_t i = 0; i < st.weapons.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ItemCombo(lib, "##w", items::Category::Weapons, st.weapons[i].item)) st.dirty = true;
        ImGui::SameLine();
        if (MaterialCombo(lib, "##wm", items::Category::Weapons, st.weapons[i].item, st.weapons[i].material)) st.dirty = true;
        ImGui::PopID();
    }
    if (st.weapons.size() < 2 && ImGui::SmallButton("Add a weapon")) { st.weapons.push_back({}); }
    if (!st.weapons.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove the last##w")) { st.weapons.pop_back(); st.dirty = true; }
    }
    ImGui::SeparatorText("Armour");
    int remove = -1;
    for (size_t i = 0; i < st.armour.size(); ++i) {
        ImGui::PushID(1000 + static_cast<int>(i));
        if (ItemCombo(lib, "##a", items::Category::Armors, st.armour[i].item)) st.dirty = true;
        ImGui::SameLine();
        if (MaterialCombo(lib, "##am", items::Category::Armors, st.armour[i].item, st.armour[i].material)) st.dirty = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) { st.armour.erase(st.armour.begin() + remove); st.dirty = true; }
    if (ImGui::SmallButton("Add armour")) st.armour.push_back({});

    // Animation
    ImGui::SeparatorText("Animation");
    if (scene.clips.empty()) {
        ImGui::TextDisabled("This figure has no animations (.anm).");
    } else {
        if (!st.clip.empty() && !scene.clips.count(st.clip)) st.clip.clear();
        ImGui::SetNextItemWidth(-90);
        if (ImGui::BeginCombo("Animation", st.clip.empty() ? "(at rest)" : st.clip.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("(at rest)", st.clip.empty())) { st.clip.clear(); st.animTime = 0; }
            for (const auto& kv : scene.clips)
                if (ImGui::Selectable(kv.first.c_str(), kv.first == st.clip)) { st.clip = kv.first; st.animTime = 0; st.animPlaying = true; }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("c...: loops (idle, walk, run); u...: actions (attacks, casts, hits, deaths); s...: stance changes");
        const auto ci = scene.clips.find(st.clip);
        const int frames = ci != scene.clips.end() ? static_cast<int>(ci->second.FrameCount()) : 0;
        ImGui::BeginDisabled(frames == 0);
        if (ImGui::Button(st.animPlaying ? "Pause" : "Play")) st.animPlaying = !st.animPlaying;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Speed", &st.animSpeed, 0.1f, 3.0f, "%.1fx");
        ImGui::SetNextItemWidth(-90);
        if (ImGui::SliderFloat("Frame", &st.animTime, 0.0f, static_cast<float>(std::max(frames - 1, 0)), "%.0f")) st.animPlaying = false;
        ImGui::EndDisabled();
        if (frames > 0 && st.animPlaying) st.animTime = std::fmod(st.animTime + ImGui::GetIO().DeltaTime * 15.0f * st.animSpeed, static_cast<float>(frames));
        if (frames > 0) ImGui::TextDisabled("%d frames, %.1f s at the game's 15 frames a second", frames, frames / 15.0f);
    }

    if (st.dirty) Rebuild(lib, scene, st);
    scene.Pose(st.clip, st.animTime); // every frame: the scene's vertices follow the clip (or go back to rest)
}

} // namespace ui
