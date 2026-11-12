#include "vaultik/image/ntfs.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <string>

namespace vaultik::image {

namespace {

// Metadata files by their record number.
constexpr std::uint64_t kMftMirr = 1, kLogFile = 2, kVolume = 3, kRoot = 5, kBitmap = 6, kBadClus = 8;

// Attribute types.
constexpr std::uint32_t kAttrList = 0x20, kFileName = 0x30, kVolumeInformation = 0x70, kData = 0x80,
                        kIndexRoot = 0x90, kIndexAllocation = 0xA0, kEnd = 0xFFFFFFFF;
// Attribute flags.
constexpr std::uint16_t kCompressed = 0x0001, kEncrypted = 0x4000, kSparse = 0x8000;
// The update sequence stride: every 512 bytes of a record end in the sequence number.
constexpr std::uint32_t kStride = 512;
// The volume's dirty flag in $VOLUME_INFORMATION.
constexpr std::uint16_t kVolumeDirty = 0x0001;

[[noreturn]] void refuse(const std::string& why) { throw Error("the NTFS volume cannot be shrunk: " + why); }

std::uint64_t get(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
void put(std::uint8_t* p, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Bounds-checked little-endian access to a record or sector.
std::uint64_t at(const Bytes& b, std::size_t off, int n) {
    if (off + static_cast<std::size_t>(n) > b.size()) refuse("a metadata record is damaged");
    return get(b.data() + off, n);
}
void set(Bytes& b, std::size_t off, std::uint64_t v, int n) {
    if (off + static_cast<std::size_t>(n) > b.size()) refuse("a metadata record is damaged");
    put(b.data() + off, v, n);
}

// One run of a non-resident attribute: `length` clusters from `vcn` on, at `lcn` (-1: sparse).
struct Run {
    std::uint64_t vcn = 0;
    std::int64_t lcn = -1;
    std::uint64_t length = 0;
};

// The bytes a signed value takes in mapping pairs.
int signed_bytes(std::int64_t v) {
    for (int n = 1; n < 8; ++n) {
        const std::int64_t lim = std::int64_t{1} << (8 * n - 1);
        if (v >= -lim && v < lim) return n;
    }
    return 8;
}

// Encodes runs as mapping pairs, ending in a zero byte.
Bytes encode_runs(const std::vector<Run>& runs) {
    Bytes out;
    std::int64_t prev = 0;
    for (const Run& r : runs) {
        const int nl = signed_bytes(static_cast<std::int64_t>(r.length));
        int no = 0;
        std::int64_t delta = 0;
        if (r.lcn >= 0) {
            delta = r.lcn - prev;
            no = signed_bytes(delta);
            prev = r.lcn;
        }
        out.push_back(static_cast<std::uint8_t>((no << 4) | nl));
        for (int i = 0; i < nl; ++i) out.push_back(static_cast<std::uint8_t>(r.length >> (8 * i)));
        for (int i = 0; i < no; ++i) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(delta) >> (8 * i)));
    }
    out.push_back(0);
    return out;
}

// An attribute inside a (fixed-up) file record.
struct Attr {
    std::size_t off = 0;  // in the record
    std::uint32_t type = 0;
    std::uint32_t length = 0;
    bool non_resident = false;
    std::u16string name;
    std::uint16_t flags = 0;
};

std::vector<Attr> attributes(const Bytes& rec) {
    std::vector<Attr> out;
    std::size_t off = static_cast<std::size_t>(at(rec, 0x14, 2));
    const std::size_t used = std::min<std::size_t>(static_cast<std::size_t>(at(rec, 0x18, 4)), rec.size());
    while (true) {
        if (off + 8 > used) refuse("a file record has no end marker");
        Attr a;
        a.off = off;
        a.type = static_cast<std::uint32_t>(at(rec, off, 4));
        if (a.type == kEnd) break;
        a.length = static_cast<std::uint32_t>(at(rec, off + 4, 4));
        if (a.length < 0x18 || a.length % 8 != 0 || off + a.length > used) refuse("a file record has a damaged attribute");
        a.non_resident = at(rec, off + 8, 1) != 0;
        if (a.non_resident && a.length < 0x40) refuse("a file record has a damaged attribute");
        const std::size_t name_len = static_cast<std::size_t>(at(rec, off + 9, 1));
        const std::size_t name_off = static_cast<std::size_t>(at(rec, off + 10, 2));
        if (name_off + 2 * name_len > a.length) refuse("a file record has a damaged attribute name");
        for (std::size_t i = 0; i < name_len; ++i)
            a.name.push_back(static_cast<char16_t>(at(rec, off + name_off + 2 * i, 2)));
        a.flags = static_cast<std::uint16_t>(at(rec, off + 12, 2));
        off += a.length;
        out.push_back(std::move(a));
    }
    return out;
}

const Attr* find_attr(const std::vector<Attr>& attrs, std::uint32_t type, std::u16string_view name) {
    for (const Attr& a : attrs)
        if (a.type == type && a.name == name) return &a;
    return nullptr;
}

// The runs of a non-resident attribute. Checks that they are well formed and lie inside
// `clusters` clusters.
std::vector<Run> decode_runs(const Bytes& rec, const Attr& a, std::uint64_t clusters) {
    std::vector<Run> runs;
    const std::size_t mp = static_cast<std::size_t>(at(rec, a.off + 0x20, 2));
    if (mp < 0x40 || mp >= a.length) refuse("an attribute has damaged runs");
    std::uint64_t vcn = at(rec, a.off + 0x10, 8);
    const std::uint64_t highest = at(rec, a.off + 0x18, 8);
    std::size_t p = a.off + mp;
    const std::size_t end = a.off + a.length;
    std::int64_t lcn = 0;
    while (true) {
        if (p >= end) refuse("an attribute has damaged runs");
        const std::uint8_t h = rec[p++];
        if (h == 0) break;
        const int nl = h & 0x0F;
        const int no = h >> 4;
        if (nl == 0 || nl > 8 || no > 8 || p + static_cast<std::size_t>(nl + no) > end) refuse("an attribute has damaged runs");
        const std::uint64_t len = get(&rec[p], nl);
        p += static_cast<std::size_t>(nl);
        if (len == 0 || len >= (std::uint64_t{1} << 62)) refuse("an attribute has damaged runs");
        Run r;
        r.vcn = vcn;
        r.length = len;
        if (no > 0) {
            std::uint64_t d = get(&rec[p], no);
            if (no < 8 && (d >> (8 * no - 1)) != 0) d |= ~std::uint64_t{0} << (8 * no);  // sign extension
            p += static_cast<std::size_t>(no);
            lcn += static_cast<std::int64_t>(d);
            if (lcn < 0 || static_cast<std::uint64_t>(lcn) > clusters || len > clusters - static_cast<std::uint64_t>(lcn))
                refuse("a file's data lies outside the volume");
            r.lcn = lcn;
        }
        runs.push_back(r);
        vcn += len;
    }
    if (!runs.empty() && vcn != highest + 1) refuse("an attribute's runs do not match its size");
    return runs;
}

std::uint64_t run_clusters(const std::vector<Run>& runs) {
    std::uint64_t n = 0;
    for (const Run& r : runs) n += r.length;
    return n;
}

// Undoes the update sequence of a record read from disk; false when it is torn or damaged.
bool unfix(Bytes& rec) {
    const std::size_t usa = static_cast<std::size_t>(get(&rec[4], 2));
    const std::size_t count = static_cast<std::size_t>(get(&rec[6], 2));
    if (count != rec.size() / kStride + 1 || usa < 0x08 || usa + 2 * count > rec.size()) return false;
    const std::uint16_t usn = static_cast<std::uint16_t>(get(&rec[usa], 2));
    for (std::size_t i = 1; i < count; ++i) {
        const std::size_t end = i * kStride - 2;
        if (get(&rec[end], 2) != usn) return false;
        rec[end] = rec[usa + 2 * i];
        rec[end + 1] = rec[usa + 2 * i + 1];
    }
    return true;
}

// The record as it goes to disk: a new sequence number at the end of every 512 bytes.
Bytes refix(Bytes rec) {
    const std::size_t usa = static_cast<std::size_t>(get(&rec[4], 2));
    const std::size_t count = static_cast<std::size_t>(get(&rec[6], 2));
    std::uint16_t usn = static_cast<std::uint16_t>(get(&rec[usa], 2) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    put(&rec[usa], usn, 2);
    for (std::size_t i = 1; i < count; ++i) {
        const std::size_t end = i * kStride - 2;
        rec[usa + 2 * i] = rec[end];
        rec[usa + 2 * i + 1] = rec[end + 1];
        put(&rec[end], usn, 2);
    }
    return rec;
}

class Volume {
public:
    Volume(const VolumeReader& read, std::uint64_t partition_length) : read_(read) {
        Bytes head = bytes(0, 512);
        if (std::memcmp(&head[3], "NTFS    ", 8) != 0 || head[510] != 0x55 || head[511] != 0xAA)
            refuse("the partition does not start with an NTFS boot sector");
        f.sector_size = static_cast<std::uint32_t>(get(&head[0x0B], 2));
        if (f.sector_size < 512 || f.sector_size > 4096 || !std::has_single_bit(f.sector_size)) refuse("its sector size is not supported");
        const std::uint8_t spc = head[0x0D];
        const std::uint64_t sectors_per_cluster = spc <= 0x80 ? spc : std::uint64_t{1} << (256 - spc);
        if (sectors_per_cluster == 0 || !std::has_single_bit(sectors_per_cluster) || sectors_per_cluster * f.sector_size > (2u << 20))
            refuse("its cluster size is not supported");
        f.cluster_size = static_cast<std::uint32_t>(sectors_per_cluster * f.sector_size);
        const auto rec = static_cast<std::int8_t>(head[0x40]);
        const std::uint64_t record_size =
            rec > 0 ? std::uint64_t(rec) * f.cluster_size : rec < 0 && rec > -31 ? std::uint64_t{1} << -rec : 0;
        if (record_size < 1024 || record_size > 65536 || !std::has_single_bit(record_size)) refuse("its file record size is not supported");
        f.record_size = static_cast<std::uint32_t>(record_size);
        f.total_sectors = get(&head[0x28], 8);
        f.clusters = f.total_sectors / sectors_per_cluster;
        if (f.clusters == 0 || f.total_sectors > partition_length / f.sector_size)
            refuse("its size does not match its partition");
        boot = bytes(0, f.sector_size);
        const std::uint64_t mft_lcn = get(&boot[0x30], 8);
        if (mft_lcn >= f.clusters) refuse("its master file table lies outside the volume");

        // $MFT: its runs, from record 0 at the place the boot sector gives. A master file table
        // too fragmented for one record (it then has an attribute list) is not handled here.
        Bytes r0 = bytes(mft_lcn * f.cluster_size, f.record_size);
        if (std::memcmp(r0.data(), "FILE", 4) != 0 || !unfix(r0)) refuse("its master file table is damaged");
        const std::vector<Attr> a0 = attributes(r0);
        if (find_attr(a0, kAttrList, u"") != nullptr) refuse("its master file table is too fragmented");
        const Attr* data = find_attr(a0, kData, u"");
        if (data == nullptr || !data->non_resident || at(r0, data->off + 0x10, 8) != 0) refuse("its master file table is damaged");
        mft_runs = decode_runs(r0, *data, f.clusters);
        if (run_clusters(mft_runs) * f.cluster_size < at(r0, data->off + 0x28, 8)) refuse("its master file table is too fragmented");
        for (const Run& r : mft_runs)
            if (r.lcn < 0) refuse("its master file table is damaged");
        mft_records = std::min(at(r0, data->off + 0x30, 8), at(r0, data->off + 0x38, 8)) / f.record_size;
        if (mft_records <= kBadClus) refuse("its master file table is damaged");

        // $Bitmap: the clusters in use.
        Bytes r6 = record(kBitmap);
        const std::vector<Attr> a6 = attributes(r6);
        const Attr* bm = find_attr(a6, kData, u"");
        if (bm == nullptr || !bm->non_resident) refuse("$Bitmap is damaged");
        bitmap_runs = decode_runs(r6, *bm, f.clusters);
        for (const Run& r : bitmap_runs)
            if (r.lcn < 0) refuse("$Bitmap is damaged");
        const std::uint64_t size = at(r6, bm->off + 0x30, 8);
        if (size < (f.clusters + 7) / 8 || size > (std::uint64_t{1} << 34)) refuse("$Bitmap is too short");
        bitmap = attr_bytes(bitmap_runs, 0, static_cast<std::size_t>(size));
        // Whole bytes at a time, the last one bit by bit (bits past the last cluster do not count).
        for (std::uint64_t c = 0; c < f.clusters;) {
            const std::uint8_t byte = bitmap[static_cast<std::size_t>(c / 8)];
            if (c + 8 <= f.clusters) {
                if (byte != 0) {
                    f.used_clusters += static_cast<std::uint64_t>(std::popcount(byte));
                    f.used_end = c + static_cast<std::uint64_t>(std::bit_width(byte));
                }
                c += 8;
                continue;
            }
            if (byte & (1u << (c % 8))) {
                f.used_end = c + 1;
                ++f.used_clusters;
            }
            ++c;
        }
    }

    NtfsFacts f;
    Bytes boot;
    std::vector<Run> mft_runs;
    std::uint64_t mft_records = 0;
    std::vector<Run> bitmap_runs;
    Bytes bitmap;

    Bytes bytes(std::uint64_t off, std::size_t len) const {
        Bytes b(len);
        read_(off, b.data(), len);
        return b;
    }

    // The volume byte ranges holding bytes [off, off + len) of an attribute.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> places(const std::vector<Run>& runs, std::uint64_t off,
                                                                std::uint64_t len) const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        const std::uint64_t cs = f.cluster_size;
        std::uint64_t pos = off;
        const std::uint64_t end = off + len;
        for (const Run& r : runs) {
            const std::uint64_t rb = r.vcn * cs, re = (r.vcn + r.length) * cs;
            if (pos >= end) break;
            if (re <= pos || rb >= end) continue;
            if (rb > pos) refuse("an attribute has damaged runs");
            const std::uint64_t n = std::min(end, re) - pos;
            if (r.lcn < 0) refuse("a metadata file has unexpected sparse runs");
            out.emplace_back(static_cast<std::uint64_t>(r.lcn) * cs + (pos - rb), n);
            pos += n;
        }
        if (pos < end) refuse("a metadata file is shorter than it says");
        return out;
    }

