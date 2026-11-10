#pragma once

// Saved backup jobs: what to back up, where to, when, and with which resource limits.
//
// Jobs live in the per-user config directory (config_dir()) as one JSON file each:
//   jobs/<id>.json        the job definition
//   secrets/<id>.key      the repository password (DPAPI-encrypted on Windows, mode 0600 elsewhere)
//   secrets/<id>.cloud    the cloud storage secret key, sealed the same way
//   history/<id>.jsonl    one line per run, newest last

#include <cstdint>
#include <utility>
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
    // A monthly test restore: a few files restored into a temporary folder and checked (see
    // ops/restore_test.hpp). Off by default; "vaultik test-restore" runs one any time.
    bool restore_test = false;
};

// Lower speed limits while people work: on the given days (bit 0 = Monday) from start_minute to
// end_minute (minutes after local midnight), a run reads and writes at most these rates (bytes
// per second; 0 keeps the job's normal limit for that direction).
struct WorkHours {
    bool enabled = false;
    std::uint8_t weekdays = 0x1F;  // Monday to Friday
    int start_minute = 9 * 60;
    int end_minute = 17 * 60;
    std::uint64_t read_rate = 0;
    std::uint64_t write_rate = 0;
};

// Whether a local time (weekday 0 = Monday, minute after midnight) falls in the working hours.
bool in_work_hours(const WorkHours& w, int weekday, int minute);
// The speed limits that apply at the current local time: the lower of the job's own and, during
// working hours, the working-hours ones (0 means no limit).
std::pair<std::uint64_t, std::uint64_t> rate_limits_now(const ResourcePolicy& normal, const WorkHours& w);

// A second copy of the job's backup (3-2-1), made after each run: another folder or cloud
// storage, a repository of its own with the same password (created on first use). Its cloud keys
// are stored like the job's (JobStore::set_copy_secret).
struct SecondCopy {
    std::string repository;  // empty: no second copy
    std::string cloud_key_id;
    std::string cloud_region;
    int cloud_lock_days = 0;
    bool cloud_lock_compliance = false;
    // A folder copy's drive, recorded like Job::repository_volume, so the copy is found under
    // another letter and never created on a different drive that took the letter meanwhile.
    std::string volume;
    std::string relative;
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
    // S3 only: keep uploaded data locked (Object Lock) for this many days; 0 for none. See
    // CloudCredentials::lock_days.
    int cloud_lock_days = 0;
    bool cloud_lock_compliance = false;  // compliance mode instead of governance (see there)
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
    WorkHours work_hours;
    SecondCopy second_copy;
    // Old snapshots of this job to remove after each run (then unused data is pruned). Empty keeps
    // everything.
    RetentionPolicy retention;
    HealthPolicy health;
    // A folder destination's drive, recorded when the job is saved (see os::volume_of), so the job
    // finds its backup when the drive comes back with another letter. Empty for cloud storage,
    // network shares and folders not reachable when saved.
    std::string repository_volume;
    std::string repository_relative;  // the folder below the volume's root
    // Windows: also back up as soon as the destination drive is connected (watched by the tray).
    bool backup_on_connect = false;
    // Command lines run by run-job before and after the backup (cmd.exe on Windows): e.g. a
    // database dump before, starting a stopped program again after. A failing "before" command
    // stops the run; "after" runs whatever happened, with VAULTIK_RESULT set to the result.
    std::string before_command;
    std::string after_command;
};

// Points job.repository at the job's drive wherever it is connected now (a changed drive letter);
// leaves it alone when the drive is not connected or no drive was recorded.
void relocate(Job& job);
// Whether the job's recorded drive is connected; true when none was recorded.
bool drive_connected(const Job& job);
// The same for the second copy's drive.
bool copy_drive_connected(const Job& job);
// Records the drive of the second copy's folder now (after it was created there).
void record_copy_volume(Job& job);

// The retention a new job starts with: 7 daily, 4 weekly and 12 monthly snapshots.
RetentionPolicy default_retention();

// Ready-made exclusions, each a set of patterns for Job::excludes (with '/' as the separator,
// which matches '\' on Windows). A job uses a preset when it has all of its patterns.
struct ExclusionPreset {
    std::string id;  // "temp", "caches", "dev", "clutter", "recycle"
    std::vector<std::string> patterns;
    bool default_on = false;  // ticked for a new job
};
const std::vector<ExclusionPreset>& exclusion_presets();

// The health checks a job saving to `repository` starts with: weekly 10%, and the monthly full
// check only for a folder (a full check of cloud storage downloads everything).
HealthPolicy default_health(std::string_view repository);

enum class RunStatus { Success, Partial, Failed, Cancelled };

// What a run did: a backup, a health check that read a sample of the data or all of it, or the
// second copy after a backup.
enum class RunKind { Backup, Check, FullCheck, Copy, RestoreTest };
inline bool is_check(RunKind k) {
    return k == RunKind::Check || k == RunKind::FullCheck;
}

struct RunRecord {
    RunKind kind = RunKind::Backup;
    bool on_connect = false;  // started because the destination drive was connected
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
    // Set when the backup saw signs of ransomware (see ChangeAlarm in ops/backup.hpp); the version
    // before it was pinned and old versions were not removed.
    std::string alert;
    // Set when old versions must not be removed after this backup for another reason: a backup
    // path could not be read at all, or far fewer files were found than before.
    std::string hold;
    // Set when the destination drive reported a problem (its health or condition) at the end of
    // the run, e.g. "WD My Passport 2627: warning (Predictive failure)".
    std::string drive;
};

// Per-user config directory; VAULTIK_CONFIG_DIR overrides it (tests). A portable copy (a file
// named "portable" next to the programs) keeps it in "Settings" there. Otherwise Windows:
// %LOCALAPPDATA%\Vaultik; Linux: $XDG_CONFIG_HOME/vaultik or ~/.config/vaultik.
std::string config_dir();
bool is_portable();

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
const RunRecord* last_copy(const std::vector<RunRecord>& history);
const RunRecord* last_restore_test(const std::vector<RunRecord>& history);
// Whether the monthly test restore is on and none finished (not cancelled) in the last 30 days.
bool restore_test_due(const HealthPolicy& policy, const std::vector<RunRecord>& history, std::uint64_t now);
// A health check that found damaged or missing data (not one that could not run).
inline bool found_damage(const RunRecord& r) {
    return is_check(r.kind) && r.status == RunStatus::Failed && r.errors > 0;
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
    // Empty when no password is stored, or when it was stored on another computer or account
    // (Windows seals it for this account), so the caller asks for it again.
    std::string password(const std::string& id) const;

    // The cloud storage secret key, sealed like the password (secrets/<id>.cloud). Empty when none.
    void set_cloud_secret(const std::string& id, const std::string& secret) const;
    // The same for the second copy's cloud storage (secrets/<id>.copycloud).
    void set_copy_secret(const std::string& id, const std::string& secret) const;
    std::string copy_secret(const std::string& id) const;
    CloudCredentials copy_credentials(const Job& job) const;
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
