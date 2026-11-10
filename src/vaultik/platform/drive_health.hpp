#pragma once

// What the operating system says about the health of the physical drives (Windows: the Storage
// Management API, MSFT_PhysicalDisk; S.M.A.R.T. details where they can be read). Best effort:
// nothing here throws, and an unknown answer is Status::Unknown.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vaultik::os {

struct DriveHealth {
    enum class Status { Healthy, Warning, Unhealthy, Unknown };

    int disk = -1;         // the system's disk number
    std::string model;     // "WD My Passport 2627"
    std::string bus;       // "USB", "SATA", "NVMe", ...
    std::string media;     // "HDD", "SSD" or empty
    std::uint64_t size = 0;
    Status status = Status::Unknown;
    std::string condition;  // the operational state when not plain OK: "Predictive failure", ...
    // S.M.A.R.T. details; Windows only lets administrators read them.
    std::optional<int> temperature;  // degrees Celsius
    std::optional<int> wear;         // percent of an SSD's rated life used
    std::optional<std::uint64_t> power_on_hours;
    std::optional<std::uint64_t> read_errors_uncorrected;
};

// The drives a folder (or file) is on: usually one, several for a volume spanning disks. Empty
// for network shares, cloud locations and when the system cannot tell.
std::vector<DriveHealth> drive_health(const std::string& path);
// Every physical drive of this computer.
std::vector<DriveHealth> all_drives();
// The disk numbers a folder's volume lies on (empty when unknown).
std::vector<int> disk_numbers(const std::string& path);

const char* to_string(DriveHealth::Status status);

}  // namespace vaultik::os
