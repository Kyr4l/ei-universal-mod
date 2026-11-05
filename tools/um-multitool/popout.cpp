// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Tool windows that can leave the main window. See popout.hpp.

#include "popout.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "i18n.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "imgui_internal.h" // GImGui->NextWindowData: a caller's SetNextWindow* for a popped-out window

namespace popout {

namespace {

struct Panel {
    std::string title;          // the ImGui title, "Comparison##db"
    bool* open = nullptr;       // the caller's open flag
    bool detached = false;
    GLFWwindow* win = nullptr;
    ImGuiContext* ctx = nullptr;
    bool inFrame = false;       // its context is between NewFrame and Render
    bool drawn = false;         // its code ran this frame
    bool drawnBefore = false;   // ... and the frame before (false: the window is opening)
    bool keptInside = false;    // docked back by hand: the default does not pop it out again until it closes
    bool wantOut = false, wantIn = false;
    ImVec2 size{640, 480}, pos{0, 0}; // the ImGui window's last size and place (the OS window starts there)
};

Setup g_setup;
std::map<std::string, Panel> g_panels;
std::vector<std::pair<Panel*, ImGuiContext*>> g_stack; // Begin -> End: the panel and the context to go back to
bool g_wayland = false;

std::string Shown(const std::string& title) { // the part of an ImGui title before ##
    const size_t p = title.find("##");
    return p == std::string::npos ? title : title.substr(0, p);
}

void Destroy(Panel& p) {
    if (!p.win) return;
    ImGuiContext* back = ImGui::GetCurrentContext();
    GLFWwindow* backGl = glfwGetCurrentContext();
    glfwMakeContextCurrent(p.win);
    ImGui::SetCurrentContext(p.ctx);
    if (p.inFrame) ImGui::EndFrame();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(p.ctx);
    ImGui::SetCurrentContext(back == p.ctx ? nullptr : back);
    glfwDestroyWindow(p.win);
    glfwMakeContextCurrent(backGl == p.win ? g_setup.mainWindow : backGl);
    p.win = nullptr; p.ctx = nullptr; p.inFrame = false; p.detached = false;
}

bool Create(Panel& p) {
    ImGuiContext* mainCtx = ImGui::GetCurrentContext();
    const ImGuiStyle style = ImGui::GetStyle();
    glfwDefaultWindowHints();
    if (g_setup.windowHints) g_setup.windowHints(nullptr);
    const std::string title = std::string(i18n::Tr(Shown(p.title).c_str())) + " - " + g_setup.appName;
    const int w = static_cast<int>(std::max(320.0f, p.size.x)), h = static_cast<int>(std::max(200.0f, p.size.y));
    p.win = glfwCreateWindow(w, h, title.c_str(), nullptr, g_setup.mainWindow);
    if (!p.win) return false;
    if (!g_wayland) { // where the ImGui window was, on the screen
        int mx = 0, my = 0;
        glfwGetWindowPos(g_setup.mainWindow, &mx, &my);
        glfwSetWindowPos(p.win, mx + static_cast<int>(p.pos.x), my + static_cast<int>(p.pos.y));
    }
    glfwMakeContextCurrent(p.win);
    glfwSwapInterval(0); // the main window's swap keeps the pace
    p.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(p.ctx);
    ImGui::GetStyle() = style;
    ImGui::GetIO().IniFilename = nullptr;
    if (g_setup.fonts) g_setup.fonts(ImGui::GetIO());
    ImGui_ImplGlfw_InitForOpenGL(p.win, true);
    ImGui_ImplOpenGL2_Init();
    ImGui::SetCurrentContext(mainCtx);
    glfwMakeContextCurrent(g_setup.mainWindow);
    p.detached = true;
    return true;
}

// The row on top of a window: its title (popped out) and the button that moves it.
bool MoveButton(const char* label, const char* tip) {
    const float w = ImGui::CalcTextSize(i18n::Tr(label)).x + ImGui::GetStyle().FramePadding.x * 2;
    const float x = ImGui::GetWindowContentRegionMax().x - w;
    const ImVec2 back = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(std::max(back.x, x), back.y));
    const bool pressed = ImGui::SmallButton(label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr(tip));
    ImGui::SetCursorPos(back);
    return pressed;
}

} // namespace

