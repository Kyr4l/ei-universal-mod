// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the heap hooks: overrun reports, the free quarantine, guarded threads.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// ---------------------------------------------------------------------------
// HEAP_OVERRUN_REPORTS
//
// Crash dumps from Wine show game.exe destroying a UI screen (0x5f6620) whose
// child-widget list still holds a pointer to a widget that was already freed:
// the virtual call through it (0x5ec010) reads a vtable pointer out of freed
// memory. Windows' heap tends to leave a freed block's contents intact, so
// the stale pointer still lands on a valid vtable and the bug goes unnoticed;
// Wine's heap overwrites/reuses the block almost immediately and the process
// dies. Holding recently freed blocks back - never touching them, never
// handing them out again - gives Wine the same forgiving behavior, and any
// write through a stale pointer then corrupts nothing else either.
//
// game.exe's static CRT (VC7.1) selects the plain system heap on any NT 5+
// OS (__heap_select at 0x6f539e), so every malloc/free/new/delete reaches
// HeapAlloc/HeapFree on its private _crtheap through game.exe's own import
// table, which is exactly what PatchImportedFunction hooks. (On an OS that
// reports as Windows 9x it would use its own small-block heap instead, which
// bypasses HeapFree - ReadGameCrtHeapMode() below is logged so that's visible.)
// ---------------------------------------------------------------------------

CRITICAL_SECTION g_quarantineLock;
// Set only once the hooks are live, and deliberately never cleared by a config
// reload: blocks already held must keep being recognized as held.
bool g_heapFreeQuarantineInstalled = false;
// Every live allocation game.exe made through its own HeapAlloc since this hook went in (start ->
// requested size). A HeapFree for a pointer that is not a block's start but lies inside a live block
// is a free of the MIDDLE of an allocation (an array element): on Wine that corrupts the heap, and
// the crashes seen so far (stale children, zeroed list nodes, damaged headers) fit it.
static const int kAllocCallerSlots = 3;

// Open-addressing hash table from a 32-bit address to V (linear probing, backward-shift deletion so
// there are no tombstones), in memory of its own from VirtualAlloc. The hooks run for every game
// allocation; a std::map costs a tree walk with cache misses and a malloc per operation, which measured
// at ~3 us per free+alloc with 300,000 live blocks. Callers hold g_quarantineLock.
template <typename V>
class PtrTable {
public:
    struct Slot { DWORD key; V value; };   // key 0 = empty
    size_t Size() const { return count_; }
    size_t Capacity() const { return capacity_; }
    Slot& At(size_t index) { return slots_[index]; }
    V* Find(DWORD key) {
        if (!slots_) return nullptr;
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return &slots_[i].value;
            if (slots_[i].key == 0) return nullptr;
        }
    }
    bool Put(DWORD key, const V& value) {
        if (!slots_ || (count_ + 1) * 8 > capacity_ * 5) {
            if (!Grow() && (!slots_ || (count_ + 1) * 10 > capacity_ * 9)) return false;
        }
        size_t i = Home(key);
        while (slots_[i].key != 0 && slots_[i].key != key) i = (i + 1) & mask_;
        if (slots_[i].key == 0) { ++count_; slots_[i].key = key; }
        slots_[i].value = value;
        return true;
    }
    bool Erase(DWORD key) {
        if (!slots_) return false;
        size_t i = Home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        EraseAt(i);
        return true;
    }
    void EraseAt(size_t hole) {
        size_t i = hole, j = hole;
        for (;;) {
            slots_[i].key = 0;
            for (;;) {
                j = (j + 1) & mask_;
                if (slots_[j].key == 0) { --count_; return; }
                const size_t home = Home(slots_[j].key);
                // The entry at j may move back into the hole at i unless its home lies in (i, j].
                if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) break;
            }
            slots_[i] = slots_[j];
            i = j;
        }
    }
    void Clear() {
        if (slots_) memset(slots_, 0, capacity_ * sizeof(Slot));
        count_ = 0;
    }
private:
    size_t Home(DWORD key) const { return static_cast<size_t>(((key >> 3) * 2654435761u) >> shift_); }
    bool Grow() {
        const size_t newCapacity = capacity_ ? capacity_ * 2 : (static_cast<size_t>(1) << 16);
        Slot* fresh = static_cast<Slot*>(VirtualAlloc(NULL, newCapacity * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!fresh) return false;
        Slot* old = slots_;
        const size_t oldCapacity = capacity_;
        slots_ = fresh;
        capacity_ = newCapacity;
        mask_ = newCapacity - 1;
        shift_ = 32;
        for (size_t c = newCapacity; c > 1; c >>= 1) --shift_;
        for (size_t k = 0; k < oldCapacity; ++k) {
            if (old[k].key == 0) continue;
            size_t i = Home(old[k].key);
            while (slots_[i].key != 0) i = (i + 1) & mask_;
            slots_[i] = old[k];
        }
        if (old) VirtualFree(old, 0, MEM_RELEASE);
        return true;
    }
    Slot* slots_ = nullptr;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    unsigned shift_ = 32;
    size_t count_ = 0;
};

struct LiveBlock {
    DWORD size;                          // the size the game asked for (the real block is g_canaryBytes larger)
    HANDLE heap;
    DWORD tickMs;                        // GetTickCount() at the allocation
    DWORD callers[kAllocCallerSlots];    // who allocated it (return-address candidates, innermost first)
};
// Every allocation is made g_canaryBytes larger and the extra bytes are filled with kCanaryFill; the
// pointer returned to the game is unchanged. A buffer overrun lands in those bytes first, so a change
// there means "this block was written past its end" - and the block's allocation site is known.
static const DWORD kMaxCanaryBytes = 512;
DWORD g_canaryBytes = 64;         // HEAP_ALLOC_PADDING, for the statistics only: see PaddingFor
// The padding of a block of this size. The game's figure objects (656 bytes) overrun by 12, but the
// arrays of pointers of 1 KB and more overrun by 130+ bytes (measured with a 160-byte guard), so those
// get a proportionally larger one: a quarter of their size, at most 512 bytes.
static DWORD PaddingFor(SIZE_T size) {
    if (g_heapAllocPadding <= 0) return 0;
    DWORD padding = static_cast<DWORD>(g_heapAllocPadding);
    if (size >= 1024) {
        const DWORD scaled = size / 4 > 512 ? 512 : static_cast<DWORD>(size / 4);
        if (scaled > padding) padding = scaled;
    }
    return padding;
}
static const BYTE kCanaryFill = 0xA5;
static const unsigned long kOverrunLogLimit = 25;
static unsigned long g_quarantineOverruns = 0;
static unsigned long g_quarantineOverlaps = 0;
static std::unordered_set<DWORD> g_reportedOverruns;
static std::map<DWORD, unsigned long> g_overrunSites;   // allocation site (innermost caller) -> blocks it overran
static void FormatOverrunSites(char* out, size_t outSize);
static PtrTable<LiveBlock> g_liveBlocks;

