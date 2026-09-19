// SPDX-License-Identifier: MIT
#include "mcp/setup.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "cc/permissions.hpp"
#include "core/json.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <direct.h>
#include <io.h>
#define cc_isatty _isatty
#define cc_fileno _fileno
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define cc_isatty isatty
#define cc_fileno fileno
#endif

namespace cc::mcp {
namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : std::string{};
}

std::string home() {
#if defined(_WIN32)
    std::string p = env("USERPROFILE");
    if (!p.empty()) return p;
    return env("HOMEDRIVE") + env("HOMEPATH");
#else
    return env("HOME");
#endif
}

bool exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path);
    return f.good();
}

// The directory part of a path, so we can tell "client installed but never
// configured" from "client not installed".
std::string parent_dir(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool dir_exists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    // Opening a directory as a stream fails, but stat-free detection is enough
    // here: a readable path that is not a regular file is treated as present.
    std::ifstream f(path);
    if (f.good()) return true;
    return ::access(path.c_str(), F_OK) == 0;
#endif
}

bool make_parent_dirs(const std::string& path) {
    const std::string dir = parent_dir(path);
    if (dir.empty() || dir_exists(dir)) return true;
    // Build the path one component at a time; no <filesystem> because libstdc++
    // needs a separate link flag for it on some distributions this targets.
    std::string acc;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        acc += dir[i];
        const bool sep = (dir[i] == '/' || dir[i] == '\\');
        if (!sep && i + 1 != dir.size()) continue;
        std::string part = sep ? acc.substr(0, acc.size() - 1) : acc;
        if (part.empty() || part == "~") continue;
#if defined(_WIN32)
        _mkdir(part.c_str());
#else
        ::mkdir(part.c_str(), 0755);
#endif
    }
    return dir_exists(dir);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& text, std::string* error) {
    if (!make_parent_dirs(path)) {
        if (error) *error = "cannot create " + parent_dir(path);
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    out << text;
    return out.good();
}

}  // namespace

std::vector<ClientTarget> client_targets() {
    const std::string h = home();
    std::vector<ClientTarget> out;

#if defined(__APPLE__)
    const std::string app_support = h + "/Library/Application Support";
    out.push_back(
        {"claude-code", "Claude Code", h + "/.claude.json", "mcpServers", false, false, ""});
    out.push_back({"claude-desktop", "Claude Desktop",
                   app_support + "/Claude/claude_desktop_config.json", "mcpServers", false, false,
                   "restart Claude Desktop afterwards"});
    out.push_back({"vscode", "VS Code / GitHub Copilot", app_support + "/Code/User/mcp.json",
                   "servers", false, true, ""});
    out.push_back({"cursor", "Cursor", h + "/.cursor/mcp.json", "mcpServers", false, false, ""});
    out.push_back({"windsurf", "Windsurf", h + "/.codeium/windsurf/mcp_config.json", "mcpServers",
                   false, false, ""});
    out.push_back(
        {"codex", "Codex CLI", h + "/.codex/config.toml", "mcp_servers", true, false, ""});
    out.push_back(
        {"zed", "Zed", h + "/.config/zed/settings.json", "context_servers", false, false, ""});
#elif defined(_WIN32)
    const std::string appdata = env("APPDATA");
    out.push_back(
        {"claude-code", "Claude Code", h + "/.claude.json", "mcpServers", false, false, ""});
    out.push_back({"claude-desktop", "Claude Desktop",
                   appdata + "/Claude/claude_desktop_config.json", "mcpServers", false, false,
                   "restart Claude Desktop afterwards"});
    out.push_back({"vscode", "VS Code / GitHub Copilot", appdata + "/Code/User/mcp.json", "servers",
                   false, true, ""});
    out.push_back({"cursor", "Cursor", h + "/.cursor/mcp.json", "mcpServers", false, false, ""});
    out.push_back({"windsurf", "Windsurf", h + "/.codeium/windsurf/mcp_config.json", "mcpServers",
                   false, false, ""});
    out.push_back(
        {"codex", "Codex CLI", h + "/.codex/config.toml", "mcp_servers", true, false, ""});
#else
    const std::string cfg =
        env("XDG_CONFIG_HOME").empty() ? h + "/.config" : env("XDG_CONFIG_HOME");
    out.push_back(
        {"claude-code", "Claude Code", h + "/.claude.json", "mcpServers", false, false, ""});
    out.push_back({"vscode", "VS Code / GitHub Copilot", cfg + "/Code/User/mcp.json", "servers",
                   false, true, ""});
    out.push_back({"cursor", "Cursor", h + "/.cursor/mcp.json", "mcpServers", false, false, ""});
    out.push_back({"windsurf", "Windsurf", h + "/.codeium/windsurf/mcp_config.json", "mcpServers",
                   false, false, ""});
    out.push_back(
        {"codex", "Codex CLI", h + "/.codex/config.toml", "mcp_servers", true, false, ""});
    out.push_back({"zed", "Zed", cfg + "/zed/settings.json", "context_servers", false, false, ""});
#endif
    return out;
}

