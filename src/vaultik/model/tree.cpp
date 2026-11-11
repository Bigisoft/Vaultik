#include "vaultik/model/tree.hpp"

#include <algorithm>

namespace vaultik {

namespace {

// Version 1: Windows only (no mode/owner). Version 2 adds mode, uid, gid and symlink targets.
// Version 3 adds a list of tagged extras to every node (Windows security descriptors, alternate
// data streams, link targets); it is only written when a node has one.
constexpr std::uint64_t kTreeVersion = 3;

// Extra tags (version 3). Unknown tags are skipped by readers.
constexpr std::uint8_t kExtraSecurity = 1;  // id (32): Data blob with the security descriptor
constexpr std::uint8_t kExtraStream = 2;    // name, varint size, varint count, ids
constexpr std::uint8_t kExtraLink = 3;      // u32 tag, u8 flags (1 = relative), target
constexpr std::uint8_t kExtraNames = 4;     // varint: number of names of a hard-linked file (> 1)

bool has_extras(const Node& n) {
    return n.security.has_value() || !n.streams.empty() || n.link.has_value() || n.names > 1;
}

void write_extras(ByteWriter& w, const Node& n) {
    std::vector<std::pair<std::uint8_t, Bytes>> extras;
    if (n.security) {
        ByteWriter e;
        e.raw(*n.security);
        extras.emplace_back(kExtraSecurity, e.take());
    }
    for (const DataStream& s : n.streams) {
        ByteWriter e;
        e.str(s.name);
        e.varint(s.size);
        e.varint(s.content.size());
        for (const Id& id : s.content) e.raw(id);
        extras.emplace_back(kExtraStream, e.take());
    }
    if (n.link) {
        ByteWriter e;
        e.u32(n.link->tag);
        e.u8(n.link->relative ? 1 : 0);
        e.str(n.link->target);
        extras.emplace_back(kExtraLink, e.take());
    }
    if (n.names > 1) {
        ByteWriter e;
        e.varint(n.names);
        extras.emplace_back(kExtraNames, e.take());
    }
    w.varint(extras.size());
    for (const auto& [tag, bytes] : extras) {
        w.u8(tag);
        w.varint(bytes.size());
        w.raw(bytes);
    }
}

void read_extras(ByteReader& r, Node& n) {
    const std::size_t count = r.count(2);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t tag = r.u8();
        const std::uint64_t len = r.varint();
        if (len > r.remaining()) throw Error("malformed tree: extra out of range");
        ByteReader e(r.raw(static_cast<std::size_t>(len)));
        switch (tag) {
            case kExtraSecurity: {
                Id id;
                e.raw_into(id);
                n.security = id;
                break;
            }
            case kExtraStream: {
                DataStream s;
                s.name = e.str();
                s.size = e.varint();
                s.content.resize(e.count(32));
                for (Id& id : s.content) e.raw_into(id);
                n.streams.push_back(std::move(s));
                break;
            }
            case kExtraLink: {
                LinkInfo l;
                l.tag = e.u32();
                l.relative = (e.u8() & 1) != 0;
                l.target = e.str();
                n.link = std::move(l);
                break;
            }
            case kExtraNames: {
                const std::uint64_t names = e.varint();
                n.names = names > 1 ? static_cast<std::uint32_t>(std::min<std::uint64_t>(names, 0xFFFFFFFFu)) : 0;
                break;
            }
            default:
                break;  // from a newer version: skipped
        }
    }
}

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
    // Version 2 when no node has extras, so such trees stay readable by older versions.
    const bool v3 = std::any_of(tree.nodes.begin(), tree.nodes.end(), has_extras);
    w.varint(v3 ? kTreeVersion : 2);
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
        if (v3) write_extras(w, n);
    }
    return w.take();
}

Tree parse_tree(ByteSpan data) {
    ByteReader r(data);
    const std::uint64_t version = r.varint();
    if (version < 1 || version > 3) throw Error("unsupported tree version");
    const bool v2 = version >= 2;
    const bool v3 = version >= 3;
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
        if (v3) read_extras(r, n);
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
#ifdef _WIN32
    // Names that are fine on Linux but that Windows programs cannot open or delete: a trailing dot
    // or space (the shell strips it), and the reserved device names, with or without an extension
    // ("con", "NUL.txt", "COM1", also "COM¹" which Windows treats the same way).
    if (name.back() == '.' || name.back() == ' ') return false;
    std::string base(name.substr(0, name.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (char& c : base)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$")
        return false;
    if (base.size() >= 4 && (base.starts_with("COM") || base.starts_with("LPT"))) {
        const std::string_view digit = std::string_view(base).substr(3);
        if ((digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') || digit == "\xC2\xB9" || digit == "\xC2\xB2" ||
            digit == "\xC2\xB3")
            return false;
    }
#endif
    return true;
}

}  // namespace vaultik
