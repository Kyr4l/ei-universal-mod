// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Turns an Items database row into what the game shows for it: a figure (the
// ground/inventory model) and the textures that fit it. Pure logic, no GL, so the
// same code backs the GUI and the --resolve command line.
//
// Naming rules, all checked against the vanilla figures.res / textures.res /
// redress.res and databaselmp.res (every Weapons, QuickItems, QuestItems and
// treasure LootItems row resolves to an existing figure):
//
//   Weapons     figure initwe<code><TTI>      axe=ax sword=sw dagger=dg spear=sp hammer=hm crossbow=cb bow=bw
//   Armors      figure initar<code><TTI>      helm=hl plate=pl leggings=lg shirt=sh pants=pt boots=bt gloves=gl
//     textures  redress.res <race><code>_<TTI 2 digits>.<material code>.<TTI2>, e.g. unhumaax_03.br.0
//               textures.res <code>_<TTI 2 digits>.<TTI2>, e.g. ax_03.0: the blueprint texture - a blueprint
//               has no material, so it is greyscale (a tinted grey, unlike the material textures)
//     The ground figure is the same geometry as the equipped mesh (initweax3 and
//     unhuma.mod's rh3.axe03 both have 56 vertices / 80 triangles), so the
//     equipped (redress) textures fit it.
//   QuickItems  figure initqi<TTI>, texture qitem<TTI 4 digits>[.<material code>] (wands have one per material)
//   QuestItems  figure initqu<TTI>, texture quitem<TTI 4 digits>
//   LootItems   type "treasure": figure initlitr<TTI>, texture litem<TTI 4 digits>
//               type "material": figure initlimt<Material ID>, texture material<ID 4 digits>
//
// The default material is the first one of the item's M.Type (in database order)
// that has a texture - e.g. a Metal axe opens in bronze.
#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "asset_source.hpp"
#include "item_db.hpp"

namespace resolve {

inline std::string Lower(const std::string& s) { return res::Archive::ToLower(s); }

inline std::string Pad(int value, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", digits, value);
    return buf;
}

// Every texture name available across the texture layers (lowercase base names, .mmp or .dds).
struct TextureIndex {
    std::vector<std::string> names; // sorted, unique
    std::set<std::string> lookup;

    void Build(const LayeredAssetSource& textures) {
        names.clear();
        for (auto& n : textures.ListBaseNames({".mmp", ".dds"})) names.push_back(Lower(n));
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        lookup = std::set<std::string>(names.begin(), names.end());
    }
    bool Has(const std::string& name) const { return lookup.count(Lower(name)) != 0; }
};

// Figure names, for the manual figure picker and for "does this figure exist".
struct FigureIndex {
    std::set<std::string> baseNames; // lowercase, without extension, of every .lnk/.fig/.mod

    void Build(const LayeredAssetSource& figures) {
        baseNames.clear();
        for (auto& n : figures.ListBaseNames({".lnk", ".fig", ".mod"})) baseNames.insert(Lower(n));
    }
    // The name to hand to LoadNamedModel: the .lnk name when there is one, else the
    // model file itself (a mod may ship "initwesw6weapon.fig" without its .lnk).
    std::string Loadable(const std::string& name) const {
        std::string n = Lower(name);
        if (baseNames.count(n)) return n;
        for (const char* suffix : {"weapon", "armor", "item"}) {
            if (baseNames.count(n + suffix)) return n + suffix;
        }
        return "";
    }
    // Item figures of one family ("initwe", "initar"...), short names only, naturally sorted.
    std::vector<std::string> Family(const std::string& prefix) const {
        std::set<std::string> shortNames;
        for (auto& n : baseNames) {
            if (n.compare(0, prefix.size(), prefix) != 0) continue;
            std::string s = n;
            for (const char* suffix : {"weapon", "armor", "item"}) {
                size_t len = std::char_traits<char>::length(suffix);
                if (s.size() > len && s.compare(s.size() - len, len, suffix) == 0) { s.resize(s.size() - len); break; }
            }
            shortNames.insert(s);
        }
        std::vector<std::string> out(shortNames.begin(), shortNames.end());
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
            // natural order: initweax2 before initweax10
            size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i] && !isdigit(static_cast<unsigned char>(a[i]))) ++i;
            if (i < a.size() && i < b.size() && isdigit(static_cast<unsigned char>(a[i])) && isdigit(static_cast<unsigned char>(b[i]))) {
                long na = std::strtol(a.c_str() + i, nullptr, 10), nb = std::strtol(b.c_str() + i, nullptr, 10);
                if (na != nb) return na < nb;
            }
            return a < b;
        });
        return out;
    }
};

inline const std::map<std::string, std::string>& WeaponCodes() {
    static const std::map<std::string, std::string> m = {
        {"axe", "ax"}, {"sword", "sw"}, {"dagger", "dg"}, {"spear", "sp"},
        {"hammer", "hm"}, {"crossbow", "cb"}, {"bow", "bw"},
    };
    return m;
}

