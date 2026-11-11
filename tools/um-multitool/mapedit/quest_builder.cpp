// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Quest Builder main tab: a step-by-step wizard (Map, Story, Units, Objectives, Rewards, Review) that writes a quest
// (<name>.mq and <name>.mob, quest_draft.hpp) to a folder, with the map's picture to place things on and the difficulty
// rating (map_rating.cpp) of the draft shown as it grows.
#include "map_internal.hpp"

namespace mapedit {

namespace {

constexpr ImVec4 kWarn(0.94f, 0.69f, 0.31f, 1);

const char* const kSteps[] = {"Map", "Story", "Units", "Objectives", "Rewards", "Review"};
constexpr int kStepCount = 6;

// ---- small widgets --------------------------------------------------------------------------------------------

bool Edit(const char* label, std::string& s, int cap = 256, float width = -1) {
    std::vector<char> buf(static_cast<size_t>(cap), 0);
    std::snprintf(buf.data(), buf.size(), "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputText(label, buf.data(), buf.size());
    if (changed) s = buf.data();
    return changed;
}

bool EditMulti(const char* label, std::string& s, int cap, float height) {
    std::vector<char> buf(static_cast<size_t>(cap), 0);
    std::snprintf(buf.data(), buf.size(), "%s", s.c_str());
    const bool changed = ImGui::InputTextMultiline(label, buf.data(), buf.size(), ImVec2(-1, height));
    if (changed) s = buf.data();
    return changed;
}

std::string Num0(float v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v); return b; }

// ---- the zone ------------------------------------------------------------------------------------------------------

// The terrain seen from above: height shading and the water, the map in the picture's top left corner (y = 0 at the top, as the game's quest maps).
void MakePicture(BuilderUi& b) {
    const int P = 512;
    const float L = std::max(b.mapW, b.mapH);
    std::vector<uint8_t> rgba(static_cast<size_t>(P) * P * 4, 0);
    const mpr::Map& m = b.terrain;
    const float maxZ = std::max(m.maxZ, 1.0f);
    for (int py = 0; py < P; ++py)
        for (int px = 0; px < P; ++px) {
            const float wx = (px + 0.5f) / P * L, wy = (py + 0.5f) / P * L;
            uint8_t* o = &rgba[(static_cast<size_t>(py) * P + px) * 4];
            if (wx >= b.mapW || wy >= b.mapH) { o[0] = o[1] = o[2] = 22; o[3] = 255; continue; }
            const float h = m.HeightAt(wx, wy) / maxZ;
            const float gx = (m.HeightAt(wx + 1.5f, wy) - m.HeightAt(wx - 1.5f, wy)) / maxZ * 40.0f;
            const float gy = (m.HeightAt(wx, wy + 1.5f) - m.HeightAt(wx, wy - 1.5f)) / maxZ * 40.0f;
            const float shade = std::clamp(0.85f + (gx + gy) * 0.5f, 0.45f, 1.25f); // lit from the top left
            float r = 86 + h * 90, g = 112 + h * 40, bl = 64 + h * 100;
            const mpr::Sector* s = m.At(static_cast<int>(wx / 32.0f), static_cast<int>(wy / 32.0f));
            if (s && s->water) {
                const int tx = std::clamp(static_cast<int>((wx - std::floor(wx / 32.0f) * 32.0f) / 2.0f), 0, 15);
                const int ty = std::clamp(static_cast<int>((wy - std::floor(wy / 32.0f) * 32.0f) / 2.0f), 0, 15);
                if (s->waterMaterial[ty][tx] >= 0) { r = 52; g = 92; bl = 150; }
            }
            o[0] = static_cast<uint8_t>(std::clamp(r * shade, 0.0f, 255.0f));
            o[1] = static_cast<uint8_t>(std::clamp(g * shade, 0.0f, 255.0f));
            o[2] = static_cast<uint8_t>(std::clamp(bl * shade, 0.0f, 255.0f));
            o[3] = 255;
        }
    if (!b.tex) {
        glGenTextures(1, &b.tex);
        glBindTexture(GL_TEXTURE_2D, b.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else glBindTexture(GL_TEXTURE_2D, b.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, P, P, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

std::string MapsBlockOf(const std::string& mapTxt) { // the lines after #maps
    std::string out;
    bool in = false;
    size_t at = 0;
    while (at < mapTxt.size()) {
        size_t end = mapTxt.find('\n', at);
        std::string line = mapTxt.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? mapTxt.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) { if (in) break; continue; }
        if (line[0] == '#') { if (in) break; in = quest::Lower(line.substr(0, 5)) == "#maps"; continue; }
        if (in) out += line + "\n";
    }
    return out;
}

// Who talks besides the hero in a quest's first briefing (#show X).
std::string NpcOf(const quest::Quest& q) {
    for (const std::string& e : quest::ListEntries(q)) {
        const std::string l = quest::Lower(e);
        if (l.rfind("briefing", 0) != 0 || l.size() < 2 || l.compare(l.size() - 2, 2, "_1") != 0) continue;
        std::vector<uint8_t> bytes;
        if (!quest::ReadEntry(q, e, bytes)) continue;
        const std::string t(bytes.begin(), bytes.end());
        size_t at = 0;
        while ((at = t.find("#show ", at)) != std::string::npos) {
            const size_t end = t.find_first_of("\r\n", at);
            std::string who = t.substr(at + 6, end == std::string::npos ? std::string::npos : end - at - 6);
            while (!who.empty() && who.back() == ' ') who.pop_back();
            if (!who.empty() && who != "Hero") return who;
            at += 6;
        }
    }
    return "Hero";
}

void LoadZone(App& app, BuilderUi& b, const std::string& zone);

// The quest a new one copies the map's settings from: one on the same terrain, else one in the same allod.
const quest::QuestSet* TemplateFor(App& app, const std::string& zone) {
    for (const quest::QuestSet& s : app.quests) if (Lower(s.shown.terrain) == Lower(zone)) return &s;
    const int a = app.rating.allods.Find(zone);
    if (a >= 0) for (const quest::QuestSet& s : app.quests) if (app.rating.allods.Find(s.shown.terrain) == a) return &s;
    return nullptr;
}

void Defaults(App& app, BuilderUi& b) {
    qdraft::Draft& d = b.d;
    d.terrain = b.zone;
    d.baseMap = b.zone + "-lmp";
    d.groups.clear();
    d.objectives.clear();
    b.selGroup = b.selObj = -1;
    d.firstId = 1000000;
    b.templateName.clear();
    d.region.clear(); d.sky = "normal"; d.weather = "none"; d.mapsBlock.clear(); d.exitTarget = "bz1mpg 1"; d.exitTitle = "Leave";
    d.deploy = d.remove = quest::Rect{};
    d.view = 0;
    d.npc = "Hero";
    if (const quest::QuestSet* t = TemplateFor(app, b.zone)) {
        const quest::Quest& q = t->shown;
        b.templateName = q.name;
        d.region = q.region;
        d.sky = q.sky.empty() ? "normal" : q.sky;
        d.weather = q.weather.empty() ? "none" : q.weather;
        if (!q.exits.empty()) {
            d.exitTarget = q.exits[0].target;
            d.exitTitle = q.exits[0].title.empty() ? "Leave" : q.exits[0].title;
            d.view = q.exits[0].view;
            if (Lower(q.terrain) == Lower(b.zone)) { d.deploy = q.exits[0].deploy; d.remove = q.exits[0].remove; }
        }
        const std::string entry = quest::MapTxtEntry(q);
        std::vector<uint8_t> bytes;
        if (!entry.empty() && quest::ReadEntry(q, entry, bytes)) d.mapsBlock = MapsBlockOf(std::string(bytes.begin(), bytes.end()));
        d.npc = NpcOf(q);
    }
    if (!d.deploy.set) { d.deploy = {true, b.mapW * 0.1f, b.mapH * 0.1f, b.mapW * 0.1f + 6, b.mapH * 0.1f + 4}; }
    if (!d.remove.set) { d.remove = {true, b.mapW * 0.1f + 8, b.mapH * 0.1f, b.mapW * 0.1f + 16, b.mapH * 0.1f + 4}; }
    // the first free block of 100 object IDs among the quests of this terrain
    std::vector<uint32_t> used;
    for (const quest::QuestSet& s : app.quests) {
        if (Lower(s.shown.terrain) != Lower(b.zone)) continue;
        mob::File f;
        const std::string path = ResolveMapFile(app, s.shown.name + ".mob", s.shown.path);
        if (path.empty() || !mob::Load(path, f)) continue;
        for (const mob::Range& r : f.secRanges) used.push_back(r.max);
        for (const mob::Object& o : f.objects) if (o.id >= 1000000) used.push_back(o.id);
    }
    d.firstId = qdraft::NextIdBlock(used);
    if (d.intro.empty()) d.intro = {{d.npc, "I have a job for you."}, {"Hero", "Tell me more."}, {d.npc, "Please clear the area and come back."}};
    if (d.reject.empty()) d.reject = {{"Hero", "Not now."}};
    if (d.outro.empty()) d.outro = {{d.npc, "Well done."}};
}

void LoadZone(App& app, BuilderUi& b, const std::string& zone) {
    b.zone = zone;
    b.terrainOk = b.baseOk = false;
    b.message.clear();
    b.baseError.clear();
    b.builtKey.clear();
    b.resultOk = false;
    b.terrain = mpr::Map();
    std::string err;
    const std::string mpath = ResolveMapFile(app, zone + ".mpr", "");
    if (mpath.empty() || !mpr::Load(mpath, b.terrain, err)) { b.baseError = "The terrain " + zone + ".mpr was not found or cannot be read."; return; }
    b.terrainOk = true;
    b.mapW = b.terrain.Width();
    b.mapH = b.terrain.Height();
    MakePicture(b);
    const std::string base = ResolveMapFile(app, zone + "-lmp.mob", "");
    if (base.empty() || !mob::Load(base, b.base)) b.baseError = "The base map " + zone + "-lmp.mob was not found: the quest has nothing to run on.";
    else b.baseOk = true;
    Defaults(app, b);
}

// ---- the picture ------------------------------------------------------------------------------------------------------

ImVec2 ToScreen(const BuilderUi&, ImVec2 origin, float ppu, float x, float y) { return ImVec2(origin.x + x * ppu, origin.y + y * ppu); } // ppu: pixels per world unit

void DrawRectWorld(const BuilderUi& b, ImDrawList* dl, ImVec2 origin, float ppu, const quest::Rect& r, ImU32 col, const char* label) {
    if (!r.set) return;
    const ImVec2 a = ToScreen(b, origin, ppu, std::min(r.x1, r.x2), std::min(r.y1, r.y2)), c = ToScreen(b, origin, ppu, std::max(r.x1, r.x2), std::max(r.y1, r.y2));
    dl->AddRectFilled(a, c, (col & 0x00FFFFFF) | 0x50000000);
    dl->AddRect(a, c, col, 0, 0, 2.0f);
    dl->AddText(ImVec2(a.x + 2, a.y - ImGui::GetTextLineHeight() - 1), col, label);
}

void Picture(App& app, BuilderUi& b, float ppu) {
    qdraft::Draft& d = b.d;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float L = std::max(b.mapW, b.mapH);
    const ImVec2 size(b.mapW * ppu, b.mapH * ppu);
    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(b.tex)), size, ImVec2(0, 0), ImVec2(b.mapW / L, b.mapH / L));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    DrawRectWorld(b, dl, origin, ppu, d.deploy, IM_COL32(110, 220, 120, 255), "Start");
    DrawRectWorld(b, dl, origin, ppu, d.remove, IM_COL32(110, 170, 255, 255), "Exit");
    int objNo = 0;
    for (size_t i = 0; i < d.objectives.size(); ++i) {
        const qdraft::Objective& o = d.objectives[i];
        if (o.kind != qdraft::Objective::Area) continue;
        const float h = o.size * 0.5f;
        DrawRectWorld(b, dl, origin, ppu, {true, o.x - h, o.y - h, o.x + h, o.y + h}, static_cast<int>(i) == b.selObj ? IM_COL32(255, 255, 255, 255) : IM_COL32(240, 170, 60, 255), ("Goal " + std::to_string(i + 1)).c_str());
        ++objNo;
    }
    const std::vector<std::string> names = qdraft::GroupNames(d);
    for (size_t g = 0; g < d.groups.size(); ++g) {
        const qdraft::Group& gr = d.groups[g];
        const ImVec2 c = ToScreen(b, origin, ppu, gr.x, gr.y);
        const float rad = std::max(3.0f, gr.radius * ppu);
        const bool sel = static_cast<int>(g) == b.selGroup;
        dl->AddCircleFilled(c, rad, IM_COL32(220, 70, 60, 60));
        dl->AddCircle(c, rad, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 90, 80, 255), 0, 2.0f);
        dl->AddText(ImVec2(c.x + 3, c.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(255, 235, 230, 255), (names[g] + " x" + std::to_string(gr.count)).c_str());
    }
    dl->PopClipRect();
    ImGuiIO& io = ImGui::GetIO();
    const float wx = (io.MousePos.x - origin.x) / ppu, wy = (io.MousePos.y - origin.y) / ppu;
    const bool onMap = hovered && wx >= 0 && wy >= 0 && wx < b.mapW && wy < b.mapH;
    if (onMap) {
        const char* hint = b.tool == BuilderUi::Deploy ? "click: the party's start" : b.tool == BuilderUi::Exit ? "click: the exit" : b.tool == BuilderUi::GroupAt ? "click: the group's place"
                         : b.tool == BuilderUi::AreaAt ? "click: the goal's place" : "click a group to select it";
        ImGui::SetTooltip("x %.0f  y %.0f  %s", wx, wy, hint);
        if (b.tool != BuilderUi::None) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            switch (b.tool) {
            case BuilderUi::Deploy: d.deploy = {true, wx - 3, wy - 2, wx + 3, wy + 2}; b.tool = BuilderUi::None; break;
            case BuilderUi::Exit: d.remove = {true, wx - 4, wy - 2, wx + 4, wy + 2}; b.tool = BuilderUi::None; break;
            case BuilderUi::GroupAt: if (b.selGroup >= 0 && b.selGroup < static_cast<int>(d.groups.size())) { d.groups[static_cast<size_t>(b.selGroup)].x = wx; d.groups[static_cast<size_t>(b.selGroup)].y = wy; } b.tool = BuilderUi::None; break;
            case BuilderUi::AreaAt: if (b.selObj >= 0 && b.selObj < static_cast<int>(d.objectives.size())) { d.objectives[static_cast<size_t>(b.selObj)].x = wx; d.objectives[static_cast<size_t>(b.selObj)].y = wy; } b.tool = BuilderUi::None; break;
            default: {
                float best = 12.0f * 12.0f;
                for (size_t g = 0; g < d.groups.size(); ++g) {
                    const ImVec2 c = ToScreen(b, origin, ppu, d.groups[g].x, d.groups[g].y);
                    const float dd = (c.x - io.MousePos.x) * (c.x - io.MousePos.x) + (c.y - io.MousePos.y) * (c.y - io.MousePos.y);
                    if (dd < best) { best = dd; b.selGroup = static_cast<int>(g); b.goStep = 2; }
                }
            }
            }
        }
    }
    if (b.tool != BuilderUi::None && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape))) b.tool = BuilderUi::None;
    (void)objNo;
    (void)app;
}

