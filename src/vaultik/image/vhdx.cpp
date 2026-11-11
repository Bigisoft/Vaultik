#include "vaultik/image/vhdx.hpp"

#include <algorithm>
#include <cstring>

namespace vaultik::image {

namespace {

constexpr std::uint64_t kMiB = 1u << 20;
constexpr std::uint64_t kHeader1 = 64 * 1024;
constexpr std::uint64_t kHeader2 = 128 * 1024;
constexpr std::uint64_t kRegion1 = 192 * 1024;
constexpr std::uint64_t kRegion2 = 256 * 1024;
constexpr std::uint64_t kLogOffset = 1 * kMiB;
constexpr std::uint64_t kLogLength = 1 * kMiB;
constexpr std::uint64_t kMetadataOffset = 2 * kMiB;
constexpr std::uint64_t kMetadataLength = 1 * kMiB;
constexpr std::uint64_t kBatOffset = 3 * kMiB;

constexpr std::string_view kBatRegion = "2DC27766-F623-4200-9D64-115E9BFD4A08";
constexpr std::string_view kMetadataRegion = "8B7CA206-4790-4B9A-B8FE-575F050F886E";
constexpr std::string_view kFileParameters = "CAA16737-FA36-4D43-B3B6-33F0AA44E76B";
constexpr std::string_view kVirtualDiskSize = "2FA54224-CD1B-4876-B211-5DBED83BF4B8";
constexpr std::string_view kVirtualDiskId = "BECA12AB-B2E6-4523-93EF-C309E000C746";
constexpr std::string_view kLogicalSectorSize = "8141BF1D-A96F-4709-BA47-F233A8FAAB5F";
constexpr std::string_view kPhysicalSectorSize = "CDA348C7-445D-4471-9CC9-E9885251C556";
constexpr std::string_view kParentLocator = "A8D35F2D-B30B-454D-ABF7-D3D84834AB0C";

constexpr std::uint64_t kBlockNotPresent = 0;
constexpr std::uint64_t kBlockZero = 2;
constexpr std::uint64_t kBlockUnmapped = 3;
constexpr std::uint64_t kBlockFullyPresent = 6;
constexpr std::uint64_t kBlockPartiallyPresent = 7;

void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get_u32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    return get_u32(p) | (static_cast<std::uint64_t>(get_u32(p + 4)) << 32);
}
void put_guid(std::uint8_t* p, std::string_view text) {
    const Guid g = parse_guid(text);
    std::memcpy(p, g.data(), g.size());
}
void put_guid(std::uint8_t* p, const Guid& g) {
    std::memcpy(p, g.data(), g.size());
}
bool guid_is(const std::uint8_t* p, std::string_view text) {
    const Guid g = parse_guid(text);
    return std::memcmp(p, g.data(), g.size()) == 0;
}

bool all_zero(const std::uint8_t* p, std::size_t n) {
    // Word at a time; the tail byte by byte.
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w != 0) return false;
    }
    for (; i < n; ++i)
        if (p[i] != 0) return false;
    return true;
}

std::uint64_t round_up(std::uint64_t v, std::uint64_t to) {
    return (v + to - 1) / to * to;
}

std::uint64_t bat_entries(std::uint64_t payload_blocks, std::uint64_t chunk_ratio) {
    return payload_blocks == 0 ? 0 : payload_blocks + (payload_blocks - 1) / chunk_ratio;
}

}  // namespace

void MemoryFile::write_at(std::uint64_t offset, ByteSpan data) {
    if (offset + data.size() > bytes.size()) bytes.resize(static_cast<std::size_t>(offset + data.size()));
    std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void MemoryFile::read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset) throw Error("read past the end of the file");
    std::memcpy(dst, bytes.data() + offset, length);
}

