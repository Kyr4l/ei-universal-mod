// Static checker for the Evil Islands mission script language (the text stored
// encrypted in a .mob's SS_TEXT node, and dumped as .eis by um-multitool).
//
// Pure standard C++ (no Windows headers) so it can be unit-tested natively; um.cpp
// includes it and reports the findings through its logger.
//
// The language, as found in the shipped maps:
//
//   GlobalVars ( name : type, ... )            type = object | group | float | string
//   DeclareScript Name ( param : type, ... )
//   Script Name ( statements )                 parameters come from its DeclareScript
//   WorldScript ( statements )
//
//   statement  = if ( condition... ) then ( statements ) [ else ( statements ) ]
//              | Command( expression, ... ) [ ( statements ) ]    -- For/ForIf take a block
//              | variable = expression
//   expression = number | "string" | variable | Command( expression, ... )
//
// Identifiers may contain '#', '-' and '.'; comments start with //. Several
// conditions inside one if ( ) are all required. Two things are implicit and
// therefore NOT errors: a group variable is created by its first use in a group
// position, and an undeclared name in an object position refers to a named
// object placed in the map (which may live in another .mob loaded on top).
//
// What is reported:
//   E  the script cannot work: syntax errors, wrong argument count for a known
//      command, unknown types, undeclared variables in number/string positions,
//      a command that returns nothing used as a value.
//   W  suspicious: an argument/condition/assignment of the wrong type, a script
//      call with the wrong argument count, an unknown name that looks like a
//      typo of a known command, a called script that is declared but has no body.
//   I  worth knowing but usually fine: a name that is neither a known command
//      nor a script of this file (scripts may be defined in another .mob).
#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mob_script_functions.hpp"

struct MobScriptIssue {
    int line;
    char severity; // 'E', 'W' or 'I'
    std::string message;
};

// What a script declares, so that a quest map (which the game loads on top of its
// zone's base map and which freely uses the base map's variables and scripts) can be
// checked against the base map's declarations as well as its own.
struct MobScriptDeclarations {
    std::unordered_map<std::string, char> globals;  // lower-case name -> type letter
    std::unordered_map<std::string, std::string> scripts; // lower-case name -> parameter type letters
    std::unordered_set<std::string> defined;        // lower-case names that have a Script body
};

// A reference from the script to something that lives in the map (checked by the caller
// against the map's objects, which this header knows nothing about).
struct MobScriptReference {
    std::string text; // an object ID (digits) or an object name
    int line;
};

// An item or spell name given to a command as a text literal, to be looked up in the database.
struct MobScriptDatabaseName {
    char kind;           // 'i' item, 's' spell
    std::string text;
    int line;
    std::string command; // for the message
};

struct MobScriptReport {
    std::vector<MobScriptIssue> issues; // capped at kMaxIssues
    int scriptCount = 0;                // Script + WorldScript bodies parsed
    int errors = 0;
    int warnings = 0;
    int infos = 0;
    int suppressed = 0;                 // issues beyond the cap (not stored)
    MobScriptDeclarations declarations; // what this script declares (for the maps loaded on top of it)
    std::vector<MobScriptReference> objectIds;    // IDs in GetObject(N) / GetObjectByID("N"), also inside strings
    std::vector<MobScriptReference> objectNames;  // undeclared names used where an object is expected
    std::vector<std::string> addMobs;             // files loaded with AddMob("file.mob")
    std::vector<MobScriptDatabaseName> databaseNames; // item/spell names to look up in the database
};

namespace mobscript {

static const size_t kMaxIssues = 40;
static const size_t kMaxSourceBytes = 2u * 1024 * 1024;
static const int kMaxDepth = 100;

struct Token {
    enum Kind { End, Ident, Number, String, Punct } kind;
    std::string text;
    int line;
};

inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

inline std::string LowerCase(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = Lower(r[i]);
    return r;
}

// Characters that can be part of a name or number (bytes >= 0x80 are Cyrillic in cp1251).
inline bool IsWordChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '#' || c == '.' || c == '-' || c >= 0x80;
}

