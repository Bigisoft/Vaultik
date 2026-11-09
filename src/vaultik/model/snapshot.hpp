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
    // Compared with the parent snapshot (all files are new without one). Known for snapshots made
    // by Vaultik 0.4 or later.
    bool changes_known = false;
    std::uint64_t files_new = 0;
    std::uint64_t files_changed = 0;
    std::uint64_t items_removed = 0;  // files and folders of the parent that are gone
};

// Tags starting with this are Vaultik's own (pin, note, change counts); they are stored as tags so
// older versions can still read the snapshot, and are not shown as tags.
constexpr const char* kReservedTagPrefix = "vaultik:";

struct Snapshot {
    std::uint64_t time = 0;  // FILETIME UTC
    std::string hostname;
    std::string username;
    std::vector<std::string> paths;
    Id tree{};
    std::optional<Id> parent;
    std::vector<std::string> tags;  // the user's tags, without the reserved ones
    SnapshotStats stats;
    bool pinned = false;  // never removed by retention or a size limit
    std::string note;     // the user's description, e.g. "before Windows update"
    // In a second copy: the id of the snapshot it was copied from (in the first repository).
    std::optional<Id> copy_of;
};

Bytes serialize_snapshot(const Snapshot& s);
Snapshot parse_snapshot(ByteSpan data);

}  // namespace vaultik
