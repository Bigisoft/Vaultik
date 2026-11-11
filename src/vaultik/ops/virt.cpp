#include "vaultik/ops/virt.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "vaultik/platform/os.hpp"
#include "vaultik/util/base64.hpp"
#include "vaultik/util/error.hpp"

#ifdef _WIN32
#include <windows.h>

#include <winioctl.h>

#include "vaultik/platform/acl.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"
#endif

namespace vaultik {

namespace {

using json = nlohmann::json;

std::string fold(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

// Splits a version path at either separator, without empty components.
std::vector<std::string> path_parts(std::string_view path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    return parts;
}

// Runs wsl.exe with UTF-8 output and returns what it printed; throws Error when it fails or does
// not answer in time (it hangs now and then while its service is busy).
std::string run_wsl(const std::vector<std::string>& args, int* exit_code = nullptr,
                    std::chrono::seconds timeout = std::chrono::seconds(60)) {
    std::vector<std::string> argv = {"wsl.exe"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string output;
    os::CommandOptions o;
    o.output = &output;
    o.timeout = timeout;
    const int code = os::run_command(argv, o);
    if (exit_code != nullptr) *exit_code = code;
    return console_text(output);
}

// The first line of a program's messages, for an error text.
std::string first_line(const std::string& text) {
    const std::string t = trim(text);
    const auto nl = t.find('\n');
    return nl == std::string::npos ? t : trim(t.substr(0, nl));
}

// "wsl --export <name> -", read as a stream. A watchdog stops the export when the backup is
// cancelled or WSL sends nothing for a long time, which ends the read that waits for it. Only
// time spent waiting for WSL counts: while the backup itself does not read (a slow upload holds
// it up), WSL simply waits for room in the pipe.
class WslExport : public StreamSource {
public:
    WslExport(const std::string& name, std::function<bool()> cancelled, std::chrono::seconds stall)
        : name_(name),
          process_(std::vector<std::string>{"wsl.exe", "--export", name, "-"}, os::PipedProcess::Direction::FromProgram,
                   {{"WSL_UTF8", "1"}}),
          cancelled_(std::move(cancelled)),
          stall_(stall) {
        watchdog_ = std::thread([this] { watch(); });
    }
    ~WslExport() override {
        {
            const std::lock_guard<std::mutex> g(lock_);
            done_ = true;
        }
        wake_.notify_all();
        watchdog_.join();
    }

    std::size_t read(std::uint8_t* dst, std::size_t max) override {
        waiting(true);
        std::size_t n = 0;
        try {
            n = process_.read(dst, max);
        } catch (...) {
            waiting(false);
            throw;
        }
        waiting(false);
        const std::lock_guard<std::mutex> g(lock_);
        total_ += n;
        return n;
    }

    void finish() override {
        waiting(true);  // for WSL to end
        const int code = process_.wait();
        {
            const std::lock_guard<std::mutex> g(lock_);
            done_ = true;
            waiting_ = false;
        }
        wake_.notify_all();
        if (stopped_) throw Error(stop_reason_);
        if (code != 0) {
            const std::string why = first_line(console_text(process_.messages()));
            throw Error("wsl --export failed (exit code " + std::to_string(code) + ")" + (why.empty() ? "" : ": " + why));
        }
        // A tar file ends with at least two empty 512-byte blocks; anything shorter is not one.
        if (total_ < 1024) throw Error("wsl --export gave no data");
    }

private:
    // Whether this waits for WSL now (in a read, or for it to end), and since when.
    void waiting(bool on) {
        const std::lock_guard<std::mutex> g(lock_);
        waiting_ = on;
        if (on) wait_started_ = std::chrono::steady_clock::now();
    }

    void watch() {
        std::unique_lock<std::mutex> g(lock_);
        while (!done_) {
            wake_.wait_for(g, std::chrono::milliseconds(500));
            if (done_) break;
            const bool cancel = cancelled_ && cancelled_();
            const bool stalled = waiting_ && std::chrono::steady_clock::now() - wait_started_ > stall_;
            if (cancel || stalled) {
                stop_reason_ = cancel ? "cancelled" : "wsl --export stopped sending data and was ended";
                stopped_ = true;
                process_.stop();
                break;
            }
        }
    }

    std::string name_;
    os::PipedProcess process_;
    std::function<bool()> cancelled_;
    std::chrono::seconds stall_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool waiting_ = false;
    std::chrono::steady_clock::time_point wait_started_;
    std::uint64_t total_ = 0;
    bool done_ = false;
    std::atomic<bool> stopped_{false};
    std::string stop_reason_;
    std::thread watchdog_;
};

#ifdef _WIN32
std::wstring reg_string(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t))
        return {};
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf.data(), &size) !=
        ERROR_SUCCESS)
        return {};
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    return buf;
}

