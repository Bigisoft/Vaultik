#pragma once

// The programs installed on a computer, kept with each version so they can be installed again on
// a new one: read from the Windows uninstall list and, where the signed-in user has it, matched to
// winget packages (which `vaultik apps install` installs with winget).

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct InstalledApp {
    std::string name;       // as the uninstall list shows it; the package id for winget-only ones
    std::string version;
    std::string publisher;
    std::string winget_id;  // empty when no winget package is known for it
    std::string source;     // the winget source of winget_id ("winget", "msstore")
};

// One entry of an Uninstall registry key, as read.
struct UninstallEntry {
    std::string key_name;
    std::string display_name;
    std::string display_version;
    std::string publisher;
    std::string parent_key_name;
    std::string release_type;
    bool system_component = false;  // SystemComponent = 1
};

struct WingetPackage {
    std::string id;
    std::string version;
    std::string source;  // the source's name
};

// The programs worth listing: entries with a name that are no system component, no part of another
// program (ParentKeyName) and no update (KB numbers, ReleaseType), each name and version once.
std::vector<InstalledApp> filter_uninstall_entries(const std::vector<UninstallEntry>& entries);

// The packages of a `winget export` file. Throws Error on JSON that is not one.
std::vector<WingetPackage> parse_winget_export(std::string_view json);

// The programs with their winget package where one matches by name; packages that match none are
// added under their id.
std::vector<InstalledApp> merge_apps(std::vector<InstalledApp> programs, const std::vector<WingetPackage>& packages);

// A name for comparing: lower case letters and digits only, without parts in brackets and
// version numbers ("7-Zip 24.08 (x64)" -> "7zip").
std::string normalize_app_name(std::string_view name);

// A winget package id or source name that may be passed to winget: 1 to 128 characters out of
// letters, digits and . _ - +, starting with a letter or digit.
bool valid_winget_id(std::string_view id);
bool valid_winget_source(std::string_view source);

// The stored form: compact JSON, sorted, so an unchanged list is the same blob.
std::string apps_to_json(std::vector<InstalledApp> apps);
std::vector<InstalledApp> apps_from_json(std::string_view json);  // throws Error

// The programs of `stored` that are not on this computer (`here`): by winget id when both sides
// know one (and `here_winget` says the list here came with winget), else by normalized name.
std::vector<InstalledApp> missing_apps(const std::vector<InstalledApp>& stored, const std::vector<InstalledApp>& here,
                                       bool here_winget);

// ---- this computer ------------------------------------------------------------------------

// The entries of the Uninstall keys (HKLM in the 64 and 32 bit views, HKCU). Empty on POSIX.
std::vector<UninstallEntry> read_uninstall_entries();

// winget.exe of the signed-in user, or empty when there is none, this runs as the system account
// (which has no winget) or with administrator rights (winget is found in folders the account's
// programs without those rights can change, so it is not started then). Windows only.
std::string winget_program();

// The winget packages installed here, from `winget export`; nothing when winget is missing, fails
// or takes longer than `timeout`.
std::optional<std::vector<WingetPackage>> winget_packages(std::chrono::seconds timeout = std::chrono::seconds(60),
                                                          const std::function<bool()>& cancelled = {});

struct AppScan {
    std::vector<InstalledApp> apps;
    bool winget = false;  // the winget packages were read
};
// The programs installed here; with `use_winget`, matched to winget packages (given up on when
// `cancelled` says so).
AppScan scan_installed_apps(bool use_winget, const std::function<bool()>& cancelled = {});

// Installs one package with winget, silently, as the current user (winget asks for administrator
// approval itself when the installer needs it). Returns winget's exit code; `output` gets its
// messages. Throws Error for an id or source that fails valid_winget_id, or when winget is missing.
int winget_install(const std::string& id, const std::string& source, std::string* output = nullptr,
                   const std::function<bool()>& cancelled = {});

// ---- repository ---------------------------------------------------------------------------

// Saves the list as a Data blob and returns its id (the same id for the same list).
Id save_app_list(Repository& repo, const std::vector<InstalledApp>& apps);
// The list a snapshot recorded; nothing when it recorded none. Throws Error when the blob is lost.
std::optional<std::vector<InstalledApp>> load_app_list(Repository& repo, const Snapshot& snapshot);

}  // namespace vaultik
