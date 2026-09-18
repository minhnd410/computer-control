// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cc::text {

// Truncates to at most `max_bytes`, never splitting a UTF-8 sequence.
//
// printf's "%.28s" truncates by bytes and will happily cut a multi-byte
// character in half. That lone continuation byte then travels into a JSON
// string and makes the whole document invalid, which on the MCP stdio
// transport means the client drops the connection with a parse error far away
// from the cause. Any place that shortens an OS-supplied string - a window
// title, an app name, an accessibility label - has to use this instead.
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);

// Pads to `width` bytes with spaces for column alignment, truncating first if
// needed. Width is counted in bytes, not display columns: getting real column
// width right needs wcwidth and an East-Asian-width table, and this is for
// terminal output where a slightly ragged column is not worth that.
std::string pad_utf8(std::string_view s, std::size_t width);

// Replaces malformed sequences with U+FFFD so the result is always valid
// UTF-8. Accessibility APIs mostly return well-formed text, but "mostly" is
// not a property a wire protocol can be built on.
std::string sanitize_utf8(std::string_view s);

// True when every byte sequence is well-formed UTF-8.
bool is_valid_utf8(std::string_view s);

// Base64 of the UTF-16LE encoding, with no BOM.
//
// This is the form PowerShell's -EncodedCommand wants, and using it is the
// only reliable way to hand PowerShell a script from C++: quoting a command on
// a Windows command line has to survive CommandLineToArgvW, possibly cmd, and
// then PowerShell's own parser, so anything containing a double quote - which
// is most real PowerShell - arrives mangled. Lives here rather than in the
// Windows backend so it can be tested on any platform.
std::string utf16le_base64(std::string_view utf8);

}  // namespace cc::text
