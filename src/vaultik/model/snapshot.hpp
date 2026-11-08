#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik {

struct SnapshotStats {
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t bytes = 0;         // total size of all files in the snapshot
    std::uint64_t new_bytes = 0;     // uncompressed bytes of chunks that were not in the repository yet
    std::uint64_t stored_bytes = 0;  // bytes added to the repository (compressed + encrypted)
    std::uint64_t errors = 0;        // files or folders that could not be read
    std::uint64_t skipped = 0;       // reparse points and cloud placeholders stored without content
};

struct Snapshot {
    std::uint64_t time = 0;  // FILETIME UTC
    std::string hostname;
    std::string username;
    std::vector<std::string> paths;
    Id tree{};
    std::optional<Id> parent;
    std::vector<std::string> tags;
    SnapshotStats stats;
};

Bytes serialize_snapshot(const Snapshot& s);
Snapshot parse_snapshot(ByteSpan data);

}  // namespace vaultik
