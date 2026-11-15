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
    // Called just before the new partition table is written, with the id it gets: a write that
    // fails part way may leave the disk with that table (or a part of it).
    std::function<void(const std::string& table_id)> on_table_planned;
    // Called once the new partition table is on the disk, with its id (DiskInfo::id, such as
    // "gpt-<GUID>"): from then on the disk is not blank any more, also when a later step fails.
    std::function<void(const std::string& table_id)> on_table;
    bool force_dismount = false;  // take volumes in use away from the programs using them (see open_wipe_disk)
};

// ---- drive letters (Windows) -------------------------------------------------------------------

// The drive letters in use now, as GetLogicalDrives() tells them (bit 0 is A:). A letter counts
// as in use also when nothing can be read there: an empty card reader slot or optical drive, or
// a locked BitLocker drive. 0 on Linux.
std::uint32_t used_drive_letters();
// The letters of D to Z that `used` (a used_drive_letters() mask) leaves free, in order.
std::string free_letters(std::uint32_t used);
// Why letter `letter` cannot be given to a new volume when `used` letters are taken: not one of
// D to Z, or in use already. Empty when it can.
std::string letter_problem(char letter, std::uint32_t used);

// ---- what belongs to the disk being set up itself ------------------------------------------------
// Setting a disk up (also after erasing it) removes its own volumes, so a drive letter one of them
// has is not in the way of its new volume, and the volume Windows keeps of a removable-media disk
// without a partition table does not make that disk "not blank". Everything else (a letter another
// drive has, a volume with a file system) is kept exactly as strict.

// Whether `letter` (D to Z, any case) is the drive letter of one of disk `d`'s own volumes
// ("F:\" gives F).
bool own_letter(const DiskInfo& d, char letter);
// letter_problem for the first new volume of disk `d`: a letter of one of its own volumes
// (own_letter) counts as free, any other must not be in `used`; the range is always checked.
std::string letter_problem(const DiskInfo& d, char letter, std::uint32_t used);
// Whether all disk `d` shows besides "no partition table" is what Windows keeps of a
// removable-media device without one: a single volume of that disk itself, with no file system
// (RAW), from the disk's first byte and no longer than the disk. Windows makes it for such a
// device, gives it the drive letter, and never takes it away, so it says nothing about what is
// on the disk. False for anything else (a partition table, two volumes, a file system, a fixed disk).
bool only_whole_disk_raw_volume(const DiskInfo& d);

// How the new volume gets its drive letter (Windows; faked in tests).
struct LetterSteps {
    // Adds mount point `root` ("F:\") to the new volume; false with `why` set when it fails.
    std::function<bool(const std::string& root, std::string& why)> add;
    // Removes mount point `root` (the one Windows gave the volume by itself).
    std::function<void(const std::string& root)> remove;
    // A free letter (as free_letter picks it), 0 when none is.
    std::function<char()> free;
    // Whether letter `root` is given to another drive now (used_drive_letters).
    std::function<bool(const std::string& root)> in_use;
};
struct LetterResult {
    std::string root;     // the letter the volume has now ("F:\")
    std::string warning;  // the letter asked for could not be given: why, and which one it has instead
};
// Gives the new volume letter `wanted` (0: any), where Windows gave it `automatic` ("E:\", or
// empty when it gave none). The one asked for is added first and the automatic one removed only
// once it is set; where Windows allows a volume one letter only, they are swapped (only while
// the one asked for is not in use) and the automatic one is put back when that fails. So a letter that cannot be
// given leaves the volume with the automatic one (or a free one) and a warning, never without a
// letter. Throws Error only when it ends up without any letter.
LetterResult give_letter(char wanted, const std::string& automatic, const LetterSteps& steps);

// Checks the options before anything is written: the file system exists on this system, the
// label fits it (NTFS 32 characters, exFAT 11, ext4 16; no characters the file system refuses),
// the letter is a free one (letter_problem with used_drive_letters()). Throws Error.
void check_init_options(const NewDiskOptions& o);
// The same for setting up disk `target`: a letter one of its own volumes has counts as free
// (letter_problem(target, ...)), as setting it up removes that volume. disk init, the setup after
// an erase and init_disk all check this way, so a letter that went to the disk's own new volume
// (a setup that failed after giving it) does not stop setting it up again.
void check_init_options(const NewDiskOptions& o, const DiskInfo& target);

// The programs setting up a disk with file system `filesystem` runs, each with the package that
// has it: on Linux sfdisk, the mkfs tool of that file system, and mount when `mount` is set;
// none on Windows (format.com is part of it).
std::vector<std::pair<std::string, std::string>> setup_tools(const std::string& filesystem, bool mount);
// Linux: the full path of program `name` in the system's folders (/usr/sbin, ...), empty when it
// is not there. Empty on Windows.
std::string find_setup_tool(const std::string& name);