// game.exe's CRT heap mode: 1 = system heap, 2 = V5 small-block heap, 3 = V6
// small-block heap (__active_heap, 0x7caf0c in the one known OBT-1 game.exe
// build, which is loaded at its preferred base). Returns -1 when this is not
// that build or the address isn't readable - the number means nothing then.
long ReadGameCrtHeapMode() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleA(NULL));
    if (base != reinterpret_cast<const BYTE*>(0x400000)) {
        return -1;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage <= 0x7caf10 - 0x400000) {
        return -1;
    }
    const DWORD* mode = reinterpret_cast<const DWORD*>(0x7caf0c);
    if (IsBadReadPtr(mode, sizeof(DWORD))) {
        return -1;
    }
    return static_cast<long>(*mode);
}

// Does the value found on the stack look like a return address, i.e. does a
// call instruction end right before it? Recognizes the call encodings MSVC
// emits (E8 rel32, FF /2 with register/[reg]/[reg+disp8]/[reg+disp32]/[imm32]).
// On the direct form, the callee is written to *target.
bool LooksLikeReturnAddress(const BYTE* address, DWORD* target, bool knownReadable) {
    *target = 0;
    if (!knownReadable && IsBadReadPtr(address - 6, 6)) {
        return false;
    }
    if (address[-5] == 0xE8) {
        *target = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(address) +
            *reinterpret_cast<const LONG*>(address - 4));
        return true;
    }
    if (address[-2] == 0xFF && ((address[-1] & 0xF8) == 0xD0 || (address[-1] & 0xF8) == 0x10)) {
        return true;
    }
    if (address[-3] == 0xFF && (address[-2] & 0xF8) == 0x50) {
        return true;
    }
    if (address[-6] == 0xFF && (address[-5] == 0x15 || (address[-5] & 0xF8) == 0x90)) {
        return true;
    }
    return false;
}

// game.exe's own address ranges, for recognising vtable pointers and return addresses without
// probing memory. Set up when the hooks are installed.
DWORD g_imageLow = 0, g_imageHigh = 0, g_codeLow = 0, g_codeHigh = 0;
// The one known OBT-1 build: its static CRT (its own free/delete frames) lives at 0x6E0000 and
// above, which is noise in a "who freed this" list.
bool g_knownGameBuild = false;
static const DWORD kKnownBuildCrtStart = 0x6E0000;

void InitGameImageRanges() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleA(NULL));
    if (!base) return;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_imageLow = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(base));
    g_imageHigh = g_imageLow + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_CNT_CODE) {
            DWORD low = g_imageLow + section->VirtualAddress;
            DWORD high = low + section->Misc.VirtualSize;
            if (g_codeLow == 0 || low < g_codeLow) g_codeLow = low;
            if (high > g_codeHigh) g_codeHigh = high;
        }
    }
    g_knownGameBuild = ReadGameCrtHeapMode() == 1;
}

// Where the game freed a block: return-address-looking words in game.exe's code found in the
// next 768 bytes of this thread's stack, innermost first (the static CRT's own frames left out
// on the known build). No symbols and no memory probing - a scan this cheap runs on every free.
void CaptureCallers(DWORD* out, int slots) {
    const DWORD* sp = reinterpret_cast<const DWORD*>(__builtin_frame_address(0));
    const DWORD* stackBase = reinterpret_cast<const DWORD*>(__readfsdword(4));
    const DWORD* end = sp + 192 < stackBase ? sp + 192 : stackBase;
    int found = 0;
    for (const DWORD* word = sp; word < end && found < slots; ++word) {
        DWORD value = *word;
        if (value < g_codeLow + 6 || value >= g_codeHigh) continue;
        if (g_knownGameBuild && value >= kKnownBuildCrtStart) continue;
        DWORD target = 0;
        if (!LooksLikeReturnAddress(reinterpret_cast<const BYTE*>(value), &target, true)) continue;
        out[found++] = value;
    }
}

static void FormatCallerList(const DWORD* callers, int count, char* out, size_t outSize) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count && callers[i]; ++i) {
        int wrote = snprintf(out + used, outSize - used, "%s0x%08lX", i ? " <- " : "", static_cast<unsigned long>(callers[i]));
        if (wrote < 0 || static_cast<size_t>(wrote) >= outSize - used) break;
        used += static_cast<size_t>(wrote);
    }
    if (out[0] == '\0') snprintf(out, outSize, "(not recorded)");
}

static void LogQuarantineStats(const char* reason) {
    EnterCriticalSection(&g_quarantineLock);
    unsigned long overruns = g_quarantineOverruns;
    unsigned long overlaps = g_quarantineOverlaps;
    unsigned long liveBlocks = static_cast<unsigned long>(g_liveBlocks.Size());
    char overrunSites[160];
    FormatOverrunSites(overrunSites, sizeof(overrunSites));
    LeaveCriticalSection(&g_quarantineLock);
    LogLine("DEBUG", "[HEAPFIX] %s: buffer overruns=%lu (sites: %s), overlapping allocations=%lu, live blocks tracked=%lu (padding %.1f MB)",
        reason, overruns, overrunSites, overlaps, liveBlocks, liveBlocks * static_cast<double>(g_canaryBytes) / 1048576.0);
}

// Point-in-time numbers for the overlay; false when the allocation tracking isn't active.
bool GetQuarantineSnapshot(unsigned long* problems, unsigned long* overruns) {
    if (!g_heapFreeQuarantineInstalled) {
        return false;
    }
    EnterCriticalSection(&g_quarantineLock);
    // Overruns are absorbed by the padding, so they are counted apart from the real problems.
    *overruns = g_quarantineOverruns;
    *problems = g_quarantineOverlaps;
    LeaveCriticalSection(&g_quarantineLock);
    return true;
}

