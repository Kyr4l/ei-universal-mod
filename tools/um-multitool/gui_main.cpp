/**
 * ============================================================================
 * um-multitool GUI - Dear ImGui front-end, built into the um-multitool binary
 * ============================================================================
 *
 * Two main tabs:
 *   - File Processing: the five modding tools (ddsmmp, inireg, mobdump,
 *     restool, xlsxdb). A run launches this same executable again with the
 *     subcommand, as a hidden subprocess, and streams its stdout/stderr into
 *     the log panel below, with no separate console window.
 *   - 3D Viewer: the item model viewer (viewer/viewer_app.cpp).
 * Opened when the program is started without arguments from a file manager
 * (double-click), or with `um-multitool gui` - see main.cpp.
 *
 * Toolkit: Dear ImGui (vendor/imgui, vendored as source per its normal
 * distribution model) rendering through its OpenGL2 (legacy fixed-pipeline)
 * backend, windowed via GLFW. GLFW auto-selects X11 or Wayland on Linux from
 * the running session with no code on our end, and provides the Win32 window
 * on Windows - one codebase for all three targets. OpenGL2 (not OpenGL3) was
 * chosen specifically because it needs no GL function loader library and has
 * historically been the more robust legacy-GL path under Wine/older drivers.
 *
 * Each subtool gets its own sub-tab exposing every CLI flag it supports.
 * ============================================================================
 */

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include "gui.hpp"
#include "version.hpp"
#include "icon_data.hpp"
#include "viewer/viewer_app.hpp"
#include "viewer/library.hpp"
#include "viewer/ui_sources.hpp"
#include "mapedit/map_app.hpp"
#include "dllconnect/connector_app.hpp"
#include "viewer/dds_texture.hpp"

#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <climits>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// Native File/Folder Dialogs
// ============================================================================
//
// Dear ImGui draws widgets only - it has no file dialog of its own. On
// Windows we use the standard comdlg32/shell32 APIs (no extra dependency).
// On Linux there is no single "native" dialog API without pulling in GTK or
// Qt as a build dependency, so we shell out to whichever desktop file-picker
// is already on the user's system (zenity or kdialog - present on the vast
// majority of Linux desktops, and both go through the Wayland portal
// automatically when running under Wayland). If neither is installed, the
// path field is still a plain text box the user can type/paste into directly.

#ifndef _WIN32
static bool RunPickerCommand(const std::string& cmd, std::string& outPath) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    std::string result;
    while (fgets(buf, sizeof(buf), p)) result += buf;
    int status = pclose(p);
    if (status != 0) return false;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

static bool HasCommand(const char* name) {
    std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}
#endif

// filterName/filterExt are only honored on Windows (e.g. "Spreadsheet", "*.xlsx");
// pass nullptr for "all files". Linux file pickers are shown unfiltered.
// ---- background picture (Settings > Background) ------------------------------------------------------

// The GL texture of a picture file (.jpg, .png, .bmp, .dds, .mmp...), loaded again when the path changes; 0 when none.
static GLuint BackgroundTexture(const std::string& path, ImVec2& size) {
    static std::string loadedPath;
    static GLuint texture = 0;
    static ImVec2 loadedSize;
    if (path != loadedPath) {
        loadedPath = path;
        if (texture) { glDeleteTextures(1, &texture); texture = 0; }
        std::vector<uint8_t> bytes;
        if (!path.empty()) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (n > 0) { bytes.resize(static_cast<size_t>(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
                std::fclose(f);
            }
        }
        mmp::Image image;
        std::string err;
        if (!bytes.empty() && DecodeTextureFile(bytes, image, err)) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, image.rgba.data());
            loadedSize = ImVec2(static_cast<float>(image.width), static_cast<float>(image.height));
        }
    }
    size = loadedSize;
    return texture;
}

// The picture over the whole window, covering it (cropped to the window's shape, not stretched), blended
// over the plain background by `opacity`.
static void DrawBackground(GLuint texture, ImVec2 size, int fbW, int fbH, float opacity) {
    if (!texture || fbW <= 0 || fbH <= 0 || size.x <= 0 || size.y <= 0) return;
    const float win = static_cast<float>(fbW) / fbH, pic = size.x / size.y;
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (pic > win) { const float keep = win / pic; u0 = (1 - keep) * 0.5f; u1 = u0 + keep; }
    else { const float keep = pic / win; v0 = (1 - keep) * 0.5f; v1 = v0 + keep; }
    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, opacity);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(0, 0);
    glTexCoord2f(u1, v0); glVertex2f(1, 0);
    glTexCoord2f(u1, v1); glVertex2f(1, 1);
    glTexCoord2f(u0, v1); glVertex2f(0, 1);
    glEnd();
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
}

