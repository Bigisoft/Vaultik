#include "vaultik/jobs/job.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

using nlohmann::json;

constexpr int kJobFormatVersion = 1;
constexpr std::size_t kHistoryTrimBytes = 1u << 20;  // rewrite the history file beyond this
constexpr std::size_t kHistoryKeep = 500;

const char* kWeekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

Schedule::Kind kind_from(const std::string& s) {
    if (s == "manual") return Schedule::Kind::Manual;
    if (s == "hourly") return Schedule::Kind::Hourly;
    if (s == "daily") return Schedule::Kind::Daily;
    if (s == "weekly") return Schedule::Kind::Weekly;
    throw Error("unknown schedule kind \"" + s + "\"");
}

RunStatus status_from(const std::string& s) {
    if (s == "success") return RunStatus::Success;
    if (s == "partial") return RunStatus::Partial;
    if (s == "failed") return RunStatus::Failed;
    if (s == "cancelled") return RunStatus::Cancelled;
    throw Error("unknown run status \"" + s + "\"");
}

std::vector<std::string> strings(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key)) {
        for (const auto& v : j.at(key)) out.push_back(v.get<std::string>());
    }
    return out;
}

bool is_id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

std::string text_of(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

ByteSpan bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

bool is_portable() {
    try {
        return os::exists(os::join(os::parent_of(os::executable_path()), "portable"));
    } catch (const Error&) {
        return false;
    }
}

std::string config_dir() {
    const std::string over = os::get_env("VAULTIK_CONFIG_DIR");
    if (!over.empty()) return os::absolute_path(over);
    if (is_portable()) return os::join(os::parent_of(os::executable_path()), "Settings");
#ifdef _WIN32
    const std::string base = os::get_env("LOCALAPPDATA");
    if (base.empty()) throw Error("LOCALAPPDATA is not set");
    return os::join(base, "Vaultik");
#else
    const std::string xdg = os::get_env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return os::join(xdg, "vaultik");
    const std::string home = os::get_env("HOME");
    if (home.empty()) throw Error("HOME is not set");
    return os::join(os::join(home, ".config"), "vaultik");
#endif
}

std::string make_id(const std::string& name, const std::vector<std::string>& taken) {
    std::string base;
    for (const char c : ascii_lower(name)) {
        if (is_id_char(c) && c != '-') {
            base.push_back(c);
        } else if (!base.empty() && base.back() != '-') {
            base.push_back('-');
        }
        if (base.size() >= 40) break;
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "job";
    std::string id = base;
    for (int n = 2; std::find(taken.begin(), taken.end(), id) != taken.end(); ++n) id = base + "-" + std::to_string(n);
    return id;
}

RetentionPolicy default_retention() {
    RetentionPolicy p;
    p.daily = 7;
    p.weekly = 4;
    p.monthly = 12;
    return p;
}

namespace {

// A folder location on the drive it was recorded on, wherever that drive is connected now.
void relocate_one(std::string& location, const std::string& volume, const std::string& relative) {
    if (volume.empty() || location.empty() || is_cloud_location(location)) return;
    const std::string root = os::volume_root(volume);
    if (root.empty()) return;
    const std::string now = relative.empty() ? root : os::join(root, relative);
    if (os::fold_case(now) != os::fold_case(os::absolute_path(location))) location = now;
}

bool connected_one(const std::string& location, const std::string& volume) {
    return volume.empty() || location.empty() || is_cloud_location(location) || !os::volume_root(volume).empty();
}

// Records the drive of a folder location (see Job::repository_volume).
void record_one(const std::string& location, std::string& volume, std::string& relative) {
    if (location.empty() || is_cloud_location(location) || location.starts_with("\\\\") || location.starts_with("//")) {
        volume.clear();
        relative.clear();
        return;
    }
    // Only a folder that is there: the letter may belong to another drive while this one is away.
    const os::VolumeLocation loc = os::is_directory(location) ? os::volume_of(location) : os::VolumeLocation{};
    if (!loc.volume.empty()) {
        volume = loc.volume;
        relative = loc.relative;
        return;
    }
    // Otherwise the drive is not connected now: keep what was recorded when it was, unless the
    // location now names another folder ("X:\" plus the recorded folder is still the same one).
    const bool drive_form = location.size() >= 3 && location[1] == ':' && (location[2] == '\\' || location[2] == '/');
    if (!drive_form || os::fold_case(location.substr(3)) != os::fold_case(relative)) {
        volume.clear();
        relative.clear();
    }
}

void record_volume(Job& job) {
    record_one(job.repository, job.repository_volume, job.repository_relative);
    record_one(job.second_copy.repository, job.second_copy.volume, job.second_copy.relative);
}

}  // namespace

void relocate(Job& job) {
    relocate_one(job.repository, job.repository_volume, job.repository_relative);
    relocate_one(job.second_copy.repository, job.second_copy.volume, job.second_copy.relative);
}

bool drive_connected(const Job& job) {
    return connected_one(job.repository, job.repository_volume);
}

bool copy_drive_connected(const Job& job) {
    return connected_one(job.second_copy.repository, job.second_copy.volume);
}

void record_copy_volume(Job& job) {
    record_one(job.second_copy.repository, job.second_copy.volume, job.second_copy.relative);
}

bool in_work_hours(const WorkHours& w, int weekday, int minute) {
    if (!w.enabled || weekday < 0 || weekday > 6 || (w.weekdays & (1u << weekday)) == 0) return false;
    if (w.start_minute <= w.end_minute) return minute >= w.start_minute && minute < w.end_minute;
    return minute >= w.start_minute || minute < w.end_minute;  // across midnight
}

std::pair<std::uint64_t, std::uint64_t> rate_limits_now(const ResourcePolicy& normal, const WorkHours& w) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const int weekday = (tm.tm_wday + 6) % 7;  // tm: 0 = Sunday
    if (!in_work_hours(w, weekday, tm.tm_hour * 60 + tm.tm_min)) return {normal.read_rate, normal.write_rate};
    const auto lower = [](std::uint64_t a, std::uint64_t b) { return a == 0 ? b : b == 0 ? a : std::min(a, b); };
    return {lower(normal.read_rate, w.read_rate), lower(normal.write_rate, w.write_rate)};
}

const std::vector<ExclusionPreset>& exclusion_presets() {
    static const std::vector<ExclusionPreset> presets = {
        {"temp", {"*.tmp", "*.temp", "~$*", "*/AppData/Local/Temp", "*/Windows/Temp"}, true},
        {"caches",
         {"*/AppData/Local/*/Cache", "*/AppData/Local/*/Code Cache", "*/AppData/Local/*/GPUCache",
          "*/AppData/Local/*/cache2", "*/AppData/Local/Microsoft/Windows/INetCache", "*/.cache"},
         true},
        {"dev", {"node_modules", "__pycache__", ".pytest_cache", ".mypy_cache"}, false},
        {"clutter",
         {"Thumbs.db", "desktop.ini", "System Volume Information", "pagefile.sys", "hiberfil.sys", "swapfile.sys",
          "*/Windows/SoftwareDistribution/Download"},
         true},
        {"recycle", {"$Recycle.Bin"}, true},
    };
    return presets;
}

HealthPolicy default_health(std::string_view repository) {
    HealthPolicy h;
    h.monthly = !is_cloud_location(repository);
    return h;
}

std::optional<RunKind> due_check(const HealthPolicy& policy, const std::vector<RunRecord>& history,
                                 std::uint64_t now) {
    constexpr std::uint64_t kDay = 864'000'000'000ull;  // FILETIME ticks
    std::uint64_t last_full = 0, last_any = 0;
    for (const RunRecord& r : history) {
        if (!is_check(r.kind) || r.status == RunStatus::Cancelled) continue;
        last_any = std::max(last_any, r.finished);
        if (r.kind == RunKind::FullCheck) last_full = std::max(last_full, r.finished);
    }
    const auto older_than = [now](std::uint64_t at, std::uint64_t age) { return at == 0 || at + age <= now; };
    if (policy.monthly && older_than(last_full, 30 * kDay)) return RunKind::FullCheck;
    if (policy.weekly && older_than(last_any, 7 * kDay)) return RunKind::Check;
    return std::nullopt;
}

void validate(const Job& job) {
    if (job.name.empty()) throw Error("the job needs a name");
    if (!job.id.empty() && !std::all_of(job.id.begin(), job.id.end(), is_id_char))
        throw Error("invalid job id \"" + job.id + "\"");
    if (job.repository.empty()) throw Error("the job needs a backup destination (repository)");
    switch (cloud_kind(job.repository)) {
        case CloudKind::None:
            break;
        case CloudKind::S3:
            parse_s3_location(job.repository);  // throws with what is wrong
            if (job.cloud_key_id.empty()) throw Error("cloud storage needs an access key ID");
            break;
        default:
            cloud_folder(job.repository);
            if (job.cloud_key_id.empty())
                throw Error(std::string("sign in to ") + cloud_kind_name(cloud_kind(job.repository)) + " first");
    }
    if (job.sources.empty()) throw Error("the job needs at least one folder or file to back up");
    for (const std::string& s : job.sources)
        if (s.empty()) throw Error("empty source path");
    const Schedule& s = job.schedule;
    if (s.hour < 0 || s.hour > 23 || s.minute < 0 || s.minute > 59) throw Error("invalid schedule time");
    if (s.kind == Schedule::Kind::Weekly && (s.weekdays & 0x7F) == 0) throw Error("pick at least one weekday");
    if (s.kind == Schedule::Kind::Hourly && (s.every_hours < 1 || s.every_hours > 23))
        throw Error("hourly schedules run every 1 to 23 hours");
    if (job.resources.cpu_limit_percent > 100) throw Error("CPU limit must be between 1 and 100 percent");
    if (job.resources.max_memory != 0 && job.resources.max_memory < kMinMemoryLimit)
        throw Error("memory limit must be at least 128M");
    const RetentionPolicy& k = job.retention;
    for (const int n : {k.last, k.hourly, k.daily, k.weekly, k.monthly, k.yearly, k.within_days})
        if (n < 0 || n > 10000) throw Error("retention counts must be between 0 and 10000");
    if (k.max_bytes != 0 && k.max_bytes < (1ull << 30)) throw Error("a size limit must be at least 1 GB");
    if (job.backup_on_connect && (job.repository_volume.empty() || is_cloud_location(job.repository)))
        throw Error("backing up when the drive is connected needs a destination folder on a local or USB drive");
    // A connected drive starts the run from the user's session, without administrator rights;
    // such a job would then skip what only an administrator can read and still clean up.
    if (job.backup_on_connect && job.run_as_admin)
        throw Error("a job with administrator rights cannot also back up when its drive is connected");
    const WorkHours& w = job.work_hours;
    if (w.start_minute < 0 || w.start_minute >= 24 * 60 || w.end_minute < 0 || w.end_minute >= 24 * 60)
        throw Error("working hours must be times of the day");
    if (w.enabled && (w.weekdays & 0x7F) == 0) throw Error("working hours need at least one day");
    const SecondCopy& sc = job.second_copy;
    if (!sc.repository.empty()) {
        if (os::fold_case(sc.repository) == os::fold_case(job.repository))
            throw Error("the second copy must go to another place than the backup itself");
        if (sc.cloud_lock_days < 0 || sc.cloud_lock_days > 3650) throw Error("the lock must be between 0 and 3650 days");
        if (sc.cloud_lock_days > 0 && cloud_kind(sc.repository) != CloudKind::S3)
            throw Error("locking uploaded data needs S3-compatible storage with Object Lock");
        if (cloud_kind(sc.repository) == CloudKind::S3 && sc.cloud_key_id.empty())
            throw Error("the second copy's cloud storage needs an access key ID");
    }
    if (job.cloud_lock_days < 0 || job.cloud_lock_days > 3650)
        throw Error("the lock must be between 0 and 3650 days");
    if (job.cloud_lock_days > 0 && cloud_kind(job.repository) != CloudKind::S3)
        throw Error("locking uploaded data needs S3-compatible storage with Object Lock");
    if (job.health.sample_percent < 1 || job.health.sample_percent > 100)
        throw Error("the weekly check must read between 1% and 100% of the data");
}

std::string to_json(const Job& job) {
    const ResourcePolicy& r = job.resources;
    json j = {
        {"version", kJobFormatVersion},
        {"id", job.id},
        {"name", job.name},
        {"repository", job.repository},
        {"cloud_key_id", job.cloud_key_id},
        {"cloud_region", job.cloud_region},
        {"cloud_lock_days", job.cloud_lock_days},
        {"cloud_lock_mode", job.cloud_lock_compliance ? "compliance" : "governance"},
        {"sources", job.sources},
        {"excludes", job.excludes},
        {"tags", job.tags},
        {"vss", job.vss},
        {"vss_skip_sql", job.vss_skip_sql},
        {"run_as_admin", job.run_as_admin},
        {"schedule",
         {{"kind", to_string(job.schedule.kind)},
          {"hour", job.schedule.hour},
          {"minute", job.schedule.minute},
          {"weekdays", job.schedule.weekdays},
          {"every_hours", job.schedule.every_hours}}},
        {"resources",
         {{"cpu", vaultik::to_string(r.cpu)},
          {"io", vaultik::to_string(r.io)},
          {"ram", vaultik::to_string(r.memory)},
          {"cpu_limit", r.cpu_limit_percent},
          {"max_memory", r.max_memory},
          {"read_limit", r.read_rate},
          {"write_limit", r.write_rate}}},
        {"retention",
         {{"last", job.retention.last},
          {"hourly", job.retention.hourly},
          {"daily", job.retention.daily},
          {"weekly", job.retention.weekly},
          {"monthly", job.retention.monthly},
          {"yearly", job.retention.yearly},
          {"within_days", job.retention.within_days},
          {"max_bytes", job.retention.max_bytes}}},
        {"volume", {{"id", job.repository_volume}, {"path", job.repository_relative}}},
        {"backup_on_connect", job.backup_on_connect},
        {"second_copy",
         {{"repository", job.second_copy.repository},
          {"cloud_key_id", job.second_copy.cloud_key_id},
          {"cloud_region", job.second_copy.cloud_region},
          {"cloud_lock_days", job.second_copy.cloud_lock_days},
          {"cloud_lock_mode", job.second_copy.cloud_lock_compliance ? "compliance" : "governance"},
          {"volume", job.second_copy.volume},
          {"path", job.second_copy.relative}}},
        {"work_hours",
         {{"enabled", job.work_hours.enabled},
          {"weekdays", job.work_hours.weekdays},
          {"start", job.work_hours.start_minute},
          {"end", job.work_hours.end_minute},
          {"read_limit", job.work_hours.read_rate},
          {"write_limit", job.work_hours.write_rate}}},
        {"health",
         {{"weekly", job.health.weekly},
          {"sample_percent", job.health.sample_percent},
          {"monthly", job.health.monthly}}},
    };
    return j.dump(2) + "\n";
}

Job job_from_json(const std::string& text) {
    try {
        const json j = json::parse(text);
        if (j.value("version", 0) != kJobFormatVersion) throw Error("unsupported job file version");
        Job job;
        job.id = j.at("id").get<std::string>();
        job.name = j.at("name").get<std::string>();
        job.repository = j.at("repository").get<std::string>();
        job.cloud_key_id = j.value("cloud_key_id", std::string());
        job.cloud_region = j.value("cloud_region", std::string());
        job.cloud_lock_days = j.value("cloud_lock_days", 0);
        job.cloud_lock_compliance = j.value("cloud_lock_mode", std::string("governance")) == "compliance";
        job.sources = strings(j, "sources");
        job.excludes = strings(j, "excludes");
        job.tags = strings(j, "tags");
        job.vss = j.value("vss", true);
        job.vss_skip_sql = j.value("vss_skip_sql", false);
        job.run_as_admin = j.value("run_as_admin", false);
        if (j.contains("schedule")) {
            const json& s = j.at("schedule");
            job.schedule.kind = kind_from(s.value("kind", std::string("manual")));
            job.schedule.hour = s.value("hour", 2);
            job.schedule.minute = s.value("minute", 0);
            job.schedule.weekdays = s.value("weekdays", static_cast<std::uint8_t>(0x7F));
            job.schedule.every_hours = s.value("every_hours", 1);
        }
        if (j.contains("resources")) {
            const json& r = j.at("resources");
            job.resources.cpu = parse_cpu_priority(r.value("cpu", std::string("normal")));
            job.resources.io = parse_io_priority(r.value("io", std::string("normal")));
            job.resources.memory = parse_memory_priority(r.value("ram", std::string("normal")));
            job.resources.cpu_limit_percent = r.value("cpu_limit", 0u);
            job.resources.max_memory = r.value("max_memory", std::uint64_t{0});
            job.resources.read_rate = r.value("read_limit", std::uint64_t{0});
            job.resources.write_rate = r.value("write_limit", std::uint64_t{0});
        }
        if (j.contains("retention")) {  // missing in jobs saved before 0.2: keep everything
            const json& k = j.at("retention");
            job.retention.last = k.value("last", 0);
            job.retention.hourly = k.value("hourly", 0);
            job.retention.daily = k.value("daily", 0);
            job.retention.weekly = k.value("weekly", 0);
            job.retention.monthly = k.value("monthly", 0);
            job.retention.yearly = k.value("yearly", 0);
            job.retention.within_days = k.value("within_days", 0);
            job.retention.max_bytes = k.value("max_bytes", std::uint64_t{0});
        }
        if (j.contains("volume")) {
            job.repository_volume = j.at("volume").value("id", std::string());
            job.repository_relative = j.at("volume").value("path", std::string());
        }
        // Saved by 0.5.0 together with administrator rights, which it no longer allows: off.
        job.backup_on_connect = j.value("backup_on_connect", false) && !job.run_as_admin;
        if (j.contains("second_copy")) {
            const json& c = j.at("second_copy");
            job.second_copy.repository = c.value("repository", std::string());
            job.second_copy.cloud_key_id = c.value("cloud_key_id", std::string());
            job.second_copy.cloud_region = c.value("cloud_region", std::string());
            job.second_copy.cloud_lock_days = c.value("cloud_lock_days", 0);
            job.second_copy.cloud_lock_compliance = c.value("cloud_lock_mode", std::string()) == "compliance";
            job.second_copy.volume = c.value("volume", std::string());
            job.second_copy.relative = c.value("path", std::string());
        }
        if (j.contains("work_hours")) {
            const json& w = j.at("work_hours");
            job.work_hours.enabled = w.value("enabled", false);
            job.work_hours.weekdays = w.value("weekdays", static_cast<std::uint8_t>(0x1F));
            job.work_hours.start_minute = w.value("start", 9 * 60);
            job.work_hours.end_minute = w.value("end", 17 * 60);
            job.work_hours.read_rate = w.value("read_limit", std::uint64_t{0});
            job.work_hours.write_rate = w.value("write_limit", std::uint64_t{0});
        }
        job.health = default_health(job.repository);  // missing in jobs saved before 0.5
        if (j.contains("health")) {
            const json& h = j.at("health");
            job.health.weekly = h.value("weekly", job.health.weekly);
            job.health.sample_percent = h.value("sample_percent", job.health.sample_percent);
            job.health.monthly = h.value("monthly", job.health.monthly);
        }
        validate(job);
        return job;
    } catch (const json::exception& e) {
        throw Error(std::string("invalid job file: ") + e.what());
    }
}

std::string to_json(const RunRecord& r) {
    json j = {
        {"started", r.started},     {"finished", r.finished},       {"status", to_string(r.status)},
        {"snapshot", r.snapshot},   {"files", r.files},             {"bytes", r.bytes},
        {"new_bytes", r.new_bytes}, {"stored_bytes", r.stored_bytes}, {"errors", r.errors},
        {"message", r.message},
    };
    if (r.kind != RunKind::Backup) j["kind"] = to_string(r.kind);
    if (r.on_connect) j["trigger"] = "connect";
    if (!r.alert.empty()) j["alert"] = r.alert;
    if (!r.hold.empty()) j["hold"] = r.hold;
    return j.dump();
}

RunRecord record_from_json(const std::string& text) {
    try {
        const json j = json::parse(text);
        RunRecord r;
        const std::string kind = j.value("kind", std::string("backup"));  // missing before 0.5
        r.kind = kind == "full_check" ? RunKind::FullCheck
                 : kind == "check"    ? RunKind::Check
                 : kind == "copy"     ? RunKind::Copy
                                      : RunKind::Backup;
        r.on_connect = j.value("trigger", std::string()) == "connect";
        r.alert = j.value("alert", std::string());
        r.hold = j.value("hold", std::string());
        r.started = j.value("started", std::uint64_t{0});
        r.finished = j.value("finished", std::uint64_t{0});
        r.status = status_from(j.value("status", std::string("failed")));
        r.snapshot = j.value("snapshot", std::string());
        r.files = j.value("files", std::uint64_t{0});
        r.bytes = j.value("bytes", std::uint64_t{0});
        r.new_bytes = j.value("new_bytes", std::uint64_t{0});
        r.stored_bytes = j.value("stored_bytes", std::uint64_t{0});
        r.errors = j.value("errors", std::uint64_t{0});
        r.message = j.value("message", std::string());
        return r;
    } catch (const json::exception& e) {
        throw Error(std::string("invalid run record: ") + e.what());
    }
}

const RunRecord* last_backup(const std::vector<RunRecord>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (it->kind == RunKind::Backup) return &*it;
    return nullptr;
}

const RunRecord* last_check(const std::vector<RunRecord>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (is_check(it->kind)) return &*it;
    return nullptr;
}

const RunRecord* last_copy(const std::vector<RunRecord>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (it->kind == RunKind::Copy) return &*it;
    return nullptr;
}

const char* to_string(RunKind kind) {
    switch (kind) {
        case RunKind::Backup: return "backup";
        case RunKind::Check: return "check";
        case RunKind::FullCheck: return "full_check";
        case RunKind::Copy: return "copy";
    }
    return "backup";
}

const char* to_string(RunStatus status) {
    switch (status) {
        case RunStatus::Success: return "success";
        case RunStatus::Partial: return "partial";
        case RunStatus::Failed: return "failed";
        case RunStatus::Cancelled: return "cancelled";
    }
    return "failed";
}

const char* to_string(Schedule::Kind kind) {
    switch (kind) {
        case Schedule::Kind::Manual: return "manual";
        case Schedule::Kind::Hourly: return "hourly";
        case Schedule::Kind::Daily: return "daily";
        case Schedule::Kind::Weekly: return "weekly";
    }
    return "manual";
}

std::string describe(const Schedule& s) {
    char time[16];
    std::snprintf(time, sizeof time, "%02d:%02d", s.hour, s.minute);
    switch (s.kind) {
        case Schedule::Kind::Manual:
            return "Manual";
        case Schedule::Kind::Hourly: {
            char minute[8];
            std::snprintf(minute, sizeof minute, ":%02d", s.minute);
            return (s.every_hours == 1 ? std::string("Every hour") : "Every " + std::to_string(s.every_hours) + " hours") +
                   " at " + minute;
        }
        case Schedule::Kind::Daily:
            return std::string("Daily at ") + time;
        case Schedule::Kind::Weekly: {
            std::string days;
            if ((s.weekdays & 0x7F) == 0x7F) {
                days = "Every day";
            } else if ((s.weekdays & 0x7F) == 0x1F) {
                days = "Weekdays";
            } else {
                for (int d = 0; d < 7; ++d)
                    if (s.weekdays & (1u << d)) days += (days.empty() ? "" : ", ") + std::string(kWeekdays[d]);
            }
            return days + " at " + time;
        }
    }
    return "Manual";
}

// ---- JobStore -----------------------------------------------------------------------------

JobStore::JobStore(std::string dir) : dir_(std::move(dir)) {}

namespace {

std::string job_path(const std::string& dir, const std::string& id) {
    return os::join(os::join(dir, "jobs"), id + ".json");
}

std::string secret_path(const std::string& dir, const std::string& id) {
    return os::join(os::join(dir, "secrets"), id + ".key");
}

std::string cloud_secret_path(const std::string& dir, const std::string& id) {
    return os::join(os::join(dir, "secrets"), id + ".cloud");
}

std::string copy_secret_path(const std::string& dir, const std::string& id) {
    return os::join(os::join(dir, "secrets"), id + ".copycloud");
}

void seal_to(const std::string& path, const std::string& secret) {
    Bytes sealed = os::protect_secret(bytes_of(secret));
    os::write_file_atomic(path, sealed, 0600);
    secure_wipe(sealed.data(), sealed.size());
}

// Empty when the file does not exist.
std::string unseal_from(const std::string& path) {
    Bytes sealed;
    try {
        sealed = os::read_file(path, 64 * 1024);
    } catch (const os::OsError& e) {
        if (os::is_not_found(e.code())) return {};
        throw;
    }
    Bytes plain = os::unprotect_secret(sealed);
    std::string out(plain.begin(), plain.end());
    secure_wipe(plain.data(), plain.size());
    secure_wipe(sealed.data(), sealed.size());
    return out;
}

std::string history_path(const std::string& dir, const std::string& id) {
    return os::join(os::join(dir, "history"), id + ".jsonl");
}

void check_id(const std::string& id) {
    if (id.empty() || !std::all_of(id.begin(), id.end(), is_id_char)) throw Error("invalid job id \"" + id + "\"");
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos) lines.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return lines;
}

}  // namespace

