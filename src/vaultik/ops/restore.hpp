#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct RestoreProgress {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::string current;
};

struct RestoreOptions {
    std::string target;  // UTF-8 folder to restore into (created if missing)
    // Snapshot path to restore, '/' or '\' separated, e.g. "C/Users/me/Documents". Empty = everything.
    std::string subpath;
    // With a folder as `subpath`: what is in it goes straight into `target` (Move to this PC puts
    // an old Documents folder's content into this account's Documents folder), not the folder.
    bool contents_only = false;
    bool overwrite = false;  // replace files that already exist in the target
    // Leave files in the target that are already identical (same size and modification time, and
    // the same content, read the way the backup stores it) as they are, counted in
    // RestoreResult::identical: an interrupted restore run again goes on where it stopped (a file
    // it left half written differs, and with `overwrite` is restored again).
    bool skip_identical = false;
    // Windows: apply each item's stored owner, group and access list. Off by default: restored
    // into another folder, another account's permissions could lock the user out of the files.
    bool permissions = false;
    // Windows: bring symlinks and junctions back as links (their targets are not followed).
    bool links = true;

    std::function<void(const RestoreProgress&)> on_progress;
    std::function<void(const std::string&)> on_warning;
    // Polled between files; returning true stops the restore with a Cancelled error.
    std::function<bool()> should_cancel;
};

struct RestoreResult {
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t skipped = 0;
    std::uint64_t identical = 0;  // already in the target as stored (skip_identical)
};

// Before a restore: how much it writes. `bytes` is the size of every file it restores, `needed`
// leaves out files already in the target with the same size (which are skipped, or with
// skip_identical left alone when they are the same).
struct RestoreEstimate {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::uint64_t needed = 0;
};
RestoreEstimate estimate_restore(Repository& repo, const Snapshot& snapshot, const RestoreOptions& options);

RestoreResult run_restore(Repository& repo, const Snapshot& snapshot, const RestoreOptions& options);

// Resolves a '/'-separated path inside a snapshot. Returns false when it does not exist. An empty
// path yields the root as a virtual directory node.
bool resolve_snapshot_path(Repository& repo, const Snapshot& snapshot, const std::string& path, Node& out);

// Where a backed-up path ends up below a restore target: target + its snapshot components.
std::string restored_path(const std::string& target, const std::string& snapshot_path);

}  // namespace vaultik