VhdxWriter::VhdxWriter(RandomFile& file, std::uint64_t virtual_size, std::uint32_t logical_sector,
                       std::uint32_t physical_sector, std::uint32_t block_size)
    : file_(file), size_(virtual_size), logical_(logical_sector), physical_(physical_sector), block_(block_size) {
    if (logical_ != 512 && logical_ != 4096) throw Error("a virtual disk needs 512 or 4096-byte sectors");
    if (physical_ != 512 && physical_ != 4096) throw Error("a virtual disk needs 512 or 4096-byte physical sectors");
    if (block_ < kMiB || block_ > 256 * kMiB || (block_ & (block_ - 1)) != 0) throw Error("bad virtual disk block size");
    if (size_ == 0 || size_ % logical_ != 0 || size_ > (64ull << 40))
        throw Error("a virtual disk must be a whole number of sectors, at most 64 TB");
    chunk_ratio_ = ((1ull << 23) * logical_) / block_;
    blocks_.assign(static_cast<std::size_t>((size_ + block_ - 1) / block_), 0);
    bat_offset_ = kBatOffset;
    bat_length_ = round_up(std::max<std::uint64_t>(bat_entries(blocks_.size(), chunk_ratio_) * 8, 1), kMiB);
    end_ = round_up(bat_offset_ + bat_length_, kMiB);
    file_.resize(end_);
}

void VhdxWriter::write(std::uint64_t offset, ByteSpan data) {
    if (finished_) throw Error("virtual disk already finished");
    if (offset > size_ || data.size() > size_ - offset) throw Error("write past the end of the virtual disk");
    std::size_t done = 0;
    while (done < data.size()) {
        const std::uint64_t pos = offset + done;
        const std::uint64_t b = pos / block_;
        const std::uint64_t within = pos % block_;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(block_ - within, data.size() - done));
        const std::uint8_t* p = data.data() + done;
        std::uint64_t& at = blocks_[static_cast<std::size_t>(b)];
        if (at == 0) {
            if (all_zero(p, n)) {
                done += n;
                continue;  // still zeros: no need to store the block
            }
            at = end_;
            end_ += block_;
            file_.resize(end_);
            ++allocated_;
        }
        file_.write_at(at + within, ByteSpan(p, n));
        done += n;
    }
}

