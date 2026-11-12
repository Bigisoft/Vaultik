#include "vaultik/platform/drive_health.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

namespace vaultik::os {

namespace {

// A little-endian number of `bytes` bytes (only the low 8 count; the NVMe log's 16-byte counters
// never reach past them).
std::uint64_t little(const std::uint8_t* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = std::min(bytes, 8) - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void put_little(std::uint8_t* p, int bytes, std::uint64_t v) {
    for (int i = 0; i < bytes && i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

}  // namespace

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

const char* to_string(DriveHealth::Status status) {
    switch (status) {
        case DriveHealth::Status::Healthy: return "healthy";
        case DriveHealth::Status::Warning: return "warning";
        case DriveHealth::Status::Unhealthy: return "unhealthy";
        case DriveHealth::Status::Unknown: break;
    }
    return "unknown";
}

bool apply_nvme_log(DriveHealth& d, const std::uint8_t* log, std::size_t size) {
    if (log == nullptr || size < 512) return false;
    // NVMe base specification, "SMART / Health Information": critical warning (byte 0), composite
    // temperature in kelvin (1-2), available spare (3) and its threshold (4), percentage used (5),
    // power-on hours (128-143), media and data integrity errors (160-175).
    if (!d.wear) d.wear = log[5];
    const int kelvin = static_cast<int>(little(log + 1, 2));
    if (!d.temperature && kelvin > 273) d.temperature = kelvin - 273;
    if (!d.power_on_hours)
        if (const std::uint64_t hours = little(log + 128, 16); hours > 0) d.power_on_hours = hours;
    if (!d.read_errors_uncorrected) d.read_errors_uncorrected = little(log + 160, 16);
    const std::uint8_t critical = log[0];
    if (critical != 0 && d.status == DriveHealth::Status::Healthy) {
        d.status = DriveHealth::Status::Warning;
        // The bits of the critical warning, most serious first (several can be set at once).
        static constexpr std::pair<std::uint8_t, const char*> kWarnings[] = {
            {0x04, "Reliability degraded"},
            {0x08, "Read-only"},
            {0x01, "Spare space low"},
            {0x02, "Temperature out of range"},
        };
        if (d.condition.empty()) {
            d.condition = "Critical warning";
            for (const auto& [bit, text] : kWarnings)
                if (critical & bit) {
                    d.condition = text;
                    break;
                }
        }
    }
    return true;
}

bool apply_smartctl_json(DriveHealth& d, std::string_view json, std::string* problem) {
    if (problem != nullptr) problem->clear();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json.begin(), json.end());
    } catch (const std::exception&) {
        if (problem != nullptr) *problem = "smartctl gave no readable answer";
        return false;
    }
    if (!j.is_object()) return false;
    const auto object = [](const nlohmann::json& o, const char* key) -> const nlohmann::json* {
        const auto it = o.find(key);
        return it != o.end() && it->is_object() ? &*it : nullptr;
    };
    const auto text = [](const nlohmann::json& o, const char* key) -> std::string {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    // A count; negative or fractional numbers (never sent for these) give 0 or the whole part.
    const auto number = [](const nlohmann::json& o, const char* key) -> std::optional<std::uint64_t> {
        const auto it = o.find(key);
        if (it == o.end() || !it->is_number()) return std::nullopt;
        if (it->is_number_unsigned()) return it->get<std::uint64_t>();
        if (it->is_number_integer()) return static_cast<std::uint64_t>(std::max<std::int64_t>(0, it->get<std::int64_t>()));
        return static_cast<std::uint64_t>(std::max(0.0, it->get<double>()));
    };

    if (problem != nullptr)
        if (const nlohmann::json* s = object(j, "smartctl")) {
            const auto it = s->find("messages");
            if (it != s->end() && it->is_array())
                for (const nlohmann::json& m : *it)
                    if (m.is_object() && text(m, "severity") == "error" && !text(m, "string").empty()) {
                        *problem = text(m, "string");
                        break;
                    }
        }

    if (d.model.empty()) d.model = text(j, "model_name");
    if (d.serial.empty()) d.serial = text(j, "serial_number");
    if (d.size == 0)
        if (const nlohmann::json* c = object(j, "user_capacity")) d.size = number(*c, "bytes").value_or(0);
    std::string protocol;
    if (const nlohmann::json* dev = object(j, "device")) protocol = text(*dev, "protocol");
    // 0 is an SSD; a spinning disk gives its speed.
    if (const auto rpm = number(j, "rotation_rate"); rpm && protocol == "ATA") d.media = *rpm > 0 ? "HDD" : "SSD";

    const nlohmann::json* verdict = object(j, "smart_status");
    const nlohmann::json* nvme = object(j, "nvme_smart_health_information_log");
    std::vector<SmartAttribute> table;
    if (const nlohmann::json* ata = object(j, "ata_smart_attributes")) {
        const auto it = ata->find("table");
        if (it != ata->end() && it->is_array())
            for (const nlohmann::json& e : *it) {
                if (!e.is_object()) continue;
                SmartAttribute a;
                a.id = static_cast<int>(std::min<std::uint64_t>(number(e, "id").value_or(0), 256));
                if (a.id <= 0 || a.id > 255) continue;
                a.value = static_cast<int>(std::min<std::uint64_t>(number(e, "value").value_or(0), 255));
                a.worst = static_cast<int>(std::min<std::uint64_t>(number(e, "worst").value_or(0), 255));
                a.threshold = static_cast<int>(std::min<std::uint64_t>(number(e, "thresh").value_or(0), 255));
                if (const nlohmann::json* f = object(e, "flags")) {
                    const auto pre = f->find("prefailure");
                    a.prefail = pre != f->end() && pre->is_boolean() && pre->get<bool>();
                }
                if (const nlohmann::json* r = object(e, "raw")) a.raw = number(*r, "value").value_or(0) & 0xFFFFFFFFFFFFull;
                table.push_back(a);
            }
    }
    if (verdict == nullptr && nvme == nullptr && table.empty()) return false;

    // The drive answered: healthy unless what it says shows otherwise.
    if (d.status == DriveHealth::Status::Unknown) d.status = DriveHealth::Status::Healthy;
    if (const nlohmann::json* t = object(j, "temperature"); t != nullptr && !d.temperature)
        if (const auto c = number(*t, "current"); c && *c > 0 && *c < 150) d.temperature = static_cast<int>(*c);
    if (const nlohmann::json* p = object(j, "power_on_time"); p != nullptr && !d.power_on_hours)
        if (const auto h = number(*p, "hours"); h && *h > 0) d.power_on_hours = *h;

    if (nvme != nullptr) {
        // Put back into the log page the drive sent, so it is read by the same rules as a log read
        // directly.
        std::uint8_t log[512] = {};
        log[0] = static_cast<std::uint8_t>(number(*nvme, "critical_warning").value_or(0));
        if (const auto c = number(*nvme, "temperature"); c && *c > 0 && *c < 150) put_little(log + 1, 2, *c + 273);
        log[3] = static_cast<std::uint8_t>(std::min<std::uint64_t>(number(*nvme, "available_spare").value_or(0), 255));
        log[4] = static_cast<std::uint8_t>(std::min<std::uint64_t>(number(*nvme, "available_spare_threshold").value_or(0), 255));
        log[5] = static_cast<std::uint8_t>(std::min<std::uint64_t>(number(*nvme, "percentage_used").value_or(0), 255));
        put_little(log + 128, 8, number(*nvme, "power_on_hours").value_or(0));
        put_little(log + 160, 8, number(*nvme, "media_errors").value_or(0));
        if (d.media.empty()) d.media = "SSD";
        apply_nvme_log(d, log, sizeof log);
    }
    if (!table.empty()) {
        d.smart = std::move(table);
        apply_smart(d);
    }
    if (verdict != nullptr) {
        const auto passed = verdict->find("passed");
        if (passed != verdict->end() && passed->is_boolean() && !passed->get<bool>()) {
            d.status = DriveHealth::Status::Unhealthy;
            if (d.condition.empty()) d.condition = "Failing (the drive's own S.M.A.R.T. verdict)";
        }
    }
    return true;
}

}  // namespace vaultik::os
