// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Map Editor: the difficulty rating (rating.hpp) - the Files tab's quest list (allod, score, range), the Rating tab for the open
// maps, and the Allod Range window. Shared state and the other files' functions: map_internal.hpp.
#include "map_internal.hpp"

#include <ctime>

#include "../db_model.hpp"

namespace mapedit {

namespace {

constexpr ImVec4 kGold(0.91f, 0.70f, 0.24f, 1.0f);

std::string Thousands(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", std::fabs(v));
    std::string s = b, out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return (v < 0 ? "-" : "") + out;
}

ImVec4 TierColor(double score) {
    if (score < 2500) return ImVec4(0.31f, 0.70f, 0.65f, 1);
    if (score < 4500) return ImVec4(0.55f, 0.75f, 0.42f, 1);
    if (score < 6500) return ImVec4(0.89f, 0.54f, 0.25f, 1);
    if (score < 8200) return ImVec4(0.86f, 0.37f, 0.27f, 1);
    return ImVec4(0.76f, 0.23f, 0.38f, 1);
}

std::string RatingsPath() { return config::UserDir() + "/um-multitool-ratings.tsv"; }

std::string Today() {
    char b[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
}

// A file's size and date: what makes a rating "changed" when it differs.
std::string FileSig(const std::string& path) {
    if (path.empty()) return "-";
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return "?";
    const auto time = std::filesystem::last_write_time(path, ec);
    return std::to_string(size) + ":" + (ec ? "0" : std::to_string(time.time_since_epoch().count()));
}

// The map.txt of a quest: the packed .mq itself, or <folder>/map.txt, or <folder>/<name>/map.txt.
std::string MapTxtPath(const quest::Quest& q) {
    std::error_code ec;
    const std::filesystem::path p(q.path);
    if (!std::filesystem::is_directory(p, ec)) return q.path;
    if (std::filesystem::exists(p / "map.txt", ec)) return (p / "map.txt").string();
    for (const auto& e : std::filesystem::directory_iterator(p, ec))
        if (e.is_directory() && std::filesystem::exists(e.path() / "map.txt", ec)) return (e.path() / "map.txt").string();
    return q.path;
}

// The map folders' files by name, read once per batch.
struct Resolver {
    std::map<std::string, std::string> byName;
    explicit Resolver(App& app) {
        for (const Library::MapFile& f : app.lib.ListMapFiles()) byName[Lower(f.name)] = f.path;
    }
    std::string Find(const std::string& name, const std::string& questPath) const {
        auto it = byName.find(Lower(name));
        if (it != byName.end()) return it->second;
        std::error_code ec;
        const std::filesystem::path p(questPath);
        const std::string dir = std::filesystem::is_directory(p, ec) ? p.parent_path().string() : checks::DirectoryOf(questPath);
        return checks::FindInDirectory(dir, name);
    }
};

struct QuestFiles { std::string base, own; };
QuestFiles FilesOf(const Resolver& r, const quest::Quest& q) {
    QuestFiles f;
    if (!q.baseMap.empty()) f.base = r.Find(q.baseMap + ".mob", q.path);
    f.own = r.Find(q.name + ".mob", q.path);
    return f;
}

std::string DbSig(const App& app) { return FileSig(app.lib.dbPath); }

std::string StampOf(const App& app, const quest::Quest& q, const QuestFiles& f) {
    return "v" + std::to_string(rating::kFormulaVersion) + (app.rating.store.storedStats ? "s" : "d") + "|db:" + DbSig(app) + "|q:" + FileSig(MapTxtPath(q)) + "|b:" + FileSig(f.base) + "|o:" + FileSig(f.own);
}

void EnsureStore(App& app) {
    RatingUi& r = app.rating;
    if (r.storeLoaded) return;
    r.storeLoaded = true;
    r.store.Load(RatingsPath());
}
void SaveStore(App& app) { app.rating.store.Save(RatingsPath()); }

void EnsureAllods(App& app) {
    RatingUi& r = app.rating;
    if (r.allodsTried) return;
    r.allodsTried = true;
    std::vector<std::string> hints{app.lib.dbPath};
    for (const auto& l : app.lib.maps.layers) hints.push_back(l.path);
    for (const std::string& q : app.lib.questFolders) hints.push_back(q);
    for (const std::string& q : app.lib.questPacks) hints.push_back(q);
    allods::FindAndLoad(hints, r.allods, &IniRegRegToText);
}

// The database's Monsters sheet is read on a thread (a spreadsheet takes a moment); again when the database file changes.
void StartDbLoad(App& app) {
    RatingUi& r = app.rating;
    if (r.dbLoading || app.lib.dbPath.empty()) return;
    if (r.dbThread.joinable()) r.dbThread.join();
    r.dbLoading = true;
    r.dbDone = false;
    r.dbError.clear();
    r.dbStamp = DbSig(app);
    const std::string path = app.lib.dbPath;
    r.dbThread = std::thread([&r, path]() {
        dbmodel::Book book;
        std::string err;
        rating::Params params;
        rating::Monsters m;
        if (!dbmodel::LoadBook(path, book, err) || !rating::LoadMonsters(book, params, m, err)) r.dbError = err.empty() ? "cannot read the database" : err;
        else r.dbMonsters = std::move(m);
        r.dbDone = true;
    });
}

bool MonstersReady(App& app) {
    RatingUi& r = app.rating;
    if (r.monstersReady && r.monstersStamp == DbSig(app)) return true;
    if (!r.dbLoading && (r.dbError.empty() || r.dbStamp != DbSig(app))) StartDbLoad(app);
    return false;
}

struct Item {
    std::string key, name, terrain;
    std::vector<std::string> mobs;
    size_t baseCount = 0;
    quest::Rect deploy, remove;
    std::string stamp, allod;
};

void RunWorker(RatingUi* ui, std::vector<Item> items, rating::Monsters monsters, bool storedStats) {
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mobs < b.mobs; }); // a zone's quests together: its base map is read once
    rating::Params params;
    params.storedStats = storedStats;
    std::unique_ptr<mob::File> base;
    std::string basePath;
    for (const Item& it : items) {
        if (ui->runCancel) break;
        { std::lock_guard<std::mutex> lock(ui->runMutex); ui->runCurrent = it.name; }
        std::vector<std::unique_ptr<mob::File>> own;
        rating::Input in;
        size_t at = 0;
        if (it.baseCount > 0) {
            if (!base || basePath != it.mobs[0]) {
                base = std::make_unique<mob::File>();
                mob::Load(it.mobs[0], *base);
                basePath = it.mobs[0];
            }
            in.files.push_back(base.get());
            in.baseCount = 1;
            at = 1;
        }
        for (; at < it.mobs.size(); ++at) {
            own.push_back(std::make_unique<mob::File>());
            if (mob::Load(it.mobs[at], *own.back())) in.files.push_back(own.back().get());
        }
        in.deploy = it.deploy;
        in.remove = it.remove;
        const rating::Result r = rating::Rate(in, monsters, params);
        if (r.ok) {
            RatingUi::Done d;
            d.key = it.key;
            d.entry.score = r.total;
            for (size_t i = 0; i < 5; ++i) d.entry.pts[i] = r.pts[i];
            d.entry.units = r.units; d.entry.fights = r.fights; d.entry.spawns = r.spawnSites; d.entry.objectives = r.objectives;
            d.entry.route = r.routeLength; d.entry.exposure = r.exposure;
            d.entry.allod = it.allod; d.entry.stamp = it.stamp; d.entry.rated = Today();
            std::lock_guard<std::mutex> lock(ui->runMutex);
            ui->runResults.push_back(std::move(d));
        }
        ++ui->runDone;
    }
    ui->runFinished = true;
}

void StartRun(App& app, const std::vector<const quest::QuestSet*>& sets) {
    RatingUi& r = app.rating;
    if (r.running || sets.empty()) return;
    if (!MonstersReady(app)) { // the database loads on a thread; the run starts when it is there
        r.pending.clear();
        for (const quest::QuestSet* s : sets) r.pending.push_back(ratingstore::Key(s->shown.name));
        r.message = app.lib.dbPath.empty() ? "No database: set it in Settings" : "Loading the database...";
        return;
    }
    const Resolver resolver(app);
    std::vector<Item> items;
    std::string skipped;
    for (const quest::QuestSet* set : sets) {
        const quest::Quest& q = set->shown;
        const QuestFiles f = FilesOf(resolver, q);
        if (f.own.empty() && f.base.empty()) { skipped += (skipped.empty() ? "" : ", ") + q.name; continue; }
        Item it;
        it.key = ratingstore::Key(q.name);
        it.name = q.name;
        it.terrain = q.terrain;
        if (!f.base.empty()) { it.mobs.push_back(f.base); it.baseCount = 1; }
        if (!f.own.empty()) it.mobs.push_back(f.own);
        if (!q.exits.empty()) { it.deploy = q.exits[0].deploy; it.remove = q.exits[0].remove; }
        it.stamp = StampOf(app, q, f);
        it.allod = r.allods.NameOf(q.terrain);
        items.push_back(std::move(it));
    }
    r.message = skipped.empty() ? std::string() : "No map files found for " + skipped;
    if (items.empty()) return;
    if (r.runThread.joinable()) r.runThread.join();
    r.running = true;
    r.runCancel = false;
    r.runFinished = false;
    r.runDone = 0;
    r.runTotal = static_cast<int>(items.size());
    r.runCurrent.clear();
    r.runThread = std::thread(RunWorker, &r, std::move(items), r.monsters, r.store.storedStats);
}

const quest::QuestSet* FindSet(App& app, const std::string& key) {
    for (const quest::QuestSet& s : app.quests) if (ratingstore::Key(s.shown.name) == key) return &s;
    return nullptr;
}

void RefreshStatus(App& app) {
    RatingUi& r = app.rating;
    const Resolver resolver(app);
    r.status.clear();
    r.stamps.clear();
    for (const quest::QuestSet& s : app.quests) {
        const quest::Quest& q = s.shown;
        const std::string key = ratingstore::Key(q.name);
        const std::string stamp = StampOf(app, q, FilesOf(resolver, q));
        r.stamps[key] = stamp;
        auto it = r.store.entries.find(key);
        r.status[key] = it == r.store.entries.end() ? 0 : it->second.stamp == stamp ? 1 : 2;
    }
    r.rangesDirty = true;
}

void RefreshRanges(App& app) {
    RatingUi& r = app.rating;
    r.rangesDirty = false;
    std::vector<std::vector<double>> scores(r.allods.allods.size());
    for (const quest::QuestSet& s : app.quests) {
        const std::string key = ratingstore::Key(s.shown.name);
        auto st = r.status.find(key);
        if (st == r.status.end() || st->second != 1) continue;
        const int a = r.allods.Find(s.shown.terrain);
        if (a >= 0) scores[static_cast<size_t>(a)].push_back(r.store.entries[key].score);
    }
    std::vector<allodrange::Lock> locks(r.allods.allods.size());
    for (size_t a = 0; a < locks.size(); ++a) {
        auto it = r.store.locks.find(ratingstore::Key(r.allods.allods[a].name));
        if (it != r.store.locks.end()) locks[a] = {true, it->second.lo, it->second.hi};
    }
    r.ranges = allodrange::Compute(scores, locks);
}

const allodrange::Range* RangeOfZone(App& app, const std::string& zone) {
    const int a = app.rating.allods.Find(zone);
    if (a < 0 || static_cast<size_t>(a) >= app.rating.ranges.size()) return nullptr;
    return &app.rating.ranges[static_cast<size_t>(a)];
}

// The allod's range as a bar: the tiers faintly, the range boxed, the score as a white line.
void RangeBar(double score, const allodrange::Range* range, float width = -1) {
    if (width < 0) width = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    static const double cuts[] = {0, 2500, 4500, 6500, 8200, 10000};
    for (int i = 0; i < 5; ++i) {
        ImVec4 c = TierColor((cuts[i] + cuts[i + 1]) / 2);
        c.w = 0.45f;
        d->AddRectFilled(ImVec2(p.x + width * static_cast<float>(cuts[i] / 10000), p.y + h * 0.35f), ImVec2(p.x + width * static_cast<float>(cuts[i + 1] / 10000), p.y + h * 0.65f), ImGui::GetColorU32(c));
    }
    if (range) {
        d->AddRectFilled(ImVec2(p.x + width * static_cast<float>(range->lo / 10000), p.y + 1), ImVec2(p.x + width * static_cast<float>(range->hi / 10000), p.y + h - 1), IM_COL32(232, 178, 60, 40));
        d->AddRect(ImVec2(p.x + width * static_cast<float>(range->lo / 10000), p.y + 1), ImVec2(p.x + width * static_cast<float>(range->hi / 10000), p.y + h - 1), ImGui::GetColorU32(kGold), 0, 0, 2.0f);
    }
    const float x = p.x + width * static_cast<float>(std::max(0.0, std::min(10000.0, score)) / 10000);
    d->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), IM_COL32(255, 255, 255, 255), 2.5f);
    ImGui::Dummy(ImVec2(width, h));
}

