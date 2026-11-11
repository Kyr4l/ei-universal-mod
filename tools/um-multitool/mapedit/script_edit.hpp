// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Editing a mission script as a tree: the script text written back from script_ast.hpp's Program (Write), and the
// operations the node editor does on it (set a value, delete, duplicate, mute, insert a command, move). Nothing here knows
// about the GUI: an operation changes the Program, the caller writes it and commits the text as one undo step.
//
// Addressing: a statement is a path from a script down through blocks (a script's body, an If's then / else, a loop's
// body), each step a (branch, index); an expression is one of its statement's root expressions plus the argument indices
// down a call tree. A path stays valid until the tree changes; every operation says which paths it moves.
//
// The text written is the Program only: comments between statements are kept (they are statements), comments outside
// scripts and the original layout are not. The editor owns the layout, like Blender owns a node tree's.
#pragma once

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "script_ast.hpp"

namespace scriptedit {

using scriptast::Expr;
using scriptast::Program;
using scriptast::Script;
using scriptast::Stmt;

// ---- writing --------------------------------------------------------------------------------------------------------

inline std::string WriteExpr(const Expr& e) {
    switch (e.kind) {
    case Expr::Number:
    case Expr::Name: return e.text;
    case Expr::String: return "\"" + e.text + "\"";
    case Expr::Call: {
        std::string s = e.text + "(";
        if (e.args.empty()) return s + "  )";
        s += " ";
        for (size_t i = 0; i < e.args.size(); ++i) s += (i ? ", " : "") + WriteExpr(e.args[i]);
        return s + " )";
    }
    }
    return e.text;
}

inline void WriteStmt(const Stmt& s, int indent, std::ostringstream& out) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    auto block = [&](const std::vector<Stmt>& v) {
        out << pad << "(\n";
        for (const Stmt& c : v) WriteStmt(c, indent + 1, out);
        out << pad << ")\n";
    };
    switch (s.kind) {
    case Stmt::Comment: out << pad << "//" << s.target << "\n"; break;
    case Stmt::Assign: out << pad << s.target << " = " << WriteExpr(s.expr) << "\n"; break;
    case Stmt::Call: out << pad << WriteExpr(s.expr) << "\n"; break;
    case Stmt::Loop:
        out << pad << WriteExpr(s.expr) << "\n";
        block(s.body);
        break;
    case Stmt::If:
        out << pad << "if\n" << pad << "(\n";
        for (const Expr& c : s.conditions) out << pad << "  " << WriteExpr(c) << "\n";
        out << pad << ")\n" << pad << "then\n";
        block(s.then);
        if (s.hasElse) { out << pad << "else\n"; block(s.otherwise); }
        break;
    }
}

inline std::string WriteStmt(const Stmt& s, int indent = 0) {
    std::ostringstream out;
    WriteStmt(s, indent, out);
    return out.str();
}

// The whole script: GlobalVars, a DeclareScript for each script that has a body (the game wants the declarations first), the scripts.
inline std::string Write(const Program& p) {
    std::ostringstream out;
    auto typed = [&](const std::vector<scriptast::Typed>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : " ") + v[i].name + " : " + v[i].type;
        return s;
    };
    if (!p.globals.empty()) {
        out << "GlobalVars (\n";
        for (size_t i = 0; i < p.globals.size(); ++i) out << "  " << p.globals[i].name << " : " << p.globals[i].type << (i + 1 < p.globals.size() ? "," : "") << "\n";
        out << ")\n\n";
    }
    bool declared = false;
    for (const Script& s : p.scripts) {
        if (s.world) continue;
        out << "DeclareScript " << s.name << " (" << typed(s.params) << (s.params.empty() ? " " : " ") << ")\n";
        declared = true;
    }
    if (declared) out << "\n";
    for (const Script& s : p.scripts) {
        if (s.world) out << "WorldScript\n(\n";
        else out << "Script " << s.name << "\n(\n";
        for (const Stmt& c : s.body) WriteStmt(c, 1, out);
        out << ")\n\n";
    }
    return out.str();
}

// ---- addressing -----------------------------------------------------------------------------------------------------

