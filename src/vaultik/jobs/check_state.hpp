#pragma once

// What the health checks of a repository read, kept on this computer (never in the repository):
// when each pack was last read whole and found intact. With it a check reads the packs read
// longest ago first, so the weekly share covers the whole backup over time, and a full check that
// stopped part way (its time limit, a cancel, an unplugged drive) goes on where it stopped.
//
// One file per repository and place, <settings dir>/checks/<key>.json (see check_state_key), so
// jobs that back up to the same repository share it, and a copy of a repository elsewhere (on
// another drive, in another folder) starts from scratch: what was read from the original says
// nothing about the copy's own sectors.
//
// The monthly full check goes in cycles: a scheduled one starts a cycle when none is going on,
// reads (within its time limit) the packs not read since the cycle started, and the next ones go
// on until every pack was read since then; then the cycle is complete. So every pack is read once
// per cycle, however the reads of the cycles before were spread out. A full check started by the
// user reads every pack.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik::jobs {

using PackTimes = std::unordered_map<Id, std::uint64_t, IdHash>;

using PackSet = std::unordered_set<Id, IdHash>;

struct CheckState {
    PackTimes packs;  // when each pack was last read whole and found intact (FILETIME ticks)
    // When each pack was last checked and found damaged, or could not be checked
    // (VerifyOptions::damaged_state): visited for the cycle, which a damaged pack would otherwise
    // keep open for good.
    PackTimes damaged;
    // Packs found in archive storage although the job does not say it keeps data there
    // (VerifyOptions::archived_state): a check cannot read them.
    PackSet archived;
    // When the scheduled full check's cycle going on started (0: none).
    std::uint64_t cycle_started = 0;
};

// The file's name part for a repository where it is now: from its id and its place (the drive's
// identity and the folder on it, or the cloud location), in hex.
std::string check_state_key(Repository& repo);
std::string check_state_key(const Repository& repo, const std::string& location);
// Empty when there is none yet or it cannot be read (the check then starts from scratch).
CheckState load_check_state(const std::string& settings_dir, const std::string& key);
// Written atomically. Packs no longer in `repo`'s index (removed by a prune) are left out. The
// file an earlier version kept under the repository's id alone (checks/<repo id>.json) is
// removed: it may belong to a copy elsewhere, and is not used any more.
void save_check_state(const std::string& settings_dir, const std::string& key, const CheckState& state,
                      const Repository& repo);

// How many of the repository's packs that can be read (not in archive storage) were not read
// whole at or after `since` (a time after `now`, as a clock set back leaves, counts as never).
std::uint64_t packs_unread_since(const PackTimes& times, const Repository& repo, std::uint64_t since, std::uint64_t now);
// The same for a check's state: a pack found damaged at or after `since` counts as visited, and
// one found in archive storage (CheckState::archived) as one that cannot be read.
std::uint64_t packs_unread_since(const CheckState& state, const Repository& repo, std::uint64_t since,
                                 std::uint64_t now);

// A full check about to start: `resumable` for a scheduled one (or one asked to keep to the time
// limit), which goes on with the cycle going on or starts one now. Returns since when packs count
// as read for this check (VerifyOptions::fresh_since; 0: every pack is read).
std::uint64_t plan_full_check(CheckState& state, bool resumable, std::uint64_t now);
// After a full check (also one that stopped part way): how many packs are still to read (`since`
// as plan_full_check returned, or for a check that read everything its own start), closing the
// cycle once none is left (see packs_unread_since for the state: damaged and archived packs do
// not keep it open).
std::uint64_t finish_full_check(CheckState& state, const Repository& repo, std::uint64_t since, std::uint64_t now);

}  // namespace vaultik::jobs
