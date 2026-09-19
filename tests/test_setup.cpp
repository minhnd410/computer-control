// SPDX-License-Identifier: MIT
//
// Setup writes into files other applications own. Getting it wrong does not
// produce a failed build or a bad tool call - it silently destroys somebody's
// editor configuration, which is the worst failure mode in this project. Every
// case here is about not damaging what is already in the file.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/json.hpp"
#include "mcp/setup.hpp"
#include "test_framework.hpp"

using namespace cc;

namespace {

std::string temp_dir() {
#if defined(_WIN32)
    const char* base = std::getenv("TEMP");
    return std::string(base ? base : ".") + "\\cc_setup_test";
#else
    return "/tmp/cc_setup_test";
#endif
}

std::string write_temp(const std::string& name, const std::string& contents) {
    const std::string path = temp_dir() + "/" + name;
    // configure_client creates missing parents, so a nested path is fine here.
    std::ofstream out(path, std::ios::binary);
    if (out) out << contents;
    return path;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

mcp::ClientTarget json_target(const std::string& path, const std::string& container = "mcpServers",
                              bool needs_type = false) {
    mcp::ClientTarget t;
    t.id = "test";
    t.name = "Test Client";
    t.config = path;
    t.container = container;
    t.needs_type = needs_type;
    return t;
}

void remove_file(const std::string& path) {
    std::remove(path.c_str());
}

}  // namespace

TEST(setup_creates_a_config_that_did_not_exist) {
    const std::string path = temp_dir() + "/fresh/mcp.json";
    remove_file(path);

    std::string error;
    CHECK(
        mcp::configure_client(json_target(path), "computer-control", "/usr/local/bin/ccm", &error));
    CHECK_EQ(error, std::string());

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["mcpServers"]["computer-control"]["command"].as_string(),
             std::string("/usr/local/bin/ccm"));
    remove_file(path);
}

TEST(setup_keeps_every_server_it_did_not_add) {
    // The whole file belongs to the user. Ours is one key in it.
    const std::string path = write_temp("keep.json", R"({
      "mcpServers": {
        "theirs": {"command": "/usr/bin/other", "args": ["--flag"]}
      },
      "unrelatedTopLevelKey": {"keep": true}
    })");

    std::string error;
    CHECK(mcp::configure_client(json_target(path), "computer-control", "/bin/ccm", &error));

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["mcpServers"]["theirs"]["command"].as_string(), std::string("/usr/bin/other"));
    CHECK_EQ(v["mcpServers"]["theirs"]["args"][0].as_string(), std::string("--flag"));
    CHECK(v["unrelatedTopLevelKey"]["keep"].as_bool());
    CHECK_EQ(v["mcpServers"]["computer-control"]["command"].as_string(), std::string("/bin/ccm"));
    remove_file(path);
}

TEST(setup_refuses_to_overwrite_a_config_it_cannot_parse) {
    // A malformed file is far more likely to be a user's work-in-progress edit
    // than junk. Replacing it with a valid file containing only our entry would
    // delete every other server they had.
    const std::string broken = R"({"mcpServers": { this is not json)";
    const std::string path = write_temp("broken.json", broken);

    std::string error;
    CHECK(!mcp::configure_client(json_target(path), "computer-control", "/bin/ccm", &error));
    CHECK(!error.empty());
    CHECK_EQ(slurp(path), broken);
    remove_file(path);
}

TEST(setup_is_idempotent) {
    const std::string path = write_temp("idem.json", "");
    std::string error;
    CHECK(mcp::configure_client(json_target(path), "computer-control", "/bin/ccm", &error));
    const std::string first = slurp(path);
    CHECK(mcp::configure_client(json_target(path), "computer-control", "/bin/ccm", &error));
    CHECK_EQ(slurp(path), first);
    remove_file(path);
}

