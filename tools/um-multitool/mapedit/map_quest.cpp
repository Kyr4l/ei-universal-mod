// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Quest tab, the navmesh job, saving changes, the unload dialog.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Quest tab: the files of the open quest as text. Packed quests hold quest.reg (the game's binary
// registry), shown and saved as INI text; unpacked ones hold quest.ini. Texts keep their encoding.
// ------------------------------------------------------------------------------------------------

static bool IsLanguageFree(const std::string& entry) {
    return quest::EndsWithLower(entry, "map.txt") || quest::EndsWithLower(entry, "quest.ini") || quest::EndsWithLower(entry, "quest.reg");
}

// The same file in another copy of the quest: same name with either slash, or quest.ini <-> quest.reg.
static std::string MatchEntry(const quest::Quest& copy, const std::string& entry) {
    auto norm = [](std::string n) {
        n = Lower(n);
        std::replace(n.begin(), n.end(), '\\', '/');
        if (quest::EndsWithLower(n, "quest.reg") || quest::EndsWithLower(n, "quest.ini")) n = n.substr(0, n.size() - 4) + ".cfg";
        return n;
    };
    const std::string want = norm(entry);
    for (const std::string& e : quest::ListEntries(copy)) if (norm(e) == want) return e;
    return std::string();
}

static bool g_questCp1250 = false; // the quest texts' 8-bit files read as CP1250 (Polish), chosen in the encoding list

// The file as UTF-8 text with \n lines (quest.reg converted to INI).
static bool ReadEntryText(const quest::Quest& q, const std::string& entry, std::string& text, codec::Encoding& enc, bool& crlf, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!quest::ReadEntry(q, entry, bytes)) { err = "cannot read " + entry; return false; }
    if (quest::EndsWithLower(entry, ".reg")) {
        std::string ini;
        if (!IniRegRegToText(bytes, ini, err)) return false;
        bytes.assign(ini.begin(), ini.end());
    }
    enc = codec::Detect(bytes);
    if (g_questCp1250 && enc == codec::Encoding::Cp1251) enc = codec::Encoding::Cp1250; // not told apart by the bytes
    std::string t = codec::ToUtf8(bytes, enc);
    crlf = t.find("\r\n") != std::string::npos;
    text.clear();
    for (char c : t) if (c != '\r') text += c;
    return true;
}

static bool EncodeEntryText(const std::string& entry, const std::string& text, codec::Encoding enc, bool crlf, std::vector<uint8_t>& out, std::string& err) {
    std::string t;
    for (char c : text) { if (c == '\n' && crlf) t += '\r'; t += c; }
    std::vector<uint8_t> bytes;
    if (!codec::FromUtf8(t, enc, bytes, err)) return false;
    if (quest::EndsWithLower(entry, ".reg")) {
        std::string ini(bytes.begin(), bytes.end());
        if (!IniRegTextToReg(ini, out)) { err = "not valid INI text"; return false; }
        return true;
    }
    out = std::move(bytes);
    return true;
}

static int ResizeStringCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = static_cast<std::string*>(d->UserData);
        str->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

// Saves the Quest tab's text (to this copy, or every copy when "Save to every language" is on).
static void SaveQuestText(App& app) {
    if (!app.openQuest || !app.mqTextDirty) return;
    quest::QuestSet& set = *app.openQuest;
    const bool mapTxt = quest::EndsWithLower(app.mqEntry, "map.txt");
    if (mapTxt && app.questDirty) {
        app.mqMessage = "Save or revert the moved areas first: they would overwrite this text";
        return;
    }
    std::string errors;
    int saved = 0;
    for (size_t i = 0; i < set.copies.size(); ++i) {
        if (!app.mqApplyAll && static_cast<int>(i) != app.mqCopy) continue;
        const quest::Quest& c = set.copies[i];
        std::string entry = static_cast<int>(i) == app.mqCopy ? app.mqEntry : MatchEntry(c, app.mqEntry);
        if (entry.empty()) { errors += " " + set.labels[i] + " (no such file)"; continue; }
        std::vector<uint8_t> bytes;
        std::string err;
        if (!EncodeEntryText(entry, app.mqText, app.mqEncoding, app.mqCrlf, bytes, err) || !quest::WriteEntries(c, {{entry, bytes}}, err)) {
            errors += " " + set.labels[i] + ": " + err;
            continue;
        }
        ++saved;
    }
    app.mqMessage = "Saved " + app.mqEntry + " to " + std::to_string(saved) + " cop" + (saved == 1 ? "y" : "ies") +
                    (errors.empty() ? "" : "; failed:" + errors);
    app.mqTextDirty = false;
    if (mapTxt) ReloadQuest(app); // the areas and exits come from map.txt
}

