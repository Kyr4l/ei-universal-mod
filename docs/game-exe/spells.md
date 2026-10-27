# Spells, buffs and effects in game.exe (CSpell, CSpellConstructor, CSpellManager, CSpellEffect, the buff record, CEffect*)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x676000-0x683000 the
CEffect* classes, 0x683000-0x68C000 the spell database / constructor / manager / CSpell, plus the callers: `CSpellEffect` 0x67E510-0x683A00,
`CWorldServer::CastSpell(Multi)` 0x58E670/0x58E850, the buff code of `CMapObject` 0x50F1E0-0x50F730 and `CMapObjectServer::TickBuffs`
0x515820, the cast side of `CUnitServer` 0x54B950-0x54EE10, the stat readers 0x523740-0x524560). The unit tick, orders and damage are in
`docs/game-exe/combat.md`; the items that carry spells belong to the items page. Batch files: `_cpr/claude-re/full/batch-spells.tsv`
(217 function rows) and `batch-spells-structs.tsv` (187 field rows). Two formulas were checked against the machine code with objdump
(the `d`/`t` rune arithmetic and the buff level), the rest is read from the decompiles.

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess. Addresses are VAs, offsets bytes,
"tick" = the server world tick `CWorldServer +0x64` (18/s). "proto index" = the row number in the `SpellPrototypes` table of `spells.sdb`.

## How to re-implement it (short version)

1. **A spell is a prototype row plus runes.** `spells.sdb` (`SpellsDb::Load` 0x684550) holds the `SpellPrototypes` rows (`SpellProto`, 0x84
   bytes, index = the spell *kind*), the `SpellModifiers` rows (`PerkProto`, 0x24 bytes: the runes `r0..r2 t1 t2 a0..a2 e0..e2 d0..d2 m1 m2
   ff fe fh fg fo fl it ic ...`) and three shop tables. `CSpellConstructor` (world +0x50) takes a blueprint and up to 8 runes and
   `CSpellContainer::Finalize` 0x68BF60 computes the result: `range += v * protoRange`, `power += v * protoEffect`, `area += v * protoArea`
   (radius = sqrt(area/pi)), `duration += trunc(v * protoDuration)`, `targets += trunc(v * protoTargets)`, `mana += v * protoMana`, filter bits;
   every rune also adds its own mana, price and skill requirement. The result is a `CSpell` (0x5C bytes) whose id = `kind << 16 | sequence`.
2. **Casting is an order** (combat.md 3): order 4 (point) / 5 (unit) with the inventory slot or the quick-item id; the unit needs
   `MagicSkill(school) >= spell +0x50`, mana `>= cost * 25 / int` and no feeblemind flag; cast time = `spell +0x4C * 15 / actionSpeed` ticks;
   at the end `CastOrderSpell` 0x54EE10 pays the mana and calls `CWorldServer::CastSpellMulti` 0x58E850, which picks the targets
   (explicit unit first, then the nearest live units within the range, visible, line of sight when the proto says so, filters) and makes one
   `CSpellEffect` (0xA8) per target with `CastSpell` 0x58E670.
3. **`CSpellEffect` is a small per-cast state machine** ticked by `CWorldServer::Update` (list +0x12E2C): phase 0 `Start` 0x67F870 (a
   `switch` on the kind: creates the visual effect objects, applies instant results), phase 1 `TickFlight` 0x681BB0 (arrows and the fireball
   until they arrive), phase 2 `TickActive` 0x682650 (walls / fog / campfire hit every 4 ticks, timed ends), phase 3 = removed.
4. **Damage goes through the normal combat path**: `BuildHitRecord` 0x683950 fills a 15-dword hit with the element factor
   (thermal/chemical/electrical = 1), damage = the spell power, roll = FLT_MAX (unresolved), location 6 (whole body); continuous spells
   scale the damage and the armor factor by `4 / duration` and hit every 4 ticks (so the power is the total). `target->vt[31]` = `CUnitServer::TakeHit`
   rolls and applies it exactly like a weapon hit.
5. **Buffs are 0x14-byte records on `CMapObject +0x198`**: `{kind = proto index, level, duration ticks, power, radius}`. `Start` copies the
   spell's block (`CSpell +0x1C`) with `level = trunc(16 * (pow(clamp(power/protoPower, 1, 8), 0.33) - 0.2))` and calls `AddBuff` (slot 45);
   `CMapObjectServer::TickBuffs` 0x515820 counts the durations down and removes expired ones; nothing prevents stacking (readers take the
   maximum power per kind). The readers: `UnitStats_Recompute2` 0x524560 (protections, senses, HP/mana maxima, action speed),
   `UnitStats_ApplyRangeBuffs` 0x523D30 (damage x `(1+0.003*str)/(1+0.003*weak)`), `Placement_Regenerate` (regeneration rate), slot 49
   (feeblemind flag), slot 46 (stun/teleport freeze), `ApplyBuffKinds` (enlarge/shrink/strengthen/weaken scale).
6. **Effect objects are server-owned, client-drawn.** `CWorldServer::AddEffectObject` 0x58E520 puts a `CEffect*` into the hash +0x12DE8,
   `ReplicateToPlayer` sends it as message 0x50 (full state, slot 22) and later 0x51 (delta); the client (`OnMsg_WorldObject` 0x668940)
   creates the same class through `CEffectManager::CreateByKind` 0x67E170 and its `ReadDelta` (slot 21) -> `Activate` (slot 23) builds the
   particle system / light / sound / figure. Each object's slot 6 `Update` runs on both sides; returning 0 removes it.

## 1. Conventions

| Thing | Value |
|---|---|
| spell kind | the proto index (row of `SpellPrototypes`, 0..0x29 in vanilla): `CSpell +0x18 >> 16`; it is also the **buff kind byte** and the switch key of the three `CSpellEffect` scripts [V] |
| spell id | `kind << 16 \| sequence`; sequence 0 = the bare prototype spell (`CreateSpellById` 0x688EC0), > 0 = a constructed spell registered in `CSpellManager +0xBC` [V] |
| spell object | `CSpell` 0x5C (kind 0x3002, vt 0x743A94, typeId 0x4F); its data block `+0x1C..+0x2F` is exactly a buff record [V] |
| container | `CSpellContainer` 0x90 (kind 0x3003): blueprint `+0x1C`, rune list `+0x20`, result `CSpell` at `+0x30` [V] |
| spell item | `CItemSpell` (kind 0x3008, 0x50): the `CSpell` at `+0x44`, its id at `+0x4C`; weapons / armor / quick items (subtype 5 charged, 8 scroll) carry their embedded spell at `+0x44` too (items page) [V] |
| managers | `CWorld +0x4C` `CSpellManager` (world `vt[22]()`), `+0x50` `CSpellConstructor` (world `vt[17]()`), `+0x48` `CEffectManager` (world `vt[14]()`) [V] |
| database globals | `g_0x7C8510/14` spell proto vector (0x84 stride), `g_0x7C851C` code map, `g_0x7C8530/34` perk vector (0x24), `g_0x7C853C` perk code map (the `SpellsDb` 0x64 object) [V] |
| schools | `proto +0x14`: 0 fire 1 lightning 2 acid (elemental, skill byte `UnitStats +0x6CE`), 3 illusion 4 divination (sense, `+0x6CF`), 5 enchantments 6 healing 7 domination (astral, `+0x6D0`); `MagicSkill = int - 25 + byte + perk bonus` (`UnitStats_MagicSkill` 0x527EB0) [V] |
| target kinds | `proto +0x30`: 'u' (117) unit -> flag 0x10000000, 't' (116) point -> 0x20000000, 's' (115) neither (shapechange) [V] |
| mana cost | `spell +0x40 * 25 / intelligence` (`UnitStats_ManaCostFactor` 0x523CD0), paid from `placement +0x34` by player units; charged items pay `charge +0x28 -= cost` [V] |
| cast time | `spell +0x4C (proto Actions) * 15 / actionSpeed` ticks (`BuildSpellAnimQueue` 0x550620) [V] |
| damage element | `CSpell::GetElementIndex` 0x68B700: kinds 0/3/6/0x28 -> 3 thermal, 1/4/7/0x29 -> 5 electrical, 2/5/8 -> 4 chemical [V] |
| particle types | `0x2000 + k` (CParticleManager), e.g. 0x2000 fireball, 0x2002 blast, 0x2006 heal, 0x2007 acid fog, 0x2010 fire wall, 0x2019 teleport [V] |
| sounds | `magic\<Spell>\start.wav`, `circle.wav` (loop), `end.wav`, `Firework\%i.wav` 1..5; volume 10, radius 50 [V] |

