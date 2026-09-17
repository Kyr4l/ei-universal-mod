/**
 * ============================================================================
 * um-modelviewer - Evil Islands 3D model/animation viewer
 * ============================================================================
 *
 * Renders .fig/.bon/.mod/.anm/.lnk figures (see docs/file-formats/figure-format.md)
 * with their .mmp textures and database-driven complection, either from loose
 * res-unpacked-style directories or packed .res archives. Toolkit: Dear ImGui
 * + GLFW + OpenGL2, matching um-multitool-gui's setup (see that tool's header
 * comment for why: no GL loader needed, most robust legacy-GL path under Wine,
 * GLFW auto-selects X11/Wayland on Linux with no code on our end).
 * ============================================================================
 */

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "camera.hpp"
#include "config.hpp"
#include "db_units.hpp"
#include "model_loader.hpp"
#include "mmp_texture.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

// Native file/folder pickers - same approach as um-multitool-gui (see that
// tool's gui_main.cpp): shell out to zenity/kdialog on Linux since neither
// GLFW nor Dear ImGui provide one, avoiding a Qt/GTK build dependency.
#ifndef _WIN32
static bool RunPickerCommand(const std::string& cmd, std::string& outPath) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    std::string result;
    while (fgets(buf, sizeof(buf), p)) result += buf;
    int status = pclose(p);
    if (status != 0) return false;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (result.empty()) return false;
    outPath = result;
    return true;
}
static bool HasCommand(const char* name) {
    std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}
#endif

static bool NativePickFile(std::string& outPath) {
#ifdef _WIN32
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) { outPath = buf; return true; }
    return false;
#else
    if (HasCommand("zenity")) return RunPickerCommand("zenity --file-selection 2>/dev/null", outPath);
    if (HasCommand("kdialog")) return RunPickerCommand("kdialog --getopenfilename 2>/dev/null", outPath);
    return false;
#endif
}

static bool NativePickFolder(std::string& outPath) {
#ifdef _WIN32
    char displayName[MAX_PATH] = "";
    BROWSEINFOA bi{};
    bi.pszDisplayName = displayName;
    bi.lpszTitle = "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    if (ok) { outPath = path; return true; }
    return false;
#else
    if (HasCommand("zenity")) return RunPickerCommand("zenity --file-selection --directory 2>/dev/null", outPath);
    if (HasCommand("kdialog")) return RunPickerCommand("kdialog --getexistingdirectory 2>/dev/null", outPath);
    return false;
#endif
}

static void PathPickerRow(const char* idSuffix, char* buf, size_t bufSize, bool isFolder) {
    ImGui::PushID(idSuffix);
    if (ImGui::Button(isFolder ? "Folder..." : "File...")) {
        std::string picked;
        bool ok = isFolder ? NativePickFolder(picked) : NativePickFile(picked);
        if (ok) std::snprintf(buf, bufSize, "%s", picked.c_str());
    }
    ImGui::PopID();
}

// ----------------------------------------------------------------------------
// GL texture upload
// ----------------------------------------------------------------------------

// Mipmapping needs glGenerateMipmap, which isn't guaranteed available without
// a GL loader library; skip it entirely rather than risk a missing-symbol
// crash under Wine/older drivers - fine for a model preview tool.
static GLuint UploadTextureNoMip(const mmp::Image& img) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    return tex;
}

// ----------------------------------------------------------------------------
// Application state
// ----------------------------------------------------------------------------

struct PartRenderData {
    std::vector<float> positions;  // 3 floats per vertex-component
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint16_t> indices;
    fig::Vec3 origin{}; // world-space anchor for this part (for skeleton view)
    fig::Vec3 parentOrigin{};
    bool hasParent = false;
};

struct AppState {
    // Layered sources: index 0 = base game (loaded first, lowest priority),
    // later layers (e.g. Universal-Mod, then Hard Lands) override earlier ones
    // for any file present in both - matching how the real mod chain resolves.
    LayeredAssetSource figureSource;
    LayeredAssetSource textureSource;
    db::UnitDatabase database;
    bool databaseLoaded = false;
    std::string databaseError;

    char figurePathBuf[512] = "";
    char texturePathBuf[512] = "";
    char databasePathBuf[512] = "";
    char modelNameBuf[128] = "";
    std::string statusText;

