// SPDX-License-Identifier: MIT
#include "mcp/config.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/json.hpp"

namespace cc::mcp {
namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : std::string{};
}

std::string home() {
#if defined(_WIN32)
    const std::string user = env("USERPROFILE");
    return user.empty() ? env("HOMEDRIVE") + env("HOMEPATH") : user;
#else
    return env("HOME");
#endif
}

std::string parent_dir(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool directory_exists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    return _access(path.c_str(), 0) == 0;
#else
    return ::access(path.c_str(), F_OK) == 0;
#endif
}

bool make_parent_dirs(const std::string& path) {
    const std::string dir = parent_dir(path);
    if (dir.empty() || directory_exists(dir)) return true;

    std::string current;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        current += dir[i];
        const bool separator = dir[i] == '/' || dir[i] == '\\';
        if (!separator && i + 1 != dir.size()) continue;
        const std::string part = separator ? current.substr(0, current.size() - 1) : current;
        if (part.empty()) continue;
#if defined(_WIN32)
        _mkdir(part.c_str());
#else
        ::mkdir(part.c_str(), 0755);
#endif
    }
    return directory_exists(dir);
}

Error config_error(const std::string& path, const std::string& message) {
    return err(ErrorCode::InvalidArgument, "invalid server config " + path + ": " + message,
               "Fix the JSON or remove the file and run `computer-control-mcp setup` again.");
}

bool read_string(const json::Value& object, const char* key, std::string* out,
                 const std::string& path, Error* error) {
    if (!object.contains(key)) return true;
    if (!object[key].is_string()) {
        *error = config_error(path, std::string("'") + key + "' must be a string");
        return false;
    }
    *out = object[key].as_string();
    return true;
}

bool read_bool(const json::Value& object, const char* key, bool* out, const std::string& path,
               Error* error) {
    if (!object.contains(key)) return true;
    if (!object[key].is_bool()) {
        *error = config_error(path, std::string("'") + key + "' must be a boolean");
        return false;
    }
    *out = object[key].as_bool();
    return true;
}

bool read_int(const json::Value& object, const char* key, int* out, const std::string& path,
              Error* error) {
    if (!object.contains(key)) return true;
    if (!object[key].is_number()) {
        *error = config_error(path, std::string("'") + key + "' must be a number");
        return false;
    }
    *out = static_cast<int>(object[key].as_int());
    return true;
}

bool read_string_array(const json::Value& object, const char* key,
                       std::vector<std::string>* out, const std::string& path, Error* error) {
    if (!object.contains(key)) return true;
    if (!object[key].is_array()) {
        *error = config_error(path, std::string("'") + key + "' must be an array of strings");
        return false;
    }
    out->clear();
    for (const auto& item : object[key].as_array()) {
        if (!item.is_string()) {
            *error = config_error(path, std::string("'") + key + "' must contain only strings");
            return false;
        }
        out->push_back(item.as_string());
    }
    return true;
}

bool validate_server(ServerConfig* cfg, const std::string& path, Error* error) {
    if (cfg->transport != "stdio" && cfg->transport != "http") {
        *error = config_error(path, "'transport' must be 'stdio' or 'http'");
        return false;
    }
    if (cfg->port < 1 || cfg->port > 65535) {
        *error = config_error(path, "'port' must be between 1 and 65535");
        return false;
    }
    if (cfg->session.default_max_capture_dimension < 0) {
        *error = config_error(path, "'default_max_capture_dimension' cannot be negative");
        return false;
    }
    return true;
}

}  // namespace

std::string default_server_config_path() {
#if defined(_WIN32)
    std::string base = env("APPDATA");
    if (base.empty()) base = home() + "/AppData/Roaming";
#else
    std::string base = env("XDG_CONFIG_HOME");
    if (base.empty()) base = home() + "/.config";
#endif
    return base + "/computer-control/config.json";
}

