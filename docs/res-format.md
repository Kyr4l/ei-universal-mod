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
| `0x08` | `uint32_t` | `dataSize` | Byte size of the contiguous payload data section |
| `0x0C` | `uint32_t` | `dirSize` | Byte size of the trailing names block |

---

## Directory & File Descriptor Layout

The directory section starts immediately after the file data section at file offset `16 + dataSize`.

### Base Names Offset:
The names block starts at:
$$\text{namesOffset} = \text{fileSize} - \text{dirSize}$$

### File Descriptor Records:
For each file in the archive (from `0` to `numFiles - 1`), the archive stores:

| Field | Type | Size | Description |
| :--- | :--- | :--- | :--- |
| `nameLen` | `uint16_t` | 2 bytes | Length of the filename in bytes |
| `nameOffset` | `uint32_t` | 4 bytes | Relative byte offset into the names block (`namesOffset + nameOffset`) |
| `nextIndex` | `int32_t` | 4 bytes | Hash collision chain index (`-1` / `0xFFFFFFFF` if end of chain) |
| `dataLength` | `uint32_t` | 4 bytes | Uncompressed size of the file payload in bytes |
| `dataOffset` | `uint32_t` | 4 bytes | Absolute file offset where the file payload begins |
| `timestamp` | `uint32_t` | 4 bytes | File modification timestamp (Unix epoch time) |

> **Deduplication Note**: When two files in the archive contain identical content, the second file descriptor can share the same `dataOffset` and `dataLength` without duplicating the payload bytes in the data block.

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

1. Enumerate all files in the source directory.
2. For each file:
   - Read file content and record modification timestamp.
   - If payload already exists in payload cache (deduplication), point `dataOffset` to the existing payload offset.
   - Otherwise, append payload to contiguous data buffer at current offset (padded to 16-byte boundary).
3. Build the names block by concatenating all relative filename strings and recording their respective offsets.
4. Construct file descriptors and collision hash chains.
5. Write 16-byte header: `0x019CE23C`, `numFiles`, `dataSize`, `dirSize`.
6. Write file data block.
7. Write file descriptors block.
8. Write names block.
