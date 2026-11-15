#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/image/disk_tools.hpp"
#include "vaultik/image/vhdx.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/task_events.hpp"

#include "engine_channel.hpp"

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

    MemoryDevice other(3 * kWipeChunk);
    WipeOptions c;
    int polls = 0;
    c.should_cancel = [&] { return ++polls > 1; };
    CHECK_THROWS_AS(wipe_device(other, c), Cancelled);
    CHECK(other.writes <= kWipeChunk);  // at most the one block started, then it stopped
}

namespace {

// A device written from several threads at once, as a disk is. It keeps what was written (unless
// `keep` is false), every write's offset and length, how many writes ran at the same time and
// how many started after `stopped` was set (by the device when a write failed, or by the test
// when it cancelled). It can fail the write that covers `fail_at`, return a corrupted sector at
// `corrupt_at`, and make every write take `delay` (cut short by cancel()).
class ParallelDevice : public WipeDevice {
public:
    explicit ParallelDevice(std::uint64_t size, bool keep = true) : size_(size), data(keep ? size : 0, 0xAB) {}
    std::uint64_t size() const override { return size_; }
    std::uint32_t block_size() const override { return 512; }
    bool parallel_io() const override { return true; }
    void write_at(std::uint64_t offset, ByteSpan d) override {
        std::unique_lock<std::mutex> lock(m);
        ++active;
        max_active = std::max(max_active, active);
        if (stopped) ++late_starts;
        writes.emplace_back(offset, d.size());
        struct Leave {
            ParallelDevice& dev;
            ~Leave() { --dev.active; }
        } leave{*this};
        if (delay.count() > 0) {
            cv.wait_for(lock, delay, [&] { return cancelled; });
            if (cancelled) throw Error("writing the test device at byte " + std::to_string(offset) + ": cancelled");
        }
        if (fail_at >= offset && fail_at < offset + d.size()) {
            stopped = true;
            throw Error("writing the test device at byte " + std::to_string(offset) + ": a broken sector");
        }
        lock.unlock();
        if (!data.empty()) std::memcpy(data.data() + offset, d.data(), d.size());
        std::this_thread::sleep_for(std::chrono::microseconds(200));  // long enough for others to start meanwhile
        lock.lock();
        written += d.size();
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        std::memcpy(dst, data.data() + offset, length);
        for (std::uint64_t at = corrupt_at; at < corrupt_at + 512; ++at)
            if (at >= offset && at < offset + length) dst[at - offset] ^= 0xFF;
    }
    void flush() override {
        std::lock_guard<std::mutex> lock(m);
        ++flushes;
    }
    void cancel() override {
        std::lock_guard<std::mutex> lock(m);
        cancelled = true;
        cv.notify_all();
    }

    std::uint64_t size_;
    Bytes data;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::uint64_t, std::size_t>> writes;
    std::uint64_t written = 0;  // bytes of the writes that completed
    int active = 0;
    int max_active = 0;
    int late_starts = 0;
    int flushes = 0;
    bool stopped = false;
    bool cancelled = false;
    std::uint64_t fail_at = ~0ull;
    std::uint64_t corrupt_at = ~0ull;
    std::chrono::milliseconds delay{0};
};

}  // namespace

TEST_CASE("drive cleanser: several writes at once put the pattern on every byte, the last part too") {
    // Not a whole number of writes: the last one is shorter.
    const std::uint64_t size = 5 * kWipeChunk + 3 * 512;
    const std::uint64_t chunks = 6;

    SUBCASE("three passes, verified: every write once per pass, at its place, and zeros everywhere") {
        ParallelDevice dev(size);
        WipeOptions o;
        o.method = WipeMethod::Dod3;
        o.verify = true;
        bool counted_ahead = false;
        o.on_progress = [&](const WipeProgress& p) {
            std::lock_guard<std::mutex> lock(dev.m);
            // Only bytes whose write completed (the device counts them before the erase does).
            if (!p.verifying && p.pass == 1 && p.done > dev.written) counted_ahead = true;
        };
        const WipeResult r = wipe_device(dev, o);
        CHECK(r.verified);
        CHECK(r.bytes == 3 * size);
        CHECK_FALSE(counted_ahead);
        CHECK(dev.max_active > 1);
        CHECK(dev.max_active <= static_cast<int>(kWipeInFlight));
        CHECK(dev.flushes == 3);
        std::vector<std::pair<std::uint64_t, std::size_t>> writes = dev.writes;
        std::sort(writes.begin(), writes.end());
        REQUIRE(writes.size() == 3 * chunks);
        for (std::uint64_t k = 0; k < chunks; ++k) {
            const std::uint64_t at = k * kWipeChunk;
            const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kWipeChunk, size - at));
            for (int pass = 0; pass < 3; ++pass) {
                CAPTURE(k);
                CHECK(writes[k * 3 + pass] == std::make_pair(at, length));
            }
        }
        CHECK(writes.back().second == 3 * 512);
        CHECK(all_zero(dev.data.data(), dev.data.size()));
    }
    SUBCASE("random, verified: no sector keeps its old bytes or is left zero") {
        ParallelDevice dev(size);
        WipeOptions o;
        o.method = WipeMethod::Random;
        o.verify = true;
        CHECK(wipe_device(dev, o).verified);  // read back in full and compared with the stream
        CHECK(dev.max_active > 1);
        bool old = false;
        bool zero = false;
        for (std::uint64_t at = 0; at < size; at += 512) {
            const std::uint8_t* s = dev.data.data() + at;
            old = old || std::all_of(s, s + 512, [](std::uint8_t b) { return b == 0xAB; });
            zero = zero || all_zero(s, 512);
        }
        CHECK_FALSE(old);
        CHECK_FALSE(zero);
    }
}

TEST_CASE("drive cleanser: a failed write stops the erase, and its place is told") {
    ParallelDevice dev(64 * kWipeChunk, false);
    const std::uint64_t bad = 10 * kWipeChunk + 4096;
    dev.fail_at = bad;
    WipeOptions o;
    o.method = WipeMethod::Random;
    o.verify = true;
    CHECK_THROWS_WITH_AS(wipe_device(dev, o), doctest::Contains(("at byte " + std::to_string(10 * kWipeChunk)).c_str()), Error);
    // No write was started after it but those already on their way, none reached the end of the
    // drive, and the pass was not taken as done.
    CHECK(dev.late_starts < static_cast<int>(kWipeInFlight));
    std::uint64_t last = 0;
    for (const auto& [at, length] : dev.writes) last = std::max(last, at);
    CHECK(last < bad + (kWipeInFlight + 2) * kWipeChunk);
    CHECK(dev.writes.size() < 20);
    CHECK(dev.flushes == 0);
    CHECK(dev.active == 0);
}

TEST_CASE("drive cleanser: a cancel stops at once, cutting short the writes still running") {
    ParallelDevice dev(256 * kWipeChunk, false);
    dev.delay = std::chrono::milliseconds(2000);  // a very slow drive: only a cancel ends a write early
    WipeOptions o;
    std::chrono::steady_clock::time_point asked{};
    o.should_cancel = [&] {
        std::lock_guard<std::mutex> lock(dev.m);
        if (dev.writes.size() < kWipeInFlight) return false;  // every thread has a write running
        if (!dev.stopped) asked = std::chrono::steady_clock::now();
        dev.stopped = true;
        return true;
    };
    CHECK_THROWS_AS(wipe_device(dev, o), Cancelled);
    const auto took = std::chrono::steady_clock::now() - asked;
    CHECK(took < std::chrono::milliseconds(1500));  // not the 2 seconds the writes would take
    std::lock_guard<std::mutex> lock(dev.m);
    CHECK(dev.cancelled);
    CHECK(dev.active == 0);  // nothing runs any more when it returns
    CHECK(dev.written == 0);
    CHECK(dev.late_starts == 0);
    CHECK(dev.writes.size() == kWipeInFlight);
    CHECK(dev.flushes == 0);
}

