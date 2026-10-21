// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The INI <-> REG tab: see inireg_editor.hpp.
//
// A file opens as lines: an .ini as it is, a binary .reg (found by its magic, not its extension) as the INI text inireg.cpp's REG -> INI makes of it. The
// "Keys" view groups the lines in sections, with each key and value in its own box; the "Text" view is the plain text. Saving to a name ending in .reg
// writes the binary format again (the keys' types come from the shape of their values: a number, a number with a decimal point, anything else; a key
// repeated in a section is an array), any other name writes the text. A .reg that would not come back byte for byte from its text says so when it opens.
// Texts are shown as UTF-8; the file's own encoding (CP1251, as the game's) is kept when saving.

#include "inireg_editor.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "imgui.h"

#include "cp1251.hpp"
#include "i18n.hpp"
#include "inireg_model.hpp"
#include "subtools.hpp"
#include "viewer/ui_common.hpp"

namespace fs = std::filesystem;
using namespace inimodel;

namespace inireg_edit {
namespace {

enum class FileKind { None, Ini, Reg };

struct Doc {
    std::string path;
    FileKind kind = FileKind::None;
    std::vector<std::string> lines;
    bool crlf = false, cp1251 = false, bom = false;
    bool dirty = false;
    std::string warning;          // opened with a caveat (does not round trip)
    size_t fileSize = 0;
};

Doc g_doc;
int g_view = 0;                   // 0 keys, 1 text
std::string g_text;               // the Text view's buffer
int g_selected = 0;
std::string g_search, g_newKey, g_newValue, g_newSection;
std::string g_status;
bool g_statusError = false;
char g_pathBuf[1024] = "";
// The box being typed in keeps its own text (the lines are re-read trimmed each frame: a space at the end would vanish while typing).
ImGuiID g_editId = 0;
std::string g_editBuf;

void Say(const std::string& text, bool error = false) { g_status = text; g_statusError = error; }

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}
bool IsRegMagic(const std::vector<uint8_t>& b) { return b.size() >= 6 && b[0] == 0xFB && b[1] == 0x3E && b[2] == 0xAB && b[3] == 0x45; }
bool IsValidUtf8(const std::vector<uint8_t>& b) {
    size_t i = 0;
    while (i < b.size()) {
        const uint8_t c = b[i];
        const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99) return false;
        for (size_t k = 1; k <= n; ++k) if (i + k >= b.size() || (b[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}
std::string LowerCopy(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool HasExtension(const std::string& path, const char* ext) { return LowerCopy(fs::path(path).extension().string()) == ext; }

void SyncFromText() { g_doc.lines = Split(g_text); }
void SyncToText() { g_text = Join(g_doc.lines, g_doc.crlf); }

// The editor's text as the bytes of a file of that kind; `lost` counts the characters the encoding cannot hold.
bool Encode(FileKind kind, std::vector<uint8_t>& out, int& lost, std::string& err) {
    const std::string text = Join(g_doc.lines, g_doc.crlf);
    lost = 0;
    std::vector<uint8_t> bytes;
    if (kind == FileKind::Reg || g_doc.cp1251) {
        bytes = cp1251::EncodeCp1251(text);
        for (size_t i = 0; i < bytes.size(); ++i) if (bytes[i] == '?') ++lost;
        for (char c : text) if (c == '?') --lost;
        if (lost < 0) lost = 0;
    } else {
        bytes.assign(text.begin(), text.end());
        if (g_doc.bom) bytes.insert(bytes.begin(), {0xEF, 0xBB, 0xBF});
    }
    if (kind == FileKind::Reg) {
        std::vector<uint8_t> reg;
        if (!IniRegTextToReg(std::string(bytes.begin(), bytes.end()), reg)) { err = i18n::Tr("The text could not be made into a .reg"); return false; }
        out = reg;
    } else out = bytes;
    return true;
}

bool LoadFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) { Say(std::string(i18n::Tr("Cannot read ")) + path, true); return false; }
    Doc d;
    d.path = path;
    d.fileSize = bytes.size();
    std::string text;
    if (IsRegMagic(bytes)) {
        std::string ini, err;
        if (!IniRegRegToText(bytes, ini, err)) { Say(std::string(i18n::Tr("Not a readable .reg: ")) + err, true); return false; }
        d.kind = FileKind::Reg;
        d.cp1251 = true;
        text = cp1251::DecodeCp1251(reinterpret_cast<const uint8_t*>(ini.data()), ini.size());
        std::vector<uint8_t> back;
        if (!IniRegTextToReg(ini, back) || back != bytes)
            d.warning = i18n::Tr("This .reg does not come back byte for byte from its text (the types or the order of some keys may change when it is saved).");
    } else {
        d.kind = FileKind::Ini;
        size_t skip = 0;
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { d.bom = true; skip = 3; }
        const std::vector<uint8_t> body(bytes.begin() + static_cast<long>(skip), bytes.end());
        if (IsValidUtf8(body)) text.assign(body.begin(), body.end());
        else { d.cp1251 = true; text = cp1251::DecodeCp1251(body.data(), body.size()); }
    }
    d.crlf = UsesCrlf(text);
    d.lines = Split(text);
    g_doc = d;
    g_text.clear();
    g_selected = 0;
    g_editId = 0;
    std::snprintf(g_pathBuf, sizeof g_pathBuf, "%s", path.c_str());
    Say(std::string(i18n::Tr("Opened ")) + path);
    return true;
}

bool SaveTo(const std::string& path) {
    if (g_view == 1) SyncFromText();
    const FileKind kind = HasExtension(path, ".reg") ? FileKind::Reg : FileKind::Ini;
    std::vector<uint8_t> bytes; int lost = 0; std::string err;
    if (!Encode(kind, bytes, lost, err)) { Say(err, true); return false; }
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::exists(path + ".bak", ec)) fs::copy_file(path, path + ".bak", ec); // the first save of a file keeps the original beside it
    if (!WriteFileBytes(path, bytes)) { Say(std::string(i18n::Tr("Cannot write ")) + path, true); return false; }
    g_doc.path = path;
    g_doc.kind = kind;
    g_doc.fileSize = bytes.size();
    g_doc.dirty = false;
    g_doc.warning.clear();
    std::snprintf(g_pathBuf, sizeof g_pathBuf, "%s", path.c_str());
    Say(std::string(i18n::Tr("Saved ")) + path + (lost ? std::string(" - ") + std::to_string(lost) + i18n::Tr(" character(s) the file's encoding cannot hold became '?'") : ""), lost > 0);
    return true;
}

// ---- boxes -------------------------------------------------------------------------------------------------------

int NoEquals(ImGuiInputTextCallbackData* d) { return d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter && d->EventChar == '=' ? 1 : 0; }
struct EditCtx { std::string* s; ImGuiInputTextCallback filter; };
int EditCallback(ImGuiInputTextCallbackData* d) {
    auto* c = static_cast<EditCtx*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        c->s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = c->s->data();
        return 0;
    }
    return c->filter ? c->filter(d) : 0;
}
bool EditString(const char* id, std::string& s, ImGuiInputTextCallback filter = nullptr, const char* hint = nullptr) {
    EditCtx ctx{&s, filter};
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | (filter ? ImGuiInputTextFlags_CallbackCharFilter : 0);
    if (hint) return ImGui::InputTextWithHint(id, hint, s.data(), s.capacity() + 1, flags, EditCallback, &ctx);
    return ImGui::InputText(id, s.data(), s.capacity() + 1, flags, EditCallback, &ctx);
}

// ---- syntax colours (the Text view through imgui's colouring hook, the Keys view per box) -----------------------------------------------------------------

const ImVec4 kSectionColor(1.0f, 0.85f, 0.4f, 1), kKeyColor(0.78f, 0.65f, 0.98f, 1), kIntColor(0.55f, 0.75f, 1.0f, 1), kFloatColor(0.6f, 0.88f, 0.6f, 1),
    kTextColor(0.96f, 0.8f, 0.55f, 1), kCommentColor(0.5f, 0.55f, 0.5f, 1), kPunctColor(0.62f, 0.62f, 0.62f, 1);
ImVec4 ValueColor(ValueType t) { return t == ValueType::Int ? kIntColor : t == ValueType::Float ? kFloatColor : kTextColor; }
ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// imgui's UmColorLineFn: the coloured spans of one line of the text.
int ColorLine(void*, const char* b, const char* e, UmColorSpan* spans, int max) {
    const std::string line(b, e);
    const LineKind kind = Kind(line);
    const int len = static_cast<int>(line.size());
    int first = 0; while (first < len && (line[static_cast<size_t>(first)] == ' ' || line[static_cast<size_t>(first)] == '\t')) ++first;
    int last = len; while (last > first && (line[static_cast<size_t>(last - 1)] == ' ' || line[static_cast<size_t>(last - 1)] == '\t' || line[static_cast<size_t>(last - 1)] == '\r')) --last;
    int n = 0;
    auto add = [&](int from, int to, const ImVec4& c) { if (n < max && to > from) spans[n++] = {from, to, U32(c)}; };
    if (kind == LineKind::Section) add(first, last, kSectionColor);
    else if (kind == LineKind::Other) add(first, last, kCommentColor);
    else if (kind == LineKind::KeyValue) {
        const int eq = static_cast<int>(line.find('='));
        if (line[static_cast<size_t>(first)] == ';') { add(first, last, kCommentColor); return n; } // a commented-out key
        int keyEnd = eq; while (keyEnd > first && (line[static_cast<size_t>(keyEnd - 1)] == ' ' || line[static_cast<size_t>(keyEnd - 1)] == '\t')) --keyEnd;
        int valueStart = eq + 1; while (valueStart < last && (line[static_cast<size_t>(valueStart)] == ' ' || line[static_cast<size_t>(valueStart)] == '\t')) ++valueStart;
        add(first, keyEnd, kKeyColor);
        add(eq, eq + 1, kPunctColor);
        add(valueStart, last, ValueColor(Classify(line.substr(static_cast<size_t>(valueStart), static_cast<size_t>(last - valueStart)))));
    }
    return n;
}

// A key's or a value's box for line `index`: while it is the active box it keeps its own text. Returns true when the line changed.
bool LineField(int index, bool isKey) {
    std::string& line = g_doc.lines[static_cast<size_t>(index)];
    std::string key, value;
    SplitKeyValue(line, key, value);
    ImGui::PushID(index * 2 + (isKey ? 0 : 1));
    const ImGuiID id = ImGui::GetID("##f");
    std::string shown = (g_editId == id) ? g_editBuf : (isKey ? key : value);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_Text, isKey ? kKeyColor : ValueColor(Classify(shown)));
    {
        i18n::Verbatim verbatim;
        if (EditString("##f", shown, isKey ? NoEquals : nullptr)) {
            changed = true;
            if (isKey) line = MakeKeyValue(shown, value);
            else line = MakeKeyValue(key, shown);
            g_editBuf = shown;
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemActive()) { g_editId = id; if (!changed) g_editBuf = shown; }
    else if (g_editId == id) g_editId = 0;
    ImGui::PopID();
    if (changed) g_doc.dirty = true;
    return changed;
}

bool Contains(const std::string& hay, const std::string& needleLower) { return LowerCopy(hay).find(needleLower) != std::string::npos; }

// One key line as a table row: x (delete), +, key, value, type.
void KeyRow(int index, const std::vector<Section>& sections, int sectionIndex, bool withSection, int& deleteLine, int& duplicateLine) {
    ImGui::TableNextRow();
    ImGui::PushID(index);
    ImGui::TableSetColumnIndex(0);
    if (ImGui::SmallButton("x")) deleteLine = index;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("Delete this line"));
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) duplicateLine = index;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("Add the same key again below (a key repeated in a section is an array in a .reg)"));
    int col = 1;
    if (withSection) {
        ImGui::TableSetColumnIndex(col++);
        i18n::Verbatim verbatim;
        ImGui::TextDisabled("%s", sections[static_cast<size_t>(sectionIndex)].name.c_str());
    }
    ImGui::TableSetColumnIndex(col++);
    LineField(index, true);
    ImGui::TableSetColumnIndex(col++);
    LineField(index, false);
    ImGui::TableSetColumnIndex(col);
    std::string key, value;
    SplitKeyValue(g_doc.lines[static_cast<size_t>(index)], key, value);
    const int same = CountKey(g_doc.lines, sections[static_cast<size_t>(sectionIndex)], key);
    const ValueType t = Classify(value);
    const ImVec4 color = t == ValueType::Int ? ImVec4(0.55f, 0.75f, 1.0f, 1) : t == ValueType::Float ? ImVec4(0.6f, 0.85f, 0.6f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
    ImGui::TextColored(color, "%s", TypeName(t));
    if (same > 1) { ImGui::SameLine(); ImGui::TextDisabled("x%d", same); }
    ImGui::PopID();
}