    LoadedModel loaded;
    std::vector<PartRenderData> renderParts;
    fig::Vec3 constitution{0.5f, 0.5f, 0.5f};

    std::map<std::string, GLuint> textureCache; // texture base name -> GL texture id (0 = load failed)
    std::vector<std::string> availableTextures;  // merged .mmp base names across all texture layers
    std::string primaryTexture;
    std::string secondaryTexture;

    // Composite models (e.g. unhuma.mod has 348 entries for only 16 hierarchy
    // slots) bake every equipment/hair variant into one archive - see
    // figure-format.md's "Body-Part-Name Convention for Equipment Variants".
    // Loading all of them at once overlaps a naked body with every piece of
    // armor and every hairstyle simultaneously, so each part's visibility is
    // tracked separately, defaulting to only the bare hierarchy-slot meshes.
    std::map<std::string, bool> partVisible; // key: lowercase part name

    bool showWireframe = false;
    bool showSkeleton = true;
    bool showMesh = true;

    std::string selectedClip;
    bool playing = false;
    float animTime = 0.0f;
    static constexpr float kFramesPerSecond = 15.0f; // no timing info in .anm; a reasonable fixed rate

    OrbitCamera camera;
    bool draggingOrbit = false;
    bool draggingPan = false;
};

// ----------------------------------------------------------------------------
// Mesh building (complection blend -> flat GL-friendly arrays)
// ----------------------------------------------------------------------------

static void BuildPartRenderData(const fig::ModelPart& part, const fig::Vec3& constitution, bool visible, PartRenderData& out) {
    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();

    // Origin is always computed (skeleton joints/parent references stay correct
    // even for a hidden part - e.g. a hidden "bd.armor03" that a visible child
    // hangs off of), but a hidden part contributes no mesh geometry to draw.
    if (!visible) {
        out.origin = fig::BlendComplection(part.accumulatedOffset, constitution);
        return;
    }

    fig::Vec3 offset = fig::BlendComplection(part.accumulatedOffset, constitution);
    out.origin = offset;

    const fig::FigureMesh& mesh = part.mesh;
    out.positions.reserve(mesh.vertexComponents.size() * 3);
    out.normals.reserve(mesh.vertexComponents.size() * 3);
    out.uvs.reserve(mesh.vertexComponents.size() * 2);

    for (size_t i = 0; i < mesh.vertexComponents.size(); ++i) {
        const auto& vc = mesh.vertexComponents[i];
        fig::Vec3 pos = mesh.BlendedPosition(i, constitution);
        pos = pos + offset;
        out.positions.push_back(pos.x);
        out.positions.push_back(pos.y);
        out.positions.push_back(pos.z);

        fig::Vec3 n = (vc.normalIndex < mesh.normals.size()) ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1};
        out.normals.push_back(n.x);
        out.normals.push_back(n.y);
        out.normals.push_back(n.z);

        fig::Vec2 uv = (vc.uvIndex < mesh.uvs.size()) ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0};
        out.uvs.push_back(uv.x);
        out.uvs.push_back(uv.y);
    }
    out.indices = mesh.indices;
}

static void RebuildRenderData(AppState& app) {
    app.renderParts.clear();
    if (!app.loaded.ok) return;
    app.renderParts.resize(app.loaded.model.parts.size());
    for (size_t i = 0; i < app.loaded.model.parts.size(); ++i) {
        const std::string& name = app.loaded.model.parts[i].name;
        std::string lower = res::Archive::ToLower(name);
        auto it = app.partVisible.find(lower);
        bool visible = (it != app.partVisible.end()) ? it->second : true; // default visible for parts not yet tracked (e.g. simple single-part figures)
        BuildPartRenderData(app.loaded.model.parts[i], app.constitution, visible, app.renderParts[i]);
    }
    // Resolve parent origins for the skeleton view (drawn as lines part-origin -> parent-origin).
    for (size_t i = 0; i < app.loaded.model.parts.size(); ++i) {
        const auto& part = app.loaded.model.parts[i];
        if (part.parentName.empty()) { app.renderParts[i].hasParent = false; continue; }
        int parentIdx = app.loaded.model.FindPartIndex(part.parentName);
        if (parentIdx < 0) { app.renderParts[i].hasParent = false; continue; }
        app.renderParts[i].hasParent = true;
        app.renderParts[i].parentOrigin = app.renderParts[static_cast<size_t>(parentIdx)].origin;
    }
}