inline const std::map<std::string, std::string>& ArmorCodes() {
    static const std::map<std::string, std::string> m = {
        {"helm", "hl"}, {"plate", "pl"}, {"leggings", "lg"}, {"shirt", "sh"},
        {"pants", "pt"}, {"boots", "bt"}, {"gloves", "gl"},
    };
    return m;
}

// The figure family an item category draws from, for the manual picker.
inline std::string FamilyPrefix(items::Category c, const std::string& code) {
    switch (c) {
    case items::Category::Weapons: return "initwe" + code;
    case items::Category::Armors: return "initar" + code;
    case items::Category::QuickItems: return "initqi";
    case items::Category::QuestItems: return "initqu";
    case items::Category::LootItems: return "initli";
    default: return "init";
    }
}

struct TextureOption {
    std::string name;                        // texture base name, e.g. "unhumaax_03.br.0"
    const items::Material* material = nullptr; // the material it shows, when known
    std::string note;                        // e.g. "human male, bronze" / "blueprint (greyscale, no material)"
};

struct Resolution {
    std::string figure;       // short figure name from the rules, e.g. "initweax3" (empty if no rule applies)
    std::string loadable;     // name to load (see FigureIndex::Loadable), empty if the figure does not exist
    std::string familyPrefix; // for the manual figure picker
    std::vector<const items::Material*> materials; // materials the user can pick (may be empty)
    int material = -1;        // index into materials, -1 = none
    std::vector<TextureOption> textures; // candidates, best first
    int texture = -1;         // index into textures of the one to show, -1 = none found
    std::vector<std::string> notes; // why things were or were not found, for the UI
};

inline int RaceRank(const std::string& prefix) {
    static const char* const order[] = {"unhuma", "unhufe", "unorma", "unorfe"};
    for (int i = 0; i < 4; ++i) if (prefix == order[i]) return i;
    return prefix.empty() ? 5 : 4;
}

inline std::string RaceLabel(const std::string& prefix) {
    if (prefix == "unhuma") return "human male";
    if (prefix == "unhufe") return "human female";
    if (prefix == "unorma") return "orc male";
    if (prefix == "unorfe") return "orc female";
    return prefix.empty() ? "blueprint (greyscale, no material)" : prefix;
}

