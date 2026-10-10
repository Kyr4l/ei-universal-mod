// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Problem alerts shared by every tab: when a tab detects new errors or warnings (database checks, map
// and script checks, a File Processing job), it raises an alert here. An error opens a popup with a
// button to the tab that lists it; both can play a sound (sfx/, built into the binary):
//   errors   - sfx/ab-ap-dc.mp3
//   warnings - sfx/ab-athr.mp3
// Sounds are off by default (Settings, SFX_ENABLED and SFX_VOLUME in um-multitool.cfg); popups are on
// (ALERT_POPUPS).
#pragma once

#include <functional>
#include <string>

namespace alerts {

enum class Level { Warning, Error };

// `goTo` shows the problems (switches the main tab and whatever sub-tab lists them); the popup's button
// calls it. `place` names it on that button ("Map Editor > Checks").
void Raise(Level level, const std::string& message, const std::string& place = {}, std::function<void()> goTo = {});

void SetSound(bool enabled);
void SetPopups(bool enabled);
void SetVolume(int percent); // 0-100
void PlaySound(Level level); // ignores SetSound: for the Settings tab's test buttons

// Inside the ImGui frame (main window): draws the popup of the last error.
void Draw();

void Shutdown(); // closes the audio device

} // namespace alerts
