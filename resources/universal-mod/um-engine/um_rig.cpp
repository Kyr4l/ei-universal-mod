// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The frame recorder and the rig commands (docs/rig-commands.md).
//
// Recorder: every world frame leaves one record (what the sun shadows did: pads, area, map, D7VK's result, timings) in a ring of the last few seconds. The recorder key
// (um-engine.cfg Recorder::Key, Pause by default; Ctrl too with Recorder::Ctrl=1) writes the ring as a text report and a PNG of the world as drawn (no interface) into um-reports\ beside the DLL: the moment
// a glitch is seen, press it and send the two files.
//
// Rig commands (UmEngineRig, reached through um.dll's ENGINE command): run on the game's thread at the end of the next world frame: SHOT, REC, STATE, CAM, PAUSE, HOUR, SCRIPT.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "um_rig.h"

extern "C" void UmEngineLog(const char* format, ...);
extern "C" bool UmEngineConfigNumbers(const char* const* keys, double* values, int count);
extern "C" bool UmEngineDirectory(char* out, int size);   // um_engine.cpp: the folder of um-engine.dll, with a trailing backslash
extern "C" const char* UmEngineVersion();

UmFrameRec g_umFrame;

namespace {

const uintptr_t kWorld = 0x7AF5B8;     // CWorldClient
const uintptr_t kCamera = 0x79B2A8;    // CCamera

template <typename T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }

// ---- config (re-read once a second) ----
struct RigCfg { double key = 19, frames = 600, screenshot = 1, maxReports = 20, enabled = 1, ctrl = 0, burst = 0, burstFrames = 240, burstScale = 4; };
RigCfg g_cfg; DWORD g_cfgAt = 0;
const RigCfg& Cfg() {
    const DWORD now = GetTickCount();
    if (g_cfgAt == 0 || now - g_cfgAt > 1000) {
        g_cfgAt = now ? now : 1;
        static const char* const keys[9] = { "Recorder::Key", "Recorder::Frames", "Recorder::Screenshot", "Recorder::MaxReports", "Recorder::Enabled", "Recorder::Ctrl", "Recorder::Burst", "Recorder::BurstFrames", "Recorder::BurstScale" };
        double v[9] = { g_cfg.key, g_cfg.frames, g_cfg.screenshot, g_cfg.maxReports, g_cfg.enabled, g_cfg.ctrl, g_cfg.burst, g_cfg.burstFrames, g_cfg.burstScale };
        if (UmEngineConfigNumbers(keys, v, 9)) {
            auto clamp = [](double x, double lo, double hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; };
            g_cfg.key = clamp(v[0], 0, 254); g_cfg.frames = clamp(v[1], 0, 7200); g_cfg.screenshot = v[2]; g_cfg.maxReports = clamp(v[3], 1, 500); g_cfg.enabled = v[4]; g_cfg.ctrl = v[5]; g_cfg.burst = v[6]; g_cfg.burstFrames = clamp(v[7], 30, 1200); g_cfg.burstScale = clamp(v[8], 1, 8);
        }
    }
    return g_cfg;
}

// ---- the ring ----
std::vector<UmFrameRec> g_ring; size_t g_ringNext = 0, g_ringCount = 0, g_ringCapacity = 0;
UmFrameRec g_last;            // the last stored record (STATE)
bool g_haveLast = false;
uint32_t g_frameNumber = 0;
LARGE_INTEGER g_freq, g_beginAt, g_prevBegin;

typedef void (*StatsFn)(uint32_t*);
typedef int (*ShotFn)(const char*, int);
typedef void (*RingConfigFn)(int, int);
typedef int (*RingGrabFn)(unsigned);
typedef int (*RingDumpFn)(const char*);
StatsFn g_stats = nullptr; ShotFn g_shot = nullptr; bool g_looked = false;
RingConfigFn g_ringConfig = nullptr; RingGrabFn g_ringGrab = nullptr; RingDumpFn g_ringDump = nullptr;
bool g_ringOn = false; int g_ringFrames = 0, g_ringScale = 0;   // the ring as last configured

