# .mp: multiplayer characters (`<game>/mp/N.mp`)

Each multiplayer character is one file. A mod keeps its own folder, e.g. `Universal-Mod/mp`. um-multitool lists
and edits them in **File Processing → MP** (`tools/um-multitool/mp_file.hpp`, `mp_editor.cpp`; also
`um-multitool gui --mp <folder>`).

It was reverse-engineered from game.exe (`EIStarter OBT-1/Engine`):
- **loading:** 0x662DD0, then the reader 0x579830 and the decompressor 0x430960;
- **saving:** 0x662AC0, then the writer 0x661C10, which calls 0x65FDE0 per party member and 0x528800 for the
  unit parameters.

## File

```
u32 0x114          (same in every file; meaning unknown)
u32 version        0x74
u32 checksum       of the decompressed data: u = 0; for each u32 w: x = u ^ w; u = x * ((x & 0xFC) | 3)
u8  1
u32 size           of the decompressed data
compressed data
```

The compression is LZ-style:
- **Bits:** one byte stream holds both the bits (LSB first, one byte at a time) and whole literal bytes. Bit 0 means
  a literal byte; bit 1 means a match.
- **Matches:** a length (2..226) and an offset (0..5473) follow, each as a prefix code, copying from a 1 KB window
  (`pos & 0x3FF`, from `pos - offset - 1`).
- **Writing:** a file made only of literals (flag bytes of 0 bits) is valid. The game loads it and compresses it
  again when it saves the character.

## Data

```
zone\0                        the map the character was saved in (bz1mpg, a quest's zone ...)
u32
items:  u32 n, n x {u32 object id, u32 kind, u16 a, u16 b}
spells: u32 n, n x {u32 object id, u32 kind, u16 a, u16 b}
the items' detail blocks, then u32 0xFFFFFADF, u8 0
the spells' detail blocks, then u32 0xFFFFFADF, u8 0
backpack: u32 n, n x u32 object id (-1: empty)
u32 members (1), per member:
    5 strings: name, figure (unhuma, unhufe ...), "", "", unit name
    u32
    0x704 bytes: the unit parameters, copied from memory (pointers inside are meaningless)
    3 floats: complection
    4 lists of object ids (u32 n, n x u32): weapons, belt, the worn armour, spells
    string ("Hero"), u32, u32, u8, u32, u32, string (prototype name)
quest variables: {name\0, f32 value} ..., then "\0"
money: u32 a, u32 b, amount = a ^ b (a changes on every save)
```

**Kinds and the database:**

| Kind | Object | Database sheet (b = row) | a |
|---|---|---|---|
| 0x3002 | spell | SpellPrototypes | |
| 0x3004 | weapon | Weapons | material (Materials row) |
| 0x3005 | armour | Armors | material |
| 0x3006 | quick item | QuickItems | |
| 0x3007 | loot item (materials, toad legs...) | LootItems | material, for the "material" row (material.granite) |
| 0x3009 | quest item (keys...) | QuestItems | |
| 0x3008 | spell container | QuickItems | the spell's object id at +40 of its block |

**Detail block sizes:** 0x3002 66, 0x3004 68, 0x3005 108, 0x3006 56, 0x3007 64, 0x3008 56, 0x3009 56. A loot item's block
holds its quantity at +48 (the number in the inventory slot's corner) and a number at +52 (1 for materials, 0 for a
toad leg; the script function HaveItem compares it for quest items). In the game's memory the item object is the
block shifted by 0x18 (+0x48 quantity, +0x4C that number). Each block starts with
`{u32 id, u16 a, u16 b}`. For weapons and armour, the next words are:

| Word | Meaning |
|---|---|
| +8 | durability (float; worn items: below the max) |
| +12 | durability max |
| +16 | energy (the item card's "Energy") |
| +20 | energy max |
| +24 | 0.001 in every item seen (?) |
| +28 | weight |
| +32, +36 | ? (the battle axe: 6404500, 1000) |
| +40 | an attached spell's object id, or -1 |
| +48 | weapons: complexity max (the card's "Complexity: 30/48"; the used part is the spell's) |

Weapons end with 3 floats: +56 the minimum damage, +60 the damage range (max = min + range: 48.51 + 37.8 shows as
"Damage: 48-86"), +64 0. Armour ends with 2 × 7 protection floats, one per damage type (the third and sixth are 0.8 ×
the others in every piece seen). Checked against the game's item card (an alloy battle axe of 1.mp, 2026-10-03).

**Unit parameters (0x704 bytes),** checked against the game's character screens:

| Offset | Meaning |
|---|---|
| +0x04 | experience, total (float) |
| +0x08 | experience spent; the game's "Your experience" is total − spent |
| +0x0C/+0x10 | Strength: base (before abilities), and the shown value |
| +0x14/+0x18 | Dexterity: base, shown |
| +0x1C/+0x20 | Intelligence: base, shown |
| +0x2C/+0x30 | Actions: base, shown |
| +0x50/+0x54 | encumbrance: current (as last saved), max |
| +0xB4 | sight |
| +0x164 + k × 0xF4 | the 6 body parts (k: head, body, arm, arm, leg, leg), see below |
| +0x6C8 | Use/Steal (byte) |
| +0x6CB + id | the other skills (bytes; id from the database's Skills: melee 0, archery 1, elemental 3, sense 4, astral 5) |
| +0x6D3 + group | abilities, the level 0..3 (bytes) |

A group is 3 rows of the database's Perks sheet (Specialist, Expert, Master):

| Groups | Abilities |
|---|---|
| 0–6 | sword, axe, dagger, spear, bludgeon, bow, crossbow |
| 7–14 | the magic schools: fire, lightning, acid, illusion, divination, enchantments, healing, domination |
| 15 | night vision |
| 16 | health |
| 17 | mana (shown as Stamina) |
| 18 | vitality (Regeneration) |
| 19 | spirit (Recovery) |
| 20 | quickness (Actions) |
| 21 | lift (Encumbrance) |
| 22 | backstab |
| 23–25 | strength, dexterity, intelligence |

Health and stamina are not stored; the game computes them.

**Body parts** (0xF4 bytes each, from +0x164; checked against the character screen's Armor lines):

| Offset | Meaning |
|---|---|
| +0x00 | 7 floats: protection (head 0, body 8.0, arms and legs 16.0 for HALAL MAN; which pieces give it: ?) |
| +0x38 | u32 3 |
| +0x48 | 7 floats: protection (28.455 on every part for HALAL MAN) |
| +0x80, +0x8C | u32 3 |
| +0x90, +0x94 | the part's health, max and current (head 784, body 1569, arms 523, legs 1046) |
| +0x98 | a weight (head 1.5, body 3, arms 1, legs 2: the hit chance?) |
| +0x9C | a factor (1.01, 1.01, 0.34, 0.34, 0.67) |
| +0xA0 | u32 1..3 |
| +0xA4 … | pointers of the running game (meaningless in the file) |

The screen's value for a part is the first float of both arrays added: head 28.5, body 36.5, arms and legs 44.5. The
last part's block runs into the skills at +0x6C8, so its fields past +0x94 are not in the file.

**Money** is the file's last 8 bytes: two u32 whose XOR is the amount. The game picks a new first value on every
save.
