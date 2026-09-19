// SPDX-License-Identifier: MIT
#include "mcp/setup.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <chrono>
#include <thread>

#include "cc/permissions.hpp"
#include "core/json.hpp"
#include "devices/device_internal.hpp"

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

namespace {

// Named construction rather than a seven-field aggregate initialiser: the
// positional form is unreadable at the call site and silently shifts meaning
// when a field is added in the middle.
ClientTarget target(std::string id, std::string name, std::string config, std::string container) {
    ClientTarget t;
    t.id = std::move(id);
    t.name = std::move(name);
    t.config = std::move(config);
    t.container = std::move(container);
    return t;
}

}  // namespace

std::vector<ClientTarget> client_targets() {
    const std::string h = home();
    std::vector<ClientTarget> out;

    auto add = [&out](ClientTarget t) { out.push_back(std::move(t)); };

#if defined(__APPLE__)
    const std::string app_support = h + "/Library/Application Support";
    const std::string vscode_cfg = app_support + "/Code/User/mcp.json";
    const std::string desktop_cfg = app_support + "/Claude/claude_desktop_config.json";
#elif defined(_WIN32)
    const std::string appdata = env("APPDATA");
    const std::string vscode_cfg = appdata + "/Code/User/mcp.json";
    const std::string desktop_cfg = appdata + "/Claude/claude_desktop_config.json";
#else
    const std::string cfg_home =
        env("XDG_CONFIG_HOME").empty() ? h + "/.config" : env("XDG_CONFIG_HOME");
    const std::string vscode_cfg = cfg_home + "/Code/User/mcp.json";
    const std::string desktop_cfg;  // Claude Desktop has no Linux build
#endif

    {
        ClientTarget t = target("claude-code", "Claude Code", h + "/.claude.json", "mcpServers");
        t.supports_http = true;
        add(std::move(t));
    }
    if (!desktop_cfg.empty()) {
        ClientTarget t = target("claude-desktop", "Claude Desktop", desktop_cfg, "mcpServers");
        // Claude Desktop only launches local commands from its config file, so
        // it cannot be pointed at the shared service and keeps needing its own
        // Accessibility grant.
        t.supports_http = false;
        t.note = "restart Claude Desktop afterwards";
        add(std::move(t));
    }
    {
        ClientTarget t = target("vscode", "VS Code / GitHub Copilot", vscode_cfg, "servers");
        t.needs_type = true;
        t.supports_http = true;
        add(std::move(t));
    }
    {
        ClientTarget t = target("cursor", "Cursor", h + "/.cursor/mcp.json", "mcpServers");
        t.supports_http = true;
        add(std::move(t));
    }
    add(target("windsurf", "Windsurf", h + "/.codeium/windsurf/mcp_config.json", "mcpServers"));
    {
        ClientTarget t = target("codex", "Codex CLI", h + "/.codex/config.toml", "mcp_servers");
        t.toml = true;
        // Codex takes a streamable-HTTP server as `url`, but reads the bearer
        // token from an environment variable it must already have - it will
        // not take the token itself from the config file.
        t.supports_http = true;
        t.note = "export CC_AUTH_TOKEN so Codex can authenticate (see setup --status)";
        add(std::move(t));
    }
#if !defined(_WIN32)
    {
        const std::string zed =
#if defined(__APPLE__)
            h + "/.config/zed/settings.json";
#else
            cfg_home + "/zed/settings.json";
#endif
        add(target("zed", "Zed", zed, "context_servers"));
    }
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

// --- the shared background service ---------------------------------------

namespace {

constexpr const char* kAgentLabel = "dev.computercontrol.mcp";

std::string agent_plist_path() {
#if defined(__APPLE__)
    return home() + "/Library/LaunchAgents/" + kAgentLabel + ".plist";
#else
    return {};
#endif
}

std::string token_path() {
    return home() + "/.config/computer-control/token";
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Reads the port out of an installed plist, so status does not have to assume
// the default.
int port_from_plist(const std::string& text) {
    const auto pos = text.find("--port");
    if (pos == std::string::npos) return 0;
    const auto open = text.find("<string>", pos + 6);
    if (open == std::string::npos) return 0;
    const auto close = text.find("</string>", open);
    if (close == std::string::npos) return 0;
    return std::atoi(text.substr(open + 8, close - open - 8).c_str());
}

bool port_answers(int port, const std::string& token) {
    // A bare TCP connect would say "something is listening"; asking for
    // tools/list says "our server is listening and working", which is the
    // thing worth reporting.
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
                             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                             R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
    std::vector<std::string> args{
        "-fsS", "-m", "4", "-X", "POST", "http://127.0.0.1:" + std::to_string(port) + "/mcp"};
    if (!token.empty()) {
        args.push_back("-H");
        args.push_back("Authorization: Bearer " + token);
    }
    args.push_back("-d");
    args.push_back(body);
    const auto r = devices::exec("curl", args, std::chrono::milliseconds{6000});
    return r.exit_code == 0 && r.out.find("\"tools\"") != std::string::npos;
}

}  // namespace

std::string agent_token() {
    const std::string existing = trim(read_file(token_path()));
    if (!existing.empty()) return existing;

    // 32 hex characters from the system CSPRNG. The token guards a loopback
    // port that can drive the desktop, so it is not a formality.
    const auto r = devices::exec("openssl", {"rand", "-hex", "16"});
    std::string token = trim(r.out);
    if (token.size() < 32) return {};

    std::string error;
    if (!write_file(token_path(), token + "\n", &error)) return {};
#if !defined(_WIN32)
    ::chmod(token_path().c_str(), 0600);
#endif
    return token;
}

AgentStatus agent_status() {
    AgentStatus st;
    st.plist = agent_plist_path();
    if (st.plist.empty()) {
        st.detail = "the shared service is macOS-only (it uses launchd)";
        return st;
    }
    const std::string text = read_file(st.plist);
    st.installed = !text.empty();
    if (!st.installed) {
        st.detail = "not installed";
        return st;
    }
    st.port = port_from_plist(text);
    if (st.port == 0) st.port = 8765;

    const auto pos = text.find("<string>/");
    if (pos != std::string::npos) {
        const auto close = text.find("</string>", pos);
        if (close != std::string::npos) st.binary = text.substr(pos + 8, close - pos - 8);
    }

    st.running = port_answers(st.port, agent_token());
    st.detail = st.running ? "running" : "installed but not answering";
    return st;
}

Status install_agent(const std::string& command, int port, std::string* token_out) {
#if !defined(__APPLE__)
    (void)command;
    (void)port;
    (void)token_out;
    return err(ErrorCode::Unsupported,
               "the shared service is macOS-only, because it is a launchd job",
               "On Windows and Linux each client launches its own copy over stdio, which needs "
               "no permission grant on those platforms anyway.");
#else
    const std::string token = agent_token();
    if (token.empty()) {
        return err(ErrorCode::IoError, "could not create the bearer token",
                   "Check that " + token_path() + " is writable.");
    }
    if (token_out) *token_out = token;

    // KeepAlive restarts it after a crash or a log-out; RunAtLoad starts it
    // now and on every login. The token goes in the environment rather than
    // the argument list so it does not show up in `ps`.
    std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>";
    plist += kAgentLabel;
    plist += "</string>\n  <key>ProgramArguments</key>\n  <array>\n";
    for (const std::string& a :
         {command, std::string("--transport"), std::string("http"), std::string("--host"),
          std::string("127.0.0.1"), std::string("--port"), std::to_string(port)}) {
        plist += "    <string>" + a + "</string>\n";
    }
    plist +=
        "  </array>\n"
        "  <key>EnvironmentVariables</key>\n  <dict>\n"
        "    <key>CC_AUTH_TOKEN</key><string>" +
        token +
        "</string>\n  </dict>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "  <key>ProcessType</key><string>Interactive</string>\n"
        "</dict>\n</plist>\n";

    std::string error;
    const std::string path = agent_plist_path();
    if (!write_file(path, plist, &error)) return err(ErrorCode::IoError, error);
#if !defined(_WIN32)
    ::chmod(path.c_str(), 0600);
#endif

    const std::string domain = "gui/" + std::to_string(static_cast<int>(::getuid()));
    // Replacing an existing job requires booting the old one out first;
    // bootstrap on a loaded label fails with "service already loaded".
    (void)devices::exec("launchctl", {"bootout", domain + "/" + kAgentLabel},
                        std::chrono::milliseconds{5000});
    const auto boot =
        devices::exec("launchctl", {"bootstrap", domain, path}, std::chrono::milliseconds{10000});
    if (boot.exit_code != 0) {
        return err(ErrorCode::BackendFailure,
                   "launchctl bootstrap failed: " + trim(boot.err.empty() ? boot.out : boot.err),
                   "Remove " + path + " and re-run, or start it by hand with:\n  " + command +
                       " --transport http --port " + std::to_string(port));
    }

    // Give it a moment to bind before reporting success, so "installed" and
    // "working" do not drift apart in the output.
    for (int i = 0; i < 20; ++i) {
        if (port_answers(port, token)) return ok();
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
    return err(ErrorCode::Timeout,
               "the service was installed but is not answering on port " + std::to_string(port),
               "Check its log with:\n  launchctl print " + domain + "/" + kAgentLabel);
#endif
}

Status uninstall_agent() {
#if !defined(__APPLE__)
    return err(ErrorCode::Unsupported, "the shared service is macOS-only");
#else
    const std::string path = agent_plist_path();
    const std::string domain = "gui/" + std::to_string(static_cast<int>(::getuid()));
    (void)devices::exec("launchctl", {"bootout", domain + "/" + kAgentLabel},
                        std::chrono::milliseconds{5000});
    if (!path.empty()) std::remove(path.c_str());
    return ok();
#endif
}

bool configure_client_http(const ClientTarget& t, const std::string& server_name,
                           const std::string& url, const std::string& token, std::string* error) {
    if (!t.supports_http) {
        if (error) *error = t.name + " cannot be pointed at an HTTP endpoint from its config";
        return false;
    }

    if (t.toml) {
        // Codex names an environment variable rather than carrying the token,
        // so the secret never lands in the config file - but the variable has
        // to be exported where Codex can see it.
        std::string text = read_file(t.config);
        const std::string header = "[" + t.container + "." + server_name + "]";
        if (text.find(header) != std::string::npos) return true;
        if (!text.empty() && text.back() != '\n') text += "\n";
        if (!text.empty()) text += "\n";
        text += header + "\n";
        text += "url = \"" + url + "\"\n";
        text += "bearer_token_env_var = \"CC_AUTH_TOKEN\"\n";
        return write_file(t.config, text, error);
    }

    json::Value root = json::Value::object();
    const std::string existing = read_file(t.config);
    if (!existing.empty()) {
        json::ParseError pe;
        json::Value parsed = json::parse(existing, &pe);
        if (!pe.ok) {
            if (error) *error = "existing config is not valid JSON (" + pe.message + ")";
            return false;
        }
        if (parsed.is_object()) root = parsed;
    }

    json::Value headers = json::Value::object();
    headers.set("Authorization", "Bearer " + token);

    json::Value entry = json::Value::object();
    entry.set("type", "http");
    entry.set("url", url);
    entry.set("headers", headers);

    json::Value servers = root.contains(t.container) && root[t.container].is_object()
                              ? root[t.container]
                              : json::Value::object();
    servers.set(server_name, entry);
    root.set(t.container, servers);
    return write_file(t.config, root.dump(2) + "\n", error);
}

bool configure_client_bridge(const ClientTarget& t, const std::string& server_name,
                             const std::string& command, const std::string& url,
                             std::string* error) {
    if (t.toml) {
        std::string text = read_file(t.config);
        const std::string header = "[" + t.container + "." + server_name + "]";
        if (text.find(header) != std::string::npos) return true;
        if (!text.empty() && text.back() != '\n') text += "\n";
        if (!text.empty()) text += "\n";
        text += header + "\n";
        text += "command = \"" + command + "\"\n";
        text += "args = [\"bridge\", \"" + url + "\"]\n";
        return write_file(t.config, text, error);
    }

    json::Value root = json::Value::object();
    const std::string existing = read_file(t.config);
    if (!existing.empty()) {
        json::ParseError pe;
        json::Value parsed = json::parse(existing, &pe);
        if (!pe.ok) {
            if (error) *error = "existing config is not valid JSON (" + pe.message + ")";
            return false;
        }
        if (parsed.is_object()) root = parsed;
    }

    json::Value args = json::Value::array();
    args.push_back("bridge");
    args.push_back(url);

    json::Value entry = json::Value::object();
    if (t.needs_type) entry.set("type", "stdio");
    entry.set("command", command);
    entry.set("args", args);
    // No token here on purpose: the bridge reads it from the file the service
    // wrote, so the secret does not get copied into every client's config.

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

    // Saying "granted" without saying who holds it sends people looking in
    // System Settings for a row that is never going to be there. On macOS a
    // grant belongs to the responsible process - the terminal or client that
    // launched this - so the binary's own name does not appear at all.
    const std::string owner = permission_owner();
    if (!owner.empty()) {
        std::cout << "\n  These are " << owner
                  << "'s grants, inherited because it launched this process.\n"
                  << "  computer-control-mcp will not appear in System Settings on its own;\n"
                  << "  that is normal. `computer-control-mcp --doctor` explains it in full.\n";
    }
}

}  // namespace

int run_setup(const SetupOptions& opts_in) {
    SetupOptions opts = opts_in;
    if (opts.command.empty()) opts.command = executable_path();
    if (opts.command.empty()) opts.command = "computer-control-mcp";

    const auto targets = client_targets();

    if (opts.stop) {
        if (auto st = uninstall_agent(); !st) {
            std::cerr << st.error().message << "\n";
            return 1;
        }
        std::cout << "Shared service stopped and removed.\n"
                  << "Client configs still point at it; re-run `setup` to switch them back to\n"
                  << "a copy per client.\n";
        return 0;
    }

    if (opts.status) {
        const AgentStatus st = agent_status();
        std::cout << "Shared service: " << st.detail << "\n";
        if (st.installed) {
            std::cout << "  plist : " << st.plist << "\n";
            if (!st.binary.empty()) std::cout << "  binary: " << st.binary << "\n";
            std::cout << "  url   : http://127.0.0.1:" << st.port << "/mcp\n";
        }
        return st.installed && st.running ? 0 : 1;
    }

    if (opts.restart) {
        const AgentStatus before = agent_status();
        if (!before.installed) {
            std::cerr << "No shared service is installed. Run `setup` first.\n";
            return 1;
        }
        std::string token;
        if (auto st = install_agent(before.binary.empty() ? opts.command : before.binary,
                                    before.port ? before.port : opts.port, &token);
            !st) {
            std::cerr << st.error().message << "\n";
            return 1;
        }
        std::cout << "Restarted on 127.0.0.1:" << (before.port ? before.port : opts.port) << "\n";
        return 0;
    }

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

    // One shared service, always - there is no per-client mode any more.
    //
    // On macOS a permission belongs to the process that launched the server,
    // so a copy spawned by each client needs a grant per client, usually with
    // no prompt to guide it. A launchd job is its own responsible process: one
    // grant, every client. Clients that speak HTTP connect directly; the rest
    // launch `bridge`, which forwards and needs no permission of its own. That
    // removed the last reason to keep two modes.
    //
    // Elsewhere there is no launchd and a stdio child needs no grant at all,
    // so the service would be machinery bought for nothing and clients keep
    // launching the server directly.
#if defined(__APPLE__)
    const bool shared = true;
#else
    const bool shared = false;
#endif

    std::string token;
    const int port = opts.port;
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";

    if (shared) {
        const AgentStatus before = agent_status();
        std::cout << (before.running ? "Shared service already running.\n"
                                     : "Starting the shared service...\n");
        if (auto st = install_agent(opts.command, port, &token); !st) {
            std::cout << "  " << st.error().message << "\n";
            if (!st.error().remedy.empty()) std::cout << "  " << st.error().remedy << "\n";
            return 1;
        }
        std::cout << "  listening on 127.0.0.1:" << port << ", starts again at login\n\n";
    }

    int failures = 0;
    bool any_bridged = false, any_codex = false;
    for (const auto* t : chosen) {
        std::string error;
        bool done = false;
        const char* how = "";
        if (!shared) {
            done = configure_client(*t, opts.server_name, opts.command, &error);
        } else if (t->supports_http) {
            done = configure_client_http(*t, opts.server_name, url, token, &error);
            how = "  (shared service)";
            if (t->toml) any_codex = true;
        } else {
            done = configure_client_bridge(*t, opts.server_name, opts.command, url, &error);
            how = "  (shared service, via bridge)";
            any_bridged = true;
        }

        if (done) {
            std::cout << "  added to " << t->name << how << "  (" << t->config << ")\n";
            if (!t->note.empty()) std::cout << "      " << t->note << "\n";
        } else {
            ++failures;
            std::cout << "  could not update " << t->name << ": " << error << "\n";
        }
    }
    if (!chosen.empty()) std::cout << "\n";

    if (any_bridged) {
        std::cout << "Clients that can only launch a command run `bridge`, which forwards to\n"
                  << "the service. The bridge holds no permissions itself, so the grant below\n"
                  << "still covers them.\n\n";
    }
    if (any_codex) {
        std::cout << "Codex reads its token from the environment rather than its config, so\n"
                  << "add this to your shell profile:\n"
                  << "  export CC_AUTH_TOKEN=$(cat ~/.config/computer-control/token)\n\n";
    }

    if (opts.permissions) {
        std::cout << "Permissions:\n";
        if (shared) {
            // The agent is the process that needs the grant now, and it is a
            // different process from this one - so this process's own state
            // says nothing useful about it.
            std::cout << "  The shared service runs as its own process, so grant the permission\n"
                      << "  to it rather than to any client. It appears in System Settings as\n"
                      << "     computer-control-mcp\n"
                      << "  under Privacy & Security > Accessibility, and again under Screen &\n"
                      << "  System Audio Recording. Add it with + if it is not listed yet.\n\n";
            if (opts.assume_yes || !interactive()) {
                (void)open_permission_settings(Permission::Accessibility);
            } else {
                std::cout << "  Open that pane now? [Y/n] " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer.empty() || answer[0] == 'y' || answer[0] == 'Y') {
                    (void)open_permission_settings(Permission::Accessibility);
                }
            }
            std::cout << "\n  After granting, restart the service so it picks the grant up:\n"
                      << "    computer-control-mcp setup --restart\n\n";
            std::cout << "Check anything later with:\n"
                      << "  computer-control-mcp setup --status    is the service running?\n"
                      << "  computer-control-mcp setup --stop      remove it\n"
                      << "  computer-control-mcp --doctor          what this host can do\n";
            return failures == 0 ? 0 : 1;
        }
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