void Lookup() {
    if (g_looked) return;
    g_looked = true;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    if (!module) return;
    g_stats = reinterpret_cast<StatsFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowStats")));
    g_shot = reinterpret_cast<ShotFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowScreenshot")));
    g_ringConfig = reinterpret_cast<RingConfigFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowRingConfig")));
    g_ringGrab = reinterpret_cast<RingGrabFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowRingGrab")));
    g_ringDump = reinterpret_cast<RingDumpFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowRingDump")));
    if (!g_shot) UmEngineLog("um-engine: recorder: the DirectDraw library has no UmShadowScreenshot (an older d7vk.dll): reports come without a picture");
}

std::string Format(const char* format, ...) {
    char text[1024];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// ---- reports ----
std::string ReportFolder() {
    char dir[MAX_PATH];
    if (!UmEngineDirectory(dir, sizeof(dir))) return std::string();
    return std::string(dir) + "um-reports\\";
}

// Keeps the folder at most `max` reports: the oldest report (a file of ours: um-report-*.txt with its .png) goes when a new one is made
void TrimReports(const std::string& folder, int max) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA((folder + "um-report-*.txt").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return;
    do names.push_back(found.cFileName); while (FindNextFileA(h, &found));
    FindClose(h);
    if (static_cast<int>(names.size()) < max) return;
    std::sort(names.begin(), names.end());   // the names carry the time: the oldest first
    for (size_t i = 0; i + static_cast<size_t>(max) - 1 < names.size(); ++i) {
        const std::string base = folder + names[i].substr(0, names[i].size() - 4);
        DeleteFileA((base + ".txt").c_str());
        DeleteFileA((base + ".png").c_str());
        WIN32_FIND_DATAA pic;   // its burst folder (only the .png files this recorder wrote into it)
        const std::string dir = base + "-frames\\";
        HANDLE hp = FindFirstFileA((dir + "*.png").c_str(), &pic);
        if (hp != INVALID_HANDLE_VALUE) { do DeleteFileA((dir + pic.cFileName).c_str()); while (FindNextFileA(hp, &pic)); FindClose(hp); }
        RemoveDirectoryA((base + "-frames").c_str());
    }
}

const char* ModeName(int mode) {
    static const char* const names[] = { "nopass", "drawn", "kept", "none", "off" };
    return mode >= 0 && mode < 5 ? names[mode] : "?";
}

void Summary(std::string& out, const std::vector<UmFrameRec>& f) {
    if (f.empty()) { out += "no frames recorded\n"; return; }
    const double seconds = (f.back().tick - f.front().tick) / 1000.0;
    double dtMin = 1e9, dtMax = 0, dtSum = 0, rMax = 0, rSum = 0; int slow50 = 0, slow100 = 0;
    int modes[5] = {}, guards[3] = {}, dvkResult[8] = {};
    int padMin[4] = { 9999, 9999, 9999, 9999 }, padMax[4] = {};
    int radiusChanges = 0, mapChanges = 0, padChanges = 0, modeChanges = 0, resultChanges = 0;
    double centerJumpMax = 0; uint32_t drawsMin = 0xFFFFFFFFu, drawsMax = 0; double drawsSum = 0; int drawsN = 0;
    std::string events; int eventCount = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        const UmFrameRec& r = f[i];
        if (i > 0) { dtMin = std::fmin(dtMin, r.dtMs); dtMax = std::fmax(dtMax, r.dtMs); dtSum += r.dtMs; if (r.dtMs > 50) ++slow50; if (r.dtMs > 100) ++slow100; }
        rMax = std::fmax(rMax, r.renderMs); rSum += r.renderMs;
        if (r.mode < 5) ++modes[r.mode];
        if (r.guard < 3) ++guards[r.guard];
        for (int k = 0; k < 4; ++k) { padMin[k] = r.pad[k] < padMin[k] ? r.pad[k] : padMin[k]; padMax[k] = r.pad[k] > padMax[k] ? r.pad[k] : padMax[k]; }
        if (r.dvk[2] < 8) ++dvkResult[r.dvk[2]];
        if (r.mode == kModeDrawn || r.mode == kModeKept) { drawsMin = std::min(drawsMin, r.dvk[1]); drawsMax = std::max(drawsMax, r.dvk[1]); drawsSum += r.dvk[1]; ++drawsN; }
        if (i == 0) continue;
        const UmFrameRec& p = f[i - 1];
        std::string why;
        if (r.mode != p.mode) { ++modeChanges; why += Format(" mode %s->%s", ModeName(p.mode), ModeName(r.mode)); }
        if (r.radius != p.radius) { ++radiusChanges; why += Format(" radius %.0f->%.0f", p.radius, r.radius); }
        if (r.mapSize != p.mapSize) { ++mapChanges; why += Format(" map %u->%u", p.mapSize, r.mapSize); }
        if (std::memcmp(r.pad, p.pad, sizeof(r.pad)) != 0) {
            ++padChanges;
            int worst = 0; for (int k = 0; k < 4; ++k) worst = std::max(worst, std::abs(r.pad[k] - p.pad[k]));
            if (worst >= 24) why += Format(" pads jump %d px", worst);
        }
        if (r.dvk[2] != p.dvk[2]) { ++resultChanges; why += Format(" d7vk result %u->%u", p.dvk[2], r.dvk[2]); }
        if (r.guard != p.guard) why += Format(" guard %u->%u", p.guard, r.guard);
        const double jump = std::sqrt(double(r.center[0] - p.center[0]) * (r.center[0] - p.center[0]) + double(r.center[1] - p.center[1]) * (r.center[1] - p.center[1]) + double(r.center[2] - p.center[2]) * (r.center[2] - p.center[2]));
        centerJumpMax = std::fmax(centerJumpMax, jump);
        if (r.dtMs > 100) why += Format(" slow frame %.0f ms", r.dtMs);
        if (!why.empty() && eventCount < 60) { events += Format("  frame %u (+%d ms):%s\n", r.frame, static_cast<int>(r.tick - f.back().tick), why.c_str()); ++eventCount; }
    }
    out += Format("frames: %zu in %.1f s (%.1f per second)\n", f.size(), seconds, seconds > 0 ? f.size() / seconds : 0.0);
    out += Format("frame time ms: min %.1f avg %.1f max %.1f; over 50 ms: %d, over 100 ms: %d; the world pass itself avg %.1f max %.1f\n", dtMin > 1e8 ? 0.0 : dtMin, f.size() > 1 ? dtSum / (f.size() - 1) : 0.0, dtMax, slow50, slow100, rSum / f.size(), rMax);
    out += Format("shadow frames: drawn %d, kept the last parameters %d, none %d, off %d, no pass %d (mode changes: %d)\n", modes[kModeDrawn], modes[kModeKept], modes[kModeNone], modes[kModeOff], modes[kModeNoPass], modeChanges);
    out += Format("guard band: drawn %d, off %d, stacks not as expected %d; pads left %d..%d top %d..%d right %d..%d bottom %d..%d; pads changed in %d frames\n", guards[1], guards[0], guards[2],
                  padMin[0], padMax[0], padMin[1], padMax[1], padMin[2], padMax[2], padMin[3], padMax[3], padChanges);
    out += Format("area: radius changed %d times, map size %d times, centre moved at most %.2f units in one frame\n", radiusChanges, mapChanges, centerJumpMax);
    out += Format("D7VK result per frame: drawn %d, no draws %d, no targets %d, map %d, pipeline %d, depth %d, light %d (changes: %d); caster draws min %u avg %.0f max %u\n", dvkResult[0], dvkResult[1], dvkResult[2], dvkResult[3], dvkResult[4], dvkResult[5], dvkResult[6], resultChanges,
                  drawsN ? drawsMin : 0u, drawsN ? drawsSum / drawsN : 0.0, drawsMax);
    if (!events.empty()) out += "changes between frames (+ms = before the report):\n" + events;
}

