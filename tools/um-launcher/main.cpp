// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um-launcher: starts Evil Islands with a mod (the replacement of EIStarter OBT-1, see docs/launcher.md). It keeps its settings in launcher.cfg,
// writes what the loader chain (Engine\dinput.dll -> addon.dll) reads from the registry at the moment the game starts, starts Engine\game.exe,
// and shows "Starting..." until the game window is up. The looks, sounds, fonts, languages and mod folders are EIStarter's, so its skins and
// translations work.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "laa.hpp"
#include "lang.hpp"
#include "mods.hpp"
#include "options_ui.hpp"
#include "platform.hpp"
#include "regsync.hpp"
#include "skin.hpp"

using namespace launcher;

static constexpr const char* kVersion = "0.1.0";
static constexpr int kW = 640, kH = 360;
static const ImVec4 kSteel(70 / 255.f, 130 / 255.f, 180 / 255.f, 1);

struct Button { const char* mask; const char* fallback; int x, y; };
static const Button kButtons[] = {
    {"pl", "Play", 237, 91}, {"op", "Options", 237, 126}, {"re", "Readme", 237, 161},
    {"we", "Web", 237, 196}, {"un", "Uninstall", 237, 231}, {"ex", "Exit", 237, 266},
};

struct App {
    fs::path dir;                       // the launcher's folder
    LauncherConfig cfg;
    std::vector<ModInfo> mods;
    int modIndex = 0;
    std::vector<std::string> languages;
    Skin skin;
    Texts texts;
    OptionsWindow options;
    ImFont* uiFont = nullptr;
    std::map<std::string, ImFont*> skinFonts;
    ImFont* skinFont = nullptr;
    std::string clickSound;
    bool quit = false;
    // The game start.
    enum class Phase { Idle, Starting, Running, Failed } phase = Phase::Idle;
    platform::Process proc;
    double phaseStart = 0, windowSeen = 0;
    std::string status, failure;
    bool showInfo = false;
    std::string message;                // a message popup
    std::ofstream log;

