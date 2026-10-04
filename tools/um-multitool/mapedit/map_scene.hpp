// The map editor's 3D view: the terrain of an .mpr (textured tiles, translucent water) and the
// objects of the loaded .mob files (their figures, or a marker for lights, particles, sounds and
// objects whose figure is missing). Fixed-function OpenGL 2 like the 3D Viewer; the terrain and
// every figure are compiled into display lists once.
//
// Placement follows ei_maper: an object's z is added to the terrain height under it (units stand on
// their lowest point), lights, particles and sounds are absolute, the rotation is the quaternion
// w,x,y,z, and figures are blended by the object's complection.
#pragma once

#include <GLFW/glfw3.h>

// OpenGL 1.4 names the shadow map uses (Windows' gl.h stops at 1.1).
#ifndef GL_DEPTH_TEXTURE_MODE
#define GL_DEPTH_TEXTURE_MODE 0x884B
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_COMPARE_R_TO_TEXTURE
#define GL_COMPARE_R_TO_TEXTURE 0x884E
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../viewer/camera.hpp"
#include "../viewer/dds_texture.hpp"
#include "../viewer/library.hpp"
#include "../viewer/model_loader.hpp"
#include "dress.hpp"
#include "mob_file.hpp"
#include "mpr_file.hpp"
#include "quest_file.hpp"

namespace mapedit {

struct MapViewOptions {
    bool terrain = true, water = true, objects = true, units = true, markers = true, exits = true;
    bool dressUnits = true; // units on the default0 placeholder wear their race's skin and their equipment
    bool poseUnits = true;  // figures with animations stand in their idle pose (cidle01, frame 0), not the T-pose
    bool animateUnits = false; // ... and play it (walking units in the patrol simulation: their walk clip)
    bool shadows = true;    // with the lighting on: the sun's shadows on the terrain
    bool navmesh = false;   // the game's walkability graph (AI_GRAPH), one node per 4 x 4 units
    int navLayer = 1;       // which of its 8 layers
    bool walkability = false; // the computed walkability grid the patrol simulation uses
    bool navCompare = false;  // where the map's graph and the one this editor builds disagree
    bool scriptAreas = true;  // the areas the scripts declare (AddRoundToArea / AddRectToArea)
    bool textured = true, wireframe = false;
    float background[3] = {0.42f, 0.55f, 0.68f};
};

// One figure as the map shows it: built for a template, texture, complection and body parts.
struct MapModel {
    bool tried = false, ok = false;
    GLuint list = 0;         // display list of the geometry
    GLuint texture = 0;      // 0 = untextured
    fig::Vec3 boundsMin{}, boundsMax{};
    std::vector<fig::Vec3> triangles; // the drawn geometry, three corners each (for picking)
    // A dressed unit (dress.hpp): one display list per texture instead of list/texture; overlays are
    // clothing textures drawn blended over the skin of the same parts.
    struct Layer { GLuint list = 0; GLuint texture = 0; bool overlay = false; };
    std::vector<Layer> layers;
    std::string dressed; // what dressed it, for the details
    std::string error;
    // An animated figure: what built it, so it can be baked again at another frame of its clip (BakeModel).
    struct AnimSource {
        std::shared_ptr<const LoadedModel> figure; // shared by every model of that figure (MapScene's cache)
        std::string clip;                // the clip it plays ("" none: at rest)
        fig::Vec3 constitution{0.5f, 0.5f, 0.5f};
        std::vector<std::string> parts;  // OBJ_BODYPARTS (lower case), empty: all
        dress::Dress dress;              // dressed units
        std::string texture;             // the others' texture
        float groundScale = 0;           // fig::GroundScale: stands on the ground (0: not computed / no clip)
    };
    std::shared_ptr<AnimSource> anim;
    int bakedFrame = -1;
    int frames = 0;                      // of its clip (0: not animated)
};

struct Ray { fig::Vec3 origin, dir; };

// A point of a unit's logic in the active map: a patrol point (look -1), one of its look points, or the
// guard place (place).
struct LogicPointRef {
    int object = -1, logic = -1, point = -1, look = -1;
    bool place = false;
    int trap = 0; // a magic trap's activation area (1) or cast point (2), `point` its index
    bool operator==(const LogicPointRef& o) const {
        return object == o.object && logic == o.logic && point == o.point && look == o.look && place == o.place && trap == o.trap;
    }
};

// Lighting mode: the colours of the map's lighting file at its time of day.
struct SceneLight {
    bool on = false;
    float sun[3] = {0.78f, 0.76f, 0.72f}, ambient[3] = {0.42f, 0.42f, 0.45f}, sky[3] = {0.42f, 0.55f, 0.68f};
    float sunDir[3] = {0.35f, -0.45f, 0.82f}; // towards the sun (world), from the time of day
    bool sunUp = true;                         // false at night: no shadows
};

class MapScene {
public:
    OrbitCamera camera;
    MapViewOptions options;
    int selectedFile = -1, selectedObject = -1; // the map of the selection and its main object (the details)
    std::vector<int> selection;                 // every selected object of selectedFile

    void ClearSelection() { selectedFile = selectedObject = -1; selection.clear(); logicPoints.clear(); }
    void Select(int file, int object) { selectedFile = file; selectedObject = object; selection.assign(1, object); logicPoints.clear(); }
    bool IsLogicPointSelected(const LogicPointRef& r) const { return std::find(logicPoints.begin(), logicPoints.end(), r) != logicPoints.end(); }
    bool IsSelected(int file, int object) const {
        return file == selectedFile && std::find(selection.begin(), selection.end(), object) != selection.end();
    }
    // Shift+click: adds the object, or takes it out when it is already selected.
    void Toggle(int file, int object) {
        if (file != selectedFile) { Select(file, object); return; }
        auto it = std::find(selection.begin(), selection.end(), object);
        if (it != selection.end()) selection.erase(it);
        else selection.push_back(object);
        selectedObject = selection.empty() ? -1 : (it == selection.end() ? object : selection.back());
        if (selection.empty()) selectedFile = -1;
    }
    // Keeps only the objects `keep` accepts (e.g. units in logic mode).
    template <typename F> void FilterSelection(F keep) {
        if (selectedFile < 0 || selectedFile >= static_cast<int>(maps_.size())) { ClearSelection(); return; }
        const auto& objects = maps_[selectedFile]->objects;
        selection.erase(std::remove_if(selection.begin(), selection.end(), [&](int oi) { return !keep(objects[oi]); }), selection.end());
        if (selection.empty()) ClearSelection();
        else if (std::find(selection.begin(), selection.end(), selectedObject) == selection.end()) selectedObject = selection.back();
    }
    int activeFile = 0;      // the map whose objects can be selected
    bool logicMode = false;  // units' behaviours shown, only units selectable
    std::vector<LogicPointRef> logicPoints; // selected points of the selected units' logic (logic mode)
    bool logicSelectedOnly = true;
    bool logicAlways = false; // outside logic mode: the selected units' logic shows too
    SceneLight light;
    const quest::Quest* quest = nullptr; // its deploy / exit areas are drawn
    // Lines drawn over everything (a move/scale's axis constraint): from, to, colour.
    struct Guide { fig::Vec3 a, b; float r, g, bl; };
    std::vector<Guide> guides;
    int modelsPending = 0;   // figures still to build (a few are built per frame)

    MapScene() {
        camera.pitchDeg = 50.0f;
        camera.distance = 120.0f;
    }

    // ---- terrain -------------------------------------------------------------------------------
    void SetTerrain(const Library& lib, const mpr::Map* map) {
        DropTerrain();
        terrain_ = map;
        if (!map) return;
        terrainTextures_.assign(static_cast<size_t>(std::max(map->textureCount, 1)), 0);
        texturesFound_ = 0;
        for (int i = 0; i < map->textureCount; ++i) {
            std::vector<uint8_t> bytes;
            mmp::Image image;
            std::string err;
            if (!lib.textures.ReadTexture(map->name + "00" + std::to_string(i), bytes) || !DecodeTextureFile(bytes, image, err)) continue;
            terrainTextures_[i] = Upload(image, true);
            texturesFound_++;
            textureSize_ = static_cast<int>(image.width);
        }
        BuildTerrainLists();
    }
    int TerrainTexturesFound() const { return texturesFound_; }
    GLuint TerrainTexture(int i) const { return i >= 0 && i < static_cast<int>(terrainTextures_.size()) ? terrainTextures_[i] : 0; }
    // After tiles or materials changed: the geometry again, the textures kept.
    void RebuildTerrainGeometry() {
        if (!terrain_) return;
        for (GLuint l : landLists_) if (l) glDeleteLists(l, 1);
        for (GLuint l : waterLists_) if (l) glDeleteLists(l, 1);
        BuildTerrainLists();
    }

    // ---- objects -------------------------------------------------------------------------------
    void SetMaps(const std::vector<const mob::File*>& maps, const std::vector<bool>& visible) {
        maps_ = maps;
        visible_ = visible;
    }

    // Drops every figure and texture, e.g. after the figure or texture sources changed.
    void DropModels() {
        for (auto& kv : models_) {
            if (kv.second.list) glDeleteLists(kv.second.list, 1);
            for (const MapModel::Layer& l : kv.second.layers) if (l.list) glDeleteLists(l.list, 1);
        }
        models_.clear();
        figures_.clear();
        for (auto& kv : objectTextures_) if (kv.second) glDeleteTextures(1, &kv.second);
        objectTextures_.clear();
    }

    void DropAll() {
        DropModels();
        DropTerrain();
    }

    // Where an object is drawn (its z on the ground), and its model once built.
    // Where units can walk: the game's tile map (navmesh_gen.hpp: 0.5 x 0.5 unit tiles, per AI layer, built
    // from the terrain and the objects exactly as the game builds it), for the shown navmesh layer. The Map
    // Editor fills it (walkBuilder); the patrol simulation routes on it with the game's step costs.
    struct WalkGrid {
        int w = 0, h = 0;
        float cell = 0.5f;
        std::vector<uint8_t> value;   // per tile: 0 blocked, 1 (hardest) .. 15 (easiest), see navgen::Generator
        std::vector<int> factor;      // a value's step cost factor (1024 = 1), from the generator
        bool Blocked(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h || value[static_cast<size_t>(y) * w + x] == 0; }
        float Cost(int x, int y) const { return factor.empty() ? 1.0f : factor[value[static_cast<size_t>(y) * w + x]] / 1024.0f; }
    } walk;
    std::function<void(WalkGrid&)> walkBuilder; // set by the Map Editor (it has the objects and figures)

    void BuildWalkGrid() {
        walk = WalkGrid{};
        ++walkBuilds_;
        if (terrain_ && walkBuilder) walkBuilder(walk);
    }

    // The patrol simulation's units: where they are and which way they face (radians about Z), in place of
    // the map's values while it runs.
    struct SimPose { float x, y, yaw; bool moving = false; };
    std::unordered_map<const mob::Object*, SimPose> simPoses;

    fig::Vec3 DrawPosition(const mob::Object& o, const MapModel* model) const {
        fig::Vec3 p{o.position.x, o.position.y, o.position.z};
        if (!simPoses.empty()) {
            auto it = simPoses.find(&o);
            if (it != simPoses.end()) { p.x = it->second.x; p.y = it->second.y; }
        }
        if (!mob::HasFigure(o.kind)) return p;
        if (terrain_) p.z += terrain_->HeightAt(p.x, p.y);
        if (o.kind == mob::Kind::Unit && model && model->ok) p.z -= model->boundsMin.z;
        return p;
    }