// ---- the steps -------------------------------------------------------------------------------------------------------

void ToolButton(BuilderUi& b, const char* label, BuilderUi::Tool t, const char* tip) {
    const bool on = b.tool == t;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label)) b.tool = on ? BuilderUi::None : t;
    if (on) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", tip);
}

void StepMap(App& app, BuilderUi& b) {
    RatingUi& r = app.rating;
    qdraft::Draft& d = b.d;
    // the zones that have a terrain file, by allod
    std::set<std::string> terrains;
    for (const Library::MapFile& f : app.lib.ListMapFiles())
        if (f.terrain) terrains.insert(Lower(std::filesystem::path(f.name).stem().string()));
    std::vector<std::string> allods{"All zones"};
    for (const allods::Allod& a : r.allods.allods) allods.push_back(a.name);
    b.allodIndex = std::clamp(b.allodIndex, -1, static_cast<int>(r.allods.allods.size()) - 1);
    ImGui::TextUnformatted("Allod");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##allod", allods[static_cast<size_t>(b.allodIndex + 1)].c_str())) {
        for (size_t i = 0; i < allods.size(); ++i) if (ImGui::Selectable(allods[i].c_str(), static_cast<int>(i) == b.allodIndex + 1)) b.allodIndex = static_cast<int>(i) - 1;
        ImGui::EndCombo();
    }
    std::vector<std::string> zones;
    if (b.allodIndex >= 0) { for (const std::string& z : r.allods.allods[static_cast<size_t>(b.allodIndex)].zones) if (terrains.count(Lower(z))) zones.push_back(Lower(z)); }
    else zones.assign(terrains.begin(), terrains.end());
    ImGui::TextUnformatted("Zone");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##zone", b.zone.empty() ? "Pick a zone" : b.zone.c_str())) {
        for (const std::string& z : zones) {
            const std::string ally = r.allods.NameOf(z);
            if (ImGui::Selectable((z + (ally.empty() ? "" : "  (" + ally + ")")).c_str(), z == b.zone)) LoadZone(app, b, z);
        }
        ImGui::EndCombo();
    }
    if (zones.empty()) ImGui::TextDisabled("No terrain files: add the mod's map folder in Settings.");
    if (!b.baseError.empty()) ImGui::TextColored(kWarn, "%s", b.baseError.c_str());
    if (!b.terrainOk) return;
    ImGui::TextDisabled("%.0f x %.0f units%s", b.mapW, b.mapH, b.templateName.empty() ? "" : ("; settings copied from " + b.templateName).c_str());
    ImGui::SeparatorText("Start and exit");
    ToolButton(b, "Place start", BuilderUi::Deploy, "Click the picture: where the party arrives");
    ImGui::SameLine();
    ToolButton(b, "Place exit", BuilderUi::Exit, "Click the picture: where the party leaves the map");
    ImGui::TextDisabled("Start %.0f, %.0f   Exit %.0f, %.0f", (d.deploy.x1 + d.deploy.x2) / 2, (d.deploy.y1 + d.deploy.y2) / 2, (d.remove.x1 + d.remove.x2) / 2, (d.remove.y1 + d.remove.y2) / 2);
    ImGui::TextUnformatted("Exit leads to");
    Edit("##exittarget", d.exitTarget, 64);
    ImGui::SetItemTooltip("The map the party goes to: a quest map's name and the exit number there (\"bz1mpg 1\")");
    ImGui::TextUnformatted("Exit name");
    Edit("##exittitle", d.exitTitle, 64);
    ImGui::SeparatorText("Look");
    ImGui::TextUnformatted("Lighting region");
    Edit("##region", d.region, 32);
    ImGui::SetItemTooltip("lights<region>.ini: gipat, ingos, suslanger, jigran...");
    static const char* const skies[] = {"normal", "cave"};
    int sky = d.sky == "cave" ? 1 : 0;
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("Sky", &sky, skies, 2)) d.sky = skies[sky];
    ImGui::SetNextItemWidth(120);
    Edit("Weather", d.weather, 32, 120);
}

