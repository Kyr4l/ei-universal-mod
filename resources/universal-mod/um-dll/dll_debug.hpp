// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The DLL server's debugger (dll_server.hpp holds the connection and the command list): reading, searching
// and writing game.exe's memory, its modules, memory regions, threads and their stacks, and hardware
// breakpoints. Everything runs inside the game, on the server's thread (the breakpoints' handler on the
// thread that hits them), so it takes care never to crash it:
//  - memory is read and written with ReadProcessMemory / WriteProcessMemory on our own process, which fail
//    on a bad address instead of faulting;
//  - a thread is only suspended for as long as it takes to read its registers (or set its debug
//    registers), and nothing is allocated meanwhile: a suspended thread may hold the heap's lock, and the
//    server would then wait for it forever. For the same reason there is no "pause the game" command.
// Addresses are 32-bit (game.exe is a 32-bit program).
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace dlldebug {

// ---- text helpers -----------------------------------------------------------------------------------

inline std::string Hex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(v));
    return buf;
}

inline std::string HexBytes(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { out[i * 2] = digits[p[i] >> 4]; out[i * 2 + 1] = digits[p[i] & 15]; }
    return out;
}

// "..." with quotes, backslashes and bytes outside printable ASCII escaped (\xHH): one line, one word.
inline std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
        else { char buf[8]; snprintf(buf, sizeof(buf), "\\x%02X", c); out += buf; }
    }
    return out + "\"";
}

// A number: 0x... hexadecimal, else decimal (negative allowed).
inline bool ParseNumber(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (hex) out = static_cast<int64_t>(strtoull(s.c_str() + 2, &end, 16));
    else out = strtoll(s.c_str(), &end, 10);
    return end && *end == '\0';
}

// An address: hexadecimal with or without 0x ("4A1F20", "0x4A1F20"), or module+offset ("game.exe+0x1234",
// "um.dll+10").
inline bool ParseAddress(const std::string& s, uint32_t& out) {
    const size_t plus = s.find('+');
    uint32_t base = 0;
    std::string rest = s;
    if (plus != std::string::npos) {
        HMODULE m = GetModuleHandleA(s.substr(0, plus).c_str());
        if (!m) return false;
        base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
        rest = s.substr(plus + 1);
    }
    if (rest.size() > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X')) rest = rest.substr(2);
    if (rest.empty()) return false;
    char* end = nullptr;
    const unsigned long v = strtoul(rest.c_str(), &end, 16);
    if (!end || *end != '\0') return false;
    out = base + static_cast<uint32_t>(v);
    return true;
}

// ---- memory -----------------------------------------------------------------------------------------

// Reads what can be read from `address` on: the number of bytes read (fewer when a page on the way is
// not readable).
inline size_t ReadMemory(uint32_t address, void* out, size_t length) {
    SIZE_T done = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out, length, &done) && done == length)
        return length;
    // Page by page, up to the first one that fails.
    size_t total = 0;
    uint8_t* dst = static_cast<uint8_t*>(out);
    while (total < length) {
        const uint32_t at = address + static_cast<uint32_t>(total);
        const size_t chunk = std::min<size_t>(length - total, 0x1000 - (at & 0xFFF));
        done = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(at)), dst + total, chunk, &done) || done != chunk)
            break;
        total += chunk;
    }
    return total;
}

inline bool WriteMemory(uint32_t address, const void* data, size_t length, std::string& err) {
    LPVOID at = reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address));
    SIZE_T done = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), at, data, length, &done) || done != length) {
        // Read-only pages (code, constants): unprotect them for the write.
        DWORD old = 0;
        if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) { err = "not writable, and cannot be unprotected"; return false; }
        const BOOL ok = WriteProcessMemory(GetCurrentProcess(), at, data, length, &done);
        DWORD ignored = 0;
        VirtualProtect(at, length, old, &ignored);
        if (!ok || done != length) { err = "the write failed"; return false; }
    }
    FlushInstructionCache(GetCurrentProcess(), at, length);
    return true;
}

// The value types PEEK / POKE / FIND / SCAN / WATCH know.
enum class Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Ptr, Str, WStr, Bytes, Invalid };

inline Type ParseType(const std::string& s) {
    static const struct { const char* name; Type type; } names[] = {
        {"u8", Type::U8}, {"i8", Type::I8}, {"u16", Type::U16}, {"i16", Type::I16}, {"u32", Type::U32}, {"i32", Type::I32},
        {"u64", Type::U64}, {"i64", Type::I64}, {"f32", Type::F32}, {"f64", Type::F64}, {"ptr", Type::Ptr},
        {"str", Type::Str}, {"wstr", Type::WStr}, {"bytes", Type::Bytes}};
    for (const auto& n : names) if (s == n.name) return n.type;
    return Type::Invalid;
}

