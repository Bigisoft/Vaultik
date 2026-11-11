#pragma once

#include <cstdint>
#include <optional>
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

// An alternate data stream of a Windows file (NTFS "file.txt:name"), stored like file content.
struct DataStream {
    std::string name;  // without the colons and the ":$DATA" type
    std::uint64_t size = 0;
    std::vector<Id> content;
    bool operator==(const DataStream&) const = default;
};

// What a Windows symlink or junction (a Reparse node) points at, to bring the link back.
struct LinkInfo {
    std::uint32_t tag = 0;   // IO_REPARSE_TAG_SYMLINK (0xA000000C) or IO_REPARSE_TAG_MOUNT_POINT (0xA0000003)
    std::string target;      // as stored in the link ("..\x", "C:\Data", "\??\C:\Data")
    bool relative = false;   // a symlink with a relative target
    bool operator==(const LinkInfo&) const = default;
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
    // Windows extras (tree version 3). The security descriptor (owner, group and access list,
    // self-relative) is stored as a Data blob, so the many files that share one store it once.
    std::optional<Id> security;
    std::vector<DataStream> streams;
    std::optional<LinkInfo> link;  // Reparse nodes that are symlinks or junctions
    // Files with more than one name (hard links): how many it had when last read; 0 for one. A
    // Windows folder listing only shows the size and times as of the last change through that
    // name, so for these the backup asks the file itself before taking it for unchanged.
    std::uint32_t names = 0;
};

// Every Data blob a node needs: its content, its streams' content and its security descriptor.
template <class Fn>
void for_each_data_blob(const Node& n, Fn&& fn) {
    for (const Id& id : n.content) fn(id);
    for (const DataStream& s : n.streams)
        for (const Id& id : s.content) fn(id);
    if (n.security) fn(*n.security);
}

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
