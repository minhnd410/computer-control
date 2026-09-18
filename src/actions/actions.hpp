// SPDX-License-Identifier: MIT
#pragma once

// The single action dispatcher.
//
// Two front-ends need the same behaviour: the MCP server and the CLI. Rather
// than implement argument parsing, coordinate resolution and error shaping
// twice (and drift), each action is defined once here as JSON in / JSON out.
// The MCP tool layer is then a thin schema wrapper, and `cc click --at
// 100,200` is a thin argv-to-JSON wrapper.

#include <string>
#include <vector>

#include "cc/session.hpp"
#include "core/json.hpp"

namespace cc::actions {

struct ActionResult {
    bool ok = true;
    json::Value value;                // structured payload
    std::string text;                 // human/LLM-readable summary
    std::vector<std::uint8_t> image;  // optional encoded image
    std::string image_mime;
    Error error;
};

// Every action name the system understands, with its JSON schema. Used to
// generate MCP tool definitions and CLI help from one source.
struct ActionSpec {
    const char* name;
    const char* title;
    const char* description;
    const char* schema_json;  // JSON Schema for the arguments
    bool read_only;
    bool destructive;
};

const std::vector<ActionSpec>& registry();
const ActionSpec* find_spec(std::string_view name);

// Runs one action. Never throws; failures come back in ActionResult::error.
ActionResult run(Session& session, std::string_view name, const json::Value& args);

// Runs a JSON array of {"action": ..., ...} objects, stopping at the first
// failure and reporting which index failed.
ActionResult run_batch(Session& session, const json::Value& actions);

// Shared argument helpers, exposed for the CLI.
Result<Point> parse_point(const json::Value& v, const char* field);
Result<Rect> parse_rect(const json::Value& v, const char* field);
Modifier parse_modifiers(const json::Value& v);

}  // namespace cc::actions
