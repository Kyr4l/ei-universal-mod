// Script highlighting, shared by the Map Editor's script view and the UM DLL Connector's: comments,
// strings, numbers, the language's keywords and types, known commands (um.dll's table,
// mob_script_functions.hpp), the scripts and global variables the file declares.
#pragma once

#include <string>
#include <unordered_set>

#include "imgui.h"

#include "checks.hpp"
#include "i18n.hpp"

namespace scripthl {


struct ScriptNames {
    std::unordered_set<std::string> scripts, globals; // lower-case
};

inline ImVec4 TokenColor(const std::string& word, const ScriptNames& names) {
    static const std::unordered_set<std::string> keywords = {"globalvars", "declarescript", "script", "worldscript", "if", "then", "else"};
    static const std::unordered_set<std::string> types = {"object", "group", "float", "string"};
    std::string lower = mobscript::LowerCase(word);
    if (mobscript::IsNumberText(word)) return ImVec4(0.70f, 0.87f, 0.55f, 1);
    if (keywords.count(lower)) return ImVec4(0.80f, 0.58f, 0.98f, 1);
    if (types.count(lower)) return ImVec4(0.55f, 0.75f, 0.95f, 1);
    if (mobscript::FunctionTable().count(lower)) return ImVec4(0.96f, 0.84f, 0.45f, 1);
    if (names.scripts.count(lower)) return ImVec4(0.45f, 0.88f, 0.90f, 1);
    if (names.globals.count(lower)) return ImVec4(0.72f, 0.80f, 1.0f, 1);
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// Splits one line of script into coloured pieces: fn(begin, end, colour). What fn draws or measures is
// the script's own text: never translated by the UI language (a script's "Script Quest" stays as it is).
template <typename F> inline void EachScriptToken(const char* begin, const char* end, const ScriptNames& names, F fn) {
    const i18n::Verbatim verbatim;
    const ImVec4 comment(0.48f, 0.62f, 0.48f, 1), string(0.90f, 0.64f, 0.44f, 1), punct(0.62f, 0.62f, 0.66f, 1);
    const char* p = begin;
    while (p < end) {
        if (p + 1 < end && p[0] == '/' && p[1] == '/') { fn(p, end, comment); break; }
        if (*p == '"') {
            const char* q = p + 1;
            while (q < end && *q != '"') ++q;
            if (q < end) ++q;
            fn(p, q, string);
            p = q;
        } else if (mobscript::IsWordChar(static_cast<unsigned char>(*p))) {
            const char* q = p;
            while (q < end && mobscript::IsWordChar(static_cast<unsigned char>(*q))) ++q;
            fn(p, q, TokenColor(std::string(p, q), names));
            p = q;
        } else {
            const char* q = p;
            while (q < end && *q != '"' && !mobscript::IsWordChar(static_cast<unsigned char>(*q)) && !(q + 1 < end && q[0] == '/' && q[1] == '/')) ++q;
            fn(p, q, punct);
            p = q;
        }
    }
}

// One line of script, token by token on the same row.
inline void HighlightedLine(const char* begin, const char* end, const ScriptNames& names) {
    bool first = true;
    EachScriptToken(begin, end, names, [&](const char* b, const char* e, const ImVec4& color) {
        if (b >= e) return;
        if (!first) ImGui::SameLine(0.0f, 0.0f);
        first = false;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(b, e);
        ImGui::PopStyleColor();
    });
    if (first) ImGui::TextUnformatted("");
}

// The names a script declares (its scripts and globals), for the highlighting.
inline ScriptNames NamesOf(const std::string& cp1251) {
    ScriptNames names;
    MobScriptReport report = CheckMobScript(cp1251);
    for (const auto& kv : report.declarations.scripts) names.scripts.insert(kv.first);
    for (const auto& name : report.declarations.defined) names.scripts.insert(name);
    for (const auto& kv : report.declarations.globals) names.globals.insert(kv.first);
    return names;
}


} // namespace scripthl
