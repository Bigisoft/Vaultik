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

// Signs that files were encrypted by ransomware since the parent snapshot: changed files whose
// content went from compressible to random-looking (documents, databases, text; photos and videos
// are random-looking already and never count), and files replaced by the same name with an extra
// extension ("report.docx" -> "report.docx.locked").
struct ChangeAlarm {
    std::uint64_t parent_files = 0;  // files in the parent snapshot (0 without one)
    std::uint64_t became_random = 0;
    std::uint64_t renamed = 0;
    std::vector<std::string> examples;  // a few affected paths

    std::uint64_t suspicious() const { return became_random + renamed; }
    // At least 20 such files, and at least 5% of the parent's files.
    bool raised() const { return suspicious() >= 20 && suspicious() * 20 >= parent_files; }
};

struct BackupResult {
    Id snapshot_id{};
    Snapshot snapshot;
    ChangeAlarm alarm;
    std::vector<std::string> missing_sources;  // backup paths that could not be read at all
};

// What a backup of `paths` would read: files, folders and bytes, after the exclusions, walked as a
// backup walks them (links and placeholders are not followed). Folders that cannot be listed are
// counted in `unreadable`. `progress` is called now and then with the totals so far.
struct SourceEstimate {
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t bytes = 0;
    std::uint64_t unreadable = 0;
};
SourceEstimate estimate_sources(const std::vector<std::string>& paths, const std::vector<std::string>& excludes,
                                const std::function<bool()>& cancelled = {},
                                const std::function<void(const SourceEstimate&)>& progress = {});

// Shannon entropy of the bytes, in bits per byte (0-8); encrypted or compressed data is near 8.
double byte_entropy(ByteSpan data);

// Thrown when a should_cancel callback returns true. No snapshot is saved; data packs already
// written stay in the repository unreferenced until a prune removes them.
class Cancelled : public Error {
public:
    Cancelled() : Error("cancelled") {}
};

BackupResult run_backup(Repository& repo, const BackupOptions& options);

}  // namespace vaultik
