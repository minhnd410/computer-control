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
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
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

std::string ask_service(const std::string& tool) {
    const AgentStatus st = agent_status();
    if (!st.installed || st.port == 0) return {};

    const std::string body = std::string(R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)") +
                             R"("params":{"name":")" + tool + R"(","arguments":{},"_meta":{)" +
                             R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)" +
                             R"("io.modelcontextprotocol/clientCapabilities":{}}}})";

    std::vector<std::string> args{
        "-fsS", "-m", "8", "-X", "POST", "http://127.0.0.1:" + std::to_string(st.port) + "/mcp"};
    const std::string token = agent_token();
    if (!token.empty()) {
        args.push_back("-H");
        args.push_back("Authorization: Bearer " + token);
    }
    args.push_back("-d");
    args.push_back(body);

    const auto r = devices::exec("curl", args, std::chrono::milliseconds{10000});
    if (r.exit_code != 0 || r.out.empty()) return {};

    json::ParseError pe;
    const json::Value v = json::parse(r.out, &pe);
    if (!pe.ok) return {};
    const json::Value& content = v["result"]["content"];
    if (!content.is_array() || content.size() == 0) return {};
    return content[0]["text"].as_string();
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

// Colour, but only when it is going to a terminal.
//
// Escape codes in a redirected stream turn a log into line noise, and NO_COLOR
// is the convention for people who do not want them at all. This is checked
// once: the answer cannot change while the process runs, and calling isatty on
// every write is silly.
bool color_enabled() {
    static const bool on = [] {
        if (std::getenv("NO_COLOR") != nullptr) return false;
        const char* term = std::getenv("TERM");
        if (term != nullptr && std::string(term) == "dumb") return false;
        return cc_isatty(cc_fileno(stdout)) != 0;
    }();
    return on;
}

std::string sgr(const char* code, const std::string& text) {
    // Styling an empty string emits a pair of escapes around nothing, which is
    // invisible until someone reads the raw output and wonders what broke.
    if (text.empty() || !color_enabled()) return text;
    return std::string("\x1b[") + code + "m" + text + "\x1b[0m";
}

std::string bold(const std::string& t) {
    return sgr("1", t);
}
std::string dim(const std::string& t) {
    return sgr("2", t);
}
std::string green(const std::string& t) {
    return sgr("32", t);
}
std::string red(const std::string& t) {
    return sgr("31", t);
}
std::string yellow(const std::string& t) {
    return sgr("33", t);
}
std::string cyan(const std::string& t) {
    return sgr("36", t);
}

// Marks degrade to ASCII wherever colour is off, which is also where a glyph
// is least likely to render.
const char* mark_ok() {
    return color_enabled() ? "✓" : "ok";
}
const char* mark_bad() {
    return color_enabled() ? "✗" : "!!";
}

void heading(const std::string& text) {
    std::cout << "\n" << bold(text) << "\n";
}

// An arrow-key checkbox picker.
//
// Typing "1 3 4" works but makes you hold the mapping in your head while you
// read the list. This shows the state you are choosing.
//
// The terminal is put in raw mode to read single keypresses, which is the
// dangerous part: leaving it that way gives the user a shell with no echo and
// no line editing, and they have to blind-type `reset`. RawMode restores it
// from its destructor on every path, and ISIG stays disabled so Ctrl-C arrives
// as a byte we handle rather than a signal that kills us mid-mode.
#if !defined(_WIN32)
class RawMode {
public:
    RawMode() {
        if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        ok_ = true;
        termios raw = saved_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~RawMode() {
        if (ok_) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;
    bool ok() const { return ok_; }

private:
    termios saved_{};
    bool ok_ = false;
};

// Returns the chosen indices, or nullopt when the picker cannot run (no raw
// mode, a terminal that cannot do escapes) so the caller falls back.
std::optional<std::vector<std::size_t>> pick_clients(
    const std::vector<const ClientTarget*>& found) {
    const char* term = std::getenv("TERM");
    if (term && std::string(term) == "dumb") return std::nullopt;

    RawMode raw;
    if (!raw.ok()) return std::nullopt;

    // Everything starts selected: the common case is wanting all of them, and
    // an empty list would make Enter mean "do nothing", which reads as broken.
    std::vector<bool> chosen(found.size(), true);
    std::size_t cursor = 0;
    bool first = true;

    auto draw = [&]() {
        if (!first) std::cout << "\x1b[" << (found.size() + 2) << "A";
        first = false;
        std::cout << "\x1b[?25l";  // hide the cursor while redrawing
        for (std::size_t i = 0; i < found.size(); ++i) {
            const std::string box = chosen[i] ? green("[x]") : dim("[ ]");
            std::cout << "\x1b[2K" << (i == cursor ? "  " + cyan(">") + " " : "    ") << box << " "
                      << (i == cursor ? bold(found[i]->name) : found[i]->name) << "\n";
        }
        std::cout << "\x1b[2K\n"
                  << "\x1b[2K  " << dim("space toggles, a all, enter confirms, esc cancels") << "\n"
                  << std::flush;
    };

    draw();
    for (;;) {
        char c = 0;
        if (::read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == '\r' || c == '\n') break;
        if (c == 3 || c == 'q') {  // Ctrl-C
            std::cout << "\x1b[?25h" << std::flush;
            return std::vector<std::size_t>{};
        }
        if (c == ' ') {
            chosen[cursor] = !chosen[cursor];
        } else if (c == 'a' || c == 'A') {
            const bool all = std::all_of(chosen.begin(), chosen.end(), [](bool b) { return b; });
            std::fill(chosen.begin(), chosen.end(), !all);
        } else if (c == 'j') {
            cursor = (cursor + 1) % found.size();
        } else if (c == 'k') {
            cursor = (cursor + found.size() - 1) % found.size();
        } else if (c == 27) {
            // Either a bare Escape, or the start of an arrow sequence. VMIN=1
            // blocks, so peek with a short poll rather than waiting forever on
            // a lone Escape keypress.
            char seq[2] = {0, 0};
            fd_set set;
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            timeval tv{0, 50000};
            if (::select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) <= 0) {
                std::cout << "\x1b[?25h" << std::flush;
                return std::vector<std::size_t>{};
            }
            if (::read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (::read(STDIN_FILENO, &seq[1], 1) != 1) break;
            if (seq[0] == '[' && seq[1] == 'B') cursor = (cursor + 1) % found.size();
            if (seq[0] == '[' && seq[1] == 'A') cursor = (cursor + found.size() - 1) % found.size();
        }
        draw();
    }

    std::cout << "\x1b[?25h" << std::flush;
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        if (chosen[i]) out.push_back(i);
    }
    return out;
}
#endif

bool interactive_terminal_impl();

}  // namespace

