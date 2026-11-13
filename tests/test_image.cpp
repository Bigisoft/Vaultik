#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

#include "vaultik/image/disk.hpp"
#include "vaultik/image/extents.hpp"
#include "vaultik/image/image.hpp"
#include "vaultik/image/layout.hpp"
#include "vaultik/image/ntfs.hpp"
#include "vaultik/image/recovery_media.hpp"
#include "vaultik/image/vhdx.hpp"
#include "vaultik/compress/zstd.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
using namespace vaultik::image;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "image test password";
constexpr std::uint64_t kMiB = 1u << 20;

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-image-test-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::unique_ptr<Repository> make_repo(const fs::path& dir) {
    LocalBackend backend(u8(dir), true);
    InitOptions options;
    options.kdf_iterations = 1000;
    Repository::init(backend, kPassword, options);
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

void fill_random(std::uint8_t* p, std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(rng());
}

// A disk in memory, read like a real one (sector-aligned reads only).
struct MemDisk : DiskSource {
    DiskLayout l;
    Bytes bytes;
    std::map<std::size_t, Bytes> bitmaps;  // per Used partition
    std::map<std::size_t, Ranges> keep;    // per Used partition: unused ranges a restore writes
    std::uint32_t cluster = 4096;

    DiskLayout& layout() override { return l; }
    std::vector<Extent> plan(std::size_t i) override {
        const Partition& p = l.partitions[i];
        if (p.method == ReadMethod::Used)
            return plan_extents(bitmaps[i], p.length / cluster, cluster, p.length, {}, kMinHole, keep[i]);
        return {{0, p.length, Extent::Kind::Data}};
    }
    void read(std::size_t i, std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        REQUIRE(offset % 512 == 0);
        REQUIRE(length % 512 == 0);
        const Partition& p = l.partitions[i];
        REQUIRE(offset + length <= p.length);
        std::memcpy(dst, bytes.data() + p.offset + offset, length);
    }
    void read_disk(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        REQUIRE(offset % 512 == 0);
        REQUIRE(length % 512 == 0);
        std::memcpy(dst, bytes.data() + offset, length);
    }
};

// A disk target that is not empty and checks every write is sector-aligned.
struct MemTarget : DiskTarget {
    Bytes bytes;
    bool finished = false;
    explicit MemTarget(std::uint64_t size, std::uint8_t fill) : bytes(static_cast<std::size_t>(size), fill) {}
    std::uint64_t size() const override { return bytes.size(); }
    std::uint32_t sector_size() const override { return 512; }
    bool zeroed() const override { return false; }
    std::uint32_t alignment() const override { return 512; }
    void write(std::uint64_t offset, ByteSpan data) override {
        REQUIRE(offset % 512 == 0);
        REQUIRE(data.size() % 512 == 0);
        REQUIRE(offset + data.size() <= bytes.size());
        std::memcpy(bytes.data() + offset, data.data(), data.size());
    }
    void finish() override { finished = true; }
};

// A disk target that notes, at every write into a partition, whether the disk showed a
// partition table then (an MBR entry, or a GPT header in the primary or backup place).
struct OrderTarget : MemTarget {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> parts;  // [begin, end) of the partitions
    std::uint64_t original_size = 0;
    int partition_writes = 0;
    int with_tables = 0;
    OrderTarget(std::uint64_t size, std::uint8_t fill) : MemTarget(size, fill) {}
    bool shows_tables() const {
        if (std::any_of(bytes.begin() + 446, bytes.begin() + 510, [](std::uint8_t b) { return b != 0; })) return true;
        if (std::memcmp(bytes.data() + 512, "EFI PART", 8) == 0) return true;
        return std::memcmp(bytes.data() + original_size - 512, "EFI PART", 8) == 0;
    }
    void write(std::uint64_t offset, ByteSpan data) override {
        for (const auto& [b, e] : parts)
            if (offset < e && offset + data.size() > b) {
                ++partition_writes;
                if (shows_tables()) ++with_tables;
                break;
            }
        MemTarget::write(offset, data);
    }
};

// A 64 MiB GPT disk: an EFI partition (read raw) and an "NTFS" partition read by its bitmap,
// with a 20 MiB unused run (a hole) and scattered unused clusters full of old data.
MemDisk make_gpt_disk() {
    MemDisk d;
    d.l.style = PartitionStyle::Gpt;
    d.l.size = 64 * kMiB;
    d.l.disk_guid = "11111111-2222-3333-4444-555555555555";
    d.l.model = "Test disk";
    Partition efi;
    efi.number = 1;
    efi.offset = 1 * kMiB;
    efi.length = 8 * kMiB;
    efi.type = std::string(kEfiSystemType);
    efi.guid = "AAAAAAAA-0000-0000-0000-000000000001";
    efi.name = "EFI system partition";
    efi.filesystem = "FAT32";
    Partition data;
    data.number = 2;
    data.offset = 9 * kMiB;
    data.length = 52 * kMiB;
    data.type = std::string(kBasicDataType);
    data.guid = "AAAAAAAA-0000-0000-0000-000000000002";
    data.name = "Basic data partition";
    data.filesystem = "NTFS";
    data.mount = "C:\\";
    data.method = ReadMethod::Used;
    d.l.partitions = {efi, data};
    d.l.id = disk_id(d.l);
    d.bytes.assign(static_cast<std::size_t>(d.l.size), 0);
    fill_random(d.bytes.data() + efi.offset, efi.length, 1);
    fill_random(d.bytes.data() + data.offset, data.length, 2);
    // Some zero runs inside the EFI partition (raw: must come back exactly).
    std::memset(d.bytes.data() + efi.offset + 4096, 0, 64 * 1024);
    // Bitmap of the data partition: everything used except every 7th cluster and clusters for
    // 20 MiB from 24 MiB on.
    const std::uint64_t clusters = data.length / d.cluster;
    Bytes bm(static_cast<std::size_t>((clusters + 7) / 8), 0);
    for (std::uint64_t c = 0; c < clusters; ++c) {
        const std::uint64_t at = c * d.cluster;
        const bool used = c % 7 != 3 && !(at >= 24 * kMiB && at < 44 * kMiB);
        if (used) bm[static_cast<std::size_t>(c / 8)] |= static_cast<std::uint8_t>(1u << (c % 8));
    }
    d.bitmaps[1] = bm;
    for (const DiskWrite& w : partition_tables(d.l, d.l.size))
        std::memcpy(d.bytes.data() + w.offset, w.data.data(), w.data.size());
    return d;
}

// What a restore must give back: the disk with the unused clusters of Used partitions zeroed.
Bytes expected_image(MemDisk& d) {
    Bytes e = d.bytes;
    for (std::size_t i = 0; i < d.l.partitions.size(); ++i) {
        const Partition& p = d.l.partitions[i];
        if (p.method != ReadMethod::Used) continue;
        for (const Extent& x : d.plan(i))
            if (x.kind != Extent::Kind::Data) std::memset(e.data() + p.offset + x.offset, 0, static_cast<std::size_t>(x.length));
    }
    return e;
}

Bytes read_vhdx(MemoryFile& f) {
    VhdxReader r(f);
    Bytes out(static_cast<std::size_t>(r.virtual_size()));
    r.read(0, out.data(), out.size());
    return out;
}

}  // namespace

