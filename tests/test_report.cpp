#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/report.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;
using namespace vaultik::jobs;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;
constexpr std::uint64_t kHour = kDay / 24;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("vaultik-report-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string dir() const {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }
};

RunRecord run(std::uint64_t started, RunKind kind, RunStatus status) {
    RunRecord r;
    r.kind = kind;
    r.started = started;
    r.finished = started + kHour / 4;
    r.status = status;
    return r;
}

RunRecord backup(std::uint64_t started, RunStatus status, std::uint64_t stored = 0, std::uint64_t size = 0) {
    RunRecord r = run(started, RunKind::Backup, status);
    r.stored_bytes = stored;
    r.backup_size = size;
    return r;
}

Job job(const std::string& id, const std::string& name, Schedule::Kind kind) {
    Job j;
    j.id = id;
    j.name = name;
    j.schedule.kind = kind;
    return j;
}

const JobReport& find(const MonthlyReport& r, const std::string& id) {
    for (const JobReport& j : r.jobs)
        if (j.id == id) return j;
    FAIL("no job " << id);
    return r.jobs.front();
}

bool has(const MonthlyReport& r, AttentionItem::Kind kind, const std::string& subject, std::uint64_t* count = nullptr) {
    for (const AttentionItem& a : r.attention)
        if (a.kind == kind && a.subject == subject) {
            if (count) *count = a.count;
            return true;
        }
    return false;
}

std::size_t attention_of(const MonthlyReport& r, const std::string& subject) {
    std::size_t n = 0;
    for (const AttentionItem& a : r.attention) n += a.subject == subject;
    return n;
}

}  // namespace

TEST_CASE("report months follow the local calendar") {
    CHECK(valid_month("2026-10"));
    CHECK_FALSE(valid_month("2026-13"));
    CHECK_FALSE(valid_month("2026-00"));
    CHECK_FALSE(valid_month("2026-1"));
    CHECK_FALSE(valid_month("26-10-01"));
    CHECK_FALSE(valid_month("abcd-ef"));
    CHECK(previous_month("2026-01") == "2025-12");
    CHECK(previous_month("2026-10") == "2026-09");
    CHECK(next_month("2026-12") == "2027-01");
    CHECK(next_month("2026-02") == "2026-03");

    const std::uint64_t oct = month_start("2026-10");
    const std::uint64_t nov = month_start("2026-11");
    CHECK(month_of(oct) == "2026-10");
    CHECK(month_of(oct - 1) == "2026-09");
    CHECK(month_of(nov - 1) == "2026-10");
    CHECK(month_of(nov) == "2026-11");
    CHECK((nov - oct) / kHour >= 31 * 24 - 1);  // a daylight saving change moves it by an hour
    CHECK((nov - oct) / kHour <= 31 * 24 + 1);
    CHECK_THROWS_AS(month_start("2026-13"), Error);
}

