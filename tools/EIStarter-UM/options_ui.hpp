// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The Options window: everything EIStarter's Options dialog offered (Graphics, Addon, Multiplayer, Camera, Paths, Utils), plus the window mode
// (fullscreen / windowed / borderless windowed) and the launcher's own options. It edits a copy of the configuration; OK keeps it.
#pragma once

#include <imgui.h>

#include <string>
#include <vector>

#include "config.hpp"
#include "lang.hpp"
#include "platform.hpp"
#include "regsync.hpp"

namespace launcher {

class OptionsWindow {
public:
    // Opens the window on a copy of `cfg`; `pathsTab`: start on the Paths tab (the game folder is missing).
    void Open(const LauncherConfig& cfg, bool pathsTab, const std::string& hint, const std::string& modCfg = {}) {
        edit_ = cfg;
        modMsaa_ = !modCfg.empty() && LowerAscii(GetCfgLine(modCfg, "MSAA")) == "true";
        modBorderless_ = !modCfg.empty() && !GetCfgLine(modCfg, "BORDERLESS").empty();
        open_ = true; requestTab_ = pathsTab ? 4 : 0; hint_ = hint;
        adapters_ = platform::Adapters();
        resolutions_ = platform::Resolutions();
        const std::string cur = edit_.game.GetString("general settings", "videoresolution");
        bool have = cur.empty();
        for (const std::string& r : resolutions_) have = have || r == cur;
        if (!have) resolutions_.insert(resolutions_.begin(), cur);
    }
    bool IsOpen() const { return open_; }

