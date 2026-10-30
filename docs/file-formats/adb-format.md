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
f32[3]  root-position / model-offset scale at height 0, reference, height 1 (CObject3D::GetModelLerp 0x518D30, game-exe/renderer.md; Male 0.507 1.052 1.073; Bird00 1.0 1.01 1.02)
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

## Files that do not fit (vanilla `databaseadb.res`, 54 `.adb`)

The game does not parse an `.adb`: it keeps the file's bytes mapped and reads them as 88-byte records from +44 (count
at +4; `CObject3D::PickAnimDbEntryByMask` 0x5190C0, `AnimDb_FindEntryByName` 0x512F60). Four files are not
`44 + records x 88` bytes (decoded 2026-10-20 from their bytes and the clip frame counts):

| File | Size | Records | Layout |
|---|---|---|---|
| `unmoco3.adb`, `unmoco4.adb` | 296 | 3 | the old record of 84 bytes: the same fields without the probability (+28): +24 flags, +28 the last frame, +32 the locomotion float |
| `unmozozo.adb` (`Zombie00`) | 716 | 8 | the same 84-byte record (`cwalk` has 0.614 at +32 where a walk's distance goes) |
| `inmm1.adb` (`MainColumn`) | 80 | 1 | a record of 36 bytes: name, index, key, flags, last frame; the header's name field has stray bytes after the name |

The game reads these with the 88-byte stride, so from the second record on it sees shifted fields (the clip names
come out cut, the keys wrong): these four are old files the engine cannot use properly; check before editing them
that no unit of the mod uses their models. In the other 50, the last frame + 1 equals the `.anm` clip's frame
count for 936 of 941 clips.

## How the game chooses a clip (read from game.exe 2026-10-20)

The dword at record +20 (bytes +20..+23: weapon mask, 0, action, type) is the clip's **key**. The game builds a key
for what the unit is doing and `CObject3D::PickAnimDbEntryByMask` (0x5190C0) returns a record that fits it:

- The key's bits 15..31 must equal the record's bits 15..31 exactly.
- Weapon bits 0..14: the key carries the one bit `1 << kind` of the weapon in hand (`CItemWeapon::GetDamageKind`: 0
  sword, 1 axe, 2 dagger, 3 spear, 4 bludgeon, 5 bow, 6 crossbow; with no weapon the unit's body class
  (record +0x28C) when it is below 7, else no bit). A key with a weapon bit fits records whose mask has that bit
  (`0x7F` = any weapon); a key without one fits only records with mask 0. So `cwalk01` (mask 0) is the walk of
  an unarmed unit and `cwalk02` (mask 0x7F) the one of an armed one.
- Several records fit: each is dropped when its chance (+28) is below a random percent, then one of the rest is
  picked at random (that is how `cidle01..22` and the attacks `uattack01..03` vary). One record fits: it plays.
- `CUnit::FindAnimRecord` (0x5301B0, only for the unit kinds 0x32..0x34) tries the key, then the key without
  the weapon bits, then (stance 4) the key without the stance and move bits; with its "fallback" flag a random
  record that fits the key's upper bits whatever the weapon.

The key's layout (as the record stores it):

| Bits | Byte | Meaning |
|---|---|---|
| 0..6 | +20 | the weapon (above) |
| 15 | +21 | stance 4 (sitting): `crest`, `ucross28`, `uhit21`, `udeath27` |
| 16..17 | +22 | the move mode: 3 - the stance: 0 stance 3 (run), 1 stance 2 (walk), 2 stance 1 (sneak), 3 stance 0 (crawl) (stances: docs/game-memory.md) |
| 18..21 | +22 | the kind of action: 1 special / briefing (`ubriefing`, `uspecial`, actions 0x04 0x44 0x84 0xC4), 2 attack (0x89), 3 cast (0x8C-0x8E), 4 run (0x10-0x11), 5 walk (0x14-0x17), 6 idle (0x58-0x5B), 7 death (0x1C-0x1F), 8 hit (0x20-0x23), 9 stance change (0x24-0x26, 0x64.., `ucross`) |
| 22..23 | +22 | a side / limb variant: for deaths and hits a random one (0x1C, 0x5C, 0x9C, 0xDC: another fall), for walking the crippled leg (+0x238 of the unit: 0x54 / 0x94 limp), for attacks and casts bit 7 is always set |
| 24..30 | +23 | a variant number (attack 1-3), or the type: 0x06 / 0x02 / 0x04 the reaction clips (hit and death; other unit kinds use 0x02 / 0x04), 0x40 a stance transition (`scross`: moving into a stance) |
| 31 | +23 | the clip loops (`cidle`, `cwalk`, `crun`, `ccrawl`, `crest`) |

Who builds which key (all in `CUnitServer`, the unit's server specific; the client plays what the server queues):

| Key | Builder | For |
|---|---|---|
| weapon \| stance bit 15 \| move mode \| 0x80580000 | BuildIdleAnimQueue 0x550D90 | the idle loop (a sneaking unit gets a placeholder entry) |
| weapon \| move mode \| 0x6000000 \| 0x1C0000 (death) or 0x200000 (hit) \| a random side bit | BuildHitDeathAnimQueue 0x550A80 (was BuildAnimQueueForState) | the reaction when hit or killed |
| weapon \| variant \| 0x90000 | BuildActionAnimQueue 0x550100 | an attack; the record's duration and its +64 event frame (the hit) are queued with it |
| 0x5440000 / 0x5040000 / 0x4C40000 by the interaction, 0xC000000 / 0xD000000 / 0xD400000 / 0x5800000 for talking, by stance and weapon | BuildActionAnimQueue, kind 6 | interactions (the briefing / special clips) |
| weapon \| from-mode \| 0x240000 \| to-mode bits | BuildTransitionAnim 0x5516D0 | a change of stance: the `ucross` clips |
| weapon \| move mode \| (stance 3: 0x100000 else 0x140000) \| 0x40000000 (into) or 0x80000000 (out of) | BuildStanceAnimQueue 0x5511A0 | the `scross` clips |
| weapon \| move mode \| the moving key | ApplyMoveAnimSet 0x5474B0, RebuildMoveAnims 0x550F30, GetStanceAnimSet 0x551400 | walk / run / crawl while moving (the set is rebuilt when the key changes) |

When nothing fits an attack key, the game tries `weapon | 0x2890000` and otherwise prints the debug message
"Fucking ShITTT!!! ArtIsTs MuSt diE!!!" and plays record 0: a model whose `.adb` lacks a clip for a weapon.

The duration a clip takes in the game is its frame count times a per-unit speed factor chosen by the key's
action kind (`CUnit::AnimDurationFor` 0x5327D0): attack unit record +0x4B0, cast +0x4B4, hit +0x4B8, death +0x4BC,
stance change +0x4C0, special +0x4C4, everything else 1.0 (the factors come from the unit's database record).
A unit playing a hit reaction (kind 8) is known to the server for the clip's duration
(`CUnitServer::CurrentAnimIsHitReaction`).

What stays unknown: the +68 / +72 words (they exist exactly on the first clip of a group, `cidle01`, `uhit01`,
`udeath01`; the selection, duration and event code read so far never touches them). 
