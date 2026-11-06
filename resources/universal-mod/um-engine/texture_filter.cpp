// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Real trilinear filtering (RE: docs/game-exe/renderer.md 2.2, the per-stage filter states of the renderer start-up, 0x5011D4..0x5012D6).
// The options' "Filtering" (registry, 0 point / 1 bilinear / 2 trilinear) sets MINFILTER / MAGFILTER per texture stage, and with
// "Mipmapping" on all three choices then share one block that sets D3DTSS_MIPFILTER = 2, which is D3DTFP_POINT (the nearest mip level):
// "Trilinear" was drawn exactly like "Bilinear". Here the trilinear branch's tail (`je skip; jmp shared block`, 8 bytes at 0x501220)
// jumps to a copy of that block with D3DTFP_LINEAR (3); point and bilinear keep the game's own code. Both UI draws that switch the mip
// filter off for a moment (0x5EC933, 0x6468EB) read the current value first and put it back, so they keep the linear one.
#include <windows.h>

#include <cstring>

#include "um_engine.h"

extern "C" void UmEngineLog(const char* format, ...);
extern "C" void UmTrilinearStub();

// In: flags of `test ecx, ecx` (ecx = Mipmapping), ebx = the renderer, edi = the texture stage. Same registers out as the game's block.
asm(R"(
.text
.globl _UmTrilinearStub
_UmTrilinearStub:
    testl %ecx, %ecx
    jz 1f
    movl 0x20(%ebx), %esi
    pushl $3
    pushl $0x12
    pushl %edi
    movl 0x54(%esi), %eax
    pushl %eax
    movl (%eax), %ecx
    call *0x94(%ecx)
    movl %eax, 8(%esi)
1:
    pushl $0x005012B1
    ret
)");

namespace {
const unsigned long kTail = 0x00501220;                                                      // je 0x5012B1; jmp 0x50129A
const unsigned char kTailBytes[8] = { 0x0F, 0x84, 0x8B, 0x00, 0x00, 0x00, 0xEB, 0x72 };
const unsigned long kShared = 0x0050129A;                                                    // the shared block: mov esi,[ebx+0x20]; push 2; push 0x12 ...
const unsigned char kSharedBytes[8] = { 0x8B, 0x73, 0x20, 0x6A, 0x02, 0x6A, 0x12, 0x57 };
}

// Returns false (nothing changed) when game.exe is another build.
extern "C" bool UmTrilinearInstall() {
    if (memcmp(reinterpret_cast<void*>(kTail), kTailBytes, 8) != 0 || memcmp(reinterpret_cast<void*>(kShared), kSharedBytes, 8) != 0) {
        UmEngineLog("um-engine: Renderer::Trilinear skipped, game.exe has other code at the filter sites (another build)");
        return false;
    }
    if (!UmPatchJump(kTail, 0xE9, reinterpret_cast<void*>(&UmTrilinearStub))) { UmEngineLog("um-engine: cannot patch Renderer::Trilinear"); return false; }
    static const unsigned char nops[3] = { 0x90, 0x90, 0x90 };
    UmPatchCode(reinterpret_cast<void*>(kTail + 5), nops, 3);
    UmEngineLog("um-engine: Renderer::Trilinear installed (Filtering = trilinear now blends the mip levels)");
    return true;
}
