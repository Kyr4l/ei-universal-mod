// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: map validation: the database names, the .mob checks, the mission script checks.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// Read one node header at offset; returns false if it doesn't fit in [0, size).
static bool ReadMobNodeHeader(const BYTE* data, size_t size, size_t offset, DWORD* type, DWORD* length) {
    if (offset + 8 > size) {
        return false;
    }
    memcpy(type, data + offset, sizeof(*type));
    memcpy(length, data + offset + 4, sizeof(*length));
    return true;
}

// Read a whole file (bounded) into a heap buffer using the real CreateFileA,
// independent of any handle the game itself has open on the same path.
static bool ReadWholeFileForDatabase(const char* path, BYTE** outData, size_t* outSize) {
    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(handle);
        return false;
    }
    DWORD toRead = static_cast<DWORD>(size.QuadPart);
    BYTE* buffer = static_cast<BYTE*>(malloc(toRead));
    DWORD bytesRead = 0;
    BOOL ok = buffer && ReadFile(handle, buffer, toRead, &bytesRead, NULL);
    CloseHandle(handle);
    if (!ok || bytesRead != toRead) {
        free(buffer);
        return false;
    }
    *outData = buffer;
    *outSize = bytesRead;
    return true;
}

// The item/spell/armor database files (database.res, databaselmp.res,
// databaseadb.res) are themselves .res archives holding records in the
// tagged-value format documented in docs/file-formats/database-format.md:
// every leaf string is [tag:1][length][cp1251 bytes + null terminator],
// where `length` is a single even byte giving 2x the payload size, or (when
// that byte is odd) the low byte of a 4-byte little-endian value giving
// 2x+1. This walks every byte position as a candidate [tag,length] header,
// and accepts it only when the payload it implies fits in the buffer and
// ends with the required null terminator - i.e. it follows the real record
// framing instead of guessing from byte values alone. The accepted name is
// the leading printable-ASCII prefix of that payload (most fields are pure
// ASCII identifiers already, but Lever "Lever Text" fields mix an ASCII
// prefix with localized Cyrillic text in the same field, so the prefix is
// taken rather than requiring the whole payload to be ASCII).
//
// This replaces an earlier version that scanned for bare printable-ASCII
// runs (any 3+ character stretch, with a defensive "drop the first
// character" fallback for when a preceding length/tag byte itself happened
// to be printable and bled into the run). That version is retired in favor
// of exact framing now that the format is fully understood; re-verified
// against every name the old heuristic ever produced for both database.res
// and databaselmp.res, confirming this version recovers the same real
// identifiers (usually as a single clean entry rather than several noisy
// off-by-one variants of one), and the only names it does NOT reproduce are
// the RES container's own internal member filenames (e.g. "acks.db") and a
// handful of coincidental byte-value matches inside unrelated binary
// fields - neither of which is a real item/spell/material identifier a
// .mob file could legitimately reference.
static void ExtractDatabaseNames(const BYTE* data, size_t size, std::unordered_set<std::string>& names) {
    static const size_t kMaxPayloadSize = 4096; // no real database field is anywhere near this long
    if (size < 2) {
        return;
    }
    for (size_t p = 0; p + 1 < size; ++p) {
        BYTE lengthByte = data[p + 1];
        size_t rawPayloadSize;
        size_t payloadStart;
        if ((lengthByte % 2) == 0) {
            rawPayloadSize = lengthByte / 2;
            payloadStart = p + 2;
        } else {
            if (p + 5 > size) {
                continue;
            }
            DWORD wideLength = 0;
            memcpy(&wideLength, data + p + 1, sizeof(wideLength));
            if ((wideLength % 2) == 0) {
                continue; // the wide form's low byte must be odd to be plausible at all
            }
            rawPayloadSize = (wideLength - 1) / 2;
            payloadStart = p + 5;
        }
        if (rawPayloadSize == 0 || rawPayloadSize > kMaxPayloadSize) {
            continue;
        }
        size_t payloadEnd = payloadStart + rawPayloadSize;
        if (payloadEnd > size || data[payloadEnd - 1] != 0x00) {
            continue;
        }

        size_t j = payloadStart;
        while (j < payloadEnd - 1 && data[j] >= 0x20 && data[j] <= 0x7E) {
            ++j;
        }
        size_t prefixLength = j - payloadStart;
        if (prefixLength >= 1) {
            std::string run(reinterpret_cast<const char*>(data + payloadStart), prefixLength);
            for (size_t k = 0; k < run.size(); ++k) {
                run[k] = static_cast<char>(tolower(static_cast<unsigned char>(run[k])));
            }
            names.insert(std::move(run));
        }
    }
}

// Load every database.res/databaselmp.res/databaseadb.res found beside the
// game executable and under each installed mod's own "res" folder, since the
// mod that ships a map is not necessarily the one that ships the database
// (the loader merges mod resources).
static void LoadDatabaseFilesFromDirectory(const char* directory, std::unordered_set<std::string>& names) {
    static const char* kDatabaseFileNames[] = { "database.res", "databaselmp.res", "databaseadb.res" };
    for (size_t i = 0; i < sizeof(kDatabaseFileNames) / sizeof(kDatabaseFileNames[0]); ++i) {
        char path[MAX_PATH] = {};
        _snprintf(path, sizeof(path) - 1, "%s\\%s", directory, kDatabaseFileNames[i]);
        BYTE* data = NULL;
        size_t size = 0;
        if (ReadWholeFileForDatabase(path, &data, &size)) {
            ExtractDatabaseNames(data, size, names);
            free(data);
        }
    }
}