std::vector<Job> JobStore::list() const {
    std::vector<Job> jobs;
    os::DirReader reader(os::join(dir_, "jobs"));
    std::string name;
    os::FileInfo info;
    while (reader.next(name, info)) {
        if (info.type != os::EntryType::File || !name.ends_with(".json")) continue;
        try {
            jobs.push_back(job_from_json(text_of(os::read_file(os::join(os::join(dir_, "jobs"), name)))));
            relocate(jobs.back());
        } catch (const Error&) {
            // a damaged or foreign file: leave it alone and keep listing
        }
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const Job& a, const Job& b) { return ascii_lower(a.name) < ascii_lower(b.name); });
    return jobs;
}

std::optional<Job> JobStore::find(const std::string& id_or_name) const {
    const std::vector<Job> all = list();
    for (const Job& j : all)
        if (j.id == id_or_name) return j;
    const std::string lname = ascii_lower(id_or_name);
    for (const Job& j : all)
        if (ascii_lower(j.name) == lname) return j;
    return std::nullopt;
}

void JobStore::save(Job& job) const {
    record_volume(job);
    validate(job);
    if (job.id.empty()) {
        std::vector<std::string> taken;
        for (const Job& j : list()) taken.push_back(j.id);
        job.id = make_id(job.name, taken);
    }
    check_id(job.id);
    os::write_file_atomic(job_path(dir_, job.id), bytes_of(to_json(job)));
}

