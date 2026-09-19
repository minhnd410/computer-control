// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "actions/actions.hpp"
#include "cc/permissions.hpp"
#include "mcp/protocol.hpp"
#include "mcp/server.hpp"
#include "mcp/setup.hpp"

namespace {

using cc::mcp::ServerConfig;

void print_usage() {
    std::cout << R"(computer-control-mcp - MCP server for desktop and mobile-simulator control

USAGE
  computer-control-mcp [options]

TRANSPORT
  --transport stdio|http     Default: stdio (what MCP clients launch).
  --host HOST                HTTP bind address. Default: 127.0.0.1
  --port PORT                HTTP port. Default: 8765
  --auth-token TOKEN         Require `Authorization: Bearer TOKEN` on HTTP.
                             Also read from CC_AUTH_TOKEN so the token never
                             has to appear in a process listing.

TOOLS
  --tools a,b,c              Enable only these tools.
  --exclude-tools a,b        Disable these tools.
  --list-tools               Print every tool name and exit, noting which are
                             hidden by the current safety flags.

SAFETY
  --no-shell                 Refuse the shell tool. Recommended when the
                             client is not fully trusted.
  --no-clipboard             Refuse clipboard access.
  --allow-registry           Permit Windows registry writes (off by default).
  --max-capture-dimension N  Downscale captures so the longest side is N px.
                             Default 1600; 0 disables downscaling.

SETUP
  setup                      Register this server with the MCP clients on this
                             machine and request the permissions it needs. Run
                             `setup --help` for its options, `setup --list` to
                             see every client and where its config lives.

PROTOCOL
  Speaks MCP 2026-07-28, and falls back to 2025-06-18 for clients that open
  with an `initialize` handshake. No flag selects between them: the era is
  decided per request by how the client opens. `--version` prints both.

DIAGNOSTICS
  --doctor                   Print the permission and capability report and
                             exit. Run this first when the server is connected
                             but every tool seems to do nothing. Exits non-zero
                             if a permission is missing.
  --request-permissions      Ask the OS for anything missing, then --doctor.
                             On macOS this is what puts the binary in the
                             Accessibility list.
  --list-tools               (see TOOLS)

OTHER
  --prompt-permissions       Ask the OS for accessibility/recording access on
                             first use instead of failing.
  --verbose                  Log each tool call to stderr.
  --version, --help

ENVIRONMENT
  CC_AUTH_TOKEN              Same as --auth-token.
  CC_MAX_CAPTURE_DIMENSION   Same as --max-capture-dimension.
  CC_NO_SHELL=1              Same as --no-shell.

EXAMPLES
  # Claude Desktop / Cursor style stdio launch
  computer-control-mcp

  # Local HTTP with a token
  CC_AUTH_TOKEN=$(openssl rand -hex 16) computer-control-mcp --transport http

  # Screenshot-only server for an untrusted client
  computer-control-mcp --tools capabilities,displays,screenshot,snapshot,zoom
)";
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim, so "--tools click, type" behaves like "--tools click,type".
        const auto b = item.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        const auto e = item.find_last_not_of(" \t");
        out.push_back(item.substr(b, e - b + 1));
    }
    return out;
}