void VerdictText(double score, const allodrange::Range* range) {
    if (!range) { ImGui::TextDisabled("no range"); return; }
    double d = 0;
    switch (allodrange::Judge(score, *range, &d)) {
    case allodrange::Verdict::TooHard: ImGui::TextColored(ImVec4(1.0f, 0.54f, 0.48f, 1), "Too Hard +%s", Thousands(d).c_str()); break;
    case allodrange::Verdict::TooEasy: ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "Too Easy -%s", Thousands(d).c_str()); break;
    default: ImGui::TextColored(ImVec4(0.56f, 0.88f, 0.61f, 1), "In Range"); break;
    }
}

std::string RangeText(const allodrange::Range& r) {
    return Thousands(r.lo) + "-" + Thousands(r.hi) + (r.locked ? "  locked" : r.estimated ? "  estimated" : "  auto");
}

} // namespace

// ---- every frame ------------------------------------------------------------------------------------------------------

void PollRating(App& app) {
    RatingUi& r = app.rating;
    EnsureStore(app);
    EnsureAllods(app);
    if (r.dbLoading && r.dbDone) {
        if (r.dbThread.joinable()) r.dbThread.join();
        r.dbLoading = false;
        if (r.dbError.empty()) { r.monsters = std::move(r.dbMonsters); r.monstersReady = true; r.monstersStamp = r.dbStamp; r.monstersError.clear(); }
        else r.monstersError = r.dbError;
        r.statusAt = -100;
        if (r.monstersReady && !r.pending.empty()) {
            std::vector<const quest::QuestSet*> sets;
            for (const std::string& key : r.pending) if (const quest::QuestSet* s = FindSet(app, key)) sets.push_back(s);
            r.pending.clear();
            r.message.clear();
            StartRun(app, sets);
        } else if (!r.monstersReady) r.pending.clear();
    }
    if (r.running) {
        std::vector<RatingUi::Done> got;
        { std::lock_guard<std::mutex> lock(r.runMutex); got.swap(r.runResults); }
        for (RatingUi::Done& d : got) r.store.entries[d.key] = d.entry;
        if (!got.empty()) { r.statusAt = -100; }
        if (r.runFinished) {
            if (r.runThread.joinable()) r.runThread.join();
            r.running = false;
            SaveStore(app);
            r.statusAt = -100;
            r.message = (r.runCancel ? "Rating cancelled, finished scores kept" : "Rated " + std::to_string(r.runDone) + " quest" + (r.runDone == 1 ? "" : "s")) +
                        (r.message.empty() || r.message.rfind("Rat", 0) == 0 ? std::string() : "; " + r.message);
        }
    }
    const double now = ImGui::GetTime();
    if (now - r.statusAt > 2.0 && !app.quests.empty()) { r.statusAt = now; RefreshStatus(app); }
    if (r.rangesDirty) RefreshRanges(app);
}