    // The part names of a figure, once a model of it was built (for choosing the body parts).
    const std::vector<std::string>* PartNames(const std::string& templ) const {
        auto it = partNames_.find(checksLower(templ));
        return it == partNames_.end() ? nullptr : &it->second;
    }

    const MapModel* ModelFor(const mob::Object& o) const {
        if (Walking(o)) { // its walk variant once built; the idle one meanwhile
            auto w = models_.find(ModelKey(o) + "|walk");
            if (w != models_.end() && w->second.ok) return &w->second;
        }
        auto it = models_.find(ModelKey(o));
        return it == models_.end() ? nullptr : &it->second;
    }
    // A unit moving in the patrol simulation (with animations on): drawn with its walk clip.
    bool Walking(const mob::Object& o) const {
        if (!options.poseUnits || !options.animateUnits || simPoses.empty()) return false;
        auto it = simPoses.find(&o);
        return it != simPoses.end() && it->second.moving;
    }
    // Plays the animated models: each at the frame of `seconds` (15 a second), baked again when it changes.
    // Models of the same look share their geometry, so such units move in step.
    void AnimateModels(const Library& lib, double seconds) {
        if (!options.poseUnits || !options.animateUnits) return;
        for (auto& [key, m] : models_) {
            if (!m.ok || !m.anim || m.frames <= 1) continue;
            const int frame = static_cast<int>(seconds * 15.0) % m.frames;
            if (frame != m.bakedFrame) BakeModel(lib, m, frame);
        }
    }

    // Frames the whole terrain (or the objects when there is none).
    void FrameAll() {
        float w = 64, h = 64, cx = 32, cy = 32;
        if (terrain_) {
            w = terrain_->Width(); h = terrain_->Height();
            cx = w * 0.5f; cy = h * 0.5f;
        } else {
            bool any = false;
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            for (const mob::File* f : maps_)
                for (const mob::Object& o : f->objects) {
                    if (!any) { x0 = x1 = o.position.x; y0 = y1 = o.position.y; any = true; }
                    x0 = std::min(x0, o.position.x); x1 = std::max(x1, o.position.x);
                    y0 = std::min(y0, o.position.y); y1 = std::max(y1, o.position.y);
                }
            if (any) { w = std::max(x1 - x0, 16.0f); h = std::max(y1 - y0, 16.0f); cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f; }
        }
        camera.targetX = cx;
        camera.targetY = cy;
        camera.targetZ = terrain_ ? terrain_->HeightAt(cx, cy) : 0.0f;
        camera.yawDeg = -90.0f;
        camera.pitchDeg = 55.0f;
        camera.distance = std::max(w, h) * 0.9f;
    }

    void FocusOn(const mob::Object& o) {
        fig::Vec3 p = DrawPosition(o, ModelFor(o));
        camera.targetX = p.x;
        camera.targetY = p.y;
        camera.targetZ = p.z;
        camera.distance = std::min(camera.distance, 25.0f);
    }

    // ---- drawing -------------------------------------------------------------------------------
    void Draw(const Library& lib, int x, int y, int width, int height) {
        width_ = width; height_ = height;
        glViewport(x, y, width, height);
        const float* bg = light.on ? light.sky : options.background;
        glClearColor(bg[0], bg[1], bg[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        const bool shadows = light.on && light.sunUp && options.shadows && terrain_ && !options.wireframe && RenderShadowMap(lib, x, y, width, height);
        glViewport(x, y, width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        float farZ = std::max(camera.distance * 4.0f, 400.0f) + (terrain_ ? terrain_->Width() + terrain_->Height() : 0.0f);
        Mat4 proj = Mat4::Perspective(kFov, height > 0 ? static_cast<float>(width) / height : 1.0f, std::max(camera.distance * 0.005f, 0.05f), farZ);
        Mat4 view = camera.ViewMatrix();
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(proj.m);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view.m);
        // Wireframe alone: lines only. With textures: the textured scene, then its edges drawn over it.
        const bool wireOver = options.wireframe && options.textured;
        glPolygonMode(GL_FRONT_AND_BACK, options.wireframe && !wireOver ? GL_LINE : GL_FILL);
        SetupSun();

        if (terrain_ && options.terrain) DrawTerrainBatch(landLists_, false);
        if (options.objects || options.units) DrawObjects(lib);
        if (shadows && options.terrain) DrawShadows();
        if (options.markers) DrawMarkers();
        if (terrain_ && options.water) DrawTerrainBatch(waterLists_, true);
        if (wireOver) {
            wirePass_ = true;
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-1.0f, -1.0f);
            if (terrain_ && options.terrain) DrawTerrainBatch(landLists_, false);
            if (options.objects || options.units) DrawObjects(lib);
            glDisable(GL_POLYGON_OFFSET_LINE);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            wirePass_ = false;
        }
        if (quest && options.exits) DrawExits(lib);
        if (options.navmesh) DrawNavmesh();
        if (options.walkability) DrawWalkability();
        if (options.navCompare) DrawNavCompare();
        if (options.scriptAreas) DrawScriptAreas();
        if (logicMode || logicAlways) DrawLogic();
        DrawTraps();
        DrawSelection();
        if (!guides.empty()) {
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_DEPTH_TEST);
            glLineWidth(2.0f);
            glBegin(GL_LINES);
            for (const Guide& g : guides) { glColor3f(g.r, g.g, g.bl); glVertex3f(g.a.x, g.a.y, g.a.z); glVertex3f(g.b.x, g.b.y, g.b.z); }
            glEnd();
            glLineWidth(1.0f);
            glEnable(GL_DEPTH_TEST);
        }
        view_ = view;
        proj_ = proj;

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
    }

    // ---- minimap --------------------------------------------------------------------------------
    // The map seen straight from above, like the game's <zone>map textures (made by ZoneView): a square
    // `size` x `size` image, the map in its top-left corner with its longer side filling the image, y = 0
    // at the top (as the game's minimaps have it), lit from the top-left for relief, the rest white. Rendered in tiles through the current framebuffer (fbW x fbH, which the
    // next frame paints over), so any size works. RGBA, top row first.
    // It is rendered at kSuper times the size and averaged down (kSuper x kSuper pixels per pixel), which
    // smooths textures and leaf edges the way ZoneView's minimaps are.
    static constexpr int kSuper = 4;

    bool RenderMinimap(const Library& lib, int size, bool objects, bool units, int fbW, int fbH, std::vector<uint8_t>& rgba) {
        if (!terrain_ || size < 16) return false;
        const float W = terrain_->Width(), H = terrain_->Height();
        const int big = size * kSuper;                    // the supersampled image's side
        const float scale = big / std::max(W, H);         // its pixels per world unit
        const int mapW = std::min(big, static_cast<int>(std::lround(W * scale))), mapH = std::min(big, static_cast<int>(std::lround(H * scale)));
        // Tiles are whole multiples of kSuper, so each output pixel's samples come from one tile.
        const int T = std::max(kSuper * 4, (std::min({fbW, fbH, 1024}) / kSuper) * kSuper);
        std::vector<uint32_t> sum(static_cast<size_t>(size) * size * 3, 0);
        const MapViewOptions saved = options;
        options.objects = objects;
        options.units = units;
        options.markers = false;
        options.wireframe = false;
        std::vector<uint8_t> tile(static_cast<size_t>(T) * T * 4);
        glEnable(GL_SCISSOR_TEST);
        for (int py0 = 0; py0 < mapH; py0 += T)
            for (int px0 = 0; px0 < mapW; px0 += T) {
                const int pw = std::min(T, mapW - px0), ph = std::min(T, mapH - py0);
                const float x0 = px0 / scale, x1 = (px0 + pw) / scale;
                // Image rows go with +y. GL reads rows from the bottom up, so an ordinary (unmirrored) projection
                // of y0..y1 already comes out in image order; mirroring it would flip the faces and their lighting.
                const float y0 = py0 / scale, y1 = (py0 + ph) / scale;
                glViewport(0, 0, pw, ph);
                glScissor(0, 0, pw, ph);
                glClearColor(1, 1, 1, 1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                glOrtho(x0, x1, y0, y1, -4000.0, 4000.0); // looking straight down
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                SetupSun();
                { // a low sun from the image's top-left (-x, -y) makes the relief read like the game's minimaps
                    const float dir[4] = {-0.5f, -0.5f, 0.7f, 0.0f};
                    const float diffuse[4] = {light.on ? light.sun[0] * 1.1f : 0.95f, light.on ? light.sun[1] * 1.1f : 0.93f, light.on ? light.sun[2] * 1.1f : 0.88f, 1.0f};
                    const float ambient[4] = {light.on ? light.ambient[0] : 0.32f, light.on ? light.ambient[1] : 0.32f, light.on ? light.ambient[2] : 0.34f, 1.0f};
                    glLightfv(GL_LIGHT0, GL_POSITION, dir);
                    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
                    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
                }
                DrawTerrainBatch(landLists_, false);
                alphaCut_ = 0.05f;
                if (objects || units) DrawObjects(lib);
                alphaCut_ = 0.4f;
                DrawTerrainBatch(waterLists_, true);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, tile.data());
                for (int r = 0; r < ph; ++r) // GL row r is y0 + r: supersampled row py0 + r
                    for (int c = 0; c < pw; ++c) {
                        const uint8_t* px = &tile[(static_cast<size_t>(r) * pw + c) * 4];
                        uint32_t* out = &sum[(static_cast<size_t>((py0 + r) / kSuper) * size + (px0 + c) / kSuper) * 3];
                        out[0] += px[0]; out[1] += px[1]; out[2] += px[2];
                    }
            }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_LIGHTING);
        options = saved;
        // Average each output pixel's samples; pixels past the map stay white.
        rgba.assign(static_cast<size_t>(size) * size * 4, 255);
        const int outW = (mapW + kSuper - 1) / kSuper, outH = (mapH + kSuper - 1) / kSuper;
        for (int y = 0; y < outH; ++y)
            for (int x = 0; x < outW; ++x) {
                const int sx = std::min(kSuper, mapW - x * kSuper), sy = std::min(kSuper, mapH - y * kSuper);
                const uint32_t n = static_cast<uint32_t>(sx * sy);
                const uint32_t* in = &sum[(static_cast<size_t>(y) * size + x) * 3];
                uint8_t* o = &rgba[(static_cast<size_t>(y) * size + x) * 4];
                for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>((in[k] + n / 2) / n);
            }
        return true;
    }

    // ---- picking -------------------------------------------------------------------------------
    // The ray under a point of the viewport, as fractions of its size from its top-left.
    Ray RayAt(float px, float py) const {
        float ex, ey, ez;
        camera.EyePosition(ex, ey, ez);
        fig::Vec3 eye{ex, ey, ez}, target{camera.targetX, camera.targetY, camera.targetZ};
        fig::Vec3 f = Normalize(target - eye);
        float yaw = camera.yawDeg * kPi / 180.0f, pitch = camera.pitchDeg * kPi / 180.0f;
        fig::Vec3 up{-std::cos(yaw) * std::sin(pitch), -std::sin(yaw) * std::sin(pitch), std::cos(pitch)};
        fig::Vec3 s = Normalize(Cross(f, up));
        fig::Vec3 u = Cross(s, f);
        float aspect = height_ > 0 ? static_cast<float>(width_) / height_ : 1.0f;
        float t = std::tan(kFov * kPi / 360.0f);
        float nx = (2.0f * px - 1.0f) * t * aspect, ny = (1.0f - 2.0f * py) * t;
        return {eye, Normalize(f + s * nx + u * ny)};
    }

