// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: Script tab: the script editor, its completion, and the script as nodes.
// Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

namespace mapedit {

// Script highlighting: mapedit/script_highlight.hpp (shared with the UM DLL Connector).
using scripthl::ScriptNames;
using scripthl::EachScriptToken;
using scripthl::HighlightedLine;
using scripthl::NamesOf;

// ---- script editing ----------------------------------------------------------------------------------

// Puts new text (UTF-8) in a map's script, as one undo step. False (with a message) when it cannot.
static bool CommitScript(App& app, MobEntry& m, const std::string& utf8) {
    std::vector<uint8_t> cp;
    std::string err;
    if (!codec::FromUtf8(utf8, codec::Encoding::Cp1251, cp, err)) { app.scriptMessage = "Not applied: the text has " + err; return false; }
    const std::string text(cp.begin(), cp.end());
    if (text == m.file.script) { app.scriptMessage = "No change"; return true; }
    EditStep step;
    step.kind = EditStep::Bytes;
    step.file = m.file.path;
    step.bytes = m.file.bytes;
    if (!mob::SetScript(m.file, text)) { app.scriptMessage = "This map has no script node to write to"; return false; }
    PushUndo(app, std::move(step), "Script of " + m.file.fileName);
    app.checksDirty = true;
    app.scriptMessage = "Script changed (save with " + ui::BindName(app.lib.mapKeys[config::kKeySave]) + ")";
    return true;
}

void ApplyScriptEdit(App& app) {
    if (!app.scriptEditing) return;
    MobEntry* m = FindMob(app, app.scriptEditFor);
    if (m && CommitScript(app, *m, app.scriptEdit)) app.scriptEditing = false;
}

// The script opened in the system's editor for .eis files: saves there come back as an undoable change.
static void OpenScriptExternally(App& app, const MobEntry& m) {
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::temp_directory_path(ec) / ("um-multitool-" + std::filesystem::path(m.file.fileName).stem().string() + ".eis");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { app.scriptMessage = "Cannot write " + path.string(); return; }
        const std::string text = mob::Utf8(m.file.script);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    app.scriptExtPath = path.string();
    app.scriptExtMob = m.file.path;
    app.scriptExtTime = std::filesystem::last_write_time(path, ec);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const bool opened = pio.Platform_OpenInShellFn && pio.Platform_OpenInShellFn(ImGui::GetCurrentContext(), app.scriptExtPath.c_str());
    app.scriptMessage = opened ? "Opened in the external editor; its saves come back here" : "Open this file in an editor; its saves come back here";
}

// Called every frame: picks up saves of the externally edited copy (checked twice a second).
void PollExternalScript(App& app) {
    if (app.scriptExtPath.empty()) return;
    static double last = 0.0;
    const double now = ImGui::GetTime();
    if (now - last < 0.5) return;
    last = now;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(app.scriptExtPath, ec);
    if (ec || t == app.scriptExtTime) return;
    app.scriptExtTime = t;
    MobEntry* m = FindMob(app, app.scriptExtMob);
    if (!m) { app.scriptExtPath.clear(); return; }
    std::ifstream in(app.scriptExtPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF)
        text.erase(0, 3); // a BOM some editors add
    if (CommitScript(app, *m, text)) app.scriptMessage = "Script updated from the external editor (" + m->file.fileName + ")";
    if (app.scriptEditing && app.scriptEditFor == m->file.path) app.scriptEdit = mob::Utf8(m->file.script);
}

// ---- script completion ------------------------------------------------------------------------------

static bool IsNameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '#' || u >= 0x80;
}

// A command's argument names (docs/scripting.md's, or EI ATD's when its commands are on; "" where unknown).
static std::vector<std::string> ParamNames(const MobScriptFunction& fn) {
    std::vector<std::string> names;
    const AtdScriptFunction* atd = scriptcmds::AtdDoc(fn.name);
    const scriptdocs::Doc* doc = atd ? nullptr : scriptdocs::Find(fn.name);
    if (atd || doc) {
        std::string p = atd ? atd->paramNames : doc->params, one;
        for (char ch : p + ",") {
            if (ch == ',') { while (!one.empty() && one.front() == ' ') one.erase(0, 1); const size_t colon = one.find(':'); names.push_back(colon == std::string::npos ? one : one.substr(0, colon)); one.clear(); }
            else one += ch;
        }
    }
    return names;
}

// "Name(param: type, ...) -> type" (the names from docs/scripting.md where known), the argument `current` in [ ].
static std::string Signature(const MobScriptFunction& fn, int current = -1) {
    auto type = [](char t) -> const char* {
        switch (t) { case 'f': return "float"; case 's': return "string"; case 'o': return "object"; case 'g': return "group"; case 'v': return "nothing"; default: return "any"; }
    };
    const std::vector<std::string> names = ParamNames(fn);
    const AtdScriptFunction* atd = scriptcmds::AtdDoc(fn.name); // EI ATD's description, when its commands are on
    const scriptdocs::Doc* doc = atd ? nullptr : scriptdocs::Find(fn.name);
    std::string out = std::string(fn.name) + "(";
    int i = 0;
    for (const char* p = fn.params; *p; ++p, ++i) {
        std::string arg = (i < static_cast<int>(names.size()) && !names[static_cast<size_t>(i)].empty() ? names[static_cast<size_t>(i)] + ": " : std::string()) + type(*p);
        if (i == current) arg = "[" + arg + "]";
        out += (i ? ", " : "") + arg;
    }
    out += ")";
    if (fn.returns != 'v') out += std::string(" -> ") + type(fn.returns);
    if (atd && atd->text[0]) out += std::string("\n") + atd->text;
    else if (doc) out += std::string("\n") + doc->text;
    return out;
}

// What the word before the cursor may become: script commands, the language's words, and the names
// written in the text (globals, scripts...), those starting with it first.
static void UpdateCompletion(App& app, const char* buf, int len, int cursor) {
    App::Completion& c = app.completion;
    int start = cursor;
    while (start > 0 && IsNameChar(buf[start - 1])) --start;
    const std::string prefix(buf + start, buf + cursor);
    const bool afterWord = cursor < len && IsNameChar(buf[cursor]);
    if (prefix != c.prefix || start != c.wordStart) c.dismissed = false;
    c.prefix = prefix;
    c.wordStart = start;
    int lineStart = cursor;
    while (lineStart > 0 && buf[lineStart - 1] != '\n') --lineStart;
    c.lineBeforeCursor.assign(buf + lineStart, buf + cursor);
    c.line = 0;
    for (int i = 0; i < cursor; ++i) if (buf[i] == '\n') ++c.line;
    // The call around the cursor: the name before the nearest open parenthesis.
    c.signature.clear();
    int depth = 0;
    for (int i = cursor - 1; i >= 0 && i > cursor - 400; --i) {
        if (buf[i] == ')') ++depth;
        else if (buf[i] == '(') {
            if (depth-- > 0) continue;
            int e = i;
            while (e > 0 && buf[e - 1] == ' ') --e;
            int b = e;
            while (b > 0 && IsNameChar(buf[b - 1])) --b;
            auto it = mobscript::FunctionTable().find(mobscript::LowerCase(std::string(buf + b, buf + e)));
            int arg = 0, d2 = 0; // which argument the cursor is in: the commas outside nested calls and strings
            bool quoted = false;
            for (int k = i + 1; k < cursor; ++k) {
                if (buf[k] == '"') quoted = !quoted;
                else if (quoted) continue;
                else if (buf[k] == '(') ++d2;
                else if (buf[k] == ')') --d2;
                else if (buf[k] == ',' && d2 == 0) ++arg;
            }
            if (it != mobscript::FunctionTable().end()) c.signature = Signature(*it->second, arg);
            break;
        }
    }
    // The names written in the text, gathered again when it changes.
    const size_t hash = std::hash<std::string_view>{}(std::string_view(buf, static_cast<size_t>(len)));
    if (hash != c.wordsFor) {
        c.wordsFor = hash;
        c.words.clear();
        std::unordered_set<std::string> seen;
        for (int i = 0; i < len;) {
            if (buf[i] == '/' && i + 1 < len && buf[i + 1] == '/') { while (i < len && buf[i] != '\n') ++i; continue; }
            if (buf[i] == '"') { ++i; while (i < len && buf[i] != '"' && buf[i] != '\n') ++i; ++i; continue; }
            if (!IsNameChar(buf[i])) { ++i; continue; }
            int b = i;
            while (i < len && IsNameChar(buf[i])) ++i;
            std::string w(buf + b, buf + i);
            if (w.size() >= 3 && !(w[0] >= '0' && w[0] <= '9') && seen.insert(mobscript::LowerCase(w)).second) c.words.push_back(w);
        }
    }
    c.items.clear();
    if (prefix.size() < 2 || afterWord || c.dismissed || (prefix[0] >= '0' && prefix[0] <= '9')) { c.index = 0; return; }
    const std::string lp = mobscript::LowerCase(prefix);
    std::vector<std::string> starts, contains;
    std::unordered_set<std::string> added;
    auto consider = [&](const std::string& w) {
        const std::string lw = mobscript::LowerCase(w);
        if (lw == lp || !added.insert(lw).second) return;
        const size_t at = lw.find(lp);
        if (at == 0) starts.push_back(w);
        else if (at != std::string::npos && lp.size() >= 3) contains.push_back(w);
    };
    static const char* const language[] = {"GlobalVars", "DeclareScript", "Script", "WorldScript", "if", "then", "else", "object", "group", "float", "string"};
    for (const char* w : language) consider(w);
    for (const MobScriptFunction* fn : mobscript::FunctionList()) consider(fn->name);
    for (const std::string& w : c.words) if (mobscript::LowerCase(w) != lp) consider(w);
    auto byLength = [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; };
    std::sort(starts.begin(), starts.end(), byLength);
    std::sort(contains.begin(), contains.end(), byLength);
    c.items = starts;
    c.items.insert(c.items.end(), contains.begin(), contains.end());
    if (c.items.size() > 12) c.items.resize(12);
    if (c.index >= static_cast<int>(c.items.size())) c.index = 0;
}

