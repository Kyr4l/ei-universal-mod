# The world's look in game.exe: terrain, textures, materials, lights, sky, fog, decals, lightning, water

Written 2026-10-28 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x6BE000-0x6CC000
CTerrain and the terrain sector, 0x6CC000-0x6E0000 prints / bitmaps / the two small databases, 0x68C000-0x699000 lights, materials,
textures, registries, 0x4D6000-0x4DA000 clip buffer and sky, 0x4E1000-0x4E3000 fog of war, 0x466000-0x469000 lightning). It is the
reference for um-engine's lighting / shadow / water replacements and for um-multitool's map rendering. It builds on
`_cpr/shadows/terrain-light.md` (the 2026-10-24 lighting pass) and `receivers.md` (the shadow receiver mesh) and re-verified their
claims where this page repeats them; corrections to the older notes are in section 12.

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess / not resolved.
Addresses are VAs in game.exe; offsets are bytes; floats unless said.

## How to re-implement it (short version)

1. **The map is a grid of 32 x 32 unit sectors**, each a 33 x 33 vertex height field with 16 x 16 tiles of 2 x 2 units
   (`.mp` header + one `.sec` file per sector inside the zone's `.mpr` archive, section 2). A vertex is 8 packed bytes: x / y
   offsets of +-0.5 units in 1/252 steps, a 16-bit height (`* maxHeight / 65535`), an 11:11:10-bit normal. A tile is a 16-bit word:
   `rotation << 14 | tileId`, the tile id selects a texture (`id / tilesPerTexture`) and the cell inside it; an optional second
   vertex layer is the water sheet with a per-tile material (section 2.3). No baked light anywhere.
2. **Sectors live in an MRU cache** (`CTerrain::GetSector` 0x6C8140): a sector is loaded on demand, gets its mesh built
   (`TerrainSector_BuildMesh` 0x6C1C00: 0x54-byte vertex records, per-quarter per-texture strips of 9 triplets per tile) and is
   fully lit (`TerrainSector_Relight` 0x6C3A00) before it is returned; a sector unused for 240 frames is recycled (section 3).
3. **Lighting is per vertex on the CPU, no light ever reaches the GPU.** `diffuse = max(ambient, sun N.(-D) * sunColour)` (ambient
   is a floor, not an addend; under water both are darkened by depth^2), then `max` with every point light within radius
   (`1 - d^2/r^2`, wrap term for back faces), packed to D3DCOLOR; `specular = night-glow lights` with alpha = map-edge fade; the
   sector is drawn as pre-transformed FVF 0x1C4 vertices (section 4). The terrain-on-terrain shadow is a hidden option
   (`EnableSelfShadowing`, retail 0): a binary 95-step Bresenham ray march over the vertex heights (section 4.3).
4. **The sun** is one directional `CLight` rewritten every frame by `CWorldClient::UpdateDayLight` 0x566360 (azimuth = hour/12 pi,
   elevation 30..75 deg, never below the horizon; colours from `config/lights<world>.ini`, 24 entries); the terrain re-reads it
   once per game hour and every `LightingFreq/66` ticks (section 4.4).
5. **Water** is the second vertex layer of a sector, drawn after the objects with the passes `textureCount+2..`: a flat sheet at
   the material's level (movable through the script's water levels), animated every frame by a sine wave table along the wind
   and, every third frame, by cycling the tile's texture cell (section 5). The 0.8 s stripes of `docs/water.md` are the wave
   highlight band that the sine lookup turns on and off (section 5.4).
6. **Textures** are `.mmp` files from `textures.res` / `redress.res` / `menus.res` (or loose files, or a `.bmp` + alpha `.bmp`
   pair): the MMP header picks a DDPIXELFORMAT the device has, DXT data is copied raw when the device supports the FourCC and
   decoded in software otherwise, the mips are uploaded to a system-memory staging surface and `IDirect3DDevice7::Load`ed into a
   managed texture; a byte budget of 1/10 of the free video memory evicts the oldest textures (section 6).
7. **Sky** = a dome figure carried by the camera (z - 16), spinning slowly, recoloured per hour with alpha 0 at the horizon ring
   so it fades into the fog colour. **Fog of war** = a hash of 0.5 m cells drawn as particle quads that fade in / out. **Lightning**
   = noise-displaced polylines + glow quads. **Prints** (blood, footprints, scorch) = terrain triangles cut by the shadow prism
   machinery (`ClipBuf`) with the light straight down, textured from atlases (sections 7-10).

## 1. Conventions

