# The renderer core of game.exe: the D3D7 device, the figure pipeline and the animation system

Written 2026-10-28 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x4E3000-0x501000 device /
render manager, 0x501000-0x50E000 I3DFigureImpl + animation, 0x50E000-0x520000 CMapObject / CObject3D and their specifics, 0x59F000-0x5AD000
CFigure / CFigureManager, 0x453000 CFigure ctors, 0x5E3000 CI3DFigure). The frame order and `CWorldClient::Render / RenderObjects` are in
`world-client-players.md` section 8; the vanilla shadow is `docs/shadows.md`; the file formats are `docs/file-formats/figure-format.md` and
`adb-format.md`; terrain, textures, materials, lights, sky and fog are another page (their entry points are only named here).
The function batch is `_cpr/claude-re/full/batch-renderer.tsv`, the field batch `batch-renderer-structs.tsv`.

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs in game.exe; offsets are bytes; "stack n" = matrix stack n of the render context (section 2.3).

## How to re-implement it (short version)

1. **Everything is CPU-transformed.** The game loads `3DfpFPU.dll` (`CD3DLibrary::Load` 0x4F0B90, export `GetTLPipeline`) and a plain
   `IID_IDirect3DHALDevice` (the TnL HAL is skipped, `CRenderDevice::CRenderDevice` 0x4E5E50). Every triangle reaches Direct3D as a
   pre-transformed `VertexTL` (FVF 0x1C4: x, y, z, rhw, diffuse, specular, u, v; 32 bytes) through `IDirect3DDevice7::DrawIndexedPrimitive`
   with `LIGHTING 0`, `CLIPPING 0` (InitRenderer 0x500B70). The vertex-buffer path exists but is dead: `ReserveBatchVertices_Stub` 0x4E7820
   returns 0 and `CreateVertexBuffer` 0x4E72A0 has no callers, so `FlushVertexBuffer` 0x4E7840 is a no-op.
2. **Matrices are column-vector, row-major floats** (`p' = M p`, translation in `m[3], m[7], m[11]`; `Mat4_Viewport` 0x4F1370,
   `CullBox` 0x4F16A0). Sixteen stacks live in the render context `g_0x7AF068` (CD3DLibrary +0x14 + 8n); a push multiplies the new matrix on
   the right (`MatrixStack_Push` 0x4D9B80: `top * M`), so the matrix pushed last is applied first. Stack 1 = viewport * world * projection
   (screen space), stack 2 = world * projection (clip space, used by the culler and the clipper).