void LinesEditor(BuilderUi& b, const char* id, std::vector<qdraft::Line>& lines) {
    ImGui::PushID(id);
    for (size_t i = 0; i < lines.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        qdraft::Line& l = lines[i];
        int who = l.speaker == "Hero" ? 0 : 1;
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Hero").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetFrameHeight());
        const std::string npc = b.d.npc;
        if (ImGui::BeginCombo("##who", who == 0 ? "Hero" : l.speaker.c_str())) {
            if (ImGui::Selectable("Hero", who == 0)) l.speaker = "Hero";
            if (npc != "Hero" && ImGui::Selectable(npc.c_str(), who == 1)) l.speaker = npc;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        Edit("##text", l.text, 512, -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        if (ImGui::Button("x")) { lines.erase(lines.begin() + static_cast<long>(i)); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ line")) lines.push_back({lines.empty() || lines.back().speaker != "Hero" ? "Hero" : b.d.npc, ""});
    ImGui::PopID();
}

void StepStory(App& app, BuilderUi& b) {
    (void)app;
    qdraft::Draft& d = b.d;
    ImGui::TextUnformatted("Quest ID");
    Edit("##name", d.name, 32);
    ImGui::SetItemTooltip("The file name, and what the game and the scripts call the quest: letters, digits, _ (z26q9)");
    if (!d.name.empty() && !qdraft::ValidName(d.name)) ImGui::TextColored(kWarn, "3-24 letters, digits or _ , a letter first");
    for (const quest::QuestSet& s : app.quests) if (!d.name.empty() && Lower(s.shown.name) == Lower(d.name)) { ImGui::TextColored(kWarn, "A quest with this ID exists"); break; }
    ImGui::TextUnformatted("Title");
    Edit("##title", d.title, 128);
    ImGui::TextUnformatted("Summary");
    EditMulti("##summary", d.summary, 1024, ImGui::GetTextLineHeight() * 4);
    ImGui::TextUnformatted("Quest giver");
    Edit("##npc", d.npc, 32, 160);
    ImGui::SetItemTooltip("Who talks in the briefings besides the hero (a character name the game knows: Huber, KMage...)");
    if (ImGui::CollapsingHeader("Briefing: receiving the quest", ImGuiTreeNodeFlags_DefaultOpen)) LinesEditor(b, "intro", d.intro);
    if (ImGui::CollapsingHeader("Briefing: refusing it")) LinesEditor(b, "reject", d.reject);
    if (ImGui::CollapsingHeader("Briefing: completing it")) LinesEditor(b, "outro", d.outro);
}

void StepUnits(App& app, BuilderUi& b) {
    qdraft::Draft& d = b.d;
    if (!b.terrainOk) { ImGui::TextDisabled("Pick a map first."); return; }
    if (app.lib.unitsDb.monsters.empty()) { ImGui::TextColored(kWarn, "No database set in Settings: there are no units to pick."); return; }
    ImGui::TextDisabled("Enemy groups: units that stand together. Click the picture to place the selected group.");
    if (ImGui::Button("+ Add group")) {
        qdraft::Group g;
        const units::Monster* pick = &app.lib.unitsDb.monsters.front();
        for (const units::Monster& c : app.lib.unitsDb.monsters) if (Lower(c.name).rfind("human hero", 0) != 0) { pick = &c; break; }
        const units::Monster& m = *pick;
        g.monster = m.name;
        if (const units::Race* race = app.lib.unitsDb.FindRace(m.race)) g.templ = race->mask;
        g.name = qdraft::Identifier(m.name, "Group");
        g.x = b.mapW * 0.5f;
        g.y = b.mapH * 0.5f;
        d.groups.push_back(g);
        b.selGroup = static_cast<int>(d.groups.size()) - 1;
        b.tool = BuilderUi::GroupAt;
    }
    ImGui::BeginDisabled(b.selGroup < 0 || b.selGroup >= static_cast<int>(d.groups.size()));
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) { qdraft::Group g = d.groups[static_cast<size_t>(b.selGroup)]; g.x += 6; g.y += 6; d.groups.push_back(g); b.selGroup = static_cast<int>(d.groups.size()) - 1; }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        const int gi = b.selGroup;
        d.groups.erase(d.groups.begin() + gi);
        for (qdraft::Objective& o : d.objectives) { if (o.group == gi) o.group = 0; else if (o.group > gi) --o.group; }
        b.selGroup = std::min(gi, static_cast<int>(d.groups.size()) - 1);
    }
    ImGui::EndDisabled();
    const std::vector<std::string> names = qdraft::GroupNames(d);
    if (ImGui::BeginListBox("##groups", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * std::min<int>(6, static_cast<int>(d.groups.size()) + 1)))) {
        for (size_t g = 0; g < d.groups.size(); ++g)
            if (ImGui::Selectable((names[g] + "  " + std::to_string(d.groups[g].count) + " x " + d.groups[g].monster).c_str(), static_cast<int>(g) == b.selGroup)) b.selGroup = static_cast<int>(g);
        ImGui::EndListBox();
    }
    if (b.selGroup < 0 || b.selGroup >= static_cast<int>(d.groups.size())) { ImGui::TextDisabled("Add a group to start."); return; }
    qdraft::Group& g = d.groups[static_cast<size_t>(b.selGroup)];
    ImGui::SeparatorText("Selected group");
    ImGui::TextUnformatted("Name");
    Edit("##gname", g.name, 32, 200);
    ImGui::SetItemTooltip("What the script calls the group");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Units", &g.count, 1, 40);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Spread", &g.radius, 1.0f, 20.0f, "%.0f");
    ImGui::SetItemTooltip("The radius they stand in");
    ToolButton(b, "Place on map", BuilderUi::GroupAt, "Click the picture: where the group stands");
    ImGui::SameLine();
    ImGui::TextDisabled("at %.0f, %.0f", g.x, g.y);
    ImGui::TextUnformatted("Unit");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g.monster.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mfilter", "search the units", b.monsterFilter, sizeof b.monsterFilter);
    const std::string filter = Lower(b.monsterFilter);
    if (ImGui::BeginListBox("##monsters", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 9))) {
        int shown = 0;
        for (const units::Monster& m : app.lib.unitsDb.monsters) {
            if (!filter.empty() && Lower(m.name).find(filter) == std::string::npos) continue;
            if (++shown > 500) { ImGui::TextDisabled("(more: search to narrow)"); break; }
            if (ImGui::Selectable(m.name.c_str(), m.name == g.monster)) {
                const bool autoName = g.name == qdraft::Identifier(g.monster, "Group");
                g.monster = m.name;
                if (const units::Race* race = app.lib.unitsDb.FindRace(m.race)) g.templ = race->mask;
                if (autoName) g.name = qdraft::Identifier(m.name, "Group");
            }
        }
        ImGui::EndListBox();
    }
}

