#include "vaultik/repo/index.hpp"

namespace vaultik {

namespace {

constexpr std::uint32_t kIndexVersion = 1;

}  // namespace

std::uint32_t Index::add_pack(const Id& pack_id) {
    auto [it, inserted] = pack_pos_.try_emplace(pack_id, static_cast<std::uint32_t>(packs_.size()));
    if (inserted) packs_.push_back(pack_id);
    return it->second;
}

void Index::add(const BlobEntry& entry, std::uint32_t pack) {
    BlobLocation loc;
    loc.pack = pack;
    loc.type = entry.type;
    loc.offset = entry.offset;
    loc.length = entry.length;
    loc.raw_len = entry.raw_len;
    blobs_.try_emplace(entry.id, loc);  // a duplicate copy in another pack is harmless; keep the first
}

const BlobLocation* Index::find(const Id& blob_id) const {
    auto it = blobs_.find(blob_id);
    return it == blobs_.end() ? nullptr : &it->second;
}

Bytes serialize_index_file(const IndexFileContent& content) {
    ByteWriter w;
    w.u32(kIndexVersion);
    w.varint(content.size());
    for (const auto& [pack_id, entries] : content) {
        w.raw(pack_id);
        w.varint(entries.size());
        for (const BlobEntry& e : entries) write_blob_entry(w, e);
    }
    return w.take();
}

IndexFileContent parse_index_file(ByteSpan data) {
    ByteReader r(data);
    if (r.u32() != kIndexVersion) throw Error("unsupported index version");
    IndexFileContent content;
    const std::size_t packs = r.count(33);
    content.reserve(packs);
    for (std::size_t i = 0; i < packs; ++i) {
        Id pack_id;
        r.raw_into(pack_id);
        const std::size_t n = r.count(kMinBlobEntrySize);
        std::vector<BlobEntry> entries;
        entries.reserve(n);
        for (std::size_t j = 0; j < n; ++j) entries.push_back(read_blob_entry(r));
        content.emplace_back(pack_id, std::move(entries));
    }
    r.expect_done();
    return content;
}

}  // namespace vaultik
