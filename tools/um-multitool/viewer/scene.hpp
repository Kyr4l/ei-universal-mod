// The 3D viewport: holds the loaded figure as flat GL arrays, the textures in GL,
// an orbit camera that frames the figure, and draws it with fixed-function
// OpenGL 2 (no GL loader needed; the most robust path under Wine and old drivers).
#pragma once

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <fstream>
#include <string>
#include <vector>

#include "camera.hpp"
#include "gif_writer.hpp"
#include "dds_texture.hpp"
#include "library.hpp"
#include "model_loader.hpp"

struct ScenePart {
    std::vector<float> positions, normals, uvs;
    std::vector<uint16_t> indices;
    std::string texture; // its own texture (a unit's parts: the composed body, a weapon's); "" = the scene's
};

struct GlTexture {
    GLuint id = 0;
    int width = 0, height = 0;
    std::string file;  // the file it came from, e.g. "unhumaax_03.br.0.mmp"
    std::string error; // why it could not be loaded
};

struct ViewOptions {
    bool wireframe = false;
    bool textured = true;
    bool lighting = true;
    bool grid = true;
    bool autoRotate = false;
    bool atlasUvs = true;
    bool checkerboard = false; // squares behind the model (the GIF preview's "transparent") // map the figure's atlas UVs back onto the single texture (see Draw)
    float background[3] = {0.16f, 0.16f, 0.18f};
};

class Scene {
public:
    OrbitCamera camera;
    ViewOptions options;

    bool hasModel = false;
    std::string modelName;   // the figure that is loaded
    std::string modelError;
    std::string textureName; // the texture applied to it
    int vertexCount = 0, triangleCount = 0;
    fig::Vec3 boundsMin{}, boundsMax{};
    // The model's orientation around its centre (the tab's rotation, turned 45 degrees at a time about the world axes).
    fig::Quat orientation;

    void SetOrientation(const std::array<float, 4>* q) {
        orientation = q ? fig::QuatNormalize(fig::Quat{(*q)[0], (*q)[1], (*q)[2], (*q)[3]}) : fig::Quat{};
    }
    // A spin on top of that, about one world axis through the centre (0 = X, 1 = Y, 2 = Z): the GIF turn.
    float spinDegrees = 0.0f;
    int spinAxis = 2;

