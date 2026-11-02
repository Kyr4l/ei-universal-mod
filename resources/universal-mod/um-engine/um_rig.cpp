// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The rig commands (docs/rig-commands.md).
//
// Every world frame leaves its state (what the sun shadows did: pads, area, map, D7VK's result, timings, camera) for ENGINE STATE.
// Rig commands (UmEngineRig, reached through um.dll's ENGINE command): run on the game's thread at the end of the next world frame: SHOT, STATE, CAM, PAUSE, HOUR, SCRIPT.
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "um_rig.h"

UmFrameRec g_umFrame;

namespace {

const uintptr_t kWorld = 0x7AF5B8;     // CWorldClient
const uintptr_t kCamera = 0x79B2A8;    // CCamera

template <typename T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }

// ---- the last world frame (STATE) ----
UmFrameRec g_last;
bool g_haveLast = false;
uint32_t g_frameNumber = 0;
LARGE_INTEGER g_freq, g_beginAt, g_prevBegin;

typedef void (*StatsFn)(uint32_t*);
typedef int (*ShotFn)(const char*, int);
StatsFn g_stats = nullptr; ShotFn g_shot = nullptr; bool g_looked = false;

void Lookup() {
    if (g_looked) return;
    g_looked = true;
    HMODULE module = GetModuleHandleA("d7vk.dll");
    if (!module) module = GetModuleHandleA("ddraw.dll");
    if (!module) return;
    g_stats = reinterpret_cast<StatsFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowStats")));
    g_shot = reinterpret_cast<ShotFn>(reinterpret_cast<void*>(GetProcAddress(module, "UmShadowScreenshot")));
}

std::string Format(const char* format, ...) {
    char text[1024];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

const char* ModeName(int mode) {
    static const char* const names[] = { "nopass", "drawn", "kept", "none", "off" };
    return mode >= 0 && mode < 5 ? names[mode] : "?";
}

// ---- the game's functions the rig commands use ----
typedef void* (__attribute__((thiscall)) *CStringCtor)(void* self, const char* text);        // 0x7009DE
typedef void (__attribute__((thiscall)) *CStringDtor)(void* self);                            // 0x700970
typedef int (__attribute__((stdcall)) *ScriptExecute)(void* text, char run, void* out);       // 0x5785D0 WorldScript_Execute (text: a CString*, out: {CString, code, line})
typedef void (__attribute__((thiscall)) *CameraSetCarrier)(void* camera, int id);             // 0x40D420 CCamera::FollowUnitId
typedef void (__attribute__((thiscall)) *CameraSetTarget)(void* camera, const float* xyz);    // 0x40F5C0 CCamera::SetTarget
typedef void (__attribute__((fastcall)) *CameraUpdate)(void* camera);                         // 0x40F8B0 CCamera::UpdateFromTarget
typedef void (__attribute__((thiscall)) *CameraSetYaw)(void* camera, float angle);            // 0x4109E0 CCamera::SetYaw

bool WorldReady() { return Read<uintptr_t>(0x7AF068) != 0 && Read<uintptr_t>(kWorld + 0x180) != 0; }

// ---- requests: a client thread (the DLL server) hands one over, the game thread runs it at the end of its next world frame ----
enum Kind { kNone, kShot, kCam, kPause, kHour, kScript, kState };
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
    return Format("frame=%u mode=%s guard=%u pads=%d,%d,%d,%d radius=%.0f height=%.1f map=%u d7vk_draws=%u d7vk_result=%u hour=%.2f dt_ms=%.1f render_ms=%.1f cam=%.1f,%.1f,%.1f target=%.1f,%.1f focus=%d",
                  r.frame, ModeName(r.mode), r.guard, r.pad[0], r.pad[1], r.pad[2], r.pad[3], r.radius, r.height, r.mapSize, r.dvk[1], r.dvk[2], r.hour, r.dtMs, r.renderMs, r.cam[0], r.cam[1], r.cam[2], r.target[0], r.target[1],
                  GameHasFocus() ? 1 : 0);
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

} // namespace

extern "C" __attribute__((used, force_align_arg_pointer)) void UmRigFrameBegin() {
    LARGE_INTEGER now;
    if (g_freq.QuadPart == 0) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&now);
    g_prevBegin = g_beginAt; g_beginAt = now;
    g_umFrame = UmFrameRec();
    g_umFrame.frame = ++g_frameNumber;
    g_umFrame.dtMs = g_prevBegin.QuadPart ? static_cast<float>((now.QuadPart - g_prevBegin.QuadPart) * 1000.0 / g_freq.QuadPart) : 0.0f;
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
    g_last = r; g_haveLast = true;
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
    return answer(false, "unknown ENGINE command (STATE, SHOT file.png [half], CAM x y [dist [yaw]], PAUSE 0|1, HOUR h, SCRIPT text)");
}
