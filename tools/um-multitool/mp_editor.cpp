// File Processing > MP: the multiplayer characters of a folder (<game>/mp), and one of them edited: name and clan
// tag, experience, money, attributes, skills, abilities, items (with their database names), equipment and quest
// variables, with checks like the DB tab's. See mp_file.hpp for the format.
//
// Safeguards: what can break a character (its unit, prototype, zone, quest variables, the items list itself) is
// read-only until "Allow unsafe edits" is ticked.
#include "mp_editor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "db_model.hpp"
#include "mp_file.hpp"
#include "viewer/library.hpp"
#include "viewer/ui_common.hpp"

namespace mpedit {
namespace fs = std::filesystem;

namespace {

struct Entry { std::string path, file, name, zone, error; float exp = 0; };
struct Finding { char level; std::string text; }; // 'E' error, 'W' warning

struct State {
    char folder[1024] = "";
    bool folderInit = false;
    std::vector<Entry> files;
    int selected = -1;
    mp::Character ch;
    std::vector<uint8_t> savedBytes; // Serialize(ch) as loaded / last saved (for the unsaved mark)
    std::string message;
    bool unsafe = false;              // "Allow unsafe edits"
    bool showRaw = false;
    // the database's names (sheet -> rows), by path
    std::string dbFor;
    std::map<std::string, std::vector<std::string>> sheets;
};
State g;

void LoadDatabase(const Library& lib) {
    if (g.dbFor == lib.dbPath) return;
    g.dbFor = lib.dbPath;
    g.sheets.clear();
    if (lib.dbPath.empty()) return;
    dbmodel::Book book;
    std::string err;
    if (!dbmodel::LoadBook(lib.dbPath, book, err)) return;
    for (const sheetio::Sheet& s : book) {
        std::vector<std::string>& names = g.sheets[s.name];
        int col = 1; // the "Name" column of the header row (row 2; the sheets often start at column B)
        for (int c = 1; c <= s.maxCol; ++c) {
            auto h = s.cells.find({2, c});
            if (h != s.cells.end() && h->second.text == "Name") { col = c; break; }
        }
        for (int row = 4; row <= s.maxRow; ++row) { // rows 1-3: the headers
            auto it = s.cells.find({row, col});
            names.push_back(it == s.cells.end() ? std::string() : it->second.text);
        }
    }
}
const std::vector<std::string>* Sheet(const char* name) {
    auto it = g.sheets.find(name);
    return it == g.sheets.end() ? nullptr : &it->second;
}
std::string Row(const char* sheet, int index) {
    const auto* s = Sheet(sheet);
    if (!s || index < 0 || index >= static_cast<int>(s->size())) return "#" + std::to_string(index);
    return (*s)[index];
}
// The sheet an object kind's row (b) indexes; Materials for a (weapons, armour).
const char* KindSheet(uint32_t kind) {
    switch (kind) {
    case 0x3002: return "SpellPrototypes";
    case 0x3004: return "Weapons";
    case 0x3005: return "Armors";
    case 0x3006: case 0x3008: return "QuickItems";
    case 0x3007: return "LootItems"; // checked in the game's memory: row 1 "material" (a = the material), 52 "lmp toad 1"
    case 0x3009: return "QuestItems";
    }
    return nullptr;
}
// Weapons and armour have a material; so do loot items (only the "material" row uses it: material.granite).
bool HasMaterial(uint32_t kind) { return kind == 0x3004 || kind == 0x3005 || kind == 0x3007; }
std::string ObjectName(const mp::Object& o) {
    const char* sheet = KindSheet(o.kind);
    if (!sheet) return "?";
    std::string n = Row(sheet, o.b);
    if (o.kind == 0x3007) { if (n == "material") n += "." + Row("Materials", o.a); }
    else if (HasMaterial(o.kind)) n += " (" + Row("Materials", o.a) + ")";
    return n;
}
mp::Object* FindObject(uint32_t id) {
    for (auto& list : g.ch.lists)
        for (mp::Object& o : list) if (o.id == id) return &o;
    return nullptr;
}
// A weapon's attached spell, or a spell container's spell: the object id at +40 of the block (-1: none).
uint32_t HeldSpell(const mp::Object& o) {
    if (o.detail.size() < 44 || (o.kind != 0x3004 && o.kind != 0x3005 && o.kind != 0x3008)) return 0xFFFFFFFFu;
    uint32_t ref;
    std::memcpy(&ref, o.detail.data() + 40, 4);
    return ref;
}
std::string IdName(uint32_t id) {
    if (id == 0xFFFFFFFFu) return "(empty)";
    const mp::Object* o = FindObject(id);
    if (!o) return "missing object " + std::to_string(id);
    std::string n = ObjectName(*o);
    if (o->kind == 0x3008) if (const mp::Object* s = FindObject(HeldSpell(*o))) n = ObjectName(*s) + " (" + n + ")";
    return n;
}

void Rescan() {
    g.files.clear();
    std::error_code ec;
    if (!fs::is_directory(g.folder, ec)) return;
    for (const auto& e : fs::directory_iterator(g.folder, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".mp") continue;
        Entry en;
        en.path = e.path().string();
        en.file = e.path().filename().string();
        mp::Character c;
        if (mp::Load(en.path, c, en.error) && !c.members.empty()) {
            en.name = c.members[0].strings[0];
            en.zone = c.zone;
            en.exp = mp::GetF(c.members[0].stats, mp::kExpTotal);
        }
        g.files.push_back(en);
    }
    std::sort(g.files.begin(), g.files.end(), [](const Entry& a, const Entry& b) { return a.file < b.file; });
}

void Open(int i) {
    g.selected = i;
    g.message.clear();
    std::string err;
    if (!mp::Load(g.files[i].path, g.ch, err)) { g.message = err; g.selected = -1; return; }
    g.savedBytes = mp::Serialize(g.ch);
}
bool Dirty() { return g.selected >= 0 && mp::Serialize(g.ch) != g.savedBytes; }

void SaveCurrent() {
    const std::string path = g.files[g.selected].path;
    std::error_code ec;
    const bool backup = !fs::exists(path + ".bak", ec);
    if (backup) fs::copy_file(path, path + ".bak", ec); // the first save keeps the original
    std::string err;
    if (!mp::Save(path, g.ch, err)) { g.message = "Not saved: " + err; return; }
    g.savedBytes = mp::Serialize(g.ch);
    g.message = "Saved" + std::string(backup ? " (the original is kept as .bak)" : "");
    const int sel = g.selected;
    Rescan();
    g.selected = sel;
}

// ---- checks ---------------------------------------------------------------------------------------------
std::vector<Finding> Check() {
    std::vector<Finding> out;
    auto err = [&](const std::string& t) { out.push_back({'E', t}); };
    auto warn = [&](const std::string& t) { out.push_back({'W', t}); };
    if (g.ch.members.empty()) { err("No character in the file"); return out; }
    const mp::Member& m = g.ch.members[0];
    if (m.strings[0].empty()) err("The name is empty");
    if (std::count(m.strings[0].begin(), m.strings[0].end(), '|') > 1) warn("The name has more than one '|' (name | clan tag)");
    if (m.strings[0].size() > 31) warn("The name is long (" + std::to_string(m.strings[0].size()) + " characters): the game may cut it");
    const float total = mp::GetF(m.stats, mp::kExpTotal), spent = mp::GetF(m.stats, mp::kExpSpent);
    if (spent > total) err("Experience spent (" + std::to_string(static_cast<long long>(spent)) + ") is more than the total");
    if (total < 0 || spent < 0) err("Negative experience");
    for (int a = 0; a < 3; ++a)
        if (mp::GetF(m.stats, 0x0C + a * 8) <= 0 || mp::GetF(m.stats, 0x10 + a * 8) <= 0) err("An attribute is 0 or negative");
    for (int p = 0; p < mp::kPerkGroups; ++p)
        if (m.stats[mp::kPerkBase + p] > 3) err(std::string("Ability ") + mp::PerkGroupName(p) + " is above level 3");
    // references
    std::vector<uint32_t> refs = g.ch.backpack;
    for (const auto& l : m.lists) refs.insert(refs.end(), l.begin(), l.end());
    for (uint32_t id : refs)
        if (id != 0xFFFFFFFFu && !FindObject(id)) err("Equipment or backpack refers to object " + std::to_string(id) + ", which is not in the file");
    std::map<uint32_t, int> ids;
    for (const auto& list : g.ch.lists)
        for (const mp::Object& o : list) {
            if (++ids[o.id] == 2) err("Object id " + std::to_string(o.id) + " is used twice");
            const std::string what = ObjectName(o) + " (object " + std::to_string(o.id) + ")";
            if (const char* sheet = KindSheet(o.kind)) {
                const auto* rows = Sheet(sheet);
                if (rows && o.b >= rows->size()) err(what + ": row " + std::to_string(o.b) + " is not in the database's " + sheet);
                const auto* mats = Sheet("Materials");
                if (HasMaterial(o.kind) && mats && o.a >= mats->size()) err(what + ": material " + std::to_string(o.a) + " is not in the database");
            }
            if (o.kind == 0x3004 || o.kind == 0x3005) {
                const float dur = mp::GetF(o.detail, 8), max = mp::GetF(o.detail, 12);
                if (dur > max) warn(what + ": durability " + std::to_string(dur) + " is above its max");
                if (max <= 0) warn(what + ": durability max is 0");
                if (mp::GetF(o.detail, 16) > mp::GetF(o.detail, 20)) warn(what + ": energy " + std::to_string(mp::GetF(o.detail, 16)) + " is above its max");
            }
            const uint32_t held = HeldSpell(o);
            if (held != 0xFFFFFFFFu && !FindObject(held)) err(what + ": its spell (object " + std::to_string(held) + ") is missing");
        }
    if (Sheet("Weapons") == nullptr) warn("No database set in Settings: item rows are not checked");
    return out;
}

// ---- items: remove, duplicate, change ----------------------------------------------------------------------
uint32_t NewId() {
    uint32_t top = 0;
    for (const auto& list : g.ch.lists) for (const mp::Object& o : list) top = std::max(top, o.id);
    return top + 2; // the game numbers objects by 2
}
void RemoveRefs(uint32_t id) {
    mp::Member& m = g.ch.members[0];
    for (auto& l : m.lists) l.erase(std::remove(l.begin(), l.end(), id), l.end());
    g.ch.backpack.erase(std::remove(g.ch.backpack.begin(), g.ch.backpack.end(), id), g.ch.backpack.end());
}
void RemoveObject(uint32_t id) {
    const mp::Object* o = FindObject(id);
    if (!o) return;
    const uint32_t held = HeldSpell(*o);
    for (auto& list : g.ch.lists)
        list.erase(std::remove_if(list.begin(), list.end(), [&](const mp::Object& x) { return x.id == id; }), list.end());
    RemoveRefs(id);
    if (held != 0xFFFFFFFFu) RemoveObject(held); // its spell goes with it
}
void SetDetailId(mp::Object& o) { if (o.detail.size() >= 4) std::memcpy(o.detail.data(), &o.id, 4); }
// A copy of an item (and of its spell), put in the backpack.
void DuplicateObject(uint32_t id) {
    const mp::Object* src = FindObject(id);
    if (!src) return;
    mp::Object copy = *src;
    copy.id = NewId();
    SetDetailId(copy);
    const uint32_t held = HeldSpell(*src);
    if (const mp::Object* spell = held != 0xFFFFFFFFu ? FindObject(held) : nullptr) {
        mp::Object sc = *spell;
        sc.id = copy.id + 2;
        SetDetailId(sc);
        std::memcpy(copy.detail.data() + 40, &sc.id, 4);
        g.ch.lists[1].push_back(sc);
    }
    g.ch.lists[0].push_back(copy);
    g.ch.backpack.push_back(copy.id);
}
// Another database row (and material): the list record and the block's header; the block's other values
// (durability, energy, protection) stay those of the old item until the game updates them.
void SetRow(mp::Object& o, uint16_t a, uint16_t b) {
    o.a = a;
    o.b = b;
    if (o.kind != 0x3002 && o.detail.size() >= 8) { std::memcpy(o.detail.data() + 4, &a, 2); std::memcpy(o.detail.data() + 6, &b, 2); }
}

// ---- widgets -----------------------------------------------------------------------------------------
// Read-only fields look dimmer than the editable ones (text and box).
void PushLocked(bool locked) {
    if (!locked) return;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, st.Colors[ImGuiCol_TextDisabled]);
    ImVec4 bg = st.Colors[ImGuiCol_FrameBg];
    bg.w *= 0.35f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bg);
}
void PopLocked(bool locked) { if (locked) ImGui::PopStyleColor(4); }
bool InputString(const char* label, std::string& s, float width = 260, bool locked = false) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    PushLocked(locked);
    const bool changed = ImGui::InputText(label, buf, sizeof buf, locked ? ImGuiInputTextFlags_ReadOnly : 0);
    PopLocked(locked);
    if (!changed) return false;
    s = buf;
    return true;
}
// A float field: plain digits (no "e+07"), more decimals only when it has them.
bool InputFloatField(const char* label, std::vector<uint8_t>& block, int at, float width = 140, bool locked = false) {
    float v = mp::GetF(block, at);
    const bool whole = v == static_cast<float>(static_cast<long long>(v));
    ImGui::SetNextItemWidth(width);
    PushLocked(locked);
    const bool changed = ImGui::InputFloat(label, &v, 0, 0, whole ? "%.0f" : "%.2f", locked ? ImGuiInputTextFlags_ReadOnly : 0);
    PopLocked(locked);
    if (!changed) return false;
    mp::SetF(block, at, v);
    return true;
}
void Locked() {
    if (!g.unsafe) ImGui::SetItemTooltip("Read-only: changing it can break the character. Tick \"Allow unsafe edits\" to change it.");
}

