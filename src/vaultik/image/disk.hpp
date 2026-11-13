#pragma once

// The physical disks of this computer (Windows): listing them, reading one for an image backup
// (from shadow copies of its NTFS volumes), writing an image back onto one, attaching a VHDX,
// and making a restored disk start. Everything that writes is guarded: see target_refusal.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vaultik/image/image.hpp"
#include "vaultik/image/vhdx.hpp"

namespace vaultik::image {

struct DiskVolume {
    std::string name;   // "\\?\Volume{GUID}\"
    std::string mount;  // "C:\" or empty
    std::string filesystem;
    std::string label;
    std::uint64_t offset = 0;  // where it starts on the disk
    std::uint64_t length = 0;
    // Only a part of a volume that lies on several disks (a spanned, striped or mirrored dynamic
    // volume, or a Storage Spaces volume): offset and length are this disk's part.
    bool spans = false;
};

struct DiskInfo {
    int number = -1;
    std::string model;   // vendor and product as the drive reports them
    std::string serial;
    std::string bus;     // "NVMe", "SATA", "USB", "Virtual", ...
    std::uint64_t size = 0;
    std::uint32_t sector_size = 512;
    std::uint32_t physical_sector_size = 512;
    std::string style;   // "GPT", "MBR" or "RAW" (no partition table)
    std::string id;      // "gpt-<GUID>", "mbr-<signature>" or empty
    bool removable = false;
    bool system = false;  // Windows runs from it
    bool dynamic = false;  // a dynamic disk, or a disk of a Storage Spaces pool (Windows)
    bool offline = false;  // Windows keeps it offline or read-only
    int partitions = 0;
    std::string path;    // the device: "\\.\PhysicalDrive2", "/dev/sdb"
    std::string media;   // "SSD", "HDD", or empty when the drive does not say
    std::vector<DiskVolume> volumes;
};

// Every physical disk, by number. Works without administrator rights.
std::vector<DiskInfo> list_disks();
// One line: "Disk 1: Samsung SSD 980 1TB, 1000 GB, NVMe, serial S64ANS0R123456".
std::string describe(const DiskInfo& d);

// The disks Windows runs from: the one holding the Windows folder and the one holding the
// partition the firmware starts it from (the EFI system partition, or the active "System
// Reserved" partition of a BIOS computer). Empty in a recovery environment, which runs from
// memory. When they cannot be found the list holds kUnknownSystemDisk, and every disk is refused.
constexpr int kUnknownSystemDisk = -2;
std::vector<int> system_disks();

// Why a restore must not write to `target`: it is a disk Windows runs from (or those disks are
// not known: kUnknownSystemDisk), or one in `protected_disks` (the disk holding the backup, the
// program, ...). Empty when it may.
std::string target_refusal(const DiskInfo& target, const std::vector<int>& running_from,
                           const std::vector<int>& protected_disks);
// The text the user must type to let a restore erase disk `number` ("ERASE DISK 2").
std::string confirmation_text(int number);
// Whether `typed` is that text (case and surrounding spaces do not matter).
bool confirmed(const std::string& typed, int number);

// The disk with the stable id (DiskInfo::id, or "system" for the disk Windows runs from); -1 when
// none is connected.
int find_disk(const std::vector<DiskInfo>& disks, const std::string& id);

// Sources for an image backup of these disks (administrator rights): their layouts are read and
// one shadow copy set is made of every NTFS volume on them (unless `shadow_copy` is false or it
// fails, then they are read live, with a warning). Throws Error.
std::vector<std::unique_ptr<DiskSource>> open_disk_sources(const std::vector<int>& disks, bool shadow_copy,
                                                           const std::function<void(const std::string&)>& warn);

// What the user saw of the disk they agreed to erase. Disk numbers pass to other disks when
// disks are unplugged or plugged in, so a restore checks again, when it opens the disk, that the
// disk at that number is still this one.
struct DiskIdentity {
    std::string serial;
    std::uint64_t size = 0;
    std::string id;  // DiskInfo::id
};
DiskIdentity identity_of(const DiskInfo& d);
// Why `now` is not the disk `expected` describes; empty when it is.
std::string identity_mismatch(const DiskInfo& now, const DiskIdentity& expected);

// Writing onto disk `number` (administrator rights). Refuses a disk Windows runs from, one in
// `protected_disks` (see target_refusal), and one that is not `expected` (checked on the opened
// disk itself). Takes every volume on it offline until the restore is done; then Windows reads
// the new partitions.
std::unique_ptr<DiskTarget> open_disk_target(int number, const DiskIdentity& expected,
                                             const std::vector<int>& protected_disks);

// A new dynamic VHDX file at `path` of `size` bytes (it must not exist yet).
class VhdxTarget : public DiskTarget {
public:
    VhdxTarget(std::unique_ptr<RandomFile> file, std::uint64_t size, std::uint32_t sector_size,
               std::uint32_t physical_sector_size);
    std::uint64_t size() const override { return writer_.virtual_size(); }
    std::uint32_t sector_size() const override { return sector_; }
    bool zeroed() const override { return true; }
    std::uint32_t alignment() const override { return 1; }
    void write(std::uint64_t offset, ByteSpan data) override { writer_.write(offset, data); }
    void finish() override { writer_.finish(); }

private:
    std::unique_ptr<RandomFile> file_;
    std::uint32_t sector_;
    VhdxWriter writer_;
};

// A VHDX attached as a disk (Windows, administrator rights); detached when destroyed.
class AttachedDisk {
public:
    virtual ~AttachedDisk() = default;
    virtual int number() const = 0;
    // Drive letters of its volumes ("E:\"), after Windows mounted them.
    virtual std::vector<std::string> mounts() const = 0;
};
std::unique_ptr<AttachedDisk> attach_vhdx(const std::string& path, bool read_only);

// After a restore onto disk `number`: makes it start (bcdboot from its Windows folder onto its
// EFI system partition, or its active partition on an MBR disk). Returns what was done; throws
// Error when it could not.
std::string repair_boot(int number);

}  // namespace vaultik::image
