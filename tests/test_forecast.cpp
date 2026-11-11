#include <doctest/doctest.h>

#include "vaultik/jobs/forecast.hpp"
#include "vaultik/jobs/report.hpp"

using namespace vaultik;
using namespace vaultik::jobs;

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;
constexpr std::uint64_t kGB = 1ull << 30;
constexpr std::uint64_t kNow = 134'000'000'000'000'000ull;  // some time in 2025

// A size per day for `days` days up to now, growing by `per_day`.
std::vector<SizePoint> linear(std::uint64_t start, std::uint64_t per_day, int days) {
    std::vector<SizePoint> out;
    for (int i = 0; i < days; ++i)
        out.push_back({kNow - static_cast<std::uint64_t>(days - 1 - i) * kDay, start + per_day * static_cast<std::uint64_t>(i)});
    return out;
}

RunRecord backup(std::uint64_t finished, std::uint64_t size, std::uint64_t free = 0) {
    RunRecord r;
    r.kind = RunKind::Backup;
    r.started = finished - 1;
    r.finished = finished;
    r.backup_size = size;
    r.free_space = free;
    return r;
}

}  // namespace

TEST_CASE("forecast: steady growth fills the free space at that rate") {
    const auto points = linear(100 * kGB, kGB, 20);
    const FillForecast f = forecast_fill(points, 42 * kGB, kNow, kNow);
    CHECK(f.state == FillForecast::State::Fills);
    CHECK(f.growth_per_day == doctest::Approx(static_cast<double>(kGB)));
    CHECK(f.days == doctest::Approx(42.0));
    CHECK(fill_time_text(f.days) == "in about 6 weeks");
    CHECK_FALSE(fill_warning(f));
    // The free space measured 2 days ago has shrunk by two days' growth since.
    const FillForecast older = forecast_fill(points, 42 * kGB, kNow - 2 * kDay, kNow);
    CHECK(older.days == doctest::Approx(40.0));
    const FillForecast soon = forecast_fill(points, 10 * kGB, kNow, kNow);
    CHECK(fill_warning(soon));
    CHECK(soon.days == doctest::Approx(10.0));
}

TEST_CASE("forecast: too little history gives no forecast") {
    CHECK(forecast_fill({}, 10 * kGB, kNow, kNow).state == FillForecast::State::NoData);
    CHECK(forecast_fill(linear(kGB, kGB, 2), 10 * kGB, kNow, kNow).state == FillForecast::State::NoData);
    // Many runs on one day count as one.
    std::vector<SizePoint> same_day;
    for (int i = 0; i < 10; ++i) same_day.push_back({kNow - static_cast<std::uint64_t>(i) * 1000, kGB + static_cast<std::uint64_t>(i)});
    CHECK(forecast_fill(same_day, 10 * kGB, kNow, kNow).state == FillForecast::State::NoData);
    // Points older than the window are left out.
    auto old = linear(kGB, kGB, 10);
    for (SizePoint& p : old) p.time -= 200 * kDay;
    CHECK(forecast_fill(old, 10 * kGB, kNow, kNow).state == FillForecast::State::NoData);
}

TEST_CASE("forecast: the regular cleanup's small drops and odd days do not move the trend much") {
    // Grows 2 GB a day; every 3rd day the cleanup takes 1 GB back: net about 1.67 GB a day.
    std::vector<SizePoint> points;
    std::uint64_t size = 500 * kGB;
    for (int i = 0; i < 30; ++i) {
        size += 2 * kGB;
        if (i % 3 == 2) size -= kGB;
        points.push_back({kNow - static_cast<std::uint64_t>(29 - i) * kDay, size});
    }
    // One odd day (a big temporary file backed up and gone again the next day).
    points[15].size += 30 * kGB;
    const FillForecast f = forecast_fill(points, 100 * kGB, kNow, kNow);
    REQUIRE(f.state == FillForecast::State::Fills);
    CHECK(f.growth_per_day > 1.3 * static_cast<double>(kGB));
    CHECK(f.growth_per_day < 2.0 * static_cast<double>(kGB));
}

TEST_CASE("forecast: a big cleanup starts the trend again") {
    auto points = linear(100 * kGB, kGB, 20);
    // Cleaned up on day 10 to half; it grows half a GB a day since.
    for (std::size_t i = 10; i < points.size(); ++i) points[i].size = 55 * kGB + (i - 10) * (kGB / 2);
    const FillForecast f = forecast_fill(points, 100 * kGB, kNow, kNow);
    REQUIRE(f.state == FillForecast::State::Fills);
    CHECK(f.growth_per_day == doctest::Approx(static_cast<double>(kGB) / 2));
    // Right after the cleanup there is too little to go on.
    auto just = linear(100 * kGB, kGB, 20);
    just.back().size = 40 * kGB;
    CHECK(forecast_fill(just, 100 * kGB, kNow, kNow).state == FillForecast::State::NoData);
}