void Table(std::string& out, const std::vector<UmFrameRec>& f) {
    out += "frame    +ms   dt  rnd mode   g  pads(l t r b)        camera(x y z)           target(x y)    radius height  map  centre(x y z)            light(x y z)        d7vk(draws res) hour\n";
    for (const UmFrameRec& r : f) {
        out += Format("%-7u %5d %4.0f %4.1f %-6s %u  %4d %4d %4d %4d  %7.1f %7.1f %6.1f  %7.1f %7.1f  %5.0f %6.1f %5u  %7.1f %7.1f %6.1f  %5.2f %5.2f %5.2f  %5u %u  %5.2f\n",
                      r.frame, static_cast<int>(r.tick - f.back().tick), r.dtMs, r.renderMs, ModeName(r.mode), r.guard, r.pad[0], r.pad[1], r.pad[2], r.pad[3], r.cam[0], r.cam[1], r.cam[2], r.target[0], r.target[1],
                      r.radius, r.height, r.mapSize, r.center[0], r.center[1], r.center[2], r.light[0], r.light[1], r.light[2], r.dvk[1], r.dvk[2], r.hour);
    }
}

std::vector<UmFrameRec> Ordered() {
    std::vector<UmFrameRec> f;
    f.reserve(g_ringCount);
    const size_t start = g_ringCount < g_ringCapacity ? 0 : g_ringNext;
    for (size_t i = 0; i < g_ringCount; ++i) f.push_back(g_ring[(start + i) % g_ringCapacity]);
    return f;
}

