# Data files, .mob descriptors, particles and sound in game.exe (UnitsDb, QuestsDb, LeversDb, CObjectDesc.., CParticle, CSoundManager)

Written 2026-11-01 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x463000 Levers.ldb,
0x469000 .mob descriptors, 0x484000 particles, 0x53A000 units.udb, 0x6AE000 quests.qdb, 0x6B4000 sound / music; plus the record
serializers at 0x52D6F0-0x52DEE0 and 0x6AD7D0 / 0x6AD900 that the loaders call). Batch files: `_cpr/claude-re/full/batch-data.tsv`
(317 function rows) and `batch-data-structs.tsv` (418 field rows). Sibling pages: items.idb in [items.md](items.md), spells.sdb /
perks.pdb in [spells.md](spells.md), prints.db / acks.db in [terrain-lighting.md](terrain-lighting.md), the descriptor -> object step in
[world-server.md](world-server.md) 4.4, the quest log UI in [ui-screens.md](ui-screens.md) 9, the unit logic slots in [unit-ai.md](unit-ai.md) 3.6.

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess / not resolved. Addresses are VAs,
offsets are bytes, "tag" = the one-byte tag of the database archive, "chunk" = the 4-byte .mob node id.

> **Chunk ids verified 2026-11-01** by parsing Universal-Mod/maps/basecam-mp.mob: UNIT 0xBBBB0000 holds the item lists 0xBBBB0005..0009 and five UNIT_LOGIC 0xBBBC0000 records (fields 0xBBBC0002..000E, guard points 0xBBBD0000, action points 0xBBBE0000); MAGIC_TRAP 0xBBAB0000, LEVER 0xBBAB0000. The decimal constants of um-multitool (mob_file.hpp, mobdumper.cpp) and um.dll agree; only the hex column of mob-format.md was wrong (3149594624 is 0xBBBB0000).

## How to re-implement it (short version)

1. **Every database file is the same tagged tree** (`Archive_ReadTagged` 0x409070 / `Archive_WriteTagged` 0x409110): `[tag byte][len*2
   as 1 byte, or (len*2|1) as 4 bytes][len bytes]`, nested file -> block -> record -> field -> list item. Readers ask for fields by tag
   (`Archive_SerializeBytes` 0x4092B0, `Archive_SerializeCString` 0x4093A0): a missing field is zero / "" (**no field is mandatory**),
   unknown tags are skipped, order does not matter. Lists are a tagged section whose items are all tag 1 (`DbStream_SerializeCStringVector`
   0x52DF40). The 10-byte trailer is `Archive_Close` 0x40A000 writing an empty tag-0 section and a tag-2 section holding `{tag 2: int
   version = 1}` (`DbArchive +0x54`).
2. **units.udb** = four blocks: 1 HitLocations (`UdbHitLocation` 0x44), 2 RaceModels (`UdbRaceModel` 0x168), 3 Monsters (`UnitProto`
   0x120), 4 NPC (`UdbNpcRecord` 0x60); `UnitsDb::Load` 0x53A200 reads them into `g_0x7AF358` and then **resolves names to indices**:
   `UnitProto +8` = RaceModels index of "Base Race", `UdbRaceModel +0x8c + k*0x10` = HitLocations index of the six "HL .. Type" names.
   A unit takes its stats from the Monster and its race (combat.md 5), the client takes figure / skins / sounds from the race (section 3.6).
3. **quests.qdb** = block 1 quests (`Quest` 0x38: id, exp, money, zone, order, places) and block 2 briefings (`Briefing` 0x6c: exp, money
   and five name lists). `QuestsDb::Load` 0x6AE3A0 reads it once; every map's `quest.reg` is parsed on top by `QuestsDb::ParseQuestReg`
   0x6AE8E0 (same fields, ini syntax). Quest *state* is not here: it is the player's named values `q.<zone>.<quest>` (ui-screens.md 8 / 9).
4. **.mob descriptors** are ChunkFile readers (`OnChunk` slot 0, one switch per class, section 5): `CObjectDesc` 0xB0xx, `CUnitDesc`
   0xBBBB + logic 0xBBBC / BBBD / BBBE, `CTorchDesc` 0xBBBF, `CMagicTrapDesc` 0xBBAB, `CLeverDesc` 0xBBAC, `CLightDesc` 0xAAxx,
   `CSoundDesc` 0xCCxx, `CParticleDesc` 0xDDxx. Slot 7 `Spawn` calls `CWorldServer::CreateObjectFromDesc` 0x5927C0 by `kind +0x48`.
5. **A particle system is one `CParticle`** (0x1f0, section 6): `Init` 0x485540 is a constant table per type `0x2000 + k` (texture,
   spawn box, size, counts, life, three callbacks). `Update` 0x496EB0 spawns `max(round(speed / +0xd4), +0xd0)` particles per tick with
   probability `+0xcc` % while fewer than `+0xd8` live, runs the step callback on every live particle (0 = dead), keeps the bounds.
   `Render` 0x4976B0 interpolates each particle between the previous and the current tick by the frame fraction and draws one
   screen-aligned quad per particle (`I3DFigureImpl::DrawParticleQuads` 0x502CA0) with an atlas frame (`+0xdc` frames of `+0xe0` columns).
6. **Sound is Miles (AIL) with 15 channels** (`SoundChannel` 0x54 at `CSoundManager +0x24c`, section 7): `Play3D` 0x6B8600 /
   `Play2D` 0x6B8250 pick the lowest-priority channel, load the wav through an 8 MB LRU sample cache (`LoadSample` 0x6B89F0), attenuate
   (`UpdateChannel3D` 0x6B7FC0: linear between min / max range in the xy plane, times the camera-height curve 30..70, times the category
   volume), start the sample; looping sounds that lose their channel are kept as pending records and resumed by `Update` 0x6B9B10.
   Music is an mp3 stream fed by its own thread (`MusicThreadProc` 0x6BB910, 100 ms, three buffers) and directed by `CMusicSystem`
   (`music.reg`: calm / combat / briefing / constructor tracks per zone, `Update` 0x6B5350).

## 1. Conventions

| Thing | Value |
|---|---|
| databases | `CMappedResource` archives `<res dir>database.res` (`g_0x79BC94` dir) or `databaseLMP.res` (the units / perks / spells / items loaders take the name; `Databases_LoadAll` 0x57CB50 reloads those four when the session switches); quests / levers / acks / prints always `database.res` [V] |
| database globals | UnitsDb `g_0x7AF358` (0x80), QuestsDb `g_0x7C87A8` (0x40), LeversDb `g_0x79C1A0` (0x20), ItemsDb `g_0x7C86D8`, SpellsDb `g_0x7C8510`, PerksDb `g_0x7AF268`, CAcksDb `g_0x7C8A60`, CPrintsDb `g_0x7C89C8` (all loaded by `main` 0x456440) [V] |
| name maps | STLport hash_map<std::string, record*> (0x18 nodes {next, string, value}, 100/101 buckets); lookups copy the C string into a std::string first; a lookup inserts a null slot when missing [V] |
| record strings | MFC `CString` (4-byte pointer, length at -8) in the udb / ldb / idb records; `std::string` (12 bytes) in the quest records [V] |
| lists in records | STLport `vector<CString>` / `vector<float>` = 3 pointers (begin, end, cap) [V] |
| .mob node | `{u32 id, u32 length incl. header, payload}`; a descriptor's `OnChunk` sees `+4` = the child id; vec3 = 3 floats, quaternion = 4 floats, byte = 1, dword = 4, string = raw cp1251 (no terminator), string array = count + (id, len, bytes) items [V] |
| particle tick | the client world tick (`CWorldClient::Update` calls `CParticleManager::RemoveFinished` which runs every `Update`); render interpolation fraction from `g_0x7AF61C` / `g_0x7AFCA8` [V] |
| particle colour | packed ARGB in the record `+0x58`; `-1` = white |
| sound volume | percent 0..100 -> AIL 0..127 = `round(p * 1.27)`; pan -100..100 -> `(pan + 100) * 127 / 200` [V] |
| sound id | `CSoundManager +0x268` counter, one per started sample or pending record; `-1` = failed; stream slots return `generation << 8 \| slot` [V] |
| random | ISAAC words `g_0x7A9D48[g_0x7A9D44--]` (`Isaac_RandRange` 0x488970 for floats; `rand % n` for choices); `GetStepSoundName` uses CRT `rand()` [V] |

## 2. The tagged database archive (`DbArchive` 0x98, `_cpr` name; "Archive_*" functions at 0x409000)

`DbArchive::ctor` 0x53A7B0 (was Archive_ctor) builds two CMemStream-like objects (+0 read, +0x28 write), the open-section stack `+0x50`
(CList of 0x24 nodes `{+8 tag, +0xc offset, +0x10 size, +0x14 occurrence}`), `+0x54` version, `+0x58` loading flag. `Archive_Open`
0x409AE0 (archive, CMemStream, loading = 1, version = 1) binds the stream; the loaders then call one `XVector_Serialize(archive, blockTag,
vector)` per block and `Archive_Close` 0x40A000 [V].

| primitive | behaviour [V] |
|---|---|
| `Archive_ReadTagged` 0x409070 | reads `tag` (1 byte) and the length byte `L`: odd `L` -> the length is the 4-byte LE value `>> 1`, even -> `L >> 1`; returns the payload pointer / size, false at the end of the enclosing section |
| `Archive_WriteTagged` 0x409110 | `len*2 < 0x100` -> 1 length byte, else 4 bytes `len*2 \| 1`; **every** entry (file, block, record, field, list item, leaf) is written the same way |
| `Archive_BeginTag` 0x4098D0 | pushes a section node; loading: scans the current section for the n-th occurrence of the tag (false when absent, so the section body is skipped) |
| `Archive_CountTag` 0x409A60 | number of children with that tag in the current section (list sizes) |
| `Archive_SerializeBytes` 0x4092B0 | loading: copies `size` bytes of the n-th child with that tag, **zero-fills when absent**; storing: writes them |
| `Archive_SerializeCString` 0x4093A0 | the same for a CString (`len + 1` bytes written, i.e. with the terminator; "" when absent) |
| `DbStream_SerializeCStringVector` 0x52DF40 | section `tag` with every string as a tag-1 child (`Archive_CountTag(1)` items on load) |
| `Archive_Close` 0x40A000 | storing: flushes the deferred object list `+0x70`, then `SerializeBytes(2, &version, 4)`, then writes the whole tree as a tag-0 entry followed by the tag-2 entry -> the shipped `00 00 02 0C 02 08 01 00 00 00` trailer |

