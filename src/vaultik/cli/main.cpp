// Vaultik command-line interface.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "vaultik/cli/console.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/lock.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/resources.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/vss.hpp"

#ifdef _WIN32
#include "vaultik/util/win.hpp"
#endif

namespace vaultik::cli {

namespace {

constexpr char kVersion[] = "0.2.0";

// Exit codes.
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitAuth = 2;
constexpr int kExitPartial = 3;  // finished, but some files could not be read or written
constexpr int kExitCancelled = 4;
constexpr int kExitLocked = 5;  // another run holds a conflicting repository lock

constexpr char kUsage[] = R"(Vaultik - deduplicating, compressed, encrypted backups

Usage: vaultik <command> -r <repository> [options]

Commands:
  init                         Create a new repository (asks for a password)
  backup <path>...             Back up folders or files
      --exclude <pattern>      Skip names matching * ? wildcards (repeatable); a
                               pattern containing a path separator matches the
                               full path
      --tag <tag>              Label the snapshot (repeatable)
      --no-parent              Re-read every file instead of reusing unchanged ones
      --parent <snapshot>      Compare against this snapshot instead of the latest
      --vss <mode>             Windows: read from a Volume Shadow Copy, so open and
                               locked files are included and everything is from one
                               moment. auto (default): when running as administrator,
                               otherwise read live with a warning; on: required;
                               off: never. run-job follows the job's setting
  snapshots                    List snapshots
  ls <snapshot> [path]         List a folder inside a snapshot (-R for recursive)
  restore <snapshot>           Restore files
      --target <folder>        Where to restore (required)
      --path <path>            Only this file or folder, e.g. C/Users/me/Documents
      --overwrite              Replace existing files in the target
  verify                       Check repository structure and snapshots
      --read-data              Also read and decrypt every stored chunk
  stats                        Repository size and deduplication summary
  forget                       Remove old snapshots by a keep policy; each rule
                               keeps the newest snapshot of each of the last N
                               periods (local time), applied per job, or per
                               computer and paths for snapshots made without one
      --keep-last <n>          The n newest snapshots
      --keep-hourly <n>        One per hour   --keep-daily <n>    One per day
      --keep-weekly <n>        One per week   --keep-monthly <n>  One per month
      --keep-yearly <n>        One per year
      --tag <tag>              Only snapshots with this tag (repeatable), e.g.
                               job:documents
      --prune                  Run prune afterwards
      --dry-run                Only show what would be removed
  prune                        Delete data no snapshot uses any more
      --dry-run                Only show what would be deleted
  unlock                       Remove locks left by runs that ended abnormally
      --all                    Remove every lock (only when no other run is active)
  jobs                         List saved backup jobs (created in vaultik-gui)
  run-job <job>                Run a saved job by ID or name, with its own
                               sources, destination, password and resource
                               settings; the run is added to the job's history
  schedule <job>               Register the job's schedule with the system
                               scheduler (Task Scheduler or a systemd user timer)
  unschedule <job>             Remove it again (the job itself is kept)

<snapshot> is an ID prefix (at least 4 characters) or "latest".

Resource use (any command):
  --priority <level>           background, low, normal (default) or high. Sets the
                               CPU, disk and RAM priorities below in one go
  --cpu <level>                CPU priority: idle, low, normal, high
  --io <level>                 Disk and network priority: background, low, normal
  --ram <level>                Memory priority: low (Windows reclaims Vaultik's
                               memory first), normal
  --cpu-limit <percent>        Hard cap on total CPU use, 1-100
  --max-memory <size>          Hard cap on memory, e.g. 256M (at least 128M)
  --read-limit <rate>          Max read speed per second, e.g. 50M
  --write-limit <rate>         Max write speed per second, e.g. 20M (also caps
                               uploads to a network share)
Priorities only matter when the PC is busy; limits apply all the time.

Options:
  -r, --repo <folder>          Repository location (or set VAULTIK_REPO)
  --job <job>                  Use a saved job's repository and stored password
  --password-file <file>       Read the password from a file (or set VAULTIK_PASSWORD)
  --json                       Machine-readable output: one JSON event per line;
                               a line "cancel" on stdin stops a backup or restore
  --lock-wait <time>           When another run holds the repository (prune holds
                               it alone), wait this long for it, e.g. 30s, 10m
                               (default: do not wait; run-job: 10m)
  -h, --help                   Show this help
  --version                    Show the version

Exit codes: 0 ok, 1 error, 2 wrong password or damaged data, 3 finished with skipped files,
4 cancelled, 5 repository in use by another run.
)";

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> values;
    std::set<std::string> flags;

    bool has(const char* flag) const { return flags.contains(flag); }

    std::optional<std::string> value(const char* key) const {
        auto it = values.find(key);
        if (it == values.end() || it->second.empty()) return std::nullopt;
        return it->second.back();
    }

    std::vector<std::string> all(const char* key) const {
        auto it = values.find(key);
        return it == values.end() ? std::vector<std::string>{} : it->second;
    }
};

