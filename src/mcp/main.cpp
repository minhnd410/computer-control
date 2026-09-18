// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "actions/actions.hpp"
#include "mcp/server.hpp"

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
            return 0;
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