// Ctrl+S / the toolbar's Save: whatever the open quest has unsaved (its areas, the Quest tab's text).
bool AnyMobDirty(const App& app) {
    for (auto& m : app.mobs) if (m->Dirty()) return true;
    return TerrainUnsaved(app);
}

// Writes the terrain (to its own file, or `path`): the header and the edited sectors, the rest kept.
bool SaveTerrain(App& app, const std::string& path) {
    std::vector<int> sectors(app.terrainEditedSectors.begin(), app.terrainEditedSectors.end());
    std::string err;
    const std::string old = app.terrainPath;
    if (!mpr::Save(app.terrain, path, sectors, app.terrainHeaderEdited || path != old, err)) {
        app.terrainMessage = "Terrain not saved: " + err;
        app.questMessage = app.terrainMessage;
        return false;
    }
    if (path != old) { // the terrain is now that file: its undo steps and load order follow it
        for (auto* v : {&app.undoSteps, &app.redoSteps}) for (EditStep& st : *v) if (st.file == old) st.file = path;
        std::replace(app.loadOrder.begin(), app.loadOrder.end(), old, path);
        app.terrainPath = path;
    }
    app.terrainEditedSectors.clear();
    app.terrainHeaderEdited = false;
    app.terrainMessage = "Saved " + path;
    return true;
}

std::vector<navgen::Object> NavObjects(const LayeredAssetSource& figures, const std::vector<const mob::File*>& maps, int* missing);

static void StartJob(App& app, const std::string& title, std::function<void()> work, std::function<void()> apply) {
    if (app.job && app.job->running) return;
    auto job = std::make_unique<Job>();
    job->title = title;
    job->apply = std::move(apply);
    job->started = std::chrono::steady_clock::now();
    job->running = true;
    Job* raw = job.get();
    job->thread = std::thread([raw, work = std::move(work)] { work(); raw->done = true; });
    app.job = std::move(job);
    umlog::Write(umlog::Level::Info, "Map Editor: " + title + "...");
}

// Each frame: finishes a done job (its `apply` on the main thread) and draws the progress window of a running one.
void PollJob(App& app) {
    if (!app.job || !app.job->running) return;
    if (app.job->done) {
        app.job->thread.join();
        app.job->running = false;
        std::function<void()> apply = std::move(app.job->apply);
        const auto seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - app.job->started).count();
        umlog::Write(umlog::Level::Info, "Map Editor: " + app.job->title + " done in " + ui::Num(seconds, 1) + " s");
        app.job.reset();
        if (apply) apply();
        return;
    }
    ImGui::OpenPopup("##busyjob");
    ImGui::SetNextWindowPos(ImVec2((app.viewportMin.x + app.viewportMax.x) * 0.5f, app.viewportMax.y - 60), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    if (ImGui::BeginPopupModal("##busyjob", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - app.job->started).count();
        ImGui::Text("%s  (%.0f s)", i18n::Tr(app.job->title.c_str()), seconds);
        // no progress count comes out of the generator: a bar that sweeps
        const float t = std::fmod(seconds * 0.5f, 1.0f);
        ImGui::ProgressBar(-1.0f * t, ImVec2(-1, 0), "");
        ImGui::TextDisabled("%s", i18n::Tr("The editor is working, not stuck: the view comes back when it is done."));
        ImGui::EndPopup();
    }
}

