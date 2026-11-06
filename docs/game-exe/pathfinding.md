# Movement and pathfinding in game.exe (CAIMap, path finders, CMover, MoveCurve)

Written 2026-10-25 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (module 0x5AD000-0x5C8000 plus the
callers in CUnit 0x52E000, CUnitServer 0x545000, the AI 0x5C8000-0x5D8000 and the net handlers 0x66xxxx). It goes deeper than
`docs/pathfinding.md` (the 2026-10-20 overview) and corrects it in four places (see "Corrections" at the end). The navmesh that is
*stored* in a zone's `.mob` is `docs/file-formats/aigraph.md`; how it is *generated* is `_cpr/claude-re/navmesh/NAVMESH-RE.md`
(verified again here where it overlaps).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs in game.exe; offsets are bytes. "tile" = one 0.5 m cell of the AI grid; "nav cell" = 8 x 8 tiles = 4 m.

## How to re-implement it (short version)

1. **Grids** (server only, `CAIMap` embedded in `CWorldServer` at +0x170, global 0x7B00D0). Per 0.5 m tile: a 9-byte terrain cell
   `{u32 passability (8 nibbles, one per unit class), u16 height, u16 water-depth/flags, u8 "standing-unit" level}`, a u32 flags cell
   (material + object volumes), a u8 "moving-unit" level, and a scratch u8. Passability comes from height steps, slope, water depth,
   material cost and object volumes, then is *eroded* by the class's footprint (1 / 2 / 3 tiles) (0x5B8FA0). Objects and spells
   change it at run time through a dirty rectangle.
2. **Units are stamped**, not looked up: every unit writes a 16 x 16 byte "distance-to-edge" stamp of its radius into one of two
   byte grids (standing units -> the terrain cell's byte +8, walking units -> the obstacle grid), `level = (r + 2 - d/2) * 16`. A unit
   of radius r fits where the level is <= `(2 - r) * 16`. That is the whole unit-vs-unit collision.
3. **Search = Dijkstra, no heuristic, at two levels.** Tile level: 8-neighbour Dijkstra in a **32 x 32 window** (34 x 34 nodes with a closed
   border), step cost `((slope(dh) * cost[nibble]) >> 10) * (1024 | 1448) >> 10`, bidirectional for short hauls (< 25 tiles).
   Nav level (the AIGRAPH from the `.mob`): bidirectional Dijkstra over 4 m cells, 8 layers (classes), 8 stored neighbour costs. A long
   path = nav path -> each nav cell's representative tile as a waypoint -> sliding 32 x 32 window from waypoint to waypoint, back-links
   written into a global direction grid, cell list read back from it.
4. **The result is a list of tile cells**, delivered in chunks of at most 32 (`MoveRequest` kind 2, replicated). Each cell carries a
   speed factor (512 = 1.0, from the material nibble and slope). No smoothing of the cell list except a turn-penalised re-plan of the first
   cells (0x5C46C0).
5. **Movement is a pure function of time**: the cell list is turned into runs -> segments (position, unit direction, speed, cell) ->
   one Hermite cubic per segment, parameter = distance (0x5C67D0, 0x5C6FC0). Position at world tick t is `Evaluate(t - startTick)`.
   The server does not integrate; each tick it evaluates the curve, tests the segment's cell against the stamps and moves the unit.
   Clients receive only the `MoveRequest` (start pos, heading, start tick, cells + speed factors) and rebuild the same curve.
6. **Blocked?** The level the blocker was found at decides: standing blocker (terrain level) -> re-path (stamping the walking neighbours
   as static); walking blocker (obstacle level) -> wait, unless we are faster -> re-path. Follow-chain deadlocks are broken explicitly.

## 1. Conventions

| Thing | Value |
|---|---|
| world position | floats `CUnit +0x1C/+0x20/+0x24` = x, y, z in metres |
| tile index | `round(pos * 2 - 0.5)` in x and y [V] (`CAIMap::IsUnitCellPassable` 0x5BA6E0 etc.); tile (i, j) covers metres [i/2, i/2 + 0.5) |
| path / mover coordinates | metres * 2 (floats, "tile units"): cell centre = (i + 0.5, j + 0.5); conversion back `* 0.5` (TickMovement) [V] |
| map size | `width = sectors * 64`, `height likewise` at `CAIMap +0x26A0 / +0x26A4`; map sector = 32 m = 64 tiles. The **collision-map** sectors are smaller: `Resize(w >> 5, h >> 5)`, 32 tiles = 16 m [V] |
| world tick | `CWorldServer +0x64` (`DAT_007AFFC4`), 55 ms at normal speed (docs/game-memory.md); all movement times are ticks (ints, evaluated as floats) |
| direction codes | 1 N (y-1), 2 NW, 3 W, 4 SW, 5 S (y+1), 6 SE, 7 E, 8 NE (x+1,y-1) [V] (`Cell_Neighbour` 0x5C5CC0, `PathFinder::ExtractPath`, table 0x791988). The code stored in a node is the way **back** to its parent. In an AIGRAPH file the same eight directions are 0..7 (code - 1) |
| facing | `heading = atan2(dir.y, dir.x)` of the facing vector; the game takes `dir = -QuatForwardAxis(CUnit +0x34)` [V] |
| unit class | `CUnit +0x324` (0..7) = which nibble of the passability dword and which AIGRAPH layer the unit uses |
| radius | `CUnit +0x32C` (collision radius), `+0x328` = radius + 0.05 |

## 2. The call chain, from a click to the curve

```
client UI click                                   (client sends the "move to" message; the unit ids + target point + flag)
 CPlayerServer::OnMsg_MoveTo 0x66FBD0             reads ids, target, flag; Formation_ComputeOffsets 0x66FD70
  (WorldServer::OnMoveOrder 0x66FFF0 -> WorldServer::OrderMoveTo 0x5D39E0   same for the other message; script/Order_MoveTo 0x5D7AE0 calls QueueOrderMoveTo directly)
  AiLogicPlayer::MoveTo 0x5D3AE0   (per unit)     target += offset if Brain::CanWalkBetween 0x5D37E0 says the offset point is reachable
   some follower of this unit farther than FollowMaxDist*2+1 = 9 m (Brain::IsNearLeader 0x5D4860) and the player is not in mode 2
                      -> CUnitServer::MoveToWithParty 0x554830  (only a reachability check + report; the destination is parked in the brain, state 2,
                         and AiLogicPlayer::Update 0x5D4A90 queues the real order once the followers are within FollowMaxDist*1.4+1 = 6.6 m)
   else               -> CUnitServer::QueueOrderMoveTo 0x554740   queuedOrder (+0x1B4) = kind 1, pos, z = max path length (1e6 !)
 CUnitServer::Think / TickState -> DispatchQueuedOrder 0x54A900, case 1:
   stands up if sitting; arrival pre-test |unit - orderPos|^2 < 0.02 -> order done;
   CollectPartyFollowers 0x5468D0; CUnit+0x43C = IsCellWater(unit); CUnitServer::BeginMoveTo 0x548700
    CMover::Reset; CAIMap::FindPath 0x5BB350  (unit, goal, &moveState = CUnitServer +0x3C, maxLen, 0)   <- THE PATH
     FindFreeCellNear 0x5BAC10 -> short: PathFinder x2 + MeetLocalSearches 0x5B9690 | long: FindLongPath 0x5B9C90
     SmoothPathStart 0x5C46C0 (corridor re-plan) -> DeliverPathChunk 0x5BB190 (first <= 32 cells -> MoveRequest kind 2)
    CUnit::ResetMover 0x52E9D0 -> CMover::BuildPath 0x5C67D0 (MoveCurve at CUnit +0x27C), MarkNetDirty(10) = replicate
    state = 0 (walking), move anim set
 every tick: CUnitServer::TickState 0x5499F0 -> TickMovement 0x547890
    Mover::Predict 0x5C7430 -> CMover::Evaluate 0x5C6FC0 -> position, direction, speed, cell
    CanUnitMoveTo / FindOverlappingUnit / CollisionMap::MoveUnit / SetYawFromDir
    path end -> ContinueMove 0x5BB900 -> StepAlongPath (next chunk) -> ResetMover ... or arrival: Reset + SetRect(stand), state 6
```

Walk / run / sneak / crawl are **not** part of the order: the speed is the unit's *stance* (CUnit +0x264: 0 crawl, 1 sneak, 2 walk, 3 run,
4 sit), set by separate stance messages. `CUnit::GetSpeeds` 0x52F920 reads the stance's speed byte. `UpdateStance` 0x548520 (when it
applies a stance / sneak change) and `CUnitServer::v22_RecomputeStats` 0x553500 (stats changed) set `CUnitServer +0x146`; `TickState` (state 0) then calls `BeginMoveTo` again with the stored goal (`+0x9C`) and max
length (`+0xA4`): **a stance or speed change re-plans the whole path** (class, speed and cost tables change). [V]

The `z` of the order position is **not a height**: `QueueOrderMoveTo(pos, flag, run, z)` stores `z` at `queuedOrder +0x14`, `DispatchQueuedOrder` passes
`orderPos[2]` as `BeginMoveTo(goal, speed)` and that parameter is `CAIMap::FindPath`'s `maxLength` (all callers use 1e6; `Brain::CanWalkBetween` uses 2 x distance,
`FindRandomReachableSpot` 3 x distance + 10). It is stored in the mover at +0x68 (CUnitServer +0xA4). [V] (docs/game-memory.md calls it "z = 1000000 = ground".)

