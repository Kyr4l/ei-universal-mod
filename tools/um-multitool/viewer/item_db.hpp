// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The "Items" gameplay database (items.idb) read straight from a database RES
// archive: database.res or databaselmp.res, whichever holds items.idb. All six
// blocks are read: Materials, Weapons, Armors, QuickItems, QuestItems, LootItems.
//
// items.idb is a tree of tagged values (docs/file-formats/database-format.md,
// "Universal Tagged-Value Primitive"): file -> block (tag = block id) -> record
// -> field (tag = field id). Field ids and meanings follow
// tools/third-party-tools/EIDBEditor_1.4.4/dbheaders.txt ("[Items]").
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cp1251.hpp"
#include "res_archive.hpp"

namespace items {

// ---------------------------------------------------------------------------
// Tagged-value primitive: a tag byte, then a length byte; an even length byte v
// means v/2 content bytes follow, an odd one means the length is the 4-byte word
// starting at that byte, (word - 1) / 2.
// ---------------------------------------------------------------------------
struct Span { uint8_t tag = 0; size_t start = 0; size_t length = 0; };

inline bool ReadTag(const uint8_t* data, size_t end, size_t& off, Span& out) {
    if (off + 2 > end) return false;
    uint8_t tag = data[off];
    uint8_t lengthByte = data[off + 1];
    size_t length, start;
    if ((lengthByte & 1) == 0) {
        length = lengthByte / 2;
        start = off + 2;
    } else {
        if (off + 5 > end) return false;
        uint32_t wide;
        std::memcpy(&wide, data + off + 1, 4);
        length = (wide - 1) / 2;
        start = off + 5;
    }
    if (start > end || length > end - start) return false;
    out = Span{tag, start, length};
    off = start + length;
    return true;
}

inline std::vector<Span> Children(const uint8_t* data, const Span& parent) {
    std::vector<Span> out;
    size_t off = parent.start, end = parent.start + parent.length;
    Span span;
    while (off < end && ReadTag(data, end, off, span)) out.push_back(span);
    return out;
}

inline std::string Cp1251ToUtf8(const uint8_t* bytes, size_t length) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (int i = 0; i < 256; ++i) t[i] = static_cast<uint32_t>(i);
        for (const auto& pair : cp1251::NonAsciiTable()) t[pair.byte] = pair.codepoint;
        return t;
    }();
    std::string out;
    for (size_t i = 0; i < length; ++i) {
        uint32_t cp = table[bytes[i]];
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// One record's raw fields, with typed readers.
struct Fields {
    const uint8_t* data = nullptr;
    std::map<uint8_t, Span> byId;

    bool Has(uint8_t id) const { return byId.count(id) != 0; }
    std::string String(uint8_t id) const {
        auto it = byId.find(id);
        if (it == byId.end()) return "";
        size_t length = it->second.length;
        while (length > 0 && data[it->second.start + length - 1] == 0) --length;
        return Cp1251ToUtf8(data + it->second.start, length);
    }
    int32_t Int(uint8_t id) const {
        auto it = byId.find(id);
        if (it == byId.end() || it->second.length < 4) return 0;
        int32_t v;
        std::memcpy(&v, data + it->second.start, 4);
        return v;
    }
    float Float(uint8_t id) const {
        auto it = byId.find(id);
        if (it == byId.end() || it->second.length < 4) return 0.0f;
        float v;
        std::memcpy(&v, data + it->second.start, 4);
        return v;
    }
    // FloatList: a bare run of packed floats, no per-element tags.
    std::vector<float> Floats(uint8_t id) const {
        std::vector<float> out;
        auto it = byId.find(id);
        if (it == byId.end()) return out;
        for (size_t i = 0; i + 4 <= it->second.length; i += 4) {
            float v;
            std::memcpy(&v, data + it->second.start + i, 4);
            out.push_back(v);
        }
        return out;
    }
    // StringList: tag-1 items, each a CP1251 string + NUL.
    std::vector<std::string> Strings(uint8_t id) const {
        std::vector<std::string> out;
        auto it = byId.find(id);
        if (it == byId.end()) return out;
        for (const Span& item : Children(data, it->second)) {
            size_t length = item.length;
            while (length > 0 && data[item.start + length - 1] == 0) --length;
            out.push_back(Cp1251ToUtf8(data + item.start, length));
        }
        return out;
    }
};

enum class Category { Weapons = 0, Armors, QuickItems, QuestItems, LootItems, Count };

// The key a category's settings are saved under, e.g. ROTATION_QUICK_ITEMS.
inline const char* CategoryKey(Category c) {
    switch (c) {
    case Category::Weapons: return "WEAPONS";
    case Category::Armors: return "ARMORS";
    case Category::QuickItems: return "QUICK_ITEMS";
    case Category::QuestItems: return "QUEST_ITEMS";
    case Category::LootItems: return "LOOT_ITEMS";
    default: return "OTHER";
    }
}

inline const char* CategoryLabel(Category c) {
    switch (c) {
    case Category::Weapons: return "Weapons";
    case Category::Armors: return "Armors";
    case Category::QuickItems: return "Quick Items";
    case Category::QuestItems: return "Quest Items";
    case Category::LootItems: return "Loot Items";
    default: return "?";
    }
}

struct Material {
    std::string name;   // e.g. "bronze"
    std::string type;   // e.g. "Metal": what an item's M.Type field refers to
    std::string code;   // 2-letter texture code, e.g. "br" (NOT unique: "dragon red bones" is "br" too)
    int id = 0;         // field 3: the number in material<NNNN>.mmp and initlimt<N>
    float price = 0, weight = 0, damage = 0;
};

// One row of Weapons/Armors/QuickItems/QuestItems/LootItems. The first 16
// fields share one layout across all five blocks; the rest depend on the block.
struct Item {
    Category category = Category::Weapons;
    int row = 0;                  // position in its block, for stable ordering
    std::string name;             // field 0
    std::string type;             // field 1: "axe", "helm", "scroll", "treasure"... (empty for quest items)
    int typeId = 0;               // field 2
    std::string materialType;     // field 3: "Metal", "Stone"... (the material class the item is made of)
    int tti = 0;                  // field 5: Texture Type Index, selects the figure/texture number
    int tti2 = 0;                 // field 6: second texture index (redress subvariant)
    float price = 0, weight = 0;  // fields 7, 8
    int size = 0;                 // field 9
    float mana = 0;               // field 10
    int slots = 0;                // field 11
    float durability = 0;         // field 12
    // Weapons
    float range = 0, dMin = 0, dMax = 0, attack = 0, defence = 0; // 23, 24, 25, 29, 30
    std::vector<float> damageProportions;                         // 26: piercing .. general
    // Armors
    std::vector<float> mainAbsorb;                                // 21: absorb, piercing .. general
    // QuickItems
    int quickItemId = 0, graphicsLevel = 0;                       // 21, 22
    float quickDamage = 0;                                        // 23
    std::string spell;                                            // 25
    // QuestItems
    int scriptId = 0;                                             // 21
    std::vector<std::string> zones;                               // 22
    // LootItems
    int lootField21 = 0, lootField22 = 0, lootField23 = 0;
};

struct Database {
    std::vector<Material> materials;
    std::array<std::vector<Item>, static_cast<size_t>(Category::Count)> items;
    std::string sourceLabel;

    const std::vector<Item>& List(Category c) const { return items[static_cast<size_t>(c)]; }

    // Materials of one class, in database order (the order the game lists them).
    std::vector<const Material*> MaterialsOfType(const std::string& type) const {
        std::vector<const Material*> out;
        for (auto& m : materials) {
            if (res::Archive::ToLower(m.type) == res::Archive::ToLower(type)) out.push_back(&m);
        }
        return out;
    }
    const Material* FindMaterial(const std::string& name) const {
        for (auto& m : materials) if (m.name == name) return &m;
        return nullptr;
    }
};

inline bool ParseItemsIdb(const std::vector<uint8_t>& idb, Database& out, std::string& err) {
    const uint8_t* data = idb.data();
    size_t off = 0;
    Span file;
    if (!ReadTag(data, idb.size(), off, file)) { err = "items.idb: unreadable header"; return false; }
    for (const Span& block : Children(data, file)) {
        for (const Span& record : Children(data, block)) {
            Fields f;
            f.data = data;
            for (const Span& field : Children(data, record)) f.byId[field.tag] = field;
            if (block.tag == 1) {
                Material m;
                m.name = f.String(0);
                m.type = f.String(1);
                m.code = f.String(2);
                m.id = f.Int(3);
                m.price = f.Float(4);
                m.weight = f.Float(5);
                m.damage = f.Float(10);
                if (!m.name.empty()) out.materials.push_back(std::move(m));
                continue;
            }
            if (block.tag < 2 || block.tag > 6) continue;
            Item it;
            it.category = static_cast<Category>(block.tag - 2);
            it.name = f.String(0);
            if (it.name.empty()) continue;
            it.type = f.String(1);
            it.typeId = f.Int(2);
            it.materialType = f.String(3);
            it.tti = f.Int(5);
            it.tti2 = f.Int(6);
            it.price = f.Float(7);
            it.weight = f.Float(8);
            it.size = f.Int(9);
            it.mana = f.Float(10);
            it.slots = f.Int(11);
            it.durability = f.Float(12);
            switch (it.category) {
            case Category::Weapons:
                it.range = f.Float(23);
                it.dMin = f.Float(24);
                it.dMax = f.Float(25);
                it.damageProportions = f.Floats(26);
                it.attack = f.Float(29);
                it.defence = f.Float(30);
                break;
            case Category::Armors:
                it.mainAbsorb = f.Floats(21);
                break;
            case Category::QuickItems:
                it.quickItemId = f.Int(21);
                it.graphicsLevel = f.Int(22);
                it.quickDamage = f.Float(23);
                it.spell = f.String(25);
                break;
            case Category::QuestItems:
                it.scriptId = f.Int(21);
                it.zones = f.Strings(22);
                break;
            case Category::LootItems:
                it.lootField21 = f.Int(21);
                it.lootField22 = f.Int(22);
                it.lootField23 = f.Int(23);
                break;
            default:
                break;
            }
            auto& list = out.items[static_cast<size_t>(it.category)];
            it.row = static_cast<int>(list.size());
            list.push_back(std::move(it));
        }
    }
    return true;
}

// database.res and databaselmp.res are both RES archives; either works as long as it holds items.idb
// (the vanilla game has it in both, Universal-Mod only in databaselmp.res).
inline bool LoadDatabaseRes(const std::vector<uint8_t>& resBytes, const std::string& label, Database& out, std::string& err) {
    res::Archive archive;
    if (!res::ParseArchive(resBytes, archive, err)) return false;
    const std::vector<uint8_t>* idb = archive.Find("items.idb");
    if (!idb) {
        err = label + " has no items.idb (it holds " + std::to_string(archive.entries.size()) +
              " other file(s)); use the database.res or databaselmp.res that contains the items";
        return false;
    }
    Database db;
    if (!ParseItemsIdb(*idb, db, err)) return false;
    db.sourceLabel = label;
    out = std::move(db);
    return true;
}

} // namespace items