DWORD reg_dword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof value;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return value;
}

std::string powershell_path() {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) throw Error("cannot find the system folder");
    return narrow(std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe");
}

// Hyper-V refuses virtual disks in an NTFS-compressed folder: files made in `dir` from now on are
// not compressed, whatever its parent folder says. Best effort (FAT and network drives).
void stop_compression(const std::string& dir) {
    const UniqueHandle h(CreateFileW(widen(dir).c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) return;
    USHORT format = COMPRESSION_FORMAT_NONE;
    DWORD returned = 0;
    DeviceIoControl(h.get(), FSCTL_SET_COMPRESSION, &format, sizeof format, nullptr, 0, &returned, nullptr);
}

// The line a failed script's message is written on (see run_powershell).
constexpr std::string_view kScriptError = "VAULTIK-SCRIPT-ERROR: ";

// Runs a PowerShell script with a fixed environment; values go in as environment variables (read
// as $env:NAME), never into the script text. Returns its output; throws Error with the script's
// error message when it fails.
std::string run_powershell(const std::string& script, const std::vector<std::pair<std::string, std::string>>& env,
                           std::chrono::seconds timeout, const std::function<bool()>& cancelled = {}) {
    // -EncodedCommand takes the script as base64 of UTF-16LE: nothing in it is parsed by a shell.
    // With its error output redirected, PowerShell writes errors as serialized objects ("#< CLIXML"),
    // not text: the script's message is caught and written as one plain line instead.
    const std::wstring wide = widen("$ErrorActionPreference = 'Stop'\n$ProgressPreference = 'SilentlyContinue'\n"
                                    "try {\n" +
                                    script +
                                    "\n} catch {\n"
                                    "    [Console]::Out.WriteLine('" + std::string(kScriptError) +
                                    "' + ($_.Exception.Message -replace '\\s+', ' '))\n"
                                    "    exit 1\n"
                                    "}\n");
    const std::string encoded =
        base64_encode(ByteSpan(reinterpret_cast<const std::uint8_t*>(wide.data()), wide.size() * sizeof(wchar_t)));
    std::string output;
    os::CommandOptions o;
    o.output = &output;
    o.timeout = timeout;
    o.cancelled = cancelled;
    o.clean_environment = true;
    o.extra_environment = env;
    const int code = os::run_command(
        {powershell_path(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded}, o);
    if (code != 0) {
        std::string why;
        const std::size_t at = output.find(kScriptError);
        if (at != std::string::npos) why = first_line(output.substr(at + kScriptError.size()));
        else if (output.rfind("#< CLIXML", 0) != 0) why = first_line(output);
        if (why.empty()) why = "exit code " + std::to_string(code);
        throw Error(why);
    }
    return output;
}
#endif

}  // namespace

std::string console_text(std::string_view raw) {
    std::string out;
    // UTF-16LE: wsl.exe's default; ASCII text then has a zero in every second byte.
    std::size_t zeros = 0;
    for (std::size_t i = 1; i < raw.size(); i += 2)
        if (raw[i] == '\0') ++zeros;
    const bool utf16 = raw.size() >= 2 && zeros * 4 >= raw.size();  // at least half the pairs
    if (utf16) {
        std::size_t i = 0;
        if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE)
            i = 2;
        for (; i + 1 < raw.size(); i += 2) {
            std::uint32_t c = static_cast<unsigned char>(raw[i]) | (static_cast<unsigned char>(raw[i + 1]) << 8);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
                const std::uint32_t lo =
                    static_cast<unsigned char>(raw[i + 2]) | (static_cast<unsigned char>(raw[i + 3]) << 8);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (c == '\r' || c == 0) continue;
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else if (c < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else if (c < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (c >> 18)));
                out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return out;
    }
    std::string_view text = raw;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    for (const char c : text)
        if (c != '\r' && c != '\0') out.push_back(c);
    return out;
}

// ---- WSL ------------------------------------------------------------------------------------

bool valid_distro_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

std::vector<std::string> parse_distro_names(std::string_view text) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::string line = trim(text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
        if (valid_distro_name(line)) names.push_back(line);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return names;
}

std::vector<std::string> wsl_place(const std::string& distro) {
    return {kWslFolder, distro + ".tar"};
}

std::string wsl_snapshot_path(const std::string& distro) {
    return std::string(kWslFolder) + "/" + distro + ".tar";
}

std::string distro_of_path(std::string_view snapshot_path) {
    const std::vector<std::string> parts = path_parts(snapshot_path);
    if (parts.size() != 2 || parts[0] != kWslFolder || !parts[1].ends_with(".tar")) return {};
    const std::string name = parts[1].substr(0, parts[1].size() - 4);
    return valid_distro_name(name) ? name : std::string();
}

const char* to_string(WslWhenRunning w) {
    switch (w) {
        case WslWhenRunning::Skip:
            return "skip";
        case WslWhenRunning::Export:
            return "export";
        case WslWhenRunning::Stop:
            return "stop";
    }
    return "skip";
}

WslWhenRunning parse_wsl_when_running(std::string_view text) {
    if (text == "skip") return WslWhenRunning::Skip;
    if (text == "export") return WslWhenRunning::Export;
    if (text == "stop") return WslWhenRunning::Stop;
    throw Error("what to do with a running WSL distribution must be skip, export or stop, not \"" + std::string(text) +
                "\"");
}

WslPlan plan_wsl(const std::vector<std::string>& chosen, const std::vector<WslDistro>& installed,
                 const std::vector<std::string>& running, WslWhenRunning when) {
    WslPlan plan;
    std::vector<std::string> seen;
    for (const std::string& want : chosen) {
        const auto it = std::find_if(installed.begin(), installed.end(),
                                     [&](const WslDistro& d) { return fold(d.name) == fold(want); });
        if (it == installed.end()) {
            plan.not_installed.push_back(want);
            continue;
        }
        if (std::find(seen.begin(), seen.end(), fold(it->name)) != seen.end()) continue;
        seen.push_back(fold(it->name));
        const bool is_running = std::any_of(running.begin(), running.end(),
                                            [&](const std::string& r) { return fold(r) == fold(it->name); });
        if (is_running && when == WslWhenRunning::Skip) {
            plan.running_skipped.emplace_back(
                it->name, "the WSL distribution " + it->name +
                              " is running, so it was left out (the job backs up only stopped distributions; stop it "
                              "with \"wsl --terminate " + it->name + "\" or change the job's setting)");
            continue;
        }
        plan.export_names.push_back(it->name);
        if (is_running && when == WslWhenRunning::Stop) plan.stop_first.push_back(it->name);
    }
    return plan;
}

std::vector<WslDistro> wsl_distros() {
    std::vector<WslDistro> out;
#ifdef _WIN32
    HKEY lxss = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &lxss) !=
        ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t sub_name[256];
        DWORD len = 256;
        const LSTATUS st = RegEnumKeyExW(lxss, i, sub_name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(lxss, sub_name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        WslDistro d;
        d.name = narrow(reg_string(sub, L"DistributionName"));
        std::wstring base = reg_string(sub, L"BasePath");
        if (base.starts_with(L"\\\\?\\") && !base.starts_with(L"\\\\?\\UNC\\")) base = base.substr(4);
        d.folder = narrow(base);
        d.version = static_cast<int>(reg_dword(sub, L"Version", 2));
        const std::wstring vhd = reg_string(sub, L"VhdFileName");
        if (d.version >= 2 && !d.folder.empty()) d.disk = d.folder + "\\" + (vhd.empty() ? std::string("ext4.vhdx") : narrow(vhd));
        RegCloseKey(sub);
        if (!d.name.empty()) out.push_back(std::move(d));
    }
    RegCloseKey(lxss);
    std::sort(out.begin(), out.end(), [](const WslDistro& a, const WslDistro& b) { return fold(a.name) < fold(b.name); });
#endif
    return out;
}

std::vector<std::string> wsl_running() {
#ifdef _WIN32
    int code = 0;
    const std::string text = run_wsl({"--list", "--running", "--quiet"}, &code);
    // With none running it prints a message and may end with an error code; the names it lists
    // are what counts (a message line is never a valid name).
    return parse_distro_names(text);
#else
    throw Error("WSL is only available on Windows");
#endif
}

void wsl_terminate(const std::string& name) {
    if (!valid_distro_name(name)) throw Error("invalid WSL distribution name \"" + name + "\"");
    int code = 0;
    const std::string text = run_wsl({"--terminate", name}, &code);
    if (code != 0) throw Error("could not stop the WSL distribution " + name + ": " + first_line(text));
}

ExtraSource wsl_export_source(const std::string& name, std::function<bool()> cancelled, std::chrono::seconds stall) {
    ExtraSource x;
    x.place = wsl_place(name);
    x.open = [name, cancelled = std::move(cancelled), stall]() -> std::unique_ptr<StreamSource> {
        if (!valid_distro_name(name)) throw Error("invalid WSL distribution name \"" + name + "\"");
        return std::make_unique<WslExport>(name, cancelled, stall);
    };
    return x;
}

std::vector<std::string> disk_excludes(const std::vector<std::string>& files) {
    std::vector<std::string> out;
    for (std::string f : files) {
        if (f.starts_with("\\\\?\\UNC\\")) f = "\\\\" + f.substr(8);
        else if (f.starts_with("\\\\?\\")) f = f.substr(4);
        if (f.empty() || f.find_first_of("*?") != std::string::npos) continue;  // a wildcard would match more
        if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    }
    return out;
}

void wsl_import(Repository& repo, const Node& file, const std::string& name, const std::string& folder, bool replace,
                const std::function<void(const VirtProgress&)>& progress, const std::function<bool()>& cancelled) {
    if (!valid_distro_name(name))
        throw Error("\"" + name + "\" cannot be a WSL distribution name (letters, digits, '.', '_' and '-' only)");
    if (file.kind != NodeKind::File) throw Error("that is not an exported WSL distribution (a .tar file)");
    if (folder.empty()) throw Error("choose a folder to install the distribution in");
    const std::string target = os::absolute_path(folder);
    const std::vector<WslDistro> installed = wsl_distros();
    const auto existing = std::find_if(installed.begin(), installed.end(),
                                       [&](const WslDistro& d) { return fold(d.name) == fold(name); });
    VirtProgress p;
    p.total = file.size;
    if (existing != installed.end()) {
        if (!replace)
            throw Error("a WSL distribution called " + existing->name +
                        " exists already; choose another name (or replace it explicitly)");
        // Unregistering deletes the distribution's disk at once, and an import that fails later
        // would leave neither: first make sure every piece of the export is there and intact
        // (read and checked once; the import reads it again).
        for (const Id& id : file.content)
            if (!repo.index().contains(id))
                throw Error("the backup of this distribution is incomplete (a piece of it is missing), so " +
                            existing->name + " was left as it is");
        p.total = 2 * file.size;
        for (const Id& id : file.content) {
            if (cancelled && cancelled()) throw Error("cancelled");
            Bytes chunk;
            try {
                chunk = repo.load_blob(BlobType::Data, id);
            } catch (const std::exception& e) {
                throw Error("the backup of this distribution cannot be read in full (" + std::string(e.what()) +
                            "), so " + existing->name + " was left as it is");
            }
            p.bytes += chunk.size();
            if (progress) progress(p);
        }
        int code = 0;
        const std::string text = run_wsl({"--unregister", existing->name}, &code, std::chrono::minutes(10));
        if (code != 0) throw Error("could not remove the WSL distribution " + existing->name + ": " + first_line(text));
    }
    os::make_dirs(target);
    if (os::exists(os::join(target, "ext4.vhdx")))
        throw Error(target + " holds a distribution's disk (ext4.vhdx) already; choose an empty folder");

    os::PipedProcess wsl({"wsl.exe", "--import", name, target, "-"}, os::PipedProcess::Direction::ToProgram,
                         {{"WSL_UTF8", "1"}});
    try {
        for (const Id& id : file.content) {
            if (cancelled && cancelled()) throw Error("cancelled");
            const Bytes chunk = repo.load_blob(BlobType::Data, id);
            wsl.write(chunk);
            p.bytes += chunk.size();
            if (progress) progress(p);
        }
    } catch (const std::exception& e) {
        // Stop the import: a half-fed one must not become a distribution. (Whatever WSL already
        // registered of it is its own clean-up; the message says what happened.)
        wsl.stop();
        const std::string why = first_line(console_text(wsl.messages()));
        throw Error(std::string(e.what()) + (why.empty() ? "" : " (wsl: " + why + ")"));
    }
    const int code = wsl.wait();
    if (code != 0) {
        const std::string why = first_line(console_text(wsl.messages()));
        throw Error("wsl --import failed (exit code " + std::to_string(code) + ")" + (why.empty() ? "" : ": " + why));
    }
}

// ---- Hyper-V --------------------------------------------------------------------------------

bool valid_vm_name(std::string_view name) {
    if (name.empty() || name.size() > 100) return false;
    if (trim(name) != name) return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '*' || c == '?' || c == '[' || c == ']' || c == '`') return false;
    }
    return true;
}

