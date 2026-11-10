#include "vaultik/platform/drive_health.hpp"

// Linux: comes with the port (udisks2 or smartctl).

namespace vaultik::os {

std::vector<DriveHealth> drive_health(const std::string&) {
    return {};
}

std::vector<DriveHealth> all_drives() {
    return {};
}

std::vector<int> disk_numbers(const std::string&) {
    return {};
}

const char* to_string(DriveHealth::Status status) {
    switch (status) {
        case DriveHealth::Status::Healthy: return "healthy";
        case DriveHealth::Status::Warning: return "warning";
        case DriveHealth::Status::Unhealthy: return "unhealthy";
        case DriveHealth::Status::Unknown: break;
    }
    return "unknown";
}

}  // namespace vaultik::os
