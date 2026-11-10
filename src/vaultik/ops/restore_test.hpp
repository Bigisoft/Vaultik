#pragma once

// A practice restore: proves that files come back from a backup, end to end, not only that the
// stored data reads back. Restores a few files chosen at random from a version into a temporary
// folder, checks each one, and removes the folder again.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct RestoreTestOptions {
    std::size_t max_files = 20;
    std::uint64_t max_bytes = 500ull << 20;  // the files restored add up to at most this
    // A folder only this account (or, for an elevated run, only administrators) can change. A
    // fresh subfolder is made in it and removed afterwards.
    std::string work_dir;
    std::uint64_t seed = 0;  // which files are picked
    std::function<bool()> should_cancel;
    std::function<void(const std::string&)> on_progress;
};

struct RestoreTestResult {
    std::uint64_t candidates = 0;  // files in the version small enough to be picked
    std::uint64_t files = 0;       // files restored and checked
    std::uint64_t bytes = 0;
    std::uint64_t compared = 0;    // also compared byte for byte with the unchanged file on this PC
    std::vector<std::string> problems;  // one line each: "C/Users/me/a.txt: ..."
};

// Each picked file is restored on its own and must come back whole: every chunk decrypted and
// matching its hash (the restore checks that), the size as stored, and, when the file on this
// computer still has the stored size and modification time, the same bytes as that file.
// Throws Cancelled when should_cancel says so, and Error when the version cannot be read at all.
RestoreTestResult test_restore(Repository& repo, const Snapshot& snapshot, const RestoreTestOptions& options);

// Where a snapshot path ("C/Users/me/a.txt", "UNC/server/share/x") is on this computer
// ("C:\Users\me\a.txt"); empty when it has no such place.
std::string original_location(const std::string& snapshot_path);

}  // namespace vaultik
