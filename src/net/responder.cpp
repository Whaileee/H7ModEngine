// TCP challenge responder for the lobby's host verification (protocol in responder.h and docs/ONLINE.md).
//
// The game itself only listens on UDP 27015 (measured 2026-11-05), so TCP 27015 is free for this. The lobby's
// Worker connects, sends one challenge line and expects the HMAC of its nonce under the key derived from the
// game's token; nothing else is ever sent, and the key never leaves this process.
#include "responder.h"
#include "crypto.h"
#include "../core/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace h7::net {

static constexpr int kLineLen = 43;      // "H7LOBBY 1 " + 32 hex + "\n"
static constexpr int kReplyLen = 65;     // 64 hex + "\n"
static constexpr int kPrefixLen = 10;
static constexpr int kNonceLen = 32;
static constexpr int kTimeoutMs = 2000;  // per connection, whole exchange
static const char kPrefix[] = "H7LOBBY 1 ";

static std::mutex g_ctl;                 // start / stop
static std::mutex g_keyMx;
static std::string g_key;
static std::mutex g_clientMx;
static SOCKET g_client = INVALID_SOCKET; // connection being served, so Stop can cut it short
static SOCKET g_listen = INVALID_SOCKET;
static std::thread* g_thread = nullptr;  // never destroyed at exit (a joinable std::thread would terminate)
static int g_port = 0;
static std::atomic<bool> g_stop{false};
static bool g_loopback = false;

// Failed connections are summarised at most once a minute (listener thread only).
static int g_failures = 0;
static ULONGLONG g_lastFailLog = 0;

static void NoteFailure(const char* why, const char* peer) {
    ++g_failures;
    ULONGLONG now = GetTickCount64();
    if (g_lastFailLog != 0 && now - g_lastFailLog < 60000) return;
    Log("Responder: ignored %d connection(s) since the last report (last: %s from %s)", g_failures, why, peer);
    g_failures = 0;
    g_lastFailLog = now;
}

static bool WaitReadable(SOCKET s, ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    int ms = (int)(deadline - now);
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select(0, &r, nullptr, nullptr, &tv) == 1;
}

static bool IsLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

static bool ValidChallenge(const char* line) {
    if (memcmp(line, kPrefix, kPrefixLen) != 0 || line[kLineLen - 1] != '\n') return false;
    for (int i = kPrefixLen; i < kPrefixLen + kNonceLen; ++i)
        if (!IsLowerHex(line[i])) return false;
    return true;
}

// One exchange; the caller closes the socket. Returns nullptr when answered, else why not.
static const char* Serve(SOCKET c) {
    DWORD to = kTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof to);
    ULONGLONG deadline = GetTickCount64() + kTimeoutMs;

    char line[kLineLen];
    int got = 0;
    while (got < kLineLen) { // exactly 43 bytes: never read past the line
        if (!WaitReadable(c, deadline)) return "timeout";
        int n = recv(c, line + got, kLineLen - got, 0);
        if (n == 0) return "closed early";
        if (n < 0) return "receive error";
        got += n;
    }
    if (!ValidChallenge(line)) return "malformed challenge";

    std::string key;
    {
        std::lock_guard lock(g_keyMx);
        key = g_key;
    }
    std::string reply = HmacSha256Hex(key, std::string(line + kPrefixLen, kNonceLen)) + "\n";
    if ((int)reply.size() != kReplyLen) return "hashing failed";
    for (int sent = 0; sent < kReplyLen;) {
        int n = send(c, reply.data() + sent, kReplyLen - sent, 0);
        if (n <= 0) return "send failed";
        sent += n;
    }
    shutdown(c, SD_SEND);
    // Let the peer close first (bounded), so closing never resets the connection under the reply.
    char sink[64];
    ULONGLONG until = GetTickCount64() + 500;
    while (WaitReadable(c, until) && recv(c, sink, sizeof sink, 0) > 0) {}
    return nullptr;
}

static void ListenLoop(SOCKET ls) {
    for (;;) {
        sockaddr_in from{};
        int len = sizeof from;
        SOCKET c = accept(ls, (sockaddr*)&from, &len);
        if (g_stop) {
            if (c != INVALID_SOCKET) closesocket(c);
            return;
        }
        if (c == INVALID_SOCKET) { // e.g. a connection reset before accept; keep listening
            Sleep(50);
            continue;
        }
        char peer[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof peer);
        {
            std::lock_guard lock(g_clientMx);
            g_client = c;
        }
        const char* why = Serve(c);
        {
            std::lock_guard lock(g_clientMx);
            g_client = INVALID_SOCKET;
        }
        closesocket(c);
        if (why) NoteFailure(why, peer);
        else Log("Responder: answered a lobby challenge from %s", peer);
    }
}

static void StopLocked() {
    if (!g_thread) return;
    g_stop = true;
    closesocket(g_listen); // wakes accept()
    g_listen = INVALID_SOCKET;
    {
        std::lock_guard lock(g_clientMx);
        if (g_client != INVALID_SOCKET) shutdown(g_client, SD_BOTH); // wakes select(); the thread closes it
    }
    g_thread->join();
    delete g_thread;
    g_thread = nullptr;
    Log("Responder: stopped listening on TCP %d", g_port);
    g_port = 0;
}

std::string ResponderStart(int port, const std::string& key) {
    std::lock_guard ctl(g_ctl);
    if (g_thread && port == g_port) {
        std::lock_guard lock(g_keyMx);
        g_key = key;
        return {};
    }
    StopLocked();
    static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) return "Winsock is not available";
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "could not create a socket (error " + std::to_string(WSAGetLastError()) + ")";
    BOOL on = TRUE; // nobody else may bind the port while we hold it
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(g_loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        if (e == WSAEADDRINUSE || e == WSAEACCES) return "TCP port " + std::to_string(port) + " is already in use";
        return "could not listen on TCP port " + std::to_string(port) + " (error " + std::to_string(e) + ")";
    }
    {
        std::lock_guard lock(g_keyMx);
        g_key = key;
    }
    g_listen = s;
    g_port = port;
    g_stop = false;
    g_thread = new std::thread(ListenLoop, s);
    Log("Responder: listening for lobby challenges on TCP %d%s", port, g_loopback ? " (loopback only)" : "");
    return {};
}

void ResponderStop() {
    std::lock_guard ctl(g_ctl);
    StopLocked();
    std::lock_guard lock(g_keyMx);
    g_key.clear();
}

bool ResponderRunning() {
    std::lock_guard ctl(g_ctl);
    return g_thread != nullptr;
}

void ResponderLoopbackOnly(bool on) { g_loopback = on; }

} // namespace h7::net
