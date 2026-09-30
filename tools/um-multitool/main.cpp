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
              << "  um-multitool <subcommand> [options] <path>\n"
              << "  um-multitool <path> [options]         # auto-detects the right subcommand\n\n"
              << "Subcommands:\n"
              << "  ddsmmp   (alias: dds)   Convert textures between .dds <-> .mmp\n"
              << "  inireg   (alias: ini)   Convert configs between .ini <-> .reg\n"
              << "  mobdump  (alias: mob)   Dump .mob map files to .yaml / .eis\n"
              << "  restool  (alias: res)   Pack/unpack .res / .mq archives\n"
              << "  xlsxdb   (alias: db)    Compile .xlsx gameplay databases to .res\n"
              << "  viewer                  3D Viewer from the command line: list items, render, export GIFs\n"
              << "  map                     Check .mob maps like the Map Editor (and um.dll) do\n"
              << "  dll                     Commands to um.dll inside the running game (its DLL server): memory, threads, breakpoints\n"
              << "  install-desktop         Add um-multitool to the Linux application menu, with its icon (--remove: undo)\n\n"
              << "Options:\n"
              << "  --version       Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help      Print this help message\n\n"
              << "Run 'um-multitool <subcommand> --help' for subcommand-specific options.\n\n"
              << "Examples:\n"
              << "  um-multitool restool database.res\n"
              << "  um-multitool ddsmmp texture.dds\n"
              << "  um-multitool inireg -d ./ini -o ./reg -m\n"
              << "  um-multitool xlsxdb databaselmp.xlsx\n"
              << "  um-multitool texture.dds                 # auto-detected -> ddsmmp\n\n"
              << "Note: directory-mode auto-detection only succeeds when every file in the\n"
              << "directory belongs to exactly one of ddsmmp/inireg/mobdump; anything mixed,\n"
              << "unrecognized, or restool-shaped (archives / generic asset folders) requires\n"
              << "the explicit 'restool' subcommand. xlsxdb only ever operates on a single\n"
              << ".xlsx file, so it is also excluded from directory auto-detection.\n";
}

static void PrintTopLevelVersion() {
    std::cout << PROGRAM_NAME_SHOWN << " (um-multitool) version " << PROGRAM_VERSION << "\n"
              << "  bundles: ddsmmp, inireg, mobdump, restool, xlsxdb (each 1.0), the GUI, the 3D Viewer and the Map Editor\n";
}

enum class SubTool { None, DdsMmp, IniReg, MobDump, ResTool, XlsxDb };

static SubTool MatchSubcommand(const std::string& tok) {
    if (tok == "ddsmmp" || tok == "dds")  return SubTool::DdsMmp;
    if (tok == "inireg" || tok == "ini")  return SubTool::IniReg;
    if (tok == "mobdump" || tok == "mob") return SubTool::MobDump;
    if (tok == "restool" || tok == "res") return SubTool::ResTool;
    if (tok == "xlsxdb" || tok == "db")   return SubTool::XlsxDb;
    return SubTool::None;
}

static int DispatchTo(SubTool tool, int argc, char* argv[]) {
    switch (tool) {
        case SubTool::DdsMmp:  return RunDdsMmp(argc, argv);
        case SubTool::IniReg:  return RunIniReg(argc, argv);
        case SubTool::MobDump: return RunMobDump(argc, argv);
        case SubTool::ResTool: return RunResTool(argc, argv);
        case SubTool::XlsxDb:  return RunXlsxDb(argc, argv);
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
    if (ext == ".xlsx") return SubTool::XlsxDb;
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

// `um-multitool gui [--viewer <category> <item>] [--map <file>...] [--settings] [--screenshot <file.bmp>]`
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
        } else if (a == "--settings") {
            options.openSettings = true;
        } else if (a == "--screenshot" && i + 1 < argc) {
            options.screenshotPath = argv[++i];
        }
    }
    return RunGui(options);
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
              << "Universal Mod Multitool is now in the application menu (it may take a moment to appear).\n"

              << "Undo with: um-multitool install-desktop --remove\n";
    return 0;
#endif
}

int main(int argc, char* argv[]) {
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
