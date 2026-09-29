/**
 * ============================================================================
 * um-modelviewer2 - Evil Islands item model viewer
 * ============================================================================
 *
 * Browses the items database (database.res / databaselmp.res) by category, one
 * tab each - Weapons, Armors, Quick Items, Quest Items, Loot Items - and shows
 * the selected item's ground/inventory figure with the texture the database
 * points at (see item_resolve.hpp for the naming rules). Figures and textures
 * come from layered sources: .res archives or folders of loose files (.mmp or
 * .dds textures), base game first, mods on top.
 *
 * Successor of um-modelviewer (units with equipment): same toolkit - Dear ImGui
 * + GLFW + fixed-function OpenGL 2 - but item-centred and split into modules:
 *   item_db.hpp       items.idb parser (all six blocks)
 *   item_resolve.hpp  database row -> figure + candidate textures (no GL)
 *   library.hpp       sources + database + name indexes (no GL)
 *   scene.hpp         the GL viewport
 *   ui_*.hpp          the tabs
 * A Units tab can be added as one more tab over the same Library and Scene.
 * ============================================================================
 */

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "library.hpp"
#include "scene.hpp"
#include "ui_items.hpp"
#include "ui_sources.hpp"

static double g_scrollY = 0.0; // raw wheel input for zooming over the bare viewport

struct App {
    Library lib;
    Scene scene;
    ui::SourcesState sources;
    ui::ItemTabState tabs[static_cast<int>(items::Category::Count)];
    int activeTab = 0;          // 0..4 = item categories, 5 = Sources
    int startTab = -1;          // set by --open
    std::string screenshotPath; // --screenshot: save the window after a few frames, then quit
    int frame = 0;

    // GIF export dialog
    bool gifOpen = false;
    bool gifPending = false;     // export at the start of the next frame (the capture uses the back buffer)
    bool gifPreview = false;     // play the export's frames in the viewport
    double gifPreviewStart = 0.0;
    char gifPath[1024] = "";
    std::string gifMessage;
    bool gifMessageOk = true;
    std::string selectedName;    // the current tab's selected item, for the default file name
    float sidebarWidth = 480.0f;
    bool draggingOrbit = false, draggingPan = false;
};

// --------------------------------------------------------------------------
// Command line
// --------------------------------------------------------------------------

static bool ParseCategory(const std::string& s, items::Category& out) {
    std::string l = ui::LowerCopy(s);
    if (l.rfind("weapon", 0) == 0) out = items::Category::Weapons;
    else if (l.rfind("armor", 0) == 0) out = items::Category::Armors;
    else if (l.rfind("quick", 0) == 0) out = items::Category::QuickItems;
    else if (l.rfind("quest", 0) == 0) out = items::Category::QuestItems;
    else if (l.rfind("loot", 0) == 0) out = items::Category::LootItems;
    else return false;
    return true;
}

static void PrintHelp() {
    std::printf(
        "um-modelviewer2 - Evil Islands item model viewer\n\n"
        "Usage:\n"
        "  um-modelviewer2                              Open the viewer.\n"
        "  um-modelviewer2 --list <category>            List the category's items and what they resolve to.\n"
        "  um-modelviewer2 --resolve <category> <item> [--material <name>]\n"
        "                                               Show the figure and every candidate texture of one item.\n"
        "  um-modelviewer2 --render <category> <item> <out.bmp> [--material <name>] [--texture <name>]\n"
        "                                               Render one item to a BMP image (needs a display).\n"
        "  um-modelviewer2 --open <category> <item>     Open the viewer on that item.\n"
        "  --screenshot <out.bmp>                       With the viewer: save the window after a few frames and quit.\n"
        "  um-modelviewer2 --gif <category> <item> <out.gif> [--material <name>] [--texture <name>]\n"
        "                                               Export a 360 degree turn as an animated GIF, with the GIF\n"
        "                                               settings of the viewer's export dialog (needs a display).\n"
        "  --config <file>                              Use another sources file than um-modelviewer2.cfg.\n\n"
        "Categories: weapons, armors, quick, quest, loot. The sources (figures, textures, database)\n"
        "are the ones set in the viewer's Sources tab, saved in um-modelviewer2.cfg.\n");
}

static const items::Item* FindItem(const Library& lib, items::Category c, const std::string& name) {
    for (auto& it : lib.db.List(c)) if (it.name == name) return &it;
    for (auto& it : lib.db.List(c)) if (ui::LowerCopy(it.name) == ui::LowerCopy(name)) return &it;
    return nullptr;
}