void StepObjectives(App& app, BuilderUi& b) {
    (void)app;
    qdraft::Draft& d = b.d;
    if (!b.terrainOk) { ImGui::TextDisabled("Pick a map first."); return; }
    ImGui::TextDisabled("What the party has to do, in order. The game finishes the quest when every goal is done.");
    ImGui::BeginDisabled(d.groups.empty());
    if (ImGui::Button("+ Kill a group")) {
        qdraft::Objective o;
        o.group = std::clamp(b.selGroup, 0, std::max(0, static_cast<int>(d.groups.size()) - 1));
        o.title = d.groups.empty() ? "Kill the enemies" : "Defeat " + d.groups[static_cast<size_t>(o.group)].monster;
        d.objectives.push_back(o);
        b.selObj = static_cast<int>(d.objectives.size()) - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Reach an area")) {
        qdraft::Objective o;
        o.kind = qdraft::Objective::Area;
        o.title = "Reach the marked place";
        o.x = (d.remove.x1 + d.remove.x2) * 0.5f;
        o.y = (d.remove.y1 + d.remove.y2) * 0.5f;
        d.objectives.push_back(o);
        b.selObj = static_cast<int>(d.objectives.size()) - 1;
        b.tool = BuilderUi::AreaAt;
    }
    ImGui::BeginDisabled(d.groups.empty() || !d.objectives.empty());
    ImGui::SameLine();
    if (ImGui::Button("One per group")) {
        for (size_t g = 0; g < d.groups.size(); ++g) {
            qdraft::Objective o;
            o.group = static_cast<int>(g);
            o.title = "Defeat " + d.groups[g].monster;
            d.objectives.push_back(o);
        }
        b.selObj = 0;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginListBox("##objs", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * std::min<int>(6, static_cast<int>(d.objectives.size()) + 1)))) {
        for (size_t i = 0; i < d.objectives.size(); ++i)
            if (ImGui::Selectable((std::to_string(i + 1) + ". " + (d.objectives[i].kind == qdraft::Objective::Area ? "[area] " : "[kill] ") + d.objectives[i].title).c_str(), static_cast<int>(i) == b.selObj)) b.selObj = static_cast<int>(i);
        ImGui::EndListBox();
    }
    if (b.selObj < 0 || b.selObj >= static_cast<int>(d.objectives.size())) return;
    const size_t at = static_cast<size_t>(b.selObj);
    ImGui::BeginDisabled(at == 0);
    if (ImGui::SmallButton("Up")) { std::swap(d.objectives[at], d.objectives[at - 1]); --b.selObj; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(at + 1 >= d.objectives.size());
    if (ImGui::SmallButton("Down")) { std::swap(d.objectives[at], d.objectives[at + 1]); ++b.selObj; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) { d.objectives.erase(d.objectives.begin() + static_cast<long>(at)); b.selObj = std::min(b.selObj, static_cast<int>(d.objectives.size()) - 1); return; }
    qdraft::Objective& o = d.objectives[static_cast<size_t>(b.selObj)];
    ImGui::SeparatorText("Selected goal");
    ImGui::TextUnformatted("Title");
    Edit("##otitle", o.title, 128);
    ImGui::TextUnformatted("Text");
    EditMulti("##otext", o.text, 512, ImGui::GetTextLineHeight() * 3);
    if (o.kind == qdraft::Objective::KillGroup) {
        const std::vector<std::string> names = qdraft::GroupNames(d);
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("Group", o.group >= 0 && o.group < static_cast<int>(names.size()) ? names[static_cast<size_t>(o.group)].c_str() : "")) {
            for (size_t g = 0; g < names.size(); ++g) if (ImGui::Selectable(names[g].c_str(), static_cast<int>(g) == o.group)) o.group = static_cast<int>(g);
            ImGui::EndCombo();
        }
    } else {
        ToolButton(b, "Place area", BuilderUi::AreaAt, "Click the picture: the middle of the place to reach");
        ImGui::SameLine();
        ImGui::TextDisabled("at %.0f, %.0f", o.x, o.y);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Size", &o.size, 2.0f, 40.0f, "%.0f");
    }
}

