#include "vaultik/model/snapshot.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

constexpr std::uint32_t kSnapshotVersion = 1;

}  // namespace

namespace {

constexpr std::string_view kPinnedTag = "vaultik:pinned";
constexpr std::string_view kNoteTag = "vaultik:note=";
constexpr std::string_view kChangesTag = "vaultik:changes=";
constexpr std::string_view kCopyOfTag = "vaultik:copy-of=";
constexpr std::string_view kAppsTag = "vaultik:apps=";
constexpr std::string_view kImageTag = "vaultik:image";
constexpr std::string_view kCloudOnlyTag = "vaultik:cloud-only=";

std::string id_tag(std::string_view prefix, const Id& id) {
    return std::string(prefix) + to_hex(ByteSpan(id.data(), id.size()));
}

// An id written by id_tag; nothing for a damaged tag.
std::optional<Id> tag_id(const std::string& tag, std::string_view prefix) {
    try {
        const Bytes raw = from_hex(tag.substr(prefix.size()));
        if (raw.size() != sizeof(Id)) return std::nullopt;
        Id id{};
        std::copy(raw.begin(), raw.end(), id.begin());
        return id;
    } catch (const Error&) {
        return std::nullopt;
    }
}

// The user's tags plus the reserved ones that carry the newer fields.
std::vector<std::string> all_tags(const Snapshot& s) {
    std::vector<std::string> tags;
    for (const std::string& t : s.tags)
        if (!t.starts_with(kReservedTagPrefix)) tags.push_back(t);
    if (s.pinned) tags.emplace_back(kPinnedTag);
    if (!s.note.empty()) tags.push_back(std::string(kNoteTag) + s.note);
    if (s.copy_of) tags.push_back(id_tag(kCopyOfTag, *s.copy_of));
    if (s.apps) tags.push_back(id_tag(kAppsTag, *s.apps));
    if (s.image) tags.emplace_back(kImageTag);
    if (s.stats.cloud_only > 0)
        tags.push_back(std::string(kCloudOnlyTag) + std::to_string(s.stats.cloud_only) + "," +
                       std::to_string(s.stats.cloud_only_bytes));
    if (s.stats.changes_known) {
        tags.push_back(std::string(kChangesTag) + std::to_string(s.stats.files_new) + "," +
                       std::to_string(s.stats.files_changed) + "," + std::to_string(s.stats.items_removed));
    }
    return tags;
}

void read_reserved(Snapshot& s, const std::string& tag) {
    if (tag == kPinnedTag) {
        s.pinned = true;
    } else if (tag == kImageTag) {
        s.image = true;
    } else if (tag.starts_with(kNoteTag)) {
        s.note = tag.substr(kNoteTag.size());
    } else if (tag.starts_with(kCopyOfTag)) {
        s.copy_of = tag_id(tag, kCopyOfTag);  // a damaged tag: the snapshot is then simply not known as a copy
    } else if (tag.starts_with(kAppsTag)) {
        s.apps = tag_id(tag, kAppsTag);
    } else if (tag.starts_with(kCloudOnlyTag)) {
        // "<files>,<bytes>"
        const char* p = tag.c_str() + kCloudOnlyTag.size();
        char* end = nullptr;
        const std::uint64_t files = std::strtoull(p, &end, 10);
        if (end != p && *end == ',') {
            const char* q = end + 1;
            const std::uint64_t bytes = std::strtoull(q, &end, 10);
            if (end != q && *end == '\0') {
                s.stats.cloud_only = files;
                s.stats.cloud_only_bytes = bytes;
            }
        }
    } else if (tag.starts_with(kChangesTag)) {
        // "<new>,<changed>,<removed>"
        const char* p = tag.c_str() + kChangesTag.size();
        std::uint64_t v[3] = {0, 0, 0};
        int got = 0;
        for (; got < 3; ++got) {
            char* end = nullptr;
            v[got] = std::strtoull(p, &end, 10);
            if (end == p) break;
            p = end;
            if (got < 2) {
                if (*p != ',') break;
                ++p;
            }
        }
        if (got == 3 && *p == '\0') {
            s.stats.changes_known = true;
            s.stats.files_new = v[0];
            s.stats.files_changed = v[1];
            s.stats.items_removed = v[2];
        }
    }
    // Unknown reserved tags come from a newer version: ignored.
}

}  // namespace

Bytes serialize_snapshot(const Snapshot& s) {
    const std::vector<std::string> tags = all_tags(s);
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
    w.varint(tags.size());
    for (const auto& t : tags) w.str(t);
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
    for (std::size_t i = 0; i < tags; ++i) {
        std::string tag = r.str();
        if (tag.starts_with(kReservedTagPrefix)) read_reserved(s, tag);
        else s.tags.push_back(std::move(tag));
    }
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
