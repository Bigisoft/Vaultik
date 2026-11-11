#include "vaultik/ops/apps.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

using json = nlohmann::json;

char lower_char(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower_char(c);
    return out;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alnum(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// "KB" followed by at least 6 digits, as a word of its own: a Windows or Office update.
bool mentions_kb(std::string_view s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (lower_char(s[i]) != 'k' || lower_char(s[i + 1]) != 'b') continue;
        if (i > 0 && is_alnum(s[i - 1])) continue;
        std::size_t digits = 0;
        while (i + 2 + digits < s.size() && is_digit(s[i + 2 + digits])) ++digits;
        if (digits >= 6) return true;
    }
    return false;
}

// A word that is a version number: "26.02", "v1.16.4".
bool version_word(std::string_view w) {
    if (!w.empty() && (w[0] == 'v' || w[0] == 'V')) w.remove_prefix(1);
    if (w.empty() || !is_digit(w[0])) return false;
    bool dot = false;
    for (const char c : w) {
        if (c == '.') dot = true;
        else if (!is_digit(c)) return false;
    }
    return dot;
}

// Letters and digits only, lower case. Bytes of non-ASCII letters are kept as they are.
std::string alnum_only(std::string_view s) {
    std::string out;
    for (const char c : s)
        if (is_alnum(c) || static_cast<unsigned char>(c) >= 0x80) out.push_back(lower_char(c));
    return out;
}

bool same_app(const InstalledApp& a, const InstalledApp& b) {
    return a.name == b.name && a.version == b.version && a.winget_id == b.winget_id;
}

}  // namespace

std::string normalize_app_name(std::string_view name) {
    // Without "(...)" and "[...]".
    std::string plain;
    int depth = 0;
    for (const char c : name) {
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if (depth == 0) plain.push_back(c);
    }
    std::string out;
    std::size_t at = 0;
    while (at < plain.size()) {
        const std::size_t end = std::min(plain.find_first_of(" \t", at), plain.size());
        const std::string_view word = std::string_view(plain).substr(at, end - at);
        if (!version_word(word)) out += alnum_only(word);
        at = end + 1;
    }
    return out.empty() ? alnum_only(name) : out;
}

std::vector<InstalledApp> filter_uninstall_entries(const std::vector<UninstallEntry>& entries) {
    static const std::set<std::string> kUpdateTypes = {"security update", "update", "hotfix", "update rollup",
                                                       "service pack"};
    std::vector<InstalledApp> out;
    std::set<std::pair<std::string, std::string>> seen;
    for (const UninstallEntry& e : entries) {
        const std::string name = trim(e.display_name);
        if (name.empty() || e.system_component || !trim(e.parent_key_name).empty()) continue;
        if (kUpdateTypes.contains(lower(trim(e.release_type)))) continue;
        if (mentions_kb(e.key_name) || mentions_kb(name)) continue;
        InstalledApp app;
        app.name = name;
        app.version = trim(e.display_version);
        app.publisher = trim(e.publisher);
        if (!seen.emplace(lower(app.name), app.version).second) continue;  // in more than one view
        out.push_back(std::move(app));
    }
    return out;
}

std::vector<WingetPackage> parse_winget_export(std::string_view text) {
    std::vector<WingetPackage> out;
    try {
        const json j = json::parse(text);
        if (!j.is_object() || !j.contains("Sources") || !j.at("Sources").is_array())
            throw Error("not a winget export file");
        for (const json& source : j.at("Sources")) {
            if (!source.is_object()) continue;
            std::string name;
            if (source.contains("SourceDetails") && source.at("SourceDetails").is_object())
                name = source.at("SourceDetails").value("Name", std::string());
            if (!source.contains("Packages") || !source.at("Packages").is_array()) continue;
            for (const json& p : source.at("Packages")) {
                if (!p.is_object()) continue;
                WingetPackage pkg;
                pkg.id = p.value("PackageIdentifier", std::string());
                pkg.version = p.value("Version", std::string());
                pkg.source = name;
                if (pkg.version == "Unknown") pkg.version.clear();
                if (valid_winget_id(pkg.id)) out.push_back(std::move(pkg));
            }
        }
    } catch (const json::exception& e) {
        throw Error(std::string("cannot read the winget package list: ") + e.what());
    }
    return out;
}