TEST(setup_rewrites_its_own_entry_on_a_path_change) {
    // Re-running after moving the binary must update the command, not add a
    // second entry under the same name.
    const std::string path = write_temp("move.json", "");
    std::string error;
    CHECK(mcp::configure_client(json_target(path), "computer-control", "/old/ccm", &error));
    CHECK(mcp::configure_client(json_target(path), "computer-control", "/new/ccm", &error));

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["mcpServers"]["computer-control"]["command"].as_string(), std::string("/new/ccm"));
    CHECK_EQ(v["mcpServers"].size(), std::size_t{1});
    remove_file(path);
}

TEST(setup_honours_the_container_key_and_the_type_field) {
    // VS Code keeps servers under "servers" and wants an explicit transport;
    // writing the Claude shape into it produces a config it ignores.
    const std::string path = write_temp("vscode.json", "");
    std::string error;
    CHECK(mcp::configure_client(json_target(path, "servers", /*needs_type=*/true),
                                "computer-control", "/bin/ccm", &error));

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["servers"]["computer-control"]["type"].as_string(), std::string("stdio"));
    CHECK(!v.contains("mcpServers"));
    remove_file(path);
}

TEST(setup_appends_to_toml_without_touching_what_is_there) {
    const std::string path =
        write_temp("codex.toml", "model = \"o3\"\n\n[mcp_servers.theirs]\ncommand = \"other\"\n");

    mcp::ClientTarget t = json_target(path, "mcp_servers");
    t.toml = true;

    std::string error;
    CHECK(mcp::configure_client(t, "computer-control", "/bin/ccm", &error));

    const std::string text = slurp(path);
    CHECK(text.find("model = \"o3\"") != std::string::npos);
    CHECK(text.find("[mcp_servers.theirs]") != std::string::npos);
    CHECK(text.find("[mcp_servers.computer-control]") != std::string::npos);

    // And appending twice must not produce two tables with the same name,
    // which is a TOML parse error rather than a merge.
    CHECK(mcp::configure_client(t, "computer-control", "/bin/ccm", &error));
    const std::string again = slurp(path);
    CHECK_EQ(again, text);
    remove_file(path);
}

TEST(setup_knows_where_each_client_keeps_its_config) {
    const auto targets = mcp::client_targets();
    CHECK(targets.size() >= 5);
    for (const auto& t : targets) {
        char note[192];
        std::snprintf(note, sizeof(note), "client '%s'", t.id.c_str());
        ::test::report(!t.id.empty(), "client has an id", __FILE__, __LINE__, note);
        ::test::report(!t.name.empty(), "client has a name", __FILE__, __LINE__, note);
        ::test::report(!t.container.empty(), "client names its server container", __FILE__,
                       __LINE__, note);
        // An absolute path; a relative one would write into the cwd.
        ::test::report(t.config.size() > 1 && (t.config[0] == '/' || t.config[1] == ':'),
                       "client config path is absolute", __FILE__, __LINE__, note);
    }
}

TEST(setup_http_entry_carries_the_token_and_leaves_others_alone) {
    const std::string path = write_temp("http.json", R"({
      "mcpServers": {"theirs": {"command": "/usr/bin/other"}}
    })");

    std::string error;
    mcp::ClientTarget t = json_target(path);
    t.supports_http = true;
    CHECK(mcp::configure_client_http(t, "computer-control", "http://127.0.0.1:8765/mcp", "deadbeef",
                                     &error));

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    const json::Value& e = v["mcpServers"]["computer-control"];
    CHECK_EQ(e["type"].as_string(), std::string("http"));
    CHECK_EQ(e["url"].as_string(), std::string("http://127.0.0.1:8765/mcp"));
    CHECK_EQ(e["headers"]["Authorization"].as_string(), std::string("Bearer deadbeef"));
    // A stdio entry would carry `command`; switching modes must not leave both.
    CHECK(!e.contains("command"));
    CHECK_EQ(v["mcpServers"]["theirs"]["command"].as_string(), std::string("/usr/bin/other"));
    remove_file(path);
}

