# game.exe in memory

What is known of the running game's own data, found live with um.dll's DLL server
(`resources/universal-mod/um-dll/dll_server.hpp`, `um-multitool dll`). Build: game.exe with PE stamp
0x3ABF0DB3, loaded at 0x00400000 (size 0x3D7000, large address aware). Checked on z3xq1 (zone3xobr.mpr)
and basecam-mp (basegipat.mpr). um.dll's `dll_game.hpp` reads these for the `UNITS` and `CONSOLE`
commands.

## Strings

A game string is a pointer to its characters, after a 12-byte header:

| Offset from the characters | |
|---|---|
| −12 | references |
| −8 | length |
| −4 | capacity |

0x00796E5C is the shared empty string.

## Map objects and units

Every placed object (units included) is a heap record starting with 0x0073EA8C. A unit's record points
to a stats object, which starts with 0x0073DD6C. There is no game.exe global pointing to the units yet:
um.dll finds them by scanning the heap for 0x0073EA8C (on its own thread, every 3 s while asked).

| Record offset | |
|---|---|
| +0x00 | 0x0073EA8C |
| +0x04 | a counter that changes as the unit acts |
| +0x08 | flags: 0x88000 on units that have not acted yet and on bodies; 0x8000 / 0x2 / 0 while active; 0 once looted |
| +0x0C | the object's ID, as in the .mob; 0xFFFFFFFF once a body is looted |
| +0x1C, +0x20, +0x24 | position x, y, z (floats) |
| +0x34, +0x38, +0x3C, +0x40 | rotation quaternion w, x, y, z, as in the .mob (units turn about Z: w and z) |
| +0xA4 | the figure's name (a game string, e.g. "unhuma") |
| +0x170 | the stats object (null once looted) |
| +0x174, +0x1B8 to +0x1DC | path and animation state (change while moving) |
| +0x198 | the unit's effects (spells on it): a list of entries {id, type, value, ..., duration} |
| +0x1B8, +0x1BC | the unit's cell on the path grid (position × 2) |
| +0x240 | the unit's senses (an object of class 0x73EF38, below) |
| +0x280, +0x288 | its current path: an array of waypoints {x × 2, y × 2, direction x, direction y, ?, cell x, cell y}, and their count |
| +0x238 | a player's character: its name (a game string, e.g. "HALAL MAN \| TEST"); empty for the map's units |
| +0x250 | side: the .mob's "player" number (u32) |
| +0x254, +0x255 | the animation (read from game.exe 2026-10-20, not yet seen live): the `.adb` index (record +16, see file-formats/adb-format.md) of the clip playing and of the one queued after it, signed bytes: -1 none, -2 the default idle set. The server's order code writes them (queues `{clip, duration}` per action) and the client plays them (`CUnitClientSpecific::SetAnimation` 0x5352B0: CAnimationSystem2::SetAnimation / QueueAnimation) |
| +0x258 | the world time the clip started (the client seeks to now - this) |
| +0x2E4, +0x2E8 | the walking direction, as a unit vector |

Facing: θ = 2·atan2(z, w), and the unit faces (sin θ, −cos θ), because figures face their local −Y.

| Stats offset | |
|---|---|
| +0x00 | 0x0073DD6C |
| +0x28, +0x2C, +0x30 | HP, HP max, HP regeneration (floats) |
| +0x34, +0x38, +0x3C | mana, mana max, mana regeneration |
| +0x40 on | base attributes (not mapped) |

| Senses offset (record +0x240) | |
|---|---|
| +0x190 | the current order: 0 none, 3 attack (others not seen yet) |
| +0x194 | 6 for every unit seen so far |
| +0x198 | the order's target unit (its record), null without one |
| +0x19C, +0x1A0 | the order's target point (x, y): the target's position, or the last place walked to |
| +0x1A4 | 1000000.0 while attacking, −1 otherwise |
| +0x290, +0x294 | sight range: base, current (floats); the game's info window shows the current one |
| +0x298, +0x29C | a pair, base and current (75 and 99.5 for the player, 50 to 100 for villagers): not the view angle (meaning unknown) |
| +0x2A0, +0x2A4 | a pair, 0 or equal to the sight (8.5) for some units |
| +0x2A8, +0x2AC | a pair (4.3 for the player, 2.8 for villagers): hearing? |

