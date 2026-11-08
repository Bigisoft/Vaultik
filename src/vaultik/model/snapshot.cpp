#include "vaultik/model/snapshot.hpp"

namespace vaultik {

namespace {

constexpr std::uint32_t kSnapshotVersion = 1;

}  // namespace

Bytes serialize_snapshot(const Snapshot& s) {
    ByteWriter w;
    w.u32(kSnapshotVersion);
    w.u64(s.time);
    w.str(s.hostname);
    w.str(s.username);
    w.varint(s.paths.size());
    for (const auto& p : s.paths) w.str(p);
    w.raw(s.tree);
    w.u8(s.parent ? 1 : 0);
    if (s.parent) w.raw(*s.parent);
    w.varint(s.tags.size());
    for (const auto& t : s.tags) w.str(t);
    w.varint(s.stats.files);
    w.varint(s.stats.dirs);
    w.varint(s.stats.bytes);
    w.varint(s.stats.new_bytes);
    w.varint(s.stats.stored_bytes);
    w.varint(s.stats.errors);
    w.varint(s.stats.skipped);
    return w.take();
}

Snapshot parse_snapshot(ByteSpan data) {
    ByteReader r(data);
    if (r.u32() != kSnapshotVersion) throw Error("unsupported snapshot version");
    Snapshot s;
    s.time = r.u64();
    s.hostname = r.str();
    s.username = r.str();
    const std::size_t paths = r.count(1);
    for (std::size_t i = 0; i < paths; ++i) s.paths.push_back(r.str());
    r.raw_into(s.tree);
    const std::uint8_t has_parent = r.u8();
    if (has_parent > 1) throw Error("malformed snapshot");
    if (has_parent == 1) {
        Id parent;
        r.raw_into(parent);
        s.parent = parent;
    }
    const std::size_t tags = r.count(1);
    for (std::size_t i = 0; i < tags; ++i) s.tags.push_back(r.str());
    s.stats.files = r.varint();
    s.stats.dirs = r.varint();
    s.stats.bytes = r.varint();
    s.stats.new_bytes = r.varint();
    s.stats.stored_bytes = r.varint();
    s.stats.errors = r.varint();
    s.stats.skipped = r.varint();
    r.expect_done();
    return s;
}

}  // namespace vaultik
