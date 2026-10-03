/**
 * ============================================================================
 * um-multitool - Evil Islands Modding Toolkit
 * ============================================================================
 *
 * Description:
 *   One binary: a GUI (File Processing, 3D Viewer, Map Editor and Settings tabs, gui_main.cpp) and
 *   the command-line tools. Double-clicked (no terminal) it opens the GUI;
 *   run from a terminal without arguments it prints the usage; `gui` opens the
 *   GUI from a terminal. The command-line tools merge five standalone tools:
 *     - ddsmmp  (formerly um-ddsmmp):  .dds  <-> .mmp  texture conversion
 *     - inireg  (formerly um-inireg):  .ini  <-> .reg  config conversion
 *     - mobdump (formerly um-mobdump): .mob  ->  .yaml/.eis map dumping
 *     - restool (formerly um-restool): .res/.mq <-> folder pack/unpack
 *     - xlsxdb  (formerly um-xlsxdb):  .xlsx ->  .res database compiler
 *   plus `viewer`, the 3D Viewer's command-line modes (viewer/viewer_app.cpp), and `map`, the
 *   Map Editor's map checks (mapedit/map_app.cpp).
 *
 * Dispatch rules:
 *   1. Explicit subcommand: `um-multitool <subcommand> [options] <path>`
 *   2. Auto-detect: `um-multitool <path> [options]` infers the subcommand
 *      from the input's file extension (single file) or its contents
 *      (directory, only when unambiguous). Ambiguous or unrecognized
 *      input requires an explicit subcommand.
 *
 * Version:
 *   see version.hpp
 * ============================================================================
 */

#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include "gui.hpp"
#include "version.hpp"
#include "subtools.hpp"
#include "viewer/viewer_app.hpp"
#include "mapedit/map_app.hpp"
#include "dllconnect/connector_app.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;



static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static void PrintTopLevelHelp() {
    std::cout << PROGRAM_NAME_SHOWN << " (um-multitool) " << PROGRAM_VERSION << " - Evil Islands Modding Toolkit\n\n"
              << "Usage:\n"
              << "  um-multitool gui                      # open the GUI (also what double-clicking does)\n"
              << "  um-multitool gui --db <file>          # ... on File Processing > DB with this database (.res, .xlsx, .ods)\n"
              << "  um-multitool gui --viewer units <unit> [--skin <file>] [--naked]  # ... on 3D Viewer > Units (a skin to try, no equipment)\n"
              << "  um-multitool gui --mp <folder>        # ... on File Processing > MP with this characters folder (<game>/mp)\n"
              << "  um-multitool <subcommand> [options] <path>\n"
              << "  um-multitool <path> [options]         # auto-detects the right subcommand\n\n"
              << "Subcommands:\n"
              << "  ddsmmp   (alias: dds)   Convert textures between .dds <-> .mmp\n"
              << "  inireg   (alias: ini)   Convert configs between .ini <-> .reg\n"
              << "  mobdump  (alias: mob)   Dump .mob map files to .yaml / .eis\n"
              << "  restool  (alias: res)   Pack/unpack .res / .mq archives\n"
              << "  xlsxdb   (alias: db)    Compile .xlsx / .ods gameplay databases to .res\n"
              << "  dbexport                Export .res gameplay databases to .xlsx / .ods (the reverse of xlsxdb)\n"
              << "  viewer                  3D Viewer from the command line: list items, render, export GIFs\n"
              << "  map                     Check .mob maps like the Map Editor (and um.dll) do\n"
              << "  dll                     Commands to um.dll inside the running game (its DLL server): memory, threads, breakpoints\n"
              << "  completion bash         Print the bash tab-completion script: eval \"$(um-multitool completion bash)\" in ~/.bashrc\n"
              << "  install-desktop         Add um-multitool to the Linux application menu, with its icon and bash tab completion (--remove: undo)\n\n"
              << "Options:\n"
              << "  --version       Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help      Print this help message\n\n"
              << "Run 'um-multitool <subcommand> --help' for subcommand-specific options.\n\n"
              << "Examples:\n"
              << "  um-multitool restool database.res\n"
              << "  um-multitool ddsmmp texture.dds\n"
              << "  um-multitool inireg -d ./ini -o ./reg -m\n"
              << "  um-multitool xlsxdb databaselmp.xlsx\n"
              << "  um-multitool dbexport databaselmp.res -o databaselmp.ods\n"
              << "  um-multitool texture.dds                 # auto-detected -> ddsmmp\n\n"
              << "Note: directory-mode auto-detection only succeeds when every file in the\n"
              << "directory belongs to exactly one of ddsmmp/inireg/mobdump; anything mixed,\n"
              << "unrecognized, or restool-shaped (archives / generic asset folders) requires\n"
              << "the explicit 'restool' subcommand. xlsxdb only ever operates on a single\n"
              << ".xlsx file, so it is also excluded from directory auto-detection.\n";
}

