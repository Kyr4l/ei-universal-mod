// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's binary "registry" files (a mod's config.reg, ai.reg ...), read the way EIStarter reads them (its EIRegFile):
//   u32 signature 0x45AB3EFB, u16 section count, then per section a 6-byte index entry (u16, u16 offset of the section, u16);
//   a section = u16 value count, u16 name length, name, then per value a 6-byte index entry (u16, u16 offset from the section start, u16);
//   a value = u8 type (0 dword, 1 float, 2 string, 128+ arrays: not read), u16 name length, name, then u32 / f32 / (u16 length, text).
// Text is Windows-1251. Section and value names are compared case-insensitively.
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "inifile.hpp"

namespace launcher {

// Windows-1251 -> UTF-8 (the starter's mod titles are Russian).
inline std::string Cp1251ToUtf8(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x80) { out += static_cast<char>(c); continue; }
        uint32_t cp;
        if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
        else if (c == 0xA8) cp = 0x401;
        else if (c == 0xB8) cp = 0x451;
        else if (c == 0xB9) cp = 0x2116;
        else if (c == 0x85) cp = 0x2026;
        else if (c == 0x91) cp = 0x2018;
        else if (c == 0x92) cp = 0x2019;
        else if (c == 0x93) cp = 0x201C;
        else if (c == 0x94) cp = 0x201D;
        else if (c == 0x96) cp = 0x2013;
        else if (c == 0x97) cp = 0x2014;
        else if (c == 0xA0) cp = 0xA0;
        else if (c == 0xA9) cp = 0xA9;
        else if (c == 0xAB) cp = 0xAB;
        else if (c == 0xBB) cp = 0xBB;
        else if (c == 0xB0) cp = 0xB0;
        else cp = '?';
        AppendUtf8(out, cp);
    }
    return out;
}

class EiRegFile {
public:
    enum class Type : uint8_t { Dword = 0, Float = 1, String = 2, Other = 255 };
    struct Value { Type type = Type::Other; uint32_t dword = 0; float number = 0; std::string text; };

    bool Load(const std::string& path) {
        sections_.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (d.size() < 6 || U32(d, 0) != 0x45AB3EFBu) return false;
        const unsigned sections = U16(d, 4);
        size_t at = 6;
        for (unsigned i = 0; i < sections; ++i, at += 6) {
            if (at + 4 > d.size()) return false;
            size_t so = U16(d, at + 2);
            if (so + 4 > d.size()) return false;
            const unsigned count = U16(d, so), nlen = U16(d, so + 2);
            if (so + 4 + nlen > d.size()) return false;
            std::string name = Str(d, so + 4, nlen);
            size_t idx = so + 4 + nlen;
            auto& sec = sections_[LowerAscii(name)];
            for (unsigned v = 0; v < count; ++v, idx += 6) {
                if (idx + 4 > d.size()) return false;
                const size_t vo = so + U16(d, idx + 2);
                if (vo + 3 > d.size()) return false;
                const unsigned type = d[vo], vlen = U16(d, vo + 1);
                if (vo + 3 + vlen > d.size()) return false;
                const std::string vname = LowerAscii(Str(d, vo + 3, vlen));
                size_t p = vo + 3 + vlen;
                Value val;
                if (type == 0 && p + 4 <= d.size()) { val.type = Type::Dword; val.dword = U32(d, p); }
                else if (type == 1 && p + 4 <= d.size()) { val.type = Type::Float; std::memcpy(&val.number, &d[p], 4); }
                else if (type == 2 && p + 2 <= d.size()) {
                    const unsigned sl = U16(d, p);
                    if (p + 2 + sl > d.size()) return false;
                    val.type = Type::String; val.text = Str(d, p + 2, sl);
                }
                sec[vname] = val;
            }
        }
        loaded_ = true;
        return true;
    }
    bool Loaded() const { return loaded_; }
    // Text values as UTF-8 (they are stored as Windows-1251).
    std::string GetString(const std::string& section, const std::string& name, const std::string& def = {}) const {
        const Value* v = Find(section, name);
        return v && v->type == Type::String ? Cp1251ToUtf8(v->text) : def;
    }
    uint32_t GetDword(const std::string& section, const std::string& name, uint32_t def = 0) const {
        const Value* v = Find(section, name);
        return v && v->type == Type::Dword ? v->dword : def;
    }
    bool Has(const std::string& section, const std::string& name) const { return Find(section, name) != nullptr; }

private:
    static unsigned U16(const std::vector<uint8_t>& d, size_t o) { return o + 2 <= d.size() ? d[o] | (d[o + 1] << 8) : 0; }
    static uint32_t U32(const std::vector<uint8_t>& d, size_t o) { return o + 4 <= d.size() ? d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24) : 0; }
    static std::string Str(const std::vector<uint8_t>& d, size_t o, size_t n) { return std::string(reinterpret_cast<const char*>(&d[o]), n); }
    const Value* Find(const std::string& section, const std::string& name) const {
        auto s = sections_.find(LowerAscii(section));
        if (s == sections_.end()) return nullptr;
        auto v = s->second.find(LowerAscii(name));
        return v == s->second.end() ? nullptr : &v->second;
    }
    std::map<std::string, std::map<std::string, Value>> sections_;
    bool loaded_ = false;
};

} // namespace launcher
