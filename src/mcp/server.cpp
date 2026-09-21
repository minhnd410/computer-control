// SPDX-License-Identifier: MIT
#include "mcp/server.hpp"

#include <algorithm>
#include <iostream>

#include "actions/actions.hpp"
#include "cc/screen.hpp"
#include "core/text.hpp"
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

constexpr std::size_t kMaxToolTextBytes = 4096;

std::string bounded_tool_text(std::string_view value) {
    if (value.size() <= kMaxToolTextBytes) return std::string(value);
    constexpr std::string_view marker =
        "\n[summary truncated; use structuredContent for complete fields]";
    return text::truncate_utf8(value, kMaxToolTextBytes - marker.size()) + std::string(marker);
}

std::string collection_summary(std::string_view label, std::size_t count,
                               const json::Value& value) {
    const std::size_t returned = static_cast<std::size_t>(
        std::max<std::int64_t>(0, value["returned"].as_int(static_cast<std::int64_t>(count))));
    const std::size_t total = static_cast<std::size_t>(
        std::max<std::int64_t>(static_cast<std::int64_t>(returned),
                               value["total"].as_int(static_cast<std::int64_t>(returned))));
    std::string out = std::string(label) + ": " + std::to_string(returned);
    if (value["truncated"].as_bool(false)) {
        if (value.contains("total")) out += " of " + std::to_string(total);
        out += " returned; results are truncated";
        const std::string reason = value["truncation_reason"].as_string();
        if (!reason.empty()) out += " (" + reason + ")";
        out += ".";
    } else if (total > returned) {
        out += " of " + std::to_string(total) + " returned; results are truncated.";
    } else {
        out += " returned.";
    }
    return out;
}

json::Value success_structured(std::string_view action, const json::Value& data) {
    json::Value out = data.is_object() ? data : json::Value::object();
    if (!data.is_object()) out.set("data", data);
    out.set("ok", true);
    out.set("action", std::string(action));
    return out;
}

json::Value error_structured(std::string_view action, const Error& error,
                             const json::Value* partial = nullptr) {
    json::Value out = json::Value::object();
    out.set("ok", false);
    out.set("action", std::string(action));

    json::Value detail = json::Value::object();
    detail.set("code", to_string(error.code));
    detail.set("message", error.message);
    if (!error.remedy.empty()) detail.set("remedy", error.remedy);
    out.set("error", detail);
    if (partial && !partial->is_null()) out.set("result", *partial);
    return out;
}

void log(const ServerConfig& cfg, const std::string& msg) {
    if (cfg.log_requests) std::cerr << "[computer-control] " << msg << "\n";
}