bool interactive_terminal() {
    return interactive_terminal_impl();
}

namespace {

bool interactive_terminal_impl() {
    return cc_isatty(cc_fileno(stdin)) && cc_isatty(cc_fileno(stdout));
}

bool interactive() {
    return cc_isatty(cc_fileno(stdin)) && cc_isatty(cc_fileno(stdout));
}

void print_permission_summary() {
    const auto states = check_permissions();
    for (const auto& st : states) {
        if (st.state == PermissionState::NotRequired) continue;
        const bool granted = st.state == PermissionState::Granted;
        std::cout << "  " << (granted ? green(mark_ok()) : yellow(mark_bad())) << " "
                  << to_string(st.permission) << "  "
                  << (granted ? dim(to_string(st.state)) : yellow(to_string(st.state))) << "\n";
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
        std::cout << "\n  " << green(mark_ok()) << " shared service stopped and removed\n"
                  << "    " << dim("client configs still point at it; re-run `setup` to reconnect")
                  << "\n\n";
        return 0;
    }

    if (opts.status) {
        const AgentStatus st = agent_status();
        heading("Shared service");
        const std::string mark =
            st.running ? green(mark_ok()) : (st.installed ? yellow(mark_bad()) : dim(mark_bad()));
        std::cout << "  " << mark << " " << (st.running ? green(st.detail) : yellow(st.detail))
                  << "\n";
        if (st.installed) {
            std::cout << "    " << dim("url    ")
                      << cyan("http://127.0.0.1:" + std::to_string(st.port) + "/mcp") << "\n";
            if (!st.binary.empty()) std::cout << "    " << dim("binary " + st.binary) << "\n";
            std::cout << "    " << dim("plist  " + st.plist) << "\n";
        }
        std::cout << "\n";
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
        heading("Clients this can configure");
        for (const auto& t : targets) {
            const bool here = client_installed(t);
            std::cout << "  " << (here ? green(mark_ok()) : dim(mark_bad())) << " " << bold(t.id)
                      << dim(here ? "" : "  (not found)") << "\n"
                      << "    " << t.name
                      << dim(t.supports_http ? "  \u00b7 http" : "  \u00b7 stdio") << "\n    "
                      << dim(t.config) << "\n";
        }
        std::cout << "\n  " << dim("Configure one with") << "  " << cyan("setup --client <id>")
                  << "\n\n";
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
            heading("Clients");
            std::cout << "  " << yellow("none detected") << "\n\n"
                      << "  Add the server by hand with:\n    " << cyan(opts.command) << "\n\n"
                      << dim("  `setup --list` shows where each client keeps its config.")
                      << "\n\n";
        } else if (!interactive()) {
            // A pipe or a CI job cannot answer a prompt, and silently editing
            // someone's editor config because they ran this non-interactively
            // would be worse than doing nothing.
            std::cout << "Detected these MCP clients:\n\n";
            for (const auto* t : found) std::cout << "  " << t->id << "  (" << t->name << ")\n";
            std::cout << "\nNot a terminal, so nothing was changed. Re-run interactively, or:\n"
                      << "  computer-control-mcp setup --client " << found.front()->id << "\n\n";
        } else {
            heading("Clients");
            std::cout << dim("  Which should get computer-control?") << "\n\n";

            bool picked = false;
#if !defined(_WIN32)
            if (auto selection = pick_clients(found)) {
                for (std::size_t i : *selection) chosen.push_back(found[i]);
                picked = true;
            }
#endif
            if (!picked) {
                // No raw mode, or a terminal that cannot do escapes. Typing
                // numbers is worse but it always works.
                for (std::size_t i = 0; i < found.size(); ++i) {
                    std::cout << "  " << (i + 1) << ") " << found[i]->name << "\n";
                }
                std::cout << "\nEnter numbers separated by spaces, 'a' for all, or Enter to "
                             "skip: "
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
            }
            std::cout << "\n";
            if (chosen.empty()) {
                std::cout << "Nothing selected; no client configs were changed.\n\n";
            }
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

    // Selecting nothing means "not now". Installing a background service that
    // can drive the desktop, for zero clients, is not what that asked for.
    if (chosen.empty() && opts.clients.empty()) {
        std::cout << "Run `computer-control-mcp setup` again when you want to connect a client.\n";
        return 0;
    }

    if (shared) {
        const AgentStatus before = agent_status();
        heading("Service");
        if (auto st = install_agent(opts.command, port, &token); !st) {
            std::cout << "  " << red(mark_bad()) << " " << st.error().message << "\n";
            if (!st.error().remedy.empty()) {
                std::cout << "    " << dim(st.error().remedy) << "\n";
            }
            return 1;
        }
        std::cout << "  " << green(mark_ok()) << " "
                  << (before.running ? "already running" : "started") << "  "
                  << cyan("http://127.0.0.1:" + std::to_string(port) + "/mcp") << "\n"
                  << "    " << dim("runs in the background and starts again at login") << "\n";
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
            std::cout << "  " << green(mark_ok()) << " " << t->name << dim(how) << "\n"
                      << "    " << dim(t->config) << "\n";
            if (!t->note.empty()) std::cout << "    " << yellow("note") << " " << t->note << "\n";
        } else {
            ++failures;
            std::cout << "  " << red(mark_bad()) << " " << t->name << "  " << red(error) << "\n";
        }
    }
    if (!chosen.empty()) std::cout << "\n";

    if (any_bridged) {
        std::cout << dim("  Clients that can only launch a command run `bridge`, which forwards\n"
                         "  to the service. The bridge holds no permissions of its own, so the\n"
                         "  grant below still covers them.")
                  << "\n\n";
    }
    if (any_codex) {
        std::cout << "  " << yellow("Codex")
                  << dim(" reads its token from the environment, so add this to your shell:")
                  << "\n    "
                  << cyan("export CC_AUTH_TOKEN=$(cat ~/.config/computer-control/token)") << "\n\n";
    }

    if (opts.permissions) {
        heading("Permissions");
        if (shared) {
            // The agent is the process that needs the grant now, and it is a
            // different process from this one - so this process's own state
            // says nothing useful about it.
            std::cout << dim("  The service is its own process, so the permission goes to it\n"
                             "  rather than to any client.")
                      << "\n\n  Enable " << bold("computer-control-mcp") << " under\n"
                      << "    " << cyan("Privacy & Security \u203a Accessibility") << "\n"
                      << "    " << cyan("Privacy & Security \u203a Screen & System Audio Recording")
                      << "\n"
                      << dim("  Add it with + if it is not listed yet.") << "\n\n";
            if (opts.assume_yes || !interactive()) {
                (void)open_permission_settings(Permission::Accessibility);
            } else {
                std::cout << "  Open that pane now? " << dim("[Y/n]") << " " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer.empty() || answer[0] == 'y' || answer[0] == 'Y') {
                    (void)open_permission_settings(Permission::Accessibility);
                }
            }
            std::cout << "\n  " << yellow("The grant is read at launch")
                      << dim(", so restart the service afterwards:") << "\n    "
                      << cyan("computer-control-mcp setup --restart") << "\n";

            heading("Later");
            std::cout << "  " << cyan("setup --status") << dim("   is the service running?") << "\n"
                      << "  " << cyan("setup --stop") << dim("     remove it") << "\n"
                      << "  " << cyan("--doctor") << dim("         what this host can do")
                      << "\n\n";
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

    heading("Later");
    std::cout << "  " << cyan("--doctor") << dim("      what this host can do") << "\n"
              << "  " << cyan("setup --list") << dim("  where each client keeps its config") << "\n"
              << "  " << cyan("setup") << dim("         run this again") << "\n\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace cc::mcp
