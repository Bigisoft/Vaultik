#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>

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

// ---- NVMe health log and smartctl (Linux reads these; the rules are shared) -------------------

namespace {

// An NVMe SMART / Health Information log page (02h), laid out as in the NVMe base specification.
std::array<std::uint8_t, 512> nvme_log(std::uint8_t critical, int kelvin, std::uint8_t used, std::uint64_t hours,
                                       std::uint64_t media_errors) {
    std::array<std::uint8_t, 512> log{};
    log[0] = critical;
    log[1] = static_cast<std::uint8_t>(kelvin & 0xFF);
    log[2] = static_cast<std::uint8_t>(kelvin >> 8);
    log[3] = 100;    // available spare
    log[4] = 10;     // its threshold
    log[5] = used;   // percentage used
    log[32] = 0x5A;  // data units read: not used here, but present in a real log
    log[48] = 0x3C;  // data units written
    for (int b = 0; b < 8; ++b) log[128 + b] = static_cast<std::uint8_t>(hours >> (8 * b));
    for (int b = 0; b < 8; ++b) log[160 + b] = static_cast<std::uint8_t>(media_errors >> (8 * b));
    return log;
}

}  // namespace

TEST_CASE("an NVMe health log gives the wear, temperature, hours and media errors") {
    DriveHealth d;
    d.media = "SSD";
    d.status = DriveHealth::Status::Healthy;
    const auto log = nvme_log(0, 310, 3, 4660, 0);
    REQUIRE(apply_nvme_log(d, log.data(), log.size()));
    CHECK(d.wear == 3);
    CHECK(d.health_percent() == 97);
    CHECK(d.temperature == 37);
    CHECK(d.power_on_hours == 4660u);
    CHECK(d.read_errors_uncorrected == 0u);
    CHECK(d.status == DriveHealth::Status::Healthy);

    // A log that is cut short is not read.
    DriveHealth short_log;
    CHECK_FALSE(apply_nvme_log(short_log, log.data(), 256));
    CHECK_FALSE(short_log.wear.has_value());
}

TEST_CASE("an NVMe critical warning turns a healthy drive into a warning, most serious bit first") {
    DriveHealth spare;
    spare.status = DriveHealth::Status::Healthy;
    const auto low = nvme_log(0x01, 318, 98, 30000, 0);
    apply_nvme_log(spare, low.data(), low.size());
    CHECK(spare.status == DriveHealth::Status::Warning);
    CHECK(spare.condition == "Spare space low");
    CHECK(spare.health_percent() == 2);
    CHECK(spare.worn_out());

    DriveHealth both;
    both.status = DriveHealth::Status::Healthy;
    const auto bad = nvme_log(0x05, 318, 120, 30000, 17);
    apply_nvme_log(both, bad.data(), bad.size());
    CHECK(both.condition == "Reliability degraded");
    CHECK(both.health_percent() == 0);  // past its rated life
    CHECK(both.read_errors_uncorrected == 17u);
}

