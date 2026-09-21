## Evil Islands Scripting Functions - English Documentation

### Core Functions

| Function | Description |
|----------|-------------|
| **ActivateTrap** (Trap: object, Activate: float) | Activates (`Activate ≠ 0`) or deactivates (`Activate = 0`) a trap on the map. Traps only attack Diplomatic Group 0. |
| **Add** (A: float, B: float) → float | Returns the sum of two numbers. |
| **AddLoot** (nPlayer: float, PartyNameSend: string, PartyNameRecv: string) | Moves all items and gold from the supply wagon (`PartyNameSend`) to another party's wagon (`PartyNameRecv`). |
| **AddMob** (FileNameMob: string) | Loads a `.mob` file from the `MAPS` folder. |
| **AddObject** (grp: group, UnitMap: object) | Adds a unit to a game group (global variable). |
| **AddRectToArea** (idArea: float, x1: float, y1: float, x2: float, y2: float) | Adds a rectangular area to a quest zone. |
| **AddRoundToArea** (idArea: float, x: float, y: float, rad: float) | Adds a circular area to a quest zone. |
| **AddUnitToParty** (nPlayer: float, 'PartyName::NamePersRes': string, NameUnitRes: string) | Adds a localized character to a specific party. |
| **AddUnitToServer** (NameUnitMap: string, NameUnitRes: string, NameUnitControl: string, x: float, y: float, z: float) | Spawns a unit on the map at coordinates (x,y,z). |
| **AddUnitUnderControl** (nPlayer: float, Unit: object) | Adds a unit icon to the player's control panel (does not change diplomacy). |

### Group & Diplomacy Functions

| Function | Description |
|----------|-------------|
| **GetDiplomacy** (nDiplomacyGroupA: float, nDiplomacyGroupB: float) → float | Returns relationship: `1` = Ally, `0` = Neutral, `-1` = Enemy. |
| **SetDiplomacy** (nDiplomacyGroupA: float, nDiplomacyGroupB: float, Diplomacy: float) | Changes diplomatic relationship between two groups. |
| **GetPlayer** (Unit: object) → float | Returns the diplomatic group number of a unit. |
| **SetPlayer** (Unit: object, nDiplomacyGroup: float) | Assigns a unit to a diplomatic group. |

### Particle, Light & FX Functions

| Function | Description |
|----------|-------------|
| **CreateParticleSource** (id: float, x: float, y: float, z: float, R: float, constParticleSource: string) | Creates a particle source. |
| **DeleteParticleSource** (id: float) | Deletes all particle sources with the given ID. |
| **CreatePointLight** (id: float, x: float, y: float, z: float, rad: float, R: float, G: float, B: float) | Creates a point light source. |
| **DeletePointLight** (id: float) | Deletes all point lights with the given ID. |
| **CreateFXSource** (id: float, x: float, y: float, z: float, Volume: float, rad: float, NameWav: string) | Creates a looping sound source. |
| **DeleteFXSource** (id: float) | Deletes all sound sources with the given ID. |
| **CreateLightning** (id: float, x1,y1,z1, x2,y2,z2, Intensity: float) | Creates lightning effect. |
| **DeleteLightning** (id: float) | Deletes lightning with given ID. |

### Quest Functions

| Function | Description |
|----------|-------------|
| **QStart** (QuestName: string) | Starts a new quest. |
| **QFinish** () | Completes the current quest. |
| **QObjArea** (idArea: float) | Quest objective: Enter area. |
| **QObjKillUnit** (UnitName: string) | Quest objective: Kill specific unit. |
| **QObjKillGroup** ('grp': string) | Quest objective: Kill entire group. |
| **QObjGetItem** (id: float) | Quest objective: Obtain quest item. |
| **QObjSeeUnit** (UnitName: string) | Quest objective: See a unit. |
| **QObjUse** (Lever: string, State: float) | Quest objective: Use lever in specific state. |

### Unit Control & AI Functions

| Function | Description |
|----------|-------------|
| **Attack** (UnitA: object, UnitB: object) | Orders UnitA to attack UnitB. |
| **MoveToPoint** (Unit: object, x: float, y: float) | Orders unit to move to point. |
| **Follow** (UnitA: object, UnitB: object) | Orders UnitA to follow UnitB (one-time). |
| **UMFollow** (UnitA: object, UnitB: object) | Orders UnitA to permanently follow UnitB. |
| **Guard** / **UMGuard** | Orders unit to guard an area. |
| **Sentry** / **UMSentry** | Orders unit to guard a point and attack enemies in sight. |
| **BlockUnit** (Unit: object, Block: float) | Blocks/unblocks player control of a unit. |

### Inventory & Items

