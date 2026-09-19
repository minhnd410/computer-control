// SPDX-License-Identifier: MIT
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>

#include "core/json.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CC_INVALID_SOCKET INVALID_SOCKET
#define cc_close_socket closesocket
#else
#include <arpa/inet.h>
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

// MCP over stdio is newline-delimited JSON. The fatal mistake here is letting
// anything else reach stdout: a stray printf from a library corrupts the
// stream and the client drops the connection with an opaque parse error. The
// server therefore redirects C++ logging to stderr and never uses std::cout
// except through this class.
class StdioTransport final : public Transport {
public:
    StdioTransport() {
#if defined(_WIN32)
        // Without binary mode Windows rewrites \n as \r\n on the way out,
        // which breaks the framing.
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        std::ios::sync_with_stdio(false);
    }

    bool read(std::string& out) override {
        out.clear();
        if (!std::getline(std::cin, out)) return false;
        // Tolerate CRLF from clients that write Windows line endings.
        if (!out.empty() && out.back() == '\r') out.pop_back();
        if (out.empty()) return read(out);  // skip keepalive blank lines
        return true;
    }

    bool write(const std::string& msg) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::fwrite(msg.data(), 1, msg.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return true;
    }

    void close() override {}

private:
    std::mutex mu_;
};

// A deliberately small HTTP transport: one request, one JSON-RPC message, one
// response. No SSE, no session resumption. Anything more belongs behind a
// real reverse proxy, and pretending otherwise would invite exposing a desktop
// automation server to a network it should never be on.
//
// 2026-07-28 dropped the session id and added `Mcp-Method` and `Mcp-Name`, so
// that a gateway can route and authorise a call without parsing the body. The
// obvious attack is to declare a harmless method in the headers and smuggle a
// different one in the body, which is what HeaderMismatch (-32020) exists to
// stop. The headers are validated when present rather than required: a header
// that is absent cannot contradict anything, and requiring them would break
// every pre-2026 HTTP client for no gain on a loopback socket.
class HttpTransport final : public Transport {
public:
    HttpTransport(socket_t listener, std::string token)
        : listener_(listener), token_(std::move(token)) {}

    ~HttpTransport() override { close(); }

