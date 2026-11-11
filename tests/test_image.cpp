#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <random>

#include "vaultik/image/disk.hpp"
#include "vaultik/image/extents.hpp"
#include "vaultik/image/image.hpp"
#include "vaultik/image/layout.hpp"
#include "vaultik/image/recovery_media.hpp"
#include "vaultik/image/vhdx.hpp"
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
    std::uint32_t cluster = 4096;

    DiskLayout& layout() override { return l; }
    std::vector<Extent> plan(std::size_t i) override {
        const Partition& p = l.partitions[i];
        if (p.method == ReadMethod::Used) return plan_extents(bitmaps[i], p.length / cluster, cluster, p.length);
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
    const std::vector<Region> g = gap_regions(l);
    REQUIRE(g.size() == 3);
    CHECK(g[0].offset == 0);
    CHECK(g[0].length == kMiB);  // capped
    CHECK(g[1].offset == 12 * kMiB);  // the gap before the logical partition holds its EBR
    CHECK(g[1].length == kMiB);
    CHECK(g[2].offset == 70 * kMiB);
    CHECK(g[2].length == kMiB);
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