void StepRewards(App& app, BuilderUi& b) {
    (void)app;
    qdraft::Draft& d = b.d;
    ImGui::TextDisabled("Paid when the party hands the quest in.");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("Experience", &d.exp, 100, 1000);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("Gold", &d.money, 100, 1000);
    d.exp = std::max(0, d.exp);
    d.money = std::max(0, d.money);
    static const char* const tags[] = {"From the rating", "Easy", "Medium", "Hard"};
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("Briefing tag", &b.tagMode, tags, 4);
    ImGui::SetItemTooltip("The [Easy] / [Medium] / [Hard] word on the briefing's first line");
    ImGui::TextDisabled("The rating on the right shows how hard the quest is for this allod; rewards follow your own balance.");
}

// ---- saving ---------------------------------------------------------------------------------------------------------

std::string TagFor(const BuilderUi& b) {
    if (b.tagMode == 1) return "Easy";
    if (b.tagMode == 2) return "Medium";
    if (b.tagMode == 3) return "Hard";
    if (!b.resultOk || !b.result.ok) return "Medium";
    return b.result.total < 3500 ? "Easy" : b.result.total < 6500 ? "Medium" : "Hard";
}

bool WriteFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes, std::string& err) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) { err = "cannot write " + p.string(); return false; }
    return true;
}

