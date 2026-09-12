# Evil Islands Map Object File Format (.mob)

## Overview
Evil Islands `.mob` (Map Object) files contain complete map scene hierarchies: placed units, inventory, armor/weapon equipment, diplomacy tables, levers, light sources, particle emitters, torches, sounds, quest logic triggers, and mission scripts.

---

## Binary Node Architecture
Every node in a `.mob` file adheres to an 8-byte header structure:

| Offset | Type | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `uint32_t` | `type` | 32-bit Node Magic Type ID |
| `0x04` | `uint32_t` | `length` | Total node length in bytes, **including the 8-byte header** |
| `0x08` | `uint8_t[length - 8]` | `payload` | Node data payload |

> **Crucial Rule**: The declared `length` spans the entire node (8-byte header + payload). The payload size is always `length - 8`.

---

## File Layout
A `.mob` file starts with the root container node `OBJECTDBFILE` (`40960` / `0x0000A000`):
1. `OBJECTDBFILE` (Root node, length equals file size)
2. `SC_OBJECTDBFILE` / `PR_OBJECTDBFILE` (Marker node, length 8)
3. Sequence of top-level sections:
   - `WORLD_SET` (`43984`): Environment, wind direction, sunlight, time of day.
   - `SEC_RANGE` (`65280`) / `MAIN_RANGE` (`65281`): NID allocation bounds (`MIN_ID`, `MAX_ID`).
   - `DIPLOMATION` (`3722304977`): Faction diplomacy matrix (`1024` integers) and player names.
   - `VSS_SECTION` (`7680`): Visual Scripting System triggers, checks, paths, and variables.
   - `DIRICTORY` (`57344`) & `DIRICTORY_ELEMENTS` (`61440`): Scene tree folders.
   - `OBJECTSECTION` (`45056`): Map entities (units, levers, torches, particles, sounds, static objects).
   - `SS_TEXT` (`2899242187`): Encrypted mission script (`.eis`).
   - `AIGRAPH` (`826366246`): AI navmesh and waypoint graphs.
   - `ROOT` (`0`): File termination node.

---

## Node Data Types & Binary Encodings

| Node Type Category | Declared Length | Payload Format |
| :--- | :--- | :--- |
| **Record** | Variable | Container node containing nested child nodes |
| **Null** | `8` | Empty marker node (no payload) |
| **Dword** | `12` | Single 32-bit unsigned integer (`uint32_t`) |
| **Byte** | `9` | Single 8-bit unsigned integer (`uint8_t`) |
| **Float** | `12` | Single 32-bit IEEE float (`float`) |
| **Plot** | `20` | 3D coordinate tuple: 3 floats (`X, Y, Z`) |
| **Quaternion / Rectangle**| `24` | 4D tuple: 4 floats (`X, Y, Z, W`) |
| **String** | Variable | Raw CP1251 text string (`length - 8` bytes) |
| **StringArray** | Variable | `uint32_t count` + sequence of `[type(4) + length(4) + string]` |
| **UnitStats** | `180` | Array of 43 `uint32_t` unit attributes (HP, stamina, speed, etc.) |
| **Diplomacy** | `4104` | 1024 `int32_t` faction relationship matrix |
| **StringEncrypted** | Variable | `uint32_t key` + encrypted mission script ciphertext |

---

## Script Decryption (`SS_TEXT` Node)
The mission script in Evil Islands (`SS_TEXT`, magic `2899242187` / `0xACD33E2B`) is encrypted using MSVC's Linear Congruential Generator (LCG) XOR cipher:

```cpp
std::string DecryptScript(const uint8_t* payload, size_t payloadLen) {
    if (payloadLen < 4) return "";
    uint32_t key = *reinterpret_cast<const uint32_t*>(payload);
    const uint8_t* cipher = payload + 4;
    size_t cipherLen = payloadLen - 4;

    std::string text;
    for (size_t i = 0; i < cipherLen; ++i) {
        key = key * 214013u + 2531011u;
        uint8_t mask = static_cast<uint8_t>((key >> 16) & 0xFF);
        uint8_t ch = cipher[i] ^ mask;
        if (ch != 0) {
            text.push_back(static_cast<char>(ch));
        }
    }
    return text;
}
```

---

## Key Node Magic Dictionary Reference
- `OBJECTDBFILE`: `40960` (`0x0000A000`)
- `OBJECTSECTION`: `45056` (`0x0000B000`)
- `UNIT`: `3149594624` (`0xBBBC0000`)
- `NID`: `45058` (`0x0000B002`)
- `OBJNAME`: `45060` (`0x0000B004`)
- `OBJPOSITION`: `45065` (`0x0000B009`)
- `UNIT_WEAPONS`: `3149594632` (`0xBBBC0008`)
- `UNIT_ARMORS`: `3149594633` (`0xBBBC0009`)
- `UNIT_SPELLS`: `3149594631` (`0xBBBC0007`)
- `UNIT_QUEST_ITEMS`: `3149594629` (`0xBBBC0005`)
- `UNIT_QUICK_ITEMS`: `3149594630` (`0xBBBC0006`)
- `SS_TEXT`: `2899242187` (`0xACD33E2B`)
- `AIGRAPH`: `826366246` (`0x31415926`)
