// The DLL server: a TCP server on 127.0.0.1 (DLL_SERVER_PORT, 18888 by default) that um-multitool's
// "UM DLL Connector" tab (and `um-multitool dll`) connects to while the game runs. Off unless
// DLL_SERVER_ENABLED=true in um.cfg. Only this computer can connect (the socket is bound to the loopback
// address). It reports statistics, and is a debugger for the game (dll_debug.hpp): what reads is always
// on; what changes the game (WRITE, POKE, breakpoints) needs DLL_SERVER_DEBUG=true as well.
//
// The protocol is text, one message per line ('\n'), words separated by spaces, values as key=value (text
// that may hold spaces is quoted, "...", with \" \\ \xHH escapes). A command is answered by zero or more
// "ROW <COMMAND> ..." lines and then "OK <COMMAND> ..." or "ERR <COMMAND> <reason>". Lines starting with
// another word are events, sent whenever they happen:
//   HELLO um.dll version=1.2 proto=2 pid=1234 debug=on      on connecting
//   STATS cpu=12.5 cores=8 ws_mb=... private_mb=... peak_ws_mb=... vas_used_mb=... vas_total_mb=...
//         threads=23 sys_ram_mb=... sys_ram_free_mb=... uptime_s=120      every second
//   HIT slot=0 tid=... eip=... value=... eax=... ... stack=... sym="..."  a breakpoint was hit
//   WATCHED id=1 addr=... value=... old=...                   a watched value changed
//   LOG "<um.log line>"                                        after SUB log on
// Numbers are decimal unless written 0x...; addresses are hexadecimal (0x optional) or module+offset
// (game.exe+0x1234). HELP lists the commands.
//
// Included by um.cpp after its settings and LogLine, and before InitializeDllThread starts it.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "dll_debug.hpp"
#include "dll_game.hpp"

namespace dllserver {

using namespace dlldebug;

static const int kProtocol = 2;
static const int kMaxClients = 4;

// What um.cpp gives the server.
struct Host {
    const char* version = "";
    bool debugAllowed = false;                       // DLL_SERVER_DEBUG
    std::string (*mapInfo)() = nullptr;              // the map the game shows, as key=value words
    std::string (*gameFunction)(uint32_t) = nullptr; // "sub_4A1F20+0x12" (or a known name) in game.exe, else ""
    uint32_t (*mainThread)() = nullptr;              // the thread that draws the frames, 0 when not known
    void (*log)(const char* level, const char* message) = nullptr;
};

// ---- um.log lines, for the clients that asked (SUB log on) ------------------------------------------
// Filled by LogLine through NoteLog without allocating (it may run anywhere, even in a heap hook).
static const int kLogRing = 256;
static char g_logLines[kLogRing][512];
static volatile LONG g_logSeq = 0, g_logLock = 0, g_running = 0;

inline void NoteLog(const char* line) {
    if (!g_running) return;
    while (InterlockedCompareExchange(&g_logLock, 1, 0) != 0) Sleep(0);
    snprintf(g_logLines[g_logSeq % kLogRing], sizeof(g_logLines[0]), "%s", line);
    InterlockedIncrement(&g_logSeq);
    InterlockedExchange(&g_logLock, 0);
}

// ---- statistics ---------------------------------------------------------------------------------------

// CPU use since the previous call, for the whole process, normalized against every logical processor
// (Task Manager's convention: 100 = all cores busy).
struct CpuSampler {
    ULONGLONG lastWallMs = 0, lastCpu100ns = 0;
    double percent = 0.0;
    void Sample() {
        FILETIME creation, exitTime, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) return;
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        const ULONGLONG cpu = k.QuadPart + u.QuadPart, now = GetTickCount64();
        if (lastWallMs && now > lastWallMs) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            const double cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
            percent = (static_cast<double>(cpu - lastCpu100ns) / 10000.0) / static_cast<double>(now - lastWallMs) * 100.0 / cores;
        }
        lastWallMs = now;
        lastCpu100ns = cpu;
    }
};

// Seconds since game.exe started.
inline unsigned long long ProcessUptime() {
    FILETIME creation, exitTime, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = creation.dwLowDateTime; a.HighPart = creation.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000000ULL : 0;
}

inline std::string StatsLine(CpuSampler& cpu) {
    cpu.Sample();
    const double mb = 1024.0 * 1024.0;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    char line[512];
    snprintf(line, sizeof(line),
             "STATS cpu=%.1f cores=%lu ws_mb=%.1f private_mb=%.1f peak_ws_mb=%.1f vas_used_mb=%.1f vas_total_mb=%.1f"
             " threads=%d sys_ram_mb=%.1f sys_ram_free_mb=%.1f uptime_s=%llu\n",
             cpu.percent, static_cast<unsigned long>(si.dwNumberOfProcessors), pmc.WorkingSetSize / mb,
             (pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage) / mb, // Wine leaves PrivateUsage at 0
             pmc.PeakWorkingSetSize / mb, (ms.ullTotalVirtual - ms.ullAvailVirtual) / mb, ms.ullTotalVirtual / mb,
             static_cast<int>(ThreadIds().size()), ms.ullTotalPhys / mb, ms.ullAvailPhys / mb, ProcessUptime());
    return line;
}

// ---- clients ------------------------------------------------------------------------------------------

struct Watch { int id; uint32_t address; Type type; DWORD intervalMs; ULONGLONG nextMs; uint64_t last; bool known; };

struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string input;   // bytes received, up to the next '\n'
    std::string output;  // what is to be sent: answers and events
    bool logEvents = false, hitEvents = true;
    LONG logSeq = 0, hitSeq = 0;
    std::vector<Watch> watches;
    int nextWatchId = 1;
    // SCAN / NEXT: the candidate addresses and their values at the last pass.
    Type scanType = Type::Invalid;
    std::vector<uint32_t> scanAddresses;
    std::vector<uint64_t> scanValues;
    bool scanTruncated = false;
};

