# .adb: animation databases (`database.res`)

Each animated model has an `.adb` in `res/database.res`, named after its figure (`unhuma.adb` for `unhuma.mod` /
`unhuma.anm`). It lists the model's animation clips (the entries of its `.anm`, see `figure-format.md`) and, for each
one, when the game may play it: which action, which weapons, how likely, and the frames where something happens.
Reverse-engineered from the files only (2026-10-03; tools: `_cpr/claude-re/anm/adb.py`, `anm.py`). Fields marked
"?" are guesses still to be checked in the game.

## File

```
char[4] "ADB\0"
u32     records
char[24] the model's name ("Male", "Bird00", "MainColumn"...)
f32[3]  ? (Male 0.507 1.052 1.073; Bird00 1.0 1.01 1.02)
records x 88 bytes
```

## Record (88 bytes)

| Offset | Type | Meaning |
|---|---|---|
| +0 | char[16] | the clip's name in the `.anm` (`uattack01`, `cwalk02`...) |
| +16 | u32 | index (0, 1, 2... with gaps) |
| +20 | u8 | weapon mask: bit 0 sword, 1 axe, 2 dagger, 3 spear, 4 bludgeon, 5 bow, 6 crossbow (the abilities' order); 0 = unarmed, 0x7F = any |
| +21 | u8 | 0 |
| +22 | u8 | the action (see below) |
| +23 | u8 | bit 7: the clip loops (the `c` clips); low bits: a variant (attacks 1-3, deaths 2/4/6...) |
| +24 | u32 | flags (deaths: larger values whose bytes repeat the flags). Bit 3 (8) is set exactly when +68 / +72 are filled (775 of 775 records checked): on the first clip of a group (`cidle01`, `uhit`, `udeath01`). Bit 1 (2) on nearly every clip; bits 2, 4, 5 not tied to anything yet |
| +28 | u32 | probability among the clips that fit (100; idles 0-100) |
| +32 | u32 | the last frame (the `.anm` clip has this + 1 frames) |
| +36 | f32 | locomotion: the distance one cycle covers (walk 1.04, run 3.76, crawl 1.0); also set on some crosses and deaths |
| +40 | u32 × 2 | bow: the frames where the arrow is drawn and released (20, 45...) |
| +48 | u32 × 4 | event frames: footsteps for walks and runs (7, 19), the hit for melee attacks, the cast... |
| +64 | u32 | the main event frame (melee: the hit; casts: the release) |
| +68 | u32 | 0 unless +24 bit 3: then 1, 7, 10, 18... (an id or a count for the group?) |
| +72 | u32 × 4 | 0 unless +24 bit 3 (melee and hits: 1 2 3 4; bow: 7 0 0 0; crossbow: 8 0 0 0) |

Actions (+22) seen in `unhuma.adb`: 0x10-0x11 run, 0x14-0x16 walk, 0x17 crawl, 0x58-0x5B idle, 0x89 attack, 0x8C-0x8E
cast, 0x10-0x17 with bit 0x40 in +23 the stance changes (`scross`), 0x24-0xE7 crosses (`ucross`), 0x1C-0x1F death, 0x20-0x23
hit, 0x04-0xC4 with +23 4-0x13 the special and briefing actions. The low bits look like variants (e.g. armed / unarmed
walks).

## Files that do not fit (vanilla `database.res`, 54 `.adb`)

Four files are not `44 + records × 88` bytes: `unmoco3.adb`, `unmoco4.adb` (296 bytes, 3 records), `inmm1.adb`
(80, 1), `unmozozo.adb` (716, 8): their records are shorter or the header differs; not decoded. In the other 50,
the last frame + 1 equals the `.anm` clip's frame count for 936 of 941 clips.

## How the game chooses a clip (to check)

For an action, the clips whose action code fits, whose weapon mask has the bit of the weapon in hand (unit record
+0x49C holds the weapon type) and whose probability allows it; the looping ones repeat, the others play once and the
event frames trigger the hit, the footsteps, the arrow.
