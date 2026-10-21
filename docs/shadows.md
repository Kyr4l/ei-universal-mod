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