3. **A model is a `CRenderScene` (0xB0) per (model name, complection)**: `CRenderManager::FindOrCreateScene` 0x4F7150 reuses the scene whose
   three morph coefficients are within 1e-5, else `CreateScene` 0x4F7B10 blends every part's 8 `.fig` corners with `lerp(lerp(lerp(c0,c1,s),
   lerp(c2,c3,s), d), ..., h)` (`BuildFigureMesh` 0x4F7F00) into shared SoA arrays (blocks of 4 vertices: 4x, 4y, 4z, 4w). A buff that
   "enlarges" a unit only changes the coefficients (`ApplyBuffKinds` 0x50F2D0 adds 0.4): nothing is ever scaled by a matrix.
4. **A part is a `CFigure` (0x220) drawn by a `CFigureClientSpecific`** (`RenderLit` 0x5A7A70): cull box in clip space, lights transformed
   into figure space, the pipeline's light kernel (`vt[40]`) writes one colour per *normal* into `+0x138`, the `.anm` per-vertex deltas
   are added, `DrawMeshBatch` 0x4E9010 expands the `{normalIdx, vertexIdx, uvIdx}` components into `VertexTL`s and draws. Lighting is cached
   per object (flag 0x400) until a light moves. The texture comes from the `.fig` header's `textureNumber` (texture set), the material from
   the header word 17..25 (`AttachPart` 0x51B050).
5. **Animation = one key per world tick.** `CUnitClientSpecific::SetAnimation` 0x5352B0 hands the `.adb` entry of the replicated clip to
   `CAnimationSystem2::SetAnimation` 0x50CA50 and `SetTime(worldTime + 1 - startTick)` 0x50D600; `CAnimationTrack::Evaluate` 0x50ACD0 lerps /
   slerps the keys at `floor(time)` with the fraction. `CAnimationNode::UpdateTransform` 0x50C230: a child's position is the parent's position
   plus the parent rotation applied to the child's `.bon` rest offset (blended at the complection); the track's positions are used only by the
   root, scaled by the `.adb` header floats (`GetModelLerp` 0x518D30). The clip ends when `time > lastFrame (+0x20)`: a looping entry
   (flags +0x14 & 0xC0000000 == 0x80000000) wraps, else the pose holds and the queued clip starts.
6. **The vanilla shadow** is a 64x64 silhouette bitmap draped on the terrain (`RenderShadow` 0x51CE60, `docs/shadows.md`); the wounds are a
   second alpha-blended pass of the same mesh (`DrawIndexedTwoPassBlend` 0x4EB370).

## 1. Conventions

| Thing | Value |
|---|---|
| vertex format | `VertexTL` FVF 0x1C4 (0x20 B): float x, y, z (0..1), rhw, u32 diffuse 0xAARRGGBB, u32 specular (alpha << 24), float u, v [V] 0x4E9010 |
| line format | FVF 0xC4 (XYZRHW + DIFFUSE, 0x14 B) for `DrawPolyline2D` 0x5045D0 [V] |
| primitive | `D3DPT_TRIANGLELIST` (4) everywhere except `DrawSprite` (TRIANGLESTRIP 5) and `DrawPolyline2D` (LINESTRIP 3) [V] |
| mesh storage | SoA blocks of 4 vertices: `VertexSoABlock` {4 x, 4 y, 4 z, 4 w = 1} = 0x40 B; vertex i is block i >> 2, lane i & 3 [V] 0x4F7F00, 0x4F3C60 |
| matrix | 16 floats row-major, used as column-vector `p' = M p`, translation in [3], [7], [11]; product `Matrix4_Mul(out, a, b) = a * b` 0x4D9880; a stack push gives `top * M` [V] |
| projection | `Mat4_Perspective` 0x4F0E80: [0] = cos(fov/2), [5] = cos(fov/2) * aspect, [10] = Q = sin(fov/2) * far / (far - near), [11] = -Q * near, [14] = sin(fov/2) (so w' = sin(fov/2) * z); aspect multiplies y: with 4:3 the x axis is 0.75 of y on screen [V] |
| viewport | `Mat4_Viewport` 0x4F1370: x' = hw * x + hw, y' = -hh * y + hh, z' = z (hw, hh = half size of the device rect +0x24) [V] |
| culling | `CullBox` 0x4F16A0 on stack 2 with 6-bit outcodes (x + w >= 0, x <= w, y likewise, 0 <= z <= w): 0 inside, 1 partly, 2 outside [V] |
| handedness | the camera's view matrix (`CCamera::BuildViewMatrix` 0x4F0EF0) and `CULLMODE = CW` (2) [V]; z is up in the world (`+0x24` of objects) |
| colours | D3DCOLOR 0xAARRGGBB; the light kernel writes one colour per normal; the specular dword carries only alpha (edge fade, ambient alpha) [V] |
| time | animation time = world ticks since the clip's start tick (float; 1 frame per tick, 55 ms at normal speed) [V] 0x5352B0, 0x50D600 |
| complection | three floats (s = strength, d = dexterity, h = height) at `CObject3D +0xC4`; corner order c0 (s0,d0) c1 (s1,d0) c2 (s0,d1) c3 (s1,d1) at h = 0, c4..c7 at h = 1 [V] |
| globals | `g_0x7AF060` I3DFigureImpl (the renderer), `+8 = g_0x7AF068` CD3DLibrary (the context and the stacks), `+0x1C = g_0x7AF07C` the sun CLight, `+0x20 = g_0x7AF080` CRenderDevice; `g_0x7AEE78` the DirectDraw critical section; `g_0x7AF744` texture manager, `g_0x7AF748` light manager |

## 2. The device

### 2.1 Creation (`I3DFigureImpl::InitRenderer` 0x500B70, called once from WinMain 0x456D55) [V]

1. `LoadLibraries` 0x500540: `new CD3DLibrary` (0x94) loads `<Work path>3DfpFPU.dll` and resolves `GetTLPipeline` -> `+0xC` (the ITLPipeline);
   the cpuid MMX bit is required ("MMX required in this version"); `2DintMMX.dll` / `GetGraphicsProcessor` -> `+0x10` (the 2D blitter used by the
   shadow rasteriser and the software cursor). Failures: `FatalError` + `_exit(-1)`.
2. `CDXDirectDraw::FindHw3dDriver` 0x4EE5D0 (or the `DrawingTarget` index into the driver list `g_0x7AEE64`): the first DirectDraw driver
   whose caps have `DDCAPS_3D`; `FatalError("Can't find any driver with HW 3D.")` + `_exit(-1)` when none.
3. `new CRenderDevice(hwnd, driver, FullScreen, w, h, bpp, 0, 1, zDepth, 0)` 0x4E5E50. The base `CDXDirectDraw` ctor 0x4EDD70: `DirectDrawCreateEx`
   for the driver, `SetCooperativeLevel`, `SetDisplayMode(w, h, bpp, refresh)` when fullscreen, `CreateSurfaces(1 back buffer, with3D)` 0x4EEE30
   (fullscreen: a flipping complex primary 0x2218 with `GetAttachedSurface(BACKBUFFER)`; windowed: the primary plus an OFFSCREENPLAIN |
   VIDEOMEMORY | 3DDEVICE surface of the window rect, "Back buffer is not in video memory" otherwise), then the YUV surface probe for the Bink
   video (first of YUY2 / UYVY / UYNV / YUNV / YV12 / YVU9 the driver can Blt -> `+0x3C`, overlay -> `+0x40`).
   The derived part: `QueryInterface(IID_IDirect3D7)` -> `+0x50`; `EnumDevices` (callback `D3D_EnumDevicesCallback_Add` 0x4ED820, node 0x10C
   with the D3DDEVICEDESC7 at +0x20) and the choice of **the node whose deviceGUID is `IID_IDirect3DHALDevice`** (0x745A50); a TnL HAL
   (0x745A10) is skipped. The device must render the mode's bit depth (`dwDeviceRenderBitDepth` & 0x400 / 0x200 / 0x100 for 16 / 24 / 32)
   or "Can't create HW IDirect3DDevice". `CreateDevice(GUID, back surface)` -> `+0x54`. `+0x60 = 1` (a W-buffer choice from `dwRasterCaps &
   WBUFFER` is computed and immediately overwritten). `EnumTextureFormats` 0x4E6EE0 (section 2.5), `EnumZBufferFormats`, `CreateZBuffer`
   0x4E6C30 (a DDPF_ZBUFFER format with the back buffer's bit depth, else the requested zDepth, DDSCAPS_ZBUFFER | VIDEOMEMORY, attached to
   the render target), `SetRenderTarget`, the viewport = the device rect with z 0..1, and four global texture-stage state blocks (2.4).
   Every failure throws a `GameException` that `InitRenderer` catches, `FatalError`s and swallows (`notes-renderer-init.md`): the device
   pointer `g_0x7AF080` stays null and the game crashes at 0x456D8D when `addon.dll` silences `FatalError`.
4. The stacks of `g_0x7AF068` are filled (2.3), the pipeline gets `SetProjection` (vt[4]), `SetWindowSize(w, h)` (vt[1]) and
   `SetFog(FogDensity 0x79BED4, FarClipDistance 0x79BE54)` (vt[5]); the render states of 2.4 are set; the frame stats are reset; the default
   sun (`CLight` kind 0x2E, direction (5, 5, -8), white, range 1e6) is registered (`SetSunLight` 0x500800); `SetFogColor(0xFF323232)`;
   `new CStateBlockSet` (0x118) 0x4FCA60 compiles the 9 default effects (2.4).

`SetupCamera` 0x5015A0 (slot 25, from `CWorldClient::Render` and the UI 3D widgets) repeats the stack set-up for a rect and fov:
P = perspective(fov of `g_0x79B2A8` or the given one, rect aspect, near, far), V = viewport(rect), identity W.

### 2.2 The pipeline interface (3DfpFPU.dll `GetTLPipeline()`, CD3DLibrary +0xC) [V] (slot numbers = vtable index)

| slot | called from | what it does |
|---|---|---|
| 1 | InitRenderer, SetupCamera | `SetWindowSize(w, h)` |
| 2 | SetSunLight 0x500800 | `SetDirectionalLight(CLight*)` (0 to clear) |
| 3 | SetAmbientColor 0x500740 | `SetAmbient(r, g, b)` floats |
| 4 | InitRenderer, SetupCamera | `SetProjection(matrix)` (the projection with row 0 columns 1..3 from the viewport; the 2D library gets the transpose) |
| 5 | InitRenderer | `SetFog(density, farClip)` [I] |
| 7 | BuildFigureMesh, LoadFig8 | `ComputeBounds(BoundsRecord*, VertexSoABlock*, blocks)`: sphere + box of a blended mesh |
| 9 | DrawMeshBatch (flag 2), TransformForDraw | `TransformClipCodes(stack-2 top, SoA, outXYZW (g_0x7AEE10), outCodes (g_0x7AEE30), blocks)` -> 0 / 1 / 2 |
| 14 | DrawMeshBatch, TransformForDraw | `Transform(stack-1 top, SoA, outXYZW, blocks)` -> 0 all inside / 1 / 2 all outside |
| 29 | MorphAndProjectVertices 0x5A0CD0 | project to the light plane (shadow caster) |
| 30 | DrawProjected64 0x5A9310 | transform by a given matrix into 2D |
| 34 | DrawMeshBatch (clipped) | finish the clipped vertices (`g_0x7AEE00`, count) |
| 35 | DrawMeshBatch, DrawIndexedTriangles when device +0x158 | a post-process over the VertexTL batch [?] |
| 37 | DrawMeshBatch (clipped) | `ClipTriangles(vertices, codes, indices, outIndices, &vertCount, &indexCount)` |
| 40 | RenderLit 0x5A7A70 | `LightVertices(SoA vertices, blocks, outColours, +0xBC, +0xC0, CLight** lights, count, ambient rgb)`: the light kernel `min(1, emissive + max(ambient, N.(-D) sun, point lights))` (um-engine `fp_light.cpp` replaces it) |

### 2.3 The matrix stacks (CD3DLibrary +0x14 + 8n: {buffer of 16 x 0x40, depth}) [V]

| n | offset | after InitRenderer / SetupCamera | pushed by |
|---|---|---|---|
| 0 | +0x14 | V * W * P | (copy of 1; the guard-band code of um-engine rewrites 0 / 1) |
| 1 | +0x1C | V * W * P (screen space) | `CObject3DClientSpecific::Draw` 0x51B7E0: the object matrix, then the part matrix; `CI3DFigure::Draw`; unclipped `DrawMeshBatch` reads its top |
| 2 | +0x24 | W * P (clip space) | the same model matrices; `CullBox`, clipped transforms, `DrawTrianglesTransformed` read it |
| 3 | +0x2C | empty | the object matrix when the lighting is recomputed; `RenderLit` inverts its top to move the lights into figure space |
| 5 | +0x3C | W * P | (unused by this module) |
| 6 | +0x44 | P | |
| 7 | +0x4C | W = identity | |
| 8 | +0x54 | V (viewport) | |
| 9 | +0x5C | empty | the camera view (`CWorldClient::Render` 0x569E40) |
| 10 | +0x64 | empty | the shadow light projection (`RenderShadow`) |

`MatrixStack_Push(ctx, M, n)` 0x4D9B80 stores `top * M` (depth 0: `M`), `MatrixStack_Pop` 0x51C110 decrements the depth. With column vectors
the top of stack 1 for a part is `V * W * P * Mobject * Mpart`: the part matrix is applied first.

### 2.4 Render states and effects [V]

Set once by `InitRenderer` (never changed by the module except where noted):

| state | value | note |
|---|---|---|
| ZENABLE 7 / ZWRITEENABLE 14 | TRUE / TRUE | z-write off only in the shadow / decal / HUD passes (callers) |
| FILLMODE 8 / SHADEMODE 9 | SOLID / GOURAUD | `ToggleWireframe` 0x569970 flips it |
| ALPHABLENDENABLE 27 | TRUE | SRCBLEND 19 = ONE, DESTBLEND 20 = ZERO (opaque by default); `SetAlphaBlendModes` 0x501C60 and the materials change them |
| ALPHATESTENABLE 15 / ALPHAFUNC 25 / ALPHAREF 24 | TRUE / GREATEREQUAL / 200 | the leaf / grass cut-out: texels with alpha < 200 are discarded everywhere |
| CULLMODE 22 | CW (2) | |
| SPECULARENABLE 29 / DITHERENABLE 26 / TEXTUREPERSPECTIVE 4 / ANTIALIAS 2 | AppSettings `Specular` +0x64 / `Dithering` +0x60 / `PerspectiveCorrection` +0x78 / `Antialiasing` +0x68 ? 2 : 0 | |
| FOGENABLE 28 / FOGVERTEXMODE 140 / FOGCOLOR 34 | TRUE / LINEAR / 0 | the fog factor is in the vertex (specular alpha / rhw from the pipeline); `SetFogColor` 0x501AB0 |
| EXTENTS 138, CLIPPING 136, LIGHTING 137, AMBIENT 139 | 0 | the fixed pipeline neither lights nor clips: the CPU does |
| per stage MAGFILTER 16 / MINFILTER 17 | 1 point or 2 linear by `Filtering` +0x74 (0 point, 1 / 2 linear) | MIPFILTER 18 = LINEAR when `Mipmapping` +0x70 (cleared when the caps lack the mip bit) |

Four global texture-stage state blocks are recorded by the ctor (handles `g_0x7AEDDC`, `D8`, `D4`, `D0`): COLOROP MODULATE with
COLORARG1 TEXTURE and COLORARG2 CURRENT / DIFFUSE / DIFFUSE / DIFFUSE, ALPHAOP SELECTARG1 (texture alpha) / MODULATE / SELECTARG1 /
SELECTARG1 of DIFFUSE. [V] 0x4E5E50

`CStateBlockSet` (0x118) builds the **9 default effects** (`BuildDefaultEffect` 0x4FCE90 -> `RegisterEffect` 0x4FD2A0) as D3D state blocks
from `EffectPassRecord`s (0x28: stage op / args, blend enable, src / dest): 0 and 8 plain; 1 = second pass MODULATE texture x current with
blend DESTCOLOR / ZERO (a multiply pass); 2 = MODULATE with arg 0x22 (texture alpha replicate), ZERO / SRCALPHA; 3 = ADD, ONE / ONE (additive);
4 = BLENDTEXTUREALPHA, SRCALPHA / INVSRCALPHA; 5 = three passes (ONE / ZERO, ADD ONE / ONE, MODULATE2X DESTCOLOR / SRCCOLOR); 6 / 7 =
BUMPENVMAP 0x16 / BUMPENVMAPLUMINANCE 0x17 then ADD. A material's effect record `{handles per pass, stages per pass}` is what
`DrawIndexedWithMaterial` 0x4EB550 applies: per pass `ApplyStateBlock`, the stage records (`TextureStageRecord` 0x14: TEXCOORDINDEX, texture
name, CTexture) bound, one `DrawIndexedPrimitive`, then ALPHABLENDENABLE / SRCBLEND / DESTBLEND restored. The only material this module sets
is the object's `textureBlend` (+0xDC, the wounds overlay); the materials proper (`CMaterial`, kinds 17..25) belong to the materials page.

### 2.5 Surfaces and formats [V]

`EnumTextureFormats` 0x4E6EE0 lists the device's texture formats and keys them in the hash `+0xBC`: `0x5650` R5G6B5, `0x5550` X1R5G5B5,
`0x5551` A1R5G5B5, `0x4444` A4R4G4B4, `0x8888` A8R8G8B8 and the DXTn fourccs; `+0xD0` = the best id. `FindTextureFormat` 0x4E7C40 serves the
texture manager. The z-buffer format comes from the enumerated list (`FindZBufferFormat` 0x4E6BC0). `CDXDirectDrawSurface` (0x20: +0x10
IDirectDrawSurface7, +0x14.. pixel format) wraps every created surface; `CDDSurface::LoadPicture` 0x4F0060 converts an 8888 picture into any
of them (`PixelFormat_PackRGBA` 0x4EDC00). `SetGamma` 0x504A60 drives `IDirectDrawGammaControl` for the Brightness setting; `RestoreSurfaces`
0x504780 handles the lost device.

### 2.6 The frame [V]

`BeginFrame` 0x503F40 (timing into `+0x34..+0x50`, `Clear` 0x501A40 = TARGET | ZBUFFER (| STENCIL) with `+0x14`, `BeginScene`, the stats
`device +0x16C..+0x178` reset) ... `CWorldClient::Render` ... `EndFrame` 0x5040A0 (`EndScene`, the debug "num verts / num trias" text). The
`Flip` / `Blt` to the primary is `App_RenderFrame` 0x458010, not this module. Everything passes through one of:

| draw entry | callers | path |
|---|---|---|
| `CRenderDevice::DrawMeshBatch` 0x4E9010 | `CFigureClientSpecific::Render / RenderLit`, `CRenderScene::DrawMesh / DrawMesh2` | section 3.5 |
| `DrawMeshBatchBlended` 0x4E9A50 | `RenderLit` with blend textures, `DrawMeshBlended / 2` | DrawMeshBatch + `DrawIndexedTwoPassBlend` 0x4EB370 (pass 2 = texture B with SRCALPHA / INVSRCALPHA) |
| `DrawTrianglesTransformed` 0x4EA4A0 | `RenderShadow` (receiver mesh), `CPrintDecal::Render`, `DrawArrow3D` | plain xyz + uv + indices, constant colours, z * zScale, `CD3DLibrary::TransformForDraw` 0x4F2650 (cull + clip) |
| `I3DFigureImpl::DrawIndexedTriangles` 0x501BB0 (slot 12) | terrain, water, particles (`DrawParticleQuads` 0x502CA0), polylines (`DrawTexturedPolyline` 0x501D90), UI quads (`DrawRect2D` 0x5041F0, `DrawSprite` 0x504860) | the caller built the `VertexTL`s itself |

All of them end in `IDirect3DDevice7::DrawIndexedPrimitive(TRIANGLELIST, 0x1C4, ...)` under the `g_0x7AEE78` lock, counting
`+0x16C` vertices, `+0x170` triangles, `+0x174` calls, `+0x178` texture changes. Textures are bound per stage through `SetTextureStage` 0x4EB300
(cache `+0x12C[8]`, `SetTexture` only on change). `SetAlphaBlendModes` 0x501C60 is the only blend switch the effect passes use.

## 3. The figure pipeline

### 3.1 Objects and parts (`CObject3D`, 0x170) [V]

A map object owns an array of **parts** (`+0xD0`, `CFigure*`), an optional **animation system** (`+0x98`), the **model name** (`+0xA4`),
the **complection** (`+0xC4` s, d, h), a **render scene** (`+0xF0`, the blended meshes) and a **render specific** (`+0x94`,
`CObject3DClientSpecific` on the client). `LoadModel` 0x516DA0 (name, optional part names): `SetMorphParams` and `BindSkeleton` on the
animation system (the `.mod` link table -> nodes, `CFigureManager::LoadSkeleton` 0x5AC880), then the scene for the complection
(`FindOrCreateScene` through the figure manager's slot 4 / 3), then `LoadFigures` -> `CreatePart` 0x517240 per name: a `CFigure` of kind
0x41 ('A'; 0x42 with object flag 0x1000) whose `+0x158 / +0x15C` (material kind, texture number) and bounds record `+0x188` are copied from
the scene entry of that name; `SetOwner`, `BindPart` (the animation node of the same name takes it), the specific's `AttachPart` (texture set
and material slot). Units and buildings then `SetAnimation(0) + Update` (rest pose) and `ComputeBoundsFromParts` 0x518310 fills the object box
`+0x124..+0x14C` that `ClassifyCellObjects` culls with. Static objects are **activated lazily**: `CMapObject::Activate` 0x517500 creates the
pending parts (`+0xE0`) on the first `Draw`.

`BuildFromAnimation` 0x5179E0 is the server / loader path without meshes: the part records `+0x160` (`BoundsRecord` 0x38 each) are filled
from the `.fig` bounds (`LoadPartBounds` 0x5AC010) and the node positions, name prefixes `BASE` (0x400: walkable, the bridge quads),
`CROWN` (0x90), `EMPTY` (0x890) set flags, and the skeleton is released again.

`SetMorphParams` 0x517700 (new s, d, h): a new scene from the cache (or a fresh blend), every part's bounds re-copied, the animation nodes'
rest offsets rebuilt (`SetNodeRestOffset` 0x5ACB80), the object box recomputed. It is reached by `ReadNetState` section 1 and `ApplyBuffKinds`.

### 3.2 The render manager and the scene cache [V]

`CRenderManager` (0x38, vt 0x73D808, built with "figures.res" and "menus.res" opened as `CMappedResource2` archives in `+0xC`; more archives
through `OpenResourceFile` 0x4F6D30) is the world's figure loader behind `CFigureManager`:

- `GetModel` 0x4FAB00 (name) -> `FigureModel` (0x20: `.mod`, `.bon`, `.anm` resources, a hash of opened clip archives) cached in `+0x10`;
  0 when there is no `.mod` (a plain `.fig` figure). `CFigureManager::GetModel` 0x5AA5F0 is the same cache on the manager side (`+0xBC`) used by
  the animation code.
- `FindOrCreateScene` 0x4F7150 (name, part names, s, d, h, forceNew) -> `CRenderScene`: the multimap `+0x28` (name -> scenes) is scanned for a
  scene whose `+0x30..+0x38` are within 1e-5 (squared) of (s, d, h); missing parts are added to it; else `CreateScene` 0x4F7B10 (parts from the
  `.mod` entries or `<name><part>.fig`, `LoadFigFile` 0x4F7D20) or `CreateSceneFromLink` 0x4F77C0 (part list from the `.mod`'s own entry or
  `<name>.lnk`). Scenes are refcounted by their objects; nothing evicts them in this module (the world drops them with the zone).
- `BuildFigureMesh` 0x4F7F00 (FIG8) / `BuildFigureMeshOld` 0x4F87D0 (the 6-corner format) blend one part into the scene (3.3).

`CRenderScene` (0xB0, vt 0x73D790) holds six shared, 32-byte aligned arrays (`+0x3C` vertices in `VertexSoABlock`s, `+0x4C` normals,
`+0x5C` uvs, `+0x6C` `FigComponent`s, `+0x7C` morph components, `+0x8C` indices), a map part name -> `SceneRange` (`+0x9C`: start / count into
each array) and a map part name -> `SceneEntry` (`+0x14`: texture number, material kind, `BoundsRecord`, last screen rect). `AddMesh` 0x4F30F0
appends; `RemoveMesh` 0x4F3530 erases and shifts; `DrawMesh` 0x4F3C60 (slot 9) draws one part through `DrawMeshBatch` with the scene's
arrays (optionally adding morph deltas into `g_0x7AEED0`) and stores the screen rect in the entry.

### 3.3 Building a mesh from a `.fig` (`BuildFigureMesh` 0x4F7F00, `CFigure::LoadFig8` 0x5A5C70) [V]

Header (10 dwords): signature, vertBlocks, normalBlocks, uvCount, indexCount, componentCount, morphComponentCount, 0, **materialKind**
(the word 17..25: `SceneEntry +4`, `CFigure +0x158`) and **textureNumber** (`SceneEntry +0`, `CFigure +0x15C`). The bounds (3 x 96 + 32 bytes)
are skipped by the mesh builder and read by `ReadBoundsHeader` 0x5A18E0 / `LoadPartBounds` instead (the same trilinear blend into a
`BoundsRecord`: centre, radius, min, max). Then per vertex block (0x180 bytes = 3 axes x 8 corners x 4 points):

```
a = lerp(c0, c1, s); b = lerp(c2, c3, s); lo = lerp(a, b, d)      // height 0
a = lerp(c4, c5, s); b = lerp(c6, c7, s); hi = lerp(a, b, d)      // height 1
v = lerp(lo, hi, h)                                                // per axis, 4 points at once
```

into `VertexSoABlock {4 x, 4 y, 4 z, 4 w = 1}`. The pipeline's `ComputeBounds` (vt[7]) fills the entry's record from the blended vertices.
Normals (4 floats per point, one value per vertex, no morph), uvs, indices, components `{normalIdx, vertexIdx, uvIdx}` and the morph-component
trailer are copied as they are. `LoadFig8` does the same into a figure's own arrays (`+0x1C0` vertices, `+0x1D0` normals, `+0x1E0`
components, `+0x1F0` morph components, `+0x200` indices, `+0x210` uvs; `BindRawBuffers` 0x5A6D40 points the draw pointers `+0x9C..+0xC8` at
them and sizes the colour arrays `+0x138 / +0x148` to 4 x normal blocks). The kinds: 0x41 "A" (0x1E0, only vertices / normals: the parts of
a scene-backed object), 0x42 "B" (0x220, own full mesh), 0x43 `CI3DFigure` (0x284, UI figure with a scale matrix `+0x23C`, ambient
`+0x224`, texture `+0x280`), 0x44 `CSkyObject` (`CFigureManager::CreateByKind` 0x5AA8E0).

