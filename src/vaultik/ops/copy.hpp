#pragma once

// A second copy of a backup (3-2-1): copies snapshots from one repository into another, for
// example from a USB drive to cloud storage. The two have their own keys, so every chunk and
// folder listing is decrypted and stored again under the target's key; data the target already
// has is not stored twice. A copy remembers its original (Snapshot::copy_of), so each snapshot is
// copied once.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct CopyOptions {
    std::vector<std::string> tags;  // only snapshots with all of these, e.g. "job:documents"
    // A file remembering which stored item became which in the target, so later copies only read
    // what is new. Optional; entries are checked against the target before use.
    std::string map_file;
    std::function<bool()> should_cancel;
    std::function<void(const std::string&)> on_progress;  // "version 2 of 5: C/Users/me/x.txt"
};

struct CopyResult {
    std::size_t copied = 0;   // snapshots copied now
    std::size_t present = 0;  // already in the target
    std::uint64_t blobs = 0;  // chunks and listings stored in the target now
    std::uint64_t bytes = 0;  // their size before compression
};

// Copies the snapshots the target does not have yet, oldest first. Hold a shared lock on both.
// Throws Error (Cancelled on cancel); snapshots copied before that stay.
CopyResult copy_snapshots(Repository& from, Repository& to, const CopyOptions& options);

}  // namespace vaultik
