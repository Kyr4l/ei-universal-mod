// The game's network protocol (UDP), as far as it is decoded (_cpr/claude-re/net/README.md): every packet starts
// with a type byte.
//   03 u32                get info        -> 05 u32(echo) u32 key, host name\0, game details
//   04 u32 key u32 cookie u32 0   login   -> 06 u32 our cookie (accepted) / 07 u32 u8 reason (rejected)
//   00 u16 seq u16 ack u32 ackbits u16 id u8 len [len] u8 n [n messages]   updates (the game's messages inside);
//                         an empty one (len 0, n 0) is a keep-alive the server accepts
//   01 u32 client id      disconnect
// Game messages inside updates: a flag byte before every 8, then u8 kind, u16 id, payload (ReadMessages). The kind is the message
// class (0x4142F0's factories): 1 player record (the server's), 5 JOIN (name\0 unit\0 u32 u8 u32): the server
// then lists the player, announces "connected" in the chat and shows its face in the lobby.
#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

struct ServerInfo {
    uint32_t key = 0;
    std::string host;           // the hosting player's name
    std::vector<uint8_t> details; // after the name (players, max ...: not all decoded)
};

class Client {
public:
    enum class State { Idle, AskingInfo, LoggingIn, Accepted, Rejected, Failed };
    State state = State::Idle;
    ServerInfo info;
    uint32_t clientId = 0;
    std::string error;
    std::vector<std::string> log;
    bool joined = false;           // the server lists our player

    // The session as the server's messages describe it (_cpr/claude-re/net/README.md: kinds 1, 2, 3, 8, 9, 11).
    struct Player { std::string name, unit; uint32_t state = 0, unitId = 0; };
    std::map<uint16_t, Player> players;          // kind 1, by its object id
    std::map<uint16_t, std::string> quests;      // kind 11: "<quest>.mq"
    std::map<uint16_t, std::string> questStates; // kind 8: "<quest>_1" (available), "_2" (its zone is open)
    std::string chosenQuest;                     // kind 9
    bool ZoneOpen() const {                      // the chosen quest's zone can be entered (the host is in it)
        for (const auto& kv : questStates) if (kv.second == chosenQuest + "_2") return true;
        return false;
    }

    // The JOIN message sent once accepted (from the character's .mp): its name (with the clan tag), its unit name
    // and its u32 after the strings. The u32 1 / u8 1 are the host's own record's values (meaning not known yet).
    void SetCharacter(const std::string& name, const std::string& unit, uint32_t id) {
        name_ = name;
        join_.assign(name.begin(), name.end());
        join_.push_back(0);
        join_.insert(join_.end(), unit.begin(), unit.end());
        join_.push_back(0);
        Put32(join_, 1);
        join_.push_back(1);
        Put32(join_, id);
    }

    ~Client() { Close(); }

    // Hands a packet to the client as if the server had sent it (replaying a capture: tests, research).
    void Feed(const uint8_t* b, int n) { Handle(b, n); }

    // Starts: resolves the server, opens a UDP socket, asks for its info.
    bool Connect(const std::string& host, int port) {
        Close();
        error.clear();
#ifdef _WIN32
        static bool wsa = false;
        if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
#endif
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return Fail("cannot resolve " + host);
        std::memcpy(&server_, res->ai_addr, sizeof server_);
        freeaddrinfo(res);
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kBad) return Fail("cannot open a UDP socket");
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sock_, FIONBIO, &nb);
#else
        fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL, 0) | O_NONBLOCK);