| Thing | Value |
|---|---|
| world unit | the terrain vertex spacing; a sector is 32 x 32 units = 33 x 33 vertices; the AI grid of `pathfinding.md` is 0.5 of it ("tile" there = half a unit; the terrain "tile" here is 2 x 2 units = 16 x 16 per sector) [V] (`TerrainSector_Load` 0x6C13A0: `x = sx*32 + col`, `TerrainSector_BuildMesh`: record of tile (tx, ty) = vertex (2tx, 2ty)) |
| sector index | `sx = x >> 5`, local vertex `x & 31` (a coordinate that is a multiple of 32, other than 0, belongs to the previous sector as its vertex 32) [V] (`Terrain_RectToSectorRect` 0x6BF4A0, `CTerrain::GetVertex` 0x6C85A0) |
| map size | `sectorsX * 32`, `sectorsY * 32` units (floats at CTerrain +0x10C / +0x110) |
| z | up; packed as `ushort * maxHeight / 65535` (load) and `z * 65534 / maxHeight` (pack) [V] |
| texture page | one `.mmp` of `textureSize` texels holding `tilesPerRow^2` tiles of `tileSize` texels (retail: 512 texels, 64-texel tiles = 8 x 8 = 64 tiles per texture) [V] (`CTerrain::Load`); UV inset 8 texels per tile edge |
| colour | floats 0..1 in the records, packed as D3DCOLOR `A<<24 | R<<16 | G<<8 | B` for the draw [V] |
| vertex format | FVF 0x1C4 = XYZRHW, DIFFUSE, SPECULAR, TEX1 (32 bytes) [V] (`TerrainSector_Draw`) |
| time | "tick" = `CWorldClient::Update` (55 ms); "frame" = a render; `CTerrain +0x218` counts ticks (`CTerrain::Tick` 0x6CB1E0) [V] |
| light direction `D` | the way the light travels (the sun's z < 0); `-D` points at the sun [V] |

## 2. The map files: `CTerrain::Load` 0x6C68B0 and `TerrainSector_Load` 0x6C13A0

### 2.1 `<zone>.mp` (inside the zone's `.mpr` archive; the terrain header) [V]

`CTerrain::Load(mapPath, worldKey, isDungeon)`: `Unload`, opens the archive through the reader at +0x15C (vt[1]; file filter `*.mp`
0x674AC0), keeps the path (+0xFC) and the file stem (+0xF8), reads `<stem>.mp` (vt[5]) and checks `FatalError "Unable to open file"`
/ `"Wrong file format: "`.

| off | type | field | goes to |
|---|---|---|---|
| 0x00 | u32 | magic `0xCE4AF672` | |
| 0x04 | f32 | max height | +0x114 |
| 0x08 | i32 | sectors X | +0x104 (the sector grid +0x1F8 is allocated `sectorsX` columns of `sectorsY` slots) |
| 0x0C | i32 | sectors Y | +0x108 |
| 0x10 | i32 | texture count | +0x11C |
| 0x14 | i32 | texture size (texels) | +0x118 |
| 0x18 | i32 | tile type count | (array +0x13C) |
| 0x1C | i32 | tile size (texels) | +0x120; `+0x124 = textureSize / tileSize` tiles per row, `+0x128` = its square |
| 0x20 | u16 | material count | (array +0x12C) |
| 0x22 | u16 | animated tile count | (map +0x14C) |
| 0x24 | u16 | (read into the size check as the high half of the animated count: 0 in practice) | |
| 0x26 | material[count] | 11 dwords each (0x2C) | TerrainMaterial 0x38 (below) |
| | i32[tileTypeCount] | tile id -> tile type | +0x13C, followed by 16 entries of 0xF |
| | {u16 tileId, u16 frames}[animated] | animated tiles | +0x14C: `map[tileId] = frames` (dword per tile id, 0 = static) |

File size must be `0x26 + 4 * (11 * materials + tileTypes + animated)`.

**TerrainMaterial** (0x38 in memory, from the 11 file dwords) [V]:

| mem | file | meaning |
|---|---|---|
| +0x00 | w0 | type: 4 = flat water (no wave offset; its vertices snap to the ground x/y), other values wave; it is also the key of the water tile map (section 2.3) |
| +0x04..+0x0C | w1..w3 | r g b (marsh 0.19 0.54 0.36, river 0.19 0.32 0.65) |
| +0x10 | w4 | alpha |
| +0x14 | | `1 / (1 - alpha)`: the under-water absorption factor |
| +0x18 | | byte `round(alpha * 255)` = the water vertex alpha |
| +0x1C..+0x24 | w5 * rgb | `min(1, w5 * r,g,b)`: the water vertex base colour ("tint") |
| +0x2C | w6 | wave amplitude; also the priority when a vertex touches tiles of several materials (the lowest wins) |
| +0x30 | w7 | unused in the code read |
| +0x34 | | accumulated water level change (`CTerrain::SetMaterialHeight` 0x6CC7F0), 0 at load |
| | w8..w10 | not read |

Then `CTerrain::LoadTextures(textureCount)` 0x6C5AA0: for `i < count` the texture `"<stem>%3.3i"` (`CTextureManager::FindOrCreateNamed`
kind 0xB, refcount++), and for `i = count, count + 1` the optional lightmap pages `"<stem>L%2.2i"` (+ `.mmp`) only while the reader
(vt[21]) finds the file (+0xAE4 counts them; no retail map has them: `_cpr/shadows/terrain-light.md` section 8). Every entry gets a
`CMaterial` (kind 0x12: diffuse 0.3, alpha 1, ambient 0.7, specular 0.2, emissive 0, power 5; never used by the terrain draw, which
lights on the CPU) and `CTextureManager::LoadTexture` (section 6). Then the UV tables: 4 rotations x `tilesPerTexture` tiles at
+0x1C8 and 4 x `tilesPerRow` full-height strips at +0x1D8 (`Terrain_BuildTileUVs` 0x6C7940: 18 floats per tile = `u[9]` then
`v[9]` for the corners, edge midpoints and centre, inset by `8 / tileSize` of a tile = 8 texels, rotated by quarter turns, offset
by the tile's column / row; a page strip has v 0..1 and no row offset) [V]. `+0x100 = 1`, the ambient world name +0xAE0 (`Dungeon`
when isDungeon, else `Ingos` / `Suslanger` / `Gipat` by the world key), the ambient wav counts (table 0x7969D4), `flags +0x94 |= 0xFFFF`,
and the per-sector material height delta table +0xAEC (`sectorsX * sectorsY` records of 0x14: a float vector with one entry per
material).

### 2.2 `<stem>XXXYYY.sec` (one per sector, read on demand by `GetSector`) [V]

`TerrainSector_Load(sector, reader, sx, sy, textureCount, tilesPerTexture, stem, terrain)`: `"%s%3.3i%3.3i.sec"` through reader vt[5]
(`FatalError "Unable to locate resource: "`), header `u32 magic 0xCF4BF774, u8 flags` (bit 0 = water layer -> sector +0x10, `flags & 3`
-> +0x14), payload size `0x2408` (one layer) or `0x4810` (two) plus `0x200` when `flags & 3` (`"Wrong format resource: "`):

| offset (payload) | content |
|---|---|
| 0 | 0x441 packed vertices x 8 bytes: `i8 xOff, i8 yOff, u16 z, u32 normal` (ground layer) |
| 0x2208 | the same for the water layer (when bit 0) |
| after the layers | 0x100 x u16 tile words `rotation << 14 \| tileId` (ground), then 0x100 for the water layer |
| after the tiles (+0x4810 with water) | 0x100 x u16 water material per tile, 0xFFFF = none (-> sector +0x2B4 as int, -1) |

Unpacking: `x = sx * 32 + col + xOff * 0.5 / 126` (+-0.5 units in 1/252 steps), `y` likewise, `z = u16 * maxHeight / 65535`; the normal
is decoded by BuildMesh: `nx = ((d >> 11 & 0x7FF) - 1000) / 1000`, `ny = ((d & 0x7FF) - 1000) / 1000`, `nz = (d >> 22) / 1000`.
`TerrainSector_PackVertex` 0x6C4310 is the inverse (`z * 65534 / maxHeight`, `(x - grid) * 126 / 0.5`, `nz * 1000 << 22 | (nx + 1) * 1000
<< 11 | (ny + 1) * 1000`). A water vertex: the material is the lowest-+0x2C material among the tiles touching the vertex
(`CTerrain +0x1E8` links, section 2.3); type 4 -> the vertex takes the ground vertex's x / y (so the sheet's quads coincide with
the ground's), other types the grid position, no material -> grid position and z = 0. The sector's +0x18 keeps the mapped payload
(BuildMesh reads the normals and tiles from it). Flags |= 1.

### 2.3 Tiles, textures and the water material map [V]

`tileId = word & 0x3FFF`; `texture = tileId / tilesPerTexture` (sector +0x24); when that is >= `textureCount` the tile is in the page
area: `page = ((tileId - tilesPerTexture * textureCount) >> 3) + textureCount`, cell = `(tileId - ...) & 7`. The cell inside a
texture is `tileId % tilesPerTexture` (row-major, `tilesPerRow` per row). `CTerrain::GetTileUVs` 0x6C53C0 `(rotation, cell,
textureIndex)` returns the page table when `textureIndex mod (textureCount + 2)` is a page index (the water passes are offset by
`textureCount + 2`). The tile type (`+0x13C[tileId]`, 0..15, 0xF beyond the table) is the material class the AI, the ambient sounds
and prints.db use (`CTerrain::GetMaterialAt` 0x6C8D40: tile = `(x & 31) / 2, (y & 31) / 2`; the water layer's tile when the water is
above the ground; 8 outside the map).

`CTerrain::BuildVertexTileLinks` 0x6C7A80 (from `CTerrain::Init`, into +0x1E8, 0x441 x `{count, tile[4]}`): the 16 x 16 tiles touching
each of the 33 x 33 vertices (even/even vertices are corners of up to 4 tiles, odd/even edge midpoints of 2, odd/odd the centre of
1). The water tile map (sector +0x2B4, 256 ints) is indexed by the tile of a link.

## 3. The sector cache and the mesh

### 3.1 `CTerrain::GetSector(sx, sy, wantedFlags, load)` 0x6C8140 [V]

The grid +0x1F8[sx][sy] holds the node of the MFC `CList` at +0x1FC (head +0x200 = most recently used, tail +0x204 = oldest, count
+0x208). A hit whose stamp +0x384 is older than the frame counter +0x218 is restamped and moved to the head. A miss with `load`:
a new `TerrainSector` (0x394 bytes, `TerrainSector_ctor` 0x6BF5F0) unless the tail sector was not used for **0xF0 = 240 ticks**, in which
case the tail is recycled (and further stale tail sectors are deleted); `TerrainSector_Load`; `TerrainSector_EnsureState(wanted)`
0x6C3DD0 builds the mesh when flag 2 is wanted and missing and relights fully (`TerrainSector_Relight(sun +0x258, 0, 0, 0, 32, 32)`)
when flag 4 is wanted and missing. The sector is returned only when `flags & wanted != 0`. There is no byte budget: the cache holds
every sector touched in the last 240 ticks. Sector flags +4: 1 loaded, 2 geometry, 4 lit. `UnloadAllSectors` 0x6C84E0 empties
the list.

### 3.2 `TerrainSector_BuildMesh` 0x6C1C00 [V]

Per layer (ground, then water when +0x10): the 0x54-byte **vertex records** at +0x194 (0x441 per layer; the water layer at byte
+0x16554): normal +0x2B.., alpha +0x4F = 0xFF, base colour +0x43.. = 0, material +0x25 = 0xFFFF; water records get the material of
the first linked tile that has one, its alpha byte and tint, the base position +0x19.. (the vertex), and write the **ground**
record's +0x27 = water z - ground z (the depth; negative on dry land). zMin / zMax at +0x154 / +0x158.

| TerrainVertex (0x54, packed) | |
|---|---|
| +0x00 float[4] | screen x, y, z, rhw (TransformVertices, every frame) |
| +0x10 byte | clip flags (0 = fully inside) |
| +0x11 D3DCOLOR | DIFFUSE: a = +0x4F, rgb = the lit colour |
| +0x15 D3DCOLOR | SPECULAR: a = map-edge fade, rgb = glow lights / wave highlight |
| +0x19 float[3] | base position (water) |
| +0x25 u16 | material (water), 0xFFFF none |
| +0x27 float | water depth over this ground vertex |
| +0x2B float[3] | normal |
| +0x37 float[3] | cached sun colour |
| +0x43 float[3] | base colour (water tint) |
| +0x4F byte | alpha |
| +0x50 ptr | light-link list head |

Then the tiles: rotation byte -> +0x1B4, tile id -> +0x1A4 (dword per tile; +0x100 more for the water layer), per-quarter per-pass
tile counts (a local 4 x 0x100 table), the animated tile count, and the material histogram +0x374 (16 ints by tile type). The
**passes** are `2 * textureCount + 4`: ground textures, 2 pages, then the same for the water layer. The four **quarters** (8 x 8 tiles
= 17 x 17 vertices each, quarter `q = (tx >> 3) + 2 * (ty >> 3)`) each own a `CArray<CByteArray>` at `+0x2C4 + q * 0x14` (one byte
array per pass: per-tile edge flags 1 top row, 2 bottom, 4 left column, 8 right) and a `CArray<Vector10>` at `+0x314 + q * 0x14`
(one vector per pass of **9 triplets per tile** `{TerrainVertex*, u, 1 - v}` = the 3 x 3 vertices `(2tx + i, 2ty + j)` with the tile's
UVs from `GetTileUVs`). An animated tile (`+0x14C[tileId] != 0`) also gets an entry in +0x364 (0x18: triplets, rotation, cell,
random start frame, frame count, chance = `0x796960[tileType]`). Sector bounds +0x28.. (centre `sx*32+16, sy*32+16, zmid`; +-16;
zMin-1 .. zMax+1) and the 4 quarter boxes +0x64 + q*0x38 (+-8). `CTerrain::RelinkLightsInSector`, flags |= 2. With water: the
sector's water material list +0x15C and the terrain's height-delta vector for this sector (`+0xAEC[sector][material] = material
+0x34`), then `TerrainSector_ApplyMaterialHeightDeltas` 0x6C30E0 (raises the vertices of each material by the pending delta, zeroes
it; also run by Draw).

Earlier notes called the four groups "LOD strip groups": they are the four **quarters**; the LOD is applied at draw time (3.4).

### 3.3 Per frame: visibility, LOD, transform (`CTerrain::RenderSectors` 0x6C6460) [V]

`CWorldClient::Render` sets the visible sector rows / column ranges (+0xAA4 / +0xAAC / +0xAA8 / +0xAB8: `CTerrain::SetVisibleQuad`
0x6C98F0, the sectors whose centre lies inside the ground quadrilateral of the view) and calls `CTerrain::RenderOpaque` 0x6C6230
(passes `0 .. textureCount + 1`) before the objects and `CTerrain::RenderWater` 0x6C6330 (passes `textureCount + 2 .. 2 * textureCount
+ 3`) after them (call sites 0x56A06B / 0x56A112). Both push the terrain matrix (+0x54, identity + position) on the context's world
and view stacks and run `RenderSectors(ctx, first, last)`:

1. every visible sector: `GetSector(sx, sy, 4, 1)` (mesh + full relight if needed), `CTerrain::SelectSectorLod` 0x6C66E0 (squared
   distance of the sector centre to the camera g_0x79B2C4..: `< LOD1^2` (g_0x79BD2C, AppSettings `LOD1`) -> 0, `< LOD2^2` -> 1, else 2,
   never below the floor +0xAA0), `TerrainSector_CullFrustum` 0x6BFB20 (`CViewFrustum::CullBox` of the sector box -> +0x60 0 / 1 / 2,
   then of the four quarter boxes -> +0x144[q]);
2. visible sectors: `TerrainSector_UpdateLodSeams` 0x6BFB80 (+0x174 / +0x178 = 1 when the y-1 / y+1 neighbour is LOD 1 while we are
   LOD 0, or the x-1 / x+1 neighbour is LOD 2 while we are LOD 1), `TerrainSector_TransformVertices` 0x6BFC80 (per non-culled quarter:
   the 17 x 17 vertices, stepping 2 / 4 at LOD 1 / 2, both layers, projected by `0x4F29F0` (the 3DfpFPU pipeline: ctx +8, in g_0x7C88F8,
   out g_0x7C88E8, clip bytes g_0x7C88D4, count, quarter box); result -> +0x144[q] (1 = partially clipped); screen x y z rhw ->
   record +0x00, clip byte -> +0x10);
3. per pass: sectors with triplets for that pass in any quarter: `ctx vt[11](0, texture[pass mod (textureCount + 2)])` once, then
   `TerrainSector_Draw`.

### 3.4 `TerrainSector_Draw(sector, ctx, terrain, pass)` 0x6BFF40 [V]

Per quarter with +0x144[q] != 2 and triplets for the pass: the vertices are copied into the static buffer g_0x7C8890 (0x20 bytes
each: record +0x00..+0x0F, the 8 bytes diffuse + specular from +0x11, then `u, 1 - v` of the triplet) with the clip byte in
g_0x7C884C; LOD 0 takes all 9 vertices and 8 triangles of a tile, LOD 1 six vertices and 4 triangles (3 when the sector's seam flag
+0x174 / +0x178 meets the tile's edge flag 4 / 8), LOD 2 four vertices and 2 triangles (tables 0x79699C vertices / 0x7969A8
triangles per tile); indices into g_0x7C887C. A partially clipped quarter runs `ctx vt[37]` (clip) and compacts the vertices
(g_0x7C88B8 / g_0x7C88A8 -> g_0x7C8864). Then `ctx vt[34](vertices, n)` (perspective divide), render state 0x1D SPECULARENABLE = 1
(renderer g_0x7AF080 +0x54 vt[20]), `ctx vt[12]` = `I3DFigureImpl::DrawIndexedTriangles` 0x501BB0 (`DrawIndexedPrimitive`,
TRIANGLELIST, FVF 0x1C4), SPECULARENABLE = 0. Water sectors first apply the pending material height deltas. The texture stage
state is the terrain block g_0x7AEDDC (COLOROP MODULATE texture x diffuse, ALPHAOP SELECTARG1 texture): the diffuse alpha (water
material alpha) does NOT reach the blend through it; see `receivers.md` section 9 for the state blocks.

## 4. Lighting

Verified against `_cpr/shadows/terrain-light.md`; the formulas below are the ones in the code.

### 4.1 The relight pass: `TerrainSector_Relight(sector, sun, terrain, dynamicOnly, x0, y0, x1, y1)` 0x6C3A00 [V]

Water layer first (only vertices with a material), then the ground, inclusive ranges: for each vertex `terrain +0x24C/+0x250/+0x254`
= sector / column / row (for the ray trace), `LightVertexSun` unless dynamicOnly, then `LightVertexDynamic`; flags |= 4; a full
relight also sets the countdown +0x388 = -1. A full relight happens lazily in `GetSector(.., 4, ..)` (`EnsureState`) whenever flag
4 is missing; light changes relight sub-rectangles with `dynamicOnly = 1` (`CTerrain::RelightRect` 0x6CAB50: only sectors that are
lit; unlit ones get it with their full relight).

### 4.2 The sun term: `TerrainSector_LightVertexSun` 0x6C36B0 -> record +0x37 [V]

```
amb = terrain.ambient(+0x224..);  base = rec.+0x43..;  I = 1
ground vertex under water (water rec.+0x25 != 0xFFFF and rec.+0x27 >= 0):
    k = (1/15) * material[+0x14] * depth^2            (g_0x7C8908 = 1/15)
    amb *= max(0, 1 - k);  I = max(0, 1 - sun.+0x24^2 * k)   (sun +0x24 = the sun object's position z, 0: I stays 1 [I])
    base = max(base, water tint)
if (ground and terrain.flags & 0x10 and TraceSunRay(pos) == 0):  rec.sun = min(1, amb + base)
else:  rec.sun = max(amb, ApplyDirectional(sun, I, base, N))  per channel
```
`CLight::ApplyDirectional` 0x68D310: `t = -(D . N)`; `t <= 0 -> out = in`; else `out_c = min(1, in_c + t * I * colour_c)`. The sun has no
intensity but its colour; the ambient is a floor (a surface with `N.(-D) * sun < ambient` shows the ambient).

### 4.3 The terrain's own shadow: `CTerrain::TraceSunRay(vertex)` 0x6C92C0 [V]

Only with terrain flag 0x10 (`AppSettings EnableSelfShadowing` g_0x79BD3C, retail 0; `CWorldClient::LoadZone` clears the flag).
`SetSunLight` precomputes from the unit vector `n` towards the sun (components clamped to `|c| >= 0.01`): `+0x234 = nz/nx`, `+0x23C
= nz/ny`, `+0x244 = round(95 nx)`, `+0x248 = round(95 ny)` (g_0x7C8964 = 95). The trace is a Bresenham walk over the ground vertices
from the lit vertex's cell: `A = |+0x244|`, `B = |+0x248|`; per iteration an x step (`z += |nz/nx|`, col +- 1) when `2A - B >= err`
and / or a y step (`z += |nz/ny|`); sector borders cross through `GetSector(.., 1, 1)` (vertex 32 = next sector's vertex 0; an
unloaded or outside sector ends the ray as lit); the vertex is shadowed as soon as the ray's z is below a walked vertex's z
(the compare is on the float bits as unsigned: heights below 0 mis-order); lit after `max(A, B)` (<= 95) steps; a sun with
`D.z >= 0` shadows everything. Both slopes can be added in one iteration, so on diagonal suns the ray rises up to twice as fast
as the geometry says (shadows about half as long) [V, as decompiled]. Casters: the ground vertex heights only; receivers: ground
vertices only; the water layer never traces. What um-engine's `SunShadows` (docs/shadows.md) replaces is this plus the caster
bitmaps of `receivers.md`; the terrain still computes `rec.+0x37` and draws it, the GPU pass darkens on top.

### 4.4 The sun and the day: `CTerrain::SetSunLight(light, ambient)` 0x6C8ED0, `CTerrain::Tick` 0x6CB1E0 [V]

`CWorldClient::UpdateDayLight` 0x566360 (every frame) writes the sun `CLight` (g_0x7AF07C = render globals 0x7AF060 +0x1C):
`hour = fmod(tick * world+0x20 + world+0x1C, 24)`, `az = hour/12 * pi`, elevation `e = 60 - 3.75 |hour - 12|` deg for 4..20 h and
`30 + 45 * (4 - h)/4` / `30 + 45 * (h - 20)/4` outside (30..75 deg, never below the horizon), `D = (cos az cos e, sin az cos e, -sin e)`
-> light +0xD4..; colours from `config/lights<[cave]><world>.ini` (`[sunlight]`, `[ambient]`, `[sky]`, 24 entries, linear
interpolation; ambient = 0.8 x sun when its entry is 0,0,0) -> light +0x98.. and `CWorldClient +0x6AC..`; indoors `D = (0.5, 0.5,
-0.7071)` and the hour-0 colours. Once per integer hour it calls `SetSunLight(sun, &+0x6AC)`: ambient -> +0x224, ray slopes, light
-> +0x258, and the relight: budget +0x21C < 1 (`LightingFreq` < 66 ms) -> every loaded sector loses flag 4 (full relight at its next
draw); else the lit sectors are sorted (qsort 0x6C8E90 compares memory 8 / 12 bytes behind the slots: an arbitrary order) and
sector i of n gets `ScheduleRelight(round(i * (budget - 1) / n))` (0x6C3E30; `TickRelight` 0x6C3E50 clears flag 4 at 0).
`CTerrain::Tick` (slot 6, every tick from `CWorldClient::Update`): the countdown +0x220 reloads +0x21C and calls `SetSunLight` again
(a lit sector is relit about once per `LightingFreq`), ticks the countdowns, the ambient sounds, +0x218++, the wave phase, and
`TerrainSector_Animate` for every visible sector. `SetRelightBudget` 0x6C9290 = `LightingFreq / 66` from `LoadZone` (retail 1000 ms -> 15).

### 4.5 Point lights: `TerrainSector_LightVertexDynamic` 0x6C31C0 -> DIFFUSE +0x11, SPECULAR +0x15 [V]

```
c = rec.sun;  glow = 0
for each light L in rec.lightLinks (+0x50):
    V = L.pos - pos;  d = |V|;  atten = 1 (ground under water: max(0, 1 - material[+0x14] * depth^2 * (Vz/d)^2 / 15))
    out = ApplyPointWrap(L, atten, base, N, V/d, d):   k = 1 - d^2 * L.attenK(+0x104); k < 0 -> out = base
                                                       w = N.L <= 0 ? N.L + 1 : 1;  out_c = min(1, base_c + w * k * atten * L.colour_c)
    L.flags & 0x40 (night glow): glow = max(glow, out), sector.byte0 = 1;  else c = max(c, out)
c = max(c, glow)
DIFFUSE  = alpha(+0x4F) << 24 | round(min(1, c) * 255) rgb
SPECULAR = GetEdgeFadeAlpha(pos) << 24 | (sector.byte0 ? glow * 255 rgb : 0)
```
`CLight` (0x134 bytes; `CLight::CLight` 0x68CED0): +0x94 kind (1 point, 3 directional), +0x98 colour, +0x1C position, +0xD4
direction, +0xE0 radius (ctor `sqrt(FLT_MAX)`), +0x100 r^2, +0x104 `1/r^2`; `LuminanceAt` 0x68D730 = `0.3R + 0.6G + 0.1B`
(x `1 - d^2/r^2` inside the radius for kind 0x31). Lights are attached to vertices by `CTerrain::AddLight` 0x6CA5D0 (the hash +0x25C by
light id with the sector rect of `pos +- radius`; `TerrainSector_LinkLight` 0x6CAC10 pushes a 16-byte node on every vertex within
the 3-D radius; `RelightRect`), moved by `MoveLight` 0x6CA830 (relink over the union rect), removed by `RemoveLight` 0x6CA720; a sector
built later picks the existing lights up (`RelinkLightsInSector`). Point lights are never shadowed. Creators: hero / quest-item /
torch / spell / script / map lights (`terrain-light.md` section 6).

`CTerrain::GetEdgeFadeAlpha(pos)` 0x6CB020 (flag 4): `W = BorderFogDistance` (g_0x79BD34); the negative parts of
`(x - W - 1, y - W - 1, sizeX - W - x - 1, sizeY - W - y - 1)` -> `r = sqrt(sum^2)`, `alpha = 255 - round(255 * min(r, W) / W)`; the map
border fades out through the specular-alpha-as-fog path of 3DfpFPU (objects pass the same byte as their draw alpha).

### 4.6 Materials: `CMaterial` and `CMaterialManager` [V]

`CMaterial` (0x9C; `CMaterial::CMaterial` 0x68DBC0): name +0x1C; the D3DMATERIAL7 block +0x20 diffuse rgb, +0x2C alpha, +0x30 ambient,
+0x40 specular, +0x50 emissive, +0x60 power; a texture-frame `std::deque` at +0x64 (0x200-byte nodes) and texture names +0x8C / +0x94
for animated materials. `CMaterialManager::Init` 0x68E640 opens `materials.res` and reads one template per kind from the table
0x743BF8.. (u16 name length + name + 17 dwords; defaults 0.3 / 1.0 / 0.7 / 0.2 / 0 / 5 when the file is absent; kind 0x17 forces three
fields to 1.0) into the hash +0xBC (0x44-byte `{kind, 16 dwords}`); `CreateAndRegister(kind)` 0x68E970 creates a material through
vt[23] and copies the template. A material is bound for a figure draw by `CLight::PrecomputeMaterialTerms` 0x68D280 (`+0x114 =
intensity * material.diffuse * colour`, `+0x124 = intensity * material.specular * colour`, `+0x120 = alpha`) before the 3DfpFPU
vertex kernel (slot 40, replaced by um-engine `fp_light.cpp`); the terrain never binds its materials.

## 5. Water

### 5.1 Data [V]
The water is the sector's second vertex layer (section 2.2): a vertex exists wherever the tile map +0x2B4 gives one of its tiles
a material, including over ground that rises above the water (the z-buffer cuts the sheet at the shore: no border). The level
of a vertex is the `.sec` height of the water layer; flat (type 4) water snaps x / y to the ground vertex. The ground record's
+0x27 depth drives the under-water darkening of both the sun and the point lights (4.2, 4.5).

### 5.2 Levels [V]
`CTerrain::SetMaterialHeight(material, h)` 0x6CC7F0: `delta = h - material.+0x34`, `+0x34 += delta`, and every sector's height-delta
vector `+0xAEC[sector][material] += delta`; `TerrainSector_ApplyMaterialHeightDeltas` 0x6C30E0 raises the sector's vertices
(+0x180 z and record +0x21) of that material when the sector is next built or drawn. The only caller is `WaterLevels::Read` 0x6D45D0
(the 16-byte records `{materialId, stepsLeft, level, step}` of `CWorld +0x70`, animated on the server by `WaterLevels::Tick`
0x6D44E0 and set by the script through `WaterLevels::SetTarget` 0x6D4410): this is how the script's water levels move the sheet.

### 5.3 Animation: `TerrainSector_Animate(sector, terrain, frame)` 0x6C3E80 [V]
Every frame, when terrain flag 1 (`EnableWaterWaves`) and the sector has water, for every water vertex with a material:
```
angle = wavePhase(+0x288) + windDir.x(+0x274) * col + windDir.y(+0x278) * row + phaseTable(+0x290)[row][col]
s = sineTable(+0x2A0)[round(angle * 256 / pi) & 0x1FF]            (512 entries, sin(i pi / 256): one cycle per 512)
specular +0x15 rgb = s >= 0 ? s * windStrength(+0x284) * material.amplitude(+0x2C) * 50 * material.rgb : 0   (alpha kept)
amp = waveAmplitude(+0x280) * material.amplitude
type 4: position unchanged (x/y drift only when rec.+0x27 < 0, which the water record never has [I])
else:   z = base.z + s * amp * 0.25;  x = base.x + sin(col pi/4 + t 0.05) * amp * 3;  y = base.y + sin(row pi/4 + t 0.05) * amp * 3
```
with `t = g_0x7AF61C + g_0x7AFCA8` (the frame time counters). `CTerrain::Tick` advances `+0x288 += +0x28C` every tick; `SetWindStrength(f)`
0x6C7D70 sets `+0x284 = clamp(f)`, `+0x280 = 0.3 f`, `+0x28C = -0.15 f` and scales the wind direction by `0.449 (pi/2 + asin(..))`; the
phase table is 40 random radial bumps of amplitude pi/2 with wrapping borders (`BuildWaveTable` 0x6C57E0, built once per
`CTerrain::Init`). Every third frame, when terrain flag 2, the animated tiles (+0x364) advance: an entry mid-cycle, or at frame 0 when
`rand % 1000 + 1 <= chance` (chance = `0x796960[tileType]`; >= 1000 = continuous), takes `frame = (frame + 1) % frameCount` and rewrites
the u / 1-v of its 9 triplets with the UVs of `cell + frame` (the animation frames are the next cells of the same texture).

### 5.4 The stripes of docs/water.md [I from 5.3]
The specular highlight is a plane wave along the wind (`windDir.x * col + windDir.y * row`), travelling with `+0x288`, and it is
drawn only where `s >= 0`: a crest band appears, moves, and vanishes every half cycle. The cycle length is `512 / (256/pi *
|+0x28C|)` ticks: with the retail wind strength 0.4 (`DefaultWindForce`), `|+0x28C| = 0.06`, one cycle is 2 pi / 0.06 = 105 ticks
(5.8 s) and a band is visible for half of it; with a stronger wind the bands are faster. The tile cycling (every 3rd frame, 16
frames = 48 frames = 0.8 s at 60 FPS) is the other periodic change. The user saw a 0.8 s pop with everything else off, which
matches the **tile cycle** better than the sine band; the band is what moves diagonally. Not settled from the code alone: whether
the water textures' animation frames differ enough to "pop" (they are texture cells; the random start frame per tile desynchronises
neighbours). A test: `EnableWaterWaves = 0` removes the sine band (flag 1), terrain flag 2 the tile cycle.

