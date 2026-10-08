// The look follows measurements of the game's own quest maps (zone7quest and the others, see _cpr/PENDING.md #93):
// the map fills the whole 256-pixel width with the frame cutting into it; the paper is a saturated ochre with a
// strong mottle; the cliffs are thin dark-orange outlines; water is a flat grey-green with a dark olive shore line;
// bridges are red bars drawn at 0 / 45 / 90 / 135 degrees; the burnt edge is a 10-pixel gradient to near-black.
#include "texedit/quest_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../mapedit/mpr_file.hpp"
#include "quest_icons_generated.hpp"

namespace questmap {
namespace {

float Clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
float Smooth(float a, float b, float v) {
    const float t = Clamp01((v - a) / (b - a));
    return t * t * (3 - 2 * t);
}
float Mix(float a, float b, float t) { return a + (b - a) * t; }
uint8_t To8(float v) { return static_cast<uint8_t>(Clamp01(v / 255.0f) * 255.0f + 0.5f); }

float Lattice(int x, int y, int period, int seed) {
    if (period > 0) { x = ((x % period) + period) % period; y = ((y % period) + period) % period; }
    return (Hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(seed)) & 0xFFFF) / 65535.0f;
}

struct Rgb { float r, g, b; };
Rgb MixRgb(const Rgb& a, const Rgb& b, float t) { return {Mix(a.r, b.r, t), Mix(a.g, b.g, t), Mix(a.b, b.b, t)}; }
Rgb Scale(const Rgb& c, float k) { return {c.r * k, c.g * k, c.b * k}; }

// Measured on zone7quest (means of the pixel classes).
const Rgb kPlain{205, 152, 45}, kLight{229, 187, 86};              // the paper: plain, and the lighter plateau tops
const Rgb kOutline{155, 53, 1};                                      // the cliff lines
const Rgb kWater{139, 141, 88}, kWaterEdge{133, 135, 78};            // water, and its first pixel at the shore
const Rgb kShore1{109, 108, 41}, kShore2{109, 74, 5}, kShore3{158, 98, 6}; // the land's three shore rings
const Rgb kBridge{139, 24, 0}, kBridgeOutline{66, 33, 1};
const Rgb kForest{86, 95, 32};
const Rgb kWallInk{52, 34, 18}, kDark{62, 30, 4}, kShadow{150, 94, 28}, kHighlight{240, 206, 118}; // the steepest cliffs' dark strokes; the slopes' shadow and lit sides
// The burnt edge, by distance from the torn boundary (ring 1 = the outermost opaque pixel).
const Rgb kEdge[10] = {{74, 38, 2}, {110, 80, 11}, {124, 97, 22}, {131, 105, 31}, {136, 110, 36}, {142, 116, 41}, {149, 121, 47}, {151, 123, 50}, {153, 127, 52}, {160, 133, 55}};

// What the water layer holds at a point: 0 nothing, 1 water, 2 marsh. A tile with a water material whose surface
// is above the land (hidden water lies under it); the material's colour tells water (blue) from marsh (green: the
// game's maps draw it as a forest, zone7's bottom left).
int WaterKindAt(const mpr::Map& m, float x, float y) {
    if (x < 0 || y < 0 || x >= m.Width() || y >= m.Height()) return 0;
    const int sx = std::min(static_cast<int>(x / 32.0f), m.sectorsX - 1), sy = std::min(static_cast<int>(y / 32.0f), m.sectorsY - 1);
    const mpr::Sector* s = m.At(sx, sy);
    if (!s || !s->water) return 0;
    const float lx = x - sx * 32.0f, ly = y - sy * 32.0f;
    const int c = std::min(static_cast<int>(lx / 2.0f), 15), r = std::min(static_cast<int>(ly / 2.0f), 15);
    const int mat = s->waterMaterial[r][c];
    if (mat < 0) return 0;
    const int vc = std::min(static_cast<int>(lx), 31), vr = std::min(static_cast<int>(ly), 31);
    const float k = m.maxZ / 65535.0f;
    if (s->waterVerts[vr][vc].z * k <= m.HeightAt(x, y) + 0.02f) return 0;
    if (mat < static_cast<int>(m.materials.size()) && m.materials[static_cast<size_t>(mat)].g > m.materials[static_cast<size_t>(mat)].b) return 2;
    return 1;
}
bool WaterAt(const mpr::Map& m, float x, float y) { return WaterKindAt(m, x, y) == 1; }

void Ink(mmp::Image& img, int x, int y, const Rgb& c, float a) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height) || a <= 0) return;
    uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    if (p[3] == 0) return; // off the paper
    p[0] = To8(Mix(p[0], c.r, a)); p[1] = To8(Mix(p[1], c.g, a)); p[2] = To8(Mix(p[2], c.b, a));
}