static int ScriptEditCallback(ImGuiInputTextCallbackData* d) {
    App& app = *static_cast<App*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        app.scriptEdit.resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = app.scriptEdit.data();
        return 0;
    }
    App::Completion& c = app.completion;
    if (app.scriptEditGotoLine >= 0) { // entering edit mode: the cursor on the line the view showed at its top
        int pos = 0, line = 0;
        while (line < app.scriptEditGotoLine && pos < d->BufTextLen) { if (d->Buf[pos] == '\n') ++line; ++pos; }
        d->CursorPos = d->SelectionStart = d->SelectionEnd = pos;
        app.scriptEditScrollLine = app.scriptEditGotoLine;
        app.scriptEditGotoLine = -1;
    }
    if (c.keepCursor) { // Up / Down went to the list
        d->CursorPos = d->SelectionStart = d->SelectionEnd = std::min(c.cursor, d->BufTextLen);
        c.keepCursor = false;
    }
    // Tab / Enter with the list up: the picked word replaces the one being typed (a command gets its "(").
    if ((c.accept || c.tab) && !c.items.empty() && c.wordStart <= d->CursorPos) {
        std::string w = c.items[c.index];
        if (mobscript::FunctionTable().count(mobscript::LowerCase(w)) && (d->CursorPos >= d->BufTextLen || d->Buf[d->CursorPos] != '(')) w += "(";
        d->DeleteChars(c.wordStart, d->CursorPos - c.wordStart);
        d->InsertChars(c.wordStart, w.c_str());
        c.items.clear();
        c.dismissed = true;
    } else if (c.tab) {
        d->InsertChars(d->CursorPos, "  "); // no list: Tab indents
    }
    c.accept = c.tab = false;
    c.cursor = d->CursorPos;
    int line = 1;
    for (int i = 0; i < d->CursorPos && i < d->BufTextLen; ++i) if (d->Buf[i] == '\n') ++line;
    app.scriptCursorLine = line;
    if (!d->HasSelection()) UpdateCompletion(app, d->Buf, d->BufTextLen, d->CursorPos);
    else c.items.clear();
    return 0;
}

void ScriptContent(App& app);

// The Script tab: the script, or a note while it is in its own window.
// One area call of a map's script rewritten with new numbers (an undo step; the script is saved with the map).
void SetAreaCall(App& app, MobEntry& m, const quests::AreaCall& a) {
    std::string text = m.file.script;
    if (a.end > text.size() || a.begin >= a.end) return;
    text.replace(a.begin, a.end - a.begin, quests::FormatAreaCall(a));
    if (CommitScript(app, m, codec::ToUtf8(std::vector<uint8_t>(text.begin(), text.end()), codec::Encoding::Cp1251)))
        app.scriptModelKey.clear(); // the areas are read again
}

// The script area (its file and call) under a ground point: a circle containing it, else a rectangle.
bool AreaAt(App& app, float x, float y, std::string& file, size_t& index) {
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            const quests::AreaCall& a = calls[i];
            const float e = std::max(0.4f, app.scene.camera.distance * 0.012f); // the edge can be grabbed from just outside
            const bool in = a.round ? std::hypot(x - a.v[0], y - a.v[1]) <= a.v[2] + e
                                    : x >= std::min(a.v[0], a.v[2]) - e && x <= std::max(a.v[0], a.v[2]) + e && y >= std::min(a.v[1], a.v[3]) - e &&
                                          y <= std::max(a.v[1], a.v[3]) + e;
            if (in) { file = m->file.path; index = i; return true; }
        }
    }
    return false;
}

void PlaceArea(App& app, float x, float y) {
    MobEntry* m = FindMob(app, app.areaPlace.file);
    if (!m) return;
    std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
    if (app.areaPlace.index >= calls.size()) return;
    quests::AreaCall a = calls[app.areaPlace.index];
    if (a.round) { a.v[0] = x; a.v[1] = y; }
    else {
        const float w = a.v[2] - a.v[0], h = a.v[3] - a.v[1];
        a.v[0] = x - w * 0.5f; a.v[1] = y - h * 0.5f; a.v[2] = a.v[0] + w; a.v[3] = a.v[1] + h;
    }
    SetAreaCall(app, *m, a);
}

// The areas the loaded maps' scripts declare, editable: their numbers (Enter applies), or "Place here".
static void AreasPanel(App& app) {
    int count = 0;
    for (auto& m : app.mobs) count += static_cast<int>(quests::AreaCalls(m->file.script).size());
    if (!ImGui::CollapsingHeader(("Areas (" + std::to_string(count) + ")###areas").c_str())) return;
    if (count == 0) { ImGui::TextDisabled("No AddRoundToArea / AddRectToArea in the loaded maps' scripts."); return; }
    ImGui::TextDisabled("The scripts' areas (Layers -> Script areas). Change a number and press Enter, or Place here then click the map:");
    ImGui::TextDisabled("the call in the script is rewritten (undo with %s; saved with the map).", ui::BindName(app.lib.mapKeys[config::kKeyUndo]).c_str());
    for (auto& m : app.mobs) {
        const std::vector<quests::AreaCall> calls = quests::AreaCalls(m->file.script);
        for (size_t i = 0; i < calls.size(); ++i) {
            quests::AreaCall a = calls[i];
            ImGui::PushID((m->file.path + "#" + std::to_string(i)).c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Area %d %s", a.id, a.round ? "(circle)" : "(rectangle)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m->file.fileName.c_str());
            ImGui::SameLine(150);
            ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 110));
            const bool changed = a.round ? ImGui::InputFloat3("##v", a.v, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)
                                         : ImGui::InputFloat4("##v", a.v, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(a.round ? "x, y, radius" : "x1, y1, x2, y2 (two corners)");
            if (changed) SetAreaCall(app, *m, a);
            ImGui::SameLine();
            const bool placing = app.areaPlace.on && app.areaPlace.file == m->file.path && app.areaPlace.index == i;
            if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.35f, 0.75f, 1));
            if (ImGui::Button(placing ? "Click the map" : "Place here")) {
                if (placing) app.areaPlace.on = false;
                else { app.areaPlace.on = true; app.areaPlace.file = m->file.path; app.areaPlace.index = i; app.scene.options.scriptAreas = true; }
            }
            if (placing) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Then click the map: the area's centre moves there (Escape cancels)");
            ImGui::PopID();
        }
    }
    ImGui::Separator();
}

void ScriptTab(App& app) {
    AreasPanel(app);
    if (app.scriptWindow) {
        ImGui::TextWrapped("The script is shown in its own window.");
        if (ImGui::Button("Show it here again")) app.scriptWindow = false;
        return;
    }
    ScriptContent(app);
}


// ---- the script as nodes ---------------------------------------------------------------------------
// The Script tab's Nodes mode (view only), like Blender's node editor: each script a frame holding its
// entry node; its statements flow down a spine, if / for each send their branches to the chains on their
// right, and a command's arguments come in from the left, through wires from the commands that compute
// them (a number, a string or a variable is written in its socket). Laid out once per text, in canvas units
// of the UI font's size; drawn panned and zoomed.
namespace nodegraph {

using scriptast::Expr;
using scriptast::Stmt;

struct Row {
    std::string label, value;
    char type = '?';
    bool wired = false;
    std::string full; // a shortened value, whole
    Row() = default;
    Row(std::string l, std::string v, char t, bool w) : label(std::move(l)), value(std::move(v)), type(t), wired(w) {}
};

struct Node {
    enum Kind { Entry, Command, Function, ScriptCall, Control, Assign, Comment } kind = Command;
    std::string title, script; // script: the frame it is in (lower-case)
    std::string target;        // ScriptCall: the called script (lower-case)
    int line = 0;
    ImVec2 pos, size;
    std::vector<Row> rows;             // inputs (left sockets), or the entry's parameters / a comment's lines
    std::vector<std::string> branches; // right sockets: then / else / body
    bool out = false;                  // a value goes out (right of the header)
    char outType = '?';
    bool flowIn = false;               // on the spine (top) / reached by a branch
};

struct Wire {
    ImVec2 a, b;
    ImU32 color;
    bool flow;         // down a spine
    bool call = false; // from a script call to the called script's entry node (across frames)
    int from = -1, to = -1;
};
struct Frame { std::string name, lower, subtitle; ImVec2 min, max; int line = 0; };

// A wire's box in canvas units, the curve's reach included (the same distances as where it is drawn).
inline void WireBounds(const Wire& w, float F, ImVec2& lo, ImVec2& hi) {
    lo = ImVec2(std::min(w.a.x, w.b.x), std::min(w.a.y, w.b.y));
    hi = ImVec2(std::max(w.a.x, w.b.x), std::max(w.a.y, w.b.y));
    const float dx = std::fabs(w.b.x - w.a.x), dy = std::fabs(w.b.y - w.a.y);
    const float d = w.call ? std::max(F * 4, dx * 0.5f + dy * 0.15f) : std::max(F, dx * 0.5f);
    lo.x -= d; hi.x += d;
    if (w.flow) { lo.y -= std::max(4.0f, dy * 0.5f); hi.y += std::max(4.0f, dy * 0.5f); }
}

struct Graph {
    std::vector<Node> nodes;
    std::vector<Wire> wires;
    std::vector<Frame> frames;
    ImVec2 min{0, 0}, max{0, 0};

    // The nodes and wires by place, so that a frame touches the ones in view (a 30000-line script has tens of
    // thousands of each): a grid of cells over the graph, each holding the indices of what overlaps it. A wire
    // reaching over many cells (most calls to other scripts) is kept in `longWires` and tested every time.
    float cell = 1;
    int cols = 1, rows = 1;
    std::vector<std::vector<int>> nodeCells, wireCells;
    std::vector<int> longWires;

    int CellX(float x) const { return std::clamp(static_cast<int>(std::floor((x - min.x) / cell)), 0, cols - 1); }
    int CellY(float y) const { return std::clamp(static_cast<int>(std::floor((y - min.y) / cell)), 0, rows - 1); }

