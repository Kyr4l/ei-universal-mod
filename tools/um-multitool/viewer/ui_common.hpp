// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Small UI helpers shared by the tabs: native file/folder pickers and a status dot.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // NavMovedHere: the nav state

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace ui {

// The item just submitted got the keyboard/gamepad focus this frame (Up/Down in a list). A list that calls it
// after each Selectable selects as the arrows move, as the item lists do, instead of needing Enter.
inline bool NavMovedHere() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    return ImGui::IsItemFocused() && g.NavJustMovedToId != 0 && g.NavJustMovedToId == g.LastItemData.ID;
}

// Whether a background picture is shown this frame (gui_main.cpp): the panels that paint the plain
// background then let it show through.
inline bool& BackgroundShown() { static bool shown = false; return shown; }
inline ImVec4 PanelBg() {
    ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    if (BackgroundShown()) c.w = 0.0f;
    return c;
}

// Neither GLFW nor Dear ImGui has a file dialog: use the Windows one, or zenity /
// kdialog on Linux (no GTK/Qt build dependency).
#ifndef _WIN32
inline bool RunPicker(const std::string& command, std::string& out) {
    FILE* p = popen(command.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    std::string result;
    while (fgets(buf, sizeof(buf), p)) result += buf;
    if (pclose(p) != 0) return false;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (result.empty()) return false;
    out = result;
    return true;
}
inline bool HasCommand(const char* name) {
    return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
}
#endif

inline bool PickFile(std::string& out) {
#ifdef _WIN32
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) { out = buf; return true; }
    return false;
#else
    if (HasCommand("zenity")) return RunPicker("zenity --file-selection 2>/dev/null", out);
    if (HasCommand("kdialog")) return RunPicker("kdialog --getopenfilename 2>/dev/null", out);
    return false;
#endif
}

// "Save as" dialog; `suggested` is the path offered first, `extension` e.g. "gif" or "png".
inline bool PickSaveFile(const std::string& suggested, std::string& out, const char* extension = "gif") {
#ifdef _WIN32
    char buf[MAX_PATH] = "";
    std::snprintf(buf, sizeof(buf), "%s", suggested.c_str());
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    std::string filter = std::string(extension) + " image";
    filter += '\0';
    filter += std::string("*.") + extension;
    filter += '\0';
    filter += "All files";
    filter += '\0';
    filter += "*.*";
    filter += '\0';
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    for (char* c = buf; *c; ++c) if (*c == '/') *c = '\\'; // Windows refuses to open with a '/' in the offered name (Wine does not)
    if (GetSaveFileNameA(&ofn)) { out = buf; return true; }
    if (CommDlgExtendedError() == FNERR_INVALIDFILENAME) { // the offered name or its folder is not valid: open empty instead
        buf[0] = '\0';
        if (GetSaveFileNameA(&ofn)) { out = buf; return true; }
    }
    return false;
#else
    std::string quoted = "'";
    for (char c : suggested) quoted += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    if (HasCommand("zenity"))
        return RunPicker("zenity --file-selection --save --confirm-overwrite --file-filter='*." + std::string(extension) +
                         "' --filename=" + quoted + " 2>/dev/null", out);
    if (HasCommand("kdialog")) return RunPicker("kdialog --getsavefilename " + quoted + " 2>/dev/null", out);
    return false;
#endif
}

inline bool PickFolder(std::string& out) {
#ifdef _WIN32
    char name[MAX_PATH] = "";
    BROWSEINFOA bi{};
    bi.pszDisplayName = name;
    bi.lpszTitle = "Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    if (ok) { out = path; return true; }
    return false;
#else
    if (HasCommand("zenity")) return RunPicker("zenity --file-selection --directory 2>/dev/null", out);
    if (HasCommand("kdialog")) return RunPicker("kdialog --getexistingdirectory 2>/dev/null", out);
    return false;
#endif
}

// A green filled / red hollow circle with a tooltip. Drawn, not a text glyph: the built-in font has no circles.
inline void StatusDot(bool ok, const std::string& okTip, const std::string& failTip) {
    float h = ImGui::GetTextLineHeight();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    ImVec2 center(pos.x + h * 0.5f, pos.y + h * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (ok) draw->AddCircleFilled(center, h * 0.3f, IM_COL32(80, 215, 80, 255));
    else draw->AddCircle(center, h * 0.3f, IM_COL32(230, 80, 80, 255), 0, 1.5f);
    const std::string& tip = ok ? okTip : failTip;
    if (ImGui::IsItemHovered() && !tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
}

inline void Note(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// Label on the left, value on the right column - for the item details.
inline void Row(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value.c_str());
}

inline std::string Num(float v, int decimals = 2) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

// A side panel beside a 3D view, on the left or the right, with a draggable bar between them.
// Call Begin, draw the panel or the view as it says (in order), and End.
struct SplitLayout {
    static constexpr float kBar = 6.0f;
    float* width;
    bool panelRight;
    float height = 0, total = 0;

    // Returns the widths to give BeginChild: the panel's and the view's (0 = the rest).
    void Begin(float& panelWidth, float& viewWidth) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        total = avail.x;
        height = avail.y;
        *width = std::max(260.0f, std::min(*width, std::max(260.0f, total - 240.0f)));
        panelWidth = *width;
        viewWidth = panelRight ? std::max(1.0f, total - *width - kBar) : 0.0f;
    }
    // Between the two children.
    void Bar() {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##splitbar", ImVec2(kBar, std::max(height, 1.0f)));
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (active) *width += (panelRight ? -1.0f : 1.0f) * ImGui::GetIO().MouseDelta.x;
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, a.y), ImVec2(b.x - 2, b.y),
                                                  ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_Separator));
        ImGui::SameLine(0.0f, 0.0f);
    }
};

} // namespace ui
