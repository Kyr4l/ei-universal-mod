/**
 * ============================================================================
 * um-multitool-gui - FLTK front-end for um-multitool
 * ============================================================================
 *
 * A small cross-platform (Windows/Linux) GUI wrapping the four merged
 * Evil Islands modding CLI tools (ddsmmp, inireg, mobdump, restool) exposed
 * by the um-multitool binary built alongside this GUI. Depends on it at
 * runtime: this GUI spawns it as a hidden subprocess and streams its
 * stdout/stderr into the log panel below, with no separate console window.
 *
 * Each subtool gets its own tab exposing every CLI flag it supports.
 * ============================================================================
 */

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Tooltip.H>

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
#else
#include <unistd.h>
#include <climits>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// Layout Constants
// ============================================================================

static constexpr int WIN_W = 780;
static constexpr int WIN_H = 700;
static constexpr int TABS_Y = 30;
static constexpr int TABS_H = 380;
static constexpr int ROW_H = 26;
static constexpr int ROW_GAP = 8;
static constexpr int LABEL_W = 95;
static constexpr int FIELD_X = 10 + LABEL_W;
static constexpr int BROWSE_W = 60;
static constexpr int FIELD_W = 780 - FIELD_X - 2 * BROWSE_W - 30;

// Accent color for primary actions and progress fill (steel blue).
static const Fl_Color kAccentColor = fl_rgb_color(41, 98, 163);

// ============================================================================
// Subprocess Launch Helpers
// ============================================================================

static Fl_Text_Buffer* g_logBuffer = nullptr;
static Fl_Box* g_statusBox = nullptr;

static void AppendLog(const std::string& text) {
    if (g_logBuffer) {
        g_logBuffer->append(text.c_str());
    }
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
// its end of the pipe (process exited), appending each chunk for the UI
// timer to drain. Never touches FLTK widgets directly (wrong thread).
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
// starts a background thread that streams the captured output for the GUI's
// log panel to display. Returns an invalid handle (per IsValid) on failure.
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

// Adds a labeled text field with "File" and "Folder" browse buttons feeding
// into it. When saveDialog is true, the "File" button lets the user type a
// new (not-yet-existing) destination filename instead of requiring one to pick.
// If multiThreadCb is given, it's switched on when a folder is chosen (or a
// typed/pasted path resolves to one) and off for a single file, since batch
// jobs benefit from -m while single-file conversions don't.
static Fl_Input* AddBrowsableRow(int y, const char* label, bool saveDialog,
                                 Fl_Check_Button* multiThreadCb = nullptr) {
    Fl_Box* box = new Fl_Box(10, y, LABEL_W, ROW_H, label);
    box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);

    Fl_Input* input = new Fl_Input(FIELD_X, y, FIELD_W, ROW_H);

    // Bundles the target Fl_Input with the dialog kind and the tab's
    // multi-thread checkbox; intentionally leaked, same lifetime as the widgets.
    struct BrowseCtx { Fl_Input* input; bool saveDialog; Fl_Check_Button* multiThreadCb; };
    auto* ctx = new BrowseCtx{input, saveDialog, multiThreadCb};

    Fl_Button* fileBtn = new Fl_Button(FIELD_X + FIELD_W + 5, y, BROWSE_W, ROW_H, "File");
    fileBtn->callback([](Fl_Widget*, void* data) {
        auto* ctx = static_cast<BrowseCtx*>(data);
        Fl_Native_File_Chooser chooser;
        chooser.type(ctx->saveDialog ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                                      : Fl_Native_File_Chooser::BROWSE_FILE);
        if (chooser.show() == 0 && chooser.filename()) {
            ctx->input->value(chooser.filename());
            if (ctx->multiThreadCb) ctx->multiThreadCb->value(0);
        }
    }, ctx);

    Fl_Button* dirBtn = new Fl_Button(FIELD_X + FIELD_W + BROWSE_W + 10, y, BROWSE_W, ROW_H, "Folder");
    dirBtn->callback([](Fl_Widget*, void* data) {
        auto* ctx = static_cast<BrowseCtx*>(data);
        Fl_Native_File_Chooser chooser;
        chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        if (chooser.show() == 0 && chooser.filename()) {
            ctx->input->value(chooser.filename());
            if (ctx->multiThreadCb) ctx->multiThreadCb->value(1);
        }
    }, ctx);

    if (multiThreadCb) {
        input->callback([](Fl_Widget* w, void* data) {
            auto* ctx = static_cast<BrowseCtx*>(data);
            std::string val = static_cast<Fl_Input*>(w)->value();
            if (val.empty()) return;
            std::error_code ec;
            ctx->multiThreadCb->value(fs::is_directory(val, ec) ? 1 : 0);
        }, ctx);
    }

    return input;
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
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* multiThread = nullptr;
    Fl_Check_Button* dryRun = nullptr;
    Fl_Round_Button* modeAuto = nullptr;
    Fl_Round_Button* modeDdsToMmp = nullptr;
    Fl_Round_Button* modeMmpToDds = nullptr;
};