// Find a "\Mods\" path component (case-insensitive) in a path and copy the
// directory up to and including "Mods" into outModsRoot. Deriving the mods
// root from the .mob file's own path is more reliable than assuming it is a
// sibling of game.exe, since the executable is not always installed at the
// root of the mod tree.
static bool FindModsRootFromPath(const char* path, char* outModsRoot, size_t outSize) {
    size_t length = strlen(path);
    for (size_t i = 0; i + 5 <= length; ++i) {
        bool isBoundaryBefore = (i == 0) || path[i - 1] == '\\' || path[i - 1] == '/';
        bool isBoundaryAfter = path[i + 4] == '\\' || path[i + 4] == '/';
        if (isBoundaryBefore && isBoundaryAfter &&
                tolower(static_cast<unsigned char>(path[i])) == 'm' &&
                tolower(static_cast<unsigned char>(path[i + 1])) == 'o' &&
                tolower(static_cast<unsigned char>(path[i + 2])) == 'd' &&
                tolower(static_cast<unsigned char>(path[i + 3])) == 's') {
            size_t modsRootLength = i + 4;
            if (modsRootLength >= outSize) {
                return false;
            }
            memcpy(outModsRoot, path, modsRootLength);
            outModsRoot[modsRootLength] = '\0';
            return true;
        }
    }
    return false;
}

// Enumerate every "<modsRoot>\<mod>\res" directory and load its database files.
static void LoadDatabaseFilesFromModsRoot(const char* modsRoot, std::unordered_set<std::string>& names) {
    char modsPattern[MAX_PATH] = {};
    _snprintf(modsPattern, sizeof(modsPattern) - 1, "%s\\*", modsRoot);
    WIN32_FIND_DATAA findData = {};
    HANDLE find = FindFirstFileA(modsPattern, &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            char modResDir[MAX_PATH] = {};
            _snprintf(modResDir, sizeof(modResDir) - 1, "%s\\%s\\res", modsRoot, findData.cFileName);
            LoadDatabaseFilesFromDirectory(modResDir, names);
        }
    } while (FindNextFileA(find, &findData));
    FindClose(find);
}

// Return the cached set of valid item/spell/armor names, loading it from disk
// on first use. Loading is attempted only once per process even if no
// database files are found, so a missing database cannot repeatedly hit disk.
// The mods root is derived from the .mob file's own path (reliable) rather
// than assumed to be beside game.exe (which may live in a different folder).
static std::unordered_set<std::string>& GetMobDatabaseNames(const char* mobPath) {
    static std::unordered_set<std::string> names;
    static bool loaded = false;
    if (loaded) {
        return names;
    }
    loaded = true;

    char modsRoot[MAX_PATH] = {};
    if (mobPath && FindModsRootFromPath(mobPath, modsRoot, sizeof(modsRoot))) {
        LoadDatabaseFilesFromModsRoot(modsRoot, names);
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
        char* slash = strrchr(exePath, '\\');
        if (slash) {
            *slash = '\0';
        }
        char baseResDir[MAX_PATH] = {};
        _snprintf(baseResDir, sizeof(baseResDir) - 1, "%s\\res", exePath);
        LoadDatabaseFilesFromDirectory(baseResDir, names);

        char exeModsRoot[MAX_PATH] = {};
        _snprintf(exeModsRoot, sizeof(exeModsRoot) - 1, "%s\\Mods", exePath);
        LoadDatabaseFilesFromModsRoot(exeModsRoot, names);
    }

    LogLine("DEBUG", "[MOBCHECK] loaded %zu candidate item/spell/armor names for database validation", names.size());
    return names;
}

// Strip a trailing "[...]" annotation (enchantment/count) and surrounding
// whitespace, e.g. "iron [prot_fire {ic; e2; d2}] " -> "iron".
static std::string StripBracketAnnotation(const std::string& value) {
    size_t bracket = value.find('[');
    std::string result = (bracket == std::string::npos) ? value : value.substr(0, bracket);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

static std::string ToLowerString(const std::string& value) {
    std::string result = value;
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

// Check a weapon/armor name ("template.material") against the database,
// tolerating a bracketed annotation on the material part.
static bool IsKnownWeaponOrArmorName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    size_t dot = lower.rfind('.');
    if (dot == std::string::npos) {
        return names.count(lower) != 0;
    }
    std::string templateName = lower.substr(0, dot);
    std::string material = StripBracketAnnotation(lower.substr(dot + 1));
    return names.count(templateName) != 0 && names.count(material) != 0;
}

// Check a spell name, stripping its "{param,...}" argument list first.
static bool IsKnownSpellName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    size_t brace = lower.find('{');
    std::string base = (brace == std::string::npos) ? lower : lower.substr(0, brace);
    return names.count(base) != 0;
}

// Check a quest/quick item name, tolerating a bracketed annotation, or
// falling back to the weapon/armor "template.material" check for items that
// use that same naming convention (e.g. "material.scrab bones [2]").
static bool IsKnownSimpleItemName(const std::string& name, const std::unordered_set<std::string>& names) {
    std::string lower = ToLowerString(name);
    if (names.count(lower) != 0 || names.count(StripBracketAnnotation(lower)) != 0) {
        return true;
    }
    if (lower.find('.') != std::string::npos) {
        return IsKnownWeaponOrArmorName(name, names);
    }
    return false;
}

