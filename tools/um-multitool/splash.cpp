// The splash screen (splash.hpp): its own GLFW window and its own Dear ImGui context, so it can be drawn before
// the main window's context exists and torn down without touching it. The banner PNG is embedded in the
// executable the way the alert sounds are (alerts.cpp: .incbin, so no generated source to keep in sync).
#include "splash.hpp"

#include <GLFW/glfw3.h>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "vendor/stb/stb_image.h"
#include "version.hpp"

#if defined(_WIN32)
#define SPLASH_SECTION ".section .rdata,\"dr\"\n"
#else
#define SPLASH_SECTION ".section .rodata\n"
#endif
#if __SIZEOF_POINTER__ == 8
#define SPLASH_PTR ".quad"
#else
#define SPLASH_PTR ".long"
#endif
#if defined(_WIN32) && !defined(_WIN64)
#define SPLASH_TABLE "_um_splash_table"
#else
#define SPLASH_TABLE "um_splash_table"
#endif
__asm__(SPLASH_SECTION
        ".balign 16\n"
        "um_splash_png:\n.incbin \"assets/splash.png\"\num_splash_png_end:\n"
        ".balign 8\n"
        ".globl " SPLASH_TABLE "\n"
        SPLASH_TABLE ":\n" SPLASH_PTR " um_splash_png, um_splash_png_end\n"
        ".text\n");
extern "C" const uint8_t* const um_splash_table[2];
void AddFallbackFonts(ImGuiIO& io); // gui_main.cpp: the GUI's font, so the splash reads the same

namespace splash {

namespace {

GLFWwindow* g_main = nullptr;
GLFWwindow* g_win = nullptr;
ImGuiContext* g_ctx = nullptr;
GLuint g_banner = 0;
int g_w = 480, g_h = 300;
std::string g_text;
float g_fraction = 0;
bool g_darkBand = false; // the banner's band is dark: light text, a dark trough
ImU32 g_accent = IM_COL32(70, 120, 200, 255);
ImFont* g_bold = nullptr;   // the version's face: a bold system font next to the GUI's, when there is one
std::chrono::steady_clock::time_point g_shownAt;
constexpr double kMinSeconds = 1.2;  // the splash stays at least this long: a flash reads as a glitch
constexpr int kBand = 44;            // the bottom band holds the status line and the bar (the banner leaves it quiet)

// The banner's pixels: the file beside the executable when there is one, else the embedded one.
bool LoadBanner(const std::string& overridePath, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::vector<uint8_t> bytes;
    if (!overridePath.empty()) {
        std::ifstream in(overridePath, std::ios::binary);
        if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) bytes.assign(um_splash_table[0], um_splash_table[1]);
    int channels = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!px) return false;
    rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

void Draw() {
    if (!g_win) return;
    GLFWwindow* previous = glfwGetCurrentContext();
    ImGuiContext* previousCtx = ImGui::GetCurrentContext();
    glfwMakeContextCurrent(g_win);
    ImGui::SetCurrentContext(g_ctx);
    glfwPollEvents();
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##splash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    if (g_banner) ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(g_banner)), ImVec2(0, 0), ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    else ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)), IM_COL32(40, 50, 70, 255));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float top = static_cast<float>(g_h - kBand);
    { // the version, bottom right above the band (the banner leaves the place), from version.hpp
        const std::string v = PROGRAM_VERSION; // the number alone, bold, in the accent: as the banner previews show it
        ImFont* f = g_bold ? g_bold : ImGui::GetFont();
        const float size = 17.0f;
        const ImVec2 tw = f->CalcTextSizeA(size, FLT_MAX, 0.0f, v.c_str());
        const ImVec2 at(static_cast<float>(g_w) - 14.0f - tw.x, top - 46.0f);
        dl->AddText(f, size, at, g_darkBand ? g_accent : IM_COL32(40, 50, 80, 255), v.c_str());
    }
    // The band: the status line, the bar in the 2005 way (a flat blue fill in a sunken trough).
    dl->AddText(ImVec2(12.0f, top + 7.0f), g_darkBand ? IM_COL32(205, 210, 220, 255) : IM_COL32(60, 60, 60, 255), g_text.c_str());
    const float bx0 = 12.0f, bx1 = static_cast<float>(g_w) - 12.0f, by0 = static_cast<float>(g_h) - 17.0f, by1 = static_cast<float>(g_h) - 8.0f;
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), g_darkBand ? IM_COL32(16, 18, 24, 255) : IM_COL32(250, 250, 250, 255));
    dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), g_darkBand ? IM_COL32(110, 120, 140, 255) : IM_COL32(140, 140, 140, 255));
    const float fill = bx0 + 1.0f + (bx1 - bx0 - 2.0f) * (g_fraction < 0 ? 0 : g_fraction > 1 ? 1 : g_fraction);
    if (fill > bx0 + 1.0f) dl->AddRectFilled(ImVec2(bx0 + 1.0f, by0 + 1.0f), ImVec2(fill, by1 - 1.0f), g_accent);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::Render();
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(g_win, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(g_win);
    ImGui::SetCurrentContext(previousCtx);
    glfwMakeContextCurrent(previous);
}

} // namespace