    void BuildIndex(float F) {
        const float w = std::max(max.x - min.x, 1.0f), h = std::max(max.y - min.y, 1.0f);
        cell = std::max(F * 30, std::sqrt(w * h / 20000.0f)); // at most about 20000 cells
        cols = static_cast<int>(w / cell) + 1;
        rows = static_cast<int>(h / cell) + 1;
        nodeCells.assign(static_cast<size_t>(cols) * rows, {});
        wireCells.assign(static_cast<size_t>(cols) * rows, {});
        longWires.clear();
        for (size_t i = 0; i < nodes.size(); ++i) {
            const Node& n = nodes[i];
            for (int y = CellY(n.pos.y); y <= CellY(n.pos.y + n.size.y); ++y)
                for (int x = CellX(n.pos.x - F); x <= CellX(n.pos.x + n.size.x + F); ++x) nodeCells[static_cast<size_t>(y) * cols + x].push_back(static_cast<int>(i));
        }
        for (size_t i = 0; i < wires.size(); ++i) {
            ImVec2 lo, hi;
            WireBounds(wires[i], F, lo, hi);
            const int x0 = CellX(lo.x), x1 = CellX(hi.x), y0 = CellY(lo.y), y1 = CellY(hi.y);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) > 12) { longWires.push_back(static_cast<int>(i)); continue; }
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) wireCells[static_cast<size_t>(y) * cols + x].push_back(static_cast<int>(i));
        }
    }

    // The nodes and wires that may be in the canvas rectangle [lo, hi], once each, in order.
    void Query(ImVec2 lo, ImVec2 hi, float F, std::vector<int>& outNodes, std::vector<int>& outWires) const {
        outNodes.clear();
        outWires.clear();
        if (hi.x < min.x - cell || lo.x > max.x + cell || hi.y < min.y - cell || lo.y > max.y + cell) return;
        const int x0 = CellX(lo.x), x1 = CellX(hi.x), y0 = CellY(lo.y), y1 = CellY(hi.y);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const size_t c = static_cast<size_t>(y) * cols + x;
                outNodes.insert(outNodes.end(), nodeCells[c].begin(), nodeCells[c].end());
                outWires.insert(outWires.end(), wireCells[c].begin(), wireCells[c].end());
            }
        for (int i : longWires) {
            ImVec2 wl, wh;
            WireBounds(wires[static_cast<size_t>(i)], F, wl, wh);
            if (wh.x >= lo.x && wl.x <= hi.x && wh.y >= lo.y && wl.y <= hi.y) outWires.push_back(i);
        }
        for (std::vector<int>* v : {&outNodes, &outWires}) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
    }

    // The last (topmost) node containing the canvas point, or -1.
    int NodeAt(ImVec2 p) const {
        if (p.x < min.x - cell || p.x > max.x + cell || p.y < min.y - cell || p.y > max.y + cell) return -1;
        int best = -1;
        for (int i : nodeCells[static_cast<size_t>(CellY(p.y)) * cols + CellX(p.x)]) {
            const Node& n = nodes[static_cast<size_t>(i)];
            if (p.x >= n.pos.x && p.x <= n.pos.x + n.size.x && p.y >= n.pos.y && p.y <= n.pos.y + n.size.y) best = std::max(best, i);
        }
        return best;
    }
};

inline ImVec4 TypeColor(char t) {
    switch (t) {
    case 'f': return ImVec4(0.63f, 0.63f, 0.63f, 1);
    case 's': return ImVec4(0.44f, 0.70f, 1.00f, 1);
    case 'o': return ImVec4(0.93f, 0.62f, 0.36f, 1);
    case 'g': return ImVec4(0.39f, 0.78f, 0.39f, 1);
    default: return ImVec4(0.65f, 0.55f, 0.85f, 1);
    }
}

inline ImVec4 HeaderColor(Node::Kind k) {
    switch (k) {
    case Node::Entry: return ImVec4(0.45f, 0.20f, 0.20f, 1);
    case Node::Command: return ImVec4(0.16f, 0.38f, 0.40f, 1);
    case Node::Function: return ImVec4(0.20f, 0.30f, 0.52f, 1);
    case Node::ScriptCall: return ImVec4(0.50f, 0.33f, 0.12f, 1);
    case Node::Control: return ImVec4(0.38f, 0.26f, 0.50f, 1);
    case Node::Assign: return ImVec4(0.45f, 0.22f, 0.30f, 1);
    default: return ImVec4(0.24f, 0.30f, 0.22f, 1);
    }
}

