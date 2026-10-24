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
| map size | `width = sectors * 64`, `height likewise` at `CAIMap +0x26A0 / +0x26A4`; sector = 32 m = 64 tiles |
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
     ChooseExitDirection 0x5C46C0 (start smoothing) -> StepAlongPath 0x5BB190 (first <= 32 cells -> MoveRequest kind 2)
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

Order kinds that move (CUnitServer +0x190 / queued +0x1B4): 1 = walk to a point; **2 = turn in place to face a direction** (`QueueOrderMoveTo2` 0x5546B0, pos = direction
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
| **obstacle (walking units)** | obj +0x1F4, rows +0x1F8 | u8 level | `ObstacleStampApplier` 0x5B3B20/0x5B3B90 (StampMap at +0x12A4) | `CanUnitStandAt`/`CanUnitMoveTo` (level 2), `StampUnitFootprint` 0x5B7EF0 |
| **cell-object records** | +0x14C (floors/obstacles) and +0x1A0 (bridges); 24-byte records, 16 hash heads at +0x15C / +0x1B0 | `{x, y, zmin, zmax, type u8 @+0x10, flag u8 @+0x11, border u8 @+0x12, next @+0x14}` | `StampObject` 0x5B6A80, `CSpellEffect::UpdateVolumes` 0x67EFC0 (via `CollectCircleCells` 0x5B2760 / `CollectHalfCircleCells` 0x5B2A00, type 3) | `AddCellObjects` 0x5B65B0, `SetBridgeCells` 0x5B6740 |
| **sector entry chains** | `CollisionMap +0x14` sectors (below) at CAIMap +0x138, 0x54 bytes each | entries of 8 bytes `{u16 zmax, u16 zmin, u8 type, u8 flag, i16 next}` in the sector's CArray (+0x44), free chain +0x3C | `NavSector::AllocObjectEntry` 0x5BFE70 / `UnlinkObjectEntry` 0x5BFFD0 | `GetCellStepCost` |
| **bridge hash** | +0x10C (ScriptRefCacheHash, buckets) | key `x << 16 | y` -> saved original `{height, flags}` of a cell turned into a bridge | `SetBridgeCells`, `StampHash_TakeInto` 0x5B30B0 | `GetWalkHeightAt` 0x5BBA10, `ShortPairHash_Find` 0x5B3160 |
| **NavGrid (AIGRAPH)** | +0x22F8, size 0x29C | see section 4.2 | `NavGrid_Read` 0x5B1180 from the `.mob` (and `NavGrid_Build` 0x5AFC70 on dynamic changes) | `NavGrid_FindPath` 0x5B0D90, `DominantRegion`, `IsCellInRegion` |
| **StimulusMap** | +0x26C8 | | hearing / vision (module 0x5C8000) | not movement |

### 3.1 The terrain cell (9 bytes) [V]

| byte | meaning |
|---|---|
| +0 u32 | 8 nibbles; nibble k (bits 4k..4k+3) = passability of unit class k: **0 blocked, 1..15 usable** (15 best). 7..15 are "walkable" (cost factor < 12000), 1..6 are swamp-like (factor 12000..36000); `IsUnitPositionWalkable` 0x5BA5A0 accepts only a step cost 1..11999 |
| +4 u16 | ground height in steps of `maxZ/511` (`CAIMap +0x120 = 511/maxZ` is the scale; `GetTerrainHeight` 0x5B77F0 divides by it). Bridge cells hold the bridge top (`SetBridgeCells`) |
| +6 u16 | bits 0-5 water depth (0x3F = very deep), **bit 6 (0x40) cell is a bridge / platform** (original data saved in the bridge hash), bit 7 (0x80) water. The high byte is preserved by `TerrainGrid_Alloc` [?] |
| +8 u8 | **standing-unit level** (see 3.3). 0 on a freshly loaded map |

The border: `UpdatePassability` forces the 3 outermost rows / columns to nibble 0 (units can't leave the map); `PathFinder::Init`'s 34 x 34 window is closed on its edge.

### 3.2 How the nibbles are computed (`UpdatePassability` 0x5B8FA0, `GetCellStepCost` 0x5B8BA0, `MarkSteepCells` 0x5B8710) [V]

Called for a rectangle (+3 margin) after load and after every object change. For each of the 8 classes k:

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
   margin), the units are put back. `navGridOn` is `CWorldServer +0x172`; set after a zone/save is loaded, cleared during loading (`CWorldServer::BeginPaint/EndPaint` 0x595A90/0x595A70,
   misnamed: see names file) and for the duration of `ApproachTarget` / `ValidateQueuedOrder`.

The class an individual unit uses (`CUnitServer::v23_ComputeAnimSpeedClasses` 0x552FB0, [V]): size `S = CUnit::GetSize`, speed `s = proto[+0x120] * field14C`:
S <= 0.85: class 1 if s < 0.6, 2 if < 1.2, 3 if < 1.9, else 4; S <= 1.5: class 5 (s <= 1.7) or 6; S > 1.5: class 7. Stored `+0x248` (walk / run), `+0x24C`
(crawl, uses 0.2 s) and `+0x24E` (sneak, 0.6 s); `UpdateMoveParams` 0x530570 copies the right one into `CUnit +0x324` by stance. Class 0 = flying / floating units (server
`+0x240 == 2`, `+0x33F == 2`); they use the "class 0" slope table and treat water as ground.

### 3.3 The unit footprint stamps and the two levels [V]

* `CollisionMap::AddUnit` 0x5C0DF0 / `RemoveUnit` 0x5C1010 / `MoveUnit` 0x5C11D0 (the map at CAIMap +0x124, global 0x7B01F4; sectors of 32 x 32 tiles, 0x54 bytes each, a unit list at
  sector +0x20 and an object list at +4; the unit's tile is cached in `CUnit +0x1B8/+0x1BC`, flag 0x8000 in `CUnit +8`) call `CAIMap::StampObject` 0x5B6A80.
* For a unit (type id 0x50) `StampObject` takes `FootprintStamp_GetCached(radius)` 0x5B8350 (cache keyed by radius rounded to 0.1; `FootprintStamp::Ctor` 0x5B3440:
  16 x 16 bytes, value `max(0, round((r + 2 - dist/2) * 16))`, `dist` in tiles from the centre tile (8, 8)) and `StampMap::AddStamp` 0x5B36B0 / `RemoveOwner` 0x5B3890 it at `tile - 8`.
  A `StampMap` keeps 4 x 4 blocks in 1024 buckets keyed `(by + bx*17) & 0x3FF`; adding **max**-merges (applier vt[1]), removing deletes the owner's blocks and **re-sets** the cell
  block to the max of the remaining owners (vt[0]).
* **Which grid**: `CUnit::Get278` (`+0x278` = "walking": set by `TickMovement` when state == 0, cleared by `StopMove`) selects the `StampMap` at **+0x12A4 -> obstacle grid** for
  walking units and **+0x27C -> terrain byte +8** for standing ones. A unit with `+0x274` set (vt[55]; `GetDisplayPos` uses the same slot to switch to an attach point, so probably "attached to / carried by something") is not stamped at all [?].
* **Fit test** `CAIMap::CanUnitStandAt` 0x5B80F0: `R = max(0, round((2 - radius) * 16))`; fits when `terrain.b8 <= R` (else blocked at *level 1*) and `obstacle <= R` (else *level 2*).
  The stamps are exactly `(r_other + 2 - d)*16` so the test is `d >= r_other + r_self` (a circle test quantised to tiles).
  `CanUnitMoveTo` 0x5B81B0 additionally accepts a cell that is no tighter than the unit's current cell (so a unit already inside a stamp can leave).
* **The path finders only read the terrain byte (standing units), never the obstacle grid**: walking units are invisible to the planner. During planning the unit itself is removed from the
  map (`FindPath` does `RemoveUnit` ... `AddUnit`), and `CUnitServer::CollectPartyFollowers` 0x5468D0 additionally removes the player's followers and moving party members near the
  target so a group does not block itself.
* `PathFinder::SaveFreeRadius` 0x5C33F0 / `WriteFreeRadius` 0x5C3530 clear the byte +8 of the window for nav-level work (so nav connectivity ignores units) and restore it.
* Static objects do **not** use stamps: they become volumes (`cell-object records`) via `StampObject`: each body part (`OBJ_BODYPARTS`; part flags: bits 3-5 type, 6-9 flag, 0x400 bridge,
  0x800 skip) box faces are sampled ~3 per tile and merged per tile (`AddUnitStamp` 0x5B2D10 keeps the highest top sample, `AddObjectStamp` 0x5B2E10 the lowest bottom/side sample);
  `BASE` parts are floors (tile height raised, material ROAD, bit 6 set via `SetBridgeCells`), `CROWN` ignored. Exact rules: aigraph.md "Objects".

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
* `ShiftWindow` 0x5C25D0 slides the window (< 31 tiles) keeping costs: border-crossing nodes are re-opened; `keepReached` decides whether already-closed nodes stay closed.
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

1. reject if `goal.x^2 + goal.y^2 > 1e8`; store the requested goal in the mover (`+0x60..+0x68`).
2. `CollisionMap::RemoveUnit(self)`. `FindFreeCellNear(unit, goal, checkRegion = 0, minDist)` 0x5BAC10: if the unit can stand at the goal tile (nibble != 0, `CanUnitStandAt`) it stays, else a search along the 8 rotated
   directions of the unit->goal line with growing distance (from `minDist*2 - 1`, up to the line's length) for a tile it can stand on (and, with `checkRegion`, that `IsCellInRegion` accepts); the result is the tile centre
   nudged 0.49 toward the original goal. Stored as the snapped goal (`+0x58/+0x5C`).
3. same tile as the unit -> one-cell path (flag 0x200; only if the unit can stand there and its nibble != 0). Octile distance `|dx - dy| + 1.414 * min < 25` tiles and both `PathFinder::BeginForUnit` succeed -> `MeetLocalSearches` 0x5B9690 (forward from the unit, backward from the goal,
   alternate `Expand` until one's current node is reached by the other; extract, reverse, join, de-duplicate).
4. otherwise (or on a miss) `FindFreeCellNear(checkRegion = 1)` again, then `FindLongPath` 0x5B9C90 with `allowRetry = (mover.+0x6E || mover.+0x6F)` (AI units, or the first request after a reset).
5. `ChooseExitDirection` 0x5C46C0 (see 4.5) with the unit's facing direction (8 directions); `StepAlongPath` 0x5BB190; `CollisionMap::AddUnit(self)`; on failure mover `+0x6D = 1`; `+0x6F = 0`.

### 4.4 `FindLongPath` 0x5B9C90 [V]

`NavGrid_FindPath(class, start tile, goal tile, blocked cells)`; if it fails -> fail. If `navCells * 8 > maxLength` (in tiles) -> fail. Else clear the direction grid around the nav path (`ClearOccupancyAround`),
turn every nav cell into its representative tile (B nibbles) and append the goal tile; walk the chain with **one** `PathFinder`: `BeginForUnit(start, wp1)`, `RunUntil(wp1)`, `WriteOccupancy`, then `ShiftWindow(prev wp, next wp)`
for each further waypoint. If a waypoint is not reached the 21 x 21 neighbours (sorted by distance, `CellOffsets_Build` 0x5BF8F0) are tried; at the last waypoint the search is run to the end if the goal is still unreached.
The tile path is read back with `ExtractOccupancyPath` 0x5B9A20 (follows the direction grid from the goal to the 0 start cell). If the walk dies **within 15 tiles** of the start and `allowRetry`: the failing nav cell is appended to the mover's
blocked list (`+0x70..`) and the whole function recurses (a unit-blocked corridor is routed around). If it dies farther away: the path to the last reached tile is kept, the mover's snapped goal becomes that tile and
`+0x6D = 1` ("partial, continue later"). The nav search ignores units; the tile windows see the standing ones.

### 4.5 Start smoothing, chunking, speeds

* `ChooseExitDirection` 0x5C46C0 (for paths of >= 3 cells): re-plans the first cells with a heading penalty (each 45 deg beyond the first costs 600, diagonal moves get `(1 - straightness) * 2048`) over the tables built by
  `NavPortalTables_Build` 0x5C42B0 / `NavGraph::RelaxPortal` 0x5C45D0, and replaces the leading cells when cheaper. [V that it exists and what it feeds; the portal tables' exact meaning is only partly decoded [?]]
* `StepAlongPath` 0x5BB190: `MoveRequest::Clear`, `SetRect(unit pos, facing, tick - 1)`, moves **up to 32 cells** (`0x20`) from the pending list (`CMover +0x3C`) into the request's list (`+0x20`). All delivered ->
  `flags[0] (+0x6C) = 1` and the request target is the exact snapped goal; else `flags[0] = 0`, the last cell is pushed back to the front of the pending list (so the next chunk starts there) and the request target is that cell's centre.
  Then `RateFootprintPath` 0x5BA400 stamps each cell node: `+0xC` = speed factor `= (table94[nibble(next)] << 9) / slope(h(next) - h(cur))` (512 = 1.0; 0x200 if the slope is blocked), `+0xE` = bit0 water (`terrain +6 bit 7`),
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

`SetRect` 0x5C7B00 = kind 0 (stand at pos with heading and tick; returns whether anything changed), `SetTurnTo` 0x5C7C00 kind 1, `0x5C7C70` kind 2 target, `0x5C7C20` kind 3 (`ResetWithTarget2`, used by `StartAttackAnim2` to face a target),
`SetTurnToTimed` 0x5C7C40 kind 4, `Clear` 0x5C7D50, `ClearTargetIf(id)` 0x5C7C90 (kind 3 with that id -> -1; called before every curve rebuild with the unit's own id), `ShiftIds` 0x5C7CB0 (adds a delta to the start tick and, for kind 3,
the two end ticks; the client applies -1 when `CUnit +0x260 == 1` [?]).

**Wire format** (`MoveState_WriteNet` 0x5C7DB0 / `ReadUnitOrder` 0x5C7FC0, net section 10 of the unit; `CUnit::WriteNetState` 0x52EC70 / `ReadNetState` 0x52EF90): u8 kind, u8 count high, u8 count low, Vec2 (+4) quantised
(`Q64`: 16 bit each, step 1/60.2), heading as 16-bit range -pi..pi, then +0x10, +0x14, +0x18, +0x1C as raw dwords, then per cell one u16 `bits 0-1 = x code (0: x-1, 1: same, 2: x+1, 3: explicit i16 follows), bits 2-3 = y code the same,
bits 4-5 = flags & 3, bits 6-15 = speedFactor clamped to 0x3FF` plus the explicit shorts. The stream is delta-coded against the previous state (DeltaCodec). A section-10 read does `ReadUnitOrder`, `ClearTargetIf(own id)`,
`GetSpeeds` -> `CUnit +0x2B8/+0x2BC`, `CMover::BuildPath` and `MarkNetDirty(10)` again (relay). Also message 0x0E (below).

### 5.2 `CMover` (server, 0x88) [V]

+0x3C pending cell list (vtable, head +0x40, tail +0x44, count +0x48, free +0x4C, plex +0x50, block +0x54), **+0x58/+0x5C snapped goal** (tile units, f32), **+0x60/+0x64 requested goal x, y (metres)**, **+0x68 max path length** (the order's z),
flags **+0x6C "whole path delivered"**, **+0x6D "partial / no path: re-plan from the stored goal when the chunk ends"**, **+0x6E "AI unit" (no player)**, **+0x6F "first request after reset"**, +0x70 blocked nav-cell list (CArray of `{x, y}` pairs: vtable +0x70, data +0x74,
count +0x78, cap +0x7C), +0x84 int. In CUnitServer terms: goal `+0x9C`, length `+0xA4`, delivered `+0xA8`, partial `+0xA9`, AI `+0xAA`. `Reset` 0x5B23A0, `ResetForUnit` 0x5B2190, `ResetWithTarget*` 0x5B1DE0 / 0x5B1F10 / 0x5B2050
(reset + request kind 1 / 3 / 4), `ExportPath` 0x5B22C0 (request + all cells incl. pending, speed factors forced to 0x200, for the report message), `ReadState/WriteState` (save games).

### 5.3 `MoveCurve` (0xA4 at CUnit +0x27C) [V from `BuildPath` / `Evaluate` / `Predict`]

Layout: +0 vtable, +4/+8/+0xC/+0x10 segment array (0x1C-byte `{x, y, dirx, diry, speed, cellx, celly}`, count, cap, grow), +0x14 vtable, +0x18/+0x1C.. point array (0xC-byte `{x, y, runLength}`), +0x28 vtable, +0x2C/+0x30.. cell speed array (f32),
**+0x3C base speed** (= `CUnit +0x2B8`, `GetSpeeds` a), **+0x40 turn rate** (= `CUnit +0x2BC`, `GetSpeeds` b = run-speed stat * `AiInfo.RotateSpeedMult` (+0x40 of the AiInfo block at 0x7C53F8, global 0x7C5438; the exe's initial value is 100, the [Logic] section may override it [?]);
with 100 a turn of pi takes a small fraction of a tick, i.e. effectively instantaneous), +0x44 current turn speed, +0x48 start heading, +0x4C turn delta, +0x50..+0x74 the cached segment (start xy, start dir, end xy, end dir, length, speed), +0x78 X cubic (4 f32), +0x88 Y cubic, +0x98 / +0x9C
segment start / end time, +0xA0 cached segment index (`CUnit +0x31C`, -1 = none).

`CMover::BuildPath` 0x5C67D0(request):
* kind 0 / 3: nothing; kind 1 / 4: turn delta = wrapped (angle - heading), turn speed from the request or `+0x40`.
* kind 2: **points**: start position first; for each cell the current run's counter is incremented and the cell's speed factor appended; direction `D` of the step to the next cell via table 0x791988; a corner point is inserted
  when the direction changes by >= 2 steps (>= 90 deg) (positions at cell edge midpoints / corners depending on `D`), otherwise the run continues; the request goal (`+0x14/+0x18`) is the last point. `BuildSegments` 0x5C66C0 /
  `AddSegmentsBetween` 0x5C6370: between consecutive points, `count` segments at fractions `(i + 0.5 + off0) / (count + off0 - off1)` (`off0 = -0.5` at the path start, `off1 = +0.5` at its end), all with the run's unit direction and
  `speed = cellSpeed[i] * baseSpeed / 512`; a one-run path is just two end points. `LimitSegmentSpeeds` 0x5C6290: a segment's speed is capped so traversing it takes at least `turnAngle / turnRate`.
  `CopySegmentCells` 0x5C6750 copies the request cells' x / y into the segments.
* initial turn: if the first segment's direction is within cos > 0.9 (~26 deg) of the heading the first direction is replaced by the heading and there is no turn, otherwise a turn in place by the wrapped angle precedes the path.

`Evaluate` 0x5C6FC0 (t = ticks since `startTick`): before the turn ends -> heading only (`turning = 1`); otherwise find the segment (cached), `Curve_Init` 0x5C73B0 builds the cubic `p(s) = p0 + d0 s + a2 s^2 + a3 s^3` with
`p(len) = p1`, `p'(len) = d1` (Hermite in **distance**, `s = (t - t0) * segmentSpeed`, `t0` accumulating `len / speed`), position = `(Poly3_Eval(X), Poly3_Eval(Y))`, direction = the derivative; returns the *next* segment's cell as "the
cell being entered"; past the last segment: the end point, speed 0, cell = the last segment's. `Mover::Predict` 0x5C7430 switches on the request kind (kind 3: heading interpolated from the start heading to the target's bearing between
start and end tick, or the bearing; position fixed) and clamps `t` to `startTick`. Speed units: tile units per tick (walk ~0.2 -> ~1.9 m/s at 18 ticks/s; docs/game-memory.md's "x 10 = m/s" is ~10 % high) [I].

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
3. `state == 0`, standing on a type-3 tile (`HasObjectType3AtPos`, e.g. in a spell zone), not in attack mode and a counter `+0x4C < world +0x2804` [? meaning] -> re-path.
4. Moved: `CollisionMap::MoveUnit` (restamp), yaw from `dir`, `CUnit +0x278 = (state == 0)` (re-registers the unit when it changes, `MarkNetDirty(7)`).
5. **Re-path** (level 1): clear the pending cells, `CAIMap::FindPath(self, goal +0x9C, moveState, maxLen +0xA4)` (after `StampUnitFootprint` 0x5B7EF0 copies the obstacle grid 15 x 15 around us into the terrain level for the walking neighbours, removed again afterwards); failure -> `StopMove`, state 6 (`+0x145 = (partial flag == 1)`); success -> `CUnit::ResetMover` (new curve, replicated).
6. **Wait** (level 2): pause flag `+0x20/+0x21` (animation), the unit is re-registered as standing (`+0x278 = 0`, `MarkNetDirty(7)`). The curve keeps running in time, so after a wait the evaluated position is *ahead* of the unit (it catches up in one step if free) [I].
7. `ended` (curve finished its chunk): `flags[0]` set -> arrived: `CMover::Reset`, `SetRect(stand at current pos)`, `ResetMover`, `SetSide(0)`, state 6 (idle). Else `ContinueMove`; refused -> same stop; ok -> `ResetMover` (next chunk).

Order completion: a move order (kind 1 / 2) is finished when `TickMovement` has put the unit into **state 6** (idle); `CUnitServer::Think` 0x54B390 (called from `Update` 0x552300 and `TickState` before the state
switch) then clears the order, and when `+0x145` (blocked / partial path) is set sends the player message 0xE ("cannot reach").

Arrival conditions: whole path delivered and curve ended; `DispatchQueuedOrder` pre-test (already within 0.14 m of the goal); `OnMoveFinished` 0x54EB30 afterwards compares the final goal with the request: `|snapped goal - requested|^2 < 0.01 * |unit - requested|^2` is "reached" -> player message 1 else 0xE (blocked), and `CPlayerServer::SendMsg0E_MoveState` 0x66D490 sends the complete path (`ExportPath`) to the commanding client; `CPlayerClient::OnMsg_UnitMoveState` 0x669FA0 stores it
into the client unit's request (and, with the debug flag `0x79BE70`, rebuilds the curve and spawns the path effects 0x2039 / 0x203A) [? exact purpose].

### 5.5 Chase, follow, group

* **Group move**: the click carries the selected units; `Formation_ComputeOffsets` 0x66FD70 gives each unit the unit vector from the group's centroid to itself (1 m), added to the target when `Brain::CanWalkBetween` finds a path to it; `FindFreeCellNear` then
  shifts colliding targets; `run`/"group" = count > 1.
* **Party cohesion** (`AiLogicPlayer`, brain kind 6): a hero's move order is held back while any unit that follows it is farther than 9 m (`Brain::IsNearLeader(run = 1)`, `FollowMaxDist * 2 + 1`); `AiLogicPlayer::Update` 0x5D4A90 (state 2)
  queues the order when the followers are within 6.6 m (`IsNearLeader(0)`, factor 1.4) and cancels it again with player message 0x21 when they fall behind during the walk (`IsNearLeader(1)` false). Followers
  (`Regroup` 0x5D4170, `Brain::TickFollow`) simply walk to the leader / its predicted position. [V from the two functions; the mode-2 exception: `CPlayerServer +0x5120 == 2` skips the check, meaning unknown]
* **Chase / attack approach** `CUnitServer::ApproachTarget` 0x54D140 (every tick of combat orders): uniform-cost table (`CUnit +0x43C = 1`); target out of reach -> predict its position with `vt[52]` at the end of our attack window, if it will come into reach at our speed
  just wait (state 2), else (every 16 ticks, or when idle) re-path to the interception point `distance - reach / (our speed + target's radial speed)` clamped to 2..64 ticks, with the target temporarily removed from the map; `BeginMoveTo(point*0.999 + own*0.001, FollowDistance + 5)`.
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

* `Vec2Q64` range / base: 16 bits over +-32 cannot be an absolute coordinate; the delta codec's base semantics are unresolved [?].
* The exact value of `RotateSpeedMult` in the shipped aiinfo ([Logic] block read by `AiInfo_Load` 0x5CE0F0 from a registry I did not locate; the `aiinfo.res` archive holds `tileDesc.reg`, `units.reg`, `normal.dat` only).
* `NavPortalTables` (0x5C3690-0x5C4440, `ChooseExitDirection`'s portal tables): construction read, meaning only partly understood.
* `CUnit +0x274` (vt[55]) meaning; bridge heights for rendering (`GetWalkHeightAt` has no callers; the z of walkers comes from the world height function vt[27]).
* The `PathCells` flag +0xE bits' effect on the client (animation?).
* Whether a pause (wait) shifts the request's start tick (no code found that does; see 5.4 step 6).
* `CollisionMap::FindUnitsNear` radius classes (the `* 0.03125` radius scaling and the offset table lookup were read but not verified against sector size).
* The unit stamp rule for dead / hidden units, and the editors' use of `BeginPaint/EndPaint`.

## 8. Corrections to earlier notes

1. `docs/pathfinding.md`: "`+0x6D` no path / partial" is right, but the mover's `+0x68` is the **max path length** (order z), not a speed; the mover is three objects (above).
2. `DispatchQueuedOrder` kind 2 is **turn in place**, not "direct move"; `QueueOrderMoveTo2` queues a turn.
3. `docs/game-memory.md`: order `z = 1000000` = max path length; the "x 10 = m/s" for `+0x2B8` is approximate (speed unit = tiles per tick).
4. `full/structs.tsv` `CMover` (size 0xA4) merged the server move state (0x88) with the curve (0xA4); `batch-pathfinding-structs.tsv` separates them.
5. Moving units are invisible to the planner; only standing units (and the units' own removal) matter, which explains units "walking through" each other until they collide and wait.

## 9. Function index (addresses)

CAIMap data: Ctor 0x5B3C30, Load 0x5B43C0, Dtor 0x5B5E10, InitCostTables 0x5B6070, InitStepTables 0x5B4150, PackMaterialCosts 0x5B8620, AddCellObject 0x5B61B0, RemoveCellObject 0x5B6380, SetBridgeCells 0x5B6740, StampObject 0x5B6A80,
PlaceObjectVolumes 0x5B7820, RemoveObjectVolumes 0x5B78F0, UpdatePassability 0x5B8FA0, GetCellStepCost 0x5B8BA0, MarkSteepCells 0x5B8710, ClearPassabilityClass 0x5B89F0. Queries: CanUnitStandAt 0x5B80F0, CanUnitMoveTo 0x5B81B0, CanUnitBeAt 0x5B7A40,
FindOverlappingUnit 0x5B7BB0, IsUnitPositionWalkable 0x5BA5A0, IsUnitCellPassable 0x5BA6E0, IsCellPassableClass 0x5BA790, HasPassableNeighbour 0x5BA830, IsCellWater 0x5BBC70, HasObjectType3At(Pos) 0x5BBD10 / 0x5BBDB0, GetMaxSlopeAt 0x5BC2A0, TraceVisibility 0x5BC3E0.
Stamps: StampMap Ctor 0x5B4110, AddStamp 0x5B36B0, RemoveOwner 0x5B3890, appliers 0x5B39D0 / 0x5B3A70 (terrain) and 0x5B3B20 / 0x5B3B90 (obstacle), FootprintStamp Ctor 0x5B3440, GetCached 0x5B8350, StampUnitFootprint 0x5B7EF0.
Collision map: Ctor 0x5C02E0, Resize 0x5C06F0, AddUnit 0x5C0DF0, RemoveUnit 0x5C1010, MoveUnit 0x5C11D0, FindUnitsNear 0x5C0BD0. Path: FindPath 0x5BB350, FindLongPath 0x5B9C90, MeetLocalSearches 0x5B9690, StepAlongPath 0x5BB190,
ContinueMove 0x5BB900, RateFootprintPath 0x5BA400, FindFreeCellNear 0x5BAC10, FindNearestReachableCell 0x5BA9D0, ChooseExitDirection 0x5C46C0. PathFinder 0x5C1D50-0x5C35F0; NavGrid / PathSearch 0x5AED60-0x5B1380 (Build 0x5AFC70, FindPath 0x5B0D90, Read 0x5B1180, Write 0x5B1380).
Movement: MoveRequest 0x5C7B00-0x5C7D50, wire 0x5C7DB0 / 0x5C7FC0, CMover 0x5B1DE0-0x5B2570, MoveCurve 0x5C6270-0x5C79B0. Server: BeginMoveTo 0x548700, TickMovement 0x547890, DispatchQueuedOrder 0x54A900, ApproachTarget 0x54D140, MoveToWithParty 0x554830,
QueueOrderMoveTo 0x554740, OnMoveFinished 0x54EB30, UpdateStance 0x548520; unit: CUnit::ResetMover 0x52E9D0, GetSpeeds 0x52F920, UpdateMoveParams 0x530570, ComputeAnimSpeedClasses 0x552FB0; client: ReplayMove 0x535850, ReadNetState 0x52EF90.
