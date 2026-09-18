// SPDX-License-Identifier: MIT
#pragma once

// MCP wire protocol, revision 2026-07-28, with a fallback for 2025-06-18.
//
// 2026-07-28 made the protocol stateless. There is no initialize handshake any
// more: every request carries its protocol version and the client's
// capabilities in `_meta`, and the server must not infer anything from
// previous requests on the same connection. `ping`, `logging/setLevel` and the
// handshake itself were removed; `server/discover` became mandatory; every
// result must carry a `resultType`.
//
// Clients lag servers, so this speaks both eras. Which one a request gets is
// decided by how it opens, per the spec's dual-era rules: a request carrying
// modern `_meta` is served statelessly, an `initialize` selects legacy
// semantics for the rest of the stdio process.

#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

namespace cc::mcp {

// Newest first: this is also the order advertised in server/discover and in
// the `supported` list of an UnsupportedProtocolVersionError.
inline constexpr const char* kModernProtocol = "2026-07-28";
inline constexpr const char* kLegacyProtocol = "2025-06-18";

// Reserved `_meta` keys. Spelled out rather than built from a prefix constant
// so a grep for the literal finds every use.
namespace meta_keys {
inline constexpr const char* kProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kClientInfo = "io.modelcontextprotocol/clientInfo";
inline constexpr const char* kClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kLogLevel = "io.modelcontextprotocol/logLevel";
inline constexpr const char* kServerInfo = "io.modelcontextprotocol/serverInfo";
}  // namespace meta_keys

// MCP-specification error codes. -32020..-32099 is reserved for the spec;
// implementations must not invent codes in that range.
namespace error_codes {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kMissingRequiredClientCapability = -32021;
inline constexpr int kUnsupportedProtocolVersion = -32022;
}  // namespace error_codes

enum class Era { Modern, Legacy };

// What a request's `_meta` said about the client.
struct RequestContext {
    Era era = Era::Modern;
    std::string protocol_version;
    std::string client_name;
    std::string client_version;
    json::Value client_capabilities;
    std::string log_level;
};

// Outcome of inspecting a request against the dual-era rules.
struct ContextResult {
    bool ok = false;
    RequestContext context;
    // Set when ok is false; ready to send as the `error` member.
    json::Value error;
};

bool is_supported_version(std::string_view version);
std::vector<std::string> supported_versions();

// Decides which era a request belongs to and validates its metadata.
//
// `legacy_session` is whether an `initialize` has already been seen on this
// connection. It is the one piece of state a dual-era server has to keep, and
// only to serve clients from before the protocol became stateless.
ContextResult classify_request(const json::Value& message, bool legacy_session);

// Wraps a handler's result so it satisfies the modern shape: a `resultType`
// and the server's identity in `_meta`. Harmless for legacy clients, which
// ignore unknown fields, so it is applied unconditionally.
json::Value finalize_result(json::Value result, std::string_view result_type = "complete");

// Adds the CacheableResult fields that 2026-07-28 requires on list endpoints.
json::Value& add_cache_hints(json::Value& result, long long ttl_ms, std::string_view scope);

json::Value unsupported_version_error(std::string_view requested);
json::Value missing_capability_error(const std::vector<std::string>& required);
json::Value invalid_params_error(std::string_view message);

// The server's own identity, as reported in `_meta` and server/discover.
json::Value server_info();
json::Value server_capabilities();

}  // namespace cc::mcp
