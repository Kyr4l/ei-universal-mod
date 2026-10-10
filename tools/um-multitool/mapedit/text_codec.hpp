// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Quest texts (briefings, map.txt, quest.ini) are edited as UTF-8 and saved back in the encoding the
// file had: UTF-8 (the mod's French texts), CP949 (Korean) or CP1251 (the game's own, Russian and
// English). Detection is the one the 3D Viewer uses for item texts (item_texts.hpp).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cp1251.hpp"
#include "../cp1250.hpp"
#include "../viewer/item_texts.hpp"

namespace codec {

enum class Encoding { Cp1251, Utf8, Cp949, Cp1250 };

inline const char* EncodingName(Encoding e) {
    return e == Encoding::Utf8 ? "UTF-8" : e == Encoding::Cp949 ? "CP949 (Korean)" : e == Encoding::Cp1250 ? "CP1250 (Polish)" : "CP1251";
}

// Plain ASCII counts as CP1251, the game's own encoding.
inline Encoding Detect(const std::vector<uint8_t>& b) {
    bool ascii = true;
    for (uint8_t c : b) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return Encoding::Cp1251;
    if (texts::IsValidUtf8(b)) return Encoding::Utf8;
    if (texts::LooksPolish(b)) return Encoding::Cp1250;
    if (texts::LooksKorean(b)) return Encoding::Cp949;
    return Encoding::Cp1251;
}

inline std::string ToUtf8(const std::vector<uint8_t>& b, Encoding e) {
    if (e == Encoding::Utf8) return std::string(b.begin(), b.end());
    if (e == Encoding::Cp1250) return cp1250::ToUtf8(b);
    if (e == Encoding::Cp1251) return texts::DecodeCp1251(b);
    if (e == Encoding::Cp949) return texts::DecodeToUtf8(b);
    return std::string(b.begin(), b.end());
}

// False (with the first character that does not fit) when the text cannot be written in `e`.
inline bool FromUtf8(const std::string& text, Encoding e, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (e == Encoding::Utf8) { out.assign(text.begin(), text.end()); return true; }
    static std::unordered_map<uint32_t, uint16_t> cp949Reverse;
    if (e == Encoding::Cp949 && cp949Reverse.empty()) {
        for (uint32_t lead = cp949::kLeadFirst; lead <= 0xFE; ++lead)
            for (uint32_t trail = cp949::kTrailFirst; trail < cp949::kTrailFirst + cp949::kTrailCount; ++trail) {
                uint32_t cp = texts::Cp949(static_cast<uint8_t>(lead), static_cast<uint8_t>(trail));
                if (cp && !cp949Reverse.count(cp)) cp949Reverse[cp] = static_cast<uint16_t>((lead << 8) | trail);
            }
    }
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = cp1251::DecodeUtf8(text, i);
        if (cp < 0x80) { out.push_back(static_cast<uint8_t>(cp)); continue; }
        if (e == Encoding::Cp949) {
            auto it = cp949Reverse.find(cp);
            if (it == cp949Reverse.end()) { err = "a character (U+" + std::to_string(cp) + ") that CP949 cannot hold"; return false; }
            out.push_back(static_cast<uint8_t>(it->second >> 8));
            out.push_back(static_cast<uint8_t>(it->second & 0xFF));
        } else if (e == Encoding::Cp1250) {
            const uint8_t b = cp1250::FromCodepoint(cp);
            if (b == '?') { err = "a character (U+" + std::to_string(cp) + ") that CP1250 cannot hold: save as UTF-8 instead"; return false; }
            out.push_back(b);
        } else {
            uint8_t b = cp1251::CodepointToByte(cp);
            if (b == '?') { err = "a character (U+" + std::to_string(cp) + ") that CP1251 cannot hold: save as UTF-8 instead"; return false; }
            out.push_back(b);
        }
    }
    return true;
}

} // namespace codec
