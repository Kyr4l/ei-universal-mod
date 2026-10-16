// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Minimap export, new terrain from a picture, crop, merge.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// ------------------------------------------------------------------------------------------------
// Minimap export
// ------------------------------------------------------------------------------------------------

// An uncompressed 32-bit BGRA .dds (A8R8G8B8, one mip level), which texture tools and um-multitool
// The minimap as a file: a 32-bit DDS or the 16-bit RGBA5551 .mmp of the game's own quest maps (ddsmmp.cpp's writers).
static bool WriteMinimapFile(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba, bool mmp) {
    std::vector<uint8_t> bytes;
    std::string err;
    const bool made = mmp ? RgbaToMmp(static_cast<uint32_t>(w), static_cast<uint32_t>(h), rgba, bytes, err, MmpFormat::Argb1555)
                          : RgbaToDds(static_cast<uint32_t>(w), static_cast<uint32_t>(h), rgba, bytes);
    if (!made) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

// A small 3D look at a terrain to be made: the heights `height(u, v)` (u, v in 0..1, west to east and south to
// north) drawn as an isometric relief into the current window, each cell shaded by its slope, as a square of
// `side` pixels. Enough to see what the Crop / New terrain tools will make.
static void ReliefPreview(const std::function<float(float, float)>& height, float maxZ, float side, int grid = 40) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(28, 30, 34, 255));
    const float k = side / (grid * 2.0f);                      // a cell's isometric width: the diamond spans the square
    const float zk = side * 0.26f / std::max(maxZ, 1.0f);      // height to pixels
    std::vector<float> h(static_cast<size_t>(grid + 1) * (grid + 1));
    for (int j = 0; j <= grid; ++j) for (int i = 0; i <= grid; ++i) h[static_cast<size_t>(j) * (grid + 1) + i] = height(i / static_cast<float>(grid), j / static_cast<float>(grid));
    auto at = [&](int i, int j) { // isometric: the south-west corner at the left, north up the screen
        return ImVec2(p0.x + side * 0.5f + (i - j) * k * 0.98f, p0.y + side * 0.72f - (i + j) * k * 0.5f - h[static_cast<size_t>(j) * (grid + 1) + i] * zk);
    };
    for (int j = grid - 1; j >= 0; --j)     // back to front
        for (int i = grid - 1; i >= 0; --i) {
            const float z00 = h[static_cast<size_t>(j) * (grid + 1) + i], z10 = h[static_cast<size_t>(j) * (grid + 1) + i + 1], z01 = h[static_cast<size_t>(j + 1) * (grid + 1) + i];
            const float dx = (z10 - z00) * grid / 64.0f, dy = (z01 - z00) * grid / 64.0f;     // slope, lit from the north-west
            const float len = std::sqrt(dx * dx + dy * dy + 1), lit = std::clamp((-dx * -0.5f + dy * 0.5f + 1.0f) / len * 0.75f + 0.25f, 0.2f, 1.0f);
            const float t = std::clamp(z00 / std::max(maxZ, 1.0f), 0.0f, 1.0f);              // low = green, high = brown-grey
            const ImU32 c = IM_COL32(static_cast<int>((70 + 110 * t) * lit), static_cast<int>((120 - 20 * t) * lit), static_cast<int>((50 + 60 * t) * lit), 255);
            dl->AddQuadFilled(at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), c);
        }
    dl->AddRect(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(90, 90, 90, 255));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(200, 200, 200, 200), "N");
}

