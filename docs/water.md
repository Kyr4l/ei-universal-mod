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
Foam where the water meets the land, drawn right after the game's water layer (the call after `RenderWater`, 0x56A120, patched like the sun shadows' sites; needs D7VK).
- um-engine collects, for the terrain in view (every 0.4 s, only sectors the game has loaded), per terrain vertex: the water's depth over the ground (ground record +0x27, -100 where the water layer has no vertex or the liquid is red = lava) and the water's level (+0x21 of the water vertex).
  The map is one R32G32F texture ((sectors x 32 + 1) squared) handed to D7VK when it changes.
- D7VK draws one full-screen pass (`dxvk_um_water_common.glsl`): every pixel's world position comes from the depth buffer (as in the shadow resolve); the depth map is sampled there (bilinear) and its gradient gives the distance to the waterline along the ground, so the band is as wide
  as asked in world units on any slope. Pixels well above the water's level (a bridge, units, trees) are left alone. Water side: a broken, slowly lapping line of foam right at the waterline plus slightly paler shallows; dry side: a thin wet (darker) strip. Flat marsh (slope under ~0.04) draws nothing: there is no sharp shoreline.
- Realism: the foam is near white but semi-transparent (<= 0.45 alpha at the default), broken up by two octaves of noise, lit like the world (ambient + sun on an upward surface: dim at night); the shallows tint is 16 % at most; the wet strip 30 %.

| `um-engine.cfg` key | Default | |
|---|---|---|
| `WaterBorder` | false | the call site (needs a restart; D7VK with `UmWaterMap` / `UmWaterEnd` required) |
| `WaterBorder::Width` | 0.35 | foam line width inside the water (world units) |
| `WaterBorder::Strength` | 0.55 | 0..1: how visible the shore is (0 = nothing drawn) |
| `WaterBorder::Wet` | 0.3 | wet strip width on the dry side |
| `WaterBorder::Speed` | 1 | movement of the foam |

Presets compared on the rig (`_cpr/water-samples/`): A subtle (0.22 / 0.35 / 0.25), B natural (0.35 / 0.55 / 0.3, the default), C visible (0.5 / 0.75 / 0.4).
D7VK research switch: the environment variable `UM_WATER_STAGE` (0 nothing, 1 only the map upload, 2 the pass; default 2).