const std::set<std::string> kValueOptions = {
    "--repo",     "--tag", "--exclude", "--target",    "--path",       "--password-file",
    "--parent",   "--priority", "--cpu", "--io",       "--ram",        "--cpu-limit",
    "--max-memory", "--read-limit", "--write-limit", "--job", "--lock-wait", "--vss", "--keep-last",
    "--keep-hourly", "--keep-daily", "--keep-weekly", "--keep-monthly", "--keep-yearly"};
const std::set<std::string> kFlagOptions = {"--json",    "--no-parent", "--overwrite", "--read-data", "--recursive",
                                              "--help",    "--version",   "--dry-run",   "--prune",     "--all"};

Args parse_args(const std::vector<std::string>& argv) {
    Args a;
    const std::size_t argc = argv.size();
    for (std::size_t i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-r") s = "--repo";
        if (s == "-R") s = "--recursive";
        if (s == "-h" || s == "/?") s = "--help";
        if (s.starts_with("--")) {
            const std::size_t eq = s.find('=');
            const std::string key = s.substr(0, eq);
            if (kValueOptions.contains(key)) {
                std::string v;
                if (eq != std::string::npos) {
                    v = s.substr(eq + 1);
                } else {
                    if (i + 1 >= argc) throw Error("missing value for " + key);
                    v = argv[++i];
                }
                a.values[key].push_back(v);
                continue;
            }
            if (kFlagOptions.contains(key) && eq == std::string::npos) {
                a.flags.insert(key);
                continue;
            }
            throw Error("unknown option " + s + " (see vaultik --help)");
        }
        if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

// ---- machine-readable output (--json) -------------------------------------------------------

bool g_json = false;
std::atomic<bool> g_cancel{false};

// One JSON object per line on stdout; the frontend parses these.
void emit(const nlohmann::json& event) {
    out(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

void warning(const std::string& msg) {
    if (g_json) {
        emit({{"event", "warning"}, {"message", msg}});
    } else {
        err("warning: " + msg + "\n");
    }
}

// With --json, a line "cancel" on stdin stops the running backup or restore cleanly.
void start_cancel_watcher() {
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line == "cancel") {
                g_cancel = true;
                return;
            }
        }
    }).detach();
}

// Resource settings from the command line on top of `base` (a job's saved settings, or the
// defaults). --priority replaces only the three priorities, not the limits.
ResourcePolicy resource_policy(const Args& a, ResourcePolicy p = {}) {
    if (auto v = a.value("--priority")) {
        const ResourcePolicy preset = priority_preset(*v);
        p.cpu = preset.cpu;
        p.io = preset.io;
        p.memory = preset.memory;
    }
    if (auto v = a.value("--cpu")) p.cpu = parse_cpu_priority(*v);
    if (auto v = a.value("--io")) p.io = parse_io_priority(*v);
    if (auto v = a.value("--ram")) p.memory = parse_memory_priority(*v);
    if (auto v = a.value("--cpu-limit")) p.cpu_limit_percent = parse_percent(*v);
    if (auto v = a.value("--max-memory")) p.max_memory = parse_size(*v);
    if (auto v = a.value("--read-limit")) p.read_rate = parse_size(*v);
    if (auto v = a.value("--write-limit")) p.write_rate = parse_size(*v);
    return p;
}

std::string describe(const ResourcePolicy& p) {
    std::string s = std::string("CPU ") + to_string(p.cpu) + ", disk " + to_string(p.io) + ", RAM " +
                    to_string(p.memory);
    if (p.cpu_limit_percent) s += ", CPU cap " + std::to_string(p.cpu_limit_percent) + "%";
    if (p.max_memory) s += ", memory cap " + format_bytes(p.max_memory);
    if (p.read_rate) s += ", read " + format_bytes(p.read_rate) + "/s";
    if (p.write_rate) s += ", write " + format_bytes(p.write_rate) + "/s";
    return s;
}

// The saved job named by --job, if any.
std::optional<jobs::Job> job_arg(const Args& a) {
    const auto name = a.value("--job");
    if (!name) return std::nullopt;
    auto job = jobs::JobStore().find(*name);
    if (!job) throw Error("no saved job called " + *name + " (see vaultik jobs)");
    return job;
}

std::string repo_path(const Args& a) {
    if (auto v = a.value("--repo")) return *v;
    if (auto job = job_arg(a)) return job->repository;
    std::string env = os::get_env("VAULTIK_REPO");
    if (!env.empty()) return env;
    throw Error("no repository given: use -r <folder> or set VAULTIK_REPO");
}

// Reads a small file (at most 64 KiB) such as a password file.
std::string read_small_file(const std::string& path, const char* what) {
    os::ErrorCode e = 0;
    os::File f = os::File::open_read(os::absolute_path(path), e);
    if (!f.valid()) throw Error(std::string("cannot read ") + what + " " + path + ": " + os::error_message(e));
    std::string data;
    std::uint8_t buf[4096];
    for (;;) {
        const std::size_t n = f.read(buf, sizeof buf);
        if (n == 0) break;
        data.append(reinterpret_cast<const char*>(buf), n);
        if (data.size() > (64u << 10)) throw Error(std::string(what) + " is too large: " + path);
    }
    secure_wipe(buf, sizeof buf);
    return data;
}

