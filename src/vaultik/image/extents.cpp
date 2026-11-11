#include "vaultik/image/extents.hpp"

#include <algorithm>

namespace vaultik::image {

namespace {

void add(std::vector<Extent>& out, std::uint64_t offset, std::uint64_t length, Extent::Kind kind) {
    if (length == 0) return;
    if (!out.empty() && out.back().kind == kind && out.back().offset + out.back().length == offset) {
        out.back().length += length;
        return;
    }
    out.push_back({offset, length, kind});
}

}  // namespace

std::vector<Extent> plan_extents(ByteSpan bitmap, std::uint64_t clusters, std::uint32_t cluster_size,
                                 std::uint64_t partition_length, const Ranges& unused, std::uint64_t min_hole,
                                 const Ranges& keep) {
    if (cluster_size == 0) throw Error("cluster size of 0");
    clusters = std::min<std::uint64_t>(clusters, partition_length / cluster_size);
    const std::uint64_t mapped = std::min<std::uint64_t>(clusters, std::uint64_t{bitmap.size()} * 8);

    // Used / unused runs of the clusters the bitmap covers.
    std::vector<Extent> runs;
    std::uint64_t c = 0;
    while (c < mapped) {
        const bool used = (bitmap[c / 8] >> (c % 8)) & 1;
        std::uint64_t e = c + 1;
        // Whole bytes at once where they are all 0x00 or 0xFF.
        while (e < mapped) {
            if (e % 8 == 0 && e + 8 <= mapped) {
                const std::uint8_t b = bitmap[e / 8];
                if (b == (used ? 0xFF : 0x00)) {
                    e += 8;
                    continue;
                }
            }
            if (((bitmap[e / 8] >> (e % 8)) & 1) != (used ? 1 : 0)) break;
            ++e;
        }
        add(runs, c * cluster_size, (e - c) * cluster_size, used ? Extent::Kind::Data : Extent::Kind::Zero);
        c = e;
    }
    // Everything past the bitmap is read.
    add(runs, mapped * cluster_size, partition_length - mapped * cluster_size, Extent::Kind::Data);

    // Ranges declared unused, clipped to the cluster area.
    // Sorted, merged, and only whole clusters inside the cluster area.
    Ranges cut;
    {
        Ranges sorted = unused;
        std::sort(sorted.begin(), sorted.end());
        const std::uint64_t area = clusters * cluster_size;
        for (const auto& [off, len] : sorted) {
            const std::uint64_t b = (off + cluster_size - 1) / cluster_size * cluster_size;
            const std::uint64_t e = std::min<std::uint64_t>((off + len) / cluster_size * cluster_size, area);
            if (e <= b) continue;
            if (!cut.empty() && cut.back().first + cut.back().second >= b) {
                const std::uint64_t end = std::max(cut.back().first + cut.back().second, e);
                cut.back().second = end - cut.back().first;
            } else {
                cut.emplace_back(b, e - b);
            }
        }
    }
    if (!cut.empty()) {
        std::vector<Extent> next;
        for (const Extent& r : runs) {
            if (r.kind != Extent::Kind::Data) {
                add(next, r.offset, r.length, r.kind);
                continue;
            }
            std::uint64_t pos = r.offset;
            const std::uint64_t end = r.offset + r.length;
            auto it = std::upper_bound(cut.begin(), cut.end(), pos,
                                       [](std::uint64_t v, const std::pair<std::uint64_t, std::uint64_t>& c) {
                                           return v < c.first + c.second;
                                       });
            for (; it != cut.end() && it->first < end; ++it) {
                const std::uint64_t cb = std::max(it->first, pos);
                const std::uint64_t ce = std::min(it->first + it->second, end);
                add(next, pos, cb - pos, Extent::Kind::Data);
                add(next, cb, ce - cb, Extent::Kind::Zero);
                pos = ce;
            }
            add(next, pos, end - pos, Extent::Kind::Data);
        }
        runs = std::move(next);
    }

    // Ranges a restore must write, widened to whole clusters.
    Ranges written;
    for (const auto& [off, len] : keep) {
        if (len == 0) continue;
        const std::uint64_t b = off / cluster_size * cluster_size;
        const std::uint64_t e = std::min<std::uint64_t>((off + len + cluster_size - 1) / cluster_size * cluster_size,
                                                        partition_length);
        if (e > b) written.emplace_back(b, e - b);
    }
    std::sort(written.begin(), written.end());

    // Long unused runs become holes, but for the ranges to write, which stay zeros.
    std::vector<Extent> out;
    for (const Extent& r : runs) {
        if (r.kind != Extent::Kind::Zero || r.length < min_hole) {
            add(out, r.offset, r.length, r.kind);
            continue;
        }
        std::uint64_t pos = r.offset;
        const std::uint64_t end = r.offset + r.length;
        for (const auto& [wb, wl] : written) {
            const std::uint64_t b = std::max(wb, pos);
            const std::uint64_t e = std::min(wb + wl, end);
            if (e <= b) continue;
            add(out, pos, b - pos, Extent::Kind::Hole);
            add(out, b, e - b, Extent::Kind::Zero);
            pos = e;
        }
        add(out, pos, end - pos, Extent::Kind::Hole);
    }
    return out;
}

std::vector<Extent> clip(const std::vector<Extent>& extents, std::uint64_t from, std::uint64_t to) {
    std::vector<Extent> out;
    auto it = std::upper_bound(extents.begin(), extents.end(), from,
                               [](std::uint64_t v, const Extent& e) { return v < e.offset + e.length; });
    for (; it != extents.end() && it->offset < to; ++it) {
        const std::uint64_t b = std::max(it->offset, from);
        const std::uint64_t e = std::min(it->offset + it->length, to);
        if (e > b) out.push_back({b, e - b, it->kind});
    }
    return out;
}

Ranges holes_of(const std::vector<Extent>& extents) {
    Ranges out;
    for (const Extent& e : extents)
        if (e.kind == Extent::Kind::Hole) out.emplace_back(e.offset, e.length);
    return out;
}

std::uint64_t data_bytes(const std::vector<Extent>& extents) {
    std::uint64_t n = 0;
    for (const Extent& e : extents)
        if (e.kind == Extent::Kind::Data) n += e.length;
    return n;
}

bool inside(const Ranges& ranges, std::uint64_t offset, std::uint64_t length) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), offset,
                               [](std::uint64_t v, const std::pair<std::uint64_t, std::uint64_t>& r) { return v < r.first; });
    if (it == ranges.begin()) return false;
    --it;
    return offset >= it->first && offset + length <= it->first + it->second;
}

std::vector<std::uint64_t> zero_chunk_sizes(std::uint64_t length, std::uint64_t max_chunk) {
    std::vector<std::uint64_t> out(length / max_chunk, max_chunk);
    if (length % max_chunk != 0) out.push_back(length % max_chunk);
    return out;
}

}  // namespace vaultik::image
