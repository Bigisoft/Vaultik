#pragma once

// Retention: forget removes snapshots by a keep policy, prune removes the data no snapshot uses
// any more. Both need an exclusive repository lock (RepoLock::Mode::Exclusive).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

// How many snapshots to keep. Each rule keeps the newest snapshot of each of the last N hours,
// days, weeks (Monday to Sunday), months or years that have one, in local time; `last` keeps the
// N newest. A snapshot kept by any rule stays. All zero means "keep everything".
struct RetentionPolicy {
    int last = 0;
    int hourly = 0;
    int daily = 0;
    int weekly = 0;
    int monthly = 0;
    int yearly = 0;

    bool empty() const { return last <= 0 && hourly <= 0 && daily <= 0 && weekly <= 0 && monthly <= 0 && yearly <= 0; }
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
    bool dry_run = false;
};

struct ForgetResult {
    std::vector<ForgetDecision> decisions;
    std::size_t removed = 0;
};

// Throws Error for an empty policy rather than removing everything.
ForgetResult run_forget(Repository& repo, const ForgetOptions& options);

struct PruneOptions {
    bool dry_run = false;
    // A pack whose unused share is above this is rewritten with only the data still in use.
    double repack_unused = 0.25;
    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_warning;
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

}  // namespace vaultik