// A cubic Bezier drawn only where it is seen: subdivided until flat, pieces whose control points lie outside
// `clip` dropped, so that a curve hundreds of thousands of pixels long costs the vertices of its visible part
// (ImDrawList::AddBezierCubic tessellates all of it, which put 240000 vertices in one frame).
class BezierInView {
public:
    BezierInView(ImDrawList* dl, ImRect clip, ImU32 color, float thickness) : dl_(dl), clip_(clip), color_(color), thickness_(thickness) {
        clip_.Expand(thickness + 1);
    }
    void Draw(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {
        points_.clear();
        Split(p0, p1, p2, p3, 0);
        Flush();
    }

private:
    ImDrawList* dl_;
    ImRect clip_;
    ImU32 color_;
    float thickness_;
    std::vector<ImVec2>& points_ = Scratch(); // one buffer for all the wires of a frame
    static std::vector<ImVec2>& Scratch() { static std::vector<ImVec2> v; return v; }

    void Flush() {
        if (points_.size() >= 2) dl_->AddPolyline(points_.data(), static_cast<int>(points_.size()), color_, ImDrawFlags_None, thickness_);
        points_.clear();
    }
    void Split(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, int depth) {
        const float lox = std::min(std::min(p0.x, p1.x), std::min(p2.x, p3.x)), hix = std::max(std::max(p0.x, p1.x), std::max(p2.x, p3.x));
        const float loy = std::min(std::min(p0.y, p1.y), std::min(p2.y, p3.y)), hiy = std::max(std::max(p0.y, p1.y), std::max(p2.y, p3.y));
        if (hix < clip_.Min.x || lox > clip_.Max.x || hiy < clip_.Min.y || loy > clip_.Max.y) { Flush(); return; } // not seen (the curve stays in its control points' box)
        // flat enough: the control points within 0.5 px of the chord and between its ends (not out beyond them, as
        // with a call to a script on the left at the same height: all four on a line, the curve still loops out)
        const float cx = p3.x - p0.x, cy = p3.y - p0.y, len = std::sqrt(cx * cx + cy * cy);
        bool flat;
        if (len < 1.0f) flat = (hix - lox) + (hiy - loy) < 2.0f;
        else {
            const float d1 = std::fabs((p1.x - p0.x) * cy - (p1.y - p0.y) * cx), d2 = std::fabs((p2.x - p0.x) * cy - (p2.y - p0.y) * cx);
            const float t1 = ((p1.x - p0.x) * cx + (p1.y - p0.y) * cy) / len, t2 = ((p2.x - p0.x) * cx + (p2.y - p0.y) * cy) / len;
            flat = d1 + d2 < 0.5f * len && t1 >= -0.5f && t1 <= len + 0.5f && t2 >= -0.5f && t2 <= len + 0.5f;
        }
        if (flat || depth >= 20) {
            if (points_.empty()) points_.push_back(p0);
            points_.push_back(p3);
            return;
        }
        const ImVec2 p01((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), p12((p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f), p23((p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f);
        const ImVec2 a((p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f), b((p12.x + p23.x) * 0.5f, (p12.y + p23.y) * 0.5f);
        const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        Split(p0, p01, a, m, depth + 1);
        Split(m, b, p23, p3, depth + 1);
    }
};

class Builder {
public:
    Builder(const scriptast::Program& p, const ScriptNames& names) : prog_(p), names_(names) {
        F = ImGui::GetFontSize();
        headerH = F * 1.5f; rowH = F * 1.35f; pad = F * 0.5f; gapX = F * 2.2f; gapY = F * 0.9f;
        for (const scriptast::Script& s : p.scripts) if (!s.world) scripts_[mobscript::LowerCase(s.name)] = &s;
    }

    Graph Build() {
        const i18n::Verbatim verbatim; // the script's words are measured as they are
        // Who calls each script, for the frames' subtitles.
        std::unordered_map<std::string, std::vector<std::string>> calledBy;
        for (const scriptast::Script& s : prog_.scripts) {
            std::vector<std::string> calls;
            Calls(s.body, calls);
            for (const std::string& c : calls) {
                auto& v = calledBy[c];
                if (std::find(v.begin(), v.end(), s.name) == v.end()) v.push_back(s.name);
            }
        }
        // The frames in columns by call depth, like a node tree read left to right: WorldScript (the game runs
        // it when the map loads), the scripts the game starts itself ("#...": events, dialogs, objects) and the
        // ones nothing calls first; then the scripts they call, and so on. In a column, the scripts are in the
        // order of the calls coming to them (fewer crossing wires).
        std::unordered_map<std::string, std::vector<std::string>> callees;
        std::unordered_map<std::string, const scriptast::Script*> byLower;
        for (const scriptast::Script& s : prog_.scripts) {
            const std::string l = mobscript::LowerCase(s.name);
            byLower[l] = &s;
            Calls(s.body, callees[l]);
        }
        std::unordered_map<std::string, int> depth;
        std::vector<std::string> todo;
        for (const scriptast::Script& s : prog_.scripts) {
            const std::string l = mobscript::LowerCase(s.name);
            if (s.world || (!s.name.empty() && s.name[0] == '#') || !calledBy.count(l)) { depth[l] = 0; todo.push_back(l); }
        }
        for (size_t i = 0; i < todo.size(); ++i)
            for (const std::string& c : callees[todo[i]])
                if (byLower.count(c) && !depth.count(c)) { depth[c] = depth[todo[i]] + 1; todo.push_back(c); }
        int deepest = 0;
        for (const scriptast::Script& s : prog_.scripts) {
            const std::string l = mobscript::LowerCase(s.name);
            if (!depth.count(l)) depth[l] = 0; // only in a loop of calls nothing outside reaches
            deepest = std::max(deepest, depth[l]);
        }
        std::vector<std::vector<const scriptast::Script*>> columns(static_cast<size_t>(deepest) + 1);
        for (const scriptast::Script& s : prog_.scripts) if (s.world) columns[0].push_back(&s);
        for (const scriptast::Script& s : prog_.scripts) if (!s.world) columns[static_cast<size_t>(depth[mobscript::LowerCase(s.name)])].push_back(&s);
        std::unordered_map<std::string, int> entryOf; // lower-case script name -> its entry node
        float colX = 0;
        for (size_t col = 0; col < columns.size(); ++col) {
            if (col > 0) { // by the height of the first call coming from the columns on the left
                std::unordered_map<std::string, float> callY;
                for (const Node& n : g_.nodes)
                    if (n.kind == Node::ScriptCall) {
                        auto it = callY.find(n.target);
                        if (it == callY.end() || n.pos.y < it->second) callY[n.target] = n.pos.y;
                    }
                std::stable_sort(columns[col].begin(), columns[col].end(), [&](const scriptast::Script* a, const scriptast::Script* b) {
                    auto ka = callY.find(mobscript::LowerCase(a->name)), kb = callY.find(mobscript::LowerCase(b->name));
                    return (ka == callY.end() ? 1e30f : ka->second) < (kb == callY.end() ? 1e30f : kb->second);
                });
            }
            float y = 0, right = colX;
            for (const scriptast::Script* sp : columns[col]) {
                const scriptast::Script& s = *sp;
                const float left = colX;
                script_ = mobscript::LowerCase(s.name);
                const size_t first = g_.nodes.size();
                Node entry;
                entry.kind = Node::Entry;
                entry.title = s.world ? "WorldScript" : "Script " + s.name;
                entry.line = s.line;
                for (const scriptast::Typed& t : s.params) entry.rows.push_back({t.name, t.type, '?', false});
                // How it starts: the game (WorldScript; "#..." scripts are the game's events, dialogs and objects'),
                // the scripts that call it (wired in), or nothing.
                const bool called = calledBy.count(script_) != 0;
                entry.flowIn = called;
                if (s.world) entry.rows.push_back({"run by the game when the map loads", std::string(), 'n', false});
                else if (!s.name.empty() && s.name[0] == '#') entry.rows.push_back({"started by the game (event, dialog or object)", std::string(), 'n', false});
                else if (!called) entry.rows.push_back({"called by no script", std::string(), 'w', false});
                entry.size = NodeSize(entry);
                const float inW = ChainInputsWidth(s.body);
                entry.pos = ImVec2(left + inW, y + F * 2.2f); // room for the frame's label
                const int e = Add(entry);
                entryOf[script_] = e;
                float bottom = entry.pos.y + entry.size.y;
                if (!s.body.empty()) bottom = Chain(s.body, left, bottom + gapY, e, -1);
                Frame fr;
                fr.name = entry.title;
                fr.lower = script_;
                fr.line = s.line;
                auto cb = calledBy.find(script_);
                if (cb != calledBy.end()) {
                    fr.subtitle = "called by: ";
                    for (size_t i = 0; i < cb->second.size(); ++i) fr.subtitle += (i ? ", " : "") + cb->second[i];
                }
                fr.min = ImVec2(1e9f, y);
                fr.max = ImVec2(-1e9f, bottom);
                for (size_t i = first; i < g_.nodes.size(); ++i) {
                    fr.min.x = std::min(fr.min.x, g_.nodes[i].pos.x);
                    fr.max.x = std::max(fr.max.x, g_.nodes[i].pos.x + g_.nodes[i].size.x);
                    fr.max.y = std::max(fr.max.y, g_.nodes[i].pos.y + g_.nodes[i].size.y);
                }
                fr.min.x -= F;
                fr.max.x += F;
                fr.max.y += F;
                fr.max.x = std::max(fr.max.x, fr.min.x + ImGui::CalcTextSize((fr.name + "    " + fr.subtitle).c_str()).x + 2 * F);
                g_.frames.push_back(fr);
                right = std::max(right, fr.max.x);
                y = fr.max.y + F * 2.5f;
            }
            colX = right + F * 6;
        }
        // Every script call wired to the called script's entry node.
        for (size_t i = 0; i < g_.nodes.size(); ++i) {
            const Node& n = g_.nodes[i];
            if (n.kind != Node::ScriptCall) continue;
            auto to = entryOf.find(n.target);
            if (to == entryOf.end()) continue;
            Wire w{CallSocket(n), FlowIn(g_.nodes[static_cast<size_t>(to->second)]), IM_COL32(230, 150, 60, 150), false};
            w.call = true;
            w.from = static_cast<int>(i);
            w.to = to->second;
            g_.wires.push_back(w);
        }
        g_.min = ImVec2(1e9f, 1e9f);
        g_.max = ImVec2(-1e9f, -1e9f);
        for (const Frame& f : g_.frames) {
            g_.min = ImVec2(std::min(g_.min.x, f.min.x), std::min(g_.min.y, f.min.y));
            g_.max = ImVec2(std::max(g_.max.x, f.max.x), std::max(g_.max.y, f.max.y));
        }
        if (g_.frames.empty()) g_.min = g_.max = ImVec2(0, 0);
        g_.BuildIndex(F);
        return std::move(g_);
    }

    float headerH = 0, rowH = 0, pad = 0, F = 0;

private:
    const scriptast::Program& prog_;
    const ScriptNames& names_;
    std::unordered_map<std::string, const scriptast::Script*> scripts_;
    std::string script_;
    Graph g_;
    float gapX = 0, gapY = 0;

    void Calls(const std::vector<Stmt>& v, std::vector<std::string>& out) {
        std::function<void(const Expr&)> expr = [&](const Expr& e) {
            if (e.kind == Expr::Call && scripts_.count(mobscript::LowerCase(e.text))) {
                const std::string n = mobscript::LowerCase(e.text);
                if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
            }
            for (const Expr& a : e.args) expr(a);
        };
        for (const Stmt& s : v) {
            for (const Expr& e : s.conditions) expr(e);
            expr(s.expr);
            Calls(s.then, out);
            Calls(s.otherwise, out);
            Calls(s.body, out);
        }
    }

    int Add(Node n) {
        n.script = script_;
        g_.nodes.push_back(std::move(n));
        return static_cast<int>(g_.nodes.size()) - 1;
    }

    static std::string Short(const std::string& v) {
        if (v.size() <= 28) return v;
        size_t cut = 25;
        while (cut > 0 && (static_cast<unsigned char>(v[cut]) & 0xC0) == 0x80) --cut; // not inside a UTF-8 character
        return v.substr(0, cut) + "...";
    }

    ImVec2 NodeSize(const Node& n) const {
        float w = ImGui::CalcTextSize(n.title.c_str()).x + 2 * pad + (n.out ? F : 0);
        for (const Row& r : n.rows) {
            const std::string t = r.value.empty() || r.wired ? r.label : (r.label.empty() ? r.value : r.label + ": " + r.value);
            w = std::max(w, ImGui::CalcTextSize(t.c_str()).x + 2 * pad + F * 0.6f);
        }
        for (const std::string& b : n.branches) w = std::max(w, ImGui::CalcTextSize(b.c_str()).x + 2 * pad + F);
        w = std::max(w, F * 7.0f);
        const size_t rows = n.rows.size() + n.branches.size();
        return ImVec2(w, headerH + rowH * static_cast<float>(rows) + (rows ? pad * 0.5f : 0));
    }

    ImVec2 InSocket(const Node& n, size_t row) const { return ImVec2(n.pos.x, n.pos.y + headerH + rowH * (static_cast<float>(row) + 0.5f)); }
    ImVec2 OutSocket(const Node& n) const { return ImVec2(n.pos.x + n.size.x, n.pos.y + headerH * 0.5f); }
    ImVec2 BranchSocket(const Node& n, size_t b) const {
        return ImVec2(n.pos.x + n.size.x, n.pos.y + headerH + rowH * (static_cast<float>(n.rows.size() + b) + 0.5f));
    }
    ImVec2 FlowIn(const Node& n) const { return ImVec2(n.pos.x, n.pos.y + headerH * 0.5f); }
    ImVec2 CallSocket(const Node& n) const { return ImVec2(n.pos.x + n.size.x, n.pos.y + headerH * 0.5f); }

    // A node for a call (the arguments as rows: a literal or a variable written in, a call wired in).
    Node CallNode(const Expr& e) const {
        Node n;
        n.title = e.text;
        n.line = e.line;
        const std::string lower = mobscript::LowerCase(e.text);
        auto fn = mobscript::FunctionTable().find(lower);
        std::vector<std::string> pnames;
        const char* params = "";
        if (fn != mobscript::FunctionTable().end()) {
            pnames = ParamNames(*fn->second);
            params = fn->second->params;
            n.kind = fn->second->returns == 'v' ? Node::Command : Node::Function;
            n.out = fn->second->returns != 'v';
            n.outType = fn->second->returns;
        } else if (scripts_.count(lower)) {
            n.kind = Node::ScriptCall;
            n.target = lower;
            const scriptast::Script* s = scripts_.at(lower);
            for (const scriptast::Typed& t : s->params) pnames.push_back(t.name);
        } else {
            n.kind = Node::Function;
        }
        const size_t np = std::strlen(params);
        for (size_t i = 0; i < e.args.size(); ++i) {
            const Expr& a = e.args[i];
            Row r;
            r.label = i < pnames.size() ? pnames[i] : std::string();
            r.type = params[0] == '*' ? params[1] : i < np ? params[i] : '?';
            r.wired = a.kind == Expr::Call;
            if (!r.wired) {
                const std::string v = a.kind == Expr::String ? "\"" + a.text + "\"" : a.text;
                r.value = Short(v);
                if (r.value != v) r.full = v;
            }
            if (r.wired && r.label.empty()) r.label = a.text;
            n.rows.push_back(r);
        }
        n.size = NodeSize(n);
        return n;
    }

    // The width the calls feeding these arguments take on the left (0: none).
    float ArgsWidth(const std::vector<Expr>& args) const {
        float w = 0;
        for (const Expr& a : args)
            if (a.kind == Expr::Call) w = std::max(w, SubtreeWidth(a));
        return w > 0 ? w + gapX : 0;
    }
    float SubtreeWidth(const Expr& e) const { return CallNode(e).size.x + ArgsWidth(e.args); }

    // The calls feeding node `to`'s arguments, right of them at `right`, from `top` down; their height.
    float Feed(const std::vector<Expr>& args, int to, float right, float top) {
        float y = top;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].kind != Expr::Call) continue;
            const float h = PlaceExpr(args[i], right, y, to, i);
            y += h + gapY * 0.6f;
        }
        return y > top ? y - top - gapY * 0.6f : 0;
    }

    // A call computing a value, its right edge at `right`, wired into row `row` of node `to`; its subtree's height.
    float PlaceExpr(const Expr& e, float right, float top, int to, size_t row) {
        Node n = CallNode(e);
        n.pos = ImVec2(right - n.size.x, top);
        const int idx = Add(n);
        const Node& made = g_.nodes[static_cast<size_t>(idx)];
        const ImVec2 a = OutSocket(made), b = InSocket(g_.nodes[static_cast<size_t>(to)], row);
        g_.wires.push_back({a, b, ImGui::GetColorU32(TypeColor(made.outType == '?' ? g_.nodes[static_cast<size_t>(to)].rows[row].type : made.outType)), false});
        const float height = made.size.y, left = made.pos.x; // read before Feed: it adds nodes, and the vector may move (`made` would dangle: a crash on Windows)
        const float fed = Feed(e.args, idx, left - gapX, top);
        return std::max(height, fed);
    }

    // A statement's own node (not placed yet), and the expressions feeding its rows.
    Node StmtNode(const Stmt& s, std::vector<const Expr*>& feeds) const {
        Node n;
        n.line = s.line;
        n.flowIn = true;
        switch (s.kind) {
        case Stmt::Call:
            n = CallNode(s.expr);
            n.flowIn = true;
            for (const Expr& a : s.expr.args) feeds.push_back(&a);
            break;
        case Stmt::Assign: {
            n.kind = Node::Assign;
            n.title = "Set " + s.target;
            Row r{"value", std::string(), '?', s.expr.kind == Expr::Call};
            if (!r.wired) r.value = Short(s.expr.kind == Expr::String ? "\"" + s.expr.text + "\"" : s.expr.text);
            n.rows.push_back(r);
            feeds.push_back(&s.expr);
            break;
        }
        case Stmt::If:
            n.kind = Node::Control;
            n.title = s.conditions.size() > 1 ? "If (all of)" : "If";
            for (size_t i = 0; i < s.conditions.size(); ++i) {
                const Expr& c = s.conditions[i];
                Row r{s.conditions.size() > 1 ? "condition " + std::to_string(i + 1) : "condition", std::string(), 'f', c.kind == Expr::Call};
                if (!r.wired) r.value = Short(c.text);
                n.rows.push_back(r);
                feeds.push_back(&c);
            }
            if (s.conditions.empty()) n.rows.push_back({"(always)", std::string(), '?', false});
            n.branches.push_back("then");
            if (s.hasElse) n.branches.push_back("else");
            break;
        case Stmt::Loop: {
            n.kind = Node::Control;
            n.title = mobscript::LowerCase(s.expr.text) == "forif" ? "For each (where)" : "For each";
            static const char* const labels[] = {"each", "in", "where"};
            for (size_t i = 0; i < s.expr.args.size(); ++i) {
                const Expr& a = s.expr.args[i];
                Row r{i < 3 ? labels[i] : "", std::string(), i == 1 ? 'g' : i == 0 ? 'o' : 'f', a.kind == Expr::Call};
                if (!r.wired) r.value = Short(a.text);
                n.rows.push_back(r);
                feeds.push_back(&a);
            }
            n.branches.push_back("body");
            break;
        }
        case Stmt::Comment: {
            n.kind = Node::Comment;
            n.title = "//";
            n.flowIn = false;
            // the comment's text in rows of at most ~40 characters
            std::string line, word;
            std::istringstream in(s.target);
            while (in >> word) {
                if (!line.empty() && line.size() + word.size() > 40) { n.rows.push_back({line, std::string(), '?', false}); line.clear(); }
                line += (line.empty() ? "" : " ") + word;
            }
            if (!line.empty() || n.rows.empty()) n.rows.push_back({line, std::string(), '?', false});
            break;
        }
        }
        n.size = NodeSize(n);
        return n;
    }

    float StmtInputsWidth(const Stmt& s) const {
        std::vector<const Expr*> feeds;
        StmtNode(s, feeds);
        float w = 0;
        for (const Expr* e : feeds) if (e->kind == Expr::Call) w = std::max(w, SubtreeWidth(*e));
        return w > 0 ? w + gapX : 0;
    }
    float ChainInputsWidth(const std::vector<Stmt>& v) const {
        float w = 0;
        for (const Stmt& s : v) w = std::max(w, StmtInputsWidth(s));
        return w;
    }

    // Statements down a column whose inputs start at `left`, from `y`: the first one is reached from node
    // `prev` (down its spine) or, when `branchOf` >= 0, from branch `branch` of node `branchOf`. The bottom.
    float Chain(const std::vector<Stmt>& v, float left, float y, int prev, int branchOf, size_t branch = 0) {
        const float x = left + ChainInputsWidth(v);
        float bottom = y;
        for (const Stmt& s : v) {
            std::vector<const Expr*> feeds;
            Node n = StmtNode(s, feeds);
            n.pos = ImVec2(x, y);
            const int idx = Add(n);
            const Node& made = g_.nodes[static_cast<size_t>(idx)];
            // reached from the previous statement, or from the branch that starts this column
            const ImU32 flow = IM_COL32(230, 230, 230, 200);
            if (made.kind != Node::Comment) {
                if (prev >= 0) {
                    const Node& p = g_.nodes[static_cast<size_t>(prev)];
                    g_.wires.push_back({ImVec2(p.pos.x + F, p.pos.y + p.size.y), ImVec2(made.pos.x + F, made.pos.y), flow, true});
                } else if (branchOf >= 0) {
                    g_.wires.push_back({BranchSocket(g_.nodes[static_cast<size_t>(branchOf)], branch), FlowIn(made), flow, false});
                }
                prev = idx;
                branchOf = -1;
            }
            // the calls feeding its rows, on the left
            float fed = 0, fy = y;
            for (size_t i = 0; i < feeds.size(); ++i) {
                if (feeds[i]->kind != Expr::Call) continue;
                const float h = PlaceExpr(*feeds[i], x - gapX, fy, idx, i);
                fy += h + gapY * 0.6f;
                fed = fy - y - gapY * 0.6f;
            }
            float rowBottom = y + std::max(g_.nodes[static_cast<size_t>(idx)].size.y, fed);
            // the branches, in a column on its right
            const Node& placed = g_.nodes[static_cast<size_t>(idx)];
            const float bx = placed.pos.x + placed.size.x + gapX;
            if (s.kind == Stmt::If) {
                float by = Chain(s.then, bx, y, -1, idx, 0);
                if (s.hasElse) by = Chain(s.otherwise, bx, by + gapY, -1, idx, 1);
                rowBottom = std::max(rowBottom, by);
            } else if (s.kind == Stmt::Loop) {
                rowBottom = std::max(rowBottom, Chain(s.body, bx, y, -1, idx, 0));
            }
            bottom = rowBottom;
            y = rowBottom + gapY;
        }
        return bottom;
    }
};

} // namespace nodegraph

static void EditScriptAt(App& app, const mob::File& f, int line) {
    app.scriptEditing = true;
    app.scriptEditFor = f.path;
    app.scriptEdit = mob::Utf8(f.script);
    app.scriptEditFocus = true;
    app.scriptMessage.clear();
    app.scriptEditGotoLine = std::max(0, line - 1);
}

static void ScriptNodes(App& app, const mob::File& f, const std::string& utf8, size_t hash, const ScriptNames& names) {
    using namespace nodegraph;
    // The tree and its graph, built again when the text, the commands (EI ATD's) or the font size change.
    struct Cache { const mob::File* file = nullptr; size_t hash = 0; bool atd = false; float font = 0; scriptast::Program prog; Graph g; float headerH = 0, rowH = 0, pad = 0; };
    static Cache c;
    if (c.file != &f || c.hash != hash || c.atd != scriptcmds::AtdActive() || c.font != ImGui::GetFontSize()) {
        c.file = &f;
        c.hash = hash;
        c.atd = scriptcmds::AtdActive();
        c.font = ImGui::GetFontSize();
        c.prog = scriptast::Parse(utf8);
        Builder b(c.prog, names);
        c.g = b.Build();
        c.headerH = b.headerH;
        c.rowH = b.rowH;
        c.pad = b.pad;
    }
    const Graph& g = c.g;
    const float F = c.font;
    View& view = app.nodeViews[f.path];

    std::unordered_map<int, char> severity;
    for (const checks::Finding& fd : app.findings)
        if (fd.file == app.scriptFile && fd.line > 0) { char& s = severity[fd.line]; if (!s || fd.severity == 'E') s = fd.severity; }

    if (c.prog.errorLine)
        ui::Note("Shown up to line " + std::to_string(c.prog.errorLine) + ", where the script cannot be read: " + c.prog.error +
                 " (the text view and the Checks tab tell more).");
    // The toolbar: go to a script, frame everything, the zoom.
    ImVec2 focus(-1, -1); // a canvas point to bring to the view's top left (or its centre: focusCentre)
    bool focusCentre = false;
    if (ImGui::SmallButton("Frame all")) {
        view.zoom = 1.0f;
        view.placed = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("100%")) { view.zoom = 1.0f; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x));
    const std::string gotoLabel = std::to_string(g.frames.size()) + " " + i18n::Tr("scripts: go to...");
    if (ImGui::BeginCombo("##nodegoto", gotoLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        const i18n::Verbatim verbatim;
        for (size_t i = 0; i < g.frames.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(g.frames[i].name.c_str())) focus = g.frames[i].min;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (app.nodeGoto.size()) { // a script call was clicked last frame
        for (const Frame& fr : g.frames) if (fr.lower == app.nodeGoto) focus = fr.min;
        app.nodeGoto.clear();
    }
    if (app.scrollToLine && app.scriptLine > 0) { // the Checks tab's line: its node in the middle
        const Node* best = nullptr;
        for (const Node& n : g.nodes) if (n.line >= app.scriptLine && (!best || n.line < best->line)) best = &n;
        if (best) { focus = ImVec2(best->pos.x + best->size.x / 2, best->pos.y + best->size.y / 2); focusCentre = true; app.nodeFlashLine = best->line; }
        app.scrollToLine = false;
    }

    // The canvas: any button drags it, the wheel zooms where the mouse is.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(50.0f, ImGui::GetContentRegionAvail().x), std::max(50.0f, ImGui::GetContentRegionAvail().y));
    ImGui::InvisibleButton("##nodecanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    if (!view.placed) { // the whole width of the graph in view (not smaller than half size), its top at the top
        const float w = g.max.x - g.min.x;
        view.zoom = w > 0 ? std::clamp((size.x - 2 * F) / w, 0.5f, 1.0f) : 1.0f;
        view.pan = ImVec2(F - g.min.x * view.zoom, F - g.min.y * view.zoom);
        view.placed = true;
    }
    if (focus.x > -1e8f && focus.x != -1) {
        view.pan = focusCentre ? ImVec2(size.x / 2 - focus.x * view.zoom, size.y / 2 - focus.y * view.zoom)
                               : ImVec2(F - focus.x * view.zoom, F - focus.y * view.zoom);
    }
    static bool dragged = false;
    if (ImGui::IsItemActivated()) dragged = false;
    if (active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2))) {
        view.pan = ImVec2(view.pan.x + io.MouseDelta.x, view.pan.y + io.MouseDelta.y);
        dragged = true;
    }
    if (hovered && io.MouseWheel != 0) {
        const float z = std::clamp(view.zoom * std::pow(1.15f, io.MouseWheel), 0.15f, 2.5f);
        const ImVec2 m(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        const ImVec2 at((m.x - view.pan.x) / view.zoom, (m.y - view.pan.y) / view.zoom);
        view.zoom = z;
        view.pan = ImVec2(m.x - at.x * z, m.y - at.y * z);
    }
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Home, false)) view.placed = false;
    const float z = view.zoom;
    auto S = [&](ImVec2 p) { return ImVec2(origin.x + view.pan.x + p.x * z, origin.y + view.pan.y + p.y * z); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(29, 29, 29, 255));
    { // the grid
        const float step = F * 2 * z;
        if (step > 6) {
            const float sx = std::fmod(view.pan.x, step), sy = std::fmod(view.pan.y, step);
            for (float x = sx; x < size.x; x += step) dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(40, 40, 40, 255));
            for (float y = sy; y < size.y; y += step) dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(40, 40, 40, 255));
        }
    }
    const char* const emptyText = i18n::Tr("Nothing in this part of the graph");
    const char* const nearestText = i18n::Tr("Go to the nearest node");
    const i18n::Verbatim verbatim; // everything drawn below is the script's own words
    ImFont* font = ImGui::GetFont();
    const float fs = F * z;
    const bool text = fs >= 5.5f;
    const ImRect clip(origin, ImVec2(origin.x + size.x, origin.y + size.y));
    auto visible = [&](ImVec2 a, ImVec2 b) { return b.x >= clip.Min.x && a.x <= clip.Max.x && b.y >= clip.Min.y && a.y <= clip.Max.y; };
    // What is in view, from the grid: the work of a frame follows the window, not the script's size.
    static std::vector<int> inNodes, inWires;
    g.Query(ImVec2((0 - view.pan.x) / z, (0 - view.pan.y) / z), ImVec2((size.x - view.pan.x) / z, (size.y - view.pan.y) / z), F, inNodes, inWires);
    // Zoomed out, the detail goes: the text of the rows (below ~8 px), the rounded corners and the curves (every
    // curve is dozens of segments, each a few vertices), the lines' anti-aliasing: tens of thousands of vertices.
    const bool rowText = fs >= 8.0f, simple = z < 0.3f;
    const ImDrawListFlags dlFlags = dl->Flags;
    if (simple) dl->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    // frames
    for (const Frame& fr : g.frames) {
        const ImVec2 a = S(fr.min), b = S(fr.max);
        if (!visible(a, b)) continue;
        dl->AddRectFilled(a, b, IM_COL32(60, 60, 70, 90), 4 * z);
        dl->AddRect(a, b, IM_COL32(90, 90, 105, 160), 4 * z);
        if (text) {
            dl->AddText(font, fs * 1.15f, ImVec2(a.x + F * 0.6f * z, a.y + F * 0.4f * z), IM_COL32(220, 200, 230, 255), fr.name.c_str());
            if (!fr.subtitle.empty()) {
                const float tw = font->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, fr.name.c_str()).x;
                dl->AddText(font, fs, ImVec2(a.x + F * 0.6f * z + tw + F * z, a.y + F * 0.55f * z), IM_COL32(150, 150, 160, 255), fr.subtitle.c_str());
            }
        }
    }
    // the node under the mouse, found before the wires: the calls to and from it are drawn brighter
    const Node* hover = nullptr;
    if (hovered) {
        const int at = g.NodeAt(ImVec2((io.MousePos.x - origin.x - view.pan.x) / z, (io.MousePos.y - origin.y - view.pan.y) / z));
        if (at >= 0) hover = &g.nodes[static_cast<size_t>(at)];
    }
    const int hoverIndex = hover ? static_cast<int>(hover - g.nodes.data()) : -1;
    // wires
    int drawn = 0; // the nodes that really reached the canvas (the grid also lists what only has its bounding box in view, and a call's wire can cross an empty part)
    for (int wi : inWires) {
        const Wire& w = g.wires[static_cast<size_t>(wi)];
        const ImVec2 a = S(w.a), b = S(w.b);
        if (w.call) { // a script call to its script's entry: out to the right, round, in from the left
            const float d = std::max(F * 4 * z, std::fabs(b.x - a.x) * 0.5f + std::fabs(b.y - a.y) * 0.15f);
            if (!visible(ImVec2(std::min(a.x, b.x) - d, std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x) + d, std::max(a.y, b.y)))) continue;
            const bool lit = w.from == hoverIndex || w.to == hoverIndex;
            BezierInView(dl, clip, lit ? IM_COL32(255, 175, 80, 255) : w.color, std::max(1.0f, (lit ? 3.0f : 1.8f) * z))
                .Draw(a, ImVec2(a.x + d, a.y), ImVec2(b.x - d, b.y), b);
            continue;
        }
        if (!visible(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)))) continue;
        if (std::fabs(b.x - a.x) + std::fabs(b.y - a.y) < 2.0f) continue; // under two pixels
        if (simple) { dl->AddLine(a, b, w.color, 1.0f); continue; }
        if (w.flow) { // down the spine
            const float d = std::max(4.0f, (b.y - a.y) * 0.5f);
            BezierInView(dl, clip, w.color, std::max(1.0f, 2.0f * z)).Draw(a, ImVec2(a.x, a.y + d), ImVec2(b.x, b.y - d), b);
        } else {
            const float d = std::max(F * z, std::fabs(b.x - a.x) * 0.5f);
            BezierInView(dl, clip, w.color, std::max(1.0f, 1.6f * z)).Draw(a, ImVec2(a.x + d, a.y), ImVec2(b.x - d, b.y), b);
        }
    }
    // nodes
    const float r = std::max(2.0f, F * 0.28f * z), round = 4 * z;
    const double now = ImGui::GetTime();
    if (app.nodeFlashLine && app.nodeFlashUntil < now) { app.nodeFlashUntil = now + 2.0; }
    const float corner = round >= 2.0f ? round : 0.0f; // too small to see: square (a rounded corner is a dozen vertices)
    for (int ni : inNodes) {
        const Node& n = g.nodes[static_cast<size_t>(ni)];
        const ImVec2 a = S(n.pos), b = S(ImVec2(n.pos.x + n.size.x, n.pos.y + n.size.y));
        if (!visible(ImVec2(a.x - r, a.y), ImVec2(b.x + r, b.y))) continue;
        ++drawn;
        const bool comment = n.kind == Node::Comment;
        const ImVec4 hc = HeaderColor(n.kind);
        if (b.x - a.x < 3.0f) { dl->AddRectFilled(a, b, ImGui::GetColorU32(hc)); continue; } // a speck: its colour
        dl->AddRectFilled(a, b, comment ? IM_COL32(38, 46, 36, 235) : IM_COL32(48, 48, 48, 240), corner);
        const float hh = c.headerH * z;
        dl->AddRectFilled(a, ImVec2(b.x, a.y + hh), ImGui::GetColorU32(hc), corner, ImDrawFlags_RoundCornersTop);
        auto sev = severity.find(n.line);
        const bool flash = app.nodeFlashLine == n.line && app.nodeFlashUntil > now;
        const ImU32 border = flash ? IM_COL32(255, 220, 120, 255) : sev != severity.end() ? ImGui::GetColorU32(SeverityColor(sev->second)) : IM_COL32(20, 20, 20, 255);
        dl->AddRect(a, b, border, corner, 0, sev != severity.end() || flash ? std::max(1.5f, 2.5f * z) : 1.0f);
        if (n.flowIn) { // where the flow (or a call) comes in: a small diamond left of the header
            const ImVec2 p(a.x, a.y + hh * 0.5f);
            const float d = r * 1.1f;
            dl->AddQuadFilled(ImVec2(p.x - d, p.y), ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d), IM_COL32(230, 230, 230, 230));
        }
        if (n.out) dl->AddCircleFilled(ImVec2(b.x, a.y + hh * 0.5f), r, ImGui::GetColorU32(TypeColor(n.outType)), r < 4 ? 6 : 0);
        if (n.kind == Node::ScriptCall) { // where the call leaves for the called script
            const ImVec2 p(b.x, a.y + hh * 0.5f);
            const float d = r * 1.2f;
            dl->AddQuadFilled(ImVec2(p.x - d, p.y), ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d), IM_COL32(240, 160, 70, 255));
        }
        if (!text) continue;
        dl->AddText(font, fs, ImVec2(a.x + c.pad * z, a.y + (hh - fs) * 0.5f), IM_COL32(235, 235, 235, 255), n.title.c_str());
        if (!rowText) continue;
        for (size_t i = 0; i < n.rows.size(); ++i) {
            const Row& row = n.rows[i];
            const float cy = a.y + hh + c.rowH * z * (static_cast<float>(i) + 0.5f);
            if (n.kind != Node::Entry && !comment && !(n.kind == Node::Control && row.label == "(always)"))
                dl->AddCircleFilled(ImVec2(a.x, cy), r, ImGui::GetColorU32(TypeColor(row.type)));
            const std::string label = row.wired || row.value.empty() ? row.label : (row.label.empty() ? row.value : row.label + ": ");
            const ImVec2 tp(a.x + c.pad * z + (comment ? 0 : r), cy - fs * 0.5f);
            const ImU32 tc = comment ? IM_COL32(150, 190, 140, 255) : row.type == 'w' ? IM_COL32(240, 200, 90, 255) : row.type == 'n' ? IM_COL32(150, 150, 160, 255)
                           : n.kind == Node::Entry ? IM_COL32(180, 180, 190, 255) : IM_COL32(200, 200, 200, 255);
            dl->AddText(font, fs, tp, tc, label.c_str());
            if (!row.wired && !row.value.empty() && !row.label.empty()) {
                const float lw = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str()).x;
                const ImVec4 vc = row.value[0] == '"' ? ImVec4(0.90f, 0.64f, 0.44f, 1) : scripthl::TokenColor(row.value, names);
                dl->AddText(font, fs, ImVec2(tp.x + lw, tp.y), ImGui::GetColorU32(vc), row.value.c_str());
            }
        }
        for (size_t k = 0; k < n.branches.size(); ++k) {
            const float cy = a.y + hh + c.rowH * z * (static_cast<float>(n.rows.size() + k) + 0.5f);
            const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, n.branches[k].c_str()).x;
            dl->AddText(font, fs, ImVec2(b.x - c.pad * z - r - tw, cy - fs * 0.5f), IM_COL32(220, 220, 220, 255), n.branches[k].c_str());
            const float d = r * 1.1f;
            dl->AddQuadFilled(ImVec2(b.x - d, cy), ImVec2(b.x, cy - d), ImVec2(b.x + d, cy), ImVec2(b.x, cy + d), IM_COL32(230, 230, 230, 230));
        }
    }
    if (hover) dl->AddRect(S(hover->pos), S(ImVec2(hover->pos.x + hover->size.x, hover->pos.y + hover->size.y)), IM_COL32(255, 255, 255, 220), corner, 0, 1.5f);
    dl->Flags = dlFlags;
    if (drawn == 0 && !g.nodes.empty()) { // dragged away from everything: say so, and a way back
        const ImVec2 ts = ImGui::CalcTextSize(emptyText);
        const ImVec2 c(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y - 8.0f), IM_COL32(190, 190, 190, 255), emptyText);
        const ImVec2 bs(ImGui::CalcTextSize(nearestText).x + 24.0f, ImGui::GetFrameHeight()), bp(c.x - bs.x * 0.5f, c.y + 2.0f); // a button drawn by hand: the canvas already owns the mouse
        const bool over = hovered && ImGui::IsMouseHoveringRect(bp, ImVec2(bp.x + bs.x, bp.y + bs.y));
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), over ? IM_COL32(75, 100, 140, 255) : IM_COL32(55, 70, 100, 255), 3.0f);
        dl->AddText(ImVec2(bp.x + 12.0f, bp.y + (bs.y - ts.y) * 0.5f), IM_COL32(235, 235, 235, 255), nearestText);
        if (over && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragged) {
            const float cx = (size.x * 0.5f - view.pan.x) / z, cy = (size.y * 0.5f - view.pan.y) / z;
            float best = 1e30f; ImVec2 at(cx, cy);
            for (const Node& n : g.nodes) {
                const ImVec2 m(n.pos.x + n.size.x * 0.5f, n.pos.y + n.size.y * 0.5f);
                const float d = (m.x - cx) * (m.x - cx) + (m.y - cy) * (m.y - cy);
                if (d < best) { best = d; at = m; }
            }
            view.pan = ImVec2(size.x * 0.5f - at.x * z, size.y * 0.5f - at.y * z);
        }
    }
    dl->PopClipRect();
    if (text == false && hovered) ImGui::SetTooltip("%s", i18n::Tr("Zoom in to read the nodes (mouse wheel)"));

    // The node under the mouse: what it is and the findings of its line; a script call goes to its frame.
    if (hover) {
        std::string tip;
        auto fn = mobscript::FunctionTable().find(mobscript::LowerCase(hover->title));
        if (hover->kind == Node::Command || hover->kind == Node::Function) tip = fn != mobscript::FunctionTable().end() ? Signature(*fn->second) : hover->title + i18n::Tr(": neither a known command nor a script of this map");
        else if (hover->kind == Node::ScriptCall) tip = "script " + hover->title + i18n::Tr(": click to go to it");
        else if (hover->kind == Node::Entry) {
            tip = hover->title;
            for (const Row& row : hover->rows) if (row.type == 'n' || row.type == 'w') tip += "\n" + row.label;
            if (hover->flowIn) tip += std::string("\n") + i18n::Tr("The orange wires come from the calls to it.");
        }
        else if (hover->kind == Node::Comment) for (const Row& row : hover->rows) tip += (tip.empty() ? "// " : " ") + row.label;
        else tip = hover->title;
        for (const Row& row : hover->rows) // values shortened in the node: whole here
            if (!row.full.empty()) tip += "\n" + (row.label.empty() ? std::string() : row.label + ": ") + row.full;
        for (const checks::Finding& fd : app.findings)
            if (fd.file == app.scriptFile && fd.line == hover->line) tip += "\n" + std::string(1, fd.severity) + ": " + fd.message;
        tip += "\n" + std::string(i18n::Tr("Line ")) + std::to_string(hover->line) + i18n::Tr(". Double-click: edit the text here.");
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(tip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragged && hover->kind == Node::ScriptCall) app.nodeGoto = hover->target;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) EditScriptAt(app, f, hover->line);
    }
}