    Bytes attr_bytes(const std::vector<Run>& runs, std::uint64_t off, std::size_t len) const {
        Bytes out;
        out.reserve(len);
        for (const auto& [v, n] : places(runs, off, len)) {
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(n));
            read_(v, out.data() + at, static_cast<std::size_t>(n));
        }
        return out;
    }

    // File record `n`, fixed up. Throws when it is not a valid record in use.
    Bytes record(std::uint64_t n) const {
        Bytes r = attr_bytes(mft_runs, n * f.record_size, f.record_size);
        if (std::memcmp(r.data(), "FILE", 4) != 0 || !unfix(r) || (get(&r[0x16], 2) & 1) == 0)
            refuse("file record " + std::to_string(n) + " is damaged");
        return r;
    }

private:
    const VolumeReader& read_;
};

// Changed sectors of the volume, read from the stored volume when first touched.
class SectorPatches {
public:
    explicit SectorPatches(const Volume& v) : v_(v) {}

    void write(std::uint64_t off, const std::uint8_t* p, std::size_t n) {
        const std::uint64_t ss = v_.f.sector_size;
        while (n > 0) {
            const std::uint64_t s = off / ss;
            auto it = sectors_.find(s);
            if (it == sectors_.end()) it = sectors_.emplace(s, v_.bytes(s * ss, static_cast<std::size_t>(ss))).first;
            const std::size_t within = static_cast<std::size_t>(off % ss);
            const std::size_t k = std::min<std::size_t>(n, static_cast<std::size_t>(ss) - within);
            std::memcpy(it->second.data() + within, p, k);
            off += k;
            p += k;
            n -= k;
        }
    }

