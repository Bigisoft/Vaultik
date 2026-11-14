#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct VerifyOptions {
    // Also download every pack, check its hash and decrypt every blob. Without it, only pack
    // headers, indexes, snapshots and trees are checked.
    bool read_data = false;
    // With read_data, read only this share of the packs (0-1]; the others get the cheap header
    // check. Which packs are read follows `sample_seed`, so checks with different seeds (one per
    // week, say) cover different parts of the repository over time.
    double read_share = 1.0;
    std::uint64_t sample_seed = 0;
    // With read_data: a pack read whole and intact that has no repair data yet gets it, at this
    // percent (0: none added). Health checks pass the repository's setting, so data saved before
    // repair data was turned on gets it over time.
    unsigned add_parity = 0;
    // Stops the check early (throws Cancelled).
    std::function<bool()> should_cancel;

    // Resumable checks, with read_data. When given, this map holds when each pack was last read
    // whole and found intact (FILETIME ticks, kept on this computer, never in the repository),
    // and the check updates it as it goes, also when it stops early. Packs are then read oldest
    // first (never read ones before all others) instead of by the seed: a sampled check reads
    // the read_share of packs read longest ago, so over time every pack is read; a full check
    // (read_share 1) skips packs read within `fresh_ticks` of `now`, so a check that stopped
    // part way goes on where it stopped.
    std::unordered_map<Id, std::uint64_t, IdHash>* pack_state = nullptr;
    std::uint64_t now = 0;
    std::uint64_t fresh_ticks = 0;
    // With pack_state: no further pack is read once this much time went by (0: no limit), though
    // every check reads at least one; the packs left get the cheap header check and are counted
    // in VerifyResult::packs_left.
    std::chrono::milliseconds time_budget{0};

    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_error;
    // How far the reading is: the stored bytes of the packs read so far and of all packs this
    // check reads (both from the index, so `done` reaches `total` when every one was read).
    // Called once before the first pack and after each pack read.
    std::function<void(std::uint64_t done, std::uint64_t total)> on_bytes;
};

struct VerifyResult {
    std::uint64_t packs = 0;
    std::uint64_t blobs = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t trees = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t packs_read = 0;  // read in full (all of them without sampling)
    std::uint64_t errors = 0;
    std::uint64_t packs_repairable = 0;  // damaged, but their repair data can rebuild them
    std::uint64_t parity_added = 0;      // packs that got repair data (add_parity)
    // Packs in archive storage (Backend::data_needs_thaw): only checked to be there, neither their
    // header nor their data can be read without a thaw.
    std::uint64_t packs_archived = 0;
    // With pack_state: packs this check was to read but left for the next one (time_budget).
    std::uint64_t packs_left = 0;
};

VerifyResult run_verify(Repository& repo, const VerifyOptions& options);

}  // namespace vaultik