// What was written: readable text if it looks like text (script text, names), else the first bytes in hex.
static void DescribeWrittenBytes(const BYTE* bytes, SIZE_T available, char* out, size_t outSize) {
    size_t length = available < 48 ? available : 48;
    size_t printable = 0;
    for (size_t i = 0; i < length; ++i) if (bytes[i] >= 0x20 && bytes[i] < 0x7F) ++printable;
    if (length >= 8 && printable * 10 >= length * 7) {
        char text[64];
        for (size_t i = 0; i < length; ++i) text[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<char>(bytes[i]) : '.';
        text[length] = '\0';
        snprintf(out, outSize, "text \"%s\"", text);
    } else {
        size_t shown = length < 16 ? length : 16;
        size_t used = snprintf(out, outSize, "bytes");
        for (size_t i = 0; i < shown && used + 4 < outSize; ++i) used += snprintf(out + used, outSize - used, " %02X", bytes[i]);
    }
}

// True when the guard bytes after a live block were changed (the 16 bytes found are copied out).
// Unreadable memory counts as intact: a block freed behind the hooks' back must not crash the scan.
static bool CanaryDamaged(DWORD start, DWORD size, BYTE* found, bool probe = true) {
    const DWORD padding = PaddingFor(size);
    if (padding == 0) return false;
    const BYTE* tail = reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(start)) + size;
    if (probe && IsBadReadPtr(tail, padding)) return false;
    bool damaged = false;
    for (DWORD i = 0; i < padding; ++i) {
        found[i] = tail[i];
        if (tail[i] != kCanaryFill) damaged = true;
    }
    return damaged;
}

// The most-overrunning allocation sites, "0x005AA92E x40, ...", for the stats and the crash report.
static void FormatOverrunSites(char* out, size_t outSize) {
    std::vector<std::pair<unsigned long, DWORD>> sites;
    for (const auto& entry : g_overrunSites) sites.push_back({entry.second, entry.first});
    std::sort(sites.begin(), sites.end(), [](const std::pair<unsigned long, DWORD>& x, const std::pair<unsigned long, DWORD>& y) {
        return x.first > y.first;
    });
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sites.size() && i < 6; ++i) {
        int wrote = snprintf(out + used, outSize - used, "%s0x%08lX x%lu", i ? ", " : "", static_cast<unsigned long>(sites[i].second), sites[i].first);
        if (wrote < 0 || static_cast<size_t>(wrote) >= outSize - used) break;
        used += static_cast<size_t>(wrote);
    }
    if (out[0] == '\0') snprintf(out, outSize, "none");
}

// Once per block; logged for the first few blocks of each allocation site (a site that overruns
// does it for every object it makes). Lock held.
static void ReportOverrun(DWORD start, const LiveBlock& block, const BYTE* found, const char* when) {
    if (g_reportedOverruns.size() >= 200000) g_reportedOverruns.clear(); // bounded: a very long session must not grow it forever
    if (!g_reportedOverruns.insert(start).second) return;
    ++g_quarantineOverruns;
    static bool explained = false;
    if (!explained) {
        explained = true;
        LogLine("INFO", "[HEAPFIX] game.exe writes a few bytes past the end of some of its own allocations (a bug of the game, "
            "on any setup). The padding absorbs it and nothing is damaged; each allocation site is reported below (DEBUG while the "
            "overrun stays in the padding, ERROR when it may have gone past it).");
    }
    const DWORD padding = PaddingFor(block.size);
    int first = -1, last = -1;
    for (DWORD i = 0; i < padding; ++i) {
        if (found[i] != kCanaryFill) { if (first < 0) first = static_cast<int>(i); last = static_cast<int>(i); }
    }
    if (first < 0) return;
    const unsigned long siteCount = ++g_overrunSites[block.callers[0]];
    const bool usedUp = static_cast<DWORD>(last + 1) >= padding;   // the write may have gone on beyond the padding
    const bool powerOfTen = siteCount == 10 || siteCount == 100 || siteCount == 1000 || siteCount == 10000;
    if (siteCount > 3 && !powerOfTen && !usedUp) return;
    char callers[100], written[140];
    FormatCallerList(block.callers, kAllocCallerSlots, callers, sizeof(callers));
    DWORD shown = padding - static_cast<DWORD>(first) < 24 ? padding - static_cast<DWORD>(first) : 24;
    DescribeWrittenBytes(found + first, shown, written, sizeof(written));
    // Contained in the padding: harmless (DEBUG). The whole padding used: it may have reached the next block (ERROR).
    LogLine(usedUp ? "ERROR" : "DEBUG", "[HEAPFIX] BUFFER OVERRUN (%s): block 0x%08lX (%lu bytes, allocated %.1f s ago by %s) was written past its end, "
        "up to %d of its %lu padding bytes (first at +%d: %s); this allocation site has now overrun %lu block(s)%s%s",
        when, static_cast<unsigned long>(start), static_cast<unsigned long>(block.size), (GetTickCount() - block.tickMs) / 1000.0,
        callers, last + 1, static_cast<unsigned long>(padding), first, written, siteCount,
        usedUp ? "; THE WHOLE PADDING WAS USED, the write may have gone further: raise HEAP_ALLOC_PADDING" : "",
        siteCount > 3 && !usedUp ? " (not logging each one any more)" : "");
}

// The heap returned memory that overlaps a block the game still has: something is wrong with the
// allocator's bookkeeping (a stale free, or damaged headers). Lock held.
static void ReportOverlap(DWORD start, DWORD size, const DWORD* callers, DWORD otherStart, const LiveBlock& other) {
    unsigned long number = ++g_quarantineOverlaps;
    if (number > kOverrunLogLimit) return;
    char byCallers[100], otherCallers[100];
    FormatCallerList(callers, kAllocCallerSlots, byCallers, sizeof(byCallers));
    FormatCallerList(other.callers, kAllocCallerSlots, otherCallers, sizeof(otherCallers));
    LogLine("WARN", "[HEAPFIX] OVERLAPPING ALLOCATION #%lu: HeapAlloc returned 0x%08lX (%lu bytes, requested by %s) which overlaps the LIVE "
        "block 0x%08lX (%lu bytes, allocated %.1f s ago by %s) - the heap handed out memory that was still in use", number,
        static_cast<unsigned long>(start), static_cast<unsigned long>(size), byCallers, static_cast<unsigned long>(otherStart),
        static_cast<unsigned long>(other.size), (GetTickCount() - other.tickMs) / 1000.0, otherCallers);
}


