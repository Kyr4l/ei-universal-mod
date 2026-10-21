// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The mod updater: asks GitHub for the newest release (or pre-release) of a mod's repository, downloads its Universal-Mod*.zip, unpacks it and
// replaces the mod's folder, keeping `mp` and `saves` (the saves). The old folder is renamed first and put back when anything fails.
// HTTPS goes through WinINet (Windows, Wine); the JSON, zip reading (stored / deflate, CRC checked) and the folder swap are plain C++.
// Windows XP cannot speak the TLS GitHub requires: the check then fails with a message.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "inflate.hpp"
#include "inifile.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

namespace launcher::update {

namespace fs = std::filesystem;

// ---- JSON (just enough for GitHub's release list) ------------------------------------------------------------------------------------------
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* Get(const std::string& key) const { for (const auto& m : members) if (m.first == key) return &m.second; return nullptr; }
    std::string Str(const std::string& key) const { const Json* j = Get(key); return j && j->type == Type::String ? j->text : std::string(); }
    bool Bool(const std::string& key) const { const Json* j = Get(key); return j && j->type == Type::Bool && j->boolean; }
    double Num(const std::string& key) const { const Json* j = Get(key); return j && j->type == Type::Number ? j->number : 0; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool Parse(Json& out) { Skip(); if (!Value(out, 0)) return false; Skip(); return true; }

private:
    void Skip() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_; }
    bool Lit(const char* w) { const size_t n = std::strlen(w); if (s_.compare(p_, n, w) != 0) return false; p_ += n; return true; }
    bool Str(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_; out.clear();
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c != '\\') { out += c; continue; }
            if (p_ >= s_.size()) return false;
            c = s_[p_++];
            switch (c) {
            case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': {
                if (p_ + 4 > s_.size()) return false;
                uint32_t cp = static_cast<uint32_t>(std::strtoul(s_.substr(p_, 4).c_str(), nullptr, 16)); p_ += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(p_, 2, "\\u") == 0 && p_ + 6 <= s_.size()) {
                    const uint32_t lo = static_cast<uint32_t>(std::strtoul(s_.substr(p_ + 2, 4).c_str(), nullptr, 16));
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p_ += 6; }
                }
                AppendUtf8(out, cp); break;
            }
            default: out += c;
            }
        }
        if (p_ >= s_.size()) return false;
        ++p_;
        return true;
    }
    bool Value(Json& j, int depth) {
        if (depth > 64 || p_ >= s_.size()) return false;
        const char c = s_[p_];
        if (c == '{') {
            ++p_; j.type = Json::Type::Object; Skip();
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
            for (;;) {
                Skip(); std::string key; if (!Str(key)) return false;
                Skip(); if (p_ >= s_.size() || s_[p_++] != ':') return false;
                Skip(); Json v; if (!Value(v, depth + 1)) return false;
                j.members.emplace_back(std::move(key), std::move(v));
                Skip(); if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_; j.type = Json::Type::Array; Skip();
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
            for (;;) {
                Skip(); Json v; if (!Value(v, depth + 1)) return false;
                j.items.push_back(std::move(v));
                Skip(); if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { j.type = Json::Type::String; return Str(j.text); }
        if (Lit("true")) { j.type = Json::Type::Bool; j.boolean = true; return true; }
        if (Lit("false")) { j.type = Json::Type::Bool; j.boolean = false; return true; }
        if (Lit("null")) { j.type = Json::Type::Null; return true; }
        char* end = nullptr;
        j.number = std::strtod(s_.c_str() + p_, &end);
        if (end == s_.c_str() + p_) return false;
        j.type = Json::Type::Number; p_ = static_cast<size_t>(end - s_.c_str());
        return true;
    }
    const std::string& s_;
    size_t p_ = 0;
};

// ---- the release to install ----------------------------------------------------------------------------------------------------------------
struct Release {
    std::string tag, name, published;
    bool prerelease = false;
    std::string assetName, assetUrl, assetUpdated;
    uint64_t assetSize = 0;
    // What identifies this exact build (a pre-release's asset is replaced after each commit under the same tag).
    std::string Key() const { return tag + "|" + assetUpdated + "|" + std::to_string(assetSize); }
};

inline bool IsModZip(const std::string& name) {
    const std::string l = LowerAscii(name);
    return l.compare(0, 13, "universal-mod") == 0 && l.size() > 4 && l.compare(l.size() - 4, 4, ".zip") == 0;
}

// The newest release of the list (GitHub lists newest first) that is not a draft, is a release when `stableOnly`, and has a Universal-Mod*.zip.
inline bool PickRelease(const std::string& json, bool stableOnly, Release& out, std::string& error) {
    Json root;
    JsonParser parser(json);
    if (!parser.Parse(root) || root.type != Json::Type::Array) {
        const std::string msg = root.type == Json::Type::Object ? root.Str("message") : std::string();
        error = msg.empty() ? "GitHub's answer was not a release list" : "GitHub says: " + msg;
        return false;
    }
    for (const Json& r : root.items) {
        if (r.Bool("draft") || (stableOnly && r.Bool("prerelease"))) continue;
        const Json* assets = r.Get("assets");
        if (!assets) continue;
        for (const Json& a : assets->items) {
            const std::string name = a.Str("name");
            if (!IsModZip(name)) continue;
            out.tag = r.Str("tag_name"); out.name = r.Str("name"); out.published = r.Str("published_at"); out.prerelease = r.Bool("prerelease");
            out.assetName = name; out.assetUrl = a.Str("browser_download_url"); out.assetUpdated = a.Str("updated_at");
            out.assetSize = static_cast<uint64_t>(a.Num("size"));
            return !out.assetUrl.empty();
        }
    }
    error = stableOnly ? "no release with a Universal-Mod zip" : "no release or pre-release with a Universal-Mod zip";
    return false;
}

// ---- HTTP ----------------------------------------------------------------------------------------------------------------------------------
using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

#ifdef _WIN32
struct Net {
    HINTERNET session = nullptr;
    Net() { session = InternetOpenA("EIStarter-UM", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0); }
    ~Net() { if (session) InternetCloseHandle(session); }
};
inline std::string LastNetError(const std::string& what) {
    char buf[256] = {};
    DWORD code = GetLastError(), len = sizeof buf;
    if (code == ERROR_INTERNET_EXTENDED_ERROR) { DWORD e = 0; InternetGetLastResponseInfoA(&e, buf, &len); }
    return std::string(what) + " (Windows error " + std::to_string(code) + (buf[0] ? std::string(": ") + buf : std::string()) + ")";
}
// Downloads `url`: into `body` (when path is empty) or into the file `path`. 0 total = unknown size.
inline bool HttpFetch(const std::string& url, std::string* body, const fs::path& path, const ProgressFn& progress, const std::atomic<bool>& cancel, std::string& error) {
    Net net;
    if (!net.session) { error = LastNetError("cannot start the internet library"); return false; }
    const char* headers = "Accept: application/vnd.github+json\r\n";
    HINTERNET req = InternetOpenUrlA(net.session, url.c_str(), headers, static_cast<DWORD>(-1), INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!req) { error = LastNetError("cannot connect to " + url); return false; }
    DWORD status = 0, size = sizeof status;
    HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr);
    if (status != 200) { error = "the server answered " + std::to_string(status) + " for " + url; InternetCloseHandle(req); return false; }
    char lenText[32] = {}; DWORD lenSize = sizeof lenText; uint64_t total = 0;
    if (HttpQueryInfoA(req, HTTP_QUERY_CONTENT_LENGTH, lenText, &lenSize, nullptr)) total = std::strtoull(lenText, nullptr, 10);
    std::ofstream out;
    if (!path.empty()) { out.open(path, std::ios::binary | std::ios::trunc); if (!out) { error = "cannot write " + path.string(); InternetCloseHandle(req); return false; } }
    std::vector<char> buf(256 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        if (cancel) { error = "cancelled"; ok = false; break; }
        DWORD got = 0;
        if (!InternetReadFile(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) { error = LastNetError("the download broke off"); ok = false; break; }
        if (got == 0) break;
        if (body) body->append(buf.data(), got); else out.write(buf.data(), got);
        done += got;
        if (progress) progress(done, total);
        if (!path.empty() && !out) { error = "cannot write " + path.string() + " (disk full?)"; ok = false; break; }
    }
    InternetCloseHandle(req);
    if (ok && total && done != total) { error = "the download is incomplete (" + std::to_string(done) + " of " + std::to_string(total) + " bytes)"; ok = false; }
    return ok;
}
#else
inline bool HttpFetch(const std::string&, std::string*, const fs::path&, const ProgressFn&, const std::atomic<bool>&, std::string& error) { error = "downloads need Windows or Wine"; return false; }
#endif

// ---- zip -----------------------------------------------------------------------------------------------------------------------------------
inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } made = true; }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// A path inside the archive is safe when it is relative and has no ".." or drive part.
inline bool SafeRelative(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || (name.size() > 1 && name[1] == ':')) return false;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find_first_of("/\\", i);
        if (j == std::string::npos) j = name.size();
        if (name.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

// Unpacks `zipPath` into `dest` (created). The first folder of the archive is dropped when every entry sits in the same one (the release zips hold
// "Universal-Mod/..."). Stored and deflated entries only; every file's CRC is checked. Returns the number of files.
inline bool ExtractZip(const fs::path& zipPath, const fs::path& dest, const std::function<void(size_t, size_t)>& progress, const std::atomic<bool>& cancel, size_t& files, std::string& error) {
    std::ifstream in(zipPath, std::ios::binary | std::ios::ate);
    if (!in) { error = "cannot open " + zipPath.string(); return false; }
    const int64_t size = static_cast<int64_t>(in.tellg());
    auto read = [&](int64_t at, void* buf, size_t n) { in.clear(); in.seekg(at); in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n)); return static_cast<size_t>(in.gcount()) == n; };
    auto u16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
    auto u32 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); };
    // End of central directory.
    const int64_t tail = std::min<int64_t>(size, 66000);
    std::vector<uint8_t> t(static_cast<size_t>(tail));
    if (!read(size - tail, t.data(), t.size())) { error = "cannot read the zip's end"; return false; }
    int64_t eocd = -1;
    for (int64_t i = tail - 22; i >= 0; --i) if (u32(&t[static_cast<size_t>(i)]) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) { error = "not a zip file"; return false; }
    const uint8_t* e = &t[static_cast<size_t>(eocd)];
    const uint32_t count = u16(e + 10), cdSize = u32(e + 12), cdOffset = u32(e + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) { error = "zip64 archives are not supported"; return false; }
    std::vector<uint8_t> cd(cdSize);
    if (!read(cdOffset, cd.data(), cd.size())) { error = "cannot read the zip's directory"; return false; }
    struct Entry { std::string name; uint32_t method, crc, csize, usize, offset, flags; };
    std::vector<Entry> entries;
    for (size_t p = 0, n = 0; n < count; ++n) {
        if (p + 46 > cd.size() || u32(&cd[p]) != 0x02014b50u) { error = "damaged zip directory"; return false; }
        Entry en;
        en.flags = u16(&cd[p + 8]); en.method = u16(&cd[p + 10]); en.crc = u32(&cd[p + 16]); en.csize = u32(&cd[p + 20]); en.usize = u32(&cd[p + 24]); en.offset = u32(&cd[p + 42]);
        const size_t nlen = u16(&cd[p + 28]), xlen = u16(&cd[p + 30]), clen = u16(&cd[p + 32]);
        if (p + 46 + nlen > cd.size()) { error = "damaged zip directory"; return false; }
        en.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
        for (char& c : en.name) if (c == '\\') c = '/';
        entries.push_back(std::move(en));
        p += 46 + nlen + xlen + clen;
    }
    // A common root folder?
    std::string root;
    bool common = !entries.empty();
    for (const Entry& en : entries) {
        const size_t s = en.name.find('/');
        if (s == std::string::npos) { common = false; break; }
        const std::string first = en.name.substr(0, s);
        if (root.empty()) root = first; else if (root != first) { common = false; break; }
    }
    std::error_code ec;
    fs::create_directories(dest, ec);
    files = 0;
    std::vector<uint8_t> comp;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancel) { error = "cancelled"; return false; }
        const Entry& en = entries[i];
        std::string rel = common ? en.name.substr(root.size() + 1) : en.name;
        if (progress) progress(i, entries.size());
        if (rel.empty()) continue;
        if (!SafeRelative(rel)) { error = "unsafe path in the archive: " + en.name; return false; }
        const fs::path target = dest / fs::u8path(rel);
        if (rel.back() == '/') { fs::create_directories(target, ec); continue; }
        if (en.flags & 1) { error = "encrypted zip entries are not supported"; return false; }
        uint8_t lh[30];
        if (!read(en.offset, lh, sizeof lh) || u32(lh) != 0x04034b50u) { error = "damaged zip entry: " + en.name; return false; }
        const int64_t data = static_cast<int64_t>(en.offset) + 30 + u16(lh + 26) + u16(lh + 28);
        comp.resize(en.csize);
        if (en.csize && !read(data, comp.data(), comp.size())) { error = "cannot read " + en.name; return false; }
        std::vector<uint8_t> raw;
        const uint8_t* bytes = comp.data();
        size_t length = comp.size();
        if (en.method == 8) {
            try { raw = inflatelib::InflateRaw(comp.data(), comp.size(), en.usize); } catch (const std::exception& ex) { error = "cannot unpack " + en.name + ": " + ex.what(); return false; }
            bytes = raw.data(); length = raw.size();
        } else if (en.method != 0) { error = "unsupported compression in " + en.name; return false; }
        if (length != en.usize) { error = "wrong size of " + en.name; return false; }
        if (Crc32(bytes, length) != en.crc) { error = "checksum mismatch in " + en.name + " (a damaged download)"; return false; }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write " + target.string(); return false; }
        out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(length));
        out.close();
        if (!out) { error = "cannot write " + target.string() + " (disk full?)"; return false; }
        ++files;
    }
    if (progress) progress(entries.size(), entries.size());
    return true;
}