static int RunList(const Library& lib, items::Category c) {
    for (auto& it : lib.db.List(c)) {
        auto r = resolve::Resolve(lib.db, it, lib.figureIndex, lib.textureIndex, nullptr);
        std::printf("%-36s %-12s figure %-14s %-4s texture %s\n", it.name.c_str(), it.type.c_str(),
                    r.figure.empty() ? "-" : r.figure.c_str(), r.loadable.empty() ? "MISS" : "ok",
                    r.texture >= 0 ? r.textures[r.texture].name.c_str() : "-");
    }
    return 0;
}

static int RunResolve(const Library& lib, items::Category c, const std::string& name, const std::string& material) {
    const items::Item* it = FindItem(lib, c, name);
    if (!it) { std::fprintf(stderr, "no %s named \"%s\"\n", items::CategoryLabel(c), name.c_str()); return 1; }
    auto r = resolve::Resolve(lib.db, *it, lib.figureIndex, lib.textureIndex, material.empty() ? nullptr : lib.db.FindMaterial(material));
    std::printf("%s: type %s, material class %s, TTI %d, TTI2 %d\n", it->name.c_str(), it->type.c_str(),
                it->materialType.c_str(), it->tti, it->tti2);
    std::printf("figure   %s (%s)\n", r.figure.empty() ? "-" : r.figure.c_str(), r.loadable.empty() ? "not found" : ("loads " + r.loadable).c_str());
    std::printf("material %s\n", r.material >= 0 ? r.materials[r.material]->name.c_str() : "-");
    for (size_t i = 0; i < r.textures.size(); ++i) {
        std::printf("%s %-26s %s\n", static_cast<int>(i) == r.texture ? "*" : " ", r.textures[i].name.c_str(), r.textures[i].note.c_str());
    }
    for (auto& n : r.notes) std::printf("note: %s\n", n.c_str());
    return 0;
}

static bool WriteBmp(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t dataSize = static_cast<uint32_t>(rowSize * h), fileSize = 54 + dataSize;
    uint8_t header[54] = {'B', 'M'};
    std::memcpy(header + 2, &fileSize, 4);
    uint32_t offset = 54, infoSize = 40;
    uint16_t planes = 1, bpp = 24;
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &infoSize, 4);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &dataSize, 4);
    f.write(reinterpret_cast<char*>(header), 54);
    std::vector<uint8_t> row(rowSize, 0);
    for (int y = 0; y < h; ++y) { // glReadPixels rows are bottom-up, like BMP's
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = rgb[(y * w + x) * 3 + 2];
            row[x * 3 + 1] = rgb[(y * w + x) * 3 + 1];
            row[x * 3 + 2] = rgb[(y * w + x) * 3 + 0];
        }
        f.write(reinterpret_cast<char*>(row.data()), rowSize);
    }
    return true;
}