void VhdxWriter::finish() {
    if (finished_) return;
    // Block allocation table: payload entries with a sector bitmap entry after every chunk_ratio
    // of them (not present; only differencing disks use those).
    Bytes bat(static_cast<std::size_t>(bat_length_), 0);
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const std::uint64_t index = i + i / chunk_ratio_;
        const std::uint64_t entry = blocks_[i] == 0 ? kBlockNotPresent : (kBlockFullyPresent | ((blocks_[i] / kMiB) << 20));
        put_u64(&bat[static_cast<std::size_t>(index * 8)], entry);
    }
    file_.write_at(bat_offset_, bat);

    // Metadata region: the table, then the items from 64 KiB on.
    Bytes meta(kMetadataLength, 0);
    std::memcpy(meta.data(), "metadata", 8);
    put_u16(&meta[10], 5);
    struct Item {
        std::string_view id;
        std::uint32_t length;
        std::uint32_t flags;  // bit 0 user, bit 1 virtual disk, bit 2 required
    };
    const Item items[] = {{kFileParameters, 8, 4},
                          {kVirtualDiskSize, 8, 6},
                          {kVirtualDiskId, 16, 6},
                          {kLogicalSectorSize, 4, 6},
                          {kPhysicalSectorSize, 4, 6}};
    std::uint32_t data_at = 64 * 1024;
    for (std::size_t i = 0; i < 5; ++i) {
        std::uint8_t* e = &meta[32 + i * 32];
        put_guid(e, items[i].id);
        put_u32(e + 16, data_at);
        put_u32(e + 20, items[i].length);
        put_u32(e + 24, items[i].flags);
        std::uint8_t* d = &meta[data_at];
        switch (i) {
            case 0:
                put_u32(d, block_);
                put_u32(d + 4, 0);  // blocks need not stay allocated, no parent
                break;
            case 1: put_u64(d, size_); break;
            case 2: put_guid(d, random_guid()); break;
            case 3: put_u32(d, logical_); break;
            default: put_u32(d, physical_); break;
        }
        data_at += 4096;
    }
    file_.write_at(kMetadataOffset, meta);

    // An empty log region (the zero log GUID in the headers says there is nothing to replay).
    file_.write_at(kLogOffset, Bytes(kLogLength, 0));

    // Region tables (two identical copies).
    Bytes region(64 * 1024, 0);
    put_u32(&region[0], 0x69676572);  // "regi"
    put_u32(&region[8], 2);
    put_guid(&region[16], kBatRegion);
    put_u64(&region[32], bat_offset_);
    put_u32(&region[40], static_cast<std::uint32_t>(bat_length_));
    put_u32(&region[44], 1);
    put_guid(&region[48], kMetadataRegion);
    put_u64(&region[64], kMetadataOffset);
    put_u32(&region[72], static_cast<std::uint32_t>(kMetadataLength));
    put_u32(&region[76], 1);
    put_u32(&region[4], crc32c(region));
    file_.write_at(kRegion1, region);
    file_.write_at(kRegion2, region);

    // Headers: two valid ones, the second current.
    const Guid file_write = random_guid();
    const Guid data_write = random_guid();
    for (int h = 0; h < 2; ++h) {
        Bytes head(4096, 0);
        put_u32(&head[0], 0x64616568);  // "head"
        put_u64(&head[8], static_cast<std::uint64_t>(h + 1));
        put_guid(&head[16], file_write);
        put_guid(&head[32], data_write);
        // Log GUID (48..63) stays zero: no log to replay.
        put_u16(&head[64], 0);  // log version
        put_u16(&head[66], 1);  // version
        put_u32(&head[68], static_cast<std::uint32_t>(kLogLength));
        put_u64(&head[72], kLogOffset);
        put_u32(&head[4], crc32c(head));
        file_.write_at(h == 0 ? kHeader1 : kHeader2, head);
    }

    // File type identifier, written last: until then the file is not taken for a VHDX.
    Bytes ident(64 * 1024, 0);
    std::memcpy(ident.data(), "vhdxfile", 8);
    const char creator[] = "Vaultik";
    for (std::size_t i = 0; creator[i] != '\0'; ++i) put_u16(&ident[8 + 2 * i], static_cast<std::uint16_t>(creator[i]));
    file_.write_at(0, ident);
    file_.resize(end_);
    file_.flush();
    finished_ = true;
}

