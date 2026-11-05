// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Tool windows that can leave the main window: popout::Begin / popout::End replace ImGui::Begin / ImGui::End for
// a titled window. Inside the main window it is an ordinary ImGui window with a "Pop out" button; popped out, the
// same code draws into an OS window of its own (a second GLFW window sharing the main window's GL objects, with
// its own ImGui context, like the Map Editor's script window), with a "Dock back" button. Closing that OS window
// closes the tool window (its open flag goes false).
// Works on every platform the tool runs on (Wayland and Windows XP included): no ImGui multi-viewport needed.
//
// The main loop calls BeginFrames() before its own ImGui::NewFrame() and EndFrames() after its buffer swap; a
// popped-out window drawn by code that only runs while its tab is shown shows a note while that tab is hidden.
#pragma once

#include <functional>

#include "imgui.h"

struct GLFWwindow;

namespace popout {

struct Setup {
    GLFWwindow* mainWindow = nullptr;
    std::function<void(ImGuiIO&)> fonts;          // the fonts of a new context (the main window's fallback fonts)
    std::function<void(GLFWwindow*)> windowHints; // before glfwCreateWindow: app id / class hints
    const char* appName = "";                     // shown after the window's title
    std::function<bool()> separateByDefault;      // Settings: a window that opens pops out at once
};

void Init(const Setup& setup);
void BeginFrames();                 // before the main ImGui::NewFrame(): starts a frame in every popped-out window
void EndFrames();                   // after the main swap: draws the popped-out windows, opens / closes them as asked
void Shutdown();
bool Busy();                        // a popped-out window has the mouse or keyboard (the main loop's resting test)

// Instead of ImGui::Begin(title, open, flags); always pair with popout::End(), as with ImGui.
bool Begin(const char* title, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void End();

} // namespace popout