// Best-effort animation posing: displaces each part's origin by the clip's
// position track (added on top of the static bon assembly offset) and leaves
// rotation for a future pass - see figure-format.md's ".anm" section for the
// open questions here (no reference implementation existed to verify against;
// this reproduces the documented rotation/position layout but the composition
// with the static per-part offset is this viewer's own interpretation).
static fig::Vec3 SampleAnimPosition(const fig::BoneTrack& track, float timeSeconds, float fps) {
    if (track.positions.empty()) return {};
    size_t frameCount = track.rotations.size();
    if (frameCount == 0) return track.positions[0];
    float frameF = timeSeconds * fps;
    int frame = static_cast<int>(std::fmod(frameF, static_cast<float>(frameCount)));
    if (frame < 0) frame += static_cast<int>(frameCount);
    return track.positions[static_cast<size_t>(frame)];
}

static void ApplyAnimationToRenderData(AppState& app) {
    if (app.selectedClip.empty()) return;
    auto it = app.loaded.animClips.find(app.selectedClip);
    if (it == app.loaded.animClips.end()) return;
    const fig::AnimClip& clip = it->second;

    for (size_t i = 0; i < app.loaded.model.parts.size(); ++i) {
        const std::string& partName = app.loaded.model.parts[i].name;
        std::string lower = res::Archive::ToLower(partName);
        auto trackIt = clip.bones.find(lower);
        if (trackIt == clip.bones.end()) continue;
        fig::Vec3 animPos = SampleAnimPosition(trackIt->second, app.animTime, AppState::kFramesPerSecond);

        PartRenderData& rd = app.renderParts[i];
        fig::Vec3 delta = animPos - fig::BlendComplection(app.loaded.model.parts[i].accumulatedOffset, app.constitution);
        for (size_t v = 0; v + 2 < rd.positions.size(); v += 3) {
            rd.positions[v + 0] += delta.x;
            rd.positions[v + 1] += delta.y;
            rd.positions[v + 2] += delta.z;
        }
        rd.origin = rd.origin + delta;
    }
    // Second pass to refresh parent-origin references now that origins moved.
    for (size_t i = 0; i < app.loaded.model.parts.size(); ++i) {
        const auto& part = app.loaded.model.parts[i];
        if (part.parentName.empty()) continue;
        int parentIdx = app.loaded.model.FindPartIndex(part.parentName);
        if (parentIdx < 0) continue;
        app.renderParts[i].parentOrigin = app.renderParts[static_cast<size_t>(parentIdx)].origin;
    }
}

// ----------------------------------------------------------------------------
// Loading actions
// ----------------------------------------------------------------------------

// The 16 canonical hierarchy slots a bare-named part can be (see
// figure-format.md); everything else in a composite .mod (numbered armor/hair
// variants like "bd.armor03"/"hr.00", weapon meshes like "baseaxe00") is an
// alternate/equipment mesh that should stay hidden until explicitly equipped.
// Whether `part` (or any ancestor up to the root, including itself) has a
// numbered-variant name like "bd.armor03" or "rh3.axe00". This is a
// structural rule rather than a hardcoded list of human body-slot names, so
// it generalizes to any creature's own hierarchy naming: every equipment/
// weapon mesh family observed (baseaxe*, bwparta*, crbow*...) hangs off a
// dotted node somewhere in its ancestor chain even when its own name has no
// dot (e.g. "baseaxe00" is a child of "rh3.axe00") - see figure-format.md's
// "Body-Part-Name Convention for Equipment Variants".
static bool IsEquipmentVariantOrDescendant(const fig::Model& model, const fig::ModelPart& part) {
    const fig::ModelPart* cur = &part;
    while (true) {
        if (cur->name.find('.') != std::string::npos) return true;
        if (cur->parentName.empty()) return false;
        int parentIdx = model.FindPartIndex(cur->parentName);
        if (parentIdx < 0) return false;
        cur = &model.parts[static_cast<size_t>(parentIdx)];
    }
}