// Writes um-report-<time>.txt (and .png) into um-reports\; `why` says what asked for it. Returns the text file's path, "" when it failed.
std::string WriteReport(const char* why, bool picture, bool* pictureOk) {
    if (pictureOk) *pictureOk = false;
    const std::string folder = ReportFolder();
    if (folder.empty()) return std::string();
    CreateDirectoryA(folder.c_str(), nullptr);
    TrimReports(folder, static_cast<int>(Cfg().maxReports));
    SYSTEMTIME t; GetLocalTime(&t);
    const std::string base = folder + Format("um-report-%04d%02d%02d-%02d%02d%02d-%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    Lookup();
    bool shotOk = false;
    if (picture && g_shot) shotOk = g_shot((base + ".png").c_str(), Cfg().screenshot >= 1.5 ? 1 : 0) != 0;
    if (pictureOk) *pictureOk = shotOk;

    int burstPictures = 0;
    if (g_ringOn && g_ringDump) {
        CreateDirectoryA((base + "-frames").c_str(), nullptr);
        burstPictures = g_ringDump((base + "-frames\\f").c_str());
    }
    std::string out;
    out += Format("um-engine %s report, %04d-%02d-%02d %02d:%02d:%02d (%s)\n", UmEngineVersion(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, why);
    out += shotOk ? Format("picture: %s.png (the world as drawn at the end of the last row, without the interface; may be half size)\n", base.substr(base.find_last_of('\\') + 1).c_str()) : "picture: none\n";
    static const char* const cfgKeys[] = { "SunShadows::Enabled", "SunShadows::MapSize", "SunShadows::Strength", "SunShadows::Bias", "SunShadows::SlopeBias", "SunShadows::Margin", "SunShadows::Height",
        "SunShadows::Extra", "SunShadows::Indoor", "SunShadows::Guard", "SunShadows::GuardLog", "SunShadows::Debug" };
    double values[12] = {};
    out += "um-engine.cfg:";
    if (UmEngineConfigNumbers(cfgKeys, values, 12)) for (int i = 0; i < 12; ++i) out += Format(" %s=%g", cfgKeys[i] + 12, values[i]);
    out += "\n";
    if (burstPictures > 0) out += Format("burst: %d consecutive frames in %s-frames\\ (f<frame number>.png, 1/%d of the screen in each direction; the frame numbers are the table's first column)\n", burstPictures, base.substr(base.find_last_of('\\') + 1).c_str(), g_ringScale);
    const std::vector<UmFrameRec> frames = Ordered();
    Summary(out, frames);
    out += "\nrows are oldest first; the last row is the frame of the report (+ms is the time before it). mode: drawn = shadows with this frame's parameters, kept = last good ones, none = no shadows, off = not wanted.\n";
    Table(out, frames);

    const std::string path = base + ".txt";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return std::string();
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    UmEngineLog("um-engine: recorder: report %s (%s)%s", path.c_str(), why, shotOk ? " with a picture" : "");
    return path;
}

// ---- the game's functions the rig commands use ----
typedef void* (__attribute__((thiscall)) *CStringCtor)(void* self, const char* text);        // 0x7009DE
typedef void (__attribute__((thiscall)) *CStringDtor)(void* self);                            // 0x700970
typedef int (__attribute__((stdcall)) *ScriptExecute)(void* text, char run, void* out);       // 0x5785D0 WorldScript_Execute (text: a CString*, out: {CString, line, code})
typedef void (__attribute__((thiscall)) *CameraSetCarrier)(void* camera, int id);             // 0x40D420 CCamera::SetCarrierId
typedef void (__attribute__((thiscall)) *CameraSetTarget)(void* camera, const float* xyz);    // 0x40F5C0 CCamera::SetTarget
typedef void (__attribute__((fastcall)) *CameraUpdate)(void* camera);                         // 0x40F8B0 CCamera::UpdateFromTarget
typedef void (__attribute__((thiscall)) *CameraSetYaw)(void* camera, float angle);            // 0x4109E0 CCamera::SetYaw

bool WorldReady() { return Read<uintptr_t>(0x7AF068) != 0 && Read<uintptr_t>(kWorld + 0x180) != 0; }

// ---- requests: a client thread (the DLL server) hands one over, the game thread runs it at the end of its next world frame ----
enum Kind { kNone, kShot, kReport, kCam, kPause, kHour, kScript, kState };
struct Request { Kind kind = kNone; std::string text; double a = 0, b = 0, c = 0, d = 0; int count = 0; bool ok = false; std::string reply; };
Request g_req;
volatile LONG g_state = 0;   // 0 free, 1 handed over, 2 being run, 3 done
volatile LONG g_busy = 0;
HANDLE g_event = nullptr;

bool GameHasFocus() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

std::string StateLine() {
    if (!g_haveLast) return "no world frame yet";
    const UmFrameRec& r = g_last;
    return Format("frame=%u mode=%s guard=%u pads=%d,%d,%d,%d radius=%.0f height=%.1f map=%u d7vk_draws=%u d7vk_result=%u hour=%.2f dt_ms=%.1f render_ms=%.1f cam=%.1f,%.1f,%.1f target=%.1f,%.1f focus=%d ctrl=%d key=%d",
                  r.frame, ModeName(r.mode), r.guard, r.pad[0], r.pad[1], r.pad[2], r.pad[3], r.radius, r.height, r.mapSize, r.dvk[1], r.dvk[2], r.hour, r.dtMs, r.renderMs, r.cam[0], r.cam[1], r.cam[2], r.target[0], r.target[1],
                  GameHasFocus() ? 1 : 0, (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0, (GetAsyncKeyState(static_cast<int>(Cfg().key)) & 0x8000) ? 1 : 0);
}

void Run(Request& q) {
    q.ok = false;
    switch (q.kind) {
    case kShot: {
        Lookup();
        if (!g_shot) { q.reply = "d7vk.dll has no screenshot export"; return; }
        q.ok = g_shot(q.text.c_str(), q.count) != 0;   // count: 1 = half size
        q.reply = q.ok ? "file=\"" + q.text + "\"" : "the picture could not be read or written";
        return;
    }
    case kReport: {
        bool picture = false;
        const std::string path = WriteReport("command", Cfg().screenshot >= 0.5, &picture);
        q.ok = !path.empty();
        q.reply = q.ok ? "file=\"" + path + "\" picture=" + (picture ? "yes" : "no") : "the report could not be written";
        return;
    }
    case kState: q.ok = true; q.reply = StateLine(); return;
    case kCam: {
        if (!WorldReady()) { q.reply = "no world"; return; }
        void* camera = reinterpret_cast<void*>(kCamera);
        reinterpret_cast<CameraSetCarrier>(0x40D420)(camera, -1);   // not following a unit any more (a scroll key did this by hand)
        const float xyz[3] = { static_cast<float>(q.a), static_cast<float>(q.b), 0.0f };
        reinterpret_cast<CameraSetTarget>(0x40F5C0)(camera, xyz);    // the height comes from the terrain
        if (q.count >= 3 && q.c >= 2 && q.c <= 200) *reinterpret_cast<float*>(kCamera + 0xF0) = static_cast<float>(q.c);   // the distance
        if (q.count >= 4) reinterpret_cast<CameraSetYaw>(0x4109E0)(camera, static_cast<float>(q.d * 3.14159265358979323846 / 180.0));
        reinterpret_cast<CameraUpdate>(0x40F8B0)(camera);
        q.ok = true;
        q.reply = Format("target=%.1f,%.1f,%.1f distance=%.1f", Read<float>(kCamera + 0xE0), Read<float>(kCamera + 0xE4), Read<float>(kCamera + 0xE8), Read<float>(kCamera + 0xF0));
        return;
    }
    case kPause: {
        *reinterpret_cast<uint32_t*>(0x7AF624) = q.a != 0 ? 1u : 0u;   // the two flags the space bar sets (docs/game-memory.md)
        *reinterpret_cast<uint32_t*>(0x7AFFCC) = q.a != 0 ? 1u : 0u;
        q.ok = true; q.reply = q.a != 0 ? "paused" : "running";
        return;
    }
    case kHour: {
        if (!WorldReady()) { q.reply = "no world"; return; }
        const double tick = Read<uint32_t>(kWorld + 0x64), rate = Read<float>(kWorld + 0x20);   // hour = fmod(tick * rate + offset, 24)
        *reinterpret_cast<float*>(kWorld + 0x1C) = static_cast<float>(q.a - tick * rate);
        q.ok = true;
        q.reply = Format("hour=%.2f", std::fmod(Read<uint32_t>(kWorld + 0x64) * static_cast<double>(Read<float>(kWorld + 0x20)) + Read<float>(kWorld + 0x1C), 24.0));
        return;
    }
    case kScript: {
        if (!WorldReady()) { q.reply = "no world"; return; }
        // WorldScript_Execute: compiles the text into the client script VM and runs it (the console's @ prefix does the same behind its cheat switch: the line is wrapped in WorldScript( ... )). A value is printed with ConsoleFloat(x).
        const CStringCtor ctor = reinterpret_cast<CStringCtor>(0x7009DE); const CStringDtor dtor = reinterpret_cast<CStringDtor>(0x700970);
        void* text = nullptr; void* out[3] = { *reinterpret_cast<void* const*>(0x796E48), nullptr, nullptr };   // out starts as the empty CString
        ctor(&text, ("WorldScript( " + q.text + " )").c_str());   // the console's @ prefix wraps the line the same way
        const int result = reinterpret_cast<ScriptExecute>(0x5785D0)(&text, 1, out);
        const char* error = reinterpret_cast<const char*>(out[0]);
        q.ok = result != 0;
        q.reply = q.ok ? "ran" : std::string("script error: ") + (error ? error : "?");
        dtor(&text); dtor(&out[0]);
        return;
    }
    default: q.reply = "unknown request"; return;
    }
}

// Hands a request over and waits for the game thread; false (reply says why) when it did not run in time.
bool Submit(Kind kind, const std::string& text, double a, double b, double c, double d, int count, std::string& reply) {
    while (InterlockedCompareExchange(&g_busy, 1, 0) != 0) Sleep(5);
    if (!g_event) g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ResetEvent(g_event);
    g_req = Request(); g_req.kind = kind; g_req.text = text; g_req.a = a; g_req.b = b; g_req.c = c; g_req.d = d; g_req.count = count;
    InterlockedExchange(&g_state, 1);
    bool ran = WaitForSingleObject(g_event, 5000) == WAIT_OBJECT_0;
    if (!ran) {
        if (InterlockedCompareExchange(&g_state, 0, 1) == 1) {   // never picked up: no world frame is drawn (loading, minimized)
            reply = "the game did not run it within 5 s (no world frame is drawn: menu, loading, minimized?)";
            InterlockedExchange(&g_busy, 0);
            return false;
        }
        ran = WaitForSingleObject(g_event, 5000) == WAIT_OBJECT_0;   // it is running: let it finish
    }
    const bool ok = ran && g_req.ok;
    reply = ran ? g_req.reply : "the game did not finish it";
    InterlockedExchange(&g_state, 0);
    InterlockedExchange(&g_busy, 0);
    return ok;
}

bool g_keyWas = false; DWORD g_lastReportAt = 0; bool g_reportWanted = false; std::string g_reportReason;

} // namespace

extern "C" __attribute__((used, force_align_arg_pointer)) void UmRigFrameBegin() {
    LARGE_INTEGER now;
    if (g_freq.QuadPart == 0) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&now);
    g_prevBegin = g_beginAt; g_beginAt = now;
    g_umFrame = UmFrameRec();
    g_umFrame.tick = GetTickCount();
    g_umFrame.frame = ++g_frameNumber;
    g_umFrame.dtMs = g_prevBegin.QuadPart ? static_cast<float>((now.QuadPart - g_prevBegin.QuadPart) * 1000.0 / g_freq.QuadPart) : 0.0f;
    // the report key (with Ctrl when Recorder::Ctrl is 1), while the game has the focus (the edge of the key only; the focus is asked only then)
    const RigCfg& cfg = Cfg();
    if (cfg.enabled >= 0.5 && cfg.key >= 1) {
        const bool down = (GetAsyncKeyState(static_cast<int>(cfg.key)) & 0x8001) && (cfg.ctrl < 0.5 || (GetAsyncKeyState(VK_CONTROL) & 0x8000));   // 0x0001: pressed since the last call (a tap between two frames)
        if (down && !g_keyWas && GetTickCount() - g_lastReportAt > 2000 && GameHasFocus()) { g_reportWanted = true; g_reportReason = "hotkey"; }
        g_keyWas = down;
    }
}

extern "C" __attribute__((used, force_align_arg_pointer)) void UmRigFrameEnd() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    UmFrameRec& r = g_umFrame;
    r.renderMs = static_cast<float>((now.QuadPart - g_beginAt.QuadPart) * 1000.0 / g_freq.QuadPart);
    r.hour = static_cast<float>(std::fmod(Read<uint32_t>(kWorld + 0x64) * static_cast<double>(Read<float>(kWorld + 0x20)) + Read<float>(kWorld + 0x1C), 24.0));
    for (int i = 0; i < 3; ++i) r.cam[i] = Read<float>(kCamera + 0x1C + 4 * i);
    r.target[0] = Read<float>(kCamera + 0xE0); r.target[1] = Read<float>(kCamera + 0xE4);
    Lookup();
    if (g_stats) g_stats(r.dvk);
    const RigCfg& cfg = Cfg();
    if (cfg.frames >= 1) {
        const size_t want = static_cast<size_t>(cfg.frames);
        if (g_ringCapacity != want) { g_ring.assign(want, UmFrameRec()); g_ringCapacity = want; g_ringNext = 0; g_ringCount = 0; }
        g_ring[g_ringNext] = r;
        g_ringNext = (g_ringNext + 1) % g_ringCapacity;
        if (g_ringCount < g_ringCapacity) ++g_ringCount;
    }
    g_last = r; g_haveLast = true;
    if (cfg.enabled >= 0.5 && cfg.burst >= 0.5 && g_ringConfig && g_ringGrab) {   // the burst ring (a few ms a frame: the render target is read back)
        const int frames = static_cast<int>(cfg.burstFrames), scale = static_cast<int>(cfg.burstScale);
        if (!g_ringOn || frames != g_ringFrames || scale != g_ringScale) { g_ringConfig(frames, scale); g_ringFrames = frames; g_ringScale = scale; g_ringOn = true; UmEngineLog("um-engine: recorder: burst ring on (%d frames at 1/%d size)", frames, scale); }
        g_ringGrab(r.frame);
    } else if (g_ringOn && g_ringConfig) { g_ringConfig(0, 1); g_ringOn = false; UmEngineLog("um-engine: recorder: burst ring off"); }

    if (g_reportWanted) {
        g_reportWanted = false; g_lastReportAt = GetTickCount();
        bool picture = false;
        const std::string path = WriteReport(g_reportReason.c_str(), cfg.screenshot >= 0.5, &picture);
        if (path.empty()) UmEngineLog("um-engine: recorder: the report could not be written");
    }
    if (InterlockedCompareExchange(&g_state, 2, 1) == 1) {   // a request from the DLL server
        Run(g_req);
        InterlockedExchange(&g_state, 3);
        SetEvent(g_event);
    }
}