    std::vector<DiskWrite> writes() const {
        std::vector<DiskWrite> out;
        const std::uint64_t ss = v_.f.sector_size;
        for (const auto& [s, data] : sectors_) {
            if (!out.empty() && out.back().offset + out.back().data.size() == s * ss)
                out.back().data.insert(out.back().data.end(), data.begin(), data.end());
            else
                out.push_back({s * ss, data});
        }
        return out;
    }

private:
    const Volume& v_;
    std::map<std::uint64_t, Bytes> sectors_;
};

// Sets the allocated and data sizes in a $FILE_NAME value at `p` when it holds the old ones (the
// copy of a file's sizes that a file name keeps; left alone when it was not kept up to date).
// True when it changed them.
bool update_name_sizes(Bytes& b, std::size_t p, std::uint64_t old_alloc, std::uint64_t old_size, std::uint64_t new_alloc,
                       std::uint64_t new_size) {
    if (at(b, p + 0x28, 8) != old_alloc || at(b, p + 0x30, 8) != old_size) return false;
    set(b, p + 0x28, new_alloc, 8);
    set(b, p + 0x30, new_size, 8);
    return true;
}

// Updates the file name sizes of file record `ref` in the index entries from `begin` to `end` of
// `b`; true when one changed.
bool update_index_entries(Bytes& b, std::size_t begin, std::size_t end, std::uint64_t ref, std::uint64_t old_alloc,
                          std::uint64_t old_size, std::uint64_t new_alloc, std::uint64_t new_size) {
    bool changed = false;
    std::size_t p = begin;
    while (p + 16 <= end) {
        const std::size_t len = static_cast<std::size_t>(at(b, p + 8, 2));
        const std::size_t key = static_cast<std::size_t>(at(b, p + 10, 2));
        const std::uint32_t flags = static_cast<std::uint32_t>(at(b, p + 12, 4));
        if (len < 16 || p + len > end) refuse("the root folder's index is damaged");
        if ((flags & 2) != 0) break;  // the last entry has no key
        if ((at(b, p, 8) & 0xFFFFFFFFFFFFull) == ref && key >= 0x42 && 16 + key <= len)
            changed = update_name_sizes(b, p + 16, old_alloc, old_size, new_alloc, new_size) || changed;
        p += len;
    }
    return changed;
}

}  // namespace