// ---- replacing the mod's folder ---------------------------------------------------------------------------------------------------------------
// Swaps `modDir` for the unpacked `fresh` folder: the old one is renamed to `old` first, the folders in `keep` (mp, saves) are moved into the new one, the
// the configs (um.cfg, um-engine.cfg) are copied to `backup` and kept when the new build brings none, and on any failure the old folder is put back. `old` is removed at the end.
inline bool SwapModFolder(const fs::path& modDir, const fs::path& fresh, const fs::path& old, const fs::path& backup, const std::vector<std::string>& keep, std::string& error) {
    std::error_code ec;
    fs::create_directories(old.parent_path(), ec);
    fs::remove_all(old, ec);
    const bool hadOld = fs::exists(modDir, ec); // not there yet: a fresh install
    if (hadOld) {
        fs::rename(modDir, old, ec);
        if (ec) { error = "cannot replace " + modDir.string() + ": " + ec.message() + " (is the game running?)"; return false; }
    }
    auto rollback = [&](const std::string& why) {
        std::error_code e2;
        fs::remove_all(modDir, e2);
        if (hadOld) fs::rename(old, modDir, e2);
        error = why;
        return false;
    };
    fs::rename(fresh, modDir, ec);
    if (ec) return rollback("cannot put the new files in place: " + ec.message());
    for (const std::string& k : keep) {
        if (!fs::exists(old / k, ec)) continue;
        fs::remove_all(modDir / k, ec);
        fs::rename(old / k, modDir / k, ec);
        if (ec) return rollback("cannot keep " + k + ": " + ec.message());
    }
    fs::create_directories(backup, ec);
    for (const char* cfg : {"um.cfg", "um-engine.cfg"}) { // the release has none (they are not in the repository): keep the user's; else the new ones win and the old are saved
        if (!fs::exists(old / cfg, ec)) continue;
        fs::copy_file(old / cfg, backup / cfg, fs::copy_options::overwrite_existing, ec);
        if (!fs::exists(modDir / cfg, ec)) fs::copy_file(old / cfg, modDir / cfg, ec);
    }
    fs::remove_all(old, ec);
    return true;
}