| Function | Description |
|----------|-------------|
| **GiveItem** (nPlayer: float, Item: string) | Gives a regular item to the player. |
| **GiveQuestItem** (nPlayer: float, QuestItem: string) | Gives a quest item. |
| **GiveMoney** (nPlayer: float, Money: float) | Gives money. |
| **EraseQuestItem** (nPlayer: float, idQuestItem: float) | Removes a quest item. |
| **HaveItem** (nPlayer: float, idQuestItem: float) → float | Checks if player has a quest item. |

### Other Useful Functions

| Function | Description |
|----------|-------------|
| **Sleep** (Sleep: float) | Pauses script execution (in 1/15 sec units). |
| **KillScript** () | Stops the current script (only in map script modules). |
| **KillUnit** (Unit: object) | Kills a unit (no XP granted). |
| **InflictDamage** (Unit: object, Amount: float) | Deals damage (XP is granted). |
| **IsAlive** (Unit: object) → float | Checks if unit is alive. |
| **IsDead** (Unit: object) → float | Checks if unit is dead. |
| **GetX / GetY / GetZ** (Object: object) → float | Returns world coordinates. |
| **GetObjectByID** (idObject: string) → object | Gets object by string ID (supports 10-digit IDs). |

---

## Script validation (`MOB_VALIDATION` in um.dll)

With `MOB_VALIDATION=true`, `um.dll` checks each `.mob` file's mission script (the encrypted `SS_TEXT` node, dumped as `.eis` by `um-multitool mobdump`) when the game opens the file, and writes the findings to `um.log` as `[MOBCHECK]` lines. Line numbers count lines of the script text, so they match the dumped `.eis` file. Only maps inside the folder of the mod that contains `um.dll` are checked; other mods' maps and the base game's, which the game also opens, are left alone.

The language is small and regular: `GlobalVars`, `DeclareScript`, `Script`, `WorldScript`, `if ( conditions ) then ( ... ) [else ( ... )]`, nested `Command( args )` calls, `variable = value`, and the `For( var, group ) ( ... )` loop. The command list (name, parameter types, return type) comes from the VGG editor's `syntax.ini`/`scripts.htm` and MobExplorer's `script_refs.txt`, corrected against the shipped maps; see `resources/universal-mod/um-dll/mob_script_functions.hpp`.

| Level in `um.log` | Meaning |
| :--- | :--- |
| `ERROR` | The script cannot work: syntax error, wrong argument count for a known command, unknown type, a command that returns nothing used as a value, an undeclared number/string variable. |
| `WARN` | Suspicious: a wrong argument/condition/assignment type, a script call with the wrong argument count, or an unknown name that looks like a typo of a known command (with a "did you mean" hint). |
| `DEBUG` | Usually fine: a name that is neither a known command nor a script of this file (it may be defined in another `.mob`). |

**Quest maps.** A quest map (`z12q2.mob`, with its `z12q2.mq` archive) is loaded by the game on top of its zone's base map (`zone12-lmp.mob`), and its script freely uses the base map's variables (in the shipped maps, 23 of 42 quests do). The checker finds the base map from the `#res` line of the quest archive's `map.txt` and resolves names against both files. Object IDs must not repeat between the two: the quest's object silently replaces the base map's object with the same ID, which is reported as a `WARN`.

**Checks against the map itself.** The script's references are also checked against the objects of the map, its base map and every map its script loads with `AddMob(...)` (a script often pulls in extra maps at runtime; in the shipped maps this explains most IDs that are not in the map's own file):

- `GetObject( N )`, `GetObjectByID( "N" )` and `"GetObject(N)"` inside quest commands must name an object ID that exists. One summarised `WARN` per script lists how many IDs are missing and a few examples.
- An undeclared name used where an object is expected must be the name of an object in the map.
- The text argument of `GiveItem`, `GiveQuestItem`, `CastSpellUnit` and `CastSpellPoint` must be an item or spell known to the database (`material.iron[1]` and `lightning{a1}` are checked by their parts).

These checks are skipped when a map they depend on cannot be found (a quest whose `.mq` archive or base map is not available, or an `AddMob` target that is missing), so an unavailable map never causes a false warning. Unit names (`AddUnitToServer`) are not checked: no shipped map uses them, so there is no data to validate a rule against.

Two things are implicit in the language and are therefore never reported: a group variable is created by its first use, and an undeclared name where an object is expected refers to a named object placed in the map (possibly in the base map).

The checker is plain C++ (`resources/universal-mod/um-dll/mob_script_check.hpp`) with regression tests in `resources/universal-mod/um-dll/tests/`. It reports no errors on any of the 237 shipped and community maps it was developed against.

