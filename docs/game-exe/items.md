# Items, inventory and the party in game.exe (CItem*, ItemsDb, CItemConstructor, CParty, HeroRecord, the shops)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x69F000-0x6AE000
item classes + items.idb, 0x699000-0x69B000 shop stock generator, 0x65F000-0x664000 CParty / HeroRecord / PartyStore,
0x461000-0x463000 CInventory + loot collector, plus the callers in CUnit / CUnitServer (0x52E000-0x55E000), CWorldServer
(0x58E000-0x598000), CPlayer* (0x664000-0x674000) and the camp screen (0x60E000-0x625000)). Spells and effects are another
document; here only the item side of wands / scrolls / enchantments and the spell-side entry points are named. Combat use of
items (damage rolls, armor tiers, durability wear) is `docs/game-exe/combat.md` sections 5-6. Batch files:
`_cpr/claude-re/full/batch-items.tsv` (218 function rows) and `batch-items-structs.tsv` (203 field rows).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs; offsets are bytes; "slot N" = virtual slot N (offset N*4) of the object's vtable.

## How to re-implement it (short version)

1. **An item is a small CObject** (`CItem`, 0x4c bytes + per-kind fields): `+0x10` kind (0x3004 weapon, 0x3005 armor, 0x3006
   quick item, 0x3007 loot / material / blueprint, 0x3008 spell container, 0x3009 quest item), `+0x18` id = `protoRow << 16 |
   materialId`, nine floats `+0x20..+0x40` (durability cur/max, mana cur/max, mana regen 0.001, weight, price, size, slots),
   `+0x44` the embedded `CSpell`, `+0x48` the stack count / charges. Every stat is computed once, when the item is built, from
   an items.idb prototype row and a material row (section 3); afterwards only durability, mana and count change.
2. **items.idb** (`ItemsDb` 0xc0 at `g_0x7C86D8`, `ItemsDb::Load` 0x6A67A0) is six STL vectors of fixed-size records with name
   maps: Materials 0x50, Weapons 0xb4, Armors 200, QuickItems 0x8c, QuestItems 0x7c, LootItems 0x78. The field tags of the
   records are the spreadsheet FieldIDs of `docs/file-formats/database-format.md`; section 2 maps every tag to its offset.
3. **Building an item** = `CItemConstructor` (one per world and per party): `BeginCreateItem(kind)`, `SetProto`, `SetMaterial`,
   `SetSpell`, `FinishItem` -> `ApplyProto` 0x6A5D60 computes a material-adjusted copy of the prototype and calls the item's
   `InitFromProto`. `CItemManager::CreateItemByName` 0x6AC970 parses `"name.material[count or spell]"`; `CreateItem` 0x6AD000
   takes (kind, proto row, material row). Every clone of an item goes through `CWorld::CloneItem` (slot 24) = CreateItem +
   `CloneInto` (slot 15).
