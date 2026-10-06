/**
 * ============================================================================
 * um-multitool GUI - Dear ImGui front-end, built into the um-multitool binary
 * ============================================================================
 *
 * Two main tabs:
 *   - File Processing: the DB editor (db_editor.cpp: open, check, edit, save
 *     and compile the gameplay databases, in-process) and the file tools
 *     (restool, inireg, ddsmmp, mobdump). A tool run launches this same
 *     executable again with the subcommand, as a hidden subprocess, and
 *     streams its stdout/stderr into the log panel below, with no separate
 *     console window. Problems found anywhere raise alerts (alerts.hpp).
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
#include "texedit/texture_app.hpp"
#include "alerts.hpp"
#include "i18n.hpp"
#include "db_editor.hpp"
#include "text_editor.hpp"
#include "mp_editor.hpp"
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
        if (ImGui::Button("Folder...", ImVec2(75, 0))) {
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
    bool plain32 = false;
};

static DdsMmpTab g_dds;

static void DrawDdsMmpTab() {
    if (PathRow("dds_in", "Input:", g_dds.inputPath, sizeof(g_dds.inputPath), false, true)) {
        std::error_code ec;
        g_dds.multiThread = fs::is_directory(g_dds.inputPath, ec);
    }
    PathRow("dds_out", "Output:", g_dds.outputPath, sizeof(g_dds.outputPath), true, true);

    ImGui::Checkbox("Multi-threaded (-m)", &g_dds.multiThread);
    ImGui::Checkbox("Dry run (--dry-run)", &g_dds.dryRun);

    ImGui::Spacing();
    ImGui::TextUnformatted("Conversion direction:");
    ImGui::RadioButton("Auto-detect (default)", &g_dds.mode, 0);
    ImGui::RadioButton("Force DDS -> MMP", &g_dds.mode, 1);
    ImGui::RadioButton("Force MMP -> DDS", &g_dds.mode, 2);
    ImGui::Checkbox("32-bit as plain ARGB 8888 (--plain32)", &g_dds.plain32);
    ImGui::SetItemTooltip("DDS -> MMP: write 32-bit textures in the format of the game's cursors and logos instead of PNT3 (packed, first mipmap only)");
}

static std::vector<std::string> BuildDdsMmpArgs() {
    std::vector<std::string> args;
    if (g_dds.dryRun) args.push_back("--dry-run");
    if (g_dds.multiThread) args.push_back("-m");
    if (g_dds.mode == 1) args.push_back("--dds2mmp");
    else if (g_dds.mode == 2) args.push_back("--mmp2dds");
    if (g_dds.plain32) args.push_back("--plain32");

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
    PathRow("ini_out", "Output:", g_ini.outputPath, sizeof(g_ini.outputPath), true, true);

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
    bool groupTexts = false;
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
    PathRow("res_out", "Output:", g_res.outputPath, sizeof(g_res.outputPath), true, true);

    ImGui::SetNextItemWidth(150);
    ImGui::InputText("Archive extension (--ext)", g_res.extOverride, sizeof(g_res.extOverride));
    ImGui::SetItemTooltip("Packing: the extension of the archive made (.res or .mq). Empty: from the folder's suffix\n"
                          "(foo_res -> foo.res, z3q1_mq -> z3q1.mq). Only needed when the folder name does not say.");
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Exclude (-e, comma-separated)", g_res.excludeNames, sizeof(g_res.excludeNames));

    ImGui::Checkbox("Batch mode (-d)", &g_res.dirMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Processes each archive or folder inside the input folder separately. Not needed to pack one folder into one archive.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Multi-threaded (-m)", &g_res.multiThread);

    ImGui::Checkbox("Dry run (--dry-run)", &g_res.dryRun);
    ImGui::SameLine();
    ImGui::Checkbox("Strip _res/_mq suffix (-s)", &g_res.stripExt);
    ImGui::Checkbox("Texts: grouped .umtexts files (--grouped-texts)", &g_res.groupTexts);
    ImGui::SetItemTooltip("Unpacking texts.res / textslmp.res: one <TYPE>.umtexts file per string type (ARMOR, WEAPON...)\n"
                          "instead of one file per entry. Packing a folder of .umtexts files works without it.");

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
    if (g_res.groupTexts) args.push_back("--grouped-texts");
    if (g_res.action == 1) args.push_back("--pack");
    else if (g_res.action == 2) args.push_back("--unpack");

    if (g_res.extOverride[0]) { args.push_back("--ext"); args.push_back(g_res.extOverride); }
    if (g_res.excludeNames[0]) { args.push_back("-e"); args.push_back(g_res.excludeNames); }
    if (g_res.outputPath[0]) { args.push_back("-o"); args.push_back(g_res.outputPath); }

    AppendDirAndPath(args, g_res.dirMode, g_res.inputPath);
    return args;
}

// ============================================================================
// Run Button Dispatch & Main Loop
// ============================================================================

static ProcHandle g_activeProc;
static double g_progressPhase = 0.0;
static std::string g_statusText = "Idle";
static int g_activeTab = 0;
static const char* g_activeSubcommand = ""; // the shown File Processing sub-tab's tool ("" for DB)
constexpr int kDbSubTab = 0; // File Processing's DB sub-tab (see the tabs in RunGui)
constexpr int kMpSubTab = 2; // its MP sub-tab
// The "?" of the main tab bar: every key and mouse control of the tab shown.
static void KeysHelp(const Library& lib, int tab) { // tab: 0 File Processing, 1 3D Viewer, 2 Map Editor (main()'s order)
    static const char* const kButtons[] = {"left", "right", "middle", "side 1", "side 2"};
    auto row = [](const std::string& keys, const char* what) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(keys.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(i18n::Tr(what));
    };
    auto mouse = [&](int b) { return std::string(i18n::Tr(kButtons[std::min(std::max(b, 0), 4)])) + i18n::Tr(" button drag"); };
    if (!ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
    if (tab == 2) {
        for (int k = 0; k < config::kMapKeyCount; ++k) row(ui::BindName(lib.mapKeys[k]), config::MapKeyLabel(k));
        row(mouse(lib.mapMouseOrbit), "Turn the camera");
        row(mouse(lib.mapMousePan), "Move the camera");
        row(i18n::Tr("Wheel"), "Zoom");
        row(i18n::Tr("Click / Shift+click"), "Select / add to or remove from the selection");
        row(i18n::Tr("Drag (Shift: add)"), "Select with a box");
        row("X / Y / Z", "While moving, turning or scaling: only along that axis (Shift: the two others)");
        row(i18n::Tr("Ctrl (held)"), "While moving, turning or scaling: no rounding");
        row(i18n::Tr("Enter / left click"), "Apply the move, turn or scale");
        row(i18n::Tr("Escape / right click"), "Cancel it");
        row("1 ... 9", "Tile painting: the quick tiles");
        row(", / .", "Tile painting: turn the brush");
        row(i18n::Tr("Alt+click"), "Tile painting: pick the tile under the mouse; script areas shown: pick an area");
        row(i18n::Tr("Ctrl+click"), "Logic mode: add a patrol point to the selected unit; a selected trap: a cast point");
        row(i18n::Tr("Ctrl+Shift+click"), "A selected trap: an activation area");
    } else if (tab == 1) {
        row(mouse(lib.mapMouseOrbit), "Turn the camera");
        row(mouse(lib.mapMousePan), "Move the camera");
        row(i18n::Tr("Wheel"), "Zoom");
        row(i18n::Tr("Up / Down"), "The previous / next row of the list");
    } else if (tab == 0) {
        row("Ctrl+S", "Save (DB editor, text editor)");
        row(i18n::Tr("Escape"), "Close a message");
    } else {
        row("-", "No keys in this tab");
    }
    ImGui::EndTable();
    if (tab == 2 || tab == 1) ImGui::TextDisabled("%s", i18n::Tr("The keys and mouse buttons are set in Settings."));
}

static Library* g_library = nullptr; // for the sub-tabs that need the sources (Texts)
static void DrawTextsTab() { textedit::DrawTab(*g_library); }
static void DrawMpTab() { mpedit::DrawTab(*g_library); }
static int g_requestMainTab = -1, g_requestSubTab = -1; // asked by an alert's button: shown at the next frame
static int g_jobTab = 0;                                // the File Processing sub-tab the running job came from
static std::string g_jobOutput;                         // the running job's output (counted for the alerts)

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
        g_jobOutput += chunk;
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
        // Alerts: the tools mark problems with [ERROR] / [WARN] lines.
        int errors = 0, warnings = 0;
        std::string firstError, firstWarning;
        std::istringstream lines(g_jobOutput);
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("[ERROR]", 0) == 0 || line.rfind("Error:", 0) == 0) { if (!errors++) firstError = line; }
            else if (line.rfind("[WARN", 0) == 0) { if (!warnings++) firstWarning = line; }
        }
        g_jobOutput.clear();
        if (errors || warnings || exitCode != 0) {
            const bool error = errors > 0 || exitCode != 0;
            std::string text = errors || warnings ? "The job finished" + (exitCode ? " with exit code " + std::to_string(exitCode) : std::string()) +
                                                        ": " + std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s)."
                                                  : "The job failed (exit code " + std::to_string(exitCode) + ").";
            if (!(error ? firstError : firstWarning).empty()) text += "\n" + (error ? firstError : firstWarning);
            const int tab = g_jobTab;
            alerts::Raise(error ? alerts::Level::Error : alerts::Level::Warning, text, "File Processing log",
                          [tab] { g_requestMainTab = 0; g_requestSubTab = tab; });
        }
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
    const std::string tool = g_activeSubcommand;
    if (tool == "ddsmmp") { activeInput = g_dds.inputPath; tokens = BuildDdsMmpArgs(); subcommand = "ddsmmp"; }
    else if (tool == "inireg") { activeInput = g_ini.inputPath; tokens = BuildIniRegArgs(); subcommand = "inireg"; }
    else if (tool == "mobdump") { activeInput = g_mob.inputPath; tokens = BuildMobDumpArgs(); subcommand = "mobdump"; }
    else if (tool == "restool") { activeInput = g_res.inputPath; tokens = BuildResToolArgs(); subcommand = "restool"; }
    else return;

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
    g_jobTab = g_activeTab;
    g_jobOutput.clear();
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
    // The first Latin/Cyrillic system font is the MAIN font, for every character: merged behind the built-in pixel
    // font, accented / Polish / Cyrillic letters came out in another size and style than the ASCII ones around them.
    bool haveLatin = false;
    for (const char* path : candidates) {
        std::error_code ec;
        if (haveLatin || !fs::is_regular_file(path, ec) || std::strstr(path, "algun") || std::strstr(path, "anum")) continue;
        haveLatin = io.Fonts->AddFontFromFileTTF(path, 15.0f) != nullptr;
    }
    if (!haveLatin) io.Fonts->AddFontDefault(); // no system font: the built-in one (ASCII only)
    for (const char* path : candidates) { // Korean merged behind it
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || !(std::strstr(path, "algun") || std::strstr(path, "anum"))) continue;
        ImFontConfig config;
        config.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path, 0.0f, &config); // size 0: the main font's (1.92 refuses another when merging)
    }
}

// GLFW 3.4 added the platform query and the Wayland app id; the Windows XP builds use GLFW 3.3 (the last one
// that runs on XP), where neither exists (and there is no Wayland).
#if GLFW_VERSION_MAJOR * 100 + GLFW_VERSION_MINOR >= 304
static bool OnWayland() { return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND; }
static void HintAppId(const char* id) { glfwWindowHintString(GLFW_WAYLAND_APP_ID, id); }
#else
static bool OnWayland() { return false; }
static void HintAppId(const char*) {}
#endif

// Why the window could not open: in the console, and on Windows also in a message box (a double-clicked
// program's console closes at once, so it would otherwise fail without a word).
static std::string g_glfwError;
static void ReportStartFailure(const char* what) {
    const std::string text = std::string(what) + (g_glfwError.empty() ? "" : ":\n" + g_glfwError);
    std::fprintf(stderr, "%s\n", text.c_str());
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), "um-multitool", MB_OK | MB_ICONERROR);
#endif
}

int RunGui(const GuiOptions& options) {
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
        g_glfwError = description ? description : "";
    });
    if (!glfwInit()) { ReportStartFailure("The window system could not start (GLFW)"); return 1; }

    glfwWindowHint(GLFW_DEPTH_BITS, 24); // the 3D Viewer needs a depth buffer
    // The name desktops match against um-multitool.desktop (StartupWMClass / the Wayland app id): that is
    // where a Wayland desktop takes the window's icon from.
    HintAppId("um-multitool");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "um-multitool");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "um-multitool");
    const std::string title = std::string(PROGRAM_NAME_SHOWN) + " " + PROGRAM_VERSION;
    // The window as it was last closed (um-multitool.cfg): size, maximized, and position where the
    // platform allows it (Wayland does not let a program place its window).
    const config::Config saved = config::Load();
    const bool wayland = OnWayland();
    // Created at the normal size and maximized once shown: a window that starts maximized gives the
    // window manager no size to go back to when it is un-maximized (KWin then keeps the full screen).
    GLFWwindow* window = glfwCreateWindow(std::max(saved.windowW, 640), std::max(saved.windowH, 400), title.c_str(), nullptr, nullptr);
    if (!window) {
        ReportStartFailure("The window could not be created (OpenGL)");
        glfwTerminate();
        return 1;
    }
    if (!wayland && saved.windowX != -100000) glfwSetWindowPos(window, saved.windowX, saved.windowY);
    glfwMakeContextCurrent(window);
    // The window icon (the battle axe, icon_data.hpp). Wayland has no way to set one from the program.
    if (!OnWayland()) {
        GLFWimage icons[3] = {{64, 64, const_cast<unsigned char*>(logo::kIcon64)},
                              {48, 48, const_cast<unsigned char*>(logo::kIcon48)},
                              {32, 32, const_cast<unsigned char*>(logo::kIcon32)}};
        glfwSetWindowIcon(window, 3, icons);
    }
    glfwSwapInterval(1); // vsync; also paces our polling loop like the old 60ms timer did

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini: ImGui would write it into the current directory (wherever um-multitool is started
    // from); the windows are fixed and the settings live in um-multitool.cfg beside the executable.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Number fields: a click (without dragging) on a drag field types the value; sliders: a double-click
    // (vendor/imgui patched, SliderScalar) or Ctrl+click.
    io.ConfigDragClickToInputText = true;
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
    g_library = &library;
    {
        i18n::Lang lang = i18n::Lang::English;
        i18n::FromCode(library.language, lang); // empty / unknown: English, and the first-start popup asks
        i18n::Set(lang);
    }
    bool askLanguage = library.language.empty();
    ui::SourcesState sourcesState;
    std::snprintf(sourcesState.databasePath, sizeof(sourcesState.databasePath), "%s", library.dbPath.c_str());

    viewer::Context* viewerCtx = viewer::Create(library);
    mapedit::Context* mapCtx = mapedit::Create(library);
    // The connector finds and opens the game's map through the Map Editor (switching to its tab).
    int requestedFromDll = -1;
    dllconnect::Hooks dllHooks;
    dllHooks.resolveMap = [mapCtx](const std::string& t, const std::string& b, const std::string& q, const std::vector<std::string>& gp,
                                   std::string& tp, std::vector<std::string>& mp, std::string& missing) {
        return mapedit::ResolveGameMap(mapCtx, t, b, q, gp, tp, mp, missing);
    };
    dllHooks.openInMapEditor = [mapCtx, &requestedFromDll](const std::string& t, const std::string& b, const std::string& q,
                                                          const std::vector<std::string>& gp) {
        requestedFromDll = 2; // kMap
        return mapedit::OpenGameMap(mapCtx, t, b, q, gp);
    };
    dllconnect::Context* dllCtx = dllconnect::Create(library, dllHooks);
    alerts::SetSound(library.sfxEnabled);
    alerts::SetPopups(library.alertPopups);
    alerts::SetVolume(library.sfxVolume);
    dbedit::SetHooks({[](bool save, const char* filterName, const char* filterExt, std::string& path) {
                          return NativePickFile(save, filterName, filterExt, path);
                      },
                      [] { g_requestMainTab = 0; g_requestSubTab = kDbSubTab; }, // File Processing > DB
                      [&library] { return library.dbPath; },
                      [&library] { return library.dbAutoLoad; },
                      [&library](bool on) { library.dbAutoLoad = on; library.SaveConfig(); },
                      [&library](const std::string& db) {
                          auto it = library.dbCompileTo.find(db);
                          return it == library.dbCompileTo.end() ? std::string() : it->second;
                      },
                      [&library](const std::string& db, const std::string& res) {
                          if (library.dbCompileTo[db] != res) { library.dbCompileTo[db] = res; library.SaveConfig(); }
                      }});
    if (!options.dbFile.empty()) dbedit::OpenFile(options.dbFile);
    // Saved in the config as numbers (GUI_TAB, BACKGROUND_*): new tabs are added at the end, whatever their place.
    enum { kFiles, kViewer, kMap, kSettings, kDll, kTex, kNone };
    // The tab asked for on the command line, else the one open when the GUI was last closed.
    int requestedTab = options.openSettings ? kSettings : options.openMap ? kMap : options.openViewer ? kViewer
                     : (library.guiTab >= kFiles && library.guiTab <= kTex ? library.guiTab : kNone);
    if (!options.viewerCategory.empty()) {
        std::string err;
        if (!viewer::OpenItem(viewerCtx, options.viewerCategory, options.viewerItem, err, options.viewerSkin, options.viewerNaked, options.viewerClip, options.viewerFrame)) std::fprintf(stderr, "%s\n", err.c_str());
        requestedTab = kViewer;
    }
    if (!options.mapFiles.empty()) mapedit::OpenFiles(mapCtx, options.mapFiles, options.mapFocus);
    if (!options.dbFile.empty()) {
        requestedTab = kFiles;
        g_requestSubTab = kDbSubTab; // DB
    }
    if (!options.mpFolder.empty()) {
        mpedit::OpenFolder(*g_library, options.mpFolder);
        requestedTab = kFiles;
        g_requestSubTab = kMpSubTab;
    }
    bool seeThrough = false;       // a 3D tab was shown last frame (its viewport must see through the window)
    int frameCount = 0;
    double lastTime = glfwGetTime();

    // The DB tab first: kDbSubTab.
    const TabInfo tabs[] = {
        {"DB", dbedit::DrawTab, nullptr, nullptr, nullptr},  // in-process: no Run button nor log
        {"Texts", DrawTextsTab, nullptr, nullptr, nullptr},  // the same
        {"MP / Saves (WIP)", DrawMpTab, nullptr, nullptr, nullptr},        // multiplayer characters (in-process)
        {"RES / MQ", DrawResToolTab, BuildResToolArgs, "restool", nullptr},
        {"INI <-> REG", DrawIniRegTab, BuildIniRegArgs, "inireg", nullptr},
        {"DDS <-> MMP", DrawDdsMmpTab, BuildDdsMmpArgs, "ddsmmp", nullptr},
        {"MOB Dump", DrawMobDumpTab, BuildMobDumpArgs, "mobdump", nullptr},
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
        dbedit::Update();
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
        const int bgTab = library.guiTab >= kFiles && library.guiTab <= kTex ? library.guiTab : kFiles;
        const std::string& bgPath = !library.tabBackground[bgTab].empty() ? library.tabBackground[bgTab] : library.background;
        const GLuint bgTexture = BackgroundTexture(bgPath, bgSize);
        ui::BackgroundShown() = bgTexture != 0 && library.backgroundOpacity > 0.0f;
        if (seeThrough || ui::BackgroundShown()) ImGui::SetNextWindowBgAlpha(0.0f); // drawn underneath ImGui
        ImGui::Begin("##main", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        bool shown3d = false;
        int shownTab = kNone;
        { // the "?" in the top right corner: the keys of the tab shown (drawn first, the tabs never reach it)
            const ImVec2 back = ImGui::GetCursorPos();
            const float w = ImGui::GetFrameHeight();
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - w, back.y));
            if (ImGui::Button("?", ImVec2(w, 0))) ImGui::OpenPopup("##keyshelp");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("The keys and mouse controls of this tab"));
            if (ImGui::BeginPopup("##keyshelp")) {
                KeysHelp(library, library.guiTab);
                ImGui::EndPopup();
            }
            ImGui::SetCursorPos(back);
        }
        if (ImGui::BeginTabBar("##maintabs")) {
            if (requestedFromDll >= 0) { requestedTab = requestedFromDll; requestedFromDll = -1; }
            if (g_requestMainTab >= 0) { requestedTab = g_requestMainTab; g_requestMainTab = -1; }
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
            if (ImGui::BeginTabItem("Texture Editor", nullptr, flags(kTex))) {
                texedit::DrawTab(library);
                shownTab = kTex;
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
        // New map or script check problems (the checks run while the Map Editor is shown).
        {
            int newErrors = 0, newWarnings = 0, errors = 0, warnings = 0;
            if (mapedit::TakeNewProblems(mapCtx, newErrors, newWarnings, errors, warnings)) {
                const std::string text = "Map Editor checks: " + std::to_string(errors) + " error(s), " + std::to_string(warnings) +
                                         " warning(s) (" + std::to_string(newErrors) + " new error(s), " + std::to_string(newWarnings) +
                                         " new warning(s)).";
                alerts::Raise(newErrors ? alerts::Level::Error : alerts::Level::Warning, text, "Map Editor > Checks",
                              [mapCtx] { g_requestMainTab = 2; mapedit::ShowChecks(mapCtx); });
            }
        }
        alerts::Draw();
        // First start (no LANGUAGE in um-multitool.cfg): ask for the display language. English is the default.
        if (askLanguage) {
            ImGui::OpenPopup("Language / Язык##firstLanguage");
            askLanguage = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Language / Язык##firstLanguage", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("Choose the display language.");
            ImGui::TextUnformatted("Выберите язык интерфейса.");
            ImGui::TextDisabled("It can be changed later in Settings. / Можно изменить в настройках.");
            ImGui::Spacing();
            // And the problem sounds (off by default), asked once with the language.
            if (ImGui::Checkbox("Warning and error sounds / Звуки предупреждений и ошибок", &library.sfxEnabled))
                alerts::SetSound(library.sfxEnabled);
            ImGui::Spacing();
            for (i18n::Lang lang : {i18n::Lang::English, i18n::Lang::Russian}) {
                if (lang != i18n::Lang::English) ImGui::SameLine();
                if (ImGui::Button(i18n::NativeName(lang), ImVec2(140, 0))) {
                    i18n::Set(lang);
                    library.language = i18n::Code(lang);
                    library.SaveConfig();
                    ImGui::CloseCurrentPopup();
                }
                if (lang == i18n::Lang::English) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndPopup();
        }
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
            HintAppId("um-multitool");
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
    alerts::Shutdown();
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
        const int requested = g_requestSubTab;
        g_requestSubTab = -1;
        for (int i = 0; i < static_cast<int>(count); ++i) {
            if (ImGui::BeginTabItem(tabs[i].name, nullptr, requested == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                g_activeTab = i;
                g_activeSubcommand = tabs[i].subcommand ? tabs[i].subcommand : "";
                ImGui::Spacing();
                tabs[i].draw();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    if (!tabs[g_activeTab].buildArgs) return; // a tab working in-process (DB)

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
    if (ImGui::Button("Clear log", ImVec2(110, 32))) {
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