static int RunRender(Library& lib, items::Category c, const std::string& name, const std::string& out,
                     const std::string& material, const std::string& texture) {
    const items::Item* it = FindItem(lib, c, name);
    if (!it) { std::fprintf(stderr, "no %s named \"%s\"\n", items::CategoryLabel(c), name.c_str()); return 1; }
    auto r = resolve::Resolve(lib.db, *it, lib.figureIndex, lib.textureIndex, material.empty() ? nullptr : lib.db.FindMaterial(material));
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    const int w = 640, h = 480;
    GLFWwindow* window = glfwCreateWindow(w, h, "render", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    Scene scene;
    scene.LoadModel(lib, r.loadable, true);
    auto rot = lib.rotations.find(items::CategoryKey(c)); // the same rotation as the viewer's tab
    if (rot != lib.rotations.end()) for (int a = 0; a < 3; ++a) scene.rotationDegrees[a] = rot->second[a];
    scene.textureName = !texture.empty() ? texture : (r.texture >= 0 ? r.textures[r.texture].name : "");
    int status = 0;
    if (!scene.hasModel) {
        std::fprintf(stderr, "figure not loaded: %s\n", scene.modelError.empty() ? "none resolved" : scene.modelError.c_str());
        status = 1;
    } else {
        scene.Draw(lib, 0, 0, w, h, 0.0f);
        glFinish();
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        if (!WriteBmp(out, w, h, rgb)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); status = 1; }
        else std::printf("%s: figure %s, texture %s -> %s\n", it->name.c_str(), scene.modelName.c_str(),
                         scene.textureName.empty() ? "-" : scene.textureName.c_str(), out.c_str());
    }
    scene.ClearTextures();
    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

static bool ExportGif(Library& lib, Scene& scene, const std::string& path, int size, std::string& message);

static int RunGif(Library& lib, items::Category c, const std::string& name, const std::string& out,
                  const std::string& material, const std::string& texture) {
    const items::Item* it = FindItem(lib, c, name);
    if (!it) { std::fprintf(stderr, "no %s named \"%s\"\n", items::CategoryLabel(c), name.c_str()); return 1; }
    auto r = resolve::Resolve(lib.db, *it, lib.figureIndex, lib.textureIndex, material.empty() ? nullptr : lib.db.FindMaterial(material));
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    int size = std::max(16, lib.gif.size);
    GLFWwindow* window = glfwCreateWindow(size, size, "gif", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    Scene scene;
    scene.LoadModel(lib, r.loadable, true);
    scene.textureName = !texture.empty() ? texture : (r.texture >= 0 ? r.textures[r.texture].name : "");
    auto rot = lib.rotations.find(items::CategoryKey(c));
    if (rot != lib.rotations.end()) for (int a = 0; a < 3; ++a) scene.rotationDegrees[a] = rot->second[a];
    std::string message;
    bool ok = ExportGif(lib, scene, out, size, message);
    std::printf("%s\n", message.c_str());
    scene.ClearTextures();
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
}

// --------------------------------------------------------------------------
// Viewer
// --------------------------------------------------------------------------

static std::string SafeFileName(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? "item" : out;
}

static std::string DescribeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    long long bytes = f.is_open() ? static_cast<long long>(f.tellg()) : 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    return buf;
}

// Captures the turn and writes the GIF; the framebuffer must be at least size x size.
static bool ExportGif(Library& lib, Scene& scene, const std::string& path, int size, std::string& message) {
    std::vector<gif::Frame> frames = scene.CaptureTurntable(lib, lib.gif, size);
    if (frames.empty()) { message = "Nothing to export: no model shown"; return false; }
    int delayCs = std::max(2, static_cast<int>(std::lround(100.0 / std::max(lib.gif.fps, 1))));
    if (!gif::Write(path, size, size, frames, delayCs, lib.gif.transparent, message)) return false;
    message = "Saved " + path + " (" + std::to_string(frames.size()) + " frames, " + DescribeFile(path) + ")";
    return true;
}

static void GifDialog(App& app, int maxSize) {
    if (!app.gifOpen) return;
    // Top-left of the viewport, out of the way of the preview (bottom-right).
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.sidebarWidth + 10.0f, 44.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export GIF", &app.gifOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    config::GifSettings& g = app.lib.gif;
    bool changed = false;
    ImGui::TextWrapped("A full 360 degree turn of %s, from the current camera angle and zoom.",
                       app.selectedName.empty() ? "the shown item" : app.selectedName.c_str());
    ImGui::Spacing();
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::SliderInt("Size (px)", &g.size, 64, std::max(64, maxSize));
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::SliderInt("Frames per second", &g.fps, 5, 50);
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::SliderFloat("Speed (degrees/s)", &g.degreesPerSecond, 10.0f, 360.0f, "%.0f");
    ImGui::TextUnformatted("Spin about");
    for (int a = 0; a < 3; ++a) {
        ImGui::SameLine();
        static const char* const names[3] = {"X", "Y", "Z (vertical)"};
        changed |= ImGui::RadioButton(names[a], &g.axis, a);
    }
    changed |= ImGui::Checkbox("Turn the other way", &g.reverse);
    changed |= ImGui::Checkbox("Transparent background", &g.transparent);
    if (!g.transparent) {
        float c[3] = {((g.background >> 16) & 0xFF) / 255.0f, ((g.background >> 8) & 0xFF) / 255.0f, (g.background & 0xFF) / 255.0f};
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##gifbg", c, ImGuiColorEditFlags_NoInputs)) {
            g.background = (static_cast<unsigned>(c[0] * 255 + 0.5f) << 16) | (static_cast<unsigned>(c[1] * 255 + 0.5f) << 8) |
                           static_cast<unsigned>(c[2] * 255 + 0.5f);
            changed = true;
        }
    }
    g.size = std::min(g.size, std::max(64, maxSize));
    int frames = Scene::TurntableFrames(g);
    ImGui::TextDisabled("%d frames, %.1f s per turn", frames, frames / static_cast<float>(std::max(g.fps, 1)));
    if (g.fps > 50 || 100 % g.fps) ImGui::TextDisabled("GIF timing is in 1/100 s: plays at %.1f fps",
                                                      100.0f / std::max(2, static_cast<int>(std::lround(100.0 / g.fps))));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##gifpath", app.gifPath, sizeof(app.gifPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("Browse...") && ui::PickSaveFile(app.gifPath, picked)) std::snprintf(app.gifPath, sizeof(app.gifPath), "%s", picked.c_str());
    ImGui::BeginDisabled(!app.scene.hasModel);
    if (ImGui::Button(app.gifPreview ? "Stop preview" : "Preview", ImVec2(120, 0))) {
        app.gifPreview = !app.gifPreview;
        app.gifPreviewStart = glfwGetTime();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the GIF's frames in the viewport, at its size and speed");
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.scene.hasModel || app.gifPath[0] == '\0');
    if (ImGui::Button("Export", ImVec2(120, 0))) {
        app.gifPending = true;
        app.gifMessage = "Exporting...";
        app.gifMessageOk = true;
    }
    ImGui::EndDisabled();
    if (!app.gifMessage.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, app.gifMessageOk ? ImVec4(0.5f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
        ImGui::TextWrapped("%s", app.gifMessage.c_str());
        ImGui::PopStyleColor();
    }
    if (changed) app.lib.SaveConfig();
    ImGui::End();
}

// The shown model gets the rotation of the tab it belongs to (the Sources tab keeps the last one).
static void ApplyRotation(App& app) {
    if (app.activeTab >= static_cast<int>(items::Category::Count)) return;
    auto it = app.lib.rotations.find(items::CategoryKey(static_cast<items::Category>(app.activeTab)));
    for (int a = 0; a < 3; ++a) app.scene.rotationDegrees[a] = it == app.lib.rotations.end() ? 0 : it->second[a];
}

static void ViewportToolbar(App& app, float x, float y, float w) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##view", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ViewOptions& o = app.scene.options;
    ImGui::Checkbox("Textured", &o.textured);
    ImGui::SameLine();
    ImGui::Checkbox("Lighting", &o.lighting);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &o.wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &o.grid);
    ImGui::SameLine();
    ImGui::Checkbox("Turn", &o.autoRotate);
    ImGui::SameLine();
    ImGui::Checkbox("Atlas UVs", &o.atlasUvs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Item figures address a 256x256 texture atlas, not the texture itself.\n"
                                                  "Keep this on unless a custom figure maps its texture directly.");
    ImGui::SameLine();
    if (ImGui::Button("Frame")) app.scene.Frame();
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.scene.hasModel);
    if (ImGui::Button("Export GIF...")) {
        app.gifOpen = true;
        app.gifMessage.clear();
        std::string dir = app.lib.gif.lastDirectory.empty() ? config::ExeDir() : app.lib.gif.lastDirectory;
        std::snprintf(app.gifPath, sizeof(app.gifPath), "%s/%s.gif", dir.c_str(), SafeFileName(app.selectedName).c_str());
    }
    ImGui::EndDisabled();
    // Rotation of the current tab's models, remembered per tab in um-modelviewer2.cfg.
    if (app.activeTab < static_cast<int>(items::Category::Count)) {
        std::array<int, 3>& r = app.lib.rotations[items::CategoryKey(static_cast<items::Category>(app.activeTab))];
        static const char* const axes[3] = {"X", "Y", "Z"};
        ImGui::SameLine();
        ImGui::TextDisabled(" Rotate");
        for (int a = 0; a < 3; ++a) {
            ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof(label), "%s %d##rot%d", axes[a], r[a], a);
            if (ImGui::Button(label)) {
                r[a] = (r[a] + 45) % 360;
                app.lib.SaveConfig();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn this tab's models 45 degrees about %s", axes[a]);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!r[0] && !r[1] && !r[2]);
        if (ImGui::Button("Reset##rot")) {
            r = {0, 0, 0};
            app.lib.SaveConfig();
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::ColorEdit3("##bg", o.background, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background colour");
    ImGui::SameLine();
    ImGui::TextDisabled("  left-drag orbit, right-drag pan, wheel zoom");
    ImGui::End();
}

static void Sidebar(App& app, float height) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(app.sidebarWidth, height));
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, height), ImVec2(900, height));
    ImGui::Begin("um-modelviewer2", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    app.sidebarWidth = ImGui::GetWindowWidth();
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        // Select the start tab once; read it before the loop, which updates activeTab as it goes.
        static bool firstFrame = true;
        const int requested = firstFrame ? app.activeTab : -1;
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c) {
            ImGuiTabItemFlags flags = requested == c ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(items::CategoryLabel(static_cast<items::Category>(c)), nullptr, flags)) {
                app.activeTab = c;
                ImGui::BeginChild("##tab", ImVec2(0, 0));
                ui::ItemTab(app.lib, app.scene, static_cast<items::Category>(c), app.tabs[c]);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        const int sourcesTab = static_cast<int>(items::Category::Count);
        ImGuiTabItemFlags flags = requested == sourcesTab ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Sources", nullptr, flags)) {
            app.activeTab = sourcesTab;
            ImGui::BeginChild("##tab", ImVec2(0, 0));
            ui::SourcesTab(app.lib, app.scene, app.sources);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        firstFrame = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void CameraInput(App& app, float viewportLeft) {
    ImGuiIO& io = ImGui::GetIO();
    bool overViewport = io.MousePos.x >= viewportLeft && !io.WantCaptureMouse;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overViewport) app.draggingOrbit = true;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && overViewport) app.draggingPan = true;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) app.draggingOrbit = false;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) app.draggingPan = false;
    OrbitCamera& cam = app.scene.camera;
    if (app.draggingOrbit) cam.Orbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
    if (app.draggingPan) {
        float k = cam.distance * 0.0015f; // pan speed follows the zoom, so tiny items stay controllable
        cam.Pan(-io.MouseDelta.x * k, io.MouseDelta.y * k);
    }
    if (overViewport && g_scrollY != 0.0) {
        // Zoom by a ratio, not a fixed step: items range from a ring to a two-metre spear.
        cam.distance *= std::pow(0.87f, static_cast<float>(g_scrollY));
        cam.distance = std::max(0.01f, std::min(cam.distance, 100.0f));
    }
    g_scrollY = 0.0;
}