### 3.4 Drawing an object (`CObject3DClientSpecific::Draw` 0x51B7E0, slot 19, from `CWorldClient::RenderObjects`) [V]

1. `Activate`; the lighting cache flag 0x400 of the object cleared unless 0x200 (static lighting); `CAnimationSystem2::Update` (3.6).
2. The nearby lights: every light of `g_0x7AF748` within `range (+0xE0) + 10` of the object, not hidden; a light flagged 0x100 (moved)
   invalidates the cache. The sun is not in this list (RenderLit adds `g_0x7AF07C` itself).
3. The object matrix (`Quat_ToMatrix4` of `+0x34` into `+0x54`, position `+0x1C` in column 3) pushed on stacks 2 and 1 (and 3 when relighting);
   a scale matrix from `+0xF0..+0xF8` on top when not 1.
4. Edge alpha = `CTerrain::GetEdgeFadeAlpha(position)` unless flag 0x800 (objects fade out at the map edge).
5. For each of the two **texture sets** (`+0x20` primary: texture kinds 1, 3, 4, 6..9, 0xB, 0xD..0x10 = the body / skin; `+0x24`
   secondary: the other kinds, e.g. 2 = items with their own texture): renderer `SetTexture(0, set texture)`, `CRenderDevice::SetMaterial`
   (the wounds blend `+0xDC` for set 0); for each **material slot** of the set (one `CMaterial` of kind 17..25) for each part: part flag 0x10
   (colours valid) cleared unless cached, the part matrix pushed, `DrawFigureLit(part, lights, material, edge alpha, rect)` = `RenderLit`,
   flag 0x40 (drawn) unions the part rect into `+0xC8`, pops.
