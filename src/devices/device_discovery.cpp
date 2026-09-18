// SPDX-License-Identifier: MIT
//
// Discovery of devices that are visible on screen but have no CLI bridge.
//
// This exists because the bridge and the device are independent: a booted iOS
// Simulator is fully drivable through its window whether or not `simctl` is on
// PATH, and `xcode-select` pointing at the Command Line Tools instead of a
// full Xcode is common enough that bridge-only discovery would report "no
// devices" on a machine with a simulator plainly open. The same applies to
// iPhone Mirroring, scrcpy and QuickTime device mirroring, none of which have
// a CLI at all.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "devices/device_internal.hpp"

namespace cc::devices {
namespace {

// Simulator titles look like "iPhone 15 Pro — 18.0" or "iPhone 15 Pro - iOS
// 18.0". Splitting on the separator gives the model on the left and the
// runtime on the right; the em dash is what Xcode 15+ actually uses.
const char* const kSeparators[] = {" \xE2\x80\x94 ", " \xE2\x80\x93 ", " - "};

std::pair<std::string, std::string> split_title(const std::string& title) {
    for (const char* sep : kSeparators) {
        const auto pos = title.find(sep);
        if (pos != std::string::npos) {
            return {trim(title.substr(0, pos)), trim(title.substr(pos + std::strlen(sep)))};
        }
    }
    return {trim(title), std::string{}};
}

// "iOS 18.0" -> "18.0"; a bare "18.0" passes through unchanged.
std::string version_of(std::string runtime) {
    const auto digit = runtime.find_first_of("0123456789");
    if (digit == std::string::npos) return {};
    return trim(runtime.substr(digit));
}

bool icontains(const std::string& hay, const char* needle) {
    std::string a = hay, b = needle;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

}  // namespace

Result<std::vector<DeviceInfo>> list_onscreen_devices(WindowBackend* windows) {
    std::vector<DeviceInfo> out;
    if (!windows) return out;

    // Offscreen windows are included deliberately. On macOS "on screen" means
    // "on the active Space", so a simulator sitting on another desktop would
    // otherwise vanish from discovery the moment the user switches Spaces -
    // and it is still perfectly drivable once brought forward. The same
    // applies to a minimised emulator window on Windows.
    auto all = windows->list_windows(true);
    if (!all) return out;  // not an error: onscreen discovery is best-effort

    for (const auto& w : all.value()) {
        DeviceInfo info;
        bool matched = false;

        if (icontains(w.app_name, "Simulator") && !w.title.empty()) {
            // Simulator owns several auxiliary windows whose titles contain a
            // device family name - "Apple TV Remote" is the one that bites -
            // so a model match alone is not enough.
            static const char* kNotDevices[] = {"Remote",   "Touch Bar", "Preferences",
                                                "Settings", "Console",   "Recording"};
            bool auxiliary = false;
            for (const char* aux : kNotDevices) {
                if (icontains(w.title, aux)) {
                    auxiliary = true;
                    break;
                }
            }
            if (!auxiliary && (icontains(w.title, "iPhone") || icontains(w.title, "iPad") ||
                               icontains(w.title, "Apple TV") ||
                               icontains(w.title, "Apple Watch") || icontains(w.title, "iPod"))) {
                auto [model, runtime] = split_title(w.title);
                info.name = model;
                info.os_version = version_of(runtime);
                info.kind = DeviceKind::Simulator;
                info.platform =
                    icontains(info.name, "iPad") ? DevicePlatform::IPadOS : DevicePlatform::IOS;
                apply_ios_metrics(info);
                matched = true;
            }
        } else if (icontains(w.app_name, "iPhone Mirroring") ||
                   icontains(w.title, "iPhone Mirroring")) {
            info.name = "iPhone Mirroring";
            info.kind = DeviceKind::Mirrored;
            info.platform = DevicePlatform::IOS;
            matched = true;
        } else if (icontains(w.app_name, "qemu") || icontains(w.app_name, "emulator") ||
                   icontains(w.title, "Android Emulator")) {
            info.name = w.title.empty() ? "Android Emulator" : split_title(w.title).first;
            info.kind = DeviceKind::Emulator;
            info.platform = DevicePlatform::Android;
            matched = true;
        } else if (icontains(w.app_name, "scrcpy")) {
            info.name = w.title.empty() ? "scrcpy" : split_title(w.title).first;
            info.kind = DeviceKind::Mirrored;
            info.platform = DevicePlatform::Android;
            matched = true;
        }

        if (!matched) continue;

        info.id = "onscreen:" + std::to_string(w.id);
        info.host_window_id = w.id;
        info.booted = true;  // the window exists, so the device is running
        info.host_viewport = w.bounds;
        info.available_transports.push_back("onscreen");
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace cc::devices
