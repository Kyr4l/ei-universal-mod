// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Sidebar tabs, and the editing of an object's parameters (fields, unit stats, logic, the kinds' other fields).
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Sidebar tabs
// ------------------------------------------------------------------------------------------------

void SelectObject(App& app, int file, int object, bool focus) {
    if (file >= 0 && file != app.activeMob) SetActiveMob(app, file); // e.g. a finding in another map
    app.scene.Select(file, object);
    if (focus && file >= 0 && file < static_cast<int>(app.mobs.size()) && object >= 0 &&
        object < static_cast<int>(app.mobs[file]->file.objects.size()))
        app.scene.FocusOn(app.mobs[file]->file.objects[object]);
}

static bool IsLoaded(const App& app, const std::string& path) {
    if (app.terrainLoaded && app.terrainPath == path) return true;
    for (auto& m : app.mobs) if (m->file.path == path) return true;
    return false;
}

// The .mpr and .mob files of the map folders set in Settings: a click loads one.
static void FolderList(App& app) {
    if (app.listedMapsVersion != app.lib.mapsVersion) {
        app.folderFiles = app.lib.ListMapFiles();
        app.listedMapsVersion = app.lib.mapsVersion;
    }
    ImGui::SeparatorText("In the map folders");
    if (app.lib.maps.layers.empty()) {
        ImGui::TextDisabled("Add the game's (and a mod's) maps folder in the Settings tab to list their files here.");
        return;
    }
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##folderfilter", "filter, e.g. zone12", app.folderFilter, sizeof(app.folderFilter));
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        for (auto& layer : app.lib.maps.layers) {
            std::string err;
            layer.ok = layer.source.Load(layer.path, err);
            layer.error = err;
        }
        ++app.lib.mapsVersion;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for new or removed files in the folders");
    const std::string filter = Lower(app.folderFilter);
    if (ImGui::BeginTable("##folderfiles", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 190))) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthFixed, 150);
        for (size_t i = 0; i < app.folderFiles.size(); ++i) {
            const Library::MapFile& f = app.folderFiles[i];
            if (!filter.empty() && Lower(f.name).find(filter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            const bool loaded = IsLoaded(app, f.path);
            if (loaded) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.5f, 1));
            if (ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !loaded) {
                if (f.terrain ? LoadTerrain(app, f.path) : AddMob(app, f.path)) SaveSession(app);
            }
            if (loaded) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", f.path.c_str(), loaded ? "(loaded)" : f.terrain ? "Click: use as the terrain" : "Click: load on top of the loaded maps");
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string folder = f.folder;
            size_t cut = folder.find_last_of("/\\", folder.size() > 1 ? folder.size() - 2 : std::string::npos);
            ImGui::TextDisabled("%s", cut == std::string::npos ? folder.c_str() : folder.c_str() + cut + 1);
        }
        ImGui::EndTable();
    }
}

// The quests of the quest folders and language packs set in Settings: a click opens one.
static void QuestList(App& app) {
    RescanQuests(app);
    if (app.lib.questFolders.empty() && app.lib.questPacks.empty()) return;
    ImGui::SeparatorText("Quests");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##questfilter", "filter, e.g. z8q", app.questFilter, sizeof(app.questFilter));
    ImGui::SameLine();
    if (ImGui::Button("Rescan##quests")) app.questsFor.clear();
    const std::string filter = Lower(app.questFilter);
    if (ImGui::BeginTable("##quests", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 150))) {
        ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 110);
        for (size_t i = 0; i < app.quests.size(); ++i) {
            const quest::QuestSet& set = app.quests[i];
            const quest::Quest& q = set.shown;
            if (!filter.empty() && Lower(q.name + " " + q.terrain + " " + q.region).find(filter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            bool open = app.openQuest && app.openQuest->shown.path == q.path;
            std::string label = q.name + (set.languagePack ? "  (" + std::to_string(set.copies.size()) + " languages)" : "");
            if (ImGui::Selectable(label.c_str(), open, ImGuiSelectableFlags_SpanAllColumns)) {
                if (app.questDirty) app.questMessage = "Save or revert the changes of " + app.openQuest->shown.name + " first";
                else OpenQuest(app, set);
            }
            if (ImGui::IsItemHovered()) {
                std::string copies;
                for (size_t k = 0; k < set.copies.size(); ++k) copies += "\n  " + set.labels[k] + ": " + set.copies[k].path;
                ImGui::SetTooltip("%s, %zu exit(s)%s\nClick: open its terrain, base map and quest map", q.packed ? "packed .mq" : "unpacked",
                                  q.exits.size(), copies.c_str());
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", q.terrain.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s%s", q.region.c_str(), q.sky == "cave" ? " (cave)" : "");
        }
        ImGui::EndTable();
    }
}

// The area as numbers; when the user starts changing them, `before` goes on the undo stack.
static bool RectFields(App& app, const char* id, quest::Rect& r, const std::vector<quest::Exit>& before) {
    if (!r.set) return false;
    float v[4] = {r.x1, r.y1, r.x2, r.y2};
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::DragFloat4(id, v, 0.25f, 0.0f, 0.0f, "%.1f");
    if (ImGui::IsItemActivated()) PushAreaUndo(app, before);
    if (!changed) return false;
    r.x1 = v[0]; r.y1 = v[1]; r.x2 = v[2]; r.y2 = v[3];
    return true;
}

