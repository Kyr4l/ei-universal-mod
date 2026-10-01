// um-bot: a companion player for Evil Islands multiplayer. It is meant to join a game like a normal
// player (its own network client, no game copy) and play alongside a human: follow, heal, fight their
// targets, by the settings of this window.
//
// Skeleton: the settings and the window. The network client (the game's UDP protocol, port 8888) is not
// written yet: Connect only says so. See README.md for the plan.
#include <GLFW/glfw3.h>

#include <cstdio>
#include <deque>
#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include "bot_config.hpp"

static const char* const kVersion = "0.1";

struct App {
    bot::Config cfg;
    bool dirty = false;               // settings changed since the last save
    enum class Link { Disconnected, Connecting, Connected } link = Link::Disconnected;
    std::deque<std::string> log;      // newest last
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
    const bool idle = app.link == App::Link::Disconnected;
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
    char name[64];
    std::snprintf(name, sizeof name, "%s", c.name.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("Bot name", name, sizeof name)) { c.name = name; app.dirty = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (idle) {
        if (ImGui::Button("Connect"))
            app.Log("Connect to " + c.host + ":" + std::to_string(c.port) +
                    ": not available yet, the game's network protocol is still to be decoded (see README.md).");
    } else if (ImGui::Button("Disconnect")) {
        app.link = App::Link::Disconnected;
        app.Log("Disconnected");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.link == App::Link::Connected ? "Connected" : app.link == App::Link::Connecting ? "Connecting..." : "Not connected");
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
    const bool on = app.link == App::Link::Connected;
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
        if (a == "--help" || a == "-h") {
            std::printf("um-bot %s - an Evil Islands companion player (skeleton: settings only, no network yet)\n"
                        "Usage: um-bot [--version]\nSettings: um-bot.cfg next to the program.\n", kVersion);
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
    app.Log("um-bot " + std::string(kVersion) + ": the network client is not written yet; the settings are saved for it.");

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
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
