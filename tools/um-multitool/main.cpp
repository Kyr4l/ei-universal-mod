/**
 * ============================================================================
 * um-multitool - Evil Islands Modding Toolkit (merged CLI)
 * ============================================================================
 *
 * Description:
 *   Single binary merging four standalone tools:
 *     - ddsmmp  (formerly um-ddsmmp):  .dds  <-> .mmp  texture conversion
 *     - inireg  (formerly um-inireg):  .ini  <-> .reg  config conversion
 *     - mobdump (formerly um-mobdump): .mob  ->  .yaml/.eis map dumping
 *     - restool (formerly um-restool): .res/.mq <-> folder pack/unpack
 *
 * Dispatch rules:
 *   1. Explicit subcommand: `um-multitool <subcommand> [options] <path>`
 *   2. Auto-detect: `um-multitool <path> [options]` infers the subcommand
 *      from the input's file extension (single file) or its contents
 *      (directory, only when unambiguous). Ambiguous or unrecognized
 *      input requires an explicit subcommand.
 *
 * Version:
 *   0.1
 * ============================================================================
 */

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include "subtools.hpp"

namespace fs = std::filesystem;

static constexpr const char* PROGRAM_VERSION = "0.1";

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static void PrintTopLevelHelp() {
    std::cout << "um-multitool - Evil Islands Modding Toolkit (merged CLI)\n\n"
              << "Usage:\n"
              << "  um-multitool <subcommand> [options] <path>\n"
              << "  um-multitool <path> [options]         # auto-detects the right subcommand\n\n"
              << "Subcommands:\n"
              << "  ddsmmp   (alias: dds)   Convert textures between .dds <-> .mmp\n"
              << "  inireg   (alias: ini)   Convert configs between .ini <-> .reg\n"
              << "  mobdump  (alias: mob)   Dump .mob map files to .yaml / .eis\n"
              << "  restool  (alias: res)   Pack/unpack .res / .mq archives\n\n"
              << "Options:\n"
              << "  -v, --version   Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help      Print this help message\n\n"
              << "Run 'um-multitool <subcommand> --help' for subcommand-specific options.\n\n"
              << "Examples:\n"
              << "  um-multitool restool database.res\n"
              << "  um-multitool ddsmmp texture.dds\n"
              << "  um-multitool inireg -d ./ini -o ./reg -m\n"
              << "  um-multitool texture.dds                 # auto-detected -> ddsmmp\n\n"
              << "Note: directory-mode auto-detection only succeeds when every file in the\n"
              << "directory belongs to exactly one of ddsmmp/inireg/mobdump; anything mixed,\n"
              << "unrecognized, or restool-shaped (archives / generic asset folders) requires\n"
              << "the explicit 'restool' subcommand.\n";
}

static void PrintTopLevelVersion() {
    std::cout << "um-multitool version " << PROGRAM_VERSION << "\n"
              << "  bundles: ddsmmp, inireg, mobdump, restool (each 0.1)\n";
}

enum class SubTool { None, DdsMmp, IniReg, MobDump, ResTool };

static SubTool MatchSubcommand(const std::string& tok) {
    if (tok == "ddsmmp" || tok == "dds")  return SubTool::DdsMmp;
    if (tok == "inireg" || tok == "ini")  return SubTool::IniReg;
    if (tok == "mobdump" || tok == "mob") return SubTool::MobDump;
    if (tok == "restool" || tok == "res") return SubTool::ResTool;
    return SubTool::None;
}

static int DispatchTo(SubTool tool, int argc, char* argv[]) {
    switch (tool) {
        case SubTool::DdsMmp:  return RunDdsMmp(argc, argv);
        case SubTool::IniReg:  return RunIniReg(argc, argv);
        case SubTool::MobDump: return RunMobDump(argc, argv);
        case SubTool::ResTool: return RunResTool(argc, argv);
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
                 "Please specify an explicit subcommand: ddsmmp | inireg | mobdump | restool";
    }
    return tool;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintTopLevelHelp();
        return 1;
    }

    std::string first = argv[1];
    if (first == "-h" || first == "--help") {
        PrintTopLevelHelp();
        return 0;
    }
    if (first == "-v" || first == "--version") {
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

    return DispatchTo(detected, argc, argv);
}