Eagle Sight (`@CastSpellUnit("eagle_sight",0,0,GetLeader())`) raised the player's current sight from 13 to
14 (and +0x29C from 75 to 99.5). Villagers: sight 8.5. The sight drops at night. Players' characters see all
around (a third-person camera); other units see 180° in front of them.

**Life cycle.** A unit is alive while HP > 0. It is a body when HP ≤ 0, and its record and flags stay.
When the body is looted, the record is emptied in place: ID 0xFFFFFFFF, flags 0, stats pointer null.

**The player's characters** have a name at +0x238 (the map's units have none). They are on their own
side (0 in basecam-mp), with IDs that are in no loaded .mob. The ID is new each session (1000011214,
1000000013...). No global pointer to them is known yet. In multiplayer every player's character has its
name. Which one is "me" is not stored where we know: the radar takes the one nearest the camera's aim
point, unless one is picked.

One linked list (nodes {next, next, record}) held the player first and then 8 units near them. It may
be a spatial cell or the player's view, but it is not the full unit list.

**Code writing HP** (from a hardware breakpoint while fighting):

| Function | What it does |
|---|---|
| sub_525070+0x345 | applies damage (its strings mention "impaling") |
| sub_524560+0x765 | stores HP after every change |
| sub_5243D0+0x16C | another change, probably regeneration |
| sub_523740+0x1C9 | another change, probably regeneration |
| sub_523370+0x7 | sets HP to 0 when the record is emptied |

### Attacking and looting

Checked on z3xq1 with the game paused through its flags (below):

- `@Attack(GetObject(4517),GetLeader())` made a boar attack the player: its senses showed order 3, target the
  player's record, target point the player's position.
- `@Attack(GetLeader(),GetObject(4517))` made the player attack the boar (once in sight, and with cheats on):
  the player's senses showed order 3 and the boar's record; the order went back to 0 when the boar died.
- The console command `lootall` ("Looting...") looted every body around: their IDs became 0xFFFFFFFF.
- `@` commands are ignored silently while cheats are off (0x7C7B64 = 0); `thingamabob` turns them on, and
  they are off again in a new game session.
- Writing 1 to both pause flags (0x007AF624 and 0x007AFFCC) pauses the game like Space (the tick counter
  stops); 0 resumes it.

## The current map

| Address | |
|---|---|
| 0x007C2CCC | the terrain's file name ("zone3xobr.mpr"), a game string |
| 0x007C2CC8 | the base map's file name ("zone3xobr-lmp.mob") |
| 0x007AFE90 (also 0x007C7DFC) | the quest's name ("z3xq1"); the empty string outside a quest |

The strings are rewritten in place when the map changes. um.dll's file-open tracking got this wrong after
going back to a map the game already had in memory (it does not open the files again).

## Camera

The game camera is a global in game.exe's .data section.

| Address | |
|---|---|
| 0x0079B2C4 | position x, y, z |
| 0x0079B2DC | rotation quaternion (4 floats) |
| 0x0079B388 | the point it turns around (where it aims), x, y, z; the free camera moves it on X and Y |
| 0x0079B39C, 0x0079B3A8 | sine and cosine of half the camera's turn about Z |

The view direction on the ground is from the position to the aim point.

## Game speed and time

Found by recording game.exe's .data once a second on z3xq1 while the game ran at normal speed, was
paused (Space), resumed, then set to ×2:

| Address | |
|---|---|
| 0x007AF624 (copy at 0x007AFFCC) | 1 while the game time is paused (Space), else 0 (u32) |
| 0x007C7DE4 | 1 at ×2 speed, else 0 (u32) |
| 0x0078FF34 | milliseconds per game tick: 55 at normal speed, 27 at ×2 (u32) |
| 0x007AF61C (copies at 0x007AFFC4, 0x007B279C) | game tick counter: +18.4 a second at normal speed, stopped while paused, +37 a second at ×2 (u32) |

