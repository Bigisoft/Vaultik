#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>

#include "vaultik/jobs/computers.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
using namespace vaultik::jobs;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "test password";
constexpr std::uint64_t kDay = 24ull * 3600 * 10000000;

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-test-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::unique_ptr<Repository> make_repo(const fs::path& dir) {
    {
        LocalBackend backend(u8(dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

std::unique_ptr<Repository> reopen(const fs::path& dir) {
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

JobStatus sample_job(const std::string& id, std::uint64_t now) {
    JobStatus j;
    j.id = id;
    j.name = "Job " + id;
    j.schedule = "Daily at 02:00";
    j.last_run = now - kDay / 2;
    j.last_kind = "backup";
    j.last_result = "success";
    j.files = 12;
    j.bytes = 3456;
    j.last_backup = j.last_run;
    j.last_backup_result = "success";
    j.last_success = j.last_run;
    j.next_run = now + kDay / 2;
    return j;
}

// A folder that, on the first write of a status note, lets "another run" of the same computer
// write its own note over it right after (two jobs ending together).
class RacingBackend : public Backend {
public:
    RacingBackend(std::unique_ptr<Backend> inner, std::function<void()> race)
        : inner_(std::move(inner)), race_(std::move(race)) {}
    std::string location() const override { return inner_->location(); }
    bool exists(const std::string& name) override { return inner_->exists(name); }
    void put(const std::string& name, ByteSpan data) override {
        inner_->put(name, data);
        if (name.starts_with("status/") && race_) {
            auto race = std::move(race_);
            race_ = nullptr;
            race();
        }
    }
    Bytes get(const std::string& name) override { return inner_->get(name); }
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override {
        return inner_->get_range(name, offset, length);
    }
    std::uint64_t size(const std::string& name) override { return inner_->size(name); }
    std::vector<std::string> list(const std::string& dir) override { return inner_->list(dir); }
    void remove(const std::string& name) override { inner_->remove(name); }

private:
    std::unique_ptr<Backend> inner_;
    std::function<void()> race_;
};

Snapshot snap(const std::string& host, std::uint64_t time, std::uint64_t bytes, std::uint64_t stored) {
    Snapshot s;
    s.hostname = host;
    s.time = time;
    s.stats.bytes = bytes;
    s.stats.stored_bytes = stored;
    return s;
}

}  // namespace

TEST_CASE("a status record survives sealing and detects tampering") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    const std::uint64_t now = os::now();
    HostStatus s;
    s.host = "office-pc";
    s.version = "0.7.0";
    s.updated = now;
    s.jobs.push_back(sample_job("docs", now));
    s.jobs[0].warnings = {"backup drive: WD 2627: warning"};
    s.jobs[0].damage = true;

    const Id id = computer_id(*repo, "machine-1");
    write_status(*repo, id, s);
    CHECK(repo->backend().exists(status_name(id)));
    CHECK(status_name(id) == "status/" + id_hex(id));

    const auto back = read_status(*repo, id);
    REQUIRE(back.has_value());
    CHECK(back->host == "office-pc");
    CHECK(back->version == "0.7.0");
    CHECK(back->updated == now);
    REQUIRE(back->jobs.size() == 1);
    CHECK(back->jobs[0].id == "docs");
    CHECK(back->jobs[0].files == 12);
    CHECK(back->jobs[0].next_run == s.jobs[0].next_run);
    CHECK(back->jobs[0].damage);
    CHECK(back->jobs[0].warnings == s.jobs[0].warnings);

    // The plaintext never shows in the file.
    const Bytes sealed = repo->backend().get(status_name(id));
    const std::string raw(sealed.begin(), sealed.end());
    CHECK(raw.find("office-pc") == std::string::npos);

    // A changed byte fails authentication.
    Bytes bad = sealed;
    bad[bad.size() / 2] ^= 0x01;
    CHECK_THROWS_AS(open_status(*repo, id, bad), AuthError);
    // So does the record of one computer put under another computer's name.
    CHECK_THROWS_AS(open_status(*repo, computer_id(*repo, "machine-2"), sealed), AuthError);
    // And a record from another repository.
    auto other = make_repo(tmp.path / "other");
    CHECK_THROWS(open_status(*other, id, sealed));

    // list_statuses reports a damaged record instead of failing, and skips foreign names.
    repo->backend().put(status_name(id), bad);
    const std::string junk = "not a status record";
    repo->backend().put("status/readme.txt", ByteSpan(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size()));
    const std::vector<StatusFile> files = list_statuses(*repo);
    REQUIRE(files.size() == 1);
    CHECK_FALSE(files[0].status.has_value());
    CHECK_FALSE(files[0].error.empty());
}

TEST_CASE("the per-computer id is stable and differs between repositories") {
    TempDir tmp;
    auto a = make_repo(tmp.path / "a");
    auto b = make_repo(tmp.path / "b");
    const Id a1 = computer_id(*a, "4c4c4544-0042-3510-8048-b4c04f4e3732");
    CHECK(a1 == computer_id(*a, "4c4c4544-0042-3510-8048-b4c04f4e3732"));
    CHECK(a1 == computer_id(*reopen(tmp.path / "a"), "4c4c4544-0042-3510-8048-b4c04f4e3732"));
    CHECK(a1 != computer_id(*a, "4c4c4544-0042-3510-8048-b4c04f4e3733"));
    CHECK(a1 != computer_id(*b, "4c4c4544-0042-3510-8048-b4c04f4e3732"));
    // Not a plain hash of the machine id: nobody without the key can link it to a computer.
    const std::string m = "4c4c4544-0042-3510-8048-b4c04f4e3732";
    CHECK(a1 != blake3(ByteSpan(reinterpret_cast<const std::uint8_t*>(m.data()), m.size())));
    // This computer has a machine id at all.
    CHECK_FALSE(os::machine_id().empty());
    CHECK(os::machine_id() == os::machine_id());
}

TEST_CASE("updating a computer's record keeps its other jobs") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    const std::uint64_t now = os::now();
    const Id id = computer_id(*repo, "machine-1");
    update_status(*repo, id, "pc", "0.7.0", sample_job("b", now), now);
    update_status(*repo, id, "pc", "0.7.0", sample_job("a", now), now);
    JobStatus again = sample_job("b", now);
    again.last_result = "failed";
    update_status(*repo, id, "pc-renamed", "0.7.1", again, now + 1);

    const auto s = read_status(*repo, id);
    REQUIRE(s.has_value());
    CHECK(s->host == "pc-renamed");
    CHECK(s->version == "0.7.1");
    CHECK(s->updated == now + 1);
    REQUIRE(s->jobs.size() == 2);
    CHECK(s->jobs[0].id == "a");
    CHECK(s->jobs[1].id == "b");
    CHECK(s->jobs[1].last_result == "failed");

    // A damaged record is replaced, not a reason to fail.
    Bytes sealed = repo->backend().get(status_name(id));
    sealed[20] ^= 0xFF;
    repo->backend().put(status_name(id), sealed);
    update_status(*repo, id, "pc", "0.7.1", sample_job("c", now), now);
    const auto fresh = read_status(*repo, id);
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->jobs.size() == 1);
    CHECK(fresh->jobs[0].id == "c");
}

TEST_CASE("two jobs of one computer ending together both stay in its record") {
    TempDir tmp;
    auto first = make_repo(tmp.path / "repo");
    const std::uint64_t now = os::now();
    const Id id = computer_id(*first, "machine-1");
    update_status(*first, id, "pc", "0.7.0", sample_job("a", now), now);
    // Job b's run read the record (with a) before job c's run wrote it, and writes after it:
    // c's entry is lost from the file, and c's run must notice and write it again.
    auto other = reopen(tmp.path / "repo");
    HostStatus stale = *read_status(*other, id);
    auto racing = Repository::open(std::make_unique<RacingBackend>(std::make_unique<LocalBackend>(u8(tmp.path / "repo")),
                                                                   [&] {
                                                                       stale.jobs.push_back(sample_job("b", now));
                                                                       write_status(*other, id, stale);
                                                                   }),
                                   kPassword);
    update_status(*racing, id, "pc", "0.7.0", sample_job("c", now), now);
    const auto s = read_status(*first, id);
    REQUIRE(s.has_value());
    REQUIRE(s->jobs.size() == 3);
    CHECK(s->jobs[0].id == "a");
    CHECK(s->jobs[1].id == "b");
    CHECK(s->jobs[2].id == "c");
}

TEST_CASE("removing a retired computer's status note") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    const std::uint64_t now = os::now();
    const Id old_pc = computer_id(*repo, "machine-old");
    const Id twin1 = computer_id(*repo, "machine-twin-1");
    const Id twin2 = computer_id(*repo, "machine-twin-2");
    const Id mine = computer_id(*repo, os::machine_id());
    update_status(*repo, old_pc, "Old-Laptop", "0.7.0", sample_job("a", now), now);
    update_status(*repo, twin1, "Twin", "0.7.0", sample_job("a", now), now);
    update_status(*repo, twin2, "twin", "0.7.0", sample_job("a", now), now);
    update_status(*repo, mine, os::hostname(), "0.7.0", sample_job("a", now), now);
    const std::vector<StatusFile> files = list_statuses(*repo);
    REQUIRE(files.size() == 4);
    const std::string me = status_name(mine);

    // By name (any case), by the whole id, by its start, by the file name.
    CHECK(resolve_forget(files, "old-laptop", me) == status_name(old_pc));
    CHECK(resolve_forget(files, id_hex(old_pc), me) == status_name(old_pc));
    CHECK(resolve_forget(files, id_hex(old_pc).substr(0, 8), me) == status_name(old_pc));
    CHECK(resolve_forget(files, status_name(old_pc), me) == status_name(old_pc));
    // Not too short an id, not an unknown name, not two computers of one name, never this one.
    CHECK_THROWS_AS(resolve_forget(files, id_hex(old_pc).substr(0, 4), me), Error);
    CHECK_THROWS_AS(resolve_forget(files, "nobody", me), Error);
    CHECK_THROWS_AS(resolve_forget(files, "", me), Error);
    CHECK_THROWS_AS(resolve_forget(files, "TWIN", me), Error);
    CHECK(resolve_forget(files, id_hex(twin2), me) == status_name(twin2));
    CHECK_THROWS_AS(resolve_forget(files, me, me), Error);
    CHECK_THROWS_AS(resolve_forget(files, os::hostname(), me), Error);
    CHECK(resolve_computer(files, os::hostname()) == me);  // asking needs no such rule here

    // Removing it takes a request addressed to it along; the others stay.
    BackupRequest r;
    r.from = "pc";
    r.created = now;
    r.jobs = {"a"};
    write_request(*repo, old_pc, r);
    CHECK(forget_computer(*repo, "Old-Laptop") == "Old-Laptop");
    CHECK_FALSE(read_status(*repo, old_pc).has_value());
    CHECK_FALSE(read_request(*repo, old_pc).has_value());
    CHECK(list_statuses(*repo).size() == 3);
    CHECK_THROWS_AS(forget_computer(*repo, os::hostname()), Error);
    CHECK(read_status(*repo, mine).has_value());
}