    // Loads a figure by name from the figure sources; the camera is re-framed when `frame` is set.
    void LoadModel(const Library& lib, const std::string& name, bool frame) {
        parts_.clear();
        shownParts.clear();
        hasModel = false;
        unitModel = false;
        modelName = name;
        modelError.clear();
        vertexCount = triangleCount = 0;
        if (name.empty()) return;
        LoadedModel loaded;
        if (!LoadNamedModel(lib.figures, name, loaded)) {
            modelError = loaded.error;
            return;
        }
        // Item figures carry the same 8 complection corners as units; items are not
        // scaled by complection in the game, so the first corner is used as is.
        const fig::Vec3 constitution{0.0f, 0.0f, 0.0f};
        bool first = true;
        for (const fig::ModelPart& part : loaded.model.parts) {
            ScenePart sp;
            const fig::FigureMesh& mesh = part.mesh;
            fig::Vec3 offset = fig::BlendComplection(part.accumulatedOffset, constitution);
            for (size_t i = 0; i < mesh.vertexComponents.size(); ++i) {
                const fig::VertComponent& vc = mesh.vertexComponents[i];
                fig::Vec3 p = mesh.BlendedPosition(i, constitution) + offset;
                sp.positions.insert(sp.positions.end(), {p.x, p.y, p.z});
                fig::Vec3 n = vc.normalIndex < mesh.normals.size() ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1};
                sp.normals.insert(sp.normals.end(), {n.x, n.y, n.z});
                fig::Vec2 uv = vc.uvIndex < mesh.uvs.size() ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0};
                sp.uvs.insert(sp.uvs.end(), {uv.x, uv.y});
                if (first) { boundsMin = boundsMax = p; first = false; }
                boundsMin = {std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z)};
                boundsMax = {std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z)};
            }
            for (uint16_t index : mesh.indices) {
                if (index < mesh.vertexComponents.size()) sp.indices.push_back(index);
            }
            vertexCount += static_cast<int>(mesh.vertexComponents.size());
            triangleCount += static_cast<int>(sp.indices.size() / 3);
            parts_.push_back(std::move(sp));
        }
        hasModel = !parts_.empty();
        if (!hasModel) modelError = name + " has no geometry";
        if (hasModel && frame) Frame();
    }

    // A unit: a figure with its complection, only the parts `shown` keeps, each with the texture `textureOf`
    // gives (the Map Editor's dressing rules, dress.hpp). Units map their textures directly (no item atlas).
    bool unitModel = false;
    std::set<std::string> shownParts; // the parts drawn (lower case): what Export UV draws
    // The unit's animation clips (its .anm), and the pose shown: Pose() moves the parts.
    std::map<std::string, fig::AnimClip> clips;
    void LoadUnit(const Library& lib, const std::string& name, const fig::Vec3& constitution, bool frame,
                  const std::function<bool(const fig::Model&, const fig::ModelPart&)>& shown,
                  const std::function<std::string(const fig::Model&, const fig::ModelPart&)>& textureOf) {
        parts_.clear();
        shownParts.clear();
        hasModel = false;
        unitModel = true;
        clips.clear();
        unit_ = fig::Model{};
        partSource_.clear();
        modelName = name;
        modelError.clear();
        vertexCount = triangleCount = 0;
        if (name.empty()) return;
        LoadedModel loaded;
        if (!LoadNamedModel(lib.figures, name, loaded)) {
            modelError = loaded.error;
            return;
        }
        bool first = true;
        unit_ = loaded.model;
        clips = loaded.animClips;
        constitution_ = constitution;
        for (size_t pi = 0; pi < loaded.model.parts.size(); ++pi) {
            const fig::ModelPart& part = loaded.model.parts[pi];
            if (!shown(loaded.model, part)) continue;
            partSource_.push_back(static_cast<int>(pi));
            shownParts.insert(LowerName(part.name));
            ScenePart sp;
            sp.texture = textureOf(loaded.model, part);
            const fig::FigureMesh& mesh = part.mesh;
            const fig::Vec3 offset = fig::BlendComplection(part.accumulatedOffset, constitution);
            for (size_t i = 0; i < mesh.vertexComponents.size(); ++i) {
                const fig::VertComponent& vc = mesh.vertexComponents[i];
                const fig::Vec3 p = mesh.BlendedPosition(i, constitution) + offset;
                sp.positions.insert(sp.positions.end(), {p.x, p.y, p.z});
                const fig::Vec3 n = vc.normalIndex < mesh.normals.size() ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1};
                sp.normals.insert(sp.normals.end(), {n.x, n.y, n.z});
                const fig::Vec2 uv = vc.uvIndex < mesh.uvs.size() ? mesh.uvs[vc.uvIndex] : fig::Vec2{0, 0};
                sp.uvs.insert(sp.uvs.end(), {uv.x, uv.y});
                if (first) { boundsMin = boundsMax = p; first = false; }
                boundsMin = {std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z)};
                boundsMax = {std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z)};
            }
            for (uint16_t index : mesh.indices) if (index < mesh.vertexComponents.size()) sp.indices.push_back(index);
            vertexCount += static_cast<int>(mesh.vertexComponents.size());
            triangleCount += static_cast<int>(sp.indices.size() / 3);
            parts_.push_back(std::move(sp));
        }
        hasModel = !parts_.empty();
        if (!hasModel) modelError = name + " has no shown geometry";
        if (hasModel && frame) { Frame(); camera.yawDeg = 225.0f; camera.pitchDeg = 10.0f; } // from the front (figures face -Y)
    }

    // Poses the unit at a frame of one of its clips (frame: fractional, interpolated; wraps); an empty or unknown clip
    // puts it back at rest. The rule (docs/file-formats/figure-format.md): W(part) = q(part) * W(parent), the root at
    // its track's position (relative to its first frame, so the unit stays where the rest pose framed it), every
    // other part at P(parent) + W(parent) * its own .bon offset for this complection, its vertices P + W * v.
    void Pose(const std::string& clipName, float frame) {
        if (!unitModel || !hasModel) return;
        auto ci = clips.find(clipName);
        const fig::AnimClip* clip = ci != clips.end() ? &ci->second : nullptr;
        const size_t n = unit_.parts.size();
        std::vector<fig::Quat> W(n);
        std::vector<fig::Vec3> P(n);
        std::vector<int> state(n, 0); // 0 to do, 1 doing, 2 done
        std::map<std::string, int> byName;
        for (size_t i = 0; i < n; ++i) byName[LowerName(unit_.parts[i].name)] = static_cast<int>(i);
        auto sample = [&](const fig::BoneTrack& t, fig::Quat& q, fig::Vec3& pos, bool& hasPos) {
            const size_t frames = t.rotations.size();
            if (frames == 0) { q = {}; hasPos = false; return; }
            float f = std::fmod(std::max(frame, 0.0f), static_cast<float>(frames));
            const size_t a = static_cast<size_t>(f) % frames, b = (a + 1) % frames;
            const float k = f - std::floor(f);
            q = fig::QSlerp(t.rotations[a], t.rotations[b], k);
            hasPos = a < t.positions.size() && b < t.positions.size();
            if (hasPos) pos = fig::Lerp(t.positions[a], t.positions[b], k);
        };
        std::function<void(size_t)> solve = [&](size_t i) {
            if (state[i] == 2) return;
            state[i] = 1;
            const fig::ModelPart& part = unit_.parts[i];
            fig::Quat q{};
            fig::Vec3 trackPos{};
            bool hasPos = false;
            const fig::BoneTrack* track = nullptr;
            if (clip) {
                auto t = clip->bones.find(LowerName(part.name));
                if (t != clip->bones.end()) { track = &t->second; sample(*track, q, trackPos, hasPos); }
            }
            const fig::Vec3 offset = fig::BlendComplection(part.offset, constitution_);
            auto parent = part.parentName.empty() ? byName.end() : byName.find(LowerName(part.parentName));
            if (parent == byName.end() || state[static_cast<size_t>(parent->second)] == 1) { // the root
                W[i] = q;
                P[i] = offset;
                if (track && hasPos && !track->positions.empty()) P[i] = offset + (trackPos - track->positions[0]);
            } else {
                const size_t pi = static_cast<size_t>(parent->second);
                solve(pi);
                W[i] = fig::QMul(q, W[pi]);
                P[i] = P[pi] + fig::QRotate(W[pi], offset);
            }
            state[i] = 2;
        };
        for (size_t i = 0; i < n; ++i) solve(i);
        for (size_t s = 0; s < parts_.size() && s < partSource_.size(); ++s) {
            const size_t i = static_cast<size_t>(partSource_[s]);
            const fig::FigureMesh& mesh = unit_.parts[i].mesh;
            ScenePart& sp = parts_[s];
            for (size_t v = 0; v < mesh.vertexComponents.size() && 3 * v + 2 < sp.positions.size(); ++v) {
                const fig::VertComponent& vc = mesh.vertexComponents[v];
                const fig::Vec3 p = P[i] + fig::QRotate(W[i], mesh.BlendedPosition(v, constitution_));
                sp.positions[3 * v] = p.x; sp.positions[3 * v + 1] = p.y; sp.positions[3 * v + 2] = p.z;
                const fig::Vec3 nr = fig::QRotate(W[i], vc.normalIndex < mesh.normals.size() ? mesh.normals[vc.normalIndex] : fig::Vec3{0, 0, 1});
                sp.normals[3 * v] = nr.x; sp.normals[3 * v + 1] = nr.y; sp.normals[3 * v + 2] = nr.z;
            }
        }
    }

    static std::string LowerName(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Points the camera at the model and backs off far enough to see all of it.
    void Frame() {
        if (!hasModel) return;
        fig::Vec3 c = (boundsMin + boundsMax) * 0.5f;
        fig::Vec3 d = boundsMax - boundsMin;
        float radius = 0.5f * std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        camera.targetX = c.x;
        camera.targetY = c.y;
        camera.targetZ = c.z;
        camera.distance = std::max(radius, 0.02f) / std::sin(25.0f * 3.14159265f / 180.0f) * 1.08f;
        camera.yawDeg = 45.0f;
        camera.pitchDeg = 20.0f;
    }

    float Radius() const {
        fig::Vec3 d = boundsMax - boundsMin;
        return 0.5f * std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    // A texture's bytes: a file path (a texture picked with Browse: .dds, .mmp or .png) is read from
    // disk, anything else is a base name in the texture sources.
    static bool ReadTextureBytes(const Library& lib, const std::string& name, std::vector<uint8_t>& bytes, std::string* found = nullptr) {
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            std::ifstream f(name, std::ios::binary);
            if (!f.is_open()) return false;
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (found) *found = name;
            return true;
        }
        return lib.textures.ReadTexture(name, bytes, found);
    }

    // "skin|item|item...": the first picture, each next one painted over it by its alpha (sampled at the first
    // one's size when they differ), as the game redresses a unit. False when the first cannot be read.
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
            if (name.empty() || !ReadTextureBytes(lib, name, bytes) || !DecodeTextureFile(bytes, img, err)) {
                if (first) return false;
                continue;
            }
            if (first) { out = std::move(img); first = false; continue; }
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x) {
                    const uint32_t sx = x * img.width / out.width, sy = y * img.height / out.height;
                    const uint8_t* src = img.rgba.data() + (static_cast<size_t>(sy) * img.width + sx) * 4;
                    uint8_t* d = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
                    const int a = src[3];
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((src[c] * a + d[c] * (255 - a)) / 255);
                    d[3] = static_cast<uint8_t>(std::max<int>(d[3], a));
                }
        }
        return !first;
    }

    // The GL texture for a texture base name (or a picked file's path), loaded on first use (id 0 = could not load).
    const GlTexture& Texture(const Library& lib, const std::string& name) {
        auto it = textures_.find(name);
        if (it != textures_.end()) return it->second;
        GlTexture& t = textures_[name];
        mmp::Image image;
        if (name.rfind("compose:", 0) == 0) { // a dressed unit's body: the skin, the worn items painted over it
            if (!ComposeTexture(lib, name.substr(8), image)) { t.error = "its skin was not found"; return t; }
            t.file = name.substr(8);
        } else {
            std::vector<uint8_t> bytes;
            if (!ReadTextureBytes(lib, name, bytes, &t.file)) {
                t.error = "not found in the texture sources";
                return t;
            }
            if (!DecodeTextureFile(bytes, image, t.error)) return t;
        }
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        t.width = static_cast<int>(image.width);
        t.height = static_cast<int>(image.height);
        return t;
    }

    // Drops every GL texture, e.g. after the texture sources changed.
    void ClearTextures() {
        for (auto& kv : textures_) if (kv.second.id) glDeleteTextures(1, &kv.second.id);
        textures_.clear();
    }

    // Draws into the viewport rectangle (x, y, width, height) of the current framebuffer; the caller
    // scissors it if other things share the framebuffer.
    void Draw(const Library& lib, int x, int y, int width, int height, float dt) {
        if (options.autoRotate) camera.yawDeg += dt * 30.0f;
        glViewport(x, y, width, height);
        glClearColor(options.background[0], options.background[1], options.background[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (options.checkerboard) DrawCheckerboard(width, height);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        float radius = hasModel ? std::max(Radius(), 0.02f) : 1.0f;
        Mat4 proj = Mat4::Perspective(50.0f, height > 0 ? static_cast<float>(width) / height : 1.0f,
                                      camera.distance * 0.01f, camera.distance * 20.0f + radius * 4.0f);
        Mat4 view = camera.ViewMatrix();
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(proj.m);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view.m);
        if (hasModel) {
            fig::Vec3 c = (boundsMin + boundsMax) * 0.5f;
            glTranslatef(c.x, c.y, c.z);
            if (spinDegrees != 0.0f) glRotatef(spinDegrees, spinAxis == 0 ? 1.0f : 0.0f, spinAxis == 1 ? 1.0f : 0.0f, spinAxis == 2 ? 1.0f : 0.0f);
            fig::Vec3 ax = fig::QuatRotate(orientation, {1, 0, 0}), ay = fig::QuatRotate(orientation, {0, 1, 0}),
                      az = fig::QuatRotate(orientation, {0, 0, 1});
            const float m[16] = {ax.x, ax.y, ax.z, 0, ay.x, ay.y, ay.z, 0, az.x, az.y, az.z, 0, 0, 0, 0, 1}; // column-major
            glMultMatrixf(m);
            glTranslatef(-c.x, -c.y, -c.z);
        }

        if (options.grid) {
            glPushMatrix();
            glLoadMatrixf(view.m); // the grid stays level whatever the model's rotation
            DrawGrid(radius);
            glPopMatrix();
        }
        if (!hasModel) return;

        GLuint tex = 0;
        int texWidth = 0;
        if (unitModel && options.textured) { // per-part textures, mapped directly
            for (const ScenePart& p : parts_) if (!p.texture.empty() && Texture(lib, p.texture).id) { tex = Texture(lib, p.texture).id; break; }
        } else if (options.textured && !textureName.empty()) {
            const GlTexture& t = Texture(lib, textureName);
            tex = t.id;
            texWidth = t.width;
        }
        // Wireframe alone: lines only. With a texture: the textured model, then its edges over it.
        bool textured = tex != 0;
        const bool wireOver = options.wireframe && textured;

        glPolygonMode(GL_FRONT_AND_BACK, options.wireframe && !wireOver ? GL_LINE : GL_FILL);
        if (textured) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex);
            glColor3f(1.0f, 1.0f, 1.0f);
            // Item figures do not address their texture directly: their UVs point into the
            // bottom-left corner of a 256x256 atlas the game builds, where a texture of width W
            // takes a (W/256)-sized square. Checked on every textured item of the vanilla game +
            // Universal-Mod (445 of 450 fit exactly; the rest only fit once UVs wrap, which
            // GL_REPEAT does too - e.g. the mod's own weapon figures, whose v is shifted by -1).
            // Undo that here: u = u'/s, v = (v' - (1 - s))/s.
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            if (!unitModel && options.atlasUvs && texWidth > 0 && texWidth < 256) {
                float slot = texWidth / 256.0f;
                glScalef(1.0f / slot, 1.0f / slot, 1.0f);
                glTranslatef(0.0f, -(1.0f - slot), 0.0f);
            }
            glMatrixMode(GL_MODELVIEW);
            // Cut-out alpha (e.g. bow strings, fur edges): discard the clear parts, blend the soft edges.
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.1f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_TEXTURE_2D);
            if (options.wireframe) glColor3f(0.85f, 0.85f, 0.9f);
            else glColor3f(0.72f, 0.70f, 0.64f);
        }
        if (options.lighting && (!options.wireframe || wireOver)) {
            // A headlight from the camera plus ambient: normals only shade, colours come from the texture.
            glEnable(GL_LIGHTING);
            glEnable(GL_LIGHT0);
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
            glEnable(GL_NORMALIZE);
            float ex, ey, ez;
            camera.EyePosition(ex, ey, ez);
            const float position[4] = {ex - camera.targetX, ey - camera.targetY, ez - camera.targetZ + camera.distance * 0.5f, 0.0f};
            const float diffuse[4] = {0.75f, 0.75f, 0.75f, 1.0f};
            const float ambient[4] = {0.45f, 0.45f, 0.45f, 1.0f};
            glLightfv(GL_LIGHT0, GL_POSITION, position);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
        }

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        if (textured) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        for (ScenePart& p : parts_) {
            if (p.indices.empty()) continue;
            if (textured && unitModel) { // each part its own texture; untextured (grey) when it has none
                const GLuint id = p.texture.empty() ? 0 : Texture(lib, p.texture).id;
                if (id) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, id); glColor3f(1, 1, 1); }
                else { glDisable(GL_TEXTURE_2D); glColor3f(0.72f, 0.70f, 0.64f); }
            }
            glVertexPointer(3, GL_FLOAT, 0, p.positions.data());
            glNormalPointer(GL_FLOAT, 0, p.normals.data());
            if (textured) glTexCoordPointer(2, GL_FLOAT, 0, p.uvs.data());
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.indices.size()), GL_UNSIGNED_SHORT, p.indices.data());
        }
        if (wireOver) { // the edges over the textured model
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glColor3f(0.05f, 0.05f, 0.05f);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-1.0f, -1.0f);
            for (ScenePart& p : parts_) {
                if (p.indices.empty()) continue;
                glVertexPointer(3, GL_FLOAT, 0, p.positions.data());
                glNormalPointer(GL_FLOAT, 0, p.normals.data());
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.indices.size()), GL_UNSIGNED_SHORT, p.indices.data());
            }
            glDisable(GL_POLYGON_OFFSET_LINE);
        }
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_LIGHTING);
        glDisable(GL_COLOR_MATERIAL);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
    }

    // Puts the model at frame `index` of the turn the GIF settings describe.
    void SetTurntableFrame(const config::GifSettings& g, int index) {
        int count = TurntableFrames(g);
        spinAxis = g.axis;
        spinDegrees = (g.reverse ? -360.0f : 360.0f) * (index % count) / count;
    }

    // Frames per full turn for these settings (at least 2).
    static int TurntableFrames(const config::GifSettings& g) {
        float seconds = 360.0f / std::max(g.degreesPerSecond, 1.0f);
        return std::max(2, static_cast<int>(std::lround(seconds * std::max(g.fps, 1))));
    }

    // One full turn about the vertical axis from the current view, `size` x `size` pixels, drawn into
    // the current framebuffer (which must be at least that big) and read back. The ground grid is left
    // out. A transparent background is found from the depth buffer: whatever the model did not cover
    // (including alpha-tested cut-outs) keeps the cleared depth.
    std::vector<gif::Frame> CaptureTurntable(const Library& lib, const config::GifSettings& g, int size) {
        std::vector<gif::Frame> frames;
        if (!hasModel || size <= 0) return frames;
        ViewOptions saved = options;
        float savedYaw = camera.yawDeg;
        options.grid = false;
        options.autoRotate = false;
        if (g.transparent) {
            // Soft texture edges blend into this: a dark neutral grey reads fine on light and dark pages.
            options.background[0] = options.background[1] = options.background[2] = 0.18f;
        } else {
            options.background[0] = ((g.background >> 16) & 0xFF) / 255.0f;
            options.background[1] = ((g.background >> 8) & 0xFF) / 255.0f;
            options.background[2] = (g.background & 0xFF) / 255.0f;
        }
        const int count = TurntableFrames(g);
        std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
        std::vector<float> depth(static_cast<size_t>(size) * size);
        glDisable(GL_SCISSOR_TEST);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        for (int i = 0; i < count; ++i) {
            SetTurntableFrame(g, i);
            Draw(lib, 0, 0, size, size, 0.0f);
            glFinish();
            glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glReadPixels(0, 0, size, size, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
            gif::Frame frame;
            frame.rgba.resize(rgba.size());
            for (int y = 0; y < size; ++y) { // GL rows are bottom-up, GIF rows top-down
                const uint8_t* src = &rgba[static_cast<size_t>(size - 1 - y) * size * 4];
                const float* d = &depth[static_cast<size_t>(size - 1 - y) * size];
                uint8_t* dst = &frame.rgba[static_cast<size_t>(y) * size * 4];
                for (int x = 0; x < size; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 0];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 2];
                    dst[x * 4 + 3] = (!g.transparent || d[x] < 1.0f) ? 255 : 0;
                }
            }
            frames.push_back(std::move(frame));
        }
        camera.yawDeg = savedYaw;
        spinDegrees = 0.0f;
        options = saved;
        return frames;
    }