std::string vm_file_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x20 || std::strchr("<>:\"/\\|?*", c) != nullptr ? '_' : c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.back() = '_';  // Windows drops them
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

std::vector<std::string> hyperv_place(const std::string& machine) {
    return {kHyperVFolder, vm_file_name(machine)};
}

std::string hyperv_snapshot_path(const std::string& machine) {
    return std::string(kHyperVFolder) + "/" + vm_file_name(machine);
}

std::string machine_of_path(std::string_view snapshot_path) {
    const std::vector<std::string> parts = path_parts(snapshot_path);
    if (parts.size() != 2 || parts[0] != kHyperVFolder) return {};
    return parts[1];
}

std::vector<HyperVm> parse_hyperv_list(std::string_view text) {
    std::vector<HyperVm> out;
    const std::string body = trim(text);
    if (body.empty()) return out;
    json j;
    try {
        j = json::parse(body);
    } catch (const json::exception& e) {
        throw Error(std::string("unexpected answer from Get-VM: ") + e.what());
    }
    const auto one = [&](const json& v) {
        if (!v.is_object()) return;
        HyperVm m;
        m.name = v.value("Name", std::string());
        // ConvertTo-Json writes an enum as its number unless it was turned into text first.
        if (v.contains("State") && v.at("State").is_string()) m.state = v.at("State").get<std::string>();
        if (v.contains("Disks")) {
            const json& d = v.at("Disks");
            if (d.is_string()) m.disks.push_back(d.get<std::string>());
            else if (d.is_array())
                for (const json& x : d)
                    if (x.is_string()) m.disks.push_back(x.get<std::string>());
        }
        if (!m.name.empty()) out.push_back(std::move(m));
    };
    if (j.is_array()) {
        for (const json& v : j) one(v);
    } else {
        one(j);
    }
    return out;
}