// Weapons/Armors: every texture named <anything><code>_<NN>.<...>, sorted so that the
// materials of the item's own class come first, in database order, human male first,
// the item's own TTI2 subvariant first.
inline void CollectEquipmentTextures(const items::Database& db, const items::Item& item, const std::string& code,
                                     const TextureIndex& textures, Resolution& r) {
    const std::string key = code + "_" + Pad(item.tti, 2) + ".";
    struct Found { std::string name, race, matCode; int sub; };
    std::vector<Found> found;
    for (const std::string& n : textures.names) {
        size_t pos = n.find(key);
        if (pos == std::string::npos) continue;
        std::string prefix = n.substr(0, pos);
        bool alpha = std::all_of(prefix.begin(), prefix.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
        if (!alpha) continue;
        std::string rest = n.substr(pos + key.size()); // "br.0" or "0"
        Found f{n, prefix, "", -1};
        size_t dot = rest.find('.');
        if (dot == std::string::npos) {
            f.sub = std::atoi(rest.c_str());
        } else {
            f.matCode = rest.substr(0, dot);
            f.sub = std::atoi(rest.c_str() + dot + 1);
        }
        found.push_back(f);
    }
    r.materials = db.MaterialsOfType(item.materialType);
    auto materialFor = [&](const std::string& matCode) -> const items::Material* {
        for (auto* m : r.materials) if (Lower(m->code) == matCode) return m;
        return nullptr;
    };
    auto rank = [&](const Found& f) {
        const items::Material* m = f.matCode.empty() ? nullptr : materialFor(f.matCode);
        int materialOrder = 1000;
        if (m) materialOrder = static_cast<int>(std::find(r.materials.begin(), r.materials.end(), m) - r.materials.begin());
        else if (!f.matCode.empty()) materialOrder = 500; // a material code outside the item's class (unique items)
        return std::make_tuple(materialOrder, RaceRank(f.race), f.sub == item.tti2 ? 0 : 1, f.name);
    };
    std::sort(found.begin(), found.end(), [&](const Found& a, const Found& b) { return rank(a) < rank(b); });
    for (auto& f : found) {
        TextureOption o;
        o.name = f.name;
        o.material = f.matCode.empty() ? nullptr : materialFor(f.matCode);
        o.note = RaceLabel(f.race);
        if (o.material) o.note += ", " + o.material->name;
        else if (!f.matCode.empty()) o.note += ", material code '" + f.matCode + "'";
        r.textures.push_back(o);
    }
    if (found.empty()) r.notes.push_back("no texture named *" + key + "* in the texture sources");
}

// QuickItems/QuestItems/LootItems: "<base>" plus any "<base>.<material code>".
inline void CollectPlainTextures(const std::string& base, const std::vector<const items::Material*>& materials,
                                 const TextureIndex& textures, Resolution& r) {
    for (auto* m : materials) {
        std::string n = base + "." + Lower(m->code);
        if (textures.Has(n)) r.textures.push_back({n, m, m->name});
    }
    // Without a material code: for an item made of a material (a wand), its greyscale blueprint texture;
    // for anything else (a potion, a quest item), simply its texture.
    if (textures.Has(base)) r.textures.push_back({base, nullptr, materials.empty() ? "no material" : "blueprint (greyscale, no material)"});
    for (const std::string& n : textures.names) { // other variants (e.g. a mod texture with its own code)
        if (n.size() <= base.size() + 1 || n.compare(0, base.size() + 1, base + ".") != 0) continue;
        bool already = std::any_of(r.textures.begin(), r.textures.end(), [&](const TextureOption& o) { return o.name == n; });
        if (!already) r.textures.push_back({n, nullptr, "variant '" + n.substr(base.size() + 1) + "'"});
    }
    if (r.textures.empty()) r.notes.push_back("no texture named " + base + " in the texture sources");
}

// chosenMaterial: the material the user picked, or nullptr for the default.
inline Resolution Resolve(const items::Database& db, const items::Item& item, const FigureIndex& figures,
                          const TextureIndex& textures, const items::Material* chosenMaterial) {
    Resolution r;
    using items::Category;
    switch (item.category) {
    case Category::Weapons:
    case Category::Armors: {
        const auto& codes = item.category == Category::Weapons ? WeaponCodes() : ArmorCodes();
        auto it = codes.find(Lower(item.type));
        if (it == codes.end()) {
            r.familyPrefix = item.category == Category::Weapons ? "initwe" : "initar";
            r.notes.push_back("type '" + item.type + "' has no known figure family; pick a figure by hand");
            r.materials = db.MaterialsOfType(item.materialType);
            break;
        }
        const std::string& code = it->second;
        r.figure = (item.category == Category::Weapons ? "initwe" : "initar") + code + std::to_string(item.tti);
        r.familyPrefix = FamilyPrefix(item.category, code);
        if (item.tti < 0) r.notes.push_back("TTI is " + std::to_string(item.tti) + ": this row has no model in the game");
        CollectEquipmentTextures(db, item, code, textures, r);
        break;
    }
    case Category::QuickItems:
        r.figure = "initqi" + std::to_string(item.tti);
        r.familyPrefix = "initqi";
        r.materials = db.MaterialsOfType(item.materialType);
        CollectPlainTextures("qitem" + Pad(item.tti, 4), r.materials, textures, r);
        break;
    case Category::QuestItems:
        r.figure = "initqu" + std::to_string(item.tti);
        r.familyPrefix = "initqu";
        CollectPlainTextures("quitem" + Pad(item.tti, 4), {}, textures, r);
        break;
    case Category::LootItems: {
        r.familyPrefix = "initli";
        std::string type = Lower(item.type);
        if (type == "material") {
            // One row stands for every material: the user picks which one.
            for (auto& m : db.materials) r.materials.push_back(&m);
            const items::Material* m = chosenMaterial ? chosenMaterial : (r.materials.empty() ? nullptr : r.materials.front());
            if (m) {
                r.figure = "initlimt" + std::to_string(m->id);
                CollectPlainTextures("material" + Pad(m->id, 4), {}, textures, r);
                for (auto& o : r.textures) o.material = m;
            }
        } else if (type == "treasure") {
            r.figure = "initlitr" + std::to_string(item.tti);
            CollectPlainTextures("litem" + Pad(item.tti, 4), {}, textures, r);
        } else {
            r.notes.push_back("'" + item.type + "' loot has no ground figure of its own in the game files");
        }
        break;
    }
    default:
        break;
    }

    if (!r.figure.empty()) {
        r.loadable = figures.Loadable(r.figure);
        if (r.loadable.empty()) r.notes.push_back("figure " + r.figure + " not found in the figure sources");
    }

    // Material: the user's pick, else the first one that has a texture, else the first one.
    if (!r.materials.empty()) {
        auto indexOf = [&](const items::Material* m) {
            auto found = std::find(r.materials.begin(), r.materials.end(), m);
            return found == r.materials.end() ? -1 : static_cast<int>(found - r.materials.begin());
        };
        if (chosenMaterial) r.material = indexOf(chosenMaterial);
        if (r.material < 0) {
            for (auto& o : r.textures) {
                if (o.material && indexOf(o.material) >= 0) { r.material = indexOf(o.material); break; }
            }
        }
        if (r.material < 0) r.material = 0;
    }

    // Texture: the best candidate showing the chosen material, else the best candidate at all.
    const items::Material* want = r.material >= 0 ? r.materials[r.material] : nullptr;
    for (size_t i = 0; i < r.textures.size() && r.texture < 0; ++i) {
        if (want && r.textures[i].material == want) r.texture = static_cast<int>(i);
    }
    if (r.texture < 0 && !r.textures.empty()) {
        r.texture = 0;
        if (want && item.category != items::Category::LootItems) {
            r.notes.push_back("no texture for " + want->name + "; showing " + r.textures[0].name);
        }
    }
    return r;
}

} // namespace resolve
