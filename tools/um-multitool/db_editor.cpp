// The DB sub-tab: a small spreadsheet editor for the gameplay databases. See db_editor.hpp.
//
// The database is a dbmodel::Book (sheets of cells, as DBEditor lays them out: titles on row 2, FLDx-y
// markers on row 3, records from row 4, column A empty). Every change runs the checks again, so the
// cells and rows with problems stay highlighted; new problems raise an alert (alerts.hpp).

#include "db_editor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "imgui.h"
#include "imgui_internal.h" // ImGuiInputTextState: reloading the text after a completion

#include "alerts.hpp"
#include "db_model.hpp"

namespace fs = std::filesystem;

namespace dbedit {

namespace {

constexpr int kFirstRow = 4, kFirstCol = 2; // records start at B4
const ImVec4 kErrorColor(0.95f, 0.42f, 0.38f, 1), kWarningColor(0.95f, 0.78f, 0.35f, 1);

// One undo step: a cell's value before and after, or a whole sheet before and after (row changes).
struct Change {
    int sheet = 0;
    int row = 0, col = 0;
    std::optional<sheetio::Cell> before, after;
    std::optional<sheetio::Sheet> sheetBefore, sheetAfter;
};

struct State {
    Hooks hooks;
    char path[1024] = "";
    char savePath[1024] = "";
    char resPath[1024] = "";
    dbmodel::Book book;
    std::string loadedPath;
    bool loaded = false, dirty = false;
    int sheet = 0;
    int version = 0; // bumped by every change (the shown rows are recomputed)

    // Checks
    std::vector<dbmodel::Issue> issues;
    int errors = 0, warnings = 0;
    int alertedErrors = 0, alertedWarnings = 0;
    std::vector<std::map<std::pair<int, int>, int>> cellIssue; // per sheet: (row, col) -> 0 error / 1 warning
    std::vector<std::map<int, int>> rowIssue;                  // per sheet: row -> 0 / 1
    std::vector<std::pair<int, int>> sheetCounts;              // per sheet: errors, warnings
    double checkMs = 0;
    bool showErrors = true, showWarnings = true, issuesOfSheetOnly = false;

    // The table
    int selRow = -1, selCol = -1;
    bool editing = false, startEdit = false;
    char editBuf[8192] = "";
    bool scrollToSelection = false;
    char filter[256] = "";
    bool onlyProblems = false;
    std::vector<int> rows; // shown rows of the current sheet
    int rowsVersion = -1, rowsSheet = -1;
    std::string rowsFilter;
    bool rowsOnlyProblems = false;

    // Autocompletion of the cell being typed into (dbmodel::Completer, rebuilt after changes)
    std::unique_ptr<dbmodel::Completer> completer;
    int completerVersion = -1;
    dbmodel::Completion completion;
    std::string completionFor; // the text before the cursor it was made for
    int cursor = 0;            // the cursor in editBuf (bytes)
    int acSelected = 0;        // the highlighted suggestion
    bool acChosen = false;     // chosen with the arrows: Enter takes it
    int acHovered = -1;        // the suggestion under the mouse last frame
    bool selectAllOnEdit = true;