std::string get_password(const Args& a, bool confirm) {
    if (auto file = a.value("--password-file")) {
        std::string pw = read_small_file(*file, "password file");
        while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back();
        if (pw.empty()) throw Error("password file is empty");
        return pw;
    }
    if (auto job = job_arg(a)) {
        std::string stored = jobs::JobStore().password(job->id);
        if (!stored.empty()) return stored;
    }
    const std::string env = os::get_env("VAULTIK_PASSWORD");
    if (!env.empty()) return env;

    std::string pw = read_password("Repository password: ");
    if (confirm) {
        if (pw.size() < 8) throw Error("use a password of at least 8 characters");
        std::string again = read_password("Repeat password: ");
        const bool same = again == pw;
        secure_wipe(again.data(), again.size());
        if (!same) throw Error("passwords do not match");
    }
    if (pw.empty()) throw Error("empty password");
    return pw;
}

std::unique_ptr<Repository> open_repo(const Args& a) {
    auto backend = std::make_unique<LocalBackend>(repo_path(a));
    std::string pw = get_password(a, false);
    status("Opening repository...");
    std::unique_ptr<Repository> repo;
    try {
        repo = Repository::open(std::move(backend), pw);
    } catch (...) {
        secure_wipe(pw.data(), pw.size());
        clear_status();
        throw;
    }
    secure_wipe(pw.data(), pw.size());
    clear_status();
    return repo;
}

// --lock-wait: "90", "30s", "10m" or "2h".
std::chrono::seconds lock_wait(const Args& a, std::chrono::seconds fallback = std::chrono::seconds(0)) {
    const auto v = a.value("--lock-wait");
    if (!v) return fallback;
    std::string s = *v;
    long long unit = 1;
    if (!s.empty()) {
        const char u = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
        if (u == 's' || u == 'm' || u == 'h') {
            unit = u == 's' ? 1 : u == 'm' ? 60 : 3600;
            s.pop_back();
        }
    }
    try {
        std::size_t used = 0;
        const long long n = std::stoll(s, &used);
        if (used == s.size() && n >= 0 && n <= 7 * 24 * 3600) return std::chrono::seconds(n * unit);
    } catch (const std::exception&) {
    }
    throw Error("invalid --lock-wait value " + *v + " (use for example 30s or 10m)");
}

RetentionPolicy retention_arg(const Args& a) {
    RetentionPolicy p;
    auto count = [&](const char* key, int& out) {
        const auto v = a.value(key);
        if (!v) return;
        try {
            std::size_t used = 0;
            const int n = std::stoi(*v, &used);
            if (used == v->size() && n >= 0) {
                out = n;
                return;
            }
        } catch (const std::exception&) {
        }
        throw Error(std::string("invalid value for ") + key + ": " + *v);
    };
    count("--keep-last", p.last);
    count("--keep-hourly", p.hourly);
    count("--keep-daily", p.daily);
    count("--keep-weekly", p.weekly);
    count("--keep-monthly", p.monthly);
    count("--keep-yearly", p.yearly);
    return p;
}

constexpr char kLockLost[] =
    "the repository lock was lost (this computer was probably asleep or stalled for a long time and "
    "another run took over); stopped without saving";

PruneResult prune_and_report(Repository& repo, RepoLock& lock, bool dry_run) {
    PruneOptions o;
    o.dry_run = dry_run;
    o.before_delete = [&lock] {
        if (!lock.held()) throw LockedError(kLockLost);
    };
    o.on_progress = [](const std::string& msg) {
        if (!g_json) status("Prune: " + msg + "...");
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    PruneResult r = run_prune(repo, o);
    if (!g_json) clear_status();
    return r;
}

std::uint64_t space_freed(const PruneResult& r) {
    return r.bytes_deleted > r.bytes_written ? r.bytes_deleted - r.bytes_written : 0;
}

struct SnapshotRef {
    Id id{};
    Snapshot snapshot;
};

std::vector<SnapshotRef> load_all_snapshots(Repository& repo) {
    std::vector<SnapshotRef> all;
    for (const Id& id : repo.list_snapshot_ids()) {
        try {
            all.push_back({id, repo.load_snapshot(id)});
        } catch (const Error& e) {
            err("warning: cannot read snapshot " + id_hex(id).substr(0, 8) + ": " + e.what() + "\n");
        }
    }
    std::sort(all.begin(), all.end(), [](const SnapshotRef& x, const SnapshotRef& y) {
        return x.snapshot.time < y.snapshot.time;
    });
    return all;
}

SnapshotRef resolve_snapshot(Repository& repo, const std::string& ref_w) {
    std::string ref = ref_w;
    for (char& c : ref) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ref == "latest") {
        auto all = load_all_snapshots(repo);
        if (all.empty()) throw Error("the repository has no snapshots yet");
        return all.back();
    }
    if (ref.size() < 4) throw Error("snapshot ID prefix must be at least 4 characters");
    std::vector<Id> matches;
    for (const Id& id : repo.list_snapshot_ids())
        if (id_hex(id).starts_with(ref)) matches.push_back(id);
    if (matches.empty()) throw Error("no snapshot matches " + ref);
    if (matches.size() > 1) throw Error("snapshot ID " + ref + " is ambiguous; give more characters");
    return {matches[0], repo.load_snapshot(matches[0])};
}

