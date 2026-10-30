// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Patrol simulation.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ---- patrol simulation --------------------------------------------------------------------------------
// Units with a patrol path walk it: at their walking speed (stats, per tick, 15 ticks a second), along the
// shortest route around what blocks them (the scene's walkability grid) smoothed where the way is clear, turning to each look point of a patrol point and waiting its time there; then on to
// the next point (back to the first when cyclic, else back along the path). Nothing is written to the map.

static const char* kSimLimits =
    "Units walk their patrol paths like in the game:\n"
        "- routes: the shortest way on the game's walkable tiles (Layers > Walkability); units do not avoid\n"
        "  one another; without a terrain they walk straight\n"
        "- speed: the unit's walking speed (15 ticks a second); no running\n"
        "- no fighting, calls for help, alarms or scripts; guards and sentries stay put\n"
        "- nothing is written to the map; editing waits until the simulation stops";

// A* over the walkability grid (8 directions, diagonals only between free sides), then the corners that
// are needed: a straight line between two points is kept when every cell it crosses is free. A start or
// target in a blocked cell (a unit against a wall) searches from the nearest free cell.
static std::vector<mob::Vec3> FindRoute(const MapScene::WalkGrid& g, mob::Vec3 from, mob::Vec3 to) {
    const std::vector<mob::Vec3> straight{to};
    if (g.w == 0) return straight;
    auto cellOf = [&](float v, int n) { return std::min(std::max(static_cast<int>(v / g.cell), 0), n - 1); };
    auto nearestFree = [&](int& x, int& y) {
        if (!g.Blocked(x, y)) return true;
        for (int r = 1; r <= 32; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if ((std::abs(dx) == r || std::abs(dy) == r) && !g.Blocked(x + dx, y + dy)) { x += dx; y += dy; return true; }
        return false;
    };
    int sx = cellOf(from.x, g.w), sy = cellOf(from.y, g.h), tx = cellOf(to.x, g.w), ty = cellOf(to.y, g.h);
    if (!nearestFree(sx, sy) || !nearestFree(tx, ty) || (sx == tx && sy == ty)) return straight;
    const int n = g.w * g.h;
    std::vector<float> cost(static_cast<size_t>(n), 1e30f);
    std::vector<int> back(static_cast<size_t>(n), -1);
    std::vector<char> closed(static_cast<size_t>(n), 0);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto h = [&](int x, int y) { const int dx = std::abs(x - tx), dy = std::abs(y - ty); return static_cast<float>(std::max(dx, dy)) + 0.4142f * std::min(dx, dy); };
    const int start = sy * g.w + sx, goal = ty * g.w + tx;
    cost[static_cast<size_t>(start)] = 0;
    open.push({h(sx, sy), start});
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int visited = 0;
    while (!open.empty() && visited < 3000000) {
        const int cur = open.top().second;
        open.pop();
        if (closed[static_cast<size_t>(cur)]) continue;
        closed[static_cast<size_t>(cur)] = 1;
        ++visited;
        if (cur == goal) break;
        const int cx = cur % g.w, cy = cur / g.w;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + dxs[d], ny = cy + dys[d];
            if (g.Blocked(nx, ny)) continue;
            if (d >= 4 && (g.Blocked(cx + dxs[d], cy) || g.Blocked(cx, cy + dys[d]))) continue; // no cutting corners
            const int ni = ny * g.w + nx;
            const float nc = cost[static_cast<size_t>(cur)] + (d >= 4 ? 1.4142f : 1.0f) * g.Cost(nx, ny); // the game's step cost
            if (nc < cost[static_cast<size_t>(ni)]) {
                cost[static_cast<size_t>(ni)] = nc;
                back[static_cast<size_t>(ni)] = cur;
                open.push({nc + h(nx, ny), ni});
            }
        }
    }
    if (back[static_cast<size_t>(goal)] < 0 && goal != start) return straight; // no way there: straight
    std::vector<mob::Vec3> cells;
    for (int c = goal; c != start && c >= 0; c = back[static_cast<size_t>(c)])
        cells.push_back({(c % g.w + 0.5f) * g.cell, (c / g.w + 0.5f) * g.cell, 0});
    std::reverse(cells.begin(), cells.end());
    cells.push_back(to);
    auto clear = [&](mob::Vec3 a, mob::Vec3 b) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        const int steps = std::max(1, static_cast<int>(len / (g.cell * 0.25f)));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            if (g.Blocked(cellOf(a.x + (b.x - a.x) * t, g.w), cellOf(a.y + (b.y - a.y) * t, g.h))) return false;
        }
        return true;
    };
    std::vector<mob::Vec3> route;
    mob::Vec3 at = from;
    size_t i = 0;
    while (i < cells.size()) {
        size_t reach = i; // "far" is a macro in the Windows headers
        for (size_t j = cells.size(); j-- > i + 1;) if (clear(at, cells[j])) { reach = j; break; }
        route.push_back(cells[reach]);
        at = cells[reach];
        i = reach + 1;
    }
    return route;
}

