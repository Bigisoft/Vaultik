#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "vaultik/jobs/drive_history.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;
using namespace vaultik::jobs;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("vaultik-drives-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string dir() const { return path.string(); }
};

os::DriveHealth hdd(std::uint64_t reallocated) {
    os::DriveHealth d;
    d.model = "Test HDD";
    d.serial = "  WD-123  ";
    d.size = 1000;
    d.smart = {{5, true, 100, 100, 10, reallocated}, {9, false, 90, 90, 0, 1234}};
    d.smart_health = reallocated == 0 ? 100 : 94;
    return d;
}

DriveReading at(std::uint64_t day, std::optional<int> health, std::optional<std::uint64_t> reallocated) {
    DriveReading r;
    r.time = day * kDay;
    r.key = "sn:X";
    r.health = health;
    r.reallocated = reallocated;
    return r;
}

}  // namespace

TEST_CASE("drive key uses the serial number, else model and size") {
    os::DriveHealth d = hdd(0);
    CHECK(drive_key(d) == "sn:WD-123");
    d.serial.clear();
    CHECK(drive_key(d) == "model:Test HDD:1000");
}

TEST_CASE("readings are stored only when something changed") {
    TempDir t;
    record_drives({hdd(0)}, t.dir());
    record_drives({hdd(0)}, t.dir());  // the same, within a day
    CHECK(drive_history("sn:WD-123", t.dir()).size() == 1);
    record_drives({hdd(3)}, t.dir());
    const auto h = drive_history("sn:WD-123", t.dir());
    REQUIRE(h.size() == 2);
    CHECK(h[1].reallocated == 3u);
    CHECK(h[1].health == 94);
}

TEST_CASE("the trend reports new bad sectors and a falling health") {
    const std::vector<DriveReading> history = {at(100, 100, 0), at(120, 100, 0), at(150, 97, 1)};
    const auto changes = drive_trend(history, at(160, 88, 4));
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].what == DriveChange::What::Health);
    CHECK(changes[0].from == 100);
    CHECK(changes[0].to == 88);
    CHECK(changes[1].what == DriveChange::What::Reallocated);
    CHECK(changes[1].to - changes[1].from == 4);
    CHECK(describe(changes[1]).rfind("4 more reallocated sectors since ", 0) == 0);
}

TEST_CASE("no trend from a steady drive, unknown values or old readings") {
    CHECK(drive_trend({at(100, 100, 0)}, at(110, 98, 0)).empty());     // 2 points is noise
    CHECK(drive_trend({at(100, {}, {})}, at(110, 90, 5)).empty());     // nothing to compare with
    CHECK(drive_trend({at(100, 100, 0)}, at(110, {}, {})).empty());    // nothing known now
    CHECK(drive_trend({at(1, 100, 0)}, at(200, 80, 9)).empty());       // outside the 90 days
}