std::string short_id(const Id& id) {
    return id_hex(id).substr(0, 8);
}

std::string pad(std::string s, std::size_t width) {
    // Width in code points is close enough for a terminal table.
    std::size_t cps = 0;
    for (const unsigned char c : s) cps += (c & 0xC0) != 0x80;
    if (cps < width) s.append(width - cps, ' ');
    return s;
}

std::string rpad(const std::string& s, std::size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------

int cmd_init(const Args& a) {
    LocalBackend backend(repo_path(a), /*create=*/true);
    std::string pw = get_password(a, true);
    status("Creating repository...");
    Repository::init(backend, pw);
    secure_wipe(pw.data(), pw.size());
    out("Created repository at " + backend.location() + "\n");
    out("Keep the password safe: without it these backups cannot be restored.\n");
    return kExitOk;
}

enum class VssMode { Off, Auto, On };

// --vss auto|on|off. Auto (the Windows default) uses a shadow copy when it can be made.
VssMode vss_arg(const Args& a, VssMode fallback) {
    const auto v = a.value("--vss");
    if (!v) return fallback;
    if (*v == "auto") return VssMode::Auto;
    if (*v == "on") return VssMode::On;
    if (*v == "off") return VssMode::Off;
    throw Error("--vss takes auto, on or off");
}

#ifdef _WIN32
constexpr VssMode kDefaultVss = VssMode::Auto;
#else
constexpr VssMode kDefaultVss = VssMode::Off;
#endif

// Makes shadow copies of the source volumes and points the backup's reads at them. The returned
// object must outlive the backup. Auto falls back to reading live with a warning.
std::unique_ptr<os::ShadowCopy> prepare_shadow_copy(VssMode mode, BackupOptions& o) {
    if (mode == VssMode::Off) return nullptr;
    std::vector<std::string> full;
    for (const std::string& p : o.paths) full.push_back(os::absolute_path(p));
    if (!g_json) status("Creating a shadow copy...");
    std::unique_ptr<os::ShadowCopy> sc;
    try {
        sc = os::ShadowCopy::create(full);
    } catch (const Error& e) {
        if (!g_json) clear_status();
        if (mode == VssMode::On) throw;
        warning(std::string("reading files live, without a shadow copy (open or locked files may be skipped): ") +
                e.what());
        return nullptr;
    }
    if (!g_json) clear_status();
    for (const std::string& s : sc->skipped()) warning("no shadow copy for " + s + "; read live");
    if (sc->volumes().empty()) return nullptr;
    if (!g_json) {
        std::string list;
        for (const std::string& v : sc->volumes()) list += (list.empty() ? "" : ", ") + v;
        err("Reading from a shadow copy of " + list + "\n");
    }
    const os::ShadowCopy* raw = sc.get();
    o.read_path = [raw](const std::string& p) { return raw->map(p); };
    return sc;
}

// Runs a backup with console or JSON reporting. Fills `record` with the outcome (also on failure,
// before rethrowing) and returns the exit code. `lock` is the shared lock held for it.
int backup_and_report(Repository& repo, BackupOptions o, const std::string& repo_location, jobs::RunRecord& record,
                      RepoLock& lock, VssMode vss) {
    // Never back up the repository into itself.
    const std::string repo_full = os::absolute_path(repo_location);
    o.excludes.push_back(repo_full);
    o.excludes.push_back(os::join(repo_full, "*"));

    const auto start = std::chrono::steady_clock::now();
    record.started = os::now();
    o.on_progress = [&](const BackupProgress& p) {
        if (g_json) {
            emit({{"event", "progress"},
                  {"files", p.files},
                  {"dirs", p.dirs},
                  {"bytes", p.bytes},
                  {"new_bytes", p.new_bytes},
                  {"current", p.current}});
            return;
        }
        const double secs = std::max(seconds_since(start), 0.001);
        status(std::to_string(p.files) + " files, " + format_bytes(p.bytes) + " (" +
               format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.bytes) / secs)) + "/s), new " +
               format_bytes(p.new_bytes) + "  " + p.current);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    o.should_cancel = [&lock] { return g_cancel.load() || lock.lost(); };
    o.before_save = [&lock] {
        if (!lock.held()) throw LockedError(kLockLost);
    };

    BackupResult r;
    std::unique_ptr<os::ShadowCopy> shadow;
    try {
        shadow = prepare_shadow_copy(vss, o);
        r = run_backup(repo, o);
    } catch (const Cancelled&) {
        record.finished = os::now();
        if (lock.lost()) {
            record.status = jobs::RunStatus::Failed;
            record.message = kLockLost;
            throw LockedError(kLockLost);
        }
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = e.what();
        throw;
    }
    const SnapshotStats& s = r.snapshot.stats;
    record.finished = os::now();
    record.status = s.errors > 0 ? jobs::RunStatus::Partial : jobs::RunStatus::Success;
    record.snapshot = id_hex(r.snapshot_id);
    record.files = s.files;
    record.bytes = s.bytes;
    record.new_bytes = s.new_bytes;
    record.stored_bytes = s.stored_bytes;
    record.errors = s.errors;
    record.message = s.errors > 0 ? std::to_string(s.errors) + " item(s) could not be read" : "";

    if (g_json) {
        emit({{"event", "done"},
              {"status", jobs::to_string(record.status)},
              {"snapshot", record.snapshot},
              {"parent", r.snapshot.parent ? id_hex(*r.snapshot.parent) : std::string()},
              {"files", s.files},
              {"dirs", s.dirs},
              {"bytes", s.bytes},
              {"new_bytes", s.new_bytes},
              {"stored_bytes", s.stored_bytes},
              {"skipped", s.skipped},
              {"errors", s.errors},
              {"seconds", seconds_since(start)}});
        return s.errors > 0 ? kExitPartial : kExitOk;
    }
    clear_status();
    out("Snapshot " + short_id(r.snapshot_id) + " saved\n");
    out("  files:    " + std::to_string(s.files) + " in " + std::to_string(s.dirs) + " folders, " +
        format_bytes(s.bytes) + "\n");
    out("  new data: " + format_bytes(s.new_bytes) + ", stored " + format_bytes(s.stored_bytes) +
        " after compression and encryption\n");
    if (s.skipped > 0) out("  links/placeholders kept without content: " + std::to_string(s.skipped) + "\n");
    out("  time:     " + format_duration(seconds_since(start)) +
        (r.snapshot.parent ? " (compared with " + short_id(*r.snapshot.parent) + ")" : std::string(" (full)")) +
        "\n");
    if (s.errors > 0) {
        out("  " + std::to_string(s.errors) + " file(s) or folder(s) could not be read; see warnings above\n");
        return kExitPartial;
    }
    return kExitOk;
}

