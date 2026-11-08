#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik {

enum class NodeKind : std::uint8_t {
    File = 0,
    Dir = 1,
    // Metadata only, no content: Windows symlinks, junctions and cloud placeholders, POSIX
    // devices, sockets and pipes.
    Reparse = 2,
    Symlink = 3,  // POSIX symbolic link; link_target holds the target
};

struct Node {
    std::string name;  // UTF-8, single path component
    NodeKind kind = NodeKind::File;
    std::uint32_t attributes = 0;  // Windows FILE_ATTRIBUTE_* bits (0 from POSIX)
    std::uint32_t mode = 0;        // POSIX permission bits 07777 (0 from Windows)
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint64_t ctime = 0;  // Windows: creation time. POSIX: status change time
    std::uint64_t mtime = 0;
    // No access time: reading a file changes it, which would make every folder listing differ
    // between backups and defeat deduplication of unchanged trees.
    std::uint64_t size = 0;
    std::vector<Id> content;  // File: chunk ids in order
    Id subtree{};             // Dir: tree blob id
    std::string link_target;  // Symlink
};

struct Tree {
    std::vector<Node> nodes;  // sorted by name (bytewise)

    void sort();
    // Binary search; nodes must be sorted.
    const Node* find(std::string_view name) const;
};

Bytes serialize_tree(const Tree& tree);
Tree parse_tree(ByteSpan data);

// True if `name` is safe to use as a single path component when restoring on this platform.
bool is_safe_name(std::string_view name);

}  // namespace vaultik
