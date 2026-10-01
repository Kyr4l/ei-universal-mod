# AIGRAPH: the navmesh of a zone (`.mob` node `0x31415926`)

The walkability graph units path-find on. It sits among a zone `.mob`'s top-level nodes, usually after the
root's length. The game builds it, from the terrain and the objects, and only loads it from the map
(`CWorldServer::LoadMap` fails with "Can't load AIGraph" without one). EI_Plugin's `GraphGen` makes the game
build it again at load and writes the result into the `.mob` (the old file kept as `.bak`).

um-multitool reproduces the game's generator (`tools/um-multitool/mapedit/navmesh_gen.hpp`): the Map Editor
rebuilds it on save ("Navmesh" next to Save), shows where a map's graph is out of date (Layers → Navmesh
differences), and `um-multitool map --navmesh` builds and compares it from the command line. On the maps that
EI_Plugin regenerated, 99-100 % of the nodes come out identical.

All of it was reverse-engineered from game.exe (the `EIStarter OBT-1/Engine` build); the addresses below are
that build's.

## Layout

```
u32 W, H                      cells (4 x 4 world units each): W = sectorsX * 8, H = sectorsY * 8
for layer 0..7, for row 0..H-1:
    A: W x 8 u16              step costs to the 8 neighbours (0xFFFF: no step)
    B: W bytes                the cell's representative tile: x | y << 4, in tiles of 0.5 units
    C: W u16                  the connected component (1, 2, ... in row order)
```

19 bytes per cell and layer. Directions: 0 (0,-1), 1 (-1,-1), 2 (-1,0), 3 (-1,1), 4 (0,1), 5 (1,1), 6 (1,0),
7 (1,-1). The layers are the AI unit classes; a straight step costs about 64 (layer 0) or 127 (others) on
open ground, a diagonal one about 90 or 180.

## How the game builds it

### Tiles (`CAIMap::Load` 0x5B43C0)

A grid of 0.5-unit tiles (64 per sector side). Per tile:

- **height**, in steps of `maxZ / 511`: each terrain vertex quad gives 2 x 2 tiles,
  `t00 = h00/2 + (h10+h01)/4`, `t10 = h10/2 + (h00+h11)/4`, `t01 = h01/2 + (h00+h11)/4`,
  `t11 = h11/2 + (h10+h01)/4`, rounded to the nearest step.
- **water depth** (0..63 steps) in sectors with water: the same from the water vertices, minus the ground.
- **material**: the terrain tile type (`.mp` tile types) of its 2 x 2-unit land tile, or of the water tile
  where there is water. Materials are `aiinfo.res`'s `tileDesc.reg` (GRASS, GROUND, STONE, SAND, ROCK, FIELD,
  WATER, ROAD, ASTRAL, SNOW, ICE, DRYGRASS, SNOWBALLS, Lava, Swamp), whose `CostMul` matters: 2 for most,
  1 for ROAD and Lava, 8 for WATER, 50 for Swamp.
- **volumes** of objects (below).

### Objects (0x5B6A80)

Every object with a figure, but units, adds the boxes of its parts (`OBJ_BODYPARTS`, or all of them): the
part's `.fig` bounds blended by the object's complection (not clamped to 0..1), placed at the part's offset
plus the figure's centre, rotated by the object, standing on the ground at its tile (its z is above the
ground). Each box face is sampled about 3 times per tile; per tile, the lowest sample of the bottom and side
faces and the highest of the top faces make one volume (one per object). Part names:

- `BASE...`: a floor. The ground rises to its top, it is dry, its material becomes ROAD.
- `CROWN...`: ignored (tree tops).
- `EMPTY...`: left out.
- anything else: an obstacle.

### Tile values (0x5B8FA0, 0x5B8BA0)

Per layer `k`, a value 0..15 (0: not walkable). Each class has a step height
`T = {2, 0.4, 1, 1.6, 2.2, 1.7, 3, 6}` units. A unit spans `[ground, ground + T]` (layer 0 floats:
`ground + water depth`).

- Base cost: 1 for layer 0, else the material's `CostMul`. Base speed: 1024.
- Water (layers 1..7): too deep (≥ the step, or any water for layer 1) blocks it. Deeper than half the step:
  speed 800, cost +30. Else speed 880.
- Each obstacle volume overlapping the span by a fraction `f`: cost `+ 290 f`, speed `* 0.6^(7 f)`.
- Cost over 70, or a blocked speed: 0. Else, with `s = min(15, (speed + 50) / 102)` and the cost's row (under 2,
  2, 3-9, 10-34, 35+), the value comes from a table (row 0 at s = 10 gives 14, row 1 gives 13).

Then:

- **Footprint**: a tile is 0 if any tile of the class's footprint around it is 0. The footprint is a cross of
  5 for layers 0-4, a diamond of 13 for 5-6, and 29 tiles for 7.
- **Cliffs**: with the largest height difference `m` to the 4 neighbours, steeper than 60° (one tile = 0.5
  units) clears every layer; steeper than 40° clears all but layer 0. Layers 5-6 are also cleared on one
  neighbour, and layer 7 on the 13 tiles around.
- The 3 tiles along the map's edges are 0.

### Steps between tiles

A Dijkstra over the tiles in a window. A step to a tile costs `slope(dh) * factor(value) * base`, with base
1024 straight and 1448 diagonal, all in 1/1024ths:
- `factor`: per value, from 25 (value 1) down to 1.0 (value 14) and 1.2 (value 15). A value of 0 blocks.
- `slope`: by the angle of the height change over 0.5 units. Layer 0 allows up to 60°, at no extra cost. The
  other layers allow up to 40°, at `(1 + sin² a)²` uphill and `1 / (1 + sin² a)` downhill.

### The graph (0x5AFDA0)

- **B**: from the cell's centre outwards (squares of 2, 4, 6, 8 tiles), the tile with the best value whose
  search within the cell (9 x 9 tiles) reaches both of its opposite edges (left and right, or top and
  bottom). With none, the last walkable tile; with no walkable tile, 0x33.
- **A**: a search from the cell's tile over the 25 x 25 tiles around it. The cost to each neighbour cell's
  tile is the distance / 128 (at most 0x7FFF), or 0xFFFF if it can't be reached. The map's edges get 0xFFFF.
  Each pair of opposite steps then gets the lower of the two, 0xFFFF counting as lower.
- **C**: components numbered in row order, through the steps that exist.
