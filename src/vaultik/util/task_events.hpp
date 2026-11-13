#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The engine's --json event lines as the app shows them for a task that runs with administrator
// rights (erasing, setting up a disk, restoring a disk image, making a recovery drive). The
// fields are the ones the engine writes and the app's Runner reads: "progress" (done, total,
// speed, seconds_left, pass, passes, verifying, current), "step" (message), "warning",
// "in_use" (volumes) and "done" (status, message, volumes).

namespace vaultik {

struct TaskEvent {
    enum class Type { Unknown, Progress, Step, Warning, InUse, Done };
    Type type = Type::Unknown;
    // Progress: how far it is, when it tells (has_total).
    bool has_total = false;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    double speed = -1;         // bytes per second; -1 when not given
    double seconds_left = -1;  // -1 when not given or not known yet
    int pass = 0;              // 1-based; 0 when the task has no passes
    int passes = 0;
    bool verifying = false;
    std::string text;    // progress: "current"; step, warning, in_use, done: "message"
    std::string status;  // done: success, failed, cancelled or in_use
    std::vector<std::string> volumes;  // in_use and done with status in_use
};

// Never throws: a line that is not such an event comes back as Type::Unknown.
TaskEvent parse_task_event(std::string_view line);

// Speed and time left for progress events that do not carry them (a disk image restore): an
// average over about the last half minute, started again when the total changes.
class RateEstimator {
public:
    // `now` in seconds on any steady clock.
    void add(std::uint64_t done, std::uint64_t total, double now);
    double speed() const;         // bytes per second; -1 until known
    double seconds_left() const;  // -1 until known

private:
    struct Sample {
        double at;
        std::uint64_t done;
    };
    std::vector<Sample> samples_;
    std::uint64_t total_ = 0;
};

// A time left as the app shows it: "33 h 06 min left", "12 min left", "less than a minute left".
struct TimeLeft {
    bool known = false;
    bool under_a_minute = false;
    std::int64_t hours = 0;
    int minutes = 0;  // 0-59
};
// Rounded up to whole minutes (the last minute shows as "1 min" until under a minute is left).
TimeLeft split_time_left(double seconds);

}  // namespace vaultik
