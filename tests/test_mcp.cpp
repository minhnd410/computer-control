// SPDX-License-Identifier: MIT
//
// MCP wire conformance. These drive Server::handle_message with real JSON-RPC
// text rather than calling the handlers directly, because the interesting
// behaviour - which era a request gets, and what a malformed one is told - all
// lives in the dispatch path, not in the handlers.
//
// Nothing here may reach a tool that needs a session: the session is created
// lazily on the first tools/call, and creating one on a developer's machine
// pops a permission dialog. Every case below stops short of that.

#include <string>

#include "actions/actions.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"
#include "test_framework.hpp"

using namespace cc;
using cc::mcp::Era;

namespace {

mcp::ServerConfig test_config() {
    mcp::ServerConfig cfg;
    cfg.transport = "stdio";
    return cfg;
}

// Parses a reply and returns its `result`, failing the case if it is an error.
json::Value result_of(const std::string& reply) {
    json::ParseError pe;
    json::Value msg = json::parse(reply, &pe);
    if (!pe.ok) {
        ::test::report(false, "reply parses as JSON", __FILE__, __LINE__, pe.message);
        return json::Value();
    }
    if (msg.contains("error")) {
        ::test::report(false, "reply is a result, not an error", __FILE__, __LINE__,
                       msg["error"]["message"].as_string());
        return json::Value();
    }
    return msg["result"];
}

json::Value error_of(const std::string& reply) {
    json::ParseError pe;
    json::Value msg = json::parse(reply, &pe);
    if (!pe.ok) return json::Value();
    return msg["error"];
}

// A request body carrying the metadata every 2026-07-28 request must have.
std::string modern(const std::string& method, const std::string& extra_params = {}) {
    std::string params = R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                         R"("io.modelcontextprotocol/clientCapabilities":{}})";
    if (!extra_params.empty()) params += "," + extra_params;
    return R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":{)" + params + "}}";
}

json::Value meta_of(const json::Value& v) {
    return v["_meta"];
}

}  // namespace

// --- protocol layer -------------------------------------------------------

TEST(mcp_advertises_newest_version_first) {
    const auto versions = mcp::supported_versions();
    CHECK_EQ(versions.size(), std::size_t{3});
    // Clients pick the first version they recognise, so the order is load
    // bearing, not cosmetic.
    CHECK_EQ(versions[0], std::string(mcp::kModernProtocol));
    CHECK_EQ(versions[1], std::string(mcp::kLegacyProtocol));
    CHECK_EQ(versions[2], std::string(mcp::kOldestProtocol));
    CHECK(mcp::is_supported_version(mcp::kModernProtocol));
    CHECK(mcp::is_supported_version(mcp::kLegacyProtocol));
    CHECK(mcp::is_supported_version(mcp::kOldestProtocol));
    // The revision real clients ask for today. Losing it would silently
    // downgrade every Claude Code session to an older one.
    CHECK(mcp::is_supported_version("2025-11-25"));
    CHECK(!mcp::is_supported_version("2024-11-05"));
    CHECK(!mcp::is_supported_version(""));
}

TEST(mcp_finalize_result_adds_the_required_fields) {
    json::Value v = json::Value::object();
    v.set("tools", json::Value::array());
    json::Value out = mcp::finalize_result(std::move(v));

    CHECK_EQ(out["resultType"].as_string(), std::string("complete"));
    // A stateless client never saw a handshake, so the server identifies
    // itself in every result instead.
    CHECK_EQ(meta_of(out)["io.modelcontextprotocol/serverInfo"]["name"].as_string(),
             std::string("computer-control"));
    CHECK(out.contains("tools"));
}

TEST(mcp_finalize_result_preserves_an_existing_meta) {
    json::Value v = json::Value::object();
    json::Value meta = json::Value::object();
    meta.set("vendor/trace", "abc");
    v.set("_meta", meta);

    json::Value out = mcp::finalize_result(std::move(v));
    CHECK_EQ(meta_of(out)["vendor/trace"].as_string(), std::string("abc"));
    CHECK(meta_of(out).contains("io.modelcontextprotocol/serverInfo"));
}

