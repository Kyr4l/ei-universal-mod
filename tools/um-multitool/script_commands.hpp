// script_commands.txt beside um-multitool: script commands added to the built-in list, or changing one of
// it, for a game whose commands differ from the original's (a mod's executable, e.g. EI ATD). Read once at
// start; the script checks, the highlighting and the script editor's completion then know them.
//
//   ; a comment
//   CastSpellUnit v sffoo      name, return type, parameter types (as in mob_script_functions.hpp)
//   KillAll v -                a command without parameters
#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include "log.hpp"
#include "mob_script_check.hpp"
#include "viewer/config.hpp"

namespace scriptcmds {

inline std::string FilePath() { return config::ExeDir() + "/script_commands.txt"; }

// A signature's letters: return type v f s o g ?, parameters f s o g ? (a leading * = any number of the next).
inline bool ValidTypes(char returns, const std::string& params) {
    if (std::string("vfsog?").find(returns) == std::string::npos) return false;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i] == '*' && i == 0 && params.size() == 2) continue;
        if (std::string("fsog?").find(params[i]) == std::string::npos) return false;
    }
    return true;
}

// The file as first written: how to use it, then every built-in command commented out, to copy and change.
inline std::string Template() {
    std::string t =
        "; um-multitool: the script commands of your game, where they differ from the original game's.\n"
        "; The built-in list (below, commented out) is the original's: the VGG editor's syntax.ini, MobExplorer's\n"
        "; script_refs.txt with the SpellAddon's commands, checked against the shipped maps. A game or mod whose executable adds\n"
        "; commands or changes their parameters (e.g. EI ATD) needs its own lines here, or the Map Editor's\n"
        "; checks report its scripts' calls as wrong. Remove the ';' before a line and change it, or add a new\n"
        "; line; restart um-multitool after a change. Its log tells what was taken.\n"
        ";\n"
        ";   Name  return  parameters\n"
        ";   return:     v nothing, f number, s string, o object, g group, ? any\n"
        ";   parameters: one letter per parameter: f s o g, or ? (not checked); - for none;\n"
        ";               a leading * = any number (two or more) of the next type, e.g. *f\n"
        ";\n"
        "; e.g. if your game's CastSpellUnit takes one more object (the caster) after the target:\n"
        ";   CastSpellUnit v sffoo\n"
        ";\n";
    for (const MobScriptFunction* f : mobscript::FunctionList())
        t += std::string("; ") + f->name + " " + f->returns + " " + (f->params[0] ? f->params : "-") + "\n";
    return t;
}

// Reads the file; a missing one is written (Template), changing nothing. Returns the number of commands
// taken; bad lines are logged.
inline int Load() {
    std::ifstream in(FilePath());
    if (!in) {
        std::ofstream out(FilePath(), std::ios::binary);
        if (out) out << Template();
        return 0;
    }
    int taken = 0, lineNo = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        const size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        std::istringstream words(line);
        std::string name, returns, params, extra;
        if (!(words >> name)) continue;
        words >> returns >> params >> extra;
        if (params == "-") params.clear();
        bool nameOk = !name.empty();
        for (char c : name) nameOk &= mobscript::IsWordChar(static_cast<unsigned char>(c));
        if (!nameOk || returns.size() != 1 || !extra.empty() || !ValidTypes(returns[0], params)) {
            umlog::Write(umlog::Level::Warning, "script_commands.txt line " + std::to_string(lineNo) +
                                                    ": expected \"Name returns parameters\" (e.g. CastSpellUnit v sffoo), skipped");
            continue;
        }
        const bool known = mobscript::FunctionTable().count(mobscript::LowerCase(name)) != 0;
        mobscript::AddFunction(name, returns[0], params);
        umlog::Write(umlog::Level::Info, "script_commands.txt: " + name + (known ? " changed" : " added") + " (" + returns + " " +
                                             (params.empty() ? "-" : params) + ")");
        ++taken;
    }
    return taken;
}

} // namespace scriptcmds