### 5.5 Draw
`CTerrain::RenderWater` (passes `textureCount + 2 ..`, after `RenderObjects`), the water records' DIFFUSE alpha = the material's
alpha byte; the state block of the pass is set by `CWorldClient::Render` (see `receivers.md` section 9: whether the diffuse alpha
reaches the blend depends on the block, the terrain block selects the texture alpha) [?].

## 6. Textures: `CTexture` and `CTextureManager`

### 6.1 Objects [V]
`CTexture` (0x68, created by `CTextureManager::CreateByKind` 0x691810): CObject header, name +0x1C, width / height +0x20 / +0x24 (of the
loaded level 0), byte size +0x28, the MMP extra block +0x2C (CArray<byte>), DDPIXELFORMAT +0x3C (dwSize 0x20, chosen by
`CRenderDevice::FindTextureFormat`), surface +0x5C (`CDXDirectDrawSurface*`, refcounted; +0x10 of it = `IDirectDrawSurface7`), +0x60
pixel map (never set by the mmp path), +0x64 last-use time. Flags +8: 8 loaded; 0x40 / 0x80 / 0x100 / 0x200 by kind at registration
(`RegisterInstant` 0x691AA0: default / 0x3E / 0x3F / 0x40) -> DDSCAPS2 OPAQUE / HINTSTATIC / HINTDYNAMIC; 0x4000..0x20000 = the
header's alpha bits 0 / 1 / 4 / 8 after a load.