bool client_installed(const ClientTarget& t) {
    if (t.config.empty()) return false;
    // Either the config is already there, or the client's directory is - which
    // is the case for a client installed but never given an MCP server.
    return exists(t.config) || dir_exists(parent_dir(t.config));
}

namespace {

// Codex uses TOML, and pulling in a TOML library for one table would be a
// dependency for a dozen lines. The entry is appended when absent and left
// alone when present, which is the only edit needed here.
bool configure_toml(const ClientTarget& t, const std::string& server_name,
                    const std::string& command, std::string* error) {
    std::string text = read_file(t.config);
    const std::string header = "[" + t.container + "." + server_name + "]";
    if (text.find(header) != std::string::npos) return true;  // already there

    if (!text.empty() && text.back() != '\n') text += "\n";
    if (!text.empty()) text += "\n";
    text += header + "\n";
    text += "command = \"" + command + "\"\n";
    text += "args = []\n";
    return write_file(t.config, text, error);
}

}  // namespace

bool configure_client(const ClientTarget& t, const std::string& server_name,
                      const std::string& command, std::string* error) {
    if (t.config.empty()) {
        if (error) *error = "not supported on this platform";
        return false;
    }
    if (t.toml) return configure_toml(t, server_name, command, error);

    json::Value root = json::Value::object();
    const std::string existing = read_file(t.config);
    if (!existing.empty()) {
        json::ParseError pe;
        json::Value parsed = json::parse(existing, &pe);
        if (!pe.ok) {
            // Never overwrite a config we cannot read: the user's other
            // servers live in this file, and losing them to fix ours is a bad
            // trade. Say what is wrong and let them fix it.
            if (error) *error = "existing config is not valid JSON (" + pe.message + ")";
            return false;
        }
        if (parsed.is_object()) root = parsed;
    }

    json::Value entry = json::Value::object();
    if (t.needs_type) entry.set("type", "stdio");
    entry.set("command", command);

    json::Value servers = root.contains(t.container) && root[t.container].is_object()
                              ? root[t.container]
                              : json::Value::object();
    servers.set(server_name, entry);
    root.set(t.container, servers);

    return write_file(t.config, root.dump(2) + "\n", error);
}

namespace {

bool interactive() {
    return cc_isatty(cc_fileno(stdin)) && cc_isatty(cc_fileno(stdout));
}

void print_permission_summary() {
    const auto states = check_permissions();
    for (const auto& st : states) {
        if (st.state == PermissionState::NotRequired) continue;
        const char* mark = st.state == PermissionState::Granted ? "ok" : "--";
        std::cout << "  [" << mark << "] " << to_string(st.permission) << "  "
                  << to_string(st.state) << "\n";
    }
}

}  // namespace