// The torn boundary: for each pixel its distance inside the opaque area (< 0: outside). The sheet is the whole
// `sw` x `sh` rectangle at the picture's top left, the tear eating a few pixels into it, as the game's maps have it.
void TornDepth(int w, int h, int sw, int sh, int seed, float roughness, float px, std::vector<float>& depth) {
    depth.assign(static_cast<size_t>(w) * h, -1.0f);
    const float amp = (1.0f + 4.0f * roughness) * px; // the tear's depth
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = std::min(x + 0.5f, sw - 0.5f - x), dy = std::min(y + 0.5f, sh - 0.5f - y);
            const float d = std::min(dx, dy);
            if (d < -1) continue;
            const float fine = ValueNoise(x * 0.45f / px, y * 0.45f / px, 0, seed) * 2.0f - 1.0f;
            const float coarse = ValueNoise(x * 0.08f / px, y * 0.08f / px, 0, seed + 11) * 2.0f - 1.0f;
            const float tear = amp * (0.55f + 0.5f * coarse + 0.45f * fine);
            depth[static_cast<size_t>(y) * w + x] = d - std::max(tear, 0.0f);
        }
}

// The paper's colour at a pixel before anything is drawn on it: plain ochre with the measured mottle.
Rgb Paper(int x, int y, float px, int seed) {
    const float blotch = ValueNoise(x * 0.035f / px, y * 0.035f / px, 0, seed + 21);      // big light / dark areas
    const float mottle = ValueNoise(x * 0.16f / px, y * 0.16f / px, 0, seed + 33) - 0.5f; // the parchment's clouds
    const float grain = ValueNoise(x * 0.7f / px, y * 0.7f / px, 0, seed + 3) - 0.5f;     // fine grain
    Rgb c = MixRgb(kPlain, kLight, Smooth(0.25f, 0.6f, blotch));
    c = Scale(c, 1.0f + mottle * 0.34f + grain * 0.14f);
    c.b = std::max(0.0f, c.b + (mottle + grain) * 70.0f); // the blue channel swings the most on the originals (0 to 90)
    return c;
}

// The burnt edge and the dark outline of the tear.
Rgb Edge(Rgb c, float depth, float px) {
    const float ring = depth / px; // in game pixels
    if (ring < 10.0f) {
        const int i = std::max(0, static_cast<int>(ring));
        const float f = ring - i;
        const Rgb a = kEdge[std::min(i, 9)], b = i + 1 < 10 ? kEdge[i + 1] : c;
        const Rgb target = MixRgb(a, b, f);
        const float strength = ring < 1.0f ? 1.0f : 1.0f - Smooth(1.0f, 10.0f, ring) * 0.85f;
        c = MixRgb(c, target, strength);
    }
    return c;
}