static void PrintTopLevelVersion() {
    std::cout << PROGRAM_NAME_SHOWN << " (um-multitool) version " << PROGRAM_VERSION << "\n"
              << "  bundles: ddsmmp, inireg, mobdump, restool, xlsxdb, dbexport (each 1.0), the GUI, the 3D Viewer and the Map Editor\n";
}

enum class SubTool { None, DdsMmp, IniReg, MobDump, ResTool, XlsxDb, DbExport };

static SubTool MatchSubcommand(const std::string& tok) {
    if (tok == "ddsmmp" || tok == "dds")  return SubTool::DdsMmp;
    if (tok == "inireg" || tok == "ini")  return SubTool::IniReg;
    if (tok == "mobdump" || tok == "mob") return SubTool::MobDump;
    if (tok == "restool" || tok == "res") return SubTool::ResTool;
    if (tok == "xlsxdb" || tok == "db")   return SubTool::XlsxDb;
    if (tok == "dbexport")                return SubTool::DbExport;
    return SubTool::None;
}

static int DispatchTo(SubTool tool, int argc, char* argv[]) {
    switch (tool) {
        case SubTool::DdsMmp:  return RunDdsMmp(argc, argv);
        case SubTool::IniReg:  return RunIniReg(argc, argv);
        case SubTool::MobDump: return RunMobDump(argc, argv);
        case SubTool::ResTool: return RunResTool(argc, argv);
        case SubTool::XlsxDb:  return RunXlsxDb(argc, argv);
        case SubTool::DbExport: return RunDbExport(argc, argv);
        default: return 1;
    }
}

// Finds the first positional (non-flag, non-flag-value) argument, if any.
static std::string FindPositionalArg(int argc, char* argv[]) {
    static const std::set<std::string> valueFlags = {
        "-o", "--output", "--ext", "-e", "--exclude"
    };
    bool expectingValue = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (expectingValue) {
            expectingValue = false;
            continue;
        }
        if (valueFlags.count(a)) {
            expectingValue = true;
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            continue;
        }
        return a;
    }
    return "";
}

struct ExtCounts {
    size_t ddsmmp = 0;
    size_t inireg = 0;
    size_t mobdump = 0;
    size_t resmq = 0;
    size_t other = 0;
};

static SubTool DetectFromExtension(const std::string& ext) {
    if (ext == ".dds" || ext == ".mmp") return SubTool::DdsMmp;
    if (ext == ".ini" || ext == ".reg") return SubTool::IniReg;
    if (ext == ".mob") return SubTool::MobDump;
    if (ext == ".res" || ext == ".mq") return SubTool::ResTool;
    if (ext == ".xlsx" || ext == ".ods") return SubTool::XlsxDb;
    return SubTool::None;
}

