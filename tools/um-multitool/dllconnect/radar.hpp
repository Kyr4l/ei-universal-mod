// The UM DLL Connector's radar: the game's units (um.dll's UNITS) on a flat top view of the zone, drawn
// with ImGui (no 3D). The map files the game runs (um.dll's MAP) are loaded here too: the terrain for the
// picture, the .mob files for the units' names and the diplomacy table that colors them.
#pragma once

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "../mapedit/mob_file.hpp"
#include "../mapedit/mpr_file.hpp"

namespace dllconnect {

struct RadarUnit {
    unsigned id = 0, side = 0, flags = 0;
    std::string name; // a player's character: its name; empty for the map's units
    float x = 0, y = 0, z = 0, yaw = 0, hp = 0, hpMax = 0, mana = 0, manaMax = 0;
    float sight = 0, viewAngle = 0; // current sight range (world units); viewAngle: senses +0x29C, meaning unknown
    bool Dead() const { return hp <= 0.0f; }
};

// The game camera: where it is and the point it aims at (um.dll's CAMERA).
struct RadarCamera {
    bool valid = false;
    float x = 0, y = 0, targetX = 0, targetY = 0;
};

// What the .mob files say of a unit, by ID.
struct UnitInfo {
    std::string name, kind, file; // kind: the parent template ("Human Gipath Archer 2"), else the figure
    int side = -1;
};

enum class Attitude { Ally, Neutral, Enemy, Unknown };

struct RadarMap {
    std::string terrainName, baseName, questName; // as um.dll reports them
    std::vector<std::string> gamePaths;           // the files' paths as the game opened them
    std::string missing;                          // the files not found
    bool terrainLoaded = false;
    float width = 0, height = 0;                  // world units
    GLuint texture = 0;
    std::unordered_map<unsigned, UnitInfo> units;
    std::vector<int32_t> diplomacy;               // 32 x 32, row = a side's attitude towards the column's
    std::vector<std::string> mobFiles;

    void Drop() {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
        terrainLoaded = false;
        units.clear();
        diplomacy.clear();
        mobFiles.clear();
        width = height = 0;
    }

    // A top view of the terrain: the land shaded by its height and slope, water in blue. Row 0 is the
    // north edge (the highest y).
    void BuildTerrain(const mpr::Map& m) {
        const int w = m.sectorsX * 32, h = m.sectorsY * 32;
        if (w <= 0 || h <= 0) return;
        const float k = m.maxZ / 65535.0f;
        std::vector<float> land(static_cast<size_t>(w) * h, 0.0f), water(static_cast<size_t>(w) * h, -1e9f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const mpr::Sector* s = m.At(x / 32, y / 32);
                if (!s) continue;
                const int r = y % 32, c = x % 32;
                land[static_cast<size_t>(y) * w + x] = s->land[r][c].z * k;
                if (s->water && s->waterMaterial[r / 2][c / 2] >= 0) water[static_cast<size_t>(y) * w + x] = s->waterVerts[r][c].z * k;
            }
        float lo = 1e9f, hi = -1e9f;
        for (float v : land) { lo = std::min(lo, v); hi = std::max(hi, v); }
        const float range = std::max(hi - lo, 1.0f);
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const float v = land[i], t = (v - lo) / range;
                // Light from the north-west: brighter on slopes facing it.
                const float dx = land[static_cast<size_t>(y) * w + std::min(x + 1, w - 1)] - land[static_cast<size_t>(y) * w + std::max(x - 1, 0)];
                const float dy = land[static_cast<size_t>(std::min(y + 1, h - 1)) * w + x] - land[static_cast<size_t>(std::max(y - 1, 0)) * w + x];
                const float shade = std::clamp(1.0f + (-dx + dy) * 0.25f, 0.55f, 1.35f);
                float r = (0.22f + 0.38f * t) * shade, g = (0.36f + 0.22f * t) * shade, b = (0.16f + 0.16f * t) * shade;
                if (water[i] > v + 0.2f) {
                    const float depth = std::clamp((water[i] - v) / 3.0f, 0.0f, 1.0f);
                    r = 0.12f + 0.08f * (1 - depth); g = 0.26f + 0.12f * (1 - depth); b = 0.48f + 0.14f * (1 - depth);
                }
                uint8_t* p = &rgba[(static_cast<size_t>(h - 1 - y) * w + x) * 4];
                p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255);
                p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255);
                p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255);
                p[3] = 255;
            }
        if (texture) glDeleteTextures(1, &texture);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        width = m.Width();
        height = m.Height();
        terrainLoaded = true;
    }

    void AddMob(const mob::File& f) {
        mobFiles.push_back(f.fileName);
        if (f.hasDiplomacy && diplomacy.empty()) diplomacy = f.diplomacy; // quest maps use their base map's
        for (const mob::Object& o : f.objects) {
            if (o.kind != mob::Kind::Unit || !o.hasId) continue;
            UnitInfo& u = units[o.id];
            u.name = o.name;
            u.kind = !o.parentTemplate.empty() ? o.parentTemplate : o.templ;
            u.file = f.fileName;
            u.side = o.player;
        }
    }

    Attitude Towards(unsigned side, unsigned playerSide) const {
        if (side == playerSide) return Attitude::Ally;
        if (diplomacy.size() != 1024 || side >= 32 || playerSide >= 32) return Attitude::Unknown;
        const int32_t v = diplomacy[side * 32 + playerSide];
        return v == 0 ? Attitude::Ally : v == 1 ? Attitude::Neutral : v == 2 ? Attitude::Enemy : Attitude::Unknown;
    }
};