The counter's rate (55 ms a tick, about 18 a second) differs from the 15 ticks a second measured with
spell durations (Fireworks: 75 ticks = 5 s): the spells may count another clock.

Two floats at 0x007AFC68 (copy 0x007AFC78) and 0x007AFC88 rise slowly (0.0017 and 0.0047 a second)
and stop while paused, but grow only about 1.8 times faster at ×2: perhaps the day cycle (not confirmed).

## Console

game.exe keeps a pointer to the console object at **0x007C7D64**. The console starts with 0x007C7D50,
but that value is shared by many interface objects, so it does not identify the console.

| Console offset | |
|---|---|
| +0x04 | set while the console is open (0 when closed) |
| +0x0C | command history: a list (class 0x00745F5C) with its array at +0x10, count +0x14, capacity +0x18 |
| +0x24 | the input line (a game string) |
| +0x28 | the cursor position |
| +0x2C | the input's length |
| +0x58 | the lines shown: a list with its array of strings at +0x5C, count +0x60, capacity +0x64 |
| +0x7A | 1 while the console is open, 0 when closed (byte) |

The first lines of the console read "18:-1070071807" (ten of them in basecamp).

The command dispatcher is sub_5DC620. It pushes "Unknown command" (0x0078D848) at 0x005DD7A0.

### Console commands

Found statically in sub_5DC620 (game.exe at 0x5DC620 to 0x5DE200):

- `help`, `debuginfo`, `console`, `history`, `filter`, `show`
- `listvar`, `loadvar`, `lastfps`, `exec`, `execute`, `give`, `fadeout`, `lootall`, `memusage`, `days`, `fps`
- network: `join`, `server`, `net`, `disconnect`, `kick`, `ban`, `rate`, `localrate`, `partyresend`, `generateacks`
- `quit`, `exit`

Positions for `debuginfo` and `console`: `left`, `right`, `lefttop` (`lt`), `righttop` (`rt`), `top`,
`bottom`, `center`, `fullscreen`. Other words: `on`, `off`, and `none`, `ai`, `event`, `graphics`
(after "AI: ").

Commands are looked up in a table at 0x7C7B28 (sub_5E0B20 finds a name).

A line's first character can make it a script call instead:

| Prefix | Runs |
|---|---|
| `@text` | `WorldScript( text )`: any MOB script expression |
| `#text` | `WorldScript( ConsoleFloat( text ))`: prints a number |
| `$text` | `WorldScript( ConsoleString( text ))`: prints a text |

Errors are printed as "Script error: ...". The prefixes only work when the byte at **0x7C7B64** is not 0:
the cheats switch, which the console command `thingamabob` turns on ("Activated!"). They also need
sub_57F440 on the object at 0x7AFE20 to return 0 (not known yet; it passed in single player).

Checked live (basecamp, cheats on), by typing into the game window (WM_CHAR, then Enter):
- `#Sin(0)` prints 0; `#GetWorldTime()` 6.21778 (hours); `#IsNight()` 0.439722; `#GetMoney(0)`
  1.1992e+008; `#GetDiplomacy(0,2)` and `(2,0)` 0; `#GetMercsNumber(0)` 0.
- `#HP(GetObject(927062))` 22 and `#GetX(GetObject(927062))` 56.7006: the same as the unit's record.
  `GetObject(id)` takes the .mob ID; a player's character's ID is too large for a script number, but
  `GetLeader()` and `GetUnitOfPlayer(0,0)` return it.
- `@MoveToPoint(GetLeader(),59,60)`: the character walked to (59, 60). The same order to a villager
  (AI class 3) did nothing.
- `@SetCameraPosition(57,65,20)` did nothing. Writing the camera's aim point (0x0079B388, x and y)
  moves the camera: its position follows, keeping its distance and angle.
- `help` is answered "Unknown command".
With them, the console can call the script functions: `SetCameraPosition`, `SetCameraOrientation`,
`MoveToPoint`, `KillUnit`, `CastSpellUnit`, `HP`... (see `resources/universal-mod/um-dll/mob_script_functions.hpp`).

um.dll's `CONSOLE send` types into the game window instead of calling it. That only works while the
console is open; where the "console open" flag is stored is not known yet.

## Scripts and quests

