// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: editing in the Script tab's node view, like Blender's node editor. Included by map_script.cpp after the graph types
// (nodegraph::Node / Graph) and CommitScript; not for anything else.
//
// What it does (script_edit.hpp does the tree work, this file the mouse and the keys):
//   - click selects a node (Shift adds, a drag on the empty canvas draws a box); the selected ones have an orange outline, the active one a white one
//   - drag a statement onto another node to move it there (the upper half: before it, the lower half: after it, a control's branch rows: into that
//     branch, a script's header: first in the script); the drop place shows as a line
//   - X / Delete deletes, M mutes (turns a command into a comment, again: back), Shift D duplicates, Shift A or a right click adds a node
//   - double-click a literal in a node to type over it; right click on a row: wire a function in, or unwire
//   - the middle button (or Alt + the left button) pans, the wheel zooms: the canvas keeps its own keys
//   - G moves the selected nodes freely (a click places them, Esc / a right click puts them back), Alt G returns them to their laid-out
//     place: only the picture changes, the script does not; the places are kept per map file in um-multitool-nodes.tsv beside the settings
//   - drag from a function's output (its right dot) onto a value row of another node: the call moves there (its old place gets a plain
//     value); drag a wired row's left dot away and let go on the empty canvas: disconnected
// Every change is written as script text and committed as one undo step (the map editor's Ctrl+Z).
#pragma once

namespace nodeedit {

using namespace nodegraph;
using scriptedit::ExprPath;
using scriptedit::Step;
using scriptedit::StmtPath;

struct Sel { bool expr = false; StmtPath s; ExprPath e; };

inline std::string StmtKey(const StmtPath& p) {
    std::ostringstream k;
    k << 'S' << p.script;
    for (const Step& st : p.steps) k << '/' << st.branch << '.' << st.index;
    return k.str();
}
inline std::string ExprKey(const ExprPath& p) {
    std::ostringstream k;
    k << 'E' << StmtKey(p.stmt).substr(1) << '#' << p.slot;
    for (int a : p.args) k << '.' << a;
    return k.str();
}
inline std::string Key(const Node& n) { return n.isStmt ? StmtKey(n.stmt) : n.isExpr ? ExprKey(n.expr) : std::string(); }
inline std::string Key(const Sel& s) { return s.expr ? ExprKey(s.e) : StmtKey(s.s); }

struct State {
    bool on = true;
    std::string file;
    std::vector<Sel> items; // the selection; the last one is the active node
    // typing over a value
    bool editOpen = false, editFocus = false;
    ExprPath editPath;
    char buf[512] = "";
    ImVec2 editPos{0, 0};
    float editWidth = 160;
    bool handledDouble = false; // a double-click this frame was taken by the value editor
    // pressing a node: a drag moves it
    int pressNode = -1;
    ImVec2 pressPos{0, 0};
    bool dragging = false;
    // a box on the empty canvas
    bool boxing = false;
    ImVec2 boxA{0, 0};
    // menus
    bool addOpen = false, ctxOpen = false, searchFocus = false, wiring = false;
    int ctxNode = -1, ctxRow = -1;
    ImVec2 menuCanvas{0, 0}; // where a menu was opened, in canvas units (the script frame an added node goes to)
    ExprPath wireRow;
    char search[64] = "";
    int where = 0; // add: 0 after the selected node, 1 into its first branch (then / body), 2 into its second (else)
    std::string message;
    // G: moving the selected nodes by hand
    bool grabbing = false;
    ImVec2 grabStart{0, 0};
    std::map<std::string, ImVec2> grabBase; // their offsets when G was pressed (Esc puts these back)
    // dragging a wire: from a function's output, or picked up at a row's input
    bool wiringDrag = false, pickedUp = false;
    ExprPath wireFrom;
    ImVec2 wireAnchor{0, 0}; // the output socket, in canvas units
    // a function picked in the add menu: it follows the mouse until it is dropped on a value row
    std::string placing;
};

// ---- nodes moved by hand: offsets from the laid-out place, per map file -----------------------------------------------------

inline std::map<std::string, std::map<std::string, ImVec2>>& AllOffsets() {
    static std::map<std::string, std::map<std::string, ImVec2>> all;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream in(config::UserDir() + "/um-multitool-nodes.tsv");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '\t')) f.push_back(part);
            if (f.size() == 4) all[f[0]][f[1]] = ImVec2(static_cast<float>(std::atof(f[2].c_str())), static_cast<float>(std::atof(f[3].c_str())));
        }
    }
    return all;
}
inline std::map<std::string, ImVec2>& OffsetsOf(const std::string& file) { return AllOffsets()[file]; }
inline void SaveOffsets() {
    std::ofstream out(config::UserDir() + "/um-multitool-nodes.tsv", std::ios::trunc);
    out << "# um-multitool: script nodes moved by hand (Map Editor > Script > Nodes, G): map file, node, offset x, offset y\n";
    for (const auto& file : AllOffsets())
        for (const auto& kv : file.second)
            if (kv.second.x != 0 || kv.second.y != 0) out << file.first << '\t' << kv.first << '\t' << kv.second.x << '\t' << kv.second.y << '\n';
}
inline std::string OffsetSig(const std::map<std::string, ImVec2>& off) {
    std::ostringstream s;
    for (const auto& kv : off) s << kv.first << ':' << kv.second.x << ',' << kv.second.y << ';';
    return s.str();
}
// The laid-out graph with the moved nodes: their wires follow, their frames grow around them.
inline void ApplyOffsets(Graph& g, const std::map<std::string, ImVec2>& off, const std::unordered_map<std::string, int>& byKey, float F, float headerH) {
    std::vector<ImVec2> d(g.nodes.size(), ImVec2(0, 0));
    bool any = false;
    for (const auto& kv : off) {
        auto it = byKey.find(kv.first);
        if (it == byKey.end() || it->second < 0 || static_cast<size_t>(it->second) >= g.nodes.size()) continue;
        d[static_cast<size_t>(it->second)] = kv.second;
        any = any || kv.second.x != 0 || kv.second.y != 0;
    }
    if (!any) return;
    for (size_t i = 0; i < g.nodes.size(); ++i) { g.nodes[i].pos.x += d[i].x; g.nodes[i].pos.y += d[i].y; }
    for (Wire& w : g.wires) {
        if (w.from >= 0 && static_cast<size_t>(w.from) < d.size()) { w.a.x += d[static_cast<size_t>(w.from)].x; w.a.y += d[static_cast<size_t>(w.from)].y; }
        if (w.to >= 0 && static_cast<size_t>(w.to) < d.size()) { w.b.x += d[static_cast<size_t>(w.to)].x; w.b.y += d[static_cast<size_t>(w.to)].y; }
    }
    for (Frame& fr : g.frames)
        for (const Node& n : g.nodes) {
            if (n.scriptIdx != fr.scriptIdx || fr.scriptIdx < 0) continue;
            fr.min.x = std::min(fr.min.x, n.pos.x - F);
            fr.min.y = std::min(fr.min.y, n.pos.y - headerH - F);
            fr.max.x = std::max(fr.max.x, n.pos.x + n.size.x + F);
            fr.max.y = std::max(fr.max.y, n.pos.y + n.size.y + F);
        }
    g.min = ImVec2(1e9f, 1e9f);
    g.max = ImVec2(-1e9f, -1e9f);
    for (const Frame& fr : g.frames) {
        g.min = ImVec2(std::min(g.min.x, fr.min.x), std::min(g.min.y, fr.min.y));
        g.max = ImVec2(std::max(g.max.x, fr.max.x), std::max(g.max.y, fr.max.y));
    }
    if (g.frames.empty()) g.min = g.max = ImVec2(0, 0);
    g.BuildIndex(F);
}