TEST(mcp_classify_accepts_a_modern_request) {
    json::ParseError pe;
    json::Value msg =
        json::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
                    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                    R"("io.modelcontextprotocol/clientCapabilities":{"elicitation":{}},)"
                    R"("io.modelcontextprotocol/clientInfo":{"name":"probe","version":"9.9"},)"
                    R"("io.modelcontextprotocol/logLevel":"debug"}}})",
                    &pe);
    CHECK(pe.ok);

    const auto ctx = mcp::classify_request(msg, /*legacy_session=*/false);
    CHECK(ctx.ok);
    CHECK(ctx.context.era == Era::Modern);
    CHECK_EQ(ctx.context.protocol_version, std::string("2026-07-28"));
    CHECK_EQ(ctx.context.client_name, std::string("probe"));
    CHECK_EQ(ctx.context.client_version, std::string("9.9"));
    CHECK_EQ(ctx.context.log_level, std::string("debug"));
    CHECK(ctx.context.client_capabilities.contains("elicitation"));
}

TEST(mcp_classify_rejects_an_unsupported_version) {
    json::ParseError pe;
    json::Value msg =
        json::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
                    R"("io.modelcontextprotocol/protocolVersion":"2030-01-01",)"
                    R"("io.modelcontextprotocol/clientCapabilities":{}}}})",
                    &pe);
    CHECK(pe.ok);

    const auto ctx = mcp::classify_request(msg, false);
    CHECK(!ctx.ok);
    CHECK_EQ(ctx.error["code"].as_int(), std::int64_t{-32022});
    // The client cannot fall forward without being told what this server
    // does speak.
    CHECK_EQ(ctx.error["data"]["requested"].as_string(), std::string("2030-01-01"));
    CHECK_EQ(ctx.error["data"]["supported"].size(), std::size_t{3});
}

TEST(mcp_classify_requires_client_capabilities) {
    json::ParseError pe;
    json::Value msg =
        json::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
                    R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
                    &pe);
    CHECK(pe.ok);

    const auto ctx = mcp::classify_request(msg, false);
    CHECK(!ctx.ok);
    CHECK_EQ(ctx.error["code"].as_int(), std::int64_t{-32602});
    CHECK(ctx.error["message"].as_string().find("clientCapabilities") != std::string::npos);
}

TEST(mcp_classify_routes_initialize_to_the_legacy_era) {
    json::ParseError pe;
    json::Value msg = json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                                  R"("protocolVersion":"2025-06-18","capabilities":{"roots":{}},)"
                                  R"("clientInfo":{"name":"claude-desktop","version":"1.2"}}})",
                                  &pe);
    CHECK(pe.ok);

    const auto ctx = mcp::classify_request(msg, false);
    CHECK(ctx.ok);
    CHECK(ctx.context.era == Era::Legacy);
    CHECK_EQ(ctx.context.protocol_version, std::string("2025-06-18"));
    CHECK_EQ(ctx.context.client_name, std::string("claude-desktop"));
    CHECK(ctx.context.client_capabilities.contains("roots"));
}

TEST(mcp_classify_rejects_a_bare_request_but_says_why) {
    json::ParseError pe;
    json::Value msg =
        json::parse(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})", &pe);
    CHECK(pe.ok);

    const auto ctx = mcp::classify_request(msg, /*legacy_session=*/false);
    CHECK(!ctx.ok);
    CHECK_EQ(ctx.error["code"].as_int(), std::int64_t{-32602});
    // Naming the missing key is the difference between a client author
    // finding this in a minute and in an hour.
    CHECK(ctx.error["message"].as_string().find("protocolVersion") != std::string::npos);

    // The same request is fine once a handshake has happened.
    const auto after = mcp::classify_request(msg, /*legacy_session=*/true);
    CHECK(after.ok);
    CHECK(after.context.era == Era::Legacy);
}

