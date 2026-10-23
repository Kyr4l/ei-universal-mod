// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's weapon prototype lookups, safe for an item id outside the table (RE: _cpr/claude-re/full, game.exe).
//
// An item id is (prototype index << 16 | material index). The game keeps the weapon prototypes in one array of 0xB4-byte records (begin at
// 0x7C86F8, end at 0x7C86FC) and the materials in one of 0x50-byte records (0x7C86D8 / 0x7C86DC). Every lookup compares the index with the
// number of records and, when it is too high, carries on with a NULL record: it reads +0xC (the weapon kind), +0x90 (the price), +0x98 ...
// of address 0 and crashes (an access violation at a very low address, e.g. 0x6A2BBF reading 0xC).
//
// When it bites (reproduced 2026-10-22): leaving the multiplayer menu reloads the item table from the BASE game's database (database.res,
// 37 weapons) while the menu's 3D views of the characters are still alive and updated (the game never destroys them, see docs/fixes/...):
// a character wielding an item of the mod's extra rows (37 and up: "crossbow", row 45, in HALAL MAN's case) crashes the game when the menu
// closes, with or without any new model.
//
// Here: the lookups return what an empty slot gives (0 / no kind / skill 6) for an index outside the table, the first few are logged.
// Everything else is what the game computes (the price and weight in the same extended precision). Replaced functions:
//   0x6A2B80 ItemId_GetProtoType   0x6A2A50 ItemId_GetPrice   0x6A2AE0 ItemId_GetPrice2   0x6A2BD0 ItemId_GetWeight
//   0x6A2C60 CItemWeapon::GetSkill   0x6A07F0 CItemWeapon::GetDamageKind   0x526860 Weapon_SkillGroupByte (the weapon tooltip)
// and three inlined copies of the same lookup (ApplyWeaponAnimMask, Hero_BuildEquipmentEntries) are patched in um_engine.cpp.
#include <cstdio>

extern "C" void UmEngineLog(const char* format, ...);

#ifdef UM_HOST_TEST   // tests/item_proto_test.cpp builds this file on Linux with tables of its own
extern const char* g_begin;
extern const char* g_end;
extern const char* g_matBegin;
extern const char* g_matEnd;
#define UM_WEAPONS_BEGIN g_begin
#define UM_WEAPONS_END g_end
#define UM_MATERIALS_BEGIN g_matBegin
#define UM_MATERIALS_END g_matEnd
#define UM_THISCALL
#define UM_FASTCALL
#define UM_STDCALL
#else
#define UM_WEAPONS_BEGIN (*reinterpret_cast<const char* const*>(0x007C86F8))
#define UM_WEAPONS_END (*reinterpret_cast<const char* const*>(0x007C86FC))
#define UM_MATERIALS_BEGIN (*reinterpret_cast<const char* const*>(0x007C86D8))
#define UM_MATERIALS_END (*reinterpret_cast<const char* const*>(0x007C86DC))
#define UM_THISCALL __attribute__((thiscall))
#define UM_FASTCALL __attribute__((fastcall))
#define UM_STDCALL __attribute__((stdcall))
#endif

namespace {
// The record of the weapon prototype an item id names, or null when the table is empty, being rebuilt or too short (the game's own division and unsigned compare).
const char* WeaponRecord(unsigned itemId) {
    const char* begin = UM_WEAPONS_BEGIN;
    const char* end = UM_WEAPONS_END;
    const long count = (end - begin) / 0xB4;
    const unsigned index = itemId >> 16;
    return count > 0 && index < static_cast<unsigned long>(count) ? begin + static_cast<size_t>(index) * 0xB4 : nullptr;
}
const char* MaterialRecord(unsigned itemId) {
    const char* begin = UM_MATERIALS_BEGIN;
    const char* end = UM_MATERIALS_END;
    const long count = (end - begin) / 0x50;
    const unsigned index = itemId & 0xFFFF;
    return count > 0 && index < static_cast<unsigned long>(count) ? begin + static_cast<size_t>(index) * 0x50 : nullptr;
}
void Report(const char* what, unsigned itemId) {
    static int reported = 0;
    if (reported >= 8) return;
    ++reported;
    UmEngineLog("um-engine: %s: item id 0x%08X is outside the item tables (weapon %u of %ld, material %u): the answer of an empty slot is returned instead of crashing the game",
                what, itemId, itemId >> 16, static_cast<long>((UM_WEAPONS_END - UM_WEAPONS_BEGIN) / 0xB4), itemId & 0xFFFF);
}
inline float Float(const char* record, int offset) { return *reinterpret_cast<const float*>(record + offset); }
}

