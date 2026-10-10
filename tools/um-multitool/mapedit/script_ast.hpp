// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// A mission script as a tree, for the Script tab's visual view: its globals, its scripts and their
// statements (if / then / else, loops, calls, assignments, comments), each with its line. The grammar is
// the checker's (mob_script_check.hpp); this parser keeps the structure instead of checking it, and stops
// at the first syntax error (what was read before it is kept, the error's line is told).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "mob_script_check.hpp"

namespace scriptast {

struct Expr {
    enum Kind { Call, Name, Number, String } kind = Name;
    std::string text;       // the name, number or string (without its quotes)
    int line = 0;
    std::vector<Expr> args; // Call
};

struct Stmt {
    enum Kind { If, Call, Assign, Loop, Comment } kind = Call;
    int line = 0;
    std::vector<Expr> conditions;         // If: all of them non-zero
    std::vector<Stmt> then, otherwise;    // If
    bool hasElse = false;
    Expr expr;                            // Call: the call; Assign: the value; Loop: For( var, group [, condition] )
    std::string target;                   // Assign: the variable; Comment: the text
    std::vector<Stmt> body;               // Loop
};

struct Typed { std::string name, type; };

struct Script {
    std::string name;    // "WorldScript" for the world script
    bool world = false;
    int line = 0;
    std::vector<Typed> params; // from its DeclareScript
    std::vector<Stmt> body;
};

struct Program {
    std::vector<Typed> globals;
    int globalsLine = 0;
    std::vector<Script> scripts;
    int errorLine = 0;   // 0: read to the end
    std::string error;
};

namespace detail {

struct Tok {
    enum Kind { End, Word, Number, String, Punct, Comment } kind;
    std::string text;
    int line;
};

inline std::vector<Tok> Lex(const std::string& src) {
    std::vector<Tok> out;
    size_t pos = 0;
    int line = 1;
    while (pos < src.size()) {
        const unsigned char c = static_cast<unsigned char>(src[pos]);
        if (c == '\n') { ++line; ++pos; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == 0 || c == 0x1A) { ++pos; continue; }
        if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
            const size_t start = pos + 2;
            while (pos < src.size() && src[pos] != '\n') ++pos;
            std::string text = src.substr(start, pos - start);
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
            out.push_back({Tok::Comment, text, line});
            continue;
        }
        if (c == '"') {
            const size_t end = src.find('"', pos + 1);
            const size_t stop = end == std::string::npos ? src.size() : end;
            std::string text = src.substr(pos + 1, stop - pos - 1);
            out.push_back({Tok::String, text, line});
            for (char ch : text) if (ch == '\n') ++line;
            pos = stop + 1;
            continue;
        }
        if (c == '(' || c == ')' || c == ',' || c == ':' || c == '=') {
            out.push_back({Tok::Punct, std::string(1, static_cast<char>(c)), line});
            ++pos;
            continue;
        }
        if (mobscript::IsWordChar(c)) {
            const size_t start = pos;
            while (pos < src.size() && mobscript::IsWordChar(static_cast<unsigned char>(src[pos]))) ++pos;
            std::string text = src.substr(start, pos - start);
            out.push_back({mobscript::IsNumberText(text) ? Tok::Number : Tok::Word, text, line});
            continue;
        }
        ++pos; // a stray character: the checker reports it
    }
    out.push_back({Tok::End, "", line});
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& src) : toks_(Lex(src)) {}

    Program Run() {
        while (!failed_) {
            SkipComments();
            const Tok& t = Peek();
            if (t.kind == Tok::End) break;
            const std::string w = mobscript::LowerCase(t.text);
            if (t.kind != Tok::Word) { Fail(t.line, "unexpected '" + t.text + "' at the top level"); break; }
            Next();
            if (w == "globalvars") { prog_.globalsLine = t.line; TypedList(prog_.globals); }
            else if (w == "declarescript") Declare();
            else if (w == "script" || w == "worldscript") ScriptBody(w == "worldscript", t.line);
            else Fail(t.line, "unexpected '" + t.text + "' at the top level");
        }
        for (Script& s : prog_.scripts) // the parameters come from the declarations
            for (const auto& d : declared_)
                if (!s.world && mobscript::LowerCase(d.first) == mobscript::LowerCase(s.name)) s.params = d.second;
        return std::move(prog_);
    }

private:
    std::vector<Tok> toks_;
    size_t at_ = 0;
    bool failed_ = false;
    int depth_ = 0;
    Program prog_;
    std::vector<std::pair<std::string, std::vector<Typed>>> declared_;

