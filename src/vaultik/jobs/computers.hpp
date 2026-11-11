#pragma once

// Several computers in one view. Computers that back up into the same repository each keep a
// small sealed status record there (status/<computer id>, see docs/FORMAT.md): how their jobs
// last ran and when they run next. Any of them can then read every record and see how all the
// computers' backups are doing, next to what the snapshots themselves tell (versions per host).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/jobs/job.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik::jobs {

// One job of a computer, as its status record tells it. Times are FILETIME ticks (0: never).
struct JobStatus {
    std::string id;
    std::string name;
    std::string schedule;        // describe(Schedule), "Manual" for none
    std::uint64_t last_run = 0;  // when the newest run of any kind ended
    std::string last_kind;       // to_string(RunKind) of that run
    std::string last_result;     // to_string(RunStatus) of that run
    std::string last_message;
    std::uint64_t files = 0;  // counts of that run
    std::uint64_t bytes = 0;
    std::uint64_t new_bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t last_backup = 0;  // the newest backup run
    std::string last_backup_result;
    std::uint64_t last_success = 0;     // the newest backup that saved a version
    std::uint64_t next_run = 0;         // the next scheduled run; 0 for a manual job
    bool damage = false;                // the newest health check or test restore found damage
    std::vector<std::string> warnings;  // drive health, ransomware, damage, in words
};

struct HostStatus {
    std::string host;     // the computer's name
    std::string version;  // the Vaultik version that wrote the record
    std::uint64_t updated = 0;
    std::vector<JobStatus> jobs;  // sorted by name
};

std::string to_json(const HostStatus& status);
std::string to_json(const JobStatus& job);
HostStatus host_status_from_json(const std::string& text);  // throws Error

// The computer's id in this repository: BLAKE3 keyed with the repository's id key over
// "VKHOST01" || repo_id || machine id. Stable for a computer, different in every repository, so
// records in two repositories cannot be linked to the same computer.
Id computer_id(const Repository& repo, std::string_view machine_id);
// "status/<64 hex>".
std::string status_name(const Id& computer);

// Sealed with AAD "VKSTAT01" || computer id, so a record cannot be moved to another name.
Bytes seal_status(Repository& repo, const Id& computer, const HostStatus& status);
HostStatus open_status(Repository& repo, const Id& computer, ByteSpan sealed);  // throws AuthError, Error
void write_status(Repository& repo, const Id& computer, const HostStatus& status);
// Empty when the computer has no record.
std::optional<HostStatus> read_status(Repository& repo, const Id& computer);

struct StatusFile {
    std::string name;                  // "status/<hex>"
    std::optional<HostStatus> status;  // empty when it cannot be read
    std::string error;
};
std::vector<StatusFile> list_statuses(Repository& repo);

// When the schedule starts the job next after `now` (FILETIME ticks); empty for a manual job.
// Mirrors the Task Scheduler triggers (an hourly job repeats from 2026-01-01 00:<minute>, local).
std::optional<std::uint64_t> next_run(const Schedule& schedule, std::uint64_t now);

// The job's entry, from its history (oldest first).
JobStatus job_status(const Job& job, const std::vector<RunRecord>& history, std::uint64_t now);

// Puts `job` into the record of `computer` (read, the job's entry replaced, written back), with
// the given host name and version. Entries of other jobs stay.
void update_status(Repository& repo, const Id& computer, const std::string& host, const std::string& version,
                   const JobStatus& job, std::uint64_t now);
// The same for this computer. Never throws: a status that cannot be written must not fail a run.
// Returns why it failed, or empty.
std::string publish_status(Repository& repo, const Job& job, const std::vector<RunRecord>& history,
                           const std::string& version) noexcept;

enum class Health { Ok, Unknown, Warning, Stale, Failed };  // from best to worst
const char* to_string(Health health);
inline Health worst(Health a, Health b) {
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}
// A scheduled job is stale when its next run is this long overdue.
constexpr std::uint64_t kStaleTicks = 3ull * 24 * 3600 * 10000000;

// Failed: the newest backup failed, or a check found damage. Stale: the job has not run for
// kStaleTicks past its next scheduled run. Warning: a problem that saved a version anyway (files
// skipped, a cancelled run, a drive or ransomware warning, a failed copy), or no version yet.
Health job_health(const JobStatus& job, std::uint64_t now);

// A computer, from its status record and the snapshots that name it as their host.
struct Computer {
    std::string host;
    bool this_computer = false;
    std::optional<HostStatus> status;  // empty: no record (older Vaultik, or no job ran yet)
    std::string status_name;           // empty: no record
    std::size_t versions = 0;
    std::uint64_t latest = 0;        // the newest version's time
    std::uint64_t latest_bytes = 0;  // the newest version's size
    std::uint64_t added_bytes = 0;   // bytes its versions added to the repository
    Health health = Health::Unknown;
};

// Joins status records and snapshots by host name (case-insensitively): one entry per record,
// plus one per host that only has snapshots. Sorted by host name. `this_record` (a status name)
// and `this_host` mark this computer's entry.
std::vector<Computer> merge_computers(const std::vector<StatusFile>& statuses, const std::vector<Snapshot>& snapshots,
                                      std::uint64_t now, std::string_view this_record, std::string_view this_host);
// Reads everything from the repository (snapshots that cannot be read are left out).
std::vector<Computer> list_computers(Repository& repo, std::uint64_t now);

}  // namespace vaultik::jobs
