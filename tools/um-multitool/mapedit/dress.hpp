// Dressing a map unit the way the game does: its prototype's monster record (units.udb) gives the race's
// figure and skin, the hair, and the default equipment; the unit's own armor and weapon lists in the map
// replace that equipment when it has any. Each item ("blueprint.material", items.idb) picks a numbered
// mesh variant of the figure (its TTI) and a redress texture
// "<race mask><type code>_<TTI>.<material code>.<TTI2>" (redress.res).
//
// The rules come from the unit viewer's research (checked against the real .mod hierarchies and
// redress files): helms, plates and leggings have meshes of their own that replace the bare parts
// under them; shirts, pants, boots and gloves are only textures laid over the bare parts; a helm hides
// the hair; a weapon hides the empty-hand mesh; the quiver shows with a bow or crossbow; the "00" node
// of a weapon family is a placeholder, except the bow's and crossbow's shared pieces.
#pragma once

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../viewer/figure_format.hpp"
#include "../viewer/library.hpp"
#include "mob_file.hpp"

namespace dress {

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The equipment slot of a dotted part: "hd.armor01" -> "hd:armor", "rh3.axe00" -> "rh3", "hr.02" -> "hr".
inline std::string Group(const std::string& name) {
    const size_t dot = name.find('.');
    if (dot == std::string::npos) return {};
    size_t end = dot + 1;
    while (end < name.size() && std::isalpha(static_cast<unsigned char>(name[end]))) ++end;
    const std::string prefix = Lower(name.substr(0, dot));
    return Lower(name.substr(dot + 1, end - dot - 1)) == "armor" ? prefix + ":armor" : prefix;
}

inline bool MultiPieceWeapon(const std::string& name) {
    const size_t dot = name.find('.');
    if (dot == std::string::npos) return false;
    size_t end = dot + 1;
    while (end < name.size() && std::isalpha(static_cast<unsigned char>(name[end]))) ++end;
    const std::string suffix = Lower(name.substr(dot + 1, end - dot - 1));
    return suffix == "bwpartb" || suffix == "crbow";
}

// A piece of a bow or crossbow and the weapon number it belongs to. Bows are four families of numbered
// pieces, each under its own "00" piece: the body lh3.bwpartbNN, the other limb bwpartaNN and the two
// string halves bwtetivaaNN / bwtetivabNN. Crossbows: rh3.crbowNNmain, crbowNNpartKK, crbowNNtetivaKK
// (NN the crossbow, nested under the 01 pieces). Only the pieces of the equipped number show.
struct WeaponPiece { const char* slot = nullptr; int number = -1; };
inline WeaponPiece PieceOf(const std::string& lowerName) {
    auto digitsAt = [&](size_t at) {
        int n = 0, k = 0;
        while (at < lowerName.size() && std::isdigit(static_cast<unsigned char>(lowerName[at])) && k < 2) { n = n * 10 + (lowerName[at] - '0'); ++at; ++k; }
        return k ? n : -1;
    };
    for (const char* prefix : {"lh3.bwpartb", "bwparta", "bwtetivaa", "bwtetivab"})
        if (lowerName.rfind(prefix, 0) == 0) return {"lh3", digitsAt(std::strlen(prefix))};
    const size_t cb = lowerName.find("crbow");
    if (cb != std::string::npos && (cb == 0 || lowerName.rfind("rh3.", 0) == 0)) return {"rh3", digitsAt(cb + 5)};
    return {};
}

struct Dress {
    bool on = false;
    std::string skin, secondSkin;                 // textures of the bare parts (texture number 0 / 1)
    std::map<std::string, std::string> selected;  // slot -> the variant part shown (lower case)
    std::map<std::string, std::string> texture;   // variant part (lower case) -> its redress texture
    std::map<std::string, std::string> overlay;   // (unused: clothing is composed into the body's texture)
    std::string body;                             // the body's texture: "compose:" skin, then the worn items' textures
    std::set<std::string> bare;                   // OBJ_BODYPARTS (lower case); empty: all bare parts
    std::string summary;                          // what was applied, for the details
};

namespace detail {

struct Slot { const char* group; const char* variant; const char* suffix; const char* code; };

inline const Slot* WeaponSlot(const std::string& type) {
    static const std::map<std::string, Slot> table = {
        {"axe", {"rh3", "axe", "", "ax"}},      {"sword", {"rh3", "sword", "", "sw"}}, {"dagger", {"rh3", "dagger", "", "dg"}},
        {"spear", {"rh3", "pike", "", "sp"}},   {"hammer", {"rh3", "club", "", "hm"}}, {"crossbow", {"rh3", "crbow", "main", "cb"}},
        {"bow", {"lh3", "bwpartb", "", "bw"}},
    };
    auto it = table.find(Lower(type));
    return it == table.end() ? nullptr : &it->second;
}

inline const std::vector<std::string>* ArmorParts(const std::string& type) {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"helm", {"hd"}}, {"plate", {"bd", "lh1", "lh2", "rh1", "rh2"}}, {"shirt", {"bd"}}, {"gloves", {"lh3", "rh3"}},
        {"pants", {"hp", "ll1", "ll2", "rl1", "rl2"}}, {"leggings", {"hp", "ll1", "ll2", "rl1", "rl2"}}, {"boots", {"ll3", "rl3"}},
    };
    auto it = table.find(Lower(type));
    return it == table.end() ? nullptr : &it->second;
}