private:
    std::vector<ScenePart> parts_;
    fig::Model unit_;               // the unit's parts as loaded, for Pose()
    std::vector<int> partSource_;   // parts_[i] is unit_.parts[partSource_[i]]
    fig::Vec3 constitution_{0.5f, 0.5f, 0.5f};
    std::map<std::string, GlTexture> textures_;

    // Grey and white squares behind the model, to show what the GIF will have transparent.
    void DrawCheckerboard(int width, int height) {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, width, 0, height, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        const int cell = 12;
        glBegin(GL_QUADS);
        for (int y = 0; y < height; y += cell) {
            for (int x = 0; x < width; x += cell) {
                float v = ((x / cell + y / cell) & 1) ? 0.62f : 0.78f;
                glColor3f(v, v, v);
                glVertex2i(x, y); glVertex2i(x + cell, y); glVertex2i(x + cell, y + cell); glVertex2i(x, y + cell);
            }
        }
        glEnd();
        glEnable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
    }

    // The lowest point of the model's bounding box once rotated, where the grid goes.
    float RotatedBottom() const {
        fig::Vec3 c = (boundsMin + boundsMax) * 0.5f;
        float bottom = c.z;
        for (int i = 0; i < 8; ++i) {
            fig::Vec3 p{(i & 1) ? boundsMax.x : boundsMin.x, (i & 2) ? boundsMax.y : boundsMin.y, (i & 4) ? boundsMax.z : boundsMin.z};
            p = p - c;
            p = fig::QuatRotate(orientation, p);
            bottom = std::min(bottom, c.z + p.z);
        }
        return bottom;
    }

    // A ground grid under the model, spaced to its size (items are 10 cm to 2 m long).
    void DrawGrid(float radius) {
        float step = std::pow(10.0f, std::floor(std::log10(std::max(radius, 0.001f) * 0.5f)));
        float z = hasModel ? RotatedBottom() : 0.0f;
        float cx = hasModel ? std::round(camera.targetX / step) * step : 0.0f;
        float cy = hasModel ? std::round(camera.targetY / step) * step : 0.0f;
        const int n = 20;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        glBegin(GL_LINES);
        for (int i = -n; i <= n; ++i) {
            float shade = (i == 0) ? 0.42f : 0.28f;
            glColor3f(shade, shade, shade + 0.02f);
            glVertex3f(cx + i * step, cy - n * step, z);
            glVertex3f(cx + i * step, cy + n * step, z);
            glVertex3f(cx - n * step, cy + i * step, z);
            glVertex3f(cx + n * step, cy + i * step, z);
        }
        glEnd();
    }
};