// Auto-detects the right subtool for a path. Only dispatches automatically
// when the input is unambiguous; otherwise returns SubTool::None with errOut set.
static SubTool AutoDetect(const fs::path& path, std::string& errOut) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        ExtCounts counts;
        for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = ToLower(entry.path().extension().string());
            if (ext == ".dds" || ext == ".mmp") counts.ddsmmp++;
            else if (ext == ".ini" || ext == ".reg") counts.inireg++;
            else if (ext == ".mob") counts.mobdump++;
            else if (ext == ".res" || ext == ".mq") counts.resmq++;
            else counts.other++;
        }

        size_t recognizedKinds = (counts.ddsmmp > 0) + (counts.inireg > 0) + (counts.mobdump > 0);
        if (recognizedKinds == 1 && counts.resmq == 0 && counts.other == 0) {
            if (counts.ddsmmp > 0)  return SubTool::DdsMmp;
            if (counts.inireg > 0)  return SubTool::IniReg;
            if (counts.mobdump > 0) return SubTool::MobDump;
        }

        errOut = "Cannot determine which tool to use for directory '" + path.string() + "'.\n"
                 "Found: " + std::to_string(counts.ddsmmp) + " .dds/.mmp, " +
                 std::to_string(counts.inireg) + " .ini/.reg, " +
                 std::to_string(counts.mobdump) + " .mob, " +
                 std::to_string(counts.resmq) + " .res/.mq, " +
                 std::to_string(counts.other) + " other file(s).\n"
                 "Please specify an explicit subcommand: ddsmmp | inireg | mobdump | restool";
        return SubTool::None;
    }

    std::string ext = ToLower(path.extension().string());
    SubTool tool = DetectFromExtension(ext);
    if (tool == SubTool::None) {
        errOut = "Cannot determine which tool to use for '" + path.string() +
                 "' (unrecognized extension '" + ext + "').\n"
                 "Please specify an explicit subcommand: ddsmmp | inireg | mobdump | restool | xlsxdb";
    }
    return tool;
}

// Was the program started from a terminal (as opposed to a file manager / double-click)?
static bool StartedFromTerminal() {
#ifdef _WIN32
    // A console program that is double-clicked gets a console of its own, attached to nothing else;
    // started from cmd or PowerShell it shares theirs.
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) > 1;
#else
    return isatty(STDIN_FILENO) || isatty(STDOUT_FILENO);
#endif
}

// `um-multitool gui [--viewer <category> <item>] [--map <file>...] [--db <file>] [--settings] [--screenshot <file.bmp>]`
static int StartGui(int argc, char* argv[], int first) {
    GuiOptions options;
    for (int i = first; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--viewer" && i + 2 < argc) {
            options.viewerCategory = argv[i + 1];
            options.viewerItem = argv[i + 2];
            i += 2;
        } else if (a == "--viewer") {
            options.openViewer = true;
        } else if (a == "--map") {
            options.openMap = true;
            while (i + 1 < argc && argv[i + 1][0] != '-') options.mapFiles.push_back(argv[++i]);
        } else if (a == "--db" && i + 1 < argc) {
            options.dbFile = argv[++i];
        } else if (a == "--naked") {
            options.viewerNaked = true;
        } else if (a == "--skin" && i + 1 < argc) {
            options.viewerSkin = argv[++i];
        } else if (a == "--mp" && i + 1 < argc) {
            options.mpFolder = argv[++i];
        } else if (a == "--settings") {
            options.openSettings = true;
        } else if (a == "--screenshot" && i + 1 < argc) {
            options.screenshotPath = argv[++i];
        }
    }
    return RunGui(options);
}

static const char* BashCompletionScript(); // defined with the completion code below

// ~/.bashrc entry for systems without the bash-completion package: a comment line, then the line that
// loads the completion. Both are found again (by the comment) to be removed.
static const char* const kBashrcMarker = "# um-multitool: bash tab completion (added by 'um-multitool install-desktop', removed by 'install-desktop --remove')";
static const char* const kBashrcLine = "command -v um-multitool >/dev/null 2>&1 && eval \"$(um-multitool completion bash)\"";

