// The interface between um.dll and um-engine.dll (shared by both).
#pragma once

struct UmEngineFunction {
    const char* name;       // the game function's name, also its key in um-engine.cfg
    unsigned long address;  // where it starts in game.exe
    void* replacement;      // the new code (same calling convention and arguments)
    int required;           // 1: needed by the mod, installed whatever the config says
};

// Exported by um-engine.dll:
//   const char* UmEngineVersion();
//   const UmEngineFunction* UmEngineFunctions(int* count);   // the ones to install (config applied)
typedef const char* (*UmEngineVersionFn)();
typedef const UmEngineFunction* (*UmEngineFunctionsFn)(int* count);
