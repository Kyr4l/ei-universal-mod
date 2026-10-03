// The Texts sub-tab: see text_editor.hpp.
//
// A text is one file named by its key ("WEAPON Stone_Sword Malachite", "zone z3q3"...): its first line
// is a name, the rest a description. A language pack is the files of its folders and .res archives
// (Settings > Texts, TEXT_PACK lines); a key is the file name, case ignored. Compared with the
// reference pack, a pack may lack texts (to translate) or have texts the reference has not: those are
// not mistakes as such - a language can fix a base-game text of its own - so they are only listed.
//
// Each pack keeps its encoding when a text is saved: UTF-8 (the mod's French), CP949 (Korean) or
// CP1251 (English and the Russian game), and each file its line ends (CRLF or LF). Texts inside .res
// archives are read-only here (unpack the archive with RES / MQ to edit them).

#include "text_editor.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

#include "imgui.h"

#include "viewer/item_texts.hpp"
#include "viewer/library.hpp"
#include "viewer/res_archive.hpp"

namespace fs = std::filesystem;

namespace textedit {

namespace {

const ImVec4 kMissingColor(0.95f, 0.42f, 0.38f, 1), kOnlyHereColor(0.5f, 0.75f, 1.0f, 1), kSameColor(0.6f, 0.6f, 0.6f, 1),
    kOkColor(0.6f, 0.85f, 0.6f, 1);

enum class Encoding { Ascii, Utf8, Cp949, Cp1251 };

const char* EncodingName(Encoding e) {
    switch (e) {
    case Encoding::Ascii: return "ASCII";
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Cp949: return "CP949 (Korean)";
    default: return "CP1251";
    }
}

Encoding Detect(const std::vector<uint8_t>& b) {
    if (std::all_of(b.begin(), b.end(), [](uint8_t c) { return c < 0x80; })) return Encoding::Ascii;
    if (texts::IsValidUtf8(b)) return Encoding::Utf8;
    if (texts::LooksKorean(b)) return Encoding::Cp949;
    return Encoding::Cp1251;
}

// UTF-8 text into the encoding; `lost` counts the characters it has not (written as '?').
std::vector<uint8_t> Encode(const std::string& utf8, Encoding e, int& lost) {
    lost = 0;
    if (e == Encoding::Utf8) return std::vector<uint8_t>(utf8.begin(), utf8.end());
    static std::unordered_map<uint32_t, uint16_t> cp949;
    if (e == Encoding::Cp949 && cp949.empty())
        for (uint16_t lead = cp949::kLeadFirst; lead < cp949::kLeadFirst + 126; ++lead)
            for (uint16_t k = 0; k < cp949::kTrailCount; ++k) {
                const uint32_t cp = cp949::kTable[(lead - cp949::kLeadFirst) * cp949::kTrailCount + k];
                if (cp) cp949.emplace(cp, static_cast<uint16_t>((lead << 8) | (cp949::kTrailFirst + k)));
            }
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = cp1251::DecodeUtf8(utf8, i);
        if (cp < 0x80) { out.push_back(static_cast<uint8_t>(cp)); continue; }
        if (e == Encoding::Cp949) {
            auto it = cp949.find(cp);
            if (it != cp949.end()) { out.push_back(static_cast<uint8_t>(it->second >> 8)); out.push_back(static_cast<uint8_t>(it->second)); continue; }
        } else {
            const uint8_t b = cp1251::CodepointToByte(cp);
            if (b != '?') { out.push_back(b); continue; }
        }
        out.push_back('?');
        ++lost;
    }
    return out;
}

struct TextFile {
    std::string name;  // as in the folder or archive
    int source = 0;    // index into the pack's sources
    std::string text;  // UTF-8, line ends as '\n'
    Encoding encoding = Encoding::Ascii;
    bool crlf = false, finalNewline = false;
};

struct Source {
    std::string path;
    bool folder = false, ok = false;
    std::string error;
};

struct Pack {
    std::string language;
    std::vector<Source> sources;
    std::map<std::string, TextFile> files; // by lower-case key
    Encoding encoding = Encoding::Cp1251;   // what its texts use (the most common non-ASCII one)
    bool crlf = true;
    int overridden = 0;                     // keys found in two of its sources (the later one is kept)
};

std::string Lower(std::string s) { return res::Archive::ToLower(s); }

void AddFile(Pack& p, int source, const std::string& name, const std::vector<uint8_t>& bytes) {
    TextFile f;
    f.name = name;
    f.source = source;
    f.encoding = Detect(bytes);
    const std::string raw = texts::DecodeToUtf8(bytes);
    f.crlf = raw.find("\r\n") != std::string::npos;
    for (char c : raw) if (c != '\r') f.text += c;
    f.finalNewline = !f.text.empty() && f.text.back() == '\n';
    if (f.finalNewline) f.text.pop_back();
    if (p.files.count(Lower(name))) ++p.overridden;
    p.files[Lower(name)] = std::move(f);
}

void LoadPack(Pack& p) {
    p.files.clear();
    p.overridden = 0;
    for (size_t i = 0; i < p.sources.size(); ++i) {
        Source& s = p.sources[i];
        std::error_code ec;
        s.folder = fs::is_directory(s.path, ec);
        s.ok = false;
        if (s.folder) {
            for (const auto& entry : fs::directory_iterator(s.path, ec)) {
                if (!entry.is_regular_file()) continue;
                std::ifstream f(entry.path(), std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                AddFile(p, static_cast<int>(i), entry.path().filename().string(), bytes);
            }
            s.ok = true;
        } else if (fs::is_regular_file(s.path, ec)) {
            std::ifstream f(s.path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            res::Archive archive;
            if (res::ParseArchive(bytes, archive, s.error)) {
                for (const auto& [lower, e] : archive.entries) AddFile(p, static_cast<int>(i), e.originalName, e.data);
                s.ok = true;
            }
        } else {
            s.error = "not found";
        }
    }
    // The pack's encoding and line ends: those of most of its texts.
    std::map<Encoding, int> encodings;
    int crlf = 0;
    for (const auto& [k, f] : p.files) {
        if (f.encoding != Encoding::Ascii) ++encodings[f.encoding];
        crlf += f.crlf ? 1 : -1;
    }
    p.encoding = Encoding::Cp1251;
    int best = 0;
    for (const auto& [e, n] : encodings) if (n > best) { best = n; p.encoding = e; }
    p.crlf = crlf >= 0;
}

enum class Status { Ok, Missing, OnlyHere, Same };

struct State {
    int packsVersion = -1;
    std::vector<Pack> packs;
    int pack = 0, reference = 0;
    char filter[256] = "";
    bool showOk = true, showMissing = true, showOnlyHere = true, showSame = true;
    std::string selected;  // lower-case key
    std::string edit;      // the text being edited (UTF-8)
    std::string editFor;   // pack language + key it was loaded for
    std::string message;
    bool messageIsError = false;
};
State g;

void Say(const std::string& m, bool error = false) { g.message = m; g.messageIsError = error; }

void Reload(Library& lib) {
    std::vector<Pack> packs;
    for (const auto& [language, path] : lib.textPacks) {
        auto it = std::find_if(packs.begin(), packs.end(), [&](const Pack& p) { return p.language == language; });
        if (it == packs.end()) { packs.push_back({}); it = packs.end() - 1; it->language = language; }
        Source source;
        source.path = path;
        it->sources.push_back(source);
    }
    for (Pack& p : packs) LoadPack(p);
    const std::string shown = g.pack < static_cast<int>(g.packs.size()) ? g.packs[g.pack].language : "";
    g.packs = std::move(packs);
    g.pack = g.reference = 0;
    for (size_t i = 0; i < g.packs.size(); ++i) {
        if (g.packs[i].language == lib.textReference) g.reference = static_cast<int>(i);
        if (g.packs[i].language == shown) g.pack = static_cast<int>(i);
    }
    // No reference chosen yet: English when there is one.
    if (lib.textReference.empty())
        for (size_t i = 0; i < g.packs.size(); ++i)
            if (Lower(g.packs[i].language).rfind("en", 0) == 0) g.reference = static_cast<int>(i);
    if (g.pack == g.reference && g.packs.size() > 1) g.pack = g.reference == 0 ? 1 : 0;
    g.editFor.clear();
    g.packsVersion = lib.textPacksVersion;
}

Status StatusOf(const std::string& key) {
    const Pack& p = g.packs[g.pack];
    const Pack& r = g.packs[g.reference];
    auto here = p.files.find(key);
    auto ref = r.files.find(key);
    if (here == p.files.end()) return Status::Missing;
    if (ref == r.files.end()) return Status::OnlyHere;
    if (g.pack != g.reference && here->second.text == ref->second.text) return Status::Same;
    return Status::Ok;
}

// The file written for a key in the shown pack: its own, or (missing) the reference's name in the
// pack's source at the same place as the reference's, else its first folder. Empty: nowhere to write.
std::string TargetPath(const std::string& key, std::string& name) {
    const Pack& p = g.packs[g.pack];
    auto here = p.files.find(key);
    if (here != p.files.end()) {
        name = here->second.name;
        const Source& s = p.sources[here->second.source];
        return s.folder ? (fs::path(s.path) / name).string() : std::string();
    }
    const Pack& r = g.packs[g.reference];
    auto ref = r.files.find(key);
    if (ref == r.files.end()) return {};
    name = ref->second.name;
    const int index = ref->second.source;
    if (index < static_cast<int>(p.sources.size()) && p.sources[index].folder) return (fs::path(p.sources[index].path) / name).string();
    for (const Source& s : p.sources)
        if (s.folder) return (fs::path(s.path) / name).string();
    return {};
}

void Save(const std::string& key) {
    Pack& p = g.packs[g.pack];
    std::string name;
    const std::string path = TargetPath(key, name);
    if (path.empty()) { Say("This pack has no folder to write into (texts inside a .res are read-only: unpack it with RES / MQ).", true); return; }
    auto here = p.files.find(key);
    const bool exists = here != p.files.end();
    const Encoding enc = exists && here->second.encoding != Encoding::Ascii ? here->second.encoding : p.encoding;
    const bool crlf = exists ? here->second.crlf : p.crlf;
    const bool finalNewline = exists ? here->second.finalNewline : false;
    std::string text = g.edit;
    std::string out;
    for (char c : text) {
        if (c == '\n' && crlf) out += '\r';
        out += c;
    }
    if (finalNewline) out += crlf ? "\r\n" : "\n";
    int lost = 0;
    const std::vector<uint8_t> bytes = Encode(out, enc == Encoding::Ascii ? Encoding::Cp1251 : enc, lost);
    std::ofstream f(path, std::ios::binary);
    if (!f) { Say("Cannot write " + path, true); return; }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    // Keep the pack in step without reading everything again.
    int source = 0;
    for (size_t i = 0; i < p.sources.size(); ++i)
        if (p.sources[i].folder && fs::path(path).parent_path() == fs::path(p.sources[i].path)) source = static_cast<int>(i);
    p.files.erase(key); // replaced, not an override
    AddFile(p, source, name, bytes);
    Say("Saved " + path + " (" + EncodingName(Detect(bytes) == Encoding::Ascii ? Encoding::Ascii : enc) + ")" +
            (lost ? ": " + std::to_string(lost) + " character(s) this encoding has not became '?'" : ""),
        lost > 0);
}

bool InputMultiline(const char* id, std::string& text, ImVec2 size, ImGuiInputTextFlags flags = 0) {
    struct Resize {
        static int Callback(ImGuiInputTextCallbackData* d) {
            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                auto* s = static_cast<std::string*>(d->UserData);
                s->resize(static_cast<size_t>(d->BufTextLen));
                d->Buf = s->data();
            }
            return 0;
        }
    };
    return ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize,
                                     Resize::Callback, &text);
}

} // namespace