Consequences for an editor: field order inside a record is free (the game searches by tag), fields may be omitted (zero), a wide
length without the `|1` flag is **misread** by the game (it would take one length byte), and the trailer is `[0][2:{2:int 1}]`.

## 3. units.udb (`UnitsDb` 0x80 at `g_0x7AF358`, `UnitsDb::Load` 0x53A200) [V]

| block tag | sheet (dbblocks) | record | stride | vector / map | serializer |
|---|---|---|---|---|---|
| 1 | HitLocations | `UdbHitLocation` | 0x44 | `+0x60` / `+0x6c` (`g_0x7AF3B8`) | `UdbHitLocation::SerializeFields` 0x52DEE0 |
| 2 | RaceModels | `UdbRaceModel` | 0x168 | `+0x40` / `+0x4c` (`g_0x7AF398`) | `UdbRaceModel::SerializeFields` 0x52DC70 |
| 3 | Monsters | `UnitProto` | 0x120 | `+0` / `+0xc` (`g_0x7AF358`) | `UnitProto::SerializeFields` 0x52D740 |
| 4 | NPC | `UdbNpcRecord` | 0x60 | `+0x20` / `+0x2c` (`g_0x7AF384` map) | `UdbNpcRecord::SerializeFields` 0x52DA60 |

The blocks are read in that order, each as its own `Archive_Open / Close` pass over the same memory stream (so block order in the file
does not matter). After the four passes: `UnitProto +8 = index of FindRaceModelByName(+4)` (-1 unknown) and for every race model and
`k < 6`: `+0x8c + k*0x10 = index of FindHitLocationByName(+0x88 + k*0x10)` (-1 unknown). The sheet values of those two fields are
therefore ignored. `UnitsDb::CountSoundVariants` 0x53A8E0 (after the load and after `CWorldClient::InitClientResources`) lists `*.wav` of
every sound archive and stores per race model `+0x144[i]` = the number of wav files whose upper-cased name starts with the step stem `+0x138[i]`.

The field ids below are the sheet's `FLDx-y` (= the tag byte x, list index y); the sheet titles are those of `dbheaders.txt`
(um-multitool `db_headers_generated.hpp`), which match the game's use.

### 3.1 Monsters (`UnitProto` 0x120) [V]

| tag | sheet | off | type | runtime use |
|---|---|---|---|---|
| 0 | Name | +0 | CString | the prototype name (`.mob` UNIT_PROTONAME, `Units_FindProtoByName` 0x5144D0) |
| 1 | Base Race | +4 | CString | -> `+8` race index (load) |
| 2 | Unk2-0 | +8 | int | **overwritten**: RaceModels index (`CUnit::GetProto2`, `g_0x7AF398[+8]`) |
| 3 | Skin Index | +0xc | int | `<mask>skin_NN` texture |
| 4 | Hair | +0x10 | int | `hr.NN`, -1 none |
| 5..7 | Complection X Y Z | +0x14 | float[3] | morph parameters (`CObject3D +0xc4`; heroes `HeroRecord +0x724`) |
| 8 | Unk8-0 | +0x34 | CString | unused |
| 9 / 10 | Health / Mana | +0x48 / +0x4c | float | max HP / MP; monsters also `str = HP / 3`, `dex = MP / 3` (combat.md 5) |
| 11 | Absorbtion | +0x58 | float | armor factor |
| 12 | Tuning Actions | +0x5c | float | action speed tuning |
| 13 | Tuning Move | +0x60 | float | base speed |
| 14 / 15 / 36 / 16 / 17 | Attack Range / To Hit / To hit Random / Parry / Weapon Weight | +0x64 / +0x68 / +0x6c / +0x70 / +0x74 | float[5] | natural attack -> `UnitStats +0x70..+0x80` |
| 18 / 39 | Weapon Type Id / Real Weapon Type Id | +0x78 / +0x7c | int | attack kind (0 sword .. 6 crossbow, 8 natural) |
| 19 / 20 | Damage Min / Max | +0x80 / +0x84 | float[2] | natural damage min, spread |
| 21 / 22 / 23 / 24 | Skills General / Steal / Tame / Peripheral | +0x88 / +0x8c / +0x90 / +0x94 | float | skill bases |
| 25 | Senses Sight Infra SenseLife Hearing Smell Tracking | +0x98 | float[6] | perception bases -> `UnitStats +0xb0` pairs |
| 26 | Detections (same six) | +0xb0 | float[6] | detectability bases -> `UnitStats +0xe0` pairs |
| 27 / 28 | Common / Rare loot Probability Mask Min Max | +0xc8 / +0xd8 | float[4] | TypeList(21) sub-tags 0..3 (loot generation [?]: no reader in this round) |
| 29 | Items | +0xe8 | vector<CString> | "proto.material[count]" (`Unit_GiveProtoEquipment`) |
| 30 | Skills Elemental Senses Astral | +0xf4 | float[3] | magic skill bases |
| 31 | Spells | +0x100 | vector<CString> | "code{mods}" (`UnitProto::ApplyTo` 0x477B00 adds them) |
| 32 | Equipment Wears | +0x10c | vector<CString> | armor names |
| 33 / 42 | Weapon 1 / 2 | +0x118 / +0x11c | CString | weapons; `+0x118` also selects the animation set (`CUnitClientSpecific::PlayAnimation` 0x537500) |
| 34 | Info Window Scale | +0x20 | float | unit info window |
| 35 | Altitude | +0x24 | float | flying height (>= 1.5: not meleeable) |
| 37 / 38 | Dialog Camera Distance / Height | +0x28 / +0x2c | float | dialog camera |
| 40 | Detonation | +0x30 | int | |
| 41 | Base Level | +0x50 | int | `CUnit::GetLevel` 0x532D40 for monsters |
| 43 | Exp | +0x54 | float | experience given to the killer |

Gap `+0x38..+0x44` is never written or read [V]. The record is exactly 0x120 bytes (`vector<UnitProto 0x120>`): the earlier
`speedMult +0x120` row belongs to the next record (correction below).

### 3.2 RaceModels (`UdbRaceModel` 0x168; was "UnitDbEntry") [V]

| tag | sheet | off | type | runtime use |
|---|---|---|---|---|
| 0 | Name | +0 | CString | "Human Male".. (`FindRaceModelByName` 0x53D240) |
| 1 | TypeId | +4 | int | `Units_GetProtoTypeByName` 0x513240 |
| 2 / 3 | Regenerate Health / Mana | +8 / +0xc | float | placement regen |
| 4 / 5 / 6 | Secondary Language / Locomotion / Vision arc | +0x10 / +0x14 / +0x18 | int / int / float | move type (1 = legs), `cos(arc/2)` -> `UnitStats +0x44` |
| 7 | Speed Run Walk Sneak Crawl | +0x1c | float[4] | stance multipliers -> `UnitStats +0x34` |
| 8 / 9 / 10 | Attack Distance / AI Class Stay / Lie | +0x44 / +0x48 / +0x4c | float / int / int | |
| 11 | Attack Piercing .. General | +0x50 | float[7] | natural damage factors -> `UnitStats +0x90` |
| 12..17 | HL Head / Torso / Right Arm / Left Arm / Right Leg / Left Leg | +0x88 + k*0x10 | `UdbHitLocationSlot` | TypeList(20): tag 0 Type (name), 1 index (overwritten), 2 Weight, 3 Vitality |
| 30 | Mask Name | +0xe8 | CString | the figure stem ("unhuma"); copied as the hero class name (`CParty::InitHeroFromProto` 0x6621F0) |
| 31 / 32 | Primary / Secondary textures | +0xec / +0xf8 | vector<CString> | skin texture names (`CWorldClient::PrebuildUnitSkinsAndModels` 0x568800) |
| 33 | Model Shift | +0x104 | float | `Unit_ApplyPrototype` 0x513B60: model offset `(0, -shift, 0)` |
| 34 | SFX Path | +0x124 | CString | sound folder |
| 35 | Steps | +0x138 | vector<CString> | step sound stems; counts in `+0x144` (`GetStepSoundName` 0x534590: `"<stem>%d.wav"`, `"steps\human\crawl%d.wav"` when crawling, kind 2 when the unit is on a bridge above the terrain) |
| 36 | Animation Speed Attack Cast Hit Death Cross Special | +0x2c | float[6] | `CUnit +0x4b0` (CreateObjectFromDesc) |
| 37 / 38 | Sounds Probability Idle / Attack | +0x150 / +0x154 | int | voice chance |
| 39 | Defence Piercing .. General | +0x6c | float[7] | |
| 40 / 41 / 42 / 43 | Blood Type / Cast Type / Footprint Type / Leg Segments | +0x108 / +0x114 / +0x10c / +0x110 | int | prints.db kinds |
| 44 / 45 / 46 | Skin Type / First Step Right / Head Height | +0x118 / +0x11c / +0x120 | CString / byte / float | |
| 47..50 | Unk47..50 | +0x158.. | int[4] | unused |

`+0x128..+0x134` is not serialised (copied by `operator=` only) [V]; `+0x144` is the runtime step-sound count vector.

### 3.3 HitLocations (`UdbHitLocation` 0x44) and the slot record [V]

