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

### Unit stance & orders

| Function | Description |
|----------|-------------|
| **Crawl** (Unit: object) | Sets the unit's stance to crouching (stance 1). |
| **Walk** (Unit: object) | Sets the unit's stance to walking (stance 2). |
| **Stand** (Unit: object) | Same as `Walk` (stance 2). |
| **Run** (Unit: object) | Sets the unit's stance to running (stance 3). |
| **Lie** (Unit: object) | Sets the unit's stance to lying down (stance 0). |
| **Idle** (Unit: object) | Drops all of the unit's behaviours and its current order: it stands and does nothing. |
| **Rest** (Unit: object) | Orders the unit to rest (it sits down when it can). |
| **RotateTo** (Unit: object, x: float, y: float) | Turns the unit to face the point (x,y). Does nothing right after `AddUnitToServer`: the unit has to exist on the map first, and `Sleep` only works in map script modules. |
| **MoveToObject** (UnitA: object, UnitB: object) | Orders UnitA to walk, in its current stance, to the place where UnitB stands at this moment (it does not follow). |
| **PlayAnimation** (Unit: object, Animation: string) | Plays an animation from the unit's `.adb` file (`res/database.res/<Mask name>.adb`, the mask is in `RaceModels.txt`). The first letter sets the kind: `u` = once, then back to normal; `c` = loops, the unit ignores everything until the next order; `s` = one step of a movement. Families: `uattack01..09`, `udeath01..31`, `cidle01..09`, `ucast01..04`, `uhit01..21`, `crun01..02`, `cwalk01..09`, `ccrawl01`, `ucross01..09`, `scross01..09`, `crest`; humans also `ubriefing##`, `uspecial##`, `cspecial##`. An unknown name does nothing. |
| **StartAnimation** (Unit: object, PathAnimation: string) | Not implemented in game.exe (does nothing). Use `PlayAnimation`. |
| **WaitEndAnimation** () | Not implemented in game.exe (does nothing). |
| **SleepUntil** (bool: float) | Pauses the script until the condition becomes non-zero. |
| **SleepUntilIdle** (Unit: object) | Pauses the script until the unit has no order any more. |
| **WaitSegment** (Wait: float) | Pauses the called procedure for `Wait` segments (15 per second); map script modules only. |
| **SetCP** (Object: object, x: float, y: float, z: float) | Moves any map object (not only a unit) to (x,y). Units lose their orders, and the passability graph is recalculated (slower than `SetCPFast`). |
| **SetCPFast** (Object: object, x: float, y: float, z: float) | Like `SetCP`, but the AI passability graph is not recalculated. |
| **HideObject** (Unit: object, Hide: float) | Hides (`Hide ≠ 0`) or shows (`0`) a map object. Meant for briefing zones. |
| **RemoveObjectFromServer** (Object: object) | Removes the object from the server (same as `RemoveUnitFromServer`). |
| **RemoveUnitFromServer** (Unit: object) | Removes the object from the server (same as `RemoveObjectFromServer`). |
| **RemoveObject** (grp: group, Unit: object) | Removes the unit from a game group (the group variable of the map script; not usable from the console). |
| **For** (GlobalVariable: object, grp: group) (Command) | Runs the commands in the body once for every object of the group, with the global variable set to that object. The group may be a diplomatic group, a game group or the result of `GroupAdd` / `GroupCross`. |
| **ForIf** (GlobalVariable: object, grp: group, condition: float) (Command) | Like `For`, but only for the objects for which the condition is non-zero. |

### AI behaviours (UM commands)

