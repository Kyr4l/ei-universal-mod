// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: The view: move (G) and scale (T), the logic points (logic mode), input, overlays, the tool shelf.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ---- move (G) and scale (T) ------------------------------------------------------------------------
// Blender's way: the key starts it, the mouse drives it, X / Y / Z keep it to one axis (Shift+axis: all
// but that one), a left click or Enter applies it, a right click or Esc puts everything back. Moving
// follows the ground under the mouse (Z: the mouse's height on screen); scaling changes the complection
// (like ei_maper's scale tool) with the mouse's horizontal movement.

// The world position of the selection's centre as drawn (figures stand on the ground).
static mob::Vec3 DrawnCentre(App& app, const mob::Vec3& c) {
    return {c.x, c.y, app.scene.Ground(c.x, c.y) + std::max(c.z, 0.0f)};
}

// The mouse's angle (radians) around the selection's centre on screen, for rotating.
static float ScreenAngle(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    mob::Vec3 c = DrawnCentre(app, app.xf.centre);
    float fx, fy;
    if (!app.scene.Project({c.x, c.y, c.z}, fx, fy)) return 0.0f;
    return std::atan2(mouse.y - (min.y + fy * size.y), mouse.x - (min.x + fx * size.x));
}

void StartTransform(App& app, Transform::Mode mode, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    if (app.simulating) return; // editing waits for the simulation to stop
    if (app.scene.selectedFile != app.activeMob || app.scene.selection.empty() || app.mobs.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    // Logic points selected (logic mode): G moves them rather than the units.
    PruneLogicPoints(app);
    const bool points = !app.scene.logicPoints.empty();
    if (points && mode != Transform::Move) return;
    Transform& x = app.xf;
    x = Transform{};
    x.mode = mode;
    x.axes = mode == Transform::Move ? 1 | 2 : mode == Transform::Rotate ? 4 : 1 | 2 | 4;
    x.startMouse = mouse;
    x.file = f.path;
    if (points) {
        x.points = app.scene.logicPoints;
        x.bytesBefore = f.bytes;
        for (const LogicPointRef& r : x.points) {
            if (r.trap) x.trapsBefore.emplace(r.object, std::make_pair(f.objects[r.object].trapAreas, f.objects[r.object].trapTargets));
            else x.logicsBefore.emplace(std::make_pair(r.object, r.logic), f.objects[r.object].logics[r.logic]);
            mob::Vec3 p;
            LogicPointAt(f, r, p);
            x.centre.x += p.x; x.centre.y += p.y; x.centre.z += p.z;
        }
    } else {
        x.before = CaptureObjects(f, app.scene.selection);
    }
    for (const ObjectState& st : x.before) { x.centre.x += st.position.x; x.centre.y += st.position.y; x.centre.z += st.position.z; }
    const float n = static_cast<float>(std::max<size_t>(points ? x.points.size() : x.before.size(), 1));
    x.centre = {x.centre.x / n, x.centre.y / n, x.centre.z / n};
    ImVec2 at((mouse.x - min.x) / std::max(size.x, 1.0f), (mouse.y - min.y) / std::max(size.y, 1.0f));
    x.haveGround = app.scene.GroundAt(at.x, at.y, x.startGround);
    x.startAngle = ScreenAngle(app, mouse, min, size);
}

static void EndTransform(App& app, bool apply) {
    Transform& x = app.xf;
    MobEntry* m = FindMob(app, x.file);
    if (m && !x.points.empty()) { // logic points
        if (apply) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = x.file;
            step.bytes = std::move(x.bytesBefore);
            PushUndo(app, std::move(step), "Move logic points");
            app.checksDirty = true;
            app.listedKey.clear();
        } else {
            m->file.bytes = std::move(x.bytesBefore);
            mob::Reparse(m->file);
        }
    } else if (m) {
        if (apply) {
            EditStep step;
            step.kind = EditStep::Objects;
            step.file = x.file;
            step.objects = x.before;
            PushUndo(app, std::move(step), std::string(x.mode == Transform::Move ? "Move " : x.mode == Transform::Rotate ? "Rotate " : "Scale ") + std::to_string(x.before.size()) + " object(s)");
            app.checksDirty = true;
        } else {
            ApplyObjects(m->file, x.before);
        }
    }
    x = Transform{};
    app.scene.guides.clear();
}

// The world position of an object as drawn (for the guide lines): figures stand on the ground.
static mob::Vec3 GuideCentre(App& app) {
    mob::Vec3 c = app.xf.centre;
    return {c.x, c.y, app.scene.Ground(c.x, c.y) + std::max(c.z, 0.0f) + 0.3f};
}

static void UpdateTransform(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size) {
    Transform& x = app.xf;
    MobEntry* m = FindMob(app, x.file);
    if (!m) { x = Transform{}; return; }
    ImGuiIO& io = ImGui::GetIO();
    // Axis keys (by the letter the layout prints: X, Y and Z mean the axes).
    for (int a = 0; a < 3; ++a) {
        if (!ImGui::IsKeyPressed(a == 0 ? ImGuiKey_X : a == 1 ? ImGuiKey_Y : ImGuiKey_Z, false)) continue;
        if (x.mode == Transform::Rotate) { x.axes = x.axes == (1 << a) ? 4 : (1 << a); continue; } // one axis; again: back to Z
        int want = io.KeyShift ? (7 & ~(1 << a)) : (1 << a);
        const int free = x.mode == Transform::Move ? 3 : 7;
        x.axes = x.axes == want ? free : want; // the same key again: back to free
    }
    // Typed values (like Blender): digits, '.', '-' and ',' between the axes' values; Backspace erases.
    for (ImWchar c : io.InputQueueCharacters)
        if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-') x.typed += static_cast<char>(c);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !x.typed.empty()) x.typed.pop_back();
    const int axisCount = x.mode == Transform::Rotate ? 1 : ((x.axes & 1) != 0) + ((x.axes & 2) != 0) + ((x.axes & 4) != 0);
    const std::vector<float> typed = TypedValues(x.typed, axisCount);
    auto typedFor = [&](int axis) { // the typed value of this axis (0..2), by its place among the axes in use
        int i = 0;
        for (int a = 0; a < axis; ++a) if (x.axes & (1 << a)) ++i;
        return typed[i];
    };
    std::vector<ObjectState> now = x.before;
    if (x.mode == Transform::Move) {
        mob::Vec3 d{0, 0, 0};
        if (x.axes & 3) {
            ImVec2 at((mouse.x - min.x) / std::max(size.x, 1.0f), (mouse.y - min.y) / std::max(size.y, 1.0f));
            fig::Vec3 g;
            if (x.haveGround && app.scene.GroundAt(at.x, at.y, g)) { d.x = g.x - x.startGround.x; d.y = g.y - x.startGround.y; }
        }
        if (x.axes & 4) d.z = (x.startMouse.y - mouse.y) * app.scene.camera.distance / 600.0f;
        if (!(x.axes & 1)) d.x = 0;
        if (!(x.axes & 2)) d.y = 0;
        if (!(x.axes & 4)) d.z = 0;
        if (!io.KeyCtrl) { d.x = std::round(d.x * 10) / 10; d.y = std::round(d.y * 10) / 10; d.z = std::round(d.z * 10) / 10; }
        if (!typed.empty()) d = {(x.axes & 1) ? typedFor(0) : 0.0f, (x.axes & 2) ? typedFor(1) : 0.0f, (x.axes & 4) ? typedFor(2) : 0.0f};
        if (!x.points.empty()) { // logic points: each keeps to the ground (plus any Z offset)
            std::map<std::pair<int, int>, mob::Logic> now = x.logicsBefore;
            auto traps = x.trapsBefore;
            for (const LogicPointRef& r : x.points) {
                if (r.trap) { // a trap's area or cast point: x and y only (an area's z is its radius)
                    auto& list = r.trap == 1 ? traps[r.object].first : traps[r.object].second;
                    list[static_cast<size_t>(r.point)].x += d.x;
                    list[static_cast<size_t>(r.point)].y += d.y;
                    continue;
                }
                mob::Logic& g = now[std::make_pair(r.object, r.logic)];
                const mob::Logic& was = x.logicsBefore[std::make_pair(r.object, r.logic)];
                mob::Vec3 p = r.place ? was.guardPlace : r.look >= 0 ? was.patrol[r.point].looks[r.look].position : was.patrol[r.point].position;
                p.x += d.x;
                p.y += d.y;
                p.z = r.place ? p.z + d.z : app.scene.Ground(p.x, p.y) + d.z;
                SetLogicPoint(g, r, p);
            }
            m->file.bytes = x.bytesBefore;
            mob::Reparse(m->file);
            for (auto& [key, g] : now) mob::SetLogic(m->file, key.first, key.second, g);
            for (auto& [oi, lists] : traps) {
                mob::ReplaceField(m->file, oi, mob::kMagicTrapAreas, mob::TrapListPayload(true, lists.first));
                mob::ReplaceField(m->file, oi, mob::kMagicTrapTargets, mob::TrapListPayload(false, lists.second));
            }
            x.delta = d;
            app.scene.guides.clear();
            if (x.axes != 3) {
                mob::Vec3 c = GuideCentre(app);
                const float L = 400.0f;
                if (x.axes & 1) app.scene.guides.push_back({{c.x - L, c.y, c.z}, {c.x + L, c.y, c.z}, 0.95f, 0.25f, 0.25f});
                if (x.axes & 2) app.scene.guides.push_back({{c.x, c.y - L, c.z}, {c.x, c.y + L, c.z}, 0.35f, 0.9f, 0.3f});
                if (x.axes & 4) app.scene.guides.push_back({{c.x, c.y, c.z - L}, {c.x, c.y, c.z + L}, 0.3f, 0.5f, 1.0f});
            }
            return;
        }
        for (ObjectState& st : now) { st.position.x += d.x; st.position.y += d.y; st.position.z += d.z; }
        x.delta = d;
    } else if (x.mode == Transform::Rotate) {
        // The angle the mouse turned around the centre on screen; turning counterclockwise on screen turns
        // the objects counterclockwise seen from above (about Z). 1 degree steps unless Ctrl is held.
        float turned = -(ScreenAngle(app, mouse, min, size) - x.startAngle) * 180.0f / 3.14159265f;
        while (turned > 180.0f) turned -= 360.0f;
        while (turned < -180.0f) turned += 360.0f;
        if (!io.KeyCtrl) turned = std::round(turned);
        if (!typed.empty()) turned = typed[0];
        x.angle = turned;
        const int axis = x.axes == 1 ? 0 : x.axes == 2 ? 1 : 2;
        const float half = turned * 3.14159265f / 360.0f;
        const fig::Quat q{std::cos(half), axis == 0 ? std::sin(half) : 0.0f, axis == 1 ? std::sin(half) : 0.0f, axis == 2 ? std::sin(half) : 0.0f};
        for (ObjectState& st : now) {
            // Each object turns in place, and around the selection's centre with the others (world axes).
            fig::Quat r = fig::QuatNormalize(fig::QuatMul(q, fig::Quat{st.rotation[0], st.rotation[1], st.rotation[2], st.rotation[3]}));
            st.rotation[0] = r.w; st.rotation[1] = r.x; st.rotation[2] = r.y; st.rotation[3] = r.z;
            fig::Vec3 off = fig::QuatRotate(q, {st.position.x - x.centre.x, st.position.y - x.centre.y, st.position.z - x.centre.z});
            st.position = {x.centre.x + off.x, x.centre.y + off.y, x.centre.z + off.z};
        }
        x.delta = {0, 0, 0};
    } else {
        float k = (mouse.x - x.startMouse.x) / 200.0f;
        if (!io.KeyCtrl) k = std::round(k * 100) / 100;
        mob::Vec3 d{(x.axes & 1) ? k : 0.0f, (x.axes & 2) ? k : 0.0f, (x.axes & 4) ? k : 0.0f};
        if (!typed.empty()) d = {(x.axes & 1) ? typedFor(0) : 0.0f, (x.axes & 2) ? typedFor(1) : 0.0f, (x.axes & 4) ? typedFor(2) : 0.0f};
        for (ObjectState& st : now) { st.complection.x += d.x; st.complection.y += d.y; st.complection.z += d.z; }
        x.delta = d;
    }
    ApplyObjects(m->file, now);
    // Guide lines for a constrained axis: X red, Y green, Z blue.
    app.scene.guides.clear();
    const bool constrained = x.mode == Transform::Rotate || x.axes != (x.mode == Transform::Move ? 3 : 7);
    if (constrained) {
        mob::Vec3 c = GuideCentre(app);
        const float L = 400.0f;
        if (x.axes & 1) app.scene.guides.push_back({{c.x - L, c.y, c.z}, {c.x + L, c.y, c.z}, 0.95f, 0.25f, 0.25f});
        if (x.axes & 2) app.scene.guides.push_back({{c.x, c.y - L, c.z}, {c.x, c.y + L, c.z}, 0.35f, 0.9f, 0.3f});
        if (x.axes & 4) app.scene.guides.push_back({{c.x, c.y, c.z - L}, {c.x, c.y, c.z + L}, 0.3f, 0.5f, 1.0f});
    }
}

