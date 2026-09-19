// SPDX-License-Identifier: MIT
#pragma once

// First-run setup: register this server with the MCP clients on the machine,
// then get the OS permissions it needs.
//
// This is a command rather than something an installer does, because neither
// Homebrew nor winget can prompt. A formula's post-install runs with no
// terminal attached and winget's portable installer has no hook at all, so an
// installer that "asks which client to use" cannot exist. What can exist is a
// single command the install instructions point at, which is what this is.

#include <string>
#include <vector>

namespace cc::mcp {

// Where a client keeps its MCP servers, and under which key.
struct ClientTarget {
    std::string id;           // stable, used by --client
    std::string name;         // shown to a person
    std::string config;       // absolute path, empty if unsupported on this OS
    std::string container;    // JSON key holding the server map ("mcpServers")
    bool toml = false;        // Codex keeps its config in TOML
    bool needs_type = false;  // VS Code wants an explicit "type": "stdio"
    std::string note;         // shown alongside the entry
};

// Every client this knows how to configure, in a stable order. Entries whose
// config directory does not exist are still returned; `installed` says which
// are actually present.
std::vector<ClientTarget> client_targets();
bool client_installed(const ClientTarget& t);

struct SetupOptions {
    std::vector<std::string> clients;  // empty = ask
    bool list = false;
    bool permissions = true;
    bool assume_yes = false;
    std::string server_name = "computer-control";
    std::string command;  // defaults to this executable's path
};

// Runs the flow. Returns a process exit code.
int run_setup(const SetupOptions& opts);

// Writes (or updates) one client's config. `error` is set on failure.
bool configure_client(const ClientTarget& t, const std::string& server_name,
                      const std::string& command, std::string* error);

}  // namespace cc::mcp
