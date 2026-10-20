// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the main-thread profiler and its window.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

char g_profileLines[kProfileMaxLines][kProfileTextWidth];
int g_profileLineCount = 0;
volatile LONG g_profileLock = 0;
static volatile LONG g_profilerThreadAlive = 0;
static HANDLE g_profilerStopEvent = NULL;
static std::vector<uint32_t> g_profileFunctions;   // sorted entry addresses of game.exe's functions
static std::unordered_map<DWORD, std::string> g_profileHints; // what an unnamed function refers to
static std::unordered_map<DWORD, std::string> g_profileNames;

struct ProfileNode {
    DWORD function;
    DWORD count;     // samples with this function on the stack below its parents
    DWORD self;      // samples where it was the innermost game function
    int firstChild;
    int nextSibling;
};
static const size_t kProfileMaxNodes = 20000;
static const DWORD kProfileStackBytes = 8192;

struct ProfileWindow {
    std::vector<ProfileNode> nodes; // [0] is the root
    DWORD samples = 0;
    std::vector<std::pair<std::string, DWORD>> categories; // where the innermost frame was: game.exe or a module
    std::vector<std::pair<std::string, DWORD>> externals;  // "module!export" of the innermost frame outside game.exe
    void Reset() {
        nodes.assign(1, ProfileNode{0, 0, 0, -1, -1});
        samples = 0;
        categories.clear();
        externals.clear();
    }
};

// Function entries and the names worked out from the code (see profile_symbols.hpp): classes' virtual
// methods, constructors, and hints from the strings and Windows APIs a function uses.
static void BuildProfileFunctionTable() {
    g_profileFunctions.clear();
    g_profileNames.clear();
    g_profileHints.clear();
    profsym::Image image;
    image.data = reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(g_imageLow));
    image.base = g_imageLow;
    image.size = g_imageHigh - g_imageLow;
    if (!image.Parse()) return;
    g_profileFunctions = profsym::FindFunctions(image);
    profsym::Symbols symbols = profsym::Analyze(image, g_profileFunctions);
    for (const auto& entry : symbols.names) g_profileNames[entry.first] = entry.second;
    for (const auto& entry : symbols.hints) g_profileHints[entry.first] = entry.second;
    LogLine("DEBUG", "[PERF] found %lu functions in game.exe: %lu named from %lu class vtables, %lu with a string or API hint",
        static_cast<unsigned long>(g_profileFunctions.size()), static_cast<unsigned long>(g_profileNames.size()),
        static_cast<unsigned long>(symbols.namedVtables), static_cast<unsigned long>(g_profileHints.size()));
}

// The optional um-names.txt next to um.dll: one "address name" pair per line, '#' starts a comment.
static void LoadProfileNames() {
    if (g_knownGameBuild) {
        // Identified by hand (docs/game-memory.md: units, orders, weapons, inventory, journal).
        static const struct { DWORD address; const char* name; } kKnown[] = {
            {0x457970, "MainMessageHandler"},
            {0x552300, "CUnitServer::Update"},               // the unit's per-frame update (vtable 6)
            {0x553610, "CUnitServer::SelectWeapon"},         // (slot 0-3 or -1), vtable 25
            {0x54A900, "CUnitServer::ApplyPendingOrder"},    // moves the pending order (+0x1B4) into the current one
            {0x54B390, "CUnitServer::RunPendingOrder"},      // calls ApplyPendingOrder (from Update)
            {0x554740, "CUnitServer::OrderMoveToPoint"},     // pending order type 1 with the point
            {0x54B320, "Order::Assign"},                     // copies an order (0x24 bytes)
            {0x545F40, "Order::Reset"},                      // type 9 = none
            {0x54B230, "Order::Clear"},
            {0x5D39E0, "MoveUnitInFormation"},               // a selected unit's point + its formation offset
            {0x5318B0, "Unit::ItemInWeaponSlot"},
            {0x5800C0, "Journal::Refresh"},                  // the journal's message indexes (0x7AFEC0...)
            // The CRT (MSVC 6 byte patterns): how much time a SIMD replacement could save (PENDING #80).
            {0x6E7960, "crt memcpy"},
            {0x6EC360, "crt memmove"},
            {0x6E9CA0, "crt memset"},
            {0x6ECC70, "crt strlen"},
            {0x6F0790, "crt strcmp"},
            {0x6E9D00, "crt strcpy/strcat"},
            {0x7005AF, "crt malloc"},
            {0x7005D8, "crt free"},
        };
        for (const auto& k : kKnown) g_profileNames[k.address] = k.name;
    }
    char path[MAX_PATH] = {};
    if (!g_dllModule || GetModuleFileNameA(g_dllModule, path, sizeof(path)) == 0) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, sizeof(path) - (slash + 1 - path), "um-names.txt");
    FILE* file = fopen(path, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char* text = line;
        while (*text == ' ' || *text == '\t') ++text;
        if (*text == '#' || *text == '\r' || *text == '\n' || *text == '\0') continue;
        char* end = NULL;
        unsigned long address = strtoul(text, &end, 16);
        if (end == text || address == 0) continue;
        while (*end == ' ' || *end == '\t') ++end;
        std::string name = end;
        size_t comment = name.find('#');
        if (comment != std::string::npos) name.resize(comment);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) name.pop_back();
        if (!name.empty()) g_profileNames[static_cast<DWORD>(address)] = name;
    }
    fclose(file);
}

