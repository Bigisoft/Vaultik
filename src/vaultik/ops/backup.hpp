#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct BackupProgress {
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t bytes = 0;      // bytes of files processed (read or reused)
    std::uint64_t new_bytes = 0;  // bytes that were not in the repository yet
    std::string current;          // file being read
};

struct BackupOptions {
    std::vector<std::string> paths;  // UTF-8 folders or files to back up
    // Wildcard patterns (* ?). A pattern without a path separator matches a name; one with a
    // separator matches the full path. Case-insensitive on Windows.
    std::vector<std::string> excludes;
    std::vector<std::string> tags;
    // Parent snapshot for incremental change detection. When unset and use_parent is true, the
    // latest snapshot from this computer with the same paths is used.
    std::optional<Id> parent;
    bool use_parent = true;

    // Where to read a path from (for example inside a shadow copy). Snapshot paths, excludes and
    // messages always use the real path. Unset: read the path itself.
    std::function<std::string(const std::string&)> read_path;

    std::function<void(const BackupProgress&)> on_progress;  // called at most ~5 times per second
    std::function<void(const std::string&)> on_warning;
    // Polled between files; returning true stops the backup with a Cancelled error.
    std::function<bool()> should_cancel;
    // Called just before the snapshot is saved; may throw to discard it (for example when the
    // repository lock was lost and data the snapshot relies on may have been pruned).
    std::function<void()> before_save;
};

struct BackupResult {
    Id snapshot_id{};
    Snapshot snapshot;
};

// Thrown when a should_cancel callback returns true. No snapshot is saved; data packs already
// written stay in the repository unreferenced until a prune removes them.
class Cancelled : public Error {
public:
    Cancelled() : Error("cancelled") {}
};

BackupResult run_backup(Repository& repo, const BackupOptions& options);

}  // namespace vaultik