static int RunViewer(App& app) {
    glfwSetErrorCallback([](int error, const char* description) { std::fprintf(stderr, "GLFW error %d: %s\n", error, description); });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    GLFWwindow* window = glfwCreateWindow(1400, 860, "um-modelviewer2", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = config::ExeDir() + "/um-modelviewer2-imgui.ini";
    io.IniFilename = iniPath.c_str();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();
    glfwSetScrollCallback(window, [](GLFWwindow* w, double dx, double dy) {
        ImGui_ImplGlfw_ScrollCallback(w, dx, dy);
        g_scrollY += dy;
    });

    // Start on the Sources tab until there is something to show.
    app.activeTab = app.startTab >= 0 ? app.startTab : app.lib.Ready() ? 0 : static_cast<int>(items::Category::Count);
    std::snprintf(app.sources.databasePath, sizeof(app.sources.databasePath), "%s", app.lib.dbPath.c_str());

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) { ImGui_ImplGlfw_Sleep(16); continue; }
        double now = glfwGetTime();
        float dt = static_cast<float>(now - last);
        last = now;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        float scaleX = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
        float height = static_cast<float>(fbH) / scaleX;
        Sidebar(app, height);
        float viewLeft = app.sidebarWidth;
        ViewportToolbar(app, viewLeft, 0, static_cast<float>(fbW) / scaleX - viewLeft);
        GifDialog(app, std::min(fbW, fbH));
        ImGui::Render();

        if (app.gifPending) { // draws into the back buffer, which this frame then paints over
            app.gifPending = false;
            ApplyRotation(app);
            int size = std::min(app.lib.gif.size, std::min(fbW, fbH));
            app.gifMessageOk = ExportGif(app.lib, app.scene, app.gifPath, size, app.gifMessage);
            if (app.gifMessageOk) {
                std::string p = app.gifPath;
                size_t slash = p.find_last_of("/\\");
                if (slash != std::string::npos) app.lib.gif.lastDirectory = p.substr(0, slash);
                app.lib.SaveConfig();
            }
        }
        glViewport(0, 0, fbW, fbH);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        int vx = static_cast<int>(viewLeft * scaleX);
        int vw = std::max(1, fbW - vx);
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, 0, vw, fbH);
        if (app.gifPreview && !app.gifOpen) app.gifPreview = false; // closing the dialog ends the preview
        if (app.gifPreview && app.scene.hasModel) {
            // The export's own frames, in a square the size of the GIF, in the pane's bottom-right corner.
            const config::GifSettings& g = app.lib.gif;
            const int margin = static_cast<int>(16 * scaleX);
            int side = std::max(16, std::min(g.size, std::min(vw, fbH) - 2 * margin));
            int px = vx + vw - side - margin, py = margin;
            glClearColor(0.07f, 0.07f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(px, py, side, side);
            ViewOptions saved = app.scene.options;
            app.scene.options.grid = false;
            app.scene.options.autoRotate = false;
            app.scene.options.checkerboard = g.transparent;
            if (!g.transparent) {
                app.scene.options.background[0] = ((g.background >> 16) & 0xFF) / 255.0f;
                app.scene.options.background[1] = ((g.background >> 8) & 0xFF) / 255.0f;
                app.scene.options.background[2] = (g.background & 0xFF) / 255.0f;
            }
            int frameIndex = static_cast<int>((glfwGetTime() - app.gifPreviewStart) * std::max(g.fps, 1));
            app.scene.SetTurntableFrame(g, frameIndex);
            app.scene.Draw(app.lib, px, py, side, side, 0.0f);
            app.scene.spinDegrees = 0.0f;
            app.scene.options = saved;
        } else {
            app.scene.Draw(app.lib, vx, 0, vw, fbH, dt);
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, fbW, fbH);

        CameraInput(app, viewLeft);
        ApplyRotation(app);
        app.selectedName.clear();
        if (app.activeTab < static_cast<int>(items::Category::Count)) {
            const ui::ItemTabState& st = app.tabs[app.activeTab];
            const auto& list = app.lib.db.List(static_cast<items::Category>(app.activeTab));
            if (app.lib.dbLoaded && st.selected >= 0 && st.selected < static_cast<int>(list.size())) app.selectedName = list[st.selected].name;
        }
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        if (!app.screenshotPath.empty() && ++app.frame == 8) {
            std::vector<uint8_t> rgb(static_cast<size_t>(fbW) * fbH * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbW, fbH, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
            WriteBmp(app.screenshotPath, fbW, fbH, rgb);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        glfwSwapBuffers(window);
    }

    app.scene.ClearTextures();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // A GUI-subsystem program has no console: when started from one (cmd, PowerShell), print into it,
    // so --list/--resolve/--help show their output there.
    if (argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    App app;
    std::string material, texture;
    for (size_t i = 0; i < args.size();) { // options that can appear anywhere
        if ((args[i] == "--config" || args[i] == "--material" || args[i] == "--texture" || args[i] == "--screenshot") &&
            i + 1 < args.size()) {
            (args[i] == "--config" ? app.lib.configPath : args[i] == "--material" ? material
                                   : args[i] == "--texture" ? texture : app.screenshotPath) = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else {
            ++i;
        }
    }
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) { PrintHelp(); return 0; }
    app.lib.LoadConfig();
    if (args.empty()) return RunViewer(app);

    items::Category c;
    if (args.size() < 2 || !ParseCategory(args[1], c)) { PrintHelp(); return 1; }
    if (!app.lib.dbLoaded) {
        std::fprintf(stderr, "no items database: %s\n", app.lib.dbError.empty() ? "set one in the Sources tab first" : app.lib.dbError.c_str());
        return 1;
    }
    if (args[0] == "--open" && args.size() >= 3) {
        const items::Item* it = FindItem(app.lib, c, args[2]);
        if (!it) { std::fprintf(stderr, "no %s named \"%s\"\n", items::CategoryLabel(c), args[2].c_str()); return 1; }
        app.startTab = static_cast<int>(c);
        ui::SelectRow(app.tabs[static_cast<int>(c)], it->row);
        return RunViewer(app);
    }
    if (args[0] == "--gif" && args.size() >= 4) return RunGif(app.lib, c, args[2], args[3], material, texture);
    if (args[0] == "--list") return RunList(app.lib, c);
    if (args[0] == "--resolve" && args.size() >= 3) return RunResolve(app.lib, c, args[2], material);
    if (args[0] == "--render" && args.size() >= 4) return RunRender(app.lib, c, args[2], args[3], material, texture);
    PrintHelp();
    return 1;
}