namespace {

// Trimmed from "smartctl -j -a /dev/sda" (smartmontools 7.4) on a SATA SSD with two reallocated
// sectors.
constexpr char kSmartctlSata[] = R"json({
  "json_format_version": [1, 0],
  "smartctl": {"version": [7, 4], "svn_revision": "5530", "platform_info": "x86_64-linux-6.8.0-45-generic",
               "build_info": "(local build)", "argv": ["smartctl", "-j", "-a", "/dev/sda"], "exit_status": 0},
  "local_time": {"time_t": 1762950000, "asctime": "Wed Nov 12 13:20:00 2025 UTC"},
  "device": {"name": "/dev/sda", "info_name": "/dev/sda [SAT]", "type": "sat", "protocol": "ATA"},
  "model_family": "Samsung based SSDs",
  "model_name": "Samsung SSD 860 EVO 500GB",
  "serial_number": "S3Z1NB0K123456A",
  "wwn": {"naa": 5, "oui": 9528, "id": 61817282345},
  "firmware_version": "RVT04B6Q",
  "user_capacity": {"blocks": 976773168, "bytes": 500107862016},
  "logical_block_size": 512,
  "rotation_rate": 0,
  "smart_support": {"available": true, "enabled": true},
  "smart_status": {"passed": true},
  "ata_smart_attributes": {
    "revision": 1,
    "table": [
      {"id": 5, "name": "Reallocated_Sector_Ct", "value": 99, "worst": 99, "thresh": 10, "when_failed": "",
       "flags": {"value": 51, "string": "PO--CK ", "prefailure": true, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": true},
       "raw": {"value": 2, "string": "2"}},
      {"id": 9, "name": "Power_On_Hours", "value": 97, "worst": 97, "thresh": 0, "when_failed": "",
       "flags": {"value": 50, "string": "-O--CK ", "prefailure": false, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": true},
       "raw": {"value": 13764, "string": "13764"}},
      {"id": 12, "name": "Power_Cycle_Count", "value": 99, "worst": 99, "thresh": 0, "when_failed": "",
       "flags": {"value": 50, "string": "-O--CK ", "prefailure": false, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": true},
       "raw": {"value": 1121, "string": "1121"}},
      {"id": 177, "name": "Wear_Leveling_Count", "value": 96, "worst": 96, "thresh": 0, "when_failed": "",
       "flags": {"value": 19, "string": "PO--C- ", "prefailure": true, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": false},
       "raw": {"value": 61, "string": "61"}},
      {"id": 190, "name": "Airflow_Temperature_Cel", "value": 69, "worst": 49, "thresh": 0, "when_failed": "",
       "flags": {"value": 50, "string": "-O--CK ", "prefailure": false, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": true},
       "raw": {"value": 31, "string": "31"}},
      {"id": 241, "name": "Total_LBAs_Written", "value": 99, "worst": 99, "thresh": 0, "when_failed": "",
       "flags": {"value": 50, "string": "-O--CK ", "prefailure": false, "updated_online": true, "performance": false,
                 "error_rate": false, "event_count": true, "auto_keep": true},
       "raw": {"value": 61382928374, "string": "61382928374"}}
    ]
  },
  "power_on_time": {"hours": 13764},
  "power_cycle_count": 1121,
  "temperature": {"current": 31}
})json";

// Trimmed from "smartctl -j -a /dev/nvme0" on an NVMe SSD.
constexpr char kSmartctlNvme[] = R"json({
  "json_format_version": [1, 0],
  "smartctl": {"version": [7, 4], "argv": ["smartctl", "-j", "-a", "/dev/nvme0"], "exit_status": 0},
  "device": {"name": "/dev/nvme0", "info_name": "/dev/nvme0", "type": "nvme", "protocol": "NVMe"},
  "model_name": "KINGSTON SNVS1000G",
  "serial_number": "50026B7784A1B2C3",
  "firmware_version": "S8442101",
  "nvme_pci_vendor": {"id": 9798, "subsystem_id": 9798},
  "nvme_total_capacity": 1000204886016,
  "user_capacity": {"blocks": 1953525168, "bytes": 1000204886016},
  "smart_support": {"available": true, "enabled": true},
  "smart_status": {"passed": true, "nvme": {"value": 0}},
  "nvme_smart_health_information_log": {
    "critical_warning": 0, "temperature": 41, "available_spare": 100, "available_spare_threshold": 10,
    "percentage_used": 7, "data_units_read": 21865312, "data_units_written": 30715207,
    "host_reads": 241835631, "host_writes": 393510045, "controller_busy_time": 1031, "power_cycles": 1630,
    "power_on_hours": 2381, "unsafe_shutdowns": 97, "media_errors": 0, "num_err_log_entries": 0,
    "warning_temp_time": 0, "critical_comp_time": 0, "temperature_sensors": [41, 46]
  },
  "temperature": {"current": 41},
  "power_cycle_count": 1630,
  "power_on_time": {"hours": 2381}
})json";

// What smartctl says without the rights to open the drive.
constexpr char kSmartctlDenied[] = R"json({
  "json_format_version": [1, 0],
  "smartctl": {"version": [7, 4], "argv": ["smartctl", "-j", "-a", "/dev/sda"],
               "messages": [{"string": "Smartctl open device: /dev/sda failed: Permission denied", "severity": "error"}],
               "exit_status": 2},
  "local_time": {"time_t": 1762950000, "asctime": "Wed Nov 12 13:20:00 2025 UTC"}
})json";

}  // namespace

