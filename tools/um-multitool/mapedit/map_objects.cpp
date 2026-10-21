// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Objects: find, delete / copy / paste / duplicate / new, randomize, Diplomacy, IDs.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

#include <cstdarg>

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Find (Ctrl+F): the active map's objects by name (wildcards * and ?, case-sensitive or not), kind and
// ID range; select the matches.
// ------------------------------------------------------------------------------------------------

static bool WildcardMatch(const char* p, const char* t) {
    // * any run, ? any one character (iterative, with backtracking to the last *)
    const char *star = nullptr, *resume = nullptr;
    while (*t) {
        if (*p == '?' || *p == *t) { ++p; ++t; }
        else if (*p == '*') { star = p++; resume = t; }
        else if (star) { p = star + 1; t = ++resume; }
        else return false;
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

static bool FindMatches(const App& app, const mob::Object& o) {
    static const mob::Kind kinds[] = {mob::Kind::Object, mob::Kind::Unit, mob::Kind::Lever, mob::Kind::Torch, mob::Kind::MagicTrap,
                                      mob::Kind::Light, mob::Kind::Particle, mob::Kind::Sound};
    if (app.findKind > 0 && o.kind != kinds[app.findKind - 1]) return false;
    if (app.findIds && (!o.hasId || static_cast<int64_t>(o.id) < app.findIdMin || static_cast<int64_t>(o.id) > app.findIdMax)) return false;
    std::string pattern = app.findText;
    if (pattern.empty()) return true;
    auto fold = [&](std::string t) { return app.findCase ? t : Lower(t); };
    const bool wild = pattern.find_first_of("*?") != std::string::npos;
    pattern = fold(pattern);
    std::vector<std::string> fields = {mob::Utf8(o.name.empty() ? o.particleName : o.name)};
    if (app.findMore) { fields.push_back(o.templ); fields.push_back(mob::Utf8(o.prototype)); fields.push_back(mob::Utf8(o.parentTemplate)); }
    for (const std::string& f : fields) {
        const std::string t = fold(f);
        if (wild ? WildcardMatch(pattern.c_str(), t.c_str()) : t.find(pattern) != std::string::npos) return true;
    }
    return false;
}

// ---- delete, copy, paste, duplicate, new ---------------------------------------------------------------

void DeleteSelection(App& app) {
    if (app.simulating) return;
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    PushUndo(app, std::move(step), "Delete " + std::to_string(app.scene.selection.size()) + " object(s)");
    const size_t n = app.scene.selection.size();
    mob::RemoveObjects(f, app.scene.selection);
    app.scene.ClearSelection();
    app.checksDirty = true;
    app.listedKey.clear();
    app.questMessage = "Deleted " + std::to_string(n) + " object(s)";
}

// The clipboard is also written to a file in the temporary folder, so another running um-multitool can
// paste it (like ei_maper's copy_paste_buffer.json): "UMCB", count, then per object its position and node.
std::filesystem::path ClipboardFile() {
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "um-multitool-clipboard.bin";
}

static void WriteClipboardFile(const std::vector<App::Copied>& items) {
    std::ofstream out(ClipboardFile(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    const uint32_t n = static_cast<uint32_t>(items.size());
    out.write("UMCB", 4);
    out.write(reinterpret_cast<const char*>(&n), 4);
    for (const App::Copied& c : items) {
        const uint32_t size = static_cast<uint32_t>(c.node.size());
        out.write(reinterpret_cast<const char*>(&c.position), 12);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(c.node.data()), size);
    }
}

static bool ReadClipboardFile(std::vector<App::Copied>& items) {
    std::ifstream in(ClipboardFile(), std::ios::binary);
    char magic[4] = {};
    uint32_t n = 0;
    if (!in.read(magic, 4) || std::memcmp(magic, "UMCB", 4) != 0 || !in.read(reinterpret_cast<char*>(&n), 4) || n > 100000) return false;
    std::vector<App::Copied> out(n);
    for (App::Copied& c : out) {
        uint32_t size = 0;
        if (!in.read(reinterpret_cast<char*>(&c.position), 12) || !in.read(reinterpret_cast<char*>(&size), 4) || size < 8 || size > (64u << 20)) return false;
        c.node.resize(size);
        if (!in.read(reinterpret_cast<char*>(c.node.data()), size) || mob::U32(c.node.data() + 4) != size) return false;
    }
    items = std::move(out);
    return true;
}

void CopySelection(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    app.clipboard.clear();
    for (int oi : app.scene.selection) app.clipboard.push_back({mob::ObjectNode(f, oi), f.objects[oi].position});
    WriteClipboardFile(app.clipboard);
    app.questMessage = "Copied " + std::to_string(app.clipboard.size()) + " object(s)";
}

// Free IDs for new objects: from the active map's first ID range, skipping every loaded map's IDs.
static std::vector<uint32_t> FreeIds(App& app, size_t count) {
    std::set<uint32_t> used;
    for (auto& m : app.mobs) for (const mob::Object& o : m->file.objects) if (o.hasId) used.insert(o.id);
    const mob::File& f = app.mobs[app.activeMob]->file;
    uint64_t next = f.mainRanges.empty() ? 1 : std::max<uint32_t>(f.mainRanges[0].min, 1);
    const uint64_t hi = f.mainRanges.empty() ? 0xFFFFFFFFu : f.mainRanges[0].max;
    std::vector<uint32_t> ids;
    while (ids.size() < count && next <= hi) { if (!used.count(static_cast<uint32_t>(next))) ids.push_back(static_cast<uint32_t>(next)); ++next; }
    return ids;
}

// Where pasted or new objects go: the ground under the mouse when it is over the view, else the view's centre.
static mob::Vec3 PlacePoint(App& app) {
    if (app.hoverGround) return {app.ground.x, app.ground.y, 0};
    return {app.scene.camera.targetX, app.scene.camera.targetY, 0};
}

// Inserts copies of these nodes into the active map with new IDs, moved so that their centre lands at
// `at` (or where they were, when `inPlace`), and selects them. One undo step.
bool InsertCopies(App& app, std::vector<App::Copied> items, bool inPlace, mob::Vec3 at) {
    if (app.simulating) return false;
    if (app.mobs.empty() || items.empty()) return false;
    mob::File& f = app.mobs[app.activeMob]->file;
    if (!f.objectSectionAt) { app.questMessage = "This map has no object section to add to"; return false; }
    std::vector<uint32_t> ids = FreeIds(app, items.size());
    if (ids.size() < items.size()) { app.questMessage = "Not enough free IDs in the map's ID range"; return false; }
    mob::Vec3 c{0, 0, 0};
    for (auto& it : items) { c.x += it.position.x; c.y += it.position.y; }
    c.x /= items.size();
    c.y /= items.size();
    std::vector<std::vector<uint8_t>> nodes;
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<uint8_t> n = items[i].node;
        mob::NodeSetId(n, ids[i]);
        if (!inPlace) mob::NodeMove(n, at.x - c.x, at.y - c.y, 0.0f);
        nodes.push_back(std::move(n));
    }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    PushUndo(app, std::move(step), "Add " + std::to_string(items.size()) + " object(s)");
    std::vector<int> added = mob::InsertObjects(f, nodes);
    app.scene.ClearSelection();
    for (int oi : added) {
        if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
        else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
    }
    app.checksDirty = true;
    app.listedKey.clear();
    app.revealSelection = true;
    return true;
}

// Pasting puts the objects where they were copied from (ei_maper's way: copies between the .mob files of one map
// land in place); `atCursor` (Shift, or the second button) puts them under the mouse instead.
void Paste(App& app, bool atCursor) {
    ReadClipboardFile(app.clipboard); // the last copy, from this um-multitool or another one
    if (app.clipboard.empty()) { app.questMessage = "Nothing copied"; return; }
    if (InsertCopies(app, app.clipboard, !atCursor, PlacePoint(app)))
        app.questMessage = "Pasted " + std::to_string(app.clipboard.size()) + " object(s)" + (atCursor ? " at the cursor" : " where they were copied from");
}

// Ctrl+D: copies of the selection where it is, then moving them (like Blender's Shift+D).
void StartTransform(App& app, Transform::Mode mode, ImVec2 mouse, ImVec2 min, ImVec2 size);
void PruneLogicPoints(App& app);
bool LogicPointAt(const mob::File& f, const LogicPointRef& r, mob::Vec3& out);
void SetLogicPoint(mob::Logic& g, const LogicPointRef& r, const mob::Vec3& v);
void DeleteLogicPoints(App& app);
void Duplicate(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    std::vector<App::Copied> items;
    for (int oi : app.scene.selection) items.push_back({mob::ObjectNode(f, oi), f.objects[oi].position});
    if (InsertCopies(app, items, true, {}) && app.viewHovered)
        StartTransform(app, Transform::Move, ImGui::GetIO().MousePos, app.viewMin, app.viewSize);
}

// The objects whose figure the editor cannot show: listed; a click selects and shows one.
void MissingWindow(App& app) {
    if (++app.missingCheck % 60 == 1 && app.scene.modelsPending == 0) app.missing = app.scene.MissingFigures();
    if (!app.missingOpen) return;
    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_Appearing);
    if (!ImGui::Begin("Figures that cannot be shown", &app.missingOpen)) { ImGui::End(); return; }
    if (app.missing.empty()) ImGui::TextDisabled("None.");
    if (ImGui::BeginTable("##miss", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Map"); ImGui::TableSetupColumn("Object"); ImGui::TableSetupColumn("Figure"); ImGui::TableSetupColumn("Why");
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < app.missing.size(); ++k) {
            const MapScene::Missing& mi = app.missing[k];
            if (mi.file < 0 || mi.file >= static_cast<int>(app.mobs.size())) continue;
            const mob::File& f = app.mobs[static_cast<size_t>(mi.file)]->file;
            if (mi.object >= static_cast<int>(f.objects.size())) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((f.fileName + "##mi" + std::to_string(k)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) SelectObject(app, mi.file, mi.object, true);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(checks::Label(f.objects[static_cast<size_t>(mi.object)]).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mi.figure.empty() ? "-" : mi.figure.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mi.error.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static const char* FigureCategory(const std::string& lowerName) { return objects::Category(lowerName); }
static void BuildFigureTextures(App& app) {
    std::vector<const mob::File*> loaded;
    for (const auto& m : app.mobs) loaded.push_back(&m->file);
    objects::Worn().Build(app.lib, loaded);
}
static std::string SuitedTexture(const App& app, const std::string& figure) { return objects::Worn().Suited(app.lib, figure); }

// What the preview shows: "o:<figure>|<texture>" or "u:<unit>".
static std::string PreviewKey(const App& app) {
    if (!app.newPreview) return "";
    if (app.newTab == 1) return app.newUnit.empty() ? "" : "u:" + app.newUnit;
    return app.newFigure[0] ? std::string("o:") + app.newFigure + "|" + app.newTexture : "";
}

// Draws the preview into the back buffer's corner (RenderGl, before the map paints over it) and keeps it as a texture.
void RenderPreview(App& app) {
    if (!app.newOpen || app.previewWanted.empty()) return;
    const int size = 256;
    if (!app.preview) app.preview = std::make_unique<Scene>();
    Scene& sc = *app.preview;
    if (app.previewShown != app.previewWanted) {
        if (app.previewWanted.rfind("u:", 0) == 0) {
            mob::Object o;
            o.kind = mob::Kind::Unit;
            o.prototype = ToCp(app.previewWanted.substr(2));
            o.primTexture = "default0";
            const units::Monster* m = app.lib.unitsDb.FindMonster(app.previewWanted.substr(2));
            const units::Race* race = m ? app.lib.unitsDb.FindRace(m->race) : nullptr;
            o.templ = race ? race->mask : "";
            dress::Dress d = dress::Resolve(app.lib, o);
            if (!d.on) sc.LoadModel(app.lib, o.templ, true);
            else {
                sc.LoadUnit(app.lib, o.templ, fig::Vec3{0.5f, 0.5f, 0.5f}, true,
                            [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartShown(d, model, part); },
                            [&](const fig::Model& model, const fig::ModelPart& part) { return dress::PartTexture(d, model, part); });
                sc.textureName = d.skin;
            }
        } else {
            const std::string v = app.previewWanted.substr(2);
            const size_t bar = v.find('|');
            const std::string figure = v.substr(0, bar);
            sc.LoadModel(app.lib, figure, true);
            sc.textureName = bar == std::string::npos ? "" : v.substr(bar + 1);
            sc.options.atlasUvs = Lower(figure).rfind("in", 0) == 0; // item figures use the 256 atlas, scenery its texture directly
            if (sc.textureName.empty()) sc.textureName = SuitedTexture(app, figure);
        }
        app.previewShown = app.previewWanted;
    }
    sc.options.grid = false;
    sc.camera.yawDeg = app.previewYaw;
    glDisable(GL_SCISSOR_TEST);
    sc.Draw(app.lib, 0, 0, size, size, 0.0f);
    glFinish();
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4), flipped(rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (int y = 0; y < size; ++y) std::memcpy(&flipped[static_cast<size_t>(y) * size * 4], &rgba[static_cast<size_t>(size - 1 - y) * size * 4], static_cast<size_t>(size) * 4);
    if (!app.previewTex) glGenTextures(1, &app.previewTex);
    glBindTexture(GL_TEXTURE_2D, app.previewTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
}

// The Add object / unit window: a figure (by category, searchable) or a database unit (dressed as the game does),
// a 3D preview, then a copy of a pattern of that kind (the selected object, else the active map's first one) with
// the choice applied, placed under the mouse (else the view's centre).
void NewObjectWindow(App& app) {
    if (!app.newOpen) return;
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 60, app.viewportMin.y + 60), ImGuiCond_Appearing);
    if (!ImGui::Begin("Add object / unit", &app.newOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("Load a map first."); ImGui::End(); return; }
    BuildFigureTextures(app);
    const mob::File& f = app.mobs[app.activeMob]->file;
    if (ImGui::BeginTabBar("##newtabs")) {
        if (ImGui::BeginTabItem("Objects")) { app.newTab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Units")) { app.newTab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    const mob::Kind want = app.newTab == 1 ? mob::Kind::Unit : mob::Kind::Object;
    int pattern = -1;
    if (app.scene.selectedFile == app.activeMob && app.scene.selectedObject >= 0 && f.objects[app.scene.selectedObject].kind == want)
        pattern = app.scene.selectedObject;
    for (size_t i = 0; pattern < 0 && i < f.objects.size(); ++i) if (f.objects[i].kind == want) pattern = static_cast<int>(i);

    ImGui::BeginChild("##pick", ImVec2(330, -ImGui::GetFrameHeightWithSpacing() * 2), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##figfilter", app.newTab == 1 ? "search the units" : "search the figures", app.newFilter, sizeof(app.newFilter));
    const std::string filter = Lower(app.newFilter);
    if (app.newTab == 0) {
        static const char* const kCats[] = {"All", "Buildings", "Walls & fences", "Bridges", "Structures", "Statues & ruins", "Ruins & props", "Plants",
                                            "Trees", "Stones", "Nature", "Items & props", "Containers", "Creature figures", "Effects & markers", "Other"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##cat", &app.newCategory, kCats, IM_ARRAYSIZE(kCats));
        if (ImGui::BeginListBox("##figures", ImVec2(-1, -1))) {
            int shown = 0;
            for (const std::string& name : app.lib.figureIndex.baseNames) {
                const std::string low = Lower(name);
                if (!filter.empty() && low.find(filter) == std::string::npos) continue;
                if (app.newCategory > 0 && std::string(FigureCategory(low)) != kCats[app.newCategory]) continue;
                if (++shown > 600) { ImGui::TextDisabled("(more: search to narrow)"); break; }
                if (ImGui::Selectable(name.c_str(), name == app.newFigure)) { std::snprintf(app.newFigure, sizeof(app.newFigure), "%s", name.c_str()); app.newTexture[0] = '\0'; }
            }
            ImGui::EndListBox();
        }
    } else {
        if (app.lib.unitsDb.monsters.empty()) ImGui::TextDisabled("No database set in Settings: no units to pick.");
        if (ImGui::BeginListBox("##units", ImVec2(-1, -1))) {
            int shown = 0;
            for (const units::Monster& m : app.lib.unitsDb.monsters) {
                if (!filter.empty() && Lower(m.name).find(filter) == std::string::npos) continue;
                if (++shown > 800) { ImGui::TextDisabled("(more: search to narrow)"); break; }
                if (ImGui::Selectable(m.name.c_str(), m.name == app.newUnit)) app.newUnit = m.name;
            }
            ImGui::EndListBox();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Checkbox("3D preview", &app.newPreview);
    app.previewWanted = PreviewKey(app);
    if (app.newPreview && app.previewTex && !app.previewWanted.empty()) {
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(app.previewTex)), ImVec2(256, 256));
        ImGui::SetNextItemWidth(256);
        ImGui::SliderFloat("##yaw", &app.previewYaw, -180.0f, 180.0f, "turn %.0f");
    } else {
        ImGui::Dummy(ImVec2(256, 256));
    }
    if (app.newTab == 0) {
        ImGui::TextUnformatted("Texture");
        objects::TextureCombo(app.lib, app.newFigure, app.newTexture, sizeof(app.newTexture), 256);
    }
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(256);
    ImGui::InputTextWithHint("##name", pattern >= 0 ? mob::Utf8(f.objects[pattern].name).c_str() : "", app.newName, sizeof(app.newName));
    ImGui::EndGroup();

    if (pattern < 0) ImGui::TextDisabled(app.newTab == 1 ? "The active map has no unit to copy the other fields from." : "The active map has no object to copy the other fields from.");
    else ImGui::TextDisabled("Other fields from: %s (select another %s to use it)", checks::Label(f.objects[pattern]).c_str(), app.newTab == 1 ? "unit" : "object");
    const bool ready = pattern >= 0 && (app.newTab == 1 ? !app.newUnit.empty() : app.newFigure[0] != '\0');
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Add", ImVec2(120, 0))) {
        const mob::Object& p = f.objects[pattern];
        std::vector<uint8_t> node = mob::ObjectNode(f, pattern);
        std::string what;
        if (app.newTab == 1) {
            const units::Monster* m = app.lib.unitsDb.FindMonster(app.newUnit);
            const units::Race* race = m ? app.lib.unitsDb.FindRace(m->race) : nullptr;
            mob::NodeReplaceField(node, mob::kUnitPrototype, mob::TextPayload(ToCp(app.newUnit)));
            if (race && !race->mask.empty()) mob::NodeReplaceField(node, mob::kObjTemplate, mob::TextPayload(race->mask));
            mob::NodeReplaceField(node, mob::kObjPrimTexture, mob::TextPayload("default0")); // dressed from the database
            what = app.newUnit;
        } else {
            mob::NodeReplaceField(node, mob::kObjTemplate, mob::TextPayload(app.newFigure));
            if (app.newTexture[0]) mob::NodeReplaceField(node, mob::kObjPrimTexture, mob::TextPayload(app.newTexture));
            what = app.newFigure;
        }
        if (app.newName[0]) mob::NodeReplaceField(node, mob::kObjName, mob::TextPayload(ToCp(app.newName)));
        if (InsertCopies(app, {{node, p.position}}, false, PlacePoint(app))) app.newMessage = "Added " + what + " (G to move it)";
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("At the ground under the mouse, else the view's centre; press G to move it");
    if (!app.newMessage.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", app.newMessage.c_str()); }
    ImGui::End();
}

// Ctrl+A: everything in the active map that can be selected (only units in logic mode).
void SelectAll(App& app) {
    if (app.mobs.empty()) return;
    const auto& objects = app.mobs[app.activeMob]->file.objects;
    app.scene.ClearSelection();
    for (size_t i = 0; i < objects.size(); ++i) {
        if (app.scene.logicMode && objects[i].kind != mob::Kind::Unit) continue;
        if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, static_cast<int>(i));
        else { app.scene.selection.push_back(static_cast<int>(i)); app.scene.selectedObject = static_cast<int>(i); }
    }
}

// Moves every selected object along one axis by exactly the typed value (positive or negative). A
// figure's z is its height above the ground, a light's, particle's or sound's its absolute height.
void OffsetWindow(App& app) {
    if (!app.offsetOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_Appearing);
    if (!ImGui::Begin("Offset the selection", &app.offsetOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    const bool ok = !app.mobs.empty() && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    if (ok) ImGui::Text("%zu object(s) selected in %s", app.scene.selection.size(), app.mobs[app.activeMob]->file.fileName.c_str());
    else ImGui::TextDisabled("Select objects in the active map first.");
    ImGui::TextUnformatted("Axis");
    for (int a = 0; a < 3; ++a) {
        ImGui::SameLine();
        ImGui::RadioButton(a == 0 ? "X" : a == 1 ? "Y" : "Z", &app.offsetAxis, a);
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputDouble("by", &app.offsetValue, 1.0, 10.0, "%.4f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negative to move the other way. Added to each object's own position.");
    ImGui::BeginDisabled(!ok || app.offsetValue == 0.0);
    if (ImGui::Button("Apply", ImVec2(120, 0))) {
        mob::File& f = app.mobs[app.activeMob]->file;
        EditStep step;
        step.kind = EditStep::Objects;
        step.file = f.path;
        step.objects = CaptureObjects(f, app.scene.selection);
        std::vector<ObjectState> now = step.objects;
        const double v = app.offsetValue;
        for (ObjectState& st : now) { // in double, then stored as the file's float
            float* c = app.offsetAxis == 0 ? &st.position.x : app.offsetAxis == 1 ? &st.position.y : &st.position.z;
            *c = static_cast<float>(static_cast<double>(*c) + v);
        }
        PushUndo(app, std::move(step), "Offset " + std::to_string(now.size()) + " object(s)");
        ApplyObjects(f, now);
        app.checksDirty = true;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "Moved %zu object(s) by %+.4f on %c (undo: %s)", now.size(), v, "XYZ"[app.offsetAxis],
                      ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
        app.offsetMessage = msg;
    }
    ImGui::EndDisabled();
    if (!app.offsetMessage.empty()) ImGui::TextDisabled("%s", app.offsetMessage.c_str());
    ImGui::End();
}

// ---- randomize ------------------------------------------------------------------------------------------
// Like ei_maper's "Randomize parameter": one parameter of each selected object gets its own random value
// in a range, or is moved by one. Rotations in degrees; one undo step per click.

static const char* const kRandomParams[] = {"Position X", "Position Y", "Position Z (above ground)", "Rotation about X", "Rotation about Y",
                                             "Rotation about Z (turn)", "Complection X", "Complection Y", "Complection Z", "Complection X Y Z (together)"};

// Euler angles (radians, about X then Y then Z) of a quaternion w x y z, and back.
static void QuatToEuler(const float q[4], float& rx, float& ry, float& rz) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    rx = std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y));
    ry = std::asin(std::max(-1.0f, std::min(1.0f, 2.0f * (w * y - z * x))));
    rz = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
}
static void EulerToQuat(float rx, float ry, float rz, float q[4]) {
    const float cr = std::cos(rx / 2), sr = std::sin(rx / 2), cp = std::cos(ry / 2), sp = std::sin(ry / 2), cy = std::cos(rz / 2), sy = std::sin(rz / 2);
    q[0] = cr * cp * cy + sr * sp * sy;
    q[1] = sr * cp * cy - cr * sp * sy;
    q[2] = cr * sp * cy + sr * cp * sy;
    q[3] = cr * cp * sy - sr * sp * cy;
}

void RandomizeWindow(App& app) {
    if (!app.randomOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 50, app.viewportMin.y + 70), ImGuiCond_Appearing);
    if (!ImGui::Begin("Randomize", &app.randomOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    const bool ok = !app.mobs.empty() && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    if (ok) ImGui::Text("%zu object(s) selected in %s", app.scene.selection.size(), app.mobs[app.activeMob]->file.fileName.c_str());
    else ImGui::TextDisabled("Select objects in the active map first.");
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Parameter", &app.randomParam, kRandomParams, IM_ARRAYSIZE(kRandomParams));
    ImGui::RadioButton("Set to a random value", &app.randomMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Add a random value", &app.randomMode, 1);
    const bool rotation = app.randomParam >= 3 && app.randomParam <= 5;
    ImGui::SetNextItemWidth(120);
    ImGui::InputFloat("min", &app.randomMin, 0, 0, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputFloat("max", &app.randomMax, 0, 0, "%.3f");
    if (rotation) { ImGui::SameLine(); ImGui::TextDisabled("degrees"); }
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Randomize", ImVec2(120, 0))) {
        static std::mt19937 rng{std::random_device{}()};
        const float lo = std::min(app.randomMin, app.randomMax), hi = std::max(app.randomMin, app.randomMax);
        std::uniform_real_distribution<float> dist(lo, hi);
        mob::File& f = app.mobs[app.activeMob]->file;
        EditStep step;
        step.kind = EditStep::Objects;
        step.file = f.path;
        step.objects = CaptureObjects(f, app.scene.selection);
        std::vector<ObjectState> now = step.objects;
        const bool add = app.randomMode == 1;
        int done = 0;
        for (ObjectState& st : now) {
            const mob::Object& o = f.objects[st.index];
            const bool figure = mob::HasFigure(o.kind);
            const float v = dist(rng);
            auto apply = [&](float& field) { field = add ? field + v : v; };
            switch (app.randomParam) {
            case 0: apply(st.position.x); break;
            case 1: apply(st.position.y); break;
            case 2: apply(st.position.z); break;
            case 3: case 4: case 5: {
                if (!figure) continue;
                const int axis = app.randomParam - 3;
                const float rad = v * 3.14159265f / 180.0f;
                if (add) { // turned about the world axis, like R
                    const float h = rad / 2;
                    const fig::Quat q{std::cos(h), axis == 0 ? std::sin(h) : 0.0f, axis == 1 ? std::sin(h) : 0.0f, axis == 2 ? std::sin(h) : 0.0f};
                    fig::Quat r = fig::QuatNormalize(fig::QuatMul(q, fig::Quat{st.rotation[0], st.rotation[1], st.rotation[2], st.rotation[3]}));
                    st.rotation[0] = r.w; st.rotation[1] = r.x; st.rotation[2] = r.y; st.rotation[3] = r.z;
                } else { // that angle set, the other two kept
                    float e[3];
                    QuatToEuler(st.rotation, e[0], e[1], e[2]);
                    e[axis] = rad;
                    EulerToQuat(e[0], e[1], e[2], st.rotation);
                }
                break;
            }
            case 6: if (!figure) continue; apply(st.complection.x); break;
            case 7: if (!figure) continue; apply(st.complection.y); break;
            case 8: if (!figure) continue; apply(st.complection.z); break;
            default: if (!figure) continue; apply(st.complection.x); apply(st.complection.y); apply(st.complection.z); break;
            }
            ++done;
        }
        PushUndo(app, std::move(step), std::string("Randomize ") + kRandomParams[app.randomParam]);
        ApplyObjects(f, now);
        app.checksDirty = true;
        app.randomMessage = "Randomized " + std::string(kRandomParams[app.randomParam]) + " of " + std::to_string(done) + " object(s) (undo: " +
                            ui::BindName(app.lib.mapKeys[config::kKeyUndo]) + ")";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Each object gets its own value between min and max. Rotation and complection apply to figures only.");
    if (!app.randomMessage.empty()) ImGui::TextDisabled("%s", app.randomMessage.c_str());
    ImGui::End();
}

void FindWindow(App& app) {
    if (!app.findOpen) return;
    ImGui::SetNextWindowSize(ImVec2(440, 460), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 30, app.viewportMin.y + 50), ImGuiCond_Appearing);
    if (!ImGui::Begin("Find objects", &app.findOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    ImGui::TextDisabled("In the active map: %s", app.mobs[app.activeMob]->file.fileName.c_str());
    if (app.findFocus) { ImGui::SetKeyboardFocusHere(); app.findFocus = false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##findtext", "name; wildcards: * any, ? one character (e.g. Orc*-1?7*)", app.findText, sizeof(app.findText));
    ImGui::Checkbox("Case sensitive", &app.findCase);
    ImGui::SameLine();
    ImGui::Checkbox("Also figure, prototype, template", &app.findMore);
    static const char* kinds[] = {"All kinds", "World objects", "Units", "Levers", "Torches", "Magic traps", "Lights", "Particles", "Sounds"};
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##findkind", &app.findKind, kinds, IM_ARRAYSIZE(kinds));
    ImGui::SameLine();
    ImGui::Checkbox("ID from", &app.findIds);
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.findIds);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##idmin", &app.findIdMin, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##idmax", &app.findIdMax, 0);
    ImGui::EndDisabled();

    const auto& objects = app.mobs[app.activeMob]->file.objects;
    std::vector<int> found;
    for (size_t i = 0; i < objects.size(); ++i) {
        const mob::Object& o = objects[i];
        if (app.scene.logicMode && o.kind != mob::Kind::Unit) continue; // logic mode selects units only
        if (FindMatches(app, o)) found.push_back(static_cast<int>(i));
    }
    ImGui::Text("%zu found", found.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(found.empty());
    if (ImGui::Button("Select all matching")) {
        app.scene.ClearSelection();
        for (int oi : found) {
            if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
            else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
        }
        app.findMessage = std::to_string(found.size()) + " selected";
        app.requestTab = SideTab::Objects;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to selection")) {
        int added = 0;
        for (int oi : found) {
            if (app.scene.IsSelected(app.activeMob, oi)) continue;
            if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
            else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
            ++added;
        }
        app.findMessage = std::to_string(added) + " added";
    }
    ImGui::EndDisabled();
    if (!app.findMessage.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", app.findMessage.c_str()); }
    if (ImGui::BeginTable("##found", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(found.size()));
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const int oi = found[r];
                const mob::Object& o = objects[oi];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(oi);
                if (ImGui::Selectable(ObjectLabel(o).c_str(), app.scene.IsSelected(app.activeMob, oi),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::GetIO().KeyShift) app.scene.Toggle(app.activeMob, oi);
                    else SelectObject(app, app.activeMob, oi, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || // a double click, or Enter
                                                              ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
                } else if (ui::NavMovedHere()) {
                    SelectObject(app, app.activeMob, oi, false); // Up/Down select (the data shows), without moving the view
                } else if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::GetIO().WantTextInput &&
                           !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt) {
                    SelectObject(app, app.activeMob, oi, true); // F: focus the view on it, as a double click does
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", mob::KindName(o.kind));
                ImGui::TableNextColumn();
                if (o.hasId) ImGui::TextDisabled("%u", o.id);
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------------
// Diplomacy: the active map's 32 x 32 table between player groups (DIPLOMATION), as ei_maper edits it:
// 0 friend (green), 1 neutral (yellow), 2 enemy (red); a double-click cycles the value, both ways by default.
// Cell [row r][column c] = what group r thinks of group c; the mirror cell [c][r] is the opposite opinion.
// ------------------------------------------------------------------------------------------------

static ImVec4 DiplomacyColor(int v) {
    return v == 0 ? ImVec4(0.35f, 0.8f, 0.35f, 1) : v == 1 ? ImVec4(0.9f, 0.8f, 0.25f, 1) : v == 2 ? ImVec4(0.9f, 0.3f, 0.25f, 1) : ImVec4(0.55f, 0.55f, 1, 1);
}

static std::string DiplomacyWord(int v) {
    return v == 0 ? i18n::Tr("friend") : v == 1 ? i18n::Tr("neutral") : v == 2 ? i18n::Tr("enemy") : std::string(i18n::Tr("value")) + " " + std::to_string(v);
}

// The groups that group `g` thinks of as value `v` (row) or that think of `g` as `v` (column), as "0 3 12".
static std::string DiplomacyList(const mob::File& f, int g, int v, bool row) {
    std::string out;
    for (int o = 0; o < 32; ++o) {
        if (o == g) continue;
        if (f.diplomacy[static_cast<size_t>(row ? g : o) * 32 + (row ? o : g)] == v) out += (out.empty() ? "" : " ") + std::to_string(o);
    }
    return out.empty() ? "-" : out;
}

// A translated printf-style text (the format is looked up in the language table first).
static std::string TrFmt(const char* english, ...) {
    char buf[512];
    va_list args;
    va_start(args, english);
    std::vsnprintf(buf, sizeof(buf), i18n::Tr(english), args);
    va_end(args);
    return buf;
}

void DiplomacyTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    mob::File& f = m.file;
    ImGui::Text("%s", f.fileName.c_str());
    if (!f.hasDiplomacy) { ImGui::TextDisabled("This map has no diplomacy table (quest maps use their base map's)."); return; }
    auto name = [&](int i) -> std::string {
        if (i < static_cast<int>(f.diplomacyNames.size()) && !f.diplomacyNames[i].empty()) return mob::Utf8(f.diplomacyNames[i]);
        return std::string(i18n::Tr("group")) + " " + std::to_string(i);
    };
    auto at = [&](int r, int c) { return f.diplomacy[static_cast<size_t>(r) * 32 + c]; };

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("A row is the group that has the opinion, a column the group it has the opinion about: row 0, column 3 is what group 0 thinks of group 3. The mirror cell (row 3, column 0) is the opposite opinion, what group 3 thinks of group 0: the two may differ.");
    ImGui::PopStyleColor();
    ImGui::Checkbox("Both ways", &app.dipSymmetric);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: a change is written to the cell and to its mirror cell, so the two groups think the same of each other.\nOff: only the cell you change; group 0 can be hostile to group 3 while group 3 stays friendly to group 0.");
    ImGui::SameLine();
    ImGui::Checkbox("Mark pairs that differ", &app.dipMarkAsym);
    int differing = 0;
    for (int r = 0; r < 32; ++r)
        for (int c = r + 1; c < 32; ++c) differing += at(r, c) != at(c, r);
    // the legend
    for (int v = 0; v < 3; ++v) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 2, p.y + h), ImGui::GetColorU32(DiplomacyColor(v)));
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextColored(DiplomacyColor(v), "%s", DiplomacyWord(v).c_str());
        ImGui::SameLine();
    }
    ImGui::TextDisabled("(%d pair(s) differ)", differing);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::Tr("A pair is two groups: the cell and its mirror cell. A white corner mark on a cell means the mirror cell has another value.\nScripts read the same table: GetDiplomacy returns 1 ally / 0 neutral / -1 enemy."));
    if (app.dipSelected >= 0) {
        int count = 0;
        for (const mob::Object& o : f.objects) if (o.kind == mob::Kind::Unit && o.player == app.dipSelected) ++count;
        ImGui::Text("Selected: group %d", app.dipSelected);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d unit%s of this map; marked in the view)", count, count == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) app.dipSelected = -1;
    }

    // What the hovered cell says, in words, above the grid so that it is in view (drawn once the hover is known; four lines always).
    const float lineH = ImGui::GetFontSize() + 3.0f;
    const ImVec2 infoPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(1, lineH * 4 + 4));

    const float headX = 44.0f, headY = 40.0f;
    const float cell = std::max(10.0f, std::min(22.0f, (ImGui::GetContentRegionAvail().x - headX - 8.0f) / 32.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dipgrid", ImVec2(headX + cell * 32, headY + cell * 32), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hr = -1, hc = -1, headRow = -1, headCol = -1; // the cell under the mouse, or the row / column number under it
    if (hovered) {
        const int c = static_cast<int>(std::floor((mouse.x - origin.x - headX) / cell)), r = static_cast<int>(std::floor((mouse.y - origin.y - headY) / cell));
        if (c >= 0 && c < 32 && r >= 0 && r < 32) { hr = r; hc = c; }
        else if (mouse.x < origin.x + headX && r >= 0 && r < 32 && mouse.y >= origin.y + headY) headRow = r;
        else if (mouse.y < origin.y + headY && c >= 0 && c < 32 && mouse.x >= origin.x + headX) headCol = c;
    }
    const int focusRow = hr >= 0 ? hr : headRow, focusCol = hr >= 0 ? hc : headCol; // what is lit: the hovered cell's row and column (or the header's)
    // the axes: who thinks (rows, down the left) about whom (columns, along the top)
    draw->AddText(ImVec2(origin.x + headX, origin.y + 2), IM_COL32(200, 200, 205, 255), i18n::Tr("about whom (column) ->"));
    draw->AddText(ImVec2(origin.x + 2, origin.y + headY - 2 * ImGui::GetFontSize() - 2), IM_COL32(200, 200, 205, 255), i18n::Tr("who"));
    draw->AddText(ImVec2(origin.x + 2, origin.y + headY - ImGui::GetFontSize() - 1), IM_COL32(200, 200, 205, 255), i18n::Tr("(row)"));
    for (int i = 0; i < 32; ++i) {
        char n[8];
        std::snprintf(n, sizeof(n), "%d", i);
        const ImU32 col = i == app.dipSelected ? IM_COL32(255, 120, 255, 255) : (i == focusRow || i == focusCol) ? IM_COL32(255, 225, 130, 255) : IM_COL32(170, 170, 175, 255);
        // Column numbers: every other one when two digits do not fit a cell.
        if (i < 10 || ImGui::CalcTextSize(n).x + 2 <= cell || i % 2 == 0) draw->AddText(ImVec2(origin.x + headX + i * cell + 1, origin.y + headY - ImGui::GetFontSize() - 2), col, n);
        draw->AddText(ImVec2(origin.x + headX - ImGui::CalcTextSize(n).x - 4, origin.y + headY + i * cell + (cell - ImGui::GetFontSize()) * 0.5f), col, n);
    }
    for (int r = 0; r < 32; ++r)
        for (int c = 0; c < 32; ++c) {
            const int v = at(r, c);
            const ImU32 col = r == c ? IM_COL32(60, 60, 64, 255) : v == 0 ? IM_COL32(40, 130, 50, 255) : v == 1 ? IM_COL32(170, 150, 40, 255)
                                                           : v == 2 ? IM_COL32(160, 45, 40, 255) : IM_COL32(90, 90, 200, 255);
            const ImVec2 a(origin.x + headX + c * cell, origin.y + headY + r * cell), b(a.x + cell - 1, a.y + cell - 1);
            draw->AddRectFilled(a, b, col);
            if (app.dipMarkAsym && r != c && v != at(c, r)) { // the mirror cell says something else
                const float t = std::max(4.0f, cell * 0.38f);
                draw->AddTriangleFilled(ImVec2(b.x - t, a.y), ImVec2(b.x, a.y), ImVec2(b.x, a.y + t), IM_COL32(255, 255, 255, 235));
            }
        }
    if (focusRow >= 0 || focusCol >= 0) { // the row and the column of what the mouse is on: a light band over each
        const ImU32 band = IM_COL32(255, 255, 255, 38);
        if (focusRow >= 0) draw->AddRectFilled(ImVec2(origin.x + headX, origin.y + headY + focusRow * cell), ImVec2(origin.x + headX + 32 * cell - 1, origin.y + headY + focusRow * cell + cell - 1), band);
        if (focusCol >= 0) draw->AddRectFilled(ImVec2(origin.x + headX + focusCol * cell, origin.y + headY), ImVec2(origin.x + headX + focusCol * cell + cell - 1, origin.y + headY + 32 * cell - 1), band);
    }
    if (hr >= 0 && hr != hc) { // the mirror cell: the opposite opinion
        const ImVec2 a(origin.x + headX + hr * cell, origin.y + headY + hc * cell);
        draw->AddRect(a, ImVec2(a.x + cell - 1, a.y + cell - 1), IM_COL32(90, 220, 255, 255), 0, 0, 2.0f);
    }
    if (hr >= 0) draw->AddRect(ImVec2(origin.x + headX + hc * cell, origin.y + headY + hr * cell), ImVec2(origin.x + headX + hc * cell + cell - 1, origin.y + headY + hr * cell + cell - 1), IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);
    if (app.dipSelected >= 0) { // the selected group's row and column
        const float g = app.dipSelected * cell;
        draw->AddRect(ImVec2(origin.x + headX - 1, origin.y + headY + g - 1), ImVec2(origin.x + headX + cell * 32, origin.y + headY + g + cell), IM_COL32(255, 120, 255, 220), 0, 0, 2.0f);
        draw->AddRect(ImVec2(origin.x + headX + g - 1, origin.y + headY - 1), ImVec2(origin.x + headX + g + cell, origin.y + headY + cell * 32), IM_COL32(255, 120, 255, 220), 0, 0, 2.0f);
    }
    if ((headRow >= 0 || headCol >= 0) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app.dipSelected = headRow >= 0 ? headRow : headCol;
    if (headRow >= 0 || headCol >= 0) ImGui::SetTooltip("%s: click to select the group", name(headRow >= 0 ? headRow : headCol).c_str());
    if (hr >= 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app.dipSelected = hr; // a click selects the row's group
        const int v = at(hr, hc);
        const bool left = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left), right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if ((left || right) && hr != hc) {
            EditStep step;
            step.kind = EditStep::Diplomacy;
            step.file = f.path;
            step.diplomacy = f.diplomacy;
            PushUndo(app, std::move(step), "Diplomacy");
            const int next = ((v + (left ? 1 : 2)) % 3 + 3) % 3; // 0 -> 1 -> 2 -> 0 (right: backwards)
            mob::SetDiplomacy(f, hr, hc, next);
            if (app.dipSymmetric) mob::SetDiplomacy(f, hc, hr, next);
        }
    }

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 plain = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        auto put = [&](ImVec2& p, const std::string& text, ImU32 col) { dl->AddText(p, col, text.c_str()); p.x += ImGui::CalcTextSize(text.c_str()).x; };
        auto line = [&](int n) { return ImVec2(infoPos.x, infoPos.y + lineH * static_cast<float>(n)); };
        if (hr >= 0 && hr != hc) {
            const int v = at(hr, hc), back = at(hc, hr), next = (v + 1) % 3;
            ImVec2 p = line(0);
            put(p, TrFmt("Group %d (%s) regards group %d (%s) as:", hr, name(hr).c_str(), hc, name(hc).c_str()) + " ", plain);
            put(p, DiplomacyWord(v), ImGui::GetColorU32(DiplomacyColor(v)));
            p = line(1);
            put(p, TrFmt("The other way round, group %d regards group %d as:", hc, hr) + " ", plain);
            put(p, DiplomacyWord(back), ImGui::GetColorU32(DiplomacyColor(back)));
            p = line(2);
            if (back == v) put(p, i18n::Tr("The same opinion both ways."), dim);
            else put(p, i18n::Tr("Different opinions (the cell has a white corner mark)."), IM_COL32(255, 255, 255, 255));
            p = line(3);
            put(p, std::string(i18n::Tr("Double-click: becomes")) + " ", dim);
            put(p, DiplomacyWord(next), ImGui::GetColorU32(DiplomacyColor(next)));
            put(p, std::string(" ") + i18n::Tr(app.dipSymmetric ? "(and the mirror cell: Both ways)" : "(the mirror cell stays)"), dim);
        } else if (hr >= 0) {
            ImVec2 p = line(0);
            put(p, TrFmt("Group %d (%s) with itself: not editable.", hr, name(hr).c_str()), plain);
        } else {
            ImVec2 p = line(0);
            put(p, i18n::Tr("Hover a cell to read it: who thinks what of whom."), dim);
        }
    }
    if (app.dipSelected >= 0) { // the selected group's whole row and column
        const int g = app.dipSelected;
        ImGui::Separator();
        ImGui::Text("Group %d (%s) regards as:", g, name(g).c_str());
        for (int v = 0; v < 3; ++v) ImGui::TextColored(DiplomacyColor(v), "  %s: %s", DiplomacyWord(v).c_str(), DiplomacyList(f, g, v, true).c_str());
        ImGui::Text("Regarded by the others as:");
        for (int v = 0; v < 3; ++v) ImGui::TextColored(DiplomacyColor(v), "  %s: %s", DiplomacyWord(v).c_str(), DiplomacyList(f, g, v, false).c_str());
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click a number or a cell: selects that group (its units are marked in the view). Double-click a cell: next value; right-click: previous. The groups' names are the map's DIPLOMATION_PL_NAMES. Save with %s, undo with %s.",
                       ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------------------------------------
// IDs: the active map's ID ranges (MAIN_RANGE / SEC_RANGE), giving the selection new IDs, copying them,
// and checking the map's IDs (repeated, shared with the other loaded maps, outside the ranges).
// ------------------------------------------------------------------------------------------------

static std::set<uint32_t> UsedIds(const App& app) {
    std::set<uint32_t> used;
    for (auto& m : app.mobs)
        for (const mob::Object& o : m->file.objects) if (o.hasId) used.insert(o.id);
    return used;
}

static void CheckIds(App& app) {
    app.idIssues.clear();
    app.idChecked = true;
    if (app.mobs.empty()) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    std::map<uint32_t, std::vector<int>> byId;
    int noId = 0;
    for (size_t i = 0; i < f.objects.size(); ++i) {
        if (f.objects[i].hasId) byId[f.objects[i].id].push_back(static_cast<int>(i));
        else ++noId;
    }
    for (auto& kv : byId)
        if (kv.second.size() > 1)
            for (int oi : kv.second) app.idIssues.push_back({"ID " + std::to_string(kv.first) + " is used " + std::to_string(kv.second.size()) + " times in this map: " + checks::Label(f.objects[oi]), oi});
    for (size_t k = 0; k < app.mobs.size(); ++k) {
        if (static_cast<int>(k) == app.activeMob) continue;
        for (const mob::Object& other : app.mobs[k]->file.objects) {
            if (!other.hasId) continue;
            auto it = byId.find(other.id);
            if (it != byId.end())
                app.idIssues.push_back({"ID " + std::to_string(other.id) + " is also used in " + app.mobs[k]->file.fileName + ": " + checks::Label(f.objects[it->second[0]]), it->second[0]});
        }
    }
    if (!f.mainRanges.empty()) {
        for (size_t i = 0; i < f.objects.size(); ++i) {
            const mob::Object& o = f.objects[i];
            if (!o.hasId) continue;
            bool inside = false;
            for (const mob::Range& r : f.mainRanges) inside |= o.id >= r.min && o.id <= r.max;
            if (!inside) app.idIssues.push_back({"ID " + std::to_string(o.id) + " is outside the map's ID ranges: " + checks::Label(o), static_cast<int>(i)});
        }
    }
    if (noId) app.idIssues.push_back({std::to_string(noId) + " object(s) without an ID field", -1});
}

void IdsTab(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    mob::File& f = m.file;
    ImGui::Text("%s", f.fileName.c_str());
    {
        const uint32_t kind = f.bytes.size() >= 16 ? mob::U32(f.bytes.data() + 8) : 0;
        bool quest = mob::IsQuestMob(f);
        ImGui::BeginDisabled(kind != mob::kBaseMob && kind != mob::kQuestMob);
        if (ImGui::Checkbox("Quest MOB", &quest)) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::SetQuestMob(f, quest)) { PushUndo(app, std::move(step), quest ? "Quest MOB on" : "Quest MOB off"); app.checksDirty = true; }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("PR_OBJECT_DB_FILE: a quest map, loaded over a zone (ei_maper's \"is Quest Mob?\"): no WORLD_SET, uses SEC_RANGE.\n"
                "Off, SC_OBJECT_DB_FILE: a zone's own map, with WORLD_SET (time, ambient, sun, wind), usually MAIN_RANGE.");
        ImGui::SameLine();
        ImGui::TextDisabled(f.hasWorld ? "(has WORLD_SET)" : "(no WORLD_SET)");
    }

    ImGui::SeparatorText("ID ranges");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The IDs this map's objects may use (MAIN_RANGE), and SEC_RANGE. New IDs are taken from them.");
    ImGui::PopStyleColor();
    auto rangeRows = [&](std::vector<mob::Range>& ranges, const char* label) {
        for (size_t i = 0; i < ranges.size(); ++i) {
            ImGui::PushID(label);
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s %zu", label, i + 1);
            ImGui::SameLine(110);
            uint32_t v[2] = {ranges[i].min, ranges[i].max};
            ImGui::SetNextItemWidth(220);
            static std::map<ImGuiID, std::array<uint32_t, 2>> pending;
            const ImGuiID key = ImGui::GetID("##r");
            auto it = pending.find(key);
            if (it != pending.end()) { v[0] = it->second[0]; v[1] = it->second[1]; }
            ImGui::InputScalarN("##r", ImGuiDataType_U32, v, 2);
            if (ImGui::IsItemActive()) pending[key] = {v[0], v[1]};
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                pending.erase(key);
                if (v[0] <= v[1] && (v[0] != ranges[i].min || v[1] != ranges[i].max)) {
                    EditStep step;
                    step.kind = EditStep::Bytes;
                    step.file = f.path;
                    step.bytes = f.bytes;
                    PushUndo(app, std::move(step), "ID range");
                    mob::SetRange(f, ranges[i], v[0], v[1]);
                    app.checksDirty = true;
                } else if (v[0] > v[1]) {
                    app.idMessage = "A range's start must not be past its end";
                }
            } else if (!ImGui::IsItemActive()) {
                pending.erase(key);
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    };
    rangeRows(f.mainRanges, "Main");
    rangeRows(f.secRanges, "Sec");
    if (f.mainRanges.empty() && f.secRanges.empty()) ImGui::TextDisabled("(this map has no ID ranges)");

    ImGui::SeparatorText("The selection");
    const bool sel = app.scene.selectedFile == app.activeMob && !app.scene.selection.empty();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Copy IDs")) {
        std::string ids;
        for (int oi : app.scene.selection)
            if (f.objects[oi].hasId) ids += (ids.empty() ? "" : ", ") + std::to_string(f.objects[oi].id);
        ImGui::SetClipboardText(ids.c_str());
        app.idMessage = "Copied the IDs of the selection";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("To the clipboard, separated by commas");
    ImGui::EndDisabled();
    // New IDs: the lowest ones from the chosen start that no loaded map uses, within the range.
    std::vector<std::string> rangeNames;
    for (const mob::Range& r : f.mainRanges) rangeNames.push_back(std::to_string(r.min) + " - " + std::to_string(r.max));
    if (rangeNames.empty()) rangeNames.push_back("1 - 4294967295 (no ranges)");
    app.idRange = std::min(std::max(app.idRange, 0), static_cast<int>(rangeNames.size()) - 1);
    const uint32_t lo = f.mainRanges.empty() ? 1 : f.mainRanges[app.idRange].min;
    const uint32_t hi = f.mainRanges.empty() ? 0xFFFFFFFFu : f.mainRanges[app.idRange].max;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("New IDs in");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##idrange", rangeNames[app.idRange].c_str())) {
        for (size_t i = 0; i < rangeNames.size(); ++i)
            if (ImGui::Selectable(rangeNames[i].c_str(), static_cast<int>(i) == app.idRange)) { app.idRange = static_cast<int>(i); app.idStart = 0; }
        ImGui::EndCombo();
    }
    if (app.idStart < lo || app.idStart > hi) app.idStart = lo;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("from");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputScalar("##idstart", ImGuiDataType_U32, &app.idStart);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Reset selected IDs")) {
        std::set<uint32_t> used = UsedIds(app);
        for (int oi : app.scene.selection) if (f.objects[oi].hasId) used.erase(f.objects[oi].id); // their own IDs may be reused
        std::vector<int> targets;
        for (int oi : app.scene.selection) if (f.objects[oi].hasId && f.objects[oi].idAt) targets.push_back(oi);
        std::sort(targets.begin(), targets.end());
        std::vector<uint32_t> ids;
        uint64_t next = std::max(app.idStart, lo);
        while (ids.size() < targets.size() && next <= hi) {
            if (!used.count(static_cast<uint32_t>(next))) ids.push_back(static_cast<uint32_t>(next));
            ++next;
        }
        if (ids.size() < targets.size()) {
            app.idMessage = "Not enough free IDs in that range from " + std::to_string(app.idStart);
        } else {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            PushUndo(app, std::move(step), "New IDs");
            for (size_t k = 0; k < targets.size(); ++k) mob::SetId(f, f.objects[targets[k]], ids[k]);
            app.checksDirty = true;
            app.listedKey.clear();
            app.idMessage = "Gave " + std::to_string(targets.size()) + " object(s) the IDs " + std::to_string(ids.front()) +
                            (ids.size() > 1 ? " to " + std::to_string(ids.back()) : "");
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Gives each selected object the next ID no loaded map uses (scripts naming the old IDs\n"
            "won't find them: the Checks tab lists those).");

    ImGui::SeparatorText("Check IDs");
    if (ImGui::Button("Check IDs")) CheckIds(app);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("IDs used twice in this map, also used by another loaded map, or outside the map's ranges");
    if (!app.idMessage.empty()) { ImGui::SameLine(); ImGui::TextWrapped("%s", app.idMessage.c_str()); }
    if (app.idChecked) {
        if (app.idIssues.empty()) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "No ID problems.");
        for (size_t i = 0; i < app.idIssues.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto& issue = app.idIssues[i];
            if (ImGui::Selectable(issue.text.c_str()) && issue.object >= 0 && issue.object < static_cast<int>(f.objects.size()))
                SelectObject(app, app.activeMob, issue.object, true);
            ImGui::PopID();
        }
    }
}


} // namespace mapedit
