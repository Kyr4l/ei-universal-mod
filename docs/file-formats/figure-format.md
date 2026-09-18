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
int32   unknown            // observed 0 in every sampled file
int32   group              // presumed material/atlas group id; unconfirmed, safe to ignore for single-texture rendering
int32   textureNumber      // presumed primary(0)/secondary(1) texture-slot selector; unconfirmed
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

## `.bon` — Assembly Offsets

`.bon` files come in two shapes, matching whether the model is a simple single-mesh figure or a composite `.mod`:

### Simple form (plain `.bon` next to a plain `.fig`)

Exactly **96 bytes**: 8 × `vec3<float>`, one offset per complection morph corner (same 8-corner order as `.fig`'s morph arrays). This offset is trilinearly blended by complection exactly like the mesh data, then added to the figure's own position — but since a single monolithic mesh has no sibling parts to align, this offset is typically near-zero and has no visible effect for these simple figures.

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

The base human rigs (`unhuma`/`unhufe`) are far larger than the hierarchy above suggests — `unhuma.mod` has 348 entries for only 16 actual hierarchy slots (`hp`, `bd`, `hd`, `hr`, `lh1-3`, `rh1-3`, `ll1-3`, `rl1-3`). The link-table only ever references one canonical name per slot (e.g. `hr.00` is the hierarchy child of `hd`, not bare `hr`); the rest are **sibling variant meshes**, named `<part>.<variant>` (e.g. `hr.00`/`hr.01`/`hr.02` for hairstyles, `bd.armor01`...`bd.armor15` for torso armor pieces) or `<baseWeaponType><NN>` (e.g. `baseaxe00`...`baseaxe05`, `rh3.axe00`...`rh3.axe05` for axe meshes attached at the right-hand socket). The database's per-unit `OBJBODYPARTS` list (a `.mob`/gameplay-level field, distinct from this file's own hierarchy) uses the bare slot name (`hr`, not `hr.00`); resolving which numbered variant to actually display is a separate, not-yet-fully-reverse-engineered selection driven by the equipped item/material data, **not** a fixed 1:1 name match — a loader that only knows the hierarchy above needs that additional mapping to pick the right variant mesh for a given piece of equipment.

---

## `.anm` — Skeletal Animation

Only present for true composite (`.mod`+`.bon`) rigs. An `.anm` file is a **RES archive of RES archives**:

1. **Outer RES archive**: one entry per animation clip, named by the clip's logical name (observed in vanilla data: `cidle`, `cwalk`, `crun`, `uattack01`, `uattack02`, `uattack03`, `uhit`, `udeath01`, `udeath02` — `c`-prefixed = continuous/looping locomotion, `u`-prefixed = one-shot action clips).
2. **Each clip is itself a RES archive**: one entry per bone/part, named identically to the part names in the corresponding `.mod`'s hierarchy (`hd`, `bd`, `hp`, `rh1`...`box03`, `box23`, etc. — all parts get a track, including the otherwise-static hand-attachment sockets).
3. **Each bone track**:
   ```
   uint32 frameCount              // N
   quat<float> rotations[N]       // component order (w, x, y, z) — verified unit-length
   vec3<float> positions[N + 1]   // NOTE: one MORE entry than the rotation track
   ```
   The rotation-vs-position count mismatch (`N` vs `N+1`) is consistent across every sampled clip/bone/model (frame counts observed: 11, 25, 27, 66 — always exactly `rotationCount + 1` positions). The purpose of the extra trailing position sample is not confirmed; in at least one sample (`cidle`/`bd`) it does not continue the smooth per-frame motion curve of the preceding samples (its Y/Z components drop to exactly `0.0`), suggesting it may be a distinct terminator/auxiliary value rather than a genuine extra keyframe. A conservative renderer should use `positions[frame]` paired with `rotations[frame]` for `frame in 0..N-1` and can ignore `positions[N]`.

   To pose a bone at a given animation time: pick (or interpolate/slerp between) the surrounding frame(s), combine `rotation` and `position` into a local transform, and concatenate down the hierarchy from the root exactly as for `.bon` assembly offsets — animation replaces the static per-part offset while playing, it does not add to it.

---

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