| tag | sheet | off | type |
|---|---|---|---|
| 0 | Name | +0 | CString |
| 1 | Physical Piercing / Shashing / Bludgeoning, Elemental Termal / Chemic / Electric, Other General | +8 | float[7] (armor tier 1) |
| 2 | the same "2" columns | +0x24 | float[7] (armor tier 2) |
| 3 / 4 | Unk3-0 / Unk4-0 | +0x40 / +4 | int |

`Unit_InitStatsFromProto` 0x5136D0 / `Hero_InitStatsFromProto` 0x513DA0 index `g_0x7AF3B8` by the slot's resolved index to build the
body part armor tiers (combat.md 5: `+0x110` parts, 3 tiers of 7 values); the slot's Weight / Vitality give the part's hit weight and HP
fraction [I].

### 3.4 NPC (`UdbNpcRecord` 0x60; was "HeroRecord", which collides with the party record) [V]

| tag | sheet | off | type | use (`CParty::ApplyHeroKit` 0x6625B0, `Hero_InitStatsFromProto`) |
|---|---|---|---|---|
| 0 | Name | +0 | CString | the hero prototype name |
| 1 | Unk1-0 | +4 | int | |
| 2 | Exp | +8 | float | |
| 3 / 4 / 5 | Strength / Dexterity / Intelligence | +0xc / +0x10 / +0x14 | float | `UnitStats +0xc..+0x20` base = current |
| 6 | Science Stealing Tame | +0x18 | byte[3] | `UnitStats +0x6c8` |
| 7 | Melee Archery Backstab Elemental Senses Astral Stealth Awareness | +0x1b | byte[8] | `UnitStats +0x6cb` |
| 8 | Perks | +0x24 | vector<CString> | perk codes |
| 9 / 10 / 11 | Weapons / Quest Items / Spells | +0x30 / +0x3c / +0x48 | vector<CString> | the kit: weapons -> `HeroRecord +0x730`, items -> belt `+0x740`, spells -> `+0x760` |
| 12 / 13 | Exp To Distribute / Money | +0x54 / +0x58 | float / int | added to the party |
| 14 | Using Voice | +0x5c | byte | |

### 3.5 Who reads the units database [V]

`Unit_InitStatsFromProto` 0x5136D0 (stats, combat.md 5), `Unit_ApplyPrototype` 0x513B60 (model, offset, morph, equipment),
`UnitProto::ApplyTo` 0x477B00 (the `.mob` UNIT_STATS block `CUnitDesc +0x210`, 0xac bytes = a packed stats record, applied over the
prototype), `CUnit::GetProto` 0x530930 / `GetProto2` 0x5309E0 (43 callers), `CUnit::SetProtoIndex` 0x5308A0 (`+0x23c = (proto -
base) / 0x120`), `CUnitClientSpecific::PlayAnimation` / `GetStepSoundName` / `BuildSkinTextures` 0x536670 / `GetAnimSoundName`
0x534760 (client), `CWorldClient::CreateHeroUnits` 0x566F70, `CPlayerClient::OnMsg40_UnitCreate` 0x667EC0, `CParty::InitHeroFromProto`,
`UIUnitPreview::PickFaceFigure` 0x5ED7B0 ('unhuma' / 'unhufe' -> face figures), `CUnit::PlayAck` 0x6D8430 (was CAcksDb::PlayUnitHitSound; the unit event code -> acks.db sound, combat.md 9).

### 3.6 What um-multitool assumes (`viewer/unit_db.hpp`) - verified

Block 2 = races (fields 0 name, 30 mask, 31 / 32 textures) and block 3 = monsters (0 name, 1 race, 3 skin, 4 hair, 32 wears, 33 / 42
weapons): all correct. Not modelled there: the race index at Monsters field 2 and the HL index at RaceModels fields 12..17 sub-field 1 are
**recomputed by the game** (any value compiles fine), and the complection (5..7) / model shift (RaceModels 33) that the viewer could use.

## 4. quests.qdb and quest.reg (`QuestsDb` 0x40 at `g_0x7C87A8`) [V]

`QuestsDb::Load` 0x6AE3A0 (main, once): `database.res` -> "quests.qdb" -> tag 1 `QuestVector_Serialize` 0x6B1F20 -> `+0`
(`vector<Quest 0x38>`, map `+0xc` = `g_0x7C87B4`), tag 2 `BriefingVector_Serialize` 0x6B2010 -> `+0x20` (`vector<Briefing 0x6c>`, map
`+0x2c` = `g_0x7C87D4`). quests.qdb is **not** one of DBEditor's seven databases (`dbfiles.txt`); the same records are also produced
from text by `QuestsDb::ParseQuestReg` 0x6AE8E0 (was QuestsDb_ParseIni; `this` = the global, lost by Ghidra), which `QuestFiles_Scan`
0x5810A0 runs on `<map>\quest.reg` (through `CTextManager::OpenText`) for every map of the lobby map list [V].

### 4.1 Quest (0x38) [V]

| tag | quest.reg key ([quest] section) | off | type |
|---|---|---|---|
| 0 | name | +0 | std::string: the quest id (`q.<zone>.<id>` state key, "quest <id>" text section) |
| 1 | exp | +0xc | int |
| 2 | money | +0x10 | int |
| 3 | order | +0x18 | int |
| 4 | - | +0x2c | std::string (not read from quest.reg) |
| 5 | zone index | +0x14 | int |
| 6 | - | +0x1c | int |
| 7 | questplaces "{x;y}{x;y}" | +0x20 | vector<float> (tag-1 items; pairs) |

### 4.2 Briefing (0x6c) [V]

| tag | quest.reg key ([briefing receive] / [briefing reject] / [briefing complete]) | off | type | `CWorldServer::OnNamedEvent` 0x592090 / `DialogScreen` |
|---|---|---|---|---|
| 0 | name | +0 | std::string | the briefing id (`StringHash_GetOrInsert g_0x7C87D4`) |
| 1 / 2 | exp / money | +0xc / +0x10 | int | party exp / money += ("brief_get_exp" / "brief_get_money") |
| 3 | give items | +0x14 | vector<CString> | `CItemManager::CreateItemByName` -> `CPlayer::AddItems` ("brief_get_item") |
| 4 | - | +0x5c | std::string | |
| 5 | remove items | +0x20 | vector<CString> | items released from the party ("brief_lost_item"; quest items 0x14 by script id) |
| 6 | give quests | +0x2c | vector<CString> | quest ids set (`q.*` = 1, "brief_get_quest") |
| 7 | complete quests | +0x38 | vector<CString> | quest ids completed (= 2, "brief_complete_quest") |
| 8 | available zones | +0x44 | vector<CString> | zones made reachable (`z.<zone>` = 2) |
| 9 | complete zones | +0x50 | vector<CString> | zones completed |
| 10 | - | +0x68 | int | ("brief_get_specials_%d" [?]) |

ParseQuestReg appends a quest / briefing only when its name is new (`BriefingTable_AddUnique` 0x6B0600; quests: map lookup first), so
quests.qdb entries win over quest.reg duplicates [V]. Readers: the quest log rewards (`CQuestLogWin::FillObjectives` 0x64DC60:
`g_0x7C87B4[id]` exp / money), the briefing rewards in dialogs (ui-screens.md 5: `g_0x7C87D4[topic]`), `LobbyServer::OnMapRecord`
0x418ED0 (map records). The quest texts come from `CTextManager::OpenText("quest <id>")`, the markers from `map.txt` (ui-screens.md 8 / 9).

## 5. database.res: every table and its loader [V]

| file | loader | global | records (stride) | page |
|---|---|---|---|---|
| items.idb | `ItemsDb::Load` 0x6A67A0 | `g_0x7C86D8` (0xc0) | tag 1 Materials 0x50, 2 Weapons 0xb4, 3 Armors 200, 4 QuickItems 0x8c, 5 QuestItems 0x7c, 6 LootItems 0x78 | items.md |
| spells.sdb | `SpellsDb::Load` 0x684550 | `g_0x7C8510` (0x64) | 1 SpellPrototypes 0x84, 2 SpellModifiers 0x24, 3..5 shop sets 0x34 | spells.md |
| perks.pdb | `PerksDb_Load` 0x520220 | `g_0x7AF268` | 1 skills 0x18, 2 perks 0x44 | combat.md 5 |
| units.udb | `UnitsDb::Load` 0x53A200 | `g_0x7AF358` (0x80) | 1 HitLocations 0x44, 2 RaceModels 0x168, 3 Monsters 0x120, 4 NPC 0x60 | section 3 |
| Levers.ldb | `LeversDb::Load` 0x463390 | `g_0x79C1A0` (0x20) | 1 LeverPrototypes 0x2c | below |
| prints.db | `CPrintsDb::Load` 0x6D0630 | `g_0x7C89C8` | 1 BloodPrints, 2 FootPrints, 3 FirePrints | terrain-lighting.md |
| acks.db | `CAcksDb::Load` 0x6D7620 | `g_0x7C8A60` | 1 Answers 0x1c4, 2 Cryes 0x5c, 3 Others 0x38 | terrain-lighting.md |
| quests.qdb | `QuestsDb::Load` 0x6AE3A0 | `g_0x7C87A8` (0x40) | 1 quests 0x38, 2 briefings 0x6c | section 4 |

The "0x6CC000 loaders" of findings.md are the prints / acks ones; the 0x463000 module only holds Levers.ldb. **LeverPrototypes**
(`LeverRecord` 0x2c, `LeverRecord::SerializeFields` 0x463030): tag 0 Name (+0; the lever **model name**: `CLeverObject +0xa4`), 1
Place1-1 Place1-2 Place2-1 Place2-2 (+4 float[4]), 2 Unk2-0 (+0x14 int), 3 Scale (+0x18 float), 4 Switch Time (+0x1c int), 5 Material
(+0x20), 6 Switch Sound (+0x24), 7 Lever Text (+0x28). The only reader found is `CLeverObject::SwitchState` 0x52B7A0: a negative
duration argument becomes the record's Switch Time (10 when the model has no record) [V]; the sound / text / places have no reader in
the code read this round [?].