// A unit's own equipment/spell/quest-item lists are stored as: a 4-byte count,
// then that many [type(4), length(4), name bytes] entries. A blank entry name
// is not a legitimate item and is a reliable sign of a corrupted item list;
// this was confirmed by comparing a known-good and a known-bad copy of the
// same map (the bad copy had one blank armor slot appended) and by scanning
// every real .mob file shipped with the mod, none of which has a blank entry.
// When enabled, non-blank entries are additionally checked against the
// item/spell database so a typo'd or made-up name is reported before the
// game can crash on it, instead of only catching blanks; this is always on
// alongside structural validation, since checking structure without also
// checking item/spell/armor names against the database misses most real bugs.
static void ValidateMobStringArray(const char* path, const BYTE* data, size_t payloadStart,
        size_t payloadEnd, const char* fieldName, int fieldKind, const char* unitLabel, bool* hasError) {
    if (payloadStart + 4 > payloadEnd) {
        return;
    }
    DWORD count = 0;
    memcpy(&count, data + payloadStart, sizeof(count));
    size_t pos = payloadStart + 4;
    for (DWORD i = 0; i < count; ++i) {
        DWORD entryType = 0, entryLength = 0;
        if (pos + 8 > payloadEnd) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a truncated %s list (expected %lu entries)",
                path, unitLabel, fieldName, count);
            *hasError = true;
            return;
        }
        memcpy(&entryType, data + pos, sizeof(entryType));
        memcpy(&entryLength, data + pos + 4, sizeof(entryLength));
        if (entryLength < 8 || pos + entryLength > payloadEnd) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a %s entry with an invalid length %lu",
                path, unitLabel, fieldName, entryLength);
            *hasError = true;
            return;
        }
        if (entryLength == 8) {
            LogLine("ERROR", "[MOBCHECK] %s unit %s has a blank entry (#%lu of %lu) in its %s list",
                path, unitLabel, i + 1, count, fieldName, fieldName);
            *hasError = true;
        } else {
            std::string entryName(reinterpret_cast<const char*>(data + pos + 8), entryLength - 8);
            const std::unordered_set<std::string>& databaseNames = GetMobDatabaseNames(path);
            bool known;
            if (fieldKind == 0) known = IsKnownWeaponOrArmorName(entryName, databaseNames);
            else if (fieldKind == 1) known = IsKnownSpellName(entryName, databaseNames);
            else known = IsKnownSimpleItemName(entryName, databaseNames);
            if (!known) {
                LogLine("ERROR", "[MOBCHECK] %s unit %s has a %s entry '%s' that does not exist in the item/spell database",
                    path, unitLabel, fieldName, entryName.c_str());
                *hasError = true;
            }
        }
        pos += entryLength;
    }
}

// Walk a single UNIT record's fields, checking its equipment/spell/quest lists.
// Fields are not stored in a fixed order (NID/OBJ_NAME can appear before or
// after the item lists), so the unit's name/NID are collected in a first pass
// before the item lists are validated in a second pass, so errors can always
// name the unit instead of only reporting a raw file offset.
static void ValidateMobUnit(const char* path, const BYTE* data, size_t unitStart, size_t unitEnd, bool* hasError) {
    char unitLabel[160] = {};
    strcpy(unitLabel, "(unnamed)");
    DWORD nid = 0;
    bool haveNid = false;
    size_t pos = unitStart + 8;
    while (pos + 8 <= unitEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, unitEnd, pos, &type, &length) || length < 8 || pos + length > unitEnd) {
            break;
        }
        if (type == kMobTypeNid && length == 12) {
            memcpy(&nid, data + pos + 8, sizeof(nid));
            haveNid = true;
        } else if (type == kMobTypeObjName) {
            size_t nameLength = length - 8;
            if (nameLength >= sizeof(unitLabel)) {
                nameLength = sizeof(unitLabel) - 1;
            }
            memcpy(unitLabel, data + pos + 8, nameLength);
            unitLabel[nameLength] = '\0';
        }
        pos += length;
    }

    char labelWithNid[192] = {};
    if (haveNid) {
        _snprintf(labelWithNid, sizeof(labelWithNid) - 1, "'%s' (NID %lu)", unitLabel, nid);
    } else {
        _snprintf(labelWithNid, sizeof(labelWithNid) - 1, "'%s'", unitLabel);
    }

    pos = unitStart + 8;
    while (pos + 8 <= unitEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, unitEnd, pos, &type, &length) || length < 8 || pos + length > unitEnd) {
            return;
        }
        const char* fieldName = NULL;
        int fieldKind = 2; // 0=weapon/armor (template.material), 1=spell ({params}), 2=quest/quick (simple)
        if (type == kMobTypeUnitQuestItems) fieldName = "quest item";
        else if (type == kMobTypeUnitQuickItems) fieldName = "quick item";
        else if (type == kMobTypeUnitSpells) { fieldName = "spell"; fieldKind = 1; }
        else if (type == kMobTypeUnitWeapons) { fieldName = "weapon"; fieldKind = 0; }
        else if (type == kMobTypeUnitArmors) { fieldName = "armor"; fieldKind = 0; }
        if (fieldName) {
            ValidateMobStringArray(path, data, pos + 8, pos + length, fieldName, fieldKind, labelWithNid, hasError);
        }
        pos += length;
    }
}

// Walk the OBJECT_SECTION node's direct children and validate every UNIT found.
static void ValidateMobObjectSection(const char* path, const BYTE* data, size_t sectionStart, size_t sectionEnd, bool* hasError) {
    size_t pos = sectionStart + 8;
    while (pos + 8 <= sectionEnd) {
        DWORD type = 0, length = 0;
        if (!ReadMobNodeHeader(data, sectionEnd, pos, &type, &length) || length < 8 || pos + length > sectionEnd) {
            LogLine("ERROR", "[MOBCHECK] %s has a corrupted object entry inside OBJECT_SECTION", path);
            *hasError = true;
            return;
        }
        if (type == kMobTypeUnit) {
            ValidateMobUnit(path, data, pos, pos + length, hasError);
        }
        pos += length;
    }
}