## 2. The spell database (`spells.sdb`)

`SpellsDb::Load` 0x684550 opens `spells.sdb` from the database `.res` (`CMappedResource`) and reads a tagged archive: tag 1 = the
`SpellPrototypes` rows, tag 2 = `SpellModifiers`, then a second archive with tags 3/4/5 = `S_Spells`, `S_Armors`, `S_Weapons` (the shop
sets, `SpellSetRecord` 0x34: prototype code, required runes, optional runes, power rune, shop bits). Both proto vectors get a code -> row map
(**keyed by the code column, not the name**; `CreateSpellByName` therefore takes `fireball{e1;r1}`, and `GiveUnitSpell` / `CastSpellPoint` in
scripts use codes). Field tags = the `FLD` numbers of the xlsx dump (`resources/universal-mod/xlsx-dump/databaselmp.md`):

### 2.1 `SpellProto` (0x84) [V] (`SpellProto::SerializeFields` 0x683B80)

| off | field | column | used by |
|---|---|---|---|
| +0 / +4 / +8 | name / code / subtype name (CStrings) | Name / Code / SubType | code map, `SpellsDb_ListSpellNames` |
| +0xC | price (float) | Price | `CSpell +0x48` |
| +0x10 | type 0 elemental 1 sense 2 astral | Type | type 1 casts silently (`CastOrderSpell`) |
| +0x14 | school 0..7 | SubTypeCode | `CanCastSpell` skill check |
| +0x18 | mana | Mana | `CSpell +0x40`, final `max(proto, computed)` |
| +0x1C | slots | Slots | `CSpell +0x44` base |
| +0x20 | speed (m/s) | Speed | fireball flight (`CSpell +0x30`); -1 = instant |
| +0x24 | range | Range | `CSpell +0x34` |
| +0x28 | area | Area | `CSpell +0x28` radius after `a` runes; `AreaFactor` |
| +0x2C | effect | Effect | `CSpell +0x24` power; `PowerFactor` |
| +0x30 | target char | Target | unit / point flag |
| +0x34 | targets | Targets | `CSpell +0x38` (`CastSpellMulti` count) |
| +0x38 | duration (ticks) | Duration | `CSpell +0x20` buff / wall life |
| +0x3C | actions | Actions | cast time `CSpell +0x4C` |
| +0x40 | req.trace | Req.Trace | 1 = line of sight (`ComputeCastTarget`) |
| +0x44 | complex | Complex | required magic skill `CSpell +0x50` |
| +0x48 / +0x54 | vector<dword> | Build-in Mods / Special Mods | item constructor only [?] |
| +0x60 | TTI | TTI | not read by the spell code |
| +0x64..+0x6C, +0x70, +0x74 | light rgb, light radius, fadeout | L:Red.. Fadeout Time | the point lights of the effects; drift = radius / max(fadeout, 1) |
| +0x78 | 5 bytes r/t/a/e/d | Range/Targets/Area/Effect/Dur. Mod | which rune kinds the blueprint accepts |
| +0x80 | shop bits | Shop 1..5 | shops |

### 2.2 The vanilla prototype table (indices = kinds = buff kinds) [V] (dump rows 0..0x29; the mod appends more)

| idx | code (name) | school | target | engine behaviour (section 5 / 6) |
|---|---|---|---|---|
| 0 | arrow (Fire Arrow) | fire | u | `CEffectArrow` 0x2011 -> thermal hit on arrival |
| 1 | lightning (Lightning Arc) | lightning | u | instant electrical hit, `CEffectLightning` |
| 2 | acid_ray | acid | u | arrow 0x2012 -> chemical hit |
| 3 | fireball | fire | t | particle 0x2000 flies at `speed`, thermal area hit |
| 4 | inv_lit (Invoke Lightning) | lightning | t | bolt from z+30, instant electrical area hit (radius) |
| 5 | acid_column | acid | t | particle 0x200C, chemical area hit 15 ticks after the start |
| 6 | firewall | fire | t | `CEffectWall` 0x2010, thermal beam hit every 4 ticks for `duration`, blocks the passability map |
| 7 | litnwall (Smtg Lightning) | lightning | t | lightning wall, electrical beam hit every 4 ticks |
| 8 | acid_fog | acid | t | particle 0x2007, chemical area hit every 4 ticks |
| 9 / 10 / 11 | prot_fire / prot_electro / prot_acid | fire / lightning / acid | u | buff: placement armor +0x3C / +0x4C / +0x44 += power (thermal / electrical / chemical) |
| 12 / 13 / 14 | eagle_sight / infravision / detect_life | divination | u | buff: senses `stats +0xB4 / +0xBC / +0xC4` += power |
| 15 / 16 / 17 | invisibility / silence / lichdom | illusion | u | buff: `stats +0xE4 / +0xFC / +0xF4` -= power (visibility / noise radius / third detectability); 15 sets the client flag 0x10000 |
| 0x12 | fireworks | illusion | t | particle 0x2009 + sound seq; every tick activates the objects around and `AiMemory::UpdateLastSeen` for units in the radius (lure) |
| 0x13 | stench | illusion | u | buff: `stats +0x104` += power; hostile |
| 0x14 | clairvoyence | divination | t | particle 0x2018; the units in the radius enter the caster player's known list |
| 0x15 | possession | divination | u | target and its neighbours join the caster's known list, target flag 0x2000000; needs game mode 1 or no caster |
| 0x16 | shapechange | illusion | s | no script: phase 3 at once (nothing happens) |
| 0x17 | vision_fog | divination | u | `CEffectShowVisibility(caster, target)`: the caster sees the target's sight map while it lives |
| 0x18 | healing | healing | u | `CUnitServer::Heal(power)` (slot 21), particle 0x2006 |
| 0x19 | stun | domination | u | buff 0x19: slot 46 freeze (unit flag 0x1000000) while present |
| 0x1A | antimagic | enchantments | u | buff: its power is max-ed into all three protections (+0x3C / +0x44 / +0x4C) [V]; counts as offensive for the AI |
| 0x1B | link | enchantments | u | `CEffectWall` 0x2014 between caster and target until one dies; refused (msg 0x18) when either is busy |
| 0x1C | teleport | enchantments | t | after `max(1, duration)` ticks the caster is moved to the point (`CanUnitBeAt` / `MoveUnit`), buff 0x1C freezes it, msg 0x10 when blocked |
| 0x1D / 0x1E | strength / weak | enchantments | u | buff: damage and ranges x `(1+0.003 s)/(1+0.003 w)`, max HP/mana x `(1+0.005 s)/(1+0.005 w)`, scale +-0.4 |
| 0x1F | regeneration | healing | u | buff: HP regeneration rate x power |
| 0x20 | feeblemind | domination | u | buff: unit flag 0x10000000 (cannot cast) |
| 0x21 / 0x22 | speed / slow | enchantments | u | buff: action speed `+ power` / `- power` |
| 0x23 | comprehend | domination | u | no script |
| 0x24 | charm | domination | u | `CUnitServer::OnProvoked(caster, power + target +0x200)` until the target's player matches; msg 0x1A when resisted |
| 0x25 / 0x26 | enlarge / shrink | enchantments | u | buff: morph scale +-0.4 (`ApplyBuffKinds`) |
| 0x27 | campfire | fire | t | thermal "hit" every 4 ticks in the radius for `duration` (the hit record of kind 0x27), blocks the map |
| 0x28 / 0x29 | rick_magic / curse_magic | fire / lightning | u | the arrow (0x204E) / lightning (style 1) variants used by scripts |

