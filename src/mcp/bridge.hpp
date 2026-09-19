// SPDX-License-Identifier: MIT
#pragma once

// A stdio front-end for the shared service.
//
// Some clients only know how to launch a local command; Claude Desktop is the
// one that matters here. Left on their own they spawn their own copy of the
// server, which on macOS means it runs as *their* child and uses *their*
// Accessibility grant - the whole problem the shared service exists to solve.
//
// This bridges the gap: the client launches this, it forwards JSON-RPC to the
// service over loopback, and the answers come back. The bridge itself touches
// no OS API and needs no permission of any kind. The grant stays with the
// launchd job, so one grant still covers every client.

#include <string>

namespace cc::mcp {

// Reads newline-delimited JSON-RPC on stdin, forwards to `url`, writes the
// replies to stdout. Returns a process exit code; blocks until stdin closes.
int run_bridge(const std::string& url, const std::string& token);

}  // namespace cc::mcp
