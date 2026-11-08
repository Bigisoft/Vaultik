#include <doctest/doctest.h>

#include <filesystem>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/job.hpp"
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
    const Job back = job_from_json(to_json(j));
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
