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
// the given host name and version. Entries of other jobs stay. Two jobs of one computer ending
// together would each write the record they read, and one entry would be lost: so the record is
// read back after writing, and when the entry is not in it (another writer replaced the file
// meanwhile) merged and written again, a few times with a random pause.
void update_status(Repository& repo, const Id& computer, const std::string& host, const std::string& version,
                   const JobStatus& job, std::uint64_t now);
// The same for this computer. Never throws: a status that cannot be written must not fail a run.
// Returns why it failed, or empty.
std::string publish_status(Repository& repo, const Job& job, const std::vector<RunRecord>& history,
                           const std::string& version) noexcept;

// A request from one computer to another sharing the repository: run these jobs once, now. Kept
// at status/requests/<computer id> (one per computer; a new request replaces an older one),
// sealed with AAD "VKREQ001" || computer id. The asked computer looks for it when its jobs on
// the repository run and when the frontend polls; it runs the jobs it has with these ids that
// back up to this repository, removing each from the request when it ran, and the request when
// none is left. A request older than kRequestExpiry is dropped.
struct BackupRequest {
    std::string from;                // the asking computer's name
    std::uint64_t created = 0;       // when it asked
    std::vector<std::string> jobs;   // ids of the asked computer's jobs still to run
    std::string state = "pending";   // "pending", "running" (the asked computer took it up)
    std::uint64_t started = 0;       // when it was taken up
};
constexpr std::uint64_t kRequestExpiry = 7ull * 24 * 3600 * 10000000;

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
    // Its status record is older than its newest version: every run writes the record after its
    // version, so either the record could not be written, or the storage side put an older,
    // genuine record back (records are sealed, but a copy of an old one still opens). What the
    // record says may then be out of date; health is at least Warning.
    bool record_behind = false;
    std::optional<BackupRequest> request;  // a request to back up addressed to it
};

// Joins status records and snapshots by host name (case-insensitively): one entry per record,
// plus one per host that only has snapshots. Sorted by host name. `this_record` (a status name)
// and `this_host` mark this computer's entry.
std::vector<Computer> merge_computers(const std::vector<StatusFile>& statuses, const std::vector<Snapshot>& snapshots,
                                      std::uint64_t now, std::string_view this_record, std::string_view this_host);
// Reads everything from the repository (snapshots that cannot be read are left out).
std::vector<Computer> list_computers(Repository& repo, std::uint64_t now);

// ---- Removing a retired computer ----------------------------------------------------------

// Which status record `what` names: a status name ("status/<hex>"), the computer id in hex (all
// of it, or its first 8 or more digits), or a host name (ignoring case; it must name one record).
// Throws Error when nothing or several records match.
std::string resolve_computer(const std::vector<StatusFile>& statuses, std::string_view what);
// The same for removing it: also throws when the record is `this_record` (this computer's own:
// its next run would write it again, and its jobs still run).
std::string resolve_forget(const std::vector<StatusFile>& statuses, std::string_view what,
                           std::string_view this_record);
// Removes that record and a request addressed to that computer (see below); the snapshots stay.
// Returns the removed record's host name ("?" when it could not be read). Status records are not
// under S3 Object Lock, so this works on locked cloud storage too.
std::string forget_computer(Repository& repo, std::string_view what);

// ---- Asking another computer to back up now ------------------------------------------------

// The request (see BackupRequest above) and what to do with it.

std::string to_json(const BackupRequest& request);
BackupRequest request_from_json(const std::string& text);  // throws Error
// "status/requests/<64 hex>".
std::string request_name(const Id& computer);
void write_request(Repository& repo, const Id& computer, const BackupRequest& request);
// Empty when there is none. Throws AuthError when it was changed, Error when malformed.
std::optional<BackupRequest> read_request(Repository& repo, const Id& computer);
void remove_request(Repository& repo, const Id& computer);
// The computer id in a status name ("status/<hex>"); throws Error for another name.
Id status_computer(const std::string& status_name);

// Asks the computer of `status` (a record from list_statuses) to run `jobs` (ids from its record;
// all of its jobs when empty). Throws Error when it names no job, or when the record is this
// computer's own (start a job here instead).
void ask_backup(Repository& repo, const StatusFile& status, std::vector<std::string> jobs, const std::string& from,
                std::uint64_t now);

// What a request asks of this computer: the ids among `local` (this computer's jobs on this
// repository) it lists, unless it expired. Empty when none.
std::vector<std::string> requested_jobs(const BackupRequest& request, const std::vector<std::string>& local,
                                        std::uint64_t now);

// Requests carry no secret the storage side lacks to repeat them: it can put back a copy of an
// old, genuine one. So this computer remembers when the last request it took up was made
// (`taken`, 0: none) and takes up only newer ones: Take a newer one, Keep the one it took up
// (marked "running" then; its runs may still be going; it is not run again), Drop an older one
// and the one it took up when it shows as "pending" again (the asker's copy put back).
enum class RequestStep { Take, Keep, Drop };
RequestStep request_step(const BackupRequest& request, std::uint64_t taken);
// The request after `job_id` ran: without that job; empty when nothing is left.
std::optional<BackupRequest> after_run(BackupRequest request, const std::string& job_id);

// This computer, at a poll: the request addressed to it, if any; the job ids among `runnable` it
// asks for (marking it taken up, and remembering that in `state_dir`, see RequestStep), or empty.
// An expired request, one for jobs this computer does not have here (`known`: all of its jobs on
// this repository, those with administrator rights too, whose own runs answer it), and a copy of
// an older one are removed. Never throws: a request that cannot be read is left alone.
std::vector<std::string> take_request(Repository& repo, const std::vector<std::string>& known,
                                      const std::vector<std::string>& runnable, std::uint64_t now,
                                      const std::string& state_dir = config_dir()) noexcept;
// After `job_id` ran on this computer: removes it from the request addressed here (and the request
// when it was the last). Never throws.
void finish_request(Repository& repo, const std::string& job_id) noexcept;

// A request in the view: whom it asks, its state.
struct RequestFile {
    std::string target;  // the asked computer's status name ("status/<hex>")
    std::optional<BackupRequest> request;  // empty when it cannot be read
};
std::vector<RequestFile> list_requests(Repository& repo);

}  // namespace vaultik::jobs
