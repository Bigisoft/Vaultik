#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/image/disk_tools.hpp"
#include "vaultik/image/vhdx.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace vaultik;
using namespace vaultik::image;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-disk-tools-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const char* name) const {
        const std::u8string s = (path / name).u8string();
        return std::string(s.begin(), s.end());
    }
};

// A file of random bytes standing in for a disk.
std::string fake_disk(const TempDir& t, std::uint64_t size) {
    const std::string path = t.file("disk.img");
    std::ofstream f(fs::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    Bytes chunk(static_cast<std::size_t>(kMiB));
    for (std::uint64_t at = 0; at < size; at += kMiB) {
        random_bytes(chunk);
        f.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(kMiB, size - at)));
    }
    return path;
}

Bytes read_all(const std::string& path) {
    return os::read_file(path, 256u << 20);
}

bool all_zero(const std::uint8_t* p, std::size_t n) {
    return std::all_of(p, p + n, [](std::uint8_t b) { return b == 0; });
}

// A device in memory whose reads can be made to return other data than was written.
class MemoryDevice : public WipeDevice {
public:
    explicit MemoryDevice(std::size_t size) : data(size, 0xAB) {}
    std::uint64_t size() const override { return data.size(); }
    std::uint32_t block_size() const override { return 512; }
    void write_at(std::uint64_t offset, ByteSpan d) override {
        std::memcpy(data.data() + offset, d.data(), d.size());
        writes += d.size();
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        std::memcpy(dst, data.data() + offset, length);
        if (flip_at >= offset && flip_at < offset + length) dst[flip_at - offset] ^= 1;
    }
    void flush() override { ++flushes; }
    Bytes data;
    std::uint64_t writes = 0;
    int flushes = 0;
    std::uint64_t flip_at = ~0ull;
};

}  // namespace

TEST_CASE("drive cleanser: methods and their passes") {
    CHECK(parse_wipe_method("zeros") == WipeMethod::Zeros);
    CHECK(parse_wipe_method("RANDOM") == WipeMethod::Random);
    CHECK(parse_wipe_method("dod3") == WipeMethod::Dod3);
    CHECK_FALSE(parse_wipe_method("gutmann").has_value());
    CHECK(wipe_passes(WipeMethod::Zeros) == std::vector<bool>{false});
    CHECK(wipe_passes(WipeMethod::Random) == std::vector<bool>{true});
    // DoD 5220.22-M style: random, random, then zeros.
    CHECK(wipe_passes(WipeMethod::Dod3) == std::vector<bool>{true, true, false});
    CHECK(std::string(to_string(WipeMethod::Dod3)) == "dod3");
}

TEST_CASE("drive cleanser: a file used as a disk is overwritten through the disk write path") {
    TempDir t;
    const std::uint64_t size = 9 * kMiB + 512;  // not a whole number of write chunks
    const std::string path = fake_disk(t, size);
    const Bytes before = read_all(path);

    SUBCASE("zeros, verified") {
        auto dev = open_wipe_file(path);
        CHECK(dev->size() == size);
        WipeOptions o;
        o.verify = true;
        std::vector<WipeProgress> seen;
        o.on_progress = [&](const WipeProgress& p) { seen.push_back(p); };
        const WipeResult r = wipe_device(*dev, o);
        dev.reset();
        CHECK(r.verified);
        CHECK(r.bytes == size);
        const Bytes after = read_all(path);
        REQUIRE(after.size() == size);
        CHECK(all_zero(after.data(), after.size()));
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.back().verifying);
        CHECK(seen.back().done == size);
    }
    SUBCASE("random, verified, then no partition table left") {
        auto dev = open_wipe_file(path);
        WipeOptions o;
        o.method = WipeMethod::Random;
        o.verify = true;
        o.clear_partition_table = true;
        const WipeResult r = wipe_device(*dev, o);
        dev.reset();
        CHECK(r.verified);
        const Bytes after = read_all(path);
        REQUIRE(after.size() == size);
        CHECK(all_zero(after.data(), kMiB));
        CHECK(all_zero(after.data() + size - kMiB, kMiB));
        // The middle is new random data: neither the old bytes nor zeros.
        const std::size_t mid = static_cast<std::size_t>(4 * kMiB);
        CHECK(std::memcmp(after.data() + mid, before.data() + mid, 4096) != 0);
        CHECK_FALSE(all_zero(after.data() + mid, 4096));
    }
    SUBCASE("three passes end with zeros") {
        auto dev = open_wipe_file(path);
        WipeOptions o;
        o.method = WipeMethod::Dod3;
        o.verify = true;
        int passes_seen = 0;
        o.on_progress = [&](const WipeProgress& p) {
            CHECK(p.passes == 3);
            passes_seen = std::max(passes_seen, p.pass);
        };
        const WipeResult r = wipe_device(*dev, o);
        dev.reset();
        CHECK(r.bytes == 3 * size);
        CHECK(passes_seen == 3);
        const Bytes after = read_all(path);
        CHECK(all_zero(after.data(), after.size()));
    }
}

TEST_CASE("drive cleanser: the verification notices data the drive did not keep, and a wipe can be cancelled") {
    MemoryDevice dev(static_cast<std::size_t>(6 * kMiB));
    WipeOptions o;
    o.method = WipeMethod::Random;
    o.verify = true;
    dev.flip_at = 5 * kMiB + 17;
    CHECK_THROWS_WITH_AS(wipe_device(dev, o), doctest::Contains("byte 5242897"), Error);

    MemoryDevice other(static_cast<std::size_t>(6 * kMiB));
    WipeOptions c;
    int polls = 0;
    c.should_cancel = [&] { return ++polls > 1; };
    CHECK_THROWS_AS(wipe_device(other, c), Cancelled);
    CHECK(other.writes == 4 * kMiB);  // one block, then it stopped
}