UM_GUARDED_THREAD(OverrunWatchThread) {
    for (;;) {
        Sleep(500);
        size_t resumeAt = 0;
        for (;;) {
            EnterCriticalSection(&g_quarantineLock);
            const size_t capacity = g_liveBlocks.Capacity();
            const size_t end = resumeAt + 4096 < capacity ? resumeAt + 4096 : capacity;
            for (size_t i = resumeAt; i < end;) {
                auto& slot = g_liveBlocks.At(i);
                if (slot.key == 0) { ++i; continue; }
                BYTE found[kMaxCanaryBytes];
                if (CanaryDamaged(slot.key, slot.value.size, found)) {
                    const void* pointer = reinterpret_cast<const void*>(static_cast<ULONG_PTR>(slot.key));
                    if (HeapSize(slot.value.heap, 0, pointer) == static_cast<SIZE_T>(-1)) {
                        g_liveBlocks.EraseAt(i); // the entry shifted into this slot has to be looked at too
                        continue;
                    }
                    ReportOverrun(slot.key, slot.value, found, "found by the background scan");
                }
                ++i;
            }
            resumeAt = end;
            const bool done = resumeAt >= capacity;
            LeaveCriticalSection(&g_quarantineLock);
            if (done) break;
            Sleep(1);
        }
    }
    return 0;
}

// Called for every HeapFree game.exe makes directly: looks at the block's guard bytes, forgets it, and lets
// the real HeapFree free it (returns false: the caller forwards the free).
static bool QuarantineHeapFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    (void)heap;
    (void)flags;
    EnterCriticalSection(&g_quarantineLock);
    const DWORD address = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer));
    if (LiveBlock* live = g_liveBlocks.Find(address)) {
        BYTE found[kMaxCanaryBytes];
        if (CanaryDamaged(address, live->size, found, false)) {
            ReportOverrun(address, *live, found, "found when the game freed the block");
        }
        g_liveBlocks.Erase(address);
    }
    LeaveCriticalSection(&g_quarantineLock);
    return false;
}

// Forget every tracked block of a heap that is about to be destroyed.
static void QuarantineForgetHeap(HANDLE heap) {
    EnterCriticalSection(&g_quarantineLock);
    std::vector<DWORD> doomed;
    for (size_t i = 0; i < g_liveBlocks.Capacity(); ++i) {
        if (g_liveBlocks.At(i).key != 0 && g_liveBlocks.At(i).value.heap == heap) doomed.push_back(g_liveBlocks.At(i).key);
    }
    for (DWORD key : doomed) g_liveBlocks.Erase(key);
    LeaveCriticalSection(&g_quarantineLock);
}

// For a register that looks like a pointer to a heap object: what is at that address, and what
// does the game's heap say about it? "FREE" means the game's own heap no longer has an allocated
// block starting there - the pointer is stale - while "allocated" means something (possibly a
// different object that reused the address) lives there now. The first dwords show the vtable
// (which class it is) or zero/garbage if the memory was cleared or reused.
static void LogHeapPointerProbe(const char* label, DWORD value) {
    if (value < 0x10000 || value >= 0x7FFF0000 || (value & 7) != 0) return;
    if (value >= g_imageLow && value < g_imageHigh) return;
    const void* pointer = reinterpret_cast<const void*>(static_cast<ULONG_PTR>(value));
    if (IsBadReadPtr(pointer, 16)) return;
    DWORD dwords[4] = {};
    memcpy(dwords, pointer, sizeof(dwords));
    HANDLE heap = NULL;
    if (g_knownGameBuild) {
        const HANDLE* crtHeap = reinterpret_cast<const HANDLE*>(0x7caf08); // game.exe's _crtheap
        if (!IsBadReadPtr(crtHeap, sizeof(HANDLE))) heap = *crtHeap;
    }
    const char* state = "game heap not identified";
    char stateBuffer[80];
    if (heap) {
        SIZE_T size = HeapSize(heap, 0, pointer);
        if (size == static_cast<SIZE_T>(-1)) {
            state = "NOT an allocated block of the game heap: freed (stale pointer) or not a block start";
        } else {
            snprintf(stateBuffer, sizeof(stateBuffer), "an ALLOCATED block of %lu bytes in the game heap", static_cast<unsigned long>(size));
            state = stateBuffer;
        }
    }
    LogLine("FATAL", "[HEAPFIX] crash: %s = 0x%08lX is %s; first dwords %08lX %08lX %08lX %08lX", label,
        static_cast<unsigned long>(value), state, dwords[0], dwords[1], dwords[2], dwords[3]);
}

// Crash report: which freed block, if any, do the crashing thread's registers and stack point
// into? "None" says the object is older than the window (or is not a heap block at all).
// Non-blocking on the lock: the crash may have happened inside it.
// Only the words right at the stack pointer are looked at: a stack slot deeper down is very
// often a stale leftover, and with hundreds of MB of freed blocks held one would "hit" one by
// coincidence. Registers and the fault address are the strong evidence.
static const DWORD kCrashStackWords = 32;