### 2.3 `PerkProto` (runes, 0x24) [V] (`PerkProto::SerializeFields` 0x683AF0)

`+0 name, +4 code, +8 price, +0xC type (0 r 1 t 2 a 3 e 4 d 5 m 6 filter 7 item), +0x10 mana, +0x14 value v, +0x18 complex, +0x1C allod,
+0x20 shop bits`. The vanilla values: `r0 0.5, r1 1, r2 2; t1 1, t2 2; a0 0.5, a1 1, a2 2; e0 0.5, e1 1, e2 2; d0 0.5, d1 1, d2 2;
m1 -20 mana, m2 -40; ff/fe/fh/fg/fo/fl filters; it / ic item runes; ee1..ee5 (0.1..10) and c1..c5 colours` (mod extras).

## 3. CSpell, the constructor and the manager

### 3.1 `CSpell` (0x5C, kind 0x3002) [V]

| off | field | from the proto (`InitFromProto` 0x68B430) | changed by runes (`Finalize` 0x68BF60) |
|---|---|---|---|
| +0x18 | id | `kind << 16` | sequence from the registry |
| +0x1C / +0x1D | buff kind byte / level | kind, 0x10 | - (the level is recomputed at cast time) |
| +0x20 | duration | Duration | `d`: `+= trunc(v * protoDuration)`, clamped >= 0 (objdump-verified `fild [proto+0x38]; fmul [perk+0x14]; call ftol`) |
| +0x24 | power | Effect | `e`: `+= v * protoEffect` |
| +0x28 | radius | Area | `a`: area `+= v * protoArea`, then `radius = sqrt(area / pi)` when > 0 |
| +0x30 | speed | Speed | - |
| +0x34 | range | Range | `r`: `+= v * protoRange` |
| +0x38 | targets | Targets | `t`: `+= trunc(v * protoTargets)` (verified) |
| +0x3C | flags | 'u' 0x10000000 / 't' 0x20000000 | `ff` 1, `fe` 2, `fh` 4, `fg` 8, `fo` 0x10, `fl` 0x20, `it` 0x10000, `ic` 0x20000 |
| +0x40 | mana | Mana | `m`: `+= v * protoMana`; every rune `+= perk mana`; final `= max(protoMana, mana)` |
| +0x44 | slots | Slots | `+1` per rune |
| +0x48 | price | Price | `+= perk price` per rune |
| +0x4C | cast actions | Actions | - |
| +0x50 | required skill | Complex | `+= perk complex` per rune |
| +0x54 | perks[8] | 0 | perk index + 1 per rune, sorted ascending, 0-terminated |

`CSpell::GetProto` 0x68BA20 = `g_0x7C8510[id >> 16]`. `IsValidTarget` 0x68B4C0 applies the filter bits: `ff` needs a non-enemy
(player masks `+0x244/+0x248/+0x250`), `fe` an enemy, `fh/fg/fo/fl` a model name `unhuma|unhufe / unmogo / unorma|unorfe / unmoli`; any
filter needs a unit target. `IsHostileSpell` 0x68B770 (0..8, 0x11, 0x13, 0x19, 0x1E, 0x20, 0x22, 0x24, 0x26) and `HasItemPerk` 0x68B7C0
(`ic` / `it`) classify spells for the AI and the items. Save format (`Write` 0x68B920): buff block (1,1,4,4,4), `+0x30..+0x4C`, perks (8),
`+0x8`, `+0x50` = the 66 bytes of game-memory.md.

### 3.2 The constructor [V]

