#include "vaultik/cli/console.hpp"

#include <cstdio>

namespace vaultik::cli {

std::string format_bytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f %s", v, kUnits[unit]);
    }
    return buf;
}

std::string format_duration(double seconds) {
    char buf[32];
    if (seconds < 60) {
        std::snprintf(buf, sizeof buf, "%.1fs", seconds);
    } else {
        const auto total = static_cast<unsigned long long>(seconds);
        std::snprintf(buf, sizeof buf, "%llum%02llus", total / 60, total % 60);
    }
    return buf;
}

}  // namespace vaultik::cli