// --- dispatch: modern era -------------------------------------------------

TEST(mcp_discover_answers_without_a_handshake) {
    mcp::Server server(test_config());
    json::Value r = result_of(server.handle_message(modern("server/discover")));

    CHECK_EQ(r["resultType"].as_string(), std::string("complete"));
    CHECK_EQ(r["supportedVersions"].size(), std::size_t{3});
    CHECK_EQ(r["supportedVersions"][0].as_string(), std::string("2026-07-28"));
    CHECK(r["capabilities"].contains("tools"));
    CHECK(!r["instructions"].as_string().empty());
    // server/discover is a list endpoint: CacheableResult is required.
    CHECK(r["ttlMs"].as_int() > 0);
    CHECK_EQ(r["cacheScope"].as_string(), std::string("public"));
    CHECK(meta_of(r).contains("io.modelcontextprotocol/serverInfo"));
}

TEST(mcp_tools_list_works_statelessly) {
    mcp::Server server(test_config());
    json::Value r = result_of(server.handle_message(modern("tools/list")));

    CHECK(r["tools"].size() > 0);
    CHECK_EQ(r["resultType"].as_string(), std::string("complete"));
    CHECK(r["ttlMs"].as_int() > 0);

    // Every advertised tool needs the three fields a client reads.
    for (std::size_t i = 0; i < r["tools"].size(); ++i) {
        const json::Value& t = r["tools"][i];
        CHECK(!t["name"].as_string().empty());
        CHECK(!t["description"].as_string().empty());
        CHECK(t["inputSchema"].is_object());
    }
}

TEST(mcp_a_modern_request_does_not_open_a_legacy_session) {
    mcp::Server server(test_config());
    CHECK(!result_of(server.handle_message(modern("tools/list"))).is_null());

    // Serving one stateless request must not make the server start guessing
    // for the next one - that would silently mask a broken client.
    const std::string bare = R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})";
    CHECK_EQ(error_of(server.handle_message(bare))["code"].as_int(), std::int64_t{-32602});
}

// --- dispatch: legacy era -------------------------------------------------

TEST(mcp_legacy_handshake_still_works_end_to_end) {
    mcp::Server server(test_config());

    const std::string init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                             R"("protocolVersion":"2025-06-18","capabilities":{},)"
                             R"("clientInfo":{"name":"claude-desktop","version":"1.0"}}})";
    json::Value r = result_of(server.handle_message(init));
    CHECK_EQ(r["protocolVersion"].as_string(), std::string("2025-06-18"));
    CHECK_EQ(r["serverInfo"]["name"].as_string(), std::string("computer-control"));
    CHECK(!r["instructions"].as_string().empty());

    // notifications/initialized must produce no reply at all: answering a
    // notification makes strict clients abort the connection.
    CHECK_EQ(server.handle_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"),
             std::string());

    // And now the client may send bare requests for the rest of the process.
    CHECK(result_of(server.handle_message(
              R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{}})"))["tools"]
              .size() > 0);
    // ping and logging/setLevel are gone in 2026-07-28, but a legacy client
    // has no way to discover that, so they are still answered.
    CHECK(
        !result_of(server.handle_message(R"({"jsonrpc":"2.0","id":4,"method":"ping"})")).is_null());
}

TEST(mcp_initialize_falls_back_on_an_unknown_version) {
    mcp::Server server(test_config());
    const std::string init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                             R"("protocolVersion":"2024-11-05","capabilities":{}}})";
    json::Value r = result_of(server.handle_message(init));
    // A handshake-era client has no fall-forward mechanism, so refusing here
    // leaves it with nothing. Offer the newest handshake revision instead.
    CHECK_EQ(r["protocolVersion"].as_string(), std::string("2025-11-25"));
}

