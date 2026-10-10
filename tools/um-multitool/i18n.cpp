// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// See i18n.hpp. The table (lang/ru.txt: built in, and read again from beside the program at start, so a
// fix there needs no rebuild) has two kinds of entries:
//   T(english, russian): the whole text drawn, or a whole printf-style format;
//   F(english, russian): a fragment of a text a program builds from pieces ("Opened " + n + " sheet(s)"),
//                        replaced inside any drawn text that is not a T entry.
// Every T key is the text exactly as it is in the source, escapes included (the file is also C++).
#include "i18n.hpp"

#include <cstring>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <list>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace i18n {
namespace {

struct Entry { const char* en; const char* ru; };

#define T(en, ru) {en, ru},
#define F(en, ru)
const Entry kWhole[] = {
#include "lang/ru.txt"
    {nullptr, nullptr}};
#undef T
#undef F
#define T(en, ru)
#define F(en, ru) {en, ru},
const Entry kFragments[] = {
#include "lang/ru.txt"
    {nullptr, nullptr}};
#undef T
#undef F

Lang g_lang = Lang::English;
int g_verbatim = 0; // above 0: drawn texts stay as they are (Verbatim, UmVerbatim)

struct Tables {
    std::unordered_map<std::string_view, std::string_view> whole;
    std::vector<Entry> fragments;                    // longest first
    std::unordered_map<std::string_view, const std::string*> built; // cache of texts translated by fragments
    std::list<std::string> storage;                  // owns the cache's keys and values
};

// lang/ru.txt beside the program.
std::string LanguageFile() {
    char buf[4096] = {};
#ifdef _WIN32
    std::string p(buf, GetModuleFileNameA(nullptr, buf, sizeof buf - 1));
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p(buf, n > 0 ? static_cast<size_t>(n) : 0);
#endif
    const size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos ? std::string(".") : p.substr(0, slash)) + "/lang/ru.txt";
}
// A C string literal at s[i] (i at the opening quote); false when there is none.
bool ReadLiteral(const std::string& s, size_t& i, std::string& out) {
    while (i < s.size() && s[i] != '"') ++i;
    if (i >= s.size()) return false;
    out.clear();
    for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
        } else {
            out += s[i];
        }
    }
    if (i >= s.size()) return false;
    ++i;
    return true;
}
std::list<std::string>& FileStrings() { static std::list<std::string> s; return s; } // owns the file's texts

Tables& Get() {
    static Tables t = [] {
        Tables x;
        for (const Entry& e : kWhole) if (e.en) x.whole.emplace(e.en, e.ru);
        std::unordered_map<std::string_view, std::string_view> fragments;
        for (const Entry& e : kFragments) if (e.en) fragments.emplace(e.en, e.ru);
        // The file beside the program wins over the built-in copy (its fixes need no rebuild).
        std::ifstream in(LanguageFile());
        std::string line, en, ru;
        while (std::getline(in, line)) {
            if (line.size() < 2 || (line[0] != 'T' && line[0] != 'F') || line[1] != '(') continue;
            size_t i = 2;
            if (!ReadLiteral(line, i, en) || !ReadLiteral(line, i, ru)) continue;
            auto& strings = FileStrings();
            strings.push_back(en);
            const std::string_view key(strings.back());
            strings.push_back(ru);
            const std::string_view value(strings.back());
            if (line[0] == 'T') x.whole[key] = value;
            else fragments[key] = value;
        }
        for (const auto& [e, r] : fragments) x.fragments.push_back({e.data(), r.data()});
        std::stable_sort(x.fragments.begin(), x.fragments.end(),
                         [](const Entry& a, const Entry& b) { return std::strlen(a.en) > std::strlen(b.en); });
        return x;
    }();
    return t;
}

// Replaces every fragment found in `text`; returns false when none was.
bool ApplyFragments(const Tables& t, std::string_view text, std::string& out) {
    out.clear();
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        bool hit = false;
        for (const Entry& f : t.fragments) {
            const size_t n = std::strlen(f.en);
            if (n && text.compare(i, n, f.en) == 0) {
                out += f.ru;
                i += n;
                hit = any = true;
                break;
            }
        }
        if (!hit) out += text[i++];
    }
    return any;
}

} // namespace

Lang Current() { return g_lang; }

void Set(Lang lang) {
    g_lang = lang;
    Tables& t = Get();
    t.built.clear();
    t.storage.clear();
}

bool FromCode(const std::string& code, Lang& out) {
    if (code == "en") { out = Lang::English; return true; }
    if (code == "ru") { out = Lang::Russian; return true; }
    return false;
}

const char* Tr(const char* english) {
    if (g_lang == Lang::English || !english) return english;
    Tables& t = Get();
    auto it = t.whole.find(english);
    return it == t.whole.end() ? english : it->second.data(); // the table's values are NUL-terminated literals
}

Verbatim::Verbatim() { ++g_verbatim; }
Verbatim::~Verbatim() { --g_verbatim; }

} // namespace i18n

// ---- hooks called from vendor/imgui -------------------------------------------------------------

// A drawn text [begin, end): swapped for its translation when it has one. The result stays valid
// until the language changes or the cache is cleared (a frame later at the earliest).
bool UmTranslateText(const char*& begin, const char*& end) {
    using namespace i18n;
    if (g_lang == Lang::English || g_verbatim > 0 || end - begin < 2) return false;
    Tables& t = Get();
    const std::string_view text(begin, static_cast<size_t>(end - begin));
    auto it = t.whole.find(text);
    if (it != t.whole.end()) {
        begin = it->second.data();
        end = begin + it->second.size();
        return true;
    }
    auto c = t.built.find(text);
    if (c == t.built.end()) {
        if (t.built.size() > 8192) { t.built.clear(); t.storage.clear(); } // texts that change every frame
        std::string out;
        const bool any = ApplyFragments(t, text, out);
        t.storage.push_back(any ? std::move(out) : std::string());
        const std::string* value = &t.storage.back();
        t.storage.push_back(std::string(text));
        c = t.built.emplace(std::string_view(t.storage.back()), any ? value : nullptr).first;
    }
    if (!c->second) return false;
    begin = c->second->data();
    end = begin + c->second->size();
    return true;
}

void UmVerbatim(int delta) { i18n::g_verbatim += delta; }

const char* UmTranslateFmt(const char* fmt) {
    if (i18n::Current() == i18n::Lang::English) return fmt;
    return i18n::Tr(fmt);
}