`CTextureManager` (`CTextureManager::CTextureManager` 0x691140; global g_0x7AF744): the loaded-texture `CList` +0xC0 (head = oldest),
+0xD8 bytes in use, +0xDC budget, +0xE0 the shared 64 x 64 shadow scratch texture, +0xE4 device, +0xE8 mip skip (AppSettings
g_0x79BCDC, 0..3), +0xEC small-texture flag. `Init` 0x691500 deletes the loose `textures\*.mmp` skin cache and opens `textures.res`,
`redress.res`, `menus.res`; `SetDevice` 0x6913A0 sets the mip skip, +0xEC = (max texture size < 257) and the budget =
`GetAvailableVidMem(TEXTURE | LOCALVIDMEM) / 10`.

### 6.2 Load path [V]
`CTextureManager::LoadTexture(texture)` 0x6922A0 (19 callers: terrain, prints, figures, UI):
1. loaded (flag 8) -> done.
2. `"<root g_0x79BC90>textures\<name>.bmp"` exists -> the **BMP path**: pixel format by kind (1 / 2 / 3 / 4 / 8 -> ARGB1555 id 0x5551,
   6 / 0xD -> 4444, else RGB565 0x5650; `FatalError` when the device lacks it), `CTexture::LoadBmpPair` 0x690260 -> `CTexture::LoadFromBmp`
   0x68FBF0: the 24-bit BMP (`CBitmapDib::LoadStream`) and the optional 8-bit alpha BMP `"<dir>\<prefix 0x795BE4><name>.bmp"`, packed per
   pixel with `PixelFormat_PackRGBA` into a 16-bit system-memory surface, `IDirect3DDevice7::Load` into the texture surface.