// Compares dotted versions ("0.8.6", "v.1.0.0") number by number: <0, 0, >0.
inline int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) { std::vector<long> v; long cur = -1; for (char c : s) { if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (c - '0'); else if (cur >= 0) { v.push_back(cur); cur = -1; } } if (cur >= 0) v.push_back(cur); return v; };
    const std::vector<long> x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) { const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0; if (p != q) return p < q ? -1 : 1; }
    return 0;
}

// ---- the updater the UI talks to -----------------------------------------------------------------------------------------------------------
class Updater {
public:
    enum class State { Idle, Checking, Available, UpToDate, Downloading, Extracting, Applying, Done, Failed };
    struct Snapshot { State state = State::Idle; Release latest; std::string message; double progress = 0; uint64_t done = 0, total = 0; };

    ~Updater() { Cancel(); Join(); }

    Snapshot Get() const { std::lock_guard<std::mutex> l(mutex_); return snap_; }
    bool Installing() const { const State s = Get().state; return s == State::Downloading || s == State::Extracting || s == State::Applying; } // a check does not count: Play stays usable
    bool Busy() const { const State s = Get().state; return s == State::Checking || s == State::Downloading || s == State::Extracting || s == State::Applying; }
    void Cancel() { cancel_ = true; }

    // `installedKey`: what the cfg recorded for this mod ("" = unknown): the update is offered when it differs from the newest.
    void Check(const std::string& repo, bool stableOnly, const std::string& installedKey) {
        if (Busy()) return;
        Join();
        cancel_ = false;
        Set(State::Checking, "Checking...", 0, 0, 0);
        thread_ = std::thread([=] {
            std::string api = std::getenv("EISTARTER_UM_API") ? std::getenv("EISTARTER_UM_API") : "https://api.github.com";
            std::string body, error;
            if (!HttpFetch(api + "/repos/" + repo + "/releases?per_page=30", &body, {}, nullptr, cancel_, error)) { Fail(error); return; }
            Release r;
            if (!PickRelease(body, stableOnly, r, error)) { Fail(error); return; }
            { std::lock_guard<std::mutex> l(mutex_); snap_.latest = r; }
            if (!installedKey.empty() && installedKey == r.Key()) Set(State::UpToDate, "Up to date", 0, 0, 0);
            else Set(State::Available, "", 0, 0, 0);
        });
    }

