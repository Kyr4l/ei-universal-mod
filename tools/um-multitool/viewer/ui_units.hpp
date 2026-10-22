// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
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
#include "weapon_import.hpp"

namespace ui {

// The Weapon import panel's choices and its last result.
struct WeaponImportState {
    char model[512] = "";
    int kind = 0;                   // index in wimp::AllKinds()
    int number = -1;                // the model number: crbow04... (the item's TTI); -1: the first free one
    char reference[8] = "";         // the vanilla variant whose scales are used; empty: the kind's own
    int uvMode = 0;                 // 0: the file's own (direct texture coordinates, v down from the top row), 1: a ground model's, through the 256 atlas
    int uvWidth = 128;              // the texture's width in pixels, for uvMode 0
    int normals = 0;                // 0: as the model has them, 1: smoothed (one per vertex, like ei_figer)
    bool autoFit = true, level = false, heightFromGrip = true, prodAtHinge = true, replace = false, flip = false, keepDepth = false, ground = true;
    float scale = 0;                // 0: the kind's own (crossbow, bow: from the strings; melee: 1)
    float shift[3] = {0, 0, 0};
    int up = 1;                     // crossbow: 0 auto (experimental), 1 the model's +Z, 2 turned over
    float roll = 0, grip = 0;       // melee
    struct Edit { int target = 0; float t0 = 0.6f, t1 = 0.85f, factor = 0.8f, soft = 0.06f; }; // target 0 main (the weapon / stock), 1 limbs, 2 strings
    std::vector<Edit> edits;
    std::string projectPath;
    std::vector<wimp::RaceOutput> outputs; // the last import
    std::vector<std::string> log;
    std::string message;
    bool previewing = false;
};

struct UnitsTabState {
    char filter[96] = "";
    int selected = -1;            // in lib.unitsDb.monsters
    bool dirty = true;            // rebuild the scene's model
    bool frame = true;            // and frame it
    float zoomOverride = -1.0f;   // gui --viewer units --zoom: the camera distance x this after the next rebuild (<= 0: none)
    float lookShift[3] = {0, 0, 0}; // ... and the camera target moved by this (--look)
    float yawOverride = -1000.0f; // gui --viewer units --yaw: the camera's yaw after the next rebuild (-1000: none)
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
    WeaponImportState wimport;
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
    const bool dump = std::getenv("UM_DRESS_DEBUG") != nullptr; // research: every part, shown or not, and its texture
    if (dump) {
        std::fprintf(stderr, "dress %s: %s\n  body %s\n", m.name.c_str(), d.summary.c_str(), d.body.c_str());
        for (const auto& kv : d.selected) std::fprintf(stderr, "  selected %s = %s\n", kv.first.c_str(), kv.second.c_str());
        for (const auto& kv : d.texture) std::fprintf(stderr, "  texture %s = %s\n", kv.first.c_str(), kv.second.c_str());
    }
    scene.LoadUnit(lib, o.templ, k, frame,
                   [&](const fig::Model& model, const fig::ModelPart& part) {
                       const bool shown = dress::PartShown(d, model, part);
                       if (dump) std::fprintf(stderr, "  part %-24s %s  tex#%d group %d  %s\n", part.name.c_str(), shown ? "SHOWN " : "hidden", part.mesh.textureNumber, part.mesh.group, shown ? dress::PartTexture(d, model, part).c_str() : "");
                       return shown;
                   },
                   [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartTexture(d, model, part); });
    if (st.yawOverride > -999.0f) { scene.camera.yawDeg = st.yawOverride; st.yawOverride = -1000.0f; }
    if (st.zoomOverride > 0) {
        scene.camera.distance *= st.zoomOverride;
        scene.camera.targetX += st.lookShift[0]; scene.camera.targetY += st.lookShift[1]; scene.camera.targetZ += st.lookShift[2];
        st.zoomOverride = -1.0f;
    }
    scene.textureName = skin;
}


// ---- Weapon import ------------------------------------------------------------------------------------
inline wimp::ImportOptions WeaponOptions(const WeaponImportState& w) {
    static const char* const targets[] = {"main", "limbs", "strings"};
    wimp::ImportOptions opt;
    opt.kind = wimp::AllKinds()[static_cast<size_t>(std::max(0, std::min(static_cast<int>(wimp::AllKinds().size()) - 1, w.kind)))].id;
    opt.number = w.number;
    opt.referenceNumber = w.reference[0] ? wimp::TwoDigits(std::atoi(w.reference)) : std::string();
    opt.uvMode = w.uvMode == 1 ? wimp::UvMode::Texture : wimp::UvMode::Raw;
    opt.uvTextureWidth = w.uvWidth;
    opt.normals = w.normals == 1 ? wimp::NormalMode::Smooth : wimp::NormalMode::Keep;
    opt.replace = w.replace;
    opt.fit.autoFit = w.autoFit;
    opt.fit.levelStock = w.level;
    opt.fit.gripHeight = w.heightFromGrip;
    opt.fit.prodAtHinge = w.prodAtHinge;
    opt.fit.scale = w.scale;
    opt.fit.shift = {w.shift[0], w.shift[1], w.shift[2]};
    opt.fit.flip = w.flip;
    opt.fit.keepDepth = w.keepDepth;
    opt.fit.upAuto = w.up == 0; opt.fit.upIsZ = w.up != 2;
    opt.ground = w.ground;
    opt.fit.roll = w.roll;
    opt.fit.grip = w.grip;
    for (const auto& e : w.edits) opt.edits.push_back({targets[std::max(0, std::min(2, e.target))], e.t0, e.t1, e.factor, e.soft});
    return opt;
}

// Runs the import of the panel's model into the human rigs (nothing is written): the result is in w.outputs / w.log.
inline bool RunWeaponImport(Library& lib, WeaponImportState& w) {
    w.outputs.clear();
    w.log.clear();
    lib.figures.overrides.clear(); // the real rigs, not a previous preview
    w.previewing = false;
    std::string text, err;
    if (!wimp::ReadWholeFile(w.model, text)) { w.message = std::string("cannot read ") + w.model; return false; }
    wimp::ObjFile obj;
    if (!wimp::ParseObj(text, obj, err)) { w.message = err; return false; }
    if (!wimp::ImportWeapon(lib.figures, obj, WeaponOptions(w), w.outputs, w.log, err)) { w.message = err; w.outputs.clear(); return false; }
    w.message = "imported into " + std::to_string(w.outputs.size()) + " rigs, as " + wimp::AllKinds()[static_cast<size_t>(w.kind)].label + " " + wimp::TwoDigits(w.outputs[0].number) + ": nothing is saved yet";
    return true;
}

inline void WeaponImportPanel(Library& lib, UnitsTabState& st) {
    WeaponImportState& w = st.wimport;
    if (!ImGui::CollapsingHeader("Weapon import")) return;
    const std::vector<wimp::Kind>& kinds = wimp::AllKinds();
    w.kind = std::max(0, std::min(static_cast<int>(kinds.size()) - 1, w.kind));
    const wimp::Kind& kind = kinds[static_cast<size_t>(w.kind)];
    const bool crossbow = kind.family == wimp::Family::Crossbow, bow = kind.family == wimp::Family::Bow, melee = kind.family == wimp::Family::Melee;
    ImGui::TextDisabled("A new weapon for the human rigs. Nothing is written until Save.");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##wimodel", "Model (.obj)", w.model, sizeof w.model);
    ImGui::SetItemTooltip("Crossbow: 5 pieces (main, part01/02, tetiva01/02)\nBow: 4 (partb, parta, tetivab, tetivaa)\nMelee: the whole model is one weapon");
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...##wimodel") && PickFile(picked)) std::snprintf(w.model, sizeof w.model, "%s", picked.c_str());
    {
        std::string names;
        for (const wimp::Kind& k : kinds) { names += k.label; names += '\0'; }
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("Kind", &w.kind, names.c_str());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("Number", &w.number, 0, 0);
    w.number = std::max(-1, std::min(99, w.number));
    ImGui::SetItemTooltip("The model number of the item that holds it (its TTI). -1: the first free one");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    ImGui::InputText("Like", w.reference, sizeof w.reference, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SetItemTooltip("The vanilla variant whose scales are used. Empty: %s", crossbow ? "02" : bow ? "00" : "all of them, averaged");
    const char* uvs[] = {"direct (hand-held)", "atlas (ground model)"};
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("UVs", &w.uvMode, uvs, 2);
    ImGui::SetItemTooltip("Hand-held: u across, v down from the top row.\nAtlas: a ground / inventory model's, through the 256 atlas.");
    if (w.uvMode == 1) { ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt("px", &w.uvWidth, 0, 0); w.uvWidth = std::max(16, std::min(2048, w.uvWidth)); }
    ImGui::SameLine();
    const char* nm[] = {"as is", "smooth"};
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("Normals", &w.normals, nm, 2);
    ImGui::SetItemTooltip("Smooth: one normal per vertex, like ei_figer's export");
    ImGui::Checkbox("Fit", &w.autoFit);
    ImGui::SetItemTooltip("Turn, scale and place the model on the rig.\nOff: it is already in the rig's frame (Export reference).");
    ImGui::BeginDisabled(!w.autoFit);
    if (crossbow) {
        ImGui::SameLine(); ImGui::Checkbox("level", &w.level);
        ImGui::SetItemTooltip("Stock exactly along the axis, not at the reference's tilt");
        ImGui::SameLine(); ImGui::Checkbox("height from grip", &w.heightFromGrip);
        ImGui::SetItemTooltip("Off: the string ends' height decides");
        ImGui::SameLine(); ImGui::Checkbox("limbs at hinge", &w.prodAtHinge);
        ImGui::SetNextItemWidth(90);
        const char* ups[] = {"auto (test)", "model +Z", "turned over"};
        ImGui::Combo("Up", &w.up, ups, 3);
        ImGui::SetItemTooltip("Which way is up on the crossbow: the model's +Z, or turned over (half a turn about the stock)");
        ImGui::SetItemTooltip("Lower the limbs and strings until the limb roots sit at the hinge's height (some female death clips turn a limb over)");
    }
    if (bow) {
        ImGui::SameLine(); ImGui::Checkbox("keep depth", &w.keepDepth);
        ImGui::SetItemTooltip("Off: the limbs are bent so the grip sits on the rig's pivot and the string on its anchors.\nOn: the model's own depth (the limb halves come apart at the grip in the draw clips if it differs).");
    }
    if (melee) {
        ImGui::SameLine(); ImGui::Checkbox("flip", &w.flip);
        ImGui::SetItemTooltip("Tip and butt the other way round");
        ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputFloat("Roll", &w.roll, 0, 0, "%.0f");
        ImGui::SetItemTooltip("Extra turn about the weapon's length, degrees");
        ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputFloat("Grip", &w.grip, 0, 0, "%.3f");
        ImGui::SetItemTooltip("The hand's distance from the butt. 0: the vanilla rule");
    }
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat("Scale", &w.scale, 0, 0, "%.4f");
    ImGui::SetItemTooltip(melee ? "0: 1 (the model's own size; the female rig gets her vanilla ratio on top)" : "0: from the strings' outer ends");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputFloat3("Shift", w.shift, "%.3f");
    ImGui::SetItemTooltip("Extra move after the fit, rig units (+X right, +Y toward the shooter, +Z up)");
    ImGui::EndDisabled();
    ImGui::Checkbox("Replace", &w.replace);
    if (crossbow) {
        ImGui::SameLine();
        ImGui::Checkbox("Ground model", &w.ground);
        ImGui::SetItemTooltip("Also make the ground / inventory figure (initwecb<N>weapon.fig)");
    }
    ImGui::SetItemTooltip("Overwrite parts of that number the rig already has");

    // Shape edits: the model's one mesh is reshaped before every corner and both races are built from it.
    ImGui::SeparatorText("Shape edits");
    ImGui::TextDisabled("t: 0 at the %s, 1 at the %s", melee ? "tip" : crossbow ? "limbs (stock)" : "middle", melee || crossbow ? "butt" : "tip");
    bool changed = false;
    int removeEdit = -1;
    for (size_t i = 0; i < w.edits.size(); ++i) {
        ImGui::PushID(2000 + static_cast<int>(i));
        auto& e = w.edits[i];
        const char* tg[] = {melee ? "weapon" : "stock", "limbs", "strings"};
        ImGui::SetNextItemWidth(80);
        if (melee || (bow && e.target == 0)) e.target = melee ? 0 : 1;
        changed |= ImGui::Combo("##target", &e.target, tg, 3);
        ImGui::SameLine(); ImGui::SetNextItemWidth(150);
        changed |= ImGui::DragFloatRange2("##range", &e.t0, &e.t1, 0.005f, 0.0f, 1.0f, "from %.2f", "to %.2f");
        ImGui::SetItemTooltip("The stretch that changes");
        ImGui::SameLine(); ImGui::SetNextItemWidth(120);
        changed |= ImGui::SliderFloat("##factor", &e.factor, 0.2f, 1.8f, "x%.2f");
        ImGui::SetItemTooltip("Thickness: 1 unchanged, 0.7 a third thinner (about the piece's centre line)");
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        changed |= ImGui::SliderFloat("##soft", &e.soft, 0.0f, 0.3f, "%.2f");
        ImGui::SetItemTooltip("How softly it fades in and out");
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeEdit = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeEdit >= 0) { w.edits.erase(w.edits.begin() + removeEdit); changed = true; }
    if (ImGui::SmallButton("Add edit")) { WeaponImportState::Edit e; e.target = melee ? 0 : crossbow ? 0 : 1; w.edits.push_back(e); }
    ImGui::SetItemTooltip("E.g. a thinner handle: from 0.64 to 0.86, x0.7");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open project...")) {
        std::string path, text, model, err;
        if (PickFile(path) && wimp::ReadWholeFile(path, text)) {
            wimp::ImportOptions po;
            const size_t slash = path.find_last_of("/\\");
            if (!wimp::ParseProject(text, slash == std::string::npos ? "" : path.substr(0, slash), model, po, err)) w.message = path + ": " + err;
            else {
                std::snprintf(w.model, sizeof w.model, "%s", model.c_str());
                for (size_t k = 0; k < kinds.size(); ++k) if (po.kind == kinds[k].id) w.kind = static_cast<int>(k);
                w.number = po.number; std::snprintf(w.reference, sizeof w.reference, "%s", po.referenceNumber.c_str());
                w.uvMode = po.uvMode == wimp::UvMode::Texture ? 1 : 0; w.uvWidth = po.uvTextureWidth; w.normals = po.normals == wimp::NormalMode::Smooth ? 1 : 0;
                w.autoFit = po.fit.autoFit; w.level = po.fit.levelStock; w.heightFromGrip = po.fit.gripHeight; w.prodAtHinge = po.fit.prodAtHinge; w.replace = po.replace;
                w.flip = po.fit.flip; w.keepDepth = po.fit.keepDepth; w.up = po.fit.upAuto ? 0 : po.fit.upIsZ ? 1 : 2; w.ground = po.ground; w.roll = static_cast<float>(po.fit.roll); w.grip = static_cast<float>(po.fit.grip);
                w.scale = static_cast<float>(po.fit.scale); w.shift[0] = po.fit.shift.x; w.shift[1] = po.fit.shift.y; w.shift[2] = po.fit.shift.z;
                w.edits.clear();
                for (const auto& e : po.edits) w.edits.push_back({e.piece == "limbs" ? 1 : e.piece == "strings" ? 2 : 0, static_cast<float>(e.t0), static_cast<float>(e.t1), static_cast<float>(e.factor), static_cast<float>(e.soft)});
                w.projectPath = path; w.message = "project " + path + " loaded"; changed = true;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save project...")) {
        std::string path;
        if (PickSaveFile(w.projectPath.empty() ? "weapon.umweapon" : w.projectPath, path, "umweapon")) {
            const std::string text = wimp::ProjectText(w.model, WeaponOptions(w));
            w.message = wimp::WriteWholeFile(path, text.data(), text.size()) ? "project saved: " + path + " (viewer --weapon-project <it> --out <folder> rebuilds everything)" : "cannot write " + path;
            if (w.message.rfind("project saved", 0) == 0) w.projectPath = path;
        }
    }
    ImGui::SetItemTooltip("Model, options and edits in one small text file");

    bool doPreview = changed && w.previewing;   // an edit changed while a preview is shown: show the new one
    if (ImGui::Button("Preview") || doPreview) {
        if (RunWeaponImport(lib, w)) {
            for (const wimp::RaceOutput& ro : w.outputs) {
                lib.figures.overrides[ro.race + ".mod"] = ro.mod;
                lib.figures.overrides[ro.race + ".bon"] = ro.bon;
            }
            w.previewing = true;
            // hold a weapon of that kind with that model number, so the new parts show
            bool held = false;
            for (const items::Item& it : lib.db.List(items::Category::Weapons)) {
                if (LowerCopy(it.type) != kind.itemType || it.tti != w.outputs[0].number) continue;
                const auto mats = lib.db.MaterialsOfType(it.materialType);
                st.weapons.clear();
                st.weapons.push_back({it.name, mats.empty() ? "" : mats[0]->name});
                held = true;
                break;
            }
            if (!held) w.message += " (no " + std::string(kind.itemType) + " item has the model number " + std::to_string(w.outputs[0].number) + ": add one to the database, or give Number an existing item's with Replace)";
            st.dirty = true;
        }
    }
    ImGui::SetItemTooltip("Show it on the unit above (pick a Human Male / Female: it holds the item with that model number)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!w.previewing);
    if (ImGui::Button("Stop")) { lib.figures.overrides.clear(); w.previewing = false; st.dirty = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(w.outputs.empty());
    if (ImGui::Button("Save...")) {
        std::string dir;
        if (PickFolder(dir)) {
            std::string err;
            w.message = wimp::SaveImport(lib.figures, w.outputs, w.log, dir, "", err, w.uvMode == 1 ? wimp::UvMode::Texture : wimp::UvMode::Raw, w.uvWidth) ? "saved unhuma / unhufe .mod and .bon, placed-*.obj and import-report.txt in " + dir : err;
        }
    }
    ImGui::SetItemTooltip("<race>.mod / <race>.bon (the rigs with the new parts), the placed model as OBJ and the report, in a folder.\nThe game's and the mod's own files are never written.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Export reference...")) {
        std::string path;
        char suggestion[64];
        const std::string refNN = w.reference[0] ? wimp::TwoDigits(std::atoi(w.reference)) : std::string(kind.defaultReference).empty() ? (melee ? "02" : "00") : std::string(kind.defaultReference);
        std::snprintf(suggestion, sizeof suggestion, "%s-%s.obj", kind.id, refNN.c_str());
        if (PickSaveFile(suggestion, path, "obj")) {
            wimp::Rig rig;
            std::string err;
            const std::map<std::string, std::vector<uint8_t>> saved = lib.figures.overrides;
            lib.figures.overrides.clear();
            const bool loaded = wimp::LoadRig(lib.figures, "unhuma", rig, err);
            lib.figures.overrides = saved;
            const std::string text = loaded ? wimp::ExportReferenceObj(rig, kind, refNN, err, w.uvMode == 1 ? wimp::UvMode::Texture : wimp::UvMode::Raw, w.uvWidth) : std::string();
            w.message = !text.empty() && wimp::WriteWholeFile(path, text.data(), text.size()) ? "written " + path : "cannot export: " + err;
        }
    }
    ImGui::SetItemTooltip("A vanilla %s as an OBJ in the rig's frame: a model to build on", kind.id);
    if (!w.message.empty()) ImGui::TextWrapped("%s", w.message.c_str());
    if (!w.log.empty()) {
        ImGui::BeginChild("##wilog", ImVec2(0, 170), ImGuiChildFlags_Borders);
        for (const std::string& l : w.log) ImGui::TextWrapped("%s", l.c_str());
        ImGui::EndChild();
    }
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

    WeaponImportPanel(lib, st);

    if (st.dirty) Rebuild(lib, scene, st);
    scene.Pose(st.clip, st.animTime); // every frame: the scene's vertices follow the clip (or go back to rest)
}

} // namespace ui
