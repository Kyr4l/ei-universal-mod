/**
 * ============================================================================
 * um-multitool-gui - FLTK front-end for um-multitool
 * ============================================================================
 *
 * A small cross-platform (Windows/Linux) GUI wrapping the four merged
 * Evil Islands modding CLI tools (ddsmmp, inireg, mobdump, restool) exposed
 * by the um-multitool binary built alongside this GUI. Depends on it at
 * runtime: this GUI is just a form builder that spawns it in a visible
 * console window, so detailed progress is shown natively by the CLI tool.
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

// Wraps a single argument in quotes for the target shell: cmd.exe on Windows,
// POSIX sh (via `sh -c`) on Linux. POSIX also needs `$` and backtick escaped,
// since those still expand inside double quotes; cmd.exe treats them literally.
static std::string QuoteArg(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
#ifdef _WIN32
        if (c == '"' || c == '\\') out.push_back('\\');
#else
        if (c == '"' || c == '\\' || c == '$' || c == '`') out.push_back('\\');
#endif
        out.push_back(c);
    }
    out += "\"";
    return out;
}

#ifndef _WIN32
// Returns true if `name` is a runnable command on this system.
static bool CommandExists(const std::string& name) {
    std::string cmd = "command -v " + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Picks the first available terminal emulator on the system.
static std::string FindTerminalEmulator() {
    static const char* candidates[] = {
        "x-terminal-emulator", "xterm", "gnome-terminal",
        "konsole", "xfce4-terminal", "alacritty", "kitty"
    };
    for (const char* c : candidates) {
        if (CommandExists(c)) return c;
    }
    return "";
}
#endif

// Opaque handle to the spawned console process, used to poll its lifetime
// so the progress bar can reflect whether the job is still running.
#ifdef _WIN32
using ProcHandle = HANDLE;
static constexpr ProcHandle kInvalidProc = nullptr;
#else
using ProcHandle = pid_t;
static constexpr ProcHandle kInvalidProc = -1;
#endif

static bool IsProcessRunning(ProcHandle h) {
#ifdef _WIN32
    if (h == kInvalidProc) return false;
    return WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
#else
    if (h == kInvalidProc) return false;
    int status = 0;
    pid_t r = waitpid(h, &status, WNOHANG);
    return r == 0;
#endif
}

static void CleanupProcess(ProcHandle h) {
#ifdef _WIN32
    if (h != kInvalidProc) CloseHandle(h);
#else
    (void)h; // Already reaped by the waitpid() call that detected completion.
#endif
}

// Launches `binary subcommand tokens...` in a new, visible console window so
// the user can watch the real CLI tool's detailed progress output directly.
// Returns a process handle for lifetime polling, or kInvalidProc on failure.
static ProcHandle LaunchInConsole(const std::string& binary, const std::string& subcommand,
                                  const std::vector<std::string>& tokens, std::string& err) {
    std::ostringstream inner;
    inner << QuoteArg(binary) << ' ' << subcommand;
    for (const auto& t : tokens) {
        inner << ' ' << QuoteArg(t);
    }
    AppendLog("$ " + inner.str() + "\n");

#ifdef _WIN32
    std::string cmdLine = "cmd /k " + inner.str();
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                              CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!ok) {
        err = "Failed to launch console window.";
        return kInvalidProc;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
#else
    std::string term = FindTerminalEmulator();
    if (term.empty()) {
        err = "No terminal emulator found (tried xterm, gnome-terminal, konsole, ...).";
        return kInvalidProc;
    }
    std::string innerWithPause = inner.str() +
        "; echo; echo [Exit code: $?]; printf 'Press Enter to close...'; read _";

    pid_t pid = fork();
    if (pid < 0) {
        err = "Failed to fork a new process.";
        return kInvalidProc;
    }
    if (pid == 0) {
        execlp(term.c_str(), term.c_str(), "-e", "sh", "-c", innerWithPause.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
#endif
}

// ============================================================================
// Shared Widget Helpers
// ============================================================================

// Adds a labeled text field with "File" and "Folder" browse buttons feeding
// into it. When saveDialog is true, the "File" button lets the user type a
// new (not-yet-existing) destination filename instead of requiring one to pick.
static Fl_Input* AddBrowsableRow(int y, const char* label, bool saveDialog) {
    Fl_Box* box = new Fl_Box(10, y, LABEL_W, ROW_H, label);
    box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);

    Fl_Input* input = new Fl_Input(FIELD_X, y, FIELD_W, ROW_H);

    // Bundles the target Fl_Input with the dialog kind for the "File" callback;
    // intentionally leaked, same lifetime as the widgets themselves.
    struct FileBrowseCtx { Fl_Input* input; bool saveDialog; };
    auto* ctx = new FileBrowseCtx{input, saveDialog};

    Fl_Button* fileBtn = new Fl_Button(FIELD_X + FIELD_W + 5, y, BROWSE_W, ROW_H, "File");
    fileBtn->callback([](Fl_Widget*, void* data) {
        auto* ctx = static_cast<FileBrowseCtx*>(data);
        Fl_Native_File_Chooser chooser;
        chooser.type(ctx->saveDialog ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                                      : Fl_Native_File_Chooser::BROWSE_FILE);
        if (chooser.show() == 0 && chooser.filename()) {
            ctx->input->value(chooser.filename());
        }
    }, ctx);

    Fl_Button* dirBtn = new Fl_Button(FIELD_X + FIELD_W + BROWSE_W + 10, y, BROWSE_W, ROW_H, "Folder");
    dirBtn->callback([](Fl_Widget*, void* data) {
        Fl_Input* target = static_cast<Fl_Input*>(data);
        Fl_Native_File_Chooser chooser;
        chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        if (chooser.show() == 0 && chooser.filename()) {
            target->value(chooser.filename());
        }
    }, input);

    return input;
}

// Appends "-d" immediately followed by inputPath if dirMode is set (matching
// the CLI parsers' lookahead), otherwise appends inputPath as a bare positional.
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
    Fl_Check_Button* dirMode = nullptr;
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

    int row = y + 15;
    g_dds.inputPath = AddBrowsableRow(row, "Input:", false);
    row += ROW_H + ROW_GAP;
    g_dds.outputPath = AddBrowsableRow(row, "Output:", true);
    row += ROW_H + ROW_GAP;

    g_dds.dirMode = new Fl_Check_Button(10, row, 220, ROW_H, "Directory mode (-d)");
    row += ROW_H + ROW_GAP;
    g_dds.multiThread = new Fl_Check_Button(10, row, 220, ROW_H, "Multi-threaded (-m)");
    row += ROW_H + ROW_GAP;
    g_dds.dryRun = new Fl_Check_Button(10, row, 220, ROW_H, "Dry run (--dry-run)");
    row += ROW_H + ROW_GAP + 5;

    Fl_Box* modeLabel = new Fl_Box(10, row, 200, ROW_H, "Conversion direction:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    row += ROW_H;
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

    AppendDirAndPath(args, g_dds.dirMode->value(), g_dds.inputPath->value());
    return args;
}

// ============================================================================
// INI <-> REG Tab
// ============================================================================

struct IniRegTab {
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* dirMode = nullptr;
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

    int row = y + 15;
    g_ini.inputPath = AddBrowsableRow(row, "Input:", false);
    row += ROW_H + ROW_GAP;
    g_ini.outputPath = AddBrowsableRow(row, "Output:", true);
    row += ROW_H + ROW_GAP;

    g_ini.dirMode = new Fl_Check_Button(10, row, 220, ROW_H, "Directory mode (-d)");
    row += ROW_H + ROW_GAP;
    g_ini.multiThread = new Fl_Check_Button(10, row, 220, ROW_H, "Multi-threaded (-m)");
    row += ROW_H + ROW_GAP;
    g_ini.dryRun = new Fl_Check_Button(10, row, 220, ROW_H, "Dry run (--dry-run)");
    row += ROW_H + ROW_GAP + 5;

    Fl_Box* modeLabel = new Fl_Box(10, row, 200, ROW_H, "Conversion direction:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    row += ROW_H;
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

    AppendDirAndPath(args, g_ini.dirMode->value(), g_ini.inputPath->value());
    return args;
}

// ============================================================================
// MOB Dump Tab
// ============================================================================

struct MobDumpTab {
    Fl_Input* inputPath = nullptr;
    Fl_Input* outputPath = nullptr;
    Fl_Check_Button* dirMode = nullptr;
    Fl_Check_Button* multiThread = nullptr;
    Fl_Check_Button* dryRun = nullptr;
};

static MobDumpTab g_mob;

static Fl_Group* BuildMobDumpTab(int x, int y, int w, int h) {
    Fl_Group* grp = new Fl_Group(x, y, w, h, "MOB Dump");
    grp->user_data(reinterpret_cast<void*>(2));

    int row = y + 15;
    g_mob.inputPath = AddBrowsableRow(row, "Input:", false);
    row += ROW_H + ROW_GAP;
    g_mob.outputPath = AddBrowsableRow(row, "Output:", false);
    row += ROW_H + ROW_GAP;

    g_mob.dirMode = new Fl_Check_Button(10, row, 220, ROW_H, "Directory mode (-d)");
    row += ROW_H + ROW_GAP;
    g_mob.multiThread = new Fl_Check_Button(10, row, 220, ROW_H, "Multi-threaded (-m)");
    row += ROW_H + ROW_GAP;
    g_mob.dryRun = new Fl_Check_Button(10, row, 220, ROW_H, "Dry run (--dry-run)");

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

    AppendDirAndPath(args, g_mob.dirMode->value(), g_mob.inputPath->value());
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

    int row = y + 15;
    g_res.inputPath = AddBrowsableRow(row, "Input:", false);
    row += ROW_H + ROW_GAP;
    g_res.outputPath = AddBrowsableRow(row, "Output:", true);
    row += ROW_H + ROW_GAP;

    Fl_Box* extLabel = new Fl_Box(10, row, LABEL_W, ROW_H, "Ext override:");
    extLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    g_res.extOverride = new Fl_Input(FIELD_X, row, 150, ROW_H);
    g_res.extOverride->tooltip("Optional. e.g. .mq, .res (--ext)");
    row += ROW_H + ROW_GAP;

    Fl_Box* exLabel = new Fl_Box(10, row, LABEL_W, ROW_H, "Exclude:");
    exLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    g_res.excludeNames = new Fl_Input(FIELD_X, row, 300, ROW_H);
    g_res.excludeNames->tooltip("Comma-separated file names to omit when packing (-e / --exclude)");
    row += ROW_H + ROW_GAP;

    g_res.dirMode = new Fl_Check_Button(10, row, 220, ROW_H, "Directory mode (-d)");
    g_res.multiThread = new Fl_Check_Button(240, row, 220, ROW_H, "Multi-threaded (-m)");
    row += ROW_H + ROW_GAP;
    g_res.dryRun = new Fl_Check_Button(10, row, 220, ROW_H, "Dry run (--dry-run)");
    g_res.stripExt = new Fl_Check_Button(240, row, 260, ROW_H, "Strip _res/_mq suffix (-s)");
    g_res.stripExt->value(1);
    row += ROW_H + ROW_GAP + 5;

    Fl_Box* modeLabel = new Fl_Box(10, row, 200, ROW_H, "Action:");
    modeLabel->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    row += ROW_H;
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
static ProcHandle g_activeProc = kInvalidProc;
static double g_progressPhase = 0.0;

// Polls the spawned console process; while it's alive, advances an
// indeterminate progress fill, otherwise resets the UI to idle.
static void ProgressTimerCb(void*) {
    if (!IsProcessRunning(g_activeProc)) {
        CleanupProcess(g_activeProc);
        g_activeProc = kInvalidProc;
        if (g_progress) {
            g_progress->value(0.0f);
            g_progress->label("Idle");
        }
        if (g_runButton) {
            g_runButton->activate();
            g_runButton->label("Run");
        }
        if (g_statusBox) {
            g_statusBox->label("Job finished (see console window for details)");
        }
        return;
    }

    g_progressPhase += 3.0;
    if (g_progressPhase > 100.0) g_progressPhase = 0.0;
    if (g_progress) {
        g_progress->value(static_cast<float>(g_progressPhase));
        g_progress->label("Running... (see console window)");
    }
    Fl::repeat_timeout(0.06, ProgressTimerCb, nullptr);
}

static void OnRunClicked(Fl_Widget*, void*) {
    if (g_activeProc != kInvalidProc) {
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
    std::string err;
    ProcHandle proc = LaunchInConsole(binary, subcommand, tokens, err);
    if (proc == kInvalidProc) {
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
    if (g_statusBox) g_statusBox->label("Launched in console window");
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