TEST_CASE("drive cleanser: wiping free space leaves the files and removes its own") {
    TempDir t;
    const std::string keep = t.file("keep.txt");
    os::write_file_atomic(keep, ByteSpan(reinterpret_cast<const std::uint8_t*>("hello"), 5));
    FreeSpaceOptions o;
    o.limit = 5 * kMiB + 100;  // stands in for a full drive
    std::uint64_t last = 0;
    o.on_progress = [&](const FreeSpaceProgress& p) { last = p.done; };
    const std::uint64_t written = wipe_free_space(t.file(""), o);
    CHECK(written == o.limit);
    CHECK(last == written);
    // Only the file that was there.
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(t.path)) names.push_back(e.path().filename().string());
    CHECK(names == std::vector<std::string>{"keep.txt"});
    const Bytes back = read_all(keep);
    CHECK(std::string(back.begin(), back.end()) == "hello");

    // A fill file a killed run left behind goes; a file of another name stays. On Linux the fill
    // file is out of the folder while it is written (it goes away also when the program is killed).
    const std::string stale = t.file("vaultik-free-space-0123456789ab-1.tmp");
    const std::string other = t.file("vaultik-free-space-notes.tmp");
    os::write_file_atomic(stale, ByteSpan(reinterpret_cast<const std::uint8_t*>("old"), 3));
    os::write_file_atomic(other, ByteSpan(reinterpret_cast<const std::uint8_t*>("mine"), 4));
    bool seen = false;
    o.on_progress = [&](const FreeSpaceProgress&) {
        for (const auto& e : fs::directory_iterator(t.path))
            if (e.path().filename().string().starts_with(kFillFilePrefix) && e.path().filename().string() != "vaultik-free-space-notes.tmp")
                seen = true;
    };
    CHECK(wipe_free_space(t.file(""), o) == o.limit);
    CHECK_FALSE(os::exists(stale));
    CHECK(os::exists(other));
#ifndef _WIN32
    CHECK_FALSE(seen);
#endif
    os::remove_file(other);
    o.on_progress = nullptr;

    // Three passes write the same files again; cancelling removes them too.
    o.method = WipeMethod::Dod3;
    CHECK(wipe_free_space(t.file(""), o) == 3 * o.limit);
    int polls = 0;
    o.should_cancel = [&] { return ++polls > 1; };
    CHECK_THROWS_AS(wipe_free_space(t.file(""), o), Cancelled);
    names.clear();
    for (const auto& e : fs::directory_iterator(t.path)) names.push_back(e.path().filename().string());
    CHECK(names == std::vector<std::string>{"keep.txt"});
}

TEST_CASE("drive cleanser and new disk: refusals and confirmations") {
    DiskInfo d;
    d.number = 3;
    d.size = 64 * kMiB;
    d.path = "/dev/sdc";
    CHECK(tool_refusal(d, {}, {}).empty());
    DiskInfo sys = d;
    sys.system = true;
    CHECK_FALSE(tool_refusal(sys, {}, {}).empty());
    CHECK_FALSE(tool_refusal(d, {3}, {}).empty());  // the system runs from it
    CHECK(tool_refusal(d, {}, {3}).find("Vaultik") != std::string::npos);
#ifndef _WIN32
    DiskInfo mounted = d;
    DiskVolume v;
    v.name = "/dev/sdc1";
    v.mount = "/media/usb";
    mounted.volumes.push_back(v);
    CHECK(tool_refusal(mounted, {}, {}).find("mounted") != std::string::npos);
    mounted.volumes[0].mount.clear();
    mounted.volumes[0].filesystem = "swap";
    CHECK(tool_refusal(mounted, {}, {}).find("swap") != std::string::npos);
    CHECK(erase_text(d) == "ERASE DISK sdc");
    CHECK(erase_volume_text("/dev/sdc1") == "ERASE sdc1");
#else
    CHECK(erase_text(d) == "ERASE DISK 3");
#endif
    CHECK(erase_volume_text("E:\\") == "ERASE E:");
    CHECK(typed_matches(" erase  e: ", "ERASE E:"));
    CHECK_FALSE(typed_matches("ERASE F:", "ERASE E:"));
    CHECK_FALSE(typed_matches("", ""));

    // The disk this computer runs from is never offered, and never opened for writing.
    const std::vector<DiskInfo> disks = list_tool_disks();
    const std::vector<int> running = system_disks();
    const std::vector<int> keep = protected_tool_disks();
    for (const DiskInfo& disk : disks) {
        if (!disk.system) continue;
        CHECK_FALSE(tool_refusal(disk, running, keep).empty());
        CHECK_THROWS_AS(open_wipe_disk(disk, identity_of(disk), keep), Error);
        NewDiskOptions o;
        CHECK_THROWS_AS(init_disk(disk, identity_of(disk), keep, o), Error);
    }
    DiskInfo none;
    none.number = 99999;
    none.size = kMiB;
    CHECK_THROWS_AS(open_wipe_disk(none, identity_of(none), keep), Error);
#ifdef _WIN32
    // The disks Windows runs and starts from (the EFI system partition's disk too) were found.
    CHECK(std::find(running.begin(), running.end(), kUnknownSystemDisk) == running.end());
    CHECK_FALSE(running.empty());
    for (const DiskInfo& disk : disks)
        if (std::find(running.begin(), running.end(), disk.number) != running.end())
            CHECK_FALSE(tool_refusal(disk, running, keep).empty());
#endif
}