TEST_CASE("smartctl's JSON for a SATA drive is scored like an attribute table read directly") {
    DriveHealth d;
    std::string problem;
    REQUIRE(apply_smartctl_json(d, kSmartctlSata, &problem));
    CHECK(problem.empty());
    CHECK(d.model == "Samsung SSD 860 EVO 500GB");
    CHECK(d.serial == "S3Z1NB0K123456A");
    CHECK(d.size == 500107862016u);
    CHECK(d.media == "SSD");
    REQUIRE(d.smart.size() == 6);
    CHECK(d.smart[0].id == 5);
    CHECK(d.smart[0].prefail);
    CHECK(d.smart[0].threshold == 10);
    CHECK(d.smart[5].raw == 61382928374u);
    CHECK(d.temperature == 31);
    CHECK(d.power_on_hours == 13764u);
    CHECK(d.wear == 4);               // Wear_Leveling_Count at 96
    CHECK(d.health_percent() == 90);  // 100 - 6 * log2(3) for the two reallocated sectors
    CHECK(d.status == DriveHealth::Status::Warning);
    CHECK(d.condition == "2 reallocated sectors");

    // The same table as a SMART READ DATA sector gives the same health.
    DriveHealth direct;
    direct.media = "SSD";
    direct.status = DriveHealth::Status::Healthy;
    const auto s = sector({{5, 99, 2, true}, {9, 97, 13764}, {12, 99, 1121}, {177, 96, 61, true}, {190, 69, 31}});
    const auto t = thresholds({{5, 10}});
    direct.smart = parse_smart(s.data(), t.data());
    apply_smart(direct);
    CHECK(direct.health_percent() == d.health_percent());
    CHECK(direct.status == d.status);
}

TEST_CASE("smartctl's JSON for an NVMe drive is read like its health log") {
    DriveHealth d;
    REQUIRE(apply_smartctl_json(d, kSmartctlNvme));
    CHECK(d.model == "KINGSTON SNVS1000G");
    CHECK(d.media == "SSD");
    CHECK(d.status == DriveHealth::Status::Healthy);
    CHECK(d.wear == 7);
    CHECK(d.health_percent() == 93);
    CHECK(d.temperature == 41);
    CHECK(d.power_on_hours == 2381u);
    CHECK(d.read_errors_uncorrected == 0u);
    CHECK(d.smart.empty());
}

TEST_CASE("smartctl's errors and failed verdicts") {
    DriveHealth denied;
    std::string problem;
    CHECK_FALSE(apply_smartctl_json(denied, kSmartctlDenied, &problem));
    CHECK(problem == "Smartctl open device: /dev/sda failed: Permission denied");
    CHECK(denied.status == DriveHealth::Status::Unknown);
    CHECK_FALSE(denied.health_percent().has_value());

    DriveHealth garbage;
    CHECK_FALSE(apply_smartctl_json(garbage, "smartctl: command not found", &problem));
    CHECK_FALSE(problem.empty());

    DriveHealth failing;
    REQUIRE(apply_smartctl_json(failing, R"({"device": {"protocol": "ATA"}, "rotation_rate": 7200,
                                             "smart_status": {"passed": false}})"));
    CHECK(failing.status == DriveHealth::Status::Unhealthy);
    CHECK(failing.media == "HDD");
    CHECK_FALSE(failing.condition.empty());
}