inline const char* ArmorCode(const std::string& type) {
    static const std::map<std::string, const char*> table = {
        {"helm", "hl"}, {"plate", "pl"}, {"shirt", "sh"}, {"pants", "pt"}, {"boots", "bt"}, {"gloves", "gl"}, {"leggings", "lg"},
    };
    auto it = table.find(Lower(type));
    return it == table.end() ? nullptr : it->second;
}

inline bool TextureOnly(const std::string& type) {
    const std::string t = Lower(type);
    return t == "shirt" || t == "pants" || t == "boots" || t == "gloves";
}

inline const items::Item* FindItem(const Library& lib, items::Category c, const std::string& name) {
    const std::string l = Lower(name);
    for (const items::Item& it : lib.db.List(c)) if (Lower(it.name) == l) return &it;
    return nullptr;
}

inline const items::Material* FindMaterial(const Library& lib, const std::string& name) {
    const std::string l = Lower(name);
    for (const items::Material& m : lib.db.materials) if (Lower(m.name) == l) return &m;
    return nullptr;
}

inline bool HasTexture(const Library& lib, const std::string& name) {
    return lib.textures.Contains(name + ".mmp") || lib.textures.Contains(name + ".dds");
}

// "<mask><code>_<TTI>.<material>.<TTI2>": the unit's own race first, then the human ones (orcs reuse them).
inline std::string Redress(const Library& lib, const std::string& mask, const char* code, int tti, int tti2, const std::string& material) {
    char num[8];
    std::snprintf(num, sizeof(num), "%02d", tti);
    for (const std::string& race : {mask, std::string("unhuma"), std::string("unhufe")}) {
        const std::string name = race + code + "_" + num + "." + material + "." + std::to_string(tti2);
        if (HasTexture(lib, name)) return name;
    }
    return {};
}

// "Skin_05" -> "<mask>skin_05"; other names are used as they are.
inline std::string RaceTexture(const std::string& entry, const std::string& mask) {
    if (entry.size() >= 4 && Lower(entry.substr(0, 4)) == "skin") return mask + "skin" + entry.substr(4);
    return entry;
}

} // namespace detail

inline bool HasTextureNamed(const Library& lib, const std::string& name) { return !name.empty() && detail::HasTexture(lib, name); }