Result<ServerConfig> load_server_config(const std::string& requested_path) {
    const std::string path = requested_path.empty() ? default_server_config_path() : requested_path;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (errno == ENOENT) return ServerConfig{};
        return config_error(path, "cannot read the file");
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    json::ParseError parse_error;
    const json::Value root = json::parse(contents.str(), &parse_error);
    if (!parse_error.ok || !root.is_object()) {
        return config_error(path, parse_error.ok ? "top level must be an object"
                                                 : parse_error.message);
    }

    ServerConfig cfg;
    const json::Value& server = root["server"];
    const json::Value& session = root["session"];
    if (!server.is_null() && !server.is_object()) {
        return config_error(path, "'server' must be an object");
    }
    if (!session.is_null() && !session.is_object()) {
        return config_error(path, "'session' must be an object");
    }

    Error error;
    if (server.is_object()) {
        if (!read_string(server, "transport", &cfg.transport, path, &error) ||
            !read_string(server, "host", &cfg.host, path, &error) ||
            !read_string(server, "auth_token", &cfg.auth_token, path, &error) ||
            !read_int(server, "port", &cfg.port, path, &error) ||
            !read_bool(server, "log_requests", &cfg.log_requests, path, &error) ||
            !read_string_array(server, "enabled_tools", &cfg.enabled_tools, path, &error) ||
            !read_string_array(server, "disabled_tools", &cfg.disabled_tools, path, &error)) {
            return error;
        }
    }
    if (session.is_object()) {
        if (!read_bool(session, "allow_shell", &cfg.session.allow_shell, path, &error) ||
            !read_bool(session, "allow_filesystem", &cfg.session.allow_filesystem, path, &error) ||
            !read_bool(session, "allow_registry", &cfg.session.allow_registry, path, &error) ||
            !read_bool(session, "allow_clipboard", &cfg.session.allow_clipboard, path, &error) ||
            !read_bool(session, "block_when_locked", &cfg.session.block_when_locked, path, &error) ||
            !read_bool(session, "prompt_for_permissions", &cfg.session.prompt_for_permissions,
                       path, &error) ||
            !read_int(session, "default_max_capture_dimension",
                      &cfg.session.default_max_capture_dimension, path, &error)) {
            return error;
        }
    }

    if (!validate_server(&cfg, path, &error)) return error;
    return cfg;
}

Status save_server_config(const ServerConfig& cfg, const std::string& requested_path) {
    const std::string path = requested_path.empty() ? default_server_config_path() : requested_path;
    Error error;
    ServerConfig checked = cfg;
    if (!validate_server(&checked, path, &error)) return error;
    if (!make_parent_dirs(path)) {
        return err(ErrorCode::IoError, "cannot create the directory for " + path);
    }

    json::Value server = json::Value::object();
    server.set("transport", checked.transport);
    server.set("host", checked.host);
    server.set("port", checked.port);
    server.set("auth_token", checked.auth_token);
    server.set("log_requests", checked.log_requests);
    json::Value enabled = json::Value::array();
    for (const auto& tool : checked.enabled_tools) enabled.push_back(tool);
    server.set("enabled_tools", enabled);
    json::Value disabled = json::Value::array();
    for (const auto& tool : checked.disabled_tools) disabled.push_back(tool);
    server.set("disabled_tools", disabled);

    json::Value session = json::Value::object();
    session.set("allow_shell", checked.session.allow_shell);
    session.set("allow_filesystem", checked.session.allow_filesystem);
    session.set("allow_registry", checked.session.allow_registry);
    session.set("allow_clipboard", checked.session.allow_clipboard);
    session.set("block_when_locked", checked.session.block_when_locked);
    session.set("prompt_for_permissions", checked.session.prompt_for_permissions);
    session.set("default_max_capture_dimension", checked.session.default_max_capture_dimension);

    json::Value root = json::Value::object();
    root.set("server", server);
    root.set("session", session);

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return err(ErrorCode::IoError, "cannot write " + path);
        output << root.dump(2) << "\n";
        if (!output.good()) return err(ErrorCode::IoError, "cannot write " + path);
    }
#if !defined(_WIN32)
    ::chmod(path.c_str(), 0600);
#endif
    return ok();
}

}  // namespace cc::mcp