TEST(mcp_initialize_echoes_the_revision_real_clients_send) {
    // Claude Code opens with exactly this, confirmed by teeing a real session:
    // initialize{2025-11-25} -> notifications/initialized -> tools/list. If
    // this ever answers something else, every such client is silently
    // downgraded and nothing says so.
    mcp::Server server(test_config());
    const std::string init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                             R"("protocolVersion":"2025-11-25","capabilities":{},)"
                             R"("clientInfo":{"name":"claude-code","version":"2.0"}}})";
    json::Value r = result_of(server.handle_message(init));
    CHECK_EQ(r["protocolVersion"].as_string(), std::string("2025-11-25"));

    // And the bare follow-ups a handshake client sends must work.
    CHECK_EQ(server.handle_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"),
             std::string());
    CHECK(result_of(server.handle_message(
              R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})"))["tools"]
              .size() > 0);
}

// --- dispatch: errors -----------------------------------------------------

TEST(mcp_malformed_json_is_a_parse_error_with_a_null_id) {
    mcp::Server server(test_config());
    json::ParseError pe;
    json::Value msg = json::parse(server.handle_message("{not json"), &pe);
    CHECK(pe.ok);
    CHECK_EQ(msg["error"]["code"].as_int(), std::int64_t{-32700});
    CHECK(msg["id"].is_null());
}

TEST(mcp_unknown_method_is_method_not_found) {
    mcp::Server server(test_config());
    CHECK_EQ(error_of(server.handle_message(modern("tools/frobnicate")))["code"].as_int(),
             std::int64_t{-32601});
}

TEST(mcp_unknown_notification_is_silently_dropped) {
    mcp::Server server(test_config());
    CHECK_EQ(server.handle_message(R"({"jsonrpc":"2.0","method":"notifications/cancelled"})"),
             std::string());
    // No method and no id is malformed, but still a notification.
    CHECK_EQ(server.handle_message(R"({"jsonrpc":"2.0"})"), std::string());
}

TEST(mcp_a_disabled_tool_reports_itself_as_a_tool_error) {
    mcp::ServerConfig cfg = test_config();
    cfg.disabled_tools = {"screenshot"};
    mcp::Server server(cfg);

    // A disabled tool is not advertised at all, rather than advertised and
    // refusing, so the model's attention is not spent on it.
    json::Value list = result_of(server.handle_message(modern("tools/list")));
    for (std::size_t i = 0; i < list["tools"].size(); ++i) {
        CHECK(list["tools"][i]["name"].as_string() != "screenshot");
    }

    // Calling it anyway is a tool error inside the result, not a JSON-RPC
    // error - and it must not create a session on the way.
    json::Value r = result_of(
        server.handle_message(modern("tools/call", R"("name":"screenshot","arguments":{})")));
    CHECK(r["isError"].as_bool());
    CHECK(r["content"][0]["text"].as_string().find("disabled") != std::string::npos);
}

// Walks a JSON Schema and reports the nodes that would fail a strict
// validator. Recursive because a bad node can be nested arbitrarily deep.
namespace {

void check_schema_node(const json::Value& node, const std::string& tool, const std::string& path) {
    if (node.is_object()) {
        // An `array` with no `items` is the one that bites in practice:
        // GitHub Copilot rejects the whole tool with "tool parameters array
        // type must have items", so a single missing key takes the server
        // down for that client rather than degrading one argument.
        if (node["type"].as_string() == "array") {
            // `items` must exist *and* name a type. GitHub Copilot rejects the
            // whole tool otherwise - "tool parameters array type must have
            // items" - because it cannot turn an untyped element into a
            // function-calling parameter. An `items` carrying only a
            // description looks fine to a JSON Schema linter and still fails
            // there, which is how this survived a first fix.
            const bool has_typed_items =
                node.contains("items") && node["items"].is_object() &&
                (node["items"].contains("type") || node["items"].contains("oneOf") ||
                 node["items"].contains("anyOf") || node["items"].contains("$ref"));
            char note[256];
            std::snprintf(note, sizeof(note), "%s: '%s' is type array whose items declare no type",
                          tool.c_str(), path.c_str());
            ::test::report(has_typed_items, "array items declare a type", __FILE__, __LINE__, note);
        }
        for (const auto& [key, child] : node.as_object()) {
            check_schema_node(child, tool, path.empty() ? key : path + "." + key);
        }
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            check_schema_node(node[i], tool, path + "[]");
        }
    }
}

}  // namespace

