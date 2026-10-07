// Problem alerts: see alerts.hpp. The sounds are decoded and played by miniaudio (vendor/miniaudio,
// public domain / MIT-0: MP3 decoding, and PulseAudio/ALSA on Linux, WASAPI on Windows, loaded at run
// time so no audio library is needed at build time). Without an audio device, sounds stay silent.

#include "alerts.hpp"
#include "log.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "imgui.h"

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_FLAC
#define MA_NO_WAV
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// The two sounds, built into the executable from sfx/ (the paths are relative to where make runs).
#if defined(_WIN32)
#define SFX_SECTION ".section .rdata,\"dr\"\n"
#else
#define SFX_SECTION ".section .rodata\n"
#endif
// Pointer-sized entries, and the C name of the table (32-bit Windows prefixes C symbols with '_').
#if __SIZEOF_POINTER__ == 8
#define SFX_PTR ".quad"
#else
#define SFX_PTR ".long"
#endif
#if defined(_WIN32) && !defined(_WIN64)
#define SFX_TABLE "_um_sfx_table"
#else
#define SFX_TABLE "um_sfx_table"
#endif
__asm__(SFX_SECTION
        ".balign 16\n"
        "um_sfx_error:\n.incbin \"sfx/ab-ap-dc.mp3\"\num_sfx_error_end:\n"
        ".balign 16\n"
        "um_sfx_warning:\n.incbin \"sfx/ab-athr.mp3\"\num_sfx_warning_end:\n"
        ".balign 8\n"
        ".globl " SFX_TABLE "\n"
        SFX_TABLE ":\n" SFX_PTR " um_sfx_error, um_sfx_error_end, um_sfx_warning, um_sfx_warning_end\n"
        ".text\n");
extern "C" const uint8_t* const um_sfx_table[4];

namespace alerts {

namespace {

bool g_sound = false;
bool g_popups = true;
float g_volume = 1.0f;

// ---- sound -----------------------------------------------------------------------------------------

// The audio device opens on the first sound, on a thread of its own (it can take a moment, and the
// GUI must not wait). Each sound is decoded once and replayed from the start.
struct Audio {
    std::mutex mutex;
    bool started = false, ready = false;
    ma_engine engine{};
    ma_decoder decoders[2]{};
    ma_sound sounds[2]{};
    bool loaded[2] = {false, false};
    std::chrono::steady_clock::time_point last[2]{};
};
Audio g_audio;

void PlayNow(int which) {
    std::lock_guard<std::mutex> lock(g_audio.mutex);
    if (!g_audio.ready || !g_audio.loaded[which]) return;
    // The same sound again within a second (several tabs at once): once is enough.
    const auto now = std::chrono::steady_clock::now();
    if (now - g_audio.last[which] < std::chrono::seconds(1)) return;
    g_audio.last[which] = now;
    ma_sound_set_volume(&g_audio.sounds[which], g_volume);
    ma_sound_seek_to_pcm_frame(&g_audio.sounds[which], 0);
    ma_sound_start(&g_audio.sounds[which]);
}

void Open(int firstSound) {
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (ma_engine_init(nullptr, &g_audio.engine) != MA_SUCCESS) return;
        for (int i = 0; i < 2; ++i) {
            const uint8_t* begin = um_sfx_table[i * 2];
            const size_t size = static_cast<size_t>(um_sfx_table[i * 2 + 1] - begin);
            if (ma_decoder_init_memory(begin, size, nullptr, &g_audio.decoders[i]) != MA_SUCCESS) continue;
            if (ma_sound_init_from_data_source(&g_audio.engine, &g_audio.decoders[i], MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                               &g_audio.sounds[i]) != MA_SUCCESS) {
                ma_decoder_uninit(&g_audio.decoders[i]);
                continue;
            }
            g_audio.loaded[i] = true;
        }
        g_audio.ready = true;
    }
    PlayNow(firstSound);
}

void Play(int which) {
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (!g_audio.started) {
            g_audio.started = true;
            std::thread(Open, which).detach();
            return;
        }
    }
    PlayNow(which); // nothing while the device is still opening
}

// ---- popup -----------------------------------------------------------------------------------------

struct Pending {
    std::string message, place;
    std::function<void()> goTo;
};
std::deque<Pending> g_pending; // errors not acknowledged yet, oldest first

} // namespace

void SetSound(bool enabled) { g_sound = enabled; }
void SetPopups(bool enabled) { g_popups = enabled; }
void SetVolume(int percent) { g_volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f; }

void PlaySound(Level level) { Play(level == Level::Error ? 0 : 1); }

void Raise(Level level, const std::string& message, const std::string& place, std::function<void()> goTo) {
    umlog::Write(level == Level::Error ? umlog::Level::Error : umlog::Level::Warning, message + (place.empty() ? "" : " [" + place + "]"));
    if (g_sound) PlaySound(level);
    if (level != Level::Error || !g_popups) return;
    // The same problem raised again while its popup is open: one popup.
    for (Pending& p : g_pending)
        if (p.message == message && p.place == place) { p.goTo = std::move(goTo); return; }
    if (g_pending.size() >= 8) g_pending.pop_front();
    g_pending.push_back({message, place, std::move(goTo)});
}

void Draw() {
    if (g_pending.empty()) return;
    const char* id = "Error detected##alert";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(720, FLT_MAX));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const Pending& p = g_pending.front();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.38f, 1), "%s", p.message.c_str());
    ImGui::PopTextWrapPos();
    if (g_pending.size() > 1) ImGui::TextDisabled("%zu more after this one", g_pending.size() - 1);
    ImGui::Spacing();
    bool close = false;
    if (p.goTo) {
        const std::string label = "Show" + (p.place.empty() ? std::string() : " (" + p.place + ")");
        if (ImGui::Button(label.c_str())) { p.goTo(); close = true; }
        ImGui::SameLine();
    }
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
    if (g_pending.size() > 1) {
        ImGui::SameLine();
        if (ImGui::Button("Close all")) { g_pending.clear(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    }
    if (close) {
        g_pending.pop_front();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_audio.mutex);
    if (!g_audio.ready) return;
    for (int i = 0; i < 2; ++i)
        if (g_audio.loaded[i]) { ma_sound_uninit(&g_audio.sounds[i]); ma_decoder_uninit(&g_audio.decoders[i]); }
    ma_engine_uninit(&g_audio.engine);
    g_audio.ready = false;
}

} // namespace alerts