static float AngleTo(float from, float to) { // the shortest turn from one heading to another
    float d = std::fmod(to - from + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return d - 3.14159265f;
}

// The figures face along their local -Y (a heading of 0 looks along the world's -Y).
static float HeadingTo(float dx, float dy) { return std::atan2(dx, -dy); }

static void StartSimulation(App& app) {
    app.sim.clear();
    app.scene.simPoses.clear();
    app.scene.BuildWalkGrid(); // the ground as it is now
    for (auto& m : app.mobs) {
        if (!m->visible) continue;
        const mob::File& f = m->file;
        for (size_t oi = 0; oi < f.objects.size(); ++oi) {
            const mob::Object& o = f.objects[oi];
            if (o.kind != mob::Kind::Unit) continue;
            for (const mob::Logic& g : o.logics) {
                if (!g.use || g.model != 2 || g.patrol.empty()) continue;
                App::SimUnit u;
                u.object = &o;
                u.logic = g;
                u.x = o.position.x;
                u.y = o.position.y;
                u.yaw = 2.0f * std::atan2(o.rotation[3], o.rotation[0]);
                float walk = 0.0f;
                if (o.stats.size() >= 32) std::memcpy(&walk, o.stats.data() + 28, 4);
                u.speed = walk > 0.01f && walk < 3.0f ? walk * 15.0f : 3.5f; // units a second
                int ai = o.stats.size() >= 57 ? o.stats[56] : 1;
                u.layer = std::min(std::max(ai, 0), mob::kAiLayers - 1);
                app.sim.push_back(std::move(u));
                break;
            }
        }
    }
    app.simulating = true;
    app.simPaused = false;
}

static void StopSimulation(App& app) {
    app.simulating = false;
    app.sim.clear();
    app.scene.simPoses.clear();
}

void StepSimulation(App& app, float dt) {
    if (!app.simulating) return;
    // A map was unloaded or changed under it: the units it knew may be gone.
    size_t objects = 0;
    for (auto& m : app.mobs) objects += m->file.objects.size();
    if (objects != app.simObjects) { if (app.simObjects) { StopSimulation(app); return; } app.simObjects = objects; }
    if (app.simPaused) dt = 0;
    const float turnDefault = 3.14159265f; // radians a second when a look point says 0
    for (App::SimUnit& u : app.sim) {
        float left = dt;
        for (int guard = 0; guard < 16 && left > 0; ++guard) {
            const auto& pts = u.logic.patrol;
            const mob::PatrolPoint& target = pts[static_cast<size_t>(u.point)];
            if (u.phase == App::SimUnit::Walk) {
                if (u.route.empty()) {
                    u.route = FindRoute(app.scene.walk, {u.x, u.y, 0}, target.position);
                    u.leg = 0;
                }
                const mob::Vec3 next = u.route[static_cast<size_t>(u.leg)];
                const float dx = next.x - u.x, dy = next.y - u.y, dist = std::hypot(dx, dy);
                if (dist > 0.01f) {
                    // Turn towards the way first (quickly), then walk.
                    const float want = HeadingTo(dx, dy), turn = AngleTo(u.yaw, want), maxTurn = 2.0f * turnDefault * left;
                    u.yaw += std::max(-maxTurn, std::min(maxTurn, turn));
                }
                const float step = u.speed * left;
                if (step < dist) { u.x += dx / dist * step; u.y += dy / dist * step; left = 0; break; }
                u.x = next.x;
                u.y = next.y;
                left -= u.speed > 0 ? dist / u.speed : left;
                if (++u.leg >= static_cast<int>(u.route.size())) { u.route.clear(); u.phase = App::SimUnit::Look; u.look = 0; u.timer = 0; }
            } else { // at a patrol point: each look point in turn, turning to it then waiting its time
                if (u.look >= static_cast<int>(target.looks.size())) {
                    const int n = static_cast<int>(pts.size());
                    if (n > 1) {
                        if (u.logic.cyclic) u.point = (u.point + 1) % n;
                        else {
                            if (u.point + u.dir < 0 || u.point + u.dir >= n) u.dir = -u.dir;
                            u.point += u.dir;
                        }
                    }
                    u.phase = App::SimUnit::Walk;
                    if (n == 1) { u.look = 0; } // one point: look around it again and again
                    continue;
                }
                const mob::LookPoint& l = target.looks[static_cast<size_t>(u.look)];
                const float want = HeadingTo(l.position.x - u.x, l.position.y - u.y), turn = AngleTo(u.yaw, want);
                const float speed = l.turnSpeed > 0.001f ? l.turnSpeed * 15.0f : turnDefault;
                if (std::fabs(turn) > 0.01f && u.timer == 0) {
                    const float can = speed * left;
                    if (std::fabs(turn) > can) { u.yaw += turn > 0 ? can : -can; left = 0; break; }
                    u.yaw = want;
                    left -= std::fabs(turn) / speed;
                }
                const float wait = l.wait / 15.0f;
                if (u.timer + left < wait) { u.timer += left; left = 0; break; }
                left -= wait - u.timer;
                u.timer = 0;
                ++u.look;
            }
        }
        { // moving: it changed place since the last step (its walk animation plays; else its idle one)
            auto prev = app.scene.simPoses.find(u.object);
            const bool moving = prev != app.scene.simPoses.end() && std::hypot(prev->second.x - u.x, prev->second.y - u.y) > 1e-4f;
            app.scene.simPoses[u.object] = {u.x, u.y, u.yaw, moving};
        }
    }
}

// The small window of a running simulation: pause, speed, stop.
void SimulationWindow(App& app) {
    if (!app.simulating) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMax.y - 110), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Patrol simulation", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%zu unit(s) on patrol%s", app.sim.size(), app.scene.walk.w ? "" : " (no terrain loaded: straight lines)");
        if (ImGui::Button(app.simPaused ? "Play" : "Pause", ImVec2(70, 0))) app.simPaused = !app.simPaused;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##simspeed", &app.simSpeed, 0.25f, 8.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) open = false;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSimLimits);
    }
    ImGui::End();
    if (!open) StopSimulation(app);
}

