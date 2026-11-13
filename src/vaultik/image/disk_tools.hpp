#pragma once

// Disk tools: setting up a new (blank or unwanted) disk for use, and erasing a disk, a volume or
// a volume's free space so what was on it cannot be read back. Both write to whole disks, so
// they share the guards of a disk image restore (see target_refusal in disk.hpp): never the disk
// the system runs from or the one holding Vaultik, never a disk with a mounted system, boot or
// page file volume, and only after the user typed the confirmation shown.
//
// Windows: through the raw disk (\\.\PhysicalDriveN) with its volumes locked and dismounted
// first; the new partition table is written with IOCTL_DISK_CREATE_DISK and
// IOCTL_DISK_SET_DRIVE_LAYOUT_EX, the file system with format.com. Linux: through the block
// device (/dev/sdb), sfdisk for the partition table and mkfs for the file system. Both need
// administrator rights (root on Linux).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/image/disk.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik::image {

// ---- the disks the tools may work on --------------------------------------------------------

// Every disk of this computer, as list_disks() shows them on Windows. Linux: the block devices of
// /sys/block that are disks (also loop devices with a file attached), numbered by their device
// number, with DiskInfo::path set to their device ("/dev/sdb").
std::vector<DiskInfo> list_tool_disks();
// The disks the tools must never write to besides the one the system runs from: the disk holding
// Vaultik itself (and on Linux every disk holding a mounted file system the system needs).
std::vector<int> protected_tool_disks();

// Why the tools must not erase `d`: target_refusal (the system disk, a protected disk, no size),
// then a volume on it the system uses (Windows: one holding Windows, a page file, swap file or
// hibernation file; Linux: one mounted anywhere, used as swap or by LVM, RAID or encryption).
// Empty when it may be erased.
std::string tool_refusal(const DiskInfo& d, const std::vector<int>& running_from, const std::vector<int>& protected_disks);

// A disk given on the command line: its number, "/dev/sdb" (Linux) or a stable id. Throws Error.
const DiskInfo& find_tool_disk(const std::vector<DiskInfo>& disks, const std::string& arg);

// What the user types to let a tool erase `d`: "ERASE DISK 2" (Linux: "ERASE DISK sdb").
std::string erase_text(const DiskInfo& d);
// What the user types to let a tool erase a volume: "ERASE E:" (Linux: "ERASE sdb1").
std::string erase_volume_text(const std::string& volume);
// Whether `typed` is `want` (case and spaces do not matter).
bool typed_matches(const std::string& typed, const std::string& want);

// Whether the disk is a solid-state drive (an SSD, NVMe drive or memory card): overwriting it
// does not reach the spare blocks the drive keeps for itself (see README, "Drive cleanser").
bool solid_state(const DiskInfo& d);

// ---- erasing ----------------------------------------------------------------------------------

enum class WipeMethod {
    Zeros,   // one pass of zeros (the default)
    Random,  // one pass of random data
    Dod3,    // three passes: random, random, zeros (as DoD 5220.22-M)
};
std::optional<WipeMethod> parse_wipe_method(const std::string& text);  // "zeros", "random", "dod3"
const char* to_string(WipeMethod m);
// The passes of a method, in order: true for random data, false for zeros.
std::vector<bool> wipe_passes(WipeMethod m);

struct WipeProgress {
    int pass = 1;  // 1-based
    int passes = 1;
    bool verifying = false;  // reading the last pass back
    std::uint64_t done = 0;   // bytes of this pass (or of the verification)
    std::uint64_t total = 0;  // bytes per pass
    double bytes_per_second = 0;
    double seconds_left = -1;  // the whole job; negative while not known yet
};

// Something to overwrite from start to end: a disk, a volume, or a plain file (tests).
class WipeDevice {
public:
    virtual ~WipeDevice() = default;
    virtual std::uint64_t size() const = 0;
    // Writes and reads are made in multiples of this (and at offsets that are).
    virtual std::uint32_t block_size() const = 0;
    virtual void write_at(std::uint64_t offset, ByteSpan data) = 0;
    virtual void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) = 0;
    // Everything written is on the device.
    virtual void flush() = 0;
};

struct WipeOptions {
    WipeMethod method = WipeMethod::Zeros;
    bool verify = false;  // read the last pass back and compare it
    // A whole disk: also zero its first and last MiB when the last pass was random, so no
    // partition table is left and the system shows the disk as not initialised.
    bool clear_partition_table = false;
    std::function<void(const WipeProgress&)> on_progress;  // about twice a second
    std::function<bool()> should_cancel;                    // polled between blocks; true throws Cancelled
};

struct WipeResult {
    std::uint64_t bytes = 0;  // written in all
    double seconds = 0;
    bool verified = false;
};

// Overwrites the whole device with the method's passes. A random pass is a keyed stream (a fresh
// key each pass), so its verification regenerates it instead of keeping it. Throws Error when a
// write fails or the verification finds a difference, Cancelled when stopped (the device is then
// partly overwritten).
WipeResult wipe_device(WipeDevice& device, const WipeOptions& options);

// Opens a regular file to overwrite in place, through the same writes as a disk (tests, and disk
// image files). Throws Error.
std::unique_ptr<WipeDevice> open_wipe_file(const std::string& path);