// Builds the function table and names once, for the profiler and the DLL server (whichever asks first;
// the other waits). Read-only afterwards.
static volatile LONG g_profileSymbolsState = 0; // 0 not built, 1 building, 2 ready
void EnsureProfileSymbols() {
    if (g_profileSymbolsState == 2) return;
    if (InterlockedCompareExchange(&g_profileSymbolsState, 1, 0) == 0) {
        if (g_codeLow == 0) InitGameImageRanges(); // not done when the heap hooks are off
        BuildProfileFunctionTable();
        LoadProfileNames();
        InterlockedExchange(&g_profileSymbolsState, 2);
        return;
    }
    while (g_profileSymbolsState != 2) Sleep(10);
}

// The function a code address belongs to (its entry), 0 when there is none close enough.
DWORD ProfileFunctionOf(DWORD address) {
    auto after = std::upper_bound(g_profileFunctions.begin(), g_profileFunctions.end(), address);
    if (after == g_profileFunctions.begin()) return 0;
    DWORD entry = *(after - 1);
    return address - entry < 0x10000 ? entry : 0;
}

std::string ProfileNameOf(DWORD function) {
    auto known = g_profileNames.find(function);
    if (known != g_profileNames.end()) return known->second;
    char text[24];
    snprintf(text, sizeof(text), "sub_%lX", static_cast<unsigned long>(function));
    auto hint = g_profileHints.find(function);
    return hint != g_profileHints.end() ? std::string(text) + " " + hint->second : std::string(text);
}

// Which module an address is in: "game.exe" or the DLL's file name, and the name of the export it is
// in or just after (both cached per module).
struct ProfileModuleInfo {
    std::string name;
    profsym::ExportSymbols exports;
};
static std::string ProfileModuleOf(DWORD address, std::string* symbol) {
    static std::unordered_map<DWORD, ProfileModuleInfo> cache; // only the sampler thread calls this
    symbol->clear();
    if (address >= g_imageLow && address < g_imageHigh) return "game.exe";
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0 || !info.AllocationBase) return "other";
    const DWORD base = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(info.AllocationBase));
    auto found = cache.find(base);
    if (found == cache.end()) {
        ProfileModuleInfo module;
        module.name = "other";
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(info.AllocationBase), path, sizeof(path)) != 0) {
            const char* slash = strrchr(path, '\\');
            module.name = slash ? slash + 1 : path;
            const BYTE* header = reinterpret_cast<const BYTE*>(info.AllocationBase);
            if (!IsBadReadPtr(header, 0x200) && header[0] == 'M' && header[1] == 'Z') {
                DWORD lfanew = 0;
                memcpy(&lfanew, header + 0x3c, 4);
                DWORD imageSize = 0;
                if (lfanew < 0x1000 && !IsBadReadPtr(header + lfanew, 0x100)) memcpy(&imageSize, header + lfanew + 24 + 56, 4);
                if (imageSize >= 0x1000 && imageSize < 0x10000000) {
                    profsym::Image image;
                    image.data = header;
                    image.base = base;
                    image.size = imageSize;
                    module.exports.Load(image);
                }
            }
        }
        found = cache.emplace(base, std::move(module)).first;
    }
    // "!name+0x10" for an address in or just after an export, else "+0xRVA" (a DLL without exports, or an
    // internal function of one).
    const std::string described = found->second.exports.Describe(address - base);
    if (!described.empty()) {
        *symbol = "!" + described;
    } else {
        char offset[24];
        snprintf(offset, sizeof(offset), "+0x%lX", static_cast<unsigned long>(address - base));
        *symbol = offset;
    }
    return found->second.name;
}

