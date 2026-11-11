#include "vaultik/jobs/computers.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

using json = nlohmann::json;

constexpr char kStatusDir[] = "status";
constexpr char kRequestDir[] = "status/requests";
constexpr char kStatusAadPrefix[] = "VKSTAT01";
constexpr char kRequestAadPrefix[] = "VKREQ001";
constexpr char kHostIdPrefix[] = "VKHOST01";
constexpr int kStatusFormat = 1;
constexpr std::size_t kMaxStatusSize = 1u << 20;

Bytes status_aad(const Id& computer) {
    Bytes aad(kStatusAadPrefix, kStatusAadPrefix + std::strlen(kStatusAadPrefix));
    aad.insert(aad.end(), computer.begin(), computer.end());
    return aad;
}

Bytes request_aad(const Id& computer) {
    Bytes aad(kRequestAadPrefix, kRequestAadPrefix + std::strlen(kRequestAadPrefix));
    aad.insert(aad.end(), computer.begin(), computer.end());
    return aad;
}

// "status/requests/<64 hex>" -> id.
std::optional<Id> parse_request_name(const std::string& name) {
    const std::string prefix = std::string(kRequestDir) + "/";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 64) return std::nullopt;
    try {
        return id_from_hex(std::string_view(name).substr(prefix.size()));
    } catch (const Error&) {
        return std::nullopt;
    }
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

// A status record or request is small: one that is not is not read (it is checked before it is
// downloaded, so a huge file put there by the storage side costs nothing).
Bytes get_small(Repository& repo, const std::string& name, const char* what) {
    if (repo.backend().size(name) > kMaxStatusSize) throw Error(std::string(what) + " too large");
    return repo.backend().get(name);
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
    return open_status(repo, computer, get_small(repo, name, "status record"));
}