// The keys of the Settings tab (by key position). Held: move forward/back/left/right relative to where
// the camera looks, up/down, faster. Pressed once: logic mode, next active map (its list shows while
// held), unload the last file, reset the camera, lighting. Not while a text field is being edited.
void Keys(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    GLFWwindow* window = glfwGetCurrentContext();
    app.switchListShown = false;
    if (!window) return;
    const int mods = ui::HeldMods(window);
    bool down[config::kMapKeyCount];
    for (int k = 0; k < config::kMapKeyCount; ++k) {
        config::KeyBind b = app.lib.mapKeys[k];
        if (b.mods & config::kModLetter) { b.key = ui::KeyForLetter(b.key); b.mods &= ~config::kModLetter; } // the key printing that letter
        // While a text field is being typed in, only the save key (Ctrl+S) works: the rest would type or move.
        const bool allowed = !io.WantTextInput || (k == config::kKeySave && b.mods != 0);
        down[k] = allowed && b.key > 0 && glfwGetKey(window, b.key) == GLFW_PRESS && (k < config::kFirstActionKey || mods == b.mods);
    }
    auto pressed = [&](int k) { return down[k] && !app.keyWasDown[k]; };
    if (pressed(config::kKeyLogicMode)) {
        app.scene.logicMode = !app.scene.logicMode;
        if (app.scene.logicMode) app.scene.FilterSelection([](const mob::Object& o) { return o.kind == mob::Kind::Unit; }); // only units stay selected
    }
    // The map list shows from the press until its modifiers (Ctrl by default) are let go, like Alt+Tab,
    // so it can be read; without modifiers, while the key is held.
    const config::KeyBind& switchBind = app.lib.mapKeys[config::kKeySwitchMob];
    if (pressed(config::kKeySwitchMob) && !app.mobs.empty()) {
        SetActiveMob(app, app.activeMob + 1);
        app.switchListLatched = true;
    }
    const int switchMods = switchBind.mods & ~config::kModLetter; // the letter flag is not a key that is held
    if (switchMods ? (mods & switchMods) != switchMods : !down[config::kKeySwitchMob]) app.switchListLatched = false;
    app.switchListShown = app.switchListLatched && app.mobs.size() > 1;
    if (pressed(config::kKeyUnloadLast)) UnloadLast(app);
    if (pressed(config::kKeyResetCamera)) app.scene.FrameAll();
    if (pressed(config::kKeyLighting)) { app.lib.lightingOn = !app.lib.lightingOn; app.lib.SaveConfig(); }
    if (pressed(config::kKeyToolShelf) && (app.tileBrush || app.heightBrush)) app.toolShelf = !app.toolShelf; // T: in Object mode it scales
    if (pressed(config::kKeySidebar)) app.sidebarShown = !app.sidebarShown;
    if (pressed(config::kKeySave) && app.xf.mode == Transform::None) {
        ApplyScriptEdit(app); // a script being edited goes in first
        SaveQuestChanges(app);
    }
    // G / T with a selection and the mouse over the view.
    bool transformStarted = false;
    if (app.xf.mode == Transform::None && app.viewHovered && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty() && !app.tileBrush && !app.heightBrush) {
        if (pressed(config::kKeyMove)) { StartTransform(app, Transform::Move, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
        else if (pressed(config::kKeyScale)) { StartTransform(app, Transform::Scale, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
        else if (pressed(config::kKeyRotate)) { StartTransform(app, Transform::Rotate, io.MousePos, app.viewMin, app.viewSize); transformStarted = true; }
    }
    if (pressed(config::kKeyFind)) { app.findOpen = true; app.findFocus = true; }
    if (app.xf.mode == Transform::None) { TerrainBrushKeys(app); HeightBrushKeys(app); }
    if (app.xf.mode == Transform::None && app.tileBrush && pressed(config::kKeyRotate)) app.brushRotation = (app.brushRotation + 1) % 4; // R: turn the tile
    if (pressed(config::kKeySelectAll) && app.xf.mode == Transform::None) { SelectAll(app); app.requestTab = SideTab::Objects; }
    if (app.xf.mode == Transform::None) {
        if (pressed(config::kKeyDelete)) {
            if (!app.scene.logicPoints.empty()) DeleteLogicPoints(app); // the selected points, not their unit or trap
            else DeleteSelection(app);
        }
        if (pressed(config::kKeyCopy)) CopySelection(app);
        if (pressed(config::kKeyPaste)) Paste(app, ImGui::GetIO().KeyShift);
        if (pressed(config::kKeyDuplicate)) Duplicate(app);
        if (pressed(config::kKeyResetPaths)) ResetLogicPaths(app);
        if (pressed(config::kKeyNewObject)) { app.newOpen = true; app.newMessage.clear(); }
    }
    if (pressed(config::kKeyUndo) && app.xf.mode == Transform::None) UndoRedo(app, false);
    if (pressed(config::kKeyRedo) && app.xf.mode == Transform::None) UndoRedo(app, true);
    for (int k = 0; k < config::kMapKeyCount; ++k) app.keyWasDown[k] = down[k];
    if (app.xf.mode != Transform::None || transformStarted) return; // the camera stays still while transforming

    // Movement ignores modifiers (Shift is the default "faster"), but not with Ctrl held: Ctrl+... are actions.
    if (mods & config::kModCtrl) return;
    float forward = (down[config::kKeyForward] ? 1.0f : 0.0f) - (down[config::kKeyBack] ? 1.0f : 0.0f);
    float right = (down[config::kKeyRight] ? 1.0f : 0.0f) - (down[config::kKeyLeft] ? 1.0f : 0.0f);
    float up = (down[config::kKeyUp] ? 1.0f : 0.0f) - (down[config::kKeyDown] ? 1.0f : 0.0f);
    if (forward == 0 && right == 0 && up == 0) return;
    const OrbitCamera& cam = app.scene.camera;
    float speed = std::max(cam.distance, 8.0f) * 0.9f * app.lib.mapCameraSpeed * io.DeltaTime * (down[config::kKeyFast] ? 3.0f : 1.0f);
    float yaw = cam.yawDeg * 3.14159265f / 180.0f;
    // The camera sits at +(cos yaw, sin yaw) from its target, so it looks along -(cos yaw, sin yaw).
    float fx = -std::cos(yaw), fy = -std::sin(yaw), rx = -std::sin(yaw), ry = std::cos(yaw); // right = forward x up
    MoveTarget(app, (fx * forward + rx * right) * speed, (fy * forward + ry * right) * speed, up * speed * 0.6f);
}

// An area's handles: 0-3 corners (x1 y1, x2 y1, x2 y2, x1 y2), 4-7 sides (x1, x2, y1, y2), 8 the centre.
// The script areas' drag squares: rect corners, sides and centre; a round area's centre and rim (east point).
struct ScriptHandle { std::string file; size_t index = 0; float x = 0, y = 0; int resize = 0; };
static std::vector<ScriptHandle> ScriptHandles(App& app) {
    std::vector<ScriptHandle> out;
    if (!app.scene.options.scriptAreas) return out;
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            const float* v = calls[i].v;
            auto add = [&](float x, float y, int resize) { out.push_back({m->file.path, i, x, y, resize}); };
            if (calls[i].round) { add(v[0], v[1], 0); add(v[0] + v[2], v[1], 1); continue; }
            const float mx = (v[0] + v[2]) * 0.5f, my = (v[1] + v[3]) * 0.5f;
            add(v[0], v[1], 2 | 4); add(v[2], v[1], 8 | 4); add(v[2], v[3], 8 | 16); add(v[0], v[3], 2 | 16);
            add(v[0], my, 2); add(v[2], my, 8); add(mx, v[1], 4); add(mx, v[3], 16); add(mx, my, 0);
        }
    }
    return out;
}

static mob::Vec3 HandlePoint(const quest::Rect& r, int h) {
    const float mx = (r.x1 + r.x2) * 0.5f, my = (r.y1 + r.y2) * 0.5f;
    switch (h) {
    case 0: return {r.x1, r.y1, 0};
    case 1: return {r.x2, r.y1, 0};
    case 2: return {r.x2, r.y2, 0};
    case 3: return {r.x1, r.y2, 0};
    case 4: return {r.x1, my, 0};
    case 5: return {r.x2, my, 0};
    case 6: return {mx, r.y1, 0};
    case 7: return {mx, r.y2, 0};
    default: return {mx, my, 0};
    }
}

// Starts dragging the area handle under the mouse (within a few pixels), if any.
static bool GrabAreaHandle(App& app, ImVec2 mouse, ImVec2 min, ImVec2 size, fig::Vec3 ground) {
    if (!app.openQuest || !app.scene.options.exits) return false;
    float best = 9.0f * 9.0f;
    App::RectDrag found;
    for (size_t ei = 0; ei < app.openQuest->shown.exits.size(); ++ei) {
        const quest::Exit& e = app.openQuest->shown.exits[ei];
        for (int which = 0; which < 2; ++which) {
            const quest::Rect& r = which ? e.remove : e.deploy;
            if (!r.set) continue;
            for (int h = 0; h < 9; ++h) {
                mob::Vec3 hp = HandlePoint(r, h);
                float fx, fy;
                if (!app.scene.Project({hp.x, hp.y, app.scene.Ground(hp.x, hp.y) + 0.12f}, fx, fy)) continue;
                float dx = min.x + fx * size.x - mouse.x, dy = min.y + fy * size.y - mouse.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    found = App::RectDrag{true, static_cast<int>(ei), which == 1, h, r, ground};
                }
            }
        }
    }
    if (!found.on) return false;
    PushAreaUndo(app, app.openQuest->shown.exits); // the areas before this drag
    app.rectDrag = found;
    return true;
}

// Moves the dragged handle to the ground point under the mouse (whole units unless Shift is held).
static void DragAreaHandle(App& app, fig::Vec3 ground, bool snap) {
    App::RectDrag& d = app.rectDrag;
    quest::Exit& e = app.openQuest->shown.exits[d.exit];
    quest::Rect& r = d.remove ? e.remove : e.deploy;
    auto round = [&](float v) { return snap ? std::round(v) : std::round(v * 10.0f) / 10.0f; };
    const float gx = round(ground.x), gy = round(ground.y);
    r = d.start;
    switch (d.handle) {
    case 0: r.x1 = gx; r.y1 = gy; break;
    case 1: r.x2 = gx; r.y1 = gy; break;
    case 2: r.x2 = gx; r.y2 = gy; break;
    case 3: r.x1 = gx; r.y2 = gy; break;
    case 4: r.x1 = gx; break;
    case 5: r.x2 = gx; break;
    case 6: r.y1 = gy; break;
    case 7: r.y2 = gy; break;
    default: {
        float dx = round(ground.x - d.from.x), dy = round(ground.y - d.from.y);
        r.x1 += dx; r.x2 += dx; r.y1 += dy; r.y2 += dy;
        break;
    }
    }
    app.questDirty = !SameAreas(app.openQuest->shown.exits, app.savedAreas);
}

// Drawn over the viewport: logic labels (behaviour, point numbers, waits) and, while the switch key is
// held, the list of maps with the active one marked.
// The crop rectangle's handles in world units: corners 0-3, edge middles 4-7 (west, east, south, north).
static mob::Vec3 CropHandlePoint(const App& app, int h) {
    const float x0 = app.cropRect[0] * 32.0f, y0 = app.cropRect[1] * 32.0f, x1 = x0 + app.cropRect[2] * 32.0f, y1 = y0 + app.cropRect[3] * 32.0f;
    const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    switch (h) {
    case 0: return {x0, y0, 0}; case 1: return {x1, y0, 0}; case 2: return {x1, y1, 0}; case 3: return {x0, y1, 0};
    case 4: return {x0, my, 0}; case 5: return {x1, my, 0}; case 6: return {mx, y0, 0}; default: return {mx, y1, 0};
    }
}
static bool CropHandleScreen(App& app, int h, ImVec2 min, ImVec2 size, ImVec2& out) {
    const mob::Vec3 p = CropHandlePoint(app, h);
    float fx, fy;
    if (!app.scene.Project({p.x, p.y, app.scene.Ground(p.x, p.y) + 0.3f}, fx, fy)) return false;
    out = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
    return true;
}

// Tools > Crop terrain: the rectangle is dragged in the view: a corner or an edge middle resizes it, the inside moves
// it, whole sectors. True while it takes the mouse.
static bool CropDragInput(App& app, ImVec2 local, ImVec2 min, ImVec2 size, bool hovered) {
    if (!app.cropOpen || !app.terrainLoaded) { app.cropDrag.on = false; return false; }
    const mpr::Map& m = app.terrain;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    fig::Vec3 g;
    const bool onGround = app.scene.GroundAt(local.x, local.y, g);
    App::CropDrag& d = app.cropDrag;
    if (!d.on) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
        int handle = -1;
        float best = 10.0f * 10.0f;
        for (int h = 0; h < 8; ++h) {
            ImVec2 sp;
            if (!CropHandleScreen(app, h, min, size, sp)) continue;
            const float dx = sp.x - mouse.x, dy = sp.y - mouse.y;
            if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; handle = h; }
        }
        if (handle < 0 && onGround && g.x >= app.cropRect[0] * 32.0f && g.x < (app.cropRect[0] + app.cropRect[2]) * 32.0f &&
            g.y >= app.cropRect[1] * 32.0f && g.y < (app.cropRect[1] + app.cropRect[3]) * 32.0f) handle = 8; // inside: move
        if (handle < 0) return false;
        d.on = true; d.handle = handle; d.from = g;
        for (int i = 0; i < 4; ++i) d.start[i] = app.cropRect[i];
        return true; // this function takes the mouse until the release (swallowLeftRelease would block it from running meanwhile)
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { d.on = false; return true; }
    if (!onGround) return true;
    auto sector = [](float v) { return static_cast<int>(std::lround(v / 32.0f)); };
    int x0 = d.start[0], y0 = d.start[1], x1 = x0 + d.start[2], y1 = y0 + d.start[3];
    const int gx = sector(g.x), gy = sector(g.y);
    switch (d.handle) {
    case 0: x0 = gx; y0 = gy; break;
    case 1: x1 = gx; y0 = gy; break;
    case 2: x1 = gx; y1 = gy; break;
    case 3: x0 = gx; y1 = gy; break;
    case 4: x0 = gx; break;
    case 5: x1 = gx; break;
    case 6: y0 = gy; break;
    case 7: y1 = gy; break;
    default: {
        const int dx = sector(g.x - d.from.x), dy = sector(g.y - d.from.y);
        x0 += dx; x1 += dx; y0 += dy; y1 += dy;
        break;
    }
    }
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (d.handle == 8) { // moving keeps the size inside the terrain
        x0 = std::clamp(x0, 0, m.sectorsX - d.start[2]); y0 = std::clamp(y0, 0, m.sectorsY - d.start[3]);
        x1 = x0 + d.start[2]; y1 = y0 + d.start[3];
    }
    x0 = std::clamp(x0, 0, m.sectorsX - 1); y0 = std::clamp(y0, 0, m.sectorsY - 1);
    x1 = std::clamp(x1, x0 + 1, m.sectorsX); y1 = std::clamp(y1, y0 + 1, m.sectorsY);
    app.cropRect[0] = x0; app.cropRect[1] = y0; app.cropRect[2] = x1 - x0; app.cropRect[3] = y1 - y0;
    return true;
}

