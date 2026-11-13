#pragma once

// Disk tools: setting up a new (blank or unwanted) disk for use, and erasing a disk, a volume or
// a volume's free space so what was on it cannot be read back. Both write to whole disks, so
// they share the guards of a disk image restore (see target_refusal in disk.hpp) and add their
// own (see tool_refusal and find_wipe_volume). Never written to:
//   - the disk holding the Windows that is running, and the disk holding the partition the
//     firmware starts it from (the EFI system partition, or the active "System Reserved"
//     partition of a BIOS computer); when these cannot be found, no disk at all;
//   - the disk Vaultik runs from (Linux: also every disk holding a file system the system needs);
//   - a disk holding the page, swap or hibernation file, or (Linux) a mounted, swap, LVM, RAID or
//     encrypted volume;
//   - a dynamic disk, a Storage Spaces disk, and any disk holding a part of a volume that lies on
//     several disks;
//   - a disk Windows keeps offline or read-only (it is brought online in Disk Management first);
//   - a disk or volume that is not the one the user saw any more (its size, serial number and
//     partition table or volume id are checked again on the opened device just before writing);
// and only after the user typed the confirmation shown.
//
// A volume some program has open (Windows: it cannot be locked; Linux: it is mounted) stops the
// tool with VolumesInUse, unless the caller asked to force it closed (--force-dismount): then
// Windows dismounts it anyway, which takes it away from the programs using it (as format /X
// does), and Linux unmounts it (never lazily). That happens only after every check above passed
// and the user typed the confirmation.
//
// Windows: through the raw disk (\\.\PhysicalDriveN) with its volumes locked and dismounted
// first; the new partition table is written with IOCTL_DISK_CREATE_DISK and
// IOCTL_DISK_SET_DRIVE_LAYOUT_EX, the file system with format.com. Linux: through the block
// device (/dev/sdb), sfdisk for the partition table and mkfs for the file system. Both need
// administrator rights (root on Linux).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vaultik/image/disk.hpp"
#include "vaultik/util/bytes.hpp"

#ifndef _WIN32
#include "vaultik/platform/mounts_posix.hpp"
#endif

