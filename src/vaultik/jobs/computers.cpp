#include "vaultik/jobs/computers.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

using json = nlohmann::json;

constexpr char kStatusDir[] = "status";
constexpr char kStatusAadPrefix[] = "VKSTAT01";
constexpr char kHostIdPrefix[] = "VKHOST01";
constexpr int kStatusFormat = 1;
constexpr std::size_t kMaxStatusSize = 1u << 20;

Bytes status_aad(const Id& computer) {
    Bytes aad(kStatusAadPrefix, kStatusAadPrefix + std::strlen(kStatusAadPrefix));
    aad.insert(aad.end(), computer.begin(), computer.end());
    return aad;
}

// "status/<64 hex>" -> id, or nothing when the name does not have that shape.
std::optional<Id> parse_status_name(const std::string& name) {
    const std::string prefix = std::string(kStatusDir) + "/";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 64) return std::nullopt;
    try {
        return id_from_hex(std::string_view(name).substr(prefix.size()));
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::tm local_tm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::time_t make_local(std::tm tm) {
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

template <typename T>
T get_or(const json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    return it->get<T>();
}

json job_json(const JobStatus& s) {
    return {{"id", s.id},
            {"name", s.name},
            {"schedule", s.schedule},
            {"last_run", s.last_run},
            {"last_kind", s.last_kind},
            {"last_result", s.last_result},
            {"last_message", s.last_message},
            {"files", s.files},
            {"bytes", s.bytes},
            {"new_bytes", s.new_bytes},
            {"errors", s.errors},
            {"last_backup", s.last_backup},
            {"last_backup_result", s.last_backup_result},
            {"last_success", s.last_success},
            {"next_run", s.next_run},
            {"damage", s.damage},
            {"warnings", s.warnings}};
}

}  // namespace

std::string to_json(const JobStatus& job) {
    return job_json(job).dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string to_json(const HostStatus& status) {
    json jobs = json::array();
    for (const JobStatus& s : status.jobs) jobs.push_back(job_json(s));
    const json j = {{"format", kStatusFormat},
                    {"host", status.host},
                    {"version", status.version},
                    {"updated", status.updated},
                    {"jobs", jobs}};
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

HostStatus host_status_from_json(const std::string& text) {
    try {
        const json j = json::parse(text);
        if (!j.is_object() || get_or<int>(j, "format", 0) < 1) throw Error("malformed status record");
        HostStatus s;
        s.host = get_or<std::string>(j, "host", "");
        s.version = get_or<std::string>(j, "version", "");
        s.updated = get_or<std::uint64_t>(j, "updated", 0);
        if (const auto it = j.find("jobs"); it != j.end() && it->is_array()) {
            for (const json& o : *it) {
                if (!o.is_object()) continue;
                JobStatus js;
                js.id = get_or<std::string>(o, "id", "");
                js.name = get_or<std::string>(o, "name", "");
                js.schedule = get_or<std::string>(o, "schedule", "");
                js.last_run = get_or<std::uint64_t>(o, "last_run", 0);
                js.last_kind = get_or<std::string>(o, "last_kind", "");
                js.last_result = get_or<std::string>(o, "last_result", "");
                js.last_message = get_or<std::string>(o, "last_message", "");
                js.files = get_or<std::uint64_t>(o, "files", 0);
                js.bytes = get_or<std::uint64_t>(o, "bytes", 0);
                js.new_bytes = get_or<std::uint64_t>(o, "new_bytes", 0);
                js.errors = get_or<std::uint64_t>(o, "errors", 0);
                js.last_backup = get_or<std::uint64_t>(o, "last_backup", 0);
                js.last_backup_result = get_or<std::string>(o, "last_backup_result", "");
                js.last_success = get_or<std::uint64_t>(o, "last_success", 0);
                js.next_run = get_or<std::uint64_t>(o, "next_run", 0);
                js.damage = get_or<bool>(o, "damage", false);
                js.warnings = get_or<std::vector<std::string>>(o, "warnings", {});
                s.jobs.push_back(std::move(js));
            }
        }
        return s;
    } catch (const json::exception& e) {
        throw Error(std::string("malformed status record: ") + e.what());
    }
}

Id computer_id(const Repository& repo, std::string_view machine_id) {
    Bytes data(kHostIdPrefix, kHostIdPrefix + std::strlen(kHostIdPrefix));
    const auto& rid = repo.config().repo_id;
    data.insert(data.end(), rid.begin(), rid.end());
    data.insert(data.end(), machine_id.begin(), machine_id.end());
    return repo.keyed_hash(data);
}

std::string status_name(const Id& computer) {
    return std::string(kStatusDir) + "/" + id_hex(computer);
}

Bytes seal_status(Repository& repo, const Id& computer, const HostStatus& status) {
    const std::string text = to_json(status);
    return repo.seal_file(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                          status_aad(computer));
}

HostStatus open_status(Repository& repo, const Id& computer, ByteSpan sealed) {
    if (sealed.size() > kMaxStatusSize) throw Error("status record too large");
    const Bytes plain = repo.open_file(sealed, status_aad(computer));
    return host_status_from_json(std::string(plain.begin(), plain.end()));
}

void write_status(Repository& repo, const Id& computer, const HostStatus& status) {
    // One put: a folder backend writes it under a temporary name and renames it into place, cloud
    // storage replaces the object whole, so readers never see half a record.
    repo.backend().put(status_name(computer), seal_status(repo, computer, status));
}

std::optional<HostStatus> read_status(Repository& repo, const Id& computer) {
    const std::string name = status_name(computer);
    if (!repo.backend().exists(name)) return std::nullopt;
    return open_status(repo, computer, repo.backend().get(name));
}

std::vector<StatusFile> list_statuses(Repository& repo) {
    std::vector<StatusFile> out;
    for (const std::string& name : repo.backend().list(kStatusDir)) {
        const std::optional<Id> id = parse_status_name(name);
        if (!id) continue;  // a temporary file, or something not ours
        StatusFile f;
        f.name = name;
        try {
            f.status = open_status(repo, *id, repo.backend().get(name));
        } catch (const std::exception& e) {
            if (!repo.backend().exists(name)) continue;  // removed while we looked
            f.error = e.what();
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const StatusFile& a, const StatusFile& b) { return a.name < b.name; });
    return out;
}

std::optional<std::uint64_t> next_run(const Schedule& s, std::uint64_t now) {
    if (s.kind == Schedule::Kind::Manual || now < os::kUnixEpochTicks) return std::nullopt;
    const std::time_t t = static_cast<std::time_t>((now - os::kUnixEpochTicks) / 10000000);
    const auto ticks = [](std::time_t v) { return os::from_unix(static_cast<std::int64_t>(v), 0); };
    switch (s.kind) {
        case Schedule::Kind::Manual:
            return std::nullopt;
        case Schedule::Kind::Hourly: {
            std::tm start{};
            start.tm_year = 2026 - 1900;
            start.tm_mday = 1;
            start.tm_min = s.minute;
            const std::time_t first = make_local(start);
            if (first == static_cast<std::time_t>(-1)) return std::nullopt;
            const std::time_t step = static_cast<std::time_t>(std::max(1, s.every_hours)) * 3600;
            if (t < first) return ticks(first);
            return ticks(first + ((t - first) / step + 1) * step);
        }
        case Schedule::Kind::Daily:
        case Schedule::Kind::Weekly: {
            const std::tm today = local_tm(t);
            for (int i = 0; i <= 7; ++i) {
                std::tm day = today;
                day.tm_mday += i;
                day.tm_hour = s.hour;
                day.tm_min = s.minute;
                day.tm_sec = 0;
                const std::time_t when = make_local(day);
                if (when == static_cast<std::time_t>(-1) || when <= t) continue;
                if (s.kind == Schedule::Kind::Weekly) {
                    const int weekday = (local_tm(when).tm_wday + 6) % 7;  // tm: 0 = Sunday
                    if ((s.weekdays & (1u << weekday)) == 0) continue;
                }
                return ticks(when);
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

JobStatus job_status(const Job& job, const std::vector<RunRecord>& history, std::uint64_t now) {
    JobStatus s;
    s.id = job.id;
    s.name = job.name;
    s.schedule = describe(job.schedule);
    if (const auto next = next_run(job.schedule, now)) s.next_run = *next;
    const RunRecord* newest = nullptr;
    for (const RunRecord& r : history)
        if (newest == nullptr || r.finished >= newest->finished) newest = &r;
    if (newest != nullptr) {
        s.last_run = newest->finished;
        s.last_kind = to_string(newest->kind);
        s.last_result = to_string(newest->status);
        s.last_message = newest->message.substr(0, 500);
        s.files = newest->files;
        s.bytes = newest->bytes;
        s.new_bytes = newest->new_bytes;
        s.errors = newest->errors;
    }
    for (const RunRecord& r : history) {
        if (r.kind == RunKind::Backup && (r.status == RunStatus::Success || r.status == RunStatus::Partial))
            s.last_success = std::max(s.last_success, r.finished);
    }
    if (const RunRecord* b = last_backup(history)) {
        s.last_backup = b->finished;
        s.last_backup_result = to_string(b->status);
        if (!b->alert.empty()) s.warnings.push_back("ransomware warning: " + b->alert);
        if (!b->drive.empty()) s.warnings.push_back("backup drive: " + b->drive);
        if (!b->hold.empty()) s.warnings.push_back(b->hold);
    }
    if (const RunRecord* c = last_check(history); c != nullptr && found_damage(*c)) {
        s.damage = true;
        s.warnings.push_back("the health check found damage");
    }
    if (const RunRecord* t = last_restore_test(history);
        t != nullptr && t->status == RunStatus::Failed && t->errors > 0) {
        s.damage = true;
        s.warnings.push_back("the test restore found problems");
    }
    return s;
}

void update_status(Repository& repo, const Id& computer, const std::string& host, const std::string& version,
                   const JobStatus& job, std::uint64_t now) {
    HostStatus status;
    try {
        if (auto old = read_status(repo, computer)) status = std::move(*old);
    } catch (const std::exception&) {
        // unreadable (damaged, or changed by someone without the key): start a new record
    }
    status.host = host;
    status.version = version;
    status.updated = now;
    std::erase_if(status.jobs, [&job](const JobStatus& j) { return j.id == job.id; });
    status.jobs.push_back(job);
    std::sort(status.jobs.begin(), status.jobs.end(), [](const JobStatus& a, const JobStatus& b) {
        return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    write_status(repo, computer, status);
}

std::string publish_status(Repository& repo, const Job& job, const std::vector<RunRecord>& history,
                           const std::string& version) noexcept {
    try {
        const std::uint64_t now = os::now();
        update_status(repo, computer_id(repo, os::machine_id()), os::hostname(), version,
                      job_status(job, history, now), now);
        return {};
    } catch (const std::exception& e) {
        try {
            return e.what();
        } catch (...) {
            return {};
        }
    } catch (...) {
        return "unknown error";
    }
}

const char* to_string(Health health) {
    switch (health) {
        case Health::Ok: return "ok";
        case Health::Unknown: return "unknown";
        case Health::Warning: return "warning";
        case Health::Stale: return "stale";
        case Health::Failed: return "failed";
    }
    return "unknown";
}

Health job_health(const JobStatus& job, std::uint64_t now) {
    if (job.damage || job.last_backup_result == "failed") return Health::Failed;
    if (job.next_run != 0 && now > job.next_run && now - job.next_run > kStaleTicks) return Health::Stale;
    if (job.last_success == 0 || !job.warnings.empty() || job.last_backup_result == "partial" ||
        job.last_backup_result == "cancelled" || job.last_result == "failed")
        return Health::Warning;
    return Health::Ok;
}

std::vector<Computer> merge_computers(const std::vector<StatusFile>& statuses, const std::vector<Snapshot>& snapshots,
                                      std::uint64_t now, std::string_view this_record, std::string_view this_host) {
    struct Versions {
        std::string host;
        std::size_t count = 0;
        std::uint64_t latest = 0;
        std::uint64_t latest_bytes = 0;
        std::uint64_t added = 0;
    };
    std::map<std::string, Versions> by_host;  // by lower-case host name
    for (const Snapshot& s : snapshots) {
        Versions& v = by_host[lower(s.hostname)];
        if (v.host.empty()) v.host = s.hostname;
        ++v.count;
        v.added += s.stats.stored_bytes;
        if (s.time >= v.latest) {
            v.latest = s.time;
            v.latest_bytes = s.stats.bytes;
        }
    }
    const auto add_versions = [](Computer& c, const Versions& v) {
        c.versions = v.count;
        c.latest = v.latest;
        c.latest_bytes = v.latest_bytes;
        c.added_bytes = v.added;
    };

    std::vector<Computer> out;
    std::set<std::string> with_record;
    for (const StatusFile& f : statuses) {
        Computer c;
        c.status_name = f.name;
        c.status = f.status;
        c.this_computer = !this_record.empty() && f.name == this_record;
        if (f.status) {
            c.host = f.status->host;
            c.health = f.status->jobs.empty() ? Health::Unknown : Health::Ok;
            for (const JobStatus& j : f.status->jobs) c.health = worst(c.health, job_health(j, now));
            with_record.insert(lower(c.host));
            if (const auto it = by_host.find(lower(c.host)); it != by_host.end()) add_versions(c, it->second);
        } else {
            c.host = "?";
            c.health = Health::Unknown;
        }
        out.push_back(std::move(c));
    }
    const bool record_of_mine =
        std::any_of(out.begin(), out.end(), [](const Computer& x) { return x.this_computer; });
    for (const auto& [key, v] : by_host) {
        if (with_record.contains(key)) continue;
        Computer c;
        c.host = v.host;
        // Without a record of its own, this computer is the host of its name.
        c.this_computer = !record_of_mine && key == lower(this_host);
        add_versions(c, v);
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Computer& a, const Computer& b) {
        const std::string la = lower(a.host), lb = lower(b.host);
        return la != lb ? la < lb : a.status_name < b.status_name;
    });
    return out;
}

std::vector<Computer> list_computers(Repository& repo, std::uint64_t now) {
    const std::vector<StatusFile> statuses = list_statuses(repo);
    std::vector<Snapshot> snapshots;
    for (const Id& id : repo.list_snapshot_ids()) {
        try {
            snapshots.push_back(repo.load_snapshot(id));
        } catch (const std::exception&) {
            // unreadable snapshots are reported by verify
        }
    }
    return merge_computers(statuses, snapshots, now, status_name(computer_id(repo, os::machine_id())),
                           os::hostname());
}

}  // namespace vaultik::jobs