// ---------------------------------------------------------------------------
// Mission script checking
//
// A .mob carries its mission script in an SS_TEXT node (encrypted; see
// docs/file-formats/mob-format.md). The game loads a QUEST map (zNqM.mob) on top
// of its zone's base map (zoneN-lmp.mob) and the quest's script freely uses the
// base map's variables, so a quest is checked together with its base: the quest's
// own zNqM.mq archive holds a map.txt whose "#res" line names the zone's base map.
// Object IDs must not repeat between the two - the quest's copy silently replaces
// the base map's object - which is reported too.
// ---------------------------------------------------------------------------
static const DWORD kMobTypeSsTextOld = 2899242186u; // plain-text script (older files)
static const DWORD kMobTypeSsText = 2899242187u;    // encrypted script
static const DWORD kMqMagic = 0x019CE23Cu;
static const size_t kMqMaxBytes = 8u * 1024 * 1024;

// MSVC LCG XOR cipher over everything after the 4-byte key; NUL bytes are padding.
static std::string DecryptMobScript(const BYTE* payload, size_t length) {
    std::string text;
    if (length < 4) {
        return text;
    }
    DWORD key = 0;
    memcpy(&key, payload, sizeof(key));
    text.reserve(length - 4);
    for (size_t i = 4; i < length; ++i) {
        key = key * 214013u + 2531011u;
        BYTE plain = static_cast<BYTE>(payload[i] ^ ((key >> 16) & 0xFF));
        if (plain != 0) {
            text.push_back(static_cast<char>(plain));
        }
    }
    return text;
}

// Pull the mission script text and every object's NID out of a .mob image.
static void ExtractMobScriptAndIds(const BYTE* data, size_t size, std::string* scriptText,
        std::unordered_set<DWORD>* objectIds, std::unordered_set<std::string>* objectNames = nullptr) {
    size_t pos = 16; // past the root node and the marker node, like ValidateMobFile
    while (pos + 8 <= size) {
        DWORD type = 0, length = 0;
        memcpy(&type, data + pos, sizeof(type));
        memcpy(&length, data + pos + 4, sizeof(length));
        if (type == kMobTypeRoot || length < 8 || pos + length > size) {
            break;
        }
        if (type == kMobTypeSsText) {
            *scriptText = DecryptMobScript(data + pos + 8, length - 8);
        } else if (type == kMobTypeSsTextOld) {
            scriptText->clear();
            for (size_t i = 0; i < length - 8; ++i) {
                if (data[pos + 8 + i] != 0) scriptText->push_back(static_cast<char>(data[pos + 8 + i]));
            }
        } else if (type == kMobTypeObjectSection) {
            size_t sectionEnd = pos + length;
            size_t child = pos + 8;
            while (child + 8 <= sectionEnd) {
                DWORD childType = 0, childLength = 0;
                memcpy(&childType, data + child, sizeof(childType));
                memcpy(&childLength, data + child + 4, sizeof(childLength));
                if (childLength < 8 || child + childLength > sectionEnd) {
                    break;
                }
                size_t field = child + 8;
                size_t childEnd = child + childLength;
                while (field + 8 <= childEnd) {
                    DWORD fieldType = 0, fieldLength = 0;
                    memcpy(&fieldType, data + field, sizeof(fieldType));
                    memcpy(&fieldLength, data + field + 4, sizeof(fieldLength));
                    if (fieldLength < 8 || field + fieldLength > childEnd) {
                        break;
                    }
                    if (fieldType == kMobTypeNid && fieldLength == 12) {
                        DWORD nid = 0;
                        memcpy(&nid, data + field + 8, sizeof(nid));
                        objectIds->insert(nid);
                    } else if (fieldType == kMobTypeObjName && objectNames) {
                        std::string objectName(reinterpret_cast<const char*>(data + field + 8), fieldLength - 8);
                        objectName.resize(strlen(objectName.c_str())); // stop at the terminating NUL
                        for (size_t i = 0; i < objectName.size(); ++i) {
                            objectName[i] = static_cast<char>(tolower(static_cast<unsigned char>(objectName[i])));
                        }
                        objectNames->insert(objectName);
                    }
                    field += fieldLength;
                }
                child = childEnd;
            }
        }
        pos += length;
    }
}

static bool ReadWholeFile(const std::string& path, size_t limitBytes, std::vector<BYTE>* out) {
    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
        static_cast<ULONGLONG>(size.QuadPart) <= limitBytes;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, out->data(), static_cast<DWORD>(out->size()), &read, NULL) && read == out->size();
    }
    CloseHandle(handle);
    return ok;
}