namespace {

// A drive whose cancel (like CancelIoEx) cuts short only the writes it already has: a write
// that reaches it after a cancel runs in full (`delay`) unless it is cancelled again. Each write
// but the first takes `before` before it reaches the drive, as a thread that took its slot
// just before the stop and had not issued its write yet.
class LateDevice : public WipeDevice {
public:
    explicit LateDevice(std::uint64_t size) : size_(size) {}
    std::uint64_t size() const override { return size_; }
    std::uint32_t block_size() const override { return 512; }
    bool parallel_io() const override { return true; }
    void write_at(std::uint64_t offset, ByteSpan) override {
        std::unique_lock<std::mutex> lock(m);
        const bool first = entered++ == 0;
        if (!first) {
            lock.unlock();
            std::this_thread::sleep_for(before);
            lock.lock();
        }
        ++issued;
        const int generation = cancels;
        if (!cv.wait_for(lock, delay, [&] { return cancels != generation; })) {
            ++completed;
            return;
        }
        throw Error("writing the test device at byte " + std::to_string(offset) + ": cancelled");
    }
    void read_at(std::uint64_t, std::uint8_t*, std::size_t) override {}
    void flush() override {}
    void cancel() override {
        std::lock_guard<std::mutex> lock(m);
        ++cancels;
        cv.notify_all();
    }

    std::uint64_t size_;
    std::mutex m;
    std::condition_variable cv;
    int entered = 0;
    int issued = 0;     // writes that reached the drive
    int completed = 0;  // ran in full
    int cancels = 0;
    std::chrono::milliseconds before{300};
    std::chrono::milliseconds delay{3000};
};

}  // namespace

TEST_CASE("drive cleanser: a write that reaches the drive just after a cancel is cut short too") {
    LateDevice dev(64 * kWipeChunk);
    WipeOptions o;
    o.should_cancel = [&] {
        std::lock_guard<std::mutex> lock(dev.m);
        // The first write runs; the others have their slot but did not reach the drive yet.
        return dev.issued >= 1;
    };
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(wipe_device(dev, o), Cancelled);
    const auto took = std::chrono::steady_clock::now() - start;
    // Not the 3 seconds of a whole write: those that came late were cancelled again.
    CHECK(took < std::chrono::milliseconds(2000));
    std::lock_guard<std::mutex> lock(dev.m);
    CHECK(dev.completed == 0);
    CHECK(dev.issued <= static_cast<int>(kWipeInFlight));
}