// 0x6A2B80, fastcall / thiscall with the id in ECX: the weapon kind (the record's +0xC)
extern "C" int UM_THISCALL UmItemProtoType(unsigned itemId) {
    const char* record = WeaponRecord(itemId);
    if (!record) { Report("ItemId_GetProtoType", itemId); return 0; }
    return *reinterpret_cast<const int*>(record + 0xC);
}

// 0x6A2A50 / 0x6A2AE0 / 0x6A2BD0: price, price with the upgrade, weight: prototype value times the material's, in extended precision (the result in ST0)
extern "C" long double UM_FASTCALL UmItemPrice(unsigned itemId) {
    const char* p = WeaponRecord(itemId);
    const char* m = MaterialRecord(itemId);
    if (!p || !m) { Report("ItemId_GetPrice", itemId); return 0; }
    return static_cast<long double>(Float(p, 0x90)) * static_cast<long double>(Float(m, 0x28));
}
extern "C" long double UM_FASTCALL UmItemPrice2(unsigned itemId) {
    const char* p = WeaponRecord(itemId);
    const char* m = MaterialRecord(itemId);
    if (!p || !m) { Report("ItemId_GetPrice2", itemId); return 0; }
    return (static_cast<long double>(Float(p, 0x94)) + static_cast<long double>(Float(p, 0x90))) * static_cast<long double>(Float(m, 0x28));
}
extern "C" long double UM_FASTCALL UmItemWeight(unsigned itemId) {
    const char* p = WeaponRecord(itemId);
    const char* m = MaterialRecord(itemId);
    if (!p || !m) { Report("ItemId_GetWeight", itemId); return 0; }
    return static_cast<long double>(Float(p, 0x38)) * static_cast<long double>(Float(m, 0x14));
}

// 0x6A2C60: the skill of a weapon = the first of the 7 floats at +0x98 that is not zero, else 6
extern "C" unsigned UM_FASTCALL UmWeaponSkill(unsigned itemId) {
    const char* p = WeaponRecord(itemId);
    if (!p) { Report("CItemWeapon::GetSkill", itemId); return 6; }
    for (unsigned i = 0; i < 7; ++i)
        if (Float(p, 0x98 + 4 * static_cast<int>(i)) != 0.0f) return i;
    return 6;
}

// 0x6A07F0 (ECX = the weapon item, its id at +0x18): the attack kind
extern "C" int UM_FASTCALL UmWeaponDamageKind(const char* weapon) {
    const unsigned itemId = *reinterpret_cast<const unsigned*>(weapon + 0x18);
    const char* record = WeaponRecord(itemId);
    if (!record) { Report("CItemWeapon::GetDamageKind", itemId); return 0; }
    return *reinterpret_cast<const int*>(record + 0xC);
}

// 0x526860 (stdcall, the weapon id on the stack, the result in ST0): the byte of the prototype that rates the weapon's skill group (the weapon tooltip
// reads it). The game reads the record's kind before it tests the record for null, so an id outside the table crashes it there; a kind it has no
// group for (7 and 9 up) makes it read from the record + 0x8000007A. Both give 0 here.
extern "C" long double UM_STDCALL UmWeaponSkillGroupByte(unsigned itemId) {
    const char* p = WeaponRecord(itemId);
    if (!p) { Report("Weapon_SkillGroupByte", itemId); return 0; }
    const int kind = *reinterpret_cast<const int*>(p + 0xC);
    int group;
    if (kind < 5 || kind == 8) group = 0;
    else if (kind == 5 || kind == 6) group = 1;
    else return 0;
    return static_cast<long double>(static_cast<unsigned char>(p[0x7B + group]));
}