// The base map's name from a quest archive's map.txt ("#res <mpr> <base mob>"), without extension.
bool ReadMqBaseMapName(const std::string& mqPath, std::string* baseName, std::string* mprName) {
    std::vector<BYTE> file;
    if (!ReadWholeFile(mqPath, kMqMaxBytes, &file) || file.size() < 16) {
        return false;
    }
    DWORD magic = 0, count = 0, tableOffset = 0, namesLength = 0;
    memcpy(&magic, file.data(), 4);
    memcpy(&count, file.data() + 4, 4);
    memcpy(&tableOffset, file.data() + 8, 4);
    memcpy(&namesLength, file.data() + 12, 4);
    if (magic != kMqMagic || count == 0 || count > 65536) {
        return false;
    }
    // Each descriptor is 22 bytes: next(4) length(4) offset(4) timestamp(4) nameLength(2) nameOffset(4).
    unsigned long long namesOffset = static_cast<unsigned long long>(tableOffset) + static_cast<unsigned long long>(count) * 22;
    if (namesOffset + namesLength > file.size()) {
        return false;
    }
    for (DWORD i = 0; i < count; ++i) {
        const BYTE* descriptor = file.data() + tableOffset + static_cast<size_t>(i) * 22;
        DWORD dataLength = 0, dataOffset = 0, nameOffset = 0;
        WORD nameLength = 0;
        memcpy(&dataLength, descriptor + 4, 4);
        memcpy(&dataOffset, descriptor + 8, 4);
        memcpy(&nameLength, descriptor + 16, 2);
        memcpy(&nameOffset, descriptor + 18, 4);
        if (nameLength < 7 || static_cast<unsigned long long>(nameOffset) + nameLength > namesLength) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(file.data() + namesOffset + nameOffset), nameLength);
        if (!EqualsIgnoreCase(name.substr(name.size() - 7).c_str(), "map.txt")) {
            continue;
        }
        if (static_cast<unsigned long long>(dataOffset) + dataLength > file.size()) {
            return false;
        }
        std::string text(reinterpret_cast<const char*>(file.data() + dataOffset), dataLength);
        size_t at = 0;
        bool afterRes = false;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            if (afterRes) {
                size_t space = line.find_first_of(" \t");
                if (space == std::string::npos) return false;
                if (mprName) *mprName = line.substr(0, space);
                size_t second = line.find_first_not_of(" \t", space);
                if (second == std::string::npos) return false;
                size_t secondEnd = line.find_first_of(" \t", second);
                *baseName = line.substr(second, secondEnd == std::string::npos ? std::string::npos : secondEnd - second);
                return !baseName->empty();
            }
            if (line.size() >= 4 && EqualsIgnoreCase(line.substr(0, 4).c_str(), "#res")) {
                afterRes = true;
            }
        }
        return false;
    }
    return false;
}


// Where the game itself opened each .mob (lower-case file name -> full path). A quest's base
// map is opened by the game just before the quest, often from a different folder (a mod's
// quests sit in the mod's maps folder, the zone's base map in the game's own), so this is
// how it is found when it is not next to the quest.
static volatile LONG g_seenMobPathsLock = 0;
static std::unordered_map<std::string, std::string> g_seenMobPaths;

std::string LowerCaseCopy(const std::string& text) {
    std::string result = text;
    for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    return result;
}

void RememberSeenMobPath(const char* path) {
    std::string full = path;
    size_t slash = full.find_last_of("\\/");
    std::string name = LowerCaseCopy(slash == std::string::npos ? full : full.substr(slash + 1));
    while (InterlockedCompareExchange(&g_seenMobPathsLock, 1, 0) != 0) Sleep(0);
    g_seenMobPaths[name] = full;
    InterlockedExchange(&g_seenMobPathsLock, 0);
}

static std::string FindSeenMobPath(const std::string& mobFileName) {
    std::string name = LowerCaseCopy(mobFileName);
    while (InterlockedCompareExchange(&g_seenMobPathsLock, 1, 0) != 0) Sleep(0);
    auto it = g_seenMobPaths.find(name);
    std::string result = it != g_seenMobPaths.end() ? it->second : std::string();
    InterlockedExchange(&g_seenMobPathsLock, 0);
    return result;
}

// Base maps are read on demand (whether or not the game opened them yet) and kept.
static volatile LONG g_mobContextLock = 0;
static std::unordered_map<std::string, std::shared_ptr<MobScriptContext>> g_mobContextCache;

std::shared_ptr<MobScriptContext> LoadMobContext(const std::string& mobPath) {
    std::string key = mobPath;
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
    while (InterlockedCompareExchange(&g_mobContextLock, 1, 0) != 0) Sleep(0);
    auto cached = g_mobContextCache.find(key);
    std::shared_ptr<MobScriptContext> result = cached != g_mobContextCache.end() ? cached->second : nullptr;
    InterlockedExchange(&g_mobContextLock, 0);
    if (result) {
        return result;
    }
    std::vector<BYTE> file;
    if (!ReadWholeFile(mobPath, kMobMaxWalkBytes, &file) || file.size() < 16) {
        return nullptr;
    }
    DWORD rootType = 0;
    memcpy(&rootType, file.data(), sizeof(rootType));
    if (rootType != kMobTypeObjectDbFile) {
        return nullptr;
    }
    result = std::make_shared<MobScriptContext>();
    std::string scriptText;
    ExtractMobScriptAndIds(file.data(), file.size(), &scriptText, &result->objectIds, &result->objectNames);
    if (!scriptText.empty()) {
        MobScriptReport scriptReport = CheckMobScript(scriptText);
        result->declarations = scriptReport.declarations;
        for (const std::string& target : scriptReport.addMobs) {
            std::string name = LowerCaseCopy(target);
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".mob") != 0) name += ".mob";
            result->addMobs.push_back(name);
        }
    }
    while (InterlockedCompareExchange(&g_mobContextLock, 1, 0) != 0) Sleep(0);
    g_mobContextCache[key] = result;
    InterlockedExchange(&g_mobContextLock, 0);
    return result;
}