    // Where a world point shows in the viewport, as fractions of its size from the top-left (false when
    // behind the camera). Uses the matrices of the last Draw.
    bool Project(const fig::Vec3& p, float& fx, float& fy) const {
        const float* v = view_.m;
        const float* pr = proj_.m;
        float e[4] = {v[0] * p.x + v[4] * p.y + v[8] * p.z + v[12], v[1] * p.x + v[5] * p.y + v[9] * p.z + v[13],
                      v[2] * p.x + v[6] * p.y + v[10] * p.z + v[14], 1.0f};
        float cx = pr[0] * e[0] + pr[8] * e[2], cy = pr[5] * e[1] + pr[9] * e[2], cw = -e[2];
        if (cw <= 0.01f) return false;
        fx = (cx / cw + 1.0f) * 0.5f;
        fy = (1.0f - cy / cw) * 0.5f;
        return true;
    }

    // The ground height at a point (0 without terrain).
    float Ground(float x, float y) const { return terrain_ ? terrain_->HeightAt(x, y) : 0.0f; }

    // The objects of the active map whose position shows inside a viewport rectangle (fractions of its
    // size), among those that can be selected (visible; only units in logic mode).
    std::vector<int> ObjectsInRect(float x0, float y0, float x1, float y1) const {
        std::vector<int> out;
        if (activeFile < 0 || activeFile >= static_cast<int>(maps_.size())) return out;
        if (activeFile < static_cast<int>(visible_.size()) && !visible_[activeFile]) return out;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        const auto& objects = maps_[activeFile]->objects;
        for (size_t oi = 0; oi < objects.size(); ++oi) {
            const mob::Object& o = objects[oi];
            if (!Shown(o) || (logicMode && o.kind != mob::Kind::Unit)) continue;
            const MapModel* m = ModelFor(o);
            fig::Vec3 p = DrawPosition(o, m);
            if (m && m->ok) p = p + fig::QuatRotate(Rotation(o), (m->boundsMin + m->boundsMax) * 0.5f);
            float fx, fy;
            if (Project(p, fx, fy) && fx >= x0 && fx <= x1 && fy >= y0 && fy <= y1) out.push_back(static_cast<int>(oi));
        }
        return out;
    }

    // The object under a viewport point: nearest hit of the visible objects' bounding spheres, in the
    // active map only (and only units in logic mode).
    // Where a ray first meets an object: its figure's triangles when it has one (the bounding sphere only
    // rules objects out quickly), else a small sphere (markers). false when it misses.
    bool RayHitsObject(const mob::Object& o, const MapModel* m, const Ray& r, float& t) const {
        const fig::Vec3 p = DrawPosition(o, m);
        fig::Vec3 c = p;
        float radius = 0.6f;
        const bool mesh = m && m->ok && !m->triangles.empty();
        if (mesh) {
            c = p + fig::QuatRotate(Rotation(o), (m->boundsMin + m->boundsMax) * 0.5f);
            fig::Vec3 d = m->boundsMax - m->boundsMin;
            radius = std::max(0.5f * std::sqrt(Dot(d, d)), 0.3f);
        }
        const fig::Vec3 oc = c - r.origin;
        const float along = Dot(oc, r.dir);
        const fig::Vec3 closest = r.origin + r.dir * along - c;
        if (Dot(closest, closest) > radius * radius || along < -radius) return false;
        if (!mesh) { t = along; return along >= 0; }
        // The ray in the figure's own space, then Moller-Trumbore on each triangle (both sides).
        const fig::Quat q = Rotation(o), inv{q.w, -q.x, -q.y, -q.z};
        const fig::Vec3 ro = fig::QuatRotate(inv, r.origin - p), rd = fig::QuatRotate(inv, r.dir);
        float best = 1e30f;
        const auto& tri = m->triangles;
        for (size_t i = 0; i + 2 < tri.size(); i += 3) {
            const fig::Vec3 e1 = tri[i + 1] - tri[i], e2 = tri[i + 2] - tri[i];
            const fig::Vec3 h = Cross(rd, e2);
            const float a = Dot(e1, h);
            if (std::fabs(a) < 1e-9f) continue;
            const float f = 1.0f / a;
            const fig::Vec3 sv = ro - tri[i];
            const float u = f * Dot(sv, h);
            if (u < 0.0f || u > 1.0f) continue;
            const fig::Vec3 qv = Cross(sv, e1);
            const float v = f * Dot(rd, qv);
            if (v < 0.0f || u + v > 1.0f) continue;
            const float tt = f * Dot(e2, qv);
            if (tt > 0.0f && tt < best) best = tt;
        }
        if (best >= 1e30f) return false;
        t = best;
        return true;
    }

    bool Pick(float px, float py, int& fileOut, int& objectOut) const {
        Ray r = RayAt(px, py);
        float best = 1e30f;
        bool hit = false;
        for (size_t fi = 0; fi < maps_.size(); ++fi) {
            if (static_cast<int>(fi) != activeFile) continue;
            if (fi < visible_.size() && !visible_[fi]) continue;
            const auto& objects = maps_[fi]->objects;
            for (size_t oi = 0; oi < objects.size(); ++oi) {
                const mob::Object& o = objects[oi];
                if (!Shown(o)) continue;
                if (logicMode && o.kind != mob::Kind::Unit) continue;
                float t = 0.0f;
                if (!RayHitsObject(o, ModelFor(o), r, t)) continue;
                if (t < best) { best = t; fileOut = static_cast<int>(fi); objectOut = static_cast<int>(oi); hit = true; }
            }
        }
        return hit;
    }

    // The centre of the visible object under a viewport point, in any map (for the orbit pivot).
    bool PickAny(float px, float py, fig::Vec3& centre) const {
        Ray r = RayAt(px, py);
        float best = 1e30f;
        bool hit = false;
        for (size_t fi = 0; fi < maps_.size(); ++fi) {
            if (fi < visible_.size() && !visible_[fi]) continue;
            for (const mob::Object& o : maps_[fi]->objects) {
                if (!Shown(o)) continue;
                float t = 0.0f;
                if (!RayHitsObject(o, ModelFor(o), r, t) || t >= best) continue;
                best = t;
                centre = r.origin + r.dir * t; // the point hit, which the camera turns around
                hit = true;
            }
        }
        return hit;
    }

    // Where the ray under a viewport point meets the terrain (marching, then refining).
    bool GroundAt(float px, float py, fig::Vec3& out) const {
        if (!terrain_) return false;
        Ray r = RayAt(px, py);
        float step = std::max(camera.distance / 400.0f, 0.1f), t = 0.0f;
        float maxT = camera.distance * 6.0f + terrain_->Width() + terrain_->Height();
        auto above = [&](float tt) {
            fig::Vec3 p = r.origin + r.dir * tt;
            return p.z - terrain_->HeightAt(p.x, p.y);
        };
        float prev = above(0.0f);
        for (t = step; t < maxT; t += step) {
            float cur = above(t);
            if (prev > 0 && cur <= 0) {
                float lo = t - step, hi = t;
                for (int i = 0; i < 20; ++i) { float mid = (lo + hi) * 0.5f; (above(mid) > 0 ? lo : hi) = mid; }
                out = r.origin + r.dir * hi;
                fig::Vec3 p = out;
                return p.x >= 0 && p.y >= 0 && p.x <= terrain_->Width() && p.y <= terrain_->Height();
            }
            prev = cur;
        }
        return false;
    }

    // The objects whose figure cannot be shown (not found or not readable), once their models were tried.
    struct Missing { int file = 0, object = 0; std::string figure, error; };
    std::vector<Missing> MissingFigures() const {
        std::vector<Missing> out;
        for (size_t fi = 0; fi < maps_.size(); ++fi)
            for (size_t oi = 0; oi < maps_[fi]->objects.size(); ++oi) {
                const mob::Object& o = maps_[fi]->objects[oi];
                if (!mob::HasFigure(o.kind)) continue;
                if (o.templ.empty()) { out.push_back({static_cast<int>(fi), static_cast<int>(oi), "", "no figure named"}); continue; }
                const auto it = models_.find(ModelKey(o));
                if (it != models_.end() && it->second.tried && !it->second.error.empty())
                    out.push_back({static_cast<int>(fi), static_cast<int>(oi), o.templ, it->second.error});
            }
        return out;
    }

    // Builds up to `budget` missing figures; call once per frame.
    void BuildSomeModels(const Library& lib, int budget) {
        modelsPending = 0;
        for (size_t fi = 0; fi < maps_.size(); ++fi)
            for (const mob::Object& o : maps_[fi]->objects) {
                if (!mob::HasFigure(o.kind) || o.templ.empty()) continue;
                std::string key = ModelKey(o);
                const bool walking = Walking(o);
                if (walking && !models_.count(key + "|walk") && budget > 0) { --budget; BuildModel(lib, o, models_[key + "|walk"], true); }
                auto it = models_.find(key);
                if (it != models_.end()) continue;
                if (budget <= 0) { ++modelsPending; continue; }
                --budget;
                BuildModel(lib, o, models_[key]);
            }
    }

private:
    static constexpr float kPi = 3.14159265f;
    static constexpr float kFov = 50.0f;

    const mpr::Map* terrain_ = nullptr;
    std::vector<GLuint> terrainTextures_;
    int texturesFound_ = 0, textureSize_ = 512;
    std::vector<GLuint> landLists_, waterLists_; // one per terrain texture
    std::vector<const mob::File*> maps_;
    std::vector<bool> visible_;
    std::map<std::string, MapModel> models_;
    std::map<std::string, std::shared_ptr<const LoadedModel>> figures_; // loaded figures by name (lower case)
    std::map<std::string, GLuint> objectTextures_;
    std::map<std::string, std::vector<std::string>> partNames_; // by lower-case figure name
    bool wirePass_ = false; // drawing the wireframe over the textured scene
    bool depthPass_ = false; // drawing the scene from the sun for its shadow map
    GLuint walkList_ = 0;    // the walkability overlay's display list, and the grid build it shows
    unsigned walkBuilds_ = 0, walkListBuild_ = 0;
    // The cells whose centre is inside a triangle (seen from above), and those along its edges.
    GLuint navList_ = 0;
    GLuint cmpList_ = 0;
    std::string cmpKey_;     // the navmesh's display list, and what it was built for
    std::string navKey_;
    GLuint shadowTex_ = 0;
    int shadowSize_ = 0;
    float shadowRadius_ = 0, shadowFar_ = 0;
    float shadowBox_[6] = {};   // the sun's orthographic box: left, right, bottom, top, near, far
    Mat4 shadowView_{};
    int width_ = 1, height_ = 1;
    float alphaCut_ = 0.4f; // cut-out alpha for figures (lower for the minimap: full tree canopies, like ZoneView's)
    Mat4 view_ = Mat4::Identity(), proj_ = Mat4::Identity();