void JobStore::remove(const std::string& id) const {
    check_id(id);
    os::remove_file(job_path(dir_, id));
    os::remove_file(secret_path(dir_, id));
    os::remove_file(cloud_secret_path(dir_, id));
    os::remove_file(copy_secret_path(dir_, id));
    os::remove_file(history_path(dir_, id));
}

void JobStore::set_password(const std::string& id, const std::string& password) const {
    check_id(id);
    seal_to(secret_path(dir_, id), password);
}

std::string JobStore::password(const std::string& id) const {
    check_id(id);
    try {
        return unseal_from(secret_path(dir_, id));
    } catch (const Error&) {
        // Sealed for another account or computer (a portable copy on another PC): ask again.
        return {};
    }
}

void JobStore::set_cloud_secret(const std::string& id, const std::string& secret) const {
    check_id(id);
    if (secret.empty()) {
        os::remove_file(cloud_secret_path(dir_, id));
        return;
    }
    seal_to(cloud_secret_path(dir_, id), secret);
}

std::string JobStore::cloud_secret(const std::string& id) const {
    check_id(id);
    return unseal_from(cloud_secret_path(dir_, id));
}

void JobStore::set_copy_secret(const std::string& id, const std::string& secret) const {
    check_id(id);
    if (secret.empty()) {
        os::remove_file(copy_secret_path(dir_, id));
        return;
    }
    seal_to(copy_secret_path(dir_, id), secret);
}

