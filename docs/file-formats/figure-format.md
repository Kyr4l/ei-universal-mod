# Evil Islands 3D Model File Formats (.fig / .bon / .mod / .anm / .lnk)

## Overview

The `figures_res` archive holds every in-game 3D model: units, creatures, weapons, items, and static scenery props. Five file types work together:

| Extension | Role |
| :--- | :--- |
| `.fig` | A single mesh: vertices, normals, UVs, triangle indices. Vertices are not static — see "Complection Morphing" below. |
| `.bon` | Per-part assembly offsets used to position the parts of a composite (multi-mesh) model relative to each other. |
| `.mod` | A composite model: a small nested **RES archive** containing a body-part hierarchy plus one `.fig`-format mesh per part. |
| `.anm` | Skeletal animation clips (rigid per-part rotation/position tracks), for the handful of models that are actually animated. |
| `.lnk` | A tiny attachment-socket redirect (e.g. "when asked for `initwecb4`, actually load `initwecb4weapon`"). |

Most figures in the game (56 of the ~70 unique models sampled from a subset of the vanilla `figures_res`) are simple: one `.fig` + one `.bon` pair, no hierarchy, no animation — a single mesh whose shape morphs by body build (strength/dexterity/height) using the 8-corner interpolation described below.

The rest are composite `.mod`+`.bon` rigs (see below) — the full `figures_res` has 42 of these, not just a handful. Most static-scenery ones (e.g. `goldpile00`, `stwa*`, `jbr*`, `jstatue*`) ship no `.anm` and only need the static part-hierarchy assembly. Every actual creature/unit rig does ship its own `.anm` and has playable animation clips - an earlier draft of this doc undercounted this at "only 4" based on a too-small sample; the real count across vanilla `figures_res` is over 50 (`unhuma`, `unhufe`, `unanwicr`, `unmodg3`, `unmohi`, `unorma`, and every other `un*` creature/unit prefix all have a matching `.anm`). Notably, the base human models used by the large majority of humanoid NPCs in the game (`unhuma`/`unhufe` — "Human Male"/"Human Female" in the Units database) are *also* full composite rigs (`unhuma.mod` alone has 348 body-part/equipment-variant entries) — see "Body-Part-Name Convention for Equipment Variants" below for what most of those entries actually are.