NtfsFacts ntfs_facts(const VolumeReader& read, std::uint64_t partition_length) {
    return Volume(read, partition_length).f;
}

std::uint64_t ntfs_min_length(const NtfsFacts& f, std::uint64_t align, std::uint64_t reserve) {
    const std::uint64_t spc = f.cluster_size / f.sector_size;
    std::uint64_t clusters = std::max(f.used_end, f.used_clusters + (reserve + f.cluster_size - 1) / f.cluster_size);
    clusters = std::max<std::uint64_t>(clusters, 1);
    const std::uint64_t bytes = (clusters * spc + 1) * f.sector_size;  // and the backup boot sector
    return (bytes + align - 1) / align * align;
}

VolumePatch ntfs_shrink(const VolumeReader& read, std::uint64_t old_length, std::uint64_t new_length,
                        std::uint64_t old_first_sector, std::uint64_t new_first_sector) {
    const Volume v(read, old_length);
    const NtfsFacts& f = v.f;
    const std::uint64_t cs = f.cluster_size;
    const std::uint64_t spc = cs / f.sector_size;
    if (new_length % f.sector_size != 0 || new_length > old_length || new_length < 2ull * f.sector_size)
        refuse("the new size is not whole sectors");
    const std::uint64_t new_sectors = new_length / f.sector_size - 1;  // the last one holds the backup boot sector
    const std::uint64_t new_clusters = new_sectors / spc;
    if (new_clusters < f.used_end)
        refuse("clusters in use reach " + std::to_string(f.used_end * cs) + " bytes into it, past the new end");

    // Every file record in use: no run of any attribute may reach past the new end (the bitmap
    // says none does; a volume where they disagree is not cut).
    const std::uint64_t rs = f.record_size;
    const std::uint64_t batch = std::max<std::uint64_t>(1, (1u << 20) / rs);
    for (std::uint64_t first = 0; first < v.mft_records; first += batch) {
        const std::uint64_t count = std::min(batch, v.mft_records - first);
        const Bytes block = v.attr_bytes(v.mft_runs, first * rs, static_cast<std::size_t>(count * rs));
        for (std::uint64_t k = 0; k < count; ++k) {
            Bytes r(block.begin() + static_cast<std::ptrdiff_t>(k * rs), block.begin() + static_cast<std::ptrdiff_t>((k + 1) * rs));
            const bool file = std::memcmp(r.data(), "FILE", 4) == 0;
            if (!file) {
                if (std::memcmp(r.data(), "BAAD", 4) == 0) refuse("file record " + std::to_string(first + k) + " is damaged (run chkdsk on it)");
                continue;  // never used
            }
            if ((get(&r[0x16], 2) & 1) == 0) continue;  // not in use
            if (!unfix(r)) refuse("file record " + std::to_string(first + k) + " is damaged (run chkdsk on it)");
            for (const Attr& a : attributes(r)) {
                if (!a.non_resident) continue;
                for (const Run& run : decode_runs(r, a, f.clusters))
                    if (run.lcn >= 0 && static_cast<std::uint64_t>(run.lcn) + run.length > new_clusters)
                        refuse("file record " + std::to_string(first + k) +
                               " has data past the new end that the volume bitmap shows as free (run chkdsk on it)");
            }
        }
    }

    SectorPatches out(v);
    std::vector<std::pair<std::uint64_t, Bytes>> records;  // changed file records, fixed up

    // $Bitmap: one bit per cluster of the smaller volume, in whole 8 bytes; its clusters past
    // what that needs are freed (its runs are cut, so nothing of it moves).
    Bytes r6 = v.record(kBitmap);
    const std::vector<Attr> a6 = attributes(r6);
    if (find_attr(a6, kAttrList, u"") != nullptr) refuse("$Bitmap is too fragmented");
    const Attr& bm = *find_attr(a6, kData, u"");
    if ((bm.flags & (kCompressed | kSparse | kEncrypted)) != 0 || at(r6, bm.off + 0x10, 8) != 0) refuse("$Bitmap is not as expected");
    const std::uint64_t bm_old_alloc = at(r6, bm.off + 0x28, 8);
    const std::uint64_t bm_old_size = at(r6, bm.off + 0x30, 8);
    const std::uint64_t bm_size = ((new_clusters + 7) / 8 + 7) / 8 * 8;
    const std::uint64_t bm_clusters = (bm_size + cs - 1) / cs;
    if (run_clusters(v.bitmap_runs) < bm_clusters || bm_old_size < bm_size) refuse("$Bitmap is shorter than it should be");
    Bytes bits(v.bitmap.begin(), v.bitmap.begin() + static_cast<std::ptrdiff_t>(bm_size));
    auto set_bit = [&](std::uint64_t c, bool on) {
        if (on) bits[c / 8] |= static_cast<std::uint8_t>(1u << (c % 8));
        else bits[c / 8] &= static_cast<std::uint8_t>(~(1u << (c % 8)));
    };
    // The bits past the last cluster: as the volume had them (set by most tools), set when it had none.
    const std::uint64_t old_pad_end = std::min<std::uint64_t>(bm_old_size * 8, ((f.clusters + 7) / 8 + 7) / 8 * 64);
    bool pad = true;
    if (old_pad_end > f.clusters) {
        pad = false;
        for (std::uint64_t c = f.clusters; c < old_pad_end; ++c)
            if (v.bitmap[c / 8] & (1u << (c % 8))) pad = true;
    }
    for (std::uint64_t c = new_clusters; c < bm_size * 8; ++c) set_bit(c, pad);
    std::vector<Run> kept;
    for (const Run& r : v.bitmap_runs) {
        if (r.vcn >= bm_clusters) {
            for (std::uint64_t c = 0; c < r.length; ++c) set_bit(static_cast<std::uint64_t>(r.lcn) + c, false);
            continue;
        }
        Run k = r;
        if (r.vcn + r.length > bm_clusters) {
            k.length = bm_clusters - r.vcn;
            for (std::uint64_t c = k.length; c < r.length; ++c) set_bit(static_cast<std::uint64_t>(r.lcn) + c, false);
        }
        kept.push_back(k);
    }
    {
        // The new runs take no more room than the old ones (cutting never makes them longer).
        const Bytes mp = encode_runs(kept);
        const std::size_t mp_off = static_cast<std::size_t>(at(r6, bm.off + 0x20, 2));
        if (mp_off + mp.size() > bm.length) refuse("$Bitmap's runs do not fit");
        std::fill(r6.begin() + static_cast<std::ptrdiff_t>(bm.off + mp_off), r6.begin() + static_cast<std::ptrdiff_t>(bm.off + bm.length),
                  std::uint8_t{0});
        std::copy(mp.begin(), mp.end(), r6.begin() + static_cast<std::ptrdiff_t>(bm.off + mp_off));
        set(r6, bm.off + 0x18, bm_clusters - 1, 8);  // highest VCN
        set(r6, bm.off + 0x28, bm_clusters * cs, 8);
        set(r6, bm.off + 0x30, bm_size, 8);
        set(r6, bm.off + 0x38, bm_size, 8);
        for (const Attr& a : a6)
            if (a.type == kFileName && !a.non_resident)
                update_name_sizes(r6, a.off + static_cast<std::size_t>(at(r6, a.off + 0x14, 2)), bm_old_alloc, bm_old_size,
                                  bm_clusters * cs, bm_size);
        records.emplace_back(kBitmap, std::move(r6));
        // Its data: the bits, then zeros to the end of its last cluster.
        bits.resize(static_cast<std::size_t>(bm_clusters * cs), 0);
        std::size_t pos = 0;
        for (const auto& [off, n] : v.places(kept, 0, bm_clusters * cs)) {
            out.write(off, bits.data() + pos, static_cast<std::size_t>(n));
            pos += static_cast<std::size_t>(n);
        }
    }

    // $BadClus: its $Bad stream spans the volume (sparse, with the bad clusters in it); cut it to
    // the new size.
    {
        Bytes r8 = v.record(kBadClus);
        const std::vector<Attr> a8 = attributes(r8);
        if (find_attr(a8, kAttrList, u"") != nullptr) refuse("$BadClus is not as expected");
        const Attr* bad = find_attr(a8, kData, u"$Bad");
        if (bad == nullptr || !bad->non_resident || at(r8, bad->off + 0x10, 8) != 0) refuse("$BadClus is not as expected");
        const std::vector<Run> runs = decode_runs(r8, *bad, f.clusters);
        if (run_clusters(runs) < new_clusters) refuse("$BadClus is shorter than the volume");
        std::vector<Run> cut;
        std::uint64_t real = 0;
        for (const Run& r : runs) {
            if (r.vcn >= new_clusters) break;
            Run k = r;
            k.length = std::min(r.length, new_clusters - r.vcn);
            if (k.lcn >= 0) real += k.length;
            cut.push_back(k);
        }
        const Bytes mp = encode_runs(cut);
        const std::size_t mp_off = static_cast<std::size_t>(at(r8, bad->off + 0x20, 2));
        if (mp_off + mp.size() > bad->length) refuse("$BadClus's runs do not fit");
        std::fill(r8.begin() + static_cast<std::ptrdiff_t>(bad->off + mp_off),
                  r8.begin() + static_cast<std::ptrdiff_t>(bad->off + bad->length), std::uint8_t{0});
        std::copy(mp.begin(), mp.end(), r8.begin() + static_cast<std::ptrdiff_t>(bad->off + mp_off));
        const std::uint64_t bytes = new_clusters * cs;
        set(r8, bad->off + 0x18, new_clusters - 1, 8);
        set(r8, bad->off + 0x28, bytes, 8);
        set(r8, bad->off + 0x30, bytes, 8);
        set(r8, bad->off + 0x38, std::min(at(r8, bad->off + 0x38, 8), bytes), 8);
        if ((bad->flags & (kCompressed | kSparse)) != 0) {
            if (mp_off < 0x48) refuse("$BadClus is not as expected");
            set(r8, bad->off + 0x40, real * cs, 8);  // the clusters it really holds
        }
        records.emplace_back(kBadClus, std::move(r8));
    }

    // The root folder's index keeps a copy of $Bitmap's sizes in its entry for it.
    {
        Bytes r5 = v.record(kRoot);
        const std::vector<Attr> a5 = attributes(r5);
        const Attr* root = find_attr(a5, kIndexRoot, u"$I30");
        if (root == nullptr || root->non_resident) refuse("the root folder is damaged");
        const std::size_t value = root->off + static_cast<std::size_t>(at(r5, root->off + 0x14, 2));
        const std::size_t value_len = static_cast<std::size_t>(at(r5, root->off + 0x10, 4));
        if (value + value_len > root->off + root->length || value_len < 32) refuse("the root folder is damaged");
        const std::uint64_t block_size = at(r5, value + 8, 4);
        const std::size_t header = value + 16;
        const std::size_t entries = header + static_cast<std::size_t>(at(r5, header, 4));
        const std::size_t entries_end = header + static_cast<std::size_t>(at(r5, header + 4, 4));
        if (entries_end > value + value_len) refuse("the root folder is damaged");
        if (update_index_entries(r5, entries, entries_end, kBitmap, bm_old_alloc, bm_old_size, bm_clusters * cs, bm_size))
            records.emplace_back(kRoot, r5);
        if (const Attr* alloc = find_attr(a5, kIndexAllocation, u"$I30")) {
            if (!alloc->non_resident || block_size < kStride || block_size > 65536 || !std::has_single_bit(block_size))
                refuse("the root folder is damaged");
            const std::vector<Run> runs = decode_runs(r5, *alloc, f.clusters);
            const std::uint64_t size = at(r5, alloc->off + 0x30, 8);
            for (std::uint64_t b = 0; b + block_size <= size; b += block_size) {
                Bytes blk = v.attr_bytes(runs, b, static_cast<std::size_t>(block_size));
                if (std::memcmp(blk.data(), "INDX", 4) != 0 || !unfix(blk)) continue;  // not in use
                const std::size_t h = 0x18;
                const std::size_t e = h + static_cast<std::size_t>(at(blk, h, 4));
                const std::size_t ee = h + static_cast<std::size_t>(at(blk, h + 4, 4));
                if (ee > blk.size()) refuse("the root folder's index is damaged");
                if (!update_index_entries(blk, e, ee, kBitmap, bm_old_alloc, bm_old_size, bm_clusters * cs, bm_size)) continue;
                const Bytes disk = refix(std::move(blk));
                std::size_t pos = 0;
                for (const auto& [off, n] : v.places(runs, b, block_size)) {
                    out.write(off, disk.data() + pos, static_cast<std::size_t>(n));
                    pos += static_cast<std::size_t>(n);
                }
            }
        }
    }

    // $Volume: marked dirty, so Windows checks the volume once when it mounts it first.
    {
        Bytes r3 = v.record(kVolume);
        const std::vector<Attr> a3 = attributes(r3);
        const Attr* info = find_attr(a3, kVolumeInformation, u"");
        if (info == nullptr || info->non_resident || at(r3, info->off + 0x10, 4) < 12) refuse("$Volume is damaged");
        const std::size_t value = info->off + static_cast<std::size_t>(at(r3, info->off + 0x14, 2));
        set(r3, value + 0x0A, at(r3, value + 0x0A, 2) | kVolumeDirty, 2);
        records.emplace_back(kVolume, std::move(r3));
    }

    // $LogFile: emptied (all 0xFF), so nothing logged against the old metadata is replayed.
    VolumePatch patch;
    {
        const Bytes r2 = v.record(kLogFile);
        const std::vector<Attr> a2 = attributes(r2);
        const Attr* log = find_attr(a2, kData, u"");
        if (log == nullptr || !log->non_resident || find_attr(a2, kAttrList, u"") != nullptr) refuse("$LogFile is not as expected");
        const std::vector<Run> runs = decode_runs(r2, *log, f.clusters);
        for (const auto& range : v.places(runs, 0, at(r2, log->off + 0x30, 8))) patch.fill.push_back(range);
    }

    // The changed records, where the master file table keeps them and, for the first few, in its
    // mirror too.
    const Bytes r1 = v.record(kMftMirr);
    const std::vector<Attr> a1 = attributes(r1);
    const Attr* mirr = find_attr(a1, kData, u"");
    if (mirr == nullptr || !mirr->non_resident) refuse("$MFTMirr is damaged");
    const std::vector<Run> mirr_runs = decode_runs(r1, *mirr, f.clusters);
    const std::uint64_t mirrored = at(r1, mirr->off + 0x30, 8) / rs;
    for (auto& [n, rec] : records) {
        const Bytes disk = refix(rec);
        auto write_to = [&](const std::vector<Run>& runs) {
            std::size_t pos = 0;
            for (const auto& [off, len] : v.places(runs, n * rs, rs)) {
                out.write(off, disk.data() + pos, static_cast<std::size_t>(len));
                pos += static_cast<std::size_t>(len);
            }
        };
        write_to(v.mft_runs);
        if (n < mirrored) write_to(mirr_runs);
    }

    // The boot sector, and its backup in the partition's new last sector.
    Bytes boot = v.boot;
    put(&boot[0x28], new_sectors, 8);
    if (get(&boot[0x1C], 4) == old_first_sector && new_first_sector <= 0xFFFFFFFFu) put(&boot[0x1C], new_first_sector, 4);
    out.write(0, boot.data(), boot.size());
    out.write(new_sectors * f.sector_size, boot.data(), boot.size());

    patch.writes = out.writes();
    return patch;
}