// The Tools menu: exact offsets, random values, the minimap, saving the active map elsewhere, patrol paths.
static void ToolsMenu(App& app) {
    const bool selection = !app.scene.selection.empty() && app.scene.selectedFile == app.activeMob;
    ImGui::SetNextItemWidth(90);
    if (!ImGui::BeginCombo("##tools", "Tools", ImGuiComboFlags_HeightLarge)) return;
    if (ImGui::Selectable("Offset...", false, selection ? 0 : ImGuiSelectableFlags_Disabled)) app.offsetOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move the selection along one axis by an exact value");
    if (ImGui::Selectable("Randomize...", false, selection ? 0 : ImGuiSelectableFlags_Disabled)) app.randomOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Give each selected object a random position, rotation or complection in a range (like ei_maper)");
    if (ImGui::Selectable("Merge from another map...", false, app.mobs.size() >= 2 ? 0 : ImGuiSelectableFlags_Disabled)) app.mergeOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", i18n::Tr("Copy the objects of another loaded .mob into the active map, in place: by kind, by name, skipping what the active map already has at the same spot"));
    if (ImGui::Selectable("Minimap...", false, app.terrainLoaded ? 0 : ImGuiSelectableFlags_Disabled)) {
        app.minimapOpen = true;
        if (app.minimapPath[0] == '\0') {
            std::string dir = app.lib.gif.lastDirectory.empty() ? config::DefaultSaveDir() : app.lib.gif.lastDirectory;
            // Named after the .mpr file (zone6x.mpr -> zone6xmap), not the name inside it (which may be zone6).
            std::string stem = std::filesystem::path(app.terrainPath).stem().string();
            for (char& ch : stem) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::snprintf(app.minimapPath, sizeof(app.minimapPath), "%s/%smap.mmp", dir.c_str(), stem.c_str());
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Export a minimap texture of the terrain, like ZoneView's <zone>map");
    if (ImGui::Selectable("New terrain...", false, app.terrainLoaded ? 0 : ImGuiSelectableFlags_Disabled)) {
        app.newTerrainOpen = true;
        if (app.newTerrainPath[0] == '\0') {
            const std::string dir = std::filesystem::path(app.terrainPath).parent_path().string();
            std::snprintf(app.newTerrainPath, sizeof(app.newTerrainPath), "%s/newzone.mpr", dir.c_str());
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new flat terrain (.mpr) from scratch, with the open terrain's textures and materials");
    if (ImGui::Selectable("Crop terrain...", false, app.terrainLoaded ? 0 : ImGuiSelectableFlags_Disabled)) {
        app.cropOpen = true;
        if (app.cropPath[0] == '\0') {
            const std::filesystem::path p(app.terrainPath);
            std::snprintf(app.cropPath, sizeof(app.cropPath), "%s", (p.parent_path() / (p.stem().string() + "-crop.mpr")).string().c_str());
        }
        app.cropRect[2] = std::min(app.cropRect[2], app.terrain.sectorsX);
        app.cropRect[3] = std::min(app.cropRect[3], app.terrain.sectorsY);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A smaller copy of the terrain: a rectangle of its sectors, with the open maps' objects inside it (a lobby or base zone from a big zone)");
    ImGui::Separator();
    if (ImGui::Selectable("MOB parameters...", false, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) app.mobParamsOpen = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The active map's kind and WORLD_SET (wind, time, ambient, sun), like ei_maper's");
    if (ImGui::Selectable("Undo history", app.historyOpen, ImGuiSelectableFlags_DontClosePopups)) app.historyOpen = !app.historyOpen;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A window listing the steps undo and redo can take; a click goes to one");
    if (ImGui::Selectable(app.simulating ? "Stop the patrol simulation" : "Simulate patrols", app.simulating, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
        if (app.simulating) StopSimulation(app);
        else { app.simObjects = 0; StartSimulation(app); }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kSimLimits);
    // The navmesh again now, even without an edit (Save then writes it).
    bool hasGraph = false;
    for (auto& m : app.mobs) hasGraph |= m->file.aiGraphBytes != 0;
    if (ImGui::Selectable("Rebuild navmesh", false, hasGraph && app.terrain.sectorsX > 0 ? 0 : ImGuiSelectableFlags_Disabled)) {
        StartNavmeshJob(app, [&app](const NavResult& res) {
            if (res.payload.empty()) { app.filesMessage = "navmesh not rebuilt: " + res.err; return; }
            bool changed = false;
            std::string done;
            for (auto& m : app.mobs)
                if (m->file.aiGraphBytes) {
                    const std::vector<uint8_t> before = m->file.bytes;
                    mob::SetAiGraph(m->file, res.payload);
                    changed |= m->file.bytes != before;
                    done += (done.empty() ? "" : ", ") + m->file.fileName;
                }
            app.filesMessage = "navmesh rebuilt in " + done + (res.missing ? " (" + std::to_string(res.missing) + " objects without a figure left out)" : "") +
                               (changed ? ": save to write it" : " (it was already up to date)");
            if (app.scene.options.navCompare) BuildCompareNavmesh(app);
        });
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Builds the navmesh (AI_GRAPH) of the open zone map again from the terrain and the open maps' objects,\n"
                          "as the game does, without an edit first. Save writes it. Needs a terrain and a map that has a navmesh.");
    ImGui::Separator();
    const std::string resetLabel = "Clear patrol paths (" + ui::BindName(app.lib.mapKeys[config::kKeyResetPaths]) + ")";
    if (ImGui::Selectable(resetLabel.c_str(), false, selection ? 0 : ImGuiSelectableFlags_Disabled)) ResetLogicPaths(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Removes every patrol point of the selected units (ei_maper's Reset logic paths)");
    if (ImGui::Selectable("Save active MOB as...", false, app.mobs.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
        app.saveAsOpen = true;
        app.saveAsMessage.clear();
        std::snprintf(app.saveAsPath, sizeof(app.saveAsPath), "%s", app.mobs[app.activeMob]->file.path.c_str());
    }
    ImGui::EndCombo();
}

void Toolbar(App& app) {
    MapViewOptions& o = app.scene.options;
    // What is drawn (the layers), in a dropdown to keep the bar short; its label says what is hidden.
    {
        const int hidden = !o.terrain + !o.water + !o.objects + !o.units + !o.markers;
        const std::string label = hidden ? "Layers (" + std::to_string(5 - hidden) + "/5)" : std::string("Layers");
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##show", label.c_str(), ImGuiComboFlags_HeightLarge)) {
            ImGui::Checkbox("Terrain", &o.terrain);
            ImGui::Checkbox("Water", &o.water);
            ImGui::Checkbox("Objects", &o.objects);
            ImGui::Checkbox("Units", &o.units);
            if (ImGui::Checkbox("Units: idle pose", &o.poseUnits)) app.scene.DropModels();
            ImGui::SameLine();
            ImGui::BeginDisabled(!o.poseUnits);
            ImGui::Checkbox("Animated", &o.animateUnits);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("They play their animations at the game's 15 frames a second: idle; walking when they move in the patrol simulation");
            ImGui::SetItemTooltip("Units (and other animated figures) stand in their idle animation's first frame, as in the game; off: the T-pose");
            ImGui::Checkbox("Markers", &o.markers);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lights (yellow), particles (magenta), sounds (cyan) and objects\nwhose figure is missing (red)");
            if (ImGui::Checkbox("Walkability", &o.walkability) && o.walkability) app.scene.BuildWalkGrid();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Where units can walk, as the game computes it, for the navmesh layer below. Red: blocked; orange: hard ground (water, slopes, obstacles). Patrols route on it.");
            ImGui::Checkbox("Game navmesh (AI_GRAPH)", &o.navmesh);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The zone map's stored navmesh: a node per 4 x 4 units, lines to the neighbours a unit can reach (green cheap, red costly), red squares where it can go nowhere.");
            ImGui::SetNextItemWidth(110);
            if (ImGui::SliderInt("Navmesh layer", &o.navLayer, 0, mob::kAiLayers - 1) && o.walkability) app.scene.BuildWalkGrid();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The graph has 8 layers: units use the one of their AI class");
            if (ImGui::Checkbox("Navmesh differences", &o.navCompare) && o.navCompare) BuildCompareNavmesh(app);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The map's navmesh against the one the game would build now, per 4 x 4 node: orange: only the map's walks there; blue: only the rebuilt one; yellow: both, other costs. Anything shown: out of date.\n"
                    "Now: %d orange, %d blue, %d yellow.%s",
                                  app.scene.navCompareGame, app.scene.navCompareEditor, app.scene.navCompareCost,
                                  app.navCompareNote.empty() ? "" : ("\n" + app.navCompareNote).c_str());
            if (o.navCompare) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Rebuild")) BuildCompareNavmesh(app);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build it again from the maps as they are now (after edits)");
            }
            ImGui::Checkbox("Script areas", &o.scriptAreas);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The areas the scripts declare (magenta), with their number and the quest objectives that use them. %zu loaded.\n"
                    "Alt + drag inside one moves it; on its edge, resizes it.",
                                  app.scene.scriptAreas.size());
            ImGui::Checkbox("Shadows", &o.shadows);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the lighting on: the sun's shadows of the terrain and the figures, on the terrain");
            if (ImGui::Checkbox("Selected unit's logic", &app.lib.logicAlways)) app.lib.SaveConfig();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The selected units' paths, points and radii also show outside logic mode\n(their points are selected and moved in logic mode)");
            if (ImGui::Checkbox("Logic: selected only", &app.lib.logicSelectedOnly)) app.lib.SaveConfig();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("In logic mode, only the selected units show their paths, patrol points and radii.\nOff: every unit of the active map shows them.");
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ToolsMenu(app);
    ImGui::SameLine();
    ImGui::Checkbox("Textured", &o.textured);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &o.wireframe);
    if (app.openQuest) {
        ImGui::SameLine();
        ImGui::Checkbox("Exits", &o.exits);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The quest's deploy areas (green) and exit areas (red), from its map.txt");
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) app.scene.FrameAll();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole map (%s)", ui::BindName(app.lib.mapKeys[config::kKeyResetCamera]).c_str());

    // Second row: movement speed, modes, and the status on the right.
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##speed", &app.lib.mapCameraSpeed, 0.1f, 5.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.lib.SaveConfig();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera speed of the movement keys (Ctrl+click to type a value)");
    ImGui::SameLine();
    bool logic = app.scene.logicMode;
    if (ImGui::Checkbox("Logic", &logic)) app.scene.logicMode = logic;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Units' behaviours: patrol paths and points (yellow), look directions (blue), guard radius (orange),\n"
                          "sentry places (green), help radius (purple). Only units can be selected. %s",
                          ui::BindName(app.lib.mapKeys[config::kKeyLogicMode]).c_str());
    // Blender-like modes: Object (select and edit objects), Tile paint, Sculpt; the active tool's settings follow.
    ImGui::SameLine();
    {
        static const char* const kModes[] = {"Object mode", "Tile paint", "Sculpt", "Water paint"};
        int mode = app.heightBrush ? 2 : app.tileBrush ? (app.brushWater ? 3 : 1) : 0;
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##mode", kModes[mode])) {
            for (int i = 0; i < 4; ++i) {
                const bool needsTerrain = i > 0 && !app.terrainLoaded;
                if (ImGui::Selectable(kModes[i], i == mode, needsTerrain ? ImGuiSelectableFlags_Disabled : 0)) {
                    app.tileBrush = i == 1 || i == 3;
                    app.heightBrush = i == 2;
                    if (i == 1 || i == 3) app.brushWater = i == 3;
                }
                if (needsTerrain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Load a terrain (.mpr) first");
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The editing mode, as in Blender: objects, painting the terrain's tiles, shaping its ground, painting its water.\n%s shows the mode's tools in the view, %s hides the sidebar.",
                              ui::BindName(app.lib.mapKeys[config::kKeyToolShelf]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeySidebar]).c_str());
        if (mode == 1 && app.materialBrush && !app.brushWater) {
            ImGui::SameLine();
            tilemat::Terrain* tm = tilemat::Lookup(app.terrain.name);
            const bool chosen = tm && app.brushMaterialId >= 0 && app.brushMaterialId < static_cast<int>(tm->materials.size());
            if (chosen && !tm->materials[static_cast<size_t>(app.brushMaterialId)].baseTiles.empty()) { TileImage(app, tm->materials[static_cast<size_t>(app.brushMaterialId)].baseTiles[0], ImGui::GetFrameHeight()); ImGui::SameLine(); }
            ImGui::TextDisabled("ground %s (transitions by themselves)", chosen ? tm->materials[static_cast<size_t>(app.brushMaterialId)].name.c_str() : "?");
        } else if (mode == 1) {
            ImGui::SameLine();
            TileImage(app, std::max(app.brushTile, 0), ImGui::GetFrameHeight(), app.brushRotation);
            ImGui::SameLine();
            ImGui::TextDisabled("tile %d, turn %d (R , . Ctrl+wheel; Space: tiles; %s: the tile at the mouse)  %s", app.brushTile, app.brushRotation * 90,
                               ui::BindName(app.lib.mapKeys[config::kKeyTurnTile]).c_str(), app.brushWater ? "water" : "land");
        } else if (mode == 2) {
            static const char* const kTools[] = {"Raise", "Lower", "Smooth", "Flatten"};
            for (int i = 0; i < 4; ++i) { ImGui::SameLine(); ImGui::RadioButton(kTools[i], &app.heightMode, i); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##radius", &app.heightRadius, 1.0f, 32.0f, "radius %.1f");
            ImGui::SetItemTooltip("F + move the mouse left/right");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##strength", &app.heightStrength, 0.2f, 20.0f, "strength %.1f");
            ImGui::SetItemTooltip("Shift+F + move the mouse left/right");
        }
    }
    if (app.xf.mode != Transform::None) { // a move or scale in progress: how it works
        ImGui::SameLine();
        const char* axis[8] = {"free", "X", "Y", "X Y", "Z", "X Z", "Y Z", "X Y Z"};
        if (!app.xf.typed.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 1.0f, 1.0f), "typed: %s%s", app.xf.typed.c_str(), app.xf.mode == Transform::Rotate ? " deg" : "");
            ImGui::SameLine();
        }
        if (app.xf.mode == Transform::Rotate)
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "Rotate about %s  %.1f deg   X/Y/Z axis, type degrees, Ctrl free angle, click/Enter apply, right-click/Esc cancel",
                               axis[app.xf.axes & 7], app.xf.angle);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "%s %s  %.2f %.2f %.2f   X/Y/Z axis, Shift+axis all but, type a value (5 or 1,-2), click/Enter apply, right-click/Esc cancel",
                               app.xf.mode == Transform::Move ? "Move" : "Scale", axis[app.xf.axes & 7], app.xf.delta.x, app.xf.delta.y, app.xf.delta.z);
    } else if (app.questDirty || app.mqTextDirty || AnyMobDirty(app)) { // unsaved: say so where it is seen, with the way to save
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.1f, 1.0f));
        const std::string label = "Save (" + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ")";
        if (ImGui::Button(label.c_str())) SaveQuestChanges(app);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            std::string what;
            for (auto& m : app.mobs) if (m->Dirty()) what += "\n  " + m->file.fileName;
            if (TerrainUnsaved(app)) what += "\n  the terrain " + std::filesystem::path(app.terrainPath).filename().string();
            if (app.questDirty) what += "\n  the quest's areas";
            if (app.mqTextDirty) what += "\n  " + app.mqEntry;
            ImGui::SetTooltip("Unsaved:%s", what.c_str());
        }
        // Regenerate the zone's navmesh (AI_GRAPH) when saving, as the game builds it (navmesh_gen.hpp).
        ImGui::SameLine();
        if (ImGui::Checkbox("Navmesh", &app.lib.mapRegenNavmesh)) app.lib.SaveConfig();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rebuild the navmesh on save: the open zone map's AI_GRAPH is built again from the terrain and the open maps' objects, as the game does. A few seconds on big maps.");
    }
    // Always one item after SameLine, even empty: otherwise the viewport below would start on this line.
    ImGui::SameLine();
    ImGui::TextUnformatted("");
}

