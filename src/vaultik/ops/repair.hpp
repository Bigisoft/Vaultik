#pragma once

// Repairs a damaged repository, so whatever is still intact can be restored and later backups
// work again. Open the repository with Repository::open(..., load_index = false), since a
// damaged index file would stop a normal open, and hold the exclusive lock.
//
//   1. Read every pack's own header (and, unless `quick`, decrypt and check every blob in it)
//      and build the index from what is intact; packs that cannot be read stay where they are.
//   2. Write fresh index files for it, then delete all the old ones.
//   3. Check every version: one whose manifest cannot be read is deleted (it cannot be opened
//      anyway). In the others, files whose data is gone and folders whose listing is gone are
//      left out: the version is saved again without them (its note says so) and the old manifest
//      deleted. The next backup stores those files again when they are still on the computer.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct RepairOptions {
    bool quick = false;    // trust the pack headers; do not read every blob
    bool dry_run = false;  // report what would be done, change nothing
    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_warning;  // each problem found, as it is found
    std::function<bool()> should_cancel;                 // checked while reading (before any change)
};

struct RepairResult {
    std::uint64_t packs = 0;
    std::uint64_t packs_unreadable = 0;
    std::uint64_t packs_rebuilt = 0;  // damaged, but rebuilt whole from their repair data
    std::uint64_t blobs = 0;          // intact blobs indexed
    std::uint64_t blobs_damaged = 0;  // blobs that failed their check (left out of the index)
    std::uint64_t index_files_replaced = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t snapshots_removed = 0;   // unreadable, or nothing in them survived
    std::uint64_t snapshots_repaired = 0;  // saved again without what was lost
    std::uint64_t files_lost = 0;          // left out of the repaired versions (counted per version)
    std::uint64_t folders_lost = 0;
    bool changed() const {
        return blobs_damaged > 0 || packs_unreadable > 0 || packs_rebuilt > 0 || snapshots_removed > 0 ||
               snapshots_repaired > 0;
    }
};

RepairResult run_repair(Repository& repo, const RepairOptions& options);

}  // namespace vaultik
