#include "vaultik/jobs/report.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::jobs {

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;  // FILETIME ticks

// Fills %1 .. %9 in a phrase (after it was translated), in one pass: a value that contains "%2"
// stays as it is.
std::string fill(const std::string& text, const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '1' && text[i + 1] <= '9') {
            const std::size_t n = static_cast<std::size_t>(text[i + 1] - '1');
            if (n < args.size()) {
                out += args[n];
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

struct Say {
    const ReportWords& words;
    std::string operator()(const std::string& phrase, const std::vector<std::string>& args = {}) const {
        return fill(words ? words(phrase) : phrase, args);
    }
};

std::string num(std::uint64_t n) {
    return std::to_string(n);
}

// "1.5 GB" (binary units, like the job settings).
std::string size_text(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, kUnits[unit]);
    return buf;
}

std::string growth_text(std::uint64_t from, std::uint64_t to) {
    return to >= from ? "+" + size_text(to - from) : "-" + size_text(from - to);
}

std::string date_text(std::uint64_t time) {
    return os::format_time_local(time).substr(0, 16);
}

const char* const kMonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

std::string month_title(const MonthlyReport& r, const Say& say) {
    if (!valid_month(r.month)) return r.month;
    const int m = std::stoi(r.month.substr(5, 2));
    return say("%1 %2", {say(kMonthNames[m - 1]), r.month.substr(0, 4)});
}

const char* kind_name(AttentionItem::Kind k) {
    switch (k) {
        case AttentionItem::Kind::NeverSaved: return "never_saved";
        case AttentionItem::Kind::NoRecentVersion: return "no_recent_version";
        case AttentionItem::Kind::LastFailed: return "last_failed";
        case AttentionItem::Kind::Damage: return "damage";
        case AttentionItem::Kind::CheckFailed: return "check_failed";
        case AttentionItem::Kind::TestFailed: return "test_failed";
        case AttentionItem::Kind::CopyFailed: return "copy_failed";
        case AttentionItem::Kind::Ransomware: return "ransomware";
        case AttentionItem::Kind::BackupDrive: return "backup_drive";
        case AttentionItem::Kind::DriveWorse: return "drive_worse";
    }
    return "other";
}

std::string attention_text(const AttentionItem& a, const Say& say) {
    const std::string n = num(a.count);
    switch (a.kind) {
        case AttentionItem::Kind::NeverSaved: return say("%1 has not saved a version yet", {a.subject});
        case AttentionItem::Kind::NoRecentVersion: return say("%1 has not saved a version for %2 days", {a.subject, n});
        case AttentionItem::Kind::LastFailed:
            return say("The last backup of %1 failed (%2 failed this month)", {a.subject, n});
        case AttentionItem::Kind::Damage:
            return say("Health checks or test restores of %1 found damage (%2 times)", {a.subject, n});
        case AttentionItem::Kind::CheckFailed: return say("Health checks of %1 could not run (%2 times)", {a.subject, n});
        case AttentionItem::Kind::TestFailed: return say("Test restores of %1 could not run (%2 times)", {a.subject, n});
        case AttentionItem::Kind::CopyFailed: return say("The second copy of %1 failed (%2 times)", {a.subject, n});
        case AttentionItem::Kind::Ransomware:
            return say("Backups of %1 saw signs of ransomware (%2 times)", {a.subject, n});
        case AttentionItem::Kind::BackupDrive:
            return say("The backup drive of %1 reported a problem (%2 runs)", {a.subject, n});
        case AttentionItem::Kind::DriveWorse: return say("Drive %1 is getting worse", {a.subject});
    }
    return {};
}

// A job's facts as label/value lines, shared by the text and HTML forms.
std::vector<std::pair<std::string, std::string>> job_lines(const MonthlyReport& r, const JobReport& j, const Say& say) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string none = say("none this month");
    out.emplace_back(say("Backups"), j.backups == 0 ? none
                                                    : say("%1 (%2 succeeded, %3 partly, %4 failed, %5 cancelled)",
                                                          {num(j.backups), num(j.succeeded), num(j.partial),
                                                           num(j.failed), num(j.cancelled)}));
    out.emplace_back(say("Last successful backup"), j.last_version ? date_text(j.last_version) : say("none"));
    out.emplace_back(say("Data added"), size_text(j.added));
    std::string size = say("not recorded");
    if (j.size > 0 && j.size_start > 0 && j.size_since < r.start)
        size = say("%1 (%2 this month)", {size_text(j.size), growth_text(j.size_start, j.size)});
    else if (j.size > 0 && j.size_start > 0)
        size = say("%1 (%2 since %3)", {size_text(j.size), growth_text(j.size_start, j.size), date_text(j.size_since)});
    else if (j.size > 0)
        size = size_text(j.size);
    out.emplace_back(say("Backup size"), size);
    out.emplace_back(say("Health checks"),
                     j.checks == 0 ? none
                                   : say("%1 (%2 fine, %3 found damage, %4 could not run)",
                                         {num(j.checks), num(j.checks_ok), num(j.checks_damage), num(j.checks_failed)}));
    out.emplace_back(say("Test restores"),
                     j.tests == 0 ? none
                                  : say("%1 (%2 fine, %3 found problems, %4 could not run)",
                                        {num(j.tests), num(j.tests_ok), num(j.tests_damage), num(j.tests_failed)}));
    if (j.copies > 0) out.emplace_back(say("Second copies"), say("%1 (%2 failed)", {num(j.copies), num(j.copies_failed)}));
    for (const std::string& d : j.drive_problems) out.emplace_back(say("Backup drive"), d);
    return out;
}

