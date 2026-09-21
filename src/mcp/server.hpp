// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "core/json.hpp"
#include "mcp/config.hpp"

namespace cc::actions {
struct ActionResult;
}

namespace cc::mcp {

// A transport moves newline- or Content-Length-framed JSON-RPC messages.
class Transport {
public:
    virtual ~Transport() = default;
    // Blocks until a message arrives. Returns false at end of stream.
    virtual bool read(std::string& out) = 0;
    virtual bool write(const std::string& msg) = 0;
    virtual void close() = 0;
};

std::unique_ptr<Transport> make_stdio_transport();
std::unique_ptr<Transport> make_http_transport(const ServerConfig& cfg, std::string* error);

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    // Runs until the transport closes. Returns a process exit code.
    int run();

    // Exposed for tests: handle one JSON-RPC message and return the reply,
    // or an empty string for a notification.
    std::string handle_message(const std::string& raw);

private:
    json::Value handle_discover();
    json::Value handle_initialize(const json::Value& params);
    json::Value handle_tools_list(const json::Value& params);
    json::Value handle_tools_call(const json::Value& params, bool& is_error);

    bool tool_enabled(std::string_view name) const;

    ServerConfig cfg_;
    std::shared_ptr<Session> session_;
    // The single piece of connection state a dual-era server keeps, and only
    // so that a pre-2026 client which opened with `initialize` can keep
    // sending requests without per-request metadata. Modern requests never
    // consult it.
    std::atomic<bool> legacy_session_{false};
    std::unique_ptr<Transport> transport_;
};

// Builds the MCP tool list from the action registry, so a new action is
// exposed everywhere at once.
json::Value tool_definitions(const ServerConfig& cfg);

// Server instructions handed to the client during initialize.
const char* server_instructions();

// Converts verbose action summaries into compact MCP content while leaving
// structuredContent available for clients that need every returned field.
std::string compact_tool_text(std::string_view name, const actions::ActionResult& result);

}  // namespace cc::mcp
