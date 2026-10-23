// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// EIStarter-UM: starts Evil Islands with a mod (the replacement of EIStarter OBT-1, see docs/EIStarter-UM.md). It keeps its settings in EIStarter-UM.cfg,
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
#include "icon_data.hpp"
#include "lang.hpp"
#include "mods.hpp"
#include "options_ui.hpp"
#include "platform.hpp"
#include "regsync.hpp"
#include "skin.hpp"
#include "update.hpp"

using namespace launcher;

static constexpr const char* kVersion = "0.2.2";
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
    // The keyboard selection, as in OBT-1 (Tab moves it): 0 Play, 1 mod chooser, 2 Options, 3 Readme, 4 Website, 5 Uninstall, 6 Exit, 7 Lang, 8 (i).
    int focus = 0;
    static constexpr int kFocusCount = 9;
    std::string message;                // a message popup
    std::ofstream log;
    void Log(const std::string& line) {
        if (!log.is_open()) log.open((dir / "EIStarter-UM.log").string(), std::ios::trunc);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char t[32]; std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&now));
        log << t << " " << line << "\n"; log.flush();
    }

    // Mod updates (update.hpp). The mod's two lines are installed side by side so their saves never mix: the newest release into mods/Universal-Mod,
    // the newest pre-release (beta) into mods/Universal-Mod-Beta.
    struct Channel { const char* label; const char* folder; bool stableOnly; update::Updater updater; bool handled = true; };
    Channel channels[2] = {{"Release (master)", "Universal-Mod", true}, {"Pre-Release (beta)", "Universal-Mod-Beta", false}};
    std::string UpdateRepo() const { auto it = cfg.updateRepos.find("*"); return it != cfg.updateRepos.end() ? it->second : std::string("Kyr4l/ei-universal-mod"); }
    bool UpdateBusy() const { for (const Channel& c : channels) if (c.updater.Busy()) return true; return false; }
    bool UpdateInstalling() const { for (const Channel& c : channels) if (c.updater.Installing()) return true; return false; } // files are being replaced: no Play
    const ModInfo* FindMod(const std::string& folder) const { for (const ModInfo& m : mods) if (LowerAscii(m.folder) == LowerAscii(folder)) return &m; return nullptr; }
    // Mods that share a title are told apart by their folder in the chooser.
    std::string DisplayTitle(const ModInfo& m) const {
        int same = 0;
        for (const ModInfo& o : mods) if (o.title == m.title) ++same;
        if (same <= 1) return m.title;
        const std::string l = LowerAscii(m.folder);
        const bool beta = l.size() > 5 && l.compare(l.size() - 5, 5, "-beta") == 0;
        return m.title + " (" + (beta ? "beta" : l == "universal-mod" ? "release" : m.folder) + ")";
    }
    // First start with no Universal Mod in mods\\: offer to download it (the same updater as Info > Updates) and let the user pick the line.
    enum class Offer { None, Asking, Working, Failed };
    Offer offer = Offer::None;
    int offerChannel = 0;           // which of channels[]: 0 release, 1 beta
    bool offerInstalling = false;   // the install of the chosen channel has been started
    std::string offerError;
    bool UmDetected() const { for (const ModInfo& m : mods) if (m.umListed && m.umFile) return true; return false; }
    void StartOffer() {
        Channel& c = channels[offerChannel];
        Log(std::string("no Universal Mod: downloading ") + c.label);
        offerInstalling = false; offerError.clear();
        offer = Offer::Working;
        CheckChannel(c);
    }
    void PollOffer() {
        if (offer != Offer::Working) return;
        using S = update::Updater::State;
        Channel& c = channels[offerChannel];
        const auto snap = c.updater.Get();
        if (!offerInstalling && (snap.state == S::Available || snap.state == S::UpToDate)) { offerInstalling = true; InstallChannel(c); }
        else if (offerInstalling && snap.state == S::Done && c.handled) { // Poll has read the new mod: choose it
            for (size_t i = 0; i < mods.size(); ++i) if (mods[i].folder == c.folder) { modIndex = static_cast<int>(i); Save(); break; }
            Log("Universal Mod installed: " + snap.message);
            offer = Offer::None;
        } else if (snap.state == S::Failed) { offerError = snap.message; offer = Offer::Failed; Log("download failed: " + snap.message); }
    }
    void CheckChannel(Channel& c) {
        auto key = cfg.updateKeys.find(c.folder);
        c.updater.Check(UpdateRepo(), c.stableOnly, key == cfg.updateKeys.end() ? std::string() : key->second);
    }
    void CheckUpdates() { for (Channel& c : channels) CheckChannel(c); }
    // The commit of the build in a channel's folder: build.info (makemod writes it), else the one the updater recorded.
    std::string InstalledHash(const Channel& c) const {
        const ModInfo* m = FindMod(c.folder);
        if (!m) return {};
        if (!m->commit.empty()) return m->commit;
        auto it = cfg.updateKeys.find(c.folder);
        if (it != cfg.updateKeys.end()) { const size_t a = it->second.find('|'); if (a != std::string::npos && it->second.find('|', a + 1) == std::string::npos) return it->second.substr(a + 1); }
        return {};
    }
    static bool SameCommit(const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        return n >= 7 && a.compare(0, n, b, 0, n) == 0;
    }
    // "0.8.6 (b373a09)": the version with the commit when it is known.
    static std::string BuildLabel(const std::string& version, const std::string& hash) { return version + (hash.size() >= 7 ? " (" + hash.substr(0, 7) + ")" : std::string()); }
    // True when the channel's newest build is worth offering for its folder: a missing folder always; otherwise another commit than the folder's; with no
    // commit to compare, another build than the updater recorded; for a folder it did not install, a newer version number.
    bool WorthInstalling(const Channel& c, const update::Release& r) const {
        const ModInfo* m = FindMod(c.folder);
        if (!m) return true;
        const std::string inst = InstalledHash(c);
        if (!r.hash.empty() && !inst.empty()) return !SameCommit(inst, r.hash);
        auto it = cfg.updateKeys.find(c.folder);
        if (it != cfg.updateKeys.end()) return it->second != r.Key();
        return update::CompareVersions(r.tag, m->version) > 0;
    }
    void InstallChannel(Channel& c) {
        const auto snap = c.updater.Get();
        if (snap.state != update::Updater::State::Available && snap.state != update::Updater::State::UpToDate) return;
        Log("update: " + snap.latest.tag + " (" + snap.latest.assetName + ") into mods/" + c.folder);
        c.handled = false;
        c.updater.Install(snap.latest, dir / "mods" / c.folder, dir / "mods" / ".update", {"mp", "saves"});
    }

    const ModInfo* CurrentMod() const { return modIndex >= 0 && modIndex < static_cast<int>(mods.size()) ? &mods[static_cast<size_t>(modIndex)] : nullptr; }

    void Save() { cfg.mod = CurrentMod() ? CurrentMod()->folder : cfg.mod; SaveConfig(dir, cfg); }

    // The mod folder's design\ when ModSkins is on and it has one.
    void ApplySkin() {
        std::string modDesign;
        if (cfg.modSkins && CurrentMod()) { std::error_code ec; const fs::path p = fs::path(CurrentMod()->path) / "design"; if (fs::is_directory(p, ec)) modDesign = p.string(); }
        skin.Configure(dir, modDesign, cfg.language); // no Clear(): textures already in this frame's draw list must stay valid
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
        if (!CurrentMod() || CurrentMod()->folder.empty()) { message = "No mod in the mods folder."; Log(message); return; }
        if (!GameFolderOk()) { options.Open(cfg, true, "Evil Islands not found. Choose its folder."); return; }
        if (NormalizePaths(cfg.game)) Log("path settings repaired");
        Save();
        Log("writing the registry");
        static App* self = this;
        SyncResult r = SyncRegistry(cfg, dir, CurrentMod()->folder, cfg.debugLog ? +[](const std::string& line) { self->Log("  " + line); } : nullptr);
        if (!r.ok) { message = r.error; Log(message); return; }
        ApplyBorderless();
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
        for (Channel& c : channels) { // a finished update: remember the build, read the new mod
            const auto snap = c.updater.Get();
            if (snap.state == update::Updater::State::Done && !c.handled) {
                c.handled = true;
                cfg.updateKeys[c.folder] = snap.latest.Key();
                Log("update done: " + snap.message);
                ReloadMods(); ApplySkin(); Save();
            } else if (snap.state == update::Updater::State::Failed && !c.handled) { c.handled = true; Log("update failed: " + snap.message); }
        }
        PollOffer();
        if (phase != Phase::Starting && phase != Phase::Running) return;
        int code = 0;
        if (!platform::Running(proc, code)) {
            failure = "The game exited (code " + std::to_string(code) + "). See addon.log.";
            Log(failure);
            phase = phase == Phase::Starting ? Phase::Failed : Phase::Idle;
            return;
        }
        if (phase == Phase::Starting) {
            if (platform::HasWindow(proc)) {
                if (windowSeen == 0) { windowSeen = glfwGetTime(); Log("game window up"); }
                if (glfwGetTime() - windowSeen > 1.0) { phase = Phase::Running; status = "The game is running."; if (cfg.closeOnStart) quit = true; }
            } else if (glfwGetTime() - phaseStart > 120) { failure = "The game did not start within 120 s."; phase = Phase::Failed; Log(failure); }
        }
    }
};