4. **Where items live**: a unit holds raw pointers in four slot arrays (`CUnit +0x440[4]` weapons, `+0x450[4]` belt / quick
   items, `+0x460[8]` spell containers, `+0x480[7]` armor by type, `+0x4a0[]` extra quest items); the hero's persistent copy is
   the `HeroRecord` (0x784 bytes) in the player's `CParty` (`CPlayer +0x70`): the same four arrays `+0x730 / +0x740 / +0x750 /
   +0x760` plus the stats block; the party's loose inventory is `CParty +0x40[]`, money `+0x50`. `StorePartyFromUnits`
   0x590C70 copies unit -> record (each unit item gets a `PartyItemLink +0x1c` to its record twin), `CreateHeroUnits` 0x566F70
   / `SpawnPartyHeroes` copy record -> unit. On the client a hero unit answers `GetWeaponSlotItem` & co. straight from the
   record. Equipping is `CUnit::EquipArmor` / `AddWeaponSlotItem` / `AddQuickSlotItem` / `AddSpellSlotItem` (slot pointers +
   net section 5); the weapon in hand is the slot index `CUnit +0x26c` (`CUnitServer::SelectWeapon` slot 25).
5. **Persistence**: `CParty::Write` 0x661C10 is the `.mp` body (zone, item and spell object headers + detail blocks, backpack
   ids, hero records, variables, money ^ random); `CItem::Write` 0x6A0360 is the 48-byte detail block common to every kind
   (section 5). The campaign save embeds the same stream (`CParty::ArchiveMp`). Over the net the server sends new items as
   opcode 5 (object blob) + opcode 6 (money + ids) (`CPlayerServer::SendItems`), the whole party as opcode 0x80 / 0x83 / 0x50.
6. **Shops** (`CGSPlayer`): the stock is generated, not stored: `GenerateItemShop` 0x4A6DF0 draws 40..50 weapons, armors and
   quick items from the protos whose "Shop N" bit is set, picks a material of the right type whose stock is not exhausted
   (price-weighted), embeds a random spell in half of them, adds 1..3 blueprints per proto and 20..50 materials
   (`ShopStock::GenerateItems` 0x69AE20). Prices in the camp are `CItem::GetPrice` x the mode coefficient
   (`docs/game-memory.md`, verified live); deconstruction and construction are `CCampItemConstr::Deconstruct` 0x6129C0 /
   `TryConstruct` 0x612600.

## 1. Conventions

| Thing | Value |
|---|---|
| item object | `CItem` (vtable 0x743DA4, 22 slots, 0x4c bytes) -> `CItemWeapon` 0x5c, `CItemArmor` 0x8c, `CItemQuick` 0x50, `CItemQuest` 0x50, `CItemLoot` 0x58, `CItemSpell` 0x50; `CInventory` (0x64, typeId 0x17) is an unused sibling that hosts the base Read / Write [V] |
| kind / typeId | `+0x10` kind 0x3004..0x3009 (the object kind the manager creates, `CItemManager::CreateByKind` 0x6AC810); slot 12 typeId 0x11 weapon, 0x12 armor, 0x13 quick, 0x14 quest, 0x15 loot, 0x16 spell container [V] |
| item id | `+0x18` = `protoRow << 16 \| materialId & 0xffff`; the low word is the Materials "ID" column (every table lookup indexes the vector with it, so ID must equal the row) [V: `ApplyProto`] |
| tables | `g_0x7C86D8` materials (begin) / `0x7C86DC` (end), stride 0x50; `0x7C86F8/FC` weapons 0xb4; `0x7C8718/1C` armors 200; `0x7C8738/3C` quick 0x8c; `0x7C8758/5C` quest 0x7c; `0x7C8778/7C` loot 0x78; every reader tests `row < (end-begin)/stride` and carries a NULL record on when it fails (`docs/game-exe/item-tables.md`) [V] |
| spell tables | `g_0x7C8510` spell protos (0x84 stride, name map 0x7C851C), `0x7C8530` perks / spell modifiers (0x24, map 0x7C853C), the three S_* filter tables `0x7C8550` S_Spells, `0x7C855C` S_Armors, `0x7C8568` S_Weapons (0x34 records, `+0x30` per-player mask) [I] |
| weapon types | proto `+0xc`: 0 sword 1 axe 2 dagger 3 spear 4 hammer/club 5 bow 6 crossbow (model prefixes sw ax dg sp hm bw cb) [V: `GetModelName`] |
| armor types / slots | proto `+0xc` = equipment slot: 0 helmet (hl) 1 plate (pl) 2 legs (lg) 3 shield (sh) 4 pants (pt) 5 boots (bt) 6 gloves (gl); tier 2 for 0..2, tier 1 for 3..6 (`CItemArmor::GetArmorTier` 0x6A31A0) [V] |
| quick item subtypes | QuickItems "ItemID" `+0x6c`: 2 consumable (order 10), 5 charged magic item (uses its mana), 8 wand / scroll / spell container (uses charges `+0x48`) [V: `CanAffordSpell`, `QuickItemUsability`, `GenerateItems`] |
| loot categories | LootItems "TypeID" `+0x6c`: 1 material (`+0x54` = material row), 2 instruction / blueprint (`+0x50` table 0 weapon 1 armor 2 quick 3 spell 4 perk, `+0x54` row) [V] |
| damage types | index 0..6 = impaling, slashing, crushing, thermal, chemical, electrical, general (the 7-float lists everywhere) |
| refcounts | items: `+4` int, released with slot 4; `PartyItemLink` and the proto refs: 16-bit at `+4` (`RefCounted16_Release` 0x69FD50) |
| random | CRT `rand()` for shop counts / picks, ISAAC words (`g_0x7A9D48[g_0x7A9D44]`) for percentages and damage rolls |

## 2. Item data: items.idb and the prototype records

`ItemsDb::Load` 0x6A67A0 [V]: `items.idb` is opened through a `CMappedResource` (database.res, or databaseLMP.res in
multiplayer: `Game_SessionTick` swaps the whole `ItemsDb` object, see item-tables.md) into a `CMemStream`; one tagged archive per
block: tag 1 Materials -> vector `+0` (map `+0xc`), 2 Weapons `+0x20` (map `+0x2c`), 3 Armors `+0x40` (`+0x4c`), 4 QuickItems
`+0x60` (`+0x6c`), 5 QuestItems `+0x80` (`+0x8c`), 6 LootItems `+0xa0` (`+0xac`); each map is `std::string name -> record*`
(filled from `+4` of every record). The record vtables (0x744068 weapons, 0x744064 armors, 0x74405C quick, 0x744058 quest,
0x744054 loot) have one slot: `GetItemKind` returns 0x3004 / 0x3005 / 0x3006 / 0x3009 / 0x3007 (constants, 0x6A6430..70).

**Materials** (`ItemMaterial`, 0x50, `ItemMaterial::SerializeFields` 0x69F620) [V]:

| off | tag | column | use |
|---|---|---|---|
| +0 | 0 | Name | `material.<name>` |
| +4 | 1 | Type | must equal the prototype's M.Type (shop, camp constructor) |
| +8 | 2 | Code | |
| +0xc | 3 | ID | the low word of every item id made with this material |
| +0x10 | 4 | Price | price += Components x this |
| +0x14 | 5 | Weight | weight factor |
| +0x18 | 6 | Mana | mana += this |
| +0x1c | 7 | Slots | slots (complexity) += this |
| +0x20 | 8 | Durability | durability factor |
| +0x24 | 9 | Skill | not read in this round [?] |
| +0x28 | 10 | Damage | damage factor (weapons) |
| +0x2c | 11 | Absorption[7] | armor factor per damage type |
| +0x48 | 12 | Unk12 | ORed into the armor flags `+0x8c` / `+0xb0` |
| +0x4c | 13 | Shop 1..5 | bit (player index - 1): the shop sells it |

**Prototype base** (`ItemProtoBase`, 0x6c, 0x69F700; the five item tables share it) [V]:

| off | tag | column | notes |
|---|---|---|---|
| +4 | 0 | Name | the key of `CreateItemByName` |
| +8 | 1 | Type | "" -> the shop skips the proto |
| +0xc | 2 | TypeID | weapon kind / armor slot (table above) |
| +0x10 | 3 | M.Type | material Type accepted, "" = no material |
| +0x14..+0x1c | 4..6 | Unk4, TTI, TTI2 | texture type indices |
| +0x20 | - | CStringArray | runtime only (slot 18 of the item returns `[0]`) |
| +0x34 | 7 | Price | |
| +0x38 | 8 | Weight | |
| +0x3c | 9 | Size | |
| +0x40 | 10 | Mana | |
| +0x44 | 11 | Slots | complexity (int) |
| +0x48 | 12 | Durability | |
| +0x4c | 13 | Components | materials consumed by construction / returned by deconstruction |
| +0x50 | - | CStringArray | runtime only |
| +0x64 | 14 | Shop 1..5 | shop bits |
| +0x68 | 15 | Dec. | byte, deconstructable (`Item_IsUsable`) |

Per table (the Ghidra decompiles index these as `this[1].field_xx` because the structs are declared short) [V]:

| record | off | tag | column |
|---|---|---|---|
| WeaponProto 0xb4 | +0x6c | 0x15 | Actions (attack frames: period = this x 15 / action speed) |
| | +0x70 float[2] | 0x16 | Unk22 |
| | +0x78 byte[3] | 0x1b | Unk27 |
| | +0x7b byte[8] | 0x1c | Unk28: +0x7b melee skill bonus, +0x7c archery, +0x7d.. attribute bonuses (combat.md) |
| | +0x84 / +0x88 | 0x1d / 0x1e | Attack / Defence modifier (client AttackParams[6] / [7]) |
| | +0x8c | 0x17 | Range -> `CItemWeapon +0x58` (not scaled; 0 for many melee rows) |
| | +0x90 / +0x94 | 0x18 / 0x19 | D.Min / D.Max; **the code uses D.Max as the spread** (max = min + D.Max) |
| | +0x98 float[7] | 0x1a | damage proportions |
| ArmorProto 200 | +0x6c | 0x15[0] | M. Absorb. (factor of the main values) |
| | +0x70 float[7] | 0x15[1..7] | main values -> `CItemArmor +0x4c` |
| | +0x8c | 0x15[8] | flags (Unk21-8), ORed with material +0x48 |
| | +0x90 | 0x16[0] | A. Absorb. |
| | +0x94 float[7] | 0x16[1..7] | additional values -> `CItemArmor +0x68` |
| | +0xb0 | 0x16[8] | flags (Unk22-8) |
| | +0xb4 byte | 0x17 | Apply Wounds |
| | +0xb8 | 0x18 | Wear Order (`ArmorProto_GetWearOrder` 0x6A3200) |
| | +0xbc byte[3], +0xbf byte[8] | 0x19, 0x1a | Unk25 / Unk26 |
| QuickProto 0x8c | +0x6c | 0x15 | ItemID = subtype (2 / 5 / 8) |
| | +0x70 | 0x16 | GD Level -> `CItemQuick +0x4c` (icon variant) |
| | +0x74 | 0x17 | Damage |
| | +0x78 | 0x18 | Unk24 |
| | +0x7c CString | 0x19 | Spell: created and embedded for subtype 8 protos (spell containers) |
| | +0x80 byte[3], +0x83 byte[8] | 0x1a, 0x1b | science / stealing modifiers (never written by DBEditor), Unk27 |
| QuestProto 0x7c | +0x6c | 0x15 | ScriptID -> `CItemQuest +0x4c` |
| | +0x70 vector<CString> | 0x16 | Zones (`FilterQuestItems` keeps the item only in a listed zone) |
| LootProto 0x78 | +0x6c / +0x70 / +0x74 | 0x15 / 0x16 / 0x17 | category / referenced table / row |

Name lookups: `StrHashTable_LookupId` 0x4AB720 = weapon map, `LookupId2` 0x4AB820 = armor map, `0x613820` = quick map
(`g_0x7C8744`), `ItemsDb_FindLootProtoByName` 0x6AD580, `0x69D380` = quest map, `0x69D290` = material map [I: from the hash
helpers each one calls and the fill order in `ItemsDb::Load`]. The item-side helpers that index by id: `ItemId_GetProto`
0x6A2D40, `CItem::GetProtoRecord` 0x6A0110 (any kind, by `+0x10`), `CItem::GetProtoSubtype` 0x6A4230 (proto `+0x6c` of any
kind), `CItemArmor::GetArmorType` 0x6A39E0 (reads the **armor** table whatever the item is).

## 3. Building an item: CItemConstructor and the formulas

`CItemConstructor` (vtable 0x744020, typeId 0x1c, 0x34 bytes; `CWorld +0x58` and `CParty +0x20`): `+0x1c` manager, `+0x20`
material, `+0x24` proto, `+0x28` the item, `+0x2c` spell to embed, `+0x30` count, `+8` bit 3 while building [V]. The protocol
every creator follows: `SetManager(mgr)`, `BeginCreateItem(kind, id)` (0x6A5C50: `mgr->vt[23] CreateObject(kind, id)`, count =
1), `SetProto` / `SetMaterial` / `SetSpell`, `FinishItem` 0x6A5CF0 (`ApplyProto`; on failure the item is released and 0
returned), `SetManager(0)`.

`ApplyProto` 0x6A5D60 [V]: fails for a weapon / armor without a material. Builds a temporary record T of the proto's size, with
P = proto and M = material:

| T field | with a material | without |
|---|---|---|
| Price +0x34 | `P.Price + P.Components * M.Price` | P.Price |
| Weight +0x38 | `P.Weight * M.Weight` | |
| Size +0x3c | P.Size | |
| Mana +0x40 | `P.Mana + M.Mana` | |
| Slots +0x44 | `P.Slots + M.Slots` (int) | |
| Durability +0x48 | `P.Durability * M.Durability` | |
| Components +0x4c | P.Components | |
| weapon Range +0x8c | P.Range | |
| weapon D.Min / D.Max +0x90 / +0x94 | `P * M.Damage` (both) | |
| armor flags +0x8c / +0xb0 | `P \| M.flags(+0x48)` | |
| armor main[k] +0x70 | `P.main[k] * M.Absorb[k] * P.MAbsorb(+0x6c)` | |
| armor add[k] +0x94 | `P.add[k] * M.Absorb[k] * P.AAbsorb(+0x90)` | |
| quick +0x70, quest +0x6c, loot +0x6c/+0x70 | copied; loot category 1 with a material: `+0x74 = M.ID` | |

then `item->vt[13] InitFromProto(T)`; a quick proto with ItemID 8 (spell container) gets the spell named in P `+0x7c`
(`CSpellManager::CreateSpellByName`) attached **without** a price (`SetSpellNoPrice` 0x6A0520), any other item gets the
constructor's `+0x2c` spell through `CItem::SetSpell` 0x6A04D0 (price `+0x38 += spell +0x48`); `item +0x18 = row << 16 |
M.ID`, `+0x48 = count`. `CItem::InitFromProto` 0x6A0490: `+0x20 = +0x24 = T.Durability`, `+0x28 = +0x2c = T.Mana`, `+0x30 =
0.001`, `+0x34 = T.Weight`, `+0x38 = T.Price`, `+0x3c = T.Size`, `+0x40 = T.Slots`; the weapon adds `+0x58 = Range`, `+0x50 =
D.Min`, `+0x54 = D.Max` (spread); the armor copies the two 7-float sets; quick / spell `+0x4c = GD Level`; quest `+0x4c =
ScriptID`; loot `+0x4c..+0x54`.

Checked on `Universal-Mod/mp/1.mp` (decoded offline): a weapon row 42 / material 16 has durability 71400, mana 66, weight 6,
price 2616, size 900, slots 18 = exactly these products; the battle axe with an attached spell shows price 6404500 = base +
spell price (the `SetSpell` addition).

`CItemManager::CreateItemByName` 0x6AC970 [V]: `"name.material[x]"` -> the `[x]` text is split off, the rest split on `.` and
upper-cased; `spell container` -> `CreateSpellContainer(spell name = x)`; else the first part is looked up in the weapon, armor,
quick, loot and quest maps in that order (`FatalError "Can't find prototype for item"`), the world's constructor runs
`BeginCreateItem(proto->GetItemKind(), -1)`, `SetProto`, `SetMaterial(second part)`, and `x` is the **count** (`atol`) when the
proto is the loot row named `material`, otherwise a **spell name** to embed. For a category-2 loot item (blueprint) the second
part names what it makes and its row goes to `+0x54` (weapon / armor / quick / spell / perk maps). `CItemManager::CreateItem`
0x6AD000 (kind, proto row, material row, id) picks the record by kind (quick for 0x3008 too) and the material for weapons /
armors only. `CreateSpellContainer` 0x6AD1E0 = `CreateItem(0x3008, row of "spell container", 0, -1)` + `SetSpell`.

Client-side shortcuts that bypass the constructor (the HUD): `ItemId_GetDamageMin` 0x6A2A50 = `D.Min * M.Damage`,
`ItemId_GetDamageMax` 0x6A2AE0 = `(D.Min + D.Max) * M.Damage`, `ItemId_GetWeight` 0x6A2BD0 = `Weight * M.Weight`,
`ItemId_GetWeaponType` 0x6A2B80, `CItemWeapon::GetMainDamageType` 0x6A2C60 (first nonzero proportion), all from the packed id
(`CUnitClientSpecific::GetAttackParams` 0x538960); the server reads the item fields instead (`CItemWeapon::RollDamage`
0x6A2DE0 = `+0x50 + ISAAC * +0x54`).

**Deconstruction / construction (camp mode 5)** [V]: `CCampItemConstr::Deconstruct` 0x6129C0 clones the single item of the
component party (`+0x1c0`), removes it and adds to the party an "Instruction Weapon / Armor / Quick Item" loot item with `+0x54
= item +0x18 >> 16`, `Components` x `material.<name of material (+0x18 & 0xffff)>` and, when the item had a spell, a "spell
container" with a copy of it. `TryConstruct` 0x612600 scans the component party for one blueprint (category 2, table 0/1/2 ->
kind 0x3004/5/6, proto by `+0x54`), N materials of one material and an optional spell container; it builds the item only when
`N == proto Components` and `material Type == proto M.Type` (case-insensitive). The fees are in docs/game-memory.md.

## 4. The item classes

Vtable (22 slots; CObject slots 0..12 as in hierarchy.md: 0 dtor, 2 Init, 3 Done, 4 Release, 9 Read, 10 Write, 11 ClassName,
12 GetTypeId) [V]:

| slot | CItem | overrides |
|---|---|---|
| 6 | `RegenMana` 0x6A0070: `+0x28 += +0x30 * +0x2c`, clamped (regen <= 0 -> full); returns 1 when changed | all share it |
| 9 / 10 | `Read` 0x6A0270 / `Write` 0x6A0360: the 48-byte block (section 5) | weapon + 4 floats, armor + 14 floats, quick / spell / quest + `+0x4c`, loot + 3 ints |
| 13 | `InitFromProto` 0x6A0490 | per kind (section 3) |
| 14 | `Equals` 0x69FE90: kind, id, `+0x20..+0x40` (not the count) and the same spell | + the kind fields |
| 15 | `CloneInto(dst, spellMgr)` 0x69FFC0: same kind required; copies `+0x18`, `+0x20..+0x40`, `dst +0x44 = CSpell::FillFromId(spell)` (a spell clone), `dst +0x48 = 1` | + the kind fields |
| 16 | `GetPrice` 0x4622F0: `+0x38` | `CItemLoot::GetPrice` 0x6A5470: the referenced record's price (material `+0x10`, proto `+0x34`, spell proto `+0xc`, perk `+8`) |
| 17 | `GetModelName` "" | weapon 0x6A0840 / armor 0x6A3240: `"%s_%.2d.%s.%d"` (type prefix, row, material ...) |
| 18 | `GetProtoText0` "" | weapon / armor: proto CStringArray `+0x20[0]` |
| 19 | `GetIconName` "" | weapon / armor: `"unhuma" + model name`; quick `"qitem%04d[.%s]"`; quest `"quitem%04d"`; loot `"material%04d"` / `"%s_%.2d.%d"` / `"qitem%04d"` / `"prototype%04d"` / `"modifier.."` / `"litem%04d"`; spell `"spell%04d"` |
| 20 | `GetInitIconName` "" | `"initwe%s%dweapon"`, `"initar%s%darmor"`, `"initqi%ditem"`, `"initqu%ditem"`, `"initlim%ditem"`, spell `"initqi1item"` |
| 21 | `GetStackInfo` zeros | quest: `{8, script id, 0}` |

`CItem` layout (0x4c) [V]:

| off | field | notes |
|---|---|---|
| +4 / +8 / +0xc / +0x14 | refcount, flags, object id, manager | CObject |
| +0x10 | kind | 0x3004.. |
| +0x18 | packedId | `row << 16 \| material ID` |
| +0x1c | partyLink | `PartyItemLink` (0xc: vt 0x73FA88, 16-bit refcount, `+8` party record item id) on unit items that came from a hero record; 0 otherwise |
| +0x20 / +0x24 | durability / max | worn by `Placement_Damage`; `< 1` = broken (`DropBrokenItems` 0x532AC0, `SelectWeapon` refuses it) |
| +0x28 / +0x2c | mana / max | the charge of magic items and of embedded spells (`CanAffordSpell`: `+0x28 >= spell cost +0x40`); `ConsumeItemCharge` 0x552C60 subtracts and puts the item on the server's update list `+0x12e44` so `RegenMana` refills it |
| +0x30 | manaRegen | 0.001 of max per update |
| +0x34 | weight | summed by `CUnit::GetCarriedWeight` 0x5307B0 (UnitStats `+0x50`) and `HeroRecord::TotalWeight` |
| +0x38 | price | |
| +0x3c | size | |
| +0x40 | slots | complexity cap of the embedded spell (the card's "Complexity: used/max") |
| +0x44 | spell | `CSpell*`: enchantment / wand spell / the contained spell; `CUnit::GetItemSpell` reads it for the spell slots |
| +0x48 | count | stack (party inventory, materials, blueprints) or charges (wands / scrolls: `Unit_ConsumeItemCharge` 0x595020 decrements it and the party twin's) |

Kind fields: `CItemWeapon` `+0x4c` ? (saved, never set by InitFromProto), `+0x50` damage min, `+0x54` spread, `+0x58` range;
`CItemArmor` `+0x4c[7]` main, `+0x68[7]` additional, `+0x84/+0x88` 0; `CItemQuick` / `CItemSpell` `+0x4c` GD level;
`CItemQuest` `+0x4c` script id; `CItemLoot` `+0x4c` category, `+0x50` table, `+0x54` row.

## 5. Persistence: the detail block, the hero record, the party file

`CItem::Write` 0x6A0360 writes, after the 4-byte object id the manager writes: `+0x18, +0x20, +0x24, +0x28, +0x2c, +0x30, +0x34,
+0x38, +0x3c, spell id (+0x44 -> +0xc, or -1), +0x48, +0x40` = 48 bytes; so in the block: +4 id/material, +8 durability, +12
max, +16 mana, +20 max, +24 0.001, +28 weight, +32 price, +36 size, +40 spell id, **+44 count**, +48 slots [V, checked on
1.mp]. Then: weapon +52 `+0x4c`, +56 min, +60 spread, +64 range (68 bytes); armor 14 floats (108); quick / spell container /
quest one int (56); loot category, table, row (64: the "row it makes" of a blueprint is +60). `CItem::Read` resolves the spell
id with `CBaseManager::FindById` on the world's spell manager, so the spells must be read first.

`HeroRecord` (0x784) [V: ctor 0x65F5F0, `Read` 0x65F9B0, `Write` 0x65FDE0]: CStrings `+0` display name, `+4` model (unhuma..),
`+8` class ("Hero", udb `+0xe8`), `+0xc`, `+0x10` skin, `+0x14` proto name, `+0x18` copy; `+0x1c` id = `String_HashCI(model)`
(1e9..2e9, the hero unit id: `IsHeroId`); `+0x20` UnitStats 0x704; `+0x724` complection[3]; `+0x730` weapons, `+0x740` quick
items, `+0x750` armor, `+0x760` spell containers (aligned pointer arrays {data, count, max, grow}); `+0x770` stance (2),
`+0x774` weapon slot in hand, `+0x778` byte, `+0x77c` follow target id (-1), `+0x780` battle button (1). File order = the ctor
order: 5 strings (+0, +8, +0xc, +0x10, +0x14), id, 0x704 raw stats, 12 bytes, 4 x (count + object ids), string +4, then the
version-gated tail (`+0x770`, `+0x774`, byte, `+0x77c`, `+0x780`, string `+0x18` for stream versions > 0x50 / 0x5a / 0x60 /
0x6a). `HeroRecord::Add*` (0x6608D0 weapons, 0x660AB0 quick, 0x660C90 armor, 0x660E70 spells) clone the item through
`CItem::Clone` 0x69FF70 (the item's manager `vt[23]` + `CloneInto`) before storing it.

`CParty` (0x70, typeId 9, vtable 0x743000 = the CWorld container slots 13..26: 16 item constructor `+0x20`, 17 spell constructor
`+0x28`, 18 item manager `+0x1c`, 22 spell manager `+0x24`, 24 `CloneItem` shared with CWorldClient) [V]: `+0x2c` hero CArray
(`+0x30` records, `+0x34` count), `+0x40` items (`+0x44` count), `+0x50` money, `+0x54` variables (string -> float), `+0x68`
zone name, `+0x6c` u32. `Init` 0x6614F0 creates the four managers; `Clear` 0x661700 empties everything; `AddItem` 0x661DB0
(item, keepCount, merge): count = `item +0x48` or 1, merges into an `Equals` match or clones (`vt[24]`); `RemoveItem` 0x661F30
(item, all) decrements the stack or releases. `CParty::Read` 0x6618C0 / `Write` 0x661C10 are the `.mp` body of
`docs/file-formats/mp-format.md`: zone string, u32, item manager + spell manager headers (`{id, kind, u16 material, u16 row}`),
their detail blocks (`CBaseManager::ReadObjects`, each list closed by 0xFFFFFADF 00), backpack count + ids, member count +
`HeroRecord` each, variables when asked, money as `a ^ b` (b random on every write; the XOR only for stream versions > 0x6b).
`PartySlot::SaveToFile` 0x662AC0 writes it through `temp.mp` + `MoveFileA`; `CParty::LoadMp` 0x662DD0 reads a file;
`PartyStore::LoadAll` 0x662FB0 lists `dir\*.mp` and keeps the 1-hero parties as 0x74 slots (`CParty` + file name). The campaign
save embeds the same stream (`CParty::ArchiveMp` 0x6617F0), `CParty::Assign` / `CopyCtor` deep-copy through it.

Hero creation: `CParty::AddHero` 0x6628D0 (model, proto, display name) -> `InitHeroFromProto` 0x6621F0: stats from the proto,
`Units_GetProtoEquipment` 0x5143E0 (armor list `+0x10c` -> `+0x750`, weapon `+0x118` / `+0x11c` -> `+0x730`, spells `+0x100` ->
`+0x760`); `ApplyHeroKit` 0x6625B0 replaces weapons and spells from the units.udb NPC record (`HeroKit` 0x60: `+0x30` weapons,
`+0x3c` quick items, `+0x48` spells, `+0x54` experience, `+0x58` money).

Unit <-> record: `CWorldServer::StorePartyFromUnits` 0x590C70 (unit weapon slots -> `+0x730`, armor -> `+0x750`, quick (count
kept) -> `+0x740`, spell slots -> `+0x760`, a `PartyItemLink` on each unit item, stats copied out); `CWorldClient::CreateHeroUnits`
0x566F70 (`+0x730` -> `AddWeaponSlotItem` + `SelectWeapon(0)`, `+0x750` -> `EquipArmor`, `+0x740` -> `AddQuickSlotItem`; the
spell slots are not copied: a client hero reads them from the record); `CWorldServer::DetachHeroes` 0x58FD90 stores `+0x770
/ +0x774 / +0x780` from the unit. **On the client** `CUnit::GetWeaponSlotItem` 0x5318B0, `GetQuickSlotItem` 0x531920,
`GetSpellSlotItem` 0x5319A0 and `GetItemSpell` 0x531A10 return the party record's arrays when the unit's specific typeId is
0x3d (client hero): the record is the source of truth there [V].

Net: `CPlayerServer::SendItems` 0x66E7D0 (slot 17, from `CPlayer::AddItems` 0x665470) sends opcode 5 + a blob of the spells
then the items (`ObjList_WriteStream`), then opcode 6 with money (u32), a byte and the item ids ending in -1;
`CPlayerClient::OnMsg_AddItems` 0x669490 resolves them in the client item manager `g_0x7AF60C` (filled by
`CWorldClient::ReadSpellsAndItems` 0x567A60 from opcode 5); opcode 0x80 / 0x83 (server) and 0x50 (client) carry a whole
party through `CParty::Write`; units replicate their slot ids in net section 5 (`CUnit::WriteNetState`, combat.md 8) and
`CUnitClientSpecific::SetEquipment` / `SetWeapons` rebuild the figures from ids.

## 6. Inventory and equipment on the unit

`CUnit`: `+0x440[4]` weapon slots, `+0x450[4]` quick slots, `+0x460[8]` spell slots, `+0x480[7]` armor by `GetArmorType`,
`+0x4a0[]` extra items (`+0x4a4` count: quest items given to the unit; `PopRandomQuickItem` 0x531400 pops them first);
`+0x26c` weapon slot in hand (`+0x270` wanted). `AddWeaponSlotItem` 0x531070 / `AddQuickSlotItem` 0x531180 /
`AddSpellSlotItem` 0x531820 take the first free slot (1 when present, -1 when full), `ClearWeaponSlot` / `ClearQuickSlot`
release one; `EquipArmor` 0x530730 (durability >= 1 only) replaces `+0x480[type]`, tells the specific (`vt[26]` Equip: figure +
textures on the client, `UnitStats_AddArmorModifiers` preview on the server) and marks net section 5; `UnequipArmor`
0x530840 (`vt[27]`, sections 5 + 4); `DropAllItems` 0x5315B0 clears everything. `CUnitServer::SelectWeapon` 0x553610 (slot 25;
the client asks through `CPlayerServer::OnMsg_UnitByte` -> `RequestWeapon` 0x5552E0 which sets `+0x270`, `TickState` switches) accepts a slot whose item has durability,
sets `+0x26c = +0x270`, rebuilds the AI spell infos for player units and `DropOtherWeapons` 0x5528F0 (only the weapon in hand
stays attached; the passive spell list `+0x920` is rebuilt from the items whose spell has flag 0x20000). Carried weight =
sum of `+0x34` over the 23 slots (`GetCarriedWeight`, capped by `str * 12 * (1 + lift)` in UnitStats). Two-handed / shield
rules: none in this code; the only exclusivity is one armor per type [V]. Quick slots are also the belt: `CUnit::QuickItemUsability`
0x52F8B0 (3 not a magic quick item (subtype 5 / 8), 1 subtype 5 without mana >= spell cost, 0 usable); spell slots:
`SpellUsability` 0x52F820 (3 empty, 4 silenced, 1 mana `+0x182 < cost * 25/int`).

Durability wear [V]: armor through `Unit_DamageArmorSlots` 0x525720 (heroes only: the items covering the hit location at the
tier, tables 0x73E09C / 0x73E0A0 / 0x73E0A4, `Placement_Damage` with the messages 0x23 damaged / 0x25 broken); weapons by
the damage absorbed (combat.md 6); `DropBrokenItems` 0x532AC0 moves `< 1` items of the hero into the player's loose list
(`CPlayer::AddItems`), shifting the weapon slots down and re-selecting (-1). Repair = the camp mode 8 price (`GetItemPrice`)
and `+0x20 = +0x24`.

Loot [V]: `CWorldServer::LootUnit` 0x5942D0 (unit, player) on a corpse (or `LootAllDeadUnits` 0x593D60 for the host):
`CHoover::CollectUnitLoot` 0x461CB0 creates the prototype's drop list (`UnitProto +0xe8` names, `CreateItemByName`), rolls the
money (`rand(+0xd0..+0xd4)` with chance `+0xc8` %, plus `rand(+0xe0..+0xe4)` with chance `+0xd8` %), takes the quick slots and
the `+0x4a0` extras of every unit and, for hero ids, the armor and weapon slots too, and sets flag 0x100000; in a lobby game the
host player gets the quest items (typeId 0x14) first; `player +0xc0 += money`, `CPlayer::AddItems(player, items, 0)`; a unit
already flagged 0x20000000 gives its stored loot record (`+0x12e64` list: unit id, lobby index, money, item ids). The pick-up
order itself is `QueueOrderInteract*` (combat.md 3) -> `ExecuteInteraction` 0x54F000 -> `WorldServer::OnLootOrder` 0x670310;
items never lie on the ground as objects: they go straight to `CPlayer +0xe4`, and `GivePartyItemsToPlayer` 0x590460 /
`MoveTrackedItemsToParty` 0x590160 move them between that list and the party backpack at zone changes.

The camp [V]: `CCampScreen::PutOnItem` 0x60FCA0 moves a party item into the hero record (weapon max 4, quick max 4, spells max
8, armor replaces the same type and `EquipArmor`s the live unit) and `CParty::RemoveItem`s it; `UnequipItem` 0x60FE40 the
reverse (`CParty::AddItem(.., 0, 1)` merges stacks); `CanDropItem` 0x6100E0 is the per-mode acceptance rule (mode 0: weapons /
armor / quick, broken ones not from the party; 2 / 3 spell items and blueprints; 4 trade: no quest items / spells; 5 constructor:
one blueprint or usable item, materials of one kind). `ApplyWeaponSlots` 0x60F9E0 / `CCampScreen2::StoreWeaponSlots` write
`+0x730` back into the unit. The shop stock: section 7.

## 7. The shops (CGSPlayer, ShopStock)

Each `CGSPlayer` (the five multiplayer "GS" players) owns two parties used as stock (`+0x3c` item shop, `+0xac` spell shop) and
two `ShopStock` objects (0xb8, at `+0x11c` / `+0x1d4`; named "GSPlayerLayer" earlier) [V]. `InitAllowedLists` 0x4A7080 fills
them from the databases: weapon / armor / quick protos whose Shop bits `+0x64` have bit `(player index + 8 - 1)`, materials
(bits `+0x4c`) with the range 20..50, spell protos (mask `+0x80`) and perks (mask `+0x20`) into both. `SetShopLimits` 0x4A6DC0
stores two float[8] (per-school complexity caps). The stock is regenerated by `GenerateItemShop` 0x4A6DF0 (when `+0xc`):
`RollMaterialStock` (stock = rand(min..max) per material), `GenerateWeapons` / `GenerateArmors` / `GenerateQuickItems` 40..50
each, 1..3 blueprints per allowed proto (`AddWeaponInstructions` & co.), `AddMaterials` 20..50; `GenerateSpellShop`
0x4A6F60: 40..50 spell containers (`AddSpellContainers`), 1..3 spell blueprints ("prototype"), 20..50 runes.

`ShopStock::GenerateItems` 0x69AE20 (proto vector, min, max, out, kind) [V]: `SelectSpellFilters` with S_Weapons (kind 0 / 3)
or S_Armors (kind 1); n = rand(min..max) protos picked by a partial shuffle (protos with Type "" skipped); the material: among
the stock entries whose material Type equals the proto's M.Type and whose stock >= proto Components, the first one accepted by
a price test (material price p vs proto price P: chance `|(p - P/7) / (P/7 - P/5)| * 100` % when `2P/7 - P/5 <= p <= P/5`,
else 10 %), otherwise a random one; stock -= Components; no candidate -> no item. With 50 % probability an embedded spell:
`GetSlotsAndMana` 0x6A6480 -> `GenerateSpells` with the item's slots as every school's limit -> a random one whose mana cost
`+0x40 <= item mana`. Wands (quick subtype 8) get 20 + rand%30 charges. `GenerateSpells` 0x69A780 builds, per filtered spell
proto, a perk list (mandatory runes `+0xc`, optional `+0x18` at 50 %, repeats of `+0x24` while the summed complexity stays
under the school's limit and the count < 8) and `CSpellManager::CreateSpellWithPerks` (spell side: another document).

## 8. What is not known

* `CItemWeapon +0x4c`: saved and compared, never set by `InitFromProto` (stays -1 from the ctor; 0 in 1.mp) [?]. `ItemMaterial
  +0x24` Skill and the proto bytes `+0x78`, `+0x7d..`, `+0xbc..`, `+0x83..`: no reader found in this round.
* The runtime `CStringArray +0x20 / +0x50` of every proto (slot 18 `GetProtoText0`): who fills them (probably
  `CTextManager::GetItemTextKeys` or the camp screens) was not traced.
* `ShopStock +8` vs `+0x14` (armor / weapon protos) rests on which name map `StrHashTable_LookupId` uses (the weapon map by
  the helper it shares with `ItemsDb::Load`) [I]; the old names `AddUnique_Weapon` (+8) / `AddUnique_Armor` (+0x14) said the
  opposite. The S_* table assignment (S_Armors 0x7C855C for kind 1, S_Weapons 0x7C8568 for kinds 0 / 3) follows from it.
* `CInventory` (typeId 0x17): no creator found; its hash set is never filled.
* The exact `"%s_%.2d.%s.%d"` arguments of the model names (which string is the material part) and `Weapon_GetAttachBones`
  were not re-read (they are named in functions.tsv from the 2026-10-18 pass).
* `CWorldServer::CreateItem`'s `desc[10] / desc[11]` (mana / max of the source) and the update list `+0x12e44`: who ticks it
  (`TickActiveObjects` 0x58D670 is the likely consumer) [?].
* UnitProto `+0x50` as "natural skill" vs the 7-float read in `GetAttackParams` (combat.md open point) is unchanged.

## 9. Corrections to earlier notes

1. `full/structs.tsv` `CItem`: `+0x20/+0x24` are durability cur / max, `+0x28/+0x2c` mana (energy) cur / max, `+0x30` mana
   regen, `+0x34` weight, `+0x38` price, `+0x3c` size, `+0x40` slots, `+0x44` spell, `+0x48` count, `+0x1c` the party link (not a
   prototype ref). `CItem::Update` regenerates mana, not durability. `CItem::GetFloat38` (slot 16) returns the price, not the
   weight; `SetSubItem44` moves the spell's price, not weight.
2. `full/structs.tsv` `WeaponProto +0x50 damageMin / +0x54 damageSpread`: those are `CItemWeapon` fields; the proto's `+0x50`
   is a runtime CStringArray. The proto damage is `+0x90 / +0x94`. `docs/game-exe/hierarchy.md` "+0x20/+0x24 count, +0x28/+0x2c
   durability": same correction.
3. `docs/game-exe/item-tables.md` / um-engine `item_proto.cpp`: `ItemId_GetPrice` 0x6A2A50 and `ItemId_GetPrice2` 0x6A2AE0
   compute the damage min / max (`D.Min * material Damage`, `(D.Min + D.Max) * material Damage`), not prices; `CItemWeapon::GetSkill`
   0x6A2C60 is the first nonzero damage-proportion index. (The replacement is still right, the names are not.)
4. `docs/file-formats/mp-format.md`: the quantity of a loot item is the u32 at **+44** of its block (every kind: it is `CItem
   +0x48`), **+48** is the slots / complexity max; the lists of a member are weapons, belt (quick items), worn armour, spells
   (confirmed: `+0x730 / +0x740 / +0x750 / +0x760`). The game-side names in functions.tsv were swapped: `HeroRecord::AddInventoryItem`
   (+0x730) is `AddWeapon`, `AddWeapon` (+0x740) is `AddQuickItem`, `AddQuickItem` (+0x760) is `AddSpellItem`; `CUnit::GetWeaponSlotItem`
   0x531920 reads the **quick** slots and `CUnit::GetItemById` 0x5318B0 the weapon slots; `CUnit +0x460` are the spell containers.
5. `full/functions.tsv` `CItem::GetSlotClass` 0x6A31A0: types 0..2 (helmet, plate, legs) are tier **2**, 3..6 tier 1 (the old
   purpose had it reversed; combat.md 6 "at its tier" is unaffected).
6. `docs/game-memory.md` camp section: "Deconstruction = CCampItemConstr::CreateItem 0x6129C0" is right in effect; the function
   is the deconstruction (`Deconstruct`), the construction is `RefreshComponents` 0x612600 (`TryConstruct`). The blueprint's `+0x54`
   is the item's prototype row, the materials come one per proto Components: confirmed.
7. `docs/game-exe/findings.md` 0x699xxx "CItemConstructor (lists ... BuildResultItem)": that class is the shop stock
   generator (`ShopStock`, CGSPlayer +0x11c / +0x1d4), not the `CItemConstructor` of 0x6A5AB0; `CGSPlayer::CollectUnitsIntoParty`
   (0x4A6DF0 / 0x4A6F60) generate the item / spell shop stock. `CItemRegistry::AddUnique_*` are its list builders (names fixed).
8. `docs/game-exe/combat.md` 6: "Durability: items are placements too (+0x20 durability, +0x24 max, +0x48 count)" holds; "charge
   +0x28" is the item's mana. `CItemQuick::GetSubtype` works for every kind (`CItem::GetProtoSubtype`).

## 10. Function index

| addr | name | one line |
|---|---|---|
| 0x461CB0 | CHoover::CollectUnitLoot | drops + money of a corpse into lists, flags it looted |
| 0x4622F0 | CItem::GetPrice | slot 16: +0x38 |
| 0x4A6DF0 / 0x4A6F60 | CGSPlayer::GenerateItemShop / GenerateSpellShop | regenerate the two shop parties |
| 0x4A7080 | CGSPlayer::InitAllowedLists | protos / materials / spells / perks with the player's shop bit |
| 0x5318B0 / 0x531920 / 0x5319A0 | CUnit::GetWeaponSlotItem / GetQuickSlotItem / GetSpellSlotItem | slot getters (client heroes: the party record) |
| 0x531070 / 0x531180 / 0x531820 | CUnit::AddWeaponSlotItem / AddQuickSlotItem / AddSpellSlotItem | first free slot |
| 0x530730 / 0x530840 | CUnit::EquipArmor / UnequipArmor | +0x480[type], specific vt[26] / vt[27] |
| 0x5307B0 | CUnit::GetCarriedWeight | sum of +0x34 over the 23 slots |
| 0x532AC0 | CUnit::DropBrokenItems | durability < 1 -> the player's loose list |
| 0x553610 / 0x5528F0 | CUnitServer::SelectWeapon / DropOtherWeapons | weapon in hand = slot index |
| 0x552C60 / 0x595020 | CUnitServer::ConsumeItemCharge / Unit_ConsumeItemCharge | mana / count down, party twin updated |
| 0x55E940 / 0x594CE0 | CWorld::CloneItem / CWorldServer::CloneItem | slot 24: CreateItem + CloneInto |
| 0x566F70 | CWorldClient::CreateHeroUnits | hero record -> unit slots |
| 0x590C70 | CWorldServer::StorePartyFromUnits | unit slots -> hero record (+ PartyItemLink) |
| 0x5942D0 / 0x58FBF0 / 0x590460 | CWorldServer::LootUnit / GiveItemsToPlayer / GivePartyItemsToPlayer | loot and hand-over |
| 0x5EE0B0 / 0x5EE110 / 0x5EE0D0 / 0x5EE130 | Item_IsSpellContainer / IsLootCat1 / IsLootCat2Sub0to2 / IsUsable | item predicates of the camp |
| 0x60FAB0 | CCampScreen::GetItemPrice | price x mode coefficient (game-memory.md) |
| 0x60FCA0 / 0x60FE40 | CCampScreen::PutOnItem / UnequipItem | party <-> hero record |
| 0x6100E0 | CCampScreen::CanDropItem | per-mode acceptance |
| 0x6129C0 / 0x612600 | CCampItemConstr::Deconstruct / TryConstruct | blueprint + materials <-> item |
| 0x65F5F0 / 0x65F9B0 / 0x65FDE0 | HeroRecord::ctor / Read / Write | the 0x784 record and its file form |
| 0x6608D0 / 0x660AB0 / 0x660C90 / 0x660E70 | HeroRecord::AddWeapon / AddQuickItem / AddArmor / AddSpellItem | clone into +0x730 / +0x740 / +0x750 / +0x760 |
| 0x661050 / 0x6614F0 / 0x661700 | CParty::ctor / Init / Clear | layout, managers, reset |
| 0x661DB0 / 0x661F30 | CParty::AddItem / RemoveItem | backpack stacks |
| 0x6618C0 / 0x661C10 | CParty::Read / Write | the .mp body |
| 0x6621F0 / 0x6625B0 / 0x6628D0 | CParty::InitHeroFromProto / ApplyHeroKit / AddHero | hero creation |
| 0x662AC0 / 0x662DD0 / 0x662FB0 | PartySlot::SaveToFile / CParty::LoadMp / PartyStore::LoadAll | the mp folder |
| 0x665470 | CPlayer::AddItems | loose list +0xe4 + SendItems |
| 0x66E7D0 / 0x669490 | CPlayerServer::SendItems / CPlayerClient::OnMsg_AddItems | opcodes 5 + 6 |
| 0x69A480 / 0x69A780 / 0x69AE20 | ShopStock::SelectSpellFilters / GenerateSpells / GenerateItems | the stock generator |
| 0x69F620 / 0x69F700 / 0x69F800 / 0x69F8C0 / 0x69F940 / 0x69F9C0 / 0x69FA90 | ItemMaterial / ItemProtoBase / WeaponProto / ArmorProto / QuickProto / QuestProto / LootProto ::SerializeFields | items.idb tags |
| 0x69FCA0 / 0x69FE90 / 0x69FF70 / 0x69FFC0 | CItem::ctor / Equals / Clone / CloneInto | base object |
| 0x6A0070 / 0x6A0270 / 0x6A0360 / 0x6A0490 | CItem::RegenMana / Read / Write / InitFromProto | base slots 6 / 9 / 10 / 13 |
| 0x6A04D0 / 0x6A0520 | CItem::SetSpell / SetSpellNoPrice | embedded spell |
| 0x6A0110 / 0x6A4230 / 0x6A39E0 / 0x6A31A0 | CItem::GetProtoRecord / GetProtoSubtype / CItemArmor::GetArmorType / GetArmorTier | proto lookups by id |
| 0x6A0660 / 0x6A3070 / 0x6A3BE0 / 0x6A44D0 / 0x6A4AB0 | CItemWeapon / CItemArmor / CItemQuick / CItemQuest / CItemLoot ::InitFromProto | kind fields |
| 0x6A2A50 / 0x6A2AE0 / 0x6A2BD0 / 0x6A2B80 / 0x6A2C60 / 0x6A2DE0 | ItemId_GetDamageMin / GetDamageMax / GetWeight / GetWeaponType / CItemWeapon::GetMainDamageType / RollDamage | id-based readers |
| 0x6A0840 / 0x6A3240 / 0x6A2750 / 0x6A3720 / 0x6A3CD0 / 0x6A45C0 / 0x6A4B90 / 0x6A5820 | GetModelName / GetIconName of each kind | UI names |
| 0x6A5470 | CItemLoot::GetPrice | the referenced record's price |
| 0x6A5AB0 / 0x6A5C50 / 0x6A5CF0 / 0x6A5D60 / 0x6A6480 | CItemConstructor ctor / BeginCreateItem / FinishItem / ApplyProto / GetSlotsAndMana | the builder |
| 0x6A67A0 | ItemsDb::Load | items.idb |
| 0x6AC810 / 0x6AC970 / 0x6AD000 / 0x6AD1E0 | CItemManager::CreateByKind / CreateItemByName / CreateItem / CreateSpellContainer | item creation entry points |
