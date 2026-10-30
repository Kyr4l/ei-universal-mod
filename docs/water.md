# The water (reverse engineering notes)

## How the game draws water
- **A second vertex layer.** Every terrain sector (`TerrainSector`, 0x394 bytes) has a ground layer and, when `sector+0x10` is set, a water layer: 0x441 (33 x 33) vertices each, one vertex per world unit, 32 units per sector.
  Vertex positions (12 bytes, x y z) are in the array at `sector+0x180` (water layer at +0x330C); the vertex records (0x54 bytes, 0x441 per layer) at `sector+0x194` (water layer at +0x16554).
- **The water is a sheet at the water's level over the whole basin.** A water vertex exists wherever the water layer is non-empty (`record+0x25` material index, `0xFFFF` = none), also where the ground rises above it:
  the depth buffer cuts the sheet at the shore (a hard intersection line: no border). `CTerrain::RenderWater` (0x6C6330, called at 0x56A112 in `CWorldClient::Render`, after `RenderObjects`) draws it with the passes `textureCount + 2 .. 2 * textureCount + 3` (water textures).
- **Record fields (water layer):** +0x15 specular colour (animated), +0x19 / +0x1D / +0x21 base x y z, +0x25 material (u16), +0x2B normal, +0x37 sun colour, +0x43 material colour, +0x4F alpha. **Ground record +0x27:** the water's depth over this ground vertex
  (water surface minus ground, negative on dry land: verified, e.g. 3.14 in the river, -0.45 on the bank). The water materials (`terrain+0x12C`, 0x38 bytes): +0 type, +4 / +8 / +0xC colour r g b (marsh 0.19 0.54 0.36, river 0.19 0.32 0.65), +0x10 alpha, +0x14 depth attenuation, +0x2C wave amplitude.
- **Lighting under water.** `TerrainSector_LightVertexSun` (0x6C36B0) attenuates the ambient and sun light of a ground vertex under water by depth^2 x material +0x14: that is the dark soft edge the shore has in the stock game.
- **Animation.** `TerrainSector_Animate` (0x6C3E80): every third frame the animated tiles (water textures: +0x364, 0x18 bytes each: UV destination, rotation, tile, frame, frame count, chance / 1000) advance one frame (random start frames, so
  neighbouring tiles are out of step); the water vertices get a wave offset (xy drift, z x 0.25) and a specular colour from a 512-entry sine table (`terrain+0x2A0`) driven by the wind and the phase (game-exe/terrain-lighting.md: the band travels with +0x288, +0x28C = -0.15 x wind per tick, and is drawn only on the positive half of the sine; the x / y drift uses a separate sin(col pi/4 + t/20) term; flat type-4 water never drifts; the 0.8 s stripes fit the 48-frame tile cycle at 60 FPS, test with EnableWaterWaves=0) table `terrain+0x290`
  (`CTerrain::BuildWaveTable` 0x6C57E0: 33 x 33 phases, 40 random radial bumps). Terrain flags (+0x94): 1 water waves, 2 animated tiles, 0x10 sun shadows of the terrain (`EnableSelfShadowing`, off in the stock settings).
  The long diagonal stripes that pop in and out every ~0.8 s (reported as shadow flicker) are this animation: the same cycle shows with the sun shadows, um-engine's patches and D7VK all switched off, and in the unmodified game (the user checked).

## A shore border was tried and removed
A "water border" (foam where the water meets the land: the shore found from the depth buffer plus a per-vertex map of the water's depth over the ground, drawn by a D7VK full-screen pass after `RenderWater`) was built in um-engine 0.15.0
and removed again at the user's request ("in the end i don't like it"). What is worth keeping from it: the water's depth over a ground vertex is `ground record +0x27` (negative on dry land), the water layer's vertex says where the sheet exists
(`record +0x25` != 0xFFFF) and at which level (`+0x21`), the liquid's colour is the material's r g b (`terrain+0x12C`, +4 / +8 / +0xC; lava is red), and the call after `CTerrain::RenderWater` is at 0x56A120. The code is in git history:
commits 088cfb09d and b8d3dffe3 (`water_border.cpp`, `dxvk_um_water_common.glsl`, D7VK `UmWaterMap` / `UmWaterEnd`).