void StartNavmeshJob(App& app, std::function<void(const NavResult&)> onDone) {
    auto result = std::make_shared<NavResult>();
    std::vector<const mob::File*> all;
    for (auto& m : app.mobs) all.push_back(&m->file);
    const mpr::Map* terrain = &app.terrain;
    const LayeredAssetSource* figures = &app.lib.figures;
    StartJob(app, "Building the navmesh",
             [result, all, terrain, figures] {
                 const std::vector<navgen::Object> objects = NavObjects(*figures, all, &result->missing);
                 if (!navgen::Generate(*terrain, objects, result->payload, result->err)) result->payload.clear();
             },
             [result, onDone] { onDone(*result); });
}

// Save with "Navmesh" on: the open maps that have a navmesh (AI_GRAPH, the zone's main map) get it built again
// from the terrain and every open map's objects, as the game builds it (navmesh_gen.hpp). Returns what to say.
static std::string RegenerateNavmeshes(App& app) {
    std::vector<mob::File*> targets;
    for (auto& m : app.mobs) if (m->file.aiGraphBytes) targets.push_back(&m->file);
    if (targets.empty()) return "";
    if (app.terrain.sectorsX <= 0) return "navmesh not rebuilt: no terrain open";
    std::vector<const mob::File*> all;
    for (auto& m : app.mobs) all.push_back(&m->file);
    int missing = 0;
    const std::vector<navgen::Object> objects = NavObjects(app.lib.figures, all, &missing);
    std::vector<uint8_t> payload;
    std::string err;
    if (!navgen::Generate(app.terrain, objects, payload, err)) return "navmesh not rebuilt: " + err;
    std::string done;
    for (mob::File* f : targets) {
        mob::SetAiGraph(*f, payload);
        done += (done.empty() ? "" : ", ") + f->fileName;
    }
    return "navmesh rebuilt in " + done + (missing ? " (" + std::to_string(missing) + " objects without a figure left out)" : "");
}

// "Navmesh differences": the navmesh built from the open terrain and maps, for the view to compare.
void BuildCompareNavmesh(App& app) {
    app.scene.builtNav.clear();
    ++app.scene.builtNavStamp;
    if (app.terrain.sectorsX <= 0) { app.navCompareNote = "No terrain open."; return; }
    if (app.job && app.job->running) return;
    StartNavmeshJob(app, [&app](const NavResult& res) {
        if (res.payload.empty()) { app.navCompareNote = res.err; return; }
        app.scene.builtNav = res.payload;
        ++app.scene.builtNavStamp;
        app.navCompareNote = res.missing ? std::to_string(res.missing) + " objects without a figure left out." : "";
    });
}

// Ctrl+S / the toolbar's Save: every unsaved change (edited maps, the quest's areas, the Quest tab's text).
static void SaveQuestChangesNow(App& app, const std::string& navmesh);
// Ctrl+S: with "Navmesh" on and an edited map, the navmesh is built first (on a thread, a progress window
// meanwhile), then everything is written.
void SaveQuestChanges(App& app) {
    bool needsNavmesh = app.lib.mapRegenNavmesh && AnyMobDirty(app) && app.terrain.sectorsX > 0;
    if (needsNavmesh) {
        bool any = false;
        for (auto& m : app.mobs) any |= m->file.aiGraphBytes != 0;
        needsNavmesh = any;
    }
    if (!needsNavmesh) { SaveQuestChangesNow(app, app.lib.mapRegenNavmesh && AnyMobDirty(app) ? RegenerateNavmeshes(app) : std::string()); return; }
    StartNavmeshJob(app, [&app](const NavResult& res) {
        std::string note;
        if (res.payload.empty()) note = "navmesh not rebuilt: " + res.err;
        else {
            std::string done;
            for (auto& m : app.mobs)
                if (m->file.aiGraphBytes) { mob::SetAiGraph(m->file, res.payload); done += (done.empty() ? "" : ", ") + m->file.fileName; }
            note = "navmesh rebuilt in " + done + (res.missing ? " (" + std::to_string(res.missing) + " objects without a figure left out)" : "");
        }
        SaveQuestChangesNow(app, note);
    });
}

