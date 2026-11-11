#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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

// A stream of bytes a backup stores as one file, e.g. the output of "wsl --export".
class StreamSource {
public:
    virtual ~StreamSource() = default;
    // Up to `max` bytes; 0 at the end. Throws Error when the stream breaks off.
    virtual std::size_t read(std::uint8_t* dst, std::size_t max) = 0;
    // Called after the last read; throws Error when what was read is not complete (the program
    // that produced it failed), so the file is left out of the version.
    virtual void finish() {}
};

// Something a backup stores at a place of its own in the version instead of under its real path
// (see ops/virt.hpp: WSL distributions as "WSL/<name>.tar", Hyper-V machines as
// "Hyper-V/<name>/"). Either a stream, stored as a file, or a folder on disk, stored with its
// content as a folder named `place.back()`.
struct ExtraSource {
    std::vector<std::string> place;  // components in the version, e.g. {"WSL", "Ubuntu.tar"}
    std::string folder;              // a folder to read in place of `place`, or empty for:
    // Opens the stream; throws Error when it cannot start (the item is then skipped with a warning).
    std::function<std::unique_ptr<StreamSource>()> open;
};

struct BackupOptions {
    std::vector<std::string> paths;  // UTF-8 folders or files to back up
    // Stored next to the paths, at their own places (a stream is always read again: it has no
    // times to compare with the last version, and deduplication finds what is unchanged).
    std::vector<ExtraSource> extras;
    // Wildcard patterns (* ?). A pattern without a path separator matches a name; one with a
    // separator matches the full path. Case-insensitive on Windows.
    std::vector<std::string> excludes;
    std::vector<std::string> tags;
    // Parent snapshot for incremental change detection. When unset and use_parent is true, the
    // latest snapshot from this computer with the same paths is used.
    std::optional<Id> parent;
    bool use_parent = true;
    // Save no new snapshot when nothing changed since the parent (the same tree: every file,
    // folder and their metadata as before). The result then says so (unchanged).
    bool skip_unchanged = false;
    // Ask every file that looks unchanged for its real size and times instead of trusting the
    // folder listing (Windows): slower, but also catches a file that got a second name (hard
    // link) after the last backup and was changed through it. Files known to have several names
    // are always asked.
    bool check_every_file = false;

    // The change journal (Windows, with administrator rights; see platform/usn_journal.hpp): when
    // the parent snapshot is `journal_parent`, a folder for which `dir_unchanged(path)` is true had
    // nothing in it added, removed, renamed or changed since that snapshot, so its listing (and its
    // files, whose content is in the repository) is taken from the parent without reading the
    // folder. Its subfolders are asked in turn. Unset: every folder is read.
    std::optional<Id> journal_parent;
    std::function<bool(const std::string& dir)> dir_unchanged;
    // Asked of a folder `dir_unchanged` said no to: whether the folder itself has a record (it was
    // renamed, moved in, created or its attributes changed). Then nothing inside it is taken from
    // the parent: the parent version's folder of that name may have been another folder (two
    // folders swapping names write no records for what is in them). Unset: treated as no.
    std::function<bool(const std::string& dir)> dir_moved;

    // Where to read a path from (for example inside a shadow copy). Snapshot paths, excludes and
    // messages always use the real path. Unset: read the path itself.
    std::function<std::string(const std::string&)> read_path;

    // Files to read are opened, chunked and hashed on this many threads at once (0 or 1: on the
    // calling thread, one after the other). The repository is still only used from the calling
    // thread, which stores each file's chunks in listing order, so the snapshot is the same
    // either way. Each reader holds its own read buffer (twice the largest chunk, 8 MiB by
    // default), and at most 2 x read_threads files wait with up to 4 MiB of chunks each.
    unsigned read_threads = 0;

    std::function<void(const BackupProgress&)> on_progress;  // called at most ~5 times per second
    std::function<void(const std::string&)> on_warning;
    // Polled between files; returning true stops the backup with a Cancelled error.
    std::function<bool()> should_cancel;
    // Called just before the snapshot is saved; may throw to discard it (for example when the
    // repository lock was lost and data the snapshot relies on may have been pruned).
    std::function<void()> before_save;
    // The list of installed programs to keep with the version (see ops/apps.hpp: apps_to_json),
    // asked for once the files are stored; nothing keeps none. Errors are warnings.
    std::function<std::optional<std::string>()> app_list;
    // Keep the parent version's list instead, when it has one (a run soon after the last one: the
    // programs rarely change within hours, and winget takes a while). Used when app_list is unset.
    bool reuse_app_list = false;
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
    Id snapshot_id{};  // zero when `unchanged`
    Snapshot snapshot;
    bool unchanged = false;  // skip_unchanged and nothing changed: no snapshot was saved
    ChangeAlarm alarm;
    std::vector<std::string> missing_sources;  // backup paths that could not be read at all
    std::uint64_t dirs_reused = 0;             // folders taken from the parent through the change journal
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

// Whether a backup with these exclusions leaves out `path`: the path itself or a folder above it
// matches one of them (the same rules the backup applies to each folder and file it meets).
bool path_excluded(const std::vector<std::string>& excludes, const std::string& path);

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
