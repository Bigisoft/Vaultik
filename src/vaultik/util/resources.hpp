#pragma once

// Process-wide resource policy: how much of the machine a Vaultik run may use.
//
// Priorities decide who wins when the PC is busy (Vaultik yields to the user's programs).
// Limits are hard caps that apply even when the PC is otherwise idle.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vaultik {

enum class CpuPriority { Idle, Low, Normal, High };
enum class IoPriority { Background, Low, Normal };
enum class MemoryPriority { Low, Normal };

struct ResourcePolicy {
    CpuPriority cpu = CpuPriority::Normal;
    IoPriority io = IoPriority::Normal;
    MemoryPriority memory = MemoryPriority::Normal;
    unsigned cpu_limit_percent = 0;  // 1-100 of the machine's total CPU time, 0 = no cap
    std::uint64_t max_memory = 0;    // committed bytes, 0 = no cap
    std::uint64_t read_rate = 0;     // bytes per second, 0 = unlimited
    std::uint64_t write_rate = 0;    // bytes per second, 0 = unlimited

    bool is_default() const;
};

// Smallest accepted memory cap; below this the chunker and pack buffers do not fit.
constexpr std::uint64_t kMinMemoryLimit = 128ull << 20;

// "background", "low", "normal" or "high". Throws Error for anything else.
ResourcePolicy priority_preset(std::string_view name);
CpuPriority parse_cpu_priority(std::string_view text);    // idle, low, normal, high
IoPriority parse_io_priority(std::string_view text);      // background, low, normal
MemoryPriority parse_memory_priority(std::string_view text);  // low, normal
unsigned parse_percent(std::string_view text);            // "50" or "50%", 1-100
// "1048576", "512K", "64M", "1.5G", "2T"; binary units, optional "B"/"iB" and "/s".
std::uint64_t parse_size(std::string_view text);

const char* to_string(CpuPriority p);
const char* to_string(IoPriority p);
const char* to_string(MemoryPriority p);

// Applies the policy to the current process. Call once, early. Throws Error on failure.
void apply_resource_policy(const ResourcePolicy& policy);
const ResourcePolicy& current_resource_policy();

// Applies the I/O priority to a file handle Vaultik opened. Best effort: some file
// systems (for example network shares) do not support priority hints.
void apply_io_priority(void* handle);

// Account for bytes read or written; sleeps as needed to honor the rate limits.
void throttle_read(std::size_t bytes);
void throttle_write(std::size_t bytes);

}  // namespace vaultik
