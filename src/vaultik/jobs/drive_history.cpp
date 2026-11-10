#include "vaultik/jobs/drive_history.hpp"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;
constexpr std::uint64_t kTrimBytes = 1u << 20;
constexpr std::size_t kKeepPerDrive = 400;

std::string history_file(const std::string& dir) {
    return os::join(dir, "drive-history.jsonl");
}

std::optional<std::uint64_t> raw_of(const os::DriveHealth& d, int id) {
    for (const os::SmartAttribute& a : d.smart)
        if (a.id == id) return a.raw & 0xFFFFFFFFull;  // the count is in the low 32 bits
    return std::nullopt;
}

template <class T>
void put(nlohmann::json& j, const char* name, const std::optional<T>& v) {
    if (v) j[name] = *v;
}

template <class T>
std::optional<T> get(const nlohmann::json& j, const char* name) {
    const auto it = j.find(name);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<T>();
}

std::string to_line(const DriveReading& r) {
    nlohmann::json j = {{"time", r.time}, {"key", r.key}, {"model", r.model}};
    put(j, "health", r.health);
    put(j, "reallocated", r.reallocated);
    put(j, "pending", r.pending);
    put(j, "uncorrectable", r.uncorrectable);
    put(j, "read_errors", r.read_errors);
    return j.dump();
}

std::optional<DriveReading> from_line(const std::string& line) {
    try {
        const nlohmann::json j = nlohmann::json::parse(line);
        DriveReading r;
        r.time = j.at("time").get<std::uint64_t>();
        r.key = j.at("key").get<std::string>();
        r.model = j.value("model", std::string());
        r.health = get<int>(j, "health");
        r.reallocated = get<std::uint64_t>(j, "reallocated");
        r.pending = get<std::uint64_t>(j, "pending");
        r.uncorrectable = get<std::uint64_t>(j, "uncorrectable");
        r.read_errors = get<std::uint64_t>(j, "read_errors");
        return r;
    } catch (const std::exception&) {
        return std::nullopt;  // a torn last line after a crash
    }
}

std::vector<DriveReading> load(const std::string& dir) {
    std::vector<DriveReading> out;
    std::string text;
    try {
        const Bytes b = os::read_file(history_file(dir));
        text.assign(b.begin(), b.end());
    } catch (const std::exception&) {
        return out;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos)
            if (auto r = from_line(text.substr(pos, end - pos))) out.push_back(std::move(*r));
        pos = end + 1;
    }
    return out;
}

ByteSpan bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// True when a value known in both differs.
template <class T>
bool differs(const std::optional<T>& a, const std::optional<T>& b) {
    return a && b && *a != *b;
}

bool changed(const DriveReading& last, const DriveReading& r) {
    return differs(last.health, r.health) || differs(last.reallocated, r.reallocated) ||
           differs(last.pending, r.pending) || differs(last.uncorrectable, r.uncorrectable) ||
           differs(last.read_errors, r.read_errors) || (!last.reallocated && r.reallocated);
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

}  // namespace

std::string drive_key(const os::DriveHealth& d) {
    const std::string serial = trim(d.serial);
    if (!serial.empty()) return "sn:" + serial;
    return "model:" + d.model + ":" + std::to_string(d.size);
}

DriveReading reading_of(const os::DriveHealth& d, std::uint64_t now) {
    DriveReading r;
    r.time = now;
    r.key = drive_key(d);
    r.model = d.model;
    r.health = d.health_percent();
    r.reallocated = raw_of(d, 5);
    r.pending = raw_of(d, 197);
    r.uncorrectable = raw_of(d, 198);
    r.read_errors = d.read_errors_uncorrected;
    return r;
}