Order kinds that move (CUnitServer +0x190 / queued +0x1B4): 1 = walk to a point; **2 = turn in place to face a direction** (`CUnitServer::QueueOrderDirectMove` 0x5546B0, pos = direction
vector, `+0x1D0` = turn speed; `DispatchQueuedOrder` case 2 -> `CMover::ResetWithTarget` = request kind 1; the old name "direct move" is wrong); 3..6 attack / cast /
interact -> `ExecuteCombatOrder`, `ApproachTarget` 0x54D140 (chase, below); 7/8/10/11 animations, consumables, sit.

## 3. CAIMap: the data

`CAIMap` (ctor 0x5B3C30, dtor 0x5B5E10) is a member of the server world (`CWorldServer +0x170`); `CAIMap::Load` 0x5B43C0 has exactly one caller, `CWorldServer::NewZone` 0x595C70, so
only the server has grids and **a client does no pathfinding** (the client code only evaluates `MoveCurve`s) [I]. Pointers like `terrainGridRows` are row
tables (`rows[y] -> first cell of row y`); each grid object is `{+0 data, +4 rows, +8 width, +0xC height}`.

| Grid | Where | Cell | Written by | Read by |
|---|---|---|---|---|
| **terrain** | obj +0x2698, rows +0x269C | 9 bytes (below) | `Load` (height, water, material), `UpdatePassability` 0x5B8FA0 (nibbles), `SetBridgeCells` (height, bit 6), `TerrainStampApplier` 0x5B39D0/0x5B3A70 (byte +8) | everything: `PathFinder::StepCost` 0x5C1D50, `CanUnitStandAt` 0x5B80F0, `RateFootprintPath`, `IsCellWater`... |
| **flags** | obj +0x26A8, rows +0x26AC | u32: bits 0-4 material index; bits 5-6 object flag; bit 7 (0x80) "chain in the sector"; bit 8 (0x100) empty; inline volume: bits 9-11 type, 12-21 zmax, 22-31 zmin; chained: bits 9-28 entry index | `Load` (material, 0x100 init), `AddCellObject` 0x5B61B0 / `RemoveCellObject` 0x5B6380 | `GetCellStepCost` 0x5B8BA0, `GetMaterialAt`, `HasObjectType3At` 0x5BBD10 |
| **occupancy / scratch** | obj +0x26B8, rows +0x26BC | u8 | `UpdatePassability` (per-class cost nibble before erosion), `PathFinder::WriteOccupancy` 0x5C2A30 (direction codes 1..8 during a long search), `FindFreeCellNear` (reachability marks 1/2) | `ExtractOccupancyPath` 0x5B9A20, `ClearPassabilityClass` 0x5B89F0 (skips cells != 0) |
| **obstacle (walking units)** | obj +0x1F4, rows +0x1F8 | u8 level | `ObstacleStampApplier` 0x5B3B20/0x5B3B90 (StampMap at +0x12A4) | `CanUnitStandAt`/`CanUnitMoveTo` (level 2), `CAIMap::PromoteWalkersToStanding` 0x5B7EF0 |
| **cell-object records** | +0x14C (floors/obstacles) and +0x1A0 (bridges); 24-byte records, 16 hash heads at +0x15C / +0x1B0 | `{x, y, zmin, zmax, type u8 @+0x10, flag u8 @+0x11, border u8 @+0x12, next @+0x14}` | `StampObject` 0x5B6A80, `CSpellEffect::UpdateVolumes` 0x67EFC0 (via `CollectCircleCells` 0x5B2760 / `CollectHalfCircleCells` 0x5B2A00, type 3) | `AddCellObjects` 0x5B65B0, `SetBridgeCells` 0x5B6740 |
| **sector entry chains** | `CollisionMap +0x14` sectors (below) at CAIMap +0x138, 0x54 bytes each | entries of 8 bytes `{u16 zmax, u16 zmin, u8 type, u8 flag, i16 next}` in the sector's CArray (+0x44), free chain +0x3C | `NavSector::AllocObjectEntry` 0x5BFE70 / `UnlinkObjectEntry` 0x5BFFD0 | `GetCellStepCost` |
| **bridge hash** | +0x10C (ScriptRefCacheHash, buckets) | key `x << 16 | y` -> saved original `{height, flags}` of a cell turned into a bridge | `SetBridgeCells`, `CAIMap::RestoreBridgeCell` 0x5B30B0 | `GetWalkHeightAt` 0x5BBA10, `CAIMap::BridgeHashFind` 0x5B3160 |
| **NavGrid (AIGRAPH)** | +0x22F8, size 0x29C | see section 4.2 | `NavGrid_Read` 0x5B1180 from the `.mob` (and `NavGrid_Build` 0x5AFC70 on dynamic changes) | `NavGrid_FindPath` 0x5B0D90, `DominantRegion`, `IsCellInRegion` |
| **StimulusMap** | +0x26C8 | | hearing / vision (module 0x5C8000) | not movement |

### 3.0 `CAIMap::Load(name)` 0x5B43C0: how the grids are filled [V, deep-read 2026-11-06]

The terrain files are the renderer's (`.mp` header and one `.sec` per sector inside the zone's `.mpr`; byte layout in terrain-lighting.md 2.1 / 2.2);
the AI reads them again on its own:
1. `.mp`: magic `0xCE4AF672` and size `0x26 + 4 x (11 x materials + tileTypes + animated)` ("Unable to open file", "Wrong file format" -> `FatalError`);
   `heightScale (+0x120) = 511 / maxHeight`; the water materials (11 dwords each) and the `tileId -> tile type` table are kept for step 4.
2. Slope tables (0x3FF entries each, index `dh + 511`, dh in height steps): `a = atan(|dh / heightScale| / 0.5 m)` in degrees,
   `s = (1 + sin^2 a)`; walkers `dh >= 0` -> `round(s^2 x 1024)`, `dh < 0` -> `round(1024 / s)`, -1 above 40 deg; class 0: 0x400, -1 above 60 deg.
3. Materials: `aiinfo.res` (work dir) -> `tileDesc.reg`; 16 records of 0x118 bytes (defaults: name "", Speed -1, CostMul 1, the three step floats
   1.0); every section is a material whose **index is the section name read as a number**; keys: a name string -> +0, `CostMul` (int, default -1)
   +0x104, `Speed` (int, -1) +0x100, `StepLook` / `StepSmell` / `StepSound` (ints / 100, default 100) +0x108 / +0x10C / +0x110 ("Can't open
   aiReg", "Can't open file tileDesc.reg", "Unable to read file aiinfo.res").
4. Per sector `"%s%03i%03i.sec"` (magic `0xCF4BF774`, size 0x240D / 0x4815 (water layer) + 0x200 when `flags & 3`; "Unable to locate resource",
   "Wrong format resource"); each 1 m vertex cell gives 2 x 2 tiles. **Height** of a tile = `0.5 x its nearest corner + 0.25 x the two corners
   adjacent to that corner` (the far corner does not count), vertex `z = u16 x maxHeight / 65535`, stored `round(h x heightScale)`. **Water depth**
   (water layer only) = the same blend of the water vertices minus the tile's ground height, `round(x heightScale)` clamped 0..0x3F into bits 0-5.
   **Material** (flags bits 0-4, per 2 m texture tile = 4 x 4 AI tiles) = the tile type of `tileId & 0x3FFF` (15 when `tileId >= textureCount x 64`).
   **Water bit** 0x80 = the tile's water material is set (not 0xFFFF) and its type (dword 0) is **2 or 3**, and the depth is non-zero (else cleared);
   such tiles then take the material of the **water layer's** tile word instead of the ground's.
5. `loaded = 1`, `navGridOn = 0`, `PackMaterialCosts`, `InitCostTables`, the obstacle grid (w x h bytes) zeroed, `PathFinder::Init` forward (0) and
   backward (1), `NavGrid::Init`, `StimulusMap::Load(w / 2, h / 2 metres)`, `CollisionMap::Resize(w, h)`, then `UpdatePassability` over the whole map
   (or the dirty rectangle while batching). Returns 1 (0 when the resource does not open; the `FatalError` paths return its value).

### 3.1 The terrain cell (9 bytes) [V]

| byte | meaning |
|---|---|
| +0 u32 | 8 nibbles; nibble k (bits 4k..4k+3) = passability of unit class k: **0 blocked, 1..15 usable** (15 best). 7..15 are "walkable" (cost factor < 12000), 1..6 are swamp-like (factor 12000..36000); `IsUnitPositionWalkable` 0x5BA5A0 accepts only a step cost 1..11999 |
| +4 u16 | ground height in steps of `maxZ/511` (`CAIMap +0x120 = 511/maxZ` is the scale; `GetTerrainHeight` 0x5B77F0 divides by it). Bridge cells hold the bridge top (`SetBridgeCells`) |
| +6 u16 | bits 0-5 water depth (0x3F = very deep), **bit 6 (0x40) cell is a bridge / platform** (original data saved in the bridge hash), bit 7 (0x80) water. The high byte (+7) is unused [I]: `TerrainGrid_Alloc` 0x5BD030 masks the word with 0xFF00 on fresh `operator new` memory (so it stays uninitialised), every writer keeps it (`& 0xFFC0` in 0x5B30B0, `& 0xFF40` / `& 0xFF3F` in 0x5B6740) and no 16-bit read of the cell tests bits 8-15 (all readers mask 0x3F / 0x40 / 0x80) |
| +8 u8 | **standing-unit level** (see 3.3). 0 on a freshly loaded map |

The border: `UpdatePassability` forces the 3 outermost rows / columns to nibble 0 (units can't leave the map); `PathFinder::Init`'s 34 x 34 window is closed on its edge.

### 3.2 How the nibbles are computed (`UpdatePassability` 0x5B8FA0, `GetCellStepCost` 0x5B8BA0, `MarkSteepCells` 0x5B8710) [V]

Called for a rectangle (+3 margin) after load and after every object change [V, deep-read 2026-11-06: the scratch step costs are computed over the
rectangle + 3 (clamped to the map), the nibbles written and eroded only inside the rectangle itself, so the erosion sees the margin]. For each of the 8 classes k:

1. per tile `GetCellStepCost(k, thr = round(T[k] * heightScale), x, y)` into the scratch grid. `T = {2.0, 0.4, 1.0, 1.6, 2.2, 1.7, 3.0, 6.0}` (table 0x7912BC, metres of
   head-room / step). Empty dry tiles use the material's pre-packed nibble (`PackMaterialCosts` 0x5B8620, record +0x114). Otherwise: `lo` = ground (+ water depth
   for class 0), `hi = lo + thr`; speed starts 0x400, cost = 1 (class 0) or the material `CostMul` (record +0x104: GRASS 2 ... WATER 8, Swamp 50); class > 0 in water:
   deep (0x3F) or depth >= limit-1 blocks (limit = 0 for class 1, thr else), depth > limit/2 -> speed 800 and cost +30, else speed 0x370; each object volume overlapping
   [lo, hi] (types 0/1) adds `cost += round(A[type] * f)` (A = {290, 20}) and `speed *= 0.6^(B[type] * f)` (B = {7, 2}; ln 0.6 stored at 0x7C2F8C), f = overlap / (hi - lo); type 2 and 4 are
   ignored, **type 3 doubles the cost** (spell zones from `UpdateVolumes`: Fire wall, lightning, circles; also read by `HasObjectType3At`, which the fear AI uses for "standing in a bad spot").
   `speed < 0` or `cost > 70` -> 0; `s = min(15, (speed + 50) / 102)`; `row = cost<3 ? (cost>1) : cost<10 ? 2 : cost>34 ? 4 : 3`; nibble = `LUT[row*16 + s]`
   (table 0x791164, 5 x 16). Row 0: `0 0 1 1 1 1 4 7 11 12 14 14 15 15 15 15`, row 1: same up to 11 12 13 13 ..., row 2: `.. 4 7 8 9 10 ..`, row 3: `.. 4 5 6 6 ..`, row 4: `.. 2 3 3 3 ..`.
2. Copy into the terrain dword's nibble k; then **erode**: for every footprint offset in the class's list clear the nibble in the shifted rectangle where the
   scratch value is 0 (`ClearPassabilityClass` 0x5B89F0). Lists: classes 0..4 `+0x204` = 5-tile cross (table 0x7912DC), classes 5..6 `+0x218` = 13-tile diamond r2 (0x79130C), class 7
   `+0x22C` = 29-tile diamond r3 (0x79137C). Static obstacle clearance for the class is therefore baked into the nibbles (0.5 / 1.0 / 1.5 m).
3. `MarkSteepCells`: cells whose largest height difference to the neighbours exceeds the slope tables' limit are cleared (all classes for > 60 deg; > 40 deg keeps class 0
   only) and the neighbours' classes 5-7 are cleared by lists `+0x240 / +0x254 / +0x268`.
