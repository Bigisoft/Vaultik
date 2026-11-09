#pragma once

#include <cstdint>
#include <functional>
#include <string>

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
    // Stops the check early (throws Cancelled).
    std::function<bool()> should_cancel;

    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_error;
};

struct VerifyResult {
    std::uint64_t packs = 0;
    std::uint64_t blobs = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t trees = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t packs_read = 0;  // read in full (all of them without sampling)
    std::uint64_t errors = 0;
};

VerifyResult run_verify(Repository& repo, const VerifyOptions& options);

}  // namespace vaultik