VolumePatch ntfs_move(const VolumeReader& read, std::uint64_t length, std::uint64_t old_first_sector,
                      std::uint64_t new_first_sector) {
    VolumePatch patch;
    if (length < 1024 || new_first_sector > 0xFFFFFFFFu) return patch;
    Bytes head(512);
    read(0, head.data(), head.size());
    if (std::memcmp(&head[3], "NTFS    ", 8) != 0 || head[510] != 0x55 || head[511] != 0xAA) return patch;
    const std::uint64_t ss = get(&head[0x0B], 2);
    if (ss < 512 || ss > 4096 || !std::has_single_bit(ss) || length % ss != 0) return patch;
    if (get(&head[0x1C], 4) != old_first_sector) return patch;
    Bytes boot(static_cast<std::size_t>(ss));
    read(0, boot.data(), boot.size());
    const std::uint64_t backup = get(&boot[0x28], 8) * ss;
    put(&boot[0x1C], new_first_sector, 4);
    patch.writes.push_back({0, boot});
    if (backup + ss <= length && backup >= ss) {
        Bytes old(static_cast<std::size_t>(ss));
        read(backup, old.data(), old.size());
        if (std::memcmp(&old[3], "NTFS    ", 8) == 0 && get(&old[0x1C], 4) == old_first_sector) {
            put(&old[0x1C], new_first_sector, 4);
            patch.writes.push_back({backup, old});
        }
    }
    return patch;
}

}  // namespace vaultik::image