int run_setup(const SetupOptions& opts_in) {
    SetupOptions opts = opts_in;
    if (opts.command.empty()) opts.command = executable_path();
    if (opts.command.empty()) opts.command = "computer-control-mcp";

    const auto targets = client_targets();

    if (opts.list) {
        std::cout << "Clients this can configure:\n\n";
        for (const auto& t : targets) {
            std::cout << "  " << (client_installed(t) ? "found    " : "not found")  //
                      << "  " << t.id << "\n      " << t.name << "\n      " << t.config << "\n";
        }
        std::cout << "\nConfigure one with:  computer-control-mcp setup --client <id>\n";
        return 0;
    }

    std::vector<const ClientTarget*> chosen;

    if (!opts.clients.empty()) {
        for (const auto& want : opts.clients) {
            auto it = std::find_if(targets.begin(), targets.end(),
                                   [&](const ClientTarget& t) { return t.id == want; });
            if (it == targets.end()) {
                std::cerr << "Unknown client '" << want
                          << "'. Try: computer-control-mcp setup --list\n";
                return 2;
            }
            chosen.push_back(&*it);
        }
    } else {
        std::vector<const ClientTarget*> found;
        for (const auto& t : targets) {
            if (client_installed(t)) found.push_back(&t);
        }

        if (found.empty()) {
            std::cout << "No MCP clients detected on this machine.\n\n"
                      << "Add the server by hand with this command:\n  " << opts.command << "\n\n"
                      << "computer-control-mcp setup --list  shows where each client keeps its "
                         "config.\n\n";
        } else if (!interactive()) {
            // A pipe or a CI job cannot answer a prompt, and silently editing
            // someone's editor config because they ran this non-interactively
            // would be worse than doing nothing.
            std::cout << "Detected these MCP clients:\n\n";
            for (const auto* t : found) std::cout << "  " << t->id << "  (" << t->name << ")\n";
            std::cout << "\nNot a terminal, so nothing was changed. Re-run interactively, or:\n"
                      << "  computer-control-mcp setup --client " << found.front()->id << "\n\n";
        } else {
            std::cout << "Found these MCP clients. Which should get computer-control?\n\n";
            for (std::size_t i = 0; i < found.size(); ++i) {
                std::cout << "  " << (i + 1) << ") " << found[i]->name << "\n";
            }
            std::cout << "\nEnter numbers separated by spaces, 'a' for all, or Enter to skip: "
                      << std::flush;

            std::string line;
            std::getline(std::cin, line);
            if (line == "a" || line == "A" || line == "all") {
                chosen = found;
            } else {
                std::istringstream ss(line);
                int n = 0;
                while (ss >> n) {
                    if (n >= 1 && n <= static_cast<int>(found.size())) {
                        chosen.push_back(found[static_cast<std::size_t>(n - 1)]);
                    }
                }
            }
            std::cout << "\n";
        }
    }

    int failures = 0;
    for (const auto* t : chosen) {
        std::string error;
        if (configure_client(*t, opts.server_name, opts.command, &error)) {
            std::cout << "  added to " << t->name << "  (" << t->config << ")\n";
            if (!t->note.empty()) std::cout << "      " << t->note << "\n";
        } else {
            ++failures;
            std::cout << "  could not update " << t->name << ": " << error << "\n";
        }
    }
    if (!chosen.empty()) std::cout << "\n";

    if (opts.permissions) {
        std::cout << "Permissions:\n";
        print_permission_summary();

        const std::string guidance = permission_guidance();
        if (!guidance.empty()) {
            bool go = opts.assume_yes;
            if (!go && interactive()) {
                std::cout << "\nSomething is missing. Ask the OS for it now? [Y/n] " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                go = answer.empty() || answer[0] == 'y' || answer[0] == 'Y';
            }
            if (go) {
                for (const auto& st : check_permissions()) {
                    if (st.state != PermissionState::Granted &&
                        st.state != PermissionState::NotRequired) {
                        request_permission(st.permission);
                    }
                }
                std::cout << "\n";
                print_permission_summary();
            }
            const std::string after = permission_guidance();
            if (!after.empty()) std::cout << "\n" << after << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "Check anything later with:\n"
              << "  computer-control-mcp --doctor          what this host can do\n"
              << "  computer-control-mcp setup --list      where each client keeps its config\n"
              << "  computer-control-mcp setup             run this again\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace cc::mcp