bool safe_name(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::string marker_path(const std::string& dir, const std::string& kind, const std::string& month) {
    if (!safe_name(kind) || !valid_month(month)) throw Error("invalid report marker " + kind + " " + month);
    return os::join(os::join(dir, "reports"), kind + "-" + month);
}

}  // namespace

std::string month_of(std::uint64_t time) {
    const std::string t = os::format_time_local(time);
    return t.size() >= 7 ? t.substr(0, 7) : std::string();
}

bool valid_month(const std::string& month) {
    if (month.size() != 7 || month[4] != '-') return false;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u})
        if (month[i] < '0' || month[i] > '9') return false;
    const int year = std::stoi(month.substr(0, 4));
    const int m = std::stoi(month.substr(5, 2));
    return year >= 1971 && year <= 3000 && m >= 1 && m <= 12;
}

namespace {
std::string make_month(int year, int m) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d", year, m);
    return buf;
}
}  // namespace

std::string previous_month(const std::string& month) {
    const int year = std::stoi(month.substr(0, 4));
    const int m = std::stoi(month.substr(5, 2));
    return m == 1 ? make_month(year - 1, 12) : make_month(year, m - 1);
}

std::string next_month(const std::string& month) {
    const int year = std::stoi(month.substr(0, 4));
    const int m = std::stoi(month.substr(5, 2));
    return m == 12 ? make_month(year + 1, 1) : make_month(year, m + 1);
}

std::uint64_t month_start(const std::string& month) {
    if (!valid_month(month)) throw Error("invalid month \"" + month + "\" (use YYYY-MM, e.g. 2026-10)");
    std::tm tm{};
    tm.tm_year = std::stoi(month.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(month.substr(5, 2)) - 1;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;  // whatever daylight saving time says for that day
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) throw Error("invalid month \"" + month + "\"");
    return os::from_unix(static_cast<std::int64_t>(t), 0);
}

bool MonthlyReport::any_runs() const {
    for (const JobReport& j : jobs)
        if (j.backups + j.checks + j.tests + j.copies > 0) return true;
    return false;
}

int days_without_version(Schedule::Kind kind) {
    switch (kind) {
        case Schedule::Kind::Hourly: return 2;
        case Schedule::Kind::Daily: return 7;
        case Schedule::Kind::Weekly: return 14;
        case Schedule::Kind::Manual: return 31;
    }
    return 7;
}