void LogQuarantineCrashAnalysis(const CONTEXT* context, const EXCEPTION_RECORD* record) {
#if defined(_M_IX86) || defined(__i386__)
    if (!g_heapFreeQuarantineInstalled || !context) {
        return;
    }
    struct Value { const char* label; DWORD value; DWORD stackOffset; };
    std::vector<Value> values;
    values.push_back({"EAX", context->Eax, 0});
    values.push_back({"EBX", context->Ebx, 0});
    values.push_back({"ECX", context->Ecx, 0});
    values.push_back({"EDX", context->Edx, 0});
    values.push_back({"ESI", context->Esi, 0});
    values.push_back({"EDI", context->Edi, 0});
    values.push_back({"EBP", context->Ebp, 0});
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        values.push_back({"fault address", static_cast<DWORD>(record->ExceptionInformation[1]), 0});
    }
    const DWORD stackLimit = __readfsdword(8);
    const DWORD stackBase = __readfsdword(4);
    if (context->Esp >= stackLimit && context->Esp < stackBase && (context->Esp & 3) == 0) {
        const DWORD* words = reinterpret_cast<const DWORD*>(context->Esp);
        for (DWORD i = 0; i < kCrashStackWords && context->Esp + (i + 1) * 4 <= stackBase; ++i) {
            values.push_back({"stack", words[i], i * 4});
        }
    }

    // Registers first: what each heap-looking one points at. Skipped when the crash is not in
    // game.exe itself (inside ntdll the heap may be damaged and a HeapSize call is unsafe).
    if (record && reinterpret_cast<DWORD>(record->ExceptionAddress) >= g_imageLow &&
            reinterpret_cast<DWORD>(record->ExceptionAddress) < g_imageHigh) {
        for (size_t i = 0; i < 7; ++i) {
            LogHeapPointerProbe(values[i].label, values[i].value);
        }
    }

    if (!TryEnterCriticalSection(&g_quarantineLock)) {
        LogLine("FATAL", "[HEAPFIX] crash: could not check the tracked blocks (another thread holds the lock)");
        return;
    }
    {
        unsigned long overrunBlocks = 0;
        for (size_t i = 0; i < g_liveBlocks.Capacity(); ++i) {
            const auto& entry = g_liveBlocks.At(i);
            if (entry.key == 0) continue;
            BYTE found[kMaxCanaryBytes];
            if (CanaryDamaged(entry.key, entry.value.size, found)) {
                ++overrunBlocks;
                ReportOverrun(entry.key, entry.value, found, "found by the crash report");
            }
        }
        char sites[160];
        FormatOverrunSites(sites, sizeof(sites));
        LogLine("FATAL", "[HEAPFIX] crash: %lu live block(s) have overwritten guard bytes (a buffer was written past its end); "
            "allocation sites that overran so far: %s; %lu overlapping allocation(s) were seen this session", overrunBlocks, sites,
            g_quarantineOverlaps);
    }
    LeaveCriticalSection(&g_quarantineLock);
#else
    (void)context; (void)record;
#endif
}

static std::string FileNameOfPath(const char* path) {
    const char* name = strrchr(path, '\\');
    const char* slash = strrchr(path, '/');
    if (slash && (!name || slash > name)) {
        name = slash;
    }
    return name ? name + 1 : path;
}

static void CopyMapText(char* dest, size_t destSize, const std::string& text) {
    snprintf(dest, destSize, "%s", text.c_str());
}

// A map is several files. The quest map (z35q1.mob, with a z35q1.mq archive whose map.txt names the
// terrain and the base map) is opened next to its base map (zone35x-lmp.mob), and its script can load
// more script-only maps (z35q1_strongwarriors.mob) with AddMob, so "the last .mob opened" names the
// wrong thing. A .mob is an extra map when a script already loaded says it AddMobs it; otherwise it
// starts a new map (or completes the current one, when it is the base map the quest asked for).
static const ULONGLONG kMapSameLoadMs = 30000;

static void NoteMapFile(const char* path, bool isMob) {
    const std::string name = FileNameOfPath(path);
    const std::string lower = LowerCaseCopy(name);
    const ULONGLONG now = GetTickCount64();

    if (!isMob) {
        while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
        CopyMapText(g_map.recentMpr, sizeof(g_map.recentMpr), name);
        g_map.recentMprTickMs = now;
        if (g_map.valid && g_map.mpr[0] == '\0' && now - g_map.openedTickMs < kMapSameLoadMs) {
            CopyMapText(g_map.mpr, sizeof(g_map.mpr), name);
        }
        InterlockedExchange(&g_currentMapLock, 0);
        return;
    }

    // Read outside the lock: this parses the map's script (cached for the next time).
    std::shared_ptr<MobScriptContext> context = LoadMobContext(path);
    std::string mqBase, mqMpr;
    bool isQuest = false;
    size_t dot = std::string(path).find_last_of('.');
    if (dot != std::string::npos) {
        std::string mqPath = std::string(path).substr(0, dot) + ".mq";
        if (GetFileAttributesA(mqPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            isQuest = true;
            ReadMqBaseMapName(mqPath, &mqBase, &mqMpr);
        }
    }
    if (!mqMpr.empty() && mqMpr.find('.') == std::string::npos) mqMpr += ".mpr";
    const std::string mqBaseFile = mqBase.empty() ? std::string() : mqBase + ".mob";

    while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
    const bool fresh = g_map.valid && now - g_map.openedTickMs < kMapSameLoadMs;
    bool startNew = false;
    if (g_map.valid && g_mapExpectedExtras.count(lower) != 0 &&
            !EqualsIgnoreCase(name.c_str(), g_map.quest) && !EqualsIgnoreCase(name.c_str(), g_map.base)) {
        ++g_map.extraCount;
        CopyMapText(g_map.lastExtra, sizeof(g_map.lastExtra), name);
    } else if (fresh && (EqualsIgnoreCase(name.c_str(), g_map.quest) || EqualsIgnoreCase(name.c_str(), g_map.base))) {
        // The same map file read again while it loads: nothing new.
    } else if (fresh && !isQuest && g_map.baseWanted[0] && lower == g_map.baseWanted) {
        CopyMapText(g_map.base, sizeof(g_map.base), name); // the quest opened first, its base map follows
    } else if (fresh && isQuest && g_map.quest[0] == '\0' && g_map.base[0] &&
            EqualsIgnoreCase(g_map.base, mqBaseFile.c_str())) {
        CopyMapText(g_map.quest, sizeof(g_map.quest), name); // the base map opened first, its quest follows
        if (g_map.mpr[0] == '\0' && !mqMpr.empty()) CopyMapText(g_map.mpr, sizeof(g_map.mpr), mqMpr);
    } else {
        startNew = true;
    }
    if (startNew) {
        memset(g_map.mpr, 0, sizeof(g_map.mpr));
        memset(g_map.base, 0, sizeof(g_map.base));
        memset(g_map.quest, 0, sizeof(g_map.quest));
        memset(g_map.baseWanted, 0, sizeof(g_map.baseWanted));
        memset(g_map.lastExtra, 0, sizeof(g_map.lastExtra));
        g_map.extraCount = 0;
        g_map.valid = true;
        g_map.openedTickMs = now;
        g_mapExpectedExtras.clear();
        if (isQuest) {
            CopyMapText(g_map.quest, sizeof(g_map.quest), name);
            if (!mqMpr.empty()) CopyMapText(g_map.mpr, sizeof(g_map.mpr), mqMpr);
            if (!mqBaseFile.empty()) CopyMapText(g_map.baseWanted, sizeof(g_map.baseWanted), LowerCaseCopy(mqBaseFile));
        } else {
            CopyMapText(g_map.base, sizeof(g_map.base), name);
            if (g_map.recentMpr[0] && now - g_map.recentMprTickMs < kMapSameLoadMs) {
                CopyMapText(g_map.mpr, sizeof(g_map.mpr), g_map.recentMpr);
            }
        }
    }
    if (context) {
        for (const std::string& added : context->addMobs) g_mapExpectedExtras.insert(added);
    }
    InterlockedExchange(&g_currentMapLock, 0);
}

bool CopyMapState(MapLoadState* out) {
    while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) {
        Sleep(0);
    }
    *out = g_map;
    InterlockedExchange(&g_currentMapLock, 0);
    return out->valid;
}

