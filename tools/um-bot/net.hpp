// The game's network protocol (UDP), as far as it is decoded (_cpr/claude-re/net/README.md): every packet starts
// with a type byte.
//   03 u32                get info        -> 05 u32(echo) u32 key, host name\0, game details
//   04 u32 key u32 cookie u32 0   login   -> 06 u32 our cookie (accepted) / 07 u32 u8 reason (rejected)
//   00 u16 seq u16 ack u32 ackbits u16 id u8 len [len] u8 n [n messages]   updates (the game's messages inside);
//                         an empty one (len 0, n 0) is a keep-alive the server accepts
//   01 u32 client id      disconnect
#pragma once

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

    ~Client() { Close(); }

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
        // In the session: an empty update (keep-alive) every 0.3 s, acknowledging the server's last update.
        if (state == State::Accepted && Now() - sentAt_ > 0.3) {
            std::vector<uint8_t> p{0x00};
            Put16(p, seq_++);
            Put16(p, ackSeq_);
            Put32(p, 0);
            Put16(p, 0);
            p.push_back(0);
            p.push_back(0);
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
            if (++updates_ == 1) Log("in the session: the server sends its updates (players, quests); the bot answers with keep-alives");
            break;
        default:
            Log("received a packet of type " + std::to_string(b[0]) + " (" + std::to_string(n) + " bytes): not handled yet");
        }
    }
};

} // namespace net
