// Disk tools on Linux: the disks from sysfs, erasing through the block device (opened
// exclusively, so the kernel refuses while anything has it mounted or in use), and a new disk
// through sfdisk and mkfs. Writing to a disk needs root.

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "vaultik/image/disk_tools.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/mounts_posix.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::image {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    return s.substr(b);
}

std::string read_small(const std::string& path) {
    try {
        const Bytes b = os::read_file(path, 256u << 10);
        return trim(std::string(b.begin(), b.end()));
    } catch (const std::exception&) {
        return {};
    }
}

std::string real_path(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) return {};
    return buf;
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    try {
        os::DirReader reader(dir);
        std::string name;
        os::FileInfo info;
        while (reader.next(name, info)) out.push_back(name);
    } catch (const std::exception&) {
    }
    std::sort(out.begin(), out.end());
    return out;
}

// "8:16" of a sysfs block folder.
bool dev_numbers(const std::string& sys, unsigned& major, unsigned& minor) {
    return std::sscanf(read_small(os::join(sys, "dev")).c_str(), "%u:%u", &major, &minor) == 2;
}

// What udev recorded for a block device (readable by anyone), as KEY -> value.
std::string udev_value(const std::string& sys, const std::string& key) {
    const std::string data = read_small("/run/udev/data/b" + read_small(os::join(sys, "dev")));
    const std::string prefix = "E:" + key + "=";
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        if (data.compare(pos, prefix.size(), prefix) == 0) return data.substr(pos + prefix.size(), end - pos - prefix.size());
        pos = end + 1;
    }
    return {};
}

std::vector<std::string> active_swaps() {
    std::vector<std::string> out;
    const std::string text = read_small("/proc/swaps");
    std::size_t pos = text.find('\n');  // past the heading
    while (pos != std::string::npos && pos + 1 < text.size()) {
        const std::size_t end = text.find('\n', pos + 1);
        const std::string line = text.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        const std::size_t space = line.find_first_of(" \t");
        if (space != std::string::npos) out.push_back(real_path(line.substr(0, space)));
        pos = end;
    }
    return out;
}

// How a block device (a partition, or a whole disk with a file system on it) is used now: a
// volume entry whose filesystem is "swap" or "holder" (LVM, RAID, encryption on top), or with the
// mount point set; nothing when it is not used.
std::optional<DiskVolume> use_of(const std::string& sys, const std::string& name,
                                 const std::vector<os::mounts::Mount>& mounts, const std::vector<std::string>& swaps) {
    DiskVolume v;
    v.name = "/dev/" + name;
    unsigned major = 0, minor = 0;
    if (!dev_numbers(sys, major, minor)) return std::nullopt;
    for (const os::mounts::Mount& m : mounts) {
        if (m.major != major || m.minor != minor) continue;
        v.mount = m.point;
        v.filesystem = m.type;
        return v;
    }
    if (std::find(swaps.begin(), swaps.end(), real_path(v.name)) != swaps.end()) {
        v.filesystem = "swap";
        return v;
    }
    if (!entries(os::join(sys, "holders")).empty()) {
        v.filesystem = "holder";
        return v;
    }
    return std::nullopt;
}

std::string find_tool(const std::string& name) {
    std::vector<std::string> dirs = {"/usr/sbin", "/sbin", "/usr/bin", "/bin", "/usr/local/sbin"};
    const std::string path = os::get_env("PATH");
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t colon = path.find(':', at);
        const std::string dir = path.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (dir.starts_with("/")) dirs.push_back(dir);
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    for (const std::string& dir : dirs) {
        const std::string full = os::join(dir, name);
        if (::access(full.c_str(), X_OK) == 0) return full;
    }
    return {};
}

std::string need_tool(const std::string& name, const std::string& package) {
    const std::string full = find_tool(name);
    if (full.empty()) throw Error(name + " was not found; install " + package);
    return full;
}

void run_tool(const std::vector<std::string>& argv, const std::string& what) {
    std::string output;
    const int code = os::run_command(argv, &output);
    if (code != 0) throw Error(what + " failed (exit code " + std::to_string(code) + "): " + trim(output));
}