std::string JobStore::copy_secret(const std::string& id) const {
    check_id(id);
    return unseal_from(copy_secret_path(dir_, id));
}

CloudCredentials JobStore::copy_credentials(const Job& job) const {
    const SecondCopy& sc = job.second_copy;
    if (!is_cloud_location(sc.repository) || job.id.empty()) return {};
    CloudCredentials c{sc.cloud_key_id, copy_secret(job.id), sc.cloud_region};
    c.lock_days = sc.cloud_lock_days;
    c.lock_compliance = sc.cloud_lock_compliance;
    c.on_secret_changed = [dir = dir_, id = job.id](const std::string& secret) {
        try {
            JobStore(dir).set_copy_secret(id, secret);
        } catch (const Error&) {
        }
    };
    return c;
}

CloudCredentials JobStore::cloud_credentials(const Job& job) const {
    if (!is_cloud_location(job.repository) || job.id.empty()) return {};
    CloudCredentials c{job.cloud_key_id, cloud_secret(job.id), job.cloud_region};
    c.lock_days = job.cloud_lock_days;
    c.lock_compliance = job.cloud_lock_compliance;
    // A renewed sign-in token is stored right away, so the next run starts from it.
    c.on_secret_changed = [dir = dir_, id = job.id](const std::string& secret) {
        try {
            JobStore(dir).set_cloud_secret(id, secret);
        } catch (const Error&) {
            // the old token keeps working for a while; the next renewal tries again
        }
    };
    return c;
}

