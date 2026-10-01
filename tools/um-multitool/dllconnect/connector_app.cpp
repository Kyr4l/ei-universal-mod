// The UM DLL Connector tab (see connector_app.hpp). A background thread holds the TCP connection to
// um.dll and hands the lines it receives to the GUI thread; the GUI parses them and draws the sub-tabs.

#ifdef _WIN32
#include <winsock2.h> // before anything that includes windows.h
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include "connector_app.hpp"
#include "radar.hpp"
#include "quests.hpp"
#include "mapedit/script_highlight.hpp"

#include <algorithm>
#include <set>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "../viewer/library.hpp"
#include "../viewer/ui_common.hpp"

namespace dllconnect {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool SetNonBlocking(Socket s, bool on) { u_long mode = on ? 1 : 0; return ioctlsocket(s, FIONBIO, &mode) == 0; }
int LastError() { return WSAGetLastError(); }
bool InProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void StartSockets() { static bool started = false; if (!started) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); started = true; } }
#else
using Socket = int;
const Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool SetNonBlocking(Socket s, bool on) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
int LastError() { return errno; }
bool InProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK; }
void StartSockets() {}
#endif

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string TimeStamp() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

// The connection, on its own thread. The GUI thread starts it, reads `incoming`, queues `outgoing`, and
// stops it; the thread ends by itself when the game closes the connection.
struct Link {
    enum State { Idle, Connecting, Connected };
    std::thread thread;
    std::atomic<int> state{Idle};
    std::atomic<bool> stop{false}, finished{true};
    std::mutex lock;
    std::deque<std::string> incoming, outgoing; // lines without their '\n'
    std::string error;                          // why the last attempt or connection ended

    void Start(int port) {
        Join();
        stop = false;
        finished = false;
        state = Connecting;
        {
            std::lock_guard<std::mutex> g(lock);
            incoming.clear();
            outgoing.clear();
            error.clear();
        }
        thread = std::thread([this, port] { Run(port); finished = true; });
    }
    void Stop() { stop = true; Join(); }
    void Join() { if (thread.joinable()) thread.join(); }
    void Send(const std::string& line) { std::lock_guard<std::mutex> g(lock); outgoing.push_back(line); }
    void Fail(const std::string& why) { std::lock_guard<std::mutex> g(lock); error = why; }

