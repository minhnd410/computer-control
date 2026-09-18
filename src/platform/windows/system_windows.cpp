// SPDX-License-Identifier: MIT
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "cc/system.hpp"

namespace cc {
namespace {

std::string narrow(const wchar_t* w, int len = -1) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(len < 0 ? n - 1 : n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// The clipboard is a single global resource and OpenClipboard fails while
// another process holds it. Retrying briefly turns a common transient failure
// into a success; not retrying makes copy/paste flaky in exactly the
// situations automation runs in.
class ClipboardLock {
public:
    ClipboardLock() {
        for (int i = 0; i < 12 && !open_; ++i) {
            open_ = ::OpenClipboard(nullptr) != FALSE;
            if (!open_) ::Sleep(25);
        }
    }
    ~ClipboardLock() {
        if (open_) ::CloseClipboard();
    }
    bool ok() const { return open_; }

private:
    bool open_ = false;
};

HKEY root_for(std::string_view path, std::string* rest) {
    struct Entry {
        const char* prefix;
        HKEY key;
    };
    static const Entry kRoots[] = {
        {"HKEY_CURRENT_USER", HKEY_CURRENT_USER},
        {"HKCU", HKEY_CURRENT_USER},
        {"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE},
        {"HKLM", HKEY_LOCAL_MACHINE},
        {"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT},
        {"HKCR", HKEY_CLASSES_ROOT},
        {"HKEY_USERS", HKEY_USERS},
        {"HKU", HKEY_USERS},
        {"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG},
        {"HKCC", HKEY_CURRENT_CONFIG},
    };
    std::string p(path);
    for (const auto& e : kRoots) {
        const std::size_t n = std::strlen(e.prefix);
        if (p.size() >= n && _strnicmp(p.c_str(), e.prefix, n) == 0) {
            std::size_t start = n;
            // Accept both PowerShell "HKCU:\..." and plain "HKCU\..." forms.
            if (start < p.size() && p[start] == ':') ++start;
            if (start < p.size() && (p[start] == '\\' || p[start] == '/')) ++start;
            *rest = p.substr(start);
            return e.key;
        }
    }
    return nullptr;
}

class WinSystem final : public SystemBackend {
public:
    std::string name() const override { return "Win32"; }
    Status initialize() override { return ok(); }

    Result<ClipboardContent> clipboard_get() override {
        ClipboardLock lock;
        if (!lock.ok()) {
            return err(ErrorCode::Busy, "another process is holding the clipboard",
                       "Retry in a moment; a clipboard manager or remote-desktop client often "
                       "holds it briefly.");
        }
        ClipboardContent c;
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
            if (auto* text = static_cast<const wchar_t*>(::GlobalLock(h))) {
                c.text = narrow(text);
                c.has_text = true;
                ::GlobalUnlock(h);
            }
        }
        if (HANDLE h = ::GetClipboardData(CF_HDROP)) {
            auto drop = static_cast<HDROP>(h);
            const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH]{};
                if (::DragQueryFileW(drop, i, path, MAX_PATH)) {
                    c.file_paths.push_back(narrow(path));
                    c.has_files = true;
                }
            }
        }
        c.has_image = ::IsClipboardFormatAvailable(CF_DIB) != FALSE;
        return c;
    }

    Status clipboard_set(const ClipboardContent& c) override {
        ClipboardLock lock;
        if (!lock.ok()) return err(ErrorCode::Busy, "another process is holding the clipboard");
        if (!::EmptyClipboard()) return err(ErrorCode::BackendFailure, "EmptyClipboard failed");

        if (c.has_text || !c.text.empty()) {
            const std::wstring wide = widen(c.text);
            const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!mem) return err(ErrorCode::BackendFailure, "GlobalAlloc failed");
            if (auto* dst = static_cast<wchar_t*>(::GlobalLock(mem))) {
                std::memcpy(dst, wide.c_str(), bytes);
                ::GlobalUnlock(mem);
            }
            // Ownership transfers to the clipboard on success; on failure we
            // must free it ourselves or it leaks for the process lifetime.
            if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
                ::GlobalFree(mem);
                return err(ErrorCode::BackendFailure, "SetClipboardData failed");
            }
        }
        return ok();
    }