// The tool shelf (T): the mode's tools over the view's left side, as Blender's, while painting or sculpting.
void ToolShelf(App& app, ImVec2 min, ImVec2 size) {
    if (!app.toolShelf || !(app.tileBrush || app.heightBrush) || !app.terrainLoaded) return;
    const float w = std::min(340.0f, size.x * 0.45f), h = size.y - 16;
    if (w < 120 || h < 80) return;
    ImGui::SetCursorScreenPos(ImVec2(min.x + 8, min.y + 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 0.92f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, h)); // as tall as its tools, scrolling past the view's height
    if (ImGui::BeginChild("##toolShelf", ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        if (app.heightBrush) SculptPanel(app, true); else TilePanel(app, true);
        ImGui::TextDisabled("%s hides this shelf", ui::BindName(app.lib.mapKeys[config::kKeyToolShelf]).c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Overlays(App& app, ImVec2 min, ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), true); // nothing over the bars around the view
    if (app.cropOpen && app.terrainLoaded) { // Tools > Crop terrain: the rectangle on the ground, a point every 4 units so it follows the relief
        const float x0 = app.cropRect[0] * 32.0f, y0 = app.cropRect[1] * 32.0f, x1 = x0 + app.cropRect[2] * 32.0f, y1 = y0 + app.cropRect[3] * 32.0f;
        std::vector<ImVec2> ring;
        auto add = [&](float x, float y) {
            float fx, fy;
            if (app.scene.Project({x, y, app.scene.Ground(x, y) + 0.3f}, fx, fy)) ring.push_back(ImVec2(min.x + fx * size.x, min.y + fy * size.y));
        };
        for (float x = x0; x < x1; x += 4) add(x, y0);
        for (float y = y0; y < y1; y += 4) add(x1, y);
        for (float x = x1; x > x0; x -= 4) add(x, y1);
        for (float y = y1; y > y0; y -= 4) add(x0, y);
        if (ring.size() >= 3) {
            draw->AddPolyline(ring.data(), static_cast<int>(ring.size()), IM_COL32(255, 220, 80, 230), ImDrawFlags_Closed, 3.0f);
            draw->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), IM_COL32(255, 220, 80, 28));
        }
        for (int h = 0; h < 8; ++h) { // the handles: corners and edge middles
            ImVec2 sp;
            if (!CropHandleScreen(app, h, min, size, sp)) continue;
            const bool held = app.cropDrag.on && app.cropDrag.handle == h;
            draw->AddRectFilled(ImVec2(sp.x - 5, sp.y - 5), ImVec2(sp.x + 5, sp.y + 5), held ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 220, 80, 255));
            draw->AddRect(ImVec2(sp.x - 5, sp.y - 5), ImVec2(sp.x + 5, sp.y + 5), IM_COL32(40, 30, 0, 255));
        }
    }
    auto label = [&](const mob::Vec3& p, float lift, ImU32 color, const std::string& text) {
        float fx, fy;
        if (!app.scene.Project({p.x, p.y, app.scene.Ground(p.x, p.y) + lift}, fx, fy) || fx < 0 || fx > 1 || fy < 0 || fy > 1) return;
        ImVec2 at(min.x + fx * size.x + 6, min.y + fy * size.y - 7);
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        draw->AddRectFilled(ImVec2(at.x - 2, at.y - 1), ImVec2(at.x + ts.x + 2, at.y + ts.y + 1), IM_COL32(0, 0, 0, 150), 3.0f);
        draw->AddText(at, color, text.c_str());
    };
    for (size_t k = 0; k < app.missing.size() && k < 400; ++k) { // objects without a figure shown
        const MapScene::Missing& mi = app.missing[k];
        if (mi.file < 0 || mi.file >= static_cast<int>(app.mobs.size())) continue;
        const auto& objs = app.mobs[static_cast<size_t>(mi.file)]->file.objects;
        if (mi.object < static_cast<int>(objs.size())) label(objs[static_cast<size_t>(mi.object)].position, 0.5f, IM_COL32(255, 90, 70, 255), "? " + (mi.figure.empty() ? std::string("(no figure)") : mi.figure));
    }
    // The scripts' areas: their number and the objectives that use them.
    if (app.scene.options.scriptAreas) {
        for (const MapScene::ScriptArea& a : app.scene.scriptAreas) {
            std::string text = "Area " + std::to_string(a.id);
            for (const quests::Quest& q : app.scriptModel.quests)
                for (size_t k = 0; k < q.objectives.size(); ++k) {
                    const quests::Call& c = q.objectives[k].call;
                    if (c.name == "QObjArea" && !c.args.empty() && std::atoi(c.args[0].c_str()) == a.id)
                        text += "\n" + q.name + " #" + std::to_string(k + 1) + (q.objectives[k].title.empty() ? "" : ": " + q.objectives[k].title);
                }
            const float cx = a.round ? a.x : (a.x + a.x2) * 0.5f, cy = a.round ? a.y : (a.y + a.y2) * 0.5f;
            label({cx, cy, 0}, 0.5f, IM_COL32(245, 120, 245, 255), text);
        }
    }
    // A look point's wait, beside its eye (drawn in the scene).
    auto eye = [&](const mob::Vec3& p, float wait) { label(p, 0.0f, IM_COL32(150, 195, 255, 255), ui::Num(wait / 15.0f, 1) + " s"); };
    if (app.scene.logicMode || app.scene.logicAlways) {
        app.scene.ForEachLogicUnit([&](const mob::Object& o, bool selected) {
            std::string head;
            for (const mob::Logic& g : o.logics) {
                if (!g.use) continue;
                if (!head.empty()) head += " + ";
                head += mob::LogicModelName(g.model);
                if (g.wait >= 0) head += ", waits " + ui::Num(g.wait / 15.0f, 1) + " s";
            }
            if (head.empty()) return;
            label(o.position, 2.2f, selected ? IM_COL32(255, 225, 120, 255) : IM_COL32(230, 230, 230, 220), ObjectLabel(o) + ": " + head);
            for (const mob::Logic& g : o.logics) {
                if (!g.use) continue;
                if (g.model == 1) label(g.guardPlace, 0.4f, IM_COL32(245, 150, 60, 255), "radius " + ui::Num(g.guardRadius, 1));
                for (size_t i = 0; i < g.patrol.size(); ++i) {
                    const mob::PatrolPoint& p = g.patrol[i];
                    label(p.position, 1.5f, IM_COL32(235, 225, 90, 255), std::to_string(i + 1));
                    for (size_t k = 0; k < p.looks.size(); ++k) eye(p.looks[k].position, static_cast<float>(p.looks[k].wait));
                }
            }
        });
    }
    // The selected traps' areas and cast points.
    if (!app.mobs.empty() && app.scene.selectedFile == app.activeMob) {
        const mob::File& f = app.mobs[app.activeMob]->file;
        for (int oi : app.scene.selection) {
            if (oi < 0 || oi >= static_cast<int>(f.objects.size()) || f.objects[oi].kind != mob::Kind::MagicTrap) continue;
            const mob::Object& o = f.objects[oi];
            for (size_t i = 0; i < o.trapAreas.size(); ++i)
                label({o.trapAreas[i].x, o.trapAreas[i].y, 0}, 0.3f, IM_COL32(245, 150, 80, 255), "area " + std::to_string(i + 1) + ", radius " + ui::Num(o.trapAreas[i].z, 1));
            for (size_t i = 0; i < o.trapTargets.size(); ++i)
                label({o.trapTargets[i].x, o.trapTargets[i].y, 0}, 0.8f, IM_COL32(235, 130, 235, 255), "cast point " + std::to_string(i + 1));
        }
    }
    for (const ScriptHandle& h : ScriptHandles(app)) { // the script areas' drag squares
        float fx, fy;
        if (!app.scene.Project({h.x, h.y, app.scene.Ground(h.x, h.y) + 0.12f}, fx, fy)) continue;
        const ImVec2 c(min.x + fx * size.x, min.y + fy * size.y);
        const bool active = app.areaDrag.on && app.areaDrag.file == h.file && app.areaDrag.index == h.index && app.areaDrag.resize == h.resize;
        const float hs = h.resize == 0 ? 5.0f : 4.0f;
        draw->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), active ? IM_COL32(255, 230, 120, 255) : IM_COL32(120, 200, 255, 255));
        draw->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(0, 0, 0, 200));
    }
    if (app.openQuest && app.scene.options.exits) {
        for (size_t ei = 0; ei < app.openQuest->shown.exits.size(); ++ei) {
            const quest::Exit& e = app.openQuest->shown.exits[ei];
            // Handles: corners, sides and centre of each area.
            for (int which = 0; which < 2; ++which) {
                const quest::Rect& r = which ? e.remove : e.deploy;
                if (!r.set) continue;
                for (int h = 0; h < 9; ++h) {
                    mob::Vec3 hp = HandlePoint(r, h);
                    float fx, fy;
                    if (!app.scene.Project({hp.x, hp.y, app.scene.Ground(hp.x, hp.y) + 0.12f}, fx, fy)) continue;
                    ImVec2 c(min.x + fx * size.x, min.y + fy * size.y);
                    bool active = app.rectDrag.on && app.rectDrag.exit == static_cast<int>(ei) && app.rectDrag.remove == (which == 1) && app.rectDrag.handle == h;
                    ImU32 col = which ? IM_COL32(250, 120, 100, 255) : IM_COL32(110, 240, 130, 255);
                    float hs = h == 8 ? 5.0f : 4.0f;
                    draw->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), active ? IM_COL32(255, 230, 120, 255) : col);
                    draw->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(0, 0, 0, 200));
                }
            }
            std::string title = e.title.empty() ? "exit " + std::to_string(e.number) : e.title;
            auto centre = [](const quest::Rect& r) { return mob::Vec3{(r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f, 0}; };
            if (e.deploy.set) label(centre(e.deploy), 0.5f, IM_COL32(120, 245, 140, 255), title + ": deploy");
            if (e.remove.set) label(centre(e.remove), 1.2f, IM_COL32(250, 130, 110, 255), title + ": exit");
        }
    }
    if (!app.leftCells.empty() && app.terrainLoaded) { // the cells the ground painter left, tinted (fading out at the end)
        const double leftTime = app.leftCellsUntil - ImGui::GetTime();
        if (leftTime <= 0) app.leftCells.clear();
        else {
            const float alpha = static_cast<float>(std::min(1.0, leftTime / 1.5));
            const float cx[4] = {0, 2, 2, 0}, cy[4] = {0, 0, 2, 2};
            for (const auto& [gx, gy] : app.leftCells) {
                ImVec2 pts[4];
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) {
                    const float x = gx * 2.0f + cx[i], y = gy * 2.0f + cy[i];
                    float fx = 0, fy = 0;
                    ok = app.scene.Project({x, y, app.scene.Ground(x, y) + 0.05f}, fx, fy);
                    pts[i] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
                }
                if (!ok) continue;
                draw->AddConvexPolyFilled(pts, 4, IM_COL32(255, 90, 60, static_cast<int>(110 * alpha)));
                draw->AddPolyline(pts, 4, IM_COL32(255, 120, 80, static_cast<int>(230 * alpha)), ImDrawFlags_Closed, 1.5f);
            }
            if (ImGui::IsMouseHoveringRect(min, ImVec2(min.x + size.x, min.y + size.y)) && app.hoverGround) {
                int si, row, col;
                if (TileAt(app.terrain, app.ground.x, app.ground.y, si, row, col)) {
                    const std::pair<int, int> cell{(si % app.terrain.sectorsX) * 16 + col, (si / app.terrain.sectorsX) * 16 + row};
                    if (std::find(app.leftCells.begin(), app.leftCells.end(), cell) != app.leftCells.end())
                        ImGui::SetTooltip("Left as it was: the textures have no tile for the grounds meeting at its corners.\nPaint it by hand (Tiles tab) or widen the stroke.");
                }
            }
        }
    }
    TerrainBrushOutline(app, draw, min, size);
    if (app.dipSelected >= 0) { // Diplomacy: the selected group's units, a magenta ring each
        for (size_t fi = 0; fi < app.mobs.size(); ++fi) {
            if (!app.scene.FileVisible(static_cast<int>(fi))) continue;
            for (const mob::Object& o : app.mobs[fi]->file.objects) {
                if (o.kind != mob::Kind::Unit || o.player != app.dipSelected) continue;
                float fx, fy;
                if (!app.scene.Project({o.position.x, o.position.y, app.scene.Ground(o.position.x, o.position.y) + 1.0f}, fx, fy)) continue;
                const ImVec2 c(min.x + fx * size.x, min.y + fy * size.y);
                draw->AddCircle(c, 9.0f, IM_COL32(255, 120, 255, 230), 16, 2.5f);
                draw->AddCircle(c, 3.0f, IM_COL32(255, 120, 255, 230), 8, 2.0f);
            }
        }
    }
    if (app.heightBrush && app.terrainLoaded && app.hoverGround) { // the height brush's circle on the ground
        ImVec2 pts[48];
        int n = 0;
        for (int i = 0; i < 48; ++i) {
            const float a2 = i * 6.2831853f / 48, x = app.ground.x + std::cos(a2) * app.heightRadius, y = app.ground.y + std::sin(a2) * app.heightRadius;
            float fx, fy;
            if (app.scene.Project({x, y, app.terrain.HeightAt(x, y) + 0.1f}, fx, fy)) pts[n++] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
        }
        if (n > 2) draw->AddPolyline(pts, n, IM_COL32(255, 210, 90, 220), ImDrawFlags_Closed, 2.0f);
    }
    draw->PopClipRect();
    if (app.switchListShown) {
        ImGui::SetNextWindowPos(ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.35f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::Begin("##switchmob", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextDisabled("Active map (%s: next)", ui::BindName(app.lib.mapKeys[config::kKeySwitchMob]).c_str());
        for (size_t i = 0; i < app.mobs.size(); ++i) {
            bool active = static_cast<int>(i) == app.activeMob;
            if (active) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "> %s", app.mobs[i]->file.fileName.c_str());
            else ImGui::Text("  %s", app.mobs[i]->file.fileName.c_str());
        }
        ImGui::End();
    }
}