6. When every part had valid colours: object flag 0x400 (lit cache) and `+0xEC` = time; the union rect -> owner `+0x100` (picking:
   `HitTestPoint` 0x51C130 / `HitTestPointParts` 0x51C160 / `HitTestRect` 0x51C1D0); object flag 0x20 = something drawn; `SetMaterial(0)`.

`AttachPart` 0x51B050 is where a part lands in a set / slot: by `textureNumber` (`GetOrCreateTextureSet` 0x51B2F0 -> `CWorldClient::FindTexture
(kind)`) and `materialKind` (`GetOrCreateMaterial` 0x51B470 -> `CWorldClient::FindMaterial(kind)`; `RegisterMaterials` 0x51C970 registers them
with the material manager). `SetTextureNames` 0x51C250 swaps the set textures by name (the composed skins "<prefix>skin_NN.mmp", armor
textures); `ReplaceTexture` 0x51B630 swaps one of the same kind.

### 3.5 Drawing a part (`CFigureClientSpecific::RenderLit` 0x5A7A70 / `Render` 0x5A7640 -> `DrawMeshBatch` 0x4E9010) [V]

1. Hidden (figure flag 0x80) -> nothing. `CullBox(figure +0x188)` on stack 2: 2 -> nothing, 1 -> draw flag 2 (clip).
2. Texture numbers 0xD / 0xE / 0x10 are animated uv grids (4x2 of 8, 4x4 of 16, 8x4 of 32 frames; `AnimateTextureFrame` 0x5A7160 sets the
   uv offsets `+0xCC / +0xD0` from the integer world time).