    void Log(const std::string& line) {
        if (!log.is_open()) log.open((dir / "launcher.log").string(), std::ios::trunc);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char t[32]; std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&now));
        log << t << " " << line << "\n"; log.flush();
    }

    const ModInfo* CurrentMod() const { return modIndex >= 0 && modIndex < static_cast<int>(mods.size()) ? &mods[static_cast<size_t>(modIndex)] : nullptr; }

    void Save() { cfg.mod = CurrentMod() ? CurrentMod()->folder : cfg.mod; SaveConfig(dir, cfg); }

    // The mod folder's design\ when ModSkins is on and it has one.
    void ApplySkin() {
        std::string modDesign;
        if (cfg.modSkins && CurrentMod()) { std::error_code ec; const fs::path p = fs::path(CurrentMod()->path) / "design"; if (fs::is_directory(p, ec)) modDesign = p.string(); }
        skin.Clear();
        skin.Configure(dir, modDesign, cfg.language);
        clickSound = skin.ClickSound();
        texts.Load(dir.string(), cfg.language);
        const std::string fp = skin.FontPath();
        skinFont = nullptr;
        if (!fp.empty()) {
            auto it = skinFonts.find(fp);
            if (it == skinFonts.end()) {
                ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(fp.c_str(), 14.5f);
                it = skinFonts.emplace(fp, f).first;
            }
            skinFont = it->second;
            Log(skinFont ? "skin font " + fp : "skin font " + fp + " could not be loaded");
        }
    }

    void ReloadMods() {
        const std::string keep = CurrentMod() ? CurrentMod()->folder : cfg.mod;
        mods = ScanMods(dir, cfg.usingIniConfigs);
        modIndex = 0;
        bool found = false;
        for (size_t i = 0; i < mods.size(); ++i) if (mods[i].folder == keep) { modIndex = static_cast<int>(i); found = true; }
        // Nothing chosen yet: the first real mod ("no mods" is the folder .0).
        if (!found) for (size_t i = 0; i < mods.size(); ++i) if (mods[i].folder != ".0") { modIndex = static_cast<int>(i); break; }
        if (mods.empty()) { ModInfo none; none.title = "No mods"; none.folder = ""; mods.push_back(none); }
    }

    void Click() { if (!clickSound.empty()) platform::PlayWav(clickSound); }

    bool GameFolderOk() const { return LooksLikeGameFolder(cfg.game.GetString("Path Settings", "Work Path")); }

    void Play() {
        Log("Play: mod '" + (CurrentMod() ? CurrentMod()->folder : std::string()) + "', engine " + cfg.exeName);
        const fs::path exe = dir / "Engine" / cfg.exeName;
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec)) { message = "Engine/" + cfg.exeName + " not found!"; Log(message); return; }
        if (!CurrentMod() || CurrentMod()->folder.empty()) { message = "There is no mod to start (mods/ holds no mod folder)."; Log(message); return; }
        if (!GameFolderOk()) { options.Open(cfg, true, "Evil Islands was not found. Choose the folder of the original game (the one with game.exe and res)."); return; }
        Save();
        Log("writing the registry");
        SyncResult r = SyncRegistry(cfg, dir, CurrentMod()->folder);
        if (!r.ok) { message = r.error; Log(message); return; }
        ApplyBorderless();
        if (cfg.largeAddressAware) {
            const int laa = EnsureLargeAddressAware(exe);
            Log(laa > 0 ? "Engine/" + cfg.exeName + ": large address aware flag set (the original is kept as .orig)" : laa == 0 ? "large address aware: already set" : "large address aware: could not patch " + exe.string());
        }
        std::string error;
        if (!platform::Start(exe, dir / "Engine", proc, error)) { failure = error; phase = Phase::Failed; Log("start failed: " + error); return; }
        Log("game started");
        phase = Phase::Starting; phaseStart = glfwGetTime(); windowSeen = 0; status = "Starting the game...";
    }

    // Borderless windowed (the game window keeps running when it loses the focus, the mouse stays inside): um.dll does it for the mods that use it
    // (um.cfg BORDERLESS), Engine\\um-window.asi (window/) for every other mod, switched by Engine\\um-window.ini.
    void ApplyBorderless() {
        const bool on = cfg.borderless && !cfg.game.GetBool("general settings", "fullscreen");
        std::error_code ec;
        if (CurrentMod()) {
            const std::string umcfg = (fs::path(CurrentMod()->path) / "um.cfg").string();
            if (SetCfgLine(umcfg, "BORDERLESS", on ? "true" : "false")) Log(std::string("um.cfg BORDERLESS ") + (on ? "on" : "off"));
        }
        if (fs::exists(dir / "Engine" / "um-window.asi", ec)) {
            IniFile ini;
            ini.SetInt("Window", "Borderless", on ? 1 : 0);
            ini.Save((dir / "Engine" / "um-window.ini").string());
            Log(std::string("Engine/um-window.ini Borderless ") + (on ? "1" : "0"));
        }
    }

    void Poll() {
        if (phase != Phase::Starting && phase != Phase::Running) return;
        int code = 0;
        if (!platform::Running(proc, code)) {
            failure = "The game exited" + std::string(phase == Phase::Starting ? " before its window came up" : "") + " (exit code " + std::to_string(code) + "). See addon.log in this folder.";
            Log(failure);
            phase = phase == Phase::Starting ? Phase::Failed : Phase::Idle;
            return;
        }
        if (phase == Phase::Starting) {
            if (platform::HasWindow(proc)) {
                if (windowSeen == 0) { windowSeen = glfwGetTime(); Log("game window up"); }
                if (glfwGetTime() - windowSeen > 1.0) { phase = Phase::Running; status = "The game is running."; if (cfg.closeOnStart) quit = true; }
            } else if (glfwGetTime() - phaseStart > 120) { failure = "The game window did not come up in 120 seconds."; phase = Phase::Failed; Log(failure); }
        }
    }
};