TEST_CASE("asking another computer to back up") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");
    const std::uint64_t now = os::now();
    const Id other = computer_id(*repo, "machine-other");
    const Id mine = computer_id(*repo, os::machine_id());
    update_status(*repo, other, "Desk", "0.7.0", sample_job("docs", now), now);
    update_status(*repo, other, "Desk", "0.7.0", sample_job("photos", now), now);
    update_status(*repo, mine, os::hostname(), "0.7.0", sample_job("x", now), now);
    const std::vector<StatusFile> files = list_statuses(*repo);
    const auto file_of = [&](const Id& id) {
        for (const StatusFile& f : files)
            if (f.name == status_name(id)) return f;
        FAIL("no record");
        return StatusFile{};
    };

    // All its jobs by default; never this computer itself.
    ask_backup(*repo, file_of(other), {}, "Laptop", now);
    auto r = read_request(*repo, other);
    REQUIRE(r.has_value());
    CHECK(r->from == "Laptop");
    CHECK(r->created == now);
    CHECK(r->state == "pending");
    CHECK(r->jobs == std::vector<std::string>{"docs", "photos"});
    CHECK_THROWS_AS(ask_backup(*repo, file_of(mine), {}, "Laptop", now), Error);
    StatusFile empty = file_of(other);
    empty.status->jobs.clear();
    CHECK_THROWS_AS(ask_backup(*repo, empty, {}, "Laptop", now), Error);

    // Sealed for that computer: moved to another one's name it does not open.
    repo->backend().put(request_name(mine), repo->backend().get(request_name(other)));
    CHECK_THROWS_AS(read_request(*repo, mine), AuthError);
    repo->backend().remove(request_name(mine));
    CHECK(BackupRequest{}.state == "pending");
    CHECK(request_from_json(to_json(*r)).jobs == r->jobs);
    CHECK_THROWS_AS(request_from_json("[]"), Error);

    // What it asks of a computer: only its own jobs here, only while fresh.
    CHECK(requested_jobs(*r, {"photos", "music"}, now) == std::vector<std::string>{"photos"});
    CHECK(requested_jobs(*r, {"music"}, now).empty());
    CHECK(requested_jobs(*r, {"docs", "photos"}, now + kRequestExpiry + 1).empty());
    CHECK(requested_jobs(*r, {"docs"}, now + kRequestExpiry - 1) == std::vector<std::string>{"docs"});
    const auto left = after_run(*r, "docs");
    REQUIRE(left.has_value());
    CHECK(left->jobs == std::vector<std::string>{"photos"});
    CHECK_FALSE(after_run(*left, "photos").has_value());
    CHECK(after_run(*left, "music")->jobs == left->jobs);
    CHECK(request_step(*r, 0) == RequestStep::Take);
    CHECK(request_step(*r, r->created) == RequestStep::Drop);  // still "pending": put back
    BackupRequest running = *r;
    running.state = "running";
    CHECK(request_step(running, r->created) == RequestStep::Keep);
    CHECK(request_step(running, r->created + 1) == RequestStep::Drop);

    // The view shows it next to the computer it asks.
    bool shown = false;
    for (const Computer& c : list_computers(*repo, now))
        if (c.status_name == status_name(other)) shown = c.request.has_value() && c.request->from == "Laptop";
    CHECK(shown);
    const std::vector<RequestFile> listed = list_requests(*repo);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].target == status_name(other));

    // This computer takes up a request addressed to it, and answers it job by job.
    BackupRequest to_me;
    to_me.from = "Desk";
    to_me.created = now;
    to_me.jobs = {"x", "y"};
    write_request(*repo, mine, to_me);
    const std::string state = u8(tmp.path / "state");
    fs::create_directories(tmp.path / "state");
    CHECK(take_request(*repo, {"x", "z"}, {"x", "z"}, now + 5, state) == std::vector<std::string>{"x"});
    // Taken up: a later look does not run it again.
    CHECK(take_request(*repo, {"x", "z"}, {"x", "z"}, now + 10, state).empty());
    CHECK(read_request(*repo, mine).has_value());
    auto taken = read_request(*repo, mine);
    REQUIRE(taken.has_value());
    CHECK(taken->state == "running");
    CHECK(taken->started == now + 5);
    finish_request(*repo, "z");  // not asked for: no change
    CHECK(read_request(*repo, mine)->jobs.size() == 2);
    finish_request(*repo, "x");
    CHECK(read_request(*repo, mine)->jobs == std::vector<std::string>{"y"});
    finish_request(*repo, "y");
    CHECK_FALSE(read_request(*repo, mine).has_value());
    // The storage side puts the old, genuine request back: it is not run again, and removed.
    write_request(*repo, mine, to_me);
    CHECK(take_request(*repo, {"x"}, {"x"}, now + 20, state).empty());
    CHECK_FALSE(read_request(*repo, mine).has_value());
    BackupRequest older = to_me;
    older.created = now - 100;
    write_request(*repo, mine, older);
    CHECK(take_request(*repo, {"x"}, {"x"}, now + 20, state).empty());
    CHECK_FALSE(read_request(*repo, mine).has_value());
    // A newer one is taken up.
    BackupRequest newer = to_me;
    newer.created = now + 50;
    write_request(*repo, mine, newer);
    CHECK(take_request(*repo, {"x"}, {"x"}, now + 60, state) == std::vector<std::string>{"x"});
    remove_request(*repo, mine);
    // One for a job with administrator rights only (known, not run from here) waits for that
    // job's own run instead of being removed unanswered.
    BackupRequest admin = to_me;
    admin.created = now + 70;
    admin.jobs = {"a"};
    write_request(*repo, mine, admin);
    CHECK(take_request(*repo, {"a", "x"}, {"x"}, now + 80, state).empty());
    REQUIRE(read_request(*repo, mine).has_value());
    CHECK(read_request(*repo, mine)->state == "pending");
    finish_request(*repo, "a");
    CHECK_FALSE(read_request(*repo, mine).has_value());
    // One for jobs this computer does not have here, or an expired one, is removed unanswered.
    to_me.created = now + 100;
    write_request(*repo, mine, to_me);
    CHECK(take_request(*repo, {"q"}, {"q"}, now + 100, state).empty());
    CHECK_FALSE(read_request(*repo, mine).has_value());
    write_request(*repo, mine, to_me);
    CHECK(take_request(*repo, {"x"}, {"x"}, now + 100 + kRequestExpiry + 1, state).empty());
    CHECK_FALSE(read_request(*repo, mine).has_value());
    // Requests are not status notes.
    CHECK(list_statuses(*repo).size() == 2);
}