// Appends the two lines to the file unless the marker is already there. Returns true when it added them.
static bool BashrcAdd(const fs::path& bashrc) {
    std::string text;
    { std::ifstream in(bashrc); if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
    if (text.find(kBashrcMarker) != std::string::npos) return false;
    std::ofstream out(bashrc, std::ios::app);
    if (!out.is_open()) return false;
    if (!text.empty() && text.back() != '\n') out << "\n";
    out << kBashrcMarker << "\n" << kBashrcLine << "\n";
    return out.good();
}

// Removes the marker line and the line after it. Returns true when it did; the rest of the file is kept as is.
static bool BashrcRemove(const fs::path& bashrc) {
    std::ifstream in(bashrc);
    if (!in) return false;
    std::string line, result;
    bool removed = false;
    while (std::getline(in, line)) {
        if (!removed && line == kBashrcMarker) {
            removed = true;
            std::getline(in, line); // the eval line
            if (line != kBashrcLine) result += line + "\n"; // not ours (edited by hand): keep it
            continue;
        }
        result += line + "\n";
    }
    in.close();
    if (!removed) return false;
    std::ofstream out(bashrc, std::ios::trunc);
    out << result;
    return out.good();
}

// `um-multitool install-desktop [--remove]` (Linux): installs um-multitool.desktop and the icon for the
// current user (~/.local/share/applications, and the 256-pixel icon in ~/.local/share/icons/hicolor),
// with this binary's absolute path, so the tool shows in the application menu with its icon (on Wayland,
// also the window's). Menus find the icon by its theme name and scale it to the size they need.
static int InstallDesktop(int argc, char* argv[]) {
#ifdef _WIN32
    (void)argc; (void)argv;
    std::cerr << "install-desktop is for Linux desktops; on Windows the .exe carries its icon.\n";
    return 1;
#else
    const bool remove = argc > 2 && std::string(argv[2]) == "--remove";
    std::error_code ec;
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if ((!dataHome || !*dataHome) && (!home || !*home)) { std::cerr << "Error: HOME is not set\n"; return 1; }
    const fs::path data = dataHome && *dataHome ? fs::path(dataHome) : fs::path(home) / ".local" / "share";
    const fs::path desktop = data / "applications" / "um-multitool.desktop";
    const fs::path hicolor = data / "icons" / "hicolor";
    // Bash tab completion: bash-completion loads this file by the command's name on the first Tab.
    // A file of its own, so no existing file (.bashrc) is edited.
    const fs::path completion = data / "bash-completion" / "completions" / "um-multitool";
    const char* homeDir = std::getenv("HOME");
    const fs::path bashrc = homeDir && *homeDir ? fs::path(homeDir) / ".bashrc" : fs::path();
    // Earlier versions installed every usual size: --remove still removes them all.
    const int sizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
    auto iconAt = [&](int size) { return hicolor / (std::to_string(size) + "x" + std::to_string(size)) / "apps" / "um-multitool.png"; };
    // Tells the desktop to read the menu entries and the icons again: the menu database
    // (update-desktop-database), the icon cache and KDE's service cache. Each is optional.
    auto refresh = [&]() {
        const std::string cmd = "update-desktop-database -q \"" + desktop.parent_path().string() + "\" >/dev/null 2>&1;"
                                " gtk-update-icon-cache -q -t \"" + hicolor.string() + "\" >/dev/null 2>&1;"
                                " (kbuildsycoca6 || kbuildsycoca5) >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {} // other desktops notice the files by themselves
    };
    if (remove) {
        bool any = fs::remove(desktop, ec);
        if (any) std::cout << "Removed " << desktop.string() << "\n";
        if (fs::remove(completion, ec)) { std::cout << "Removed " << completion.string() << " (bash tab completion)\n"; any = true; }
        if (!bashrc.empty() && BashrcRemove(bashrc)) { std::cout << "Removed the um-multitool completion lines from " << bashrc.string() << "\n"; any = true; }
        for (int size : sizes) if (fs::remove(iconAt(size), ec)) { std::cout << "Removed " << iconAt(size).string() << "\n"; any = true; }
        if (!any) std::cout << "Nothing to remove.\n";
        else refresh();
        return 0;
    }
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) { std::cerr << "Error: cannot find this program's path\n"; return 1; }
    fs::create_directories(desktop.parent_path(), ec);
    {
        const fs::path from = exe.parent_path() / "assets" / "logo-256.png";
        const fs::path to = iconAt(256);
        fs::create_directories(to.parent_path(), ec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
            std::cerr << "Error: cannot copy " << from << " to " << to << ": " << ec.message() << "\n";
            return 1;
        }
    }
    std::ofstream f(desktop, std::ios::trunc);
    if (!f.is_open()) { std::cerr << "Error: cannot write " << desktop << "\n"; return 1; }
    // Exec quotes the path (it may hold spaces); Icon is the theme name, found in hicolor.
    f << "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Universal Mod Multitool\n"
         "GenericName=Evil Islands Modding Toolkit\n"
         "Comment=Evil Islands modding: file conversion, 3D viewer and map editor\n"
         "Exec=\"" << exe.string() << "\" gui\n"
         "Path=" << exe.parent_path().string() << "\n"
         "Icon=um-multitool\n"
         "Terminal=false\n"
         "Categories=Development;\n"
         "Keywords=Evil Islands;Cursed Lands;modding;map editor;mob;mpr;\n"
         "StartupWMClass=um-multitool\n"
         "StartupNotify=true\n";
    f.close();
    refresh();
    std::cout << "Installed " << desktop << "\n     and the icon in " << hicolor << " (256 pixels)\n"
              << "Universal Mod Multitool is now in the application menu (it may take a moment to appear).\n";
    // The completion is optional: a failure here does not undo the menu entry.
    fs::create_directories(completion.parent_path(), ec);
    std::ofstream c(completion, std::ios::trunc);
    if (c.is_open() && (c << BashCompletionScript()) && (c.close(), true)) {
        std::cout << "Installed " << completion << " (bash tab completion for um-multitool).\n";
        // The bash-completion package is what loads that file. Without it, ~/.bashrc loads the completion instead.
        if (fs::exists("/usr/share/bash-completion/bash_completion", ec) || fs::exists("/etc/bash_completion", ec)) {
            std::cout << "  It is loaded by the bash-completion package in new shells; " << (bashrc.empty() ? "" : "~/.bashrc is not edited.") << "\n";
        } else if (!bashrc.empty() && BashrcAdd(bashrc)) {
            std::cout << "  The bash-completion package was not found, so these two lines were added to the end of " << bashrc.string() << ":\n"
                      << "    " << kBashrcMarker << "\n    " << kBashrcLine << "\n"
                      << "  (--remove takes them out again). Open a new terminal to use the completion.\n";
        } else {
            std::cout << "  The bash-completion package was not found: add this line to ~/.bashrc to load it: eval \"$(um-multitool completion bash)\"\n";
        }
    } else {
        std::cerr << "Warning: cannot write " << completion << " (bash tab completion not installed)\n";
    }
    std::cout << "Undo with: um-multitool install-desktop --remove\n";
    return 0;
#endif
}


