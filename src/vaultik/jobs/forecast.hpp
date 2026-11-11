#pragma once

// When the backup drive fills up: "at this rate, the drive is full in about 6 weeks". Every
// backup records the stored size of its whole destination (RunRecord::backup_size) and, for a
// folder destination, the space left on its drive (RunRecord::free_space). From those a robust
// trend gives the growth per day, and the free space divided by it the time left.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// One known size of the destination (os::now() units, bytes).
struct SizePoint {
    std::uint64_t time = 0;
    std::uint64_t size = 0;
};

// The sizes the history's backups recorded, oldest first (runs that did not record one are left
// out).
std::vector<SizePoint> size_points(const std::vector<RunRecord>& history);

struct FillForecast {
    enum class State {
        NoData,       // too little history: fewer than 3 days with a size, or less than 3 days apart
        Shrinking,    // the backup gets smaller (old versions are removed faster than new data comes)
        Steady,       // it grows so slowly that the drive will not fill within 10 years
        NoSpaceInfo,  // it grows, but the free space is not known (cloud storage)
        Fills,        // it grows and fills the drive in `days`
    };
    State state = State::NoData;
    double growth_per_day = 0;  // bytes per day; negative when shrinking
    std::uint64_t free = 0;     // the free space the forecast used
    double days = 0;            // Fills: days from `now` until the drive is full (0: full already)
};

// Warn when the drive fills up sooner than this.
constexpr int kFillWarnDays = 30;
// The history the trend is fitted to.
constexpr std::uint64_t kForecastWindow = 90ull * 864'000'000'000ull;

// The trend: the points of the last kForecastWindow before `now`, one per day (the day's newest),
// and only those after the last big drop (a fall of more than 10% between two days: a cleanup or
// a manual prune, after which the old growth says nothing). Its growth per day is the Theil-Sen
// estimate, the median of the slopes between every two points, which ignores the small drops of
// the regular cleanup as well as single odd days, while still following the net growth of a
// backup whose cleanup keeps it about the same size. `free` (bytes, measured at `free_time`) is
// what is left; empty when not known.
FillForecast forecast_fill(const std::vector<SizePoint>& points, std::optional<std::uint64_t> free,
                           std::uint64_t free_time, std::uint64_t now);
// The same from a job's history (oldest first): the free space is the one the newest backup that
// recorded it saw, unless `free_now` (measured now) is given.
FillForecast forecast_for(const std::vector<RunRecord>& history, std::uint64_t now,
                          std::optional<std::uint64_t> free_now = std::nullopt);

// Whether the forecast warrants a warning (fills within kFillWarnDays).
inline bool fill_warning(const FillForecast& f) {
    return f.state == FillForecast::State::Fills && f.days < kFillWarnDays;
}

// The time left in words, as a fixed English phrase with %1 for the number and that number:
// {"within a day", 0}, {"in about %1 days", 9}, {"in about %1 weeks", 6}, {"in about %1 months", 5},
// {"in about %1 years", 2}, {"in more than 10 years", 0}.
std::pair<std::string, long long> fill_time_phrase(double days);
// "in about 6 weeks" (English).
std::string fill_time_text(double days);
// One line in English for the command line: "fills up in about 6 weeks (grows 1.2 GB a day, 50.0 GB free)".
std::string describe(const FillForecast& f);
const char* to_string(FillForecast::State state);

}  // namespace vaultik::jobs