static DdsMmpTab g_dds;

static Fl_Group* BuildDdsMmpTab(int x, int y, int w, int h) {
    Fl_Group* grp = new Fl_Group(x, y, w, h, "DDS <-> MMP");
    grp->user_data(reinterpret_cast<void*>(0));

    const int inputY   = y + 15;
    const int outputY  = inputY + ROW_H + ROW_GAP;
    const int multiY   = outputY + ROW_H + ROW_GAP;
    const int dryRunY  = multiY + ROW_H + ROW_GAP;
    const int modeLblY = dryRunY + ROW_H + ROW_GAP + 5;

    // Created before the input row so its Folder/File buttons can toggle it.
    g_dds.multiThread = new Fl_Check_Button(10, multiY, 220, ROW_H, "Multi-threaded (-m)");

    g_dds.inputPath = AddBrowsableRow(inputY, "Input:", false, g_dds.multiThread);
    g_dds.outputPath = AddBrowsableRow(outputY, "Output:", true);

    g_dds.dryRun = new Fl_Check_Button(10, dryRunY, 220, ROW_H, "Dry run (--dry-run)");

    Fl_Box* modeLabel = new Fl_Box(10, modeLblY, 200, ROW_H, "Conversion direction:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    int row = modeLblY + ROW_H;
    g_dds.modeAuto = new Fl_Round_Button(20, row, 200, ROW_H, "Auto-detect (default)");
    g_dds.modeAuto->type(FL_RADIO_BUTTON);
    g_dds.modeAuto->value(1);
    row += ROW_H;
    g_dds.modeDdsToMmp = new Fl_Round_Button(20, row, 200, ROW_H, "Force DDS -> MMP");
    g_dds.modeDdsToMmp->type(FL_RADIO_BUTTON);
    row += ROW_H;
    g_dds.modeMmpToDds = new Fl_Round_Button(20, row, 200, ROW_H, "Force MMP -> DDS");
    g_dds.modeMmpToDds->type(FL_RADIO_BUTTON);

    grp->end();
    return grp;
}

static std::vector<std::string> BuildDdsMmpArgs() {
    std::vector<std::string> args;

    if (g_dds.dryRun->value()) args.push_back("--dry-run");
    if (g_dds.multiThread->value()) args.push_back("-m");
    if (g_dds.modeDdsToMmp->value()) args.push_back("--dds2mmp");
    else if (g_dds.modeMmpToDds->value()) args.push_back("--mmp2dds");

    std::string out = g_dds.outputPath->value();
    if (!out.empty()) {
        args.push_back("-o");
        args.push_back(out);
    }

    std::string in = g_dds.inputPath->value();
    if (!in.empty()) args.push_back(in);
    return args;
}

// ============================================================================
// INI <-> REG Tab
// ============================================================================

struct IniRegTab {
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* multiThread = nullptr;
    Fl_Check_Button* dryRun = nullptr;
    Fl_Round_Button* modeAuto = nullptr;
    Fl_Round_Button* modeIniToReg = nullptr;
    Fl_Round_Button* modeRegToIni = nullptr;
};

static IniRegTab g_ini;

static Fl_Group* BuildIniRegTab(int x, int y, int w, int h) {
    Fl_Group* grp = new Fl_Group(x, y, w, h, "INI <-> REG");
    grp->user_data(reinterpret_cast<void*>(1));

    const int inputY   = y + 15;
    const int outputY  = inputY + ROW_H + ROW_GAP;
    const int multiY   = outputY + ROW_H + ROW_GAP;
    const int dryRunY  = multiY + ROW_H + ROW_GAP;
    const int modeLblY = dryRunY + ROW_H + ROW_GAP + 5;

    g_ini.multiThread = new Fl_Check_Button(10, multiY, 220, ROW_H, "Multi-threaded (-m)");

    g_ini.inputPath = AddBrowsableRow(inputY, "Input:", false, g_ini.multiThread);
    g_ini.outputPath = AddBrowsableRow(outputY, "Output:", true);

    g_ini.dryRun = new Fl_Check_Button(10, dryRunY, 220, ROW_H, "Dry run (--dry-run)");

    Fl_Box* modeLabel = new Fl_Box(10, modeLblY, 200, ROW_H, "Conversion direction:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    int row = modeLblY + ROW_H;
    g_ini.modeAuto = new Fl_Round_Button(20, row, 200, ROW_H, "Auto-detect (default)");
    g_ini.modeAuto->type(FL_RADIO_BUTTON);
    g_ini.modeAuto->value(1);
    row += ROW_H;
    g_ini.modeIniToReg = new Fl_Round_Button(20, row, 200, ROW_H, "Force INI -> REG");
    g_ini.modeIniToReg->type(FL_RADIO_BUTTON);
    row += ROW_H;
    g_ini.modeRegToIni = new Fl_Round_Button(20, row, 200, ROW_H, "Force REG -> INI");
    g_ini.modeRegToIni->type(FL_RADIO_BUTTON);

    grp->end();
    return grp;
}

static std::vector<std::string> BuildIniRegArgs() {
    std::vector<std::string> args;

    if (g_ini.dryRun->value()) args.push_back("--dry-run");
    if (g_ini.multiThread->value()) args.push_back("-m");
    if (g_ini.modeIniToReg->value()) args.push_back("--ini2reg");
    else if (g_ini.modeRegToIni->value()) args.push_back("--reg2ini");

    std::string out = g_ini.outputPath->value();
    if (!out.empty()) {
        args.push_back("-o");
        args.push_back(out);
    }

    std::string in = g_ini.inputPath->value();
    if (!in.empty()) args.push_back(in);
    return args;
}

// ============================================================================
// MOB Dump Tab
// ============================================================================

struct MobDumpTab {
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* multiThread = nullptr;
    Fl_Check_Button* dryRun = nullptr;
};

static MobDumpTab g_mob;

static Fl_Group* BuildMobDumpTab(int x, int y, int w, int h) {
    Fl_Group* grp = new Fl_Group(x, y, w, h, "MOB Dump");
    grp->user_data(reinterpret_cast<void*>(2));

    const int inputY  = y + 15;
    const int outputY = inputY + ROW_H + ROW_GAP;
    const int multiY  = outputY + ROW_H + ROW_GAP;
    const int dryRunY = multiY + ROW_H + ROW_GAP;

    g_mob.multiThread = new Fl_Check_Button(10, multiY, 220, ROW_H, "Multi-threaded (-m)");

    g_mob.inputPath = AddBrowsableRow(inputY, "Input:", false, g_mob.multiThread);
    g_mob.outputPath = AddBrowsableRow(outputY, "Output:", false);

    g_mob.dryRun = new Fl_Check_Button(10, dryRunY, 220, ROW_H, "Dry run (--dry-run)");

    grp->end();
    return grp;
}

static std::vector<std::string> BuildMobDumpArgs() {
    std::vector<std::string> args;

    if (g_mob.dryRun->value()) args.push_back("--dry-run");
    if (g_mob.multiThread->value()) args.push_back("-m");

    std::string out = g_mob.outputPath->value();
    if (!out.empty()) {
        args.push_back("-o");
        args.push_back(out);
    }

    std::string in = g_mob.inputPath->value();
    if (!in.empty()) args.push_back(in);
    return args;
}

// ============================================================================
// RES/MQ Archive Tab (restool)
// ============================================================================

struct ResToolTab {
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* dirMode = nullptr;
    Fl_Check_Button* multiThread = nullptr;
    Fl_Check_Button* dryRun = nullptr;
    Fl_Check_Button* stripExt = nullptr;
    Fl_Input* extOverride = nullptr;
    Fl_Input* excludeNames = nullptr;
    Fl_Round_Button* actionAuto = nullptr;
    Fl_Round_Button* actionPack = nullptr;
    Fl_Round_Button* actionUnpack = nullptr;
};

static ResToolTab g_res;

static Fl_Group* BuildResToolTab(int x, int y, int w, int h) {
    Fl_Group* grp = new Fl_Group(x, y, w, h, "RES / MQ");
    grp->user_data(reinterpret_cast<void*>(3));

    const int inputY     = y + 15;
    const int outputY    = inputY + ROW_H + ROW_GAP;
    const int extY       = outputY + ROW_H + ROW_GAP;
    const int excludeY   = extY + ROW_H + ROW_GAP;
    const int checkRow1Y = excludeY + ROW_H + ROW_GAP;
    const int checkRow2Y = checkRow1Y + ROW_H + ROW_GAP;
    const int actionLblY = checkRow2Y + ROW_H + ROW_GAP + 5;

    // Created before the input row so its Folder/File buttons can toggle it.
    // Unlike ddsmmp/inireg/mobdump, restool's -d changes meaning rather than
    // being redundant: it batch-processes every item found inside the given
    // folder as a separate target, instead of packing that one folder itself.
    g_res.multiThread = new Fl_Check_Button(240, checkRow1Y, 220, ROW_H, "Multi-threaded (-m)");
    g_res.dirMode = new Fl_Check_Button(10, checkRow1Y, 220, ROW_H, "Batch mode (-d)");
    g_res.dirMode->tooltip(
        "Treats the input folder as a container of MANY archives/folders to\n"
        "process separately. Not needed to pack a single folder into one\n"
        "archive - that already happens automatically.");

    g_res.inputPath = AddBrowsableRow(inputY, "Input:", false, g_res.multiThread);
    g_res.outputPath = AddBrowsableRow(outputY, "Output:", true);

    Fl_Box* extLabel = new Fl_Box(10, extY, LABEL_W, ROW_H, "Ext override:");
    extLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    g_res.extOverride = new Fl_Input(FIELD_X, extY, 150, ROW_H);
    g_res.extOverride->tooltip("Optional. e.g. .mq, .res (--ext)");

    Fl_Box* exLabel = new Fl_Box(10, excludeY, LABEL_W, ROW_H, "Exclude:");
    exLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    g_res.excludeNames = new Fl_Input(FIELD_X, excludeY, 300, ROW_H);
    g_res.excludeNames->tooltip("Comma-separated file names to omit when packing (-e / --exclude)");

    g_res.dryRun = new Fl_Check_Button(10, checkRow2Y, 220, ROW_H, "Dry run (--dry-run)");
    g_res.stripExt = new Fl_Check_Button(240, checkRow2Y, 260, ROW_H, "Strip _res/_mq suffix (-s)");
    g_res.stripExt->value(1);

    Fl_Box* modeLabel = new Fl_Box(10, actionLblY, 200, ROW_H, "Action:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    int row = actionLblY + ROW_H;
    g_res.actionAuto = new Fl_Round_Button(20, row, 200, ROW_H, "Auto-detect (default)");
    g_res.actionAuto->type(FL_RADIO_BUTTON);
    g_res.actionAuto->value(1);
    row += ROW_H;
    g_res.actionPack = new Fl_Round_Button(20, row, 200, ROW_H, "Force Pack (--pack)");
    g_res.actionPack->type(FL_RADIO_BUTTON);
    row += ROW_H;
    g_res.actionUnpack = new Fl_Round_Button(20, row, 200, ROW_H, "Force Unpack (--unpack)");
    g_res.actionUnpack->type(FL_RADIO_BUTTON);

    grp->end();
    return grp;
}

static std::vector<std::string> BuildResToolArgs() {
    std::vector<std::string> args;

    if (g_res.dryRun->value()) args.push_back("--dry-run");
    if (g_res.multiThread->value()) args.push_back("-m");
    if (!g_res.stripExt->value()) args.push_back("--no-strip-ext");
    if (g_res.actionPack->value()) args.push_back("--pack");
    else if (g_res.actionUnpack->value()) args.push_back("--unpack");

    std::string ext = g_res.extOverride->value();
    if (!ext.empty()) {
        args.push_back("--ext");
        args.push_back(ext);
    }

    std::string exclude = g_res.excludeNames->value();
    if (!exclude.empty()) {
        args.push_back("-e");
        args.push_back(exclude);
    }

    std::string out = g_res.outputPath->value();
    if (!out.empty()) {
        args.push_back("-o");
        args.push_back(out);
    }

    AppendDirAndPath(args, g_res.dirMode->value(), g_res.inputPath->value());
    return args;
}

// ============================================================================
// Run Button Dispatch
// ============================================================================

static Fl_Tabs* g_tabs = nullptr;
static Fl_Button* g_runButton = nullptr;
static Fl_Progress* g_progress = nullptr;
static ProcHandle g_activeProc;
static double g_progressPhase = 0.0;

// Drains captured subprocess output into the log panel and, once the process
// has exited and the reader thread has drained the last of the pipe, resets
// the UI to idle. While running, advances an indeterminate progress fill.
static void ProgressTimerCb(void*) {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        chunk.swap(g_pendingOutput);
    }
    if (!chunk.empty()) {
        AppendLog(chunk);
    }

    bool stillRunning = true;
    int exitCode = 0;
    PollProcess(g_activeProc, stillRunning, exitCode);

    if (!stillRunning && !g_readerAlive.load()) {
        CleanupProcess(g_activeProc);
        if (g_progress) {
            g_progress->value(0.0f);
            g_progress->label("Idle");
        }
        if (g_runButton) {
            g_runButton->activate();
            g_runButton->label("Run");
        }
        if (g_statusBox) {
            g_statusBox->label(exitCode == 0 ? "Job finished successfully" : "Job finished with errors");
        }
        AppendLog(exitCode == 0
            ? "\n[Job finished successfully]\n\n"
            : ("\n[Job finished, exit code " + std::to_string(exitCode) + "]\n\n"));
        return;
    }

    g_progressPhase += 3.0;
    if (g_progressPhase > 100.0) g_progressPhase = 0.0;
    if (g_progress) {
        g_progress->value(static_cast<float>(g_progressPhase));
        g_progress->label("Running...");
    }
    Fl::repeat_timeout(0.06, ProgressTimerCb, nullptr);
}

static void OnRunClicked(Fl_Widget*, void*) {
    if (IsValid(g_activeProc)) {
        return; // A job is already running.
    }

    Fl_Widget* activeTab = g_tabs->value();
    if (!activeTab) {
        return;
    }
    intptr_t subtool = reinterpret_cast<intptr_t>(activeTab->user_data());

    // Checked directly against the widget rather than re-parsed from the built
    // token list, since values of -o/--ext/-e would otherwise look positional too.
    const Fl_Input* activeInput = nullptr;
    std::vector<std::string> tokens;
    const char* subcommand = "";
    switch (subtool) {
        case 0: activeInput = g_dds.inputPath; tokens = BuildDdsMmpArgs(); subcommand = "ddsmmp"; break;
        case 1: activeInput = g_ini.inputPath; tokens = BuildIniRegArgs(); subcommand = "inireg"; break;
        case 2: activeInput = g_mob.inputPath; tokens = BuildMobDumpArgs(); subcommand = "mobdump"; break;
        case 3: activeInput = g_res.inputPath; tokens = BuildResToolArgs(); subcommand = "restool"; break;
        default: return;
    }

    if (!activeInput || std::string(activeInput->value()).empty()) {
        fl_alert("Please specify an input path.");
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
        if (g_statusBox) g_statusBox->label("Failed to launch");
        fl_alert("%s", err.c_str());
        return;
    }

    g_activeProc = proc;
    g_progressPhase = 0.0;
    if (g_runButton) {
        g_runButton->deactivate();
        g_runButton->label("Running...");
    }
    if (g_statusBox) g_statusBox->label("Running...");
    Fl::add_timeout(0.06, ProgressTimerCb, nullptr);
}

static void OnClearLogClicked(Fl_Widget*, void*) {
    if (g_logBuffer) {
        g_logBuffer->text("");
    }
}

// ============================================================================
// Application Entry Point
// ============================================================================

int main(int argc, char** argv) {
    Fl::scheme("gtk+");
    Fl::background(240, 240, 240);
    Fl_Tooltip::size(12);

    Fl_Window* window = new Fl_Window(WIN_W, WIN_H, "um-multitool GUI");
    window->color(fl_rgb_color(240, 240, 240));

    g_tabs = new Fl_Tabs(10, TABS_Y, WIN_W - 20, TABS_H);
    g_tabs->selection_color(kAccentColor);
    BuildDdsMmpTab(10, TABS_Y + 25, WIN_W - 20, TABS_H - 25);
    BuildIniRegTab(10, TABS_Y + 25, WIN_W - 20, TABS_H - 25);
    BuildMobDumpTab(10, TABS_Y + 25, WIN_W - 20, TABS_H - 25);
    BuildResToolTab(10, TABS_Y + 25, WIN_W - 20, TABS_H - 25);
    g_tabs->end();

    int controlsY = TABS_Y + TABS_H + 12;
    g_runButton = new Fl_Button(10, controlsY, 110, 32, "@> Run");
    g_runButton->callback(OnRunClicked);
    g_runButton->color(kAccentColor);
    g_runButton->labelcolor(FL_WHITE);
    g_runButton->labelfont(FL_HELVETICA_BOLD);

    Fl_Button* clearBtn = new Fl_Button(130, controlsY, 110, 32, "Clear Log");
    clearBtn->callback(OnClearLogClicked);

    g_statusBox = new Fl_Box(250, controlsY, WIN_W - 270, 32, "Idle");
    g_statusBox->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    g_statusBox->labelfont(FL_HELVETICA_ITALIC);

    int progressY = controlsY + 40;
    g_progress = new Fl_Progress(10, progressY, WIN_W - 20, 22, "Idle");
    g_progress->minimum(0.0f);
    g_progress->maximum(100.0f);
    g_progress->color(FL_WHITE);
    g_progress->selection_color(kAccentColor);
    g_progress->labelsize(12);

    int logY = progressY + 32;
    Fl_Text_Display* logDisplay = new Fl_Text_Display(10, logY, WIN_W - 20, WIN_H - logY - 10);
    g_logBuffer = new Fl_Text_Buffer();
    logDisplay->buffer(g_logBuffer);
    logDisplay->textfont(FL_COURIER);
    logDisplay->textsize(12);

    window->end();
    window->resizable(logDisplay);
    window->show(argc, argv);

    return Fl::run();
}