## 6. The .mob descriptors (module 0x469000) [V]

Class tree (vtables): `CDescBase` 0x73C5C4 (0x4c: ChunkFile base, `+0x18` id, `+0x1c` object, `+0x20` pos, `+0x2c` quaternion, `+0x3c`
name, `+0x40` comment, `+0x48` kind) -> `CObjectDesc` 0x73C5F0 (0x8c, kind 1) -> `CTorchDesc` 0x73C620 (0xa0, kind 6),
`CMagicTrapDesc` 0x73C650 (0xd0, kind 7), `CLeverDesc` 0x73C6B0 (0xa4, kind 8), `CUnitDesc` 0x73C6E0 (0x324, kind 5); `CDescBase` ->
`CEffectDesc` 0x73C728 (`+0x4c` effect, `+0x50` enabled) -> `CParticleDesc` 0x73C758 (kind 4), `CLightDesc` 0x73C788 (kind 2),
`CSoundDesc` 0x73C7B8 (kind 3). Slots (re-checked by the generic sweep 2026-11-01 against vt 0x73C5C4..0x73C7B8): 0 OnChunk (one `switch` on the child id), 2 GetObject, 3 empty one-arg slot, 4 GetTypeName / GetDisplayName, 5 MoveBy,
7 Spawn, 6 Remove, 8 MarkDirty, 9 Write / WriteChunk, 10 deleting dtor, 11 WriteFields. `MobFile_ParseChunks` 0x481F00 creates them per OBJECTSECTION child and
`CreateObjectFromDesc` 0x5927C0 turns them into objects (world-server.md 4.4, not repeated). The readers are tolerant: unknown children
are ignored, missing ones keep the ctor defaults listed below.

### 6.1 OBJECT 0xB001 (`CObjectDesc::OnChunk` 0x46AB20 / `WriteFields` 0x46A940) [V]

| chunk | field | type | default | runtime |
|---|---|---|---|---|
| 0xB002 | `+0x18` id (NID) | dword | -1 | -1 = allocate |
| 0xB003 | `+0x50` type | dword | 0 | the manager creation key (0x3010 units, 0x35 trees ..) |
| 0xB004 | `+0x3c` name | string | "none" | `CMapObject +0x1a8` |
| 0xB006 | `+0x54` model | string | "none" | figure |
| 0xB007 / 0xB008 | `+0x70` / `+0x74` | string | "none" | specific +0x78 / +0x7c |
| 0xB009 | `+0x20` position | vec3 | 0 | |
| 0xB00A | `+0x2c` rotation | quaternion | identity | |
| 0xB00C | `+0x64`, `+0x60`, `+0x68` complection | 3 floats **(y, x, z order)** | 0.5 | `SetMorphParams` |
| 0xB00D | `+0x78` body parts | string array | empty | specific +0x80 |
| 0xB00E | `+0x58` prototype | string | "none" | non-unit prototype (units use UNIT_PROTONAME) |
| 0xB00F | `+0x40` comment | string | "none" | |
| 0xB011 | `+0x6c` | byte | 0 | object flag 0x20000 |
| 0xB012 | `+0x5c` | dword | -1 | |
| 0xB013 | `+0x6d` dynamic | byte (== 1) | 1 | 0 -> object flag 8 (static, never ticked) |
| 0xB014 | `+0x6e` side | byte (== 1) | 0 | player side |
| 0xB016 | `+0x4c` object name | string | "" | |

### 6.2 UNIT 0xBBBB0000 (`CUnitDesc::OnChunk` 0x46D080 / `Write` 0x46CF10) [V]

| chunk | field | notes |
|---|---|---|
| 0xBBBB0002 UNIT_PROTONAME | `+0x20c` CString | the Monsters name |
| 0xBBBB0004 UNIT_STATS | `+0x210` 0xac bytes | packed stats (`UnitProto::ApplyTo`; the mob-format.md "43 uint32" block) |
| 0xBBBB0005 UNIT_QUEST_ITEMS | `+0x2bc` CStringArray | |
| 0xBBBB0006 UNIT_QUICK_ITEMS | `+0x2d0` | |
| 0xBBBB0007 UNIT_SPELLS | `+0x2e4` | |
| 0xBBBB0008 UNIT_WEAPONS | `+0x2f8` | |
| 0xBBBB0009 UNIT_ARMORS | `+0x30c` | |
| 0xBBBB000A UNIT_NEED_IMPORT | `+0x320` byte -> int | |
| 0xBBBC0000 UNIT_LOGIC | `+0x90 + n*0x4c`, n = `+0x8c`++ | up to 5 `MobLogicSlot`s, read by `MobLogicSlot_OnChunk` 0x46CBC0 with `+0x28 = 0`, `+0x48 = 4` (-> 0 when no 000E chunk) |
| anything else | `CObjectDesc::OnChunk` | |

UNIT_LOGIC children (unit-ai.md 3.6 has the meanings): 0xBBBC0001 byte (legacy, clamps AggrMode to 0), 0002 byte `+0x24` cyclic, 0003
dword `+0` model, 0004 float `+0x2c` guard radius, 0005 vec3 `+0x18` guard point, 0006 byte `+0x34` NumAlarm, 0007 byte `+0x38` Use,
0008 / 0009 bytes (legacy revenge / fear), 000A float `+0x30` wait, 000B byte `+0x40` AlarmCondition, 000C float `+0x3c` help radius,
000D byte -> bit 3 of `+0x28` AlwaysActive, 000E byte `+0x48` AgressionMode; **0xBBBD0000 GUARD_PT** appends a `MobLogicPoint` (0x20:
0xBBBD0001 position + a CArray of `MobLookAtRecord` 0x14, one per **0xBBBE0000 ACTION_PT**: 0xBBBE0001 look-at vec3, 0002 wait
segments (dword -> short `+0x10`), 0003 turn speed float `+0xc`, 0004 flags byte `+0x12`) (`MobLogicPoint_OnChunk` 0x46C7F0,
`MobLookAtRecord_OnChunk` 0x46C610).

### 6.3 The other object kinds [V]

| class / chunk | children -> field (default) |
|---|---|
| TORCH 0xBBBF0000 (`CTorchDesc::OnChunk` 0x46B3C0) | 0001 strength float `+0x8c`, 0002 point link vec3 `+0x90`, 0003 sound string `+0x9c` ("nature\\..") |
| MAGIC_TRAP 0xBBAB0000 (`CMagicTrapDesc::OnChunk` 0x46B7D0) | 0001 diplomacy dword `+0x8c` (-1), 0002 spell string `+0x90`, 0003 areas: count + N x 12 bytes -> CArray `+0xa0`, 0004 targets: count + N x 8 bytes -> CArray `+0xb4`, 0005 cast interval dword `+0xc8` (15), 0xBBAC0005 cast once byte `+0xcc` |
| LEVER 0xBBAC0000 (`CLeverDesc::OnChunk` 0x46BF20) | 0002 cur state byte `+0x98`, 0003 total states byte `+0x9c` (2), 0004 cycled byte `+0xa2`, 0006 science stats 12 bytes `+0x8c`, 0007 is door byte `+0xa0`, 0008 recalc graph byte `+0xa1` (0001 / 0005 are not read by the lever) |
| LIGHT 0xAA01 (`CLightDesc::OnChunk` 0x46DBF0) | AA02 range float `+0x64` (1), AA03 name, AA04 position, AA05 id, AA06 shadow byte `+0x60` (1), AA07 colour vec3 `+0x54` (1,1,1), AA08 comment -> `CEffectPointLight` colour `+0x14c`, radius `+0x158` |
| SOUND 0xCC01 (`CSoundDesc::OnChunk` 0x46E190) | CC02 id, CC03 position, CC04 range dword `+0x68`, CC05 name, CC06 min `+0x74`, CC07 max `+0x78`, CC08 comment, CC0A wave list -> CStringArray `+0x54` (cleared first), CC0B range2 `+0x6c`, CC0D ambient byte `+0x84`, CC0E music byte `+0x85` -> `CEffectSound` (0x48) or `CEffectSoundSeq` (0x49) when ambient; spawned only when the list is non-empty |
| PARTICL 0xDD01 (`CParticleDesc::OnChunk` 0x46D730) | DD02 id, DD03 position, DD04 comment, DD05 name, DD06 type dword `+0x54` (0x2000 + k), DD07 scale float `+0x58` (1) -> `CEffectParticle +0x158 / +0x15c` |

`CEffectDesc::MoveBy` 0x46D420 clamps z up to the terrain height; the Spawn slots of the three effect descriptors only create the effect
once (`+0x1c == 0`) and afterwards just push the position / parameters into the existing `CEffect` and `CEffect::MarkDirty` (editor /
script moves). Spawn names "light / efcu0", "sound / efcu2", "particles / efcu3" are built and dropped (debug leftovers) [V].

## 7. The particle system (`CParticle` 0x1f0, `CParticleManager` at `g_0x7AF754`, module 0x484000) [V]

### 7.1 Objects and the per-frame loop

`CParticleManager::CreateParticleSystem` 0x498400 (type) = the manager factory `CreateByKind` 0x4981B0 (new `CParticle`, `Reset`,
`+0x10 = type`) + `Init` + refcount + the id set `+0xcc`. Clients: `CEffectParticle::CreateParticleSystem` 0x678EC0 (spells.md: it sets
the position, `ScaleParams(scale / +0x128)`, `+0x118` intensity, `+0x11c` radius, attaches to the unit and pre-runs up to 30 `Update`s to
catch up with the server tick), the buff auras (`UpdateBuffEffects`), tornado, weather. Per frame: `CWorldClient::Update` ->
`RemoveFinished` 0x498480 (every system's `Update`; dead ones released), `CWorldClient::Render` -> `SortByDepth` 0x498940 (depth = dot
with the camera up axis `g_0x79B2DC`, introsort, back to front, blend mode switched between additive `(5, 2)` and normal `(5, 6)` groups,
systems on invisible units skipped) -> `Render`. `RemoveUnreferenced` 0x498630 on zone load drops systems only the manager holds.
`EnableWeather` 0x498B20 / `DisableWeather` 0x498BB0 mute / unmute the rain, snow and fog types (0x2001 / 0x2004 / 0x2003) (indoor maps).
Particle records (0x64) come from the manager pool `+0xb4` (`AllocParticle` 0x498870 / `ReleaseParticle` 0x4987C0; `new(100)` when empty).