TEST_CASE("drive cleanser: disks of pools, offline disks and system disks that cannot be found are refused") {
    DiskInfo d;
    d.number = 3;
    d.size = 64 * kMiB;
    d.path = "/dev/sdc";
    CHECK(tool_refusal(d, {}, {}).empty());
    // Not known which disks Windows runs and starts from: none at all.
    CHECK_FALSE(tool_refusal(d, {kUnknownSystemDisk}, {}).empty());
    CHECK_FALSE(target_refusal(d, {kUnknownSystemDisk}, {}).empty());
    DiskInfo pool = d;
    pool.dynamic = true;
    CHECK(tool_refusal(pool, {}, {}).find("dynamic") != std::string::npos);
    DiskInfo offline = d;
    offline.offline = true;
    CHECK(tool_refusal(offline, {}, {}).find("online") != std::string::npos);
    DiskInfo spanned = d;
    DiskVolume part;
    part.name = "\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\";
    part.spans = true;
    spanned.volumes.push_back(part);
    CHECK(tool_refusal(spanned, {}, {}).find("several disks") != std::string::npos);
}

TEST_CASE("drive cleanser: system, reserved, recovery, active and pool partitions are not erased on their own") {
    Partition p;
    p.type = std::string(kBasicDataType);
    CHECK(partition_refusal(p, true).empty());
    p.type = std::string(kEfiSystemType);
    CHECK(partition_refusal(p, true).find("EFI") != std::string::npos);
    p.type = std::string(kMsrType);
    CHECK_FALSE(partition_refusal(p, true).empty());
    p.type = std::string(kRecoveryType);
    CHECK(partition_refusal(p, true).find("recovery") != std::string::npos);
    p.type = "af9b60a0-1431-4f62-bc68-3311714a69ad";  // LDM data, any case
    CHECK(pool_partition(p, true));
    CHECK_FALSE(partition_refusal(p, true).empty());
    p.type = std::string(kBasicDataType);
    p.attributes = 1;  // the platform needs it
    CHECK_FALSE(partition_refusal(p, true).empty());
    Partition m;
    m.mbr_type = 0x07;
    CHECK(partition_refusal(m, false).empty());
    m.active = true;
    CHECK(partition_refusal(m, false).find("active") != std::string::npos);
    m.active = false;
    for (const std::uint8_t t : {std::uint8_t{0xEF}, std::uint8_t{0x27}, std::uint8_t{0x42}}) {
        m.mbr_type = t;
        CHECK_FALSE(partition_refusal(m, false).empty());
    }
}

TEST_CASE("drive cleanser: a volume that is not the one listed any more is refused") {
    VolumeTarget v;
    v.name = "E:";
    v.size = 32 * kMiB;
    v.id = "\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
    v.serial = "1A2B3C4D";
    const DiskIdentity id = volume_identity(v);
    CHECK(volume_mismatch(v, id).empty());
    VolumeTarget other = v;
    other.size += 512;
    CHECK(volume_mismatch(other, id).find("size") != std::string::npos);
    other = v;
    other.id = "\\\\?\\Volume{99999999-2222-3333-4444-555555555555}\\";
    CHECK(volume_mismatch(other, id).find("id") != std::string::npos);
    other = v;
    other.serial = "0000AAAA";
    CHECK(volume_mismatch(other, id).find("serial") != std::string::npos);
    other = v;
    other.serial = "1a2b3c4d";  // case does not matter
    CHECK(volume_mismatch(other, id).empty());
    // A drive that is not there (or not the one expected) is never opened for writing.
    VolumeTarget gone;
#ifdef _WIN32
    gone.name = "B:";
#else
    gone.name = "/dev/vaultik-no-such-partition1";
#endif
    CHECK_THROWS_AS(open_wipe_volume(gone, volume_identity(gone)), Error);
}

TEST_CASE("drive cleanser: a fill file the drive keeps compressed is noticed") {
    CHECK_FALSE(fill_compressed(10 * kMiB, 0));  // too little written to tell
    CHECK(fill_compressed(128 * kMiB, 4 * kMiB));
    CHECK_FALSE(fill_compressed(128 * kMiB, 128 * kMiB));
    CHECK_FALSE(fill_compressed(128 * kMiB, 100 * kMiB));
}

TEST_CASE("new disk: where the partitions go") {
    const std::uint64_t disk = 100 * kMiB;
    // GPT, the whole disk: 1 MiB in, the last MiB left for the backup table.
    auto p = plan_partitions(disk, 512, TableStyle::Gpt, {}, false);
    REQUIRE(p.size() == 1);
    CHECK(p[0].offset == kMiB);
    CHECK(p[0].length == disk - 2 * kMiB);
    // Windows puts a 16 MiB reserved partition first.
    p = plan_partitions(disk, 512, TableStyle::Gpt, {}, true);
    REQUIRE(p.size() == 2);
    CHECK(p[0].reserved);
    CHECK(p[0].length == 16 * kMiB);
    CHECK(p[1].offset == 17 * kMiB);
    CHECK(p[1].offset + p[1].length == disk - kMiB);
    // MBR uses the disk to its end.
    p = plan_partitions(disk, 512, TableStyle::Mbr, {}, true);
    REQUIRE(p.size() == 1);
    CHECK(p[0].offset + p[0].length == disk);
    // Sizes, rounded up to whole MiB, then the rest.
    p = plan_partitions(disk, 4096, TableStyle::Mbr, {20 * kMiB + 1, 30 * kMiB, 0}, false);
    REQUIRE(p.size() == 3);
    CHECK(p[0].length == 21 * kMiB);
    CHECK(p[1].offset == 22 * kMiB);
    CHECK(p[2].offset == 52 * kMiB);
    CHECK(p[2].offset + p[2].length == disk);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Mbr, {kMiB, kMiB, kMiB, kMiB, kMiB}, false), Error);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Gpt, {0, kMiB}, false), Error);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Gpt, {200 * kMiB}, false), Error);
    CHECK_THROWS_AS(plan_partitions(3ull << 40, 512, TableStyle::Mbr, {}, false), Error);  // over 2 TiB
    CHECK_NOTHROW(plan_partitions(3ull << 40, 512, TableStyle::Gpt, {}, false));
    // Sizes so large that adding them would wrap around to a small number.
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Gpt, {~0ull}, false), Error);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Gpt, {~0ull - kMiB + 2}, false), Error);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Gpt, {10 * kMiB, ~0ull - 5 * kMiB}, false), Error);
    CHECK_THROWS_AS(plan_partitions(disk, 512, TableStyle::Mbr, {1ull << 63, 1ull << 63}, false), Error);

    p = plan_partitions(disk, 512, TableStyle::Gpt, {}, false);
    CHECK(sfdisk_script(TableStyle::Gpt, p, 512, "ext4") ==
          "label: gpt\nunit: sectors\n\nstart=2048, size=200704, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4\n");
    CHECK(sfdisk_script(TableStyle::Mbr, p, 512, "ntfs").find("type=7\n") != std::string::npos);
    CHECK(partition_device("/dev/sdb", 1) == "/dev/sdb1");
    CHECK(partition_device("/dev/nvme0n1", 2) == "/dev/nvme0n1p2");
    CHECK(partition_device("/dev/loop3", 1) == "/dev/loop3p1");
}

