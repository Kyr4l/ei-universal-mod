// um-bot: a companion player for Evil Islands multiplayer. It is meant to join a game like a normal
// player (its own network client, no game copy) and play alongside a human: follow, heal, fight their
// targets, by the settings of this window.
//
// Skeleton: the settings and the window. The network client (the game's UDP protocol, port 8888) is not
// written yet: Connect only says so. See README.md for the plan.
#include "net.hpp" // first: winsock2.h before windows.h

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <deque>
#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include "bot_config.hpp"
#include "i18n.hpp"           // um-multitool's translations (lang/ru.txt)
#include "icon_data.hpp"       // the window icon (the Sacred flower)
#include "mp_file.hpp"         // um-multitool's .mp reader (../um-multitool)
#include "viewer/item_db.hpp"  // item names (the database beside the character's mp folder)
#include "viewer/ui_common.hpp" // the file dialog

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <fstream>
#include <iterator>

static const char* const kVersion = "0.1.1";


struct App {
    bot::Config cfg;
    bool dirty = false;               // settings changed since the last save
    std::deque<std::string> log;      // newest last
    net::Client client;
    size_t clientLogShown = 0;
    mp::Character character;          // the loaded .mp
    std::string characterError;
    std::string characterFor;         // the path it was loaded from
    bool nameDirty = false;           // the name was changed, not saved yet
    items::Database db;               // for the equipment's names (<mp folder>/../res/databaselmp.res)
    std::string dbFor;                // the character it was looked for
    bool dbLoaded = false;
    void Log(const std::string& s) {
        log.push_back(s);
        while (log.size() > 200) log.pop_front();
    }
};

// A combo over an enum's labels; true when changed.
template <typename E, size_t N>
static bool EnumCombo(const char* label, E& value, const char* const (&names)[N]) {
    int i = static_cast<int>(value);
    if (!ImGui::Combo(label, &i, names, static_cast<int>(N))) return false;
    value = static_cast<E>(i);
    return true;
}

// The character .mp's decompressed content: what the bot uploads when entering a quest.
static std::vector<uint8_t> MpRaw(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {}), raw;
    std::string err;
    if (!in || !mp::Unpack(file, raw, err)) raw.clear();
    return raw;
}

