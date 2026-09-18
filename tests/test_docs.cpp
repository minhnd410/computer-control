// SPDX-License-Identifier: MIT
//
// Docs that state a number about the code drift the moment someone adds a
// tool, and nothing catches it - the docs still build, the tests still pass,
// and the number is quietly wrong for a release or two. This file pins the
// counted claims to the registry so adding a tool fails the build until the
// prose is updated.
//
// The test reads the repository, so it needs the source directory; CMake
// passes it in as CC_TEST_SOURCE_DIR.

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "actions/actions.hpp"
#include "mcp/server.hpp"
#include "test_framework.hpp"

using namespace cc;

namespace {

#ifndef CC_TEST_SOURCE_DIR
#define CC_TEST_SOURCE_DIR ""
#endif

std::string read_file(const std::string& relative) {
    const std::string root = CC_TEST_SOURCE_DIR;
    if (root.empty()) return {};
    std::ifstream in(root + "/" + relative, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The first line containing `needle`, or empty.
std::string line_containing(const std::string& text, const std::string& needle) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) return line;
    }
    return {};
}

std::vector<int> integers_in(const std::string& line) {
    std::vector<int> out;
    for (std::size_t i = 0; i < line.size();) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }
        int value = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            value = value * 10 + (line[i] - '0');
            ++i;
        }
        out.push_back(value);
    }
    return out;
}

}  // namespace

TEST(docs_tool_counts_match_the_registry) {
    const std::string doc = read_file("docs/mcp.md");
    if (doc.empty()) SKIP("docs/mcp.md is not readable from the build directory");

    const int total = static_cast<int>(actions::registry().size());
    const int advertised = static_cast<int>(mcp::tool_definitions(mcp::ServerConfig{}).size());

    const std::string line = line_containing(doc, "tools exist");
    if (line.empty()) {
        ::test::report(false, "docs/mcp.md states the tool count", __FILE__, __LINE__,
                       "expected a line reading \"<N> tools exist; <M> are advertised by "
                       "default\"; rewording is fine, the two numbers must come first");
        return;
    }

    const std::vector<int> numbers = integers_in(line);
    char note[256];
    std::snprintf(note, sizeof(note),
                  "docs/mcp.md says %d/%d, the registry has %d tools with %d advertised by "
                  "default - update the sentence in docs/mcp.md",
                  numbers.size() > 0 ? numbers[0] : -1, numbers.size() > 1 ? numbers[1] : -1, total,
                  advertised);

    ::test::report(numbers.size() >= 2 && numbers[0] == total && numbers[1] == advertised,
                   "docs/mcp.md tool counts match the registry", __FILE__, __LINE__, note);
}

TEST(docs_every_tool_is_documented_somewhere) {
    const std::string doc = read_file("docs/mcp.md");
    if (doc.empty()) SKIP("docs/mcp.md is not readable from the build directory");

    // A tool nobody wrote down is a tool nobody can find. This is a spelling
    // check, not a quality one: it only asks that the name appears.
    for (const auto& spec : actions::registry()) {
        const std::string name = spec.name;
        char note[192];
        std::snprintf(note, sizeof(note), "tool '%s' is not mentioned in docs/mcp.md",
                      name.c_str());
        ::test::report(doc.find("`" + name + "`") != std::string::npos, "tool is documented",
                       __FILE__, __LINE__, note);
    }
}
