#pragma once

// Saved backup jobs: what to back up, where to, when, and with which resource limits.
//
// Jobs live in the per-user config directory (config_dir()) as one JSON file each:
//   jobs/<id>.json        the job definition
//   secrets/<id>.key      the repository password (DPAPI-encrypted on Windows, mode 0600 elsewhere)
//   secrets/<id>.cloud    the cloud storage secret key, sealed the same way
//   history/<id>.jsonl    one line per run, newest last

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/ops/prune.hpp"
#include "vaultik/repo/s3_backend.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik::jobs {

struct Schedule {
    enum class Kind { Manual, Hourly, Daily, Weekly };
    Kind kind = Kind::Manual;
    int hour = 2;         // Daily, Weekly: local time
    int minute = 0;       // all kinds (Hourly: minute past the hour)
    std::uint8_t weekdays = 0x7F;  // Weekly: bit 0 = Monday ... bit 6 = Sunday
    int every_hours = 1;  // Hourly: 1-23
};

// Health checks run after a scheduled backup when they are due: a weekly check reads a share of
// the stored data back and compares it with its hashes, a monthly full check reads all of it.
// Both also check that every version's structure is complete. Reading from cloud storage costs
// downloads, so a cloud job starts without the monthly full check.
struct HealthPolicy {
    bool weekly = true;
    int sample_percent = 10;  // 1..100: the share of stored data the weekly check reads
    bool monthly = true;
};

struct Job {
    std::string id;    // file-name safe, derived from the name when created
    std::string name;  // shown in the UI
    std::string repository;  // a folder, or cloud storage (see repo/cloud.hpp for the forms)
    // Cloud storage: for S3 the access key ID and signing region (empty: from the endpoint); for
    // the other services the signed-in account. The secret key or sign-in token is stored like the
    // password (JobStore::set_cloud_secret).
    std::string cloud_key_id;
    std::string cloud_region;
    std::vector<std::string> sources;
    std::vector<std::string> excludes;
    std::vector<std::string> tags;
    // Windows: read from a Volume Shadow Copy when the run has administrator rights, so open and
    // locked files are backed up too. Without the rights the files are read live.
    bool vss = true;
    // Leave SQL Server out of the shadow copy, so it is not paused (see os::ShadowCopy::create).
    bool vss_skip_sql = false;
    Schedule schedule;
    // Windows: the scheduled task runs with administrator rights (so the shadow copy above works).
    // Registering such a task asks for administrator approval each time the schedule is saved.
    bool run_as_admin = false;
    ResourcePolicy resources;
    // Old snapshots of this job to remove after each run (then unused data is pruned). Empty keeps
    // everything.
    RetentionPolicy retention;
    HealthPolicy health;
};

// The retention a new job starts with: 7 daily, 4 weekly and 12 monthly snapshots.
RetentionPolicy default_retention();

// The health checks a job saving to `repository` starts with: weekly 10%, and the monthly full
// check only for a folder (a full check of cloud storage downloads everything).
HealthPolicy default_health(std::string_view repository);

enum class RunStatus { Success, Partial, Failed, Cancelled };

// What a run did: a backup, a health check that read a sample of the data, or one that read all.
enum class RunKind { Backup, Check, FullCheck };

struct RunRecord {
    RunKind kind = RunKind::Backup;
    std::uint64_t started = 0;   // FILETIME ticks
    std::uint64_t finished = 0;
    RunStatus status = RunStatus::Success;
    std::string snapshot;  // snapshot id (hex) when one was saved
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::uint64_t new_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t errors = 0;
    std::string message;  // error text for Failed, summary otherwise
};

// Per-user config directory; VAULTIK_CONFIG_DIR overrides it (tests, portable installs).
// Windows: %LOCALAPPDATA%\Vaultik. Linux: $XDG_CONFIG_HOME/vaultik or ~/.config/vaultik.
std::string config_dir();

// A file-name safe id from a display name ("My Docs!" -> "my-docs"), unique among `taken`.
std::string make_id(const std::string& name, const std::vector<std::string>& taken);

// Throws Error describing the first problem (empty name, no sources, bad schedule, ...).
void validate(const Job& job);

std::string to_json(const Job& job);
Job job_from_json(const std::string& text);
std::string to_json(const RunRecord& record);
RunRecord record_from_json(const std::string& text);

// The newest backup run, or the newest health check, in a history (oldest first); null if none.
const RunRecord* last_backup(const std::vector<RunRecord>& history);
const RunRecord* last_check(const std::vector<RunRecord>& history);
// A health check that found damaged or missing data (not one that could not run).
inline bool found_damage(const RunRecord& r) {
    return r.kind != RunKind::Backup && r.status == RunStatus::Failed && r.errors > 0;
}

const char* to_string(RunStatus status);
const char* to_string(RunKind kind);
const char* to_string(Schedule::Kind kind);

// The health check due now (FILETIME ticks), judged from the job's history (oldest first): a full
// check when the monthly one is on and no full check finished in the last 30 days, otherwise a
// sampled check when the weekly one is on and no check of either kind finished in the last 7
// days. A cancelled check does not count, so it is tried again after the next backup.
std::optional<RunKind> due_check(const HealthPolicy& policy, const std::vector<RunRecord>& history,
                                 std::uint64_t now);
// "Daily at 02:00", "Mon, Wed at 21:30", "Every 4 hours at :15", "Manual".
std::string describe(const Schedule& schedule);

class JobStore {
public:
    explicit JobStore(std::string dir = config_dir());

    const std::string& dir() const { return dir_; }

    std::vector<Job> list() const;  // sorted by name; unreadable files are skipped
    std::optional<Job> find(const std::string& id_or_name) const;
    // Validates and writes the job. Assigns an id when it has none.
    void save(Job& job) const;
    // Removes the job, its password and its history.
    void remove(const std::string& id) const;

    void set_password(const std::string& id, const std::string& password) const;
    // Empty when no password is stored.
    std::string password(const std::string& id) const;

    // The cloud storage secret key, sealed like the password (secrets/<id>.cloud). Empty when none.
    void set_cloud_secret(const std::string& id, const std::string& secret) const;
    std::string cloud_secret(const std::string& id) const;
    // The job's cloud storage keys (empty for a folder destination).
    CloudCredentials cloud_credentials(const Job& job) const;

    void append_history(const std::string& id, const RunRecord& record) const;
    // Most recent last; at most `limit` records.
    std::vector<RunRecord> history(const std::string& id, std::size_t limit = 100) const;

private:
    std::string dir_;
};

}  // namespace vaultik::jobs
