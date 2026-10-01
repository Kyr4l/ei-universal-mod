// What the DLL server knows of game.exe's own data (found with the server's debugger, see
// docs/game-memory.md): its units and its console. Reads only, through ReadMemory (a bad pointer fails
// instead of faulting), except ConsoleSend which types into the game window.
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "dll_debug.hpp"

namespace dllgame {

using dlldebug::ReadMemory;

// ---- units --------------------------------------------------------------------------------------------
inline bool ReadGameString(uint32_t chars, std::string& out);

// Every placed object (units included) is a record starting with kObjectClass. A unit's record points at
// +0x170 to its stats object (starting with kStatsClass). Record: +0x08 flags, +0x0C the object's ID (as
// in the .mob; 0xFFFFFFFF once a body is looted), +0x1C x, y, z, +0x34 rotation quaternion w, x, y, z
// (about the vertical axis: w and z), +0x238 the name of a player's character (a game string), +0x250
// side (the .mob's "player"). Stats: +0x28 HP, +0x2C HP max,
// +0x34 mana, +0x38 mana max (floats).
const uint32_t kObjectClass = 0x0073EA8C, kStatsClass = 0x0073DD6C;
const uint32_t kNoId = 0xFFFFFFFF;

struct Unit {
    uint32_t record, id, side, flags;
    std::string name; // a player's character: its name (record +0x238); empty for the map's units
    float x, y, z, yaw; // yaw: the quaternion's angle about Z (the unit faces (sin yaw, -cos yaw))
    float hp, hpMax, mana, manaMax;
    float sight = 0, viewAngle = 0; // current sight range and view angle (degrees); 0 when unknown
};

// Reads one unit; false when the record is no longer a live or dead unit (freed, looted, not a unit).
inline bool ReadUnit(uint32_t record, Unit& u) {
    uint8_t r[0x254 > 0x244 ? 0x254 : 0x244];
    if (ReadMemory(record, r, sizeof(r)) != sizeof(r)) return false;
    uint32_t cls, stats;
    memcpy(&cls, r, 4);
    memcpy(&stats, r + 0x170, 4);
    if (cls != kObjectClass || stats < 0x10000) return false;
    uint8_t s[0x3C];
    if (ReadMemory(stats, s, sizeof(s)) != sizeof(s)) return false;
    uint32_t statsCls;
    memcpy(&statsCls, s, 4);
    if (statsCls != kStatsClass) return false;
    u.record = record;
    memcpy(&u.flags, r + 0x08, 4);
    memcpy(&u.id, r + 0x0C, 4);
    if (u.id == kNoId) return false;
    memcpy(&u.x, r + 0x1C, 4);
    memcpy(&u.y, r + 0x20, 4);
    memcpy(&u.z, r + 0x24, 4);
    float qw, qz;
    memcpy(&qw, r + 0x34, 4);
    memcpy(&qz, r + 0x40, 4);
    u.yaw = 2.0f * atan2f(qz, qw);
    memcpy(&u.side, r + 0x250, 4);
    memcpy(&u.hp, s + 0x28, 4);
    memcpy(&u.hpMax, s + 0x2C, 4);
    memcpy(&u.mana, s + 0x34, 4);
    memcpy(&u.manaMax, s + 0x38, 4);
    uint32_t namePtr;
    memcpy(&namePtr, r + 0x238, 4);
    if (!ReadGameString(namePtr, u.name)) u.name.clear();
    // The unit's senses (record +0x240): +0x290 / +0x294 sight range, base and current; +0x298 / +0x29C
    // view angle in degrees, base and current.
    uint32_t senses;
    memcpy(&senses, r + 0x240, 4);
    float sv[4];
    if (senses >= 0x10000 && ReadMemory(senses + 0x290, sv, sizeof(sv)) == sizeof(sv) && isfinite(sv[1]) && isfinite(sv[3]) &&
        sv[1] >= 0 && sv[1] < 1000 && sv[3] >= 0 && sv[3] <= 360) {
        u.sight = sv[1];
        u.viewAngle = sv[3];
    }
    return isfinite(u.x) && isfinite(u.y) && isfinite(u.hp);
}

// The unit records, found by a scan of the game's memory on a thread of its own (a scan takes a second
// or two): it runs while someone asks for units (Want), every few seconds, so that new units appear.
class UnitScanner {
public:
    void Want() {
        lastWantMs_ = GetTickCount64();
        if (InterlockedCompareExchange(&started_, 1, 0) == 0) {
            InitializeCriticalSection(&lock_);
            InterlockedExchange(&ready_, 1);
            HANDLE t = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    // The records found by the last scan, and how long ago it ended (ms; -1: none yet).
    std::vector<uint32_t> Records(long long& ageMs) {
        std::vector<uint32_t> out;
        ageMs = -1;
        if (!ready_) return out;
        EnterCriticalSection(&lock_);
        out = records_;
        if (scannedMs_) ageMs = static_cast<long long>(GetTickCount64() - scannedMs_);
        LeaveCriticalSection(&lock_);
        return out;
    }
    void Rescan() { InterlockedExchange(&rescan_, 1); }

private:
    volatile LONG started_ = 0, ready_ = 0, rescan_ = 0;
    volatile ULONGLONG lastWantMs_ = 0;
    CRITICAL_SECTION lock_;
    std::vector<uint32_t> records_;
    ULONGLONG scannedMs_ = 0;

    static DWORD WINAPI Thread(LPVOID self) {
        try {
            static_cast<UnitScanner*>(self)->Loop();
        } catch (...) {
        }
        return 0;
    }
    void Loop() {
        for (;;) {
            if (GetTickCount64() - lastWantMs_ > 15000) { Sleep(200); continue; } // nobody is looking
            std::vector<uint32_t> found = Scan();
            EnterCriticalSection(&lock_);
            records_.swap(found);
            scannedMs_ = GetTickCount64();
            LeaveCriticalSection(&lock_);
            for (int i = 0; i < 30 && !rescan_; ++i) Sleep(100); // every 3 s, or at once when asked
            InterlockedExchange(&rescan_, 0);
        }
    }
    static std::vector<uint32_t> Scan() {
        std::vector<uint32_t> out;
        std::vector<uint8_t> buf(1 << 20);
        for (const dlldebug::Region& r : dlldebug::Regions(0x10000, dlldebug::HighestAddress(), true)) {
            if (r.type == MEM_IMAGE) continue; // the records are on the heap
            for (uint32_t at = r.base; at < r.base + r.size; at += static_cast<uint32_t>(buf.size())) {
                const size_t got = ReadMemory(at, buf.data(), std::min<size_t>(buf.size(), r.base + r.size - at));
                for (size_t i = 0; i + 4 <= got; i += 4) {
                    uint32_t v;
                    memcpy(&v, buf.data() + i, 4);
                    if (v != kObjectClass) continue;
                    Unit u;
                    if (ReadUnit(at + static_cast<uint32_t>(i), u)) out.push_back(at + static_cast<uint32_t>(i));
                }
                if (got == 0) break;
            }
        }
        return out;
    }
};

// ---- the map the game runs ----------------------------------------------------------------------------
// game.exe's globals: 0x007C2CCC the terrain ("zone3xobr.mpr"), 0x007C2CC8 the base map
// ("zone3xobr-lmp.mob"), 0x007AFE90 the quest ("z3xq1"; the empty string outside a quest; 0x007C7DFC
// holds it too). All game strings.
inline bool GameMapNames(std::string& terrain, std::string& base, std::string& quest) {
    uint32_t t = 0, b = 0, q = 0;
    if (ReadMemory(0x007C2CCC, &t, 4) != 4 || ReadMemory(0x007C2CC8, &b, 4) != 4 || ReadMemory(0x007AFE90, &q, 4) != 4) return false;
    if (!ReadGameString(t, terrain) || !ReadGameString(b, base)) return false;
    if (!ReadGameString(q, quest)) quest.clear();
    if (!quest.empty() && quest.find('.') == std::string::npos) quest += ".mob";
    return !terrain.empty();
}

// ---- the camera ---------------------------------------------------------------------------------------
// game.exe's camera, a global: its position at 0x0079B2C4 (x, y, z), its rotation quaternion at
// 0x0079B2DC, and the point it turns around (where it aims) at 0x0079B388 (x, y, z).
struct Camera { float x, y, z, qx, qy, qz, qw, targetX, targetY, targetZ; };

inline bool ReadCamera(Camera& c) {
    float pos[3], rot[4], target[3];
    if (ReadMemory(0x0079B2C4, pos, sizeof(pos)) != sizeof(pos) || ReadMemory(0x0079B2DC, rot, sizeof(rot)) != sizeof(rot) ||
        ReadMemory(0x0079B388, target, sizeof(target)) != sizeof(target))
        return false;
    c = Camera{pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3], target[0], target[1], target[2]};
    for (float v : {c.x, c.y, c.z, c.targetX, c.targetY}) if (!isfinite(v)) return false;
    return true;
}

// ---- the console --------------------------------------------------------------------------------------
// game.exe keeps a pointer to its console at kConsolePtr. Console: +0x10 / +0x14 the command history
// (array of strings, count), +0x24 the input line (a game string), +0x28 the cursor, +0x2C its length,
// +0x5C / +0x60 the lines shown (array of strings, count), +0x7A 1 while it is open.
// A game string pointer points at its characters, after a header [references][length][capacity].
const uint32_t kConsolePtr = 0x007C7D64;

inline bool ReadGameString(uint32_t chars, std::string& out) {
    out.clear();
    if (chars < 0x10000) return false;
    uint32_t header[3];
    if (ReadMemory(chars - 12, header, sizeof(header)) != sizeof(header)) return false;
    const uint32_t length = header[1];
    if (length > 4096) return false;
    out.resize(length);
    return length == 0 || ReadMemory(chars, &out[0], length) == length;
}

inline uint32_t Console() {
    uint32_t c = 0;
    ReadMemory(kConsolePtr, &c, 4);
    return c;
}

inline bool ConsoleOpen() {
    const uint32_t c = Console();
    uint8_t open = 0;
    return c && ReadMemory(c + 0x7A, &open, 1) == 1 && open != 0;
}

// The console's lines from `from` on (at most `max`), and the total count.
inline bool ConsoleLines(uint32_t from, uint32_t max, std::vector<std::string>& lines, uint32_t& count, std::string& input) {
    const uint32_t c = Console();
    if (!c) return false;
    uint32_t array = 0, inputPtr = 0;
    if (ReadMemory(c + 0x5C, &array, 4) != 4 || ReadMemory(c + 0x60, &count, 4) != 4 || ReadMemory(c + 0x24, &inputPtr, 4) != 4) return false;
    if (count > 100000) return false;
    ReadGameString(inputPtr, input);
    for (uint32_t i = from; i < count && lines.size() < max; ++i) {
        uint32_t p = 0;
        std::string text;
        if (ReadMemory(array + 4 * i, &p, 4) == 4) ReadGameString(p, text);
        lines.push_back(text);
    }
    return true;
}

// The game's main window: the largest visible top-level window of this process.
struct WindowSearch { DWORD pid; HWND best; LONG area; };
inline BOOL CALLBACK WindowSearchStep(HWND w, LPARAM p) {
    WindowSearch& s = *reinterpret_cast<WindowSearch*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != s.pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(w, &r);
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > s.area) { s.area = area; s.best = w; }
    return TRUE;
}
inline HWND GameWindow() {
    WindowSearch s{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(WindowSearchStep, reinterpret_cast<LPARAM>(&s));
    return s.best;
}

// Types the text into the game window and presses Enter, as the keyboard would. Refused while the
// console is closed: the keys would reach the game as its shortcuts.
inline bool ConsoleSend(const std::string& text, std::string& err) {
    if (!ConsoleOpen()) { err = "the console is closed: open it in the game first"; return false; }
    HWND w = GameWindow();
    if (!w) { err = "the game window was not found"; return false; }
    for (unsigned char ch : text) PostMessageA(w, WM_CHAR, ch, 1);
    // Enter as a key press only: the console also takes a '\r' character as Enter, which made it twice.
    const LPARAM down = 1 | (0x1C << 16), up = down | (1u << 30) | (1u << 31);
    PostMessageA(w, WM_KEYDOWN, VK_RETURN, down);
    PostMessageA(w, WM_KEYUP, VK_RETURN, up);
    return true;
}

} // namespace dllgame