inline ImU32 AttitudeColor(Attitude a, float alpha = 1.0f) {
    const int al = static_cast<int>(alpha * 255);
    switch (a) {
    case Attitude::Ally: return IM_COL32(70, 215, 80, al);
    case Attitude::Neutral: return IM_COL32(235, 205, 55, al);
    case Attitude::Enemy: return IM_COL32(235, 65, 55, al);
    default: return IM_COL32(170, 170, 190, al);
    }
}

inline const char* AttitudeName(Attitude a) {
    return a == Attitude::Ally ? "ally" : a == Attitude::Neutral ? "neutral" : a == Attitude::Enemy ? "enemy" : "unknown";
}

// The view: pan, zoom, what it follows, and whether it turns with the game camera.
struct RadarView {
    float centerX = 0, centerY = 0, pixelsPerUnit = 0; // 0: fit the map on the next draw
    // One choice, so the options cannot contradict each other: the whole map always in view, following my
    // character (or a unit clicked on), following the camera's aim point, or free (pan and zoom by hand).
    enum Follow { WholeMap, FollowPlayer, FollowCamera, FollowNone } follow = WholeMap;
    bool rotate = false;                                // the camera's view direction up
    bool showBars = true, showBodies = true, showNames = false, showCamera = true;
    int sight = 1;                                      // view circles: 0 none, 1 mine, 2 all players' characters
    unsigned followId = 0;                              // a unit clicked on; 0: my character
    std::string me;                                     // the player chosen as "me" (by name); empty: automatic
};

// The players' characters are the units with a name (the map's units have none). "Me" is the one picked
// in the list, else the one nearest the camera's aim point (the camera starts on one's own character),
// else the strongest. Without named units: the strongest unit that is in no loaded .mob.
struct Players {
    std::vector<const RadarUnit*> all;
    const RadarUnit* me = nullptr;
};

inline Players FindPlayers(const std::vector<RadarUnit>& units, const RadarMap& map, const RadarCamera& cam, const std::string& chosen) {
    Players p;
    for (const RadarUnit& u : units)
        if (!u.name.empty()) p.all.push_back(&u);
    for (const RadarUnit* u : p.all)
        if (u->name == chosen) p.me = u;
    if (!p.me && cam.valid) {
        float best = 1e30f;
        for (const RadarUnit* u : p.all) {
            const float d = (u->x - cam.targetX) * (u->x - cam.targetX) + (u->y - cam.targetY) * (u->y - cam.targetY);
            if (d < best) { best = d; p.me = u; }
        }
    }
    for (const RadarUnit* u : p.all)
        if (!p.me || (!cam.valid && u->hpMax > p.me->hpMax)) p.me = u;
    if (p.all.empty()) {
        for (const RadarUnit& u : units)
            if (!map.units.count(u.id) && (!p.me || u.hpMax > p.me->hpMax)) p.me = &u;
        if (p.me) p.all.push_back(p.me);
    }
    return p;
}