// Branch numbers: 0 = a script's body / an If's then / a loop's body; 1 = an If's else.
struct Step { int branch = 0, index = 0; };
struct StmtPath {
    int script = -1;
    std::vector<Step> steps; // the last step is the statement; empty: the script itself
    bool operator==(const StmtPath& o) const {
        if (script != o.script || steps.size() != o.steps.size()) return false;
        for (size_t i = 0; i < steps.size(); ++i) if (steps[i].branch != o.steps[i].branch || steps[i].index != o.steps[i].index) return false;
        return true;
    }
};
struct ExprPath {
    StmtPath stmt;
    int slot = 0;           // which root expression: a call / assignment / loop has one (slot 0), an If one per condition
    std::vector<int> args;  // down through the call's arguments
};

// The statements list a path's last step lives in (the script's body, or the block of the statement above), or null.
inline std::vector<Stmt>* ChainOf(Program& p, const StmtPath& path, size_t upTo, int branch) {
    if (path.script < 0 || path.script >= static_cast<int>(p.scripts.size())) return nullptr;
    std::vector<Stmt>* v = &p.scripts[static_cast<size_t>(path.script)].body;
    for (size_t i = 0; i < upTo; ++i) {
        const Step& st = path.steps[i];
        if (st.index < 0 || st.index >= static_cast<int>(v->size())) return nullptr;
        Stmt& s = (*v)[static_cast<size_t>(st.index)];
        const int b = i + 1 < upTo ? path.steps[i + 1].branch : branch;
        if (s.kind == Stmt::If) v = b == 1 ? &s.otherwise : &s.then;
        else if (s.kind == Stmt::Loop) v = &s.body;
        else return nullptr;
    }
    return v;
}

inline Stmt* GetStmt(Program& p, const StmtPath& path) {
    if (path.steps.empty()) return nullptr;
    // the chain holding the last step: the branch used to get into it is the last step's own branch
    std::vector<Stmt>* v = ChainOf(p, path, path.steps.size() - 1, path.steps.back().branch);
    const int i = path.steps.back().index;
    return v && i >= 0 && i < static_cast<int>(v->size()) ? &(*v)[static_cast<size_t>(i)] : nullptr;
}

inline Expr* RootExpr(Stmt& s, int slot) {
    switch (s.kind) {
    case Stmt::If: return slot >= 0 && slot < static_cast<int>(s.conditions.size()) ? &s.conditions[static_cast<size_t>(slot)] : nullptr;
    case Stmt::Call:
    case Stmt::Assign:
    case Stmt::Loop: return slot == 0 ? &s.expr : nullptr;
    default: return nullptr;
    }
}

inline Expr* GetExpr(Program& p, const ExprPath& path) {
    Stmt* s = GetStmt(p, path.stmt);
    Expr* e = s ? RootExpr(*s, path.slot) : nullptr;
    for (size_t i = 0; e && i < path.args.size(); ++i) {
        const int a = path.args[i];
        e = e->kind == Expr::Call && a >= 0 && a < static_cast<int>(e->args.size()) ? &e->args[static_cast<size_t>(a)] : nullptr;
    }
    return e;
}

// ---- operations -----------------------------------------------------------------------------------------------------

// A literal typed in a socket: quoted text is a string, a number is a number, anything else a name (a variable).
inline Expr Literal(const std::string& typed) {
    Expr e;
    std::string t = typed;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') { e.kind = Expr::String; e.text = t.substr(1, t.size() - 2); }
    else if (mobscript::IsNumberText(t)) { e.kind = Expr::Number; e.text = t; }
    else { e.kind = Expr::Name; e.text = t; }
    return e;
}

// Puts a literal in an argument / condition. False when the address is not there, the text is empty, or the place holds a call
// (a wired value: unwire it by deleting its node).
inline bool SetValue(Program& p, const ExprPath& path, const std::string& typed) {
    Expr* e = GetExpr(p, path);
    if (!e || e->kind == Expr::Call) return false;
    Expr lit = Literal(typed);
    if (lit.text.empty() && lit.kind != Expr::String) return false;
    lit.line = e->line;
    *e = lit;
    return true;
}

// Replaces an argument by a call (a node wired in), or a call by a literal.
inline bool SetExpr(Program& p, const ExprPath& path, const Expr& value) {
    Expr* e = GetExpr(p, path);
    if (!e) return false;
    *e = value;
    return true;
}