void ScriptContent(App& app) {
    if (app.mobs.empty()) { ImGui::TextDisabled("(no maps loaded)"); return; }
    app.scriptFile = std::min(std::max(app.scriptFile, 0), static_cast<int>(app.mobs.size()) - 1);
    const char* detach = app.scriptWindow ? "Back to the tab" : "Open in a window";
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(detach).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##scriptfile", app.mobs[app.scriptFile]->file.fileName.c_str())) {
        for (size_t i = 0; i < app.mobs.size(); ++i)
            if (ImGui::Selectable(app.mobs[i]->file.fileName.c_str(), static_cast<int>(i) == app.scriptFile)) {
                app.scriptFile = static_cast<int>(i);
                app.scriptLine = 0;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(detach)) app.scriptWindow = !app.scriptWindow;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The script in a window of its own, beside the program's (moved, resized and placed by the desktop)");
    MobEntry& entry = *app.mobs[app.scriptFile];
    const mob::File& f = entry.file;
    if (!f.hasScript) { ImGui::TextDisabled("This map has no mission script."); return; }
    // Editing: the whole text, applied as one undoable change (Ctrl+S applies and saves).
    if (app.scriptEditing && app.scriptEditFor != f.path) app.scriptEditing = false;
    if (app.scriptEditing) {
        const ImGuiID editId = ImGui::GetID("##scriptedit");
        char childName[512];
        std::snprintf(childName, sizeof(childName), "%s/%s_%08X", ImGui::GetCurrentWindow()->Name, "##scriptedit", editId);
        auto leaveAt = [&]() { // the view then shows the lines the edit box showed
            if (ImGuiWindow* box = ImGui::FindWindowByName(childName)) app.scriptViewGotoLine = static_cast<int>(box->Scroll.y / ImGui::GetFontSize() + 0.5f);
        };
        if (ImGui::Button("Apply")) { leaveAt(); ApplyScriptEdit(app); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the text in the map (undoable); %s also applies it and saves", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { leaveAt(); app.scriptEditing = false; app.scriptMessage.clear(); }
        ImGui::SameLine();
        const bool changed = mob::Utf8(f.script) != app.scriptEdit;
        ImGui::TextDisabled("line %d%s", app.scriptCursorLine, changed ? "  (not applied)" : "");
        if (!app.scriptMessage.empty()) ImGui::TextDisabled("%s", app.scriptMessage.c_str());
        App::Completion& c = app.completion;
        { // the call around the cursor, else the picked suggestion's signature, else how the list works
            std::string hint = c.signature;
            if (!c.items.empty() && c.index < static_cast<int>(c.items.size())) {
                auto it = mobscript::FunctionTable().find(mobscript::LowerCase(c.items[static_cast<size_t>(c.index)]));
                if (it != mobscript::FunctionTable().end()) hint = Signature(*it->second);
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", hint.empty() ? "Tab: complete (or indent), Up/Down: pick, Esc: hide the list" : hint.c_str());
            ImGui::PopTextWrapPos();
        }
        if (app.scriptEditFocus) { ImGui::SetKeyboardFocusHere(); app.scriptEditFocus = false; }
        const bool editing = ImGui::GetActiveID() == editId;
        // The list takes Up / Down / Enter / Esc from the text while it shows; Tab always (complete or indent).
        const ImGuiID owner = ImGui::GetID("##scriptcompletion");
        if (editing) {
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) c.tab = true;
            ImGui::SetKeyOwner(ImGuiKey_Tab, owner, ImGuiInputFlags_LockThisFrame);
            // Esc would undo everything typed since the text box took the focus: it only hides the list.
            if (c.items.empty()) ImGui::SetKeyOwner(ImGuiKey_Escape, owner, ImGuiInputFlags_LockThisFrame);
            if (!c.items.empty()) {
                const int n = static_cast<int>(c.items.size());
                // The text box owns Up / Down: it moves its cursor, which the callback puts back.
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, editId)) { c.index = (c.index + 1) % n; c.keepCursor = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, editId)) { c.index = (c.index + n - 1) % n; c.keepCursor = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) c.accept = true;
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { c.dismissed = true; c.items.clear(); }
                for (ImGuiKey k : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
                    ImGui::SetKeyOwner(k, owner, ImGuiInputFlags_LockThisFrame);
            }
        }
        const ImGuiWindow* parent = ImGui::GetCurrentWindow();
        // The text box draws its text invisibly on a dark neutral ground; the coloured text is painted
        // over it below, at the same places (only the lines in view).
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.105f, 0.105f, 0.115f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.30f, 0.45f, 0.75f, 0.45f));
        ImGui::InputTextMultiline("##scriptedit", app.scriptEdit.data(), app.scriptEdit.capacity() + 1, ImVec2(-1, -1),
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways, ScriptEditCallback, &app);
        ImGui::PopStyleColor(6);
        const bool boxHovered = ImGui::IsItemHovered();
        ImGuiWindow* box = ImGui::FindWindowByName(childName);
        if (!box) return;
        (void)parent;
        if (app.scriptEditScrollLine >= 0) { // the line that was at the top of the view stays at the top
            ImGui::SetScrollY(box, app.scriptEditScrollLine * ImGui::GetFontSize());
            app.scriptEditScrollLine = -1;
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        {
            // The names the text declares, found again at most twice a second while it changes.
            static ScriptNames names;
            static size_t namesFor = 0;
            static double namesAt = -10.0;
            const size_t hash = std::hash<std::string_view>{}(std::string_view(app.scriptEdit.data(), std::strlen(app.scriptEdit.c_str())));
            if (hash != namesFor && ImGui::GetTime() - namesAt > 0.5) {
                std::vector<uint8_t> cp;
                std::string err;
                codec::FromUtf8(app.scriptEdit.c_str(), codec::Encoding::Cp1251, cp, err);
                names = NamesOf(std::string(cp.begin(), cp.end()));
                namesFor = hash;
                namesAt = ImGui::GetTime();
            }
            const float lineH = ImGui::GetFontSize();
            const ImVec2 origin(box->Pos.x + style.FramePadding.x - box->Scroll.x, box->Pos.y + style.FramePadding.y - box->Scroll.y);
            const int firstLine = std::max(0, static_cast<int>((box->Scroll.y - style.FramePadding.y) / lineH) - 1);
            const int lastLine = firstLine + static_cast<int>(box->Size.y / lineH) + 3;
            ImDrawList* dl = box->DrawList;
            dl->PushClipRect(box->InnerClipRect.Min, box->InnerClipRect.Max, true);
            // The findings of the last check, by line (the applied text's numbering: lines shift while editing).
            std::unordered_map<int, char> flagged;
            for (const checks::Finding& fd : app.findings)
                if (fd.file == app.scriptFile && fd.line > 0) { char& sev = flagged[fd.line]; if (!sev || fd.severity == 'E') sev = fd.severity; }
            const char* text = app.scriptEdit.c_str();
            const char* p = text;
            for (int line = 0; *p && line <= lastLine; ++line) {
                const char* e = p;
                while (*e && *e != '\n') ++e;
                if (line >= firstLine) {
                    float x = origin.x;
                    const float y = origin.y + line * lineH;
                    auto flag = flagged.find(line + 1);
                    if (flag != flagged.end()) { // a tinted band and a bar in the finding's colour, as the view draws them
                        const ImVec4 col = SeverityColor(flag->second);
                        const ImVec2 rowMin(box->Pos.x, y), rowMax(box->Pos.x + box->Size.x, y + lineH);
                        dl->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.12f)));
                        dl->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), ImGui::GetColorU32(col));
                        if (boxHovered && ImGui::IsMouseHoveringRect(rowMin, rowMax)) {
                            std::string tip;
                            for (const checks::Finding& fd : app.findings)
                                if (fd.file == app.scriptFile && fd.line == line + 1) tip += std::string(1, fd.severity) + ": " + fd.message + "\n";
                            ImGui::SetTooltip("%s(line numbers of the last check: apply the text to check it again)", tip.c_str());
                        }
                    }
                    EachScriptToken(p, (e > p && e[-1] == '\r') ? e - 1 : e, names, [&](const char* b, const char* t, const ImVec4& color) {
                        dl->AddText(ImVec2(x, y), ImGui::GetColorU32(color), b, t);
                        x += ImGui::CalcTextSize(b, t, false).x;
                    });
                }
                p = *e ? e + 1 : e;
            }
            dl->PopClipRect();
        }
        if (!editing || c.items.empty()) return;
        // The list under the cursor (measured and written as the script's text: not translated).
        const i18n::Verbatim verbatim;
        const float x = ImGui::CalcTextSize(c.lineBeforeCursor.c_str()).x - ImGui::CalcTextSize(c.prefix.c_str()).x;
        ImVec2 at(box->Pos.x + style.FramePadding.x - box->Scroll.x + x, box->Pos.y + style.FramePadding.y - box->Scroll.y + (c.line + 1) * ImGui::GetFontSize() + 2);
        ImGui::SetNextWindowPos(at);
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::Begin("##completion", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_Tooltip);
        for (size_t i = 0; i < c.items.size(); ++i) {
            const std::string& w = c.items[i];
            auto fn = mobscript::FunctionTable().find(mobscript::LowerCase(w));
            const bool picked = static_cast<int>(i) == c.index;
            const std::string text = fn != mobscript::FunctionTable().end() ? Signature(*fn->second) : w;
            if (picked) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.55f, 1.0f), "> %s", text.c_str());
            else ImGui::Text("  %s", text.c_str());
        }
        ImGui::End();
        return;
    }
    if (ImGui::Button("Edit")) {
        app.scriptEditing = true;
        app.scriptEditFor = f.path;
        app.scriptEdit = mob::Utf8(f.script);
        app.scriptEditFocus = true;
        app.scriptMessage.clear();
        app.scriptEditGotoLine = static_cast<int>(app.scriptViewScrollY / ImGui::GetTextLineHeightWithSpacing() + 0.5f); // stay where the view was
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in external editor")) OpenScriptExternally(app, entry);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens a copy (.eis, UTF-8) in the system's editor; each save there comes back into\n"
            "the map as an undoable change. The map itself is written with %s.", ui::BindName(app.lib.mapKeys[config::kKeySave]).c_str());
    if (!app.scriptExtPath.empty() && app.scriptExtMob == f.path) {
        ImGui::SameLine();
        ImGui::TextDisabled("watching %s", app.scriptExtPath.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy the path");
        if (ImGui::IsItemClicked()) ImGui::SetClipboardText(app.scriptExtPath.c_str());
    }
    if (ImGui::RadioButton("Text", !app.scriptVisual)) app.scriptVisual = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Nodes", app.scriptVisual)) app.scriptVisual = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The script as nodes, like Blender's node editor: each script a frame, its statements down a line, if / for each\n"
                          "branching to the right, a command's arguments wired in from the commands computing them.\n"
                          "Drag: move the view, wheel: zoom, Home: everything. Hover a node: what it takes and its line's findings;\n"
                          "click a script call: go to that script; double-click: edit the text there. More room: Open in a window.");
    if (!app.scriptMessage.empty()) ImGui::TextDisabled("%s", app.scriptMessage.c_str());
    if (f.script.empty()) { ImGui::TextDisabled("The script is empty."); return; }
    // Lines as the checker counts them (every '\n').
    static const mob::File* cachedFor = nullptr;
    static std::vector<std::pair<size_t, size_t>> lines;
    static std::string text;
    static size_t cachedHash = 0;
    static ScriptNames names;
    const size_t hash = std::hash<std::string>{}(f.script);
    if (cachedFor != &f || cachedHash != hash) {
        cachedFor = &f;
        cachedHash = hash;
        text = mob::Utf8(f.script);
        names = NamesOf(f.script);
        lines.clear();
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                size_t end = i;
                if (end > start && text[end - 1] == '\r') --end;
                lines.push_back({start, end});
                start = i + 1;
            }
        }
    }
    if (app.scriptVisual) { ScriptNodes(app, f, text, cachedHash, names); return; }
    ImGui::TextDisabled("%zu lines - the same numbering as the checks and um-multitool mobdump's .eis", lines.size());
    ImGui::BeginChild("##script", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    if (app.scrollToLine && app.scriptLine > 0) {
        ImGui::SetScrollY(std::max(0.0f, (app.scriptLine - 6) * lineHeight));
        app.scrollToLine = false;
    } else if (app.scriptViewGotoLine >= 0) { // back from edit mode: the same lines in view
        ImGui::SetScrollY(app.scriptViewGotoLine * lineHeight);
        app.scriptViewGotoLine = -1;
    }
    app.scriptViewScrollY = ImGui::GetScrollY();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines.size()), lineHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            bool marked = i + 1 == app.scriptLine;
            bool flagged = false;
            char severity = 0;
            for (const checks::Finding& fd : app.findings)
                if (fd.file == app.scriptFile && fd.line == i + 1) { flagged = true; if (!severity || fd.severity == 'E') severity = fd.severity; }
            // The line the checks jumped to gets a yellow band, lines with findings a tinted band and a bar
            // in the finding's colour; the text keeps its highlighting.
            ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x + ImGui::GetScrollMaxX() + 4000.0f, rowMin.y + ImGui::GetTextLineHeight());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (marked) draw->AddRectFilled(rowMin, rowMax, IM_COL32(255, 220, 120, 40));
            if (flagged) {
                ImVec4 c = SeverityColor(severity);
                draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.12f)));
                draw->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), ImGui::GetColorU32(c));
            }
            ImGui::TextDisabled("%5d", i + 1);
            ImGui::SameLine();
            ImGui::BeginGroup();
            HighlightedLine(text.data() + lines[i].first, text.data() + lines[i].second, names);
            ImGui::EndGroup();
            if (flagged && ImGui::IsItemHovered()) {
                std::string tip;
                for (const checks::Finding& fd : app.findings)
                    if (fd.file == app.scriptFile && fd.line == i + 1) tip += std::string(1, fd.severity) + ": " + fd.message + "\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
    }
    ImGui::EndChild();
}


} // namespace mapedit