    // Downloads and installs `latest` over `modDir`. `workDir` (mods/.update) holds the download and the staging folders.
    void Install(const Release& latest, const fs::path& modDir, const fs::path& workDir, const std::vector<std::string>& keep) {
        if (Busy()) return;
        Join();
        cancel_ = false;
        Set(State::Downloading, "Downloading...", 0, 0, latest.assetSize);
        thread_ = std::thread([=] {
            std::error_code ec;
            fs::create_directories(workDir, ec);
            const std::string leaf = modDir.filename().string();
            const fs::path zip = workDir / fs::u8path(latest.assetName), fresh = workDir / fs::u8path(leaf + "-new"), old = workDir / fs::u8path(leaf + "-old");
            std::string error;
            fs::remove_all(fresh, ec);
            if (!HttpFetch(latest.assetUrl, nullptr, zip, [&](uint64_t d, uint64_t t) { Progress(d, t); }, cancel_, error)) { fs::remove(zip, ec); Fail(error); return; }
            Set(State::Extracting, "Unpacking...", 0, 0, 0);
            size_t files = 0;
            if (!ExtractZip(zip, fresh, [&](size_t i, size_t n) { Progress(i, n); }, cancel_, files, error)) { fs::remove_all(fresh, ec); fs::remove(zip, ec); Fail(error); return; }
            if (!fs::exists(fresh / "config.reg", ec)) { fs::remove_all(fresh, ec); fs::remove(zip, ec); Fail("the archive does not look like a mod (no config.reg in it)"); return; }
            Set(State::Applying, "Replacing files...", 0, 0, 0);
            if (!SwapModFolder(modDir, fresh, old, workDir / "backup", keep, error)) { fs::remove_all(fresh, ec); Fail(error); return; }
            fs::remove(zip, ec);
            Set(State::Done, "Updated to " + latest.tag, 1, 0, 0);
        });
    }

    void Join() { if (thread_.joinable()) thread_.join(); }

private:
    void Set(State s, const std::string& message, double progress, uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> l(mutex_);
        snap_.state = s; snap_.message = message; snap_.progress = progress; snap_.done = done; snap_.total = total;
    }
    void Progress(uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> l(mutex_);
        snap_.done = done; snap_.total = total ? total : snap_.total;
        snap_.progress = snap_.total ? static_cast<double>(done) / static_cast<double>(snap_.total) : 0;
    }
    void Fail(const std::string& message) { Set(State::Failed, message, 0, 0, 0); }

    mutable std::mutex mutex_;
    Snapshot snap_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
};

} // namespace launcher::update