3. else the **MMP path**: `"<name>.mmp"` from the archives (manager vt[19], released with vt[20]) or the loose file
   `"%stextures\%s.mmp"`; `CRenderDevice::FindTextureFormat(header)` -> DDPIXELFORMAT +0x3C (fourcc 0x8888 falls back to 0x4444 when
   unsupported); first level = `min(mipSkip, mips - 1)`; `CTexture::LoadMipChain` 0x690430.

`LoadMipChain(device, header, firstLevel)`: w / h = header >> first (more levels skipped while > 511 with +0xEC); levels = `mips -
first` (1 when AppSettings g_0x79BCE0 mipmapping is off); a system-memory staging surface (DDSURFACEDESC2 flags 0x1007 (+0x20000
MIPMAPCOUNT), caps `TEXTURE | SYSTEMMEMORY` 0x1800 / 0x401808 with mips) and the managed texture +0x5C (caps 0x11000, caps2
`D3DTEXTUREMANAGE` 0x20000 | the kind hint); `CTexture::UploadMip` 0x68FA10 fills the staging chain: FourCC `DXT*` copied raw
(`CopyToSurface` 0x690970, per level `GetAttachedSurface`) when the device format has `DDPF_FOURCC` and the same FourCC, otherwise
decoded in software (`UnpackToSurface` 0x690D30, mode DXT1 -> 565 / 555 / 5551 = 0 / 1 / 2, DXT2 / DXT3 -> 4444 = 3 / 4; `"Can't find
decode format."` for anything else); 0x8888 sources through `CopyToSurface32` 0x690B30 (row conversion to the 16-bit target); the
16-bit formats copied; then `IDirect3DDevice7::Load(texture, 0, staging, 0, 0)` under the DDraw critical section, flags |= 8, surface
`SetPriority`-like vt[45](100), and the header's extra block copied to +0x2C. `MmpHeader::GetMipOffset(level)` 0x6908E0 = `hdr[0x48] +
(w * h * bpp / 8) * (4^n - 1) * 4 / (3 * 4^n)` from the end of the 0x4C header (`hdr[0x48]`, the "reserved" field of
`mmp-format.md`, is the size of an extra block between the header and level 0; it is 0 in the files seen and the block's meaning
is unknown [?]). Pixel formats: `PixelFormat_ARGB1555 / 4444 / 8888` 0x693850 / 0x6938B0 / 0x693910 build the mask descriptors of
`mmp-format.md` (QU / DD / 0x8888).

### 6.3 Cache [V]
`CTextureManager::TrimCache` 0x694020 (every 32 ticks from `CWorldClient::Update` 0x56477C): while `bytesInUse(+0xD8) > budget(+0xDC)`
the head of the list (the oldest texture) is unlinked, its size (+0x28) subtracted and the texture deleted. `PurgeOlderThan` 0x692E20
deletes by the +0x64 stamp. `FindByName` 0x693C60 (case-insensitive, no load), `FindOrLoad` 0x692D30, `FindOrCreateNamed(kind, name)`
0x691D00 are the lookups; `ReloadAll` / `ReloadFlaggedAll` rebuild after a device reset. The composed unit skins:
`ComposeLayers(dest, names, size)` 0x693010 blends each layer's archive `.mmp` (`2DintMMX` g_0x7AF06C vt[5](dest, pixels = hdr + 0x4C +
hdr[0x48], size^2, isPNT3)`; `"Texture %s dimentions are incorrect"` fatal) and `ComposeAndSaveLayers` 0x693180 writes the result as
`textures\<name>.mmp` in ARGB1555 / 4444 / 8888 (the cache `Init` deletes).

### 6.4 Registries, briefly [V]
`CFileRegistry` (`Open` 0x6945A0: `CMappedFile`, magic 0x45AB3EFB; `FindData(section, key, type)` 0x6946D0: hash = sum of
`tolower(c)` % bucket count, 6-byte buckets `{u16 next, u32 offset}`, section then key tables, names with u16 lengths compared
case-insensitively, key entry `{u8 type, u16 len, name, value}`, types 0 int / 1 float / 2 string / 0x80 float[] / 0x81 int[] /
0x82 string[]; `reg-format.md`); `CIniFile` = `GetPrivateProfileString` wrappers (`GetString` 0x696030, `GetIntArray`, `GetVec3`,
`WriteString` 0x696E60 ...); `CWinRegistry` = `RegOpenKeyEx / RegQueryValueEx` wrappers (`GetDWord` 0x698610, `GetBinFloat` 0x698750,
`GetString` 0x6976B0, the Set* twins) under the HKCU key the AppSettings use.

## 7. Sky: `CSky` (CWorldClient +0x1AC) and `CSkyObject` [V]