    void Run(int port) {
        StartSockets();
        Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket) { Fail("cannot create a socket"); state = Idle; return; }
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // Connect without blocking, so that Stop() is never kept waiting.
        SetNonBlocking(s, true);
        bool connected = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        if (!connected && !InProgress(LastError())) { Fail("refused (is the game running with DLL_SERVER_ENABLED=true?)"); CloseSocket(s); state = Idle; return; }
        for (int waited = 0; !connected && waited < 3000 && !stop; waited += 100) {
            fd_set writable, failed;
            FD_ZERO(&writable); FD_ZERO(&failed);
            FD_SET(s, &writable); FD_SET(s, &failed);
            timeval tv{0, 100000};
            if (select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                if (err != 0 || FD_ISSET(s, &failed)) { Fail("refused (is the game running with DLL_SERVER_ENABLED=true?)"); CloseSocket(s); state = Idle; return; }
                connected = true;
            }
        }
        if (!connected) { if (!stop) Fail("no answer"); CloseSocket(s); state = Idle; return; }
        state = Connected;
        std::string pending;
        while (!stop) {
            std::deque<std::string> out;
            { std::lock_guard<std::mutex> g(lock); out.swap(outgoing); }
            for (const std::string& line : out) {
                const std::string text = line + "\n";
                size_t sent = 0;
                while (sent < text.size() && !stop) {
                    const int n = static_cast<int>(send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));
                    if (n > 0) { sent += static_cast<size_t>(n); continue; }
                    if (!InProgress(LastError())) { Fail("the connection broke while sending"); CloseSocket(s); state = Idle; return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            timeval tv{0, 100000};
            if (select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &tv) <= 0) continue;
            char buf[4096];
            const int n = static_cast<int>(recv(s, buf, sizeof(buf), 0));
            if (n == 0) { Fail("the game closed the connection"); break; }
            if (n < 0) {
                if (InProgress(LastError())) continue;
                Fail("the connection broke");
                break;
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t end;
            std::lock_guard<std::mutex> g(lock);
            while ((end = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                incoming.push_back(line);
            }
        }
        CloseSocket(s);
        state = Idle;
    }
};

// A value's recent history for a graph: one sample a second.
struct History {
    static constexpr int kSize = 120;
    std::vector<float> values;
    void Add(float v) { values.push_back(v); if (static_cast<int>(values.size()) > kSize) values.erase(values.begin()); }
    float Max() const { float m = 0; for (float v : values) m = std::max(m, v); return m; }
};

} // namespace

struct Context {
    Library& lib;
    Link link;
    bool wanted = false;        // connect, or keep connected (the Connect button, or auto-connect)
    double nextAttempt = 0;     // when to try again after a failed attempt
    bool wasConnected = false;
    int portEdit = 0;           // the port field while it is edited
    bool portEditing = false;
    bool tabRestored = false;   // the sub-tab open last time was selected
    // What um.dll said.
    std::map<std::string, std::string> hello;          // version, proto, pid
    std::map<std::string, double> stats;               // the last STATS line
    double statsTime = 0;                              // when it arrived
    History cpu, workingSet, privateBytes;
    std::deque<std::string> log;                        // connection events and messages (not STATS)
    char command[512] = "";                             // the Commands tab's command line
    std::vector<std::string> history;                   // and the commands sent
    int historyAt = -1;
    bool scrollToEnd = false;
    Hooks hooks;
    // The commands sent, oldest first, to route their answers (ROW / OK / ERR <WORD>): the tab's own
    // polling (radar, game console, map) is not shown in the Commands log.
    struct Pending { std::string word; bool internal; };
    std::deque<Pending> pending;
    // Radar.
    std::vector<RadarUnit> units, incomingUnits;
    double unitsTime = 0, nextUnitsPoll = 0, nextMapPoll = 0, radarShown = -100, consoleShown = -100;
    RadarMap map;
    RadarView view;
    RadarCamera camera;
    std::string mapMessage;
    // The game's console.
    std::vector<std::string> consoleLines;
    std::string consoleInput, consoleStatus;
    int consoleOpen = -1;                       // the game's console: 1 open, 0 closed, -1 not reported
    unsigned consoleCount = 0;
    double nextConsolePoll = 0;
    char consoleCommand[512] = "";
    std::vector<std::string> consoleHistory;
    int consoleHistoryAt = -1;
    bool consoleScroll = false;
    // Quests: the map's quests and scripts (from its files) and the game's state of them (VARS, SCRIPTS).
    quests::Model quests;
    std::map<std::string, float> vars, incomingVars;          // "q.z3xq3.z3xq3.4" -> 1
    struct ScriptState { int map = 0; unsigned refs = 0; bool running = false; };
    std::map<std::string, ScriptState> scriptStates, incomingScripts;
    double questsShown = -100, nextQuestPoll = 0, varsTime = 0;
    int scriptSource = 0, scrollToLine = -1;                  // the script view: the file shown, a line to show
    std::vector<scripthl::ScriptNames> scriptNames;           // per source, for the highlighting
    Context(Library& l, Hooks h) : lib(l), hooks(std::move(h)) { wanted = lib.dllAutoConnect; }
    ~Context() { map.Drop(); }
};

namespace {

void AddLog(Context& c, const std::string& text) {
    c.log.push_back(TimeStamp() + "  " + text);
    while (c.log.size() > 20000) c.log.pop_front();
}

// "WORD key=value key="quoted value" ..." -> the pairs (quoted values unescaped: \" \\ \xHH).
std::map<std::string, std::string> Pairs(const std::string& line) {
    std::map<std::string, std::string> out;
    size_t i = line.find(' ');
    while (i != std::string::npos && i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t eq = line.find('=', i);
        const size_t space = line.find(' ', i);
        if (eq == std::string::npos || (space != std::string::npos && space < eq)) { i = space; continue; }
        const std::string key = line.substr(i, eq - i);
        std::string value;
        i = eq + 1;
        if (i < line.size() && line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                    if (line[i] == 'x' && i + 2 < line.size()) { value += static_cast<char>(std::strtoul(line.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
                    else value += line[i];
                } else {
                    value += line[i];
                }
            }
            ++i;
        } else {
            const size_t end = line.find(' ', i);
            value = line.substr(i, end == std::string::npos ? std::string::npos : end - i);
            i = end;
        }
        out[key] = value;
    }
    return out;
}

void SendCommand(Context& c, const std::string& command, bool internal) {
    std::string word = command.substr(0, command.find(' '));
    for (char& ch : word) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    c.link.Send(command);
    c.pending.push_back({word, internal});
}

// The map the game runs changed: load its files (found as the Map Editor would) for the radar.
void LoadGameMap(Context& c, const std::map<std::string, std::string>& kv) {
    auto get = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
    const std::string terrain = get("terrain"), base = get("base"), quest = get("quest");
    std::vector<std::string> gamePaths;
    for (const char* k : {"terrain_path", "base_path", "quest_path"})
        if (!get(k).empty()) gamePaths.push_back(get(k));
    if (terrain == c.map.terrainName && base == c.map.baseName && quest == c.map.questName && gamePaths == c.map.gamePaths) return;
    c.map.Drop();
    c.quests.Clear();
    c.scriptNames.clear();
    c.scriptSource = 0;
    c.map.gamePaths = gamePaths;
    c.map.terrainName = terrain;
    c.map.baseName = base;
    c.map.questName = quest;
    c.view.pixelsPerUnit = 0;
    c.view.followId = 0;
    if (!c.hooks.resolveMap || terrain.empty()) return;
    std::string terrainPath, missing;
    std::vector<std::string> mobs;
    c.hooks.resolveMap(terrain, base, quest, gamePaths, terrainPath, mobs, missing);
    c.map.missing = missing;
    if (!terrainPath.empty()) {
        mpr::Map m;
        std::string err;
        if (mpr::Load(terrainPath, m, err)) c.map.BuildTerrain(m);
        else c.map.missing += " " + terrain + " (" + err + ")";
    }
    for (const std::string& path : mobs) {
        mob::File f;
        if (!mob::Load(path, f)) continue;
        c.map.AddMob(f);
        const size_t before = c.quests.quests.size();
        quests::AddMob(c.quests, f);
        // The quests it declares take their texts from the .mq beside it (same name), else from the
        // Settings' quest folders and language packs.
        const size_t dot = path.find_last_of('.');
        for (size_t i = before; i < c.quests.quests.size(); ++i) {
            quests::Quest& q = c.quests.quests[i];
            if (dot == std::string::npos || !quests::LoadTexts(path.substr(0, dot) + ".mq", q)) quests::LoadTexts(c.lib.questFolders, c.lib.questPacks, q);
        }
    }
    AddLog(c, "radar: map " + terrain + (base.empty() ? "" : " + " + base) + (quest.empty() ? "" : " + " + quest) +
                  (missing.empty() ? "" : " (not found:" + missing + ")"));
}

// Answers to the tab's own commands.
void HandleInternal(Context& c, const std::string& word, const std::string& line, bool last) {
    const auto kv = Pairs(line);
    auto num = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? 0.0 : std::atof(it->second.c_str()); };
    if (word == "UNITS") {
        if (line.compare(0, 4, "ROW ") == 0) {
            RadarUnit u;
            u.id = static_cast<unsigned>(std::strtoul(kv.count("id") ? kv.at("id").c_str() : "0", nullptr, 10));
            u.side = static_cast<unsigned>(num("side"));
            u.flags = static_cast<unsigned>(std::strtoul(kv.count("flags") ? kv.at("flags").c_str() : "0", nullptr, 16));
            u.x = static_cast<float>(num("x")); u.y = static_cast<float>(num("y")); u.z = static_cast<float>(num("z"));
            u.yaw = static_cast<float>(num("yaw"));
            u.hp = static_cast<float>(num("hp")); u.hpMax = static_cast<float>(num("hpmax"));
            u.mana = static_cast<float>(num("mp")); u.manaMax = static_cast<float>(num("mpmax"));
            if (kv.count("name")) u.name = kv.at("name");
            u.sight = static_cast<float>(num("sight")); u.viewAngle = static_cast<float>(num("fov"));
            c.incomingUnits.push_back(u);
        } else if (last) {
            if (line.compare(0, 3, "OK ") == 0) { c.units.swap(c.incomingUnits); c.unitsTime = NowSeconds(); }
            c.incomingUnits.clear();
        }
    } else if (word == "CONSOLE") {
        if (line.compare(0, 4, "ROW ") == 0) {
            const unsigned i = static_cast<unsigned>(num("i"));
            if (i < 200000) {
                if (c.consoleLines.size() <= i) c.consoleLines.resize(i + 1);
                c.consoleLines[i] = kv.count("text") ? kv.at("text") : "";
                c.consoleScroll = true;
            }
        } else if (line.compare(0, 4, "ERR ") == 0) {
            c.consoleStatus = line.substr(12);
        } else if (kv.count("sent")) {
            c.consoleStatus = "sent: " + kv.at("sent");
        } else {
            const unsigned count = static_cast<unsigned>(num("count"));
            if (count < c.consoleLines.size()) c.consoleLines.resize(count); // cleared in the game
            c.consoleCount = count;
            c.consoleInput = kv.count("input") ? kv.at("input") : "";
            c.consoleOpen = kv.count("open") ? std::atoi(kv.at("open").c_str()) : -1;
            if (c.consoleStatus.compare(0, 4, "sent") != 0) c.consoleStatus.clear();
        }
    } else if (word == "VARS") {
        if (line.compare(0, 4, "ROW ") == 0 && kv.count("name")) c.incomingVars[kv.at("name")] = static_cast<float>(num("value"));
        else if (last) {
            if (line.compare(0, 3, "OK ") == 0) { c.vars.swap(c.incomingVars); c.varsTime = NowSeconds(); }
            c.incomingVars.clear();
        }
    } else if (word == "SCRIPTS") {
        if (line.compare(0, 4, "ROW ") == 0 && kv.count("name")) {
            Context::ScriptState& s = c.incomingScripts[kv.at("name")];
            s.map = static_cast<int>(num("map"));
            s.refs = std::max(s.refs, static_cast<unsigned>(num("refs")));
            s.running = s.running || num("running") > 0;
        } else if (last) {
            if (line.compare(0, 3, "OK ") == 0) c.scriptStates.swap(c.incomingScripts);
            c.incomingScripts.clear();
        }
    } else if (word == "CAMERA" && last) {
        c.camera.valid = line.compare(0, 3, "OK ") == 0;
        c.camera.x = static_cast<float>(num("x")); c.camera.y = static_cast<float>(num("y"));
        c.camera.targetX = static_cast<float>(num("tx")); c.camera.targetY = static_cast<float>(num("ty"));
    } else if (word == "MAP" && last && line.compare(0, 3, "OK ") == 0) {
        LoadGameMap(c, kv);
    }
}

void Handle(Context& c, const std::string& line) {
    const std::string word = line.substr(0, line.find(' '));
    if (word == "STATS") {
        c.stats.clear();
        for (const auto& kv : Pairs(line)) c.stats[kv.first] = std::atof(kv.second.c_str());
        c.statsTime = NowSeconds();
        c.cpu.Add(static_cast<float>(c.stats["cpu"]));
        c.workingSet.Add(static_cast<float>(c.stats["ws_mb"]));
        c.privateBytes.Add(static_cast<float>(c.stats["private_mb"]));
        return;
    }
    if (word == "HELLO") c.hello = Pairs(line);
    const bool row = line.compare(0, 4, "ROW ") == 0, ok = line.compare(0, 3, "OK ") == 0 || line == "OK", err = line.compare(0, 4, "ERR ") == 0;
    if ((row || ok || err) && !c.pending.empty()) {
        const size_t start = row ? 4 : ok ? 3 : 4;
        const std::string answered = line.substr(start, line.find(' ', start) - start);
        if (answered == c.pending.front().word) {
            const Context::Pending p = c.pending.front();
            if (ok || err) c.pending.pop_front();
            if (p.internal) { HandleInternal(c, p.word, line, ok || err); return; }
        }
    }
    AddLog(c, "< " + line);
}

double Stat(const Context& c, const char* key) {
    auto it = c.stats.find(key);
    return it == c.stats.end() ? 0.0 : it->second;
}

void StatRow(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void StatRow(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void Graph(const char* id, const History& h, float maxValue, const char* overlay) {
    if (h.values.empty()) return;
    ImGui::PlotLines(id, h.values.data(), static_cast<int>(h.values.size()), 0, overlay, 0.0f, maxValue,
                     ImVec2(-1, 70));
}

void StatisticsTab(Context& c) {
    if (c.stats.empty()) {
        ImGui::TextDisabled("No statistics yet: they come once connected, every second.");
        return;
    }
    const bool fresh = NowSeconds() - c.statsTime < 3.0;
    if (!fresh) ui::Note("These are the last values received: the game is not sending any more.");
    const double cores = std::max(1.0, Stat(c, "cores"));
    char overlay[64];

    ImGui::SeparatorText("CPU");
    if (ImGui::BeginTable("##cpu", 2, ImGuiTableFlags_SizingFixedFit)) {
        StatRow("game.exe", "%.1f %% of the computer (%.0f logical processors)", Stat(c, "cpu"), cores);
        StatRow("", "%.0f %% of one core", Stat(c, "cpu") * cores);
        StatRow("Threads", "%.0f", Stat(c, "threads"));
        ImGui::EndTable();
    }
    std::snprintf(overlay, sizeof(overlay), "CPU, last %d s (0-100 %% of one core)", History::kSize);
    {
        // Graphed per core: a game that keeps one core busy reads 100 %, whatever the number of cores.
        History perCore;
        for (float v : c.cpu.values) perCore.Add(static_cast<float>(v * cores));
        Graph("##cpugraph", perCore, std::max(100.0f, perCore.Max()), overlay);
    }

    ImGui::SeparatorText("Memory");
    if (ImGui::BeginTable("##mem", 2, ImGuiTableFlags_SizingFixedFit)) {
        StatRow("RAM in use", "%.1f MB (working set; peak %.1f MB)", Stat(c, "ws_mb"), Stat(c, "peak_ws_mb"));
        if (Stat(c, "private_mb") > 0) StatRow("Private", "%.1f MB (memory the game allocated)", Stat(c, "private_mb"));
        StatRow("Address space", "%.0f of %.0f MB used", Stat(c, "vas_used_mb"), Stat(c, "vas_total_mb"));
        ImGui::EndTable();
    }
    const double vasTotal = Stat(c, "vas_total_mb");
    if (vasTotal > 0) {
        const double used = Stat(c, "vas_used_mb") / vasTotal;
        std::snprintf(overlay, sizeof(overlay), "address space %.0f %%", used * 100.0);
        if (used > 0.85) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.3f, 0.25f, 1.0f));
        ImGui::ProgressBar(static_cast<float>(used), ImVec2(-1, 0), overlay);
        if (used > 0.85) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("game.exe is a 32-bit program: it can address 2 GB, or 4 GB when it is large address aware.\n"
                              "It crashes when this is full, whatever the free RAM.");
    }
    std::snprintf(overlay, sizeof(overlay), "RAM in use, last %d s (MB)", History::kSize);
    Graph("##wsgraph", c.workingSet, std::max(64.0f, c.workingSet.Max() * 1.15f), overlay);

    ImGui::SeparatorText("Computer");
    if (ImGui::BeginTable("##sys", 2, ImGuiTableFlags_SizingFixedFit)) {
        const double total = Stat(c, "sys_ram_mb"), freeMb = Stat(c, "sys_ram_free_mb");
        StatRow("RAM", "%.0f of %.0f MB used (%.0f MB free)", total - freeMb, total, freeMb);
        const long long up = static_cast<long long>(Stat(c, "uptime_s"));
        StatRow("Game running for", "%lld:%02lld:%02lld", up / 3600, (up / 60) % 60, up % 60);
        ImGui::EndTable();
    }
}

void Send(Context& c, const std::string& command) {
    SendCommand(c, command, false);
    AddLog(c, "> " + command);
    if (c.history.empty() || c.history.back() != command) c.history.push_back(command);
    c.historyAt = -1;
}

// Up / Down in the command line: the commands sent before.
int CommandCallback(ImGuiInputTextCallbackData* data) {
    Context& c = *static_cast<Context*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || c.history.empty()) return 0;
    const int n = static_cast<int>(c.history.size());
    if (data->EventKey == ImGuiKey_UpArrow) c.historyAt = c.historyAt < 0 ? n - 1 : std::max(0, c.historyAt - 1);
    else if (data->EventKey == ImGuiKey_DownArrow) c.historyAt = c.historyAt < 0 || c.historyAt + 1 >= n ? -1 : c.historyAt + 1;
    data->DeleteChars(0, data->BufTextLen);
    if (c.historyAt >= 0) data->InsertChars(0, c.history[static_cast<size_t>(c.historyAt)].c_str());
    return 0;
}

// Commands to um.dll and everything it answers (not the statistics): the protocol and the commands are
// described in resources/universal-mod/um-dll/dll_server.hpp; HELP lists them.
void CommandsTab(Context& c) {
    const bool connected = c.link.state == Link::Connected;
    ImGui::BeginDisabled(!connected);
    for (const char* quick : {"HELP", "INFO", "MAP", "THREADS", "BP list"}) {
        if (ImGui::Button(quick)) Send(c, quick);
        ImGui::SameLine();
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Clear")) c.log.clear();
    const float lineHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log", ImVec2(0, -lineHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(c.log.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const std::string& line = c.log[static_cast<size_t>(i)];
            const bool mine = line.find("  > ") != std::string::npos, error = line.find("  < ERR ") != std::string::npos;
            if (mine || error) ImGui::PushStyleColor(ImGuiCol_Text, mine ? ImVec4(0.55f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextUnformatted(line.c_str());
            if (mine || error) ImGui::PopStyleColor();
        }
    if (c.scrollToEnd || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    c.scrollToEnd = false;
    ImGui::EndChild();
    ImGui::BeginDisabled(!connected);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##command", "a command for um.dll (HELP lists them), Enter sends it; Up/Down: the previous ones",
                                 c.command, sizeof(c.command), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                 CommandCallback, &c)) {
        if (c.command[0]) Send(c, c.command);
        c.command[0] = '\0';
        c.scrollToEnd = true;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndDisabled();
}

} // namespace

// The radar: the units on the map, updated 5 times a second while it is shown.
void RadarTab(Context& c) {
    c.radarShown = NowSeconds();
    const bool connected = c.link.state == Link::Connected;
    if (!c.map.terrainName.empty()) {
        ImGui::TextDisabled("%s%s%s", c.map.terrainName.c_str(), c.map.baseName.empty() ? "" : ("  " + c.map.baseName).c_str(),
                            c.map.questName.empty() ? "" : ("  " + c.map.questName).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!c.hooks.openInMapEditor);
        if (ImGui::SmallButton("Open in the Map Editor")) c.mapMessage = c.hooks.openInMapEditor(c.map.terrainName, c.map.baseName, c.map.questName, c.map.gamePaths);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens these files in the Map Editor tab (the quest, when there is one, as the Map Editor's Quest tab does)");
        if (!c.map.missing.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "not found in the map folders:%s", c.map.missing.c_str());
        }
        if (!c.mapMessage.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", c.mapMessage.c_str()); }
    } else if (!connected) {
        ImGui::TextDisabled("Not connected.");
        return;
    }
    DrawRadar(c.view, c.map, c.units, c.camera, NowSeconds() - c.unitsTime < 2.0);
}

// The game's own console: its lines, and commands typed into it.
int GameCommandCallback(ImGuiInputTextCallbackData* data) {
    Context& c = *static_cast<Context*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || c.consoleHistory.empty()) return 0;
    const int n = static_cast<int>(c.consoleHistory.size());
    if (data->EventKey == ImGuiKey_UpArrow) c.consoleHistoryAt = c.consoleHistoryAt < 0 ? n - 1 : std::max(0, c.consoleHistoryAt - 1);
    else if (data->EventKey == ImGuiKey_DownArrow) c.consoleHistoryAt = c.consoleHistoryAt < 0 || c.consoleHistoryAt + 1 >= n ? -1 : c.consoleHistoryAt + 1;
    data->DeleteChars(0, data->BufTextLen);
    if (c.consoleHistoryAt >= 0) data->InsertChars(0, c.consoleHistory[static_cast<size_t>(c.consoleHistoryAt)].c_str());
    return 0;
}

// ---- Quests ---------------------------------------------------------------------------------------------

// What can be seen now of an objective's condition (from the units um.dll reports and the map files).
std::string LiveCheck(const Context& c, const quests::Call& call, ImVec4& color) {
    const ImVec4 good(0.5f, 0.85f, 0.5f, 1), bad(0.95f, 0.75f, 0.35f, 1), grey(0.6f, 0.6f, 0.6f, 1);
    color = grey;
    std::vector<const RadarUnit*> heroes;
    for (const RadarUnit& u : c.units) if (!u.name.empty() && !u.Dead()) heroes.push_back(&u);
    auto nearest = [&](float x, float y, const RadarUnit** who) {
        float best = 1e30f;
        for (const RadarUnit* h : heroes) {
            const float d = std::hypot(h->x - x, h->y - y);
            if (d < best) { best = d; if (who) *who = h; }
        }
        return best;
    };
    auto unitById = [&](unsigned id) -> const RadarUnit* { for (const RadarUnit& u : c.units) if (u.id == id) return &u; return nullptr; };
    char buf[160];
    const std::string a0 = call.args.empty() ? "" : call.args[0];
    if (heroes.empty() && call.name != "QObjGetItem") return "(no player character seen)";
    if (call.name == "QObjArea") {
        auto it = c.quests.areas.find(std::atoi(a0.c_str()));
        if (it == c.quests.areas.end()) return "area not declared in the scripts";
        float best = 1e30f;
        for (const quests::Area& a : it->second)
            for (const RadarUnit* h : heroes) best = std::min(best, a.Distance(h->x, h->y));
        color = best <= 0 ? good : bad;
        snprintf(buf, sizeof(buf), best <= 0 ? "a hero is inside" : "nearest hero %.0f away", best);
        return buf;
    }
    if (call.name == "QObjKillGroup") {
        auto it = c.quests.groups.find(a0);
        if (it == c.quests.groups.end()) return "group not filled by the scripts";
        int alive = 0;
        for (unsigned id : it->second) if (const RadarUnit* u = unitById(id); u && !u->Dead()) ++alive;
        color = alive == 0 ? good : bad;
        snprintf(buf, sizeof(buf), "%d of %zu alive", alive, it->second.size());
        return buf;
    }
    if (call.name == "QObjSeeUnit" || call.name == "QObjKillUnit") {
        const RadarUnit* u = unitById(quests::ObjectId(a0));
        if (!u || u->Dead()) { color = call.name == "QObjKillUnit" ? good : grey; return u ? "dead" : "not seen (dead and looted, or not loaded)"; }
        const RadarUnit* who = nullptr;
        const float d = nearest(u->x, u->y, &who);
        const bool inSight = who && who->sight > 0 && d <= who->sight;
        color = call.name == "QObjSeeUnit" && inSight ? good : bad;
        snprintf(buf, sizeof(buf), "alive, HP %.0f/%.0f, %.0f from %s%s", u->hp, u->hpMax, d, who ? who->name.c_str() : "?",
                 inSight ? " (within sight range)" : "");
        return buf;
    }
    if (call.name == "QObjSeeObject" || call.name == "QObjUse") {
        auto it = c.quests.objects.find(quests::ObjectId(a0));
        if (it == c.quests.objects.end()) return "object not in the map files";
        const float d = nearest(it->second.x, it->second.y, nullptr);
        color = call.name == "QObjSeeObject" && d <= 7.0f ? good : bad;
        snprintf(buf, sizeof(buf), "nearest hero %.0f away%s", d, call.name == "QObjSeeObject" ? " (needs 7)" : "");
        return buf;
    }
    if (call.name == "QObjGetItem") return "(the inventory is not read yet)";
    return "";
}

// The map's scripts as text, with where they are: the condition a running script waits on, the quest's
// objectives done or active. The engine gives no position inside WorldScript or an action list: only which
// scripts run (they wait on their condition) and the quest variables.
void ScriptView(Context& c) {
    auto& sources = c.quests.sources;
    if (sources.empty()) { ImGui::TextDisabled("No script in the map files."); return; }
    if (c.scriptNames.size() != sources.size()) {
        c.scriptNames.clear();
        for (const quests::Source& s : sources) c.scriptNames.push_back(scripthl::NamesOf(s.cp1251));
    }
    c.scriptSource = std::min(c.scriptSource, static_cast<int>(sources.size()) - 1);
    const ImVec4 waiting(1.0f, 0.86f, 0.4f, 1), done(0.5f, 0.85f, 0.5f, 1), dim(0.55f, 0.55f, 0.55f, 1);
    // Markers per line of each file, and the lines where something waits.
    struct Mark { std::string text; ImVec4 color; bool highlight = false; };
    std::vector<std::map<int, Mark>> marks(sources.size());
    const bool live = c.link.state == Link::Connected && !c.scriptStates.empty();
    std::vector<int> firstWaiting(sources.size(), -1);
    for (size_t si = 0; si < sources.size(); ++si) {
        // WorldScript: the map's setup (a short Sleep, then group and object assignments): it runs once when
        // the map loads and leaves no running instance to follow.
        if (sources[si].worldScript >= 0) marks[si][sources[si].worldScript] = {"runs at map load", dim};
        for (const quests::Source::Block& b : sources[si].blocks) {
            auto it = c.scriptStates.find(b.name);
            const bool running = live && it != c.scriptStates.end() && it->second.running;
            if (!live) continue;
            if (running && b.condFirst >= 0) {
                for (int l = b.condFirst; l <= b.condLast; ++l) marks[si][l] = {l == b.condFirst ? "<- waiting" : "", waiting, true};
                marks[si][b.header] = {"running", waiting};
                if (firstWaiting[si] < 0 || b.condFirst < firstWaiting[si]) firstWaiting[si] = b.condFirst;
            } else {
                marks[si][b.header] = {"not running", dim};
            }
        }
    }
    auto var = [&](const std::string& name) { auto it = c.vars.find(name); return it == c.vars.end() ? 0.0f : it->second; };
    for (const quests::Quest& q : c.quests.quests) {
        if (q.sourceIndex >= sources.size() || c.vars.empty()) continue;
        for (size_t k = 0; k < q.objectives.size(); ++k) {
            const int line = q.objectives[k].line;
            if (line < 0) continue;
            const float s = var("q." + q.name + "." + q.name + "." + std::to_string(k + 1));
            if (s >= 2) marks[q.sourceIndex][line] = {"done", done};
            else if (s >= 1) {
                marks[q.sourceIndex][line] = {"<- active", waiting, true};
                if (firstWaiting[q.sourceIndex] < 0) firstWaiting[q.sourceIndex] = line;
            } else marks[q.sourceIndex][line] = {"-", dim};
        }
    }
    // The file shown, and the buttons.
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##scriptfile", sources[c.scriptSource].file.c_str())) {
        for (size_t i = 0; i < sources.size(); ++i) {
            const std::string label = sources[i].file + (firstWaiting[i] >= 0 ? "  (something waits)" : "");
            if (ImGui::Selectable(label.c_str(), c.scriptSource == static_cast<int>(i))) c.scriptSource = static_cast<int>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // Find waiting: the shown file's waiting line, else the first file that has one (switching to it).
    int target = firstWaiting[c.scriptSource] >= 0 ? c.scriptSource : -1;
    for (size_t i = 0; i < sources.size() && target < 0; ++i) if (firstWaiting[i] >= 0) target = static_cast<int>(i);
    ImGui::BeginDisabled(target < 0);
    if (ImGui::Button("Find waiting")) { c.scriptSource = target; c.scrollToLine = firstWaiting[target]; }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", target >= 0 ? "Shows the condition a running script waits on, or the active quest objective"
                                : !live ? "Needs the connection to the game (the scripts' state comes from um.dll)"
                                        : "No script waits and no quest objective is active in the loaded map files");
    const int si = c.scriptSource;
    ImGui::SameLine();
    ImGui::TextDisabled(live ? "%zu lines" : "%zu lines (not connected: no state)", sources[si].lines.size());
    // The text: line number, marker, highlighted code.
    const quests::Source& src = sources[si];
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##scripttext", ImVec2(0, std::max(200.0f, ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (c.scrollToLine >= 0) { ImGui::SetScrollY(std::max(0.0f, c.scrollToLine * lineH - ImGui::GetWindowHeight() / 3)); c.scrollToLine = -1; }
    const float numW = ImGui::CalcTextSize("00000").x, markW = ImGui::CalcTextSize("(position unknown) ").x;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(src.lines.size()), lineH);
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto mk = marks[si].find(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (mk != marks[si].end() && mk->second.highlight)
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x + ImGui::GetScrollMaxX() + 2000, p.y + lineH),
                                                          ImGui::GetColorU32(ImVec4(0.45f, 0.36f, 0.08f, 0.45f)));
            ImGui::TextDisabled("%4d", i + 1);
            ImGui::SameLine(numW + 8);
            if (mk != marks[si].end()) ImGui::TextColored(mk->second.color, "%s", mk->second.text.c_str());
            else ImGui::TextUnformatted("");
            ImGui::SameLine(numW + 8 + markW);
            const char* b = src.utf8.data() + src.lines[i].first;
            const char* e = src.utf8.data() + src.lines[i].second;
            bool first = true;
            scripthl::EachScriptToken(b, e, c.scriptNames[si], [&](const char* tb, const char* te, const ImVec4& color) {
                if (tb >= te) return;
                if (!first) ImGui::SameLine(0.0f, 0.0f);
                first = false;
                ImGui::TextColored(color, "%.*s", static_cast<int>(te - tb), tb);
            });
            if (first) ImGui::TextUnformatted("");
        }
    ImGui::EndChild();
}