    const Tok& Peek(size_t ahead = 0) {
        size_t i = at_;
        for (;;) { // comments are skipped where they are not statements
            while (i < toks_.size() - 1 && toks_[i].kind == Tok::Comment) ++i;
            if (ahead == 0) return toks_[i];
            --ahead;
            ++i;
            if (i >= toks_.size()) return toks_.back();
        }
    }
    const Tok& Next() {
        while (at_ < toks_.size() - 1 && toks_[at_].kind == Tok::Comment) ++at_;
        const Tok& t = toks_[at_];
        if (at_ < toks_.size() - 1) ++at_;
        return t;
    }
    void SkipComments() { while (at_ < toks_.size() - 1 && toks_[at_].kind == Tok::Comment) ++at_; }
    bool IsPunct(const Tok& t, char c) { return t.kind == Tok::Punct && t.text[0] == c; }
    void Fail(int line, const std::string& what) {
        if (failed_) return;
        failed_ = true;
        prog_.errorLine = line;
        prog_.error = what;
    }
    bool Expect(char c, const char* where) {
        const Tok& t = Peek();
        if (IsPunct(t, c)) { Next(); return true; }
        Fail(t.line, std::string("expected '") + c + "' " + where);
        return false;
    }

    void TypedList(std::vector<Typed>& out) {
        if (!Expect('(', "after the name")) return;
        while (!failed_ && !IsPunct(Peek(), ')') && Peek().kind != Tok::End) {
            const Tok& name = Next();
            if (!Expect(':', "after a name")) return;
            const Tok& type = Next();
            out.push_back({name.text, type.text});
            if (IsPunct(Peek(), ',')) Next();
        }
        Expect(')', "to close the list");
    }

    void Declare() {
        const Tok& name = Next();
        std::vector<Typed> params;
        TypedList(params);
        declared_.push_back({name.text, params});
    }

    void ScriptBody(bool world, int line) {
        Script s;
        s.world = world;
        s.line = line;
        s.name = "WorldScript";
        if (!world) s.name = Next().text;
        if (!Expect('(', "to start the script")) return;
        Statements(s.body);
        Expect(')', "to close the script");
        prog_.scripts.push_back(std::move(s));
    }

    void Statements(std::vector<Stmt>& out) {
        if (++depth_ > 100) { Fail(Peek().line, "nested too deep"); return; }
        while (!failed_) {
            // comments between statements are kept
            while (at_ < toks_.size() - 1 && toks_[at_].kind == Tok::Comment) {
                Stmt c;
                c.kind = Stmt::Comment;
                c.line = toks_[at_].line;
                c.target = toks_[at_].text;
                out.push_back(std::move(c));
                ++at_;
            }
            const Tok& t = Peek();
            if (IsPunct(t, ')') || t.kind == Tok::End) break;
            out.push_back(Statement());
        }
        --depth_;
    }

    void Block(std::vector<Stmt>& out, const char* where) {
        if (!Expect('(', where)) return;
        Statements(out);
        Expect(')', "to close the block");
    }

    Stmt Statement() {
        Stmt s;
        const Tok& first = Peek();
        s.line = first.line;
        const std::string w = mobscript::LowerCase(first.text);
        if (first.kind == Tok::Word && w == "if") {
            Next();
            s.kind = Stmt::If;
            if (!Expect('(', "after 'if'")) return s;
            while (!failed_ && !IsPunct(Peek(), ')') && Peek().kind != Tok::End) s.conditions.push_back(Expression());
            if (!Expect(')', "to close the conditions")) return s;
            const Tok& then = Next();
            if (mobscript::LowerCase(then.text) != "then") { Fail(then.line, "expected 'then'"); return s; }
            Block(s.then, "after 'then'");
            if (!failed_ && Peek().kind == Tok::Word && mobscript::LowerCase(Peek().text) == "else") {
                Next();
                s.hasElse = true;
                Block(s.otherwise, "after 'else'");
            }
            return s;
        }
        if (first.kind == Tok::Word && IsPunct(Peek(1), '=')) {
            s.kind = Stmt::Assign;
            s.target = Next().text;
            Next(); // '='
            s.expr = Expression();
            return s;
        }
        if (first.kind == Tok::Word && IsPunct(Peek(1), '(')) {
            s.expr = Expression();
            if ((w == "for" || w == "forif") && IsPunct(Peek(), '(')) {
                s.kind = Stmt::Loop;
                Block(s.body, "to start the loop");
            }
            return s;
        }
        Next();
        Fail(first.line, "unexpected '" + first.text + "' where a statement was expected");
        return s;
    }

    Expr Expression() {
        Expr e;
        if (++depth_ > 100) { Fail(Peek().line, "nested too deep"); return e; }
        const Tok& t = Next();
        e.line = t.line;
        e.text = t.text;
        if (t.kind == Tok::Number) e.kind = Expr::Number;
        else if (t.kind == Tok::String) e.kind = Expr::String;
        else if (t.kind == Tok::Word && IsPunct(Peek(), '(')) {
            e.kind = Expr::Call;
            Next();
            while (!failed_ && !IsPunct(Peek(), ')') && Peek().kind != Tok::End) {
                e.args.push_back(Expression());
                if (IsPunct(Peek(), ',')) Next();
                else if (!IsPunct(Peek(), ')')) { Fail(Peek().line, "expected ',' or ')' in the arguments of " + e.text + "()"); break; }
            }
            Expect(')', "to close the arguments");
        } else if (t.kind == Tok::Word) e.kind = Expr::Name;
        else Fail(t.line, "expected a value but found '" + t.text + "'");
        --depth_;
        return e;
    }
};

} // namespace detail

inline Program Parse(const std::string& text) { return detail::Parser(text).Run(); }

} // namespace scriptast
