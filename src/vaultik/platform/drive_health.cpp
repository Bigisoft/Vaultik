#include "vaultik/platform/drive_health.hpp"

#include <algorithm>
#include <cmath>

namespace vaultik::os {

std::vector<SmartAttribute> parse_smart(const std::uint8_t* values, const std::uint8_t* thresholds) {
    std::vector<SmartAttribute> out;
    if (values == nullptr) return out;
    // The last byte makes the sum of all 512 a multiple of 256. Some drives leave it 0; accept
    // those, but not data that has a checksum and fails it (a bridge's garbage).
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += values[i];
    if ((sum & 0xFF) != 0 && values[511] != 0) return out;
    // 30 entries of 12 bytes from offset 2: id, flags (2), value, worst, raw (6), reserved.
    for (int i = 0; i < 30; ++i) {
        const std::uint8_t* e = values + 2 + i * 12;
        if (e[0] == 0) continue;
        SmartAttribute a;
        a.id = e[0];
        a.prefail = (e[1] & 0x01) != 0;
        a.value = e[3];
        a.worst = e[4];
        for (int b = 5; b >= 0; --b) a.raw = (a.raw << 8) | e[5 + b];
        if (thresholds != nullptr) {
            // The same layout: id, threshold, reserved (10).
            for (int t = 0; t < 30; ++t) {
                const std::uint8_t* te = thresholds + 2 + t * 12;
                if (te[0] == e[0]) {
                    a.threshold = te[1];
                    break;
                }
            }
        }
        out.push_back(a);
    }
    if (out.size() < 3) out.clear();  // not an attribute table worth trusting
    return out;
}

const char* smart_attribute_name(int id) {
    switch (id) {
        case 1: return "Read error rate";
        case 3: return "Spin-up time";
        case 4: return "Start/stop count";
        case 5: return "Reallocated sectors";
        case 7: return "Seek error rate";
        case 9: return "Power-on hours";
        case 10: return "Spin retries";
        case 11: return "Calibration retries";
        case 12: return "Power cycles";
        case 169: return "Remaining life";
        case 173: return "Wear leveling count";
        case 177: return "Wear leveling count";
        case 179: return "Used reserved blocks";
        case 181: return "Program fail count";
        case 182: return "Erase fail count";
        case 183: return "Runtime bad blocks";
        case 184: return "End-to-end errors";
        case 187: return "Reported uncorrectable errors";
        case 188: return "Command timeouts";
        case 190: return "Airflow temperature";
        case 191: return "Shock events";
        case 192: return "Power-off retracts";
        case 193: return "Load cycles";
        case 194: return "Temperature";
        case 195: return "Hardware ECC recovered";
        case 196: return "Reallocation events";
        case 197: return "Pending sectors";
        case 198: return "Offline uncorrectable sectors";
        case 199: return "Interface CRC errors";
        case 200: return "Write error rate";
        case 202: return "Percent lifetime remaining";
        case 231: return "SSD life left";
        case 233: return "Media wearout indicator";
        case 235: return "Good blocks";
        case 241: return "Total LBAs written";
        case 242: return "Total LBAs read";
        default: return "";
    }
}

void apply_smart(DriveHealth& d) {
    if (d.smart.empty()) return;
    const auto find = [&](int id) -> const SmartAttribute* {
        for (const SmartAttribute& a : d.smart)
            if (a.id == id) return &a;
        return nullptr;
    };
    // Counts sit in the low bytes; some drives put other data in the high ones.
    const auto count = [&](int id) -> std::uint64_t {
        const SmartAttribute* a = find(id);
        return a ? (a->raw & 0xFFFFFFFFull) : 0;
    };

    struct Penalty {
        int id;
        double weight;
        const char* one;
        const char* many;
        bool caution;  // worth a warning on its own
    };
    static constexpr Penalty kPenalties[] = {
        {5, 6, "reallocated sector", "reallocated sectors", true},
        {197, 8, "pending sector", "pending sectors", true},
        {198, 8, "offline uncorrectable sector", "offline uncorrectable sectors", true},
        {10, 10, "spin retry", "spin retries", true},
        {184, 10, "end-to-end error", "end-to-end errors", true},
        {187, 4, "reported uncorrectable error", "reported uncorrectable errors", false},
        {196, 2, "reallocation event", "reallocation events", false},
    };
    double penalty = 0;
    bool caution = false;
    d.smart_issues.clear();
    for (const Penalty& p : kPenalties) {
        const std::uint64_t n = count(p.id);
        if (n == 0) continue;
        penalty += p.weight * std::log2(1.0 + static_cast<double>(n));
        caution = caution || p.caution;
        d.smart_issues.push_back(std::to_string(n) + " " + (n == 1 ? p.one : p.many));
    }
    int health = static_cast<int>(std::lround(100.0 - penalty));
    health = std::clamp(health, penalty > 0 ? 1 : 0, 100);

    // The drive's own verdict: a pre-fail attribute at or below its threshold.
    std::string failed;
    for (const SmartAttribute& a : d.smart)
        if (a.prefail && a.threshold > 0 && a.threshold < 0xFE && a.value <= a.threshold) {
            const char* name = smart_attribute_name(a.id);
            failed = *name ? name : "attribute " + std::to_string(a.id);
            break;
        }
    if (!failed.empty()) {
        health = std::min(health, 10);
        d.smart_issues.insert(d.smart_issues.begin(), failed + " failed");
    }
    d.smart_health = health;

    if (!d.temperature) {
        for (const int id : {194, 190}) {
            if (const SmartAttribute* a = find(id)) {
                const int t = static_cast<int>(a->raw & 0xFF);
                if (t > 0 && t < 100) {
                    d.temperature = t;
                    break;
                }
            }
        }
    }
    if (!d.power_on_hours)
        if (const SmartAttribute* a = find(9); a && (a->raw & 0xFFFFFFFFull) > 0) d.power_on_hours = a->raw & 0xFFFFFFFFull;
    if (!d.read_errors_uncorrected) {
        if (find(187)) d.read_errors_uncorrected = count(187);
        else if (find(198)) d.read_errors_uncorrected = count(198);
    }
    // An SSD's life left, as the drive rates it (normalized: 100 when new).
    if (!d.wear && d.media != "HDD") {
        for (const int id : {231, 202, 169, 233, 177, 173}) {
            if (const SmartAttribute* a = find(id); a && a->value > 0 && a->value <= 100) {
                d.wear = 100 - a->value;
                break;
            }
        }
    }

    if (!failed.empty()) {
        d.status = DriveHealth::Status::Unhealthy;
        if (d.condition.empty()) d.condition = failed + " (S.M.A.R.T.)";
    } else if (caution && (d.status == DriveHealth::Status::Healthy || d.status == DriveHealth::Status::Unknown)) {
        d.status = DriveHealth::Status::Warning;
        if (d.condition.empty()) d.condition = d.smart_issues.front();
    }
}

}  // namespace vaultik::os