static bool NativePickFile(bool saveDialog, const char* filterName, const char* filterExt, std::string& outPath) {
#ifdef _WIN32
    char buf[MAX_PATH] = "";
    std::string filter;
    if (filterName && filterExt) {
        filter = std::string(filterName) + '\0' + filterExt + '\0' + "All Files" + '\0' + "*.*" + '\0';
    } else {
        filter = std::string("All Files") + '\0' + "*.*" + '\0';
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = saveDialog ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = saveDialog ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (ok) { outPath = buf; return true; }
    return false;
#else
    (void)filterName;
    (void)filterExt;
    if (HasCommand("zenity")) {
        return RunPickerCommand(saveDialog ? "zenity --file-selection --save --confirm-overwrite 2>/dev/null"
                                            : "zenity --file-selection 2>/dev/null", outPath);
    }
    if (HasCommand("kdialog")) {
        return RunPickerCommand(saveDialog ? "kdialog --getsavefilename 2>/dev/null"
                                            : "kdialog --getopenfilename 2>/dev/null", outPath);
    }
    return false;
#endif
}

static bool NativePickFolder(std::string& outPath) {
#ifdef _WIN32
    char displayName[MAX_PATH] = "";
    BROWSEINFOA bi{};
    bi.pszDisplayName = displayName;
    bi.lpszTitle = "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    if (ok) { outPath = path; return true; }
    return false;
#else
    if (HasCommand("zenity")) {
        return RunPickerCommand("zenity --file-selection --directory 2>/dev/null", outPath);
    }
    if (HasCommand("kdialog")) {
        return RunPickerCommand("kdialog --getexistingdirectory 2>/dev/null", outPath);
    }
    return false;
#endif
}

// ============================================================================
// Subprocess Launch Helpers (unchanged in spirit from the FLTK version: none
// of this depends on the GUI toolkit, only on the OS process APIs)
// ============================================================================

static std::string g_log;
static bool g_logDirty = false;

static void AppendLog(const std::string& text) {
    g_log += text;
    g_logDirty = true;
}

// The running executable itself: the GUI and the command-line tools are one binary, so a
// conversion runs this same program again with the subcommand.
static std::string FindMultitoolBinary() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::string(buf, len);
    return "um-multitool.exe";
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) return std::string(buf, static_cast<size_t>(len));
    return "um-multitool";
#endif
}

// Wraps a single argument in double quotes for cmd.exe's command-line parsing.
// Only used on Windows; POSIX spawns via execvp() with a real argv array, so
// no shell quoting is needed there at all.
#ifdef _WIN32
static std::string QuoteArg(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out += "\"";
    return out;
}
#endif

// Opaque handle to the spawned subprocess plus the read end of its output
// pipe, used both to poll its lifetime and to drain captured stdout/stderr.
struct ProcHandle {
#ifdef _WIN32
    HANDLE hProcess = nullptr;
    HANDLE hRead = nullptr;
#else
    pid_t pid = -1;
    int readFd = -1;
#endif
};

static bool IsValid(const ProcHandle& h) {
#ifdef _WIN32
    return h.hProcess != nullptr;
#else
    return h.pid > 0;
#endif
}

static std::mutex g_outputMutex;
static std::string g_pendingOutput;
static std::atomic<bool> g_readerAlive{false};

// Background thread body: blocks on read()/ReadFile() until the child closes
// its end of the pipe (process exited), appending each chunk for the main
// loop to drain. Never touches ImGui/GL state directly (wrong thread).
static void ReaderThreadFunc(ProcHandle proc) {
    char buf[4096];
    while (true) {
#ifdef _WIN32
        DWORD n = 0;
        BOOL ok = ReadFile(proc.hRead, buf, sizeof(buf), &n, nullptr);
        if (!ok || n == 0) break;
#else
        ssize_t n = read(proc.readFd, buf, sizeof(buf));
        if (n <= 0) break;
#endif
        std::lock_guard<std::mutex> lock(g_outputMutex);
        g_pendingOutput.append(buf, static_cast<size_t>(n));
    }
    g_readerAlive = false;
}

