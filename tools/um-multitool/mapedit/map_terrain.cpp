// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Terrain editing: the tile brush, paint by ground, sculpt, water, tile parameters, lighting, the sidebar.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ---- terrain editing (ei_maper's tile brush and tile parameters) --------------------------------------
// A packed tile (mpr_file.hpp): bits 0-5 the tile in its texture (8 x 8), 6-13 the texture, 14-15 the
// rotation. The brush's tile is texture * 64 + tile, the same index the tile types use.

static const char* const kTileTypeNames[] = {"grass", "ground", "stone", "sand", "rock", "field", "water", "road",
                                             "undefined", "snow", "ice", "dry grass", "snowballs", "lava", "swamp", "high rock"};

uint16_t PackTile(int tile, int rotation) {
    return static_cast<uint16_t>(((tile / 64) << 6) | (tile % 64) | ((rotation & 3) << 14));
}

// The texture and the part of it (ImGui UVs: row 0 of the tiles is at the bottom of the picture) of a tile.
static void TileUvs(int tile, ImVec2& uv0, ImVec2& uv1) {
    const int t = tile % 64, cx = t % 8, cy = 7 - t / 8;
    uv0 = ImVec2(cx / 8.0f, cy / 8.0f);
    uv1 = ImVec2((cx + 1) / 8.0f, (cy + 1) / 8.0f);
}

// The land or water tile at a world point: its sector index and place; false outside the map.
bool TileAt(const mpr::Map& m, float x, float y, int& sector, int& row, int& col) {
    if (x < 0 || y < 0 || x >= m.Width() || y >= m.Height()) return false;
    const int sx = static_cast<int>(x / 32.0f), sy = static_cast<int>(y / 32.0f);
    if (!m.At(sx, sy)) return false;
    sector = sy * m.sectorsX + sx;
    col = std::min(static_cast<int>((x - sx * 32.0f) / 2.0f), 15);
    row = std::min(static_cast<int>((y - sy * 32.0f) / 2.0f), 15);
    return true;
}

static void PushTerrainHeaderUndo(App& app) {
    EditStep step;
    step.kind = EditStep::Terrain;
    step.file = app.terrainPath;
    step.header = true;
    step.materials = app.terrain.materials;
    step.tileTypes = app.terrain.tileTypes;
    step.animTiles = app.terrain.animTiles;
    PushUndo(app, std::move(step), "Terrain parameters");
}

// Paints a material around (x, y) with the brush's radius and the panel's fallbacks (tile_paint.hpp), keeping the
// sectors as they were in the stroke's undo step.
static void PaintMaterialAt(App& app, float x, float y) {
    mpr::Map& m = app.terrain;
    tilemat::Terrain* tm = tilemat::Lookup(m.name);
    int si, row, col;
    if (!tm || app.brushMaterialId < 0 || !TileAt(m, x, y, si, row, col)) return;
    const int gx = (si % m.sectorsX) * 16 + col, gy = (si / m.sectorsX) * 16 + row; // the cell under the mouse
    tilemat::PaintResult r;
    tilemat::PaintGround(*tm, m, gx, gy, app.materialRadius, app.brushMaterialId, static_cast<uint32_t>(app.materialSeed), app.materialRepair, app.materialBridge, r,
                         [&](int s2) {
                             bool kept = false;
                             for (const auto& pr : app.stroke.sectors) kept |= pr.first == s2;
                             if (!kept) app.stroke.sectors.push_back({s2, m.sectors[static_cast<size_t>(s2)]});
                         });
    for (int s2 : r.sectors) app.terrainEditedSectors.insert(s2);
    if (r.changed) app.terrainRebuild = true;
    app.materialMessage = r.Message();
    for (const auto& c : r.leftCells) // shown tinted for a while (#109): the mapper sees what to fix by hand
        if (std::find(app.leftCells.begin(), app.leftCells.end(), c) == app.leftCells.end()) app.leftCells.push_back(c);
    if (!r.leftCells.empty()) app.leftCellsUntil = ImGui::GetTime() + 8.0;
}

// Paints the brush's tile on the tile at (x, y), keeping the sector as it was in the stroke's undo step.
static void PaintAt(App& app, float x, float y) {
    mpr::Map& m = app.terrain;
    if (app.materialBrush && !app.brushWater) { PaintMaterialAt(app, x, y); return; }
    int si, row, col;
    if (!TileAt(m, x, y, si, row, col)) return;
    mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
    if (app.brushWater && !s.water) { app.terrainMessage = "This sector has no water layer to paint on"; return; }
    const uint16_t packed = PackTile(app.brushTile, app.brushRotation);
    const bool same = app.brushWater ? s.waterTiles[row][col] == packed && s.waterMaterial[row][col] == app.brushMaterial
                                     : s.landTiles[row][col] == packed;
    if (same) return;
    bool kept = false;
    for (const auto& p : app.stroke.sectors) kept |= p.first == si;
    if (!kept) app.stroke.sectors.push_back({si, s});
    if (app.brushWater) {
        s.waterTiles[row][col] = packed;
        s.waterMaterial[row][col] = static_cast<int16_t>(app.brushMaterial);
    } else {
        s.landTiles[row][col] = packed;
    }
    app.terrainEditedSectors.insert(si);
    app.terrainRebuild = true;
}

// Turns the tile laid at the mouse a quarter (`dir` 1 as R turns the brush, -1 back), the water tile in Water
// paint: Shift+R / Shift+wheel over the view. One undo step per turn.
void TurnTileAtMouse(App& app, int dir) {
    mpr::Map& m = app.terrain;
    int si, row, col;
    if (!app.tileBrush || !app.terrainLoaded || !app.hoverGround || !TileAt(m, app.ground.x, app.ground.y, si, row, col)) return;
    mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
    if (app.brushWater && (!s.water || s.waterMaterial[row][col] < 0)) { app.terrainMessage = "No water tile here to turn"; return; }
    EditStep step;
    step.kind = EditStep::Terrain;
    step.file = app.terrainPath;
    step.sectors.push_back({si, s});
    uint16_t& t = app.brushWater ? s.waterTiles[row][col] : s.landTiles[row][col];
    t = static_cast<uint16_t>((t & 0x3FFF) | ((((t >> 14) + dir) & 3) << 14));
    PushUndo(app, std::move(step), "Turn a terrain tile");
    app.terrainEditedSectors.insert(si);
    app.terrainRebuild = true;
}

// The brush takes a tile (the palette, the tiles at the mouse, Alt+click); a tile not among the quick
// tiles goes in the first empty one, so the keys 1-8 bring back the tiles used without setting them up.
static void PickTile(App& app, int tile) {
    app.brushTile = tile;
    if (std::find(std::begin(app.quickTiles), std::end(app.quickTiles), tile) != std::end(app.quickTiles)) return;
    for (int& q : app.quickTiles) if (q < 0) { q = tile; return; }
}

// The brush in the view: a left drag paints (one undo step per stroke), Alt+click takes the tile under
// the mouse. True when it took the mouse.
bool TerrainBrushInput(App& app, ImVec2 local) {
    if (!app.tileBrush || !app.terrainLoaded) { app.painting = false; return false; }
    ImGuiIO& io = ImGui::GetIO();
    fig::Vec3 g;
    const bool onGround = app.scene.GroundAt(local.x, local.y, g);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) { // the eyedropper
            int si, row, col;
            if (onGround && TileAt(app.terrain, g.x, g.y, si, row, col)) {
                const mpr::Sector& s = app.terrain.sectors[static_cast<size_t>(si)];
                const bool water = app.brushWater && s.water && s.waterMaterial[row][col] >= 0;
                const uint16_t packed = water ? s.waterTiles[row][col] : s.landTiles[row][col];
                PickTile(app, ((packed >> 6) & 255) * 64 + (packed & 63));
                app.brushRotation = (packed >> 14) & 3;
                if (water) app.brushMaterial = s.waterMaterial[row][col];
                if (app.materialBrush && !water) { // the material under the mouse (a plain tile's; a transition's first ground)
                    if (tilemat::Terrain* tm = tilemat::Lookup(app.terrain.name)) {
                        int corners[4];
                        tilemat::Corners(*tm, packed, corners);
                        if (corners[0] >= 0) app.brushMaterialId = corners[0];
                    }
                }
            }
            app.swallowLeftRelease = true;
            return true;
        }
        app.painting = true;
        app.stroke = EditStep{};
        app.stroke.kind = EditStep::Terrain;
        app.stroke.file = app.terrainPath;
        app.leftCells.clear(); // a new stroke: the last one's left cells are shown no more
    }
    if (!app.painting) return false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (onGround) PaintAt(app, g.x, g.y);
    } else {
        app.painting = false;
        if (!app.stroke.sectors.empty()) PushUndo(app, std::move(app.stroke), "Paint terrain tiles");
        app.stroke = EditStep{};
    }
    return true;
}

