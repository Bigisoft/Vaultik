#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/pack.hpp"

namespace vaultik {

struct BlobLocation {
    std::uint32_t pack = 0;  // position in Index::packs()
    BlobType type = BlobType::Data;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t raw_len = 0;
};

// In-memory map from blob id to its location in a pack.
class Index {
public:
    // Returns the pack's position; adding the same pack twice returns the existing position.
    std::uint32_t add_pack(const Id& pack_id);
    void add(const BlobEntry& entry, std::uint32_t pack);

    const BlobLocation* find(const Id& blob_id) const;
    bool contains(const Id& blob_id) const { return find(blob_id) != nullptr; }

    const std::vector<Id>& packs() const { return packs_; }
    std::size_t blob_count() const { return blobs_.size(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [id, loc] : blobs_) fn(id, loc);
    }

private:
    std::vector<Id> packs_;
    std::unordered_map<Id, std::uint32_t, IdHash> pack_pos_;
    std::unordered_map<Id, BlobLocation, IdHash> blobs_;
};

// One index file: a list of packs with their blob entries.
using IndexFileContent = std::vector<std::pair<Id, std::vector<BlobEntry>>>;

Bytes serialize_index_file(const IndexFileContent& content);
IndexFileContent parse_index_file(ByteSpan data);

}  // namespace vaultik