void Begin(GLFWwindow* mainWindow, const std::string& overridePath, int accentR, int accentG, int accentB) {
    g_main = mainWindow;
    g_accent = IM_COL32(accentR, accentG, accentB, 255);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    const bool haveBanner = LoadBanner(overridePath, rgba, w, h);
    if (haveBanner) {
        g_w = w; g_h = h;
        long sum = 0; int n = 0; // the band's brightness, sampled along its left edge
        for (int y = h - kBand + 2; y < h - 2; ++y) for (int x = 2; x < 8; ++x) { const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4]; sum += p[0] + p[1] + p[2]; ++n; }
        g_darkBand = n > 0 && sum / n < 384;
    }
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "um-multitool");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "um-multitool");
    g_win = glfwCreateWindow(g_w, g_h, "um-multitool", nullptr, nullptr);
    if (!g_win) return;
    // Centred on the primary monitor's work area where the platform lets a program place a window.
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        if (mw > 0 && mh > 0) glfwSetWindowPos(g_win, mx + (mw - g_w) / 2, my + (mh - g_h) / 2);
    }
    glfwMakeContextCurrent(g_win);
    glfwSwapInterval(1);
    ImGuiContext* previousCtx = ImGui::GetCurrentContext();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGui::GetIO().IniFilename = nullptr;
    AddFallbackFonts(ImGui::GetIO());
    {
        static const char* const bold[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf",
#else
            "/usr/share/fonts/truetype/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
#endif
        };
        for (const char* path : bold) {
            if (g_bold) break;
            if (std::ifstream(path, std::ios::binary)) g_bold = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 17.0f);
        }
    }
    ImGui_ImplGlfw_InitForOpenGL(g_win, true);
    ImGui_ImplOpenGL2_Init();
    if (haveBanner) {
        glGenTextures(1, &g_banner);
        glBindTexture(GL_TEXTURE_2D, g_banner);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    ImGui::SetCurrentContext(previousCtx);
    glfwMakeContextCurrent(g_main);
    glfwShowWindow(g_win);
    g_shownAt = std::chrono::steady_clock::now();
    Draw();
    Draw(); // the first frame builds the font atlas; the second is the one seen
}

void Step(const std::string& text, float fraction) {
    g_text = text;
    g_fraction = fraction;
    Draw();
}

void End() {
    if (!g_win) return;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - g_shownAt).count() < kMinSeconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Draw();
    }
    GLFWwindow* previous = glfwGetCurrentContext();
    ImGuiContext* previousCtx = ImGui::GetCurrentContext();
    GLFWwindow* const restoreWindow = previous == g_win ? g_main : previous;
    ImGuiContext* const restoreCtx = previousCtx == g_ctx ? nullptr : previousCtx;
    glfwMakeContextCurrent(g_win);
    ImGui::SetCurrentContext(g_ctx);
    if (g_banner) { glDeleteTextures(1, &g_banner); g_banner = 0; }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(g_ctx);
    g_ctx = nullptr;
    ImGui::SetCurrentContext(restoreCtx);
    glfwDestroyWindow(g_win);
    g_win = nullptr;
    glfwMakeContextCurrent(restoreWindow);
}

} // namespace splash
