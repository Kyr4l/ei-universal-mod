// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// g++ -std=c++17 -I. -I.. -I../../../resources/universal-mod/um-dll tests/script_edit_test.cpp -o /tmp/script_edit_test && /tmp/script_edit_test [folder with .mob files ...]
// With folders: every .mob script is read, written back, read again: the tree must be the same and the checker must report the same errors.
#include <cstdio>
#include <filesystem>

#include "mapedit/mob_file.hpp"
#include "mapedit/script_edit.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace scriptedit;

static bool SameExpr(const Expr& a, const Expr& b) {
    if (a.kind != b.kind || a.text != b.text || a.args.size() != b.args.size()) return false;
    for (size_t i = 0; i < a.args.size(); ++i) if (!SameExpr(a.args[i], b.args[i])) return false;
    return true;
}
static bool SameChain(const std::vector<Stmt>& a, const std::vector<Stmt>& b);
static bool SameStmt(const Stmt& a, const Stmt& b) {
    if (a.kind != b.kind || a.target != b.target || a.hasElse != b.hasElse || a.conditions.size() != b.conditions.size()) return false;
    for (size_t i = 0; i < a.conditions.size(); ++i) if (!SameExpr(a.conditions[i], b.conditions[i])) return false;
    return SameExpr(a.expr, b.expr) && SameChain(a.then, b.then) && SameChain(a.otherwise, b.otherwise) && SameChain(a.body, b.body);
}
static bool SameChain(const std::vector<Stmt>& a, const std::vector<Stmt>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!SameStmt(a[i], b[i])) return false;
    return true;
}
static bool SameProgram(const Program& a, const Program& b) {
    if (a.globals.size() != b.globals.size() || a.scripts.size() != b.scripts.size()) return false;
    for (size_t i = 0; i < a.globals.size(); ++i) if (a.globals[i].name != b.globals[i].name || a.globals[i].type != b.globals[i].type) return false;
    for (size_t i = 0; i < a.scripts.size(); ++i) {
        const scriptast::Script &x = a.scripts[i], &y = b.scripts[i];
        if (x.name != y.name || x.world != y.world || x.params.size() != y.params.size() || !SameChain(x.body, y.body)) return false;
        for (size_t k = 0; k < x.params.size(); ++k) if (x.params[k].name != y.params[k].name || x.params[k].type != y.params[k].type) return false;
    }
    return true;
}

static StmtPath P(int script, std::initializer_list<Step> steps) { StmtPath p; p.script = script; p.steps = steps; return p; }

static const char* kScript =
    "GlobalVars (\n  NULL : object,\n  Heroes : group\n)\n\n"
    "DeclareScript Door (  this : object )\n\n"
    "Script Door\n(\n  // opens the door\n  if\n  (\n    IsEqual( GetLeverState( GetObject( 12 ) ) , 1 )\n  )\n  then\n  (\n    Sleep( 2 )\n    Attack( NULL, NULL )\n  )\n  else\n  (\n    Sleep( 5 )\n  )\n  Sleep( 1 )\n)\n\n"
    "WorldScript\n(\n  Sleep( 3 )\n  Door( NULL )\n)\n";