// An import thunk or tail jump: what a direct call into another module goes through.
static bool ProfileIsThunk(DWORD address) {
    if (address < g_codeLow || address + 2 > g_codeHigh) return false;
    const BYTE* p = reinterpret_cast<const BYTE*>(address);
    return p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25);
}

// The call chain, innermost function first. A word on the stack that looks like a return address may
// be a leftover in a frame's uninitialised space, so a direct call is only believed when its target is at or
// just before the address of the frame found so far (the function it must have called), or is a thunk;
// indirect calls (virtual, through the import table) cannot be checked.
static int ProfileBuildPath(DWORD eip, const DWORD* stack, DWORD words, DWORD* path, int capacity) {
    int length = 0;
    DWORD inner = 0; // where the frame found so far is executing; 0 = the leaf is outside the game
    if (eip >= g_codeLow && eip < g_codeHigh) {
        DWORD function = ProfileFunctionOf(eip);
        if (function) { path[length++] = function; inner = eip; }
    }
    for (DWORD i = 0; i < words && length < capacity; ++i) {
        DWORD value = stack[i];
        if (value < g_codeLow + 6 || value >= g_codeHigh) continue;
        DWORD target = 0;
        if (!LooksLikeReturnAddress(reinterpret_cast<const BYTE*>(value), &target, true)) continue;
        if (target != 0 && !(target <= inner && inner - target < 0x8000) && !ProfileIsThunk(target)) continue; // stale
        DWORD function = ProfileFunctionOf(value - 1);
        if (!function) continue;
        if (length == 0 || path[length - 1] != function) path[length++] = function;
        inner = value;
    }
    return length;
}

static void ProfileAddSample(ProfileWindow& window, const DWORD* path, int length, const std::string& category,
        const std::string& external) {
    ++window.samples;
    if (!external.empty() && window.externals.size() < 400) {
        bool seen = false;
        for (auto& entry : window.externals) {
            if (entry.first == external) { ++entry.second; seen = true; break; }
        }
        if (!seen) window.externals.push_back(std::make_pair(external, 1u));
    }
    bool counted = false;
    for (auto& entry : window.categories) {
        if (entry.first == category) { ++entry.second; counted = true; break; }
    }
    if (!counted) window.categories.push_back(std::make_pair(category, 1u));
    int current = 0;
    ++window.nodes[0].count;
    for (int i = length - 1; i >= 0; --i) { // outermost first
        int child = window.nodes[current].firstChild;
        while (child >= 0 && window.nodes[child].function != path[i]) child = window.nodes[child].nextSibling;
        if (child < 0) {
            if (window.nodes.size() >= kProfileMaxNodes) break;
            window.nodes.push_back(ProfileNode{path[i], 0, 0, -1, window.nodes[current].firstChild});
            child = static_cast<int>(window.nodes.size()) - 1;
            window.nodes[current].firstChild = child;
        }
        ++window.nodes[child].count;
        current = child;
    }
    ++window.nodes[current].self;
}

static void ProfileEmit(const ProfileWindow& window, int parent, int depth, double frameMs,
        std::vector<std::string>& lines, size_t maxLines, DWORD minCount) {
    std::vector<int> children;
    for (int child = window.nodes[parent].firstChild; child >= 0; child = window.nodes[child].nextSibling) {
        if (window.nodes[child].count >= minCount) children.push_back(child);
    }
    std::sort(children.begin(), children.end(),
        [&](int a, int b) { return window.nodes[a].count > window.nodes[b].count; });
    for (int index : children) {
        if (lines.size() >= maxLines) return;
        const ProfileNode& node = window.nodes[index];
        // A frame that is on nearly every sample and does nothing itself (the loop that calls the
        // real work) is left out; its children are shown at the same depth.
        const bool wrapper = node.count * 100 >= window.samples * 97 && node.self * 100 <= window.samples * 3;
        if (!wrapper) {
            char text[kProfileTextWidth];
            const double share = static_cast<double>(node.count) / window.samples;
            std::string indent(static_cast<size_t>(depth > 10 ? 10 : depth) * 2, ' ');
            std::string name = ProfileNameOf(node.function);
            if (name.size() > 46) name.resize(46);
            if (frameMs > 0.0) {
                snprintf(text, sizeof(text), "%6.2f %s%s", share * frameMs, indent.c_str(), name.c_str());
            } else {
                snprintf(text, sizeof(text), "%5.1f%% %s%s", share * 100.0, indent.c_str(), name.c_str());
            }
            if (node.self * 50 >= window.samples) { // 2% or more spent in the function's own code
                size_t used = strlen(text);
                snprintf(text + used, sizeof(text) - used, frameMs > 0.0 ? "  (own %.2f)" : "  (own %.1f%%)",
                    frameMs > 0.0 ? static_cast<double>(node.self) / window.samples * frameMs
                                  : static_cast<double>(node.self) / window.samples * 100.0);
            }
            lines.push_back(text);
        }
        ProfileEmit(window, index, wrapper ? depth : depth + 1, frameMs, lines, maxLines, minCount);
    }
}

