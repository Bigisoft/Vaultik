#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/drive_lock.hpp"
#include "vaultik/jobs/eject.hpp"
#include "vaultik/jobs/protection.hpp"
#include "vaultik/jobs/run_log.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/jobs/update_check.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
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
    RunRecord found = run(RunKind::Check, now - day, RunStatus::Failed);
    found.errors = 3;
    history.push_back(found);  // problems found still count
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

    // A check that could not finish (the drive was unplugged part way) does not count either: the
    // full check stays due, and the record is one the alerts announce.
    history = {run(RunKind::FullCheck, now - 40 * day), run(RunKind::Check, now - 2 * day)};
    CHECK(due_check(h, history, now) == RunKind::FullCheck);
    RunRecord broken = run(RunKind::FullCheck, now - day, RunStatus::Failed);
    history.push_back(broken);
    CHECK(due_check(h, history, now) == RunKind::FullCheck);
    CHECK(check_not_finished(broken));
    CHECK_FALSE(check_completed(broken));
    CHECK_FALSE(found_damage(broken));
    RunRecord damage = broken;
    damage.errors = 1;
    CHECK(check_completed(damage));
    CHECK_FALSE(check_not_finished(damage));
    history.back() = damage;
    CHECK_FALSE(due_check(h, history, now).has_value());
    // A full check that stopped at its time limit: the weekly check is done, the full one stays due.
    history = {run(RunKind::FullCheck, now - 40 * day), run(RunKind::FullCheck, now - day, RunStatus::Partial)};
    CHECK(due_check(h, history, now) == RunKind::FullCheck);
    h.monthly = false;
    CHECK_FALSE(due_check(h, history, now).has_value());
    h.monthly = true;
    // One that found damage and also stopped at its time limit: a failure that is alerted, and
    // not a complete full check (the rest was never read).
    RunRecord damaged_part = run(RunKind::FullCheck, now - day, RunStatus::Failed);
    damaged_part.errors = 2;
    damaged_part.packs_left = 7;
    history = {run(RunKind::FullCheck, now - 40 * day), damaged_part};
    CHECK(due_check(h, history, now) == RunKind::FullCheck);
    CHECK(record_from_json(to_json(damaged_part)).packs_left == 7);
    {
        AlertSettings alerts;
        alerts.webhook_url = "https://example.com/hook";
        const auto a = alert_for_run(alerts, "Docs", damaged_part, "pc");
        REQUIRE(a.has_value());
        CHECK(a->event == "damage");
    }
    // A sampled check that failed leaves the weekly one due.
    h.monthly = false;
    history = {run(RunKind::Check, now - 8 * day), run(RunKind::Check, now - day, RunStatus::Failed)};
    CHECK(due_check(h, history, now) == RunKind::Check);
    h = HealthPolicy{};

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
    CHECK(xml.find("<Arguments>run-job docs --scheduled</Arguments>") != std::string::npos);
    CHECK(xml.find("<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>") != std::string::npos);
    CHECK(xml.find("<WakeToRun>false</WakeToRun>") != std::string::npos);
    CHECK(xml.find("<StartWhenAvailable>true</StartWhenAvailable>") != std::string::npos);
    CHECK(xml.find("<RunLevel>LeastPrivilege</RunLevel>") != std::string::npos);
    j.run_as_admin = true;
    const std::string admin = schedule_definition(j, "C:\\x");
    CHECK(admin.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos);
    CHECK(admin.find("<UserId>S-1-5-18</UserId>") != std::string::npos);  // the system account
    CHECK(admin.find("InteractiveToken") == std::string::npos);
    CHECK(admin.find("--admin --user " + vaultik::os::current_user_sid() + "</Arguments>") != std::string::npos);
    CHECK(admin.find("<SecurityDescriptor>D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;") != std::string::npos);
    CHECK(xml.find("--admin") == std::string::npos);
    j.run_as_admin = false;
    j.schedule.skip_on_battery = true;
    j.schedule.wake_computer = true;
    const std::string conditions = schedule_definition(j, "D:/x");
    CHECK(conditions.find("<DisallowStartIfOnBatteries>true</DisallowStartIfOnBatteries>") != std::string::npos);
    CHECK(conditions.find("<WakeToRun>true</WakeToRun>") != std::string::npos);
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

TEST_CASE("a job with control or text-direction characters is refused") {
    Job j = sample();
    CHECK_NOTHROW(validate(j));
    j.name = "Docs" + std::string(1, '\n') + "To: C:\\Safe";
    CHECK_THROWS(validate(j));
    j = sample();
    j.sources.push_back("C:\\Users\\\xE2\x80\xAE" "evil");  // U+202E right-to-left override
    CHECK_THROWS(validate(j));
    j = sample();
    j.before_command = std::string("echo a") + char(13) + "echo b";
    CHECK_THROWS(validate(j));
}

TEST_CASE("a webhook alert with a name that is not valid UTF-8 is still sent") {
    Alert a;
    a.event = "failed";
    a.job = "Docs";
    a.title = "Backup of Docs failed";
    a.message = "cannot read /home/a/caf\xE9.txt";  // Latin-1, not UTF-8
    const std::string body = webhook_body(a, "pc", "2026-11-11 10:00:00");
    const auto j = nlohmann::json::parse(body);
    CHECK(j.at("job") == "Docs");
    CHECK(j.at("message").get<std::string>().find("\xEF\xBF\xBD") != std::string::npos);
}

TEST_CASE("alerts: a health check that could not finish is announced, one that found damage too") {
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    RunRecord r;
    r.kind = RunKind::FullCheck;
    r.status = RunStatus::Failed;
    r.message = "the check could not run: cannot read E:\\Backup\\data\\ab: the device is not ready";
    auto a = alert_for_run(s, "Docs", r, "pc");
    REQUIRE(a.has_value());
    CHECK(a->event == "check-failed");
    CHECK(a->title.find("could not finish") != std::string::npos);
    CHECK(a->message.find("E:") == std::string::npos);  // never the run's own message or paths
    s.on_failure = false;
    CHECK_FALSE(alert_for_run(s, "Docs", r, "pc").has_value());
    s.on_failure = true;
    r.errors = 2;  // found damage
    a = alert_for_run(s, "Docs", r, "pc");
    REQUIRE(a.has_value());
    CHECK(a->event == "damage");
    r.status = RunStatus::Cancelled;  // cancelled: not news
    r.errors = 0;
    CHECK_FALSE(alert_for_run(s, "Docs", r, "pc").has_value());
    r.status = RunStatus::Success;
    CHECK_FALSE(alert_for_run(s, "Docs", r, "pc").has_value());
    s.webhook_url.clear();  // no channel set up: nothing
    r.status = RunStatus::Failed;
    CHECK_FALSE(alert_for_run(s, "Docs", r, "pc").has_value());
}

TEST_CASE("an unattended run of a job without a saved password fails at once and never asks") {
    TempStore tmp;
    fs::create_directories(tmp.path / "src");
    JobStore store(tmp.dir());
    Job j;
    j.name = "Docs";
    j.repository = (tmp.path / "repo").string();
    j.sources = {(tmp.path / "src").string()};
    j.schedule.kind = Schedule::Kind::Daily;
    store.save(j);
    REQUIRE(store.password(j.id).empty());

    std::string output;
    os::CommandOptions o;
    o.output = &output;
    o.timeout = std::chrono::seconds(60);  // a prompt would wait until this ends the test
    o.extra_environment = {{"VAULTIK_CONFIG_DIR", tmp.dir()}, {"VAULTIK_PASSWORD", ""}};
    const auto started = std::chrono::steady_clock::now();
    const int code = os::run_command({VAULTIK_TEST_CLI, "run-job", j.id, "--scheduled"}, o);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
    CHECK(code == 2);
    CHECK(output.find("not saved on this PC") != std::string::npos);
    CHECK(output.find("Repository password:") == std::string::npos);
    const std::vector<RunRecord> history = store.history(j.id);
    REQUIRE(history.size() == 1);
    CHECK(history[0].status == RunStatus::Failed);
    CHECK(history[0].reason == "password");
    CHECK_FALSE(fs::exists(tmp.path / "repo"));  // nothing was touched

    // Its alert says so, in fixed words.
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    const auto a = alert_for_run(s, j.name, history[0], "pc");
    REQUIRE(a.has_value());
    CHECK(a->event == "password");
    CHECK(a->level == Alert::Level::Critical);
    // The record survives a round trip.
    CHECK(record_from_json(to_json(history[0])).reason == "password");
}

TEST_CASE("a run whose backup drive is away waits quietly: recorded once, not alerted, caught up later") {
    // The record and how it reads.
    const RunRecord w = waiting_for_drive_record(RunKind::Backup, 500, "E:\\Backup");
    CHECK(w.status == RunStatus::Waiting);
    CHECK(std::string(to_string(w.status)) == "waiting");
    CHECK(record_from_json(to_json(w)).status == RunStatus::Waiting);
    CHECK(record_from_json(to_json(w)).reason == "drive");
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    s.on_success = true;
    CHECK_FALSE(alert_for_run(s, "Docs", w, "pc").has_value());
    RunRecord copy = waiting_for_drive_record(RunKind::Copy, 500, "F:\\Copy");
    CHECK_FALSE(alert_for_run(s, "Docs", copy, "pc").has_value());
    const SystemLogEntry e = system_log_entry(w, "Docs", "docs");
    CHECK(e.id == 104);
    CHECK(e.level == 0);

    // It is not a backup run, and it marks a missed run until a backup runs.
    RunRecord saved;
    saved.started = 100;
    saved.finished = 200;
    std::vector<RunRecord> history = {saved, w};
    REQUIRE(last_backup(history) != nullptr);
    CHECK(last_backup(history)->finished == 200);
    CHECK(missed_run_waiting(history) == &history[1]);
    history.push_back(copy);  // other kinds do not end the wait
    CHECK(missed_run_waiting(history) == &history[1]);
    saved.started = 600;
    history.push_back(saved);
    CHECK(missed_run_waiting(history) == nullptr);

    // The command line: exit code 8, one record however often it runs, never an error.
    TempStore tmp;
    JobStore store(tmp.dir());
    Job j;
    j.name = "Away";
    j.sources = {tmp.dir()};
    j.schedule.kind = Schedule::Kind::Hourly;
#ifdef _WIN32
    j.repository = "Q:\\Backups\\vaultik";
    j.repository_volume = "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\";
    j.repository_relative = "Backups\\vaultik";
#else
    j.repository = "/run/media/nobody/USB/Backups/vaultik";
    j.repository_volume = "uuid:00000000-0000-0000-0000-00000000dead";
    j.repository_relative = "Backups/vaultik";
#endif
    store.save(j);
    REQUIRE_FALSE(drive_connected(*store.find(j.id)));
    os::CommandOptions o;
    std::string output;
    o.output = &output;
    o.timeout = std::chrono::seconds(60);
    o.extra_environment = {{"VAULTIK_CONFIG_DIR", tmp.dir()}, {"VAULTIK_PASSWORD", "not used here"}};
    CHECK(os::run_command({VAULTIK_TEST_CLI, "run-job", j.id, "--scheduled"}, o) == 8);
    CHECK(output.find("waiting for the backup drive") != std::string::npos);
    CHECK(os::run_command({VAULTIK_TEST_CLI, "run-job", j.id, "--scheduled"}, o) == 8);
    const std::vector<RunRecord> recorded = store.history(j.id);
    REQUIRE(recorded.size() == 1);
    CHECK(recorded[0].status == RunStatus::Waiting);
    CHECK(missed_run_waiting(recorded) != nullptr);
}

TEST_CASE("alert settings: saved, read back, tokens sealed, only https") {
    TempStore tmp;
    AlertSettings s;
    CHECK_FALSE(load_alerts(tmp.dir()).any());
    s.ntfy_url = "http://ntfy.example.com/topic";
    CHECK_THROWS(save_alerts(s, tmp.dir()));  // tokens and messages only over https
    s.ntfy_url = "https://ntfy.example.com/my-long-topic";
    s.telegram_chat = "12345";
    s.on_success = true;
    save_alerts(s, tmp.dir());
    const AlertSettings back = load_alerts(tmp.dir());
    CHECK(back.ntfy_url == s.ntfy_url);
    CHECK(back.telegram_chat == "12345");
    CHECK(back.on_success);
    CHECK(back.any());
    set_alert_secret("telegram", "123:abc", tmp.dir());
    CHECK(alert_secret("telegram", tmp.dir()) == "123:abc");
    set_alert_secret("telegram", "", tmp.dir());
    CHECK(alert_secret("telegram", tmp.dir()).empty());
}