TEST_CASE("job status from the history") {
    Job job;
    job.id = "docs";
    job.name = "Documents";
    job.schedule.kind = Schedule::Kind::Daily;
    const std::uint64_t now = os::now();
    std::vector<RunRecord> history;
    RunRecord ok;
    ok.kind = RunKind::Backup;
    ok.started = now - 3 * kDay;
    ok.finished = ok.started + 1000;
    ok.status = RunStatus::Success;
    ok.files = 5;
    history.push_back(ok);
    RunRecord failed = ok;
    failed.started = now - kDay;
    failed.finished = failed.started + 1000;
    failed.status = RunStatus::Failed;
    failed.drive = "Disk 1: warning";
    history.push_back(failed);
    RunRecord check;
    check.kind = RunKind::Check;
    check.started = now - kDay / 2;
    check.finished = check.started + 1000;
    check.status = RunStatus::Success;
    history.push_back(check);

    const JobStatus s = job_status(job, history, now);
    CHECK(s.id == "docs");
    CHECK(s.name == "Documents");
    CHECK(s.last_run == check.finished);
    CHECK(s.last_kind == "check");
    CHECK(s.last_result == "success");
    CHECK(s.last_backup == failed.finished);
    CHECK(s.last_backup_result == "failed");
    CHECK(s.last_success == ok.finished);
    CHECK(s.next_run > now);
    CHECK(s.next_run <= now + kDay);
    CHECK_FALSE(s.damage);
    REQUIRE(s.warnings.size() == 1);
    CHECK(job_health(s, now) == Health::Failed);

    RunRecord damage = check;
    damage.finished += 10;
    damage.status = RunStatus::Failed;
    damage.errors = 2;
    history.push_back(damage);
    CHECK(job_status(job, history, now).damage);

    // No runs at all: a warning (nothing saved yet), never stale before its first schedule.
    const JobStatus empty = job_status(job, {}, now);
    CHECK(empty.last_run == 0);
    CHECK(job_health(empty, now) == Health::Warning);
}

