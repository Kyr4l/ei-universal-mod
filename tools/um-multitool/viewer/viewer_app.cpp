// The 3D Viewer tab of um-multitool (formerly the standalone um-modelviewer2): browses the items
// database by category and shows each item's ground/inventory figure with the texture the database
// points at (see item_resolve.hpp), with 45 degree rotation per tab and a GIF turntable export.
//
//   item_db.hpp       items.idb parser (all six blocks)
//   item_resolve.hpp  database row -> figure + candidate textures (no GL)
//   library.hpp       sources + database + name indexes (no GL)
//   scene.hpp         the GL viewport; gif_writer.hpp the GIF encoder
//   ui_*.hpp          the item tabs, and the sources (drawn by the GUI's Settings tab)
//
// The tab lives inside um-multitool's main window: DrawTab lays out a sidebar and a transparent
// viewport region during the ImGui frame, and RenderGl draws the 3D view into that region after
// ImGui::Render, underneath ImGui's own drawing.

#include "viewer_app.hpp"

#include <GLFW/glfw3.h>

#include "imgui.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "library.hpp"
#include "png_writer.hpp"
#include "uv_map.hpp"
#include <set>
#include <map>
#include "scene.hpp"
#include "item_texts.hpp"
#include "ui_items.hpp"
#include "ui_units.hpp"

namespace viewer {

struct App {
    explicit App(Library& shared) : lib(shared) {}
    Library& lib;               // the GUI's, shared with the other tabs
    Scene scene;
    int seenTexturesVersion = -1; // lib.texturesVersion the scene's GL textures belong to
    ui::ItemTabState tabs[static_cast<int>(items::Category::Count)];
    ui::UnitsTabState units;    // the Units tab (index kUnitsTab)
    int activeTab = 0;          // 0..4 = item categories, kUnitsTab = units
    int requestTab = -1;        // select this tab on the next frame
    float sidebarWidth = 460.0f;

    // The viewport region reserved by DrawTab this frame (ImGui coordinates), used by RenderGl.
    bool drawnThisFrame = false;
    ImVec2 viewportMin{0, 0}, viewportMax{0, 0};