inline size_t TypeSize(Type t) {
    switch (t) {
    case Type::U8: case Type::I8: return 1;
    case Type::U16: case Type::I16: return 2;
    case Type::U32: case Type::I32: case Type::F32: case Type::Ptr: return 4;
    case Type::U64: case Type::I64: case Type::F64: return 8;
    default: return 0;
    }
}

inline bool IsFloat(Type t) { return t == Type::F32 || t == Type::F64; }

// Raw bytes (little-endian, TypeSize(t) of them) as text.
inline std::string FormatValue(Type t, uint64_t raw) {
    char buf[64];
    switch (t) {
    case Type::U8: snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(raw & 0xFF)); break;
    case Type::I8: snprintf(buf, sizeof(buf), "%d", static_cast<int>(static_cast<int8_t>(raw))); break;
    case Type::U16: snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(raw & 0xFFFF)); break;
    case Type::I16: snprintf(buf, sizeof(buf), "%d", static_cast<int>(static_cast<int16_t>(raw))); break;
    case Type::U32: snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(raw & 0xFFFFFFFF)); break;
    case Type::I32: snprintf(buf, sizeof(buf), "%ld", static_cast<long>(static_cast<int32_t>(raw))); break;
    case Type::Ptr: return Hex(static_cast<uint32_t>(raw));
    case Type::U64: snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(raw)); break;
    case Type::I64: snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(raw)); break;
    case Type::F32: { float f; uint32_t r = static_cast<uint32_t>(raw); memcpy(&f, &r, 4); snprintf(buf, sizeof(buf), "%.9g", f); break; }
    case Type::F64: { double d; memcpy(&d, &raw, 8); snprintf(buf, sizeof(buf), "%.17g", d); break; }
    default: return "?";
    }
    return buf;
}

// Text as the raw bytes of a value of type t.
inline bool EncodeValue(Type t, const std::string& text, uint64_t& raw) {
    raw = 0;
    if (t == Type::F32) { char* e; float f = strtof(text.c_str(), &e); if (*e) return false; uint32_t r; memcpy(&r, &f, 4); raw = r; return true; }
    if (t == Type::F64) { char* e; double d = strtod(text.c_str(), &e); if (*e) return false; memcpy(&raw, &d, 8); return true; }
    if (t == Type::Ptr) { uint32_t a; if (!ParseAddress(text, a)) return false; raw = a; return true; }
    int64_t v;
    if (!ParseNumber(text, v)) return false;
    raw = static_cast<uint64_t>(v);
    const size_t n = TypeSize(t);
    if (n && n < 8) raw &= (1ULL << (n * 8)) - 1;
    return n != 0;
}

inline double AsDouble(Type t, uint64_t raw) {
    switch (t) {
    case Type::I8: return static_cast<int8_t>(raw);
    case Type::I16: return static_cast<int16_t>(raw);
    case Type::I32: return static_cast<int32_t>(raw);
    case Type::I64: return static_cast<double>(static_cast<int64_t>(raw));
    case Type::F32: { float f; uint32_t r = static_cast<uint32_t>(raw); memcpy(&f, &r, 4); return f; }
    case Type::F64: { double d; memcpy(&d, &raw, 8); return d; }
    default: return static_cast<double>(raw);
    }
}

// "8B 45 ?? 0C" (or "8B45??0C"): the bytes and which of them must match.
inline bool ParsePattern(const std::string& text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    std::string digits;
    for (char c : text) if (c != ' ') digits += c;
    if (digits.empty() || digits.size() % 2) return false;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (digits[i] == '?' && digits[i + 1] == '?') { bytes.push_back(0); mask.push_back(0); continue; }
        char pair[3] = {digits[i], digits[i + 1], 0};
        char* end;
        const unsigned long v = strtoul(pair, &end, 16);
        if (*end) return false;
        bytes.push_back(static_cast<uint8_t>(v));
        mask.push_back(1);
    }
    return true;
}

// Readable committed memory, one region at a time. `writableOnly`: what the game can change (data, heaps,
// stacks), not code or read-only constants.
struct Region { uint32_t base, size; DWORD protect, type; };

inline bool Readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD p = protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
inline bool Writable(DWORD protect) {
    const DWORD p = protect & 0xFF;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

inline std::vector<Region> Regions(uint32_t start, uint32_t end, bool writableOnly) {
    std::vector<Region> out;
    uint32_t at = start;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(at)), &info, sizeof(info)) == 0) break;
        const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
        const uint32_t size = static_cast<uint32_t>(info.RegionSize);
        if (size == 0) break;
        if (info.State == MEM_COMMIT && Readable(info.Protect) && (!writableOnly || Writable(info.Protect))) {
            const uint32_t lo = std::max(base, start), hi = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(base) + size, end));
            if (hi > lo) out.push_back({lo, hi - lo, info.Protect, info.Type});
        }
        if (static_cast<uint64_t>(base) + size >= 0xFFFFFFFFull) break;
        at = base + size;
    }
    return out;
}