int cmd_backup(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik backup -r <repo> <path>...");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "backup", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted

    BackupOptions o;
    o.paths = a.positional;
    o.excludes = a.all("--exclude");
    o.tags = a.all("--tag");
    o.use_parent = !a.has("--no-parent");
    if (auto p = a.value("--parent")) o.parent = resolve_snapshot(*repo, *p).id;
    jobs::RunRecord record;
    return backup_and_report(*repo, std::move(o), repo_path(a), record, lock, vss_arg(a, kDefaultVss));
}

int cmd_jobs(const Args& /*a*/) {
    const jobs::JobStore store;
    const auto all = store.list();
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& j : all) list.push_back(nlohmann::json::parse(jobs::to_json(j)));
        emit({{"event", "jobs"}, {"jobs", list}});
        return kExitOk;
    }
    if (all.empty()) {
        out("No saved jobs. Create one in vaultik-gui.\n");
        return kExitOk;
    }
    out(pad("ID", 24) + pad("Schedule", 26) + pad("Last run", 30) + "Repository\n");
    for (const auto& j : all) {
        const auto h = store.history(j.id, 1);
        const std::string last =
            h.empty() ? "never" : os::format_time_local(h.back().started) + " " + jobs::to_string(h.back().status);
        out(pad(j.id, 24) + pad(jobs::describe(j.schedule), 26) + pad(last, 30) + j.repository + "\n");
    }
    return kExitOk;
}

int cmd_schedule(const Args& a, bool install) {
    if (a.positional.empty()) throw Error(std::string("usage: vaultik ") + (install ? "schedule" : "unschedule") + " <job>");
    const auto job = jobs::JobStore().find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    if (!install || job->schedule.kind == jobs::Schedule::Kind::Manual) {
        jobs::remove_schedule(job->id);
        out("Job \"" + job->name + "\" has no system schedule now.\n");
        return kExitOk;
    }
    jobs::apply_schedule(*job, os::parent_of(os::executable_path()));
    out("Job \"" + job->name + "\" scheduled: " + jobs::describe(job->schedule) + "\n");
    return kExitOk;
}

// After a job's backup: removes its snapshots beyond its retention, then the data nothing uses any
// more. Never fails the run; a problem becomes a warning and a note for the history.
std::string clean_up(Repository& repo, const jobs::Job& job) {
    try {
        // Exclusive, without waiting: if another run is busy, cleaning up can wait for next time.
        RepoLock lock(repo, RepoLock::Mode::Exclusive, "cleanup (job " + job.id + ")");
        if (!g_json) status("Removing old snapshots...");
        ForgetOptions fo;
        fo.policy = job.retention;
        fo.tags = {"job:" + job.id};
        const ForgetResult fr = run_forget(repo, fo);
        if (!g_json) clear_status();
        if (fr.removed == 0) return {};
        const PruneResult pr = prune_and_report(repo, lock, false);
        const std::string note = "removed " + std::to_string(fr.removed) + " old snapshot(s), freed " +
                                 format_bytes(space_freed(pr));
        if (g_json) {
            emit({{"event", "cleanup"}, {"removed", fr.removed}, {"freed", space_freed(pr)}, {"message", note}});
        } else {
            out("Cleanup: " + note + " (keeping " + describe(job.retention) + ")\n");
        }
        return note;
    } catch (const LockedError&) {
        const std::string note = "old snapshots were not removed this time: another run is using the repository";
        warning(note);
        return note;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        const std::string note = std::string("removing old snapshots failed: ") + e.what();
        warning(note);
        return note;
    }
}

