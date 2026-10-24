// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The interface between um.dll and um-engine.dll (shared by both).
#pragma once
#include <cstring>
// (the includer has included <windows.h>: UmPatchCode / UmPatchJump below use VirtualProtect and the Interlocked functions)

struct UmEngineFunction {
    const char* name;       // the game function's name, also its key in um-engine.cfg
    unsigned long address;  // where it starts in game.exe
    void* replacement;      // the new code (same calling convention and arguments)
    int required;           // 1: needed by the mod, installed whatever the config says; -1: experimental, off unless the config says true
    unsigned char expect[8]; // the original's first 8 bytes: not patched unless they match (another game.exe build)
};

// The layout of UmEngineFunction and the exports below: um.dll refuses an um-engine.dll whose UmEngineAbi() differs (a field added on one side would shift the whole table).
#define UM_ENGINE_ABI (static_cast<unsigned>(sizeof(UmEngineFunction)) | (1u << 16))

// Exported by um-engine.dll:
//   unsigned UmEngineAbi();                                  // UM_ENGINE_ABI of the build
//   const char* UmEngineVersion();
//   const UmEngineFunction* UmEngineFunctions(int* count);   // the ones to install (config applied)
//   void UmEngineSetLogger(UmEngineLogFn log);               // um.dll's log: um-engine's messages go to um.log (called before UmEngineFunctions)
typedef unsigned (*UmEngineAbiFn)();
typedef const char* (*UmEngineVersionFn)();
typedef const UmEngineFunction* (*UmEngineFunctionsFn)(int* count);
typedef void (*UmEngineLogFn)(const char* level, const char* text);   // level: "INFO", "WARN", "DEBUG"
typedef void (*UmEngineSetLoggerFn)(UmEngineLogFn log);

// Writes up to 8 bytes into game code while the game's threads may be running it. Within one cache line the 8 bytes are replaced by ONE locked compare-exchange: another thread sees
// the old or the new instructions, never half of each. At a line boundary (a locked access across it is a "split lock", slow or fatal on some Linux setups): the first two bytes are
// first turned into a jump-to-self, the rest written, then the first two bytes: a thread that arrives meanwhile spins for a few nanoseconds.
inline bool UmPatchCode(void* address, const void* bytes, size_t length) {
    unsigned char* at = static_cast<unsigned char*>(address);
    if (length == 0 || length > 8) return false;
    DWORD old = 0;
    if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    if ((reinterpret_cast<uintptr_t>(at) & 63) <= 56) {
        for (int tries = 0; tries < 16 && !ok; ++tries) {
            LONG64 expect, want;
            std::memcpy(&expect, at, 8); want = expect; std::memcpy(&want, bytes, length);
            ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(at), want, expect) == expect;
        }
    } else {
        const unsigned char spin[2] = { 0xEB, 0xFE };
        if (length > 2) {
            std::memcpy(at, spin, 2);
            std::memcpy(at + 2, static_cast<const unsigned char*>(bytes) + 2, length - 2);
        }
        std::memcpy(at, bytes, length > 2 ? 2 : length);
        ok = true;
    }
    VirtualProtect(at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 8);
    return ok;
}

// A jmp / call rel32 (opcode 0xE9 / 0xE8) at `address`, the following `keep` bytes (0..3) as they are.
inline bool UmPatchJump(unsigned long address, unsigned char opcode, const void* target) {
    unsigned char code[5] = { opcode, 0, 0, 0, 0 };
    const long rel = reinterpret_cast<long>(target) - static_cast<long>(address + 5);
    std::memcpy(code + 1, &rel, 4);
    return UmPatchCode(reinterpret_cast<void*>(address), code, 5);
}