static std::string FormatInfo(const Texts& tx, const ModInfo& m) {
    std::string t = tx.Get("ModInfo", "ModDesc", "{0} \nVersion: {1} \nAuthor(s): {2} \nDate: {3} \n\nSite: {4} \nE-mail: {5} \n\nSingleplayer: {6} \nMultiplayer: {7} \n");
    const std::string vals[8] = {m.title, m.version, m.author, m.date, m.site, m.email, m.single ? "True" : "False", m.multi ? "True" : "False"};
    for (int i = 0; i < 8; ++i) for (size_t p; (p = t.find("{" + std::to_string(i) + "}")) != std::string::npos;) t.replace(p, 3, vals[i]);
    return t;
}

static ImTextureID Tid(const Texture& t) { return static_cast<ImTextureID>(t.id); }

// One of the six menu buttons: its image (pressed / hovered / normal) at the original position, or a plain button without a skin.
static bool MenuButton(App& app, const char* mask, const char* fallback, ImVec2 pos, bool enabled = true) {
    const Texture& normal = app.skin.Image(mask);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!normal) {
        ImGui::SetCursorPos(pos);
        ImGui::BeginDisabled(!enabled);
        const bool hit = ImGui::Button(fallback, ImVec2(130, 34));
        ImGui::EndDisabled();
        return hit;
    }
    const ImVec2 size(static_cast<float>(normal.w - 1), static_cast<float>(normal.h - 1));
    ImGui::SetCursorPos(pos);
    ImGui::InvisibleButton((std::string("##") + mask).c_str(), size);
    const bool hovered = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    const Texture* shown = &normal;
    if (down) { const Texture& d = app.skin.Image(std::string(mask) + "_d"); if (d) shown = &d; }
    else if (hovered) { const Texture& h = app.skin.Image(std::string(mask) + "_h"); if (h) shown = &h; }
    const ImVec2 origin = ImGui::GetWindowPos();
    dl->AddImage(Tid(*shown), ImVec2(origin.x + pos.x, origin.y + pos.y), ImVec2(origin.x + pos.x + static_cast<float>(shown->w), origin.y + pos.y + static_cast<float>(shown->h)));
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return ImGui::IsItemDeactivated() && hovered && enabled;
}

