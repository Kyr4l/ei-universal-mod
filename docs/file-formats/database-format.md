# Evil Islands Gameplay Database Format (database*.res)

## Overview

Evil Islands stores its gameplay databases — items, spells, perks, units, footprints, lever prototypes, and NPC dialogue ("Acks") — as binary blobs packed inside standard `.res` archives (see [res-format.md](res-format.md) for the outer container). This document covers the **payload format** of those blobs: `items.idb`, `levers.ldb`, `perks.pdb`, `prints.db`, `spells.sdb`, `units.udb`, and `acks.db`.

These files are normally authored as `.xlsx` spreadsheets (`database.xlsx` for Acks dialogue, `databaselmp.xlsx` for the six gameplay-stat databases) and compiled by `um-xlsxdb` (`tools/um-standalone-tools/um-xlsxdb`), which replaces the legacy `wine`-hosted `EIDBEditor` (`tools/ei-um-autopacker/bin/eidbeditor-144/DBEditor.exe`).

Everything below was reverse-engineered from scratch by diffing `um-xlsxdb`'s output against the shipped `Universal-Mod/res/database.res` and `Universal-Mod/res/databaselmp.res`, and by round-tripping controlled edits through the real `DBEditor.exe` under `wine` (a "wine oracle") to observe exactly which bytes changed. The result is **verified byte-for-byte identical** to both release archives — every field type documented here has a matching, checksum-confirmed reference encoder.

`.adb` files (per-NPC override blobs unpacked once by `ADBedit` and re-packed as static binary resources) are a separate, unrelated format and are out of scope here.

---

## The Universal Tagged-Value Primitive

Every nesting level of every database file — file, block, record, field, list item, and scalar leaf value — is written with **one single recursive encoding primitive**:

```cpp
EncodeTagged(tag, raw):
    n = len(raw)
    doubled = 2 * n
    if doubled <= 255:
        emit tag as 1 byte
        emit doubled as 1 byte
    else:
        emit tag as 1 byte
        emit (doubled + 1) as a 4-byte little-endian uint32
    emit raw bytes
```

The length field is **self-describing without any separate marker byte**: a length that fits in one byte is always even (`2*n`), while a length that needs the wide 4-byte form always has its low byte odd (`2*n+1`). A reader therefore only needs to look at whether the first length byte is even or odd to know how many more bytes to read — no lookahead or fixed per-field width table is required. This holds for every one of the thousands of tagged values across both shipped databases; it was confirmed by testing values on both sides of the 255-byte boundary via the wine oracle.

There is exactly one exception to "encode_tagged everywhere": scalar leaf values and anonymous list items never receive the odd "+1" flag even when they need the wide 4-byte form (a leaf/item is never "absent", so the flag — which otherwise marks "this container held at least one byte of content" — is meaningless for them and is simply omitted, i.e. `doubled` with no `+1`). Named containers (file/block/record/field) do carry the flag once their content is non-empty; an *empty* named container (e.g. an `AcksUniqueType` field with zero list items) is written as a bare `[tag, 0x00]` two-byte entry.

---

## File → Block → Record → Field Hierarchy

Both database families (Acks and the six gameplay-stat databases) share the same top three nesting levels, which is `EncodeTagged` applied recursively:

```
File   = EncodeTagged(tag=1,      content = Block₁ + Block₂ + ...)
Block  = EncodeTagged(tag=BlockID, content = Record₁ + Record₂ + ...)
Record = EncodeTagged(tag=1,       content = Field₀ + Field₁ + ... in ascending FieldID order)
```

