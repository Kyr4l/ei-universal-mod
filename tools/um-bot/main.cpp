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
#include "mp_file.hpp"         // um-multitool's .mp reader (../um-multitool)
#include "viewer/ui_common.hpp" // the file dialog

static const char* const kVersion = "0.2";

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

static void Tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static void ConnectionPanel(App& app) {
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
        if (ImGui::Button("Connect")) app.client.Connect(c.host, c.port);
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
        // The bot's name is its character's: "<name> | <clan tag>"; only the name can be changed here.
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
        ImGui::TextDisabled("clan tag: %s", tag.empty() ? "(none)" : tag.c_str());
        if (app.nameDirty) {
            ImGui::SameLine();
            if (ImGui::Button("Save name")) {
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
                if (mp::Save(c.character, app.character, err)) { app.nameDirty = false; app.Log("Saved the name into " + c.character); }
                else app.Log("Name not saved: " + err);
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
    app.dirty |= ImGui::SliderInt("Damage <-> Defence", &c.tankiness, 0, 100, "%d%% defence");
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
    app.dirty |= ImGui::SliderInt("Heal the player below", &c.healPlayerBelow, 0, 100, "%d%% health");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Heal itself below", &c.healSelfBelow, 0, 100, "%d%% health");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderInt("Mana kept for heals", &c.manaReserve, 0, 100, "%d%%");
    Tip("Mana it does not spend on attacks");
    app.dirty |= ImGui::Checkbox("Use potions", &c.usePotions);
    static const char* const kEngagement[] = {"Passive", "Defensive", "Aggressive"};
    ImGui::SetNextItemWidth(200);
    app.dirty |= EnumCombo("Engagement", c.engagement, kEngagement);
    Tip("Passive: never starts a fight, only heals and follows\n"
        "Defensive: fights what attacks the player or itself\n"
        "Aggressive: attacks the enemies it sees");
}

static void MovementPanel(App& app) {
    bot::Config& c = app.cfg;
    static const char* const kPace[] = {"Like the player", "Always run", "Always walk"};
    ImGui::SetNextItemWidth(200);
    app.dirty |= EnumCombo("Pace", c.pace, kPace);
    Tip("Like the player: runs, walks, sneaks and crawls when the player does (it does not spoil a sneak)");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderFloat("Follow distance", &c.followDistance, 1.0f, 15.0f, "%.1f units");
    Tip("How far behind the player it stays");
    ImGui::SetNextItemWidth(200);
    app.dirty |= ImGui::SliderFloat("Leash", &c.leashDistance, 10.0f, 60.0f, "%.0f units");
    Tip("Farther than this from the player, it drops what it does and comes back");
}

static void StatusPanel(App& app) {
    const bool on = app.client.state == net::Client::State::Accepted;
    ImGui::Text("Health: %s   Mana: %s", on ? "?" : "-", on ? "?" : "-");
    ImGui::Text("Doing: %s", on ? "?" : "nothing (not connected)");
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), false);
    for (const std::string& s : app.log) ImGui::TextWrapped("%s", s.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
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
        ImGui::TableNextColumn();
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
            client.Connect(cfg.host, cfg.port);
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && client.state != net::Client::State::Failed &&
                   client.state != net::Client::State::Rejected) {
                client.Update();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            client.Disconnect();
            for (const std::string& l : client.log) std::printf("%s\n", l.c_str());
            return client.clientId ? 0 : 1;
        }
        if (a == "--help" || a == "-h") {
            std::printf("um-bot %s - an Evil Islands companion player (skeleton: settings only, no network yet)\n"
                        "Usage: um-bot [--version | --connect-test]\nSettings: um-bot.cfg next to the program.\n", kVersion);
            return 0;
        }
    }
    if (!glfwInit()) { std::fprintf(stderr, "cannot start GLFW\n"); return 1; }
    GLFWwindow* window = glfwCreateWindow(900, 560, (std::string("um-bot ") + kVersion).c_str(), nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "cannot open a window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini: the layout is fixed
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    App app;
    bot::Load(app.cfg);
    app.Log("um-bot " + std::string(kVersion) + ": it joins a game (handshake and login); playing in it comes next.");

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