void Init(const Setup& setup) {
    g_setup = setup;
#if defined(GLFW_PLATFORM_WAYLAND)
    g_wayland = glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#endif
}

void BeginFrames() {
    ImGuiContext* mainCtx = ImGui::GetCurrentContext();
    const ImGuiStyle style = ImGui::GetStyle();
    for (auto& [key, p] : g_panels) {
        p.drawnBefore = p.drawn;
        p.drawn = false;
        if (p.open && !*p.open) p.keptInside = false; // closed: the next opening follows the setting again
        if (!p.detached || !p.ctx || p.inFrame) continue;
        glfwMakeContextCurrent(p.win);
        ImGui::SetCurrentContext(p.ctx);
        ImGui::GetStyle() = style; // follows theme changes
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        p.inFrame = true;
    }
    ImGui::SetCurrentContext(mainCtx);
    glfwMakeContextCurrent(g_setup.mainWindow);
}

bool Begin(const char* title, bool* open, ImGuiWindowFlags flags) {
    Panel& p = g_panels[title];
    if (p.title.empty()) p.title = title;
    p.open = open;
    p.drawn = true;
    if (p.detached && p.inFrame) {
        GImGui->NextWindowData.ClearFlags(); // the caller's SetNextWindowPos / Size were meant for the window inside the main one
        g_stack.push_back({&p, ImGui::GetCurrentContext()});
        ImGui::SetCurrentContext(p.ctx);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin(("##popout" + p.title).c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | (flags & ImGuiWindowFlags_MenuBar));
        if (MoveButton("Dock back", "Put this window back inside the main window")) { p.wantIn = true; p.keptInside = true; }
        return true;
    }
    if (!p.drawnBefore && !p.detached && !p.keptInside && g_setup.separateByDefault && g_setup.separateByDefault()) p.wantOut = true;
    g_stack.push_back({&p, nullptr});
    const bool shown = ImGui::Begin(title, open, flags);
    if (shown) {
        p.size = ImGui::GetWindowSize();
        p.pos = ImGui::GetWindowPos();
        if (MoveButton("Pop out", "Open this window in a separate window, outside the main one")) p.wantOut = true;
    }
    return shown;
}

void End() {
    if (g_stack.empty()) { ImGui::End(); return; }
    const auto [p, back] = g_stack.back();
    g_stack.pop_back();
    ImGui::End();
    if (back) ImGui::SetCurrentContext(back);
}

void EndFrames() {
    ImGuiContext* mainCtx = ImGui::GetCurrentContext();
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    for (auto& [key, p] : g_panels) {
        if (p.detached && p.inFrame) {
            glfwMakeContextCurrent(p.win);
            ImGui::SetCurrentContext(p.ctx);
            if (!p.drawn) { // its code did not run (its tab is hidden): say so instead of an empty window
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(vp->WorkPos);
                ImGui::SetNextWindowSize(vp->WorkSize);
                ImGui::Begin(("##popout" + p.title).c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
                if (MoveButton("Dock back", "Put this window back inside the main window")) { p.wantIn = true; p.keptInside = true; }
                ImGui::TextWrapped("%s", i18n::Tr("This window shows its content while its tab is open in the main window."));
                ImGui::End();
            }
            ImGui::Render();
            int w = 0, h = 0;
            glfwGetFramebufferSize(p.win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(p.win);
            p.inFrame = false;
            ImGui::SetCurrentContext(mainCtx);
            glfwMakeContextCurrent(g_setup.mainWindow);
            if (glfwWindowShouldClose(p.win)) { // the OS window's close button closes the tool window
                if (p.open) *p.open = false;
                Destroy(p);
                continue;
            }
        }
        if (p.detached && p.open && !*p.open) Destroy(p); // closed by its own code
        if (p.wantIn) { p.wantIn = false; Destroy(p); }
        if (p.wantOut) { p.wantOut = false; if (!p.detached) Create(p); }
    }
    ImGui::SetCurrentContext(mainCtx);
    glfwMakeContextCurrent(g_setup.mainWindow);
}

bool Busy() {
    for (auto& [key, p] : g_panels)
        if (p.detached && p.win && glfwGetWindowAttrib(p.win, GLFW_FOCUSED)) return true;
    return false;
}

void Shutdown() {
    for (auto& [key, p] : g_panels) Destroy(p);
    g_panels.clear();
}

} // namespace popout