// ---- logic points (logic mode) ------------------------------------------------------------------------
// The selected units' patrol points (flags), look points (eyes) and guard places can be clicked to select
// them (Shift: add or remove), taken in a rectangle, dragged, moved with G and deleted.

// Calls fn(ref, position, lift) for each point of the selected units' logic in use in the active map;
// lift is the height above the ground where it is grabbed.
template <typename F> static void EachLogicPoint(App& app, F fn) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) return;
    const mob::File& f = app.mobs[app.activeMob]->file;
    for (int oi : app.scene.selection) {
        if (oi < 0 || oi >= static_cast<int>(f.objects.size())) continue;
        const mob::Object& o = f.objects[oi];
        if (o.kind == mob::Kind::MagicTrap) { // a selected trap's areas and cast points
            for (size_t i = 0; i < o.trapAreas.size(); ++i)
                fn(LogicPointRef{oi, -1, static_cast<int>(i), -1, false, 1}, mob::Vec3{o.trapAreas[i].x, o.trapAreas[i].y, 0}, 'a');
            for (size_t i = 0; i < o.trapTargets.size(); ++i)
                fn(LogicPointRef{oi, -1, static_cast<int>(i), -1, false, 2}, mob::Vec3{o.trapTargets[i].x, o.trapTargets[i].y, 0}, 'c');
            continue;
        }
        if (o.kind != mob::Kind::Unit || !app.scene.logicMode) continue; // a unit's logic: in logic mode
        for (size_t gi = 0; gi < o.logics.size(); ++gi) {
            const mob::Logic& g = o.logics[gi];
            if (!g.use) continue;
            const int l = static_cast<int>(gi);
            if (g.model == 1 || g.model == 3) fn(LogicPointRef{oi, l, -1, -1, true}, g.guardPlace, 'g');
            if (g.model != 2) continue;
            for (size_t i = 0; i < g.patrol.size(); ++i) {
                fn(LogicPointRef{oi, l, static_cast<int>(i), -1, false}, g.patrol[i].position, 'f');
                for (size_t k = 0; k < g.patrol[i].looks.size(); ++k)
                    fn(LogicPointRef{oi, l, static_cast<int>(i), static_cast<int>(k), false}, g.patrol[i].looks[k].position, 'e');
            }
        }
    }
}