// ---- the Files tab -----------------------------------------------------------------------------------------------------

void RatingQuestFilters(App& app) {
    RatingUi& r = app.rating;
    static const char* const shows[] = {"All", "Not rated", "Changed", "Out of range"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("##ratingshow", shows[r.filter])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(shows[i], r.filter == i)) r.filter = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Allod");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    std::string current = r.allodFilter == -1 ? "All" : r.allodFilter == -2 ? "No allod" : r.allodFilter >= 0 && static_cast<size_t>(r.allodFilter) < r.allods.allods.size() ? r.allods.allods[static_cast<size_t>(r.allodFilter)].name : "All";
    if (ImGui::BeginCombo("##ratingallod", current.c_str())) {
        if (ImGui::Selectable("All", r.allodFilter == -1)) r.allodFilter = -1;
        for (size_t a = 0; a < r.allods.allods.size(); ++a) if (ImGui::Selectable(r.allods.allods[a].name.c_str(), r.allodFilter == static_cast<int>(a))) r.allodFilter = static_cast<int>(a);
        if (ImGui::Selectable("No allod", r.allodFilter == -2)) r.allodFilter = -2;
        ImGui::EndCombo();
    }
    int changed = 0;
    std::string names;
    for (const quest::QuestSet& s : app.quests) {
        auto it = r.status.find(ratingstore::Key(s.shown.name));
        if (it != r.status.end() && it->second == 2) { ++changed; if (changed <= 4) names += (names.empty() ? "" : ", ") + s.shown.name; }
    }
    if (r.running) {
        ImGui::ProgressBar(r.runTotal ? static_cast<float>(r.runDone) / r.runTotal : 0.0f, ImVec2(-70, 0), ("Rating " + std::to_string(r.runDone) + " of " + std::to_string(r.runTotal)).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel##rating")) r.runCancel = true;
    } else {
        if (changed) {
            ImGui::TextColored(ImVec4(0.94f, 0.69f, 0.31f, 1), "%d quest%s changed since rated, scores reset: %s%s", changed, changed == 1 ? "" : "s", names.c_str(), changed > 4 ? ", ..." : "");
            if (ImGui::SmallButton("Rescan changed")) {
                std::vector<const quest::QuestSet*> sets;
                for (const quest::QuestSet& s : app.quests) { auto it = r.status.find(ratingstore::Key(s.shown.name)); if (it != r.status.end() && it->second == 2) sets.push_back(&s); }
                StartRun(app, sets);
            }
        }
        const bool ready = r.monstersReady && r.monstersStamp == DbSig(app);
        ImGui::BeginDisabled(r.dbLoading);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Rate");
        ImGui::SameLine();
        if (ImGui::Button("Open quest")) { if (app.openQuest) { std::vector<const quest::QuestSet*> sets; if (const quest::QuestSet* s = FindSet(app, ratingstore::Key(app.openQuest->shown.name))) sets.push_back(s); StartRun(app, sets); } else r.message = "Open a quest first"; }
        ImGui::SameLine();
        if (ImGui::Button("Not rated")) {
            std::vector<const quest::QuestSet*> sets;
            for (const quest::QuestSet& s : app.quests) { auto it = r.status.find(ratingstore::Key(s.shown.name)); if (it == r.status.end() || it->second != 1) sets.push_back(&s); }
            StartRun(app, sets);
        }
        ImGui::SameLine();
        if (ImGui::Button("All")) { std::vector<const quest::QuestSet*> sets; for (const quest::QuestSet& s : app.quests) sets.push_back(&s); StartRun(app, sets); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Rates every quest of the list on one thread (cancel any time). A rating is kept with the files it read:\nwhen one changes, the rating is reset and a rescan is suggested.");
        if (r.dbLoading) { ImGui::SameLine(); ImGui::TextDisabled("loading the database..."); }
        else if (!ready && !r.monstersError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1), "%s", r.monstersError.c_str());
    }
    if (!r.message.empty()) ImGui::TextDisabled("%s", r.message.c_str());
}

bool RatingShows(App& app, const quest::QuestSet& set) {
    RatingUi& r = app.rating;
    const std::string key = ratingstore::Key(set.shown.name);
    if (r.allodFilter != -1) {
        const int a = r.allods.Find(set.shown.terrain);
        if (r.allodFilter == -2 ? a >= 0 : a != r.allodFilter) return false;
    }
    if (r.filter == 0) return true;
    auto st = r.status.find(key);
    const int state = st == r.status.end() ? 0 : st->second;
    if (r.filter == 1) return state == 0;
    if (r.filter == 2) return state == 2;
    if (state != 1) return false;
    const allodrange::Range* range = RangeOfZone(app, set.shown.terrain);
    return range && allodrange::Judge(r.store.entries[key].score, *range) != allodrange::Verdict::InRange;
}

// Three cells: Allod, Score, Range.
void RatingColumns(App& app, const quest::QuestSet& set) {
    RatingUi& r = app.rating;
    const std::string key = ratingstore::Key(set.shown.name);
    const std::string allod = r.allods.NameOf(set.shown.terrain);
    ImGui::TableNextColumn();
    if (allod.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(allod.c_str());
    if (allod.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s is not listed in config.ini [MPZone]", set.shown.terrain.c_str());
    auto st = r.status.find(key);
    const int state = st == r.status.end() ? 0 : st->second;
    ImGui::TableNextColumn();
    if (state == 1) {
        const double score = r.store.entries[key].score;
        ImGui::TextColored(TierColor(score), "%s", Thousands(score).c_str());
    } else if (state == 2) {
        ImGui::TextColored(ImVec4(0.94f, 0.69f, 0.31f, 1), "changed");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A file this rating read has changed since it was calculated (was %s). Rate it again.", Thousands(r.store.entries[key].score).c_str());
    } else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    if (state == 1) {
        if (const allodrange::Range* range = RangeOfZone(app, set.shown.terrain)) {
            double d = 0;
            const double score = r.store.entries[key].score;
            switch (allodrange::Judge(score, *range, &d)) {
            case allodrange::Verdict::TooHard: ImGui::TextColored(ImVec4(1.0f, 0.54f, 0.48f, 1), "+%s", Thousands(d).c_str()); break;
            case allodrange::Verdict::TooEasy: ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "-%s", Thousands(d).c_str()); break;
            default: ImGui::TextColored(ImVec4(0.56f, 0.88f, 0.61f, 1), "ok"); break;
            }
        }
    }
}

void RatingAllodList(App& app) {
    RatingUi& r = app.rating;
    if (r.allods.Empty()) {
        ImGui::TextDisabled("No [MPZone] section found in a config.ini / config.reg beside the database or the map folders: no allods.");
        return;
    }
    ImGui::SeparatorText("Allods");
    ImGui::TextDisabled("From %s. Click one to edit its range.", std::filesystem::path(r.allods.source).filename().string().c_str());
    if (ImGui::BeginTable("##allods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Allod", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Rated", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Zones", ImGuiTableColumnFlags_WidthFixed, 45);
        for (size_t a = 0; a < r.allods.allods.size() && a < r.ranges.size(); ++a) {
            const allodrange::Range& range = r.ranges[a];
            int total = 0;
            for (const quest::QuestSet& s : app.quests) total += r.allods.Find(s.shown.terrain) == static_cast<int>(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(a));
            if (ImGui::Selectable(r.allods.allods[a].name.c_str(), r.selectedAllod == static_cast<int>(a) && r.allodWindow, ImGuiSelectableFlags_SpanAllColumns)) { r.selectedAllod = static_cast<int>(a); r.allodWindow = true; }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(RangeText(range).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d/%d", range.rated, total);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", r.allods.allods[a].zones.size());
        }
        ImGui::EndTable();
    }
    if (!r.allods.commented.empty()) {
        std::string z;
        for (const std::string& c : r.allods.commented) z += (z.empty() ? "" : ", ") + c;
        ImGui::TextDisabled("Not assigned (commented out): %s", z.c_str());
    }
}

void RatingOpenQuestLine(App& app) {
    RatingUi& r = app.rating;
    if (!app.openQuest) return;
    const std::string key = ratingstore::Key(app.openQuest->shown.name);
    auto st = r.status.find(key);
    if (st == r.status.end() || st->second != 1) { ImGui::TextDisabled("Not rated: Rating tab, or Rate > Open quest in the list"); return; }
    const double score = r.store.entries[key].score;
    ImGui::TextColored(TierColor(score), "Difficulty %s %s", Thousands(score).c_str(), rating::TierName(score));
    ImGui::SameLine();
    VerdictText(score, RangeOfZone(app, app.openQuest->shown.terrain));
}

// ---- the Allod Range window ------------------------------------------------------------------------------------------

void AllodRangeWindow(App& app) {
    RatingUi& r = app.rating;
    if (!r.allodWindow) return;
    if (r.selectedAllod < 0 || static_cast<size_t>(r.selectedAllod) >= r.allods.allods.size() || static_cast<size_t>(r.selectedAllod) >= r.ranges.size()) { r.allodWindow = false; return; }
    const size_t a = static_cast<size_t>(r.selectedAllod);
    const allods::Allod& allod = r.allods.allods[a];
    allodrange::Range& range = r.ranges[a];
    ImGui::SetNextWindowSize(ImVec2(360, 330), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(("Allod Range - " + allod.name + "###allodrange").c_str(), &r.allodWindow)) { ImGui::End(); return; }
    ImGui::TextDisabled("%zu zones, config.ini [MPZone]", allod.zones.size());
    const std::string key = ratingstore::Key(allod.name);
    bool locked = range.locked;
    if (ImGui::RadioButton("Auto", !locked)) { if (locked) { r.store.locks.erase(key); SaveStore(app); r.rangesDirty = true; } }
    ImGui::SameLine();
    if (ImGui::RadioButton("Locked", locked)) { if (!locked) { r.store.locks[key] = {range.lo, range.hi}; SaveStore(app); r.rangesDirty = true; } }
    ImGui::SameLine();
    ImGui::TextDisabled("(a locked range is kept by rescans)");
    ImGui::BeginDisabled(!locked);
    int lo = static_cast<int>(range.lo), hi = static_cast<int>(range.hi);
    ImGui::SetNextItemWidth(110);
    bool edited = ImGui::InputInt("Minimum", &lo, 50, 500);
    ImGui::SetNextItemWidth(110);
    edited = ImGui::InputInt("Maximum", &hi, 50, 500) || edited;
    ImGui::EndDisabled();
    if (locked && edited) {
        lo = std::max(0, std::min(lo, 9900));
        hi = std::max(lo + 100, std::min(hi, 10000));
        r.store.locks[key] = {static_cast<double>(lo), static_cast<double>(hi)};
        SaveStore(app);
        r.rangesDirty = true;
    }
    if (range.hasMedian) ImGui::TextDisabled("Median %s of %d rated quest%s.", Thousands(range.median).c_str(), range.rated, range.rated == 1 ? "" : "s");
    else ImGui::TextDisabled("No rated quests yet.");
    if (range.estimated) ImGui::TextDisabled("Fewer than 4 rated quests: estimated from the allods next to it.");
    RangeBar(range.hasMedian ? range.median : (range.lo + range.hi) / 2, &range);
    ImGui::SeparatorText("Quests");
    for (const quest::QuestSet& s : app.quests) {
        if (r.allods.Find(s.shown.terrain) != static_cast<int>(a)) continue;
        auto st = r.status.find(ratingstore::Key(s.shown.name));
        if (st == r.status.end() || st->second != 1) continue;
        const double score = r.store.entries[ratingstore::Key(s.shown.name)].score;
        ImGui::TextColored(TierColor(score), "%7s", Thousands(score).c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(s.shown.name.c_str());
        ImGui::SameLine();
        VerdictText(score, &range);
    }
    ImGui::SeparatorText("Zones");
    std::string z;
    for (const std::string& w : allod.zones) z += (z.empty() ? "" : ", ") + w;
    ImGui::TextWrapped("%s", z.c_str());
    ImGui::End();
}

// ---- the Rating tab: the maps open in the editor ---------------------------------------------------------------------

namespace {

// The rating of the loaded maps as the editor holds them (edits included), recalculated when they change.
void UpdateLive(App& app) {
    RatingUi& r = app.rating;
    std::string key = std::to_string(app.undoSteps.size()) + ":" + std::to_string(app.redoSteps.size());
    for (const auto& m : app.mobs) key += "|" + m->file.path + ":" + std::to_string(m->file.bytes.size());
    if (app.openQuest) key += "|q:" + app.openQuest->shown.name;
    if (r.liveValid && key == r.liveKey) return;
    r.liveKey = key;
    rating::Input in;
    std::string ownName;
    if (app.openQuest) {
        ownName = Lower(app.openQuest->shown.name + ".mob");
        const auto& q = app.openQuest->shown;
        if (!q.exits.empty()) { in.deploy = q.exits[0].deploy; in.remove = q.exits[0].remove; }
    }
    in.baseCount = 0;
    for (const auto& m : app.mobs) {
        if (!m->file.loaded) continue;
        if (app.openQuest && Lower(m->file.fileName) == ownName) in.baseCount = in.files.size();
        in.files.push_back(&m->file);
    }
    if (app.openQuest && in.baseCount == 0 && !in.files.empty() && ownName != Lower(in.files[0]->fileName)) in.baseCount = in.files.size(); // no own map: all base
    rating::Params params;
    params.storedStats = r.store.storedStats;
    r.live = rating::Rate(in, r.monsters, params);
    r.liveValid = true;
}

} // namespace

void RatingTab(App& app) {
    RatingUi& r = app.rating;
    EnsureStore(app);
    EnsureAllods(app);
    ImGui::SeparatorText("Difficulty");
    if (!MonstersReady(app)) {
        if (r.dbLoading) ImGui::TextDisabled("Loading the database...");
        else if (!r.monstersError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1), "%s", r.monstersError.c_str());
        else ImGui::TextDisabled("No database: set it in Settings.");
        return;
    }
    if (app.mobs.empty()) { ImGui::TextDisabled("Open a quest or a map: its units and script are rated here."); return; }
    UpdateLive(app);
    const rating::Result& res = r.live;
    if (!res.ok) { ImGui::TextDisabled("%s", res.note.c_str()); return; }
    const std::string terrain = app.openQuest ? app.openQuest->shown.terrain : std::filesystem::path(app.terrainPath).stem().string();
    const allodrange::Range* range = RangeOfZone(app, terrain);
    const std::string allod = r.allods.NameOf(terrain);
    ImGui::TextDisabled("%s - %s", app.openQuest ? app.openQuest->shown.name.c_str() : "loaded maps", allod.empty() ? "no allod" : allod.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, TierColor(res.total));
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted(Thousands(res.total).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(rating::TierName(res.total));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("level %d-%d", rating::PartyLevelLow(res.total), rating::PartyLevelLow(res.total) + 2);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, TierColor(res.total));
    ImGui::ProgressBar(static_cast<float>(res.total / 10000.0), ImVec2(-1, 0), (Thousands(res.total) + " / 10,000").c_str());
    ImGui::PopStyleColor();
    if (range) {
        ImGui::TextDisabled("%s range", allod.c_str());
        RangeBar(res.total, range);
        ImGui::TextDisabled("%s", RangeText(*range).c_str());
        ImGui::SameLine();
        VerdictText(res.total, range);
    } else ImGui::TextDisabled("No range: the zone is not in config.ini [MPZone].");
    ImGui::Spacing();
    for (int f = 0; f < rating::kFactors; ++f) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%-12s", rating::FactorName(f));
        ImGui::SameLine(110);
        ImGui::ProgressBar(static_cast<float>(res.pts[static_cast<size_t>(f)] / rating::kMaxPoints[static_cast<size_t>(f)]), ImVec2(-1, 0),
                           (Thousands(res.pts[static_cast<size_t>(f)]) + " / " + Thousands(rating::kMaxPoints[static_cast<size_t>(f)])).c_str());
    }
    {
        std::string maps;
        for (const auto& f : res.files) maps += (maps.empty() ? "" : ", ") + f.first + " (" + std::to_string(f.second) + ")";
        ImGui::TextDisabled("Maps read, enemies met in each: %s", maps.c_str());
    }
    ImGui::TextDisabled("%d enemies in %zu aggro groups, %d real fights, walk %.0f units (%d%% seen), %d scripted spawn site%s, %d objective%s.", res.units, res.pulls.size(), res.fights,
                        res.routeLength, static_cast<int>(res.exposure * 100 + 0.5f), res.spawnSites, res.spawnSites == 1 ? "" : "s", res.objectives, res.objectives == 1 ? "" : "s");
    {
        bool stored = r.store.storedStats;
        if (ImGui::Checkbox("Use the stats stored in the map", &stored)) { r.store.storedStats = stored; SaveStore(app); r.liveValid = false; r.statusAt = -100; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off (default): every unit is rated with its prototype's row of the database, which is what you balance.\nOn: a unit with \"Stats imported\" off carries its own stats in the map (Objects tab > Stats), which may be older than the database.\nChanging this resets the stored ratings (they are calculated again).");
    }
    if (res.storedStats) ImGui::TextDisabled("%d unit%s counted with the stats stored in the map (Stats imported is off), not the database row.", res.storedStats, res.storedStats == 1 ? "" : "s");
    if (res.unknown) ImGui::TextColored(ImVec4(0.94f, 0.69f, 0.31f, 1), "%d unit%s without a Monsters row in the database (not counted).", res.unknown, res.unknown == 1 ? "" : "s");
    if (res.objectives == 0 && app.openQuest) ImGui::TextColored(ImVec4(0.94f, 0.69f, 0.31f, 1), "No objectives found in the script (QObj* calls): the walk is only deploy to exit.");
    ImGui::TextDisabled("0 is an empty map; 10,000 is %.0f of this database's strongest unit (%s) in one aggro group. Only enemies and the script count, never potions or gear.",
                        r.monsters.anchorUnit > 0 ? 8.0f : 0.0f, r.monsters.anchorName.c_str());
    if (ImGui::CollapsingHeader("Aggro groups")) {
        if (ImGui::BeginTable("##pulls", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 160))) {
            ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Units", ImGuiTableColumnFlags_WidthStretch);
            std::vector<const rating::Pull*> order;
            for (const rating::Pull& p : res.pulls) order.push_back(&p);
            std::sort(order.begin(), order.end(), [](const rating::Pull* a, const rating::Pull* b) { return a->power > b->power; });
            for (const rating::Pull* p : order) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((std::to_string(static_cast<int>(p->x)) + ", " + std::to_string(static_cast<int>(p->y))).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    OrbitCamera& cam = app.scene.camera;
                    cam.targetX = p->x; cam.targetY = p->y; cam.targetZ = app.scene.Ground(p->x, p->y);
                    cam.distance = std::max(25.0f, p->radius * 3.0f);
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.0f%%", p->norm * 100);
                ImGui::TableNextColumn();
                std::string names;
                for (const std::string& n : p->names) names += (names.empty() ? "" : ", ") + n;
                ImGui::TextUnformatted(names.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Click a group to go there. Power is the share of the anchor group's.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Rate and save")) {
        if (app.openQuest) { std::vector<const quest::QuestSet*> sets; if (const quest::QuestSet* s = FindSet(app, ratingstore::Key(app.openQuest->shown.name))) sets.push_back(s); StartRun(app, sets); }
        else r.message = "Open a quest to store its rating";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stores the rating of the quest's files on disk (the list's Score column). Unsaved edits are not in it.");
    if (!r.message.empty()) ImGui::TextDisabled("%s", r.message.c_str());
}

} // namespace mapedit
