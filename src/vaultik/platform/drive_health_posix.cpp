#include "vaultik/platform/drive_health.hpp"

// Linux: the drives come from sysfs (/sys/block), a folder's drives from the mount table through
// partitions, device-mapper (LVM, LUKS), software RAID and loop devices down to the whole disks.
// The health comes from the drive itself: an NVMe drive's health log (Get Log Page 02h through
// NVME_IOCTL_ADMIN_CMD), an ATA drive's S.M.A.R.T. attribute table (SMART READ DATA and READ
// THRESHOLDS through SG_IO and ATA PASS-THROUGH (16), which SATA, most USB enclosures and the
// kernel's own translation understand), else "smartctl -j" (smartmontools). The kernel lets only
// root send these commands, so without root the health is unknown and the reason says so; only
// the temperature (hwmon) can be read by anyone. Nothing here writes to a drive.

#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vaultik/platform/mounts_posix.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::os {

namespace {

std::string real_path(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) return {};
    return buf;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

// A small sysfs file, without the surrounding white space (models are padded with spaces).
std::string read_small(const std::string& path) {
    try {
        const Bytes b = read_file(path, 4096);
        return trim(std::string(b.begin(), b.end()));
    } catch (const std::exception&) {
        return {};
    }
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    DirReader reader(dir);
    std::string name;
    FileInfo info;
    while (reader.next(name, info)) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

bool contains_folded(std::string text, std::string_view what) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text.find(what) != std::string::npos;
}

int disk_number(const std::string& sys) {
    unsigned major = 0;
    unsigned minor = 0;
    if (std::sscanf(read_small(join(sys, "dev")).c_str(), "%u:%u", &major, &minor) != 2) return -1;
    return static_cast<int>(::makedev(major, minor) & 0x7FFFFFFF);
}

std::vector<std::string> disks_below(const std::string& path, int depth);

// Adds the sysfs folders (/sys/devices/.../block/sda) of the whole disks a block device lies on:
// a partition's disk, the devices below device-mapper (LVM, LUKS) and software RAID, and the disks
// holding a loop device's file.
void whole_disks(const std::string& sys, std::vector<std::string>& out, int depth) {
    if (sys.empty() || depth > 8) return;
    if (exists(join(sys, "partition"))) {
        whole_disks(parent_of(sys), out, depth + 1);
        return;
    }
    const std::string slaves = join(sys, "slaves");
    const std::vector<std::string> below = entries(slaves);
    if (!below.empty()) {
        for (const std::string& name : below) whole_disks(real_path(join(slaves, name)), out, depth + 1);
        return;
    }
    const std::string name = base_name(sys);
    if (name.starts_with("loop")) {
        const std::string file = read_small(join(sys, "loop/backing_file"));
        if (file.starts_with("/"))
            for (const std::string& d : disks_below(file, depth + 1))
                if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
        return;
    }
    if (name.starts_with("ram") || name.starts_with("zram")) return;  // memory, not a drive
    if (std::find(out.begin(), out.end(), sys) == out.end()) out.push_back(sys);
}

// The whole disks the file system holding `path` lies on. Empty for network shares and file
// systems without a device (tmpfs, overlay, ZFS datasets). Btrfs: the device the mount table
// names (one of several for a file system spanning disks).
std::vector<std::string> disks_below(const std::string& path, int depth) {
    std::vector<std::string> out;
    const std::vector<mounts::Mount> table = mounts::read_mounts();
    const mounts::Mount* m = mounts::mount_of(table, absolute_path(path));
    if (m == nullptr || m->major == 0) return out;
    whole_disks(real_path("/sys/dev/block/" + std::to_string(m->major) + ":" + std::to_string(m->minor)), out, depth);
    return out;
}

// The model, serial, size, bus and kind of the disk at `sys`.
DriveHealth describe(const std::string& sys) {
    DriveHealth d;
    const std::string name = base_name(sys);
    d.device = "/dev/" + name;
    d.disk = disk_number(sys);
    const std::string dev = join(sys, "device");
    const std::string vendor = read_small(join(dev, "vendor"));
    d.model = read_small(join(dev, "model"));
    if (d.model.empty()) d.model = read_small(join(dev, "name"));  // a memory card ("SD64G")
    // SCSI and USB disks split the name; "ATA" is the kernel's translation, not a maker.
    if (!vendor.empty() && vendor != "ATA" && !vendor.starts_with("0x") && !d.model.starts_with(vendor))
        d.model = vendor + " " + d.model;
    d.serial = read_small(join(dev, "serial"));
    // What udev found (an ATA drive's serial number is not in sysfs), readable by anyone.
    std::string udev;
    try {
        const std::string dev_no = read_small(join(sys, "dev"));
        const Bytes b = read_file("/run/udev/data/b" + dev_no, 256u << 10);
        udev.assign(b.begin(), b.end());
    } catch (const std::exception&) {
    }
    const auto udev_value = [&](std::string_view key) -> std::string {
        const std::string prefix = "E:" + std::string(key) + "=";
        std::size_t pos = 0;
        while (pos < udev.size()) {
            std::size_t end = udev.find('\n', pos);
            if (end == std::string::npos) end = udev.size();
            if (udev.compare(pos, prefix.size(), prefix) == 0) return udev.substr(pos + prefix.size(), end - pos - prefix.size());
            pos = end + 1;
        }
        return {};
    };
    if (d.serial.empty()) d.serial = udev_value("ID_SERIAL_SHORT");
    if (d.model.empty()) {
        d.model = udev_value("ID_MODEL");
        std::replace(d.model.begin(), d.model.end(), '_', ' ');
    }
    d.serial = trim(d.serial);
    d.size = std::strtoull(read_small(join(sys, "size")).c_str(), nullptr, 10) * 512;  // always 512-byte units

    if (name.starts_with("nvme")) d.bus = "NVMe";
    else if (sys.find("/usb") != std::string::npos) d.bus = "USB";
    else if (name.starts_with("mmcblk")) d.bus = "SD";
    else if (name.starts_with("vd") || sys.find("/virtio") != std::string::npos) d.bus = "Virtio";
    else if (sys.find("/ata") != std::string::npos) d.bus = "SATA";
    else d.bus = "SCSI";
    if (d.bus == "NVMe") d.media = "SSD";
    else if (d.bus != "SD") d.media = read_small(join(sys, "queue/rotational")) == "1" ? "HDD" : "SSD";
    return d;
}

// A disk of a virtual machine (Hyper-V and WSL, QEMU/KVM, VMware, VirtualBox): no drive to ask.
bool is_virtual(const DriveHealth& d, const std::string& sys) {
    return d.bus == "Virtio" || contains_folded(sys, "/vmbus") || contains_folded(sys, "/msft1000") ||
           contains_folded(d.model, "virtual") || d.model.starts_with("Msft ") || d.model.starts_with("QEMU") ||
           d.model.starts_with("VMware") || d.model.starts_with("VBOX");
}

// The temperature the kernel's hwmon driver reports (nvme, drivetemp), which anyone may read.
std::optional<int> hwmon_temperature(const std::string& dev) {
    for (const std::string& base : {dev, join(dev, "hwmon")}) {
        for (const std::string& name : entries(base)) {
            if (!name.starts_with("hwmon") || name == "hwmon") continue;
            const std::string value = read_small(join(join(base, name), "temp1_input"));
            if (value.empty()) continue;
            const long milli = std::strtol(value.c_str(), nullptr, 10);
            if (milli > 0 && milli < 150000) return static_cast<int>((milli + 500) / 1000);
        }
    }
    return std::nullopt;
}

int open_device(const std::string& device, int& err) {
    // Read-only: the commands below only read, and a drive is never opened for writing here.
    const int fd = ::open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    err = fd < 0 ? errno : 0;
    return fd;
}

// The NVMe SMART / Health Information log (512 bytes) through the controller's device.
bool nvme_log(const std::string& device, std::uint8_t* out512, int& err) {
    const int fd = open_device(device, err);
    if (fd < 0) return false;
    std::memset(out512, 0, 512);
    struct nvme_admin_cmd cmd{};
    cmd.opcode = 0x02;       // Get Log Page
    cmd.nsid = 0xFFFFFFFFu;  // the controller as a whole
    cmd.addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(out512));
    cmd.data_len = 512;
    cmd.cdw10 = (127u << 16) | 0x02;  // 128 dwords (counted from 0), log page 02h
    cmd.timeout_ms = 5000;
    const int r = ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    err = r < 0 ? errno : 0;
    ::close(fd);
    return r == 0;  // above 0: the drive's own error status
}