struct NewDiskResult {
    std::vector<std::string> volumes;  // "E:\" on Windows, "/dev/sdb1" (or its mount point) on Linux
    std::vector<std::string> warnings;  // done, but not quite as asked (a drive letter that could not be given)
};
// Sets up disk `d` (administrator rights): checks it may be erased and is still `expected`, then
// removes what is on it, writes a new partition table with the planned partitions, formats them
// and gives the first one its letter (Windows) or mounts it (Linux). Throws VolumesInUse (see
// NewDiskOptions::force_dismount) or Error.
NewDiskResult init_disk(const DiskInfo& d, const DiskIdentity& expected, const std::vector<int>& protected_disks,
                     const NewDiskOptions& options);

// ---- erasing a whole disk, then setting it up again ------------------------------------------

// Whether an MBR partition table can cover all of disk `d` (up to 2 TiB with 512-byte sectors);
// plan_partitions refuses MBR on a larger disk.
bool mbr_fits(const DiskInfo& d);

// Checks, before whole disk `d` is erased, that it can be set up with `o` afterwards
// (check_init_options, plan_partitions, and on Linux the programs of setup_tools, looked for
// with `find`, find_setup_tool when not given), so a setup that cannot work stops before
// anything is written. A drive letter one of the disk's own volumes has now counts as free: the
// erase frees it (check_init_options with the disk). Throws Error.
void check_setup_after_erase(const DiskInfo& d, const NewDiskOptions& o,
                             const std::function<std::string(const std::string&)>& find = {});

// What the disk set up after an erase must be: the identity the erase was pinned to (its serial
// number and size), with no partition table any more (the erase removed it, so its id is gone).
DiskIdentity erased_identity(const DiskIdentity& erased);

// Why `now` (the disk at the erased disk's number, listed after the erase) is not disk `erased`
// as it was erased, pinned to `pinned`: another number, serial number, size, sector size,
// model, bus or device, or it is not blank (a partition table, partitions, volumes, or on the
// whole disk a file system or encryption the system knows). On Windows the one volume it keeps
// of a removable-media device without a partition table (only_whole_disk_raw_volume) does not
// count; on Linux any volume does. With `table_id` (the id of the partition table a setup that
// failed later had written), it must have exactly that partition table instead of being blank.
// Empty when it is that disk, as it must be.
std::string erased_disk_mismatch(const DiskInfo& now, const DiskInfo& erased, const DiskIdentity& pinned,
                                 const std::string& table_id = {});

// What erase_and_set_up does to the system, faked in tests.
struct EraseSetupSteps {
    // Opens the disk for erasing, checked (open_wipe_disk).
    std::function<std::unique_ptr<WipeDevice>()> open;
    // The disks now (list_tool_disks).
    std::function<std::vector<DiskInfo>()> list;
    // Sets up the disk found again after the erase, pinned to the identity given (init_disk with
    // the protected disks, which checks everything again on its own).
    std::function<NewDiskResult(const DiskInfo&, const DiskIdentity&, const NewDiskOptions&)> init;
    // Whether a stop was asked for (checked once the erase ended, and again just before the
    // setup writes anything).
    std::function<bool()> stop_asked;
    // Waits, once the disk was found blank, until the system let go of what the erase removed
    // (Windows: the drive letter its old volume had). Nothing when not given.
    std::function<void(const DiskInfo&, const NewDiskOptions&)> settle;
    // How setup programs are found (see check_setup_after_erase); find_setup_tool when not given.
    std::function<std::string(const std::string&)> find_tool;
    // The drive letters given now (used_drive_letters when not given): a letter asked for that
    // another drive took while the disk was erased is not taken from it (see erase_and_set_up).
    std::function<std::uint32_t()> used_letters;
    // Called when the setup starts.
    std::function<void()> on_setup;
    // How often the disk is looked for again after the erase while the system has not caught up
    // with it yet (not listed, or still with its old partition table or volume), and how long
    // between.
    int tries = 20;
    std::chrono::milliseconds wait{500};
};

struct EraseSetupResult {
    WipeResult wipe;
    bool set_up = false;      // the setup ran and finished
    bool stopped = false;     // erased completely, but a stop was asked before the setup started
    NewDiskResult setup;      // its volumes, when set_up
    std::string setup_error;  // the disk was erased completely, but setting it up failed: why
    // The setup wrote a new partition table (also when a later step failed): the disk is not
    // blank any more, and setting it up again must be pinned to that table. table_id is its id;
    // it is set already when the setup was about to write it (table_written false then): a write
    // that failed part way may have left that table, so setting it up again accepts the disk
    // blank or with exactly that table (setup_again_pin).
    bool table_written = false;
    std::string table_id;
};

