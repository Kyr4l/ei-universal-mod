# How a unit finds its way and walks (game.exe, read 2026-10-20)

For the AI player, um-engine and the map tools. Addresses are the Russian `EIStarter OBT-1/Engine/game.exe`. The navmesh the
game stores in a zone's `.mob` is described in `file-formats/aigraph.md` (what it holds, how to generate it); this page is the
runtime: the grids behind it, the searches that use it, and the mover that follows the result. All of it is `CAIMap` (the
global at 0x7B00D0), `CollisionMap`, `PathFinder` / `PathSearch`, `NavGrid` and `CMover`, in the module 0x5AD000-0x5C8000.

## The grids (`CAIMap::Load` 0x5B43C0)

Built when a map loads from the sectors' terrain (`<map>_3NNN_3NNN` sector resources + `tileDesc.reg` of `aiinfo.res`), 64 x 64
cells per sector, a cell = 0.5 x 0.5 m (`width = sectors * 64`, `height` likewise; `CAIMap` +0x26A0 / +0x26A4):

| Grid | Offset | Cell |
|---|---|---|
| terrain | +0x269C (rows) | 9 bytes: +0 dword, 8 passability nibbles, one per unit class (class k = bits 4k..4k+3, 0 = blocked, 1..15 = how fast / cheap); +4 u16 height (steps of `maxZ / 511`, `heightScale` +0x120); +6 u16: bits 0-5 water depth, bit 6 bridge / platform, bit 7 water; +8 byte: the free-radius level a unit of a given radius needs |
| flags | +0x26AC | dword: bits 0-4 material (index into the 0x118-byte `tileDesc.reg` records at +0xFC: Speed, CostMul, StepSound / Look / Smell), bit 0x80 the cell has an entry chain in its sector, 0x100 empty, otherwise one object volume inline (type bits 9-11: 2 = ignored, 3 = a mark that doubles the cost, 4 = a floor / bridge, other = an obstacle; zmin bits 22+, zmax bits 12-21) |
| occupancy | +0x26BC | byte: scratch, the path's direction codes 1..8 while a long search runs, otherwise 0 |
| obstacle | +0x1F8 | byte per cell: the radius level free around it (units stamp their footprint in and out as they move) |

Objects add volumes (`CAIMap::PlaceObjectVolumes` 0x5B7820, `StampObject` 0x5B6A80, the rules are in aigraph.md "Objects"); units are
stamped as footprints of their radius (`CUnit` +0x32C) into a stamp map (`StampUnitFootprint` 0x5B7EF0). A change marks a dirty rectangle and
`CAIMap::UpdatePassability` (0x5B8FA0) recomputes the nibbles of the 8 unit classes there (`GetCellStepCost` 0x5B8BA0, `MarkSteepCells`
0x5B8710, the class footprints), then the coarse cells of the nav grid in it. `CollisionMap` (+0x124; sectors of 32 x 32 cells, 0x54 bytes, a unit
list and an object list each) answers "which units are near" (`FindUnitsNear` 0x5C0BD0, 22 callers: the AI's and the combat's neighbour query).

Questions the game asks of the grids, useful to a bot: `CAIMap::CanUnitStandAt` 0x5B80F0 / `CanUnitBeAt` 0x5B7A40 (the unit's class nibble and
its radius level), `IsCellPassableFor`, `IsUnitCellPassable` (`round(pos * 2 - 0.5)` is the unit's cell), `GetWalkHeightAt`, `TraceVisibility` 0x5BC3E0 (the
line of sight the monsters use, docs/monster-ai.md), `FindFreeCellNear` 0x5BAC10 (the nearest cell a unit can stand on: a spiral over 8 directions).

## The searches

Two levels. All costs are 1/1024 steps; a straight step is 1024 x cost, a diagonal 1448 x.