TEST_CASE("drive cleanser: no write starts once the erase stopped") {
    // A drive whose first write fails while the others still run: the threads that had not
    // reached the drive yet when it failed start nothing.
    struct Failing : WipeDevice {
        std::atomic<int> calls{0};
        std::atomic<bool> failed{false};
        std::atomic<int> after_failure{0};
        std::uint64_t size() const override { return 64 * kWipeChunk; }
        std::uint32_t block_size() const override { return 512; }
        bool parallel_io() const override { return true; }
        void write_at(std::uint64_t offset, ByteSpan) override {
            if (calls++ == 0) {
                // Waits until every other thread has taken a slot, then fails.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                failed = true;
                throw Error("writing the test device at byte " + std::to_string(offset) + ": a broken sector");
            }
            if (failed) ++after_failure;
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        void read_at(std::uint64_t, std::uint8_t*, std::size_t) override {}
        void flush() override {}
    } failing;
    WipeOptions o;
    CHECK_THROWS_WITH_AS(wipe_device(failing, o), doctest::Contains("a broken sector"), Error);
    // The writes running when it failed end by themselves (this device cannot cut them short);
    // what was queued meanwhile never started.
    CHECK(failing.calls.load() <= static_cast<int>(kWipeInFlight));
    CHECK(failing.after_failure.load() == 0);
}

TEST_CASE("drive cleanser: once a write failed, no thread takes another one, also when the others return at once") {
    // Every thread has its write on the drive and more are queued when the first one fails. The
    // others end only when the erase cuts them short (it stopped by then), and return at once:
    // a thread free again must not take a queued write. Nothing depends on timing here.
    struct Gate : WipeDevice {
        std::mutex m;
        std::condition_variable cv;
        int calls = 0;
        int after_stop = 0;  // writes that reached the drive after cancel()
        bool cancelled = false;
        bool all_running = false;
        std::uint64_t size() const override { return 64 * kWipeChunk; }
        std::uint32_t block_size() const override { return 512; }
        bool parallel_io() const override { return true; }
        void write_at(std::uint64_t offset, ByteSpan) override {
            std::unique_lock<std::mutex> lock(m);
            if (cancelled) ++after_stop;
            const bool first = calls++ == 0;
            cv.notify_all();
            if (first) {
                // Fails once every thread has its write running.
                all_running = cv.wait_for(lock, std::chrono::seconds(20), [&] { return calls == static_cast<int>(kWipeInFlight); });
                throw Error("writing the test device at byte " + std::to_string(offset) + ": a broken sector");
            }
            cv.wait(lock, [&] { return cancelled; });
            // Returns as if it completed: the thread is free at once.
        }
        void read_at(std::uint64_t, std::uint8_t*, std::size_t) override {}
        void flush() override {}
        void cancel() override {
            std::lock_guard<std::mutex> lock(m);
            cancelled = true;
            cv.notify_all();
        }
    } gate;
    WipeOptions o;
    CHECK_THROWS_WITH_AS(wipe_device(gate, o), doctest::Contains("a broken sector"), Error);
    std::lock_guard<std::mutex> lock(gate.m);
    CHECK(gate.all_running);
    CHECK(gate.cancelled);
    CHECK(gate.calls == static_cast<int>(kWipeInFlight));  // the writes queued behind them never started
    CHECK(gate.after_stop == 0);
}

TEST_CASE("drive cleanser: the verification reading several parts at once finds one bad sector") {
    const std::uint64_t size = 4 * kWipeChunk + 512;
    for (const WipeMethod method : {WipeMethod::Zeros, WipeMethod::Random}) {
        ParallelDevice dev(size);
        dev.corrupt_at = 2 * kWipeChunk + 3 * 4096;
        WipeOptions o;
        o.method = method;
        o.verify = true;
        CAPTURE(to_string(method));
        CHECK_THROWS_WITH_AS(wipe_device(dev, o), doctest::Contains(("at byte " + std::to_string(dev.corrupt_at)).c_str()), Error);
    }
    ParallelDevice last(size);
    last.corrupt_at = size - 512;  // the last sector, in the short last part
    WipeOptions o;
    o.verify = true;
    CHECK_THROWS_WITH_AS(wipe_device(last, o), doctest::Contains(("at byte " + std::to_string(size - 512)).c_str()), Error);
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

// ---- erasing a whole disk, then setting it up again ----------------------------------------------

namespace {

// A device of the test (a MemoryDevice it keeps), as erase_and_set_up opens and closes it.
class BorrowedDevice : public WipeDevice {
public:
    explicit BorrowedDevice(MemoryDevice& d, bool fail_writes = false) : d_(d), fail_(fail_writes) {}
    std::uint64_t size() const override { return d_.size(); }
    std::uint32_t block_size() const override { return d_.block_size(); }
    void write_at(std::uint64_t offset, ByteSpan data) override {
        if (fail_) throw Error("the drive did not take the write");
        d_.write_at(offset, data);
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override { d_.read_at(offset, dst, length); }
    void flush() override { d_.flush(); }
    void forget_partition_table() override { ++forgot; }
    static inline int forgot = 0;

private:
    MemoryDevice& d_;
    bool fail_;
};

// A disk as listed before the erase: GPT, one volume named "Old" on F:.
DiskInfo listed_disk() {
    DiskInfo d;
    d.number = 7;
    d.model = "Fake Drive";
    d.serial = "SER-1";
    d.bus = "USB";
    d.size = 64 * kMiB;  // room for the 16 MiB reserved partition Windows puts first on GPT
    d.sector_size = 512;
    d.style = "GPT";
    d.id = "gpt-11111111-2222-3333-4444-555555555555";
    d.partitions = 1;
    d.path = "/dev/fake";
    DiskVolume v;
    v.name = "\\\\?\\Volume{fake}\\";
    v.mount = "F:\\";
    v.label = "Old";
    v.offset = kMiB;
    v.length = 2 * kMiB;
    d.volumes.push_back(v);
    return d;
}

// The same disk after the erase: blank.
DiskInfo blank_of(DiskInfo d) {
    d.style = "RAW";
    d.id.clear();
    d.partitions = 0;
    d.volumes.clear();
    return d;
}

struct FakeSetup {
    MemoryDevice device{static_cast<std::size_t>(4 * kMiB)};
    std::vector<DiskInfo> after;  // what the listing shows after the erase
    bool fail_writes = false;
    bool stop = false;
    int opened = 0;
    int listed = 0;
    std::vector<std::pair<DiskInfo, DiskIdentity>> inits;
    std::string init_fails;  // init throws this when set
    std::string table_id;    // init writes a partition table with this id first, when set
    std::string planned_id;  // init is about to write a partition table with this id, when set
    std::optional<std::uint32_t> used;  // the drive letters given now (the system's when not set)
    std::vector<NewDiskOptions> options;  // what each init was asked for
    std::string missing_tool;  // the setup program the fake lookup does not find
    int settled = 0;
    bool stop_while_settling = false;
    EraseSetupSteps steps() {
        EraseSetupSteps s;
        s.open = [this] {
            ++opened;
            return std::unique_ptr<WipeDevice>(std::make_unique<BorrowedDevice>(device, fail_writes));
        };
        s.list = [this] {
            ++listed;
            return after;
        };
        s.init = [this](const DiskInfo& d, const DiskIdentity& id, const NewDiskOptions& o) {
            CHECK_FALSE(o.force_dismount);
            options.push_back(o);
            if (!planned_id.empty() && o.on_table_planned) o.on_table_planned(planned_id);
            if (!table_id.empty() && o.on_table) o.on_table(table_id);
            if (!init_fails.empty()) throw Error(init_fails);
            inits.emplace_back(d, id);
            NewDiskResult r;
            r.volumes.push_back("F:\\");
            return r;
        };
        s.stop_asked = [this] { return stop; };
        s.settle = [this](const DiskInfo&, const NewDiskOptions&) {
            ++settled;
            if (stop_while_settling) stop = true;
        };
        s.find_tool = [this](const std::string& name) { return name == missing_tool ? std::string() : "/fake/bin/" + name; };
        if (used) s.used_letters = [this] { return *used; };
        s.tries = 2;
        s.wait = std::chrono::milliseconds(1);
        return s;
    }
};

}  // namespace

TEST_CASE("erase then set up: the disk is set up only after an erase that finished completely") {
    const DiskInfo d = listed_disk();
    const DiskIdentity pinned = identity_of(d);
    NewDiskOptions setup;
    setup.label = "Backup";
    setup.force_dismount = true;  // never passed on: a blank disk has nothing to force closed

    SUBCASE("a complete erase, then the setup on the disk erased, pinned to its serial number and size") {
        FakeSetup f;
        f.after = {blank_of(d)};
        BorrowedDevice::forgot = 0;
        bool announced = false;
        EraseSetupSteps steps = f.steps();
        steps.on_setup = [&] { announced = true; };
        WipeOptions w;
        w.verify = true;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, steps);
        CHECK(r.wipe.verified);
        CHECK(r.set_up);
        CHECK(r.setup_error.empty());
        CHECK(announced);
        CHECK(BorrowedDevice::forgot == 1);
        REQUIRE(f.inits.size() == 1);
        CHECK(f.inits[0].first.number == 7);
        CHECK(f.inits[0].second.serial == "SER-1");
        CHECK(f.inits[0].second.size == 64 * kMiB);
        CHECK(f.inits[0].second.id.empty());  // the erase removed the partition table
        CHECK(r.setup.volumes == std::vector<std::string>{"F:\\"});
        CHECK(all_zero(f.device.data.data(), f.device.data.size()));
    }
    SUBCASE("a cancelled erase is never set up") {
        FakeSetup f;
        f.after = {blank_of(d)};
        BorrowedDevice::forgot = 0;
        WipeOptions w;
        w.should_cancel = [] { return true; };
        CHECK_THROWS_AS(erase_and_set_up(d, pinned, w, setup, f.steps()), Cancelled);
        CHECK(f.inits.empty());
        CHECK(f.listed == 0);
        CHECK(BorrowedDevice::forgot == 0);
    }
    SUBCASE("a failed erase is never set up") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.fail_writes = true;
        WipeOptions w;
        CHECK_THROWS_AS(erase_and_set_up(d, pinned, w, setup, f.steps()), Error);
        CHECK(f.inits.empty());
        CHECK(f.listed == 0);
    }
    SUBCASE("an erase whose check finds other data is never set up") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.device.flip_at = 3 * kMiB + 5;
        WipeOptions w;
        w.verify = true;
        CHECK_THROWS_AS(erase_and_set_up(d, pinned, w, setup, f.steps()), Error);
        CHECK(f.inits.empty());
    }
    SUBCASE("a stop asked as the erase ended: erased, not set up") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.stop = true;
        WipeOptions w;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CHECK(r.stopped);
        CHECK_FALSE(r.set_up);
        CHECK(f.inits.empty());
    }
    SUBCASE("only the disk erased is set up") {
        WipeOptions w;
        std::vector<DiskInfo> others;
        DiskInfo serial = blank_of(d);
        serial.serial = "SER-2";
        DiskInfo size = blank_of(d);
        size.size += kMiB;
        DiskInfo model = blank_of(d);
        model.model = "Another Drive";
        DiskInfo bus = blank_of(d);
        bus.bus = "SATA";
        DiskInfo path = blank_of(d);
        path.path = "/dev/other";
        DiskInfo table = blank_of(d);  // got a partition table again since
        table.style = "GPT";
        table.id = "gpt-99999999-2222-3333-4444-555555555555";
        DiskInfo parts = blank_of(d);
        parts.partitions = 1;
        DiskInfo number = blank_of(d);
        number.number = 8;
        for (const DiskInfo& now : {serial, size, model, bus, path, table, parts, number}) {
            FakeSetup f;
            f.after = {now};
            const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
            CAPTURE(r.setup_error);
            CHECK_FALSE(r.set_up);
            CHECK_FALSE(r.setup_error.empty());
            CHECK(f.inits.empty());
            CHECK(f.listed == 2);  // looked for again, in case the system had not caught up yet
        }
        CHECK(erased_disk_mismatch(blank_of(d), d, pinned).empty());
        CHECK_FALSE(erased_disk_mismatch(d, d, pinned).empty());  // not blank
        // A whole disk with a file system or encryption of its own has no partition table either.
        DiskInfo whole_fs = blank_of(d);
        whole_fs.filesystem = "ext4";
        CHECK(erased_disk_mismatch(whole_fs, d, pinned).find("ext4") != std::string::npos);
        whole_fs.filesystem = "crypto_LUKS";
        CHECK_FALSE(erased_disk_mismatch(whole_fs, d, pinned).empty());
        DiskInfo with_volume = blank_of(d);
        DiskVolume v;
        v.name = "/dev/fake";
        with_volume.volumes.push_back(v);
        CHECK_FALSE(erased_disk_mismatch(with_volume, d, pinned).empty());
        FakeSetup f;
        f.after = {whole_fs};
        CHECK_FALSE(erase_and_set_up(d, pinned, w, setup, f.steps()).set_up);
        CHECK(f.inits.empty());
    }
    SUBCASE("a setup that fails after a complete erase is told, and nothing throws") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.init_fails = "format.com failed";
        WipeOptions w;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CHECK_FALSE(r.set_up);
        CHECK(r.setup_error == "format.com failed");
        CHECK_FALSE(r.table_written);  // failed before any partition table was written
        CHECK(r.table_id.empty());
    }
    SUBCASE("a setup that fails after writing the partition table says so, with the table's id") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.table_id = "gpt-ABCDEF01-2222-3333-4444-555555555555";
        f.init_fails = "format.com failed";
        WipeOptions w;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CHECK_FALSE(r.set_up);
        CHECK(r.table_written);
        CHECK(r.table_id == f.table_id);
    }
    SUBCASE("a stop asked while waiting for the old drive letter: nothing is written") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.stop_while_settling = true;
        WipeOptions w;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CHECK(f.settled == 1);
        CHECK(r.stopped);
        CHECK_FALSE(r.set_up);
        CHECK(f.inits.empty());
    }
#ifndef _WIN32
    SUBCASE("a missing mkfs program stops it before anything is erased") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.missing_tool = "mkfs.exfat";
        NewDiskOptions exfat = setup;
        exfat.filesystem = "exfat";
        WipeOptions w;
        CHECK_THROWS_WITH_AS(erase_and_set_up(d, pinned, w, exfat, f.steps()), doctest::Contains("exfatprogs"), Error);
        CHECK(f.opened == 0);  // nothing was opened, so nothing was erased
        CHECK(f.inits.empty());
        f.missing_tool = "sfdisk";
        CHECK_THROWS_WITH_AS(erase_and_set_up(d, pinned, w, setup, f.steps()), doctest::Contains("util-linux"), Error);
        CHECK(f.opened == 0);
        f.missing_tool = "mkfs.ntfs";  // not the one asked for
        CHECK(erase_and_set_up(d, pinned, w, setup, f.steps()).set_up);
    }
