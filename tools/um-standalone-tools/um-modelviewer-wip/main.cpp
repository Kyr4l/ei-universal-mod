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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "camera.hpp"
#include "config.hpp"
#include "db_units.hpp"
#include "db_items.hpp"
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
    bool visible = false; // mirrors the "visible" argument BuildPartRenderData was called with
    int32_t textureNumber = 0; // from the part's own .fig header - 0=primary, 1=secondary texture slot
    std::string partNameLower; // for looking up AppState::variantTexture per-part overrides
};

// Raw GLFW scroll accumulator - see the scroll callback installed in main() for why
// this exists alongside io.MouseWheel.
static double g_rawScrollY = 0.0;

struct AppState {
    // Layered sources: index 0 = base game (loaded first, lowest priority),
    // later layers (e.g. Universal-Mod, then Hard Lands) override earlier ones
    // for any file present in both - matching how the real mod chain resolves.
    LayeredAssetSource figureSource;
    LayeredAssetSource textureSource;
    db::UnitDatabase database;
    bool databaseLoaded = false;
    db::ItemDatabase itemDatabase; // Materials/Weapons/Armors - lives in the same res/xlsx as `database` above
    bool itemDatabaseLoaded = false;
    std::string currentMaskName; // base name the currently-loaded model was loaded under, e.g. "unorfe"

    // Manual weapon editor state (see DrawWeaponEditorPanel) - independent of
    // whatever a selected Monster record's own EquipmentWeapon fields say, so
    // picking a unit and then experimenting with the dropdowns doesn't require
    // editing the database.
    std::string weaponEditorType;      // e.g. "axe" - which Weapons.Type is being browsed
    std::string weaponEditorBlueprint; // Weapons.Name
    std::string weaponEditorMaterial;  // Materials.Name

    // Manual armor editor state (see DrawArmorEditorPanel) - one independent
    // Blueprint+Material pick per armor Type (helm/plate/shirt/pants/boots/
    // gloves/leggings), since unlike a held weapon, several armor pieces are
    // worn at once. Keyed by Armors.Type.
    std::map<std::string, std::string> armorEditorBlueprint;
    std::map<std::string, std::string> armorEditorMaterial;

    std::string databaseError;

    char figurePathBuf[512] = "";
    char texturePathBuf[512] = "";
    char databasePathBuf[512] = "";
    char modelNameBuf[128] = "";
    std::string statusText;
    std::vector<std::string> lastEquipLog; // what ApplyMonsterEquipment did/skipped for the last unit picked from the list

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
    std::map<std::string, bool> partVisible; // key: lowercase base (non-variant) part name

    // Each dotted-name equipment/hair/weapon family (e.g. "hd.armor01".."hd.armor16",
    // grouped under prefix "hd") is a mutually-exclusive slot: at most one variant
    // in a group is ever actually equipped at once in-game. Key: lowercase prefix
    // (text before the first '.'), value: the currently selected full part name
    // (empty = none equipped, matching the default of showing the bare body only).
    std::map<std::string, std::string> selectedVariant;

    // Per-part texture override (key: lowercase part name, e.g. "rh3.axe01" or
    // "bd.armor12") for equipped weapons/armor, which have their own distinct
    // "redress" texture per (race, item type, TTI, material) - completely
    // separate from the body's primary/secondary skin textures. See
    // ResolveWeaponVariant/ResolveArmorVariant. Empty/missing = fall back to
    // the part's own mesh.textureNumber-selected primary/secondary slot.
    std::map<std::string, std::string> variantTexture;

    bool showWireframe = false;
    bool showSkeleton = false;
    bool skeletonVisibleOnly = true; // hide bones for parts that aren't currently rendered (e.g. unequipped weapon sockets)
    bool showMesh = true;

    std::string selectedClip;
    bool playing = false;
    float animTime = 0.0f;
    static constexpr float kFramesPerSecond = 15.0f; // no timing info in .anm; a reasonable fixed rate

    OrbitCamera camera;
    bool draggingOrbit = false;
    bool draggingPan = false;

    float sidebarWidth = 340.0f; // user-resizable via the Controls window's right edge
};

// ----------------------------------------------------------------------------
// Mesh building (complection blend -> flat GL-friendly arrays)
// ----------------------------------------------------------------------------

