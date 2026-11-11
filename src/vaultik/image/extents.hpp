#pragma once

// Which parts of a partition an image backup reads: from an NTFS volume bitmap (one bit per
// cluster, set when in use), the ranges to read, the small unused ranges stored as zeros inside
// the data around them, and the large unused ranges ("holes") stored as runs of zero chunks.

#include <cstdint>
#include <utility>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik::image {

struct Extent {
    enum class Kind : std::uint8_t {
        Data,  // read from the volume
        Zero,  // not in use, short: zeros inside the data stream
        Hole,  // not in use, long: zero chunks, and a restore need not write it
    };
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    Kind kind = Kind::Data;
    bool operator==(const Extent&) const = default;
};

using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;  // (offset, length), bytes

// Unused ranges this long or longer become holes (four of the largest chunks).
constexpr std::uint64_t kMinHole = 16u << 20;

// The extents covering [0, partition_length) in order, adjacent ones of the same kind merged.
// `bitmap` has one bit per cluster (bit 0 of byte 0 = cluster 0), `clusters` of them. Clusters
// past the bitmap and bytes past the last cluster (NTFS keeps a copy of its boot sector there)
// are read. `unused` are byte ranges to treat as not in use even when their clusters are (the
// page file and hibernation file).
std::vector<Extent> plan_extents(ByteSpan bitmap, std::uint64_t clusters, std::uint32_t cluster_size,
                                 std::uint64_t partition_length, const Ranges& unused = {},
                                 std::uint64_t min_hole = kMinHole);

// The extents clipped to [from, to).
std::vector<Extent> clip(const std::vector<Extent>& extents, std::uint64_t from, std::uint64_t to);

// The holes of a plan, as ranges.
Ranges holes_of(const std::vector<Extent>& extents);
// Bytes of Data extents.
std::uint64_t data_bytes(const std::vector<Extent>& extents);

// Whether [offset, offset + length) lies entirely inside the (sorted, non-overlapping) ranges.
bool inside(const Ranges& ranges, std::uint64_t offset, std::uint64_t length);

// The sizes of the zero chunks a hole of `length` bytes is stored as: `max_chunk` bytes each and
// a shorter last one.
std::vector<std::uint64_t> zero_chunk_sizes(std::uint64_t length, std::uint64_t max_chunk);

// Partitions are stored in segments of this size (the last one shorter), so an unchanged part of
// a large partition keeps its folder listing from one backup to the next.
constexpr std::uint64_t kSegmentSize = 1ull << 30;
constexpr std::uint64_t kSegmentsPerGroup = 64;

}  // namespace vaultik::image