inline bool DeleteStmt(Program& p, const StmtPath& path) {
    if (path.steps.empty()) return false;
    std::vector<Stmt>* v = ChainOf(p, path, path.steps.size() - 1, path.steps.back().branch);
    const int i = path.steps.back().index;
    if (!v || i < 0 || i >= static_cast<int>(v->size())) return false;
    v->erase(v->begin() + i);
    return true; // the statements below move up by one
}

// A copy right after the statement; its path (the original's index + 1).
inline bool DuplicateStmt(Program& p, const StmtPath& path, StmtPath* copy = nullptr) {
    if (path.steps.empty()) return false;
    std::vector<Stmt>* v = ChainOf(p, path, path.steps.size() - 1, path.steps.back().branch);
    const int i = path.steps.back().index;
    if (!v || i < 0 || i >= static_cast<int>(v->size())) return false;
    Stmt c = (*v)[static_cast<size_t>(i)];
    v->insert(v->begin() + i + 1, std::move(c));
    if (copy) { *copy = path; copy->steps.back().index = i + 1; }
    return true;
}

// The chain a container's block holds: `container` is the If / loop whose block it is (steps empty: the script's own body),
// `branch` picks then / else.
inline std::vector<Stmt>* BlockOf(Program& p, const StmtPath& container, int branch) {
    if (container.steps.empty()) {
        if (container.script < 0 || container.script >= static_cast<int>(p.scripts.size())) return nullptr;
        return &p.scripts[static_cast<size_t>(container.script)].body;
    }
    Stmt* parent = GetStmt(p, container);
    if (!parent) return nullptr;
    if (parent->kind == Stmt::If) { if (branch == 1) parent->hasElse = true; return branch == 1 ? &parent->otherwise : &parent->then; }
    if (parent->kind == Stmt::Loop) return &parent->body;
    return nullptr;
}

// Inserts a statement into a block at `index` (clamped); the new statement's path.
inline bool InsertStmt(Program& p, const StmtPath& container, int branch, int index, Stmt s, StmtPath* made = nullptr) {
    std::vector<Stmt>* v = BlockOf(p, container, branch);
    if (!v) return false;
    index = std::max(0, std::min(index, static_cast<int>(v->size())));
    v->insert(v->begin() + index, std::move(s));
    if (made) { *made = container; made->steps.push_back({container.steps.empty() ? 0 : branch, index}); }
    return true;
}

// Inserts right after a statement (the node a new one is added from).
inline bool InsertAfter(Program& p, const StmtPath& after, Stmt s, StmtPath* made = nullptr) {
    if (after.steps.empty()) return false;
    StmtPath container = after;
    const Step last = container.steps.back();
    container.steps.pop_back();
    return InsertStmt(p, container, last.branch, last.index + 1, std::move(s), made);
}

// A call of a known function with every argument at a harmless default (0, "", NULL), as a statement (a command) or a value.
inline Expr NewCallExpr(const std::string& name) {
    Expr e;
    e.kind = Expr::Call;
    e.text = name;
    auto fn = mobscript::FunctionTable().find(mobscript::LowerCase(name));
    if (fn == mobscript::FunctionTable().end()) return e;
    e.text = fn->second->name;
    const char* params = fn->second->params;
    if (params[0] == '*') return e; // any number of arguments: none to start with
    for (const char* c = params; *c; ++c) {
        Expr a;
        if (*c == 'f') { a.kind = Expr::Number; a.text = "0"; }
        else if (*c == 's') { a.kind = Expr::String; }
        else { a.kind = Expr::Name; a.text = "NULL"; }
        e.args.push_back(a);
    }
    return e;
}
inline Stmt NewCall(const std::string& name) {
    Stmt s;
    s.kind = Stmt::Call;
    s.expr = NewCallExpr(name);
    return s;
}
inline Stmt NewIf() {
    Stmt s;
    s.kind = Stmt::If;
    Expr c;
    c.kind = Expr::Number;
    c.text = "1";
    s.conditions.push_back(c);
    return s;
}
inline Stmt NewAssign(const std::string& variable) {
    Stmt s;
    s.kind = Stmt::Assign;
    s.target = variable;
    s.expr.kind = Expr::Number;
    s.expr.text = "0";
    return s;
}
inline Stmt NewComment(const std::string& text) {
    Stmt s;
    s.kind = Stmt::Comment;
    s.target = " " + text;
    return s;
}

