#include "vaultik/cli/console.hpp"

#include <cstdio>
#include <mutex>

#include "vaultik/util/error.hpp"

namespace vaultik::cli {

namespace {

std::mutex g_prompts_lock;
std::string g_no_prompts;  // why prompts are disabled; empty: they are not

}  // namespace

void disable_prompts(std::string why) {
    std::lock_guard<std::mutex> lock(g_prompts_lock);
    g_no_prompts = why.empty() ? std::string("nothing can be asked here") : std::move(why);
}

void check_prompts_allowed(std::string_view without_console) {
    std::lock_guard<std::mutex> lock(g_prompts_lock);
    if (g_no_prompts.empty()) return;
    throw Error(std::string(without_console) + " (" + g_no_prompts + ")");
}

// "Type ERASE DISK 3 to go on (anything else stops): " -> the question, for the error of a
// prompt that cannot be shown.
std::string prompt_question(std::string_view prompt) {
    std::string q(prompt);
    while (!q.empty() && (q.back() == ' ' || q.back() == ':' || q.back() == '\n')) q.pop_back();
    return "no console to answer \"" + q + "\"";
}

std::string format_bytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f %s", v, kUnits[unit]);
    }
    return buf;
}

std::string format_duration(double seconds) {
    char buf[32];
    if (seconds < 60) {
        std::snprintf(buf, sizeof buf, "%.1fs", seconds);
    } else {
        const auto total = static_cast<unsigned long long>(seconds);
        std::snprintf(buf, sizeof buf, "%llum%02llus", total / 60, total % 60);
    }
    return buf;
}

}  // namespace vaultik::cli