// The profiler's current tree appended to um-profile.txt (beside um.log), for reports (OVERLAY_PROFILER_DUMP_KEY).
void DumpProfile() {
    if (g_logPath[0] == '\0') { LogLine("WARN", "Profiler dump: no log folder known (logging is off)"); return; }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", g_logPath);
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? static_cast<size_t>(slash + 1 - path) : 0), "um-profile.txt");
    FILE* file = fopen(path, "a");
    if (!file) { LogLine("WARN", "Profiler dump: cannot write %s", path); return; }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "---- um.dll %s profile %04d-%02d-%02d %02d:%02d:%02d (%s)\n", UM_VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        g_profilerRunning ? "profiler running" : "profiler NOT running: open it first (OVERLAY_PROFILER_KEY) and wait a few seconds");
    while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
    for (int i = 0; i < g_profileLineCount; ++i) fprintf(file, "%s\n", g_profileLines[i]);
    const int count = g_profileLineCount;
    InterlockedExchange(&g_profileLock, 0);
    fprintf(file, "\n");
    fclose(file);
    LogLine("INFO", "Profiler dump: %d line(s) appended to %s", count, path);
}

static void PublishProfile(const std::vector<std::string>& lines) {
    while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
    g_profileLineCount = 0;
    for (const std::string& line : lines) {
        if (g_profileLineCount >= kProfileMaxLines) break;
        snprintf(g_profileLines[g_profileLineCount++], kProfileTextWidth, "%s", line.c_str());
    }
    InterlockedExchange(&g_profileLock, 0);
}

static DWORD WINAPI ProfilerThreadBody(LPVOID);
static DWORD WINAPI ProfilerThread(LPVOID parameter) {
    DWORD result = 0;
    try {
        result = ProfilerThreadBody(parameter);
    } catch (...) {
        g_profilerRunning = 0;
        InterlockedExchange(&g_profilerThreadAlive, 0);
    }
    return result;
}