#endif
        cookie_ = std::random_device{}();
        state = State::AskingInfo;
        Send({0x03, 0, 0, 0, 0});
        sentAt_ = Now();
        Log("asked " + host + ":" + std::to_string(port) + " for its info");
        return true;
    }

    // Every frame: reads what arrived, goes on with the handshake, retries, times out.
    void Update() {
        if (sock_ == kBad) return;
        uint8_t buf[2048];
        while (true) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const int n = static_cast<int>(recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n <= 0) break;
            Handle(buf, n);
        }
        // In the session: an update every 0.3 s, acknowledging the server's last one; it carries the JOIN until the
        // server lists us, then it is an empty keep-alive.
        if (state == State::Accepted && Now() - sentAt_ > 0.3) {
            std::vector<uint8_t> p{0x00};
            Put16(p, seq_++);
            Put16(p, ackSeq_);
            Put32(p, 0);
            Put16(p, 0);
            p.push_back(0);
            if (!joined && !join_.empty()) {
                p.push_back(1);    // one message
                p.push_back(0x01); // its flag byte
                p.push_back(5);    // JOIN
                Put16(p, 1);
                p.insert(p.end(), join_.begin(), join_.end());
            } else {
                p.push_back(0);
            }
            Send(p);
            sentAt_ = Now();
        }
        const double waited = Now() - sentAt_;
        if ((state == State::AskingInfo || state == State::LoggingIn) && waited > 3.0) {
            if (++retries_ > 3) { Fail("no answer from the server"); return; }
            if (state == State::AskingInfo) Send({0x03, 0, 0, 0, 0});
            else SendLogin();
            sentAt_ = Now();
        }
    }

    void Disconnect() {
        if (sock_ != kBad && state == State::Accepted) {
            std::vector<uint8_t> p{0x01};
            Put32(p, clientId);
            Send(p);
            Log("disconnected");
        }
        Close();
        state = State::Idle;
    }

private:
#ifdef _WIN32
    static constexpr SOCKET kBad = INVALID_SOCKET;
    SOCKET sock_ = INVALID_SOCKET;
#else
    static constexpr int kBad = -1;
    int sock_ = -1;