HyperVList hyperv_machines() {
    HyperVList list;
#ifdef _WIN32
    // The management service must exist: without Hyper-V there is no Get-VM to ask.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    bool installed = false;
    if (scm != nullptr) {
        SC_HANDLE svc = OpenServiceW(scm, L"vmms", SERVICE_QUERY_STATUS);
        installed = svc != nullptr;
        if (svc != nullptr) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    }
    if (!installed) {
        list.problem = "Hyper-V is not installed on this computer";
        return list;
    }
    if (!os::process_elevated()) {
        list.problem = "listing Hyper-V virtual machines needs administrator rights";
        return list;
    }
    try {
        const std::string out = run_powershell(
            "$list = @(Get-VM | ForEach-Object { [pscustomobject]@{ Name = $_.Name; State = $_.State.ToString(); "
            "Disks = @(Get-VMHardDiskDrive -VM $_ | Where-Object { $_.Path } | ForEach-Object { $_.Path }) } })\n"
            "ConvertTo-Json -InputObject $list -Depth 3 -Compress\n",
            {}, std::chrono::minutes(2));
        list.machines = parse_hyperv_list(out);
        list.available = true;
    } catch (const Error& e) {
        list.problem = std::string("could not list the Hyper-V virtual machines: ") + e.what();
    }
#else
    list.problem = "Hyper-V is only available on Windows";
#endif
    return list;
}