3. Colours: flag 0x100 -> constants `+0xD4 / +0xD8` (UI figures, `Render` path); flag 0x10 -> cached; else the lights and the negated sun
   direction are transformed into figure space with the inverse of stack 3's top (light `+0x28`, sun `+0x108`), `CLight::PrecomputeMaterialTerms`
   with the material and `g_0x79BE9C`, then pipeline vt[40] `LightVertices` (or, for a scene part, the scene's slot 8) writes `+0x138`; "crown"
   parts pull their colours 60 % toward the mean; `+0x148` = ambient << 24 unless the material flag 0x10; flag 0x10 set.
4. The `.anm` morph deltas `+0x128` are added into the static SoA copy `g_0x7C2E48` (`g_0x7C2E60` for `Render`).
5. `DrawMeshBatch(device, vertices, blocks, normals, normalBlocks, diffuse, specular, uvs, uvCount, components, compCount, -, -, indices,
   indexCount, uOffset, vOffset, alpha, flags)`: transform (vt[14] on stack 1, or vt[9] on stack 2 when flag 2), result 2 -> culled; flag 4 ->
   constant device colours; the `VertexTL`s are built in `g_0x7AEE00` from the components (position from `g_0x7AEE10`, diffuse / specular by
   the **normal index**, uv + offsets); clipped: outcodes per component, vt[37] `ClipTriangles`, vt[34]; flag 1 -> screen rect
   (`Mesh_ComputeBounds2D_B` 0x4E86B0 into `device +0x15C`); flag 8 -> `SPECULARENABLE` around the call; `DrawIndexedWithMaterial`.
6. The rect is copied to `+0x20` of the specific and the out RECT; flag 0x40; `DrawBoundsDebug` 0x5A7290 (world flag 0x4000).

With blend textures (`CreateWoundsTexture` 0x51C3D0 made `+0xD8` and the `textureBlend +0xDC`) the draw goes through
`DrawMeshBatchBlended` 0x4E9A50: the same mesh twice, the second pass with the wounds texture alpha-blended (`DrawIndexedTwoPassBlend`).

`CI3DFigure::Draw` 0x5E6D90 (UI figures) is the unlit variant: texture `+0x280`, the ambient `+0x224` as the renderer's ambient for the
call, `DrawFigure` (slot 14 = `Render`) with constant colours.

### 3.6 Shadows, invisibility, stun, colour push / pop [V]

- **Shadow** (`RenderShadow` 0x51CE60, slot 20, called from `RenderObjects`' last pass through `CMapObject` slot 24): a 64x64 A4R4G4B4
  silhouette of the parts projected along the light (sum of the point lights of kind 0x31 in range, weighted by colour and distance, plus the
  sun, clamped to >= 20 degrees), rasterised flat with the 2D MMX blitter, blurred at quality 2, uploaded with `IDirect3DDevice7::Load` into
  `+0x7C`, and draped on the terrain triangles of the light prism (`CWorldClient::BuildShadowReceiverMesh` -> `DrawTrianglesTransformed` with
  z * 0.9999 and diffuse 0x80FFFFFF). Not a stencil, not a projected depth test: `docs/shadows.md` has the complete map and the um-engine
  replacement. `IsShadowEnabled` 0x51E200 / `IsShadowVisible` 0x51E300 gate it per kind (`ShadowQuality` 0x79BD0C, `UnitShadow` 0x79BD14,
  `FloraShadow` 0x79BD1C, `BuildingShadow` 0x79BD18).
- **Invisibility**: `SetMaterialsAlphaByHour` 0x51CC20 sets every material's alpha `+0x2C` to `0.2 + ramp(time) / 30` (a shimmer),
  `SetMaterialsOpaque` 0x51CCA0 ends it. **Stun**: `SetMaterialsEmissiveByHour` 0x51CCD0 pulses the emissive `+0x40` and flag 0x10,
  `ClearMaterialsEmissive` 0x51CD60 ends it. **Colour push / pop**: `SaveRestoreMaterialColors` 0x51C9F0 pushes the diffuse `+0x50..+0x5C` of
  every material on a per-material deque and resets it to 1.0; pop restores (the death fade of `CUnitClientSpecific`).
- **Scale**: `GetScale` 0x51AD20 returns `+0xF0..+0xF8`; `Draw` pushes a diagonal matrix when they are not 1 (the only true scaling in the
  module; the buffs do not use it).

## 4. The animation system

### 4.1 Objects [V]

`CAnimationSystem2` (0x78, kind 0x300D): owner `+0x1C`, root node `+0x20`, node list `+0x24`, current `.adb` entry `+0x40`, queue `+0x44`,
time `+0x60` (frames), end callback `+0x64`, morph params `+0x68`, dirty `+0x74`. `CAnimationNode` (0xA0, kind 0x300C): name / parent name /
model name, parent `+0x28`, children `+0x2C`, bound part `+0x48`, current track `+0x4C` (= main track `+0x50`), rest offset `+0x5C`, morph
params `+0x68`, world position `+0x74`, world rotation `+0x80`. `CAnimationTrack` (0x78, kind 0x300B): entry `+0x1C`, node `+0x20`, position
keys `+0x24 / +0x28`, rotation keys `+0x2C / +0x30` (pointers into the mapped `.anm`, not copied), morph `V +0x34`, frame pointers `+0x38`
(`F +0x3C`), time `+0x48`, evaluated position `+0x4C`, rotation `+0x58`, morph vertices `+0x68`.

### 4.2 Loading [V]

`LoadSkeleton` 0x5AC880: the `.mod`'s own entry (or `<model>.lnk`): count, then (part name, parent name) pairs -> `CreateNode` 0x50CC60 (the
parent adopts it; `SetNodeRestOffset` 0x5ACB80 blends the `.bon` entry of the node name: 0x60 bytes = 8 corners (`Interpolate8` 0x50B8C0,
same corner order as the `.fig`), 0x48 = 6 corners (`Interpolate6`)); `SelectRootNode` 0x50D2E0 = the first node with an empty parent name.
`SetupAnimation` 0x5ACCF0 (system, model, entry): the clip archive from the `.anm` (`FigureModel::GetAnimationResource` 0x5A9EF0, cached per
clip name), then `LoadTracks` 0x50DA10: every node's track from the clip's entry of the node's name (`CAnimationTrack::LoadFromData` 0x50AAF0:
u32 N, N quats (w, x, y, z), u32 N, N Vec3, u32 V, u32 F, F x V Vec3 deltas); a node without an entry keeps its rest pose. Without a `.anm`
entry the per-file fallback `LoadNodeTrack` 0x5ACA60 (`<model><node><clip>` + ext) is tried.

### 4.3 Playing [V]