static std::string FormatInfo(const Texts& tx, const ModInfo& m) {
    std::string t = tx.Get("ModInfo", "ModDesc", "{0} \nVersion: {1} \nAuthor(s): {2} \nDate: {3} \n\nSite: {4} \nE-mail: {5} \n\nSingle player: {6} \nMultiplayer: {7} \n");
    const std::string vals[8] = {m.title, m.version, m.author, m.date, m.site, m.email, m.single ? "True" : "False", m.multi ? "True" : "False"};
    for (int i = 0; i < 8; ++i) for (size_t p; (p = t.find("{" + std::to_string(i) + "}")) != std::string::npos;) t.replace(p, 3, vals[i]);
    return t;
}

static ImTextureID Tid(const Texture& t) { return static_cast<ImTextureID>(t.id); }

// One of the six menu buttons: its image (pressed / hovered / normal) at the original position, or a plain button without a skin.
static bool MenuButton(App& app, const char* mask, const char* fallback, ImVec2 pos, int index, bool enabled = true) {
    const Texture& normal = app.skin.Image(mask);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const bool focused = app.focus == index;
    if (!normal) {
        ImGui::SetCursorPos(pos);
        ImGui::BeginDisabled(!enabled);
        if (focused) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        const bool hit = ImGui::Button(fallback, ImVec2(130, 34));
        if (focused) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemActivated()) app.focus = index;
        return hit;
    }
    const ImVec2 size(static_cast<float>(normal.w - 1), static_cast<float>(normal.h - 1));
    ImGui::SetCursorPos(pos);
    ImGui::InvisibleButton((std::string("##") + mask).c_str(), size);
    const bool hovered = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) app.focus = index; // a click selects the button, like in OBT-1
    const Texture* shown = &normal;
    // Pressed or keyboard-selected: the _d picture (what OBT-1 shows for a focused button); the mouse over it: _h.
    if (down || focused) { const Texture& d = app.skin.Image(std::string(mask) + "_d"); if (d) shown = &d; }
    else if (hovered) { const Texture& h = app.skin.Image(std::string(mask) + "_h"); if (h) shown = &h; }
    const ImVec2 origin = ImGui::GetWindowPos();
    dl->AddImage(Tid(*shown), ImVec2(origin.x + pos.x, origin.y + pos.y), ImVec2(origin.x + pos.x + static_cast<float>(shown->w), origin.y + pos.y + static_cast<float>(shown->h)));
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return ImGui::IsItemDeactivated() && hovered && enabled;
}