TEST_CASE("update check") {
    using namespace vaultik::jobs;
    CHECK(newer_version("0.7.0", "0.6.0"));
    CHECK(newer_version("v0.10.0", "0.9.9"));
    CHECK(newer_version("1.0", "0.99.99"));
    CHECK_FALSE(newer_version("0.6.0", "0.6.0"));
    CHECK_FALSE(newer_version("0.5.9", "0.6.0"));
    CHECK_FALSE(newer_version("garbage", "0.6.0"));
    const auto r = parse_release(R"({"tag_name":"v0.7.0","name":"Vaultik 0.7.0","draft":false,"prerelease":false,
        "html_url":"https://github.com/Bigisoft/Vaultik/releases/tag/v0.7.0"})");
    REQUIRE(r.has_value());
    CHECK(r->version == "0.7.0");
    CHECK(r->url == "https://github.com/Bigisoft/Vaultik/releases/tag/v0.7.0");
    CHECK_FALSE(parse_release(R"({"tag_name":"v0.8.0","prerelease":true,
        "html_url":"https://github.com/Bigisoft/Vaultik/releases/tag/v0.8.0"})"));
    CHECK_FALSE(parse_release(R"({"tag_name":"v0.8.0","draft":true,
        "html_url":"https://github.com/Bigisoft/Vaultik/releases/tag/v0.8.0"})"));
    // Only a page of Vaultik's own releases is ever opened.
    CHECK_FALSE(parse_release(R"({"tag_name":"v0.8.0","html_url":"https://evil.example/releases/"})"));
    CHECK_FALSE(parse_release(R"({"tag_name":"v0.8.0","html_url":"https://github.com/Bigisoft/Vaultik/releases/x" y"})"));
    CHECK_FALSE(parse_release(R"({"tag_name":"latest","html_url":"https://github.com/Bigisoft/Vaultik/releases/x"})"));
    CHECK_FALSE(parse_release("not json"));
    CHECK_FALSE(parse_release("[]"));
}

TEST_CASE("system log entries of runs") {
    using namespace vaultik::jobs;
    RunRecord r;
    r.files = 12;
    r.new_bytes = 3000;
    SystemLogEntry e = system_log_entry(r, "Docs", "docs");
    CHECK(e.id == 100);
    CHECK(e.level == 0);
    CHECK(e.text.find("Backup of the Vaultik job \"Docs\" (docs): success") == 0);
    CHECK(e.text.find("Files: 12") != std::string::npos);
    r.alert = "many files look encrypted";
    e = system_log_entry(r, "Docs", "docs");
    CHECK(e.id == 105);
    CHECK(e.level == 1);
    CHECK(e.text.find("signs of ransomware") != std::string::npos);
    CHECK(e.text.find("many files look encrypted") == std::string::npos);  // no messages: others read the log
    r.alert.clear();
    r.status = RunStatus::Failed;
    r.message = "the backup drive is not connected";
    r.kind = RunKind::Check;
    e = system_log_entry(r, "Docs", "docs");
    CHECK(e.id == 202);
    CHECK(e.level == 2);
    CHECK(e.text.find("Files:") == std::string::npos);
    CHECK(e.text.find("not connected") == std::string::npos);
    r.kind = RunKind::RestoreTest;
    r.status = RunStatus::Cancelled;
    CHECK(system_log_entry(r, "Docs", "docs").id == 403);
}

#ifdef _WIN32
TEST_CASE("jobs with administrator rights run as the system account") {
    using namespace vaultik;
    // Only a run as the system account works for another account.
    CHECK_THROWS(jobs::act_for_account(os::current_user_sid()));
    CHECK_FALSE(jobs::account_is_administrator("S-1-5-18"));            // not a person's account
    CHECK_FALSE(jobs::account_is_administrator("S-1-5-21-1-2-3-1001"));  // no such account
    CHECK_FALSE(jobs::account_is_administrator("S-1-5-21-1-2-3-x"));
    // The admin store's secrets are sealed with the computer's key; others with the account's.
    const std::string store = jobs::admin_store_dir("S-1-5-21-1-2-3-1001");
    CHECK(jobs::in_admin_store(os::join(os::join(store, "secrets"), "x.key")));
    CHECK_FALSE(jobs::in_admin_store(os::join(os::program_data_dir(), "Vaultik\\other\\x.key")));
    CHECK_FALSE(jobs::in_admin_store("C:\\Users\\someone\\AppData\\Local\\Vaultik\\secrets\\x.key"));
    const std::string text = "secret";
    const ByteSpan plain(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    const Bytes machine = os::protect_secret(plain, true);
    const Bytes back = os::unprotect_secret(machine);
    CHECK(std::string(back.begin(), back.end()) == text);
}
#endif

TEST_CASE("backups started by events: settings, tasks and spacing") {
    Job j = sample();
    j.id = "docs";
    CHECK(has_system_schedule(j.schedule));
    j.schedule.kind = Schedule::Kind::Manual;
    CHECK_FALSE(has_system_schedule(j.schedule));
    j.schedule.on_idle = true;
    j.schedule.on_lock = true;
    j.schedule.on_sign_out = true;
    j.schedule.event_gap_hours = 6;
    j.continuous = true;
    j.continuous_minutes = 15;
    CHECK(has_system_schedule(j.schedule));
    CHECK(describe(j.schedule) == "When idle, when locked and at sign-out");
    const Job back = job_from_json(to_json(j));
    CHECK(back.schedule.on_idle);
    CHECK(back.schedule.on_lock);
    CHECK(back.schedule.on_sign_out);
    CHECK(back.schedule.event_gap_hours == 6);
    CHECK(back.continuous);
    CHECK(back.continuous_minutes == 15);
    // Older job files have none of it.
    const Job old = job_from_json(R"({"version":1,"id":"x","name":"x","repository":"E:/b","sources":["C:/a"]})");
    CHECK_FALSE(old.schedule.on_idle);
    CHECK_FALSE(old.continuous);
    CHECK(old.schedule.event_gap_hours == 4);
    CHECK(old.continuous_minutes == 5);

    // The tray starts these from the user's session: not for administrator jobs.
    Job admin = j;
    admin.run_as_admin = true;
    CHECK_THROWS_AS(validate(admin), Error);
    admin.continuous = false;
    CHECK_THROWS_AS(validate(admin), Error);
    admin.schedule.on_sign_out = false;
    CHECK_NOTHROW(validate(admin));
    Job bad = j;
    bad.schedule.event_gap_hours = 0;
    CHECK_THROWS_AS(validate(bad), Error);
    bad = j;
    bad.continuous_minutes = 0;
    CHECK_THROWS_AS(validate(bad), Error);

    RunRecord r;
    r.trigger = "sign-out";
    CHECK(record_from_json(to_json(r)).trigger == "sign-out");
    r.trigger.clear();
    r.on_connect = true;
    const RunRecord c = record_from_json(to_json(r));
    CHECK(c.on_connect);
    CHECK(c.trigger.empty());

#ifdef _WIN32
    // Idle and lock: a second task, whose runs skip a recent backup; none without them.
    const std::string xml = events_definition(j, "C:\\Program Files\\Vaultik");
    CHECK(xml.find("<IdleTrigger>") != std::string::npos);
    CHECK(xml.find("<StateChange>SessionLock</StateChange><UserId>" + os::current_user_sid() + "</UserId>") !=
          std::string::npos);
    CHECK(xml.find("<Arguments>run-job docs --scheduled --on-event</Arguments>") != std::string::npos);
    CHECK(xml.find("<StopOnIdleEnd>false</StopOnIdleEnd>") != std::string::npos);
    CHECK(xml.find("<StartWhenAvailable>false</StartWhenAvailable>") != std::string::npos);
    CHECK(xml.find("<CalendarTrigger>") == std::string::npos);
    j.schedule.on_idle = false;
    const std::string lock = events_definition(j, "C:\\x");
    CHECK(lock.find("<IdleTrigger>") == std::string::npos);
    CHECK(lock.find("SessionLock") != std::string::npos);
    j.schedule.on_lock = false;
    CHECK(events_definition(j, "C:\\x").empty());
    // The time schedule's task is unchanged by them.
    j.schedule.kind = Schedule::Kind::Daily;
    j.schedule.on_idle = true;
    CHECK(describe(j.schedule) == "Daily at 21:30, also when idle and at sign-out");
    CHECK(schedule_definition(j, "C:\\x").find("<IdleTrigger>") == std::string::npos);
#endif

    // Not again within the gap after a backup that did not fail.
    TempStore tmp;
    JobStore store(tmp.dir());
    Job e = sample();
    e.repository = tmp.dir();
    e.schedule.event_gap_hours = 4;
    store.save(e);
    CHECK(event_backup_due(store, e));
    RunRecord done;
    done.started = os::now() - 2ull * 3600 * 10'000'000;
    done.finished = os::now() - 1ull * 3600 * 10'000'000;  // an hour ago
    done.status = RunStatus::Failed;
    store.append_history(e.id, done);
    CHECK(event_backup_due(store, e));  // a failed one does not count
    done.status = RunStatus::Partial;
    store.append_history(e.id, done);
    CHECK_FALSE(event_backup_due(store, e));
    e.schedule.event_gap_hours = 1;
    CHECK(event_backup_due(store, e));
}

TEST_CASE("backups before Windows updates: trigger, task and the update's title") {
    Job j = sample();
    j.id = "docs";
    j.schedule.kind = Schedule::Kind::Manual;
    CHECK_FALSE(has_system_schedule(j.schedule));
    j.schedule.on_update = true;
    CHECK(has_system_schedule(j.schedule));
    CHECK(describe(j.schedule) == "Before Windows updates");
    CHECK(job_from_json(to_json(j)).schedule.on_update);
    CHECK_FALSE(job_from_json(R"({"version":1,"id":"x","name":"x","repository":"E:/b","sources":["C:/a"]})").schedule.on_update);
    // A job with administrator rights may have it: its task starts it, not the tray.
    Job admin = j;
    admin.run_as_admin = true;
    CHECK_NOTHROW(validate(admin));
    RunRecord r;
    r.trigger = "update";
    CHECK(record_from_json(to_json(r)).trigger == "update");

#ifdef _WIN32
    // An event trigger on event 43 of Windows Update in the System log, in the events task.
    const std::string xml = events_definition(j, "C:\\x");
    CHECK(xml.find("<EventTrigger><Enabled>true</Enabled><Subscription>&lt;QueryList&gt;&lt;Query Id=&quot;0&quot; "
                   "Path=&quot;System&quot;&gt;&lt;Select Path=&quot;System&quot;&gt;*[System[Provider[@Name='"
                   "Microsoft-Windows-WindowsUpdateClient'] and EventID=43]]&lt;/Select&gt;") != std::string::npos);
    CHECK(xml.find("<Arguments>run-job docs --scheduled --on-event</Arguments>") != std::string::npos);
    CHECK(xml.find("<IdleTrigger>") == std::string::npos);
    j.schedule.on_update = false;
    CHECK(events_definition(j, "C:\\x").empty());
    j.schedule.on_update = true;
#endif

    // What wevtutil prints, two events (the second one escaped).
    const std::string events =
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>43</EventID></System>"
        "<EventData><Data Name='updateTitle'>9NRZT3Q9R3DL-Microsoft.WindowsAppRuntime.2</Data>"
        "<Data Name='updateGuid'>{32079cbb}</Data></EventData></Event>"
        "<Event><EventData><Data Name='updateTitle'>2026-11 Cumulative Update for Windows 11 &amp; more (KB5070000)"
        "</Data></EventData></Event>";
    const std::vector<std::string> titles = update_titles(events);
    REQUIRE(titles.size() == 2);
    CHECK(titles[0] == "9NRZT3Q9R3DL-Microsoft.WindowsAppRuntime.2");
    CHECK(titles[1] == "2026-11 Cumulative Update for Windows 11 & more (KB5070000)");
    CHECK(update_titles("").empty());
    CHECK(update_titles("<Data Name='updateTitle'>cut off").empty());

    CHECK(routine_update(titles[0]));
    CHECK_FALSE(routine_update(titles[1]));
    CHECK(routine_update("Security Intelligence Update for Microsoft Defender Antivirus - KB2267602 (Version 1.463.226.0) - Current Channel (Broad)"));
    CHECK(routine_update("9NQ58D7GHB2X-DevX-Cipher.Detect-It-Easy"));
    CHECK_FALSE(routine_update("Intel Corporation - Display - 32.0.101.6881"));
    CHECK_FALSE(routine_update("2026-11 Security Update (KB5070001)"));

    // Updates only: a real update runs, a routine one is skipped, an unknown one runs.
    CHECK(events_trigger(j.schedule, {titles[1]}) == "update");
    CHECK(events_trigger(j.schedule, {titles[0]}).empty());
    CHECK(events_trigger(j.schedule, {}) == "update");
    // With idle or lock too: a routine update or none means idle or lock started it.
    j.schedule.on_lock = true;
    CHECK(events_trigger(j.schedule, {titles[0], titles[1]}) == "update");
    CHECK(events_trigger(j.schedule, {titles[0]}) == "event");
    CHECK(events_trigger(j.schedule, {}) == "event");
    j.schedule.on_update = false;
    CHECK(events_trigger(j.schedule, {titles[1]}) == "event");

    // The marker the tray holds the restart up for, while the run's process is alive.
    TempStore tmp;
    CHECK(running_update_backups(tmp.dir()).empty());
    mark_update_run(tmp.dir(), "docs");
    const std::vector<std::uint32_t> running = running_update_backups(tmp.dir());
    REQUIRE(running.size() == 1);
    CHECK(running[0] == os::process_id());
    clear_update_run(tmp.dir(), "docs");
    CHECK(running_update_backups(tmp.dir()).empty());
    // One left by a process that ended is removed.
    os::make_dirs(os::join(tmp.dir(), "running"));
    const std::string dead = "4294967290";
    os::write_file_atomic(os::join(os::join(tmp.dir(), "running"), "update-old"),
                          ByteSpan(reinterpret_cast<const std::uint8_t*>(dead.data()), dead.size()));
    CHECK(running_update_backups(tmp.dir()).empty());
    CHECK_FALSE(os::exists(os::join(os::join(tmp.dir(), "running"), "update-old")));
#ifdef _WIN32
    // One written before the process with its id started (left from before a restart, the id
    // reused) holds nothing up either.
    mark_update_run(tmp.dir(), "docs");
    const std::string marker = os::join(os::join(tmp.dir(), "running"), "update-docs");
    std::filesystem::last_write_time(std::filesystem::path(std::u8string(marker.begin(), marker.end())),
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 365));
    CHECK(running_update_backups(tmp.dir()).empty());
    CHECK_FALSE(os::exists(marker));
#endif
}

TEST_CASE("continuous backups ignore changes the backup leaves out") {
#ifdef _WIN32
    // Windows paths: case does not matter.
    const std::vector<std::string> ex = {"*.tmp", "node_modules", "C:\\Users\\me\\AppData\\Local\\Temp"};
    CHECK(vaultik::path_excluded(ex, "C:\\Users\\me\\Documents\\a.tmp"));
    CHECK(vaultik::path_excluded(ex, "C:\\work\\node_modules\\left-pad\\index.js"));
    CHECK(vaultik::path_excluded(ex, "c:\\users\\ME\\AppData\\Local\\Temp\\x\\y.txt"));
    CHECK_FALSE(vaultik::path_excluded(ex, "C:\\Users\\me\\Documents\\report.docx"));
    CHECK_FALSE(vaultik::path_excluded(ex, "C:\\Users\\me\\AppData\\Local\\Temporary.txt"));
    CHECK_FALSE(vaultik::path_excluded({}, "C:\\a.tmp"));
#else
    // Linux paths: case matters.
    const std::vector<std::string> ex = {"*.tmp", "node_modules", "/home/me/.cache"};
    CHECK(vaultik::path_excluded(ex, "/home/me/Documents/a.tmp"));
    CHECK(vaultik::path_excluded(ex, "/work/node_modules/left-pad/index.js"));
    CHECK(vaultik::path_excluded(ex, "/home/me/.cache/x/y.txt"));
    CHECK_FALSE(vaultik::path_excluded(ex, "/home/me/.Cache/x/y.txt"));
    CHECK_FALSE(vaultik::path_excluded(ex, "/home/me/Documents/report.docx"));
    CHECK_FALSE(vaultik::path_excluded(ex, "/home/me/.cached.txt"));
    CHECK_FALSE(vaultik::path_excluded({}, "/a.tmp"));
#endif
}

TEST_CASE("a paused job keeps its schedule but nothing starts it on its own") {
    Job j = sample();
    j.id = "docs";
    j.schedule.on_idle = true;
    CHECK(has_system_schedule(j.schedule));
    j.schedule.paused = true;
    // No task or timer while paused, but what it would have once resumed is still known.
    CHECK_FALSE(has_system_schedule(j.schedule));
    CHECK(has_system_schedule_unpaused(j.schedule));
    CHECK(describe(j.schedule) == "Paused (Mon, Wed at 21:30, also when idle)");
    Schedule manual;
    manual.paused = true;
    CHECK(describe(manual) == "Paused (Manual)");

    // Saved and read back; older files without the setting load as not paused.
    const Job back = job_from_json(to_json(j));
    CHECK(back.schedule.paused);
    CHECK(back.schedule.kind == Schedule::Kind::Weekly);
    CHECK(back.schedule.weekdays == 0b0000101);
    CHECK(nlohmann::json::parse(to_json(j)).at("schedule").at("paused") == true);
    const Job old = job_from_json(R"({"version":1,"id":"x","name":"x","repository":"E:/b","sources":["C:/a"],)"
                                  R"("schedule":{"kind":"daily","hour":18}})");
    CHECK_FALSE(old.schedule.paused);
    CHECK(has_system_schedule(old.schedule));

    // Neither events (idle, lock, sign-out, updates) nor connecting the drive start it.
    TempStore t;
    const JobStore store(t.dir());
    Job running = sample();
    running.id = "docs";
    CHECK(event_backup_due(store, running));
    CHECK_FALSE(event_backup_due(store, j));
    j.backup_on_connect = true;
    CHECK_FALSE(connect_backup_due(store, j));
}

TEST_CASE("pausing a job: when its schedule cannot be changed, it is saved back as it was") {
    TempStore dir;
    const JobStore store(dir.dir());
    Job j = sample();
    j.schedule.kind = Schedule::Kind::Daily;
    store.save(j);
    REQUIRE_FALSE(j.schedule.paused);
    // The approval was declined.
    CHECK_THROWS_AS(set_paused(store, j, true, [](const Job&) { throw Error("the approval was declined"); }), Error);
    CHECK_FALSE(j.schedule.paused);
    CHECK_FALSE(store.find(j.id)->schedule.paused);
    // It worked: saved paused, and the schedule applied to the paused job.
    bool applied_paused = false;
    set_paused(store, j, true, [&](const Job& x) { applied_paused = x.schedule.paused && store.find(x.id)->schedule.paused; });
    CHECK(applied_paused);
    CHECK(store.find(j.id)->schedule.paused);
    // Resuming that fails leaves it paused.
    CHECK_THROWS_AS(set_paused(store, j, false, [](const Job&) { throw Error("Task Scheduler refused"); }), Error);
    CHECK(store.find(j.id)->schedule.paused);
    CHECK(j.schedule.paused);
}

TEST_CASE("a drive away for days is alerted as missing, once a day, also for the second copy") {
    constexpr std::uint64_t day = 864'000'000'000ull;
    const std::uint64_t now = 1000 * day;
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    Job j = sample();
    j.schedule.kind = Schedule::Kind::Daily;
    CHECK(drive_wait_alert_days(j) == 3);

    // A job that never saved a version: only the first record of the wait is kept.
    std::vector<RunRecord> history = {waiting_for_drive_record(RunKind::Backup, now - 2 * day, "E:\\Backup")};
    CHECK(waiting_since(history, RunKind::Backup) == now - 2 * day);
    CHECK_FALSE(drive_missing_alert(s, j, history, RunKind::Backup, now, "pc").has_value());  // 2 days: quiet
    const auto a = drive_missing_alert(s, j, history, RunKind::Backup, now + day, "pc");
    REQUIRE(a.has_value());
    CHECK(a->event == "drive-missing");
    CHECK(a->level == Alert::Level::Critical);
    CHECK(a->title.find("missing for 3 days") != std::string::npos);
    CHECK(a->message.find("E:") == std::string::npos);  // no paths leave the computer
    CHECK(alert_repeats(a->event));
    // Not for a paused job, nor without "on failure", nor once a backup ran after the wait.
    Job paused = j;
    paused.schedule.paused = true;
    CHECK_FALSE(drive_missing_alert(s, paused, history, RunKind::Backup, now + day, "pc").has_value());
    AlertSettings quiet = s;
    quiet.on_failure = false;
    CHECK_FALSE(drive_missing_alert(quiet, j, history, RunKind::Backup, now + day, "pc").has_value());
    RunRecord saved;
    saved.started = now - day;
    saved.finished = now - day + 10;
    std::vector<RunRecord> back = history;
    back.push_back(saved);
    CHECK_FALSE(waiting_since(back, RunKind::Backup).has_value());
    CHECK_FALSE(drive_missing_alert(s, j, back, RunKind::Backup, now + 5 * day, "pc").has_value());

    // The second copy waits on its own: the backups ran, its drive stayed away.
    std::vector<RunRecord> copy = {saved, waiting_for_drive_record(RunKind::Copy, now - 4 * day, "F:\\Copy"), saved};
    CHECK_FALSE(waiting_since(copy, RunKind::Backup).has_value());
    CHECK(waiting_since(copy, RunKind::Copy) == now - 4 * day);
    const auto c = drive_missing_alert(s, j, copy, RunKind::Copy, now, "pc");
    REQUIRE(c.has_value());
    CHECK(c->title.find("second copy") != std::string::npos);

    // A weekly job waits a week before it counts as missing.
    j.schedule.kind = Schedule::Kind::Weekly;
    j.schedule.weekdays = 1;  // Mondays
    CHECK(drive_wait_alert_days(j) == 8);
    CHECK_FALSE(drive_missing_alert(s, j, history, RunKind::Backup, now + 3 * day, "pc").has_value());

    // At most once a day per job and kind; the others are not held up by it.
    TempStore tmp;
    fs::create_directories(tmp.path);
    CHECK(alert_once_a_day(tmp.dir(), "docs", "drive-missing", now));
    CHECK_FALSE(alert_once_a_day(tmp.dir(), "docs", "drive-missing", now + day / 2));
    CHECK(alert_once_a_day(tmp.dir(), "docs", "password", now + day / 2));
    CHECK(alert_once_a_day(tmp.dir(), "photos", "drive-missing", now + day / 2));
    CHECK(alert_once_a_day(tmp.dir(), "docs", "drive-missing", now + day));
    CHECK_FALSE(alert_once_a_day(tmp.dir(), "docs", "drive-missing", now + day + 1));
}

TEST_CASE("alerts that repeat while their cause lasts go out once a day: password, unfinished check") {
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    const RunRecord pw = password_not_saved_record(5);
    const auto a = alert_for_run(s, "Docs", pw, "pc");
    REQUIRE(a.has_value());
    CHECK(alert_repeats(a->event));
    RunRecord check;
    check.kind = RunKind::Check;
    check.status = RunStatus::Failed;
    const auto c = alert_for_run(s, "Docs", check, "pc");
    REQUIRE(c.has_value());
    CHECK(c->event == "check-failed");
    CHECK(alert_repeats(c->event));
    // Damage, a failed backup and ransomware signs are news every time.
    CHECK_FALSE(alert_repeats("damage"));
    CHECK_FALSE(alert_repeats("failed"));
    CHECK_FALSE(alert_repeats("ransomware"));

    constexpr std::uint64_t day = 864'000'000'000ull;
    TempStore tmp;
    fs::create_directories(tmp.path);
    int sent = 0;
    for (int run = 0; run < 24; ++run)  // hourly runs, all failing the same way
        sent += alert_once_a_day(tmp.dir(), "docs", "password", 100 * day + static_cast<std::uint64_t>(run) * (day / 24));
    CHECK(sent == 1);
    CHECK(alert_once_a_day(tmp.dir(), "docs", "password", 101 * day));
    // A clock set back does not silence it for good.
    CHECK(alert_once_a_day(tmp.dir(), "docs", "check-failed", 200 * day));
    CHECK(alert_once_a_day(tmp.dir(), "docs", "check-failed", 50 * day));
}

TEST_CASE("an alert that repeats is noted as sent only once a channel took it") {
    constexpr std::uint64_t day = 864'000'000'000ull;
    TempStore tmp;
    fs::create_directories(tmp.path);
    // Every channel failed (no network yet): not noted, so the next run sends it again.
    int tries = 0;
    const auto fails = [&] {
        ++tries;
        return false;
    };
    CHECK_FALSE(alert_once_a_day(tmp.dir(), "docs", "drive-missing", 10 * day, fails));
    CHECK_FALSE(alert_once_a_day(tmp.dir(), "docs", "drive-missing", 10 * day + day / 24, fails));
    CHECK(tries == 2);
    // One went through: noted, and none other that day.
    const auto works = [&] {
        ++tries;
        return true;
    };
    CHECK(alert_once_a_day(tmp.dir(), "docs", "drive-missing", 10 * day + day / 12, works));
    CHECK(tries == 3);
    CHECK_FALSE(alert_once_a_day(tmp.dir(), "docs", "drive-missing", 10 * day + day / 6, works));
    CHECK(tries == 3);  // not even tried
    CHECK(alert_once_a_day(tmp.dir(), "docs", "drive-missing", 11 * day + day / 12, works));

    AlertSettings s;
    CHECK(alert_channels(s) == 0);
    s.ntfy_url = "https://ntfy.sh/topic";
    s.webhook_url = "https://example.com/hook";
    CHECK(alert_channels(s) == 2);
}

TEST_CASE("the second copy's missing drive is alerted for a job that backs up every hour") {
    constexpr std::uint64_t day = 864'000'000'000ull;
    constexpr std::uint64_t hour = day / 24;
    TempStore tmp;
    JobStore store(tmp.dir());
    Job j;
    j.name = "Docs";
    j.repository = (tmp.path / "repo").string();
    j.sources = {(tmp.path / "src").string()};
    j.schedule.kind = Schedule::Kind::Hourly;
    store.save(j);
    const std::uint64_t start = 1000 * day;
    // The copy's drive went away; since then one record of the wait, and four days of hourly
    // backups (far more than 50 records), each with no second record of the wait.
    RunRecord copied;
    copied.kind = RunKind::Copy;
    copied.started = copied.finished = start - hour;
    store.append_history(j.id, copied);
    store.append_history(j.id, waiting_for_drive_record(RunKind::Copy, start, "F:\\Copy"));
    for (int h = 1; h <= 4 * 24; ++h) {
        RunRecord b;
        b.started = start + static_cast<std::uint64_t>(h) * hour;
        b.finished = b.started + 1000;
        store.append_history(j.id, b);
    }
    const std::uint64_t now = start + 4 * day + hour;
    CHECK(waiting_since(combined_history(store, j.id, 50), RunKind::Copy) == std::nullopt);  // out of the window
    const std::vector<RunRecord> copies = combined_history_of(store, j.id, RunKind::Copy, 50);
    REQUIRE(copies.size() == 2);
    CHECK(copies.back().status == RunStatus::Waiting);
    CHECK(waiting_since(copies, RunKind::Copy) == start);
    AlertSettings s;
    s.webhook_url = "https://example.com/hook";
    const auto a = drive_missing_alert(s, *store.find(j.id), copies, RunKind::Copy, now, "pc");
    REQUIRE(a.has_value());
    CHECK(a->title.find("second copy") != std::string::npos);
    CHECK(a->title.find("4 days") != std::string::npos);
    // The other kinds: the newest records of each, oldest first, at most the limit.
    const std::vector<RunRecord> backups = combined_history_of(store, j.id, RunKind::Backup, 10);
    REQUIRE(backups.size() == 10);
    CHECK(backups.back().started == start + 4 * day);
    CHECK(backups.front().started < backups.back().started);
    CHECK(combined_history_of(store, j.id, RunKind::FullCheck, 10).empty());
    CHECK(combined_history_of(store, "nobody", RunKind::Copy, 10).empty());
}

TEST_CASE("drive lock: two runs for the same drive take turns, other drives do not wait") {
    TempStore tmp;
    const std::string dir = tmp.dir();
    fs::create_directories(tmp.path);
    CHECK(drive_lock_key("s3:https://s3.example.com/bucket").empty());  // cloud storage: no drive
    CHECK(drive_lock_key("E:/Backup", "\\\\?\\Volume{1}\\") == "\\\\?\\Volume{1}\\");
    CHECK_FALSE(drive_lock_key(dir).empty());
    CHECK(drive_lock_key(dir) == drive_lock_key(dir));

    const std::string key = "test-drive-" + tmp.path.filename().string();
    std::atomic<int> state{0};  // 1: the first run holds the drive, 2: it let go
    std::thread first([&] {
        DriveLock lock(key, dir, std::chrono::milliseconds(5000));
        state = lock.held() ? 1 : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        state = 2;
    });
    while (state == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(state == 1);
    {
        // Another drive: at once.
        DriveLock other(key + "-other", dir, std::chrono::milliseconds(0));
        CHECK(other.held());
        CHECK_FALSE(other.waited());
    }
    // The same drive with a short wait: busy.
    bool busy = false;
    int told = 0;
    try {
        DriveLock quick(key, dir, std::chrono::milliseconds(100), [&] { ++told; });
    } catch (const DriveBusy&) {
        busy = true;
    }
    CHECK(busy);
    CHECK(told == 1);
    // Cancelled while it waits.
    bool cancelled = false;
    try {
        DriveLock stop(key, dir, std::chrono::milliseconds(5000), {}, [] { return true; });
    } catch (const DriveBusy&) {
    } catch (const Error&) {
        cancelled = true;
    }
    CHECK(cancelled);
    // A long enough wait: it gets the drive once the first run is done, never while it runs.
    told = 0;
    {
        DriveLock second(key, dir, std::chrono::milliseconds(10000), [&] { ++told; });
        CHECK(second.held());
        CHECK(second.waited());
        CHECK(state == 2);
        CHECK(told == 1);
    }
    first.join();
    // Free again.
    DriveLock again(key, dir, std::chrono::milliseconds(0));
    CHECK(again.held());
    // No key: no lock, no wait.
    DriveLock none("", dir, std::chrono::milliseconds(0));
    CHECK_FALSE(none.held());
}

namespace {

// Holds the drive lock of `key` on another thread (a Windows mutex lets its own thread in again)
// until release() or the end of the scope.
struct HeldDrive {
    std::atomic<int> state{0};  // 1: held, 2: may let go, -1: could not take it
    std::pair<std::string, std::string> env;
    std::thread thread;
    HeldDrive(const std::string& key, const std::string& dir) {
        thread = std::thread([this, key, dir] {
            DriveLock lock(key, dir, std::chrono::milliseconds(0));
            env = lock.child_environment();
            state = lock.held() ? 1 : -1;
            while (state == 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (state == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void release() {
        if (state == 1) state = 2;
        if (thread.joinable()) thread.join();
    }
    ~HeldDrive() { release(); }
};

void init_test_repo(const fs::path& dir) {
    LocalBackend backend(dir.string(), true);
    InitOptions options;
    options.kdf_iterations = 1000;
    Repository::init(backend, "test password", options);
}

}  // namespace

TEST_CASE("drive lock: a command started by the run holding the drive does not wait for it") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-nested-" + tmp.path.filename().string();
    HeldDrive parent(key, tmp.dir());
    REQUIRE(parent.state == 1);
    CHECK(parent.env.first == kHeldDrivesVariable);
    CHECK_FALSE(parent.env.second.empty());

    // Without the marker: busy.
    CHECK_THROWS_AS(DriveLock(key, tmp.dir(), std::chrono::milliseconds(50)), DriveBusy);
    // With it (as the parent's before and after commands get it): at once, without taking it.
    os::set_env(kHeldDrivesVariable, "other:1;" + parent.env.second);
    {
        DriveLock nested(key, tmp.dir(), std::chrono::milliseconds(50));
        CHECK(nested.inherited());
        CHECK_FALSE(nested.held());
        CHECK_FALSE(nested.waited());
        // And passed on to what it starts in turn.
        CHECK(nested.child_environment().second.find(parent.env.second) != std::string::npos);
        // Another drive still locks as usual.
        DriveLock other(key + "-other", tmp.dir(), std::chrono::milliseconds(0));
        CHECK(other.held());
        CHECK_FALSE(other.inherited());
    }
    // The run that passed it on has ended, but what its command started still runs: named, yet
    // free, so taken as usual (and held for good measure against the next run).
    parent.release();
    {
        DriveLock orphan(key, tmp.dir(), std::chrono::milliseconds(50));
        CHECK(orphan.held());
        CHECK_FALSE(orphan.inherited());
        HeldDrive next(key, tmp.dir());
        CHECK(next.state == -1);  // the next run waits for it
    }
    // An eject started by such a command does not take a held drive for its own.
    {
        HeldDrive again(key, tmp.dir());
        REQUIRE(again.state == 1);
        os::set_env(kHeldDrivesVariable, again.env.second);
        CHECK_THROWS_AS(DriveLock(key, tmp.dir(), std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject), DriveBusy);
        DriveLock nested(key, tmp.dir(), std::chrono::milliseconds(0));
        CHECK(nested.inherited());
    }
    os::set_env(kHeldDrivesVariable, "");

    // The command line: a prune of the same drive in the run's after command finishes at once.
    fs::create_directories(tmp.path / "repo");
    init_test_repo(tmp.path / "repo");
    HeldDrive run(drive_lock_key((tmp.path / "repo").string()), os::join(tmp.dir(), "locks"));
    REQUIRE(run.state == 1);
    std::string output;
    os::CommandOptions o;
    o.output = &output;
    o.timeout = std::chrono::seconds(60);
    o.extra_environment = {{"VAULTIK_CONFIG_DIR", tmp.dir()}, {"VAULTIK_PASSWORD", "test password"}, run.env};
    const auto started = std::chrono::steady_clock::now();
    CHECK(os::run_command({VAULTIK_TEST_CLI, "prune", "-r", (tmp.path / "repo").string(), "--dry-run"}, o) == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
    CHECK(output.find("Waiting") == std::string::npos);
}

// Another process holding a drive lock or claim, for the tests below: this test program again,
// running only this case, which does what VAULTIK_TEST_HOLD says ("lock", "claim", or "claimed",
// which asks drive_claimed) for VAULTIK_TEST_HOLD_KEY with the settings folder
// VAULTIK_TEST_HOLD_DIR, says "HOLD:held" or "HOLD:free" on its output, and keeps what it holds
// until the file VAULTIK_TEST_HOLD_UNTIL is there. POSIX: VAULTIK_TEST_HOLD_UID makes a helper
// started as root go on as that account first.
TEST_CASE("drive lock helper: holds what another test asks for in a process of its own") {
    const std::string what = os::get_env("VAULTIK_TEST_HOLD");
    if (what.empty()) return;  // only as such a helper
#ifndef _WIN32
    if (const std::string uid = os::get_env("VAULTIK_TEST_HOLD_UID"); !uid.empty() && ::geteuid() == 0) {
        const auto id = static_cast<uid_t>(std::stoul(uid));
        REQUIRE(::setgroups(0, nullptr) == 0);
        REQUIRE(::setgid(static_cast<gid_t>(id)) == 0);
        REQUIRE(::setuid(id) == 0);
    }
#endif
    const std::string key = os::get_env("VAULTIK_TEST_HOLD_KEY");
    const std::string dir = os::get_env("VAULTIK_TEST_HOLD_DIR");
    std::unique_ptr<DriveLock> lock;
    std::unique_ptr<DriveClaim> claim;
    bool held = false;
    if (what == "lock") {
        try {
            lock = std::make_unique<DriveLock>(key, dir, std::chrono::milliseconds(0));
            held = lock->held();
        } catch (const DriveBusy&) {
        }
    } else if (what == "claim") {
        claim = std::make_unique<DriveClaim>(key, dir);
        held = claim->held();
    } else if (what == "claimed") {
        held = drive_claimed(key, dir);
    }
    std::cout << (held ? "HOLD:held" : "HOLD:free") << std::endl;
    const fs::path until = fs::u8path(os::get_env("VAULTIK_TEST_HOLD_UNTIL"));
    const auto stop = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!fs::exists(until) && std::chrono::steady_clock::now() < stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

namespace {

// Starts that helper and waits for its answer; it lets go when this goes away (or release()).
struct OtherProcess {
    std::unique_ptr<os::PipedProcess> process;
    fs::path until;
    std::string said;
    OtherProcess(const std::string& what, const std::string& key, const std::string& dir, const fs::path& folder,
                 const std::string& uid = {}) {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        until = folder / ("release-" + to_hex(rnd));
        std::vector<std::pair<std::string, std::string>> env = {
            {"VAULTIK_TEST_HOLD", what},
            {"VAULTIK_TEST_HOLD_KEY", key},
            {"VAULTIK_TEST_HOLD_DIR", dir},
            {"VAULTIK_TEST_HOLD_UNTIL", until.string()}};
        if (!uid.empty()) env.push_back({"VAULTIK_TEST_HOLD_UID", uid});
        process = std::make_unique<os::PipedProcess>(
            std::vector<std::string>{os::executable_path(), "--test-case=drive lock helper*", "--no-intro=true",
                                     "--no-version=true"},
            os::PipedProcess::Direction::FromProgram, env);
        // Its answer, one byte at a time (a read waits for as much as it asks for).
        std::uint8_t c = 0;
        while (said.find("HOLD:held") == std::string::npos && said.find("HOLD:free") == std::string::npos &&
               process->read(&c, 1) == 1)
            said.push_back(static_cast<char>(c));
    }
    bool held() const { return said.find("HOLD:held") != std::string::npos; }
    int release() {
        if (!process) return -1;
        std::ofstream(until) << "go";
        std::uint8_t buf[256];
        while (process->read(buf, 1) == 1) {
        }
        const int code = process->wait();
        process.reset();
        return code;
    }
    ~OtherProcess() {
        try {
            release();
        } catch (...) {
        }
    }
};

#ifdef _WIN32
// A mutex of the drive's as a run with administrator rights leaves it: made by the system
// account, which only it may open (here: one only the system account may open, made by the test).
HANDLE system_only_mutex(const std::wstring& name) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)", SDDL_REVISION_1, &sd, nullptr));
    sa.lpSecurityDescriptor = sd;
    HANDLE m = CreateMutexW(&sa, TRUE, name.c_str());
    LocalFree(sd);
    REQUIRE(m != nullptr);
    return m;
}

std::wstring wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

// A mutex named `name` with the rights `sddl`, not taken; null when it cannot be made.
HANDLE make_mutex(const std::wstring& name, const std::string& sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wide(sddl).c_str(), SDDL_REVISION_1, &sd, nullptr))
        return nullptr;
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    HANDLE m = CreateMutexExW(&sa, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL);
    LocalFree(sd);
    return m;
}
#endif

}  // namespace

#ifdef _WIN32
// A process with the user's own rights (os::run_with_user_rights, as the app starts the runs of a
// job without administrator rights), for the tests below: this test program again, running only
// this case, which does what VAULTIK_TEST_USER_DO says for the drive VAULTIK_TEST_USER_KEY:
// "lock" a run's DriveLock (waiting VAULTIK_TEST_USER_WAIT milliseconds), "eject" an eject's,
// "claim" a DriveClaim, "claimed" asks drive_claimed, "make" makes the mutex VAULTIK_TEST_USER_NAME
// as Vaultik makes it without taking it, and "crash" takes a run's DriveLock and ends at once,
// without letting go. Its answer ("held", "unlocked", "busy" or "free", with " not-shared" when
// the lock could not be shared) goes to the file VAULTIK_TEST_USER_OUT; it keeps what it holds
// until the file VAULTIK_TEST_USER_UNTIL is there.
TEST_CASE("drive lock user helper: does what another test asks for with the user's own rights") {
    const std::string what = os::get_env("VAULTIK_TEST_USER_DO");
    if (what.empty()) return;  // only as such a helper
    const fs::path out = fs::u8path(os::get_env("VAULTIK_TEST_USER_OUT"));
    std::ofstream(out.string() + ".started") << "started";
    const std::string key = os::get_env("VAULTIK_TEST_USER_KEY");
    const std::string dir = os::get_env("VAULTIK_TEST_USER_DIR");
    const std::chrono::milliseconds wait(std::stoll("0" + os::get_env("VAULTIK_TEST_USER_WAIT")));
    std::unique_ptr<DriveLock> lock;
    std::unique_ptr<DriveClaim> claim;
    HANDLE made = nullptr;
    std::string answer;
    try {
        if (what == "lock" || what == "eject" || what == "crash") {
            lock = std::make_unique<DriveLock>(key, dir, wait, std::function<void()>{}, std::function<bool()>{},
                                               what == "eject" ? DriveLockMode::eject : DriveLockMode::run);
            answer = lock->held() ? "held" : "unlocked";
            if (lock->not_shared()) answer += " not-shared";
        } else if (what == "claim") {
            claim = std::make_unique<DriveClaim>(key, dir);
            answer = claim->held() ? "held" : "free";
        } else if (what == "claimed") {
            answer = drive_claimed(key, dir) ? "held" : "free";
        } else if (what == "make") {
            made = make_mutex(wide(os::get_env("VAULTIK_TEST_USER_NAME")), drive_lock_sddl(os::current_user_sid()));
            answer = made != nullptr ? "held" : "free";
        }
    } catch (const DriveBusy&) {
        answer = "busy";
    }
    std::ofstream(out.string() + ".part") << answer;
    fs::rename(out.string() + ".part", out);
    if (what == "crash") TerminateProcess(GetCurrentProcess(), 3);  // as a run that crashed
    const fs::path until = fs::u8path(os::get_env("VAULTIK_TEST_USER_UNTIL"));
    const auto stop = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!fs::exists(until) && std::chrono::steady_clock::now() < stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (made != nullptr) CloseHandle(made);
}

namespace {

// Starts that helper with the user's own rights; it lets go when this goes away (or release()).
struct UserProcess {
    static constexpr int kRunning = -1000;
    fs::path out;
    fs::path until;
    std::atomic<int> code{kRunning};
    std::thread thread;
    UserProcess(const std::string& what, const std::string& key, const fs::path& folder,
                std::chrono::milliseconds wait = {}, const std::wstring& name = {}) {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        out = folder / ("answer-" + to_hex(rnd));
        until = folder / ("release-" + to_hex(rnd));
        const std::vector<std::pair<std::string, std::string>> env = {
            {"VAULTIK_TEST_USER_DO", what},
            {"VAULTIK_TEST_USER_KEY", key},
            {"VAULTIK_TEST_USER_DIR", (folder / "user-settings").string()},
            {"VAULTIK_TEST_USER_WAIT", std::to_string(wait.count())},
            {"VAULTIK_TEST_USER_NAME", std::string(name.begin(), name.end())},
            {"VAULTIK_TEST_USER_OUT", out.string()},
            {"VAULTIK_TEST_USER_UNTIL", until.string()}};
        for (const auto& [n, v] : env) os::set_env(n, v);
        const std::string exe = os::executable_path();
        const std::string line =
            "\"" + exe + "\" \"--test-case=drive lock user helper*\" --no-intro=true --no-version=true";
        thread = std::thread([this, exe, line] {
            int c = -2;
            try {
                c = os::run_with_user_rights(exe, line);
            } catch (const std::exception&) {
            }
            code = c;
        });
        // Started (with this environment), before another one is set up.
        const auto stop = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (!fs::exists(out.string() + ".started") && code == kRunning && std::chrono::steady_clock::now() < stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (const auto& [n, v] : env) os::set_env(n, "");
    }
    bool answered() const { return fs::exists(out); }
    // Its answer, once it has one (empty when it ended without).
    std::string answer() const {
        const auto stop = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (!answered() && code == kRunning && std::chrono::steady_clock::now() < stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ifstream in(out);
        std::string text;
        std::getline(in, text);
        return text;
    }
    int release() {
        if (!thread.joinable()) return code;
        std::ofstream(until) << "go";
        thread.join();
        return code;
    }
    ~UserProcess() {
        try {
            release();
        } catch (...) {
        }
    }
};

// Only this process's lock account for as long as it lives (see set_drive_lock_account).
struct LockAccount {
    explicit LockAccount(const std::string& sid) { set_drive_lock_account(sid); }
    ~LockAccount() { set_drive_lock_account({}); }
    LockAccount(const LockAccount&) = delete;
    LockAccount& operator=(const LockAccount&) = delete;
};

// The rights of the drive's mutex `name` as Windows keeps them: protected, the system account and
// the administrators, then `account` (when not empty) with 0x120001 only.
void check_mutex_rights(const std::wstring& name, const std::string& account) {
    HANDLE m = OpenMutexW(READ_CONTROL, FALSE, name.c_str());
    REQUIRE(m != nullptr);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(GetSecurityInfo(m, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) ==
            ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(sd, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    REQUIRE(dacl != nullptr);
    CHECK(dacl->AceCount == (account.empty() ? 2u : 3u));
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        REQUIRE(GetAce(dacl, i, &ace));
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        CHECK(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        LPWSTR text = nullptr;
        REQUIRE(ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &text));
        const std::wstring sid(text);
        LocalFree(text);
        if (i == 2) {
            CHECK(sid == wide(account));
            CHECK(allowed->Mask == 0x120001u);
        } else {
            CHECK(sid == (i == 0 ? L"S-1-5-18" : L"S-1-5-32-544"));
        }
    }
    LocalFree(sd);
    CloseHandle(m);
}

}  // namespace
#endif

TEST_CASE("drive lock: one place per drive, whatever settings folder a run uses") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-place-" + tmp.path.filename().string();
    // A run with administrator rights (the admin-only store) and the user's own prune would hold
    // it from processes of their own, with settings folders of their own.
    {
        OtherProcess admin("lock", key, os::join(tmp.dir(), "admin-store"), tmp.path);
        REQUIRE(admin.held());
        CHECK_THROWS_AS(DriveLock(key, os::join(tmp.dir(), "user-settings"), std::chrono::milliseconds(100)), DriveBusy);
        CHECK(admin.release() == 0);
    }
    DriveLock user(key, os::join(tmp.dir(), "user-settings"), std::chrono::milliseconds(0));
    CHECK(user.held());
    // And the other way round.
    OtherProcess admin("lock", key, os::join(tmp.dir(), "admin-store"), tmp.path);
    CHECK_FALSE(admin.held());
    // The same thread again (a Windows mutex would let it in): busy too.
    CHECK_THROWS_AS(DriveLock(key, os::join(tmp.dir(), "user-settings"), std::chrono::milliseconds(50)), DriveBusy);
}

TEST_CASE("drive claims: any command with the drive open keeps it from being ejected") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    Job j = sample();
    j.repository = (tmp.path / "repo").string();
    j.repository_volume.clear();
    const std::string key = drive_lock_key(j);
    const std::string dir = os::join(tmp.dir(), "locks");
    CHECK_FALSE(drive_claimed(key, dir));
    {
        // Two commands at once in other processes (a restore and a verify, say): claims go together.
        OtherProcess one("claim", key, os::join(tmp.dir(), "one"), tmp.path);
        OtherProcess two("claim", key, os::join(tmp.dir(), "two"), tmp.path);
        REQUIRE(one.held());
        REQUIRE(two.held());
        CHECK(drive_claimed(key, dir));
        // The drive lock is free (no backup runs), yet the eject is refused, the drive never touched.
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
        CHECK(one.release() == 0);
        CHECK(drive_claimed(key, dir));
    }
    CHECK_FALSE(drive_claimed(key, dir));
    {
        // Another thread of this process, and this very thread.
        std::atomic<int> state{0};
        std::thread reader([&] {
            DriveClaim claim(key, dir);
            state = claim.held() ? 1 : -1;
            while (state == 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (state == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(state == 1);
        CHECK(drive_claimed(key, dir));
        state = 2;
        reader.join();
        CHECK_FALSE(drive_claimed(key, dir));
        DriveClaim mine(key, dir);
        REQUIRE(mine.held());
        CHECK(drive_claimed(key, dir));
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
    }
    CHECK_FALSE(drive_claimed(key, dir));
    DriveClaim none("", dir);
    CHECK_FALSE(none.held());
}

TEST_CASE("eject: a command that starts while the drive is ejected waits for the eject") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-eject-" + tmp.path.filename().string();
    const std::string dir = os::join(tmp.dir(), "locks");
    std::unique_ptr<OtherProcess> command;
    {
        std::optional<EjectHold> hold;
        hold.emplace(key, dir);
        REQUIRE_FALSE(hold->busy());
        // Another eject meanwhile: busy, not a second one under way.
        CHECK(drive_claimed(key, dir));
        // A command starting now (another process) waits until the eject is done (the drive is
        // gone then, so the command finds nothing to open).
        std::atomic<bool> answered{false};
        std::thread start([&] {
            command = std::make_unique<OtherProcess>("claim", key, os::join(tmp.dir(), "other"), tmp.path);
            answered = true;
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        CHECK_FALSE(answered.load());  // still waiting for the eject
        hold.reset();  // done: it gets its claim at once
        start.join();
    }
    REQUIRE(command);
    CHECK(command->held());
    // With its claim held, no eject.
    EjectHold refused(key, dir);
    CHECK(refused.busy());
}

TEST_CASE("drive lock: the drive's mutexes get the same rights whoever makes them") {
    const std::string user = "S-1-5-21-1-2-3-1001";
    // The account's own run (elevated or not) and a run as the system account for it (--user).
    CHECK(drive_lock_account_for(user, "") == user);
    CHECK(drive_lock_account_for("S-1-5-18", user) == user);
    CHECK(drive_lock_sddl(drive_lock_account_for(user, "")) == drive_lock_sddl(drive_lock_account_for("S-1-5-18", user)));
    CHECK(drive_lock_sddl(user) == "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x120001;;;S-1-5-21-1-2-3-1001)");
    // The system account without --user acts for nobody: no account's entry.
    CHECK(drive_lock_account_for("S-1-5-18", "").empty());
    CHECK(drive_lock_sddl(drive_lock_account_for("S-1-5-18", "")) == "D:P(A;;GA;;;SY)(A;;GA;;;BA)");
}

TEST_CASE("drive lock: a job with administrator rights goes on when the wait ends, other runs give up") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-admin-wait-" + tmp.path.filename().string();
    const std::string dir = os::join(tmp.dir(), "settings");
    OtherProcess other("lock", key, dir, tmp.path);
    REQUIRE(other.held());
    CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(300)), DriveBusy);
    {
        int told = 0;
        DriveLock admin(key, dir, std::chrono::milliseconds(300), [&] { ++told; }, {}, DriveLockMode::admin_run);
        CHECK_FALSE(admin.held());
        CHECK(admin.waited());
        CHECK(admin.gave_up_waiting());
        CHECK(told == 1);
        CHECK(admin.child_environment().first.empty());
    }
    // An eject still counts the drive as busy.
    CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject), DriveBusy);
    CHECK(other.release() == 0);
    DriveLock admin(key, dir, std::chrono::milliseconds(300), {}, {}, DriveLockMode::admin_run);
    CHECK(admin.held());
    CHECK_FALSE(admin.waited());
    CHECK_FALSE(admin.gave_up_waiting());
}

#ifdef _WIN32
TEST_CASE("drive lock: an eject sees runs and commands with administrator rights it may not open") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    Job j = sample();
    j.repository = (tmp.path / "repo").string();
    j.repository_volume.clear();
    const std::string key = drive_lock_key(j);
    const std::string hash = drive_lock_name(key);
    const std::string dir = os::join(tmp.dir(), "locks");
    {
        // A command with administrator rights has the drive open: its claim cannot be opened here.
        HANDLE claim = system_only_mutex(L"Global\\Vaultik-use-" + wide(hash) + L"-0");
        CHECK(drive_claimed(key, dir));
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
        // The user's own command still gets a claim (another slot).
        DriveClaim mine(key, dir);
        CHECK(mine.held());
        ReleaseMutex(claim);
        CloseHandle(claim);
    }
    CHECK_FALSE(drive_claimed(key, dir));
    {
        // A run with administrator rights holds the drive: an eject fails closed, a run goes on.
        HANDLE run = system_only_mutex(L"Global\\Vaultik-drive-" + wide(hash));
        CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject), DriveBusy);
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
        DriveLock user_run(key, dir, std::chrono::milliseconds(0));
        CHECK_FALSE(user_run.held());
        CHECK_FALSE(user_run.waited());
        CHECK(user_run.not_shared());
        ReleaseMutex(run);
        CloseHandle(run);
    }
    DriveLock after(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject);
    CHECK(after.held());
}

TEST_CASE("drive lock: jobs with administrator rights and the account's own runs take turns (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-admin-turn-" + tmp.path.filename().string();
    const std::wstring name = L"Global\\Vaultik-drive-" + wide(drive_lock_name(key));
    const std::string me = os::current_user_sid();
    const std::string dir = os::join(tmp.dir(), "admin-store");
    const LockAccount account(me);  // as the system account acting for this account (--user)
    {
        // A job with administrator rights holds the drive: the account's own run (with its own
        // rights) is busy at once, and takes its turn once the job is done.
        auto admin = std::make_unique<DriveLock>(key, dir, std::chrono::milliseconds(0), std::function<void()>{},
                                                 std::function<bool()>{}, DriveLockMode::admin_run);
        REQUIRE(admin->held());
        check_mutex_rights(name, me);
        {
            UserProcess quick("lock", key, tmp.path);
            CHECK(quick.answer() == "busy");
        }
        UserProcess run("lock", key, tmp.path, std::chrono::milliseconds(60000));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        CHECK_FALSE(run.answered());
        admin.reset();
        CHECK(run.answer() == "held");
        CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(200)), DriveBusy);
        CHECK(run.release() == 0);
    }
    {
        // The account's run made the lock: a job with administrator rights waits for it (it is not
        // passed by as one of another account), then takes its turn.
        UserProcess run("lock", key, tmp.path);
        REQUIRE(run.answer() == "held");
        check_mutex_rights(name, me);
        CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(200)), DriveBusy);
        std::thread later([&] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            run.release();
        });
        int told = 0;
        DriveLock admin(key, dir, std::chrono::milliseconds(60000), [&] { ++told; }, {}, DriveLockMode::admin_run);
        later.join();
        CHECK(admin.held());
        CHECK(admin.waited());
        CHECK(told == 1);
        CHECK_FALSE(admin.gave_up_waiting());
        CHECK_FALSE(admin.not_shared());
    }
}

TEST_CASE("drive claims: seen across administrator rights both ways (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    Job j = sample();
    j.repository = (tmp.path / "repo").string();
    j.repository_volume.clear();
    const std::string key = drive_lock_key(j);
    const std::string dir = os::join(tmp.dir(), "locks");
    const LockAccount account(os::current_user_sid());
    {
        // A command of the account's own: an eject with administrator rights sees it.
        UserProcess command("claim", key, tmp.path);
        REQUIRE(command.answer() == "held");
        CHECK(drive_claimed(key, dir));
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
        CHECK(command.release() == 0);
    }
    CHECK_FALSE(drive_claimed(key, dir));
    {
        // A command with administrator rights: the account's eject sees it, and its own commands
        // still get a claim.
        DriveClaim mine(key, dir);
        REQUIRE(mine.held());
        UserProcess eject("claimed", key, tmp.path);
        CHECK(eject.answer() == "held");
        UserProcess command("claim", key, tmp.path);
        CHECK(command.answer() == "held");
    }
    UserProcess eject("claimed", key, tmp.path);
    CHECK(eject.answer() == "free");
}

TEST_CASE("drive lock: a lock another account owns is passed by, an eject counts it as busy (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    Job j = sample();
    j.repository = (tmp.path / "repo").string();
    j.repository_volume.clear();
    const std::string key = drive_lock_key(j);
    const std::string hash = drive_lock_name(key);
    const std::string dir = os::join(tmp.dir(), "locks");
    // Made (not taken) by this account's own process with its own rights, so owned by it.
    UserProcess lock("make", key, tmp.path, {}, L"Global\\Vaultik-drive-" + wide(hash));
    REQUIRE(lock.answer() == "held");
    UserProcess slot("make", key, tmp.path, {}, L"Global\\Vaultik-use-" + wide(hash) + L"-0");
    REQUIRE(slot.answer() == "held");
    {
        // Seen by runs acting for another account: owned by nobody to trust.
        const LockAccount other("S-1-5-21-1-2-3-4242");
        DriveLock run(key, dir, std::chrono::milliseconds(0));
        CHECK_FALSE(run.held());
        CHECK_FALSE(run.waited());
        CHECK(run.not_shared());
        CHECK_THROWS_AS(DriveLock(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject), DriveBusy);
        CHECK(drive_claimed(key, dir));
        CHECK(eject_backup_drive(j, tmp.dir()).find("is using this drive") != std::string::npos);
    }
    {
        // Seen by runs acting for the account that made them: free, and used.
        const LockAccount owner(os::current_user_sid());
        CHECK_FALSE(drive_claimed(key, dir));
        DriveLock eject(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::eject);
        CHECK(eject.held());
        CHECK_FALSE(eject.not_shared());
    }
}

TEST_CASE("drive lock: one the account may not open is passed by, its eject counts it as busy (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-denied-" + tmp.path.filename().string();
    const std::string hash = drive_lock_name(key);
    // As a run with administrator rights left them before runs shared their turn (not taken).
    HANDLE lock = make_mutex(L"Global\\Vaultik-drive-" + wide(hash), "D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    REQUIRE(lock != nullptr);
    HANDLE slot = make_mutex(L"Global\\Vaultik-use-" + wide(hash) + L"-0", "D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    REQUIRE(slot != nullptr);
    {
        UserProcess run("lock", key, tmp.path);
        CHECK(run.answer() == "unlocked not-shared");
        UserProcess eject("eject", key, tmp.path);
        CHECK(eject.answer() == "busy");
        UserProcess claimed("claimed", key, tmp.path);
        CHECK(claimed.answer() == "held");
    }
    // An administrator's own run uses it.
    DriveLock admin(key, os::join(tmp.dir(), "admin-store"), std::chrono::milliseconds(0));
    CHECK(admin.held());
    CloseHandle(slot);
    CloseHandle(lock);
}

TEST_CASE("drive lock: a run that ended without letting go leaves the turn to the next at once (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-abandoned-" + tmp.path.filename().string();
    const std::string me = os::current_user_sid();
    const LockAccount account(me);
    // Kept open here, so the mutex outlives the run that crashed.
    HANDLE keep = make_mutex(L"Global\\Vaultik-drive-" + wide(drive_lock_name(key)), drive_lock_sddl(me));
    REQUIRE(keep != nullptr);
    {
        UserProcess crashed("crash", key, tmp.path);
        CHECK(crashed.answer() == "held");
        CHECK(crashed.release() == 3);
    }
    DriveLock next(key, os::join(tmp.dir(), "admin-store"), std::chrono::milliseconds(0));
    CHECK(next.held());
    CHECK_FALSE(next.waited());
    CloseHandle(keep);
}

TEST_CASE("drive lock: a name taken by another kind of object is passed by (Windows)") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    const std::string key = "test-event-" + tmp.path.filename().string();
    const std::wstring hash = wide(drive_lock_name(key));
    const std::string dir = os::join(tmp.dir(), "locks");
    HANDLE events[3] = {CreateEventW(nullptr, TRUE, FALSE, (L"Global\\Vaultik-drive-" + hash).c_str()),
                        CreateEventW(nullptr, TRUE, FALSE, (L"Global\\Vaultik-use-" + hash + L"-0").c_str()),
                        CreateEventW(nullptr, TRUE, FALSE, (L"Global\\Vaultik-eject-" + hash).c_str())};
    for (HANDLE e : events) REQUIRE(e != nullptr);
    {
        DriveLock run(key, dir, std::chrono::milliseconds(0));
        CHECK_FALSE(run.held());
        CHECK_FALSE(run.waited());
        CHECK_FALSE(run.not_shared());
        DriveLock admin(key, dir, std::chrono::milliseconds(0), {}, {}, DriveLockMode::admin_run);
        CHECK_FALSE(admin.held());
        DriveClaim claim(key, dir);
        CHECK(claim.held());  // another slot
        CHECK(drive_claimed(key, dir));
    }
    CHECK_FALSE(drive_claimed(key, dir));
    for (HANDLE e : events) CloseHandle(e);
}
#else
TEST_CASE("drive lock: the files in /run/lock are shared, opened without being made again") {
    if (::access("/run/lock", W_OK) != 0) {
        MESSAGE("/run/lock is not writable here: runs of one account share the lock through its settings folder");
        return;
    }
    TempStore tmp;
    fs::create_directories(tmp.path);
    // One that is there already (left by an earlier run, readable by all): opened as it is,
    // without O_CREAT, and made readable by this account alone.
    const std::string key = "test-open-" + tmp.path.filename().string();
    const fs::path file = fs::path("/run/lock") / ("vaultik-drive-" + drive_lock_name(key) + ".lock");
    std::ofstream(file).close();
    ::chmod(file.c_str(), 0644);
    {
        DriveLock lock(key, os::join(tmp.dir(), "a"), std::chrono::milliseconds(0));
        CHECK(lock.held());
        struct stat st{};
        REQUIRE(::stat(file.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (::geteuid() == 0 ? 0644 : 0600));
        CHECK_FALSE(fs::exists(fs::path(tmp.path) / "a"));  // nothing in the settings folder
        OtherProcess other("lock", key, os::join(tmp.dir(), "b"), tmp.path);
        CHECK_FALSE(other.held());
    }
    fs::remove(file);

    // Several runs starting at once, each with its own settings folder, make it once: exactly one
    // of them holds the drive.
    const std::string race = "test-race-" + tmp.path.filename().string();
    {
        std::vector<std::unique_ptr<OtherProcess>> runs(4);
        std::vector<std::thread> starts;
        for (std::size_t i = 0; i < runs.size(); ++i)
            starts.emplace_back([&, i] {
                runs[i] = std::make_unique<OtherProcess>("lock", race, os::join(tmp.dir(), "run" + std::to_string(i)), tmp.path);
            });
        for (std::thread& t : starts) t.join();
        int held = 0;
        for (const auto& r : runs) held += r->held() ? 1 : 0;
        CHECK(held == 1);
        for (std::size_t i = 0; i < runs.size(); ++i) CHECK_FALSE(fs::exists(tmp.path / ("run" + std::to_string(i))));
    }
    fs::remove(fs::path("/run/lock") / ("vaultik-drive-" + drive_lock_name(race) + ".lock"));

    // Something else in its place (a pipe, which an open would wait on for good): never waited
    // on, the settings folder is used instead.
    const std::string piped = "test-fifo-" + tmp.path.filename().string();
    const fs::path fifo = fs::path("/run/lock") / ("vaultik-drive-" + drive_lock_name(piped) + ".lock");
    REQUIRE(::mkfifo(fifo.c_str(), 0644) == 0);
    {
        DriveLock lock(piped, os::join(tmp.dir(), "c"), std::chrono::milliseconds(0));
        CHECK(lock.held());
        if (::geteuid() != 0) CHECK(fs::exists(tmp.path / "c"));
    }
    std::error_code ec;
    fs::remove(fifo, ec);
}

TEST_CASE("drive lock: root and another account share the drive's turn (Linux, as root)") {
    if (::geteuid() != 0 || ::access("/run/lock", W_OK) != 0) {
        MESSAGE("skipped: needs root (runs as root and as nobody)");
        return;
    }
    TempStore tmp;
    fs::create_directories(tmp.path);
    fs::permissions(tmp.path, fs::perms::all);  // nobody looks for its release file here
    const std::string key = "test-accounts-" + tmp.path.filename().string();
    const std::string name = drive_lock_name(key);
    const fs::path file = fs::path("/run/lock") / ("vaultik-drive-" + name + ".lock");
    {
        // The other account made the file first: root opens it as it is (never with O_CREAT,
        // which fs.protected_regular refuses even to root) and waits for its turn.
        OtherProcess user("lock", key, os::join(tmp.dir(), "nobody"), tmp.path, "65534");
        REQUIRE(user.held());
        struct stat st{};
        REQUIRE(::stat(file.c_str(), &st) == 0);
        CHECK(st.st_uid == 65534);
        CHECK_THROWS_AS(DriveLock(key, os::join(tmp.dir(), "root"), std::chrono::milliseconds(300)), DriveBusy);
        CHECK(user.release() == 0);
    }
    {
        // Root holds it: the other account waits for root, and sees its claims.
        DriveLock run(key, os::join(tmp.dir(), "root"), std::chrono::milliseconds(0));
        REQUIRE(run.held());
        OtherProcess user("lock", key, os::join(tmp.dir(), "nobody"), tmp.path, "65534");
        CHECK_FALSE(user.held());
        DriveClaim claim(key, os::join(tmp.dir(), "root"));
        REQUIRE(claim.held());
        OtherProcess eject("claimed", key, os::join(tmp.dir(), "nobody"), tmp.path, "65534");
        CHECK(eject.held());
    }
    fs::remove(file);
    fs::remove(fs::path("/run/lock") / ("vaultik-use-" + name + ".lock"));
    fs::remove(fs::path("/run/lock") / ("vaultik-drive-" + name + ".root.lock"));
}
#endif

TEST_CASE("a run queued behind a run of the same job exits without a second backup") {
    TempStore tmp;
    fs::create_directories(tmp.path / "src");
    std::ofstream(tmp.path / "src" / "file.txt") << "content";
    fs::create_directories(tmp.path / "repo");
    init_test_repo(tmp.path / "repo");
    JobStore store(tmp.dir());
    Job j;
    j.name = "Docs";
    j.repository = (tmp.path / "repo").string();
    j.sources = {(tmp.path / "src").string()};
    j.schedule.kind = Schedule::Kind::Daily;
    j.health.weekly = j.health.monthly = false;
    j.health.restore_test = false;
    store.save(j);

    // Another run of the job holds the drive, and saves a version meanwhile.
    HeldDrive other(drive_lock_key(*store.find(j.id)), os::join(tmp.dir(), "locks"));
    REQUIRE(other.state == 1);
    RunRecord saved;
    saved.started = os::now() - 10;
    saved.finished = os::now() + 600'000'000ull;  // ends after the queued run started
    store.append_history(j.id, saved);

    // It starts, finds the drive busy and says that it waits; only then is the drive let go.
    os::PipedProcess queued({VAULTIK_TEST_CLI, "run-job", j.id, "--scheduled"}, os::PipedProcess::Direction::FromProgram,
                            {{"VAULTIK_CONFIG_DIR", tmp.dir()}, {"VAULTIK_PASSWORD", "test password"}});
    std::string output;
    std::uint8_t c = 0;
    while (output.find("Waiting") == std::string::npos && queued.read(&c, 1) == 1) output.push_back(static_cast<char>(c));
    CHECK(output.find("Waiting") != std::string::npos);
    other.release();
    std::uint8_t buf[4096];
    for (std::size_t n; (n = queued.read(buf, sizeof buf)) > 0;) output.append(reinterpret_cast<const char*>(buf), n);
    const int code = queued.wait();
    CAPTURE(output);
    CAPTURE(queued.messages());
    CHECK(code == 0);
    CHECK(output.find("while this one waited") != std::string::npos);
    CHECK(store.history(j.id).size() == 1);  // no second record, no second backup
    // Nor a log of its own that no record names.
    std::error_code ec;
    const fs::path logs = tmp.path / "logs" / j.id;
    CHECK((!fs::exists(logs) || fs::is_empty(logs, ec)));
    auto repo = Repository::open(std::make_unique<LocalBackend>((tmp.path / "repo").string()), "test password");
    CHECK(repo->list_snapshot_ids().empty());

    // Without such a version, a queued run goes on as usual.
    std::vector<RunRecord> none;
    CHECK_FALSE(saved_since(none, 5));
    RunRecord failed = saved;
    failed.status = RunStatus::Failed;
    CHECK_FALSE(saved_since({failed}, saved.started));
    CHECK(saved_since({saved}, saved.started));
    CHECK_FALSE(saved_since({saved}, saved.finished + 1));
}

TEST_CASE("what is still unprotected: each gap is named, and goes away once fixed") {
    constexpr std::uint64_t day = 864'000'000'000ull;
    const std::uint64_t now = 1000 * day;
    Job j = sample();
    j.repository = "s3:https://s3.example.com/bucket";  // drives aside for now
    const auto ids = [](const std::vector<ProtectionGap>& gaps) {
        std::vector<std::string> out;
        for (const ProtectionGap& g : gaps) out.push_back(g.id);
        return out;
    };
    const auto has = [](const std::vector<std::string>& v, const char* id) {
        return std::find(v.begin(), v.end(), id) != v.end();
    };
    ProtectionFacts facts;
    RunRecord backup;
    backup.started = now - day;
    backup.finished = now - day;
    std::vector<RunRecord> history = {backup};
    auto gaps = ids(protection_gaps(j, {j}, history, facts, now));
    CHECK(has(gaps, "password"));
    CHECK(has(gaps, "second-copy"));
    CHECK(has(gaps, "kit"));
    CHECK(has(gaps, "test-restore"));
    CHECK(has(gaps, "check-overdue"));  // backed up, never checked
    CHECK_FALSE(has(gaps, "paused"));
    for (const ProtectionGap& g : protection_gaps(j, {j}, history, facts, now)) {
        CHECK_FALSE(g.text.empty());
        CHECK_FALSE(g.fix.empty());
    }

    // Fixed one by one.
    facts.password_saved = true;
    facts.kit_saved = true;
    j.second_copy.repository = "s3:https://s3.example.com/copy";
    RunRecord check;
    check.kind = RunKind::Check;
    check.finished = now - day;
    RunRecord test;
    test.kind = RunKind::RestoreTest;
    test.finished = now - day;
    history = {backup, check, test};
    CHECK(protection_gaps(j, {j}, history, facts, now).empty());

    // A paused schedule, a failed or overdue check, checks turned off.
    j.schedule.paused = true;
    CHECK(ids(protection_gaps(j, {j}, history, facts, now)) == std::vector<std::string>{"paused"});
    j.schedule.paused = false;
    history[1].status = RunStatus::Failed;  // could not finish
    CHECK(ids(protection_gaps(j, {j}, history, facts, now)) == std::vector<std::string>{"check-failed"});
    history[1].status = RunStatus::Success;
    CHECK(ids(protection_gaps(j, {j}, history, facts, now + 20 * day)) == std::vector<std::string>{"check-overdue"});
    j.health.weekly = j.health.monthly = false;
    CHECK(ids(protection_gaps(j, {j}, history, facts, now)) == std::vector<std::string>{"checks-off"});
    j.health = HealthPolicy{};
    // Files only in the cloud that the last backup left out: named also once the job downloads
    // them, until a backup did.
    history[0].cloud_only = 3;
    CHECK(ids(protection_gaps(j, {j}, history, facts, now)) == std::vector<std::string>{"cloud-only"});
    j.download_cloud_files = true;
    CHECK(ids(protection_gaps(j, {j}, history, facts, now)) == std::vector<std::string>{"cloud-only"});
    history[0].cloud_only = 0;
    CHECK(protection_gaps(j, {j}, history, facts, now).empty());
    j.download_cloud_files = false;
    // A manual job needs no saved password.
    facts.password_saved = false;
    j.schedule.kind = Schedule::Kind::Manual;
    CHECK(protection_gaps(j, {j}, history, facts, now).empty());

    // Every job on one drive without a second copy.
    TempStore tmp;
    fs::create_directories(tmp.path / "a");
    Job a = sample();
    a.repository = (tmp.path / "a").string();
    a.sources = {(tmp.path / "a").string()};
    Job b = a;
    b.name = "Other";
    facts.password_saved = true;
    auto local = ids(protection_gaps(a, {a, b}, {}, facts, now));
    CHECK(has(local, "one-drive"));
    CHECK(has(local, "same-drive"));  // its files are on the backup's drive
    b.second_copy.repository = "s3:https://s3.example.com/copy";
    CHECK_FALSE(has(ids(protection_gaps(a, {a, b}, {}, facts, now)), "one-drive"));

    // The recovery kit marker, however the destination is typed.
    CHECK_FALSE(recovery_kit_noted(tmp.dir(), a.repository));
    note_recovery_kit(tmp.dir(), a.repository);
    CHECK(recovery_kit_noted(tmp.dir(), a.repository));
    CHECK(recovery_kit_noted(tmp.dir(), a.repository + "/"));
    CHECK_FALSE(recovery_kit_noted(tmp.dir(), (tmp.path / "other").string()));
}

TEST_CASE("eject: refused while a run uses the drive, and the per-drive setting") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    Job j = sample();
    j.repository = (tmp.path / "repo").string();
    j.repository_volume.clear();
    // A run (another thread, as a Windows mutex lets its own thread in again) holds the drive:
    // nothing is ejected, the drive is never touched.
    {
        std::atomic<int> state{0};  // 1: held, 2: may let go
        std::thread run([&] {
            const DriveLock lock(drive_lock_key(j), os::join(tmp.dir(), "locks"), std::chrono::milliseconds(0));
            state = lock.held() ? 1 : -1;
            while (state == 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (state == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (state == 1) {
            const std::string failed = eject_backup_drive(j, tmp.dir());
            CHECK(failed.find("is using this drive") != std::string::npos);
        }
        const bool held = state == 1;
        state = 2;
        run.join();
        REQUIRE(held);
    }
    // Cloud storage has no drive to eject.
    Job cloud = sample();
    cloud.repository = "s3:https://s3.example.com/bucket";
    CHECK(backup_drive_root(cloud).empty());
    CHECK_FALSE(eject_backup_drive(cloud, tmp.dir()).empty());
    // What is not a removable drive is refused before anything is done to it.
#ifdef _WIN32
    CHECK_FALSE(os::eject_drive("no drive").empty());
#else
    CHECK_FALSE(os::eject_drive("/").empty());
#endif

    // "Eject when done", per drive.
    CHECK_FALSE(eject_when_done(tmp.dir(), "uuid:1"));
    set_eject_when_done(tmp.dir(), "uuid:1", true);
    CHECK(eject_when_done(tmp.dir(), "uuid:1"));
    CHECK_FALSE(eject_when_done(tmp.dir(), "uuid:2"));
    set_eject_when_done(tmp.dir(), "uuid:1", false);
    CHECK_FALSE(eject_when_done(tmp.dir(), "uuid:1"));
    CHECK_THROWS(set_eject_when_done(tmp.dir(), "", true));
}

TEST_CASE("run log: every line up to its cap, the newest logs of a job kept") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    std::string first;
    {
        RunLog log(tmp.dir(), "docs", 100, 2000);
        first = log.path();
        for (int i = 0; i < 10; ++i) log.line("warning: skipped C:\\file" + std::to_string(i) + ".txt: in use");
    }
    const auto read_text = [](const fs::path& p) {
        std::ifstream in(p);  // closed again at once: Windows deletes no file that is open
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string text = read_text(fs::u8path(first));
    CHECK(text.find("file0.txt") != std::string::npos);
    CHECK(text.find("file9.txt") != std::string::npos);
    {
        RunLog log(tmp.dir(), "docs", 200, 2000);
        for (int i = 0; i < 500; ++i) log.line("warning: skipped a file with a long enough name " + std::to_string(i));
    }
    const auto size_of = [](const fs::path& p) { return fs::file_size(p); };
    const fs::path second = tmp.path / "logs" / "docs" / "00000000000000000200.log";
    REQUIRE(fs::exists(second));
    CHECK(size_of(second) <= 2000 + 200);  // the cap, plus the line that says it was cut short
    const std::string text2 = read_text(second);
    CHECK(text2.find("cut short") != std::string::npos);

    // Only the newest are kept: starting a run makes room for it.
    for (std::uint64_t t = 300; t < 300 + kRunLogsKept + 5; ++t) {
        RunLog log(tmp.dir(), "docs", t);
        log.line("backup: success");
    }
    std::size_t count = 0;
    for (const auto& e : fs::directory_iterator(tmp.path / "logs" / "docs")) count += e.path().extension() == ".log";
    CHECK(count == kRunLogsKept);
    CHECK_FALSE(fs::exists(first));
    CHECK_FALSE(fs::exists(second));
    // Other jobs keep theirs.
    { RunLog other(tmp.dir(), "photos", 1); other.line("x"); }
    prune_run_logs(tmp.dir(), "photos", 0);
    CHECK_FALSE(fs::exists(tmp.path / "logs" / "photos" / "00000000000000000001.log"));
    // The record names it.
    RunRecord r;
    r.log = first;
    CHECK(record_from_json(to_json(r)).log == first);
}

TEST_CASE("run log: a run that ends with nothing to record leaves no log behind") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    RunLog log(tmp.dir(), "docs", 300, 100);
    for (int i = 0; i < 20; ++i) log.line("warning: something " + std::to_string(i));  // past the cap: written
    log.flush();
    REQUIRE(fs::exists(fs::u8path(log.path())));
    log.discard();
    CHECK_FALSE(fs::exists(fs::u8path(log.path())));
    log.line("warning: after the end");
    log.flush();
    CHECK_FALSE(fs::exists(fs::u8path(log.path())));
}

TEST_CASE("run log paths: only a log right in the job's own logs folder is opened") {
    TempStore tmp;
    fs::create_directories(tmp.path);
    std::string path;
    {
        RunLog log(tmp.dir(), "docs", 100);
        log.line("backup: success");
        path = log.path();
    }
    CHECK(is_run_log_of(tmp.dir(), "docs", path));
    CHECK_FALSE(is_run_log_of(tmp.dir(), "photos", path));  // another job's folder
    CHECK_FALSE(is_run_log_of(os::join(tmp.dir(), "other"), "docs", path));
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", path + ".missing"));
    CHECK_FALSE(is_run_log_of(tmp.dir(), "", path));
    CHECK_FALSE(is_run_log_of(tmp.dir(), "..", path));
    // Not a .log, or not right in the folder, or reached through "..": refused.
    const fs::path folder = tmp.path / "logs" / "docs";
    std::ofstream(folder / "run.exe") << "MZ";
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (folder / "run.exe").string()));
    fs::create_directories(folder / "sub");
    std::ofstream(folder / "sub" / "deeper.log") << "x";
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (folder / "sub" / "deeper.log").string()));
    std::ofstream(tmp.path / "elsewhere.log") << "x";
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (tmp.path / "elsewhere.log").string()));
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (folder / ".." / ".." / "elsewhere.log").string()));
    CHECK(is_run_log_of(tmp.dir(), "docs", (folder / "sub" / ".." / fs::path(path).filename()).string()));
    fs::create_directories(folder / "dir.log");
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (folder / "dir.log").string()));
#ifndef _WIN32
    // A link in the folder to a file elsewhere: what it leads to counts.
    fs::create_symlink(tmp.path / "elsewhere.log", folder / "link.log");
    CHECK_FALSE(is_run_log_of(tmp.dir(), "docs", (folder / "link.log").string()));