TEST(mcp_every_builtin_schema_actually_parses) {
    // tools.cpp substitutes a permissive {"type":"object"} when a built-in
    // schema fails to parse, so a stray brace does not crash the server - it
    // silently strips every argument description from that one tool instead.
    // That is far harder to notice than a crash, and it is exactly how a
    // broken `gesture` schema shipped, so it is checked here rather than
    // trusted.
    for (const auto& spec : actions::registry()) {
        json::ParseError pe;
        json::Value schema = json::parse(spec.schema_json, &pe);
        char note[256];
        std::snprintf(note, sizeof(note), "%s: schema_json does not parse: %s", spec.name,
                      pe.message.c_str());
        ::test::report(pe.ok, "built-in schema is valid JSON", __FILE__, __LINE__, note);
        if (!pe.ok) continue;

        std::snprintf(note, sizeof(note), "%s: schema root is not an object", spec.name);
        ::test::report(schema.is_object() && schema["type"].as_string() == "object",
                       "schema root is an object", __FILE__, __LINE__, note);

        // A tool with arguments must describe them. An empty `properties` on a
        // tool that takes arguments is the shape the fallback produces.
        if (std::string(spec.name) != "cursor_position" &&
            std::string(spec.name) != "release_all") {
            std::snprintf(note, sizeof(note), "%s: schema has no properties - fallback shape?",
                          spec.name);
            ::test::report(schema.contains("properties"), "schema declares properties", __FILE__,
                           __LINE__, note);
        }
    }
}

TEST(mcp_tool_schemas_survive_a_strict_validator) {
    mcp::ServerConfig cfg = test_config();
    // Every tool, including the gated ones, because a client that enables them
    // validates them too.
    cfg.session.allow_registry = true;
    mcp::Server server(cfg);

    json::Value list = result_of(server.handle_message(modern("tools/list")));
    CHECK(list["tools"].size() > 0);

    for (std::size_t i = 0; i < list["tools"].size(); ++i) {
        const json::Value& tool = list["tools"][i];
        const std::string name = tool["name"].as_string();
        const json::Value& schema = tool["inputSchema"];
        // A tool whose schema is not an object cannot be validated at all.
        CHECK(schema.is_object());
        CHECK(schema["type"].as_string() == "object");
        check_schema_node(schema, name, "");
        if (name == "device") {
            const json::Value& points = schema["properties"]["points"];
            CHECK_EQ(points["type"].as_string(), std::string("array"));
            CHECK(points["items"].is_object());
            CHECK_EQ(points["items"]["type"].as_string(), std::string("object"));
        }

        // Every argument names a type. An untyped property passes a JSON
        // Schema linter and still tells a model nothing about the shape to
        // send, which shows up as wrong tool calls rather than as an error.
        for (const auto& [key, prop] : schema["properties"].as_object()) {
            const bool typed =
                prop.is_object() &&
                (prop.contains("type") || prop.contains("enum") || prop.contains("oneOf") ||
                 prop.contains("anyOf") || prop.contains("$ref"));
            char note[256];
            std::snprintf(note, sizeof(note), "%s: argument '%s' declares no type", name.c_str(),
                          key.c_str());
            ::test::report(typed, "argument declares a type", __FILE__, __LINE__, note);
        }
    }
}

TEST(mcp_registry_is_hidden_unless_it_is_asked_for) {
    mcp::Server server(test_config());
    json::Value list = result_of(server.handle_message(modern("tools/list")));
    for (std::size_t i = 0; i < list["tools"].size(); ++i) {
        CHECK(list["tools"][i]["name"].as_string() != "registry");
    }
}
