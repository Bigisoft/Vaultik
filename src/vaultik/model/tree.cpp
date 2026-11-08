#include "vaultik/model/tree.hpp"

#include <algorithm>

namespace vaultik {

namespace {

// Version 1: Windows only (no mode/owner). Version 2 adds mode, uid, gid and symlink targets.
constexpr std::uint64_t kTreeVersion = 2;

// Smallest possible node (a metadata-only node with a one-byte empty name length).
constexpr std::size_t kMinNodeSizeV1 = 1 + 1 + 4 + 8 * 3;
constexpr std::size_t kMinNodeSizeV2 = 1 + 1 + 4 + 4 * 3 + 8 * 3;

}  // namespace

void Tree::sort() {
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
}

const Node* Tree::find(std::string_view name) const {
    auto it = std::lower_bound(nodes.begin(), nodes.end(), name,
                               [](const Node& n, std::string_view key) { return n.name < key; });
    if (it == nodes.end() || it->name != name) return nullptr;
    return &*it;
}

Bytes serialize_tree(const Tree& tree) {
    ByteWriter w;
    w.varint(kTreeVersion);
    w.varint(tree.nodes.size());
    for (const Node& n : tree.nodes) {
        w.str(n.name);
        w.u8(static_cast<std::uint8_t>(n.kind));
        w.u32(n.attributes);
        w.u32(n.mode);
        w.u32(n.uid);
        w.u32(n.gid);
        w.u64(n.ctime);
        w.u64(n.mtime);
        w.u64(n.size);
        switch (n.kind) {
            case NodeKind::File:
                w.varint(n.content.size());
                for (const Id& id : n.content) w.raw(id);
                break;
            case NodeKind::Dir:
                w.raw(n.subtree);
                break;
            case NodeKind::Symlink:
                w.str(n.link_target);
                break;
            case NodeKind::Reparse:
                break;
        }
    }
    return w.take();
}

Tree parse_tree(ByteSpan data) {
    ByteReader r(data);
    const std::uint64_t version = r.varint();
    if (version != 1 && version != 2) throw Error("unsupported tree version");
    const bool v2 = version == 2;
    Tree tree;
    const std::size_t count = r.count(v2 ? kMinNodeSizeV2 : kMinNodeSizeV1);
    tree.nodes.reserve(count);
    const auto max_kind = static_cast<std::uint8_t>(v2 ? NodeKind::Symlink : NodeKind::Reparse);
    for (std::size_t i = 0; i < count; ++i) {
        Node n;
        n.name = r.str();
        const std::uint8_t kind = r.u8();
        if (kind > max_kind) throw Error("malformed tree: unknown node kind");
        n.kind = static_cast<NodeKind>(kind);
        n.attributes = r.u32();
        if (v2) {
            n.mode = r.u32();
            n.uid = r.u32();
            n.gid = r.u32();
        }
        n.ctime = r.u64();
        n.mtime = r.u64();
        n.size = r.u64();
        switch (n.kind) {
            case NodeKind::File: {
                const std::size_t chunks = r.count(32);
                n.content.resize(chunks);
                for (Id& id : n.content) r.raw_into(id);
                break;
            }
            case NodeKind::Dir:
                r.raw_into(n.subtree);
                break;
            case NodeKind::Symlink:
                n.link_target = r.str();
                break;
            case NodeKind::Reparse:
                break;
        }
        if (!tree.nodes.empty() && !(tree.nodes.back().name < n.name))
            throw Error("malformed tree: entries not sorted or duplicated");
        tree.nodes.push_back(std::move(n));
    }
    r.expect_done();
    return tree;
}

bool is_safe_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u == 0 || c == '/') return false;
#ifdef _WIN32
        if (u < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
#endif
    }
    return true;
}

}  // namespace vaultik