static void DrawMain(App& app, GLFWwindow* window) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(kW, kH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);

    const bool idle = app.phase == App::Phase::Idle || app.phase == App::Phase::Running;
    const bool modal = app.options.IsOpen() || app.showInfo || !app.message.empty() || app.phase == App::Phase::Starting || app.phase == App::Phase::Failed;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const Texture& back = app.skin.Background();
    if (back) bg->AddImage(Tid(back), ImVec2(0, 0), ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    else bg->AddRectFilled(ImVec2(0, 0), ImVec2(kW, kH), IM_COL32(44, 44, 44, 255));

    ImGui::BeginDisabled(modal);
    bool play = false, opt = false, readme = false, web = false, uninstall = false, exit = false;
    play = MenuButton(app, "pl", "Play", ImVec2(237, 91), idle);
    opt = MenuButton(app, "op", "Options", ImVec2(237, 126));
    readme = MenuButton(app, "re", "Readme", ImVec2(237, 161));
    web = MenuButton(app, "we", "Web", ImVec2(237, 196));
    uninstall = MenuButton(app, "un", "Uninstall", ImVec2(237, 231));
    exit = MenuButton(app, "ex", "Exit", ImVec2(237, 266));

    // The mod chooser beside Play, and the (i) button.
    const Texture& playImg = app.skin.Image("pl");
    const float comboX = 237 + (playImg ? playImg.w - 1 : 130) + 10;
    ImGui::SetCursorPos(ImVec2(comboX, 98));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.2f, 0.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.2f, 0.3f, 1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.3f, 0.45f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    if (app.skinFont) ImGui::PushFont(app.skinFont, 14.5f);
    ImGui::SetNextItemWidth(169);
    const ModInfo* cur = app.CurrentMod();
    if (ImGui::BeginCombo("##mod", cur ? cur->title.c_str() : "No mods")) {
        for (int i = 0; i < static_cast<int>(app.mods.size()); ++i)
            if (ImGui::Selectable((app.mods[static_cast<size_t>(i)].title + "##" + std::to_string(i)).c_str(), i == app.modIndex)) {
                app.Click();
                if (i != app.modIndex) { app.modIndex = i; app.ApplySkin(); app.Save(); }
            }
        ImGui::EndCombo();
    }
    if (app.skinFont) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);
    {
        const float infoX = comboX + 169 + 10;
        const Texture& infoImg = app.skin.Image("info");
        if (infoImg) {
            ImGui::SetCursorPos(ImVec2(infoX, 93));
            ImGui::InvisibleButton("##info", ImVec2(static_cast<float>(infoImg.w - 1), static_cast<float>(infoImg.h - 1)));
            const Texture& show = ImGui::IsItemHovered() && app.skin.Image("info_h") ? app.skin.Image("info_h") : infoImg;
            bg->AddImage(Tid(show), ImVec2(infoX, 93), ImVec2(infoX + static_cast<float>(show.w), 93 + static_cast<float>(show.h)));
            if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) { app.Click(); app.showInfo = true; }
        } else {
            ImGui::SetCursorPos(ImVec2(infoX, 98));
            if (ImGui::Button("i", ImVec2(24, 23))) app.showInfo = true;
        }
        // The language button, bottom left.
        const Texture& langImg = app.skin.Image("lang");
        ImGui::SetCursorPos(ImVec2(1, static_cast<float>(kH) - (langImg ? static_cast<float>(langImg.h - 1) : 22.f)));
        bool lang = false;
        if (langImg) {
            ImGui::InvisibleButton("##lang", ImVec2(static_cast<float>(langImg.w - 1), static_cast<float>(langImg.h - 1)));
            const Texture& show = ImGui::IsItemHovered() && app.skin.Image("lang_h") ? app.skin.Image("lang_h") : langImg;
            const ImVec2 p = ImGui::GetItemRectMin();
            bg->AddImage(Tid(show), p, ImVec2(p.x + static_cast<float>(show.w), p.y + static_cast<float>(show.h)));
            lang = ImGui::IsItemDeactivated() && ImGui::IsItemHovered();
        } else lang = ImGui::Button(("Lang: " + app.cfg.language).c_str());
        if (lang && app.languages.size() > 1) {
            auto it = std::find(app.languages.begin(), app.languages.end(), app.cfg.language);
            app.cfg.language = app.languages[it == app.languages.end() ? 0 : static_cast<size_t>(it - app.languages.begin() + 1) % app.languages.size()];
            app.ApplySkin(); app.Save();
        }
    }
    ImGui::EndDisabled();

    if (!modal) {
        if (play || ImGui::IsKeyPressed(ImGuiKey_Enter)) { app.Click(); app.Play(); }
        if (opt) { app.Click(); app.options.Open(app.cfg, !app.GameFolderOk(), app.GameFolderOk() ? "" : "Choose the folder of the original game.", app.CurrentMod() ? (fs::path(app.CurrentMod()->path) / "um.cfg").string() : std::string()); }
        if (readme) {
            app.Click();
            fs::path p = app.dir / "lang" / app.cfg.language / "readme.txt", en = app.dir / "lang" / "en" / "readme.txt";
            std::error_code ec;
            if (fs::exists(p, ec)) platform::OpenWithDefault(p.string()); else if (fs::exists(en, ec)) platform::OpenWithDefault(en.string());
        }
        if (web) { app.Click(); platform::OpenWithDefault("https://allods.gipat.ru"); }
        if (uninstall) {
            app.Click();
            std::error_code ec;
            if (fs::exists(app.dir / "uninstall.exe", ec)) platform::OpenWithDefault((app.dir / "uninstall.exe").string()); else app.message = "Uninstall.exe not found!";
        }
        if (exit || ImGui::IsKeyPressed(ImGuiKey_Escape)) { app.Click(); app.quit = true; }
    }

    // Dragging the window by its picture.
    static bool dragging = false; static double sx = 0, sy = 0;
    if (!modal || app.phase == App::Phase::Starting) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ) { dragging = true; glfwGetCursorPos(window, &sx, &sy); }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) { dragging = true; glfwGetCursorPos(window, &sx, &sy); }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    if (dragging) {
        double cx, cy; glfwGetCursorPos(window, &cx, &cy);
        int wx, wy; glfwGetWindowPos(window, &wx, &wy);
        glfwSetWindowPos(window, wx + static_cast<int>(cx - sx), wy + static_cast<int>(cy - sy));
    }
    (void)io;
    ImGui::End();

    // Options.
    {
        LauncherConfig result;
        if (app.options.Draw(app.texts, result, ImVec2(kW, kH)) == OptionsWindow::Result::Ok) {
            const bool reloadMods = result.usingIniConfigs != app.cfg.usingIniConfigs;
            const bool reskin = result.modSkins != app.cfg.modSkins;
            app.cfg = result;
            if (reloadMods) app.ReloadMods();
            if (reskin || reloadMods) app.ApplySkin();
            app.Save();
        }
    }

    // The mod's information.
    if (app.showInfo) {
        ImGui::SetNextWindowPos(ImVec2(kW / 2.f, kH / 2.f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 300));
        if (ImGui::Begin((app.texts.Get("ModInfo", "ModInfo", "ModInfo") + "###info").c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            const ModInfo* m = app.CurrentMod();
            if (m) {
                ImGui::BeginChild("##infotext", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4));
                ImGui::TextWrapped("%s", FormatInfo(app.texts, *m).c_str());
                ImGui::EndChild();
                // The mod folder's readme / changelog, and the mod's own script (its [starter] plugin button).
                std::error_code ec; std::string readme, changelog;
                for (const fs::directory_entry& f : fs::directory_iterator(m->path, ec)) {
                    std::string n = LowerAscii(f.path().filename().string());
                    if (n.find("read") != std::string::npos && n.find("me") != std::string::npos && readme.empty()) readme = f.path().string();
                    if (n.find("change") != std::string::npos && n.find("log") != std::string::npos && changelog.empty()) changelog = f.path().string();
                }
                if (!readme.empty() && ImGui::Button(app.texts.Get("ModInfo", "btReadme", "Read Me").c_str())) platform::OpenWithDefault(readme);
                if (!readme.empty()) ImGui::SameLine();
                if (!changelog.empty() && ImGui::Button(app.texts.Get("ModInfo", "btChangelog", "Changelog").c_str())) platform::OpenWithDefault(changelog);
                if (!changelog.empty()) ImGui::SameLine();
                if (m->pluginPath != "none" && fs::exists(fs::path(m->path) / m->pluginPath, ec) && ImGui::Button(m->pluginText.c_str())) platform::RunScript(fs::path(m->path) / m->pluginPath);
            }
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 90, ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 8));
            if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) app.showInfo = false;
        }
        ImGui::End();
    }

    // "Starting..." / the failure.
    if (app.phase == App::Phase::Starting || app.phase == App::Phase::Failed) {
        ImGui::SetNextWindowPos(ImVec2(kW / 2.f, kH / 2.f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, app.phase == App::Phase::Failed ? 170 : 110));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.04f, 0.07f, 0.94f));
        if (ImGui::Begin("##starting", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            if (app.phase == App::Phase::Starting) {
                ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
                ImGui::SetCursorPosY(24);
                const char* t = app.windowSeen > 0 ? "Loading the game..." : "Starting the game...";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) / 2);
                ImGui::TextUnformatted(t);
                ImGui::PopStyleColor();
                const float t0 = static_cast<float>(glfwGetTime());
                ImGui::SetCursorPos(ImVec2(30, 62));
                ImGui::ProgressBar(-1.0f * t0, ImVec2(ImGui::GetWindowWidth() - 60, 14), "");
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The game did not start");
                ImGui::TextWrapped("%s", app.failure.c_str());
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 36);
                if (ImGui::Button("Open addon.log")) platform::OpenWithDefault((app.dir / "addon.log").string());
                ImGui::SameLine(); ImGui::Button("launcher.log") ? platform::OpenWithDefault((app.dir / "launcher.log").string()) : (void)0;
                ImGui::SameLine(); if (ImGui::Button("Close")) app.phase = App::Phase::Idle;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // A message.
    if (!app.message.empty()) {
        ImGui::SetNextWindowPos(ImVec2(kW / 2.f, kH / 2.f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(380, 120));
        if (ImGui::Begin("##message", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("%s", app.message.c_str());
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 90, ImGui::GetWindowHeight() - 34));
            if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) app.message.clear();
        }
        ImGui::End();
    }
}