* `BlockID` is the numeric ID from `dbblocks.txt` (e.g. `Materials`=1, `Weapons`=2, ... within the `Items` database), **not** a fixed value of `1` — this is only apparent once a database has more than one block, since it happens to coincide with `1` for a database's first block.
* A record is emitted for **every non-fully-blank row** in the sheet (checked across *all* columns, not just the name/key column — a row with an empty name but populated data columns still becomes a record; see "Known Quirks" below).
* Every database ends with a **fixed 10-byte trailer**, `00 00 02 0C 02 08 01 00 00 00`, appended after the outermost File-level `EncodeTagged` blob. It is byte-for-byte identical across every database file in both `database.res` and `databaselmp.res`, and was confirmed constant across dozens of content edits during reverse engineering — it does not encode a size, checksum, or anything content-dependent that we could find.

---

## Acks Family (`database.xlsx` → `acks.db`)

The `Acks` database (`dbfiles.txt` block 7) stores NPC dialogue. Its three blocks/sheets are `Answers` (BlockID 1), `Cryes` (2), and `Others` (3).

### Field Layout

* Field `1` of every record is the `Name` string (the NPC/actor key), always a plain leaf `EncodeTagged(1, cp1251_bytes + '\0')` — never omitted, even when empty.
* All other declared fields (10–44 for `Answers`, 10–14 for `Cryes`, 10–11 for `Others`) are `AcksUniqueType` **list** fields:

```
Field = EncodeTagged(FieldID, Item₁ + Item₂ + ... )   // or empty raw if the cell has no {..} groups
Item  = EncodeTagged(tag=1,   Subfield₁ + Subfield₂ + ...)
```

Each `Item` corresponds to one `{...}` group in the spreadsheet cell, written using a small key=value mini-language, `##`-separated:

```
{1=Acks\HMW\Move\1.wav##2=30##3=Ха!##4=HMW_Move001}{1=Acks\HMW\Move\2.wav##2=30##3=Угу.##4=HMW_Move002}
```

`AcksUniqueType(20)` subfields (used by every Acks field except one):

| Subfield | Type | Meaning |
| :--- | :--- | :--- |
| `1` | String | Voice line `.wav` path |
| `2` | SignedLong | Probability weight |
| `3` | String | Subtitle text |
| `4` | String | Voice-line text ID |

`AcksUniqueType(21)` — used **only** by `Answers` field `32` ("Bored") — adds one more subfield:

| Subfield | Type | Meaning |
| :--- | :--- | :--- |
| `10` | UnsignedLong | Extra parameter (observed as a cooldown/timer value) |

A subfield is only written if its key is present in the `{...}` group (e.g. `{...##4=}` still writes field `4` as an empty string, but a group with no `4=` key at all omits it entirely).

---

## Gameplay Stat Databases (`databaselmp.xlsx` → 6 files)

`Items`, `Levers`, `Perks`, `Prints`, `Spells`, and `Units` all use the plain `File → Block → Record → Field` hierarchy with **scalar or flat-array field types** (no `AcksUniqueType`-style repeatable struct lists), driven directly by `dbtypes.txt`. Column-to-field mapping is read from each sheet's row-3 `FLDx-y` marker cells (`x`=FieldID, `y`=FieldIndex, i.e. list-element position for multi-column fields).

### Leaf Types

| Type | On-disk payload |
| :--- | :--- |
| `String` | CP1251 bytes + trailing `\0` |
| `SignedLong` / `UnsignedLong` | 4-byte little-endian `int32`/`uint32` |
| `Float` | 4-byte little-endian IEEE-754 `float` |
| `Byte` | 1 raw byte |
| `Hex` | The cell's hex-digit string decoded to raw bytes, **no** terminator |
| `FixedString(N)` | CP1251 bytes, truncated/zero-padded to exactly `N` bytes, **no** terminator |
| `BitList(N)` / `BitByte` | The `N` (or 8, for `BitByte`) boolean columns packed LSB-first into a single 4-byte (`BitList`) or 1-byte (`BitByte`) mask — **not** a per-element array |

### Flat-Array List Types

`FloatList`, `ByteList`, and `UnsignedLongList` are a bare concatenation of packed elements with **no per-element tag** — just `EncodeTagged(FieldID, elem₁ + elem₂ + ...)`. Element count comes from however many `FLDx-y` columns exist for that field.