#endif
    SUBCASE("without a setup it erases as before") {
        FakeSetup f;
        BorrowedDevice::forgot = 0;
        WipeOptions w;
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, std::nullopt, f.steps());
        CHECK_FALSE(r.set_up);
        CHECK(f.listed == 0);
        CHECK(BorrowedDevice::forgot == 1);
        CHECK(all_zero(f.device.data.data(), f.device.data.size()));
    }
}

TEST_CASE("erase then set up: MBR is refused on a disk over 2 TiB before anything is erased") {
    DiskInfo big = listed_disk();
    big.size = 3ull << 40;
    CHECK_FALSE(mbr_fits(big));
    DiskInfo two = listed_disk();
    two.size = (2ull << 40) - 512;
    CHECK(mbr_fits(two));
    two.size = 2ull << 40;  // 2^32 sectors: one more than an MBR entry can count
    CHECK_FALSE(mbr_fits(two));
    DiskInfo big_sectors = big;  // 4096-byte sectors: MBR reaches 16 TiB
    big_sectors.sector_size = 4096;
    CHECK(mbr_fits(big_sectors));

    NewDiskOptions mbr;
    mbr.style = TableStyle::Mbr;
    CHECK_THROWS_AS(check_setup_after_erase(big, mbr), Error);
    FakeSetup f;
    f.after = {blank_of(big)};
    WipeOptions w;
    CHECK_THROWS_AS(erase_and_set_up(big, identity_of(big), w, mbr, f.steps()), Error);
    CHECK(f.opened == 0);  // nothing was opened, so nothing was erased
    NewDiskOptions gpt;
    const auto all_tools = [](const std::string& name) { return "/fake/bin/" + name; };
    CHECK_NOTHROW(check_setup_after_erase(big, gpt, all_tools));
    NewDiskOptions bad_label;
    bad_label.filesystem = "exfat";
    bad_label.label = "a name too long for exfat";
    CHECK_THROWS_AS(check_setup_after_erase(listed_disk(), bad_label), Error);
#ifdef _WIN32
    // A letter in use is refused, unless the disk erased has it (the erase frees it).
    for (char c = 'D'; c <= 'Z'; ++c) {
        if ((used_drive_letters() & (1u << (c - 'A'))) == 0) continue;
        NewDiskOptions taken;
        taken.letter = c;
        DiskInfo other = listed_disk();
        other.volumes[0].mount = "Q:\\";
        if (c == 'Q') continue;
        CHECK_THROWS_AS(check_setup_after_erase(other, taken), Error);
        DiskInfo own = listed_disk();
        own.volumes[0].mount = std::string(1, c) + ":\\";
        CHECK_NOTHROW(check_setup_after_erase(own, taken));
        break;
    }
#else
    NewDiskOptions letter;
    letter.letter = 'F';
    CHECK_THROWS_AS(check_setup_after_erase(listed_disk(), letter), Error);  // no drive letters here
#endif
}

TEST_CASE("erase then set up: what the app passes to the engine") {
    const DiskInfo d = listed_disk();
    SetupDefaults s = setup_defaults(d);
    CHECK(s.label == "Old");
    DiskInfo unnamed = listed_disk();
    unnamed.volumes[0].label.clear();
    CHECK(setup_defaults(unnamed).label == "Backup");
    CHECK(setup_letters(d, "GFHk") == "FGHK");  // its own first, each once
#ifdef _WIN32
    CHECK(s.letter == 'F');
    // Without a letter of its own: any free one, picked when it is set up (a letter that looks
    // free now may be taken hours later, when the erase ended).
    DiskInfo no_letter = listed_disk();
    no_letter.volumes[0].mount.clear();
    CHECK(setup_defaults(no_letter).letter == 0);
#else
    CHECK(s.letter == 0);
#endif

    EraseRequest r;
    r.disk = "7";
    r.shown = identity_of(d);
    r.confirm = "ERASE DISK 7";
    r.method = "random";
    r.verify = true;
    const std::vector<std::string> plain{"disk", "wipe", "7", "--expect-size", std::to_string(64 * kMiB), "--expect-serial", "SER-1",
                                         "--expect-id", d.id, "--method", "random", "--confirm", "ERASE DISK 7", "--verify"};
    CHECK(erase_disk_args(r) == plain);
    NewDiskOptions o;
    o.style = TableStyle::Mbr;
    o.filesystem = "exfat";
    o.label = "Backup";
    o.letter = 'F';
    r.setup = o;
    std::vector<std::string> with = plain;
    with.insert(with.end(), {"--then-init", "mbr", "--fs", "exfat", "--label", "Backup", "--letter", "F"});
    CHECK(erase_disk_args(r) == with);
    // Set up again after a failed setup: disk init, pinned to the erased disk without a partition table.
    const std::vector<std::string> again{"disk", "init", "7", "--mbr", "--fs", "exfat", "--label", "Backup", "--letter", "F",
                                         "--confirm", "ERASE DISK 7", "--expect-size", std::to_string(64 * kMiB),
                                         "--expect-serial", "SER-1"};
    CHECK(setup_again_args(r) == again);
    r.setup->style = TableStyle::Gpt;
    r.setup->label.clear();
    r.setup->letter = 0;
    r.shown.serial.clear();
    CHECK(setup_again_args(r) == std::vector<std::string>{"disk", "init", "7", "--gpt", "--fs", "exfat", "--confirm", "ERASE DISK 7",
                                                          "--expect-size", std::to_string(64 * kMiB)});
    // After a setup that had written its partition table: pinned to exactly that table.
    CHECK(setup_again_args(r, "gpt-ABCDEF01-2222-3333-4444-555555555555") ==
          std::vector<std::string>{"disk", "init", "7", "--gpt", "--fs", "exfat", "--confirm", "ERASE DISK 7", "--expect-size",
                                   std::to_string(64 * kMiB), "--expect-id", "gpt-ABCDEF01-2222-3333-4444-555555555555"});

    // The events the status window reads for the two parts and the end.
    TaskEvent e = parse_task_event(R"({"event":"step","phase":"setup","message":"Setting up the drive"})");
    CHECK(e.type == TaskEvent::Type::Step);
    CHECK(e.phase == "setup");
    e = parse_task_event(R"({"event":"done","status":"setup_failed","erased":true,"message":"Erased, but setting it up failed: x"})");
    CHECK(e.status == "setup_failed");
    CHECK(e.erased);
    CHECK_FALSE(e.table_written);
    CHECK(e.table_id.empty());
    e = parse_task_event(R"({"event":"done","status":"setup_failed","erased":true,"table_written":true,"table_id":"gpt-AB","message":"x"})");
    CHECK(e.table_written);
    CHECK(e.table_id == "gpt-AB");
    e = parse_task_event(R"({"event":"done","status":"setup_failed","table_written":"yes","table_id":7})");
    CHECK_FALSE(e.table_written);  // only a real true
    CHECK(e.table_id.empty());
    e = parse_task_event(R"({"event":"done","status":"success","volumes":["F:"]})");
    CHECK_FALSE(e.erased);
    CHECK(e.volumes == std::vector<std::string>{"F:"});
}

TEST_CASE("erase then set up: setting the drive up again is pinned to the table the failed setup wrote") {
    const DiskInfo d = listed_disk();
    const DiskIdentity pinned = identity_of(d);
    const std::string table = "gpt-ABCDEF01-2222-3333-4444-555555555555";
    DiskInfo written = blank_of(d);  // the setup wrote its table and partition, then failed
    written.style = "GPT";
    written.id = table;
    written.partitions = 2;
    DiskVolume v;
    v.name = "\\\\?\\Volume{new}\\";
    v.offset = 17 * kMiB;
    written.volumes.push_back(v);
    CHECK(erased_disk_mismatch(written, d, pinned, table).empty());
    CHECK(erased_disk_mismatch(written, d, pinned, "gpt-abcdef01-2222-3333-4444-555555555555").empty());  // as identity_mismatch
    CHECK_FALSE(erased_disk_mismatch(written, d, pinned).empty());  // not blank
    CHECK_FALSE(erased_disk_mismatch(written, d, pinned, "gpt-99999999-2222-3333-4444-555555555555").empty());
    CHECK_FALSE(erased_disk_mismatch(blank_of(d), d, pinned, table).empty());  // not that table
    DiskInfo other = written;  // another disk with that table
    other.serial = "SER-2";
    CHECK_FALSE(erased_disk_mismatch(other, d, pinned, table).empty());
    // The engine checks the same pin on its own (identity_mismatch, as for --expect-id).
    DiskIdentity again = erased_identity(pinned);
    again.id = table;
    CHECK(identity_mismatch(written, again).empty());
    again.id = "gpt-99999999-2222-3333-4444-555555555555";
    CHECK_FALSE(identity_mismatch(written, again).empty());
    CHECK_FALSE(identity_mismatch(written, erased_identity(pinned)).empty());
}