- `SetAnimation(entry)` 0x50CA50: tracks loaded, `SetTime(0)`, `+0x40 = entry`. `QueueAnimation(entry)` 0x50CBA0 appends to `+0x44`.
- `SetTime(t)` 0x50D600: `t` in frames = world ticks. No current entry but a queue -> the first queued entry starts at 0. While
  `entry +0x20 (last frame) < t`: `t -= lastFrame`, the end callback, the next queued entry; with nothing queued and a **non-looping** entry
  (`flags +0x14 & 0xC0000000 != 0x80000000`) `t` clamps to the last frame (the pose holds). A change of entry reloads the tracks.
- `Update` 0x50D450 (when dirty): every node's current track `Evaluate(time)` 0x50ACD0: `f = floor(t)`, `u = frac`: position = lerp(key f,
  key f + 1, u) (last key beyond the end, zero without keys), rotation = `Quat_Slerp` 0x49ED20 (identity without keys), morph vertices = lerp of
  frames f, f + 1; then `ApplyTransforms` 0x50D3E0 -> `UpdateTransform(root, offset)` 0x50C230 with `offset = GetModelLerp(owner, h) *
  modelOffset (+0xF4)`.
- `UpdateTransform` (node, offset): **root**: `worldPos = track position * GetModelLerp(h) - offset`, `worldRot = track rotation`.
  **child**: `worldPos = parent.worldPos + rotate(parent.worldRot, restOffset +0x5C)`, `worldRot = parent.worldRot * track rotation`
  (quaternion product, parent first). The bound part gets `+0x1C = worldPos`, `+0x34 = worldRot` and `CFigure::SetMorphDeltas(track +0x68)`;
  recursion over the children. So the track positions of non-root parts are ignored (they are the rotated rest offsets anyway:
  `figure-format.md` section `.anm`), and a clip plays on any complection because the rest offsets are blended, not stored.
- `GetModelLerp(h)` 0x518D30: for units with an `.adb`, `((hdr +0x28 - hdr +0x20) * h + hdr +0x20) / hdr +0x24` from the three header floats
  of the `.adb` (file offsets 0x20, 0x24, 0x28: "0.507 1.052 1.073" for Male): the root position and the model offset scale with the height
  coefficient. (`adb-format.md` lists these floats as unknown.)
- The hand-over: `CUnitClientSpecific::SetAnimation(time)` 0x5352B0 (slot 14, from the replicated section 8: clip index `CUnit +0x254`,
  next `+0x255`, start tick `+0x258`): `CUnit::GetAnimRecord` turns the indices into `.adb` entries, `SetAnimation(current)`,
  `QueueAnimation(next)`, `SetTime(time + 1 - startTick)` when positive. Index -2 = the weapon idle set (`ApplyWeaponAnimMask`). The server
  decides durations (`CUnit::AnimDurationFor`); the client only replays.

### 4.4 Old animation code (not the game path) [V]

`AnimNodeOld_*` 0x505410-0x508D30, `CAnimationSystemOld::ReadNodes` 0x508D30, `CFigureManager::MakeAnimationNode / LoadAnimationFile /
LoadSkeletonOld` 0x5AC400-0x5AC770 read the `.ani` double-precision keys of the 3ds tool chain (`CFakeFigure`, `Scene3ds_BuildFigures`).
Nothing in the unit / object draw calls them.

## 5. `CMapObject`: net state and buff visuals

### 5.1 The object's part of the 0x50 / 0x51 payloads (`WriteNetState` 0x50EAF0 / `ReadNetState` 0x50EC30) [V]

The `DeltaCodecSet` at `+0x1C4` holds one `NetSection` (0xA0: four CMemStreams) per section; `PackFullNetState` 0x50EA40 (mode 0: every
field) and `PackDeltaNetState` 0x50EA70 (mode 1: only the fields whose value changed, section by section from the dirty mask `+0x1DC`,
`MarkNetDirty` 0x50E9F0) write; `UnpackNetState` 0x50EAB0 reads and returns 0 when a section was not consumed exactly (the connection
drops the object). The sections of a plain map object:

| section | written | read |
|---|---|---|
| 0 | the packed placement `+0x17C` (`Placement_WriteNet`: HP / max HP / mana shorts ...) | `DeltaReader::ReadUnitSmall`; a changed HP (`+0x17E`) or a full read -> vt[58] |
| 1 | the complection `+0xC4` (12 bytes) | `SetMorphParams` (the scene is rebuilt from the cache) |
| 2 | byte = flag bit 3, position `+0x1C` (12), quaternion `+0x34` (16) | flag 8, the position through the world's slot 41 (grid move), `+0x34`, slot 51 `SnapZToTerrain` |
| 3 | byte count, per buff (0x14): kind byte, level byte, duration u16 (+4), byte +0x10 | the array `+0x198` resized and filled; non-units: specific vt[13] `Show` (`UpdateBuffEffects` 0x510B80 / `UpdateBuffSounds` 0x5112A0) |

`CUnit` adds sections 4..13 (`combat.md` section 8). `ApplyBuffKinds` 0x50F2D0 (slot 27) turns the buff kinds 0x1D / 0x1E / 0x25 / 0x26 into
+-0.4 of the complection and marks section 1 dirty; `ClearBuffs` 0x50F5B0 resets. The server ticks the durations in
`CMapObjectServer::TickBuffs` 0x515820.

### 5.2 Buff visuals (`CMapObjectClientSpecific`, 0x58) [V]

`UpdateBuffEffects` 0x510B80: one `CEffect` per buff kind (highest level) in `+0x28` (0x10 records: kind, level, effect, active), rescaled on
a level change. `UpdateBuffSounds` 0x5112A0 / `BuffSound_Start` 0x5114D0: a looping sound per kind in `+0x3C` ("magic\\..." folders by kind).
`Update` 0x510890 (slot 6): a hero's buff below 30 ticks plays "magic\\end1.wav" once (`+0x54`), stench (0x13) plays a random
"magic\\Stench%i.wav" every 16 ticks, kind 0x20 keeps its sound at the owner, and the object light `+0x50` (heroes / quest items, kind 0x31,
`ObjLight_Create` 0x511830 at z + 2, `SetQuestLight` 0x511A00) follows the owner through `CTerrain::MoveLight`.

## 6. What is not known

- The exact meaning of pipeline slots 5 (`SetFog`?) and 35 (the `+0x158` post-process) and the precise clipping algorithm live inside
  `3DfpFPU.dll`; only the call shapes were read. [?]
- `CFigureClientSpecific +0x30..+0x38` (three 1.0 floats) and `CI3DFigure +0x27C` (lighting params) are set but their readers were not found. [?]
- `CStateBlockSet::CaptureEffect` 0x4FDBC0 / `ApplyDefaultEffect` 0x4FDD80 were named from their shape, not from a reader of their callers
  (the material manager, other page). [?]
- `BuildFigureMeshB / C` 0x4F9FD0 / 0x4FA670 and `LoadFigFile2` 0x4F9E50 (the "second path" with `GetNamedValue`) were not read.
- Which objects use the alternate node tracks `+0x54 / +0x58` (`SetTrackAnimationB / C`) was not found.
- `CMapObject +0x178` (1.0) and `+0x1A8` (a CString) are guesses. [?]