// um.dll's ENGINE command: one line in, rows of text out (OK / ERR first word). Runs on the DLL server's thread; the game's thread does the work.
extern "C" __declspec(dllexport) int UmEngineRig(const char* line, char* reply, int replySize) {
    if (!reply || replySize < 16) return 0;
    reply[0] = 0;
    auto answer = [&](bool ok, const std::string& text) { snprintf(reply, static_cast<size_t>(replySize), "%s", text.c_str()); return ok ? 1 : 0; };
    if (!line) return answer(false, "empty command");
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    const size_t space = s.find(' ');
    std::string verb = s.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string() : s.substr(s.find_first_not_of(' ', space));
    for (char& c : verb) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    std::string text;
    if (verb == "STATE") { const bool ok = Submit(kState, "", 0, 0, 0, 0, 0, text); return answer(ok, text); }
    if (verb == "REC") { const bool ok = Submit(kReport, "", 0, 0, 0, 0, 0, text); return answer(ok, text); }
    if (verb == "SHOT") {
        if (rest.empty() || rest.size() > MAX_PATH - 8) return answer(false, "usage: SHOT file.png [half]");
        std::string file = rest; int half = 0;
        if (file.size() > 5 && file.compare(file.size() - 5, 5, " half") == 0) { file.erase(file.size() - 5); half = 1; }
        const bool ok = Submit(kShot, file, 0, 0, 0, 0, half, text); return answer(ok, text);
    }
    if (verb == "CAM") {
        double v[4] = {}; int n = sscanf(rest.c_str(), "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]);
        if (n < 2) return answer(false, "usage: CAM x y [distance [yaw-degrees]]");
        const bool ok = Submit(kCam, "", v[0], v[1], v[2], v[3], n, text); return answer(ok, text);
    }
    if (verb == "PAUSE") {
        if (rest != "0" && rest != "1") return answer(false, "usage: PAUSE 0|1");
        const bool ok = Submit(kPause, "", rest == "1" ? 1 : 0, 0, 0, 0, 0, text); return answer(ok, text);
    }
    if (verb == "HOUR") {
        double h = 0; if (sscanf(rest.c_str(), "%lf", &h) != 1 || h < 0 || h >= 24) return answer(false, "usage: HOUR 0..24");
        const bool ok = Submit(kHour, "", h, 0, 0, 0, 0, text); return answer(ok, text);
    }
    if (verb == "SCRIPT") {
        if (rest.empty() || rest.size() > 480) return answer(false, "usage: SCRIPT mob-script-text (480 characters at most)");
        const bool ok = Submit(kScript, rest, 0, 0, 0, 0, 0, text); return answer(ok, text);
    }
    return answer(false, "unknown ENGINE command (STATE, REC, SHOT file.png [half], CAM x y [dist [yaw]], PAUSE 0|1, HOUR h, SCRIPT text)");
}