// Quest maps are named z<zone>q<n> (z12q2, z3xq1, z11d2q1, z0jq1); base maps are zone<N>-lmp
// and the like. A quest whose archive (and so its base map) cannot be found must not be
// held to "every object it mentions is in this file".
static bool LooksLikeQuestMapName(const std::string& fileName) {
    std::string name = LowerCaseCopy(fileName);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.size() < 4 || name[0] != 'z' || name[1] < '0' || name[1] > '9') {
        return false;
    }
    size_t q = name.find_last_of('q');
    if (q == std::string::npos || q < 2 || q + 1 >= name.size()) {
        return false;
    }
    for (size_t i = q + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static std::string MobFileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Is this text argument an item/spell the database knows? Same shape rule as the
// unit weapon/armor check: drop a spell's "{parameters}" and an item's "[count]",
// then every dot-separated part must be a database name ("material.iron[1]").
static bool ScriptDatabaseNameKnown(const std::string& raw, const std::unordered_set<std::string>& names) {
    std::string text = LowerCaseCopy(raw);
    if (names.count(text) != 0) {
        return true;
    }
    size_t brace = text.find('{');
    if (brace != std::string::npos) text.resize(brace);
    size_t bracket = text.find('[');
    if (bracket != std::string::npos) text.resize(bracket);
    size_t start = 0;
    bool any = false;
    while (start <= text.size()) {
        size_t dot = text.find('.', start);
        std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) {
            any = true;
            if (names.count(part) == 0) return false;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return any;
}

// What a script refers to inside the map, checked against the map's own objects: the IDs
// and names of this map, its base map (for a quest) and every map the script loads with
// AddMob(). Findings are summarised per script, since a stale reference usually repeats.
// Skipped whenever one of those maps could not be read, so a map that simply is not
// available never produces a false warning.
static void ValidateScriptAgainstMap(const char* path, const std::string& fileName, const MobScriptReport& report,
        const std::unordered_set<DWORD>& ownIds, const std::unordered_set<std::string>& ownNames,
        const std::shared_ptr<MobScriptContext>& base, bool baseMissing, bool* hasError) {
    (void)hasError;
    // --- item / spell names against the database
    const std::unordered_set<std::string>& databaseNames = GetMobDatabaseNames(path);
    if (!databaseNames.empty()) {
        std::unordered_set<std::string> reported;
        for (const MobScriptDatabaseName& ref : report.databaseNames) {
            if (ScriptDatabaseNameKnown(ref.text, databaseNames) || !reported.insert(LowerCaseCopy(ref.text)).second) {
                continue;
            }
            LogLine("WARN", "[MOBCHECK] %s script line %d: %s(): '%s' is not %s in the database", fileName.c_str(),
                ref.line, ref.command.c_str(), ref.text.c_str(), ref.kind == 's' ? "a spell" : "an item");
        }
    }

    // --- object IDs and names against the map's objects
    if (baseMissing || (report.objectIds.empty() && report.objectNames.empty())) {
        return;
    }
    std::unordered_set<DWORD> ids = ownIds;
    std::unordered_set<std::string> names = ownNames;
    if (base) {
        ids.insert(base->objectIds.begin(), base->objectIds.end());
        names.insert(base->objectNames.begin(), base->objectNames.end());
    }
    std::string directory;
    size_t slash = std::string(path).find_last_of("\\/");
    if (slash != std::string::npos) directory = std::string(path).substr(0, slash + 1);
    for (const std::string& target : report.addMobs) {
        std::string name = target;
        if (name.size() < 4 || !EqualsIgnoreCase(name.substr(name.size() - 4).c_str(), ".mob")) name += ".mob";
        std::shared_ptr<MobScriptContext> added = LoadMobContext(directory + name);
        if (!added) {
            std::string seen = FindSeenMobPath(name);
            if (!seen.empty()) added = LoadMobContext(seen);
        }
        if (!added) {
            LogLine("DEBUG", "[MOBCHECK] %s loads '%s' with AddMob, which was not found; object IDs and names "
                "in its script are not checked", fileName.c_str(), target.c_str());
            return;
        }
        ids.insert(added->objectIds.begin(), added->objectIds.end());
        names.insert(added->objectNames.begin(), added->objectNames.end());
    }

    char examples[200] = {};
    size_t used = 0;
    int firstLine = 0;
    int missingIds = 0;
    std::unordered_set<std::string> seenIds;
    for (const MobScriptReference& ref : report.objectIds) {
        unsigned long id = strtoul(ref.text.c_str(), NULL, 10);
        if (ref.text.size() > 10 || ids.count(static_cast<DWORD>(id)) != 0 || !seenIds.insert(ref.text).second) {
            continue;
        }
        if (missingIds == 0) firstLine = ref.line;
        if (missingIds < 5) {
            int wrote = snprintf(examples + used, sizeof(examples) - used, "%s%s", missingIds ? ", " : "", ref.text.c_str());
            if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(examples) - used) used += static_cast<size_t>(wrote);
        }
        ++missingIds;
    }
    if (missingIds > 0) {
        LogLine("WARN", "[MOBCHECK] %s script line %d: refers to %d object ID(s) that no object has in this map, its base "
            "map or the maps it loads with AddMob (e.g. %s); commands using them get no object", fileName.c_str(),
            firstLine, missingIds, examples);
    }

    char nameExamples[200] = {};
    used = 0;
    int missingNames = 0;
    firstLine = 0;
    for (const MobScriptReference& ref : report.objectNames) {
        if (names.count(LowerCaseCopy(ref.text)) != 0) {
            continue;
        }
        if (missingNames == 0) firstLine = ref.line;
        if (missingNames < 5) {
            int wrote = snprintf(nameExamples + used, sizeof(nameExamples) - used, "%s'%s'", missingNames ? ", " : "", ref.text.c_str());
            if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(nameExamples) - used) used += static_cast<size_t>(wrote);
        }
        ++missingNames;
    }
    if (missingNames > 0) {
        LogLine("WARN", "[MOBCHECK] %s script line %d: uses %d object name(s) that no object is called in this map, its "
            "base map or the maps it loads with AddMob: %s", fileName.c_str(), firstLine, missingNames, nameExamples);
    }
}

