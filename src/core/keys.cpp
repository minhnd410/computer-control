// SPDX-License-Identifier: MIT
#include "cc/keys.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <unordered_map>

namespace cc {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// One table for every alias any of the three platforms (or either reference
// project) uses, so "win+r", "cmd+r" and "super+r" all parse.
const std::unordered_map<std::string, Key>& key_table() {
    static const std::unordered_map<std::string, Key> t = [] {
        std::unordered_map<std::string, Key> m;
        for (char c = 'a'; c <= 'z'; ++c)
            m[std::string(1, c)] = static_cast<Key>(static_cast<int>(Key::A) + (c - 'a'));
        for (char c = '0'; c <= '9'; ++c)
            m[std::string(1, c)] = static_cast<Key>(static_cast<int>(Key::Digit0) + (c - '0'));
        for (int i = 1; i <= 24; ++i)
            m["f" + std::to_string(i)] = static_cast<Key>(static_cast<int>(Key::F1) + (i - 1));

        m["return"] = Key::Return;
        m["enter"] = Key::Return;
        m["escape"] = Key::Escape;
        m["esc"] = Key::Escape;
        m["backspace"] = Key::Backspace;
        m["back"] = Key::Backspace;
        m["bksp"] = Key::Backspace;
        m["tab"] = Key::Tab;
        m["space"] = Key::Space;
        m["spacebar"] = Key::Space;
        m["minus"] = Key::Minus;
        m["-"] = Key::Minus;
        m["equal"] = Key::Equal;
        m["="] = Key::Equal;
        m["plus"] = Key::Equal;
        m["leftbracket"] = Key::LeftBracket;
        m["["] = Key::LeftBracket;
        m["rightbracket"] = Key::RightBracket;
        m["]"] = Key::RightBracket;
        m["backslash"] = Key::Backslash;
        m["\\"] = Key::Backslash;
        m["semicolon"] = Key::Semicolon;
        m[";"] = Key::Semicolon;
        m["quote"] = Key::Quote;
        m["'"] = Key::Quote;
        m["grave"] = Key::Grave;
        m["`"] = Key::Grave;
        m["tilde"] = Key::Grave;
        m["comma"] = Key::Comma;
        m[","] = Key::Comma;
        m["period"] = Key::Period;
        m["."] = Key::Period;
        m["dot"] = Key::Period;
        m["slash"] = Key::Slash;
        m["/"] = Key::Slash;
        m["capslock"] = Key::CapsLock;

        m["printscreen"] = Key::PrintScreen;
        m["prtsc"] = Key::PrintScreen;
        m["scrolllock"] = Key::ScrollLock;
        m["pause"] = Key::Pause;
        m["break"] = Key::Pause;
        m["insert"] = Key::Insert;
        m["ins"] = Key::Insert;
        m["home"] = Key::Home;
        m["pageup"] = Key::PageUp;
        m["pgup"] = Key::PageUp;
        m["delete"] = Key::Delete;
        m["del"] = Key::Delete;
        m["forwarddelete"] = Key::Delete;
        m["end"] = Key::End;
        m["pagedown"] = Key::PageDown;
        m["pgdn"] = Key::PageDown;
        m["right"] = Key::Right;
        m["arrowright"] = Key::Right;
        m["rightarrow"] = Key::Right;
        m["left"] = Key::Left;
        m["arrowleft"] = Key::Left;
        m["leftarrow"] = Key::Left;
        m["down"] = Key::Down;
        m["arrowdown"] = Key::Down;
        m["downarrow"] = Key::Down;
        m["up"] = Key::Up;
        m["arrowup"] = Key::Up;
        m["uparrow"] = Key::Up;

        m["numlock"] = Key::NumLock;
        m["numpaddivide"] = Key::KeypadDivide;
        m["numpadmultiply"] = Key::KeypadMultiply;
        m["numpadsubtract"] = Key::KeypadMinus;
        m["numpadadd"] = Key::KeypadPlus;
        m["numpadenter"] = Key::KeypadEnter;
        m["numpaddecimal"] = Key::KeypadPeriod;
        for (int i = 0; i <= 9; ++i) {
            Key k = (i == 0) ? Key::Keypad0
                             : static_cast<Key>(static_cast<int>(Key::Keypad1) + (i - 1));
            m["numpad" + std::to_string(i)] = k;
            m["kp" + std::to_string(i)] = k;
        }

        m["ctrl"] = Key::LeftControl;
        m["control"] = Key::LeftControl;
        m["lctrl"] = Key::LeftControl;
        m["rctrl"] = Key::RightControl;
        m["shift"] = Key::LeftShift;
        m["lshift"] = Key::LeftShift;
        m["rshift"] = Key::RightShift;
        m["alt"] = Key::LeftAlt;
        m["option"] = Key::LeftAlt;
        m["opt"] = Key::LeftAlt;
        m["lalt"] = Key::LeftAlt;
        m["ralt"] = Key::RightAlt;
        m["altgr"] = Key::RightAlt;
        m["cmd"] = Key::LeftMeta;
        m["command"] = Key::LeftMeta;
        m["meta"] = Key::LeftMeta;
        m["win"] = Key::LeftMeta;
        m["windows"] = Key::LeftMeta;
        m["super"] = Key::LeftMeta;
        m["lcmd"] = Key::LeftMeta;
        m["rcmd"] = Key::RightMeta;
        m["lwin"] = Key::LeftMeta;
        m["rwin"] = Key::RightMeta;

        m["volumeup"] = Key::VolumeUp;
        m["volumedown"] = Key::VolumeDown;
        m["mute"] = Key::Mute;
        m["playpause"] = Key::PlayPause;
        m["play"] = Key::PlayPause;
        m["nexttrack"] = Key::NextTrack;
        m["previoustrack"] = Key::PreviousTrack;
        m["stop"] = Key::Stop;
        m["brightnessup"] = Key::BrightnessUp;
        m["brightnessdown"] = Key::BrightnessDown;
        m["eject"] = Key::Eject;
        m["menu"] = Key::Menu;
        m["apps"] = Key::Menu;
        m["contextmenu"] = Key::Menu;
        m["help"] = Key::Help;
        m["fn"] = Key::Fn;
        m["function"] = Key::Fn;
        return m;
    }();
    return t;
}

}  // namespace