TEST_CASE("the monthly report counts runs, growth and what needs attention") {
    const std::string month = "2026-10";
    const std::uint64_t start = month_start(month);
    const std::uint64_t end = month_start("2026-11");

    // A daily job with a bit of everything.
    ReportJob docs{job("docs", "Docs", Schedule::Kind::Daily), {}};
    auto& h = docs.history;
    h.push_back(backup(start - 2 * kDay, RunStatus::Success, 900, 1000));  // before the month
    RunRecord first = backup(start + kDay, RunStatus::Success, 100, 1100);
    first.alert = "many files renamed";
    h.push_back(first);
    h.push_back(run(start + kDay + kHour, RunKind::Check, RunStatus::Success));
    RunRecord partial = backup(start + 2 * kDay, RunStatus::Partial, 50, 1150);
    partial.drive = "WD Disk: warning";
    h.push_back(partial);
    RunRecord damage = run(start + 3 * kDay, RunKind::Check, RunStatus::Failed);
    damage.errors = 2;
    h.push_back(damage);
    h.push_back(backup(start + 3 * kDay + kHour, RunStatus::Failed));
    h.push_back(backup(start + 4 * kDay, RunStatus::Cancelled));
    h.push_back(run(start + 5 * kDay, RunKind::FullCheck, RunStatus::Failed));  // could not run
    h.push_back(run(start + 5 * kDay + kHour, RunKind::Check, RunStatus::Cancelled));
    h.push_back(run(start + 6 * kDay, RunKind::RestoreTest, RunStatus::Success));
    h.push_back(run(start + 6 * kDay + kHour, RunKind::Copy, RunStatus::Failed));
    h.push_back(backup(end + kDay, RunStatus::Success, 5000, 9999));  // after the month

    // A job run by hand that never ran, with a name that needs escaping.
    ReportJob photos{job("photos", "Photos <&> \"x\"", Schedule::Kind::Manual), {}};

    // An hourly job whose size is first known within the month.
    ReportJob mail{job("mail", "Mail", Schedule::Kind::Hourly), {}};
    mail.history.push_back(backup(start + 10 * kDay, RunStatus::Success, 10, 500));
    mail.history.push_back(backup(end - kHour, RunStatus::Success, 20, 700));

    // Runs right at the month's edges.
    ReportJob edge{job("edge", "Edge", Schedule::Kind::Daily), {}};
    edge.history.push_back(backup(start - 1, RunStatus::Success, 7));
    edge.history.push_back(backup(start, RunStatus::Success, 1));
    edge.history.push_back(backup(end - 1, RunStatus::Success, 2));
    edge.history.push_back(backup(end, RunStatus::Success, 4));

    // A drive that got worse during the month, and one only read before it.
    std::vector<DriveReading> drives;
    DriveReading a;
    a.key = "sn:1";
    a.model = "Disk <1>";
    a.time = start - 20 * kDay;
    a.reallocated = 0;
    drives.push_back(a);
    a.time = start + 15 * kDay;
    a.reallocated = 5;
    drives.push_back(a);
    DriveReading b;
    b.key = "sn:2";
    b.model = "Old disk";
    b.time = start - 40 * kDay;
    b.reallocated = 0;
    drives.push_back(b);
    b.time = start - 10 * kDay;
    b.reallocated = 9;
    drives.push_back(b);

    const MonthlyReport r = build_report({docs, photos, mail, edge}, drives, month, end + 10 * kDay, "PC1");
    CHECK(r.month == month);
    CHECK(r.as_of == end);
    CHECK(r.any_runs());
    REQUIRE(r.jobs.size() == 4);

    const JobReport& d = find(r, "docs");
    CHECK(d.backups == 4);
    CHECK(d.succeeded == 1);
    CHECK(d.partial == 1);
    CHECK(d.failed == 1);
    CHECK(d.cancelled == 1);
    CHECK(d.added == 150);
    CHECK(d.last_version == start + 2 * kDay);  // a partial backup saved a version too
    CHECK(d.size == 1150);
    CHECK(d.size_start == 1000);
    CHECK(d.size_since < start);
    CHECK(d.checks == 3);  // the cancelled one is not counted
    CHECK(d.checks_ok == 1);
    CHECK(d.checks_damage == 1);
    CHECK(d.checks_failed == 1);
    CHECK(d.tests == 1);
    CHECK(d.tests_ok == 1);
    CHECK(d.copies == 1);
    CHECK(d.copies_failed == 1);
    CHECK(d.ransomware == 1);
    CHECK(d.drive_problems == std::vector<std::string>{"WD Disk: warning"});

    std::uint64_t n = 0;
    CHECK(has(r, AttentionItem::Kind::NoRecentVersion, "Docs", &n));
    CHECK(n >= 28);
    CHECK(has(r, AttentionItem::Kind::LastFailed, "Docs", &n));
    CHECK(n == 1);
    CHECK(has(r, AttentionItem::Kind::Damage, "Docs"));
    CHECK(has(r, AttentionItem::Kind::CheckFailed, "Docs"));
    CHECK_FALSE(has(r, AttentionItem::Kind::TestFailed, "Docs"));
    CHECK(has(r, AttentionItem::Kind::CopyFailed, "Docs"));
    CHECK(has(r, AttentionItem::Kind::Ransomware, "Docs"));
    CHECK(has(r, AttentionItem::Kind::BackupDrive, "Docs"));

    CHECK(has(r, AttentionItem::Kind::NeverSaved, "Photos <&> \"x\""));
    CHECK(find(r, "photos").backups == 0);

    const JobReport& m = find(r, "mail");
    CHECK(m.size == 700);
    CHECK(m.size_start == 500);
    CHECK(m.size_since == start + 10 * kDay);
    CHECK(attention_of(r, "Mail") == 0);

    const JobReport& e = find(r, "edge");
    CHECK(e.backups == 2);  // at the first moment, and just before the next month
    CHECK(e.added == 3);
    CHECK(attention_of(r, "Edge") == 0);

    REQUIRE(r.drives.size() == 1);
    CHECK(r.drives[0].model == "Disk <1>");
    CHECK(has(r, AttentionItem::Kind::DriveWorse, "Disk <1>"));
    CHECK_FALSE(has(r, AttentionItem::Kind::DriveWorse, "Old disk"));

    // The current month: judged as of now, not its end.
    const ReportJob early{mail.job, {mail.history[0]}};
    const MonthlyReport now = build_report({early}, {}, month, start + 12 * kDay, "PC1");
    CHECK(now.as_of == start + 12 * kDay);
    CHECK(find(now, "mail").backups == 1);
    // Two days without a version is too long for an hourly job.
    CHECK(has(now, AttentionItem::Kind::NoRecentVersion, "Mail", &n));
    CHECK(n == 2);

    // A month nobody ran anything in.
    const MonthlyReport quiet = build_report({photos}, {}, "2026-09", end, "PC1");
    CHECK_FALSE(quiet.any_runs());

    // The text form.
    const std::string text = render_text(r);
    CHECK(text.find("Vaultik monthly report: October 2026") != std::string::npos);
    CHECK(text.find("Needs attention:") != std::string::npos);
    CHECK(text.find("Backups: 4 (1 succeeded, 1 partly, 1 failed, 1 cancelled)") != std::string::npos);
    CHECK(text.find("Backup size: 1.1 KB (+150 B this month)") != std::string::npos);
    CHECK(text.find("Backup size: 700 B (+200 B since ") != std::string::npos);
    CHECK(text.find("The last backup of Docs failed (1 failed this month)") != std::string::npos);
    const std::string bg = render_text(r, [](const std::string& s) { return s == "Backups" ? std::string("Архиви") : s; });
    CHECK(bg.find("Архиви: 4 (") != std::string::npos);

    // The HTML form: every text escaped, nothing active.
    const std::string html = render_html(r);
    CHECK(html.find("Photos &lt;&amp;&gt; &quot;x&quot;") != std::string::npos);
    CHECK(html.find("Photos <&>") == std::string::npos);
    CHECK(html.find("Disk &lt;1&gt;") != std::string::npos);
    CHECK(html.find("<script") == std::string::npos);
    CHECK(html.find("href") == std::string::npos);
    CHECK(html_escape("a<b>&'\"") == "a&lt;b&gt;&amp;&#39;&quot;");

    // JSON.
    const nlohmann::json j = nlohmann::json::parse(to_json(r));
    CHECK(j["month"] == month);
    CHECK(j["jobs"].size() == 4);
    CHECK(j["attention"].size() == r.attention.size());

    // The alert summary: counts in fixed words, no job or drive names.
    const Alert alert = summary_alert(r);
    CHECK(alert.event == "report");
    CHECK(alert.level == Alert::Level::Warning);
    CHECK(alert.title == "Vaultik monthly report for 2026-10 (PC1)");
    CHECK(alert.message.find("4 job(s), 8 backup(s): 5 succeeded, 1 partly, 1 failed.") != std::string::npos);
    for (const std::string name : {"Docs", "Photos", "Mail", "Edge", "Disk", "WD"}) {
        CHECK(alert.message.find(name) == std::string::npos);
        CHECK(alert.title.find(name) == std::string::npos);
    }
    CHECK(alert.job.empty());
}