Confirmed on z3xq2 and z3xq3 (every objective, start to end).

**Quests.** A map script declares a quest with `QStart("z3xq3")` then one `QObj*` call per objective
(sub-objective), and `QFinish()`. The game turns them into a chain of generated scripts, `Su0`, `Su1`...,
one per objective, each waiting for its condition, then ending itself, starting the next one and setting the
state variables (the conditions as the game writes them, from templates in game.exe around 0x78B9B5):

| Call | Condition |
|---|---|
| `QObjArea(n)` | `any(i, Heroes, IsInArea(n, GetX(i), GetY(i)))`; areas come from `AddRoundToArea(n, x, y, r)` / `AddRectToArea(n, x1, y1, x2, y2)` |
| `QObjKillGroup(g)` | `Not(any(i, g, IsAlive(i)))`; groups come from `AddObject(g, unit)` |
| `QObjSeeUnit(u)` | `IsUnitVisible(u)` |
| `QObjKillUnit(u)` | `Not(IsAlive(u))` |
| `QObjGetItem(n)` | `HaveItem(0, n)` |
| `QObjSeeObject(o)` | `any(i, Heroes, IsLess(DistanceUnitUnit(i, o), 7))` |
| `QObjUse(o, state)` | the lever's state (`GetLeverState`) |

**Quest state** = global script variables (`GSSetVar`): `q.<quest>.<quest>` is 1 while the quest runs and 2
once completed; `q.<quest>.<quest>.<N>` is objective N: absent or 0 not received, 1 received (active), 2 done.
A quest's .mq gives the first ones (`quest.reg`: "give quests q.z3xq3.z3xq3 q.z3xq3.z3xq3.1"). Each variable
is a heap object of class 0x0073BCE8: +0x0C the name (char*), +0x18 the value (float); usually two copies,
which agree. um.dll's `VARS [prefix]` lists them.