bool is_modifier_key(Key k) noexcept {
    switch (k) {
        case Key::LeftControl:
        case Key::RightControl:
        case Key::LeftShift:
        case Key::RightShift:
        case Key::LeftAlt:
        case Key::RightAlt:
        case Key::LeftMeta:
        case Key::RightMeta:
        case Key::Fn: return true;
        default: return false;
    }
}

Modifier modifier_for_key(Key k) noexcept {
    switch (k) {
        case Key::LeftControl:
        case Key::RightControl: return Modifier::Control;
        case Key::LeftShift:
        case Key::RightShift: return Modifier::Shift;
        case Key::LeftAlt:
        case Key::RightAlt: return Modifier::Alt;
        case Key::LeftMeta:
        case Key::RightMeta: return Modifier::Meta;
        case Key::Fn: return Modifier::Fn;
        default: return Modifier::None;
    }
}

Result<Chord> parse_chord(std::string_view spec) {
    Chord chord;
    std::string s(spec);
    if (s.empty()) return err(ErrorCode::InvalidArgument, "empty key chord");

    // Accept '+' and '-' as separators, but a lone "-" or a trailing "+" means
    // the key itself: "ctrl+-" is Control plus Minus, "shift+=" is Shift plus
    // Equal. Splitting naively breaks both, so a separator only counts when it
    // is followed by more input.
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool sep = (c == '+' || c == '-');
        const bool has_more = (i + 1 < s.size());
        if (sep && !cur.empty() && has_more) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    const auto& table = key_table();
    for (const auto& raw : parts) {
        std::string name = lower(raw);
        name.erase(0, name.find_first_not_of(" \t"));
        const auto end = name.find_last_not_of(" \t");
        if (end != std::string::npos) name.erase(end + 1);
        if (name.empty()) continue;

        auto it = table.find(name);
        if (it == table.end()) {
            return err(ErrorCode::InvalidArgument, "unknown key: '" + raw + "'",
                       "Use names like 'cmd', 'ctrl', 'shift', 'alt', 'return', 'escape', "
                       "'f5', 'pageup', or a single character.");
        }
        if (is_modifier_key(it->second)) {
            chord.modifiers |= modifier_for_key(it->second);
            // A chord that is only modifiers ("cmd") still needs the key so it
            // can be tapped on its own, e.g. tapping Command to focus the Dock.
            chord.keys.push_back(it->second);
        } else {
            chord.keys.push_back(it->second);
        }
    }

    // Drop modifier keys from `keys` when there is a real key to press; the
    // backend applies `modifiers` around it instead.
    const bool has_real = std::any_of(chord.keys.begin(), chord.keys.end(),
                                      [](Key k) { return !is_modifier_key(k); });
    if (has_real) {
        chord.keys.erase(std::remove_if(chord.keys.begin(), chord.keys.end(), is_modifier_key),
                         chord.keys.end());
    } else if (chord.keys.size() > 1) {
        chord.keys.resize(1);
    }
    if (chord.keys.empty()) return err(ErrorCode::InvalidArgument, "no key in chord: '" + s + "'");
    return chord;
}

Result<std::vector<Chord>> parse_chord_sequence(std::string_view spec) {
    std::vector<Chord> out;
    std::istringstream iss{std::string(spec)};
    std::string tok;
    while (iss >> tok) {
        auto c = parse_chord(tok);
        if (!c) return c.error();
        out.push_back(c.value());
    }
    if (out.empty()) return err(ErrorCode::InvalidArgument, "empty key sequence");
    return out;
}

std::string to_string(Key k) {
    for (const auto& [name, key] : key_table()) {
        if (key == k && name.size() > 1) return name;
    }
    for (const auto& [name, key] : key_table()) {
        if (key == k) return name;
    }
    return "unknown";
}

std::string to_string(const Chord& c) {
    std::string out;
    if (has(c.modifiers, Modifier::Control)) out += "ctrl+";
    if (has(c.modifiers, Modifier::Alt)) out += "alt+";
    if (has(c.modifiers, Modifier::Shift)) out += "shift+";
    if (has(c.modifiers, Modifier::Meta)) out += "meta+";
    for (std::size_t i = 0; i < c.keys.size(); ++i) {
        if (i) out += "+";
        out += to_string(c.keys[i]);
    }
    return out;
}

}  // namespace cc