// The height brush: every land vertex within the radius moves (raise / lower by `heightStrength` units a second,
// smooth toward its neighbours, flatten toward the height under the stroke's start), with a soft falloff. A vertex on
// a sector's edge exists in both sectors: each copy moves the same. Normals are computed again around the change.
static void SculptAt(App& app, float gx, float gy, float dt) {
    mpr::Map& m = app.terrain;
    if (m.maxZ <= 0) return;
    const float R = std::max(app.heightRadius, 0.5f), k = 65535.0f / m.maxZ;
    std::vector<int> touched;
    for (int sy = std::max(0, static_cast<int>((gy - R) / 32)); sy <= std::min(m.sectorsY - 1, static_cast<int>((gy + R) / 32)); ++sy)
        for (int sx = std::max(0, static_cast<int>((gx - R) / 32)); sx <= std::min(m.sectorsX - 1, static_cast<int>((gx + R) / 32)); ++sx) {
            const int si = sy * m.sectorsX + sx;
            mpr::Sector& s = m.sectors[static_cast<size_t>(si)];
            if (!s.present) continue;
            bool changed = false, kept = false;
            for (const auto& pr : app.stroke.sectors) kept |= pr.first == si;
            const mpr::Sector before = s;
            for (int r = 0; r <= 32; ++r)
                for (int c = 0; c <= 32; ++c) {
                    const float vx = sx * 32.0f + c, vy = sy * 32.0f + r, d = std::hypot(vx - gx, vy - gy);
                    if (d > R) continue;
                    const float t = 1 - d / R, w = t * t * (3 - 2 * t);
                    const float z = s.land[r][c].z / k;
                    float target = z;
                    switch (app.heightMode) {
                    case 0: target = z + app.heightStrength * dt * w; break;
                    case 1: target = z - app.heightStrength * dt * w; break;
                    case 2: { // the average around it (from the whole map: across sector edges)
                        const float avg = (m.HeightAt(vx - 1, vy) + m.HeightAt(vx + 1, vy) + m.HeightAt(vx, vy - 1) + m.HeightAt(vx, vy + 1)) / 4;
                        target = z + (avg - z) * std::min(1.0f, app.heightStrength * dt * w);
                        break;
                    }
                    default: target = z + (app.flattenLevel - z) * std::min(1.0f, app.heightStrength * dt * w); break;
                    }
                    const uint16_t nz = static_cast<uint16_t>(std::lround(std::clamp(target, 0.0f, m.maxZ) * k));
                    if (nz != s.land[r][c].z) { s.land[r][c].z = nz; changed = true; }
                }
            if (!changed) continue;
            if (!kept) app.stroke.sectors.push_back({si, before});
            touched.push_back(si);
        }
    for (int si : touched) { // normals from the heights around each vertex
        mpr::ComputeNormals(m, si);
        app.terrainEditedSectors.insert(si);
    }
    if (!touched.empty()) app.terrainRebuild = true;
}

bool HeightBrushInput(App& app, ImVec2 local) {
    if (!app.heightBrush || !app.terrainLoaded) { if (app.painting && !app.tileBrush) app.painting = false; return false; }
    fig::Vec3 g;
    const bool onGround = app.scene.GroundAt(local.x, local.y, g);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app.painting = true;
        app.stroke = EditStep{};
        app.stroke.kind = EditStep::Terrain;
        app.stroke.file = app.terrainPath;
        if (onGround) app.flattenLevel = app.terrain.HeightAt(g.x, g.y);
    }
    if (!app.painting) return false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (onGround) SculptAt(app, g.x, g.y, std::min(ImGui::GetIO().DeltaTime, 0.1f));
    } else {
        app.painting = false;
        static const char* const kNames[] = {"Raise terrain", "Lower terrain", "Smooth terrain", "Flatten terrain"};
        if (!app.stroke.sectors.empty()) PushUndo(app, std::move(app.stroke), kNames[std::clamp(app.heightMode, 0, 3)]);
        app.stroke = EditStep{};
    }
    return true;
}

void HeightBrushKeys(App& app) { // F: radius, Shift+F: strength (while held, moving the mouse sideways)
    static float groundRadius = 1.0f; // the ground brush's radius, in cells, before rounding
    if (ImGui::GetIO().WantTextInput || !ImGui::IsKeyDown(ImGuiKey_F)) { groundRadius = static_cast<float>(app.materialRadius); return; }
    const float dx = ImGui::GetIO().MouseDelta.x;
    if (app.tileBrush && app.materialBrush && !app.brushWater) {
        groundRadius = std::clamp(groundRadius * std::pow(1.02f, dx), 1.0f, 16.0f);
        app.materialRadius = static_cast<int>(std::lround(groundRadius));
        return;
    }
    if (!app.heightBrush) return;
    if (ImGui::GetIO().KeyShift) app.heightStrength = std::clamp(app.heightStrength * std::pow(1.01f, dx), 0.2f, 20.0f);
    else app.heightRadius = std::clamp(app.heightRadius * std::pow(1.01f, dx), 1.0f, 32.0f);
}

// Tile paint's Tiles tab from the next frame on (the tab bar selects it), the brush painting by tile already now.
void ToTilesTab(App& app) {
    app.paintTabWanted = 1;
    app.materialBrush = false;
}

// Keys while the brush is on: 1-8 take a quick tile, comma and period turn the tile, Space shows the tiles at the mouse.
// In the Grounds tab they go to the Tiles tab: a tile key means painting by tile.
void TerrainBrushKeys(App& app) {
    if (!app.tileBrush || ImGui::GetIO().WantTextInput) return;
    bool tileKey = false;
    if (app.viewHovered) {
        // Space is the picker's (TilePicker) while the mouse is over the view: owned, the keyboard navigation does
        // not also press the last clicked button of the sidebar with it.
        ImGui::SetKeyOwner(ImGuiKey_Space, ImGui::GetID("##tilepickerkey"));
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { app.tilePickerOpen = true; tileKey = true; }
    }
    for (int i = 0; i < 8; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false) && app.quickTiles[i] >= 0) { app.brushTile = app.quickTiles[i]; tileKey = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) { app.brushRotation = (app.brushRotation + 3) % 4; tileKey = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) { app.brushRotation = (app.brushRotation + 1) % 4; tileKey = true; }
    if (tileKey && app.materialBrush && !app.brushWater) ToTilesTab(app);
}

// The outline of the tile the brush would paint.
void TerrainBrushOutline(App& app, ImDrawList* draw, ImVec2 min, ImVec2 size) {
    if (!app.tileBrush || !app.terrainLoaded || !app.hoverGround) return;
    int si, row, col;
    if (!TileAt(app.terrain, app.ground.x, app.ground.y, si, row, col)) return;
    const float x0 = (si % app.terrain.sectorsX) * 32.0f + col * 2.0f, y0 = (si / app.terrain.sectorsX) * 32.0f + row * 2.0f;
    if (app.materialBrush && !app.brushWater && app.materialRadius > 1) { // the ground brush's disc of cells
        ImVec2 pts[48];
        int n = 0;
        const float r = (app.materialRadius - 0.5f) * 2.0f;
        for (int i = 0; i < 48; ++i) {
            const float a2 = i * 6.2831853f / 48, x = x0 + 1.0f + std::cos(a2) * r, y = y0 + 1.0f + std::sin(a2) * r;
            float fx, fy;
            if (app.scene.Project({x, y, app.scene.Ground(x, y) + 0.05f}, fx, fy)) pts[n++] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
        }
        if (n > 2) draw->AddPolyline(pts, n, IM_COL32(255, 230, 120, 255), ImDrawFlags_Closed, 2.0f);
        return;
    }
    ImVec2 pts[4];
    const float cx[4] = {0, 2, 2, 0}, cy[4] = {0, 0, 2, 2};
    for (int i = 0; i < 4; ++i) {
        const float x = x0 + cx[i], y = y0 + cy[i];
        float fx, fy;
        if (!app.scene.Project({x, y, app.scene.Ground(x, y) + 0.05f}, fx, fy)) return;
        pts[i] = ImVec2(min.x + fx * size.x, min.y + fy * size.y);
    }
    draw->AddPolyline(pts, 4, app.brushWater ? IM_COL32(120, 200, 255, 255) : IM_COL32(255, 230, 120, 255), ImDrawFlags_Closed, 2.0f);
}

// One tile's picture (rotated as it will be painted when `rotation` is given).
void TileImage(App& app, int tile, float side, int rotation) {
    const GLuint tex = app.scene.TerrainTexture(tile / 64);
    ImVec2 uv0, uv1;
    TileUvs(tile, uv0, uv1);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    if (!tex) { ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + side, p.y + side), IM_COL32(120, 120, 120, 255)); return; }
    // The four corners' UVs turned by quarter turns (the same way the terrain turns them).
    ImVec2 c[4] = {ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y), ImVec2(uv0.x, uv1.y)};
    ImVec2 r[4];
    for (int i = 0; i < 4; ++i) r[i] = c[(i + rotation) % 4];
    ImGui::GetWindowDrawList()->AddImageQuad(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), p, ImVec2(p.x + side, p.y),
                                             ImVec2(p.x + side, p.y + side), ImVec2(p.x, p.y + side), r[0], r[1], r[2], r[3]);
}

// The terrain's tiles no sector uses (land or water), highest first: where a blended tile can go.
static void MarkUsedTiles(const mpr::Map& m, std::vector<bool>& used);
// The tiles no terrain using these textures uses: the open one and every .mpr of the map folders with the same
// name inside (zone3xobr.mpr is zone3obr inside: they share zone3obr000.mmp...), highest first.
static std::vector<int> FreeTiles(App& app) {
    const mpr::Map& m = app.terrain;
    std::vector<bool> used(static_cast<size_t>(std::max(m.textureCount, 0)) * 64, false);
    MarkUsedTiles(m, used);
    static std::map<std::string, std::vector<bool>> cache; // per .mpr path: its used tiles (read once)
    for (const Library::MapFile& f : app.lib.ListMapFiles()) {
        if (!f.terrain || f.path == app.terrainPath) continue;
        auto it = cache.find(f.path);
        if (it == cache.end()) {
            mpr::Map other;
            std::string err;
            std::vector<bool> u;
            if (mpr::Load(f.path, other, err) && other.name == m.name) { u.assign(used.size(), false); MarkUsedTiles(other, u); }
            it = cache.emplace(f.path, u).first;
        }
        for (size_t i = 0; i < it->second.size() && i < used.size(); ++i) if (it->second[i]) used[i] = true;
    }
    std::vector<int> free;
    for (int t = static_cast<int>(used.size()) - 1; t >= 0; --t) if (!used[static_cast<size_t>(t)]) free.push_back(t);
    return free;
}