// Checks whether the process has exited without blocking. Returns the exit
// code via `exitCode` only once `stillRunning` comes back false.
static void PollProcess(const ProcHandle& h, bool& stillRunning, int& exitCode) {
#ifdef _WIN32
    DWORD code = 0;
    if (!GetExitCodeProcess(h.hProcess, &code) || code != STILL_ACTIVE) {
        stillRunning = false;
        exitCode = static_cast<int>(code);
        return;
    }
    stillRunning = true;
#else
    int status = 0;
    pid_t r = waitpid(h.pid, &status, WNOHANG);
    if (r == 0) {
        stillRunning = true;
        return;
    }
    stillRunning = false;
    exitCode = (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
}

static void CleanupProcess(ProcHandle& h) {
#ifdef _WIN32
    if (h.hProcess) CloseHandle(h.hProcess);
    if (h.hRead) CloseHandle(h.hRead);
#else
    if (h.readFd >= 0) close(h.readFd);
#endif
    h = ProcHandle{};
}

// Spawns `binary subcommand tokens...` with its stdout/stderr redirected into
// a pipe (no visible console on Windows, no shell/terminal on Linux), and
// starts a background thread that streams the captured output for the log
// panel to display. Returns an invalid handle (per IsValid) on failure.
static ProcHandle LaunchCaptured(const std::string& binary, const std::string& subcommand,
                                 const std::vector<std::string>& tokens, std::string& err) {
    ProcHandle proc;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        err = "Failed to create output pipe.";
        return proc;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    std::string cmdLine = QuoteArg(binary) + " " + subcommand;
    for (const auto& t : tokens) {
        cmdLine += " " + QuoteArg(t);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);
    if (!ok) {
        CloseHandle(hReadPipe);
        err = "Failed to launch um-multitool.";
        return proc;
    }
    CloseHandle(pi.hThread);
    proc.hProcess = pi.hProcess;
    proc.hRead = hReadPipe;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        err = "Failed to create output pipe.";
        return proc;
    }

    pid_t pid = fork();
    if (pid < 0) {
        err = "Failed to fork a new process.";
        close(fds[0]);
        close(fds[1]);
        return proc;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);

        std::vector<std::string> argStorage{binary, subcommand};
        argStorage.insert(argStorage.end(), tokens.begin(), tokens.end());
        std::vector<char*> argv;
        for (auto& a : argStorage) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(binary.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    proc.pid = pid;
    proc.readFd = fds[0];
#endif

    g_readerAlive = true;
    std::thread(ReaderThreadFunc, proc).detach();
    return proc;
}

// ============================================================================
// Shared Widget Helpers
// ============================================================================

// Renders "label: [text input] [File] [Folder?]" for a path field, matching
// the FLTK version's browsable rows. Writes the picked path straight into
// `buf` and returns true if the field's value changed this frame (typed or
// via a picker), so callers can react (e.g. auto-toggle multithread mode).
static bool PathRow(const char* imguiId, const char* label, char* buf, size_t bufSize,
                     bool saveDialog, bool showFolderButton,
                     const char* winFilterName = nullptr, const char* winFilterExt = nullptr) {
    bool changed = false;
    ImGui::PushID(imguiId);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90);
    float buttonsWidth = showFolderButton ? 170.0f : 80.0f;
    ImGui::SetNextItemWidth(-buttonsWidth - 10.0f);
    if (ImGui::InputText("##path", buf, bufSize)) changed = true;

    ImGui::SameLine();
    if (ImGui::Button("File", ImVec2(75, 0))) {
        std::string picked;
        if (NativePickFile(saveDialog, winFilterName, winFilterExt, picked)) {
            std::snprintf(buf, bufSize, "%s", picked.c_str());
            changed = true;
        }
    }
    if (showFolderButton) {
        ImGui::SameLine();
        if (ImGui::Button("Folder", ImVec2(75, 0))) {
            std::string picked;
            if (NativePickFolder(picked)) {
                std::snprintf(buf, bufSize, "%s", picked.c_str());
                changed = true;
            }
        }
    }

    ImGui::PopID();
    return changed;
}

// Appends "-d" immediately followed by inputPath if dirMode is set (matching
// the CLI parsers' lookahead), otherwise appends inputPath as a bare positional.
// Only restool actually needs this: its -d means "batch-process every item
// inside this folder", a genuinely different mode from packing one folder.
static void AppendDirAndPath(std::vector<std::string>& args, bool dirMode, const std::string& inputPath) {
    if (dirMode) {
        args.push_back("-d");
    }
    if (!inputPath.empty()) {
        args.push_back(inputPath);
    }
}

// ============================================================================
// DDS <-> MMP Tab
// ============================================================================

struct DdsMmpTab {
    char inputPath[1024] = "";
    char outputPath[1024] = "";
    bool multiThread = false;
    bool dryRun = false;
    int mode = 0; // 0=auto, 1=dds2mmp, 2=mmp2dds
};

static DdsMmpTab g_dds;

static void DrawDdsMmpTab() {
    if (PathRow("dds_in", "Input:", g_dds.inputPath, sizeof(g_dds.inputPath), false, true)) {
        std::error_code ec;
        g_dds.multiThread = fs::is_directory(g_dds.inputPath, ec);
    }
    PathRow("dds_out", "Output:", g_dds.outputPath, sizeof(g_dds.outputPath), true, false);

    ImGui::Checkbox("Multi-threaded (-m)", &g_dds.multiThread);
    ImGui::Checkbox("Dry run (--dry-run)", &g_dds.dryRun);

    ImGui::Spacing();
    ImGui::TextUnformatted("Conversion direction:");
    ImGui::RadioButton("Auto-detect (default)", &g_dds.mode, 0);
    ImGui::RadioButton("Force DDS -> MMP", &g_dds.mode, 1);
    ImGui::RadioButton("Force MMP -> DDS", &g_dds.mode, 2);
}

static std::vector<std::string> BuildDdsMmpArgs() {
    std::vector<std::string> args;
    if (g_dds.dryRun) args.push_back("--dry-run");
    if (g_dds.multiThread) args.push_back("-m");
    if (g_dds.mode == 1) args.push_back("--dds2mmp");
    else if (g_dds.mode == 2) args.push_back("--mmp2dds");

    if (g_dds.outputPath[0]) { args.push_back("-o"); args.push_back(g_dds.outputPath); }
    if (g_dds.inputPath[0]) args.push_back(g_dds.inputPath);
    return args;
}