    Result<std::vector<ProcessInfo>> list_processes() override {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return err(ErrorCode::BackendFailure, "CreateToolhelp32Snapshot failed");
        }
        std::vector<ProcessInfo> out;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot, &entry)) {
            do {
                ProcessInfo p;
                p.pid = entry.th32ProcessID;
                p.ppid = entry.th32ParentProcessID;
                p.name = narrow(entry.szExeFile);

                if (HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             entry.th32ProcessID)) {
                    PROCESS_MEMORY_COUNTERS pmc{};
                    if (::GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                        p.memory_bytes = static_cast<std::int64_t>(pmc.WorkingSetSize);
                    }
                    wchar_t path[MAX_PATH]{};
                    DWORD size = MAX_PATH;
                    if (::QueryFullProcessImageNameW(h, 0, path, &size)) {
                        p.command = narrow(path, static_cast<int>(size));
                    }
                    ::CloseHandle(h);
                }
                out.push_back(std::move(p));
            } while (::Process32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        return out;
    }

    Status kill_process(std::int64_t pid, bool force) override {
        if (pid <= 4) {
            return err(ErrorCode::InvalidArgument,
                       "refusing to terminate pid " + std::to_string(pid),
                       "pids 0-4 are System and the Idle process; terminating them bluescreens "
                       "the machine.");
        }
        if (pid == ::GetCurrentProcessId()) {
            return err(ErrorCode::InvalidArgument,
                       "refusing to kill the automation process itself");
        }
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!h) {
            return err(ErrorCode::PermissionDenied, "cannot open pid " + std::to_string(pid),
                       "The process is elevated or protected. Run elevated to reach it.");
        }
        const BOOL okay = ::TerminateProcess(h, force ? 1 : 0);
        ::CloseHandle(h);
        if (!okay) return err(ErrorCode::BackendFailure, "TerminateProcess failed");
        return ok();
    }

    Result<ShellResult> run_shell(const ShellRequest& req) override {
        const auto start = std::chrono::steady_clock::now();

        std::string interpreter = req.shell;
        if (interpreter.empty()) interpreter = "powershell";

        std::wstring cmdline;
        if (interpreter == "cmd") {
            cmdline = L"cmd.exe /D /C " + widen(req.command);
        } else {
            // -NoProfile keeps the user's profile script from changing
            // behaviour and costs ~300ms less per call.
            cmdline = widen(interpreter) +
                      L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + L"\"" +
                      widen(req.command) + L"\"";
        }

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
        if (!::CreatePipe(&out_r, &out_w, &sa, 0) || !::CreatePipe(&err_r, &err_w, &sa, 0)) {
            return err(ErrorCode::BackendFailure, "CreatePipe failed");
        }
        ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;  // never flash a console window at the user
        si.hStdOutput = out_w;
        si.hStdError = err_w;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
        mutable_cmd.push_back(L'\0');
        const std::wstring cwd = widen(req.cwd);

        if (!::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                              nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
            ::CloseHandle(out_r);
            ::CloseHandle(out_w);
            ::CloseHandle(err_r);
            ::CloseHandle(err_w);
            return err(ErrorCode::BackendFailure, "CreateProcess failed for " + interpreter);
        }
        ::CloseHandle(out_w);
        ::CloseHandle(err_w);

        // Drain both pipes on threads. Reading one then waiting deadlocks as
        // soon as the child fills the other pipe's 64 KB buffer.
        ShellResult result;
        auto drain = [](HANDLE h, std::string& sink) {
            char buf[4096];
            DWORD n = 0;
            while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) sink.append(buf, n);
        };
        std::thread t_out(drain, out_r, std::ref(result.stdout_text));
        std::thread t_err(drain, err_r, std::ref(result.stderr_text));

        const DWORD wait =
            ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(req.timeout.count()));
        if (wait == WAIT_TIMEOUT) {
            result.timed_out = true;
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 2000);
        }
        t_out.join();
        t_err.join();
        ::CloseHandle(out_r);
        ::CloseHandle(err_r);

        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = result.timed_out ? -1 : static_cast<int>(code);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }

    Status notify(const NotificationRequest& req) override {
        // A real toast needs a registered AUMID and a packaged identity, which
        // a portable binary does not have. PowerShell's BurntToast is not
        // installed by default either. The shell notification area balloon
        // works from any process and needs no registration.
        ShellRequest sr;
        auto escape = [](const std::string& s) {
            std::string out;
            for (char c : s) {
                if (c == '\'')
                    out += "''";
                else
                    out.push_back(c);
            }
            return out;
        };
        sr.command =
            "[reflection.assembly]::LoadWithPartialName('System.Windows.Forms') | Out-Null; "
            "$n = New-Object System.Windows.Forms.NotifyIcon; "
            "$n.Icon = [System.Drawing.SystemIcons]::Information; "
            "$n.BalloonTipTitle = '" +
            escape(req.title) +
            "'; "
            "$n.BalloonTipText = '" +
            escape(req.message) +
            "'; "
            "$n.Visible = $true; $n.ShowBalloonTip(5000); Start-Sleep -Seconds 6; $n.Dispose()";
        sr.timeout = std::chrono::milliseconds{10000};
        auto r = run_shell(sr);
        if (!r) return r.error();
        if (r.value().exit_code != 0) {
            return err(ErrorCode::BackendFailure, "notification failed: " + r.value().stderr_text);
        }
        return ok();
    }

    Result<std::string> registry_get(std::string_view path, std::string_view name) override {
        std::string sub;
        HKEY root = root_for(path, &sub);
        if (!root)
            return err(ErrorCode::InvalidArgument,
                       "unrecognised registry root in " + std::string(path));

        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, widen(sub).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return err(ErrorCode::NotFound, "cannot open registry key " + std::string(path));
        }
        DWORD type = 0, size = 0;
        const std::wstring wname = widen(name);
        LONG rc = ::RegQueryValueExW(key, wname.c_str(), nullptr, &type, nullptr, &size);
        if (rc != ERROR_SUCCESS) {
            ::RegCloseKey(key);
            return err(ErrorCode::NotFound, "no value named '" + std::string(name) + "'");
        }
        std::vector<BYTE> buf(size);
        rc = ::RegQueryValueExW(key, wname.c_str(), nullptr, &type, buf.data(), &size);
        ::RegCloseKey(key);
        if (rc != ERROR_SUCCESS) return err(ErrorCode::BackendFailure, "RegQueryValueEx failed");

        switch (type) {
            case REG_SZ:
            case REG_EXPAND_SZ: {
                auto* w = reinterpret_cast<const wchar_t*>(buf.data());
                return narrow(w);
            }
            case REG_DWORD: {
                DWORD v = 0;
                std::memcpy(&v, buf.data(), std::min<std::size_t>(sizeof(v), buf.size()));
                return std::to_string(v);
            }
            case REG_QWORD: {
                std::uint64_t v = 0;
                std::memcpy(&v, buf.data(), std::min<std::size_t>(sizeof(v), buf.size()));
                return std::to_string(v);
            }
            default:
                return err(ErrorCode::Unsupported,
                           "registry value type " + std::to_string(type) + " is not supported",
                           "Only REG_SZ, REG_EXPAND_SZ, REG_DWORD and REG_QWORD are read.");
        }
    }

    Status registry_set(std::string_view path, std::string_view name, std::string_view value,
                        std::string_view type) override {
        std::string sub;
        HKEY root = root_for(path, &sub);
        if (!root) return err(ErrorCode::InvalidArgument, "unrecognised registry root");

        HKEY key = nullptr;
        if (::RegCreateKeyExW(root, widen(sub).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                              nullptr) != ERROR_SUCCESS) {
            return err(ErrorCode::PermissionDenied,
                       "cannot open registry key for writing: " + std::string(path),
                       "Writing under HKLM requires an elevated process.");
        }
        const std::wstring wname = widen(name);
        LONG rc = ERROR_INVALID_PARAMETER;
        if (type == "DWord" || type == "DWORD" || type == "dword") {
            const DWORD v =
                static_cast<DWORD>(std::strtoul(std::string(value).c_str(), nullptr, 0));
            rc = ::RegSetValueExW(key, wname.c_str(), 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&v), sizeof(v));
        } else {
            const std::wstring wide = widen(value);
            rc = ::RegSetValueExW(key, wname.c_str(), 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(wide.c_str()),
                                  static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
        }
        ::RegCloseKey(key);
        if (rc != ERROR_SUCCESS) return err(ErrorCode::BackendFailure, "RegSetValueEx failed");
        return ok();
    }

    Status registry_delete(std::string_view path, std::string_view name) override {
        std::string sub;
        HKEY root = root_for(path, &sub);
        if (!root) return err(ErrorCode::InvalidArgument, "unrecognised registry root");

        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, widen(sub).c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
            return err(ErrorCode::NotFound, "cannot open registry key " + std::string(path));
        }
        const LONG rc = ::RegDeleteValueW(key, widen(name).c_str());
        ::RegCloseKey(key);
        if (rc != ERROR_SUCCESS) return err(ErrorCode::NotFound, "no such value");
        return ok();
    }

    Result<std::vector<std::string>> registry_list(std::string_view path) override {
        std::string sub;
        HKEY root = root_for(path, &sub);
        if (!root) return err(ErrorCode::InvalidArgument, "unrecognised registry root");

        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, widen(sub).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return err(ErrorCode::NotFound, "cannot open registry key " + std::string(path));
        }
        std::vector<std::string> out;
        wchar_t name[16384];
        DWORD index = 0, size = 0;
        while (true) {
            size = static_cast<DWORD>(std::size(name));
            if (::RegEnumKeyExW(key, index++, name, &size, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            out.push_back("[key] " + narrow(name, static_cast<int>(size)));
        }
        index = 0;
        while (true) {
            size = static_cast<DWORD>(std::size(name));
            if (::RegEnumValueW(key, index++, name, &size, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            out.push_back("[value] " + narrow(name, static_cast<int>(size)));
        }
        ::RegCloseKey(key);
        return out;
    }
};

}  // namespace

Result<std::unique_ptr<SystemBackend>> SystemBackend::create() {
    auto backend = std::make_unique<WinSystem>();
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<SystemBackend>(std::move(backend));
}

// Non-Windows fallbacks live in each platform's system file; on Windows the
// real implementations above override them, so the base versions are never
// used here. They still need definitions because the vtable references them.
Result<std::string> SystemBackend::registry_get(std::string_view, std::string_view) {
    return err(ErrorCode::Unsupported, "not implemented");
}
Status SystemBackend::registry_set(std::string_view, std::string_view, std::string_view,
                                   std::string_view) {
    return err(ErrorCode::Unsupported, "not implemented");
}
Status SystemBackend::registry_delete(std::string_view, std::string_view) {
    return err(ErrorCode::Unsupported, "not implemented");
}
Result<std::vector<std::string>> SystemBackend::registry_list(std::string_view) {
    return err(ErrorCode::Unsupported, "not implemented");
}

}  // namespace cc
