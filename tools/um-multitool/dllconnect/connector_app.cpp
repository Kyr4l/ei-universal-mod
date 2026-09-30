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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
    char command[512] = "";                             // the Console's command line
    std::vector<std::string> history;                   // and the commands sent
    int historyAt = -1;
    bool scrollToEnd = false;
    explicit Context(Library& l) : lib(l) { wanted = lib.dllAutoConnect; }
};

namespace {

void AddLog(Context& c, const std::string& text) {
    c.log.push_back(TimeStamp() + "  " + text);
    while (c.log.size() > 20000) c.log.pop_front();
}

// "WORD key=value key=value ..." -> the pairs.
std::map<std::string, std::string> Pairs(const std::string& line) {
    std::map<std::string, std::string> out;
    size_t pos = line.find(' ');
    while (pos != std::string::npos) {
        const size_t start = pos + 1;
        pos = line.find(' ', start);
        const std::string word = line.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        const size_t eq = word.find('=');
        if (eq != std::string::npos) out[word.substr(0, eq)] = word.substr(eq + 1);
    }
    return out;
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
    c.link.Send(command);
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
void ConsoleTab(Context& c) {
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

Context* Create(Library& lib) { return new Context(lib); }

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
    if (connected && !c.wasConnected) { AddLog(c, "connected to 127.0.0.1:" + std::to_string(c.lib.dllPort)); c.hello.clear(); }
    for (const std::string& line : lines) Handle(c, line);
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
        static const char* const names[] = {"Statistics", "Console"};
        for (int i = 0; i < 2; ++i) {
            const ImGuiTabItemFlags flags = !c.tabRestored && lib.dllTab == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(names[i], nullptr, flags)) continue;
            if (lib.dllTab != i && c.tabRestored) { lib.dllTab = i; lib.SaveConfig(); }
            if (i == 0) StatisticsTab(c);
            else ConsoleTab(c);
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
