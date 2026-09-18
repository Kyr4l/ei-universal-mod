// Reads the "Items" gameplay database (Materials/Weapons/Armors blocks) from
// either a database RES archive (databaselmp.res's "items.idb" tagged-value
// blob, the same archive units.udb lives in - see db_units.hpp) or a
// database.xlsx spreadsheet, giving the model viewer enough to resolve an
// equipped weapon/armor item name into the mesh variant + material texture
// that should actually be displayed.
//
// Field layout per docs/file-formats/database-format.md and
// tools/third-party-tools/EIDBEditor_1.4.4/dbtypes.txt + dbheaders.txt
// ("Items" table); verified byte-exact against the real
// Universal-Mod/res/databaselmp.res during development (e.g. "bronze" ->
// Code "br", "stone axe" -> Type "axe" TTI 1, matching the community-known
// material-code convention used by redress_res texture filenames).
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "db_units.hpp" // reuses ReadTag/ReadFields/ReadCp1251String/ReadFloatField, ColMap/BuildColumnMap/ColFor
#include "res_archive.hpp"
#include "xlsx_reader.hpp"

namespace db {

struct Material {
    std::string name;
    std::string type; // broad material class, e.g. "Leather", "Metal" - not the texture code
    std::string code; // 2-letter texture-suffix code used in redress_res filenames, e.g. "br", "st", "al"
    // Base damage/absorb multiplier (Materials sheet field 10, "Damage") -
    // verified byte-exact against tools/dmg-calculator.ods (e.g. "granite"=3.6
    // matches that sheet's STONE block "granite" DMG cell). Combines with a
    // weapon blueprint's own dMin/dMax to give its damage range, or with an
    // armor blueprint's own armorAbsorb to give its defense value.
    //
    // The Materials sheet also has a field 11 with 7 per-damage-type values
    // (Piercing/Slashing/Bludg/Termal/Chemic/Electric/General) that mostly
    // repeat this same `damage` value - EXCEPT one type-specific "weakness"
    // outlier at exactly 80% of it (e.g. "thin" leather: Damage=3.5, but its
    // own Piercing=2.8=3.5*0.8) - confirmed intentional, not noise. Ignored
    // here rather than modeled: this viewer shows one overall number, not a
    // per-damage-type table, so `damage` (the shared/base value) is what to use.
    float damage = 0.0f;
};

// A weapon or armor "blueprint" (an Items DB row): TTI ("Texture Type Index")
// selects which numbered mesh variant to display - verified exact (not off by
// one) against real .mod hierarchies: e.g. Armors "helm" TTI values are
// exactly 1..16, matching unhuma.mod's hd.armor01..hd.armor16 one-for-one.
struct ItemBlueprint {
    std::string name;
    std::string type;         // e.g. "axe"/"sword"/... for weapons, "helm"/"plate"/... for armors
    int typeId = 0;           // fixed per-Type constant (e.g. every "shirt" row is 3) - see main.cpp's
                               // ResolveArmorVariant for why this, not TTI, selects the mesh for some armor types
    std::string materialType; // broad tier ("Stone"/"Metal"/"Crystal"...) - independent of the chosen Material
    int tti = 0;
    int tti2 = 0;              // redress_res filename's trailing "subvariant" digit - confirmed a direct DB field, not a guess