// A rolled end: the game's strip stretched to the length of a side (the caps keep their size).
void Roll(mmp::Image& out, const char* name, int x0, int y0, int length, int thick, bool vertical, float px) {
    const mmp::Image* strip = nullptr;
    for (const NamedIcon& n : Icons()) if (n.name == name) strip = &n.image;
    if (!strip) return;
    const int sw = static_cast<int>(strip->width), sh = static_cast<int>(strip->height);
    const int along = vertical ? sh : sw, across = vertical ? sw : sh;
    const int cap = 12, capPx = static_cast<int>(cap * px);
    for (int t = 0; t < thick; ++t)
        for (int l = 0; l < length; ++l) {
            int u = l < capPx ? static_cast<int>(l / px) : l >= length - capPx ? along - static_cast<int>((length - l) / px) - 1
                                                                                : cap + static_cast<int>((l - capPx) * static_cast<float>(along - 2 * cap) / std::max(1, length - 2 * capPx));
            u = std::min(std::max(u, 0), along - 1);
            const int v = std::min(static_cast<int>(t / px), across - 1);
            const uint8_t* s = &strip->rgba[(static_cast<size_t>(vertical ? u : v) * sw + (vertical ? v : u)) * 4];
            if (s[3] == 0) continue;
            const int x = x0 + (vertical ? t : l), y = y0 + (vertical ? l : t);
            if (x < 0 || y < 0 || x >= static_cast<int>(out.width) || y >= static_cast<int>(out.height)) continue;
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4];
            p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = 255;
        }
}

// A filled, outlined rectangle turned by `angle` (radians) about (cx, cy): the bridges.
void Bar(mmp::Image& out, float cx, float cy, float length, float width, float angle, const Rgb& fill, const Rgb& outline) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float r = std::ceil(std::hypot(length, width) * 0.5f) + 1.0f;
    for (int y = static_cast<int>(cy - r); y <= static_cast<int>(cy + r); ++y)
        for (int x = static_cast<int>(cx - r); x <= static_cast<int>(cx + r); ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float u = dx * c + dy * s, v = -dx * s + dy * c; // along, across
            const float d = std::max(std::fabs(u) - length * 0.5f, std::fabs(v) - width * 0.5f); // inside: negative
            if (d > 1.0f) continue;
            if (d > 0.0f) Ink(out, x, y, outline, 1.0f); // the outline sits outside the core
            else Ink(out, x, y, fill, 1.0f);
        }
}

// The sheet: frame geometry, paper, edge. `sw` x `sh` is the opaque area at the picture's top left.
void Sheet(mmp::Image& out, int W, int H, int sw, int sh, int seed, float roughness, Frame frame, float px, std::vector<float>& depth, int& roll) {
    out.width = static_cast<uint32_t>(W);
    out.height = static_cast<uint32_t>(H);
    out.rgba.assign(static_cast<size_t>(W) * H * 4, 0);
    roll = frame == Frame::Torn ? 0 : static_cast<int>(16 * px);
    TornDepth(W, H, sw, sh, seed, roughness, px, depth);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float d = depth[static_cast<size_t>(y) * W + x];
            if (d < 0) continue;
            Rgb c = Edge(Paper(x, y, px, seed), d, px);
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = To8(c.r); p[1] = To8(c.g); p[2] = To8(c.b); p[3] = 255;
        }
}

void Rolls(mmp::Image& out, int sw, int sh, int roll, Frame frame, float px) {
    if (frame == Frame::Torn || roll <= 0) return;
    if (frame == Frame::RollsTopBottom) {
        Roll(out, "roll-top", 0, 0, sw, roll, false, px);
        Roll(out, "roll-bottom", 0, sh - roll, sw, roll, false, px);
    } else {
        Roll(out, "roll-left", 0, 0, sh, roll, true, px);
        Roll(out, "roll-right", sw - roll, 0, sh, roll, true, px);
    }
}

} // namespace

uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float ValueNoise(float x, float y, int period, int seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const float a = Lattice(xi, yi, period, seed), b = Lattice(xi + 1, yi, period, seed);
    const float c = Lattice(xi, yi + 1, period, seed), d = Lattice(xi + 1, yi + 1, period, seed);
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

const char* KindName(Kind k) {
    static const char* const names[kKindCount] = {"House", "Goblin house", "Goblin houses", "Tent", "Shop tent", "Tower", "Wooden bridge", "Stone bridge", "Wall", "Forest", "Exit"};
    return names[static_cast<int>(k)];
}

const char* FrameName(Frame f) {
    static const char* const names[kFrameCount] = {"Torn sheet", "Rolled top and bottom", "Rolled left and right"};
    return names[static_cast<int>(f)];
}

bool Classify(const std::string& f, Kind& out) {
    auto starts = [&](const char* p) { return f.rfind(p, 0) == 0; };
    auto number = [&](size_t from) { return std::atoi(f.c_str() + from); };
    if (starts("stbuho")) {
        const int n = number(6);
        out = n == 4 ? Kind::GoblinHouse : n == 9 ? Kind::GoblinHouses
            : (n == 11 || n == 16 || n == 53 || n == 54) ? Kind::Tent     // tent00/01, armytent, runnerstent
            : (n == 17 || n == 18 || n == 56) ? Kind::Shop                 // shoppertent, paneltent
            : (n == 34 || n == 35 || n == 46) ? Kind::Tower                // kaniantower, necrotower
            : Kind::House;
    } else if (starts("jvhouse")) out = Kind::House;
    else if (starts("stbr")) {
        const int n = number(4);
        out = n == 11 || n == 12 || n == 13 || n == 18 || n == 19 ? Kind::BridgeStone : Kind::BridgeWood; // kanian / stone / hadagan / suspension: the grey bar
    } else if (starts("stwa")) out = Kind::Wall;
    else if (starts("nafltr") || starts("naflbu")) out = Kind::Tree;
    else return false;
    return true;
}

const std::vector<std::string>& IconsFor(Kind k) {
    static const std::vector<std::string> none;
    // The sprites by number (texedit/quest-icons/NNN.png, cut from the base game's and Lost in Astral's quest maps by
    // _cpr/claude-re/questmap/harvest4.py; quest-icons.tsv says which map each came from): paper-coloured, grey stone and
    // teal-roofed houses, green goblin huts, tents, awning shops, towers.
    static const std::vector<std::string> house{"248", "250", "251", "252", "254", "255", "256", "257", "258", "277", "280", "281", "288", "290", "187", "188", "192", "194", "196", "197", "199"},
        goblin{"028", "031"}, goblins{"029"}, tent{"021", "022", "024"}, shop{"108", "110", "111", "112"}, tower{"282", "283", "038"},
        exit{"026"};
    switch (k) {
    case Kind::House: return house;
    case Kind::GoblinHouse: return goblin;
    case Kind::GoblinHouses: return goblins;
    case Kind::Tent: return tent;
    case Kind::Shop: return shop;
    case Kind::Tower: return tower;
    case Kind::Exit: return exit;
    default: return none;
    }
}

const std::vector<NamedIcon>& Icons() {
    static std::vector<NamedIcon> icons;
    if (icons.empty())
        for (const IconData& d : kIcons) {
            NamedIcon n;
            n.name = d.name;
            n.image.width = static_cast<uint32_t>(d.w);
            n.image.height = static_cast<uint32_t>(d.h);
            n.image.rgba.assign(d.rgba, d.rgba + static_cast<size_t>(d.w) * d.h * 4);
            icons.push_back(std::move(n));
        }
    return icons;
}

void Parchment(mmp::Image& img, int w, int h, int seed, float roughness, Frame frame) {
    std::vector<float> depth;
    int roll = 0;
    const float px = std::max(1.0f, std::min(w, h) / 256.0f);
    Sheet(img, w, h, w, h, seed, roughness, frame, px, depth, roll);
    Rolls(img, w, h, roll, frame, px);
}

bool Generate(const Input& in, const Options& opt, mmp::Image& out, std::string& err, std::vector<PlacedIcon>* iconsOut) {
    if (!in.terrain || in.terrain->sectorsX <= 0 || in.terrain->sectorsY <= 0) { err = "no terrain is open in the Map Editor"; return false; }
    const mpr::Map& m = *in.terrain;
    const int W = std::max(opt.width, 64), H = std::max(opt.height, 64);
    const float px = std::max(1.0f, std::min(W, H) / 256.0f); // one game pixel
    // The map fills the picture's width (or height), its aspect kept, at the top left: the game's layout. The rolled
    // ends lie over the map's ends (zone13: a 448 x 320 map is a 256 x 183 sheet, rolls included).
    const int roll = opt.frame == Frame::Torn ? 0 : static_cast<int>(16 * px);
    const int ox = 0, oy = 0;                                                     // the map's origin
    const float scale = std::min(W / m.Width(), H / m.Height());                 // pixels per world unit
    const int sw = std::min(W, static_cast<int>(std::lround(m.Width() * scale))), sh = std::min(H, static_cast<int>(std::lround(m.Height() * scale)));
    std::vector<float> depth;
    int rollMade = 0;
    Sheet(out, W, H, sw, sh, opt.seed, opt.roughness, opt.frame, px, depth, rollMade);
    auto worldX = [&](float x) { return (x - ox) / scale; };
    auto worldY = [&](float y) { return (y - oy) / scale; };

    // --- the terrain, from smoothed fields: the originals are drawn by hand, with flowing lines and soft shading,
    // so the height and the water are blurred first and everything is read off the smooth versions
    const size_t N = static_cast<size_t>(W) * H;
    auto at = [&](int x, int y) { return static_cast<size_t>(std::min(std::max(y, 0), H - 1)) * W + static_cast<size_t>(std::min(std::max(x, 0), W - 1)); };
    std::vector<float> hgt(N, 0.0f), wat(N, 0.0f), mar(N, 0.0f);
    std::vector<uint8_t> inside(N, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float wx = worldX(x + 0.5f), wy = worldY(y + 0.5f);
            const bool in = wx >= 0 && wy >= 0 && wx < m.Width() && wy < m.Height();
            inside[at(x, y)] = in;
            hgt[at(x, y)] = m.HeightAt(std::min(std::max(wx, 0.5f), m.Width() - 0.5f), std::min(std::max(wy, 0.5f), m.Height() - 0.5f));
            const int kind = in && opt.water ? WaterKindAt(m, wx, wy) : 0;
            wat[at(x, y)] = kind == 1 ? 1.0f : 0.0f;
            mar[at(x, y)] = kind == 2 ? 1.0f : 0.0f;
        }
    auto blur = [&](std::vector<float>& f, int r) { // a box blur run twice: close to a Gaussian
        if (r < 1) return;
        std::vector<float> t(N);
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) { float sum = 0; for (int k = -r; k <= r; ++k) sum += f[at(x + k, y)]; t[at(x, y)] = sum / (2 * r + 1); }
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) { float sum = 0; for (int k = -r; k <= r; ++k) sum += t[at(x, y + k)]; f[at(x, y)] = sum / (2 * r + 1); }
        }
    };
    blur(hgt, std::max(1, static_cast<int>(std::lround(1.5f * px))));
    blur(wat, std::max(1, static_cast<int>(std::lround(1.2f * px))));
    blur(mar, std::max(1, static_cast<int>(std::lround(1.2f * px))));
    std::vector<float> broad = hgt;
    blur(broad, std::max(2, static_cast<int>(std::lround(6.0f * px)))); // the surroundings: a hilltop is what rises above them
    // The steep ground, blurred: each massif or plateau gets ONE outline, where this field crosses one half (the
    // originals outline the landform, not the height levels).
    std::vector<float> steep(N, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float gxp = (hgt[at(x + 1, y)] - hgt[at(x - 1, y)]) * 0.5f, gyp = (hgt[at(x, y + 1)] - hgt[at(x, y - 1)]) * 0.5f;
            steep[at(x, y)] = Smooth(0.3f, 0.6f, std::hypot(gxp, gyp) * scale) * (1.0f - Smooth(0.06f, 0.2f, wat[at(x, y)]));
        }
    blur(steep, std::max(1, static_cast<int>(std::lround(1.8f * px))));
    // forest density: trees per 3 x 3 units
    const float cell = 3.0f;
    const int gw = static_cast<int>(m.Width() / cell) + 1, gh = static_cast<int>(m.Height() / cell) + 1;
    std::vector<float> density(static_cast<size_t>(gw) * gh, 0.0f);
    if (opt.forests)
        for (const Marker& k : in.markers)
            if (k.kind == Kind::Tree) {
                const int gx = static_cast<int>(k.x / cell), gy = static_cast<int>(k.y / cell);
                if (gx >= 0 && gy >= 0 && gx < gw && gy < gh) density[static_cast<size_t>(gy) * gw + gx] += 1.0f;
            }

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = at(x, y);
            uint8_t* p = &out.rgba[i * 4];
            if (!p[3] || !inside[i]) continue;
            Rgb c{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
            const float mottle = ValueNoise(x * 0.4f / px, y * 0.4f / px, 0, opt.seed + 91) - 0.5f;
            const float w = wat[i];
            if (w >= 0.5f) {
                c = Scale(kWater, 1.0f + mottle * 0.1f);
                if (w < 0.62f) c = kWaterEdge;
                c = Edge(c, depth[i], px);
            } else {
                if (opt.relief && w < 0.14f) {
                    const float gxp = (hgt[at(x + 1, y)] - hgt[at(x - 1, y)]) * 0.5f, gyp = (hgt[at(x, y + 1)] - hgt[at(x, y - 1)]) * 0.5f; // per pixel
                    const float gpx = std::hypot(gxp, gyp), slope = gpx * scale;                                                          // per world unit
                    // soft shading of the slopes, lit from the top left like the originals
                    const float amount = Smooth(0.15f, 0.7f, slope);
                    const float dir = gpx > 1e-4f ? -(gxp + gyp) / gpx * 0.7071f : 0.0f; // +1 lit, -1 in shadow
                    c = MixRgb(c, kShadow, steep[i] * 0.3f);                              // the massif's brown body
                    c = dir < 0 ? MixRgb(c, kShadow, amount * -dir * 0.95f) : MixRgb(c, kHighlight, amount * dir * 0.7f);
                    // hilltops lighter
                    c = MixRgb(c, kLight, Clamp01((hgt[i] - broad[i]) * 0.5f) * 0.7f);
                    // the outline of the landform: where the blurred steepness crosses one half (anti-aliased by its gradient)
                    {
                        const float sx = (steep[at(x + 1, y)] - steep[at(x - 1, y)]) * 0.5f, sy = (steep[at(x, y + 1)] - steep[at(x, y - 1)]) * 0.5f;
                        const float g = std::hypot(sx, sy);
                        if (g > 1e-4f) {
                            const float dist = std::fabs(steep[i] - 0.5f) / g; // pixels to the crossing
                            c = MixRgb(c, kOutline, Clamp01(0.6f * px + 0.5f - dist) * 0.9f);
                        }
                    }
                    // the steepest: dark strokes
                    const float stroke = Smooth(0.4f, 0.7f, ValueNoise(x * 0.5f / px + y * 0.2f / px, y * 0.5f / px, 0, opt.seed + 77));
                    c = MixRgb(c, kDark, Smooth(1.8f, 2.8f, slope) * 0.7f * stroke);
                }
                // the shore: three rings on the land beside the water (measured on zone7quest), then a fade
                if (w > 0.36f) c = MixRgb(c, kShore1, 0.9f);
                else if (w > 0.24f) c = MixRgb(c, kShore2, 0.9f);
                else if (w > 0.14f) c = MixRgb(c, kShore3, 0.85f);
                else if (w > 0.06f) c = MixRgb(c, kShore3, 0.3f * (w - 0.06f) / 0.08f);
                // the marsh, and the forests when asked for: green blobs on the paper
                float f = mar[i] >= 0.5f ? 1.0f : 0.0f;
                if (opt.forests) {
                    const float wx = worldX(x + 0.5f), wy = worldY(y + 0.5f);
                    const float dn = density[static_cast<size_t>(std::min(static_cast<int>(wy / cell), gh - 1)) * gw + std::min(static_cast<int>(wx / cell), gw - 1)];
                    f = std::max(f, Smooth(0.5f, 3.0f, dn));
                }
                if (f > 0.05f && ValueNoise(x * 0.5f / px, y * 0.5f / px, 0, opt.seed + 57) > 0.55f - f * 0.25f) c = MixRgb(c, kForest, 0.9f);
            }
            p[0] = To8(c.r); p[1] = To8(c.g); p[2] = To8(c.b);
        }

    // --- the rolls over the sheet's ends
    Rolls(out, sw, sh, roll, opt.frame, px);

    // --- bridges: the segments of one bridge (within 8 units) become one red bar across the water, at the nearest
    // of 0 / 45 / 90 / 135 degrees, as the game's maps draw them
    if (opt.bridges) {
        struct Cluster { Kind kind; float yaw; std::vector<std::pair<float, float>> pts; };
        std::vector<Cluster> clusters;
        const mmp::Image* stone[2] = {nullptr, nullptr};
        for (const NamedIcon& n : Icons()) {
            if (n.name == "224") stone[0] = &n.image; // the grey stone bridge, across / along
            if (n.name == "226") stone[1] = &n.image;
        }
        for (const Marker& k : in.markers) {
            if (k.kind != Kind::BridgeWood && k.kind != Kind::BridgeStone) continue;
            Cluster* home = nullptr;
            for (Cluster& c : clusters)
                for (auto& q : c.pts)
                    if (!home && c.kind == k.kind && std::hypot(q.first - k.x, q.second - k.y) < 8.0f) home = &c;
            if (!home) { clusters.push_back({k.kind, k.yaw, {}}); home = &clusters.back(); }
            home->pts.push_back({k.x, k.y});
        }
        for (const Cluster& c : clusters) {
            float cx = 0, cy = 0;
            for (auto& q : c.pts) { cx += q.first; cy += q.second; }
            cx /= c.pts.size(); cy /= c.pts.size();
            float extent = 0;
            for (auto& a : c.pts) for (auto& b : c.pts) extent = std::max(extent, std::hypot(a.first - b.first, a.second - b.second));
            // the crossing: of the four directions, the one that meets the least water (a bridge crosses the river at
            // its narrowest); none wet: not a bridge over water, the game does not draw it
            int best = -1, bestWet = 1 << 30;
            for (int a = 0; a < 4; ++a) {
                const float ang = a * 3.14159265f / 4, dx = std::cos(ang), dy = std::sin(ang);
                int wet = 0;
                for (float t = -14; t <= 14; t += 1.0f) wet += WaterAt(m, cx + dx * t, cy + dy * t);
                if (wet > 0 && wet < bestWet) { bestWet = wet; best = a; }
            }
            if (best < 0) { // no water about: along the segments' axis, or the lone piece's heading
                float ang;
                if (c.pts.size() >= 2) {
                    float sxx = 0, syy = 0, sxy = 0;
                    for (auto& q : c.pts) { sxx += (q.first - cx) * (q.first - cx); syy += (q.second - cy) * (q.second - cy); sxy += (q.first - cx) * (q.second - cy); }
                    ang = 0.5f * std::atan2(2 * sxy, sxx - syy);
                } else ang = c.yaw;
                best = static_cast<int>(std::lround(ang / (3.14159265f / 4))) & 3;
            }
            const int iconScale = std::max(1, static_cast<int>(std::lround(px)));
            if (c.kind == Kind::BridgeStone && stone[0] && stone[1]) { // the game's grey stone bridge, across the river
                Composite(out, *stone[(best == 2 || best == 1) ? 0 : 1], static_cast<int>(ox + cx * scale), static_cast<int>(oy + cy * scale), iconScale); // bars across the bridge
                continue;
            }
            const float length = std::min(14.0f, std::max(8.0f, extent * scale * 0.8f + 4.0f)) * px, width = 3.0f * px;
            Bar(out, ox + cx * scale, oy + cy * scale, length, width, best * 3.14159265f / 4, kBridge, kBridgeOutline);
        }
    }

    // --- icons: the game's own sprites, one per village
    if (opt.icons) {
        const int iconScale = opt.iconScale > 0 ? opt.iconScale : std::max(1, static_cast<int>(std::lround(px)));
        std::vector<const NamedIcon*> imgs[kKindCount];
        for (int k = 0; k < kKindCount; ++k)
            for (const std::string& name : IconsFor(static_cast<Kind>(k)))
                for (const NamedIcon& n : Icons()) if (n.name == name) imgs[k].push_back(&n);
        static const int kCell[kKindCount] = {26, 44, 44, 24, 24, 24, 20, 20, 8, 8, 8};
        const int tw8 = W / 8 + 2;
        std::vector<std::vector<uint8_t>> taken(kKindCount, std::vector<uint8_t>(static_cast<size_t>(tw8) * (H / 8 + 2), 0));
        for (const Marker& k : in.markers) {
            if (k.kind == Kind::Exit && !opt.exits) continue;
            const int cx = ox + static_cast<int>(k.x * scale), cy = oy + static_cast<int>(k.y * scale);
            if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
            if (k.kind == Kind::Wall) {
                for (int dy = 0; dy < iconScale; ++dy)
                    for (int dx = 0; dx < iconScale; ++dx) Ink(out, cx + dx, cy + dy, kWallInk, 0.95f);
                continue;
            }
            const auto& list = imgs[static_cast<int>(k.kind)];
            if (list.empty()) continue;
            const int cellPx = std::max(8, kCell[static_cast<int>(k.kind)] * iconScale);
            bool crowded = false;
            for (int dy = -cellPx / 8; dy <= cellPx / 8 && !crowded; ++dy)
                for (int dx = -cellPx / 8; dx <= cellPx / 8 && !crowded; ++dx) {
                    const int tx = cx / 8 + dx, ty = cy / 8 + dy;
                    if (tx >= 0 && ty >= 0 && tx < tw8 && ty < H / 8 + 2 && taken[static_cast<size_t>(k.kind)][static_cast<size_t>(ty) * tw8 + tx]) crowded = true;
                }
            if (crowded) continue;
            taken[static_cast<size_t>(k.kind)][static_cast<size_t>(cy / 8) * tw8 + static_cast<size_t>(cx / 8)] = 1;
            const size_t which = Hash(static_cast<uint32_t>(k.x * 4), static_cast<uint32_t>(k.y * 4), static_cast<uint32_t>(opt.seed)) % list.size();
            if (iconsOut) iconsOut->push_back({list[which]->name, cx, cy, iconScale});
            else Composite(out, list[which]->image, cx, cy, iconScale);
        }
    }
    return true;
}

void Composite(mmp::Image& dst, const mmp::Image& stamp, int cx, int cy, int scale) {
    if (stamp.width == 0 || stamp.height == 0 || scale < 1) return;
    const int w = static_cast<int>(stamp.width) * scale, h = static_cast<int>(stamp.height) * scale;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = &stamp.rgba[(static_cast<size_t>(y / scale) * stamp.width + static_cast<size_t>(x / scale)) * 4];
            if (s[3] == 0) continue;
            Ink(dst, cx - w / 2 + x, cy - h / 2 + y, Rgb{static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2])}, s[3] / 255.0f);
        }
}

} // namespace questmap
