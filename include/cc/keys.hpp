// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cc/types.hpp"

namespace cc {

// Platform-independent key identifiers. Values are stable and form part of the
// C ABI, so append only.
enum class Key : std::uint16_t {
    Unknown = 0,

    A = 1,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    Digit0 = 40,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,

    Return = 60,
    Escape,
    Backspace,
    Tab,
    Space,
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Quote,
    Grave,
    Comma,
    Period,
    Slash,

    CapsLock = 90,
    F1 = 100,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,

    PrintScreen = 140,
    ScrollLock,
    Pause,
    Insert,
    Home,
    PageUp,
    Delete,
    End,
    PageDown,
    Right,
    Left,
    Down,
    Up,

    NumLock = 160,
    KeypadDivide,
    KeypadMultiply,
    KeypadMinus,
    KeypadPlus,
    KeypadEnter,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
    Keypad0,
    KeypadPeriod,
    KeypadEqual,

    // Modifiers. Left/right variants are distinct because some apps (games,
    // IDEs) bind them separately; the generic alias maps to the left key.
    LeftControl = 200,
    LeftShift,
    LeftAlt,
    LeftMeta,
    RightControl,
    RightShift,
    RightAlt,
    RightMeta,

    // Media / system
    VolumeUp = 220,
    VolumeDown,
    Mute,
    PlayPause,
    NextTrack,
    PreviousTrack,
    Stop,
    BrightnessUp,
    BrightnessDown,
    Eject,

    Menu = 240,
    Help,
    Fn,
};

enum class Modifier : std::uint32_t {
    None = 0,
    Shift = 1u << 0,
    Control = 1u << 1,
    Alt = 1u << 2,   // Option on macOS
    Meta = 1u << 3,  // Command on macOS, Win key, Super on Linux
    CapsLock = 1u << 4,
    NumLock = 1u << 5,
    Fn = 1u << 6,
};

inline Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline Modifier& operator|=(Modifier& a, Modifier b) {
    a = a | b;
    return a;
}
inline bool has(Modifier set, Modifier bit) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0;
}

struct Chord {
    Modifier modifiers = Modifier::None;
    std::vector<Key> keys;  // usually one, but "ctrl+k ctrl+c" style parses to several
};

// Parses "cmd+shift+a", "ctrl-c", "win+r", "Return", "F5". Aliases understood:
//   cmd/command/super/win/meta -> Meta, opt/option/alt -> Alt,
//   ctrl/control -> Control, esc -> Escape, enter/return -> Return.
// On macOS "ctrl" stays Control; use "cmd" for Command. `normalize_for_host`
// on Chord rewrites the common Control->Meta case when asked.
Result<Chord> parse_chord(std::string_view spec);

// Parses a whole sequence: "cmd+k cmd+s" -> two chords.
Result<std::vector<Chord>> parse_chord_sequence(std::string_view spec);

std::string to_string(Key k);
std::string to_string(const Chord& c);

// True when the key is itself a modifier.
bool is_modifier_key(Key k) noexcept;
Modifier modifier_for_key(Key k) noexcept;

// Maps a unicode code point to a key + modifiers on the current layout when
// possible. Returns Unknown for characters that need a dead-key sequence; the
// typing path falls back to direct unicode injection for those.
struct KeyStroke {
    Key key = Key::Unknown;
    Modifier modifiers = Modifier::None;
    char32_t codepoint = 0;  // non-zero when injected as a literal unicode char
};

}  // namespace cc