**Primary source**: the `.fig` format below was recovered from the open-source map editor [`ei_maper`](https://github.com/nsgundy/ei_maper)'s own reader (`ei::CFigure::readData`, `figure.cpp`/`figure.h`), which is authoritative — it is a working, shipped parser for this exact format, not a guess. The `.bon`/`.mod` container structure was reverse-engineered from `ei_maper`'s `CObjectList::readAssembly()` (`resourcemanager.cpp`) plus direct byte verification against the vanilla `figures_res` files. The `.anm` format has **no known reference implementation** (`ei_maper` is a static level editor and does not play animations) — it was reverse-engineered from scratch by byte analysis, cross-checked against the bone names found in the corresponding `.mod` hierarchy.

All multi-byte integers and floats are little-endian.

---

## `.fig` — Mesh Format

### Header

```
uint32  signature          // 'FIG8' little-endian = 0x38474946
int32   vertBlocks         // number of 4-point vertex blocks
int32   normalBlocks       // number of 4-point normal blocks
int32   uvCount            // number of UV coordinate pairs
int32   indexCount         // number of triangle-list indices (uint16 each)
int32   vertexComponentCount // number of {normalIdx, vertexIdx, uvIdx} triples
int32   morphingComponentCount // always == vertBlocks * 4 (see "Unread Trailer" below)
int32   unknown            // 0 in all 3564 vanilla meshes
int32   unknown2           // see "Header fields: values" below (render flags: 17..25)
int32   textureNumber      // see below: the kind of texture the mesh uses (0..8)
```

### Complection Morphing (the 8-corner cube)

Evil Islands units are not rigged with bone weights per vertex. Instead, each unit has three build sliders — **strength**, **dexterity**, and a third axis whose corner variants are labeled "scaled" in both known reference implementations (`ei_maper`'s `figure.cpp:261` comment reads `x == str, y == dex, z == scale`; the community Blender plugin `ei_figer` names corners 4-7 `b~`/`p~`/`g~`/`c~` for "big/power/growth/common (scaled)") — (`complection.x/y/z`, each in `[0,1]`) — and the `.fig` file stores the mesh's shape at all **8 corners of that unit cube** (str=0/1 × dex=0/1 × scale=0/1). At load/pose time the engine trilinearly interpolates between the 8 corners using the unit's actual complection values to get the final vertex positions. This is why every per-vertex array below is stored as "8 morph variants" rather than one. (Earlier drafts of this doc called the third axis "height/tallness" — that was a guess and is not what either reference implementation calls it.)

The interpolation itself (`ei::calcComplection`, mirrored in `CFigure::calculateConstitution`):
```cpp
// data[0..7] are the 8 morph corners in this fixed order (indices into all "×8" arrays below):
// 0: str0,dex0   1: str1,dex0   2: str0,dex1   3: str1,dex1   (all at scale=0)
// 4: str0,dex0   5: str1,dex0   6: str0,dex1   7: str1,dex1   (all at scale=1)
res0 = lerp(data[0], data[1], dex);
res1 = lerp(data[2], data[3], dex);
res2 = lerp(res0,    res1,    str);          // scale=0 result
res0 = lerp(data[4], data[5], dex);
res1 = lerp(data[6], data[7], dex);
res0 = lerp(res0,    res1,    str);          // scale=1 result
result = lerp(res2, res0, scale);
```
This same trilinear blend is applied to: the bounding-box center/min/max (below), the per-vertex morph positions, and (for composite models) the `.bon` assembly offset.

### Body

Read in this exact order, immediately after the 36-byte header:

1. **`morphCenter`**: 8 × `vec3<float>` (96 bytes) — bounding-box center per morph corner.
2. **`morphMin`**: 8 × `vec3<float>` (96 bytes) — bounding-box min per morph corner.
3. **`morphMax`**: 8 × `vec3<float>` (96 bytes) — bounding-box max per morph corner.
4. **`radius`**: 8 × `float` (32 bytes) — bounding-sphere radius per morph corner (parsed but not otherwise used by `ei_maper`).
5. **Vertices**: `vertBlocks` blocks. Each block is 4 points × 8 morph corners × {x,y,z}, stored **axis-major** (all 8×4 x-values, then all 8×4 y-values, then all 8×4 z-values):
   ```
   for block in 0..vertBlocks:
     for axis in {x,y,z}:
       for morphCorner in 0..8:
         for point in 0..4:
           read float  →  vertex[morphCorner][block*4 + point][axis]
   ```
   Total vertex count = `vertBlocks * 4`; each vertex has 8 morph-corner positions.
6. **Normals**: `normalBlocks` blocks, same nesting but 4 components (x,y,z,w) instead of 3, and axis-major order swapped to component-major-then-point (see `readNormals`):
   ```
   for block in 0..normalBlocks:
     for component in {x,y,z,w}:
       for point in 0..4:
         read float  →  normal[block*4 + point][component]
   ```
   Normals are **not** morph-varying (one value per vertex, not per corner) — the `w` component's role is unconfirmed (possibly padding/handedness).
7. **UVs**: `uvCount` × `vec2<float>` (flat list, one per unique UV, referenced by index below).
8. **Indices**: `indexCount` × `uint16` — a flat triangle list (every 3 indices = 1 triangle) into the **vertex-component** array below, not directly into vertices/normals/UVs.
9. **Vertex components**: `vertexComponentCount` × `{normalIndex: uint16, vertexIndex: uint16, uvIndex: uint16}` — this is the actual "combined vertex" used for rendering: `finalVertex = {position: vertex[vertexIndex] (after morph blend), normal: normal[normalIndex], uv: uv[uvIndex]}`. Multiple vertex-components can share the same `vertexIndex` while differing in `uvIndex` (standard UV-seam duplication).

### Unread Trailer (Known Quirk)

Immediately after the vertex-component array, every `.fig` file (both standalone and `.mod`-embedded) has an additional trailing block of `morphingComponentCount` (`= 4 * vertBlocks`) `uint32` values (`morphingComponentCount * 4` bytes) that **`ei_maper`'s reader never reads** — its `CFigure::readData()` returns immediately after the vertex-component array, leaving this block unconsumed. Byte inspection shows its values loosely resemble a per-point counter of the form `i | (i << 16)` (i.e. the same small index repeated in both halves of the 32-bit word), but this pattern **drifts by a few integers in the last several entries** of at least one sampled file, so it is not a clean closed-form index — more likely a derived/computed value from whatever internal tool exported these files. Its purpose is unconfirmed. Treat it as an ignorable trailing block sized `4 * vertBlocks * 4` bytes when parsing (needed to correctly skip past one `.fig` blob to whatever follows it, e.g. the next entry in a `.mod` archive).

---

### Header fields: values (all 3564 vanilla FIG8 meshes, 2026-10-03)

The two last header words were called `group` and `textureNumber`; their values across the vanilla `figures.res`
(plain `.fig` files and the meshes inside `.mod` files) group by kind of model:

| `textureNumber` | Count | Used by |
|---|---|---|
| 0 | 1 | water (`stwe2.mod:water`) |
| 1 | 1545 | every unit and creature part, faces |
| 2 | 477 | armour items shown on the ground (`initarhl*`) |
| 3 | 6 | plants (`naflli*`, `naflte1`) |
| 4 | 58 | buildings (`stbuho1`...) |
| 6 | 6 | wings (`unmowi.mod` body parts) |
| 7 | 57 | stones, an aquarium (`nast*`, `stst3`) |
| 8 | 1414 | items and props (`initqi*`, `ingm*`, `co*`) |

Inside the unit models it splits the parts: 1 = drawn with the body's texture (the skin with the worn clothes
composed over it), 2 = drawn with an item's own texture: weapons, helmets, bow strings, the quiver and the arrows
(the bow's redress texture holds the quiver and the arrows). So it is not a "primary/secondary texture" switch but the kind of texture (probably which texture set or atlas
the game loads it from). The word before it takes 17-25 (0x11-0x19): 18 (1231), 22 (902), 21 (722, faces), 19
(618), 17 (64), 23 (20, eyes, gems), 25 (6, wings), 24 (1, water). Read as bits over 0x10: bit 3 (24, 25) is set
on the see-through meshes (water, wing membranes): probably the render flags (blending, two-sided...); the
other bits are not tied to anything yet.

### The older figure format (22 vanilla files, no `FIG8` signature)

`in25arrow`, `efcu0lightsourse`..`efcu5eye`, `initlitr1..6item` (no 4), `initqi4item`, `ingm1gipat01..08`, `nask0sky`. Decoded 2026-10-23 from the game's own reader
(`CRenderManager::LoadFigFile` 0x4F7D20 sends a file whose first three bytes are not `FIG` to `BuildFigureMesh2` 0x4F87D0; `FIG8` goes to `BuildFigureMesh` 0x4F7F00). All 22 files parse to
exactly their size with the layout below (`_cpr/claude-re/full/tools/oldfig.py` checks it). It is the `FIG8` mesh with **6 morph corners instead of 8** and a different header; the `.bon` is 72 bytes (6 x vec3) instead of 96.

| offset | size | what |
|---|---|---|
| 0 | u32 | render flags (17-23, as in `FIG8`'s word before the texture number) |
| 4 | u32 | texture number (3 or 8) |
| 8 | 244 B | per-corner bounds data (centre, min, max, radius per corner; not needed to draw) |
| 0xF8 | u32 | `n`: vertex blocks |
| 0xFC | n x 288 B | vertex blocks: 3 vec4 of the output vertex (position, ...), each blended from **6 vec4** (24 floats) of morph corners: with the three complection weights `a`, `b`, `c`: `lerp(lerp(lerp(v0,v1,a), v2, b), lerp(lerp(v3,v4,a), v5, b), c)` (exactly the game's 24-float expression; the fourth vec4 of the output is 1,1,1,1) |
| next | u32 `a`, a x 64 B | the "normals" array (64 bytes per element, as `FIG8`'s second block) |
| next | u32 `b`, b x 8 B | UVs (2 floats) |
| next | u32 `c`, c x 52 B | records the game skips (not given to the mesh builder; per-vertex morph components?) |
| next | u32 `t`, t x 6 B | u16 triples (vertex-component references) |
| next | u32 `q`, q x 4 B | 4-byte items (the `FIG8` "morphing component" array) |
| next | u32 `r`, r x 2 B | u16 indices (triangle list) |

The file ends right after the index array. `n` equals `a` in all 22 files (one normal element per vertex block). In the `FIG8` reader the same code path's non-`FIG8` branch (header words copied from a
`FIG8` shape) is dead for these files.

## `.bon` — Assembly Offsets

`.bon` files come in two shapes, matching whether the model is a simple single-mesh figure or a composite `.mod`:

### Simple form (plain `.bon` next to a plain `.fig`)

Exactly **96 bytes**: 8 × `vec3<float>`, one offset per complection morph corner (same 8-corner order as `.fig`'s morph arrays). This offset is trilinearly blended by complection exactly like the mesh data, then added to the figure's own position — but since a single monolithic mesh has no sibling parts to align, this offset is typically near-zero and has no visible effect for these simple figures.

In the vanilla `figures.res` (2026-10-03): 440 plain 96-byte `.bon`, 452 composite ones, and 22 of 72 bytes
(next to the older figures, see above). The composite ones hold 3124 entries of 96 bytes and 33 of 72. Nothing
else: a `.bon` holds no rotations and no hierarchy (the hierarchy is the `.mod`'s link table); the parts'
rotations come from the `.anm`. The skeleton the 3D Viewer draws (Skeleton checkbox) is the parts' joints
placed by these offsets (and the clip's rotations), each linked to its parent's.

### Composite form (`.bon` next to a `.mod`)

A full **RES archive** (see `res-format.md` for the container format), with one entry per body part, each entry's payload being the same 96-byte "8 × vec3" structure as the simple form above. Entry names match the part names inside the sibling `.mod` archive exactly (e.g. `hd`, `bd`, `hp`, `rh1`...). Each part's offset is added cumulatively down the hierarchy (a child's effective offset is its own offset plus its parent's, recursively — see `applyAssemblyOffset` in `figure.cpp`), positioning each rigid part relative to its parent bone.

### Resolved: "Oversized Head" Was a Part-Selection Bug, Not a Missing Scale

An earlier version of this viewer appeared to render heads (and other parts) at nonsensical scale, with disconnected floating fragments. This was **not** a missing scale factor anywhere in `.fig`/`.bon` — neither the header, vertex/normal/UV data, nor the `.bon` assembly offset (a pure position delta, not a scale) has any per-part scale field. The actual cause was rendering the wrong part set: composite models bundle every equipment/hair/weapon mesh variant as sibling entries (see "Body-Part-Name Convention for Equipment Variants" below), and an early hardcoded human-specific default-visibility list picked an inconsistent subset of these for non-human models, occasionally including an oversized equipment-fitted variant (e.g. one of `hd.armor01`-`hd.armor16`, plausibly modeled larger to fit under a helmet) instead of, or alongside, the plain base part. Rendering only the bare (non-dotted) canonical hierarchy slots — `hd`, `bd`, `hp`, `hr`\* excluded, `lh1-3`, `rh1-3`, `ll1-3`, `rl1-3`, etc. — produces correct, normally-proportioned geometry; confirmed both structurally (every composite `.mod` sampled has exactly one bare entry per real hierarchy slot) and visually (matches a reference Blender import of `unhuma` showing the same part set with normal head/limb proportions). See `IsEquipmentVariantOrDescendant` in the viewer's `main.cpp` for the generalized (non-hardcoded) selection rule.

---

## `.mod` — Composite Model (nested RES archive)

A `.mod` file is a RES archive (magic `0x019CE23C`, see `res-format.md`) containing:

- **One "link table" entry**, named identically to the model's own base name (e.g. a file `unmosk2.mod` contains an entry literally named `unmosk2`— and, in observed vanilla files, sometimes also a second entry under an older/alias base name, e.g. `unmosk`, with identical content; this appears to be an artifact of how the assets were originally exported and both entries decode the same way). Its payload is the **bone hierarchy**:
  ```
  int32 nLink                         // number of parts
  repeat nLink times:
    int32 childNameLength             // includes the trailing NUL
    char  childName[childNameLength]  // NUL-terminated
    int32 parentNameLength            // 0 if this part is the hierarchy root
    char  parentName[parentNameLength]  // present only if parentNameLength != 0
  ```
  Every `childName` here must also exist as its own top-level entry in the same `.mod` archive (see below). Example, decoded from `unmosk2.mod`'s `unmosk2` entry:
  ```
  hp (root)
   └─ bd
       ├─ hd
       ├─ lh1 → lh2 → lh3 → box23     (left arm chain, box23 = left-hand attach socket)
       └─ rh1 → rh2 → rh3 → box03     (right arm chain, box03 = right-hand attach socket)
   ├─ ll1 → ll2 → ll3                  (left leg chain)
   └─ rl1 → rl2 → rl3                  (right leg chain)
  ```
- **N part entries**, one per part name from the hierarchy above, each containing a raw `.fig`-format mesh (see above) for that part.

Some vanilla `.mod` archives additionally contain unrelated leftover entries (observed: a full embedded `ResBuild.exe` PE executable inside `unmosk2.mod`, and multiple unrelated composite objects' link-tables sharing one archive, e.g. `goldpile00.mod` also contains link-tables and part meshes for `jbr00`, `wall01`, `stwa1`/`stwa33`/`stwa34`, and `jprops00` — these appear to be a shared "ruin decoration" asset pool bundled into one physical archive and copied under each object's own filename by the original packaging pipeline). A loader should read only the entry matching the archive's own base name and its referenced parts, and ignore anything else present.

To load a composite model: parse the `.mod` archive → build the hierarchy from its link-table entry → parse each part's mesh from its own entry → parse the sibling `.bon` archive → attach each part's (hierarchy-accumulated) assembly offset to its mesh.

### Body-Part-Name Convention for Equipment Variants

**Hair** (read from game.exe 2026-10-25): a unit's hair is the `Graphics Data Hair` number of its Monsters row (heroes: of their prototype), a full int; the game builds the part name `hr.%.2d` from it (`CWorldClient::CreateHeroUnits`, `CUnitClientSpecific::Equip` / `Unequip`, `BuildArmorModelNames`) and -1 means no hair. Nothing limits the number to the vanilla 3 (male `hr.00-02`) or 4 (female `hr.01-04`): a new hair is a new `hr.NN` mesh in the `.mod`, an entry `hr.NN` with parent `hd` in the link table, and a `.bon` entry (the vanilla hairs' offsets are zero). Replacing the files of the vanilla numbers only swaps them. Lost in Astral's heads and hairs have the same mesh at all 8 complection corners (they do not follow the build); Universal Mod carries them with corners made from the vanilla head (`_cpr/lia-heads`): LiA's head as `hd`, its hairs as `hr.03-05` (male) and `hr.05-08` (female) beside the vanilla ones; all hairs are pushed outward where the denser LiA head would show through them (`_cpr/lia-heads/fixhair.py`).

**Portraits** (camp / party screen, 2026-10-26): each hair also needs portrait figures, `infa<model><build><hair + 1>face` (`.fig`, `.bon`, `.lnk` in `figures.res`; `CUIScreen740764::BuildUnitPortraits` 0x6407E0), where the build is `th` (complection x < 0.2), `fa` (x > 0.8) or `me`, e.g. `infaunhumame1face` = male, hair 0. Without one the portrait shows the flat face texture. A portrait is one part `face`: the game's head + hair merged, posed and scaled for the portrait camera (an affine map of the game head at a fixed complection: female exact, male within 2-5 %), all 8 corners equal, UVs = the game UVs + 0.39 in v (the `face<f|m><skin>` texture), header flags 18 / texture kind 8. The face textures only hold the hair strip where the vanilla hairs use it (u up to 0.265), so a portrait hair UV outside that shows white. Universal Mod's portraits for the LiA hairs are made by `_cpr/lia-heads/genface.py`.

The base human rigs (`unhuma`/`unhufe`) are far larger than the hierarchy above suggests — `unhuma.mod` has 348 entries for only 16 actual hierarchy slots (`hp`, `bd`, `hd`, `hr`, `lh1-3`, `rh1-3`, `ll1-3`, `rl1-3`). The link-table only ever references one canonical name per slot (e.g. `hr.00` is the hierarchy child of `hd`, not bare `hr`); the rest are **sibling variant meshes**, named `<part>.<variant>` (e.g. `hr.00`/`hr.01`/`hr.02` for hairstyles, `bd.armor01`...`bd.armor15` for torso armor pieces) or `<baseWeaponType><NN>` (e.g. `baseaxe00`...`baseaxe05`, `rh3.axe00`...`rh3.axe05` for axe meshes attached at the right-hand socket). The database's per-unit `OBJBODYPARTS` list (a `.mob`/gameplay-level field, distinct from this file's own hierarchy) uses the bare slot name (`hr`, not `hr.00`); resolving which numbered variant to actually display is a separate, not-yet-fully-reverse-engineered selection driven by the equipped item/material data, **not** a fixed 1:1 name match — a loader that only knows the hierarchy above needs that additional mapping to pick the right variant mesh for a given piece of equipment.

---

## `.anm` — Skeletal Animation

Which clip the game plays for which action and weapon, how likely, and its event frames (footsteps, hits, arrow
release) are in the model's `.adb` animation database: see `adb-format.md`.

Only present for true composite (`.mod`+`.bon`) rigs. An `.anm` file is a **RES archive of RES archives**:

1. **Outer RES archive**: one entry per animation clip, named by the clip's logical name (observed in vanilla data: `cidle`, `cwalk`, `crun`, `uattack01`, `uattack02`, `uattack03`, `uhit`, `udeath01`, `udeath02` — `c`-prefixed = continuous/looping locomotion, `u`-prefixed = one-shot action clips).
2. **Each clip is itself a RES archive**: one entry per bone/part, named identically to the part names in the corresponding `.mod`'s hierarchy (`hd`, `bd`, `hp`, `rh1`...`box03`, `box23`, etc. — all parts get a track, including the otherwise-static hand-attachment sockets).
3. **Each bone track** (checked on all 22,046 tracks of the 49 vanilla `.anm`: every byte accounted for):
   ```
   uint32 N                       // frames
   quat<float> rotations[N]       // (w, x, y, z), unit length
   uint32 N                       // the same count again
   vec3<float> positions[N]
   uint32 F, uint32 V             // per-vertex animation: F frames of V vertices (0, 0 for nearly every part)
   vec3<float> vertices[F * V]    // F = the clip's frames, V = the part's real vertex count (its .fig stores them
                                  // in blocks of 4: 44 slots, 42 vertices). Used by wing membranes (`r_pereponka` of
                                  // the dragons unmodg, unmosu) and a few bodies (unmori bd, unmomi rh1/lh1/bd,
                                  // unmowi bd01-04). They are DELTAS: the engine adds frame f's vertex i to the
                                  // part's rest vertex i (the complection-blended .fig vertex of that index, before
                                  // the part's rotation: CAnimationNode::UpdateTransform -> CFigure::SetMorphDeltas,
                                  // SubmitMorphedVertices). Checked on unmodg's r_pereponka (2026-10-20): rest +
                                  // frame is a smooth, evenly meshed wing (the frame alone is a crumpled sheet),
                                  // and a vertex the wing hinges on (rest (-0.18, 0.36, 0.99)) has delta 0. The
                                  // index is the .fig vertex index; the count V is less than the .fig's slot count
                                  // by its block padding, and the track's last vertices may be 0. (An earlier note
                                  // here read them as whole positions in another frame: wrong.) Applied by both
                                  // viewers (fig::MorphDelta).
   ```
   (An earlier reading took the second count and the two trailing words for "N + 1 positions": there are N.)

4. **Posing a part at frame f** (fitted on the data: 0.01 degrees of error on every rigid part of `unhuma`):
   - its rotation, chained child first (Direct3D's row-vector order): `W(part) = q(part) * W(parent)`, the root's
     being its own `q`;
   - its position: the root's (`hp`) is its track's position (the height above the ground and its motion); any other
     part's is `P(parent) + W(parent) applied to its rest offset`;
   - the rest offset is the part's `.bon` offset at complection corner 5 (the clips were made on that build), so
     the game can use the unit's own blended `.bon` offset instead and keep the clip on any build; the track's
     positions are those offsets already rotated (`W(parent) * offset`, constant length);
   - a vertex: `P(part) + W(part) applied to the vertex`.
   - Exceptions: in the run clips, `bd` and `rl1` carry the forward travel in their positions (lengths up to 3.6);
     the game moves units itself (the stride is in the `.adb`, see `adb-format.md`), so playing in place uses the
     rest offsets.

---

## Writing a `.fig` / `.bon` / link table (verified 2026-10-22)

`viewer/figure_write.hpp` writes all three; every rule was checked on the vanilla `figures.res`: all 3,563 non-empty FIG8 meshes, all 3,564
`.bon` entries and all 452 link tables are reproduced **byte for byte** from their parsed form (`tests/figwrite_test.cpp`; the two
exceptions are `nawa1` / `nawa2`, whose one name is stored without its NUL).

- **Bounds** (per corner): centre = the bounding box's centre, `min` / `max` = **minus / plus the half extents** (not the box's corners),
  radius = the half diagonal. (ei_figer writes the absolute corners instead; the game takes both, vanilla uses the half extents.)
- **Trailer** (`morphingComponentCount` words, = one per vertex point): point `p` holds `(p << 16) | min(p, usedPoints - 1)`, where
  `usedPoints` = the highest vertex index a component uses + 1. Exact on all meshes.
- **Normals**: `w` is always 1.0; one normal per vertex point (`normalIndex == vertexIndex` in every vanilla component); the arrays are
  padded to blocks of 4 with copies of existing points (the padding is never referenced).
- **Components** are sorted by vertex index, every (vertex, uv) pair is unique, and every component is used by a triangle.
- A mesh that stores a hard edge repeats the position once per normal (a flat-shaded cube has 24 points).

## Hand-held weapons: how they scale (measured 2026-10-22)

A weapon in the hand is a few parts under the unit's hand part; each part, like every `.fig`, holds its vertices at the 8 complection
corners. For the vanilla **crossbows** (`rh3.crbowNNmain`, `crbowNNpart01/02` the limbs, `crbowNNtetiva01/02` the strings) the 8 corners are
**exactly one mesh multiplied by one number per corner**, and the `.bon` pivots follow the same numbers (residual 0.000 on every part of
unhuma; unhufe's limbs and strings 0.0004, see below). The numbers (corner 5 = 1, the build the animation clips were made on) differ per race:

| corner | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| unhuma | 0.4953 | 0.6729 | 0.5998 | 0.6452 | 0.7678 | 1 | 0.9441 | 1.0164 |
| unhufe | 0.5019 | 0.6183 | 0.5646 | 0.6948 | 0.7911 | 1 | 1.1088 | 0.9776 |

(corner index as in the `.fig`: ei_figer's `base, str, dex, unique`, then the same four "scaled"). unhufe's corner 5 is 0.9444 of unhuma's.
The bows' corners are not exactly uniform (1-6 cm of hand shaping per axis). unhufe's crossbow **stock** has a hand-made lift of its rear
vertices (0.004-0.024 in height) at the big builds (corners 3, 4, 6), probably body clearance: not derived by the importer.
The orcs (`unorma` / `unorfe`) have no crossbows.

**Frames and the animation.** Only the `01` parts of a variant family carry animation tracks (`rh3.crbow01main`, `crbow01part01/02`,
`crbow01tetiva01/02`); every other variant is their child in the link table (`crbow04part01 <- crbow01part01`, ...), so a new variant needs a link
entry and a `.bon` entry and no `.anm` change. A variant's mesh is in the frame of its `01` ancestor: the main in the hand-part's frame shifted by
`rh3.crbow01main`'s offset; a limb in the frame of its hinge (`crbow01part01`'s offset in the main's frame: (0, -0.688, 0.012) at corner 5 of
unhuma); a string in the frame of its anchor on the limb (`crbow01tetiva01`'s offset: (0.547, 0.104, -0.069)). A variant's own `.bon` offset
only shifts its mesh (vanilla variants use a few cm), so a new part can have offset 0. In the shooting clips (`uattack14/15/16`) the limbs turn up to
26 deg and the strings 60 deg, **about the vertical axis**: an anchor that is off in height moves nothing, one that is off across does.

**Limbs that are turned over.** Five clips of unhufe (`udeath23-26`, `uhit13`) hold `crbow01part01` turned 180 degrees from frame 0 on, the shooting clips only turn the limbs about the vertical axis. A vanilla limb is centred on its hinge, so the turn is invisible; a limb whose root is higher than the hinge is thrown off the stock (6.6 cm measured on cb4). The importer lowers the limbs and strings together until the roots sit at the hinge's height (`prodAtHinge`).

**The ground / inventory figure** (`initwecbNweapon.fig` + `.bon` + `initwecbN.lnk`) is the item lying on the ground and in the inventory: one mesh of all the parts, the 8 corners identical, UVs through the 256 atlas. For the vanilla crossbows 1-3 it is an exact similarity of the hand parts at corner 5 (matched by UV, residual 0.00000): ground = 1.8642 * M * hand + t, M = (x, y, z) -> ((x + y) / sqrt 2, (x - y) / sqrt 2, -z) (turned 45 degrees about Z and upside down), t centring the box in z and across the diagonal and putting the hand-frame y of about the vertex mean (-0.54) at the origin along it. Crossbow 1 is reproduced point for point (all 67 within 8 mm after sliding the origin 4 cm), 2 and 3 were touched up by hand. The importer writes it for crossbows (`--no-ground` skips it); the other kinds turn by other angles (bow 04 and sword 02 differ) and are not derived yet. The mod's old unfinished `initwecb4weapon.fig` had negative v and texture number 4.

**Shape edits.** Everything but the corner-5 mesh is derived, so reshaping that mesh before the corners are built reaches every build and both races (`ShapeEdit`: scale a piece's cross-section about its own centre line over a stretch of its length, after cutting the piece across the stretch because grips and limbs have no vertices there). The placement is computed from the model as modelled, so an edit never moves anything else.

**Blueprint textures** (`textures.res`, `cb_NN.0`) are the item's coloured texture desaturated to a flat grey: same 128 x 128 layout and footprint, about `0.51 x luminance + 96` per channel with a faint warm tint (chroma <= 14), transparent outside the islands. They are drawn with the ground / inventory figure (`initwecbNN...`), whose UVs go through the 256 atlas.

**UVs and the texture.** A hand-held part's UVs are *direct* texture coordinates: u across the image, v down from its top row (measured through the viewer with a
texture whose every texel encodes its own position: 97% of the texels the vanilla crossbow samples are `(u*W, v*H)`; the "256 atlas" mapping is for ground / inventory
models, which is why ei_figer converts the UVs of `init...` figures and leaves in-hand parts alone). The same texture also paints the **quiver and the arrows** the unit
carries with a ranged weapon; they sample columns 42-83 of the lower half (rows 63-127) of a 128 px texture. The vanilla crossbow textures are transparent there, so a
crossbow shows no quiver; a new crossbow texture must keep that band transparent, or a quiver appears on the back. The same goes for the **helmets**: a helmet's face part samples the crossbow's texture too (rows 0-49, columns 55-84 for the helm of the test hero; measured in the real game with a texture whose texels encode their position): painted there, the helmet's face turns the texture's colour. Every vanilla crossbow texture keeps ALL its art in columns 0-41 (11 of 11 textures) and is transparent elsewhere, which is the safe rule: art only in columns 0-41. Textures are 128 x 128 uncompressed 32-bit ARGB, transparent
outside the islands (`redress.res` holds them as `.mmp`: `um-multitool ddsmmp`); the bone variants (`bn bd bb bs bg bc br`) differ only in the bone's tint, the leather / dark
parts (58, 46, 31) are shared; male and female use the same pixels.

**The crossbow importer** (`viewer/weapon_import.hpp`, `um-multitool viewer --weapon-import`, 3D Viewer > Units > "Weapon import"): an OBJ of 5 pieces, placed on
the hinge and the string anchors (scale from the strings' outer ends, tilt of the reference stock, height from the stock at the hand), the 8 corners
built from the race's measured scales, parts + link entries + `.bon` entries added to a copy of the rig's `.mod` / `.bon`. Checked (`tests/weapon_import_test.cpp`,
`tests/weapon_anim_test.cpp`): rebuilt vanilla crossbows equal the originals at all 8 corners to 1e-6 (unhuma); the fit puts vanilla models back within 6 mm;
the fit does not depend on how the model is turned, moved or scaled; no anchor drifts more than vanilla's over every clip at every build; the
real game draws the result (both rigs, no crash, 2026-10-22); ei_figer's own reader reads the files.

## Hand-held weapons: bows and melee weapons (measured 2026-10-22)

The importer (`viewer/weapon_import.hpp`) takes one weapon **kind** per import: `crossbow`, `bow`, `sword`, `dagger`, `axe`, `club` (the "hammer" items), `pike` (the "spear" items). A kind says which parts the weapon is made of, where they hang, how the 8 corners are built and where the hand holds it.

| kind | parts of variant NN (unhuma and unhufe alike) | hangs under | animated ancestor |
|---|---|---|---|
| crossbow | `rh3.crbowNNmain`, `crbowNNpart01/02`, `crbowNNtetiva01/02` | the `01` parts | `01` |
| bow | `lh3.bwpartbNN` (the +Y limb half), `bwpartaNN` (-Y), `bwtetivabNN`, `bwtetivaaNN` (a string half on each) | `lh3.bwpartb00`, `bwparta00`, `bwtetivab00`, `bwtetivaa00` | `00` |
| sword, dagger | `rh3.swordNN` / `rh3.daggerNN`, and `baseswordNN` / `basedaggerNN` | `rh3.sword00` (the daggers too) | `00` |
| axe, club | `rh3.axeNN` / `rh3.clubNN`, `baseaxeNN` / `baseclubNN` | `rh3.axe00` (the clubs too) | `00` |
| pike | `rh3.pikeNN`, `basepikeNN` | `rh3.pike00` | `00` |

The `baseNNN` part is a 0.06 m cube a few cm short of the tip (the weapon's "bash" mesh: `dress.hpp` never draws it); the importer copies the base game's and sets its `.bon` offset.

**Bows.** The two limb halves are in the frame of `lh3.bwpartb00` (`bwparta00`'s own offset is 0): the half at +Y is `partb`, the one at -Y `parta`, meeting at y = 0; a string half is in the frame of its tip anchor (`bwtetivaa00`: (-0.106, -0.804, 0.004), `bwtetivab00`: (-0.119, 0.802, -0.001) at corner 5 of unhuma, 1.6065 apart; the vanilla variants' anchors differ by 1 cm) and runs toward the middle. The string is on the -X side, the limbs curve toward it, the hand is at (-0.07, 0.099, 0.017) in the frame. The `00` parts are one mesh times one number per corner (residual 0.005); the variants 01-05 are shaped by hand (1-6 cm off a single number per corner):

| corner | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| unhuma | 0.5893 | 0.6791 | 0.5612 | 0.5560 | 0.8999 | 1 | 0.9000 | 1 |
| unhufe | 0.5490 | 0.5708 | 0.4992 | 0.5637 | 0.8186 | 1 | 1.0289 | 0.9345 |

**The grip must be on the pivot.** The limbs turn about the frame's origin and the strings about their anchors, 0.113 further out in X (unhuma; 0.127 unhufe). Every vanilla bow has its two limb halves joined on that origin (the join's middle at x = -0.004 to 0.027, y and z within 1 cm) and its string ends on the anchors. A model with another depth (string to grip) cannot have both: a sample bow 0.18 deep put its string ends on the anchors and the limb halves then came apart by 4.3 cm at the grip in the draw clips, the hand 15 cm from the limbs (vanilla: 0.1 cm and 10 cm). The importer puts the grip on the pivot and bends the limbs (the tips and the strings moved in X by the difference, the limbs by (y / tip)^2 of it, normals recomputed): the same sample then stays within 0.3 cm of the vanilla bows' drift over every frame; `keep depth` / `--keep-depth` keeps the model's own.

unhufe's bow is 1.145 times unhuma's at corner 5 (the arrows have the same numbers). `uattack01-03` (60, 56, 77 frames) are the clips that move the strings; the limb halves stay joined (0.001-0.004 m) and the strings hang on the limb tips (0.005-0.008 m) in every frame at every build, the same as the vanilla bows.

**Melee weapons.** One rigid part in the frame of its family's `00` part, blade (or head) toward -Y, the hand's origin about 0.15 from the butt end; it follows the hand part `rh3`. Each vanilla variant is, per corner, an **affine copy** of its corner-5 mesh (a scale per axis and a shift along Y, residual 0.0000), but the numbers differ from one variant to the next (sword01 is 0.589 uniform at corner 0, axe03 is (0.498, 0.570, 0.380), a pike 0.604): there is no single law, so the importer averages the family's variants (one number per corner, as the bows and crossbows have; the largest remaining distance from a variant's own corners is 3-6 cm, 10 cm on a 2.8 m spear):

| corner | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| sword + dagger, unhuma | 0.5819 | 0.6459 | 0.5546 | 0.7053 | 0.8762 | 1 | 0.9779 | 0.9825 |
| sword + dagger, unhufe | 0.5575 | 0.5705 | 0.4989 | 0.5749 | 0.6906 | 1 | 1.0472 | 0.9320 |
| axe + club, unhuma | 0.5749 | 0.6941 | 0.5327 | 0.6024 | 0.8174 | 1 | 0.8882 | 0.9864 |
| axe + club, unhufe | 0.5502 | 0.5798 | 0.5133 | 0.5100 | 0.6782 | 1 | 0.9848 | 0.8936 |
| pike, unhuma | 0.5993 | 0.6875 | 0.5790 | 0.5796 | 0.9293 | 1 | 0.9213 | 1.0348 |
| pike, unhufe | 0.5729 | 0.5855 | 0.4952 | 0.5778 | 0.7007 | 1 | 1.0644 | 0.9816 |

(measured on the base game's rigs, whatever a mod adds: the importer reads the lowest figure layer for this; `--reference NN` copies one variant instead, from the rig as it is). unhufe's weapons are bigger at corner 5, by 1.126 (sword), 1.078 (dagger), 1.135 (axe), 1.123 (club), 1.157 (pike) on average: a new weapon is made that much bigger there.

**Where the hand holds it** (the handle's centre line through the hand, from the vanilla weapons): the hand is 0.152 (sword), 0.142 (dagger), 0.19 (axe), 0.153 (club) from the butt end (times the ratio above in unhufe), a pike at 41% of its length (the halberd `pike06` at 48%); the handle's centre line is at (x, z) = (0, 0) (sword), (0.004, 0) (dagger), (-0.048, -0.013) (axe, club), (-0.003, 0.008) (pike) in unhuma, (0.001, 0), (0.008, -0.002), (-0.026, -0.010) / (-0.024, -0.005), (-0.001, 0) in unhufe; the axes' and clubs' handles point 1.3 degrees above level in unhuma, level in unhufe. The head of an axe or club points to -X.

**Which end is the tip** is not in the shape alone (a sword's guard is as heavy as its point, a spear's pole as long as its shaft). Two rules, each right on all 31 vanilla weapons of its kinds: a sword or dagger ends in a point, so the end whose cross-section 4% in from it is smaller is the tip; an axe, club or spear has its cross-sections' weight (their size along the length, as a mass) toward the head. `flip` reverses the result, `roll` turns the weapon about its length.

The mesh's first header word (render flags, 18 or 19 on the melee weapons: bit 0 differs, not understood) is taken from the family's most common value, or `group=`.

**The Universal Mod's own `rh3.sword06` and `rh3.dagger05`** (not in the base game: the items "stone battle sword" and "bone dagger") hold the same mesh at all 8 corners in both races: they do not scale with the build. `viewer --weapon-export-ref <race> 06 out.obj --kind sword` and `--weapon-import out.obj --kind sword --number 6 --replace --no-fit` rebuild one with the corner scales above.

**Textures.** Every vanilla human texture of the melee weapons, the crossbows and the spears keeps its art in columns 0-41 of the 128 px image (the 12 spear textures have one stray texel). The bows are the exception that confirms why: all 26 hold the quiver and the arrows in rows 64-127, columns 42-83 (1612-1657 texels), and a new bow texture without them shows no quiver. A new bow's texture must copy that band from a vanilla bow's, and leave the helmet area (rows 0-49, columns 55-84) empty.

**Tests** (`tests/weapon_kinds_test.cpp`, `tests/weapon_anim_test.cpp --kind K`): every vanilla bow and melee weapon of both rigs is exported, turned about the vertical axis, moved and scaled, and imported with the automatic fit: it lands within 1-2 cm (bows), 3-7 cm (melee weapons, the spread of the vanilla hand), the same result whatever the model's own turn, place or scale (1e-6), a bow modelled standing up and a melee weapon lying down or end for end come out the same; a shape edit thins a stretch by its factor and moves nothing else. The animation test measures every frame of all 170-179 clips at the 8 corners and random builds: the bows' strings, limb tips, limb join and the hand's distance to the limbs, the melee weapons' hand-to-tip, hand-to-butt and hand-to-handle distances all stay within the vanilla weapons' own variation.

**LiA's weapons in Universal Mod** (2026-10-26, `_cpr/lia-weapons/build.py`): the sickle (`sword07`), the royal sword (`sword08`), the guard
dagger (`dagger06`) and LiA's bone crossbow (`crbow04`; the mod's own crossbow 04 became 05) went through the importer: the melee ones
with LiA's placement (`--no-fit`; LiA's meshes are one mesh at all 8 corners) and, for unhufe, the vanilla female/male ratio (x1.126 sword,
x1.078 dagger, + 1 cm along the blade; LiA's female dagger06 is the male mesh unchanged), the crossbow with the auto fit (scale 1.022 /
0.966, strings on the anchors). LiA stores its UVs with v negative (the hand wraps them): they are put in 0..1 first, or the ground
figure's atlas mapping misses its quadrant (an invisible inventory icon).

**Melee ground / inventory figures** (measured on the vanilla swords and daggers): the corner-5 hand mesh lying flat on the 45 degree
diagonal, centred, all 8 corners equal, UV = (u / 2, 0.5 + v / 2) (the 128 px texture in the bottom-left quadrant of the 256 atlas,
exact on every vanilla one), `.bon` 96 zero bytes, `.lnk` "weapon". Swords: turned 45 degrees about Z, daggers: turned and upside down.
The vanilla models are touched up by hand (an affine fit leaves 6-15 cm); their size grows with the weapon: ground length about
1.55 + 0.9 x the hand length (swords), 0.2 + 3.3 x (daggers). Helmets (`initarhlNNarmor`, `.lnk` "armor"): the same UV mapping, the
helmet on its back (up -> +Y), about 2.5 x.

**Weapon texture columns.** Every kind-2 item part samples one composed item picture: the weapon's art in columns 0-41, the helmet in
columns 42-83 of the top half, the quiver below it. LiA's sword08 painted up to column 59; its texture is squeezed into 0-41 (its u x 0.7).

**Metal variants.** The vanilla metal textures of an item share its layout, and its metal surfaces follow one palette per metal (bronze
yellow-olive, iron copper-brown, steel grey, mithril pale blue, adamantium lilac, meteoric dark blue, alloy near black; leather, cloth,
wood and gems stay; some variants are also repainted). `_cpr/lia-weapons/metals.py` learns brightness -> colour per metal from them and
recolours the metal texels of LiA's one painted metal. Blueprints (`textures.res`, `<code>_NN.0`): G = 0.5 x brightness + 96, R = G + 1,
B = G - 13, the item's footprint.

**pike00, the barbed pike** (2026-10-26, `_cpr/lia-weapons/pike.py`): vanilla's `rh3.pike00` (a 2.84 m pike with a barbed head, both
races, authored at all 8 corners) is the spear family's animated ancestor and is never drawn; its copy `rh3.pike08` (+ `basepike08`,
offset 0 under `rh3.pike00`) is the item "barbed pike" (TTI 8; 07 is the mod's `initwesp7` ground figure). pike00 maps its whole length
into columns 17-25 of the `sp_03` layout, where only the head is painted (v 0.84-1): `sp_08` = each metal's `sp_03` with its shaft
column (32-40) resampled into that strip above the head. Ground figures of the pikes: turned 45 degrees and upside down, length about
0.87 + 0.825 x the hand length.

**Crossbow fit fixes** (2026-10-26). LiA's crossbow 04: in LiA's rig the stock ends short of the bow and the limbs float in front of
the bone knot (clipping it); its own ground figure has the bow 4.3 cm behind the knot, which the port now follows (`build.py`
`lia_crossbow_obj`, `--no-fit`). The mod's crossbow 05 is the "hunter crossbow" (the "ingos crossbow"
until 2026-10-27; a metal item sold on Ingos, Shop 3; database row kept in place, text keys `Hunter_Crossbow`): the user's reshaped
`cb4.blend`, re-imported (`blender-models/weapons-um/cb5-hunter/`), its texture painted from the 3D shape (`cb5paint.py`: wood stock,
the prod and fittings in the item's metal). Its islands are grown 4 texels, colour and alpha, inside columns 0-41: the old texture
(crossbow 02 copied by nearest point, `cb5tex.py`) had alpha 0 right at the edges of 1-2 texel islands, and the game's filtering and
mip levels frayed the strings and the stock's edges into holes.

**A crown (helm 18) that keeps the head and the hair.** A helm slot changes `hd` -> `hd.armorNN` and the hair `hr.NN` -> `hr.NN.armorNN`
(`BuildArmorModelNames` 0x535CB0, the part list at 0x73EC24: `hr`, `hd`); a missing `hr.NN.armorNN` hides the hair (every vanilla helm:
none exists). A kind-2 helm mesh is drawn with the helm's own texture (`hl_NN.<metal>.<TTI2>`), the head stays: LiA's crown
(`hd.armor18`, kind 1 with UVs in the ground convention: on the body texture it showed skin) is a kind-2 crown with UV (2u, 2v - 1),
put on the mod's head per corner. **The crown sits on top of the hair** (2026-10-28, `_cpr/helmets/crowntop.py`; the earlier squeezes of
the hair under a tight crown showed the scalp in the game: the crown's inner ring was up to 12 mm inside the vanilla female head at the
front). Per complection corner the crown (two rings 1.5-3 cm apart, tilted, its bottom edge up to 3 cm lower at the back; LiA's flat lid
across the ring, hidden in LiA's skull, dropped) is widened about its vertical axis until its inner ring clears the head by 15 mm
(female) / 8 mm (male). Every hair has an `hr.NN.armor18` copy fitted to that ring (`crowncap.py`): cut along the band's bottom edge and
12 mm lower per corner, hair at the band's height brought to the inner ring - 2 mm from both sides (thick hair in, thin hair out), the
flare below fading over 12 mm; the hair inside the ring and over the band (tops, buns, top knots, ponytails) kept unless it cuts the
crown; under the band a cap (the head's own surface lifted 1.5 mm, the hair's texture) so the scalp can never show. Checked in the game
(the View screen, 8 turns, with and without the crown): every hair of both races at its own build and the extreme corners. Helmet 02 (`hd.armor02`, male): the vanilla head shows through its back at wide builds (up to
12 mm); its existing points were pushed out locally (`_cpr/helmets/helmback.py`, no new points).

## `.lnk` — Attachment Socket Redirect

A tiny fixed-format file used for weapon/item attachment points (their filenames match the hand-socket part names `box03`/`box23` seen above by convention, e.g. a unit template referring to a "weapon" or "item" slot):

```
uint32 tag              // observed constant 1
uint32 suffixLength     // includes trailing NUL
char   suffix[suffixLength]  // NUL-terminated ASCII, e.g. "item", "weapon"
uint32 zero             // observed constant 0 (reserved/padding)
```

**Resolution rule**: given a `.lnk` file named `X.lnk` containing suffix `S`, the engine loads the figure `(X + S)` — e.g. `initlimt0.lnk` (suffix `item`) redirects to `initlimt0item.bon`/`.fig`, and `initwecb4.lnk` (suffix `weapon`) redirects to `initwecb4weapon.bon`/`.fig`.

Because the suffix string is always one of a small fixed set of socket-type words (`item`, `weapon`, ...) shared by many unrelated `.lnk` files, files with the same suffix are **byte-identical** — this is the source of the "many `.lnk` files share the same checksum" observation: it's not a bug, the base filename that determines the actual redirect target is never stored inside the file at all, only implied by the `.lnk` file's own name on disk.

**Known anomaly**: one vanilla file, `initwesp7.lnk`, deviates from the rule — its stored suffix is the *entire* target name `initwesp7weapon` rather than just the suffix `weapon`. Naively applying the append rule to this file produces `initwesp7initwesp7weapon`, which does not exist on disk. This looks like a one-off data-entry mistake in the original vanilla assets (comparable to the two known `DBEditor` bugs documented in `database-format.md`), not a variant encoding — a robust loader should fall back to trying the stored string as an absolute target name if the appended name doesn't resolve.

---

## Texture Assignment

Figures do not store their own texture filename. Per `ei_maper`'s `CWorldObj`/`worldobj.cpp`, a placed unit's primary/secondary texture names come from its map-object properties (`OBJ_PRIM_TXTR` / `OBJ_SEC_TXTR` in `.mob` files, or the `Units` database block's texture fields — see `database-format.md`), and the actual image is loaded as `<textureName>.mmp` from the textures resource (see `mmp-format.md`). UV coordinates in `.fig` already address that texture directly; there is no per-figure texture reference to resolve beyond this name lookup.