4. When `CAIMap +0x02` (`navGridOn`) is set: units in the area are taken out of the collision map, `NavGrid_Build` rebuilds the nav cells of the rectangle (+1 cell
   margin), the units are put back (the units come from `FindUnitsNear(centre of the margin rectangle, 0.5 x sqrt((w + 1)^2 + (h + 1)^2))` with w, h its size in metres, so the 16 m sector
   rounding of 7 applies). `navGridOn` is `CWorldServer +0x172`; set after a zone/save is loaded, cleared during loading (`CWorldServer::BeginPaint/EndPaint` 0x595A90/0x595A70,
   misnamed: now `EndAIMapBatch` / `BeginAIMapBatch`, and neither has any reference in the exe: dead editor code [V]) and for the duration of `ApproachTarget` / `ValidateQueuedOrder`.

The class an individual unit uses (`CUnitServer::ComputeMoveClasses` 0x552FB0, [V]): size `S = CUnit::GetSize`, speed `s = proto[+0x120] * field14C`:
S <= 0.85: class 1 if s < 0.6, 2 if < 1.2, 3 if < 1.9, else 4; S <= 1.5: class 5 (s <= 1.7) or 6; S > 1.5: class 7. Stored `+0x248` (walk / run), `+0x24C`
(crawl, uses 0.2 s) and `+0x24E` (sneak, 0.6 s); `UpdateMoveParams` 0x530570 copies the right one into `CUnit +0x324` by stance. Class 0 = flying / floating units (server
`+0x240 == 2`, `+0x33F == 2`); they use the "class 0" slope table and treat water as ground.

### 3.3 The unit footprint stamps and the two levels [V]

