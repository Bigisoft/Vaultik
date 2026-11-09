#include <doctest/doctest.h>

#include <filesystem>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
using namespace vaultik::jobs;
namespace fs = std::filesystem;

namespace {

struct TempStore {
    fs::path path;
    TempStore() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-jobs-" + to_hex(rnd));
    }
    ~TempStore() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string dir() const {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }
};

Job sample() {
    Job j;
    j.name = "My Documents!";
    j.repository = "E:/Backups/vaultik";
    j.sources = {"C:/Users/me/Documents", "D:/Projects"};
    j.excludes = {"*.tmp", "node_modules"};
    j.tags = {"daily"};
    j.schedule.kind = Schedule::Kind::Weekly;
    j.schedule.weekdays = 0b0000101;  // Mon, Wed
    j.schedule.hour = 21;
    j.schedule.minute = 30;
    j.resources = priority_preset("low");
    j.resources.write_rate = 20ull << 20;
    return j;
}

}  // namespace

TEST_CASE("job ids are file-name safe and unique") {
    CHECK(make_id("My Documents!", {}) == "my-documents");
    CHECK(make_id("  ", {}) == "job");
    CHECK(make_id("Фото", {}) == "job");
    CHECK(make_id("Docs", {"docs"}) == "docs-2");
    CHECK(make_id("Docs", {"docs", "docs-2"}) == "docs-3");
}

TEST_CASE("job JSON round trip") {
    Job j = sample();
    j.id = "my-documents";
    j.run_as_admin = true;
    j.vss_skip_sql = true;
    const Job back = job_from_json(to_json(j));
    CHECK(back.run_as_admin);
    CHECK(back.vss_skip_sql);
    CHECK(back.id == j.id);
    CHECK(back.name == j.name);
    CHECK(back.sources == j.sources);
    CHECK(back.excludes == j.excludes);
    CHECK(back.schedule.kind == Schedule::Kind::Weekly);
    CHECK(back.schedule.weekdays == 0b0000101);
    CHECK(back.schedule.hour == 21);
    CHECK(back.resources.cpu == CpuPriority::Low);
    CHECK(back.resources.io == IoPriority::Low);
    CHECK(back.resources.write_rate == (20ull << 20));
    CHECK(describe(back.schedule) == "Mon, Wed at 21:30");
    CHECK_THROWS_AS(job_from_json("{not json"), Error);
}

TEST_CASE("health checks: settings and when they are due") {
    Job j = sample();
    j.id = "docs";
    j.health.sample_percent = 25;
    j.health.monthly = false;
    Job back = job_from_json(to_json(j));
    CHECK(back.health.weekly);
    CHECK(back.health.sample_percent == 25);
    CHECK_FALSE(back.health.monthly);

    // Jobs saved before health checks get the defaults: no monthly full check of cloud storage.
    auto old = nlohmann::json::parse(to_json(j));
    old.erase("health");
    back = job_from_json(old.dump());
    CHECK(back.health.weekly);
    CHECK(back.health.sample_percent == 10);
    CHECK(back.health.monthly);
    old["repository"] = "s3:https://s3.example.com/bucket";
    old["cloud_key_id"] = "AKIDEXAMPLE";
    CHECK_FALSE(job_from_json(old.dump()).health.monthly);

    j.health.sample_percent = 0;
    CHECK_THROWS_AS(validate(j), Error);

    // Records keep their kind; old records without one are backups.
    RunRecord r;
    r.kind = RunKind::FullCheck;
    r.started = 5;
    CHECK(record_from_json(to_json(r)).kind == RunKind::FullCheck);
    CHECK(record_from_json(R"({"started":1,"finished":2,"status":"success"})").kind == RunKind::Backup);

    constexpr std::uint64_t day = 864'000'000'000ull;
    const std::uint64_t now = 1000 * day;
    const auto run = [](RunKind kind, std::uint64_t at, RunStatus status = RunStatus::Success) {
        RunRecord rec;
        rec.kind = kind;
        rec.started = at - 10;
        rec.finished = at;
        rec.status = status;
        return rec;
    };
    HealthPolicy h;  // weekly and monthly
    CHECK(due_check(h, {}, now) == RunKind::FullCheck);
    std::vector<RunRecord> history = {run(RunKind::FullCheck, now - 20 * day), run(RunKind::Backup, now - day)};
    CHECK(due_check(h, history, now) == RunKind::Check);  // full check recent, no check this week
    history.push_back(run(RunKind::Check, now - 2 * day));
    CHECK_FALSE(due_check(h, history, now).has_value());
    history.push_back(run(RunKind::Check, now - day, RunStatus::Failed));  // problems found still count
    CHECK_FALSE(due_check(h, history, now + 5 * day).has_value());
    CHECK(due_check(h, history, now + 11 * day) == RunKind::FullCheck);  // 31 days after the full check
    h.monthly = false;
    CHECK(due_check(h, history, now + 11 * day) == RunKind::Check);
    h.weekly = false;
    CHECK_FALSE(due_check(h, history, now + 11 * day).has_value());

    // A cancelled check is tried again.
    h = HealthPolicy{};
    history = {run(RunKind::FullCheck, now - day, RunStatus::Cancelled)};
    CHECK(due_check(h, history, now) == RunKind::FullCheck);

    history = {run(RunKind::Backup, 1), run(RunKind::Check, 2), run(RunKind::Backup, 3)};
    REQUIRE(last_backup(history) != nullptr);
    CHECK(last_backup(history)->finished == 3);
    CHECK(last_check(history)->finished == 2);
    CHECK(last_check({run(RunKind::Backup, 1)}) == nullptr);
    RunRecord damaged = run(RunKind::Check, 4, RunStatus::Failed);
    CHECK_FALSE(found_damage(damaged));  // could not run
    damaged.errors = 2;
    CHECK(found_damage(damaged));
}