static void MarkUsedTiles(const mpr::Map& m, std::vector<bool>& used) {
    for (const mpr::Sector& s : m.sectors) {
        if (!s.present) continue;
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 16; ++c) {
            const int lt = ((s.landTiles[r][c] >> 6) & 0xFF) * 64 + (s.landTiles[r][c] & 63);
            if (lt < static_cast<int>(used.size())) used[static_cast<size_t>(lt)] = true;
            if (s.water && s.waterMaterial[r][c] >= 0) {
                const int wt = ((s.waterTiles[r][c] >> 6) & 0xFF) * 64 + (s.waterTiles[r][c] & 63);
                if (wt < static_cast<int>(used.size())) used[static_cast<size_t>(wt)] = true;
            }
        }
    }
    for (const auto& a : m.animTiles) for (int k = 0; k < a.second; ++k) if (a.first + k < static_cast<int>(used.size())) used[static_cast<size_t>(a.first + k)] = true;
}

static const std::string& BlendFolder(App& app);

// Tile painting > Blend two tiles: the brush's tile (A) fading into tile B through a soft, slightly noisy mask,
// written into a free tile of the terrain's textures (in the texture source they come from), then painted with.
static void BlendPanel(App& app) {
    if (!ImGui::CollapsingHeader("Blend two tiles")) return;
    const mpr::Map& m = app.terrain;
    ImGui::TextWrapped("A soft transition from the brush's tile (A) to tile B, made into a new tile of the terrain's textures. "
                       "Paint it between the two grounds, turning it with %s / %s.", ",", ".");
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    TileImage(app, std::max(app.brushTile, 0), 48, 0);
    ImGui::SameLine();
    if (app.blendTileB >= 0) TileImage(app, app.blendTileB, 48, 0); else ImGui::Dummy(ImVec2(48, 48));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 2, p0.y + 2), IM_COL32(255, 255, 255, 230), "A");
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 58, p0.y + 2), IM_COL32(255, 255, 255, 230), "B");
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button("Brush tile -> B")) app.blendTileB = app.brushTile;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick tile B with the brush first, then this, then pick tile A");
    static const char* const kMasks[] = {"Edge (B on the right)", "Corner (B bottom right)", "Inner corner (A top left)"};
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("Shape", &app.blendMask, kMasks, 3);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Softness", &app.blendSoftness, 0.05f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Variation", &app.blendSeed);
    ImGui::EndGroup();
    const std::vector<int> free = FreeTiles(app);
    if (app.blendSlot < 0 || std::find(free.begin(), free.end(), app.blendSlot) == free.end()) app.blendSlot = free.empty() ? -1 : free.front();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Into tile", &app.blendSlot) && std::find(free.begin(), free.end(), app.blendSlot) == free.end()) app.blendSlot = free.empty() ? -1 : free.front();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A tile no sector of this terrain uses (%zu free). Careful: other terrains sharing these textures\n"
                          "(zone6x uses zone6's) may use it.", free.size());
    ImGui::SameLine();
    ImGui::TextDisabled("texture %d, tile %d", app.blendSlot / 64, app.blendSlot % 64);
    char folder[512];
    std::snprintf(folder, sizeof folder, "%s", BlendFolder(app).c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##blendfolder", folder, sizeof folder)) app.blendFolder = folder;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The output folder of the blended textures (loose .mmp files)");
    ImGui::BeginDisabled(app.blendTileB < 0 || app.brushTile < 0 || app.blendSlot < 0 || app.blendFolder.empty());
    if (ImGui::Button("Make the tile")) {
        app.blendMessage.clear();
        auto texName = [&](int tile) { return m.name + "00" + std::to_string(tile / 64); };
        std::vector<uint8_t> ta, tb, tt;
        std::string found;
        if (!app.lib.textures.ReadTexture(texName(app.brushTile), ta) || !app.lib.textures.ReadTexture(texName(app.blendTileB), tb) ||
            !app.lib.textures.ReadTexture(texName(app.blendSlot), tt, &found))
            app.blendMessage = "The terrain's textures were not found in the texture sources";
        const blend::Texture ia = blend::Inspect(ta), ib = blend::Inspect(tb), it = blend::Inspect(tt);
        if (app.blendMessage.empty() && (!ia.ok || !ib.ok || !it.ok || ia.width != ib.width || ia.width != it.width))
            app.blendMessage = "Only DXT1 terrain textures of one size can be blended";
        int layer = app.lib.textures.LayerWith(found);
        if (app.blendMessage.empty() && app.blendFolder.empty()) app.blendMessage = "Choose the output folder first";
        if (app.blendMessage.empty()) {
            const int size = it.width / 8;
            const std::vector<uint8_t> mixed = blend::Mix(blend::ReadTile(ta, ia, app.brushTile % 64), blend::ReadTile(tb, ib, app.blendTileB % 64), size,
                                                          static_cast<blend::Mask>(app.blendMask), app.blendSoftness, static_cast<uint32_t>(app.blendSeed));
            blend::WriteTile(tt, it, app.blendSlot % 64, mixed);
            // Never into an archive (the game's own textures.res!): a loose .mmp in the output folder, which becomes the
            // top texture source so the editor shows it; pack it into the mod's textures archive for the game.
            std::error_code ec;
            std::filesystem::create_directories(app.blendFolder, ec);
            const std::string out = (std::filesystem::path(app.blendFolder) / found).string();
            {
                std::ofstream f(out, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(tt.data()), static_cast<std::streamsize>(tt.size()));
                if (!f) app.blendMessage = "Cannot write " + out;
            }
            bool listed = false;
            for (size_t li = 0; li < app.lib.textures.layers.size(); ++li) if (app.lib.textures.layers[li].path == app.blendFolder) { listed = true; layer = static_cast<int>(li); }
            if (!listed) { app.lib.textures.AddLayer(app.blendFolder); layer = static_cast<int>(app.lib.textures.layers.size()) - 1; }
            if (app.blendMessage.empty()) {
                app.lib.textures.ReloadLayer(static_cast<size_t>(layer));
                app.lib.RebuildTextureIndex();
                PushTerrainHeaderUndo(app);
                if (app.blendSlot < static_cast<int>(app.terrain.tileTypes.size()) && app.brushTile < static_cast<int>(app.terrain.tileTypes.size()))
                    app.terrain.tileTypes[static_cast<size_t>(app.blendSlot)] = app.terrain.tileTypes[static_cast<size_t>(app.brushTile)];
                app.terrainHeaderEdited = true;
                app.terrainDirty = true; // the textures again
                app.blendMessage = "Tile " + std::to_string(app.blendSlot) + " written into " + out + " (a texture source for this session; "
                                   "pack it into the mod's textures archive for the game): the brush paints with it now";
                app.brushTile = app.blendSlot;
                app.brushRotation = 0;
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the changed texture as a loose .mmp into the output folder (never into an archive), shown\n"
                          "here at once; pack it into the mod's textures archive for the game. The tile's type (footsteps)\n"
                          "is A's; save the terrain to keep it.");
    if (!app.blendMessage.empty()) ImGui::TextWrapped("%s", app.blendMessage.c_str());
}

// The Sculpt mode's tools (the Terrain tab and the tool shelf).
void SculptPanel(App& app, bool inShelf) {
    ImGui::SeparatorText("Sculpt");
    if (!inShelf && !app.heightBrush)
        ImGui::TextDisabled("Choose Sculpt in the mode selector above the view to shape the ground (left drag; %s shows these tools in the view).",
                            ui::BindName(app.lib.mapKeys[config::kKeyToolShelf]).c_str());
    {
        static const char* const kModes[] = {"Raise", "Lower", "Smooth", "Flatten"};
        for (int i = 0; i < 4; ++i) { if (i) ImGui::SameLine(); ImGui::RadioButton(kModes[i], &app.heightMode, i); }
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Radius", &app.heightRadius, 1.0f, 32.0f, "%.1f units");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Strength", &app.heightStrength, 0.2f, 20.0f, "%.1f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Raise / lower: units per second at the centre; smooth / flatten: how fast it gets there");
        ImGui::TextDisabled("Height range of this terrain: 0 to %.1f", app.terrain.maxZ);
    }

}

// The output folder of textures the editor writes (blended and borrowed tiles): loose .mmp files beside the
// terrain, never an archive (the game's own textures.res!); pack them into the mod's textures archive for the game.
static const std::string& BlendFolder(App& app) {
    if (app.blendFolder.empty() && !app.terrainPath.empty())
        app.blendFolder = (std::filesystem::path(app.terrainPath).parent_path() / "blended-textures").string();
    return app.blendFolder;
}

static std::string TexName(const std::string& terrain, int index) { // the game's atlas names: <terrain>000 .. 255
    char buf[16];
    std::snprintf(buf, sizeof buf, "%03d", index);
    return terrain + buf;
}

static int GuessTileType(const std::string& material) { // kTileTypeNames: the footsteps / walking kind of a borrowed tile
    const std::string kind = material.substr(0, material.find('/'));
    if (kind == "grass") return 0;
    if (kind == "ground") return 1;
    if (kind == "stone" || kind == "paving" || kind == "slabs") return 2;
    if (kind == "sand") return 3;
    if (kind == "rock" || kind == "rubble") return 4;
    if (kind == "water") return 6;
    if (kind == "snow") return 9;
    if (kind == "ice") return 10;
    if (kind == "lava") return 13;
    if (kind == "swamp") return 14;
    return 8;
}

// A thumbnail of a tile of ANOTHER terrain's textures (decoded once, kept for the session; 0 when not readable).
static GLuint BorrowThumb(App& app, const std::string& terrain, int tile) {
    const std::string key = terrain + "/" + std::to_string(tile);
    auto it = app.borrowThumbs.find(key);
    if (it != app.borrowThumbs.end()) return it->second;
    GLuint id = 0;
    std::vector<uint8_t> bytes;
    mmp::Image image;
    std::string err;
    if (app.lib.textures.ReadTexture(TexName(terrain, tile / 64), bytes) && mmp::Decode(bytes, image, err) && image.width >= 8 && image.width == image.height) {
        const int cell = static_cast<int>(image.width) / 8, t = tile % 64, cx = t % 8, cy = 7 - t / 8;
        std::vector<uint8_t> rgba(static_cast<size_t>(cell) * cell * 4);
        for (int y = 0; y < cell; ++y)
            std::memcpy(&rgba[static_cast<size_t>(y) * cell * 4], &image.rgba[(static_cast<size_t>(cy * cell + y) * image.width + static_cast<size_t>(cx * cell)) * 4], static_cast<size_t>(cell) * 4);
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cell, cell, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    app.borrowThumbs[key] = id;
    return id;
}

// Another allod's ground brought into this terrain: its plain tiles, and its transitions to the grounds this
// terrain already has, copied into free tiles of this terrain's textures (loose .mmp files in the output folder,
// as the Blend tool writes them) and listed in <folder>/<terrain>-materials.tsv so the next session knows them.
static void BorrowMaterial(App& app, const std::string& srcTerrain, int srcMaterial) {
    mpr::Map& m = app.terrain;
    tilemat::Terrain* src = tilemat::Lookup(srcTerrain);
    tilemat::Terrain* dst = tilemat::Lookup(m.name);
    app.borrowMessage.clear();
    if (!src || !dst || srcMaterial < 0 || srcMaterial >= static_cast<int>(src->materials.size())) { app.borrowMessage = "No material data for these textures"; return; }
    const std::string name = src->materials[static_cast<size_t>(srcMaterial)].name;
    const int have = dst->FindMaterial(name);
    if (have >= 0 && !dst->materials[static_cast<size_t>(have)].baseTiles.empty()) { app.brushMaterialId = have; return; }
    struct Copy { int srcTile; std::string a, b; int pattern; };
    std::vector<Copy> copies;
    const std::vector<int>& bases = src->materials[static_cast<size_t>(srcMaterial)].baseTiles;
    for (size_t i = 0; i < bases.size() && i < 4; ++i) copies.push_back({bases[i], name, "", 0}); // up to 4 plain variants
    const size_t baseCount = copies.size();
    for (int t : src->blendTiles) { // its transitions to grounds this terrain has
        const tilemat::Terrain::Info& info = src->tiles[static_cast<size_t>(t)];
        const int other = info.a == srcMaterial ? info.b : info.b == srcMaterial ? info.a : -1;
        if (other < 0) continue;
        const std::string otherName = src->materials[static_cast<size_t>(other)].name;
        const int od = dst->FindMaterial(otherName);
        if (od < 0 || dst->materials[static_cast<size_t>(od)].baseTiles.empty()) continue;
        copies.push_back({t, src->materials[static_cast<size_t>(info.a)].name, src->materials[static_cast<size_t>(info.b)].name, info.pattern});
    }
    const std::vector<int> free = FreeTiles(app);
    if (baseCount > free.size()) { app.borrowMessage = "Needs " + std::to_string(baseCount) + " free tiles for the plain tiles, this terrain has " + std::to_string(free.size()) + ": add a texture first"; return; }
    std::string trimmed;
    if (copies.size() > free.size()) { // the transitions that fit (the plain tiles always come)
        trimmed = " (" + std::to_string(copies.size() - free.size()) + " transition tile(s) left out: only " + std::to_string(free.size()) + " tiles were free)";
        copies.resize(free.size());
    }
    const std::string& folder = BlendFolder(app);
    if (folder.empty()) { app.borrowMessage = "No output folder"; return; }
    std::map<int, std::vector<uint8_t>> srcTex, dstTex; // by texture index
    std::map<int, std::string> dstFound;
    auto texOf = [&](const std::string& terrain, int tile, std::map<int, std::vector<uint8_t>>& into, std::string* found) -> std::vector<uint8_t>* {
        const int i = tile / 64;
        auto it = into.find(i);
        if (it != into.end()) return &it->second;
        std::vector<uint8_t> bytes;
        if (!app.lib.textures.ReadTexture(TexName(terrain, i), bytes, found)) return nullptr;
        return &(into[i] = std::move(bytes));
    };
    std::vector<std::pair<int, Copy>> placed; // free slot, what
    for (size_t i = 0; i < copies.size(); ++i) {
        const int slot = free[i];
        std::string found;
        std::vector<uint8_t>* ts = texOf(srcTerrain, copies[i].srcTile, srcTex, nullptr);
        std::vector<uint8_t>* td = texOf(m.name, slot, dstTex, &found);
        if (!ts || !td) { app.borrowMessage = "The textures of " + (ts ? m.name : srcTerrain) + " were not found in the texture sources"; return; }
        if (!found.empty()) dstFound[slot / 64] = found;
        const blend::Texture is = blend::Inspect(*ts), id = blend::Inspect(*td);
        if (!is.ok || !id.ok || is.width != id.width) { app.borrowMessage = "Only DXT1 terrain textures of one size can be copied (" + srcTerrain + " and " + m.name + " differ)"; return; }
        blend::WriteTile(*td, id, slot % 64, blend::ReadTile(*ts, is, copies[i].srcTile % 64));
        placed.push_back({slot, copies[i]});
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    for (auto& kv : dstTex) {
        auto fn = dstFound.find(kv.first);
        const std::string out = (std::filesystem::path(folder) / (fn != dstFound.end() ? fn->second : TexName(m.name, kv.first) + ".mmp")).string();
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(kv.second.data()), static_cast<std::streamsize>(kv.second.size()));
        if (!f) { app.borrowMessage = "Cannot write " + out; return; }
    }
    int layer = -1;
    for (size_t li = 0; li < app.lib.textures.layers.size(); ++li) if (app.lib.textures.layers[li].path == folder) layer = static_cast<int>(li);
    if (layer < 0) { app.lib.textures.AddLayer(folder); layer = static_cast<int>(app.lib.textures.layers.size()) - 1; }
    app.lib.textures.ReloadLayer(static_cast<size_t>(layer));
    app.lib.RebuildTextureIndex();
    PushTerrainHeaderUndo(app);
    const std::string sidecar = (std::filesystem::path(folder) / (m.name + "-materials.tsv")).string();
    for (const auto& pc : placed) {
        if (pc.second.b.empty()) dst->AddBase(pc.first, pc.second.a); else dst->AddBlend(pc.first, pc.second.a, pc.second.b, pc.second.pattern);
        tilemat::AppendSidecar(sidecar, pc.first, pc.second.a, pc.second.b, pc.second.pattern);
        if (pc.first < static_cast<int>(m.tileTypes.size())) m.tileTypes[static_cast<size_t>(pc.first)] = GuessTileType(pc.second.b.empty() ? pc.second.a : pc.second.b);
    }
    app.terrainHeaderEdited = true;
    app.terrainDirty = true; // the textures again
    app.brushMaterialId = dst->FindMaterial(name);
    app.borrowMessage = name + " from " + srcTerrain + ": " + std::to_string(placed.size()) + " tile(s) copied into free tiles" + trimmed + ", textures written to " + folder +
                        " (pack them into the mod's textures archive for the game); save the terrain to keep the tile types";
    umlog::Write(umlog::Level::Info, "Map Editor: " + app.borrowMessage);
}

// A new texture (atlas) for the terrain: 64 free tiles, flat grey, the size and format of texture 0, written as a
// loose .mmp into the output folder; the terrain's header counts it (save the terrain). The game's limit on
// the number of atlases is NOT known: the vanilla terrains have 8 at most; a terrain with more must be tried in
// the game before being relied on.
static void AddTerrainTexture(App& app) {
    mpr::Map& m = app.terrain;
    app.borrowMessage.clear();
    std::vector<uint8_t> t0;
    std::string found;
    if (!app.lib.textures.ReadTexture(TexName(m.name, 0), t0, &found)) { app.borrowMessage = "The terrain's textures were not found in the texture sources"; return; }
    const blend::Texture info = blend::Inspect(t0);
    if (!info.ok) { app.borrowMessage = "Only DXT1 terrain textures can be copied"; return; }
    const std::string& folder = BlendFolder(app);
    if (folder.empty()) { app.borrowMessage = "No output folder"; return; }
    const int size = info.width / 8;
    const std::vector<uint8_t> flat(static_cast<size_t>(size) * size * 4, 110);
    for (int t = 0; t < 64; ++t) blend::WriteTile(t0, info, t, flat);
    const int index = m.textureCount;
    const std::string ext = found.size() > 4 ? found.substr(found.size() - 4) : std::string(".mmp");
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string out = (std::filesystem::path(folder) / (TexName(m.name, index) + ext)).string();
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(t0.data()), static_cast<std::streamsize>(t0.size()));
        if (!f) { app.borrowMessage = "Cannot write " + out; return; }
    }
    int layer = -1;
    for (size_t li = 0; li < app.lib.textures.layers.size(); ++li) if (app.lib.textures.layers[li].path == folder) layer = static_cast<int>(li);
    if (layer < 0) { app.lib.textures.AddLayer(folder); layer = static_cast<int>(app.lib.textures.layers.size()) - 1; }
    app.lib.textures.ReloadLayer(static_cast<size_t>(layer));
    app.lib.RebuildTextureIndex();
    PushTerrainHeaderUndo(app);
    m.textureCount = index + 1;
    m.tileTypes.resize(static_cast<size_t>(m.textureCount) * 64, 8);
    app.terrainHeaderEdited = true;
    app.terrainDirty = true;
    app.borrowMessage = "Texture " + std::to_string(index) + " added (64 free tiles): " + out + ". Save the terrain to keep it. NOT verified: whether the game "
                        "accepts more than 8 textures per terrain (no vanilla terrain has more): try the saved terrain in the game first.";
    umlog::Write(umlog::Level::Info, "Map Editor: " + app.borrowMessage);
}

