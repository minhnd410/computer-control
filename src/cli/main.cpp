// SPDX-License-Identifier: MIT
//
// `cc` - the command-line front end.
//
// Every subcommand maps onto the same action dispatcher the MCP server uses,
// so the CLI is a thin argv-to-JSON translator. That keeps behaviour identical
// between "cc click --at 100,200" and the MCP `click` tool, and makes the CLI
// a usable debugging tool for the server.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "capi/actions.hpp"
#include "cc/permissions.hpp"
#include "cc/session.hpp"
#include "core/json.hpp"

using cc::json::Value;

namespace {

// Which OS permissions an action actually needs. Checking before running lets
// the tool explain a missing grant up front, rather than after a click has
// silently gone nowhere - which on macOS is the default failure mode, because
// synthetic input into an ungranted process is discarded without an error.
std::vector<cc::Permission> permissions_for(const std::string& action) {
    static const std::set<std::string> kNeedsScreen = {"screenshot", "snapshot", "zoom"};
    static const std::set<std::string> kNeedsAccessibility = {
        "snapshot", "elements", "wait_for", "click",  "type",           "key",
        "key_hold", "key_down", "key_up",   "move",   "scroll",         "drag",
        "stroke",   "gesture",  "windows",  "device", "cursor_position"};

    std::vector<cc::Permission> out;
    if (kNeedsScreen.count(action)) out.push_back(cc::Permission::ScreenRecording);
    if (kNeedsAccessibility.count(action)) out.push_back(cc::Permission::Accessibility);
    return out;
}

// Prints a warning for anything missing. Returns true if something was wrong.
bool warn_about_permissions(const std::string& action) {
    bool warned = false;
    for (cc::Permission needed : permissions_for(action)) {
        const cc::PermissionStatus status = cc::check_permission(needed);
        if (status.state == cc::PermissionState::Granted ||
            status.state == cc::PermissionState::NotRequired) {
            continue;
        }
        if (!warned) std::cerr << "\n";
        std::cerr << "cc: " << cc::to_string(needed) << " is " << cc::to_string(status.state)
                  << " - `" << action << "` will not work correctly.\n";
        if (!status.detail.empty()) std::cerr << "    " << status.detail << "\n";
        if (!status.remedy.empty()) {
            std::istringstream lines(status.remedy);
            std::string line;
            while (std::getline(lines, line)) std::cerr << "    " << line << "\n";
        }
        std::cerr << "\n";
        warned = true;
    }
    return warned;
}

void print_usage() {
    std::cout << R"(cc - control this computer from the shell

USAGE
  cc <command> [--key value ...]
  cc <command> --json '{"...": ...}'

COMMANDS
)";
    for (const auto& spec : cc::actions::registry()) {
        std::printf("  %-16s %s\n", spec.name, std::string(spec.description).substr(0, 78).c_str());
    }
    std::cout << R"(
GLOBAL OPTIONS
  --json JSON        Pass arguments as a JSON object instead of flags.
  --out FILE         Write an image result to FILE instead of discarding it.
  --raw              Print the raw JSON result rather than the text summary.
  --no-shell         Disable the shell action for this invocation.
  --help, --version

ARGUMENT FORMS
  Points accept "x,y" and "x,y@image" (or @physical).
  Rects accept "x,y,w,h".
  Booleans accept true/false; a bare flag means true.

EXAMPLES
  cc permissions              # what the OS is allowing, and how to fix it
  cc permissions --request    # prompt for anything missing
  cc capabilities
  cc displays
  cc screenshot --out screen.png --max_dimension 1200
  cc snapshot --raw | jq '.result.elements[] | select(.role=="button")'
  cc click --at 640,480
  cc click --label 7
  cc type --text "hello" --enter
  cc gesture --kind pinch --scale 2 --at 700,400
  cc drag --from 100,100 --to 400,400 --profile human
  cc device --mode list
  cc device --mode tap --device "iPhone 15" --at 196,420
  cc batch --json '[{"action":"click","at":[10,10]},{"action":"type","text":"hi"}]'
)";
}

