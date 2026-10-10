// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The splash screen: the first thing on screen when the GUI starts. A small undecorated window with the banner
// (assets/splash.png, embedded; a splash.png beside the executable replaces it), a status line and a progress
// bar, shown while the sources, the editors and the database load; the main window stays hidden until End().
#pragma once

#include <string>

struct GLFWwindow;

namespace splash {

// Opens the splash next to the (hidden) main window. The accent colours the progress bar (the theme's). `overridePath`: a PNG that replaces the embedded banner
// when it exists. The main window's GL context is current again when this returns.
void Begin(GLFWwindow* mainWindow, const std::string& overridePath, int accentR = 70, int accentG = 120, int accentB = 200);
// Redraws the splash with a new status line and progress (0..1); polls the window system so it stays alive.
void Step(const std::string& text, float fraction);
// Keeps the splash up for at least a moment (a flash would look broken), then closes it. The caller shows
// the main window after this.
void End();

} // namespace splash