`CSky::CSky` 0x4D9680 (from `CWorldClient::CreateSky` 0x562F70): `{+0 CSkyObject* figure, +4 0, +8 CTextureHolder kind 0x3D "sky00",
+0xC rotate = 1}`. `CSky::Render(ctx, cameraPos)` 0x4D96E0 (the first draw of `CWorldClient::Render`, 0x569FB7): the figure is placed
at `(camera x, camera y, camera z - 16)`, rotated about Z by `Game_GetTimeSum() / 3000` rad, its matrix +0x54 rebuilt
(`Quat_ToMatrix4` 0x4D9AA0) and pushed on the context's world stack and `MatrixStack_Push(index 1)` 0x4D9B80, the texture bound
(`ctx vt[11]`), `ctx vt[14] DrawFigure(figure, g_0x7AA890, g_0x7AA8A0, 0, 0xFF, 0)`. `CSkyObject` (`ctor` 0x4D9110: a `CFigure` with
SoA position / normal arrays +0x1C0 / +0x1D0, index arrays, per-vertex colour arrays +0x138 (diffuse) / +0x148 (specular), a
horizon table +0x220). `CSkyObject::Recolour` 0x4D9360 (through `CSky::Recolour` 0x4D9DE0 from `UpdateDayLight`, 0x566633 indoor /
0x566B9C outdoor, with the hour's colours): the horizon table (`sqrt(-normal.z)` for vertices whose position z >= 1, else 0) is
rebuilt when the vertex count changed; a horizon-ring vertex gets diffuse `(r g b, alpha 0)` and specular 0, every other vertex
diffuse `0xFF r g b` and the second colour; the dome fades into the fog colour (render globals +0x14) at the horizon. The dome
mesh itself is a figure resource (`sky00` texture, the figure name is in the holder's model [?]).

## 8. Fog of war: `CVisibilityFog` / `CVisFogManager` [V]

`CVisibilityFog` (0x69; `CVisFogManager::CreateByKind` 0x4E2290; created by `CreateAndRegister` 0x4E28D0 into the manager's list +0xB8):
a hash map +0x54 of cells keyed by `(y << 16) | x` of the 0.5 m AI tile; bounds +0x1C.. / +0x30 / +0x3C. `RebuildCells(grid {x0, y0,
rows, w, h})` 0x4E1C60: every cell is marked stale, every non-zero grid byte revives or creates its cell: `{+8 short x, +0xA short y,
+0xC world x = 0.5 (x + rand/2^33 - 0.25), +0x10 world y, +0x14 z = world height (vt[30]) + 0.5, +0x18 alpha 0, +0x1C alive, +0x1D
sprite = rand % 15}` (0x20-byte nodes). `Tick` 0x4E1AB0 (slot 6, per tick): alive cells ease `alpha = 1 - (1 - alpha)/2`, stale cells
halve it and are removed below 0.01; the sprite index cycles `(s + 1) % 15`; the fog dies (returns 0) when its refcount < 2 and
no cells remain (`CVisFogManager::Tick` 0x4E2760 unlinks it). `Render(ctx)` 0x4E1EB0: `ComputeBounds` 0x4E1770 when > 12000 cells
and stale bounds, `CViewFrustum::CullBox`, then one particle quad per cell (`{x, y, z, size 0.4}` into g_0x7ABA30, `{colour 0xFFFFFF
| round(alpha * 255) << 24, uv record g_0x7ABA58 + sprite * 4}` into g_0x7ABA44) submitted with `ctx vt[17] DrawParticleQuads`.
`CVisFogManager::RenderAll` 0x4E2700 binds the fog atlas texture +0xB0 first. The byte grid comes from the server's visibility
(`CEffectShowVisibility` / player sight, not read here).

## 9. Lightning: `CLightning` (0x514) [V]

`Lightning_LoadNoiseTables` 0x468A00 reads `gfx.res\lightning.dat` = 32 tables x 400 floats (0xC800 bytes) into g_0x79CE78
(the harmonic noise bands `LightningNoise_BuildTable` 0x4660B0 describes: sums of sines in the bands 1..5 / 10..25 / 25..100 /
100..200 / 200..400 with amplitudes 0.1..0.001). `CLightning::Init(from, to, type, demon)` 0x4680D0: `+0x50C type` (> 0 thin spell
bolt, 0 random-branch bolt, < 0 static), `+0x510 amplitude` (0.3 / 0.05; 0.05 when |dz| == 30), the main segment +0x5C
(`LightningSegment_Init` 0x466820: origin, `dir = (to - from) / count`, side / up orthonormal scaled to min(len, 15), 3 noise seed
pairs `LightningSeed_Init` 0x466190, 16-byte points), width +0x100 = |type| (x1.5 if < 0, 4 if 0), six branches +0x104 (0xA8 each): type
0 between jittered copies of from / to (+-5 % of the length, random widths 2.45..5.95), otherwise from `lerp(from, to, {0.2, 0.3,
0.5, 0.8, 0.9, 0.9})` to `to` with widths `(0.7 + rand) * |type| / 2`; `Update`, `ComputeBounds` 0x467150 (+-1), the textures
`SmtgLight` / `Lightning` or `DemonLight` / `DemonLightning` (6 frames each). `Update` 0x4672E0: `UpdatePoints` 0x466CE0 of the main
segment (time / 4, scale 1.5 / 1 / 2 by type) = `EvalNoise` 0x4662C0 (the seeds advanced by whole time steps, a quadratic B-spline
blend of the tables per point index) -> `point i = origin + i dir + dx side + dy up`; branches re-anchored at sine-animated
offsets (frequencies 0x787C90) along the main bolt and re-aimed; their points likewise. `Render(ctx)` 0x467AD0: cull box, `Update`
when type >= 0, `ctx vt[16] DrawTexturedPolyline(points, width, .., |dir| * count * 0.6, random frame 0..3 * 0.25, 0.25)` for the main
bolt and the 6 branches (static bolts without the width / frame terms), then `RenderGlow` 0x467590 at the far end (and at the
origin for type 0): 8 quads along the bolt direction sized `size * sqrt(1 - t^2)` (size = clamp(len * 0.11, 0.5, 1.2) for spell
bolts), frame `(world tick / 2) % 15` of a 4 x 4 atlas, `DrawParticleQuads`. `CEffectLightning` (kind 0x4D) owns it; weather flashes
come from `CWorldServer::UpdateEnvironment`.

## 10. Prints (decals) and the receiver mesh

### 10.1 `CPrintDecal` and its three kinds [V]
Base layout (0x8C; vtables with 3 slots Update / Read / Write): +4 mesh built, +5 active, +8 / +0xC centre, +0x10 / +0x14 half
extents, +0x18 growth per tick, +0x1C ttl (short), +0x20 `std::string` texture name, +0x2C texture, +0x30 / +0x34 atlas origin,
+0x38 / +0x3C atlas cell size, +0x40 uv rotation, +0x44 rectangle angle, +0x48 edge fade byte, +0x4C opacity, +0x50 max opacity,
+0x54 colour, +0x58 life0, +0x5C / +0x6C / +0x7C the receiver mesh arrays (xyz SoA blocks, uv, ushort indices; counts +0x60 / +0x70 /
+0x80). The vtables are: **0x73F754 = CBloodPrint**, **0x73F760 = CFootPrint**, **0x73F76C = CFirePrint** (the constructors set them:
0x6CF660 / 0x6CFBA0 / 0x6CFEA0); the Read / Write names in `functions.tsv` were swapped and are corrected in this round.

| | CBloodPrint 0x6CF660 (0x9C) | CFootPrint 0x6CFBA0 (0x94) | CFirePrint 0x6CFEA0 (0x8C) |
|---|---|---|---|
| texture / atlas | `bloodprints`, 4 x 4 cells of 0.25 (cell bits from prints.db) | `footprints` or `footprints%d` (unit type +0x90), 16 x 8 cells (1/16 x 1/8) | `fireprints`, 2 x 2 cells of 0.5 (random cell) |
| uv rotation +0x40 | random k pi/2 | 0 | k pi/2 of the random cell |
| angle +0x44 | random, spins by +0x8C = pi/180 * 30 / ttl per tick | the unit's direction | parameter |
| growth | `(sizeMax - size0) / growTicks`, growTicks +0x90 = min(ttl/2, 45); the mesh is rebuilt every tick while growing (`CBloodPrint::Update` 0x6CFA20) | 0 | 0 |
| colour +0x54 | the light colour of the moment (world +0x6CC) | 0xFFFFFFFF | 0xFFFFFFFF |
| creator | `CWorldClient::AddTerrainPrint` 0x56CE90 (gate `EnableBloodprints` g_0x79BD50), list +0x1B8 | `AddTerrainPrint2` 0x56D150 (`EnableFootprints` g_0x79BD4C), +0x1BC | `AddTerrainPrint3` 0x56D4C0 (`EnableFireprints` g_0x79BD54), +0x1C0 |

`CPrintDecal::Update` 0x6CECD0 (footprints, scorch): `ttl--`, extents += growth, `opacity = (1 - clamp01((life0 - ttl) / life0)) *
maxOpacity`; a ttl <= 0 lets `CWorldClient::Update` delete the print. Every print takes `CTerrain::GetEdgeFadeAlpha` at creation.

### 10.2 `prints.db`: `CPrintsDb` 0x6D0250 (global g_0x7C89C8) [V]
`CPrintsDb::Load(dataDir)` 0x6D0630 (once from `main` 0x45707D): `database.res` -> `prints.db` (`"Can't open terrain prints database"`),
Archive tags 1 / 2 / 3 = the `BloodPrints` (0x28-byte records) / `FootPrints` (0x34) / `FirePrints` (0x28) blocks of
`database-format.md` into the vectors +0 / +0x20 / +0x40 with name -> record hash maps (+0xC / +0x2C / +0x4C). A 0x28 record is
`{int, std::string terrain type name, +0x10 scale, +0x14 / +0x18 ids, +0x1C..+0x24 night variant}` (receivers.md); the AddTerrainPrint
functions look the terrain tile type up by name (`CTerrain::GetMaterialAt`) for the opacity, lifetime and fade of a print.
(The old names `CDatabase` / `CPrintManager::~body` meant this class.)

### 10.3 Drawing a print: `CPrintDecal::Render` 0x6CEDE0 and the shadow machinery [V]
Inactive -> nothing. Mesh not built: `z0 = CBridgeManager::GetHeightAt(world +0x1D4, x, y, terrain height)`, the rectangle's 4 corners
(half extents rotated by +0x44) at z0 -> `CWorldClient::RenderShadowVolume(light (0, 0, -1), pos (x, y, z0 + 1), radius 0, corners,
the three arrays, bridgeCheck = 1, objects = 1)` = `ShadowProjector::Build` 0x4D8000 + `CTerrain::BuildShadowReceiverMesh` 0x6CCF20 +
`ClipBuf::Finish` 0x4D89A0; then every uv is rotated about (0.5, 0.5) by +0x40 and mapped into the atlas cell, every z += 0.01.
Then the colour's alpha byte = `round(opacity * 255)`, `CRenderDevice::SetTextureByDesc(stage 0, +0x20)`, `DrawQuadBatchClippedB(xyz,
uv, blocks, indices, count, colour, edgeFade << 24, 0, zScale 1.0)` with z-write off and the state block g_0x7AEDD8 (texture x diffuse,
alpha texture x diffuse): the prints are drawn after the units and before the shadows and the water. A print centred on a bridge
quad gets no mesh (bridgeCheck).

**ClipBuf / ShadowProjector** (global g_0x7AF488, 0x7C bytes; `receivers.md` section 1-3, re-verified): `Build` makes the prism of the
rectangle along the light (u / v axes +0xC / +0x18 = the rectangle edges with their light component removed, divided by their
squared length; `cap = -((pos - L * 0.33 a) . L)` at +0x68); `BuildShadowReceiverMesh` slides the rectangle up the light ray to the
highest vertex of the covered box (1-cell margin, clamped to the map), `ReserveForTriangles(((nx+1)(ny+1) + 5(nx+ny+1)) * 4)`
0x4D8780, and feeds the two triangles of every 1-unit cell (`v00 v10 v11`, `v00 v11 v01`, `GetVertex` layer 0) through
`ClipTriangleA` 0x4D8490 -> `ClipStage1` 0x4D7BD0 (cap, u >= 0, v >= 0) -> `ClipStage3` 0x4D71D0 / `ClipStage2` 0x4D74B0 (1 - u, 1 - v)
(`Triangle_ClipByPlane5` 0x4D6E90 Sutherland-Hodgman on `x y z u v`, `ClipVertex_Interpolate5` 0x4D7140); `ClipBuf::AddVertex`
0x4D6C50 stores x / y / z in SoA blocks of 4 (`x[4] y[4] z[4] pad[4]`), `(u, v)` in the second array, indices as a running counter
(no sharing); `Finish` trims the counts and detaches the arrays. The same functions build the unit / object shadow receivers
(`CObject3DClientSpecific::Render` 0x51CE60); the object-receiver branch (`ClipTriangleB`, list `CWorldClient +0x1F4`) is dead code.
`CBitmapDib` (0x48, vt 0x744288) is the BMP / DIB helper of the BMP texture path and the screenshot code (`LoadFile` 0x6D4D40 maps a
`.bmp`, `LoadStream` 0x6D5910, `LoadMmpMipLevel` 0x6D5BF0 decodes one DXT1..5 mip of an MMP into a 16-bit 565 DIB, `FromSurface` 0x6D6D40,
`WriteBmp` 0x6D5AC0).

## 11. The two small databases (data, not rendering) [V]

| class | file (in `database.res`) | blocks (Archive tags) | record sizes | readers |
|---|---|---|---|---|
| `CPrintsDb` 0x6D0250 (g_0x7C89C8), `Load` 0x6D0630 | `prints.db` | 1 BloodPrints, 2 FootPrints, 3 FirePrints | 0x28, 0x34, 0x28 | `CWorldClient::AddTerrainPrint` 1 / 2 / 3 |
| `CAcksDb` 0x6D7220, `Load` 0x6D7620 (once from `main` 0x45706C) | `acks.db` | 1 Answers, 2 Cryes, 3 Others | 0x1C4 (`AcksAnswer`, 0x6DC000), 0x5C, 0x38 | `CAcksDb::PlayUnitHitSound` 0x6D8430, `FindUnitRecord` 0x6D8A40 (the unit's prototype name + gender) |

Each block is a `std::vector` of records with a name -> record hash map (+0xC, +0x2C, +0x4C); an `AcksAnswer` additionally maps the
sound-kind ids 0..0x18 to its voice-line lists (+0x20 + k * 0xC, 0x28-byte line records `{wav path, weight, subtitle, text id}` =
`AcksUniqueType` of `database-format.md`; ids 0x16..0x18 have no list). The many `Db*Vector_*` / `StrHashSet_*` functions around
0x6D1000-0x6E0000 are the STLport instantiations of these containers.

## 12. What is not known

* The MMP extra block (`hdr[0x48]` bytes after the header, copied to `CTexture +0x2C`): no file seen has one; what the game would
  put there is unknown.
* The sun object's `+0x24` in the under-water sun attenuation (4.2): read it on the rig (expected 0).
* `TraceSunRay`'s double slope on diagonal suns is "as decompiled"; a rig comparison of the shadow length would settle it.
* Which of the two periodic water changes (sine band, tile cycle) the user's 0.8 s stripes are (5.4): test with `EnableWaterWaves = 0`.
* Whether the water pass's state block lets the material alpha through (5.5) and the exact blend of the water pass.
* The sky dome's figure file and the meaning of the second colour array (+0x148, "specular") beyond the per-hour colours.
* `CTextureManager::LoadTextureFromRegistry` 0x691BF0 (searches the list by kind and name, then creates): its name is kept, its
  second half was not read.
* The `TerrainSector +0x390` field (reset to 0, never read in this module) and `CTerrain +0x218`'s initial value 1.
* `CTerrain::SetVisibleQuad`'s exact inclusion rule for sectors on the edge (centre test only: thin visible slivers of a sector
  whose centre is outside are not drawn?).

## 13. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| `functions.tsv` (TerrainSector_ctor / dtor / FreeData / BuildMesh / RenderSectors) | "4 LOD strip groups" at +0x2CC.. | 4 **quarter** groups: `CArray<CByteArray>` at +0x2C4 + q*0x14 (edge flags per pass) and `CArray<Vector10>` at +0x314 + q*0x14 (9 triplets per tile per pass); the LOD is chosen at draw time |
| `functions.tsv` TerrainSector_BuildMesh / SetTile / Pack / GetMaterialAt | "+0x1A4 texture index" | +0x1A4 holds the 14-bit **tile id**; the texture index is derived (`id / tilesPerTexture`, pages beyond the count) |
| `functions.tsv` TerrainSector_BuildVertexTileLinks | a sector function | `CTerrain::BuildVertexTileLinks`: `this` is the terrain (called from `CTerrain::Init`), the links live at CTerrain +0x1E8 |
| `functions.tsv` CFootPrint::Write / Read 0x6CFAC0 / 0x6CFB40, CBloodPrint::Write / Read 0x6CFE10 / 0x6CFE60, CPrintDecal2::Update 0x6CFA20 | vtable 0x73F754 = footprint | 0x73F754 is **CBloodPrint** (ctor 0x6CF660), 0x73F760 CFootPrint, 0x73F76C CFirePrint: the four Read / Write names swap, 0x6CFA20 is `CBloodPrint::Update` (receivers.md had this right) |
| `functions.tsv` + `findings.md` CTextureManager::TrimCache | "+0xDC size, +0xD8 budget" | +0xD8 = bytes in use, +0xDC = budget (free video memory / 10) |
| `functions.tsv` CTexture::Load / FillSurface | "picks the .mmp variant" / "loads the .mmp image" | the **BMP** path: `LoadBmpPair` / `LoadFromBmp` read a 24-bit `.bmp` plus an 8-bit alpha `.bmp`; the mmp path is `CTextureManager::LoadTexture` -> `LoadMipChain` |
| `functions.tsv` CTexture::GetMipOffset | a CTexture method, "level 0 = +0x48" | `MmpHeader::GetMipOffset`: `this` is the MMP header, level 0 starts `hdr[0x48]` (the extra block size) bytes after the 0x4C header |
| `functions.tsv` CDatabase / CDatabase2 / CDatabaseTables / CPrintManager | generic database classes | `CPrintsDb` (prints.db), `CAcksDb` (acks.db), `AcksAnswer` (the 0x1C4 record) |
| `functions.tsv` CBitmapDib::LoadMipLevel | "mip level data of a BMP stream" | an **MMP** stream: DXT1..5 decoded into a 16-bit 565 DIB |
| `functions.tsv` TerrainLayer_SetHeight 0x6CC7F0 | "layer height field" | `CTerrain::SetMaterialHeight`: the water level of one material through the per-sector delta table; called only by `WaterLevels::Read` |
| `functions.tsv` CSkyObject::UpdateTransform / SkyBlend_ctor / SkyHolder_Rebuild | methods of the dome | `CSky::Render` / `CSky::CSky` / `CSky::Recolour`: `this` is the 0x10-byte holder at CWorldClient +0x1AC |
| `findings.md` (CTerrain layout) | "+0x1fc sector CList (LRU, 0xf0 frames)" | correct, but the stamp counts **ticks** (`CTerrain::Tick`), not rendered frames |
| `docs/water.md` | "the specular colour from a 512-entry sine table driven by the wind and the phase table" | right; add: the band travels with +0x288 (+0x28C = -0.15 x wind strength per tick) and is drawn only on the positive half of the sine, the x / y drift uses a separate `sin(col pi/4 + t/20)` term, flat (type 4) water never drifts |
| `_cpr/shadows/terrain-light.md` 1.2 | "+0x2B4 water tile map; +0x2CC.. 4 LOD strip groups" | +0x2B4 = water **material** per tile (int, -1 none); quarters as above |
| `_cpr/shadows/terrain-light.md` 1.1 | "+0x15C resource reader; +0x1C8 [4] / +0x1D8 [4] ... (0x48 bytes per tile = 9 u,v pairs)" | the 18 floats are `u[9]` then `v[9]`, not interleaved pairs |

## 14. Function index

| addr | name | one line |
|---|---|---|
| 0x6BE3A0 | Terrain_PlaneZAt | z of the plane through three vertices (GetHeightAt) |
| 0x6BF4A0 | Terrain_RectToSectorRect | vertex rect -> sector rect + local ranges |
| 0x6BF5F0 / 0x6BF8A0 | TerrainSector_ctor / dtor | the 0x394-byte sector |
| 0x6BFB20 | TerrainSector_CullFrustum | sector and quarter boxes vs the view frustum |
| 0x6BFB80 | TerrainSector_UpdateLodSeams | seam flags against coarser neighbours |
| 0x6BFC80 | TerrainSector_TransformVertices | 17 x 17 per quarter through the 3DfpFPU projection |
| 0x6BFF40 | TerrainSector_Draw | FVF 0x1C4 vertices + LOD / seam index patterns, DrawIndexedTriangles |
| 0x6C1390 | TerrainSector_SetLod | +0x170 |
| 0x6C13A0 | TerrainSector_Load | the `.sec` file |
| 0x6C1C00 | TerrainSector_BuildMesh | records, tiles, quarters, strips, bounds |
| 0x6C30E0 | TerrainSector_ApplyMaterialHeightDeltas | water level deltas |
| 0x6C31C0 | TerrainSector_LightVertexDynamic | point lights -> DIFFUSE / SPECULAR |
| 0x6C36B0 | TerrainSector_LightVertexSun | ambient / sun / shadow / water -> sun colour |
| 0x6C3A00 | TerrainSector_Relight | both layers, a vertex range |
| 0x6C3DD0 | TerrainSector_EnsureState | build / relight on demand |
| 0x6C3E30 / 0x6C3E50 | TerrainSector_ScheduleRelight / TickRelight | staggered relights |
| 0x6C3E80 | TerrainSector_Animate | tile cycling, water waves |
| 0x6C42A0 / 0x6C4310 | TerrainSector_Pack / PackVertex | `.sec` writer (editor leftover) |
| 0x6C4620 / 0x6C4660 | TerrainSector_SetTile / GetTile | tile id + rotation |
| 0x6C5150 / 0x6C5230 | TerrainVertex_FreeLightLinks / LightLinkNode_Free | the 16-byte light-link pool |
| 0x6C53C0 | CTerrain::GetTileUVs | the rotated UV table of a tile |
| 0x6C5430 / 0x6C54D0 / 0x6C5620 | CTerrain::Init / Done / InitFields | |
| 0x6C57E0 | CTerrain::BuildWaveTable | 33 x 33 phase bumps |
| 0x6C5AA0 / 0x6C5EA0 | CTerrain::LoadTextures / ReleaseTextures | textures + materials per pass |
| 0x6C6230 / 0x6C6330 / 0x6C6460 | CTerrain::RenderOpaque / RenderWater / RenderSectors | the passes |
| 0x6C66E0 | CTerrain::SelectSectorLod | LOD1 / LOD2 distances |
| 0x6C67E0 | CTerrain::SaveModifiedSectors | editor leftover |
| 0x6C68B0 / 0x6C7750 | CTerrain::Load / Unload | the `.mp` header |
| 0x6C7940 | Terrain_BuildTileUVs | u[9], v[9] of a tile |
| 0x6C7A80 | CTerrain::BuildVertexTileLinks | vertex -> tiles |
| 0x6C7D70 / 0x6C7EB0 | CTerrain::SetWindStrength / SetWind | wave parameters |
| 0x6C7FF0 / 0x6C80C0 | CTerrain::AllocSectorGrid / FreeSectorGrid | |
| 0x6C8140 / 0x6C84E0 | CTerrain::GetSector / UnloadAllSectors | the MRU cache |
| 0x6C85A0 / 0x6C8680 / 0x6C8930 | CTerrain::GetVertex / GetHeightAt / GetGroundAndWaterDepth | height queries |
| 0x6C8990 | CTerrain::SnapQuadsToGround | editor / object placement helper |
| 0x6C8D40 | CTerrain::GetMaterialAt | tile type under a point |
| 0x6C8ED0 / 0x6C9290 / 0x6C92C0 | CTerrain::SetSunLight / SetRelightBudget / TraceSunRay | the sun |
| 0x6C9560 | CTerrain::TraceRayToGround | ray vs the height field (picking) |
| 0x6C98F0 / 0x6CA260 | CTerrain::SetVisibleQuad / SetVisibleArea | visible sector ranges |
| 0x6CA5D0 / 0x6CA720 / 0x6CA830 / 0x6CA9C0 / 0x6CAA70 / 0x6CAB50 / 0x6CAC10 | AddLight / RemoveLight / MoveLight / RelinkLightsInSector / PrepareLightRect / RelightRect / TerrainSector_LinkLight | point lights on vertices |
| 0x6CB020 | CTerrain::GetEdgeFadeAlpha | map border fade |
| 0x6CB1E0 | CTerrain::Tick | per tick |
| 0x6CB390 / 0x6CBC00 / 0x6CBCD0 | CTerrain::UpdateAmbientSounds / Terrain_DayTimeName / StopAmbientSounds | ambient loops |
| 0x6CBD10 / 0x6CBE60 / 0x6CBF80 / 0x6CC2A0 / 0x6CC3E0 / 0x6CC530 | SetVertexHeight / SetSharedVertexPos / CollectSharedVertexSlots / Collect*EdgeVertices / ComputeVertexNormal | editor leftovers |
| 0x6CC7F0 | CTerrain::SetMaterialHeight | water levels |
| 0x6CC870 / 0x6CC8D0 | CTerrain::SetTileAtWorld / SetTile | editor leftovers |
| 0x6CCF20 | CTerrain::BuildShadowReceiverMesh | terrain triangles inside the shadow prism |
| 0x6CECD0 / 0x6CEDE0 / 0x6CF370 / 0x6CF4F0 / 0x6CF960 | CPrintDecal::Update / Render / Write / Read / FreeBuffers | |
| 0x6CF660 / 0x6CFA20 / 0x6CFAC0 / 0x6CFB40 | CBloodPrint::ctor / Update / Write / Read | |
| 0x6CFBA0 / 0x6CFE10 / 0x6CFE60 | CFootPrint::ctor / Write / Read | |
| 0x6CFEA0 | CFirePrint::ctor | |
| 0x6D0250 / 0x6D0630 / 0x6D00E0 | CPrintsDb::ctor / Load / dtor | prints.db |
| 0x6D4410 / 0x6D44E0 / 0x6D45D0 | WaterLevels::SetTarget / Tick / Read | script water levels |
| 0x6D4AA0 / 0x6D4D40 / 0x6D5910 / 0x6D5BF0 / 0x6D6D40 | CBitmapDib ctor / LoadFile / LoadStream / LoadMmpMipLevel / FromSurface | |
| 0x6D7220 / 0x6D7620 / 0x6D8430 / 0x6D8A40 | CAcksDb::ctor / Load / PlayUnitHitSound / FindUnitRecord | acks.db |
| 0x68CED0 / 0x68D010 / 0x68D0A0 / 0x68D280 / 0x68D310 / 0x68D5A0 / 0x68D730 | CLight ctor / InitPositional / InitDirectional / PrecomputeMaterialTerms / ApplyDirectional / ApplyPointWrap / LuminanceAt | |
| 0x68DA40 | CLightManager::CreateByKind | kinds 0x2E / 0x31 |
| 0x68DBC0 / 0x68E640 / 0x68E970 | CMaterial::ctor / CMaterialManager::Init / CreateAndRegister | materials.res templates |
| 0x68FA10 / 0x68FBF0 / 0x690260 / 0x690430 / 0x690970 / 0x690B30 / 0x690D30 | CTexture::UploadMip / LoadFromBmp / LoadBmpPair / LoadMipChain / CopyToSurface / CopyToSurface32 / UnpackToSurface | |
| 0x6908E0 | MmpHeader::GetMipOffset | |
| 0x691140 / 0x6913A0 / 0x691500 / 0x691810 / 0x691AA0 | CTextureManager ctor / SetDevice / Init / CreateByKind / RegisterInstant | |
| 0x6922A0 / 0x6926F0 / 0x6928C0 | CTextureManager::LoadTexture / LoadTextureFromDir / LoadMmpExtraBlock | |
| 0x692D30 / 0x693C60 / 0x691D00 / 0x692E20 / 0x694020 | FindOrLoad / FindByName / FindOrCreateNamed / PurgeOlderThan / TrimCache | |
| 0x693010 / 0x693180 | ComposeLayers / ComposeAndSaveLayers | unit skins |
| 0x693850 / 0x6938B0 / 0x693910 | PixelFormat_ARGB1555 / 4444 / 8888 | |
| 0x6945A0 / 0x6946D0 | CFileRegistry::Open / FindData | .reg files |
| 0x4D6C50 / 0x4D6CF0 / 0x4D6E90 / 0x4D7140 | ClipBuf::AddVertex / AddTriangle / Triangle_ClipByPlane5 / ClipVertex_Interpolate5 | |
| 0x4D71D0 / 0x4D74B0 / 0x4D7BD0 / 0x4D7F60 / 0x4D8000 / 0x4D8490 / 0x4D85F0 / 0x4D8780 / 0x4D89A0 | ShadowProjector::ClipStage3 / 2 / 1 / SphereVsPrism / Build / ClipTriangleA / B / ClipBuf::ReserveForTriangles / Finish | |
| 0x4D9110 / 0x4D9360 / 0x4D92B0 | CSkyObject::ctor / Recolour / FreeArrays | |
| 0x4D9680 / 0x4D96E0 / 0x4D9DE0 | CSky::CSky / Render / Recolour | |
| 0x4D9880 / 0x4D9AA0 / 0x4D9B80 | Matrix4_Mul / Quat_ToMatrix4 / MatrixStack_Push | |
| 0x4E1770 / 0x4E1AB0 / 0x4E1C60 / 0x4E1EB0 / 0x4E23D0 | CVisibilityFog::ComputeBounds / Tick / RebuildCells / Render / Init | |
| 0x4E2290 / 0x4E2700 / 0x4E2760 / 0x4E28D0 | CVisFogManager::CreateByKind / RenderAll / Tick / CreateAndRegister | |
| 0x4660B0 / 0x466190 / 0x466820 / 0x466CE0 / 0x4662C0 | LightningNoise_BuildTable / LightningSeed_Init / LightningSegment_Init / CLightning::UpdatePoints / EvalNoise | |
| 0x4680D0 / 0x4672E0 / 0x467AD0 / 0x467590 / 0x467E10 / 0x467150 / 0x468A00 | CLightning::Init / Update / Render / RenderGlow / InitRandom / ComputeBounds / Lightning_LoadNoiseTables | |