static void Tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static void ConnectionPanel(App& app) {
    { // the language (um-multitool's table: lang/ru.txt)
        i18n::Lang cur = i18n::Current();
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("Language", i18n::NativeName(cur))) {
            for (i18n::Lang l : {i18n::Lang::English, i18n::Lang::Russian})
                if (ImGui::Selectable(i18n::NativeName(l), l == cur)) { i18n::Set(l); app.cfg.language = i18n::Code(l); app.dirty = true; }
            ImGui::EndCombo();
        }
    }
    bot::Config& c = app.cfg;
    const bool idle = app.client.state != net::Client::State::AskingInfo && app.client.state != net::Client::State::LoggingIn &&
                      app.client.state != net::Client::State::Accepted;
    ImGui::BeginDisabled(!idle);
    char host[256];
    std::snprintf(host, sizeof host, "%s", c.host.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("Server", host, sizeof host)) { c.host = host; app.dirty = true; }
    Tip("The address of the game hosting the session (the player's computer)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("Port", &c.port, 0)) { c.port = c.port < 1 ? 1 : c.port > 65535 ? 65535 : c.port; app.dirty = true; }
    Tip("The game's UDP port (8888 by default)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool active = app.client.state == net::Client::State::AskingInfo || app.client.state == net::Client::State::LoggingIn ||
                        app.client.state == net::Client::State::Accepted;
    if (!active) {
        ImGui::BeginDisabled(app.characterFor.empty() || !app.characterError.empty());
        if (ImGui::Button("Connect")) {
            if (!app.character.members.empty()) {
                const mp::Member& m = app.character.members[0];
                app.client.SetCharacter(m.strings[0], m.strings[4], m.u0, MpRaw(app.characterFor));
            }
            app.client.Connect(c.host, c.port);
        }
        ImGui::EndDisabled();
        Tip("Joins the game: asks the server for its info, then logs in (needs a character)");
    } else if (ImGui::Button("Disconnect")) {
        app.client.Disconnect();
    }
    ImGui::SameLine();
    using S = net::Client::State;
    const S st = app.client.state;
    ImGui::TextUnformatted(st == S::Accepted ? "Logged in (the game's messages after the login are not written yet)"
                           : st == S::AskingInfo || st == S::LoggingIn ? "Connecting..."
                           : st == S::Rejected || st == S::Failed ? app.client.error.c_str() : "Not connected");
    if (st == S::Accepted || st == S::LoggingIn)
        ImGui::TextDisabled("Server: %s (key %08X), client id %u", app.client.info.host.c_str(), app.client.info.key, app.client.clientId);

    // The bot's character
    char path[1024];
    std::snprintf(path, sizeof path, "%s", c.character.c_str());
    ImGui::SetNextItemWidth(-160);
    if (ImGui::InputTextWithHint("##character", "the bot's character: a .mp file", path, sizeof path, ImGuiInputTextFlags_EnterReturnsTrue)) {
        c.character = path;
        app.dirty = true;
    }
    Tip("A multiplayer character of the game (its mp folder, e.g. Universal-Mod/mp/8.mp). A fresh character is best:\n"
        "the bot plays and saves it like a player would.");
    ImGui::SameLine();
    std::string picked;
    if (ImGui::Button("Character...") && ui::PickFile(picked)) { c.character = picked; app.dirty = true; }
    if (app.characterFor != c.character) { // (re)load it
        app.characterFor = c.character;
        app.nameDirty = false;
        app.characterError.clear();
        if (!c.character.empty() && !mp::Load(c.character, app.character, app.characterError)) app.Log("Character not read: " + app.characterError);
    }
    if (c.character.empty()) ImGui::TextDisabled("Choose the bot's character (.mp). A fresh one is recommended.");
    else if (!app.characterError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", app.characterError.c_str());
    else if (!app.character.members.empty()) {
        mp::Member& m = app.character.members[0];
        // The bot's name is its character's: "<name> | <clan tag>"; both can be changed here.
        const size_t bar = m.strings[0].find('|');
        auto trim = [](std::string v) {
            while (!v.empty() && v.back() == ' ') v.pop_back();
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            return v;
        };
        std::string name = trim(bar == std::string::npos ? m.strings[0] : m.strings[0].substr(0, bar));
        const std::string tag = bar == std::string::npos ? "" : trim(m.strings[0].substr(bar + 1));
        char nb[64];
        std::snprintf(nb, sizeof nb, "%s", name.c_str());
        ImGui::BeginDisabled(!idle);
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("Name", nb, sizeof nb) && nb[0]) { m.strings[0] = tag.empty() ? std::string(nb) : std::string(nb) + " | " + tag; app.nameDirty = true; }
        ImGui::EndDisabled();
        Tip("The bot's name in the game: its character's (saved into the .mp file)");
        ImGui::SameLine();
        char tb[32];
        std::snprintf(tb, sizeof tb, "%s", tag.c_str());
        ImGui::BeginDisabled(!idle);
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputText("Clan tag", tb, sizeof tb)) {
            std::string t = trim(tb);
            m.strings[0] = t.empty() ? name : name + " | " + t;
            app.nameDirty = true;
        }
        ImGui::EndDisabled();
        Tip("Shown after the name in the game (\"Kevina | BOT\"); empty: no tag");
        if (app.nameDirty) {
            ImGui::SameLine();
            if (ImGui::Button("Save name and tag")) {
                std::string err;
                const std::string bak = c.character + ".bak";
                if (FILE* f = std::fopen(bak.c_str(), "rb")) std::fclose(f);
                else if (FILE* in = std::fopen(c.character.c_str(), "rb")) { // the first save keeps the original
                    std::vector<char> bytes;
                    char buf[4096];
                    size_t n;
                    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) bytes.insert(bytes.end(), buf, buf + n);
                    std::fclose(in);
                    if (FILE* out = std::fopen(bak.c_str(), "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), out); std::fclose(out); }
                }
                if (mp::Save(c.character, app.character, err)) { app.nameDirty = false; app.Log("Saved the name and clan tag into " + c.character); }
                else app.Log("Name and tag not saved: " + err);
            }
        }
        ImGui::TextDisabled("%s, experience %.0f, money %u, %zu items", m.strings[4].c_str(), mp::GetF(m.stats, mp::kExpTotal),
                            mp::Money(app.character), app.character.lists[0].size());
    }
}

static void BuildPanel(App& app) {
    bot::Config& c = app.cfg;
    static const char* const kRoles[] = {"Melee", "Ranged", "Mage", "Hybrid"};
    ImGui::SetNextItemWidth(200);
    app.dirty |= EnumCombo("Role", c.role, kRoles);
    Tip("What the bot builds and plays:\n"
        "Melee: weapons and armour, in front of the player\n"
        "Ranged: bows and crossbows, behind the player\n"
        "Mage: spells, heals and buffs first\n"
        "Hybrid: a weapon plus healing spells");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Damage <-> Defence", &c.tankiness, 0, 100, i18n::Tr("%d%% defence"));
    Tip("Where its skill points and gear go: 0 all damage, 100 all defence (health, armour)");
}

static void BehaviourPanel(App& app) {
    bot::Config& c = app.cfg;
    static const char* const kPriorities[] = {"Keep the player alive", "Focus the player's target", "Guard the player"};
    ImGui::SetNextItemWidth(220);
    app.dirty |= EnumCombo("Priority", c.priority, kPriorities);
    Tip("Keep the player alive: heals and buffs come first, it fights when the player is safe\n"
        "Focus the player's target: attacks what the player attacks, heals below the threshold\n"
        "Guard the player: attacks whatever attacks the player");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Heal the player below", &c.healPlayerBelow, 0, 100, i18n::Tr("%d%% health"));
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Heal itself below", &c.healSelfBelow, 0, 100, i18n::Tr("%d%% health"));
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Mana kept for heals", &c.manaReserve, 0, 100, "%d%%");
    Tip("Mana it does not spend on attacks");
    app.dirty |= ImGui::Checkbox("Use potions", &c.usePotions);
    static const char* const kEngagement[] = {"Passive", "Defensive", "Aggressive"};
    ImGui::SetNextItemWidth(200);
    app.dirty |= EnumCombo("Engagement", c.engagement, kEngagement);
    Tip("Passive: never starts a fight, only heals and follows\n"
        "Defensive: fights what attacks the player or itself, and the player's target\n"
        "Aggressive: attacks the enemies it sees");
}

static void MovementPanel(App& app) {
    bot::Config& c = app.cfg;
    static const char* const kPace[] = {"Like the player", "Always run", "Always walk"};
    ImGui::SetNextItemWidth(200);
    app.dirty |= EnumCombo("Pace", c.pace, kPace);
    Tip("Like the player: runs, walks, sneaks and crawls when the player does (it does not spoil a sneak)");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderFloat("Follow distance", &c.followDistance, 1.0f, 15.0f, i18n::Tr("%.1f units"));
    Tip("How far behind the player it stays");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderFloat("Leash", &c.leashDistance, 10.0f, 60.0f, i18n::Tr("%.0f units"));
    Tip("Farther than this from the player, it drops what it does and comes back");
}

// The sonar: what is around the bot, bare (the players' heroes it knows the position of), a sweeping line for style.
static void Sonar(const net::Client& c, float size, float range) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = size * 0.5f - 2;
    const ImVec2 o(p.x + size * 0.5f, p.y + size * 0.5f);
    const ImU32 green = IM_COL32(60, 220, 90, 255), dim = IM_COL32(60, 220, 90, 70);
    dl->AddCircleFilled(o, r, IM_COL32(5, 25, 10, 255), 64);
    for (int k = 1; k <= 3; ++k) dl->AddCircle(o, r * k / 3, dim, 64);
    dl->AddLine(ImVec2(o.x - r, o.y), ImVec2(o.x + r, o.y), dim);
    dl->AddLine(ImVec2(o.x, o.y - r), ImVec2(o.x, o.y + r), dim);
    const float a = static_cast<float>(ImGui::GetTime() * 1.5);
    for (int k = 0; k < 24; ++k) { // the sweep and its fading trail
        const float b = a - k * 0.03f;
        dl->AddLine(o, ImVec2(o.x + std::cos(b) * r, o.y + std::sin(b) * r), IM_COL32(60, 220, 90, 200 - k * 8), 2);
    }
    dl->AddCircle(o, r, green, 64, 2);
    float mx, my;
    if (c.Where(c.worldUnit, mx, my)) {
        std::set<uint32_t> heroes;
        for (const auto& kv : c.players) heroes.insert(kv.second.unitId);
        for (const auto& kv : c.unitPos) { // the other units in range: small hollow dots
            float x, y;
            if (heroes.count(kv.first) || !c.Where(kv.first, x, y)) continue;
            const float dx = (x - mx) / range, dy = (y - my) / range;
            if (dx * dx + dy * dy <= 1) dl->AddCircle(ImVec2(o.x + dx * r, o.y - dy * r), 3, green, 12, 1.5f);
        }
        for (const auto& kv : c.players) {
            float x, y;
            if (kv.second.unitId == c.worldUnit || !c.Where(kv.second.unitId, x, y)) continue;
            float dx = (x - mx) / range, dy = (y - my) / range;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 1) { dx /= d; dy /= d; } // beyond the range: on the edge
            const ImVec2 q(o.x + dx * r, o.y - dy * r); // north up
            dl->AddCircleFilled(q, 4, green);
            dl->AddText(ImVec2(q.x + 6, q.y - 7), green, kv.second.name.c_str());
        }
    } else {
        const char* t = i18n::Tr(c.worldUnit ? "position unknown" : "not in the world");
        dl->AddText(ImVec2(o.x - ImGui::CalcTextSize(t).x * 0.5f, o.y + r * 0.4f), green, t);
    }
    dl->AddCircleFilled(o, 3, IM_COL32(230, 255, 230, 255));
    ImGui::Dummy(ImVec2(size, size));
}