TEST_CASE("new disk: options are checked before anything is written") {
    NewDiskOptions o;
    o.label = "Backups";
    CHECK_NOTHROW(check_init_options(o));
    o.filesystem = "exfat";
    o.label = "TWELVE CHARS";
    CHECK_THROWS_AS(check_init_options(o), Error);  // exFAT: 11 at most
    o.label = "BAD:NAME";
    CHECK_THROWS_AS(check_init_options(o), Error);
    o.label = std::string(33, 'x');
    o.filesystem = "ntfs";
    CHECK_THROWS_AS(check_init_options(o), Error);
    o.label = "bad\nname";
    CHECK_THROWS_AS(check_init_options(o), Error);
    o.label.clear();
    o.filesystem = "fat12";
    CHECK_THROWS_AS(check_init_options(o), Error);
#ifdef _WIN32
    o.filesystem = "ext4";
    CHECK_THROWS_AS(check_init_options(o), Error);
    o.filesystem.clear();
    o.letter = 'C';
    CHECK_THROWS_AS(check_init_options(o), Error);  // in use (and the system drive)
#else
    o.filesystem = "ext4";
    o.label = std::string(17, 'x');
    CHECK_THROWS_AS(check_init_options(o), Error);  // ext4: 16 bytes at most
    o.label.clear();
    o.letter = 'E';
    CHECK_THROWS_AS(check_init_options(o), Error);  // no letters on Linux
#endif
}

// ---- volumes in use (--force-dismount) ---------------------------------------------------------

namespace {

// A volume to lock: free, used by a program (until forced closed), or used by a program that
// opens it again at once (it stays in use).
class FakeLock : public VolumeLock {
public:
    FakeLock(std::string n, bool in_use, bool reopened = false) : name_(std::move(n)), in_use_(in_use), reopened_(reopened) {}
    std::string name() const override { return name_; }
    bool lock() override {
        ++attempts;
        if (fails) throw Error(name_ + " cannot be locked for writing (Windows error 21); nothing was written");
        locked = !in_use_ || (forced > 0 && !reopened_);
        return locked;
    }
    void force_dismount() override { ++forced; }
    bool fails = false;  // the lock fails for another reason than being in use
    int attempts = 0;
    int forced = 0;
    bool locked = false;

private:
    std::string name_;
    bool in_use_;
    bool reopened_;
};

// The vaultik program with `args`; its exit code, with what it printed in `output`.
int run_cli(const std::vector<std::string>& args, std::string& output) {
    std::vector<std::string> argv{VAULTIK_TEST_CLI};
    argv.insert(argv.end(), args.begin(), args.end());
    output.clear();
    return os::run_command(argv, &output);
}

}  // namespace

TEST_CASE("drive cleanser and new disk: a volume in use is forced closed only when asked") {
    const LockRetry quick{3, std::chrono::milliseconds(0)};
    SUBCASE("nothing in use: all locked, nothing forced") {
        FakeLock e("E:", false), f("F:", false);
        lock_volumes({&e, &f}, false, quick);
        CHECK((e.locked && f.locked));
        CHECK(e.forced + f.forced == 0);
    }
    SUBCASE("without the option: every volume in use is named, none is forced") {
        FakeLock e("E:", true), f("F:", false), g("G:", true);
        try {
            lock_volumes({&e, &f, &g}, false, quick);
            FAIL("no VolumesInUse");
        } catch (const VolumesInUse& x) {
            CHECK(x.volumes == std::vector<std::string>{"E:", "G:"});
            CHECK(std::string(x.what()).find("E:, G: are in use") != std::string::npos);
        }
        CHECK(e.forced + f.forced + g.forced == 0);
        CHECK(e.attempts == 1);
        CHECK(g.attempts == 1);
    }
    SUBCASE("with the option: only the volumes in use are forced, then locked") {
        FakeLock e("E:", true), f("F:", false);
        lock_volumes({&e, &f}, true, quick);
        CHECK(e.forced == 1);
        CHECK(f.forced == 0);
        CHECK((e.locked && f.locked));
    }
    SUBCASE("a lock that fails for another reason: that error, not the in-use question, and nothing forced") {
        FakeLock e("E:", true), f("F:", false);
        f.fails = true;
        try {
            lock_volumes({&e, &f}, true, quick);
            FAIL("no error");
        } catch (const VolumesInUse&) {
            FAIL("asked whether to force it");
        } catch (const Error& x) {
            CHECK(std::string(x.what()).find("F: cannot be locked") != std::string::npos);
        }
        CHECK(e.forced + f.forced == 0);
    }
    SUBCASE("still in use after it was forced: an error (not the in-use question again), after the retries") {
        FakeLock e("E:", true, true);
        try {
            lock_volumes({&e}, true, quick);
            FAIL("no error");
        } catch (const VolumesInUse&) {
            FAIL("asked again");
        } catch (const Error& x) {
            CHECK(std::string(x.what()).find("E: is still in use") != std::string::npos);
        }
        CHECK(e.forced == 1);
        CHECK(e.attempts == 1 + quick.tries);
    }
}