MonthlyReport build_report(const std::vector<ReportJob>& jobs, const std::vector<DriveReading>& drives,
                           const std::string& month, std::uint64_t now, const std::string& computer) {
    MonthlyReport r;
    r.month = month;
    r.computer = computer;
    r.made = now;
    r.start = month_start(month);
    r.end = month_start(next_month(month));
    r.as_of = std::min(now, r.end);
    const auto in_month = [&](const RunRecord& x) { return x.started >= r.start && x.started < r.end; };

    for (const ReportJob& in : jobs) {
        JobReport j;
        j.id = in.job.id;
        j.name = in.job.name;
        j.schedule = in.job.schedule.kind;
        const RunRecord* last_backup = nullptr;  // the newest finished backup up to the end
        std::set<std::string> drive_seen;
        std::uint64_t drive_runs = 0;
        for (const RunRecord& x : in.history) {
            if (x.started >= r.end) continue;
            const bool inside = in_month(x);
            // The size: the last known value before the month, then the newest in it.
            if (x.backup_size > 0) {
                if (!inside) {
                    j.size_start = x.backup_size;
                    j.size_since = x.started;
                } else if (j.size_start == 0) {
                    j.size_start = x.backup_size;
                    j.size_since = x.started;
                }
                j.size = x.backup_size;
            }
            if (x.kind == RunKind::Backup) {
                if (x.status != RunStatus::Cancelled) last_backup = &x;
                if (x.status == RunStatus::Success || x.status == RunStatus::Partial) j.last_version = x.started;
            }
            if (!inside) continue;
            switch (x.kind) {
                case RunKind::Backup:
                    ++j.backups;
                    if (x.status == RunStatus::Success) ++j.succeeded;
                    else if (x.status == RunStatus::Partial) ++j.partial;
                    else if (x.status == RunStatus::Failed) ++j.failed;
                    else ++j.cancelled;
                    j.added += x.stored_bytes;
                    if (!x.alert.empty()) ++j.ransomware;
                    if (!x.drive.empty()) {
                        ++drive_runs;
                        if (drive_seen.insert(x.drive).second) j.drive_problems.push_back(x.drive);
                    }
                    break;
                case RunKind::Check:
                case RunKind::FullCheck:
                    if (x.status == RunStatus::Cancelled) break;
                    ++j.checks;
                    if (x.status == RunStatus::Failed && x.errors > 0) ++j.checks_damage;
                    else if (x.status == RunStatus::Failed) ++j.checks_failed;
                    else ++j.checks_ok;
                    break;
                case RunKind::RestoreTest:
                    if (x.status == RunStatus::Cancelled) break;
                    ++j.tests;
                    if (x.status == RunStatus::Failed && x.errors > 0) ++j.tests_damage;
                    else if (x.status == RunStatus::Failed) ++j.tests_failed;
                    else ++j.tests_ok;
                    break;
                case RunKind::Copy:
                    if (x.status == RunStatus::Cancelled) break;
                    ++j.copies;
                    if (x.status == RunStatus::Failed) ++j.copies_failed;
                    break;
            }
        }

        using K = AttentionItem::Kind;
        const auto attend = [&](K kind, std::uint64_t count) { r.attention.push_back({kind, j.name, count}); };
        if (j.last_version == 0) {
            attend(K::NeverSaved, 0);
        } else {
            const std::uint64_t days = r.as_of > j.last_version ? (r.as_of - j.last_version) / kDay : 0;
            if (days >= static_cast<std::uint64_t>(days_without_version(j.schedule))) attend(K::NoRecentVersion, days);
        }
        if (last_backup != nullptr && in_month(*last_backup) && last_backup->status == RunStatus::Failed)
            attend(K::LastFailed, j.failed);
        if (j.checks_damage + j.tests_damage > 0) attend(K::Damage, j.checks_damage + j.tests_damage);
        if (j.checks_failed > 0) attend(K::CheckFailed, j.checks_failed);
        if (j.tests_failed > 0) attend(K::TestFailed, j.tests_failed);
        if (j.copies_failed > 0) attend(K::CopyFailed, j.copies_failed);
        if (j.ransomware > 0) attend(K::Ransomware, j.ransomware);
        if (drive_runs > 0) attend(K::BackupDrive, drive_runs);
        r.jobs.push_back(std::move(j));
    }

    // Drives that got worse, as of their last reading in the month.
    std::map<std::string, std::vector<DriveReading>> by_drive;
    for (const DriveReading& d : drives)
        if (d.time < r.end) by_drive[d.key].push_back(d);
    for (auto& [key, list] : by_drive) {
        std::stable_sort(list.begin(), list.end(), [](const DriveReading& a, const DriveReading& b) { return a.time < b.time; });
        const DriveReading& last = list.back();
        if (last.time < r.start) continue;
        const std::vector<DriveChange> changes = drive_trend(list, last);
        if (changes.empty()) continue;
        DriveReport d;
        d.model = last.model.empty() ? key : last.model;
        for (const DriveChange& c : changes) d.changes.push_back(describe(c));
        r.attention.push_back({AttentionItem::Kind::DriveWorse, d.model, changes.size()});
        r.drives.push_back(std::move(d));
    }
    return r;
}