int cmd_run_job(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik run-job <job id or name>");
    const jobs::JobStore store;
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");

    // The job's own resource settings, unless overridden on the command line.
    const ResourcePolicy policy = resource_policy(a, job->resources);
    apply_resource_policy(policy);
    if (!policy.is_default() && !g_json) err("Resources: " + describe(policy) + "\n");

    jobs::RunRecord record;
    record.started = os::now();
    int code = kExitError;
    try {
        std::string pw = store.password(job->id);
        if (pw.empty()) pw = get_password(a, false);
        status("Opening repository...");
        std::unique_ptr<Repository> repo;
        try {
            repo = Repository::open(std::make_unique<LocalBackend>(job->repository), pw);
        } catch (...) {
            secure_wipe(pw.data(), pw.size());
            clear_status();
            throw;
        }
        secure_wipe(pw.data(), pw.size());
        clear_status();
        if (!g_json) out("Job \"" + job->name + "\": backing up to " + job->repository + "\n");

        BackupOptions o;
        o.paths = job->sources;
        o.excludes = job->excludes;
        o.tags = job->tags;
        o.tags.push_back("job:" + job->id);
        {
            // A scheduled run waits a while for a prune of the same repository to finish.
            RepoLock lock(*repo, RepoLock::Mode::Shared, "backup (job " + job->id + ")",
                          lock_wait(a, std::chrono::minutes(10)));
            repo->reload_index();  // a prune may have finished before the lock was granted
            code = backup_and_report(*repo, std::move(o), job->repository, record, lock,
                                     vss_arg(a, job->vss ? kDefaultVss : VssMode::Off));
        }
        if (!job->retention.empty()) {
            const std::string note = clean_up(*repo, *job);
            if (!note.empty()) record.message += (record.message.empty() ? "" : "; ") + note;
        }
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the backup started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        throw;
    }
    store.append_history(job->id, record);
    return code;
}

int cmd_forget(const Args& a) {
    ForgetOptions fo;
    fo.policy = retention_arg(a);
    fo.tags = a.all("--tag");
    fo.dry_run = a.has("--dry-run");
    if (fo.policy.empty()) throw Error("give at least one --keep-... rule; forget never removes every snapshot");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "forget", lock_wait(a));
    const ForgetResult fr = run_forget(*repo, fo);

    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const ForgetDecision& d : fr.decisions)
            list.push_back({{"id", id_hex(d.entry.id)}, {"time", d.entry.snapshot.time}, {"keep", d.keep}, {"reason", d.reason}});
        emit({{"event", "forget"}, {"dry_run", fo.dry_run}, {"removed", fr.removed}, {"snapshots", list}});
    } else {
        for (const ForgetDecision& d : fr.decisions) {
            out(std::string(d.keep ? "keep    " : (fo.dry_run ? "remove? " : "removed ")) + short_id(d.entry.id) + "  " +
                os::format_time_local(d.entry.snapshot.time) + "  " + d.reason + "\n");
        }
        out(std::to_string(fr.removed) + " of " + std::to_string(fr.decisions.size()) + " snapshot(s) " +
            (fo.dry_run ? "would be removed" : "removed") + " (keeping " + describe(fo.policy) + ")\n");
    }
    if (a.has("--prune") && fr.removed > 0 && !fo.dry_run) {
        const PruneResult pr = prune_and_report(*repo, lock, false);
        if (g_json) {
            emit({{"event", "prune"}, {"freed", space_freed(pr)}});
        } else {
            out("Prune: freed " + format_bytes(space_freed(pr)) + "\n");
        }
    }
    return kExitOk;
}

int cmd_prune(const Args& a) {
    const bool dry = a.has("--dry-run");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "prune", lock_wait(a));
    const auto start = std::chrono::steady_clock::now();
    const PruneResult r = prune_and_report(*repo, lock, dry);
    if (g_json) {
        emit({{"event", "done"},
              {"status", "success"},
              {"dry_run", dry},
              {"packs", r.packs},
              {"packs_deleted", r.packs_deleted},
              {"packs_repacked", r.packs_repacked},
              {"bytes_before", r.bytes_before},
              {"freed", space_freed(r)}});
        return kExitOk;
    }
    out(std::to_string(r.blobs_used) + " chunk(s) in use, " + std::to_string(r.blobs_unused) + " unused, in " +
        std::to_string(r.packs) + " pack(s) (" + format_bytes(r.bytes_before) + ")\n");
    if (dry) {
        out("Would delete " + std::to_string(r.packs_deleted) + " pack(s) and rewrite " +
            std::to_string(r.packs_repacked) + ", freeing about " + format_bytes(r.bytes_deleted) + "\n");
        return kExitOk;
    }
    out("Deleted " + std::to_string(r.packs_deleted) + " pack(s), rewrote " + std::to_string(r.packs_repacked) +
        " into " + std::to_string(r.packs_written) + "; freed " + format_bytes(space_freed(r)) + " in " +
        format_duration(seconds_since(start)) + "\n");
    return kExitOk;
}

int cmd_unlock(const Args& a) {
    auto repo = open_repo(a);
    const std::size_t removed = remove_locks(*repo, a.has("--all"));
    out("Removed " + std::to_string(removed) + " lock(s)\n");
    for (const LockEntry& e : list_locks(*repo))
        out("still held: " + (e.info ? describe(*e.info) : e.name + " (unreadable)") + "\n");
    return kExitOk;
}

