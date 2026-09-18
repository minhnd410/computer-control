// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cc/types.hpp"

namespace cc {

struct ClipboardContent {
    std::string text;
    std::vector<std::uint8_t> image_png;
    std::vector<std::string> file_paths;
    bool has_text = false;
    bool has_image = false;
    bool has_files = false;
};

struct ProcessInfo {
    std::int64_t pid = 0;
    std::int64_t ppid = 0;
    std::string name;
    std::string command;
    double cpu_percent = 0;
    std::int64_t memory_bytes = 0;
    std::string user;
};

struct ShellRequest {
    std::string command;
    std::string shell;  // "" = platform default (zsh/bash, powershell)
    std::string cwd;
    std::map<std::string, std::string> env;
    std::chrono::milliseconds timeout{30000};
    bool capture_stderr = true;
};

struct ShellResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    std::chrono::milliseconds elapsed{0};
};

struct NotificationRequest {
    std::string title = "computer-control";
    std::string subtitle;
    std::string message;
    std::string sound;
};

class SystemBackend {
public:
    virtual ~SystemBackend() = default;

    static Result<std::unique_ptr<SystemBackend>> create();

    virtual std::string name() const = 0;
    virtual Status initialize() = 0;

    virtual Result<ClipboardContent> clipboard_get() = 0;
    virtual Status clipboard_set(const ClipboardContent& c) = 0;

    virtual Result<std::vector<ProcessInfo>> list_processes() = 0;
    virtual Status kill_process(std::int64_t pid, bool force) = 0;

    virtual Result<ShellResult> run_shell(const ShellRequest& req) = 0;
    virtual Status notify(const NotificationRequest& req) = 0;

    // Windows only; every other platform returns Unsupported.
    virtual Result<std::string> registry_get(std::string_view path, std::string_view name);
    virtual Status registry_set(std::string_view path, std::string_view name,
                                std::string_view value, std::string_view type);
    virtual Status registry_delete(std::string_view path, std::string_view name);
    virtual Result<std::vector<std::string>> registry_list(std::string_view path);
};

}  // namespace cc