TEST_CASE("a monthly report is delivered once per month") {
    TempDir tmp;
    CHECK_FALSE(report_claimed(tmp.dir(), "alert", "2026-10"));
    CHECK(claim_report(tmp.dir(), "alert", "2026-10"));
    CHECK(report_claimed(tmp.dir(), "alert", "2026-10"));
    CHECK_FALSE(claim_report(tmp.dir(), "alert", "2026-10"));  // a second process
    CHECK(claim_report(tmp.dir(), "notice", "2026-10"));      // another kind is separate
    CHECK(claim_report(tmp.dir(), "alert", "2026-11"));

    // A delivery that failed is released and tried again.
    release_report(tmp.dir(), "alert", "2026-11");
    CHECK_FALSE(report_claimed(tmp.dir(), "alert", "2026-11"));
    CHECK(claim_report(tmp.dir(), "alert", "2026-11"));

    // Markers older than a year go.
    CHECK(claim_report(tmp.dir(), "alert", "2025-09"));
    CHECK(claim_report(tmp.dir(), "alert", "2026-01"));
    CHECK(claim_report(tmp.dir(), "alert", "2026-12"));
    CHECK_FALSE(report_claimed(tmp.dir(), "alert", "2025-09"));
    CHECK(report_claimed(tmp.dir(), "alert", "2026-01"));
    CHECK(report_claimed(tmp.dir(), "notice", "2026-10"));

    CHECK_THROWS_AS(claim_report(tmp.dir(), "../x", "2026-10"), Error);
    CHECK_THROWS_AS(claim_report(tmp.dir(), "alert", "2026-1"), Error);

    // No alerts set up: nothing is sent and nothing is claimed.
    TempDir other;
    const JobStore store(other.dir());
    CHECK(deliver_monthly_summary(store).empty());
    CHECK_FALSE(report_claimed(other.dir(), "alert", previous_month(month_of(os::now()))));
}