// One ATA S.M.A.R.T. command answering a 512-byte sector (SMART READ DATA 0xD0, READ THRESHOLDS
// 0xD1) through the SCSI-ATA translation's ATA PASS-THROUGH (16).
bool ata_smart(int fd, std::uint8_t feature, std::uint8_t* out512, int& err) {
    std::uint8_t cdb[16] = {};
    cdb[0] = 0x85;    // ATA PASS-THROUGH (16)
    cdb[1] = 4 << 1;  // protocol: PIO data-in
    cdb[2] = 0x0E;    // to the host, length in sectors, from the sector count
    cdb[4] = feature;
    cdb[6] = 1;       // sector count
    cdb[8] = 1;       // LBA low
    cdb[10] = 0x4F;   // LBA mid and high: the S.M.A.R.T. signature
    cdb[12] = 0xC2;
    cdb[13] = 0xA0;   // device
    cdb[14] = 0xB0;   // SMART
    std::uint8_t sense[32] = {};
    std::memset(out512, 0, 512);
    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.mx_sb_len = sizeof sense;
    io.dxfer_len = 512;
    io.dxferp = out512;
    io.cmdp = cdb;
    io.sbp = sense;
    io.timeout = 5000;  // ms
    if (::ioctl(fd, SG_IO, &io) < 0) {
        err = errno;
        return false;
    }
    err = 0;
    return (io.info & SG_INFO_OK_MASK) == SG_INFO_OK && io.resid == 0;
}