| Function | Description |
|----------|-------------|
| **ResetTarget** (Unit: object) | Resets the unit's AI to its default behaviours: guarding, patrolling and aggression are cleared. Call it before compiling a patrol or guard list. The newest command (service pack 1.02). |
| **UMClear** (Unit: object) | Removes all of the unit's independent behaviours (the current order stays). |
| **UMStandard** (Unit: object) | The standard enemy: aggression + suspicion + corpse watching. |
| **UMAggression** (Unit: object) | Adds aggression: the unit attacks enemies in sight. |
| **UMSuspection** (Unit: object) | Adds suspicion: the unit goes to the nearest enemy it sees. |
| **UMCorpseWatcher** (Unit: object) | Adds corpse watching: the unit reacts to dead bodies. |
| **UMAg** (Unit: object, Ag: float) | Replaces the unit's fear / aggression / suspicion behaviours by a preset: `0` = always afraid (flees), `1` = afraid (fear without the always flag), `2` = revenge + fear; each also adds suspicion and corpse watching. |
| **UMFear** (Unit: object, Fear: float) | The unit never attacks and tries to hide from enemies (the default of neutral beasts). `Fear ≠ 0`: it is afraid of the player for good. |
| **UMRevenge** (Unit: object) | Conditional fear: the unit runs away from everyone hostile; it only fights when wounded or cornered. |
| **UMGuardEx** (Unit: object, x: float, y: float, rad: float, Delay: float) | The unit guards the point (x,y) within radius `rad`, pausing `Delay` at random stopping points. |
| **UMPatrol** (Unit: object) | Adds a patrol behaviour. The reference of the VGG editor says this command raises an error; to patrol, call `ResetTarget`, then `UMPatrolAddPoint`. |
| **UMPatrolAddPoint** (Unit: object, x: float, y: float) | Adds the patrol point (x,y). Does nothing when the unit has no patrol behaviour. |
| **UMPatrolAddPointLook** (Unit: object, x: float, y: float, Delay: float) | Sets the look-at point (x,y) and the stay time `Delay` of the last patrol point. |
| **UMPatrolClear** (Unit: object) | Removes all patrol points (`ResetTarget` also resets the other behaviours). |
| **UMPlayer** (Unit: object) | The unit obeys the rules of a player's unit (sensible behaviour; briefing zones use it for scripted NPCs). Clears its other behaviours and orders. |
| **SetEnemy** (Unit: object, nDiplomacyGroup: float) | Makes the diplomatic group hostile to the unit, as if one of its members had attacked it (the engine sets that bit of the unit's alarm mask; units only). |
| **InvokeAlarm** (nAlarm: float, x: float, y: float) | Raises the alarm `nAlarm` at the point (x,y); only one alarm is active at a time, a new number replaces the previous. Enemies with that alarm code in their `.mob` go to the point and guard it. `InvokeAlarm(0,0,0)` clears the alarm and they return to their places. |
| **SetPlayerAggression** (float, float) | Does nothing. |
| **SetSpellAggression** (Unit: object, Aggression: float) | Does nothing. |

### Spells, party & stats

| Function | Description |
|----------|-------------|
| **Cast** (Unit: object, x: float, y: float) | The unit casts its active area spell at the point (x,y); the spell must be one meant for a place, not for targets. |
| **CastSpellPoint** (constSpell: string, x1: float, y1: float, x2: float, y2: float) | Casts a terrain spell by its name: the effect starts at (x1,y1) and lands at (x2,y2). Area spells only. |
| **CastSpellUnit** (constSpell: string, x: float, y: float, Unit: object) | Casts a spell on the unit; the visual effect starts at (x,y). |
| **GiveUnitSpell** (Unit: object, sSpell: string) | Puts the spell into the unit's set of active spells (nothing happens when it is full). Does not work in the shop; on a zone the spell is given but the panel does not refresh. |
| **GiveSkill** (Unit: object, Skill: string, Amount: float) | Raises a skill (`science`, `melee`, `archery`, …) by `Amount` points: the experience needed is added to the unit (not the party) and spent on the skill, so Health and Stamina follow. |
| **GiveStrength** (Unit: object, Amount: float) | Adds strength points. Zones only; works on enemies as well. |
| **GiveDexterity** (Unit: object, Amount: float) | Adds dexterity points. Zones only; works on enemies as well. |
| **GiveIntelligence** (Unit: object, Amount: float) | Adds intelligence points. Zones only; works on enemies as well. |
| **GiveUnitQuestItem** (Unit: object, QuestItem: string) | Puts a quest item into the unit's backpack (a chest's contents). For a member of the player's party it goes to the party's wagon. |
| **RemoveQuestItem** (Unit: object, id: float) | Removes a quest item from the unit's backpack (only those given by `GiveUnitQuestItem` or listed in the `.mob`'s QuestItems). |
| **FixItems** () | Repairs every item of the current party: wagon, backpack, worn equipment and weapons. |
| **CopyItems** (nPlayer: float, NameSendRES: string, 'PartyName::NameRecvRES': string) | Copies the equipment (not wagon, not backpack) of a member of the current party to a member of another party of the same player. |
| **CopyStats** (nPlayer: float, NameSendRES: string, 'PartyName::NameRecvRES': string) | Copies skills, abilities and parameters of a member of the current party to a member of another party of the same player. |
| **CopyLoot** (nPlayer: float, PartyNameSend: string, PartyNameRecv: string) | Copies the wagon (items and gold) to another party's wagon, which is emptied first. Do not confuse wagon and backpack: `RedeployParty` empties the sender's backpack and the new party's one stays empty, though its items still show in the window. |
| **CreateParty** (nPlayer: float, PartyName: string) | Creates a party (replaces one with the same name; the name is case sensitive). |
| **RemoveParty** (nPlayer: float, PartyName: string) | Removes the party. |
| **SetCurrentParty** (nPlayer: float, PartyName: string) | Picks the current party. |
| **RedeployParty** (nPlayer: float) | Finishes the change of the current party. |
| **RemoveUnitFromParty** (nPlayer: float, NamePersRes: string) | Removes a character from its party (`'PartyName::Name'` reaches another party than the current). |
| **RemoveUnitFromControl** (nPlayer: float, Unit: object) | Removes the unit's icon from the player's control panel. |
| **GodMode** (nPlayer: float, Mode: float) | The current party's mode: `0` normal, `1` invulnerable when running and mana is free, `2` like 1, and every hit kills. |
| **RecalcMercBriefings** () | Single player: recalculates which dialogues companions and candidates offer. |
| **QuestComplete** (nPlayer: float, QuestName: string) | Completes the quest and pays its experience, like `QFinish` but without the message in the info window. |
| **QObjSeeObject** (ObjectName: string) | Dynamic multiplayer quests: the objective is to see this object (a chest, not a unit). A macro for ordinary commands. |
| **LeaveToZone** (nPlayer: float, MapZone: string, nEnterMap: float) | Moves the player (every party and the wagon) to the zone, to its entry place `nEnterMap` (numbers in `Res/texts.res/map.txt`). |
| **GSSetVar** (nPlayer: float, Var: string, nVar: float) | Sets the player's global variable (works differently in single and multiplayer). |
| **GSSetVarMax** (nPlayer: float, Var: string, nVar: float) | Sets the variable only when the new value is higher than the stored one. |
| **GSDelVar** (nPlayer: float, Var: string) | Deletes the player's variable. |

