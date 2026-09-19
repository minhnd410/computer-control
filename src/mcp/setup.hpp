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

#include "cc/types.hpp"

namespace cc::mcp {

// Where a client keeps its MCP servers, and under which key.
struct ClientTarget {
    std::string id;           // stable, used by --client
    std::string name;         // shown to a person
    std::string config;       // absolute path, empty if unsupported on this OS
    std::string container;    // JSON key holding the server map ("mcpServers")
    bool toml = false;        // Codex keeps its config in TOML
    bool needs_type = false;  // VS Code wants an explicit "type": "stdio"
    // Whether this client can talk to an HTTP endpoint from its config file.
    // Claude Desktop cannot - it only launches local commands - so it keeps a
    // stdio entry even in shared mode, and keeps needing its own grant.
    bool supports_http = false;
    std::string note;  // shown alongside the entry
};

// The shared background service: one process started by launchd, which every
// client connects to over loopback HTTP.
//
// This exists for one reason. A grant belongs to the *responsible process*, so
// a server launched over stdio by Claude Desktop uses Claude Desktop's grant,
// one launched by VS Code uses VS Code's, and each of them has to be granted
// Accessibility separately - usually with no prompt, because macOS considers
// the request already answered by the parent. Started by launchd the server is
// its own responsible process: it appears in System Settings under its own
// name, and one grant serves every client.
struct AgentStatus {
    bool installed = false;
    bool running = false;
    int port = 0;
    std::string plist;
    std::string binary;
    std::string detail;
};

AgentStatus agent_status();
// Runs a tool on the shared service and returns its text output. Empty on
// failure. Used by --doctor, which must report the service's state rather than
// the state of whatever terminal happened to launch it.
std::string ask_service(const std::string& tool);
// Whether stdin and stdout are both a terminal, so a prompt can be answered.
bool interactive_terminal();

// Walks the user through each OS permission in turn, raising the prompt from
// the service's own process and waiting until it is actually granted before
// moving to the next. Returns true when everything ended up granted.
bool guide_permissions(bool assume_yes);
// Writes the plist, starts the job, and waits for the port to answer.
Status install_agent(const std::string& command, int port, std::string* token_out);
Status uninstall_agent();
// The bearer token, generated on first install and stored 0600.
std::string agent_token();

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
    bool stop = false;     // tear the agent down and exit
    bool status = false;   // report on the agent and exit
    bool restart = false;  // reload the agent, after a permission change
    int port = 8765;
    std::string server_name = "computer-control";
    std::string command;            // defaults to this executable's path
    bool command_explicit = false;  // --command was passed, so honour it
};

// Runs the flow. Returns a process exit code.
int run_setup(const SetupOptions& opts);

// Writes (or updates) one client's config. `error` is set on failure.
bool configure_client(const ClientTarget& t, const std::string& server_name,
                      const std::string& command, std::string* error);

// Points a client at the shared service instead of spawning its own.
bool configure_client_http(const ClientTarget& t, const std::string& server_name,
                           const std::string& url, const std::string& token, std::string* error);

// For a client that can only launch a command: registers `bridge <url>`, which
// forwards to the service. The bridge needs no permission of its own, so the
// grant still lives with the service.
bool configure_client_bridge(const ClientTarget& t, const std::string& server_name,
                             const std::string& command, const std::string& url,
                             std::string* error);

}  // namespace cc::mcp
