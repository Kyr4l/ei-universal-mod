# The water (reverse engineering notes) and the water border (um-engine 0.15.0)

## How the game draws water
- **A second vertex layer.** Every terrain sector (`TerrainSector`, 0x394 bytes) has a ground layer and, when `sector+0x10` is set, a water layer: 0x441 (33 x 33) vertices each, one vertex per world unit, 32 units per sector.
  Vertex positions (12 bytes, x y z) are in the array at `sector+0x180` (water layer at +0x330C); the vertex records (0x54 bytes, 0x441 per layer) at `sector+0x194` (water layer at +0x16554).
- **The water is a sheet at the water's level over the whole basin.** A water vertex exists wherever the water layer is non-empty (`record+0x25` material index, `0xFFFF` = none), also where the ground rises above it:
  the depth buffer cuts the sheet at the shore (a hard intersection line: no border). `CTerrain::RenderWater` (0x6C6330, called at 0x56A112 in `CWorldClient::Render`, after `RenderObjects`) draws it with the passes `textureCount + 2 .. 2 * textureCount + 3` (water textures).
- **Record fields (water layer):** +0x15 specular colour (animated), +0x19 / +0x1D / +0x21 base x y z, +0x25 material (u16), +0x2B normal, +0x37 sun colour, +0x43 material colour, +0x4F alpha. **Ground record +0x27:** the water's depth over this ground vertex
  (water surface minus ground, negative on dry land: verified, e.g. 3.14 in the river, -0.45 on the bank). The water materials (`terrain+0x12C`, 0x38 bytes): +0 type, +4 / +8 / +0xC colour r g b (marsh 0.19 0.54 0.36, river 0.19 0.32 0.65), +0x10 alpha, +0x14 depth attenuation, +0x2C wave amplitude.
- **Lighting under water.** `TerrainSector_LightVertexSun` (0x6C36B0) attenuates the ambient and sun light of a ground vertex under water by depth^2 x material +0x14: that is the dark soft edge the shore has in the stock game.
- **Animation.** `TerrainSector_Animate` (0x6C3E80): every third frame the animated tiles (water textures: +0x364, 0x18 bytes each: UV destination, rotation, tile, frame, frame count, chance / 1000) advance one frame (random start frames, so
  neighbouring tiles are out of step); the water vertices get a wave offset (xy drift, z x 0.25) and a specular colour from a 512-entry sine table (`terrain+0x2A0`) driven by the wind and the phase table `terrain+0x290`
  (`CTerrain::BuildWaveTable` 0x6C57E0: 33 x 33 phases, 40 random radial bumps). Terrain flags (+0x94): 1 water waves, 2 animated tiles, 0x10 sun shadows of the terrain (`EnableSelfShadowing`, off in the stock settings).
  The long diagonal stripes that pop in and out every ~0.8 s (reported as shadow flicker) are this animation: the same cycle shows with the sun shadows, um-engine's patches and D7VK all switched off, and in the unmodified game (the user checked).

## The water border (`WaterBorder`, `water_border.cpp` + D7VK `UmWaterMap` / `UmWaterEnd`)
A soft border where the water meets the land, drawn right after the game's water layer (the call after `RenderWater`, 0x56A120, patched like the sun shadows' sites; needs D7VK).
- um-engine collects, for the terrain in view (every 0.4 s, only sectors the game has loaded), per terrain vertex: the water's depth over the ground (ground record +0x27, -100 where the water layer has no vertex or the liquid is red = lava),
  the water's level (+0x21 of the water vertex) and the liquid's colour (its material's r g b). **Short shores are taken out** before the map goes to D7VK: the dry regions and the water regions of the map are labelled (flood fill); a region that does not reach unknown cells and
  whose edge is shorter than `MinEdge` world units (about four times the square root of its area: an islet, a stone in the shallows, a puddle) is turned into the other kind, so no shoreline is left there.
- D7VK draws one full-screen pass (`dxvk_um_water_common.glsl`): every pixel's world position comes from the depth buffer (as in the shadow resolve); the map is sampled there (bilinear) and its gradient gives the distance to the waterline along the ground, so the band is as wide
  as asked in world units on any slope. Pixels well above the water's level (a bridge, units, trees) and flat marsh (slope under ~0.04: no sharp shoreline) are left alone.
- **Colour:** relative to the liquid's own colour (the nearest vertex's material), lit like the world (ambient + sun on an upward surface: dim at night) and lightened, never white: the shallows 22 % toward white, the foam 22-50 %. The dry side gets a thin wet (darker) strip, 20 % at most.
- Styles (`WaterBorder::Style`): 0 soft (paler shallows and wet ground only, no foam), 1 patchy foam (the default: foam gathers in patches, a low-frequency noise picks where, the rest of the shore stays plain), 2 an even thin foam line.

| `um-engine.cfg` key | Default | |
|---|---|---|
| `WaterBorder` | false | the call site (needs a restart; D7VK with `UmWaterMap` / `UmWaterEnd` required) |
| `WaterBorder::Style` | 1 | 0 soft, 1 patchy foam, 2 even thin line |
| `WaterBorder::Width` | 0.25 | foam width inside the water (world units) |
| `WaterBorder::Strength` | 0.5 | 0..1: how visible the shore is (0 = nothing drawn) |
| `WaterBorder::Wet` | 0.3 | wet strip width on the dry side |
| `WaterBorder::Speed` | 1 | movement of the foam |
| `WaterBorder::MinEdge` | 24 | shores shorter than this (world units) get no border; 0 = every shore |

Checked on the rig (`ENGINE WATERSPOT` finds the best shorelines of a map; `_cpr/water-samples/`): the Gipath base and its quest allods (zone3xobr river / marsh, the other easy quests), the Jigran base (murky pond), the Ingos base (hardly any water: nothing drawn), and the Suslanger base, whose liquid is lava: no border at all (red liquids are skipped).
D7VK research switch: the environment variable `UM_WATER_STAGE` (0 nothing, 1 only the map upload, 2 the pass; default 2).