// Opens disk `d` for erasing (administrator rights): checks again that it may be erased and that
// it is still the disk the user chose (`expected`), then locks and dismounts its volumes (Linux:
// opens it exclusively). Throws Error.
std::unique_ptr<WipeDevice> open_wipe_disk(const DiskInfo& d, const DiskIdentity& expected,
                                           const std::vector<int>& protected_disks);

// A volume to erase: "E:" or "E:\" on Windows, a partition's device ("/dev/sdb1") on Linux.
struct VolumeTarget {
    std::string name;     // as given, cleaned up ("E:", "/dev/sdb1")
    std::string device;   // what is opened
    std::uint64_t size = 0;
    std::string filesystem;
    std::string label;
    int disk = -1;
};
// The volume, or Error when it must not be erased: the one the system runs from, Vaultik's, one
// with a page or swap file, or (Linux) one that is mounted or in use.
VolumeTarget find_wipe_volume(const std::string& name);
std::unique_ptr<WipeDevice> open_wipe_volume(const VolumeTarget& v);

// After a whole-disk wipe: tells the system the disk has no partitions any more (Windows: it
// shows as not initialised; Linux: the kernel reads the empty table).
void disk_erased(const DiskInfo& d);

// ---- wiping free space ------------------------------------------------------------------------

struct FreeSpaceProgress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;  // the free space when it started
    double bytes_per_second = 0;
    double seconds_left = -1;
};
struct FreeSpaceOptions {
    WipeMethod method = WipeMethod::Zeros;  // Dod3 writes the file three times
    std::function<void(const FreeSpaceProgress&)> on_progress;
    std::function<bool()> should_cancel;
    std::uint64_t limit = 0;  // tests: stop as if the drive were full after this many bytes (0: none)
};
// A file that fills a volume: a write that does not fit any more says so instead of throwing
// (the drive is full, or the file reached the largest size the file system allows, 4 GiB on
// FAT32). Created new (it must not exist); closing it does not delete it.
enum class FillResult { Written, Full, TooLarge };
class FillFile {
public:
    virtual ~FillFile() = default;
    virtual FillResult write_at(std::uint64_t offset, ByteSpan data) = 0;
    virtual void flush() = 0;
};
std::unique_ptr<FillFile> create_fill_file(const std::string& path);

// Fills the free space of the volume holding `folder` with a temporary file of zeros (or random
// data), until the volume is full, then deletes it. Files already there are not touched. The file
// is removed also when this fails or is cancelled. Returns the bytes written.
std::uint64_t wipe_free_space(const std::string& folder, const FreeSpaceOptions& options);

// ---- setting up a new disk ---------------------------------------------------------------------

enum class TableStyle { Gpt, Mbr };

struct NewPartition {
    std::uint64_t offset = 0;  // bytes from the start of the disk
    std::uint64_t length = 0;
    bool reserved = false;     // the Microsoft reserved partition Windows puts first on a GPT disk
};

// Where the partitions go on a disk of `disk_size` bytes: aligned to 1 MiB, the first at 1 MiB,
// each of the `sizes` (0 for "the rest of the disk", only as the last one), and on a Windows GPT
// disk a 16 MiB reserved partition first (`with_msr`). GPT leaves the last MiB for its backup
// table. Throws Error when they do not fit, an MBR disk is over 2 TiB, or there are more than 4
// (MBR) or 128 (GPT) partitions.
std::vector<NewPartition> plan_partitions(std::uint64_t disk_size, std::uint32_t sector_size, TableStyle style,
                                          const std::vector<std::uint64_t>& sizes, bool with_msr);

struct NewDiskOptions {
    TableStyle style = TableStyle::Gpt;
    std::string filesystem;  // "ntfs", "exfat" or "ext4"; empty: NTFS on Windows, ext4 on Linux
    std::string label;
    char letter = 0;          // Windows: the first partition's drive letter (0: a free one)
    std::string mount;        // Linux: mount the first partition here (empty: not mounted)
    std::vector<std::uint64_t> sizes;  // empty: one partition over the whole disk
    std::function<void(const std::string&)> on_step;  // what it does now, for the progress
};

// Checks the options before anything is written: the file system exists on this system, the
// label fits it (NTFS 32 characters, exFAT 11, ext4 16; no characters the file system refuses),
// the letter is a free one. Throws Error.
void check_init_options(const NewDiskOptions& o);

struct NewDiskResult {
    std::vector<std::string> volumes;  // "E:\" on Windows, "/dev/sdb1" (or its mount point) on Linux
};
// Sets up disk `d` (administrator rights): checks it may be erased and is still `expected`, then
// removes what is on it, writes a new partition table with the planned partitions, formats them
// and gives the first one its letter (Windows) or mounts it (Linux). Throws Error.
NewDiskResult init_disk(const DiskInfo& d, const DiskIdentity& expected, const std::vector<int>& protected_disks,
                     const NewDiskOptions& options);

// The sfdisk script for a new partition table (Linux; exposed for tests).
std::string sfdisk_script(TableStyle style, const std::vector<NewPartition>& parts, std::uint32_t sector_size,
                          const std::string& filesystem);
// The device of partition `n` (1-based) of disk device `disk`: /dev/sdb -> /dev/sdb1,
// /dev/nvme0n1 -> /dev/nvme0n1p1.
std::string partition_device(const std::string& disk, int n);

}  // namespace vaultik::image