void CharacterPanel() {
    mp::Member& m = g.ch.members[0];
    ImGui::SeparatorText("Character");
    // The game's names are "<name> | <clan tag>".
    std::string& full = m.strings[0];
    const size_t bar = full.find('|');
    std::string name = bar == std::string::npos ? full : full.substr(0, bar), tag = bar == std::string::npos ? "" : full.substr(bar + 1);
    auto trim = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        return s;
    };
    name = trim(name);
    tag = trim(tag);
    bool changed = InputString("Name", name);
    ImGui::SetItemTooltip("The character's name (the game shows \"<name> | <clan tag>\")");
    changed |= InputString("Clan tag", tag, 140);
    ImGui::SetItemTooltip("After the '|' in the game; empty: no tag");
    if (changed) full = tag.empty() ? name : name + " | " + tag;
    InputString("Unit name", m.strings[4], 260, !g.unsafe);
    Locked();
    InputString("Prototype", m.s6, 260, !g.unsafe);
    Locked();
    InputString("Figure", m.strings[1], 140, !g.unsafe);
    Locked();
    InputString("Zone", g.ch.zone, 140, !g.unsafe);
    ImGui::SetItemTooltip("The map it was saved in (e.g. bz1mpg, or a quest's zone)%s", g.unsafe ? "" : "\nRead-only: tick \"Allow unsafe edits\" to change it.");

    ImGui::SeparatorText("Experience and money");
    InputFloatField("Experience (total)", m.stats, mp::kExpTotal, 160);
    InputFloatField("Experience (spent)", m.stats, mp::kExpSpent, 160, !g.unsafe);
    ImGui::SetItemTooltip("Spent on skills and abilities; the game's \"Your experience\" is total - spent");
    ImGui::Text("Free experience: %.0f", mp::GetF(m.stats, mp::kExpTotal) - mp::GetF(m.stats, mp::kExpSpent));
    uint32_t money = mp::Money(g.ch);
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputScalar("Money", ImGuiDataType_U32, &money)) mp::SetMoney(g.ch, money);

    ImGui::SeparatorText("Attributes");
    int count = 0;
    const mp::StatField* f = mp::StatFields(count);
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        InputFloatField(f[i].name, m.stats, f[i].offset, 140, !g.unsafe);
        if (f[i].tip[0]) ImGui::SetItemTooltip("%s%s", f[i].tip, g.unsafe ? "" : "\nRead-only: tick \"Allow unsafe edits\" to change it.");
        ImGui::PopID();
    }
    ImGui::TextDisabled("Health and stamina are computed by the game (body, abilities).");
    ImGui::SetNextItemWidth(260);
    ImGui::InputFloat3("Complection", m.complection, "%.3f");
    ImGui::SetItemTooltip("Body proportions (0..1): the figure's morphs");

    ImGui::SeparatorText("Skills");
    const mp::SkillByte* sk = mp::Skills(count);
    for (int i = 0; i < count; ++i) {
        int v = m.stats[sk[i].offset];
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputInt(sk[i].name, &v)) m.stats[sk[i].offset] = static_cast<uint8_t>(std::clamp(v, 0, 255));
    }
    ImGui::SeparatorText("Abilities");
    static const char* const kLevels[4] = {"-", "1 (Specialist)", "2 (Expert)", "3 (Master)"};
    for (int p = 0; p < mp::kPerkGroups; ++p) {
        int v = std::min<int>(m.stats[mp::kPerkBase + p], 3);
        ImGui::SetNextItemWidth(140);
        PushLocked(!g.unsafe);
        ImGui::BeginDisabled(!g.unsafe);
        if (ImGui::Combo(mp::PerkGroupName(p), &v, kLevels, 4)) m.stats[mp::kPerkBase + p] = static_cast<uint8_t>(v);
        ImGui::EndDisabled();
        PopLocked(!g.unsafe);
        if (const auto* perks = Sheet("Perks"); perks && v > 0 && p * 3 + v - 1 < static_cast<int>(perks->size()))
            ImGui::SetItemTooltip("%s", (*perks)[p * 3 + v - 1].c_str());
    }

    ImGui::Checkbox("All parameters (advanced)", &g.showRaw);
    if (g.showRaw && ImGui::BeginTable("raw", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 300))) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Integer", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Float");
        ImGui::TableSetupColumn("Name");
        ImGui::TableHeadersRow();
        f = mp::StatFields(count);
        for (int at = 0; at + 4 <= static_cast<int>(m.stats.size()); at += 4) {
            ImGui::TableNextRow();
            ImGui::PushID(at);
            uint32_t u;
            std::memcpy(&u, m.stats.data() + at, 4);
            ImGui::TableNextColumn();
            ImGui::Text("+0x%03X", at);
            ImGui::TableNextColumn();
            ImGui::Text("%u", u);
            ImGui::TableNextColumn();
            InputFloatField("##f", m.stats, at, -1, !g.unsafe);
            ImGui::TableNextColumn();
            for (int i = 0; i < count; ++i) if (f[i].offset == at) ImGui::TextUnformatted(f[i].name);
            if (at == mp::kExpTotal) ImGui::TextUnformatted("Experience (total)");
            if (at == mp::kExpSpent) ImGui::TextUnformatted("Experience (spent)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void EquipmentPanel() {
    const mp::Member& m = g.ch.members[0];
    ImGui::SeparatorText("Equipment");
    static const char* const kLists[4] = {"Weapons", "Belt", "Armour (worn)", "Spells"};
    for (int l = 0; l < 4; ++l) {
        if (!ImGui::TreeNodeEx(kLists[l], ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", kLists[l], m.lists[l].size())) continue;
        for (uint32_t id : m.lists[l]) ImGui::BulletText("%s", IdName(id).c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Backpack", ImGuiTreeNodeFlags_DefaultOpen, "Backpack (%zu)", g.ch.backpack.size())) {
        for (uint32_t id : g.ch.backpack) ImGui::BulletText("%s", IdName(id).c_str());
        ImGui::TreePop();
    }
}

// A database row picker (searchable), for an item's row or material.
bool RowCombo(const char* id, const char* sheet, uint16_t& value) {
    const auto* rows = Sheet(sheet);
    const std::string current = Row(sheet, value);
    ImGui::SetNextItemWidth(-1);
    if (!rows || !ImGui::BeginCombo(id, current.c_str(), ImGuiComboFlags_HeightLarge)) return false;
    static char filter[64] = "";
    if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::InputTextWithHint("##filter", "search", filter, sizeof filter);
    bool changed = false;
    std::string f = filter;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    for (int i = 0; i < static_cast<int>(rows->size()); ++i) {
        std::string n = (*rows)[i];
        std::string low = n;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!f.empty() && low.find(f) == std::string::npos) continue;
        if (ImGui::Selectable((std::to_string(i) + "  " + n).c_str(), i == value)) { value = static_cast<uint16_t>(i); changed = true; }
    }
    ImGui::EndCombo();
    return changed;
}

void ItemsPanel() {
    ImGui::SeparatorText("Items");
    ImGui::TextDisabled(g.unsafe ? "Remove, duplicate (into the backpack) or change items. A changed item keeps its old durability,\n"
                                   "energy and protection until the game updates them."
                                 : "Durability and energy can be changed. Tick \"Allow unsafe edits\" to remove, duplicate or change items.");
    uint32_t removeId = 0, dupId = 0;
    if (ImGui::BeginTable("items", g.unsafe ? 8 : 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Item");
        if (g.unsafe) ImGui::TableSetupColumn("Material");
        ImGui::TableSetupColumn("Durability", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Energy", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Energy max", ImGuiTableColumnFlags_WidthFixed, 70);
        if (g.unsafe) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();
        for (auto& list : g.ch.lists)
            for (mp::Object& o : list) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(o.id));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(mp::KindName(o.kind));
                ImGui::TableNextColumn();
                const char* sheet = KindSheet(o.kind);
                if (g.unsafe && sheet && Sheet(sheet)) {
                    uint16_t b = o.b;
                    if (RowCombo("##row", sheet, b)) SetRow(o, o.a, b);
                } else {
                    ImGui::TextUnformatted(IdName(o.id).c_str());
                }
                ImGui::SetItemTooltip("Object %u, database row %u, material %u", o.id, o.b, o.a);
                if (g.unsafe) {
                    ImGui::TableNextColumn();
                    if (HasMaterial(o.kind) && Sheet("Materials")) {
                        uint16_t a = o.a;
                        if (RowCombo("##mat", "Materials", a)) SetRow(o, a, o.b);
                    }
                }
                if (o.kind == 0x3004 || o.kind == 0x3005) { // block: id, a, b, then durability, its max, energy, its max
                    for (int c = 0; c < 4; ++c) {
                        ImGui::TableNextColumn();
                        ImGui::PushID(c);
                        InputFloatField("##v", o.detail, 8 + 4 * c, -1);
                        ImGui::PopID();
                    }
                } else {
                    for (int c = 0; c < 4; ++c) ImGui::TableNextColumn();
                }
                if (g.unsafe) {
                    ImGui::TableNextColumn();
                    if (o.kind != 0x3002) {
                        if (ImGui::SmallButton("Copy")) dupId = o.id;
                        ImGui::SetItemTooltip("A copy into the backpack (with its spell)");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("Remove")) removeId = o.id;
                    ImGui::SetItemTooltip("Removes it from the character (and from where it is worn)");
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (removeId) RemoveObject(removeId);
    // A new item: the block of an item of the same kind is the template (the game updates its values).
    if (g.unsafe) {
        static int kindIndex = 0;
        static uint16_t row = 0, material = 0;
        static const uint32_t kKinds[] = {0x3004, 0x3005, 0x3006, 0x3007};
        static const char* const kNames[] = {"Weapon", "Armour", "Quick item", "Loot item"};
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##newkind", &kindIndex, kNames, 4);
        const uint32_t kind = kKinds[kindIndex];
        const mp::Object* tmpl = nullptr;
        for (const mp::Object& o : g.ch.lists[0]) if (o.kind == kind) { tmpl = &o; break; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (Sheet(KindSheet(kind))) RowCombo("##newrow", KindSheet(kind), row);
        if (HasMaterial(kind) && Sheet("Materials")) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            RowCombo("##newmat", "Materials", material);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!tmpl || !Sheet(KindSheet(kind)));
        if (ImGui::Button("Add to the backpack") && tmpl) {
            mp::Object o = *tmpl;
            o.id = NewId();
            SetDetailId(o);
            SetRow(o, HasMaterial(kind) ? material : o.a, row);
            if (o.detail.size() >= 44 && (kind == 0x3004 || kind == 0x3005)) { const uint32_t none = 0xFFFFFFFFu; std::memcpy(o.detail.data() + 40, &none, 4); }
            g.ch.lists[0].push_back(o);
            g.ch.backpack.push_back(o.id);
        }
        ImGui::EndDisabled();
        if (!tmpl) ImGui::SetItemTooltip("The character has no %s yet: one is needed as a template (its file layout)", kNames[kindIndex]);
        else ImGui::SetItemTooltip("A copy of the character's %s with this item and material; durability, energy and protection\n"
                                   "stay the template's until the game updates them. No spell attached.", ObjectName(*tmpl).c_str());
    }
    if (dupId) DuplicateObject(dupId);
}

void VarsPanel() {
    ImGui::SeparatorText("Quest variables");
    ImGui::TextDisabled("The quests' progress (q.<quest>...: 1 running, 2 done) and other script variables.%s",
                        g.unsafe ? "" : "\nRead-only: tick \"Allow unsafe edits\" to change them.");
    int remove = -1;
    for (size_t i = 0; i < g.ch.vars.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        InputString("##n", g.ch.vars[i].name, 260, !g.unsafe);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        PushLocked(!g.unsafe);
        ImGui::InputFloat("##v", &g.ch.vars[i].value, 0, 0, "%g", g.unsafe ? 0 : ImGuiInputTextFlags_ReadOnly);
        PopLocked(!g.unsafe);
        if (g.unsafe) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (remove >= 0) g.ch.vars.erase(g.ch.vars.begin() + remove);
    if (g.unsafe && ImGui::SmallButton("Add a variable")) g.ch.vars.push_back({"q.", 1.0f});
    for (mp::QuestVar& v : g.ch.vars) if (v.name.empty()) v.name = "?"; // an empty name would end the list in the file
}

void ChecksPanel(const std::vector<Finding>& found) {
    int errors = 0, warnings = 0;
    for (const Finding& f : found) (f.level == 'E' ? errors : warnings)++;
    const std::string head = "Checks: " + std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s)###checks";
    if (errors) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
    const bool open = ImGui::CollapsingHeader(head.c_str(), errors ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (errors) ImGui::PopStyleColor();
    if (!open) return;
    if (found.empty()) ImGui::TextDisabled("No problem found.");
    for (const Finding& f : found)
        ImGui::TextColored(f.level == 'E' ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s %s", f.level == 'E' ? "ERROR" : "WARN ", f.text.c_str());
}

} // namespace

void OpenFolder(Library& lib, const std::string& folder) {
    std::snprintf(g.folder, sizeof g.folder, "%s", folder.c_str());
    g.folderInit = true;
    lib.mpFolder = folder;
    Rescan();
    g.selected = -1;
    if (!g.files.empty() && g.files[0].error.empty()) Open(0);
}

void DrawTab(Library& lib) {
    if (!g.folderInit) {
        g.folderInit = true;
        std::snprintf(g.folder, sizeof g.folder, "%s", lib.mpFolder.c_str());
        Rescan();
    }
    LoadDatabase(lib);
    ImGui::SetNextItemWidth(420);
    if (ImGui::InputText("Characters folder", g.folder, sizeof g.folder, ImGuiInputTextFlags_EnterReturnsTrue)) {
        lib.mpFolder = g.folder; lib.SaveConfig(); Rescan(); g.selected = -1;
    }
    ImGui::SetItemTooltip("The game's (or a mod's) mp folder, e.g. Universal-Mod/mp");
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("Folder...") && ui::PickFolder(picked)) {
        std::snprintf(g.folder, sizeof g.folder, "%s", picked.c_str());
        lib.mpFolder = picked; lib.SaveConfig(); Rescan(); g.selected = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) { const int s = g.selected; Rescan(); g.selected = s < static_cast<int>(g.files.size()) ? s : -1; }
    ImGui::SameLine();
    ImGui::Checkbox("Allow unsafe edits", &g.unsafe);
    ImGui::SetItemTooltip("Unlocks the edits that can break a character: unit, prototype, figure, zone, experience spent, attributes, abilities, quest variables, items and raw values.");
    if (g.sheets.empty()) ImGui::TextDisabled("No database set in Settings: items show as row numbers and are not checked.");

    ImGui::BeginChild("list", ImVec2(260, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (g.files.empty()) ImGui::TextDisabled(g.folder[0] ? "No .mp files here." : "Choose the mp folder.");
    for (int i = 0; i < static_cast<int>(g.files.size()); ++i) {
        const Entry& e = g.files[i];
        const std::string label = e.file + "  " + (e.error.empty() ? e.name : "(" + e.error + ")");
        if (ImGui::Selectable(label.c_str(), g.selected == i)) {
            if (Dirty()) g.message = "Save or revert the open character first.";
            else Open(i);
        }
        if (e.error.empty()) ImGui::SetItemTooltip("%s\nzone %s, experience %.0f", e.name.c_str(), e.zone.c_str(), e.exp);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("edit", ImVec2(0, 0));
    if (g.selected < 0 || g.ch.members.empty()) {
        ImGui::TextDisabled("Pick a character on the left.");
        if (!g.message.empty()) ImGui::TextWrapped("%s", g.message.c_str());
        ImGui::EndChild();
        return;
    }
    const std::vector<Finding> found = Check();
    const bool dirty = Dirty();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Save")) SaveCurrent();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) Open(g.selected);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", g.files[g.selected].file.c_str(), dirty ? " (unsaved)" : "");
    if (!g.message.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(g.message.c_str()); }
    ImGui::TextDisabled("Close the game first, or it may overwrite the file with its own copy.");
    ChecksPanel(found);
    if (ImGui::BeginTable("cols", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableNextColumn();
        CharacterPanel();
        VarsPanel();
        ImGui::TableNextColumn();
        EquipmentPanel();
        ItemsPanel();
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace mpedit