namespace {

// A USB stick or SD card as Windows lists it once erased: no partition table, but the one volume
// Windows keeps of a removable-media device, RAW (no file system), over the whole disk, with
// its drive letter.
DiskInfo erased_stick(const DiskInfo& d) {
    DiskInfo s = blank_of(d);
    s.removable = true;
    s.bus = d.bus;
    DiskVolume v;
    v.name = "\\\\?\\Volume{stick}\\";
    v.mount = "F:\\";
    v.offset = 0;
    v.length = d.size;
    s.volumes.push_back(v);
    return s;
}

}  // namespace

TEST_CASE("erase then set up: what belongs to the disk being set up itself") {
    DiskInfo d = listed_disk();
    d.removable = true;
    const DiskIdentity pinned = identity_of(d);
    // Its own letters (F:, of its volume), D to Z only.
    CHECK(own_letter(d, 'F'));
    CHECK(own_letter(d, 'f'));
    CHECK_FALSE(own_letter(d, 'G'));
    CHECK_FALSE(own_letter(blank_of(d), 'F'));
    DiskInfo on_c = d;
    on_c.volumes[0].mount = "C:\\";
    CHECK_FALSE(own_letter(on_c, 'C'));
    DiskInfo in_folder = d;
    in_folder.volumes[0].mount = "C:\\mnt\\F\\";
    CHECK_FALSE(own_letter(in_folder, 'C'));
    const auto bit = [](char c) { return 1u << (c - 'A'); };
    const std::uint32_t used = bit('C') | bit('F') | bit('G');
    CHECK(letter_problem(d, 'F', used).empty());  // its own: free once it is set up
    CHECK(letter_problem(d, 'G', used) == "G: is in use already");  // another drive's: never
    CHECK(letter_problem(d, 'H', used).empty());
    CHECK(letter_problem(on_c, 'C', used) == "the drive letter must be one of D to Z");
    CHECK(letter_problem(blank_of(d), 'F', used) == "F: is in use already");

    // The volume Windows keeps of an erased removable-media device.
    const DiskInfo stick = erased_stick(d);
    CHECK(only_whole_disk_raw_volume(stick));
    DiskInfo labelled_raw = stick;
    labelled_raw.volumes[0].filesystem = "RAW";
    CHECK(only_whole_disk_raw_volume(labelled_raw));
    // Anything else is not: a fixed disk, a file system, not from the start, longer than the disk,
    // a part of a volume on several disks, two volumes, a partition table.
    DiskInfo fixed = stick;
    fixed.removable = false;
    DiskInfo with_fs = stick;
    with_fs.volumes[0].filesystem = "FAT32";
    DiskInfo later = stick;
    later.volumes[0].offset = kMiB;
    later.volumes[0].length = d.size - kMiB;
    DiskInfo longer = stick;
    longer.volumes[0].length = d.size + 512;
    DiskInfo empty = stick;
    empty.volumes[0].length = 0;
    DiskInfo spans = stick;
    spans.volumes[0].spans = true;
    DiskInfo two = stick;
    two.volumes.push_back(stick.volumes[0]);
    DiskInfo table = stick;
    table.style = "MBR";
    table.id = "mbr-12345678";
    DiskInfo parts = stick;
    parts.partitions = 1;
    for (const DiskInfo& no : {fixed, with_fs, later, longer, empty, spans, two, table, parts}) {
        CHECK_FALSE(only_whole_disk_raw_volume(no));
        CHECK_FALSE(erased_disk_mismatch(no, d, pinned).empty());
    }
    CHECK_FALSE(only_whole_disk_raw_volume(blank_of(d)));
#ifdef _WIN32
    // Blank as Windows shows an erased stick: set up, with the letter of its own volume.
    CHECK(erased_disk_mismatch(stick, d, pinned).empty());
#else
    // Linux keeps its rule: no volume at all, and nothing on the whole disk.
    CHECK_FALSE(erased_disk_mismatch(stick, d, pinned).empty());
#endif
    DiskInfo whole_fs = stick;
    whole_fs.filesystem = "vfat";
    CHECK_FALSE(erased_disk_mismatch(whole_fs, d, pinned).empty());
    DiskInfo other_serial = stick;
    other_serial.serial = "SER-2";
    CHECK_FALSE(erased_disk_mismatch(other_serial, d, pinned).empty());

    NewDiskOptions setup;
    setup.label = "Stick";
    WipeOptions w;
#ifdef _WIN32
    SUBCASE("an erased stick is set up, with the letter its own volume has") {
        setup.letter = 'F';
        FakeSetup f;
        f.after = {stick};
        f.used = bit('C') | bit('F');  // F: is the stick's own volume
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CAPTURE(r.setup_error);
        CHECK(r.set_up);
        REQUIRE(f.options.size() == 1);
        CHECK(f.options[0].letter == 'F');
        CHECK(r.setup.warnings.empty());
        CHECK_NOTHROW(check_init_options(setup, stick));
    }
    SUBCASE("a letter another drive took while the disk was erased is not taken from it") {
        setup.letter = 'F';
        FakeSetup f;
        f.after = {blank_of(d)};  // F: is not the disk's any more
        f.used = bit('C') | bit('F');
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CAPTURE(r.setup_error);
        CHECK(r.set_up);
        REQUIRE(f.options.size() == 1);
        CHECK(f.options[0].letter == 0);  // the letter Windows gives it
        REQUIRE(r.setup.warnings.size() == 1);
        CHECK(r.setup.warnings[0].starts_with("F: was in use by another drive"));
        // Free still: the one asked for, no warning.
        FakeSetup free;
        free.after = {blank_of(d)};
        free.used = bit('C');
        const EraseSetupResult kept = erase_and_set_up(d, pinned, w, setup, free.steps());
        CHECK(kept.set_up);
        REQUIRE(free.options.size() == 1);
        CHECK(free.options[0].letter == 'F');
        CHECK(kept.setup.warnings.empty());
    }
#endif
    SUBCASE("a setup that fails while writing the partition table tells the id it was writing") {
        FakeSetup f;
        f.after = {blank_of(d)};
        f.planned_id = "gpt-ABCDEF01-2222-3333-4444-555555555555";
        f.init_fails = "writing a new partition table on disk 7: the device is not ready";
        const EraseSetupResult r = erase_and_set_up(d, pinned, w, setup, f.steps());
        CHECK_FALSE(r.set_up);
        CHECK_FALSE(r.table_written);
        CHECK(r.table_id == f.planned_id);
        // Written after all: both are told.
        FakeSetup g;
        g.after = {blank_of(d)};
        g.planned_id = g.table_id = f.planned_id;
        g.init_fails = "format.com failed";
        const EraseSetupResult w2 = erase_and_set_up(d, pinned, w, setup, g.steps());
        CHECK(w2.table_written);
        CHECK(w2.table_id == f.planned_id);
    }
}

