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