    static fig::Vec3 Cross(const fig::Vec3& a, const fig::Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    static float Dot(const fig::Vec3& a, const fig::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static fig::Vec3 Normalize(const fig::Vec3& v) {
        float l = std::sqrt(Dot(v, v));
        return l > 1e-9f ? v * (1.0f / l) : v;
    }
    fig::Quat Rotation(const mob::Object& o) const {
        if (!simPoses.empty()) {
            auto it = simPoses.find(&o);
            if (it != simPoses.end()) return fig::Quat{std::cos(it->second.yaw / 2), 0.0f, 0.0f, std::sin(it->second.yaw / 2)};
        }
        return fig::QuatNormalize(fig::Quat{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]});
    }

    bool Shown(const mob::Object& o) const {
        if (!mob::HasFigure(o.kind)) return options.markers;
        return o.kind == mob::Kind::Unit ? options.units : options.objects;
    }

    static GLuint Upload(const mmp::Image& image, bool clamp) {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // GL 1.2's CLAMP_TO_EDGE: the Windows GL headers stop at 1.1, so by value.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? 0x812F : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? 0x812F : GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, image.rgba.data());
        return id;
    }

    void DropTerrain() {
        for (GLuint l : landLists_) if (l) glDeleteLists(l, 1);
        for (GLuint l : waterLists_) if (l) glDeleteLists(l, 1);
        landLists_.clear();
        waterLists_.clear();
        for (GLuint t : terrainTextures_) if (t) glDeleteTextures(1, &t);
        terrainTextures_.clear();
        terrain_ = nullptr;
        texturesFound_ = 0;
    }

    // Tile UVs as in ei_maper's CTile::generateDrawVertexData: tile i of a texture spans u in
    // [(i%8)/8, +1/8] and v in [(7 - i/8)/8, +1/8]; the rotation turns which corner goes where.
    void TileUv(int tileInTexture, int rotation, int row, int col, float& u, float& v) const {
        int r = row, c = col;
        switch (rotation & 3) {
        case 1: r = col; c = 2 - row; break;
        case 2: r = 2 - row; c = 2 - col; break;
        case 3: r = 2 - col; c = row; break;
        default: break;
        }
        const float inset = 0.5f / std::max(textureSize_, 1); // keep neighbouring tiles from bleeding in
        float u0 = (tileInTexture % 8) / 8.0f + inset, u1 = (tileInTexture % 8 + 1) / 8.0f - inset;
        float v0 = (7 - tileInTexture / 8) / 8.0f + inset, v1 = (8 - tileInTexture / 8) / 8.0f - inset;
        u = u0 + (u1 - u0) * c * 0.5f;
        v = v1 - (v1 - v0) * r * 0.5f;
    }

    void BuildTerrainLists() {
        const mpr::Map& m = *terrain_;
        const int textures = std::max(m.textureCount, 1);
        landLists_.assign(static_cast<size_t>(textures), 0);
        waterLists_.assign(static_cast<size_t>(textures), 0);
        const float k = m.maxZ / 65535.0f;
        for (int water = 0; water < 2; ++water) {
            for (int tex = 0; tex < textures; ++tex) {
                GLuint list = glGenLists(1);
                glNewList(list, GL_COMPILE);
                glBegin(GL_TRIANGLES);
                for (int sy = 0; sy < m.sectorsY; ++sy)
                    for (int sx = 0; sx < m.sectorsX; ++sx) {
                        const mpr::Sector* s = m.At(sx, sy);
                        if (!s || (water && !s->water)) continue;
                        for (int row = 0; row < 16; ++row)
                            for (int col = 0; col < 16; ++col) {
                                uint16_t packed = water ? s->waterTiles[row][col] : s->landTiles[row][col];
                                if (water) {
                                    int mat = s->waterMaterial[row][col];
                                    if (mat < 0) continue;
                                    if (mat < static_cast<int>(m.materials.size())) {
                                        const mpr::Material& wm = m.materials[mat];
                                        glColor4f(0.55f + wm.r * 0.45f, 0.55f + wm.g * 0.45f, 0.55f + wm.b * 0.45f, std::min(std::max(wm.a, 0.35f), 0.85f));
                                    } else {
                                        glColor4f(0.7f, 0.8f, 0.9f, 0.6f);
                                    }
                                }
                                int texture = std::min((packed >> 6) & 255, textures - 1);
                                if (texture != tex) continue;
                                int tile = packed & 63, rotation = (packed >> 14) & 3;
                                const mpr::Vertex (&verts)[33][33] = water ? s->waterVerts : s->land;
                                float px[3][3], py[3][3], pz[3][3], n[3][3][3], tu[3][3], tv[3][3];
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 3; ++c) {
                                        const mpr::Vertex& vx = verts[row * 2 + r][col * 2 + c];
                                        px[r][c] = sx * 32.0f + col * 2 + c + vx.xOffset / 254.0f;
                                        py[r][c] = sy * 32.0f + row * 2 + r + vx.yOffset / 254.0f;
                                        pz[r][c] = vx.z * k;
                                        mpr::Normal(vx.packedNormal, n[r][c]);
                                        TileUv(tile, rotation, r, c, tu[r][c], tv[r][c]);
                                    }
                                auto vert = [&](int r, int c) {
                                    glTexCoord2f(tu[r][c], tv[r][c]);
                                    glNormal3f(n[r][c][0], n[r][c][1], n[r][c][2]);
                                    glVertex3f(px[r][c], py[r][c], pz[r][c]);
                                };
                                for (int r = 0; r < 2; ++r)
                                    for (int c = 0; c < 2; ++c) {
                                        vert(r, c); vert(r, c + 1); vert(r + 1, c + 1);
                                        vert(r, c); vert(r + 1, c + 1); vert(r + 1, c);
                                    }
                            }
                    }
                glEnd();
                glEndList();
                (water ? waterLists_ : landLists_)[tex] = list;
            }
        }
    }

    // ---- shadows (lighting on) -------------------------------------------------------------------
    // A shadow map: the terrain and the figures seen from the sun (an orthographic view around the camera's
    // target, alpha-tested so leaves cast leaf shapes), their depth copied into a depth texture. The
    // terrain is then drawn once more with that texture projected on it, darkening what the sun does not
    // reach (the texture's compare gives 1 there). Figures cast shadows but do not receive them.

    bool RenderShadowMap(const Library& lib, int x, int y, int width, int height) {
        int size = 256;
        while (size * 2 <= std::min(std::min(width, height), 2048)) size *= 2;
        if (size < 256 || std::min(width, height) < size) return false;
        const float* dir = light.sunDir; // as SetupSun lights, from the time of day
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        const float sx = dir[0] / len, sy = dir[1] / len, sz = dir[2] / len;
        // Steady shadows: the sun looks from a fixed place (not from the camera's target), the box it covers
        // grows in steps with the zoom (32, 64 ... 512 units), and it moves in whole shadow-map pixels. A box
        // that followed the camera smoothly would slide the map's pixel grid over the ground and make the
        // shadows' edges shimmer as the camera moves.
        float radius = 32.0f;
        while (radius < camera.distance * 1.3f && radius < 512.0f) radius *= 2.0f;
        shadowRadius_ = radius;
        const float sunDistance = 4000.0f; // ("far" is a macro in Windows' headers)
        shadowView_ = Mat4::LookAt(sx * sunDistance, sy * sunDistance, sz * sunDistance, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        const float* v = shadowView_.m; // the target in the sun's view
        const float lx = v[0] * camera.targetX + v[4] * camera.targetY + v[8] * camera.targetZ + v[12];
        const float ly = v[1] * camera.targetX + v[5] * camera.targetY + v[9] * camera.targetZ + v[13];
        const float lz = v[2] * camera.targetX + v[6] * camera.targetY + v[10] * camera.targetZ + v[14];
        const float texel = 2.0f * radius / size;
        const float cx = std::floor(lx / texel) * texel, cy = std::floor(ly / texel) * texel;
        const float depth = radius * 2.0f + (terrain_ ? terrain_->maxZ : 0.0f) + 50.0f;
        shadowBox_[0] = cx - radius; shadowBox_[1] = cx + radius;
        shadowBox_[2] = cy - radius; shadowBox_[3] = cy + radius;
        shadowBox_[4] = std::max(1.0f, -lz - depth); shadowBox_[5] = -lz + depth;
        shadowFar_ = shadowBox_[5];
        glViewport(x, y, size, size);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(shadowBox_[0], shadowBox_[1], shadowBox_[2], shadowBox_[3], shadowBox_[4], shadowBox_[5]);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(shadowView_.m);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(4.0f, 12.0f); // the surfaces a little behind themselves: no self-shadowing stripes
        depthPass_ = true;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        if (options.terrain) for (GLuint l : landLists_) if (l) glCallList(l);
        if (options.objects || options.units) DrawObjects(lib);
        depthPass_ = false;
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (!shadowTex_ || shadowSize_ != size) {
            if (!shadowTex_) glGenTextures(1, &shadowTex_);
            glBindTexture(GL_TEXTURE_2D, shadowTex_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            const float border[4] = {1, 1, 1, 1}; // outside the map: nothing in front, no shadow
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_GREATER); // 1 where something is nearer the sun
            glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE); // the result as a grey (1: in shadow)
            glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, x, y, size, size, 0);
            shadowSize_ = size;
        } else {
            glBindTexture(GL_TEXTURE_2D, shadowTex_);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, size, size);
        }
        return glGetError() == GL_NO_ERROR;
    }

    // The terrain again, darkened where the shadow map says the sun is hidden.
    void DrawShadows() {
        if (!shadowTex_) return;
        // World coordinates as texture coordinates: eye-linear planes given while the modelview holds the
        // camera's view. The texture matrix takes them into the shadow map (0..1).
        const float planes[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        const GLenum coords[4] = {GL_S, GL_T, GL_R, GL_Q};
        const GLenum gens[4] = {GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q};
        for (int i = 0; i < 4; ++i) {
            glTexGeni(coords[i], GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            glTexGenfv(coords[i], GL_EYE_PLANE, planes[i]);
            glEnable(gens[i]);
        }
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glTranslatef(0.5f, 0.5f, 0.5f);
        glScalef(0.5f, 0.5f, 0.5f);
        glOrtho(shadowBox_[0], shadowBox_[1], shadowBox_[2], shadowBox_[3], shadowBox_[4], shadowBox_[5]);
        glMultMatrixf(shadowView_.m);
        glMatrixMode(GL_MODELVIEW);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, shadowTex_);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glDisable(GL_LIGHTING);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // the scene times (1 - strength * in shadow)
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        // Darker under a brighter sun; the ambient light keeps shadows from going black.
        const float sun = (light.sun[0] + light.sun[1] + light.sun[2]) / 3.0f;
        const float k = std::min(0.6f, std::max(0.15f, sun * 0.6f));
        glColor4f(k, k, k, 1.0f);
        for (GLuint l : landLists_) if (l) glCallList(l);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        for (GLenum g : gens) glDisable(g);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
    }

    void SetupSun() const {
        glEnable(GL_LIGHT0);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
        glEnable(GL_NORMALIZE);
        const float sun[4] = {light.sunDir[0], light.sunDir[1], light.sunDir[2], 0.0f}; // world direction (the modelview holds the view)
        const float diffuse[4] = {light.sun[0], light.sun[1], light.sun[2], 1.0f};
        const float ambient[4] = {light.ambient[0], light.ambient[1], light.ambient[2], 1.0f};
        glLightfv(GL_LIGHT0, GL_POSITION, sun);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    }

    void DrawTerrainBatch(const std::vector<GLuint>& lists, bool water) {
        if (wirePass_) { // the edges over the textured terrain
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.05f, 0.05f, 0.05f, 1.0f);
            for (GLuint l : lists) if (l) glCallList(l);
            return;
        }
        glEnable(GL_LIGHTING);
        if (water) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glColor4f(1, 1, 1, 1);
        }
        for (size_t i = 0; i < lists.size(); ++i) {
            GLuint tex = i < terrainTextures_.size() ? terrainTextures_[i] : 0;
            bool textured = options.textured && tex;
            if (textured) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); }
            else {
                glDisable(GL_TEXTURE_2D);
                if (!water) glColor4f(0.55f, 0.6f, 0.45f, 1.0f);
            }
            if (lists[i]) glCallList(lists[i]);
        }
        glDisable(GL_TEXTURE_2D);
        if (water) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }

    static std::string ModelKey(const mob::Object& o) {
        char comp[64];
        std::snprintf(comp, sizeof(comp), "|%.2f|%.2f|%.2f|", o.complection.x, o.complection.y, o.complection.z);
        std::string key = checksLower(o.templ) + "|" + checksLower(o.primTexture) + comp;
        for (const std::string& p : o.bodyParts) key += checksLower(p) + ",";
        if (o.kind == mob::Kind::Unit && checksLower(o.primTexture) == "default0") { // dressed: by what it wears
            key += "|" + o.prototype + "|" + o.parentTemplate;
            for (const mob::ItemList& l : o.lists)
                if (l.type == mob::kUnitWeapons || l.type == mob::kUnitArmors) for (const std::string& e : l.entries) key += "|" + e;
        }
        return key;
    }
    static std::string checksLower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // "skin|item|item...": the first picture, each next one painted over it by its alpha (sampled at the
    // first one's size when they differ). False when the first cannot be read.
    static bool ComposeTexture(const Library& lib, const std::string& list, mmp::Image& out) {
        size_t start = 0;
        bool first = true;
        while (start <= list.size()) {
            size_t bar = list.find('|', start);
            if (bar == std::string::npos) bar = list.size();
            const std::string name = list.substr(start, bar - start);
            start = bar + 1;
            std::vector<uint8_t> bytes;
            mmp::Image img;
            std::string err;
            if (name.empty() || !lib.textures.ReadTexture(name, bytes) || !DecodeTextureFile(bytes, img, err)) {
                if (first) return false;
                continue;
            }
            if (first) { out = std::move(img); first = false; continue; }
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x) {
                    const uint32_t sx = x * img.width / out.width, sy = y * img.height / out.height;
                    const uint8_t* s = img.rgba.data() + (static_cast<size_t>(sy) * img.width + sx) * 4;
                    uint8_t* d = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
                    const int a = s[3];
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((s[c] * a + d[c] * (255 - a)) / 255);
                    d[3] = static_cast<uint8_t>(std::max<int>(d[3], a));
                }
        }
        return !first;
    }

