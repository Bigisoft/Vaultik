#pragma once

// What changed between two snapshots: files and folders added, removed or changed. Paths are
// '/'-separated as `ls` shows them ("C/Users/me/report.docx").

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct Change {
    enum class Kind { Added, Removed, Changed };
    Kind kind = Kind::Changed;
    std::string path;
    NodeKind node_kind = NodeKind::File;  // of the newer item (the older one for Removed)
    std::uint64_t old_size = 0;           // files only
    std::uint64_t new_size = 0;
    Node old_node;  // empty for Added
    Node new_node;  // empty for Removed
};

struct DiffResult {
    std::vector<Change> changes;  // sorted by path
    std::uint64_t added = 0;      // counted over everything, also beyond `limit`
    std::uint64_t removed = 0;
    std::uint64_t changed = 0;
    bool truncated = false;  // more changes than `limit`
};

// Compares `older` with `newer`. A folder only on one side is one change (its contents are not
// listed); a folder with the same listing in both is skipped without reading it. Only content
// counts as a change, not times or attributes.
DiffResult diff_snapshots(Repository& repo, const Snapshot& older, const Snapshot& newer, std::size_t limit = 100000,
                          const std::function<bool()>& cancelled = {});

}  // namespace vaultik