// The screen rectangle a logic point's marker covers, as the scene draws it (map_scene Flag / Eye /
// GroundPoint): the flag's base, pole and cloth; the eye and its base; the guard place's cube.
static bool MarkerScreenRect(App& app, const mob::Vec3& p, char kind, ImVec2 min, ImVec2 size, ImVec2& lo, ImVec2& hi) {
    const float z = app.scene.Ground(p.x, p.y);
    const Mat4 view = app.scene.camera.ViewMatrix();
    const fig::Vec3 right{view.m[0], view.m[4], view.m[8]}, up{view.m[1], view.m[5], view.m[9]};
    std::vector<fig::Vec3> pts;
    auto ring = [&](float radius, float h) {
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * i / 8;
            pts.push_back({p.x + std::cos(a) * radius, p.y + std::sin(a) * radius, z + h});
        }
    };
    if (kind == 'f') {
        const float yaw = app.scene.camera.yawDeg * 3.14159265f / 180.0f;
        ring(0.45f, 0.04f);
        pts.push_back({p.x, p.y, z + 1.4f});
        pts.push_back({p.x - std::sin(yaw) * 0.75f, p.y + std::cos(yaw) * 0.75f, z + 1.15f});
    } else if (kind == 'a') { // a trap area's centre handle
        ring(0.5f, 0.04f);
    } else if (kind == 'c') { // a trap's cast point
        ring(0.35f, 0.0f);
        ring(0.35f, 0.6f);
    } else if (kind == 'e') {
        ring(0.3f, 0.04f);
        const fig::Vec3 c{p.x, p.y, z + 0.85f};
        for (float sx : {-0.6f, 0.6f})
            for (float sy : {-0.42f, 0.45f}) pts.push_back(c + right * sx + up * sy);
    } else {
        ring(0.28f, 0.0f);
        ring(0.28f, 0.4f);
    }
    bool any = false;
    for (const fig::Vec3& q : pts) {
        float fx, fy;
        if (!app.scene.Project(q, fx, fy)) continue;
        const ImVec2 s(min.x + fx * size.x, min.y + fy * size.y);
        if (!any) { lo = hi = s; any = true; }
        lo.x = std::min(lo.x, s.x); lo.y = std::min(lo.y, s.y);
        hi.x = std::max(hi.x, s.x); hi.y = std::max(hi.y, s.y);
    }
    return any;
}