// ============================================================================
// INI <-> REG Tab
// ============================================================================

struct IniRegTab {
    char inputPath[1024] = "";
    char outputPath[1024] = "";
    bool multiThread = false;
    bool dryRun = false;
    int mode = 0; // 0=auto, 1=ini2reg, 2=reg2ini
};

static IniRegTab g_ini;

static void DrawIniRegTab() {
    if (PathRow("ini_in", "Input:", g_ini.inputPath, sizeof(g_ini.inputPath), false, true)) {
        std::error_code ec;
        g_ini.multiThread = fs::is_directory(g_ini.inputPath, ec);
    }
    PathRow("ini_out", "Output:", g_ini.outputPath, sizeof(g_ini.outputPath), true, false);

    ImGui::Checkbox("Multi-threaded (-m)", &g_ini.multiThread);
    ImGui::Checkbox("Dry run (--dry-run)", &g_ini.dryRun);

    ImGui::Spacing();
    ImGui::TextUnformatted("Conversion direction:");
    ImGui::RadioButton("Auto-detect (default)", &g_ini.mode, 0);
    ImGui::RadioButton("Force INI -> REG", &g_ini.mode, 1);
    ImGui::RadioButton("Force REG -> INI", &g_ini.mode, 2);
}

static std::vector<std::string> BuildIniRegArgs() {
    std::vector<std::string> args;
    if (g_ini.dryRun) args.push_back("--dry-run");
    if (g_ini.multiThread) args.push_back("-m");
    if (g_ini.mode == 1) args.push_back("--ini2reg");
    else if (g_ini.mode == 2) args.push_back("--reg2ini");

    if (g_ini.outputPath[0]) { args.push_back("-o"); args.push_back(g_ini.outputPath); }
    if (g_ini.inputPath[0]) args.push_back(g_ini.inputPath);
    return args;
}

// ============================================================================
// MOB Dump Tab
// ============================================================================

struct MobDumpTab {
    char inputPath[1024] = "";
    char outputPath[1024] = "";
    bool multiThread = false;
    bool dryRun = false;
};

static MobDumpTab g_mob;

static void DrawMobDumpTab() {
    if (PathRow("mob_in", "Input:", g_mob.inputPath, sizeof(g_mob.inputPath), false, true)) {
        std::error_code ec;
        g_mob.multiThread = fs::is_directory(g_mob.inputPath, ec);
    }
    PathRow("mob_out", "Output:", g_mob.outputPath, sizeof(g_mob.outputPath), false, true);

    ImGui::Checkbox("Multi-threaded (-m)", &g_mob.multiThread);
    ImGui::Checkbox("Dry run (--dry-run)", &g_mob.dryRun);
}

static std::vector<std::string> BuildMobDumpArgs() {
    std::vector<std::string> args;
    if (g_mob.dryRun) args.push_back("--dry-run");
    if (g_mob.multiThread) args.push_back("-m");

    if (g_mob.outputPath[0]) { args.push_back("-o"); args.push_back(g_mob.outputPath); }
    if (g_mob.inputPath[0]) args.push_back(g_mob.inputPath);
    return args;
}

// ============================================================================
// RES/MQ Archive Tab (restool)
// ============================================================================

struct ResToolTab {
    char inputPath[1024] = "";
    char outputPath[1024] = "";
    char extOverride[64] = "";
    char excludeNames[512] = "";
    bool dirMode = false;
    bool multiThread = false;
    bool dryRun = false;
    bool stripExt = true;
    int action = 0; // 0=auto, 1=pack, 2=unpack
};

static ResToolTab g_res;

static void DrawResToolTab() {
    // Unlike ddsmmp/inireg/mobdump, restool's -d changes meaning rather than
    // being redundant: it batch-processes every item found inside the given
    // folder as a separate target, instead of packing that one folder itself.
    if (PathRow("res_in", "Input:", g_res.inputPath, sizeof(g_res.inputPath), false, true)) {
        std::error_code ec;
        g_res.multiThread = fs::is_directory(g_res.inputPath, ec);
    }
    PathRow("res_out", "Output:", g_res.outputPath, sizeof(g_res.outputPath), true, false);

    ImGui::SetNextItemWidth(150);
    ImGui::InputText("Ext override (--ext)", g_res.extOverride, sizeof(g_res.extOverride));
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Exclude (-e, comma-separated)", g_res.excludeNames, sizeof(g_res.excludeNames));

    ImGui::Checkbox("Batch mode (-d)", &g_res.dirMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Treats the input folder as a container of MANY archives/folders to\n"
            "process separately. Not needed to pack a single folder into one\n"
            "archive - that already happens automatically.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Multi-threaded (-m)", &g_res.multiThread);

    ImGui::Checkbox("Dry run (--dry-run)", &g_res.dryRun);
    ImGui::SameLine();
    ImGui::Checkbox("Strip _res/_mq suffix (-s)", &g_res.stripExt);

    ImGui::Spacing();
    ImGui::TextUnformatted("Action:");
    ImGui::RadioButton("Auto-detect (default)", &g_res.action, 0);
    ImGui::RadioButton("Force Pack (--pack)", &g_res.action, 1);
    ImGui::RadioButton("Force Unpack (--unpack)", &g_res.action, 2);
}