// A disk, partition or plain file, written with pwrite.
class FdDevice : public WipeDevice {
public:
    FdDevice(int fd, std::uint64_t size, std::uint32_t block, std::string what)
        : fd_(fd), size_(size), block_(block), what_(std::move(what)) {}
    ~FdDevice() override { ::close(fd_); }
    std::uint64_t size() const override { return size_; }
    std::uint32_t block_size() const override { return block_; }
    void write_at(std::uint64_t offset, ByteSpan data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0)
                throw Error("writing " + what_ + " at byte " + std::to_string(offset + done) + ": " +
                            (n < 0 ? std::strerror(errno) : "nothing written"));
            done += static_cast<std::size_t>(n);
        }
        // Written out as it goes, so the page cache does not fill with the whole disk.
        ::sync_file_range(fd_, static_cast<off_t>(offset), static_cast<off_t>(data.size()), SYNC_FILE_RANGE_WRITE);
        ::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(data.size()), POSIX_FADV_DONTNEED);
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        // Not from the cache: what the drive itself holds.
        ::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_DONTNEED);
        std::size_t done = 0;
        while (done < length) {
            const ssize_t n = ::pread(fd_, dst + done, length - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error("reading " + what_ + " at byte " + std::to_string(offset + done) + ": cut short");
            done += static_cast<std::size_t>(n);
        }
    }
    void flush() override {
        if (::fsync(fd_) != 0) throw Error("writing " + what_ + ": " + std::strerror(errno));
    }

private:
    int fd_;
    std::uint64_t size_;
    std::uint32_t block_;
    std::string what_;
};

// A block device opened exclusively: the kernel refuses while it, or a partition of it, is
// mounted, used as swap or by device-mapper.
std::unique_ptr<WipeDevice> open_block_device(const std::string& device) {
    const int fd = ::open(device.c_str(), O_RDWR | O_EXCL | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (e == EBUSY) throw Error(device + " is in use (mounted, swap, LVM or RAID); stop using it first");
        if (e == EACCES || e == EPERM) throw Error("opening " + device + " for writing needs root (sudo)");
        throw Error("cannot open " + device + ": " + std::strerror(e));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode)) {
        ::close(fd);
        throw Error(device + " is not a disk");
    }
    std::uint64_t size = 0;
    int sector = 512;
    if (::ioctl(fd, BLKGETSIZE64, &size) != 0 || ::ioctl(fd, BLKSSZGET, &sector) != 0 || sector <= 0) {
        ::close(fd);
        throw Error("cannot read the size of " + device);
    }
    return std::make_unique<FdDevice>(fd, size, static_cast<std::uint32_t>(sector), device);
}

class PosixFillFile : public FillFile {
public:
    explicit PosixFillFile(int fd) : fd_(fd) {}
    ~PosixFillFile() override { ::close(fd_); }
    FillResult write_at(std::uint64_t offset, ByteSpan data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == ENOSPC || errno == EDQUOT)) return FillResult::Full;
            if (n < 0 && errno == EFBIG) return FillResult::TooLarge;
            if (n <= 0) throw Error(std::string("writing the free space: ") + (n < 0 ? std::strerror(errno) : "nothing written"));
            done += static_cast<std::size_t>(n);
        }
        return FillResult::Written;
    }
    void flush() override {
        if (::fsync(fd_) != 0 && errno != ENOSPC) throw Error(std::string("writing the free space: ") + std::strerror(errno));
    }

private:
    int fd_;
};

}  // namespace

