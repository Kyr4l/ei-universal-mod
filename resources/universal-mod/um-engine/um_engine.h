// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The interface between um.dll and um-engine.dll (shared by both).
#pragma once

struct UmEngineFunction {
    const char* name;       // the game function's name, also its key in um-engine.cfg
    unsigned long address;  // where it starts in game.exe
    void* replacement;      // the new code (same calling convention and arguments)
    int required;           // 1: needed by the mod, installed whatever the config says; -1: experimental, off unless the config says true
    unsigned char expect[8]; // the original's first 8 bytes: not patched unless they match (another game.exe build)
};

// Exported by um-engine.dll:
//   const char* UmEngineVersion();
//   const UmEngineFunction* UmEngineFunctions(int* count);   // the ones to install (config applied)
//   void UmEngineSetLogger(UmEngineLogFn log);               // um.dll's log: um-engine's messages go to um.log (called before UmEngineFunctions)
typedef const char* (*UmEngineVersionFn)();
typedef const UmEngineFunction* (*UmEngineFunctionsFn)(int* count);
typedef void (*UmEngineLogFn)(const char* level, const char* text);   // level: "INFO", "WARN", "DEBUG"
typedef void (*UmEngineSetLoggerFn)(UmEngineLogFn log);