// Muting turns a command or an assignment into a comment holding its text (Blender's M); unmuting parses it back.
inline bool Mute(Program& p, const StmtPath& path) {
    Stmt* s = GetStmt(p, path);
    if (!s || (s->kind != Stmt::Call && s->kind != Stmt::Assign)) return false;
    std::string text = WriteStmt(*s, 0);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    Stmt c;
    c.kind = Stmt::Comment;
    c.line = s->line;
    c.target = " " + text;
    *s = c;
    return true;
}
inline bool Unmute(Program& p, const StmtPath& path) {
    Stmt* s = GetStmt(p, path);
    if (!s || s->kind != Stmt::Comment) return false;
    const Program q = scriptast::Parse("WorldScript\n(\n" + s->target + "\n)\n");
    if (q.errorLine || q.scripts.size() != 1 || q.scripts[0].body.size() != 1) return false;
    const Stmt& t = q.scripts[0].body[0];
    if (t.kind != Stmt::Call && t.kind != Stmt::Assign) return false;
    const int line = s->line;
    *s = t;
    s->line = line;
    return true;
}
inline bool IsMutedText(const std::string& commentText) {
    if (commentText.find('(') == std::string::npos && commentText.find('=') == std::string::npos) return false; // cheap: most comments are words
    const Program q = scriptast::Parse("WorldScript\n(\n" + commentText + "\n)\n");
    return !q.errorLine && q.scripts.size() == 1 && q.scripts[0].body.size() == 1 &&
           (q.scripts[0].body[0].kind == Stmt::Call || q.scripts[0].body[0].kind == Stmt::Assign);
}
inline bool IsMuted(Program& p, const StmtPath& path) {
    Stmt* s = GetStmt(p, path);
    return s && s->kind == Stmt::Comment && IsMutedText(s->target);
}

// Moves a statement into a block at `index` (the index counts the chain as it is before the move). False when the target is inside
// the statement itself. The path of the statement after the move.
inline bool MoveStmt(Program& p, const StmtPath& from, const StmtPath& container, int branch, int index, StmtPath* now = nullptr) {
    if (from.steps.empty()) return false;
    // not into itself or its own blocks
    if (container.script == from.script && container.steps.size() >= from.steps.size()) {
        bool inside = true;
        for (size_t i = 0; i < from.steps.size() && inside; ++i)
            inside = container.steps[i].index == from.steps[i].index && (i == 0 || container.steps[i].branch == from.steps[i].branch);
        if (inside) return false;
    }
    Stmt* s = GetStmt(p, from);
    if (!s) return false;
    Stmt copy = *s;
    // a container below the removed statement in the same chain shifts up by one: find it before removing
    StmtPath target = container;
    const size_t depth = from.steps.size() - 1; // the step of `from` in the chain it leaves
    bool sameChain = container.script == from.script && container.steps.size() > depth;
    for (size_t i = 0; sameChain && i < depth; ++i) sameChain = container.steps[i].index == from.steps[i].index && container.steps[i].branch == from.steps[i].branch;
    if (sameChain && container.steps[depth].branch == from.steps[depth].branch && container.steps[depth].index > from.steps[depth].index) --target.steps[depth].index;
    const bool sameBlock = container.script == from.script && container.steps.size() == depth && [&] {
        for (size_t i = 0; i < depth; ++i) if (container.steps[i].index != from.steps[i].index || (i > 0 && container.steps[i].branch != from.steps[i].branch)) return false;
        return branch == from.steps[depth].branch || (depth == 0 && branch == 0);
    }();
    if (!DeleteStmt(p, from)) return false;
    if (sameBlock && from.steps.back().index < index) --index;
    return InsertStmt(p, target, branch, index, std::move(copy), now);
}

// One place up (-1) or down (+1) in its own block.
inline bool MoveBy(Program& p, const StmtPath& path, int delta, StmtPath* now = nullptr) {
    if (path.steps.empty()) return false;
    StmtPath container = path;
    const Step last = container.steps.back();
    container.steps.pop_back();
    std::vector<Stmt>* v = BlockOf(p, container, last.branch);
    if (!v) return false;
    const int to = last.index + delta;
    if (to < 0 || to >= static_cast<int>(v->size())) return false;
    std::swap((*v)[static_cast<size_t>(last.index)], (*v)[static_cast<size_t>(to)]);
    if (now) { *now = path; now->steps.back().index = to; }
    return true;
}

} // namespace scriptedit