static bool MatchesSearch(const std::string& name, const char* search) {
    if (!*search) return true;
    std::string a = Lower(name), b = Lower(search);
    return a.find(b) != std::string::npos;
}

// The Grounds tab: paint by material (the transitions chosen by themselves), this terrain's grounds and every
// other allod's, the brush's size, the whole-terrain blend.
static void GroundsPanel(App& app, tilemat::Terrain& tm) {
    mpr::Map& m = app.terrain;
    if (!app.sidecarLoaded.count(app.terrainPath)) { // borrowed tiles of earlier sessions
        app.sidecarLoaded.insert(app.terrainPath);
        const std::string& folder = BlendFolder(app);
        if (!folder.empty()) tilemat::LoadSidecar(tm, (std::filesystem::path(folder) / (m.name + "-materials.tsv")).string());
    }
    ImGui::SetNextItemWidth(150);
    ImGui::SliderInt("Brush", &app.materialRadius, 1, 16, "%d cells");
    ImGui::SetItemTooltip("The cells painted around the mouse; F + move the mouse sideways changes it in the view");
    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("Variation", &app.materialSeed);
    ImGui::SetItemTooltip("Another value: other plain tiles and transitions picked where several fit");
    ImGui::Checkbox("Fix corners the textures lack", &app.materialRepair);
    ImGui::SetItemTooltip("A cell by the brush the textures have no tile for (three grounds meeting, a concave notch...; the artists'\n"
                          "own junction tiles are used first): one of its corners that is not painted takes whichever ground gives the\n"
                          "four cells around it a tile each, the way the artists filled such spots. Off: the cell is left as it is.");
    ImGui::Checkbox("Bridge with a middle ground", &app.materialBridge);
    ImGui::SetItemTooltip("Where the brush meets a ground the textures cannot blend it with (no transition at all, or not the shape a\n"
                          "cell needs), a band one vertex wide of a ground that blends with both is laid all round the brush, like\n"
                          "zone 7's light stone rims around grass on watered rock. Off: the cell is left as it is.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##groundsearch", "search a ground (grass, rock, sand...)", app.materialSearch, sizeof app.materialSearch);
    const float cell = 44.0f;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 6)));
    ImGui::TextDisabled("This terrain's grounds");
    int shown = 0;
    for (size_t i = 0; i < tm.materials.size(); ++i) {
        const tilemat::Material& mat = tm.materials[i];
        if (mat.baseTiles.empty() || !MatchesSearch(mat.name, app.materialSearch)) continue; // only blended, never plain: cannot be painted
        if (shown++ % perRow) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        TileImage(app, mat.baseTiles[0], cell);
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##mat", ImVec2(cell, cell))) app.brushMaterialId = static_cast<int>(i);
        if (ImGui::IsItemHovered()) {
            const bool blends = app.brushMaterialId >= 0 && tilemat::CanBlend(tm, app.brushMaterialId, static_cast<int>(i));
            ImGui::SetTooltip("%s (%zu plain tiles)%s", mat.name.c_str(), mat.baseTiles.size(), blends || app.brushMaterialId < 0 ? "" : "\nno transition to the chosen ground in these textures");
        }
        const bool chosen = static_cast<int>(i) == app.brushMaterialId;
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + cell, p.y + cell), chosen ? IM_COL32(255, 220, 120, 255) : IM_COL32(70, 70, 70, 255), 0, 0, chosen ? 3.0f : 1.0f);
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled("(no ground matches)");
    if (app.brushMaterialId >= 0 && app.brushMaterialId < static_cast<int>(tm.materials.size())) ImGui::Text("Ground: %s", tm.materials[static_cast<size_t>(app.brushMaterialId)].name.c_str());
    else ImGui::TextDisabled("Pick a ground, then paint in the view");
        if (ImGui::Button("Blend the hard edges of the whole terrain")) {
            EditStep step;
            step.kind = EditStep::Terrain;
            step.file = app.terrainPath;
            mpr::Map& map = app.terrain;
            const tilemat::AutoBlendResult r = tilemat::BlendHardEdges(tm, map, static_cast<uint32_t>(app.materialSeed), app.autoBlendSameKind,
                                                                       [&](int si) { step.sectors.push_back({si, map.sectors[static_cast<size_t>(si)]}); });
            if (!step.sectors.empty()) {
                PushUndo(app, std::move(step), "Blend hard edges");
                for (int si : r.sectors) app.terrainEditedSectors.insert(si);
                app.terrainRebuild = true;
            }
            char msg[256];
            std::snprintf(msg, sizeof msg, "Hard edges: %d vertices where two plain grounds meet, %d cells by them: %d changed in %d round(s), %d left (no transition for those grounds, or an unknown tile)",
                          r.hardVertices, r.cells, r.changed, r.rounds, r.left);
            app.materialMessage = msg;
            umlog::Write(umlog::Level::Info, std::string("Map Editor: ") + msg);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Everywhere plain tiles of two grounds touch although the textures hold a transition for them (hand-painted\n"
                              "maps), the cells around get the transition tiles. Cells away from such an edge are not touched; a terrain\n"
                              "whose transitions are right comes out unchanged. The rarer ground keeps its cells, the band is laid into\n"
                              "the commoner one. One undo step; rebuild the navmesh after if tile types changed.");
        ImGui::Checkbox("Hard edges between kinds of one ground too", &app.autoBlendSameKind);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Also where two kinds of one ground touch (grass/dark beside grass/common). The vanilla maps lay those\n"
                              "side by side on purpose, so this re-tiles a lot of a vanilla terrain: off by default.");
    if (!app.materialMessage.empty()) ImGui::TextWrapped("%s", app.materialMessage.c_str());
    // Other allods' grounds: copied into free tiles of this terrain's textures on a click.
    if (ImGui::CollapsingHeader("Other allods' grounds")) {
        ImGui::TextWrapped("A click copies the ground's tiles (and its transitions to the grounds above) into free tiles of this terrain's "
                           "textures, written as loose .mmp files into the output folder; %zu tiles are free.", FreeTiles(app).size());
        char folder[512];
        std::snprintf(folder, sizeof folder, "%s", BlendFolder(app).c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##borrowfolder", folder, sizeof folder)) app.blendFolder = folder;
        ImGui::SetItemTooltip("The output folder of the written textures (never an archive); pack it into the mod's textures archive for the game");
        if (ImGui::Button("Add a texture (64 free tiles)")) AddTerrainTexture(app);
        ImGui::SetItemTooltip("A new atlas for this terrain, flat grey, written into the output folder; the header counts it (save the terrain).\n"
                              "The vanilla terrains have 8 textures at most and most are full: whether the game accepts a 9th is NOT\n"
                              "verified. Try the saved terrain in the game before painting much with borrowed grounds.");
        if (!app.borrowMessage.empty()) ImGui::TextWrapped("%s", app.borrowMessage.c_str());
        for (const std::string& terrain : tilemat::KnownTerrains()) {
            if (terrain == tm.name) continue;
            tilemat::Terrain* other = tilemat::Lookup(terrain);
            if (!other) continue;
            std::vector<int> offer;
            for (size_t i = 0; i < other->materials.size(); ++i) {
                const tilemat::Material& mat = other->materials[i];
                if (mat.baseTiles.empty() || !MatchesSearch(mat.name, app.materialSearch)) continue;
                const int have = tm.FindMaterial(mat.name);
                if (have >= 0 && !tm.materials[static_cast<size_t>(have)].baseTiles.empty()) continue; // this terrain has it
                offer.push_back(static_cast<int>(i));
            }
            if (offer.empty()) continue;
            if (!ImGui::TreeNode(terrain.c_str(), "%s (%zu)", terrain.c_str(), offer.size())) continue;
            int n = 0;
            for (int i : offer) {
                const tilemat::Material& mat = other->materials[static_cast<size_t>(i)];
                if (n++ % perRow) ImGui::SameLine();
                ImGui::PushID(i);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const GLuint thumb = BorrowThumb(app, terrain, mat.baseTiles[0]);
                if (thumb) ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(thumb)), ImVec2(cell, cell));
                else { ImGui::Dummy(ImVec2(cell, cell)); ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(120, 120, 120, 255)); }
                ImGui::SetCursorScreenPos(p);
                if (ImGui::InvisibleButton("##borrow", ImVec2(cell, cell))) BorrowMaterial(app, terrain, i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s from %s (%zu plain tiles)%s", mat.name.c_str(), terrain.c_str(), mat.baseTiles.size(), thumb ? "" : "\n(textures not found in the texture sources)");
                ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(70, 70, 70, 255));
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
}