std::vector<InstalledApp> merge_apps(std::vector<InstalledApp> programs, const std::vector<WingetPackage>& packages) {
    // Names a package may go by: the id without its publisher part ("Notepad++.Notepad++" ->
    // "notepad"), compared with the program's name with and without the publisher in front
    // ("Microsoft OneDrive" for "Microsoft.OneDrive"). Only whole names match.
    std::vector<std::string> names(programs.size());
    for (std::size_t i = 0; i < programs.size(); ++i) names[i] = normalize_app_name(programs[i].name);
    for (const WingetPackage& pkg : packages) {
        const std::size_t dot = pkg.id.find('.');
        const std::string publisher = dot == std::string::npos ? std::string() : alnum_only(pkg.id.substr(0, dot));
        const std::string rest = alnum_only(dot == std::string::npos ? pkg.id : pkg.id.substr(dot + 1));
        bool matched = false;
        if (!rest.empty()) {
            for (std::size_t i = 0; i < programs.size() && !matched; ++i) {
                if (!programs[i].winget_id.empty()) continue;
                const std::string& n = names[i];
                const bool hit = n == rest || (!publisher.empty() && n.size() > publisher.size() &&
                                               n.starts_with(publisher) && n.substr(publisher.size()) == rest);
                if (hit) {
                    programs[i].winget_id = pkg.id;
                    programs[i].source = pkg.source;
                    matched = true;
                }
            }
        }
        if (!matched) {
            InstalledApp app;
            app.name = pkg.id;
            app.version = pkg.version;
            app.winget_id = pkg.id;
            app.source = pkg.source;
            programs.push_back(std::move(app));
        }
    }
    return programs;
}

bool valid_winget_id(std::string_view id) {
    if (id.empty() || id.size() > 128 || !is_alnum(id[0])) return false;
    return std::all_of(id.begin(), id.end(),
                       [](char c) { return is_alnum(c) || c == '.' || c == '_' || c == '-' || c == '+'; });
}

bool valid_winget_source(std::string_view source) {
    return source == "winget" || source == "msstore";
}

std::string apps_to_json(std::vector<InstalledApp> apps) {
    std::sort(apps.begin(), apps.end(), [](const InstalledApp& a, const InstalledApp& b) {
        return std::make_tuple(lower(a.name), a.name, a.winget_id, a.version, a.publisher) <
               std::make_tuple(lower(b.name), b.name, b.winget_id, b.version, b.publisher);
    });
    apps.erase(std::unique(apps.begin(), apps.end(), same_app), apps.end());
    json list = json::array();
    for (const InstalledApp& a : apps) {
        json o = {{"name", a.name}};
        if (!a.version.empty()) o["version"] = a.version;
        if (!a.publisher.empty()) o["publisher"] = a.publisher;
        if (!a.winget_id.empty()) o["id"] = a.winget_id;
        if (!a.source.empty()) o["source"] = a.source;
        list.push_back(std::move(o));
    }
    return json{{"version", 1}, {"apps", std::move(list)}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::vector<InstalledApp> apps_from_json(std::string_view text) {
    std::vector<InstalledApp> out;
    try {
        const json j = json::parse(text);
        if (!j.is_object() || j.value("version", 0) != 1 || !j.contains("apps") || !j.at("apps").is_array())
            throw Error("unsupported program list");
        for (const json& o : j.at("apps")) {
            if (!o.is_object()) continue;
            InstalledApp a;
            a.name = o.value("name", std::string());
            a.version = o.value("version", std::string());
            a.publisher = o.value("publisher", std::string());
            a.winget_id = o.value("id", std::string());
            a.source = o.value("source", std::string());
            if (!a.winget_id.empty() && !valid_winget_id(a.winget_id)) a.winget_id.clear();
            if (!a.source.empty() && !valid_winget_source(a.source)) a.source.clear();
            if (a.name.empty()) continue;
            out.push_back(std::move(a));
        }
    } catch (const json::exception& e) {
        throw Error(std::string("cannot read the program list: ") + e.what());
    }
    return out;
}

std::vector<InstalledApp> missing_apps(const std::vector<InstalledApp>& stored, const std::vector<InstalledApp>& here,
                                       bool here_winget) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const InstalledApp& a : here) {
        if (!a.winget_id.empty()) ids.insert(lower(a.winget_id));
        names.insert(normalize_app_name(a.name));
    }
    std::vector<InstalledApp> out;
    std::unordered_set<std::string> listed;
    for (const InstalledApp& a : stored) {
        if (names.contains(normalize_app_name(a.name))) continue;
        if (!a.winget_id.empty()) {
            if (here_winget && ids.contains(lower(a.winget_id))) continue;
            if (!listed.insert(lower(a.winget_id)).second) continue;
        }
        out.push_back(a);
    }
    return out;
}