bool SaveQuest(App& app, BuilderUi& b, std::string& message) {
    qdraft::Draft d = b.d;
    d.tag = TagFor(b);
    const std::filesystem::path dir(b.outFolder);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) { message = "The output folder does not exist."; return false; }
    const std::filesystem::path mq = dir / (d.name + ".mq"), mobPath = dir / (d.name + ".mob");
    if (!b.overwrite && (std::filesystem::exists(mq, ec) || std::filesystem::exists(mobPath, ec))) { message = d.name + ".mq or .mob is already in that folder (tick Overwrite to replace them)."; return false; }
    mob::File f;
    std::string err;
    if (!qdraft::BuildMob(d, f, err)) { message = err; return false; }
    std::vector<uint8_t> reg;
    if (!IniRegTextToReg(qdraft::QuestIni(d), reg)) { message = "quest.reg could not be made"; return false; }
    std::map<std::string, std::vector<uint8_t>> files;
    auto put = [&](const std::string& n, const std::string& t) { files[n] = std::vector<uint8_t>(t.begin(), t.end()); };
    put("quest " + d.name, qdraft::QuestText(d));
    put("briefing " + d.name + "_1", qdraft::BriefingReceive(d));
    put("briefing " + d.name + "_2", qdraft::BriefingReject(d));
    put("briefing " + d.name + "_3", qdraft::BriefingComplete(d));
    put(d.name + "\\map.txt", qdraft::MapTxt(d));
    files[d.name + "\\quest.reg"] = reg;
    const std::vector<uint8_t> archive = res::WriteArchive(files, static_cast<uint32_t>(std::time(nullptr)));
    if (!WriteFile(mq, archive, err) || !WriteFile(mobPath, f.bytes, err)) { message = err; return false; }
    message = "Saved " + mq.string() + " and " + mobPath.filename().string();
    b.savedQuest = d.name;
    b.savedFolder = dir.string();
    app.questsFor.clear(); // the quest list reads the folder again
    RescanQuests(app);
    return true;
}

