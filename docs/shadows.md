# How the game draws shadows (reverse notes, 2026-10-21)

Read from game.exe (Ghidra decompile) and 2DintMMX.dll, checked on the test rig with um.dll's `UM_TRACE_D3D=1` tracer (`um_trace.cpp`: draw states, and the first shadow textures and meshes dumped to um-shadow-N.raw / .txt).

## Units (`CObject3DClientSpecific::Render` 0x51CE60, seed name UnitShadow::Render)
Per unit with shadows on (`g_0x79BD0C` > 0; per kind `g_0x79BD14` units, `g_0x79BD1C`, `g_0x79BD18` others), every frame:
1. the effective light direction: the sun (`g_0x7AF230..238`) combined with the point lights (objects of type 0x31) near the unit;
2. each figure part's triangles are projected to 2D in light space (`CFigure::EmitTriangles2D`) into a point buffer;
3. a scratch texture ("Format25", `CTextureManager::GetOrCreateFormat25`) is locked, cleared (8 KB = 64 x 64 x 16 bit) and the silhouette polygons are filled; the useful data is the top 4 bits of each 16-bit pixel (16 alpha levels, `0xF000` = full shadow);
4. in shadow quality 2 (`g_0x79BD0C == 2`) two separable 3-tap box blurs run over it (2DintMMX.dll, vtable slots 11 and 12 = 0x10003300 vertical, 0x10003400 horizontal, `* 0x5556 >> 16` = divide by 3, `& 0xF000`);
5. the texture is uploaded with `IDirect3DDevice7::Load` into the unit's own 64 x 64 video texture (`this + 0x7C`);
6. `CWorldClient::RenderShadowVolume` (0x56C750) builds the RECEIVER mesh: the terrain triangles inside the shadow volume of the light (`CTerrain::ClipAndAddTriangle`, clipped to the frustum), and, when asked, the triangles of the objects in the volume (bridges, buildings); the mesh is textured with the shadow texture (projective u, v in 0..1).

Draw state of the shadow mesh (traced): pre-transformed vertices (FVF 0x1C4, the game's own T&L), triangle list, Z test on, Z write off, alpha test off, alpha blend SRCALPHA / INVSRCALPHA, texture 64x64 16 bit, MAG / MIN filter LINEAR, address mode WRAP, COLOROP / ALPHAOP MODULATE.

## Terrain
The terrain has its OWN per-vertex sun shadow: `CTerrain::SetSunLight` 0x6C8ED0 (ray slopes from the light direction, relight scheduling), `CTerrain::TraceSunRay` 0x6C92C0 (a Bresenham walk along the sun direction over the sector grid, a vertex is shadowed when a vertex of the path is higher than the ray), `TerrainSector_LightVertexSun` 0x6C36B0 (ambient + sun per vertex). So terrain-on-terrain shade exists, at vertex resolution (cell-sized steps), and the unit / object shadow textures are drawn on top of it independently.

## What the tracer showed
The shadow texture is a smooth blob (blurred edges, ~3 px), the mesh follows the terrain relief. The cell-sized "crenellated" edges seen in some screenshots are therefore probably the terrain's per-vertex sun shadow (or the 64 x 64 resolution on very large casters), not the unit texture's filtering: to be separated by turning the game's shadow setting off at that spot.

## Footprints (not shadows) and the "pixelated and smooth" look
Footprints, blood and scorch marks are terrain decals (`CWorldClient::AddTerrainPrint` / 2 / 3, `CFootPrint`, `CBloodPrint`, `CFirePrint`; records built from terrain triangles clipped to the print's area, like the shadow receiver mesh), textured from the atlases `footprints`, `bloodprints`, `fireprints`. `prints.db` only holds opacity, lifetime and fade-out per terrain type and weather; the print's size comes from the unit's footprint type.
The crenellated, stepped look of a giant creature's footprint (ice golem) is in the ART: HD Lands' `HDTexRes/t_world_res/footprints.mmp` (1024 x 1024 DXT3, alpha only) has several large tiles that are low-resolution art with flat half-transparent 8 x 8 cells along their edges, magnified by the creature's size. A cleaned atlas (smoothing only around those cells) is in `_cpr/footprint-fix/`; the mod does not ship it (a report for the HD Lands authors).

## Unit shadow resolution (um-engine 0.6.0, `UnitShadow::Resolution`)
The 64 x 64 of the unit shadow texture is written as constants in game.exe: the two texture creators (0x692010, 0x692140: width / height), the silhouette scale `63.0 / extent` (two `fdivr` operands in Render), the clearing of the scratch texture (`mov ecx, 0x800` dwords), the two blur calls (`push 0x40` x 4) and the `0x200` in the target descriptor given to 2DintMMX's polygon drawer. um-engine patches all of them at once when the cfg value is 128 or 256 (`shadow_res.cpp`; all or nothing, byte-checked). Tested at 128 on the rig: the shadows draw correctly and the edge is only slightly smoother; the user judged the default 64 "just fine", so it stays off. 256 and the 0x200 = 8 x size guess are untested.

## Terrain and time of day (recon)
- The sun is live: `CTerrain::SetSunLight` (0x6C8ED0) takes the sun light object and the ambient colour, derives the ray slopes and per-step offsets from the light direction and, with a relight budget (`SetRelightBudget`), schedules the lit sectors for relighting with staggered countdowns (nearest first). So when the time of day moves the sun, the per-vertex terrain shadows follow, sector by sector.
- The per-vertex trace (`TraceSunRay` 0x6C92C0, `TerrainSector_LightVertexSun` 0x6C36B0) is binary: a vertex is either lit by the sun or gets the ambient colour only; the sun is ignored when its light +0xDC is positive (below the horizon). It runs only when the terrain flag +0x94 & 0x10 is set (`CTerrain::Load` sets +0x94 |= 0xFFFF, so it is on after every load). Its resolution is the terrain vertex grid (one step = one vertex), which is why terrain shadows are cell-sized and hard-edged.
- Unit / object shadows (Render 0x51CE60) are separate: a texture draped over the receiver mesh, with the light direction made of the sun plus nearby point lights (type 0x31); shadow quality is per kind (units 0x79BD14, kind 0x52 0x79BD1C, others 0x79BD18).

## The game's own terrain shadow is a hidden option, and um-engine can soften it (um-engine 0.7.0, `TerrainShadow::Softness`)
- The terrain sun shadow (TraceSunRay) runs only when the registry value `EnableSelfShadowing` (Terrain Settings, next to `ShadowingFreq` = the relight rate and `EnableDynamicLights`) is 1; it is 0 in the stock settings, so by default the terrain never casts shadows on itself. CWorldClient load (0x567AB0) turns the terrain flag 0x10 off when it is 0.
- With it on, the shadow is hard-edged at the vertex grid. `TerrainShadow::Softness=N` (um-engine.cfg) redirects the one TraceSunRay call (0x6C389F) to `terrain_shadow.cpp`: the ray is traced at the vertex and at four heights up to N game units above it, and the sun is scaled by the fraction that reaches the vertex, so a vertex that only just grazes the relief is partly lit. TraceSunRay's argument is the VERTEX (x, y, z floats), not a height. Heights below the vertex are not sampled (a point under the ground is always shadowed by its own cell).
- Compared on the rig at 17.5 h (base, gate shadow): `_cpr/shadow-compare/terrain-shadow-softness.png` (off / 1 / 2.5): 1 softens the edge a little, 2.5 more and makes the shadow fainter. Cost: 5 traces per relit vertex (relights are staggered by the game).