// Draws the radar in the space left in the window. `units` are the last ones received.
inline void DrawRadar(RadarView& view, const RadarMap& map, const std::vector<RadarUnit>& units, const RadarCamera& cam, bool fresh) {
    const Players players = FindPlayers(units, map, cam, view.me);
    const unsigned mySide = players.me ? players.me->side : 0;

    ImGui::SetNextItemWidth(170);
    const char* followNames[] = {"View: whole map", "View: follow my character", "View: follow the camera", "View: free"};
    int follow = static_cast<int>(view.follow);
    if (ImGui::Combo("##follow", &follow, followNames, 4)) {
        view.follow = static_cast<RadarView::Follow>(follow);
        view.followId = 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Whole map: always all of it. Follow: centered on my character (or the unit clicked on) or on the\n"
                          "camera's aim point; the wheel zooms. Free: the wheel zooms, a drag pans (dragging in any view goes free).");
    ImGui::SameLine();
    ImGui::BeginDisabled(!cam.valid);
    ImGui::Checkbox("Turn with the camera", &view.rotate);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The camera's view direction points up, as on the screen of the game");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const std::string meLabel = players.me ? (players.me->name.empty() ? std::string("my character") : players.me->name) : std::string("(none)");
    if (ImGui::BeginCombo("##me", ("Me: " + meLabel).c_str())) {
        if (ImGui::Selectable("Automatic (nearest the camera)", view.me.empty())) view.me.clear();
        for (const RadarUnit* u : players.all)
            if (!u->name.empty() && ImGui::Selectable(u->name.c_str(), view.me == u->name)) view.me = u->name;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The player whose point of view colors the units (allies, neutral, enemies)");
    ImGui::SameLine();
    ImGui::Checkbox("Bars", &view.showBars);
    ImGui::SameLine();
    ImGui::Checkbox("Bodies", &view.showBodies);
    ImGui::SameLine();
    ImGui::Checkbox("Names", &view.showNames);
    ImGui::SameLine();
    ImGui::Checkbox("Camera", &view.showCamera);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    const char* sightNames[] = {"No view circles", "My view circle", "Players' view circles"};
    ImGui::Combo("##sight", &view.sight, sightNames, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What players' characters see: their sight range all around (the game lowers it at night)");

    int counts[4] = {0, 0, 0, 0}, bodies = 0;
    for (const RadarUnit& u : units) {
        if (u.Dead()) { ++bodies; continue; }
        ++counts[static_cast<int>(map.Towards(u.side, mySide))];
    }
    ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.35f, 1), "%d allies", counts[0]);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.92f, 0.8f, 0.25f, 1), "%d neutral", counts[1]);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.92f, 0.3f, 0.25f, 1), "%d enemies", counts[2]);
    ImGui::SameLine();
    ImGui::TextDisabled("%d bodies%s, %d player(s)%s", bodies, counts[3] ? (", " + std::to_string(counts[3]) + " unknown").c_str() : "",
                        static_cast<int>(players.all.size()), fresh ? "" : "   (not updating)");
    if (map.diplomacy.size() != 1024) {
        ImGui::SameLine();
        ImGui::TextDisabled("- no diplomacy table (the map files were not found): grey = another side");
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 50.0f), std::max(ImGui::GetContentRegionAvail().y, 50.0f));
    ImGui::InvisibleButton("##radar", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(18, 20, 24, 255));

    float mapW = map.width, mapH = map.height;
    if (mapW <= 0 || mapH <= 0) { // no terrain: the units' extent
        mapW = mapH = 64;
        for (const RadarUnit& u : units) { mapW = std::max(mapW, u.x + 16); mapH = std::max(mapH, u.y + 16); }
    }
    if (view.pixelsPerUnit <= 0) {
        view.pixelsPerUnit = std::min(size.x / mapW, size.y / mapH) * 0.95f;
        view.centerX = mapW * 0.5f;
        view.centerY = mapH * 0.5f;
    }
    // The rotation: the camera's view direction (from it to its aim point) up on the screen.
    float angle = 0.0f;
    if (view.rotate && cam.valid) {
        const float dx = cam.targetX - cam.x, dy = cam.targetY - cam.y;
        if (dx * dx + dy * dy > 1e-6f) angle = -1.5707963f - std::atan2(-dy, dx);
    }
    const float ca = std::cos(angle), sa = std::sin(angle);
    const ImVec2 mid(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    // World -> screen: x right, y up, then turned by `angle`; and back.
    auto toScreen = [&](float x, float y) {
        const float sx = (x - view.centerX) * view.pixelsPerUnit, sy = -(y - view.centerY) * view.pixelsPerUnit;
        return ImVec2(mid.x + sx * ca - sy * sa, mid.y + sx * sa + sy * ca);
    };
    auto toWorld = [&](float px, float py, float& x, float& y) {
        const float sx = px - mid.x, sy = py - mid.y;
        const float ux = sx * ca + sy * sa, uy = -sx * sa + sy * ca;
        x = view.centerX + ux / view.pixelsPerUnit;
        y = view.centerY - uy / view.pixelsPerUnit;
    };
    auto direction = [&](float wx, float wy) { // a world direction on the screen
        const float sx = wx, sy = -wy;
        return ImVec2(sx * ca - sy * sa, sx * sa + sy * ca);
    };
    // Fit: the map's corners, turned, inside the space; centered on the map.
    if (view.follow == RadarView::WholeMap) {
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 4; ++k) {
            const float wx = (k & 1) ? mapW : 0.0f, wy = (k & 2) ? mapH : 0.0f;
            const float sx = wx, sy = -wy;
            const float rx = sx * ca - sy * sa, ry = sx * sa + sy * ca;
            lo[0] = std::min(lo[0], rx); hi[0] = std::max(hi[0], rx);
            lo[1] = std::min(lo[1], ry); hi[1] = std::max(hi[1], ry);
        }
        view.pixelsPerUnit = std::min(size.x / std::max(hi[0] - lo[0], 1.0f), size.y / std::max(hi[1] - lo[1], 1.0f)) * 0.96f;
        view.centerX = mapW * 0.5f;
        view.centerY = mapH * 0.5f;
    }
    // Wheel: zoom about the mouse; drag (any button): pan, which stops following. Both stop fitting.
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0 && view.follow == RadarView::WholeMap) view.follow = RadarView::FollowNone;
    if (hovered && io.MouseWheel != 0) {
        float wx, wy;
        toWorld(io.MousePos.x, io.MousePos.y, wx, wy);
        view.pixelsPerUnit = std::clamp(view.pixelsPerUnit * std::pow(1.2f, io.MouseWheel), 0.2f, 60.0f);
        if (view.follow == RadarView::FollowNone) {
            float nx, ny;
            toWorld(io.MousePos.x, io.MousePos.y, nx, ny);
            view.centerX += wx - nx;
            view.centerY += wy - ny;
        }
    }
    if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        view.follow = RadarView::FollowNone;
        float ax, ay, bx, by;
        toWorld(io.MousePos.x - io.MouseDelta.x, io.MousePos.y - io.MouseDelta.y, ax, ay);
        toWorld(io.MousePos.x, io.MousePos.y, bx, by);
        view.centerX -= bx - ax;
        view.centerY -= by - ay;
    }
    if (view.follow == RadarView::FollowCamera && cam.valid) { view.centerX = cam.targetX; view.centerY = cam.targetY; }
    if (view.follow == RadarView::FollowPlayer) {
        for (const RadarUnit& u : units)
            if ((view.followId && u.id == view.followId) || (!view.followId && players.me && u.id == players.me->id)) { view.centerX = u.x; view.centerY = u.y; }
    }

    if (map.texture) {
        draw->AddImageQuad(static_cast<ImTextureID>(static_cast<intptr_t>(map.texture)), toScreen(0, map.height), toScreen(map.width, map.height),
                           toScreen(map.width, 0), toScreen(0, 0), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
    } else {
        draw->AddQuad(toScreen(0, mapH), toScreen(mapW, mapH), toScreen(mapW, 0), toScreen(0, 0), IM_COL32(90, 90, 100, 255));
    }

    // View circles, under the units: the sight range of players' characters, all around (a third-person
    // camera sees everything). Only theirs: one per unit for hundreds of units overflows ImGui's 16-bit
    // vertex indices (the OpenGL 2 back end draws a window in one go).
    if (view.sight > 0)
        for (const RadarUnit& u : units) {
            const bool mine = players.me && u.id == players.me->id;
            if (u.Dead() || u.sight <= 0 || (view.sight == 1 && !mine) || (view.sight == 2 && !mine && u.name.empty())) continue;
            const float angle = (!u.name.empty() || mine) ? 360.0f : 180.0f;
            const ImVec2 p = toScreen(u.x, u.y);
            const float radius = u.sight * view.pixelsPerUnit;
            if (radius < 2.0f) continue;
            const ImU32 fill = mine ? IM_COL32(255, 255, 255, 40) : (AttitudeColor(map.Towards(u.side, mySide)) & 0x00FFFFFF) | (18u << 24);
            const ImU32 edge = mine ? IM_COL32(255, 255, 255, 120) : (AttitudeColor(map.Towards(u.side, mySide)) & 0x00FFFFFF) | (60u << 24);
            const float half = angle * 0.5f * 3.14159265f / 180.0f;
            const int steps = static_cast<int>(angle / 6.0f);
            std::vector<ImVec2> pts;
            pts.push_back(p);
            for (int i = 0; i <= steps; ++i) {
                const float a = u.yaw - half + 2.0f * half * i / steps; // facing (sin a, -cos a) in the world
                const ImVec2 d = direction(std::sin(a), -std::cos(a));
                pts.push_back(ImVec2(p.x + d.x * radius, p.y + d.y * radius));
            }
            for (size_t i = 1; i + 1 < pts.size(); ++i) draw->AddTriangleFilled(pts[0], pts[i], pts[i + 1], fill);
            if (angle < 360.0f) pts.push_back(p); // a full circle has no sides back to the unit
            draw->AddPolyline(pts.data() + (angle < 360.0f ? 0 : 1), static_cast<int>(pts.size()) - (angle < 360.0f ? 0 : 1), edge, 0, 1.2f);
        }

    const float r = std::clamp(view.pixelsPerUnit * 0.55f, 3.0f, 9.0f);
    const RadarUnit* hover = nullptr;
    float hoverDist = 12.0f * 12.0f;
    for (int pass = 0; pass < 2; ++pass) // bodies under the living
        for (const RadarUnit& u : units) {
            if (u.Dead() != (pass == 0)) continue;
            if (u.Dead() && !view.showBodies) continue;
            const ImVec2 p = toScreen(u.x, u.y);
            if (p.x < origin.x - 60 || p.y < origin.y - 60 || p.x > origin.x + size.x + 60 || p.y > origin.y + size.y + 60) continue;
            const Attitude a = map.Towards(u.side, mySide);
            const ImU32 col = AttitudeColor(a, u.Dead() ? 0.85f : 1.0f);
            const float d2 = (io.MousePos.x - p.x) * (io.MousePos.x - p.x) + (io.MousePos.y - p.y) * (io.MousePos.y - p.y);
            if (hovered && d2 < hoverDist) { hoverDist = d2; hover = &u; }
            if (u.Dead()) {
                const float s = r * 0.8f;
                draw->AddLine(ImVec2(p.x - s, p.y - s), ImVec2(p.x + s, p.y + s), col, 2.5f);
                draw->AddLine(ImVec2(p.x - s, p.y + s), ImVec2(p.x + s, p.y - s), col, 2.5f);
                continue;
            }
            // The facing: an arrowhead (">") pointing where the unit looks: (sin yaw, -cos yaw) in the world.
            const ImVec2 f = direction(std::sin(u.yaw), -std::cos(u.yaw));
            const ImVec2 tip(p.x + f.x * (r + 7), p.y + f.y * (r + 7));
            const ImVec2 back(p.x + f.x * (r + 1), p.y + f.y * (r + 1));
            const ImVec2 side1(back.x - f.y * r * 0.8f, back.y + f.x * r * 0.8f), side2(back.x + f.y * r * 0.8f, back.y - f.x * r * 0.8f);
            draw->AddTriangleFilled(side1, tip, side2, col);
            draw->AddCircleFilled(p, r, col);
            draw->AddCircle(p, r, IM_COL32(0, 0, 0, 200), 0, 1.2f);
            const bool isPlayer = !u.name.empty() || (players.me && u.id == players.me->id);
            if (isPlayer) draw->AddCircle(p, r + 3.5f, players.me && u.id == players.me->id ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 150), 0, 2.0f);
            float labelY = p.y - r - 9.0f;
            if (view.showBars) {
                const float bw = std::max(16.0f, r * 3.0f), bh = 3.0f, top = p.y - r - 17.0f; // above the facing arrow
                auto bar = [&](float y, float frac, ImU32 fill) {
                    frac = std::clamp(frac, 0.0f, 1.0f);
                    draw->AddRectFilled(ImVec2(p.x - bw / 2 - 1, y - 1), ImVec2(p.x + bw / 2 + 1, y + bh + 1), IM_COL32(0, 0, 0, 200));
                    draw->AddRectFilled(ImVec2(p.x - bw / 2, y), ImVec2(p.x - bw / 2 + bw * frac, y + bh), fill);
                };
                bar(top, u.hpMax > 0 ? u.hp / u.hpMax : 0, IM_COL32(220, 50, 45, 255));
                if (u.manaMax > 0) bar(top + bh + 1, u.mana / u.manaMax, IM_COL32(60, 120, 235, 255));
                labelY = top - 2.0f;
            }
            // Players' names always; the map's units' kinds with "Names".
            std::string label;
            if (!u.name.empty()) label = u.name;
            else if (view.showNames) {
                auto it = map.units.find(u.id);
                label = it != map.units.end() ? it->second.kind : std::to_string(u.id);
            }
            if (!label.empty()) {
                const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                const ImVec2 at(p.x - ts.x * 0.5f, labelY - ts.y);
                draw->AddRectFilled(ImVec2(at.x - 2, at.y), ImVec2(at.x + ts.x + 2, at.y + ts.y), IM_COL32(0, 0, 0, 140));
                draw->AddText(at, isPlayer ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 225, 225, 220), label.c_str());
            }
        }
    // The camera: where it is, and a line to the point it aims at.
    if (view.showCamera && cam.valid) {
        const ImVec2 c = toScreen(cam.x, cam.y), t = toScreen(cam.targetX, cam.targetY);
        draw->AddLine(c, t, IM_COL32(120, 200, 255, 200), 1.5f);
        draw->AddCircle(t, 5.0f, IM_COL32(120, 200, 255, 220), 0, 1.5f);
        draw->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(120, 200, 255, 230));
    }
    draw->PopClipRect();

    if (hover) {
        const RadarUnit& u = *hover;
        auto it = map.units.find(u.id);
        ImGui::BeginTooltip();
        if (!u.name.empty()) {
            ImGui::TextUnformatted(u.name.c_str());
            ImGui::TextDisabled(players.me && u.id == players.me->id ? "a player's character (me)" : "a player's character");
        } else if (it != map.units.end()) {
            ImGui::TextUnformatted(it->second.name.c_str());
            ImGui::TextDisabled("%s (%s)", it->second.kind.c_str(), it->second.file.c_str());
        } else {
            ImGui::TextUnformatted("Not in the loaded maps (a summon, a unit a script created...)");
        }
        ImGui::Text("ID %u   side %u (%s)", u.id, u.side, AttitudeName(map.Towards(u.side, mySide)));
        ImGui::Text("HP %.1f / %.1f   mana %.1f / %.1f%s", u.hp, u.hpMax, u.mana, u.manaMax, u.Dead() ? "   dead" : "");
        ImGui::Text("x %.2f  y %.2f  z %.2f   facing %.0f deg", u.x, u.y, u.z, std::fmod(u.yaw * 57.29578f + 720.0f, 360.0f));
        if (u.sight > 0) ImGui::Text("sight %.1f", u.sight);
        ImGui::TextDisabled("click: follow this unit");
        ImGui::EndTooltip();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9.0f) {
            view.followId = players.me && u.id == players.me->id ? 0 : u.id;
            view.follow = RadarView::FollowPlayer;
        }
    }
}

} // namespace dllconnect