### Levers, areas, world & effects

| Function | Description |
|----------|-------------|
| **EnableLever** (Lever: object, Enable: float) | Lets the player use (`1`) or not use (`0`) a lever; a usable one shows the grabbing-hand cursor. |
| **SwitchLeverState** (Lever: object, State: float) | Puts the lever into the off (`0`) or on (`1`) position; `GetLeverState` reads it. |
| **SwitchLeverStateEx** (Lever: object, State: float, Delay: float) | Like `SwitchLeverState`, with a delay before the animation. |
| **SetScience** (Object: object, idQuestItem: float, SkillAmount: float, boolHands: float, boolLatchkey: float, boolCrowbar: float, boolKey: float) | Sets how a lever can be switched: by the quest item `idQuestItem`, with that much Sleight of hand, and by hands / lockpick / crowbar / key (`1` allowed, `0` not). |
| **DeleteArea** (idArea: float) | Deletes every part of the area `idArea`. |
| **SetWaterLevel** (idWBassein: float, Level: float, delay: float) | Sets the water level of a basin, relative to its bottom, after `delay`. Does not change where the party can wade. |
| **SetWind** (x: float, y: float, z: float, force: float) | A gust of wind toward the point (x,y,z) with the given force. |
| **SetBackGroundColor** (R: float, G: float, B: float) | Sets the colour the screen is cleared to (the sky behind everything); each value 0-255. |
| **FixWorldTime** (Time: float) | Sets the time of day and stops the clock. |
| **RunWorldTime** (Hour: float) | Sets the time of day and lets the clock run at the normal speed. |
| **SetSunLight** (sunlightR: float, sunlightG: float, sunlightB: float, ambientR: float, ambientG: float, ambientB: float, skyR: float, skyG: float, skyB: float) | Not implemented in game.exe (does nothing). |
| **AttachParticleSource** (id: float, idObject: float) | Attaches the particle source `id` to the unit with the ID `idObject`; it follows with the offset given in `CreateParticleSource`. |
| **AttachParticles** (id: float, Object: object) | Attaches the particle source `id` to the unit `Object`; it follows with the offset given in `CreateParticleSource`. |
| **MoveParticleSource** (id: float, x: float, y: float, z: float, Time: float, Remove: float) | Moves the particle source to (x,y,z) over `Time` (1/15 s); when `Remove` is set the source is deleted on arrival. |
| **SetParticleSourceSize** (id: float, rad: float) | Changes the radius of the particle source. |
| **MovePointLight** (id: float, x: float, y: float, z: float, Time: float, Remove: float) | Moves the point light to (x,y,z) over `Time` (1/15 s); when `Remove` is set the light is deleted on arrival. |
| **CreateFX** (x: float, y: float, z: float, Volume: float, rad: float, NameWav: string) | Plays a WAV once at (x,y,z), audible within `rad`. |
| **CreateRandomizedFXSource** (id: float, x1: float, x2: float, y1: float, y2: float, z1: float, z2: float, Volume: float, rad: float, NameWav: string) | A looped sound source `id` placed (randomized) inside the box x1..x2, y1..y2, z1..z2; remove it with `DeleteFXSource`. |
| **PlayFX** (x: float, y: float, z: float, rad: float, NameWav: string) | Not implemented in game.exe (does nothing). |
| **PlayCamera** (PlayName: string) | Starts the automatic camera path in `CAMERA\<PlayName>`. |
| **PlayMovie** (PlayName: string) | Plays the video `MOVIES\<PlayName>` (with its extension). |
| **PlayMusic** (nPlayer: float, PlayName: string) | Plays the piece `STREAM\<PlayName>` (no extension) for the player. |
| **SetCameraOrientation** (float) | Not implemented in game.exe (does nothing). |
| **SetCameraPosition** (float, float, float) | Not implemented in game.exe (does nothing). |
| **SetDirectionToObject** (nUnit: float) | Not implemented in game.exe (does nothing). |
| **ShowBitmap** (float, Bitmap: string) | Not implemented in game.exe (does nothing). |
| **ShowCredits** (string) | Does nothing. |