// Where a logic point is (the file's current values); false when the reference no longer fits.
bool LogicPointAt(const mob::File& f, const LogicPointRef& r, mob::Vec3& out) {
    if (r.object < 0 || r.object >= static_cast<int>(f.objects.size())) return false;
    if (r.trap) {
        const auto& list = r.trap == 1 ? f.objects[r.object].trapAreas : f.objects[r.object].trapTargets;
        if (r.point < 0 || r.point >= static_cast<int>(list.size())) return false;
        out = {list[static_cast<size_t>(r.point)].x, list[static_cast<size_t>(r.point)].y, 0.0f};
        return true;
    }
    const auto& logics = f.objects[r.object].logics;
    if (r.logic < 0 || r.logic >= static_cast<int>(logics.size())) return false;
    const mob::Logic& g = logics[r.logic];
    if (r.place) { out = g.guardPlace; return true; }
    if (r.point < 0 || r.point >= static_cast<int>(g.patrol.size())) return false;
    const mob::PatrolPoint& p = g.patrol[r.point];
    if (r.look < 0) { out = p.position; return true; }
    if (r.look >= static_cast<int>(p.looks.size())) return false;
    out = p.looks[r.look].position;
    return true;
}

void SetLogicPoint(mob::Logic& g, const LogicPointRef& r, const mob::Vec3& v) {
    if (r.place) g.guardPlace = v;
    else if (r.look >= 0) g.patrol[r.point].looks[r.look].position = v;
    else g.patrol[r.point].position = v;
}

// Drops selected points that no longer exist (after an undo, a delete...).
void PruneLogicPoints(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) { app.scene.logicPoints.clear(); return; }
    const mob::File& f = app.mobs[app.activeMob]->file;
    auto& v = app.scene.logicPoints;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const LogicPointRef& r) { mob::Vec3 p; return !LogicPointAt(f, r, p); }), v.end());
}