inline uint32_t HighestAddress() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress));
}

// ---- modules and symbols ----------------------------------------------------------------------------

struct Module { uint32_t base, size; std::string name, path; };

inline std::vector<Module> Modules() {
    std::vector<Module> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32 e;
    e.dwSize = sizeof(e);
    if (Module32First(snap, &e)) {
        do {
            out.push_back({static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e.modBaseAddr)), static_cast<uint32_t>(e.modBaseSize), e.szModule, e.szExePath});
        } while (Module32Next(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

// "game.exe+0x1234", "um.dll+0x50", or "" outside every module.
inline std::string ModuleOffset(uint32_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &info, sizeof(info)) == 0 || !info.AllocationBase || info.Type != MEM_IMAGE)
        return "";
    char path[MAX_PATH];
    if (GetModuleFileNameA(reinterpret_cast<HMODULE>(info.AllocationBase), path, sizeof(path)) == 0) return "";
    const char* slash = strrchr(path, '\\');
    char buf[MAX_PATH + 16];
    snprintf(buf, sizeof(buf), "%s+0x%lX", slash ? slash + 1 : path,
             static_cast<unsigned long>(address - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.AllocationBase))));
    return buf;
}

inline bool IsExecutable(uint32_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &info, sizeof(info)) == 0) return false;
    const DWORD p = info.Protect & 0xFF;
    return info.State == MEM_COMMIT && (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
}

// A return address: in code, just after a call (E8 rel32, FF /2 through a register or memory).
inline bool LooksLikeReturnAddress(uint32_t address) {
    if (!IsExecutable(address)) return false;
    uint8_t code[7];
    if (ReadMemory(address - 7, code, 7) != 7) return false;
    if (code[2] == 0xE8) return true;                                      // call rel32
    if (code[5] == 0xFF && ((code[6] >> 3) & 7) == 2) return true;        // call reg / [reg]
    if (code[4] == 0xFF && ((code[5] >> 3) & 7) == 2) return true;        // call [reg+disp8]
    if (code[1] == 0xFF && ((code[2] >> 3) & 7) == 2) return true;        // call [disp32] / [reg+disp32]
    if (code[3] == 0xFF && ((code[4] >> 3) & 7) == 2) return true;        // call [reg+reg*s+disp8]
    return false;
}

// ---- threads ----------------------------------------------------------------------------------------

inline std::vector<DWORD> ThreadIds() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 e;
    e.dwSize = sizeof(e);
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &e)) {
        do { if (e.th32OwnerProcessID == pid) out.push_back(e.th32ThreadID); } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

inline std::string ThreadName(HANDLE thread) {
    typedef HRESULT(WINAPI * GetDescription)(HANDLE, PWSTR*);
    static GetDescription get = reinterpret_cast<GetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription")));
    if (!get) return "";
    PWSTR text = nullptr;
    std::string out;
    if (SUCCEEDED(get(thread, &text)) && text) {
        char buf[256];
        if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf), nullptr, nullptr) > 0) out = buf;
        LocalFree(text);
    }
    return out;
}

inline uint32_t ThreadStartAddress(HANDLE thread) {
    typedef LONG(WINAPI * QueryThread)(HANDLE, int, PVOID, ULONG, PULONG);
    static QueryThread query = reinterpret_cast<QueryThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread")));
    if (!query) return 0;
    PVOID start = nullptr;
    if (query(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) != 0) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(start));
}

// A thread's registers and the top of its stack, read while it is suspended for a moment.
struct ThreadSnapshot {
    CONTEXT context;
    uint32_t stackBase = 0;
    size_t stackBytes = 0;
    uint8_t stack[16384];
};

inline bool SnapshotThread(DWORD tid, ThreadSnapshot& out, std::string& err) {
    if (tid == GetCurrentThreadId()) { err = "that is the DLL server's own thread"; return false; }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { err = "no such thread"; return false; }
    memset(&out.context, 0, sizeof(out.context));
    out.context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        // Nothing allocated from here to ResumeThread (the thread may hold the heap's lock).
        ok = GetThreadContext(thread, &out.context) != 0;
        if (ok) {
            out.stackBase = out.context.Esp;
            out.stackBytes = ReadMemory(out.context.Esp, out.stack, sizeof(out.stack));
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!ok) err = "cannot read the thread's registers";
    return ok;
}

// ---- hardware breakpoints ---------------------------------------------------------------------------
// The processor's 4 debug registers, set in every thread of the game: a hit raises a single-step exception
// in the thread that did it, which Handler records (registers, the top of the stack, the value) and lets go
// on. The game never stops: breakpoints report, they do not pause. New threads get them within a second
// (the server re-applies them).