TEST_CASE("erase then set up: setting the drive up again accepts only the disk as the failed setup left it") {
    const DiskInfo d = listed_disk();
    const DiskIdentity pinned = identity_of(d);
    const std::string table = "gpt-ABCDEF01-2222-3333-4444-555555555555";
    CHECK(valid_table_id(table));
    CHECK(valid_table_id("gpt-abcdef01-2222-3333-4444-555555555555"));
    CHECK(valid_table_id("mbr-0A1B2C3D"));
    for (const char* bad : {"", "gpt-", "mbr-", "gpt-AB", "mbr-0A1B2C3", "mbr-0A1B2C3D4", "mbr-0A1B2C3G",
                                  "MBR-0A1B2C3D", "gpt-ABCDEF01-2222-3333-4444-55555555555", "gpt-ABCDEF01-2222-3333-4444-5555555555555",
                                  "gpt-ABCDEF0122222-3333-4444-555555555555", "gpt-ABCDEF01-2222-3333-4444-55555555555X",
                                  "gpt-ABCDEF01-2222-3333-4444-555555555555 --force-dismount", "--force-dismount",
                                  "mbr-0A1B2C3D\n--x", "gpt-{ABCDEF01-2222-3333-4444-555555555555}"})
        CHECK_FALSE(valid_table_id(bad));

    DiskInfo written = blank_of(d);
    written.style = "GPT";
    written.id = table;
    written.partitions = 2;
    const DiskInfo blank = blank_of(d);
    SUBCASE("nothing written: blank only") {
        SetupAgainPin p = setup_again_pin(&blank, d, pinned, false, {});
        CHECK(p.ok);
        CHECK(p.table_id.empty());
        p = setup_again_pin(&written, d, pinned, false, {});
        CHECK_FALSE(p.ok);
        CHECK_FALSE(p.why.empty());
    }
    SUBCASE("written: exactly that table") {
        SetupAgainPin p = setup_again_pin(&written, d, pinned, true, table);
        CHECK(p.ok);
        CHECK(p.table_id == table);
        CHECK_FALSE(setup_again_pin(&blank, d, pinned, true, table).ok);
        CHECK_FALSE(setup_again_pin(&written, d, pinned, true, {}).ok);  // which one is not known
        CHECK_FALSE(setup_again_pin(&written, d, pinned, true, "gpt-99999999-2222-3333-4444-555555555555").ok);
    }
    SUBCASE("failed while writing it: blank, or exactly that table") {
        SetupAgainPin p = setup_again_pin(&blank, d, pinned, false, table);
        CHECK(p.ok);
        CHECK(p.table_id.empty());
        p = setup_again_pin(&written, d, pinned, false, table);
        CHECK(p.ok);
        CHECK(p.table_id == table);
        DiskInfo other_table = written;
        other_table.id = "gpt-99999999-2222-3333-4444-555555555555";
        CHECK_FALSE(setup_again_pin(&other_table, d, pinned, false, table).ok);
        DiskInfo other_disk = written;
        other_disk.serial = "SER-2";
        CHECK_FALSE(setup_again_pin(&other_disk, d, pinned, false, table).ok);
    }
    SUBCASE("an id that is not one the engine writes is never passed on") {
        DiskInfo odd = written;
        odd.id = "gpt-AB --force-dismount";
        const SetupAgainPin p = setup_again_pin(&odd, d, pinned, true, odd.id);
        CHECK_FALSE(p.ok);
        CHECK(p.table_id.empty());
        CHECK_FALSE(setup_again_pin(&blank, d, pinned, false, "--letter").ok);
    }
    SUBCASE("a disk that is not connected any more") {
        CHECK_FALSE(setup_again_pin(nullptr, d, pinned, false, {}).ok);
    }
}

TEST_CASE("new disk: drive letters are known by the letters Windows gave, not by whether they can be read") {
    const auto bit = [](char c) { return 1u << (c - 'A'); };
    // C: and an empty card reader slot on E: (nothing to read there, but the letter is taken).
    const std::uint32_t used = bit('C') | bit('E');
    const std::string free = free_letters(used);
    CHECK(free.find('E') == std::string::npos);
    CHECK(free.substr(0, 3) == "DFG");
    CHECK(free.size() == 22);  // D to Z but E
    CHECK(free_letters(0xFFFFFFFFu).empty());
    CHECK(letter_problem('E', used) == "E: is in use already");
    CHECK(letter_problem('e', used) == "E: is in use already");
    CHECK(letter_problem('F', used).empty());
    CHECK_FALSE(letter_problem('C', 0).empty());  // D to Z only
    CHECK_FALSE(letter_problem('1', 0).empty());
#ifndef _WIN32
    CHECK(used_drive_letters() == 0);
#endif
    // The erased disk had no letter of its own: none is preselected, since a letter that looks
    // free now may be taken by the end of the erase.
    DiskInfo no_letter = listed_disk();
    no_letter.volumes[0].mount.clear();
    CHECK(setup_defaults(no_letter).letter == 0);
}

namespace {

// A volume's drive letters as Windows keeps them: one letter per volume when `one_only`; the
// letters in `taken` belong to other drives.
struct FakeLetters {
    std::vector<std::string> mounts;
    std::string taken;
    std::string refused;  // letters that cannot be set although no drive has them
    bool one_only = false;
    char free_one = 'S';
    std::vector<std::string> log;
    LetterSteps steps() {
        LetterSteps s;
        s.add = [this](const std::string& root, std::string& why) {
            log.push_back("add " + root);
            if (taken.find(root[0]) != std::string::npos || refused.find(root[0]) != std::string::npos) {
                why = "in use";
                return false;
            }
            if (one_only && std::any_of(mounts.begin(), mounts.end(), [](const std::string& m) { return m.size() <= 3; })) {
                why = "the volume has a letter";
                return false;
            }
            mounts.push_back(root);
            return true;
        };
        s.remove = [this](const std::string& root) {
            log.push_back("remove " + root);
            mounts.erase(std::remove(mounts.begin(), mounts.end(), root), mounts.end());
        };
        s.free = [this] { return free_one; };
        s.in_use = [this](const std::string& root) { return taken.find(root[0]) != std::string::npos; };
        return s;
    }
};

}  // namespace

TEST_CASE("new disk: the automatic drive letter stays when the one asked for cannot be given") {
    for (const bool one_only : {false, true}) {
        CAPTURE(one_only);
        SUBCASE("the letter asked for is given, and the automatic one goes") {
            FakeLetters f;
            f.one_only = one_only;
            f.mounts = {"E:\\"};
            const LetterResult r = give_letter('G', "E:\\", f.steps());
            CHECK(r.root == "G:\\");
            CHECK(r.warning.empty());
            CHECK(f.mounts == std::vector<std::string>{"G:\\"});
        }
        SUBCASE("a letter taken meanwhile: the volume keeps the automatic one, with a warning") {
            FakeLetters f;
            f.one_only = one_only;
            f.mounts = {"E:\\"};
            f.taken = "G";
            const LetterResult r = give_letter('G', "E:\\", f.steps());
            CHECK(r.root == "E:\\");
            CHECK(r.warning.find("G: could not be given") != std::string::npos);
            CHECK(r.warning.find("E: instead") != std::string::npos);
            CHECK(f.mounts == std::vector<std::string>{"E:\\"});
            CHECK(std::find(f.log.begin(), f.log.end(), "remove E:\\") == f.log.end());  // never removed
        }
        SUBCASE("a letter that cannot be set for another reason: the automatic one is put back") {
            FakeLetters f;
            f.one_only = one_only;
            f.mounts = {"E:\\"};
            f.refused = "G";
            const LetterResult r = give_letter('G', "E:\\", f.steps());
            CHECK(r.root == "E:\\");
            CHECK_FALSE(r.warning.empty());
            CHECK(f.mounts == std::vector<std::string>{"E:\\"});
        }
        SUBCASE("no letter asked for: the automatic one") {
            FakeLetters f;
            f.one_only = one_only;
            f.mounts = {"E:\\"};
            CHECK(give_letter(0, "E:\\", f.steps()).root == "E:\\");
            CHECK(f.log.empty());
        }
        SUBCASE("no automatic letter and the one asked for is taken: a free one, with a warning") {
            FakeLetters f;
            f.one_only = one_only;
            f.taken = "G";
            const LetterResult r = give_letter('G', "", f.steps());
            CHECK(r.root == "S:\\");
            CHECK_FALSE(r.warning.empty());
            CHECK(f.mounts == std::vector<std::string>{"S:\\"});
        }
        SUBCASE("no letter can be given at all: an error, never a silent volume without a letter") {
            FakeLetters f;
            f.one_only = one_only;
            f.taken = "GS";
            CHECK_THROWS_AS(give_letter('G', "", f.steps()), Error);
            f.free_one = 0;
            CHECK_THROWS_AS(give_letter(0, "", f.steps()), Error);
        }
        SUBCASE("the letter asked for is the automatic one") {
            FakeLetters f;
            f.one_only = one_only;
            f.mounts = {"G:\\"};
            CHECK(give_letter('g', "G:\\", f.steps()).root == "G:\\");
            CHECK(f.log.empty());
        }
    }
}

