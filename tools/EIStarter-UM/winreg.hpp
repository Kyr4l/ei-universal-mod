// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The few registry operations the launcher needs (HKEY_CURRENT_USER only): the game and its addon.dll / dinput.dll loader read
// their settings from there. On Windows (and under Wine) these are the real registry; on other systems an in-memory stand-in, so
// the logic can be tested natively. Names are case-insensitive like the registry's.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "inifile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace launcher::winreg {

struct Value {
    enum class Kind { Dword, Binary, String } kind = Kind::String;
    uint32_t dword = 0;
    std::vector<uint8_t> bytes;
    std::string text;
    static Value Dword(uint32_t v) { Value x; x.kind = Kind::Dword; x.dword = v; return x; }
    static Value String(std::string s) { Value x; x.kind = Kind::String; x.text = std::move(s); return x; }
    static Value Float(float f) { Value x; x.kind = Kind::Binary; x.bytes.resize(4); std::memcpy(x.bytes.data(), &f, 4); return x; }
    static Value Bytes(std::vector<uint8_t> b) { Value x; x.kind = Kind::Binary; x.bytes = std::move(b); return x; }
};

#ifdef _WIN32
inline std::wstring W(const std::string& s) {
    std::wstring w; w.resize(s.size() + 1);
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], static_cast<int>(w.size()));
    w.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
    return w;
}
inline std::string FromW(const wchar_t* w, size_t len) {
    std::string s; s.resize(len * 4 + 1);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), &s[0], static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return s;
}
inline bool Open(const std::string& key, REGSAM access, HKEY& out) { return RegOpenKeyExW(HKEY_CURRENT_USER, W(key).c_str(), 0, access, &out) == ERROR_SUCCESS; }

inline bool Get(const std::string& key, const std::string& name, Value& out) {
    HKEY h; if (!Open(key, KEY_QUERY_VALUE, h)) return false;
    DWORD type = 0, size = 0;
    const std::wstring wn = W(name);
    bool ok = RegQueryValueExW(h, wn.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    if (ok) {
        std::vector<uint8_t> buf(size + 2, 0);
        ok = RegQueryValueExW(h, wn.c_str(), nullptr, &type, buf.data(), &size) == ERROR_SUCCESS;
        if (ok) {
            if (type == REG_DWORD && size >= 4) out = Value::Dword(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (static_cast<uint32_t>(buf[3]) << 24));
            else if (type == REG_SZ || type == REG_EXPAND_SZ) {
                size_t chars = size / 2;
                while (chars && reinterpret_cast<wchar_t*>(buf.data())[chars - 1] == 0) --chars;
                out = Value::String(FromW(reinterpret_cast<wchar_t*>(buf.data()), chars));
            } else { buf.resize(size); out = Value::Bytes(buf); }
        }
    }
    RegCloseKey(h);
    return ok;
}
inline bool Set(const std::string& key, const std::string& name, const Value& v) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, W(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r;
    const std::wstring wn = W(name);
    if (v.kind == Value::Kind::Dword) { DWORD d = v.dword; r = RegSetValueExW(h, wn.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), 4); }
    else if (v.kind == Value::Kind::Binary) r = RegSetValueExW(h, wn.c_str(), 0, REG_BINARY, v.bytes.data(), static_cast<DWORD>(v.bytes.size()));
    else { const std::wstring ws = W(v.text); r = RegSetValueExW(h, wn.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(ws.c_str()), static_cast<DWORD>((ws.size() + 1) * 2)); }
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
inline bool KeyExists(const std::string& key) { HKEY h; if (!Open(key, KEY_READ, h)) return false; RegCloseKey(h); return true; }
inline bool CreateKey(const std::string& key) {
    HKEY h; if (RegCreateKeyExW(HKEY_CURRENT_USER, W(key).c_str(), 0, nullptr, 0, KEY_READ, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    RegCloseKey(h); return true;
}
inline std::vector<std::string> SubKeys(const std::string& key) {
    std::vector<std::string> out; HKEY h; if (!Open(key, KEY_ENUMERATE_SUB_KEYS, h)) return out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) { DWORD n = 512; if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; out.push_back(FromW(name, n)); }
    RegCloseKey(h); return out;
}
inline std::vector<std::string> ValueNames(const std::string& key) {
    std::vector<std::string> out; HKEY h; if (!Open(key, KEY_QUERY_VALUE, h)) return out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) { DWORD n = 512; if (RegEnumValueW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; out.push_back(FromW(name, n)); }
    RegCloseKey(h); return out;
}
inline void DeleteTree(const std::string& key) {
    for (const std::string& s : SubKeys(key)) DeleteTree(key + "\\" + s);
    RegDeleteKeyW(HKEY_CURRENT_USER, W(key).c_str());
}
#else
// In-memory stand-in: key (lower case) -> value name (lower case) -> value.
inline std::map<std::string, std::map<std::string, Value>>& Store() { static std::map<std::string, std::map<std::string, Value>> s; return s; }
inline std::string Norm(std::string k) { for (char& c : k) if (c == '/') c = '\\'; return LowerAscii(k); }
inline bool Get(const std::string& key, const std::string& name, Value& out) {
    auto k = Store().find(Norm(key)); if (k == Store().end()) return false;
    auto v = k->second.find(LowerAscii(name)); if (v == k->second.end()) return false;
    out = v->second; return true;
}
inline bool Set(const std::string& key, const std::string& name, const Value& v) { Store()[Norm(key)][LowerAscii(name)] = v; return true; }
inline bool KeyExists(const std::string& key) { return Store().count(Norm(key)) != 0; }
inline bool CreateKey(const std::string& key) { Store()[Norm(key)]; return true; }
inline std::vector<std::string> SubKeys(const std::string& key) {
    std::vector<std::string> out; const std::string p = Norm(key) + "\\";
    for (auto& kv : Store()) if (kv.first.compare(0, p.size(), p) == 0 && kv.first.find('\\', p.size()) == std::string::npos) out.push_back(kv.first.substr(p.size()));
    return out;
}
inline std::vector<std::string> ValueNames(const std::string& key) {
    std::vector<std::string> out; auto k = Store().find(Norm(key));
    if (k != Store().end()) for (auto& kv : k->second) out.push_back(kv.first);
    return out;
}
inline void DeleteTree(const std::string& key) {
    const std::string p = Norm(key);
    for (auto it = Store().begin(); it != Store().end();) { if (it->first == p || it->first.compare(0, p.size() + 1, p + "\\") == 0) it = Store().erase(it); else ++it; }
}
#endif

// Copies a key with its values and sub keys (src and dst are paths under HKEY_CURRENT_USER). Existing destination values are overwritten.
inline void CopyTree(const std::string& src, const std::string& dst) {
    CreateKey(dst);
    for (const std::string& n : ValueNames(src)) { Value v; if (Get(src, n, v)) Set(dst, n, v); }
    for (const std::string& s : SubKeys(src)) CopyTree(src + "\\" + s, dst + "\\" + s);
}

inline bool GetString(const std::string& key, const std::string& name, std::string& out) {
    Value v; if (!Get(key, name, v) || v.kind != Value::Kind::String) return false;
    out = v.text; return true;
}

} // namespace launcher::winreg
