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
    // Conditions for scheduled runs (a backup started by hand always runs):
    bool skip_on_battery = false;  // not while the computer runs on battery
    bool wake_computer = false;    // wake it from sleep at the scheduled time
    bool skip_metered = false;     // cloud storage: not over a metered (paid by the byte) connection
    // Windows: also back up, at most once every event_gap_hours, when the computer has been idle
    // for a while or the user locks it (both Task Scheduler triggers of a second task), and when
    // the user signs out or shuts down (the tray holds that up until the backup is done).
    bool on_idle = false;
    bool on_lock = false;
    bool on_sign_out = false;
    // Windows: also back up when Windows Update starts installing an update (an event trigger of the
    // same second task; see scheduler.hpp). Windows does not wait for the backup: it runs alongside
    // the installation, and the tray holds up the restart that follows while it is still running.
    bool on_update = false;
    int event_gap_hours = 4;  // 1..168
};

// Whether the job has anything for the system scheduler to start (a time, idle, lock or updates).
bool has_system_schedule(const Schedule& s);

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
    std::string cloud_storage_class;  // S3 only, see Job::cloud_storage_class
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
    // S3 only: the storage class for packs of file data (CloudCredentials::storage_class); empty
    // for the standard class.
    std::string cloud_storage_class;
    std::vector<std::string> sources;
    std::vector<std::string> excludes;
    // A disk image job (Windows, administrator rights): whole disks instead of `sources`, by their
    // stable id ("system" for the disk Windows runs from, "gpt-<GUID>", "mbr-<signature>"; see
    // image/disk.hpp). Empty for a job backing up files.
    std::vector<std::string> image_disks;
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
    // Windows: back up continuously, `continuous_minutes` after files in the job's folders change
    // (watched by the tray, so Vaultik must be running).
    bool continuous = false;
    int continuous_minutes = 5;  // 1..1440
    // Save a version only when something changed since the last one (a run that finds nothing
    // changed is recorded as a success without a version).
    bool skip_unchanged = false;
    // Ask every file for its real size and times instead of trusting the folder listing (see
    // BackupOptions::check_every_file): for folders with hard links, such as package stores.
    bool check_every_file = false;
    // Keep the list of programs installed on this computer with each version (see ops/apps.hpp),
    // so they can be installed again on a new one.
    bool record_apps = true;
    // Windows: WSL distributions to back up through "wsl --export" (see ops/virt.hpp), each as the
    // file WSL/<name>.tar of a version, and what to do with one that is running: "skip" it (with a
    // warning), "export" it as it runs, or "stop" it first. WSL belongs to the user's own account,
    // so a job with administrator rights (which runs as the system account) cannot have any.
    std::vector<std::string> wsl_distros;
    std::string wsl_when_running = "skip";
    // Windows: Hyper-V virtual machines to back up through Export-VM, each as the folder
    // Hyper-V/<name>/ of a version. Needs run_as_admin. They are exported into `hyperv_staging`
    // first (empty: <ProgramData>\Vaultik\hyperv-export), which needs room for the largest one,
    // and removed from there after the backup.
    std::vector<std::string> hyperv_vms;
    std::string hyperv_staging;
    // Repair data written with each pack, in percent of its size (0: none; see
    // Repository::set_parity_percent). A backup sets the destination's setting to this.
    unsigned repair_data = 5;
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

struct RunRecord;
// Whether JobStore::append_history also writes each run to the system's log (os::system_log).
// Off by default (tests); the command line turns it on.
void log_runs_to_system(bool on);
// The system log entry for a run: its level, event id and text (see system_log_entry in job.cpp).
struct SystemLogEntry {
    int level = 0;  // os::LogLevel
    std::uint32_t id = 0;
    std::string text;
};
SystemLogEntry system_log_entry(const RunRecord& record, const std::string& job_name, const std::string& job_id);

struct RunRecord {
    RunKind kind = RunKind::Backup;
    bool on_connect = false;  // started because the destination drive was connected
    // What else started it: "event" (the computer idle or locked), "update" (Windows Update started
    // installing), "sign-out", "change" (files changed, for continuous backups); empty for a
    // schedule, by hand or on_connect.
    std::string trigger;
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
    // A backup: the stored size of the whole destination after it (and after its cleanup), from
    // the repository's index; 0 when not known (older runs, other kinds of runs).
    std::uint64_t backup_size = 0;
    // A backup to a folder: the space left on the destination's drive after it (for the forecast of
    // when the drive is full, see forecast.hpp); 0 when not known (cloud storage, older runs).
    std::uint64_t free_space = 0;
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
