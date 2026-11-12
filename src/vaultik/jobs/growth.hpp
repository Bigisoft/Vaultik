#pragma once

// "What grew": which folders added the most new data to a backup over a period, from the versions'
// folder listings alone (no file data is read). A chunk counts as new in the first version of the
// period that has it when the versions before the period did not; its stored size (compressed and
// encrypted, from the index) goes to the folder of the file that brought it. The folders are
// reported as specific as they can be while still holding a share of the growth, and a folder that
// one of the ready-made exclusions (or a well-known folder such as Downloads) covers comes with a
// suggestion to leave it out from now on.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik::jobs {

struct GrowthOptions {
    std::uint64_t since = 0;  // FILETIME ticks; versions made from then on count
    std::uint64_t until = 0;  // and before this (0: no end)
    // Which versions are the job's (the others are only taken into account for what is new). Unset:
    // every version.
    std::function<bool(const Snapshot&)> select;
    // The job's exclusions: ready-made ones it uses already, and folders it leaves out already, are
    // not suggested.
    std::vector<std::string> excludes;
    std::size_t top = 15;      // folders reported at most
    double min_share = 0.02;   // a folder is split up further while parts of it hold this share
    // Where a snapshot path is on this computer (for the exclusions); unset: original_location.
    std::function<std::string(const std::string& snapshot_path)> local_path;
    std::function<bool()> should_cancel;
    std::function<void(const std::string&)> on_progress;
};

// A way to leave a folder out of the job from now on.
struct GrowthSuggestion {
    std::string preset;                 // a ready-made exclusion's id; empty for the folder itself
    std::vector<std::string> patterns;  // what to add to the job's exclusions
};

struct FolderGrowth {
    std::string path;           // on this computer (or the snapshot path when it has no such place)
    std::string snapshot_path;  // '/' separated
    bool rest = false;          // only what is directly in it and in its smaller subfolders
    std::uint64_t stored = 0;   // new stored bytes
    std::uint64_t raw = 0;      // the same before compression
    std::uint64_t files = 0;    // files that brought new data
    std::optional<GrowthSuggestion> suggestion;
};

// New data that a ready-made exclusion the job does not use would have left out.
struct PresetGrowth {
    std::string id;
    std::vector<std::string> patterns;
    std::uint64_t stored = 0;
    std::uint64_t files = 0;
};

struct GrowthReport {
    std::uint64_t since = 0;
    std::uint64_t until = 0;
    std::uint64_t versions = 0;  // the job's versions in the period
    std::uint64_t stored = 0;    // new stored bytes in all
    std::uint64_t raw = 0;
    std::uint64_t files = 0;
    std::vector<FolderGrowth> folders;  // largest first
    std::vector<PresetGrowth> presets;  // largest first, only those with new data
};

// `snapshots`: every version of the repository (any order). Before the period, the newest version
// of each computer and set of folders stands for what was stored already. Throws Cancelled when
// should_cancel says so, Error when a folder listing cannot be read.
GrowthReport what_grew(Repository& repo, const std::vector<std::pair<Id, Snapshot>>& snapshots,
                       const GrowthOptions& options);

// Suggestions for a folder on this computer (`path`), or nothing: a ready-made exclusion the job
// does not use that leaves it out, else a well-known folder of downloads, temporary files, caches
// or build output, left out by its full path. Nothing when `excludes` leave it out already.
std::optional<GrowthSuggestion> suggest_exclusion(const std::string& path, const std::vector<std::string>& excludes);

std::string to_json(const GrowthReport& report);

}  // namespace vaultik::jobs