static DWORD WINAPI ProfilerThreadBody(LPVOID) {
    LabelCurrentThread(L"um.dll: Profiler");
    EnsureProfileSymbols();
    static BYTE stackCopy[kProfileStackBytes];
    ProfileWindow window;
    window.Reset();
    HANDLE thread = NULL;
    DWORD threadId = 0;
    ULONGLONG windowStart = GetTickCount64();
    LONG windowFrames = g_overlayFrameCount;
    LARGE_INTEGER counterFrequency = {};
    QueryPerformanceFrequency(&counterFrequency);
    InterlockedExchange(&g_heapHookTicks, 0);
    InterlockedExchange(&g_heapHookCalls, 0);
    for (int k = 0; k < kDdKinds; ++k) { InterlockedExchange(&g_ddCalls[k], 0); InterlockedExchange(&g_ddTicks[k], 0); InterlockedExchange(&g_ddPixels[k], 0); InterlockedExchange(&g_ddWholeSurface[k], 0); }
    int snapshots = 0;
    const DWORD periodMs = g_profilerHz >= 1000 ? 1 : static_cast<DWORD>(1000 / (g_profilerHz > 0 ? g_profilerHz : 250));

    while (g_profilerRunning) {
        DWORD wanted = g_profilerFrameThreadId;
        if (wanted != 0 && wanted != threadId) {
            if (thread) CloseHandle(thread);
            thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, wanted);
            threadId = thread ? wanted : 0;
            window.Reset(); // measure from the moment there is a thread to look at
            windowStart = GetTickCount64();
            windowFrames = g_overlayFrameCount;
            InterlockedExchange(&g_heapHookTicks, 0);
            InterlockedExchange(&g_heapHookCalls, 0);
            for (int k = 0; k < kDdKinds; ++k) { InterlockedExchange(&g_ddCalls[k], 0); InterlockedExchange(&g_ddTicks[k], 0); InterlockedExchange(&g_ddPixels[k], 0); InterlockedExchange(&g_ddWholeSurface[k], 0); }
        }
        if (thread) {
            // Nothing between SuspendThread and ResumeThread may take a lock the game thread could hold.
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            SIZE_T copied = 0;
            bool have = false;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(thread, &context)) {
                    SIZE_T got = 0;
                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Esp), stackCopy, kProfileStackBytes, &got);
                    copied = got;
                    have = true;
                }
                ResumeThread(thread);
            }
            if (have) {
                DWORD path[64];
                int length = ProfileBuildPath(context.Eip, reinterpret_cast<const DWORD*>(stackCopy),
                    static_cast<DWORD>(copied / 4), path, 64);
                std::string symbol;
                std::string module = ProfileModuleOf(context.Eip, &symbol);
                // Outside the game, also who called in: the innermost game function on the stack.
                std::string external;
                if (module != "game.exe") {
                    external = module + symbol;
                    if (length > 0) external += " <- " + ProfileNameOf(path[0]);
                }
                ProfileAddSample(window, path, length, module, external);
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - windowStart >= 2000 && window.samples > 0) {
            const LONG frames = g_overlayFrameCount - windowFrames;
            const double frameMs = frames > 0 ? static_cast<double>(now - windowStart) / frames : 0.0;
            std::vector<std::string> lines;
            char text[kProfileTextWidth];
            if (frameMs > 0.0) {
                snprintf(text, sizeof(text), "%.1f ms/frame (%.0f FPS), %lu samples", frameMs, 1000.0 / frameMs,
                    static_cast<unsigned long>(window.samples));
            } else {
                snprintf(text, sizeof(text), "no frames presented, %lu samples", static_cast<unsigned long>(window.samples));
            }
            lines.push_back(text);
            std::sort(window.categories.begin(), window.categories.end(),
                [](const std::pair<std::string, DWORD>& a, const std::pair<std::string, DWORD>& b) { return a.second > b.second; });
            std::string where = "in:";
            for (size_t i = 0; i < window.categories.size() && i < 4; ++i) {
                char part[64];
                snprintf(part, sizeof(part), " %s %.0f%%", window.categories[i].first.c_str(),
                    window.categories[i].second * 100.0 / window.samples);
                where += part;
            }
            lines.push_back(where);
            const LONG hookTicks = InterlockedExchange(&g_heapHookTicks, 0);
            const LONG hookCalls = InterlockedExchange(&g_heapHookCalls, 0);
            if (hookCalls > 0 && frames > 0 && counterFrequency.QuadPart > 0) {
                const double hookMs = static_cast<double>(hookTicks) * 1000.0 / counterFrequency.QuadPart / frames;
                snprintf(text, sizeof(text), "um.dll heap hooks: %.0f calls, %.2f ms per frame (%.0f%%)",
                    static_cast<double>(hookCalls) / frames, hookMs, frameMs > 0.0 ? hookMs * 100.0 / frameMs : 0.0);
                lines.push_back(text);
            }
            if (frames > 0 && counterFrequency.QuadPart > 0) {
                static const char* const kinds[kDdKinds] = { "Flip", "Blt", "BltFast", "Lock" };
                std::string dd;
                for (int k = 0; k < kDdKinds; ++k) {
                    const LONG calls = InterlockedExchange(&g_ddCalls[k], 0);
                    const LONG ticks = InterlockedExchange(&g_ddTicks[k], 0);
                    const LONG pixels = InterlockedExchange(&g_ddPixels[k], 0);
                    const LONG whole = InterlockedExchange(&g_ddWholeSurface[k], 0);
                    if (calls <= 0) continue;
                    char part[110];
                    snprintf(part, sizeof(part), "%s%s %.1fx %.2f ms", dd.empty() ? "" : " | ", kinds[k],
                        static_cast<double>(calls) / frames, static_cast<double>(ticks) * 1000.0 / counterFrequency.QuadPart / frames);
                    dd += part;
                    if (pixels > 0) {
                        snprintf(part, sizeof(part), " %.2f Mpx", static_cast<double>(pixels) / frames / 1e6);
                        dd += part;
                    }
                    if (whole > 0) {
                        snprintf(part, sizeof(part), " (%.1f whole-surface)", static_cast<double>(whole) / frames);
                        dd += part;
                    }
                }
                if (!dd.empty()) lines.push_back("DirectDraw per frame: " + dd);
                if (g_shadowLockCache || g_uiBlitCache) {
                    const LONG sh = InterlockedExchange(&g_shadowStaged, 0), ui = InterlockedExchange(&g_uiStaged, 0);
                    snprintf(text, sizeof(text), "Staging per frame: %.1f shadow textures, %.1f interface pictures (copied on the CPU: the game's own are never GPU sources)", frames > 0 ? static_cast<double>(sh) / frames : 0.0, frames > 0 ? static_cast<double>(ui) / frames : 0.0);
                    lines.push_back(text);
                }
                if (g_uiQuadBlit) {
                    const LONG drawn = InterlockedExchange(&g_uiQuadDrawn, 0), fell = InterlockedExchange(&g_uiQuadFallback, 0);
                    snprintf(text, sizeof(text), "UI quads per frame: %.1f interface pictures drawn as quads, %.1f blitted the normal way", frames > 0 ? static_cast<double>(drawn) / frames : 0.0, frames > 0 ? static_cast<double>(fell) / frames : 0.0);
                    lines.push_back(text);
                }
                std::vector<std::pair<std::string, DdPairStat>> pairs;
                while (InterlockedCompareExchange(&g_ddInfoLock, 1, 0) != 0) Sleep(0);
                pairs.assign(g_ddPairs.begin(), g_ddPairs.end());
                g_ddPairs.clear();
                InterlockedExchange(&g_ddInfoLock, 0);
                std::sort(pairs.begin(), pairs.end(),
                    [](const std::pair<std::string, DdPairStat>& a, const std::pair<std::string, DdPairStat>& b) { return a.second.ticks > b.second.ticks; });
                for (size_t i = 0; i < pairs.size() && i < 5; ++i) {
                    const double ms = static_cast<double>(pairs[i].second.ticks) * 1000.0 / counterFrequency.QuadPart / frames;
                    if (ms < 0.05) break;
                    snprintf(text, sizeof(text), "  %.2f ms %.1fx %s", ms, static_cast<double>(pairs[i].second.calls) / frames, pairs[i].first.c_str());
                    lines.push_back(text);
                }
            }
            ProfileEmit(window, 0, 0, frameMs, lines, static_cast<size_t>(g_profilerLineCount) + lines.size(),
                window.samples / 100 > 0 ? window.samples / 100 : 1);
            if (!window.externals.empty() && frameMs > 0.0) {
                std::sort(window.externals.begin(), window.externals.end(),
                    [](const std::pair<std::string, DWORD>& a, const std::pair<std::string, DWORD>& b) { return a.second > b.second; });
                lines.push_back("outside game.exe (ms per frame):");
                for (size_t i = 0; i < window.externals.size() && i < 6; ++i) {
                    if (window.externals[i].second * 100 < window.samples) break; // under 1%
                    std::string name = window.externals[i].first;
                    if (name.size() > 70) name.resize(70);
                    snprintf(text, sizeof(text), "%6.2f  %s", static_cast<double>(window.externals[i].second) / window.samples * frameMs, name.c_str());
                    lines.push_back(text);
                }
            }
            PublishProfile(lines);
            if (g_profilerAutoDumpSeconds > 0) {
                static ULONGLONG lastAuto = 0;
                if (now - lastAuto >= static_cast<ULONGLONG>(g_profilerAutoDumpSeconds) * 1000) { lastAuto = now; DumpProfile(); }
            }
            if (++snapshots % 15 == 1) {
                for (const std::string& line : lines) LogLine("DEBUG", "[PERF] %s", line.c_str());
            }
            window.Reset();
            windowStart = now;
            windowFrames = g_overlayFrameCount;
            InterlockedExchange(&g_heapHookTicks, 0);
            InterlockedExchange(&g_heapHookCalls, 0);
            for (int k = 0; k < kDdKinds; ++k) { InterlockedExchange(&g_ddCalls[k], 0); InterlockedExchange(&g_ddTicks[k], 0); InterlockedExchange(&g_ddPixels[k], 0); InterlockedExchange(&g_ddWholeSurface[k], 0); }
        }
        WaitForSingleObject(g_profilerStopEvent, periodMs);
    }
    if (thread) CloseHandle(thread);
    InterlockedExchange(&g_profilerThreadAlive, 0);
    return 0;
}

