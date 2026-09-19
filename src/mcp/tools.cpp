// SPDX-License-Identifier: MIT
#include <algorithm>

#include "actions/actions.hpp"
#include "mcp/server.hpp"

namespace cc::mcp {

const char* server_instructions() {
    return "computer-control drives this machine's desktop, and any iOS simulator, Android "
           "emulator or mirrored phone visible on it.\n"
           "\n"
           "Start with `capabilities` on an unfamiliar host. It reports display scale factors, "
           "which permissions are missing, and - importantly - which multi-touch gestures are "
           "real versus emulated on this platform.\n"
           "\n"
           "Coordinates. Every point carries a space:\n"
           "  logical  (default) OS points, what clicks use.\n"
           "  physical device pixels; on a 2x Retina display these are double the logical values.\n"
           "  image    pixels of the screenshot you were last handed.\n"
           "Read a position off a screenshot, pass it back as "
           "{\"x\":..,\"y\":..,\"space\":\"image\"}, and it is converted for you. Passing raw "
           "screenshot pixels as logical coordinates is the single most common way to click the "
           "wrong thing.\n"
           "\n"
           "Prefer labels to pixels. `snapshot` numbers every interactive element; `click`, "
           "`type` and `scroll` accept `label`. Labels survive scrolling and window movement in a "
           "way coordinates do not.\n"
           "\n"
           "Batch. Each call costs a model round trip that dwarfs the action itself. When you can "
           "predict a sequence - click a field, type, press Return - send it as one `batch`.\n"
           "\n"
           "After an interrupted drag, call `release_all` so no button is left held.";
}

json::Value tool_definitions(const ServerConfig& cfg) {
    json::Value tools = json::Value::array();

    for (const auto& spec : actions::registry()) {
        const std::string name = spec.name;

        if (!cfg.enabled_tools.empty()) {
            if (std::find(cfg.enabled_tools.begin(), cfg.enabled_tools.end(), name) ==
                cfg.enabled_tools.end()) {
                continue;
            }
        }
        if (std::find(cfg.disabled_tools.begin(), cfg.disabled_tools.end(), name) !=
            cfg.disabled_tools.end()) {
            continue;
        }
        // Do not advertise capabilities the session has switched off: a tool
        // that always answers "disabled" wastes the model's attention.
        if (name == "shell" && !cfg.session.allow_shell) continue;
        if (name == "registry" && !cfg.session.allow_registry) continue;
        if (name == "clipboard" && !cfg.session.allow_clipboard) continue;

        json::ParseError pe;
        json::Value schema = json::parse(spec.schema_json, &pe);
        if (!pe.ok) {
            // A malformed built-in schema is a programming error, but shipping
            // a permissive schema beats dropping the tool at runtime.
            schema = json::Value::object();
            schema.set("type", "object");
        }

        json::Value tool = json::Value::object();
        tool.set("name", name);
        tool.set("title", spec.title);
        tool.set("description", spec.description);
        tool.set("inputSchema", schema);

        json::Value ann = json::Value::object();
        ann.set("title", spec.title);
        ann.set("readOnlyHint", spec.read_only);
        ann.set("destructiveHint", spec.destructive);
        ann.set("idempotentHint", spec.read_only);
        // openWorldHint means the tool can reach entities outside this
        // machine. Almost everything here drives the local desktop and cannot,
        // but `shell` runs whatever it is given - it can fetch from the
        // network or install software - and `device` talks to a simulator,
        // emulator or handset that is a separate system. Claiming a closed
        // world for those two would understate what a client is approving.
        ann.set("openWorldHint", name == "shell" || name == "device");
        tool.set("annotations", ann);

        tools.push_back(tool);
    }
    return tools;
}

}  // namespace cc::mcp