// The open quest: its exits, with their deploy and exit areas as numbers (drag or type; the view's
// handles do the same), and saving them to every copy.
static void QuestPanel(App& app) {
    if (!app.openQuest) return;
    quest::QuestSet& set = *app.openQuest;
    quest::Quest& q = set.shown;
    ImGui::SeparatorText(("Quest " + q.name + (app.questDirty ? " (changed)" : "")).c_str());
    ImGui::TextDisabled("%s%s, terrain %s, base map %s, lighting %s", q.region.c_str(), q.sky == "cave" ? " cave" : "", q.terrain.c_str(),
                        q.baseMap.c_str(), q.LightingFile().c_str());
    if (set.languagePack) {
        std::string langs;
        for (const std::string& l : set.labels) langs += (langs.empty() ? "" : ", ") + l;
        ImGui::TextDisabled("language packs: %s (area changes go to all of them)", langs.c_str());
    }
    const std::vector<quest::Exit> before = q.exits; // for the undo stack, when a field starts changing
    for (size_t i = 0; i < q.exits.size(); ++i) {
        quest::Exit& e = q.exits[i];
        ImGui::PushID(static_cast<int>(i));
        std::string label = "#" + std::to_string(e.number) + " " + (e.title.empty() ? std::string("exit") : e.title) + "  -> " + e.target;
        if (ImGui::Selectable(label.c_str())) FocusRect(app, e.deploy.set ? e.deploy : e.remove);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: go there");
        if (e.deploy.set) {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "deploy");
            ImGui::SameLine(70);
            if (RectFields(app, "##deploy", e.deploy, before)) app.questDirty = !SameAreas(q.exits, app.savedAreas);
        }
        if (e.remove.set) {
            ImGui::TextColored(ImVec4(0.98f, 0.5f, 0.42f, 1), "exit");
            ImGui::SameLine(70);
            if (RectFields(app, "##remove", e.remove, before)) app.questDirty = !SameAreas(q.exits, app.savedAreas);
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(!app.questDirty);
    if (ImGui::Button(set.copies.size() > 1 ? ("Save areas to all " + std::to_string(set.copies.size()) + " copies").c_str() : "Save areas"))
        SaveQuestAreas(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.undoSteps.empty());
    if (ImGui::Button("Undo")) UndoRedo(app, false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.redoSteps.empty());
    if (ImGui::Button("Redo")) UndoRedo(app, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", ui::BindName(app.lib.mapKeys[config::kKeyRedo]).c_str());
    ImGui::BeginDisabled(!app.questDirty);
    ImGui::SameLine();
    if (ImGui::Button("Revert")) { ReloadQuest(app, false); app.questMessage = "Reverted to the files on disk"; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Close quest")) {
        if (app.questDirty) app.questMessage = "Save or revert the changes first";
        else CloseQuest(app);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop showing its areas (the files stay loaded)");
    if (!app.questMessage.empty()) ImGui::TextWrapped("%s", app.questMessage.c_str());
    ImGui::TextDisabled("In the view: drag an area's corner, side or centre handle to resize or move it (Shift: no snapping). "
                        "Save with the button above, the toolbar's Save quest or %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
}

void FilesTab(App& app) {
    QuestList(app);
    QuestPanel(app);
    FolderList(app);
    if (!app.missing.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1), "%zu object(s) whose figure cannot be shown", app.missing.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(app.missingOpen ? "Hide##missing" : "Show##missing")) app.missingOpen = !app.missingOpen;
        ImGui::SetItemTooltip("Their figure is not in the figure sources (Settings) or cannot be read: the game would not show them either\n"
                              "if it lacks them too. Marked with a red ? in the view.");
    }
    ImGui::SeparatorText("Terrain (.mpr)");
    if (app.terrainLoaded) {
        const mpr::Map& m = app.terrain;
        ui::StatusDot(true, app.terrainPath, "");
        ImGui::SameLine();
        ImGui::TextUnformatted(std::filesystem::path(app.terrainPath).filename().string().c_str()); // the folder in the tooltip: long paths wrapped over three lines
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app.terrainPath.c_str());
        ImGui::TextDisabled("%s: %d x %d sectors (%.0f x %.0f), max height %.1f, %d/%d textures found", m.name.c_str(), m.sectorsX,
                            m.sectorsY, m.Width(), m.Height(), m.maxZ, app.scene.TerrainTexturesFound(), m.textureCount);
        if (app.scene.TerrainTexturesFound() < m.textureCount)
            ui::Note("Terrain textures " + m.name + "000.mmp... are missing from the texture sources (Settings): add the textures.res that "
                     "has them (a mod's may be textures-zones.res).");
        for (const std::string& w : m.warnings) ui::Note(w);
        if (ImGui::SmallButton("Unload##mpr")) RequestUnload(app, app.terrainPath);
    } else {
        ImGui::TextDisabled("(none: objects stand at height 0)");
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mpr", "any other .mpr: its path", app.terrainInput, sizeof(app.terrainInput));
    std::string picked;
    if (ImGui::Button("File...##mpr") && ui::PickFile(picked)) std::snprintf(app.terrainInput, sizeof(app.terrainInput), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.terrainInput[0] == '\0');
    if (ImGui::Button("Load##mpr") && LoadTerrain(app, app.terrainInput)) {
        app.terrainInput[0] = '\0';
        app.filesMessage.clear();
        SaveSession(app);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Maps (.mob), in load order");
    ImGui::TextDisabled("Each map is loaded on top of the ones above it, like a quest on its zone.");
    int removeAt = -1, moveUp = -1;
    for (size_t i = 0; i < app.mobs.size(); ++i) {
        MobEntry& e = *app.mobs[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Checkbox("##vis", &e.visible)) SyncScene(app);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show its objects");
        ImGui::SameLine();
        if (ImGui::RadioButton("##active", app.activeMob == static_cast<int>(i))) SetActiveMob(app, static_cast<int>(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The active map: the only one whose objects can be selected (%s: next)",
                                                      ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
        ImGui::SameLine();
        ui::StatusDot(e.file.loaded, e.file.path, e.file.error);
        ImGui::SameLine();
        if (e.Dirty()) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "%s *", e.file.fileName.c_str());
        else ImGui::TextUnformatted(e.file.fileName.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", e.file.path.c_str(), e.Dirty() ? "\n(unsaved changes)" : "");
        ImGui::SameLine();
        float right = ImGui::GetContentRegionMax().x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - 88.0f));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up")) moveUp = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
        if (e.file.loaded) {
            int counts[8] = {};
            for (const mob::Object& o : e.file.objects) counts[static_cast<int>(o.kind)]++;
            ImGui::TextDisabled("   %d objects, %d units, %d levers, %d torches, %d traps, %d lights, %d particles, %d sounds%s",
                                counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
                                e.file.hasScript ? ", script" : "");
        }
        ImGui::PopID();
    }
    if (moveUp > 0) {
        std::swap(app.mobs[moveUp], app.mobs[moveUp - 1]);
        if (app.activeMob == moveUp) app.activeMob = moveUp - 1;
        else if (app.activeMob == moveUp - 1) app.activeMob = moveUp;
        SyncScene(app);
        SaveSession(app);
    }
    if (removeAt >= 0) RequestUnload(app, app.mobs[removeAt]->file.path);
    if (app.mobs.empty()) ImGui::TextDisabled("(none yet)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mob", "any other .mob: its path", app.mobInput, sizeof(app.mobInput));
    if (ImGui::Button("File...##mob") && ui::PickFile(picked)) std::snprintf(app.mobInput, sizeof(app.mobInput), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.mobInput[0] == '\0');
    if (ImGui::Button("Add##mob") && AddMob(app, app.mobInput)) {
        app.mobInput[0] = '\0';
        SaveSession(app);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.mobs.empty() && !app.terrainLoaded);
    if (ImGui::Button("Reload all") && !BlockedByUnsaved(app)) {
        std::vector<std::string> paths;
        if (app.terrainLoaded) paths.push_back(app.terrainPath);
        for (auto& m : app.mobs) paths.push_back(m->file.path);
        app.mobs.clear();
        app.loadOrder.clear();
        app.terrainLoaded = false;
        app.scene.ClearSelection();
        OpenPaths(app, paths);
        SyncScene(app);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read every file again, after changing them elsewhere");

    QuestSuggestion q = SuggestForQuest(app);
    if (q.any) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s.", q.label.c_str());
        if (ImGui::Button(q.baseMob.empty() ? "Load its terrain" : q.terrain.empty() ? "Load its base map" : "Load its base map and terrain")) {
            if (!q.terrain.empty()) LoadTerrain(app, q.terrain);
            if (!q.baseMob.empty()) {
                AddMob(app, q.baseMob);
                std::rotate(app.mobs.begin(), app.mobs.end() - 1, app.mobs.end()); // the base goes first
                SyncScene(app);
            }
            SaveSession(app);
        }
    }
    if (!app.filesMessage.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", app.filesMessage.c_str());
    }
}

// ---- editing an object's parameters --------------------------------------------------------------
// The fields keep what is typed while they are being edited and commit when left (Enter, Tab or a
// click elsewhere). A commit is one undo step (the file as it was) and goes to every selected object
// that has the field when "apply to all" is on. Texts are stored in CP1251, the game's encoding.

static int ResizeCallbackStr(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = static_cast<std::string*>(d->UserData);
        str->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

static bool EditText(const char* id, const std::string& current, std::string& out) {
    static std::map<ImGuiID, std::string> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::string v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(id, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallbackStr, &v);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) { out = v; pending.erase(key); return out != current; }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

// The names the object fields complete from (rebuilt when the sources change).
struct NameLists {
    unsigned figuresFor = ~0u, texturesFor = ~0u, dbFor = ~0u;
    std::vector<std::string> figures, textures, prototypes, items, materials;
};
static NameLists& Names(const Library& lib) {
    static NameLists n;
    auto sorted = [](std::vector<std::string> v) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); return v; };
    if (n.figuresFor != static_cast<unsigned>(lib.figuresVersion)) {
        n.figuresFor = static_cast<unsigned>(lib.figuresVersion);
        std::vector<std::string> f;
        for (const std::string& b : lib.figures.ListBaseNames({".mod", ".lnk"})) f.push_back(Lower(b)); // whole figures, not parts
        n.figures = sorted(f);
    }
    if (n.texturesFor != static_cast<unsigned>(lib.texturesVersion)) {
        n.texturesFor = static_cast<unsigned>(lib.texturesVersion);
        std::vector<std::string> t;
        for (const std::string& b : lib.textures.ListBaseNames({".mmp", ".dds"})) t.push_back(Lower(b));
        n.textures = sorted(t);
    }
    if (n.dbFor != static_cast<unsigned>(lib.version)) {
        n.dbFor = static_cast<unsigned>(lib.version);
        n.prototypes.clear(); n.items.clear(); n.materials.clear();
        for (const units::Monster& m : lib.unitsDb.monsters) n.prototypes.push_back(m.name);
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c)
            for (const items::Item& it : lib.db.List(static_cast<items::Category>(c))) n.items.push_back(it.name);
        for (const items::Material& m : lib.db.materials) n.materials.push_back(m.name);
        n.prototypes = sorted(n.prototypes); n.items = sorted(n.items); n.materials = sorted(n.materials);
    }
    return n;
}

// EditText with suggestions while typing: names starting with the text first, then containing it. Up / Down
// pick one, Tab (or Enter on a picked one) or a click takes it; Escape or typing on ignores them. `prefix`: kept
// before the completed part (e.g. "axe." while completing the material of "axe.bro").
static bool EditTextSuggest(const char* id, const std::string& current, std::string& out, const std::vector<std::string>& names,
                            const std::string& prefix = "", float width = -1.0f) {
    static std::map<ImGuiID, std::string> pending;
    static ImGuiID openFor = 0;
    static int highlight = -1;
    static ImVec2 boxMin, boxMax; // last frame's suggestion box
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::string v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallbackStr, &v);
    const ImVec2 fieldMin = ImGui::GetItemRectMin(), fieldMax = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    // The suggestions for what is typed (after the prefix).
    std::vector<const std::string*> found;
    const std::string typed = v.size() >= prefix.size() ? Lower(v.substr(prefix.size())) : "";
    if ((openFor == key || active) && !typed.empty()) {
        for (int pass = 0; pass < 2 && found.size() < 12; ++pass)
            for (const std::string& n : names) {
                const std::string ln = Lower(n);
                const size_t at = ln.find(typed);
                if (at == std::string::npos || (pass == 0) != (at == 0) || ln == typed) continue;
                found.push_back(&n);
                if (found.size() >= 12) break;
            }
    }
    if (active) {
        pending[key] = v;
        if (openFor != key) { openFor = key; highlight = -1; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !found.empty()) highlight = std::min(highlight + 1, static_cast<int>(found.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) highlight = std::max(highlight - 1, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { openFor = 0; found.clear(); }
    }
    bool picked = false;
    std::string choice;
    if (openFor == key && !found.empty() && !typed.empty()) { // the box under the field
        ImGui::SetNextWindowPos(ImVec2(fieldMin.x, fieldMax.y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(fieldMax.x - fieldMin.x, 0), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("##suggest", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        for (int i = 0; i < static_cast<int>(found.size()); ++i)
            if (ImGui::Selectable(found[i]->c_str(), i == highlight)) { picked = true; choice = *found[i]; }
        boxMin = ImGui::GetWindowPos();
        boxMax = ImVec2(boxMin.x + ImGui::GetWindowWidth(), boxMin.y + ImGui::GetWindowHeight());
        ImGui::End();
    } else if (openFor == key) {
        boxMin = boxMax = ImVec2(0, 0);
    }
    if (picked) { openFor = 0; pending.erase(key); out = prefix + choice; return out != current; }
    // The box left open by a click that was released elsewhere: the typed text is taken.
    if (!active && openFor == key && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(boxMin, boxMax, false)) {
        openFor = 0; pending.erase(key); out = v; return out != current;
    }
    if (ImGui::IsItemDeactivated()) {
        // A click in the box: the box takes it (next frame), not the half-typed text.
        if (ImGui::IsMouseHoveringRect(boxMin, boxMax, false) && openFor == key) return false;
        const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false), enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        openFor = 0;
        if (!found.empty() && (tab || (enter && highlight >= 0))) v = prefix + *found[highlight >= 0 ? highlight : 0];
        pending.erase(key);
        out = v;
        return out != current;
    }
    if (!active && openFor != key) pending.erase(key);
    return false;
}

// A texture field: suggestions while typing, and a button opening the full list, each texture previewed on hover.
static bool EditTexture(App& app, const char* id, const std::string& current, std::string& out) {
    const std::vector<std::string>& names = Names(app.lib).textures;
    ImGui::PushID(id);
    const float button = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    bool changed = EditTextSuggest("##t", current, out, names, "", ImGui::GetContentRegionAvail().x - button - gap);
    ImGui::SameLine(0, gap);
    if (ImGui::ArrowButton("##list", ImGuiDir_Down)) ImGui::OpenPopup("##textures");
    ImGui::SetItemTooltip("Pick a texture (previewed when hovered)");
    if (ImGui::BeginPopup("##textures")) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##filter", "search", filter, sizeof filter);
        const std::string f = Lower(filter);
        ImGui::BeginChild("##list", ImVec2(300, 320));
        for (const std::string& n : names) {
            if (!f.empty() && n.find(f) == std::string::npos) continue;
            if (ImGui::Selectable(n.c_str(), Lower(current) == n)) { out = n; changed = out != current; ImGui::CloseCurrentPopup(); }
            if (ImGui::IsItemHovered()) {
                const GLuint tex = app.scene.TexturePreview(app.lib, n);
                if (tex && ImGui::BeginTooltip()) {
                    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(160, 160));
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool EditFloats(const char* id, int n, const float* current, float* out, const char* format, float width) {
    static std::map<ImGuiID, std::array<float, 4>> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    std::array<float, 4> v{};
    for (int i = 0; i < n; ++i) v[i] = it != pending.end() ? it->second[i] : current[i];
    ImGui::SetNextItemWidth(width);
    ImGui::InputScalarN(id, ImGuiDataType_Float, v.data(), n, nullptr, nullptr, format);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        pending.erase(key);
        bool changed = false;
        for (int i = 0; i < n; ++i) { changed |= out[i] != v[i] || current[i] != v[i]; out[i] = v[i]; }
        return changed;
    }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

static bool EditU32(const char* id, uint32_t current, uint32_t& out) {
    static std::map<ImGuiID, uint32_t> pending;
    const ImGuiID key = ImGui::GetID(id);
    auto it = pending.find(key);
    uint32_t v = it != pending.end() ? it->second : current;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputScalar(id, ImGuiDataType_U32, &v);
    if (ImGui::IsItemActive()) pending[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) { pending.erase(key); out = v; return v != current; }
    if (!ImGui::IsItemActive()) pending.erase(key);
    return false;
}

static std::vector<uint8_t> F32Payload(const float* v, int n) {
    std::vector<uint8_t> p(static_cast<size_t>(n) * 4);
    std::memcpy(p.data(), v, p.size());
    return p;
}

// Writes one field into the shown object, or every selected object that has it. One undo step.
static void CommitField(App& app, int shown, uint32_t type, const std::vector<uint8_t>& payload, bool allowAll = true) {
    if (app.mobs.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::vector<int> targets;
    if (allowAll && app.editAll && app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1) targets = app.scene.selection;
    else targets = {shown};
    int done = 0;
    for (int oi : targets) done += mob::ReplaceField(f, oi, type, payload) ? 1 : 0;
    if (!done) { app.editMessage = "No selected object has that field"; return; }
    PushUndo(app, std::move(step), "Edit " + checks::Label(f.objects[shown]));
    app.checksDirty = true;
    app.listedKey.clear();
    app.editMessage = done > 1 ? "Changed " + std::to_string(done) + " objects" : std::string();
}

static void CommitText(App& app, int shown, uint32_t type, const std::string& utf8, bool allowAll = true) {
    std::vector<uint8_t> cp;
    std::string err;
    if (!codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err)) { app.editMessage = "Not saved: the text has " + err; return; }
    CommitField(app, shown, type, cp, allowAll);
}

std::string ToCp(const std::string& utf8) {
    std::vector<uint8_t> cp;
    std::string err;
    codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err);
    return std::string(cp.begin(), cp.end());
}

void Label(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableNextColumn();
}

// ---- unit stats (UNIT_STATS, ei_maper's SUnitStat) ----------------------------------------------------

struct StatField { const char* group; const char* name; int offset; char type; const char* tip; }; // type: i int, f float, b byte

static const StatField kStatFields[] = {
    {"Health and mana", "HP", 0, 'i', nullptr}, {"Health and mana", "Max HP", 4, 'i', nullptr},
    {"Health and mana", "MP", 8, 'i', nullptr}, {"Health and mana", "Max MP", 12, 'i', nullptr},
    {"Movement", "Move", 16, 'f', nullptr}, {"Movement", "Actions", 20, 'f', nullptr},
    {"Movement", "Run speed", 24, 'f', nullptr}, {"Movement", "Walk speed", 28, 'f', nullptr},
    {"Movement", "Crouch speed", 32, 'f', nullptr}, {"Movement", "Crawl speed", 36, 'f', nullptr},
    {"Vision", "Vision arc", 40, 'f', "radians"}, {"Vision", "Peripheral skill", 44, 'f', nullptr},
    {"Vision", "Peripheral arc", 48, 'f', "radians"},
    {"Combat", "Attack distance", 52, 'f', nullptr}, {"Combat", "AI class (standing)", 56, 'b', nullptr},
    {"Combat", "AI class (lying)", 57, 'b', nullptr}, {"Combat", "Range", 60, 'f', nullptr},
    {"Combat", "Attack", 64, 'f', nullptr}, {"Combat", "Defence", 68, 'f', nullptr}, {"Combat", "Weight", 72, 'f', nullptr},
    {"Combat", "Damage min", 76, 'f', nullptr}, {"Combat", "Damage range", 80, 'f', "added to the minimum at random"},
    {"Armour", "Impaling", 84, 'f', nullptr}, {"Armour", "Slashing", 88, 'f', nullptr}, {"Armour", "Crushing", 92, 'f', nullptr},
    {"Armour", "Thermal", 96, 'f', nullptr}, {"Armour", "Chemical", 100, 'f', nullptr}, {"Armour", "Electrical", 104, 'f', nullptr},
    {"Armour", "General", 108, 'f', nullptr}, {"Armour", "Absorption", 112, 'i', nullptr},
    {"Senses", "Sight", 116, 'f', nullptr}, {"Senses", "Night sight", 120, 'f', nullptr}, {"Senses", "Sense life", 124, 'f', nullptr},
    {"Senses", "Hearing", 128, 'f', nullptr}, {"Senses", "Smell", 132, 'f', nullptr}, {"Senses", "Tracking", 136, 'f', nullptr},
    {"Senses", "Sight (p)", 140, 'f', "the p values of ei_maper"}, {"Senses", "Night sight (p)", 144, 'f', nullptr},
    {"Senses", "Sense life (p)", 148, 'f', nullptr}, {"Senses", "Hearing (p)", 152, 'f', nullptr},
    {"Senses", "Smell (p)", 156, 'f', nullptr}, {"Senses", "Tracking (p)", 160, 'f', nullptr},
    {"Skills", "Science", 164, 'b', nullptr}, {"Skills", "Stealing", 165, 'b', nullptr}, {"Skills", "Taming", 166, 'b', nullptr},
    {"Skills", "Magic school 1", 167, 'b', nullptr}, {"Skills", "Magic school 2", 168, 'b', nullptr}, {"Skills", "Magic school 3", 169, 'b', nullptr},
};

// Writes one stat into the shown unit, or into every selected unit (each keeps its other stats). One undo step.
static void CommitStat(App& app, int shown, const StatField& sf, const uint8_t* value) {
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::vector<int> targets;
    if (app.editAll && app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1) targets = app.scene.selection;
    else targets = {shown};
    const size_t size = sf.type == 'b' ? 1 : 4;
    int done = 0;
    for (int oi : targets) {
        std::vector<uint8_t> stats = f.objects[oi].stats;
        if (f.objects[oi].kind != mob::Kind::Unit || stats.size() < 172) continue;
        std::memcpy(stats.data() + sf.offset, value, size);
        done += mob::ReplaceField(f, oi, mob::kUnitStats, stats) ? 1 : 0;
    }
    if (!done) return;
    PushUndo(app, std::move(step), std::string("Stat ") + sf.name);
    app.listedKey.clear();
    app.editMessage = done > 1 ? "Changed " + std::to_string(done) + " units" : std::string();
}

static void StatsEditor(App& app, const mob::Object& o, int objectIndex) {
    if (o.stats.size() < 172) return;
    if (!ImGui::CollapsingHeader("Stats")) return;
    if (o.needImport) ImGui::TextWrapped("\"Stats imported\" is on: the game takes this unit's stats from its prototype, not these.");
    const char* group = nullptr;
    bool open = false;
    for (const StatField& sf : kStatFields) {
        if (!group || std::strcmp(group, sf.group) != 0) {
            if (open) ImGui::EndTable();
            group = sf.group;
            ImGui::SeparatorText(group);
            open = ImGui::BeginTable(group, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg);
            if (open) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            }
        }
        if (!open) continue;
        Label(sf.name);
        if (sf.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sf.tip);
        ImGui::PushID(sf.offset);
        const uint8_t* at = o.stats.data() + sf.offset;
        if (sf.type == 'f') {
            float cur[1], v[1];
            std::memcpy(cur, at, 4);
            if (EditFloats("##v", 1, cur, v, "%.4g")) CommitStat(app, objectIndex, sf, reinterpret_cast<const uint8_t*>(v));
        } else {
            uint32_t cur = 0, v = 0;
            if (sf.type == 'i') std::memcpy(&cur, at, 4);
            else cur = at[0];
            if (EditU32("##v", cur, v)) {
                if (sf.type == 'b') { const uint8_t b = static_cast<uint8_t>(std::min<uint32_t>(v, 255)); CommitStat(app, objectIndex, sf, &b); }
                else CommitStat(app, objectIndex, sf, reinterpret_cast<const uint8_t*>(&v));
            }
        }
        ImGui::PopID();
    }
    if (open) ImGui::EndTable();
}

// Body parts: the figure's parts to show (OBJ_BODYPARTS); none ticked shows them all.
static void BodyPartsEditor(App& app, const mob::Object& o, int objectIndex) {
    const std::vector<std::string>* names = app.scene.PartNames(o.templ);
    if (!names && o.bodyParts.empty()) return;
    const std::string title = "Body parts (" + (o.bodyParts.empty() ? std::string("all") : std::to_string(o.bodyParts.size())) + ")###bodyparts";
    if (!ImGui::CollapsingHeader(title.c_str())) return;
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::vector<std::string> all = names ? *names : std::vector<std::string>{};
    for (const std::string& p : o.bodyParts) { // parts the figure lacks stay listed, so they can be removed
        bool found = false;
        for (const std::string& n : all) found |= lower(n) == lower(p);
        if (!found) all.push_back(p);
    }
    ImGui::TextDisabled("None ticked: the whole figure shows.");
    std::vector<std::string> now = o.bodyParts;
    bool changed = false;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 125.0f));
    if (ImGui::BeginTable("##parts", columns)) {
        for (size_t i = 0; i < all.size(); ++i) {
            ImGui::TableNextColumn();
            auto it = std::find_if(now.begin(), now.end(), [&](const std::string& p) { return lower(p) == lower(all[i]); });
            bool on = it != now.end();
            if (ImGui::Checkbox((all[i] + "##bp" + std::to_string(i)).c_str(), &on)) {
                if (on) now.push_back(all[i]);
                else now.erase(it);
                changed = true;
            }
        }
        ImGui::EndTable();
    }
    if (!now.empty() && ImGui::SmallButton("Show all (clear)")) { now.clear(); changed = true; }
    if (changed) CommitField(app, objectIndex, mob::kObjBodyParts, mob::StringArrayPayload(mob::kObjBodyParts, now), false);
}

// ---- logic (UNIT_LOGIC) -----------------------------------------------------------------------------

void CommitLogic(App& app, int objectIndex, int logicIndex, const mob::Logic& g) {
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    if (!mob::SetLogic(f, objectIndex, logicIndex, g)) { app.editMessage = "This unit's logic could not be written"; return; }
    PushUndo(app, std::move(step), "Logic of " + checks::Label(f.objects[objectIndex]));
    app.checksDirty = true;
    app.listedKey.clear();
}

// The record shown for this unit: the one picked before, else the first in use.
static int LogicRecord(App& app, const mob::Object& o, int objectIndex) {
    if (app.logicRecordFor != objectIndex || app.logicRecord < 0 || app.logicRecord >= static_cast<int>(o.logics.size())) {
        app.logicRecordFor = objectIndex;
        app.logicRecord = 0;
        for (size_t i = 0; i < o.logics.size(); ++i) if (o.logics[i].use) { app.logicRecord = static_cast<int>(i); break; }
    }
    return app.logicRecord;
}

static bool EditByte(const char* id, int current, int& out) {
    uint32_t v = 0;
    if (!EditU32(id, static_cast<uint32_t>(std::max(current, 0)), v)) return false;
    out = static_cast<int>(std::min<uint32_t>(v, 255));
    return true;
}

static void LogicEditor(App& app, const mob::Object& o, int objectIndex) {
    if (o.logics.empty()) return;
    const int gi = LogicRecord(app, o, objectIndex);
    mob::Logic g = o.logics[gi];
    bool changed = false;
    ImGui::SeparatorText("Logic");
    if (o.logics.size() > 1) {
        std::string preview = "Record " + std::to_string(gi + 1) + (g.use ? " (in use)" : "");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##record", preview.c_str())) {
            for (size_t i = 0; i < o.logics.size(); ++i) {
                std::string label = "Record " + std::to_string(i + 1) + ": " + mob::LogicModelName(o.logics[i].model) + (o.logics[i].use ? " (in use)" : "");
                if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == gi)) app.logicRecord = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A unit carries several logic records; the ones in use apply");
    }
    float v4[4];
    if (ImGui::BeginTable("##logic", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        Label("In use");
        changed |= ImGui::Checkbox("##use", &g.use);
        Label("Behaviour");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##model", mob::LogicModelName(g.model))) {
            for (int m = 0; m <= 5; ++m)
                if (ImGui::Selectable(mob::LogicModelName(m), m == g.model)) { g.model = m; changed = true; }
            ImGui::EndCombo();
        }
        if (g.model == 1) {
            Label("Guard radius");
            const float cur[1] = {g.guardRadius};
            if (EditFloats("##gradius", 1, cur, v4, "%.2f")) { g.guardRadius = v4[0]; changed = true; }
        }
        if (g.model == 1 || g.model == 3) {
            Label(g.model == 1 ? "Guard place" : "Sentry place");
            const float cur[3] = {g.guardPlace.x, g.guardPlace.y, g.guardPlace.z};
            if (EditFloats("##gplace", 3, cur, v4, "%.3f", -50)) { g.guardPlace = {v4[0], v4[1], v4[2]}; changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Here")) { g.guardPlace = {o.position.x, o.position.y, 0.0f}; changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The unit's own position (or drag the point in logic mode)");
        }
        if (g.model == 2) {
            Label("Cyclic");
            changed |= ImGui::Checkbox("##cyclic", &g.cyclic);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the first point after the last");
        }
        Label("Calls help within");
        {
            const float cur[1] = {g.help};
            if (EditFloats("##help", 1, cur, v4, "%.1f")) { g.help = v4[0]; changed = true; }
        }
        Label("Wait (s)");
        {
            const float cur[1] = {g.wait < 0 ? -1.0f : g.wait / 15.0f};
            if (EditFloats("##wait", 1, cur, v4, "%.2f")) { g.wait = v4[0] < 0 ? -1.0f : v4[0] * 15.0f; changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stored as 15 per second; -1: none");
        }
        Label("Alarm condition");
        changed |= EditByte("##alarmcond", g.alarmCondition, g.alarmCondition);
        Label("Alarm count");
        changed |= EditByte("##alarmcount", g.alarmCount, g.alarmCount);
        Label("Aggression");
        changed |= EditByte("##aggr", g.aggression, g.aggression);
        Label("Always active");
        {
            bool on = g.alwaysActive != 0;
            if (ImGui::Checkbox("##always", &on)) { g.alwaysActive = on ? 1 : 0; changed = true; }
        }
        ImGui::EndTable();
    }
    // Patrol points, each with its look points (where the unit turns to look, how long, how fast).
    if (g.model == 2 || !g.patrol.empty()) {
        ImGui::TextDisabled("Patrol points (logic mode: drag them; Ctrl+click the ground adds one)");
        int removePoint = -1;
        for (size_t i = 0; i < g.patrol.size(); ++i) {
            mob::PatrolPoint& pt = g.patrol[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu", i + 1);
            ImGui::SameLine(30);
            const float cur[3] = {pt.position.x, pt.position.y, pt.position.z};
            if (EditFloats("##pp", 3, cur, v4, "%.3f", -80)) { pt.position = {v4[0], v4[1], v4[2]}; changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ look")) {
                pt.looks.push_back({{pt.position.x + 2.0f, pt.position.y, pt.position.z}, 15, 0.0f, 0});
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removePoint = static_cast<int>(i);
            int removeLook = -1;
            for (size_t k = 0; k < pt.looks.size(); ++k) {
                mob::LookPoint& l = pt.looks[k];
                ImGui::PushID(static_cast<int>(k));
                ImGui::Indent(30);
                ImGui::TextDisabled("look %zu", k + 1);
                ImGui::SameLine(110);
                const float lc[3] = {l.position.x, l.position.y, l.position.z};
                if (EditFloats("##lp", 3, lc, v4, "%.3f", -30)) { l.position = {v4[0], v4[1], v4[2]}; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeLook = static_cast<int>(k);
                ImGui::TextDisabled("wait s, turn speed");
                ImGui::SameLine(160);
                {
                    const float wc[2] = {l.wait / 15.0f, l.turnSpeed};
                    ImGui::PushItemWidth(-1);
                    if (EditFloats("##wt", 2, wc, v4, "%.2f")) { l.wait = static_cast<uint32_t>(std::max(0.0f, v4[0]) * 15.0f + 0.5f); l.turnSpeed = v4[1]; changed = true; }
                    ImGui::PopItemWidth();
                }
                ImGui::Unindent(30);
                ImGui::PopID();
            }
            if (removeLook >= 0) { pt.looks.erase(pt.looks.begin() + removeLook); changed = true; }
            ImGui::PopID();
        }
        if (removePoint >= 0) { g.patrol.erase(g.patrol.begin() + removePoint); changed = true; }
        if (ImGui::SmallButton("+ point")) {
            mob::PatrolPoint pt;
            const mob::Vec3 from = g.patrol.empty() ? o.position : g.patrol.back().position;
            pt.position = {from.x + 3.0f, from.y, app.scene.Ground(from.x + 3.0f, from.y)};
            g.patrol.push_back(pt);
            changed = true;
        }
    }
    if (changed) CommitLogic(app, objectIndex, gi, g);
}

// ---- the other fields of each kind (as ei_maper shows them) ---------------------------------------------

// A field's payload as stored in the object (empty when it has none).
static std::vector<uint8_t> FieldBytes(const mob::File& f, int objectIndex, uint32_t type) {
    const size_t at = mob::FindField(f, objectIndex, type);
    if (!at) return {};
    const uint32_t l = mob::U32(f.bytes.data() + at + 4);
    return std::vector<uint8_t>(f.bytes.begin() + at + 8, f.bytes.begin() + at + l);
}

// One row per field the object has: a box for flags, a number, a float, a point or a text.
// kind: 'b' flag byte, 'B' number byte, 'u' dword, 'f' float, 'p' three floats, 's' text.
static void FieldRow(App& app, int objectIndex, const char* label, uint32_t type, char kind, const char* tip = nullptr) {
    const mob::File& f = app.mobs[app.activeMob]->file;
    const std::vector<uint8_t> v = FieldBytes(f, objectIndex, type);
    const size_t need = kind == 'b' || kind == 'B' ? 1 : kind == 'u' || kind == 'f' ? 4 : kind == 'p' ? 12 : 0;
    if ((need && v.size() != need) || (kind == 's' && mob::FindField(f, objectIndex, type) == 0)) return;
    Label(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PushID(static_cast<int>(type));
    float fv[4];
    uint32_t u = 0;
    std::string text;
    switch (kind) {
    case 'b': {
        bool on = v[0] != 0;
        if (ImGui::Checkbox("##v", &on)) CommitField(app, objectIndex, type, std::vector<uint8_t>{static_cast<uint8_t>(on)});
        break;
    }
    case 'B':
        if (EditU32("##v", v[0], u)) CommitField(app, objectIndex, type, std::vector<uint8_t>{static_cast<uint8_t>(std::min<uint32_t>(u, 255))});
        break;
    case 'u':
        if (EditU32("##v", mob::U32(v.data()), u)) CommitField(app, objectIndex, type, mob::U32Payload(u));
        break;
    case 'f': {
        float cur[1];
        std::memcpy(cur, v.data(), 4);
        if (EditFloats("##v", 1, cur, fv, "%.3f")) CommitField(app, objectIndex, type, F32Payload(fv, 1));
        break;
    }
    case 'p': {
        float cur[3];
        std::memcpy(cur, v.data(), 12);
        if (EditFloats("##v", 3, cur, fv)) CommitField(app, objectIndex, type, F32Payload(fv, 3));
        break;
    }
    default:
        if (EditText("##v", mob::Utf8(std::string(v.begin(), v.end())), text)) CommitText(app, objectIndex, type, text);
        break;
    }
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
}

enum : uint32_t {
    kObjIsShadow = 45076,
    kLeverCurState = 3148611586u, kLeverTotalState = 3148611587u, kLeverIsCycled = 3148611588u, kLeverCastOnce = 3148611589u,
    kLeverStats = 3148611590u, kLeverIsDoor = 3148611591u, kLeverRecalcGraph = 3148611592u,
    kTrapDiplomacy = 3148546049u, kTrapAreas = 3148546051u, kTrapTargets = 3148546052u, kTrapCastInterval = 3148546053u,
    kTorchStrength = 3149856769u, kTorchPoint = 3149856770u, kTorchSound = 3149856771u,
    kLightShadow = 43526,
    kSoundMin = 52230, kSoundMax = 52231, kSoundRange2 = 52235, kSoundAmbient = 52237, kSoundMusic = 52238,
};

// The rows of the details table for what each kind stores beyond the common fields.
static void KindFieldRows(App& app, const mob::Object& o, int objectIndex) {
    if (mob::HasFigure(o.kind)) {
        FieldRow(app, objectIndex, "Parent ID", mob::kObjParentId, 'u', "The object this one belongs to (0: none)");
        FieldRow(app, objectIndex, "Used in script", mob::kObjUseInScript, 'b');
        FieldRow(app, objectIndex, "Shadow", kObjIsShadow, 'b');
    }
    const mob::File& f = app.mobs[app.activeMob]->file;
    switch (o.kind) {
    case mob::Kind::Lever: {
        FieldRow(app, objectIndex, "State", kLeverCurState, 'B');
        FieldRow(app, objectIndex, "Number of states", kLeverTotalState, 'B');
        FieldRow(app, objectIndex, "Cycled", kLeverIsCycled, 'b');
        FieldRow(app, objectIndex, "Door", kLeverIsDoor, 'b');
        FieldRow(app, objectIndex, "Recalculate graph", kLeverRecalcGraph, 'b', "Paths are worked out again when it changes (doors, gates)");
        // LEVER_SCIENCE_STATS_NEW: how it opens, the key, the sleight of hand needed
        std::vector<uint8_t> st = FieldBytes(f, objectIndex, kLeverStats);
        if (st.size() == 12) {
            uint32_t v[3];
            std::memcpy(v, st.data(), 12);
            static const uint32_t types[] = {0, 1, 5, 8};
            static const char* const names[] = {"0: disabled", "1: enabled", "5: needs sleight of hand", "8: needs a key"};
            int cur = -1;
            for (int i = 0; i < 4; ++i) if (types[i] == v[0]) cur = i;
            Label("Opening");
            ImGui::SetNextItemWidth(-1);
            const std::string preview = cur >= 0 ? names[cur] : std::to_string(v[0]);
            if (ImGui::BeginCombo("##open", preview.c_str())) {
                for (int i = 0; i < 4; ++i)
                    if (ImGui::Selectable(names[i], i == cur)) { v[0] = types[i]; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
                ImGui::EndCombo();
            }
            uint32_t u = 0;
            Label("Key ID");
            if (EditU32("##key", v[1], u)) { v[1] = u; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
            Label("Sleight of hand");
            if (EditU32("##sleight", v[2], u)) { v[2] = u; CommitField(app, objectIndex, kLeverStats, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12)); }
        }
        break;
    }
    case mob::Kind::MagicTrap:
        FieldRow(app, objectIndex, "Diplomacy group", kTrapDiplomacy, 'u', "The player group the trap belongs to");
        FieldRow(app, objectIndex, "Cast interval", kTrapCastInterval, 'u');
        FieldRow(app, objectIndex, "Cast once", kLeverCastOnce, 'b');
        break;
    case mob::Kind::Torch:
        FieldRow(app, objectIndex, "Strength", kTorchStrength, 'f');
        FieldRow(app, objectIndex, "Point link", kTorchPoint, 'p', "Where the flame is (TORCH_PTLINK)");
        FieldRow(app, objectIndex, "Sound", kTorchSound, 's');
        break;
    case mob::Kind::Light:
        FieldRow(app, objectIndex, "Shadow", kLightShadow, 'b');
        break;
    case mob::Kind::Particle:
        FieldRow(app, objectIndex, "Type", mob::kParticleType, 'u');
        break;
    case mob::Kind::Sound:
        FieldRow(app, objectIndex, "Range 2", kSoundRange2, 'u');
        FieldRow(app, objectIndex, "Min distance", kSoundMin, 'u');
        FieldRow(app, objectIndex, "Max distance", kSoundMax, 'u');
        FieldRow(app, objectIndex, "Ambient", kSoundAmbient, 'b');
        FieldRow(app, objectIndex, "Music", kSoundMusic, 'b');
        break;
    default: break;
    }
}

// A trap's activation areas (x, y, radius) and cast points (x, y), and a sound's files: editable lists.
static void KindLists(App& app, const mob::Object& o, int objectIndex) {
    const mob::File& f = app.mobs[app.activeMob]->file;
    float v4[4];
    if (o.kind == mob::Kind::MagicTrap) {
        for (int which = 0; which < 2; ++which) {
            const uint32_t type = which == 0 ? kTrapAreas : kTrapTargets;
            const int per = which == 0 ? 3 : 2;
            std::vector<uint8_t> b = FieldBytes(f, objectIndex, type);
            if (b.size() < 4) continue;
            const uint32_t n = mob::U32(b.data());
            if (b.size() != 4 + static_cast<size_t>(n) * per * 4) continue;
            std::vector<float> vals(static_cast<size_t>(n) * per);
            std::memcpy(vals.data(), b.data() + 4, vals.size() * 4);
            ImGui::PushID(static_cast<int>(type));
            ImGui::SeparatorText(which == 0 ? "Activation areas (x, y, radius)" : "Cast points (x, y)");
            bool changed = false;
            int remove = -1;
            for (uint32_t i = 0; i < n; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (EditFloats("##e", per, vals.data() + i * per, v4, "%.2f", -30)) { std::memcpy(vals.data() + i * per, v4, per * 4); changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            if (remove >= 0) { vals.erase(vals.begin() + remove * per, vals.begin() + (remove + 1) * per); changed = true; }
            if (ImGui::SmallButton("Add")) {
                vals.push_back(o.position.x);
                vals.push_back(o.position.y);
                if (per == 3) vals.push_back(5.0f);
                changed = true;
            }
            if (changed) {
                std::vector<uint8_t> out = mob::U32Payload(static_cast<uint32_t>(vals.size() / per));
                out.resize(4 + vals.size() * 4);
                std::memcpy(out.data() + 4, vals.data(), vals.size() * 4);
                CommitField(app, objectIndex, type, out, false);
            }
            ImGui::PopID();
        }
    }
    if (o.kind == mob::Kind::Sound && mob::FindField(f, objectIndex, mob::kSoundResName)) {
        ImGui::SeparatorText("Sound files");
        std::vector<std::string> entries = o.soundFiles;
        bool changed = false;
        int remove = -1;
        for (size_t i = 0; i < entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(-40);
            std::string v;
            if (EditText("##file", mob::Utf8(entries[i]), v)) { entries[i] = ToCp(v); changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) { entries.erase(entries.begin() + remove); changed = true; }
        if (ImGui::SmallButton("Add##sound")) { entries.push_back("nature\\sound.wav"); changed = true; }
        if (changed) CommitField(app, objectIndex, mob::kSoundResName, mob::StringArrayPayload(mob::kSoundResName, entries), false);
    }
}

static void ObjectDetails(App& app, const mob::File& fileIn, int fileIndex, const mob::Object& oIn, int objectIndex) {
    (void)fileIn; (void)oIn;
    // The object is read afresh after each commit (a commit re-reads the file).
    auto obj = [&]() -> const mob::Object& { return app.mobs[app.activeMob]->file.objects[objectIndex]; };
    const mob::File& f = app.mobs[app.activeMob]->file;
    const mob::Object o = obj(); // a copy: the file may change under it this frame
    const bool multi = app.scene.selectedFile == app.activeMob && app.scene.selection.size() > 1;
    ImGui::SeparatorText("Selected");
    ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "%s", checks::Label(o).c_str());
    if (multi) {
        ImGui::Checkbox(("Apply edits to all " + std::to_string(app.scene.selection.size()) + " selected").c_str(), &app.editAll);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: only this object. Objects without the field are left alone. The ID is never shared.");
    }
    const bool figure = mob::HasFigure(o.kind);
    std::string text;
    float v4[4];
    uint32_t u;
    if (ImGui::BeginTable("##obj", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Map");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(f.fileName.c_str());

        // Name (lights, sounds and particles have their own name fields)
        const uint32_t nameType = o.kind == mob::Kind::Light ? mob::kLightName : o.kind == mob::Kind::Sound ? mob::kSoundName
                                : o.kind == mob::Kind::Particle ? mob::kParticleName : mob::kObjName;
        Label(o.kind == mob::Kind::Particle ? "Effect" : "Name");
        if (EditText("##name", mob::Utf8(o.kind == mob::Kind::Particle ? o.particleName : o.name), text)) CommitText(app, objectIndex, nameType, text);
        // ID: one object only (IDs must not repeat)
        if (o.hasId) {
            const uint32_t idType = o.kind == mob::Kind::Light ? mob::kLightId : o.kind == mob::Kind::Sound ? mob::kSoundId
                                  : o.kind == mob::Kind::Particle ? mob::kParticleId : mob::kNid;
            Label("ID");
            if (EditU32("##id", o.id, u)) CommitField(app, objectIndex, idType, mob::U32Payload(u), false);
        }
        if (figure) {
            Label("Figure");
            if (EditTextSuggest("##templ", o.templ, text, Names(app.lib).figures)) CommitText(app, objectIndex, mob::kObjTemplate, text);
            Label("Texture");
            if (EditTexture(app, "##tex", o.primTexture, text)) CommitText(app, objectIndex, mob::kObjPrimTexture, text);
            Label("2nd texture");
            if (EditTexture(app, "##tex2", o.secTexture, text)) CommitText(app, objectIndex, mob::kObjSecTexture, text);
            Label("Parent template");
            if (EditTextSuggest("##parent", mob::Utf8(o.parentTemplate), text, Names(app.lib).prototypes)) CommitText(app, objectIndex, mob::kParentTemplate, text);
        }
        // Position
        const uint32_t posType = o.kind == mob::Kind::Light ? mob::kLightPosition : o.kind == mob::Kind::Sound ? mob::kSoundPosition
                               : o.kind == mob::Kind::Particle ? mob::kParticlePosition : mob::kObjPosition;
        Label(figure ? "Position (z: above ground)" : "Position");
        {
            const float cur[3] = {o.position.x, o.position.y, o.position.z};
            if (EditFloats("##pos", 3, cur, v4)) CommitField(app, objectIndex, posType, F32Payload(v4, 3));
        }
        if (figure) {
            Label("Turn about Z (deg)");
            {
                const float yaw = 2.0f * std::atan2(o.rotation[3], o.rotation[0]) * 180.0f / 3.14159265f;
                const float cur[1] = {yaw};
                if (EditFloats("##yaw", 1, cur, v4, "%.1f")) {
                    const float h = v4[0] * 3.14159265f / 360.0f;
                    const float q[4] = {std::cos(h), 0.0f, 0.0f, std::sin(h)};
                    CommitField(app, objectIndex, mob::kObjRotation, F32Payload(q, 4));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets a turn about the vertical only (any tilt goes); the quaternion below keeps it");
            }
            Label("Rotation w x y z");
            if (EditFloats("##rot", 4, o.rotation, v4, "%.4f")) {
                float len = std::sqrt(v4[0] * v4[0] + v4[1] * v4[1] + v4[2] * v4[2] + v4[3] * v4[3]);
                if (len > 1e-6f) for (float& c : v4) c /= len; // kept a unit quaternion
                CommitField(app, objectIndex, mob::kObjRotation, F32Payload(v4, 4));
            }
            Label("Complection");
            {
                const float cur[3] = {o.complection.x, o.complection.y, o.complection.z};
                if (EditFloats("##comp", 3, cur, v4)) CommitField(app, objectIndex, mob::kObjComplection, F32Payload(v4, 3));
            }
            Label("Group (diplomacy)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The player group it belongs to (ei_maper's \"Player\"): one of the 32 groups of the\n"
                                  "Diplomacy tab, which says who is friend, neutral or enemy to whom");
            if (EditU32("##player", static_cast<uint32_t>(std::max(o.player, 0)), u))
                CommitField(app, objectIndex, mob::kObjPlayer, std::vector<uint8_t>{static_cast<uint8_t>(std::min<uint32_t>(u, 255))});
            Label("Engine type");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How the engine handles the object (ei_maper's \"Type\"): 50-52 units, 53 plants and scenery, 54 structures, 55 other objects, 58 torches, 59 magic traps, 60 levers, 8193+ particles. Change it only to fix a wrong one.");
            if (EditU32("##type", static_cast<uint32_t>(std::max(o.type, 0)), u)) CommitField(app, objectIndex, mob::kObjType, mob::U32Payload(u));
        }
        if (o.kind == mob::Kind::Unit) {
            Label("Prototype");
            if (EditTextSuggest("##proto", mob::Utf8(o.prototype), text, Names(app.lib).prototypes)) CommitText(app, objectIndex, mob::kUnitPrototype, text);
            Label("Stats imported");
            bool imported = o.needImport;
            if (ImGui::Checkbox("##needimport", &imported)) CommitField(app, objectIndex, mob::kUnitNeedImport, std::vector<uint8_t>{static_cast<uint8_t>(imported)});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: the game takes the stats from the prototype (the Stats below are ignored)");
        }
        if (o.kind == mob::Kind::MagicTrap) {
            Label("Spell");
            if (EditText("##spell", mob::Utf8(o.spell), text)) CommitText(app, objectIndex, mob::kMagicTrapSpell, text);
        }
        if (o.kind == mob::Kind::Light) {
            Label("Colour");
            {
                const float cur[3] = {o.color.x, o.color.y, o.color.z};
                if (EditFloats("##colour", 3, cur, v4)) CommitField(app, objectIndex, mob::kLightColor, F32Payload(v4, 3));
            }
            Label("Range");
            {
                const float cur[1] = {o.range};
                if (EditFloats("##range", 1, cur, v4, "%.2f")) CommitField(app, objectIndex, mob::kLightRange, F32Payload(v4, 1));
            }
        }
        if (o.kind == mob::Kind::Particle) {
            Label("Scale");
            const float cur[1] = {o.scale};
            if (EditFloats("##scale", 1, cur, v4, "%.2f")) CommitField(app, objectIndex, mob::kParticleScale, F32Payload(v4, 1));
        }
        if (o.kind == mob::Kind::Sound) {
            Label("Range");
            if (EditU32("##srange", static_cast<uint32_t>(o.range), u)) CommitField(app, objectIndex, mob::kSoundRange, mob::U32Payload(u));
        }
        if (figure) {
            Label("Comments");
            if (EditText("##comments", mob::Utf8(o.comments), text)) CommitText(app, objectIndex, mob::kObjComments, text);
            Label("Quest info");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ei_maper's \"Quest\": the quest (e.g. z3xq1) or quest objective this object belongs to;\n"
                    "the game marks it on the map while it is active.");
            if (EditText("##quest", mob::Utf8(o.questInfo), text)) CommitText(app, objectIndex, mob::kObjQuestInfo, text);
        }
        KindFieldRows(app, o, objectIndex);
        const MapModel* model = app.scene.ModelFor(o);
        if (figure && model && !model->ok) { Label("Figure problem"); ImGui::TextWrapped("%s", model->error.c_str()); }
        if (model && !model->dressed.empty()) {
            Label("Shown");
            ImGui::TextWrapped("%s", model->dressed.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Units on the default0 placeholder are dressed from the database like the game does: skin, hair and equipment (the map's own lists first).");
        }
        ImGui::EndTable();
    }
    KindLists(app, o, objectIndex);
    if (figure) BodyPartsEditor(app, o, objectIndex);
    if (o.kind == mob::Kind::Unit) StatsEditor(app, o, objectIndex);
    // A unit's item lists: each entry editable, X removes it, + adds one. They go to this unit only.
    if (o.kind == mob::Kind::Unit) {
        for (const mob::ItemList& l : o.lists) {
            ImGui::PushID(static_cast<int>(l.type));
            ImGui::SeparatorText((std::string(l.label) + "s").c_str());
            std::vector<std::string> entries = l.entries;
            bool changed = false;
            int removeAt = -1;
            for (size_t i = 0; i < entries.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(-40);
                std::string v;
                // item, then ".material": the part after the last dot completes from the materials
                const std::string cur = mob::Utf8(entries[i]);
                const size_t dot = cur.rfind('.');
                const bool spells = l.type == mob::kUnitSpells;
                const bool material = !spells && dot != std::string::npos;
                if (EditTextSuggest("##entry", cur, v, material ? Names(app.lib).materials : spells ? std::vector<std::string>() : Names(app.lib).items,
                                    material ? cur.substr(0, dot + 1) : std::string()))
                    { entries[i] = ToCp(v); changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeAt >= 0) { entries.erase(entries.begin() + removeAt); changed = true; }
            if (ImGui::SmallButton("Add")) { entries.push_back(l.type == mob::kUnitSpells ? "spell" : "item"); changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds an entry to type over (template.material for weapons and armors)");
            if (changed) CommitField(app, objectIndex, l.type, mob::StringArrayPayload(l.type, entries), false);
            ImGui::PopID();
        }
        LogicEditor(app, o, objectIndex);
    }
    if (!app.editMessage.empty()) ImGui::TextDisabled("%s", app.editMessage.c_str());
    for (const checks::Finding& fd : app.findings) {
        if (fd.file != fileIndex || fd.object != objectIndex) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(fd.severity));
        ImGui::TextWrapped("%s", fd.message.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Focus")) app.scene.FocusOn(obj());
}

static const char* KindPlural(mob::Kind k) {
    switch (k) {
    case mob::Kind::Object: return "World objects";
    case mob::Kind::Unit: return "Units";
    case mob::Kind::Lever: return "Levers";
    case mob::Kind::Torch: return "Torches";
    case mob::Kind::MagicTrap: return "Magic traps";
    case mob::Kind::Light: return "Lights";
    case mob::Kind::Particle: return "Particles";
    case mob::Kind::Sound: return "Sounds";
    }
    return "?";
}

// What an object is grouped under: a unit's prototype, else its parent template, figure or name.
static std::string GroupLabel(const mob::Object& o) {
    for (const std::string* s : {&o.prototype, &o.parentTemplate, &o.templ, &o.particleName, &o.name})
        if (!s->empty() && Lower(*s) != "none") return mob::Utf8(*s);
    return "(unnamed)";
}

std::string ObjectLabel(const mob::Object& o) {
    const std::string& n = o.name.empty() ? o.particleName : o.name;
    return n.empty() ? "(unnamed)" : mob::Utf8(n);
}

static void BuildTree(App& app) {
    app.tree.clear();
    if (app.mobs.empty()) return;
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    const std::string f = Lower(app.filter);
    std::map<mob::Kind, std::map<std::string, std::vector<int>>> byKind;
    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const mob::Object& o = objects[oi];
        if (!f.empty()) {
            std::string hay = Lower(o.name + " " + o.templ + " " + o.primTexture + " " + o.prototype + " " + o.parentTemplate + " " +
                                    o.particleName) + (o.hasId ? " " + std::to_string(o.id) : "");
            if (hay.find(f) == std::string::npos) continue;
        }
        byKind[o.kind][GroupLabel(o)].push_back(static_cast<int>(oi));
    }
    for (auto& kv : byKind) {
        ObjectCategory c;
        c.kind = kv.first;
        for (auto& g : kv.second) {
            c.count += static_cast<int>(g.second.size());
            c.groups.push_back({g.first, std::move(g.second)});
        }
        app.tree.push_back(std::move(c));
    }
}

void DeleteSelection(App& app);
std::filesystem::path ClipboardFile();
void CopySelection(App& app);
void Paste(App& app, bool atCursor = false);
void Duplicate(App& app);

void ObjectsTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Active map");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##activemob", app.mobs[app.activeMob]->file.fileName.c_str())) {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (ImGui::Selectable(app.mobs[i]->file.fileName.c_str(), static_cast<int>(i) == app.activeMob)) SetActiveMob(app, static_cast<int>(i));
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the active map's objects are listed and selectable (%s: next map)",
                                                  ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter: name, ID, prototype, figure or texture", app.filter, sizeof(app.filter));
    { // editing the objects: the same as the keys
        const bool sel = app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
        if (ImGui::SmallButton("Add...")) { app.newOpen = true; app.newMessage.clear(); }
        ImGui::SetItemTooltip("%s", ui::BindName(app.lib.mapKeys[config::kKeyNewObject]).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        if (ImGui::SmallButton("Duplicate")) Duplicate(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) CopySelection(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        std::error_code ec;
        ImGui::BeginDisabled(app.clipboard.empty() && !std::filesystem::exists(ClipboardFile(), ec));
        if (ImGui::SmallButton("Paste")) Paste(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", i18n::Tr("Where the objects were copied from (Shift + the paste key: at the cursor)"));
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste at cursor")) Paste(app, true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        if (ImGui::SmallButton("Delete")) DeleteSelection(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Keys: %s, %s, %s, %s", ui::BindName(app.lib.mapKeys[config::kKeyDelete]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyCopy]).c_str(),
                              ui::BindName(app.lib.mapKeys[config::kKeyPaste]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyDuplicate]).c_str());
    }

    std::string key = std::string(app.filter) + "|" + std::to_string(app.activeMob) + "|" + app.mobs[app.activeMob]->file.path;
    if (key != app.listedKey) {
        app.listedKey = key;
        BuildTree(app);
    }
    const bool filtering = app.filter[0] != '\0';
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    const bool hasSelection = app.scene.selectedFile == app.activeMob && app.scene.selectedObject >= 0 &&
                              app.scene.selectedObject < static_cast<int>(objects.size());
    float listHeight = hasSelection ? ImGui::GetContentRegionAvail().y * 0.5f : 0.0f;
    if (ImGui::BeginTable("##objects", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Count / ID", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        const mob::Object* sel = hasSelection ? &objects[app.scene.selectedObject] : nullptr;
        for (const ObjectCategory& c : app.tree) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool revealHere = app.revealSelection && sel && sel->kind == c.kind;
            if (filtering || revealHere) ImGui::SetNextItemOpen(true, filtering ? ImGuiCond_Appearing : ImGuiCond_Always);
            bool open = ImGui::TreeNodeEx(KindPlural(c.kind), ImGuiTreeNodeFlags_SpanAllColumns);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d", c.count);
            if (!open) continue;
            for (const ObjectGroup& g : c.groups) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool groupHasSel = revealHere && std::find(g.objects.begin(), g.objects.end(), app.scene.selectedObject) != g.objects.end();
                if (groupHasSel) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                bool gopen = ImGui::TreeNodeEx(g.label.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu", g.objects.size());
                if (!gopen) continue;
                for (int oi : g.objects) {
                    const mob::Object& o = objects[oi];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(oi);
                    bool selected = app.scene.IsSelected(app.activeMob, oi);
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns |
                                               (selected ? ImGuiTreeNodeFlags_Selected : 0);
                    ImGui::TreeNodeEx("##o", flags, "%s", ObjectLabel(o).c_str());
                    if (ImGui::IsItemClicked()) {
                        if (ImGui::GetIO().KeyShift) app.scene.Toggle(app.activeMob, oi);
                        else SelectObject(app, app.activeMob, oi, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %s\n(double-click: go to it; Shift+click: add or remove)", mob::KindName(o.kind), o.templ.c_str());
                    if (selected && app.revealSelection) { ImGui::SetScrollHereY(0.3f); app.revealSelection = false; }
                    ImGui::TableNextColumn();
                    if (o.hasId) ImGui::TextDisabled("%u", o.id);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
    if (hasSelection) {
        ImGui::BeginChild("##details");
        if (app.scene.selection.size() > 1) {
            ImGui::TextDisabled("%zu objects selected; details of the last one picked", app.scene.selection.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) app.scene.ClearSelection();
        }
        const mob::File& f = app.mobs[app.activeMob]->file;
        if (app.scene.selectedObject >= 0) ObjectDetails(app, f, app.activeMob, f.objects[app.scene.selectedObject], app.scene.selectedObject);
        ImGui::EndChild();
    }
}

void ChecksTab(App& app) {
    if (app.mobs.empty()) {
        ImGui::TextWrapped("Load one or more .mob files in the Files tab to check them.");
        return;
    }
    if (ImGui::Button("Check again")) app.checksDirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The checks run by themselves when the files or sources change");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('E'));
    ImGui::Checkbox(("Errors (" + std::to_string(app.summary.errors) + ")").c_str(), &app.showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('W'));
    ImGui::Checkbox(("Warnings (" + std::to_string(app.summary.warnings) + ")").c_str(), &app.showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor('I'));
    ImGui::Checkbox(("Info (" + std::to_string(app.summary.infos) + ")").c_str(), &app.showInfos);
    ImGui::PopStyleColor();
    if (app.database.Empty())
        ui::Note("No items database in the Settings tab: item, spell and prototype names are not checked.");
    else
        ImGui::TextDisabled("Names checked against %zu database file(s) beside %s", app.database.files.size(),
                            app.lib.dbPath.substr(app.lib.dbPath.find_last_of("/\\") + 1).c_str());
    if (app.findings.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "No problems found.");
        return;
    }
    if (ImGui::BeginTable("##findings", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 14);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, app.mobs.size() > 1 ? 150.0f : 70.0f);
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < app.findings.size(); ++i) {
            const checks::Finding& f = app.findings[i];
            if ((f.severity == 'E' && !app.showErrors) || (f.severity == 'W' && !app.showWarnings) || (f.severity == 'I' && !app.showInfos)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(f.severity));
            char sev[2] = {f.severity, 0};
            bool clicked = ImGui::Selectable(sev, false, ImGuiSelectableFlags_SpanAllColumns);
            ImGui::PopStyleColor();
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s", f.category.c_str(),
                                  f.object >= 0 ? "\nClick: select the object and go to it" : f.line > 0 ? "\nClick: show the script line" : "");
            }
            ImGui::TableNextColumn();
            // The file only when several are loaded; the script line when there is one.
            std::string where = app.mobs.size() > 1 && f.file >= 0 && f.file < static_cast<int>(app.mobs.size()) ? app.mobs[f.file]->file.fileName : "";
            if (f.line > 0) where += (where.empty() ? "line " : ":") + std::to_string(f.line);
            ImGui::TextUnformatted(where.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", f.message.c_str());
            if (clicked) {
                if (f.object >= 0) {
                    SelectObject(app, f.file, f.object, true);
                    app.requestTab = SideTab::Objects;
                } else if (f.line > 0) {
                    app.scriptFile = f.file;
                    app.scriptLine = f.line;
                    app.scrollToLine = true;
                    app.requestTab = SideTab::Script;
                }
            }
        }
        ImGui::EndTable();
    }
}


} // namespace mapedit
