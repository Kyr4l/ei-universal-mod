# The item tables and what happens to an id outside them (game.exe, audit 2026-10-23)

An item id is `prototype index << 16 | material index`. Weapons: one array of 0xB4-byte records (begin 0x7C86F8, end 0x7C86FC), materials: 0x50-byte records
(0x7C86D8 / 0x7C86DC). Every lookup compares the index with the record count and, when it is too high, carries on with a NULL record.
`Game_SessionTick` swaps the tables between database.res (campaign / main menu: 37 weapons) and databaseLMP.res (multiplayer: the mod's 64) while the menu's
character views stay alive (they are never destroyed), so a character holding a mod row (37 and up) is looked up in the short table.
The 52 functions that reference the tables (ghidra_funcs.json data refs, decompiled with tools/gh.sh dec) fall into:

**Replaced by um-engine (`item_proto.cpp`, keys in um-engine.cfg, host test tests/item_proto_test.cpp)**: ItemId_GetProtoType 0x6A2B80, ItemId_GetPrice 0x6A2A50,
ItemId_GetPrice2 0x6A2AE0, ItemId_GetWeight 0x6A2BD0, CItemWeapon::GetSkill 0x6A2C60, CItemWeapon::GetDamageKind 0x6A07F0, Weapon_SkillGroupByte 0x526860
(the weapon tooltip: it reads the kind before testing the record; checked in the rig: `CALL 0x526860 cc=std 0x2D0000` crashes the game without the key, answers 0 with it).

**Patched inline (`ItemTable::InlineLookups`)**: CUnitClientSpecific::ApplyWeaponAnimMask 0x5354FB, Hero_BuildEquipmentEntries 0x56849C / 0x568529 (the compiler inlined the lookup).

**Safe in the game's own code (the record is tested before use)**: CUnitClientSpecific::BuildEquipmentList, CUnitClientSpecific::GetAttackParams (UnitRecord_SkillValue tests its record),
CItemWeapon::GetProtoEntry / GetNameText / GetDescText, CItem::GetProtoFloats98, Weapon_FormatModelName, Weapon_GetBaseModelName, Weapon_GetAttachBones, ItemId_GetProto / GetProto2.

**Count or iterate the tables only (no index from an item)**: Databases_LoadAll / Reload, ItemsDb_ListWeaponNames / ListMaterialNames, CItemConstructor::AddWeaponInstructions / LoadState / ApplyProto,
CItemManager::CreateItemByName, CGSPlayer::InitAllowedLists, CCampTradeWin::Draw, CCampItemConstr::RefreshComponents.

**Not guarded, inventory / shop / tooltip screens only (they run on an item the player opened: not reached while the menu's character views are alive)**: after an
out-of-range index they carry a NULL record on and no null test follows in the decompile (not proven to crash in each case): CItemWeapon::GetText20 0x6A27E0 (no caller:
a virtual slot), CItemLoot::GetIconName / GetInitIconName 0x6A4B90 / 0x6A5030, CCampScreen::CanDropItem / OnBuyOrRepair, CCampItemConstr::CreateItem / OnOpen,
ArmorName_Format (the material), UIItemInfoWin::Refresh, CItemManager::CreateItem. If one of them shows up in a crash dump, the fix is the same as above (a replaced function or a patched inline
lookup); um.log (lines starting "um-engine:") names every out-of-range id the replaced functions meet (the first 8).