// Called from the overlay's timer: the sampler runs only while its window is on screen.
void UpdateProfilerState() {
    const bool wanted = g_profilerEnabled && g_profilerVisible;
    if (wanted && !g_profilerRunning) {
        if (InterlockedCompareExchange(&g_profilerThreadAlive, 1, 0) != 0) return; // the last one is still ending
        if (!g_profilerStopEvent) g_profilerStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        ResetEvent(g_profilerStopEvent);
        g_profilerRunning = 1;
        HANDLE handle = CreateThread(NULL, 0, ProfilerThread, NULL, 0, NULL);
        if (handle) {
            CloseHandle(handle);
        } else {
            g_profilerRunning = 0;
            InterlockedExchange(&g_profilerThreadAlive, 0);
        }
    } else if (!wanted && g_profilerRunning) {
        g_profilerRunning = 0;
        if (g_profilerStopEvent) SetEvent(g_profilerStopEvent);
        while (InterlockedCompareExchange(&g_profileLock, 1, 0) != 0) Sleep(0);
        g_profileLineCount = 0;
        InterlockedExchange(&g_profileLock, 0);
    }
}



void AddOverlayLine(OverlayLine* lines, int* count, int capacity, bool alert,
        const char* format, ...) {
    if (*count >= capacity) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(lines[*count].text, sizeof(lines[*count].text), format, arguments);
    va_end(arguments);
    lines[*count].alert = alert;
    ++*count;
}