static std::vector<std::string> BuildResToolArgs() {
    std::vector<std::string> args;
    if (g_res.dryRun) args.push_back("--dry-run");
    if (g_res.multiThread) args.push_back("-m");
    if (!g_res.stripExt) args.push_back("--no-strip-ext");
    if (g_res.action == 1) args.push_back("--pack");
    else if (g_res.action == 2) args.push_back("--unpack");

    if (g_res.extOverride[0]) { args.push_back("--ext"); args.push_back(g_res.extOverride); }
    if (g_res.excludeNames[0]) { args.push_back("-e"); args.push_back(g_res.excludeNames); }
    if (g_res.outputPath[0]) { args.push_back("-o"); args.push_back(g_res.outputPath); }

    AppendDirAndPath(args, g_res.dirMode, g_res.inputPath);
    return args;
}

// ============================================================================
// XLSX -> RES Database Compiler Tab (xlsxdb)
// ============================================================================

struct XlsxDbTab {
    char inputPath[1024] = "";
    char outputPath[1024] = "";
};

static XlsxDbTab g_xlsxdb;

static void DrawXlsxDbTab() {
    PathRow("xlsx_in", "Input:", g_xlsxdb.inputPath, sizeof(g_xlsxdb.inputPath), false, false, "Spreadsheet", "*.xlsx");
    PathRow("xlsx_out", "Output:", g_xlsxdb.outputPath, sizeof(g_xlsxdb.outputPath), true, false, "RES Archive", "*.res");
    if (g_xlsxdb.outputPath[0] == '\0') {
        ImGui::TextDisabled("Optional. Defaults to the input's name with a .res extension.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Compiles an Evil Islands gameplay database spreadsheet (database.xlsx or "
        "databaselmp.xlsx) directly into a packed .res archive, detecting which "
        "database(s) the workbook contains sheets for. See "
        "docs/file-formats/database-format.md for the on-disk format.");
}

static std::vector<std::string> BuildXlsxDbArgs() {
    std::vector<std::string> args;
    if (g_xlsxdb.outputPath[0]) { args.push_back("-o"); args.push_back(g_xlsxdb.outputPath); }
    if (g_xlsxdb.inputPath[0]) args.push_back(g_xlsxdb.inputPath);
    return args;
}

// ============================================================================
// Run Button Dispatch & Main Loop
// ============================================================================

static ProcHandle g_activeProc;
static double g_progressPhase = 0.0;
static std::string g_statusText = "Idle";
static int g_activeTab = 0;

// Drains captured subprocess output into the log and, once the process has
// exited and the reader thread has drained the last of the pipe, resets the
// UI to idle. Called once per frame (the main loop already runs at the
// display's refresh rate, so no separate timer is needed like FLTK's).
static void PumpActiveProcess() {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        chunk.swap(g_pendingOutput);
    }
    if (!chunk.empty()) {
        AppendLog(chunk);
    }

    if (!IsValid(g_activeProc)) return;

    bool stillRunning = true;
    int exitCode = 0;
    PollProcess(g_activeProc, stillRunning, exitCode);

    if (!stillRunning && !g_readerAlive.load()) {
        CleanupProcess(g_activeProc);
        g_statusText = (exitCode == 0) ? "Job finished successfully" : "Job finished with errors";
        AppendLog(exitCode == 0
            ? "\n[Job finished successfully]\n\n"
            : ("\n[Job finished, exit code " + std::to_string(exitCode) + "]\n\n"));
        return;
    }

    g_progressPhase += 1.2;
    if (g_progressPhase > 100.0) g_progressPhase = 0.0;
}

struct TabInfo { const char* name; void (*draw)(); std::vector<std::string> (*buildArgs)(); const char* subcommand; const char* activeInput; };

static void OnRunClicked() {
    if (IsValid(g_activeProc)) return; // A job is already running.

    const char* activeInput = nullptr;
    std::vector<std::string> tokens;
    const char* subcommand = "";
    switch (g_activeTab) {
        case 0: activeInput = g_dds.inputPath; tokens = BuildDdsMmpArgs(); subcommand = "ddsmmp"; break;
        case 1: activeInput = g_ini.inputPath; tokens = BuildIniRegArgs(); subcommand = "inireg"; break;
        case 2: activeInput = g_mob.inputPath; tokens = BuildMobDumpArgs(); subcommand = "mobdump"; break;
        case 3: activeInput = g_res.inputPath; tokens = BuildResToolArgs(); subcommand = "restool"; break;
        case 4: activeInput = g_xlsxdb.inputPath; tokens = BuildXlsxDbArgs(); subcommand = "xlsxdb"; break;
        default: return;
    }

    if (!activeInput || activeInput[0] == '\0') {
        AppendLog("[ERROR] Please specify an input path.\n");
        return;
    }

    std::string binary = FindMultitoolBinary();
    std::ostringstream cmdEcho;
    cmdEcho << "$ um-multitool " << subcommand;
    for (const auto& t : tokens) cmdEcho << ' ' << t;
    AppendLog(cmdEcho.str() + "\n");

    std::string err;
    ProcHandle proc = LaunchCaptured(binary, subcommand, tokens, err);
    if (!IsValid(proc)) {
        AppendLog("[ERROR] " + err + "\n");
        g_statusText = "Failed to launch";
        return;
    }

    g_activeProc = proc;
    g_progressPhase = 0.0;
    g_statusText = "Running...";
}