### 7.2 `CParticle` fields that matter (full list in the structs batch)

| off | field | meaning |
|---|---|---|
| +8 | flags | 1 emitting, 2 emit only while attached, 4 keep alive when detached (`Mute` 0x485440 = `(f & ~1) \| 4`, `Unmute` 0x485450 = `f \| 5`, `Stop` 0x485360 = `f & ~5` + detach), 8 removable, 0x10 use the carrier attach offset, 0x40 hidden |
| +0x1c / +0x94 / +0x13c | position / carrier / attach mode | position is relative to the carrier's attach point: `GetCarrierOffset` 0x484C00: mode 0 = `CUnit::GetAttachPoint2`, 1.. = the part "body0N" (`FindPartByNameStart`), non-units `Carrier_GetAttachPoint` 0x484B40 |
| +0x98 / +0xa4 / +0xc8 / +0xb0 / +0xbc | world pos / delta / speed / side / up | recomputed every Update from the carrier; the emitters use them for trails |
| +0xcc / +0xd0 / +0xd4 / +0xd8 | spawn probability % / min tries / distance per extra try / max live | `tries = max(round(speed / +0xd4), +0xd0)`; each try spawns with `rand % 100 < +0xcc`; one forced when `+0xcc > 50` and none spawned; nothing while `+0x1c0 >= +0xd8` |
| +0xdc / +0xe0 / +0x1d0 | frames / columns / uv rects | atlas of `+0xdc` frames, `+0xe0` per row; rect `i` = `{(i % c) / c, 1 - (i / c) / c, ..}` inset 1/512 |
| +0xe4 / +0xec | interval / life | per-type counters copied into the records (`+0x4c`, `+0x50`) |
| +0xf0 / +0xfc / +0x10c / +0x110 / +0x114 | spawn box min / max / size / growth / damping | the per-type constants (table below); `ScaleParams` 0x485480 multiplies the box, size, `+0x110`, `+0x128`, `+0xd4` |
| +0x118 / +0x11c / +0x130 | intensity / radius / direction | from the effect (power factor, area radius, wall direction) |
| +0x150 | additive | 1 = additive blend |
| +0x154.. | records | aligned array of 0x38 emitter records (streams, wind bands, link / beam ends, explosion seeds), ticked by `+0x1e8` |
| +0x164 / +0x170 / +0x180.. | bounds | min / max / centre / half extents of the live particles, frustum-culled by Render |
| +0x1b8.. | live list | intrusive list of `{next, prev, ParticleRecord*}` nodes, count `+0x1c0` |
| +0x1e0 / +0x1e4 / +0x1e8 | emit / step / tick callbacks | `emit(record, arg) -> bool`, `step(record) -> alive`, `tick(emitter record)` |
| +0x1ec | texture | `CTextureHolder::SetTextureByName(name, 6)` |

`ParticleRecord` (0x64 = 25 floats): `+0` pos, `+0xc` size, `+0x10` prev pos, `+0x1c` prev size, `+0x20` / `+0x2c` / `+0x38` per-type
velocity / parameter / velocity2 triples, `+0x44` frame, `+0x48` render frame, `+0x4c` counter, `+0x50` life, `+0x54` variant, `+0x58`
colour, `+0x5c` prev colour, `+0x60` dead [V]. `Update` copies pos -> prev pos, size -> prev size, colour -> prev colour, frame -> render
frame **before** the step callback, and restores them when the step returns 0 (the particle is drawn one last time unchanged) [V].

### 7.3 The type table (`CParticle::Init` 0x485540) [V]

Common prologue: `Reset`, bounds empty, `+0xe0 = 4`, `+0xcc = 100`, `+0xd8 = 500`, `+0xdc = 16`, flag 1. Then per type (only the values
the type sets; `LAB_` callbacks are code without a function symbol):

| type | texture | emit / step / tick | constants |
|---|---|---|---|
| 0x2000 fireball | FireSmoke | LAB_48C800 / LAB_48CAD0 | box -0.2..0.2 (z -1..0), size 0.8, damping 0.94, prob 88, min 1, dist 0.09, frames 15, life 15, interval 1 |
| 0x2001 rain, 0x2003 fog, 0x2004 snow, 0x2010 fire wall | TendriSmoke | `ParticleEmit_Cloud` 0x48E9E0 / `ParticleStep_Pulse` 0x48EC20 (0x2010: LAB_48ED60 + `ParticleTick_SetCount` 0x48EE20, one 0x38 record, min 20, max 1500) | box -0.2..0.2, size 0.8, growth 0.3, damping 0.85, prob 88, min 6, frames 15, life 32, interval 1 |
| 0x2002 blast | Explosion | LAB_48C090 / `ParticleStep_Grow` 0x48C590 | box -0.02..0.02, size 0.6, growth 1, damping 0.8, min 60, max 600, 15 records, life 15 |
| 0x2005 volcano | Vulkan | `ParticleEmit_Burst` 0x48BD50 / LAB_48C010 | box (0.2, 0.5, 0)..(0.3, 4, 0), size 2.5, growth intensity/64, damping 0.99, min 2, max 400, life 16 |
| 0x2006 healing | Healing | `ParticleEmit_Healing` 0x48B630 ("Particle effect Healing without carrier") / LAB_48BBE0 | min inf, life 8, size 0.5, additive |
| 0x2007 acid fog, 0x2008 poison cloud | PoisonCloud | `ParticleEmit_GridFill` 0x48B080 / `ParticleStep_FadeColour` 0x48B570 | prob 50, min 1, max inf, frames 15, life 3236, grid of spacing 1 over `+0x11c` |
| 0x2009 fireworks | Fireworks | LAB_48A8F0 / LAB_48ADA0 | box -0.1..0.1 (z -0.02..0.02), growth 0.3, min 20, interval 100, 3 records, additive |
| 0x200a tornado | Tornado | `ParticleEmit_Sparks` 0x489F70 / `ParticleStep_Trail` 0x48A310 | min 4, max 800, growth 0.2, 6 records, interval = packed colour |
| 0x200b | FireSmoke | LAB_489320 / LAB_489A50 | min inf, life -1 |
| 0x200c acid column | AcidRay | LAB_488F90 / `ParticleStep_Bounce` 0x489190 / `ParticleTick_Decay` 0x4892E0 | min 128, max 400, growth 1, 1 record |
| 0x200d banshee | Banshee | LAB_489BE0 / LAB_489EF0 | min inf, no records |
| 0x200e change hero, 0x2052 explosion | ChangeHero / Explosion | LAB_4889D0 / LAB_488DB0 / `ParticleTick_Explosion` 0x488F00 (camera shake) | min 10, max 4000, growth 1 |
| 0x200f rain streams, 0x2031..0x2033 | RainSnow | `ParticleEmit_Streams` 0x48EEA0 / LAB_48F300 | box -0.2..0.2, min 10, max 300, interval 15, life 6, attach 7, 1 record |
| 0x2011 fire arrow, 0x204e | FireArrowSmoke / RikArrowSmoke | LAB_48CBA0 / `ParticleStep_Respawn` 0x48CED0 | box -0.2..0.2, size 0.3, damping 1.03, dist 0.06, frames 15, life 15, `+0x12c = 0` |
| 0x2012 acid ray | AcidRay | LAB_48D050 / LAB_48D330 | box -1..1, size 0.3, dist 0.06, frames 15, life 15 |
| 0x2013 stench | Stench | LAB_48D420 / LAB_48D7F0 | box -1..1, size 1, prob 80, min 5, interval 3, life 45 |
| 0x2014 link | (none) | LAB_48D8D0 ("Particle effect Link without carrier" in `ParticleEmit_Link` 0x48E070 for the protections) / `ParticleStep_Vortex` 0x48DE30 | box -0.2..0.2, size 1, min 10, interval 3, life 45, `+0x130 = -10` |
| 0x2015 .. 0x2017, 0x201a protections / antimagic | ProtectionA / E / F, AntiMagic | `ParticleEmit_Link` 0x48E070 / `ParticleStep_Orbit` 0x48E7C0 | box -0.2..0.2 (z -1..0), size 1, prob 90, min 1, frames 15, life 4 / 15, additive |
| 0x2018 clay ring | ClayRing | LAB_48F7F0 / LAB_48FAB0 | min 80, interval 2, life 15, 10 records, additive |
| 0x2019 teleport | Teleport | `ParticleEmit_Ring` 0x48FDB0 / LAB_490060 | min 20, max 320, life 14, 1 record |
| 0x201b .. 0x2026 modifiers (12 types) | (none) | `ParticleEmit_Modifier` 0x490130 ("Particle effect Modifier without carrier") / LAB_490990 | min 80, interval 2, life 14 |
| 0x2027 .. 0x202a casting 4 / 7 / fire / 8 | Casting4 / Casting7 / FireSmoke / Casting8 | LAB_4918E0 / LAB_492030 | min 20 (6 for 0x202a), interval 5 / 1, life 5, attach 0 / 2, additive |
| 0x202b .. 0x202e casting 1 / 2 / 3 / 6 | Casting3 / 2 / 6 / 1 | LAB_490C10 / LAB_491230 | box -0.04..0.04, min 10, max 600, life 1 / 2 |
| 0x202f | Teleport | LAB_491420 / LAB_491830 | min 25, life 14 |
| 0x2030 invoke lightning / storm | TendriLight | `ParticleEmit_Weather` 0x4923B0 / LAB_492B80 | box -0.2..0.2, size 1, prob 88, min 250, max 600, frames 15, life 15, additive, 10 wind-band records |
| 0x2034 .. 0x2037 rain / snow fall | RainSnow | LAB_48F3B0 / `ParticleStep_Fall` 0x48F670 / `ParticleTick_RandomDrift` 0x48F6F0 | box -0.2..0.2, min 45, max 300, interval 15, life 6, attach 7, records = intensity |
| 0x2038 rain | (none) | `ParticleEmit_Rain` 0x492D30 / LAB_492F40 | min 10, max 600, box `+0x118 x +0x11c` (5 x 5) |
| 0x2039 | ShapeChange | LAB_492FC0 / `ParticleStep_RampAlpha` 0x493280 (ramp `g_0x78AA78`) | min 2500, max 2500 |
| 0x203a / 0x203b | ShapeChange | LAB_493300 / `ParticleStep_Ground` 0x4933F0 / `ParticleTick_SurfaceNormal` 0x4934F0 | min 128 / 256, 1 record |
| 0x203c | (none) | LAB_493560 / LAB_493770 | min 100, max 800, 80 records |
| 0x203d | Casting6 | LAB_493D10 / LAB_493D50 | min 1, max 1 |
| 0x203e | Teleport | LAB_494470 / `ParticleStep_Spiral` 0x494770 / `CObject3D::SetIntFromFloat` 0x494930 (tick) | min 50, max 800, interval 3, life 10, additive, 1 record |
| 0x203f / 0x2040 | Casting1 / Casting2 | LAB_493DE0 / LAB_494170 | min 30, max 600, 2 records |
| 0x2041 / 0x2042 | AcidRay / FireArrowSmoke | LAB_494950 / LAB_4949B0 | min 1, life 15, attach 7 |
| 0x2043 | Casting6 | LAB_494A10 / `ParticleStep_Smoke` 0x494CD0 (alpha `g_0x78AB98`) | min 10, max 300, attach 7, additive |
| 0x2044 .. 0x2046, 0x204c | (none) | LAB_494E00 / LAB_494F30 | min 3, life 0..3, attach 2, additive |
| 0x2047 / 0x2048 | (none) | LAB_4950B0 / LAB_495120; LAB_495320 / LAB_4955D0 | min 2 / 4, max 300 (0x2048), attach 7, additive |
| 0x2049 fountain | (none) | `ParticleEmit_Fountain` 0x4957E0 / LAB_495950 | min 5, max 300, attach 1, additive |
| 0x204a / 0x204b vision fog | VisionFog | `ParticleEmit_Beam` 0x495A30 / LAB_495C80 | min 8, max 300, life = colour 0xff / 0xff0000, additive, 2 records |
| 0x204d | (none) | LAB_495FB0 / LAB_4961B0 | min 1, max 300, attach 0, additive |
| 0x204f | Healing | LAB_496270 / LAB_4964B0 | min 8, max 300, attach 7, additive |
| 0x2050 | Tornado | `ParticleEmit_Sparks` / `ParticleStep_Trail` | min 4, max 800, growth 0.2, attach 7, 6 records |
| 0x2051 zone exit | ZoneExit | DAT_4965E0 / LAB_496840 / `ParticleTick_Shrink` 0x496990 | min 1, max 300, attach 7, 1 record |
| other | | `FatalError("CParticle::Create(): Unknown EType")` | |