static void StatusPanel(App& app) {
    const bool on = app.client.state == net::Client::State::Accepted;
    app.client.followDistance = app.cfg.followDistance;
    app.client.pace = app.cfg.pace == bot::Pace::AlwaysWalk ? 2 : 3;
    app.client.engagement = static_cast<int>(app.cfg.engagement); // Passive / Defensive / Aggressive
    app.client.fightInSight = app.client.engagement > 0;
    if (!app.character.members.empty() && mp::GetF(app.character.members[0].stats, 0xB4) > 1) app.client.sight = mp::GetF(app.character.members[0].stats, 0xB4);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float size = std::clamp(std::min(avail.x * 0.45f, avail.y - 4), 80.0f, 240.0f);
    float sight = 0; // the edge of the sonar: the character's sight
    if (!app.character.members.empty()) sight = mp::GetF(app.character.members[0].stats, 0xB4);
    Sonar(app.client, size, sight > 1 ? sight : 12.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.0f);
    if (on && app.client.worldUnit && app.client.Health(app.client.worldUnit) > 0)
        ImGui::Text("Health: %d   Mana: %d", app.client.Health(app.client.worldUnit), app.client.Mana(app.client.worldUnit));
    else ImGui::Text("Health: -   Mana: -");
    ImGui::Text("Doing: %s", on ? app.client.Doing().c_str() : "nothing (not connected)");
    if (on) { // the session, from the server's messages
        const net::Client& c = app.client;
        ImGui::Text("Quest: %s%s", c.chosenQuest.empty() ? i18n::Tr("none chosen") : c.chosenQuest.c_str(),
                    c.chosenQuest.empty() ? "" : c.ZoneOpen() ? i18n::Tr("  (its zone is open)") : i18n::Tr("  (its zone is not open yet)"));
        static const char* const kStates[] = {"in the quest", "in the lobby", "?", "loading", "connecting"};
        std::string ps;
        for (const auto& kv : c.players)
            ps += (ps.empty() ? "" : ", ") + kv.second.name + " (" + i18n::Tr(kv.second.state <= 4 ? kStates[kv.second.state] : "?") + ")";
        ImGui::TextWrapped("Players: %s", ps.empty() ? "-" : ps.c_str()); // names as the server has them
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

static void LogPanel(App& app) {
    ImGui::BeginChild("log", ImVec2(0, 0), false);
    for (const std::string& s : app.log) ImGui::TextWrapped("%s", s.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// The bot's character, read only: what its .mp holds (health and mana are computed by the game, not stored).
static void CharacterPanel(App& app) {
    if (app.character.members.empty()) { ImGui::TextDisabled("No character loaded."); return; }
    const mp::Member& m = app.character.members[0];
    if (app.dbFor != app.cfg.character) { // the mod's database: <mp folder>/../res/databaselmp.res
        app.dbFor = app.cfg.character;
        app.dbLoaded = false;
        std::string dir = app.cfg.character;
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "." : dir.substr(0, slash);
        for (const char* rel : {"/../res/databaselmp.res", "/../res/database.res"}) {
            std::ifstream f(dir + rel, std::ios::binary);
            if (!f) continue;
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string err;
            if (items::LoadDatabaseRes(bytes, rel + 8, app.db, err)) { app.dbLoaded = true; break; }
        }
    }
    const float total = mp::GetF(m.stats, mp::kExpTotal), spent = mp::GetF(m.stats, mp::kExpSpent);
    ImGui::Text("%s", m.strings[0].c_str());
    ImGui::TextDisabled("%s", m.strings[4].c_str());
    ImGui::Text("Experience %.0f (free %.0f)   Money %u", total, total - spent, mp::Money(app.character));
    auto f = [&](int at) { return mp::GetF(m.stats, at); };
    ImGui::Text("Strength %.0f   Dexterity %.0f   Intelligence %.0f   Actions %.0f", f(0x10), f(0x18), f(0x20), f(0x30));
    Tip("As the game shows them (abilities included); health and mana are computed by the game");
    ImGui::Text("Encumbrance %.0f / %.0f   Sight %.1f", f(0x50), f(0x54), f(0xB4));
    // Skills, and the abilities it has
    std::string skills;
    int n = 0;
    const mp::SkillByte* sk = mp::Skills(n);
    for (int i = 0; i < n; ++i) {
        const int v = sk[i].offset < static_cast<int>(m.stats.size()) ? m.stats[sk[i].offset] : 0;
        if (v) skills += std::string(skills.empty() ? "" : ", ") + i18n::Tr(sk[i].name) + " " + std::to_string(v);
    }
    ImGui::TextWrapped("Skills: %s", skills.empty() ? "none" : skills.c_str());
    // The level names the game gives (its PERK texts): weapons Specialist / Expert / Master, magic schools
    // Apprentice / Expert / Master, the others Increased / X2 / X3.
    static const char* const kWeapon[] = {"", "Specialist", "Expert", "Master"};
    static const char* const kMagic[] = {"", "Apprentice", "Expert", "Master"};
    static const char* const kOther[] = {"", "Increased", "X2", "X3"};
    std::string perks;
    for (int g = 0; g < mp::kPerkGroups; ++g) {
        const int at = mp::kPerkBase + g, v = at < static_cast<int>(m.stats.size()) ? m.stats[at] : 0;
        const char* const* level = g <= 6 ? kWeapon : g <= 14 ? kMagic : kOther;
        if (v >= 1 && v <= 3) perks += std::string(perks.empty() ? "" : ", ") + i18n::Tr(mp::PerkGroupName(g)) + " (" + i18n::Tr(level[v]) + ")";
    }
    ImGui::TextWrapped("Abilities: %s", perks.empty() ? "none" : perks.c_str());
    // Equipment: the member's lists of object ids, named from the database when it was found
    auto name = [&](uint32_t id) -> std::string {
        for (const auto& list : app.character.lists)
            for (const mp::Object& o : list) {
                if (o.id != id) continue;
                if (!app.dbLoaded) return std::string(mp::KindName(o.kind)) + " " + std::to_string(o.b);
                auto row = [&](items::Category c) -> std::string {
                    const auto& v = app.db.List(c);
                    return o.b < v.size() ? v[o.b].name : "?";
                };
                auto mat = [&]() -> std::string { return o.a < app.db.materials.size() ? app.db.materials[o.a].name : "?"; };
                switch (o.kind) {
                case 0x3004: return row(items::Category::Weapons) + " (" + mat() + ")";
                case 0x3005: return row(items::Category::Armors) + " (" + mat() + ")";
                case 0x3006: case 0x3008: return row(items::Category::QuickItems);
                case 0x3007: { std::string r = row(items::Category::LootItems); return r == "material" ? r + "." + mat() : r; }
                case 0x3009: return row(items::Category::QuestItems);
                default: return std::string(mp::KindName(o.kind));
                }
            }
        return "?";
    };
    static const char* const kLists[] = {"Weapons", "Belt", "Armour", "Spells"};
    for (int l = 0; l < 4; ++l) {
        if (l == 3) { ImGui::Text("Spells: %zu", m.lists[3].size()); continue; } // spell names: SpellPrototypes, not read here
        std::string t;
        for (uint32_t id : m.lists[l]) t += std::string(t.empty() ? "" : ", ") + name(id);
        ImGui::TextWrapped("%s: %s", i18n::Tr(kLists[l]), t.empty() ? i18n::Tr("none") : t.c_str());
    }
    ImGui::TextDisabled("Backpack: %zu items%s", app.character.backpack.size(), app.dbLoaded ? "" : " (no database found beside the mp folder: no item names)");
}

// `um-bot --install-desktop [--remove]` (Linux): the menu entry (um-bot.desktop, with this binary's path) and the
// 256-pixel icon for the current user, so the bot shows in the application menu with its icon.
static int InstallDesktop(bool remove) {
#ifdef _WIN32
    (void)remove;
    std::fprintf(stderr, "--install-desktop is for Linux desktops; on Windows the .exe carries its icon.\n");
    return 1;
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if ((!dataHome || !*dataHome) && (!home || !*home)) { std::fprintf(stderr, "HOME is not set\n"); return 1; }
    const fs::path data = dataHome && *dataHome ? fs::path(dataHome) : fs::path(home) / ".local" / "share";
    const fs::path desktop = data / "applications" / "um-bot.desktop";
    const fs::path icon = data / "icons" / "hicolor" / "256x256" / "apps" / "um-bot.png";
    auto refresh = [&]() {
        const std::string cmd = "update-desktop-database -q \"" + desktop.parent_path().string() + "\" >/dev/null 2>&1;"
                                " gtk-update-icon-cache -q -t \"" + (data / "icons" / "hicolor").string() + "\" >/dev/null 2>&1;"
                                " (kbuildsycoca6 || kbuildsycoca5) >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {} // other desktops notice the files by themselves
    };
    if (remove) {
        bool any = false;
        for (const fs::path& p : {desktop, icon}) if (fs::remove(p, ec)) { std::printf("Removed %s\n", p.string().c_str()); any = true; }
        if (!any) std::printf("Nothing to remove.\n");
        else refresh();
        return 0;
    }
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) { std::fprintf(stderr, "cannot find this program's path\n"); return 1; }
    fs::create_directories(icon.parent_path(), ec);
    fs::create_directories(desktop.parent_path(), ec);
    const fs::path from = exe.parent_path() / "assets" / "logo-256.png";
    if (!fs::copy_file(from, icon, fs::copy_options::overwrite_existing, ec)) {
        std::fprintf(stderr, "cannot copy %s to %s: %s\n", from.string().c_str(), icon.string().c_str(), ec.message().c_str());
        return 1;
    }
    std::ofstream f(desktop, std::ios::trunc);
    if (!f) { std::fprintf(stderr, "cannot write %s\n", desktop.string().c_str()); return 1; }
    f << "[Desktop Entry]\nType=Application\nName=um-bot\nGenericName=Evil Islands companion player\n"
         "Comment=A bot that joins your Evil Islands multiplayer game and plays at your side\n"
         "Exec=\"" << exe.string() << "\"\nPath=" << exe.parent_path().string() << "\nIcon=um-bot\nTerminal=false\n"
         "Categories=Game;\nKeywords=Evil Islands;Cursed Lands;bot;multiplayer;\nStartupWMClass=um-bot\nStartupNotify=true\n";
    f.close();
    refresh();
    std::printf("Installed %s and %s\n", desktop.string().c_str(), icon.string().c_str());
    return 0;
#endif
}

static void Frame(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("um-bot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ConnectionPanel(app);
    ImGui::Separator();
    if (ImGui::BeginTable("cols", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Build");
        BuildPanel(app);
        ImGui::SeparatorText("Behaviour");
        BehaviourPanel(app);
        ImGui::SeparatorText("Movement");
        MovementPanel(app);
        ImGui::Spacing();
        ImGui::BeginDisabled(!app.dirty);
        if (ImGui::Button("Save settings")) {
            if (bot::Save(app.cfg)) { app.dirty = false; app.Log("Settings saved to " + app.cfg.path); }
            else app.Log("Cannot write " + app.cfg.path);
        }
        ImGui::EndDisabled();
        ImGui::SeparatorText("Log");
        LogPanel(app);
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Character");
        CharacterPanel(app);
        ImGui::SeparatorText("Status");
        StatusPanel(app);
        ImGui::EndTable();
    }
    ImGui::End();
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version") { std::printf("um-bot %s\n", kVersion); return 0; }
        if (a == "--connect-test") { // without the window: join the configured server, print what happens, leave
            bot::Config cfg;
            bot::Load(cfg);
            net::Client client;
            mp::Character ch;
            std::string err;
            if (!cfg.character.empty() && mp::Load(cfg.character, ch, err) && !ch.members.empty())
                client.SetCharacter(ch.members[0].strings[0], ch.members[0].strings[4], ch.members[0].u0, MpRaw(cfg.character));
            else
                std::printf("no character (%s): logs in without joining\n", err.empty() ? "CHARACTER not set" : err.c_str());
            client.followHost = std::getenv("UM_BOT_FOLLOW") != nullptr;
            client.followDistance = cfg.followDistance;
            client.pace = cfg.pace == bot::Pace::AlwaysWalk ? 2 : 3; // MatchPlayer: run until the host's pace is read
            client.fightInSight = std::getenv("UM_BOT_FIGHT") != nullptr;
            client.engagement = client.fightInSight ? (std::getenv("UM_BOT_FIGHT")[0] == '1' ? 2 : std::atoi(std::getenv("UM_BOT_FIGHT"))) : 0;
            if (!ch.members.empty() && mp::GetF(ch.members[0].stats, 0xB4) > 1) client.sight = mp::GetF(ch.members[0].stats, 0xB4);
            client.Connect(cfg.host, cfg.port);
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(std::getenv("UM_BOT_STAY") ? std::atoi(std::getenv("UM_BOT_STAY")) : client.ZoneOpen() && !client.worldUnit ? 30 : 8) && client.state != net::Client::State::Failed &&
                   client.state != net::Client::State::Rejected) {
                client.Update();
                static size_t printed = 0; // the log as it comes, with the time
                for (; printed < client.log.size(); ++printed)
                    std::printf("%6.1f %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), client.log[printed].c_str());
                std::fflush(stdout);
                static int attackStep = 0; // research: UM_BOT_ATTACK=1: 12 s in, walk near the nearest monster; 8 s later, attack it
                static uint32_t target = 0;
                if (std::getenv("UM_BOT_ATTACK") && client.worldUnit) {
                    const auto in = std::chrono::steady_clock::now() - start;
                    float mx = 0, my = 0, tx = 0, ty = 0, d = 0;
                    client.Where(client.worldUnit, mx, my);
                    if (attackStep == 0 && in > std::chrono::seconds(12)) {
                        target = client.NearestOther(d);
                        if (target && client.Where(target, tx, ty) && d > 7) client.MoveTo(tx + (mx - tx) * 6 / d, ty + (my - ty) * 6 / d);
                        std::printf("target %X at %.1f (me at %.1f %.1f): walking near\n", target, d, mx, my); std::fflush(stdout);
                        attackStep = 1;
                    } else if (attackStep == 1 && in > std::chrono::seconds(22)) {
                        if (target) client.Attack(target);
                        client.Where(target, tx, ty);
                        std::printf("attack %X (me at %.1f %.1f, it at %.1f %.1f)\n", target, mx, my, tx, ty); std::fflush(stdout);
                        attackStep = 2;
                    }
                }
                static int moves = 0; // research: UM_BOT_MOVE="x,y;x,y;..." once in the world, one every 8 s
                static auto movedAt = std::chrono::steady_clock::now();
                if (const char* mv = std::getenv("UM_BOT_MOVE"); mv && client.worldUnit &&
                    std::chrono::steady_clock::now() - movedAt > std::chrono::seconds(8)) {
                    const char* q = mv;
                    for (int k = 0; k < moves && q; ++k) { q = std::strchr(q, ';'); if (q) ++q; }
                    float x = 0, y = 0;
                    if (q && std::sscanf(q, "%f,%f", &x, &y) == 2) { client.MoveTo(x, y); std::printf("move to %.1f %.1f\n", x, y); std::fflush(stdout); }
                    ++moves; movedAt = std::chrono::steady_clock::now();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            client.Disconnect();
            for (const auto& pl : client.players) std::printf("player %s state %u unit %08X\n", pl.second.name.c_str(), pl.second.state, pl.second.unitId);
            for (const auto& q : client.questStates) std::printf("quest state %s\n", q.second.c_str());
            { float x = 0, y = 0; client.Where(client.worldUnit, x, y); std::printf("units known: %zu, me at %.1f %.1f\n", client.unitPos.size(), x, y); }
            std::printf("world unit: %u\n", client.worldUnit); return client.joined ? 0 : 1;
        }
        if (a == "--install-desktop") return InstallDesktop(i + 1 < argc && std::string(argv[i + 1]) == "--remove");
        if (a == "--help" || a == "-h") {
            std::printf("um-bot %s - an Evil Islands companion player\n"
                        "Usage: um-bot [--version | --connect-test | --install-desktop [--remove]]\n"
                        "  --connect-test       join the configured server with the configured character, then leave\n"
                        "  --install-desktop    add um-bot to the Linux application menu, with its icon (--remove: undo)\n"
                        "Settings: um-bot.cfg next to the program.\n", kVersion);
            return 0;
        }
    }
    if (!glfwInit()) { std::fprintf(stderr, "cannot start GLFW\n"); return 1; }
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "um-bot"); // the menu entry's (um-bot.desktop): its icon on Wayland
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "um-bot");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "um-bot");
    GLFWwindow* window = glfwCreateWindow(900, 560, (std::string("um-bot ") + kVersion).c_str(), nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "cannot open a window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) { // Wayland has no way to set one from the program
        GLFWimage icons[3] = {{64, 64, const_cast<unsigned char*>(logo::kIcon64)},
                              {48, 48, const_cast<unsigned char*>(logo::kIcon48)},
                              {32, 32, const_cast<unsigned char*>(logo::kIcon32)}};
        glfwSetWindowIcon(window, 3, icons);
    }
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    { // the built-in font has no Cyrillic: merge a system font (as um-multitool does)
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        for (const char* path : {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
                                 "/usr/share/fonts/truetype/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                 "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"}) {
            if (FILE* f = std::fopen(path, "rb")) {
                std::fclose(f);
                ImFontConfig config;
                config.MergeMode = true;
                io.Fonts->AddFontFromFileTTF(path, 0.0f, &config);
                break;
            }
        }
    }
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini: the layout is fixed
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    App app;
    bot::Load(app.cfg);
    { i18n::Lang l; if (i18n::FromCode(app.cfg.language, l)) i18n::Set(l); }
    app.Log("um-bot " + std::string(kVersion) + ": it joins the session, enters the chosen quest when the host is in it and follows the host. Fighting comes next.");

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.05);
        app.client.Update();
        for (; app.clientLogShown < app.client.log.size(); ++app.clientLogShown) app.Log(app.client.log[app.clientLogShown]);
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        Frame(app);
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    if (app.dirty) bot::Save(app.cfg); // keep what was set
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
