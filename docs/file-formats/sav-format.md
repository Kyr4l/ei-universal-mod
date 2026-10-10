# .sav: single-player saves (`<game or mod>/saves/saveNN/`)

Work in progress (2026-10-03). Decoded from the saves of the XP-Mates and Lost In Astral mods. Research tools and
decompressed samples are in `_cpr/claude-re/sav/`.

## The folder

A save is a folder of `.sav` files:

| File | Content |
|---|---|
| `info.sav` | not compressed: u32 0x111, u32 3, f32 (a game time?), the allod (`Ingos\0`), the zone (`bz8k\0`), u32 ?, the save's title (newer saves; 28 bytes without it) |
| `shot.sav` | 98,304 bytes, not compressed: the thumbnail, 256 x 192 pixels of 16 bits (probably RGB565) |
| `scenario.sav` | the campaign: item stocks, the party and the quest variables |
| `mission.sav` | the zone being played, in full (its units, the party's units...) |
| `<zone>.sav` | one per zone visited (`bz7g`, `gz11k`, `cz0k`...): its saved state |

## The container

The compressed files use the `.mp` container (`mp-format.md`): u32 head, u32 version 0x74, u32 checksum, u8 1,
u32 size, then the same LZ-style compression. The head is 0x102 for `mission.sav` and the zone files, 0xDEAD for
`scenario.sav` (0x114 in a `.mp`).

## scenario.sav

It holds several item groups in the `.mp` list format: u32 n, n x {u32 id, u32 kind, u16 a, u16 b} for the items,
the same for the spells, then each object's detail block, each group closed by `DF FA FF FF` and a byte. In the
sample there are five groups. The four before the party alternate stocks of equipment and of spell containers
(merchants' or the camp's?). The last is the party's, followed exactly as in a `.mp` by:

- the backpack: u32 n, n x u32 object id (-1 empty);
- u32 members, then each member in the `.mp` member format: 5 strings (the name, empty for the hero; the figure;
  "", ""; the unit name), u32 id, the 0x704 unit parameters, 12 bytes, 4 lists, a string, u32, u32, u8, u32,
  u32, a string. The hero's id is 0x3B9AF5CE, as in every `.mp`; the role string is `Hero` for the hero and
  `merc6`, `merc9`... for the mercenaries.

A quest item's detail block (kind 0x3009) is 56 bytes.

After the members come the quest variables exactly as in a `.mp` (string name, f32 value, until an empty name: 109
to 821 in the samples), then a tail of about 260 bytes whose first 8 bytes hold the money as a `.mp`'s last 8 do
(two u32, money = a XOR b). A scenario.sav can hold SEVERAL sections in the party format: the party (the one whose member has the role
`Hero`, with its mercenaries) and other stored characters (an XP-Mates save: a spare "Human Hero Hadagan", the NPC
"Human Hadagan Pretty" with the role `Nalo`). They are found where a `.mp` party section reads and writes back
exactly; the quest variables follow the last one. All 255 scenario.sav files of the installed mods are written back
byte for byte. In a save made inside a zone, mission.sav holds the party too (the same format, near its end). um-multitool's MP editor
opens saves (a saves folder: each saveNN) with these.

## Zone files and mission.sav

The file starts with `<map>.mob\0<terrain>.mpr\0<zone>\0` (the map can be another zone's: cz0k uses zone21.mpr),
then a 128-byte header, then a variable part not decoded yet (about 5 KB; it starts with a long run of the u16
0x00FF: a per-cell map such as the explored area?), then one record per unit (about 3.2 KB each), then the zone's item groups.

A unit record:

- its name 3 times (`DriadGreenA7\0` x 3);
- u32 0x007AFF61 (?), u32 2, f32 health, health max, regeneration (?), mana, mana max, regeneration (?), 14 x f32;
- strings, from here not aligned on 4 bytes: the AI or group (`PADriad00`?), the texture (`default0`), ..., the
  figure (`unmodr`), then the complection (3 x f32);
- about 160 bytes in: the position (3 x f32);
- the 0x704 unit parameters, the same block as in a `.mp` member (attributes, sight, the 6 body parts, skills),
  most often about 476 bytes into the record;
- then about 1.3 KB more (the position again near the end: the logic's place?).

### The 128-byte zone header (after the three names; 16 zone files of 2 saves)

| Offset | Type | Seen | Probably |
|---|---|---|---|
| +0 | f32 × 3 | (-0.894, 0.447, 0) or (0, 1, 0) | a unit vector: the sun's direction |
| +12, +16 | f32 | 0.07, 0.255 / 0.076 | light or fog settings |
| +100 | u32 | 160562..181538, different per zone | a game time (when the zone was last left?) |
| +104 | u32 | 0 or 2 | ? |
| +108 | f32 | 0 or 40 | ? |
| +112 | f32 | 21.07, 20.36, 13.36 | the time of day (hours) |
| +116 | f32 | 60 | minutes per hour? |
| +120 | u32 | the same in every zone of a save (123459, 176323) | the save's game time |
| +124 | u32 | 3 (bz*, cz* zones), 1 (gz* zones) | ? (not the allod: cz and bz are both 3) |

The other words are 0.

## The container's head word

The u32 before the version tells what the file holds: 0x114 a character (`.mp`), 0x102 a zone's state
(`mission.sav`, `<zone>.sav`), 0xDEAD the campaign (`scenario.sav`).

## Not decoded yet

- the 5 KB before the first unit of a zone file;
- the strings between a unit's names and its position, and the end of a record;
- the start of `scenario.sav` (before the first item group); the rest of its tail (after the money: strings such as
  `zt22`, `q48k` and floats);
- which stocks the four other item groups are.
