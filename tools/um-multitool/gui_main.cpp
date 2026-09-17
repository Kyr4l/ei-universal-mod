/**
 * ============================================================================
 * um-multitool-gui - Dear ImGui front-end for um-multitool
 * ============================================================================
 *
 * A small cross-platform (Windows/Linux) GUI wrapping the five merged
 * Evil Islands modding CLI tools (ddsmmp, inireg, mobdump, restool, xlsxdb)
 * exposed by the um-multitool binary built alongside this GUI. Depends on it
 * at runtime: this GUI spawns it as a hidden subprocess and streams its
 * stdout/stderr into the log panel below, with no separate console window.
 *
 * Toolkit: Dear ImGui (vendor/imgui, vendored as source per its normal
 * distribution model) rendering through its OpenGL2 (legacy fixed-pipeline)
 * backend, windowed via GLFW. GLFW auto-selects X11 or Wayland on Linux from
 * the running session with no code on our end, and provides the Win32 window
 * on Windows - one codebase for all three targets. OpenGL2 (not OpenGL3) was
 * chosen specifically because it needs no GL function loader library and has
 * historically been the more robust legacy-GL path under Wine/older drivers.
 *
 * Each subtool gets its own tab exposing every CLI flag it supports.
 * ============================================================================
 */

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

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

// Directory containing the running GUI executable.
static std::string GetExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, len);
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, len);
#endif
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

// Locates the um-multitool binary built alongside this GUI in the same
// directory, falling back to a sibling tools/um-multitool/ layout or PATH.
static std::string FindMultitoolBinary() {
#ifdef _WIN32
    const char* exeName = "um-multitool.exe";
#else
    const char* exeName = "um-multitool";
#endif
    std::string exeDir = GetExeDir();
    std::error_code ec;

    fs::path sameDir = fs::path(exeDir) / exeName;
    if (fs::exists(sameDir, ec)) return fs::absolute(sameDir, ec).string();

    fs::path sibling = fs::path(exeDir) / ".." / "um-multitool" / exeName;
    if (fs::exists(sibling, ec)) return fs::absolute(sibling, ec).string();

    return exeName; // Fall back to PATH lookup.
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

int main(int, char**) {
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
    });
    if (!glfwInit()) return 1;

    GLFWwindow* window = glfwCreateWindow(800, 700, "um-multitool GUI", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync; also paces our polling loop like the old 60ms timer did

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Default Dear ImGui look and colors - no theme customization.

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    const TabInfo tabs[] = {
        {"DDS <-> MMP", DrawDdsMmpTab, BuildDdsMmpArgs, "ddsmmp", nullptr},
        {"INI <-> REG", DrawIniRegTab, BuildIniRegArgs, "inireg", nullptr},
        {"MOB Dump", DrawMobDumpTab, BuildMobDumpArgs, "mobdump", nullptr},
        {"RES / MQ", DrawResToolTab, BuildResToolArgs, "restool", nullptr},
        {"XLSX -> RES", DrawXlsxDbTab, BuildXlsxDbArgs, "xlsxdb", nullptr},
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(16);
            continue;
        }

        PumpActiveProcess();

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fbW) / io.DisplayFramebufferScale.x,
                                         static_cast<float>(fbH) / io.DisplayFramebufferScale.y));
        ImGui::Begin("##main", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (ImGui::BeginTabBar("##tabs")) {
            for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
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

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
