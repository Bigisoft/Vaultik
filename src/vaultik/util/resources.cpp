#include "vaultik/util/resources.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "vaultik/util/error.hpp"
#include "vaultik/util/resources_detail.hpp"

namespace vaultik {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

// Token bucket allowing at most one second of burst.
class RateLimiter {
public:
    void set_rate(std::uint64_t rate) {
        std::lock_guard lock(mu_);
        rate_ = rate;
        tokens_ = 0;
        last_ = Clock::now();
    }

    void consume(std::size_t bytes) {
        std::unique_lock lock(mu_);
        if (rate_ == 0) return;
        const double rate = static_cast<double>(rate_);
        const auto now = Clock::now();
        tokens_ = std::min(tokens_ + std::chrono::duration<double>(now - last_).count() * rate, rate);
        last_ = now;
        tokens_ -= static_cast<double>(bytes);
        if (tokens_ >= 0) return;
        const std::chrono::duration<double> wait(-tokens_ / rate);
        lock.unlock();
        std::this_thread::sleep_for(wait);
    }

private:
    using Clock = std::chrono::steady_clock;
    std::mutex mu_;
    std::uint64_t rate_ = 0;
    double tokens_ = 0;
    Clock::time_point last_ = Clock::now();
};

ResourcePolicy g_policy;
RateLimiter g_read_limiter;
RateLimiter g_write_limiter;

}  // namespace

bool ResourcePolicy::is_default() const {
    return cpu == CpuPriority::Normal && io == IoPriority::Normal && memory == MemoryPriority::Normal &&
           cpu_limit_percent == 0 && max_memory == 0 && read_rate == 0 && write_rate == 0;
}

ResourcePolicy priority_preset(std::string_view name) {
    const std::string n = lower(name);
    ResourcePolicy p;
    if (n == "background") {
        p.cpu = CpuPriority::Idle;
        p.io = IoPriority::Background;
        p.memory = MemoryPriority::Low;
    } else if (n == "low") {
        p.cpu = CpuPriority::Low;
        p.io = IoPriority::Low;
        p.memory = MemoryPriority::Low;
    } else if (n == "normal") {
        // defaults
    } else if (n == "high") {
        p.cpu = CpuPriority::High;
    } else {
        throw Error("unknown priority \"" + std::string(name) + "\" (use background, low, normal or high)");
    }
    return p;
}

CpuPriority parse_cpu_priority(std::string_view text) {
    const std::string t = lower(text);
    if (t == "idle") return CpuPriority::Idle;
    if (t == "low") return CpuPriority::Low;
    if (t == "normal") return CpuPriority::Normal;
    if (t == "high") return CpuPriority::High;
    throw Error("unknown CPU priority \"" + std::string(text) + "\" (use idle, low, normal or high)");
}

IoPriority parse_io_priority(std::string_view text) {
    const std::string t = lower(text);
    if (t == "background") return IoPriority::Background;
    if (t == "low") return IoPriority::Low;
    if (t == "normal") return IoPriority::Normal;
    throw Error("unknown I/O priority \"" + std::string(text) + "\" (use background, low or normal)");
}

MemoryPriority parse_memory_priority(std::string_view text) {
    const std::string t = lower(text);
    if (t == "low") return MemoryPriority::Low;
    if (t == "normal") return MemoryPriority::Normal;
    throw Error("unknown memory priority \"" + std::string(text) + "\" (use low or normal)");
}

unsigned parse_percent(std::string_view text) {
    std::string_view t = text;
    if (t.ends_with('%')) t.remove_suffix(1);
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), value);
    if (ec != std::errc{} || end != t.data() + t.size() || value < 1 || value > 100)
        throw Error("invalid percentage \"" + std::string(text) + "\" (use 1-100)");
    return value;
}

std::uint64_t parse_size(std::string_view text) {
    std::string s = lower(text);
    if (s.ends_with("/s")) s.resize(s.size() - 2);
    std::size_t digits = 0;
    while (digits < s.size() && ((s[digits] >= '0' && s[digits] <= '9') || s[digits] == '.')) ++digits;
    double value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + digits, value);
    if (digits == 0 || ec != std::errc{} || end != s.data() + digits)
        throw Error("invalid size \"" + std::string(text) + "\" (examples: 512K, 64M, 1.5G)");

    std::string unit = s.substr(digits);
    if (unit.ends_with("ib")) {
        unit.resize(unit.size() - 2);
    } else if (unit.ends_with('b')) {
        unit.pop_back();
    }
    double mult = 1;
    if (unit == "k") {
        mult = 1024.0;
    } else if (unit == "m") {
        mult = 1024.0 * 1024;
    } else if (unit == "g") {
        mult = 1024.0 * 1024 * 1024;
    } else if (unit == "t") {
        mult = 1024.0 * 1024 * 1024 * 1024;
    } else if (!unit.empty()) {
        throw Error("invalid size unit in \"" + std::string(text) + "\" (use K, M, G or T)");
    }
    const double bytes = value * mult;
    if (!(bytes >= 1.0) || bytes > 1.8e19) throw Error("size out of range: " + std::string(text));
    return static_cast<std::uint64_t>(bytes);
}

const char* to_string(CpuPriority p) {
    switch (p) {
        case CpuPriority::Idle: return "idle";
        case CpuPriority::Low: return "low";
        case CpuPriority::Normal: return "normal";
        case CpuPriority::High: return "high";
    }
    return "?";
}

const char* to_string(IoPriority p) {
    switch (p) {
        case IoPriority::Background: return "background";
        case IoPriority::Low: return "low";
        case IoPriority::Normal: return "normal";
    }
    return "?";
}

const char* to_string(MemoryPriority p) {
    switch (p) {
        case MemoryPriority::Low: return "low";
        case MemoryPriority::Normal: return "normal";
    }
    return "?";
}

void apply_resource_policy(const ResourcePolicy& p) {
    if (p.cpu_limit_percent > 100) throw Error("CPU limit must be between 1 and 100 percent");
    if (p.max_memory != 0 && p.max_memory < kMinMemoryLimit) throw Error("memory limit must be at least 128M");
    detail::apply_platform_policy(p);
    g_read_limiter.set_rate(p.read_rate);
    g_write_limiter.set_rate(p.write_rate);
    g_policy = p;
}

const ResourcePolicy& current_resource_policy() {
    return g_policy;
}

void throttle_read(std::size_t bytes) {
    g_read_limiter.consume(bytes);
    detail::cpu_cap_tick();
}

void throttle_write(std::size_t bytes) {
    g_write_limiter.consume(bytes);
    detail::cpu_cap_tick();
}

}  // namespace vaultik