#endif
    sockaddr_in server_{};
    uint32_t cookie_ = 0;
    double sentAt_ = 0;
    int retries_ = 0;

    static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    uint16_t seq_ = 1, ackSeq_ = 0;
    int updates_ = 0;
    std::string name_;
    std::vector<uint8_t> join_;
    static void Put16(std::vector<uint8_t>& p, uint16_t v) { p.push_back(static_cast<uint8_t>(v)); p.push_back(static_cast<uint8_t>(v >> 8)); }
    static void Put32(std::vector<uint8_t>& p, uint32_t v) { for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    static uint32_t Get32(const uint8_t* b) { uint32_t v; std::memcpy(&v, b, 4); return v; }
    void Log(const std::string& s) { log.push_back(s); }
    bool Fail(const std::string& why) { error = why; state = State::Failed; Log("failed: " + why); Close(); return false; }
    void Send(const std::vector<uint8_t>& p) {
        sendto(sock_, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0, reinterpret_cast<const sockaddr*>(&server_), sizeof server_);
    }
    void SendLogin() {
        std::vector<uint8_t> p{0x04};
        Put32(p, info.key);
        Put32(p, cookie_);
        Put32(p, 0);
        Send(p);
    }
    void Close() {
        if (sock_ == kBad) return;
#ifdef _WIN32
        closesocket(sock_);
#else
        close(sock_);
#endif
        sock_ = kBad;
    }
    std::set<std::string> seenMessages_; // reliable messages come again until acknowledged: each handled once

    // The reliable messages of a server update: u8 n, then per message (a flag byte before every 8, LSB first:
    // 1 = with its payload, 0 = the object removed) u8 kind, u16 id, the payload. Kinds holding object references
    // (4, 12, 13, 15, 16, 17) also use flag bits: reading stops there (the rest of the packet is not read).
    void ReadMessages(const uint8_t* b, int n) {
        if (n < 12) return;
        int o = 11;
        const int unreliable = b[o++];
        o += unreliable;
        if (o >= n) return;
        const int count = b[o++];
        uint8_t flags = 0;
        for (int i = 0; i < count && o < n; ++i) {
            if (i % 8 == 0) flags = b[o++];
            const bool payload = (flags >> (i % 8)) & 1;
            if (o + 3 > n) return;
            const int kind = b[o];
            uint16_t id;
            std::memcpy(&id, b + o + 1, 2);
            const int start = o;
            o += 3;
            auto str = [&]() { std::string v; while (o < n && b[o]) v += static_cast<char>(b[o++]); ++o; return v; };
            auto u32 = [&]() { uint32_t v = 0; if (o + 4 <= n) std::memcpy(&v, b + o, 4); o += 4; return v; };
            auto u16 = [&]() { uint16_t v = 0; if (o + 2 <= n) std::memcpy(&v, b + o, 2); o += 2; return v; };
            std::string text;
            if (!payload) { // removed
                if (kind == 1) players.erase(id);
                else if (kind == 8) questStates.erase(id);
                else if (kind == 11) quests.erase(id);
                else if (kind == 9) chosenQuest.clear();
                continue;
            }
            switch (kind) {
            case 1: { Player p; p.name = str(); p.unit = str(); p.state = u32(); o += 1; p.unitId = u32(); u32(); players[id] = p; break; }
            case 2: { u32(); text = str(); break; }           // a system line ("Quest accepted: ...")
            case 3: { const std::string a = str(); text = a + " " + str(); break; } // a chat line (a printed pair)
            case 5: { str(); str(); u32(); o += 1; u32(); break; }
            case 6: case 9: text = str(); if (kind == 9) chosenQuest = text; break;
            case 7: { u32(); text = str(); break; }
            case 8: questStates[id] = str(); break;
            case 10: { str(); u32(); u32(); break; }          // a quest variable
            case 11: { u16(); u32(); quests[id] = str(); break; }
            case 14: { u32(); u32(); text = str(); break; }
            default:
                Log("server message kind " + std::to_string(kind) + ": not read (the rest of this update skipped)");
                return;
            }
            const std::string key(reinterpret_cast<const char*>(b + start), static_cast<size_t>(std::min(o, n) - start));
            if (!seenMessages_.insert(key).second) continue;
            if (kind == 2 || kind == 3 || kind == 6) Log("server: " + text);
            else if (kind == 9) Log("the host chose the quest " + text);
            else if (kind == 8 && questStates[id] == chosenQuest + "_2") Log("the zone of " + chosenQuest + " is open: the host is in it");
            else if (kind == 1) Log("player " + players[id].name + ": state " + std::to_string(players[id].state));
        }
    }

    void Handle(const uint8_t* b, int n) {
        switch (b[0]) {
        case 0x05: // info
            if (n < 9 || state != State::AskingInfo) return;
            info.key = Get32(b + 5);
            {
                int e = 9;
                while (e < n && b[e]) ++e;
                info.host.assign(reinterpret_cast<const char*>(b + 9), static_cast<size_t>(e - 9));
                info.details.assign(b + std::min(e + 1, n), b + n);
            }
            Log("server \"" + info.host + "\" answered; logging in");
            state = State::LoggingIn;
            retries_ = 0;
            SendLogin();
            sentAt_ = Now();
            break;
        case 0x06: // accepted
            if (n < 5) return;
            clientId = Get32(b + 1);
            state = State::Accepted;
            Log("accepted (the server echoed our cookie)");
            break;
        case 0x07: // rejected
            state = State::Rejected;
            error = "rejected by the server (reason " + std::to_string(n >= 6 ? b[5] : -1) + ")";
            Log(error);
            Close();
            break;
        case 0x00: // the server's update: acknowledged in the next keep-alive (its content is not decoded yet)
            if (n >= 3) std::memcpy(&ackSeq_, b + 1, 2);
            if (++updates_ == 1) Log("in the session: the server sends its updates (players, quests)");
            if (!joined && !name_.empty() && std::search(b, b + n, name_.begin(), name_.end()) != b + n) {
                joined = true;
                Log("joined: the server lists \"" + name_ + "\" (chat: connected, face in the lobby)");
            }
            ReadMessages(b, n);
            break;
        default:
            Log("received a packet of type " + std::to_string(b[0]) + " (" + std::to_string(n) + " bytes): not handled yet");
        }
    }
};

} // namespace net