// 1% low FPS (average of the slowest 1% of frames) and worst frame time over
// the last OVERLAY_FRAME_STATS_WINDOW_MS; false until enough frames exist.
bool ComputeFrameStats(double* onePercentLowFps, double* worstFrameMs, double* windowSeconds) {
    static float window[OVERLAY_FRAME_TIME_CAPACITY]; // only the overlay thread calls this
    LONG count = g_overlayFrameTimeCount;
    LONG next = g_overlayFrameTimeNext;
    int used = 0;
    double totalMs = 0.0;
    double worst = 0.0;
    for (LONG i = 0; i < count && totalMs < OVERLAY_FRAME_STATS_WINDOW_MS; ++i) {
        float frameMs = g_overlayFrameTimesMs[(next - 1 - i + OVERLAY_FRAME_TIME_CAPACITY) % OVERLAY_FRAME_TIME_CAPACITY];
        window[used++] = frameMs;
        totalMs += frameMs;
        if (frameMs > worst) {
            worst = frameMs;
        }
    }
    if (used < 10) {
        return false;
    }
    int slowest = used / 100 > 1 ? used / 100 : 1;
    std::nth_element(window, window + (slowest - 1), window + used,
        [](float a, float b) { return a > b; });
    double sumMs = 0.0;
    for (int i = 0; i < slowest; ++i) {
        sumMs += window[i];
    }
    *onePercentLowFps = sumMs > 0.0 ? 1000.0 * slowest / sumMs : 0.0;
    *worstFrameMs = worst;
    *windowSeconds = totalMs / 1000.0;
    return true;
}

ULONGLONG GetProcessUptimeSeconds() {
    FILETIME creation = {}, exitTime = {}, kernel = {}, user = {}, now = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        return 0;
    }
    GetSystemTimeAsFileTime(&now);
    ULONGLONG created = (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    ULONGLONG current = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return current > created ? (current - created) / 10000000ULL : 0;
}

// "2m14s" under an hour, "1h05m" beyond.
void FormatDuration(ULONGLONG seconds, char* out, size_t outSize) {
    if (seconds >= 3600) {
        snprintf(out, outSize, "%luh%02lum", static_cast<unsigned long>(seconds / 3600),
            static_cast<unsigned long>((seconds % 3600) / 60));
    } else {
        snprintf(out, outSize, "%lum%02lus", static_cast<unsigned long>(seconds / 60),
            static_cast<unsigned long>(seconds % 60));
    }
}