### Debug & messages

| Function | Description |
|----------|-------------|
| **ConsoleFloat** (float) | Prints a number to the console (synonym `#`). |
| **ConsoleString** (string) | Prints a string to the console (synonym `$`). |
| **SendEvent** (nPlayer: float, event: float) | Debug: broadcasts a number to the engine's players. |
| **SendStringEvent** (nPlayer: float, event: string) | Sends a string command to the engine: `"say sNN Name"` starts an NPC line (text `say sNN` in `texts.res`, sound from the `Acks` field), `say_block` the same but blocks the speaker, `"tutorial zt1"` a tutorial, `briefing`, `autosave`, `endofgame`. |

---

## Script validation (`MOB_VALIDATION` in um.dll)

With `MOB_VALIDATION=true`, `um.dll` checks each `.mob` file's mission script (the encrypted `SS_TEXT` node, dumped as `.eis` by `um-multitool mobdump`) when the game opens the file, and writes the findings to `um.log` as `[MOBCHECK]` lines. Line numbers count lines of the script text, so they match the dumped `.eis` file. Only maps inside the folder of the mod that contains `um.dll` are checked; other mods' maps and the base game's, which the game also opens, are left alone.

The language is small and regular: `GlobalVars`, `DeclareScript`, `Script`, `WorldScript`, `if ( conditions ) then ( ... ) [else ( ... )]`, nested `Command( args )` calls, `variable = value`, and the `For( var, group ) ( ... )` loop. The command list (name, parameter types, return type) comes from the VGG editor's `syntax.ini`/`scripts.htm` and MobExplorer's `script_refs.txt`, corrected against the shipped maps; see `resources/universal-mod/um-dll/mob_script_functions.hpp`. In um-multitool, `script_commands.txt` beside the program adds or changes commands for a game whose executable differs (see `tools/um-multitool/mapedit/README.md`, Checks).

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

