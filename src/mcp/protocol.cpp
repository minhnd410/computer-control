// SPDX-License-Identifier: MIT
#include "mcp/protocol.hpp"

#include <algorithm>

#include "cc/session.hpp"

namespace cc::mcp {
namespace {

// Methods that only exist in the legacy era. Seeing one is how a dual-era
// server learns the client predates the stateless protocol.
bool is_legacy_only_method(std::string_view method) {
    return method == "initialize" || method == "notifications/initialized" ||
           method == "initialized" || method == "ping" || method == "logging/setLevel";
}

}  // namespace

std::vector<std::string> supported_versions() {
    // Newest first: clients pick the first they recognise.
    return {kModernProtocol, kLegacyProtocol, kOldestProtocol};
}

bool is_supported_version(std::string_view version) {
    for (const auto& v : supported_versions()) {
        if (v == version) return true;
    }
    return false;
}

json::Value server_info() {
    json::Value info = json::Value::object();
    info.set("name", "computer-control");
    info.set("version", build_info().version);
    return info;
}

json::Value server_capabilities() {
    json::Value tools = json::Value::object();
    // No listChanged: the tool set is fixed at startup, and claiming the
    // capability would promise notifications that never arrive.
    json::Value caps = json::Value::object();
    caps.set("tools", tools);
    return caps;
}

json::Value finalize_result(json::Value result, std::string_view result_type) {
    if (!result.is_object()) {
        json::Value wrapped = json::Value::object();
        wrapped.set("value", std::move(result));
        result = std::move(wrapped);
    }
    result.set("resultType", std::string(result_type));

    // Servers SHOULD identify themselves in every result, since a stateless
    // client has no handshake to have learned it from.
    json::Value meta = result.contains("_meta") ? result["_meta"] : json::Value::object();
    meta.set(meta_keys::kServerInfo, server_info());
    result.set("_meta", meta);
    return result;
}

json::Value& add_cache_hints(json::Value& result, long long ttl_ms, std::string_view scope) {
    result.set("ttlMs", ttl_ms);
    result.set("cacheScope", std::string(scope));
    return result;
}

json::Value unsupported_version_error(std::string_view requested) {
    json::Value supported = json::Value::array();
    for (const auto& v : supported_versions()) supported.push_back(v);

    json::Value data = json::Value::object();
    data.set("supported", supported);
    data.set("requested", std::string(requested));

    json::Value error = json::Value::object();
    error.set("code", error_codes::kUnsupportedProtocolVersion);
    error.set("message", "Unsupported protocol version");
    error.set("data", data);
    return error;
}

json::Value invalid_params_error(std::string_view message) {
    json::Value error = json::Value::object();
    error.set("code", error_codes::kInvalidParams);
    error.set("message", std::string(message));
    return error;
}

ContextResult classify_request(const json::Value& message, bool legacy_session) {
    ContextResult out;
    const std::string method = message["method"].as_string();
    const json::Value& params = message["params"];
    const json::Value& params_meta = params["_meta"];
    const json::Value& request_meta = message["_meta"];
    const json::Value* selected_meta = &params_meta;
    if ((!params_meta.is_object() || !params_meta.contains(meta_keys::kProtocolVersion)) &&
        request_meta.is_object() && request_meta.contains(meta_keys::kProtocolVersion)) {
        selected_meta = &request_meta;
    }
    const json::Value& meta = *selected_meta;

    const bool has_modern_meta = meta.is_object() && meta.contains(meta_keys::kProtocolVersion);

    // A modern request is self-describing, so it is served statelessly no
    // matter what came before it on this connection.
    if (has_modern_meta) {
        out.context.era = Era::Modern;
        out.context.protocol_version = meta[meta_keys::kProtocolVersion].as_string();

        if (!is_supported_version(out.context.protocol_version)) {
            out.error = unsupported_version_error(out.context.protocol_version);
            return out;
        }
        // clientCapabilities is required; a server must not guess at what the
        // client can do.
        if (!meta.contains(meta_keys::kClientCapabilities)) {
            out.error = invalid_params_error(std::string("missing required _meta field '") +
                                             meta_keys::kClientCapabilities + "'");
            return out;
        }
        out.context.client_capabilities = meta[meta_keys::kClientCapabilities];

        if (meta.contains(meta_keys::kClientInfo)) {
            out.context.client_name = meta[meta_keys::kClientInfo]["name"].as_string();
            out.context.client_version = meta[meta_keys::kClientInfo]["version"].as_string();
        }
        if (meta.contains(meta_keys::kLogLevel)) {
            out.context.log_level = meta[meta_keys::kLogLevel].as_string();
        }
        out.ok = true;
        return out;
    }

    // No modern protocol version. A legacy client may have completed a
    // handshake, and several MCP clients send ordinary requests with either no
    // metadata or unrelated metadata such as a progress token. Treat all of
    // those shapes as legacy compatibility. Modern requests are still strict
    // once they declare a protocol version above.
    if (is_legacy_only_method(method) || legacy_session || !has_modern_meta) {
        out.context.era = Era::Legacy;
        out.context.protocol_version = kLegacyProtocol;
        if (method == "initialize") {
            const std::string requested = params["protocolVersion"].as_string();
            if (!requested.empty()) out.context.protocol_version = requested;
            out.context.client_name = params["clientInfo"]["name"].as_string();
            out.context.client_version = params["clientInfo"]["version"].as_string();
            out.context.client_capabilities = params["capabilities"];
        }
        out.ok = true;
        return out;
    }

    // An ordinary method with no protocol version and no prior handshake. The
    // spec makes this malformed rather than something to guess at, and naming
    // the missing key is the difference between a client author finding the
    // problem in a minute and in an hour.
    out.error = invalid_params_error(
        std::string("missing required _meta field '") + meta_keys::kProtocolVersion +
        "'. This server speaks MCP " + kModernProtocol +
        ", where every request carries its protocol version, and also accepts the legacy " +
        kLegacyProtocol + " initialize handshake.");
    return out;
}

}  // namespace cc::mcp
