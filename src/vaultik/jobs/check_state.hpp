#pragma once

// What the health checks of a repository read, kept on this computer (never in the repository):
// when each pack was last read whole and found intact. With it a check reads the packs read
// longest ago first, so the weekly share covers the whole backup over time, and a full check that
// stopped part way (its time limit, a cancel, an unplugged drive) goes on where it stopped.
//
// One file per repository, <settings dir>/checks/<repository id>.json, so jobs that back up to the
// same repository share it.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik::jobs {

using PackTimes = std::unordered_map<Id, std::uint64_t, IdHash>;

// The file's name part for a repository (its id, in hex).
std::string check_state_key(const Repository& repo);
// Empty when there is none yet or it cannot be read (the check then starts from scratch).
PackTimes load_check_state(const std::string& settings_dir, const std::string& key);
// Written atomically. Packs no longer in `repo`'s index (removed by a prune) are left out.
void save_check_state(const std::string& settings_dir, const std::string& key, const PackTimes& times,
                      const Repository& repo);

// How many of the repository's packs were not read within `period` ticks of `now`.
std::uint64_t packs_due(const PackTimes& times, const Repository& repo, std::uint64_t now, std::uint64_t period);

}  // namespace vaultik::jobs