// ---- shell completion (bash) ---------------------------------------------------------------------
// `um-multitool completion bash` prints a script to load with: eval "$(um-multitool completion bash)"
// (put that line in ~/.bashrc). The script calls `um-multitool __complete <words>` for the candidates; with no
// candidates bash completes file names as usual. The flags below are those of each subcommand's --help.

struct CompletionEntry { const char* name; const char* flags; };
static const CompletionEntry kCompletions[] = {
    {"ddsmmp",   "-d --dir -m --multi -o --output --dry-run --dds2mmp --mmp2dds -h --help --version"},
    {"inireg",   "-d --dir -m --multi -o --output --dry-run --ini2reg --reg2ini -h --help --version"},
    {"mobdump",  "-d --dir -m --multi -o --output --dry-run -h --help --version"},
    {"restool",  "-d --dir -m --multi -o --output --dry-run --pack --unpack --ext -e --exclude -s --strip --no-strip --strip-ext --no-strip-ext -h --help --version"},
    {"xlsxdb",   "-o --output --check --no-check -h --help --version"},
    {"dbexport", "-o --output -h --help"},
    {"viewer",   "--list --resolve --render --gif --uvdump --uvmap --material --texture --size --config --help"},
    {"map",      "--check --navmesh --mpr --write --force --config --help"},
    {"dll",      "--port --listen --stats --help"},
    {"gui",      "--db --viewer --map --mp --skin --naked --settings --screenshot"},
    {"install-desktop", "--remove"},
    {"completion", "bash"},
};
// Subcommand aliases, to the names above.
static std::string CompletionName(const std::string& tok) {
    if (tok == "dds") return "ddsmmp";
    if (tok == "ini") return "inireg";
    if (tok == "mob") return "mobdump";
    if (tok == "res") return "restool";
    if (tok == "db") return "xlsxdb";
    return tok;
}

