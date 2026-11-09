#pragma once

// Saved backup jobs: what to back up, where to, when, and with which resource limits.
//
// Jobs live in the per-user config directory (config_dir()) as one JSON file each:
//   jobs/<id>.json        the job definition
//   secrets/<id>.key      the repository password (DPAPI-encrypted on Windows, mode 0600 elsewhere)
//   history/<id>.jsonl    one line per run, newest last

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/ops/prune.hpp"
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

struct Job {
    std::string id;    // file-name safe, derived from the name when created
    std::string name;  // shown in the UI
    std::string repository;
    std::vector<std::string> sources;
    std::vector<std::string> excludes;
    std::vector<std::string> tags;
    // Windows: read from a Volume Shadow Copy when the run has administrator rights, so open and
    // locked files are backed up too. Without the rights the files are read live.
    bool vss = true;
    Schedule schedule;
    // Windows: the scheduled task runs with administrator rights (so the shadow copy above works).
    // Registering such a task asks for administrator approval each time the schedule is saved.
    bool run_as_admin = false;
    ResourcePolicy resources;
    // Old snapshots of this job to remove after each run (then unused data is pruned). Empty keeps
    // everything.
    RetentionPolicy retention;
};

// The retention a new job starts with: 7 daily, 4 weekly and 12 monthly snapshots.
RetentionPolicy default_retention();

enum class RunStatus { Success, Partial, Failed, Cancelled };

struct RunRecord {
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

const char* to_string(RunStatus status);
const char* to_string(Schedule::Kind kind);
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

    void append_history(const std::string& id, const RunRecord& record) const;
    // Most recent last; at most `limit` records.
    std::vector<RunRecord> history(const std::string& id, std::size_t limit = 100) const;

private:
    std::string dir_;
};

}  // namespace vaultik::jobs