// Whether expression path `inner` lies inside `outer` (or is it).
inline bool ExprWithin(const ExprPath& inner, const ExprPath& outer) {
    if (StmtKey(inner.stmt) != StmtKey(outer.stmt) || inner.slot != outer.slot || inner.args.size() < outer.args.size()) return false;
    for (size_t i = 0; i < outer.args.size(); ++i) if (inner.args[i] != outer.args[i]) return false;
    return true;
}

struct Ctx {
    App& app;
    const mob::File& f;
    const scriptast::Program& prog;
    const Graph& g;
    const std::unordered_map<std::string, int>& byKey;
    ImVec2 origin, pan;
    float zoom, F, headerH, rowH;
    ImDrawList* dl;
    int hoverIndex;
    bool hovered;
    std::map<std::string, ImVec2>& offsets; // this file's nodes moved by hand
};

// An operation on the parsed script: written back as text and committed as one undoable change.
inline bool Apply(Ctx& c, const std::function<bool(scriptast::Program&)>& op) {
    scriptast::Program p = c.prog;
    if (!op(p)) return false;
    MobEntry* m = FindMob(c.app, c.f.path);
    return m && CommitScript(c.app, *m, scriptedit::Write(p));
}

inline bool PathLess(const StmtPath& a, const StmtPath& b) {
    if (a.script != b.script) return a.script < b.script;
    const size_t n = std::min(a.steps.size(), b.steps.size());
    for (size_t i = 0; i < n; ++i) {
        if (a.steps[i].branch != b.steps[i].branch) return a.steps[i].branch < b.steps[i].branch;
        if (a.steps[i].index != b.steps[i].index) return a.steps[i].index < b.steps[i].index;
    }
    return a.steps.size() < b.steps.size();
}

// A harmless value to put where a wired call was taken away: 0, "" or NULL, by the parameter's type.
inline scriptast::Expr DefaultFor(scriptast::Program& p, const ExprPath& path) {
    scriptast::Expr e;
    e.kind = scriptast::Expr::Number;
    e.text = "0";
    if (path.args.empty()) {
        if (scriptast::Stmt* s = scriptedit::GetStmt(p, path.stmt)) if (s->kind == scriptast::Stmt::If) e.text = "1";
        return e;
    }
    ExprPath parentPath = path;
    const int idx = parentPath.args.back();
    parentPath.args.pop_back();
    const scriptast::Expr* parent = scriptedit::GetExpr(p, parentPath);
    if (!parent || parent->kind != scriptast::Expr::Call) return e;
    auto fn = mobscript::FunctionTable().find(mobscript::LowerCase(parent->text));
    if (fn == mobscript::FunctionTable().end()) return e;
    const char* params = fn->second->params;
    const size_t np = std::strlen(params);
    const char t = params[0] == '*' ? params[1] : static_cast<size_t>(idx) < np ? params[idx] : 'f';
    if (t == 's') { e.kind = scriptast::Expr::String; e.text.clear(); }
    else if (t != 'f') { e.kind = scriptast::Expr::Name; e.text = "NULL"; }
    return e;
}

inline int RowAt(const Ctx& c, const Node& n, float canvasY) {
    const int r = static_cast<int>(std::floor((canvasY - (n.pos.y + c.headerH)) / c.rowH));
    return canvasY < n.pos.y + c.headerH || r < 0 ? -1 : r;
}

