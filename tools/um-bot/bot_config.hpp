// um-bot's settings: how the bot builds and plays, and where it connects. Saved as key=value lines in
// um-bot.cfg next to the program.
#pragma once

#include <cstdio>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bot {

enum class Role { Melee, Ranged, Mage, Hybrid };
enum class Priority { HealPlayer, FocusPlayerTarget, GuardPlayer };
enum class Engagement { Passive, Defensive, Aggressive };
enum class Pace { MatchPlayer, AlwaysRun, AlwaysWalk };

struct Config {
    // Connection
    std::string host = "127.0.0.1";
    int port = 8888;
    std::string name = "Companion";
    std::string character;       // the bot's character: a .mp file of the game's (or a mod's) mp folder
    // Build
    Role role = Role::Ranged;
    int tankiness = 30;          // 0 all damage .. 100 all defence (where skill points and gear go)
    // Behaviour
    Priority priority = Priority::HealPlayer;
    int healPlayerBelow = 50;    // % of the player's health
    int healSelfBelow = 35;      // % of its own health
    int manaReserve = 20;        // % of mana kept for heals
    bool usePotions = true;
    Engagement engagement = Engagement::Defensive;
    // Movement
    float followDistance = 4.0f; // world units behind the player
    float leashDistance = 25.0f; // past this, drop the fight and come back
    Pace pace = Pace::MatchPlayer; // MatchPlayer: run, walk, sneak and crawl like the player

    std::string path;            // the file it was read from / is written to
};

// The folder of the running program (where um-bot.cfg lives).
inline std::string ProgramDir() {
    char buf[4096] = {};
#ifdef _WIN32
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf - 1);
    std::string p(buf, n);
    const size_t slash = p.find_last_of("\\/");
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p(buf, n > 0 ? static_cast<size_t>(n) : 0);
    const size_t slash = p.find_last_of('/');
#endif
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

inline void Load(Config& c) {
    if (c.path.empty()) c.path = ProgramDir() + "/um-bot.cfg";
    std::ifstream in(c.path);
    std::string line;
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const int i = std::atoi(v.c_str());
        if (k == "HOST") c.host = v;
        else if (k == "PORT") c.port = clampi(i, 1, 65535);
        else if (k == "NAME") c.name = v;
        else if (k == "CHARACTER") c.character = v;
        else if (k == "ROLE") c.role = static_cast<Role>(clampi(i, 0, 3));
        else if (k == "TANKINESS") c.tankiness = clampi(i, 0, 100);
        else if (k == "PRIORITY") c.priority = static_cast<Priority>(clampi(i, 0, 2));
        else if (k == "HEAL_PLAYER_BELOW") c.healPlayerBelow = clampi(i, 0, 100);
        else if (k == "HEAL_SELF_BELOW") c.healSelfBelow = clampi(i, 0, 100);
        else if (k == "MANA_RESERVE") c.manaReserve = clampi(i, 0, 100);
        else if (k == "USE_POTIONS") c.usePotions = i != 0;
        else if (k == "ENGAGEMENT") c.engagement = static_cast<Engagement>(clampi(i, 0, 2));
        else if (k == "FOLLOW_DISTANCE") c.followDistance = static_cast<float>(std::atof(v.c_str()));
        else if (k == "LEASH_DISTANCE") c.leashDistance = static_cast<float>(std::atof(v.c_str()));
        else if (k == "PACE") c.pace = static_cast<Pace>(clampi(i, 0, 2));
    }
}

inline bool Save(const Config& c) {
    std::ofstream o(c.path, std::ios::trunc);
    if (!o) return false;
    o << "# um-bot settings\n"
      << "HOST=" << c.host << "\nPORT=" << c.port << "\nNAME=" << c.name << "\nCHARACTER=" << c.character << "\n"
      << "ROLE=" << static_cast<int>(c.role) << "\nTANKINESS=" << c.tankiness << "\n"
      << "PRIORITY=" << static_cast<int>(c.priority) << "\nHEAL_PLAYER_BELOW=" << c.healPlayerBelow
      << "\nHEAL_SELF_BELOW=" << c.healSelfBelow << "\nMANA_RESERVE=" << c.manaReserve << "\nUSE_POTIONS=" << (c.usePotions ? 1 : 0) << "\n"
      << "ENGAGEMENT=" << static_cast<int>(c.engagement) << "\n"
      << "FOLLOW_DISTANCE=" << c.followDistance << "\nLEASH_DISTANCE=" << c.leashDistance << "\nPACE=" << static_cast<int>(c.pace) << "\n";
    return static_cast<bool>(o);
}

} // namespace bot