// words: the command line after the program name, up to and including the word being completed.
static int PrintCompletions(const std::vector<std::string>& words) {
    if (words.empty()) return 0;
    const std::string& cur = words.back();
    if (words.size() == 1) {
        if (!cur.empty() && cur[0] == '-') { std::cout << "-h --help --version\n"; return 0; }
        for (const auto& e : kCompletions) std::cout << e.name << "\n";
        std::cout << "dds ini mob res db\n";
        return 0;
    }
    const std::string sub = CompletionName(words[0]);
    const std::string prev = words[words.size() - 2];
    // Values with a fixed set: the 3D Viewer's item categories.
    if (sub == "viewer" && words.size() == 3 && (prev == "--list" || prev == "--resolve" || prev == "--render" || prev == "--gif")) {
        std::cout << "weapons armors quick quest loot\n";
        return 0;
    }
    if (!cur.empty() && cur[0] == '-') {
        for (const auto& e : kCompletions) if (sub == e.name) std::cout << e.flags << "\n";
    }
    return 0;
}

static const char* kBashCompletion =
    "_um_multitool() {\n"
    "    local cur=${COMP_WORDS[COMP_CWORD]}\n"
    "    COMPREPLY=($(compgen -W \"$(\"${COMP_WORDS[0]}\" __complete \"${COMP_WORDS[@]:1:COMP_CWORD}\" 2>/dev/null)\" -- \"$cur\"))\n"
    "}\n"
    "complete -o default -F _um_multitool um-multitool\n";
static const char* BashCompletionScript() { return kBashCompletion; }

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string(argv[1]) == "__complete") {
        return PrintCompletions(std::vector<std::string>(argv + 2, argv + argc));
    }
    if (argc >= 2 && std::string(argv[1]) == "completion") {
        if (argc == 3 && std::string(argv[2]) == "bash") { std::cout << kBashCompletion; return 0; }
        std::cerr << "Usage: eval \"$(um-multitool completion bash)\"   (bash only; add it to ~/.bashrc)\n";
        return 1;
    }
    if (argc < 2) {
        if (!StartedFromTerminal()) {
#ifdef _WIN32
            FreeConsole(); // double-clicked: close the console window Windows opened for it
#endif
            return StartGui(argc, argv, argc);
        }
        PrintTopLevelHelp();
        return 1;
    }

    std::string first = argv[1];
    if (first == "gui") return StartGui(argc, argv, 2);
    if (first == "viewer") return viewer::RunCli(argc - 1, argv + 1);
    if (first == "map") return mapedit::RunCli(argc - 1, argv + 1);
    if (first == "dll") return dllconnect::RunCli(argc - 1, argv + 1);
    if (first == "install-desktop") return InstallDesktop(argc, argv);
    if (first == "-h" || first == "--help") {
        PrintTopLevelHelp();
        return 0;
    }
    if (first == "--version") {
        PrintTopLevelVersion();
        return 0;
    }

    SubTool tool = MatchSubcommand(first);
    if (tool != SubTool::None) {
        // Forward remaining args, dropping the subcommand token itself.
        std::vector<char*> newArgv;
        newArgv.push_back(argv[0]);
        for (int i = 2; i < argc; ++i) {
            newArgv.push_back(argv[i]);
        }
        return DispatchTo(tool, static_cast<int>(newArgv.size()), newArgv.data());
    }

    // No recognized subcommand: attempt auto-detection from the first positional path.
    std::string candidate = FindPositionalArg(argc, argv);
    if (candidate.empty()) {
        std::cerr << "Error: No subcommand or input path recognized.\n\n";
        PrintTopLevelHelp();
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(candidate, ec)) {
        std::cerr << "Error: Input path does not exist: " << candidate << "\n";
        return 1;
    }

    std::string detectErr;
    SubTool detected = AutoDetect(candidate, detectErr);
    if (detected == SubTool::None) {
        std::cerr << "Error: " << detectErr << "\n";
        return 1;
    }

    // ddsmmp/inireg/mobdump now require an explicit -d for directory input;
    // inject it here since the user only supplied a bare directory path.
    if (fs::is_directory(candidate, ec) &&
        (detected == SubTool::DdsMmp || detected == SubTool::IniReg || detected == SubTool::MobDump)) {
        std::vector<char*> newArgv;
        newArgv.push_back(argv[0]);
        newArgv.push_back(const_cast<char*>("-d"));
        for (int i = 1; i < argc; ++i) {
            newArgv.push_back(argv[i]);
        }
        return DispatchTo(detected, static_cast<int>(newArgv.size()), newArgv.data());
    }

    return DispatchTo(detected, argc, argv);
}