std::vector<StatusFile> list_statuses(Repository& repo) {
    std::vector<StatusFile> out;
    for (const std::string& name : repo.backend().list(kStatusDir)) {
        const std::optional<Id> id = parse_status_name(name);
        if (!id) continue;  // a temporary file, or something not ours
        StatusFile f;
        f.name = name;
        try {
            f.status = open_status(repo, *id, get_small(repo, name, "status record"));
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
    const std::string written = job_json(job).dump();
    constexpr int kAttempts = 5;
    for (int attempt = 1;; ++attempt) {
        HostStatus status;
        try {
            if (auto old = read_status(repo, computer)) status = std::move(*old);
        } catch (const std::exception&) {
            // unreadable (damaged, or changed by someone without the key): start a new record
        }
        status.host = host;
        status.version = version;
        status.updated = std::max(now, status.updated);
        std::erase_if(status.jobs, [&job](const JobStatus& j) { return j.id == job.id; });
        status.jobs.push_back(job);
        std::sort(status.jobs.begin(), status.jobs.end(), [](const JobStatus& a, const JobStatus& b) {
            return a.name != b.name ? a.name < b.name : a.id < b.id;
        });
        write_status(repo, computer, status);

        // Read it back: another run of this computer may have written its own copy over ours.
        bool kept = false;
        try {
            if (const auto back = read_status(repo, computer))
                kept = std::any_of(back->jobs.begin(), back->jobs.end(),
                                   [&](const JobStatus& j) { return j.id == job.id && job_json(j).dump() == written; });
        } catch (const std::exception&) {
        }
        if (kept || attempt == kAttempts) return;
        std::uint8_t rnd[1];
        random_bytes(rnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50 + rnd[0]));  // the other writer goes first
    }
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
            // A version's time is when its run started; the record is written when it ends.
            if (c.latest != 0 && c.latest > f.status->updated) {
                c.record_behind = true;
                c.health = worst(c.health, Health::Warning);
            }
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
    std::map<std::string, BackupRequest> requests;  // by the asked computer's status name
    for (RequestFile& r : list_requests(repo))
        if (r.request) requests.emplace(r.target, std::move(*r.request));
    std::vector<Snapshot> snapshots;
    for (const Id& id : repo.list_snapshot_ids()) {
        try {
            snapshots.push_back(repo.load_snapshot(id));
        } catch (const std::exception&) {
            // unreadable snapshots are reported by verify
        }
    }
    std::vector<Computer> out = merge_computers(statuses, snapshots, now,
                                                status_name(computer_id(repo, os::machine_id())), os::hostname());
    for (Computer& c : out)
        if (const auto it = requests.find(c.status_name); !c.status_name.empty() && it != requests.end())
            c.request = it->second;
    return out;
}

// ---- forget -------------------------------------------------------------------------------

std::string resolve_computer(const std::vector<StatusFile>& statuses, std::string_view what) {
    const std::string key = lower(what);
    if (key.empty()) throw Error("name the computer to remove (its name or id)");
    const std::string prefix = std::string(kStatusDir) + "/";
    const bool hex = key.size() >= 8 && key.size() <= 64 &&
                     std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    std::vector<const StatusFile*> by_id, by_name;
    for (const StatusFile& f : statuses) {
        const std::string id = f.name.substr(std::min(prefix.size(), f.name.size()));
        if (f.name == what || (hex && id.starts_with(key))) by_id.push_back(&f);
        else if (f.status && lower(f.status->host) == key) by_name.push_back(&f);
    }
    const std::vector<const StatusFile*>& found = !by_id.empty() ? by_id : by_name;
    if (found.empty()) throw Error("no computer called \"" + std::string(what) + "\" has a status note here");
    if (found.size() > 1)
        throw Error("several computers match \"" + std::string(what) +
                    "\"; name one by its id (vaultik computers --json lists them)");
    return found.front()->name;
}

std::string resolve_forget(const std::vector<StatusFile>& statuses, std::string_view what,
                           std::string_view this_record) {
    std::string name = resolve_computer(statuses, what);
    if (name == this_record)
        throw Error("this is this computer's own status note; its next run would write it again");
    return name;
}

std::string forget_computer(Repository& repo, std::string_view what) {
    const std::vector<StatusFile> statuses = list_statuses(repo);
    const std::string name = resolve_forget(statuses, what, status_name(computer_id(repo, os::machine_id())));
    std::string host = "?";
    for (const StatusFile& f : statuses)
        if (f.name == name && f.status) host = f.status->host;
    const Id id = status_computer(name);
    repo.backend().remove(name);
    if (repo.backend().exists(request_name(id))) repo.backend().remove(request_name(id));
    return host;
}

// ---- requests -----------------------------------------------------------------------------

std::string to_json(const BackupRequest& r) {
    const json j = {{"format", kStatusFormat}, {"from", r.from},   {"created", r.created},
                    {"jobs", r.jobs},          {"state", r.state}, {"started", r.started}};
    return j.dump();
}

BackupRequest request_from_json(const std::string& text) {
    try {
        const json j = json::parse(text);
        if (!j.is_object() || get_or<int>(j, "format", 0) < 1) throw Error("malformed backup request");
        BackupRequest r;
        r.from = get_or<std::string>(j, "from", "");
        r.created = get_or<std::uint64_t>(j, "created", 0);
        r.jobs = get_or<std::vector<std::string>>(j, "jobs", {});
        r.state = get_or<std::string>(j, "state", "pending");
        r.started = get_or<std::uint64_t>(j, "started", 0);
        return r;
    } catch (const json::exception& e) {
        throw Error(std::string("malformed backup request: ") + e.what());
    }
}

std::string request_name(const Id& computer) {
    return std::string(kRequestDir) + "/" + id_hex(computer);
}

Id status_computer(const std::string& name) {
    const std::optional<Id> id = parse_status_name(name);
    if (!id) throw Error("not a status note: " + name);
    return *id;
}

void write_request(Repository& repo, const Id& computer, const BackupRequest& request) {
    const std::string text = to_json(request);
    repo.backend().put(request_name(computer),
                       repo.seal_file(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                                      request_aad(computer)));
}

std::optional<BackupRequest> read_request(Repository& repo, const Id& computer) {
    const std::string name = request_name(computer);
    if (!repo.backend().exists(name)) return std::nullopt;
    const Bytes sealed = get_small(repo, name, "backup request");
    if (sealed.size() > kMaxStatusSize) throw Error("backup request too large");
    const Bytes plain = repo.open_file(sealed, request_aad(computer));
    return request_from_json(std::string(plain.begin(), plain.end()));
}

void remove_request(Repository& repo, const Id& computer) {
    const std::string name = request_name(computer);
    if (repo.backend().exists(name)) repo.backend().remove(name);
}

void ask_backup(Repository& repo, const StatusFile& status, std::vector<std::string> jobs, const std::string& from,
                std::uint64_t now) {
    const Id target = status_computer(status.name);
    if (status.name == status_name(computer_id(repo, os::machine_id())))
        throw Error("this is this computer: start its backup here instead");
    if (jobs.empty() && status.status)
        for (const JobStatus& j : status.status->jobs) jobs.push_back(j.id);
    if (jobs.empty()) throw Error("that computer has no jobs here to run");
    BackupRequest r;
    r.from = from;
    r.created = now;
    r.jobs = std::move(jobs);
    write_request(repo, target, r);
}

std::vector<std::string> requested_jobs(const BackupRequest& r, const std::vector<std::string>& local,
                                        std::uint64_t now) {
    std::vector<std::string> out;
    if (r.created > now + kRequestExpiry || (now > r.created && now - r.created > kRequestExpiry)) return out;
    for (const std::string& id : r.jobs)
        if (std::find(local.begin(), local.end(), id) != local.end() &&
            std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    return out;
}

std::optional<BackupRequest> after_run(BackupRequest r, const std::string& job_id) {
    std::erase(r.jobs, job_id);
    if (r.jobs.empty()) return std::nullopt;
    return r;
}

RequestStep request_step(const BackupRequest& r, std::uint64_t taken) {
    if (r.created > taken) return RequestStep::Take;
    // The one taken up is marked "running" here when it is taken up; the same one still
    // "pending" is the asker's copy put back.
    return r.created == taken && r.state == "running" ? RequestStep::Keep : RequestStep::Drop;
}

namespace {

// When the last request taken up from each repository was made: requests-taken.json in the
// settings folder, {"<request name>": created}.
std::string taken_path(const std::string& dir) {
    return os::join(dir, "requests-taken.json");
}

json read_taken(const std::string& dir) {
    try {
        const Bytes data = os::read_file(taken_path(dir), 1u << 20);
        json j = json::parse(std::string(data.begin(), data.end()));
        if (j.is_object()) return j;
    } catch (const std::exception&) {
    }
    return json::object();
}

}  // namespace

std::vector<std::string> take_request(Repository& repo, const std::vector<std::string>& known,
                                      const std::vector<std::string>& runnable, std::uint64_t now,
                                      const std::string& state_dir) noexcept {
    try {
        const Id me = computer_id(repo, os::machine_id());
        std::optional<BackupRequest> r = read_request(repo, me);
        if (!r) return {};
        if (requested_jobs(*r, known, now).empty()) {
            // Expired, or for jobs this computer has not got here (any more): nobody will run it.
            remove_request(repo, me);
            return {};
        }
        const std::string key = request_name(me);
        json taken = read_taken(state_dir);
        const std::uint64_t last = taken.contains(key) && taken[key].is_number_unsigned() ? taken[key].get<std::uint64_t>() : 0;
        switch (request_step(*r, last)) {
            case RequestStep::Drop:
                remove_request(repo, me);
                return {};
            case RequestStep::Keep:
                return {};
            case RequestStep::Take:
                break;
        }
        const std::vector<std::string> ids = requested_jobs(*r, runnable, now);
        // Only jobs with administrator rights asked for: their own runs answer it.
        if (ids.empty()) return {};
        taken[key] = r->created;
        const std::string text = taken.dump(1) + "\n";
        os::write_file_atomic(taken_path(state_dir), Bytes(text.begin(), text.end()), 0600);
        if (r->state != "running") {
            r->state = "running";
            r->started = now;
            write_request(repo, me, *r);
        }
        return ids;
    } catch (...) {
        return {};
    }
}

void finish_request(Repository& repo, const std::string& job_id) noexcept {
    try {
        const Id me = computer_id(repo, os::machine_id());
        const std::optional<BackupRequest> r = read_request(repo, me);
        if (!r || std::find(r->jobs.begin(), r->jobs.end(), job_id) == r->jobs.end()) return;
        if (const std::optional<BackupRequest> left = after_run(*r, job_id)) write_request(repo, me, *left);
        else remove_request(repo, me);
    } catch (...) {
    }
}

std::vector<RequestFile> list_requests(Repository& repo) {
    std::vector<RequestFile> out;
    for (const std::string& name : repo.backend().list(kRequestDir)) {
        const std::optional<Id> id = parse_request_name(name);
        if (!id) continue;
        RequestFile f;
        f.target = status_name(*id);
        try {
            f.request = read_request(repo, *id);
            if (!f.request) continue;  // removed while we looked
        } catch (const std::exception&) {
        }
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace vaultik::jobs