1. **Local `PathFinder`** (0x5C1D50..): a Dijkstra with eight open lists on a **34 x 34 window** of cells (32 + a closed border; nodes of 0x14
   bytes). `BeginForUnit` fixes the unit's class, speeds and footprint; the step cost between cells comes from the class nibble, the class's cost table
   (`CAIMap::InitCostTables`: walk, a second and a third table by material class, 0x400 = 1.0, -1 impassable) and the slope table by height difference (walking: up to
   40 degrees; class 0, which floats: up to 60 degrees). `ShiftWindow` moves the window by up to 30 cells keeping the costs, so a long path is a chain of windows.
2. **The coarse `NavGrid`** (0x5AF3D0..; the `AIGRAPH`): cells of 8 x 8 terrain cells (4 m), 8 layers (the unit classes); each coarse cell has a best sub-cell and the costs to its 8 neighbours.
   `NavGrid_FindPath` 0x5B0D90 runs a **bidirectional** search (two `PathSearch` objects, forward from the start, backward from the goal, until one reaches a node
   the other closed: `NavGrid_MeetSearches` 0x5AF5F0), with a list of blocked coarse cells. Components (`NavGrid_BuildRegions`) tell at once when the goal is unreachable.

`CAIMap::FindPath` (0x5BB350, 14 callers: the order code's "walk there") puts them together:

- the unit is taken out of the collision map, the target snapped to a free cell (`FindFreeCellNear`), stored in the mover;
- start cell = goal cell: a one-cell path; the straight distance under 25 cells: the two local `PathFinder`s from both ends meet (`MeetLocalSearches` 0x5B9690);
- else, or on a miss, `FindLongPath` (0x5B9C90): the coarse path first; then the local searches walk its chain of waypoints (window shifts), writing direction codes into the occupancy grid; the
  cell path is read back from it (`ExtractOccupancyPath`). When the walk stops within 15 cells of the start the failing coarse cell is added to the mover's blocked list and the search restarts;
  a partial path sets the target to where it ended and the mover's flag +0x6D "no path / partial".
- `StepAlongPath` (0x5BB190) hands the mover at most **32 cells** of the path at a time (the rest waits in the mover's pending list +0x40); `ContinueMove` 0x5BB900 asks for the next chunk, or, when the first search found nothing,
  searches again with no length limit. `RateFootprintPath` 0x5BA400 gives every step its speed factor (the cost table by nibble / the slope) and its water flag.

## The mover (`CMover`, in every `CUnitServer`)

The cells become a **timed curve**, that can be evaluated at any time (the mover's state has its own writers and readers, `CMover::Write` / `Read` and `WriteState` / `ReadState`; whether the client evaluates the same curve from replicated data is not checked):

- `CMover::BuildPath` (0x5C67D0): from the request's cell list it makes the points (cell edges +0.5, a diagonal corner cut when the direction changes by less than 2 steps), `BuildSegments` + `AddSegmentsBetween` the segments with the
  speed of their cells (cell speed x the unit's speed / 512), `LimitSegmentSpeeds`, and an initial turn in place (turn speed +0x44) unless the first segment is within cos 0.9 of the heading.
- `CMover::Evaluate` (0x5C6FC0), `(t, &turning, pos, dir, &speed, &cellx, &celly)`: the position and direction at time t, by the cached segment window and the segment's curve (`Curve_Init`: value and derivative); after the last
  segment the end point at speed 0. `EvalTurn` the heading while turning.
- The walking tick that drives it is `CUnitServer::TickMovement` 0x547890 (states 0 / 1); the orders and stances are in game-memory.md.

## What this means for a bot or an AI player

- A path request is cheap and exact only inside the game's own server; from outside, the `.mob`'s AIGRAPH (aigraph.md) gives the same coarse level (4 m cells, 8 layers) and is what `um-multitool map --navmesh` regenerates.
- A unit's class decides everything: layer 0 floats (water is fine up to a slope of 60 degrees), layers 1..7 follow `T = {2, 0.4, 1, 1.6, 2.2, 1.7, 3, 6}` step heights (aigraph.md), and a unit is "free" only where its radius fits (`CUnit` +0x32C).
- Obstacles are other units too: a crowd blocks cells, so `FindFreeCellNear` moves a target to the nearest free cell, and a long path that fails gets the blocking coarse cell marked and tried again.