TEST_CASE("drive cleanser and new disk: mounted volumes and unmounting them (Linux, faked)") {
    DiskInfo d;
    d.number = 3;
    d.size = 64 * kMiB;
    d.path = "/dev/sdc";
    DiskVolume a;
    a.name = "/dev/sdc1";
    a.mount = "/media/usb";
    DiskVolume swap;
    swap.name = "/dev/sdc2";
    swap.filesystem = "swap";
    DiskVolume plain;
    plain.name = "/dev/sdc3";
    d.volumes = {a, swap, plain};
    CHECK(mounted_volumes(d) == std::vector<std::string>{"/media/usb"});
#ifndef _WIN32
    // Mounted: refused, unless it is to be unmounted with --force-dismount. Swap stays refused.
    d.volumes = {a};
    CHECK(tool_refusal(d, {}, {}).find("--force-dismount") != std::string::npos);
    CHECK(tool_refusal(d, {}, {}, true).empty());
    d.volumes = {a, swap};
    CHECK(tool_refusal(d, {}, {}, true).find("swap") != std::string::npos);
    // The disk the system runs from is refused before anything could be unmounted.
    d.volumes = {a};
    CHECK_FALSE(tool_refusal(d, {3}, {}, true).empty());
    CHECK_FALSE(tool_refusal(d, {}, {3}, true).empty());
#endif

    std::vector<std::string> mounted{"/media/usb", "/media/usb2"};
    std::vector<std::string> unmounted;
    const auto list = [&] { return mounted; };
    const auto ok = [&](const std::string& p, std::string&) {
        unmounted.push_back(p);
        mounted.erase(std::find(mounted.begin(), mounted.end(), p));
        return true;
    };
    unmount_volumes(list, ok);
    CHECK(mounted.empty());
    CHECK(unmounted.size() == 2);

    unmounted.clear();
    unmount_volumes(list, ok);  // nothing mounted: nothing unmounted
    CHECK(unmounted.empty());

    mounted = {"/media/busy"};
    try {
        unmount_volumes(list, [](const std::string&, std::string& why) {
            why = "target is busy";
            return false;
        });
        FAIL("no error");
    } catch (const Error& x) {
        CHECK(std::string(x.what()).find("/media/busy is still in use") != std::string::npos);
        CHECK(std::string(x.what()).find("target is busy") != std::string::npos);
    }
    // An unmount that reports success but leaves it mounted does not go round forever.
    CHECK_THROWS_AS(unmount_volumes(list, [](const std::string&, std::string&) { return true; }), Error);
}

#ifndef _WIN32
TEST_CASE("drive cleanser and new disk: another drive mounted over the one to unmount is never unmounted (Linux)") {
    const auto mount = [](unsigned major, unsigned minor, const std::string& point, const std::string& source) {
        os::mounts::Mount m;
        m.major = major;
        m.minor = minor;
        m.point = point;
        m.source = source;
        m.type = "ext4";
        return m;
    };
    const std::vector<std::pair<unsigned, unsigned>> own{{8, 16}, {8, 17}, {8, 18}};
    std::vector<os::mounts::Mount> mounts{mount(259, 2, "/", "/dev/nvme0n1p2"), mount(8, 17, "/media/usb", "/dev/sdb1")};
    CHECK(other_mount_on(mounts, "/media/usb", own).empty());  // its own: unmounted
    CHECK(other_mount_on(mounts, "/media/gone", own).empty());   // nothing there any more
    // Another drive mounted over it later: umount would take that one.
    mounts.push_back(mount(8, 33, "/media/usb", "/dev/sdc1"));
    CHECK(other_mount_on(mounts, "/media/usb", own) == "/dev/sdc1 (8:33)");
    // The drive's own second partition on top: still its own.
    mounts.push_back(mount(8, 18, "/media/usb", "/dev/sdb2"));
    CHECK(other_mount_on(mounts, "/media/usb", own).empty());
    // A partition alone: the disk's other partition over it is not it.
    CHECK(other_mount_on(mounts, "/media/usb", {{8, 17}}) == "/dev/sdb2 (8:18)");
}
#endif

TEST_CASE("disk tools: the --force-dismount option of the command-line program") {
    std::string output;
    CHECK(run_cli({"--help"}, output) == 0);
    CHECK(output.find("--force-dismount") != std::string::npos);
    // A flag: it takes no value.
    CHECK(run_cli({"disk", "wipe", "99999", "--force-dismount=yes"}, output) != 0);
    CHECK(output.find("unknown option") != std::string::npos);
    // Known to both disk wipe and disk init; the disk does not exist (or no rights): stops there.
    for (const std::string sub : {"wipe", "init"}) {
        CAPTURE(sub);
        CAPTURE(output);
        CHECK(run_cli({"disk", sub, "99999", "--force-dismount", "--confirm", "ERASE DISK 99999"}, output) == 1);
        CHECK(output.find("unknown option") == std::string::npos);
        CHECK(output.find("error:") != std::string::npos);
    }
}

// ---- on a real (virtual) disk, only where it is safe ---------------------------------------------