TEST(setup_refuses_an_http_entry_for_a_client_that_cannot_use_one) {
    // Claude Desktop only launches local commands. Writing an http entry into
    // its config would produce a server it silently ignores, which is worse
    // than telling the caller it cannot be done.
    const std::string path = write_temp("nohttp.json", "");
    mcp::ClientTarget t = json_target(path);
    t.supports_http = false;

    std::string error;
    CHECK(!mcp::configure_client_http(t, "computer-control", "http://127.0.0.1:8765/mcp", "x",
                                      &error));
    CHECK(!error.empty());
    remove_file(path);
}

TEST(setup_knows_which_clients_can_reach_an_http_endpoint) {
    // If this flips by accident, `setup --shared` either writes entries a
    // client ignores or needlessly keeps one on stdio with its own grant.
    bool saw_http = false, saw_stdio_only = false;
    for (const auto& t : mcp::client_targets()) {
        if (t.id == "claude-code" || t.id == "vscode" || t.id == "cursor") {
            char note[128];
            std::snprintf(note, sizeof(note), "%s should support http", t.id.c_str());
            ::test::report(t.supports_http, "client supports http", __FILE__, __LINE__, note);
            saw_http = true;
        }
        // Codex takes `--url` for a streamable HTTP server, verified against
        // the CLI itself; it belongs in the http group.
        if (t.id == "codex") {
            ::test::report(t.supports_http, "codex supports http", __FILE__, __LINE__,
                           "codex mcp add --url writes a url entry");
            saw_http = true;
        }
        if (t.id == "claude-desktop") {
            char note[128];
            std::snprintf(note, sizeof(note), "%s should stay stdio-only", t.id.c_str());
            ::test::report(!t.supports_http, "client is stdio-only", __FILE__, __LINE__, note);
            saw_stdio_only = true;
        }
    }
    CHECK(saw_http);
    CHECK(saw_stdio_only);
}

TEST(setup_writes_codex_http_as_toml_with_an_env_var_token) {
    // Codex will not take the token from the config file, so the entry names
    // the variable instead. Writing the JSON shape here would produce a server
    // Codex ignores.
    const std::string path = write_temp("codexhttp.toml", "model = \"o3\"\n");
    mcp::ClientTarget t = json_target(path, "mcp_servers");
    t.toml = true;
    t.supports_http = true;

    std::string error;
    CHECK(mcp::configure_client_http(t, "computer-control", "http://127.0.0.1:8765/mcp", "secret",
                                     &error));
    const std::string text = slurp(path);
    CHECK(text.find("[mcp_servers.computer-control]") != std::string::npos);
    CHECK(text.find("url = \"http://127.0.0.1:8765/mcp\"") != std::string::npos);
    CHECK(text.find("bearer_token_env_var = \"CC_AUTH_TOKEN\"") != std::string::npos);
    // The secret itself must not be in the file.
    CHECK(text.find("secret") == std::string::npos);
    CHECK(text.find("model = \"o3\"") != std::string::npos);
    remove_file(path);
}

TEST(setup_bridge_entry_launches_the_forwarder_without_the_token) {
    // The bridge reads the token from the file the service wrote. Copying the
    // secret into every client's config would spread it for no benefit.
    const std::string path = write_temp("bridge.json", "");
    mcp::ClientTarget t = json_target(path);
    t.supports_http = false;

    std::string error;
    CHECK(mcp::configure_client_bridge(t, "computer-control", "/usr/local/bin/ccm",
                                       "http://127.0.0.1:8765/mcp", &error));

    json::ParseError pe;
    json::Value v = json::parse(slurp(path), &pe);
    CHECK(pe.ok);
    const json::Value& e = v["mcpServers"]["computer-control"];
    CHECK_EQ(e["command"].as_string(), std::string("/usr/local/bin/ccm"));
    CHECK_EQ(e["args"][0].as_string(), std::string("bridge"));
    CHECK_EQ(e["args"][1].as_string(), std::string("http://127.0.0.1:8765/mcp"));
    CHECK(!e.contains("headers"));
    remove_file(path);
}