void StepReview(App& app, BuilderUi& b) {
    qdraft::Draft& d = b.d;
    std::vector<std::string> taken;
    for (const quest::QuestSet& s : app.quests) taken.push_back(s.shown.name);
    b.problems = qdraft::Problems(d, b.savedQuest == d.name ? std::vector<std::string>{} : taken);
    ImGui::SeparatorText("Quest");
    ImGui::Text("%s  -  %s", d.name.c_str(), d.title.c_str());
    ImGui::TextDisabled("%s: %d unit%s in %zu group%s, %zu objective%s; reward %d exp, %d gold", d.terrain.c_str(), qdraft::UnitCount(d), qdraft::UnitCount(d) == 1 ? "" : "s", d.groups.size(),
                        d.groups.size() == 1 ? "" : "s", d.objectives.size(), d.objectives.size() == 1 ? "" : "s", d.exp, d.money);
    if (b.builtOk) {
        const MobScriptReport rep = CheckMobScript(b.built.script);
        if (rep.errors) ImGui::TextColored(kWarn, "The generated script has %d error%s: report it.", rep.errors, rep.errors == 1 ? "" : "s");
    }
    ImGui::SeparatorText("To do");
    if (b.problems.empty()) ImGui::TextColored(ImVec4(0.56f, 0.88f, 0.61f, 1), "Ready to save.");
    for (const std::string& p : b.problems) ImGui::TextColored(kWarn, "- %s", p.c_str());
    ImGui::SeparatorText("Save");
    if (b.outFolder[0] == '\0' && !app.lib.questFolders.empty()) {
        std::error_code ec;
        const std::filesystem::path first(app.lib.questFolders.front());
        std::snprintf(b.outFolder, sizeof b.outFolder, "%s", (std::filesystem::is_directory(first, ec) ? first : first.parent_path()).string().c_str());
    }
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##out", b.outFolder, sizeof b.outFolder);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) { std::string picked; if (ui::PickFolder(picked)) std::snprintf(b.outFolder, sizeof b.outFolder, "%s", picked.c_str()); }
    ImGui::SetItemTooltip("Where <ID>.mq and <ID>.mob are written: a quest folder of the mod (Settings > Quests), never the game's install");
    ImGui::Checkbox("Overwrite", &b.overwrite);
    ImGui::BeginDisabled(!b.problems.empty() || b.outFolder[0] == '\0');
    if (ImGui::Button("Save quest", ImVec2(140, 0))) SaveQuest(app, b, b.message);
    ImGui::EndDisabled();
    if (!b.message.empty()) ImGui::TextWrapped("%s", b.message.c_str());
    if (!b.savedQuest.empty() && b.savedQuest == d.name) {
        if (ImGui::Button("Open in the Map Editor")) {
            bool opened = false;
            for (const quest::QuestSet& s : app.quests)
                if (Lower(s.shown.name) == Lower(d.name) && Lower(std::filesystem::path(s.shown.path).parent_path().string()) == Lower(b.savedFolder)) { OpenQuest(app, s); opened = true; break; }
            if (!opened) { // saved outside the quest folders of Settings: the quest is read from where it was written
                quest::QuestSet set;
                if (quest::Load((std::filesystem::path(b.savedFolder) / (d.name + ".mq")).string(), set.shown)) {
                    set.copies.push_back(set.shown);
                    set.labels.push_back(quest::FolderLabel(b.savedFolder));
                    OpenQuest(app, set);
                    opened = true;
                }
            }
            b.wantMapEditor = opened;
        }
        ImGui::SetItemTooltip("Edit the units, the script and the terrain there");
    }
}