TEST_CASE("image: GUIDs, CRC-32 and CRC-32C") {
    const Guid efi = parse_guid(kEfiSystemType);
    const std::uint8_t want[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
    CHECK(std::memcmp(efi.data(), want, 16) == 0);
    CHECK(format_guid(efi) == std::string(kEfiSystemType));
    CHECK(format_guid(parse_guid("{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}")) == std::string(kEfiSystemType));
    CHECK_THROWS_AS(parse_guid("C12A7328F81F11D2BA4B00A0C93EC93B"), Error);
    CHECK_THROWS_AS(parse_guid("G12A7328-F81F-11D2-BA4B-00A0C93EC93B"), Error);
    const Guid r = random_guid();
    CHECK(format_guid(parse_guid(format_guid(r))) == format_guid(r));
    CHECK((format_guid(r)[14] == '4'));  // version 4

    CHECK(crc32(as_bytes("123456789")) == 0xCBF43926u);
    CHECK(crc32c(as_bytes("123456789")) == 0xE3069283u);
}

TEST_CASE("image: the disk layout survives JSON and rejects nonsense") {
    MemDisk d = make_gpt_disk();
    d.l.partitions[1].holes = {{24 * kMiB, 20 * kMiB}};
    d.l.partitions[1].used_bytes = 12345;
    d.l.partitions[1].serial = 0xDEADBEEF;
    d.l.regions = gap_regions(d.l);
    const DiskLayout back = layout_from_json(to_json(d.l));
    CHECK(back.style == PartitionStyle::Gpt);
    CHECK(back.size == d.l.size);
    CHECK(back.disk_guid == d.l.disk_guid);
    REQUIRE(back.partitions.size() == 2);
    CHECK(back.partitions[0].type == std::string(kEfiSystemType));
    CHECK(back.partitions[0].method == ReadMethod::Raw);
    CHECK(back.partitions[1].method == ReadMethod::Used);
    CHECK(back.partitions[1].holes == d.l.partitions[1].holes);
    CHECK(back.partitions[1].serial == 0xDEADBEEF);
    CHECK(back.partitions[1].mount == "C:\\");
    CHECK(back.regions.size() == d.l.regions.size());
    CHECK(to_json(back) == to_json(d.l));

    std::string bad = to_json(d.l);
    CHECK_THROWS_AS(layout_from_json("{}"), Error);
    CHECK_THROWS_AS(layout_from_json("not json"), Error);
    DiskLayout newer = d.l;
    newer.version = 2;
    CHECK_THROWS_AS(layout_from_json(to_json(newer)), Error);
    DiskLayout outside = d.l;
    outside.partitions[1].length = 60 * kMiB;  // past the end of the disk
    CHECK_THROWS_AS(layout_from_json(to_json(outside)), Error);
    DiskLayout unaligned = d.l;
    unaligned.partitions[1].offset += 100;
    CHECK_THROWS_AS(layout_from_json(to_json(unaligned)), Error);
}

TEST_CASE("image: gaps between partitions are stored, up to 1 MiB each") {
    DiskLayout l;
    l.style = PartitionStyle::Mbr;
    l.size = 100 * kMiB;
    Partition a;
    a.offset = 2 * kMiB;
    a.length = 10 * kMiB;
    a.mbr_type = 0x07;
    Partition ext;  // a container: its space is not "used"
    ext.offset = 20 * kMiB;
    ext.length = 50 * kMiB;
    ext.mbr_type = 0x0F;
    Partition logical;
    logical.offset = 21 * kMiB;
    logical.length = 49 * kMiB;
    logical.mbr_type = 0x07;
    l.partitions = {a, ext, logical};
    std::vector<Region> g = gap_regions(l);
    REQUIRE(g.size() == 4);
    CHECK(g[0].offset == 0);
    CHECK(g[0].length == kMiB);  // capped
    CHECK(g[1].offset == 12 * kMiB);  // the start of the gap before the extended partition
    CHECK(g[1].length == kMiB);
    CHECK(g[2].offset == 20 * kMiB);  // the EBR at the start of the extended partition, 8 MiB further
    CHECK(g[2].length == kMiB);
    CHECK(g[3].offset == 70 * kMiB);
    CHECK(g[3].length == kMiB);

    // A second logical partition: its EBR sits where the link container starts, after free space
    // (a deleted logical partition); a gap shorter than 1 MiB is stored once, without overlaps.
    logical.length = 9 * kMiB;  // 21 to 30 MiB
    Partition link;
    link.offset = 40 * kMiB;
    link.length = 30 * kMiB;
    link.mbr_type = 0x05;
    Partition logical2;
    logical2.offset = 40 * kMiB + 512;  // right after its EBR
    logical2.length = 30 * kMiB - 512;
    logical2.mbr_type = 0x07;
    l.partitions = {a, ext, logical, link, logical2};
    g = gap_regions(l);
    REQUIRE(g.size() == 6);
    CHECK(g[2].offset == 20 * kMiB);
    CHECK(g[3].offset == 30 * kMiB);  // the start of the free space
    CHECK(g[3].length == kMiB);
    CHECK(g[4].offset == 40 * kMiB);  // the second EBR
    CHECK(g[4].length == 512);
    CHECK(g[5].offset == 70 * kMiB);
    for (std::size_t i = 1; i < g.size(); ++i) CHECK(g[i - 1].offset + g[i - 1].length <= g[i].offset);

    // GPT disks have no containers: only the gap starts.
    l.style = PartitionStyle::Gpt;
    l.partitions = {a, logical};
    CHECK(gap_regions(l).size() == 3);
}

TEST_CASE("image: GPT tables are valid and sized for the target") {
    MemDisk d = make_gpt_disk();
    const std::uint64_t target = 128 * kMiB;  // a larger disk
    const std::vector<DiskWrite> w = partition_tables(d.l, target);
    REQUIRE(w.size() == 2);
    const Bytes& head = w[0].data;
    CHECK(w[0].offset == 0);
    CHECK(head[510] == 0x55);
    CHECK(head[511] == 0xAA);
    CHECK(head[446 + 4] == 0xEE);
    CHECK(std::memcmp(&head[512], "EFI PART", 8) == 0);
    // Header CRC.
    Bytes hdr(head.begin() + 512, head.begin() + 512 + 92);
    const std::uint32_t sum = hdr[16] | (hdr[17] << 8) | (hdr[18] << 16) | (static_cast<std::uint32_t>(hdr[19]) << 24);
    std::memset(&hdr[16], 0, 4);
    CHECK(crc32(hdr) == sum);
    // The alternate header is in the target's last sector.
    std::uint64_t alt = 0;
    for (int i = 0; i < 8; ++i) alt |= static_cast<std::uint64_t>(head[512 + 32 + i]) << (8 * i);
    CHECK(alt == target / 512 - 1);
    CHECK(w[1].offset + w[1].data.size() == target);
    CHECK(std::memcmp(&w[1].data[w[1].data.size() - 512], "EFI PART", 8) == 0);
    // First entry: the EFI partition, at LBA 2048.
    const std::uint8_t* e = &head[1024];
    const Guid type = parse_guid(kEfiSystemType);
    CHECK(std::memcmp(e, type.data(), 16) == 0);
    std::uint64_t first = 0;
    for (int i = 0; i < 8; ++i) first |= static_cast<std::uint64_t>(e[32 + i]) << (8 * i);
    CHECK(first == 2048);
    CHECK(e[56] == 'E');  // name, UTF-16

    CHECK(fits(d.l, 64 * kMiB, 512).empty());
    CHECK_FALSE(fits(d.l, 61 * kMiB, 512).empty());  // the backup GPT no longer fits
    CHECK_FALSE(fits(d.l, 128 * kMiB, 4096).empty());  // other sector size
    CHECK_THROWS_AS(partition_tables(d.l, 61 * kMiB), Error);
    CHECK(partition_tables(DiskLayout{.style = PartitionStyle::Mbr, .size = 10 * kMiB}, 10 * kMiB).empty());
}

TEST_CASE("image: extents from a volume bitmap") {
    // 16 clusters of 4 KiB: used 0-3, free 4-5, used 6, free 7-15.
    Bytes bm = {0x4F, 0x00};
    std::vector<Extent> x = plan_extents(bm, 16, 4096, 16 * 4096 + 512, {}, 8 * 4096);
    REQUIRE(x.size() == 5);
    CHECK(x[0] == Extent{0, 4 * 4096, Extent::Kind::Data});
    CHECK(x[1] == Extent{4 * 4096, 2 * 4096, Extent::Kind::Zero});
    CHECK(x[2] == Extent{6 * 4096, 4096, Extent::Kind::Data});
    CHECK(x[3] == Extent{7 * 4096, 9 * 4096, Extent::Kind::Hole});
    CHECK(x[4] == Extent{16 * 4096, 512, Extent::Kind::Data});  // past the clusters: read
    CHECK(data_bytes(x) == 5 * 4096 + 512);
    CHECK(holes_of(x) == Ranges{{7 * 4096, 9 * 4096}});

    // A page file in clusters 1-2 (and a range reaching past the clusters) counts as unused.
    x = plan_extents(bm, 16, 4096, 16 * 4096, {{4096, 2 * 4096}, {15 * 4096, 10 * 4096}}, 1u << 30);
    REQUIRE(x.size() == 6);
    CHECK(x[0] == Extent{0, 4096, Extent::Kind::Data});
    CHECK(x[1] == Extent{4096, 2 * 4096, Extent::Kind::Zero});
    CHECK(x[2] == Extent{3 * 4096, 4096, Extent::Kind::Data});
    CHECK(x[3] == Extent{4 * 4096, 2 * 4096, Extent::Kind::Zero});
    CHECK(x[4] == Extent{6 * 4096, 4096, Extent::Kind::Data});
    CHECK(x[5] == Extent{7 * 4096, 9 * 4096, Extent::Kind::Zero});  // free clusters and the cut one merged

    // A hibernation file over clusters 8-15 (in use): its first cluster is written by a restore
    // (zeros, never a hole), the rest is a hole. A range to write inside data changes nothing.
    x = plan_extents(Bytes{0xFF, 0xFF}, 16, 4096, 16 * 4096, {{8 * 4096, 8 * 4096}}, 4 * 4096,
                     {{8 * 4096, 100}, {4096, 4096}});
    REQUIRE(x.size() == 3);
    CHECK(x[0] == Extent{0, 8 * 4096, Extent::Kind::Data});
    CHECK(x[1] == Extent{8 * 4096, 4096, Extent::Kind::Zero});
    CHECK(x[2] == Extent{9 * 4096, 7 * 4096, Extent::Kind::Hole});
    CHECK(holes_of(x) == Ranges{{9 * 4096, 7 * 4096}});
    // In the middle of a free run: hole, zeros, hole.
    x = plan_extents(Bytes{0x00, 0x00}, 16, 4096, 16 * 4096, {}, 4 * 4096, {{6 * 4096, 2 * 4096}});
    REQUIRE(x.size() == 3);
    CHECK(x[0] == Extent{0, 6 * 4096, Extent::Kind::Hole});
    CHECK(x[1] == Extent{6 * 4096, 2 * 4096, Extent::Kind::Zero});
    CHECK(x[2] == Extent{8 * 4096, 8 * 4096, Extent::Kind::Hole});

    // A bitmap shorter than the clusters: the rest is read.
    x = plan_extents(Bytes{0xFF}, 16, 4096, 16 * 4096);
    REQUIRE(x.size() == 1);
    CHECK(x[0] == Extent{0, 16 * 4096, Extent::Kind::Data});

    // Whole-byte runs of a large bitmap.
    Bytes big(1024, 0xFF);
    std::fill(big.begin() + 100, big.begin() + 900, 0x00);
    x = plan_extents(big, 8192, 4096, 8192ull * 4096);
    REQUIRE(x.size() == 3);
    CHECK(x[1] == Extent{800ull * 4096, 6400ull * 4096, Extent::Kind::Hole});

    // Clipping to a segment.
    const std::vector<Extent> c = clip(x, 700ull * 4096, 900ull * 4096);
    REQUIRE(c.size() == 2);
    CHECK(c[0] == Extent{700ull * 4096, 100ull * 4096, Extent::Kind::Data});
    CHECK(c[1] == Extent{800ull * 4096, 100ull * 4096, Extent::Kind::Hole});

    const Ranges holes = {{100, 50}, {1000, 500}};
    CHECK(inside(holes, 100, 50));
    CHECK(inside(holes, 1200, 300));
    CHECK_FALSE(inside(holes, 1200, 301));
    CHECK_FALSE(inside(holes, 99, 10));
    CHECK_FALSE(inside(holes, 0, 10));
    CHECK(zero_chunk_sizes(10, 4) == std::vector<std::uint64_t>{4, 4, 2});
    CHECK(zero_chunk_sizes(8, 4) == std::vector<std::uint64_t>{4, 4});
}

TEST_CASE("image: VHDX files written and read back") {
    MemoryFile f;
    {
        VhdxWriter w(f, 100 * kMiB + 512, 512, 4096, 1u << 20);
        Bytes data(3 * kMiB);
        fill_random(data.data(), data.size(), 7);
        w.write(5 * kMiB + 100, data);              // across block boundaries, unaligned
        w.write(50 * kMiB, Bytes(4 * kMiB, 0));     // zeros: no block
        w.write(100 * kMiB, Bytes(512, 0xAB));      // the last, partial block
        CHECK(w.allocated_blocks() == 5);
        CHECK_THROWS_AS(w.write(100 * kMiB, Bytes(1024, 1)), Error);  // past the end
        w.finish();
    }
    // Blocks of 1 MiB: the file is the metadata plus the five blocks.
    CHECK(f.bytes.size() == 4 * kMiB + 5 * kMiB);
    VhdxReader r(f);
    CHECK(r.virtual_size() == 100 * kMiB + 512);
    CHECK(r.logical_sector() == 512);
    Bytes back(3 * kMiB);
    r.read(5 * kMiB + 100, back.data(), back.size());
    Bytes data(3 * kMiB);
    fill_random(data.data(), data.size(), 7);
    CHECK(back == data);
    Bytes zeros(kMiB);
    r.read(60 * kMiB, zeros.data(), zeros.size());
    CHECK(std::all_of(zeros.begin(), zeros.end(), [](std::uint8_t b) { return b == 0; }));
    Bytes last(512);
    r.read(100 * kMiB, last.data(), last.size());
    CHECK(last == Bytes(512, 0xAB));

    // Damage is noticed.
    MemoryFile broken = f;
    broken.bytes[64 * 1024 + 100] ^= 1;   // header 1
    broken.bytes[128 * 1024 + 100] ^= 1;  // header 2
    CHECK_THROWS_AS(VhdxReader{broken}, Error);
    MemoryFile not_vhdx;
    not_vhdx.bytes.assign(kMiB, 0);
    CHECK_THROWS_AS(VhdxReader{not_vhdx}, Error);
    CHECK_THROWS_AS(VhdxWriter(not_vhdx, 1000, 512), Error);  // not whole sectors
}

TEST_CASE("image: back up a disk and restore it to a VHDX and onto a disk") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    MemDisk disk = make_gpt_disk();
    const Bytes expected = expected_image(disk);

    ImageBackupOptions o;
    o.disks = {&disk};
    o.tags = {"job:image"};
    std::uint64_t last_done = 0;
    o.on_progress = [&](const ImageProgress& p) { last_done = p.done; };
    const ImageBackupResult first = run_image_backup(*repo, o);
    CHECK(first.snapshot.image);
    CHECK(first.snapshot.paths == std::vector<std::string>{"image:gpt-11111111-2222-3333-4444-555555555555"});
    CHECK(last_done == 60 * kMiB + 2 * kMiB);  // partitions + the two 1 MiB gaps
    // Less than the disk: the 20 MiB hole is one 4 MiB zero chunk used five times. New data is the
    // EFI partition (8 MiB), the data around the hole (32 MiB), the gaps (2 MiB) and that chunk.
    CHECK(first.snapshot.stats.new_bytes < 47 * kMiB);

    const Snapshot snap = repo->load_snapshot(first.snapshot_id);
    CHECK(snap.image);
    const std::vector<DiskLayout> disks = image_disks(*repo, snap);
    REQUIRE(disks.size() == 1);
    CHECK(disks[0].partitions[1].holes == Ranges{{24 * kMiB, 20 * kMiB}});
    CHECK(disks[0].partitions[1].used_bytes > 0);
    CHECK(disks[0].regions.size() == 2);

    SUBCASE("to a VHDX") {
        auto owned = std::make_unique<MemoryFile>();
        MemoryFile* raw = owned.get();
        VhdxTarget target(std::move(owned), 64 * kMiB, 512, 4096);
        run_image_restore(*repo, snap, target, {});
        CHECK(read_vhdx(*raw) == expected);
    }
    SUBCASE("to a larger VHDX with new ids") {
        auto owned = std::make_unique<MemoryFile>();
        MemoryFile* raw = owned.get();
        VhdxTarget target(std::move(owned), 80 * kMiB, 512, 4096);
        ImageRestoreOptions ro;
        ro.new_ids = true;
        run_image_restore(*repo, snap, target, ro);
        const Bytes got = read_vhdx(*raw);
        // The partitions are the same; the GPT has another disk GUID.
        const Partition& p = disk.l.partitions[1];
        CHECK(std::memcmp(got.data() + p.offset, expected.data() + p.offset, static_cast<std::size_t>(p.length)) == 0);
        CHECK(std::memcmp(got.data() + 512 + 56, expected.data() + 512 + 56, 16) != 0);
        CHECK(std::memcmp(got.data() + (80 * kMiB) - 512, "EFI PART", 8) == 0);
    }
    SUBCASE("onto a disk with old data") {
        MemTarget target(64 * kMiB, 0xEE);
        run_image_restore(*repo, snap, target, {});
        CHECK(target.finished);
        // Everything matches but the hole, which keeps whatever was there (unused space).
        Bytes got = target.bytes;
        const Partition& p = disk.l.partitions[1];
        const std::size_t hole = static_cast<std::size_t>(p.offset + 24 * kMiB);
        CHECK(got[hole] == 0xEE);
        std::memset(got.data() + hole, 0, 20 * kMiB);
        // Nor is the free space after the last partition, past its stored first 1 MiB (the old
        // tables in the disk's last 1 MiB are cleared).
        CHECK(got[62 * kMiB] == 0xEE);
        std::memset(got.data() + 62 * kMiB, 0, kMiB);
        CHECK(got == expected);
    }
    SUBCASE("refused when the disk is too small") {
        MemTarget target(32 * kMiB, 0);
        CHECK_THROWS_AS(run_image_restore(*repo, snap, target, {}), Error);
        CHECK_FALSE(target.finished);
    }

    // A second backup after a small change stores little: the rest deduplicates.
    fill_random(disk.bytes.data() + disk.l.partitions[1].offset + 2 * kMiB, 64 * 1024, 99);
    ImageBackupOptions o2 = o;
    o2.parent = first.snapshot_id;
    const ImageBackupResult second = run_image_backup(*repo, o2);
    CHECK(second.snapshot.stats.new_bytes < 6 * kMiB);
    CHECK(second.snapshot.parent == first.snapshot_id);
}

