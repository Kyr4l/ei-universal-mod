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
| 0x3007 | quest item | QuestItems | |
| 0x3008 | spell container | QuickItems | the spell's object id at +40 of its block |

**Detail block sizes:** 0x3002 66, 0x3004 68, 0x3005 108, 0x3006 56, 0x3007 64, 0x3008 56. Each block starts with
`{u32 id, u16 a, u16 b}`. For weapons and armour, the next words are:

| Word | Meaning |
|---|---|
| +8 | price |
| +12 | price again (base?) |
| +16 | durability |
| +20 | durability max |
| +40 | an attached spell's object id, or -1 |

Armour then has 2 × 7 protection floats (one per body part). Weapons then have 3 damage-like floats.

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
| +0x180 … +0x6B0 | the body-part blocks (armour, health) |
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

**Money** is the file's last 8 bytes: two u32 whose XOR is the amount. The game picks a new first value on every
save.