// Deletes the selected patrol and look points (guard places stay: every guard has one). One undo step.
void DeleteLogicPoints(App& app) {
    PruneLogicPoints(app);
    if (app.scene.logicPoints.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    std::map<std::pair<int, int>, mob::Logic> changed;
    std::map<int, std::pair<std::vector<mob::Vec3>, std::vector<mob::Vec3>>> traps;
    for (const LogicPointRef& r : app.scene.logicPoints) {
        if (r.trap) traps.emplace(r.object, std::make_pair(f.objects[r.object].trapAreas, f.objects[r.object].trapTargets));
        else if (!r.place) changed.emplace(std::make_pair(r.object, r.logic), f.objects[r.object].logics[r.logic]);
    }
    if (changed.empty() && traps.empty()) return;
    for (auto& [oi, lists] : traps) { // from the back, so the other indices stay right
        std::vector<LogicPointRef> mine;
        for (const LogicPointRef& r : app.scene.logicPoints) if (r.trap && r.object == oi) mine.push_back(r);
        std::sort(mine.begin(), mine.end(), [](const LogicPointRef& a, const LogicPointRef& b) { return a.point > b.point; });
        for (const LogicPointRef& r : mine) {
            auto& list = r.trap == 1 ? lists.first : lists.second;
            list.erase(list.begin() + r.point);
        }
    }
    for (auto& [key, g] : changed) {
        // From the back, so the indices of the rest stay right.
        std::vector<LogicPointRef> mine;
        for (const LogicPointRef& r : app.scene.logicPoints) if (!r.place && r.object == key.first && r.logic == key.second) mine.push_back(r);
        std::sort(mine.begin(), mine.end(), [](const LogicPointRef& a, const LogicPointRef& b) { return a.point != b.point ? a.point > b.point : a.look > b.look; });
        for (const LogicPointRef& r : mine) {
            if (r.look >= 0) {
                // A look point of a patrol point deleted too goes with it.
                if (std::find(mine.begin(), mine.end(), LogicPointRef{r.object, r.logic, r.point, -1, false}) != mine.end()) continue;
                g.patrol[r.point].looks.erase(g.patrol[r.point].looks.begin() + r.look);
            } else {
                g.patrol.erase(g.patrol.begin() + r.point);
            }
        }
    }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    for (auto& [key, g] : changed) mob::SetLogic(f, key.first, key.second, g);
    for (auto& [oi, lists] : traps) {
        mob::ReplaceField(f, oi, mob::kMagicTrapAreas, mob::TrapListPayload(true, lists.first));
        mob::ReplaceField(f, oi, mob::kMagicTrapTargets, mob::TrapListPayload(false, lists.second));
    }
    PushUndo(app, std::move(step), "Delete points");
    app.checksDirty = true;
    app.listedKey.clear();
    app.scene.logicPoints.clear();
}

// The mouse on logic points: a click selects (Shift: adds or removes; G then moves them), Ctrl+click on
// the ground adds a patrol point. True when it took the click.
static bool LogicHandles(App& app, ImVec2 min, ImVec2 size, ImVec2 local) {
    ImGuiIO& io = ImGui::GetIO();
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob) return false;
    if (!(ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) return false;
    mob::File& f = app.mobs[app.activeMob]->file;
    // The marker under the mouse (its drawn outline, a few pixels wider); of several, the smallest on
    // screen, which is the one in front or the more precise one.
    float best = 1e30f;
    LogicPointRef hit;
    bool found = false;
    EachLogicPoint(app, [&](const LogicPointRef& r, const mob::Vec3& p, char kind) {
        ImVec2 lo, hi;
        if (!MarkerScreenRect(app, p, kind, min, size, lo, hi)) return;
        const float pad = 4.0f;
        if (io.MousePos.x < lo.x - pad || io.MousePos.x > hi.x + pad || io.MousePos.y < lo.y - pad || io.MousePos.y > hi.y + pad) return;
        const float area = (hi.x - lo.x + 1) * (hi.y - lo.y + 1);
        if (area >= best) return;
        best = area;
        hit = r;
        found = true;
    });
    if (found) {
        auto& sel = app.scene.logicPoints;
        auto it = std::find(sel.begin(), sel.end(), hit);
        if (io.KeyShift) { if (it != sel.end()) sel.erase(it); else sel.push_back(hit); }
        else sel.assign(1, hit);
        app.logicRecord = hit.logic;
        app.logicRecordFor = hit.object;
        app.swallowLeftRelease = true; // not a click on the unit behind it
        return true;
    }
    fig::Vec3 g;
    const int oi = app.scene.selectedObject;
    // A selected trap: Ctrl+click adds a cast point there, Ctrl+Shift+click an activation area.
    if (io.KeyCtrl && oi >= 0 && oi < static_cast<int>(f.objects.size()) && f.objects[oi].kind == mob::Kind::MagicTrap &&
        app.scene.GroundAt(local.x, local.y, g)) {
        const bool area = io.KeyShift;
        std::vector<mob::Vec3> list = area ? f.objects[oi].trapAreas : f.objects[oi].trapTargets;
        list.push_back({std::round(g.x * 100) / 100, std::round(g.y * 100) / 100, area ? 5.0f : 0.0f});
        EditStep step;
        step.kind = EditStep::Bytes;
        step.file = f.path;
        step.bytes = f.bytes;
        if (mob::ReplaceField(f, oi, area ? mob::kMagicTrapAreas : mob::kMagicTrapTargets, mob::TrapListPayload(area, list))) {
            PushUndo(app, std::move(step), "Add a trap point");
            app.checksDirty = true;
            app.listedKey.clear();
            app.scene.logicPoints.assign(1, LogicPointRef{oi, -1, static_cast<int>(list.size()) - 1, -1, false, area ? 1 : 2});
        } else {
            app.questMessage = "This trap has no such list in the file to add to";
        }
        app.swallowLeftRelease = true;
        return true;
    }
    if (io.KeyCtrl && app.scene.logicMode && oi >= 0 && oi < static_cast<int>(f.objects.size()) && app.scene.GroundAt(local.x, local.y, g)) {
        const mob::Object& o = f.objects[oi];
        for (size_t gi = 0; gi < o.logics.size(); ++gi) {
            if (!o.logics[gi].use || o.logics[gi].model != 2) continue;
            mob::Logic l = o.logics[gi];
            mob::PatrolPoint pt;
            pt.position = {std::round(g.x * 100) / 100, std::round(g.y * 100) / 100, g.z};
            l.patrol.push_back(pt);
            CommitLogic(app, oi, static_cast<int>(gi), l);
            app.scene.logicPoints.assign(1, LogicPointRef{oi, static_cast<int>(gi), static_cast<int>(l.patrol.size()) - 1, -1, false});
            app.swallowLeftRelease = true; // not a click that selects
            return true;
        }
    }
    return false;
}

// The logic points inside a rectangle of the view (fractions), for a rectangle selection in logic mode.
static std::vector<LogicPointRef> LogicPointsInRect(App& app, ImVec2 a, ImVec2 b) {
    std::vector<LogicPointRef> out;
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    EachLogicPoint(app, [&](const LogicPointRef& r, const mob::Vec3& p, char) {
        float fx, fy;
        if (!app.scene.Project({p.x, p.y, app.scene.Ground(p.x, p.y) + 0.05f}, fx, fy)) return;
        if (fx < x0 || fx > x1 || fy < y0 || fy > y1) return;
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    });
    return out;
}

// Left click selects (Shift: adds or removes), left drag draws a selection rectangle (Shift: adds).
// The orbit button (the wheel click by default, like ei_maper) turns the camera, the drag button (right
// by default) moves the ground, the wheel zooms; both buttons are set in Settings.
void ViewportInput(App& app, ImVec2 min, ImVec2 size) {
    ImGui::InvisibleButton("##mapview", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)), ImGuiButtonFlags_MouseButtonLeft);
    ImGuiIO& io = ImGui::GetIO();
    OrbitCamera& cam = app.scene.camera;
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // The other buttons are followed by hand (the side buttons have no ImGui button flag): a drag that
    // starts over the view keeps going until that button is let go.
    for (int b = 1; b < 5; ++b) {
        if (hovered && ImGui::IsMouseClicked(b)) {
            app.mouseHeld[b] = true;
            // Orbiting turns around what the cursor points at (an object, else the ground), not around a
            // point that may be far below or behind it. The view is not re-aimed at it: the whole camera
            // turns around it as the mouse moves.
            if (b == app.lib.mapMouseOrbit) {
                ImVec2 at((io.MousePos.x - min.x) / std::max(size.x, 1.0f), (io.MousePos.y - min.y) / std::max(size.y, 1.0f));
                app.orbitPivotSet = app.scene.PickAny(at.x, at.y, app.orbitPivot) || app.scene.GroundAt(at.x, at.y, app.orbitPivot);
            }
        }
        if (!ImGui::IsMouseDown(b)) app.mouseHeld[b] = false;
    }
    {
        if (app.mouseHeld[app.lib.mapMouseOrbit]) {
            const float yaw0 = cam.yawDeg, pitch0 = cam.pitchDeg;
            cam.Orbit(-io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
            if (app.orbitPivotSet) {
                // The target turns around the pivot with the camera: first the pitch about the view's
                // horizontal right axis, then the yaw about the vertical.
                const float d2r = 3.14159265f / 180.0f;
                const float dp = (cam.pitchDeg - pitch0) * d2r, dy = (cam.yawDeg - yaw0) * d2r;
                float v[3] = {cam.targetX - app.orbitPivot.x, cam.targetY - app.orbitPivot.y, cam.targetZ - app.orbitPivot.z};
                const float ax = -std::sin(yaw0 * d2r), ay = std::cos(yaw0 * d2r); // right axis (horizontal)
                const float th = -dp, c = std::cos(th), s = std::sin(th);
                const float dot = ax * v[0] + ay * v[1];
                const float cx = ay * v[2], cy = -ax * v[2], cz = ax * v[1] - ay * v[0]; // axis x v
                float r[3] = {v[0] * c + cx * s + ax * dot * (1 - c), v[1] * c + cy * s + ay * dot * (1 - c), v[2] * c + cz * s};
                const float cyw = std::cos(dy), syw = std::sin(dy);
                v[0] = r[0] * cyw - r[1] * syw;
                v[1] = r[0] * syw + r[1] * cyw;
                v[2] = r[2];
                cam.targetX = app.orbitPivot.x + v[0];
                cam.targetY = app.orbitPivot.y + v[1];
                cam.targetZ = app.orbitPivot.z + v[2];
            }
        }
        else if (app.mouseHeld[app.lib.mapMousePan]) {
            float k = cam.distance * 0.0016f;
            float yaw = cam.yawDeg * 3.14159265f / 180.0f;
            // The ground follows the mouse: the view's right is (-sin yaw, cos yaw), its forward -(cos yaw, sin yaw),
            // so dragging right moves the target left and dragging down moves it forward.
            float mx = io.MouseDelta.x * k, my = io.MouseDelta.y * k;
            MoveTarget(app, std::sin(yaw) * mx - std::cos(yaw) * my, -std::cos(yaw) * mx - std::sin(yaw) * my, 0.0f);
        }
    }
    if (hovered && io.MouseWheel != 0.0f && io.KeyCtrl && app.tileBrush && !app.materialBrush) { // Ctrl + wheel turns the tile
        app.brushRotation = (app.brushRotation + (io.MouseWheel > 0 ? 1 : 3)) % 4;
    } else if (hovered && io.MouseWheel != 0.0f) {
        cam.distance *= std::pow(0.85f, io.MouseWheel);
        cam.distance = std::max(1.5f, std::min(cam.distance, 3000.0f));
    }
    auto frac = [&](ImVec2 p) { return ImVec2((p.x - min.x) / std::max(size.x, 1.0f), (p.y - min.y) / std::max(size.y, 1.0f)); };
    ImVec2 local = frac(io.MousePos);
    app.viewHovered = hovered;
    app.viewMin = min;
    app.viewSize = size;

    // A move or scale in progress takes the mouse: it follows it, a left click applies, a right click cancels.
    if (app.xf.mode != Transform::None) {
        UpdateTransform(app, io.MousePos, min, size);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            EndTransform(app, true);
            app.swallowLeftRelease = true;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            EndTransform(app, false);
            for (bool& h : app.mouseHeld) h = false; // that right click is not a drag
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (app.swallowLeftRelease) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) app.swallowLeftRelease = false;
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    if (CropDragInput(app, local, min, size, hovered)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (HeightBrushInput(app, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (TerrainBrushInput(app, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    if (LogicHandles(app, min, size, local)) {
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    // Alt + drag on a script area: it follows the mouse; the script is rewritten once, on release.
    if (app.areaDrag.on) {
        fig::Vec3 g;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (app.areaDrag.resize && app.scene.GroundAt(local.x, local.y, g)) { // resizing: the grabbed edge follows
                float* v = app.areaDrag.v;
                const int r = app.areaDrag.resize;
                if (r == 1) v[2] = std::max(0.5f, std::hypot(g.x - v[0], g.y - v[1]));
                if (r & 2) v[0] = g.x;
                if (r & 4) v[1] = g.y;
                if (r & 8) v[2] = g.x;
                if (r & 16) v[3] = g.y;
                if (app.areaDrag.scene >= 0 && app.areaDrag.scene < static_cast<int>(app.scene.scriptAreas.size())) {
                    MapScene::ScriptArea& sa = app.scene.scriptAreas[app.areaDrag.scene];
                    if (r == 1) sa.r = v[2];
                    else { sa.x = v[0]; sa.y = v[1]; sa.x2 = v[2]; sa.y2 = v[3]; }
                }
            } else if (app.scene.GroundAt(local.x, local.y, g)) {
                const float ddx = g.x - app.areaDrag.startX - app.areaDrag.dx, ddy = g.y - app.areaDrag.startY - app.areaDrag.dy;
                app.areaDrag.dx += ddx; app.areaDrag.dy += ddy;
                if (app.areaDrag.scene >= 0 && app.areaDrag.scene < static_cast<int>(app.scene.scriptAreas.size())) {
                    MapScene::ScriptArea& s = app.scene.scriptAreas[app.areaDrag.scene];
                    s.x += ddx; s.y += ddy; s.x2 += ddx; s.y2 += ddy;
                }
            }
        } else {
            app.areaDrag.on = false;
            if (MobEntry* m = FindMob(app, app.areaDrag.file)) {
                std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
                if (app.areaDrag.resize && app.areaDrag.index < calls.size()) {
                    quests::AreaCall a = calls[app.areaDrag.index];
                    const int n = a.round ? 3 : 4;
                    bool changed = false;
                    for (int k = 0; k < n; ++k) {
                        const float v = std::round(app.areaDrag.v[k] * 10.0f) / 10.0f; // tenths, like the panel
                        changed |= v != a.v[k];
                        a.v[k] = v;
                    }
                    if (changed) SetAreaCall(app, *m, a);
                } else if (app.areaDrag.index < calls.size() && (app.areaDrag.dx != 0 || app.areaDrag.dy != 0)) {
                    quests::AreaCall a = calls[app.areaDrag.index];
                    a.v[0] += app.areaDrag.dx; a.v[1] += app.areaDrag.dy;
                    if (!a.round) { a.v[2] += app.areaDrag.dx; a.v[3] += app.areaDrag.dy; }
                    SetAreaCall(app, *m, a);
                }
            }
            app.scriptModelKey.clear(); // the drawn areas come back from the script
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }
    // A click on a script area's square: move it (the centre) or resize it (a corner, a side, the rim).
    if (!io.KeyAlt && !io.KeyCtrl && app.scene.options.scriptAreas && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ScriptHandle* best = nullptr;
        float bestD = 9.0f * 9.0f;
        const std::vector<ScriptHandle> handles = ScriptHandles(app);
        for (const ScriptHandle& h : handles) {
            float fx, fy;
            if (!app.scene.Project({h.x, h.y, app.scene.Ground(h.x, h.y) + 0.12f}, fx, fy)) continue;
            const float dx = min.x + fx * size.x - io.MousePos.x, dy = min.y + fy * size.y - io.MousePos.y;
            if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = &h; }
        }
        fig::Vec3 g;
        if (best && app.scene.GroundAt(local.x, local.y, g)) {
            app.areaDrag = App::AreaDrag{};
            app.areaDrag.on = true;
            app.areaDrag.file = best->file;
            app.areaDrag.index = best->index;
            app.areaDrag.startX = g.x;
            app.areaDrag.startY = g.y;
            app.areaDrag.resize = best->resize;
            const std::vector<quests::AreaCall> calls = quests::AreaCalls(FindMob(app, best->file)->file.script);
            const quests::AreaCall& a = calls[best->index];
            for (int k = 0; k < 4; ++k) app.areaDrag.v[k] = a.v[k];
            for (size_t i = 0; i < app.scene.scriptAreas.size(); ++i) {
                const MapScene::ScriptArea& sa = app.scene.scriptAreas[i];
                if (sa.round == a.round && sa.id == a.id && std::fabs(sa.x - a.v[0]) < 1e-3f && std::fabs(sa.y - a.v[1]) < 1e-3f) { app.areaDrag.scene = static_cast<int>(i); break; }
            }
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    if (io.KeyAlt && app.scene.options.scriptAreas && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        fig::Vec3 g;
        std::string file;
        size_t index = 0;
        if (app.scene.GroundAt(local.x, local.y, g) && AreaAt(app, g.x, g.y, file, index)) {
            app.areaDrag = App::AreaDrag{};
            app.areaDrag.on = true;
            app.areaDrag.file = file;
            app.areaDrag.index = index;
            app.areaDrag.startX = g.x;
            app.areaDrag.startY = g.y;
            // Grabbed near its edge: a resize (the rim of a round area; a rect's side or corner).
            {
                const quests::AreaCall a = quests::AreaCalls(FindMob(app, file)->file.script)[index];
                const float tol = std::max(0.4f, app.scene.camera.distance * 0.012f);
                for (int k = 0; k < 4; ++k) app.areaDrag.v[k] = a.v[k];
                if (a.round) {
                    if (std::fabs(std::hypot(g.x - a.v[0], g.y - a.v[1]) - a.v[2]) < tol) app.areaDrag.resize = 1;
                } else {
                    if (std::fabs(g.x - a.v[0]) < tol) app.areaDrag.resize |= 2;
                    if (std::fabs(g.y - a.v[1]) < tol) app.areaDrag.resize |= 4;
                    if (std::fabs(g.x - a.v[2]) < tol) app.areaDrag.resize |= 8;
                    if (std::fabs(g.y - a.v[3]) < tol) app.areaDrag.resize |= 16;
                }
            }
            // The drawn shape of this call (same id and place).
            for (size_t i = 0; i < app.scene.scriptAreas.size(); ++i) {
                const MapScene::ScriptArea& s = app.scene.scriptAreas[i];
                const std::vector<quests::AreaCall> calls = quests::AreaCalls(FindMob(app, file)->file.script);
                const quests::AreaCall& a = calls[index];
                if (s.round == a.round && s.id == a.id && std::fabs(s.x - a.v[0]) < 1e-3f && std::fabs(s.y - a.v[1]) < 1e-3f) { app.areaDrag.scene = static_cast<int>(i); break; }
            }
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    // Script tab -> Areas -> Place here: the click puts the area's centre there.
    if (app.areaPlace.on) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) app.areaPlace.on = false;
        else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            fig::Vec3 g;
            if (app.scene.GroundAt(local.x, local.y, g)) PlaceArea(app, g.x, g.y);
            app.areaPlace.on = false;
            app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
            return;
        }
    }
    // Left button on an area handle of the open quest: resize or move that area.
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        fig::Vec3 g;
        if (app.scene.GroundAt(local.x, local.y, g)) GrabAreaHandle(app, io.MousePos, min, size, g);
    }
    if (app.rectDrag.on) {
        fig::Vec3 g;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (app.scene.GroundAt(local.x, local.y, g)) DragAreaHandle(app, g, !io.KeyShift);
        } else {
            app.rectDrag.on = false;
        }
        app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
        return;
    }

    // Left button: a rectangle while dragging, a pick on a click.
    const ImVec2 start = io.MouseClickedPos[ImGuiMouseButton_Left];
    const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                          ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);
    if (dragging) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(start, io.MousePos, IM_COL32(255, 220, 120, 40));
        draw->AddRect(start, io.MousePos, IM_COL32(255, 220, 120, 220), 0.0f, 0, 1.5f);
    }
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        const bool additive = io.KeyShift;
        std::vector<LogicPointRef> pts;
        if (drag.x * drag.x + drag.y * drag.y >= 16.0f) pts = LogicPointsInRect(app, frac(start), local);
        if (!pts.empty()) { // logic mode: a rectangle over the selected units' points selects those
            if (!additive) app.scene.logicPoints.clear();
            for (const LogicPointRef& r : pts)
                if (!app.scene.IsLogicPointSelected(r)) app.scene.logicPoints.push_back(r);
        } else if (drag.x * drag.x + drag.y * drag.y >= 16.0f) { // a rectangle
            ImVec2 a = frac(start), b = local;
            std::vector<int> inside = app.scene.ObjectsInRect(a.x, a.y, b.x, b.y);
            if (!additive) app.scene.ClearSelection();
            for (int oi : inside) {
                if (app.scene.IsSelected(app.activeMob, oi)) continue;
                if (app.scene.selectedFile != app.activeMob) app.scene.Select(app.activeMob, oi);
                else { app.scene.selection.push_back(oi); app.scene.selectedObject = oi; }
            }
            if (!inside.empty()) { app.requestTab = SideTab::Objects; app.revealSelection = true; }
        } else { // a click
            int fi = -1, oi = -1;
            if (app.scene.Pick(local.x, local.y, fi, oi)) {
                if (additive) app.scene.Toggle(fi, oi);
                else SelectObject(app, fi, oi, false);
                app.requestTab = SideTab::Objects;
                app.revealSelection = true;
            } else if (!additive) {
                app.scene.ClearSelection();
            }
        }
    }
    app.hoverGround = hovered && app.scene.GroundAt(local.x, local.y, app.ground);
}


} // namespace mapedit
