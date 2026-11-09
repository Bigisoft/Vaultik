#include "vaultik/jobs/job.hpp"

#include <algorithm>
#include <cstdio>

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

std::string config_dir() {
    const std::string over = os::get_env("VAULTIK_CONFIG_DIR");
    if (!over.empty()) return os::absolute_path(over);
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

void validate(const Job& job) {
    if (job.name.empty()) throw Error("the job needs a name");
    if (!job.id.empty() && !std::all_of(job.id.begin(), job.id.end(), is_id_char))
        throw Error("invalid job id \"" + job.id + "\"");
    if (job.repository.empty()) throw Error("the job needs a backup destination (repository)");
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
    for (const int n : {k.last, k.hourly, k.daily, k.weekly, k.monthly, k.yearly})
        if (n < 0 || n > 10000) throw Error("retention counts must be between 0 and 10000");
}

std::string to_json(const Job& job) {
    const ResourcePolicy& r = job.resources;
    json j = {
        {"version", kJobFormatVersion},
        {"id", job.id},
        {"name", job.name},
        {"repository", job.repository},
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
          {"yearly", job.retention.yearly}}},
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
        }
        validate(job);
        return job;
    } catch (const json::exception& e) {
        throw Error(std::string("invalid job file: ") + e.what());
    }
}

std::string to_json(const RunRecord& r) {
    const json j = {
        {"started", r.started},     {"finished", r.finished},       {"status", to_string(r.status)},
        {"snapshot", r.snapshot},   {"files", r.files},             {"bytes", r.bytes},
        {"new_bytes", r.new_bytes}, {"stored_bytes", r.stored_bytes}, {"errors", r.errors},
        {"message", r.message},
    };
    return j.dump();
}

RunRecord record_from_json(const std::string& text) {
    try {
        const json j = json::parse(text);
        RunRecord r;
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
    os::remove_file(history_path(dir_, id));
}

void JobStore::set_password(const std::string& id, const std::string& password) const {
    check_id(id);
    Bytes sealed = os::protect_secret(bytes_of(password));
    os::write_file_atomic(secret_path(dir_, id), sealed, 0600);
    secure_wipe(sealed.data(), sealed.size());
}

std::string JobStore::password(const std::string& id) const {
    check_id(id);
    Bytes sealed;
    try {
        sealed = os::read_file(secret_path(dir_, id), 64 * 1024);
    } catch (const os::OsError& e) {
        if (os::is_not_found(e.code())) return {};
        throw;
    }
    Bytes plain = os::unprotect_secret(sealed);
    std::string pw(plain.begin(), plain.end());
    secure_wipe(plain.data(), plain.size());
    secure_wipe(sealed.data(), sealed.size());
    return pw;
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
