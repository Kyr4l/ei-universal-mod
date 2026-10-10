# Evil Islands Resource Archive Format (.res)

## Overview

Evil Islands `.res` (Resource) archives are flat, indexed binary archive containers used to package game assets including 3D model meshes (`figures.res`), textures (`textures.res`, `redress.res`), sound effects and voice clips (`sfx-*.res`), gameplay item/spell databases (`database*.res`), mission quests (`.mq`), and localization strings (`texts.res`, `textslmp.res`).

Every `.res` file is identified by the 32-bit magic constant `0x019CE23C` (little-endian: `0x3C, 0xE2, 0x9C, 0x01`).

---

## High-Level Archive Architecture

An Evil Islands `.res` archive consists of three consecutive sections:

1. **16-Byte Header**: Magic, entry count, data payload byte length, and names block byte length.
2. **File Data Payload Block**: Contiguous sequence of raw asset payloads, 16-byte aligned.
3. **Directory Table (Descriptors + Hash Table)**: Sequence of file descriptor records and collision hash chains.
4. **Names Block**: Contiguous CP1251 text strings of all contained filenames.

```
+-------------------------------------------------------------+
| 16-Byte Header (magic, numFiles, dataSize, dirSize)         |
+-------------------------------------------------------------+
| File Data Block (dataSize bytes, 16-byte aligned payloads)  |
+-------------------------------------------------------------+
| Directory Block (Descriptors + Hash Table)                  |
+-------------------------------------------------------------+
| Names Block (dirSize bytes of raw filename characters)      |
+-------------------------------------------------------------+
```

---

## 16-Byte Archive Header Structure

| Offset | Type | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `uint32_t` | `magic` | Fixed magic constant: `0x019CE23C` (`27058748`) |
| `0x04` | `uint32_t` | `numFiles` | Total number of files/entries in the archive |
| `0x08` | `uint32_t` | `tableOffset` | File offset where the descriptor/hash table begins (`16 + dataSize`) |
| `0x0C` | `uint32_t` | `namesLength` | Byte length of the trailing names block |

---

## Directory & File Descriptor Layout

The descriptor table starts at file offset `tableOffset` (which equals `16 + dataSize`).
The names block begins immediately after the descriptor table:
$$\text{namesOffset} = \text{tableOffset} + (\text{numFiles} \times 22)$$
$$\text{totalFileSize} = \text{namesOffset} + \text{namesLength}$$

### File Descriptor Records (22 Bytes per Slot)

Each of the `numFiles` slots in the hash table represents one hash bucket and contains a 22-byte file descriptor:

| Field | Type | Size | Description |
| :--- | :--- | :--- | :--- |
| `nextIndex` | `int32_t` | 4 bytes | Next slot index in collision chain (`-1` / `0xFFFFFFFF` if end of chain) |
| `dataLength` | `uint32_t` | 4 bytes | Uncompressed size of the file payload in bytes |
| `dataOffset` | `uint32_t` | 4 bytes | Absolute file offset where the file payload begins |
| `timestamp` | `uint32_t` | 4 bytes | File modification timestamp (Unix epoch time) |
| `nameLen` | `uint16_t` | 2 bytes | Length of the filename in bytes (0 if empty bucket slot) |
| `nameOffset` | `uint32_t` | 4 bytes | Relative byte offset into the names block (`namesOffset + nameOffset`) |

> **Deduplication Note**: when two files in the archive contain identical content, the second file descriptor shares the same `dataOffset` and `dataLength` without duplicating the payload bytes in the data block. Confirmed against the vanilla `eipacker.exe` when invoked with its standard `/pack <path>` CLI flag (the procedure the modding community has always used) - it deduplicates identical-content files exactly this way, and `um-restool` matches it (see `PackResArchive` in `tools/um-multitool/restool.cpp`). Note that the *same* `eipacker.exe` binary has a second, non-deduplicating code path when invoked via a bare directory argument (its drag-and-drop-equivalent mode) instead of `/pack`; that mode was mistaken for the "real" reference behavior partway through this investigation, which is worth flagging in case anyone reproduces this format from scratch again - always reverse-engineer against `/pack` mode specifically, since that is the mode real mod archives are actually built with.