void ApplyLineEdits(int deleteLine, int duplicateLine) {
    if (deleteLine >= 0) { g_doc.lines.erase(g_doc.lines.begin() + deleteLine); g_doc.dirty = true; g_editId = 0; }
    else if (duplicateLine >= 0) { g_doc.lines.insert(g_doc.lines.begin() + duplicateLine + 1, g_doc.lines[static_cast<size_t>(duplicateLine)]); g_doc.dirty = true; g_editId = 0; }
}

void DrawKeysView(float height) {
    const std::vector<Section> sections = Sections(g_doc.lines);
    if (g_selected >= static_cast<int>(sections.size())) g_selected = std::max(0, static_cast<int>(sections.size()) - 1);
    const std::string needle = LowerCopy(g_search);

    ImGui::BeginChild("##sections", ImVec2(270, height), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-FLT_MIN);
    EditString("##search", g_search, nullptr, "Search keys and values");
    ImGui::Separator();
    ImGui::BeginChild("##sectionlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f));
    for (int i = 0; i < static_cast<int>(sections.size()); ++i) {
        const Section& s = sections[static_cast<size_t>(i)];
        if (!needle.empty()) { // only the sections that hold a match
            bool any = Contains(s.name, needle);
            for (int l = s.first; l < s.last && !any; ++l) if (Kind(g_doc.lines[static_cast<size_t>(l)]) == LineKind::KeyValue && Contains(g_doc.lines[static_cast<size_t>(l)], needle)) any = true;
            if (!any) continue;
        }
        char label[256];
        std::snprintf(label, sizeof label, "%s  (%d)##s%d", s.header < 0 ? i18n::Tr("(before the first section)") : s.name.c_str(), s.keys, i);
        i18n::Verbatim verbatim;
        if (ImGui::Selectable(label, g_selected == i)) { g_selected = i; g_editId = 0; }
    }
    ImGui::EndChild();
    EditString("##newsec", g_newSection, nullptr, "New section name");
    ImGui::SameLine();
    ImGui::BeginDisabled(Trim(g_newSection).empty());
    if (ImGui::Button("Add##sec")) {
        AddSection(g_doc.lines, Trim(g_newSection));
        g_newSection.clear();
        g_doc.dirty = true;
        g_selected = static_cast<int>(Sections(g_doc.lines).size()) - 1;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##keys", ImVec2(0, height), ImGuiChildFlags_Borders);
    int deleteLine = -1, duplicateLine = -1;
    const bool searching = !needle.empty();
    if (sections.empty()) {
        ImGui::TextDisabled("%s", i18n::Tr("No section yet: add one on the left."));
    } else if (searching) {
        ImGui::TextDisabled("Matches in every section");
        if (ImGui::BeginTable("##matches", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 44);
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();
            for (int si = 0; si < static_cast<int>(sections.size()); ++si)
                for (int l = sections[static_cast<size_t>(si)].first; l < sections[static_cast<size_t>(si)].last; ++l)
                    if (Kind(g_doc.lines[static_cast<size_t>(l)]) == LineKind::KeyValue && (Contains(g_doc.lines[static_cast<size_t>(l)], needle) || Contains(sections[static_cast<size_t>(si)].name, needle)))
                        KeyRow(l, sections, si, true, deleteLine, duplicateLine);
            ImGui::EndTable();
        }
    } else {
        const Section& s = sections[static_cast<size_t>(g_selected)];
        {
            i18n::Verbatim verbatim;
            ImGui::TextColored(kSectionColor, "[%s]", s.header < 0 ? "" : s.name.c_str());
        }
        if (s.header >= 0) {
            ImGui::SameLine();
            static std::string rename;
            static int renamedFor = -1;
            if (ImGui::SmallButton("Rename")) { rename = s.name; renamedFor = g_selected; ImGui::OpenPopup("##rename"); }
            if (ImGui::BeginPopup("##rename")) {
                EditString("##name", rename);
                ImGui::BeginDisabled(Trim(rename).empty());
                if (ImGui::Button("OK") && renamedFor == g_selected) { g_doc.lines[static_cast<size_t>(s.header)] = "[" + Trim(rename) + "]"; g_doc.dirty = true; ImGui::CloseCurrentPopup(); }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete section")) ImGui::OpenPopup("##delsec");
            if (ImGui::BeginPopup("##delsec")) {
                ImGui::TextUnformatted("Delete the section and all its lines?");
                if (ImGui::Button("Delete")) {
                    g_doc.lines.erase(g_doc.lines.begin() + s.header, g_doc.lines.begin() + s.last);
                    g_doc.dirty = true; g_editId = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        if (g_selected < static_cast<int>(Sections(g_doc.lines).size()) && Sections(g_doc.lines)[static_cast<size_t>(g_selected)].name == s.name) {
            if (ImGui::BeginTable("##keytable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 44);
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableHeadersRow();
                for (int l = s.first; l < s.last; ++l) {
                    const LineKind k = Kind(g_doc.lines[static_cast<size_t>(l)]);
                    if (k == LineKind::KeyValue) KeyRow(l, sections, g_selected, false, deleteLine, duplicateLine);
                    else if (k == LineKind::Other) {
                        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                        i18n::Verbatim verbatim;
                        ImGui::TextDisabled("%s", g_doc.lines[static_cast<size_t>(l)].c_str());
                    }
                }
                ImGui::EndTable();
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
            EditString("##newkey", g_newKey, NoEquals, "New key");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            EditString("##newval", g_newValue, nullptr, "Value");
            ImGui::SameLine();
            ImGui::BeginDisabled(Trim(g_newKey).empty());
            if (ImGui::Button("Add key")) { InsertKey(g_doc.lines, s, Trim(g_newKey), Trim(g_newValue)); g_newKey.clear(); g_newValue.clear(); g_doc.dirty = true; }
            ImGui::EndDisabled();
        }
    }
    ApplyLineEdits(deleteLine, duplicateLine);
    ImGui::EndChild();
}

// ---- converting files and folders ---------------------------------------------------------------------------------

struct ConvertState {
    char input[1024] = "";
    char output[1024] = "";
    int mode = 0; // 0 by the file, 1 INI -> REG, 2 REG -> INI
    std::vector<std::string> results;
} g_convert;

bool ConvertOne(const fs::path& in, const fs::path& out, int mode, std::string& msg) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(in.string(), bytes)) { msg = "cannot read"; return false; }
    const bool toIni = mode == 2 || (mode == 0 && IsRegMagic(bytes));
    std::vector<uint8_t> result;
    if (toIni) {
        std::string ini, err;
        if (!IniRegRegToText(bytes, ini, err)) { msg = err; return false; }
        result.assign(ini.begin(), ini.end());
    } else if (!IniRegTextToReg(std::string(bytes.begin(), bytes.end()), result)) { msg = "not a valid INI"; return false; }
    std::error_code ec;
    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
    if (!WriteFileBytes(out.string(), result)) { msg = "cannot write"; return false; }
    msg = toIni ? "-> INI" : "-> REG";
    return true;
}

void RunConversion() {
    g_convert.results.clear();
    const fs::path in = g_convert.input, outArg = g_convert.output;
    std::error_code ec;
    if (in.empty() || !fs::exists(in, ec)) { g_convert.results.push_back(i18n::Tr("The input does not exist.")); return; }
    auto swapped = [&](const fs::path& p, int mode, const fs::path& base, const fs::path& outBase) {
        std::vector<uint8_t> head; // the target extension: the opposite of the source's kind
        ReadFileBytes(p.string(), head);
        const bool toIni = mode == 2 || (mode == 0 && IsRegMagic(head));
        fs::path rel = base.empty() ? p.filename() : fs::relative(p, base, ec);
        fs::path dest = (outBase.empty() ? p.parent_path() : outBase) / rel;
        dest.replace_extension(toIni ? ".ini" : ".reg");
        return dest;
    };
    int ok = 0, bad = 0;
    auto doFile = [&](const fs::path& p, const fs::path& dest) {
        std::string msg;
        const bool done = ConvertOne(p, dest, g_convert.mode, msg);
        (done ? ok : bad)++;
        if (g_convert.results.size() < 400) g_convert.results.push_back((done ? "ok  " : "FAILED  ") + p.string() + "  " + msg);
    };
    if (fs::is_directory(in, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(in, ec)) {
            if (!e.is_regular_file()) continue;
            const std::string ext = LowerCopy(e.path().extension().string());
            if (ext != ".ini" && ext != ".reg") continue;
            if (g_convert.mode == 1 && ext != ".ini") continue;
            if (g_convert.mode == 2 && ext != ".reg") continue;
            doFile(e.path(), swapped(e.path(), g_convert.mode, in, outArg));
        }
    } else {
        const fs::path dest = outArg.empty() ? swapped(in, g_convert.mode, fs::path(), fs::path())
                              : (fs::is_directory(outArg, ec) ? swapped(in, g_convert.mode, fs::path(), outArg) : outArg);
        doFile(in, dest);
    }
    g_convert.results.insert(g_convert.results.begin(), std::string(i18n::Tr("Converted: ")) + std::to_string(ok) + (bad ? std::string(i18n::Tr(", failed: ")) + std::to_string(bad) : ""));
}

void DrawConvertPopup() {
    ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Convert INI <-> REG files", nullptr)) return;
    ImGui::TextWrapped("%s", i18n::Tr("Converts a file, or every .ini and .reg of a folder (and its subfolders), to the other format. With no output the new files are made beside the originals."));
    auto row = [](const char* id, const char* label, char* buf, size_t size, bool folderToo) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(80);
        ImGui::SetNextItemWidth(-190.0f);
        ImGui::InputText("##p", buf, size);
        std::string picked;
        ImGui::SameLine();
        if (ImGui::Button("File...") && ui::PickFile(picked)) std::snprintf(buf, size, "%s", picked.c_str());
        if (folderToo) { ImGui::SameLine(); if (ImGui::Button("Folder...") && ui::PickFolder(picked)) std::snprintf(buf, size, "%s", picked.c_str()); }
        ImGui::PopID();
    };
    row("in", "Input:", g_convert.input, sizeof g_convert.input, true);
    row("out", "Output:", g_convert.output, sizeof g_convert.output, true);
    ImGui::RadioButton("Decide by each file", &g_convert.mode, 0); ImGui::SameLine();
    ImGui::RadioButton("INI -> REG", &g_convert.mode, 1); ImGui::SameLine();
    ImGui::RadioButton("REG -> INI", &g_convert.mode, 2);
    if (ImGui::Button("Convert", ImVec2(120, 0))) RunConversion();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::BeginChild("##convlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        i18n::Verbatim verbatim;
        for (const std::string& l : g_convert.results) ImGui::TextUnformatted(l.c_str());
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

} // namespace

bool OpenFile(const std::string& path) { return LoadFile(path); }

void DrawTab() {
    // the file bar
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File:");
    ImGui::SameLine(60);
    ImGui::SetNextItemWidth(-520.0f);
    const bool enter = ImGui::InputText("##path", g_pathBuf, sizeof g_pathBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    std::string picked;
    ImGui::SameLine();
    if (ImGui::Button("File...") && ui::PickFile(picked)) { std::snprintf(g_pathBuf, sizeof g_pathBuf, "%s", picked.c_str()); LoadFile(picked); }
    ImGui::SameLine();
    if (ImGui::Button("Open") || enter) LoadFile(g_pathBuf);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_doc.kind == FileKind::None || g_doc.path.empty());
    if (ImGui::Button("Save")) SaveTo(g_doc.path);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_doc.kind == FileKind::None);
    if (ImGui::Button("Save as...")) {
        const std::string suggested = g_doc.path.empty() ? std::string("new.ini") : g_doc.path;
        std::string out;
        if (ui::PickSaveFile(suggested, out, g_doc.kind == FileKind::Reg ? "reg" : "ini")) SaveTo(out);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::Tr("A name ending in .reg is saved as the game's binary format, any other name as text."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        Doc d; d.kind = FileKind::Ini; d.cp1251 = true; g_doc = d; g_text.clear(); g_selected = 0; g_editId = 0; g_pathBuf[0] = '\0'; Say(i18n::Tr("A new empty file: add a section, then save it with Save as."));
    }
    ImGui::SameLine();
    if (ImGui::Button("Convert...")) ImGui::OpenPopup("Convert INI <-> REG files");
    DrawConvertPopup();

    // the state line
    if (g_doc.kind == FileKind::None) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", i18n::Tr("Open an .ini or a .reg to read and edit it: a .reg is shown as text, and saved as a .reg again when its name ends in .reg. New starts an empty file; Convert... converts files and folders."));
        if (!g_status.empty()) ImGui::TextColored(g_statusError ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.6f, 0.85f, 0.6f, 1), "%s", g_status.c_str());
        return;
    }
    const std::vector<Section> sections = Sections(g_doc.lines);
    int keys = 0;
    for (const Section& s : sections) keys += s.keys;
    ImGui::TextDisabled("%s%s  |  %s  |  %zu %s, %d %s",
                        i18n::Tr(g_doc.kind == FileKind::Reg ? "REG (binary)" : "INI (text)"), g_doc.dirty ? " *" : "",
                        g_doc.cp1251 ? "CP1251" : "UTF-8", sections.size(), i18n::Tr("sections"), keys, i18n::Tr("keys"));
    if (!g_doc.warning.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "  %s", g_doc.warning.c_str()); }
    if (!g_status.empty()) ImGui::TextColored(g_statusError ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.6f, 0.85f, 0.6f, 1), "%s", g_status.c_str());

    // the views
    int view = g_view;
    if (ImGui::RadioButton("Keys", view == 0)) view = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Text", view == 1)) view = 1;
    if (view != g_view) {
        if (view == 1) SyncToText(); else SyncFromText();
        g_view = view; g_editId = 0;
    }
    const float height = ImGui::GetContentRegionAvail().y - 4.0f;
    if (g_view == 0) DrawKeysView(height);
    else {
        i18n::Verbatim verbatim;
        g_UmColorLine = ColorLine;
        const bool edited = ImGui::InputTextMultiline("##text", g_text.data(), g_text.capacity() + 1, ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_CallbackResize,
                                      [](ImGuiInputTextCallbackData* d) -> int {
                                          if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) { auto* s = static_cast<std::string*>(d->UserData); s->resize(static_cast<size_t>(d->BufTextLen)); d->Buf = s->data(); }
                                          return 0;
                                      }, &g_text);
        g_UmColorLine = nullptr;
        if (edited) g_doc.dirty = true;
    }
}

} // namespace inireg_edit