inline bool SendAll(SOCKET s, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
        const int n = send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Words of a command line; "..." keeps spaces (with \" \\ \xHH escapes).
inline std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        std::string word;
        if (line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                    if (line[i] == 'x' && i + 2 < line.size()) { word += static_cast<char>(strtoul(line.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
                    else word += line[i];
                } else {
                    word += line[i];
                }
            }
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ') word += line[i++];
        }
        out.push_back(word);
    }
    return out;
}

// "key=value" options after the positional words.
inline bool Option(const std::vector<std::string>& w, const char* key, std::string& value) {
    const std::string prefix = std::string(key) + "=";
    for (const std::string& s : w)
        if (s.compare(0, prefix.size(), prefix) == 0) { value = s.substr(prefix.size()); return true; }
    return false;
}
inline bool Flag(const std::vector<std::string>& w, const char* name) {
    for (const std::string& s : w) if (s == name) return true;
    return false;
}

class Server {
public:
    explicit Server(const Host& host) : host_(host) {}
    void Run(int port);

private:
    const Host& host_;
    std::vector<Client> clients_;
    CpuSampler cpu_;
    std::unordered_set<DWORD> bpThreads_; // the threads that have the breakpoints
    dllgame::UnitScanner scanner_;

    std::string Where(uint32_t address) const {
        std::string where = ModuleOffset(address);
        if (host_.gameFunction) {
            const std::string f = host_.gameFunction(address);
            if (!f.empty()) where += where.empty() ? f : " " + f;
        }
        return where;
    }
    void Log(const char* level, const std::string& message) { if (host_.log) host_.log(level, message.c_str()); }
    void Row(Client& c, const std::string& command, const std::string& text) { c.output += "ROW " + command + " " + text + "\n"; }
    void Ok(Client& c, const std::string& command, const std::string& text = "") { c.output += "OK " + command + (text.empty() ? "" : " " + text) + "\n"; }
    void Err(Client& c, const std::string& command, const std::string& why) { c.output += "ERR " + command + " " + why + "\n"; }

    bool Handle(Client& c, const std::string& line);
    void Help(Client& c);
    void Info(Client& c);
    void ModulesCommand(Client& c);
    void RegionsCommand(Client& c, const std::vector<std::string>& w);
    void Read(Client& c, const std::vector<std::string>& w, bool dump);
    void Peek(Client& c, const std::vector<std::string>& w);
    void Pointer(Client& c, const std::vector<std::string>& w);
    void Find(Client& c, const std::vector<std::string>& w);
    void Scan(Client& c, const std::vector<std::string>& w);
    void Next(Client& c, const std::vector<std::string>& w);
    void List(Client& c, const std::vector<std::string>& w);
    void Threads(Client& c);
    void Stack(Client& c, const std::vector<std::string>& w);
    void WatchCommand(Client& c, const std::vector<std::string>& w);
    void Breakpoints(Client& c, const std::vector<std::string>& w);
    void Write(Client& c, const std::vector<std::string>& w, bool typed);
    void SendEvents(Client& c);
    void Units(Client& c, const std::vector<std::string>& w);
    void ConsoleCommand(Client& c, const std::vector<std::string>& w);
    void RefreshBreakpointThreads(bool all);
};

inline void Server::Help(Client& c) {
    static const char* const lines[] = {
        "HELP                                  this list",
        "PING                                  answers OK PING",
        "INFO                                  game.exe: path, base, size, build stamp, LAA, main thread",
        "MAP                                   the map the game shows",
        "MODULES                               loaded modules: base, size, name, path",
        "REGIONS [from] [to] [writable]        committed readable memory: base, size, protection, type, module",
        "SYM addr                              module+offset and game.exe function of an address",
        "READ addr len                         up to 65536 bytes as hex (fewer when unreadable)",
        "DUMP addr len                         up to 4096 bytes, 16 a row, hex and text",
        "PEEK addr type [count]                typed values (u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr str wstr)",
        "PTR base off1 off2 ...                follows a pointer chain: [base]+off1, [that]+off2...",
        "FIND type value [from=] [to=] [max=] [writable]   addresses holding a value (type bytes: \"8B 45 ?? 0C\"; str / wstr: text)",
        "SCAN type value|any [from=] [to=]     starts a scan of writable memory (any: every address, needs from= to=)",
        "NEXT eq|ne|gt|lt v | changed|unchanged|inc|dec | incby|decby v   narrows the scan",
        "LIST [max=]                           the scan's addresses and current values",
        "THREADS                               threads: id, name, start address, CPU time, main",
        "STACK tid [depth]                     a thread's registers and call stack (return addresses)",
        "WATCH addr type [ms]                  reports the value when it changes (WATCHED events); WATCH list; UNWATCH id|all",
        "SUB log on|off / SUB hits on|off      um.log lines (LOG events); breakpoint hits (HIT events, on by default)",
        "BP set slot addr x|w|rw [len]         hardware breakpoint 0-3 (execute, write, read/write; len 1 2 4)  [debug]",
        "BP clear slot|all / BP list / BP sites slot   remove, list, where the hits came from (address, count)",
        "                                      (a data breakpoint reports the instruction AFTER the access; hits never pause the game)",
        "WRITE addr hexbytes                   writes bytes  [debug]",
        "POKE addr type value                  writes a typed value  [debug]",
        "UNITS [rescan]                        the game's units: id, position, facing (yaw), side, HP, mana, flags, sight, view angle, name (players)",
        "CAMERA                                the game camera: position, rotation quaternion, the point it aims at",
        "CONSOLE lines [from] [max]            the in-game console's lines (from index from, default 0), and its input line",
        "CONSOLE send text                     types the text in the game window and presses Enter; refused while the console is closed  [debug]",
        "BYE                                   closes the connection",
        "[debug]: needs DLL_SERVER_DEBUG=true in um.cfg",
    };
    for (const char* l : lines) Row(c, "HELP", Quote(l));
    Ok(c, "HELP");
}

inline void Server::Info(Client& c) {
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint32_t b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
    char text[1024];
    snprintf(text, sizeof(text), "pid=%lu base=%s size=0x%lX entry=%s stamp=0x%08lX laa=%s main_tid=%lu server_tid=%lu debug=%s path=%s",
             static_cast<unsigned long>(GetCurrentProcessId()), Hex(b).c_str(), static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage),
             Hex(b + nt->OptionalHeader.AddressOfEntryPoint).c_str(), static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
             (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "yes" : "no",
             static_cast<unsigned long>(host_.mainThread ? host_.mainThread() : 0), static_cast<unsigned long>(GetCurrentThreadId()),
             host_.debugAllowed ? "on" : "off", Quote(exe).c_str());
    Ok(c, "INFO", text);
}

inline void Server::ModulesCommand(Client& c) {
    const std::vector<Module> mods = Modules();
    for (const Module& m : mods) {
        char text[64];
        snprintf(text, sizeof(text), "base=%s size=0x%lX ", Hex(m.base).c_str(), static_cast<unsigned long>(m.size));
        Row(c, "MODULES", text + std::string("name=") + Quote(m.name) + " path=" + Quote(m.path));
    }
    Ok(c, "MODULES", "count=" + std::to_string(mods.size()));
}

inline void Server::RegionsCommand(Client& c, const std::vector<std::string>& w) {
    uint32_t from = 0x10000, to = HighestAddress();
    if (w.size() > 1 && w[1] != "writable" && !ParseAddress(w[1], from)) return Err(c, "REGIONS", "bad address " + w[1]);
    if (w.size() > 2 && w[2] != "writable" && !ParseAddress(w[2], to)) return Err(c, "REGIONS", "bad address " + w[2]);
    const std::vector<Region> regions = Regions(from, to, Flag(w, "writable"));
    for (const Region& r : regions) {
        char text[160];
        snprintf(text, sizeof(text), "base=%s size=0x%lX protect=0x%lX type=%s", Hex(r.base).c_str(), static_cast<unsigned long>(r.size),
                 static_cast<unsigned long>(r.protect), r.type == MEM_IMAGE ? "image" : r.type == MEM_MAPPED ? "mapped" : "private");
        const std::string where = ModuleOffset(r.base);
        Row(c, "REGIONS", text + (where.empty() ? std::string() : " module=" + Quote(where)));
    }
    Ok(c, "REGIONS", "count=" + std::to_string(regions.size()));
}

inline void Server::Read(Client& c, const std::vector<std::string>& w, bool dump) {
    const char* cmd = dump ? "DUMP" : "READ";
    uint32_t address;
    int64_t length;
    if (w.size() < 3 || !ParseAddress(w[1], address) || !ParseNumber(w[2], length)) return Err(c, cmd, "usage: " + std::string(cmd) + " addr len");
    const int64_t limit = dump ? 4096 : 65536;
    if (length <= 0 || length > limit) return Err(c, cmd, "len must be 1-" + std::to_string(limit));
    std::vector<uint8_t> buf(static_cast<size_t>(length));
    const size_t got = ReadMemory(address, buf.data(), buf.size());
    if (got == 0) return Err(c, cmd, "not readable at " + Hex(address));
    if (!dump) return Ok(c, cmd, "addr=" + Hex(address) + " len=" + std::to_string(got) + " hex=" + HexBytes(buf.data(), got));
    for (size_t i = 0; i < got; i += 16) {
        const size_t n = std::min<size_t>(16, got - i);
        std::string hex, text;
        for (size_t k = 0; k < 16; ++k) {
            if (k < n) { char b[4]; snprintf(b, sizeof(b), "%02X ", buf[i + k]); hex += b; }
            else hex += "   ";
            if (k < n) text += (buf[i + k] >= 0x20 && buf[i + k] < 0x7F) ? static_cast<char>(buf[i + k]) : '.';
        }
        Row(c, cmd, Hex(address + static_cast<uint32_t>(i)) + " " + hex + Quote(text));
    }
    Ok(c, cmd, "addr=" + Hex(address) + " len=" + std::to_string(got));
}

inline void Server::Peek(Client& c, const std::vector<std::string>& w) {
    uint32_t address;
    if (w.size() < 3 || !ParseAddress(w[1], address)) return Err(c, "PEEK", "usage: PEEK addr type [count]");
    const Type t = ParseType(w[2]);
    if (t == Type::Invalid || t == Type::Bytes) return Err(c, "PEEK", "unknown type " + w[2]);
    if (t == Type::Str || t == Type::WStr) {
        uint8_t buf[1024];
        const size_t got = ReadMemory(address, buf, sizeof(buf));
        if (!got) return Err(c, "PEEK", "not readable at " + Hex(address));
        std::string text;
        if (t == Type::Str) {
            for (size_t i = 0; i < got && buf[i]; ++i) text += static_cast<char>(buf[i]);
        } else {
            for (size_t i = 0; i + 1 < got; i += 2) {
                const unsigned ch = buf[i] | (buf[i + 1] << 8);
                if (!ch) break;
                text += ch < 0x80 ? static_cast<char>(ch) : '?';
            }
        }
        return Ok(c, "PEEK", "addr=" + Hex(address) + " type=" + w[2] + " len=" + std::to_string(text.size()) + " value=" + Quote(text));
    }
    int64_t count = 1;
    if (w.size() > 3 && (!ParseNumber(w[3], count) || count < 1 || count > 256)) return Err(c, "PEEK", "count must be 1-256");
    const size_t size = TypeSize(t);
    std::string values;
    for (int64_t i = 0; i < count; ++i) {
        uint64_t raw = 0;
        if (ReadMemory(address + static_cast<uint32_t>(i * size), &raw, size) != size) {
            if (i == 0) return Err(c, "PEEK", "not readable at " + Hex(address));
            break;
        }
        values += (i ? "," : "") + FormatValue(t, raw);
    }
    Ok(c, "PEEK", "addr=" + Hex(address) + " type=" + w[2] + " value=" + values);
}

inline void Server::Pointer(Client& c, const std::vector<std::string>& w) {
    uint32_t address;
    if (w.size() < 2 || !ParseAddress(w[1], address)) return Err(c, "PTR", "usage: PTR base off1 off2 ...");
    for (size_t i = 2; i < w.size(); ++i) {
        uint32_t value = 0;
        if (ReadMemory(address, &value, 4) != 4) return Err(c, "PTR", "not readable at " + Hex(address) + " (step " + std::to_string(i - 1) + ")");
        int64_t offset;
        if (!ParseNumber(w[i], offset)) return Err(c, "PTR", "bad offset " + w[i] + " (decimal, or 0x... hexadecimal)");
        Row(c, "PTR", "[" + Hex(address) + "]=" + Hex(value) + " +" + w[i]);
        address = value + static_cast<uint32_t>(offset);
    }
    uint32_t value = 0;
    const bool readable = ReadMemory(address, &value, 4) == 4;
    Ok(c, "PTR", "addr=" + Hex(address) + (readable ? " u32=" + std::to_string(value) + " hex=" + Hex(value) : " unreadable"));
}

// The bytes FIND and SCAN look for.
inline bool NeedleOf(Type t, const std::string& text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    if (t == Type::Bytes) return ParsePattern(text, bytes, mask);
    if (t == Type::Str) { bytes.assign(text.begin(), text.end()); mask.assign(bytes.size(), 1); return !bytes.empty(); }
    if (t == Type::WStr) {
        for (unsigned char ch : text) { bytes.push_back(ch); bytes.push_back(0); }
        mask.assign(bytes.size(), 1);
        return !bytes.empty();
    }
    uint64_t raw;
    if (!EncodeValue(t, text, raw)) return false;
    const size_t n = TypeSize(t);
    bytes.assign(reinterpret_cast<uint8_t*>(&raw), reinterpret_cast<uint8_t*>(&raw) + n);
    mask.assign(n, 1);
    return true;
}

// Calls found(address) for each place in these regions holding the needle (at multiples of `align`);
// found returns false to stop.
template <typename Found>
inline void SearchRegions(const std::vector<Region>& regions, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& mask,
                          size_t align, Found found) {
    std::vector<uint8_t> buf(1 << 20);
    const size_t n = bytes.size();
    // Chunks overlap by the needle's length (rounded to `align`) so a match across two is found.
    const size_t overlap = ((n - 1 + align - 1) / align) * align;
    for (const Region& r : regions) {
        for (uint32_t at = r.base; at < r.base + r.size;) {
            const size_t want = std::min<size_t>(buf.size(), r.base + r.size - at);
            const size_t got = ReadMemory(at, buf.data(), want);
            if (got < n) break;
            for (size_t i = 0; i + n <= got; i += align) {
                bool match = true;
                for (size_t k = 0; k < n && match; ++k) match = !mask[k] || buf[i + k] == bytes[k];
                if (match && !found(at + static_cast<uint32_t>(i))) return;
            }
            if (got < want || got <= overlap) break;
            at += static_cast<uint32_t>(got - overlap);
        }
    }
}

inline void Server::Find(Client& c, const std::vector<std::string>& w) {
    if (w.size() < 3) return Err(c, "FIND", "usage: FIND type value [from=] [to=] [max=] [writable]");
    const Type t = ParseType(w[1]);
    if (t == Type::Invalid) return Err(c, "FIND", "unknown type " + w[1]);
    std::vector<uint8_t> bytes, mask;
    if (!NeedleOf(t, w[2], bytes, mask)) return Err(c, "FIND", "bad value " + w[2]);
    uint32_t from = 0x10000, to = HighestAddress();
    std::string v;
    if (Option(w, "from", v) && !ParseAddress(v, from)) return Err(c, "FIND", "bad from=");
    if (Option(w, "to", v) && !ParseAddress(v, to)) return Err(c, "FIND", "bad to=");
    int64_t max = 200;
    if (Option(w, "max", v) && (!ParseNumber(v, max) || max < 1 || max > 10000)) return Err(c, "FIND", "max must be 1-10000");
    const size_t align = (t == Type::Bytes || t == Type::Str || t == Type::WStr) ? 1 : std::min<size_t>(TypeSize(t), 4);
    int64_t count = 0;
    SearchRegions(Regions(from, to, Flag(w, "writable")), bytes, mask, align, [&](uint32_t a) {
        const std::string where = ModuleOffset(a);
        Row(c, "FIND", "addr=" + Hex(a) + (where.empty() ? "" : " module=" + Quote(where)));
        return ++count < max;
    });
    Ok(c, "FIND", "count=" + std::to_string(count) + (count >= max ? " more=maybe" : ""));
}

static const size_t kScanMax = 1000000; // candidates kept (12 bytes each, in the game's own memory)

inline void Server::Scan(Client& c, const std::vector<std::string>& w) {
    if (w.size() < 3) return Err(c, "SCAN", "usage: SCAN type value|any [from=] [to=]");
    const Type t = ParseType(w[1]);
    if (TypeSize(t) == 0) return Err(c, "SCAN", "SCAN takes a number type (u8 ... f64)");
    uint32_t from = 0x10000, to = HighestAddress();
    std::string v;
    if (Option(w, "from", v) && !ParseAddress(v, from)) return Err(c, "SCAN", "bad from=");
    if (Option(w, "to", v) && !ParseAddress(v, to)) return Err(c, "SCAN", "bad to=");
    c.scanType = t;
    c.scanAddresses.clear();
    c.scanValues.clear();
    c.scanTruncated = false;
    const size_t size = TypeSize(t), align = std::min<size_t>(size, 4);
    const std::vector<Region> regions = Regions(from, to, true);
    if (w[2] == "any") {
        uint64_t total = 0;
        for (const Region& r : regions) total += r.size / align;
        if (total > kScanMax) return Err(c, "SCAN", "any: " + std::to_string(total) + " addresses in that range, over " + std::to_string(kScanMax) + "; narrow from= to=");
        std::vector<uint8_t> buf;
        for (const Region& r : regions) {
            buf.resize(r.size);
            const size_t got = ReadMemory(r.base, buf.data(), r.size);
            for (size_t i = 0; i + size <= got; i += align) {
                uint64_t raw = 0;
                memcpy(&raw, buf.data() + i, size);
                c.scanAddresses.push_back(r.base + static_cast<uint32_t>(i));
                c.scanValues.push_back(raw);
            }
        }
    } else if (IsFloat(t)) {
        // Floats: any value within 0.01 % (the game rarely holds exactly the number shown).
        uint64_t wantRaw;
        if (!EncodeValue(t, w[2], wantRaw)) return Err(c, "SCAN", "bad value " + w[2]);
        const double want = AsDouble(t, wantRaw);
        std::vector<uint8_t> buf(1 << 20);
        for (const Region& r : regions) {
            for (uint32_t at = r.base; at < r.base + r.size && !c.scanTruncated; at += static_cast<uint32_t>(buf.size())) {
                const size_t got = ReadMemory(at, buf.data(), std::min<size_t>(buf.size(), r.base + r.size - at));
                for (size_t i = 0; i + size <= got; i += align) {
                    uint64_t raw = 0;
                    memcpy(&raw, buf.data() + i, size);
                    if (!(fabs(AsDouble(t, raw) - want) <= 1e-4 * std::max(1.0, fabs(want)))) continue; // NaN too
                    if (c.scanAddresses.size() >= kScanMax) { c.scanTruncated = true; break; }
                    c.scanAddresses.push_back(at + static_cast<uint32_t>(i));
                    c.scanValues.push_back(raw);
                }
            }
        }
    } else {
        std::vector<uint8_t> bytes, mask;
        if (!NeedleOf(t, w[2], bytes, mask)) return Err(c, "SCAN", "bad value " + w[2]);
        uint64_t raw = 0;
        memcpy(&raw, bytes.data(), size);
        SearchRegions(regions, bytes, mask, align, [&](uint32_t a) {
            if (c.scanAddresses.size() >= kScanMax) { c.scanTruncated = true; return false; }
            c.scanAddresses.push_back(a);
            c.scanValues.push_back(raw);
            return true;
        });
    }
    Ok(c, "SCAN", "count=" + std::to_string(c.scanAddresses.size()) + (c.scanTruncated ? " truncated=yes" : ""));
}

inline void Server::Next(Client& c, const std::vector<std::string>& w) {
    if (c.scanType == Type::Invalid) return Err(c, "NEXT", "no scan: start one with SCAN");
    if (w.size() < 2) return Err(c, "NEXT", "usage: NEXT eq|ne|gt|lt v | changed|unchanged|inc|dec | incby|decby v");
    const std::string op = w[1];
    const bool needsValue = op == "eq" || op == "ne" || op == "gt" || op == "lt" || op == "incby" || op == "decby";
    const bool noValue = op == "changed" || op == "unchanged" || op == "inc" || op == "dec";
    if (!needsValue && !noValue) return Err(c, "NEXT", "unknown test " + op);
    double value = 0;
    if (needsValue) {
        uint64_t raw;
        if (w.size() < 3 || !EncodeValue(c.scanType, w[2], raw)) return Err(c, "NEXT", "bad value");
        value = AsDouble(c.scanType, raw);
    }
    const size_t size = TypeSize(c.scanType);
    const double tolerance = IsFloat(c.scanType) ? 1e-4 * std::max(1.0, fabs(value)) : 0.5;
    size_t kept = 0;
    for (size_t i = 0; i < c.scanAddresses.size(); ++i) {
        uint64_t raw = 0;
        if (ReadMemory(c.scanAddresses[i], &raw, size) != size) continue; // freed since
        const double now = AsDouble(c.scanType, raw), before = AsDouble(c.scanType, c.scanValues[i]);
        bool keep;
        if (op == "eq") keep = fabs(now - value) <= tolerance;
        else if (op == "ne") keep = fabs(now - value) > tolerance;
        else if (op == "gt") keep = now > value;
        else if (op == "lt") keep = now < value;
        else if (op == "changed") keep = raw != c.scanValues[i];
        else if (op == "unchanged") keep = raw == c.scanValues[i];
        else if (op == "inc") keep = now > before;
        else if (op == "dec") keep = now < before;
        else if (op == "incby") keep = fabs(now - before - value) <= tolerance;
        else keep = fabs(before - now - value) <= tolerance;
        if (!keep) continue;
        c.scanAddresses[kept] = c.scanAddresses[i];
        c.scanValues[kept] = raw;
        ++kept;
    }
    c.scanAddresses.resize(kept);
    c.scanValues.resize(kept);
    c.scanAddresses.shrink_to_fit();
    c.scanValues.shrink_to_fit();
    Ok(c, "NEXT", "count=" + std::to_string(kept));
}

inline void Server::List(Client& c, const std::vector<std::string>& w) {
    if (c.scanType == Type::Invalid) return Err(c, "LIST", "no scan: start one with SCAN");
    int64_t max = 100;
    std::string v;
    if (Option(w, "max", v) && (!ParseNumber(v, max) || max < 1 || max > 10000)) return Err(c, "LIST", "max must be 1-10000");
    const size_t size = TypeSize(c.scanType);
    for (size_t i = 0; i < c.scanAddresses.size() && static_cast<int64_t>(i) < max; ++i) {
        uint64_t raw = 0;
        const bool readable = ReadMemory(c.scanAddresses[i], &raw, size) == size;
        const std::string where = ModuleOffset(c.scanAddresses[i]);
        Row(c, "LIST", "addr=" + Hex(c.scanAddresses[i]) + " value=" + (readable ? FormatValue(c.scanType, raw) : "?") +
                           (where.empty() ? "" : " module=" + Quote(where)));
    }
    Ok(c, "LIST", "count=" + std::to_string(c.scanAddresses.size()));
}

inline void Server::Threads(Client& c) {
    const std::vector<DWORD> tids = ThreadIds();
    const DWORD mainTid = host_.mainThread ? host_.mainThread() : 0;
    for (DWORD tid : tids) {
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
        std::string name;
        uint32_t start = 0;
        double cpuMs = 0;
        if (thread) {
            name = ThreadName(thread);
            start = ThreadStartAddress(thread);
            FILETIME creation, exitTime, kernel, user;
            if (GetThreadTimes(thread, &creation, &exitTime, &kernel, &user)) {
                ULARGE_INTEGER k, u;
                k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
                cpuMs = (k.QuadPart + u.QuadPart) / 10000.0;
            }
            CloseHandle(thread);
        }
        char text[96];
        snprintf(text, sizeof(text), "tid=%lu cpu_ms=%.0f start=%s", static_cast<unsigned long>(tid), cpuMs, Hex(start).c_str());
        std::string row = text;
        if (tid == mainTid) row += " main=yes";
        if (tid == GetCurrentThreadId()) row += " server=yes";
        row += " name=" + Quote(name) + " start_sym=" + Quote(Where(start));
        Row(c, "THREADS", row);
    }
    Ok(c, "THREADS", "count=" + std::to_string(tids.size()));
}

inline void Server::Stack(Client& c, const std::vector<std::string>& w) {
    int64_t tid = 0, depth = 32;
    if (w.size() < 2 || !ParseNumber(w[1], tid)) return Err(c, "STACK", "usage: STACK tid [depth]");
    if (w.size() > 2 && (!ParseNumber(w[2], depth) || depth < 1 || depth > 256)) return Err(c, "STACK", "depth must be 1-256");
    static ThreadSnapshot snap; // 16 KB: not on the server's stack
    std::string err;
    if (!SnapshotThread(static_cast<DWORD>(tid), snap, err)) return Err(c, "STACK", err);
    const CONTEXT& x = snap.context;
    char regs[400];
    snprintf(regs, sizeof(regs), "eip=%s eax=%s ebx=%s ecx=%s edx=%s esi=%s edi=%s ebp=%s esp=%s eflags=%s",
             Hex(x.Eip).c_str(), Hex(x.Eax).c_str(), Hex(x.Ebx).c_str(), Hex(x.Ecx).c_str(), Hex(x.Edx).c_str(),
             Hex(x.Esi).c_str(), Hex(x.Edi).c_str(), Hex(x.Ebp).c_str(), Hex(x.Esp).c_str(), Hex(x.EFlags).c_str());
    Row(c, "STACK", "frame=0 addr=" + Hex(x.Eip) + " sym=" + Quote(Where(x.Eip)));
    // The words on the stack that are return addresses (code just after a call): the game's code mostly
    // does not keep frame pointers, so this is the call chain (with the odd stale entry).
    int frames = 1;
    for (size_t off = 0; off + 4 <= snap.stackBytes && frames < depth; off += 4) {
        uint32_t word;
        memcpy(&word, snap.stack + off, 4);
        if (word < 0x10000 || !LooksLikeReturnAddress(word)) continue;
        char at[48];
        snprintf(at, sizeof(at), "frame=%d esp+0x%lX", frames, static_cast<unsigned long>(off));
        Row(c, "STACK", std::string(at) + " addr=" + Hex(word) + " sym=" + Quote(Where(word)));
        ++frames;
    }
    Ok(c, "STACK", "tid=" + std::to_string(tid) + " " + regs + " stack_bytes=" + std::to_string(snap.stackBytes));
}

inline void Server::WatchCommand(Client& c, const std::vector<std::string>& w) {
    if (w[0] == "UNWATCH") {
        if (w.size() < 2) return Err(c, "UNWATCH", "usage: UNWATCH id|all");
        if (w[1] == "all") { c.watches.clear(); return Ok(c, "UNWATCH"); }
        int64_t id;
        if (!ParseNumber(w[1], id)) return Err(c, "UNWATCH", "bad id");
        const size_t before = c.watches.size();
        c.watches.erase(std::remove_if(c.watches.begin(), c.watches.end(), [&](const Watch& x) { return x.id == id; }), c.watches.end());
        return before == c.watches.size() ? Err(c, "UNWATCH", "no watch " + w[1]) : Ok(c, "UNWATCH");
    }
    if (w.size() == 2 && w[1] == "list") {
        for (const Watch& x : c.watches)
            Row(c, "WATCH", "id=" + std::to_string(x.id) + " addr=" + Hex(x.address) + " ms=" + std::to_string(x.intervalMs) +
                                " value=" + (x.known ? FormatValue(x.type, x.last) : "?"));
        return Ok(c, "WATCH", "count=" + std::to_string(c.watches.size()));
    }
    uint32_t address;
    if (w.size() < 3 || !ParseAddress(w[1], address)) return Err(c, "WATCH", "usage: WATCH addr type [ms] / WATCH list");
    const Type t = ParseType(w[2]);
    if (TypeSize(t) == 0) return Err(c, "WATCH", "WATCH takes a number type (u8 ... f64, ptr)");
    int64_t ms = 100;
    if (w.size() > 3 && (!ParseNumber(w[3], ms) || ms < 10 || ms > 60000)) return Err(c, "WATCH", "ms must be 10-60000");
    if (c.watches.size() >= 64) return Err(c, "WATCH", "64 watches at most");
    Watch x{c.nextWatchId++, address, t, static_cast<DWORD>(ms), 0, 0, false};
    c.watches.push_back(x);
    Ok(c, "WATCH", "id=" + std::to_string(x.id));
}

inline void Server::RefreshBreakpointThreads(bool all) {
    const std::vector<DWORD> tids = ThreadIds();
    std::vector<DWORD> todo;
    for (DWORD tid : tids) if (all || !bpThreads_.count(tid)) todo.push_back(tid);
    if (!todo.empty()) ApplyBreakpoints(todo);
    bpThreads_.clear();
    bpThreads_.insert(tids.begin(), tids.end());
}

inline void Server::Breakpoints(Client& c, const std::vector<std::string>& w) {
    const std::string sub = w.size() > 1 ? w[1] : "";
    if (sub == "list") {
        for (int i = 0; i < kBpSlots; ++i) {
            const Breakpoint& b = g_bp[i];
            if (!b.active) { Row(c, "BP", "slot=" + std::to_string(i) + " free"); continue; }
            const char* kind = b.kind == BpKind::Execute ? "x" : b.kind == BpKind::Write ? "w" : "rw";
            Row(c, "BP", "slot=" + std::to_string(i) + " addr=" + Hex(b.address) + " kind=" + kind + " len=" + std::to_string(b.length) +
                             " hits=" + std::to_string(b.hits) + " sym=" + Quote(Where(b.address)));
        }
        return Ok(c, "BP", "threads=" + std::to_string(bpThreads_.size()));
    }
    int64_t slot = -1;
    if (sub == "sites") {
        if (w.size() < 3 || !ParseNumber(w[2], slot) || slot < 0 || slot >= kBpSlots) return Err(c, "BP", "usage: BP sites slot");
        std::vector<std::pair<LONG, uint32_t>> sites;
        for (int i = 0; i < kSites; ++i)
            if (g_sites[slot][i].eip) sites.push_back({g_sites[slot][i].count, static_cast<uint32_t>(g_sites[slot][i].eip)});
        std::sort(sites.rbegin(), sites.rend());
        for (const auto& s : sites) Row(c, "BP", "eip=" + Hex(s.second) + " count=" + std::to_string(s.first) + " sym=" + Quote(Where(s.second)));
        return Ok(c, "BP", "sites=" + std::to_string(sites.size()));
    }
    if (!host_.debugAllowed) return Err(c, "BP", "breakpoints need DLL_SERVER_DEBUG=true in um.cfg");
    if (sub == "clear") {
        if (w.size() < 3) return Err(c, "BP", "usage: BP clear slot|all");
        for (int i = 0; i < kBpSlots; ++i)
            if (w[2] == "all" || w[2] == std::to_string(i)) g_bp[i].active = 0;
        RefreshBreakpointThreads(true);
        return Ok(c, "BP");
    }
    if (sub != "set") return Err(c, "BP", "usage: BP set|clear|list|sites ...");
    uint32_t address;
    if (w.size() < 5 || !ParseNumber(w[2], slot) || slot < 0 || slot >= kBpSlots || !ParseAddress(w[3], address))
        return Err(c, "BP", "usage: BP set slot(0-3) addr x|w|rw [len 1|2|4]");
    BpKind kind;
    if (w[4] == "x") kind = BpKind::Execute;
    else if (w[4] == "w") kind = BpKind::Write;
    else if (w[4] == "rw") kind = BpKind::ReadWrite;
    else return Err(c, "BP", "kind is x (execute), w (write) or rw (read/write)");
    int64_t length = kind == BpKind::Execute ? 1 : 4;
    if (w.size() > 5 && (!ParseNumber(w[5], length) || (length != 1 && length != 2 && length != 4))) return Err(c, "BP", "len is 1, 2 or 4");
    if (kind == BpKind::Execute) length = 1;
    if (address % length) return Err(c, "BP", "the address must be a multiple of len");
    Breakpoint& b = g_bp[slot];
    b.active = 0;
    b.address = address;
    b.kind = kind;
    b.length = static_cast<int>(length);
    b.hits = 0;
    for (int i = 0; i < kSites; ++i) { g_sites[slot][i].eip = 0; g_sites[slot][i].count = 0; }
    b.active = 1;
    RefreshBreakpointThreads(true);
    Log("INFO", "DLL server: breakpoint " + std::to_string(slot) + " at " + Hex(address) + " (" + w[4] + ")");
    Ok(c, "BP", "slot=" + std::to_string(slot) + " threads=" + std::to_string(bpThreads_.size()));
}

inline void Server::Write(Client& c, const std::vector<std::string>& w, bool typed) {
    const char* cmd = typed ? "POKE" : "WRITE";
    if (!host_.debugAllowed) return Err(c, cmd, "writing needs DLL_SERVER_DEBUG=true in um.cfg");
    uint32_t address;
    if (w.size() < (typed ? 4u : 3u) || !ParseAddress(w[1], address)) return Err(c, cmd, typed ? "usage: POKE addr type value" : "usage: WRITE addr hexbytes");
    std::vector<uint8_t> bytes, mask;
    if (typed) {
        const Type t = ParseType(w[2]);
        if (t == Type::Invalid || t == Type::Bytes) return Err(c, cmd, "unknown type " + w[2]);
        if (!NeedleOf(t, w[3], bytes, mask)) return Err(c, cmd, "bad value " + w[3]);
        if (t == Type::Str) bytes.push_back(0);
        if (t == Type::WStr) { bytes.push_back(0); bytes.push_back(0); }
    } else {
        std::string all;
        for (size_t i = 2; i < w.size(); ++i) all += w[i];
        if (!ParsePattern(all, bytes, mask) || std::find(mask.begin(), mask.end(), 0) != mask.end()) return Err(c, cmd, "bad bytes");
    }
    std::vector<uint8_t> before(bytes.size());
    const size_t had = ReadMemory(address, before.data(), before.size());
    std::string err;
    if (!WriteMemory(address, bytes.data(), bytes.size(), err)) return Err(c, cmd, err + " at " + Hex(address));
    Log("INFO", std::string("DLL server: ") + cmd + " " + Hex(address) + " " + HexBytes(before.data(), had) + " -> " + HexBytes(bytes.data(), bytes.size()));
    Ok(c, cmd, "addr=" + Hex(address) + " len=" + std::to_string(bytes.size()) + " old=" + HexBytes(before.data(), had));
}

// One row per live or dead (not yet looted) unit. The records come from a scan running on its own
// thread (every 3 s while units are asked for); their values are read now.
inline void Server::Units(Client& c, const std::vector<std::string>& w) {
    scanner_.Want();
    if (w.size() > 1 && w[1] == "rescan") scanner_.Rescan();
    long long age = -1;
    const std::vector<uint32_t> records = scanner_.Records(age);
    int count = 0;
    for (uint32_t r : records) {
        dllgame::Unit u;
        if (!dllgame::ReadUnit(r, u)) continue;
        char text[320];
        snprintf(text, sizeof(text), "addr=%s id=%lu x=%.2f y=%.2f z=%.2f yaw=%.3f side=%lu hp=%.2f hpmax=%.2f mp=%.2f mpmax=%.2f flags=0x%lX",
                 Hex(u.record).c_str(), static_cast<unsigned long>(u.id), u.x, u.y, u.z, u.yaw, static_cast<unsigned long>(u.side),
                 u.hp, u.hpMax, u.mana, u.manaMax, static_cast<unsigned long>(u.flags));
        char senses[64];
        snprintf(senses, sizeof(senses), " sight=%.2f fov=%.1f", u.sight, u.viewAngle);
        Row(c, "UNITS", std::string(text) + senses + (u.name.empty() ? "" : " name=" + Quote(u.name)));
        ++count;
    }
    Ok(c, "UNITS", "count=" + std::to_string(count) + " scan_age_ms=" + std::to_string(age));
}

inline void Server::ConsoleCommand(Client& c, const std::vector<std::string>& w) {
    const std::string sub = w.size() > 1 ? w[1] : "";
    if (sub == "lines") {
        int64_t from = 0, max = 500;
        if (w.size() > 2 && (!ParseNumber(w[2], from) || from < 0)) return Err(c, "CONSOLE", "bad from");
        if (w.size() > 3 && (!ParseNumber(w[3], max) || max < 1 || max > 5000)) return Err(c, "CONSOLE", "max must be 1-5000");
        std::vector<std::string> lines;
        uint32_t count = 0;
        std::string input;
        if (!dllgame::ConsoleLines(static_cast<uint32_t>(from), static_cast<uint32_t>(max), lines, count, input))
            return Err(c, "CONSOLE", "the console was not found");
        for (size_t i = 0; i < lines.size(); ++i) Row(c, "CONSOLE", "i=" + std::to_string(from + static_cast<int64_t>(i)) + " text=" + Quote(lines[i]));
        return Ok(c, "CONSOLE", "count=" + std::to_string(count) + " open=" + (dllgame::ConsoleOpen() ? "1" : "0") + " input=" + Quote(input));
    }
    if (sub == "send") {
        if (!host_.debugAllowed) return Err(c, "CONSOLE", "sending needs DLL_SERVER_DEBUG=true in um.cfg");
        if (w.size() < 3) return Err(c, "CONSOLE", "usage: CONSOLE send text");
        std::string text = w[2];
        for (size_t i = 3; i < w.size(); ++i) text += " " + w[i];
        std::string err;
        if (!dllgame::ConsoleSend(text, err)) return Err(c, "CONSOLE", err);
        Log("INFO", "DLL server: console command sent: " + text);
        return Ok(c, "CONSOLE", "sent=" + Quote(text));
    }
    Err(c, "CONSOLE", "usage: CONSOLE lines [from] [max] / CONSOLE send text");
}

inline bool Server::Handle(Client& c, const std::string& line) {
    std::vector<std::string> w = Split(line);
    if (w.empty()) return true;
    for (char& ch : w[0]) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    const std::string cmd = w[0];
    if (cmd == "PING") Ok(c, "PING");
    else if (cmd == "BYE") return false;
    else if (cmd == "HELP") Help(c);
    else if (cmd == "INFO") Info(c);
    else if (cmd == "MAP") Ok(c, "MAP", host_.mapInfo ? host_.mapInfo() : "");
    else if (cmd == "MODULES") ModulesCommand(c);
    else if (cmd == "REGIONS") RegionsCommand(c, w);
    else if (cmd == "SYM") {
        uint32_t a;
        if (w.size() < 2 || !ParseAddress(w[1], a)) Err(c, "SYM", "usage: SYM addr");
        else Ok(c, "SYM", "addr=" + Hex(a) + " sym=" + Quote(Where(a)));
    }
    else if (cmd == "READ") Read(c, w, false);
    else if (cmd == "DUMP") Read(c, w, true);
    else if (cmd == "PEEK") Peek(c, w);
    else if (cmd == "PTR") Pointer(c, w);
    else if (cmd == "FIND") Find(c, w);
    else if (cmd == "SCAN") Scan(c, w);
    else if (cmd == "NEXT") Next(c, w);
    else if (cmd == "LIST") List(c, w);
    else if (cmd == "THREADS") Threads(c);
    else if (cmd == "STACK") Stack(c, w);
    else if (cmd == "WATCH" || cmd == "UNWATCH") WatchCommand(c, w);
    else if (cmd == "BP") Breakpoints(c, w);
    else if (cmd == "WRITE") Write(c, w, false);
    else if (cmd == "POKE") Write(c, w, true);
    else if (cmd == "UNITS") Units(c, w);
    else if (cmd == "CONSOLE") ConsoleCommand(c, w);
    else if (cmd == "CAMERA") {
        dllgame::Camera cam;
        char text[256];
        if (!dllgame::ReadCamera(cam)) Err(c, "CAMERA", "the camera could not be read");
        else {
            snprintf(text, sizeof(text), "x=%.3f y=%.3f z=%.3f qx=%.4f qy=%.4f qz=%.4f qw=%.4f tx=%.3f ty=%.3f tz=%.3f",
                     cam.x, cam.y, cam.z, cam.qx, cam.qy, cam.qz, cam.qw, cam.targetX, cam.targetY, cam.targetZ);
            Ok(c, "CAMERA", text);
        }
    }
    else if (cmd == "SUB") {
        const bool on = w.size() > 2 && w[2] == "on";
        if (w.size() > 2 && w[1] == "log") { c.logEvents = on; c.logSeq = g_logSeq; Ok(c, "SUB", std::string("log=") + (on ? "on" : "off")); }
        else if (w.size() > 2 && w[1] == "hits") { c.hitEvents = on; c.hitSeq = g_hitCount; Ok(c, "SUB", std::string("hits=") + (on ? "on" : "off")); }
        else Err(c, "SUB", "usage: SUB log|hits on|off");
    }
    else Err(c, cmd, "unknown command (HELP lists them)");
    return true;
}

// Events: breakpoint hits, watched values that changed, um.log lines.
inline void Server::SendEvents(Client& c) {
    if (c.hitEvents) {
        const LONG last = g_hitCount;
        if (last - c.hitSeq > kHitRing) {
            c.output += "HIT dropped=" + std::to_string(last - c.hitSeq - kHitRing) + "\n";
            c.hitSeq = last - kHitRing;
        }
        for (int sent = 0; c.hitSeq < last && sent < 64; ++sent) {
            const Hit& h = g_hits[c.hitSeq % kHitRing];
            ++c.hitSeq;
            if (h.seq != c.hitSeq) continue; // being written, or already overwritten
            char text[512];
            snprintf(text, sizeof(text), "HIT slot=%d tid=%lu eip=%s value=%s eax=%s ebx=%s ecx=%s edx=%s esi=%s edi=%s ebp=%s esp=%s stack=",
                     h.slot, static_cast<unsigned long>(h.tid), Hex(h.eip).c_str(), Hex(h.value).c_str(), Hex(h.eax).c_str(), Hex(h.ebx).c_str(),
                     Hex(h.ecx).c_str(), Hex(h.edx).c_str(), Hex(h.esi).c_str(), Hex(h.edi).c_str(), Hex(h.ebp).c_str(), Hex(h.esp).c_str());
            std::string line = text;
            for (int i = 0; i < 8; ++i) line += (i ? "," : "") + Hex(h.stack[i]);
            c.output += line + " sym=" + Quote(Where(h.eip)) + "\n";
        }
    }
    const ULONGLONG now = GetTickCount64();
    for (Watch& x : c.watches) {
        if (now < x.nextMs) continue;
        x.nextMs = now + x.intervalMs;
        uint64_t raw = 0;
        const size_t size = TypeSize(x.type);
        if (ReadMemory(x.address, &raw, size) != size) {
            if (x.known) c.output += "WATCHED id=" + std::to_string(x.id) + " addr=" + Hex(x.address) + " value=unreadable old=" + FormatValue(x.type, x.last) + "\n";
            x.known = false;
            continue;
        }
        if (x.known && raw == x.last) continue;
        c.output += "WATCHED id=" + std::to_string(x.id) + " addr=" + Hex(x.address) + " value=" + FormatValue(x.type, raw) +
                    (x.known ? " old=" + FormatValue(x.type, x.last) : "") + "\n";
        x.last = raw;
        x.known = true;
    }
    if (c.logEvents) {
        std::vector<std::string> lines;
        while (InterlockedCompareExchange(&g_logLock, 1, 0) != 0) Sleep(0);
        const LONG last = g_logSeq;
        if (last - c.logSeq > kLogRing) c.logSeq = last - kLogRing;
        for (; c.logSeq < last; ++c.logSeq) lines.push_back(g_logLines[c.logSeq % kLogRing]);
        InterlockedExchange(&g_logLock, 0);
        for (const std::string& l : lines) c.output += "LOG " + Quote(l) + "\n";
    }
}

inline void Server::Run(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("ERROR", "DLL server: WSAStartup failed"); return; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { Log("ERROR", "DLL server: socket() failed"); WSACleanup(); return; }
    BOOL exclusive = TRUE; // no other program can take the port from under us
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        Log("ERROR", "DLL server: cannot listen on 127.0.0.1:" + std::to_string(port) + " (error " + std::to_string(WSAGetLastError()) + "; port in use?)");
        closesocket(listener);
        WSACleanup();
        return;
    }
    Log("INFO", "DLL server: listening on 127.0.0.1:" + std::to_string(port) + (host_.debugAllowed ? " (debug commands on)" : ""));
    InterlockedExchange(&g_running, 1);
    cpu_.Sample();
    ULONGLONG nextStatsMs = GetTickCount64() + 1000, nextThreadsMs = 0;
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        for (const Client& c : clients_) FD_SET(c.socket, &readable);
        // Short waits while something is followed (breakpoints, watches, the log), else up to the next stats.
        bool busy = AnyBreakpoint();
        for (const Client& c : clients_) busy = busy || !c.watches.empty() || c.logEvents;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG waitMs = nextStatsMs > now ? nextStatsMs - now : 0;
        if (busy) waitMs = std::min<ULONGLONG>(waitMs, 20);
        timeval tv;
        tv.tv_sec = static_cast<long>(waitMs / 1000);
        tv.tv_usec = static_cast<long>((waitMs % 1000) * 1000);
        const int ready = select(0, &readable, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) { Sleep(100); continue; }
        if (ready > 0 && FD_ISSET(listener, &readable)) {
            SOCKET s = accept(listener, nullptr, nullptr);
            if (s != INVALID_SOCKET) {
                if (static_cast<int>(clients_.size()) >= kMaxClients) {
                    SendAll(s, "ERR CONNECT too many connections\n");
                    closesocket(s);
                } else {
                    DWORD timeoutMs = 5000; // a client that stops reading is dropped
                    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
                    char hello[160];
                    snprintf(hello, sizeof(hello), "HELLO um.dll version=%s proto=%d pid=%lu debug=%s\n", host_.version, kProtocol,
                             static_cast<unsigned long>(GetCurrentProcessId()), host_.debugAllowed ? "on" : "off");
                    Client c;
                    c.socket = s;
                    c.hitSeq = g_hitCount;
                    if (SendAll(s, hello) && SendAll(s, StatsLine(cpu_))) {
                        clients_.push_back(std::move(c));
                        Log("INFO", "DLL server: a client connected");
                    } else {
                        closesocket(s);
                    }
                }
            }
        }
        std::vector<bool> drop(clients_.size(), false);
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = clients_[i];
            if (ready > 0 && FD_ISSET(c.socket, &readable)) {
                char buf[4096];
                const int n = recv(c.socket, buf, sizeof(buf), 0);
                if (n <= 0) { drop[i] = true; continue; }
                c.input.append(buf, static_cast<size_t>(n));
                if (c.input.size() > 65536) { drop[i] = true; continue; } // not a line protocol client
                size_t end;
                bool keep = true;
                while (keep && (end = c.input.find('\n')) != std::string::npos) {
                    std::string line = c.input.substr(0, end);
                    c.input.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    keep = Handle(c, line);
                }
                if (!keep) drop[i] = true;
            }
            SendEvents(c);
            if (!c.output.empty()) {
                if (!SendAll(c.socket, c.output)) drop[i] = true;
                c.output.clear();
            }
        }
        if (GetTickCount64() >= nextStatsMs) {
            nextStatsMs = GetTickCount64() + 1000;
            const std::string stats = StatsLine(cpu_);
            for (size_t i = 0; i < clients_.size(); ++i)
                if (!drop[i] && !SendAll(clients_[i].socket, stats)) drop[i] = true;
        }
        // Threads created since the breakpoints were set get them too.
        if (AnyBreakpoint() && GetTickCount64() >= nextThreadsMs) {
            nextThreadsMs = GetTickCount64() + 500;
            RefreshBreakpointThreads(false);
        }
        std::vector<Client> kept;
        for (size_t i = 0; i < clients_.size(); ++i) {
            if (drop[i]) { closesocket(clients_[i].socket); Log("INFO", "DLL server: a client disconnected"); }
            else kept.push_back(std::move(clients_[i]));
        }
        clients_.swap(kept);
    }
}

// The server thread's body.
inline void Run(int port, const Host& host) {
    static Server server(host);
    server.Run(port);
}

} // namespace dllserver