std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string render_text(const MonthlyReport& r, const ReportWords& words) {
    const Say say{words};
    std::string s = say("Vaultik monthly report") + ": " + month_title(r, say) + "\n";
    s += say("Computer") + ": " + r.computer + "\n";
    s += say("Created") + ": " + date_text(r.made) + "\n\n";
    if (r.attention.empty()) {
        s += say("Nothing needs attention.") + "\n";
    } else {
        s += say("Needs attention") + ":\n";
        for (const AttentionItem& a : r.attention) s += "  - " + attention_text(a, say) + "\n";
    }
    if (r.jobs.empty()) s += "\n" + say("No backup jobs.") + "\n";
    for (const JobReport& j : r.jobs) {
        s += "\n" + j.name + "\n";
        for (const auto& [label, value] : job_lines(r, j, say)) s += "  " + label + ": " + value + "\n";
    }
    if (!r.drives.empty()) {
        s += "\n" + say("Drives") + "\n";
        for (const DriveReport& d : r.drives)
            for (const std::string& c : d.changes) s += "  " + d.model + ": " + c + "\n";
    }
    return s;
}

std::string render_html(const MonthlyReport& r, const ReportWords& words) {
    const Say say{words};
    const auto e = [](const std::string& t) { return html_escape(t); };
    std::string s =
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
        "<title>" + e(say("Vaultik monthly report") + ": " + month_title(r, say)) + "</title>"
        "<style>body{font-family:'Segoe UI',sans-serif;margin:16px;color:#1b1b1b;background:#fff}"
        "h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:20px 0 6px}"
        "p.meta{color:#5f5f5f;margin:0}ul.attention li{color:#a4262c}p.ok{color:#107c10}"
        "table{border-collapse:collapse}td{padding:2px 16px 2px 0;vertical-align:top}"
        "td.label{color:#5f5f5f}</style></head><body>\n";
    s += "<h1>" + e(say("Vaultik monthly report") + ": " + month_title(r, say)) + "</h1>\n";
    s += "<p class=\"meta\">" + e(say("Computer") + ": " + r.computer) + "</p>\n";
    s += "<p class=\"meta\">" + e(say("Created") + ": " + date_text(r.made)) + "</p>\n";
    if (r.attention.empty()) {
        s += "<p class=\"ok\">" + e(say("Nothing needs attention.")) + "</p>\n";
    } else {
        s += "<h2>" + e(say("Needs attention")) + "</h2>\n<ul class=\"attention\">\n";
        for (const AttentionItem& a : r.attention) s += "<li>" + e(attention_text(a, say)) + "</li>\n";
        s += "</ul>\n";
    }
    if (r.jobs.empty()) s += "<p>" + e(say("No backup jobs.")) + "</p>\n";
    for (const JobReport& j : r.jobs) {
        s += "<h2>" + e(j.name) + "</h2>\n<table>\n";
        for (const auto& [label, value] : job_lines(r, j, say))
            s += "<tr><td class=\"label\">" + e(label) + "</td><td>" + e(value) + "</td></tr>\n";
        s += "</table>\n";
    }
    if (!r.drives.empty()) {
        s += "<h2>" + e(say("Drives")) + "</h2>\n<table>\n";
        for (const DriveReport& d : r.drives)
            for (const std::string& c : d.changes)
                s += "<tr><td class=\"label\">" + e(d.model) + "</td><td>" + e(c) + "</td></tr>\n";
        s += "</table>\n";
    }
    s += "</body></html>\n";
    return s;
}

std::string to_json(const MonthlyReport& r) {
    const Say say{ReportWords{}};
    nlohmann::json jobs = nlohmann::json::array();
    for (const JobReport& j : r.jobs) {
        jobs.push_back({{"id", j.id},
                        {"name", j.name},
                        {"backups", j.backups},
                        {"succeeded", j.succeeded},
                        {"partial", j.partial},
                        {"failed", j.failed},
                        {"cancelled", j.cancelled},
                        {"last_success", j.last_version},
                        {"added", j.added},
                        {"size", j.size},
                        {"size_start", j.size_start},
                        {"size_since", j.size_since},
                        {"checks", j.checks},
                        {"checks_ok", j.checks_ok},
                        {"checks_damage", j.checks_damage},
                        {"checks_failed", j.checks_failed},
                        {"tests", j.tests},
                        {"tests_ok", j.tests_ok},
                        {"tests_damage", j.tests_damage},
                        {"tests_failed", j.tests_failed},
                        {"copies", j.copies},
                        {"copies_failed", j.copies_failed},
                        {"ransomware", j.ransomware},
                        {"drive_problems", j.drive_problems}});
    }
    nlohmann::json drives = nlohmann::json::array();
    for (const DriveReport& d : r.drives) drives.push_back({{"model", d.model}, {"changes", d.changes}});
    nlohmann::json attention = nlohmann::json::array();
    for (const AttentionItem& a : r.attention)
        attention.push_back({{"kind", kind_name(a.kind)}, {"subject", a.subject}, {"count", a.count},
                             {"text", attention_text(a, say)}});
    const nlohmann::json j = {{"month", r.month},  {"computer", r.computer}, {"made", r.made},
                              {"start", r.start},  {"end", r.end},           {"jobs", jobs},
                              {"drives", drives},  {"attention", attention}};
    return j.dump(2);
}

