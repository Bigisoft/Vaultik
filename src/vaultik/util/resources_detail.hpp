#pragma once

// Platform hooks behind resources.hpp. Internal: implemented in resources_win.cpp and
// resources_posix.cpp.

#include "vaultik/util/resources.hpp"

namespace vaultik::detail {

// Applies priorities and hard caps to the current process. Throws Error on failure.
void apply_platform_policy(const ResourcePolicy& policy);

// Called from the throttle points; enforces the CPU cap where the OS has no cheap native one.
void cpu_cap_tick();

// The CPU cap's duty cycle (Linux, resources_posix.cpp): how long a thread at a throttle point
// sleeps so the process's CPU time (all its threads together) stays at or below `share` cores of
// the wall-clock time. `cpu_seconds` were used in `wall_seconds` since the reference point. 0 when
// within the cap (or no cap); differences under 5 ms are not worth a sleep.
inline double cpu_cap_delay(double share, double wall_seconds, double cpu_seconds) {
    if (share <= 0.0) return 0.0;
    const double ahead = cpu_seconds / share - wall_seconds;
    return ahead > 0.005 ? ahead : 0.0;
}

}  // namespace vaultik::detail
