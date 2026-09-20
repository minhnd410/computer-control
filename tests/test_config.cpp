// SPDX-License-Identifier: MIT

#include <cstdio>
#include <fstream>
#include <string>

#include "mcp/config.hpp"
#include "test_framework.hpp"

using namespace cc;

namespace {

const std::string kConfigPath = "/tmp/cc_server_config_test/config.json";
const std::string kInvalidConfigPath = "/tmp/cc_server_config_invalid.json";

void remove_config() {
    std::remove(kConfigPath.c_str());
    std::remove("/tmp/cc_server_config_test");
    std::remove(kInvalidConfigPath.c_str());
}

}  // namespace

TEST(server_config_missing_file_uses_defaults) {
    remove_config();
    const auto loaded = mcp::load_server_config(kConfigPath);
    CHECK(loaded.ok());
    CHECK_EQ(loaded.value().transport, std::string("stdio"));
    CHECK_EQ(loaded.value().host, std::string("127.0.0.1"));
    CHECK_EQ(loaded.value().port, 8765);
    CHECK(loaded.value().session.allow_shell);
    CHECK(loaded.value().enabled_tools.empty());
}

TEST(server_config_round_trips_server_and_session_flags) {
    remove_config();
    mcp::ServerConfig expected;
    expected.transport = "http";
    expected.host = "192.168.1.20";
    expected.port = 9876;
    expected.auth_token = "test-token";
    expected.enabled_tools = {"capabilities", "snapshot"};
    expected.disabled_tools = {"shell"};
    expected.log_requests = true;
    expected.session.allow_shell = false;
    expected.session.allow_clipboard = false;
    expected.session.allow_registry = true;
    expected.session.prompt_for_permissions = true;
    expected.session.default_max_capture_dimension = 800;

    CHECK(mcp::save_server_config(expected, kConfigPath).ok());
    const auto loaded = mcp::load_server_config(kConfigPath);
    CHECK(loaded.ok());
    CHECK_EQ(loaded.value().transport, expected.transport);
    CHECK_EQ(loaded.value().host, expected.host);
    CHECK_EQ(loaded.value().port, expected.port);
    CHECK_EQ(loaded.value().auth_token, expected.auth_token);
    CHECK_EQ(loaded.value().enabled_tools.size(), std::size_t{2});
    CHECK_EQ(loaded.value().disabled_tools[0], std::string("shell"));
    CHECK(loaded.value().log_requests);
    CHECK(!loaded.value().session.allow_shell);
    CHECK(!loaded.value().session.allow_clipboard);
    CHECK(loaded.value().session.allow_registry);
    CHECK(loaded.value().session.prompt_for_permissions);
    CHECK_EQ(loaded.value().session.default_max_capture_dimension, 800);
    remove_config();
}

TEST(server_config_rejects_invalid_values) {
    std::remove(kInvalidConfigPath.c_str());
    std::ofstream output(kInvalidConfigPath);
    output << R"({"server":{"transport":"udp"}})";
    output.close();

    const auto loaded = mcp::load_server_config(kInvalidConfigPath);
    CHECK(!loaded.ok());
    CHECK(loaded.error().message.find("transport") != std::string::npos);
    std::remove(kInvalidConfigPath.c_str());
}