#ifdef _WIN32
TEST_CASE("disk tools on a virtual disk (Windows, administrator, VAULTIK_TEST_VIRTUAL_DISKS=1 only)") {
    // Creates its own small VHDX, attaches it, and works on that disk only: never on a disk this
    // test did not make. Skipped on a developer's computer unless asked for.
    if (os::get_env("VAULTIK_TEST_VIRTUAL_DISKS") != "1" || !os::process_elevated()) {
        MESSAGE("skipped: needs administrator rights and VAULTIK_TEST_VIRTUAL_DISKS=1");
        return;
    }
    TempDir t;
    const std::string path = t.file("tools.vhdx");
    const std::uint64_t size = 256 * kMiB;
    {
        auto file = create_random_file(path);
        VhdxWriter w(*file, size);
        w.finish();
    }
    auto attached = attach_vhdx(path, false);
    const int number = attached->number();
    REQUIRE(number >= 0);
    std::vector<DiskInfo> disks = list_tool_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    // The disk at that number is the one made here: its size, blank.
    REQUIRE(it->size == size);
    REQUIRE(it->style == "RAW");
    REQUIRE_FALSE(it->system);
    const DiskInfo disk = *it;
    const std::vector<int> keep = protected_tool_disks();
    CHECK(tool_refusal(disk, system_disks(), keep).empty());

    NewDiskOptions o;
    o.label = "VaultikTest";
    const NewDiskResult r = init_disk(disk, identity_of(disk), keep, o);
    REQUIRE(r.volumes.size() == 1);
    const std::string root = r.volumes[0];
    CHECK(os::is_directory(root));
    os::write_file_atomic(os::join(root, "note.txt"), ByteSpan(reinterpret_cast<const std::uint8_t*>("x"), 1));
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    CHECK(it->style == "GPT");
    bool found = false;
    for (const DiskVolume& v : it->volumes)
        if (v.label == "VaultikTest" && v.filesystem == "NTFS") found = true;
    CHECK(found);

    // Its free space, then the volume itself.
    FreeSpaceOptions f;
    CHECK(wipe_free_space(root, f) > 100 * kMiB);
    CHECK(os::exists(os::join(root, "note.txt")));
    const VolumeTarget vol = find_wipe_volume(root);
    CHECK(vol.disk == number);
    CHECK(erase_volume_text(vol.name) == "ERASE " + root.substr(0, 2));
    CHECK(vol.id.starts_with("\\\\?\\Volume{"));
    CHECK_FALSE(vol.serial.empty());
    // Not the drive listed: nothing is written. The one listed: erased.
    DiskIdentity wrong = volume_identity(vol);
    wrong.serial = vol.serial == "00000001" ? "00000002" : "00000001";
    CHECK_THROWS_AS(open_wipe_volume(vol, wrong), Error);
    CHECK(os::exists(os::join(root, "note.txt")));

    // A file on the drive held open (as another program would): the drive is in use. Without
    // --force-dismount it is refused, named, and nothing is written; with it, it is erased.
    const std::string letter = root.substr(0, 2);
    {
        auto held = std::make_unique<std::ifstream>(fs::path(std::u8string(root.begin(), root.end())) / "note.txt", std::ios::binary);
        REQUIRE(held->is_open());
        try {
            open_wipe_volume(vol, volume_identity(vol));
            FAIL("the drive in use was opened for writing");
        } catch (const VolumesInUse& e) {
            CHECK(e.volumes == std::vector<std::string>{letter});
        }
        CHECK(read_all(os::join(root, "note.txt")) == Bytes{'x'});
        const DiskIdentity id = volume_identity(vol);
        const std::vector<std::string> args{"disk", "wipe", "--volume", letter, "--confirm", erase_volume_text(letter),
                                            "--expect-size", std::to_string(id.size), "--expect-serial", id.serial,
                                            "--expect-id", id.id, "--json"};
        std::string output;
        CHECK(run_cli(args, output) == 6);
        CAPTURE(output);
        CHECK(output.find("\"event\":\"in_use\"") != std::string::npos);
        CHECK(output.find("\"status\":\"in_use\"") != std::string::npos);
        CHECK(output.find(letter) != std::string::npos);
        CHECK(read_all(os::join(root, "note.txt")) == Bytes{'x'});
        std::vector<std::string> forced = args;
        forced.push_back("--force-dismount");
        CHECK(run_cli(forced, output) == 0);
        CHECK(output.find("\"status\":\"success\"") != std::string::npos);
        held.reset();  // its handle stopped working when the drive was forced closed
    }

    // The whole disk, set up again, with a file on it held open: refused without the option,
    // then forced closed, erased, checked, and left without a partition table.
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    const NewDiskResult again = init_disk(*it, identity_of(*it), keep, o);
    REQUIRE(again.volumes.size() == 1);
    const std::string root2 = again.volumes[0];
    os::write_file_atomic(os::join(root2, "note.txt"), ByteSpan(reinterpret_cast<const std::uint8_t*>("y"), 1));
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    const DiskInfo now = *it;
    {
        auto held = std::make_unique<std::ifstream>(fs::path(std::u8string(root2.begin(), root2.end())) / "note.txt", std::ios::binary);
        REQUIRE(held->is_open());
        try {
            open_wipe_disk(now, identity_of(now), keep);
            FAIL("the disk in use was opened for writing");
        } catch (const VolumesInUse& e) {
            CHECK(e.volumes == std::vector<std::string>{root2.substr(0, 2)});
        }
        NewDiskOptions not_forced = o;
        CHECK_THROWS_AS(init_disk(now, identity_of(now), keep, not_forced), VolumesInUse);
        CHECK(read_all(os::join(root2, "note.txt")) == Bytes{'y'});
        auto dev = open_wipe_disk(now, identity_of(now), keep, true);
        WipeOptions w;
        w.verify = true;
        w.clear_partition_table = true;
        CHECK(wipe_device(*dev, w).verified);
        dev->forget_partition_table();
        held.reset();
    }
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    CHECK(it->style == "RAW");
    CHECK(it->partitions == 0);

    // Set up again while a drive on it is in use: refused without the option (the engine names
    // the drive in use, also in the file the app reads, and nothing is written); with it, the
    // drive is forced closed and the disk gets the new layout (MBR) and file system (exFAT).
    {
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        const NewDiskResult ntfs = init_disk(*it, identity_of(*it), keep, o);
        REQUIRE(ntfs.volumes.size() == 1);
        const std::string root3 = ntfs.volumes[0];
        os::write_file_atomic(os::join(root3, "note.txt"), ByteSpan(reinterpret_cast<const std::uint8_t*>("z"), 1));
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        const DiskInfo before = *it;
        REQUIRE(before.style == "GPT");
        const std::string in_use_file = t.file("in-use.txt");
        std::vector<std::string> args{"disk", "init", std::to_string(number), "--mbr", "--fs", "exfat", "--label", "VTForced",
                                      "--confirm", erase_text(before), "--expect-size", std::to_string(before.size),
                                      "--in-use-file", in_use_file};
        if (!before.serial.empty()) args.insert(args.end(), {"--expect-serial", before.serial});
        if (!before.id.empty()) args.insert(args.end(), {"--expect-id", before.id});
        auto held = std::make_unique<std::ifstream>(fs::path(std::u8string(root3.begin(), root3.end())) / "note.txt", std::ios::binary);
        REQUIRE(held->is_open());
        std::string output;
        CHECK(run_cli(args, output) == 6);
        CAPTURE(output);
        CHECK(read_all(os::join(root3, "note.txt")) == Bytes{'z'});
        const Bytes named = read_all(in_use_file);
        CHECK(std::string(named.begin(), named.end()) == root3.substr(0, 2) + "\n");
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        CHECK(it->style == "GPT");
        CHECK(it->id == before.id);
        std::vector<std::string> forced = args;
        forced.push_back("--force-dismount");
        CHECK(run_cli(forced, output) == 0);
        held.reset();
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        CHECK(it->style == "MBR");
        CHECK(it->id != before.id);
        bool exfat = false;
        for (const DiskVolume& v : it->volumes)
            if (v.label == "VTForced" && v.filesystem == "exFAT") exfat = true;
        CHECK(exfat);
        MESSAGE("ran: disk init --force-dismount on a disk with a drive in use");
    }

    // The forced erase of a whole disk whose drive is exFAT, then FAT32, with a file on it held
    // open: after a forced dismount the drive is locked only through a new handle (these file
    // systems might grant a lock on the dismounted one), and the disk ends up blank.
    const auto forced_wipe = [&](const std::string& filesystem) {
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        const DiskInfo on = *it;
        std::string drive;
        for (const DiskVolume& v : on.volumes)
            if (v.filesystem == filesystem && v.mount.size() == 3) drive = v.mount;
        REQUIRE_FALSE(drive.empty());
        os::write_file_atomic(os::join(drive, "note.txt"), ByteSpan(reinterpret_cast<const std::uint8_t*>("w"), 1));
        auto held = std::make_unique<std::ifstream>(fs::path(std::u8string(drive.begin(), drive.end())) / "note.txt", std::ios::binary);
        REQUIRE(held->is_open());
        try {
            open_wipe_disk(on, identity_of(on), keep);
            FAIL("the disk in use was opened for writing");
        } catch (const VolumesInUse& e) {
            CHECK(e.volumes == std::vector<std::string>{drive.substr(0, 2)});
        }
        CHECK(read_all(os::join(drive, "note.txt")) == Bytes{'w'});
        {
            auto dev = open_wipe_disk(on, identity_of(on), keep, true);
            WipeOptions w;
            w.verify = true;
            w.clear_partition_table = true;
            CHECK(wipe_device(*dev, w).verified);
            dev->forget_partition_table();
        }
        held.reset();
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        CHECK(it->style == "RAW");
        CHECK(it->partitions == 0);
        MESSAGE("ran: forced erase of a disk with an " << filesystem << " drive in use");
    };
    forced_wipe("exFAT");
    {
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        NewDiskOptions fat = o;
        fat.filesystem = "exfat";
        fat.style = TableStyle::Mbr;
        const NewDiskResult r32 = init_disk(*it, identity_of(*it), keep, fat);
        REQUIRE(r32.volumes.size() == 1);
        std::string out;
        const std::string format = os::get_env("SystemRoot") + "\\System32\\format.com";
        REQUIRE(os::run_command({format, r32.volumes[0].substr(0, 2), "/FS:FAT32", "/Q", "/V:VTFAT", "/Y"}, &out) == 0);
    }
    forced_wipe("FAT32");
    attached.reset();
}
#else
TEST_CASE("disk tools on a loop device (Linux, root, VAULTIK_TEST_VIRTUAL_DISKS=1 only)") {
    // Attaches a file of its own as a loop device and works on that device only.
    if (os::get_env("VAULTIK_TEST_VIRTUAL_DISKS") != "1" || ::geteuid() != 0 || !os::exists("/sbin/losetup")) {
        MESSAGE("skipped: needs root, losetup and VAULTIK_TEST_VIRTUAL_DISKS=1");
        return;
    }
    TempDir t;
    const std::string file = t.file("loop.img");
    {
        std::ofstream f(fs::path(std::u8string(file.begin(), file.end())), std::ios::binary);
        f.seekp(static_cast<std::streamoff>(128 * kMiB - 1));
        f.put('\0');
    }
    std::string output;
    REQUIRE(os::run_command({"/sbin/losetup", "--find", "--show", "--partscan", file}, &output) == 0);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    const std::string dev = output;
    REQUIRE(dev.starts_with("/dev/loop"));
    struct Detach {
        std::string dev;
        ~Detach() {
            std::string ignored;
            os::run_command({"/sbin/losetup", "--detach", dev}, &ignored);
        }
    } detach{dev};
    std::vector<DiskInfo> disks = list_tool_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.path == dev; });
    REQUIRE(it != disks.end());
    REQUIRE(it->size == 128 * kMiB);
    const DiskInfo disk = *it;
    const std::vector<int> keep = protected_tool_disks();
    CHECK(tool_refusal(disk, system_disks(), keep).empty());

    NewDiskOptions o;
    o.label = "vtest";
    const NewDiskResult r = init_disk(disk, identity_of(disk), keep, o);
    REQUIRE(r.volumes.size() == 1);
    CHECK(r.volumes[0] == dev + "p1");

    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.path == dev; });
    REQUIRE(it != disks.end());
    CHECK(it->partitions == 1);
    const DiskInfo now = *it;
    // Mounted, with a file open on it: in use. Without --force-dismount nothing is unmounted;
    // with it, a file system still busy stays mounted (never a lazy unmount) and stops it; once
    // the file is closed it is unmounted and erased.
    const std::string mnt = t.file("mnt");
    fs::create_directories(fs::path(std::u8string(mnt.begin(), mnt.end())));
    REQUIRE(os::run_command({"/bin/mount", dev + "p1", mnt}, &output) == 0);
    struct Unmount {
        std::string mnt;
        ~Unmount() {
            std::string ignored;
            os::run_command({"/bin/umount", mnt}, &ignored);
        }
    } unmount_at_end{mnt};
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.path == dev; });
    REQUIRE(it != disks.end());
    CHECK(mounted_volumes(*it) == std::vector<std::string>{mnt});
    const DiskInfo mounted = *it;
    {
        std::ofstream held(fs::path(std::u8string(mnt.begin(), mnt.end())) / "open.txt");
        REQUIRE(held.is_open());
        CHECK_THROWS_AS(open_wipe_disk(mounted, identity_of(mounted), keep), VolumesInUse);
        try {
            open_wipe_disk(mounted, identity_of(mounted), keep, true);
            FAIL("a busy file system was unmounted");
        } catch (const VolumesInUse&) {
            FAIL("asked again");
        } catch (const Error& e) {
            CHECK(std::string(e.what()).find(mnt) != std::string::npos);
        }
        CHECK(os::exists(os::join(mnt, "open.txt")));
    }
    {
        auto w = open_wipe_disk(mounted, identity_of(mounted), keep, true);
        CHECK_FALSE(os::exists(os::join(mnt, "open.txt")));
    }
    {
        auto w = open_wipe_disk(now, identity_of(now), keep);
        WipeOptions wo;
        wo.method = WipeMethod::Random;
        wo.verify = true;
        wo.clear_partition_table = true;
        CHECK(wipe_device(*w, wo).verified);
        w->forget_partition_table();
    }
    const Bytes start = os::read_file(file, 256u << 20);
    CHECK(all_zero(start.data(), kMiB));
    CHECK(all_zero(start.data() + start.size() - kMiB, kMiB));
}
#endif