static void DrawFileProcessingTab(const TabInfo* tabs, size_t count);

// Saves the window's pixels as a 24-bit BMP (the `gui --screenshot` option, for documentation and tests).
static void SaveScreenshot(const std::string& path, int w, int h) {
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    int rowSize = (w * 3 + 3) & ~3;
    unsigned dataSize = static_cast<unsigned>(rowSize * h), fileSize = 54 + dataSize, offset = 54, info = 40;
    unsigned short planes = 1, bpp = 24;
    unsigned char header[54] = {'B', 'M'};
    std::memcpy(header + 2, &fileSize, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &info, 4);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &dataSize, 4);
    std::fwrite(header, 1, 54, f);
    std::vector<unsigned char> row(rowSize, 0);
    for (int y = 0; y < h; ++y) { // bottom-up, like glReadPixels
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = rgb[(y * w + x) * 3 + 2];
            row[x * 3 + 1] = rgb[(y * w + x) * 3 + 1];
            row[x * 3 + 2] = rgb[(y * w + x) * 3 + 0];
        }
        std::fwrite(row.data(), 1, rowSize, f);
    }
    std::fclose(f);
}

// The built-in font only has basic Latin letters. Item texts can be French (accents), Russian
// (Cyrillic) or Korean, so system fonts are merged in behind it: ImGui 1.92 loads their glyphs on
// demand, only for characters the built-in font lacks. Missing fonts are skipped. Only TrueType
// (glyf) or classic CFF fonts load; the variable "-VF" Noto CJK fonts (CFF2) do not.
static void AddFallbackFonts(ImGuiIO& io) {
    io.Fonts->AddFontDefault();
    static const char* const candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",   // Latin accents, Cyrillic
        "C:\\Windows\\Fonts\\malgun.ttf",                                         // Korean
#else
        "/usr/share/fonts/truetype/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf", "/usr/share/fonts/nanum/NanumGothic.ttf",
        "/usr/share/fonts/TTF/NanumGothic.ttf", "/usr/share/fonts/truetype/NanumGothic.ttf",
#endif
    };
    bool haveLatin = false;
    for (const char* path : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        bool korean = std::strstr(path, "algun") || std::strstr(path, "anum");
        if (!korean && haveLatin) continue; // one Latin/Cyrillic font is enough
        ImFontConfig config;
        config.MergeMode = true;
        // Size 0: take the built-in font's size (1.92 refuses an explicit size when merging into it).
        if (io.Fonts->AddFontFromFileTTF(path, 0.0f, &config) && !korean) haveLatin = true;
    }
}