int main(int argc, char** argv) {
    App app;
    app.dir = platform::ExeDir();
    bool selfTest = false; std::string shot;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) app.dir = argv[++i];
        else if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--play") selfTest = true;
    }
    app.Log(std::string("um-launcher ") + kVersion + " in " + app.dir.string());

    if (!glfwInit()) { std::fprintf(stderr, "cannot start GLFW\n"); return 1; }
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(kW, kH, "Evil Islands", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "cannot open a window\n"); glfwTerminate(); return 1; }
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int mx, my, mw, mh; glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        glfwSetWindowPos(window, mx + (mw - kW) / 2, my + (mh - kH) / 2);
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    for (const char* path : {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", "/usr/share/fonts/truetype/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"}) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec) && (app.uiFont = io.Fonts->AddFontFromFileTTF(path, 14.0f))) break;
    }
    if (!app.uiFont) app.uiFont = io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0; ImGui::GetStyle().FrameRounding = 2;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    app.cfg = LoadConfig(app.dir);
    app.languages = EnumerateLanguages(app.dir);
    if (!app.languages.empty() && std::find(app.languages.begin(), app.languages.end(), app.cfg.language) == app.languages.end()) app.cfg.language = app.languages[0];
    app.ReloadMods();
    app.ApplySkin();
    if (app.cfg.firstRun) { // a mod that already runs borderless (um.cfg BORDERLESS=true) keeps doing so
        if (const ModInfo* m = app.CurrentMod()) app.cfg.borderless = LowerAscii(GetCfgLine((fs::path(m->path) / "um.cfg").string(), "BORDERLESS")) == "true";
        app.Save();
    }
    if (!app.GameFolderOk()) { // look for the game where an installation puts it
        for (const char* guess : {"C:\\Evil Islands\\", "C:\\Program Files (x86)\\Evil Islands\\", "C:\\Program Files\\Evil Islands\\", "C:\\GOG Games\\Evil Islands\\"}) {
            if (LooksLikeGameFolder(guess)) {
                const std::string w = guess;
                app.cfg.game.SetString("Path Settings", "Work Path", w); app.cfg.game.SetString("Path Settings", "CD-ROM Path", w);
                app.cfg.game.SetString("Path Settings", "Resources Path", w + "Res\\");
                break;
            }
        }
    }
    glfwShowWindow(window);
    int frame = 0;
    while (!glfwWindowShouldClose(window) && !app.quit) {
        glfwWaitEventsTimeout(app.phase == App::Phase::Starting ? 0.016 : 0.05);
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(app.uiFont, 14.0f);
        app.Poll();
        DrawMain(app, window);
        ImGui::PopFont();
        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.17f, 0.17f, 0.17f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        ++frame;
        if (selfTest && frame == 5) app.Play();
        if (!shot.empty() && frame == 12) {
            std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(shot.c_str(), "wb")) { // PPM, flipped
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; --y) std::fwrite(&px[static_cast<size_t>(y) * static_cast<size_t>(w) * 3], 1, static_cast<size_t>(w) * 3, f);
                std::fclose(f);
            }
            break;
        }
    }
    app.Save();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