    bool read(std::string& out) override {
        for (;;) {
            if (client_ != CC_INVALID_SOCKET) {
                cc_close_socket(client_);
                client_ = CC_INVALID_SOCKET;
            }
            sockaddr_in addr{};
#if defined(_WIN32)
            int len = sizeof(addr);
#else
            socklen_t len = sizeof(addr);
#endif
            client_ = ::accept(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (client_ == CC_INVALID_SOCKET) return false;
            replied_ = false;

            // Reject anything that is not loopback unless the operator bound
            // elsewhere on purpose; a token is then mandatory.
            std::string request;
            if (!read_request(request)) continue;

            std::string body;
            if (!parse(request, body)) {
                // parse() answers for itself when it rejects a request (401,
                // or a JSON-RPC error for a bad header). Only the cases it
                // leaves unanswered get the generic 400 - sending a second
                // response on the same socket corrupts the exchange.
                if (!replied_) respond(400, "{\"error\":\"malformed request\"}");
                continue;
            }
            out = body;
            return true;
        }
    }

    bool write(const std::string& msg) override {
        respond(msg.empty() ? 202 : 200, msg);
        return true;
    }

    void close() override {
        if (client_ != CC_INVALID_SOCKET) {
            cc_close_socket(client_);
            client_ = CC_INVALID_SOCKET;
        }
        if (listener_ != CC_INVALID_SOCKET) {
            cc_close_socket(listener_);
            listener_ = CC_INVALID_SOCKET;
        }
    }

private:
    enum class ChunkedRequestState { Incomplete, Complete, Malformed };

    static ChunkedRequestState decode_chunked_request(const std::string& request,
                                                       std::size_t body_start,
                                                       std::string& body) {
        constexpr std::size_t kMaxBody = 32u * 1024 * 1024;
        body.clear();
        std::size_t cursor = body_start;

        for (;;) {
            const auto line_end = request.find("\r\n", cursor);
            if (line_end == std::string::npos) return ChunkedRequestState::Incomplete;

            const auto extension = request.find(';', cursor);
            const std::size_t size_end =
                extension != std::string::npos && extension < line_end ? extension : line_end;
            if (size_end == cursor) return ChunkedRequestState::Malformed;

            std::size_t chunk_size = 0;
            for (std::size_t i = cursor; i < size_end; ++i) {
                const unsigned char c = static_cast<unsigned char>(request[i]);
                unsigned digit;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                else
                    return ChunkedRequestState::Malformed;

                if (chunk_size > (std::numeric_limits<std::size_t>::max() - digit) / 16)
                    return ChunkedRequestState::Malformed;
                chunk_size = chunk_size * 16 + digit;
            }

            cursor = line_end + 2;
            if (chunk_size == 0) {
                // A zero-sized chunk is followed by an optional trailer block
                // and a final blank line.
                if (request.compare(cursor, 2, "\r\n") == 0) {
                    return ChunkedRequestState::Complete;
                }
                return request.find("\r\n\r\n", cursor) == std::string::npos
                           ? ChunkedRequestState::Incomplete
                           : ChunkedRequestState::Complete;
            }

            if (chunk_size > kMaxBody || body.size() > kMaxBody - chunk_size) {
                return ChunkedRequestState::Malformed;
            }
            if (cursor > request.size() || request.size() - cursor < chunk_size + 2) {
                return ChunkedRequestState::Incomplete;
            }

            body.append(request, cursor, chunk_size);
            cursor += chunk_size;
            if (request.compare(cursor, 2, "\r\n") != 0) {
                return ChunkedRequestState::Malformed;
            }
            cursor += 2;
        }
    }

    bool read_request(std::string& out) {
        char buf[8192];
        std::size_t header_end = std::string::npos;
        long long content_length = -1;
        bool chunked = false;

        for (;;) {
#if defined(_WIN32)
            const int n = ::recv(client_, buf, sizeof(buf), 0);
#else
            const ssize_t n = ::recv(client_, buf, sizeof(buf), 0);
#endif
            if (n <= 0) return false;
            out.append(buf, static_cast<std::size_t>(n));

            if (header_end == std::string::npos) {
                header_end = out.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::string headers = out.substr(0, header_end);
                    const std::string lowered = lower(headers);
                    const std::string transfer_encoding =
                        lower(header_value(headers, lowered, "transfer-encoding"));
                    std::size_t token_start = 0;
                    while (token_start < transfer_encoding.size()) {
                        const auto comma = transfer_encoding.find(',', token_start);
                        const auto token_end = comma == std::string::npos
                                                   ? transfer_encoding.size()
                                                   : comma;
                        std::string token =
                            transfer_encoding.substr(token_start, token_end - token_start);
                        const auto first = token.find_first_not_of(" \t");
                        if (first != std::string::npos) {
                            const auto last = token.find_last_not_of(" \t");
                            if (token.substr(first, last - first + 1) == "chunked") {
                                chunked = true;
                                break;
                            }
                        }
                        if (comma == std::string::npos) break;
                        token_start = comma + 1;
                    }

                    if (!chunked) {
                        const std::string length = header_value(headers, lowered, "content-length");
                        if (!length.empty()) {
                            content_length = std::strtoll(length.c_str(), nullptr, 10);
                        }
                    }
                }
            }
            if (header_end != std::string::npos) {
                if (chunked) {
                    std::string decoded;
                    const auto state = decode_chunked_request(out, header_end + 4, decoded);
                    if (state == ChunkedRequestState::Complete) {
                        out.resize(header_end + 4);
                        out += decoded;
                        return true;
                    }
                    if (state == ChunkedRequestState::Malformed) return false;
                } else if (content_length < 0) {
                    return true;
                } else if (out.size() >= header_end + 4 + static_cast<std::size_t>(content_length)) {
                    return true;
                }
            }
            if (out.size() > 32u * 1024 * 1024) return false;  // refuse absurd bodies
        }
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool parse(const std::string& request, std::string& body) {
        const auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;
        const std::string headers = request.substr(0, header_end);

        if (!token_.empty()) {
            const std::string lowered = lower(headers);
            const auto pos = lowered.find("authorization: bearer ");
            bool authorized = false;
            if (pos != std::string::npos) {
                const auto start = pos + 22;
                const auto end = headers.find("\r\n", start);
                const std::string got = headers.substr(start, end - start);
                // Constant-time-ish compare; the token is short and this is
                // not a high-value secret, but a length-leaking early return
                // is trivially avoidable.
                authorized = (got.size() == token_.size());
                unsigned diff = 0;
                for (std::size_t i = 0; i < token_.size() && i < got.size(); ++i) {
                    diff |= static_cast<unsigned>(got[i] ^ token_[i]);
                }
                authorized = authorized && (diff == 0);
            }
            if (!authorized) {
                respond(401, "{\"error\":\"missing or invalid bearer token\"}");
                return false;
            }
        }

        // Streamable HTTP clients open a GET to listen for server-initiated
        // messages. This transport has no stream to offer, and the spec's
        // answer for that is 405 - not the generic 400 an empty body would
        // otherwise produce, which reads as "your request was malformed" and
        // sends a client author looking for a fault that is not theirs.
        const std::string method = headers.substr(0, headers.find(' '));
        if (method == "GET" || method == "DELETE") {
            respond(405,
                    "{\"error\":\"this server offers no event stream; POST JSON-RPC "
                    "to this endpoint instead\"}");
            return false;
        }

        body = request.substr(header_end + 4);
        if (body.empty()) return false;
        return check_routing_headers(headers, body);
    }

    // Returns the value of `name` (given lowercased) with surrounding
    // whitespace removed, or empty if the header is absent.
    static std::string header_value(const std::string& headers, const std::string& lowered,
                                    const char* name) {
        std::string needle = "\r\n";
        needle += name;
        needle += ":";
        // The first header line has no preceding CRLF, so try it separately.
        std::size_t start;
        const std::string first = std::string(name) + ":";
        if (lowered.rfind(first, 0) == 0) {
            start = first.size();
        } else {
            const auto pos = lowered.find(needle);
            if (pos == std::string::npos) return {};
            start = pos + needle.size();
        }
        auto end = headers.find("\r\n", start);
        if (end == std::string::npos) end = headers.size();
        std::string value = headers.substr(start, end - start);
        const auto first_ch = value.find_first_not_of(" \t");
        if (first_ch == std::string::npos) return {};
        const auto last_ch = value.find_last_not_of(" \t");
        return value.substr(first_ch, last_ch - first_ch + 1);
    }

    // Sends a JSON-RPC error carrying the id from the body, so the client can
    // match it to the request it sent rather than failing the whole batch.
    void respond_rpc_error(int status, const json::Value& id, json::Value error) {
        json::Value out = json::Value::object();
        out.set("jsonrpc", "2.0");
        out.set("id", id);
        out.set("error", std::move(error));
        respond(status, out.dump());
    }

    // A browser can be made to POST to a loopback port by any page the user
    // visits (DNS rebinding, or just a form). It always sends Origin; a
    // non-browser client normally sends none. So an absent Origin is fine and
    // a cross-origin one is refused - required since MCP 2025-11-25, and the
    // only thing standing between a web page and a server that drives the
    // desktop.
    bool origin_is_allowed(const std::string& headers, const std::string& lowered) {
        const std::string origin = header_value(headers, lowered, "origin");
        if (origin.empty()) return true;
        const std::string o = lower(origin);
        static const char* const kAllowed[] = {"http://localhost", "https://localhost",
                                               "http://127.0.0.1", "https://127.0.0.1",
                                               "http://[::1]",     "https://[::1]"};
        for (const char* prefix : kAllowed) {
            const std::size_t n = std::strlen(prefix);
            if (o.compare(0, n, prefix) != 0) continue;
            // Match the whole host, so http://localhost.evil.com is not
            // mistaken for http://localhost by a prefix compare.
            if (o.size() == n || o[n] == ':') return true;
        }
        return false;
    }

    bool check_routing_headers(const std::string& headers, const std::string& body) {
        const std::string lowered = lower(headers);

        if (!origin_is_allowed(headers, lowered)) {
            respond(403, "{\"error\":\"origin not allowed\"}");
            return false;
        }

        const std::string version = header_value(headers, lowered, "mcp-protocol-version");
        json::ParseError pe;
        const json::Value msg = json::parse(body, &pe);
        // A body this transport cannot parse is the server's problem to
        // report, not the transport's: pass it through so the reply is a
        // proper -32700 rather than an HTTP 400 with no id.
        if (!pe.ok || !msg.is_object()) return true;
        const json::Value& id = msg["id"];

        if (!version.empty() && !is_supported_version(version)) {
            respond_rpc_error(400, id, unsupported_version_error(version));
            return false;
        }

        const std::string header_method = header_value(headers, lowered, "mcp-method");
        const std::string body_method = msg["method"].as_string();
        if (!header_method.empty() && !body_method.empty() && header_method != body_method) {
            json::Value data = json::Value::object();
            data.set("header", "Mcp-Method");
            data.set("headerValue", header_method);
            data.set("bodyValue", body_method);
            json::Value error = json::Value::object();
            error.set("code", error_codes::kHeaderMismatch);
            error.set("message", "Mcp-Method does not match the method in the request body");
            error.set("data", data);
            respond_rpc_error(400, id, std::move(error));
            return false;
        }

        // Mcp-Name names the tool, prompt or resource the method acts on. It
        // is only meaningful for the methods that take one.
        const std::string header_name = header_value(headers, lowered, "mcp-name");
        if (!header_name.empty()) {
            const std::string body_name = msg["params"]["name"].as_string();
            if (!body_name.empty() && header_name != body_name) {
                json::Value data = json::Value::object();
                data.set("header", "Mcp-Name");
                data.set("headerValue", header_name);
                data.set("bodyValue", body_name);
                json::Value error = json::Value::object();
                error.set("code", error_codes::kHeaderMismatch);
                error.set("message", "Mcp-Name does not match params.name in the request body");
                error.set("data", data);
                respond_rpc_error(400, id, std::move(error));
                return false;
            }
        }
        return true;
    }

    void respond(int status, const std::string& body) {
        replied_ = true;
        const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : "Error";
        std::ostringstream os;
        os << "HTTP/1.1 " << status << " " << reason << "\r\n"
           << "Content-Type: application/json\r\n"
           << "MCP-Protocol-Version: " << kModernProtocol << "\r\n"
           << (status == 405 ? "Allow: POST\r\n" : "") << "Content-Length: " << body.size()
           << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        const std::string out = os.str();
        std::size_t sent = 0;
        while (sent < out.size()) {
#if defined(_WIN32)
            const int n =
                ::send(client_, out.data() + sent, static_cast<int>(out.size() - sent), 0);
#else
            const ssize_t n = ::send(client_, out.data() + sent, out.size() - sent, 0);
#endif
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
    }

    socket_t listener_ = CC_INVALID_SOCKET;
    socket_t client_ = CC_INVALID_SOCKET;
    std::string token_;
    // Whether this connection has already been answered.
    bool replied_ = false;
};

}  // namespace

std::unique_ptr<Transport> make_stdio_transport() {
    return std::make_unique<StdioTransport>();
}

std::unique_ptr<Transport> make_http_transport(const ServerConfig& cfg, std::string* error) {
    auto set_error = [&](const std::string& m) {
        if (error) *error = m;
        return nullptr;
    };

#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return set_error("WSAStartup failed");
#endif

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == CC_INVALID_SOCKET) return set_error("socket() failed");

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(cfg.port));
    if (::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
        cc_close_socket(fd);
        return set_error("invalid host address: " + cfg.host);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        cc_close_socket(fd);
        return set_error("cannot bind " + cfg.host + ":" + std::to_string(cfg.port));
    }
    if (::listen(fd, 8) != 0) {
        cc_close_socket(fd);
        return set_error("listen() failed");
    }
    return std::make_unique<HttpTransport>(fd, cfg.auth_token);
}

}  // namespace cc::mcp
