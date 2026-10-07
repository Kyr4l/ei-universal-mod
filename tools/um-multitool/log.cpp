#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>

#include "imgui.h"
#include "viewer/config.hpp"

namespace umlog {
namespace {

std::mutex g_mutex;
std::deque<std::string> g_lines;
bool g_verbose = false;
bool g_headerWritten = false;
constexpr size_t kKeep = 2000;

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

} // namespace

std::string FilePath() { return config::ExeDir() + "/um-multitool.log"; }

void Write(Level level, const std::string& text) {
    const char* tag = level == Level::Error ? "ERROR" : level == Level::Warning ? "WARN " : "info ";
    const std::string line = Stamp() + " " + tag + " " + text;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.push_back(line);
    while (g_lines.size() > kKeep) g_lines.pop_front();
    std::ofstream f(FilePath(), std::ios::app);
    if (f.is_open()) {
        if (!g_headerWritten) { f << "---- um-multitool started " << Stamp() << "\n"; g_headerWritten = true; }
        f << line << "\n";
    }
    if (g_verbose || level == Level::Error) std::fprintf(stderr, "%s\n", line.c_str());
}

void SetVerbose(bool on) { g_verbose = on; }
bool Verbose() { return g_verbose; }

std::vector<std::string> Lines() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::vector<std::string>(g_lines.begin(), g_lines.end());
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.clear();
}

void DrawWindow(bool* open) {
    if (!open || !*open) return;
    ImGui::SetNextWindowSize(ImVec2(760, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", open)) { ImGui::End(); return; }
    ImGui::TextDisabled("%s", FilePath().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (const std::string& l : Lines()) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) Clear();
    static bool errorsOnly = false;
    ImGui::SameLine();
    ImGui::Checkbox("Errors and warnings only", &errorsOnly);
    ImGui::BeginChild("##loglines", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<std::string> lines = Lines();
    for (const std::string& l : lines) {
        const bool err = l.find(" ERROR ") != std::string::npos, warn = l.find(" WARN  ") != std::string::npos;
        if (errorsOnly && !err && !warn) continue;
        if (err) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        else if (warn) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
        ImGui::TextUnformatted(l.c_str());
        if (err || warn) ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace umlog