// The bar under the view: lighting (on/off, file, hour), the active map, what is loading and where the
// mouse points.
void BottomBar(App& app) {
    { // Settings > Performance, applied every frame
        MapViewOptions& o = app.scene.options;
        o.drawDistance = app.lib.mapDrawDistance;
        static int lowDetailSeen = -1;
        if (lowDetailSeen != static_cast<int>(app.lib.mapLowDetail)) {
            lowDetailSeen = app.lib.mapLowDetail;
            const bool low = app.lib.mapLowDetail;
            o.shadows = !low; o.dressUnits = !low; o.poseUnits = !low;
            if (low) app.lib.lightingOn = false;
            app.scene.DropModels();
        }
    }
    if (app.heightBrush || app.tileBrush) { // the mode's mouse and keys, as Blender's status bar
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.4f, 1), app.heightBrush
            ? "Sculpt: left drag shapes the ground (one undo per stroke)  F radius  Shift+F strength  %s tool shelf  (Tools > Rebuild navmesh after)"
            : app.materialBrush ? "Tile paint by ground: left drag paints (the transitions around follow)  Alt+click picks the ground  Space / 1-8: by tile  %s Shift+wheel turn the tile at the mouse  %s tool shelf"
            : app.brushWater ? "Water paint: left drag paints the water tile and material  Space tiles at the mouse  Alt+click picks  1-8 quick tiles  R , . Ctrl+wheel turn the brush  %s Shift+wheel turn the tile at the mouse  %s tool shelf"
            : "Tile paint: left drag paints  Space tiles at the mouse  Alt+click picks a tile  1-8 quick tiles  R , . Ctrl+wheel turn the brush  %s Shift+wheel turn the tile at the mouse  %s tool shelf",
            ui::BindName(app.lib.mapKeys[config::kKeyTurnTile]).c_str(), ui::BindName(app.lib.mapKeys[config::kKeyToolShelf]).c_str());
        ImGui::SameLine();
    }
    if (ImGui::Checkbox("Lighting", &app.lib.lightingOn)) app.lib.SaveConfig();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light the map with a lighting file of the Settings tab at the map's time of day. %s",
                          ui::BindName(app.lib.mapKeys[config::kKeyLighting]).c_str());
    if (app.lib.lightingOn) {
        const lighting::Table* t = ChosenLighting(app);
        ImGui::SameLine();
        if (!t) {
            ImGui::TextDisabled("(add lights*.ini files in Settings > Lighting)");
        } else {
            ImGui::SetNextItemWidth(170);
            if (ImGui::BeginCombo("##lightini", t->name.c_str())) {
                for (const lighting::Table& other : app.lightTables)
                    if (ImGui::Selectable(other.name.c_str(), &other == t)) { app.lib.lightingChoice = other.path; app.lib.SaveConfig(); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->path.c_str());
            ImGui::SameLine();
            float h = MapHour(app);
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderFloat("##hour", &h, 0.0f, 24.0f, "%.1f h")) { app.hour = h; app.hourSet = true; }
            if (ImGui::IsItemDeactivatedAfterEdit()) { app.lib.mapHour = app.hour; app.lib.SaveConfig(); } // remembered
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time of day (starts at the map's own time; right-click: back to it)");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { app.hourSet = false; app.lib.mapHour = -1.0f; app.lib.SaveConfig(); }
        }
    }
    if (!app.mobs.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  active: %s", app.mobs[app.activeMob]->file.fileName.c_str());
    }
    char status[96] = "";
    if (app.scene.modelsPending > 0) std::snprintf(status, sizeof(status), "loading %d figures...", app.scene.modelsPending);
    else if (app.hoverGround) std::snprintf(status, sizeof(status), "x %.1f  y %.1f  z %.2f", app.ground.x, app.ground.y, app.ground.z);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", status);
    if (!app.questMessage.empty()) { // the last action's result (saved, deleted, copied...)
        ImGui::SameLine();
        ImGui::TextDisabled("   %s", app.questMessage.c_str());
    }
}

// Moves the camera's target freely: its height only changes with up/down, whatever the ground does.
void MoveTarget(App& app, float dx, float dy, float dz) {
    OrbitCamera& cam = app.scene.camera;
    cam.targetX += dx;
    cam.targetY += dy;
    cam.targetZ += dz;
}


} // namespace mapedit