The objective list itself is a global at 0x007AA608 (class 0x73D46C): an array of {type, text argument,
number argument} (types 0xD7 Area, 0xDB KillGroup, 0xD9 SeeUnit, 0xDA KillUnit, 0xDD GetItem, 0xD8
SeeObject, 0xDC Use), its count and capacity. The texts are in the .mq: the entry "quest <name>" holds the
title, the description, then `#subobj N` sections (the objective's title line, then its description).

**Scripts.** The script engine keeps each script's name as a game string with its map's prefix
("!1!VCheck#3#2"); the declarations are entries of 0xC0 bytes (the name, the argument signature, value
slots of classes 0x73D430, 0x73D2E0, 0x73D2D4, 0x73D110, 0x73D304). A script that runs (started and waiting
for its condition) holds one more reference to its name string: 2 or more references = running. Seen with
z3xq3's `VCheck#3#2` (waiting for the lever): 2 references until the lever was used, then 1. um.dll's
`SCRIPTS` lists them. Most quest scripts run once (`KillScript()` first) and end at once.

The counters 0x007AFED0 / 0x007AFED4 (they rise by 2 per objective) are the journal's message indexes,
maintained by the interface (sub_5800C0, from the interface object at 0x0079B5C8): not the quest state.
(The objects of classes 0x73D294 / 0x73FF8C first suspected to be scripts are scene and model nodes.)

## Stance, weapons and orders

- Stance: unit record +0x268 the stance asked for (writing it changes the stance: the game plays the
  posture change), +0x264 the stance applied (read only), +0x2B8 the speed of that stance (x 10 = m/s): 0
  crawl (0.128), 1 sneak (0.160), 2 walk (0.208), 3 run (0.480). Walk <-> run while standing is applied once
  the unit moves; a unit does not move while its posture changes (about 1.2 s down, 1.6 s up from crawl).
- Weapons: record +0x26C (and +0x270) the weapon slot in hand, 0-3 or -1 none (the game's weapon N is slot
  N-1); +0x49C a type that seems to select the animation set (0x19 axe and spear, 0x10 dagger, 0xF bow and no
  weapon). Read only: selecting is CUnitServer method 25, `0x553610(this = [record+0x240], slot)`.
- [record+0x240] is the unit's CUnitServer (vtable 0x73EF38; the "senses" object above): its orders.
  The current order is at +0x190, the pending one at +0x1B4 (0x24 bytes: type, ?, target unit (counted
  reference), point x/y/z, flags); sub_54A900 moves the pending order into the current one. Types: 0 stand /
  step, 1 move to a point, 3 attack, 5 cast?, 9 none. A player's click: sub_66FD70 (client side) ->
  sub_5D39E0 (adds the formation offset) -> sub_554740 (move to point: pending type 1, z = 1000000 = ground).
- Inventory: unit record +0x24C -> the player object (class 0x743104); its +0xE0 is the inventory, a list
  (class 0x73BE80: +0x04 head node, +0x0C count) of 12-byte nodes {next, previous, item}. Items are objects of
  class 0x743F5C: +0x0C an ID, +0x18 a code (three granites given with `@GiveItem(0,"material.granite")`:
  0x00010005, granite being material 5 of the database: the code is the database row << 16 | the material, the
  same pair a .mp stores). Each unit also has its own array of items: record +0x4A0, count +0x4A4.
- HaveItem(player, n) (script function 0x7C, case at 0x4B4F2C, then 0x665640): true when the player's list holds an
  item of kind 0x3009 whose +0x4C is n, or one of the player's units holds an item whose +0x4C is n. um.dll's
  `ITEMS [raw]` lists both (the Connector's Quests tab uses it for "Get item"). For a quest item, +0x4C is its
  QuestItems row's ScriptID (checked: `@GiveQuestItem(0,"platekey01")` gave kind 0x3009, row 84, +0x4C 150 = its
  ScriptID), so `QObjGetItem(150)` asks for platekey01. Kinds: 0x3004 weapon, 0x3007 loot item (LootItems), 0x3009
  quest item (QuestItems); +0x48 the quantity. Quest items come from `GiveQuestItem`, not `GiveItem`.
- game.exe is client/server: CWorldClient / CPlayerClient / CUnitClientSpecific and CWorldServer /
  CUnitServer / CMapObjectServer; the host's server is authoritative (also in single player).

## Experience: levels, skills and abilities (the camp's skills window)

Everything is client side, on the party's unit parameters (the .mp `stats`): the camp changes the bytes and `+8` (experience spent) and the server takes them over at the next join.
Verified live (2026-10-20) against the game's own cost functions, and against the saved characters (a character's spent experience = its skills + its abilities at their prices).

- Experience total `stats+4`, spent `stats+8`; the free points of the window = (int)(total - spent) (`CCampSkillsWin::OnLButtonUp` 0x626920). The level is `round(log(total / 200) / log(1.61))`
  (`CUnitServer::GetLevel` 0x556760; the constants are the float table at 0x7C5430: 200 at +0x58, 1.61 at +0x5C, 1.95 at +0x54, 5 at +0x50, 50 at +0x4C).
- A skill (bytes at `stats+0x6CB`: melee, archery, backstab, elemental, sense, astral; `+0x6C8` the "science" row) rises by 1 for `trunc(50 * (1.61^((v+1)/5) - 1.61^(v/5)))`
  (0x527FF0), at most to 100.
- An ability ("perk", byte `stats+0x6D3+group`, level 0..3; 26 groups x 3 levels = the 78 rows of perks.pdb block 2): the next level costs
  `base = trunc(50 * (1.61^(c/5) - 1.61^(c'/5)))` (c = the row's cost number, db field 18; c' = the previous level's, 0 for level 1), times `1.95^N` (N = all ability levels the character owns,
  the sum of the 36 bytes `stats+0x6D3..0x6F6`), then rounded to one significant digit, or two when the leading digit is below 4 (below 100 it is rounded up) (`Perk_ComputeCost` 0x528050).