* `CollisionMap::AddObjectAndStamp` 0x5C0DF0 / `CollisionMap::RemoveObjectAndUnstamp` 0x5C1010 / `CollisionMap::MoveObjectAndRestamp` 0x5C11D0 (the map at CAIMap +0x124, global 0x7B01F4; sectors of 32 x 32 tiles, 0x54 bytes each, a unit list at
  sector +0x20 and an object list at +4; the unit's tile is cached in `CUnit +0x1B8/+0x1BC`, flag 0x8000 in `CUnit +8`) call `CAIMap::StampObject` 0x5B6A80.
* For a unit (type id 0x50) `StampObject` takes `FootprintStamp_GetCached(radius)` 0x5B8350 (cache keyed by radius rounded to 0.1; `FootprintStamp::Ctor` 0x5B3440:
  16 x 16 bytes, value `max(0, round((r + 2 - dist/2) * 16))`, `dist` in tiles from the centre tile (8, 8)) and `StampMap::AddStamp` 0x5B36B0 / `RemoveOwner` 0x5B3890 it at `tile - 8`.
  A `StampMap` keeps 4 x 4 blocks in 1024 buckets keyed `(by + bx*17) & 0x3FF`; adding **max**-merges (applier vt[1]), removing deletes the owner's blocks and **re-sets** the cell
  block to the max of the remaining owners (vt[0]).
* **Which grid**: `CUnit::Get278` (`+0x278` = "walking": set by `TickMovement` when state == 0, cleared by `StopMove`) selects the `StampMap` at **+0x12A4 -> obstacle grid** for
  walking units and **+0x27C -> terrain byte +8** for standing ones. A unit with `+0x274` set is not stamped at all: `+0x274` is the dead flag (`CUnitServer::SetDead` 0x546720 writes it, vt[55] `CUnit::Get274` 0x446880 returns it), so corpses never block [V].
* **Fit test** `CAIMap::CanUnitStandAt` 0x5B80F0: `R = max(0, round((2 - radius) * 16))`; fits when `terrain.b8 <= R` (else blocked at *level 1*) and `obstacle <= R` (else *level 2*).
  The stamps are exactly `(r_other + 2 - d)*16` so the test is `d >= r_other + r_self` (a circle test quantised to tiles).
  `CanUnitMoveTo` 0x5B81B0 additionally accepts a cell that is no tighter than the unit's current cell (so a unit already inside a stamp can leave).
* **The path finders only read the terrain byte (standing units), never the obstacle grid**: walking units are invisible to the planner. During planning the unit itself is removed from the
  map (`FindPath` does `RemoveUnit` ... `AddUnit`), and `CUnitServer::CollectPartyFollowers` 0x5468D0 additionally removes the player's followers and moving party members near the
  target so a group does not block itself.
* `PathFinder::SaveFreeRadius` 0x5C33F0 / `WriteFreeRadius` 0x5C3530 clear the byte +8 of the window for nav-level work (so nav connectivity ignores units) and restore it.
* Static objects do **not** use stamps: they become volumes (`cell-object records`) via `StampObject`: each body part (`OBJ_BODYPARTS`; part flags: bits 3-5 type, 6-9 flag, 0x400 bridge,
  0x800 skip) box faces are sampled and merged per tile (`CellObjectRecords::AddTopSample` 0x5B2D10 keeps the highest top sample, `CellObjectRecords::AddBottomSample` 0x5B2E10 the lowest bottom/side sample);
  `BASE` parts are floors (tile height raised, material ROAD, bit 6 set via `SetBridgeCells`), `CROWN` ignored. Exact rules: aigraph.md "Objects".
  [V, deep-read 2026-11-06] `StampObject(obj, remove)` in detail: both record sets are emptied first. The part names come from the object's prototype list
  (`specific +0x80`, when its count `+0x88` is non-zero) else the object's own list (`+0x110`); a name missing from the model is a `FatalError` ("Bodypart %s
  for object %s not found"). Per part (0x38-byte record at `obj +0x160`: flags +0, two offsets +4 and +0x2C added to the box, box min +0x14 / max +0x20)
  the 8 box corners are moved by the offsets, rotated by the object's matrix (from its quaternion) and placed at `obj pos + (0, 0, ground z)` (ground =
  the bridge hash height when the object's tile is a bridge). The corners are ordered so that each consecutive triple spans one face (6 faces); a face
  with edges `u`, `v` is sampled on a grid of `round(3 |u_xy|) + 2` x `round(3 |v_xy|) + 2` points at the cell centres (`|.|` in metres, so ~3 samples per
  metre = 1.5 per tile, plus 2), each sample's z quantised to `round(z * heightScale)` clamped 0..0x3FC; faces whose rotated normal points up give top
  samples, all others (bottom and sides) bottom samples; samples on the face's outer rows / columns set the tile record's border byte (+0x12). Then
  `SetBridgeCells` (when a bridge part exists), `AddCellObjects(floors, remove)` and the union of both record bounds + 3 goes to `UpdatePassability` (or
  the dirty rectangle while batching). The nav grid is rebuilt for it only when `navGridOn` and the object's vt[43] agree (`navGridOn` is restored after).
  Units return at once (1) after the stamp; objects return `AddCellObjects`' result.
  The part records come from `CObject3D::BuildFromAnimation(model)` 0x5179E0 [V, deep-read 2026-11-06]: one 0x38-byte record per skeleton node,
  box from `CFigureManager::LoadPartBounds` (morphed by the object's three morph parameters), `+0x2C..+0x34` = the node's world position in the bind
  pose, flag 4; the name prefix sets the AI flags: **`BASE` -> 0x400** (floor / bridge), **`CROWN` -> 0x90** (type bits 3-5 = 2, which the step cost
  ignores: tree crowns never block), **`EMPTY` -> 0x890** (the same + 0x800 = skipped by `StampObject`). The object box (`+0x128` centre, `+0x138` /
  `+0x144` min / max, `+0x134` radius) is the union of the part boxes, except that humanoid units (kind 0x32) get a fixed +-0.5 m box in x / y.

### 3.4 Dynamic changes

`StampObject` -> `AddCellObjects` -> `UpdatePassability(bounds + 3)` immediately, or merged into the dirty rectangle `CAIMap +4..+0x10 {x0, x1, y0, y1}` when `CAIMap +1` ("batching") is set
(world load: `0x5B8B30` starts it, `0x5B8B80` flushes). Producers: `CObjectDesc::Spawn`, `CLeverObject::SwitchState` 0x52B7A0 (gates / bridges; byte `+0x261` of the lever says whether the nav grid is
rebuilt), `CSpellEffect::UpdateVolumes` 0x67EFC0 (spell types 6 / 7 half circles, 8 / 0x27 circles -> type-3 volumes), `CWorldServer::LoadZone / LoadSave`.

## 4. The path finders

### 4.1 `PathFinder` (tile level; 0x5C1D50-0x5C35F0; objects at CAIMap +0x2594 forward and +0x2614 backward, also one inside the NavGrid at +0x98)

Plain **Dijkstra, no heuristic** [V]. Object layout (0x80): +0 active, +1 saved-radius flag, +4 node grid (34 x 34 x 0x14), +8 rows, +0x14 byte map for `SaveFreeRadius`, +0x24 CAIMap, +0x2C row pointers
into the terrain (34), +0x3C closed list, +0x40..+0x4C **4 open lists** (a node goes to list `x & 3`, each sorted by cost; `PickNext` 0x5C2F50 takes the smallest head), +0x50 current node,
+0x54/+0x58 window origin, +0x5C current cost, +0x60 `StepQuery` {cost table, centred slope table, class*4, ...}, +0x78 class, **+0x7C direction flag** (0 forward, 1 backward), +0x7D stamp radius R.
Node (0x14): `{i32 cost (0x7FFFFFFF = unreached), next, prev, i16 x, i16 y, u8 passable(0 = window border), u8 reached, u8 backDir}`.

* `BeginForUnit` 0x5C2420 / `BeginForClass` 0x5C24F0 / `Begin` 0x5C2B10: set the query (`InitStepQueryForUnit` 0x5B9580: cost table `+0x14` (walk) or `+0x54` when `CUnit +0x43C != 0`, slope table `+0xD8`
  (classes 1-7) or `+0xEC` (class 0)), the window (`SetWindow` 0x5C22A0: both points within 31 tiles, origin = midpoint - 16, never beyond `min corner - 1`), the start node (cost 0), `active = 1`.
* `Expand` 0x5C2D40: close the current node, relax the 8 neighbours, diagonals use 0x5A8 (1.414), straight 0x400; `StepCost` 0x5C1D50 [V]:
  `-1` if the node is a window border; flag 0 (forward) uses the **target** cell's nibble and slope `h(target) - h(cur)`, flag 1 (backward) the **current** cell's nibble and slope `h(cur) - h(target)`;
  `-1` if `nibble cost < 0` or `slope < 0`; `-1` if `target.b8 > R` and `cur.b8 <= target.b8` (too tight, unless we are leaving something tighter);
  else `((slope * cost[nibble]) >> 10) * mult >> 10`. **Diagonals do not check the two orthogonal neighbours** (corner cutting is possible; the erosion compensates).
* Cost tables (CAIMap `InitCostTables` 0x5B6070, 0x400 = 1.0): `+0x14` by nibble 0..15 = `-1, 0x6400, 0x77EC, 0x8BD8, 0x2FF8, 0x37F0, 0x3FFC, 0x0DFC, 0x0FFF, 0x11FD, 0x1400, 0x666, 0x732, 0x800, 0x400, 0x4CC`
  (note 14 is the cheapest, 15 costs 1.2, 11 is 1.6); `+0x54` = `-1, 0x400 x 15` (uniform: used when `CUnit +0x43C != 0`: a unit standing in water, and every chase from `ApproachTarget`);
  `+0x94` = **speed** factors (not costs) `-1, 0x200, 0x266, 0x2CC, 0x266, 0x2CC, 0x333, 0x2CC, 0x333, 0x399, 0x400, 0x333, 0x399, 0x400, 0x400, 0x4CC` (used by `RateFootprintPath`).
* Slope tables (`Load`): indexed by height difference `dh` in height steps (-511..511); `a = atan(|dh / heightScale| / 0.5)` degrees, `g = (1 + sin^2 a)^2`; walkers (classes 1-7): `dh >= 0` -> `g` x 1024,
  `dh < 0` -> `1/sqrt(g)` x 1024, blocked (-1) above 40 deg; class 0: always 0x400, blocked above 60 deg. (uphill up to 2.0x at 40 deg, downhill down to 0.71x.)
* `ShiftWindow(x0, y0, x1, y1, reopen)` 0x5C25D0 [V, deep-read 2026-11-06]: `SetWindow` for the new pair (fails unless both points are within 31 tiles), the 34
  terrain row pointers re-based, and a failure when the origin moved by >= 31 tiles in x or y. The nodes of the overlap keep cost, reached flag and back
  direction (copied from a snapshot of the old grid); the newly exposed nodes get cost INF, not reached, back direction 0; all list links are dropped. Then
  the lists are rebuilt: `reopen == 0` -> the reached nodes go to the closed list and every unreached node with a finite cost is re-queued; `reopen == 1`
  -> `m` = the cheapest cost among the copied nodes that sat on the old window's outer interior ring (rows / columns 1 and 32, the ones whose expansion
  the closed border had cut), nodes cheaper than `m` stay closed, every other finite node is re-opened. Current = the cheapest head of the 4 open lists.
  `FindLongPath` passes 0 after a waypoint was reached and 1 after a substitute waypoint and for the final run to the goal.
  `WriteOccupancy` writes the reached nodes' back-direction into the global direction grid (only where still 0). `RunUntil` 0x5C31A0 / `RunToEnd` 0x5C30E0 / `GetCost` 0x5C3090 /
  `ExtractPath` 0x5C3200 (list order start -> end, built by pushing at the front while following back-links) / `Reset` 0x5C35F0.

### 4.2 `NavGrid` / `PathSearch` (nav level; CAIMap +0x22F8, size 0x29C)

The AIGRAPH of the zone: per layer (= unit class) and per 4 m cell: `A` 8 x i16 costs to the neighbours (`min(0x7FFF, tileDijkstra>>7)`, 0xFFFF blocked, made symmetric by `NavGrid_ClearBorders` 0x5B0F70),
`B` the representative tile `x | y << 4` (`NavGrid_BuildCellOffsets` 0x5AF900: best nibble, rings 8/6/4/2 wide, verified by `CheckBlockPassage` 0x5AF7B0), `C` the region id (components, `NavGrid_BuildRegions` 0x5B0050).
Planes: A rows at `+0x120 + k*0x10`, B at `+0x1A0 + k*0x10`, C at `+0x220 + k*0x10` (each a 0x10-byte grid object starting 4 bytes earlier); `+0x124/+0x128` = width / height in cells; `+0x118` progress callback.
It is **read from the `.mob`** at zone load (`NavGrid_Read` 0x5B1180 inside `CWorldServer::NewZone`, `CWorldServer +0x2468`) and rebuilt only for changed rectangles (`NavGrid_Build` 0x5AFC70, `ComputeCellCosts` 0x5AFAE0).

`PathSearch` (0x4C bytes, forward at NavGrid +0, backward at +0x4C; `Init` 0x5AED60, `Relax` 0x5AEEC0, `ExpandCurrent` 0x5AF0C0, `TimerQueue_Insert/FindNext` 0x5AEE30/0x5AEFF0 = the open lists, **8 lists by `x & 7`**) is again Dijkstra without
heuristic over nodes of 0x18 bytes `{cost, next, prev, i16 x, i16 y, int backDir, u8 closed}`; the neighbour cost is `A[cell*8 + dir - 1]`. `NavGrid_FindPath` 0x5B0D90 [V]:
cost plane = `NavGrid +0x11C + class*0x10`; the **mover's blocked nav cells** (`CMover +0x70` list) get cost -1 for the duration; both searches are *seeded* by `SeedFrom` 0x5B0AF0 -> `NavGrid_CollectReachableCells` 0x5B0370
(a tile-level `PathFinder` run from the exact start / goal tile over the 3 x 3 nav cells around it, stamps cleared) giving each reachable neighbour cell's real distance (`>> 7`); then `NavGrid_MeetSearches` 0x5AF5F0
alternates `ExpandCurrent` on both until the current node of one is closed by the other, extracts both halves (reverse the backward one) and removes duplicates. (First meeting, not proven optimal.)
`DominantRegion` 0x5B0600 (region id most common among the reachable cells of a tile) and `IsCellInRegion` 0x5B07C0 answer "can a unit at A reach tile B" in O(1)-ish.

### 4.3 `CAIMap::FindPath` 0x5BB350 (14 callers) [V]

`FindPath(unit, goal*, mover, maxLength, minDist)`; `mover` is the unit's `CMover` (CUnitServer +0x3C):

[V, deep-read 2026-11-06]
1. reject if `goal.x^2 + goal.y^2 > 1e8` (metres); store the requested goal **x, y and z** in the mover (`+0x60..+0x68`; section 5.2 calls the z slot "max
   length" because every order puts the length into the goal's z, and the re-plans read it back as such).
2. `CollisionMap::RemoveUnit(self)`. `FindFreeCellNear(unit, goal, checkRegion = 0, minDist)` 0x5BAC10 (works in tile units, converts the goal in place):
   * with `checkRegion == 0`, a non-unit or a unit that can stand on the goal tile (`CanUnitStandAt` and its class nibble != 0) keeps the exact goal;
   * else: `v` = the unit->goal tile vector normalised so its larger component is +-1, and its +-45 deg rotations `r1 = 0.707 (vx - vy)`, `r2 = 0.707 (vx + vy)`
     (all four rescaled when a rotated component exceeds 1). For `d` from `max(0, round(2 minDist) - 1)` up to the Chebyshev unit-goal distance, the candidates
     are `goalTile - round(d * w)` with `w` in the order `v, (r1, r2), (r2, -r1), (vy, -vx), (-vy, vx), (-r2, r1), (-r1, -r2), -v`: first the tile **back toward
     the unit**, then rotated by +45, -45, -90, +90, +135, -135 and last the tile beyond the goal (only `d = 0` = the goal tile itself at distance 0). A candidate
     must pass the stand test above and, with `checkRegion`, the region test: the unit tile's `NavGrid::DominantRegion` is taken first, the occupancy grid of
     the unit-goal bounding box (+1) is cleared and used as a cache (1 = in region, 2 = not, else `NavGrid::IsCellInRegion`);
   * the accepted tile `c` gives the snapped goal `c + 0.01` on an axis where `c >= goalTile`, `c + 0.99` where `c < goalTile` (0.01 tile inside the
     candidate's edge facing the goal); `d = 0` keeps the exact goal. No candidate -> `FindPath` fails (unit put back).
   Stored as the snapped goal (`+0x58/+0x5C`). A snapped goal within `sqrt(0.001)` tile of the unit's exact position -> **failure** (no path, and the `+0x6D` flag is
   not set).
3. same tile as the unit -> a one-cell path, the cell's speed factor (+0xC) = 0x200 (only if the unit can stand there and its nibble != 0; non-units always); no
   smoothing. Octile distance `|dx - dy| + (min * 0x5A8 >> 10) < 25` tiles (the pending list is cleared) and both `PathFinder::BeginForUnit` succeed ->
   `MeetLocalSearches` 0x5B9690 (forward from the unit, backward from the goal, alternate `Expand` until one's current node is reached by the other; extract,
   reverse, join, de-duplicate).
4. otherwise (or on a miss) `FindFreeCellNear(original goal, checkRegion = 1)` again (failure -> fail); the snapped tile equal to the unit's tile -> a one-cell
   path as in 3; else `FindLongPath` 0x5B9C90 with `allowRetry = (mover.+0x6E || mover.+0x6F)` (AI units, or the first request after a reset) and
   `maxLen = round(maxLength * 2 - 0.5)` tiles.
5. `CAIMap::SmoothPathStart` 0x5C46C0 (see 4.5) with the facing code `((6 - round(atan2(-fwd.y, -fwd.x) * 4/pi)) mod 8) + 1` and the "straightness"
   `2 |snapped.x - round(snapped.x)|` (the code computes this twice from the **x** coordinate and takes the minimum: a copy-paste slip, y never counts); its
   result (the corridor cost) goes to mover `+0x84`. `CAIMap::DeliverPathChunk` 0x5BB190; `CollisionMap::AddUnit(self)`; on failure mover `+0x6D = 1`; `+0x6F = 0`.

### 4.4 `FindLongPath` 0x5B9C90 [V]

`NavGrid_FindPath(class, start tile, goal tile, blocked cells)`; if it fails -> fail. If `navCells * 8 > maxLength` (in tiles) -> fail. Else clear the direction grid around the nav path (`ClearOccupancyAround`),
turn every nav cell into its representative tile (B nibbles) and append the goal tile; walk the chain with **one** `PathFinder`: `BeginForUnit(start, wp1)`, `RunUntil(wp1)`, `WriteOccupancy`, then `ShiftWindow(prev wp, next wp)`
for each further waypoint (waypoints equal to the current position are skipped). [V, deep-read 2026-11-06] If `RunUntil` does not reach a waypoint, the
offset table (`GetCellOffsetsForRadius(1e20)`: all 21 x 21 offsets sorted by distance, `CellOffsets_Build` 0x5BF8F0) is scanned around the waypoint for the
**nearest tile the search already reached** (finite cost); that tile becomes the substitute position and the chain continues from it (none -> failure handling
below). When the last waypoint (the goal) has not been reached: `WriteOccupancy`, `ShiftWindow(current, goal, reopen 1)`, `RunToEnd`, and the goal must have a
finite cost. After each successful leg `WriteOccupancy`. At the end the start tile's direction byte is zeroed and the tile path is read back with
`ExtractOccupancyPath` 0x5B9A20 (follows the direction grid from the goal to the 0 start cell) into the mover's pending list; a path that does not begin at the
start tile is a failure.
On a failed leg the **octile distance from the start to the last reached waypoint** decides: <= 15 tiles and `allowRetry` -> a nav cell is appended to the
mover's blocked list (`+0x70..`) and the whole function recurses (a unit-blocked corridor is routed around; no depth limit, each level blocks one more cell).
The cell is the one linked from the failing waypoint's record (`+0x10`), i.e. the nav cell **before** the waypoint's own cell (the first waypoint links its own
cell). <= 15 without `allowRetry` -> failure. Farther: the path to the last reached waypoint is kept, the mover's snapped goal becomes that tile's centre and
`+0x6D = 1` ("partial, continue later"). The nav search ignores units; the tile windows see the standing ones. The blocked list is not cleared here (only by
the mover reset).

### 4.5 Start smoothing, chunking, speeds

* `CAIMap::SmoothPathStart` 0x5C46C0 (for paths of >= 3 cells; the name undersells it): a **corridor re-plan of the whole path** [V for the
  loop structure, I for the table meaning]. The state is 9 `{cost, back-link}` slots = the positions of the 3 x 3 window around the current
  path cell. Start: the 8 neighbours of the start cell cost the step (passability x slope from `walkCostTable`, x 1.414 on diagonals) plus a
  turn penalty from the unit's facing `lastDir` (`(|dir - lastDir| folded to 0..4) - 1`, min 0, x `walkCostTable +0x10` x 60). [V, deep-read 2026-11-06]
  The step query's `+0x10` is the constant 10.0 (`InitStepQueryForUnit`), so the penalty is **600 per 45 deg beyond the first** (90 deg 600, 135 deg 1200,
  reversing 1800; a straight step on the best ground costs 1024). A start neighbour is blocked unless its stamp byte is `<= R` or below the start cell's, its
  nibble cost is >= 0 and the slope factor > 0. The ninth slot (the start cell itself) starts at cost 0 with no back-link; `this` is the CAIMap's scratch
  object (`exitScratch`), not the map. Then for every
  following path cell (the whole list, head to tail) `NavGraph::RelaxPortal` moves the 9 slots to the next window through the table entry
  `g_0x7C3058[inDir][outDir]` (0x3C bytes, 9 x 9 entries, built once by `NavPortalTables_Build` 0x5C42B0 from
  `NavPortalSet_BuildForDirection` / `NavPortalPair_Build`): the positions shared by the two windows copy their cost, the entry and exit
  edges are relaxed with `PathFinder::RelaxNeighbour` (real terrain cost). At the goal the 8 approach slots get `(1 - straightness) * 2048`
  on the diagonals and the cheapest wins; when it beats the previous total the path list is rebuilt from the goal backwards along the
  back-links (`Cell_Neighbour` per stored direction, `AddHead`), and the whole pass repeats until the cost stops improving (the first pass always rewrites
  the path when any approach slot is finite; the returned cost is stored at mover `+0x84`). Net effect: the
  path may shift by one cell sideways anywhere along its length to cut corners, follow the facing and avoid diagonal arrival.
* `CAIMap::DeliverPathChunk` 0x5BB190: `MoveRequest::Clear`, `SetRect(unit pos, facing, tick - 1)`, moves **up to 32 cells** (`0x20`) from the pending list (`CMover +0x3C`) into the request's list (`+0x20`). All delivered ->
  `flags[0] (+0x6C) = 1` and the request target is the exact snapped goal; else `flags[0] = 0`, the last cell is pushed back to the front of the pending list (so the next chunk starts there) and the request target is that cell's centre.
  Then `CAIMap::RatePathCellSpeeds` 0x5BA400 stamps each cell node: `+0xC` = speed factor `= (table94[nibble(next)] << 9) / slope(h(next) - h(cur))` (512 = 1.0; 0x200 if the slope is blocked), `+0xE` = bit0 water (`terrain +6 bit 7`),
  bit1 "very costly" (step cost > 12000).
* `ContinueMove` 0x5BB900 (called when a chunk ran out): `+0x6D` set and nothing pending -> `FindPath` again from the unit to the stored goal (`+0x60`, length 1e6); else `StepAlongPath`.
  Returns "more path was expected"; 0 -> the unit stops.

## 5. Movement state

Three objects, all called "CMover/Mover" in functions.tsv (the old struct merged them; proposed names in `full/batch-pathfinding.tsv`):

| Object | Where | Size | What |
|---|---|---|---|
| **MoveRequest** | CUnitServer +0x3C (server), CUnitClientSpecific +0x38 (client), base of CMover | 0x3C | the *replicated order*: what the unit is doing from `startTick` on |
| **CMover** (server move state) | CUnitServer +0x3C = MoveRequest + 0x4C more | 0x88 | the path finder's output state: pending cells, goal, flags, blocked nav cells |
| **MoveCurve** ("Mover" in the names) | CUnit +0x27C (both sides) | 0xA4 | the timed curve built from a MoveRequest; evaluated every tick / frame |

### 5.1 `MoveRequest` (0x3C) [V]

| off | field |
|---|---|
| +0x00 | kind: 0 stand, 1 turn in place, 2 follow the cell path, 3 track a unit (turn to face it), 4 timed turn |
| +0x04 / +0x08 | start position x, y in tile units (metres * 2) |
| +0x0C | start heading (rad) |
| +0x10 | start tick |
| +0x14 / +0x18 / +0x1C | kind 1: target angle (f32), turn speed (f32, 0 = unit default); kind 2: goal x, y (f32, tile units; the chunk end); kind 3: target unit id, end-of-track tick(s) (`+0x18`, `+0x1C` are ticks); kind 4: angle, speed, time |
| +0x20..+0x38 | cell list (vtable 0x73EDB0): head +0x24, tail +0x28, count +0x2C, free +0x30, plex +0x34, block size +0x38 (0x80); node 0x10 `{next, prev, i16 x, i16 y, i16 speedFactor, i16 flags}` |

`MoveRequest::SetStand` 0x5C7B00 = kind 0 (stand at pos with heading and tick; returns whether anything changed), `SetTurnTo` 0x5C7C00 kind 1, `0x5C7C70` kind 2 target, `0x5C7C20` kind 3 (`ResetWithTarget2`, used by `StartAttackAnim2` to face a target),
`SetTurnToTimed` 0x5C7C40 kind 4, `Clear` 0x5C7D50, `ClearTargetIf(id)` 0x5C7C90 (kind 3 with that id -> -1; called before every curve rebuild with the unit's own id), `MoveRequest::ShiftTimes` 0x5C7CB0 (adds a delta to the start tick and, for kind 3,
the two end ticks). It freezes a unit's motion in time while its animation is paused [V]: `CUnitServer::TickAnimation` 0x546F30
shifts the mover by +1 every tick while `CUnitServer +0x20 paused` is set (and sets `CUnit +0x260 animPaused`), the client's
`CUnitClientSpecific::Update` 0x5341A0 does the same +1 per tick while `animPaused == 1`, and `CUnit::ReadNetState` 0x52EF90 applies -1
to a move request received while paused (the server already shifted it in that tick, the client update adds its own +1).

**Wire format** (`MoveState_WriteNet` 0x5C7DB0 / `MoveRequest::ReadNet` 0x5C7FC0, net section 10 of the unit; `CUnit::WriteNetState` 0x52EC70 / `ReadNetState` 0x52EF90): u8 kind, u8 count high, u8 count low, Vec2 (+4) quantised
(`Q64`: 16 bit each, step 1/60.2), heading as 16-bit range -pi..pi, then +0x10, +0x14, +0x18, +0x1C as raw dwords, then per cell one u16 `bits 0-1 = x code (0: x-1, 1: same, 2: x+1, 3: explicit i16 follows), bits 2-3 = y code the same,
bits 4-5 = flags & 3, bits 6-15 = speedFactor clamped to 0x3FF` plus the explicit shorts. The stream is delta-coded against the previous state (DeltaCodec). A section-10 read does `ReadUnitOrder`, `ClearTargetIf(own id)`,
`GetSpeeds` -> `CUnit +0x2B8/+0x2BC`, `CMover::BuildPath` and `MarkNetDirty(10)` again (relay). Also message 0x0E (below).

### 5.2 `CMover` (server, 0x88) [V]

+0x3C pending cell list (vtable, head +0x40, tail +0x44, count +0x48, free +0x4C, plex +0x50, block +0x54), **+0x58/+0x5C snapped goal** (tile units, f32), **+0x60/+0x64 requested goal x, y (metres)**, **+0x68 max path length** (the order's z),
flags **+0x6C "whole path delivered"**, **+0x6D "partial / no path: re-plan from the stored goal when the chunk ends"**, **+0x6E "AI unit" (no player)**, **+0x6F "first request after reset"**, +0x70 blocked nav-cell list (CArray of `{x, y}` pairs: vtable +0x70, data +0x74,
count +0x78, cap +0x7C), +0x84 the corridor cost returned by the last `SmoothPathStart` (written by `FindPath`). In CUnitServer terms: goal `+0x9C`, length `+0xA4`, delivered `+0xA8`, partial `+0xA9`, AI `+0xAA`. `Reset` 0x5B23A0, `ResetForUnit` 0x5B2190, `ResetWithTarget*` 0x5B1DE0 / 0x5B1F10 / 0x5B2050
(reset + request kind 1 / 3 / 4), `CMover::ExportFullPath` 0x5B22C0 (request + all cells incl. pending, speed factors forced to 0x200, for the report message), `ReadState/WriteState` (save games).

### 5.3 `MoveCurve` (0xA4 at CUnit +0x27C) [V from `BuildPath` / `Evaluate` / `Predict`]

Layout: +0 vtable, +4/+8/+0xC/+0x10 segment array (0x1C-byte `{x, y, dirx, diry, speed, cellx, celly}`, count, cap, grow), +0x14 vtable, +0x18/+0x1C.. point array (0xC-byte `{x, y, runLength}`), +0x28 vtable, +0x2C/+0x30.. cell speed array (f32),
**+0x3C base speed** (= `CUnit +0x2B8`, `GetSpeeds` a), **+0x40 turn rate** (= `CUnit +0x2BC`, `GetSpeeds` b = run-speed stat * `AiInfo.RotateSpeedMult` (+0x40 of the AiInfo block at 0x7C53F8, global 0x7C5438; `config\ai.reg` [RPG] may override it; the shipped configs do not, so 100 applies [V], see 7);
with 100 a turn of pi takes a small fraction of a tick, i.e. effectively instantaneous), +0x44 current turn speed, +0x48 start heading, +0x4C turn delta, +0x50..+0x74 the cached segment (start xy, start dir, end xy, end dir, length, speed), +0x78 X cubic (4 f32), +0x88 Y cubic, +0x98 / +0x9C
segment start / end time, +0xA0 cached segment index (`CUnit +0x31C`, -1 = none).

`CMover::BuildPath` 0x5C67D0(request):
* kind 0 / 3: nothing; kind 1: turn delta = wrapped (`atan2(sin a, cos a)` - heading) into [-pi, pi], turn speed = the request's `+0x18` or, when 0,
  `+0x40`; kind 4: the same delta, but always the unit's `+0x40` turn rate (the request's speed is ignored here) [V, deep-read 2026-11-06].
* kind 2 (and any kind above 4): **points**: start position first; for each cell the run counter of the last point is incremented and the cell's speed
  factor appended; `D` = direction of the step to the next cell via table 0x791988 [V, deep-read 2026-11-06, corrected]: a **diagonal** step always
  appends the corner it cuts (`D` 2 NW -> `(x, y)`, 4 SW -> `(x, y + 1)`, 6 SE -> `(x + 1, y + 1)`, 8 NE -> `(x + 1, y)`, cell coordinates); a
  **straight** step appends the midpoint of the edge it leaves through (1 N `(x + 0.5, y)`, 5 S `(x + 0.5, y + 1)`, 7 E `(x + 1, y + 0.5)`, 3 W
  `(x, y + 0.5)`) only when its direction differs by >= 90 deg (2 codes) from the previous step or from the next one; otherwise the run continues.
  The request goal (`+0x14/+0x18`) is the last point. So straight runs become one segment group and diagonal stretches pass exactly through the tile
  corners. `BuildSegments` 0x5C66C0 /
  `AddSegmentsBetween` 0x5C6370: between consecutive points, `count` segments at fractions `(i + 0.5 + off0) / (count + off0 - off1)` (`off0 = -0.5` at the path start, `off1 = +0.5` at its end), all with the run's unit direction and
  `speed = cellSpeed[i] * baseSpeed / 512`; a one-run path is just two end points. `LimitSegmentSpeeds` 0x5C6290: a segment's speed is capped so traversing it takes at least `turnAngle / turnRate`.
  `CopySegmentCells` 0x5C6750 copies the request cells' x / y into the segments.
* initial turn: if the first segment's direction is within cos > 0.9 (~26 deg) of the heading the first direction is replaced by the heading and there is no turn, otherwise a turn in place by the wrapped angle precedes the path.

`MoveCurve::EvalPath` 0x5C6FC0 (t = ticks since `startTick`): before the turn ends -> heading only (`turning = 1`); otherwise find the segment (cached), `MoveCurve::HermiteInit` 0x5C73B0 builds the cubic `p(s) = p0 + d0 s + a2 s^2 + a3 s^3` with
`p(len) = p1`, `p'(len) = d1` (Hermite in **distance**, `s = (t - t0) * segmentSpeed`, `t0` accumulating `len / speed`), position = `(Poly3_Eval(X), Poly3_Eval(Y))`, direction = the derivative; returns the *next* segment's cell as "the
cell being entered"; past the last segment: the end point, speed 0, cell = the last segment's. `Mover::Predict` 0x5C7430 switches on the request kind and clamps `t` to `startTick`
[V, deep-read 2026-11-06]: defaults position = the request start, direction z 0, speed 0.1, both cells -1, turning 0. Kind 0: direction = the start heading.
Kinds 1 / 4: turn time `T = |turnDelta / turnRate|` ticks; after `T` the heading is `start + delta`, before it `start + sign(delta) x (t - startTick) x
turnRate`; kind 1 reports `turning = 1` throughout (also after the turn), kind 4 only while turning. Kind 2: `EvalPath`. Kind 3 (track a unit): the
target looked up by id (world vt[40]; gone -> as kind 0); aim point = its predicted position at the request's `+0x18` tick (vt[52]) while `t <= +0x18`,
its live position (vt[53]) afterwards, in tile units; until the `+0x1C` tick the heading moves linearly from the start heading to that bearing
(`start + wrapped(bearing - start) x (t - startTick) / (+0x1C - startTick)`), then it is the bearing itself (normalised vector; a zero vector leaves
(0, 0)). Position never changes for kinds 0 / 1 / 3 / 4. Speed units: tile units per tick (walk ~0.2 -> ~1.9 m/s at 18 ticks/s; docs/game-memory.md's "x 10 = m/s" is ~10 % high) [I].

### 5.4 Per-tick server update `CUnitServer::TickMovement` 0x547890 (states 0 walking / 1 turning) [V unless marked]

1. `Predict(curve, world, request, tick)` -> `pos`, `dir`, `speed`, `cell`, `turning`, `ended`.
2. If `cell.x >= 0`: `RemoveUnit(self)`; `CanUnitMoveTo(cell)`; if blocked `FindOverlappingUnit` 0x5B7BB0 (units within 10 m, alive, closer than the radii sum and whose footprint stamp covers our cell); `AddUnit(self)`.
   Decision for a found blocker B (`level` = 1 terrain / 2 obstacle from `CanUnitMoveTo`):
   * B is a unit of the same player that follows us (`Brain::GetFollowTarget(B) == self`) and we issued an order < 2 ticks ago (`+0x93C`) -> **wait**;
   * we already have B as chain link (`+0x1D8` set, `+0x1DC == B`) -> **wait**;
   * both units are chasing the same target (same side, both orders in 3..5, same `orderTarget`, same move class, B not dying, target farther than sqrt(60) m^2) -> **follow-chain**: walk B's chain (`+0x1D8` flag, `+0x1DC` object);
     if it leads back to us the chain is cut and we **re-path** (neighbours stamped static); else we record B as our chain link and **wait**;
   * anything else: keep the `level` (1 = blocked by a standing unit -> re-path, 2 = blocked by a walking unit -> wait), except that when we are faster than B (`vt[59]` speed) and `level == 2` we **re-path** (`level = 1`, walking neighbours stamped as static).
   No blocker found (the stamps overlap but no unit is within the radii sum) -> move anyway.
3. `state == 0`, standing on a type-3 tile (`HasObjectType3AtPos`, e.g. in a spell zone), not fighting (`+0x174`), and the move started
   (`mover +0x4C startTick`) before the last object-volume change (`CAIMap +0x2694` = the world tick passed by `PlaceObjectVolumes` from the
   spell volumes `CSpellEffect::UpdateVolumes` 0x67EFC0) -> re-path [V, deep-read 2026-11-03]: a wall / acid fog / campfire placed after
   the route was planned makes walkers inside it re-plan.
4. Moved: `CollisionMap::MoveUnit` (restamp), yaw from `dir`, `CUnit +0x278 = (state == 0)` (re-registers the unit when it changes, `MarkNetDirty(7)`).
5. **Re-path** (level 1): clear the pending cells, `CAIMap::FindPath(self, goal +0x9C, moveState, maxLen +0xA4)` (after `CAIMap::PromoteWalkersToStanding` 0x5B7EF0 copies the obstacle grid 15 x 15 around us into the terrain level for the walking neighbours, removed again afterwards); failure -> `StopMove`, state 6 (`+0x145 = (partial flag == 1)`); success -> `CUnit::ResetMover` (new curve, replicated).
6. **Wait** (level 2): pause flag `+0x20` (`+0x21` for move type 2), the unit is re-registered as standing (`+0x278 = 0`, `MarkNetDirty(7)`). The next `TickAnimation` 0x546F30 sees the flag and calls `MoveRequest::ShiftTimes(+1)` (start tick and the kind-3 end ticks), the animation start ticks and `CUnit +0x258` + 1: time stands still for the unit, so the curve does **not** run ahead; `TickState` clears the flag at the start of the next tick [V].
7. `ended` (curve finished its chunk): `flags[0]` set -> arrived: `CMover::Reset`, `SetRect(stand at current pos)`, `ResetMover`, `SetSide(0)`, state 6 (idle). Else `ContinueMove`; refused -> same stop; ok -> `ResetMover` (next chunk).

Order completion: a move order (kind 1 / 2) is finished when `TickMovement` has put the unit into **state 6** (idle); `CUnitServer::Think` 0x54B390 (called from `Update` 0x552300 and `TickState` before the state
switch) then clears the order, and when `+0x145` (blocked / partial path) is set sends the player message 0xE ("cannot reach").

Arrival conditions: whole path delivered and curve ended; `DispatchQueuedOrder` pre-test (already within 0.14 m of the goal); `CUnitServer::ReportMoveResult` 0x54EB30 afterwards compares the final goal with the request: `|snapped goal - requested|^2 < 0.01 * |unit - requested|^2` is "reached" -> player message 1 else 0xE (blocked), and `CPlayerServer::SendMsg0E_MovePath` 0x66D490 sends the complete path (`ExportPath`) to the commanding client; `CPlayerClient::OnMsg0E_MovePath` 0x669FA0 stores it
into the client unit's request (and, with the debug flag `0x79BE70`, rebuilds the curve and spawns the path effects 0x2039 / 0x203A) [? exact purpose].

### 5.5 Chase, follow, group

* **Group move**: the click carries the selected units; `Formation_ComputeOffsets` 0x66FD70 gives each unit the unit vector from the group's centroid to itself (1 m), added to the target when `Brain::CanWalkBetween` finds a path to it; `FindFreeCellNear` then
  shifts colliding targets; `run`/"group" = count > 1.
* **Party cohesion** (`AiLogicPlayer`, brain kind 6): a hero's move order is held back while any unit that follows it is farther than 9 m (`Brain::IsNearLeader(run = 1)`, `FollowMaxDist * 2 + 1`); `AiLogicPlayer::Update` 0x5D4A90 (state 2)
  queues the order when the followers are within 6.6 m (`IsNearLeader(0)`, factor 1.4) and cancels it again with player message 0x21 when they fall behind during the walk (`IsNearLeader(1)` false). Followers
  (`Regroup` 0x5D4170, `Brain::TickFollow`) simply walk to the leader / its predicted position. [V from the two functions; the mode-2 exception: `CPlayerServer +0x5120 == 2` skips the check, meaning unknown]
* **Chase / attack approach** `CUnitServer::ApproachTarget` 0x54D140 [V, deep-read 2026-11-03] (every tick of combat orders): uniform-cost table (`CUnit +0x43C = 1`).
  *Unit target* (`+0x1A8 orderExact == 0`): when the target is still, in reach (`OrderReachDistance`) and `CanCastAt` accepts, just stand (state 2, idle anims).
  Otherwise: a moving target is predicted (`vt[52]` position at tick) at `nextAttackTick + 16`; if we would close the remaining `distance - reach` at half our
  speed (`vt[59]`) before then, wait standing. Else, when idle / standing or within 16 ticks of `approachRepathTick`: the chased object is the **chain partner**
  (`+0x1DC`) while it still attacks the same target with an order 3..5 and is not in state 5 (else the link is dropped); closing speed = our speed + the
  target's speed along its forward axis towards us (min 0.1); interception time `round((distance - reach) / closing)` clamped to 2..64 ticks; predicted
  point P at that tick. While walking (state 0) and P lies within `(distance + 1) x 0.1` of the current goal, only the path budget is refreshed
  (`maxLength = FollowDistance + 5`); else, with the chased object lifted off the collision map, `BeginMoveTo(P x 0.999 + own x 0.001, FollowDistance + 5)`
  (failure -> `+0x145 pathBlocked`); if the path's goal ends farther than `reach` from P the unit gives up for now: mover reset, stand facing forward, state 6,
  hit effect dropped, `pathBlocked`. `approachRepathTick` = the interception tick.
  *Point target* (`orderExact`): walk to `orderPos` (re-path unless already walking to a goal within 0.3 m), `BeginMoveTo(pos, 1e6)`; arrival within 0.3 m
  -> done; else the order is still kept when `CanReachTarget` and, with the unit placed at the path goal, `IsOrderTargetReachable`; otherwise message 0xB
  (attack) / 0xE (others), order cleared, `moveDone`.
* **Follow** `Brain::TickFollow` 0x5D6F90: every 8 ticks; moving target -> predicted position 5 ticks ahead, beyond `FollowMinDist`(2) walk to it, near it find a spot 1.365 m beside its predicted path; standing target -> move when farther than `FollowMaxDist`(4).
* **Flee / flank / random spot**: `AiLogicFear`, `Brain::FindRandomReachableSpot` 0x5D7F80 (path length <= 3 x distance + 10, slope <= 0.3), `Brain::TryAttackTarget` 0x5CF3C0 — all call `FindPath` for feasibility with the unit moved in the collision map.

## 6. Server vs client

| | server (`CWorldServer`, CUnitServer) | client (`CWorldClient`, CUnitClientSpecific) |
|---|---|---|
| AI map / grids / stamps | yes (CAIMap +0x170) | no [I] |
| path search | yes (`FindPath`) | none |
| MoveRequest | authoritative, rebuilt per chunk / re-path; `MarkNetDirty(10)` | received (section 10) |
| MoveCurve | built by `ResetMover` | built by `ReadNetState` / `OnMsg_UnitMoveState`; `ReplayMove` 0x535850 evaluates it every frame with the fractional time, z from the terrain, yaw smoothed `exp(-0.3 dt)`, animation speed = ground speed |
| position | `MoveUnit` each tick after the collision test | interpolated; no collision test |
| replicated | request (start, heading, tick, cells + factors), `+0x278` walking flag (section 7), stance (section 6) | |

## 7. What is not known

Settled in the T3 pass (2026-11-02; log in `_cpr/claude-re/full/t3-settled.tsv`):

* `Vec2Q64` is absolute: `(v + 32) * 60.234375` to u16 covers v in [-32, 1056) in steps of 1/60.23 (`65535 / 60.234375 = 1088`); the
  +32 is a shift, not a +-32 range. The request position is in tile units (metres * 2), so it covers -16..528 m, the same as `Pos3D`'s
  x / y range `(v + 16) * 120.46875` [V]. The delta codec bases are the previous state of the field (net-transport.md 5.6).
* `RotateSpeedMult`: `AiInfo_Load` reads `config\ai.reg` of the work dir (`main`, string 0x784898), not aiinfo.res; the shipped
  Universal-Mod/config/ai.reg (and the XP-Mates one) has no [RPG] RotateSpeedMult key, so the static default **100** applies [V]
  (parsed with the reg-format.md reader; the file holds DifficultyLevels, LMP, AddonPerk, Addon and RPG Perk Power Base 1.95).
* `NavPortalTables`: deterministic tables built once at static init from constants (no game state): `[inDir][outDir]` = how the 9
  window slots of one path cell connect to those of the next (shared cells, entry and exit edges); used by the corridor re-plan, 4.5 [I].
* `CUnit +0x274` = the dead flag (section 3.3: corpses are not stamped) [V]. Bridge heights for rendering: the client world's slots 29 /
  30 (`HeightWithBridges` 0x5618E0 / 0x561930: `CBridgeManager::GetHeightAt` 0x560F00 = highest walkable quad under the point, else the
  terrain; world-client-players.md 8) [V].
* `PathCells +0xE`: written by `RateFootprintPath`, sent by `MoveRequest::WriteNet` (bits 4-5) and stored by `ReadNet`; no other reader
  in the decompiles of the curve (0x5C6000-0x5C7FFF), the path code or the client unit [I]: transmitted, unused.
* A wait does shift the request start tick: `TickAnimation` -> `ShiftTimes(+1)` every paused tick (5.4 step 6, corrected) [V].
* `FindUnitsNear` 0x5C0BD0: the offset table (`CellOffsets_Build` 0x5BF8F0) holds the 21 x 21 sector offsets sorted by the gap distance
  `int(sqrt((|dx|-1)^2 + (|dy|-1)^2))`; the call takes the prefix with gap <= `int(radius * 0.03125)`. Sectors are 16 m (32 tiles), so the
  scan is only guaranteed to reach `16 * (int(radius / 32) + 1)` m: radii in [16, 32) m (and [48, 64), ...) can miss units in the far
  corner of the scanned square [V the arithmetic, I the effect]. `RefreshNearbyUnits` scales correctly (`radius * 2 * 0.03125`, tile
  units). Live check in `live-checklist.tsv`.
* Stamp rule: dead units are not stamped (+0x274) [V]; units removed from the world are not in the collision map at all [I].
  `BeginPaint / EndPaint` (now `BeginAIMapBatch` / `EndAIMapBatch`) have no reference in the exe: the editors' use is [untestable].

## 8. Corrections to earlier notes

1. `docs/pathfinding.md`: "`+0x6D` no path / partial" is right, but the mover's `+0x68` is the **max path length** (order z), not a speed; the mover is three objects (above).
2. `DispatchQueuedOrder` kind 2 is **turn in place**, not "direct move"; `QueueOrderMoveTo2` queues a turn.
3. `docs/game-memory.md`: order `z = 1000000` = max path length; the "x 10 = m/s" for `+0x2B8` is approximate (speed unit = tiles per tick).
4. `full/structs.tsv` `CMover` (size 0xA4) merged the server move state (0x88) with the curve (0xA4); `batch-pathfinding-structs.tsv` separates them.
5. Moving units are invisible to the planner; only standing units (and the units' own removal) matter, which explains units "walking through" each other until they collide and wait.

Deep-read round 2026-11-06 (log `_cpr/claude-re/full/t5-read-ai-combat.tsv`):

6. This page 4.3 (`FindFreeCellNear`): "searches along the 8 rotated directions ... the tile centre nudged 0.49 toward the goal" -> the candidates are
   `goalTile - round(d x w)` (first back toward the unit, then +-45, +-90, +-135, 180 deg), and the result sits 0.01 tile inside the candidate's edge
   facing the goal tile (`c + 0.01` / `c + 0.99`).
7. This page 4.3: "one-cell path (flag 0x200)" -> 0x200 is the cell's speed factor (1.0); a snapped goal on the unit's exact position is a failure;
   the mover `+0x60..+0x68` hold the goal's x, y **and z** (`+0x68` is "the max length" only because orders store it in the z).
8. This page 4.4: "If a waypoint is not reached the 21 x 21 neighbours are tried" -> the nearest **already reached** tile around it is taken as a
   substitute waypoint; the 15-tile retry test is the octile distance start -> last reached waypoint; the blocked nav cell is the one before the
   failing waypoint's cell.
9. This page 4.1: "`ShiftWindow` ... border-crossing nodes are re-opened; `keepReached` decides whether already-closed nodes stay closed" -> flag 0
   keeps every closed node closed and re-queues the open ones; flag 1 re-opens every node at least as costly as the cheapest node of the old window's
   edge ring; the shift fails when the origin moves >= 31 tiles.
10. This page 3.3: object faces "sampled ~3 per tile" -> `round(3 x edge length in metres) + 2` samples per edge (~1.5 per tile).
11. This page 5.3: "a corner point is inserted when the direction changes by >= 90 deg" -> every diagonal step adds the corner it cuts; straight steps
    add their exit-edge midpoint only next to a >= 90 deg change; kind 4 turns ignore the request's speed.
12. This page 2 / 9: `ChooseExitDirection 0x5C46C0` -> `SmoothPathStart` (one function, the corridor re-plan); the mover's `+0x84` "int" is its
    returned cost, and that cost is what `AiLogicFear::Evaluate` and `FindRandomReachableSpot` compare with `10000 x d` (unit-ai.md 6.8 / 8.4).

## 9. Function index (addresses)

CAIMap data: Ctor 0x5B3C30, Load 0x5B43C0, Dtor 0x5B5E10, InitCostTables 0x5B6070, InitStepTables 0x5B4150, PackMaterialCosts 0x5B8620, AddCellObject 0x5B61B0, RemoveCellObject 0x5B6380, SetBridgeCells 0x5B6740, StampObject 0x5B6A80,
PlaceObjectVolumes 0x5B7820, RemoveObjectVolumes 0x5B78F0, CObject3D::BuildFromAnimation 0x5179E0 (part flags), UpdatePassability 0x5B8FA0, GetCellStepCost 0x5B8BA0, MarkSteepCells 0x5B8710, ClearPassabilityClass 0x5B89F0. Queries: CanUnitStandAt 0x5B80F0, CanUnitMoveTo 0x5B81B0, CanUnitBeAt 0x5B7A40,
FindOverlappingUnit 0x5B7BB0, IsUnitPositionWalkable 0x5BA5A0, IsUnitCellPassable 0x5BA6E0, IsCellPassableClass 0x5BA790, HasPassableNeighbour 0x5BA830, IsCellWater 0x5BBC70, HasObjectType3At(Pos) 0x5BBD10 / 0x5BBDB0, GetMaxSlopeAt 0x5BC2A0, TraceVisibility 0x5BC3E0.
Stamps: StampMap Ctor 0x5B4110, AddStamp 0x5B36B0, RemoveOwner 0x5B3890, appliers 0x5B39D0 / 0x5B3A70 (terrain) and 0x5B3B20 / 0x5B3B90 (obstacle), FootprintStamp Ctor 0x5B3440, GetCached 0x5B8350, StampUnitFootprint 0x5B7EF0.
Collision map: Ctor 0x5C02E0, Resize 0x5C06F0, AddUnit 0x5C0DF0, RemoveUnit 0x5C1010, MoveUnit 0x5C11D0, FindUnitsNear 0x5C0BD0. Path: FindPath 0x5BB350, FindLongPath 0x5B9C90, MeetLocalSearches 0x5B9690, StepAlongPath 0x5BB190,
ContinueMove 0x5BB900, RateFootprintPath 0x5BA400, FindFreeCellNear 0x5BAC10, FindNearestReachableCell 0x5BA9D0, SmoothPathStart 0x5C46C0. PathFinder 0x5C1D50-0x5C35F0; NavGrid / PathSearch 0x5AED60-0x5B1380 (Build 0x5AFC70, FindPath 0x5B0D90, Read 0x5B1180, Write 0x5B1380).
Movement: MoveRequest 0x5C7B00-0x5C7D50, wire 0x5C7DB0 / 0x5C7FC0, CMover 0x5B1DE0-0x5B2570, MoveCurve 0x5C6270-0x5C79B0. Server: BeginMoveTo 0x548700, TickMovement 0x547890, DispatchQueuedOrder 0x54A900, ApproachTarget 0x54D140, MoveToWithParty 0x554830,
QueueOrderMoveTo 0x554740, OnMoveFinished 0x54EB30, UpdateStance 0x548520; unit: CUnit::ResetMover 0x52E9D0, GetSpeeds 0x52F920, UpdateMoveParams 0x530570, ComputeAnimSpeedClasses 0x552FB0; client: ReplayMove 0x535850, ReadNetState 0x52EF90.