TEST_CASE("forecast: shrinking, steady and unknown free space") {
    std::vector<SizePoint> shrinking = linear(100 * kGB, 0, 10);
    for (std::size_t i = 0; i < shrinking.size(); ++i) shrinking[i].size -= i * (kGB / 10);
    CHECK(forecast_fill(shrinking, 10 * kGB, kNow, kNow).state == FillForecast::State::Shrinking);
    CHECK(forecast_fill(linear(100 * kGB, 0, 10), 10 * kGB, kNow, kNow).state == FillForecast::State::Steady);
    // 1 MB a day on 1 TB free: thousands of years.
    CHECK(forecast_fill(linear(100 * kGB, 1 << 20, 10), 1024 * kGB, kNow, kNow).state == FillForecast::State::Steady);
    const FillForecast cloud = forecast_fill(linear(100 * kGB, kGB, 10), std::nullopt, kNow, kNow);
    CHECK(cloud.state == FillForecast::State::NoSpaceInfo);
    CHECK(cloud.growth_per_day == doctest::Approx(static_cast<double>(kGB)));
    // Full already.
    const FillForecast full = forecast_fill(linear(100 * kGB, kGB, 10), 0, kNow - 3 * kDay, kNow);
    CHECK(full.state == FillForecast::State::Fills);
    CHECK(full.days == 0);
    CHECK(fill_time_text(full.days) == "within a day");
}

TEST_CASE("forecast: from a job's history") {
    std::vector<RunRecord> history;
    for (int i = 0; i < 10; ++i) history.push_back(backup(kNow - static_cast<std::uint64_t>(9 - i) * kDay, 10 * kGB + static_cast<std::uint64_t>(i) * kGB));
    history[7].free_space = 30 * kGB;  // the newest one that recorded it (two days ago)
    RunRecord check;
    check.kind = RunKind::Check;
    check.finished = kNow;
    check.backup_size = 1;  // not a backup: ignored
    history.push_back(check);
    const FillForecast f = forecast_for(history, kNow);
    REQUIRE(f.state == FillForecast::State::Fills);
    CHECK(f.free == 30 * kGB);
    CHECK(f.days == doctest::Approx(28.0));
    CHECK(fill_warning(f));
    // A measurement now wins.
    CHECK(forecast_for(history, kNow, 70 * kGB).days == doctest::Approx(70.0));
}

TEST_CASE("forecast: time left in words") {
    CHECK(fill_time_text(0.4) == "within a day");
    CHECK(fill_time_text(9.2) == "in about 9 days");
    CHECK(fill_time_text(45) == "in about 6 weeks");
    CHECK(fill_time_text(150) == "in about 5 months");
    CHECK(fill_time_text(800) == "in about 2 years");
    CHECK(fill_time_text(5000) == "in more than 10 years");
    CHECK(fill_time_phrase(45).first == "in about %1 weeks");
}

TEST_CASE("forecast: the monthly report warns about a drive that fills up soon") {
    ReportJob in;
    in.job.id = "docs";
    in.job.name = "Documents";
    in.job.schedule.kind = Schedule::Kind::Daily;
    const std::uint64_t now = month_start("2026-10") + 20 * kDay;
    for (int i = 0; i < 15; ++i) {
        RunRecord r = backup(now - static_cast<std::uint64_t>(14 - i) * kDay, 100 * kGB + static_cast<std::uint64_t>(i) * 2 * kGB, 0);
        r.started = r.finished - 1000;
        r.snapshot = "ab";
        in.history.push_back(r);
    }
    in.history.back().free_space = 20 * kGB;  // 10 days left
    const MonthlyReport r = build_report({in}, {}, "2026-10", now, "PC");
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].forecast.state == FillForecast::State::Fills);
    bool warned = false;
    for (const AttentionItem& a : r.attention)
        if (a.kind == AttentionItem::Kind::DriveFillsUp) warned = a.count == 10;
    CHECK(warned);
    const std::string text = render_text(r);
    CHECK(text.find("The backup drive of Documents fills up in about 10 days. Keep fewer versions") != std::string::npos);
    CHECK(text.find("Drive space") != std::string::npos);
    CHECK(to_json(r).find("\"forecast\"") != std::string::npos);
}