    std::vector<Change> undo, redo;
    std::string message;
    bool messageIsError = false;
    std::string pendingOpen; // a file to open once unsaved changes are confirmed lost
    std::string autoLoaded;  // the Settings' database last opened automatically
};

State g;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Ext(const std::string& p) { return Lower(fs::path(p).extension().string()); }

void Say(const std::string& text, bool error = false) {
    g.message = text;
    g.messageIsError = error;
}

const sheetio::Cell* At(const sheetio::Sheet& sh, int row, int col) {
    auto it = sh.cells.find({row, col});
    return it != sh.cells.end() ? &it->second : nullptr;
}

void GoToIssue(const dbmodel::Issue& is);

// ---- checks -----------------------------------------------------------------------------------------

void Recheck(bool alert = true) {
    const auto t0 = std::chrono::steady_clock::now();
    g.issues = dbmodel::CheckBook(g.book);
    g.checkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g.cellIssue.assign(g.book.size(), {});
    g.rowIssue.assign(g.book.size(), {});
    g.sheetCounts.assign(g.book.size(), {0, 0});
    g.errors = g.warnings = 0;
    for (const dbmodel::Issue& is : g.issues) {
        const int sev = is.severity == dbmodel::Issue::Error ? 0 : 1;
        (sev == 0 ? g.errors : g.warnings)++;
        for (size_t s = 0; s < g.book.size(); ++s) {
            if (g.book[s].name != is.sheet) continue;
            (sev == 0 ? g.sheetCounts[s].first : g.sheetCounts[s].second)++;
            if (is.row > 0) {
                auto r = g.rowIssue[s].find(is.row);
                if (r == g.rowIssue[s].end()) g.rowIssue[s][is.row] = sev;
                else r->second = std::min(r->second, sev);
                if (is.col > 0) {
                    auto key = std::make_pair(is.row, is.col);
                    auto it = g.cellIssue[s].find(key);
                    if (it == g.cellIssue[s].end() || sev < it->second) g.cellIssue[s][key] = sev;
                }
            }
        }
    }
    // New problems (more than at the last check): an alert, with the first of them.
    if (alert && (g.errors > g.alertedErrors || g.warnings > g.alertedWarnings)) {
        const bool error = g.errors > g.alertedErrors;
        const dbmodel::Issue* first = nullptr;
        for (const dbmodel::Issue& is : g.issues)
            if ((is.severity == dbmodel::Issue::Error) == error) { first = &is; break; }
        std::string text = fs::path(g.loadedPath).filename().string() + ": " + std::to_string(g.errors) + " error(s), " +
                           std::to_string(g.warnings) + " warning(s) in the database.";
        if (first) text += "\n" + first->Where() + ": " + first->message;
        std::function<void()> show = [first = first ? std::optional<dbmodel::Issue>(*first) : std::nullopt] {
            if (first) GoToIssue(*first);
            if (g.hooks.show) g.hooks.show();
        };
        alerts::Raise(error ? alerts::Level::Error : alerts::Level::Warning, text, "File Processing > DB", show);
    }
    g.alertedErrors = g.errors;
    g.alertedWarnings = g.warnings;
}

// ---- open, save, compile ----------------------------------------------------------------------------

std::string Replace(const std::string& path, const char* ext) { return fs::path(path).replace_extension(ext).string(); }

void Open(const std::string& path) {
    dbmodel::Book book;
    std::string err;
    if (!dbmodel::LoadBook(path, book, err)) { Say("Cannot open " + path + ": " + err, true); return; }
    if (book.empty()) { Say(path + ": no database sheet", true); return; }
    g.book = std::move(book);
    g.loadedPath = path;
    g.loaded = true;
    g.dirty = false;
    g.sheet = 0;
    g.selRow = g.selCol = -1;
    g.editing = false;
    g.undo.clear();
    g.redo.clear();
    ++g.version;
    const std::string ext = Ext(path);
    std::snprintf(g.savePath, sizeof(g.savePath), "%s", (ext == ".xlsx" || ext == ".ods" ? path : Replace(path, ".xlsx")).c_str());
    std::snprintf(g.resPath, sizeof(g.resPath), "%s", (ext == ".res" ? path : Replace(path, ".res")).c_str());
    if (g.hooks.compileTo) { // where it was compiled to last time
        const std::string last = g.hooks.compileTo(path);
        if (!last.empty()) std::snprintf(g.resPath, sizeof(g.resPath), "%s", last.c_str());
    }
    g.alertedErrors = g.alertedWarnings = 0;
    Recheck();
    size_t records = 0;
    for (const sheetio::Sheet& sh : g.book) {
        std::set<int> rows;
        for (const auto& [rc, cell] : sh.cells) if (rc.first >= kFirstRow) rows.insert(rc.first);
        records += rows.size();
    }
    Say("Opened " + fs::path(path).filename().string() + ": " + std::to_string(g.book.size()) + " sheet(s), " +
        std::to_string(records) + " record(s).");
}

void Save() {
    std::string err;
    const std::string path = g.savePath;
    if (path.empty()) { Say("Choose where to save the spreadsheet.", true); return; }
    if (!dbmodel::SaveBook(path, g.book, err)) { Say("Not saved: " + err, true); return; }
    g.dirty = false;
    Say("Saved " + path + (g.errors ? " (" + std::to_string(g.errors) + " error(s) left)" : ""));
}

void Compile() {
    const std::string path = g.resPath;
    if (path.empty()) { Say("Choose where to write the .res.", true); return; }
    std::vector<uint8_t> res;
    std::vector<dbmodel::GeneratedFile> files;
    std::string err;
    if (!dbmodel::CompileBook(g.book, res, &files, err)) { Say("Not compiled: " + err, true); return; }
    std::string data(res.begin(), res.end());
    if (!sheetio::WriteFile(path, data, err)) { Say("Not written: " + err, true); return; }
    if (g.hooks.setCompileTo && !g.loadedPath.empty()) g.hooks.setCompileTo(g.loadedPath, path); // remembered
    std::string list;
    for (const auto& f : files) list += (list.empty() ? "" : ", ") + f.name;
    Say("Compiled " + path + " (" + list + ", " + std::to_string(res.size()) + " bytes)" +
        (g.errors ? ": " + std::to_string(g.errors) + " error(s) in the data, see the list" : ""), g.errors > 0);
}

// ---- changes -------------------------------------------------------------------------------------------

void Changed() {
    g.dirty = true;
    ++g.version;
    g.redo.clear();
    Recheck();
}

void Apply(const Change& c, bool undo) {
    if (c.sheet < 0 || c.sheet >= static_cast<int>(g.book.size())) return;
    sheetio::Sheet& sh = g.book[c.sheet];
    if (c.sheetBefore) {
        sh = undo ? *c.sheetBefore : *c.sheetAfter;
    } else {
        const auto& v = undo ? c.before : c.after;
        if (v) sh.Set(c.row, c.col, *v);
        else sh.cells.erase({c.row, c.col});
    }
    g.sheet = c.sheet;
    if (c.row > 0) { g.selRow = c.row; g.selCol = c.col > 0 ? c.col : g.selCol; g.scrollToSelection = true; }
    g.dirty = true;
    ++g.version;
    Recheck();
}

void Undo() {
    if (g.undo.empty()) return;
    Change c = std::move(g.undo.back());
    g.undo.pop_back();
    Apply(c, true);
    g.redo.push_back(std::move(c));
}

void Redo() {
    if (g.redo.empty()) return;
    Change c = std::move(g.redo.back());
    g.redo.pop_back();
    Apply(c, false);
    g.undo.push_back(std::move(c));
}

void PushUndo(Change c) {
    g.undo.push_back(std::move(c));
    // Whole-sheet steps are large: keep the last 200 steps.
    if (g.undo.size() > 200) g.undo.erase(g.undo.begin());
}

// The typed text into a cell: a number in a number field when it reads as one, else text; nothing
// when empty.
void SetCell(int row, int col, const std::string& typed) {
    sheetio::Sheet& sh = g.book[g.sheet];
    std::string text = typed;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::optional<sheetio::Cell> after;
    if (!text.empty()) {
        sheetio::Cell c;
        c.text = text;
        if (dbmodel::DescribeColumn(sh, col).numeric) {
            std::string t = text;
            t.erase(0, t.find_first_not_of(" \t"));
            t.erase(t.find_last_not_of(" \t") + 1);
            char* end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            const std::string lower = Lower(t);
            const bool special = lower.find("nan") != std::string::npos || lower.find("inf") != std::string::npos ||
                                 lower.find("0x") != std::string::npos;
            if (!t.empty() && *end == '\0' && !special) { c.isNumber = true; c.text = dbmodel::NumberText(v); }
        }
        after = c;
    }
    const sheetio::Cell* old = At(sh, row, col);
    std::optional<sheetio::Cell> before;
    if (old) before = *old;
    if ((!before && !after) || (before && after && before->text == after->text && before->isNumber == after->isNumber)) return;
    Change ch;
    ch.sheet = g.sheet;
    ch.row = row;
    ch.col = col;
    ch.before = before;
    ch.after = after;
    if (after) sh.Set(row, col, *after);
    else sh.cells.erase({row, col});
    PushUndo(std::move(ch));
    Changed();
}

// Moves every row from `from` on by `delta` rows (inserting or removing rows).
void ShiftRows(sheetio::Sheet& sh, int from, int delta) {
    std::map<std::pair<int, int>, sheetio::Cell> moved;
    for (auto it = sh.cells.begin(); it != sh.cells.end();) {
        if (it->first.first >= from) {
            moved[{it->first.first + delta, it->first.second}] = std::move(it->second);
            it = sh.cells.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [rc, cell] : moved) sh.cells[rc] = std::move(cell);
    sh.maxRow = 0;
    for (const auto& [rc, cell] : sh.cells) sh.maxRow = std::max(sh.maxRow, rc.first);
}

// kind: 0 insert an empty row above the selection, 1 duplicate the selected row below it, 2 delete it.
void RowAction(int kind) {
    if (g.selRow < kFirstRow) return;
    sheetio::Sheet& sh = g.book[g.sheet];
    Change ch;
    ch.sheet = g.sheet;
    ch.sheetBefore = sh;
    const int r = g.selRow;
    if (kind == 0) {
        ShiftRows(sh, r, 1);
        sh.maxRow = std::max(sh.maxRow, r);
    } else if (kind == 1) {
        std::vector<std::pair<int, sheetio::Cell>> copy;
        for (const auto& [rc, cell] : sh.cells) if (rc.first == r) copy.push_back({rc.second, cell});
        ShiftRows(sh, r + 1, 1);
        for (auto& [col, cell] : copy) sh.Set(r + 1, col, cell);
        g.selRow = r + 1;
    } else {
        for (auto it = sh.cells.begin(); it != sh.cells.end();) it = it->first.first == r ? sh.cells.erase(it) : std::next(it);
        ShiftRows(sh, r + 1, -1);
    }
    ch.sheetAfter = sh;
    ch.row = g.selRow;
    PushUndo(std::move(ch));
    g.scrollToSelection = true;
    Changed();
}

// ---- drawing ---------------------------------------------------------------------------------------

std::string ColumnTitle(const sheetio::Sheet& sh, int col) {
    const sheetio::Cell* t = At(sh, 2, col);
    if (t && !t->text.empty()) return t->text;
    const sheetio::Cell* m = At(sh, 3, col);
    return m ? m->text : sheetio::ColumnName(col);
}

void UpdateRows() {
    const std::string filter = Lower(g.filter);
    if (g.rowsVersion == g.version && g.rowsSheet == g.sheet && g.rowsFilter == filter && g.rowsOnlyProblems == g.onlyProblems) return;
    g.rowsVersion = g.version;
    g.rowsSheet = g.sheet;
    g.rowsFilter = filter;
    g.rowsOnlyProblems = g.onlyProblems;
    g.rows.clear();
    const sheetio::Sheet& sh = g.book[g.sheet];
    std::set<int> matching;
    for (const auto& [rc, cell] : sh.cells) {
        if (rc.first < kFirstRow) continue;
        if (filter.empty() || Lower(cell.text).find(filter) != std::string::npos) matching.insert(rc.first);
    }
    const int last = std::max(sh.maxRow, kFirstRow);
    for (int r = kFirstRow; r <= last; ++r) {
        if (!filter.empty() && !matching.count(r)) continue;
        if (g.onlyProblems && !g.rowIssue[g.sheet].count(r)) continue;
        g.rows.push_back(r);
    }
}

void GoToIssue(const dbmodel::Issue& is) {
    for (size_t s = 0; s < g.book.size(); ++s)
        if (g.book[s].name == is.sheet) g.sheet = static_cast<int>(s);
    g.selRow = is.row >= kFirstRow ? is.row : -1;
    g.selCol = is.col >= kFirstCol ? is.col : -1;
    g.editing = false;
    // The row may be hidden by the filter.
    g.filter[0] = '\0';
    if (g.onlyProblems && is.row > 0 && !g.rowIssue[g.sheet].count(is.row)) g.onlyProblems = false;
    g.scrollToSelection = true;
}

void SheetList(float height) {
    ImGui::BeginChild("##dbsheets", ImVec2(190, height), ImGuiChildFlags_Borders);
    for (size_t s = 0; s < g.book.size(); ++s) {
        const auto [e, w] = g.sheetCounts[s];
        ImGui::PushID(static_cast<int>(s));
        if (e) ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        else if (w) ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        std::string label = g.book[s].name;
        if (e || w) label += " (" + std::to_string(e + w) + ")";
        if (ImGui::Selectable(label.c_str(), g.sheet == static_cast<int>(s))) {
            g.sheet = static_cast<int>(s);
            g.selRow = g.selCol = -1;
            g.editing = false;
        }
        if (e || w) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d error(s), %d warning(s)", e, w);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Toolbar() {
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##dbfilter", "Filter rows (any cell contains)", g.filter, sizeof(g.filter));
    ImGui::SameLine();
    ImGui::Checkbox("Only rows with problems", &g.onlyProblems);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(g.selRow < kFirstRow);
    if (ImGui::Button("Insert row")) RowAction(0);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("An empty row above the selected one");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate row")) RowAction(1);
    ImGui::SameLine();
    if (ImGui::Button("Delete row")) RowAction(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(g.undo.empty());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g.redo.empty());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();
}

// ---- autocompletion -------------------------------------------------------------------------------------

// The text with the suggestion k in place of the word before the cursor; cursorAfter: where the cursor goes.
std::string WithCompletion(const std::string& text, int cursor, int k, int& cursorAfter) {
    const std::string& cand = g.completion.candidates[k];
    const size_t start = std::min(g.completion.tokenStart, static_cast<size_t>(cursor));
    cursorAfter = static_cast<int>(start + cand.size());
    return text.substr(0, start) + cand + text.substr(static_cast<size_t>(cursor));
}

bool CompletionCurrent(const char* buf, int cursor) {
    return !g.completion.candidates.empty() && cursor >= 0 && static_cast<size_t>(cursor) <= std::strlen(buf) &&
           g.completionFor.compare(0, std::string::npos, buf, static_cast<size_t>(cursor)) == 0;
}

int EditCallback(ImGuiInputTextCallbackData* d) {
    switch (d->EventFlag) {
    case ImGuiInputTextFlags_CallbackAlways:
        g.cursor = d->CursorPos;
        break;
    case ImGuiInputTextFlags_CallbackHistory: // Up / Down: choose a suggestion
        if (CompletionCurrent(d->Buf, d->CursorPos)) {
            const int n = static_cast<int>(g.completion.candidates.size());
            g.acSelected = (g.acSelected + (d->EventKey == ImGuiKey_UpArrow ? n - 1 : 1)) % n;
            g.acChosen = true;
        }
        break;
    case ImGuiInputTextFlags_CallbackCompletion: // Tab: take it
        if (CompletionCurrent(d->Buf, d->CursorPos)) {
            int after = 0;
            const std::string text = WithCompletion(d->Buf, d->CursorPos, g.acSelected, after);
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, text.c_str());
            d->CursorPos = d->SelectionStart = d->SelectionEnd = after;
        }
        break;
    default: break;
    }
    return 0;
}

// The suggestions for the text before the cursor, made again when it changes.
void UpdateCompletion(const sheetio::Sheet& sh, int col) {
    if (!g.completer || g.completerVersion != g.version) {
        g.completer = std::make_unique<dbmodel::Completer>(g.book);
        g.completerVersion = g.version;
        g.completionFor = "\x01"; // made again
    }
    const std::string before(g.editBuf, std::min(std::strlen(g.editBuf), static_cast<size_t>(std::max(g.cursor, 0))));
    if (before == g.completionFor) return;
    g.completionFor = before;
    g.completion = g.completer->Complete(sh, col, before);
    g.acSelected = 0;
    g.acChosen = false;
}

// The suggestions under the cell being edited (itemMin/itemMax: the input field).
void DrawCompletion(ImVec2 itemMin, ImVec2 itemMax) {
    const auto& cands = g.completion.candidates;
    if (cands.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(itemMin.x, itemMax.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(220.0f, itemMax.x - itemMin.x), 0), ImVec2(700, FLT_MAX));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##dbcompletion", nullptr, flags)) {
        const int n = static_cast<int>(cands.size()), shown = 12;
        const int first = std::clamp(g.acSelected - shown + 1, 0, std::max(0, n - shown));
        for (int k = first; k < n && k < first + shown; ++k) {
            ImGui::PushID(k);
            ImGui::Selectable(cands[k].c_str(), k == g.acSelected);
            if (ImGui::IsItemHovered()) g.acHovered = k;
            ImGui::PopID();
        }
        if (n > shown) ImGui::TextDisabled("%d more", n - shown);
        ImGui::TextDisabled("Tab or click: complete   Up/Down: choose (then Enter)");
    }
    ImGui::End();
}

void BeginEdit(const sheetio::Sheet& sh, int row, int col) {
    const sheetio::Cell* c = At(sh, row, col);
    std::snprintf(g.editBuf, sizeof(g.editBuf), "%s", c ? c->text.c_str() : "");
    g.selectAllOnEdit = true;
    g.cursor = static_cast<int>(std::strlen(g.editBuf));
    g.completionFor = "\x01";
    g.acHovered = -1;
    g.selRow = row;
    g.selCol = col;
    g.editing = true;
    g.startEdit = true;
}

// The keys of the selected cell (when the table has the focus and no cell is being typed into).
void TableKeys(const sheetio::Sheet& sh) {
    if (g.editing || g.selRow < kFirstRow || g.selCol < kFirstCol) return;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k); };
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && pressed(ImGuiKey_Z)) { Undo(); return; }
    if (ctrl && pressed(ImGuiKey_Y)) { Redo(); return; }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_F2)) { BeginEdit(sh, g.selRow, g.selCol); return; }
    if (pressed(ImGuiKey_Delete)) { SetCell(g.selRow, g.selCol, ""); return; }
    auto pos = std::find(g.rows.begin(), g.rows.end(), g.selRow);
    if (pressed(ImGuiKey_DownArrow) && pos != g.rows.end() && pos + 1 != g.rows.end()) { g.selRow = *(pos + 1); g.scrollToSelection = true; }
    if (pressed(ImGuiKey_UpArrow) && pos != g.rows.end() && pos != g.rows.begin()) { g.selRow = *(pos - 1); g.scrollToSelection = true; }
    if (pressed(ImGuiKey_RightArrow) && g.selCol < sh.maxCol) { ++g.selCol; g.scrollToSelection = true; }
    if (pressed(ImGuiKey_LeftArrow) && g.selCol > kFirstCol) { --g.selCol; g.scrollToSelection = true; }
}