TEST_CASE("next scheduled run") {
    const std::uint64_t now = os::now();
    Schedule manual;
    CHECK_FALSE(next_run(manual, now).has_value());

    Schedule daily;
    daily.kind = Schedule::Kind::Daily;
    daily.hour = 3;
    const auto d = next_run(daily, now);
    REQUIRE(d.has_value());
    CHECK(*d > now);
    CHECK(*d <= now + kDay + 3600ull * 10000000);  // a day, plus a clock change

    Schedule hourly;
    hourly.kind = Schedule::Kind::Hourly;
    hourly.every_hours = 4;
    hourly.minute = 15;
    const auto h = next_run(hourly, now);
    REQUIRE(h.has_value());
    CHECK(*h > now);
    CHECK(*h <= now + 4 * 3600ull * 10000000);
    CHECK((*h / 10000000) % 60 == 0);  // on the minute

    Schedule weekly;
    weekly.kind = Schedule::Kind::Weekly;
    weekly.weekdays = 0x01;  // Mondays
    const auto w = next_run(weekly, now);
    REQUIRE(w.has_value());
    CHECK(*w > now);
    CHECK(*w <= now + 7 * kDay + 3600ull * 10000000);
    weekly.weekdays = 0;
    CHECK_FALSE(next_run(weekly, now).has_value());

    // A paused job has no next run, so it is never stale however long it waits.
    daily.paused = true;
    CHECK_FALSE(next_run(daily, now).has_value());
    Job job;
    job.id = "docs";
    job.schedule = daily;
    RunRecord old;
    old.started = now - 30 * kDay;
    old.finished = old.started + 1000;
    const JobStatus s = job_status(job, {old}, now);
    CHECK(s.next_run == 0);
    CHECK(s.schedule == "Paused (Daily at 03:00)");
    CHECK(job_health(s, now) == Health::Ok);
}