int main(int argc, char** argv) {
    const Program p0 = scriptast::Parse(kScript);
    CHECK(p0.errorLine == 0 && p0.scripts.size() == 2 && p0.globals.size() == 2);
    // writing and reading back gives the same tree, and writing that again the same text
    const std::string w0 = Write(p0);
    const Program p1 = scriptast::Parse(w0);
    CHECK(p1.errorLine == 0 && SameProgram(p0, p1));
    CHECK(Write(p1) == w0);

    // set a value: a number, a string, a name; a wired call is refused
    Program p = p0;
    ExprPath sleepArg{P(0, {{0, 2}}), 0, {0}}; // not the if: the 3rd statement is "Sleep( 1 )"
    CHECK(GetExpr(p, sleepArg) && GetExpr(p, sleepArg)->text == "1");
    CHECK(SetValue(p, sleepArg, "7.5") && GetExpr(p, sleepArg)->kind == Expr::Number && GetExpr(p, sleepArg)->text == "7.5");
    CHECK(SetValue(p, sleepArg, "\"x\"") && GetExpr(p, sleepArg)->kind == Expr::String && GetExpr(p, sleepArg)->text == "x");
    CHECK(SetValue(p, sleepArg, "myvar") && GetExpr(p, sleepArg)->kind == Expr::Name);
    ExprPath cond{P(0, {{0, 1}}), 0, {}}; // the If's condition is a call
    CHECK(GetExpr(p, cond) && GetExpr(p, cond)->kind == Expr::Call && !SetValue(p, cond, "3"));
    ExprPath objArg{P(0, {{0, 1}}), 0, {0, 0, 0}}; // IsEqual > GetLeverState > GetObject( 12 ) > 12
    CHECK(GetExpr(p, objArg) && GetExpr(p, objArg)->text == "12");

    // delete and duplicate
    p = p0;
    CHECK(DuplicateStmt(p, P(0, {{0, 2}})));
    CHECK(p.scripts[0].body.size() == 4);
    CHECK(DeleteStmt(p, P(0, {{0, 2}})) && DeleteStmt(p, P(0, {{0, 2}})));
    CHECK(p.scripts[0].body.size() == 2);
    CHECK(DeleteStmt(p, P(0, {{0, 1}, {0, 0}})) && p.scripts[0].body[1].then.size() == 1);
    CHECK(!DeleteStmt(p, P(0, {{0, 1}, {0, 9}})) && !DeleteStmt(p, P(5, {{0, 0}})));

    // insert: after a statement, into an else block, into a script's start; a new command has defaults of its types
    p = p0;
    StmtPath made;
    CHECK(InsertAfter(p, P(0, {{0, 1}, {0, 0}}), NewCall("AddUnitToServer"), &made));
    CHECK(made.steps.size() == 2 && made.steps[1].index == 1 && made.steps[0].index == 1);
    Stmt* ins = GetStmt(p, made);
    CHECK(ins && ins->expr.text == "AddUnitToServer" && ins->expr.args.size() == 6 && ins->expr.args[0].kind == Expr::String && ins->expr.args[3].text == "0");
    CHECK(InsertStmt(p, P(0, {{0, 1}}), 1, 0, NewCall("Sleep"), &made) && made.steps.back().branch == 1 && p.scripts[0].body[1].otherwise.size() == 2);
    CHECK(InsertStmt(p, P(1, {}), 0, 0, NewComment("first"), &made) && p.scripts[1].body[0].kind == Stmt::Comment);
    CHECK(NewCall("NoSuchFunction").expr.args.empty());
    const Program reread = scriptast::Parse(Write(p));
    CHECK(reread.errorLine == 0 && SameProgram(p, reread));

    // mute / unmute
    p = p0;
    CHECK(Mute(p, P(0, {{0, 2}})) && p.scripts[0].body[2].kind == Stmt::Comment && IsMuted(p, P(0, {{0, 2}})));
    CHECK(Write(p).find("// Sleep( 1 )") != std::string::npos);
    CHECK(!IsMuted(p, P(0, {{0, 0}}))); // an ordinary comment
    CHECK(!Unmute(p, P(0, {{0, 0}})));
    CHECK(Unmute(p, P(0, {{0, 2}})) && SameProgram(p, p0));
    CHECK(!Mute(p, P(0, {{0, 1}}))); // an If cannot be muted

    // move: one place, into a block, out of a block
    p = p0;
    StmtPath now;
    CHECK(MoveBy(p, P(0, {{0, 2}}), -1, &now) && now.steps[0].index == 1 && p.scripts[0].body[1].kind == Stmt::Call);
    CHECK(!MoveBy(p, P(0, {{0, 0}}), -1));
    p = p0;
    CHECK(MoveStmt(p, P(0, {{0, 2}}), P(0, {{0, 1}}), 1, 0, &now)); // "Sleep( 1 )" into the else, first
    CHECK(p.scripts[0].body.size() == 2 && p.scripts[0].body[1].otherwise.size() == 2 && p.scripts[0].body[1].otherwise[0].expr.args[0].text == "1");
    CHECK(MoveStmt(p, P(0, {{0, 1}, {1, 0}}), P(0, {}), 0, 0, &now)); // and out to the top
    CHECK(p.scripts[0].body[0].kind == Stmt::Call && p.scripts[0].body.size() == 3);
    p = p0;
    CHECK(!MoveStmt(p, P(0, {{0, 1}}), P(0, {{0, 1}}), 0, 0)); // an If into itself
    CHECK(SameProgram(p, p0));
    p = p0;
    CHECK(MoveStmt(p, P(0, {{0, 0}}), P(0, {}), 0, 3)); // the comment to the end
    CHECK(p.scripts[0].body.back().kind == Stmt::Comment && p.scripts[0].body.size() == 3);

    // real scripts: written back and read again
    int files = 0, scripts = 0, differs = 0, checkDiffers = 0;
    for (int a = 1; a < argc; ++a) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(argv[a], ec)) {
            if (e.path().extension() != ".mob") continue;
            mob::File f;
            if (!mob::Load(e.path().string(), f) || !f.hasScript) continue;
            const Program orig = scriptast::Parse(f.script);
            if (orig.errorLine) continue; // a script the reader cannot read is not written
            ++files;
            scripts += static_cast<int>(orig.scripts.size());
            const std::string text = Write(orig);
            const Program back = scriptast::Parse(text);
            if (back.errorLine || !SameProgram(orig, back) || Write(back) != text) { ++differs; std::printf("round trip differs: %s (line %d %s)\n", e.path().filename().string().c_str(), back.errorLine, back.error.c_str()); continue; }
            const MobScriptReport r1 = CheckMobScript(f.script), r2 = CheckMobScript(text);
            if (r1.errors != r2.errors) { ++checkDiffers; std::printf("checker errors %d -> %d: %s\n", r1.errors, r2.errors, e.path().filename().string().c_str()); }
        }
    }
    if (argc > 1) {
        std::printf("%d script files, %d scripts: %d differ, %d with other checker errors\n", files, scripts, differs, checkDiffers);
        CHECK(files > 0 && differs == 0 && checkDiffers == 0);
    }
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail;
}
