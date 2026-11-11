#pragma once

// Retention: forget removes snapshots by a keep policy, prune removes the data no snapshot uses
// any more. Both need an exclusive repository lock (RepoLock::Mode::Exclusive).

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

// How many snapshots to keep. Each rule keeps the newest snapshot of each of the last N hours,
// days, weeks (Monday to Sunday), months or years that have one, in local time; `last` keeps the
// N newest; `within_days` keeps every snapshot of the last N days. A snapshot kept by any rule
// stays, and a pinned snapshot always stays. No rule means "keep everything".
//
// `max_bytes` (jobs only) then removes the oldest unpinned snapshots of the job, never the newest,
// until the data the remaining snapshots use fits (see plan_size_limit).
struct RetentionPolicy {
    int last = 0;
    int hourly = 0;
    int daily = 0;
    int weekly = 0;
    int monthly = 0;
    int yearly = 0;
    int within_days = 0;
    std::uint64_t max_bytes = 0;

    // No count rule: every snapshot is kept by the rules (a size limit may still remove some).
    bool no_rules() const {
        return last <= 0 && hourly <= 0 && daily <= 0 && weekly <= 0 && monthly <= 0 && yearly <= 0 && within_days <= 0;
    }
    bool empty() const { return no_rules() && max_bytes == 0; }
    bool operator==(const RetentionPolicy&) const = default;
};

// "last 3, 7 daily, 4 weekly", or "keep all".
std::string describe(const RetentionPolicy& policy);

struct SnapshotRecord {
    Id id{};
    Snapshot snapshot;
};

struct ForgetDecision {
    SnapshotRecord entry;
    bool keep = false;
    std::string reason;  // the rules that keep it ("daily, weekly"), empty when it goes
};

// Applies the policy separately to each group of snapshots: the snapshots of one saved job
// (tag "job:<id>"), or else those from the same computer with the same paths. Newest first within
// each group.
std::vector<ForgetDecision> plan_forget(std::vector<SnapshotRecord> snapshots, const RetentionPolicy& policy);

struct ForgetOptions {
    RetentionPolicy policy;
    std::vector<std::string> tags;  // only snapshots carrying all of these tags are considered
    // Instead of a policy: exactly these snapshots. A pinned one is refused unless `force`.
    std::vector<Id> ids;
    bool force = false;
    bool dry_run = false;
};

struct ForgetResult {
    std::vector<ForgetDecision> decisions;
    std::size_t removed = 0;
};

// Throws Error for an empty policy rather than removing everything.
ForgetResult run_forget(Repository& repo, const ForgetOptions& options);

// All snapshots that can be read, newest first.
std::vector<SnapshotRecord> load_snapshot_records(Repository& repo);

struct SizePlan {
    std::vector<SnapshotRecord> remove;  // oldest first
    std::uint64_t size_before = 0;       // stored bytes the snapshots use now
    std::uint64_t size_after = 0;        // and after removing `remove`
    bool fits = true;                    // false: even removing every candidate is not enough
};

// For a size limit on the snapshots carrying `tag` (a job's "job:<id>"): the fewest oldest of them
// to remove, never pinned ones and never the newest, so that the data all remaining snapshots in the
// repository use fits in `max_bytes`. Data shared with other snapshots counts as long as any of them
// remains. Walks the folder listings a few times (a binary search); reads nothing else.
SizePlan plan_size_limit(Repository& repo, const std::vector<SnapshotRecord>& all, const std::string& tag,
                         std::uint64_t max_bytes);

// Removes the given snapshots (after plan_size_limit). Returns how many went.
std::size_t remove_snapshots(Repository& repo, const std::vector<SnapshotRecord>& snapshots);

struct PruneOptions {
    bool dry_run = false;
    // A pack whose unused share is above this is rewritten with only the data still in use.
    double repack_unused = 0.25;
    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_warning;
    // Called after the new packs are written and before anything is deleted; may throw to stop
    // (for example when the exclusive lock was lost).
    std::function<void()> before_delete;
};

struct PruneResult {
    std::uint64_t packs = 0;           // packs before
    std::uint64_t packs_deleted = 0;   // with nothing in use (including leftovers of cancelled runs)
    std::uint64_t packs_repacked = 0;  // rewritten with the data still in use
    std::uint64_t packs_written = 0;   // new packs from repacking
    std::uint64_t blobs_used = 0;
    std::uint64_t blobs_unused = 0;
    std::uint64_t bytes_before = 0;    // stored size of all packs before
    std::uint64_t bytes_deleted = 0;   // stored size of the packs removed
    std::uint64_t bytes_written = 0;   // new packs and index files
};

// Removes data that no snapshot references. Reads every snapshot and folder listing first and
// stops without deleting anything if one cannot be read. Crash-safe: new packs and index files
// are written before anything is deleted. The repository object's in-memory index is out of date
// afterwards; open the repository again before using it for anything else.
PruneResult run_prune(Repository& repo, const PruneOptions& options);

// The packs in `indexed` that a listing of data/ (`listed`) did not show, after checking each one
// directly: a listing can come back short (a page lost, a server error read as its end), and a
// pack taken for missing is dropped from the index for good. Packs found by the direct check are
// added to `listed`. Shared by prune and repair.
std::vector<std::string> confirm_missing_packs(Backend& backend, const std::vector<std::string>& indexed,
                                               std::unordered_set<std::string>& listed);

// Whether `missing` of `indexed` packs gone is too many to be a pack lost here and there: more
// than one, and more than a tenth. Then the storage itself is more likely at fault (a wrong
// folder, a short listing) and prune and repair stop rather than drop that much data.
bool too_many_missing_packs(std::size_t missing, std::size_t indexed);

}  // namespace vaultik
