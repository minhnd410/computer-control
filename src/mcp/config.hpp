// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "cc/session.hpp"

namespace cc::mcp {

// Options shared by the normal server, the launchd service, and setup. The
// JSON file is deliberately small and uses the same names as the CLI where
// possible, so a saved configuration is easy to inspect and reproduce.
struct ServerConfig {
    std::string transport = "stdio";  // stdio | http
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string auth_token;                  // required on HTTP when set
    std::vector<std::string> enabled_tools;  // empty = all
    std::vector<std::string> disabled_tools;
    bool log_requests = false;
    SessionConfig session;
};

// Defaults to ~/.config/computer-control/config.json (or the platform's
// equivalent config directory).
std::string default_server_config_path();

// Missing files return default options. Existing files must be valid JSON.
Result<ServerConfig> load_server_config(const std::string& path = {});
Status save_server_config(const ServerConfig& cfg, const std::string& path = {});

}  // namespace cc::mcp