TEST_CASE("new disk: the partition table gets the id it is known by again (Linux)") {
    const std::vector<NewPartition> p{{kMiB, 62 * kMiB, false}};
    CHECK(sfdisk_script(TableStyle::Gpt, p, 512, "ext4", "gpt-ABCDEF01-2222-4333-8444-555555555555")
              .starts_with("label: gpt\nlabel-id: ABCDEF01-2222-4333-8444-555555555555\nunit: sectors\n\n"));
    CHECK(sfdisk_script(TableStyle::Mbr, p, 512, "ntfs", "mbr-1A2B3C4D").starts_with("label: dos\nlabel-id: 0x1A2B3C4D\nunit: sectors\n\n"));
    CHECK(sfdisk_script(TableStyle::Gpt, p, 512, "ext4").starts_with("label: gpt\nunit: sectors\n\n"));
#ifndef _WIN32
    using Tools = std::vector<std::pair<std::string, std::string>>;
    CHECK(setup_tools("", false) == Tools{{"sfdisk", "util-linux"}, {"mkfs.ext4", "e2fsprogs"}});
    CHECK(setup_tools("exfat", true) == Tools{{"sfdisk", "util-linux"}, {"mkfs.exfat", "exfatprogs"}, {"mount", "util-linux"}});
    CHECK(setup_tools("ntfs", false)[1].first == "mkfs.ntfs");
    NewDiskOptions o;
    o.filesystem = "exfat";
    CHECK_THROWS_WITH_AS(check_setup_after_erase(listed_disk(), o, [](const std::string& n) {
                             return n == "mkfs.exfat" ? std::string() : "/fake/bin/" + n;
                         }),
                         doctest::Contains("mkfs.exfat was not found"), Error);
#else
    CHECK(setup_tools("ntfs", false).empty());  // format.com is part of Windows
#endif
}