void QuestsTab(Context& c) {
    c.questsShown = NowSeconds();
    const ImVec4 doneColor(0.5f, 0.85f, 0.5f, 1), activeColor(1.0f, 0.86f, 0.4f, 1), pendingColor(0.6f, 0.6f, 0.6f, 1);
    if (c.link.state != Link::Connected) ImGui::TextDisabled("Not connected: the quests below are the map's, without the game's state.");
    if (c.quests.quests.empty() && c.quests.scripts.empty()) {
        ImGui::TextWrapped("No quest or script in the map the game runs (or its files were not found: see the Radar tab's message).");
    }
    auto var = [&](const std::string& name) { auto it = c.vars.find(name); return it == c.vars.end() ? 0.0f : it->second; };
    // The variables of this map's quests (the others are listed apart).
    std::set<std::string> known;
    for (const quests::Quest& q : c.quests.quests) {
        const std::string base = "q." + q.name + "." + q.name;
        known.insert(base);
        for (size_t k = 1; k <= q.objectives.size(); ++k) known.insert(base + "." + std::to_string(k));
    }
    for (const quests::Quest& q : c.quests.quests) {
        const std::string base = "q." + q.name + "." + q.name;
        known.insert(base);
        const float state = var(base);
        const char* status = state >= 2 ? "completed" : state >= 1 ? "running" : "not received";
        ImGui::PushID(q.name.c_str());
        const std::string header = (q.title.empty() ? q.name : q.title) + "  [" + status + "]  (" + q.name + ", " + q.file + ")###q";
        ImGui::PushStyleColor(ImGuiCol_Text, state >= 2 ? doneColor : state >= 1 ? activeColor : pendingColor);
        const bool open = ImGui::CollapsingHeader(header.c_str(), state == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        if (open) {
            if (!q.text.empty()) { ImGui::PushTextWrapPos(); ImGui::TextDisabled("%s", q.text.c_str()); ImGui::PopTextWrapPos(); }
            if (ImGui::BeginTable("##obj", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableHeadersRow();
                for (size_t k = 0; k < q.objectives.size(); ++k) {
                    const quests::Objective& o = q.objectives[k];
                    const std::string vn = base + "." + std::to_string(k + 1);
                    known.insert(vn);
                    const float s = var(vn);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(s >= 2 ? doneColor : s >= 1 ? activeColor : pendingColor, "%s", s >= 2 ? "done" : s >= 1 ? "active" : "-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu. %s", k + 1, o.title.empty() ? "(no text in the .mq)" : o.title.c_str());
                    if (!o.text.empty() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(o.text.c_str());
                        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(quests::Describe(c.quests, o.call).c_str());
                    ImGui::TableNextColumn();
                    if (s >= 2) { ImGui::TextDisabled("-"); continue; }
                    ImVec4 col;
                    const std::string live = LiveCheck(c, o.call, col);
                    ImGui::TextColored(col, "%s", live.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::PopID();
    }
    // Quest variables of other maps (quests received or done before), in the game's memory.
    std::vector<std::pair<std::string, float>> others;
    for (const auto& [n, v] : c.vars) if (!known.count(n)) others.push_back({n, v});
    if (!others.empty() && ImGui::CollapsingHeader(("Other quest variables (" + std::to_string(others.size()) + ")").c_str())) {
        for (const auto& [n, v] : others) ImGui::Text("%-36s %g", n.c_str(), v);
    }
    // The map's scripts.
    if (!c.quests.scripts.empty() &&
        ImGui::CollapsingHeader(("Scripts (" + std::to_string(c.quests.scripts.size()) + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Running: started and not ended yet (waiting for its condition). From the references the script engine holds.");
        if (ImGui::BeginTable("##scripts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Script");
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();
            for (const auto& [name, file] : c.quests.scripts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", file.c_str());
                ImGui::TableNextColumn();
                auto it = c.scriptStates.find(name);
                if (c.link.state != Link::Connected || c.scriptStates.empty()) ImGui::TextDisabled("?");
                else if (it == c.scriptStates.end()) ImGui::TextDisabled("not loaded");
                else if (it->second.running) ImGui::TextColored(activeColor, "running");
                else ImGui::TextColored(pendingColor, "ended or not started");
            }
            ImGui::EndTable();
        }
    }
    // The scripts as text, with where they wait (last: it takes the rest of the height).
    if (ImGui::CollapsingHeader("Script view", ImGuiTreeNodeFlags_DefaultOpen)) ScriptView(c);
}

void GameConsoleTab(Context& c) {
    c.consoleShown = NowSeconds();
    const bool connected = c.link.state == Link::Connected;
    if (ImGui::Button("Clear")) { c.consoleLines.clear(); c.consoleCount = 0; } // shows the new lines only
    ImGui::SameLine();
    ImGui::TextDisabled("%u line(s) in the game's console%s%s", c.consoleCount, c.consoleInput.empty() ? "" : "; typed there: ",
                        c.consoleInput.c_str());
    const float lineHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##gamelog", ImVec2(0, -lineHeight * 2), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4; // as last drawn: new lines keep it there
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(c.consoleLines.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(c.consoleLines[static_cast<size_t>(i)].c_str());
    if (c.consoleScroll && atBottom) ImGui::SetScrollHereY(1.0f);
    c.consoleScroll = false;
    ImGui::EndChild();
    ImGui::BeginDisabled(!connected || c.consoleOpen == 0);
    ImGui::SetNextItemWidth(-90);
    bool send = ImGui::InputTextWithHint("##gamecommand", "a command for the game's console; Enter sends it; Up/Down: the previous ones",
                                         c.consoleCommand, sizeof(c.consoleCommand),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, GameCommandCallback, &c);
    if (send) ImGui::SetKeyboardFocusHere(-1);
    ImGui::SameLine();
    send |= ImGui::Button("Send", ImVec2(-1, 0));
    ImGui::EndDisabled();
    if (send && c.consoleCommand[0]) {
        std::string text = c.consoleCommand, quoted = "\"";
        for (char ch : text) quoted += (ch == '"' || ch == '\\') ? std::string("\\") + ch : std::string(1, ch);
        SendCommand(c, "CONSOLE send " + quoted + "\"", true);
        if (c.consoleHistory.empty() || c.consoleHistory.back() != text) c.consoleHistory.push_back(text);
        c.consoleHistoryAt = -1;
        c.consoleCommand[0] = '\0';
    }
    if (c.consoleOpen == 0) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "The console is closed in the game: open it there to send commands.");
    else ImGui::TextDisabled("The command is typed into the game window, then Enter (needs DLL_SERVER_DEBUG=true). @, # and $ run script "
                             "functions once cheats are on (thingamabob).");
    if (!c.consoleStatus.empty()) { ImGui::SameLine(); ImGui::TextDisabled("  %s", c.consoleStatus.c_str()); }
}

Context* Create(Library& lib, Hooks hooks) { return new Context(lib, std::move(hooks)); }

void Destroy(Context* ctx) {
    if (!ctx) return;
    ctx->link.Stop();
    delete ctx;
}

void Update(Context* ctx) {
    Context& c = *ctx;
    std::deque<std::string> lines;
    std::string error;
    {
        std::lock_guard<std::mutex> g(c.link.lock);
        lines.swap(c.link.incoming);
        error.swap(c.link.error);
    }
    // A new connection (seen before its first lines are read): forget the last one's greeting.
    const bool connected = c.link.state == Link::Connected || !lines.empty();
    if (connected && !c.wasConnected) {
        AddLog(c, "connected to 127.0.0.1:" + std::to_string(c.lib.dllPort));
        c.hello.clear();
        c.pending.clear();
        c.consoleLines.clear();
        c.consoleCount = 0;
        c.nextUnitsPoll = c.nextMapPoll = c.nextConsolePoll = 0;
    }
    for (const std::string& line : lines) Handle(c, line);
    // The tab's own polling: the map every 2 s, the units 5 times a second while the radar is shown, the
    // console's new lines twice a second while it is shown (one request of a kind at a time).
    if (c.link.state == Link::Connected) {
        const double now = NowSeconds();
        auto waiting = [&](const char* word) { for (const auto& p : c.pending) if (p.internal && p.word == word) return true; return false; };
        if (now >= c.nextMapPoll && !waiting("MAP")) { SendCommand(c, "MAP", true); c.nextMapPoll = now + 2.0; }
        if (now - c.radarShown < 1.0 && now >= c.nextUnitsPoll && !waiting("UNITS")) {
            SendCommand(c, "UNITS", true);
            if (!waiting("CAMERA")) SendCommand(c, "CAMERA", true);
            c.nextUnitsPoll = now + 0.1;
        }
        // Quests: their state every 2 s, the units (for the live checks) twice a second, while shown.
        if (now - c.questsShown < 1.0) {
            if (now >= c.nextUnitsPoll && !waiting("UNITS")) { SendCommand(c, "UNITS", true); c.nextUnitsPoll = now + 0.5; }
            if (now >= c.nextQuestPoll && !waiting("VARS") && !waiting("SCRIPTS")) {
                SendCommand(c, "VARS q.", true);
                SendCommand(c, "SCRIPTS", true);
                c.nextQuestPoll = now + 2.0;
            }
        }
        if (now - c.consoleShown < 1.0 && now >= c.nextConsolePoll && !waiting("CONSOLE")) {
            SendCommand(c, "CONSOLE lines " + std::to_string(c.consoleLines.size()), true);
            c.nextConsolePoll = now + 0.5;
        }
    }
    if (!error.empty() && (c.wasConnected || !c.lib.dllAutoConnect)) AddLog(c, error); // retries stay quiet
    if (!connected && c.wasConnected) c.nextAttempt = NowSeconds() + 2.0;
    if (!error.empty()) c.nextAttempt = NowSeconds() + 2.0;
    c.wasConnected = connected;
    if (c.link.finished && c.link.thread.joinable()) c.link.Join();
    if (!c.link.finished) return;
    if (!c.wanted && !c.lib.dllAutoConnect) return;
    if (NowSeconds() < c.nextAttempt) return;
    c.link.Start(c.lib.dllPort);
    c.nextAttempt = NowSeconds() + 2.0;
}

void DrawTab(Context* ctx) {
    Context& c = *ctx;
    Library& lib = c.lib;
    const int state = c.link.state;
    const bool connected = state == Link::Connected;

    ImGui::BeginChild("##dllpanel", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ui::StatusDot(connected, "Connected to um.dll", "Not connected");
    ImGui::SameLine();
    if (connected) {
        auto get = [&](const char* k) { auto it = c.hello.find(k); return it == c.hello.end() ? std::string("?") : it->second; };
        ImGui::Text("Connected to um.dll %s (game.exe process %s)", get("version").c_str(), get("pid").c_str());
    } else if (state == Link::Connecting) {
        ImGui::TextUnformatted("Connecting...");
    } else if (c.wanted || lib.dllAutoConnect) {
        ImGui::TextUnformatted("Waiting for the game (trying every 2 seconds)");
    } else {
        ImGui::TextUnformatted("Not connected");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    // Edited in its own value and applied when the field is left (or Enter): the retries of auto-connect
    // must not reset it while typing. Locked only while connected.
    ImGui::BeginDisabled(connected);
    if (!c.portEditing) c.portEdit = lib.dllPort;
    ImGui::InputInt("##port", &c.portEdit, 0, 0);
    c.portEditing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && c.portEdit >= 1 && c.portEdit <= 65535 && c.portEdit != lib.dllPort) {
        lib.dllPort = c.portEdit;
        lib.SaveConfig();
        if (state == Link::Connecting) c.link.Stop(); // the next attempt uses the new port
        c.nextAttempt = 0;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("DLL_SERVER_PORT in um.cfg (18888 by default)");
    ImGui::SameLine();
    if (state == Link::Idle) {
        if (ImGui::Button("Connect", ImVec2(100, 0))) { c.wanted = true; c.nextAttempt = 0; }
    } else if (ImGui::Button("Disconnect", ImVec2(100, 0))) {
        c.wanted = false;
        if (lib.dllAutoConnect) { lib.dllAutoConnect = false; lib.SaveConfig(); } // else it would reconnect at once
        if (connected) c.link.Send("BYE");
        c.link.Stop();
        AddLog(c, "disconnected");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Connect automatically", &lib.dllAutoConnect)) lib.SaveConfig();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep trying to connect while not connected (every 2 seconds): the tab connects as soon as the game starts.");

    if (!connected && c.stats.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("um.dll's DLL server is off by default. To turn it on, set in um.cfg (beside um.dll, in the game's folder):");
        ImGui::Indent();
        ImGui::TextUnformatted("DLL_SERVER_ENABLED=true");
        ImGui::Text("DLL_SERVER_PORT=%d", lib.dllPort);
        ImGui::Unindent();
        ImGui::TextWrapped("then start the game. Only programs on this computer can connect.");
    }

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##dlltabs")) {
        // New tabs go at the end: DLL_TAB in um-multitool.cfg keeps its numbers.
        static const char* const names[] = {"Statistics", "Radar", "Game console", "Commands", "Quests"};
        for (int i = 0; i < 5; ++i) {
            const ImGuiTabItemFlags flags = !c.tabRestored && lib.dllTab == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(names[i], nullptr, flags)) continue;
            if (lib.dllTab != i && c.tabRestored) { lib.dllTab = i; lib.SaveConfig(); }
            if (i == 0) StatisticsTab(c);
            else if (i == 1) RadarTab(c);
            else if (i == 2) GameConsoleTab(c);
            else if (i == 3) CommandsTab(c);
            else QuestsTab(c);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        c.tabRestored = true;
    }
    ImGui::EndChild();
}

} // namespace dllconnect

// ---- `um-multitool dll`: the same connection from a terminal ------------------------------------------

namespace dllconnect {

void PrintCliHelp() {
    std::printf(
        "Usage: um-multitool dll [--port N] [--listen SECONDS] [\"COMMAND\" ...]\n"
        "\n"
        "Sends commands to um.dll's DLL server inside the running game (DLL_SERVER_ENABLED=true in um.cfg)\n"
        "and prints the answers (the ROW lines, then OK or ERR) and the events that arrive meanwhile (not the\n"
        "per-second STATS). Each command is one argument: quote it.\n"
        "Without commands it reads them from the standard input, one a line (interactive).\n"
        "\n"
        "  --port N          the server's port (DLL_SERVER_PORT; default: the multitool's setting, else 18888)\n"
        "  --listen SECONDS  after the commands, keep printing events (breakpoint hits, watched values, log\n"
        "                    lines) this long; 0 = until Ctrl+C\n"
        "  --stats           print the STATS lines too\n"
        "\n"
        "Examples:\n"
        "  um-multitool dll HELP\n"
        "  um-multitool dll \"PEEK game.exe+0x2A1F20 u32\" \"STACK 1234\"\n"
        "  um-multitool dll \"BP set 0 0x1A2B3C0 w 4\" --listen 10\n"
        "Exit status: 0 when every command was answered OK, 1 otherwise, 2 when the server cannot be reached.\n");
}

int RunCli(int argc, char** argv) {
    int port = 0;
    double listen = -1;
    bool stats = false;
    std::vector<std::string> commands;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { PrintCliHelp(); return 0; }
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--listen" && i + 1 < argc) listen = std::atof(argv[++i]);
        else if (a == "--stats") stats = true;
        else commands.push_back(a);
    }
    if (port <= 0) {
        Library lib; // the port the GUI uses
        lib.LoadConfig();
        port = lib.dllPort > 0 ? lib.dllPort : 18888;
    }
    StartSockets();
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == kNoSocket || connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "Cannot connect to 127.0.0.1:%d: is the game running, with DLL_SERVER_ENABLED=true in um.cfg?\n", port);
        return 2;
    }
    std::string pending;
    // The next line from the server; false when it closed or nothing came within timeoutMs (-1: wait).
    auto readLine = [&](std::string& line, int timeoutMs) {
        for (;;) {
            const size_t end = pending.find('\n');
            if (end != std::string::npos) {
                line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, timeoutMs < 0 ? nullptr : &tv) <= 0) return false;
            char buf[8192];
            const int n = static_cast<int>(recv(s, buf, sizeof(buf), 0));
            if (n <= 0) return false;
            pending.append(buf, static_cast<size_t>(n));
        }
    };
    auto show = [&](const std::string& line) {
        if (!stats && line.compare(0, 6, "STATS ") == 0) return;
        if (line.compare(0, 6, "HELLO ") == 0) { std::fprintf(stderr, "%s\n", line.c_str()); return; }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    bool allOk = true;
    auto run = [&](const std::string& command) {
        const std::string text = command + "\n";
        send(s, text.data(), static_cast<int>(text.size()), 0);
        std::string word = command.substr(0, command.find(' '));
        for (char& ch : word) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (word == "BYE") return false;
        std::string line;
        while (readLine(line, -1)) {
            show(line);
            if (line.compare(0, 3, "OK ") == 0 || line == "OK" || line.compare(0, 4, "ERR ") == 0) {
                if (line.compare(0, 4, "ERR ") == 0) allOk = false;
                return true;
            }
        }
        std::fprintf(stderr, "The game closed the connection.\n");
        allOk = false;
        return false;
    };
    bool open = true;
    if (commands.empty() && listen < 0) {
        std::string command;
        while (open && std::getline(std::cin, command)) {
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command.empty()) continue;
            open = run(command);
            std::string line; // events that came meanwhile
            while (open && readLine(line, 0)) show(line);
        }
    } else {
        for (const std::string& command : commands)
            if (open) open = run(command);
    }
    if (open && listen >= 0) {
        const double until = NowSeconds() + listen;
        std::string line;
        while (listen == 0 || NowSeconds() < until) {
            const int wait = listen == 0 ? 1000 : static_cast<int>(std::max(0.0, until - NowSeconds()) * 1000.0);
            if (readLine(line, wait)) show(line);
            else if (wait > 0 && listen == 0) continue;
            else if (NowSeconds() >= until) break;
        }
    }
    if (open) send(s, "BYE\n", 4, 0);
    CloseSocket(s);
    return allOk ? 0 : 1;
}

} // namespace dllconnect