std::string compact_tool_text_impl(std::string_view name, const actions::ActionResult& result) {
    if (!result.ok || result.value.is_null()) return bounded_tool_text(result.text);

    const json::Value& value = result.value;
    if (name == "snapshot" && value["elements"].is_array()) {
        return collection_summary("Snapshot", value["elements"].size(), value);
    }
    if (name == "snapshot" && value.contains("tree_error")) {
        return value.contains("screenshot") ? "Screenshot captured; accessibility tree unavailable."
                                            : "Accessibility tree unavailable.";
    }
    if (name == "elements" && value["roots"].is_array()) {
        return collection_summary("Accessibility tree", value["element_count"].as_int(), value);
    }
    if (name == "windows" && value["windows"].is_array()) {
        return collection_summary("Windows", value["windows"].size(), value);
    }
    if (name == "app" && value["apps"].is_array()) {
        return collection_summary("Applications", value["apps"].size(), value);
    }
    if (name == "menu" && value["items"].is_array()) {
        return collection_summary("Menu items", value["items"].size(), value);
    }
    if (name == "process" && value["processes"].is_array()) {
        return collection_summary("Processes", value["processes"].size(), value);
    }
    if (name == "device" && value["devices"].is_array()) {
        return collection_summary("Devices", value["devices"].size(), value);
    }
    if (name == "device" && value["elements"].is_array()) {
        return collection_summary("Device elements", value["elements"].size(), value);
    }
    if (name == "registry" && value["entries"].is_array()) {
        return collection_summary("Registry entries", value["entries"].size(), value);
    }
    if (name == "displays" && value["displays"].is_array()) {
        return collection_summary("Displays", value["displays"].size(), value);
    }
    if (name == "permissions" && value["permissions"].is_array()) {
        return std::string("Permissions: ") +
               (value["all_granted"].as_bool(false) ? "all granted" : "action required") + " (" +
               std::to_string(value["permissions"].size()) + " checked).";
    }
    if (name == "system" && value["actions"].is_array()) {
        std::size_t supported = 0;
        for (const auto& action : value["actions"].as_array()) {
            if (action["supported"].as_bool(false)) ++supported;
        }
        return "System actions: " + std::to_string(supported) + " of " +
               std::to_string(value["actions"].size()) + " supported.";
    }
    if (name == "capabilities" && value.is_object()) {
        std::size_t available = 0;
        for (const auto& [_, backend] : value["backends"].as_object()) {
            if (backend["available"].as_bool(false)) ++available;
        }
        return "Capabilities: " + value["platform"].as_string() + ", " +
               std::to_string(value["displays"].size()) + " display(s), " +
               std::to_string(available) + " backend(s) available.";
    }
    if (name == "batch" && value["steps"].is_array()) {
        if (value.contains("failed_at")) {
            return "Batch stopped at step " + std::to_string(value["failed_at"].as_int()) + ".";
        }
        return "Batch completed " + std::to_string(value["steps"].size()) + " steps.";
    }
    if (name == "shell" && (value.contains("stdout") || value.contains("stderr"))) {
        std::string out =
            "Command exited with code " + std::to_string(value["exit_code"].as_int()) +
            "; stdout " +
            std::to_string(value["stdout_bytes"].as_int(value["stdout"].as_string().size())) +
            " bytes; stderr " +
            std::to_string(value["stderr_bytes"].as_int(value["stderr"].as_string().size())) +
            " bytes.";
        if (value["timed_out"].as_bool(false)) out += " Timed out.";
        if (value["stdout_truncated"].as_bool(false) || value["stderr_truncated"].as_bool(false))
            out += " Output truncated.";
        return out;
    }
    if (name == "clipboard" && value.contains("text")) {
        std::string out =
            "Clipboard read: " +
            std::to_string(value["text_bytes"].as_int(value["text"].as_string().size())) +
            " bytes.";
        if (value["has_image"].as_bool(false)) out += " Includes an image.";
        if (value["has_files"].as_bool(false) ||
            (value["files"].is_array() && value["files"].size() > 0))
            out += " Includes files.";
        if (value["text_truncated"].as_bool(false)) out += " Text truncated.";
        return out;
    }
    return bounded_tool_text(result.text);
}

}  // namespace

std::string compact_tool_text(std::string_view name, const actions::ActionResult& result) {
    return compact_tool_text_impl(name, result);
}

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
        Error error{ErrorCode::PermissionDenied, "tool is disabled on this server",
                    "Enable the tool in the server configuration before calling it."};
        out.set("structuredContent", error_structured(name, error));
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
            out.set("structuredContent", error_structured(name, s.error()));
            out.set("isError", true);
            return out;
        }
        session_ = s.value();
    }

    log(cfg_, "call " + name);
    auto result = actions::run(*session_, name, args.is_null() ? json::Value::object() : args);

    json::Value content = json::Value::array();

    const std::string summary = result.ok ? compact_tool_text(name, result) : std::string{};
    if (!summary.empty()) {
        json::Value t = json::Value::object();
        t.set("type", "text");
        t.set("text", summary);
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
    if (result.ok && !result.value.is_null()) {
        out.set("structuredContent", success_structured(name, result.value));
    } else if (!result.ok) {
        out.set("structuredContent", error_structured(name, result.error, &result.value));
    }
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
            // Some clients omit notifications/initialized and send tools/list
            // immediately after the handshake. The initialize request itself
            // is enough to select the legacy protocol for this connection.
            legacy_session_ = true;
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
        if (!reply.empty() || cfg_.transport == "http") {
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
