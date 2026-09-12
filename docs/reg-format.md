# Evil Islands Registry Database Format (.reg)

## Overview
Evil Islands `.reg` files are binary key-value database archives used to store engine configuration, game settings, gameplay variables, and quest metadata. Despite sharing the `.reg` extension with Windows Registry files, Evil Islands `.reg` files are **proprietary binary structures** identified by the 32-bit magic constant `0x45AB3EFB` (in byte order: `0xFB, 0x3E, 0xAB, 0x45`).

This document details the file format, data types, hashing algorithm, collision resolution, and the serialization/deserialization logic between plain-text `.ini` and binary `.reg`.

---

## File Header Structure
Every `.reg` file begins with a 6-byte header followed by the top-level section hash table:

| Offset | Type | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `uint32_t` | `magic` | Fixed magic number: `0x45AB3EFB` |
| `0x04` | `uint16_t` | `numSections` | Number of sections in the file |
| `0x06` | `HashEntry[numSections]` | `sectionTable` | Section hash table (6 bytes per section) |

---

## Section Table & Hash Table Structure
Each entry in a section or key hash table is exactly 6 bytes (little-endian):

```cpp
struct HashEntry {
    uint16_t nextIndex; // 0xFFFF if end of collision chain, or 0-based index of next slot
    uint32_t offset;    // Absolute file offset (sections) or section-relative offset (keys)
};
```

### Hash Algorithm
Evil Islands computes a simple case-insensitive ASCII sum hash:
```cpp
uint16_t CalculateEiHash(const std::string& name, uint16_t bucketCount) {
    if (bucketCount == 0) return 0;
    uint32_t sum = 0;
    for (unsigned char c : name) {
        sum += static_cast<unsigned char>(std::tolower(c));
    }
    return static_cast<uint16_t>(sum % bucketCount);
}
```

### Hash Table Construction & Collision Resolution
1. Initialize an array of `N` `HashEntry` slots with `nextIndex = 0xFFFF` and `offset = 0`.
2. For each item (in insertion order):
   - Compute `bucket = CalculateEiHash(name, N)`.
   - If `table[bucket].offset == 0`, assign `table[bucket].offset = itemOffset`.
   - If `table[bucket].offset != 0` (collision):
     - Follow the `.nextIndex` chain from `bucket` until reaching a slot where `.nextIndex == 0xFFFF`.
     - Find the first empty slot searching **backwards from index `N - 1` down to `0`**.
     - Set the previous slot's `.nextIndex` to `freeIndex`.
     - Set `table[freeIndex] = { 0xFFFF, itemOffset }`.

---

## Section Record Structure
Each section begins at its recorded file offset:

| Offset | Type | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `+0x00` | `uint16_t` | `numKeys` | Number of keys in this section |
| `+0x02` | `uint16_t` | `nameLen` | Length of the section name in bytes |
| `+0x04` | `char[nameLen]` | `name` | Section name (CP1251 / ASCII string) |
| `+0x04+nameLen` | `HashEntry[numKeys]` | `keyTable` | Key hash table (6 bytes per key) |
| `+headerEnd` | `KeyRecord[...]` | `keyData` | Sequence of binary key-value records |

---

## Key-Value Record Structure & Data Types
Each key starts with a 1-byte type tag, a 2-byte key name length, the key name string, and the value payload:

| Field | Type | Description |
| :--- | :--- | :--- |
| `tag` | `uint8_t` | Data type tag (see table below) |
| `keyLen` | `uint16_t` | Length of the key name string |
| `keyName` | `char[keyLen]` | Key name (e.g. `AntiCrash`, `Lost Money`) |
| `value` | Variable | Packed value bytes |

### Value Tags & Encoding

| Tag | Hex | Description | Value Payload Layout |
| :--- | :--- | :--- | :--- |
| `TAG_INT32` | `0x00` | Single 32-bit signed integer | `int32_t val` (4 bytes) |
| `TAG_FLOAT` | `0x01` | Single 32-bit IEEE float | `float val` (4 bytes) |
| `TAG_STRING` | `0x02` | Single length-prefixed string | `uint16_t strLen` + `char str[strLen]` |
| `TAG_ARRAY_INT32` | `0x80` | Array of 32-bit signed integers | `uint16_t count` + `int32_t val[count]` |
| `TAG_ARRAY_FLOAT` | `0x81` | Array of 32-bit IEEE floats | `uint16_t count` + `float val[count]` |
| `TAG_ARRAY_STRING` | `0x82` | Array of strings | `uint16_t count` + sequence of (`uint16_t len` + `char str[len]`) |

---

## Type Inference Rules (INI $\to$ REG)
When converting plain text INI lines `Key=Value` into binary REG:
1. **Integer Detection**: If the value contains only digits (with optional leading `+` or `-`), it is encoded as `TAG_INT32` (or `TAG_ARRAY_INT32` if multiple identical keys exist).
2. **Float Detection**: If the value contains a decimal point `.` or exponent `e`/`E` and parses as a valid float, it is encoded as `TAG_FLOAT` (or `TAG_ARRAY_FLOAT`).
3. **String Fallback**: All other values are stored as CP1251 strings (`TAG_STRING` or `TAG_ARRAY_STRING`).
4. **Repeated Keys**: If a key appears multiple times within the same section (e.g. `Absorption=0.65`, `Absorption=0.33`), it is automatically aggregated into an array record (`TAG_ARRAY_*`).