    // Weapons only (fields 23-30 of the Weapons sheet; unset/zero for armors,
    // which don't have these columns). Verified against tools/dmg-calculator.ods:
    // that sheet's "STONE WEAPON" DMIN/DMAX (2.65/2) are exactly "stone axe"'s
    // own dMin/dMax here, not a fixed per-material-class constant - every
    // weapon blueprint has its own pair. Actual damage = the equipped
    // Material's own `damage` multiplier combined with these:
    //   MinDamage = material.damage * dMin
    //   MaxDamage = material.damage * (dMin + dMax)
    // (see ComputeWeaponDamage in main.cpp).
    float range = 0.0f;    // attack reach/distance - 0 for melee, >0 for spear/bow/crossbow
    float dMin = 0.0f;
    float dMax = 0.0f;
    float attack = 0.0f;   // flat to-hit style combat modifier
    float defence = 0.0f;  // flat evasion style combat modifier (often negative)
    // Damage-type split (DP.Piercing/Slashing/Bludgeoning/Thermal/Chemical/
    // Electric/General) - observed as a one-hot vector per weapon type in
    // every sampled row (e.g. dagger/spear/bow/crossbow = 100% Piercing,
    // hammer/club = 100% Bludgeoning) but stored as 7 independent fields, so
    // treated as general fractional weights rather than assumed one-hot.
    float dpPiercing = 0.0f, dpSlashing = 0.0f, dpBludgeoning = 0.0f, dpThermal = 0.0f,
          dpChemical = 0.0f, dpElectric = 0.0f, dpGeneral = 0.0f;

    // Armors only (field 21 idx 0, "M.Absorb"; unset/zero for weapons). This
    // is the base defense value: verified against a full "gipat"/"hadagan"
    // tier ladder in the real DB that it's always populated and scales
    // cleanly (heavy slots - helm/plate/leggings - are always exactly 2x the
    // light slots - shirt/pants/boots/gloves - within the same set/tier).
    // Two things were deliberately left unused after checking real data:
    //   - "A.Absorb" (field 22 idx 0), a parallel value that equals M.Absorb
    //     for most slots but drops to exactly 0 for some (mostly "shirt",
    //     occasionally "plate") with no tier-based pattern explaining why -
    //     looks like incomplete/vestigial data rather than a real modifier.
    //   - Both M. and A.'s own 7-value per-damage-type breakdowns (Piercing.
    //     .General): confirmed flat 1.0 across every sampled row, i.e. no
    //     armor blueprint encodes an elemental weakness of its own (unlike
    //     Materials, see Material::damage's comment).
    float armorAbsorb = 0.0f;
};

struct ItemDatabase {
    std::vector<Material> materials;
    std::vector<ItemBlueprint> weapons;
    std::vector<ItemBlueprint> armors;

