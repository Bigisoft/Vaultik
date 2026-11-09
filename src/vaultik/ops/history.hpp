#pragma once

// Looking across versions: the versions of one file or folder, and a search by name through every
// snapshot. Paths are '/'-separated as `ls` and `restore --path` take them ("C/Users/me/report.docx").

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/prune.hpp"

namespace vaultik {

struct ItemVersion {
    std::size_t snapshot = 0;  // index into the snapshots passed in
    Node node;
    // Differs from the next older version that has the item, or is the oldest one.
    bool changed = false;
};

// The versions of one file or folder, newest first: one per snapshot that has it.
std::vector<ItemVersion> item_history(Repository& repo, const std::vector<SnapshotRecord>& newest_first,
                                      const std::string& path, const std::function<bool()>& cancelled = {});

struct FoundItem {
    std::string path;
    NodeKind kind = NodeKind::File;
    std::uint64_t size = 0;              // in the newest snapshot that has it
    std::vector<std::size_t> snapshots;  // the snapshots that have it, newest first
};

// Files and folders whose name matches `pattern` in any snapshot: * and ? wildcards, any case; a
// pattern without wildcards matches names containing it. Folder listings shared by snapshots are
// read once. At most `limit` items, sorted by path; `truncated` tells whether there were more.
std::vector<FoundItem> find_items(Repository& repo, const std::vector<SnapshotRecord>& newest_first,
                                  const std::string& pattern, std::size_t limit, bool* truncated = nullptr,
                                  const std::function<bool()>& cancelled = {});

// Whether `name` matches `pattern` as find_items does.
bool name_matches(const std::string& pattern, const std::string& name);

// Two versions of an item hold the same thing (content, folder listing or link target).
bool same_content(const Node& a, const Node& b);

}  // namespace vaultik