inline ImVec2 ToCanvas(const Ctx& c, ImVec2 m) { return ImVec2((m.x - c.origin.x - c.pan.x) / c.zoom, (m.y - c.origin.y - c.pan.y) / c.zoom); }
inline ImVec2 ToScreen(const Ctx& c, ImVec2 p) { return ImVec2(c.origin.x + c.pan.x + p.x * c.zoom, c.origin.y + c.pan.y + p.y * c.zoom); }

inline bool IsSelected(const State& ne, const Node& n) {
    const std::string k = Key(n);
    if (k.empty()) return false;
    for (const Sel& s : ne.items) if (Key(s) == k) return true;
    return false;
}
inline Sel SelOf(const Node& n) {
    Sel s;
    s.expr = !n.isStmt;
    s.s = n.stmt;
    s.e = n.expr;
    return s;
}
inline void Select(State& ne, const Node& n, bool add) {
    const std::string k = Key(n);
    if (k.empty()) return;
    if (!add) { ne.items.assign(1, SelOf(n)); return; }
    for (size_t i = 0; i < ne.items.size(); ++i)
        if (Key(ne.items[i]) == k) { ne.items.erase(ne.items.begin() + static_cast<long>(i)); return; } // Shift on a selected node: off
    ne.items.push_back(SelOf(n));
}

// ---- where a dragged statement would land ---------------------------------------------------------------------------

struct Drop { bool valid = false; StmtPath container; int branch = 0, index = 0; ImVec2 a, b; bool boxed = false; };

inline Drop FindDrop(const Ctx& c, int nodeIndex, ImVec2 canvas) {
    Drop d;
    if (nodeIndex < 0) return d;
    const Node& n = c.g.nodes[static_cast<size_t>(nodeIndex)];
    const ImVec2 lo = n.pos, hi(n.pos.x + n.size.x, n.pos.y + n.size.y);
    if (n.kind == Node::Entry && n.scriptIdx >= 0) { // the script's header: first in the script
        d.valid = true;
        d.container.script = n.scriptIdx;
        d.index = 0;
        d.a = ImVec2(lo.x, hi.y + 2);
        d.b = ImVec2(hi.x, hi.y + 2);
        return d;
    }
    if (!n.isStmt || n.stmt.steps.empty()) return d;
    const Step last = n.stmt.steps.back();
    const int row = RowAt(c, n, canvas.y);
    if (n.kind == Node::Control && !n.branches.empty() && row >= static_cast<int>(n.rows.size()) && row < static_cast<int>(n.rows.size() + n.branches.size())) {
        d.valid = true; // into that branch, first
        d.container = n.stmt;
        d.branch = row - static_cast<int>(n.rows.size());
        d.index = 0;
        d.boxed = true;
        d.a = ImVec2(lo.x, n.pos.y + c.headerH + c.rowH * static_cast<float>(row));
        d.b = ImVec2(hi.x, d.a.y + c.rowH);
        return d;
    }
    d.container = n.stmt;
    d.container.steps.pop_back();
    d.branch = last.branch;
    const bool after = canvas.y >= n.pos.y + n.size.y * 0.5f;
    d.index = last.index + (after ? 1 : 0);
    d.valid = true;
    const float y = after ? hi.y + 2 : lo.y - 2;
    d.a = ImVec2(lo.x, y);
    d.b = ImVec2(hi.x, y);
    return d;
}

// ---- adding ---------------------------------------------------------------------------------------------------------

inline scriptast::Expr NewScriptCall(const scriptast::Program& p, const std::string& name) {
    scriptast::Expr e;
    e.kind = scriptast::Expr::Call;
    e.text = name;
    for (const scriptast::Script& s : p.scripts) {
        if (s.world || mobscript::LowerCase(s.name) != mobscript::LowerCase(name)) continue;
        for (const scriptast::Typed& t : s.params) {
            scriptast::Expr a;
            const std::string ty = mobscript::LowerCase(t.type);
            if (ty == "float") { a.kind = scriptast::Expr::Number; a.text = "0"; }
            else if (ty == "string") a.kind = scriptast::Expr::String;
            else { a.kind = scriptast::Expr::Name; a.text = "NULL"; }
            e.args.push_back(a);
        }
    }
    return e;
}

// Where an added statement goes: after the selected statement (or into its branch), else at the end of the script frame the menu was opened in.
inline bool AddStatement(Ctx& c, State& ne, scriptast::Stmt s) {
    StmtPath made;
    bool ok = false;
    const Sel* at = nullptr;
    for (const Sel& it : ne.items) if (!it.expr) at = &it;
    if (at) {
        const StmtPath target = at->s;
        const int where = ne.where;
        ok = Apply(c, [&](scriptast::Program& p) {
            scriptast::Stmt* t = scriptedit::GetStmt(p, target);
            if (!t) return false;
            if (where >= 1 && (t->kind == scriptast::Stmt::If || t->kind == scriptast::Stmt::Loop)) return scriptedit::InsertStmt(p, target, where - 1, 0, s, &made);
            return scriptedit::InsertAfter(p, target, s, &made);
        });
    } else {
        int script = -1;
        for (const Frame& fr : c.g.frames)
            if (ne.menuCanvas.x >= fr.min.x && ne.menuCanvas.x <= fr.max.x && ne.menuCanvas.y >= fr.min.y && ne.menuCanvas.y <= fr.max.y) script = fr.scriptIdx;
        if (script < 0) { ne.message = "Select a node, or open the menu inside a script's frame"; return false; }
        StmtPath container;
        container.script = script;
        ok = Apply(c, [&](scriptast::Program& p) {
            if (script >= static_cast<int>(p.scripts.size())) return false;
            return scriptedit::InsertStmt(p, container, 0, static_cast<int>(p.scripts[static_cast<size_t>(script)].body.size()), s, &made);
        });
    }
    if (ok) { ne.items.assign(1, Sel{false, made, {}}); ne.message.clear(); }
    return ok;
}