static void BuildPartRenderData(const fig::ModelPart& part, const fig::Vec3& constitution, bool visible, PartRenderData& out) {
    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();
    out.visible = visible;
    out.textureNumber = part.mesh.textureNumber;
    out.partNameLower = res::Archive::ToLower(part.name);

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

static bool IsPartVisible(const AppState& app, const fig::ModelPart& part);

static void RebuildRenderData(AppState& app) {
    app.renderParts.clear();
    if (!app.loaded.ok) return;
    app.renderParts.resize(app.loaded.model.parts.size());
    for (size_t i = 0; i < app.loaded.model.parts.size(); ++i) {
        bool visible = IsPartVisible(app, app.loaded.model.parts[i]);
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

// Samples a bone track at a given time, linearly blending between the
// surrounding frames (slerp for rotation) and wrapping around the clip's
// length. positions has one MORE entry than rotations (see figure-format.md's
// ".anm" section) - f1 is kept within [0, frameCount-1] so the unconfirmed
// trailing extra position is never sampled, per that section's own advice.
static fig::Quat SampleAnimRotation(const fig::BoneTrack& track, float timeSeconds, float fps) {
    if (track.rotations.empty()) return {1, 0, 0, 0};
    size_t frameCount = track.rotations.size();
    float frameF = timeSeconds * fps;
    float wrapped = std::fmod(frameF, static_cast<float>(frameCount));
    if (wrapped < 0) wrapped += static_cast<float>(frameCount);
    size_t f0 = static_cast<size_t>(wrapped);
    size_t f1 = (f0 + 1) % frameCount;
    return fig::QuatSlerp(track.rotations[f0], track.rotations[f1], wrapped - static_cast<float>(f0));
}

static fig::Vec3 SampleAnimPosition(const fig::BoneTrack& track, float timeSeconds, float fps) {
    if (track.positions.empty()) return {};
    size_t frameCount = track.rotations.size();
    if (frameCount == 0) return track.positions[0];
    float frameF = timeSeconds * fps;
    float wrapped = std::fmod(frameF, static_cast<float>(frameCount));
    if (wrapped < 0) wrapped += static_cast<float>(frameCount);
    size_t f0 = static_cast<size_t>(wrapped);
    size_t f1 = (f0 + 1) % frameCount;
    return fig::Lerp(track.positions[f0], track.positions[f1], wrapped - static_cast<float>(f0));
}

// Poses the model for the current clip/time by composing each bone's local
// rotation+position (from its own track, or held rigid at its static bind
// offset if this clip has no track for it) down the hierarchy from the root -
// animation REPLACES the static per-part offset while playing rather than
// adding to it (see figure-format.md's ".anm" section). No reference
// implementation exists for .anm to verify the exact local/world convention
// against, so this uses the standard scene-graph composition
// (worldRot = parentWorldRot * localRot, worldPos = parentWorldPos +
// parentWorldRot * localPos); it is this viewer's best-effort interpretation.
static void ApplyAnimationToRenderData(AppState& app) {
    if (app.selectedClip.empty()) return;
    auto it = app.loaded.animClips.find(app.selectedClip);
    if (it == app.loaded.animClips.end()) return;
    const fig::AnimClip& clip = it->second;
    const auto& parts = app.loaded.model.parts;
    const size_t n = parts.size();

    std::vector<fig::Quat> worldRot(n, fig::Quat{1, 0, 0, 0});
    std::vector<fig::Vec3> worldPos(n);
    std::vector<bool> resolved(n, false);
    bool progress = true;
    size_t resolvedCount = 0;
    while (progress && resolvedCount < n) {
        progress = false;
        for (size_t i = 0; i < n; ++i) {
            if (resolved[i]) continue;
            const auto& part = parts[i];
            int parentIdx = part.parentName.empty() ? -1 : app.loaded.model.FindPartIndex(part.parentName);
            if (parentIdx >= 0 && !resolved[static_cast<size_t>(parentIdx)]) continue;

            std::string lower = res::Archive::ToLower(part.name);
            auto trackIt = clip.bones.find(lower);
            fig::Quat localRot{1, 0, 0, 0};
            fig::Vec3 localPos;
            if (trackIt != clip.bones.end() && !trackIt->second.rotations.empty()) {
                localRot = SampleAnimRotation(trackIt->second, app.animTime, AppState::kFramesPerSecond);
                localPos = SampleAnimPosition(trackIt->second, app.animTime, AppState::kFramesPerSecond);
            } else {
                // No track for this part in this clip - hold it rigid at its
                // static bind-pose local offset (own contribution only, not
                // yet accumulated with its parent's).
                localPos = fig::BlendComplection(part.offset, app.constitution);
            }

            if (parentIdx >= 0) {
                worldRot[i] = fig::QuatMul(worldRot[static_cast<size_t>(parentIdx)], localRot);
                worldPos[i] = worldPos[static_cast<size_t>(parentIdx)] +
                              fig::QuatRotate(worldRot[static_cast<size_t>(parentIdx)], localPos);
            } else {
                worldRot[i] = localRot;
                worldPos[i] = localPos;
            }
            resolved[i] = true;
            progress = true;
        }
        resolvedCount = static_cast<size_t>(std::count(resolved.begin(), resolved.end(), true));
    }

    for (size_t i = 0; i < n; ++i) {
        PartRenderData& rd = app.renderParts[i];
        fig::Vec3 bindOffset = fig::BlendComplection(parts[i].accumulatedOffset, app.constitution);
        for (size_t v = 0; v + 2 < rd.positions.size(); v += 3) {
            fig::Vec3 local{rd.positions[v] - bindOffset.x, rd.positions[v + 1] - bindOffset.y, rd.positions[v + 2] - bindOffset.z};
            fig::Vec3 world = fig::QuatRotate(worldRot[i], local) + worldPos[i];
            rd.positions[v] = world.x;
            rd.positions[v + 1] = world.y;
            rd.positions[v + 2] = world.z;
        }
        for (size_t vn = 0; vn + 2 < rd.normals.size(); vn += 3) {
            fig::Vec3 rotated = fig::QuatRotate(worldRot[i], fig::Vec3{rd.normals[vn], rd.normals[vn + 1], rd.normals[vn + 2]});
            rd.normals[vn] = rotated.x;
            rd.normals[vn + 1] = rotated.y;
            rd.normals[vn + 2] = rotated.z;
        }
        rd.origin = worldPos[i];
    }
    // Second pass to refresh parent-origin references now that origins moved.
    for (size_t i = 0; i < n; ++i) {
        const auto& part = parts[i];
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

// Prefix used to group a dotted variant name into its equipment slot, e.g.
// "hd.armor01" -> "hd", "rh3.axe00" -> "rh3". Two different prefixes can share
// a parent bone (e.g. "hr.*" hairstyles and "hd.armor*" helmets are both
// children of "hd") but are independent slots - a hairstyle and a helmet can
// both be equipped at once, so they must not share one selection.
//
// One deliberate exception: a hand slot's own weapon variants ("rh3.axe00",
// "rh3.sword00", "rh3.club01", "rh3.pike00", "rh3.crbow01main", "rh3.arrow00"
// - all mutually exclusive, you hold one thing at a time) share the plain
// "rh3" prefix and stay one group, but "rh3.armor01".."08" (forearm
// bracers) are pulled into their own "rh3:armor" group, since a bracer and a
// held weapon are independent and both can be equipped simultaneously - see
// unhuma.mod's real hierarchy in figure-format.md.
static std::string VariantGroupPrefix(const std::string& name) {
    auto dot = name.find('.');
    if (dot == std::string::npos) return {};
    std::string prefix = name.substr(0, dot);
    size_t afterDot = dot + 1;
    size_t alphaEnd = afterDot;
    while (alphaEnd < name.size() && std::isalpha(static_cast<unsigned char>(name[alphaEnd]))) ++alphaEnd;
    if (res::Archive::ToLower(name.substr(afterDot, alphaEnd - afterDot)) == "armor") return prefix + ":armor";
    return prefix;
}

// "quiver" (and its child "arrows") are dotless, canonical-looking parts (a
// direct child of "bd", not hanging off any dotted equipment node) that would
// otherwise default to always-visible like a real body part - but a quiver
// only makes sense while a bow or crossbow is actually equipped. Checked
// against both weapon slot groups since bow uses "lh3" while every other
// weapon type (crossbow included) uses "rh3".
static bool IsBowOrCrossbowEquipped(const AppState& app) {
    for (const char* group : {"rh3", "lh3"}) {
        auto it = app.selectedVariant.find(group);
        if (it == app.selectedVariant.end() || it->second.empty()) continue;
        std::string lower = res::Archive::ToLower(it->second);
        if (lower.find(".bwpartb") != std::string::npos || lower.find(".crbow") != std::string::npos) return true;
    }
    return false;
}

// Hair ("hr.NN") and a helm's own mesh ("hd.armorNN") are both hierarchy
// children of the same bare "hd" head slot (see figure-format.md's
// Body-Part-Name Convention section) and were rendering simultaneously,
// clipping through each other, whenever a unit had both a hairstyle and a
// helmet equipped. A real helmet fully replaces the visible hairstyle, so
// hide hair whenever the "hd:armor" group has anything selected.
static bool IsHelmEquipped(const AppState& app) {
    auto it = app.selectedVariant.find("hd:armor");
    return it != app.selectedVariant.end() && !it->second.empty();
}

// Resolves a part's actual visibility by walking its ancestor chain (self
// included): a base (non-dotted) node is subject to the manual partVisible
// toggle; a dotted variant node is visible only if it is the one currently
// selected for its slot group - this is what keeps unequipped variants (and
// everything hanging off them, e.g. an axe head hanging off "rh3.axe00") from
// rendering at all, rather than merely hiding the variant's own mesh while
// leaving its children visible.
// Most weapon variant families (axe/sword/dagger/pike/club) nest new numbered
// styles under a "00" node (e.g. "rh3.sword01" is a child of "rh3.sword00"),
// but that "00" node is a dead placeholder never referenced by any real
// database blueprint (no weapon TTI is ever 0) - it must NOT render its own
// mesh even when one of its numbered children is equipped.
//
// Bow and crossbow are structurally different: "lh3.bwpartb00" is the bow's
// actual body/grip mesh and "bwparta00"/"bwtetivaa00"/"bwtetivab00" are its
// string/limb pieces - a REQUIRED part of every bow variant's assembly, not a
// dead placeholder - and likewise "rh3.crbow01main" carries the shared
// part/string pieces every crossbow number reuses. These DO need to render
// alongside whichever numbered child is actually equipped.
static bool IsMultiPieceWeaponAncestor(const std::string& name) {
    auto dot = name.find('.');
    if (dot == std::string::npos) return false;
    size_t afterDot = dot + 1;
    size_t alphaEnd = afterDot;
    while (alphaEnd < name.size() && std::isalpha(static_cast<unsigned char>(name[alphaEnd]))) ++alphaEnd;
    std::string suffix = res::Archive::ToLower(name.substr(afterDot, alphaEnd - afterDot));
    return suffix == "bwpartb" || suffix == "crbow";
}

static bool IsPartVisible(const AppState& app, const fig::ModelPart& part) {
    const fig::ModelPart* cur = &part;
    std::string lastValidatedGroup; // no group validated yet
    bool hasValidatedAnyGroup = false;
    while (true) {
        std::string curLower = res::Archive::ToLower(cur->name);
        if (curLower == "quiver") {
            if (!IsBowOrCrossbowEquipped(app)) return false;
        } else if (curLower == "baserh3" || curLower == "baselh3") {
            // Default empty-hand filler mesh, parented directly under the bare
            // (dotless) hand socket rather than any weapon variant - so unlike
            // baseaxe01/basesword00/etc (each parented under its own matching
            // rh3.<type><NN> variant, and therefore already gated correctly by
            // the dotted-ancestor check below), this one has no weapon-variant
            // ancestor to gate it and previously stayed visible even with a
            // weapon equipped. Hide it whenever that hand has any weapon selected.
            std::string handGroup = curLower.substr(4); // "rh3" or "lh3"
            auto it = app.selectedVariant.find(handGroup);
            if (it != app.selectedVariant.end() && !it->second.empty()) return false;
        } else if (cur->name.find('.') != std::string::npos) {
            std::string group = VariantGroupPrefix(cur->name);
            if (group == "hr" && IsHelmEquipped(app)) return false;
            // Some variant chains nest more than one level deep under the same
            // group (e.g. sword: "rh3.sword01" is a child of "rh3.sword00"
            // itself, not a sibling under bare "rh3"; bow is the same via
            // "lh3.bwpartb00"). Once we've validated the FIRST (innermost)
            // dotted node hit for a given group along this walk, a FURTHER
            // ancestor sharing that SAME group is structurally part of the
            // exact same chosen item - not an independent choice - so it's
            // never re-validated by exact-name-equality (that would demand
            // e.g. "rh3.sword00" == the selection too, which it never can be).
            if (group != lastValidatedGroup || !hasValidatedAnyGroup) {
                auto it = app.selectedVariant.find(group);
                std::string selected = (it != app.selectedVariant.end()) ? it->second : std::string();
                if (selected.empty()) return false;
                bool matches;
                if (IsMultiPieceWeaponAncestor(cur->name)) {
                    // This node's own mesh is a required shared piece - visible
                    // if it's the selection itself OR one of the selection's
                    // ancestors (walking up from the selected leaf).
                    matches = false;
                    int selIdx = app.loaded.model.FindPartIndex(selected);
                    const fig::ModelPart* walk = (selIdx >= 0) ? &app.loaded.model.parts[static_cast<size_t>(selIdx)] : nullptr;
                    while (walk) {
                        if (res::Archive::ToLower(walk->name) == res::Archive::ToLower(cur->name)) { matches = true; break; }
                        if (walk->parentName.empty()) break;
                        int pIdx = app.loaded.model.FindPartIndex(walk->parentName);
                        if (pIdx < 0) break;
                        walk = &app.loaded.model.parts[static_cast<size_t>(pIdx)];
                    }
                } else {
                    matches = res::Archive::ToLower(selected) == res::Archive::ToLower(cur->name);
                }
                if (!matches) return false;
                lastValidatedGroup = group;
                hasValidatedAnyGroup = true;
            }
        } else if (cur == &part && curLower != "hd" &&
                   app.selectedVariant.count(curLower + ":armor") &&
                   !app.selectedVariant.at(curLower + ":armor").empty()) {
            // A dedicated-mesh armor variant (plate/leggings - see ArmorSlotMap)
            // fully replaces this bare body slot rather than overlaying a
            // cutout onto it (that's what the "no mesh" light-armor types use
            // variantTexture for instead); leaving the bare mesh visible
            // underneath made it visibly poke through the armor shell,
            // especially at the leg segments. "hd" is deliberately excluded:
            // a helm's own coverage/cutout shape relative to the head hasn't
            // been reported as having the same problem.
            //
            // Restricted to "cur == &part" (only when checking this bare
            // part's OWN visibility, not when it's merely an ancestor being
            // walked through for a different part): "hp" is the skeleton
            // root that "bd" and every limb attach to, so without this guard,
            // equipping leggings (which sets "hp:armor") made "hp" register
            // as a hidden ancestor of everything above the hip too - hiding
            // the entire upper body, weapon, and hair along with it. A bare
            // slot's own mesh being replaced must not cascade to unrelated
            // descendants walking through it structurally.
            return false;
        } else {
            auto it = app.partVisible.find(curLower);
            if (it != app.partVisible.end() && !it->second) return false;
        }
        if (cur->parentName.empty()) return true;
        int parentIdx = app.loaded.model.FindPartIndex(cur->parentName);
        if (parentIdx < 0) return true;
        cur = &app.loaded.model.parts[static_cast<size_t>(parentIdx)];
    }
}

static void ResetPartVisibilityToDefaults(AppState& app) {
    app.partVisible.clear();
    app.selectedVariant.clear();
    app.variantTexture.clear();
    for (auto& part : app.loaded.model.parts) {
        if (IsEquipmentVariantOrDescendant(app.loaded.model, part)) continue;
        std::string lower = res::Archive::ToLower(part.name);
        app.partVisible[lower] = true;
    }
}

// ----------------------------------------------------------------------------
// Equipment resolution (Items database -> mesh variant to display)
//
// A Monster's equipped item is stored as a compound "<Blueprint>.<Material>"
// name (e.g. "stone axe.rock") - verified byte-exact against the real
// Universal-Mod/res/databaselmp.res ("stone axe" is its own Weapons.Name row,
// "rock" its own Materials.Name row, and no "stone axe.rock" row exists on
// its own). The Blueprint's TTI ("Texture Type Index") field directly selects
// which numbered mesh variant to show - confirmed exact (not off by one)
// against real .mod hierarchies for both weapons (unhuma's rh3.axe00..05) and
// armor (unhuma's hd.armor01..16 matches Armors "helm" TTI range 1..16
// exactly, bd.armor01..15 matches "plate" TTI range 1..15 exactly).
// ----------------------------------------------------------------------------

static bool SplitBlueprintMaterial(const std::string& full, std::string& blueprint, std::string& material) {
    auto dot = full.find_last_of('.');
    if (dot == std::string::npos) return false;
    blueprint = full.substr(0, dot);
    material = full.substr(dot + 1);
    return true;
}

struct WeaponSlotInfo { std::string groupPrefix, variantPrefix, variantSuffix, textureTypeCode; };

// Verified against unhuma.mod's real hierarchy (figure-format.md's
// "Body-Part-Name Convention for Equipment Variants"): every melee/ranged
// weapon type hangs off the right-hand chain except "bow", whose body (the
// bwpartb.. chain) hangs off the left hand instead. textureTypeCode is the
// 2-letter code redress_res uses for this Type's own per-material textures
// (see ResolveWeaponVariant) - confirmed by listing real files, e.g.
// "unorfeax_01.co.0.mmp" for an orc-female axe.
static const std::map<std::string, WeaponSlotInfo>& WeaponSlotMap() {
    static const std::map<std::string, WeaponSlotInfo> table = {
        {"axe", {"rh3", "axe", "", "ax"}},
        {"sword", {"rh3", "sword", "", "sw"}},
        {"dagger", {"rh3", "dagger", "", "dg"}},
        {"spear", {"rh3", "pike", "", "sp"}},   // DB calls it "spear", the mesh variant is named "pike"
        {"hammer", {"rh3", "club", "", "hm"}},  // DB calls it "hammer", the mesh variant is named "club"
        {"crossbow", {"rh3", "crbow", "main", "cb"}},
        {"bow", {"lh3", "bwpartb", "", "bw"}},
    };
    return table;
}

// Builds the exact redress filename "<raceMask><typeCode>_<TTI2digit>.<materialCode>.<TTI2>"
// (TTI2 is a real DB field, not a guessed value) and checks it actually
// exists. Not every race has its own complete texture set for every item -
// confirmed by the user: orcs share/reuse human weapon textures, e.g. an orc's
// "bone short bow" has no "unorfebw_02.bd.0"/"unormabw_02.bd.0" file and must
// fall back to "unhumabw_02.bd.0" - so this tries the unit's own race first,
// then unhuma, then unhufe.
// triedOut, when given, collects every candidate filename actually attempted
// (whether or not it was found) - for surfacing in the UI so a failed lookup
// shows exactly what name was searched for instead of just "not found".
static std::string ResolveRedressTexture(const AppState& app, const std::string& actualRaceMask,
                                          const std::string& typeCode, int tti, int tti2,
                                          const std::string& materialCode, std::vector<std::string>* triedOut = nullptr) {
    char num[8];
    std::snprintf(num, sizeof(num), "%02d", tti);
    const std::string candidates[] = {actualRaceMask, "unhuma", "unhufe"};
    for (auto& race : candidates) {
        if (race.empty()) continue;
        std::string name = race + typeCode + "_" + num + "." + materialCode + "." + std::to_string(tti2);
        if (triedOut) triedOut->push_back(name);
        if (app.textureSource.Contains(name + ".mmp")) return name;
    }
    return "";
}

// Resolves an equipped weapon's DB reference ("stone axe.rock") to the mesh
// part that should be shown (e.g. "rh3.axe00"), which equipment slot group it
// belongs to (see VariantGroupPrefix), and its own distinct redress texture
// (e.g. "unorfeax_01.co.0") - naming confirmed by listing real redress_res
// files: "<raceMask><typeCode>_<TTI2digit>.<materialCode>.<subvariant>".
// Returns false for a weapon Type this viewer doesn't have a verified
// mesh-slot mapping for yet (e.g. "scepter", added by this mod without a
// confirmed mesh convention). textureName is left empty (not a failure) if no
// matching redress file is found - the caller falls back to the body's own
// primary/secondary texture in that case.
static bool ResolveWeaponVariant(const AppState& app, const std::string& raceMaskName, const std::string& equipRef,
                                  std::string& groupPrefix, std::string& variantName, std::string& textureName,
                                  std::vector<std::string>* triedTextures = nullptr) {
    std::string blueprintName, materialName;
    if (!SplitBlueprintMaterial(equipRef, blueprintName, materialName)) return false;
    const db::ItemBlueprint* bp = app.itemDatabase.FindWeapon(blueprintName);
    if (!bp) return false;
    auto it = WeaponSlotMap().find(bp->type);
    if (it == WeaponSlotMap().end()) return false;
    char num[8];
    std::snprintf(num, sizeof(num), "%02d", bp->tti);
    groupPrefix = it->second.groupPrefix;
    variantName = groupPrefix + "." + it->second.variantPrefix + num + it->second.variantSuffix;

    textureName.clear();
    if (const db::Material* mat = app.itemDatabase.FindMaterial(materialName)) {
        textureName = ResolveRedressTexture(app, raceMaskName, it->second.textureTypeCode, bp->tti, bp->tti2, mat->code, triedTextures);
    }
    return true;
}

// Damage a weapon blueprint actually deals with a given material equipped -
// formula verified byte-exact against tools/dmg-calculator.ods (every
// STONE/BONE/METAL/CRYSTAL example row in that sheet reproduces here to the
// same 2-4 decimal places, e.g. "stone axe"+"granite" -> min 9.54 max 16.74).
// The weapon blueprint contributes its own dMin/dMax; the material
// contributes a single "damage" multiplier applied to both.
struct WeaponDamageResult {
    bool ok = false;
    float minDamage = 0.0f;
    float maxDamage = 0.0f;
    std::string calculation; // human-readable steps, for display next to the result
};

static WeaponDamageResult ComputeWeaponDamage(const db::ItemBlueprint& weapon, const db::Material& material) {
    WeaponDamageResult result;
    result.minDamage = material.damage * weapon.dMin;
    result.maxDamage = material.damage * (weapon.dMin + weapon.dMax);
    result.ok = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "MinDamage = material.damage(%.2f) * weapon.dMin(%.2f) = %.3f\n"
                  "MaxDamage = material.damage(%.2f) * (weapon.dMin(%.2f) + weapon.dMax(%.2f)) = %.3f",
                  material.damage, weapon.dMin, result.minDamage,
                  material.damage, weapon.dMin, weapon.dMax, result.maxDamage);
    result.calculation = buf;
    return result;
}

// Renders the weapon's DP.* fractional weights as e.g. "100% Piercing" or
// "50% Piercing, 50% Slashing" - every sampled DB row so far is a one-hot
// 100% split, but the fields are independent so a mixed split is rendered
// faithfully rather than assumed away.
static std::string DescribeDamageType(const db::ItemBlueprint& weapon) {
    static const std::pair<float db::ItemBlueprint::*, const char*> kTypes[] = {
        {&db::ItemBlueprint::dpPiercing, "Piercing"},   {&db::ItemBlueprint::dpSlashing, "Slashing"},
        {&db::ItemBlueprint::dpBludgeoning, "Bludgeoning"}, {&db::ItemBlueprint::dpThermal, "Thermal"},
        {&db::ItemBlueprint::dpChemical, "Chemical"},   {&db::ItemBlueprint::dpElectric, "Electric"},
        {&db::ItemBlueprint::dpGeneral, "General"},
    };
    std::string out;
    for (auto& [member, label] : kTypes) {
        float weight = weapon.*member;
        if (weight <= 0.0f) continue;
        if (!out.empty()) out += ", ";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f%% %s", weight * 100.0f, label);
        out += buf;
    }
    return out.empty() ? "none" : out;
}

// Defense value an armor blueprint actually gives with a given material
// equipped. Unlike weapons, there's no reference calculator for this - see
// ItemBlueprint::armorAbsorb's comment for how the formula below was derived
// from the real DB data (M.Absorb confirmed as the reliable base value; the
// parallel "A.Absorb" field and both fields' per-damage-type breakdowns were
// excluded as unreliable/uninformative). Mirrors the weapon formula's shape
// (a blueprint-owned base value scaled by the equipped Material's own base
// multiplier) since that's the only other precedent this DB has.
struct ArmorDefenseResult {
    bool ok = false;
    float defense = 0.0f;
    std::string calculation;
};

static ArmorDefenseResult ComputeArmorDefense(const db::ItemBlueprint& armor, const db::Material& material) {
    ArmorDefenseResult result;
    result.defense = material.damage * armor.armorAbsorb;
    result.ok = true;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "Defense = material.damage(%.2f) * armor.armorAbsorb(%.2f) = %.3f",
                  material.damage, armor.armorAbsorb, result.defense);
    result.calculation = buf;
    return result;
}

// "helm"/"plate"/"leggings" (heavy armor) get their OWN dedicated mesh, TTI-
// selected - file-verified 1:1 for helm/plate (DB TTI ranges match those
// slots' real mesh-variant counts exactly, 16 and 15, no exceptions) and
// confirmed via --diagnose for leggings (a real "gipat medium leggins" row
// with TTI=5 resolves to hp/ll1/ll2/rl1/rl2.armor05, which all exist).
//
// "shirt"/"pants"/"boots"/"gloves" (light clothing) have NO mesh of their own
// at all - confirmed both by TTI never lining up with a real mesh (a "gipat
// medium shirt" row with TTI=0 would need "bd.armor00", which doesn't exist -
// armor variants start at 01) and directly by the user: these are a texture
// painted onto the bare body parts, not a separate piece geometry. Selecting
// one must NOT touch app.selectedVariant/mesh choice at all - only its
// resolved texture is applied, directly to the bare part(s) in ArmorSlotMap.
static bool ArmorTypeHasNoMeshOfItsOwn(const std::string& type) {
    return type == "shirt" || type == "pants" || type == "boots" || type == "gloves";
}

static const std::map<std::string, std::vector<std::string>>& ArmorSlotMap() {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"helm", {"hd"}},
        // Plate has its own dedicated mesh variant on the torso AND all four
        // arm segments (lh1.armorNN/lh2.armorNN/rh1.armorNN/rh2.armorNN exist
        // alongside bd.armorNN with the same TTI numbering, confirmed via
        // --diagnose's DIAG_DUMP_PREFIX dump) - unlike "shirt" below, this is
        // real dedicated sleeve geometry, not a texture reused across UVs.
        {"plate", {"bd", "lh1", "lh2", "rh1", "rh2"}},
        // Torso only - NOT the arm segments too (that was an untested guess,
        // now disproven: a shirt's texture is presumably painted for the
        // torso mesh's own UV layout alone, and applying that SAME image to
        // the arm segments' own (different) UV coordinates sampled mostly
        // transparent regions, which - combined with the alpha-test added
        // for cutout transparency - made the entire arm mesh disappear).
        {"shirt", {"bd"}},
        {"gloves", {"lh3", "rh3"}},
        {"pants", {"hp", "ll1", "ll2", "rl1", "rl2"}},
        {"leggings", {"hp", "ll1", "ll2", "rl1", "rl2"}},
        {"boots", {"ll3", "rl3"}},
    };
    return table;
}

// The 2-letter code redress_res uses for this armor Type's own per-material
// textures (see ResolveArmorVariant) - confirmed by listing real files, e.g.
// "unhufebt_00.lb.0.mmp" for boots. One resolved texture applies to every
// slot the type maps to above (e.g. one "shirt" texture for bd/lh1/lh2/rh1/rh2).
static const std::map<std::string, std::string>& ArmorTextureTypeCode() {
    static const std::map<std::string, std::string> table = {
        {"helm", "hl"}, {"plate", "pl"}, {"shirt", "sh"}, {"pants", "pt"},
        {"boots", "bt"}, {"gloves", "gl"}, {"leggings", "lg"},
    };
    return table;
}

// A resolved armor item can drive more than one equipment-slot group at once
// (see ArmorSlotMap) - e.g. one "shirt" sets bd, lh1, lh2, rh1, AND rh2, all
// sharing the one textureName resolved here. textureName is left empty (not a
// failure) if no matching redress file is found. bareTextureParts collects
// bare (non-dotted) part names that should get textureName directly, for
// light-clothing types with no mesh of their own (see
// ArmorTypeHasNoMeshOfItsOwn) - these must NOT go through groupsAndVariants /
// app.selectedVariant, because e.g. gloves and a held weapon both key off the
// bare "rh3"/"lh3" hand slot, and writing "rh3" into selectedVariant would
// silently clobber whatever weapon variant ("rh3.sword01") was equipped there.
static bool ResolveArmorVariant(const AppState& app, const std::string& raceMaskName, const std::string& equipRef,
                                 std::vector<std::pair<std::string, std::string>>& groupsAndVariants,
                                 std::string& textureName, std::vector<std::string>* triedTextures = nullptr,
                                 std::vector<std::string>* bareTextureParts = nullptr) {
    std::string blueprintName, materialName;
    if (!SplitBlueprintMaterial(equipRef, blueprintName, materialName)) return false;
    const db::ItemBlueprint* bp = app.itemDatabase.FindArmor(blueprintName);
    if (!bp) return false;
    auto it = ArmorSlotMap().find(bp->type);
    if (it == ArmorSlotMap().end()) return false;
    if (ArmorTypeHasNoMeshOfItsOwn(bp->type)) {
        if (bareTextureParts) {
            for (auto& barePart : it->second) bareTextureParts->push_back(barePart);
        }
    } else {
        char num[8];
        std::snprintf(num, sizeof(num), "%02d", bp->tti);
        for (auto& groupPrefix : it->second) {
            // Key must match VariantGroupPrefix's own "<prefix>:armor" special-
            // case for any "<prefix>.armorNN" part name (see its comment) - a
            // plain "<prefix>" key here would silently never be read by IsPartVisible.
            groupsAndVariants.emplace_back(groupPrefix + ":armor", groupPrefix + ".armor" + num);
        }
    }

    textureName.clear();
    auto codeIt = ArmorTextureTypeCode().find(bp->type);
    if (codeIt != ArmorTextureTypeCode().end()) {
        if (const db::Material* mat = app.itemDatabase.FindMaterial(materialName)) {
            textureName = ResolveRedressTexture(app, raceMaskName, codeIt->second, bp->tti, bp->tti2, mat->code, triedTextures);
        }
    }
    return !groupsAndVariants.empty() || (bareTextureParts && !bareTextureParts->empty());
}

// Applies everything a Monster record says it's wearing/wielding/wearing-as-hair
// to app.selectedVariant (+ app.variantTexture for each item's own redress
// texture), so loading a unit shows it the way the database actually
// describes rather than bare - see the caller in DrawUnitListPanel.
// raceMaskName is the RaceModel's maskName (e.g. "unorfe") - the prefix every
// redress texture filename starts with. Returns a human-readable line per
// attempted item (applied or why it wasn't) so a caller can surface exactly
// what happened instead of a silent guess.
static std::vector<std::string> ApplyMonsterEquipment(AppState& app, const std::string& raceMaskName, const db::Monster& m) {
    std::vector<std::string> log;

    // hairIndex directly selects "hr.NN" (confirmed authoritative) and needs no
    // Items-database lookup, so apply it regardless of itemDatabaseLoaded below.
    if (m.hairIndex >= 0) {
        char num[16];
        std::snprintf(num, sizeof(num), "%02d", m.hairIndex);
        app.selectedVariant["hr"] = std::string("hr.") + num;
        log.push_back("hair -> hr." + std::string(num));
    } else {
        log.push_back("hair: none (hairIndex=-1)");
    }

    if (!app.itemDatabaseLoaded) {
        log.push_back("weapons/armor: SKIPPED - no item database loaded (Materials/Weapons/Armors)");
        return log;
    }

    auto triedToString = [](const std::vector<std::string>& tried) {
        std::string s;
        for (auto& t : tried) s += t + ".mmp; ";
        return s;
    };

    // Appends the computed min-max damage (and its formula) to a weapon log
    // line, when the DB has enough data to compute it - see ComputeWeaponDamage.
    auto describeDamage = [&](const std::string& equipRef) {
        std::string blueprintName, materialName;
        if (!SplitBlueprintMaterial(equipRef, blueprintName, materialName)) return std::string();
        const db::ItemBlueprint* bp = app.itemDatabase.FindWeapon(blueprintName);
        const db::Material* mat = bp ? app.itemDatabase.FindMaterial(materialName) : nullptr;
        if (!bp || !mat) return std::string();
        WeaponDamageResult dmg = ComputeWeaponDamage(*bp, *mat);
        char buf[128];
        std::snprintf(buf, sizeof(buf), " dmg=%.1f-%.1f (%s, range=%.0f atk=%+.0f def=%+.0f)",
                      dmg.minDamage, dmg.maxDamage, DescribeDamageType(*bp).c_str(), bp->range, bp->attack, bp->defence);
        return std::string(buf);
    };

    std::string group, variant, texture;
    std::vector<std::string> tried;
    if (m.equipmentWeapon1.empty()) {
        log.push_back("weapon1: none in database");
    } else {
        tried.clear();
        if (ResolveWeaponVariant(app, raceMaskName, m.equipmentWeapon1, group, variant, texture, &tried)) {
            app.selectedVariant[group] = variant;
            if (!texture.empty()) app.variantTexture[res::Archive::ToLower(variant)] = texture;
            log.push_back("weapon1 '" + m.equipmentWeapon1 + "' -> " + variant +
                           (texture.empty() ? " (no redress texture found; tried: " + triedToString(tried) + ")" : " tex=" + texture) +
                           describeDamage(m.equipmentWeapon1));
        } else {
            log.push_back("weapon1 '" + m.equipmentWeapon1 + "' -> FAILED to resolve");
        }
    }
    if (m.equipmentWeapon2.empty()) {
        log.push_back("weapon2: none in database");
    } else {
        tried.clear();
        if (ResolveWeaponVariant(app, raceMaskName, m.equipmentWeapon2, group, variant, texture, &tried)) {
            app.selectedVariant[group] = variant;
            if (!texture.empty()) app.variantTexture[res::Archive::ToLower(variant)] = texture;
            log.push_back("weapon2 '" + m.equipmentWeapon2 + "' -> " + variant +
                           (texture.empty() ? " (no redress texture found; tried: " + triedToString(tried) + ")" : " tex=" + texture) +
                           describeDamage(m.equipmentWeapon2));
        } else {
            log.push_back("weapon2 '" + m.equipmentWeapon2 + "' -> FAILED to resolve");
        }
    }
    if (m.equipmentWears.empty()) log.push_back("wears: none in database");
    for (auto& wear : m.equipmentWears) {
        std::vector<std::pair<std::string, std::string>> resolved;
        std::vector<std::string> bareParts;
        tried.clear();
        if (ResolveArmorVariant(app, raceMaskName, wear, resolved, texture, &tried, &bareParts)) {
            std::string names;
            for (auto& gv : resolved) {
                app.selectedVariant[gv.first] = gv.second;
                if (!texture.empty()) app.variantTexture[res::Archive::ToLower(gv.second)] = texture;
                names += gv.second + " ";
            }
            for (auto& bp : bareParts) {
                if (!texture.empty()) app.variantTexture[res::Archive::ToLower(bp)] = texture;
                names += bp + "(texture-only) ";
            }
            std::string defenseStr;
            std::string blueprintName, materialName;
            if (SplitBlueprintMaterial(wear, blueprintName, materialName)) {
                const db::ItemBlueprint* armorBp = app.itemDatabase.FindArmor(blueprintName);
                const db::Material* mat = armorBp ? app.itemDatabase.FindMaterial(materialName) : nullptr;
                if (armorBp && mat) {
                    ArmorDefenseResult def = ComputeArmorDefense(*armorBp, *mat);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), " def=%.2f", def.defense);
                    defenseStr = buf;
                }
            }
            log.push_back("wears '" + wear + "' -> " + names +
                           (texture.empty() ? "(no redress texture found; tried: " + triedToString(tried) + ")" : "tex=" + texture) +
                           defenseStr);
        } else {
            log.push_back("wears '" + wear + "' -> FAILED to resolve");
        }
    }
    return log;
}