TEST_CASE("image: an MBR disk keeps its boot code; new ids change the signature") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    MemDisk d;
    d.l.style = PartitionStyle::Mbr;
    d.l.size = 24 * kMiB;
    d.l.signature = 0x12345678;
    Partition p;
    p.number = 1;
    p.offset = kMiB;
    p.length = 20 * kMiB;
    p.mbr_type = 0x07;
    p.active = true;
    d.l.partitions = {p};
    d.bytes.assign(static_cast<std::size_t>(d.l.size), 0);
    fill_random(d.bytes.data(), 440, 5);  // boot code
    d.bytes[440] = 0x78;
    d.bytes[441] = 0x56;
    d.bytes[442] = 0x34;
    d.bytes[443] = 0x12;
    d.bytes[510] = 0x55;
    d.bytes[511] = 0xAA;
    fill_random(d.bytes.data() + kMiB, 20 * kMiB, 6);
    ImageBackupOptions o;
    o.disks = {&d};
    const ImageBackupResult r = run_image_backup(*repo, o);
    CHECK(r.snapshot.paths[0] == "image:mbr-12345678");

    auto owned = std::make_unique<MemoryFile>();
    MemoryFile* raw = owned.get();
    VhdxTarget target(std::move(owned), 24 * kMiB, 512, 512);
    run_image_restore(*repo, r.snapshot, target, {});
    CHECK(read_vhdx(*raw) == d.bytes);

    auto owned2 = std::make_unique<MemoryFile>();
    MemoryFile* raw2 = owned2.get();
    VhdxTarget target2(std::move(owned2), 24 * kMiB, 512, 512);
    ImageRestoreOptions ro;
    ro.new_ids = true;
    run_image_restore(*repo, r.snapshot, target2, ro);
    const Bytes got = read_vhdx(*raw2);
    CHECK(std::memcmp(got.data(), d.bytes.data(), 440) == 0);
    CHECK(std::memcmp(got.data() + 440, d.bytes.data() + 440, 4) != 0);
    CHECK(std::memcmp(got.data() + 444, d.bytes.data() + 444, d.bytes.size() - 444) == 0);
}

TEST_CASE("image: the start of a hibernation file is overwritten on a disk with old data") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    MemDisk disk = make_gpt_disk();
    disk.keep[1] = {{24 * kMiB, kMiB}};  // its first 1 MiB, at the start of the hole
    ImageBackupOptions o;
    o.disks = {&disk};
    const ImageBackupResult r = run_image_backup(*repo, o);
    const std::vector<DiskLayout> disks = image_disks(*repo, r.snapshot);
    CHECK(disks[0].partitions[1].holes == Ranges{{25 * kMiB, 19 * kMiB}});
    MemTarget target(64 * kMiB, 0xEE);
    run_image_restore(*repo, r.snapshot, target, {});
    const std::size_t at = static_cast<std::size_t>(disk.l.partitions[1].offset + 24 * kMiB);
    CHECK(std::all_of(target.bytes.begin() + at, target.bytes.begin() + at + kMiB, [](std::uint8_t b) { return b == 0; }));
    CHECK(target.bytes[at + kMiB] == 0xEE);  // the rest of the hole is left alone
}