TEST_CASE("staleness and health rules") {
    const std::uint64_t now = os::now();
    JobStatus j = sample_job("a", now);
    CHECK(job_health(j, now) == Health::Ok);

    // A scheduled run missed by a day: still fine; by more than 3 days: stale.
    j.next_run = now - kDay;
    CHECK(job_health(j, now) == Health::Ok);
    j.next_run = now - 3 * kDay - 1;
    CHECK(job_health(j, now) == Health::Stale);
    // A manual job is never stale.
    j.next_run = 0;
    j.last_success = now - 400 * kDay;
    CHECK(job_health(j, now) == Health::Ok);

    j = sample_job("a", now);
    j.last_backup_result = "partial";
    CHECK(job_health(j, now) == Health::Warning);
    j = sample_job("a", now);
    j.warnings = {"ransomware warning: many files changed"};
    CHECK(job_health(j, now) == Health::Warning);
    j = sample_job("a", now);
    j.last_kind = "copy";
    j.last_result = "failed";
    CHECK(job_health(j, now) == Health::Warning);
    // A failed backup outweighs being stale.
    j = sample_job("a", now);
    j.last_backup_result = "failed";
    j.next_run = now - 10 * kDay;
    CHECK(job_health(j, now) == Health::Failed);

    CHECK(worst(Health::Ok, Health::Stale) == Health::Stale);
    CHECK(worst(Health::Failed, Health::Warning) == Health::Failed);
    CHECK(worst(Health::Unknown, Health::Ok) == Health::Unknown);
}