// Every .mpr / .mob the game opened: lower-case file name -> its full path (for the DLL server's MAP, so
// um-multitool can read the very files the game uses). Guarded by g_currentMapLock.
static std::unordered_map<std::string, std::string> g_mapPaths;

std::string MapFilePath(const char* name) {
    std::string out;
    while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
    auto it = g_mapPaths.find(LowerCaseCopy(name));
    if (it != g_mapPaths.end()) out = it->second;
    InterlockedExchange(&g_currentMapLock, 0);
    return out;
}

static void NoteMapLoad(const char* path) {
    const bool isMob = HasFileExtension(path, "mob");
    if (!isMob && !HasFileExtension(path, "mpr")) {
        return;
    }
    {
        // A relative path is relative to the game's current directory: kept absolute.
        char full[MAX_PATH] = {};
        const DWORD n = GetFullPathNameA(path, sizeof(full), full, NULL);
        const std::string key = LowerCaseCopy(FileNameOfPath(path));
        while (InterlockedCompareExchange(&g_currentMapLock, 1, 0) != 0) Sleep(0);
        g_mapPaths[key] = n > 0 && n < sizeof(full) ? full : path;
        InterlockedExchange(&g_currentMapLock, 0);
    }
    if (!g_enableOverlay || !g_overlayShowMap) {
        // The overlay is the only user of the map identity; the rest still runs for .mob files.
        if (isMob) {
            RememberSeenMobPath(path);
            if (g_heapFreeQuarantineInstalled) {
                LogQuarantineStats("Map load");
            }
        }
        return;
    }
    if (isMob) {
        RememberSeenMobPath(path); // before NoteMapFile: it reads the file and the quest's base map
    }
    NoteMapFile(path, isMob);
    if (isMob && g_heapFreeQuarantineInstalled) {
        LogQuarantineStats("Map load");
    }
}

// Convert a Windows wide path to the log's narrow system-code-page format.
static void ConvertWidePath(LPCWSTR widePath, char* path, size_t pathSize) {
    if (!widePath || pathSize == 0) {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, widePath, -1, path, static_cast<int>(pathSize), NULL, NULL);
    path[pathSize - 1] = '\0';
}

// Forward CreateFileA and record the resulting handle/path for diagnostics.
HANDLE WINAPI HookedCreateFileA(LPCSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileA(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        LogOpenedFile(handle, fileName, desiredAccess);
        if ((desiredAccess & GENERIC_READ) != 0) {
            try {
                ValidateMobFile(fileName);
                NoteMapLoad(fileName);
            } catch (...) {
                // An exception thrown here would unwind through the game's own frames and end the process.
            }
        }
    }
    return handle;
}

// Forward CreateFileW, converting its path before recording it.
HANDLE WINAPI HookedCreateFileW(LPCWSTR fileName, DWORD desiredAccess,
        DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
        DWORD flagsAndAttributes, HANDLE templateFile) {
    HANDLE handle = g_originalCreateFileW(fileName, desiredAccess, shareMode,
        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    if (handle != INVALID_HANDLE_VALUE && fileName) {
        char path[MAX_PATH] = {};
        ConvertWidePath(fileName, path, sizeof(path));
        LogOpenedFile(handle, path, desiredAccess);
        if ((desiredAccess & GENERIC_READ) != 0) {
            try {
                ValidateMobFile(path);
                NoteMapLoad(path);
            } catch (...) {
            }
        }
    }
    return handle;
}

// Forward ReadFile and log the first successful read for a tracked handle.
BOOL WINAPI HookedReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead,
        LPDWORD bytesRead, LPOVERLAPPED overlapped) {
    BOOL result = g_originalReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
    if (result) {
        char path[MAX_PATH] = {};
        if (CopyAndMarkTrackedFileIo(file, false, path, sizeof(path))) {
            DWORD length = bytesRead ? *bytesRead : 0;
            LogFileIo("File read path=%s bytes=%lu handle=%p", path, length, file);
            LogFileContents("File read", path, buffer, length);
        }
    }
    return result;
}

// Forward WriteFile and log the first successful write for a tracked handle.
BOOL WINAPI HookedWriteFile(HANDLE file, LPCVOID buffer, DWORD bytesToWrite,
        LPDWORD bytesWritten, LPOVERLAPPED overlapped) {
    BOOL result = g_originalWriteFile(file, buffer, bytesToWrite, bytesWritten, overlapped);
    if (result) {
        char path[MAX_PATH] = {};
        if (CopyAndMarkTrackedFileIo(file, true, path, sizeof(path))) {
            DWORD length = bytesWritten ? *bytesWritten : 0;
            LogFileIo("File written path=%s bytes=%lu handle=%p", path, length, file);
            LogFileContents("File written", path, buffer, length);
        }
    }
    return result;
}

// Forward CloseHandle and discard any diagnostic state for the closed handle.
BOOL WINAPI HookedCloseHandle(HANDLE handle) {
    BOOL result = g_originalCloseHandle(handle);
    if (result) {
        UntrackFileHandle(handle);
    }
    return result;
}

// These hooks only see calls game.exe makes directly through its own import
// table - which is all of them: its static CRT sends every malloc/free/new/
// delete to HeapAlloc/HeapFree/HeapReAlloc on its private heap.
// The default mode: only the padding, nothing tracked. Costs an addition per allocation.
bool g_heapPaddingOnlyInstalled = false;

// Time spent inside the heap hooks, measured only while the profiler window is shown so the
// profiler can say what the hooks themselves cost the game's main thread.
volatile LONG g_heapHookTicks = 0;   // QueryPerformanceCounter ticks
volatile LONG g_heapHookCalls = 0;
struct HeapHookTimer {
    LONGLONG start = 0;
    HeapHookTimer() {
        if (g_profilerRunning) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            start = now.QuadPart;
        }
    }
    ~HeapHookTimer() {
        if (start) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            InterlockedExchangeAdd(&g_heapHookTicks, static_cast<LONG>(now.QuadPart - start));
            InterlockedIncrement(&g_heapHookCalls);
        }
    }
};