int cmd_snapshots(const Args& a) {
    auto repo = open_repo(a);
    const auto all = load_all_snapshots(*repo);
    if (all.empty()) {
        out("No snapshots.\n");
        return kExitOk;
    }
    out(pad("ID", 10) + pad("Time", 21) + pad("Host", 16) + rpad("Size", 12) + rpad("Added", 12) + "  Paths\n");
    for (const auto& [id, s] : all) {
        std::string paths;
        for (const auto& p : s.paths) paths += (paths.empty() ? "" : ", ") + p;
        std::string line = pad(short_id(id), 10) + pad(os::format_time_local(s.time), 21) + pad(s.hostname, 16) +
                           rpad(format_bytes(s.stats.bytes), 12) + rpad(format_bytes(s.stats.stored_bytes), 12) +
                           "  " + paths;
        if (!s.tags.empty()) {
            line += "  [";
            for (std::size_t i = 0; i < s.tags.size(); ++i) line += (i ? "," : "") + s.tags[i];
            line += "]";
        }
        out(line + "\n");
    }
    out(std::to_string(all.size()) + " snapshot(s)\n");
    return kExitOk;
}

void list_tree(Repository& repo, const Id& tree_id, const std::string& prefix, bool recursive) {
    const Tree tree = parse_tree(repo.load_blob(BlobType::Tree, tree_id));
    for (const Node& n : tree.nodes) {
        const char kind = n.kind == NodeKind::Dir       ? 'd'
                          : n.kind == NodeKind::Symlink ? 'l'
                          : n.kind == NodeKind::Reparse ? 'r'
                                                        : '-';
        const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
        const std::string suffix = n.kind == NodeKind::Dir       ? "/"
                                   : n.kind == NodeKind::Symlink ? " -> " + n.link_target
                                                                 : "";
        out(std::string(1, kind) + " " + rpad(n.kind == NodeKind::File ? format_bytes(n.size) : "", 11) + "  " +
            pad(os::format_time_local(n.mtime), 20) + path + suffix + "\n");
        if (recursive && n.kind == NodeKind::Dir) list_tree(repo, n.subtree, path, true);
    }
}

int cmd_ls(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik ls -r <repo> <snapshot> [path]");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "ls", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    const std::string path = a.positional.size() > 1 ? a.positional[1] : std::string();
    Node node;
    if (!resolve_snapshot_path(*repo, ref.snapshot, path, node)) throw Error("path not found in snapshot: " + path);
    if (node.kind == NodeKind::Dir) {
        std::string prefix = path;
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        list_tree(*repo, node.subtree, prefix, a.has("--recursive"));
    } else {
        out("- " + rpad(format_bytes(node.size), 11) + "  " + pad(os::format_time_local(node.mtime), 20) + path +
            "\n");
    }
    return kExitOk;
}

int cmd_restore(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik restore -r <repo> <snapshot> --target <folder>");
    const auto target = a.value("--target");
    if (!target) throw Error("restore needs --target <folder>");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "restore", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);

    RestoreOptions o;
    o.target = *target;
    if (auto p = a.value("--path")) o.subpath = *p;
    o.overwrite = a.has("--overwrite");
    const auto start = std::chrono::steady_clock::now();
    o.on_progress = [&](const RestoreProgress& p) {
        if (g_json) {
            emit({{"event", "progress"}, {"files", p.files}, {"bytes", p.bytes}, {"current", p.current}});
            return;
        }
        const double secs = std::max(seconds_since(start), 0.001);
        status(std::to_string(p.files) + " files, " + format_bytes(p.bytes) + " (" +
               format_bytes(static_cast<std::uint64_t>(static_cast<double>(p.bytes) / secs)) + "/s)  " + p.current);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    o.should_cancel = [] { return g_cancel.load(); };

    const RestoreResult r = run_restore(*repo, ref.snapshot, o);
    if (g_json) {
        emit({{"event", "done"},
              {"status", r.errors > 0 ? "partial" : "success"},
              {"files", r.files},
              {"dirs", r.dirs},
              {"bytes", r.bytes},
              {"skipped", r.skipped},
              {"errors", r.errors},
              {"seconds", seconds_since(start)}});
        return r.errors > 0 ? kExitPartial : kExitOk;
    }
    clear_status();
    out("Restored " + std::to_string(r.files) + " files in " + std::to_string(r.dirs) + " folders, " +
        format_bytes(r.bytes) + " in " + format_duration(seconds_since(start)) + " from snapshot " +
        short_id(ref.id) + "\n");
    if (r.skipped > 0) out("  skipped: " + std::to_string(r.skipped) + " (existing files, links or placeholders)\n");
    if (r.errors > 0) {
        out("  " + std::to_string(r.errors) + " item(s) failed; see warnings above\n");
        return kExitPartial;
    }
    return kExitOk;
}