namespace {

// A device that throws away what is written to it (and reads back zeros): how fast the erase
// itself goes when the drive is never what holds it back.
class DiscardDevice : public WipeDevice {
public:
    explicit DiscardDevice(std::uint64_t size) : size_(size) {}
    std::uint64_t size() const override { return size_; }
    std::uint32_t block_size() const override { return 512; }
    void write_at(std::uint64_t, ByteSpan) override {}
    void read_at(std::uint64_t, std::uint8_t* dst, std::size_t length) override { std::memset(dst, 0, length); }
    void flush() override {}

private:
    std::uint64_t size_;
};

double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0 ? static_cast<double>(bytes) / static_cast<double>(kMiB) / seconds : 0;
}

// Erases `dev` with zeros, then random data, then zeros read back, and logs the speed of each.
void measure_wipe(WipeDevice& dev, const std::string& what) {
    const std::uint64_t size = dev.size();
    WipeOptions zeros;
    const WipeResult z = wipe_device(dev, zeros);
    WipeOptions random;
    random.method = WipeMethod::Random;
    const WipeResult r = wipe_device(dev, random);
    WipeOptions verified;
    verified.verify = true;
    const WipeResult v = wipe_device(dev, verified);
    CHECK(v.verified);
    const double verify_seconds = v.seconds - z.seconds;
    MESSAGE("throughput, " << what << " (" << size / kMiB << " MiB): zeros " << static_cast<int>(mib_per_second(size, z.seconds))
                           << " MiB/s, random " << static_cast<int>(mib_per_second(size, r.seconds)) << " MiB/s, verify about "
                           << static_cast<int>(mib_per_second(size, verify_seconds > 0 ? verify_seconds : 0)) << " MiB/s");
}

}  // namespace