LPVOID WINAPI PaddedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    HeapHookTimer timer;
    return g_originalHeapAlloc(heap, flags, size > 0x7FFFFF00 ? size : size + PaddingFor(size));
}

LPVOID WINAPI PaddedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size) {
    HeapHookTimer timer;
    return g_originalHeapReAlloc(heap, flags, pointer, size > 0x7FFFFF00 ? size : size + PaddingFor(size));
}

BOOL WINAPI HookedHeapFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    HeapHookTimer timer;
    try {
        if (g_heapFreeQuarantineInstalled && pointer && QuarantineHeapFree(heap, flags, pointer)) {
            return TRUE;
        }
    } catch (...) {
        // Out of memory in the bookkeeping: let the real free run, and release the lock if this thread holds it.
        while (g_quarantineLock.RecursionCount > 0 &&
                g_quarantineLock.OwningThread == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentThreadId()))) {
            LeaveCriticalSection(&g_quarantineLock);
        }
    }
    return g_originalHeapFree(heap, flags, pointer);
}

// Every allocation is recorded (start -> size) so a free of the middle of a block can be told from
// a free of a block's start; see g_liveBlocks.
LPVOID WINAPI HookedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    HeapHookTimer timer;
    if (!g_heapFreeQuarantineInstalled || size > 0x7FFFFF00) {
        return g_originalHeapAlloc(heap, flags, size);
    }
    // Asked for a little more, so an overrun of the block lands in guard bytes we can check.
    const DWORD padding = PaddingFor(size);
    LPVOID result = g_originalHeapAlloc(heap, flags, size + padding);
    if (!result) return NULL;
    DWORD callers[kAllocCallerSlots] = {};
    CaptureCallers(callers, kAllocCallerSlots);
    memset(static_cast<BYTE*>(result) + size, kCanaryFill, padding);
    const DWORD start = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(result));
    EnterCriticalSection(&g_quarantineLock);
    // The heap must never return memory that is still in use (only an exact repeat of a live address is
    // caught; overlaps in the middle needed an ordered table).
    if (LiveBlock* same = g_liveBlocks.Find(start)) {
        ReportOverlap(start, static_cast<DWORD>(size), callers, start, *same);
    }
    LiveBlock block = {};
    block.size = static_cast<DWORD>(size);
    block.heap = heap;
    block.tickMs = GetTickCount();
    memcpy(block.callers, callers, sizeof(callers));
    g_liveBlocks.Put(start, block);
    LeaveCriticalSection(&g_quarantineLock);
    return result;
}

LPVOID WINAPI HookedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID pointer, SIZE_T size) {
    HeapHookTimer timer;
    if (!g_heapFreeQuarantineInstalled || size > 0x7FFFFF00) {
        return g_originalHeapReAlloc(heap, flags, pointer, size);
    }
    if (pointer) {
        EnterCriticalSection(&g_quarantineLock);
        const DWORD oldStart = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer));
        LiveBlock* old = g_liveBlocks.Find(oldStart);
        BYTE found[kMaxCanaryBytes];
        if (old && CanaryDamaged(oldStart, old->size, found, false)) {
            ReportOverrun(oldStart, *old, found, "found when the game resized the block");
        }
        LeaveCriticalSection(&g_quarantineLock);
    }
    const DWORD padding = PaddingFor(size);
    LPVOID result = g_originalHeapReAlloc(heap, flags, pointer, size + padding);
    if (result) {
        DWORD callers[kAllocCallerSlots] = {};
        CaptureCallers(callers, kAllocCallerSlots);
        memset(static_cast<BYTE*>(result) + size, kCanaryFill, padding);
        EnterCriticalSection(&g_quarantineLock);
        if (pointer) g_liveBlocks.Erase(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pointer)));
        LiveBlock block = {};
        block.size = static_cast<DWORD>(size);
        block.heap = heap;
        block.tickMs = GetTickCount();
        memcpy(block.callers, callers, sizeof(callers));
        g_liveBlocks.Put(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(result)), block);
        LeaveCriticalSection(&g_quarantineLock);
    }
    return result;
}

BOOL WINAPI HookedHeapDestroy(HANDLE heap) {
    if (g_heapFreeQuarantineInstalled) {
        QuarantineForgetHeap(heap);
    }
    return g_originalHeapDestroy(heap);
}

// Overwrite one COM vtable slot process-wide (vtables are normally shared by
// every instance of a class) and return the previous function pointer, or
// NULL if the slot already holds newFunction. When alreadyPatched is given,
// it distinguishes "already patched by an earlier call" (not an error, e.g. a
// second surface sharing the same vtable) from a real VirtualProtect failure.
void* PatchVTableSlot(void* comObject, int slotIndex, void* newFunction,
        bool* alreadyPatched) {
    if (alreadyPatched) {
        *alreadyPatched = false;
    }
    if (!comObject) {
        return NULL;
    }
    void*** selfPtr = reinterpret_cast<void***>(comObject);
    void** vtable = *selfPtr;
    if (vtable[slotIndex] == newFunction) {
        if (alreadyPatched) {
            *alreadyPatched = true;
        }
        return NULL;
    }
    void* original = vtable[slotIndex];
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slotIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return NULL;
    }
    vtable[slotIndex] = newFunction;
    VirtualProtect(&vtable[slotIndex], sizeof(void*), oldProtect, &oldProtect);
    return original;
}

// Largest Blt destination area seen on the primary surface so far; used to
// tell a full-frame present apart from small partial updates (HUD, cursor).
static volatile LONG g_overlayFrameMaxBltArea = 0;

// Some game states (menus, alt-tab) can trigger more than one present-style
// call (Flip/Blt/BltFast) for what is visually the same single frame, which
// would otherwise double-count FPS. Real distinct frames can never arrive
// faster than the display's max refresh; treat anything within 2ms of the
// last counted frame as a duplicate signal for that same frame, not a new one.
static LARGE_INTEGER g_overlayPerfFrequency = {};
static LARGE_INTEGER g_overlayLastCountedFrameTime = {};
static const double OVERLAY_MIN_FRAME_INTERVAL_MS = 2.0;