AppScan scan_installed_apps(bool use_winget, const std::function<bool()>& cancelled) {
    AppScan scan;
    std::vector<InstalledApp> programs = filter_uninstall_entries(read_uninstall_entries());
    std::optional<std::vector<WingetPackage>> packages;
    if (use_winget) packages = winget_packages(std::chrono::seconds(60), cancelled);
    scan.winget = packages.has_value();
    scan.apps = packages ? merge_apps(std::move(programs), *packages) : std::move(programs);
    return scan;
}

std::optional<std::vector<WingetPackage>> winget_packages(std::chrono::seconds timeout,
                                                          const std::function<bool()>& cancelled) {
    try {
        const std::string winget = winget_program();
        if (winget.empty()) return std::nullopt;
        std::string temp = os::get_env("TEMP");
        if (temp.empty()) return std::nullopt;
        std::uint8_t rnd[8];
        random_bytes(rnd);
        const std::string file = os::join(temp, "vaultik-winget-" + to_hex(ByteSpan(rnd, sizeof rnd)) + ".json");
        struct Remove {
            std::string path;
            ~Remove() { os::remove_file(path); }
        } remove{file};
        std::string output;
        os::CommandOptions o;
        o.output = &output;
        o.timeout = timeout;
        o.cancelled = cancelled;
        os::run_command({winget, "export", "-o", file, "--accept-source-agreements", "--include-versions"}, o);
        if (!os::exists(file)) return std::nullopt;
        const Bytes data = os::read_file(file, 16u << 20);
        return parse_winget_export(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    } catch (const std::exception&) {
        return std::nullopt;  // no list from winget this time; the programs are still listed by name
    }
}

int winget_install(const std::string& id, const std::string& source, std::string* output,
                   const std::function<bool()>& cancelled) {
    if (!valid_winget_id(id)) throw Error("not a winget package id: " + id);
    if (!source.empty() && !valid_winget_source(source)) throw Error("not a winget source: " + source);
    const std::string winget = winget_program();
    if (winget.empty()) throw Error("winget is not available for this account (install App Installer from the Microsoft Store)");
    std::vector<std::string> argv = {winget, "install", "--id", id, "-e", "--silent", "--accept-package-agreements",
                                     "--accept-source-agreements"};
    if (!source.empty()) {
        argv.push_back("--source");
        argv.push_back(source);
    }
    os::CommandOptions o;
    o.output = output;
    o.timeout = std::chrono::hours(2);
    o.cancelled = cancelled;
    return os::run_command(argv, o);
}

Id save_app_list(Repository& repo, const std::vector<InstalledApp>& apps) {
    const std::string text = apps_to_json(apps);
    Id id{};
    repo.save_blob(BlobType::Data, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), id);
    return id;
}

std::optional<std::vector<InstalledApp>> load_app_list(Repository& repo, const Snapshot& snapshot) {
    if (!snapshot.apps) return std::nullopt;
    if (!repo.has_blob(*snapshot.apps))
        throw Error("the program list of this version is no longer in the backup");
    const Bytes data = repo.load_blob(BlobType::Data, *snapshot.apps);
    return apps_from_json(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

}  // namespace vaultik
