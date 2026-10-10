// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// In-game names and descriptions of items, from the game's text archives (texts.res,
// textslmp.res) or folders of loose text files (e.g. resources/universal-mod/res-texts/...).
//
// Each text is one file named by a key; its first line is the display name, the rest the
// description. Keys (case-insensitive, words of a name joined by '_'):
//   WEAPON <Blueprint> <Material>      e.g. "WEAPON Bone_Short_Bow Dry_Bones"
//   ARMOR <Blueprint> <Material>
//   QITEM <Name> [<Material>]          wands have one per material ("QITEM wand_4 adamantium")
//   QUESTITEM <Name>
//   LITEM <Name>                       treasure loot
//   MATERIAL <Name>                    material loot
//
// The language is whatever the top text source holds; the encoding is detected per text:
// UTF-8 (the mod's French texts), CP949 (Korean), CP1250 (Polish, Central European: a letter with a
// diacritic inside a Latin word), else CP1251 (the original Russian game).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "asset_source.hpp"
#include "cp1250.hpp"
#include "cp1251.hpp"
#include "cp949_table.hpp"
#include "item_db.hpp"

namespace texts {

inline void AppendUtf8(std::string& out, uint32_t cp) {
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

inline bool IsValidUtf8(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < b.size();) {
        uint8_t c = b[i];
        size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99 || i + n >= b.size()) return false;
        for (size_t k = 1; k <= n; ++k) if ((b[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

inline uint32_t Cp949(uint8_t lead, uint8_t trail) {
    if (lead < cp949::kLeadFirst || trail < cp949::kTrailFirst) return 0;
    return cp949::kTable[(lead - cp949::kLeadFirst) * cp949::kTrailCount + (trail - cp949::kTrailFirst)];
}

// Korean when every byte >= 0x80 pairs up into CP949 characters and most of them are Hangul
// syllables. Russian (CP1251) is mostly lowercase letters 0xE0..0xFF, which pair up as Hanja if
// at all, so it fails the Hangul test.
inline bool LooksKorean(const std::vector<uint8_t>& b) {
    size_t pairs = 0, hangul = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        if (b[i] < 0x80) continue;
        if (i + 1 >= b.size()) return false;
        uint32_t cp = Cp949(b[i], b[i + 1]);
        if (!cp) return false;
        ++pairs;
        if (cp >= 0xAC00 && cp <= 0xD7A3) ++hangul;
        ++i;
    }
    return pairs > 0 && hangul * 10 >= pairs * 7;
}

// Polish / Central European (CP1250) rather than Korean or Russian. CP949 accepts ASCII letters as second
// bytes, so Polish words ("śnieżnego": 0x9C 'n') can pass LooksKorean; real Korean text has both bytes of each
// character in the KS X 1001 range (>= 0xA1), so Polish only wins when the text is not like that.
inline bool LooksPolish(const std::vector<uint8_t>& b) {
    if (!cp1250::Looks(b)) return false;
    if (!LooksKorean(b)) return true;
    for (size_t i = 0; i + 1 < b.size(); ++i)
        if (b[i] >= 0x80) { if (b[i] < 0xA1 || b[i + 1] < 0xA1) return true; ++i; }
    return false;
}

// Windows-1251 bytes as UTF-8, whatever they look like.
inline std::string DecodeCp1251(const std::vector<uint8_t>& b) {
    std::string out;
    static uint32_t table[256] = {0};
    if (!table['A']) {
        for (int i = 0; i < 256; ++i) table[i] = static_cast<uint32_t>(i);
        for (const auto& pair : cp1251::NonAsciiTable()) table[pair.byte] = pair.codepoint;
    }
    for (uint8_t c : b) AppendUtf8(out, table[c]);
    return out;
}

inline std::string DecodeToUtf8(const std::vector<uint8_t>& b) {
    if (IsValidUtf8(b)) return std::string(b.begin(), b.end());
    if (LooksPolish(b)) return cp1250::ToUtf8(b);
    std::string out;
    if (LooksKorean(b)) {
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i] < 0x80) { out += static_cast<char>(b[i]); continue; }
            AppendUtf8(out, Cp949(b[i], b[i + 1]));
            ++i;
        }
        return out;
    }
    return DecodeCp1251(b);
}

// "Bone Short Bow" -> "bone_short_bow"
inline std::string KeyPart(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (c == ' ') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct ItemText {
    bool found = false;
    std::string key;         // the key looked up (the last one tried when none was found)
    std::string name;        // first line
    std::string description; // the rest, lines joined
};

// The keys to try for an item, most specific first. `material` may be null.
inline std::vector<std::string> KeysFor(const items::Item& item, const items::Material* material) {
    std::vector<std::string> keys;
    const std::string name = KeyPart(item.name);
    const std::string mat = material ? KeyPart(material->name) : "";
    switch (item.category) {
    case items::Category::Weapons: if (material) keys.push_back("weapon " + name + " " + mat); break;
    case items::Category::Armors: if (material) keys.push_back("armor " + name + " " + mat); break;
    case items::Category::QuickItems:
        if (material) keys.push_back("qitem " + name + " " + mat);
        keys.push_back("qitem " + name);
        break;
    case items::Category::QuestItems: keys.push_back("questitem " + name); break;
    case items::Category::LootItems:
        if (res::Archive::ToLower(item.type) == "material" && material) keys.push_back("material " + mat);
        else keys.push_back("litem " + name);
        break;
    default: break;
    }
    return keys;
}

// CP949 (Korean) bytes as UTF-8, whatever they look like.
inline std::string DecodeCp949(const std::vector<uint8_t>& b) {
    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (b[i] < 0x80 || i + 1 >= b.size()) { out += static_cast<char>(b[i]); continue; }
        const uint32_t cp = Cp949(b[i], b[i + 1]);
        if (!cp) { out += '?'; ++i; continue; }
        AppendUtf8(out, cp);
        ++i;
    }
    return out;
}

// Bytes as UTF-8 by a chosen encoding ("cp1251", "cp1250", "cp949"), else ("auto", "") by what they look like.
inline std::string DecodeAs(const std::vector<uint8_t>& b, const std::string& encoding) {
    if (encoding == "cp1251") return DecodeCp1251(b);
    if (encoding == "cp1250") return cp1250::ToUtf8(b);
    if (encoding == "cp949") return DecodeCp949(b);
    return DecodeToUtf8(b);
}

// `encodings`: a layer's path -> its encoding (#82: the Polish texts a mod ships as CP1250 cannot be told from
// CP1251 by their bytes), missing = auto.
inline ItemText Lookup(const LayeredAssetSource& sources, const items::Item& item, const items::Material* material,
                       const std::map<std::string, std::string>* encodings = nullptr) {
    ItemText t;
    for (const std::string& key : KeysFor(item, material)) {
        t.key = key;
        std::vector<uint8_t> bytes;
        if (!sources.ReadFile(key, bytes)) continue;
        std::string encoding;
        if (encodings) {
            const int layer = sources.LayerWith(key);
            if (layer >= 0) { auto e = encodings->find(sources.layers[static_cast<size_t>(layer)].path); if (e != encodings->end()) encoding = e->second; }
        }
        std::string text = DecodeAs(bytes, encoding);
        std::string clean;
        for (char c : text) if (c != '\r') clean += c;
        size_t nl = clean.find('\n');
        t.name = clean.substr(0, nl);
        if (nl != std::string::npos) {
            t.description = clean.substr(nl + 1);
            while (!t.description.empty() && (t.description.back() == '\n' || t.description.back() == ' ')) t.description.pop_back();
        }
        t.found = true;
        return t;
    }
    return t;
}

} // namespace texts
