// SPDX-License-Identifier: MIT
#include "mcp/bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "core/json.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CC_INVALID_SOCKET INVALID_SOCKET
#define cc_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CC_INVALID_SOCKET (-1)
#define cc_close_socket ::close
#endif

namespace cc::mcp {
namespace {

struct Endpoint {
    std::string host = "127.0.0.1";
    std::string port = "8765";
    std::string path = "/mcp";
    bool ok = false;
};

// Only http:// on a host and port - this talks to a loopback service, not the
// web, so there is no TLS and no redirect handling to get wrong.
Endpoint parse_url(const std::string& url) {
    Endpoint e;
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) return e;

    const std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) e.path = rest.substr(slash);
    if (e.path.empty()) e.path = "/";

    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
        e.host = authority;
        e.port = "80";
    } else {
        e.host = authority.substr(0, colon);
        e.port = authority.substr(colon + 1);
    }
    e.ok = !e.host.empty() && !e.port.empty();
    return e;
}

socket_t connect_to(const Endpoint& e) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (::getaddrinfo(e.host.c_str(), e.port.c_str(), &hints, &result) != 0) {
        return CC_INVALID_SOCKET;
    }

    socket_t fd = CC_INVALID_SOCKET;
    for (addrinfo* a = result; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == CC_INVALID_SOCKET) continue;
        if (::connect(fd, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0) break;
        cc_close_socket(fd);
        fd = CC_INVALID_SOCKET;
    }
    ::freeaddrinfo(result);

    if (fd != CC_INVALID_SOCKET) {
        // Each request is one small write; waiting for more data to coalesce
        // adds latency to every single tool call.
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                     sizeof(yes));
    }
    return fd;
}

bool send_all(socket_t fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int n = ::send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads one HTTP response and returns its body. `status` gets the code.
bool read_response(socket_t fd, std::string* body, int* status) {
    std::string buf;
    char chunk[8192];
    std::size_t header_end = std::string::npos;
    long long content_length = -1;

    for (;;) {
#if defined(_WIN32)
        const int n = ::recv(fd, chunk, sizeof(chunk), 0);
#else
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) break;
        buf.append(chunk, static_cast<std::size_t>(n));

        if (header_end == std::string::npos) {
            header_end = buf.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string head = buf.substr(0, header_end);
                if (head.size() > 12) *status = std::atoi(head.c_str() + 9);
                const auto pos = lower(head).find("content-length:");
                if (pos != std::string::npos) {
                    content_length = std::strtoll(head.c_str() + pos + 15, nullptr, 10);
                }
            }
        }
        if (header_end != std::string::npos && content_length >= 0 &&
            buf.size() >= header_end + 4 + static_cast<std::size_t>(content_length)) {
            break;
        }
        if (buf.size() > 64u * 1024 * 1024) return false;
    }

    if (header_end == std::string::npos) return false;
    *body = buf.substr(header_end + 4);
    if (content_length >= 0 && body->size() > static_cast<std::size_t>(content_length)) {
        body->resize(static_cast<std::size_t>(content_length));
    }
    return true;
}

// A transport failure still has to come back as JSON-RPC, or the client sees
// a dead pipe and reports nothing a person can act on.
std::string rpc_error(const json::Value& id, int code, const std::string& message,
                      const std::string& remedy) {
    json::Value err = json::Value::object();
    err.set("code", code);
    err.set("message", remedy.empty() ? message : message + ". " + remedy);

    json::Value out = json::Value::object();
    out.set("jsonrpc", "2.0");
    out.set("id", id);
    out.set("error", err);
    return out.dump();
}

}  // namespace

int run_bridge(const std::string& url, const std::string& token) {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "computer-control: WSAStartup failed\n";
        return 1;
    }
#endif
    const Endpoint endpoint = parse_url(url);
    if (!endpoint.ok) {
        std::cerr << "computer-control: not a usable http:// url: " << url << "\n";
        return 2;
    }

    // Everything the client sends goes out on stdout; anything we say for a
    // human goes to stderr, or it corrupts the protocol stream.
    std::ios::sync_with_stdio(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        json::ParseError pe;
        const json::Value message = json::parse(line, &pe);
        const json::Value id = pe.ok ? message["id"] : json::Value();
        // A notification gets no reply, so a failure forwarding one must not
        // invent a response the client never asked for.
        const bool is_notification = pe.ok && message["id"].is_null();

        const socket_t fd = connect_to(endpoint);
        if (fd == CC_INVALID_SOCKET) {
            if (!is_notification) {
                std::cout << rpc_error(id, -32603, "cannot reach the computer-control service",
                                       "Check it with `computer-control-mcp setup --status`, or "
                                       "start it with `computer-control-mcp setup --shared`.")
                          << "\n"
                          << std::flush;
            }
            continue;
        }

        std::string request = "POST " + endpoint.path + " HTTP/1.1\r\n";
        request += "Host: " + endpoint.host + ":" + endpoint.port + "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Accept: application/json\r\n";
        if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
        request += "Content-Length: " + std::to_string(line.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += line;

        std::string body;
        int status = 0;
        const bool sent = send_all(fd, request) && read_response(fd, &body, &status);
        cc_close_socket(fd);

        if (is_notification) continue;

        if (!sent) {
            std::cout << rpc_error(id, -32603, "the computer-control service closed the connection",
                                   "`computer-control-mcp setup --status` reports its state.")
                      << "\n"
                      << std::flush;
            continue;
        }
        if (status == 401) {
            std::cout << rpc_error(id, -32603, "the computer-control service rejected the token",
                                   "Re-run `computer-control-mcp setup` to rewrite this client's "
                                   "credentials.")
                      << "\n"
                      << std::flush;
            continue;
        }
        if (body.empty()) {
            std::cout << rpc_error(id, -32603, "the computer-control service returned nothing", "")
                      << "\n"
                      << std::flush;
            continue;
        }
        std::cout << body << "\n" << std::flush;
    }
    return 0;
}

}  // namespace cc::mcp