- It can be learned (`UnitStats_CanLearnPerk` 0x5260A0) when the row is enabled (db field 22; in databaselmp.res the magic schools' rows are off), the attribute minima (fields 15..17) are met, the skill
  it names (fields 11 / 12 / 14) is high enough, exactly the previous level is owned (the row names it in field 10), and no other group already owned has the same exclusion number (field 23).
- Row fields: 18 cost number, 19 bonus (percent; points for the attribute groups 23..25), 20 / 21 unused here. Groups: 0..6 sword, axe, dagger, spear, bludgeon, bow, crossbow; 7..14 fire, lightning, acid,
  illusion, divination, enchantments, healing, domination; 15 night vision, 16 health, 17 mana, 18 vitality, 19 spirit, 20 quickness, 21 lift, 22 backstab, 23 strength, 24 dexterity, 25 intelligence.
- The cached values `UnitStats_Recompute` (0x524CD0) rebuilds from the base values and the abilities: `+0x10 / +0x18 / +0x20` = base `+0xC / +0x14 / +0x1C` + the attribute abilities' bonus;
  `+0x54` (max encumbrance) = (1 + lift%) * strength * 12; `+0x2C` = dexterity * 0.2 + 10; `+0x30` (actions) = (1 + quickness%) * `+0x5C` * `+0x2C`. The game rebuilds them when the unit is created.

## Spells: blueprints, runes, the constructor (camp tab "spell trade" / "spell constructor")

Verified 2026-10-20 against the shop (live dump), every spell object of the saved characters (24 of 33 byte-identical; the others come from an older database) and a live cast.

- The shops sell **blueprints** ("Prototype" loot items, `+0x54` = SpellPrototypes row) and **runes** ("Rune" loot items, `+0x54` = runes row): every spell / rune whose "Shop N" bit is set, blueprints 1..3 of each, runes 20..47.
  Price = the blueprint's field 3 / the rune's field 2 (CItemLoot slot 16 0x6A5470 returns it in st0; the stored price 1.0 of the item is unused), times the camp's spell coefficient (buy 1.0, sell 0.5).
- The constructor (`CSpellConstructor` 0x684xxx) turns one blueprint + up to 8 runes into a **spell container** item (kind 0x3008, the "wand": 56 bytes of detail, `+0x40` = the spell object's id, price field = the spell's price + 2) holding a **spell object**
  (kind 0x3002, 66 bytes). The spell goes into the member's list 4 (spell slots); the 0x33 cast order's spell id is the index there. A container carried in the items but not worn is not castable.
- Object layout (`CSpell::Write` 0x68B920): u32 id, u8 row, u8 0x10, u32 duration (S+0x20), f32 effect (+0x24), f32 area radius (+0x28), f32 +0x30, f32 range (+0x34), i32 targets (+0x38), u32 flags (+0x3C: 0x10000000 'u' / 0x20000000 't' target kind),
  f32 mana (+0x40), i32 rune count + 1 (+0x44), f32 price (+0x48), i32 field 14 (+0x4C), 8 bytes rune rows + 1, u32 0, u32 required skill level (+0x50). Start values = the proto's fields 13, 10, 9, 7, 8, 12, 11, 5, 6, 3, 14, 21.
- `CSpellContainer::Finalize` 0x68BF60 applies each rune by the first letter of its code: a area += v * proto f9 (then radius = sqrt(area / pi)), d duration += trunc(proto f13 * v), e effect += v * proto f10, f? / i? flag bits, m mana += v * proto f5,
  r range += v * proto f8, t targets += trunc(proto f12 * v); every rune also: count + 1, required level += rune level (field 6), mana += rune field 4, price += rune price; the final mana = max(proto mana, mana). A rune of kind r/t/a/e/d needs the blueprint's field 20 flag of that kind.
- Usable (`CanDropItem` 0x6100E0, `CanCastSpell` 0x54B950): magic skill >= required level, and trunc(mana * 25 / Intelligence) <= trunc(max mana). Magic skill = Intelligence - 25 + the school's skill byte (0x6CE elemental for schools 0..2, 0x6CF sense 3..4, 0x6D0 astral 5..7)
  + the school ability's bonus; school = proto field 19. Max mana = (dexterity / 25) * 30 * 1.1^(log(exp / 50) / log(1.61)) * (1 + mana ability%) (live: 86.53 for exp 4000, dexterity 30); max health the same with strength / 25 and `+0x10`.

## Not known yet

- The live diplomacy table, in case scripts change it during play.
- A list or global pointer giving all units without a scan.
- The meaning of each flag bit.