`CSpellConstructor` (0x24: `+0x1C` manager, `+0x20` the `CSpellContainer`). `CreateSpellByName` 0x689030 (scripts, items, traps) and
`CreateSpellWithPerks` 0x689310 (the item constructor's random spells) and the camp spell constructor UI all do:
`SetManager` -> `BeginConstructSpell(proto)` 0x6841E0 (a new container from `CSpellManager::CreateContainer` 0x688EB0, `+0x1C = proto`) ->
`AddRune(perk)` 0x6842A0 (refused unless `IsPerkKindAllowed` 0x68BEC0: the first letter r/t/a/e/d must have its byte in `proto +0x78..`;
other letters always pass; `Finalize` after each) -> `EndConstructSpell` 0x684250 (`Finalize`) -> `CSpell::FillFromId` 0x68B410 registers a
copy: `CSpellManager::CreateSpellById(id)` 0x688EC0 allocates a `CSpell`, and for a non-zero low id looks the container's registry up
(`+0xBC` hash keyed by `id & 0xFFFF0000` -> sub-table keyed by `id & 0xFFFF`, "Can't find spell sequence to create child"). The manager
tracks `+0xD0` the largest high id and per sub-table `+0x14` the largest low id [V]; how the fresh sequence number is chosen for a new
construction was not followed (`FillFromId` passes the container's own id) [?].

### 3.3 Where spells live

* inventory slots `CUnit +0x460[8]` = `CItemSpell` items (`CUnit::GetItemSpell(i)` 0x531A10 returns `item +0x44`); the cast order's
  param is the slot index; `CUnit::SpellUsability` 0x52F820 gives the HUD state (3 none, 4 feebleminded, 1 no mana, 0 ok).
* quick items (`CItemQuick` 0x13) subtype 5 = charged wand (charge `+0x28`, cost per cast), subtype 8 = scroll (cast at once by
  `QueueCastAt/On` 0x554D00/0x554F50 with `Unit_ConsumeItemCharge` 0x595020); their id in the order has bit 31 set.
* embedded in weapons / armor (`item +0x44`, flag bit 16): `TriggerWeaponSpell` 0x54F850 after every attack on the target;
  `TriggerArmorSpells` 0x54F8E0 on self when hit (all but healing) and on death (healing only); `ic` (continuous) spells of equipped
  items are listed in `CUnitServer +0x920` (rebuilt by 0x5528F0) and cast on self by `TickPassiveSpells` 0x552B90 every
  `duration - 3` ticks (min 8) while affordable (healing only while hurt).
* `CMagicTrapObject` (+0x23C spell from `CreateSpellByName`) casts at the units entering its areas; scripts `CastSpellPoint` /
  `CastSpellUnit` / `GiveUnitSpell` (gs-functions.md 3.13) use `CreateSpellByName` + `CastSpellMulti` with no caster.

## 4. Casting (server)

### 4.1 Checks before the cast [V]

| check | where | rule |
|---|---|---|
| skill | `CanCastSpell` 0x54B950 (from `QueueCastAt/On`, player units only) | `UnitStats_MagicSkill(proto +0x14) >= spell +0x50`, else message 0x13; unit flag 0x10000000 (feeblemind) refuses every spell |
| mana / charge | `CanAffordSpell` 0x54BF80 (`ValidateQueuedOrder` -> message 3) | `placement +0x34 >= cost * 25/int`; wands: `charge +0x28 >= cost` |
| range | `OrderReachDistance` (combat.md 3.4) | `spell +0x34` (10 without a spell); monsters refuse targets beyond `sight * 1.05` (`CanCastAt` 0x54C2F0) |
| line of sight | `ComputeCastTarget` 0x54BBC0 | only when `proto +0x40` (Req.Trace): `CAIMap::TraceVisibility(casterEye, targetPoint + height) >= 0.1` |
| stance | `UpdateStanceFlags` | humanoids cast from crawl -> stance 1 (combat.md 3.2) |

### 4.2 The cast [V]

`PlanActionApproach` builds the casting clip (`BuildSpellAnimQueue` 0x550620: key `attack kind mask | 0xC0000`, `| 0xD0000` in combat mode,
stance 1 `| 0x28E0000`, random side bit; non-humanoids 0x2890000) and pads it with a hold entry (clip 0xFF) when the clip is shorter than
the cast time. At the action end (`CompleteOrder`) `CastOrderSpell` 0x54EE10: the spell object, the target point (target display position or
the order position), the mana (`placement +0x34 -= cost * 25/int`, clamped to `0..max`, net 0) or the charge, then
`CWorldServer::CastSpellMulti(spell, owner, ownerPos, target, point, quiet 0)` and a noise stimulus of radius `stats +0xFC * 2` (kind 0x1A)
unless the proto type is 1 (sense school). A cast is lost when the target leaves the reach (state 5 `CancelAction`), when the order is
replaced, or when the unit dies; a hit reaction does not interrupt it (combat.md 7).

### 4.3 Target selection (`CastSpellMulti` 0x58E850) [V]

* possession 0x15 / link 0x1B / charm 0x24 need `world +0x12D64 == 1` (game mode) or no caster;
* no filter bits and `targets < 2` -> one `CastSpell` on the given target / point;
* otherwise: an explicit unit target gets the first cast (skipped for `ic` spells), then `CollisionMap::FindUnitsNear(casterPos, range +0x34)`;
  the live units are taken nearest-first to the target point, each must be visible to the caster (player: the known list `player +0x510C`;
  monsters: the side mask `+0x12C64` or its neighbour list `+0xA94`), pass `ComputeCastTarget` and `IsValidTarget`; one `CastSpell` per
  unit until the count is used (a `t2` fireball therefore explodes on the explicit point and on up to two more units).

`CastSpell` 0x58E670: point-target spells (`0x20000000`) drop the unit; the point's z = terrain height; `new CSpellEffect` (0xA8),
`Init(world, spell, target, pos, caster, point, quiet)`, `Update()` once (= `Start`); when still alive the cast is linked into the world's cast
list (`+0x12E28/+0x12E2C/+0x12E30`, refcounted) and `CWorldServer::Update` ticks it every world tick. Casting a hostile non-damage kind
(0x13, 0x19, 0x1E, 0x20, 0x22, 0x26) on a unit counts as an attack at once: `UnitAI_OnAttackedBy`, `AggroTable_Add(caster, target, 10)`,
alarm flag 0x80000 on both; the damage kinds aggro later through `TakeHit`.

### 4.4 What the client gets

The client never sees `CSpellEffect`: it receives the effect objects (section 7) and the buff list (`CMapObject` net section 3: count +
per buff `{kind, level, 2 bytes, flag +0x10}`), the placement (section 0: mana) and the packed stats. The HUD spell icons and the camp
constructor read the `CSpell` copies in the party items (`UIItemInfoWin::DrawSpellItemInfo` 0x5EF5D0 formats `spell <id>` texts from
`CSpell::GetProto`).

## 5. `CSpellEffect` (0xA8, the per-cast state) [V]

| off | field | | off | field |
|---|---|---|---|---|
| +0x8 | world | | +0x34 | copy of the `CSpell` (`+0x4C` id, `+0x50` buff block, `+0x54` duration, `+0x58` power, `+0x5C` radius, `+0x64` speed, `+0x68` range, `+0x6C` targets, `+0x70` flags) |
| +0xC | flags: bit 0 = continuous hit applied | | +0x90 / +0x94 / +0x98 | main / sound / light `CEffect*` refs |
| +0x10 | position | | +0x9C | volumes placed |
| +0x1C / +0x20 | target / caster refs | | +0xA0 | countdown |
| +0x24 | target point | | +0xA4 | quiet (1 = passive item cast) |
| +0x30 | phase 0 Start, 1 TickFlight, 2 TickActive, 3 done | | | |

Common helpers: `PowerFactor` 0x67EAA0 `= pow(clamp(power / protoEffect, 1, 8), 0.33) - 0.2` (1.0 when the proto effect is 0; 0.8 for an
unmodified spell, ~1.24 for `e2`), `AreaFactor` 0x67EB60 the same of `pi r^2 / protoArea`; they scale particle sizes, light radii, lightning
widths and the buff level. `Create*` 0x67F170-0x67F7C0 make the effect objects (section 7) and `CWorldServer::AddEffectObject` them.
`UpdateVolumes` 0x67EFC0: walls (half circle of radius 0.5 along the wall), acid fog and campfire (circle of the radius) stamp
`CAIMap::PlaceObjectVolumes` so units path around them; removed at the end.

### 5.1 The hit record of a spell (`BuildHitRecord` 0x683950) [V]

```
hit[0..6] = 0, hit[element] = 1.0 (3 thermal: fire kinds, 4 chemical: acid, 5 electrical: lightning)
hit[8]  = FLT_MAX   (unresolved: CUnitServer::TakeHit rolls it against the defence)
hit[9]  = power (+0x58)         hit[10] = 1.0 armor factor      hit[11] = 6 whole body     hit[13] = 1.0
hit[12] = 1 for the continuous kinds 5 / 6 / 7 / 8 / 0x27 (OnDamaged direction code), else 0
duration > 1 (continuous): hit[9] *= 4 / duration, hit[10] *= 4 / duration   # applied every 4 ticks -> power in total, armor scaled alike
```
`ApplyHitTo` 0x6835B0 (`IsValidTarget`, then `target->vt[31](hit, caster, caster)`; with flag bit 0 the attacker argument is 0, so a wall
does not re-trigger aggro every 4 ticks), `ApplyHitArea` 0x683600 (every hittable unit within `+0x5C` of `+0x10`: `CUnit_IsDeadOrDying`
0x681B80 = `+0x324 != 0` or proto `+0x24 < 0.5`), `ApplyHitBeam` 0x683730 (units within 0.5 m of the wall line). Nothing else is special:
`TakeHit` (combat.md 4.4) applies the element to the placement armor (where the protection buffs live) and the body parts.

### 5.2 The scripts (`Start` 0x67F870 / `TickFlight` 0x681BB0 / `TickActive` 0x682650) [V]

| kind | Start (phase 0) | flight (1) | active (2) |
|---|---|---|---|
| 0, 0x28, 2 arrows | needs a unit target; `CreateArrow` from the caster's attach point (type 0x2011 / 0x204E / 0x2012, intensity `PowerFactor` (x0.4 acid)), start sound, countdown 150 | the `CEffectArrow` flies 0.667 m/tick toward the target centre; on `arrived`: hit, hit particle 0x2041/0x2042 + end sound + light x1.5 at the target, countdown 0x23 | countdown, then done |
| 1, 0x29 lightning | `CreateLightningToUnit` (width `5 * PowerFactor`), start sound, light; **hit applied at once**; countdown 7 | - | at 0 the bolt is removed and the light drifts up |
| 3 fireball | particle 0x2000 (scale `0.3 * PowerFactor`) from the caster to the point, speed `+0x64` m/s (5 when <= 0), countdown `round(dist / speed * 15) - 2`; far casts loop `Fireball\circle.wav`, near ones play `start.wav`; light; the particle's `+0x17C` = caster | countdown; at < 0: pos = point, `ApplyHitArea`, blast 0x2002 (scale `min(PowerFactor, AreaFactor)`), end sound, light x1.5, `StimulusMap::HearSound(caster, 5.0)`, countdown 0x23 | countdown, done |
| 4 invoke lightning | lightning from `z + 30` down to the point (width `-7 * factor`), particle 0x2030 at the point, `InvokeLight\start.wav` (volume 1000), light, **instant `ApplyHitTo` on every live unit within the radius**, countdown 7 | - | at countdown 5 a second bolt, then done |
| 5 acid column | particle 0x200C, start sound, light, countdown 30 | - | at countdown 15 `ApplyHitTo` every live unit within the radius; done at < 0 |
| 6 fire wall | `CEffectWall` 0x2010 at the point with direction = perpendicular of (point - caster) scaled to the radius, `Firewall\circle.wav` loop, light (radius + proto radius), countdown = duration, volumes | - | every 4 ticks `ApplyHitBeam(direction)`; at -1 the volumes / wall / loop end (`end.wav`), light drifts; done at -0x50 |
| 7 lightning wall | a `CEffectLightning` between the two wall ends (terrain + 1), `Lightwall\circle.wav`, light, countdown = duration | - | like 6 along the bolt |
| 8 acid fog | particle 0x2007 (scale radius), `poison\circle.wav`, light, countdown = duration, volumes | - | every 4 ticks `ApplyHitTo` every live unit within the radius; `end.wav` at -1 |
| 9..0x11, 0x13, 0x19, 0x1A, 0x1D..0x22, 0x25, 0x26 buffs | the buff block `+0x50` copied, `level = trunc(16 * PowerFactor)` (objdump: `fsub 0.2; fmul 16.0; call ftol`), `target->vt[45] AddBuff(buff, quiet)`; **done at once** | - | - |
| 0x12 fireworks | particle 0x2009, light, `Firework\1..5.wav`, countdown = duration | - | every tick: `World_ActivateObjectsAround(radius + 5)` and `AiMemory::UpdateLastSeen` of the units in the radius; `end.wav` at -1, done at -0x51 |
| 0x14 clairvoyance | particle 0x2018 (scale radius) at the point, light, start sound, countdown = duration | - | every tick the units within the radius are inserted into the caster player's known objects (`player +0x20 + 0x50F8`) and the objects activated; at countdown 10 the end particle / sound |
| 0x15 possession | start sound at the target | - | the target and its neighbour lists (`+0xA90 / +0xAAC`) join the caster player's known list, `target +0x8 \|= 0x2000000`, player `+0x24 = target id`, activation radius `2 * sight + 20`; countdown = duration |
| 0x17 vision fog | `CEffectShowVisibility(caster id, target id)`, start sound; countdown = duration | - | runs while the target is alive (`vt[55]`) |
| 0x18 healing | `target->CUnitServer vt[21] Heal(power)` (0x5543B0: `UnitStats` heal, recompute, net 0/4), particle 0x2006 on the target; sound and light unless quiet and already at full HP; countdown 30 | - | countdown, done |
| 0x1B link | both must be idle units (`vt[54]` / `vt[55]` clear) and different, else player message 0x18; `CEffectWall` 0x2014 from the caster toward the target, `Link\circle.wav`; countdown = duration | - | ends with `end.wav` + message 0x19 when either dies / is removed, or at the end of the duration |
| 0x1C teleport | particles 0x2019 at the caster (scale 1) and the point (scale -1, z - 0.2), start sound, light; countdown = `max(1, duration) + 30` | - | at countdown == `max(1, duration)`: `CAIMap::CanUnitBeAt(caster, point)` -> `CollisionMap::MoveUnit`, caster gets buff 0x1C (frozen for the duration), `ResetOrdersAndStance`; blocked -> message 0x10 |
| 0x24 charm | needs two units; `CUnitServer::OnProvoked(target, caster, power + target +0x200)` repeated until the target's player == the caster's player; a refusal -> message 0x1A; charm start sound | - | - |
| 0x27 campfire | pos = point (terrain + 1), countdown = duration, volumes | - | every 4 ticks `ApplyHitTo` every unit in the radius (the hit record of kind 0x27: thermal, direction 1) |
| 0x16, 0x23 and unknown | phase 3 at once | | |

## 6. Buffs

### 6.1 The record and the list [V]

`Buff` (0x14): `+0 kind` (proto index), `+1 level` (client scale), `+4 duration` ticks, `+8 power` (`CSpell +0x24`), `+0xC radius`,
`+0x10 flag byte`. `CMapObject +0x194` is a CArray of them (`+0x198` data, `+0x19C` count, `+0x1A0` capacity), saved by
`CMapObject::Read/Write` (`Buff_Read/Write` 0x688550/0x6885A0) and replicated in net section 3.

* `AddBuff` 0x50F1E0 (slot 45, `(buff, applyScale)`): append (no duplicate check), net section 3 dirty, `vt[33](scaleKind)` (CUnit:
  specific slot 19 `RecomputeFromPlacement`), stun 0x19 / teleport 0x1C -> `vt[46](1)` (flag 0x1000000 frozen: the unit's `Update` skips
  Think/TickState), `vt[28]` (= `CUnitServer::v49_UpdateFeeblemind` 0x553220: flag 0x10000000 when a 0x20 buff exists and the natural
  attack kind is not 0x10), and `vt[27] ApplyBuffKinds` when `applyScale`.
* `CMapObjectServer::TickBuffs` 0x515820 (slot 6, every server tick): `--duration`; expired records are removed (memmove), section 3 dirty,
  `vt[33]`, `vt[28]`, and `vt[46](0)` when no stun / teleport buff is left. Dying clears everything (`ClearBuffs` 0x50F5B0 from
  `OnDamaged` and `CMapObjectServer::TakeHit`).
* Readers: `HasBuffKind` 0x50F630 (any record with the kind), `GetBuffValue` 0x50F690 (the largest `+8 power` of the kind, 0 when absent).
  Stacking therefore means "the strongest one counts" for the stat effects, but `ApplyBuffKinds` scales by presence only.

### 6.2 What each buff kind does (server) [V]

| kind | reader | effect |
|---|---|---|
| 9 prot_fire, 0x1A antimagic | `UnitStats_Recompute2` 0x524560 | `placement +0x3C` (thermal armor current) `+= max(power 9, power 0x1A)` |
| 10 prot_electro | same | `+0x4C` (electrical) `+= max(power 10, power 0x1A)` |
| 11 prot_acid | same | `+0x44` (chemical) `+= max(power 11, power 0x1A)` |
| 12 / 13 / 14 eagle sight / infravision / detect life | same | `stats +0xB4 / +0xBC / +0xC4` (sight / night sight / third sense) `+= power` |
| 15 / 16 / 17 invisibility / silence / lichdom | same | `stats +0xE4 / +0xFC / +0xF4` (visibility / noise radius / detectability) `-= power`; 15 also sets the client flag 0x10000 (`UpdateBuffEffects`) |
| 0x13 stench | same | `stats +0x104 += power` |
| 0x19 stun / 0x1C teleport | `AddBuff` / `TickBuffs` | unit flag 0x1000000 (frozen) while present; client flag 0x40000 for stun |
| 0x1D strengthen / 0x1E weaken | `UnitStats_ApplyRangeBuffs` 0x523D30, `Unit_BuffHPRatio` 0x523DB0, `Unit_BuffSpeedRatio003` 0x5285D0, `ApplyBuffKinds` | damage rolls and ranges x `(1 + 0.003 s) / (1 + 0.003 w)` (capped at 1 for the fraction forms), max HP / max mana x `(1 + 0.005 s) / (1 + 0.005 w)`, morph scale `+-0.4` |
| 0x1F regeneration | `Placement_Regenerate` 0x523740 | HP regeneration rate `*= power` (when > 0) |
| 0x20 feeblemind | slot 49 | flag 0x10000000: no casting (`CanCastAt`, `QueueCastAt`, `SpellUsability` 4) |
| 0x21 speed / 0x22 slow | `UnitStats_Recompute2` | action speed `+= power 0x21 - power 0x22` (`max(1, ...)`), i.e. attack period and cast time |
| 0x25 enlarge / 0x26 shrink | `ApplyBuffKinds` 0x50F2D0 | morph scale `+0xC4/+0xC8/+0xCC += 0.4 * (has 0x25 - has 0x26)` (height skipped for the 0x1D/0x1E pair on humanoids), net section 1 |
| any | `AiSpellInfo_Score` 0x5CF9A0 | the AI never re-casts a buff the target already has (`HasBuffKind`) |

### 6.3 Client visuals [V] (`CMapObjectClientSpecific`)

`UpdateBuffEffects` 0x510B80: the buffs reduced to one record per kind (highest level); per kind a particle system attached to the
object: 9 -> 0x2017, 10 -> 0x2016, 0xB -> 0x2015, 0xC -> 0x2044, 0xD -> 0x2045, 0xE -> 0x2046, 0x10 -> 0x204C, 0x13 -> 0x2013,
0x1A -> 0x201A, 0x1F -> 0x2048, 0x20 -> 0x2049, 0x21 -> 0x204B, 0x22 -> 0x204A, scaled by `level / 16`; 0xF invisibility -> owner flag
0x10000 (drawn translucent), 0x19 stun -> flag 0x40000; 0x11 / 0x1D / 0x1E play a one-shot scale effect 0x2018 / 0x201B. Vanished kinds
stop their effect and undo the flags. `UpdateBuffSounds` 0x5112A0 / `BuffSound_Start` 0x5114D0: `magic\<Folder>\start.wav` per new kind and
`end.wav` when it vanishes (0x20 feeblemind loops `circle.wav`), folders: 9 Fireshield, 10 Lightsheild, 0xB PoisonShield, 0xC Eaglesight,
0xD Infravision, 0xE Detectlife, 0xF Invisibility, 0x10 Silence, 0x11 Lichdom, 0x19 Stun, 0x1A Antimagic, 0x1D Strengthen, 0x1E Weaken,
0x1F Regeneration, 0x20 FeebleMind, 0x21 Speed, 0x22 Slow, 0x25 Enlarge, 0x26 Shrink.

## 7. Effect objects (`CEffect*`)

### 7.1 Base and lifetime [V]

`CEffect` = `CObject` (+0x4 refcount, +0x8 flags, +0xC id, +0x10 typeId = kind) + the `CObject3D` transform (+0x1C pos, +0x28, +0x34, +0x44)
+ `+0x94 startTick`, `+0x98 velocity`, `+0xA4 moving`, `+0xA8 DeltaCodec` (four CMemStreams: +0xF8 the full net state, +0x120 the delta),
`+0x148 lastChangeTick`. `SetVelocity` 0x675D50 / `MarkDirty` 0x675CA0 set flag bit 1 and the tick. Server: `AddEffectObject` 0x58E520
(`+0x94 = tick`, hash `+0x12DE8`), `CWorldServer::Update` calls slot 6 of every effect and removes those returning 0, `RemoveEffectObject`
0x58E5D0 by id. Replication (`ReplicateToPlayer` 0x58B750): dirty objects go as message 0x50 `{id, typeId, full state}` (`WriteNetStateB`
0x675B70 = slot 22 into +0xF8) the first time and 0x51 (delta +0x120) later; the client `OnMsg_WorldObject` 0x668940 creates the class
(`CEffectManager::CreateByKind`), `CEffect::ReadDelta` 0x675BA0 -> slot 21 (reads the fields; `update` = 0 the first time) -> slot 23
`Activate(update)` builds or updates the client resource. The CSpellEffect's `Remove*` drop the server object; the client removes its copy
when told (`CWorldClient::RemoveObject`).

Vtable (24/25 slots, hierarchy.md): 0 dtor, 2 `InitNetStream`, 3 Done (= Reset), 6 `Update` (0 = remove), 9/10 Read/Write (save),
11/12 ClassName/TypeId, 13..17 CObject3D transform, 18 `Reset`, 19 `GetPercent` (volume / 100), 20 `GetPos`, 21 `ReadDelta(update, stream)`,
22 `WriteDelta`, 23 `Activate(update)`, 24 `UpdateAttached` (arrows). The 18-slot classes (particle, light, wall, lightning, arrow) reach
their Reset / ReadDelta / Activate through the same slot numbers of their own vtables [I].

### 7.2 The classes (`CEffectManager::CreateByKind` 0x67E170) [V]

| kind | class (size) | fields | server `Update` | client `Activate` |
|---|---|---|---|---|
| 0x48 | `CEffectSound` (0x264) | +0x14C wav name[256], +0x24C volume, +0x250 radius, +0x254 one-shot, +0x258 loop, +0x25C handle | moves with the velocity; removed when the one-shot flag is clear | `CSoundManager::Play3D` 0x6B8600 (loop 1000), `SetChannelPosition` |
| 0x49 | `CEffectAmbientSound` (0x174) | +0x14C names, +0x160 volume, +0x170 handle | - | `PlayToSlots` / `SetChannelParams` (script ambient sounds) |
| 0x4A | `CEffectParticle` (0x180) | +0x158 type, +0x15C scale, +0x160 intensity, +0x164 radius, +0x168 attach unit, +0x16C target point, +0x178 CParticle, +0x17C source unit | flies toward +0x16C at \|velocity\| per tick (z follows the terrain); one-shot types end 60 ticks after the start (0x2019: 120) | `CParticleManager::CreateParticleSystem(type)`, scale / intensity, attach to the unit, pre-run `tick - start` steps (max 30; not for the one-shot types), 0x2002 / 0x2030 add a scorch print, 0x2030 shakes the camera |
| 0x4B | `CEffectPointLight` (0x164) | +0x14C colour, +0x158 radius (< 0 = fading 0x400), +0x15C CLight, +0x160 flags 0x840 | moves; re-adds the light every 16 frames | `CLight` kind 0x31, `CTerrain::AddLight` / `MoveLight` |
| 0x4D | `CEffectLightning` (0x178) | +0x14C from, +0x158 to, +0x164 width, +0x168 CLightning, +0x16C/+0x170 unit ids, +0x174 style | endpoints follow the units | `CLightning::Init(from, to, width, style)`; `InitRandom` every tick (new bolt shape) |
| 0x4E | `CEffectArrow` (0x6C0) | projectile base (+0x14C arrived, +0x150 target, +0x154 from attach point) + embedded particle +0x158 / light +0x2D8 / sound +0x43C, +0x6A0 sound, +0x6A4 colour, +0x6B0 radius, +0x6B4 particle type, +0x6B8 intensity, +0x6BC caster | `ComputeFlightVector` 0x67AAB0: toward the target centre (pos + +0x130 + height/2) at 0.667 m/tick, z >= terrain + 1; `arrived` when closer than a step; returns 0 when arrived | the three sub-effects are activated at the arrow position (`Reactivate` slot 24 moves them every tick) |
| 0x5A | `CEffectWall` (0x18C) | `CEffectParticle` + +0x180 direction (half extent) | as the particle | `ReadDelta` passes the direction to the system (+0x130); the fire wall adds a scorch print |
| 0x5B | `CEffectArrowFigure` (0x19C) | projectile base + +0x158 figure, +0x15C shooter, +0x160 the resolved `Hit` (15 dwords) | as the arrow at 1.333 m/tick; **on arrival `target->vt[31](hit, shooter, shooter)`**, then removed | figure object kind 0x38, model `efar1` / anim `arrow01`, aligned to the velocity (`UpdateAttached` 0x67BEF0) |
| 0x5E | `CEffectShowVisibility` (0x170) | +0x14C viewer, +0x150 watched unit, +0x154 CVisibilityFog, +0x160 cell grid | `SightMap_Build(watched, viewer, grid)` every tick, dirty; removed when a unit is gone | `CVisFogManager::CreateAndRegister`, `RebuildCells` from the grid |
| 0x63 | `CEffectSoundSeq` (0x174) | +0x14C folder, +0x150 file list, +0x164 volume, +0x168 radius, +0x170 slot handle | moves | `PlayOnSlot` sequences (the `circle.wav` loops, `Firework\1..5.wav`) |
| 0x64 | `CEffectMoshka` (0x16C) | the hero's fly (model `unmobi`): not a spell, client only (`SetNamedObjectState` 2.0) | | `CreateBird` / `UpdateFlight` 0x67CEF0 |
| 0x65 | `CEffectTornado` (0x15C) | weather (`UpdateEnvironment` mode 3): particle 0x200A, sound, +0x154 age | after 20 ticks every 8 ticks: every unit within 5 m takes a hit of 100 damage (location 6) | particle + `nature\tornado\circle.wav` |

The ranged attack of combat.md (`FireAttack` 0x54F450) uses kind 0x5B: the hit is resolved when the shot starts and carried in the
object; the client only animates the figure.

### 7.3 `CSpellEffect` -> effect objects

`CreateArrow` 0x67F170 (0x4E), `CreateParticleEffect` 0x67F260 (0x4A, or 0x5A for the types 0x2010 / 0x2014), `CreateLightningToUnit`
0x67F410 / `CreateLightning` 0x67F500 (0x4D), `CreateSoundEffect` 0x67F580 (0x48, volume 10 radius 50), `CreateSoundSeq` 0x67F660 (0x63),
`CreatePointLight` 0x67F7C0 (0x4B, colour / radius from the proto `+0x64 / +0x70`). The main object is kept at `+0x90`, the sound at
`+0x94`, the light at `+0x98`; `RemoveEffectObjects` 0x67ED60 drops all three; the end effects of the walls give the light a drift
velocity (`LightDriftVector` 0x67EC40) and let it fade (negative radius -> flag 0x400) instead of removing it.

## 8. Client side summary

The client computes nothing about spells: it shows the spell icons / usability from the item copies (`SpellUsability`), sends the cast
request (`CPlayerServer::OnMsg_CastAt/On` 0x6708D0/0x670820 -> `AiLogicPlayer::CastAt/On` -> `QueueCastAt/On`), plays the cast clip it
receives (combat.md 8), draws the effect objects (7.1) and the buff visuals (6.3), and reads the mana from section 0 (`CUnit +0x182`).
Hit effects of damage spells are the particles the server creates (0x2041 / 0x2042 / 0x2002), not the melee blood prints.

## 9. What is not known

* The sequence number of a freshly constructed spell (`FillFromId` passes the container's own id; who assigns `id & 0xFFFF`) [?].
* `SpellProto +0x48 / +0x54` (Build-in / Special Mods as dword lists) and `+0x60` TTI: read only by the item constructor / UI, not followed.
* The exact `CEffectSoundSeq::Activate` modes (count 1..5 select different `PlayOnSlot` variants) and the ambient sound parameters.
* `CEffectArrow` is listed with an 18-slot vtable in classes.tsv while its Activate / ReadDelta exist (0x67B4E0 / 0x67B690): the table is
  probably truncated (the projectile base has 25) [?].
* Shapechange (0x16) and comprehend (0x23) have no server script: whether the UI / party code reads them was not checked.
* The buff `+0x10` flag byte (replicated in section 3) is never read in the code seen.
* `AddBuff`'s second argument (`applyScale`) is read from the stack by the decompiler as a register leftover; the call sites pass
  `CSpellEffect +0xA4` (quiet) in that position [I].

## 10. Corrections to earlier notes

1. `docs/game-exe/combat.md` 7 and `CUnitServer::v49_UpdateSilenced`: buff kind 0x20 is **feeblemind** (proto 32), not silence; silence is
   buff 0x10 (proto 16) and reduces the noise radius. The flag 0x10000000 means "cannot cast". `CUnit::SpellUsability` state 4 likewise.
2. `combat.md` 4.3 / `structs.tsv` `Hit +0xC spellDir`: hit[3..5] of a spell are the **element weights** (thermal / chemical / electrical =
   1.0), not a direction; the direction code of a spell is hit[12] (1 for walls / fog / column / campfire). `Hit_HasDirection` really means
   "has an elemental factor" (the caller treats it as a spell hit). `combat.md` "damage spells ... death kind 7": not seen in the spell code.
3. `combat.md` 5 "buffs 15/16/17 subtract from the resistance pairs, 19 adds": the pairs `+0xE0..` are the detectability values
   (visibility / noise / smell), the resistances are the placement armor pairs (+0x3C / +0x44 / +0x4C get the protections).
4. `functions.tsv` `CSpellEffect::GetRangeFactor`: `+0x58` is the power (effect), the function is the power factor; `IsDamageSpell` is the
   hostile-spell list (includes weaken, slow, charm ...); `RefCount_DecAndTest` 0x688530 is the buff duration tick; `SpellCaster_*` are
   `CSpellEffect` members; `DropOtherWeapons` 0x5528F0 also rebuilds the passive spell list.
5. `docs/game-memory.md` "Spells": the constructor description is confirmed; add: the script / item syntax uses the **code** column
   (`fireball{e1;r1}`), the `d` and `t` runes truncate `v * protoValue`, the mana final value is `max(proto, computed)`, and `+0x4C`
   ("field 14") is the cast time in action frames. "A container carried in the items but not worn is not castable" is the inventory slot rule.
6. `structs.tsv` `CMapObject +0x198`: the record is `{kind byte = proto index, level byte, duration, power, radius}`; `CUnitServer +0x920`:
   nodes `{next, prev, item ref, countdown, keep}` of the `ic` items.
7. `hierarchy.md` "24-slot effects": `CEffectArrowFigure` / projectile base have 25 (slot 24 `UpdateAttached`); `CEffectMoshka` and
   `CEffectTornado` are not spell effects (hero fly, weather).

## 11. Function index

Database: SpellsDb::Load 0x684550, dtor 0x684390, PerkMap_FindOrInsert 0x684B20, SpellProto::SerializeFields 0x683B80, PerkProto::SerializeFields
0x683AF0, SpellSetRecord::SerializeFields 0x683D40, SpellProto_FindByCode 0x68C820, PerkProto_FindByCode 0x68C920, SpellProto_FindCodeByName 0x6893F0,
PerkProto_FindByName 0x6894B0, SpellsDb_ListSpellNames 0x689570, ListPerkNames 0x689610.
CSpell: ctor 0x68B1F0, Init 0x68B320, InitFromProto 0x68B430, FillInfo 0x68B370, FillFromId 0x68B410, IsValidTarget 0x68B4C0, GetElementIndex 0x68B700,
IsHostileSpell 0x68B770, HasItemPerk 0x68B7C0, Read 0x68B860, Write 0x68B920, GetProto 0x68BA20, GetPerkCount 0x68BA60.
Container / constructor / manager: CSpellContainer ctor 0x68BA80, Init 0x68BD80, AddRune 0x68BE30, RemoveRune 0x68BE80, IsPerkKindAllowed 0x68BEC0,
Finalize 0x68BF60; CSpellConstructor ctor 0x683FF0, SetManager 0x6841A0, BeginConstructSpell 0x6841E0, EndConstructSpell 0x684250, AddRune 0x6842A0;
CSpellManager ctor 0x688840, Done 0x688B40, CreateByKind 0x688CD0, ClearRegistry 0x688D70, CreateContainer 0x688EB0, CreateSpellById 0x688EC0,
CreateSpellByName 0x689030, CreateSpellWithPerks 0x689310, SpellRegistry_ctor 0x688660 / Destroy 0x6886F0.
Casting: QueueCastAt 0x554D00, QueueCastOn 0x554F50, CanCastSpell 0x54B950, CanAffordSpell 0x54BF80, CanCastAt 0x54C2F0, ComputeCastTarget 0x54BBC0,
PrepareCastParams 0x54BCB0, BuildSpellAnimQueue 0x550620, CastOrderSpell 0x54EE10, TickPassiveSpells 0x552B90, RebuildEquipmentLists 0x5528F0,
TriggerWeaponSpell 0x54F850, TriggerArmorSpells 0x54F8E0, OrderIsOffensiveSpell 0x54B580, MagicSkill 0x527EB0, ManaCostFactor 0x523CD0,
CWorldServer::CastSpellMulti 0x58E850, CastSpell 0x58E670, Update 0x58DCD0, RemoveObject 0x5935C0, CreateSpellItemByName 0x594BC0,
CItemManager::CreateSpellContainer 0x6AD1E0, CMagicTrapObject::CastAtTargets 0x52A4B0, Spell_GetAiCategory 0x5CEDC0, AiSpellInfos_Build 0x5CEED0.
CSpellEffect: ctor 0x67E510, dtor 0x67E5D0, Init 0x67ECB0, Update 0x67E6D0, Start 0x67F870, TickFlight 0x681BB0, TickActive 0x682650, BuildHitRecord 0x683950,
ApplyHitTo 0x6835B0, ApplyHitArea 0x683600, ApplyHitBeam 0x683730, PowerFactor 0x67EAA0, AreaFactor 0x67EB60, LightDriftVector 0x67EC40, UpdateVolumes 0x67EFC0,
Create* 0x67F170 / 0x67F260 / 0x67F410 / 0x67F500 / 0x67F580 / 0x67F660 / 0x67F7C0, RemoveEffectObjects 0x67ED60, OnObjectRemoved 0x67EF00,
NotifyCasterPlayer 0x67EF90, Read 0x67E7D0, Write 0x67E960, CUnit_IsDeadOrDying 0x681B80.
Buffs: Buff_Read 0x688550, Buff_Write 0x6885A0, Buff_Clear 0x688510, Buff_TickDuration 0x688530, CMapObject::AddBuff 0x50F1E0, ApplyBuffKinds 0x50F2D0,
ClearBuffs 0x50F5B0, HasBuffKind 0x50F630, GetBuffValue 0x50F690, CMapObjectServer::TickBuffs 0x515820, v49_UpdateFeeblemind 0x553220,
UnitStats_Recompute2 0x524560, ApplyRangeBuffs 0x523D30, Unit_BuffHPRatio 0x523DB0, Placement_Regenerate 0x523740, SpellUsability 0x52F820,
UpdateBuffEffects 0x510B80, UpdateBuffSounds 0x5112A0, BuffSound_Start 0x5114D0.
Effects: CEffectManager::CreateByKind 0x67E170, CEffect::ResetState 0x675BE0, SetVelocity 0x675D50, MarkDirty 0x675CA0, ReadDelta 0x675BA0, WriteNetStateB 0x675B70,
AddEffectObject 0x58E520, RemoveEffectObject 0x58E5D0, ReplicateToPlayer 0x58B750, SendMsg50_Objects 0x66DDE0, SendMsg51_Objects 0x66E020, OnMsg_WorldObject 0x668940;
CEffectSound 0x675DD0 (Reset 0x676200, Update 0x6762F0, Play 0x6763A0, ReadNet 0x676430), CEffectSoundSeq 0x676760 (Activate 0x676C00, Update 0x677250),
CEffectAmbientSound 0x677510 (Activate 0x677950), CEffectPointLight 0x677D30 (Reset 0x6780C0, Update 0x6781B0, Activate 0x6782D0, ReadDelta 0x678520),
CEffectParticle 0x6787B0 (Reset 0x678A90, GetPos 0x678BA0, Activate 0x678CF0, ReadDelta 0x678D30, CreateParticleSystem 0x678EC0, Update 0x6795C0,
SetTargetPos 0x679310, SetIntensityRadius 0x6792E0), CEffectWall 0x6797E0 (SetDirection 0x679C30, ReadDelta 0x679C70), CEffectLightning 0x679FC0
(Activate 0x67A370, ReadDelta 0x67A490, SetEndpoints 0x67A6B0, SetUnits 0x67A890, SetStyle 0x67A8C0, Update 0x67A8F0), CEffectArrow 0x67AE30
(ComputeFlightVector 0x67AAB0, Update 0x67AC90, SetTarget 0x67AC70, Reactivate 0x67B450, Activate 0x67B4E0, ReadDelta 0x67B690), CEffectArrowFigure 0x67BB90
(Reset 0x67BAA0, Update 0x67BE20, UpdateAttached 0x67BEF0, Activate 0x67BF20, ReadNet 0x67C080), CEffectShowVisibility 0x67C340 (SetUnits 0x67C640,
Activate 0x67C660, Update 0x67C840, ReadNet 0x67C6B0), CEffectMoshka 0x67C950 (UpdateFlight 0x67CEF0, CreateBird 0x67D420, SetFlight 0x67DA50),
CEffectTornado 0x67D670 (Update 0x67DBD0, Activate 0x67DF10), Effect_FindVisiblePointAround 0x67CD90.
