// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The game's own settings (what the original Options dialog of EIStarter edited): graphics, shadows, terrain, camera, paths. The game reads
// them from the registry (HKCU\Software\Gipat.Ru\EI_Starter\EvilIslands, a clone of Nival's key that addon.dll points game.exe at); the
// launcher keeps them in its config file instead and writes them to the registry when the game starts (regsync.hpp).
// In the config file each registry sub key is a section "[Game: <key>]" with lines `name=dword:N`, `name=float:X`, `name=string:TEXT` or
// `name=hex:aa,bb,...`.
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "game_defaults.hpp"
#include "inifile.hpp"
#include "winreg.hpp"

namespace launcher {

constexpr const char* kNivalKey = "Software\\Nival Interactive\\EvilIslands";
constexpr const char* kStarterKey = "Software\\Gipat.Ru\\EI_Starter";
constexpr const char* kCloneKey = "Software\\Gipat.Ru\\EI_Starter\\EvilIslands";
constexpr const char* kAddonKey = "Software\\Gipat.Ru\\EI_Starter\\settings";

class GameSettings {
public:
    struct Item { std::string key, name; winreg::Value value; };

    const std::vector<Item>& Items() const { return items_; }
    bool Empty() const { return items_.empty(); }

    const winreg::Value* Find(const std::string& key, const std::string& name) const {
        const std::string lk = LowerAscii(key), ln = LowerAscii(name);
        for (const Item& i : items_) if (LowerAscii(i.key) == lk && LowerAscii(i.name) == ln) return &i.value;
        return nullptr;
    }
    bool Has(const std::string& key, const std::string& name) const { return Find(key, name) != nullptr; }
    void Set(const std::string& key, const std::string& name, const winreg::Value& v) {
        const std::string lk = LowerAscii(key), ln = LowerAscii(name);
        for (Item& i : items_) if (LowerAscii(i.key) == lk && LowerAscii(i.name) == ln) { i.value = v; return; }
        items_.push_back({key, name, v});
    }
    void SetBool(const std::string& k, const std::string& n, bool b) { Set(k, n, winreg::Value::Dword(b ? 1 : 0)); }
    void SetInt(const std::string& k, const std::string& n, int v) { Set(k, n, winreg::Value::Dword(static_cast<uint32_t>(v))); }
    void SetFloat(const std::string& k, const std::string& n, float v) { Set(k, n, winreg::Value::Float(v)); }
    void SetString(const std::string& k, const std::string& n, const std::string& v) { Set(k, n, winreg::Value::String(v)); }

    int GetInt(const std::string& key, const std::string& name, int def = 0) const {
        const winreg::Value* v = Find(key, name);
        return v && v->kind == winreg::Value::Kind::Dword ? static_cast<int>(v->dword) : def;
    }
    bool GetBool(const std::string& key, const std::string& name, bool def = false) const { return GetInt(key, name, def ? 1 : 0) != 0; }
    float GetFloat(const std::string& key, const std::string& name, float def = 0) const {
        const winreg::Value* v = Find(key, name);
        if (!v || v->kind != winreg::Value::Kind::Binary || v->bytes.size() != 4) return def;
        float f; std::memcpy(&f, v->bytes.data(), 4); return f;
    }
    std::string GetString(const std::string& key, const std::string& name, const std::string& def = {}) const {
        const winreg::Value* v = Find(key, name);
        return v && v->kind == winreg::Value::Kind::String ? v->text : def;
    }