static void ResetPartVisibilityToDefaults(AppState& app) {
    app.partVisible.clear();
    for (auto& part : app.loaded.model.parts) {
        std::string lower = res::Archive::ToLower(part.name);
        app.partVisible[lower] = !IsEquipmentVariantOrDescendant(app.loaded.model, part);
    }
}

static void LoadModelByName(AppState& app, const std::string& name) {
    if (!app.figureSource.AnyLoaded()) {
        app.statusText = "Load a figures source first (RES archive or res-unpacked-style directory).";
        return;
    }
    LoadedModel result;
    if (!LoadNamedModel(app.figureSource, name, result)) {
        app.statusText = "Load failed: " + result.error;
        app.loaded = LoadedModel{};
        app.renderParts.clear();
        return;
    }
    app.loaded = std::move(result);
    app.selectedClip.clear();
    if (!app.loaded.animClips.empty()) app.selectedClip = app.loaded.animClips.begin()->first;
    ResetPartVisibilityToDefaults(app);
    RebuildRenderData(app);
    app.statusText = "Loaded '" + name + "' (" + std::to_string(app.loaded.model.parts.size()) + " part(s), " +
                      std::to_string(app.loaded.animClips.size()) + " clip(s))";
}

static GLuint LoadOrGetTexture(AppState& app, const std::string& baseName) {
    if (baseName.empty()) return 0;
    auto it = app.textureCache.find(baseName);
    if (it != app.textureCache.end()) return it->second;
    if (!app.textureSource.AnyLoaded()) return 0;
    std::vector<uint8_t> bytes;
    if (!app.textureSource.ReadFile(baseName + ".mmp", bytes)) {
        app.textureCache[baseName] = 0;
        return 0;
    }
    mmp::Image img;
    std::string err;
    if (!mmp::Decode(bytes, img, err)) {
        app.textureCache[baseName] = 0;
        return 0;
    }
    GLuint tex = UploadTextureNoMip(img);
    app.textureCache[baseName] = tex;
    return tex;
}

// ----------------------------------------------------------------------------
// 3D rendering
// ----------------------------------------------------------------------------

static void Draw3DScene(AppState& app, int viewportW, int viewportH) {
    glViewport(0, 0, viewportW, viewportH);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    Mat4 proj = Mat4::Perspective(50.0f, viewportH > 0 ? static_cast<float>(viewportW) / viewportH : 1.0f, 0.05f, 200.0f);
    Mat4 view = app.camera.ViewMatrix();

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m);

    // Ground grid for scale/orientation reference.
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glColor3f(0.3f, 0.3f, 0.32f);
    glBegin(GL_LINES);
    for (int i = -10; i <= 10; ++i) {
        glVertex3f(static_cast<float>(i), -10.0f, 0.0f);
        glVertex3f(static_cast<float>(i), 10.0f, 0.0f);
        glVertex3f(-10.0f, static_cast<float>(i), 0.0f);
        glVertex3f(10.0f, static_cast<float>(i), 0.0f);
    }
    glEnd();

    GLuint primTex = LoadOrGetTexture(app, app.primaryTexture);

    if (app.showMesh) {
        glPolygonMode(GL_FRONT_AND_BACK, app.showWireframe ? GL_LINE : GL_FILL);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        if (primTex != 0 && !app.showWireframe) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, primTex);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glColor3f(1.0f, 1.0f, 1.0f);
        } else {
            glDisable(GL_TEXTURE_2D);
            glColor3f(0.75f, 0.72f, 0.65f);
        }

        for (auto& rd : app.renderParts) {
            if (rd.indices.empty()) continue;
            glVertexPointer(3, GL_FLOAT, 0, rd.positions.data());
            glNormalPointer(GL_FLOAT, 0, rd.normals.data());
            if (primTex != 0 && !app.showWireframe) {
                glTexCoordPointer(2, GL_FLOAT, 0, rd.uvs.data());
            }
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(rd.indices.size()), GL_UNSIGNED_SHORT, rd.indices.data());
        }

        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    if (app.showSkeleton) {
        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glColor3f(1.0f, 0.9f, 0.1f);
        glBegin(GL_LINES);
        for (auto& rd : app.renderParts) {
            if (!rd.hasParent) continue;
            glVertex3f(rd.origin.x, rd.origin.y, rd.origin.z);
            glVertex3f(rd.parentOrigin.x, rd.parentOrigin.y, rd.parentOrigin.z);
        }
        glEnd();
        glPointSize(6.0f);
        glColor3f(1.0f, 0.2f, 0.2f);
        glBegin(GL_POINTS);
        for (auto& rd : app.renderParts) {
            glVertex3f(rd.origin.x, rd.origin.y, rd.origin.z);
        }
        glEnd();
        glEnable(GL_DEPTH_TEST);
    }
}

