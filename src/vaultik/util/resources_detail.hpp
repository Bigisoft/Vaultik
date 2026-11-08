#pragma once

// Platform hooks behind resources.hpp. Internal: implemented in resources_win.cpp and
// resources_posix.cpp.

#include "vaultik/util/resources.hpp"

namespace vaultik::detail {

// Applies priorities and hard caps to the current process. Throws Error on failure.
void apply_platform_policy(const ResourcePolicy& policy);

// Called from the throttle points; enforces the CPU cap where the OS has no cheap native one.
void cpu_cap_tick();

}  // namespace vaultik::detail