// A RaceModel's texture list can use the placeholder prefix "Skin" instead of a
// real filename: verified against the real database, "Human Male" lists
// "Skin_00".."Skin_38" while the actual files on disk are
// "unhumaskin_00".."unhumaskin_44" (same for every other "Skin_NN"-using race,
// e.g. orcs: "Skin_00" -> "unorfeskin_00"/"unormaskin_00", confirmed present in
// redress_res). Other races embed their own creature-specific prefix directly
// (e.g. "Dragon01", "Goblin00") and are used as-is - only the literal "Skin"
// placeholder needs substituting with "<maskName>skin".
static std::string ResolveRaceTextureName(const std::string& entry, const std::string& maskName) {
    if (entry.size() >= 4 && res::Archive::ToLower(entry.substr(0, 4)) == "skin") {
        return maskName + "skin" + entry.substr(4);
    }
    return entry;
}

// A Monster's own "Graphics Data Skin Index" field is the authoritative skin
// selection - not merely one of the options RaceModel.primaryTextures lists -
// and directly IS the "<maskName>skin_NN" suffix number (e.g. skinIndex=5 on
// an unhuma-based monster -> "unhumaskin_05"). Only applies to races using the
// generic "Skin_NN" placeholder scheme (detected the same way
// ResolveRaceTextureName does); other races keep their own concrete texture
// name from primaryTextures as-is, since their skin isn't index-selected.
static std::string ResolveMonsterSkinTexture(const db::RaceModel& race, const db::Monster& m) {
    bool usesSkinPlaceholder = !race.primaryTextures.empty() &&
                               race.primaryTextures[0].size() >= 4 &&
                               res::Archive::ToLower(race.primaryTextures[0].substr(0, 4)) == "skin";
    if (usesSkinPlaceholder) {
        char num[8];
        std::snprintf(num, sizeof(num), "%02d", m.skinIndex);
        return race.maskName + "skin_" + num;
    }
    return race.primaryTextures.empty() ? "" : ResolveRaceTextureName(race.primaryTextures[0], race.maskName);
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
    app.currentMaskName = name; // for redress texture resolution in the manual Weapon panel
    app.selectedClip.clear();
    if (!app.loaded.animClips.empty()) app.selectedClip = app.loaded.animClips.begin()->first;
    ResetPartVisibilityToDefaults(app);

    // Loading directly by name (as opposed to picking a unit from the database
    // list) otherwise never guesses a skin texture at all - e.g. unhuma/unhufe's
    // actual skin textures ("unhumaskin_00".."_44"/"unhufeskin_*") live in a
    // separate redress_res archive and don't share the figure's own base name,
    // so there's nothing to guess from the model name alone without the database.
    if (app.databaseLoaded) {
        if (const db::RaceModel* race = app.database.FindRaceModelByMask(name)) {
            if (!race->primaryTextures.empty()) app.primaryTexture = ResolveRaceTextureName(race->primaryTextures[0], race->maskName);
            if (!race->secondaryTextures.empty()) app.secondaryTexture = ResolveRaceTextureName(race->secondaryTextures[0], race->maskName);
        }
    }

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

    // Each part's own .fig header picks which of these two slots it samples
    // from (mesh.textureNumber, 0=primary/1=secondary) - previously only
    // primTex was ever bound, so every part (including weapons/accessories
    // meant to use the secondary slot) rendered with the body's skin texture.
    GLuint primTex = LoadOrGetTexture(app, app.primaryTexture);
    GLuint secTex = LoadOrGetTexture(app, app.secondaryTexture);

    if (app.showMesh) {
        glPolygonMode(GL_FRONT_AND_BACK, app.showWireframe ? GL_LINE : GL_FILL);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);

        GLuint boundTex = 0;    // 0 is also a valid "nothing bound yet" sentinel here
        bool texEnabled = false; // tri-state avoided: recomputed fresh every part below
        bool alphaEnabled = false;
        auto setGlState = [&](bool textured, bool alphaBlend) {
            if (textured != texEnabled) {
                if (textured) {
                    glEnable(GL_TEXTURE_2D);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glColor3f(1.0f, 1.0f, 1.0f);
                } else {
                    glDisable(GL_TEXTURE_2D);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                    glColor3f(0.75f, 0.72f, 0.65f);
                }
                texEnabled = textured;
                boundTex = 0;
            }
            // Many equipment textures are painted with a cutout alpha mask
            // (large parts of the sheet meant to show nothing, e.g. a helm
            // texture that doesn't cover the whole head mesh) - alpha-testing
            // discards near-transparent fragments outright (avoiding
            // depth/z-fighting glitches a cutout would otherwise cause with
            // only blending), and blending smooths partially-transparent
            // edge pixels.
            bool wantAlpha = textured && alphaBlend;
            if (wantAlpha != alphaEnabled) {
                if (wantAlpha) {
                    glEnable(GL_ALPHA_TEST);
                    glAlphaFunc(GL_GREATER, 0.1f);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    glDisable(GL_ALPHA_TEST);
                    glDisable(GL_BLEND);
                }
                alphaEnabled = wantAlpha;
            }
        };
        auto drawPart = [&](PartRenderData& rd, GLuint tex, bool alphaBlend) {
            bool textured = tex != 0 && !app.showWireframe;
            setGlState(textured, alphaBlend);
            if (textured && tex != boundTex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                boundTex = tex;
            }
            glVertexPointer(3, GL_FLOAT, 0, rd.positions.data());
            glNormalPointer(GL_FLOAT, 0, rd.normals.data());
            if (texEnabled) glTexCoordPointer(2, GL_FLOAT, 0, rd.uvs.data());
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(rd.indices.size()), GL_UNSIGNED_SHORT, rd.indices.data());
        };
        for (auto& rd : app.renderParts) {
            if (rd.indices.empty()) continue;
            GLuint baseTex = (rd.textureNumber == 1 && secTex != 0) ? secTex : primTex;
            // Equipped weapons/armor have their own distinct "redress" texture
            // (see ApplyMonsterEquipment/variantTexture) that takes priority
            // over the body's primary/secondary skin texture when present.
            auto overrideIt = app.variantTexture.find(rd.partNameLower);
            GLuint overrideTex = 0;
            if (overrideIt != app.variantTexture.end()) {
                overrideTex = LoadOrGetTexture(app, overrideIt->second);
            }
            if (overrideTex == 0) {
                drawPart(rd, baseTex, false);
                continue;
            }
            // Bare (dotless) body parts are reused in place - e.g. light armor
            // (shirt/pants/boots/gloves) has no mesh of its own and is just a
            // redress texture applied straight onto "bd"/"lh3"/"rh3"/etc, so
            // there is no separate underlying draw call left to show skin
            // through the redress texture's cutout regions. Draw the base
            // skin pass first, then the redress texture as a blended overlay
            // on the very same geometry. Dotted parts (dedicated armor/weapon
            // meshes like "hd.armorNN") are distinct geometry from their bare
            // parent ("hd"), which already renders separately with skin, so a
            // single override-only draw is correct for them.
            bool isBareOverlay = rd.partNameLower.find('.') == std::string::npos;
            if (isBareOverlay) {
                drawPart(rd, baseTex, false);
                drawPart(rd, overrideTex, true);
            } else {
                drawPart(rd, overrideTex, true);
            }
        }

        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    if (app.showSkeleton) {
        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glColor3f(1.0f, 0.9f, 0.1f);
        glBegin(GL_LINES);
        for (auto& rd : app.renderParts) {
            if (!rd.hasParent) continue;
            if (app.skeletonVisibleOnly && !rd.visible) continue;
            glVertex3f(rd.origin.x, rd.origin.y, rd.origin.z);
            glVertex3f(rd.parentOrigin.x, rd.parentOrigin.y, rd.parentOrigin.z);
        }
        glEnd();
        glPointSize(6.0f);
        glColor3f(1.0f, 0.2f, 0.2f);
        glBegin(GL_POINTS);
        for (auto& rd : app.renderParts) {
            if (app.skeletonVisibleOnly && !rd.visible) continue;
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

// Shared by the "Load"/"Reload" buttons - re-reads both the Units and Items
// databases from the given path, so editing the database file (e.g. in
// EIDBEditor or the xlsx) and clicking Reload picks up the change without
// restarting the whole viewer and re-loading figures/textures from scratch.
// saveConfig=false is for --diagnose (RunDiagnoseCli): that path never
// populates app.databasePathBuf (it has no GUI text field to keep in sync),
// so letting this persist the config would silently blank out the saved
// database path - confirmed as a real bug this caused.
static void LoadDatabaseFromPath(AppState& app, const std::string& path, bool saveConfig = true) {
    std::string err, itemErr;
    db::UnitDatabase newDb;
    db::ItemDatabase newItemDb;
    bool ok, itemOk;
    if (path.size() > 5 && path.substr(path.size() - 5) == ".xlsx") {
        ok = db::LoadUnitsFromXlsx(path, newDb, err);
        itemOk = db::LoadItemsFromXlsx(path, newItemDb, itemErr);
    } else {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ok = db::LoadUnitsFromRes(bytes, newDb, err);
        itemOk = db::LoadItemsFromRes(bytes, newItemDb, itemErr);
    }
    app.databaseLoaded = ok;
    app.databaseError = err;
    app.itemDatabaseLoaded = itemOk;
    if (itemOk) app.itemDatabase = std::move(newItemDb);
    if (ok) {
        app.database = std::move(newDb);
        app.statusText = "Database loaded: " + std::to_string(app.database.raceModels.size()) + " race models, " +
                          std::to_string(app.database.monsters.size()) + " monsters, " +
                          std::to_string(app.itemDatabase.weapons.size()) + " weapons, " +
                          std::to_string(app.itemDatabase.armors.size()) + " armors, " +
                          std::to_string(app.itemDatabase.materials.size()) + " materials";
        if (saveConfig) SaveSourceConfig(app);
    } else {
        app.statusText = "Database load failed: " + err;
    }
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
    ImGui::Text("Database (database.res/databaselmp.res or .xlsx - same data, sp/mp):");
    StatusDot(app.databaseLoaded, "Loaded OK", app.databaseError.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##dbpath", app.databasePathBuf, sizeof(app.databasePathBuf));
    PathPickerRow("dbfile", app.databasePathBuf, sizeof(app.databasePathBuf), false);
    ImGui::SameLine();
    if (ImGui::Button("Load##db")) LoadDatabaseFromPath(app, app.databasePathBuf);
    ImGui::SameLine();
    if (ImGui::Button("Reload##db")) {
        if (app.databasePathBuf[0] == '\0') app.statusText = "No database path set yet - use Load first.";
        else LoadDatabaseFromPath(app, app.databasePathBuf);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-reads the same database file - use after editing it (e.g. in EIDBEditor/xlsx) without restarting the viewer.");

    ImGui::Separator();
    ImGui::Text("Load figure by name directly:");
    StatusDot(app.loaded.ok, "Model loaded OK", app.loaded.error.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##modelname", app.modelNameBuf, sizeof(app.modelNameBuf));
    ImGui::SameLine();
    if (ImGui::Button("Load##model")) LoadModelByName(app, app.modelNameBuf);
}

// Populates the manual Weapon/Armor editor dropdowns (DrawWeaponEditorPanel/
// DrawArmorEditorPanel) from a Monster's actual database equipment, so
// picking a unit from the list shows what it's already wearing in those
// panels instead of leaving them on "(choose)"/"(none)" - matching what
// ApplyMonsterEquipment just did to the 3D view itself.
static void SyncEquipmentEditorFields(AppState& app, const db::Monster& m) {
    if (!app.itemDatabaseLoaded) return;
    app.weaponEditorType.clear();
    app.weaponEditorBlueprint.clear();
    app.weaponEditorMaterial.clear();
    if (!m.equipmentWeapon1.empty()) {
        std::string blueprintName, materialName;
        if (SplitBlueprintMaterial(m.equipmentWeapon1, blueprintName, materialName)) {
            if (const db::ItemBlueprint* bp = app.itemDatabase.FindWeapon(blueprintName)) {
                app.weaponEditorType = bp->type;
                app.weaponEditorBlueprint = blueprintName;
                app.weaponEditorMaterial = materialName;
            }
        }
    }
    app.armorEditorBlueprint.clear();
    app.armorEditorMaterial.clear();
    for (auto& wear : m.equipmentWears) {
        std::string blueprintName, materialName;
        if (!SplitBlueprintMaterial(wear, blueprintName, materialName)) continue;
        if (const db::ItemBlueprint* bp = app.itemDatabase.FindArmor(blueprintName)) {
            app.armorEditorBlueprint[bp->type] = blueprintName;
            app.armorEditorMaterial[bp->type] = materialName;
        }
    }
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
                app.primaryTexture = ResolveMonsterSkinTexture(*race, m);
                app.secondaryTexture = race->secondaryTextures.empty() ? "" : ResolveRaceTextureName(race->secondaryTextures[0], race->maskName);
                // LoadModelByName already reset part visibility to the bare-body
                // defaults; layer this Monster's actual equipment on top of that.
                app.lastEquipLog = ApplyMonsterEquipment(app, race->maskName, m);
                SyncEquipmentEditorFields(app, m);
                app.lastEquipLog.insert(app.lastEquipLog.begin(),
                    "skin (raw DB skinIndex=" + std::to_string(m.skinIndex) + ", RaceModel.primaryTextures[0]='" +
                    (race->primaryTextures.empty() ? "(none)" : race->primaryTextures[0]) + "') -> " + app.primaryTexture +
                    (app.textureSource.Contains(app.primaryTexture + ".mmp") ? " (found)" : " (NOT FOUND in loaded texture source)"));
                RebuildRenderData(app);
            } else {
                app.statusText = "No RaceModel found for base race '" + m.baseRace + "'";
            }
        }
    }
    ImGui::EndChild();

    if (!app.lastEquipLog.empty()) {
        ImGui::TextUnformatted("Last pick's resolution log (selectable):");
        std::string joined;
        for (auto& line : app.lastEquipLog) { joined += line; joined += '\n'; }
        ImGui::InputTextMultiline("##equiplog", joined.data(), joined.size() + 1, ImVec2(0, 120),
                                   ImGuiInputTextFlags_ReadOnly);
    }
}

// Lets the user swap the currently-loaded unit's held weapon: pick a Type to
// narrow the blueprint list, a Blueprint (Items DB "Weapons" row - a weapon
// prototype independent of material, e.g. "stone axe"), and a Material - the
// combination resolves to a real mesh variant via ResolveWeaponVariant and is
// applied immediately, the same way loading a Monster record's own equipment
// does. See the WeaponSlotMap/ResolveWeaponVariant comments for what's
// actually verified here (all 7 weapon types) versus armor (only helm/plate).
static void DrawWeaponEditorPanel(AppState& app) {
    if (!app.itemDatabaseLoaded) {
        ImGui::TextDisabled("Load a database to browse weapons/materials.");
        return;
    }
    if (!app.loaded.ok || !app.loaded.model.isComposite) {
        ImGui::TextDisabled("Load a composite (humanoid/creature) model first.");
        return;
    }

    if (ImGui::BeginCombo("Type", app.weaponEditorType.empty() ? "(choose)" : app.weaponEditorType.c_str())) {
        for (auto& kv : WeaponSlotMap()) {
            bool isSel = kv.first == app.weaponEditorType;
            if (ImGui::Selectable(kv.first.c_str(), isSel)) {
                app.weaponEditorType = kv.first;
                app.weaponEditorBlueprint.clear();
                // Otherwise the previous type's mesh (e.g. an axe) stays
                // visible under its own group (rh3 or lh3 for bow) until a
                // new blueprint+material is actually picked for the new type.
                for (auto& slot : WeaponSlotMap()) {
                    app.selectedVariant.erase(slot.second.groupPrefix);
                    // both directions share "rh3" for melee types and "lh3" for bow/crossbow's grip hand
                }
                RebuildRenderData(app);
            }
        }
        ImGui::EndCombo();
    }

    bool changed = false;
    const db::ItemBlueprint* selectedBlueprint = nullptr;
    if (!app.weaponEditorType.empty()) {
        if (ImGui::BeginCombo("Blueprint", app.weaponEditorBlueprint.empty() ? "(choose)" : app.weaponEditorBlueprint.c_str())) {
            for (auto& w : app.itemDatabase.weapons) {
                if (w.type != app.weaponEditorType) continue;
                bool isSel = w.name == app.weaponEditorBlueprint;
                if (ImGui::Selectable(w.name.c_str(), isSel)) { app.weaponEditorBlueprint = w.name; app.weaponEditorMaterial.clear(); changed = true; }
            }
            ImGui::EndCombo();
        }
        selectedBlueprint = app.itemDatabase.FindWeapon(app.weaponEditorBlueprint);
    } else {
        ImGui::TextDisabled("Blueprint: pick a Type first.");
    }

    // A blueprint's own "M.Type" (e.g. "Bone" for "bone short bow") is the
    // Materials.Type every valid choice for it must match - narrows a ~46-item
    // flat list down to only the materials that actually apply.
    if (!selectedBlueprint) {
        ImGui::TextDisabled("Material: pick a Blueprint first.");
    } else if (ImGui::BeginCombo("Material", app.weaponEditorMaterial.empty() ? "(choose)" : app.weaponEditorMaterial.c_str())) {
        for (auto& mat : app.itemDatabase.materials) {
            if (mat.type != selectedBlueprint->materialType) continue;
            bool isSel = mat.name == app.weaponEditorMaterial;
            if (ImGui::Selectable(mat.name.c_str(), isSel)) { app.weaponEditorMaterial = mat.name; changed = true; }
        }
        ImGui::EndCombo();
    }

    if (selectedBlueprint && !app.weaponEditorMaterial.empty()) {
        if (const db::Material* mat = app.itemDatabase.FindMaterial(app.weaponEditorMaterial)) {
            WeaponDamageResult dmg = ComputeWeaponDamage(*selectedBlueprint, *mat);
            ImGui::Separator();
            ImGui::Text("Damage: %.1f - %.1f (%s)", dmg.minDamage, dmg.maxDamage, DescribeDamageType(*selectedBlueprint).c_str());
            ImGui::Text("Range: %.0f   Attack: %+.0f   Defence: %+.0f", selectedBlueprint->range, selectedBlueprint->attack, selectedBlueprint->defence);
            ImGui::TextWrapped("%s", dmg.calculation.c_str());
        }
    }

    if (changed && !app.weaponEditorBlueprint.empty() && !app.weaponEditorMaterial.empty()) {
        std::string equipRef = app.weaponEditorBlueprint + "." + app.weaponEditorMaterial;
        std::string group, variant, texture;
        std::vector<std::string> tried;
        if (ResolveWeaponVariant(app, app.currentMaskName, equipRef, group, variant, texture, &tried)) {
            app.selectedVariant[group] = variant;
            if (!texture.empty()) app.variantTexture[res::Archive::ToLower(variant)] = texture;
            else app.variantTexture.erase(res::Archive::ToLower(variant));
            RebuildRenderData(app);
            std::string triedStr;
            for (auto& t : tried) triedStr += t + ".mmp; ";
            app.statusText = "Equipped '" + app.weaponEditorBlueprint + "' (" + app.weaponEditorMaterial + ") -> " + variant +
                              (texture.empty() ? " (no redress texture found; tried: " + triedStr + ")" : ", tex=" + texture);
        } else {
            app.statusText = "Could not resolve a mesh variant for '" + equipRef + "'";
        }
    }

    ImGui::TextWrapped("Selects the weapon shape and its own redress texture "
                        "(<raceMask><typeCode>_<TTI>.<materialCode>.*) for the chosen blueprint+material. "
                        "Needs redress_res loaded as a texture layer and the current model to have been "
                        "loaded under its real race mask name (currently: '%s').",
                        app.currentMaskName.empty() ? "(none)" : app.currentMaskName.c_str());
}

// One Blueprint+Material picker per armor Type (unlike a weapon, several
// armor pieces are worn simultaneously - a helm, a plate, gloves, etc. all at
// once) - see DrawWeaponEditorPanel's header comment for why a material must
// be picked at all: the mesh variant alone (from Blueprint's TTI) doesn't
// determine which redress texture to use (e.g. stone -> ".rc.", alloy ->
// ".al." are different files for the same shape), so leaving material
// unspecified meant the piece rendered with no known texture at all.
static void DrawArmorEditorPanel(AppState& app) {
    if (!app.itemDatabaseLoaded) {
        ImGui::TextDisabled("Load a database to browse armors/materials.");
        return;
    }
    if (!app.loaded.ok || !app.loaded.model.isComposite) {
        ImGui::TextDisabled("Load a composite (humanoid/creature) model first.");
        return;
    }

    for (auto& kv : ArmorSlotMap()) {
        const std::string& type = kv.first;
        ImGui::PushID(type.c_str());
        ImGui::TextUnformatted(type.c_str());
        ImGui::SameLine(90);

        std::string& blueprint = app.armorEditorBlueprint[type]; // creates "" if absent
        std::string& material = app.armorEditorMaterial[type];
        bool changed = false;

        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##blueprint", blueprint.empty() ? "(none)" : blueprint.c_str())) {
            bool noneSelected = blueprint.empty();
            if (ImGui::Selectable("(none)", noneSelected)) { blueprint.clear(); material.clear(); changed = true; }
            for (auto& a : app.itemDatabase.armors) {
                if (a.type != type) continue;
                bool isSel = a.name == blueprint;
                if (ImGui::Selectable(a.name.c_str(), isSel)) { blueprint = a.name; material.clear(); changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        const db::ItemBlueprint* selectedArmor = blueprint.empty() ? nullptr : app.itemDatabase.FindArmor(blueprint);
        // Narrows the material list to this blueprint's own "M.Type" (e.g.
        // "Leather"), same reasoning as DrawWeaponEditorPanel.
        if (!selectedArmor) {
            ImGui::TextDisabled("(pick a blueprint)");
        } else if (ImGui::BeginCombo("##material", material.empty() ? "(pick)" : material.c_str())) {
            for (auto& mat : app.itemDatabase.materials) {
                if (mat.type != selectedArmor->materialType) continue;
                bool isSel = mat.name == material;
                if (ImGui::Selectable(mat.name.c_str(), isSel)) { material = mat.name; changed = true; }
            }
            ImGui::EndCombo();
        }
        if (selectedArmor && !material.empty()) {
            if (const db::Material* mat = app.itemDatabase.FindMaterial(material)) {
                ArmorDefenseResult def = ComputeArmorDefense(*selectedArmor, *mat);
                ImGui::SameLine();
                ImGui::Text("def=%.2f", def.defense);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", def.calculation.c_str());
            }
        }

        if (changed) {
            if (blueprint.empty()) {
                // "(none)" - clear every slot this type maps to, matching what
                // ResolveArmorVariant would have set had it been equipped. Light
                // clothing types key by the bare part name (no mesh of their
                // own - see ArmorTypeHasNoMeshOfItsOwn); everything else uses
                // the dotted-mesh ":armor" group key.
                bool noMesh = ArmorTypeHasNoMeshOfItsOwn(type);
                for (auto& part : kv.second) {
                    app.selectedVariant.erase(noMesh ? part : part + ":armor");
                    if (noMesh) app.variantTexture.erase(res::Archive::ToLower(part));
                }
                app.statusText = "Unequipped " + type;
            } else if (!material.empty()) {
                std::string equipRef = blueprint + "." + material;
                std::vector<std::pair<std::string, std::string>> resolved;
                std::vector<std::string> bareParts;
                std::string texture;
                std::vector<std::string> tried;
                if (ResolveArmorVariant(app, app.currentMaskName, equipRef, resolved, texture, &tried, &bareParts)) {
                    std::string names;
                    for (auto& gv : resolved) {
                        app.selectedVariant[gv.first] = gv.second;
                        if (!texture.empty()) app.variantTexture[res::Archive::ToLower(gv.second)] = texture;
                        else app.variantTexture.erase(res::Archive::ToLower(gv.second));
                        names += gv.second + " ";
                    }
                    for (auto& bp : bareParts) {
                        if (!texture.empty()) app.variantTexture[res::Archive::ToLower(bp)] = texture;
                        else app.variantTexture.erase(res::Archive::ToLower(bp));
                        names += bp + "(texture-only) ";
                    }
                    std::string triedStr;
                    for (auto& t : tried) triedStr += t + ".mmp; ";
                    app.statusText = "Equipped '" + blueprint + "' (" + material + ") -> " + names +
                                      (texture.empty() ? "(no redress texture found; tried: " + triedStr + ")" : "tex=" + texture);
                } else {
                    app.statusText = "Could not resolve a mesh variant for '" + equipRef + "'";
                }
            }
            RebuildRenderData(app);
        }
        ImGui::PopID();
    }

    ImGui::TextWrapped("Pick a Blueprint AND a Material for each slot you want equipped - the material "
                        "determines which redress texture variant is used (e.g. stone -> \".rc.\", "
                        "alloy -> \".al.\"), so a Blueprint alone isn't enough to render correctly.");
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

    ImGui::TextWrapped("Composite models bundle every equipment/hair/weapon variant into one file; "
                        "the game only ever shows one variant per slot at a time. Body slots are "
                        "shown by default, equipment slots start unequipped.");

    bool changed = false;
    if (ImGui::Button("Show All Body Parts")) {
        for (auto& kv : app.partVisible) kv.second = true;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide All Body Parts")) {
        for (auto& kv : app.partVisible) kv.second = false;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset To Defaults")) {
        ResetPartVisibilityToDefaults(app);
        changed = true;
    }

    static char partFilter[64] = "";
    ImGui::InputTextWithHint("##partfilter", "filter parts...", partFilter, sizeof(partFilter));
    std::string filterLower = partFilter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    ImGui::TextUnformatted("Body:");
    ImGui::BeginChild("##partlist", ImVec2(0, 150), ImGuiChildFlags_Borders);
    for (auto& part : app.loaded.model.parts) {
        if (part.name.find('.') != std::string::npos) continue; // shown in "Equipment" below instead
        std::string lower = res::Archive::ToLower(part.name);
        if (!filterLower.empty() && lower.find(filterLower) == std::string::npos) continue;
        auto it = app.partVisible.find(lower);
        if (it == app.partVisible.end()) continue; // a non-variant part hanging off equipment (e.g. a jaw bone) - no manual toggle
        bool visible = it->second;
        if (ImGui::Checkbox(part.name.c_str(), &visible)) {
            it->second = visible;
            changed = true;
        }
    }
    ImGui::EndChild();

    // Group every dotted variant name by its slot prefix (e.g. "hd.armor01" and
    // "hd.armor02" both belong to slot "hd") so each slot gets one dropdown -
    // matches how the game only ever shows one equipped item per slot.
    std::map<std::string, std::vector<const fig::ModelPart*>> groups;
    for (auto& part : app.loaded.model.parts) {
        std::string prefix = VariantGroupPrefix(part.name);
        if (prefix.empty()) continue;
        if (!filterLower.empty() && res::Archive::ToLower(part.name).find(filterLower) == std::string::npos) continue;
        groups[prefix].push_back(&part);
    }

    if (!groups.empty()) {
        ImGui::TextUnformatted("Equipment (mesh shape only - no texture; use the Weapon/Armor panels below for a textured pick):");
        ImGui::BeginChild("##equiplist", ImVec2(0, 150), ImGuiChildFlags_Borders);
        for (auto& kv : groups) {
            const std::string& prefix = kv.first;
            std::string& selected = app.selectedVariant[prefix]; // creates as "" (none) if absent
            std::string previewLabel = selected.empty() ? "(none)" : selected;
            if (ImGui::BeginCombo(prefix.c_str(), previewLabel.c_str())) {
                bool noneSelected = selected.empty();
                if (ImGui::Selectable("(none)", noneSelected)) { selected.clear(); changed = true; }
                for (auto* part : kv.second) {
                    bool isSelected = res::Archive::ToLower(selected) == res::Archive::ToLower(part->name);
                    if (ImGui::Selectable(part->name.c_str(), isSelected)) { selected = part->name; changed = true; }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::EndChild();
    }

    if (changed) RebuildRenderData(app);
}

static void DrawModelControlsPanel(AppState& app) {
    if (!app.loaded.ok) {
        ImGui::TextDisabled("No model loaded.");
        return;
    }
    bool changed = false;
    changed |= ImGui::SliderFloat("Strength", &app.constitution.x, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Dexterity", &app.constitution.y, 0.0f, 1.0f);
    // Third complection axis - confirmed via ei_maper's own source comment
    // ("x == str, y == dex, z == scale", figure.cpp) to be a body-size slider,
    // not "height/tallness" as the vertex-format docs previously assumed.
    changed |= ImGui::SliderFloat("Scale", &app.constitution.z, 0.0f, 1.0f);
    if (changed) RebuildRenderData(app);

    ImGui::Checkbox("Mesh", &app.showMesh);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &app.showWireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &app.showSkeleton);
    if (app.showSkeleton) {
        ImGui::SameLine();
        ImGui::Checkbox("Visible-only bones", &app.skeletonVisibleOnly);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hide skeleton joints/lines for parts that aren't currently "
                               "rendered (e.g. unequipped weapon/hair sockets on unhuma/unhufe) "
                               "instead of drawing the full underlying hierarchy.");
        }
    }

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
        ImGui::TextDisabled("(best-effort: no reference .anm implementation exists to verify against)");
        ImGui::SliderFloat("Time", &app.animTime, 0.0f, 10.0f);
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

// --diagnose "<Monster Name>" CLI mode: resolves everything the GUI would for
// that one Monster (figure, skin, hair, weapons, armor - mesh AND texture)
// using the same remembered source paths as the GUI (config.hpp), and prints
// it all to stdout without opening a window. Exists so a resolution mismatch
// can be tracked down from a terminal instead of clicking through the UI.
static int RunDiagnoseCli(const std::string& monsterName) {
    AppState app;
    config::Config cfg = config::Load();
    for (auto& p : cfg.figureLayers) app.figureSource.AddLayer(p);
    for (auto& p : cfg.textureLayers) app.textureSource.AddLayer(p);
    if (!app.textureSource.layers.empty()) app.availableTextures = app.textureSource.ListBaseNames({".mmp"});
    std::printf("Figure layers: %zu (%s)\n", app.figureSource.layers.size(), app.figureSource.AnyLoaded() ? "loaded" : "NONE LOADED");
    std::printf("Texture layers: %zu (%s), %zu .mmp names indexed\n", app.textureSource.layers.size(),
                app.textureSource.AnyLoaded() ? "loaded" : "NONE LOADED", app.availableTextures.size());
    if (cfg.databasePath.empty()) { std::printf("No database path in config - nothing to diagnose.\n"); return 1; }
    std::printf("Database: %s\n", cfg.databasePath.c_str());
    LoadDatabaseFromPath(app, cfg.databasePath, /*saveConfig=*/false);
    std::printf("Units DB loaded: %s (%zu race models, %zu monsters)\n", app.databaseLoaded ? "yes" : "NO",
                app.database.raceModels.size(), app.database.monsters.size());
    std::printf("Items DB loaded: %s (%zu weapons, %zu armors, %zu materials)\n\n", app.itemDatabaseLoaded ? "yes" : "NO",
                app.itemDatabase.weapons.size(), app.itemDatabase.armors.size(), app.itemDatabase.materials.size());

    const db::Monster* mon = nullptr;
    for (auto& m : app.database.monsters) if (m.name == monsterName) { mon = &m; break; }
    if (!mon) { std::printf("Monster '%s' NOT FOUND in database.\n", monsterName.c_str()); return 1; }
    const db::Monster& m = *mon;

    std::printf("=== %s ===\n", m.name.c_str());
    std::printf("Base Race: '%s'\n", m.baseRace.c_str());
    const db::RaceModel* race = app.database.FindRaceModel(m.baseRace);
    if (!race) { std::printf("  -> RaceModel NOT FOUND for this base race. Nothing else can be resolved.\n"); return 1; }
    std::printf("  -> RaceModel found: name='%s' maskName='%s' primaryTextures[0]='%s'\n",
                race->name.c_str(), race->maskName.c_str(), race->primaryTextures.empty() ? "(none)" : race->primaryTextures[0].c_str());
    std::printf("Complection: X=%.3f Y=%.3f Z=%.3f\n", m.complectionX, m.complectionY, m.complectionZ);

    std::string skinTex = ResolveMonsterSkinTexture(*race, m);
    std::printf("Skin Index: %d -> resolved texture '%s' [%s]\n", m.skinIndex, skinTex.c_str(),
                app.textureSource.Contains(skinTex + ".mmp") ? "FOUND" : "NOT FOUND");

    if (m.hairIndex >= 0) {
        char num[16];
        std::snprintf(num, sizeof(num), "%02d", m.hairIndex);
        std::string hairPart = std::string("hr.") + num;
        std::printf("Hair Index: %d -> mesh part '%s'\n", m.hairIndex, hairPart.c_str());
    } else {
        std::printf("Hair Index: %d -> no hair\n", m.hairIndex);
    }

    app.constitution = {m.complectionX, m.complectionY, m.complectionZ};
    LoadModelByName(app, race->maskName);
    std::printf("Figure load ('%s'): %s", race->maskName.c_str(), app.loaded.ok ? "OK" : "FAILED");
    if (app.loaded.ok) std::printf(" (%zu parts)", app.loaded.model.parts.size());
    std::printf("\n\n");

    if (const char* dbgPrefix = std::getenv("DIAG_DUMP_PREFIX")) {
        std::printf("-- parts starting with '%s' (name -> parent) --\n", dbgPrefix);
        std::string lowerPrefix = res::Archive::ToLower(dbgPrefix);
        for (auto& p : app.loaded.model.parts) {
            if (res::Archive::ToLower(p.name).rfind(lowerPrefix, 0) == 0) {
                std::printf("  %s -> parent='%s'\n", p.name.c_str(), p.parentName.c_str());
            }
        }
        std::printf("\n");
    }

    auto partExists = [&](const std::string& name) { return app.loaded.model.FindPartIndex(name) >= 0; };

    auto printWeapon = [&](const char* label, const std::string& equipRef) {
        if (equipRef.empty()) { std::printf("%s: none in database\n", label); return; }
        std::string blueprintName, materialName;
        SplitBlueprintMaterial(equipRef, blueprintName, materialName);
        const db::ItemBlueprint* bp = app.itemDatabase.FindWeapon(blueprintName);
        std::printf("%s: '%s' (blueprint='%s' material='%s')\n", label, equipRef.c_str(), blueprintName.c_str(), materialName.c_str());
        if (!bp) { std::printf("  -> blueprint NOT FOUND in Weapons\n"); return; }
        std::printf("  -> type='%s' materialType='%s' tti=%d tti2=%d\n", bp->type.c_str(), bp->materialType.c_str(), bp->tti, bp->tti2);
        const db::Material* mat = app.itemDatabase.FindMaterial(materialName);
        std::printf("  -> material code='%s'%s\n", mat ? mat->code.c_str() : "(not found)",
                    (mat && mat->type != bp->materialType) ? "  [WARNING: material.type != blueprint.materialType]" : "");
        std::printf("  -> stats: range=%.0f attack=%+.0f defence=%+.0f damage type: %s\n",
                    bp->range, bp->attack, bp->defence, DescribeDamageType(*bp).c_str());
        if (mat) {
            WeaponDamageResult dmg = ComputeWeaponDamage(*bp, *mat);
            std::printf("  -> damage: %.1f-%.1f\n       %s\n", dmg.minDamage, dmg.maxDamage, dmg.calculation.c_str());
        } else {
            std::printf("  -> damage: unknown (material not found, need material.damage)\n");
        }
        std::string group, variant, texture;
        std::vector<std::string> tried;
        if (ResolveWeaponVariant(app, race->maskName, equipRef, group, variant, texture, &tried)) {
            std::printf("  -> mesh variant '%s' [part %s]\n", variant.c_str(), partExists(variant) ? "EXISTS" : "MISSING FROM FIGURE");
            std::printf("  -> texture: %s\n", texture.empty() ? "NOT FOUND" : texture.c_str());
            for (auto& t : tried) std::printf("     tried: %s.mmp [%s]\n", t.c_str(), app.textureSource.Contains(t + ".mmp") ? "found" : "missing");
        } else {
            std::printf("  -> FAILED to resolve (unmapped weapon type in WeaponSlotMap?)\n");
        }
    };
    printWeapon("Weapon1", m.equipmentWeapon1);
    printWeapon("Weapon2", m.equipmentWeapon2);

    std::printf("Wears (%zu items):\n", m.equipmentWears.size());
    for (auto& wear : m.equipmentWears) {
        std::string blueprintName, materialName;
        SplitBlueprintMaterial(wear, blueprintName, materialName);
        const db::ItemBlueprint* bp = app.itemDatabase.FindArmor(blueprintName);
        std::printf("  '%s' (blueprint='%s' material='%s')\n", wear.c_str(), blueprintName.c_str(), materialName.c_str());
        if (!bp) { std::printf("    -> blueprint NOT FOUND in Armors\n"); continue; }
        std::printf("    -> type='%s' materialType='%s' tti=%d tti2=%d\n", bp->type.c_str(), bp->materialType.c_str(), bp->tti, bp->tti2);
        const db::Material* mat = app.itemDatabase.FindMaterial(materialName);
        std::printf("    -> material code='%s'%s\n", mat ? mat->code.c_str() : "(not found)",
                    (mat && mat->type != bp->materialType) ? "  [WARNING: material.type != blueprint.materialType]" : "");
        if (mat) {
            ArmorDefenseResult def = ComputeArmorDefense(*bp, *mat);
            std::printf("    -> defense: %.2f\n         %s\n", def.defense, def.calculation.c_str());
        } else {
            std::printf("    -> defense: unknown (material not found, need material.damage)\n");
        }
        std::vector<std::pair<std::string, std::string>> resolved;
        std::vector<std::string> bareParts;
        std::string texture;
        std::vector<std::string> tried;
        if (ResolveArmorVariant(app, race->maskName, wear, resolved, texture, &tried, &bareParts)) {
            for (auto& gv : resolved) std::printf("    -> mesh variant '%s' [part %s]\n", gv.second.c_str(), partExists(gv.second) ? "EXISTS" : "MISSING FROM FIGURE");
            for (auto& bp : bareParts) std::printf("    -> texture-only on bare part '%s' [part %s]\n", bp.c_str(), partExists(bp) ? "EXISTS" : "MISSING FROM FIGURE");
            std::printf("    -> texture: %s\n", texture.empty() ? "NOT FOUND" : texture.c_str());
            for (auto& t : tried) std::printf("       tried: %s.mmp [%s]\n", t.c_str(), app.textureSource.Contains(t + ".mmp") ? "found" : "missing");
        } else {
            std::printf("    -> FAILED to resolve (unmapped armor type in ArmorSlotMap?)\n");
        }
    }

    // Now actually apply everything (as the GUI would) and dump the final
    // computed IsPartVisible() result for every part with a dot in its name -
    // this is what really ends up on screen, catching cases where a resolved
    // variant is correct in isolation above but something else (a sibling
    // structural node, a shared attachment) ends up ALSO visible or hides it.
    ResetPartVisibilityToDefaults(app);
    ApplyMonsterEquipment(app, race->maskName, m);
    RebuildRenderData(app);
    std::printf("\nFinal visibility of every dotted (equipment/weapon) part, plus dotless 'base*' weapon parts:\n");
    for (auto& p : app.loaded.model.parts) {
        bool isDotted = p.name.find('.') != std::string::npos;
        bool isBaseWeapon = res::Archive::ToLower(p.name).rfind("base", 0) == 0;
        if (!isDotted && !isBaseWeapon) continue;
        if (IsPartVisible(app, p)) std::printf("  VISIBLE: %s\n", p.name.c_str());
    }
    std::printf("\nCanonical bare body slots (hidden when a dedicated-mesh armor variant replaces them):\n");
    for (const char* slot : {"hd", "bd", "hp", "lh1", "lh2", "lh3", "rh1", "rh2", "rh3",
                             "ll1", "ll2", "ll3", "rl1", "rl2", "rl3"}) {
        int idx = app.loaded.model.FindPartIndex(slot);
        if (idx < 0) continue;
        bool visible = IsPartVisible(app, app.loaded.model.parts[static_cast<size_t>(idx)]);
        std::printf("  %-4s %s\n", slot, visible ? "visible" : "HIDDEN");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::printf(
            "um-modelviewer - Evil Islands figure/database viewer\n\n"
            "Usage:\n"
            "  um-modelviewer                        Launch the normal GUI.\n"
            "  um-modelviewer --diagnose \"<Monster>\"  Resolve one Monster's figure, skin,\n"
            "                                         hair, weapons, and armor (mesh AND\n"
            "                                         texture) to stdout, with no window -\n"
            "                                         for tracking down a resolution bug\n"
            "                                         from a terminal instead of the GUI.\n"
            "                                         Uses the same remembered source paths\n"
            "                                         (figures/textures/database) as the GUI\n"
            "                                         - run the GUI at least once first, or\n"
            "                                         edit um-modelviewer.cfg directly, to\n"
            "                                         set those.\n"
            "  um-modelviewer --help | -h             Show this message.\n\n"
            "Example:\n"
            "  um-modelviewer --diagnose \"LMP Human Gipath Fighter 2\"\n");
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--diagnose") {
        if (argc < 3) { std::fprintf(stderr, "--diagnose needs a Monster name, e.g. --diagnose \"Human Hero\". Run with --help for usage.\n"); return 1; }
        return RunDiagnoseCli(argv[2]);
    }
    if (argc >= 2) {
        std::fprintf(stderr, "Unrecognized argument '%s'. Run with --help for usage.\n", argv[1]);
        return 1;
    }
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

    // Zoom reads this raw accumulator instead of io.MouseWheel - chains to ImGui's
    // own callback first so widget scrolling (combo boxes, part lists, ...) keeps
    // working, but keeps a copy that isn't subject to ImGui's per-widget wheel
    // consumption/hover routing, since that path was unreliable for driving camera
    // zoom while the cursor sits over the bare 3D viewport (no ImGui window there).
    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        ImGui_ImplGlfw_ScrollCallback(w, xoffset, yoffset);
        g_rawScrollY += yoffset;
    });

    AppState app;

    // Restore remembered source paths from the last session, if any.
    {
        config::Config cfg = config::Load();
        for (auto& p : cfg.figureLayers) app.figureSource.AddLayer(p);
        for (auto& p : cfg.textureLayers) app.textureSource.AddLayer(p);
        if (!app.textureSource.layers.empty()) app.availableTextures = app.textureSource.ListBaseNames({".mmp"});
        if (!cfg.databasePath.empty()) {
            std::snprintf(app.databasePathBuf, sizeof(app.databasePathBuf), "%s", cfg.databasePath.c_str());
            std::string err, itemErr;
            db::UnitDatabase newDb;
            db::ItemDatabase newItemDb;
            bool isXlsx = cfg.databasePath.size() > 5 && cfg.databasePath.substr(cfg.databasePath.size() - 5) == ".xlsx";
            bool ok, itemOk;
            if (isXlsx) {
                ok = db::LoadUnitsFromXlsx(cfg.databasePath, newDb, err);
                itemOk = db::LoadItemsFromXlsx(cfg.databasePath, newItemDb, itemErr);
            } else {
                std::ifstream f(cfg.databasePath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                ok = db::LoadUnitsFromRes(bytes, newDb, err);
                itemOk = db::LoadItemsFromRes(bytes, newItemDb, itemErr);
            }
            app.databaseLoaded = ok;
            app.databaseError = err;
            app.itemDatabaseLoaded = itemOk;
            if (ok) app.database = std::move(newDb);
            if (itemOk) app.itemDatabase = std::move(newItemDb);
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

        // Width is user-resizable (drag the window's right edge) so long part/unit
        // names and filter boxes can be given more room; height always tracks the
        // framebuffer, so only ImGuiWindowFlags_NoResize's Y-axis half is disabled
        // by re-asserting the height (not the width) every frame after Begin().
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(app.sidebarWidth, static_cast<float>(fbH) / io.DisplayFramebufferScale.y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, -1), ImVec2(800, -1));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
        app.sidebarWidth = ImGui::GetWindowWidth();
        if (ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen)) DrawSourcesPanel(app);
        if (ImGui::CollapsingHeader("Units", ImGuiTreeNodeFlags_DefaultOpen)) DrawUnitListPanel(app);
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) DrawModelControlsPanel(app);
        if (ImGui::CollapsingHeader("Parts", ImGuiTreeNodeFlags_DefaultOpen)) DrawPartsPanel(app);
        if (ImGui::CollapsingHeader("Weapon")) DrawWeaponEditorPanel(app);
        if (ImGui::CollapsingHeader("Armor")) DrawArmorEditorPanel(app);
        ImGui::Separator();
        ImGui::TextWrapped("%s", app.statusText.c_str());
        ImGui::End();

        // Bottom toolbar over the 3D viewport - a static reminder of the mouse
        // controls, since they're not otherwise discoverable in a viewer with
        // no menu bar.
        {
            const float barHeight = 28.0f;
            ImGui::SetNextWindowPos(ImVec2(app.sidebarWidth, static_cast<float>(fbH) / io.DisplayFramebufferScale.y - barHeight));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fbW) / io.DisplayFramebufferScale.x - app.sidebarWidth, barHeight));
            ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
            ImGui::TextUnformatted("Left-drag: Orbit    Right-drag: Pan    Scroll: Zoom");
            ImGui::End();
        }

        // Prepare (but don't render) animation-adjusted geometry before the 3D pass.
        // Only while actually playing - ApplyAnimationToRenderData only reproduces the
        // position track (see its own comment: rotation isn't implemented yet), so
        // applying it unconditionally just because a clip is selected would replace a
        // correct bind pose with an incomplete, visibly wrong one for every model that
        // happens to ship an .anm (most creatures do) even before the user asks to play.
        RebuildRenderData(app);
        if (app.playing) ApplyAnimationToRenderData(app);

        ImGui::Render();

        int viewportX = static_cast<int>(app.sidebarWidth * io.DisplayFramebufferScale.x);
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
        // Uses the raw GLFW scroll accumulator (see the callback in main()),
        // not io.MouseWheel, since the latter did not reliably drive zoom here.
        if (inViewport && g_rawScrollY != 0.0) {
            app.camera.Zoom(static_cast<float>(g_rawScrollY) * 0.3f);
        }
        g_rawScrollY = 0.0;

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