const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    ServerConfig cfg;

    // `setup` is a subcommand rather than a flag because it is a different
    // program: it edits other applications' configuration and talks to a
    // person, where everything else here speaks JSON-RPC to a machine.
    if (argc > 1 && std::string(argv[1]) == "setup") {
        cc::mcp::SetupOptions opts;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--list") {
                opts.list = true;
            } else if (a == "--shared") {
                opts.mode = cc::mcp::SetupOptions::Mode::Shared;
            } else if (a == "--per-client") {
                opts.mode = cc::mcp::SetupOptions::Mode::Stdio;
            } else if (a == "--status") {
                opts.status = true;
            } else if (a == "--stop") {
                opts.stop = true;
            } else if (a == "--restart") {
                opts.restart = true;
            } else if (a == "--port" && i + 1 < argc) {
                opts.port = std::atoi(argv[++i]);
            } else if (a == "--no-permissions") {
                opts.permissions = false;
            } else if (a == "--yes" || a == "-y") {
                opts.assume_yes = true;
            } else if (a == "--name" && i + 1 < argc) {
                opts.server_name = argv[++i];
            } else if (a == "--command" && i + 1 < argc) {
                opts.command = argv[++i];
            } else if (a == "--client" && i + 1 < argc) {
                // Comma-separated so one flag can name several.
                std::string list = argv[++i], item;
                std::istringstream ss(list);
                while (std::getline(ss, item, ',')) {
                    if (!item.empty()) opts.clients.push_back(item);
                }
            } else if (a == "--help" || a == "-h") {
                std::cout <<
                    R"(computer-control-mcp setup - register this server with the MCP clients
                            on this machine, then get the OS permissions it needs.

USAGE
  computer-control-mcp setup [options]

OPTIONS
  --list                 Show every client this can configure and where each
                         one keeps its config, then exit.
  --shared               Run one background service that every client shares,
                         without asking. macOS only.
  --per-client           Give each client its own copy over stdio, without
                         asking.
  --status               Report on the shared service and exit.
  --restart              Reload the shared service, after granting it a
                         permission.
  --stop                 Stop and remove the shared service.
  --port N               Port for the shared service. Default: 8765
  --client a,b           Configure these clients without asking. Ids come from
                         --list.
  --no-permissions       Skip the permission step.
  --yes, -y              Do not ask before requesting a missing permission.
  --name NAME            Register under this server name. Default:
                         computer-control
  --command PATH         Register this command instead of the running binary.

With no options it asks which of the detected clients to configure. Run it
again at any time; it rewrites its own entry and leaves the rest of the file
alone.
)";
                return 0;
            } else {
                std::cerr << "computer-control-mcp setup: unknown option '" << a
                          << "'. Try setup --help\n";
                return 2;
            }
        }
        return cc::mcp::run_setup(opts);
    }

    if (const char* t = env_or_null("CC_AUTH_TOKEN")) cfg.auth_token = t;
    if (const char* d = env_or_null("CC_MAX_CAPTURE_DIMENSION")) {
        cfg.session.default_max_capture_dimension = std::atoi(d);
    }
    if (env_or_null("CC_NO_SHELL")) cfg.session.allow_shell = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "computer-control: " << arg << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--version") {
            const auto b = cc::build_info();
            std::cout << "computer-control " << b.version << " (" << b.platform << ", "
                      << b.compiler << ")\n";
            // A client that refuses to connect is usually a protocol mismatch,
            // and this is the first thing worth checking.
            std::cout << "MCP protocol:";
            for (const auto& v : cc::mcp::supported_versions()) std::cout << " " << v;
            std::cout << "\n";
            return 0;
        } else if (arg == "--doctor" || arg == "--request-permissions") {
            // A server that is running but doing nothing is almost always a
            // permissions problem, and the client shows none of that. This is
            // the same report the `permissions` and `capabilities` tools give,
            // reachable without a client attached - which is why this binary
            // is the only one an operator needs to install.
            const bool request = (arg == "--request-permissions");
            if (request) {
                for (const auto& st : cc::check_permissions()) {
                    if (st.state != cc::PermissionState::Granted &&
                        st.state != cc::PermissionState::NotRequired) {
                        cc::request_permission(st.permission);
                    }
                }
            }
            auto s = cc::Session::create(cfg.session);
            if (!s) {
                std::cerr << "Cannot start a session: " << s.error().message << "\n";
                if (!s.error().remedy.empty()) std::cerr << "\n" << s.error().remedy << "\n";
                return 1;
            }
            // Runs the real tools rather than a parallel report, so what an
            // operator reads here is exactly what the model is told.
            for (const char* action : {"permissions", "capabilities"}) {
                const auto r = cc::actions::run(*s.value(), action, cc::json::Value::object());
                std::cout << r.text << "\n";
            }
            return cc::permission_guidance().empty() ? 0 : 1;
        } else if (arg == "--list-tools") {
            // The whole registry, including tools that are gated off by
            // default, so the flag answers "what exists" rather than "what
            // would this particular invocation advertise".
            for (const auto& t : cc::actions::registry()) {
                const std::string name = t.name;
                std::cout << name;
                if (name == "registry") std::cout << "  (hidden unless --allow-registry)";
                if (name == "shell") std::cout << "  (hidden by --no-shell)";
                if (name == "clipboard") std::cout << "  (hidden by --no-clipboard)";
                std::cout << "\n";
            }
            return 0;
        } else if (arg == "--transport")
            cfg.transport = next("stdio or http");
        else if (arg == "--host")
            cfg.host = next("a host");
        else if (arg == "--port")
            cfg.port = std::atoi(next("a port").c_str());
        else if (arg == "--auth-token")
            cfg.auth_token = next("a token");
        else if (arg == "--tools")
            cfg.enabled_tools = split_csv(next("a tool list"));
        else if (arg == "--exclude-tools")
            cfg.disabled_tools = split_csv(next("a tool list"));
        else if (arg == "--no-shell")
            cfg.session.allow_shell = false;
        else if (arg == "--no-clipboard")
            cfg.session.allow_clipboard = false;
        else if (arg == "--allow-registry")
            cfg.session.allow_registry = true;
        else if (arg == "--max-capture-dimension") {
            cfg.session.default_max_capture_dimension = std::atoi(next("a number").c_str());
        } else if (arg == "--prompt-permissions")
            cfg.session.prompt_for_permissions = true;
        else if (arg == "--verbose" || arg == "-v")
            cfg.log_requests = true;
        else {
            std::cerr << "computer-control: unknown option " << arg << "\n"
                      << "Run with --help for usage.\n";
            return 2;
        }
    }

    if (cfg.transport != "stdio" && cfg.transport != "http") {
        std::cerr << "computer-control: --transport must be stdio or http\n";
        return 2;
    }

    cc::mcp::Server server(std::move(cfg));
    return server.run();
}