// An ATA drive's attribute table, with its thresholds.
bool ata_table(DriveHealth& d, int& err) {
    const int fd = open_device(d.device, err);
    if (fd < 0) return false;
    std::uint8_t values[512];
    std::uint8_t thresholds[512];
    bool ok = ata_smart(fd, 0xD0, values, err);
    if (ok) {
        int ignored = 0;
        const bool have_thresholds = ata_smart(fd, 0xD1, thresholds, ignored);
        d.smart = parse_smart(values, have_thresholds ? thresholds : nullptr);
        ok = !d.smart.empty();
    }
    ::close(fd);
    return ok;
}

// "smartctl -j -a" (smartmontools), which knows the vendor commands of many USB bridges and RAID
// controllers. False with `why` when it is missing or could not read the drive.
bool smartctl(DriveHealth& d, std::string& why) {
    std::string output;
    CommandOptions options;
    options.output = &output;
    options.timeout = std::chrono::seconds(60);
    try {
        // Its exit code is a set of flags (some only about the drive's past), so the JSON tells.
        run_command({"smartctl", "-j", "-a", d.device}, options);
    } catch (const std::exception&) {
        why = "smartctl (smartmontools) is not installed";
        return false;
    }
    // The answer is one JSON object; anything printed on standard error is around it.
    const std::size_t from = output.find('{');
    const std::size_t to = output.rfind('}');
    if (from == std::string::npos || to == std::string::npos || to < from) {
        why = "smartctl gave no answer";
        return false;
    }
    std::string problem;
    if (apply_smartctl_json(d, std::string_view(output).substr(from, to - from + 1), &problem)) return true;
    why = problem.empty() ? "smartctl could not read it" : "smartctl: " + problem;
    return false;
}

constexpr const char* kNeedsRoot = "reading S.M.A.R.T. data needs root (sudo vaultik drives)";

// The drive's health, as far as this process may ask for it.
void read_health(DriveHealth& d, const std::string& sys) {
    const std::string dev = join(sys, "device");
    if (const auto t = hwmon_temperature(dev)) d.temperature = t;
    if (is_virtual(d, sys)) {
        d.unknown_reason = "a virtual disk, which has no S.M.A.R.T. data";
        return;
    }
    if (d.bus == "SD") {
        d.unknown_reason = "memory cards do not report their health";
        return;
    }
    int err = 0;
    if (d.bus == "NVMe") {
        // The controller ("nvme0") takes the admin commands.
        const std::string controller = base_name(real_path(dev));
        std::uint8_t log[512];
        if (nvme_log("/dev/" + (controller.starts_with("nvme") ? controller : base_name(d.device)), log, err)) {
            d.status = DriveHealth::Status::Healthy;
            apply_nvme_log(d, log, sizeof log);
            return;
        }
    } else if (ata_table(d, err)) {
        d.status = DriveHealth::Status::Healthy;
        apply_smart(d);
        return;
    }
    const bool denied = err == EACCES || err == EPERM;
    if (denied && ::geteuid() != 0) {
        d.unknown_reason = kNeedsRoot;  // smartctl would be refused too
        return;
    }
    std::string why;
    if (smartctl(d, why)) return;
    if (denied) d.unknown_reason = kNeedsRoot;
    else if (d.bus == "USB") d.unknown_reason = "the drive's enclosure does not pass S.M.A.R.T. data on (" + why + ")";
    else d.unknown_reason = "the drive did not hand out S.M.A.R.T. data (" + why + ")";
}

DriveHealth drive_at(const std::string& sys) {
    DriveHealth d = describe(sys);
    read_health(d, sys);
    return d;
}

}  // namespace

std::vector<DriveHealth> drive_health(const std::string& path) {
    std::vector<DriveHealth> out;
    try {
        for (const std::string& sys : disks_below(path, 0)) out.push_back(drive_at(sys));
    } catch (const std::exception&) {
        // best effort
    }
    return out;
}

std::vector<DriveHealth> all_drives() {
    std::vector<DriveHealth> out;
    try {
        for (const std::string& name : entries("/sys/block")) {
            // Not drives: loop and memory devices, device-mapper and RAID volumes (their disks are
            // listed), optical and floppy drives, network block devices, a memory card's boot and
            // replay-protected areas.
            static constexpr std::string_view kSkip[] = {"loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "mtdblock"};
            if (std::any_of(std::begin(kSkip), std::end(kSkip), [&](std::string_view p) { return name.starts_with(p); }) ||
                name.find("boot") != std::string::npos || name.find("rpmb") != std::string::npos)
                continue;
            const std::string sys = real_path(join("/sys/block", name));
            if (sys.empty() || read_small(join(sys, "size")) == "0") continue;  // an empty card reader
            out.push_back(drive_at(sys));
        }
    } catch (const std::exception&) {
        // best effort
    }
    return out;
}

std::vector<int> disk_numbers(const std::string& path) {
    std::vector<int> out;
    try {
        for (const std::string& sys : disks_below(path, 0))
            if (const int n = disk_number(sys); n >= 0) out.push_back(n);
    } catch (const std::exception&) {
    }
    return out;
}

}  // namespace vaultik::os