TEST_CASE("image: a restore writes the partition tables only after every partition") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");

    SUBCASE("GPT") {
        MemDisk disk = make_gpt_disk();
        ImageBackupOptions o;
        o.disks = {&disk};
        const ImageBackupResult r = run_image_backup(*repo, o);
        // Old data everywhere, and old tables of another layout at both ends.
        OrderTarget target(64 * kMiB, 0xEE);
        target.original_size = 64 * kMiB;
        for (const Partition& p : disk.l.partitions) target.parts.emplace_back(p.offset, p.offset + p.length);
        run_image_restore(*repo, r.snapshot, target, {});
        CHECK(target.partition_writes > 0);
        CHECK(target.with_tables == 0);
        CHECK(target.shows_tables());  // in the end
        CHECK(target.finished);
        CHECK(std::memcmp(target.bytes.data(), disk.bytes.data(), 34 * 512) == 0);
        CHECK(std::memcmp(target.bytes.data() + 64 * kMiB - 33 * 512, disk.bytes.data() + 64 * kMiB - 33 * 512, 33 * 512) == 0);
    }
    SUBCASE("MBR") {
        MemDisk d;
        d.l.style = PartitionStyle::Mbr;
        d.l.size = 24 * kMiB;
        d.l.signature = 0x12345678;
        Partition p;
        p.number = 1;
        p.offset = kMiB;
        p.length = 20 * kMiB;
        p.mbr_type = 0x07;
        p.active = true;
        d.l.partitions = {p};
        d.bytes.assign(static_cast<std::size_t>(d.l.size), 0);
        fill_random(d.bytes.data(), 446, 5);  // boot code and signature
        d.bytes[440] = 0x78;
        d.bytes[441] = 0x56;
        d.bytes[442] = 0x34;
        d.bytes[443] = 0x12;
        d.bytes[446] = 0x80;  // the entry
        d.bytes[446 + 4] = 0x07;
        d.bytes[446 + 8] = 0x00;
        d.bytes[446 + 9] = 0x08;  // LBA 2048
        d.bytes[510] = 0x55;
        d.bytes[511] = 0xAA;
        fill_random(d.bytes.data() + kMiB, 20 * kMiB, 6);
        ImageBackupOptions o;
        o.disks = {&d};
        const ImageBackupResult r = run_image_backup(*repo, o);
        OrderTarget target(24 * kMiB, 0xEE);
        target.original_size = 24 * kMiB;
        target.parts.emplace_back(p.offset, p.offset + p.length);
        run_image_restore(*repo, r.snapshot, target, {});
        CHECK(target.partition_writes > 0);
        CHECK(target.with_tables == 0);
        CHECK(std::memcmp(target.bytes.data(), d.bytes.data(), 21 * kMiB) == 0);  // the table is back, and the rest
    }
}

TEST_CASE("image: a layout the partition tables cannot be made for is refused before any write") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    MemDisk d;
    d.l.style = PartitionStyle::Gpt;
    d.l.size = 8 * kMiB;
    d.l.disk_guid = "11111111-2222-3333-4444-555555555555";
    Partition p;
    p.number = 1;
    p.offset = 8 * 512;  // inside the space of the GPT entries: fits() lets it pass
    p.length = 4 * kMiB;
    p.type = std::string(kBasicDataType);
    p.guid = "AAAAAAAA-0000-0000-0000-000000000001";
    d.l.partitions = {p};
    d.bytes.assign(static_cast<std::size_t>(d.l.size), 0);
    fill_random(d.bytes.data() + p.offset, p.length, 3);
    ImageBackupOptions o;
    o.disks = {&d};
    const ImageBackupResult r = run_image_backup(*repo, o);
    MemTarget target(8 * kMiB, 0xEE);
    REQUIRE(fits(d.l, target.size(), 512).empty());
    CHECK_THROWS_AS(run_image_restore(*repo, r.snapshot, target, {}), Error);
    CHECK(std::all_of(target.bytes.begin(), target.bytes.end(), [](std::uint8_t b) { return b == 0xEE; }));
    CHECK_FALSE(target.finished);
}

TEST_CASE("image: the partition tables are cleared from stored bytes") {
    DiskLayout l;
    l.style = PartitionStyle::Gpt;
    l.size = 10 * kMiB;
    Bytes head(kMiB, 0xAB);
    blank_tables(l, 0, head);
    CHECK(head[445] == 0xAB);  // boot code
    CHECK(std::all_of(head.begin() + 446, head.begin() + 510, [](std::uint8_t b) { return b == 0; }));
    CHECK(head[510] == 0xAB);
    CHECK(std::all_of(head.begin() + 512, head.begin() + 34 * 512, [](std::uint8_t b) { return b == 0; }));
    CHECK(head[34 * 512] == 0xAB);
    Bytes tail(kMiB, 0xAB);
    blank_tables(l, 9 * kMiB, tail);
    CHECK(tail[kMiB - 33 * 512 - 1] == 0xAB);
    CHECK(std::all_of(tail.end() - 33 * 512, tail.end(), [](std::uint8_t b) { return b == 0; }));
    Bytes middle(kMiB, 0xAB);
    blank_tables(l, 4 * kMiB, middle);
    CHECK(std::all_of(middle.begin(), middle.end(), [](std::uint8_t b) { return b == 0xAB; }));
    l.style = PartitionStyle::Mbr;
    Bytes mbr(kMiB, 0xAB);
    blank_tables(l, 0, mbr);
    CHECK(mbr[512] == 0xAB);  // only the four entries
    CHECK(mbr[446] == 0);
}

TEST_CASE("image: a file version is not an image") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    Snapshot s;
    CHECK_THROWS_AS(image_disks(*repo, s), Error);
}

TEST_CASE("image: restores never write to the disk Windows runs from") {
    DiskInfo d;
    d.number = 2;
    d.size = 500 * kMiB;
    CHECK(target_refusal(d, {0}, {}).empty());
    CHECK_FALSE(target_refusal(d, {2}, {}).empty());     // Windows runs from it
    CHECK_FALSE(target_refusal(d, {0}, {1, 2}).empty());  // it holds the backup
    DiskInfo sys = d;
    sys.system = true;
    CHECK_FALSE(target_refusal(sys, {}, {}).empty());
    DiskInfo none = d;
    none.number = -1;
    CHECK_FALSE(target_refusal(none, {}, {}).empty());
    DiskInfo empty = d;
    empty.size = 0;
    CHECK_FALSE(target_refusal(empty, {}, {}).empty());

    CHECK(confirmation_text(3) == "ERASE DISK 3");
    CHECK(confirmed("erase disk 3", 3));
    CHECK(confirmed("  ERASE   DISK 3 ", 3));
    CHECK_FALSE(confirmed("ERASE DISK 2", 3));
    CHECK_FALSE(confirmed("ERASE DISK 33", 3));
    CHECK_FALSE(confirmed("yes", 3));
    CHECK_FALSE(confirmed("", 3));

    // The disk confirmed against the disk at that number when the restore opens it.
    DiskInfo chosen = d;
    chosen.serial = "S64ANS0R123456";
    chosen.id = "gpt-AAAA";
    const DiskIdentity want = identity_of(chosen);
    CHECK(identity_mismatch(chosen, want).empty());
    DiskInfo same = chosen;
    same.serial = "  s64ans0r123456 ";  // as some drives pad it
    same.model = "another name";
    CHECK(identity_mismatch(same, want).empty());
    DiskInfo other = chosen;  // another disk got the number
    other.serial = "WD-1234";
    CHECK(identity_mismatch(other, want).find("disk 2") == 0);
    other = chosen;
    other.size += 512;
    CHECK_FALSE(identity_mismatch(other, want).empty());
    other = chosen;
    other.id = "mbr-12345678";  // its partition table changed since it was shown
    CHECK_FALSE(identity_mismatch(other, want).empty());
    other = chosen;
    other.serial.clear();  // a disk without a serial number is not the one with it
    CHECK_FALSE(identity_mismatch(other, want).empty());

    DiskInfo a = d;
    a.number = 0;
    a.id = "gpt-AAAA";
    a.system = true;
    DiskInfo b = d;
    b.number = 4;
    b.id = "mbr-12345678";
    CHECK(find_disk({a, b}, "system") == 0);
    CHECK(find_disk({a, b}, "MBR-12345678") == 4);
    CHECK(find_disk({a, b}, "gpt-BBBB") == -1);
    CHECK(find_disk({a, b}, "") == -1);
    CHECK(describe(b).find("Disk 4") == 0);
}

TEST_CASE("image: recovery drive pieces") {
    CHECK(startnet_script().find("wpeinit") != std::string::npos);
    CHECK(startnet_script().find("vaultik.exe recovery") != std::string::npos);
    CHECK(adk_help().find("Windows PE") != std::string::npos);
    CHECK(pe_imports(as_bytes("not a program"), false).empty());
#ifdef _WIN32
    const Bytes self = os::read_file(os::executable_path(), 512u << 20);
    std::vector<std::string> dlls = pe_imports(self, false);
    for (std::string& s : dlls)
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(std::find(dlls.begin(), dlls.end(), "KERNEL32.DLL") != dlls.end());
    CHECK_FALSE(usb_refusal("not a drive", {}).empty());
#endif
}

// ---- the disk image test's virtual disks ---------------------------------------------------
// The elevated disk image test (a script outside the test suite) needs a source disk with an EFI
// partition and an NTFS volume. Windows will not format virtual disks on every computer (a disk
// filter driver can refuse), so the disks are written here instead: the NTFS volume comes from
// tests/data/ntfs-1746m.vkfix, made once with mkntfs (tools/make_ntfs_fixture.sh).