TEST_CASE("drive cleanser: throughput (measured and logged, not checked)") {
    {
        DiscardDevice nowhere(1024 * kMiB);
        WipeOptions o;
        o.method = WipeMethod::Random;
        const WipeResult r = wipe_device(nowhere, o);
        MESSAGE("throughput, random data to a device that discards it (the generator's limit): "
                << static_cast<int>(mib_per_second(nowhere.size(), r.seconds)) << " MiB/s");
    }
    {
        TempDir t;
        const std::string path = fake_disk(t, 512 * kMiB);
        auto dev = open_wipe_file(path);
        measure_wipe(*dev, "a file");
    }
#ifdef _WIN32
    // A virtual disk of its own, attached and erased as a whole disk (the path a USB drive takes).
    if (os::get_env("VAULTIK_TEST_VIRTUAL_DISKS") != "1" || !os::process_elevated()) return;
    TempDir t;
    const std::string path = t.file("speed.vhdx");
    const std::uint64_t size = 1024 * kMiB;
    {
        auto file = create_random_file(path);
        VhdxWriter w(*file, size);
        w.finish();
    }
    auto attached = attach_vhdx(path, false);
    const int number = attached->number();
    REQUIRE(number >= 0);
    const std::vector<DiskInfo> disks = list_tool_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
    REQUIRE(it != disks.end());
    REQUIRE(it->size == size);
    REQUIRE(it->style == "RAW");
    {
        auto dev = open_wipe_disk(*it, identity_of(*it), protected_tool_disks());
        measure_wipe(*dev, "a virtual disk");
    }
    attached.reset();
#endif
}