void JobStore::append_history(const std::string& id, const RunRecord& record) const {
    check_id(id);
    const std::string path = history_path(dir_, id);
    os::append_file(path, bytes_of(to_json(record) + "\n"));

    os::FileInfo info;
    os::ErrorCode err = 0;
    if (os::stat(path, info, err) && info.size > kHistoryTrimBytes) {
        std::vector<std::string> lines = lines_of(text_of(os::read_file(path)));
        if (lines.size() > kHistoryKeep) lines.erase(lines.begin(), lines.end() - kHistoryKeep);
        std::string text;
        for (const std::string& l : lines) text += l + "\n";
        os::write_file_atomic(path, bytes_of(text));
    }
}

std::vector<RunRecord> JobStore::history(const std::string& id, std::size_t limit) const {
    check_id(id);
    std::string text;
    try {
        text = text_of(os::read_file(history_path(dir_, id)));
    } catch (const os::OsError& e) {
        if (os::is_not_found(e.code())) return {};
        throw;
    }
    const std::vector<std::string> lines = lines_of(text);
    std::vector<RunRecord> out;
    const std::size_t first = lines.size() > limit ? lines.size() - limit : 0;
    for (std::size_t i = first; i < lines.size(); ++i) {
        try {
            out.push_back(record_from_json(lines[i]));
        } catch (const Error&) {
            // a torn last line after a crash; skip it
        }
    }
    return out;
}

}  // namespace vaultik::jobs