// ---- the draft as a map, and its rating ---------------------------------------------------------------------------------

void Refresh(App& app, BuilderUi& b) {
    const qdraft::Draft& d = b.d;
    std::string key = d.name + "|" + d.terrain + "|" + std::to_string(d.firstId);
    for (const qdraft::Group& g : d.groups) key += "|g" + g.monster + "," + g.name + "," + std::to_string(g.count) + "," + Num0(g.x * 10) + "," + Num0(g.y * 10) + "," + Num0(g.radius);
    for (const qdraft::Objective& o : d.objectives) key += "|o" + std::to_string(o.kind) + "," + std::to_string(o.group) + "," + Num0(o.x * 10) + "," + Num0(o.y * 10) + "," + Num0(o.size);
    key += "|r" + Num0(d.deploy.x1 * 10) + "," + Num0(d.deploy.y1 * 10) + "," + Num0(d.remove.x1 * 10) + "," + Num0(d.remove.y1 * 10);
    if (key != b.builtKey) {
        b.builtKey = key;
        b.builtOk = false;
        b.resultOk = false;
        b.builtError.clear();
        qdraft::Draft tmp = d;
        if (b.baseOk && !d.groups.empty()) b.builtOk = qdraft::BuildMob(tmp, b.built, b.builtError);
    }
    if (b.builtOk && b.baseOk && !b.resultOk) {
        rating::Input in;
        in.files = {&b.base, &b.built};
        in.baseCount = 1;
        in.deploy = d.deploy;
        in.remove = d.remove;
        b.resultOk = DraftRating(app, in, b.result, b.resultWait);
    }
}

} // namespace

void QuestBuilderTab(App& app) {
    BuilderUi& b = app.builder;
    PollRating(app);
    RescanQuests(app);
    if (!b.started) {
        b.started = true;
        b.d.groups.clear();
    }
    Refresh(app, b);

    const float step = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTabBar("##qbsteps")) {
        for (int i = 0; i < kStepCount; ++i) {
            std::string label = std::to_string(i + 1) + ". " + kSteps[i];
            if (ImGui::BeginTabItem(label.c_str(), nullptr, b.goStep == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                b.step = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    b.goStep = -1;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float formW = std::clamp(avail.x * 0.42f, 320.0f, 560.0f);
    ImGui::BeginChild("##qbform", ImVec2(formW, avail.y - step), ImGuiChildFlags_Borders);
    switch (b.step) {
    case 0: StepMap(app, b); break;
    case 1: StepStory(app, b); break;
    case 2: StepUnits(app, b); break;
    case 3: StepObjectives(app, b); break;
    case 4: StepRewards(app, b); break;
    default: StepReview(app, b); break;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##qbview", ImVec2(0, avail.y - step));
    if (b.terrainOk) {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const float ppu = std::clamp(std::min(room.x / b.mapW, room.y * 0.62f / b.mapH), 0.4f, 8.0f);
        Picture(app, b, ppu);
        ImGui::SeparatorText("Difficulty");
        if (b.resultOk) DraftRatingPanel(app, b.result, b.d.terrain);
        else if (!b.baseOk) ImGui::TextDisabled("Needs the zone's base map.");
        else if (b.d.groups.empty()) ImGui::TextDisabled("Add an enemy group to see how hard the quest is.");
        else if (!b.builtError.empty()) ImGui::TextColored(kWarn, "%s", b.builtError.c_str());
        else ImGui::TextDisabled("%s", b.resultWait.empty() ? "Rating..." : b.resultWait.c_str());
    } else {
        ImGui::TextDisabled("Pick an allod and a zone to see the map.");
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(b.step == 0);
    if (ImGui::Button("Back", ImVec2(100, 0))) b.goStep = b.step - 1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Step %d of %d", b.step + 1, kStepCount);
    ImGui::SameLine();
    ImGui::BeginDisabled(b.step >= kStepCount - 1);
    if (ImGui::Button("Next", ImVec2(100, 0))) b.goStep = b.step + 1;
    ImGui::EndDisabled();
}

} // namespace mapedit
