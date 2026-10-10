// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The GUI's colours: a theme is an accent, a background and a text colour; every Dear ImGui colour is derived
// from them. The presets are the splash screen's palettes (charcoal and gold is the splash's own and the
// default); Settings > General picks one or sets the accent by hand (THEME, THEME_ACCENT in the config).
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"

namespace theme {

struct Rgb { int r, g, b; };
struct Preset { const char* key; const char* name; Rgb accent, bg, text; };

// "default" keeps Dear ImGui's own dark look.
static const Preset kPresets[] = {
    {"default", "Dear ImGui dark", {66, 150, 250}, {15, 15, 15}, {255, 255, 255}},
    {"charcoal", "Charcoal and gold", {236, 200, 130}, {24, 24, 26}, {240, 226, 190}},
    {"navy", "Navy", {120, 190, 255}, {16, 22, 36}, {230, 236, 248}},
    {"slate", "Slate and cyan", {120, 230, 245}, {22, 30, 38}, {210, 236, 244}},
    {"burgundy", "Burgundy", {255, 200, 150}, {34, 16, 22}, {250, 232, 222}},
    {"bronze", "Bronze", {255, 210, 140}, {28, 22, 18}, {248, 232, 200}},
    {"teal", "Teal", {130, 245, 230}, {10, 28, 30}, {220, 248, 244}},
};
static const int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

inline const Preset* Find(const std::string& key) {
    for (const Preset& p : kPresets) if (key == p.key) return &p;
    return nullptr;
}

inline std::string AccentText(const Rgb& c) { char b[32]; std::snprintf(b, sizeof b, "%d,%d,%d", c.r, c.g, c.b); return b; }
inline bool ParseAccent(const std::string& s, Rgb& out) {
    int r = 0, g = 0, b = 0;
    if (std::sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return false;
    auto clamp = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
    out = {clamp(r), clamp(g), clamp(b)};
    return true;
}

inline ImVec4 Mix(const Rgb& a, const Rgb& b, float t, float alpha = 1.0f) {
    return ImVec4((a.r + (b.r - a.r) * t) / 255.0f, (a.g + (b.g - a.g) * t) / 255.0f, (a.b + (b.b - a.b) * t) / 255.0f, alpha);
}

// Sets every ImGui colour from the three of the theme. `accent` may differ from the preset's (set by hand).
inline void Apply(const std::string& presetKey, const Rgb& accent) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    if (presetKey == "default" && accent.r == 66 && accent.g == 150 && accent.b == 250) return;
    const Preset* p = Find(presetKey);
    const Rgb bg = p ? p->bg : Rgb{24, 24, 26}, text = p ? p->text : Rgb{240, 240, 240};
    const Rgb a = accent;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Mix(text, text, 0);
    c[ImGuiCol_TextDisabled] = Mix(text, bg, 0.45f);
    c[ImGuiCol_WindowBg] = Mix(bg, bg, 0);
    c[ImGuiCol_ChildBg] = Mix(bg, a, 0.02f, 0.0f); // transparent, as in the default look: the 3D views draw under a child window
    c[ImGuiCol_PopupBg] = Mix(bg, a, 0.04f, 0.98f);
    c[ImGuiCol_Border] = Mix(bg, a, 0.25f, 0.6f);
    c[ImGuiCol_FrameBg] = Mix(bg, a, 0.10f);
    c[ImGuiCol_FrameBgHovered] = Mix(bg, a, 0.22f);
    c[ImGuiCol_FrameBgActive] = Mix(bg, a, 0.32f);
    c[ImGuiCol_TitleBg] = Mix(bg, a, 0.04f);
    c[ImGuiCol_TitleBgActive] = Mix(bg, a, 0.16f);
    c[ImGuiCol_TitleBgCollapsed] = Mix(bg, a, 0.04f, 0.6f);
    c[ImGuiCol_MenuBarBg] = Mix(bg, a, 0.06f);
    c[ImGuiCol_ScrollbarBg] = Mix(bg, a, 0.02f);
    c[ImGuiCol_ScrollbarGrab] = Mix(bg, a, 0.22f);
    c[ImGuiCol_ScrollbarGrabHovered] = Mix(bg, a, 0.34f);
    c[ImGuiCol_ScrollbarGrabActive] = Mix(bg, a, 0.46f);
    c[ImGuiCol_CheckMark] = Mix(a, a, 0);
    c[ImGuiCol_SliderGrab] = Mix(bg, a, 0.75f);
    c[ImGuiCol_SliderGrabActive] = Mix(a, a, 0);
    c[ImGuiCol_Button] = Mix(bg, a, 0.22f);
    c[ImGuiCol_ButtonHovered] = Mix(bg, a, 0.40f);
    c[ImGuiCol_ButtonActive] = Mix(bg, a, 0.58f);
    c[ImGuiCol_Header] = Mix(bg, a, 0.22f);
    c[ImGuiCol_HeaderHovered] = Mix(bg, a, 0.38f);
    c[ImGuiCol_HeaderActive] = Mix(bg, a, 0.50f);
    c[ImGuiCol_Separator] = Mix(bg, a, 0.30f);
    c[ImGuiCol_SeparatorHovered] = Mix(bg, a, 0.55f);
    c[ImGuiCol_SeparatorActive] = Mix(a, a, 0);
    c[ImGuiCol_ResizeGrip] = Mix(bg, a, 0.30f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = Mix(bg, a, 0.60f, 0.8f);
    c[ImGuiCol_ResizeGripActive] = Mix(a, a, 0, 0.9f);
    c[ImGuiCol_Tab] = Mix(bg, a, 0.16f);
    c[ImGuiCol_TabHovered] = Mix(bg, a, 0.42f);
    c[ImGuiCol_TabActive] = Mix(bg, a, 0.32f);
    c[ImGuiCol_TabUnfocused] = Mix(bg, a, 0.08f);
    c[ImGuiCol_TabUnfocusedActive] = Mix(bg, a, 0.20f);
    c[ImGuiCol_PlotLines] = Mix(bg, a, 0.75f);
    c[ImGuiCol_PlotLinesHovered] = Mix(a, a, 0);
    c[ImGuiCol_PlotHistogram] = Mix(bg, a, 0.75f);
    c[ImGuiCol_PlotHistogramHovered] = Mix(a, a, 0);
    c[ImGuiCol_TableHeaderBg] = Mix(bg, a, 0.14f);
    c[ImGuiCol_TableBorderStrong] = Mix(bg, a, 0.30f);
    c[ImGuiCol_TableBorderLight] = Mix(bg, a, 0.14f);
    c[ImGuiCol_TableRowBgAlt] = Mix(bg, a, 0.04f, 0.5f);
    c[ImGuiCol_TextSelectedBg] = Mix(bg, a, 0.45f, 0.7f);
    c[ImGuiCol_DragDropTarget] = Mix(a, a, 0, 0.9f);
    c[ImGuiCol_NavHighlight] = Mix(a, a, 0);
    c[ImGuiCol_NavWindowingHighlight] = Mix(a, a, 0, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = Mix(bg, bg, 0, 0.6f);
    c[ImGuiCol_ModalWindowDimBg] = Mix(bg, bg, 0, 0.6f);
}

} // namespace theme