void DrawTab(Library& lib) {
    if (g.packsVersion != lib.textPacksVersion) Reload(lib);
    if (g.packs.empty()) {
        ImGui::TextWrapped("No language packs yet: add them in Settings > Texts (a language and its folders or .res files). This tab then compares each language with a reference one: the texts it lacks (to translate), the texts only it has (often fixes for that language, not mistakes) and the texts left identical.");
        return;
    }
    // Pack and reference.
    auto combo = [&](const char* label, int& index) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        bool changed = false;
        if (ImGui::BeginCombo((std::string("##") + label).c_str(), g.packs[index].language.c_str())) {
            for (size_t i = 0; i < g.packs.size(); ++i)
                if (ImGui::Selectable(g.packs[i].language.c_str(), index == static_cast<int>(i))) { index = static_cast<int>(i); changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    };
    combo("Language", g.pack);
    ImGui::SameLine();
    if (combo("Reference", g.reference)) { lib.textReference = g.packs[g.reference].language; lib.SaveConfig(); }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { Reload(lib); Say("Read again."); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the packs' files again (after changing them elsewhere)");
    const Pack& p = g.packs[g.pack];
    const Pack& r = g.packs[g.reference];

    // Every key of the two packs, with its status.
    std::set<std::string> keys;
    for (const auto& [k, f] : p.files) keys.insert(k);
    for (const auto& [k, f] : r.files) keys.insert(k);
    int missing = 0, onlyHere = 0, same = 0;
    for (const std::string& k : keys) {
        const Status s = StatusOf(k);
        missing += s == Status::Missing;
        onlyHere += s == Status::OnlyHere;
        same += s == Status::Same;
    }
    ImGui::SameLine();
    ImGui::Text("%s: %zu texts (%s%s)", p.language.c_str(), p.files.size(), EncodingName(p.encoding), p.crlf ? ", CRLF" : "");
    for (const Source& s : p.sources)
        if (!s.ok) { ImGui::SameLine(); ImGui::TextColored(kMissingColor, "%s: %s", s.path.c_str(), s.error.c_str()); }
    if (p.overridden) { ImGui::SameLine(); ImGui::TextDisabled("(%d key(s) in two of its sources: the later one is used)", p.overridden); }

    // Filters.
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filter", "Filter (key or text)", g.filter, sizeof(g.filter));
    if (g.pack != g.reference) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kMissingColor);
        ImGui::Checkbox(("Missing (" + std::to_string(missing) + ")").c_str(), &g.showMissing);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("In %s, not in %s: to translate", r.language.c_str(), p.language.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kOnlyHereColor);
        ImGui::Checkbox(("Only in " + p.language + " (" + std::to_string(onlyHere) + ")").c_str(), &g.showOnlyHere);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Not in %s. Often intentional: a fix of a base-game text for this language only.", r.language.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kSameColor);
        ImGui::Checkbox(("Same text (" + std::to_string(same) + ")").c_str(), &g.showSame);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Identical to %s: not translated yet, or names that stay the same", r.language.c_str());
        ImGui::SameLine();
        ImGui::Checkbox("Others", &g.showOk);
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(the reference itself: choose another language to compare)");
    }

    const float listWidth = std::max(260.0f, ImGui::GetContentRegionAvail().x * 0.32f);
    const float height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##keys", ImVec2(listWidth, height), ImGuiChildFlags_Borders);
    const std::string filter = Lower(g.filter);
    std::vector<std::pair<std::string, Status>> shown;
    for (const std::string& k : keys) {
        const Status s = StatusOf(k);
        if (g.pack == g.reference && s == Status::Missing) continue;
        if ((s == Status::Missing && !g.showMissing) || (s == Status::OnlyHere && !g.showOnlyHere) || (s == Status::Same && !g.showSame) ||
            (s == Status::Ok && !g.showOk && g.pack != g.reference))
            continue;
        if (!filter.empty() && k.find(filter) == std::string::npos) {
            auto a = p.files.find(k), b = r.files.find(k);
            const bool inText = (a != p.files.end() && Lower(a->second.text).find(filter) != std::string::npos) ||
                                (b != r.files.end() && Lower(b->second.text).find(filter) != std::string::npos);
            if (!inText) continue;
        }
        shown.push_back({k, s});
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& [k, s] = shown[i];
            const auto a = p.files.find(k);
            const std::string name = a != p.files.end() ? a->second.name : r.files.at(k).name;
            const ImVec4 color = s == Status::Missing ? kMissingColor : s == Status::OnlyHere ? kOnlyHereColor : s == Status::Same ? kSameColor
                                                                                                                                   : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable((name + "##" + k).c_str(), g.selected == k)) g.selected = k;
            ImGui::PopStyleColor();
        }
    if (shown.empty()) ImGui::TextDisabled("Nothing to show");
    ImGui::EndChild();
    ImGui::SameLine();

    // The selected text: the reference's beside this pack's.
    ImGui::BeginChild("##text", ImVec2(0, height));
    if (g.selected.empty() || !keys.count(g.selected)) {
        ImGui::TextDisabled("Choose a text in the list.");
        if (!g.message.empty()) ImGui::TextColored(g.messageIsError ? kMissingColor : kOkColor, "%s", g.message.c_str());
        ImGui::EndChild();
        return;
    }
    const std::string& key = g.selected;
    const Status status = StatusOf(key);
    auto here = p.files.find(key);
    auto ref = r.files.find(key);
    const std::string editKey = p.language + "\n" + key;
    if (g.editFor != editKey) {
        g.edit = here != p.files.end() ? here->second.text : std::string();
        g.editFor = editKey;
    }
    std::string fileName;
    const std::string target = TargetPath(key, fileName);
    ImGui::TextUnformatted(fileName.c_str());
    ImGui::SameLine();
    if (status == Status::Missing) ImGui::TextColored(kMissingColor, "- missing in %s", p.language.c_str());
    else if (status == Status::OnlyHere) ImGui::TextColored(kOnlyHereColor, "- only in %s (not in %s: can be intentional)", p.language.c_str(), r.language.c_str());
    else if (status == Status::Same) ImGui::TextColored(kSameColor, "- same text as %s", r.language.c_str());
    const float half = (ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3) * 0.5f;
    if (g.pack != g.reference) {
        ImGui::TextDisabled("%s (reference)%s", r.language.c_str(), ref != r.files.end() ? "" : ": none");
        std::string refText = ref != r.files.end() ? ref->second.text : std::string();
        InputMultiline("##ref", refText, ImVec2(-1, half), ImGuiInputTextFlags_ReadOnly);
    }
    const bool modified = here != p.files.end() ? g.edit != here->second.text : !g.edit.empty();
    ImGui::TextDisabled("%s%s", p.language.c_str(), modified ? " (modified)" : "");
    const bool writable = !target.empty();
    ImGui::BeginDisabled(!writable);
    InputMultiline("##edit", g.edit, ImVec2(-1, g.pack != g.reference ? half : half * 2));
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!writable || (!modified && here != p.files.end()));
    const bool canSave = writable && (modified || here == p.files.end());
    if (ImGui::Button(here != p.files.end() ? "Save" : "Create") || (canSave && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)))
        Save(key);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", writable ? ("Ctrl+S. Writes " + target + " in " + EncodingName(p.encoding == Encoding::Ascii ? Encoding::Cp1251 : p.encoding) +
                                            " (the pack's encoding)").c_str()
                                         : "Read-only: the text is in a .res archive (unpack it with RES / MQ to edit it)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!modified);
    if (ImGui::Button("Revert")) g.edit = here != p.files.end() ? here->second.text : std::string();
    ImGui::EndDisabled();
    if (ref != r.files.end() && g.pack != g.reference) {
        ImGui::SameLine();
        if (ImGui::Button("Copy the reference text")) g.edit = ref->second.text;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("To translate it from there (then Save / Create)");
    }
    if (!g.message.empty()) ImGui::TextColored(g.messageIsError ? kMissingColor : kOkColor, "%s", g.message.c_str());
    ImGui::EndChild();
}

} // namespace textedit
