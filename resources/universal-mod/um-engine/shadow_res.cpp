// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Unit shadows at a higher resolution (RE: docs/shadows.md, game.exe CObject3DClientSpecific::Render 0x51CE60).
// Every unit's shadow is drawn into a 64 x 64 16-bit texture ("Format25", 4 bits of alpha per pixel) covering the unit's projected bounding box,
// then blurred (shadow quality 2) and stretched over the terrain below it. The size is written as constants in a handful of places, patched here
// when the size is raised (the shadow of a big creature then has a finer edge):
//   - the two texture creators (CTextureManager::CreateFormat25 0x692010 and GetOrCreateFormat25 0x692140): width and height, twice each;
//   - the scale of the projected silhouette (63.0 / extent): two `fdivr` operands in Render, pointed at a float of this DLL;
//   - the clearing of the scratch texture (`mov ecx, 0x800` dwords) and the two blur calls (`push 0x40; push 0x40`);
// All or nothing: if any of the places holds other bytes (another game.exe build), nothing is changed.
#include <windows.h>

#include <cstring>

extern "C" void UmEngineLog(const char* format, ...);

extern "C" { __attribute__((used)) unsigned UmShadowSize = 64; } // read by the blur stubs below
static float g_scale = 63.0f;

extern "C" void UmShadowBlurStubA();
extern "C" void UmShadowBlurStubB();

// The two blur calls: `push 0x40; push 0x40` (4 bytes) cannot take a size of 128 or more (a push imm8 is signed), so the code is redirected here.
asm(R"(
.text
.globl _UmShadowBlurStubA
_UmShadowBlurStubA:
    pushl _UmShadowSize
    pushl _UmShadowSize
    movl (%esi), %edx
    pushl $0x0051DC01
    ret
.globl _UmShadowBlurStubB
_UmShadowBlurStubB:
    pushl _UmShadowSize
    pushl _UmShadowSize
    pushl %eax
    pushl $0x0051DC14
    ret
)");

namespace {
struct Place {
    unsigned long address;
    unsigned char expect[6];
    size_t length;
};

const Place kCreatorSize[] = { // mov eax, 0x40  (the immediate is the width and height)
    { 0x00692031, {0xB8, 0x40, 0x00, 0x00, 0x00}, 5 }, { 0x00692081, {0xB8, 0x40, 0x00, 0x00, 0x00}, 5 },
    { 0x0069216F, {0xB8, 0x40, 0x00, 0x00, 0x00}, 5 }, { 0x006921BF, {0xB8, 0x40, 0x00, 0x00, 0x00}, 5 },
};
const Place kScale[] = { // fdivr dword ptr [0x73D044]  (63.0)
    { 0x0051D878, {0xD8, 0x3D, 0x44, 0xD0, 0x73, 0x00}, 6 }, { 0x0051D88D, {0xD8, 0x3D, 0x44, 0xD0, 0x73, 0x00}, 6 },
};
const Place kClear   = { 0x0051DA9C, {0xB9, 0x00, 0x08, 0x00, 0x00}, 5 };                 // mov ecx, 0x800 (dwords)
const Place kBlurA   = { 0x0051DBFB, {0x6A, 0x40, 0x6A, 0x40, 0x8B, 0x16}, 6 };           // push 0x40; push 0x40; mov edx,[esi]
const Place kBlurB   = { 0x0051DC0F, {0x6A, 0x40, 0x6A, 0x40, 0x50}, 5 };                 // push 0x40; push 0x40; push eax

bool Matches(const Place& p, size_t skip = 0) {
    return memcmp(reinterpret_cast<void*>(p.address + skip), p.expect + skip, p.length - skip) == 0;
}

void Write(unsigned long address, const void* bytes, size_t length) {
    unsigned char* at = reinterpret_cast<unsigned char*>(address);
    DWORD old = 0;
    if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at, bytes, length);
    VirtualProtect(at, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, length);
}

void Jump(unsigned long address, void* target, size_t length) {
    unsigned char code[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const long rel = reinterpret_cast<long>(target) - static_cast<long>(address + 5);
    memcpy(code + 1, &rel, 4);
    Write(address, code, length);
}
}

// Raises the unit shadow textures to size x size (a power of two from 64 to 256). Returns false (nothing changed) when game.exe is another build.
extern "C" bool UmShadowResolution(unsigned size) {
    if (size <= 64 || size > 256 || (size & (size - 1))) return false;
    bool ok = Matches(kClear) && Matches(kBlurA) && Matches(kBlurB);
    for (const Place& p : kCreatorSize) ok = ok && Matches(p);
    for (const Place& p : kScale) ok = ok && Matches(p);
    if (!ok) { UmEngineLog("um-engine: UnitShadow::Resolution skipped, game.exe has other code at the shadow sites (another build)"); return false; }

    UmShadowSize = size;
    g_scale = static_cast<float>(size - 1);
    for (const Place& p : kCreatorSize) Write(p.address + 1, &size, 4);
    for (const Place& p : kScale) { const void* operand = &g_scale; Write(p.address + 2, &operand, 4); }
    const unsigned dwords = size * size / 2;
    Write(kClear.address + 1, &dwords, 4);
    Jump(kBlurA.address, reinterpret_cast<void*>(&UmShadowBlurStubA), 6);
    Jump(kBlurB.address, reinterpret_cast<void*>(&UmShadowBlurStubB), 5);
    UmEngineLog("um-engine: UnitShadow::Resolution installed (%u x %u)", size, size);
    return true;
}