void record_drives(const std::vector<os::DriveHealth>& drives, const std::string& dir) {
    if (drives.empty()) return;
    try {
        const std::vector<DriveReading> all = load(dir);
        std::map<std::string, const DriveReading*> last;
        for (const DriveReading& r : all) last[r.key] = &r;
        const std::uint64_t now = os::now();
        std::string text;
        for (const os::DriveHealth& d : drives) {
            const DriveReading r = reading_of(d, now);
            if (!r.health && !r.reallocated && !r.read_errors) continue;  // nothing worth keeping
            const auto it = last.find(r.key);
            if (it != last.end() && !changed(*it->second, r) && now < it->second->time + kDay) continue;
            text += to_line(r) + "\n";
        }
        if (text.empty()) return;
        os::make_dirs(dir);
        const std::string path = history_file(dir);
        os::append_file(path, bytes_of(text));

        os::FileInfo info;
        os::ErrorCode err = 0;
        if (os::stat(path, info, err) && info.size > kTrimBytes) {
            const std::vector<DriveReading> again = load(dir);
            std::map<std::string, std::size_t> count;
            std::vector<bool> keep(again.size(), false);
            for (std::size_t i = again.size(); i-- > 0;)
                keep[i] = ++count[again[i].key] <= kKeepPerDrive;
            std::string kept;
            for (std::size_t i = 0; i < again.size(); ++i)
                if (keep[i]) kept += to_line(again[i]) + "\n";
            os::write_file_atomic(path, bytes_of(kept));
        }
    } catch (const std::exception&) {
        // best effort: the history is a help, never a reason for a backup to fail
    }
}

std::vector<DriveReading> drive_history(const std::string& key, const std::string& dir) {
    std::vector<DriveReading> out;
    for (DriveReading& r : load(dir))
        if (r.key == key) out.push_back(std::move(r));
    return out;
}

std::vector<DriveChange> drive_trend(const std::vector<DriveReading>& history, const DriveReading& now,
                                     std::uint64_t window) {
    std::vector<DriveChange> out;
    const std::uint64_t start = now.time > window ? now.time - window : 0;
    // The oldest reading in the window that knows the value.
    const auto baseline = [&](auto field) -> const DriveReading* {
        for (const DriveReading& r : history)
            if (r.key == now.key && r.time >= start && r.time < now.time && (r.*field).has_value()) return &r;
        return nullptr;
    };
    const auto rose = [&](DriveChange::What what, std::optional<std::uint64_t> DriveReading::*field) {
        if (!(now.*field)) return;
        const DriveReading* base = baseline(field);
        if (base && *(now.*field) > *(base->*field))
            out.push_back({what, *(base->*field), *(now.*field), base->time});
    };
    if (now.health) {
        if (const DriveReading* base = baseline(&DriveReading::health); base && *base->health - *now.health >= 5)
            out.push_back({DriveChange::What::Health, static_cast<std::uint64_t>(*base->health),
                           static_cast<std::uint64_t>(*now.health), base->time});
    }
    rose(DriveChange::What::Reallocated, &DriveReading::reallocated);
    rose(DriveChange::What::Pending, &DriveReading::pending);
    rose(DriveChange::What::Uncorrectable, &DriveReading::uncorrectable);
    rose(DriveChange::What::ReadErrors, &DriveReading::read_errors);
    return out;
}

std::string describe(const DriveChange& c) {
    const std::string since = " since " + os::format_time_local(c.since).substr(0, 10);
    const std::string range = " (" + std::to_string(c.from) + " -> " + std::to_string(c.to) + ")";
    const std::string more = std::to_string(c.to - c.from) + " more ";
    switch (c.what) {
        case DriveChange::What::Health:
            return "health fell from " + std::to_string(c.from) + "% to " + std::to_string(c.to) + "%" + since;
        case DriveChange::What::Reallocated: return more + "reallocated sectors" + since + range;
        case DriveChange::What::Pending: return more + "pending sectors" + since + range;
        case DriveChange::What::Uncorrectable: return more + "uncorrectable sectors" + since + range;
        case DriveChange::What::ReadErrors: return more + "unrecoverable read errors" + since + range;
    }
    return {};
}

}  // namespace vaultik::jobs
