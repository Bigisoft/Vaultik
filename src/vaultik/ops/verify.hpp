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

    std::function<void(const std::string&)> on_progress;
    std::function<void(const std::string&)> on_error;
};

struct VerifyResult {
    std::uint64_t packs = 0;
    std::uint64_t blobs = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t trees = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t errors = 0;
};

VerifyResult run_verify(Repository& repo, const VerifyOptions& options);

}  // namespace vaultik