inline bool IsNumberText(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
    if (digits == 0) return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        size_t fraction = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++fraction; }
        if (fraction == 0) return false;
    }
    return i == s.size();
}

// "a number", "a string", "an object", "a group" (with the article).
inline const char* TypeName(char t) {
    switch (t) {
    case 'f': return "a number";
    case 's': return "a string";
    case 'o': return "an object";
    case 'g': return "a group";
    default: return "a value";
    }
}

inline char TypeFromName(const std::string& lowered) {
    if (lowered == "float") return 'f';
    if (lowered == "string") return 's';
    if (lowered == "object") return 'o';
    if (lowered == "group") return 'g';
    return 0;
}

// Lookup of command signatures by lower-case name, built once.
inline const std::unordered_map<std::string, const MobScriptFunction*>& FunctionTable() {
    static const std::unordered_map<std::string, const MobScriptFunction*> table = [] {
        std::unordered_map<std::string, const MobScriptFunction*> t;
        for (const MobScriptFunction& f : kMobScriptFunctions) t[LowerCase(f.name)] = &f;
        return t;
    }();
    return table;
}

// Small edit distance for "did you mean" suggestions on names of realistic length.
inline int EditDistance(const std::string& a, const std::string& b) {
    if (a.size() > 40 || b.size() > 40) return 99;
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int best = prev[j - 1] + cost;
            if (prev[j] + 1 < best) best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            cur[j] = best;
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

class Checker {
public:
    // base: declarations of the map(s) this one is loaded on top of, or null. When the
    // file is a quest map whose base could not be found, pass baseMissing = true so names
    // that are not declared locally are reported softly instead of as errors.
    MobScriptReport Run(const std::string& source, const MobScriptDeclarations* base, bool baseMissing) {
        report_ = MobScriptReport();
        base_ = base;
        baseMissing_ = baseMissing;
        if (source.size() > kMaxSourceBytes) {
            Add(1, 'I', "script is larger than %u MB and was not checked", static_cast<unsigned>(kMaxSourceBytes >> 20));
            return report_;
        }
        if (!Lex(source)) {
            return report_;
        }
        CollectIdsFromStrings();
        while (Peek().kind != Token::End && !abort_) {
            Item();
        }
        ResolveCalls();
        for (const auto& g : globals_) report_.declarations.globals[g.first] = g.second;
        for (const auto& d : declared_) {
            std::string types;
            for (const auto& p : d.second.params) types.push_back(p.second);
            report_.declarations.scripts[d.first] = types;
        }
        report_.declarations.defined = defined_;
        return report_;
    }

private:
    // ------------------------------------------------------------------ issues
    void Add(int line, char severity, const char* format, ...) {
        if (severity == 'E') ++report_.errors;
        else if (severity == 'W') ++report_.warnings;
        else ++report_.infos;
        if (report_.issues.size() >= kMaxIssues) {
            ++report_.suppressed;
            return;
        }
        char buffer[400];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(buffer, sizeof(buffer), format, arguments);
        va_end(arguments);
        std::string message = buffer;
        if (!currentScript_.empty()) {
            message = "in " + currentScript_ + ": " + message;
        }
        report_.issues.push_back(MobScriptIssue{line, severity, message});
    }

    void Fail(int line, const char* format, ...) {
        if (abort_) return; // the first syntax error is the real one; the rest is fallout
        char buffer[400];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(buffer, sizeof(buffer), format, arguments);
        va_end(arguments);
        Add(line, 'E', "%s", buffer);
        abort_ = true;
    }

    // ------------------------------------------------------------------- lexer
    bool Lex(const std::string& src) {
        size_t pos = 0;
        int line = 1;
        bool clean = true;
        while (pos < src.size()) {
            unsigned char c = static_cast<unsigned char>(src[pos]);
            if (c == '\n') { ++line; ++pos; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == 0 || c == 0x1A) { ++pos; continue; }
            if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                while (pos < src.size() && src[pos] != '\n') ++pos;
                continue;
            }
            if (c == '"') {
                size_t end = src.find('"', pos + 1);
                if (end == std::string::npos) {
                    Add(line, 'E', "string starting here is never closed (missing closing quote)");
                    return false;
                }
                std::string text = src.substr(pos + 1, end - pos - 1);
                tokens_.push_back(Token{Token::String, text, line});
                for (size_t k = 0; k < text.size(); ++k) if (text[k] == '\n') ++line;
                pos = end + 1;
                continue;
            }
            if (c == '(' || c == ')' || c == ',' || c == ':' || c == '=') {
                tokens_.push_back(Token{Token::Punct, std::string(1, static_cast<char>(c)), line});
                ++pos;
                continue;
            }
            if (IsWordChar(c)) {
                size_t start = pos;
                while (pos < src.size() && IsWordChar(static_cast<unsigned char>(src[pos]))) ++pos;
                std::string text = src.substr(start, pos - start);
                tokens_.push_back(Token{IsNumberText(text) ? Token::Number : Token::Ident, text, line});
                continue;
            }
            Add(line, 'E', "unexpected character '%c' (0x%02X)", (c >= 32 && c < 127) ? static_cast<char>(c) : '?', c);
            clean = false;
            ++pos;
        }
        tokens_.push_back(Token{Token::End, "", line});
        return clean;
    }

    // Quest commands take object references as text: QObjSeeUnit( "GetObject(1000183)" ).
    void CollectIdsFromStrings() {
        for (const Token& t : tokens_) {
            if (t.kind != Token::String) continue;
            std::string lowered = LowerCase(t.text);
            size_t at = 0;
            while ((at = lowered.find("getobject(", at)) != std::string::npos) {
                size_t i = at + 10;
                std::string digits;
                while (i < lowered.size() && lowered[i] >= '0' && lowered[i] <= '9') digits.push_back(lowered[i++]);
                if (!digits.empty() && i < lowered.size() && lowered[i] == ')') {
                    report_.objectIds.push_back(MobScriptReference{digits, t.line});
                }
                at = i;
            }
        }
    }

    // ------------------------------------------------------------ token access
    const Token& Peek(size_t ahead = 0) const {
        size_t j = pos_ + ahead;
        return j < tokens_.size() ? tokens_[j] : tokens_.back();
    }
    const Token& Next() {
        const Token& t = Peek();
        if (pos_ < tokens_.size() - 1) ++pos_; // never step past End
        return t;
    }
    static std::string Describe(const Token& t) {
        return t.kind == Token::End ? "the end of the script" : "'" + t.text + "'";
    }
    bool IsPunct(const Token& t, char c) const {
        return t.kind == Token::Punct && t.text[0] == c;
    }
    bool Expect(char punct, const char* context) {
        if (abort_) return false;
        const Token& t = Next();
        if (t.kind == Token::Punct && t.text[0] == punct) return true;
        Fail(t.line, "expected '%c' %s but found %s", punct, context, Describe(t).c_str());
        return false;
    }
    bool ExpectWord(const char* word, const char* context) {
        if (abort_) return false;
        const Token& t = Next();
        if (t.kind == Token::Ident && LowerCase(t.text) == word) return true;
        Fail(t.line, "expected '%s' %s but found %s", word, context, Describe(t).c_str());
        return false;
    }

    // --------------------------------------------------------------- top level
    void Item() {
        const Token& head = Next();
        std::string word = LowerCase(head.text);
        if (head.kind != Token::Ident) {
            Fail(head.line, "unexpected %s at the top level (expected GlobalVars, DeclareScript, Script or WorldScript)",
                Describe(head).c_str());
            return;
        }
        if (word == "globalvars") {
            GlobalVars();
        } else if (word == "declarescript") {
            DeclareScript();
        } else if (word == "script") {
            ScriptBody(false);
        } else if (word == "worldscript") {
            ScriptBody(true);
        } else {
            Fail(head.line, "unexpected '%s' at the top level (expected GlobalVars, DeclareScript, Script or WorldScript)",
                head.text.c_str());
        }
    }

    // name : type [, name : type ...] up to the closing ')' - shared by GlobalVars and DeclareScript.
    bool ParseTypedList(std::vector<std::pair<std::string, char>>& out, const char* what) {
        while (!IsPunct(Peek(), ')') && Peek().kind != Token::End && !abort_) {
            const Token& name = Next();
            if (name.kind != Token::Ident) {
                Fail(name.line, "expected a name in %s but found %s", what, Describe(name).c_str());
                return false;
            }
            if (!Expect(':', "after the name in a declaration")) return false;
            const Token& type = Next();
            char code = type.kind == Token::Ident ? TypeFromName(LowerCase(type.text)) : 0;
            if (!code) {
                Add(type.line, 'E', "unknown type %s for '%s' (expected object, group, float or string)",
                    Describe(type).c_str(), name.text.c_str());
                code = '?';
            }
            out.push_back({LowerCase(name.text), code});
            if (IsPunct(Peek(), ',')) Next();
            else if (!IsPunct(Peek(), ')')) {
                const Token& bad = Peek();
                Fail(bad.line, "expected ',' or ')' in %s but found %s", what, Describe(bad).c_str());
                return false;
            }
        }
        return true;
    }

    void GlobalVars() {
        if (!Expect('(', "after GlobalVars")) return;
        std::vector<std::pair<std::string, char>> vars;
        if (!ParseTypedList(vars, "GlobalVars")) return;
        if (!Expect(')', "to close GlobalVars")) return;
        for (const auto& v : vars) globals_[v.first] = v.second;
    }

    void DeclareScript() {
        const Token& name = Next();
        if (name.kind != Token::Ident) {
            Fail(name.line, "expected a script name after DeclareScript but found %s", Describe(name).c_str());
            return;
        }
        if (!Expect('(', "after the script name in DeclareScript")) return;
        std::vector<std::pair<std::string, char>> params;
        if (!ParseTypedList(params, "DeclareScript")) return;
        if (!Expect(')', "to close DeclareScript")) return;
        DeclaredScript declared;
        declared.name = name.text;
        declared.params = params;
        declared.line = name.line;
        declared_[LowerCase(name.text)] = declared;
    }

    void ScriptBody(bool world) {
        scope_.clear();
        std::string name = "WorldScript";
        if (!world) {
            const Token& nameToken = Next();
            if (nameToken.kind != Token::Ident) {
                Fail(nameToken.line, "expected a script name after Script but found %s", Describe(nameToken).c_str());
                return;
            }
            name = nameToken.text;
            defined_.insert(LowerCase(name));
            auto it = declared_.find(LowerCase(name));
            if (it != declared_.end()) {
                for (const auto& p : it->second.params) scope_[p.first] = p.second;
            } else {
                scope_["this"] = 'o';
            }
            currentScript_ = "script '" + name + "'";
        } else {
            currentScript_ = "WorldScript";
        }
        if (!Expect('(', "to start the script body")) return;
        Statements();
        if (!Expect(')', "to close the script body")) return;
        ++report_.scriptCount;
        currentScript_.clear();
    }

    // -------------------------------------------------------------- statements
    void Statements() {
        if (++depth_ > kMaxDepth) {
            Fail(Peek().line, "blocks are nested more than %d levels deep", kMaxDepth);
            return;
        }
        while (!IsPunct(Peek(), ')') && Peek().kind != Token::End && !abort_) {
            Statement();
        }
        --depth_;
    }

    void Block(const char* after) {
        if (!Expect('(', after)) return;
        Statements();
        if (abort_) return;
        Expect(')', "to close the block");
    }

    void Statement() {
        const Token& first = Peek();
        std::string word = LowerCase(first.text);
        if (first.kind == Token::Ident && word == "if") {
            Next();
            if (!Expect('(', "after 'if'")) return;
            while (!IsPunct(Peek(), ')') && Peek().kind != Token::End && !abort_) {
                int line = Peek().line;
                char type = Expression(0);
                if (abort_) return;
                if (type != 'f' && type != '?' && type != 'v') {
                    Add(line, 'W', "an if condition is %s, expected a number (true = non-zero)", TypeName(type));
                }
            }
            if (!Expect(')', "to close the if conditions")) return;
            if (!ExpectWord("then", "after the if conditions")) return;
            Block("after 'then'");
            if (abort_) return;
            if (Peek().kind == Token::Ident && LowerCase(Peek().text) == "else") {
                Next();
                Block("after 'else'");
            }
            return;
        }
        if (first.kind == Token::Ident && IsPunct(Peek(1), '=')) {
            const Token& target = Next();
            Next(); // '='
            char variableType = Lookup(target, 0);
            char valueType = Expression(variableType);
            if (abort_) return;
            if (variableType && variableType != '?' && valueType != '?' && valueType != variableType && valueType != 'v') {
                Add(target.line, 'W', "assigning %s to variable '%s', which is %s",
                    TypeName(valueType), target.text.c_str(), TypeName(variableType));
            }
            return;
        }
        if (first.kind == Token::Ident && IsPunct(Peek(1), '(')) {
            std::string lowered = LowerCase(first.text);
            Call();
            if (abort_) return;
            if ((lowered == "for" || lowered == "forif") && IsPunct(Peek(), '(')) {
                Block("to start the loop body");
            }
            return;
        }
        Next();
        Fail(first.line, "unexpected %s where a statement was expected", Describe(first).c_str());
    }

    // Type of a variable, or 0 when it is not declared (after reporting it, unless the
    // slot it stands in accepts a not-yet-declared name).
    char Lookup(const Token& name, char expected) {
        std::string key = LowerCase(name.text);
        auto local = scope_.find(key);
        if (local != scope_.end()) return local->second;
        auto global = globals_.find(key);
        if (global != globals_.end()) return global->second;
        if (base_) {
            auto inBase = base_->globals.find(key);
            if (inBase != base_->globals.end()) return inBase->second;
        }
        if (expected == 'g') {
            globals_[key] = 'g'; // a group is created by its first use
            return 'g';
        }
        if (expected == 'o') {
            // a named object placed in the map
            if (noted_.insert(key).second) {
                report_.objectNames.push_back(MobScriptReference{name.text, name.line});
            }
            return 'o';
        }
        if (expected == '?') return '?';
        if (baseMissing_) {
            Add(name.line, 'W', "variable '%s' is not declared in this file (a quest map may take it from its base map, which was not found)",
                name.text.c_str());
            return '?';
        }
        Add(name.line, 'E', "variable '%s' is not declared (add it to GlobalVars)", name.text.c_str());
        return 0;
    }

    // ------------------------------------------------------------- expressions
    // Returns 'f','s','o','g', 'v' (a command that returns nothing), '?' (unknown) or 0 (error already reported).
    char Expression(char expected) {
        if (++depth_ > kMaxDepth) {
            Fail(Peek().line, "expression is nested more than %d levels deep", kMaxDepth);
            return 0;
        }
        char result = 0;
        const Token& t = Peek();
        if (t.kind == Token::Number) {
            Next();
            result = 'f';
        } else if (t.kind == Token::String) {
            Next();
            result = 's';
        } else if (t.kind == Token::Ident) {
            if (IsPunct(Peek(1), '(')) {
                result = Call();
                if (result == 'v' && !abort_) {
                    Add(t.line, 'E', "'%s()' does not return a value, so it cannot be used inside another command or condition",
                        t.text.c_str());
                }
            } else {
                std::string lowered = LowerCase(t.text);
                if (lowered == "then" || lowered == "else" || lowered == "if") {
                    Next();
                    Fail(t.line, "unexpected '%s' where a value was expected", t.text.c_str());
                } else {
                    Next();
                    result = Lookup(t, expected);
                }
            }
        } else {
            Next();
            Fail(t.line, "expected a value but found %s", Describe(t).c_str());
        }
        --depth_;
        return result;
    }

    // Parses NAME ( args ) and checks it. Returns the command's result type.
    char Call() {
        const Token& name = Next();
        Next(); // '('
        std::string lowered = LowerCase(name.text);

        const auto& table = FunctionTable();
        auto known = table.find(lowered);
        const MobScriptFunction* function = known != table.end() ? known->second : nullptr;
        auto script = declared_.find(lowered);
        const std::string* baseScriptParams = nullptr;
        if (base_) {
            auto found = base_->scripts.find(lowered);
            if (found != base_->scripts.end()) baseScriptParams = &found->second;
        }
        const bool inBase = baseScriptParams != nullptr;

        const Token& firstArg = Peek();
        const bool onlyArg = IsPunct(Peek(1), ')');
        if (onlyArg && (lowered == "getobject" || lowered == "getobjectbyid") &&
            (firstArg.kind == Token::Number || firstArg.kind == Token::String)) {
            std::string digits;
            for (char c : firstArg.text) if (c >= '0' && c <= '9') digits.push_back(c);
            if (!digits.empty() && digits.size() == firstArg.text.size()) {
                report_.objectIds.push_back(MobScriptReference{digits, firstArg.line});
            }
        } else if (onlyArg && lowered == "addmob" && firstArg.kind == Token::String) {
            report_.addMobs.push_back(firstArg.text);
        }

        std::vector<std::pair<int, char>> args; // line, type
        std::vector<std::string> literals;      // the text of an argument that is a lone string literal, else ""
        while (!IsPunct(Peek(), ')') && Peek().kind != Token::End && !abort_) {
            const Token& argToken = Peek();
            bool loneString = argToken.kind == Token::String && (IsPunct(Peek(1), ',') || IsPunct(Peek(1), ')'));
            literals.push_back(loneString ? argToken.text : std::string());
            char expectedType = 0;
            if (function) {
                const char* params = function->params;
                if (params[0] == '*') expectedType = params[1];
                else if (args.size() < strlen(params)) expectedType = params[args.size()];
            } else if (script != declared_.end() && args.size() < script->second.params.size()) {
                expectedType = script->second.params[args.size()].second;
            } else if (script == declared_.end() && inBase && args.size() < baseScriptParams->size()) {
                expectedType = (*baseScriptParams)[args.size()];
            }
            int line = Peek().line;
            char type = Expression(expectedType);
            if (abort_) return 0;
            args.push_back({line, type});
            if (IsPunct(Peek(), ',')) {
                Next();
            } else if (!IsPunct(Peek(), ')')) {
                const Token& bad = Peek();
                Fail(bad.line, "expected ',' or ')' in the arguments of %s() but found %s", name.text.c_str(), Describe(bad).c_str());
                return 0;
            }
        }
        if (!Expect(')', "to close the arguments")) return 0;

        // Commands whose text argument names an item or a spell in the database.
        {
            int paramIndex = -1;
            char kind = 0;
            if (lowered == "giveitem" || lowered == "givequestitem") { paramIndex = 1; kind = 'i'; }
            else if (lowered == "castspellunit" || lowered == "castspellpoint") { paramIndex = 0; kind = 's'; }
            if (paramIndex >= 0 && static_cast<size_t>(paramIndex) < literals.size() && !literals[paramIndex].empty()) {
                report_.databaseNames.push_back(MobScriptDatabaseName{kind, literals[paramIndex], args[paramIndex].first, name.text});
            }
        }

        if (function) {
            const char* params = function->params;
            bool variadic = params[0] == '*';
            size_t wanted = variadic ? 2 : strlen(params);
            bool countOk = variadic ? args.size() >= wanted : args.size() == wanted;
            if (!countOk) {
                Add(name.line, 'E', "%s() takes %s%u argument%s but %u %s given", function->name,
                    variadic ? "at least " : "", static_cast<unsigned>(wanted), wanted == 1 ? "" : "s",
                    static_cast<unsigned>(args.size()), args.size() == 1 ? "was" : "were");
            } else {
                for (size_t i = 0; i < args.size(); ++i) {
                    char want = variadic ? params[1] : params[i];
                    char got = args[i].second;
                    if (want == '?' || got == '?' || got == 0 || got == want) continue;
                    if (got == 'v') continue; // already reported as "does not return a value"
                    Add(args[i].first, 'W', "argument %u of %s() is %s, expected %s",
                        static_cast<unsigned>(i + 1), function->name, TypeName(got), TypeName(want));
                }
            }
            return function->returns;
        }
        if (script != declared_.end()) {
            scriptCalls_.push_back({lowered, name.line});
            if (args.size() != script->second.params.size()) {
                Add(name.line, 'W', "script '%s' takes %u argument%s but %u %s given", script->second.name.c_str(),
                    static_cast<unsigned>(script->second.params.size()), script->second.params.size() == 1 ? "" : "s",
                    static_cast<unsigned>(args.size()), args.size() == 1 ? "was" : "were");
            }
            return '?';
        }
        if (inBase) {
            scriptCalls_.push_back({lowered, name.line});
            if (args.size() != baseScriptParams->size()) {
                Add(name.line, 'W', "script '%s' (from the base map) takes %u argument%s but %u %s given", name.text.c_str(),
                    static_cast<unsigned>(baseScriptParams->size()), baseScriptParams->size() == 1 ? "" : "s",
                    static_cast<unsigned>(args.size()), args.size() == 1 ? "was" : "were");
            }
            return '?'; // a script of the base map
        }
        // Not a known command and not (yet) a declared script: decide at the end of the file,
        // since a DeclareScript may legitimately follow the first call.
        unresolved_.push_back(Unresolved{name.text, name.line, currentScript_});
        return '?';
    }

    // ---------------------------------------------------------- end-of-file checks
    // Runs even after a syntax error stopped the parse (what was read up to that point is
    // still valid). Then the file was not read to the end, so "not declared here" facts
    // that a later declaration could still change are left out.
    void ResolveCalls() {
        for (const Unresolved& u : unresolved_) {
            std::string lowered = LowerCase(u.name);
            if (declared_.count(lowered)) continue; // declared after its first use
            std::string suggestion;
            int best = 3;
            if (u.name.size() >= 4) {
                for (const MobScriptFunction& f : kMobScriptFunctions) {
                    int d = EditDistance(lowered, LowerCase(f.name));
                    if (d < best) { best = d; suggestion = f.name; }
                }
                for (const auto& d : declared_) {
                    int dist = EditDistance(lowered, d.first);
                    if (dist < best) { best = dist; suggestion = d.second.name; }
                }
            }
            std::string saved = currentScript_;
            currentScript_ = u.context;
            if (!suggestion.empty()) {
                Add(u.line, 'W', "'%s' is not a known command or script - did you mean '%s'?", u.name.c_str(), suggestion.c_str());
            } else if (abort_) {
                // not enough of the file was read to say this name is unknown
            } else {
                Add(u.line, 'I', "'%s' is not a known command or a script declared in this file (it may be defined in another .mob)",
                    u.name.c_str());
            }
            currentScript_ = saved;
        }
        if (abort_) {
            return;
        }
        std::unordered_set<std::string> reported;
        for (const auto& call : scriptCalls_) {
            if (defined_.count(call.first) || reported.count(call.first)) continue;
            if (base_ && base_->defined.count(call.first)) continue; // its body is in the base map
            auto local = declared_.find(call.first);
            if (local == declared_.end()) continue; // a base-map script: nothing to say here
            reported.insert(call.first);
            const DeclaredScript& d = local->second;
            Add(call.second, 'W', "script '%s' is called but has no Script body in this file (declared on line %d)",
                d.name.c_str(), d.line);
        }
    }

    struct DeclaredScript {
        std::string name;
        std::vector<std::pair<std::string, char>> params;
        int line = 0;
    };
    struct Unresolved {
        std::string name;
        int line;
        std::string context;
    };

    MobScriptReport report_;
    std::unordered_set<std::string> noted_;
    const MobScriptDeclarations* base_ = nullptr;
    bool baseMissing_ = false;
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    int depth_ = 0;
    bool abort_ = false;
    std::string currentScript_;
    std::unordered_map<std::string, char> globals_;
    std::unordered_map<std::string, char> scope_;
    std::unordered_map<std::string, DeclaredScript> declared_;
    std::unordered_set<std::string> defined_;
    std::vector<std::pair<std::string, int>> scriptCalls_;
    std::vector<Unresolved> unresolved_;
};

} // namespace mobscript

// Check the text of one mission script.
inline MobScriptReport CheckMobScript(const std::string& scriptText, const MobScriptDeclarations* base = nullptr,
        bool baseMissing = false) {
    mobscript::Checker checker;
    return checker.Run(scriptText, base, baseMissing);
}
