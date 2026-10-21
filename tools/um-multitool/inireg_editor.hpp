// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// File Processing > INI <-> REG: an editor for the game's INI files and its binary .reg files (a .reg is shown and edited as the INI text inireg.cpp
// makes of it, and saved back as a .reg), and the conversion of files and folders. Everything lives in inireg_editor.cpp.
#pragma once

#include <string>

namespace inireg_edit {

void DrawTab();                          // inside the INI <-> REG tab
bool OpenFile(const std::string& path);  // shows that file (gui --ini)

} // namespace inireg_edit