bool hyperv_run_left_over(const std::string& name, std::chrono::hours age, std::uint32_t own_pid,
                          const std::function<bool(std::uint32_t)>& alive) {
    if (!name.starts_with("run-")) return false;
    const std::string rest = name.substr(4);
    const std::size_t dash = rest.find('-');
    if (dash == std::string::npos) return age >= std::chrono::hours(24);
    std::uint32_t pid = 0;
    const std::string digits = rest.substr(0, dash);
    if (digits.empty() || digits.size() > 10 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        }))
        return false;
    const unsigned long long n = std::stoull(digits);
    if (n > 0xFFFFFFFFull) return false;
    pid = static_cast<std::uint32_t>(n);
    return age >= std::chrono::hours(24 * 7) || (pid != own_pid && !alive(pid));
}


std::string default_hyperv_staging() {
    return os::join(os::join(os::program_data_dir(), "Vaultik"), "hyperv-export");
}

std::string hyperv_export(const std::string& name, const std::string& folder, const std::function<bool()>& cancelled) {
#ifdef _WIN32
    if (!valid_vm_name(name)) throw Error("invalid virtual machine name \"" + name + "\"");
    os::make_dirs(folder);
    run_powershell(
        "$vm = @(Get-VM -Name $env:VAULTIK_VM)\n"
        "if ($vm.Count -ne 1) { throw \"no single virtual machine called $env:VAULTIK_VM\" }\n"
        "Export-VM -VM $vm[0] -Path $env:VAULTIK_DIR\n",
        {{"VAULTIK_VM", name}, {"VAULTIK_DIR", folder}}, std::chrono::hours(12), cancelled);
    // Export-VM puts it in a folder named after the machine; the staging folder holds only that.
    os::DirReader reader(folder);
    std::string entry;
    os::FileInfo info;
    std::string found;
    while (reader.next(entry, info))
        if (info.type == os::EntryType::Dir) {
            if (!found.empty()) throw Error("the export folder " + folder + " holds more than one machine");
            found = os::join(folder, entry);
        }
    if (found.empty()) throw Error("Export-VM left nothing in " + folder);
    return found;
#else
    (void)name;
    (void)folder;
    (void)cancelled;
    throw Error("Hyper-V is only available on Windows");
#endif
}