int RunGui(const GuiOptions& options) {
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
    });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_DEPTH_BITS, 24); // the 3D Viewer needs a depth buffer
    // The name desktops match against um-multitool.desktop (StartupWMClass / the Wayland app id): that is
    // where a Wayland desktop takes the window's icon from.
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "um-multitool");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "um-multitool");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "um-multitool");
    const std::string title = std::string(PROGRAM_NAME_SHOWN) + " " + PROGRAM_VERSION;
    // The window as it was last closed (um-multitool.cfg): size, maximized, and position where the
    // platform allows it (Wayland does not let a program place its window).
    const config::Config saved = config::Load();
    const bool wayland = glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
    // Created at the normal size and maximized once shown: a window that starts maximized gives the
    // window manager no size to go back to when it is un-maximized (KWin then keeps the full screen).
    GLFWwindow* window = glfwCreateWindow(std::max(saved.windowW, 640), std::max(saved.windowH, 400), title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    if (!wayland && saved.windowX != -100000) glfwSetWindowPos(window, saved.windowX, saved.windowY);
    glfwMakeContextCurrent(window);
    // The window icon (the battle axe, icon_data.hpp). Wayland has no way to set one from the program.
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        GLFWimage icons[3] = {{64, 64, const_cast<unsigned char*>(logo::kIcon64)},
                              {48, 48, const_cast<unsigned char*>(logo::kIcon48)},
                              {32, 32, const_cast<unsigned char*>(logo::kIcon32)}};
        glfwSetWindowIcon(window, 3, icons);
    }
    glfwSwapInterval(1); // vsync; also paces our polling loop like the old 60ms timer did

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Ctrl+Tab is the Map Editor's logic mode key (by default): not ImGui's window switcher.
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;

    // Default Dear ImGui look and colors - no theme customization.
    AddFallbackFonts(io);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    // The sources (figures, textures, texts, database) and settings, shared by the 3D Viewer and the
    // Map Editor and edited in the Settings tab.
    Library library;
    library.LoadConfig();
    ui::SourcesState sourcesState;
    std::snprintf(sourcesState.databasePath, sizeof(sourcesState.databasePath), "%s", library.dbPath.c_str());

    viewer::Context* viewerCtx = viewer::Create(library);
    mapedit::Context* mapCtx = mapedit::Create(library);
    dllconnect::Context* dllCtx = dllconnect::Create(library);
    // Saved in the config as numbers (GUI_TAB, BACKGROUND_*): new tabs are added at the end, whatever their place.
    enum { kFiles, kViewer, kMap, kSettings, kDll, kNone };
    // The tab asked for on the command line, else the one open when the GUI was last closed.
    int requestedTab = options.openSettings ? kSettings : options.openMap ? kMap : options.openViewer ? kViewer
                     : (library.guiTab >= kFiles && library.guiTab <= kDll ? library.guiTab : kNone);
    if (!options.viewerCategory.empty()) {
        std::string err;
        if (!viewer::OpenItem(viewerCtx, options.viewerCategory, options.viewerItem, err)) std::fprintf(stderr, "%s\n", err.c_str());
        requestedTab = kViewer;
    }
    if (!options.mapFiles.empty()) mapedit::OpenFiles(mapCtx, options.mapFiles);
    bool seeThrough = false;       // a 3D tab was shown last frame (its viewport must see through the window)
    int frameCount = 0;
    double lastTime = glfwGetTime();

    const TabInfo tabs[] = {
        {"DDS <-> MMP", DrawDdsMmpTab, BuildDdsMmpArgs, "ddsmmp", nullptr},
        {"INI <-> REG", DrawIniRegTab, BuildIniRegArgs, "inireg", nullptr},
        {"MOB Dump", DrawMobDumpTab, BuildMobDumpArgs, "mobdump", nullptr},
        {"RES / MQ", DrawResToolTab, BuildResToolArgs, "restool", nullptr},
        {"XLSX -> RES", DrawXlsxDbTab, BuildXlsxDbArgs, "xlsxdb", nullptr},
    };

    int maximizeIn = saved.windowMaximized ? 3 : 0; // frames: after the window is shown (Wayland maps it at the first swap)
    int normalW = std::max(saved.windowW, 640), normalH = std::max(saved.windowH, 400), normalX = saved.windowX, normalY = saved.windowY;
    ImVec2 bgSize(0, 0);
    GLFWwindow* scriptWin = nullptr;   // the script's own window, while it is open
    ImGuiContext* scriptImgui = nullptr;
    auto closeScriptWindow = [&]() {
        if (!scriptWin) return;
        ImGuiContext* mainImgui = ImGui::GetCurrentContext();
        glfwMakeContextCurrent(scriptWin);
        ImGui::SetCurrentContext(scriptImgui);
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(scriptImgui);
        ImGui::SetCurrentContext(mainImgui);
        glfwDestroyWindow(scriptWin);
        scriptWin = nullptr;
        scriptImgui = nullptr;
        glfwMakeContextCurrent(window);
    };
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (maximizeIn > 0) { if (--maximizeIn == 0) glfwMaximizeWindow(window); }
        // The last normal (not maximized) size and place, kept for the next start.
        else if (!glfwGetWindowAttrib(window, GLFW_MAXIMIZED) && !glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwGetWindowSize(window, &normalW, &normalH);
            if (!wayland) glfwGetWindowPos(window, &normalX, &normalY);
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(16);
            continue;
        }

        PumpActiveProcess();
        dllconnect::Update(dllCtx);
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fbW) / io.DisplayFramebufferScale.x,
                                         static_cast<float>(fbH) / io.DisplayFramebufferScale.y));
        // A background picture for the tab shown last frame (Settings > Background): drawn under ImGui like the 3D views.
        const int bgTab = library.guiTab >= kFiles && library.guiTab <= kDll ? library.guiTab : kFiles;
        const std::string& bgPath = !library.tabBackground[bgTab].empty() ? library.tabBackground[bgTab] : library.background;
        const GLuint bgTexture = BackgroundTexture(bgPath, bgSize);
        ui::BackgroundShown() = bgTexture != 0 && library.backgroundOpacity > 0.0f;
        if (seeThrough || ui::BackgroundShown()) ImGui::SetNextWindowBgAlpha(0.0f); // drawn underneath ImGui
        ImGui::Begin("##main", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        bool shown3d = false;
        int shownTab = kNone;
        if (ImGui::BeginTabBar("##maintabs")) {
            auto flags = [&](int tab) { return requestedTab == tab ? ImGuiTabItemFlags_SetSelected : 0; };
            if (ImGui::BeginTabItem("File Processing", nullptr, flags(kFiles))) {
                DrawFileProcessingTab(tabs, std::size(tabs));
                shownTab = kFiles;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("3D Viewer", nullptr, flags(kViewer))) {
                viewer::DrawTab(viewerCtx);
                shown3d = true;
                shownTab = kViewer;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Map Editor", nullptr, flags(kMap))) {
                mapedit::DrawTab(mapCtx);
                shown3d = true;
                shownTab = kMap;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("UM DLL Connector", nullptr, flags(kDll))) {
                dllconnect::DrawTab(dllCtx);
                shownTab = kDll;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings", nullptr, flags(kSettings))) {
                ui::SettingsTab(library, sourcesState);
                shownTab = kSettings;
                ImGui::EndTabItem();
            }
            requestedTab = kNone;
            ImGui::EndTabBar();
        }
        if (shownTab != kNone && shownTab != library.guiTab) { // remembered for the next start
            library.guiTab = shownTab;
            library.SaveConfig();
        }
        seeThrough = shown3d;
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (ui::BackgroundShown()) DrawBackground(bgTexture, bgSize, fbW, fbH, library.backgroundOpacity);
        viewer::RenderGl(viewerCtx, fbW, fbH, io.DisplayFramebufferScale.x, dt);
        mapedit::RenderGl(mapCtx, fbW, fbH, io.DisplayFramebufferScale.x);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        if (!options.screenshotPath.empty() && ++frameCount >= 8 && (!mapedit::Busy(mapCtx) || frameCount > 3000)) {
            SaveScreenshot(options.screenshotPath, fbW, fbH);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        glfwSwapBuffers(window);

        // The Map Editor's script in a window of its own: a second OS window (sharing the GL objects)
        // with its own ImGui context, drawn after the main one each frame.
        const bool scriptWanted = mapedit::ScriptWindowWanted(mapCtx);
        ImGuiContext* mainImgui = ImGui::GetCurrentContext();
        if (scriptWanted && !scriptWin) {
            const ImGuiStyle style = ImGui::GetStyle();
            glfwDefaultWindowHints();
            glfwWindowHintString(GLFW_WAYLAND_APP_ID, "um-multitool");
            glfwWindowHintString(GLFW_X11_CLASS_NAME, "um-multitool");
            glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "um-multitool");
            const std::string scriptTitle = "Script - " + std::string(PROGRAM_NAME_SHOWN);
            scriptWin = glfwCreateWindow(960, 720, scriptTitle.c_str(), nullptr, window);
            if (scriptWin) {
                glfwMakeContextCurrent(scriptWin);
                glfwSwapInterval(0); // the main window's swap keeps the pace
                scriptImgui = ImGui::CreateContext();
                ImGui::SetCurrentContext(scriptImgui);
                ImGui::GetStyle() = style;
                ImGui::GetIO().IniFilename = nullptr;
                AddFallbackFonts(ImGui::GetIO());
                ImGui_ImplGlfw_InitForOpenGL(scriptWin, true);
                ImGui_ImplOpenGL2_Init();
                ImGui::SetCurrentContext(mainImgui);
                glfwMakeContextCurrent(window);
            } else {
                mapedit::CloseScriptWindow(mapCtx);
            }
        }
        if (scriptWin && scriptWanted) {
            glfwMakeContextCurrent(scriptWin);
            ImGui::SetCurrentContext(scriptImgui);
            if (mapedit::TakeScriptWindowFocus(mapCtx)) glfwFocusWindow(scriptWin);
            ImGui_ImplOpenGL2_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            mapedit::DrawScriptWindow(mapCtx);
            ImGui::Render();
            int sw, sh;
            glfwGetFramebufferSize(scriptWin, &sw, &sh);
            glViewport(0, 0, sw, sh);
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(scriptWin);
            ImGui::SetCurrentContext(mainImgui);
            glfwMakeContextCurrent(window);
            if (glfwWindowShouldClose(scriptWin)) mapedit::CloseScriptWindow(mapCtx);
        }
        if (scriptWin && !mapedit::ScriptWindowWanted(mapCtx)) closeScriptWindow();
    }
    closeScriptWindow();

    viewer::Destroy(viewerCtx);
    mapedit::Destroy(mapCtx);
    dllconnect::Destroy(dllCtx);
    // Remember the window for the next start.
    library.windowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;
    library.windowW = normalW;
    library.windowH = normalH;
    if (!wayland) { library.windowX = normalX; library.windowY = normalY; }
    library.SaveConfig();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// The File Processing tab: one sub-tab per tool, the Run button and the log.
static void DrawFileProcessingTab(const TabInfo* tabs, size_t count) {
    if (ImGui::BeginTabBar("##tabs")) {
        for (int i = 0; i < static_cast<int>(count); ++i) {
            if (ImGui::BeginTabItem(tabs[i].name)) {
                g_activeTab = i;
                ImGui::Spacing();
                tabs[i].draw();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool running = IsValid(g_activeProc);
    ImGui::BeginDisabled(running);
    if (ImGui::Button(running ? "Running..." : "Run", ImVec2(110, 32))) {
        OnRunClicked();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Log", ImVec2(110, 32))) {
        g_log.clear();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(g_statusText.c_str());

    float progressFrac = running ? static_cast<float>(g_progressPhase / 100.0) : 0.0f;
    ImGui::ProgressBar(progressFrac, ImVec2(-1, 0), running ? "Running..." : "Idle");

    ImGui::Spacing();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(g_log.c_str());
    if (g_logDirty) {
        ImGui::SetScrollHereY(1.0f);
        g_logDirty = false;
    }
    ImGui::EndChild();
}