// Turns --key value pairs into a JSON object, inferring types so a caller does
// not have to quote everything. A value that parses as JSON is used as-is,
// which is what makes --at 100,200 and --at '[100,200]' both work.
Value args_to_json(const std::vector<std::string>& argv, std::size_t start, std::string* out_file,
                   bool* raw, bool* no_shell, std::string* error) {
    Value obj = Value::object();
    for (std::size_t i = start; i < argv.size(); ++i) {
        std::string key = argv[i];
        if (key.rfind("--", 0) != 0) {
            *error = "expected --option, got '" + key + "'";
            return obj;
        }
        key = key.substr(2);

        if (key == "raw") {
            *raw = true;
            continue;
        }
        if (key == "no-shell") {
            *no_shell = true;
            continue;
        }
        if (key == "out") {
            if (i + 1 >= argv.size()) {
                *error = "--out needs a path";
                return obj;
            }
            *out_file = argv[++i];
            continue;
        }
        if (key == "json") {
            if (i + 1 >= argv.size()) {
                *error = "--json needs a value";
                return obj;
            }
            cc::json::ParseError pe;
            Value parsed = cc::json::parse(argv[++i], &pe);
            if (!pe.ok) {
                *error = "--json is not valid JSON: " + pe.message;
                return obj;
            }
            if (parsed.is_object()) {
                for (const auto& [k, v] : parsed.as_object()) obj.set(k, v);
            } else {
                // A bare array is the natural form for `cc batch`.
                obj.set("actions", parsed);
            }
            continue;
        }

        // A flag with no following value, or followed by another flag, is a
        // boolean true.
        if (i + 1 >= argv.size() || argv[i + 1].rfind("--", 0) == 0) {
            obj.set(key, true);
            continue;
        }

        const std::string raw_value = argv[++i];
        if (raw_value == "true") {
            obj.set(key, true);
            continue;
        }
        if (raw_value == "false") {
            obj.set(key, false);
            continue;
        }

        // Numbers pass through as numbers so schemas that expect them match.
        char* end = nullptr;
        const double as_number = std::strtod(raw_value.c_str(), &end);
        if (end && *end == '\0' && !raw_value.empty()) {
            obj.set(key, as_number);
            continue;
        }

        if (!raw_value.empty() && (raw_value[0] == '[' || raw_value[0] == '{')) {
            cc::json::ParseError pe;
            Value parsed = cc::json::parse(raw_value, &pe);
            if (pe.ok) {
                obj.set(key, parsed);
                continue;
            }
        }
        obj.set(key, raw_value);
    }
    return obj;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);

    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h" || args[1] == "help") {
        print_usage();
        return args.size() < 2 ? 2 : 0;
    }
    if (args[1] == "--version") {
        const auto b = cc::build_info();
        std::cout << "cc " << b.version << " (" << b.platform << ", " << b.compiler << ")\n";
        return 0;
    }

    const std::string command = args[1];

    if (command == "doctor") {
        // `doctor` is capabilities with a human-facing framing; it is the
        // first thing to run when something is not working.
        auto s = cc::Session::create({});
        if (!s) {
            std::cerr << "Cannot start a session: " << s.error().message << "\n";
            if (!s.error().remedy.empty()) std::cerr << "\n" << s.error().remedy << "\n";
            return 1;
        }
        const std::string guidance = cc::permission_guidance();
        if (guidance.empty()) {
            std::cout << "Permissions: all granted.\n\n";
        } else {
            std::cout << "Permissions need attention:\n\n" << guidance << "\n";
        }
        std::cout << s.value()->capability_report() << "\n";
        return 0;
    }

    if (!cc::actions::find_spec(command)) {
        std::cerr << "cc: unknown command '" << command << "'\n\n";
        print_usage();
        return 2;
    }

    std::string out_file;
    bool raw = false, no_shell = false, error_set = false;
    std::string error;
    Value action_args = args_to_json(args, 2, &out_file, &raw, &no_shell, &error);
    if (!error.empty()) {
        std::cerr << "cc: " << error << "\n";
        return 2;
    }
    (void)error_set;

    cc::SessionConfig cfg;
    if (no_shell) cfg.allow_shell = false;
    cfg.allow_registry = true;  // the CLI is run by the user directly

    auto session = cc::Session::create(cfg);
    if (!session) {
        std::cerr << "cc: " << session.error().message << "\n";
        if (!session.error().remedy.empty()) std::cerr << "\n" << session.error().remedy << "\n";
        return 1;
    }

    // Warn before acting, not after: a click that lands nowhere looks like a
    // coordinate bug, and the user has no way to tell the difference.
    if (command != "permissions" && command != "capabilities") {
        warn_about_permissions(command);
    }

    auto result = cc::actions::run(*session.value(), command, action_args);

    if (!result.image.empty()) {
        if (!out_file.empty()) {
            std::ofstream f(out_file, std::ios::binary);
            if (!f) {
                std::cerr << "cc: cannot write " << out_file << "\n";
                return 1;
            }
            f.write(reinterpret_cast<const char*>(result.image.data()),
                    static_cast<std::streamsize>(result.image.size()));
            std::cerr << "Wrote " << result.image.size() << " bytes to " << out_file << "\n";
        } else if (!raw) {
            std::cerr << "(image result discarded; pass --out FILE to save it)\n";
        }
    }

    if (raw) {
        Value out = Value::object();
        out.set("ok", result.ok);
        out.set("text", result.text);
        out.set("result", result.value);
        if (!result.ok) {
            out.set("error", result.error.message);
            out.set("code", cc::to_string(result.error.code));
            if (!result.error.remedy.empty()) out.set("remedy", result.error.remedy);
        }
        std::cout << out.dump(2) << "\n";
    } else if (result.ok) {
        if (!result.text.empty()) std::cout << result.text << "\n";
    } else {
        std::cerr << "cc: " << result.error.message << "\n";
        if (!result.error.remedy.empty()) std::cerr << "\n" << result.error.remedy << "\n";
    }

    return result.ok ? 0 : 1;
}