enum class BpKind { Execute = 0, Write = 1, ReadWrite = 3 };

struct Breakpoint {
    volatile LONG active = 0;
    uint32_t address = 0;
    BpKind kind = BpKind::Execute;
    int length = 1;
    volatile LONG hits = 0;
};

struct Hit {
    volatile LONG seq;
    int slot;
    DWORD tid, eip, eax, ebx, ecx, edx, esi, edi, ebp, esp, eflags;
    DWORD stack[8];
    DWORD value; // the watched bytes after the access (data breakpoints)
};

struct Site { volatile LONG eip; volatile LONG count; }; // where the hits came from, and how often

const int kBpSlots = 4, kHitRing = 256, kSites = 64;
inline Breakpoint g_bp[kBpSlots];
inline Hit g_hits[kHitRing];
inline volatile LONG g_hitCount = 0;
inline Site g_sites[kBpSlots][kSites];
inline PVOID g_vehHandle = nullptr;

inline LONG CALLBACK BreakpointHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    int slot = -1;
    for (int i = 0; i < kBpSlots; ++i)
        if ((c->Dr6 & (1u << i)) && g_bp[i].active) { slot = i; break; }
    if (slot < 0) { // Dr6 not reported (some Wine versions): an execute breakpoint at this address
        for (int i = 0; i < kBpSlots; ++i)
            if (g_bp[i].active && g_bp[i].kind == BpKind::Execute && g_bp[i].address == c->Eip) { slot = i; break; }
    }
    if (slot < 0) return EXCEPTION_CONTINUE_SEARCH; // a single step that is not ours (a debugger's)
    const LONG n = InterlockedIncrement(&g_hitCount);
    InterlockedIncrement(&g_bp[slot].hits);
    Hit& h = g_hits[(n - 1) % kHitRing];
    h.seq = 0;
    h.slot = slot;
    h.tid = GetCurrentThreadId();
    h.eip = c->Eip; h.eax = c->Eax; h.ebx = c->Ebx; h.ecx = c->Ecx; h.edx = c->Edx;
    h.esi = c->Esi; h.edi = c->Edi; h.ebp = c->Ebp; h.esp = c->Esp; h.eflags = c->EFlags;
    for (int i = 0; i < 8; ++i) h.stack[i] = 0;
    ReadMemory(c->Esp, h.stack, sizeof(h.stack));
    h.value = 0;
    if (g_bp[slot].kind != BpKind::Execute) ReadMemory(g_bp[slot].address, &h.value, 4);
    InterlockedExchange(&h.seq, n);
    for (int i = 0; i < kSites; ++i) {
        Site& s = g_sites[slot][i];
        if (s.eip == static_cast<LONG>(c->Eip)) { InterlockedIncrement(&s.count); break; }
        if (s.eip == 0 && InterlockedCompareExchange(&s.eip, static_cast<LONG>(c->Eip), 0) == 0) { InterlockedIncrement(&s.count); break; }
    }
    c->Dr6 = 0;
    if (g_bp[slot].kind == BpKind::Execute) c->EFlags |= 0x10000; // RF: run the instruction without hitting again
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline DWORD Dr7() {
    DWORD dr7 = 0;
    for (int i = 0; i < kBpSlots; ++i) {
        if (!g_bp[i].active) continue;
        const DWORD len = g_bp[i].kind == BpKind::Execute ? 0 : g_bp[i].length == 2 ? 1 : g_bp[i].length == 4 ? 3 : 0;
        dr7 |= 1u << (i * 2);
        dr7 |= static_cast<DWORD>(g_bp[i].kind) << (16 + i * 4);
        dr7 |= len << (18 + i * 4);
    }
    return dr7;
}

// Sets the debug registers in these threads (not the calling one); the number of threads done.
inline int ApplyBreakpoints(const std::vector<DWORD>& tids) {
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
    const DWORD dr7 = Dr7();
    int done = 0;
    for (DWORD tid : tids) {
        if (tid == GetCurrentThreadId()) continue;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &c)) {
                c.Dr0 = g_bp[0].active ? g_bp[0].address : 0;
                c.Dr1 = g_bp[1].active ? g_bp[1].address : 0;
                c.Dr2 = g_bp[2].active ? g_bp[2].address : 0;
                c.Dr3 = g_bp[3].active ? g_bp[3].address : 0;
                c.Dr7 = dr7;
                c.Dr6 = 0;
                if (SetThreadContext(thread, &c)) ++done;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    return done;
}

inline bool AnyBreakpoint() {
    for (int i = 0; i < kBpSlots; ++i) if (g_bp[i].active) return true;
    return false;
}

} // namespace dlldebug