static void SaveQuestChangesNow(App& app, const std::string& navmesh) {
    std::string saved, failed;
    for (auto& m : app.mobs) {
        if (!m->Dirty()) continue;
        std::string err;
        if (mob::Save(m->file, err)) { m->savedBytes = m->file.bytes; saved += (saved.empty() ? "" : ", ") + m->file.fileName; }
        else failed += " " + m->file.fileName + ": " + err;
    }
    if (TerrainUnsaved(app)) {
        if (SaveTerrain(app, app.terrainPath)) saved += (saved.empty() ? "" : ", ") + std::filesystem::path(app.terrainPath).filename().string();
        else failed += " terrain: " + app.terrainMessage;
    }
    if (!saved.empty() || !failed.empty()) app.filesMessage = (saved.empty() ? "" : "Saved " + saved) + (failed.empty() ? "" : "; could not save" + failed);
    if (!navmesh.empty()) app.filesMessage += (app.filesMessage.empty() ? "" : "; ") + navmesh;
    if (!saved.empty()) app.questMessage = "Saved " + saved;
    if (app.questDirty) SaveQuestAreas(app);
    if (app.mqTextDirty) SaveQuestText(app);
}

// Unloading a file with unsaved changes (RequestUnload): save them, drop them, or keep the file loaded.
void UnloadDialog(App& app) {
    if (app.unloadAsk.empty()) return;
    if (!FileUnsaved(app, app.unloadAsk)) { app.unloadAsk.clear(); return; } // saved or undone meanwhile, or gone
    const bool terrain = app.terrainLoaded && app.unloadAsk == app.terrainPath;
    ImGui::OpenPopup("Unsaved changes##unload");
    ImGui::SetNextWindowPos(ImVec2((app.viewportMin.x + app.viewportMax.x) * 0.5f, (app.viewportMin.y + app.viewportMax.y) * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f)); // over the view (its size is known from its second frame on)
    if (!ImGui::BeginPopupModal("Unsaved changes##unload", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    const std::string path = app.unloadAsk;
    ImGui::Text("%s has unsaved changes.", std::filesystem::path(path).filename().string().c_str());
    ImGui::TextDisabled("%s", path.c_str());
    bool close = false;
    if (ImGui::Button("Save and unload")) {
        std::string err;
        bool saved = false;
        if (terrain) {
            saved = SaveTerrain(app, path);
            err = app.terrainMessage;
        } else if (MobEntry* m = FindMob(app, path)) {
            saved = mob::Save(m->file, err);
            if (saved) m->savedBytes = m->file.bytes;
        }
        if (saved) UnloadFile(app, path);
        else app.filesMessage = "Not saved, still loaded: " + err;
        close = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes this file as it is, then unloads it (%s saves every changed file, and also rebuilds the navmesh when Navmesh is on)",
                          ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Discard changes and unload")) {
        UnloadFile(app, path);
        close = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unloads it as it is on disk: the changes since the last save, and their undo steps, are lost");
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (close) {
        app.unloadAsk.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void QuestTab(App& app) {
    if (!app.openQuest) {
        ImGui::TextWrapped("Open a quest in the Files tab (quests come from Settings > Quests) to see and edit its files here.");
        return;
    }
    quest::QuestSet& set = *app.openQuest;
    app.mqCopy = std::min(std::max(app.mqCopy, 0), static_cast<int>(set.copies.size()) - 1);
    const quest::Quest& copy = set.copies[app.mqCopy];
    ImGui::Text("%s", set.shown.name.c_str());
    if (set.copies.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##mqcopy", set.labels[app.mqCopy].c_str())) {
            for (size_t i = 0; i < set.copies.size(); ++i)
                if (ImGui::Selectable(set.labels[i].c_str(), static_cast<int>(i) == app.mqCopy) && !app.mqTextDirty) {
                    app.mqCopy = static_cast<int>(i);
                    app.mqLoadedFor.clear();
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The language pack whose copy is shown and edited");
    }
    ImGui::TextDisabled("%s (%s)", copy.path.c_str(), copy.packed ? "packed" : "unpacked");
    if (app.mqEntriesFor != copy.path) {
        app.mqEntries = quest::ListEntries(copy);
        app.mqEntriesFor = copy.path;
        if (std::find(app.mqEntries.begin(), app.mqEntries.end(), app.mqEntry) == app.mqEntries.end()) {
            std::string match = app.mqEntry.empty() ? std::string() : MatchEntry(copy, app.mqEntry);
            app.mqEntry = !match.empty() ? match : app.mqEntries.empty() ? std::string() : app.mqEntries.front();
            for (const std::string& e : app.mqEntries) if (match.empty() && quest::EndsWithLower(e, "map.txt")) app.mqEntry = e;
        }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##mqentry", app.mqEntry.c_str())) {
        for (const std::string& e : app.mqEntries)
            if (ImGui::Selectable(e.c_str(), e == app.mqEntry) && !app.mqTextDirty) { app.mqEntry = e; app.mqLoadedFor.clear(); }
        ImGui::EndCombo();
    }
    if (app.mqEntry.empty()) return;
    const std::string key = copy.path + "|" + app.mqEntry;
    if (app.mqLoadedFor != key) {
        std::string err;
        app.mqIsReg = quest::EndsWithLower(app.mqEntry, ".reg");
        if (!ReadEntryText(copy, app.mqEntry, app.mqText, app.mqEncoding, app.mqCrlf, err)) { app.mqText.clear(); app.mqMessage = err; }
        else app.mqMessage.clear();
        app.mqLoadedFor = key;
        app.mqTextDirty = false;
        app.mqApplyAll = IsLanguageFree(app.mqEntry);
    }
    if (app.mqIsReg) ImGui::TextDisabled("binary quest.reg, shown as INI text (saved back as .reg)");
    ImGui::SetNextItemWidth(170);
    int enc = static_cast<int>(app.mqEncoding);
    const char* encs[] = {codec::EncodingName(codec::Encoding::Cp1251), codec::EncodingName(codec::Encoding::Utf8), codec::EncodingName(codec::Encoding::Cp949),
                          codec::EncodingName(codec::Encoding::Cp1250)};
    if (ImGui::Combo("encoding", &enc, encs, 4)) {
        const codec::Encoding chosen = static_cast<codec::Encoding>(enc);
        const bool polish = chosen == codec::Encoding::Cp1250, wasPolish = app.mqEncoding == codec::Encoding::Cp1250;
        if ((polish || wasPolish) && !app.mqTextDirty) { g_questCp1250 = polish; app.mqLoadedFor.clear(); } // read again in it
        else { app.mqEncoding = chosen; app.mqTextDirty = true; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The file's encoding (detected); it is saved in this encoding");
    if (set.copies.size() > 1) {
        ImGui::SameLine();
        ImGui::Checkbox("Save to every language", &app.mqApplyAll);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: every language pack's copy of this file gets this text (map.txt and quest.ini/.reg do not depend on\n"
                              "the language). Off: only the %s copy (for briefings and texts).", set.labels[app.mqCopy].c_str());
    }
    ImGui::BeginDisabled(!app.mqTextDirty);
    if (ImGui::Button("Save")) SaveQuestText(app);
    ImGui::SameLine();
    if (ImGui::Button("Revert##mq")) app.mqLoadedFor.clear();
    ImGui::EndDisabled();
    if (!app.mqMessage.empty()) { ImGui::SameLine(); ImGui::TextWrapped("%s", app.mqMessage.c_str()); }
    if (ImGui::InputTextMultiline("##mqtext", app.mqText.data(), app.mqText.capacity() + 1, ImVec2(-1, -1),
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeStringCallback, &app.mqText))
        app.mqTextDirty = true;
}


} // namespace mapedit
