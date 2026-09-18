// SPDX-License-Identifier: MIT
#include "mcp/server.hpp"

#include <algorithm>
#include <iostream>

#include "actions/actions.hpp"
#include "cc/screen.hpp"
#include "mcp/protocol.hpp"

namespace cc::mcp {
namespace {

json::Value rpc_error(const json::Value& id, int code, const std::string& message,
                      json::Value data = json::Value()) {
    json::Value err = json::Value::object();
    err.set("code", code);
    err.set("message", message);
    if (!data.is_null()) err.set("data", data);

    json::Value out = json::Value::object();
    out.set("jsonrpc", "2.0");
    out.set("id", id);
    out.set("error", err);
    return out;
}

// For an error object already built by the protocol layer.
json::Value rpc_error(const json::Value& id, json::Value error) {
    json::Value out = json::Value::object();
    out.set("jsonrpc", "2.0");
    out.set("id", id);
    out.set("error", std::move(error));
    return out;
}

json::Value rpc_result(const json::Value& id, json::Value result) {
    json::Value out = json::Value::object();
    out.set("jsonrpc", "2.0");
    out.set("id", id);
    // Every result carries resultType and the server identity; 2026-07-28
    // requires the former, and a stateless client has no handshake to have
    // learned the latter from.
    out.set("result", finalize_result(std::move(result)));
    return out;
}

void log(const ServerConfig& cfg, const std::string& msg) {
    if (cfg.log_requests) std::cerr << "[computer-control] " << msg << "\n";
}

}  // namespace

Server::Server(ServerConfig cfg) : cfg_(std::move(cfg)) {}
Server::~Server() = default;

// server/discover is mandatory in 2026-07-28. It is also the stdio
// backward-compatibility probe: a dual-era client sends it first, and a
// recognisable answer tells it this server speaks the modern protocol.
json::Value Server::handle_discover() {
    json::Value versions = json::Value::array();
    for (const auto& v : supported_versions()) versions.push_back(v);

    json::Value out = json::Value::object();
    out.set("supportedVersions", versions);
    out.set("capabilities", server_capabilities());
    out.set("instructions", server_instructions());
    // The tool set is fixed at startup, so this is cacheable for a long time
    // and is the same for every client.
    add_cache_hints(out, 3600000, "public");
    return out;
}

json::Value Server::handle_initialize(const json::Value& params) {
    json::Value out = json::Value::object();
    // Echo the requested version when supported. A handshake-era client has no
    // fall-forward mechanism, so refusing here leaves it with nothing; offer
    // the newest handshake revision instead and let it decide.
    const std::string requested = params["protocolVersion"].as_string(kLegacyProtocol);
    out.set("protocolVersion",
            is_supported_version(requested) ? requested : std::string(kLegacyProtocol));
    out.set("capabilities", server_capabilities());
    out.set("serverInfo", server_info());
    out.set("instructions", server_instructions());
    legacy_session_ = true;
    return out;
}

json::Value Server::handle_tools_list(const json::Value&) {
    json::Value out = json::Value::object();
    out.set("tools", tool_definitions(cfg_));
    // CacheableResult, required on list endpoints since 2026-07-28. The tool
    // set cannot change while the process runs, and it does not vary by
    // client, so it is publicly cacheable.
    add_cache_hints(out, 3600000, "public");
    return out;
}

bool Server::tool_enabled(std::string_view name) const {
    if (!cfg_.enabled_tools.empty()) {
        if (std::find(cfg_.enabled_tools.begin(), cfg_.enabled_tools.end(), name) ==
            cfg_.enabled_tools.end()) {
            return false;
        }
    }
    return std::find(cfg_.disabled_tools.begin(), cfg_.disabled_tools.end(), name) ==
           cfg_.disabled_tools.end();
}

json::Value Server::handle_tools_call(const json::Value& params, bool& is_error) {
    is_error = false;
    const std::string name = params["name"].as_string();
    const json::Value& args = params["arguments"];

    if (!tool_enabled(name)) {
        is_error = true;
        json::Value content = json::Value::array();
        json::Value t = json::Value::object();
        t.set("type", "text");
        t.set("text", "Tool '" + name + "' is disabled on this server.");
        content.push_back(t);
        json::Value out = json::Value::object();
        out.set("content", content);
        out.set("isError", true);
        return out;
    }

    // The session is created on first use rather than at startup, so a client
    // that only lists tools never triggers a permission prompt.
    if (!session_) {
        auto s = Session::create(cfg_.session);
        if (!s) {
            is_error = true;
            json::Value content = json::Value::array();
            json::Value t = json::Value::object();
            t.set("type", "text");
            t.set("text", "Cannot start a session: " + s.error().message +
                              (s.error().remedy.empty() ? "" : "\n\n" + s.error().remedy));
            content.push_back(t);
            json::Value out = json::Value::object();
            out.set("content", content);
            out.set("isError", true);
            return out;
        }
        session_ = s.value();
    }

    log(cfg_, "call " + name);
    auto result = actions::run(*session_, name, args.is_null() ? json::Value::object() : args);

    json::Value content = json::Value::array();

    if (!result.text.empty()) {
        json::Value t = json::Value::object();
        t.set("type", "text");
        t.set("text", result.text);
        content.push_back(t);
    }

    if (!result.ok) {
        std::string text = result.error.message;
        if (!result.error.remedy.empty()) text += "\n\n" + result.error.remedy;
        json::Value t = json::Value::object();
        t.set("type", "text");
        t.set("text", text);
        content.push_back(t);
        is_error = true;
    }

    if (!result.image.empty()) {
        json::Value img = json::Value::object();
        img.set("type", "image");
        img.set("data", base64(result.image));
        img.set("mimeType", result.image_mime.empty() ? "image/png" : result.image_mime);
        content.push_back(img);
    }

    if (content.size() == 0) {
        json::Value t = json::Value::object();
        t.set("type", "text");
        t.set("text", result.ok ? "Done." : "Failed.");
        content.push_back(t);
    }

    json::Value out = json::Value::object();
    out.set("content", content);
    // Structured output alongside the text: clients that can use it get exact
    // numbers instead of re-parsing a human-readable summary.
    if (!result.value.is_null()) out.set("structuredContent", result.value);
    if (is_error) out.set("isError", true);
    return out;
}

std::string Server::handle_message(const std::string& raw) {
    json::ParseError pe;
    json::Value msg = json::parse(raw, &pe);
    if (!pe.ok) {
        return rpc_error(json::Value(), error_codes::kParseError, "Parse error: " + pe.message)
            .dump();
    }

    const json::Value& id = msg["id"];
    const std::string method = msg["method"].as_string();
    const json::Value& params = msg["params"];
    // A notification has no id and must produce no response at all; replying
    // to one makes strict clients abort.
    const bool is_notification = id.is_null();

    if (method.empty()) {
        return is_notification
                   ? std::string{}
                   : rpc_error(id, error_codes::kInvalidRequest, "Invalid request: no method")
                         .dump();
    }

    // Notifications carry no metadata worth validating, and rejecting one is
    // impossible anyway since no response may be sent.
    if (!is_notification) {
        const ContextResult ctx = classify_request(msg, legacy_session_.load());
        if (!ctx.ok) return rpc_error(id, ctx.error).dump();
    }

    try {
        if (method == "server/discover") {
            return rpc_result(id, handle_discover()).dump();
        }
        if (method == "initialize") {
            return rpc_result(id, handle_initialize(params)).dump();
        }
        if (method == "tools/list") {
            return rpc_result(id, handle_tools_list(params)).dump();
        }
        if (method == "tools/call") {
            bool is_error = false;
            json::Value result = handle_tools_call(params, is_error);
            // A tool failure is reported inside the result with isError, not
            // as a JSON-RPC error: the distinction is protocol failure versus
            // the tool doing its job and reporting a problem.
            return rpc_result(id, std::move(result)).dump();
        }

        // Removed in 2026-07-28 but still answered, because a legacy client
        // will send them and has no way to discover that they are gone.
        if (method == "notifications/initialized" || method == "initialized") {
            legacy_session_ = true;
            return {};
        }
        if (method == "ping") {
            return rpc_result(id, json::Value::object()).dump();
        }
        if (method == "logging/setLevel") {
            return rpc_result(id, json::Value::object()).dump();
        }

        // Not advertised in capabilities, so a conforming client will not ask;
        // answering empty is friendlier than an error for one that does.
        if (method == "resources/list" || method == "prompts/list") {
            json::Value out = json::Value::object();
            out.set(method == "resources/list" ? "resources" : "prompts", json::Value::array());
            add_cache_hints(out, 3600000, "public");
            return rpc_result(id, out).dump();
        }

        if (method.rfind("notifications/", 0) == 0) return {};
        if (is_notification) return {};
        return rpc_error(id, error_codes::kMethodNotFound, "Method not found: " + method).dump();
    } catch (const std::exception& e) {
        if (is_notification) return {};
        return rpc_error(id, error_codes::kInternalError,
                         std::string("Internal error: ") + e.what())
            .dump();
    } catch (...) {
        if (is_notification) return {};
        return rpc_error(id, error_codes::kInternalError, "Internal error").dump();
    }
}

int Server::run() {
    if (cfg_.transport == "http") {
        std::string error;
        transport_ = make_http_transport(cfg_, &error);
        if (!transport_) {
            std::cerr << "computer-control: " << error << "\n";
            return 1;
        }
        std::cerr << "computer-control: listening on http://" << cfg_.host << ":" << cfg_.port
                  << (cfg_.auth_token.empty() ? "  (no auth token set)" : "  (bearer auth)")
                  << "\n";
        if (cfg_.auth_token.empty() && cfg_.host != "127.0.0.1" && cfg_.host != "localhost") {
            std::cerr << "computer-control: WARNING - bound to a non-loopback address with no "
                         "auth token. Anything that can reach this port can control this "
                         "machine.\n";
        }
    } else {
        transport_ = make_stdio_transport();
    }

    std::string line;
    while (transport_->read(line)) {
        const std::string reply = handle_message(line);
        if (!reply.empty()) {
            if (!transport_->write(reply)) break;
        }
    }

    // Never leave the desktop with a held button because the client hung up
    // mid-drag.
    if (session_) (void)session_->release_all();
    transport_->close();
    return 0;
}

}  // namespace cc::mcp