std::vector<DiskInfo> list_tool_disks() {
    std::vector<DiskInfo> out;
    const std::vector<os::mounts::Mount> mounts = os::mounts::read_mounts();
    const std::vector<std::string> swaps = active_swaps();
    const std::vector<int> root = os::disk_numbers("/");
    for (const std::string& name : entries("/sys/block")) {
        // Not disks: memory, device-mapper and RAID volumes (their disks are listed), optical and
        // floppy drives, network block devices.
        static constexpr std::string_view kSkip[] = {"ram", "zram", "dm-", "md", "sr", "fd", "nbd", "mtdblock"};
        if (std::any_of(std::begin(kSkip), std::end(kSkip), [&](std::string_view p) { return name.starts_with(p); }))
            continue;
        const std::string sys = real_path("/sys/block/" + name);
        DiskInfo d;
        d.size = std::strtoull(read_small(os::join(sys, "size")).c_str(), nullptr, 10) * 512;
        if (d.size == 0) continue;  // an empty card reader, a loop device without a file
        unsigned major = 0, minor = 0;
        if (!dev_numbers(sys, major, minor)) continue;
        d.number = static_cast<int>(::makedev(major, minor) & 0x7FFFFFFF);
        d.path = "/dev/" + name;
        d.sector_size = static_cast<std::uint32_t>(std::max(512L, std::strtol(read_small(os::join(sys, "queue/logical_block_size")).c_str(), nullptr, 10)));
        d.physical_sector_size = static_cast<std::uint32_t>(
            std::max<long>(d.sector_size, std::strtol(read_small(os::join(sys, "queue/physical_block_size")).c_str(), nullptr, 10)));
        d.removable = read_small(os::join(sys, "removable")) == "1";
        const std::string dev = os::join(sys, "device");
        const std::string vendor = read_small(os::join(dev, "vendor"));
        d.model = read_small(os::join(dev, "model"));
        if (d.model.empty()) d.model = read_small(os::join(dev, "name"));
        if (!vendor.empty() && vendor != "ATA" && !vendor.starts_with("0x") && !d.model.starts_with(vendor))
            d.model = vendor + " " + d.model;
        d.serial = read_small(os::join(dev, "serial"));
        if (d.serial.empty()) d.serial = udev_value(sys, "ID_SERIAL_SHORT");
        if (name.starts_with("loop")) {
            d.bus = "Loop";
            d.model = "Loop device (" + read_small(os::join(sys, "loop/backing_file")) + ")";
        } else if (name.starts_with("nvme")) {
            d.bus = "NVMe";
        } else if (sys.find("/usb") != std::string::npos) {
            d.bus = "USB";
        } else if (name.starts_with("mmcblk")) {
            d.bus = "SD";
        } else if (name.starts_with("vd") || sys.find("/virtio") != std::string::npos) {
            d.bus = "Virtual";
        } else if (sys.find("/ata") != std::string::npos) {
            d.bus = "SATA";
        } else {
            d.bus = "SCSI";
        }
        if (d.bus == "NVMe" || d.bus == "SD") d.media = "SSD";
        else if (d.bus != "Loop") d.media = read_small(os::join(sys, "queue/rotational")) == "1" ? "HDD" : "SSD";
        const std::string table = udev_value(sys, "ID_PART_TABLE_TYPE");
        std::string uuid = udev_value(sys, "ID_PART_TABLE_UUID");
        std::transform(uuid.begin(), uuid.end(), uuid.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (table == "gpt") {
            d.style = "GPT";
            if (!uuid.empty()) d.id = "gpt-" + uuid;
        } else if (table == "dos") {
            d.style = "MBR";
            if (!uuid.empty()) d.id = "mbr-" + uuid;
        } else {
            d.style = "RAW";
        }
        // The whole disk may itself hold a file system or be in use.
        if (auto use = use_of(sys, name, mounts, swaps)) d.volumes.push_back(*use);
        for (const std::string& part : entries(sys)) {
            const std::string psys = os::join(sys, part);
            if (!os::exists(os::join(psys, "partition"))) continue;
            ++d.partitions;
            DiskVolume v;
            v.name = "/dev/" + part;
            v.offset = std::strtoull(read_small(os::join(psys, "start")).c_str(), nullptr, 10) * 512;
            v.length = std::strtoull(read_small(os::join(psys, "size")).c_str(), nullptr, 10) * 512;
            v.label = udev_value(psys, "ID_FS_LABEL");
            v.filesystem = udev_value(psys, "ID_FS_TYPE");
            if (auto use = use_of(psys, part, mounts, swaps)) {
                v.mount = use->mount;
                if (use->filesystem == "swap" || use->filesystem == "holder" || v.filesystem.empty()) v.filesystem = use->filesystem;
            }
            d.volumes.push_back(v);
        }
        d.system = std::find(root.begin(), root.end(), d.number) != root.end();
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<int> protected_tool_disks() {
    std::vector<int> out;
    for (const char* p : {"/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home"})
        for (const int n : os::disk_numbers(p)) out.push_back(n);
    for (const int n : os::disk_numbers(os::executable_path())) out.push_back(n);
    return out;
}

std::unique_ptr<WipeDevice> open_wipe_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) throw Error("cannot open " + path + ": " + std::strerror(errno));
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        throw Error(path + " is not a file");
    }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    return std::make_unique<FdDevice>(fd, size, size % 512 == 0 ? 512 : 1, path);
}

std::unique_ptr<WipeDevice> open_wipe_disk(const DiskInfo& chosen, const DiskIdentity& expected,
                                           const std::vector<int>& protected_disks) {
    const std::vector<DiskInfo> disks = list_tool_disks();
    auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) { return d.number == chosen.number; });
    if (it == disks.end()) throw Error(chosen.path + " is not connected any more");
    const std::string why = tool_refusal(*it, {}, protected_disks);
    if (!why.empty()) throw Error(it->path + " cannot be erased: " + why);
    const std::string other = identity_mismatch(*it, expected);
    if (!other.empty()) throw Error(other + "; nothing was written");
    auto dev = open_block_device(it->path);
    if (dev->size() != it->size) throw Error(it->path + " changed; nothing was written");
    return dev;
}