TEST_CASE("erase then set up: the --then-init option of the command-line program") {
    std::string output;
    CHECK(run_cli({"--help"}, output) == 0);
    CHECK(output.find("--then-init") != std::string::npos);
    CHECK(run_cli({"disk", "wipe", "99999", "--then-init", "apm", "--confirm", "ERASE DISK 99999"}, output) == 1);
    CHECK(output.find("--then-init takes gpt or mbr") != std::string::npos);
    CHECK(run_cli({"disk", "wipe", "99999", "--fs", "exfat", "--confirm", "ERASE DISK 99999"}, output) == 1);
    CHECK(output.find("goes with --then-init") != std::string::npos);
    CHECK(run_cli({"disk", "wipe", "--volume", "Q:", "--then-init", "gpt", "--confirm", "ERASE Q:"}, output) == 1);
    CHECK(output.find("does not go with --volume") != std::string::npos);
    // Known; the disk does not exist (or no rights): stops there, and says it was not set up.
    CHECK(run_cli({"disk", "wipe", "99999", "--then-init", "gpt", "--label", "Backup", "--confirm", "ERASE DISK 99999"}, output) == 1);
    CAPTURE(output);
    CHECK(output.find("unknown option") == std::string::npos);
    CHECK(output.find("error:") != std::string::npos);
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

    // As the app runs it: the engine sends its events over a progress channel the test serves
    // (here without the approval prompt, as the tests run as administrator). Progress arrives
    // and the last one reaches 100%; then a slower erase is stopped by a cancel sent over it.
    {
        disks = list_tool_disks();
        it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
        REQUIRE(it != disks.end());
        const DiskInfo blank = *it;
        REQUIRE(blank.size == size);
        std::vector<std::string> args{"disk", "wipe", std::to_string(number), "--confirm", erase_text(blank),
                                      "--expect-size", std::to_string(blank.size)};
        if (!blank.serial.empty()) args.insert(args.end(), {"--expect-serial", blank.serial});
        if (!blank.id.empty()) args.insert(args.end(), {"--expect-id", blank.id});
        const test::EngineRun full = test::run_engine_with_channel(args, [](const std::string&, os::ProgressServer&) {});
        CAPTURE(full.output);
        CHECK(full.connected);
        CHECK(full.code == 0);
        std::vector<TaskEvent> progress;
        TaskEvent done;
        for (const std::string& line : full.lines) {
            const TaskEvent e = parse_task_event(line);
            if (e.type == TaskEvent::Type::Progress && e.has_total) progress.push_back(e);
            if (e.type == TaskEvent::Type::Done) done = e;
        }
        REQUIRE_FALSE(progress.empty());
        CHECK(progress.back().total == size);
        CHECK(progress.back().done == progress.back().total);
        CHECK(done.status == "success");
        MESSAGE("ran: disk wipe through the progress channel, " << progress.size() << " progress events");

        std::vector<std::string> slow = args;
        slow.insert(slow.end(), {"--method", "dod3", "--verify"});
        bool cancel_sent = false;
        const test::EngineRun stopped = test::run_engine_with_channel(slow, [&](const std::string& line, os::ProgressServer& server) {
            if (!cancel_sent && parse_task_event(line).type == TaskEvent::Type::Progress) cancel_sent = server.send_line("cancel");
        });
        CAPTURE(stopped.output);
        CHECK(stopped.connected);
        CHECK(cancel_sent);
        CHECK(stopped.code == 4);
        bool reached_check = false;
        TaskEvent last;
        for (const std::string& line : stopped.lines) {
            const TaskEvent e = parse_task_event(line);
            if (e.type == TaskEvent::Type::Progress && e.verifying) reached_check = true;
            if (e.type != TaskEvent::Type::Unknown) last = e;
        }
        CHECK_FALSE(reached_check);  // stopped during the first passes
        CHECK(last.type == TaskEvent::Type::Done);
        CHECK(last.status == "cancelled");
        MESSAGE("ran: disk wipe cancelled through the progress channel");
    }

    // Erased and set up again in the same run (--then-init), as the drive cleanser asks for it:
    // a stopped erase is never set up; a complete one goes on to the setup (its own part, after
    // the erase), and the disk ends up with one volume of the name and file system asked for.
    {
        const auto wipe_args = [&](const std::vector<std::string>& more) {
            disks = list_tool_disks();
            it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
            REQUIRE(it != disks.end());
            const DiskInfo on = *it;
            REQUIRE(on.size == size);
            std::vector<std::string> args{"disk", "wipe", std::to_string(number), "--confirm", erase_text(on),
                                          "--expect-size", std::to_string(on.size)};
            if (!on.serial.empty()) args.insert(args.end(), {"--expect-serial", on.serial});
            if (!on.id.empty()) args.insert(args.end(), {"--expect-id", on.id});
            args.insert(args.end(), more.begin(), more.end());
            return args;
        };
        const auto volume_named = [&](const std::string& label) -> std::optional<DiskVolume> {
            disks = list_tool_disks();
            it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
            if (it == disks.end()) return std::nullopt;
            for (const DiskVolume& v : it->volumes)
                if (v.label == label) return v;
            return std::nullopt;
        };

        bool cancel_sent = false;
        const test::EngineRun stopped = test::run_engine_with_channel(
            wipe_args({"--method", "dod3", "--verify", "--then-init", "gpt", "--fs", "ntfs", "--label", "VTNever"}),
            [&](const std::string& line, os::ProgressServer& server) {
                if (!cancel_sent && parse_task_event(line).type == TaskEvent::Type::Progress) cancel_sent = server.send_line("cancel");
            });
        CAPTURE(stopped.output);
        CHECK(cancel_sent);
        CHECK(stopped.code == 4);
        TaskEvent last;
        bool setup_started = false;
        for (const std::string& line : stopped.lines) {
            const TaskEvent e = parse_task_event(line);
            if (e.type == TaskEvent::Type::Step && e.phase == "setup") setup_started = true;
            if (e.type != TaskEvent::Type::Unknown) last = e;
        }
        CHECK_FALSE(setup_started);
        CHECK(last.status == "cancelled");
        CHECK(last.text.find("not set up") != std::string::npos);
        CHECK_FALSE(volume_named("VTNever").has_value());

        const auto upper_ascii = [](std::string text) {
            for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        };
        struct Setup {
            std::string style, fs, label, want_style, want_fs;
        };
        for (const Setup& one : std::vector<Setup>{{"gpt", "ntfs", "VTReady", "GPT", "NTFS"}, {"mbr", "exfat", "VTReadyEx", "MBR", "exFAT"}}) {
            const std::string style = one.style, fs = one.fs, label = one.label, want_style = one.want_style, want_fs = one.want_fs;
            CAPTURE(label);
            std::vector<std::string> more{"--then-init", style, "--fs", fs, "--label", label};
            // The second one asks for a letter of its own (one Windows has given to no drive),
            // which takes the place of the one Windows gives the new volume by itself.
            std::string letter;
            if (style == "mbr") {
                const std::string free = free_letters(used_drive_letters());
                REQUIRE_FALSE(free.empty());
                letter = std::string(1, free[free.size() / 2]);
                more.insert(more.end(), {"--letter", letter});
            }
            CAPTURE(letter);
            const test::EngineRun full = test::run_engine_with_channel(wipe_args(more), [](const std::string&, os::ProgressServer&) {});
            CAPTURE(full.output);
            CHECK(full.connected);
            CHECK(full.code == 0);
            bool erased_fully = false;
            bool setup_after = false;
            TaskEvent done;
            for (const std::string& line : full.lines) {
                const TaskEvent e = parse_task_event(line);
                if (e.type == TaskEvent::Type::Progress && e.has_total && e.done == e.total) erased_fully = true;
                if (e.type == TaskEvent::Type::Step && e.phase == "setup") setup_after = setup_after || erased_fully;
                if (e.type == TaskEvent::Type::Done) done = e;
            }
            CHECK(setup_after);  // the setup came once the erase had reached its end
            CHECK(done.status == "success");
            REQUIRE(done.volumes.size() == 1);
            CHECK(done.text == "Disk erased and ready as " + done.volumes[0] + " (" + label + ", " + want_fs + ").");
            if (!letter.empty()) CHECK(upper_ascii(done.volumes[0]) == letter + ":");
            const std::optional<DiskVolume> v = volume_named(label);
            REQUIRE(v.has_value());
            CHECK(v->filesystem == want_fs);
            CHECK(upper_ascii(v->mount.substr(0, 2)) == upper_ascii(done.volumes[0]));
            REQUIRE(it != disks.end());
            CHECK(it->style == want_style);
            CHECK(os::is_directory(v->mount));
            MESSAGE("ran: disk wipe --then-init " << style << " --fs " << fs << " on the test's virtual disk");
        }

        // The letter the disk's own drive has, asked for again (what the app offers by default):
        // free once the erase took that drive away.
        const auto done_of = [](const test::EngineRun& run) {
            TaskEvent done;
            for (const std::string& line : run.lines)
                if (parse_task_event(line).type == TaskEvent::Type::Done) done = parse_task_event(line);
            return done;
        };
        const auto warnings_of = [](const test::EngineRun& run) {
            std::vector<std::string> out;
            for (const std::string& line : run.lines) {
                const TaskEvent e = parse_task_event(line);
                if (e.type == TaskEvent::Type::Warning) out.push_back(e.text);
            }
            return out;
        };
        std::string own;
        {
            const std::optional<DiskVolume> ex = volume_named("VTReadyEx");
            REQUIRE(ex.has_value());
            REQUIRE(ex->mount.size() == 3);
            own = upper_ascii(ex->mount.substr(0, 1));
        }
        CAPTURE(own);
        {
            const test::EngineRun run = test::run_engine_with_channel(
                wipe_args({"--then-init", "gpt", "--fs", "ntfs", "--label", "VTOwn", "--letter", own}),
                [](const std::string&, os::ProgressServer&) {});
            CAPTURE(run.output);
            CHECK(run.code == 0);
            const TaskEvent done = done_of(run);
            CHECK(done.status == "success");
            REQUIRE(done.volumes.size() == 1);
            CHECK(upper_ascii(done.volumes[0]) == own + ":");
            CHECK(warnings_of(run).empty());
            const std::optional<DiskVolume> v = volume_named("VTOwn");
            REQUIRE(v.has_value());
            CHECK(upper_ascii(v->mount.substr(0, 2)) == own + ":");
            MESSAGE("ran: disk wipe --then-init with the disk's own drive letter");
        }

        // Set up the drive again after a setup that failed once it had written its partition
        // table and given the drive its letter: pinned to that table (--expect-id), with that
        // letter, which the disk's own new drive has now.
        {
            disks = list_tool_disks();
            it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == number; });
            REQUIRE(it != disks.end());
            const DiskInfo left = *it;
            REQUIRE(left.style == "GPT");
            REQUIRE(own_letter(left, own[0]));
            const SetupAgainPin pin = setup_again_pin(&left, left, identity_of(left), true, left.id);
            CAPTURE(pin.why);
            REQUIRE(pin.ok);
            CHECK(pin.table_id == left.id);
            EraseRequest req;
            req.disk = std::to_string(number);
            req.shown = identity_of(left);
            req.confirm = erase_text(left);
            NewDiskOptions retry;
            retry.filesystem = "exfat";
            retry.label = "VTAgain";
            retry.letter = own[0];
            req.setup = retry;
            std::vector<std::string> args = setup_again_args(req, pin.table_id);
            CHECK(std::find(args.begin(), args.end(), "--expect-id") != args.end());
            args.push_back("--json");
            std::string output;
            CHECK(run_cli(args, output) == 0);
            CAPTURE(output);
            CHECK(output.find("\"status\":\"success\"") != std::string::npos);
            const std::optional<DiskVolume> v = volume_named("VTAgain");
            REQUIRE(v.has_value());
            CHECK(v->filesystem == "exFAT");
            CHECK(upper_ascii(v->mount.substr(0, 2)) == own + ":");
            // Pinned to that table: once it is another one, nothing is written.
            std::string refused;
            CHECK(run_cli(args, refused) != 0);
            CHECK(volume_named("VTAgain").has_value());
            MESSAGE("ran: disk init --expect-id with the letter the disk's own drive has");
        }

        // A letter another drive takes while the disk is erased stays with that drive: the new
        // drive keeps the one Windows gives it, and a warning says so.
        {
            const std::string free = free_letters(used_drive_letters());
            std::string taken;
            for (const char c : free)
                if (std::string(1, c) != own) taken = std::string(1, c);
            REQUIRE_FALSE(taken.empty());
            CAPTURE(taken);
            const std::string subst = os::get_env("SystemRoot") + "\\System32\\subst.exe";
            const std::string folder = t.file("subst");
            fs::create_directories(fs::path(std::u8string(folder.begin(), folder.end())));
            struct Unsubst {
                std::string tool, drive;
                ~Unsubst() {
                    std::string ignored;
                    os::run_command({tool, drive, "/D"}, &ignored);
                }
            } unsubst{subst, taken + ":"};
            bool took = false;
            const test::EngineRun run = test::run_engine_with_channel(
                wipe_args({"--method", "dod3", "--verify", "--then-init", "gpt", "--fs", "ntfs", "--label", "VTTaken", "--letter",
                           taken}),
                [&](const std::string& line, os::ProgressServer&) {
                    if (took || parse_task_event(line).type != TaskEvent::Type::Progress) return;
                    std::string out;
                    took = os::run_command({subst, taken + ":", folder}, &out) == 0;
                });
            CAPTURE(run.output);
            REQUIRE(took);
            CHECK(run.code == 0);
            const TaskEvent done = done_of(run);
            CHECK(done.status == "success");
            REQUIRE(done.volumes.size() == 1);
            CHECK(upper_ascii(done.volumes[0]) != taken + ":");
            const std::vector<std::string> warned = warnings_of(run);
            REQUIRE(warned.size() == 1);
            CHECK(warned[0].starts_with(taken + ": was in use by another drive"));
            CHECK(os::is_directory(taken + ":\\"));  // still the other drive's
            const std::optional<DiskVolume> v = volume_named("VTTaken");
            REQUIRE(v.has_value());
            CHECK(upper_ascii(v->mount.substr(0, 2)) == upper_ascii(done.volumes[0]));
            MESSAGE("ran: disk wipe --then-init with a letter another drive took during the erase");
        }
    }
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

    // Erased and set up again in the same run (--then-init): one partition with the name and file
    // system asked for, on this loop device.
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.path == dev; });
    REQUIRE(it != disks.end());
    const DiskInfo blank = *it;
    std::vector<std::string> args{"disk", "wipe", dev, "--confirm", erase_text(blank), "--expect-size", std::to_string(blank.size),
                                  "--then-init", "gpt", "--fs", "ext4", "--label", "vready"};
    if (!blank.serial.empty()) args.insert(args.end(), {"--expect-serial", blank.serial});
    if (!blank.id.empty()) args.insert(args.end(), {"--expect-id", blank.id});
    std::vector<std::string> argv{VAULTIK_TEST_CLI};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string cli_output;
    CHECK(os::run_command(argv, &cli_output) == 0);
    CAPTURE(cli_output);
    CHECK(cli_output.find("Disk erased and ready as " + dev + "p1 (vready, ext4).") != std::string::npos);
    disks = list_tool_disks();
    it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.path == dev; });
    REQUIRE(it != disks.end());
    CHECK(it->style == "GPT");
    CHECK(it->partitions == 1);
    bool ready = false;
    for (const DiskVolume& v : it->volumes)
        if (v.name == dev + "p1" && v.label == "vready" && v.filesystem == "ext4") ready = true;
    CHECK(ready);
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