VhdxReader::VhdxReader(RandomFile& file) : file_(file) {
    std::uint8_t sig[8];
    file_.read_at(0, sig, 8);
    if (std::memcmp(sig, "vhdxfile", 8) != 0) throw Error("not a VHDX file");
    // The current header: the valid one with the higher sequence number.
    Bytes best;
    std::uint64_t best_seq = 0;
    for (const std::uint64_t at : {kHeader1, kHeader2}) {
        Bytes h(4096);
        file_.read_at(at, h.data(), h.size());
        if (get_u32(&h[0]) != 0x64616568) continue;
        const std::uint32_t sum = get_u32(&h[4]);
        put_u32(&h[4], 0);
        if (crc32c(h) != sum) continue;
        const std::uint64_t seq = get_u64(&h[8]);
        if (best.empty() || seq > best_seq) {
            best = h;
            best_seq = seq;
        }
    }
    if (best.empty()) throw Error("the VHDX file has no valid header");
    for (int i = 48; i < 64; ++i)
        if (best[static_cast<std::size_t>(i)] != 0) throw Error("the VHDX file has a log to replay; attach it in Windows once first");

    Bytes region(64 * 1024);
    file_.read_at(kRegion1, region.data(), region.size());
    const std::uint32_t sum = get_u32(&region[4]);
    put_u32(&region[4], 0);
    if (get_u32(&region[0]) != 0x69676572 || crc32c(region) != sum) throw Error("the VHDX region table is damaged");
    std::uint64_t bat_off = 0, bat_len = 0, meta_off = 0, meta_len = 0;
    const std::uint32_t count = get_u32(&region[8]);
    for (std::uint32_t i = 0; i < count && i < 2047; ++i) {
        const std::uint8_t* e = &region[16 + i * 32];
        if (guid_is(e, kBatRegion)) {
            bat_off = get_u64(e + 16);
            bat_len = get_u32(e + 24);
        } else if (guid_is(e, kMetadataRegion)) {
            meta_off = get_u64(e + 16);
            meta_len = get_u32(e + 24);
        } else if (get_u32(e + 28) & 1) {
            throw Error("the VHDX file needs a feature Vaultik does not know");
        }
    }
    if (bat_len == 0 || meta_len == 0) throw Error("the VHDX file has no block table or metadata");

    Bytes meta(static_cast<std::size_t>(meta_len));
    file_.read_at(meta_off, meta.data(), meta.size());
    if (std::memcmp(meta.data(), "metadata", 8) != 0) throw Error("the VHDX metadata is damaged");
    const std::uint16_t items = static_cast<std::uint16_t>(meta[10] | (meta[11] << 8));
    for (std::uint16_t i = 0; i < items && 32 + (i + 1) * 32u <= meta.size(); ++i) {
        const std::uint8_t* e = &meta[32 + i * 32u];
        const std::uint32_t off = get_u32(e + 16);
        const std::uint32_t len = get_u32(e + 20);
        if (off > meta.size() || len > meta.size() - off) throw Error("the VHDX metadata is damaged");
        const std::uint8_t* d = &meta[off];
        if (guid_is(e, kFileParameters) && len >= 8) {
            block_ = get_u32(d);
            if (get_u32(d + 4) & 2) throw Error("differencing VHDX files are not supported");
        } else if (guid_is(e, kVirtualDiskSize) && len >= 8) {
            size_ = get_u64(d);
        } else if (guid_is(e, kLogicalSectorSize) && len >= 4) {
            logical_ = get_u32(d);
        } else if (guid_is(e, kParentLocator)) {
            throw Error("differencing VHDX files are not supported");
        }
    }
    if (block_ < kMiB || (block_ & (block_ - 1)) != 0 || size_ == 0 || (logical_ != 512 && logical_ != 4096))
        throw Error("the VHDX metadata is damaged");
    chunk_ratio_ = ((1ull << 23) * logical_) / block_;
    const std::uint64_t entries = bat_entries((size_ + block_ - 1) / block_, chunk_ratio_);
    if (entries * 8 > bat_len) throw Error("the VHDX block table is too short");
    Bytes raw(static_cast<std::size_t>(entries * 8));
    file_.read_at(bat_off, raw.data(), raw.size());
    bat_.resize(static_cast<std::size_t>(entries));
    for (std::size_t i = 0; i < bat_.size(); ++i) bat_[i] = get_u64(&raw[i * 8]);
}

void VhdxReader::read(std::uint64_t offset, std::uint8_t* dst, std::size_t length) {
    if (offset > size_ || length > size_ - offset) throw Error("read past the end of the virtual disk");
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t pos = offset + done;
        const std::uint64_t b = pos / block_;
        const std::uint64_t within = pos % block_;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(block_ - within, length - done));
        const std::uint64_t entry = bat_[static_cast<std::size_t>(b + b / chunk_ratio_)];
        const std::uint64_t state = entry & 7;
        if (state == kBlockFullyPresent) {
            file_.read_at(((entry >> 20) * kMiB) + within, dst + done, n);
        } else if (state == kBlockNotPresent || state == kBlockZero || state == kBlockUnmapped) {
            std::memset(dst + done, 0, n);
        } else if (state == kBlockPartiallyPresent) {
            throw Error("differencing VHDX files are not supported");
        } else {
            throw Error("the VHDX block table is damaged");
        }
        done += n;
    }
}

}  // namespace vaultik::image
