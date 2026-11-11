#include "vaultik/jobs/forecast.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace vaultik::jobs {

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;  // FILETIME ticks
constexpr double kSteadyDays = 3650;               // fills later than this: no fill in sight

std::string size_text(double bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = std::fabs(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) std::snprintf(buf, sizeof buf, "%.0f B", v);
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, kUnits[unit]);
    return buf;
}

}  // namespace

std::vector<SizePoint> size_points(const std::vector<RunRecord>& history) {
    std::vector<SizePoint> out;
    for (const RunRecord& r : history)
        if (r.kind == RunKind::Backup && r.backup_size > 0 && r.finished > 0) out.push_back({r.finished, r.backup_size});
    std::stable_sort(out.begin(), out.end(), [](const SizePoint& a, const SizePoint& b) { return a.time < b.time; });
    return out;
}

FillForecast forecast_fill(const std::vector<SizePoint>& points, std::optional<std::uint64_t> free,
                           std::uint64_t free_time, std::uint64_t now) {
    FillForecast f;
    // One point per day, the day's newest, within the window.
    std::map<std::uint64_t, SizePoint> by_day;
    for (const SizePoint& p : points) {
        if (p.time > now || now - p.time > kForecastWindow || p.size == 0) continue;
        SizePoint& slot = by_day[p.time / kDay];
        if (p.time >= slot.time) slot = p;
    }
    std::vector<SizePoint> days;
    for (const auto& [day, p] : by_day) days.push_back(p);
    // Only what came after the last big drop.
    for (std::size_t i = days.size(); i-- > 1;) {
        if (static_cast<double>(days[i].size) < 0.9 * static_cast<double>(days[i - 1].size)) {
            days.erase(days.begin(), days.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (days.size() < 3 || days.back().time - days.front().time < 3 * kDay) return f;

    std::vector<double> slopes;
    slopes.reserve(days.size() * (days.size() - 1) / 2);
    for (std::size_t i = 0; i < days.size(); ++i) {
        for (std::size_t j = i + 1; j < days.size(); ++j) {
            const double dt = static_cast<double>(days[j].time - days[i].time) / static_cast<double>(kDay);
            if (dt <= 0) continue;
            slopes.push_back((static_cast<double>(days[j].size) - static_cast<double>(days[i].size)) / dt);
        }
    }
    if (slopes.empty()) return f;
    std::sort(slopes.begin(), slopes.end());
    const std::size_t n = slopes.size();
    f.growth_per_day = n % 2 == 1 ? slopes[n / 2] : (slopes[n / 2 - 1] + slopes[n / 2]) / 2;

    if (f.growth_per_day < 0) {
        f.state = FillForecast::State::Shrinking;
        return f;
    }
    if (!free) {
        f.state = f.growth_per_day == 0 ? FillForecast::State::Steady : FillForecast::State::NoSpaceInfo;
        return f;
    }
    f.free = *free;
    if (f.growth_per_day == 0) {
        f.state = FillForecast::State::Steady;
        return f;
    }
    // The free space was measured at `free_time`; it shrank by the growth since.
    const double since = now > free_time ? static_cast<double>(now - free_time) / static_cast<double>(kDay) : 0.0;
    const double left = static_cast<double>(*free) - f.growth_per_day * since;
    const double days_left = std::max(0.0, left / f.growth_per_day);
    if (days_left > kSteadyDays) {
        f.state = FillForecast::State::Steady;
        return f;
    }
    f.state = FillForecast::State::Fills;
    f.days = days_left;
    return f;
}

FillForecast forecast_for(const std::vector<RunRecord>& history, std::uint64_t now,
                          std::optional<std::uint64_t> free_now) {
    std::optional<std::uint64_t> free = free_now;
    std::uint64_t free_time = now;
    if (!free) {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->kind == RunKind::Backup && it->free_space > 0 && it->finished <= now) {
                free = it->free_space;
                free_time = it->finished;
                break;
            }
        }
    }
    return forecast_fill(size_points(history), free, free_time, now);
}

std::pair<std::string, long long> fill_time_phrase(double days) {
    if (days < 1) return {"within a day", 0};
    if (days < 14) return {"in about %1 days", std::llround(days)};
    if (days < 60) return {"in about %1 weeks", std::llround(days / 7)};
    if (days < 730) return {"in about %1 months", std::llround(days / 30.44)};
    if (days <= kSteadyDays) return {"in about %1 years", std::llround(days / 365.25)};
    return {"in more than 10 years", 0};
}

std::string fill_time_text(double days) {
    auto [phrase, n] = fill_time_phrase(days);
    const std::size_t at = phrase.find("%1");
    if (at != std::string::npos) phrase.replace(at, 2, std::to_string(n));
    return phrase;
}

std::string describe(const FillForecast& f) {
    switch (f.state) {
        case FillForecast::State::NoData: return "not enough history yet for a forecast";
        case FillForecast::State::Shrinking: return "the backup is getting smaller (" + size_text(-f.growth_per_day) + " a day)";
        case FillForecast::State::Steady:
            return "not filling up (grows " + size_text(f.growth_per_day) + " a day)";
        case FillForecast::State::NoSpaceInfo:
            return "grows " + size_text(f.growth_per_day) + " a day; the free space is not known";
        case FillForecast::State::Fills:
            return "the drive fills up " + fill_time_text(f.days) + " (grows " + size_text(f.growth_per_day) +
                   " a day, " + size_text(static_cast<double>(f.free)) + " free)";
    }
    return {};
}

const char* to_string(FillForecast::State state) {
    switch (state) {
        case FillForecast::State::NoData: return "no_data";
        case FillForecast::State::Shrinking: return "shrinking";
        case FillForecast::State::Steady: return "steady";
        case FillForecast::State::NoSpaceInfo: return "no_space_info";
        case FillForecast::State::Fills: return "fills";
    }
    return "no_data";
}

}  // namespace vaultik::jobs