// What a unit wears, or on = false when it is not dressed this way (no database, no monster record,
// or a texture of its own in the map: only the "default0" placeholder is replaced).
inline Dress Resolve(const Library& lib, const mob::Object& o) {
    Dress d;
    if (o.kind != mob::Kind::Unit || Lower(o.primTexture) != "default0" || lib.unitsDb.monsters.empty()) return d;
    const std::string proto = mob::Utf8(o.prototype);
    const units::Monster* m = lib.unitsDb.FindMonster(proto);
    if (!m) m = lib.unitsDb.FindMonster(mob::Utf8(o.parentTemplate));
    if (!m) return d;
    const units::Race* race = lib.unitsDb.FindRace(m->race);
    const std::string mask = race && !race->mask.empty() ? Lower(race->mask) : Lower(o.templ);
    d.on = true;
    for (const std::string& p : o.bodyParts) d.bare.insert(Lower(p));
    // Skin: the monster's skin index picks one of the race's textures ("Skin_NN" = <mask>skin_NN): orc
    // races list one each (Dressed Skin_00, Naked Skin_01...), humans Skin_00 upwards. An index past the
    // list is taken as the NN itself.
    if (race && !race->primary.empty()) {
        if (m->skin >= 0 && m->skin < static_cast<int>(race->primary.size())) {
            d.skin = detail::RaceTexture(race->primary[static_cast<size_t>(m->skin)], mask);
        } else if (Lower(race->primary[0]).rfind("skin", 0) == 0) {
            char num[8];
            std::snprintf(num, sizeof(num), "%02d", m->skin);
            d.skin = mask + "skin_" + num;
        } else {
            d.skin = detail::RaceTexture(race->primary[0], mask);
        }
        // The second texture only when it is a real one ("W01", "AxeShield" are not files): parts that ask
        // for it keep the skin otherwise.
        if (!race->secondary.empty()) {
            const std::string second = detail::RaceTexture(race->secondary[0], mask);
            if (detail::HasTexture(lib, second)) d.secondSkin = second;
        }
    }
    if (m->hair >= 0 && (d.bare.empty() || d.bare.count("hr"))) {
        char num[8];
        std::snprintf(num, sizeof(num), "%02d", m->hair);
        d.selected["hr"] = std::string("hr.") + num;
    }
    d.summary = "dressed as " + m->name + (race ? " (" + race->name + ")" : "") + ", skin " + d.skin;
    // The map's own lists win over the monster's defaults.
    std::vector<std::string> weapons, armors;
    for (const mob::ItemList& l : o.lists) {
        if (l.type == mob::kUnitWeapons) for (const std::string& e : l.entries) weapons.push_back(mob::Utf8(e));
        if (l.type == mob::kUnitArmors) for (const std::string& e : l.entries) armors.push_back(mob::Utf8(e));
    }
    if (weapons.empty()) for (const std::string& w : {m->weapon1, m->weapon2}) if (!w.empty()) weapons.push_back(w);
    if (armors.empty()) armors = m->wears;
    auto split = [](const std::string& full, std::string& blueprint, std::string& material) {
        const size_t dot = full.find_last_of('.');
        if (dot == std::string::npos) return false;
        blueprint = full.substr(0, dot);
        material = full.substr(dot + 1);
        return true;
    };
    std::string bp, mat;
    for (const std::string& w : weapons) {
        if (!split(w, bp, mat)) continue;
        const items::Item* item = detail::FindItem(lib, items::Category::Weapons, bp);
        const detail::Slot* slot = item ? detail::WeaponSlot(item->type) : nullptr;
        if (!slot) continue;
        char num[8];
        std::snprintf(num, sizeof(num), "%02d", item->tti);
        const std::string variant = Lower(std::string(slot->group) + "." + slot->variant + num + slot->suffix);
        d.selected[slot->group] = variant;
        if (const items::Material* material = detail::FindMaterial(lib, mat)) {
            const std::string tex = detail::Redress(lib, mask, slot->code, item->tti, item->tti2, Lower(material->code));
            if (!tex.empty()) d.texture[variant] = tex;
        }
    }
    // The body texture is composed like the game's: the skin, then each worn item's texture painted over it
    // (inside out: pants, boots, leggings, shirt, plate, gloves, helm). The body and the armor meshes all
    // use it; their UVs point into that one picture.
    std::vector<std::pair<int, std::string>> worn;
    auto rank = [](const std::string& type) {
        static const char* const order[] = {"pants", "boots", "leggings", "shirt", "plate", "gloves", "helm"};
        for (int i = 0; i < 7; ++i) if (Lower(type) == order[i]) return i;
        return 7;
    };
    for (const std::string& a : armors) {
        if (!split(a, bp, mat)) continue;
        const items::Item* item = detail::FindItem(lib, items::Category::Armors, bp);
        const std::vector<std::string>* parts = item ? detail::ArmorParts(item->type) : nullptr;
        if (!parts) continue;
        std::string tex;
        if (const char* code = detail::ArmorCode(item->type))
            if (const items::Material* material = detail::FindMaterial(lib, mat)) tex = detail::Redress(lib, mask, code, item->tti, item->tti2, Lower(material->code));
        if (!tex.empty()) worn.push_back({rank(item->type), tex});
        if (!detail::TextureOnly(item->type)) {
            char num[8];
            std::snprintf(num, sizeof(num), "%02d", item->tti);
            for (const std::string& p : *parts) d.selected[p + ":armor"] = p + ".armor" + num;
        }
    }
    std::stable_sort(worn.begin(), worn.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    d.body = "compose:" + d.skin;
    for (const auto& w : worn) d.body += "|" + w.second;
    if (!HasTextureNamed(lib, d.skin)) d.summary += " (not found)";
    return d;
}

// Whether a part of the figure shows on the dressed unit (walking up from it through its ancestors).
inline bool PartShown(const Dress& d, const fig::Model& model, const fig::ModelPart& part) {
    auto selectedFor = [&](const std::string& group) {
        auto it = d.selected.find(group);
        return it == d.selected.end() ? std::string() : it->second;
    };
    // An armor piece only counts when this figure has its mesh (not every race has every number).
    auto hasMesh = [&](const std::string& name) { return !name.empty() && model.FindPartIndex(name) >= 0; };
    const bool helm = hasMesh(selectedFor("hd:armor"));
    const WeaponPiece piece = PieceOf(Lower(part.name));
    if (piece.slot) { // a bow's or crossbow's piece: only those of the equipped weapon's number
        const WeaponPiece chosen = PieceOf(selectedFor(piece.slot));
        return chosen.slot && chosen.number == piece.number && piece.number >= 0;
    }
    const fig::ModelPart* cur = &part;
    std::string lastGroup;
    bool validated = false;
    while (true) {
        const std::string name = Lower(cur->name);
        if (name == "quiver") {
            bool ranged = false;
            for (const char* g : {"rh3", "lh3"}) {
                const std::string s = selectedFor(g);
                ranged |= s.find(".bwpartb") != std::string::npos || s.find(".crbow") != std::string::npos;
            }
            if (!ranged) return false;
        } else if (name == "baserh3" || name == "baselh3") {
            if (!selectedFor(name.substr(4)).empty()) return false; // a weapon replaces the empty hand
        } else if (name.find('.') != std::string::npos) {
            const std::string group = Group(name);
            if (group == "hr" && helm) return false;
            if (group != lastGroup || !validated) {
                const std::string selected = selectedFor(group);
                if (selected.empty()) return false;
                bool matches = selected == name;
                if (!matches && MultiPieceWeapon(name)) { // a shared piece of the chosen bow or crossbow
                    int idx = model.FindPartIndex(selected);
                    while (idx >= 0 && !matches) {
                        matches = Lower(model.parts[static_cast<size_t>(idx)].name) == name;
                        const std::string& parent = model.parts[static_cast<size_t>(idx)].parentName;
                        idx = parent.empty() ? -1 : model.FindPartIndex(parent);
                    }
                }
                if (!matches) return false;
                lastGroup = group;
                validated = true;
            }
        } else if (cur == &part) {
            if (name.size() <= 3 && !d.bare.empty() && !d.bare.count(name)) return false;  // OBJ_BODYPARTS (the body slots)
            if (name != "hd" && hasMesh(selectedFor(name + ":armor"))) return false;       // replaced by its armor mesh
        }
        if (cur->parentName.empty()) return true;
        const int parent = model.FindPartIndex(cur->parentName);
        if (parent < 0) return true;
        cur = &model.parts[static_cast<size_t>(parent)];
    }
}

// The texture a shown part is drawn with: its own item's (or an ancestor variant's, for pieces hanging
// off it), else the skin.
inline std::string PartTexture(const Dress& d, const fig::Model& model, const fig::ModelPart& part) {
    const WeaponPiece piece = PieceOf(Lower(part.name));
    if (piece.slot) { // every piece of the equipped bow or crossbow wears its texture
        auto sel = d.selected.find(piece.slot);
        if (sel != d.selected.end()) {
            auto tex = d.texture.find(sel->second);
            if (tex != d.texture.end()) return tex->second;
        }
    }
    const fig::ModelPart* cur = &part;
    while (cur) {
        auto it = d.texture.find(Lower(cur->name));
        if (it != d.texture.end()) return it->second;
        if (cur->parentName.empty()) break;
        const int parent = model.FindPartIndex(cur->parentName);
        cur = parent < 0 ? nullptr : &model.parts[static_cast<size_t>(parent)];
    }
    return part.mesh.textureNumber == 1 && !d.secondSkin.empty() ? d.secondSkin : d.body;
}

} // namespace dress
