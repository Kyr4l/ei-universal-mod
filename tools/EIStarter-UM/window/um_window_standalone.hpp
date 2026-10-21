// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// What um_window.cpp needs from um.dll's internals (um_internal.hpp), for the standalone um-window.asi: the windows headers, a log line
// written to um-window.log beside the plugin, and the import patch of um_hooks.cpp.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void LogLine(const char* level, const char* format, ...);
bool PatchImportedFunction(HMODULE module, const char* functionName, ULONG_PTR replacement, ULONG_PTR* original);
void InstallWindowHooks();
extern bool g_enableBorderless;