void SheetTable(float height) {
    const sheetio::Sheet& sh = g.book[g.sheet];
    UpdateRows();
    const int lastCol = std::max(sh.maxCol, kFirstCol);
    const int columns = 1 + (lastCol - kFirstCol + 1);
    if (columns > 512) { ImGui::TextColored(kErrorColor, "This sheet has too many columns to show (%d).", columns); return; }
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##dbtable", columns, flags, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 44.0f);
    for (int c = kFirstCol; c <= lastCol; ++c) {
        auto w = sh.widthsPx.find(c);
        const float width = w != sh.widthsPx.end() ? std::clamp(static_cast<float>(w->second), 40.0f, 400.0f) : 90.0f;
        ImGui::TableSetupColumn(ColumnTitle(sh, c).c_str(), ImGuiTableColumnFlags_WidthFixed, width);
    }
    // The header: titles, with the marker, type and description on hover.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0);
    ImGui::TableHeader("Row");
    for (int c = kFirstCol; c <= lastCol; ++c) {
        ImGui::TableSetColumnIndex(1 + c - kFirstCol);
        ImGui::PushID(c);
        ImGui::TableHeader(ImGui::TableGetColumnName(1 + c - kFirstCol));
        if (ImGui::IsItemHovered()) {
            const dbmodel::ColumnInfo info = dbmodel::DescribeColumn(sh, c);
            ImGui::BeginTooltip();
            ImGui::Text("Column %s, %s", sheetio::ColumnName(c).c_str(), info.marker.empty() ? "no marker" : info.marker.c_str());
            if (info.known) ImGui::Text("Type: %s", info.type.c_str());
            else if (!info.marker.empty()) ImGui::TextColored(kErrorColor, "Not a field of this block");
            if (!info.description.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                ImGui::TextUnformatted(info.description.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }

    const auto& cellIssue = g.cellIssue[g.sheet];
    const auto& rowIssue = g.rowIssue[g.sheet];
    int scrollIndex = -1;
    if (g.scrollToSelection) {
        auto pos = std::find(g.rows.begin(), g.rows.end(), g.selRow);
        if (pos != g.rows.end()) scrollIndex = static_cast<int>(pos - g.rows.begin());
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g.rows.size()));
    if (scrollIndex >= 0) clipper.IncludeItemByIndex(scrollIndex);
    std::optional<std::pair<std::pair<int, int>, std::string>> commit;
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const int row = g.rows[i];
            ImGui::TableNextRow();
            ImGui::PushID(row);
            ImGui::TableSetColumnIndex(0);
            auto ri = rowIssue.find(row);
            if (ri != rowIssue.end())
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ri->second == 0 ? ImVec4(0.6f, 0.15f, 0.12f, 0.75f)
                                                                                                    : ImVec4(0.55f, 0.42f, 0.08f, 0.65f)));
            ImGui::Text("%d", row);
            if (i == scrollIndex && g.selCol < kFirstCol) ImGui::SetScrollHereY(0.5f);
            for (int c = kFirstCol; c <= lastCol; ++c) {
                ImGui::TableSetColumnIndex(1 + c - kFirstCol);
                ImGui::PushID(c);
                const bool selected = row == g.selRow && c == g.selCol;
                auto ci = cellIssue.find({row, c});
                if (ci != cellIssue.end())
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ci->second == 0 ? ImVec4(0.75f, 0.18f, 0.15f, 0.8f)
                                                                                                        : ImVec4(0.7f, 0.55f, 0.1f, 0.7f)));
                else if (ri != rowIssue.end())
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ri->second == 0 ? ImVec4(0.5f, 0.12f, 0.1f, 0.25f)
                                                                                                        : ImVec4(0.5f, 0.4f, 0.08f, 0.2f)));
                if (selected && g.editing) {
                    if (g.startEdit) { ImGui::SetKeyboardFocusHere(); g.startEdit = false; }
                    const ImGuiID editId = ImGui::GetID("##edit");
                    // A click on a suggestion (under the mouse last frame): take it and keep typing. The click is
                    // used up here, else the field would take it as a click outside and close.
                    if (g.acHovered >= 0 && g.acHovered < static_cast<int>(g.completion.candidates.size()) && ImGui::IsMouseClicked(0) &&
                        CompletionCurrent(g.editBuf, g.cursor)) {
                        int after = 0;
                        const std::string text = WithCompletion(g.editBuf, g.cursor, g.acHovered, after);
                        std::snprintf(g.editBuf, sizeof(g.editBuf), "%s", text.c_str());
                        if (ImGuiInputTextState* st = ImGui::GetInputTextState(editId)) {
                            st->WantReloadUserBuf = true;
                            st->ReloadSelectionStart = st->ReloadSelectionEnd = after;
                        }
                        g.cursor = after;
                        ImGui::GetIO().MouseClicked[0] = false;
                    }
                    g.acHovered = -1;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                                                           ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory |
                                                           (g.selectAllOnEdit ? ImGuiInputTextFlags_AutoSelectAll : 0);
                    const bool enter = ImGui::InputText("##edit", g.editBuf, sizeof(g.editBuf), inputFlags, EditCallback);
                    const ImVec2 itemMin = ImGui::GetItemRectMin(), itemMax = ImGui::GetItemRectMax();
                    const bool active = ImGui::IsItemActive();
                    if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
                        std::string text = g.editBuf;
                        // Enter on a suggestion chosen with the arrows takes it.
                        if (enter && g.acChosen && CompletionCurrent(g.editBuf, g.cursor)) {
                            int after = 0;
                            text = WithCompletion(g.editBuf, g.cursor, g.acSelected, after);
                        }
                        commit = {{row, c}, text};
                        g.editing = false;
                    } else if (ImGui::IsItemDeactivated()) {
                        g.editing = false; // Escape, or clicked elsewhere without a change
                    }
                    if (active) {
                        g.selectAllOnEdit = false;
                        UpdateCompletion(sh, c);
                        DrawCompletion(itemMin, itemMax);
                    }
                } else {
                    const sheetio::Cell* cell = At(sh, row, c);
                    const char* text = cell ? cell->text.c_str() : "";
                    // One line in the cell: the first line of a multi-line text.
                    std::string shown;
                    if (cell && cell->text.find('\n') != std::string::npos) { shown = cell->text.substr(0, cell->text.find('\n')) + " ..."; text = shown.c_str(); }
                    if (ImGui::Selectable(text[0] ? text : "##empty", selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        g.selRow = row;
                        g.selCol = c;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginEdit(sh, row, c);
                    }
                    // Right-click: the quick fixes of the cell's problems.
                    if (ci != cellIssue.end() && ImGui::BeginPopupContextItem("##fixes")) {
                        bool any = false;
                        for (const dbmodel::Issue& is : g.issues) {
                            if (is.row != row || is.col != c || is.sheet != sh.name) continue;
                            for (const dbmodel::Issue::Fix& fx : is.fixes) {
                                any = true;
                                if (ImGui::MenuItem(("Fix: " + fx.label).c_str())) commit = {{row, c}, fx.text};
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", fx.text.c_str());
                            }
                        }
                        if (!any) ImGui::TextDisabled("No quick fix for this problem");
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered() && (ci != cellIssue.end() || (cell && cell->text.size() > 12))) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                        if (cell) ImGui::TextUnformatted(cell->text.c_str());
                        for (const dbmodel::Issue& is : g.issues) {
                            if (is.row != row || is.col != c || is.sheet != sh.name) continue;
                            ImGui::TextColored(is.severity == dbmodel::Issue::Error ? kErrorColor : kWarningColor, "%s", is.message.c_str());
                            if (!is.fixes.empty()) ImGui::TextDisabled("Right-click: quick fix");
                        }
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
                if (selected && i == scrollIndex) { ImGui::SetScrollHereY(0.5f); ImGui::SetScrollHereX(0.5f); }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }
    if (scrollIndex >= 0 || g.rows.empty()) g.scrollToSelection = false;
    TableKeys(sh);
    ImGui::EndTable();
    if (commit) SetCell(commit->first.first, commit->first.second, commit->second);
}

