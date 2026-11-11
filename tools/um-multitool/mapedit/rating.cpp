// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Difficulty rating: see rating.hpp.
#include "rating.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace rating {

namespace {

std::string Low(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
double ClampD(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// ---- the mission script: the few calls the rating reads --------------------------------------------------------

struct Call {
    size_t at = 0;                  // where it starts in the script text
    std::string name;               // lower-case
    std::vector<std::string> args;  // quotes removed, trimmed
};

std::string Unquote(std::string s) {
    s = Trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return Trim(s);
}

// Every call of one of `names` (lower-case) in the script text, in order. Comments are skipped.
std::vector<Call> Calls(const std::string& s, const std::set<std::string>& names) {
    std::vector<Call> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (c == '"') { ++i; while (i < s.size() && s[i] != '"') ++i; ++i; continue; }
        if (!(std::isalpha(c) || c == '_')) { ++i; continue; }
        const size_t start = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
        const std::string ident = Low(s.substr(start, i - start));
        size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (!names.count(ident) || j >= s.size() || s[j] != '(') continue;
        Call call;
        call.name = ident;
        call.at = start;
        int depth = 0;
        bool quote = false;
        std::string cur;
        size_t k = j;
        for (; k < s.size(); ++k) {
            const char ch = s[k];
            if (ch == '"') quote = !quote;
            if (!quote) {
                if (ch == '(') { if (depth++ == 0) continue; }
                else if (ch == ')') { if (--depth == 0) break; }
                else if (ch == ',' && depth == 1) { call.args.push_back(Unquote(cur)); cur.clear(); continue; }
            }
            cur += ch;
        }
        if (!Trim(cur).empty() || !call.args.empty()) call.args.push_back(Unquote(cur));
        out.push_back(std::move(call));
        i = k + 1;
    }
    return out;
}

double Num(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

struct Pt { float x = 0, y = 0; };

// A unit whose "Stats imported" is off carries its own stats (UNIT_STATS, 172 bytes; the Map Editor's stats editor has the layout):
// the game uses those, not its prototype's database row. Offsets: HP 0 / max HP 4 (int), Actions 20, Range 60, Attack 64, Defence 68,
// damage min 76, damage range 80, Absorption 112 (int), Sight 116.
bool MonsterFromStats(const std::vector<uint8_t>& s, const std::string& name, Monster& m) {
    if (s.size() < 172) return false;
    auto f = [&](size_t at) { float v; std::memcpy(&v, s.data() + at, 4); return v; };
    auto i = [&](size_t at) { int32_t v; std::memcpy(&v, s.data() + at, 4); return static_cast<float>(v); };
    m.name = name;
    m.health = std::max(i(0), i(4));
    m.actions = f(20); m.range = f(60); m.toHit = f(64); m.parry = f(68);
    m.dmgMin = f(76); m.dmgSpread = f(80); m.absorb = i(112); m.sight = f(116);
    return m.health > 0;
}

// ---- enemy units -----------------------------------------------------------------------------------------------

struct Enemy {
    Pt pos;
    size_t file = 0;     // index of its map in Input::files
    long id = -1;
    bool relevant = false; // a quest meets it: its own map's, named by the script, or near the walk
    UnitPower power;
    bool known = false;
    bool stored = false; // counted with the map's stored stats
    float help = 7;
    // where the unit notices a walker: discs of `radius` around these points (one point: a post; several: a patrol path)
    std::vector<Pt> shape;
    bool closed = false;
    float radius = 0;
    std::string name;
};

float DistToSegment(Pt p, Pt a, Pt b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = Clamp(t, 0, 1);
    const float cx = a.x + t * dx - p.x, cy = a.y + t * dy - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

bool Notices(const Enemy& e, Pt p, float margin = 0) {
    const float r = e.radius + margin;
    if (e.shape.size() < 2) {
        const Pt c = e.shape.empty() ? e.pos : e.shape[0];
        const float dx = c.x - p.x, dy = c.y - p.y;
        return dx * dx + dy * dy <= r * r;
    }
    for (size_t i = 0; i + 1 < e.shape.size(); ++i)
        if (DistToSegment(p, e.shape[i], e.shape[i + 1]) <= r) return true;
    if (e.closed && DistToSegment(p, e.shape.back(), e.shape.front()) <= r) return true;
    return false;
}

// The id in "GetObject( 1000021 )" / "GetObjectByID( \"1000005\" )" (-1: none).
long ObjectId(const std::string& text) {
    const std::string l = Low(text);
    const size_t at = l.find("getobject");
    if (at == std::string::npos) return -1;
    size_t i = l.find('(', at);
    if (i == std::string::npos) return -1;
    while (++i < l.size() && !std::isdigit(static_cast<unsigned char>(l[i])) && l[i] != ')') {}
    if (i >= l.size() || !std::isdigit(static_cast<unsigned char>(l[i]))) return -1;
    return std::strtol(l.c_str() + i, nullptr, 10);
}

} // namespace

// ---- database --------------------------------------------------------------------------------------------------

const Monster* Monsters::Find(const std::string& prototype) const {
    auto it = byName.find(Low(prototype));
    return it == byName.end() ? nullptr : &it->second;
}

UnitPower PowerOf(const Monster& m, const Monsters& db, const Params& p) {
    UnitPower u;
    const float refParry = db.haveHero ? db.hero.parry : p.refParry;
    const float refActions = db.haveHero && db.hero.actions > 0 ? db.hero.actions : p.refActions;
    u.effHp = m.health * (1.0f + m.absorb / p.absorbScale);
    const float pHit = Clamp((m.toHit + 5.0f - refParry) / 10.0f, 0.05f, 1.0f); // the game's own HUD chance (combat.md 4.2)
    const float rate = m.actions > 0 && refActions > 0 ? Clamp(m.actions / refActions, 0.3f, 3.0f) : 1.0f;
    const float dmg = m.dmgMin + m.dmgSpread * 0.5f; // the roll is min + rand(0..spread)
    u.dps = dmg * pHit * rate;
    u.power = std::sqrt(std::max(0.0f, u.effHp) * std::max(0.0f, u.dps));
    u.sight = m.sight > 0 ? m.sight : p.defaultSight;
    u.ranged = m.range >= p.rangedFrom;
    return u;
}

bool LoadMonsters(const dbmodel::Book& book, const Params& p, Monsters& out, std::string& err) {
    const sheetio::Sheet* s = nullptr;
    for (const sheetio::Sheet& sh : book) if (sh.name == "Monsters") s = &sh;
    if (!s) { err = "the database has no Monsters sheet"; return false; }
    std::map<std::string, int> col;
    for (int c = 1; c <= s->maxCol; ++c) {
        auto it = s->cells.find({2, c});
        if (it != s->cells.end() && !col.count(it->second.text)) col[it->second.text] = c;
    }
    auto find = [&](std::initializer_list<const char*> titles) {
        for (const char* t : titles) { auto it = col.find(t); if (it != col.end()) return it->second; }
        return 0;
    };
    const int cName = find({"Name"}), cHp = find({"Stats Health"}), cAbs = find({"Stats Absorbtion", "Stats Absorption"}),
              cAct = find({"Tuning Actions"}), cMove = find({"Tuning Move"}), cRange = find({"Attack Range"}),
              cHit = find({"Stats To Hit"}), cParry = find({"Stats Parry"}), cMin = find({"Stats Danage Nub", "Stats Damage Min"}),
              cMax = find({"Stats Damage Max"}), cSight = find({"Senses Sight"});
    if (!cName || !cHp || !cMin || !cMax) { err = "the Monsters sheet lacks its Name / Health / Damage columns"; return false; }
    auto num = [&](int row, int c) {
        if (!c) return 0.0f;
        auto it = s->cells.find({row, c});
        return it == s->cells.end() ? 0.0f : static_cast<float>(std::strtod(it->second.text.c_str(), nullptr));
    };
    out = Monsters();
    for (int row = 4; row <= s->maxRow; ++row) {
        auto n = s->cells.find({row, cName});
        if (n == s->cells.end() || Trim(n->second.text).empty()) continue;
        Monster m;
        m.name = Trim(n->second.text);
        m.health = num(row, cHp); m.absorb = num(row, cAbs); m.actions = num(row, cAct); m.move = num(row, cMove);
        m.range = num(row, cRange); m.toHit = num(row, cHit); m.parry = num(row, cParry);
        m.dmgMin = num(row, cMin); m.dmgSpread = num(row, cMax); m.sight = num(row, cSight);
        if (m.health <= 0) continue;
        out.byName[Low(m.name)] = m;
    }
    if (out.byName.empty()) { err = "the Monsters sheet has no units"; return false; }
    if (const Monster* h = out.Find("Human Hero")) { out.hero = *h; out.haveHero = true; }
    // The anchor is the strongest unit, except for outliers: a joke or test unit many times stronger than the rest
    // (the vanilla database has one at 60x) would make every score tiny. A unit more than 5x the next one is skipped.
    std::vector<std::pair<float, std::string>> ranked;
    for (const auto& kv : out.byName) ranked.push_back({PowerOf(kv.second, out, p).power, kv.second.name});
    std::sort(ranked.rbegin(), ranked.rend());
    size_t top = 0;
    while (top + 1 < ranked.size() && ranked[top].first > 5.0f * ranked[top + 1].first) ++top;
    out.anchorUnit = ranked[top].first;
    out.anchorName = ranked[top].second;
    return true;
}

// ---- the rating ------------------------------------------------------------------------------------------------

const char* TierName(double score) {
    if (score < 2500) return "Easy";
    if (score < 4500) return "Standard";
    if (score < 6500) return "Hard";
    if (score < 8200) return "Very Hard";
    return "Brutal";
}
int PartyLevelLow(double score) { return static_cast<int>(std::lround(2 + score / 10000.0 * 14)) - 1; }

Result Rate(const Input& in, const Monsters& db, const Params& p) {
    Result r;
    if (db.Empty()) { r.note = "no Monsters database"; return r; }
    const float anchorPull = p.anchorPullUnits * db.anchorUnit;
    if (anchorPull <= 0) { r.note = "the database has no unit with damage"; return r; }

    // -- who is an enemy: the last map's diplomacy table (0 friend, 1 neutral, 2 enemy), else any group but the player's
    const mob::File* dip = nullptr;
    for (const mob::File* f : in.files) if (f && f->hasDiplomacy && f->diplomacy.size() >= 32 * 32) dip = f;
    auto hostile = [&](int player) {
        if (player <= 0 || player >= 32) return player > 0;
        if (!dip) return true;
        return dip->diplomacy[static_cast<size_t>(player) * 32] == 2 || dip->diplomacy[static_cast<size_t>(player)] == 2;
    };

    std::vector<Enemy> enemies;
    std::map<std::string, Pt> byName;
    std::map<long, Pt> byId;
    std::string script, ownScript; // every map's script; the quest's own maps' (all of them when no file is a base map)
    for (size_t fi = 0; fi < in.files.size(); ++fi) {
        const mob::File* f = in.files[fi];
        if (!f || !f->loaded) continue;
        if (f->hasScript) { script += f->script + "\n"; if (fi >= in.baseCount) ownScript += f->script + "\n"; }
        for (const mob::Object& o : f->objects) {
            if (o.hasId) byId[static_cast<long>(o.id)] = {o.position.x, o.position.y};
            if (!o.name.empty()) byName[Low(o.name)] = {o.position.x, o.position.y};
            if (o.kind != mob::Kind::Unit || !hostile(o.player)) continue;
            const mob::Logic* logic = nullptr;
            for (const mob::Logic& l : o.logics) if (l.use) { logic = &l; break; }
            if (logic && logic->model == 4) continue; // a briefing unit: an NPC
            Enemy e;
            e.pos = {o.position.x, o.position.y};
            e.file = fi;
            e.id = o.hasId ? static_cast<long>(o.id) : -1;
            e.name = o.prototype;
            Monster stored;
            if (p.storedStats && !o.needImport && MonsterFromStats(o.stats, o.prototype, stored)) { e.known = true; e.stored = true; e.power = PowerOf(stored, db, p); }
            else if (const Monster* m = db.Find(o.prototype)) { e.known = true; e.power = PowerOf(*m, db, p); }
            else e.power.sight = p.defaultSight;
            e.help = logic && logic->help > 0 ? logic->help : p.defaultHelp;
            e.radius = e.power.sight;
            e.shape = {e.pos};
            if (logic) {
                if ((logic->model == 1 || logic->model == 5) && logic->guardRadius > e.radius) e.radius = logic->guardRadius;
                if (logic->model == 2 && logic->patrol.size() > 1) {
                    e.shape.clear();
                    for (const mob::PatrolPoint& pp : logic->patrol) e.shape.push_back({pp.position.x, pp.position.y});
                    e.closed = logic->cyclic;
                }
            }
            enemies.push_back(std::move(e));
        }
    }

    // -- the script: areas (any map's), then what the quest's own script says: objectives, groups, spawns, the objects it binds
    const std::vector<Call> areaCalls = Calls(script, {"addroundtoarea", "addrecttoarea"});
    const std::vector<Call> calls = Calls(ownScript, {"qobjarea", "qobjkillunit", "qobjseeunit", "qobjseeobject", "qobjuse", "qobjkillgroup", "addobject", "addunittoserver"});
    const std::vector<Call> stepCalls = Calls(ownScript, {"gssetvarmax", "gssetvar"});
    std::map<std::string, long> vars; // "Borka = GetObjectByID( "1000263" )": the objects the quest's script works with (levers, NPCs, bosses)
    {
        size_t pos = 0;
        while (pos < ownScript.size()) {
            size_t end = ownScript.find('\n', pos);
            if (end == std::string::npos) end = ownScript.size();
            const std::string line = ownScript.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            const long id = eq == std::string::npos ? -1 : ObjectId(line.substr(eq + 1));
            if (id >= 0) vars[Low(Trim(line.substr(0, eq)))] = id;
        }
    }
    auto idOf = [&](const std::string& arg) {
        const long id = ObjectId(arg);
        if (id >= 0) return id;
        auto it = vars.find(Low(Trim(arg)));
        return it == vars.end() ? -1L : it->second;
    };
    auto refPos = [&](const std::string& arg, Pt& out, long& id) {
        id = idOf(arg);
        if (id >= 0) { auto it = byId.find(id); if (it != byId.end()) { out = it->second; return true; } }
        auto it = byName.find(Low(Trim(arg)));
        if (it != byName.end()) { out = it->second; return true; }
        return false;
    };
    std::map<long, Pt> areas;
    for (const Call& c : areaCalls) {
        if (c.name == "addroundtoarea" && c.args.size() >= 3) areas.emplace(static_cast<long>(Num(c.args[0])), Pt{static_cast<float>(Num(c.args[1])), static_cast<float>(Num(c.args[2]))});
        else if (c.name == "addrecttoarea" && c.args.size() >= 5) areas.emplace(static_cast<long>(Num(c.args[0])), Pt{static_cast<float>((Num(c.args[1]) + Num(c.args[3])) / 2), static_cast<float>((Num(c.args[2]) + Num(c.args[4])) / 2)});
    }
    std::map<std::string, std::vector<long>> groups; // group name -> the object ids AddObject put in it
    for (const Call& c : calls)
        if (c.name == "addobject" && c.args.size() >= 2) { const long id = idOf(c.args[1]); if (id >= 0) groups[Low(Trim(c.args[0]))].push_back(id); }

    // -- the walk: deploy -> the QObj* objectives in order -> the other objects the script works with (nearest first) -> the exit.
    // Older quests have no QObj* calls: their goals are levers, NPCs and groups the script binds, with the quest's steps in variables.
    std::set<long> referenced; // units the quest names
    std::vector<Pt> route, loose;
    std::set<long> looseIds;
    auto target = [&](long id) {
        if (id < 0 || !looseIds.insert(id).second) return;
        referenced.insert(id);
        auto it = byId.find(id);
        if (it != byId.end()) loose.push_back(it->second);
    };
    if (in.deploy.set) route.push_back({(in.deploy.x1 + in.deploy.x2) / 2, (in.deploy.y1 + in.deploy.y2) / 2});
    for (const Call& c : calls) {
        Pt pt;
        long id = -1;
        if (c.name == "qobjarea" && !c.args.empty()) {
            auto it = areas.find(static_cast<long>(Num(c.args[0])));
            if (it != areas.end()) { route.push_back(it->second); ++r.objectives; }
        } else if ((c.name == "qobjkillunit" || c.name == "qobjseeunit" || c.name == "qobjseeobject" || c.name == "qobjuse") && !c.args.empty() && refPos(c.args[0], pt, id)) {
            route.push_back(pt); ++r.objectives;
            if (id >= 0) referenced.insert(id);
        } else if (c.name == "qobjkillgroup" && !c.args.empty()) {
            auto it = groups.find(Low(Trim(c.args[0])));
            if (it == groups.end()) continue;
            for (const long gid : it->second) target(gid);
            ++r.objectives;
        }
    }
    // Quests written without QObj* calls (the older style, and most hand-made quests): a "Script" block waits for something and then
    // sets the quest step "q.<quest>.<quest>.N" (GSSetVarMax / GSSetVar). What the block waits for (the GetObject ids in its condition:
    // levers, units, bosses) is where the step happens; the steps give the order and the number of objectives.
    std::set<int> steps;
    {
        const std::string lo = Low(ownScript);
        std::vector<size_t> blocks; // where each Script / WorldScript starts
        for (size_t at = 0; at < lo.size(); ++at) {
            if (at != 0 && lo[at - 1] != '\n') continue;
            if (lo.compare(at, 7, "script ") == 0 || lo.compare(at, 7, "script\n") == 0 || lo.compare(at, 11, "worldscript") == 0) blocks.push_back(at);
        }
        auto blockOf = [&](size_t pos) { int b = -1; for (size_t i = 0; i < blocks.size() && blocks[i] <= pos; ++i) b = static_cast<int>(i); return b; };
        std::map<int, int> blockStep; // block -> the lowest quest step it sets
        for (const Call& c : stepCalls) {
            if (c.args.size() < 2 || c.args[1].compare(0, 2, "q.") != 0) continue;
            const size_t dot = c.args[1].find_last_of('.');
            if (dot == std::string::npos || dot + 1 >= c.args[1].size()) continue;
            const int n = std::atoi(c.args[1].c_str() + dot + 1);
            if (n <= 0) continue;
            steps.insert(n);
            const int b = blockOf(c.at);
            if (b >= 0 && (!blockStep.count(b) || n < blockStep[b])) blockStep[b] = n;
        }
        std::map<int, std::vector<long>> byStep; // step -> the objects its block's condition names (1000: a block that sets no step)
        for (size_t at = lo.find("getobject"); at != std::string::npos; at = lo.find("getobject", at + 1)) {
            const int b = blockOf(at);
            if (b < 0) continue; // the WorldScript binds objects to variables: handled below
            const size_t thenAt = lo.find("then", blocks[static_cast<size_t>(b)]);
            if (thenAt != std::string::npos && at > thenAt) continue; // after "then": what the block does, not what it waits for
            const long id = ObjectId(ownScript.substr(at, 48));
            if (id >= 0) byStep[blockStep.count(b) ? blockStep[b] : 1000].push_back(id);
        }
        for (const auto& st : byStep)
            for (const long id : st.second) {
                if (!looseIds.insert(id).second) continue;
                referenced.insert(id);
                auto it = byId.find(id);
                if (it == byId.end()) continue;
                bool near = false;
                for (const Pt& w : route) near = near || std::hypot(w.x - it->second.x, w.y - it->second.y) < 12.0f;
                if (!near) route.push_back(it->second); // in step order, before the unordered rest
            }
    }
    for (const auto& kv : vars) target(kv.second);
    for (const auto& g : groups) for (const long gid : g.second) target(gid);
    {
        auto nearAny = [&](const std::vector<Pt>& v, Pt q) { for (const Pt& w : v) if (std::hypot(w.x - q.x, w.y - q.y) < 12.0f) return true; return false; };
        int visited = 0;
        std::vector<Pt> todo;
        for (const Pt& q : loose) if (!nearAny(route, q) && !nearAny(todo, q)) todo.push_back(q);
        Pt cur = route.empty() ? (todo.empty() ? Pt{} : todo[0]) : route.back();
        while (!todo.empty()) {
            size_t best = 0;
            float bd = 1e9f;
            for (size_t i = 0; i < todo.size(); ++i) { const float d = std::hypot(todo[i].x - cur.x, todo[i].y - cur.y); if (d < bd) { bd = d; best = i; } }
            cur = todo[best];
            route.push_back(cur);
            ++visited;
            todo.erase(todo.begin() + static_cast<long>(best));
        }
        if (r.objectives == 0) r.objectives = steps.empty() ? visited : static_cast<int>(steps.size()); // an older quest: its steps, else the objects its script works with
    }
    if (in.remove.set) route.push_back({(in.remove.x1 + in.remove.x2) / 2, (in.remove.y1 + in.remove.y2) / 2});
    std::vector<Pt> samples;
    float len = 0;
    for (size_t i = 0; i + 1 < route.size(); ++i) {
        const float dx = route[i + 1].x - route[i].x, dy = route[i + 1].y - route[i].y, d = std::sqrt(dx * dx + dy * dy);
        for (float t = 0; t < d; t += 2.0f) samples.push_back({route[i].x + dx * t / d, route[i].y + dy * t / d});
        len += d;
    }
    for (const Pt& q : route) r.route.push_back({q.x, q.y});
    r.routeLength = len;

    // -- which units count: every enemy of the quest's own map and of the zone's base map (the quest runs on top of its zone)
    for (Enemy& e : enemies) e.relevant = true;

    // -- pulls: units closer than the larger of their call-for-help radii; a pull counts when it holds a unit the quest meets
    std::vector<int> parent(enemies.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
    auto find = [&](int i) { while (parent[static_cast<size_t>(i)] != i) { parent[static_cast<size_t>(i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(i)])]; i = parent[static_cast<size_t>(i)]; } return i; };
    for (size_t a = 0; a < enemies.size(); ++a)
        for (size_t b = a + 1; b < enemies.size(); ++b) {
            const float dx = enemies[a].pos.x - enemies[b].pos.x, dy = enemies[a].pos.y - enemies[b].pos.y, link = std::max(enemies[a].help, enemies[b].help);
            if (dx * dx + dy * dy <= link * link) parent[static_cast<size_t>(find(static_cast<int>(a)))] = find(static_cast<int>(b));
        }
    std::map<int, std::vector<size_t>> groupsOf;
    for (size_t i = 0; i < enemies.size(); ++i) groupsOf[find(static_cast<int>(i))].push_back(i);
    std::vector<const Enemy*> kept;
    std::vector<int> perFile(in.files.size(), 0);
    double n2 = 0, n1 = 0;
    for (const auto& g : groupsOf) {
        bool any = false;
        for (size_t i : g.second) any = any || enemies[i].relevant;
        if (!any) continue;
        Pull pull;
        std::map<std::string, int> names;
        float cx = 0, cy = 0;
        for (size_t i : g.second) {
            const Enemy& e = enemies[i];
            kept.push_back(&e);
            if (e.stored) ++r.storedStats;
            ++perFile[e.file];
            pull.effHp += e.power.effHp; pull.dps += e.power.dps; pull.ranged += e.power.ranged ? 1 : 0;
            cx += e.pos.x; cy += e.pos.y; ++names[e.name.empty() ? "?" : e.name];
            if (!e.known) { ++r.unknown; r.unknownNames.push_back(e.name); }
        }
        pull.units = static_cast<int>(g.second.size());
        pull.x = cx / pull.units; pull.y = cy / pull.units;
        for (size_t i : g.second) pull.radius = std::max(pull.radius, std::hypot(enemies[i].pos.x - pull.x, enemies[i].pos.y - pull.y) + enemies[i].radius);
        pull.power = std::sqrt(std::max(0.0f, pull.effHp) * std::max(0.0f, pull.dps)) * (1.0f + 0.25f * pull.ranged / pull.units);
        pull.norm = pull.power / anchorPull;
        for (const auto& nm : names) pull.names.push_back(std::to_string(nm.second) + "x " + nm.first);
        if (pull.norm >= 0.002f) ++r.fights;
        r.sumNorm += pull.norm;
        r.peakNorm = std::max<double>(r.peakNorm, pull.norm);
        n2 += static_cast<double>(pull.units) * pull.units;
        n1 += pull.units;
        r.units += pull.units;
        r.pulls.push_back(std::move(pull));
    }
    std::sort(r.pulls.begin(), r.pulls.end(), [](const Pull& a, const Pull& b) { return a.x < b.x; });

    for (size_t i = 0; i < in.files.size(); ++i) if (in.files[i]) r.files.push_back({in.files[i]->fileName, perFile[i]});

    // -- how much of the walk is seen
    int seen = 0;
    for (const Pt& s : samples)
        for (const Enemy* e : kept) if (Notices(*e, s)) { ++seen; break; }
    r.exposure = samples.empty() ? 0.0f : static_cast<float>(seen) / samples.size();

    // -- scripted spawns
    double spawnPower = 0;
    for (const Call& c : calls) {
        if (c.name != "addunittoserver" || c.args.size() < 2) continue;
        ++r.spawnSites;
        if (const Monster* m = db.Find(c.args[1])) { ++r.spawnUnits; spawnPower += PowerOf(*m, db, p).power; }
    }

    // -- the factors, in points
    // Enemy power: the late quests are hundreds of times stronger than the early ones, so both the pulls' total and the
    // strongest pull go through a log scale (0 at 2% / 0.3% of the anchor group's power, 1 at 10x / 1x of it).
    auto logmap = [](double x, double lo, double hi) { return x <= lo ? 0.0 : ClampD(std::log10(x / lo) / std::log10(hi / lo), 0, 1); };
    const double strength = 0.5 * logmap(r.sumNorm, 0.02, 10.0) + 0.5 * logmap(r.peakNorm, 0.003, 1.0);
    const double avgPull = n1 > 0 ? n2 / n1 : 1;
    const double density = ClampD((avgPull - 1.0) / (p.anchorPullUnits - 1.0), 0, 1);
    const double stealth = ClampD(r.exposure * 1.8, 0, 1);
    const double duration = ClampD(len / 800.0, 0, 1) * ClampD(r.fights / 3.0, 0, 1);
    const double ambush = ClampD(0.2 * std::sqrt(static_cast<double>(r.spawnSites)) + 1.5 * spawnPower / anchorPull, 0, 1);
    const double f[kFactors] = {strength, density, stealth, duration, ambush};
    r.total = 0;
    for (int i = 0; i < kFactors; ++i) { r.pts[static_cast<size_t>(i)] = f[i] * kMaxPoints[static_cast<size_t>(i)]; r.total += r.pts[static_cast<size_t>(i)]; }
    r.total = ClampD(r.total, 0, 10000);
    r.ok = true;
    if (r.units == 0) r.note = "no enemy units found";
    return r;
}

} // namespace rating