    enum class Result { None, Ok, Cancel };
    // Draws the window (inside an ImGui frame); on OK `out` receives the edited configuration.
    Result Draw(const Texts& tx, LauncherConfig& out, ImVec2 area) {
        if (!open_) return Result::None;
        Result result = Result::None;
        const ImVec2 size(std::min(area.x - 12, 628.0f), std::min(area.y - 8, 352.0f));
        ImGui::SetNextWindowPos(ImVec2(area.x * 0.5f, area.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size);
        const std::string title = tx.Get("Options", "Options", "Options") + "###options";
        if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            if (!hint_.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.75f, 0.3f, 1)); ImGui::TextWrapped("%s", hint_.c_str()); ImGui::PopStyleColor(); }
            const float footer = ImGui::GetFrameHeightWithSpacing() + 4;
            if (ImGui::BeginTabBar("##tabs")) {
                Tab(tx, "tabGraphics", "Graphics", 0, footer, [&] { Graphics(tx); });
                Tab(tx, "tabAddon", "Addon", 1, footer, [&] { Addon(tx); });
                Tab(tx, "tabMultiplayer", "Multiplayer", 2, footer, [&] { Multiplayer(tx); });
                Tab(tx, "tabCamera", "Camera", 3, footer, [&] { Camera(tx); });
                Tab(tx, "tabPaths", "Paths", 4, footer, [&] { Paths(tx); });
                Tab(tx, "tabUtils", "Utils", 5, footer, [&] { Utils(tx); });
                ImGui::EndTabBar();
            }
            requestTab_ = -1;
            const std::string ok = tx.Get("Options", "btOk", "OK"), cancel = tx.Get("Options", "btCancel", "Cancel");
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 190);
            if (ImGui::Button(ok.c_str(), ImVec2(86, 0))) { out = edit_; open_ = false; result = Result::Ok; }
            ImGui::SameLine();
            if (ImGui::Button(cancel.c_str(), ImVec2(86, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { open_ = false; result = Result::Cancel; }
        }
        ImGui::End();
        return result;
    }

private:
    template <class F> void Tab(const Texts& tx, const char* key, const char* english, int index, float footer, F body) {
        const std::string label = tx.Get("Options", key, english) + "###tab" + std::to_string(index);
        if (ImGui::BeginTabItem(label.c_str(), nullptr, requestTab_ == index ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (ImGui::BeginChild("##body", ImVec2(0, -footer), ImGuiChildFlags_None)) body();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    // Labelled combo over a list of strings (translated by cbb<name>__<index> keys when `tx` has them).
    bool Combo(const Texts& tx, const char* label, const char* labelKey, const char* itemKeyPrefix, const std::vector<std::string>& english, int& index, bool bestFirst = false) {
        const std::string text = tx.Get("Options", labelKey, label);
        ImGui::AlignTextToFramePadding();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::TextUnformatted(text.c_str());
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(x0 + labelWidth_);
        ImGui::SetNextItemWidth(comboWidth_);
        if (index < 0 || index >= static_cast<int>(english.size())) index = 0;
        const std::string cur = tx.Get("Options", (std::string(itemKeyPrefix) + "__" + std::to_string(index)).c_str(), english[static_cast<size_t>(index)].c_str());
        bool changed = false;
        if (ImGui::BeginCombo((std::string("##") + labelKey).c_str(), cur.c_str())) {
            for (int k = 0; k < static_cast<int>(english.size()); ++k) {
                const int i = bestFirst ? static_cast<int>(english.size()) - 1 - k : k; // the stored values run from worst to best: shown best first like the other quality lists
                const std::string item = tx.Get("Options", (std::string(itemKeyPrefix) + "__" + std::to_string(i)).c_str(), english[static_cast<size_t>(i)].c_str());
                if (ImGui::Selectable(item.c_str(), i == index)) { index = i; changed = true; }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    bool Check(const Texts& tx, const char* key, const char* english, bool& v) { return ImGui::Checkbox((tx.Get("Options", key, english) + "##" + key).c_str(), &v); }

    void Graphics(const Texts& tx) {
        GameSettings& g = edit_.game;
        labelWidth_ = 110; comboWidth_ = 230;
        // Adapter and resolution.
        {
            std::vector<std::string> ad{"Primary video driver"};
            for (const std::string& a : adapters_) ad.push_back(a);
            int i = g.GetInt("general settings", "drawingtarget", 0);
            if (Combo(tx, "Adapter", "lbAdapter", "cbbAdapter", ad, i)) g.SetInt("general settings", "drawingtarget", i);
            std::string cur = g.GetString("general settings", "videoresolution");
            int ri = 0;
            for (size_t k = 0; k < resolutions_.size(); ++k) if (resolutions_[k] == cur) ri = static_cast<int>(k);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tx.Get("Options", "lbResolution", "Resolution").c_str());
            ImGui::SameLine(150); ImGui::SetNextItemWidth(170);
            if (ImGui::BeginCombo("##res", cur.c_str())) {
                for (size_t k = 0; k < resolutions_.size(); ++k) if (ImGui::Selectable(resolutions_[k].c_str(), static_cast<int>(k) == ri)) g.SetString("general settings", "videoresolution", resolutions_[k]);
                ImGui::EndCombo();
            }
        }
        ImGui::Separator();
        labelWidth_ = 100; comboWidth_ = 125;
        if (ImGui::BeginTable("##gfx", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        // Window mode (the original had a FullScreen checkbox; borderless windowed is new).
        {
            int mode = g.GetBool("general settings", "fullscreen") ? 0 : (edit_.borderless ? (edit_.borderlessScale ? 3 : 2) : 1);
            if (Combo(tx, "Window mode", "lbWindowMode", "cbbWindowMode", {"Fullscreen", "Windowed", "Borderless window", "Borderless scaled"}, mode)) {
                g.SetBool("general settings", "fullscreen", mode == 0);
                edit_.borderless = mode >= 2;
                edit_.borderlessScale = mode == 3;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(edit_.borderlessScale ? "Borderless scaled: the window covers the screen and a lower game resolution is scaled up to it (black bars keep the aspect ratio).\nSet the game resolution below your screen's in Video." : modBorderless_ ? "Keeps running when unfocused; the mouse stays inside.\nWritten to the mod's um.cfg at every Play." : "Keeps running when unfocused; the mouse stays inside.");
        }
        bool b;
        b = g.GetBool("general settings", "mipmapping"); if (Check(tx, "cbMipMapping", "Mip-Mapping", b)) g.SetBool("general settings", "mipmapping", b);
        b = g.GetBool("general settings", "antialiasing"); if (Check(tx, "cbAntialiasing", "Antialiasing", b)) g.SetBool("general settings", "antialiasing", b);
        if (modMsaa_ && ImGui::IsItemHovered()) ImGui::SetTooltip("The mod's um.cfg has MSAA=true. Keep this off.");
        b = g.GetBool("general settings", "dithering"); if (Check(tx, "cbDithering", "Dithering", b)) g.SetBool("general settings", "dithering", b);
        b = g.GetBool("general settings", "FPSIndependentCursor"); if (Check(tx, "cbFPSCursor", "FPS-independent cursor (not recommended)", b)) g.SetBool("general settings", "FPSIndependentCursor", b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flickers with D7VK.");
        int i;
        i = g.GetInt("general settings", "video"); if (Combo(tx, "Movies", "lbMovies", "cbbMovies", {"Off", "1:1", "Fit"}, i)) g.SetInt("general settings", "video", i);
        i = g.GetInt("general settings", "audio"); if (Combo(tx, "Audio", "lbAudio", "cbbAudio", {"Off", "Normal", "Safe"}, i)) g.SetInt("general settings", "audio", i);
        ImGui::TableNextColumn();
        i = g.GetInt("general settings", "filtering"); if (Combo(tx, "Filtering", "lbFiltering", "cbbFiltering", {"Point", "Bilinear", "Trilinear"}, i, true)) g.SetInt("general settings", "filtering", i);
        i = g.GetInt("general settings", "TextureQuality"); if (Combo(tx, "Texture quality", "lbTexturesQ", "cbbTextures", {"High", "Normal", "Low"}, i)) g.SetInt("general settings", "TextureQuality", i);
        i = g.GetInt("shadow settings", "shadowquality"); if (Combo(tx, "Shadow quality", "lbShadowsQ", "cbbShadows", {"None", "Low", "Smooth"}, i, true)) g.SetInt("shadow settings", "shadowquality", i);
        // Terrain LOD: a preset of three values (the original's rule).
        {
            const bool waves = g.GetBool("terrain settings", "EnableWaterWaves");
            int t = waves ? 0 : ((g.GetFloat("terrain settings", "LOD1") == 90.f && g.GetFloat("terrain settings", "LOD2") == 130.f) ? 1 : 2);
            if (Combo(tx, "Terrain LOD", "lbTerrainQ", "cbbTerrain", {"High", "Normal", "Low"}, t)) {
                if (t < 2) { g.SetFloat("terrain settings", "LOD1", 90.f); g.SetFloat("terrain settings", "LOD2", 130.f); g.SetBool("terrain settings", "EnableWaterWaves", t == 0); }
                else { g.SetFloat("terrain settings", "LOD1", 55.f); g.SetFloat("terrain settings", "LOD2", 90.f); g.SetBool("terrain settings", "EnableWaterWaves", false); }
            }
        }
        Freq(tx, "Lighting freq.", "lbLightingFreq", "cbbLighting", "LIGHTINGFREQ");
        Freq(tx, "Shadowing freq.", "lbShadowingFreq", "cbbShadowing", "SHADOWINGFREQ");
        ImGui::EndTable();
        }
        labelWidth_ = 150; comboWidth_ = 170;
    }
    void Freq(const Texts& tx, const char* label, const char* key, const char* items, const char* name) {
        GameSettings& g = edit_.game;
        const int v = g.GetInt("terrain settings", name);
        int idx = v == 1000 ? 0 : (v == 10000 ? 1 : 2);
        if (Combo(tx, label, key, items, {"Fast", "Normal", "Rare"}, idx)) g.SetInt("terrain settings", name, idx == 0 ? 1000 : (idx == 1 ? 10000 : 100000));
    }

    void Addon(const Texts& tx) {
        AddonOptions& a = edit_.addon;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tx.Get("Options", "lbDrawRadiusTerrain", "Landscape draw radius").c_str());
        ImGui::SameLine(190); ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##lr", &a.landscapeDrawRadius, 10, 100); a.landscapeDrawRadius = std::max(0, std::min(5000, a.landscapeDrawRadius));
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tx.Get("Options", "lbDrawRadiusObjects", "Objects draw radius").c_str());
        ImGui::SameLine(190); ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##or", &a.objectsDrawRadius, 10, 100); a.objectsDrawRadius = std::max(0, std::min(5000, a.objectsDrawRadius));
        ImGui::TextDisabled("0 = game default (10 - 5000)");
        Check(tx, "cbOnMinimizePause", "Pause the game when minimized", a.pauseOnMinimize);
        ImGui::Separator();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("addon.log level");
        ImGui::SameLine(190); ImGui::SetNextItemWidth(110); ImGui::InputInt("##ll", &a.logLevel, 1, 1);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Console log level");
        ImGui::SameLine(190); ImGui::SetNextItemWidth(110); ImGui::InputInt("##cl", &a.consoleLogLevel, 1, 1);
        a.logLevel = std::max(0, std::min(9, a.logLevel)); a.consoleLogLevel = std::max(0, std::min(9, a.consoleLogLevel));
    }
    void Multiplayer(const Texts& tx) {
        AddonOptions& a = edit_.addon;
        Check(tx, "cbMPConsoleSupport", "Console support", a.mpConsole);
        Check(tx, "cbMPDisableMQ", "Do not load .MQ (quest) files from the host", a.mqClient);
        Check(tx, "cbMPSecondSpeed", "Second speed support", a.mpSpeedControl);
    }

    void Float(const char* label, const char* name, float speed = 0.1f) {
        float v = edit_.game.GetFloat("camera settings", name);
        ImGui::SetNextItemWidth(110);
        if (ImGui::DragFloat((std::string(label) + "##" + name).c_str(), &v, speed)) edit_.game.SetFloat("camera settings", name, v);
    }
    void Int(const char* label, const char* name) {
        int v = edit_.game.GetInt("camera settings", name);
        ImGui::SetNextItemWidth(110);
        if (ImGui::DragInt((std::string(label) + "##" + name).c_str(), &v)) edit_.game.SetInt("camera settings", name, v);
    }
    void Flag(const char* label, const char* name) {
        bool v = edit_.game.GetBool("camera settings", name);
        if (ImGui::Checkbox((std::string(label) + "##" + name).c_str(), &v)) edit_.game.SetBool("camera settings", name, v);
    }
    // A min / max (/ default) group of camera values.
    void Range(const char* title, const char* fmt, bool withDefault) {
        if (!ImGui::TreeNode(title)) return;
        char n[96];
        std::snprintf(n, sizeof n, fmt, "Min"); Float("Min", n);
        std::snprintf(n, sizeof n, fmt, "Max"); Float("Max", n);
        if (withDefault) { std::snprintf(n, sizeof n, fmt, "Default"); Float("Default", n); }
        ImGui::TreePop();
    }
    void Move(const char* title, const char* fmt) {
        if (!ImGui::TreeNode(title)) return;
        char n[96];
        std::snprintf(n, sizeof n, fmt, "Speed"); Float("Speed", n, 0.01f);
        std::snprintf(n, sizeof n, fmt, "PowerMouse"); Float("Power: mouse", n, 0.01f);
        std::snprintf(n, sizeof n, fmt, "PowerKeyboard"); Float("Power: keyboard", n, 0.01f);
        ImGui::TreePop();
    }
    void Camera(const Texts& tx) {
        // The original CD release's camera (GOG's differs and is hard to use); the registry and the backup file are handled when OK is pressed (main.cpp).
        if (ImGui::Button((tx.Get("Options", "btCameraOriginal", "Original camera") + "##camOriginal").c_str())) { edit_.game.RestoreOriginalCamera(); edit_.cameraRestored = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tx.Get("Options", "tipCameraOriginal", "Replaces all camera settings with the values of the original CD release of the game (the GOG release changes them).\nThe old values are kept in EIStarter-UM-camera-backup.txt.").c_str());
        ImGui::SameLine();
        const size_t differ = edit_.game.CameraDifferences().size();
        if (differ == 0) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "%s", tx.Get("Options", "camSame", "Same as the original.").c_str());
        else {
            char t[160]; std::snprintf(t, sizeof t, tx.Get("Options", "camCount", "%d values differ.").c_str(), static_cast<int>(differ)); ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", t);
            if (ImGui::IsItemHovered()) { // which ones: yours -> original
                std::string list;
                for (const GameSettings::CameraDiff& d : edit_.game.CameraDifferenceDetails()) list += d.name + ": " + d.mine + " -> " + d.original + "\n";
                ImGui::SetTooltip("%s", list.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
            Flag("Flip X axis", "CameraFlipXAxis"); Flag("Flip Y axis", "CameraFlipYAxis");
            Float("Clip distance: far", "FarClipDistance", 1.0f); Float("Clip distance: near", "NearClipDistance", 0.1f);
            Float("Terrain sensitivity", "CameraTerrainSensetiveArea"); Int("Border scroll", "CameraBorderScrollArea"); Int("Frame selection sensitivity", "CameraFrameSelectionSensetiveArea");
            Float("Deploy X", "CameraDefaultXDeploy"); Float("Deploy Y", "CameraDefaultYDeploy");
            Move("Pitch", "CameraDefaultPitch%s"); Move("Rotate", "CameraDefaultRotate%s"); Move("Scroll", "CameraDefaultScroll%s"); Move("Zoom", "CameraDefaultZoom%s");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Camera")) {
            Range("Height", "Camera%sHeight", true); Range("Distance to carrier", "Camera%sDistanceToCarrier", true);
            Range("Distance to carrier limit", "Camera%sLimitDistanceToCarrier", false); Range("Pitch", "CameraDefault%sPitch", false);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Briefing camera")) {
            Range("Distance to carrier", "CameraBriefing%sDistanceToCarrier", false); Range("Distance to carrier limit", "CameraBriefing%sLimitDistanceToCarrier", false);
            Range("Pitch", "CameraBriefing%sPitch", false);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Shortcuts")) {
            for (int s = 0; s < 4; ++s) {
                if (!ImGui::TreeNode(("Shortcut " + std::to_string(s)).c_str())) continue;
                for (const char* f : {"Distance", "AxisX", "AxisY", "AxisZ", "Angle", "Pitch"}) Float(f, ("CameraShortcut " + std::to_string(s) + " " + f).c_str());
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }

    void PathRow(const Texts& tx, const char* label, const char* key, const char* name) {
        std::string v = edit_.game.GetString("Path Settings", name);
        char buf[600]; std::snprintf(buf, sizeof buf, "%s", v.c_str());
        ImGui::TextUnformatted(tx.Get("Options", key, label).c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40);
        if (ImGui::InputText((std::string("##") + name).c_str(), buf, sizeof buf)) edit_.game.SetString("Path Settings", name, buf);
        ImGui::SameLine();
        if (ImGui::Button((std::string("...##") + name).c_str(), ImVec2(30, 0))) { std::string p = v; if (platform::PickFolder(p)) edit_.game.SetString("Path Settings", name, WithSlash(p)); }
    }
    void Paths(const Texts& tx) {
        PathRow(tx, "Game folder", "lbPathGameWorkdir", "Work Path");
        const bool ok = LooksLikeGameFolder(edit_.game.GetString("Path Settings", "Work Path"));
        if (ok) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "Game found"); else ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Not an Evil Islands folder");
        PathRow(tx, "Game resources folder", "lbPathRes", "Resources Path");
        PathRow(tx, "CD folder", "lbPathCD", "CD-ROM Path");
        char buf[200]; std::snprintf(buf, sizeof buf, "%s", edit_.exeName.c_str());
        ImGui::TextUnformatted(tx.Get("Options", "lbEXEName", "Game engine file name").c_str());
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##exe", buf, sizeof buf)) edit_.exeName = buf;
        ImGui::SameLine(); ImGui::TextDisabled("in Engine");
        if (ImGui::SmallButton("Resources and CD = game folder")) {
            const std::string w = WithSlash(edit_.game.GetString("Path Settings", "Work Path"));
            edit_.game.SetString("Path Settings", "Resources Path", w + "Res" + std::string(w.find('\\') != std::string::npos ? "\\" : "/"));
            edit_.game.SetString("Path Settings", "CD-ROM Path", w);
        }
    }
    void Utils(const Texts& tx) {
        (void)tx;
        ImGui::Checkbox("Mod skins", &edit_.modSkins);
        ImGui::Checkbox("Read mod.config (INI) instead of config.reg", &edit_.usingIniConfigs);
        ImGui::Checkbox("Close when the game is up", &edit_.closeOnStart);
        ImGui::Checkbox("Debug log", &edit_.debugLog);
        ImGui::Checkbox("Check for mod updates at start", &edit_.checkUpdatesAtStart);
    }

    bool modMsaa_ = false, modBorderless_ = false;
    float labelWidth_ = 150, comboWidth_ = 170;
    LauncherConfig edit_;
    bool open_ = false;
    int requestTab_ = -1;
    std::string hint_;
    std::vector<std::string> adapters_, resolutions_;
};

} // namespace launcher