TEST_CASE("computers merge status records with the snapshots") {
    const std::uint64_t now = os::now();
    std::vector<StatusFile> files;
    StatusFile office;
    office.name = "status/" + std::string(64, 'a');
    office.status = HostStatus{"OFFICE", "0.7.0", now, {sample_job("docs", now)}};
    files.push_back(office);
    StatusFile laptop;
    laptop.name = "status/" + std::string(64, 'b');
    JobStatus late = sample_job("docs", now);
    late.next_run = now - 5 * kDay;
    JobStatus fine = sample_job("photos", now);
    laptop.status = HostStatus{"laptop", "0.7.0", now - 6 * kDay, {fine, late}};
    files.push_back(laptop);
    StatusFile broken;
    broken.name = "status/" + std::string(64, 'c');
    broken.error = "authentication failed";
    files.push_back(broken);

    std::vector<Snapshot> snaps = {snap("office", now - 2 * kDay, 100, 10), snap("Office", now - kDay, 200, 20),
                                   snap("laptop", now - 6 * kDay, 50, 5), snap("old-pc", now - 90 * kDay, 7, 7)};

    const std::vector<Computer> list = merge_computers(files, snaps, now, office.name, "OFFICE");
    REQUIRE(list.size() == 4);
    // Sorted by host name: "?", laptop, office, old-pc.
    CHECK(list[0].host == "?");
    CHECK(list[0].health == Health::Unknown);
    CHECK_FALSE(list[0].status.has_value());

    CHECK(list[1].host == "laptop");
    CHECK(list[1].health == Health::Stale);
    CHECK(list[1].versions == 1);
    CHECK_FALSE(list[1].this_computer);

    CHECK(list[2].host == "OFFICE");
    CHECK(list[2].this_computer);
    CHECK(list[2].health == Health::Ok);
    CHECK(list[2].versions == 2);  // host names match whatever their case
    CHECK(list[2].latest == now - kDay);
    CHECK(list[2].latest_bytes == 200);
    CHECK(list[2].added_bytes == 30);

    CHECK(list[3].host == "old-pc");
    CHECK_FALSE(list[3].status.has_value());
    CHECK(list[3].status_name.empty());
    CHECK(list[3].versions == 1);
    CHECK(list[3].health == Health::Unknown);
    CHECK_FALSE(list[3].this_computer);

    // Without a record of its own, this computer is found by its name among the snapshots.
    const std::vector<Computer> mine = merge_computers({}, snaps, now, "status/" + std::string(64, 'f'), "old-pc");
    REQUIRE(mine.size() == 3);
    CHECK(mine[2].host == "old-pc");
    CHECK(mine[2].this_computer);

    // A record older than the computer's newest version (put back by the storage side, or not
    // written) is flagged, and the computer's health is at least Warning.
    CHECK_FALSE(list[2].record_behind);
    snaps.push_back(snap("OFFICE", now + 60 * 10000000ull, 300, 1));
    const std::vector<Computer> behind = merge_computers(files, snaps, now, office.name, "OFFICE");
    REQUIRE(behind.size() == 4);
    CHECK(behind[2].record_behind);
    CHECK(behind[2].health == Health::Warning);
    CHECK_FALSE(behind[1].record_behind);
}