VolumeTarget find_wipe_volume(const std::string& given) {
    const std::string name = given.starts_with("/dev/") ? given : "/dev/" + given;
    for (const DiskInfo& d : list_tool_disks()) {
        for (const DiskVolume& v : d.volumes) {
            if (v.name != name || v.offset == 0) continue;
            if (v.filesystem == "swap") throw Error(name + " is in use as swap");
            if (v.filesystem == "holder") throw Error(name + " is in use (LVM, RAID or encryption)");
            if (!v.mount.empty()) throw Error(name + " is mounted on " + v.mount + "; unmount it first");
            VolumeTarget t;
            t.name = name;
            t.device = name;
            t.size = v.length;
            t.filesystem = v.filesystem;
            t.label = v.label;
            t.disk = d.number;
            return t;
        }
    }
    throw Error(given + " is not a partition of a disk of this computer (see vaultik disk list)");
}

std::unique_ptr<WipeDevice> open_wipe_volume(const VolumeTarget& v) {
    return open_block_device(v.device);
}

void disk_erased(const DiskInfo& d) {
    // The kernel reads the (now empty) partition table again.
    const int fd = ::open(d.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ::ioctl(fd, BLKRRPART);
    ::close(fd);
}

std::unique_ptr<FillFile> create_fill_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw Error("cannot create " + path + ": " + std::strerror(errno));
    return std::make_unique<PosixFillFile>(fd);
}

NewDiskResult init_disk(const DiskInfo& chosen, const DiskIdentity& expected, const std::vector<int>& protected_disks,
                     const NewDiskOptions& o) {
    check_init_options(o);
    const std::string fs = o.filesystem.empty() ? std::string("ext4") : o.filesystem;
    const std::string sfdisk = need_tool("sfdisk", "util-linux");
    const std::string wipefs = need_tool("wipefs", "util-linux");
    const std::string mkfs = fs == "ext4"    ? need_tool("mkfs.ext4", "e2fsprogs")
                             : fs == "ntfs"  ? need_tool("mkfs.ntfs", "ntfs-3g")
                                             : need_tool("mkfs.exfat", "exfatprogs");
    const auto step = [&](const std::string& s) {
        if (o.on_step) o.on_step(s);
    };

    // Checked again and opened exclusively first (nothing may use it); closed again before sfdisk
    // takes over.
    const DiskInfo& d = chosen;
    std::uint64_t size = 0;
    std::uint32_t sector = 0;
    {
        auto dev = open_wipe_disk(chosen, expected, protected_disks);
        size = dev->size();
        sector = dev->block_size();
    }
    const std::vector<NewPartition> parts = plan_partitions(size, sector, o.style, o.sizes, false);

    step("Removing what was on " + d.path);
    run_tool({wipefs, "--all", "--force", d.path}, "wipefs");
    step("Writing a new " + std::string(o.style == TableStyle::Gpt ? "GPT" : "MBR") + " partition table");
    // The script reaches sfdisk on its standard input, as an argument of the shell (no file).
    const std::string script = sfdisk_script(o.style, parts, sector, fs);
    run_tool({"/bin/sh", "-c", "printf '%s' \"$3\" | exec \"$1\" --wipe always --wipe-partitions always --no-reread -- \"$2\"",
              "sh", sfdisk, d.path, script},
             "sfdisk");
    disk_erased(d);
    const std::string settle = find_tool("udevadm");
    if (!settle.empty()) {
        std::string ignored;
        os::run_command({settle, "settle", "--timeout=15"}, &ignored);
    }

    NewDiskResult r;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string part = partition_device(d.path, static_cast<int>(i) + 1);
        for (int tries = 0; tries < 50 && !os::exists(part); ++tries) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!os::exists(part)) throw Error(part + " did not appear after the partition table was written");
        step("Formatting " + part + " as " + fs);
        if (fs == "ext4") {
            std::vector<std::string> argv = {mkfs, "-F", "-q"};
            if (!o.label.empty()) argv.insert(argv.end(), {"-L", o.label});
            argv.push_back(part);
            run_tool(argv, "mkfs.ext4");
        } else if (fs == "ntfs") {
            std::vector<std::string> argv = {mkfs, "-Q", "-F"};
            if (!o.label.empty()) argv.insert(argv.end(), {"-L", o.label});
            argv.push_back(part);
            run_tool(argv, "mkfs.ntfs");
        } else {
            std::vector<std::string> argv = {mkfs};
            if (!o.label.empty()) argv.insert(argv.end(), {"-L", o.label});
            argv.push_back(part);
            run_tool(argv, "mkfs.exfat");
        }
        if (i == 0 && !o.mount.empty()) {
            step("Mounting " + part + " on " + o.mount);
            os::make_dirs(o.mount);
            run_tool({need_tool("mount", "util-linux"), part, o.mount}, "mount");
            r.volumes.push_back(o.mount);
        } else {
            r.volumes.push_back(part);
        }
    }
    return r;
}

}  // namespace vaultik::image