namespace {

constexpr std::uint64_t kTestDiskSize = 2048 * kMiB;
constexpr std::uint64_t kTestEfiOffset = 1 * kMiB;
constexpr std::uint64_t kTestEfiLength = 300 * kMiB;
constexpr std::uint64_t kTestNtfsOffset = 301 * kMiB;
constexpr std::uint64_t kTestNtfsLength = 1746 * kMiB;

// A volume kept as its chunks that are not all zeros (see tools/make_ntfs_fixture.sh).
struct VolumeFixture {
    std::uint64_t size = 0;
    std::uint32_t chunk = 0;
    std::vector<std::uint64_t> offsets;
    Bytes data;  // the chunks, one after the other
};

VolumeFixture load_volume_fixture(const fs::path& path) {
    const Bytes raw = [&] {
        std::ifstream f(path, std::ios::binary);
        REQUIRE(f.good());
        return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }();
    std::size_t at = 0;
    const auto u = [&](int bytes) {
        REQUIRE(at + static_cast<std::size_t>(bytes) <= raw.size());
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(raw[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += static_cast<std::size_t>(bytes);
        return v;
    };
    REQUIRE(raw.size() > 8);
    REQUIRE(std::memcmp(raw.data(), "VKFIX001", 8) == 0);
    at = 8;
    VolumeFixture v;
    v.size = u(8);
    v.chunk = static_cast<std::uint32_t>(u(4));
    const std::uint64_t count = u(4);
    for (std::uint64_t i = 0; i < count; ++i) v.offsets.push_back(u(8));
    const std::uint64_t raw_len = u(8);
    REQUIRE(raw_len == count * v.chunk);
    ZstdDecompressor z;
    v.data = z.decompress(ByteSpan(raw.data() + at, raw.size() - at), static_cast<std::size_t>(raw_len));
    return v;
}

std::uint64_t le(const Bytes& b, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

const fs::path kNtfsFixture = fs::path(VAULTIK_TEST_DATA) / "ntfs-1746m.vkfix";

}  // namespace

TEST_CASE("image: the NTFS fixture is an empty volume made for the test disk's data partition") {
    const VolumeFixture v = load_volume_fixture(kNtfsFixture);
    CHECK(v.size == kTestNtfsLength);
    REQUIRE_FALSE(v.offsets.empty());
    CHECK(v.offsets.front() == 0);
    const Bytes boot(v.data.begin(), v.data.begin() + 512);
    CHECK(std::memcmp(&boot[3], "NTFS    ", 8) == 0);
    CHECK(le(boot, 11, 2) == 512);                          // bytes per sector
    CHECK(le(boot, 13, 1) == 8);                            // sectors per cluster: 4 KiB clusters
    CHECK(le(boot, 28, 4) == kTestNtfsOffset / 512);        // hidden sectors: where the partition starts
    CHECK(le(boot, 40, 8) == kTestNtfsLength / 512 - 1);    // sectors, less the backup boot sector
    CHECK(boot[510] == 0x55);
    CHECK(boot[511] == 0xAA);
    // The backup boot sector, in the volume's last sector.
    CHECK(v.offsets.back() + v.chunk == v.size);
}

TEST_CASE("image: the test disk for the elevated disk image test (only when asked)") {
    // Writes <dir>\source.vhdx (GPT: an EFI partition of known random bytes and the empty NTFS
    // volume) and <dir>\target.vhdx (blank, the same size), for the elevated test script.
    const std::string dir = os::get_env("VAULTIK_TEST_DISK_DIR");
    if (dir.empty()) {
        MESSAGE("skipped: VAULTIK_TEST_DISK_DIR is not set");
        return;
    }
    const fs::path out(std::u8string(dir.begin(), dir.end()));
    REQUIRE(fs::is_directory(out));
    const auto path_of = [&](const char* name) {
        const std::u8string p = (out / name).u8string();
        return std::string(p.begin(), p.end());
    };
    REQUIRE_FALSE(fs::exists(out / "source.vhdx"));
    REQUIRE_FALSE(fs::exists(out / "target.vhdx"));

    DiskLayout l;
    l.style = PartitionStyle::Gpt;
    l.size = kTestDiskSize;
    l.disk_guid = format_guid(random_guid());
    Partition efi;
    efi.number = 1;
    efi.offset = kTestEfiOffset;
    efi.length = kTestEfiLength;
    efi.type = std::string(kEfiSystemType);
    efi.guid = format_guid(random_guid());
    efi.name = "EFI system partition";
    Partition data;
    data.number = 2;
    data.offset = kTestNtfsOffset;
    data.length = kTestNtfsLength;
    data.type = std::string(kBasicDataType);
    data.guid = format_guid(random_guid());
    data.name = "Basic data partition";
    l.partitions = {efi, data};
    l.id = disk_id(l);

    {
        auto file = create_random_file(path_of("source.vhdx"));
        VhdxWriter w(*file, kTestDiskSize);
        for (const DiskWrite& t : partition_tables(l, kTestDiskSize)) w.write(t.offset, t.data);
        // The EFI partition: random bytes (it is copied byte for byte, not as a file system).
        Bytes chunk(static_cast<std::size_t>(kMiB));
        for (std::uint64_t at = 0; at < kTestEfiLength; at += kMiB) {
            fill_random(chunk.data(), chunk.size(), static_cast<std::uint32_t>(1000 + at / kMiB));
            w.write(kTestEfiOffset + at, chunk);
        }
        // The NTFS volume: its chunks that are not zeros; the rest of the new disk is zeros.
        const VolumeFixture v = load_volume_fixture(kNtfsFixture);
        REQUIRE(v.size == kTestNtfsLength);
        for (std::size_t i = 0; i < v.offsets.size(); ++i)
            w.write(kTestNtfsOffset + v.offsets[i],
                    ByteSpan(v.data.data() + i * v.chunk, v.chunk));
        w.finish();
    }
    {
        auto file = create_random_file(path_of("target.vhdx"));
        VhdxWriter w(*file, kTestDiskSize);
        w.finish();
    }
    MESSAGE("wrote " << path_of("source.vhdx") << " (disk " << l.disk_guid << ") and " << path_of("target.vhdx"));
}

// ---- restoring onto a smaller disk -----------------------------------------------------------

TEST_CASE("image: partitions are placed on a smaller disk by shrinking the last ones that can") {
    DiskLayout l;
    l.style = PartitionStyle::Gpt;
    l.size = 1000 * kMiB;
    l.disk_guid = "11111111-2222-3333-4444-555555555555";
    const auto part = [](int n, std::uint64_t off, std::uint64_t len) {
        Partition p;
        p.number = n;
        p.offset = off * kMiB;
        p.length = len * kMiB;
        p.type = std::string(kBasicDataType);
        p.guid = "AAAAAAAA-0000-0000-0000-00000000000" + std::to_string(n);
        return p;
    };
    // EFI, C:, D:, a recovery partition, with a gap before the recovery partition.
    l.partitions = {part(1, 1, 99), part(2, 100, 500), part(3, 600, 300), part(4, 910, 50)};
    l.partitions[3].type = std::string(kRecoveryType);
    const std::vector<std::uint64_t> full = {99 * kMiB, 500 * kMiB, 300 * kMiB, 50 * kMiB};
    const std::vector<std::uint64_t> mins = {99 * kMiB, 200 * kMiB + 5, 250 * kMiB, 50 * kMiB};
    const std::uint64_t gpt = 33 * 512;  // the backup GPT

    std::vector<Placement> p;
    CHECK(place_partitions(l, l.size, 512, mins, p).empty());
    CHECK(p == std::vector<Placement>{{kMiB, 99 * kMiB}, {100 * kMiB, 500 * kMiB}, {600 * kMiB, 300 * kMiB}, {910 * kMiB, 50 * kMiB}});
    CHECK(smallest_disk(l, {}) == 960 * kMiB + gpt);
    CHECK(smallest_disk(l, full) == 960 * kMiB + gpt);
    // C: can lose 299 MiB (whole MiB above its minimum), D: 50 MiB.
    CHECK(smallest_disk(l, mins) == 960 * kMiB + gpt - 349 * kMiB);

    // 40 MiB short (and the backup GPT): D:, the last that can shrink, gives 41 MiB; the recovery
    // partition moves down.
    REQUIRE(place_partitions(l, 920 * kMiB, 512, mins, p).empty());
    CHECK(p[0] == Placement{kMiB, 99 * kMiB});
    CHECK(p[1] == Placement{100 * kMiB, 500 * kMiB});
    CHECK(p[2] == Placement{600 * kMiB, 259 * kMiB});
    CHECK(p[3] == Placement{869 * kMiB, 50 * kMiB});
    CHECK(p[3].offset + p[3].length + gpt <= 920 * kMiB);
    // Stored bytes between partitions move with them; the ones before stay.
    CHECK(placed_offset(l, p, 0) == 0);
    CHECK(placed_offset(l, p, 900 * kMiB) == 859 * kMiB);

    // 100 MiB short: D: down to its minimum, then C:.
    REQUIRE(place_partitions(l, 860 * kMiB, 512, mins, p).empty());
    CHECK(p[2].length == 250 * kMiB);
    CHECK(p[1].length == 449 * kMiB);
    CHECK(p[2].offset == 549 * kMiB);
    CHECK(p[3].offset == 809 * kMiB);

    // Too small even then: says how small it can go.
    const std::string why = place_partitions(l, 600 * kMiB, 512, mins, p);
    CHECK(why.find("too small") != std::string::npos);
    CHECK(why.find("at least") != std::string::npos);
    CHECK_FALSE(fits(l, 600 * kMiB, 512, mins).empty());
    CHECK(fits(l, smallest_disk(l, mins), 512, mins).empty());
    CHECK_FALSE(fits(l, smallest_disk(l, mins) - 512, 512, mins).empty());
    CHECK_FALSE(fits(l, 920 * kMiB, 512).empty());         // without shrinking
    CHECK_FALSE(fits(l, 920 * kMiB, 4096, mins).empty());  // other sector size

    // The placed layout: new places, holes past a new end gone.
    l.partitions[2].holes = {{10 * kMiB, 20 * kMiB}, {250 * kMiB, 50 * kMiB}};
    REQUIRE(place_partitions(l, 920 * kMiB, 512, mins, p).empty());
    const DiskLayout placed = placed_layout(l, p);
    CHECK(placed.partitions[2].length == 259 * kMiB);
    CHECK(placed.partitions[2].holes ==
          std::vector<std::pair<std::uint64_t, std::uint64_t>>{{10 * kMiB, 20 * kMiB}, {250 * kMiB, 9 * kMiB}});
    CHECK(placed.partitions[3].offset == 869 * kMiB);
    CHECK(placed.partitions[3].guid == l.partitions[3].guid);
    CHECK(partition_tables(placed, 920 * kMiB).size() == 2);

    // MBR: the entries of shrunk and moved partitions change; logical partitions are not moved.
    DiskLayout m = l;
    m.style = PartitionStyle::Mbr;
    for (Partition& q : m.partitions) q.mbr_type = 0x07;
    m.partitions[3].mbr_type = 0x27;
    REQUIRE(place_partitions(m, 920 * kMiB, 512, mins, p).empty());
    CHECK(p[2].length == 260 * kMiB);  // no backup GPT on MBR disks
    Bytes mbr(512, 0);
    for (std::size_t k = 0; k < 4; ++k) {
        std::uint8_t* e = mbr.data() + 446 + 16 * k;
        e[4] = m.partitions[k].mbr_type;
        const auto first = static_cast<std::uint32_t>(m.partitions[k].offset / 512);
        const auto count = static_cast<std::uint32_t>(m.partitions[k].length / 512);
        for (int b = 0; b < 4; ++b) {
            e[8 + b] = static_cast<std::uint8_t>(first >> (8 * b));
            e[12 + b] = static_cast<std::uint8_t>(count >> (8 * b));
        }
    }
    const Bytes before = mbr;
    place_mbr_entries(m, p, mbr);
    CHECK(std::memcmp(mbr.data() + 446, before.data() + 446, 32) == 0);  // the first two did not change
    CHECK(le(mbr, 446 + 32 + 12, 4) == 260 * kMiB / 512);
    CHECK(le(mbr, 446 + 48 + 8, 4) == 870 * kMiB / 512);
    CHECK(le(mbr, 446 + 48 + 12, 4) == 50 * kMiB / 512);
    Partition ext = part(5, 960, 30);
    ext.mbr_type = 0x0F;
    m.partitions.push_back(ext);
    m.size = 1100 * kMiB;
    std::vector<std::uint64_t> mins5 = mins;
    mins5.push_back(30 * kMiB);
    const std::string no = place_partitions(m, 920 * kMiB, 512, mins5, p);
    CHECK(no.find("logical partitions") != std::string::npos);
}

namespace {

// A GPT (or MBR) disk like the elevated test's: a small EFI partition of random bytes, the NTFS
// fixture volume (as read from a shadow copy) and a recovery partition of random bytes after it,
// which moves down when the NTFS volume shrinks. Sparse: only what is not zeros is kept.
struct FixtureDisk : DiskSource {
    DiskLayout l;
    VolumeFixture v;
    std::map<std::uint64_t, std::size_t> chunks;  // volume offset -> chunk number
    Bytes efi, recovery, head, tail;

    explicit FixtureDisk(PartitionStyle style = PartitionStyle::Gpt) {
        v = load_volume_fixture(kNtfsFixture);
        for (std::size_t i = 0; i < v.offsets.size(); ++i) chunks[v.offsets[i]] = i;
        l.style = style;
        l.size = kTestNtfsOffset + kTestNtfsLength + 17 * kMiB;
        l.disk_guid = "11111111-2222-3333-4444-555555555555";
        l.signature = 0x12345678;
        Partition e;
        e.number = 1;
        e.offset = kTestEfiOffset;
        e.length = 8 * kMiB;
        e.type = std::string(kEfiSystemType);
        e.guid = "AAAAAAAA-0000-0000-0000-000000000001";
        e.name = "EFI system partition";
        e.mbr_type = 0xEF;
        e.filesystem = "FAT32";
        Partition n;
        n.number = 2;
        n.offset = kTestNtfsOffset;
        n.length = kTestNtfsLength;
        n.type = std::string(kBasicDataType);
        n.guid = "AAAAAAAA-0000-0000-0000-000000000002";
        n.name = "Basic data partition";
        n.mbr_type = 0x07;
        n.filesystem = "NTFS";
        n.label = "VaultikTest";
        n.mount = "E:\\";
        n.method = ReadMethod::Used;
        n.shadow_copy = true;
        n.cluster_size = 4096;
        Partition r;
        r.number = 3;
        r.offset = kTestNtfsOffset + kTestNtfsLength;
        r.length = 16 * kMiB;
        r.type = std::string(kRecoveryType);
        r.guid = "AAAAAAAA-0000-0000-0000-000000000003";
        r.name = "Recovery";
        r.attributes = 0x8000000000000001ull;
        r.mbr_type = 0x27;
        l.partitions = {e, n, r};
        l.id = disk_id(l);
        efi.resize(static_cast<std::size_t>(e.length));
        fill_random(efi.data(), efi.size(), 11);
        recovery.resize(static_cast<std::size_t>(r.length));
        fill_random(recovery.data(), recovery.size(), 12);
        head.assign(kMiB, 0);
        tail.assign(kMiB, 0);
        if (style == PartitionStyle::Gpt) {
            for (const DiskWrite& w : partition_tables(l, l.size)) {
                if (w.offset < kMiB) std::memcpy(head.data() + w.offset, w.data.data(), w.data.size());
                else std::memcpy(tail.data() + (w.offset - (l.size - kMiB)), w.data.data(), w.data.size());
            }
        } else {
            for (std::size_t k = 0; k < l.partitions.size(); ++k) {
                std::uint8_t* x = head.data() + 446 + 16 * k;
                x[4] = l.partitions[k].mbr_type;
                const auto first = static_cast<std::uint32_t>(l.partitions[k].offset / 512);
                const auto count = static_cast<std::uint32_t>(l.partitions[k].length / 512);
                for (int b = 0; b < 4; ++b) {
                    x[8 + b] = static_cast<std::uint8_t>(first >> (8 * b));
                    x[12 + b] = static_cast<std::uint8_t>(count >> (8 * b));
                }
            }
            for (int b = 0; b < 4; ++b) head[440 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(l.signature >> (8 * b));
            head[510] = 0x55;
            head[511] = 0xAA;
        }
    }

    // The volume's bytes.
    void read_volume(std::uint64_t offset, std::uint8_t* dst, std::size_t length) const {
        while (length > 0) {
            const std::uint64_t c = offset / v.chunk * v.chunk;
            const std::size_t within = static_cast<std::size_t>(offset - c);
            const std::size_t n = std::min<std::size_t>(length, v.chunk - within);
            auto it = chunks.find(c);
            if (it == chunks.end()) std::memset(dst, 0, n);
            else std::memcpy(dst, v.data.data() + it->second * v.chunk + within, n);
            offset += n;
            dst += n;
            length -= n;
        }
    }
    Bytes volume_bytes(std::uint64_t offset, std::size_t length) const {
        Bytes b(length);
        read_volume(offset, b.data(), length);
        return b;
    }

    DiskLayout& layout() override { return l; }
    std::vector<Extent> plan(std::size_t i) override {
        const Partition& p = l.partitions[i];
        if (p.method != ReadMethod::Used) return {{0, p.length, Extent::Kind::Data}};
        // "In use": the clusters of the chunks that are not zeros (enough for a plan; the restore
        // reads the volume's own bitmap).
        const std::uint64_t clusters = p.length / 4096;
        Bytes bm(static_cast<std::size_t>((clusters + 7) / 8), 0);
        for (const auto& [off, k] : chunks)
            for (std::uint64_t c = off / 4096; c < (off + v.chunk) / 4096 && c < clusters; ++c)
                bm[static_cast<std::size_t>(c / 8)] |= static_cast<std::uint8_t>(1u << (c % 8));
        return plan_extents(bm, clusters, 4096, p.length);
    }
    void read(std::size_t i, std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        if (i == 0) std::memcpy(dst, efi.data() + offset, length);
        else if (i == 2) std::memcpy(dst, recovery.data() + offset, length);
        else read_volume(offset, dst, length);
    }
    void read_disk(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        std::memset(dst, 0, length);
        if (offset < kMiB) std::memcpy(dst, head.data() + offset, std::min<std::size_t>(length, static_cast<std::size_t>(kMiB - offset)));
        if (offset >= l.size - kMiB) std::memcpy(dst, tail.data() + (offset - (l.size - kMiB)), length);
    }
};

// A target that keeps what is written in 1 MiB blocks (the rest reads as zeros), and counts writes.
struct SparseTarget : DiskTarget {
    std::uint64_t bytes = 0;
    bool is_zeroed = true;
    bool finished = false;
    int writes = 0;
    std::map<std::uint64_t, Bytes> blocks;
    explicit SparseTarget(std::uint64_t size, bool zeroed = true) : bytes(size), is_zeroed(zeroed) {}
    std::uint64_t size() const override { return bytes; }
    std::uint32_t sector_size() const override { return 512; }
    bool zeroed() const override { return is_zeroed; }
    std::uint32_t alignment() const override { return 512; }
    void write(std::uint64_t offset, ByteSpan data) override {
        REQUIRE(offset % 512 == 0);
        REQUIRE(data.size() % 512 == 0);
        REQUIRE(offset + data.size() <= bytes);
        ++writes;
        for (std::size_t done = 0; done < data.size();) {
            const std::uint64_t at = offset + done;
            Bytes& b = blocks[at / kMiB];
            if (b.empty()) b.assign(kMiB, 0);
            const std::size_t within = static_cast<std::size_t>(at % kMiB);
            const std::size_t n = std::min<std::size_t>(data.size() - done, kMiB - within);
            std::memcpy(b.data() + within, data.data() + done, n);
            done += n;
        }
    }
    void finish() override { finished = true; }
    Bytes read(std::uint64_t offset, std::size_t length) const {
        Bytes out(length);
        for (std::size_t done = 0; done < length;) {
            const std::uint64_t at = offset + done;
            const std::size_t within = static_cast<std::size_t>(at % kMiB);
            const std::size_t n = std::min<std::size_t>(length - done, kMiB - within);
            auto it = blocks.find(at / kMiB);
            if (it != blocks.end()) std::memcpy(out.data() + done, it->second.data() + within, n);
            done += n;
        }
        return out;
    }
};

using TestRuns = std::vector<std::pair<std::int64_t, std::uint64_t>>;  // (lcn or -1, clusters)

// An NTFS check written apart from the restore's own code: the file records in use, their runs,
// and the volume bitmap must agree exactly; the boot sector, its backup, $Bitmap's and $Bad's
// sizes and the mirror of the first records must fit the volume's size.
struct NtfsCheck {
    std::function<Bytes(std::uint64_t, std::size_t)> read;  // from the partition start
    std::uint64_t partition_length = 0;
    Bytes boot;
    std::uint64_t cs = 0, rs = 1024, clusters = 0;
    TestRuns mft;

    static TestRuns runs_of(const Bytes& rec, std::size_t a) {
        TestRuns out;
        std::size_t p = a + static_cast<std::size_t>(le(rec, a + 0x20, 2));
        std::int64_t lcn = 0;
        while (rec[p] != 0) {
            const int nl = rec[p] & 15, no = rec[p] >> 4;
            ++p;
            const std::uint64_t len = le(rec, p, nl);
            p += static_cast<std::size_t>(nl);
            if (no == 0) {
                out.emplace_back(-1, len);
                continue;
            }
            std::int64_t d = static_cast<std::int64_t>(le(rec, p, no));
            if (no < 8 && (rec[p + static_cast<std::size_t>(no) - 1] & 0x80)) d -= std::int64_t{1} << (8 * no);
            p += static_cast<std::size_t>(no);
            lcn += d;
            out.emplace_back(lcn, len);
        }
        return out;
    }
    // Offset of the first attribute of `type` named `name` (ASCII), or 0.
    static std::size_t attr(const Bytes& rec, std::uint64_t type, const std::string& name = "") {
        std::size_t a = static_cast<std::size_t>(le(rec, 0x14, 2));
        while (le(rec, a, 4) != 0xFFFFFFFF) {
            const std::size_t nl = rec[a + 9], no = static_cast<std::size_t>(le(rec, a + 10, 2));
            std::string n;
            for (std::size_t i = 0; i < nl; ++i) n.push_back(static_cast<char>(rec[a + no + 2 * i]));
            if (le(rec, a, 4) == type && n == name) return a;
            a += static_cast<std::size_t>(le(rec, a + 4, 4));
        }
        return 0;
    }
    static bool unfix(Bytes& r) {
        const std::size_t usa = static_cast<std::size_t>(le(r, 4, 2)), n = static_cast<std::size_t>(le(r, 6, 2));
        for (std::size_t i = 1; i < n; ++i) {
            if (le(r, i * 512 - 2, 2) != le(r, usa, 2)) return false;
            r[i * 512 - 2] = r[usa + 2 * i];
            r[i * 512 - 1] = r[usa + 2 * i + 1];
        }
        return true;
    }
    Bytes data(const TestRuns& runs, std::uint64_t size) const {
        Bytes out;
        for (const auto& [lcn, len] : runs) {
            if (out.size() >= size) break;
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(len * cs, size - out.size()));
            const Bytes b = lcn < 0 ? Bytes(n, 0) : read(static_cast<std::uint64_t>(lcn) * cs, n);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    }
    // The boot sector and the master file table's runs.
    void open() {
        boot = read(0, 512);
        REQUIRE(std::memcmp(&boot[3], "NTFS    ", 8) == 0);
        REQUIRE(static_cast<std::int8_t>(boot[0x40]) == -10);  // 1 KiB file records
        cs = le(boot, 11, 2) * le(boot, 13, 1);
        clusters = le(boot, 0x28, 8) * 512 / cs;
        Bytes r0 = read(le(boot, 0x30, 8) * cs, 1024);
        REQUIRE(unfix(r0));
        mft = runs_of(r0, attr(r0, 0x80));
    }
    Bytes record(std::uint64_t n, bool fixed = true) const {
        const Bytes all = data(mft, (n + 1) * rs);
        Bytes r(all.end() - static_cast<std::ptrdiff_t>(rs), all.end());
        if (fixed) REQUIRE(unfix(r));
        return r;
    }

    void run() {
        open();
        const std::uint64_t total = le(boot, 0x28, 8);
        CHECK(total == partition_length / 512 - 1);
        CHECK(read(total * 512, 512) == boot);  // the backup boot sector, in the last sector
        Bytes r0 = record(0);
        const std::size_t d0 = attr(r0, 0x80);
        const std::uint64_t records = le(r0, d0 + 0x38, 8) / rs;

        // Every cluster a file record in use refers to, once.
        std::vector<std::uint8_t> used(static_cast<std::size_t>(clusters), 0);
        const Bytes all = data(mft, records * rs);
        for (std::uint64_t n = 0; n < records; ++n) {
            Bytes r(all.begin() + static_cast<std::ptrdiff_t>(n * rs), all.begin() + static_cast<std::ptrdiff_t>((n + 1) * rs));
            if (std::memcmp(r.data(), "FILE", 4) != 0 || (le(r, 0x16, 2) & 1) == 0) continue;
            REQUIRE(unfix(r));
            std::size_t a = static_cast<std::size_t>(le(r, 0x14, 2));
            while (le(r, a, 4) != 0xFFFFFFFF) {
                if (r[a + 8] != 0)
                    for (const auto& [lcn, len] : runs_of(r, a)) {
                        if (lcn < 0) continue;
                        REQUIRE(static_cast<std::uint64_t>(lcn) + len <= clusters);
                        for (std::uint64_t c = 0; c < len; ++c) {
                            const auto k = static_cast<std::size_t>(static_cast<std::uint64_t>(lcn) + c);
                            CHECK(used[k] == 0);
                            used[k] = 1;
                        }
                    }
                a += static_cast<std::size_t>(le(r, a + 4, 4));
            }
        }
        // $Bitmap: sized for this volume, and exactly the clusters in use.
        const Bytes r6 = record(6);
        const std::size_t b6 = attr(r6, 0x80);
        const std::uint64_t bm_size = le(r6, b6 + 0x30, 8);
        CHECK(bm_size == ((clusters + 7) / 8 + 7) / 8 * 8);
        CHECK(le(r6, b6 + 0x38, 8) == bm_size);
        std::uint64_t bm_alloc = 0;
        for (const auto& run : runs_of(r6, b6)) bm_alloc += run.second * cs;
        CHECK(le(r6, b6 + 0x28, 8) == bm_alloc);
        CHECK(bm_alloc == (bm_size + cs - 1) / cs * cs);
        const Bytes bm = data(runs_of(r6, b6), bm_size);
        std::uint64_t wrong = 0;
        for (std::uint64_t c = 0; c < clusters; ++c)
            if (((bm[static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1) != used[static_cast<std::size_t>(c)]) ++wrong;
        CHECK(wrong == 0);
        // $BadClus:$Bad spans the volume.
        const Bytes r8 = record(8);
        const std::size_t bad = attr(r8, 0x80, "$Bad");
        REQUIRE(bad != 0);
        CHECK(le(r8, bad + 0x30, 8) == clusters * cs);
        CHECK(le(r8, bad + 0x28, 8) == clusters * cs);
        CHECK(le(r8, bad + 0x18, 8) == clusters - 1);
        std::uint64_t bad_len = 0;
        for (const auto& run : runs_of(r8, bad)) bad_len += run.second;
        CHECK(bad_len == clusters);
        // The mirror holds the first records as the master file table does.
        const Bytes r1 = record(1);
        const std::size_t m1 = attr(r1, 0x80);
        const std::uint64_t mirrored = le(r1, m1 + 0x30, 8) / rs;
        const Bytes mirror = data(runs_of(r1, m1), mirrored * rs);
        CHECK(std::equal(mirror.begin(), mirror.end(), all.begin()));
    }
};

NtfsCheck check_of(const FixtureDisk& d) {
    NtfsCheck c;
    c.read = [&d](std::uint64_t off, std::size_t n) { return d.volume_bytes(off, n); };
    c.partition_length = d.l.partitions[1].length;
    c.open();
    return c;
}

NtfsCheck check_of(const SparseTarget& t, std::uint64_t offset, std::uint64_t length) {
    NtfsCheck c;
    c.read = [&t, offset](std::uint64_t off, std::size_t n) { return t.read(offset + off, n); };
    c.partition_length = length;
    return c;
}

VolumeReader reader_of(const FixtureDisk& d) {
    return [&d](std::uint64_t off, std::uint8_t* dst, std::size_t n) { d.read_volume(off, dst, n); };
}

}  // namespace

TEST_CASE("image: NTFS metadata with impossible values is refused or not trusted") {
    FixtureDisk disk;
    const std::uint64_t length = disk.l.partitions[1].length;
    const VolumeReader base = reader_of(disk);
    const NtfsFacts good = ntfs_facts(base, length);
    // The volume as read, with `bytes` laid over it at `at`.
    const auto patched = [&base](std::uint64_t at, Bytes bytes) -> VolumeReader {
        return [&base, at, bytes](std::uint64_t off, std::uint8_t* dst, std::size_t n) {
            base(off, dst, n);
            for (std::size_t i = 0; i < bytes.size(); ++i)
                if (at + i >= off && at + i < off + n) dst[at + i - off] = bytes[i];
        };
    };
    // Sectors per cluster given as 2^(256 - value), with exponents past any cluster size and
    // past what a 64-bit shift can do.
    for (const int spc : {0x81, 0xA0, 0xBF, 0xC0, 0xF3})
        CHECK_THROWS_WITH_AS(ntfs_facts(patched(0x0D, {static_cast<std::uint8_t>(spc)}), length),
                             doctest::Contains("cluster size"), Error);

    // $Bitmap claiming a size far beyond the volume's clusters: only what they need is read.
    Bytes boot(512);
    base(0, boot.data(), boot.size());
    const std::uint64_t cs = le(boot, 0x0B, 2) * le(boot, 0x0D, 1);
    const auto rec_byte = static_cast<std::int8_t>(boot[0x40]);
    const std::uint64_t rs = rec_byte < 0 ? std::uint64_t{1} << -rec_byte : static_cast<std::uint64_t>(rec_byte) * cs;
    const std::uint64_t rec6 = le(boot, 0x30, 8) * cs + 6 * rs;  // in the master file table's first run
    Bytes r6(static_cast<std::size_t>(rs));
    base(rec6, r6.data(), r6.size());
    REQUIRE(std::memcmp(r6.data(), "FILE", 4) == 0);
    std::size_t a = static_cast<std::size_t>(le(r6, 0x14, 2));
    while (a + 8 < r6.size() && le(r6, a, 4) != 0x80 && le(r6, a, 4) != 0xFFFFFFFF) a += static_cast<std::size_t>(le(r6, a + 4, 4));
    REQUIRE(le(r6, a, 4) == 0x80);
    const std::uint64_t size_at = rec6 + a + 0x30;
    REQUIRE(size_at % 512 < 510);  // (not where the update sequence goes)
    const auto le_bytes = [](std::uint64_t v) {
        Bytes b(8);
        for (int i = 0; i < 8; ++i) b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        return b;
    };
    for (const std::uint64_t huge : {std::uint64_t{1} << 34, std::uint64_t{1} << 62}) {
        NtfsFacts f;
        CHECK_NOTHROW(f = ntfs_facts(patched(size_at, le_bytes(huge)), length));
        CHECK(f.used_clusters == good.used_clusters);
        CHECK(f.used_end == good.used_end);
    }
    // One too short for the clusters is refused.
    CHECK_THROWS_WITH_AS(ntfs_facts(patched(size_at, le_bytes(good.clusters / 8 - 1)), length),
                         doctest::Contains("$Bitmap"), Error);
}

TEST_CASE("image: an NTFS volume is shrunk to restore onto a smaller disk") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    FixtureDisk disk;
    ImageBackupOptions o;
    o.disks = {&disk};
    const ImageBackupResult r = run_image_backup(*repo, o);
    const Partition ntfs = disk.l.partitions[1];
    const Partition rec = disk.l.partitions[2];

    // How far it can shrink: to just past its last cluster in use.
    const NtfsFacts facts = ntfs_facts(reader_of(disk), ntfs.length);
    CHECK(facts.cluster_size == 4096);
    CHECK(facts.clusters == (ntfs.length / 512 - 1) / 8);
    REQUIRE(facts.used_end < facts.clusters);
    std::vector<std::string> notes;
    const std::vector<std::uint64_t> limits = shrink_limits(*repo, r.snapshot, 0, &notes);
    CHECK(notes.empty());
    REQUIRE(limits.size() == 3);
    CHECK(limits[0] == disk.l.partitions[0].length);
    CHECK(limits[2] == rec.length);
    CHECK(limits[1] == (facts.used_end * 4096 + 512 + kMiB - 1) / kMiB * kMiB);
    const std::uint64_t smallest = smallest_disk(disk.l, limits);
    CHECK(smallest < disk.l.size - 800 * kMiB);

    // Restore onto a disk 600 MiB smaller than the original.
    const std::uint64_t target_size = disk.l.size - 600 * kMiB;
    SparseTarget target(target_size);
    ImageRestoreOptions ro;
    std::vector<std::string> told;
    ro.on_note = [&](const std::string& s) { told.push_back(s); };
    run_image_restore(*repo, r.snapshot, target, ro);
    CHECK(target.finished);
    REQUIRE(told.size() == 2);
    CHECK(told[0].find("shrunk") != std::string::npos);
    CHECK(told[1].find("moves") != std::string::npos);

    // The partition table: the NTFS partition 600 MiB shorter, the recovery partition moved down
    // with its type, GUID, attributes and bytes.
    const Bytes hdr = target.read(512, 512);
    REQUIRE(std::memcmp(hdr.data(), "EFI PART", 8) == 0);
    CHECK(le(hdr, 32, 8) == target_size / 512 - 1);
    const Bytes entries = target.read(1024, 3 * 128);
    const std::uint64_t ntfs_first = le(entries, 128 + 32, 8), ntfs_last = le(entries, 128 + 40, 8);
    CHECK(ntfs_first * 512 == ntfs.offset);
    const std::uint64_t new_length = (ntfs_last + 1) * 512 - ntfs.offset;
    CHECK(new_length == ntfs.length - 600 * kMiB);  // the disk had 1 MiB free at its end
    const std::uint64_t rec_first = le(entries, 256 + 32, 8);
    CHECK(rec_first * 512 == ntfs.offset + new_length);
    CHECK((le(entries, 256 + 40, 8) + 1) * 512 == rec_first * 512 + rec.length);
    CHECK(std::memcmp(entries.data() + 256, parse_guid(kRecoveryType).data(), 16) == 0);
    CHECK(std::memcmp(entries.data() + 256 + 16, parse_guid(rec.guid).data(), 16) == 0);
    CHECK(le(entries, 256 + 48, 8) == rec.attributes);
    CHECK(target.read(rec_first * 512, static_cast<std::size_t>(rec.length)) == disk.recovery);
    CHECK(target.read(disk.l.partitions[0].offset, static_cast<std::size_t>(disk.l.partitions[0].length)) == disk.efi);

    // The volume: consistent at its new size, marked dirty, its log emptied.
    NtfsCheck check = check_of(target, ntfs.offset, new_length);
    check.run();
    CHECK(le(check.boot, 0x1C, 4) == ntfs.offset / 512);  // hidden sectors: it did not move
    const Bytes r3 = check.record(3);
    const std::size_t vi = NtfsCheck::attr(r3, 0x70);
    REQUIRE(vi != 0);
    CHECK((le(r3, vi + static_cast<std::size_t>(le(r3, vi + 0x14, 2)) + 0x0A, 2) & 1) == 1);
    const Bytes r2 = check.record(2);
    const std::size_t lg = NtfsCheck::attr(r2, 0x80);
    const Bytes log = check.data(NtfsCheck::runs_of(r2, lg), le(r2, lg + 0x30, 8));
    CHECK(std::all_of(log.begin(), log.end(), [](std::uint8_t b) { return b == 0xFF; }));
    // Every other file record came back as it was.
    const NtfsCheck src = check_of(disk);
    for (const std::uint64_t n : {0, 1, 2, 4, 7, 9, 10, 11}) CHECK(src.record(n, false) == check.record(n, false));

    SUBCASE("onto a disk with old data") {
        SparseTarget used(target_size, false);
        run_image_restore(*repo, r.snapshot, used, {});
        check_of(used, ntfs.offset, new_length).run();
    }
    SUBCASE("onto the smallest disk it fits") {
        SparseTarget small(smallest);
        run_image_restore(*repo, r.snapshot, small, {});
        NtfsCheck again = check_of(small, ntfs.offset, limits[1]);
        again.run();
        CHECK(again.clusters >= facts.used_end);
        CHECK(small.read(ntfs.offset + limits[1], static_cast<std::size_t>(rec.length)) == disk.recovery);
    }
    SUBCASE("refused below that, before anything is written") {
        SparseTarget tiny(smallest - kMiB);
        try {
            run_image_restore(*repo, r.snapshot, tiny, {});
            FAIL("restored onto a disk that is too small");
        } catch (const Error& e) {
            const std::string what = e.what();
            CHECK(what.find("at least " + size_text(smallest, true)) != std::string::npos);
        }
        CHECK(tiny.writes == 0);
    }
    SUBCASE("the volume Windows ran from keeps free space") {
        FixtureDisk win;
        win.l.partitions[1].windows = true;
        ImageBackupOptions wo;
        wo.disks = {&win};
        const ImageBackupResult wr = run_image_backup(*repo, wo);
        const std::vector<std::uint64_t> wl = shrink_limits(*repo, wr.snapshot, 0);
        CHECK(wl[1] >= facts.used_clusters * 4096 + kWindowsReserve);
        CHECK(wl[1] > limits[1]);
    }
    SUBCASE("a volume read while in use is not shrunk") {
        FixtureDisk live;
        live.l.partitions[1].shadow_copy = false;
        ImageBackupOptions lo;
        lo.disks = {&live};
        const ImageBackupResult lr = run_image_backup(*repo, lo);
        std::vector<std::string> why;
        CHECK(shrink_limits(*repo, lr.snapshot, 0, &why)[1] == ntfs.length);
        REQUIRE(why.size() == 1);
        CHECK(why[0].find("in use") != std::string::npos);
        SparseTarget t(target_size);
        CHECK_THROWS_AS(run_image_restore(*repo, lr.snapshot, t, {}), Error);
        CHECK(t.writes == 0);
    }
}

TEST_CASE("image: a volume whose bitmap misses clusters of its files is refused, not cut") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    FixtureDisk disk;
    const Partition ntfs = disk.l.partitions[1];
    // Clear the bitmap bits of the last 4 MiB up to the last cluster in use: the volume says they
    // are free while a file still uses them.
    const NtfsFacts facts = ntfs_facts(reader_of(disk), ntfs.length);
    const NtfsCheck src = check_of(disk);
    const Bytes r6 = src.record(6);
    const TestRuns bm = NtfsCheck::runs_of(r6, NtfsCheck::attr(r6, 0x80));
    REQUIRE(bm.size() == 1);
    for (std::uint64_t bit = facts.used_end - 1024; bit < facts.used_end; ++bit) {
        const std::uint64_t at = static_cast<std::uint64_t>(bm[0].first) * 4096 + bit / 8;
        const auto it = disk.chunks.find(at / disk.v.chunk * disk.v.chunk);
        REQUIRE(it != disk.chunks.end());
        disk.v.data[it->second * disk.v.chunk + at % disk.v.chunk] &= static_cast<std::uint8_t>(~(1u << (bit % 8)));
    }
    REQUIRE(ntfs_facts(reader_of(disk), ntfs.length).used_end + 512 < facts.used_end);

    ImageBackupOptions o;
    o.disks = {&disk};
    const ImageBackupResult r = run_image_backup(*repo, o);
    const std::uint64_t smallest = smallest_disk(disk.l, shrink_limits(*repo, r.snapshot, 0));
    SparseTarget t(smallest);
    try {
        run_image_restore(*repo, r.snapshot, t, {});
        FAIL("a volume with a wrong bitmap was cut");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("chkdsk") != std::string::npos);
    }
    CHECK(t.writes == 0);
    CHECK_FALSE(t.finished);
}

TEST_CASE("image: an MBR disk is restored onto a smaller disk with its entries moved") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    FixtureDisk disk(PartitionStyle::Mbr);
    ImageBackupOptions o;
    o.disks = {&disk};
    const ImageBackupResult r = run_image_backup(*repo, o);
    const std::uint64_t target_size = disk.l.size - 300 * kMiB;
    SparseTarget target(target_size);
    run_image_restore(*repo, r.snapshot, target, {});
    const Bytes mbr = target.read(0, 512);
    CHECK(le(mbr, 440, 4) == 0x12345678);
    CHECK(mbr[510] == 0x55);
    const Partition& ntfs = disk.l.partitions[1];
    CHECK(le(mbr, 446 + 16 + 8, 4) * 512 == ntfs.offset);
    const std::uint64_t new_length = le(mbr, 446 + 16 + 12, 4) * 512;
    CHECK(new_length == ntfs.length - 299 * kMiB);  // the disk had 1 MiB free at its end
    CHECK(le(mbr, 446 + 32 + 8, 4) * 512 == ntfs.offset + new_length);  // the recovery partition, moved
    CHECK(le(mbr, 446 + 32 + 12, 4) * 512 == disk.l.partitions[2].length);
    CHECK(mbr[446 + 32 + 4] == 0x27);
    CHECK(target.read(ntfs.offset + new_length, static_cast<std::size_t>(disk.l.partitions[2].length)) == disk.recovery);
    check_of(target, ntfs.offset, new_length).run();
}