TEST_CASE("prune and verify leave status records alone") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    fs::create_directories(src);
    {
        std::ofstream f(src / "a.txt", std::ios::binary);
        f << "some data to back up";
    }
    auto repo = make_repo(tmp.path / "repo");
    BackupOptions bo;
    bo.paths = {u8(src)};
    run_backup(*repo, bo);

    const std::uint64_t now = os::now();
    const Id id = computer_id(*repo, os::machine_id());
    update_status(*repo, id, os::hostname(), "0.7.0", sample_job("docs", now), now);
    const Bytes before = repo->backend().get(status_name(id));

    const PruneResult pr = run_prune(*repo, PruneOptions{});
    CHECK(pr.packs_deleted == 0);
    const VerifyResult vr = run_verify(*repo, VerifyOptions{});
    CHECK(vr.errors == 0);
    VerifyOptions full;
    full.read_data = true;
    CHECK(run_verify(*repo, full).errors == 0);

    CHECK(repo->backend().get(status_name(id)) == before);
    const std::vector<Computer> list = list_computers(*repo, now);
    REQUIRE(list.size() == 1);
    CHECK(list[0].this_computer);
    CHECK(list[0].versions == 1);
    REQUIRE(list[0].status.has_value());
    CHECK(list[0].status->jobs.size() == 1);
}