int cmd_verify(const Args& a) {
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "verify", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted
    VerifyOptions o;
    o.read_data = a.has("--read-data");
    o.on_progress = [](const std::string& msg) { status("Checking " + msg); };
    o.on_error = [](const std::string& msg) { err("error: " + msg + "\n"); };
    const auto start = std::chrono::steady_clock::now();
    const VerifyResult r = run_verify(*repo, o);
    clear_status();
    out("Checked " + std::to_string(r.packs) + " packs, " + std::to_string(r.blobs) + " blobs, " +
        std::to_string(r.snapshots) + " snapshots, " + std::to_string(r.trees) + " folders" +
        (o.read_data ? ", read " + format_bytes(r.bytes_read) : std::string()) + " in " +
        format_duration(seconds_since(start)) + "\n");
    if (r.errors > 0) {
        out(std::to_string(r.errors) + " problem(s) found\n");
        return kExitAuth;
    }
    out(o.read_data ? "No problems found; all data is readable.\n"
                    : "No problems found. Run with --read-data to also check every stored chunk.\n");
    return kExitOk;
}

int cmd_stats(const Args& a) {
    auto repo = open_repo(a);
    const Index& index = repo->index();
    std::uint64_t unique_raw = 0;
    std::uint64_t stored = 0;
    std::uint64_t data_blobs = 0;
    index.for_each([&](const Id&, const BlobLocation& loc) {
        if (loc.type == BlobType::Data) {
            unique_raw += loc.raw_len;
            ++data_blobs;
        }
        stored += loc.length;
    });
    const auto snaps = load_all_snapshots(*repo);
    std::uint64_t logical = 0;
    for (const auto& s : snaps) logical += s.snapshot.stats.bytes;

    out("Repository:      " + repo->backend().location() + "\n");
    out("Snapshots:       " + std::to_string(snaps.size()) + "\n");
    out("Packs:           " + std::to_string(index.packs().size()) + "\n");
    out("Chunks:          " + std::to_string(data_blobs) + "\n");
    out("Data in snapshots (sum of all): " + format_bytes(logical) + "\n");
    out("Unique data:     " + format_bytes(unique_raw) + "\n");
    out("Stored size:     " + format_bytes(stored) + "\n");
    if (stored > 0 && logical > 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1fx", static_cast<double>(logical) / static_cast<double>(stored));
        out("Space saving:    " + std::string(buf) + " (deduplication and compression)\n");
    }
    return kExitOk;
}

int run(const std::vector<std::string>& argv) {
    const Args a = parse_args(argv);
    if (a.has("--version") || a.command == "version") {
        out(std::string("vaultik ") + kVersion + "\n");
        return kExitOk;
    }
    if (a.command.empty() || a.has("--help") || a.command == "help") {
        out(kUsage);
        return a.command.empty() && !a.has("--help") ? kExitError : kExitOk;
    }
    g_json = a.has("--json");
    if (g_json) start_cancel_watcher();

    if (a.command == "jobs") return cmd_jobs(a);
    if (a.command == "run-job") return cmd_run_job(a);  // applies the job's own resource settings
    if (a.command == "schedule") return cmd_schedule(a, true);
    if (a.command == "unschedule") return cmd_schedule(a, false);

    const ResourcePolicy policy = resource_policy(a);
    apply_resource_policy(policy);
    if (!policy.is_default() && !g_json) err("Resources: " + describe(policy) + "\n");

    if (a.command == "init") return cmd_init(a);
    if (a.command == "backup") return cmd_backup(a);
    if (a.command == "snapshots") return cmd_snapshots(a);
    if (a.command == "ls") return cmd_ls(a);
    if (a.command == "restore") return cmd_restore(a);
    if (a.command == "verify") return cmd_verify(a);
    if (a.command == "stats") return cmd_stats(a);
    if (a.command == "forget") return cmd_forget(a);
    if (a.command == "prune") return cmd_prune(a);
    if (a.command == "unlock") return cmd_unlock(a);
    throw Error("unknown command " + a.command + " (see vaultik --help)");
}

}  // namespace

}  // namespace vaultik::cli

namespace {

// Reports a fatal error on stderr and, with --json, as a final "done" event on stdout.
int fail(const char* status, const std::string& message, int code) {
    using namespace vaultik::cli;
    if (g_json) emit({{"event", "done"}, {"status", status}, {"message", message}});
    err((std::string(status) == "cancelled" ? "" : "error: ") + message + "\n");
    return code;
}

int run_main(const std::vector<std::string>& args) {
    using namespace vaultik::cli;
    init_console();
    try {
        return run(args);
    } catch (const vaultik::Cancelled&) {
        return fail("cancelled", "cancelled; no snapshot was saved", kExitCancelled);
    } catch (const vaultik::AuthError& e) {
        return fail("failed", e.what(), kExitAuth);
    } catch (const vaultik::LockedError& e) {
        return fail("failed", e.what(), kExitLocked);
    } catch (const vaultik::Error& e) {
        return fail("failed", e.what(), kExitError);
    } catch (const std::bad_alloc&) {
        return fail("failed", "out of memory (raise or remove --max-memory)", kExitError);
    } catch (const std::exception& e) {
        return fail("failed", std::string("internal error: ") + e.what(), kExitError);
    }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    // Arguments arrive as UTF-16; everything inside Vaultik is UTF-8.
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(vaultik::narrow(argv[i]));
    return run_main(args);
}
#else
int main(int argc, char** argv) {
    return run_main(std::vector<std::string>(argv, argv + argc));
}
#endif