// ----------------------------------------------------------------------------
// UI panels
// ----------------------------------------------------------------------------

// A green/red dot plus tooltip - the "is this actually loaded" indicator used
// throughout the sources panel and the model/texture status lines.
static void StatusDot(bool ok, const char* okTip, const char* failTip) {
    ImVec4 color = ok ? ImVec4(0.2f, 0.85f, 0.2f, 1.0f) : ImVec4(0.85f, 0.2f, 0.2f, 1.0f);
    ImGui::TextColored(color, "%s", ok ? "●" : "○"); // filled/hollow circle
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ok ? okTip : failTip);
}

// Renders one layered source's editor: existing layers (bottom = loaded first/
// lowest priority, top = highest priority, matching how override order reads
// naturally) with reorder/remove buttons and a load-status dot, plus an
// add-new-layer row. Returns true if the layer list changed (caller should
// refresh anything derived from it, e.g. cached textures or the browse list).
static bool DrawLayeredSourceEditor(const char* idPrefix, LayeredAssetSource& source,
                                     char* pathBuf, size_t pathBufSize, std::string& statusText) {
    bool changed = false;
    ImGui::PushID(idPrefix);

    // Highest-priority layer first for readability ("top of the stack wins").
    for (size_t i = source.layers.size(); i-- > 0;) {
        auto& layer = source.layers[i];
        ImGui::PushID(static_cast<int>(i));

        // Capture the row's full width BEFORE drawing anything on it: SameLine(x)
        // and SetCursorPosX(x) both take x relative to the window's content-region
        // left edge, not the current cursor - computing it from
        // GetContentRegionAvail() *after* already drawing the dot/text on this
        // line (as this used to) measures remaining space from wherever the
        // cursor ended up instead, which collapses toward zero (throwing the
        // buttons back on top of the text) whenever the path string is long,
        // e.g. right after adding a new layer.
        float rowWidth = ImGui::GetContentRegionAvail().x;
        constexpr float kButtonBlockWidth = 110.0f; // Up + Down + X + spacing

        StatusDot(layer.ok, "Loaded OK", layer.error.c_str());
        ImGui::SameLine();
        float textAreaWidth = rowWidth - kButtonBlockWidth - ImGui::GetCursorPosX();
        // Clip long paths to a fixed-width child instead of drawing raw text,
        // so an overlong path can never visually collide with the buttons -
        // it scrolls/clips within its own region instead. Full path on hover.
        ImGui::BeginChild("##pathclip", ImVec2(std::max(textAreaWidth, 20.0f), ImGui::GetTextLineHeight()), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(layer.path.c_str());
        ImGui::EndChild();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer.path.c_str());

        ImGui::SameLine();
        ImGui::SetCursorPosX(rowWidth - kButtonBlockWidth);
        ImGui::BeginDisabled(i + 1 == source.layers.size());
        if (ImGui::SmallButton("Up")) { source.MoveLayerUp(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Down")) { source.MoveLayerDown(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) { source.RemoveLayer(i); changed = true; }
        ImGui::PopID();
    }
    if (source.layers.empty()) ImGui::TextDisabled("(no sources loaded - add the base game first, then mods on top)");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##addpath", pathBuf, pathBufSize);
    PathPickerRow("file", pathBuf, pathBufSize, false);
    ImGui::SameLine();
    PathPickerRow("folder", pathBuf, pathBufSize, true);
    ImGui::SameLine();
    if (ImGui::Button("Add layer") && pathBuf[0] != '\0') {
        bool ok = source.AddLayer(pathBuf);
        statusText = ok ? (std::string("Added source: ") + pathBuf) : (std::string("Failed to add source: ") + source.layers.back().error);
        pathBuf[0] = '\0';
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

// Persists the currently loaded source paths so they're remembered next launch.
static void SaveSourceConfig(const AppState& app) {
    config::Config cfg;
    for (auto& layer : app.figureSource.layers) cfg.figureLayers.push_back(layer.path);
    for (auto& layer : app.textureSource.layers) cfg.textureLayers.push_back(layer.path);
    cfg.databasePath = app.databasePathBuf;
    config::Save(cfg);
}

static void DrawSourcesPanel(AppState& app) {
    ImGui::Text("Figures sources (load order: base game first, mods on top override it):");
    if (DrawLayeredSourceEditor("figsrc", app.figureSource, app.figurePathBuf, sizeof(app.figurePathBuf), app.statusText)) {
        // Nothing cached needs invalidating for figures - each Load call re-resolves layers fresh.
        SaveSourceConfig(app);
    }

    ImGui::Separator();
    ImGui::Text("Textures sources (same layering: base game first, mods on top):");
    if (DrawLayeredSourceEditor("texsrc", app.textureSource, app.texturePathBuf, sizeof(app.texturePathBuf), app.statusText)) {
        app.textureCache.clear();
        app.availableTextures = app.textureSource.ListBaseNames({".mmp"});
        SaveSourceConfig(app);
    }

    ImGui::Separator();
    ImGui::Text("Database (databaselmp.res or databaselmp.xlsx):");
    StatusDot(app.databaseLoaded, "Loaded OK", app.databaseError.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##dbpath", app.databasePathBuf, sizeof(app.databasePathBuf));
    PathPickerRow("dbfile", app.databasePathBuf, sizeof(app.databasePathBuf), false);
    ImGui::SameLine();
    if (ImGui::Button("Load##db")) {
        std::string path = app.databasePathBuf;
        std::string err;
        db::UnitDatabase newDb;
        bool ok;
        if (path.size() > 5 && path.substr(path.size() - 5) == ".xlsx") {
            ok = db::LoadUnitsFromXlsx(path, newDb, err);
        } else {
            std::ifstream f(path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            ok = db::LoadUnitsFromRes(bytes, newDb, err);
        }
        app.databaseLoaded = ok;
        app.databaseError = err;
        if (ok) {
            app.database = std::move(newDb);
            app.statusText = "Database loaded: " + std::to_string(app.database.raceModels.size()) + " race models, " +
                              std::to_string(app.database.monsters.size()) + " monsters";
            SaveSourceConfig(app);
        } else {
            app.statusText = "Database load failed: " + err;
        }
    }

    ImGui::Separator();
    ImGui::Text("Load figure by name directly:");
    StatusDot(app.loaded.ok, "Model loaded OK", app.loaded.error.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##modelname", app.modelNameBuf, sizeof(app.modelNameBuf));
    ImGui::SameLine();
    if (ImGui::Button("Load##model")) LoadModelByName(app, app.modelNameBuf);
}

static void DrawUnitListPanel(AppState& app) {
    if (!app.databaseLoaded) {
        ImGui::TextDisabled("Load a database to browse units.");
        return;
    }
    static char filter[128] = "";
    ImGui::InputText("Filter", filter, sizeof(filter));
    std::string filterLower = filter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    ImGui::BeginChild("##unitlist", ImVec2(0, 220), ImGuiChildFlags_Borders);
    for (auto& m : app.database.monsters) {
        std::string lowerName = m.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (!filterLower.empty() && lowerName.find(filterLower) == std::string::npos) continue;
        if (ImGui::Selectable(m.name.c_str())) {
            const db::RaceModel* race = app.database.FindRaceModel(m.baseRace);
            if (race) {
                app.constitution = {m.complectionX, m.complectionY, m.complectionZ};
                LoadModelByName(app, race->maskName);
                app.primaryTexture = race->primaryTextures.empty() ? "" : race->primaryTextures[0];
                app.secondaryTexture = race->secondaryTextures.empty() ? "" : race->secondaryTextures[0];
            } else {
                app.statusText = "No RaceModel found for base race '" + m.baseRace + "'";
            }
        }
    }
    ImGui::EndChild();
}

// Dropdown of every .mmp base name found across the loaded texture layers,
// with a text filter (there can be hundreds) and a load-status dot showing
// whether the currently selected name actually decodes to a usable texture.
static void DrawTextureSelector(AppState& app, const char* label, std::string& selected) {
    ImGui::PushID(label);
    GLuint tex = LoadOrGetTexture(app, selected);
    StatusDot(!selected.empty() && tex != 0,
              "Texture found and decoded", selected.empty() ? "No texture selected" : "Not found / failed to decode");
    ImGui::SameLine();
    if (ImGui::BeginCombo(label, selected.empty() ? "(none)" : selected.c_str())) {
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "filter...", filter, sizeof(filter));
        std::string filterLower = filter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
        if (ImGui::Selectable("(none)", selected.empty())) selected.clear();
        for (auto& name : app.availableTextures) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (!filterLower.empty() && lower.find(filterLower) == std::string::npos) continue;
            if (ImGui::Selectable(name.c_str(), name == selected)) selected = name;
        }
        if (app.availableTextures.empty()) ImGui::TextDisabled("(load a textures source to browse)");
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

// Composite models bake every equipment/hair variant into one archive (see
// figure-format.md's "Body-Part-Name Convention for Equipment Variants") -
// this lets the user show/hide individual mesh parts, since there's no
// database-driven "equip this specific item" mapping figured out yet (the
// exact field that selects a numbered variant like "bd.armor03" for a given
// database item wasn't conclusively identified during investigation).
static void DrawPartsPanel(AppState& app) {
    if (!app.loaded.ok) {
        ImGui::TextDisabled("No model loaded.");
        return;
    }
    if (!app.loaded.model.isComposite) {
        ImGui::TextDisabled("Simple (single-mesh) figure - no separate parts to toggle.");
        return;
    }

    ImGui::TextWrapped("Composite models bundle every equipment/hair variant into one file; "
                        "toggle which meshes are visible. Body slots are shown by default.");

    if (ImGui::Button("Show All")) {
        for (auto& kv : app.partVisible) kv.second = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All")) {
        for (auto& kv : app.partVisible) kv.second = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Body Only (default)")) {
        ResetPartVisibilityToDefaults(app);
    }

    static char partFilter[64] = "";
    ImGui::InputTextWithHint("##partfilter", "filter parts...", partFilter, sizeof(partFilter));
    std::string filterLower = partFilter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    ImGui::BeginChild("##partlist", ImVec2(0, 260), ImGuiChildFlags_Borders);
    for (auto& part : app.loaded.model.parts) {
        std::string lower = res::Archive::ToLower(part.name);
        if (!filterLower.empty() && lower.find(filterLower) == std::string::npos) continue;
        bool visible = app.partVisible.count(lower) ? app.partVisible[lower] : true;
        if (ImGui::Checkbox(part.name.c_str(), &visible)) {
            app.partVisible[lower] = visible;
        }
    }
    ImGui::EndChild();
}

static void DrawModelControlsPanel(AppState& app) {
    if (!app.loaded.ok) {
        ImGui::TextDisabled("No model loaded.");
        return;
    }
    bool changed = false;
    changed |= ImGui::SliderFloat("Strength", &app.constitution.x, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Dexterity", &app.constitution.y, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Height", &app.constitution.z, 0.0f, 1.0f);
    if (changed) RebuildRenderData(app);

    ImGui::Checkbox("Mesh", &app.showMesh);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &app.showWireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &app.showSkeleton);

    ImGui::Separator();
    ImGui::Text("Textures:");
    DrawTextureSelector(app, "Primary", app.primaryTexture);
    DrawTextureSelector(app, "Secondary", app.secondaryTexture);

    if (!app.loaded.animClips.empty()) {
        ImGui::Separator();
        ImGui::Text("Animation:");
        if (ImGui::BeginCombo("Clip", app.selectedClip.c_str())) {
            for (auto& kv : app.loaded.animClips) {
                bool isSel = kv.first == app.selectedClip;
                if (ImGui::Selectable(kv.first.c_str(), isSel)) { app.selectedClip = kv.first; app.animTime = 0.0f; }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Play", &app.playing);
        ImGui::SameLine();
        ImGui::SliderFloat("Time", &app.animTime, 0.0f, 10.0f);
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

int main(int, char**) {
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
    });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "um-modelviewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    AppState app;

    // Restore remembered source paths from the last session, if any.
    {
        config::Config cfg = config::Load();
        for (auto& p : cfg.figureLayers) app.figureSource.AddLayer(p);
        for (auto& p : cfg.textureLayers) app.textureSource.AddLayer(p);
        if (!app.textureSource.layers.empty()) app.availableTextures = app.textureSource.ListBaseNames({".mmp"});
        if (!cfg.databasePath.empty()) {
            std::snprintf(app.databasePathBuf, sizeof(app.databasePathBuf), "%s", cfg.databasePath.c_str());
            std::string err;
            db::UnitDatabase newDb;
            bool ok = (cfg.databasePath.size() > 5 && cfg.databasePath.substr(cfg.databasePath.size() - 5) == ".xlsx")
                          ? db::LoadUnitsFromXlsx(cfg.databasePath, newDb, err)
                          : [&] {
                                std::ifstream f(cfg.databasePath, std::ios::binary);
                                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                                return db::LoadUnitsFromRes(bytes, newDb, err);
                            }();
            app.databaseLoaded = ok;
            app.databaseError = err;
            if (ok) app.database = std::move(newDb);
        }
        if (!cfg.figureLayers.empty() || !cfg.textureLayers.empty()) {
            app.statusText = "Restored " + std::to_string(cfg.figureLayers.size()) + " figure layer(s), " +
                              std::to_string(cfg.textureLayers.size()) + " texture layer(s) from last session.";
        }
    }

    double lastTime = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(16);
            continue;
        }

        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        if (app.playing) app.animTime += dt;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(340, static_cast<float>(fbH) / io.DisplayFramebufferScale.y));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen)) DrawSourcesPanel(app);
        if (ImGui::CollapsingHeader("Units", ImGuiTreeNodeFlags_DefaultOpen)) DrawUnitListPanel(app);
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) DrawModelControlsPanel(app);
        if (ImGui::CollapsingHeader("Parts", ImGuiTreeNodeFlags_DefaultOpen)) DrawPartsPanel(app);
        ImGui::Separator();
        ImGui::TextWrapped("%s", app.statusText.c_str());
        ImGui::End();

        // Bottom toolbar over the 3D viewport - a static reminder of the mouse
        // controls, since they're not otherwise discoverable in a viewer with
        // no menu bar.
        {
            const float barHeight = 28.0f;
            ImGui::SetNextWindowPos(ImVec2(340, static_cast<float>(fbH) / io.DisplayFramebufferScale.y - barHeight));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fbW) / io.DisplayFramebufferScale.x - 340, barHeight));
            ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
            ImGui::TextUnformatted("Left-drag: Orbit    Right-drag: Pan    Scroll: Zoom");
            ImGui::End();
        }

        // Prepare (but don't render) animation-adjusted geometry before the 3D pass.
        RebuildRenderData(app);
        if (app.playing || !app.selectedClip.empty()) ApplyAnimationToRenderData(app);

        ImGui::Render();

        int viewportX = static_cast<int>(340 * io.DisplayFramebufferScale.x);
        int viewportW = std::max(1, fbW - viewportX);
        int viewportH = fbH;

        glViewport(0, 0, fbW, fbH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_SCISSOR_TEST);
        glScissor(viewportX, 0, viewportW, viewportH);
        glViewport(viewportX, 0, viewportW, viewportH);
        Draw3DScene(app, viewportW, viewportH);
        glDisable(GL_SCISSOR_TEST);

        // Camera controls: drag with the 3D pane hovered and no ImGui widget active.
        bool inViewport = io.MousePos.x >= static_cast<float>(viewportX) / io.DisplayFramebufferScale.x;
        if (!io.WantCaptureMouse || app.draggingOrbit || app.draggingPan) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inViewport) app.draggingOrbit = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && inViewport) app.draggingPan = true;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) app.draggingOrbit = false;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) app.draggingPan = false;
            if (app.draggingOrbit) app.camera.Orbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
            if (app.draggingPan) app.camera.Pan(-io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
        }
        // Zoom is intentionally not gated behind !io.WantCaptureMouse like the
        // drag controls above: a stray hover/active state elsewhere (e.g. a
        // combo box that was left open) shouldn't block scrolling the 3D view
        // when the cursor is unambiguously positioned over the viewport itself.
        if (inViewport && io.MouseWheel != 0.0f) {
            app.camera.Zoom(io.MouseWheel * 0.3f);
        }

        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