### Tagged-Item List Types

`StringList` and `MinfixedStringList(N)` read a **comma-separated** cell value (a single column, `FieldIndex 0`) and write one `EncodeTagged(tag=1, ...)` entry per comma-separated part:

* `StringList`: each part is a plain CP1251 string + `\0` terminator.
* `MinfixedStringList(N)`: each part is zero-padded to **at least** `N` bytes with **no** terminator byte added on top of the padding (a part already `>= N` bytes is written as-is).

### `TypeList(N)` — Fixed Single-Instance Substructures

Unlike `AcksUniqueType`, `TypeList(N)` is **one fixed struct instance**, not a repeatable list, and has **no** `tag=1` item wrapper around it:

```
Field = EncodeTagged(FieldID, Subfield_a + Subfield_b + ...)
```

Its subfields are read directly from separate spreadsheet **columns** (`FieldIndex` = subfield ID), not from a `{...}` mini-language cell. The substructure's own field list (block `20` or `21`, scoped per database — e.g. `Prints`'s block `20` differs from `Units`'s block `20`) comes from the same `dbtypes.txt` section as the parent database, just under a different (reused) `BlockID`.

Every subfield slot is always written, even when its cell is blank (defaulting to `0`/`""`) — unlike `AcksUniqueType`, there is no per-subfield presence check.

---

## Known Quirks (Undocumented DBEditor Behavior)

These were discovered empirically and are **not** reflected in `dbtypes.txt`/`dbheaders.txt`. They are almost certainly real bugs/limitations in the original `DBEditor.exe`, reproduced here only because byte-exact output requires it:

1. **`Items` block `4` (`QuickItems`), field `26` (`ByteList`, "Science/Stealing modifier") is never written**, regardless of its cell values. Confirmed via the wine oracle by forcing a non-zero value into all three of its columns and observing the output was unchanged (the field's tag is completely absent from the record).
2. **`Items` block `4` (`QuickItems`), field `27` (`Hex`, "Unk27-0") is always written with zero-length content** (`tag, 0x00`), regardless of its cell's actual hex string. Confirmed the same way — forcing a non-zero hex string in the spreadsheet still produced an empty field.
3. **A record is emitted even when its name/key column is blank**, as long as at least one other cell in that row has a value (observed in `BloodPrints`, a mid-sheet row with an empty `Terrain Type` but real `Opacity`/`Lifetime`/`Fadeout` data). Rows where *every* cell is blank are skipped.

Neither quirk affects Acks records — every declared Acks field is always written, even when empty (see "Universal Tagged-Value Primitive" above).

---

## Container Packing (Correction to res-format.md)

[res-format.md](res-format.md) states file payloads in a `.res` archive are "16-byte aligned" — that describes `um-restool`'s own packing choice when it builds an archive from scratch, **not** a hard requirement of the format itself:

* `Universal-Mod/res/database.res` and `databaselmp.res` (built by the real `DBEditor.exe`) place payloads **back-to-back with zero padding** between them.
* The original vanilla `Evil Islands/res/database.res` has small (3–10 byte), *non*-16-aligned gaps between some files — consistent with incremental hand-editing over the file's lifetime, not any fixed alignment rule.

`um-xlsxdb` reproduces `DBEditor.exe`'s exact behavior: **no inter-file padding**, and files inserted into the archive in **ascending alphabetical filename order** (confirmed against both release archives). `um-restool`'s 16-byte-aligned packing remains a perfectly valid choice for archives it builds itself — the game only ever reads via the recorded `dataOffset`/`dataLength` pair, so padding (or its absence) is cosmetic, not a correctness requirement — but it is a different convention from `DBEditor.exe`'s output, so the two tools do not currently produce bit-identical archives from the same input file set. Byte-exact reproduction of a *specific* existing archive (as required here) cannot assume either convention; it must match whatever the original writer did.
