// Small UI helpers shared by the tabs: native file/folder pickers and a status dot.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace ui {

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

// "Save as" dialog; `suggested` is the path offered first.
inline bool PickSaveFile(const std::string& suggested, std::string& out) {
#ifdef _WIN32
    char buf[MAX_PATH] = "";
    std::snprintf(buf, sizeof(buf), "%s", suggested.c_str());
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrFilter = "GIF image\0*.gif\0All files\0*.*\0";
    ofn.lpstrDefExt = "gif";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameA(&ofn)) { out = buf; return true; }
    return false;
#else
    std::string quoted = "'";
    for (char c : suggested) quoted += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    if (HasCommand("zenity"))
        return RunPicker("zenity --file-selection --save --confirm-overwrite --filename=" + quoted + " 2>/dev/null", out);
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

} // namespace ui