## 7. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| `findings.md` 0x4E3000 section, `functions.tsv` | "DrawMeshBatch ... appends indices (CVertexBuffer::AppendIndices) and flushes", "FlushVertexBuffer ... draw-queue accounting" | the vertex-buffer path is dead: `ReserveBatchVertices_Stub` 0x4E7820 returns 0, `CreateVertexBuffer` has no callers; every batch is an immediate `DrawIndexedPrimitive`; `FlushVertexBuffer` returns at once (um-engine's `FlushPending` is harmless but a no-op) |
| `functions.tsv` | `DDraw_EnumDriversCallbackEx_Add` 0x4ED820 "DirectDrawEnumerateEx callback" | the `IDirect3D7::EnumDevices` callback (description, name, D3DDEVICEDESC7) |
| `functions.tsv` | `CRenderDevice::SetMaterialTextures` 0x4EB550 "bind the textures of a material" | it is the draw: `DrawIndexedWithMaterial` (state blocks per pass + `DrawIndexedPrimitive`) |
| `functions.tsv` | `DrawQuadBatchClipped` 0x4E9A50 "clipped 2D quad / sprite batch", `DrawShadowQuadBatch` 0x4EA4A0 | `DrawMeshBatchBlended` (the two-texture wounds pass of a figure) and `DrawTrianglesTransformed` (plain xyz triangles: shadow receiver, prints, arrows) |
| `functions.tsv` | `CRenderScene::GetVertexData / Ex` 0x4F3C60 / 0x4F42D0 "copy vertex data of a named entry" | they draw a part (`DrawMesh / DrawMeshBlended`) |
| `functions.tsv`, `findings.md` | "NewFigureInstance 0xB0 bytes figure instance", `FindNearestFigure` "nearest in a float key" | the 0xB0 object is a `CRenderScene`; the search is an exact match (1e-5) on the complection: the per-build mesh cache |
| `functions.tsv` | `CViewFrustum::CullBox` "transform a box ... by the current matrix stack (+0x24, depth +0x28)" | `this` is the `CD3DLibrary` render context `g_0x7AF068`; stack 2 is clip space; the record is a `BoundsRecord` {flags, centre, radius, min, max} |
| `structs.tsv` | `CMapObject +0xC4 morphScale "x / height / y scale factors"` | the three complection coefficients (s, d, h) of `CObject3D::SetMorphParams`; the field is CObject3D's; buffs change them by 0.4 and the mesh is re-blended from the 8 corners, nothing is scaled |
| `findings.md` 0x50A000 | "CObject3D +0xc4..+0xcc are NOT scale but the 3 morph parameters" (right) but "the .adb entry: +0x20/+0x24/+0x28 model lerp" | the model lerp floats are the **.adb header's** three floats (file +0x20..+0x28, read through the entry's base), not fields of the entry (whose +0x20 is the last frame) |
| `adb-format.md` | header `f32[3] ?` | the root-position / model-offset scale at height 0, the reference, at height 1 (`GetModelLerp` 0x518D30) |
| `figure-format.md` | header `unknown2` "render flags: 17..25 ... probably the render flags (blending, two-sided)" | the **material kind** looked up with `CWorldClient::FindMaterial(kind)` (`AttachPart` 0x51B050); `textureNumber` is the texture kind for `CWorldClient::FindTexture` |
| `figure-format.md` | "`.fig` header 36 bytes" | 40 bytes (10 dwords); the bounds start at 0x28 and the vertex blocks at 0x168 (checked on vanilla files) |
| `notes-renderer-init.md` | "CRenderDevice ... pick the HW/SW device" | only `IID_IDirect3DHALDevice` is accepted (the TnL HAL is skipped, a software device never) |
| `shadows.md` (older section) | "`g_0x7AF230..238` sun vector" | confirmed zero (`StaticInit_SunDirFallback` 0x51A2A0): the light vector is built from the light manager and the sun object |
| `functions.tsv` | `CStateBlockSet::Record / Capture / CRenderBatch::*` | the "default effects" (9 render-state sets compiled into D3D state blocks) registered by id; see 2.4 |

## 8. Function index (one line each; full purposes in the batch file)

**Device** — 0x4EDD70 CDXDirectDraw ctor; 0x4EEE30 CreateSurfaces; 0x4EE5D0 FindHw3dDriver; 0x4EE760 EnumDriverModes; 0x4EEA90 CreateDriver;
0x4EEBC0 SetDisplayMode; 0x4EECF0 SetCooperativeLevel; 0x4EE330 ~CDXDirectDraw; 0x4ED610 DDraw_EnumDriversCallback_Add; 0x4ED820
D3D_EnumDevicesCallback_Add; 0x4E5E50 CRenderDevice ctor; 0x4E68A0 ~CRenderDevice; 0x4E6C30 CreateZBuffer; 0x4E6BC0 FindZBufferFormat; 0x4E6EE0
EnumTextureFormats; 0x4E7C40 FindTextureFormat; 0x4E72A0 CreateVertexBuffer (dead); 0x4E7700 CVertexBuffer ctor (dead); 0x4E7820
ReserveBatchVertices_Stub; 0x4E7840 / 0x4E7A40 FlushVertexBuffer / 2 (no-ops); 0x4E7E00 AppendBatchIndices (dead); 0x4E9010 DrawMeshBatch;
0x4E9A50 DrawMeshBatchBlended; 0x4EA4A0 DrawTrianglesTransformed; 0x4EB550 DrawIndexedWithMaterial; 0x4EB370 DrawIndexedTwoPassBlend; 0x503680
DrawIndexedPrimitive; 0x502A20 DrawIndexedTriangles (device); 0x4EB170 SetMaterial; 0x4EB1D0 SetTextureByDesc; 0x4EB300 SetTextureStage; 0x4EBAF0
SetTextureFromObject; 0x4E7FB0 / 0x4E86B0 / 0x4E8900 Mesh_ComputeBounds2D_A / B / C; 0x4EDAA0 PixelFormat_FromDDPF; 0x4EDC00 / 0x4EDC70
PixelFormat_PackRGBA / UnpackRGBA; 0x4EF7B0 CDXDirectDrawSurface ctor; 0x4EFA60 CreateDDSurface; 0x4EF690 CDDSurface ctor; 0x4F0060 LoadPicture;
0x4EF510 CreateClipper; 0x4EF610 Blit.

**Context / renderer** — 0x4F0B90 CD3DLibrary::Load; 0x4F0E10 ~CD3DLibrary; 0x4D9B80 MatrixStack_Push; 0x51C110 MatrixStack_Pop; 0x4D9880
Matrix4_Mul; 0x4F0E80 Mat4_Perspective; 0x4F1370 Mat4_Viewport; 0x4F1330 Mat4_Identity; 0x4F13F0 Mat4_Translation; 0x501980 Matrix4_Transpose;
0x4F16A0 CullBox; 0x4F2650 / 0x4F29F0 TransformForDraw / 2; 0x4F1440 Shadow_BuildLightProjectionMatrix; 0x500A70 I3DFigureImpl::Ctor; 0x500AC0
dtor; 0x500540 LoadLibraries; 0x500B70 InitRenderer; 0x5015A0 SetupCamera; 0x503F40 BeginFrame; 0x5040A0 EndFrame; 0x501A40 Clear; 0x501AB0
SetFogColor; 0x504A40 GetFogRGB; 0x500740 SetAmbientColor; 0x500800 SetSunLight; 0x5006E0 ReleaseSunLight; 0x501AF0 SetTexture; 0x501C60
SetAlphaBlendModes; 0x501BB0 DrawIndexedTriangles; 0x501D00 DrawFigure; 0x501D40 DrawFigureLit; 0x501D90 DrawTexturedPolyline; 0x502CA0
DrawParticleQuads; 0x503A40 DrawArrow3D; 0x5041F0 DrawRect2D; 0x5045D0 DrawPolyline2D; 0x504860 DrawSprite; 0x504A60 SetGamma; 0x504780
RestoreSurfaces; 0x5019D0 GetStackTop; 0x4FCA60 CStateBlockSet ctor; 0x4FCE90 BuildDefaultEffect; 0x4FD2A0 RegisterEffect; 0x4FD9D0
DeleteEffects; 0x4FDB00 EraseEffect; 0x4FDB50 FindEffect; 0x4FDBA0 GetDefaultEffectHandle; 0x4FDBC0 CaptureEffect; 0x4FDD80 ApplyDefaultEffect.