static void CountPresentedFrame() {
    if (g_onMainThreadFrame) g_onMainThreadFrame();
    if (g_overlayPerfFrequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_overlayPerfFrequency);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_overlayLastCountedFrameTime.QuadPart != 0 && g_overlayPerfFrequency.QuadPart != 0) {
        double elapsedMs = static_cast<double>(now.QuadPart - g_overlayLastCountedFrameTime.QuadPart) *
            1000.0 / static_cast<double>(g_overlayPerfFrequency.QuadPart);
        if (elapsedMs < OVERLAY_MIN_FRAME_INTERVAL_MS) {
            return;
        }
        LONG slot = g_overlayFrameTimeNext;
        g_overlayFrameTimesMs[slot] = static_cast<float>(elapsedMs);
        g_overlayFrameTimeNext = (slot + 1) % OVERLAY_FRAME_TIME_CAPACITY;
        if (g_overlayFrameTimeCount < OVERLAY_FRAME_TIME_CAPACITY) {
            InterlockedIncrement(&g_overlayFrameTimeCount);
        }
    }
    g_overlayLastCountedFrameTime = now;
    g_profilerFrameThreadId = GetCurrentThreadId();
    InterlockedIncrement(&g_overlayFrameCount);
}

volatile LONG g_ddCalls[kDdKinds];
volatile LONG g_ddTicks[kDdKinds];      // QueryPerformanceCounter ticks
volatile LONG g_ddPixels[kDdKinds];     // area of the rectangles the game named (0 when it named none)
volatile LONG g_ddWholeSurface[kDdKinds]; // calls that named no rectangle at all (the whole surface)
// What a surface is, worked out once per surface pointer: "1920x1080 32bpp sys" (sys = system memory,
// vid = video memory, primary/back/tex/3d/plain from its caps). Slot 22 (GetSurfaceDesc) and the byte
// offsets of DDSURFACEDESC2 (height 8, width 12, pixel format bit count 84, caps 104) are as in the
// header note above.
volatile LONG g_ddInfoLock = 0;
static std::unordered_map<void*, std::string> g_ddSurfaceText;
std::map<std::string, DdPairStat> g_ddPairs;      // "BltFast 800x600 32bpp sys -> 1920x1080 32bpp primary"

std::string DescribeDdSurface(void* surface) {
    if (!surface) return "(none)";
    while (InterlockedCompareExchange(&g_ddInfoLock, 1, 0) != 0) Sleep(0);
    auto found = g_ddSurfaceText.find(surface);
    std::string text = found != g_ddSurfaceText.end() ? found->second : std::string();
    InterlockedExchange(&g_ddInfoLock, 0);
    if (!text.empty()) return text;
    text = "?";
    void** vtable = IsBadReadPtr(surface, sizeof(void*)) ? NULL : *reinterpret_cast<void***>(surface);
    if (vtable && !IsBadReadPtr(vtable + 22, sizeof(void*))) {
        BYTE desc[128] = {};
        const DWORD size = 124;
        memcpy(desc, &size, 4);
        typedef HRESULT (WINAPI *GetSurfaceDescFunction)(void*, void*);
        if (SUCCEEDED(reinterpret_cast<GetSurfaceDescFunction>(vtable[22])(surface, desc))) {
            DWORD flags = 0, height = 0, width = 0, bits = 0, caps = 0;
            memcpy(&flags, desc + 4, 4); memcpy(&height, desc + 8, 4); memcpy(&width, desc + 12, 4);
            memcpy(&bits, desc + 84, 4); memcpy(&caps, desc + 104, 4);
            char part[96];
            if (flags & 0x1000) snprintf(part, sizeof(part), "%lux%lu %lubpp", width, height, bits);   // DDSD_PIXELFORMAT
            else snprintf(part, sizeof(part), "%lux%lu", width, height);
            text = part;
            if (caps & 0x200) text += " primary";
            if (caps & 0x4) text += " back";
            if (caps & 0x800) text += " sys";
            if (caps & 0x4000) text += " vid";
            if (caps & 0x1000) text += " tex";
            if (caps & 0x2000) text += " 3d";
            if (caps & 0x40) text += " plain";
        }
    }
    while (InterlockedCompareExchange(&g_ddInfoLock, 1, 0) != 0) Sleep(0);
    if (g_ddSurfaceText.size() < 4096) g_ddSurfaceText[surface] = text;
    InterlockedExchange(&g_ddInfoLock, 0);
    return text;
}


// Count one presented frame for the overlay's FPS/frametime readout.
HRESULT WINAPI HookedDDFlip(void* self, void* targetOverride, DWORD flags) {
    DdTimer timer(kDdFlip, NULL, self, NULL);
    CountPresentedFrame();
    return g_originalDDFlip(self, targetOverride, flags);
}

// A NULL destRect conventionally means "blit the entire surface", which is
// only true for a full-frame present; otherwise only count a Blt as a frame
// once its area is close to the largest one seen (partial/HUD/cursor blits
// are typically much smaller than a full-frame redraw).
HRESULT WINAPI HookedDDBlt(void* self, LPRECT destRect, void* srcSurface,
        LPRECT srcRect, DWORD flags, void* bltFx) {
    DdTimer timer(kDdBlt, destRect, self, srcSurface, __builtin_return_address(0));
    if (g_uiQuadBlit && (UiQuadCursorSave(self, destRect, srcSurface, srcRect) || UiQuadBlt(self, destRect, srcSurface, srcRect, flags, bltFx))) return 0;
    if (!destRect) {
        CountPresentedFrame();
    } else {
        LONG area = (destRect->right - destRect->left) * (destRect->bottom - destRect->top);
        LONG maxArea = g_overlayFrameMaxBltArea;
        if (area > maxArea) {
            InterlockedExchange(&g_overlayFrameMaxBltArea, area);
            maxArea = area;
        }
        if (maxArea > 0 && area * 10 >= maxArea * 9) {
            CountPresentedFrame();
        }
    }
    return g_originalDDBlt(self, destRect, srcSurface, srcRect, flags, bltFx);
}

// BltFast has no destination rect, only an origin; a full-frame present
// conventionally targets (0,0), while sprite/HUD/cursor blits usually don't.
HRESULT WINAPI HookedDDBltFast(void* self, DWORD x, DWORD y, void* srcSurface,
        LPRECT srcRect, DWORD trans) {
    DdTimer timer(kDdBltFast, srcRect, self, srcSurface, __builtin_return_address(0));
    if (x == 0 && y == 0) {
        CountPresentedFrame();
    }
    return g_originalDDBltFast(self, x, y, srcSurface, srcRect, trans);
}

