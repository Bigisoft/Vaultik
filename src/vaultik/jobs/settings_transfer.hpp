#pragma once

// Settings and plans to another computer: one file (Vaultik-settings-<computer>-<date>
// .vaultik-settings) with the frontend's settings, the alert settings and every saved job, so a
// new computer is set up like the old one.
//
// The file is JSON. Without secrets it holds no password, cloud storage key, database password or
// alert token at all. With them, everything it holds is sealed with AES-256-GCM under a key
// derived from a password the user types (PBKDF2-HMAC-SHA256, as for a repository's key), so
// nothing is ever written in clear:
//   {"format": "vaultik-settings", "version": 1, "encrypted": false, "content": {...}}
//   {"format": "vaultik-settings", "version": 1, "encrypted": true, "kdf": "pbkdf2-sha256",
//    "iterations": n, "salt": "<hex>", "data": "<base64 of nonce, ciphertext and tag>"}
// content: {"computer", "created", "app_version", "app": {...}, "alerts": {...}, "jobs": [...],
//           "secrets": {"<job id>": {...}, ...}, "alert_secrets": {...}}
//
// Imported jobs keep their schedule but start paused, so nothing runs before the user checked
// their folders on the new computer.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

constexpr int kSettingsFormatVersion = 1;
constexpr const char* kSettingsExtension = ".vaultik-settings";

struct JobSecrets {
    std::string password;
    std::string cloud;  // the cloud storage secret key or sign-in token
    std::string copy;   // the same for the second copy
    std::map<std::string, std::string> databases;  // by database dump id
    void wipe();
};

struct SettingsBundle {
    std::string computer;
    std::uint64_t created = 0;  // FILETIME ticks
    std::string app_version;
    nlohmann::json app = nlohmann::json::object();  // the frontend's settings; empty from the command line
    std::optional<AlertSettings> alerts;
    std::vector<Job> jobs;
    bool with_secrets = false;
    std::map<std::string, JobSecrets> secrets;           // by job id (with_secrets only)
    std::map<std::string, std::string> alert_secrets;    // "ntfy", "telegram" (with_secrets only)
    ~SettingsBundle();
};

// What `store` holds now (and its alert settings), with the secrets when asked for.
SettingsBundle collect_settings(const JobStore& store, bool with_secrets, const nlohmann::json& app = nlohmann::json::object());

// "Vaultik-settings-<computer>-<YYYY-MM-DD>.vaultik-settings", file-name safe.
std::string default_settings_file_name(const std::string& computer, std::uint64_t when);

// The file's bytes. A bundle with secrets needs a password (at least 8 characters); without
// secrets, `password` must be empty (such a file is not encrypted). Throws Error.
Bytes write_settings_file(const SettingsBundle& bundle, const std::string& password);
// Whether the file is encrypted (it holds secrets), so a password must be asked for. Throws Error
// when it is not a settings file or its format version is not one this Vaultik reads.
bool settings_file_encrypted(ByteSpan file);
// Reads the file. Throws AuthError for a wrong password (or a damaged file), Error for anything
// else: not a settings file, a newer or older format, a password missing.
SettingsBundle read_settings_file(ByteSpan file, const std::string& password);

struct ImportChoice {
    std::vector<std::string> jobs;  // the ids to import; empty: all of them
    bool replace = false;           // a job whose id is taken replaces that job (else it is renamed)
    bool alerts = true;             // also the alert settings (when the file has them)
};
struct ImportedJob {
    std::string from_id;  // in the file
    std::string id;       // here (another one when renamed)
    std::string name;
    bool replaced = false;
    bool renamed = false;
    bool secrets = false;  // its password (and keys) came with it
};
struct ImportResult {
    std::vector<ImportedJob> jobs;
    bool alerts = false;
};
// Writes the chosen jobs into `store`, paused, with their secrets when the file has them. The
// caller then applies their schedules (jobs::apply_schedule), which removes the tasks of a job
// that was replaced. Throws Error for an id not in the file.
ImportResult import_settings(const JobStore& store, const SettingsBundle& bundle, const ImportChoice& choice);

}  // namespace vaultik::jobs