namespace vaultik::image {

// ---- the disks the tools may work on --------------------------------------------------------

// Volumes another program uses, so the disk or volume cannot be written to: Windows cannot lock
// them, or (Linux) they are mounted. `volumes` names each one ("F:", "/media/usb"). Nothing was
// written when this is thrown; the same job with force_dismount takes them away from those
// programs first.
class VolumesInUse : public Error {
public:
    explicit VolumesInUse(std::vector<std::string> names);
    std::vector<std::string> volumes;
};

// One volume of a disk or volume about to be written to, to lock (Windows: FSCTL_LOCK_VOLUME,
// which fails while any other program has a file or the volume open). Fakes in tests.
class VolumeLock {
public:
    virtual ~VolumeLock() = default;
    virtual std::string name() const = 0;  // as the user knows it ("F:")
    // Takes the lock; false while another program has the volume open. Throws Error when it
    // cannot be locked for another reason (forcing it closed would not help then).
    virtual bool lock() = 0;
    // Dismounts the volume although programs have it open (FSCTL_DISMOUNT_VOLUME without the
    // lock): their open files and handles stop working, so the lock can be taken next.
    virtual void force_dismount() = 0;
    // Why the last lock failed, for the error message (empty when not known).
    virtual std::string why() const { return {}; }
};
struct LockRetry {
    int tries = 10;  // lock attempts after a forced dismount
    std::chrono::milliseconds wait{300};  // between them
};
// Locks every volume. Every one is tried first; when some cannot be locked: without `force`,
// throws VolumesInUse naming each of them (and nothing is forced); with `force`, dismounts each
// of those by force and takes its lock again (retried `retry.tries` times), and throws Error
// when one still cannot be locked. An Error from a lock (not being in use) stops it as it is,
// before anything is forced. Locks already taken stay with the VolumeLock objects.
void lock_volumes(const std::vector<VolumeLock*>& volumes, bool force, const LockRetry& retry = {});

// Linux: the volumes of `d` that are mounted (not swap, LVM, RAID or encrypted ones, which
// stay refused), named by their mount point.
std::vector<std::string> mounted_volumes(const DiskInfo& d);
// Linux, with --force-dismount: unmounts whatever `mounted()` lists (mount points) through
// `unmount` (umount, never lazily; false with `why` set when it fails), until nothing is
// listed. Throws Error naming the mount point that stays in use; nothing was written then.
void unmount_volumes(const std::function<std::vector<std::string>()>& mounted,
                     const std::function<bool(const std::string& point, std::string& why)>& unmount);

#ifndef _WIN32
// Linux: what umount `point` would unmount instead of the drive being erased: the top-most mount
// at that folder (the last one /proc/self/mountinfo lists there) when its device is none of
// `devices` (major and minor numbers of the disk and its partitions, or of the one partition),
// as "/dev/sdc1 (8:33)"; another drive mounted over it. Empty when it is one of `devices`, or
// nothing is mounted there any more.
std::string other_mount_on(const std::vector<os::mounts::Mount>& mounts, const std::string& point,
                           const std::vector<std::pair<unsigned, unsigned>>& devices);
#endif

// Every disk of this computer, as list_disks() shows them on Windows. Linux: the block devices of
// /sys/block that are disks (also loop devices with a file attached), numbered by their device
// number, with DiskInfo::path set to their device ("/dev/sdb").
std::vector<DiskInfo> list_tool_disks();
// The disks the tools must never write to besides the one the system runs from: the disk holding
// Vaultik itself (and on Linux every disk holding a mounted file system the system needs).
std::vector<int> protected_tool_disks();

// Why the tools must not erase `d`: target_refusal (a disk Windows runs or starts from, a
// protected disk, no size), then a dynamic, Storage Spaces or offline disk, a part of a volume
// over several disks, then a volume on it the system uses (Windows: the running Windows' volume,
// the partition the firmware starts it from, one holding a page file, swap file or hibernation
// file; Linux: one mounted anywhere, used as swap or by LVM, RAID or encryption). Empty when it
// may be erased.
// With `mounted_ok` (Linux), a mounted volume is no reason: it is unmounted with
// --force-dismount (see mounted_volumes). A part of a file system over several devices (Linux:
// a btrfs file system over several drives, DiskVolume::spans) is refused also then.
std::string tool_refusal(const DiskInfo& d, const std::vector<int>& running_from, const std::vector<int>& protected_disks,
                         bool mounted_ok = false);

// Why a volume in partition `p` (of a GPT disk when `gpt`) must not be erased on its own: it is
// the EFI system partition, the Microsoft reserved partition, a recovery partition, one the
// platform needs, part of a dynamic disk or of a Storage Spaces pool, or (MBR) the active
// partition a BIOS computer starts from. Empty when it may.
std::string partition_refusal(const Partition& p, bool gpt);
// Whether partition `p` belongs to a dynamic disk (LDM) or a Storage Spaces pool.
bool pool_partition(const Partition& p, bool gpt);

#ifdef _WIN32
// Why the running Windows needs volume `v`: it holds the Windows folder, it is the partition the
// firmware starts Windows from, or it holds a page, swap or hibernation file in use. Empty when
// it does not. (A Windows folder of another installation, on a disk from another computer, is
// no reason.)
std::string windows_use_of(const DiskVolume& v);
#endif

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
    // Whether several writes (or reads) may run at once, each from a thread of its own, at
    // other offsets. Disks, volumes and files say yes; a device that does not gets one write
    // after another (still on a thread of its own, while the next one is made ready).
    virtual bool parallel_io() const { return false; }
    // Called from another thread when the erase stops (cancelled, or a write failed) while
    // writes or reads still run: makes them end as soon as it can (Windows: CancelIoEx), each
    // then failing or completing. Called again (every 50 ms) until nothing runs any more, so a
    // write that reached the device just after a call is cut short too. Nothing for devices
    // whose writes cannot be cut short.
    virtual void cancel() {}
    // A whole disk, after it was erased: tells the system it has no partitions any more
    // (Windows: it shows as not initialised; Linux: the kernel reads the empty table), through
    // the device opened and checked here, not by opening the disk again. Nothing for others.
    virtual void forget_partition_table() {}
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
// Writes are kWipeChunk bytes, up to kWipeInFlight of them at once (on a device with
// parallel_io), while the data of the next ones is made on the calling thread; the verification
// reads the same way. A pass ends with flush(), so it is done only once the drive has it. A
// failed write stops it: no later write is started, and it throws once the writes still running
// ended. Cancelled the same way, after cancel() cut them short. When it returns or throws, no
// write or read of it runs any more. Progress counts the bytes whose write completed.
constexpr std::size_t kWipeChunk = 8u << 20;
constexpr unsigned kWipeInFlight = 6;
WipeResult wipe_device(WipeDevice& device, const WipeOptions& options);

// Opens a regular file to overwrite in place, through the same writes as a disk (tests, and disk
// image files). Throws Error.
std::unique_ptr<WipeDevice> open_wipe_file(const std::string& path);

// Opens disk `d` for erasing (administrator rights): checks again that it may be erased and that
// it is still the disk the user chose (`expected`), then locks and dismounts its volumes (Linux:
// opens it exclusively). Throws VolumesInUse when a volume is in use, unless `force_dismount`
// (then it is dismounted or unmounted by force first), and Error for anything else.
std::unique_ptr<WipeDevice> open_wipe_disk(const DiskInfo& d, const DiskIdentity& expected,
                                           const std::vector<int>& protected_disks, bool force_dismount = false);

// A volume to erase: "E:" or "E:\" on Windows, a partition's device ("/dev/sdb1") on Linux.
struct VolumeTarget {
    std::string name;     // as given, cleaned up ("E:", "/dev/sdb1")
    std::string device;   // what is opened
    std::uint64_t size = 0;
    std::string filesystem;
    std::string label;
    int disk = -1;
    // What tells this volume from any other, checked again on the opened device before it is
    // written to. Windows: the volume's id ("\\?\Volume{GUID}\") and its file system's serial
    // number; Linux: the partition's id and where it starts ("partuuid=<uuid>,start=<bytes>")
    // and the disk's serial number. A letter or /dev/sdb1 can be another drive after a USB stick
    // was swapped.
    std::string id;
    std::string serial;
    std::string mount;  // Linux: where it is mounted now (only found with mounted_ok)
};
// The volume, or Error when it must not be erased: the one the system runs or starts from,
// Vaultik's, one with a page or swap file, an EFI system, reserved, recovery or active
// partition, one on a dynamic disk or over several disks, or (Linux) one that is mounted or in
// use. With `mounted_ok` (Linux), a mounted one is found (with VolumeTarget::mount set), to be
// unmounted with --force-dismount.
VolumeTarget find_wipe_volume(const std::string& name, bool mounted_ok = false);
// What the user saw of the volume, to pass along (--expect-size, --expect-serial, --expect-id).
DiskIdentity volume_identity(const VolumeTarget& v);
// Why `now` is not the volume `expected` describes; empty when it is.
std::string volume_mismatch(const VolumeTarget& now, const DiskIdentity& expected);
// Opens the volume for erasing (administrator rights): refuses when it is not `expected`, then
// locks it and checks again on the opened device that it is. Throws VolumesInUse when another
// program uses it, unless `force_dismount` (as for open_wipe_disk), and Error otherwise.
std::unique_ptr<WipeDevice> open_wipe_volume(const VolumeTarget& v, const DiskIdentity& expected,
                                             bool force_dismount = false);

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
// FAT32). Created new (it must not exist), not compressed where the file system allows that,
// and gone when it is closed, also when the program is killed (Windows: deleted on close;
// Linux: removed from its folder right away and written through the open file).
enum class FillResult { Written, Full, TooLarge };
class FillFile {
public:
    virtual ~FillFile() = default;
    virtual FillResult write_at(std::uint64_t offset, ByteSpan data) = 0;
    virtual void flush() = 0;
    // The bytes the file takes on the drive (less than written when the file system compresses
    // it); nothing when the system cannot tell.
    virtual std::optional<std::uint64_t> allocated() = 0;
};
std::unique_ptr<FillFile> create_fill_file(const std::string& path);
// Whether a fill file of `written` bytes taking `allocated` bytes on the drive is kept
// compressed (or deduplicated), so it would never fill the drive.
bool fill_compressed(std::uint64_t written, std::uint64_t allocated);
// The name of the fill files in `folder` start with this; files of an earlier run that was
// killed (or lost power) are removed when the next one starts.
constexpr const char* kFillFilePrefix = "vaultik-free-space-";

// Fills the free space of the volume holding `folder` with a temporary file of zeros (or random
// data), until the volume is full, then deletes it. Files already there are not touched. The file
// is removed also when this fails or is cancelled. A drive that compresses the file gets random
// data instead of zeros; one that keeps even that compressed stops with an Error. Returns the
// bytes written.
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
    bool force_dismount = false;  // take volumes in use away from the programs using them (see open_wipe_disk)
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
// and gives the first one its letter (Windows) or mounts it (Linux). Throws VolumesInUse (see
// NewDiskOptions::force_dismount) or Error.
NewDiskResult init_disk(const DiskInfo& d, const DiskIdentity& expected, const std::vector<int>& protected_disks,
                     const NewDiskOptions& options);

// The sfdisk script for a new partition table (Linux; exposed for tests).
std::string sfdisk_script(TableStyle style, const std::vector<NewPartition>& parts, std::uint32_t sector_size,
                          const std::string& filesystem);
// The device of partition `n` (1-based) of disk device `disk`: /dev/sdb -> /dev/sdb1,
// /dev/nvme0n1 -> /dev/nvme0n1p1.
std::string partition_device(const std::string& disk, int n);

}  // namespace vaultik::image