Alert summary_alert(const MonthlyReport& r) {
    // Alerts leave the computer: counts in fixed words, never job, drive or file names.
    std::uint64_t backups = 0, succeeded = 0, partial = 0, failed = 0;
    for (const JobReport& j : r.jobs) {
        backups += j.backups;
        succeeded += j.succeeded;
        partial += j.partial;
        failed += j.failed;
    }
    Alert a;
    a.level = r.attention.empty() ? Alert::Level::Info : Alert::Level::Warning;
    a.event = "report";
    a.title = "Vaultik monthly report for " + r.month + " (" + r.computer + ")";
    a.message = num(r.jobs.size()) + " job(s), " + num(backups) + " backup(s): " + num(succeeded) + " succeeded, " +
                num(partial) + " partly, " + num(failed) + " failed. " +
                (r.attention.empty() ? std::string("Nothing needs attention.")
                                     : num(r.attention.size()) + " item(s) need attention.") +
                " Open Vaultik on " + r.computer + " for the details.";
    return a;
}

MonthlyReport report_for(const JobStore& store, const std::string& month) {
    std::vector<ReportJob> in;
    for (Job& job : store.list()) {
        std::vector<RunRecord> history = combined_history(store, job.id, 20000);
        in.push_back({std::move(job), std::move(history)});
    }
    return build_report(in, all_drive_readings(), month, os::now(), os::hostname());
}

std::vector<std::string> deliver_monthly_summary(const JobStore& store) {
    try {
        const AlertSettings settings = load_alerts(store.dir());
        if (!settings.monthly_summary || !settings.any()) return {};
        const std::string month = previous_month(month_of(os::now()));
        if (report_claimed(store.dir(), "alert", month)) return {};
        const MonthlyReport report = report_for(store, month);
        if (!report.any_runs()) return {};
        if (!claim_report(store.dir(), "alert", month)) return {};  // another process has it
        const std::vector<std::string> failed = send_alert(settings, summary_alert(report), store.dir());
        const std::size_t channels = (settings.ntfy_url.empty() ? 0 : 1) + (settings.telegram_chat.empty() ? 0 : 1) +
                                     (settings.webhook_url.empty() ? 0 : 1);
        if (failed.size() >= channels) release_report(store.dir(), "alert", month);  // try again next time
        return failed;
    } catch (const std::exception& e) {
        return {std::string("monthly summary: ") + e.what()};
    }
}

bool report_claimed(const std::string& dir, const std::string& kind, const std::string& month) {
    return os::exists(marker_path(dir, kind, month));
}

bool claim_report(const std::string& dir, const std::string& kind, const std::string& month) {
    const std::string path = marker_path(dir, kind, month);
    const std::string folder = os::join(dir, "reports");
    os::make_dirs(folder);
    os::ErrorCode err = 0;
    os::File f = os::File::create(path, false, err, 0600);
    if (!f.valid()) {
        if (os::is_exists(err)) return false;
        throw os::OsError("cannot create " + path, err);
    }
    f.close();
    // Markers older than a year are not needed any more.
    const std::string oldest = make_month(std::stoi(month.substr(0, 4)) - 1, std::stoi(month.substr(5, 2)));
    os::DirReader reader(folder);
    std::string name;
    os::FileInfo info;
    std::vector<std::string> old;
    while (reader.next(name, info)) {
        if (name.size() < 8 || !name.starts_with(kind + "-")) continue;
        const std::string m = name.substr(kind.size() + 1);
        if (valid_month(m) && m < oldest) old.push_back(name);
    }
    for (const std::string& n : old) os::remove_file(os::join(folder, n));
    return true;
}

void release_report(const std::string& dir, const std::string& kind, const std::string& month) {
    os::remove_file(marker_path(dir, kind, month));
}

}  // namespace vaultik::jobs