### 7.4 Drawing (`CParticle::Render` 0x4976B0, `I3DFigureImpl::DrawParticleQuads` 0x502CA0) [V]

Culled when `+0x1c0 == 0` or `CViewFrustum::CullBox(+0x17c) == 2`. Frame fraction `f = clamp(g_0x7AF61C + g_0x7AFCA8 - tick, 0, 1)`;
per particle: position / size = `f * current + (1 - f) * previous`, colour interpolated likewise and packed to ARGB, uv rect pointer =
`+0x1d0 + renderFrame * 16`. Two static arrays hold up to 0x1000 `{x, y, z, size}` and `{colour, uv*}` entries (grown on demand);
`DrawParticleQuads(additive, positions, records)` transforms the points through the renderer's pipeline and emits four screen-aligned
corners per point (`size / w`), FVF 0x1C4, one indexed triangle list per system; the blend modes are set per group by `SortByDepth`.
There is no sorting inside a system and no per-particle rotation [V].

## 8. Sound (`CSoundManager` at `g_0x7AF784`, module 0x6B4000; Miles Sound System 6 "AIL") [V]

### 8.1 Setup

`CSoundManager::ctor` 0x6B6040 (was named Init) builds the sub-objects (section 8 of the structs batch) and `AIL_startup`; `Open`
0x6B6A30 (hwnd, reverb, device index (-1 default, else from `DirectSoundEnumerateA`), rate <= 44100, bits): `AIL_waveOutOpen` into
`+0x1c`, `AIL_set_DirectSound_HWND`, `MusicInitMiles` 0x6BAAC0 (the ".mp3" ASI codec, two sample handles `+0x104` output / `+0x1ac`
decoder, three buffers `+0x110` of `2 * AIL_minimum_sample_buffer_size`) and the music thread (`AfxBeginThread`, priority 15), then the
**15** `SoundChannel`s (0x54) at `+0x24c` each with an `AIL_allocate_sample_handle` (user data 0 = manager, 1 = channel), `+0x898 = 1`.
`main` then `AddSoundArchive` 0x6B7AB0 (a `CMappedResource` per sounds*.res, list `+0xe8`) and sets the three volumes from the config
(`g_0x79BCC8` effects, `g_0x79BCCC` voice, `g_0x79BCC4` music) [V].

### 8.2 Channels, playing, attenuation

`SoundChannel` 0x54: `+0` name (std::string), `+0xc` HSAMPLE, `+0x10` raw volume, `+0x14` volume % (2D) / attenuation (3D), `+0x18`
pan, `+0x1c` priority, `+0x20` loaded, `+0x24` loop, `+0x28` category (0 effects `+0x8b4`, 1 voice `+0x8b8`), `+0x2c` duration s,
`+0x30` done (EOS callback `Miles_MarkSampleDone` 0x6B75E0), `+0x34` is2D, `+0x38` height attenuation, `+0x3c` pos, `+0x48` / `+0x4c`
min / max range, `+0x50` id [V].

| call | arguments | what happens |
|---|---|---|
| `Play3D` 0x6B8600 (was PlaySound) | name, priority, float[3] pos, min range, max range, loop, category | `FindChannelSlot(priority)`; pos / ranges / is2D = 0; `UpdateChannel3D`; **volume 0 and not looping -> -1 without loading**; `LoadSample`; loop -> `AIL_set_sample_loop_count(0)`; volume, pan, EOS callback, reverb (`+0x8a0`), random start (`+0x8bc`), `AIL_start_sample`; returns the id |
| `Play2D` 0x6B8250 (was PlaySound3D) | name, priority, volume %, pan -100..100, loop, heightAttenuate, category, variant | is2D = 1; raw = `volume * categoryVolume * 1.27` (x the height curve when heightAttenuate); then as above. UI: priority 1000 volume 100 (`UI_PlaySound` 0x5F7E60); voices: priority 1, category 1 (`PlayVoice` 0x5380D0); ambient slots: priority 1, volume 50..100, pan 0..100 |
| `FindChannelSlot` 0x6B7D10 | priority | the channel with the **lowest** priority (free = none); `-1` when `priority <= lowest` (so equal priority never steals); a stolen **looping** channel is saved as a `SoundPendingRecord` (0x48: all parameters + `AIL_sample_position`) in `+0x884` under its id; `Play*` without a channel also create a pending record for looping requests (returned id) |
| `UpdateChannel3D` 0x6B7FC0 | channel | `d = \|(pos - listener).xy\|`: `d > max` -> 0; `d >= min` -> `100 * (1 - (d - min) / (max - min))`; else 100; x the camera-height curve (`listener z < 30`: 1, 30..70: linear to 0, > 70: 0) x category volume x 1.27 -> `+0x10`; pan from `acos(dot(listener right, dir) / d)` mapped to +-100 **but then forced to 0** (3D sounds are centred) |
| `LoadSample` 0x6B89F0 (was LoadSoundGroup) | channel, name, decompressMp3 | sample cache: map `+0x870` name -> `SampleCacheNode` (0x1c: data, name, last tick, size, refcount), LRU list `+0x854` (head = most recent); miss: file through the archives (`vt[5] OpenFile`), eviction while `+0x850 + size > 8 MB` or a node is unreferenced and 60 s old, copied into AIL memory (`AIL_decompress_ASI` for mp3), `AIL_set_named_sample_file`; `+0x2c = Wav_GetDuration` 0x6B96E0, `+0x20 = 1`, `+0x50 = +0x268++` |
| `SetChannelPosition` 0x6B92F0 / `SetChannelVolume` 0x6B9100 (was Stop3DSound) | id, pos / ranges; id, volume % | move / re-attenuate a playing channel (`CEffectSound::Update`, `CEffectSoundSeq`) |
| `StopSound` 0x6B94A0 | id | `AIL_end_sample`, cache refcount--, id -1; the pending record of that id dropped (25 callers) |
| `SetListener` 0x6B9750 | float[3] pos, float[3] right | `+0x250` / `+0x25c` (normalised xy), every loaded channel re-attenuated (`CCamera::Update`) |
| `SetEffectsVolume` 0x6B7740 (was SetMasterVolume) / `SetVoiceVolume` 0x6B78A0 (was SetEffectsVolume) / `SetMusicVolume` 0x6B7A00 | % | `+0x8b4` / `+0x8b8` + re-attenuate all; music `SoundVolume +0x144` + `AIL_set_sample_volume(+0x104)`, 0 stops the music |
| `StopAll` 0x6B71C0, `PauseAll` 0x6B7320, `ResumeAll` 0x6B7380 | | all channels; pending map and slots cleared by StopAll |