// Check the mission script of one .mob image (already validated structurally) and, for a
// quest map, its object IDs against its base map. Findings are logged; errors set *hasError.
static void ValidateMobScript(const char* path, const BYTE* data, size_t size, bool* hasError) {
    try {
        std::string mobPath = path;
        std::string fileName = MobFileNameOf(mobPath);
        std::string scriptText;
        std::unordered_set<DWORD> objectIds;
        std::unordered_set<std::string> objectNamesOfThisMap;
        ExtractMobScriptAndIds(data, size, &scriptText, &objectIds, &objectNamesOfThisMap);

        // A quest map has a sibling .mq archive naming its base map.
        std::shared_ptr<MobScriptContext> base;
        std::string baseName;
        bool baseMissing = false;
        size_t dot = mobPath.find_last_of('.');
        if (dot != std::string::npos) {
            std::string mqPath = mobPath.substr(0, dot) + ".mq";
            if (GetFileAttributesA(mqPath.c_str()) != INVALID_FILE_ATTRIBUTES && ReadMqBaseMapName(mqPath, &baseName)) {
                size_t slash = mobPath.find_last_of("\\/");
                std::string directory = slash == std::string::npos ? std::string() : mobPath.substr(0, slash + 1);
                base = LoadMobContext(directory + baseName + ".mob");
                if (!base) {
                    std::string seen = FindSeenMobPath(baseName + ".mob");
                    if (!seen.empty()) {
                        base = LoadMobContext(seen);
                    }
                }
                if (!base) {
                    baseMissing = true;
                    LogLine("DEBUG", "[MOBCHECK] %s is a quest map for base map '%s', which was not found next to it or opened by the "
                        "game before it; variables it takes from the base map cannot be resolved", fileName.c_str(), baseName.c_str());
                }
            }
        }

        if (!base && !baseMissing && LooksLikeQuestMapName(fileName)) {
            baseMissing = true; // a quest map whose archive (and base map) is not available
        }

        if (scriptText.empty()) {
            LogLine("DEBUG", "[MOBCHECK] %s has no mission script", fileName.c_str());
        } else {
            MobScriptReport report = CheckMobScript(scriptText, base ? &base->declarations : nullptr, baseMissing);
            if (report.errors > 0) {
                *hasError = true;
            }
            std::stable_sort(report.issues.begin(), report.issues.end(),
                [](const MobScriptIssue& x, const MobScriptIssue& y) { return x.line < y.line; });
            for (const MobScriptIssue& issue : report.issues) {
                const char* level = issue.severity == 'E' ? "ERROR" : issue.severity == 'W' ? "WARN" : "DEBUG";
                LogLine(level, "[MOBCHECK] %s script line %d: %s", fileName.c_str(), issue.line, issue.message.c_str());
            }
            if (report.suppressed > 0) {
                LogLine("WARN", "[MOBCHECK] %s script: %d more finding(s) not shown", fileName.c_str(), report.suppressed);
            }
            ValidateScriptAgainstMap(path, fileName, report, objectIds, objectNamesOfThisMap, base,
                baseMissing, hasError);
            if (report.errors > 0 || report.warnings > 0) {
                LogLine("INFO", "[MOBCHECK] %s script: %d error(s), %d warning(s) in %d script(s); line numbers count "
                    "lines of the map's script text (um-multitool mobdump writes it as %s.eis)",
                    fileName.c_str(), report.errors, report.warnings, report.scriptCount,
                    fileName.substr(0, fileName.find_last_of('.')).c_str());
            } else {
                LogLine("DEBUG", "[MOBCHECK] %s script checked OK (%d script(s)%s%s)", fileName.c_str(), report.scriptCount,
                    base ? ", with base map " : "", base ? baseName.c_str() : "");
            }
        }

        if (base && !objectIds.empty()) {
            std::vector<DWORD> shared;
            for (DWORD id : objectIds) {
                if (base->objectIds.count(id) != 0) shared.push_back(id);
            }
            if (!shared.empty()) {
                std::sort(shared.begin(), shared.end());
                char examples[160] = {};
                size_t used = 0;
                for (size_t i = 0; i < shared.size() && i < 5; ++i) {
                    int wrote = snprintf(examples + used, sizeof(examples) - used, "%s%lu", i ? ", " : "",
                        static_cast<unsigned long>(shared[i]));
                    if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(examples) - used) break;
                    used += static_cast<size_t>(wrote);
                }
                LogLine("WARN", "[MOBCHECK] %s shares %lu object ID(s) with its base map %s.mob (e.g. %s); the quest map's "
                    "object replaces the base map's object with the same ID", fileName.c_str(),
                    static_cast<unsigned long>(shared.size()), baseName.c_str(), examples);
            }
        }
    } catch (...) {
        LogLine("WARN", "[MOBCHECK] script check of %s failed unexpectedly and was skipped", path);
    }
}

// Return whether this exact path has already been through ValidateMobFile
// during this process's lifetime, recording it if not.
static bool HasMobFileAlreadyBeenValidated(const char* path) {
    static std::unordered_set<std::string> validatedPaths;
    std::string key(path);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
    }
    if (validatedPaths.count(key) != 0) {
        return true;
    }
    validatedPaths.insert(key);
    return false;
}

