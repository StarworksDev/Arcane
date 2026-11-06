#pragma once
#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include "RemoteTestSupport.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>

namespace RemoteTest {

// A blocking TCP socket connected to 127.0.0.1:port. REQUIREs success.
inline SocketType ConnectLoopback(std::uint16_t port) {
    REQUIRE(Arcane::InitSockets());
    const SocketType sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return sock;
}

// Sends every byte. REQUIREs one full send (requests here are small).
inline void SendAll(SocketType sock, const std::string& bytes) {
    REQUIRE(static_cast<int>(::send(sock, bytes.data(), static_cast<int>(bytes.size()), 0)) ==
            static_cast<int>(bytes.size()));
}

// Reads until `marker` is in the accumulated bytes, the peer closes, or
// `budget` passes. Each recv waits at most 100 ms, so the loop never overruns
// the budget by more than that. Returns everything read.
inline std::string RecvUntil(SocketType sock, const std::string& marker, std::chrono::milliseconds budget) {
    Arcane::SetSocketTimeout(sock, 100);
    std::string raw;
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (raw.find(marker) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        const int n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (n > 0) {
            raw.append(buf, buf + n);
            continue;
        }
        if (n == 0 || !Arcane::IsSocketTimeoutError())
            break; // closed, or a hard error
    }
    return raw;
}

// One request/response on a fresh connection. `extraHeaders` is zero or more
// complete "Name: value\r\n" lines. Returns the raw HTTP response.
inline std::string HttpExchangeHost(std::uint16_t port, const std::string& token,
                                    const std::string& hostHeader, const std::string& extraHeaders,
                                    const std::string& requestLine, const std::string& body = {}) {
    const SocketType sock = ConnectLoopback(port);
    Arcane::SetSocketTimeout(sock, 5000); // a listener that never answers fails the case, not the suite
    std::string req = requestLine + "\r\nHost: " + hostHeader +
        "\r\nAuthorization: Bearer " + token + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += extraHeaders;
    req += "\r\n";
    req += body;
    SendAll(sock, req);
    std::string raw;
    char buf[4096];
    for (;;) {
        const int n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (n <= 0) break;
        raw.append(buf, buf + n);
        const auto hdr = raw.find("\r\n\r\n");
        if (hdr == std::string::npos) continue;
        const auto key = raw.find("Content-Length:");
        if (key == std::string::npos || key > hdr) break;
        const int len = std::atoi(raw.c_str() + key + 15);
        if (static_cast<int>(raw.size() - (hdr + 4)) >= len) break;
    }
    CloseSocket(sock);
    return raw;
}

inline std::string HttpExchange(std::uint16_t port, const std::string& token,
                                const std::string& extraHeaders, const std::string& requestLine,
                                const std::string& body = {}) {
    return HttpExchangeHost(port, token, "127.0.0.1:" + std::to_string(port),
                            extraHeaders, requestLine, body);
}

// A loopback port with no listener: bound ephemerally, then closed, so a
// connect to it is refused.
inline std::uint16_t UnusedLoopbackPort() {
    REQUIRE(Arcane::InitSockets());
    const SocketType sock = Arcane::CreateListenSocket(0, "127.0.0.1");
    REQUIRE(sock != INVALID_SOCK);
    sockaddr_in addr{};
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    REQUIRE(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    CloseSocket(sock);
    return ntohs(addr.sin_port);
}

// A one-shot fake host for client transport tests: listens on an ephemeral
// 127.0.0.1 port, accepts ONE connection, reads the whole request (headers +
// Content-Length body), writes `reply` verbatim, then closes -- or, with
// holdOpen, keeps the connection open and silent until destruction (a host
// that never answers). Every wait polls with a short timeout against a stop
// flag, so destruction never blocks on accept or recv.
class CannedServer {
public:
    CannedServer(std::string reply, bool holdOpen = false)
        : m_reply(std::move(reply)), m_holdOpen(holdOpen) {
        REQUIRE(Arcane::InitSockets());
        m_listen = Arcane::CreateListenSocket(0, "127.0.0.1");
        REQUIRE(m_listen != INVALID_SOCK);
        sockaddr_in addr{};
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        REQUIRE(::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { Serve(); });
    }
    ~CannedServer() {
        m_stop.store(true);
        m_thread.join();
        CloseSocket(m_listen);
    }
    CannedServer(const CannedServer&) = delete;
    CannedServer& operator=(const CannedServer&) = delete;
    std::uint16_t Port() const { return m_port; }

private:
    bool WaitReadable(SocketType sock) {
        while (!m_stop.load()) {
            Arcane::PollInterest interest;
            interest.socket = sock;
            interest.wantRead = true;
            if (Arcane::PollSockets(std::span<Arcane::PollInterest>(&interest, 1), 20) > 0)
                return true;
        }
        return false;
    }
    void Serve() {
        if (!WaitReadable(m_listen))
            return;
        const SocketType conn = ::accept(m_listen, nullptr, nullptr);
        if (conn == INVALID_SOCK)
            return;
        std::string raw;
        char buf[4096];
        for (;;) {
            const auto hdr = raw.find("\r\n\r\n");
            if (hdr != std::string::npos) {
                const auto key = raw.find("Content-Length:");
                const std::size_t len = key == std::string::npos || key > hdr
                    ? 0 : static_cast<std::size_t>(std::atoi(raw.c_str() + key + 15));
                if (raw.size() - (hdr + 4) >= len)
                    break;
            }
            if (!WaitReadable(conn))
                break;
            const int n = static_cast<int>(::recv(conn, buf, sizeof(buf), 0));
            if (n <= 0)
                break;
            raw.append(buf, buf + n);
        }
        std::size_t sent = 0;
        while (sent < m_reply.size()) {
            const int n = static_cast<int>(::send(conn, m_reply.data() + sent,
                                                  static_cast<int>(m_reply.size() - sent), 0));
            if (n <= 0)
                break;
            sent += static_cast<std::size_t>(n);
        }
        while (m_holdOpen && !m_stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CloseSocket(conn);
    }

    std::string m_reply;
    bool m_holdOpen = false;
    SocketType m_listen = INVALID_SOCK;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

}  // namespace RemoteTest
#endif
