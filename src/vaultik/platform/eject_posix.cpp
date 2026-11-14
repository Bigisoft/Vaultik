// Linux: ejecting a backup drive through udisks (the desktop's own way, no root needed for a
// drive the desktop mounted): unmount it, then power it off so it can be unplugged.

#include <unistd.h>

#include "vaultik/platform/mounts_posix.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::os {

std::string eject_drive(const std::string& root) {
    const std::vector<mounts::Mount> all = mounts::read_mounts();
    const mounts::Mount* m = mounts::mount_of(all, mounts::resolved_path(root));
    if (m == nullptr || m->point == "/" || m->point == "/boot" || m->point == "/home" || m->point == "/usr" ||
        !m->source.starts_with("/dev/"))
        return "the drive " + root + " is not a drive that can be ejected";
    const std::string device = m->source;
    ::sync();
    std::string output;
    try {
        if (run_command({"udisksctl", "unmount", "--block-device", device, "--no-user-interaction"}, &output) != 0)
            return "the drive " + root + " cannot be unmounted now (a program may still use it): " + output;
        run_command({"udisksctl", "power-off", "--block-device", device, "--no-user-interaction"}, &output);
    } catch (const std::exception&) {
        return "udisksctl is not installed, so Vaultik cannot eject the drive; eject it from the file manager";
    }
    return {};
}

}  // namespace vaultik::os
