#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "vaultik/platform/drive_health.hpp"

using namespace vaultik::os;

namespace {

// A SMART READ DATA sector with the given (id, value, raw) entries and a valid checksum.
struct Entry {
    int id;
    int value;
    std::uint64_t raw;
    bool prefail = false;
};

std::array<std::uint8_t, 512> sector(std::initializer_list<Entry> entries) {
    std::array<std::uint8_t, 512> s{};
    s[0] = 0x10;  // revision
    int i = 0;
    for (const Entry& e : entries) {
        std::uint8_t* p = s.data() + 2 + 12 * i++;
        p[0] = static_cast<std::uint8_t>(e.id);
        p[1] = e.prefail ? 0x03 : 0x02;
        p[3] = static_cast<std::uint8_t>(e.value);
        p[4] = static_cast<std::uint8_t>(e.value);
        for (int b = 0; b < 6; ++b) p[5 + b] = static_cast<std::uint8_t>(e.raw >> (8 * b));
    }
    unsigned sum = 0;
    for (int k = 0; k < 511; ++k) sum += s[k];
    s[511] = static_cast<std::uint8_t>(0x100 - (sum & 0xFF));
    return s;
}

std::array<std::uint8_t, 512> thresholds(std::initializer_list<std::pair<int, int>> entries) {
    std::array<std::uint8_t, 512> s{};
    int i = 0;
    for (const auto& [id, t] : entries) {
        s[2 + 12 * i] = static_cast<std::uint8_t>(id);
        s[3 + 12 * i] = static_cast<std::uint8_t>(t);
        ++i;
    }
    return s;
}

DriveHealth hdd(const std::array<std::uint8_t, 512>& values, const std::uint8_t* thresh = nullptr) {
    DriveHealth d;
    d.media = "HDD";
    d.status = DriveHealth::Status::Healthy;
    d.smart = parse_smart(values.data(), thresh);
    apply_smart(d);
    return d;
}

}  // namespace

TEST_CASE("S.M.A.R.T. attribute tables are parsed and checked") {
    const auto s = sector({{5, 100, 0, true}, {9, 99, 12345}, {194, 64, 0x0012001E00 | 36}});
    const auto t = thresholds({{5, 10}});
    const std::vector<SmartAttribute> table = parse_smart(s.data(), t.data());
    REQUIRE(table.size() == 3);
    CHECK(table[0].id == 5);
    CHECK(table[0].prefail);
    CHECK(table[0].threshold == 10);
    CHECK(table[1].raw == 12345);

    auto bad = s;
    bad[20] ^= 0x55;  // breaks the checksum
    CHECK(parse_smart(bad.data(), nullptr).empty());
    CHECK(parse_smart(nullptr, nullptr).empty());
}

TEST_CASE("a clean hard disk is at 100%") {
    const DriveHealth d = hdd(sector({{5, 100, 0, true}, {9, 99, 20000}, {194, 64, 35}, {197, 100, 0}}));
    CHECK(d.health_percent() == 100);
    CHECK(d.smart_issues.empty());
    CHECK(d.status == DriveHealth::Status::Healthy);
    CHECK(d.temperature == 35);
    CHECK(d.power_on_hours == 20000u);
    CHECK_FALSE(d.wear.has_value());  // hard disks have no wear counter
}

TEST_CASE("bad sectors cost health and give a warning") {
    const DriveHealth one = hdd(sector({{5, 100, 1, true}, {9, 99, 1}, {197, 100, 0}}));
    CHECK(one.health_percent() == 94);
    CHECK(one.status == DriveHealth::Status::Warning);
    CHECK(one.condition == "1 reallocated sector");

    const DriveHealth many = hdd(sector({{5, 100, 100, true}, {9, 99, 1}, {197, 100, 0}}));
    CHECK(many.health_percent() == 60);

    const DriveHealth pending = hdd(sector({{5, 100, 8, true}, {9, 99, 1}, {197, 100, 3}}));
    CHECK(pending.health_percent() == 65);  // 100 - 6*log2(9) - 8*log2(4)
    CHECK(pending.smart_issues.size() == 2);

    // Only reallocation events: lower, but no warning by itself.
    const DriveHealth events = hdd(sector({{5, 100, 0, true}, {9, 99, 1}, {196, 100, 1}}));
    CHECK(events.health_percent() == 98);
    CHECK(events.status == DriveHealth::Status::Healthy);
}

TEST_CASE("an attribute at its threshold fails the drive") {
    const auto t = thresholds({{5, 36}});
    const DriveHealth d = hdd(sector({{5, 30, 2000, true}, {9, 99, 1}, {197, 100, 0}}), t.data());
    CHECK(d.health_percent().value_or(100) <= 10);
    CHECK(d.status == DriveHealth::Status::Unhealthy);
    CHECK(d.worn_out());
}

TEST_CASE("an SSD's life left comes from its own counter") {
    DriveHealth d;
    d.media = "SSD";
    d.status = DriveHealth::Status::Healthy;
    const auto s = sector({{5, 100, 0, true}, {9, 99, 1}, {177, 97, 40}});
    d.smart = parse_smart(s.data(), nullptr);
    apply_smart(d);
    CHECK(d.wear == 3);
    CHECK(d.health_percent() == 97);
}