**Render manager / scene** — 0x4F68C0 CRenderManager ctor; 0x4F6D30 OpenResourceFile; 0x4FAB00 GetModel; 0x4F7B10 CreateScene; 0x4F77C0
CreateSceneFromLink; 0x4F7150 FindOrCreateScene; 0x4F75E0 GetOrCreateFigure; 0x4F7480 LoadFigure; 0x4F7D20 LoadFigFile; 0x4F7370 MakeFigureName;
0x4F7F00 BuildFigureMesh; 0x4F87D0 BuildFigureMeshOld; 0x4F9FD0 / 0x4FA670 BuildFigureMeshB / C; 0x4F7E70 LoadAnimatedPicture; 0x4F2C00
CRenderScene ctor; 0x4F30F0 AddMesh; 0x4F3C60 DrawMesh; 0x4F3FA0 DrawMesh2; 0x4F42D0 DrawMeshBlended; 0x4F4150 DrawMeshBlended2; 0x4F3780
GetMeshRange; 0x4F3800 GetEntryValue; 0x4F3040 HasMesh; 0x4F3530 RemoveMesh; 0x4F45E0 RecomputeScreenRect; 0x4F4680 MorphAndProjectEntry;
0x4F4850 RasterizeEntry2D; 0x4F4B50 EstimateTriangles; 0x4F4B90 FindOrCreateEntry; 0x4F4CE0 AddEntry; 0x4F3870 / 0x4F3AD0 AddQuads / Ex (UI
quads); 0x5A9A50 FigureModel ctor; 0x5A9C90 FigureModel::Destroy; 0x5A9EF0 GetAnimationResource.

**CFigure** — 0x5AABC0 ctor; 0x59F030 dtor; 0x59F210 Init; 0x59F2B0 Done; 0x5A6C70 InitMesh; 0x5A6C80 DoneMesh; 0x5A4CF0 LoadFigHeader; 0x5A5C70
LoadFig8; 0x5A4D60 LoadFigOld; 0x5A2490 Load (CI3DFigure); 0x5A2500 CI3DFigure::ParseFIG; 0x5A3800 CI3DFigure::BuildMorphed; 0x5A18E0
ReadBoundsHeader; 0x5A1210 ReadBoundsFIG6; 0x5AAE60 / 0x5AB9A0 PartRecord_ReadBoundsFIG8 / 6; 0x5A37C0 ComputeBounds; 0x5A6D40 BindRawBuffers;
0x5A0B10 SetMorphDeltas; 0x5A0CD0 MorphAndProjectVertices; 0x5A0E80 RasterizeSilhouette2D; 0x59FDF0 AccumulateWorldBounds; 0x59F3F0
SetRotationQuat; 0x59F510 UpdateWorldTransform; 0x59F0F0 CopyFrom; 0x5A4B30 NegateQuats; 0x59F3D0 SetOwner; 0x59F390 BindClientSpecific; 0x453440
GetName; 0x5A7010 CFigureClientSpecific ctor; 0x5A70E0 Init; 0x5A7640 Render; 0x5A7A70 RenderLit; 0x5A7160 AnimateTextureFrame; 0x5A7290
DrawBoundsDebug; 0x5A9310 DrawProjected64; 0x5A8B00 / 0x5A8E60 / 0x5A8FB0 ComputeScreenRect / 2 / 3; 0x5E6C60 CI3DFigure::Init; 0x5E6D90
CI3DFigure::Draw; 0x5AA070 CFigureManager ctor; 0x5AA5F0 GetModel; 0x5AA8E0 CreateByKind; 0x5AAE00 RegisterInstant; 0x5AC2B0 CreatePart;
0x5AC2F0 AttachPart; 0x5AC010 LoadPartBounds; 0x5AC1A0 LoadStandaloneFigure; 0x5AC880 LoadSkeleton; 0x5ACA60 LoadNodeTrack; 0x5ACB80
SetNodeRestOffset; 0x5ACCF0 SetupAnimation.

**Animation** — 0x50A5D0 CAnimationTrack ctor; 0x50A810 / 0x50A8C0 Init / Done; 0x50A960 Reset; 0x50AAF0 LoadFromData; 0x50ACD0 Evaluate;
0x50AF40 CAnimationNode ctor; 0x50B370 / 0x50B510 Init / Done; 0x50B8C0 Interpolate8; 0x50BC00 Interpolate6; 0x50BF00 RemoveChild; 0x50BFA0
DetachChildren; 0x50C0D0 EnsureTrack; 0x50C100 / 0x50C170 / 0x50C060 SetTrackAnimation / B / C; 0x50C230 UpdateTransform; 0x50C5D0 LoadTrack;
0x50C5F0 CAnimationSystem2 ctor; 0x50C860 / 0x50C8E0 Init / Done; 0x50C990 ReleaseNodes; 0x50C9E0 ResetFields; 0x50CA50 SetAnimation; 0x50CBA0
QueueAnimation; 0x50CC20 FindNodeByName; 0x50CC60 CreateNode; 0x50CD70 AddNode; 0x50D0E0 BindPart; 0x50D2E0 SelectRootNode; 0x50D320
BindSkeleton; 0x50D350 UnbindPart; 0x50D3A0 UnbindAllParts; 0x50D3E0 ApplyTransforms; 0x50D450 Update; 0x50D5A0 ClearQueue; 0x50D600 SetTime;
0x50D7C0 SetMorphParams; 0x50D920 CollectNodeNames; 0x50D950 ResetTracks; 0x50DA10 LoadTracks; 0x518D30 CObject3D::GetModelLerp; 0x5187B0
CObject3D::SetAnimation; 0x518860 ResetAnimation; 0x5352B0 CUnitClientSpecific::SetAnimation (the hand-over, other module).

**CObject3D / CMapObject and specifics** — 0x515BD0 CObject3D ctor; 0x515D70 dtor; 0x515F70 InitFields; 0x516080 DoneFields; 0x516DA0
LoadModel; 0x517110 LoadFigures; 0x517240 CreatePart; 0x5164B0 AddPart; 0x5167E0 RemovePart; 0x5169A0 / 0x516A80 / 0x516920 FindPartByName /
NameStart / TypeId; 0x517700 SetMorphParams; 0x5179E0 BuildFromAnimation; 0x518100 ComputeBoundsFromFigures; 0x518310 ComputeBoundsFromParts;
0x518CA0 GetAnimDb; 0x5190C0 PickAnimDbEntryByMask; 0x519380 PickAnimDbEntryByKey; 0x517500 CMapObject::Activate; 0x50E2D0 CMapObject ctor;
0x50E770 Init; 0x50E9F0 MarkNetDirty; 0x50EA40 / 0x50EA70 PackFull / DeltaNetState; 0x50EAB0 UnpackNetState; 0x50EAF0 WriteNetState; 0x50EC30
ReadNetState; 0x50F1E0 AddBuff; 0x50F2D0 ApplyBuffKinds; 0x50F5B0 ClearBuffs; 0x50F630 HasBuffKind; 0x50F690 GetBuffValue; 0x50E440
ResetMorphParams; 0x50F7A0 / 0x50F900 Read / Write; 0x515820 CMapObjectServer::TickBuffs; 0x510450 CMapObjectClientSpecific ctor; 0x510890
Update; 0x510B80 UpdateBuffEffects; 0x5112A0 UpdateBuffSounds; 0x5114D0 BuffSound_Start; 0x511830 ObjLight_Create; 0x511A00 SetQuestLight;
0x51AB00 CObject3DClientSpecific ctor; 0x51AEC0 / 0x51AF70 Init / Done; 0x51B050 AttachPart; 0x51B160 / 0x51B210 DetachPart / ByKey; 0x51B2F0
GetOrCreateTextureSet; 0x51B470 GetOrCreateMaterial; 0x51B5B0 FindMaterial; 0x51B630 ReplaceTexture; 0x51B7E0 Draw; 0x51C130 / 0x51C160 /
0x51C1D0 HitTestPoint / Parts / Rect; 0x51C250 SetTextureNames; 0x51C3D0 CreateWoundsTexture; 0x51C920 ReleaseWoundsTexture; 0x51C970
RegisterMaterials; 0x51C9F0 SaveRestoreMaterialColors; 0x51CB30 RebuildMaterials; 0x51CC20 / 0x51CCA0 SetMaterialsAlphaByHour / Opaque;
0x51CCD0 / 0x51CD60 SetMaterialsEmissiveByHour / ClearMaterialsEmissive; 0x51CE60 RenderShadow; 0x51E200 IsShadowEnabled; 0x51E280
ResetShadowBuffers; 0x51E300 IsShadowVisible; 0x51AD20 GetScale.