    // GIF export dialog
    bool gifOpen = false;
    bool gifPending = false;     // export in the next RenderGl (the capture uses the back buffer)
    bool gifPreview = false;     // play the export's frames in the viewport
    double gifPreviewStart = 0.0;
    char gifPath[1024] = "";
    std::string gifMessage;
    bool gifMessageOk = true;
    std::string selectedName;    // the current tab's selected item, for the default file name
};

constexpr int kUnitsTab = static_cast<int>(items::Category::Count);

struct Context {
    explicit Context(Library& lib) : app(lib) {}
    App app;
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

void PrintCliHelpImpl() {
    std::printf(
        "um-multitool viewer - the 3D Viewer's command-line modes\n\n"
        "Usage:\n"
        
        "  um-multitool viewer --list <category>            List the category's items and what they resolve to.\n"
        "  um-multitool viewer --resolve <category> <item> [--material <name>]\n"
        "                                               Show the figure and every candidate texture of one item.\n"
        "  um-multitool viewer --render <category> <item> <out.bmp> [--material <name>] [--texture <name>]\n"
        "                                               Render one item to a BMP image (needs a display).\n"
        "  um-multitool gui --viewer <category> <item>    Open the GUI on that item in the 3D Viewer tab.\n"
        
        "  um-multitool viewer --gif <category> <item> <out.gif> [--material <name>] [--texture <name>]\n"
        "                                               Export a 360 degree turn as an animated GIF, with the GIF\n"
        "                                               settings of the viewer's export dialog (needs a display).\n"
        "  um-multitool viewer --uvdump <figure> [out.txt]\n"
        "                                               Every triangle of a figure (e.g. unhuma): \"part textureNumber\", then per\n"
        "                                               corner \"u v x y z\" (complection 0.5; figures face -Y, their left is +X).\n"
        "  um-multitool viewer --uvmap <figure> <out.png> [--texture <name|file>] [--size <pixels>]\n"
        "                                               Each part's UV region drawn in its own colour over a texture (a skin): the\n"
        "                                               guide for painting one. Parts: hd head, hr.NN hair, bd body, hp hips,\n"
        "                                               lh/rh 1-3 arms (3 = hand), ll/rl 1-3 legs (3 = foot).\n"
        "  um-multitool viewer --figure <figure> <out.png> [--texture <name>] [--yaw <deg>] [--pitch <deg>] [--size <px>] [--zoom <x>] [--category <c>]\n"
        "                                               Any figure (a map object too) to a PNG with a transparent background,\n"
        "                                               from that angle (default yaw 225, pitch 10: the front): logos, icons.\n"
        "                                               --category: stood up like in that item tab (e.g. questitems).\n"
        "  --config <file>                              Use another settings file than um-multitool-viewer.cfg.\n\n"
        "Categories: weapons, armors, quick, quest, loot. The sources (figures, textures, database)\n"
        "are the ones set in the GUI's Settings tab, saved in um-multitool.cfg.\n");
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
    if (lib.texts.AnyLoaded()) {
        texts::ItemText t = texts::Lookup(lib.texts, *it, r.material >= 0 ? r.materials[r.material] : nullptr);
        if (t.found) std::printf("text     [%s] %s\n         %s\n", t.key.c_str(), t.name.c_str(), t.description.c_str());
        else std::printf("text     none (looked for \"%s\")\n", t.key.c_str());
    }
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
    scene.SetOrientation(rot != lib.rotations.end() ? &rot->second : nullptr);
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

// Any figure (not only the database's items) to a PNG with a transparent background, from a chosen angle: for
// logos and icons. Drawn twice, on black then on white: the difference gives each pixel's coverage, so the soft
// edges stay clean.
static int RunFigure(Library& lib, const std::string& figure, const std::string& out, const std::string& texture,
                     float yaw, float pitch, int size, float zoom, const std::string& category) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    GLFWwindow* window = glfwCreateWindow(size, size, "figure", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    Scene scene;
    scene.LoadModel(lib, figure, true);
    scene.textureName = texture.empty() ? figure : texture;
    items::Category cat;
    if (!category.empty() && ParseCategory(category, cat)) { // stood up like in that tab of the viewer
        auto rot = lib.rotations.find(items::CategoryKey(cat));
        scene.SetOrientation(rot != lib.rotations.end() ? &rot->second : nullptr);
    }
    int status = 0;
    if (!scene.hasModel) {
        std::fprintf(stderr, "figure not loaded: %s\n", scene.modelError.empty() ? figure.c_str() : scene.modelError.c_str());
        status = 1;
    } else {
        scene.options.grid = false;
        scene.camera.yawDeg = yaw;
        scene.camera.pitchDeg = pitch;
        scene.camera.distance /= zoom;
        std::vector<uint8_t> shots[2];
        for (int pass = 0; pass < 2; ++pass) {
            const float bg = pass ? 1.0f : 0.0f;
            scene.options.background[0] = scene.options.background[1] = scene.options.background[2] = bg;
            scene.Draw(lib, 0, 0, size, size, 0.0f);
            glFinish();
            shots[pass].resize(static_cast<size_t>(size) * size * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, size, size, GL_RGB, GL_UNSIGNED_BYTE, shots[pass].data());
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const size_t from = (static_cast<size_t>(size - 1 - y) * size + x) * 3, to = (static_cast<size_t>(y) * size + x) * 4; // top row first
                int diff = 0;
                for (int k = 0; k < 3; ++k) diff += shots[1][from + k] - shots[0][from + k];
                const int a = std::clamp(255 - diff / 3, 0, 255);
                for (int k = 0; k < 3; ++k) rgba[to + k] = static_cast<uint8_t>(a ? std::min(255, shots[0][from + k] * 255 / a) : 0);
                rgba[to + 3] = static_cast<uint8_t>(a);
            }
        if (!png::Write(out, size, size, rgba)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); status = 1; }
        else std::printf("%s (texture %s) -> %s\n", scene.modelName.c_str(), scene.textureName.c_str(), out.c_str());
    }
    scene.ClearTextures();
    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

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
    auto rot = lib.rotations.find(items::CategoryKey(c)); // the same rotation as the viewer's tab
    scene.SetOrientation(rot != lib.rotations.end() ? &rot->second : nullptr);
    std::string message;
    bool ok = ExportGif(lib, scene, out, size, message);
    std::printf("%s\n", message.c_str());
    scene.ClearTextures();
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
}

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

// A rotation as angles about the world X, then Y, then Z axis (R = Rz * Ry * Rx), each in 0..359.
static void OrientationDegrees(const std::array<float, 4>& q, int out[3]) {
    fig::Quat o{q[0], q[1], q[2], q[3]};
    fig::Vec3 cx = fig::QuatRotate(o, {1, 0, 0}), cy = fig::QuatRotate(o, {0, 1, 0}), cz = fig::QuatRotate(o, {0, 0, 1}); // R's columns
    const float k = 180.0f / 3.14159265f;
    float x, y, z;
    if (std::fabs(cx.z) < 0.9999f) {
        y = -std::asin(cx.z);
        x = std::atan2(cy.z, cz.z);
        z = std::atan2(cx.y, cx.x);
    } else { // Y at +-90: fold X into Z
        y = cx.z < 0 ? 1.5707963f : -1.5707963f;
        x = 0.0f;
        z = std::atan2(-cy.x, cy.y);
    }
    const float a[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) out[i] = ((static_cast<int>(std::lround(a[i] * k)) % 360) + 360) % 360;
}

static void GifDialog(App& app, int maxSize) {
    if (!app.gifOpen) return;
    // Top-left of the viewport, out of the way of the preview (bottom-right).
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(app.viewportMin.x + 10.0f, app.viewportMin.y + 36.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export GIF", &app.gifOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    config::GifSettings& g = app.lib.gif;
    bool changed = false;
    ImGui::TextWrapped("A full 360 degree turn of %s, from the current camera angle and zoom.",
                       app.selectedName.empty() ? "the shown item" : app.selectedName.c_str());
    ImGui::Spacing();
    // Typed; the size is capped by the window, whose back buffer the frames are drawn in.
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::InputInt("Size (px)", &g.size, 16, 64);
    g.size = std::max(16, g.size);
    ImGui::SameLine();
    ImGui::TextDisabled("(max %d)", std::max(16, maxSize));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The largest square that fits the window; enlarge the window for bigger GIFs");
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::InputInt("Frames per second", &g.fps, 1, 5);
    g.fps = std::min(std::max(1, g.fps), 120);
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::SliderFloat("Speed (degrees/s)", &g.degreesPerSecond, 10.0f, 360.0f, "%.0f");
    // Straight views along the axes, so a spin about X or Y is seen square on. Sets the viewport's camera.
    ImGui::TextUnformatted("Camera from");
    struct View { const char* label; float yaw, pitch; const char* tip; };
    static const View views[] = {{"+X", 0, 0, "From the +X side, looking along X"}, {"-X", 180, 0, "From the -X side, looking along X"},
                                 {"+Y", 90, 0, "From the +Y side, looking along Y"}, {"-Y", -90, 0, "From the -Y side, looking along Y"},
                                 {"Top", -90, 90, "From above, looking down Z"}, {"Bottom", -90, -90, "From below, looking up Z"},
                                 {"Default", 45, 20, "The viewer's usual 3/4 view"}};
    for (const View& v : views) {
        ImGui::SameLine();
        if (ImGui::Button(v.label)) {
            app.scene.camera.yawDeg = v.yaw;
            app.scene.camera.pitchDeg = v.pitch;
            app.scene.options.autoRotate = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v.tip);
    }
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
    if (g.size > maxSize)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "The window fits %d px: the GIF will be %d x %d", maxSize, maxSize, maxSize);
    int frames = Scene::TurntableFrames(g);
    ImGui::TextDisabled("%d frames, %.1f s per turn", frames, frames / static_cast<float>(std::max(g.fps, 1)));
    if (g.fps > 50 || 100 % g.fps) ImGui::TextDisabled("GIF timing is in 1/100 s: plays at %.1f fps",
                                                      100.0f / std::max(2, static_cast<int>(std::lround(100.0 / g.fps))));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##gifpath", app.gifPath, sizeof(app.gifPath));
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("File...") && ui::PickSaveFile(app.gifPath, picked)) std::snprintf(app.gifPath, sizeof(app.gifPath), "%s", picked.c_str());
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

// The shown model gets the rotation of the tab it belongs to.
static void ApplyRotation(App& app) {
    if (app.activeTab == kUnitsTab) { app.scene.SetOrientation(nullptr); return; } // units stand on the grid
    if (app.activeTab >= static_cast<int>(items::Category::Count)) return;
    auto it = app.lib.rotations.find(items::CategoryKey(static_cast<items::Category>(app.activeTab)));
    app.scene.SetOrientation(it == app.lib.rotations.end() ? nullptr : &it->second);
}

// --------------------------------------------------------------------------
// The tab
// --------------------------------------------------------------------------

static void Toolbar(App& app) {
    ViewOptions& o = app.scene.options;
    ImGui::Checkbox("Textured", &o.textured);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &o.wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Lighting", &o.lighting);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &o.grid);
    ImGui::SameLine();
    ImGui::Checkbox("Turn", &o.autoRotate);
    ImGui::SameLine();
    ImGui::Checkbox("Atlas UVs", &o.atlasUvs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Item figures address a 256x256 texture atlas, not the texture itself.\n"
                                                  "Keep this on unless a custom figure maps its texture directly.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::ColorEdit3("##bg", o.background, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background colour");
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
    // Rotation of the current tab's models, remembered per tab in um-multitool-viewer.cfg. Each click
    // turns the model 45 degrees about the fixed world axis (the grid's X, Y, Z) - whatever turns came
    // before - so a button always does what it says. Right-click turns the other way.
    if (app.activeTab < static_cast<int>(items::Category::Count)) {
        const std::string key = items::CategoryKey(static_cast<items::Category>(app.activeTab));
        auto found = app.lib.rotations.find(key);
        const bool rotated = found != app.lib.rotations.end() && std::fabs(std::fabs(found->second[0]) - 1.0f) > 1e-6f;
        static const char* const axes[3] = {"X", "Y", "Z"};
        // Each button shows the turns clicked about its axis (a 3D orientation has several X/Y/Z readings, so
        // computing them back from it would show turns never clicked); older settings without the clicks: computed.
        int deg[3] = {0, 0, 0};
        auto clicks = app.lib.rotationClicks.find(key);
        if (clicks != app.lib.rotationClicks.end()) for (int a = 0; a < 3; ++a) deg[a] = clicks->second[a];
        else if (found != app.lib.rotations.end()) OrientationDegrees(found->second, deg);
        ImGui::SameLine();
        ImGui::TextDisabled(" Rotate");
        for (int a = 0; a < 3; ++a) {
            ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof(label), "%s %d##rot%d", axes[a], deg[a], a);
            ImGui::Button(label);
            int direction = ImGui::IsItemClicked(ImGuiMouseButton_Left) ? 1 : ImGui::IsItemClicked(ImGuiMouseButton_Right) ? -1 : 0;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Clicked: X %d, Y %d, Z %d degrees (each turn about the grid's fixed axis, in click order)\n"
                                  "Click: turn 45 degrees about the grid's %s axis; right-click: the other way", deg[0], deg[1], deg[2], axes[a]);
            if (direction) {
                std::array<float, 4> q = found != app.lib.rotations.end() ? found->second : std::array<float, 4>{1, 0, 0, 0};
                float half = direction * 3.14159265f / 8.0f; // 45 degrees, halved for the quaternion
                fig::Quat step{std::cos(half), a == 0 ? std::sin(half) : 0.0f, a == 1 ? std::sin(half) : 0.0f, a == 2 ? std::sin(half) : 0.0f};
                fig::Quat r = fig::QuatNormalize(fig::QuatMul(step, fig::Quat{q[0], q[1], q[2], q[3]})); // world axis: applied last
                app.lib.rotations[key] = {r.w, r.x, r.y, r.z};
                std::array<int, 3>& c = app.lib.rotationClicks.emplace(key, std::array<int, 3>{deg[0], deg[1], deg[2]}).first->second;
                c[a] = ((c[a] + direction * 45) % 360 + 360) % 360;
                app.lib.SaveConfig();
                found = app.lib.rotations.find(key);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!rotated);
        if (ImGui::Button("Reset##rot")) {
            app.lib.rotations.erase(key);
            app.lib.rotationClicks.erase(key);
            app.lib.SaveConfig();
            found = app.lib.rotations.end();
        }
        ImGui::EndDisabled();
    }
}

static void Sidebar(App& app, float width, float height) {
    // The host window is see-through on this tab (so the 3D view shows): give the sidebar its own background.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PanelBg());
    ImGui::BeginChild("##viewerSidebar", ImVec2(width, height), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##itemtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        const int requested = app.requestTab;
        app.requestTab = -1;
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c) {
            ImGuiTabItemFlags flags = requested == c ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(items::CategoryLabel(static_cast<items::Category>(c)), nullptr, flags)) {
                app.activeTab = c;
                if (app.lib.viewerTab != c && requested < 0) { app.lib.viewerTab = c; app.lib.SaveConfig(); } // remembered
                ImGui::BeginChild("##tab", ImVec2(0, 0));
                ui::ItemTab(app.lib, app.scene, static_cast<items::Category>(c), app.tabs[c]);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Units", nullptr, requested == kUnitsTab ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (app.activeTab != kUnitsTab) app.units.dirty = true; // the item tabs used the scene meanwhile
            app.activeTab = kUnitsTab;
            if (app.lib.viewerTab != kUnitsTab && requested < 0) { app.lib.viewerTab = kUnitsTab; app.lib.SaveConfig(); }
            ImGui::BeginChild("##tab", ImVec2(0, 0));
            ui::UnitsTab(app.lib, app.scene, app.units);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Orbit / pan / zoom over the viewport, with the Map Editor's mouse buttons (Settings: the wheel click
// orbits and the right button drags by default). A drag that starts over the view goes on until its
// button is let go, like the Map Editor's.
static void CameraInput(App& app, ImVec2 size) {
    ImGui::InvisibleButton("##viewport", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)), ImGuiButtonFlags_MouseButtonLeft);
    ImGuiIO& io = ImGui::GetIO();
    OrbitCamera& cam = app.scene.camera;
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    static bool held[5] = {};
    for (int b = 1; b < 5; ++b) {
        if (hovered && ImGui::IsMouseClicked(b)) held[b] = true;
        if (!ImGui::IsMouseDown(b)) held[b] = false;
    }
    if (held[app.lib.mapMouseOrbit]) {
        cam.Orbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
    } else if (held[app.lib.mapMousePan]) {
        float k = cam.distance * 0.0015f; // pan speed follows the zoom, so tiny items stay controllable
        cam.Pan(-io.MouseDelta.x * k, io.MouseDelta.y * k);
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        // Zoom by a ratio, not a fixed step: items range from a ring to a two-metre spear.
        cam.distance *= std::pow(0.87f, io.MouseWheel);
        cam.distance = std::max(0.01f, std::min(cam.distance, 100.0f));
    }
}

Context* Create(Library& lib) {
    Context* ctx = new Context(lib);
    if (lib.viewerTab >= 0 && lib.viewerTab <= kUnitsTab) ctx->app.requestTab = lib.viewerTab; // as last time
    return ctx;
}

void Destroy(Context* ctx) {
    if (!ctx) return;
    ctx->app.scene.ClearTextures();
    delete ctx;
}

bool OpenItem(Context* ctx, const std::string& category, const std::string& item, std::string& error, const std::string& skin, bool naked,
              const std::string& clip, float frame) {
    App& app = ctx->app;
    if (ui::units_detail::Lower(category) == "units") {
        const auto& ms = app.lib.unitsDb.monsters;
        for (size_t i = 0; i < ms.size(); ++i)
            if (ui::units_detail::Lower(ms[i].name) == ui::units_detail::Lower(item)) {
                app.requestTab = kUnitsTab;
                app.units.selected = static_cast<int>(i);
                ui::units_detail::ResetFromMonster(app.lib, app.units);
                app.units.customSkin = skin;
                if (naked) { app.units.weapons.clear(); app.units.armour.clear(); }
                if (!clip.empty()) { // an animation: playing, or held at that frame
                    app.units.clip = clip;
                    app.units.animTime = std::max(frame, 0.0f);
                    app.units.animPlaying = frame < 0;
                }
                app.units.dirty = app.units.frame = true;
                return true;
            }
        error = "no unit named \"" + item + "\"";
        return false;
    }
    items::Category c;
    if (!ParseCategory(category, c)) { error = "unknown category '" + category + "'"; return false; }
    if (!app.lib.dbLoaded) { error = "no items database loaded"; return false; }
    const items::Item* it = FindItem(app.lib, c, item);
    if (!it) { error = std::string("no ") + items::CategoryLabel(c) + " named \"" + item + "\""; return false; }
    app.requestTab = static_cast<int>(c);
    ui::SelectRow(app.tabs[static_cast<int>(c)], it->row);
    return true;
}

void DrawTab(Context* ctx) {
    App& app = ctx->app;
    // The item list and the view, side by side (the list on the right when set in Settings).
    ui::SplitLayout split{&app.sidebarWidth, app.lib.viewerSidebarRight};
    float panelWidth, viewWidth;
    split.Begin(panelWidth, viewWidth);
    ImVec2 min, size;
    auto view = [&] {
        ImGui::BeginChild("##viewerView", ImVec2(viewWidth, split.height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Toolbar(app);
        min = ImGui::GetCursorScreenPos();
        size = ImGui::GetContentRegionAvail();
        CameraInput(app, size);
        ImGui::EndChild();
    };
    if (split.panelRight) { view(); split.Bar(); Sidebar(app, panelWidth, split.height); }
    else { Sidebar(app, panelWidth, split.height); split.Bar(); view(); }
    app.viewportMin = min;
    app.viewportMax = ImVec2(min.x + size.x, min.y + size.y);
    app.drawnThisFrame = true;

    ApplyRotation(app);
    app.selectedName.clear();
    if (app.activeTab < static_cast<int>(items::Category::Count)) {
        const ui::ItemTabState& st = app.tabs[app.activeTab];
        const auto& list = app.lib.db.List(static_cast<items::Category>(app.activeTab));
        if (app.lib.dbLoaded && st.selected >= 0 && st.selected < static_cast<int>(list.size())) app.selectedName = list[st.selected].name;
    } else if (app.activeTab == kUnitsTab && app.units.selected >= 0 && app.units.selected < static_cast<int>(app.lib.unitsDb.monsters.size())) {
        app.selectedName = app.lib.unitsDb.monsters[static_cast<size_t>(app.units.selected)].name;
    }
    ImGuiIO& io = ImGui::GetIO();
    float scale = io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
    GifDialog(app, static_cast<int>(std::min(io.DisplaySize.x, io.DisplaySize.y) * scale));
}

void RenderGl(Context* ctx, int fbW, int fbH, float scale, float dt) {
    App& app = ctx->app;
    if (!app.drawnThisFrame) { // the tab is not shown
        app.gifPreview = false;
        return;
    }
    app.drawnThisFrame = false;
    if (scale <= 0) scale = 1.0f;
    if (app.seenTexturesVersion != app.lib.texturesVersion) { // the texture sources changed in Settings
        app.scene.ClearTextures();
        app.seenTexturesVersion = app.lib.texturesVersion;
    }

    if (app.gifPending) { // draws into the back buffer, which this frame then paints over
        app.gifPending = false;
        int size = std::min(app.lib.gif.size, std::min(fbW, fbH));
        app.gifMessageOk = ExportGif(app.lib, app.scene, app.gifPath, size, app.gifMessage);
        if (app.gifMessageOk) {
            std::string p = app.gifPath;
            size_t slash = p.find_last_of("/\\");
            if (slash != std::string::npos) app.lib.gif.lastDirectory = p.substr(0, slash);
            app.lib.SaveConfig();
        }
        glViewport(0, 0, fbW, fbH);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // The reserved region, in framebuffer pixels (GL counts rows from the bottom).
    int vx = static_cast<int>(app.viewportMin.x * scale);
    int vw = std::max(1, static_cast<int>((app.viewportMax.x - app.viewportMin.x) * scale));
    int vh = std::max(1, static_cast<int>((app.viewportMax.y - app.viewportMin.y) * scale));
    int vy = std::max(0, fbH - static_cast<int>(app.viewportMax.y * scale));
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);

    if (app.gifPreview && !app.gifOpen) app.gifPreview = false; // closing the dialog ends the preview
    if (app.gifPreview && app.scene.hasModel) {
        // The export's own frames, in a square the size of the GIF, in the viewport's bottom-right corner.
        const config::GifSettings& g = app.lib.gif;
        const int margin = static_cast<int>(16 * scale);
        int side = std::max(16, std::min(g.size, std::min(vw, vh) - 2 * margin));
        int px = vx + vw - side - margin, py = vy + margin;
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
        app.scene.Draw(app.lib, vx, vy, vw, vh, dt);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void PrintCliHelp() { PrintCliHelpImpl(); }

// ---- figure UVs (skin painting) ----------------------------------------------------------------------
// Every triangle's part, texture number, and per corner u v and the 3D position (complection 0.5).
static int RunUvDump(const Library& lib, const std::string& figure, const std::string& outPath) {
    LoadedModel m;
    if (!LoadNamedModel(lib.figures, figure, m)) { std::fprintf(stderr, "%s: %s\n", figure.c_str(), m.error.c_str()); return 1; }
    std::FILE* f = outPath.empty() ? stdout : std::fopen(outPath.c_str(), "w");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    const fig::Vec3 k{0.5f, 0.5f, 0.5f};
    for (const fig::ModelPart& p : m.model.parts) {
        const fig::FigureMesh& mesh = p.mesh;
        const fig::Vec3 off = fig::BlendComplection(p.accumulatedOffset, k);
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            std::fprintf(f, "%s %d", p.name.c_str(), mesh.textureNumber);
            for (int c = 0; c < 3; ++c) {
                const uint16_t idx = mesh.indices[i + c];
                if (idx >= mesh.vertexComponents.size()) { std::fprintf(f, " 0 0 0 0 0"); continue; }
                const fig::VertComponent& vc = mesh.vertexComponents[idx];
                const fig::Vec2 uv = vc.uvIndex < mesh.uvs.size() ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0};
                const fig::Vec3 pos = mesh.BlendedPosition(idx, k) + off;
                std::fprintf(f, " %g %g %g %g %g", uv.x, uv.y, pos.x, pos.y, pos.z);
            }
            std::fprintf(f, "\n");
        }
    }
    if (f != stdout) std::fclose(f);
    return 0;
}

// The parts' UV regions in colours over a texture (at its size) or on transparency, with a legend printed.
// Only the body: the bare parts (hd, bd, hp, lh1 ...: 3 letters at most) and the first hair.
static int RunUvMap(const Library& lib, const std::string& figure, const std::string& outPath, const std::string& texture, int size) {
    LoadedModel m;
    if (!LoadNamedModel(lib.figures, figure, m)) { std::fprintf(stderr, "%s: %s\n", figure.c_str(), m.error.c_str()); return 1; }
    mmp::Image tex;
    std::vector<uint8_t> bytes;
    std::string err;
    const bool haveTex = !texture.empty() && Scene::ReadTextureBytes(lib, texture, bytes) && DecodeTextureFile(bytes, tex, err);
    if (!texture.empty() && !haveTex) std::fprintf(stderr, "texture %s not read (%s): transparent background\n", texture.c_str(), err.c_str());
    std::set<std::string> body;
    for (const fig::ModelPart& p : m.model.parts) {
        const std::string n = ui::LowerCopy(p.name);
        if (n.find('.') == std::string::npos ? n.size() <= 3 : n == "hr.00") body.insert(n);
    }
    const uvmap::Result r = uvmap::Draw(m.model, body, haveTex ? &tex : nullptr, false, size > 0 ? size : 512);
    for (size_t i = 0; i < r.legend.size(); ++i) std::printf("%-8s colour %zu\n", r.legend[i].c_str(), i % 12);
    if (!png::Write(outPath, r.width, r.height, r.rgba)) { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    std::printf("written %s (%dx%d)\n", outPath.c_str(), r.width, r.height);
    return 0;
}

int RunCli(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc); // argv[0] is "viewer"
    Library lib;
    std::string material, texture;
    for (size_t i = 0; i < args.size();) { // options that can appear anywhere
        if ((args[i] == "--config" || args[i] == "--material" || args[i] == "--texture") && i + 1 < args.size()) {
            (args[i] == "--config" ? lib.configPath : args[i] == "--material" ? material : texture) = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else {
            ++i;
        }
    }
    if (args.empty() || args[0] == "--help" || args[0] == "-h") { PrintCliHelpImpl(); return args.empty() ? 1 : 0; }
    lib.LoadConfig();
    if (args[0] == "--uvdump" && args.size() >= 2) return RunUvDump(lib, args[1], args.size() >= 3 ? args[2] : "");
    if (args[0] == "--uvmap" && args.size() >= 3) {
        int size = 0;
        for (size_t i = 3; i + 1 < args.size(); ++i) if (args[i] == "--size") size = std::atoi(args[i + 1].c_str());
        return RunUvMap(lib, args[1], args[2], texture, size);
    }
    if (args[0] == "--figure" && args.size() >= 3) {
        float yaw = 225.0f, pitch = 10.0f, zoom = 1.0f;
        int size = 512;
        std::string category;
        for (size_t i = 3; i + 1 < args.size(); ++i) {
            if (args[i] == "--yaw") yaw = static_cast<float>(std::atof(args[i + 1].c_str()));
            else if (args[i] == "--pitch") pitch = static_cast<float>(std::atof(args[i + 1].c_str()));
            else if (args[i] == "--size") size = std::max(16, std::atoi(args[i + 1].c_str()));
            else if (args[i] == "--zoom") zoom = std::max(0.1f, static_cast<float>(std::atof(args[i + 1].c_str())));
            else if (args[i] == "--category") category = args[i + 1];
        }
        return RunFigure(lib, args[1], args[2], texture, yaw, pitch, size, zoom, category);
    }
    items::Category c;
    if (args.size() < 2 || !ParseCategory(args[1], c)) { PrintCliHelpImpl(); return 1; }
    if (!lib.dbLoaded) {
        std::fprintf(stderr, "no items database: %s\n", lib.dbError.empty() ? "set one in the GUI's Settings tab first" : lib.dbError.c_str());
        return 1;
    }
    if (args[0] == "--gif" && args.size() >= 4) return RunGif(lib, c, args[2], args[3], material, texture);
    if (args[0] == "--list") return RunList(lib, c);
    if (args[0] == "--resolve" && args.size() >= 3) return RunResolve(lib, c, args[2], material);
    if (args[0] == "--render" && args.size() >= 4) return RunRender(lib, c, args[2], args[3], material, texture);
    PrintCliHelpImpl();
    return 1;
}

} // namespace viewer
