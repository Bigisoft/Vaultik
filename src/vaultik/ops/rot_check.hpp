#pragma once

// Silent damage on this computer ("bit rot"): a health check also reads a few files back from
// this PC whose size and times are still as a version stored them, and compares their content
// with the stored one, chunk by chunk, hashed the way the backup hashes it. When the content
// differs although nothing wrote to the file (its size and times did not change), the drive
// changed the data by itself: a failing drive, bad memory or a bad cable. The backup then holds
// the earlier copy.
//
// The files are only opened for reading; nothing on this computer is changed.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct RotCheckOptions {
    std::size_t max_files = 20;                // files read and compared at most
    std::uint64_t max_bytes = 256ull << 20;    // and at most this many bytes together
    double max_seconds = 120;                  // stop after this long (a slow drive, a big file)
    std::uint64_t seed = 0;                    // which files are picked
    // Only files last written at least this long before the version was made (FILETIME ticks):
    // a file in use, such as a database written through memory mapping, can change without its
    // time changing at once. One day by default.
    std::uint64_t min_age = 864'000'000'000ull;
    // Also require the same ctime (Windows: creation time; POSIX: the time of the last change of
    // any kind, which no program can set back), and on Windows an NTFS change time (the same kind
    // of time) no later than the version. Tests on POSIX turn it off, since setting a file's
    // modification time back changes its ctime there.
    bool compare_ctime = true;
    // Where a snapshot path is on this computer; empty when it has no such place. Unset:
    // original_location (ops/restore_test.hpp).
    std::function<std::string(const std::string& snapshot_path)> local_path;
    std::function<bool()> should_cancel;
    std::function<void(const std::string&)> on_progress;
};

struct RotFinding {
    std::string path;           // on this computer
    std::string snapshot_path;  // in the version, '/' separated
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;     // the modification time both have
    std::uint64_t offset = 0;    // where the first differing chunk starts
};

struct RotCheckResult {
    std::uint64_t candidates = 0;  // files of the version that could be picked
    std::uint64_t checked = 0;     // files read and compared
    std::uint64_t bytes = 0;       // bytes read
    std::uint64_t changed = 0;     // picked, but changed since the version (or gone): not compared
    std::uint64_t unreadable = 0;  // picked, but locked or unreadable: skipped
    bool budget_reached = false;   // stopped by max_seconds
    std::vector<RotFinding> damaged;
};

// Checks a sample of the files of `snapshot` (a version made on this computer). Throws Cancelled
// when should_cancel says so, and Error when the version cannot be read.
RotCheckResult check_unchanged_files(Repository& repo, const Snapshot& snapshot, const RotCheckOptions& options);

}  // namespace vaultik