`Update` 0x6B9B10 (per frame from `CWorldClient::Update`): (1) channels marked done -> released (refcount--); (2) every free channel
takes the pending record with the **highest priority** from `+0x884` and resumes it (`LoadSample`, loop 0, saved sample position);
(3) the 8 `StreamSlot`s (`+0x5f0`, 0x4c: a wav list played file after file in 3D; `StartStreamSlot` 0x6BA2D0 / `StreamSlot_PlayNext`
0x6BA140 / `StopStreamSlot` 0x6BA7C0; `CEffectSoundSeq`, buff loops) start their next file when the current id is gone; (4) every 100 ms
the 16 `AmbientSlot`s (`+0x26c`, 0x38: name list, pos, min / max distance, min / max interval; `StartAmbientSlot` 0x6B9950,
`CEffectAmbientSound`): beyond max distance silent; between min and max the intervals shrink by `(max - min) / (max - d)`; past the min
interval a `1 / ((max - min) / 100)` chance per step, then `Play2D(random name, 1, 50..100, 0..100)` [V].

### 8.3 Music: the stream player (`CSoundManager +0x100..+0x1c8`) and the director (`CMusicSystem` 0x58) [V]

The player block is addressed through the manager (`this` = CSoundManager in every function): state `+0x100` (0 stopped, 1 playing,
2 switching), output sample `+0x104`, stream `+0x108` / decoder vtable `+0x10c` (read `+4`, seek `+8`, close `+0xc`), buffers `+0x110`
x 3 of `+0x11c` bytes, `+0x120` bytes per buffer, play position `+0x130`, switch position `+0x134`, `SoundVolume +0x144` (fade step
1/60 per 100 ms = 6 s), current / pending names `+0x160` / `+0x164`, `Markers1` / `Markers2` float vectors `+0x168` / `+0x178`, mode
`+0x1a0`, critical sections `+0x188` (names) / `+0x1b0` (stream), stop flag `+0x248`.

| function | behaviour |
|---|---|
| `MusicPlayTrack` 0x6BB020 (name, mode) | mode 1 switch now, 2 fade out (6 s) then switch at the end, 3 switch at the first `Markers1` entry >= the play position, 4 switch when the current track ends; same name -> keeps playing (fade in if it was fading out); a pending switch with a lower mode is not overridden; sets state 2, `+0x164`, `+0x134` |
| `MusicSetMode` 0x6BB240 (mode) | 4 = stop (close the stream, state 0, fade out with a 2 s deadline); other modes = `MusicPlayTrack("", mode)` = switch to silence |
| `MusicThreadProc` 0x6BB910 | every 100 ms: `SoundVolume::Update` + `AIL_set_sample_volume`; for each ready AIL buffer: rotate the three indices, `+0x130 += +0x120`, `AIL_load_sample_buffer`, `MusicFillBuffer` |
| `MusicFillBuffer` 0x6BB3A0 (was StartStream) | fills the next buffer from the decoder stream; at the switch position opens the pending track (`CResourceBase::OpenFile(+0x24, musicPath + name)`), loads its markers (`MusicLoadMarkers` 0x6BBA00: registry `+0xcc` "Markers1" / "Markers2" per track name), state 1, fade in; pads with the silence byte `+0x140`; loops at `Markers2` / stops at the end depending on the mode |
| `MusicGetCurrentTrack` 0x6BAEA0 | the current name when state 1 / 2 (the director polls it) |
| `MusicStartFadeOut` 0x6BAFA0 / `MusicStartFadeIn` 0x6BAFE0 | fade mode 2 / 1, step 1/60 |
| `SequenceThreadProc` 0x6BBCB0, `SequenceRestart` 0x6BBB80 | an XMIDI sequence player (`+0x1cc..+0x22c`, `AIL_*_sequence`) with the same marker registry; no caller starts a sequence in the code read [?] |

`CMusicSystem` (vt_CMusicSystem, a CBaseObject created with the world; `Init` 0x6B4710 reads `<g_0x7AF45C>music.reg`: `[Common]
Credits / MainMenu`, then per zone section two `MusicZoneRecord`s (0x38: name, dungeon flag, Briefing, Constructor, CalmOpen or
CalmDungeon list `+0x10`, Combat list `+0x24`)). `SetZone` 0x6B4E30 (zone, dungeon) selects the record (fatal when missing) and
stops the music; `Update` 0x6B5350 (slot 6, every tick): combat = `g_0x7AFC9C == 2` (the battle state): entering -> a random Combat
track with mode 3 (switch at the next marker), leaving -> `MusicSetMode(2)` (fade out) and a silence timer `+0x54 = now + 120 s +
rand 60 s`; while calm and silent, when the timer expires a random calm track plays with mode 4 then `MusicSetMode(1)`.
`PlayBriefing` 0x6B4F90 (dialogs), `PlayConstructor` 0x6B5080 (camp), `PlayMainMenu` 0x6B51F0, `PlayCredits` 0x6B5170 play the named
track + ".mp3" with mode 4 after `MusicSetMode(4)`; `PlayForcedTrack` 0x6B5270 (server message 0x0D, script music) replaces the zone
playlist until the track ends; `Stop` 0x6B4F70 [V].

## 9. What is not known

* The readers of the Monsters loot fields (`+0xc8` / `+0xd8`) and of RaceModels `+0x158..+0x164`, the Levers sound / text / places,
  and the udb NPC `+4` / HitLocations `+4` / `+0x40` fields: no reader in the modules of this round.
* `DescRecordA` 0x4694F0 (0x74) / `DescRecordB` 0x4698F0 (0x48): editor-side records, no runtime caller [untestable].
* `Scene3ds_BuildFigures` 0x46F1F0 (10 KB): a 3DS -> CFigure converter never reached by the game [untestable].
* ~~Particle world block~~ settled [V]: `CWorld +0x24` windDir (3 floats) + `+0x30` windStrength = the wind (SetWind, net message 0x72) copied into `CParticle +0x140`. The `LAB_` emit / step bodies (~40 per-type callbacks without function symbols) stay unread.
* ~~3D pan zeroed~~ settled [V] from the machine code (0x6B8215..0x6B8243): `pan +0x18 = round(...)`, then `neg` when the cross product is negative, then `ret`: there is NO zeroing (decompiler artefact). 3D sounds are panned +-100.
* Reverb `+0x8a8..+0x8b0`: set by `CSoundManager::SetReverbParams` 0x6B69E0, whose only caller is `CWorldClient::LoadZone` 0x567AB0 (per zone) [V]. The XMIDI sequence player (`+0x1cc..`): never started by the code read [I].
* `CSoundDesc +0x70` and the use of SOUND_RANGE2 / MIN / MAX by `CEffectSound` (spells.md gives `+0x24c` volume / `+0x250` radius).

## 10. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| `docs/file-formats/database-format.md` | "scalar leaf values and anonymous list items never receive the odd +1 flag even when wide" | the game's reader (`Archive_ReadTagged` 0x409070) decides the length width by the low bit of the first byte for **every** entry; a wide length without the flag is misread. The engine writer (`Archive_WriteTagged`) always sets it |
| `database-format.md` | the 10-byte trailer "does not encode anything" | `Archive_Close`: an empty tag-0 section + a tag-2 section with `{tag 2: int version = 1}` (`DbArchive +0x54`) |
| `database-format.md` | "Field 1 of every record is the Name" (Acks) / FLD numbering | in the six gameplay databases the name is tag 0 (`FLD0-0`), as the headers say; the game's record readers start at tag 0 |
| `docs/file-formats/mob-format.md` | `UNIT_WEAPONS = 0xBBBC0008`, `UNIT_ARMORS = 0xBBBC0009`, `UNIT_SPELLS = 0xBBBC0007`, `UNIT_QUEST_ITEMS = 0xBBBC0005`, `UNIT_QUICK_ITEMS = 0xBBBC0006` | the 0xBBBB family: 0xBBBB0005 quest items, 0006 quick items, 0007 spells, 0008 weapons, 0009 armors (0xBBBC0000 is UNIT_LOGIC) |
| `structs.tsv` CUnitDesc | `+0x2bc weapons 0xBBBC0008`, `+0x2e4 questItems`, `+0x2f8 armors`, `+0x30c spells`, size 0x330 | `+0x2bc` quest items, `+0x2d0` quick items, `+0x2e4` spells, `+0x2f8` weapons, `+0x30c` armors (tags 0xBBBB0005..0009); `+0x8c` logic count, `+0x320` need-import; the ctor allocates 0x324 |
| `findings.md` 0x469000 / `world-server.md` 4.4 / `functions.tsv` | `CLeverDesc` = vt 0x73C650 (0xBBAB), `CTrapDesc` = vt 0x73C6B0 (0xBBAC); "0xBBAB0000 levers (0xd0), 0xBBAC0000 traps (0xa4)" | vt 0x73C650 is the **magic trap** (kind 7, `GetTypeName` "MagicTrap", 0xBBAB chunks, 0xd0 bytes); vt 0x73C6B0 is the **lever** (kind 8, 0xBBAC chunks, 0xa4 bytes) |
| `structs.tsv` UnitProto | `+0x24 size`, `+0x50 naturalSkill0 [?]`, `+0xb0 resistanceBases`, `+0x120 speedMult`, size 0x124 | `+0x24` Altitude, `+0x50` Base Level, `+0xb0` Detections (detectability bases), the record is 0x120 (no `+0x120` field) |
| `findings.md` 0x53A000 / `unit-ai.md` | "HeroRecord 0x60" (udb tag 4) | the NPC block; named `UdbNpcRecord` here (the party `HeroRecord` is 0x784); "UnitDbEntry" = `UdbRaceModel`, "Record44" = `UdbHitLocation` |
| `findings.md` 0x6B4000 | "17 channel entries of 0x54 bytes at +0x24C" | 15 (`0x4ec / 0x54`, `Open` allocates `0xf`) |
| `findings.md` 0x6B4000 / `functions.tsv` | `CSoundManager::Play3D` 0x6B8600 "by name/id", `CSoundManager::Play2D` 0x6B8250 | 0x6B8600 takes a position and ranges (`Play3D`), 0x6B8250 takes volume and pan (`Play2D`) |
| `functions.tsv` | `CMusicSystem::dtor` 0x6B44B0 "construct", `CMusicZone::PlayBattle` / `PlayCamp`, `CMusicTrackObj::Play1 / Play2` | 0x6B44B0 is the destructor; the four play the Briefing / Constructor / MainMenu / Credits tracks of `music.reg`; there is one director class `CMusicSystem` and one embedded stream player in `CSoundManager` |
| `functions.tsv` | `CSoundManager::ctor` 0x6B6040 | the constructor (the slot-2 `Init` is 0x6B6D70); `CSoundManager::SetEffectsVolume` 0x6B7740 is the effects volume, `CSoundManager::SetVoiceVolume` 0x6B78A0 the voice volume |
| `functions.tsv` | `UnitLogicRecord_*` = "guard point", `GuardPointDesc_*` = "unit logic guard point" | 0xBBBD0000 = the patrol / guard point (`MobLogicPoint`), 0xBBBE0000 = its look-at / action record (`MobLookAtRecord`) |
| `findings.md` 0x6AE000 | quests.qdb "TODO: SerializeFields live in the items module" | 0x6AD7D0 / 0x6AD900 read above; and quests.qdb is loaded **once** by main, the per-map `quest.reg` is merged by `ParseQuestReg` |