void IssueList(float height) {
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::Checkbox(("Errors (" + std::to_string(g.errors) + ")").c_str(), &g.showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
    ImGui::Checkbox(("Warnings (" + std::to_string(g.warnings) + ")").c_str(), &g.showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("This sheet only", &g.issuesOfSheetOnly);
    ImGui::SameLine();
    if (ImGui::Button("Re-check")) Recheck();
    ImGui::SameLine();
    ImGui::TextDisabled("(checked in %.0f ms; errors: the .res would not hold what the cell shows, or the game misreads it)", g.checkMs);
    if (g.issues.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "No problem found.");
        return;
    }
    if (!ImGui::BeginTable("##dbissues", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                           ImVec2(0, height - ImGui::GetFrameHeightWithSpacing())))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 14);
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Quick fix", ImGuiTableColumnFlags_WidthFixed, 260);
    ImGui::TableHeadersRow();
    const std::string sheetName = g.book[g.sheet].name;
    std::optional<std::pair<dbmodel::Issue, std::string>> fix; // applied after the table
    for (size_t i = 0; i < g.issues.size(); ++i) {
        const dbmodel::Issue& is = g.issues[i];
        const bool error = is.severity == dbmodel::Issue::Error;
        if ((error && !g.showErrors) || (!error && !g.showWarnings)) continue;
        if (g.issuesOfSheetOnly && is.sheet != sheetName) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, error ? kErrorColor : kWarningColor);
        const bool current = is.sheet == sheetName && is.row == g.selRow && (is.col == g.selCol || is.col <= 0);
        if (ImGui::Selectable(error ? "E" : "W", current, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) GoToIssue(is);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(is.Where().c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(is.message.c_str());
        ImGui::TableNextColumn();
        for (size_t k = 0; k < is.fixes.size(); ++k) {
            if (k) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::SmallButton(is.fixes[k].label.c_str())) fix = {is, is.fixes[k].text};
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The cell becomes:\n%s", is.fixes[k].text.c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (fix) {
        GoToIssue(fix->first);
        SetCell(fix->first.row, fix->first.col, fix->second);
    }
}

// A path field with a file dialog button.
bool PathField(const char* id, const char* label, char* buf, size_t size, bool save, const char* filterName, const char* filterExt, float width) {
    bool picked = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##path", buf, size);
    ImGui::SameLine();
    if (ImGui::Button("...") && g.hooks.pickFile) {
        std::string p;
        if (g.hooks.pickFile(save, filterName, filterExt, p)) { std::snprintf(buf, size, "%s", p.c_str()); picked = true; }
    }
    ImGui::PopID();
    return picked;
}

void ConfirmDiscard() {
    if (!g.pendingOpen.empty() && !ImGui::IsPopupOpen("Unsaved changes##db")) ImGui::OpenPopup("Unsaved changes##db");
    if (!ImGui::BeginPopupModal("Unsaved changes##db", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%s has changes that are not saved.", fs::path(g.loadedPath).filename().string().c_str());
    if (ImGui::Button("Open the other file anyway")) {
        const std::string p = g.pendingOpen;
        g.pendingOpen.clear();
        ImGui::CloseCurrentPopup();
        Open(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { g.pendingOpen.clear(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void RequestOpen(const std::string& path) {
    if (path.empty()) { Say("Choose a database: .res, .xlsx or .ods.", true); return; }
    if (g.loaded && g.dirty) g.pendingOpen = path;
    else Open(path);
}

// "Open the Settings' database automatically": at the start, and again when the Settings change it
// (unless the open database has unsaved changes).
void AutoLoad() {
    if (!g.hooks.autoLoad || !g.hooks.autoLoad() || !g.hooks.settingsDatabase) return;
    const std::string path = g.hooks.settingsDatabase();
    if (path.empty() || path == g.autoLoaded || (g.loaded && g.dirty)) return;
    g.autoLoaded = path;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { Say("The Settings' database does not exist: " + path, true); return; }
    std::snprintf(g.path, sizeof(g.path), "%s", path.c_str());
    Open(path);
}

} // namespace

void SetHooks(Hooks hooks) { g.hooks = std::move(hooks); }

bool HasUnsavedChanges() { return g.loaded && g.dirty; }

void OpenFile(const std::string& path) {
    std::snprintf(g.path, sizeof(g.path), "%s", path.c_str());
    RequestOpen(path);
}

void Update() { AutoLoad(); }

void DrawTab() {
    const float fieldWidth = std::max(200.0f, ImGui::GetContentRegionAvail().x - 110 - 260);
    if (PathField("dbopen", "Database:", g.path, sizeof(g.path), false, "Database", "*.res;*.xlsx;*.ods", fieldWidth)) RequestOpen(g.path);
    ImGui::SameLine();
    if (ImGui::Button(g.loaded && g.loadedPath == g.path ? "Reload" : "Open", ImVec2(110, 0))) RequestOpen(g.path);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A compiled database (databaselmp.res, database.res) or its spreadsheet (.xlsx, .ods).");
    if (g.hooks.autoLoad) {
        bool on = g.hooks.autoLoad();
        if (ImGui::Checkbox("Open the Settings' database automatically", &on) && g.hooks.setAutoLoad) {
            g.hooks.setAutoLoad(on);
            g.autoLoaded.clear(); // on: open it now
        }
        if (ImGui::IsItemHovered()) {
            const std::string p = g.hooks.settingsDatabase ? g.hooks.settingsDatabase() : std::string();
            ImGui::SetTooltip("The database of Settings > Sources%s%s", p.empty() ? " (none set)" : ":\n", p.c_str());
        }
    }
    ConfirmDiscard();
    if (!g.loaded) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Opens an Evil Islands gameplay database - a compiled .res (databaselmp.res, database.res) or its "
            "spreadsheet (.xlsx, or .ods from LibreOffice) - to check it for errors and wrong values, edit it, save "
            "it as a spreadsheet and compile it to a .res. Problems are highlighted: red cells are errors (the .res "
            "would not hold what the cell shows, or the game misreads it), yellow ones warnings (unknown items, "
            "spells or materials, duplicate names...).\n\nCommand line: um-multitool dbexport (.res -> .xlsx / .ods), "
            "um-multitool xlsxdb (spreadsheet -> .res, checks first; --check only checks).");
        if (!g.message.empty()) ImGui::TextColored(g.messageIsError ? kErrorColor : ImVec4(0.6f, 0.85f, 0.6f, 1), "%s", g.message.c_str());
        return;
    }

    PathField("dbsave", "Spreadsheet:", g.savePath, sizeof(g.savePath), true, "Spreadsheet", "*.xlsx;*.ods", fieldWidth);
    ImGui::SameLine();
    // Ctrl+S too, also while typing in a cell (that cell is taken first).
    const bool ctrlS = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    if (ImGui::Button("Save", ImVec2(110, 0)) || ctrlS) {
        if (g.editing) {
            g.editing = false;
            SetCell(g.selRow, g.selCol, g.editBuf);
        }
        Save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ctrl+S. .xlsx or .ods, by the name's extension. The values, titles, column widths and frozen rows are\n"
                          "written; other formatting of a spreadsheet saved by Excel or LibreOffice is not kept.");
    PathField("dbres", "Compile to:", g.resPath, sizeof(g.resPath), true, "RES Archive", "*.res", fieldWidth);
    ImGui::SameLine();
    if (ImGui::Button("Compile .res", ImVec2(110, 0))) Compile();
    ImGui::Text("%s%s", fs::path(g.loadedPath).filename().string().c_str(), g.dirty ? " (modified)" : "");
    if (!g.message.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(g.messageIsError ? kErrorColor : ImVec4(0.6f, 0.85f, 0.6f, 1), "- %s", g.message.c_str());
    }
    ImGui::Separator();

    const float avail = ImGui::GetContentRegionAvail().y;
    const float issuesHeight = std::clamp(avail * 0.28f, 110.0f, 260.0f);
    const float top = avail - issuesHeight - ImGui::GetStyle().ItemSpacing.y;
    SheetList(top);
    ImGui::SameLine();
    ImGui::BeginGroup();
    Toolbar();
    if (g.sheet >= 0 && g.sheet < static_cast<int>(g.book.size()))
        SheetTable(top - ImGui::GetFrameHeightWithSpacing());
    ImGui::EndGroup();
    IssueList(issuesHeight);
}

} // namespace dbedit