// Tools > Crop terrain: a rectangle of sectors into a new .mpr; each open .mob is written beside it with only the
// objects inside the rectangle, moved so the rectangle's corner is the origin (their logic points with them).
void CropDialog(App& app) {
    if (!app.cropOpen) return;
    ImGui::SetNextWindowPos(ImVec2(std::max(app.viewportMin.x + 20, app.viewportMax.x - 600), app.viewportMin.y + 40), ImGuiCond_Appearing); // at the right: the rectangle is dragged in the view
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(FLT_MAX, FLT_MAX)); // room for the preview and the text beside it
    if (!ImGui::Begin("Crop terrain", &app.cropOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    const mpr::Map& m = app.terrain;
    ImGui::TextWrapped("Keeps a rectangle of %s's sectors (%d x %d) as a new terrain, the same textures. The maps open beside it are written next to "
                       "it with the objects inside the rectangle, moved so its corner is the origin. Rebuild the navmesh after (Tools).", m.name.c_str(), m.sectorsX, m.sectorsY);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2("From sector (X, Y)", app.cropRect);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2("Sectors (width, height)", app.cropRect + 2);
    app.cropRect[0] = std::clamp(app.cropRect[0], 0, m.sectorsX - 1);
    app.cropRect[1] = std::clamp(app.cropRect[1], 0, m.sectorsY - 1);
    app.cropRect[2] = std::clamp(app.cropRect[2], 1, m.sectorsX - app.cropRect[0]);
    app.cropRect[3] = std::clamp(app.cropRect[3], 1, m.sectorsY - app.cropRect[1]);
    ImGui::TextDisabled("= units x %d..%d, y %d..%d", app.cropRect[0] * 32, (app.cropRect[0] + app.cropRect[2]) * 32, app.cropRect[1] * 32, (app.cropRect[1] + app.cropRect[3]) * 32);
    { // what the rectangle holds, as a small relief (the same rectangle is outlined on the terrain in the view)
        const float x0 = app.cropRect[0] * 32.0f, y0 = app.cropRect[1] * 32.0f, w = app.cropRect[2] * 32.0f, h = app.cropRect[3] * 32.0f;
        ReliefPreview([&](float u, float v) { return m.HeightAt(x0 + u * (w - 0.01f), y0 + v * (h - 0.01f)); }, m.maxZ, 220);
        ImGui::SameLine();
        ImGui::TextWrapped("The rectangle, seen from the south-west. In the view it is outlined in yellow: drag its corners or edges to resize it, its inside to move it (whole sectors).");
    }
    ImGui::Checkbox("The open maps' objects too (<map>-crop.mob beside the terrain)", &app.cropMobs);
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##croppath", app.cropPath, sizeof(app.cropPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.cropPath, picked, "mpr")) std::snprintf(app.cropPath, sizeof(app.cropPath), "%s", picked.c_str());
    ImGui::BeginDisabled(app.cropPath[0] == '\0');
    if (ImGui::Button("Crop and open", ImVec2(160, 0))) {
        std::string path = app.cropPath, err;
        if (!EndsWith(path, ".mpr")) path += ".mpr";
        mpr::Map made;
        if (std::filesystem::exists(path)) app.cropMessage = path + " exists already: pick another name";
        else if (!mpr::Crop(m, app.cropRect[0], app.cropRect[1], app.cropRect[2], app.cropRect[3], path, made, err)) app.cropMessage = "Not cropped: " + err;
        else {
            app.cropMessage = "Cropped into " + path;
            const float dx = app.cropRect[0] * 32.0f, dy = app.cropRect[1] * 32.0f, w = app.cropRect[2] * 32.0f, h = app.cropRect[3] * 32.0f;
            if (app.cropMobs)
                for (auto& mobPtr : app.mobs) {
                    const mob::File& src = mobPtr->file;
                    if (!src.loaded) continue;
                    std::vector<std::vector<uint8_t>> kept;
                    std::vector<int> all;
                    for (size_t i = 0; i < src.objects.size(); ++i) {
                        all.push_back(static_cast<int>(i));
                        const mob::Vec3& p = src.objects[i].position;
                        if (p.x < dx || p.y < dy || p.x >= dx + w || p.y >= dy + h) continue;
                        std::vector<uint8_t> node = mob::ObjectNode(src, static_cast<int>(i));
                        mob::NodeMove(node, -dx, -dy, 0.0f);
                        kept.push_back(std::move(node));
                    }
                    mob::File out = src;
                    mob::RemoveObjects(out, all);
                    mob::InsertObjects(out, kept);
                    const std::filesystem::path mp(src.path);
                    out.path = (std::filesystem::path(path).parent_path() / (mp.stem().string() + "-crop.mob")).string();
                    out.fileName = std::filesystem::path(out.path).filename().string();
                    std::string merr;
                    if (mob::Save(out, merr)) app.cropMessage += "; " + out.fileName + " (" + std::to_string(kept.size()) + " of " + std::to_string(all.size()) + " objects)";
                    else app.cropMessage += "; " + out.fileName + " NOT written: " + merr;
                }
            LoadTerrain(app, path);
        }
    }
    ImGui::EndDisabled();
    if (!app.cropMessage.empty()) ImGui::TextWrapped("%s", app.cropMessage.c_str());
    ImGui::End();
}

// The picture a new terrain takes its heights from (#59, #106): grey, blurred, with the levels / gamma / invert
// tweaks applied, sampled anywhere in 0..1 x 0..1 (v = 0 is the picture's bottom = the map's south).
struct HeightPicture {
    int width = 0, height = 0;
    std::vector<float> grey; // 0..1, after the blur
    std::string key;         // path + blur it was made for
    bool ok = false;
    float Raw(float u, float v) const {
        const int px = std::clamp(static_cast<int>(u * width), 0, width - 1), py = std::clamp(static_cast<int>((1.0f - v) * height), 0, height - 1);
        return grey[static_cast<size_t>(py) * width + px];
    }
    // the tweaks: levels, gamma, invert, then base..top
    float Height(const App& app, float u, float v) const {
        float g = Raw(u, v);
        const float lo = std::min(app.newTerrainBlack, app.newTerrainWhite - 0.001f);
        g = std::clamp((g - lo) / std::max(app.newTerrainWhite - lo, 0.001f), 0.0f, 1.0f);
        g = std::pow(g, std::max(app.newTerrainGamma, 0.05f));
        if (app.newTerrainInvert) g = 1.0f - g;
        return app.newTerrainBase + g * std::max(app.newTerrainHeight - app.newTerrainBase, 0.0f);
    }
};

static HeightPicture& LoadHeightPicture(App& app) {
    static HeightPicture pic;
    const std::string key = std::string(app.newTerrainImage) + "|" + std::to_string(app.newTerrainBlur);
    if (pic.key == key) return pic;
    pic = HeightPicture{};
    pic.key = key;
    std::error_code fec;
    if (!app.newTerrainImage[0] || !std::filesystem::is_regular_file(app.newTerrainImage, fec)) return pic; // a half-typed path can name a folder: reading one throws
    std::ifstream f(app.newTerrainImage, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    mmp::Image img;
    std::string ierr;
    if (bytes.empty() || !DecodeTextureFile(bytes, img, ierr) || !img.width || !img.height) return pic;
    pic.width = static_cast<int>(img.width); pic.height = static_cast<int>(img.height);
    pic.grey.resize(static_cast<size_t>(pic.width) * pic.height);
    for (size_t i = 0; i < pic.grey.size(); ++i) pic.grey[i] = (img.rgba[i * 4] * 0.299f + img.rgba[i * 4 + 1] * 0.587f + img.rgba[i * 4 + 2] * 0.114f) / 255.0f;
    const int r = std::clamp(app.newTerrainBlur, 0, 64);
    if (r > 0) { // a box blur, rows then columns
        std::vector<float> tmp(pic.grey.size());
        for (int y = 0; y < pic.height; ++y)
            for (int x = 0; x < pic.width; ++x) {
                float sum = 0; int n = 0;
                for (int k = -r; k <= r; ++k) { const int xx = x + k; if (xx >= 0 && xx < pic.width) { sum += pic.grey[static_cast<size_t>(y) * pic.width + xx]; ++n; } }
                tmp[static_cast<size_t>(y) * pic.width + x] = sum / n;
            }
        for (int y = 0; y < pic.height; ++y)
            for (int x = 0; x < pic.width; ++x) {
                float sum = 0; int n = 0;
                for (int k = -r; k <= r; ++k) { const int yy = y + k; if (yy >= 0 && yy < pic.height) { sum += tmp[static_cast<size_t>(yy) * pic.width + x]; ++n; } }
                pic.grey[static_cast<size_t>(y) * pic.width + x] = sum / n;
            }
    }
    pic.ok = true;
    return pic;
}

// The tweaked picture as a grey preview texture (remade when the picture or a tweak changes).
static GLuint HeightPicturePreview(App& app, const HeightPicture& pic) {
    char key[256];
    std::snprintf(key, sizeof key, "%s|%d|%.3f|%.3f|%.3f|%.2f|%.2f", pic.key.c_str(), app.newTerrainInvert, app.newTerrainBlack, app.newTerrainWhite, app.newTerrainGamma, app.newTerrainBase, app.newTerrainHeight);
    if (app.newTerrainPreviewKey == key && app.newTerrainPreviewTex) return app.newTerrainPreviewTex;
    app.newTerrainPreviewKey = key;
    if (!pic.ok) return 0;
    const int P = 128;
    std::vector<uint8_t> rgba(static_cast<size_t>(P) * P * 4);
    const float top = std::max(app.newTerrainHeight, 0.001f);
    for (int y = 0; y < P; ++y)
        for (int x = 0; x < P; ++x) {
            const float h = pic.Height(app, (x + 0.5f) / P, 1.0f - (y + 0.5f) / P);
            const uint8_t g = static_cast<uint8_t>(std::clamp(h / top, 0.0f, 1.0f) * 255.0f);
            uint8_t* o = &rgba[(static_cast<size_t>(y) * P + x) * 4];
            o[0] = o[1] = o[2] = g; o[3] = 255;
        }
    if (!app.newTerrainPreviewTex) {
        glGenTextures(1, &app.newTerrainPreviewTex);
        glBindTexture(GL_TEXTURE_2D, app.newTerrainPreviewTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else glBindTexture(GL_TEXTURE_2D, app.newTerrainPreviewTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, P, P, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return app.newTerrainPreviewTex;
}

void NewTerrainDialog(App& app) {
    if (!app.newTerrainOpen) return;
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMin.y + 40), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(FLT_MAX, FLT_MAX)); // room for the preview and the text beside it
    if (!ImGui::Begin("New terrain", &app.newTerrainOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    ImGui::TextWrapped("A flat terrain, every tile the same, with the textures, materials and tile types of the open terrain (%s). "
                       "Then paint it, raise it, add water as usual.", app.terrain.name.c_str());
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2("Sectors (X, Y)", app.newTerrainSize);
    for (int& v : app.newTerrainSize) v = std::clamp(v, 1, 64);
    ImGui::SameLine();
    ImGui::TextDisabled("= %d x %d units", app.newTerrainSize[0] * 32, app.newTerrainSize[1] * 32);
    ImGui::SetNextItemWidth(160);
    ImGui::DragFloat("Ground height", &app.newTerrainHeight, 0.1f, 0.0f, 200.0f, "%.1f");
    { // the terrain to be made: the tweaked picture, and a small relief (flat, or the picture's heights)
        const HeightPicture& pic = LoadHeightPicture(app);
        const float top = std::max(app.newTerrainHeight, 1.0f);
        if (pic.ok) {
            const GLuint tex = HeightPicturePreview(app, pic);
            if (tex) { ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(220, 220)); ImGui::SetItemTooltip("The picture as the heights see it: black = the lowest height, white = the ground height"); ImGui::SameLine(); }
        }
        ReliefPreview([&](float u, float v) { return pic.ok ? pic.Height(app, u, v) : app.newTerrainHeight; }, top * 2, 220);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", pic.ok ? "Left: the tweaked picture. Right: its heights, seen from the south-west (black = the lowest height, white = the ground height)."
                                        : app.newTerrainImage[0] ? "The picture could not be read." : "A flat terrain, seen from the south-west. Give a picture below for heights from its shades.");
    }
    ImGui::SetNextItemWidth(-80);
    ImGui::InputTextWithHint("##heightimage", "heights from a picture (optional): .png, .dds, .mmp...", app.newTerrainImage, sizeof(app.newTerrainImage));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A grey picture stretched over the terrain: black = 0, white = the ground height above. Any size (it is sampled).");
    ImGui::SameLine();
    std::string pickedImage;
    if (ImGui::Button("Picture...") && ui::PickFile(pickedImage)) std::snprintf(app.newTerrainImage, sizeof(app.newTerrainImage), "%s", pickedImage.c_str());
    if (app.newTerrainImage[0]) { // the picture's tweaks (#106)
        const HeightPicture& pic = LoadHeightPicture(app);
        if (pic.ok) {
            ImGui::TextDisabled("%d x %d px", pic.width, pic.height);
            ImGui::SameLine();
            if (ImGui::SmallButton("Sectors from the picture")) { app.newTerrainSize[0] = std::clamp((pic.width + 16) / 32, 1, 64); app.newTerrainSize[1] = std::clamp((pic.height + 16) / 32, 1, 64); }
            ImGui::SetItemTooltip("One pixel = one unit (a 32 px square = one sector)");
        }
        ImGui::SetNextItemWidth(160);
        ImGui::DragFloat("Lowest height", &app.newTerrainBase, 0.1f, 0.0f, 200.0f, "%.1f");
        ImGui::SetItemTooltip("Black gives this height, white the ground height above");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Black point", &app.newTerrainBlack, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Shades at or below it are the lowest height (levels)");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("White point", &app.newTerrainWhite, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Shades at or above it are the ground height (levels)");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Gamma", &app.newTerrainGamma, 0.2f, 4.0f, "%.2f");
        ImGui::SetItemTooltip("Below 1: the lows rise (softer valleys); above 1: only the brightest rise (sharper peaks)");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Blur", &app.newTerrainBlur, 0, 16, "%d px");
        ImGui::SetItemTooltip("Smooths the picture before sampling: rounder hills, fewer steps");
        ImGui::SameLine();
        ImGui::Checkbox("Invert", &app.newTerrainInvert);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset tweaks")) { app.newTerrainInvert = false; app.newTerrainBlack = 0; app.newTerrainWhite = 1; app.newTerrainGamma = 1; app.newTerrainBase = 0; app.newTerrainBlur = 0; }
    }
    const uint16_t tile = PackTile(std::max(app.brushTile, 0), app.brushRotation);
    ImGui::Text("Tile: texture %d, tile %d (the tile brush's)", (tile >> 6) & 0xFF, tile & 63);
    ImGui::Checkbox("Its own textures", &app.newTerrainOwnTextures);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the new terrain keeps the name inside the file (%s), so it uses that terrain's textures.\n"
                          "On: it is named after the file, and copies of the textures are written next to it\n"
                          "(<name>000.mmp...): add that folder to the texture sources, or pack them into textures.res.", app.terrain.name.c_str());
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##newterrain", app.newTerrainPath, sizeof(app.newTerrainPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.newTerrainPath, picked, "mpr")) std::snprintf(app.newTerrainPath, sizeof(app.newTerrainPath), "%s", picked.c_str());
    ImGui::BeginDisabled(!app.terrainLoaded || app.newTerrainPath[0] == '\0');
    if (ImGui::Button("Create and open", ImVec2(160, 0))) {
        std::string path = app.newTerrainPath, err;
        if (!EndsWith(path, ".mpr")) path += ".mpr";
        std::string stem = std::filesystem::path(path).stem().string();
        for (char& ch : stem) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::string name = app.newTerrainOwnTextures ? stem : app.terrain.name;
        mpr::Map made;
        if (std::filesystem::exists(path)) app.newTerrainMessage = path + " exists already: pick another name";
        else if (!mpr::Create(app.terrain, name, app.newTerrainSize[0], app.newTerrainSize[1], app.newTerrainHeight, tile, path, made, err))
            app.newTerrainMessage = "Not created: " + err;
        else {
            std::string copied;
            if (app.newTerrainImage[0]) { // the heights from the picture, as the preview showed them
                const HeightPicture& pic = LoadHeightPicture(app);
                if (!pic.ok) app.newTerrainMessage = "The picture could not be read; ";
                else {
                    const float k = 65535.0f / made.maxZ, W = made.sectorsX * 32.0f, H = made.sectorsY * 32.0f;
                    for (size_t si = 0; si < made.sectors.size(); ++si) {
                        mpr::Sector& sec = made.sectors[si];
                        const int sx = static_cast<int>(si) % made.sectorsX, sy = static_cast<int>(si) / made.sectorsX;
                        for (int r = 0; r <= 32; ++r)
                            for (int c = 0; c <= 32; ++c) {
                                const float u = (sx * 32.0f + c) / W, v = (sy * 32.0f + r) / H; // the picture's top = the map's far side (+y)
                                sec.land[r][c].z = static_cast<uint16_t>(std::lround(std::clamp(pic.Height(app, u, v), 0.0f, made.maxZ) * k));
                            }
                    }
                    for (size_t si = 0; si < made.sectors.size(); ++si) mpr::ComputeNormals(made, static_cast<int>(si));
                    std::vector<int> all;
                    for (size_t si = 0; si < made.sectors.size(); ++si) all.push_back(static_cast<int>(si));
                    if (!mpr::Save(made, path, all, true, err)) app.newTerrainMessage = "Heights not written: " + err + "; ";
                }
            }
            if (app.newTerrainOwnTextures)
                for (int i = 0; i < app.terrain.textureCount; ++i) {
                    std::vector<uint8_t> bytes;
                    std::string found;
                    if (!app.lib.textures.ReadTexture(app.terrain.name + "00" + std::to_string(i), bytes, &found)) continue;
                    const std::string ext = std::filesystem::path(found).extension().string();
                    const std::string out = (std::filesystem::path(path).parent_path() / (name + "00" + std::to_string(i) + (ext.empty() ? ".mmp" : ext))).string();
                    std::ofstream f(out, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (f) copied += (copied.empty() ? "" : ", ") + std::filesystem::path(out).filename().string();
                }
            app.newTerrainMessage += "Created " + path + (copied.empty() ? "" : "; textures written: " + copied);
            LoadTerrain(app, path);
        }
    }
    ImGui::EndDisabled();
    if (!app.newTerrainMessage.empty()) ImGui::TextWrapped("%s", app.newTerrainMessage.c_str());
    ImGui::End();
}


// Merge from another map (#92): the use case is an extended terrain whose decorations live in another .mob: pull
// its objects into the active map without its units and scripts, and without doubling what is already there.
struct MergeCandidate { int index; bool duplicate; };
static std::vector<MergeCandidate> MergeCandidates(const App& app, int src) {
    std::vector<MergeCandidate> out;
    if (src < 0 || src >= static_cast<int>(app.mobs.size()) || src == app.activeMob) return out;
    const mob::File& from = app.mobs[static_cast<size_t>(src)]->file;
    const mob::File& into = app.mobs[app.activeMob]->file;
    const std::string needle = Lower(app.mergeFilter);
    // the active map's objects by figure, for the duplicate test
    std::map<std::string, std::vector<mob::Vec3>> have;
    for (const mob::Object& o : into.objects) have[Lower(o.templ.empty() ? o.name : o.templ)].push_back(o.position);
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
    bool area = false;
    if (app.mergeOnlySelectedArea && app.scene.selectedFile == app.activeMob && !app.scene.selection.empty()) {
        area = true;
        ax0 = ay0 = 1e9f; ax1 = ay1 = -1e9f;
        for (int oi : app.scene.selection) {
            const mob::Vec3& p = into.objects[static_cast<size_t>(oi)].position;
            ax0 = std::min(ax0, p.x); ay0 = std::min(ay0, p.y); ax1 = std::max(ax1, p.x); ay1 = std::max(ay1, p.y);
        }
    }
    for (int oi = 0; oi < static_cast<int>(from.objects.size()); ++oi) {
        const mob::Object& o = from.objects[static_cast<size_t>(oi)];
        if (!app.mergeKinds[static_cast<int>(o.kind)]) continue;
        const std::string key = Lower(o.templ.empty() ? o.name : o.templ);
        if (!needle.empty() && key.find(needle) == std::string::npos && Lower(o.name).find(needle) == std::string::npos) continue;
        if (area && (o.position.x < ax0 || o.position.x > ax1 || o.position.y < ay0 || o.position.y > ay1)) continue;
        bool dup = false;
        auto it = have.find(key);
        if (it != have.end())
            for (const mob::Vec3& p : it->second)
                if (std::hypot(p.x - o.position.x, p.y - o.position.y) <= app.mergeDupDist) { dup = true; break; }
        out.push_back({oi, dup});
    }
    return out;
}

void MergeWindow(App& app) {
    if (!app.mergeOpen) return;
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMin.y + 40), ImGuiCond_Appearing);
    if (!ImGui::Begin("Merge from another map", &app.mergeOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    if (app.mobs.size() < 2) { ImGui::TextWrapped("%s", i18n::Tr("Load the map to take the objects from as a second .mob (Files tab); the active map receives them.")); ImGui::End(); return; }
    ImGui::TextWrapped("%s", i18n::Tr("Copies objects of another loaded .mob into the active map, where they stand. Units and traps are off by default (they carry scripts); objects the active map already has at the same spot are skipped."));
    ImGui::Text("%s: %s", i18n::Tr("Into"), app.mobs[app.activeMob]->file.fileName.c_str());
    if (app.mergeSource < 0 || app.mergeSource >= static_cast<int>(app.mobs.size()) || app.mergeSource == app.activeMob) {
        app.mergeSource = -1;
        for (int i = 0; i < static_cast<int>(app.mobs.size()); ++i) if (i != app.activeMob) { app.mergeSource = i; break; }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##mergesrc", app.mergeSource >= 0 ? app.mobs[static_cast<size_t>(app.mergeSource)]->file.fileName.c_str() : "")) {
        for (int i = 0; i < static_cast<int>(app.mobs.size()); ++i)
            if (i != app.activeMob && ImGui::Selectable(app.mobs[static_cast<size_t>(i)]->file.fileName.c_str(), i == app.mergeSource)) app.mergeSource = i;
        ImGui::EndCombo();
    }
    static const char* const kindNames[8] = {"World objects", "Units", "Levers", "Torches", "Magic traps", "Lights", "Particles", "Sounds"};
    for (int k = 0; k < 8; ++k) {
        if (k % 4) ImGui::SameLine();
        ImGui::Checkbox(kindNames[k], &app.mergeKinds[k]);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mergefilter", "only figures or names containing...", app.mergeFilter, sizeof(app.mergeFilter));
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##mergedup", &app.mergeDupDist, 0.0f, 10.0f, "Duplicate within %.1f units");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("A source object with the same figure this close to one of the active map is not copied (0: everything is copied)"));
    ImGui::Checkbox("Only inside the selection's area", &app.mergeOnlySelectedArea);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("The rectangle around the active map's selected objects"));
    const std::vector<MergeCandidate> cands = MergeCandidates(app, app.mergeSource);
    size_t dups = 0;
    for (const MergeCandidate& c : cands) dups += c.duplicate;
    ImGui::Text("%zu %s, %zu %s", cands.size() - dups, i18n::Tr("object(s) to copy"), dups, i18n::Tr("skipped as duplicates"));
    ImGui::BeginDisabled(cands.size() == dups);
    if (ImGui::Button("Merge", ImVec2(120, 0))) {
        const mob::File& from = app.mobs[static_cast<size_t>(app.mergeSource)]->file;
        std::vector<App::Copied> items;
        for (const MergeCandidate& c : cands)
            if (!c.duplicate) items.push_back({mob::ObjectNode(from, c.index), from.objects[static_cast<size_t>(c.index)].position});
        if (InsertCopies(app, items, true, {})) app.questMessage = "Merged " + std::to_string(items.size()) + " object(s) from " + from.fileName + " (selected: undo takes them back)";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", i18n::Tr("The copies get new IDs and come selected."));
    ImGui::End();
}

void MinimapDialog(App& app) {
    if (!app.minimapOpen) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 20, app.viewportMin.y + 40), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export minimap", &app.minimapOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    ImGui::TextWrapped("The terrain seen straight from above, like the game's <zone>map textures: a square image, the map in its "
                       "top-left corner with its longer side filling it, y = 0 at the top.");
    static const int sizes[] = {256, 512, 1024, 2048};
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("Size", (std::to_string(app.minimapSize) + " px").c_str())) {
        for (int sz : sizes) if (ImGui::Selectable((std::to_string(sz) + " px").c_str(), sz == app.minimapSize)) app.minimapSize = sz;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Objects", &app.minimapObjects);
    ImGui::SameLine();
    ImGui::Checkbox("Units", &app.minimapUnits);
    ImGui::TextDisabled("Lighting follows the view (Lighting on: the map's lighting file and hour).");
    ImGui::TextUnformatted("Save as");
    ImGui::SameLine();
    ImGui::Checkbox(".mmp", &app.minimapMmp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("16-bit RGBA5551, the format of the game's own quest map textures");
    ImGui::SameLine();
    ImGui::Checkbox(".dds", &app.minimapDds);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uncompressed 32-bit (A8R8G8B8): for texture tools, or um-multitool ddsmmp");
    ImGui::SameLine();
    ImGui::Checkbox(".png", &app.minimapPng);
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##minipath", app.minimapPath, sizeof(app.minimapPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.minimapPath, picked, "mmp")) std::snprintf(app.minimapPath, sizeof(app.minimapPath), "%s", picked.c_str());
    ImGui::BeginDisabled(!app.terrainLoaded || app.minimapPath[0] == '\0' || app.scene.modelsPending > 0 ||
                         !(app.minimapMmp || app.minimapDds || app.minimapPng));
    if (ImGui::Button("Export", ImVec2(120, 0))) { app.minimapPending = true; app.minimapMessage = "Exporting..."; }
    ImGui::EndDisabled();
    if (app.scene.modelsPending > 0) { ImGui::SameLine(); ImGui::TextDisabled("waiting for figures to load"); }
    if (!app.minimapMessage.empty()) ImGui::TextWrapped("%s", app.minimapMessage.c_str());
    ImGui::End();
}

void ExportMinimap(App& app, int fbW, int fbH) {
    std::vector<uint8_t> rgba;
    if (!app.scene.RenderMinimap(app.lib, app.minimapSize, app.minimapObjects, app.minimapUnits, fbW, fbH, rgba)) {
        app.minimapMessage = "Nothing to export: load a terrain first";
        return;
    }
    // The path's extension, if one of the formats, is dropped: each format gets its own.
    std::string base = app.minimapPath;
    for (const char* ext : {".mmp", ".dds", ".png"}) if (EndsWith(base, ext)) { base.resize(base.size() - 4); break; }
    const int n = app.minimapSize;
    std::vector<std::string> saved, failed;
    auto one = [&](bool want, const char* ext, bool (*write)(const std::string&, int, int, const std::vector<uint8_t>&)) {
        if (want) (write(base + ext, n, n, rgba) ? saved : failed).push_back(base + ext);
    };
    one(app.minimapMmp, ".mmp", [](const std::string& p, int w, int h, const std::vector<uint8_t>& px) { return WriteMinimapFile(p, w, h, px, true); });
    one(app.minimapDds, ".dds", [](const std::string& p, int w, int h, const std::vector<uint8_t>& px) { return WriteMinimapFile(p, w, h, px, false); });
    one(app.minimapPng, ".png", [](const std::string& p, int w, int h, const std::vector<uint8_t>& px) { return png::Write(p, w, h, px); });
    auto join = [](const std::vector<std::string>& v) { std::string r; for (const auto& x : v) r += (r.empty() ? "" : ", ") + x; return r; };
    app.minimapMessage.clear();
    if (!saved.empty()) app.minimapMessage = "Saved " + join(saved);
    if (!failed.empty()) app.minimapMessage += (saved.empty() ? "" : "; ") + std::string("could not write ") + join(failed);
}


} // namespace mapedit