## 11. Function index (address, name, one line)

| addr | name | line |
|---|---|---|
| 0x409070 / 0x409110 | Archive_ReadTagged / WriteTagged | the tag + length primitive |
| 0x4092B0 / 0x4093A0 / 0x4098D0 / 0x409980 / 0x409A60 | Archive_SerializeBytes / SerializeCString / BeginTag / EndTag / CountTag | field access by tag |
| 0x409AE0 / 0x40A000 | Archive_Open / Close | bind the stream; write the trailer |
| 0x52DF40 | DbStream_SerializeCStringVector | tagged string lists |
| 0x53A7B0 | DbArchive::ctor | the archive object |
| 0x463030 / 0x463390 / 0x464750 | LeverRecord::SerializeFields / LeversDb::Load / NameMap_FindOrInsert | Levers.ldb |
| 0x52D6F0 / 0x52D740 / 0x52DA60 / 0x52DC70 / 0x52DEE0 | UdbHitLocationSlot / UnitProto / UdbNpcRecord / UdbRaceModel / UdbHitLocation ::SerializeFields | units.udb records |
| 0x53A200 / 0x53A8E0 | UnitsDb::Load / CountSoundVariants | units.udb |
| 0x53D240 / 0x53D650 / 0x5144D0 / 0x514930 | UnitsDb::FindRaceModelByName / FindHitLocationByName, Units_FindProtoByName / Units_FindDbEntryByName | lookups |
| 0x6AD7D0 / 0x6AD900 / 0x6AE3A0 / 0x6AE8E0 / 0x6B0600 | Quest / Briefing ::SerializeFields, QuestsDb::Load / ParseQuestReg, BriefingTable_AddUnique | quests |
| 0x5810A0 | QuestFiles_Scan | quest.reg per map |
| 0x469CA0 / 0x46A5C0 / 0x46AB20 / 0x46A940 / 0x46AFB0 | CDescBase::ctor, CObjectDesc::ctor / OnChunk / WriteFields / Spawn | base descriptors |
| 0x46B210 / 0x46B3C0 / 0x46B340 / 0x46B310 | CTorchDesc::ctor / OnChunk / Write / Spawn | torch |
| 0x46B4A0 / 0x46B7D0 / 0x46B680 | CMagicTrapDesc::ctor / OnChunk / Write | magic trap (0xBBAB) |
| 0x46BD40 / 0x46BF20 / 0x46BE80 | CLeverDesc::ctor / OnChunk / Write | lever (0xBBAC) |
| 0x46C010 / 0x46D080 / 0x46CF10 / 0x46C4D0 | CUnitDesc::ctor / OnChunk / Write / Spawn | unit |
| 0x46CBC0 / 0x46C900 / 0x46C7F0 / 0x46C6A0 / 0x46C610 / 0x46C590 | MobLogicSlot_OnChunk / Write, MobLogicPoint_OnChunk / Write, MobLookAtRecord_OnChunk / Write | unit logic chunks |
| 0x46D330 / 0x46D420 / 0x46D3F0 | CEffectDesc::ctor / MoveBy / SetEnabled | effect descriptor base |
| 0x46D520 / 0x46D730 / 0x46D680 / 0x46D870 | CParticleDesc::ctor / OnChunk / Write / Spawn | particle source |
| 0x46D960 / 0x46DBF0 / 0x46DB20 / 0x46DD70 | CLightDesc::ctor / OnChunk / Write / Spawn | light |
| 0x46DE70 / 0x46E190 / 0x46E090 / 0x46E380 | CSoundDesc::ctor / OnChunk / Write / Spawn | sound |
| 0x46E5E0 | Diplomacy_ctor | DIPLOMATION record |
| 0x4981B0 / 0x498400 / 0x498480 / 0x498630 / 0x498940 / 0x498870 / 0x4987C0 / 0x498B20 / 0x498BB0 / 0x497FE0 | CParticleManager::CreateByKind / CreateParticleSystem / RemoveFinished / RemoveUnreferenced / SortByDepth / AllocParticle / ReleaseParticle / EnableWeather / DisableWeather / Done | manager |
| 0x4847E0 / 0x485540 / 0x496EB0 / 0x4976B0 / 0x496C50 / 0x496A20 | CParticle::Reset / Init / Update / Render / SpawnParticle / InitParticleRecord | system core |
| 0x4849F0 / 0x484AE0 / 0x484C00 / 0x485280 / 0x485360 / 0x485380 / 0x485440 / 0x485450 / 0x485480 / 0x484710 / 0x498310 | CParticle::Attach / GetWorldPos / GetCarrierOffset / GetCarrierExtent / Stop / Clear / Mute / Unmute / ScaleParams / Done / dtor | system helpers |
| 0x488F00 .. 0x496990 | ParticleEmit_* / ParticleStep_* / ParticleTick_* | the named per-type callbacks (table 7.3) |
| 0x502CA0 | I3DFigureImpl::DrawParticleQuads | the quad draw (renderer.md) |
| 0x6B6040 / 0x6B73E0 / 0x6B6A30 / 0x6B6D80 / 0x6B65D0 | CSoundManager::ctor / InitFields / Open / Done / dtor | lifetime |
| 0x6B8600 / 0x6B8250 / 0x6B7D10 / 0x6B7FC0 / 0x6B89F0 / 0x6B94A0 / 0x6B92F0 / 0x6B9100 / 0x6B9750 / 0x6B9B10 | Play3D / Play2D / FindChannelSlot / UpdateChannel3D / LoadSample / StopSound / SetChannelPosition / SetChannelVolume / SetListener / Update | channels |
| 0x6B7740 / 0x6B78A0 / 0x6B7A00 / 0x6B7610 / 0x6B71C0 / 0x6B7320 / 0x6B7380 | SetEffectsVolume / SetVoiceVolume / SetMusicVolume / UpdateAll3D / StopAll / PauseAll / ResumeAll | volumes |
| 0x6B7AB0 / 0x6B7110 / 0x6B7060 / 0x6B8960 / 0x6BBDF0 | AddSoundArchive / ListSoundFiles / ClearSoundArchives / ForEachSoundArchive / SampleCache_FindOrInsert | archives and cache |
| 0x6B9950 / 0x6B9A30 / 0x6B9AA0 | StartAmbientSlot / SetAmbientSlotParams / ClearAmbientSlot | ambient slots |
| 0x6BA2D0 / 0x6BA140 / 0x6BA400 / 0x6BA5E0 / 0x6BA7C0 | StartStreamSlot / StreamSlot_PlayNext / SetStreamSlotPosition / SetStreamSlotParams / StopStreamSlot | stream slots |
| 0x6BAAC0 / 0x6BB020 / 0x6BB240 / 0x6BB3A0 / 0x6BB910 / 0x6BBA00 / 0x6BAEA0 / 0x6BAFA0 / 0x6BAFE0 / 0x6BAD40 / 0x6BAE20 / 0x6BA950 | Music* (InitMiles / PlayTrack / SetMode / FillBuffer / ThreadProc / LoadMarkers / GetCurrentTrack / StartFadeOut / StartFadeIn / SetVolume / SetPath), Mp3Stream_ReadCallback | the stream player |
| 0x6BBCB0 / 0x6BBB80 / 0x6BBC70 / 0x6BBAD0 | SequenceThreadProc / SequenceRestart / SequenceStart / SequenceGetCurrentName | XMIDI player |
| 0x6B5CD0 / 0x6B75E0 / 0x6B96E0 | SoundChannel::End / Miles_MarkSampleDone / Wav_GetDuration | channel helpers |
| 0x6B5D20 / 0x6B5DB0 / 0x6B5E80 / 0x6B5EE0 / 0x6B5FF0 | SoundVolume::Reset / SetPercent / SetFadeDeadline / Update / Recompute | fades |
| 0x6B4710 / 0x6B5350 / 0x6B44B0 / 0x6B4E30 / 0x6B4F70 / 0x6B4F90 / 0x6B5080 / 0x6B51F0 / 0x6B5170 / 0x6B5270 | CMusicSystem::Init / Update / dtor / SetZone / Stop / PlayBriefing / PlayConstructor / PlayMainMenu / PlayCredits / PlayForcedTrack | the music director |