// The Tiles tab: one tile at a time (the expert's way), the quick tiles, the blend tool, the palette.
static void TilesPanel(App& app) {
    mpr::Map& m = app.terrain;
    TileImage(app, app.brushTile, 48, app.brushRotation);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("tile %d of texture %d", app.brushTile % 64, app.brushTile / 64);
    for (int r = 0; r < 4; ++r) {
        if (r) ImGui::SameLine();
        ImGui::RadioButton((std::to_string(r * 90) + "##rot").c_str(), &app.brushRotation, r);
    }
    if (app.brushTile < static_cast<int>(m.tileTypes.size())) {
        int type = m.tileTypes[static_cast<size_t>(app.brushTile)];
        ImGui::SetNextItemWidth(140);
        const char* preview = type >= 0 && type < 16 ? kTileTypeNames[type] : "?";
        if (ImGui::BeginCombo("type##tiletype", preview)) {
            for (int t = 0; t < 16; ++t)
                if (ImGui::Selectable(kTileTypeNames[t], t == type)) {
                    PushTerrainHeaderUndo(app);
                    m.tileTypes[static_cast<size_t>(app.brushTile)] = t;
                    app.terrainHeaderEdited = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the game makes of this tile (footsteps, walking): saved in the terrain's header");
    }
    ImGui::EndGroup();
    // Quick tiles: click takes one, right-click keeps the brush's tile in it.
    ImGui::TextDisabled("Quick tiles (keys 1-8; the tiles picked fill them; right-click keeps the brush's tile):");
    for (int i = 0; i < 8; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (app.quickTiles[i] >= 0) TileImage(app, app.quickTiles[i], 32);
        else ImGui::Dummy(ImVec2(32, 32));
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + 32, p.y + 32), app.quickTiles[i] == app.brushTile && app.quickTiles[i] >= 0 ? IM_COL32(255, 220, 120, 255) : IM_COL32(90, 90, 90, 255));
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("##q", ImVec2(32, 32), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && app.quickTiles[i] >= 0) app.brushTile = app.quickTiles[i];
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.quickTiles[i] = app.brushTile;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d", i + 1);
        ImGui::PopID();
    }
    BlendPanel(app);
    // The terrain's textures, 8 x 8 tiles each: click a tile to paint with it.
    for (int t = 0; t < m.textureCount; ++t) {
        const GLuint tex = app.scene.TerrainTexture(t);
        if (!ImGui::TreeNodeEx(("Texture " + std::to_string(t) + "##tex" + std::to_string(t)).c_str(), t == app.brushTile / 64 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            continue;
        if (!tex) { ImGui::TextDisabled("(not found in the texture sources)"); ImGui::TreePop(); continue; }
        const float side = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(side, side));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cell = side / 8.0f;
        for (int i = 1; i < 8; ++i) {
            dl->AddLine(ImVec2(p.x + i * cell, p.y), ImVec2(p.x + i * cell, p.y + side), IM_COL32(0, 0, 0, 90));
            dl->AddLine(ImVec2(p.x, p.y + i * cell), ImVec2(p.x + side, p.y + i * cell), IM_COL32(0, 0, 0, 90));
        }
        if (app.brushTile / 64 == t) {
            const int bt = app.brushTile % 64, cx = bt % 8, cy = 7 - bt / 8;
            dl->AddRect(ImVec2(p.x + cx * cell, p.y + cy * cell), ImVec2(p.x + (cx + 1) * cell, p.y + (cy + 1) * cell), IM_COL32(255, 220, 120, 255), 0, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const int cx = std::min(7, static_cast<int>((mp.x - p.x) / cell)), cy = std::min(7, static_cast<int>((mp.y - p.y) / cell));
            const int tile = t * 64 + (7 - cy) * 8 + cx;
            const int type = tile < static_cast<int>(m.tileTypes.size()) ? m.tileTypes[static_cast<size_t>(tile)] : -1;
            ImGui::SetTooltip("tile %d (%s)", tile % 64, type >= 0 && type < 16 ? kTileTypeNames[type] : "?");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) PickTile(app, tile);
        }
        ImGui::TreePop();
    }

}

// Space over the view in Tile paint: every tile of the terrain's textures around the mouse; a click takes
// one and closes it (so does Space again, Esc, or a click beside it). The mouse stays where it paints.
void TilePicker(App& app) {
    const bool open = app.tilePickerOpen;
    app.tilePickerOpen = false;
    if (!app.tileBrush || !app.terrainLoaded) return;
    const mpr::Map& m = app.terrain;
    const int count = m.textureCount;
    if (count <= 0) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cell = 22.0f, side = cell * 8, gap = style.ItemSpacing.x;
    const int cols = std::min(count, 4), rows = (count + cols - 1) / cols;
    if (open) {
        const ImVec2 size(cols * side + (cols - 1) * gap + style.WindowPadding.x * 2,
                          rows * (side + ImGui::GetFrameHeight()) + (rows - 1) * gap + style.WindowPadding.y * 2 + ImGui::GetFrameHeight());
        const ImVec2 mouse = ImGui::GetIO().MousePos, display = ImGui::GetIO().DisplaySize;
        ImVec2 at(mouse.x - size.x / 2, mouse.y - size.y / 2); // centred on the mouse, inside the program's window
        at.x = std::clamp(at.x, 0.0f, std::max(0.0f, display.x - size.x));
        at.y = std::clamp(at.y, 0.0f, std::max(0.0f, display.y - size.y));
        app.tilePickerPos = at;
        ImGui::OpenPopup("##tilepicker");
    }
    ImGui::SetNextWindowPos(app.tilePickerPos); // every frame: else a popup sized on its first frame is placed again on its second
    if (!ImGui::BeginPopup("##tilepicker", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextDisabled("Click a tile (turn: R , . Ctrl+wheel in the view)");
    bool picked = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int t = 0; t < count; ++t) {
        if (t % cols) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("Texture %d", t);
        const GLuint tex = app.scene.TerrainTexture(t);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (!tex) {
            ImGui::Dummy(ImVec2(side, side));
            dl->AddText(ImVec2(p.x + 4, p.y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled), "(not found)");
            ImGui::EndGroup();
            continue;
        }
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(side, side));
        if (app.brushTile / 64 == t) { // the brush's tile (the texture's rows run bottom to top)
            const int bt = app.brushTile % 64, cx = bt % 8, cy = 7 - bt / 8;
            dl->AddRect(ImVec2(p.x + cx * cell, p.y + cy * cell), ImVec2(p.x + (cx + 1) * cell, p.y + (cy + 1) * cell), IM_COL32(255, 220, 120, 255), 0, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const int cx = std::clamp(static_cast<int>((mp.x - p.x) / cell), 0, 7), cy = std::clamp(static_cast<int>((mp.y - p.y) / cell), 0, 7);
            const int tile = t * 64 + (7 - cy) * 8 + cx;
            dl->AddRect(ImVec2(p.x + cx * cell, p.y + cy * cell), ImVec2(p.x + (cx + 1) * cell, p.y + (cy + 1) * cell), IM_COL32(255, 255, 255, 220));
            const int type = tile < static_cast<int>(m.tileTypes.size()) ? m.tileTypes[static_cast<size_t>(tile)] : -1;
            ImGui::SetTooltip("tile %d (%s)", tile % 64, type >= 0 && type < 16 ? kTileTypeNames[type] : "?");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { PickTile(app, tile); picked = true; }
        }
        ImGui::EndGroup();
    }
    if (picked || (!open && (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The Tile paint mode's tools: the Grounds tab (paint by material) and the Tiles tab; water paints by tile.
void TilePanel(App& app, bool inShelf) {
    mpr::Map& m = app.terrain;
    ImGui::SeparatorText(app.brushWater ? "Water paint" : "Tile paint");
    if (!inShelf && !app.tileBrush)
        ImGui::TextDisabled("Choose Tile paint or Water paint in the mode selector above the view to paint (%s shows these tools in the view).",
                            ui::BindName(app.lib.mapKeys[config::kKeyToolShelf]).c_str());
    int layer = app.brushWater ? 1 : 0;
    ImGui::RadioButton("Land", &layer, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Water", &layer, 1);
    app.brushWater = layer == 1;
    if (app.brushWater) {
        ImGui::SetNextItemWidth(-1);
        const std::string label = app.brushMaterial < 0 ? std::string("no water (removes it from the tile)") : "material " + std::to_string(app.brushMaterial);
        if (ImGui::BeginCombo("##brushmat", label.c_str())) {
            if (ImGui::Selectable("no water (removes it from the tile)", app.brushMaterial < 0)) app.brushMaterial = -1;
            for (size_t i = 0; i < m.materials.size(); ++i) {
                const mpr::Material& mt = m.materials[i];
                const std::string l = "material " + std::to_string(i) + (mt.type == 3 ? " (water)" : " (terrain)");
                if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == app.brushMaterial)) app.brushMaterial = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
    }
    if (app.brushWater) { app.materialBrush = false; TilesPanel(app); return; }
    tilemat::Terrain* tm = tilemat::Lookup(m.name);
    if (!tm) { // a custom terrain: a materials file says what its tiles are made of: the tool's materials/ folder
               // (shipped tables), then the output folder (tiles borrowed in the editor)
        tilemat::Terrain& own = tilemat::Create(m.name);
        if (own.tiles.empty() && !app.sidecarLoaded.count(app.terrainPath)) {
            app.sidecarLoaded.insert(app.terrainPath);
            const std::string shipped = (std::filesystem::path(config::ExeDir()) / "materials" / (m.name + "-materials.tsv")).string();
            tilemat::LoadSidecar(own, shipped);
            const std::string& folder = BlendFolder(app);
            if (!folder.empty()) tilemat::LoadSidecar(own, (std::filesystem::path(folder) / (m.name + "-materials.tsv")).string());
        }
        if (!own.tiles.empty()) tm = &own;
    }
    if (!tm) {
        app.materialBrush = false;
        ImGui::TextDisabled("No ground data: paint by tile");
        ImGui::SetItemTooltip("Painting by ground needs to know which grounds each tile shows: known for the vanilla terrains only.\n"
                              "A custom terrain can get it from a %s-materials.tsv table (um-multitool's materials/README.md).", m.name.c_str());
        TilesPanel(app);
        return;
    }
    if (ImGui::BeginTabBar("##painttabs")) {
        const int want = app.paintTabWanted;
        app.paintTabWanted = -1;
        if (ImGui::BeginTabItem("Grounds", nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            app.materialBrush = true;
            GroundsPanel(app, *tm);
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint a ground: the cells take its plain tiles and the cells around them the transition tiles the\n"
                                                      "textures hold for the two grounds (corner, half, inner corner, turned as needed)");
        if (ImGui::BeginTabItem("Tiles", nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            app.materialBrush = false;
            TilesPanel(app);
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint one tile at a time, turned as you like");
        ImGui::EndTabBar();
    }
}

static void TerrainTab(App& app) {
    if (!app.terrainLoaded) { ImGui::TextWrapped("Load a terrain (.mpr) in the Files tab to edit its tiles here."); return; }
    mpr::Map& m = app.terrain;
    ImGui::Text("%s", std::filesystem::path(app.terrainPath).filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d sectors, %d textures", m.sectorsX, m.sectorsY, m.textureCount);
    ImGui::BeginDisabled(!TerrainUnsaved(app));
    if (ImGui::Button("Save terrain")) SaveTerrain(app, app.terrainPath);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the edited sectors and the header into the .mpr (%s saves it with the maps)", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save terrain as...")) { app.terrainSaveAsOpen = !app.terrainSaveAsOpen; std::snprintf(app.terrainSaveAsPath, sizeof(app.terrainSaveAsPath), "%s", app.terrainPath.c_str()); }
    if (app.terrainSaveAsOpen) {
        ImGui::SetNextItemWidth(-70);
        ImGui::InputText("##mprsaveas", app.terrainSaveAsPath, sizeof(app.terrainSaveAsPath));
        ImGui::SameLine();
        std::error_code ec;
        const bool exists = std::filesystem::exists(app.terrainSaveAsPath, ec) && !std::filesystem::equivalent(app.terrainSaveAsPath, app.terrainPath, ec);
        ImGui::BeginDisabled(app.terrainSaveAsPath[0] == '\0');
        if (ImGui::Button(exists ? "Replace" : "Save")) { if (SaveTerrain(app, app.terrainSaveAsPath)) app.terrainSaveAsOpen = false; }
        ImGui::EndDisabled();
    }
    if (!app.terrainMessage.empty()) ImGui::TextDisabled("%s", app.terrainMessage.c_str());

    SculptPanel(app);
    TilePanel(app);
    ImGui::SeparatorText("Materials");
    ImGui::TextDisabled("Liquid materials (water tiles use them) and their look; changes are one undo step each.");
    int removeMat = -1;
    for (size_t i = 0; i < m.materials.size(); ++i) {
        mpr::Material& mt = m.materials[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%zu", i);
        ImGui::SameLine(30);
        int type = mt.type == 3 ? 1 : 0;
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("##type", &type, "terrain\0water\0")) { PushTerrainHeaderUndo(app); mt.type = type ? 3 : 1; app.terrainHeaderEdited = true; }
        ImGui::SameLine();
        float col[4] = {mt.r, mt.g, mt.b, mt.a};
        ImGui::SetNextItemWidth(-30);
        if (ImGui::ColorEdit4("##rgba", col, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
            mt.r = col[0]; mt.g = col[1]; mt.b = col[2]; mt.a = col[3];
            app.terrainHeaderEdited = true;
            app.terrainRebuild = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeMat = static_cast<int>(i);
        float v3[3] = {mt.selfIllumination, mt.waveMultiplier, mt.warpSpeed};
        ImGui::Indent(30);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##params", v3, 0.01f, 0.0f, 0.0f, "%.3f")) {
            mt.selfIllumination = v3[0]; mt.waveMultiplier = v3[1]; mt.warpSpeed = v3[2];
            app.terrainHeaderEdited = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Self-illumination, wave (wind) multiplier, warp speed");
        ImGui::Unindent(30);
        ImGui::PopID();
    }
    if (removeMat >= 0) {
        // Water tiles using it lose their water, those after it follow the renumbering.
        PushTerrainHeaderUndo(app);
        EditStep& header = app.undoSteps.back();
        for (size_t si = 0; si < m.sectors.size(); ++si) {
            mpr::Sector& s = m.sectors[si];
            if (!s.water) continue;
            bool touched = false;
            for (auto& rowv : s.waterMaterial) for (int16_t& v : rowv) if (v >= removeMat) touched = true;
            if (!touched) continue;
            header.sectors.push_back({static_cast<int>(si), s});
            for (auto& rowv : s.waterMaterial)
                for (int16_t& v : rowv) { if (v == removeMat) v = -1; else if (v > removeMat) --v; }
            app.terrainEditedSectors.insert(static_cast<int>(si));
        }
        m.materials.erase(m.materials.begin() + removeMat);
        app.terrainHeaderEdited = true;
        app.terrainRebuild = true;
        if (app.brushMaterial >= static_cast<int>(m.materials.size())) app.brushMaterial = static_cast<int>(m.materials.size()) - 1;
    }
    if (ImGui::SmallButton("+ material")) {
        PushTerrainHeaderUndo(app);
        mpr::Material mt;
        mt.type = 3;
        mt.r = 0.2f; mt.g = 0.35f; mt.b = 0.5f; mt.a = 0.7f;
        m.materials.push_back(mt);
        app.terrainHeaderEdited = true;
    }

    ImGui::SeparatorText("Animated tiles");
    ImGui::TextDisabled("A first tile and how many tiles after it the game cycles through.");
    int removeAnim = -1;
    for (size_t i = 0; i < m.animTiles.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        int v[2] = {m.animTiles[i].first, m.animTiles[i].second};
        ImGui::SetNextItemWidth(-30);
        if (ImGui::InputInt2("##anim", v)) {
            m.animTiles[i] = {std::max(0, v[0]), std::max(1, v[1])};
            app.terrainHeaderEdited = true;
        }
        if (ImGui::IsItemActivated()) PushTerrainHeaderUndo(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAnim = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAnim >= 0) { PushTerrainHeaderUndo(app); m.animTiles.erase(m.animTiles.begin() + removeAnim); app.terrainHeaderEdited = true; }
    if (ImGui::SmallButton("+ animated tile")) { PushTerrainHeaderUndo(app); m.animTiles.push_back({app.brushTile, 4}); app.terrainHeaderEdited = true; }
}

void Sidebar(App& app, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PanelBg());
    ImGui::BeginChild("##mapSidebar", ImVec2(width, height), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##maptabs")) {
        const SideTab requested = app.requestTab;
        app.requestTab = SideTab::Count;
        auto tab = [&](SideTab t, const char* label, void (*draw)(App&)) {
            if (ImGui::BeginTabItem(label, nullptr, requested == t ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (app.lib.mapSideTab != static_cast<int>(t) && requested == SideTab::Count) { // remembered for the next start
                    app.lib.mapSideTab = static_cast<int>(t);
                    app.lib.SaveConfig();
                }
                ImGui::BeginChild("##tab", ImVec2(0, 0));
                draw(app);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        };
        tab(SideTab::Files, "Files", FilesTab);
        tab(SideTab::Objects, "Objects", ObjectsTab);
        std::string checksLabel = "Checks";
        if (app.summary.errors || app.summary.warnings)
            checksLabel += " (" + std::to_string(app.summary.errors) + "/" + std::to_string(app.summary.warnings) + ")";
        checksLabel += "###checks";
        tab(SideTab::Checks, checksLabel.c_str(), ChecksTab);
        tab(SideTab::Script, "Script", ScriptTab);
        tab(SideTab::Quest, "Quest", QuestTab);
        tab(SideTab::Diplomacy, "Diplomacy", DiplomacyTab);
        tab(SideTab::Ids, "IDs", IdsTab);
        tab(SideTab::Terrain, TerrainUnsaved(app) ? "Terrain*###terrain" : "Terrain###terrain", TerrainTab);
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The lighting files of the Settings tab, read again when that list changes.
void RefreshLighting(App& app) {
    if (app.lightingFor == app.lib.lightingFiles) return;
    app.lightingFor = app.lib.lightingFiles;
    app.lightTables.clear();
    for (const std::string& path : lighting::Expand(app.lib.lightingFiles)) {
        lighting::Table t;
        if (lighting::Load(path, t)) app.lightTables.push_back(std::move(t));
    }
}

const lighting::Table* ChosenLighting(const App& app) {
    for (const lighting::Table& t : app.lightTables) if (t.path == app.lib.lightingChoice) return &t;
    return app.lightTables.empty() ? nullptr : &app.lightTables.front();
}

// The time of day: the hour slider once moved, else the first loaded map's WORLD_SET time.
float MapHour(const App& app) {
    if (app.hourSet) return app.hour;
    for (auto& m : app.mobs) if (m->file.hasWorld) return m->file.worldTime;
    return 12.0f;
}

void ApplyLighting(App& app) {
    SceneLight& l = app.scene.light;
    const lighting::Table* t = ChosenLighting(app);
    l = SceneLight{};
    if (!app.lib.lightingOn || !t) return;
    float h = MapHour(app);
    lighting::Color sun = lighting::Table::At(t->sun, h), amb = lighting::Table::At(t->ambient, h), sky = lighting::Table::At(t->sky, h);
    l.on = true;
    // The file's colours scaled like a light and an ambient term; the fixed-function sum stays near white at noon.
    const float sunScale = 0.8f, ambScale = 0.55f;
    l.sun[0] = sun.r * sunScale; l.sun[1] = sun.g * sunScale; l.sun[2] = sun.b * sunScale;
    l.ambient[0] = amb.r * ambScale; l.ambient[1] = amb.g * ambScale; l.ambient[2] = amb.b * ambScale;
    l.sky[0] = sky.r; l.sky[1] = sky.g; l.sky[2] = sky.b;
    // Where the sun is at that hour: rising in the east at 6, over the south at noon (70 degrees up),
    // setting in the west at 18. At night it stays low (the light files' night colours are dim) and casts
    // no shadows.
    const float day = std::fmod(h + 24.0f, 24.0f);
    const float across = std::min(std::max((day - 6.0f) / 12.0f, 0.0f), 1.0f); // 0 sunrise .. 1 sunset
    const float pi = 3.14159265f;
    const float elevation = std::max(std::sin(pi * across) * 70.0f, 8.0f) * pi / 180.0f;
    const float azimuth = pi * across;
    l.sunDir[0] = std::cos(azimuth) * std::cos(elevation);
    l.sunDir[1] = -std::sin(azimuth) * std::cos(elevation);
    l.sunDir[2] = std::sin(elevation);
    l.sunUp = day > 6.3f && day < 17.7f;
}

// Clears the patrol points of the selected units' logic in use (ei_maper's "Reset logic paths"). One undo step.
void ResetLogicPaths(App& app) {
    if (app.mobs.empty() || app.scene.selectedFile != app.activeMob || app.scene.selection.empty()) return;
    mob::File& f = app.mobs[app.activeMob]->file;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    int units = 0;
    for (int oi : app.scene.selection) {
        if (oi < 0 || oi >= static_cast<int>(f.objects.size()) || f.objects[oi].kind != mob::Kind::Unit) continue;
        bool cleared = false;
        for (size_t gi = 0; gi < f.objects[oi].logics.size(); ++gi) {
            mob::Logic g = f.objects[oi].logics[gi];
            if (!g.use || g.patrol.empty()) continue;
            g.patrol.clear();
            cleared |= mob::SetLogic(f, oi, static_cast<int>(gi), g);
        }
        units += cleared;
    }
    if (!units) { app.questMessage = "No selected unit has a patrol path"; return; }
    PushUndo(app, std::move(step), "Clear patrol paths");
    app.scene.logicPoints.clear();
    app.checksDirty = true;
    app.listedKey.clear();
    app.questMessage = "Cleared the patrol paths of " + std::to_string(units) + " unit(s)";
}

// Writes the active map to another file, which it then is (its undo steps follow it).
void SaveAsWindow(App& app) {
    if (!app.saveAsOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Save active MOB as", &app.saveAsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    MobEntry& m = *app.mobs[app.activeMob];
    ImGui::Text("%s", m.file.path.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##saveas", app.saveAsPath, sizeof(app.saveAsPath));
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path to(app.saveAsPath);
    const bool same = fs::equivalent(to, m.file.path, ec);
    const bool exists = !same && fs::exists(to, ec);
    if (exists) ImGui::Checkbox("Replace the existing file", &app.saveAsOverwrite);
    ImGui::BeginDisabled(app.saveAsPath[0] == '\0' || same || (exists && !app.saveAsOverwrite) || to.extension().string().empty());
    if (ImGui::Button("Save", ImVec2(120, 0))) {
        const std::string oldPath = m.file.path;
        mob::File copy = m.file;
        copy.path = to.string();
        std::string err;
        if (!mob::Save(copy, err)) {
            app.saveAsMessage = "Not saved: " + err;
        } else {
            m.file.path = copy.path;
            m.file.fileName = to.filename().string();
            m.savedBytes = m.file.bytes;
            // Everything that names the map by its path follows it.
            for (auto* stack : {&app.undoSteps, &app.redoSteps})
                for (EditStep& st : *stack) if (st.file == oldPath) st.file = m.file.path;
            if (app.xf.file == oldPath) app.xf.file = m.file.path;
            if (app.scriptEditFor == oldPath) app.scriptEditFor = m.file.path;
            if (app.scriptExtMob == oldPath) app.scriptExtMob = m.file.path;
            app.saveAsMessage = "Saved as " + m.file.path;
            app.saveAsOverwrite = false;
        }
    }
    ImGui::EndDisabled();
    if (same) ImGui::TextDisabled("That is the map's own file: save it with %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    if (!app.saveAsMessage.empty()) ImGui::TextWrapped("%s", app.saveAsMessage.c_str());
    ImGui::End();
}

// The undo history: every step Ctrl+Z can take back (oldest first) and those Ctrl+Y can do again (grey).
// A click goes back or forward to just after that step.
void HistoryWindow(App& app) {
    if (!app.historyOpen) return;
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMax.x - 340, app.viewportMin.y + 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo history", &app.historyOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::TextDisabled("Click a step to go back or forward to it.");
    int target = -2; // steps to keep done: -1 none
    if (ImGui::Selectable("(start)", app.undoSteps.empty())) target = 0;
    for (size_t i = 0; i < app.undoSteps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool current = i + 1 == app.undoSteps.size();
        if (ImGui::Selectable(app.undoSteps[i].label.c_str(), current)) target = static_cast<int>(i) + 1;
        if (current) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (size_t j = app.redoSteps.size(); j-- > 0;) { // the next to redo first
        ImGui::PushID(1000000 + static_cast<int>(j));
        if (ImGui::Selectable(app.redoSteps[j].label.c_str(), false))
            target = static_cast<int>(app.undoSteps.size() + (app.redoSteps.size() - j));
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    ImGui::End();
    if (target < 0) return;
    for (int guard = 0; guard < 700; ++guard) {
        const size_t done = app.undoSteps.size(), undone = app.redoSteps.size();
        if (static_cast<int>(done) > target) UndoRedo(app, false);
        else if (static_cast<int>(done) < target && undone) UndoRedo(app, true);
        else break;
        if (app.undoSteps.size() == done && app.redoSteps.size() == undone) break; // nothing could be applied
    }
}

// Writes one WORLD_SET value in place (fixed sizes). One undo step.
static void CommitWorld(App& app, uint32_t type, const float* v, int n, const char* what) {
    mob::File& f = app.mobs[app.activeMob]->file;
    const size_t at = mob::WorldSetField(f, type);
    if (!at || at + static_cast<size_t>(n) * 4 > f.bytes.size()) return;
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = f.path;
    step.bytes = f.bytes;
    std::memcpy(f.bytes.data() + at, v, static_cast<size_t>(n) * 4);
    mob::Reparse(f);
    PushUndo(app, std::move(step), what);
}

// ei_maper's MOB parameters: the map's kind and its WORLD_SET (wind, time of day, ambient and sun light),
// with the ID ranges, diplomacy and script a click away.
void MobParamsWindow(App& app) {
    if (!app.mobParamsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 40, app.viewportMin.y + 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MOB parameters", &app.mobParamsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); ImGui::End(); return; }
    mob::File& f = app.mobs[app.activeMob]->file;
    ImGui::Text("%s", f.fileName.c_str());
    {
        bool quest = mob::IsQuestMob(f);
        if (ImGui::Checkbox("Quest MOB", &quest)) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::SetQuestMob(f, quest)) { PushUndo(app, std::move(step), quest ? "Quest MOB on" : "Quest MOB off"); app.checksDirty = true; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: a quest's map, loaded over a zone (PR_OBJECT_DB_FILE). Off: a zone's own map (SC_OBJECT_DB_FILE)");
    }
    ImGui::SeparatorText("World (WORLD_SET)");
    if (!f.hasWorld) {
        ImGui::TextWrapped("This map has no WORLD_SET (quest maps take the zone's). A zone's own map needs one.");
        if (ImGui::Button("Add a WORLD_SET")) {
            EditStep step;
            step.kind = EditStep::Bytes;
            step.file = f.path;
            step.bytes = f.bytes;
            if (mob::AddWorldSet(f)) PushUndo(app, std::move(step), "Add a WORLD_SET");
        }
    } else if (ImGui::BeginTable("##world", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        float v[3];
        Label("Wind direction");
        { const float cur[3] = {f.worldWindDir.x, f.worldWindDir.y, f.worldWindDir.z}; if (EditFloats("##wdir", 3, cur, v)) CommitWorld(app, mob::kWorldWindDir, v, 3, "Wind direction"); }
        Label("Wind strength");
        { const float cur[1] = {f.worldWindStr}; if (EditFloats("##wstr", 1, cur, v)) CommitWorld(app, mob::kWorldWindStr, v, 1, "Wind strength"); }
        Label("Time (hours)");
        { const float cur[1] = {f.worldTime}; if (EditFloats("##wtime", 1, cur, v, "%.2f")) CommitWorld(app, mob::kWorldTime, v, 1, "Time of day"); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The map's time of day, 0 to 24 (the lighting follows it)");
        Label("Ambient");
        { const float cur[1] = {f.worldAmbient}; if (EditFloats("##wamb", 1, cur, v)) CommitWorld(app, mob::kWorldAmbient, v, 1, "Ambient"); }
        Label("Sun light");
        { const float cur[1] = {f.worldSunLight}; if (EditFloats("##wsun", 1, cur, v)) CommitWorld(app, mob::kWorldSunLight, v, 1, "Sun light"); }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Also");
    if (ImGui::Button("ID ranges")) app.requestTab = SideTab::Ids;
    ImGui::SameLine();
    if (ImGui::Button("Diplomacy")) app.requestTab = SideTab::Diplomacy;
    ImGui::SameLine();
    if (ImGui::Button("Script")) { if (app.scriptWindow) app.focusScriptWindow = true; else app.requestTab = SideTab::Script; }
    ImGui::End();
}


} // namespace mapedit
