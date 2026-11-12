#pragma once

// Linux: the mount table (/proc/self/mountinfo), for the volume functions in os_posix.cpp and the
// drive health in drive_health_posix.cpp.

#include <string>
#include <vector>

namespace vaultik::os::mounts {

struct Mount {
    unsigned major = 0;  // the device; for btrfs and the like the block device it lives on
    unsigned minor = 0;
    std::string root;   // the folder of the file system mounted here ("/" unless a bind mount)
    std::string point;  // where it is mounted
    std::string type;
    std::string source;  // "/dev/sda1", or a name such as "tmpfs"
};

// Every mount, in the order of the table (empty when it cannot be read).
std::vector<Mount> read_mounts();
// The mount `path` (absolute) is on: the latest (top-most) mount whose point holds it.
const Mount* mount_of(const std::vector<Mount>& mounts, const std::string& path);

}  // namespace vaultik::os::mounts