public:
    // A texture as a GL texture (loaded once): the Map Editor's texture pickers preview with it.
    GLuint TexturePreview(const Library& lib, const std::string& name) { return ObjectTexture(lib, name); }
private:
    GLuint ObjectTexture(const Library& lib, const std::string& name) {
        std::string key = checksLower(name);
        auto it = objectTextures_.find(key);
        if (it != objectTextures_.end()) return it->second;
        GLuint id = 0;
        std::vector<uint8_t> bytes;
        mmp::Image image;
        std::string err;
        if (name.rfind("compose:", 0) == 0) { // a dressed unit's body: pictures painted over one another
            if (ComposeTexture(lib, name.substr(8), image)) id = Upload(image, false);
            objectTextures_[key] = id;
            return id;
        }
        // "default0" (a stripe pattern) is kept on purpose: humanoid units use it until the game dresses them
        // from their equipment, and the stripes make such units easy to spot.
        if (!name.empty() && lib.textures.ReadTexture(name, bytes) && DecodeTextureFile(bytes, image, err))
            id = Upload(image, false);
        objectTextures_[key] = id;
        return id;
    }

    // `walk`: the variant that plays the figure's walk clip (units moving in the patrol simulation).
    void BuildModel(const Library& lib, const mob::Object& o, MapModel& out, bool walk = false) {
        out.tried = true;
        std::shared_ptr<const LoadedModel>& cached = figures_[checksLower(o.templ)];
        if (!cached) {
            auto fresh = std::make_shared<LoadedModel>();
            LoadNamedModel(lib.figures, o.templ, *fresh);
            cached = fresh;
        }
        if (!cached->ok) {
            out.error = cached->error.empty() ? "figure not found" : cached->error;
            return;
        }
        const LoadedModel& loaded = *cached;
        auto src = std::make_shared<MapModel::AnimSource>();
        for (const std::string& p : o.bodyParts) src->parts.push_back(checksLower(p));
        std::vector<std::string>& names = partNames_[checksLower(o.templ)];
        names.clear();
        for (const fig::ModelPart& part : loaded.model.parts) names.push_back(part.name);
        src->constitution = {std::min(std::max(o.complection.x, 0.0f), 1.0f), std::min(std::max(o.complection.y, 0.0f), 1.0f),
                             std::min(std::max(o.complection.z, 0.0f), 1.0f)};
        src->figure = cached;
        // Animated figures (units...): their idle clip (or the walk clip for `walk`); at rest without one.
        if (options.poseUnits) {
            const fig::AnimClip* c = walk ? WalkClip(loaded.animClips) : fig::IdleClip(loaded.animClips);
            for (const auto& kv : loaded.animClips) if (&kv.second == c) src->clip = kv.first;
        }
        src->dress = options.dressUnits ? dress::Resolve(lib, o) : dress::Dress{};
        src->texture = o.primTexture;
        if (!src->clip.empty()) src->groundScale = fig::GroundScale(loaded.model, src->constitution);
        out.anim = src;
        auto ci = loaded.animClips.find(src->clip);
        out.frames = ci != loaded.animClips.end() ? static_cast<int>(ci->second.FrameCount()) : 0;
        BakeModel(lib, out, 0);
        if (out.frames <= 1) out.anim.reset(); // nothing to play: no need to keep the source
        if (loaded.animClips.empty()) figures_.erase(checksLower(o.templ)); // a figure without animations: not kept
    }

    // The walk clip: the "cwalk..." one animating the most parts (some only move the legs).
    static const fig::AnimClip* WalkClip(const std::map<std::string, fig::AnimClip>& clips) {
        const fig::AnimClip* best = nullptr;
        for (const auto& kv : clips)
            if (kv.first.compare(0, 5, "cwalk") == 0 && (!best || kv.second.bones.size() > best->bones.size())) best = &kv.second;
        return best;
    }

    // (Re)builds a model's display lists at a frame of its clip (P + W * v per vertex: fig::PoseModel; the hips
    // where the rest pose has them).
    void BakeModel(const Library& lib, MapModel& out, int frame) {
        const MapModel::AnimSource& src = *out.anim;
        if (out.list) { glDeleteLists(out.list, 1); out.list = 0; }
        for (const MapModel::Layer& l : out.layers) if (l.list) glDeleteLists(l.list, 1);
        out.layers.clear();
        out.triangles.clear();
        out.bakedFrame = frame;
        const std::map<std::string, fig::AnimClip>& clips = src.figure->animClips;
        auto ci = clips.find(src.clip);
        const fig::Model& model = src.figure->model;
        const std::vector<fig::PartPose> pose =
            fig::PoseModel(model, ci != clips.end() ? &ci->second : nullptr, static_cast<float>(frame), src.constitution, src.groundScale);
        const fig::Vec3& constitution = src.constitution;
        bool first = true;
        auto grow = [&](const fig::Vec3& p) {
            out.triangles.push_back(p);
            if (first) { out.boundsMin = out.boundsMax = p; first = false; }
            out.boundsMin = {std::min(out.boundsMin.x, p.x), std::min(out.boundsMin.y, p.y), std::min(out.boundsMin.z, p.z)};
            out.boundsMax = {std::max(out.boundsMax.x, p.x), std::max(out.boundsMax.y, p.y), std::max(out.boundsMax.z, p.z)};
        };
        if (src.dress.on) { // a dressed unit: the parts it shows, grouped by texture, the clothing overlays over them
            const dress::Dress& d = src.dress;
            struct Vtx { fig::Vec3 p, n; fig::Vec2 uv; };
            std::map<std::pair<std::string, bool>, std::vector<Vtx>> buckets;
            for (size_t pi = 0; pi < model.parts.size(); ++pi) {
                const fig::ModelPart& part = model.parts[pi];
                if (!dress::PartShown(d, model, part)) continue;
                const std::string tex = dress::PartTexture(d, model, part);
                auto ov = d.overlay.find(dress::Lower(part.name));
                std::vector<Vtx>& base = buckets[{tex, false}];
                std::vector<Vtx>* over = ov != d.overlay.end() ? &buckets[{ov->second, true}] : nullptr;
                const fig::FigureMesh& mesh = part.mesh;
                for (uint16_t index : mesh.indices) {
                    if (index >= mesh.vertexComponents.size()) continue;
                    const fig::VertComponent& vc = mesh.vertexComponents[index];
                    Vtx v{pose[pi].p + fig::QRotate(pose[pi].w, mesh.BlendedPosition(index, constitution)),
                          fig::QRotate(pose[pi].w, vc.normalIndex < mesh.normals.size() ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1}),
                          vc.uvIndex < mesh.uvs.size() ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0}};
                    base.push_back(v);
                    if (over) over->push_back(v);
                    grow(v.p);
                }
            }
            out.ok = !first;
            if (!out.ok) { out.error = "no geometry"; return; }
            for (bool overlay : {false, true}) // the skin and meshes first, the clothing over them
                for (auto& [key, vtx] : buckets) {
                    if (key.second != overlay || vtx.empty()) continue;
                    MapModel::Layer layer;
                    layer.overlay = overlay;
                    layer.texture = ObjectTexture(lib, key.first);
                    layer.list = glGenLists(1);
                    glNewList(layer.list, GL_COMPILE);
                    glBegin(GL_TRIANGLES);
                    for (const Vtx& v : vtx) { glTexCoord2f(v.uv.x, v.uv.y); glNormal3f(v.n.x, v.n.y, v.n.z); glVertex3f(v.p.x, v.p.y, v.p.z); }
                    glEnd();
                    glEndList();
                    out.layers.push_back(layer);
                }
            out.dressed = d.summary;
            return;
        }
        out.list = glGenLists(1);
        glNewList(out.list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        for (size_t pi = 0; pi < model.parts.size(); ++pi) {
            const fig::ModelPart& part = model.parts[pi];
            // OBJ_BODYPARTS lists the parts to show; empty shows them all (ei_maper's CFigure::getVertexData).
            if (!src.parts.empty() && std::find(src.parts.begin(), src.parts.end(), checksLower(part.name)) == src.parts.end()) continue;
            const fig::FigureMesh& mesh = part.mesh;
            for (uint16_t index : mesh.indices) {
                if (index >= mesh.vertexComponents.size()) continue;
                const fig::VertComponent& vc = mesh.vertexComponents[index];
                const fig::Vec3 p = pose[pi].p + fig::QRotate(pose[pi].w, mesh.BlendedPosition(index, constitution));
                const fig::Vec3 n = fig::QRotate(pose[pi].w, vc.normalIndex < mesh.normals.size() ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1});
                const fig::Vec2 uv = vc.uvIndex < mesh.uvs.size() ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0};
                glTexCoord2f(uv.x, uv.y);
                glNormal3f(n.x, n.y, n.z);
                glVertex3f(p.x, p.y, p.z);
                grow(p);
            }
        }
        glEnd();
        glEndList();
        out.ok = !first;
        if (!out.ok) { out.error = "no geometry"; glDeleteLists(out.list, 1); out.list = 0; return; }
        out.texture = ObjectTexture(lib, src.texture);
    }

    static void ApplyObjectTransform(const fig::Vec3& p, const fig::Quat& q) {
        glTranslatef(p.x, p.y, p.z);
        fig::Vec3 ax = fig::QuatRotate(q, {1, 0, 0}), ay = fig::QuatRotate(q, {0, 1, 0}), az = fig::QuatRotate(q, {0, 0, 1});
        const float m[16] = {ax.x, ax.y, ax.z, 0, ay.x, ay.y, ay.z, 0, az.x, az.y, az.z, 0, 0, 0, 0, 1};
        glMultMatrixf(m);
    }

    void DrawObjects(const Library& lib) {
        (void)lib;
        if (wirePass_) { // the edges over the textured objects
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.05f, 0.05f, 0.05f, 1.0f);
            for (size_t fi = 0; fi < maps_.size(); ++fi) {
                if (fi < visible_.size() && !visible_[fi]) continue;
                for (const mob::Object& o : maps_[fi]->objects) {
                    if (!mob::HasFigure(o.kind) || !Shown(o)) continue;
                    const MapModel* m = ModelFor(o);
                    if (!m || !m->ok) continue;
                    glPushMatrix();
                    ApplyObjectTransform(DrawPosition(o, m), Rotation(o));
                    if (m->list) glCallList(m->list);
                    for (const MapModel::Layer& l : m->layers) if (!l.overlay) glCallList(l.list);
                    glPopMatrix();
                }
            }
            return;
        }
        glEnable(GL_LIGHTING);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, alphaCut_);
        for (size_t fi = 0; fi < maps_.size(); ++fi) {
            if (fi < visible_.size() && !visible_[fi]) continue;
            for (const mob::Object& o : maps_[fi]->objects) {
                if (!mob::HasFigure(o.kind) || !Shown(o)) continue;
                const MapModel* m = ModelFor(o);
                if (!m || !m->ok) continue;
                if (!m->layers.empty()) { // a dressed unit
                    glPushMatrix();
                    ApplyObjectTransform(DrawPosition(o, m), Rotation(o));
                    for (const MapModel::Layer& l : m->layers) {
                        const bool textured = options.textured && l.texture;
                        if (l.overlay && (!textured || depthPass_)) continue;
                        if (textured) {
                            glEnable(GL_TEXTURE_2D);
                            glBindTexture(GL_TEXTURE_2D, l.texture);
                            glColor4f(1, 1, 1, 1);
                        } else {
                            glDisable(GL_TEXTURE_2D);
                            glColor4f(0.85f, 0.55f, 0.45f, 1);
                        }
                        if (l.overlay) { // on the same surface: blended, drawn where the skin already is
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glDepthFunc(GL_LEQUAL);
                            glDepthMask(GL_FALSE);
                        }
                        glCallList(l.list);
                        if (l.overlay) {
                            glDisable(GL_BLEND);
                            glDepthFunc(GL_LESS);
                            glDepthMask(GL_TRUE);
                        }
                    }
                    glPopMatrix();
                    continue;
                }
                bool textured = options.textured && m->texture;
                if (textured) {
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, m->texture);
                    glColor4f(1, 1, 1, 1);
                } else {
                    glDisable(GL_TEXTURE_2D);
                    if (o.kind == mob::Kind::Unit) glColor4f(0.85f, 0.55f, 0.45f, 1);
                    else glColor4f(0.7f, 0.68f, 0.62f, 1);
                }
                glPushMatrix();
                ApplyObjectTransform(DrawPosition(o, m), Rotation(o));
                glCallList(m->list);
                glPopMatrix();
            }
        }
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
    }

    // Lights (yellow), particles (magenta), sounds (cyan) and objects without a figure (red), as small cubes.
    void DrawMarkers() const {
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        for (size_t fi = 0; fi < maps_.size(); ++fi) {
            if (fi < visible_.size() && !visible_[fi]) continue;
            for (const mob::Object& o : maps_[fi]->objects) {
                const MapModel* m = ModelFor(o);
                bool figureless = mob::HasFigure(o.kind) && (!m || !m->ok);
                if (mob::HasFigure(o.kind) && !figureless) continue;
                if (figureless && !(o.kind == mob::Kind::Unit ? options.units : options.objects)) continue;
                if (!figureless && !options.markers) continue;
                switch (o.kind) {
                case mob::Kind::Light: glColor3f(1.0f, 0.9f, 0.3f); break;
                case mob::Kind::Particle: glColor3f(0.95f, 0.35f, 0.9f); break;
                case mob::Kind::Sound: glColor3f(0.3f, 0.9f, 0.95f); break;
                default: glColor3f(0.95f, 0.25f, 0.2f); break;
                }
                fig::Vec3 p = DrawPosition(o, m);
                Cube(p, figureless && m && !m->tried ? 0.15f : 0.35f);
            }
        }
    }

    static void Cube(const fig::Vec3& c, float h) {
        glBegin(GL_QUADS);
        const float v[8][3] = {{c.x - h, c.y - h, c.z - h}, {c.x + h, c.y - h, c.z - h}, {c.x + h, c.y + h, c.z - h}, {c.x - h, c.y + h, c.z - h},
                               {c.x - h, c.y - h, c.z + h}, {c.x + h, c.y - h, c.z + h}, {c.x + h, c.y + h, c.z + h}, {c.x - h, c.y + h, c.z + h}};
        const int f[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 3, 7, 4}};
        for (auto& face : f) for (int i : face) glVertex3fv(v[i]);
        glEnd();
    }

    // ---- quest areas ------------------------------------------------------------------------------
    // Each exit of the open quest: where the party is deployed (#deploy, green) and the area that leaves
    // the map (#remove, red), as rectangles laid on the ground.
    void GroundRect(const quest::Rect& r, float red, float green, float blue) const {
        if (!r.set) return;
        float x0 = std::min(r.x1, r.x2), x1 = std::max(r.x1, r.x2), y0 = std::min(r.y1, r.y2), y1 = std::max(r.y1, r.y2);
        const int nx = std::max(1, std::min(64, static_cast<int>(x1 - x0))), ny = std::max(1, std::min(64, static_cast<int>(y1 - y0)));
        auto at = [&](int i, int j) {
            float x = x0 + (x1 - x0) * i / nx, y = y0 + (y1 - y0) * j / ny;
            glVertex3f(x, y, Ground(x, y) + 0.12f);
        };
        glColor4f(red, green, blue, 0.28f);
        glBegin(GL_QUADS);
        for (int i = 0; i < nx; ++i)
            for (int j = 0; j < ny; ++j) { at(i, j); at(i + 1, j); at(i + 1, j + 1); at(i, j + 1); }
        glEnd();
        glColor4f(red, green, blue, 1.0f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < nx; ++i) at(i, 0);
        for (int j = 0; j < ny; ++j) at(nx, j);
        for (int i = nx; i > 0; --i) at(i, ny);
        for (int j = ny; j > 0; --j) at(0, j);
        glEnd();
        glLineWidth(1.0f);
    }

    void DrawExits(const Library& lib) {
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const quest::Exit& e : quest->exits) {
            GroundRect(e.remove, 0.95f, 0.3f, 0.25f);
            GroundRect(e.deploy, 0.3f, 0.95f, 0.4f);
        }
        glEnable(GL_DEPTH_TEST);
        for (const quest::Exit& e : quest->exits) if (e.remove.set) ExitSparkles(lib, e.remove);
        glDisable(GL_BLEND);
    }

    // The game's floating stars over an exit (#remove) area: zoneexit's 4 x 4 frames, each star at its own
    // place in the area, twinkling through the frames while it rises and fades. Hidden behind the terrain
    // and objects, never hiding anything (additive, no depth writes).
    void ExitSparkles(const Library& lib, const quest::Rect& r) {
        const GLuint tex = ObjectTexture(lib, "zoneexit");
        if (!tex) return;
        const float x0 = std::min(r.x1, r.x2), x1 = std::max(r.x1, r.x2), y0 = std::min(r.y1, r.y2), y1 = std::max(r.y1, r.y2);
        const int count = std::max(20, std::min(600, static_cast<int>((x1 - x0) * (y1 - y0) * 1.2f)));
        float m[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        const fig::Vec3 right{m[0], m[4], m[8]}, up{m[1], m[5], m[9]};
        const double now = glfwGetTime();
        // The same stars every frame: a fixed pseudo-random sequence per area.
        uint32_t seed = static_cast<uint32_t>(x0 * 131.0f + y0 * 7919.0f) | 1u;
        auto rnd = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 16777216.0f; };
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glDisable(GL_ALPHA_TEST);
        glDepthMask(GL_FALSE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBegin(GL_QUADS);
        for (int i = 0; i < count; ++i) {
            const float px = x0 + rnd() * (x1 - x0), py = y0 + rnd() * (y1 - y0);
            const float period = 2.5f + rnd() * 2.5f, phase = rnd() * period, size = 0.18f + rnd() * 0.2f;
            const int firstFrame = static_cast<int>(rnd() * 16);
            const float t = static_cast<float>(std::fmod(now + phase, static_cast<double>(period))) / period; // 0..1 over its life
            const float z = Ground(px, py) + 0.15f + t * 1.4f;
            const float alpha = std::sin(t * kPi);
            const int frame = (firstFrame + static_cast<int>(now * 10.0)) % 16;
            const float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
            glColor4f(1.0f, 1.0f, 1.0f, alpha);
            auto corner = [&](float cx, float cy, float u, float v) {
                glTexCoord2f(u, v);
                glVertex3f(px + (right.x * cx + up.x * cy) * size, py + (right.y * cx + up.y * cy) * size, z + (right.z * cx + up.z * cy) * size);
            };
            corner(-1, -1, u0, v0 + 0.25f);
            corner(1, -1, u0 + 0.25f, v0 + 0.25f);
            corner(1, 1, u0 + 0.25f, v0);
            corner(-1, 1, u0, v0);
        }
        glEnd();
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_TEXTURE_2D);
    }

    // ---- logic mode ---------------------------------------------------------------------------
    // Draws what ei_maper's logic mode draws (objects/unit.cpp, CLogic::createLogicLines): for each used
    // behaviour of a unit, the guard radius (orange) or the patrol path through its points (yellow,
    // closed when cyclic), the look directions at each point (blue), the sentry place, and the radius
    // within which it calls for help (purple). Everything on the ground, drawn over the scene.
    void GroundCircle(const mob::Vec3& c, float radius) const {
        if (radius <= 0.0f) return;
        glBegin(GL_LINE_LOOP);
        const int n = std::max(24, std::min(96, static_cast<int>(radius * 6)));
        for (int i = 0; i < n; ++i) {
            float a = 2.0f * kPi * i / n;
            float x = c.x + std::cos(a) * radius, y = c.y + std::sin(a) * radius;
            glVertex3f(x, y, Ground(x, y) + 0.15f);
        }
        glEnd();
    }
    void GroundPoint(const mob::Vec3& p, float h) const { Cube({p.x, p.y, Ground(p.x, p.y) + h}, h); }
    // A round base on the ground where a logic point stands (what is clicked to select it).
    void Disc(const mob::Vec3& p, float radius) const {
        const float z = Ground(p.x, p.y) + 0.04f;
        glBegin(GL_TRIANGLE_FAN);
        glVertex3f(p.x, p.y, z);
        for (int i = 0; i <= 24; ++i) {
            const float a = 2.0f * kPi * i / 24;
            glVertex3f(p.x + std::cos(a) * radius, p.y + std::sin(a) * radius, z);
        }
        glEnd();
    }

    // The game's walkability graph, one layer: each step a node can take as a line to its neighbour
    // (green cheap, yellow then red dearer), and a red square where a node can go nowhere. From the first
    // shown map that has one (a zone's own map), built into a display list when it or the layer changes.
    const mob::File* NavmeshFile() const {
        for (size_t i = 0; i < maps_.size(); ++i)
            if ((i >= visible_.size() || visible_[i]) && maps_[i]->aiGraphAt) return maps_[i];
        return nullptr;
    }
    void DrawNavmesh() {
        const mob::File* f = NavmeshFile();
        if (!f) return;
        const std::string key = f->path + "|" + std::to_string(options.navLayer) + "|" + std::to_string(f->bytes.size()) + "|" +
                                std::to_string(reinterpret_cast<uintptr_t>(terrain_));
        if (key != navKey_ || !navList_) {
            if (navList_) glDeleteLists(navList_, 1);
            navKey_ = key;
            navList_ = glGenLists(1);
            glNewList(navList_, GL_COMPILE);
            const int layer = options.navLayer;
            glBegin(GL_LINES);
            for (int y = 0; y < f->aiH; ++y)
                for (int x = 0; x < f->aiW; ++x) {
                    const float cx = x * 4.0f + 2.0f, cy = y * 4.0f + 2.0f;
                    for (int d = 4; d < 8; ++d) { // each link once: towards north, north-east, east, south-east
                        const uint16_t c = mob::AiCost(*f, layer, x, y, d);
                        if (c == 0xFFFF) continue;
                        const float base = (d % 2) ? 90.0f : 64.0f;
                        const float k = std::min(std::max((c / base - 1.0f) / 1.5f, 0.0f), 1.0f); // 0 cheap .. 1 dear
                        glColor4f(0.2f + 0.8f * k, 0.9f - 0.6f * k, 0.25f, 0.8f);
                        const float nx = cx + mob::kAiDx[d] * 4.0f, ny = cy + mob::kAiDy[d] * 4.0f;
                        glVertex3f(cx, cy, Ground(cx, cy) + 0.3f);
                        glVertex3f(nx, ny, Ground(nx, ny) + 0.3f);
                    }
                }
            glEnd();
            glColor4f(0.9f, 0.15f, 0.1f, 0.35f);
            glBegin(GL_QUADS);
            for (int y = 0; y < f->aiH; ++y)
                for (int x = 0; x < f->aiW; ++x) {
                    if (mob::AiWalkable(*f, layer, x, y)) continue;
                    const float x0 = x * 4.0f + 0.4f, y0 = y * 4.0f + 0.4f, x1 = x0 + 3.2f, y1 = y0 + 3.2f;
                    glVertex3f(x0, y0, Ground(x0, y0) + 0.2f); glVertex3f(x1, y0, Ground(x1, y0) + 0.2f);
                    glVertex3f(x1, y1, Ground(x1, y1) + 0.2f); glVertex3f(x0, y1, Ground(x0, y1) + 0.2f);
                }
            glEnd();
            glEndList();
        }
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glLineWidth(1.5f);
        glCallList(navList_);
        glLineWidth(1.0f);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

public:
    // The areas the scripts declare (set by the Map Editor from the loaded maps' scripts): outlines on the
    // ground, magenta.
    struct ScriptArea { int id = 0; bool round = true; float x = 0, y = 0, r = 0, x2 = 0, y2 = 0; };
    std::vector<ScriptArea> scriptAreas;
    void DrawScriptAreas() {
        if (scriptAreas.empty()) return;
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(2.5f);
        for (const ScriptArea& a : scriptAreas) {
            std::vector<std::pair<float, float>> pts;
            if (a.round) {
                const int n = std::max(32, static_cast<int>(a.r * 2.0f));
                for (int i = 0; i <= n; ++i) {
                    const float t = 2.0f * kPi * i / n;
                    pts.push_back({a.x + std::cos(t) * a.r, a.y + std::sin(t) * a.r});
                }
            } else {
                const float c[5][2] = {{a.x, a.y}, {a.x2, a.y}, {a.x2, a.y2}, {a.x, a.y2}, {a.x, a.y}};
                for (int k = 0; k < 4; ++k) {
                    const float len = std::hypot(c[k + 1][0] - c[k][0], c[k + 1][1] - c[k][1]);
                    const int steps = std::max(1, static_cast<int>(len));
                    for (int i = 0; i < steps; ++i)
                        pts.push_back({c[k][0] + (c[k + 1][0] - c[k][0]) * i / steps, c[k][1] + (c[k + 1][1] - c[k][1]) * i / steps});
                }
                pts.push_back({c[4][0], c[4][1]});
            }
            glColor4f(0.95f, 0.3f, 0.95f, 0.9f);
            glBegin(GL_LINE_STRIP);
            for (const auto& p : pts) glVertex3f(p.first, p.second, Ground(p.first, p.second) + 0.35f);
            glEnd();
        }
        glLineWidth(1.0f);
        glDisable(GL_BLEND);
    }

    // The map's graph (AI_GRAPH, the shown layer) against the one this editor builds from the terrain and the
    // objects as the game does (navmesh_gen.hpp; set by the Map Editor in builtNav), per graph node: orange
    // where only the map's graph walks, blue where only the built one does, yellow where both walk but some
    // step costs differ. Where they differ, the map's graph is out of date.
    std::vector<uint8_t> builtNav;   // an AI_GRAPH payload
    unsigned builtNavStamp = 0;      // bumped with each new builtNav
    int navCompareGame = 0, navCompareEditor = 0, navCompareCost = 0; // the counts of each, for the legend
    void DrawNavCompare() {
        const mob::File* f = NavmeshFile();
        if (!f || builtNav.size() < 8) return;
        const uint32_t bw = builtNav[0] | (builtNav[1] << 8) | (builtNav[2] << 16) | (static_cast<uint32_t>(builtNav[3]) << 24);
        const uint32_t bh = builtNav[4] | (builtNav[5] << 8) | (builtNav[6] << 16) | (static_cast<uint32_t>(builtNav[7]) << 24);
        if (static_cast<int>(bw) != f->aiW || static_cast<int>(bh) != f->aiH || builtNav.size() != 8 + static_cast<size_t>(bw) * bh * 19 * mob::kAiLayers) return;
        auto built = [&](int layer, int x, int y, int d) {
            const size_t at = 8 + (static_cast<size_t>(layer) * bh + y) * bw * 19 + static_cast<size_t>(x) * 16 + d * 2;
            return static_cast<uint16_t>(builtNav[at] | (builtNav[at + 1] << 8));
        };
        const std::string key = f->path + "|" + std::to_string(options.navLayer) + "|" + std::to_string(f->bytes.size()) + "|" +
                                std::to_string(builtNavStamp);
        if (key != cmpKey_ || !cmpList_) {
            if (cmpList_) glDeleteLists(cmpList_, 1);
            cmpKey_ = key;
            cmpList_ = glGenLists(1);
            navCompareGame = navCompareEditor = navCompareCost = 0;
            glNewList(cmpList_, GL_COMPILE);
            glBegin(GL_QUADS);
            for (int y = 0; y < f->aiH; ++y)
                for (int x = 0; x < f->aiW; ++x) {
                    bool game = false, ours = false, costs = false;
                    for (int d = 0; d < 8; ++d) {
                        const uint16_t a = mob::AiCost(*f, options.navLayer, x, y, d), b = built(options.navLayer, x, y, d);
                        game |= a != 0xFFFF;
                        ours |= b != 0xFFFF;
                        costs |= a != b;
                    }
                    if (game && !ours) { ++navCompareGame; glColor4f(1.0f, 0.55f, 0.1f, 0.5f); }
                    else if (ours && !game) { ++navCompareEditor; glColor4f(0.2f, 0.55f, 1.0f, 0.5f); }
                    else if (costs) { ++navCompareCost; glColor4f(0.95f, 0.9f, 0.2f, 0.35f); }
                    else continue;
                    const float x0 = x * 4.0f + 0.3f, y0 = y * 4.0f + 0.3f, x1 = x0 + 3.4f, y1 = y0 + 3.4f;
                    glVertex3f(x0, y0, Ground(x0, y0) + 0.25f); glVertex3f(x1, y0, Ground(x1, y0) + 0.25f);
                    glVertex3f(x1, y1, Ground(x1, y1) + 0.25f); glVertex3f(x0, y1, Ground(x0, y1) + 0.25f);
                }
            glEnd();
            glEndList();
        }
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        glCallList(cmpList_);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

private:
    // The computed walkability grid: a red square on each blocked cell (built with BuildWalkGrid).
    void DrawWalkability() {
        if (walk.w == 0) BuildWalkGrid();
        if (walk.w == 0) return;
        if (walkListBuild_ != walkBuilds_ || !walkList_) {
            if (walkList_) glDeleteLists(walkList_, 1);
            walkListBuild_ = walkBuilds_;
            walkList_ = glGenLists(1);
            glNewList(walkList_, GL_COMPILE);
            glBegin(GL_QUADS);
            for (int y = 0; y < walk.h; ++y)
                for (int x = 0; x < walk.w; ++x) {
                    const int v = walk.value[static_cast<size_t>(y) * walk.w + x];
                    if (v >= 7) continue; // easy ground: nothing drawn
                    if (v == 0) glColor4f(0.95f, 0.2f, 0.15f, 0.40f);  // blocked: red
                    else glColor4f(0.95f, 0.65f, 0.15f, 0.30f);        // hard (water, slopes, obstacles): orange
                    const float x0 = x * walk.cell, y0 = y * walk.cell, x1 = x0 + walk.cell, y1 = y0 + walk.cell;
                    glVertex3f(x0, y0, Ground(x0, y0) + 0.12f); glVertex3f(x1, y0, Ground(x1, y0) + 0.12f);
                    glVertex3f(x1, y1, Ground(x1, y1) + 0.12f); glVertex3f(x0, y1, Ground(x0, y1) + 0.12f);
                }
            glEnd();
            glEndList();
        }
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        glCallList(walkList_);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // The selected magic traps' activation areas (orange circles, a handle at the centre) and cast points
    // (magenta, a line from the trap), like ei_maper. Selected handles are white.
    void DrawTraps() const {
        if (activeFile < 0 || activeFile >= static_cast<int>(maps_.size()) || selectedFile != activeFile) return;
        const auto& objects = maps_[activeFile]->objects;
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        for (int oi : selection) {
            if (oi < 0 || oi >= static_cast<int>(objects.size()) || objects[oi].kind != mob::Kind::MagicTrap) continue;
            const mob::Object& o = objects[oi];
            const fig::Vec3 at = DrawPosition(o, ModelFor(o));
            for (size_t i = 0; i < o.trapAreas.size(); ++i) {
                const mob::Vec3& a = o.trapAreas[i];
                const bool picked = IsLogicPointSelected({oi, -1, static_cast<int>(i), -1, false, 1});
                glColor3f(0.95f, 0.45f, 0.15f);
                GroundCircle({a.x, a.y, 0}, a.z);
                if (picked) glColor3f(1, 1, 1);
                Disc({a.x, a.y, 0}, 0.5f);
            }
            for (size_t i = 0; i < o.trapTargets.size(); ++i) {
                const mob::Vec3& t = o.trapTargets[i];
                const bool picked = IsLogicPointSelected({oi, -1, static_cast<int>(i), -1, false, 2});
                glColor3f(0.9f, 0.3f, 0.9f);
                glBegin(GL_LINES);
                glVertex3f(at.x, at.y, at.z + 0.5f);
                glVertex3f(t.x, t.y, Ground(t.x, t.y) + 0.3f);
                glEnd();
                if (picked) glColor3f(1, 1, 1);
                GroundPoint({t.x, t.y, 0}, 0.3f);
            }
        }
        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
    }

    // A patrol point as ei_maper shows it: a small flag on a short pole, on a round base; the cloth is
    // turned across the view.
    void Flag(const mob::Vec3& p, float r, float g, float b, bool picked = false) const {
        const float z = Ground(p.x, p.y);
        const float yaw = camera.yawDeg * kPi / 180.0f;
        const float rx = -std::sin(yaw), ry = std::cos(yaw); // the view's right
        const float pole = 1.4f, clothW = 0.75f, clothH = 0.5f;
        if (picked) glColor3f(1.0f, 1.0f, 1.0f); // selected: white
        else glColor3f(r * 0.8f, g * 0.8f, b * 0.8f);
        Disc(p, 0.45f);
        glColor3f(0.55f, 0.4f, 0.25f);
        glLineWidth(2.5f);
        glBegin(GL_LINES);
        glVertex3f(p.x, p.y, z);
        glVertex3f(p.x, p.y, z + pole);
        glEnd();
        if (picked) glColor3f(1.0f, 1.0f, 1.0f);
        else glColor3f(r, g, b);
        glBegin(GL_TRIANGLES);
        glVertex3f(p.x, p.y, z + pole);
        glVertex3f(p.x, p.y, z + pole - clothH);
        glVertex3f(p.x + rx * clothW, p.y + ry * clothW, z + pole - clothH * 0.5f);
        glEnd();
    }

    // A look point: an eye of Horus standing over a round base, facing the camera (a billboard), about
    // the flag's size. Its shape is drawn in a 1600-pixel picture's coordinates (y down), scaled to the world.
    void Eye(const mob::Vec3& p, bool picked) const {
        const float z = Ground(p.x, p.y);
        float m[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        const fig::Vec3 right{m[0], m[4], m[8]}, up{m[1], m[5], m[9]}; // the camera's axes in the world
        const float scale = 0.6f / 640.0f, lift = 0.85f;
        auto vtx = [&](float px, float py) {
            const float x = (px - 800.0f) * scale, y = (800.0f - py) * scale;
            glVertex3f(p.x + right.x * x + up.x * y, p.y + right.y * x + up.y * y, z + lift + right.z * x + up.z * y);
        };
        const float lr = picked ? 1.0f : 0.25f, lg = picked ? 0.9f : 0.55f, lb = picked ? 0.4f : 1.0f; // lines: blue, selected yellow
        glColor3f(lr * 0.8f, lg * 0.8f, lb * 0.8f);
        Disc(p, 0.3f);
        // The white of the eye, then the pupil.
        static const float white[][2] = {{325, 630}, {600, 540}, {800, 495}, {1000, 520}, {1260, 603}, {1100, 720}, {850, 775}, {600, 735}};
        glColor3f(0.93f, 0.95f, 1.0f);
        glBegin(GL_POLYGON);
        for (const auto& q : white) vtx(q[0], q[1]);
        glEnd();
        glColor3f(0.1f, 0.25f, 0.7f);
        glBegin(GL_TRIANGLE_FAN);
        vtx(862, 610);
        for (int i = 0; i <= 20; ++i) { const float a = 2.0f * kPi * i / 20; vtx(862 + std::cos(a) * 140, 610 + std::sin(a) * 105); }
        glEnd();
        // The eyebrow: a band between two curves.
        static const float browTop[][2] = {{175, 452}, {420, 395}, {720, 318}, {1050, 350}, {1250, 430}, {1440, 470}};
        static const float browLow[][2] = {{175, 452}, {420, 470}, {740, 385}, {1050, 410}, {1250, 470}, {1440, 470}};
        glColor3f(lr, lg, lb);
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i < 6; ++i) { vtx(browTop[i][0], browTop[i][1]); vtx(browLow[i][0], browLow[i][1]); }
        glEnd();
        // The lids, the line out to the right, the line down to the spiral, and the teardrop.
        glLineWidth(3.0f);
        auto strip = [&](std::initializer_list<std::pair<float, float>> pts) {
            glBegin(GL_LINE_STRIP);
            for (const auto& q : pts) vtx(q.first, q.second);
            glEnd();
        };
        strip({{160, 603}, {450, 560}, {800, 450}, {1100, 505}, {1405, 578}});                  // upper lid
        strip({{325, 630}, {600, 745}, {850, 790}, {1100, 725}, {1260, 603}, {1405, 610}});     // lower lid and its tail
        strip({{325, 630}, {160, 603}});
        strip({{990, 780}, {740, 1060}, {560, 1200}, {430, 1235}, {300, 1210}, {225, 1110}, {235, 990}, {330, 945},
               {430, 990}, {445, 1080}, {380, 1130}, {325, 1080}, {360, 1030}, {410, 1030}});   // down to the spiral
        glBegin(GL_POLYGON); // the teardrop under the eye's back corner
        vtx(1110, 745); vtx(1160, 805); vtx(1250, 880); vtx(1335, 870); vtx(1115, 1250);
        glEnd();
        glLineWidth(1.5f);
    }

public:
    // The units whose logic shows: the selected unit, or every unit of the active map.
    template <typename F> void ForEachLogicUnit(F f) const {
        if (activeFile < 0 || activeFile >= static_cast<int>(maps_.size())) return;
        if (activeFile < static_cast<int>(visible_.size()) && !visible_[activeFile]) return;
        const auto& objects = maps_[activeFile]->objects;
        for (size_t oi = 0; oi < objects.size(); ++oi) {
            const mob::Object& o = objects[oi];
            if (o.kind != mob::Kind::Unit || o.logics.empty()) continue;
            bool selected = IsSelected(activeFile, static_cast<int>(oi));
            if ((logicSelectedOnly || !logicMode) && !selected) continue;
            f(o, selected);
        }
    }

private:
    void DrawLogic() const {
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        const GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE); // the flags are seen from both sides
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        ForEachLogicUnit([&](const mob::Object& o, bool selected) {
            const int oi = static_cast<int>(&o - maps_[activeFile]->objects.data());
            glLineWidth(selected ? 2.5f : 1.5f);
            const float fade = selected ? 1.0f : 0.75f;
            fig::Vec3 unit = DrawPosition(o, ModelFor(o));
            for (size_t gi = 0; gi < o.logics.size(); ++gi) {
                const mob::Logic& g = o.logics[gi];
                if (!g.use) continue;
                auto picked = [&](int point, bool place) {
                    return IsLogicPointSelected({oi, static_cast<int>(gi), point, -1, place});
                };
                if (g.model == 1) { // guard: a radius around a place
                    glColor3f(0.95f * fade, 0.45f * fade, 0.1f * fade);
                    GroundCircle(g.guardPlace, g.guardRadius);
                    if (picked(-1, true)) glColor3f(1.0f, 1.0f, 1.0f);
                    GroundPoint(g.guardPlace, 0.2f);
                } else if (g.model == 2 && !g.patrol.empty()) { // patrol: a path
                    glColor3f(0.85f * fade, 0.85f * fade, 0.2f * fade);
                    glBegin(GL_LINE_STRIP);
                    glVertex3f(unit.x, unit.y, Ground(unit.x, unit.y) + 0.2f);
                    for (const mob::PatrolPoint& p : g.patrol) {
                        // Follow the ground between points, like ei_maper (splitByLen 2.0).
                        glVertex3f(p.position.x, p.position.y, Ground(p.position.x, p.position.y) + 0.2f);
                    }
                    if (g.cyclic) glVertex3f(g.patrol[0].position.x, g.patrol[0].position.y, Ground(g.patrol[0].position.x, g.patrol[0].position.y) + 0.2f);
                    glEnd();
                    for (size_t pi = 0; pi < g.patrol.size(); ++pi) {
                        const mob::PatrolPoint& p = g.patrol[pi];
                        Flag(p.position, 0.95f * fade, 0.9f * fade, 0.2f * fade, picked(static_cast<int>(pi), false));
                        for (size_t k = 0; k < p.looks.size(); ++k)
                            Eye(p.looks[k].position, IsLogicPointSelected({oi, static_cast<int>(gi), static_cast<int>(pi), static_cast<int>(k), false}));
                        glColor3f(0.2f * fade, 0.5f * fade, 0.95f * fade);
                        glBegin(GL_LINES);
                        for (const mob::LookPoint& l : p.looks) {
                            glVertex3f(p.position.x, p.position.y, Ground(p.position.x, p.position.y) + 0.3f);
                            glVertex3f(l.position.x, l.position.y, Ground(l.position.x, l.position.y) + 0.3f);
                        }
                        glEnd();
                    }
                } else if (g.model == 3) { // sentry: a place
                    glColor3f(0.3f * fade, 0.9f * fade, 0.5f * fade);
                    glBegin(GL_LINES);
                    glVertex3f(unit.x, unit.y, Ground(unit.x, unit.y) + 0.2f);
                    glVertex3f(g.guardPlace.x, g.guardPlace.y, Ground(g.guardPlace.x, g.guardPlace.y) + 0.2f);
                    glEnd();
                    if (picked(-1, true)) glColor3f(1.0f, 1.0f, 1.0f);
                    GroundPoint(g.guardPlace, 0.2f);
                }
                if (g.help > 0.0f) { // calls for help within
                    glColor3f(0.75f * fade, 0.2f * fade, 0.95f * fade);
                    // Around the guard place, like ei_maper; a unit with none set calls from where it stands.
                    const bool noPlace = g.guardPlace.x == 0.0f && g.guardPlace.y == 0.0f;
                    GroundCircle(noPlace ? mob::Vec3{unit.x, unit.y, 0} : g.guardPlace, g.help);
                }
            }
        });
        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE);
    }

    // A box around each selected object: bright yellow for the main one, paler for the others.
    void DrawSelection() const {
        if (selectedFile < 0 || selectedFile >= static_cast<int>(maps_.size())) return;
        const auto& objects = maps_[selectedFile]->objects;
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        for (int oi : selection) {
            if (oi < 0 || oi >= static_cast<int>(objects.size())) continue;
            const mob::Object& o = objects[oi];
            const MapModel* m = ModelFor(o);
            fig::Vec3 p = DrawPosition(o, m);
            const bool main = oi == selectedObject;
            glLineWidth(main ? 2.0f : 1.5f);
            if (main) glColor3f(1.0f, 0.85f, 0.2f);
            else glColor3f(0.95f, 0.8f, 0.45f);
            fig::Vec3 lo{-0.4f, -0.4f, -0.4f}, hi{0.4f, 0.4f, 0.4f};
            if (m && m->ok) { lo = m->boundsMin; hi = m->boundsMax; }
            glPushMatrix();
            ApplyObjectTransform(p, mob::HasFigure(o.kind) ? Rotation(o) : fig::Quat{});
            const float x[2] = {lo.x, hi.x}, y[2] = {lo.y, hi.y}, z[2] = {lo.z, hi.z};
            glBegin(GL_LINES);
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 2; ++b) {
                    glVertex3f(x[0], y[a], z[b]); glVertex3f(x[1], y[a], z[b]);
                    glVertex3f(x[a], y[0], z[b]); glVertex3f(x[a], y[1], z[b]);
                    glVertex3f(x[a], y[b], z[0]); glVertex3f(x[a], y[b], z[1]);
                }
            glEnd();
            glPopMatrix();
        }
        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
    }
};

} // namespace mapedit
