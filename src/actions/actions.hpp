// SPDX-License-Identifier: MIT
#pragma once

// The single action dispatcher.
//
// Every action is defined once here as JSON in / JSON out, with its own
// schema, so the MCP tool layer is a thin wrapper over this table rather than
// a second implementation of argument parsing, coordinate resolution and error
// shaping. The tool list and its schemas are generated from the registry
// below, which is why adding a capability is one entry plus its handler.

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
