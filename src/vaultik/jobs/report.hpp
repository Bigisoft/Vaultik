#pragma once

// The monthly report: what every job did in a calendar month (local time), from its run history
// and the drive health history, and what needs attention. Built from plain data (build_report)
// so it can be tested, then shown as plain text, HTML or JSON. The frontend shows it at the start
// of each month, and run-job can send a short summary to the alert channels (fixed wording and
// counts only, like every alert).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/drive_history.hpp"
#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// "2026-10" for a time (os::now() units), in local time.
std::string month_of(std::uint64_t time);
// Whether `month` is "YYYY-MM" with a month 01..12 (and a year from 1601 on).
bool valid_month(const std::string& month);
// The month before or after a valid "YYYY-MM".
std::string previous_month(const std::string& month);
std::string next_month(const std::string& month);
// The first moment of the month, local time (os::now() units). Throws Error for an invalid month.
std::uint64_t month_start(const std::string& month);

// One job and its history (oldest first; see combined_history).
struct ReportJob {
    Job job;
    std::vector<RunRecord> history;
};

struct JobReport {
    std::string id;
    std::string name;
    Schedule::Kind schedule = Schedule::Kind::Manual;
    // Backups started in the month, by result.
    std::uint64_t backups = 0, succeeded = 0, partial = 0, failed = 0, cancelled = 0;
    std::uint64_t last_version = 0;  // the newest backup that saved a version, up to the month's end (0: none)
    std::uint64_t added = 0;         // stored bytes the month's backups added
    // The backup's stored size at the month's end, and at its start (or at the first run in the
    // month that knew it, `size_since`). 0 when no run recorded it.
    std::uint64_t size = 0;
    std::uint64_t size_start = 0;
    std::uint64_t size_since = 0;
    // Health checks: fine, found damage, could not run (cancelled ones are not counted).
    std::uint64_t checks = 0, checks_ok = 0, checks_damage = 0, checks_failed = 0;
    // Test restores: fine, found problems, could not run.
    std::uint64_t tests = 0, tests_ok = 0, tests_damage = 0, tests_failed = 0;
    std::uint64_t copies = 0, copies_failed = 0;  // second copies
    std::uint64_t ransomware = 0;                 // backups that saw signs of ransomware
    std::vector<std::string> drive_problems;      // what the backup drive reported, each once
};

// A drive that got worse (see drive_trend), as of its last reading in the month.
struct DriveReport {
    std::string model;
    std::vector<std::string> changes;  // describe(DriveChange)
};

struct AttentionItem {
    enum class Kind { NeverSaved, NoRecentVersion, LastFailed, Damage, CheckFailed, TestFailed, CopyFailed,
                      Ransomware, BackupDrive, DriveWorse };
    Kind kind = Kind::NoRecentVersion;
    std::string subject;  // the job's name, or the drive's model
    std::uint64_t count = 0;  // days without a version, runs that failed, ...
};

struct MonthlyReport {
    std::string month;     // "2026-10"
    std::string computer;
    std::uint64_t made = 0;   // when it was built
    std::uint64_t start = 0;  // the month's first moment
    std::uint64_t end = 0;    // the next month's first moment
    std::uint64_t as_of = 0;  // the end, or `made` for the current month
    std::vector<JobReport> jobs;
    std::vector<DriveReport> drives;
    std::vector<AttentionItem> attention;
    // Whether any job ran in the month (no runs: there is nothing to report).
    bool any_runs() const;
};

// Days without a version after which a job needs attention: 2 for an hourly schedule, 7 daily,
// 14 weekly, 31 for a job only run by hand.
int days_without_version(Schedule::Kind kind);

// The report for `month` ("YYYY-MM"). `drives` are the stored drive readings (any order). Throws
// Error for an invalid month.
MonthlyReport build_report(const std::vector<ReportJob>& jobs, const std::vector<DriveReading>& drives,
                           const std::string& month, std::uint64_t now, const std::string& computer);

// Translates a fixed English phrase (the frontend passes its translator); %1, %2, ... are filled in
// afterwards. Empty: English.
using ReportWords = std::function<std::string(const std::string&)>;

std::string render_text(const MonthlyReport& report, const ReportWords& words = {});
// A self-contained page: inline styles, no scripts, no links, every text escaped.
std::string render_html(const MonthlyReport& report, const ReportWords& words = {});
std::string to_json(const MonthlyReport& report);
// "&lt;" and friends for &, <, >, " and '.
std::string html_escape(const std::string& text);

// The short summary for the alert channels: counts in fixed words, no job or drive names.
Alert summary_alert(const MonthlyReport& report);

// The report of `month` for every job in `store` (with the admin store's history) and every
// stored drive reading, as of now.
MonthlyReport report_for(const JobStore& store, const std::string& month);

// Once a month: when the alerts in `store`'s folder ask for a monthly summary and the month before
// the current one had runs, sends its summary_alert to the alert channels (claim_report "alert",
// so only one process sends it; released again when no channel took it). Returns one line per
// channel that failed; never throws.
std::vector<std::string> deliver_monthly_summary(const JobStore& store);

// Once-a-month bookkeeping in <dir>/reports: a marker file per delivery kind and month ("alert",
// "notice"), created exclusively, so of several processes only one delivers. claim_report returns
// true for the one that made the marker; release_report removes it again (a delivery that failed).
bool report_claimed(const std::string& dir, const std::string& kind, const std::string& month);
bool claim_report(const std::string& dir, const std::string& kind, const std::string& month);
void release_report(const std::string& dir, const std::string& kind, const std::string& month);

}  // namespace vaultik::jobs