    // The settings of a fresh installation (game_defaults.hpp); keeps values already there.
    void FillDefaults() {
        for (const DefaultValue& d : kGameDefaults) if (!Has(d.key, d.name)) Set(d.key, d.name, FromDefault(d));
    }
    // The camera of the original CD release of the game: the "Camera Settings" rows of game_defaults.hpp (read from a CD installation's registry).
    // The GOG release writes other values, which make a bad camera.
    static bool SameValue(const winreg::Value& a, const winreg::Value& b) {
        return a.kind == b.kind && a.dword == b.dword && a.bytes == b.bytes && a.text == b.text;
    }
    static bool IsCameraRow(const DefaultValue& d) { return LowerAscii(d.key) == "camera settings"; }
    // The names of the camera values that differ from the original release (a missing value counts as different).
    std::vector<std::string> CameraDifferences() const {
        std::vector<std::string> out;
        for (const DefaultValue& d : kGameDefaults) {
            if (!IsCameraRow(d)) continue;
            const winreg::Value* v = Find(d.key, d.name);
            if (!v || !SameValue(*v, FromDefault(d))) out.push_back(d.name);
        }
        return out;
    }
    // Puts every camera value of the original release back; the other settings are not touched.
    void RestoreOriginalCamera() {
        for (const DefaultValue& d : kGameDefaults) if (IsCameraRow(d)) Set(d.key, d.name, FromDefault(d));
    }
    // The camera values as "name=value" lines (the backup file written before they are replaced).
    std::vector<std::string> CameraLines() const {
        std::vector<std::string> out;
        for (const Item& i : items_) if (LowerAscii(i.key) == "camera settings") out.push_back(i.name + "=" + Encode(i.value));
        return out;
    }
    // Writes the original camera into a registry key (Nival's key, the clone): the "Camera Settings" values under `root`.
    static void WriteOriginalCameraToRegistry(const std::string& root, void (*log)(const std::string&) = nullptr) {
        for (const DefaultValue& d : kGameDefaults) {
            if (!IsCameraRow(d)) continue;
            winreg::Set(root + "\\" + d.key, d.name, FromDefault(d));
        }
        if (log) log("HKCU\\" + root + "\\Camera Settings: the original camera written");
    }
    // Overlays the values of a registry key tree (the clone or Nival's key); returns false when the key does not exist.
    bool ImportRegistry(const std::string& root) {
        if (!winreg::KeyExists(root)) return false;
        for (const std::string& sub : winreg::SubKeys(root))
            for (const std::string& n : winreg::ValueNames(root + "\\" + sub)) {
                winreg::Value v;
                if (winreg::Get(root + "\\" + sub, n, v)) Set(sub, n, v);
            }
        return true;
    }
    void ApplyToRegistry(const std::string& root, void (*log)(const std::string&) = nullptr) const {
        winreg::CreateKey(root);
        for (const Item& i : items_) {
            winreg::Set(root + "\\" + i.key, i.name, i.value);
            if (log) log("HKCU\\" + root + "\\" + i.key + "\\" + i.name + " = " + Encode(i.value));
        }
    }

    // [Game: <key>] sections of the config file.
    void Save(IniFile& ini) const {
        for (const Item& i : items_) ini.Set("Game: " + i.key, i.name, Encode(i.value));
    }
    void Load(const IniFile& ini) {
        for (const IniFile::Section& s : ini.Sections()) {
            if (s.name.compare(0, 6, "Game: ") != 0) continue;
            for (const IniFile::Entry& e : s.entries) { winreg::Value v; if (Decode(e.value, v)) Set(s.name.substr(6), e.key, v); }
        }
    }
    static std::string Encode(const winreg::Value& v) {
        char b[64];
        switch (v.kind) {
        case winreg::Value::Kind::Dword: return "dword:" + std::to_string(v.dword);
        case winreg::Value::Kind::String: return "string:" + v.text;
        case winreg::Value::Kind::Binary:
            if (v.bytes.size() == 4) { float f; std::memcpy(&f, v.bytes.data(), 4); std::snprintf(b, sizeof b, "float:%.9g", static_cast<double>(f)); return b; }
            { std::string s = "hex:"; for (size_t i = 0; i < v.bytes.size(); ++i) { std::snprintf(b, sizeof b, "%s%02x", i ? "," : "", v.bytes[i]); s += b; } return s; }
        }
        return {};
    }
    static bool Decode(const std::string& text, winreg::Value& out) {
        const size_t c = text.find(':');
        if (c == std::string::npos) return false;
        const std::string kind = text.substr(0, c), rest = text.substr(c + 1);
        if (kind == "dword") { out = winreg::Value::Dword(static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10))); return true; }
        if (kind == "string") { out = winreg::Value::String(rest); return true; }
        if (kind == "float") { out = winreg::Value::Float(std::strtof(rest.c_str(), nullptr)); return true; }
        if (kind == "hex") {
            std::vector<uint8_t> bytes;
            size_t p = 0;
            while (p < rest.size()) { bytes.push_back(static_cast<uint8_t>(std::strtoul(rest.c_str() + p, nullptr, 16))); const size_t n = rest.find(',', p); if (n == std::string::npos) break; p = n + 1; }
            out = winreg::Value::Bytes(bytes); return true;
        }
        return false;
    }

private:
    static winreg::Value FromDefault(const DefaultValue& d) {
        if (d.kind == 'D') return winreg::Value::Dword(static_cast<uint32_t>(std::strtoul(d.text, nullptr, 10)));
        if (d.kind == 'S') return winreg::Value::String(d.text);
        std::vector<uint8_t> bytes; const char* p = d.text;
        while (*p) { bytes.push_back(static_cast<uint8_t>(std::strtoul(p, nullptr, 16))); const char* n = std::strchr(p, ','); if (!n) break; p = n + 1; }
        return winreg::Value::Bytes(bytes);
    }
    std::vector<Item> items_;
};

} // namespace launcher