// Bottom right corner: whether the selected mod uses um.dll, and the state of its update.
static void DrawCorner(App& app, ImDrawList* bg) {
    const ModInfo* m = app.CurrentMod();
    if (!m) return;
    struct Line { std::string text; ImU32 color; std::string tip; };
    std::vector<Line> lines;
    const bool ok = m->umListed && m->umFile;
    const ImU32 green = IM_COL32(110, 210, 120, 255), amber = IM_COL32(235, 180, 70, 255), grey = IM_COL32(195, 195, 195, 255);
    lines.push_back({ok ? "um.dll detected" : m->umFile ? "um.dll not enabled" : m->umListed ? "um.dll missing" : "no um.dll",
                     ok ? green : (m->umFile || m->umListed) ? amber : grey,
                     ok ? "config.reg loads um.dll." : m->umFile ? "um.dll is there, but config.reg [DEV] Plugin does not name it." : m->umListed ? "config.reg names um.dll, but the file is missing." : "This mod does not use um.dll."});
    {
        using S = update::Updater::State;
        char buf[96];
        bool shown = false;
        for (App::Channel& c : app.channels) { // an update in progress, whichever channel
            const auto snap = c.updater.Get();
            if (snap.state == S::Downloading) { std::snprintf(buf, sizeof buf, "updating %s: %d%%", c.folder, static_cast<int>(snap.progress * 100)); lines.push_back({buf, amber, "Play is disabled meanwhile."}); shown = true; }
            else if (snap.state == S::Extracting || snap.state == S::Applying) { lines.push_back({std::string("updating ") + c.folder + ": " + (snap.state == S::Extracting ? "unpacking" : "replacing files"), amber, "Play is disabled meanwhile."}); shown = true; }
        }
        if (!shown) for (App::Channel& c : app.channels) {
            if (LowerAscii(c.folder) != LowerAscii(m->folder)) continue; // the update for the selected mod's own line
            const auto snap = c.updater.Get();
            if (snap.state == S::Available && app.WorthInstalling(c, snap.latest)) lines.push_back({"update available: " + snap.latest.tag, amber, "Open (i) to install it."});
            else if (snap.state == S::Done) lines.push_back({"mod updated", green, snap.message});
        }
    }
    const ImVec2 o = ImGui::GetWindowPos();
    float y = static_cast<float>(kH) - 8;
    for (size_t i = lines.size(); i-- > 0;) {
        const Line& l = lines[i];
        const ImVec2 sz = ImGui::CalcTextSize(l.text.c_str());
        const float right = o.x + static_cast<float>(kW) - 8, top = o.y + y - sz.y;
        const ImVec2 p(right - sz.x, top);
        bg->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 255), l.text.c_str());
        bg->AddText(p, l.color, l.text.c_str());
        bg->AddCircleFilled(ImVec2(p.x - 8, top + sz.y * 0.5f + 1), 4.5f, IM_COL32(0, 0, 0, 255));
        bg->AddCircleFilled(ImVec2(p.x - 8, top + sz.y * 0.5f), 4.0f, l.color);
        if (ImGui::IsMouseHoveringRect(ImVec2(p.x - 16, top), ImVec2(right, top + sz.y))) ImGui::SetTooltip("%s", l.tip.c_str());
        y -= sz.y + 5;
    }
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
    const bool modal = app.options.IsOpen() || app.showInfo || !app.message.empty() || app.offer != App::Offer::None || app.phase == App::Phase::Starting || app.phase == App::Phase::Failed;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const Texture& back = app.skin.Background();
    if (back) bg->AddImage(Tid(back), ImVec2(0, 0), ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    else bg->AddRectFilled(ImVec2(0, 0), ImVec2(kW, kH), IM_COL32(44, 44, 44, 255));

    ImGui::BeginDisabled(modal);
    bool play = false, opt = false, readme = false, web = false, uninstall = false, exit = false;
    // Keyboard: Tab / Shift+Tab move the selection, Up / Down too through the menu (or change the mod when the chooser is selected), Enter / Space press it.
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool kb = !modal && !popupOpen;
    bool kbActivate = false;
    if (kb) {
        auto vertical = [&](int dir) { static const int order[] = {0, 2, 3, 4, 5, 6}; int at = -1; for (int i = 0; i < 6; ++i) if (order[i] == app.focus) at = i; app.focus = at < 0 ? 0 : order[(at + dir + 6) % 6]; };
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) app.focus = (app.focus + (io.KeyShift ? App::kFocusCount - 1 : 1)) % App::kFocusCount;
        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if ((up || down) && app.focus == 1) { // the chooser: another mod
            const int n = static_cast<int>(app.mods.size());
            if (n > 1) { app.modIndex = (app.modIndex + (down ? 1 : n - 1)) % n; app.ApplySkin(); app.Save(); app.Click(); }
        } else if (up) vertical(-1); else if (down) vertical(1);
        kbActivate = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Space);
    }
    play = MenuButton(app, "pl", "Play", ImVec2(237, 91), 0, idle && !app.UpdateInstalling());
    opt = MenuButton(app, "op", "Options", ImVec2(237, 126), 2);
    readme = MenuButton(app, "re", "Readme", ImVec2(237, 161), 3);
    web = MenuButton(app, "we", "Web", ImVec2(237, 196), 4);
    uninstall = MenuButton(app, "un", "Uninstall", ImVec2(237, 231), 5);
    exit = MenuButton(app, "ex", "Exit", ImVec2(237, 266), 6);
    if (kbActivate) { // Enter on the chooser starts the game too (Play is the accept button)
        switch (app.focus) { case 0: case 1: play = true; break; case 2: opt = true; break; case 3: readme = true; break; case 4: web = true; break; case 5: uninstall = true; break; case 6: exit = true; break; default: break; }
    }

    // The mod chooser beside Play, and the (i) button.
    const Texture& playImg = app.skin.Image("pl");
    const float comboX = 237 + (playImg ? playImg.w - 1 : 130) + 10;
    ImGui::SetCursorPos(ImVec2(comboX, 98));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, app.focus == 1 ? kSteel : ImVec4(0.2f, 0.2f, 0.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.2f, 0.3f, 1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.3f, 0.45f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    if (app.skinFont) ImGui::PushFont(app.skinFont, 14.5f);
    ImGui::SetNextItemWidth(169);
    const ModInfo* cur = app.CurrentMod();
    const bool comboOpen = ImGui::BeginCombo("##mod", cur ? app.DisplayTitle(*cur).c_str() : "No mods");
    if (ImGui::IsItemActivated()) app.focus = 1;
    if (comboOpen) {
        for (int i = 0; i < static_cast<int>(app.mods.size()); ++i)
            if (ImGui::Selectable((app.DisplayTitle(app.mods[static_cast<size_t>(i)]) + "##" + std::to_string(i)).c_str(), i == app.modIndex)) {
                app.Click();
                if (i != app.modIndex) { app.modIndex = i; app.ApplySkin(); app.Save(); }
            }
        ImGui::EndCombo();
    }
    if (app.skinFont) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);
    DrawCorner(app, bg);
    {
        const float infoX = comboX + 169 + 10;
        const Texture& infoImg = app.skin.Image("info");
        if (infoImg) {
            ImGui::SetCursorPos(ImVec2(infoX, 93));
            ImGui::InvisibleButton("##info", ImVec2(static_cast<float>(infoImg.w - 1), static_cast<float>(infoImg.h - 1)));
            if (ImGui::IsItemActivated()) app.focus = 8;
            const Texture& show = (ImGui::IsItemHovered() || app.focus == 8) && app.skin.Image("info_h") ? app.skin.Image("info_h") : infoImg;
            bg->AddImage(Tid(show), ImVec2(infoX, 93), ImVec2(infoX + static_cast<float>(show.w), 93 + static_cast<float>(show.h)));
            if ((ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) || (kbActivate && app.focus == 8)) { app.Click(); app.showInfo = true; }
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
            if (ImGui::IsItemActivated()) app.focus = 7;
            const Texture& show = (ImGui::IsItemHovered() || app.focus == 7) && app.skin.Image("lang_h") ? app.skin.Image("lang_h") : langImg;
            const ImVec2 p = ImGui::GetItemRectMin();
            bg->AddImage(Tid(show), p, ImVec2(p.x + static_cast<float>(show.w), p.y + static_cast<float>(show.h)));
            lang = (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) || (kbActivate && app.focus == 7);
        } else lang = ImGui::Button(("Lang: " + app.cfg.language).c_str());
        if (lang && app.languages.size() > 1) {
            auto it = std::find(app.languages.begin(), app.languages.end(), app.cfg.language);
            app.cfg.language = app.languages[it == app.languages.end() ? 0 : static_cast<size_t>(it - app.languages.begin() + 1) % app.languages.size()];
            app.ApplySkin(); app.Save();
        }
    }
    ImGui::EndDisabled();

    if (!modal) {
        if (play && idle && !app.UpdateInstalling()) { app.Click(); app.Play(); }
        if (opt) { app.Click(); app.options.Open(app.cfg, !app.GameFolderOk(), app.GameFolderOk() ? "" : "Choose the Evil Islands folder.", app.CurrentMod() ? (fs::path(app.CurrentMod()->path) / "um.cfg").string() : std::string()); }
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
        ImGui::SetNextWindowSize(ImVec2(480, 350));
        if (ImGui::Begin((app.texts.Get("ModInfo", "ModInfo", "Mod info") + "###info").c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            const ModInfo* m = app.CurrentMod();
            if (m) {
                // Read-only text boxes: every line can be selected and copied (Ctrl+C); the site also has its own buttons.
                const std::string info = FormatInfo(app.texts, *m);
                std::vector<char> buf(info.begin(), info.end());
                buf.push_back('\0');
                const bool ok = m->umListed && m->umFile;
                ImGui::TextColored(ok ? ImVec4(0.43f, 0.82f, 0.47f, 1) : (m->umFile || m->umListed) ? ImVec4(0.92f, 0.7f, 0.27f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", ok ? "um.dll detected" : m->umFile ? "um.dll in the folder, not enabled (config.reg)" : m->umListed ? "um.dll listed, but the file is missing" : "no um.dll");
                // Updates: the release and the beta live in their own folders (their saves never mix); each has its own button.
                const bool umFamily = LowerAscii(m->folder).compare(0, 13, "universal-mod") == 0 || m->title == "Universal Mod";
                const float below = ImGui::GetFrameHeightWithSpacing() * (umFamily ? 6 : 2) + 12;
                ImGui::InputTextMultiline("##infotext", buf.data(), buf.size(), ImVec2(-1, -below), ImGuiInputTextFlags_ReadOnly);
                if (umFamily) {
                    using S = update::Updater::State;
                    const bool busyAny = app.UpdateBusy();
                    const bool gameUp = app.phase == App::Phase::Starting || app.phase == App::Phase::Running;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Updates");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(busyAny);
                    if (ImGui::Button("Check for updates")) app.CheckUpdates();
                    ImGui::EndDisabled();
                    if (busyAny) { ImGui::SameLine(); if (ImGui::Button("Cancel")) for (App::Channel& c : app.channels) c.updater.Cancel(); }
                    for (App::Channel& c : app.channels) {
                        const auto snap = c.updater.Get();
                        const ModInfo* installed = app.FindMod(c.folder);
                        std::string status;
                        bool offer = false;
                        switch (snap.state) {
                        case S::Idle: status = installed ? "not checked" : "not checked"; break;
                        case S::Checking: status = "checking..."; break;
                        case S::Available: case S::UpToDate:
                            offer = true;
                            {
                                std::string built = snap.latest.assetUpdated.size() >= 16 ? snap.latest.assetUpdated.substr(0, 10) + " " + snap.latest.assetUpdated.substr(11, 5) : std::string();
                                const std::string fresh = App::BuildLabel(snap.latest.tag, snap.latest.hash) + (snap.latest.hash.empty() && !built.empty() ? ", built " + built : std::string());
                                if (!installed) status = fresh;
                                else if (app.WorthInstalling(c, snap.latest)) status = App::BuildLabel(installed->version, app.InstalledHash(c)) + " -> " + fresh;
                                else status = "up to date";
                            }
                            break;
                        case S::Downloading: case S::Extracting: case S::Applying: case S::Done: case S::Failed: status = snap.message; break;
                        }
                        ImGui::PushID(c.folder);
                        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "%s", c.label);
                        ImGui::SameLine(125);
                        ImGui::BeginDisabled(busyAny || gameUp || !offer);
                        const bool worth = snap.state == S::Available || snap.state == S::UpToDate ? (!installed || app.WorthInstalling(c, snap.latest)) : false;
                        if (ImGui::Button(!installed ? "Install" : worth ? "Update" : "Reinstall", ImVec2(70, 0))) app.InstallChannel(c);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Into mods/%s. mp and saves are kept.", c.folder);
                        ImGui::SameLine();
                        if (status.size() > 40) status = status.substr(0, 39) + "...";
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(status.c_str());
                        if (snap.state == S::Downloading || snap.state == S::Extracting) {
                            char over[64] = "";
                            if (snap.state == S::Downloading) std::snprintf(over, sizeof over, "%.0f / %.0f MB", static_cast<double>(snap.done) / 1048576.0, static_cast<double>(snap.total) / 1048576.0);
                            else std::snprintf(over, sizeof over, "unpacking %d%%", static_cast<int>(snap.progress * 100));
                            ImGui::ProgressBar(static_cast<float>(snap.progress), ImVec2(-1, 14), over);
                        }
                        ImGui::PopID();
                    }
                }
                if (!m->site.empty()) {
                    if (ImGui::Button("Copy site")) ImGui::SetClipboardText(m->site.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Open site")) platform::OpenWithDefault(m->site);
                    ImGui::SameLine();
                }
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
                ImGui::SameLine(); ImGui::Button("EIStarter-UM.log") ? platform::OpenWithDefault((app.dir / "EIStarter-UM.log").string()) : (void)0;
                ImGui::SameLine(); if (ImGui::Button("Close")) app.phase = App::Phase::Idle;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // First start without Universal Mod: download it?
    if (app.offer != App::Offer::None && !app.options.IsOpen() && app.message.empty()) {
        using S = update::Updater::State;
        ImGui::SetNextWindowPos(ImVec2(kW / 2.f, kH / 2.f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(470, app.offer == App::Offer::Asking ? 236 : 150));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.04f, 0.07f, 0.96f));
        if (ImGui::Begin("##offer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            App::Channel& c = app.channels[app.offerChannel];
            if (app.offer == App::Offer::Asking) {
                ImGui::TextColored(ImVec4(0.92f, 0.7f, 0.27f, 1), "Universal Mod is not installed");
                ImGui::Spacing();
                ImGui::TextWrapped("Download it now from GitHub (Kyr4l/ei-universal-mod)? It is unpacked into the mods folder; later updates keep your saves. Which line?");
                ImGui::Spacing();
                if (ImGui::RadioButton("Release (master)", app.offerChannel == 0)) app.offerChannel = 0;
                ImGui::SameLine(190); ImGui::TextDisabled("the newest stable build (recommended)");
                if (ImGui::RadioButton("Pre-release (beta)", app.offerChannel == 1)) app.offerChannel = 1;
                ImGui::SameLine(190); ImGui::TextDisabled("new features first, may have bugs");
                ImGui::TextDisabled("Both can be installed side by side later (Info > Updates).");
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 190, ImGui::GetWindowHeight() - 34));
                if (ImGui::Button("Download", ImVec2(84, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) app.StartOffer();
                ImGui::SameLine();
                if (ImGui::Button("Not now", ImVec2(84, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { app.Log("no Universal Mod: download declined"); app.offer = App::Offer::None; }
            } else if (app.offer == App::Offer::Working) {
                const auto snap = c.updater.Get();
                ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "%s", c.label);
                const char* what = snap.state == S::Checking ? "Looking for the newest build..." : snap.state == S::Downloading ? "Downloading..." : snap.state == S::Extracting ? "Unpacking..." : snap.state == S::Applying ? "Putting the files in place..." : "Starting...";
                ImGui::TextUnformatted(what);
                char over[64] = "";
                if (snap.state == S::Downloading) std::snprintf(over, sizeof over, "%.0f / %.0f MB", static_cast<double>(snap.done) / 1048576.0, static_cast<double>(snap.total) / 1048576.0);
                else if (snap.state == S::Extracting) std::snprintf(over, sizeof over, "%d%%", static_cast<int>(snap.progress * 100));
                const bool known = snap.state == S::Downloading || snap.state == S::Extracting;
                ImGui::ProgressBar(known ? static_cast<float>(snap.progress) : -1.0f * static_cast<float>(glfwGetTime()), ImVec2(-1, 16), over);
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 94, ImGui::GetWindowHeight() - 34));
                ImGui::BeginDisabled(snap.state == S::Applying);
                if (ImGui::Button("Cancel", ImVec2(84, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { c.updater.Cancel(); app.Log("no Universal Mod: download cancelled"); app.offer = App::Offer::None; }
                ImGui::EndDisabled();
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The download did not work");
                ImGui::TextWrapped("%s", app.offerError.c_str());
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 190, ImGui::GetWindowHeight() - 34));
                if (ImGui::Button("Try again", ImVec2(84, 0))) app.StartOffer();
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(84, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) app.offer = App::Offer::None;
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
    app.Log(std::string("EIStarter-UM ") + kVersion + " in " + app.dir.string());

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
    { GLFWimage icons[3] = {{64, 64, const_cast<unsigned char*>(logo::kIcon64)}, {48, 48, const_cast<unsigned char*>(logo::kIcon48)}, {32, 32, const_cast<unsigned char*>(logo::kIcon32)}}; glfwSetWindowIcon(window, 3, icons); }
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
    if (app.cfg.firstRun && !app.UmDetected()) { app.offer = App::Offer::Asking; app.Log("first start without Universal Mod: offering the download"); }
    if (app.cfg.checkUpdatesAtStart && app.offer == App::Offer::None) app.CheckUpdates(); // in the background; the corner shows the result
    if (NormalizePaths(app.cfg.game)) { app.Log("path settings repaired"); app.Save(); }
    if (!app.GameFolderOk()) { // nothing found: ask for the game folder right away instead of at the first Play
        app.Log("Evil Islands folder not found: asking");
        app.options.Open(app.cfg, true, "Evil Islands not found. Choose its folder, then OK.");
    }
    glfwShowWindow(window);
    glfwFocusWindow(window); // so that Enter reaches the launcher at once (Play is the default button)
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