inline void AddMenu(Ctx& c, State& ne) {
    if (ne.addOpen) { ImGui::OpenPopup("##nodeadd"); ne.addOpen = false; ne.searchFocus = true; ne.search[0] = 0; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(c.F * 14, 0), ImVec2(c.F * 26, c.F * 32));
    if (!ImGui::BeginPopup("##nodeadd")) { ne.wiring = false; return; }
    if (ne.searchFocus) { ImGui::SetKeyboardFocusHere(); ne.searchFocus = false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##nodesearch", ne.wiring ? "Search..." : "Search...", ne.search, sizeof(ne.search));
    std::string q = ne.search;
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const i18n::Verbatim verbatim;
    // what was chosen
    auto choose = [&](const std::function<bool()>& act) { if (act()) ImGui::CloseCurrentPopup(); };
    const ExprPath wireRow = ne.wireRow;
    auto addCommand = [&](const std::string& name) { choose([&] { return AddStatement(c, ne, scriptedit::NewCall(name)); }); };
    auto wireFunction = [&](const std::string& name) {
        choose([&] { return Apply(c, [&](scriptast::Program& p) { return scriptedit::SetExpr(p, wireRow, scriptedit::NewCallExpr(name)); }); });
    };
    auto addScriptCall = [&](const std::string& name) {
        choose([&] {
            scriptast::Stmt s;
            s.kind = scriptast::Stmt::Call;
            s.expr = NewScriptCall(c.prog, name);
            return AddStatement(c, ne, s);
        });
    };
    auto wireScript = [&](const std::string& name) {
        choose([&] { return Apply(c, [&](scriptast::Program& p) { return scriptedit::SetExpr(p, wireRow, NewScriptCall(p, name)); }); });
    };
    struct Item { std::string name; char returns; };
    static std::vector<Item> items;
    if (items.empty()) {
        for (const MobScriptFunction* fn : mobscript::FunctionList()) items.push_back({fn->name, fn->returns});
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return mobscript::LowerCase(a.name) < mobscript::LowerCase(b.name); });
    }
    auto wanted = [&](const Item& it) { return ne.wiring ? it.returns != 'v' : it.returns == 'v'; };
    // a function (it has a value) added from the add menu: it is placed by a click on the value it feeds
    auto place = [&](const std::string& name) { ne.placing = name; ne.message = "Click a value of a node to put " + name + " there (Esc: cancel)"; ImGui::CloseCurrentPopup(); };
    auto pick = [&](const Item& it) { if (ne.wiring) wireFunction(it.name); else if (it.returns == 'v') addCommand(it.name); else place(it.name); };
    if (!q.empty()) {
        int shown = 0;
        for (const Item& it : items) {
            if ((ne.wiring && !wanted(it)) || mobscript::LowerCase(it.name).find(q) == std::string::npos) continue; // adding: commands and functions
            if (shown++ >= 40) { ImGui::TextDisabled("... keep typing"); break; }
            ImGui::PushID(shown);
            if (ImGui::Selectable((it.name + (!ne.wiring && it.returns != 'v' ? "  (value)" : "")).c_str())) pick(it);
            ImGui::PopID();
        }
        for (const scriptast::Script& s : c.prog.scripts) {
            if (s.world || mobscript::LowerCase(s.name).find(q) == std::string::npos) continue;
            ImGui::PushID(&s);
            if (ImGui::Selectable((s.name + "  (script)").c_str())) { if (ne.wiring) wireScript(s.name); else addScriptCall(s.name); }
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("Nothing matches");
    } else {
        if (!ne.wiring) {
            const Sel* at = nullptr;
            for (const Sel& it : ne.items) if (!it.expr) at = &it;
            if (at) { // a control node: choose where
                const scriptast::Stmt* t = scriptedit::GetStmt(const_cast<scriptast::Program&>(c.prog), at->s);
                if (t && (t->kind == scriptast::Stmt::If || t->kind == scriptast::Stmt::Loop)) {
                    ImGui::RadioButton("After", &ne.where, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton(t->kind == scriptast::Stmt::If ? "In then" : "In body", &ne.where, 1);
                    if (t->kind == scriptast::Stmt::If) { ImGui::SameLine(); ImGui::RadioButton("In else", &ne.where, 2); }
                    ImGui::Separator();
                } else ne.where = 0;
            }
            if (ImGui::Selectable("If")) choose([&] { return AddStatement(c, ne, scriptedit::NewIf()); });
            if (ImGui::Selectable("Set Variable")) choose([&] { return AddStatement(c, ne, scriptedit::NewAssign("variable")); });
            if (ImGui::Selectable("Comment")) choose([&] { return AddStatement(c, ne, scriptedit::NewComment("note")); });
            ImGui::Separator();
        }
        // the commands (or the functions with a value) by first letter
        char letter = 0;
        bool open = false;
        for (const Item& it : items) {
            if (!wanted(it)) continue;
            const char l = static_cast<char>(std::toupper(static_cast<unsigned char>(it.name[0])));
            if (l != letter) {
                if (open) ImGui::EndMenu();
                letter = l;
                open = ImGui::BeginMenu((std::string(ne.wiring ? "Functions " : "Commands ") + l).c_str());
            }
            if (open) {
                ImGui::PushID(&it);
                if (ImGui::MenuItem(it.name.c_str())) pick(it);
                ImGui::PopID();
            }
        }
        if (open) ImGui::EndMenu();
        if (!ne.wiring && ImGui::BeginMenu("Functions")) { // the ones with a value: placed on the value row they feed
            char fl = 0;
            bool fopen = false;
            for (const Item& it : items) {
                if (it.returns == 'v') continue;
                const char l = static_cast<char>(std::toupper(static_cast<unsigned char>(it.name[0])));
                if (l != fl) {
                    if (fopen) ImGui::EndMenu();
                    fl = l;
                    fopen = ImGui::BeginMenu((std::string("Functions ") + l).c_str());
                }
                if (fopen) {
                    ImGui::PushID(&it);
                    if (ImGui::MenuItem(it.name.c_str())) pick(it);
                    ImGui::PopID();
                }
            }
            if (fopen) ImGui::EndMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scripts")) {
            for (const scriptast::Script& s : c.prog.scripts)
                if (!s.world) {
                    ImGui::PushID(&s);
                    if (ImGui::MenuItem(s.name.c_str())) { if (ne.wiring) wireScript(s.name); else addScriptCall(s.name); }
                    ImGui::PopID();
                }
            ImGui::EndMenu();
        }
    }
    ImGui::EndPopup();
}

// ---- the node view's input ------------------------------------------------------------------------------------------

inline void Run(State& ne, Ctx& c) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = c.dl;
    const ImVec2 m = io.MousePos, cm = ToCanvas(c, m);
    ne.handledDouble = false;
    c.app.nodeEditHover = c.hovered;

    // The selection's outlines: orange, the active one white.
    for (size_t i = 0; i < ne.items.size(); ++i) {
        auto it = c.byKey.find(Key(ne.items[i]));
        if (it == c.byKey.end()) continue;
        const Node& n = c.g.nodes[static_cast<size_t>(it->second)];
        dl->AddRect(ToScreen(c, n.pos), ToScreen(c, ImVec2(n.pos.x + n.size.x, n.pos.y + n.size.y)), i + 1 == ne.items.size() ? IM_COL32(255, 255, 255, 255) : IM_COL32(237, 87, 0, 255), 4 * c.zoom, 0, std::max(2.0f, 2.5f * c.zoom));
    }

    const bool typing = io.WantTextInput;
    // ---- G: the selected nodes follow the mouse until a click (or Enter) places them; Esc or a right click puts them back
    if (ne.grabbing) {
        const ImVec2 moved(cm.x - ne.grabStart.x, cm.y - ne.grabStart.y);
        for (const auto& kv : ne.grabBase) c.offsets[kv.first] = ImVec2(kv.second.x + moved.x, kv.second.y + moved.y);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        const bool place = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool cancel = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (place || cancel) {
            if (cancel) for (const auto& kv : ne.grabBase) c.offsets[kv.first] = kv.second;
            ne.grabbing = false;
            ne.grabBase.clear();
            SaveOffsets();
        }
        return;
    }
    // ---- a function from the add menu, following the mouse: a click on a value row puts it there
    if (!ne.placing.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(ne.placing.c_str());
        dl->AddRectFilled(ImVec2(m.x + 12, m.y + 4), ImVec2(m.x + 20 + ts.x, m.y + 8 + ts.y), IM_COL32(60, 90, 160, 230), 3.0f);
        dl->AddText(ImVec2(m.x + 16, m.y + 6), IM_COL32(255, 255, 255, 255), ne.placing.c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ne.placing.clear(); ne.message.clear(); return; }
        if (c.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Node* target = c.hoverIndex >= 0 ? &c.g.nodes[static_cast<size_t>(c.hoverIndex)] : nullptr;
            const int r = target ? RowAt(c, *target, cm.y) : -1;
            const Row* row = target && r >= 0 && r < static_cast<int>(target->rows.size()) ? &target->rows[static_cast<size_t>(r)] : nullptr;
            if (row && (row->editable || row->wired) && !row->edit.stmt.steps.empty()) {
                const ExprPath to = row->edit;
                const std::string name = ne.placing;
                if (Apply(c, [&](scriptast::Program& p) { return scriptedit::SetExpr(p, to, scriptedit::NewCallExpr(name)); })) { ne.items.clear(); ne.message.clear(); }
                ne.placing.clear();
            } else ne.message = "Click a value row of a node (a number, a text, a name or a wired value)";
        }
        return;
    }
    // the sockets near the mouse: a function's output (right of its header), a wired row's input (left of the row)
    const float reach = std::max(4.0f, c.rowH * 0.6f);
    auto outSocket = [&](const Node& n) { return ImVec2(n.pos.x + n.size.x, n.pos.y + c.headerH * 0.5f); };
    auto nearSockets = [&](int& outNode, int& inNode, int& inRow) {
        outNode = inNode = inRow = -1;
        std::vector<int> nodes, wires;
        c.g.Query(ImVec2(cm.x - reach * 2, cm.y - reach * 2), ImVec2(cm.x + reach * 2, cm.y + reach * 2), c.F, nodes, wires);
        float best = reach * reach;
        for (int ni : nodes) {
            const Node& n = c.g.nodes[static_cast<size_t>(ni)];
            if (n.isExpr && n.out) {
                const ImVec2 s = outSocket(n);
                const float d = (s.x - cm.x) * (s.x - cm.x) + (s.y - cm.y) * (s.y - cm.y);
                if (d < best) { best = d; outNode = ni; inNode = -1; }
            }
            for (size_t r = 0; r < n.rows.size(); ++r) {
                if (!n.rows[r].wired || n.rows[r].edit.stmt.steps.empty()) continue;
                const ImVec2 s(n.pos.x, n.pos.y + c.headerH + c.rowH * (static_cast<float>(r) + 0.5f));
                const float d = (s.x - cm.x) * (s.x - cm.x) + (s.y - cm.y) * (s.y - cm.y);
                if (d < best) { best = d; inNode = ni; inRow = static_cast<int>(r); outNode = -1; }
            }
        }
    };
    // ---- the mouse
    int sockOut = -1, sockIn = -1, sockRow = -1;
    if (c.hovered && !typing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) nearSockets(sockOut, sockIn, sockRow);
    if (sockOut >= 0) { // a wire out of a function: dropped on a value row, the call goes there
        const Node& n = c.g.nodes[static_cast<size_t>(sockOut)];
        ne.wiringDrag = true;
        ne.pickedUp = false;
        ne.wireFrom = n.expr;
        ne.wireAnchor = outSocket(n);
    } else if (sockIn >= 0) { // a wire picked up at its end: dropped elsewhere it moves, on the empty canvas it is disconnected
        const Row& row = c.g.nodes[static_cast<size_t>(sockIn)].rows[static_cast<size_t>(sockRow)];
        auto src = c.byKey.find(ExprKey(row.edit));
        if (src != c.byKey.end()) {
            ne.wiringDrag = true;
            ne.pickedUp = true;
            ne.wireFrom = row.edit;
            ne.wireAnchor = outSocket(c.g.nodes[static_cast<size_t>(src->second)]);
        }
    }
    if (ne.wiringDrag) {
        const ImVec2 a = ToScreen(c, ne.wireAnchor);
        const float k = std::max(30.0f, std::fabs(m.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, ImVec2(a.x + k, a.y), ImVec2(m.x - k, m.y), m, IM_COL32(255, 255, 255, 220), 2.0f);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ne.wiringDrag = false;
            const ExprPath from = ne.wireFrom;
            const Node* target = c.hoverIndex >= 0 ? &c.g.nodes[static_cast<size_t>(c.hoverIndex)] : nullptr;
            const int r = target ? RowAt(c, *target, cm.y) : -1;
            const Row* row = target && r >= 0 && r < static_cast<int>(target->rows.size()) ? &target->rows[static_cast<size_t>(r)] : nullptr;
            if (row && (row->editable || row->wired) && !row->edit.stmt.steps.empty()) {
                const ExprPath to = row->edit;
                if (ExprKey(to) == ExprKey(from)) {}
                else if (ExprWithin(to, from)) ne.message = "A call cannot be wired into itself";
                else if (Apply(c, [&](scriptast::Program& p) {
                             const scriptast::Expr* e = scriptedit::GetExpr(p, from);
                             if (!e) return false;
                             const scriptast::Expr moved = *e;
                             return scriptedit::SetExpr(p, from, DefaultFor(p, from)) && scriptedit::SetExpr(p, to, moved);
                         })) ne.items.clear(); // the selected places now hold other nodes
                else ne.message = "That value cannot take this call";
            } else if (ne.pickedUp && c.hoverIndex < 0) {
                if (Apply(c, [&](scriptast::Program& p) { return scriptedit::SetExpr(p, from, DefaultFor(p, from)); })) ne.items.clear();
            }
        }
        return;
    }
    if (c.hovered && !typing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (c.hoverIndex >= 0 && !Key(c.g.nodes[static_cast<size_t>(c.hoverIndex)]).empty()) {
            const Node& n = c.g.nodes[static_cast<size_t>(c.hoverIndex)];
            if (io.KeyShift) Select(ne, n, true);
            else if (!IsSelected(ne, n)) Select(ne, n, false);
            else { // already selected: it becomes the active one
                const std::string k = Key(n);
                for (size_t i = 0; i < ne.items.size(); ++i) if (Key(ne.items[i]) == k) { Sel s = ne.items[i]; ne.items.erase(ne.items.begin() + static_cast<long>(i)); ne.items.push_back(s); break; }
            }
            ne.pressNode = c.hoverIndex;
            ne.pressPos = m;
            ne.dragging = false;
        } else if (c.hoverIndex < 0) {
            if (!io.KeyShift) ne.items.clear();
            ne.boxing = true;
            ne.boxA = m;
        }
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (ne.pressNode >= 0 && !ne.dragging && c.g.nodes[static_cast<size_t>(ne.pressNode)].isStmt &&
            std::hypot(m.x - ne.pressPos.x, m.y - ne.pressPos.y) > 6.0f) ne.dragging = true;
        if (ne.boxing) {
            dl->AddRectFilled(ne.boxA, m, IM_COL32(255, 255, 255, 18));
            dl->AddRect(ne.boxA, m, IM_COL32(255, 255, 255, 120), 0, 0, 1.0f);
        }
    }
    Drop drop;
    if (ne.dragging) {
        const Node& from = c.g.nodes[static_cast<size_t>(ne.pressNode)];
        drop = FindDrop(c, c.hoverIndex, cm);
        if (c.hoverIndex >= 0 && Key(c.g.nodes[static_cast<size_t>(c.hoverIndex)]) == Key(from)) drop.valid = false;
        // the dragged node's ghost, and the place it would land
        const ImVec2 a = ToScreen(c, from.pos), b = ToScreen(c, ImVec2(from.pos.x + from.size.x, from.pos.y + from.size.y));
        const ImVec2 off(m.x - ne.pressPos.x, m.y - ne.pressPos.y);
        dl->AddRect(ImVec2(a.x + off.x, a.y + off.y), ImVec2(b.x + off.x, b.y + off.y), IM_COL32(255, 255, 255, 140), 4 * c.zoom, 0, 1.5f);
        if (drop.valid) {
            if (drop.boxed) dl->AddRect(ToScreen(c, drop.a), ToScreen(c, drop.b), IM_COL32(120, 200, 255, 255), 2.0f, 0, 2.0f);
            else dl->AddLine(ToScreen(c, drop.a), ToScreen(c, drop.b), IM_COL32(120, 200, 255, 255), 3.0f);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (ne.dragging && drop.valid) {
            const StmtPath from = c.g.nodes[static_cast<size_t>(ne.pressNode)].stmt;
            StmtPath now;
            if (Apply(c, [&](scriptast::Program& p) { return scriptedit::MoveStmt(p, from, drop.container, drop.branch, drop.index, &now); })) ne.items.assign(1, Sel{false, now, {}});
            else ne.message = "That node cannot go there";
        } else if (ne.boxing && std::hypot(m.x - ne.boxA.x, m.y - ne.boxA.y) > 4.0f) {
            const ImVec2 a = ToCanvas(c, ImVec2(std::min(m.x, ne.boxA.x), std::min(m.y, ne.boxA.y))), b = ToCanvas(c, ImVec2(std::max(m.x, ne.boxA.x), std::max(m.y, ne.boxA.y)));
            std::vector<int> nodes, wires;
            c.g.Query(a, b, c.F, nodes, wires);
            for (int ni : nodes) {
                const Node& n = c.g.nodes[static_cast<size_t>(ni)];
                if (Key(n).empty() || n.pos.x > b.x || n.pos.x + n.size.x < a.x || n.pos.y > b.y || n.pos.y + n.size.y < a.y) continue;
                if (!IsSelected(ne, n)) ne.items.push_back(SelOf(n));
            }
        }
        ne.boxing = false;
        ne.dragging = false;
        ne.pressNode = -1;
    }
    // a double-click on a literal: type over it
    if (c.hovered && !typing && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && c.hoverIndex >= 0) {
        const Node& n = c.g.nodes[static_cast<size_t>(c.hoverIndex)];
        const int row = RowAt(c, n, cm.y);
        if (row >= 0 && row < static_cast<int>(n.rows.size()) && n.rows[static_cast<size_t>(row)].editable) {
            scriptast::Program tmp = c.prog;
            if (const scriptast::Expr* e = scriptedit::GetExpr(tmp, n.rows[static_cast<size_t>(row)].edit)) {
                std::snprintf(ne.buf, sizeof ne.buf, "%s", scriptedit::WriteExpr(*e).c_str());
                ne.editPath = n.rows[static_cast<size_t>(row)].edit;
                ne.editPos = ToScreen(c, ImVec2(n.pos.x, n.pos.y + c.headerH + c.rowH * static_cast<float>(row)));
                ne.editWidth = std::max(c.F * 9.0f, n.size.x * c.zoom);
                ne.editOpen = true;
                ne.handledDouble = true;
            }
        }
    }
    // a right click: the context menu (on a node: it becomes the selection)
    if (c.hovered && !typing && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ne.ctxNode = c.hoverIndex;
        ne.ctxRow = -1;
        ne.menuCanvas = cm;
        if (c.hoverIndex >= 0) {
            const Node& n = c.g.nodes[static_cast<size_t>(c.hoverIndex)];
            ne.ctxRow = RowAt(c, n, cm.y);
            if (!Key(n).empty() && !IsSelected(ne, n)) Select(ne, n, false);
        }
        ne.ctxOpen = true;
    }
    // ---- the keys (only over the canvas, never while typing)
    const bool keys = c.hovered && !typing && !ImGui::IsPopupOpen("##nodeadd") && !ImGui::IsPopupOpen("##nodectx") && !ImGui::IsPopupOpen("##nodevalue");
    auto pressed = [&](ImGuiKey k) { return keys && ImGui::IsKeyPressed(k, false); };
    bool del = pressed(ImGuiKey_X) || pressed(ImGuiKey_Delete), mute = pressed(ImGuiKey_M), dup = io.KeyShift && pressed(ImGuiKey_D), add = io.KeyShift && pressed(ImGuiKey_A);
    if (pressed(ImGuiKey_Escape)) ne.items.clear();
    if (pressed(ImGuiKey_G) && !ne.items.empty()) {
        if (io.KeyAlt) { // back to the laid-out place
            for (const Sel& s : ne.items) c.offsets.erase(Key(s));
            SaveOffsets();
        } else {
            ne.grabbing = true;
            ne.grabStart = cm;
            ne.grabBase.clear();
            for (const Sel& s : ne.items) { const std::string k = Key(s); auto it = c.offsets.find(k); ne.grabBase[k] = it == c.offsets.end() ? ImVec2(0, 0) : it->second; }
        }
    }
    if (pressed(ImGuiKey_A) && !io.KeyShift) {
        if (io.KeyAlt) ne.items.clear();
        else for (const Node& n : c.g.nodes) if (n.isStmt && !IsSelected(ne, n)) ne.items.push_back(SelOf(n));
    }
    if (add) { ne.menuCanvas = cm; ne.wiring = false; ne.addOpen = true; }
    const ImVec2 menuPos = m;
    (void)menuPos;
    // ---- the actions (from the keys and from the context menu)
    auto doDelete = [&] {
        std::vector<Sel> stmts, exprs;
        for (const Sel& s : ne.items) (s.expr ? exprs : stmts).push_back(s);
        std::sort(stmts.begin(), stmts.end(), [](const Sel& a, const Sel& b) { return PathLess(b.s, a.s); }); // the later and deeper ones first
        const bool ok = Apply(c, [&](scriptast::Program& p) {
            bool any = false;
            for (const Sel& e : exprs) any = scriptedit::SetExpr(p, e.e, DefaultFor(p, e.e)) || any;
            for (const Sel& s : stmts) any = scriptedit::DeleteStmt(p, s.s) || any;
            return any;
        });
        if (ok) ne.items.clear();
    };
    auto doMute = [&] {
        std::vector<Sel> stmts;
        for (const Sel& s : ne.items) if (!s.expr) stmts.push_back(s);
        bool refused = false;
        const bool ok = Apply(c, [&](scriptast::Program& p) {
            bool any = false;
            for (const Sel& s : stmts) {
                if (scriptedit::IsMuted(p, s.s)) any = scriptedit::Unmute(p, s.s) || any;
                else if (scriptedit::Mute(p, s.s)) any = true;
                else refused = true;
            }
            return any;
        });
        if (!ok && refused) ne.message = "Only commands and assignments can be muted";
    };
    auto doDuplicate = [&] {
        std::vector<Sel> stmts;
        for (const Sel& s : ne.items) if (!s.expr) stmts.push_back(s);
        std::sort(stmts.begin(), stmts.end(), [](const Sel& a, const Sel& b) { return PathLess(b.s, a.s); });
        if (Apply(c, [&](scriptast::Program& p) { bool any = false; for (const Sel& s : stmts) any = scriptedit::DuplicateStmt(p, s.s) || any; return any; })) {
            ne.items.clear();
            if (stmts.size() == 1 && !stmts[0].s.steps.empty()) { Sel copy = stmts[0]; ++copy.s.steps.back().index; ne.items.push_back(copy); } // the copy (right after its original) is the selection, as in Blender
        }
    };
    if (del && !ne.items.empty()) doDelete();
    if (mute && !ne.items.empty()) doMute();
    if (dup && !ne.items.empty()) doDuplicate();

    // ---- the popups
    if (ne.ctxOpen) { ImGui::OpenPopup("##nodectx"); ne.ctxOpen = false; }
    if (ImGui::BeginPopup("##nodectx")) {
        const i18n::Verbatim verbatim;
        const Node* node = ne.ctxNode >= 0 && ne.ctxNode < static_cast<int>(c.g.nodes.size()) ? &c.g.nodes[static_cast<size_t>(ne.ctxNode)] : nullptr;
        const Row* row = node && ne.ctxRow >= 0 && ne.ctxRow < static_cast<int>(node->rows.size()) ? &node->rows[static_cast<size_t>(ne.ctxRow)] : nullptr;
        if (row && (row->editable || row->wired) && !row->edit.stmt.steps.empty()) {
            if (row->editable && ImGui::MenuItem("Edit Value...")) {
                scriptast::Program tmp = c.prog;
                if (const scriptast::Expr* e = scriptedit::GetExpr(tmp, row->edit)) {
                    std::snprintf(ne.buf, sizeof ne.buf, "%s", scriptedit::WriteExpr(*e).c_str());
                    ne.editPath = row->edit;
                    ne.editPos = ImGui::GetMousePosOnOpeningCurrentPopup();
                    ne.editWidth = c.F * 12.0f;
                    ne.editOpen = true;
                }
            }
            if (ImGui::MenuItem("Connect Function...")) { ne.wiring = true; ne.wireRow = row->edit; ne.addOpen = true; }
            if (row->wired && ImGui::MenuItem("Disconnect")) {
                const ExprPath path = row->edit;
                Apply(c, [&](scriptast::Program& p) { return scriptedit::SetExpr(p, path, DefaultFor(p, path)); });
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Add...", "Shift A")) { ne.wiring = false; ne.addOpen = true; }
        ImGui::BeginDisabled(ne.items.empty());
        if (ImGui::MenuItem("Duplicate", "Shift D")) doDuplicate();
        if (ImGui::MenuItem("Mute", "M")) doMute();
        if (ImGui::MenuItem("Delete", "X")) doDelete();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    AddMenu(c, ne);
    if (ne.editOpen) { ImGui::SetNextWindowPos(ne.editPos); ImGui::OpenPopup("##nodevalue"); ne.editOpen = false; ne.editFocus = true; }
    if (ImGui::BeginPopup("##nodevalue")) {
        if (ne.editFocus) { ImGui::SetKeyboardFocusHere(); ne.editFocus = false; }
        ImGui::SetNextItemWidth(ne.editWidth);
        if (ImGui::InputText("##nodevaluetext", ne.buf, sizeof ne.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            const ExprPath path = ne.editPath;
            const std::string text = ne.buf;
            if (!Apply(c, [&](scriptast::Program& p) { return scriptedit::SetValue(p, path, text); })) ne.message = "That value cannot be used there";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace nodeedit