    const Material* FindMaterial(const std::string& name) const {
        for (auto& m : materials) if (m.name == name) return &m;
        return nullptr;
    }
    const ItemBlueprint* FindWeapon(const std::string& name) const {
        for (auto& w : weapons) if (w.name == name) return &w;
        return nullptr;
    }
    const ItemBlueprint* FindArmor(const std::string& name) const {
        for (auto& a : armors) if (a.name == name) return &a;
        return nullptr;
    }
};

// TTI is stored as a SignedLong field, not a Float - db_units.hpp's ReadFloatField
// would reinterpret the bytes as IEEE-754, so read it as a plain little-endian int32.
inline int32_t ReadFloatFieldAsInt(const uint8_t* data, const TagSpan& span) {
    if (span.contentLen < 4) return 0;
    int32_t v;
    std::memcpy(&v, data + span.contentStart, 4);
    return v;
}

// Parses one "items.idb" blob (already extracted from a database RES archive).
inline bool ParseItemsResBlob(const std::vector<uint8_t>& idb, ItemDatabase& out) {
    const uint8_t* data = idb.data();
    size_t size = idb.size();
    size_t off = 0;
    TagSpan file;
    if (!ReadTag(data, size, off, file)) return false;

    size_t blockOff = file.contentStart;
    size_t blockEnd = file.contentStart + file.contentLen;
    while (blockOff < blockEnd) {
        TagSpan block;
        if (!ReadTag(data, blockEnd, blockOff, block)) break;

        // block.tag (see dbblocks.txt "Items"): 1=Materials, 2=Weapons, 3=Armors,
        // 4=QuickItems, 5=QuestItems, 6=LootItems - only the first three matter here.
        if (block.tag != 1 && block.tag != 2 && block.tag != 3) continue;

        size_t recOff = block.contentStart;
        size_t recEnd = block.contentStart + block.contentLen;
        while (recOff < recEnd) {
            TagSpan record;
            if (!ReadTag(data, recEnd, recOff, record)) break;
            auto fields = ReadFields(data, record.contentStart, record.contentLen);

            if (block.tag == 1) { // Materials: 0=Name, 2=Code, 10=Damage
                Material m;
                if (auto it = fields.find(0); it != fields.end()) m.name = ReadCp1251String(data, it->second);
                if (auto it = fields.find(1); it != fields.end()) m.type = ReadCp1251String(data, it->second);
                if (auto it = fields.find(2); it != fields.end()) m.code = ReadCp1251String(data, it->second);
                if (auto it = fields.find(10); it != fields.end()) m.damage = ReadFloatField(data, it->second);
                if (!m.name.empty()) out.materials.push_back(std::move(m));
            } else { // Weapons/Armors share the same layout: 0=Name, 1=Type, 2=TypeID, 3=M.Type, 5=TTI, 6=TTI2
                ItemBlueprint b;
                if (auto it = fields.find(0); it != fields.end()) b.name = ReadCp1251String(data, it->second);
                if (auto it = fields.find(1); it != fields.end()) b.type = ReadCp1251String(data, it->second);
                if (auto it = fields.find(2); it != fields.end()) b.typeId = static_cast<int>(ReadFloatFieldAsInt(data, it->second));
                if (auto it = fields.find(3); it != fields.end()) b.materialType = ReadCp1251String(data, it->second);
                if (auto it = fields.find(5); it != fields.end()) b.tti = static_cast<int>(ReadFloatFieldAsInt(data, it->second));
                if (auto it = fields.find(6); it != fields.end()) b.tti2 = static_cast<int>(ReadFloatFieldAsInt(data, it->second));
                if (block.tag == 2) { // Weapons only: 23=Range, 24=D.Min, 25=D.Max, 29=Attack, 30=Defence.
                    // The DP.Piercing..DP.General damage-type split (field 26,
                    // 7 subfields) isn't parsed here: unlike the scalars above,
                    // it's unconfirmed whether the binary blob packs those as
                    // one multi-value tag or several sequential tags, and the
                    // XLSX path (this project's only actually-configured
                    // database source - see um-modelviewer.cfg) covers it.
                    if (auto it = fields.find(23); it != fields.end()) b.range = ReadFloatField(data, it->second);
                    if (auto it = fields.find(24); it != fields.end()) b.dMin = ReadFloatField(data, it->second);
                    if (auto it = fields.find(25); it != fields.end()) b.dMax = ReadFloatField(data, it->second);
                    if (auto it = fields.find(29); it != fields.end()) b.attack = ReadFloatField(data, it->second);
                    if (auto it = fields.find(30); it != fields.end()) b.defence = ReadFloatField(data, it->second);
                } else if (block.tag == 3) { // Armors only: 21=M.Absorb (see ItemBlueprint::armorAbsorb).
                    if (auto it = fields.find(21); it != fields.end()) b.armorAbsorb = ReadFloatField(data, it->second);
                }
                if (b.name.empty()) continue;
                if (block.tag == 2) out.weapons.push_back(std::move(b));
                else out.armors.push_back(std::move(b));
            }
        }
    }
    return true;
}

// Loads from a databaselmp.res file's bytes (looks up the "items.idb" entry inside -
// the same archive db_units.hpp's LoadUnitsFromRes reads "units.udb" from).
inline bool LoadItemsFromRes(const std::vector<uint8_t>& resBytes, ItemDatabase& out, std::string& err) {
    res::Archive archive;
    if (!res::ParseArchive(resBytes, archive, err)) return false;
    const std::vector<uint8_t>* idb = archive.Find("items.idb");
    if (!idb) { err = "items.idb not found in archive"; return false; }
    if (!ParseItemsResBlob(*idb, out)) { err = "failed to parse items.idb"; return false; }
    return true;
}

// ----------------------------------------------------------------------------
// XLSX loading (database.xlsx: sheets "Materials"/"Weapons"/"Armors"), using the
// same row-3 "FLDx-y" column markers db_units.hpp's BuildColumnMap reads.
// ----------------------------------------------------------------------------

inline bool LoadItemsFromXlsx(const std::string& xlsxPath, ItemDatabase& out, std::string& err) {
    try {
        xlsxlib::Workbook wb(xlsxPath);
        // Shared helper: fields 0.../7-1... etc. are exported as plain numeric
        // strings, never blank-but-present, so "empty -> leave default" is the
        // same guard every one of these needs.
        auto readFloat = [](const xlsxlib::Sheet& ws, int row, const ColMap& cols, int fieldId, int idx, float& dest) {
            if (auto c = ColFor(cols, fieldId, idx)) {
                auto s = ws.Get(row, *c).AsString();
                if (!s.empty()) dest = std::stof(s);
            }
        };
        if (wb.HasSheet("Materials")) {
            const auto& ws = wb.GetSheet("Materials");
            ColMap cols = BuildColumnMap(ws);
            for (int row = 4; row <= ws.MaxRow(); ++row) {
                Material m;
                if (auto c = ColFor(cols, 0)) m.name = ws.Get(row, *c).AsString();
                if (m.name.empty()) continue;
                if (auto c = ColFor(cols, 1)) m.type = ws.Get(row, *c).AsString();
                if (auto c = ColFor(cols, 2)) m.code = ws.Get(row, *c).AsString();
                readFloat(ws, row, cols, 10, 0, m.damage);
                out.materials.push_back(std::move(m));
            }
        }
        auto loadBlueprints = [&](const char* sheetName, std::vector<ItemBlueprint>& dest, bool isWeapons) {
            if (!wb.HasSheet(sheetName)) return;
            const auto& ws = wb.GetSheet(sheetName);
            ColMap cols = BuildColumnMap(ws);
            for (int row = 4; row <= ws.MaxRow(); ++row) {
                ItemBlueprint b;
                if (auto c = ColFor(cols, 0)) b.name = ws.Get(row, *c).AsString();
                if (b.name.empty()) continue;
                if (auto c = ColFor(cols, 1)) b.type = ws.Get(row, *c).AsString();
                if (auto c = ColFor(cols, 2)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) b.typeId = static_cast<int>(std::stof(s)); }
                if (auto c = ColFor(cols, 3)) b.materialType = ws.Get(row, *c).AsString();
                if (auto c = ColFor(cols, 5)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) b.tti = static_cast<int>(std::stof(s)); }
                if (auto c = ColFor(cols, 6)) { auto s = ws.Get(row, *c).AsString(); if (!s.empty()) b.tti2 = static_cast<int>(std::stof(s)); }
                if (isWeapons) { // Armors sheet doesn't have these columns - see ItemBlueprint's comment.
                    readFloat(ws, row, cols, 23, 0, b.range);
                    readFloat(ws, row, cols, 24, 0, b.dMin);
                    readFloat(ws, row, cols, 25, 0, b.dMax);
                    readFloat(ws, row, cols, 26, 0, b.dpPiercing);
                    readFloat(ws, row, cols, 26, 1, b.dpSlashing);
                    readFloat(ws, row, cols, 26, 2, b.dpBludgeoning);
                    readFloat(ws, row, cols, 26, 3, b.dpThermal);
                    readFloat(ws, row, cols, 26, 4, b.dpChemical);
                    readFloat(ws, row, cols, 26, 5, b.dpElectric);
                    readFloat(ws, row, cols, 26, 6, b.dpGeneral);
                    readFloat(ws, row, cols, 29, 0, b.attack);
                    readFloat(ws, row, cols, 30, 0, b.defence);
                } else { // Armors only - see ItemBlueprint::armorAbsorb.
                    readFloat(ws, row, cols, 21, 0, b.armorAbsorb);
                }
                dest.push_back(std::move(b));
            }
        };
        loadBlueprints("Weapons", out.weapons, true);
        loadBlueprints("Armors", out.armors, false);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

} // namespace db