---

## Hash Function & Collision Resolution

Evil Islands calculates a case-insensitive ASCII sum hash for fast lookup:

```cpp
uint32_t CalculateResHash(const std::string& name, uint32_t bucketCount) {
    if (bucketCount == 0) return 0;
    uint32_t sum = 0;
    for (unsigned char c : name) {
        sum += static_cast<unsigned char>(std::tolower(c));
    }
    return sum % bucketCount;
}
```

---

## Unpacking Algorithm (`.res` $\to$ Folder)

1. Open `.res` file in read-only binary mode.
2. Read 16-byte header, verify `magic == 0x019CE23C`.
3. Compute `descStart = 16 + dataSize` and `namesStart = fileSize - dirSize`.
4. Seek to `descStart`.
5. For each of the `numFiles` entries:
   - Read `uint16_t nameLen` and `uint32_t nameOffset`.
   - Read filename from `namesStart + nameOffset` of length `nameLen` (CP1251 decoded).
   - Read `int32_t nextIndex`, `uint32_t dataLength`, `uint32_t dataOffset`, and `uint32_t timestamp`.
   - Read `dataLength` bytes from `dataOffset`.
   - Write payload bytes to destination path `<folder>/<filename>`.
   - Set file modification time to `timestamp`.

---

## Packing Algorithm (Folder $\to$ `.res`)

1. Enumerate all files in the source directory, recursively.
2. **Sort them** before doing anything else - insertion order determines both the data block's byte layout and, on any hash collision, which file lands in which bucket (see "Hash Function & Collision Resolution" above), so getting this wrong produces a *different but still internally valid* archive that nonetheless silently corrupts whatever the engine's own lookup does. Confirmed (by packing the same directory with the vanilla `eipacker.exe`'s `/pack` flag under Wine, byte-for-byte) to be:
   - **Path-component-wise**, not a flattened-string compare: `"kiel\Steal\42.wav"` must sort before `"kiel\StealEmp\40.wav"` (as sibling directory names "Steal" < "StealEmp"), which a naive full-string compare gets backwards - at the point they diverge, `'E'` (0x45) is less than `'\\'` (0x5C), so a flat compare wrongly ends up putting `StealEmp` first.
   - **Case-insensitive** per component: `"Attack"` sorts before `"AttInDef"` as `"attack"` < `"attindef"`, not by raw byte value (capital `'I'` = 0x49 is less than lowercase `'a'` = 0x61, so a case-sensitive compare also gets this backwards).
3. For each file, in that sorted order:
   - Read file content and record its modification timestamp as a **plain Unix epoch second count** (confirmed by touching one file to a distinct date under Wine and observing only its own descriptor's timestamp change - not, e.g., a single shared "time of packing" value, and not any Windows FILETIME-style encoding). C++17's `std::filesystem::file_time_type` is **not safe** for this: its clock epoch is not guaranteed to be the Unix epoch until C++20's `clock_cast`, and using it directly produces a nonsensically offset value; read it via POSIX `stat()`/write it back via `utime()` instead.
   - If payload already exists in payload cache (deduplication - see the note in "Directory & File Descriptor Layout" above; this is genuinely active in `eipacker.exe`'s standard `/pack` mode, not merely theoretically permitted by the format), point `dataOffset`/`dataLength` to the existing payload's.
   - Otherwise, append payload to the contiguous data buffer at the current offset (padded to a 16-byte boundary).
4. Build the names block by concatenating all relative filename strings (in the same sorted order) and recording their respective offsets.
5. Construct file descriptors and collision hash chains.
6. Write 16-byte header: `0x019CE23C`, `numFiles`, `dataSize`, `dirSize`.
7. Write file data block.
8. Write file descriptors block.
9. Write names block.