#endif
}

TEST_CASE("drive note: one calm line for a drive without a journal") {
    CHECK(drive_note("exFAT").find("exFAT: always eject it before unplugging") != std::string::npos);
    CHECK(drive_note("exfat").find("reformat it to NTFS") != std::string::npos);
    CHECK_FALSE(drive_note("FAT32").empty());
    CHECK_FALSE(drive_note("vfat").empty());
    CHECK(drive_note("NTFS").empty());
    CHECK(drive_note("ext4").empty());
    CHECK(drive_note("").empty());
    TempStore tmp;
    fs::create_directories(tmp.path);
    CHECK_FALSE(os::file_system_of(tmp.dir()).empty());  // whatever the test machine uses
}

TEST_CASE("shared health checks: one completed check of a repository counts for every job using it") {
    TempStore tmp;
    fs::create_directories(tmp.path / "repo");
    fs::create_directories(tmp.path / "other");
    fs::create_directories(tmp.path / "src");
    JobStore store(tmp.dir());
    const auto make = [&](const std::string& name, const fs::path& repo) {
        Job j;
        j.name = name;
        j.repository = repo.string();
        j.sources = {(tmp.path / "src").string()};
        store.save(j);
        return j;
    };
    const Job a = make("Documents", tmp.path / "repo");
    Job b = make("Photos", tmp.path / "repo");
    const Job c = make("Music", tmp.path / "other");
    CHECK(same_repository(a, b));
    CHECK_FALSE(same_repository(a, c));
    b.repository += "/";  // another spelling of the same folder
    CHECK(same_repository(a, b));

    const std::uint64_t now = os::now();
    RunRecord full;
    full.kind = RunKind::FullCheck;
    full.started = now - 1000;
    full.finished = now - 500;
    store.append_history(a.id, full);

    HealthPolicy h;  // weekly and monthly
    CHECK_FALSE(due_check(h, history_with_shared_checks(store, a), now).has_value());
    CHECK_FALSE(due_check(h, history_with_shared_checks(store, b), now).has_value());  // a's check counts
    CHECK(due_check(h, history_with_shared_checks(store, c), now) == RunKind::FullCheck);
    CHECK(due_check(h, store.history(b.id), now) == RunKind::FullCheck);  // on its own it would be due
    // Only checks are shared, never another job's backups.
    RunRecord backup;
    backup.started = now;
    backup.finished = now;
    store.append_history(a.id, backup);
    CHECK(last_backup(history_with_shared_checks(store, b)) == nullptr);
    // A check that could not finish counts for nobody.
    RunRecord broken = full;
    broken.status = RunStatus::Failed;
    store.append_history(c.id, broken);
    CHECK(due_check(h, history_with_shared_checks(store, c), now) == RunKind::FullCheck);

    // The check of the repository is shared, the check of each job's own files is not: b's files
    // were never compared, so b's silent damage check is due although its repository check is not.
    CHECK(rot_check_due(h, store.history(a.id), now) == false);
    CHECK(rot_check_due(h, store.history(b.id), now));
    RunRecord files;
    files.kind = RunKind::Check;
    files.rot_only = true;
    files.started = now - 100;
    files.finished = now - 50;
    store.append_history(b.id, files);
    CHECK(record_from_json(to_json(files)).rot_only);
    CHECK_FALSE(rot_check_due(h, store.history(b.id), now));
    CHECK(rot_check_due(h, store.history(b.id), now + 8 * 864'000'000'000ull));
    // It never counts as a check of the repository, for b or for the others.
    CHECK(due_check(h, store.history(b.id), now) == RunKind::FullCheck);
    CHECK(due_check(h, history_with_shared_checks(store, c), now) == RunKind::FullCheck);
    for (const RunRecord& r : history_with_shared_checks(store, a)) CHECK_FALSE(r.rot_only);
    HealthPolicy no_rot;
    no_rot.rot_check = false;
    CHECK_FALSE(rot_check_due(no_rot, {}, now));
}

TEST_CASE("the files-only check runs only when the job's own schedule calls for a check") {
    constexpr std::uint64_t day = 864'000'000'000ull;
    const std::uint64_t now = 1000 * day;
    const auto check = [](RunKind kind, std::uint64_t at, bool rot_only = false) {
        RunRecord r;
        r.kind = kind;
        r.started = at - 100;
        r.finished = at;
        r.rot_only = rot_only;
        return r;
    };
    // Another job backing up to the same place did the full check 2 days ago.
    const RunRecord others = check(RunKind::FullCheck, now - 2 * day);

    // Only the monthly check on: between two of them nothing is due on its own, so no weekly
    // files-only check with its "checked by another job" message either.
    HealthPolicy monthly;
    monthly.weekly = false;
    const RunRecord own_full = check(RunKind::FullCheck, now - 10 * day);
    CHECK_FALSE(due_check(monthly, {own_full}, now).has_value());
    CHECK_FALSE(files_check_due(monthly, {own_full}, {own_full, others}, now));
    // Its own check is a month old, another job's is recent: the files-only check stands in for
    // it, once a month.
    const RunRecord old_full = check(RunKind::FullCheck, now - 40 * day);
    CHECK(due_check(monthly, {old_full}, now) == RunKind::FullCheck);
    CHECK(files_check_due(monthly, {old_full}, {old_full, others}, now));
    const RunRecord files = check(RunKind::Check, now - 8 * day, true);
    CHECK_FALSE(files_check_due(monthly, {old_full, files}, {old_full, others}, now));
    CHECK(files_check_due(monthly, {old_full, files}, {old_full, others}, now + 23 * day));
    // The shared history is due too: the repository check itself runs instead.
    CHECK_FALSE(files_check_due(monthly, {old_full}, {old_full}, now));

    // Weekly and monthly on: the files-only check every week while another job checks.
    HealthPolicy both;
    CHECK(files_check_due(both, {old_full, files}, {old_full, others}, now));
    CHECK_FALSE(files_check_due(both, {old_full, check(RunKind::Check, now - day, true)}, {old_full, others}, now));
    // Its own check is recent: nothing due, nothing stands in for it.
    CHECK_FALSE(files_check_due(both, {check(RunKind::FullCheck, now - day)}, {others}, now));
    // Off: never.
    HealthPolicy no_rot;
    no_rot.rot_check = false;
    CHECK_FALSE(files_check_due(no_rot, {old_full}, {old_full, others}, now));
}