// Read and sanity-check a .mob file without disturbing the game's own file
// handle/position; a second, independent handle is opened for this purpose.
// The node format (type:4, length:4, length includes the 8-byte header) and
// the type IDs used below come directly from ei_maper's own reader
// (util::CMobParser / CMob::deserialize), not from guesswork. Only clearly
// invalid data is flagged as an error: a root length that doesn't match the
// actual file size is common (the file can be extended, or resaved by the
// navmesh generator, after the root length was written) and is only DEBUG.
// Lower-case, backslash-separated, absolute form of a path, for comparing folders.
static std::string NormalizedFullPath(const char* path) {
    char full[MAX_PATH] = {};
    DWORD length = GetFullPathNameA(path, sizeof(full), full, NULL);
    std::string result = (length > 0 && length < sizeof(full)) ? std::string(full) : std::string(path);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = result[i] == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

// Is this file inside the folder of the mod that owns this um.dll ("...\Mods\Universal-Mod\")?
// The game opens other mods' maps and the base game's too; only this mod's are validated.
static bool IsInsideOwnModFolder(const char* path) {
    static std::string modFolder;
    if (modFolder.empty()) {
        char dllPath[MAX_PATH] = {};
        if (g_dllModule && GetModuleFileNameA(g_dllModule, dllPath, sizeof(dllPath)) != 0) {
            char* slash = strrchr(dllPath, '\\');
            if (slash) *slash = '\0';
            modFolder = NormalizedFullPath(dllPath) + "\\";
        }
    }
    if (modFolder.empty()) {
        return true; // cannot tell where this DLL lives: better to validate than to silently skip
    }
    return NormalizedFullPath(path).compare(0, modFolder.size(), modFolder) == 0;
}

void ValidateMobFile(const char* path) {
    if (!g_enableMobValidation || !path || !HasFileExtension(path, "mob")) {
        return;
    }
    if (!IsInsideOwnModFolder(path)) {
        return; // another mod's map (or the base game's) that the game also opens: not this mod's business
    }
    if (HasMobFileAlreadyBeenValidated(path)) {
        // The game routinely opens the same .mob file twice in a row (a size
        // probe followed by the real read); validating it again would only
        // duplicate identical log entries since the file cannot have changed.
        return;
    }

    HANDLE handle = g_originalCreateFileA ?
        g_originalCreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) :
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return; // cannot inspect the file; not itself a validation failure
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(handle, &fileSize)) {
        CloseHandle(handle);
        return;
    }

    ULONGLONG actualSize = static_cast<ULONGLONG>(fileSize.QuadPart);
    DWORD bytesToRead = static_cast<DWORD>(actualSize < kMobMaxWalkBytes ? actualSize : kMobMaxWalkBytes);
    BYTE* buffer = static_cast<BYTE*>(malloc(bytesToRead > 0 ? bytesToRead : 1));
    DWORD bytesRead = 0;
    BOOL readOk = buffer && ReadFile(handle, buffer, bytesToRead, &bytesRead, NULL);
    CloseHandle(handle);
    if (!buffer) {
        return;
    }

    if (!readOk || bytesRead < 16) {
        LogLine("ERROR", "[MOBCHECK] %s is only %llu bytes, too small to contain a valid .mob header",
            path, actualSize);
        free(buffer);
        return;
    }

    DWORD rootType = 0;
    memcpy(&rootType, buffer + 0, sizeof(rootType));
    if (rootType != kMobTypeObjectDbFile) {
        LogLine("ERROR", "[MOBCHECK] %s has an unexpected root node type %lu (expected %lu); file may be corrupted or is not a .mob file",
            path, rootType, kMobTypeObjectDbFile);
        free(buffer);
        return;
    }
    // The root node's declared length routinely differs from the actual file
    // size (maps get resaved/extended after it was last written), so it is
    // not a useful corruption signal and is intentionally not logged here.

    // Force the (once-per-process) database load to happen right here rather
    // than lazily on the first field that needs it: otherwise its "loaded N
    // names" log line lands wherever that first lookup happens to occur,
    // which could be partway through this file, after this one, or never at
    // all if this particular file has no items/spells/armors to check.
    GetMobDatabaseNames(path);

    if (actualSize > kMobMaxWalkBytes) {
        free(buffer);
        return;
    }

    // Skip the root node and the SC_/PR_OBJECT_DB_FILE marker (fixed 8 bytes
    // each; ei_maper's own reader does not use their declared lengths either)
    // then walk sibling top-level nodes purely by declared length until the
    // terminating ROOT node or a length that would run past the file.
    size_t pos = 16;
    bool objectSectionSeen = false;
    bool hasError = false;
    while (pos + 8 <= bytesRead) {
        DWORD type = 0, length = 0;
        memcpy(&type, buffer + pos, sizeof(type));
        memcpy(&length, buffer + pos + 4, sizeof(length));
        if (type == kMobTypeRoot) {
            break;
        }
        if (length < 8 || pos + length > bytesRead) {
            LogLine("ERROR", "[MOBCHECK] %s has a node declaring a length of %lu bytes that extends past the end of the file; the file is likely truncated or corrupted",
                path, length);
            free(buffer);
            return;
        }
        if (type == kMobTypeObjectSection) {
            objectSectionSeen = true;
            ValidateMobObjectSection(path, buffer, pos, pos + length, &hasError);
        }
        pos += length;
    }
    ValidateMobScript(path, buffer, bytesRead, &hasError);
    if (!objectSectionSeen) {
        LogLine("DEBUG", "[MOBCHECK] %s has no OBJECT_SECTION node (placement-only or menu mob?)", path);
    } else if (!hasError) {
        LogLine("DEBUG", "[MOBCHECK] %s validated OK", path);
    }
    free(buffer);
}