#ifdef _WIN32
TEST_CASE("jobs find their drive under another letter") {
    TempStore tmp;
    const fs::path repo = tmp.path / "repo";
    fs::create_directories(repo);
    const std::u8string u = repo.u8string();
    const std::string repo_dir(u.begin(), u.end());

    const os::VolumeLocation loc = os::volume_of(repo_dir);
    REQUIRE_FALSE(loc.volume.empty());
    CHECK(loc.volume.starts_with("\\\\?\\Volume{"));
    CHECK(os::volume_root(loc.volume).size() >= 3);
    CHECK(os::volume_root("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\").empty());

    JobStore store(tmp.dir());
    Job j = sample();
    j.repository = repo_dir;
    j.backup_on_connect = true;
    store.save(j);
    CHECK(j.repository_volume == loc.volume);
    CHECK(j.repository_relative == loc.relative);
    CHECK(drive_connected(j));

    // The drive came back as Q: (the stored letter is stale): the job points at its real place.
    Job moved = j;
    moved.repository = "Q:\\" + j.repository_relative;
    relocate(moved);
    CHECK(os::fold_case(moved.repository) == os::fold_case(os::absolute_path(repo_dir)));

    // A drive that is not connected leaves the job as it is.
    Job away = j;
    away.repository_volume = "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\";
    away.repository = "Q:\\Backups";
    relocate(away);
    CHECK(away.repository == "Q:\\Backups");
    CHECK_FALSE(drive_connected(away));

    // Records say when a run was started by connecting the drive.
    RunRecord r;
    r.on_connect = true;
    CHECK(record_from_json(to_json(r)).on_connect);
    CHECK_FALSE(record_from_json(to_json(RunRecord{})).on_connect);

    // Only a folder on a drive can be backed up on connect.
    Job cloud = sample();
    cloud.repository = "s3:https://s3.example.com/bucket";
    cloud.cloud_key_id = "AKIDEXAMPLE";
    cloud.backup_on_connect = true;
    CHECK_THROWS_AS(validate(cloud), Error);
}
#endif

TEST_CASE("working hours") {
    WorkHours w;
    CHECK_FALSE(in_work_hours(w, 0, 10 * 60));  // off
    w.enabled = true;                            // Monday to Friday, 09:00 to 17:00
    CHECK(in_work_hours(w, 0, 9 * 60));
    CHECK(in_work_hours(w, 4, 16 * 60 + 59));
    CHECK_FALSE(in_work_hours(w, 4, 17 * 60));
    CHECK_FALSE(in_work_hours(w, 2, 8 * 60 + 59));
    CHECK_FALSE(in_work_hours(w, 5, 10 * 60));  // Saturday
    w.start_minute = 22 * 60;                    // a night shift across midnight
    w.end_minute = 6 * 60;
    CHECK(in_work_hours(w, 1, 23 * 60));
    CHECK(in_work_hours(w, 1, 5 * 60));
    CHECK_FALSE(in_work_hours(w, 1, 12 * 60));

    Job j = sample();
    j.id = "w";
    j.work_hours = w;
    j.work_hours.read_rate = 5ull << 20;
    const Job back = job_from_json(to_json(j));
    CHECK(back.work_hours.enabled);
    CHECK(back.work_hours.start_minute == 22 * 60);
    CHECK(back.work_hours.end_minute == 6 * 60);
    CHECK(back.work_hours.read_rate == (5ull << 20));
    j.work_hours.weekdays = 0;
    CHECK_THROWS_AS(validate(j), Error);
}

TEST_CASE("job validation") {
    Job j = sample();
    j.sources.clear();
    CHECK_THROWS_AS(validate(j), Error);
    j = sample();
    j.schedule.hour = 24;
    CHECK_THROWS_AS(validate(j), Error);
    j = sample();
    j.schedule.weekdays = 0;
    CHECK_THROWS_AS(validate(j), Error);
    j = sample();
    j.resources.max_memory = 1 << 20;
    CHECK_THROWS_AS(validate(j), Error);
}

TEST_CASE("schedule descriptions") {
    Schedule s;
    CHECK(describe(s) == "Manual");
    s.kind = Schedule::Kind::Daily;
    CHECK(describe(s) == "Daily at 02:00");
    s.kind = Schedule::Kind::Hourly;
    s.every_hours = 4;
    s.minute = 15;
    CHECK(describe(s) == "Every 4 hours at :15");
    s.kind = Schedule::Kind::Weekly;
    s.weekdays = 0x1F;
    CHECK(describe(s) == "Weekdays at 02:15");
}

TEST_CASE("scheduler definitions") {
    Job j = sample();
    j.id = "docs";
#ifdef _WIN32
    const std::string xml = schedule_definition(j, "C:\\Program Files\\Vaultik");
    CHECK(xml.find("<Monday/><Wednesday/>") != std::string::npos);
    CHECK(xml.find("<StartBoundary>2026-01-01T21:30:00</StartBoundary>") != std::string::npos);
    CHECK(xml.find("<Command>C:\\Program Files\\Vaultik\\vaultik-task.exe</Command>") != std::string::npos);
    CHECK(xml.find("<Arguments>run-job docs</Arguments>") != std::string::npos);
    CHECK(xml.find("<StartWhenAvailable>true</StartWhenAvailable>") != std::string::npos);
    CHECK(xml.find("<RunLevel>LeastPrivilege</RunLevel>") != std::string::npos);
    j.run_as_admin = true;
    CHECK(schedule_definition(j, "C:\\x").find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos);
    CHECK(schedule_definition(j, "C:\\x").find("--admin</Arguments>") != std::string::npos);
    CHECK(xml.find("--admin") == std::string::npos);
    j.run_as_admin = false;
    j.schedule.kind = Schedule::Kind::Hourly;
    j.schedule.every_hours = 6;
    CHECK(schedule_definition(j, "C:\\x").find("<Interval>PT6H</Interval>") != std::string::npos);
#else
    CHECK(schedule_definition(j, "/usr/bin") == "Mon,Wed *-*-* 21:30:00");
    j.schedule.kind = Schedule::Kind::Daily;
    CHECK(schedule_definition(j, "/usr/bin") == "*-*-* 21:30:00");
    j.schedule.kind = Schedule::Kind::Hourly;
    j.schedule.every_hours = 6;
    CHECK(schedule_definition(j, "/usr/bin") == "*-*-* 00/6:30:00");
#endif
}

TEST_CASE("job store: save, list, find, password, history, remove") {
    TempStore tmp;
    JobStore store(tmp.dir());
    CHECK(store.list().empty());

    Job a = sample();
    store.save(a);
    CHECK(a.id == "my-documents");
    Job b = sample();
    b.name = "my documents!";  // same id base, different job
    store.save(b);
    CHECK(b.id == "my-documents-2");

    const auto all = store.list();
    REQUIRE(all.size() == 2);
    CHECK(store.find("my-documents-2").has_value());
    CHECK(store.find("MY DOCUMENTS!").has_value());
    CHECK_FALSE(store.find("nope").has_value());

    CHECK(store.password(a.id).empty());
    store.set_password(a.id, "correct horse battery staple");
    CHECK(store.password(a.id) == "correct horse battery staple");

    RunRecord r;
    r.started = 100;
    r.finished = 200;
    r.status = RunStatus::Partial;
    r.snapshot = "abcd1234";
    r.errors = 2;
    store.append_history(a.id, r);
    r.status = RunStatus::Success;
    store.append_history(a.id, r);
    const auto h = store.history(a.id);
    REQUIRE(h.size() == 2);
    CHECK(h[0].status == RunStatus::Partial);
    CHECK(h[0].errors == 2);
    CHECK(h[1].status == RunStatus::Success);
    CHECK(store.history(a.id, 1).size() == 1);

    store.remove(a.id);
    CHECK(store.list().size() == 1);
    CHECK(store.password(a.id).empty());
    CHECK(store.history(a.id).empty());
    CHECK_THROWS_AS(store.remove("../evil"), Error);
}

#ifdef _WIN32
TEST_CASE("admin-only checks for jobs with administrator rights") {
    const std::string program_files = os::get_env("ProgramFiles");
    REQUIRE(!program_files.empty());
    std::string why;
    CHECK_MESSAGE(os::admin_only_chain(program_files, &why), why);
    CHECK(os::admin_only_chain(os::join(program_files, "Common Files"), &why));

    // A folder the user made: the user can change it, so it is refused, with a reason.
    TempStore t;
    fs::create_directories(t.path);
    why.clear();
    CHECK_FALSE(os::admin_only(t.dir(), false, &why));
    CHECK(!why.empty());
    CHECK_THROWS_AS(check_program_dir(t.dir()), Error);

    CHECK(os::current_user_sid().starts_with("S-1-5-"));
    CHECK(admin_store_dir().find("\\Vaultik\\admin\\S-1-5-") != std::string::npos);

    // The user's own history alone when the job has no admin copy.
    const JobStore store(t.dir());
    Job j = sample();
    store.save(j);
    RunRecord r;
    r.started = 5;
    store.append_history(j.id, r);
    CHECK(combined_history(store, j.id).size() == 1);
}
#endif
