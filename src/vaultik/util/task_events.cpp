#include "vaultik/util/task_events.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

namespace vaultik {

namespace {

std::uint64_t to_count(const nlohmann::json& v) {
    if (v.is_number_unsigned()) return v.get<std::uint64_t>();
    if (v.is_number_integer()) {
        const auto i = v.get<std::int64_t>();
        return i < 0 ? 0 : static_cast<std::uint64_t>(i);
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return d > 0 && std::isfinite(d) ? static_cast<std::uint64_t>(d) : 0;
    }
    return 0;
}

double to_double(const nlohmann::json& j, const char* key, double fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const double d = it->get<double>();
    return std::isfinite(d) ? d : fallback;
}

std::string to_text(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> to_list(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

}  // namespace

TaskEvent parse_task_event(std::string_view line) {
    TaskEvent e;
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return e;
    const std::string type = to_text(j, "event");
    if (type == "progress") {
        e.type = TaskEvent::Type::Progress;
        const auto total = j.find("total");
        const auto done = j.find("done");
        if (total != j.end() && done != j.end() && total->is_number() && done->is_number()) {
            e.total = to_count(*total);
            e.done = to_count(*done);
            e.has_total = e.total > 0;
            if (e.done > e.total) e.done = e.total;
        }
        e.speed = to_double(j, "speed", -1);
        if (e.speed <= 0) e.speed = -1;
        e.seconds_left = to_double(j, "seconds_left", -1);
        if (e.seconds_left < 0) e.seconds_left = -1;
        e.pass = static_cast<int>(to_double(j, "pass", 0));
        e.passes = static_cast<int>(to_double(j, "passes", 0));
        const auto v = j.find("verifying");
        e.verifying = v != j.end() && v->is_boolean() && v->get<bool>();
        e.text = to_text(j, "current");
    } else if (type == "step") {
        e.type = TaskEvent::Type::Step;
        e.text = to_text(j, "message");
    } else if (type == "warning") {
        e.type = TaskEvent::Type::Warning;
        e.text = to_text(j, "message");
    } else if (type == "in_use") {
        e.type = TaskEvent::Type::InUse;
        e.text = to_text(j, "message");
        e.volumes = to_list(j, "volumes");
    } else if (type == "done") {
        e.type = TaskEvent::Type::Done;
        e.status = to_text(j, "status");
        e.text = to_text(j, "message");
        e.volumes = to_list(j, "volumes");
    }
    return e;
}

void RateEstimator::add(std::uint64_t done, std::uint64_t total, double now) {
    if (total != total_ || (!samples_.empty() && done < samples_.back().done)) {
        samples_.clear();
        total_ = total;
    }
    samples_.push_back({now, done});
    // Keeps the samples of the last 30 seconds (and at least two).
    std::size_t drop = 0;
    while (samples_.size() - drop > 2 && now - samples_[drop].at > 30) ++drop;
    samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(drop));
}

double RateEstimator::speed() const {
    if (samples_.size() < 2) return -1;
    const Sample& a = samples_.front();
    const Sample& b = samples_.back();
    const double secs = b.at - a.at;
    if (secs < 1 || b.done <= a.done) return -1;
    return static_cast<double>(b.done - a.done) / secs;
}

double RateEstimator::seconds_left() const {
    const double s = speed();
    if (s <= 0 || samples_.empty()) return -1;
    const std::uint64_t done = samples_.back().done;
    return done >= total_ ? 0 : static_cast<double>(total_ - done) / s;
}

TimeLeft split_time_left(double seconds) {
    TimeLeft t;
    if (!(seconds >= 0) || !std::isfinite(seconds)) return t;
    t.known = true;
    if (seconds < 60) {
        t.under_a_minute = true;
        return t;
    }
    const auto minutes = static_cast<std::int64_t>(std::ceil(seconds / 60.0));
    t.hours = minutes / 60;
    t.minutes = static_cast<int>(minutes % 60);
    return t;
}

}  // namespace vaultik
