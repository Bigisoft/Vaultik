// Vaultik command-line interface.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <condition_variable>
#include <mutex>
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
#include "vaultik/ops/repair.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/restore_test.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/keys.hpp"
#include "vaultik/repo/backends.hpp"
#include "vaultik/repo/s3_backend.hpp"
#include "vaultik/ops/copy.hpp"
#include "vaultik/ops/diff.hpp"
#include "vaultik/ops/history.hpp"
#include "vaultik/ops/mount.hpp"
#include "vaultik/repo/lock.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/resources.hpp"
#include "vaultik/version.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/vss.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/drive_history.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/drive_health.hpp"

#ifdef _WIN32
#include <cstdlib>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "vaultik/util/win.hpp"
#endif

namespace vaultik::cli {

namespace {

constexpr char kVersion[] = VAULTIK_VERSION;

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
      --threads <n>            Compress and encrypt on n worker threads (default: up
                               to 4; 0 at background priority or with --max-memory)
      --uploads <n>            Upload n packs in the background (default 2 for cloud
                               storage, 0 for folders; also accepted by run-job)
      --vss <mode>             Windows: read from a Volume Shadow Copy, so open and
                               locked files are included and everything is from one
                               moment. auto (default): when running as administrator,
                               otherwise read live with a warning; on: required;
                               off: never. run-job follows the job's setting
      --vss-skip-sql           Do not pause SQL Server for the shadow copy; its
                               database files are then copied as they would be
                               after a power cut (SQL Server recovers them)
  snapshots                    List snapshots
  ls <snapshot> [path]         List a folder inside a snapshot (-R for recursive)
  mount <snapshot>             Windows: show a version as a read-only folder for File
                               Explorer until Enter is pressed (files are read
                               from the backup when opened)
      --target <folder>        A new or empty folder to use (default: in the
                               settings folder, under browse)
  restore <snapshot>           Restore files
      --target <folder>        Where to restore (required)
      --path <path>            Only this file or folder, e.g. C/Users/me/Documents
      --overwrite              Replace existing files in the target
      --permissions            Windows: give items their stored owner and access
                               list (needs administrator rights for other owners)
      --no-links               Windows: skip symlinks and junctions
  verify                       Check repository structure and snapshots
      --read-data              Also read and decrypt every stored chunk
      --read-share <percent>   With --read-data, only this share of the packs,
                               a different one each time, e.g. 10
  stats                        Repository size and deduplication summary
  forget [<snapshot>...]       Remove the given snapshots, or old snapshots by a
                               keep policy; each rule keeps the newest snapshot of
                               each of the last N periods (local time), applied
                               per job, or per computer and paths for snapshots
                               made without one. Pinned snapshots always stay
                               (--force removes a pinned one given by ID)
      --keep-last <n>          The n newest snapshots
      --keep-within <days>     Every snapshot of the last n days, e.g. 30d
      --keep-hourly <n>        One per hour   --keep-daily <n>    One per day
      --keep-weekly <n>        One per week   --keep-monthly <n>  One per month
      --keep-yearly <n>        One per year
      --tag <tag>              Only snapshots with this tag (repeatable), e.g.
                               job:documents
      --prune                  Run prune afterwards
      --dry-run                Only show what would be removed
  pin <snapshot>               Keep a snapshot whatever the keep policy says
  unpin <snapshot>             Let the keep policy decide again
  note <snapshot> [text]       Describe a snapshot ("before Windows update");
                               no text removes the note
  history <path>               The versions of a file or folder, e.g.
                               C/Users/me/report.docx, where it changed (--all:
                               every snapshot that has it)
  estimate <path>...           How much a first backup reads and roughly how long it
                               takes (--exclude as for backup; --job: the job's
                               sources and exclusions); needs no repository
  diff <older> [<newer>]       Files and folders added (+), removed (-) and changed
                               (M) between two snapshots; with one, since the
                               snapshot before it
  find <pattern>               Files and folders by name in every snapshot; *
                               and ? wildcards, any case; without them, names
                               containing the text
  prune                        Delete data no snapshot uses any more
      --dry-run                Only show what would be deleted
  repair                       Rebuild the index from the stored data, checking
                               every chunk, and save damaged versions again
                               without what was lost (use --job <job> or -r)
      --quick                  Trust the packs' own headers; read no chunks
      --dry-run                Only show what would be done
  unlock                       Remove locks left by runs that ended abnormally
  key passwd                   Set a new password (asks for the current one, or
                               use --recovery-key); saved jobs using this
                               repository get the new password too
  key recovery                 Make a new recovery key, which can set a new
                               password when the old one is forgotten (an older
                               recovery key stops working)
      --recovery-key           Unlock with the recovery key instead of the password
                               (asked for, or set VAULTIK_RECOVERY_KEY)
      --all                    Remove every lock (only when no other run is active)
  cloud-rollback               S3 with Object Lock: bring every repository file
                               back as it was at a time, from the bucket's old
                               versions (after files were deleted or overwritten)
      --to <time>              Local time, e.g. "2026-11-09 20:30"
      --dry-run                Only list what would be brought back
  drives                       The health of this computer's drives, as Windows
                               reports it (details need administrator rights)
      --smart                  Also list each drive's S.M.A.R.T. attributes
      --history                Also list each drive's stored health readings
  jobs                         List saved backup jobs (created in vaultik-gui)
  run-job <job>                Run a saved job by ID or name, with its own
                               sources, destination, password and resource
                               settings; the run is added to the job's history.
                               Then runs the job's health check when one is due
      --no-check               Skip the health check this time
      --on-connect             Run only if the job backs up when its drive is
                               connected, the drive is there, and it is not
                               already backing up or just finished (used by
                               the tray when a drive is plugged in)
  copy <job>                   Copy the job's new versions to its second copy now
                               (run-job does this after each backup), creating it
                               there on first use with the job's password
  check-job <job>              Read the job's share of its stored data back and
                               check every version now (added to its history)
      --full                   Read all of the stored data
  test-restore <job>           Restore a few files of the newest version into a
                               temporary folder, check them (and compare them with
                               the unchanged files on this PC), then remove them
      --files <n>              How many files, at most (default 20)
      --max-size <size>        Their total size, at most (default 500M)
  schedule <job>               Register the job's schedule with the system
                               scheduler (Task Scheduler or a systemd user timer)
  unschedule <job>             Remove it again (the job itself is kept)
  admin-job install|remove     Used by schedule and the frontend for jobs with
                               administrator rights (asks for approval)

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
                               a line "cancel" on stdin stops a backup or restore,
                               "pause" and "resume" pause and continue it
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
    "--keep-hourly", "--keep-daily", "--keep-weekly", "--keep-monthly", "--keep-yearly",
    "--user", "--from", "--keep-within", "--read-share", "--to", "--files", "--max-size", "--threads", "--uploads"};
const std::set<std::string> kFlagOptions = {"--json",    "--no-parent", "--overwrite", "--read-data", "--recursive",
                                              "--help",    "--version",   "--dry-run",   "--prune",     "--all",
                                              "--recovery-key", "--vss-skip-sql", "--admin", "--force",
                                              "--full", "--no-check", "--on-connect", "--smart", "--history", "--quick", "--permissions", "--no-links"};

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
std::atomic<bool> g_stdin_closed{false};  // the program that started this one went away (--json)
std::atomic<bool> g_paused{false};        // "pause" on stdin (--json) until "resume"

// Polled between files and chunks: waits while paused, then tells whether to stop.
bool stop_requested() {
    while (g_paused.load() && !g_cancel.load() && !g_stdin_closed.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return g_cancel.load();
}

// One JSON object per line on stdout; the frontend parses these.
void emit(const nlohmann::json& event) {
    // Whole lines only: the stdin watcher reports pauses from its own thread.
    static std::mutex mu;
    const std::string line = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(mu);
    out(line);
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
            if (line == "pause" || line == "resume") {
                g_paused = line == "pause";
                emit({{"event", g_paused ? "paused" : "resumed"}});
            }
        }
        g_stdin_closed = true;
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

// Cloud storage keys: a saved job's (with --job), otherwise none, so the environment is used.
CloudCredentials cloud_credentials(const Args& a) {
    if (auto job = job_arg(a)) return jobs::JobStore().cloud_credentials(*job);
    return {};
}

std::unique_ptr<Backend> backend_for(const Args& a, bool create = false) {
    const CloudCredentials cloud = cloud_credentials(a);
    return open_backend(repo_path(a), &cloud, create);
}

std::unique_ptr<Repository> open_repo(const Args& a, bool load_index = true) {
    auto backend = backend_for(a);
    std::string pw = get_password(a, false);
    status("Opening repository...");
    std::unique_ptr<Repository> repo;
    try {
        repo = Repository::open(std::move(backend), pw, load_index);
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
    if (auto v = a.value("--keep-within")) {
        // "30" or "30d"
        std::string days = *v;
        if (!days.empty() && (days.back() == 'd' || days.back() == 'D')) days.pop_back();
        int n = -1;
        try {
            std::size_t used = 0;
            n = std::stoi(days, &used);
            if (used != days.size()) n = -1;
        } catch (const std::exception&) {
            n = -1;
        }
        if (n < 0) throw Error("invalid value for --keep-within: " + *v + " (a number of days, e.g. 30d)");
        p.within_days = n;
    }
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
    const auto backend_ptr = backend_for(a, /*create=*/true);
    Backend& backend = *backend_ptr;
    std::string pw = get_password(a, true);
    status("Creating repository...");
    Repository::init(backend, pw);
    const std::string recovery = create_recovery_key(backend, {pw});
    secure_wipe(pw.data(), pw.size());
    clear_status();
    out("Created repository at " + backend.location() + "\n");
    out("Recovery key: " + recovery + "\n");
    out("Keep the password safe: without it these backups cannot be restored. Write the\n"
        "recovery key down and keep it apart from this computer: it can set a new password\n"
        "if the password is forgotten.\n");
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
std::unique_ptr<os::ShadowCopy> prepare_shadow_copy(VssMode mode, bool pause_sql_server, BackupOptions& o) {
    if (mode == VssMode::Off) return nullptr;
    std::vector<std::string> full;
    for (const std::string& p : o.paths) full.push_back(os::absolute_path(p));
    if (!g_json) status("Creating a shadow copy...");
    std::unique_ptr<os::ShadowCopy> sc;
    try {
        sc = os::ShadowCopy::create(full, pause_sql_server);
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
// A backup that saw signs of ransomware: the version before it is pinned with a note, so no keep
// rule can remove the last good copy, and the run says so loudly. (The caller holds a lock.)
void raise_alarm(Repository& repo, const BackupResult& r, jobs::RunRecord& record) {
    const ChangeAlarm& a = r.alarm;
    std::string what = std::to_string(a.suspicious()) + " file(s)";
    if (a.parent_files > 0) what += " of " + std::to_string(a.parent_files);
    record.alert = what + " look encrypted or were renamed with an added extension since the previous version";
    if (!a.examples.empty()) record.alert += ", e.g. " + a.examples.front();
    std::string kept;
    if (r.snapshot.parent) {
        try {
            Snapshot before = repo.load_snapshot(*r.snapshot.parent);
            before.pinned = true;
            if (before.note.empty())
                before.note = "Kept automatically: the next backup (" + os::format_time_local(r.snapshot.time) +
                              ") saw many files that look encrypted";
            repo.update_snapshot(*r.snapshot.parent, before);
            kept = short_id(*r.snapshot.parent);
        } catch (const Error& e) {
            warning(std::string("could not pin the previous version: ") + e.what());
        }
    }
    record.alert += kept.empty() ? "." : ". The previous version " + kept + " is pinned so it stays.";
    if (g_json) {
        nlohmann::json examples = nlohmann::json::array();
        for (const std::string& p : a.examples) examples.push_back(p);
        emit({{"event", "alert"},
              {"message", record.alert},
              {"became_random", a.became_random},
              {"renamed", a.renamed},
              {"parent_files", a.parent_files},
              {"pinned", kept},
              {"examples", examples}});
    } else {
        clear_status();
        err("WARNING: " + record.alert + "\n");
        for (const std::string& p : a.examples) err("  " + p + "\n");
        err("If you did not change these files yourself, your computer may have ransomware. Do not trust the new "
            "version; restore from the pinned one.\n");
    }
}

int backup_and_report(Repository& repo, BackupOptions o, const std::string& repo_location, jobs::RunRecord& record,
                      RepoLock& lock, VssMode vss, bool pause_sql_server) {
    // Never back up the repository into itself.
    if (!is_cloud_location(repo_location)) {
        const std::string repo_full = os::absolute_path(repo_location);
        o.excludes.push_back(repo_full);
        o.excludes.push_back(os::join(repo_full, "*"));
    }

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
    o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
    o.before_save = [&lock] {
        if (!lock.held()) throw LockedError(kLockLost);
    };

    BackupResult r;
    std::unique_ptr<os::ShadowCopy> shadow;
    try {
        shadow = prepare_shadow_copy(vss, pause_sql_server, o);
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
    if (r.alarm.raised()) raise_alarm(repo, r, record);
    // A source that is gone (a detached disk, a locked volume, an unreachable share) or a sharp
    // drop in files makes a near-empty version; removing old versions by count or by day would
    // then throw away the good ones.
    if (!r.missing_sources.empty()) {
        record.hold = std::to_string(r.missing_sources.size()) + " backup path(s) could not be read at all (" +
                      r.missing_sources.front() + ")";
    } else if (r.alarm.parent_files >= 20 && s.files * 2 < r.alarm.parent_files) {
        record.hold = "only " + std::to_string(s.files) + " files were found, " + std::to_string(r.alarm.parent_files) +
                      " the time before";
    }
    if (!record.hold.empty()) warning(record.hold + "; old versions are kept this time");

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
              {"alert", record.alert},
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

// How many worker threads compress and encrypt, and how many packs upload in the background:
// --threads / --uploads, or by default up to 4 threads (one core is left for reading and
// hashing; none at background CPU priority or under a memory cap, where the work stays on one
// thread) and, for cloud storage, 2 uploads in flight (each holds a 32 MiB pack in memory).
void apply_parallelism(Repository& repo, const Args& a, const std::string& location, const ResourcePolicy& policy) {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    unsigned threads = std::min(4u, cores > 1 ? cores - 1 : 1u);
    if (policy.cpu == CpuPriority::Idle || policy.max_memory != 0) threads = 0;
    unsigned uploads = is_cloud_location(location) && policy.max_memory == 0 ? 2 : 0;
    if (auto v = a.value("--threads")) threads = static_cast<unsigned>(std::clamp(std::stoi(*v), 0, 16));
    if (auto v = a.value("--uploads")) uploads = static_cast<unsigned>(std::clamp(std::stoi(*v), 0, 8));
    repo.set_parallelism(threads, uploads);
}

int cmd_backup(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik backup -r <repo> <path>...");
    auto repo = open_repo(a);
    apply_parallelism(*repo, a, repo_path(a), resource_policy(a));
    RepoLock lock(*repo, RepoLock::Mode::Shared, "backup", lock_wait(a));
    repo->reload_index();  // a prune may have finished before the lock was granted

    BackupOptions o;
    o.paths = a.positional;
    o.excludes = a.all("--exclude");
    o.tags = a.all("--tag");
    o.use_parent = !a.has("--no-parent");
    if (auto p = a.value("--parent")) o.parent = resolve_snapshot(*repo, *p).id;
    jobs::RunRecord record;
    return backup_and_report(*repo, std::move(o), repo_path(a), record, lock, vss_arg(a, kDefaultVss),
                             !a.has("--vss-skip-sql"));
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
        const auto h = jobs::combined_history(store, j.id, 1);
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
        std::size_t removed = 0;
        if (!job.retention.no_rules()) {
            ForgetOptions fo;
            fo.policy = job.retention;
            fo.tags = {"job:" + job.id};
            removed += run_forget(repo, fo).removed;
        }
        std::string size_note;
        if (job.retention.max_bytes > 0) {
            // The oldest unpinned versions beyond the size limit (never the newest).
            if (!g_json) status("Checking the size limit...");
            const SizePlan plan =
                plan_size_limit(repo, load_snapshot_records(repo), "job:" + job.id, job.retention.max_bytes);
            removed += remove_snapshots(repo, plan.remove);
            if (!plan.fits) {
                size_note = "the backup is " + format_bytes(plan.size_after) + ", over its " +
                            format_bytes(job.retention.max_bytes) + " limit even with only the newest and pinned versions";
                warning(size_note);
            }
        }
        if (!g_json) clear_status();
        if (removed == 0) return size_note;
        const PruneResult pr = prune_and_report(repo, lock, false);
        std::string note = "removed " + std::to_string(removed) + " old snapshot(s), freed " +
                           format_bytes(space_freed(pr));
        if (!size_note.empty()) note += "; " + size_note;
        if (g_json) {
            emit({{"event", "cleanup"}, {"removed", removed}, {"freed", space_freed(pr)}, {"message", note}});
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

// "1.10.2" -> {1, 10, 2}; missing or bad parts are 0.
std::array<int, 3> version_parts(const std::string& v) {
    std::array<int, 3> out{};
    const char* p = v.c_str();
    for (int& part : out) {
        char* end = nullptr;
        part = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p || *end != '.') break;
        p = end + 1;
    }
    return out;
}

// After a backup to an external drive: keeps <drive>\Vaultik\ with the portable programs and a
// note on restoring, so another computer can restore from the drive with nothing installed. Only
// replaces an older copy. Never fails the run.
void refresh_drive_copy(const jobs::Job& job) {
#ifdef _WIN32
    if (is_cloud_location(job.repository)) return;
    try {
        // Never with administrator rights: anyone can prepare an external drive, e.g. with
        // "Vaultik" as a link into Windows' own folders, and an elevated run would write there.
        if (os::process_elevated()) return;
        const os::VolumeLocation loc = os::volume_of(job.repository);
        if (loc.volume.empty() || loc.system || !loc.external) return;
        const std::string target = os::join(loc.root, "Vaultik");
        // Only a real folder and real files there (or none yet): links lead somewhere else. Any
        // reparse point counts as a link, also one whose kind cannot be read (fail closed). What
        // is left is a race with someone changing the drive during this user's own backup, who
        // could then only write where this user can anyway.
        constexpr std::uint32_t kReparsePoint = 0x400;  // FILE_ATTRIBUTE_REPARSE_POINT
        const auto plain = [](const std::string& path, os::EntryType want) {
            os::FileInfo info;
            os::ErrorCode err = 0;
            if (!os::stat(path, info, err)) return os::is_not_found(err);
            return info.type == want && (info.attributes & kReparsePoint) == 0;
        };
        const char* const names[] = {"vaultik.exe", "vaultik-gui.exe", "vaultik-task.exe", "portable",
                                     "How to restore.txt", "version"};
        bool safe = plain(target, os::EntryType::Dir);
        for (const char* name : names) safe = safe && plain(os::join(target, name), os::EntryType::File);
        if (!safe) {
            warning(target + " on the backup drive is not a plain folder of Vaultik's files, so it was left alone");
            return;
        }
        const std::string from = os::parent_of(os::executable_path());
        if (os::fold_case(from) == os::fold_case(os::absolute_path(target))) return;  // running from that copy
        const std::string stamp = os::join(target, "version");
        std::string there;
        try {
            const Bytes b = os::read_file(stamp, 64);
            there.assign(b.begin(), b.end());
        } catch (const Error&) {
        }
        if (!there.empty() && version_parts(there) >= version_parts(VAULTIK_VERSION)) return;

        os::make_dirs(target);
        const auto fs_path = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
        for (const char* exe : {"vaultik.exe", "vaultik-gui.exe", "vaultik-task.exe"}) {
            const std::string src = os::join(from, exe);
            if (!os::exists(src)) continue;
            // Deleted first, then created: overwriting would write through a hard link someone
            // put there in place of the old file, into whatever file it is linked to.
            const std::string dest = os::join(target, exe);
            const os::ErrorCode gone = os::remove_file(dest);
            if (gone != 0 && !os::is_not_found(gone)) os::throw_error("cannot replace " + dest, gone);
            std::filesystem::copy_file(fs_path(src), fs_path(dest), std::filesystem::copy_options::none);
        }
        const auto write = [&](const char* name, const std::string& text) {
            os::write_file_atomic(os::join(target, name),
                                  ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        };
        write("portable",
              "This file makes this copy of Vaultik portable: its settings are kept in the Settings folder next to "
              "it, and nothing is written to the computer it runs on.\r\n");
        const std::string where = loc.relative.empty() ? std::string("the top of this drive") : loc.relative;
        write("How to restore.txt",
              "Restoring files from this drive on any Windows PC\r\n\r\n"
              "1. Start vaultik-gui.exe in this folder (nothing needs to be installed).\r\n"
              "2. Choose \"Restore from another backup...\" and select the folder " + where + " on this drive.\r\n"
              "3. Enter the backup's password (or use its recovery key), pick a version and restore.\r\n\r\n"
              "Vaultik keeps this folder up to date after each backup to this drive.\r\n");
        write("version", VAULTIK_VERSION);
        if (!g_json) out("Updated the Vaultik copy on the backup drive (" + target + ").\n");
    } catch (const std::exception& e) {
        warning(std::string("could not update the Vaultik copy on the backup drive: ") + e.what());
    }
#else
    (void)job;
#endif
}

// "uninstall-cleanup": run by the installer when Vaultik is removed; deletes every Vaultik task
// from Task Scheduler (all accounts' jobs; it runs with administrator rights). Jobs, history and
// backups stay.
int cmd_uninstall_cleanup() {
#ifdef _WIN32
    std::string list;
    os::run_command({"schtasks.exe", "/Query", "/FO", "CSV", "/NH"}, &list);
    std::set<std::string> tasks;
    std::size_t pos = 0;
    while ((pos = list.find("\"\\Vaultik\\", pos)) != std::string::npos) {
        const std::size_t end = list.find('"', pos + 1);
        if (end == std::string::npos) break;
        tasks.insert(list.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    for (const std::string& t : tasks) {
        std::string ignored;
        if (os::run_command({"schtasks.exe", "/Delete", "/TN", t, "/F"}, &ignored) == 0) out("removed " + t + "\n");
    }
#endif
    return kExitOk;
}

// The approved step behind saving or removing a job with administrator rights (see
// admin_jobs.hpp): "admin-job install|remove <id> --user <sid> [--from <config dir>]". Started by
// the frontend or `schedule` through a Windows approval prompt, without a visible console, so a
// failure is also written where the caller can read it.
int cmd_admin_job(const Args& a) {
    // gui-on|gui-off: the task behind "Start Vaultik as administrator" (no job id).
    const bool gui = !a.positional.empty() && (a.positional[0] == "gui-on" || a.positional[0] == "gui-off");
    if (a.positional.size() != (gui ? 1u : 2u) || !a.value("--user"))
        throw Error("usage: vaultik admin-job install|remove <job id> --user <account SID> [--from <folder>]\n"
                    "       vaultik admin-job gui-on|gui-off --user <account SID>");
    const std::string& verb = a.positional[0];
    const std::string id = gui ? std::string() : a.positional[1];
    const std::string sid = *a.value("--user");
    std::string error_file;
    try {
        error_file = jobs::admin_error_file(sid);
        // Only in a store that is already protected: elsewhere the file could be a planted link.
        std::string ignored;
        if (!os::admin_only(os::parent_of(error_file), false, &ignored)) error_file.clear();
        if (!error_file.empty()) os::remove_file(error_file);
    } catch (const Error&) {
        error_file.clear();
    }
    try {
        if (verb == "install") {
            const auto from = a.value("--from");
            if (!from) throw Error("admin-job install needs --from <the account's Vaultik settings folder>");
            jobs::install_admin_job(jobs::JobStore(*from), id, sid);
        } else if (verb == "remove") {
            jobs::remove_admin_job(id, sid);
        } else if (verb == "gui-on") {
            jobs::install_elevated_gui(sid);
        } else if (verb == "gui-off") {
            jobs::remove_elevated_gui(sid);
        } else {
            throw Error("admin-job takes install, remove, gui-on or gui-off");
        }
    } catch (const std::exception& e) {
        if (error_file.empty()) {
            try {
                // The store may have been created (and so protected) during the failed step.
                std::string ignored;
                const std::string f = jobs::admin_error_file(sid);
                if (os::admin_only(os::parent_of(f), false, &ignored)) error_file = f;
            } catch (const Error&) {
            }
        }
        if (!error_file.empty()) {
            try {
                const std::string text = e.what();
                os::write_file_atomic(error_file, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
            } catch (const Error&) {
            }
        }
        throw;
    }
    return kExitOk;
}

// Reads a job's stored data back (all of it for a full check, else the job's weekly share) and
// checks every version's structure, under a shared lock so backups of other jobs can go on.
// Fills `record` (kind, times, status, packs read as files, bytes read, problems as errors).
// Returns kExitOk, or kExitAuth when damage was found; throws on cancel or when the check could
// not run, after filling the record.
int health_check(Repository& repo, const jobs::Job& job, jobs::RunKind kind, jobs::RunRecord& record) {
    record.kind = kind;
    record.started = os::now();
    const bool full = kind == jobs::RunKind::FullCheck;
    std::vector<std::string> problems;
    VerifyResult r;
    try {
        RepoLock lock(repo, RepoLock::Mode::Shared, "check (job " + job.id + ")");
        repo.reload_index();  // a prune may have finished before the lock was granted
        VerifyOptions o;
        o.read_data = true;
        o.read_share = full ? 1.0 : job.health.sample_percent / 100.0;
        o.sample_seed = record.started;  // a different share each time
        o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
        o.on_progress = [](const std::string& msg) {
            if (g_json) emit({{"event", "check_progress"}, {"current", msg}});
            else status("Checking " + msg);
        };
        o.on_error = [&problems](const std::string& msg) {
            if (problems.size() < 3) problems.push_back(msg);
            warning(msg);
        };
        if (!g_json)
            out(std::string(full ? "Full health check: reading all stored data back...\n"
                                 : "Health check: reading " + std::to_string(job.health.sample_percent) +
                                       "% of the stored data back...\n"));
        r = run_verify(repo, o);
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the check could not run: ") + e.what();
        throw;
    }
    record.finished = os::now();
    record.files = r.packs_read;
    record.bytes = r.bytes_read;
    record.errors = r.errors;
    record.status = r.errors > 0 ? jobs::RunStatus::Failed : jobs::RunStatus::Success;
    if (r.errors > 0) {
        record.message = std::to_string(r.errors) + " problem(s) found: ";
        for (std::size_t i = 0; i < problems.size(); ++i) record.message += (i ? "; " : "") + problems[i];
    } else {
        record.message = "read " + format_bytes(r.bytes_read) + " in " + std::to_string(r.packs_read) + " of " +
                         std::to_string(r.packs) + " pack(s), checked " + std::to_string(r.snapshots) +
                         " version(s); no problems";
    }
    if (g_json) {
        emit({{"event", "check"},
              {"kind", jobs::to_string(kind)},
              {"status", jobs::to_string(record.status)},
              {"packs", r.packs},
              {"packs_read", r.packs_read},
              {"bytes_read", r.bytes_read},
              {"snapshots", r.snapshots},
              {"errors", r.errors},
              {"message", record.message}});
    } else {
        clear_status();
        out(std::string(r.errors > 0 ? "Health check FAILED: " : "Health check: ") + record.message + "\n");
    }
    return r.errors > 0 ? kExitAuth : kExitOk;
}

// A test restore of the job's newest version (see ops/restore_test.hpp), recorded in `record`.
// The temporary folder is made in the job store's folder: the user's own, or with administrator
// rights the admin-only store, so a privileged restore never writes where the user could plant a
// link. Returns kExitOk, or kExitError when a file did not come back right.
int restore_check(const Args& a, const jobs::JobStore& store, Repository& repo, const jobs::Job& job,
                  jobs::RunRecord& record) {
    record.kind = jobs::RunKind::RestoreTest;
    record.started = os::now();
    std::string work = store.dir();
    if (os::process_elevated() && !a.has("--admin")) work = jobs::open_admin_store().dir();
    RestoreTestResult r;
    try {
        RepoLock lock(repo, RepoLock::Mode::Shared, "test restore (job " + job.id + ")");
        repo.reload_index();
        // The newest version of this job.
        const std::vector<SnapshotRef> all = load_all_snapshots(repo);
        const SnapshotRef* newest = nullptr;  // the job's own (tagged), else the newest of any
        const SnapshotRef* newest_tagged = nullptr;
        for (const SnapshotRef& s : all) {  // oldest first
            newest = &s;
            const auto& tags = s.snapshot.tags;
            if (std::find(tags.begin(), tags.end(), "job:" + job.id) != tags.end()) newest_tagged = &s;
        }
        if (newest_tagged != nullptr) newest = newest_tagged;
        if (newest == nullptr) throw Error("the backup has no versions yet");
        RestoreTestOptions o;
        o.work_dir = work;
        o.seed = record.started;
        if (auto v = a.value("--files")) o.max_files = static_cast<std::size_t>(std::max(1, std::stoi(*v)));
        if (auto v = a.value("--max-size")) o.max_bytes = parse_size(*v);
        o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
        o.on_progress = [](const std::string& msg) {
            if (g_json) emit({{"event", "test_progress"}, {"current", msg}});
            else status("Test restore: " + msg);
        };
        if (!g_json) out("Test restore: restoring a few files of the version of " +
                         os::format_time_local(newest->snapshot.time) + "...\n");
        r = test_restore(repo, newest->snapshot, o);
        record.snapshot = id_hex(newest->id);
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled";
        throw;
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the test restore could not run: ") + e.what();
        throw;
    }
    record.finished = os::now();
    record.files = r.files;
    record.bytes = r.bytes;
    record.errors = r.problems.size();
    if (!r.problems.empty()) {
        record.status = jobs::RunStatus::Failed;
        record.message = std::to_string(r.problems.size()) + " file(s) did not come back right: ";
        for (std::size_t i = 0; i < r.problems.size() && i < 3; ++i) record.message += (i ? "; " : "") + r.problems[i];
        for (const std::string& p : r.problems) warning(p);
    } else if (r.files == 0) {
        record.status = jobs::RunStatus::Success;
        record.message = "the newest version has no files to test";
    } else {
        record.status = jobs::RunStatus::Success;
        record.message = "restored " + std::to_string(r.files) + " file(s), " + format_bytes(r.bytes) +
                         ", all intact; " + std::to_string(r.compared) + " also compared with the file on this PC";
    }
    if (g_json) {
        emit({{"event", "restore_test"}, {"status", jobs::to_string(record.status)}, {"files", r.files},
              {"bytes", r.bytes}, {"compared", r.compared}, {"problems", r.problems}, {"message", record.message}});
    } else {
        clear_status();
        out(std::string(r.problems.empty() ? "Test restore: " : "Test restore FAILED: ") + record.message + "\n");
    }
    return r.problems.empty() ? kExitOk : kExitError;
}

// The job's store: with --admin (the scheduled task of a job with administrator rights) only the
// admin-only copy of the job is trusted, never the user's own settings.
jobs::JobStore job_store(const Args& a) {
    return a.has("--admin") ? jobs::open_admin_store() : jobs::JobStore();
}

std::unique_ptr<Repository> open_job_repo(const Args& a, const jobs::JobStore& store, const jobs::Job& job) {
    std::string pw = store.password(job.id);
    if (pw.empty()) pw = get_password(a, false);
    status("Opening repository...");
    std::unique_ptr<Repository> repo;
    try {
        const CloudCredentials cloud = store.cloud_credentials(job);
        repo = Repository::open(open_backend(job.repository, &cloud), pw);
    } catch (...) {
        secure_wipe(pw.data(), pw.size());
        clear_status();
        throw;
    }
    secure_wipe(pw.data(), pw.size());
    clear_status();
    return repo;
}

// Whether another run is backing up this job right now (it holds a live lock naming the job).
bool backup_running(Repository& repo, const jobs::Job& job) {
    const std::string mine = "backup (job " + job.id + ")";
    const std::uint64_t now = os::now();
    for (const LockEntry& e : list_locks(repo))
        if (e.info && e.info->command == mine && !is_stale(*e.info, now)) return true;
    return false;
}

// Where `mount` puts versions by default: <settings>\browse\<process id>\<name>, so folders left by
// a mount whose process was killed can be told apart and removed.
std::string browse_dir() {
    return os::join(jobs::config_dir(), "browse");
}

void remove_leftover_mounts() {
    const std::string base = browse_dir();
    os::DirReader reader(base);
    std::string name;
    os::FileInfo info;
    std::vector<std::string> stale;
    while (reader.next(name, info)) {
        if (info.type != os::EntryType::Dir) continue;
        const unsigned long pid = std::strtoul(name.c_str(), nullptr, 10);
        if (pid != 0 && pid != os::process_id() && !os::process_alive(static_cast<std::uint32_t>(pid)))
            stale.push_back(os::join(base, name));
    }
    for (const std::string& dir : stale) {
        os::DirReader inner(dir);
        while (inner.next(name, info))
            if (info.type == os::EntryType::Dir) SnapshotMount::remove_leftover(os::join(dir, name));
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
    }
}

// "mount <snapshot> [--target <folder>]": shows a version as a read-only folder (File Explorer
// can browse it) until Enter is pressed, or with --json until "cancel" or the caller goes away.
int cmd_mount(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik mount -r <repo> <snapshot> [--target <folder>]");
    if (!SnapshotMount::available()) throw Error(SnapshotMount::how_to_enable());
    auto repo = open_repo(a);
    // Shared, like a restore: a prune cannot delete data the folder still shows.
    RepoLock lock(*repo, RepoLock::Mode::Shared, "browse", lock_wait(a));
    repo->reload_index();
    const SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);

    remove_leftover_mounts();
    std::string folder;
    std::string own_dir;
    if (const auto target = a.value("--target")) {
        folder = *target;
    } else {
        std::string label = job_arg(a) ? job_arg(a)->name : std::string("Backup");
        label += " " + os::format_time_local(ref.snapshot.time);
        for (char& c : label)
            if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '.';
        own_dir = os::join(browse_dir(), std::to_string(os::process_id()));
        folder = os::join(own_dir, label);
    }

    {
        SnapshotMount mount(*repo, ref.snapshot, folder);
        if (g_json) {
            emit({{"event", "mounted"}, {"folder", mount.folder()}, {"snapshot", id_hex(ref.id)}});
        } else {
            out("Version " + short_id(ref.id) + " from " + os::format_time_local(ref.snapshot.time) + " is open at\n  " +
                mount.folder() + "\nIt is read-only. Press Enter to close it.\n");
            std::thread([] {
                std::string line;
                std::getline(std::cin, line);
                g_cancel = true;
            }).detach();
        }
        while (!g_cancel && !g_stdin_closed && !lock.lost()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }  // the folder goes here
    if (!own_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(own_dir.begin(), own_dir.end())), ec);
    }
    if (g_json) emit({{"event", "done"}, {"status", "success"}, {"message", "closed"}});
    else out("Closed.\n");
    return kExitOk;
}

// "2026-11-09 20:30" (or with seconds, or only a date) in local time, as seconds since 1970.
std::int64_t parse_local_time(const std::string& text) {
    int v[6] = {0, 0, 0, 0, 0, 0};
    const char* p = text.c_str();
    const char seps[] = {'-', '-', ' ', ':', ':'};
    int n = 0;
    for (; n < 6; ++n) {
        char* end = nullptr;
        const long x = std::strtol(p, &end, 10);
        if (end == p) break;
        v[n] = static_cast<int>(x);
        p = end;
        if (*p == '\0') {
            ++n;
            break;
        }
        if (n >= 5 || (*p != seps[n] && !(n == 2 && *p == 'T'))) throw Error("give the time as YYYY-MM-DD HH:MM");
        ++p;
    }
    if (n != 3 && n != 5 && n != 6) throw Error("give the time as YYYY-MM-DD HH:MM");
    std::tm tm{};
    tm.tm_year = v[0] - 1900;
    tm.tm_mon = v[1] - 1;
    tm.tm_mday = v[2];
    tm.tm_hour = v[3];
    tm.tm_min = v[4];
    tm.tm_sec = v[5];
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1) || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31)
        throw Error("not a valid time: " + text);
    return static_cast<std::int64_t>(t);
}

// "cloud-rollback --to <time> [--dry-run]": brings an S3 repository's files back as they were at
// that time, from the bucket's old versions (after ransomware deleted or overwrote them).
int cmd_cloud_rollback(const Args& a) {
    const auto to = a.value("--to");
    if (!to) throw Error("usage: vaultik cloud-rollback -r s3:... --to \"YYYY-MM-DD HH:MM\" [--dry-run]");
    const std::int64_t when = parse_local_time(*to);
    auto backend = backend_for(a);
    auto* s3 = dynamic_cast<S3Backend*>(backend.get());
    if (s3 == nullptr) throw Error("cloud-rollback works on S3-compatible storage with versioning (Object Lock)");
    const bool dry = a.has("--dry-run");
    status("Reading the bucket's old versions...");
    const auto r = s3->roll_back(when, dry, [dry](const std::string& name) {
        if (g_json) emit({{"event", "file"}, {"name", name}});
        else out(std::string(dry ? "would restore " : "restoring ") + name + "\n");
    });
    clear_status();
    if (g_json) {
        emit({{"event", "done"},
              {"status", r.failed.empty() ? "success" : "partial"},
              {"checked", r.files_checked},
              {"restored", r.restored},
              {"unchanged", r.unchanged},
              {"newer", r.newer},
              {"failed", r.failed}});
    } else {
        for (const std::string& f : r.failed) err("error: " + f + "\n");
        out(std::to_string(r.restored) + " file(s) " + (dry ? "would be" : "were") + " brought back, " +
            std::to_string(r.unchanged) + " unchanged, " + std::to_string(r.newer) + " added later (left alone)" +
            (r.failed.empty() ? std::string() : ", " + std::to_string(r.failed.size()) + " failed") + ".\n");
        if (!dry && r.restored > 0) out("Run \"vaultik verify\" next, then restore from the versions you need.\n");
    }
    return r.failed.empty() ? kExitOk : kExitPartial;
}

// While a job runs: switches to the working-hours speed limits when they start, and back when they
// end (checked every 30 seconds). Does nothing for a job without working hours.
class SpeedSchedule {
public:
    SpeedSchedule(const ResourcePolicy& normal, const jobs::WorkHours& hours) {
        if (!hours.enabled) return;
        apply(normal, hours);
        thread_ = std::thread([this, normal, hours] {
            std::unique_lock<std::mutex> lock(mu_);
            while (!cv_.wait_for(lock, std::chrono::seconds(30), [this] { return stop_; })) apply(normal, hours);
        });
    }
    ~SpeedSchedule() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    SpeedSchedule(const SpeedSchedule&) = delete;
    SpeedSchedule& operator=(const SpeedSchedule&) = delete;

private:
    void apply(const ResourcePolicy& normal, const jobs::WorkHours& hours) {
        const auto [read, write] = jobs::rate_limits_now(normal, hours);
        if (read == read_ && write == write_) return;
        read_ = read;
        write_ = write;
        set_rate_limits(read, write);
        if (g_json) emit({{"event", "speed"}, {"read_limit", read}, {"write_limit", write}});
    }

    std::uint64_t read_ = ~0ull, write_ = ~0ull;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

std::string drive_problem(const os::DriveHealth& d);  // below, with the drives command

// After a job's backup: copies its new versions to the job's second destination (creating the
// repository there on first use, with the job's password), then removes copies beyond the job's
// retention there. Fills `record` (kind Copy) whatever happens; returns kExitOk or kExitError.
int second_copy(const Args& a, const jobs::JobStore& store, const jobs::Job& job, Repository& source,
                jobs::RunRecord& record, bool keep_old = false) {
    record.kind = jobs::RunKind::Copy;
    record.started = os::now();
    const std::string& where = job.second_copy.repository;
    try {
        if (!jobs::copy_drive_connected(job)) throw Error("the drive of the second copy is not connected (" + where + ")");
        std::string pw = store.password(job.id);
        if (pw.empty()) pw = get_password(a, false);
        std::unique_ptr<Repository> target;
        try {
            const CloudCredentials cloud = store.copy_credentials(job);
            auto backend = open_backend(where, &cloud, true);
            if (!backend->exists("config")) {
                if (!g_json) out("Creating the second copy at " + where + "\n");
                Repository::init(*backend, pw);
            }
            target = Repository::open(std::move(backend), pw);
            // Remember the drive once the copy is there, so a different drive that gets this
            // letter later is never taken for it.
            if (!is_cloud_location(where) && job.second_copy.volume.empty()) {
                try {
                    jobs::Job updated = job;
                    jobs::record_copy_volume(updated);
                    if (!updated.second_copy.volume.empty()) store.save(updated);
                } catch (const Error& e) {
                    warning(std::string("could not record the second copy's drive: ") + e.what());
                }
            }
        } catch (...) {
            secure_wipe(pw.data(), pw.size());
            throw;
        }
        secure_wipe(pw.data(), pw.size());
        if (!g_json) out("Second copy: copying new versions to " + where + "\n");

        CopyResult r;
        {
            RepoLock from_lock(source, RepoLock::Mode::Shared, "copy (job " + job.id + ")", lock_wait(a, std::chrono::minutes(10)));
            RepoLock to_lock(*target, RepoLock::Mode::Shared, "copy (job " + job.id + ")", lock_wait(a, std::chrono::minutes(10)));
            source.reload_index();
            target->reload_index();
            CopyOptions co;
            co.tags = {"job:" + job.id};
            co.map_file = os::join(os::join(store.dir(), "copies"), job.id + ".map");
            co.should_cancel = [&] { return stop_requested() || from_lock.lost() || to_lock.lost(); };
            co.on_progress = [](const std::string& msg) {
                if (g_json) emit({{"event", "copy_progress"}, {"current", msg}});
                else status("Copying " + msg);
            };
            r = copy_snapshots(source, *target, co);
        }
        if (!g_json) clear_status();
        // The copy is what a user falls back on: never thin it out after a warning on the source.
        std::string note;
        if (keep_old) note = "old versions in the copy were not removed because of the warning";
        else if (!job.retention.empty()) note = clean_up(*target, job);
        record.finished = os::now();
        record.status = jobs::RunStatus::Success;
        record.files = r.copied;
        record.bytes = r.bytes;
        record.message = r.copied == 0 ? "the second copy was up to date"
                                       : "copied " + std::to_string(r.copied) + " version(s), " + format_bytes(r.bytes) + " new";
        if (!note.empty()) record.message += "; " + note;
        if (g_json) {
            emit({{"event", "copy"}, {"status", "success"}, {"copied", r.copied}, {"bytes", r.bytes},
                  {"message", record.message}});
        } else {
            out("Second copy: " + record.message + "\n");
        }
        return kExitOk;
    } catch (const Cancelled&) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Cancelled;
        record.message = "cancelled; the versions copied so far stay";
    } catch (const std::exception& e) {
        if (!g_json) clear_status();
        record.finished = os::now();
        record.status = jobs::RunStatus::Failed;
        record.message = std::string("the second copy failed: ") + e.what();
    }
    if (g_json) emit({{"event", "copy"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    else warning(record.message);
    return kExitError;
}

// "copy --job <job>": makes the job's second copy now (also run by run-job after each backup).
int cmd_copy(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik copy <job id or name>");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    if (job->second_copy.repository.empty()) throw Error("the job \"" + job->name + "\" has no second copy set up");
    apply_resource_policy(resource_policy(a, job->resources));
    const auto repo = open_job_repo(a, store, *job);
    jobs::RunRecord record;
    const int code = second_copy(a, store, *job, *repo, record);
    store.append_history(job->id, record);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// "check-job <job> [--full]": a health check of the job's backup now, recorded in its history.
int cmd_check_job(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik check-job <job id or name> [--full]");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    apply_resource_policy(resource_policy(a, job->resources));
    jobs::RunRecord record;
    record.kind = a.has("--full") ? jobs::RunKind::FullCheck : jobs::RunKind::Check;
    record.started = os::now();
    int code = kExitError;
    try {
        const auto repo = open_job_repo(a, store, *job);
        code = health_check(*repo, *job, record.kind, record);
    } catch (const LockedError&) {
        throw;  // nothing was checked; not a result for the history
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the check started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = std::string("the check could not run: ") + e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        if (record.status != jobs::RunStatus::Cancelled) throw;
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", "health check cancelled"}});
        else err("health check cancelled\n");
        return kExitCancelled;
    }
    store.append_history(job->id, record);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// "test-restore <job> [--files n] [--max-size size]": a test restore of the job's newest version
// now, recorded in its history.
int cmd_test_restore(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik test-restore <job id or name> [--files <n>] [--max-size <size>]");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");
    apply_resource_policy(resource_policy(a, job->resources));
    jobs::RunRecord record;
    record.kind = jobs::RunKind::RestoreTest;
    record.started = os::now();
    int code = kExitError;
    try {
        const auto repo = open_job_repo(a, store, *job);
        code = restore_check(a, store, *repo, *job, record);
    } catch (const LockedError&) {
        throw;  // nothing was tested; not a result for the history
    } catch (const std::exception& e) {
        if (record.finished == 0) {  // failed before the test started
            record.finished = os::now();
            record.status = jobs::RunStatus::Failed;
            record.message = std::string("the test restore could not run: ") + e.what();
        }
        try {
            store.append_history(job->id, record);
        } catch (const Error&) {
        }
        if (record.status != jobs::RunStatus::Cancelled) throw;
        if (g_json) emit({{"event", "done"}, {"status", "cancelled"}, {"message", "test restore cancelled"}});
        else err("test restore cancelled\n");
        return kExitCancelled;
    }
    store.append_history(job->id, record);
    if (g_json) emit({{"event", "done"}, {"status", jobs::to_string(record.status)}, {"message", record.message}});
    return code;
}

// Runs a job's "before" or "after" command line (see Job::before_command). Returns its exit code;
// its output goes to the log (the start of it), each line as a warning when it failed.
int run_job_command(const jobs::Job& job, const std::string& which, const std::string& command,
                    const std::string& result) {
    os::set_env("VAULTIK_JOB", job.id);
    os::set_env("VAULTIK_JOB_NAME", job.name);
    os::set_env("VAULTIK_RESULT", result);
    if (!g_json) out("Running the command " + which + " the backup: " + command + "\n");
    std::string output;
    int code = 0;
    try {
        code = os::run_shell(command, &output);
    } catch (const std::exception& e) {
        warning("the command " + which + " the backup could not start: " + e.what());
        return -1;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    if (code != 0) {
        warning("the command " + which + " the backup failed (exit code " + std::to_string(code) + ")" +
                (output.empty() ? std::string() : ": " + output.substr(0, 500)));
    } else if (!output.empty() && !g_json) {
        out(output.substr(0, 2000) + "\n");
    }
    return code;
}

int cmd_run_job(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik run-job <job id or name>");
    const jobs::JobStore store = job_store(a);
    const auto job = store.find(a.positional[0]);
    if (!job) throw Error("no saved job called " + a.positional[0] + " (see vaultik jobs)");

    // The job's own resource settings, unless overridden on the command line.
    const bool on_connect = a.has("--on-connect");
    if (on_connect && !jobs::connect_backup_due(store, *job)) return kExitOk;

    const ResourcePolicy policy = resource_policy(a, job->resources);
    apply_resource_policy(policy);
    if (!policy.is_default() && !g_json) err("Resources: " + describe(policy) + "\n");
    const SpeedSchedule speed(policy, job->work_hours);

    jobs::RunRecord record;
    record.on_connect = on_connect;
    record.started = os::now();
    int code = kExitError;
    std::unique_ptr<Repository> repo;
    // The "after" command runs once the "before" one did (or there is none), whatever happens.
    bool after_due = false;
    const auto run_after = [&](const std::string& result) {
        if (!after_due || job->after_command.empty()) return;
        after_due = false;
        run_job_command(*job, "after", job->after_command, result);
    };
    try {
        if (!jobs::drive_connected(*job))
            throw Error("the backup drive is not connected (" + job->repository + ")");
        repo = open_job_repo(a, store, *job);
        apply_parallelism(*repo, a, job->repository, policy);
        if (on_connect && backup_running(*repo, *job)) {
            if (!g_json) out("Job \"" + job->name + "\" is already backing up.\n");
            return kExitOk;
        }
        if (!g_json) out("Job \"" + job->name + "\": backing up to " + job->repository + "\n");
        if (!job->before_command.empty()) {
            const int before = run_job_command(*job, "before", job->before_command, "");
            if (before != 0)
                throw Error("the command before the backup failed (exit code " + std::to_string(before) +
                            "), so nothing was backed up");
        }
        after_due = true;

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
                                     vss_arg(a, job->vss ? kDefaultVss : VssMode::Off),
                                     !(job->vss_skip_sql || a.has("--vss-skip-sql")));
        }
        refresh_drive_copy(*job);
        // The destination drive's own health, as the system reports it: a warning here is the
        // time to copy the backups elsewhere, before the drive fails.
        if (!is_cloud_location(job->repository)) {
            const std::vector<os::DriveHealth> drives = os::drive_health(job->repository);
            jobs::record_drives(drives);
            for (const os::DriveHealth& d : drives) {
                const std::string problem = drive_problem(d);
                if (problem.empty()) continue;
                record.drive = problem;
                warning("the backup drive reports a problem: " + problem);
            }
        }
        if (!record.alert.empty() || !record.hold.empty()) {
            // Never thin out the versions from before a possible attack or a missing source.
            if (!job->retention.empty())
                record.message += std::string(record.message.empty() ? "" : "; ") +
                                  "old versions were not removed because of the warning";
        } else if (!job->retention.empty()) {
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
        run_after(jobs::to_string(record.status));
        throw;
    }
    store.append_history(job->id, record);
    run_after(jobs::to_string(record.status));

    // The second copy, after a backup that saved a version. Its result is its own history entry.
    if (!job->second_copy.repository.empty() && !g_cancel.load() && record.status != jobs::RunStatus::Failed) {
        jobs::RunRecord copy;
        second_copy(a, store, *job, *repo, copy, !record.alert.empty() || !record.hold.empty());
        try {
            store.append_history(job->id, copy);
        } catch (const Error& e) {
            warning(std::string("could not record the second copy: ") + e.what());
        }
    }

    // Then the health check, when one is due. It never changes the backup's result: a problem
    // it finds is its own entry in the history (and a notification from the tray).
    if (a.has("--no-check") || g_cancel.load()) return code;
    const auto due = jobs::due_check(job->health, jobs::combined_history(store, job->id, 500), os::now());
    if (due) {
        jobs::RunRecord check;
        try {
            health_check(*repo, *job, *due, check);
        } catch (const LockedError&) {
            // Another run is using the repository: try again after the next backup.
            if (!g_json) clear_status();
            warning("the health check was skipped this time: another run is using the repository");
            return code;
        } catch (const std::exception&) {
            // Cancelled or could not run: the record says which.
        }
        try {
            store.append_history(job->id, check);
        } catch (const Error& e) {
            warning(std::string("could not record the health check: ") + e.what());
        }
    }

    // And the monthly test restore, when it is on and due. Like the check, its own entry.
    if (g_cancel.load() || !jobs::restore_test_due(job->health, jobs::combined_history(store, job->id, 500), os::now()))
        return code;
    jobs::RunRecord test;
    try {
        restore_check(a, store, *repo, *job, test);
    } catch (const LockedError&) {
        if (!g_json) clear_status();
        warning("the test restore was skipped this time: another run is using the repository");
        return code;
    } catch (const std::exception&) {
        // Cancelled or could not run: the record says which.
    }
    try {
        store.append_history(job->id, test);
    } catch (const Error& e) {
        warning(std::string("could not record the test restore: ") + e.what());
    }
    return code;
}

int cmd_forget(const Args& a) {
    ForgetOptions fo;
    fo.policy = retention_arg(a);
    fo.tags = a.all("--tag");
    fo.dry_run = a.has("--dry-run");
    fo.force = a.has("--force");
    if (a.positional.empty() && fo.policy.no_rules())
        throw Error("give snapshot IDs or at least one --keep-... rule; forget never removes every snapshot");
    if (!a.positional.empty() && !fo.policy.no_rules())
        throw Error("give either snapshot IDs or --keep-... rules, not both");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "forget", lock_wait(a));
    for (const std::string& ref : a.positional) fo.ids.push_back(resolve_snapshot(*repo, ref).id);
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
            (fo.dry_run ? "would be removed" : "removed") +
            (fo.ids.empty() ? " (keeping " + describe(fo.policy) + ")" : std::string()) + "\n");
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

// "repair [--quick] [--dry-run]": see ops/repair.hpp. Opens the repository without its index, so
// a damaged index file cannot stop it.
int cmd_repair(const Args& a) {
    RepairOptions o;
    o.quick = a.has("--quick");
    o.dry_run = a.has("--dry-run");
    auto repo = open_repo(a, false);
    RepoLock lock(*repo, RepoLock::Mode::Exclusive, "repair", lock_wait(a));
    o.should_cancel = [&lock] { return stop_requested() || lock.lost(); };
    o.on_progress = [](const std::string& msg) {
        if (g_json) emit({{"event", "repair_progress"}, {"current", msg}});
        else status("Repair: " + msg);
    };
    o.on_warning = [](const std::string& msg) { warning(msg); };
    const auto start = std::chrono::steady_clock::now();
    if (!g_json) out(o.quick ? "Repair: rebuilding the index from the packs' headers...\n"
                             : "Repair: reading and checking all stored data...\n");
    const RepairResult r = run_repair(*repo, o);
    if (!g_json) clear_status();
    std::string summary = std::to_string(r.blobs) + " intact chunk(s) in " + std::to_string(r.packs) + " pack(s)";
    if (r.packs_unreadable) summary += ", " + std::to_string(r.packs_unreadable) + " pack(s) unreadable";
    if (r.blobs_damaged) summary += ", " + std::to_string(r.blobs_damaged) + " damaged chunk(s)";
    summary += "; " + std::to_string(r.snapshots) + " version(s)";
    if (r.snapshots_repaired)
        summary += ", " + std::to_string(r.snapshots_repaired) + " saved again without " +
                   std::to_string(r.files_lost) + " lost file(s) and " + std::to_string(r.folders_lost) + " folder(s)";
    if (r.snapshots_removed) summary += ", " + std::to_string(r.snapshots_removed) + " removed";
    if (!r.changed()) summary += "; nothing was damaged";
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"dry_run", o.dry_run}, {"packs", r.packs},
              {"packs_unreadable", r.packs_unreadable}, {"blobs", r.blobs}, {"blobs_damaged", r.blobs_damaged},
              {"snapshots", r.snapshots}, {"snapshots_repaired", r.snapshots_repaired},
              {"snapshots_removed", r.snapshots_removed}, {"files_lost", r.files_lost},
              {"folders_lost", r.folders_lost}, {"message", summary}});
        return kExitOk;
    }
    out((o.dry_run ? "Repair (dry run, nothing changed): " : "Repair done in " + format_duration(seconds_since(start)) + ": ") +
        summary + "\n");
    if (r.changed() && !o.dry_run)
        out("Back up again soon: files still on this computer are stored again by the next backup.\n");
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

// What unlocks the repository for `key`: --recovery-key, or the password from the usual places.
Credential key_credential(const Args& a) {
    // Never taken from the command line, where other programs and the shell history can see it.
    if (a.has("--recovery-key")) {
        std::string key = os::get_env("VAULTIK_RECOVERY_KEY");
        if (key.empty()) key = read_password("Recovery key: ");
        return {std::move(key), true};
    }
    return {get_password(a, false), false};
}

bool same_location(const std::string& a, const std::string& b) {
    if (is_cloud_location(a) || is_cloud_location(b)) {
        std::string x = a;
        std::string y = b;
        while (!x.empty() && x.back() == '/') x.pop_back();
        while (!y.empty() && y.back() == '/') y.pop_back();
        return x == y;
    }
    std::string x = os::absolute_path(a);
    std::string y = os::absolute_path(b);
#ifdef _WIN32
    for (auto* s : {&x, &y})
        for (char& c : *s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    while (x.size() > 1 && (x.back() == '\\' || x.back() == '/')) x.pop_back();
    while (y.size() > 1 && (y.back() == '\\' || y.back() == '/')) y.pop_back();
    return x == y;
}

int cmd_key(const Args& a) {
    const std::string sub = a.positional.empty() ? std::string() : a.positional[0];
    if (sub != "passwd" && sub != "recovery") throw Error("usage: vaultik key passwd|recovery -r <repository>");
    if (a.positional.size() > 1)
        throw Error("unexpected \"" + a.positional[1] +
                    "\": --recovery-key asks for the key (or reads VAULTIK_RECOVERY_KEY); keep it off the command line");
    const std::string location = repo_path(a);
    const auto backend_ptr = backend_for(a);
    Backend& backend = *backend_ptr;
    Credential credential = key_credential(a);

    if (sub == "recovery") {
        status("Making a recovery key...");
        const std::string key = create_recovery_key(backend, credential);
        secure_wipe(credential.secret.data(), credential.secret.size());
        out("Recovery key: " + key + "\n");
        out("Write it down and keep it apart from this computer. Any older recovery key no longer works.\n");
        return kExitOk;
    }

    // Check the current password or recovery key before asking for the new password.
    { const MasterKey check = unlock_master_key(backend, credential); }
    std::string pw = os::get_env("VAULTIK_NEW_PASSWORD");
    if (pw.empty()) {
        pw = read_password("New password: ");
        std::string again = read_password("Repeat new password: ");
        const bool same = again == pw;
        secure_wipe(again.data(), again.size());
        if (!same) throw Error("passwords do not match");
    }
    status("Setting the new password...");
    change_password(backend, credential, pw);
    secure_wipe(credential.secret.data(), credential.secret.size());
    out("The password of " + backend.location() + " is changed.\n");

    const jobs::JobStore store;
    for (const jobs::Job& job : store.list()) {
        if (!same_location(job.repository, location)) continue;
        try {
            store.set_password(job.id, pw);
            out("Saved job \"" + job.name + "\" uses the new password.\n");
            // Its scheduled runs read the administrator copy, which needs approval to update.
            if (job.run_as_admin && job.schedule.kind != jobs::Schedule::Kind::Manual) {
                jobs::apply_schedule(job, os::parent_of(os::executable_path()));
            }
        } catch (const Error& e) {
            warning("cannot update the password of job \"" + job.name + "\": " + e.what());
        }
    }
    secure_wipe(pw.data(), pw.size());
    return kExitOk;
}

// pin / unpin <snapshot>: a pinned snapshot is never removed by retention or a size limit.
int cmd_pin(const Args& a, bool pin) {
    if (a.positional.size() != 1) throw Error(std::string("usage: vaultik ") + (pin ? "pin" : "unpin") + " -r <repo> <snapshot>");
    auto repo = open_repo(a);
    // Shared: a forget (exclusive) cannot remove the snapshot while it is rewritten.
    RepoLock lock(*repo, RepoLock::Mode::Shared, pin ? "pin" : "unpin", lock_wait(a));
    SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    ref.snapshot.pinned = pin;
    repo->update_snapshot(ref.id, ref.snapshot);
    out("Snapshot " + short_id(ref.id) + (pin ? " is pinned: it stays until it is unpinned.\n" : " is no longer pinned.\n"));
    return kExitOk;
}

// note <snapshot> [text...]: describes a snapshot; no text removes the note.
int cmd_note(const Args& a) {
    if (a.positional.empty()) throw Error("usage: vaultik note -r <repo> <snapshot> [text]");
    std::string text;
    for (std::size_t i = 1; i < a.positional.size(); ++i) text += (i > 1 ? " " : "") + a.positional[i];
    for (char& c : text)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (text.size() > 500) throw Error("a note can be at most 500 characters");
    auto repo = open_repo(a);
    RepoLock lock(*repo, RepoLock::Mode::Shared, "note", lock_wait(a));
    SnapshotRef ref = resolve_snapshot(*repo, a.positional[0]);
    ref.snapshot.note = text;
    repo->update_snapshot(ref.id, ref.snapshot);
    out(text.empty() ? "Note removed from snapshot " + short_id(ref.id) + ".\n"
                     : "Snapshot " + short_id(ref.id) + ": \"" + text + "\"\n");
    return kExitOk;
}

// history <path>: the versions of one file or folder (only those where it changed, unless --all).
// How the drive got worse in the last 90 days, from its stored readings ("2 more reallocated
// sectors since 2026-10-02 (1 -> 3)"); empty when it did not.
std::vector<std::string> drive_changes(const os::DriveHealth& d) {
    std::vector<std::string> out;
    const jobs::DriveReading now = jobs::reading_of(d, os::now());
    for (const jobs::DriveChange& c : jobs::drive_trend(jobs::drive_history(now.key), now))
        out.push_back(jobs::describe(c));
    return out;
}

// A drive's problem in words ("WD My Passport 2627: warning, Predictive failure"); empty when it
// reports none.
std::string drive_problem(const os::DriveHealth& d) {
    const bool bad = d.status == os::DriveHealth::Status::Warning || d.status == os::DriveHealth::Status::Unhealthy;
    const bool uncorrected = d.read_errors_uncorrected.value_or(0) > 0;
    const std::vector<std::string> worse = drive_changes(d);
    if (!bad && d.condition.empty() && !uncorrected && !d.worn_out() && worse.empty()) return {};
    std::string s = (d.model.empty() ? "disk " + std::to_string(d.disk) : d.model) + ": " + os::to_string(d.status);
    if (!d.condition.empty()) s += ", " + d.condition;
    if (uncorrected) s += ", " + std::to_string(*d.read_errors_uncorrected) + " unrecoverable read errors";
    if (d.worn_out()) s += ", " + std::to_string(*d.health_percent()) + "% of its rated life left";
    for (const std::string& w : worse) s += ", " + w;
    return s;
}

// "drives [--smart]": the health of this computer's drives, as Windows reports it, and with
// --smart each ATA drive's attribute table.
int cmd_drives(const Args& a) {
    const bool smart = a.has("--smart");
    const bool history = a.has("--history");
    const std::vector<os::DriveHealth> drives = os::all_drives();
    jobs::record_drives(drives);
    // Which drive letters are on each disk.
    std::map<int, std::string> letters;
#ifdef _WIN32
    for (char c = 'A'; c <= 'Z'; ++c) {
        const std::string root = std::string(1, c) + ":\\";
        if (!os::exists(root)) continue;
        for (const int n : os::disk_numbers(root)) letters[n] += (letters[n].empty() ? "" : " ") + root.substr(0, 2);
    }
#endif
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const os::DriveHealth& d : drives) {
            nlohmann::json j = {{"disk", d.disk}, {"model", d.model}, {"bus", d.bus}, {"media", d.media},
                                {"size", d.size}, {"status", os::to_string(d.status)}, {"condition", d.condition},
                                {"volumes", letters[d.disk]}};
            if (d.temperature) j["temperature"] = *d.temperature;
            if (d.wear) j["wear"] = *d.wear;
            if (const auto p = d.health_percent()) j["health_percent"] = *p;
            if (d.power_on_hours) j["power_on_hours"] = *d.power_on_hours;
            if (d.read_errors_uncorrected) j["read_errors_uncorrected"] = *d.read_errors_uncorrected;
            if (!d.smart_issues.empty()) j["issues"] = d.smart_issues;
            if (!d.serial.empty()) j["serial"] = d.serial;
            if (const auto worse = drive_changes(d); !worse.empty()) j["worse"] = worse;
            if (history) {
                nlohmann::json readings = nlohmann::json::array();
                for (const jobs::DriveReading& r : jobs::drive_history(jobs::drive_key(d))) {
                    nlohmann::json e = {{"time", os::format_time_local(r.time)}};
                    if (r.health) e["health_percent"] = *r.health;
                    if (r.reallocated) e["reallocated"] = *r.reallocated;
                    if (r.pending) e["pending"] = *r.pending;
                    if (r.uncorrectable) e["uncorrectable"] = *r.uncorrectable;
                    if (r.read_errors) e["read_errors"] = *r.read_errors;
                    readings.push_back(e);
                }
                j["history"] = readings;
            }
            if (smart && !d.smart.empty()) {
                nlohmann::json attrs = nlohmann::json::array();
                for (const os::SmartAttribute& s : d.smart)
                    attrs.push_back({{"id", s.id}, {"name", os::smart_attribute_name(s.id)}, {"value", s.value},
                                     {"worst", s.worst}, {"threshold", s.threshold}, {"raw", s.raw},
                                     {"prefail", s.prefail}});
                j["smart"] = attrs;
            }
            list.push_back(j);
        }
        emit({{"event", "drives"}, {"drives", list}});
        return kExitOk;
    }
    if (drives.empty()) {
        out("Windows reported no drives (or this system cannot tell).\n");
        return kExitOk;
    }
    bool missing = false;
    for (const os::DriveHealth& d : drives) {
        std::string line = "Disk " + std::to_string(d.disk) + "  " + d.model + "  (" + d.bus +
                           (d.media.empty() ? "" : " " + d.media) + ", " + format_bytes(d.size) + ")";
        if (!letters[d.disk].empty()) line += "  " + letters[d.disk];
        line += "\n  " + std::string(os::to_string(d.status));
        if (const auto p = d.health_percent()) line += ", health " + std::to_string(*p) + "%";
        if (!d.condition.empty()) line += ", " + d.condition;
        if (d.temperature) line += ", " + std::to_string(*d.temperature) + " C";
        if (d.power_on_hours) line += ", " + std::to_string(*d.power_on_hours) + " hours on";
        if (d.read_errors_uncorrected.value_or(0) > 0 && d.smart.empty())  // (else among the issues)
            line += ", " + std::to_string(*d.read_errors_uncorrected) + " unrecoverable read errors";
        for (const std::string& issue : d.smart_issues)
            if (issue != d.condition) line += ", " + issue;
        missing = missing || !d.health_percent() || !d.power_on_hours;
        out(line + "\n");
        for (const std::string& w : drive_changes(d)) out("  Getting worse: " + w + "\n");
        if (history) {
            const std::vector<jobs::DriveReading> readings = jobs::drive_history(jobs::drive_key(d));
            if (readings.empty()) out("    (no stored readings)\n");
            for (const jobs::DriveReading& r : readings) {
                std::string row = "    " + os::format_time_local(r.time);
                if (r.health) row += "  health " + std::to_string(*r.health) + "%";
                if (r.reallocated) row += "  reallocated " + std::to_string(*r.reallocated);
                if (r.pending) row += "  pending " + std::to_string(*r.pending);
                if (r.uncorrectable) row += "  uncorrectable " + std::to_string(*r.uncorrectable);
                if (r.read_errors) row += "  read errors " + std::to_string(*r.read_errors);
                out(row + "\n");
            }
        }
        if (smart && !d.smart.empty()) {
            out("    ID  Attribute                       Value Worst Thresh  Raw\n");
            for (const os::SmartAttribute& s : d.smart) {
                const char* name = os::smart_attribute_name(s.id);
                char row[160];
                std::snprintf(row, sizeof row, "   %3d  %-30s  %4d  %4d  %4d   %llu%s\n", s.id, *name ? name : "?",
                              s.value, s.worst, s.threshold, static_cast<unsigned long long>(s.raw),
                              s.prefail ? "  (pre-fail)" : "");
                out(row);
            }
        } else if (smart && d.bus != "NVMe") {
            out(os::process_elevated() ? "    (the drive or its enclosure did not hand out its S.M.A.R.T. attributes)\n"
                                       : "    (S.M.A.R.T. attributes need administrator rights)\n");
        }
    }
    if (missing && !os::process_elevated())
        out("(Run as administrator for the health percentage of hard disks and SATA SSDs, power-on hours\n"
            " and error counts.)\n");
    return kExitOk;
}

// "estimate <path>...": how much a first backup of these paths reads, and roughly how long it takes.
int cmd_estimate(const Args& a) {
    std::vector<std::string> paths = a.positional;
    std::vector<std::string> excludes = a.all("--exclude");
    if (const auto job = job_arg(a)) {
        if (paths.empty()) paths = job->sources;
        excludes.insert(excludes.end(), job->excludes.begin(), job->excludes.end());
    }
    if (paths.empty()) throw Error("usage: vaultik estimate <path>... [--exclude <pattern>] (or --job <job>)");
    const SourceEstimate e = estimate_sources(paths, excludes, [] { return g_cancel.load(); },
                                              [](const SourceEstimate& so_far) {
                                                  if (!g_json)
                                                      status(std::to_string(so_far.files) + " files, " +
                                                             format_bytes(so_far.bytes) + "...");
                                              });
    clear_status();
    // Typical speeds: an external hard disk, and two internet uploads (Mbit/s / 8 = MB/s).
    const auto secs = [&](double mb_per_s) { return static_cast<double>(e.bytes) / (mb_per_s * 1e6); };
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"files", e.files}, {"dirs", e.dirs}, {"bytes", e.bytes},
              {"unreadable", e.unreadable}, {"seconds_drive", secs(100)}, {"seconds_upload_20", secs(2.5)},
              {"seconds_upload_100", secs(12.5)}});
        return kExitOk;
    }
    out(std::to_string(e.files) + " files in " + std::to_string(e.dirs) + " folders, " + format_bytes(e.bytes) + "\n");
    if (e.unreadable > 0) out(std::to_string(e.unreadable) + " folder(s) could not be read\n");
    out("First backup, roughly: " + format_duration(secs(100)) + " to an external drive (100 MB/s), " +
        format_duration(secs(12.5)) + " uploading at 100 Mbit/s, " + format_duration(secs(2.5)) +
        " at 20 Mbit/s.\nIt stores less after compression and deduplication; later backups only read what "
        "changed.\n");
    return kExitOk;
}

// "diff <older> [<newer>]": what changed between two snapshots (one: since its parent).
int cmd_diff(const Args& a) {
    if (a.positional.empty() || a.positional.size() > 2)
        throw Error("usage: vaultik diff -r <repo> <older snapshot> [<newer snapshot>]");
    auto repo = open_repo(a);
    SnapshotRef older, newer;
    if (a.positional.size() == 2) {
        older = resolve_snapshot(*repo, a.positional[0]);
        newer = resolve_snapshot(*repo, a.positional[1]);
    } else {
        newer = resolve_snapshot(*repo, a.positional[0]);
        if (!newer.snapshot.parent) throw Error("snapshot " + short_id(newer.id) + " has no earlier one to compare with");
        older = resolve_snapshot(*repo, id_hex(*newer.snapshot.parent));
    }
    status("Comparing...");
    const DiffResult d = diff_snapshots(*repo, older.snapshot, newer.snapshot, 100000, [] { return g_cancel.load(); });
    clear_status();
    const auto mark = [](Change::Kind k) { return k == Change::Kind::Added ? "+" : k == Change::Kind::Removed ? "-" : "M"; };
    for (const Change& c : d.changes) {
        const bool dir = c.node_kind == NodeKind::Dir;
        if (g_json) {
            emit({{"event", "change"},
                  {"kind", c.kind == Change::Kind::Added ? "added" : c.kind == Change::Kind::Removed ? "removed" : "changed"},
                  {"path", c.path},
                  {"dir", dir},
                  {"old_size", c.old_size},
                  {"new_size", c.new_size}});
        } else {
            std::string line = std::string(mark(c.kind)) + " " + c.path + (dir ? "/" : "");
            if (c.kind == Change::Kind::Changed) line += "  (" + format_bytes(c.old_size) + " -> " + format_bytes(c.new_size) + ")";
            out(line + "\n");
        }
    }
    if (g_json) {
        emit({{"event", "done"}, {"status", "success"}, {"added", d.added}, {"removed", d.removed},
              {"changed", d.changed}, {"truncated", d.truncated}});
    } else {
        out(short_id(older.id) + " -> " + short_id(newer.id) + ": " + std::to_string(d.added) + " added, " +
            std::to_string(d.removed) + " removed, " + std::to_string(d.changed) + " changed" +
            (d.truncated ? " (list cut short)" : "") + "\n");
    }
    return kExitOk;
}

int cmd_history(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik history -r <repo> <path>   e.g. C/Users/me/report.docx");
    std::string path = a.positional[0];
    std::replace(path.begin(), path.end(), '\\', '/');
    auto repo = open_repo(a);
    const std::vector<SnapshotRecord> snaps = load_snapshot_records(*repo);
    const std::vector<ItemVersion> versions = item_history(*repo, snaps, path);
    const bool all = a.has("--all");
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const ItemVersion& v : versions) {
            if (!all && !v.changed) continue;
            const SnapshotRecord& s = snaps[v.snapshot];
            list.push_back({{"snapshot", id_hex(s.id)},
                            {"time", s.snapshot.time},
                            {"size", v.node.size},
                            {"modified", v.node.mtime},
                            {"changed", v.changed}});
        }
        emit({{"event", "history"}, {"path", path}, {"versions", list}});
        return kExitOk;
    }
    if (versions.empty()) {
        out("No snapshot has " + path + "\n");
        return kExitOk;
    }
    out(pad("Snapshot", 10) + pad("Backed up", 21) + pad("Modified", 21) + rpad("Size", 12) + "\n");
    std::size_t shown = 0;
    for (const ItemVersion& v : versions) {
        if (!all && !v.changed) continue;
        const SnapshotRecord& s = snaps[v.snapshot];
        out(pad(short_id(s.id), 10) + pad(os::format_time_local(s.snapshot.time), 21) +
            pad(os::format_time_local(v.node.mtime), 21) + rpad(format_bytes(v.node.size), 12) +
            (v.changed ? "" : "  (same)") + "\n");
        ++shown;
    }
    out(std::to_string(shown) + " version(s)" + (all ? "" : " where it changed") + ", in " +
        std::to_string(versions.size()) + " of " + std::to_string(snaps.size()) + " snapshot(s)\n");
    return kExitOk;
}

// find <pattern>: files and folders by name in every snapshot.
int cmd_find(const Args& a) {
    if (a.positional.size() != 1) throw Error("usage: vaultik find -r <repo> <name or pattern>   e.g. \"budget*.xlsx\"");
    auto repo = open_repo(a);
    const std::vector<SnapshotRecord> snaps = load_snapshot_records(*repo);
    bool truncated = false;
    const std::vector<FoundItem> items = find_items(*repo, snaps, a.positional[0], 500, &truncated);
    if (g_json) {
        nlohmann::json list = nlohmann::json::array();
        for (const FoundItem& f : items) {
            list.push_back({{"path", f.path},
                            {"folder", f.kind == NodeKind::Dir},
                            {"size", f.size},
                            {"snapshots", f.snapshots.size()},
                            {"newest", snaps[f.snapshots.front()].snapshot.time}});
        }
        emit({{"event", "find"}, {"items", list}, {"truncated", truncated}});
        return kExitOk;
    }
    for (const FoundItem& f : items) {
        const std::string newest = os::format_time_local(snaps[f.snapshots.front()].snapshot.time);
        out((f.kind == NodeKind::Dir ? std::string("d ") : std::string("- ")) + rpad(format_bytes(f.size), 11) + "  " +
            pad(newest, 20) + pad(std::to_string(f.snapshots.size()) + " snap", 9) + f.path + "\n");
    }
    out(std::to_string(items.size()) + " match(es)" + (truncated ? " (more exist; narrow the pattern)" : "") + "\n");
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
        if (s.pinned) line += "  pinned";
        if (!s.note.empty()) line += "  \"" + s.note + "\"";
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
    o.permissions = a.has("--permissions");
    o.links = !a.has("--no-links");
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
    o.should_cancel = [] { return stop_requested(); };

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
    if (const auto share = a.value("--read-share")) {
        if (!o.read_data) throw Error("--read-share needs --read-data");
        char* end = nullptr;
        const double pct = std::strtod(share->c_str(), &end);
        if (end == share->c_str() || (*end != '\0' && std::string_view(end) != "%") || !(pct > 0 && pct <= 100))
            throw Error("--read-share takes a percentage from 1 to 100");
        o.read_share = pct / 100.0;
        o.sample_seed = os::now();
    }
    o.should_cancel = [] { return stop_requested(); };
    o.on_progress = [](const std::string& msg) { status("Checking " + msg); };
    o.on_error = [](const std::string& msg) { err("error: " + msg + "\n"); };
    const auto start = std::chrono::steady_clock::now();
    const VerifyResult r = run_verify(*repo, o);
    clear_status();
    out("Checked " + std::to_string(r.packs) + " packs, " + std::to_string(r.blobs) + " blobs, " +
        std::to_string(r.snapshots) + " snapshots, " + std::to_string(r.trees) + " folders" +
        (o.read_data ? ", read " + format_bytes(r.bytes_read) + " (" + std::to_string(r.packs_read) + " packs)"
                     : std::string()) +
        " in " +
        format_duration(seconds_since(start)) + "\n");
    if (r.errors > 0) {
        out(std::to_string(r.errors) + " problem(s) found\n");
        return kExitAuth;
    }
    out(o.read_data ? (o.read_share < 1.0 ? "No problems found in the data read.\n" : "No problems found; all data is readable.\n")
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
    if (a.command == "drives") return cmd_drives(a);
    if (a.command == "run-job") return cmd_run_job(a);  // applies the job's own resource settings
    if (a.command == "check-job") return cmd_check_job(a);
    if (a.command == "test-restore") return cmd_test_restore(a);
    if (a.command == "copy") return cmd_copy(a);
    if (a.command == "cloud-rollback") return cmd_cloud_rollback(a);
    if (a.command == "mount") return cmd_mount(a);
    if (a.command == "admin-job") return cmd_admin_job(a);
    if (a.command == "uninstall-cleanup") return cmd_uninstall_cleanup();
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
    if (a.command == "repair") return cmd_repair(a);
    if (a.command == "unlock") return cmd_unlock(a);
    if (a.command == "key") return cmd_key(a);
    if (a.command == "pin") return cmd_pin(a, true);
    if (a.command == "unpin") return cmd_pin(a, false);
    if (a.command == "note") return cmd_note(a);
    if (a.command == "history") return cmd_history(a);
    if (a.command == "diff") return cmd_diff(a);
    if (a.command == "estimate") return cmd_estimate(a);
    if (a.command == "find") return cmd_find(a);
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
#if defined(_MSC_VER) && defined(_DEBUG)
    // With VAULTIK_LEAK_CHECK set, a debug build reports every heap block still allocated at exit.
    wchar_t* leak_check = nullptr;
    std::size_t leak_check_len = 0;
    if (_wdupenv_s(&leak_check, &leak_check_len, L"VAULTIK_LEAK_CHECK") == 0 && leak_check != nullptr) {
        std::free(leak_check);
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    }
#endif
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