// Erases whole disk `d` (pinned to `confirmed`) with `wipe`, then removes what the system knew of
// its partition table, and with `setup` sets it up again on the same disk:
//   - `setup` is checked first (check_setup_after_erase); nothing is opened when it cannot work;
//   - the setup starts only after an erase that ended completely: every pass written in full,
//     checked when wipe.verify asked for it, and no stop asked meanwhile. A cancelled erase
//     throws Cancelled and a failed one Error, as wipe_device does (nothing is set up then); a
//     stop asked just after the erase ended sets `stopped` and nothing is set up;
//   - the disk set up is found again by its number and must be the disk erased
//     (erased_disk_mismatch), then init is pinned to erased_identity(confirmed);
//   - a drive letter asked for that another drive has once the system let go of the erased
//     volumes (steps.settle) is not taken from it: the new volume keeps the letter Windows gives
//     it, and a warning in setup.warnings says so;
//   - when the setup fails (also when the disk found is not the one erased), its reason is in
//     setup_error and nothing throws: the erase itself succeeded. table_written and table_id
//     tell whether it had written the new partition table by then.
EraseSetupResult erase_and_set_up(const DiskInfo& d, const DiskIdentity& confirmed, const WipeOptions& wipe,
                                  const std::optional<NewDiskOptions>& setup, const EraseSetupSteps& steps);

// ---- what the app asks the engine for (kept here, without Qt, so it can be tested) ------------

// The setup the app offers after erasing disk `d`: the name its volume had (the first label
// found on it), else "Backup"; the drive letter it had (Windows, D to Z), else 0 (any free one,
// as Set up a new disk does: picked when the disk is set up).
struct SetupDefaults {
    std::string label;
    char letter = 0;
};
SetupDefaults setup_defaults(const DiskInfo& d);

// The drive letters the app offers for the disk set up after erasing `d`: the ones it has now
// (the erase frees them), then `free_letters`, in order and each once.
std::string setup_letters(const DiskInfo& d, const std::string& free_letters);

// A whole disk erase as the app starts it.
struct EraseRequest {
    std::string disk;      // the engine's disk argument: its number (Windows) or device (Linux)
    DiskIdentity shown;    // what the user saw (--expect-*)
    std::string confirm;   // what the user typed (erase_text)
    std::string method;    // zeros, random or dod3
    bool verify = false;
    std::optional<NewDiskOptions> setup;  // set it up again afterwards (--then-init)
};
// "disk wipe <disk> --expect-size ... --method ... --confirm ... [--verify] [--then-init ...]".
std::vector<std::string> erase_disk_args(const EraseRequest& r);
// "disk init <disk> ...": setting up the disk erased by `r` when the setup after the erase
// failed, pinned to the erased disk: with no partition table (erased_identity, so a disk that
// got one meanwhile is refused), or with exactly `table_id` when the failed setup had written
// that table (--expect-id). The confirmation is the one typed for the erase.
std::vector<std::string> setup_again_args(const EraseRequest& r, const std::string& table_id = {});

// Whether `id` is a partition table id as the engine writes one: "gpt-" and a GUID
// (8-4-4-4-12 hex digits) or "mbr-" and 8 hex digits. Anything else (from the engine's done
// event) is never passed on as --expect-id.
bool valid_table_id(const std::string& id);

// What setting the drive up again after a failed setup is pinned to, for disk `now` as it is
// listed now (the disk at the number of `erased`, or none): the setup's done event said
// `table_written` and `table_id`. `ok` false with `why` when it must not be offered: a table id
// that is missing (written) or not valid (valid_table_id), or a disk that is not the erased one,
// blank (erased_disk_mismatch). Written: exactly that table. Only planned (table_id without
// table_written): blank, or exactly that table (a write that failed part way). Neither: blank.
struct SetupAgainPin {
    bool ok = false;
    std::string table_id;  // --expect-id for setup_again_args; empty: blank
    std::string why;
};
SetupAgainPin setup_again_pin(const DiskInfo* now, const DiskInfo& erased, const DiskIdentity& shown, bool table_written,
                              const std::string& table_id);

// The sfdisk script for a new partition table (Linux; exposed for tests).
// With `label_id` ("gpt-<UUID>" or "mbr-<8 hex digits>", as DiskInfo::id), the table gets
// that id; without, sfdisk makes one.
std::string sfdisk_script(TableStyle style, const std::vector<NewPartition>& parts, std::uint32_t sector_size,
                          const std::string& filesystem, const std::string& label_id = {});
// The device of partition `n` (1-based) of disk device `disk`: /dev/sdb -> /dev/sdb1,
// /dev/nvme0n1 -> /dev/nvme0n1p1.
std::string partition_device(const std::string& disk, int n);

}  // namespace vaultik::image
