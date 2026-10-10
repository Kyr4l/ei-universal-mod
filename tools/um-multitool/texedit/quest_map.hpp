// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Quest maps: the parchment pictures of the game's info panel (zone<N>quest.dds). A generator that paints one from
// the open map (water, hills, forests, ink icons for houses, towers, gates, bridges, walls and gold) on a procedural
// parchment, and the ink icons themselves (drawn here, resolution independent) for the Texture Editor's stamp tool.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../viewer/mmp_texture.hpp"

namespace mpr { struct Map; }

namespace questmap {

// Cheap value noise (hash lattice), shared with the editor's pattern fill. `period` > 0 wraps it (seamless).
uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed);
float ValueNoise(float x, float y, int period, int seed);

// What an object becomes on a quest map. Worked out from the game's own maps: goblin houses are stbuho4 / stbuho9;
// the kanian / stone bridges (stbr11 / 12 / 13 / 18 / 19) are the grey bar, every other bridge the red one.
enum class Kind { House, GoblinHouse, GoblinHouses, Tent, Shop, Tower, BridgeWood, BridgeStone, Wall, Tree, Exit };
const char* KindName(Kind k);
// The icons (by name in Icons()) an object kind is drawn with; one is picked by position, so a village is not all one house.
const std::vector<std::string>& IconsFor(Kind k);
constexpr int kKindCount = 11;

// What an object of the map becomes on a quest map, by its figure name (lower case): false = nothing.
bool Classify(const std::string& lowerFigure, Kind& out);

struct Marker { Kind kind; float x, y; float yaw; }; // yaw: the object's heading, radians
struct Input {
    const mpr::Map* terrain = nullptr; // valid while the generator runs
    std::vector<Marker> markers;
    std::string name;
};

// The three frames the game's quest maps use: a torn sheet (zone1-9), a scroll with rolled ends top and bottom
// (zone10-14) and one with rolled ends left and right (zone15-20).
enum class Frame { Torn, RollsTopBottom, RollsSides };
constexpr int kFrameCount = 3;
const char* FrameName(Frame f);

struct Options {
    int width = 256, height = 256;   // the picture (the game's quest maps are 256 x 256)
    int seed = 1;
    float roughness = 0.5f;          // of the torn edge
    Frame frame = Frame::Torn;
    bool relief = true, water = true, forests = false, icons = true, bridges = true;
    bool exits = true;               // the Exit markers (the quest's exit areas, given by the Map Editor)
    int iconScale = 0;               // pixels per icon pixel (0: by the picture's size; the game's textures are 256 wide)
};

// A blank parchment of `width` x `height` with the frame (the pixels outside it fully transparent).
void Parchment(mmp::Image& img, int width, int height, int seed, float roughness, Frame frame = Frame::Torn);
// An icon the generator would draw: for the editor's sprite layer (movable), when `icons` is given to Generate.
struct PlacedIcon { std::string name; int x, y, scale; };
// The parchment with the map painted on it. false (and `err`) when there is no terrain. With `icons`, the icons are
// not drawn but returned (the bridges and walls, drawn as lines, always go on the picture).
bool Generate(const Input& in, const Options& opt, mmp::Image& out, std::string& err, std::vector<PlacedIcon>* icons = nullptr);

// The base game's own map icons, cut out of its quest map textures (quest-icons/*.png, pixel for pixel).
struct NamedIcon { std::string name; mmp::Image image; };
const std::vector<NamedIcon>& Icons();
// `stamp` over `dst` (only where dst is opaque), centred at (cx, cy), every pixel `scale` x `scale` (pixel art: no smoothing).
void Composite(mmp::Image& dst, const mmp::Image& stamp, int cx, int cy, int scale);

} // namespace questmap