std::string find_vmcx(const std::string& folder) {
    const std::string dir = os::join(folder, "Virtual Machines");
    std::vector<std::string> found;
    os::DirReader reader(dir);
    std::string entry;
    os::FileInfo info;
    while (reader.next(entry, info))
        if (info.type == os::EntryType::File && fold(entry).ends_with(".vmcx")) found.push_back(os::join(dir, entry));
    if (found.empty())
        throw Error(folder + " is not an exported virtual machine (no configuration in its \"Virtual Machines\" folder)");
    if (found.size() > 1) throw Error(folder + " holds more than one virtual machine configuration");
    return found.front();
}

std::string hyperv_import(const std::string& folder, const std::string& new_name, const std::string& destination) {
#ifdef _WIN32
    if (!new_name.empty() && !valid_vm_name(new_name))
        throw Error("\"" + new_name + "\" cannot be a virtual machine name (no * ? [ ] ` or control characters)");
    const std::string vmcx = find_vmcx(os::absolute_path(folder));
    if (destination.empty()) throw Error("choose a folder for the new machine's files");
    const std::string dest = os::absolute_path(destination);
    if (os::exists(dest)) {
        os::DirReader reader(dest);
        std::string entry;
        os::FileInfo info;
        if (reader.next(entry, info)) throw Error(dest + " is not empty; choose a new or empty folder for the machine");
    }
    os::make_dirs(dest);
    stop_compression(dest);
    // Compare-VM reads the exported configuration (and its name) without importing anything, so
    // the name is checked before a single file is copied; Import-VM then copies with a new id, and
    // never registers over an existing machine.
    const std::string out = run_powershell(
        "$report = Compare-VM -Path $env:VAULTIK_VMCX -Copy -GenerateNewId -VirtualMachinePath $env:VAULTIK_DEST "
        "-VhdDestinationPath (Join-Path $env:VAULTIK_DEST 'Virtual Hard Disks') -SnapshotFilePath $env:VAULTIK_DEST "
        "-SmartPagingFilePath $env:VAULTIK_DEST\n"
        "$name = if ($env:VAULTIK_NAME) { $env:VAULTIK_NAME } else { $report.VM.Name }\n"
        "if (@(Get-VM | Where-Object { $_.Name -eq $name }).Count -gt 0) { throw \"a virtual machine called $name "
        "exists already; choose another name\" }\n"
        "if ($report.Incompatibilities.Count -gt 0) { throw ('cannot import: ' + $report.Incompatibilities[0].Message) }\n"
        "$vm = Import-VM -CompatibilityReport $report\n"
        "if ($vm.Name -ne $name) { Rename-VM -VM $vm -NewName $name }\n"
        "Write-Output $name\n",
        {{"VAULTIK_VMCX", vmcx}, {"VAULTIK_DEST", dest}, {"VAULTIK_NAME", new_name}}, std::chrono::hours(12));
    const std::string name = first_line(out);
    return name.empty() ? new_name : name;
#else
    (void)folder;
    (void)new_name;
    (void)destination;
    throw Error("Hyper-V is only available on Windows");
#endif
}

}  // namespace vaultik
