#pragma once

// What the operating system says about the health of the physical drives (Windows: the Storage
// Management API, MSFT_PhysicalDisk; Linux: sysfs. S.M.A.R.T. details where they can be read).
// Best effort: nothing here throws, and an unknown answer is Status::Unknown.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vaultik::os {

// One entry of an ATA drive's S.M.A.R.T. attribute table.
struct SmartAttribute {
    int id = 0;
    bool prefail = false;  // the drive counts it as a sign of coming failure
    int value = 0;         // normalized, usually 100 (or 200, 253) when new, falling with wear
    int worst = 0;
    int threshold = 0;     // at or below it the drive itself says the attribute failed (0: none known)
    std::uint64_t raw = 0; // 48 bits, the drive's own count (meaning varies by attribute)
};

struct DriveHealth {
    enum class Status { Healthy, Warning, Unhealthy, Unknown };

    int disk = -1;         // the system's disk number (Linux: the whole disk's device number)
    std::string device;    // Linux: the disk's device, "/dev/nvme0n1" (empty on Windows)
    std::string model;     // "WD My Passport 2627"
    std::string serial;    // the serial number, when the system tells it
    std::string bus;       // "USB", "SATA", "NVMe", ...
    std::string media;     // "HDD", "SSD" or empty
    std::uint64_t size = 0;
    Status status = Status::Unknown;
    std::string condition;  // the operational state when not plain OK: "Predictive failure", ...
    // S.M.A.R.T. details. Windows only lets administrators read them, except an NVMe drive's
    // health log (wear, temperature), which anyone may read.
    std::optional<int> temperature;  // degrees Celsius
    std::optional<int> wear;         // percent of an SSD's rated life used (can pass 100)
    std::optional<std::uint64_t> power_on_hours;
    std::optional<std::uint64_t> read_errors_uncorrected;
    // An ATA drive's attribute table (administrators only), and what apply_smart made of it.
    std::vector<SmartAttribute> smart;
    std::optional<int> smart_health;       // 100 minus the penalties for bad sectors and the like
    std::vector<std::string> smart_issues; // "3 reallocated sectors", ...
    // Linux: why the health could not be read ("needs root", "a virtual disk"), shown instead of
    // the Windows advice to run as administrator. Not a problem with the drive.
    std::string unknown_reason;

    // The health as a percentage: for an SSD the part of its rated life left, for any ATA drive
    // whose attributes could be read also lowered by bad sectors (the lower of the two). Unknown
    // when neither could be read.
    std::optional<int> health_percent() const {
        std::optional<int> out;
        if (wear) out = *wear >= 100 ? 0 : 100 - (*wear < 0 ? 0 : *wear);
        if (smart_health && (!out || *smart_health < *out)) out = smart_health;
        return out;
    }
    // A drive this close to the end of its life is a problem even while it still works.
    bool worn_out() const { return health_percent().value_or(100) <= 10; }
};

// The drives a folder (or file) is on: usually one, several for a volume spanning disks. Empty
// for network shares, cloud locations and when the system cannot tell.
std::vector<DriveHealth> drive_health(const std::string& path);
// Every physical drive of this computer.
std::vector<DriveHealth> all_drives();
// The disk numbers a folder's volume lies on (empty when unknown).
std::vector<int> disk_numbers(const std::string& path);

const char* to_string(DriveHealth::Status status);

// The attribute table in a 512-byte SMART READ DATA answer, with the thresholds from a SMART READ
// THRESHOLDS answer when there is one. Empty when the data does not look like an attribute table.
std::vector<SmartAttribute> parse_smart(const std::uint8_t* values, const std::uint8_t* thresholds);
// The common name of an attribute ("Reallocated sectors"), or empty.
const char* smart_attribute_name(int id);
// Fills smart_health, smart_issues and the details still unknown (temperature, power-on hours,
// an SSD's wear, uncorrectable errors) from d.smart, and turns bad sectors or a failed attribute
// into a warning (or Unhealthy) when the system still called the drive healthy.
//
// The health starts at 100% and loses, for each kind of trouble, weight * log2(1 + count):
// reallocated sectors 6, pending (unstable) sectors 8, offline uncorrectable sectors 8, spin
// retries 10, end-to-end errors 10, reported uncorrectable errors 4, reallocation events 2. So
// the first bad sectors cost the most, as they are the first sign; one reallocated sector gives
// 94%, ten give 79%, a hundred 60%. An attribute at or below its threshold caps it at 10%.
void apply_smart(DriveHealth& d);

// Fills in from an NVMe drive's SMART / Health Information log (log page 02h, `size` at least 512
// bytes, little-endian as the drive sends it): the wear (percentage used), the temperature, the
// power-on hours and the media errors where still unknown, and turns a critical warning into a
// warning when the drive was called healthy. Returns false when the log is too short.
bool apply_nvme_log(DriveHealth& d, const std::uint8_t* log, std::size_t size);

// Fills in from the JSON of "smartctl -j -a <device>" (smartmontools 7 or later): the model,
// serial and size where unknown, the drive's own verdict, an ATA drive's attribute table (then
// apply_smart) or an NVMe drive's health log (then apply_nvme_log). Returns false when the output
// holds no health data; `problem` then gets smartctl's own error message, if it gave one.
bool apply_smartctl_json(DriveHealth& d, std::string_view json, std::string* problem = nullptr);

}  // namespace vaultik::os
