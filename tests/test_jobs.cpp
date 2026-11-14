#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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
    std::ifstream in(fs::u8path(first));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
    std::ifstream in2(second);
    const std::string text2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
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
